
// A regular linear probing HT, but fixes for bucket alignment to maximize cache usage

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>              /* CPU_ZERO, CPU_SET */
#include <pthread.h>            /* pthread_* */
#include <string.h>             /* memset */
#include <stdio.h>              /* printf */
#include <stdlib.h>             /* memalign */
#include <sys/time.h>           /* gettimeofday */

#include "pmoney_join_8.h"
#include "pmj_params.h"         /* constant parameters */
#include "rdtsc.h"              /* startTimer, stopTimer */
#include "lock.h"               /* lock, unlock */
#include "cpu_mapping.h"        /* get_cpu_id */
#ifdef PERF_COUNTERS
#include "perf_counters.h"      /* PCM_x */
#endif

#include "hash.h"
#include "generator.h"          /* numa_localize() */

#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)

#ifndef NEXT_POW_2
/** 
 *  compute the next number, greater than or equal to 32-bit unsigned v.
 *  taken from "bit twiddling hacks":
 *  http://graphics.stanford.edu/~seander/bithacks.html
 */
#define NEXT_POW_2(V)                           \
    do {                                        \
        V--;                                    \
        V |= V >> 1;                            \
        V |= V >> 2;                            \
        V |= V >> 4;                            \
        V |= V >> 8;                            \
        V |= V >> 16;                           \
        V++;                                    \
    } while(0)
#endif

#ifndef HASH
#define HASH(X, MASK, SKIP) (((Hash(X)) & MASK) >> SKIP)
#endif

// Debug msg logging method
#ifdef DEBUG
#define DEBUGMSG(COND, MSG, ...)                                    \
    if(COND) { fprintf(stderr, "[DEBUG] "MSG, ## __VA_ARGS__); }
#else
#define DEBUGMSG(COND, MSG, ...) 
#endif

// An experimental feature to allocate input relations numa-local
extern int numalocalize;  /* defined in generator.c */
extern int nthreads;      /* defined in generator.c */

typedef struct bucket {
  tuple_t  tuple[3];
  uint8_t  flag[3];
  char     pad[13];
} bucket_t;

typedef struct hashtable {
  bucket_t  *buckets;
  uint32_t  num_buckets;
  uint32_t  hash_mask;
  uint32_t  skip_bits;
} hashtable_t;

//===----------------------------------------------------------------------===//
// Allocate a hash table with the given number of buckets
//===----------------------------------------------------------------------===//
static inline void allocate_hashtable(hashtable_t ** ppht, uint32_t nbuckets) {
  hashtable_t * ht = (hashtable_t*) malloc(sizeof(hashtable_t));
  ht->num_buckets = nbuckets;
  NEXT_POW_2((ht->num_buckets));

  /* allocate hashtable buckets cache line aligned */
  if (posix_memalign((void**)&ht->buckets, CACHE_LINE_SIZE,
                     ht->num_buckets * sizeof(bucket_t))){
    perror("Aligned allocation failed!\n");
    exit(EXIT_FAILURE);
  }

  // Not an elegant way of passing whether we will numa-localize, but this
  // feature is experimental anyways
  /*
  if(numalocalize) {
    tuple_t * mem = (tuple_t *) ht->buckets;
    uint32_t ntuples = (ht->num_buckets*sizeof(vs_t))/sizeof(tuple_t);
    numa_localize(mem, ntuples, nthreads);
  }
  */

  memset(ht->buckets, 0, ht->num_buckets * sizeof(bucket_t));
  ht->skip_bits = 0;
  ht->hash_mask = (ht->num_buckets - 1) << ht->skip_bits;
  *ppht = ht;
}

//===----------------------------------------------------------------------===//
// Destroy the given hash table
//===----------------------------------------------------------------------===//
static inline void destroy_hashtable(hashtable_t * ht) {
  free(ht->buckets);
  free(ht);
}

//===----------------------------------------------------------------------===//
// Build a hash table using a single thread
//===----------------------------------------------------------------------===//

static inline void build_hashtable_st(hashtable_t *ht, relation_t *rel) {
  const uint32_t hashmask = ht->hash_mask;
  const uint32_t skipbits = ht->skip_bits;

  for(uint32_t i = 0; i < rel->num_tuples; i++) {
    uint32_t idx = HASH(rel->tuples[i].key, hashmask, skipbits);
    uint32_t step = 0;
    do {
      bucket_t *curr = &ht->buckets[idx/3];
      if (curr->flag[idx%3] == 0) {
        curr->tuple[idx%3] = rel->tuples[i];
        curr->flag[idx%3] = 1;
        break;
      }
      idx = (idx + 1) & hashmask;
    } while (step++ < ht->num_buckets);
  }
}

//===----------------------------------------------------------------------===//
// Probe the hash table using a single thread
//===----------------------------------------------------------------------===//
static inline int64_t probe_hashtable_st(hashtable_t *ht, relation_t *rel) {
  const uint32_t hashmask = ht->hash_mask;
  const uint32_t skipbits = ht->skip_bits;

  uint64_t matches = 0;

  for (uint32_t i = 0; i < rel->num_tuples; i++) {
    uint32_t idx = HASH(rel->tuples[i].key, hashmask, skipbits);
    bucket_t *b = ht->buckets + (idx/3);
    if (b->flag[idx%3]) {
      while (b->flag[idx%3] == 1) {
        if (b->tuple[idx%3].key == rel->tuples[i].key) {
          matches++;
          break;
        }
        idx = (idx + 1) & (ht->num_buckets-1);
        b = ht->buckets + idx;
      }
    }
  }
  return matches;
}

//===----------------------------------------------------------------------===//
// Run the algorithm
//===----------------------------------------------------------------------===//
int64_t PMJ_8(relation_t *relR, relation_t *relS, int nthreads) {
#ifndef NO_TIMING
  struct timeval start, end;
  uint64_t partition_time, build_time, probe_time;
#endif

  hashtable_t *ht;
  //uint32_t nbuckets = (2.0/3.0)*relR->num_tuples;
  uint32_t nbuckets = (4.0/9.0)*relR->num_tuples;
  size_t mem_before = get_memory_usage_bytes();
  allocate_hashtable(&ht, nbuckets);
  size_t mem_after = get_memory_usage_bytes();
  fprintf(stderr, "HT: %.2lf MB (%lu byte bucket, %lu buckets)\n",
          ((double)mem_after-mem_before)/1024.0/1024.0, 
          sizeof(bucket_t), nbuckets);

#ifdef PERF_COUNTERS
  PCM_initPerformanceMonitor(NULL, NULL);
  PCM_start();
#endif

#ifndef NO_TIMING
  gettimeofday(&start, NULL);
  startTimer(&build_time);
  startTimer(&probe_time); 
  partition_time = 0; // no partitioning
#endif

  //////////////////////////////////////
  // BUILD
  //////////////////////////////////////
  build_hashtable_st(ht, relR);

#ifdef DEBUG
  uint32_t occ = 0;
  for (uint32_t j = 0; j < ht->num_buckets; j++) {
    occ += (ht->buckets[j] != 0);
  }
  fprintf(stderr, "HT load-factor: %.2lf \%\n", (double)occ/(double)ht->num_buckets);
  fprintf(stderr, "Num allocs: %llu\n", num_allocs);
#endif

#ifndef NO_TIMING
  stopTimer(&build_time);
#endif

#ifdef PERF_COUNTERS
  PCM_stop();
  PCM_log("========== Build phase profiling results ==========\n");
  PCM_printResults();
  PCM_start();
#endif

  //////////////////////////////////////
  // PROBE
  //////////////////////////////////////
  int64_t result = probe_hashtable_st(ht, relS);

#ifdef PERF_COUNTERS
    PCM_stop();
    PCM_log("========== Probe phase profiling results ==========\n");
    PCM_printResults();
    PCM_log("===================================================\n");
    PCM_cleanup();
#endif

#ifndef NO_TIMING
  stopTimer(&probe_time);
  gettimeofday(&end, NULL);
  // Now print the timing results
  print_timing(probe_time, build_time, partition_time,
               relR->num_tuples, relS->num_tuples, result, &start, &end);
#endif


  // Cleanup
  destroy_hashtable(ht);

  return result;
}
