//===----------------------------------------------------------------------===//
//
// The same as PMJ5, but with naive partitioning
//
//===----------------------------------------------------------------------===//

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <math.h>
#include <sched.h>              /* CPU_ZERO, CPU_SET */
#include <pthread.h>            /* pthread_* */
#include <string.h>             /* memset */
#include <stdio.h>              /* printf */
#include <stdlib.h>             /* memalign */
#include <sys/time.h>           /* gettimeofday */

#include "pmoney_join_6.h"
#include "pmj_params.h"         /* constant parameters */
#include "rdtsc.h"              /* startTimer, stopTimer */
#include "lock.h"               /* lock, unlock */
#include "cpu_mapping.h"        /* get_cpu_id */
#ifdef PERF_COUNTERS
#include "perf_counters.h"      /* PCM_x */
#endif

#include "hash.h"
#include "barrier.h"            /* pthread_barrier_* */
#include "affinity.h"           /* pthread_attr_setaffinity_np */
#include "generator.h"          /* numa_localize() */

#define likely(x)       __builtin_expect((x),1)
//#define unlikely(x)     __builtin_expect((x),0)

#ifndef BARRIER_ARRIVE
/** barrier wait macro */
#define BARRIER_ARRIVE(B,RV)                            \
    RV = pthread_barrier_wait(B);                       \
    if(RV !=0 && RV != PTHREAD_BARRIER_SERIAL_THREAD){  \
        printf("Couldn't wait on barrier\n");           \
        exit(EXIT_FAILURE);                             \
    }
#endif

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

#define HASH2IDX(H, MASK, SKIP) (((H) & MASK) >> SKIP)
#define HASH2PART(H, MASK) ((H) & MASK)

// Debug msg logging method
#ifdef DEBUG
#define DEBUGMSG(COND, MSG, ...)                                    \
    if(COND) { fprintf(stderr, "[DEBUG] "MSG, ## __VA_ARGS__); }
#else
#define DEBUGMSG(COND, MSG, ...) 
#endif

// 256K L2
#define L2_CACHE_SIZE (1 << 17)

#define REUSE 3
//#define REUSE 20

#define ROUND_UP(V,D) (((V) + (D) - 1) / (D))

// An experimental feature to allocate input relations numa-local
extern int numalocalize;  /* defined in generator.c */
extern int nthreads;      /* defined in generator.c */

typedef struct vs {
#ifndef SEPARATE_CHAIN
  uint32_t next;
#endif
  tuple_t  tuple; 
} vs_t;

typedef struct hashtable {
  uint32_t  *buckets;
#ifdef SEPARATE_CHAIN
  uint32_t  *next;
#endif
  vs_t      *values;
  uint32_t  first_empty;
  uint32_t  num_buckets;
  uint32_t  hash_mask;
  uint32_t  skip_bits;
  uint32_t  skip_mask;
} hashtable_t;

typedef struct buffer {
  tuple_t  *buf;
  uint32_t sz;
} buffer_t;

typedef struct arg {
    int32_t            tid;
    hashtable_t        **hts;
    buffer_t           *bufs;
    uint32_t           num_parts;
    uint32_t           part_bits;
    uint32_t           part_start, part_end;
    relation_t         relR;
    relation_t         relS;
    pthread_barrier_t  *barrier;
    uint64_t           num_results;
#ifndef NO_TIMING
    uint64_t           probe_time, build_time, part_time;
    struct timeval     start, end;
#endif
} arg_t;

//===----------------------------------------------------------------------===//
// Allocate a hash table with the given number of buckets
//===----------------------------------------------------------------------===//
static inline void allocate_hashtable(hashtable_t ** ppht, uint32_t nbuckets) {
  hashtable_t * ht = (hashtable_t*) malloc(sizeof(hashtable_t));
  ht->num_buckets = nbuckets;
  NEXT_POW_2((ht->num_buckets));

  /* allocate hashtable buckets cache line aligned */
  if (posix_memalign((void**)&ht->buckets, CACHE_LINE_SIZE,
                     ht->num_buckets * sizeof(uint32_t))){
    perror("Aligned allocation failed!\n");
    exit(EXIT_FAILURE);
  }

#ifdef SEPARATE_CHAIN
  if (posix_memalign((void**)&ht->next, CACHE_LINE_SIZE,
                     ht->num_buckets * sizeof(uint32_t))){
    perror("Aligned allocation failed!\n");
    exit(EXIT_FAILURE);
  }
#endif

  /* allocate hashtable valuespace cache line aligned */
  if (posix_memalign((void**)&ht->values, CACHE_LINE_SIZE,
                     ht->num_buckets * sizeof(vs_t))){
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

  memset(ht->buckets, 0, ht->num_buckets * sizeof(uint32_t));
#ifdef SEPARATE_CHAIN
  memset(ht->next, 0, ht->num_buckets * sizeof(uint32_t));
#endif
  memset(ht->values, 0, ht->num_buckets * sizeof(vs_t));
  ht->skip_bits = 0;
  ht->hash_mask = (ht->num_buckets - 1) << ht->skip_bits;
  ht->first_empty = 1;
  *ppht = ht;
}

//===----------------------------------------------------------------------===//
// Destroy the given hash table
//===----------------------------------------------------------------------===//
static inline void destroy_hashtable(hashtable_t * ht) {
  free(ht->buckets);
#ifdef SEPARATE_CHAIN
  free(ht->next);
#endif
  free(ht->values);
  free(ht);
}

//===----------------------------------------------------------------------===//
// Insert the provided tuples into the appropriate hash table
//===----------------------------------------------------------------------===//
static inline void build_hashtable_part(hashtable_t **hts, tuple_t *tuples, 
                                        uint32_t n, 
                                        uint32_t part_start, uint32_t part_end) {
  const uint32_t skipbits = hts[0]->skip_bits; 
  const uint32_t hashmask = hts[0]->hash_mask; 
  const uint32_t part_mask = hts[0]->skip_mask;

  uint32_t sel[VECTOR_SIZE];
  uint32_t p = 0;

  for (uint32_t i = 0; i < n; i++) {
    uint32_t hash = Hash(tuples[i].key);
    sel[p] = i;
    uint32_t part = HASH2PART(hash, part_mask);
    p += (part >= part_start && part < part_end); 
  }
  
  for (uint32_t i = 0; i < p; i++) {
    uint32_t hash = Hash(tuples[sel[i]].key);
    uint32_t part = HASH2PART(hash, part_mask);
    uint32_t idx = HASH2IDX(hash, hashmask, skipbits);

    hashtable_t *ht = hts[part];
    uint32_t pos = ht->first_empty;

#ifdef SEPARATE_CHAIN
    ht->next[pos] = ht->buckets[idx];
#else
    ht->values[pos].next = ht->buckets[idx];
#endif    
    
    ht->buckets[idx] = pos;
    ht->values[pos].tuple = tuples[sel[i]];
    ht->first_empty++;
  }
}

//===----------------------------------------------------------------------===//
// Probe the hash table using a single thread
//===----------------------------------------------------------------------===//
static inline int64_t drain_buffer(hashtable_t *ht, buffer_t *buf, relation_t *rel, uint64_t *ret_total) {
  const uint32_t skipbits = ht->skip_bits; 
  const uint32_t hashmask = ht->hash_mask; 

  uint64_t results = 0;
  uint32_t pivot[VECTOR_SIZE], matches[VECTOR_SIZE];
  uint32_t p = 0;

  startTimer(ret_total);

//#if 0 
  for (uint32_t i = 0; i < buf->sz; i++) {
    uint32_t hash = Hash(buf->buf[i].key);
    uint32_t idx = HASH2IDX(hash, hashmask, skipbits);   
#ifdef SEPARATE_CHAIN
    for (int hit = ht->buckets[idx]; hit > 0; hit = ht->next[hit]) {
#else
    for (int hit = ht->buckets[idx]; hit > 0; hit = ht->values[hit].next) {
#endif
      results += (buf->buf[i].key == ht->values[hit].tuple.key);
    }
  }
//#endif

#if 0 
  for (uint32_t v = 0; v < buf->sz; v += VECTOR_SIZE) { 
    uint32_t start = v;
    uint32_t sz = start + VECTOR_SIZE < buf->sz ? VECTOR_SIZE : buf->sz - start;    
    
    for (uint32_t i = 0; i < sz; i++) {
      uint32_t hash = Hash(buf->buf[start+i].key);
      uint32_t idx = HASH2IDX(hash, hashmask, skipbits);   
    
      //pivot[p] = ht->buckets[idx];
      //matches[p] = start+i;
      //p += (pivot[p] != 0); 

      pivot[i] = ht->buckets[idx];
    }
    for (uint32_t i = 0; i < sz; i++) {    
      pivot[p] = pivot[i];
      matches[p] = start+i;
      p += (pivot[p] != 0); 
    }

    while (p > 0) {
      uint32_t pp = 0;
      for (uint32_t i = 0; i < p; i++) {

        uint32_t pos = pivot[i];
        tuple_t *probe = &buf->buf[matches[i]];
        tuple_t *build = &ht->values[pos].tuple;
        results += (probe->key == build->key); 
   
  #ifdef SEPARATE_CHAIN
        pivot[pp] = ht->next[pos];
  #else
        pivot[pp] = ht->values[pos].next;
  #endif
        matches[pp] = matches[i];
        pp += (pivot[pp] != 0);
      }
      p = pp;
    }
  }
#endif
  stopTimer(ret_total);

  return results;
}

static uint32_t buf_thres;

static inline int64_t probe_hashtable(hashtable_t **hts, buffer_t *bufs,
                                      relation_t *rel, uint32_t num_parts) {
  const uint32_t part_mask = hts[0]->skip_mask;
  
  uint64_t probe_time = 0, total = 0;
  uint64_t result = 0;

  startTimer(&total);
  for (uint32_t i = 0; i < rel->num_tuples; i++) {
    uint32_t hash = Hash(rel->tuples[i].key);
    uint32_t part = HASH2PART(hash, part_mask);
    buffer_t *b = &bufs[part];
    b->buf[b->sz++] = rel->tuples[i];
//#if 0 
    if (b->sz >= buf_thres) {
      uint64_t time = 0;
      result += drain_buffer(hts[part], b, rel, &time);
      probe_time += time;
      b->sz = 0;
    } 
//#endif
  }
  
  for (uint32_t i = 0; i < num_parts; i++) {
    uint64_t time = 0;
    result += drain_buffer(hts[i], &bufs[i], rel, &time);
    probe_time += time;
  }
  stopTimer(&total);

  uint64_t part_time = total - probe_time;
  fprintf(stderr, "# Probes: %u, Part: %lu (%.2lf), Probe: %lu (%.2lf CPT)\n",
          rel->num_tuples, part_time, ((double)part_time/(double)rel->num_tuples), 
          probe_time, ((double)probe_time/(double)rel->num_tuples));
  return result;
}

//===----------------------------------------------------------------------===//
// EXECUTE THE JOIN
//===----------------------------------------------------------------------===//
static inline void *pmj5_thread(void *param) {
  int rv;
  arg_t *args = (arg_t *) param;

#ifdef PERF_COUNTERS
  if (args->tid == 0){
    PCM_initPerformanceMonitor(NULL, NULL);
    PCM_start();
  }
#endif

  // Wait at a barrier until each thread starts and start timer
  BARRIER_ARRIVE(args->barrier, rv);

#ifndef NO_TIMING
  // TID 0 checkpoints the start time
  if (args->tid == 0) {
    fprintf(stderr, "Skipbits: %u, hashmask: %u, partmask: %u\n",
           args->hts[0]->skip_bits, args->hts[0]->hash_mask,
           args->hts[0]->skip_mask);

    gettimeofday(&args->start, NULL);
    startTimer(&args->probe_time);
    startTimer(&args->build_time); 
    args->part_time = 0;
  }
#endif

  if (args->part_start < args->part_end) {    
    // BUILD
    relation_t *relR = &args->relR;
    for (uint32_t i = 0; i < relR->num_tuples; i += VECTOR_SIZE) {
      uint32_t sz = i + VECTOR_SIZE < relR->num_tuples 
                    ? VECTOR_SIZE : relR->num_tuples - i;

      build_hashtable_part(args->hts, relR->tuples + i, sz, 
                           args->part_start, args->part_end);
    }
  }

  // Wait at a barrier until each thread completes build phase
  BARRIER_ARRIVE(args->barrier, rv);

//#if 0
  if (args->tid == 0) {
    double avg_load_pct = 0.0, min_load_pct = 200.0, max_load_pct = 0.0;
    uint32_t avg_occ = 0;
    for (uint32_t i = 0; i < args->num_parts; i++) {
      uint32_t o = 0;
      for (uint32_t b = 0; b < args->hts[i]->num_buckets; b++) {
        o += (args->hts[i]->buckets[b] != 0);
      }
      double load_pct = 100.0*(double)o/(double)args->hts[i]->num_buckets;
      avg_load_pct += load_pct;
      min_load_pct = fmin(min_load_pct, load_pct);
      max_load_pct = fmax(max_load_pct, load_pct);
      avg_occ += args->hts[i]->first_empty-1;
    }
    avg_load_pct /= args->num_parts;
    avg_occ /= args->num_parts;
    fprintf(stderr, "%u tables, Avg: %.2lf%%, Min: %.2lf%%, Max: %.2lf%%, Avg occ: %u\n", 
              args->num_parts, avg_load_pct, min_load_pct, max_load_pct, avg_occ);
  }
//#endif

#ifdef PERF_COUNTERS
  if (args->tid == 0) {
    PCM_stop();
    PCM_log("========== Build phase profiling results ==========\n");
    PCM_printResults();
    PCM_start();
  }
  // Just to make sure we get consistent performance numbers
  BARRIER_ARRIVE(args->barrier, rv);
#endif

#ifndef NO_TIMING
  // Build phase finished, thread-0 checkpoints the time */
  if (args->tid == 0) {
    stopTimer(&args->build_time); 
  }
#endif

  // Probe for matching tuples from the assigned part of relS
  args->num_results = probe_hashtable(args->hts, args->bufs, &args->relS, args->num_parts);

#ifndef NO_TIMING
  // For a reliable timing we have to wait until all finishes
  BARRIER_ARRIVE(args->barrier, rv);

  // Probe phase finished, thread-0 checkpoints the time
  if(args->tid == 0) {
    stopTimer(&args->probe_time); 
    gettimeofday(&args->end, NULL);
  }
#endif

#ifdef PERF_COUNTERS
  if (args->tid == 0) {
      PCM_stop();
      PCM_log("========== Probe phase profiling results ==========\n");
      PCM_printResults();
      PCM_log("===================================================\n");
      PCM_cleanup();
  }
  // Just to make sure we get consistent performance numbers
  BARRIER_ARRIVE(args->barrier, rv);
#endif

  return 0;
}

//===----------------------------------------------------------------------===//
// Run the algorithm
//===----------------------------------------------------------------------===//
int64_t PMJ_6(relation_t *relR, relation_t *relS, int nthreads) {
  const uint32_t bucket_sz = 
    4 + /* slot in buckets array */
#ifdef SEPARATE_CHAIN
    4 + /* slot in next array */
#else
    0 + /* next is inlined into the value space */
#endif
    sizeof(vs_t);
  uint32_t num_partitions = ROUND_UP(relR->num_tuples * bucket_sz, L2_CACHE_SIZE);
  NEXT_POW_2((num_partitions));
  const uint32_t partition_bits = log2(num_partitions);
  
  const uint32_t nbuckets = relR->num_tuples / num_partitions;
  uint32_t nbuckets_pow2 = nbuckets;
  NEXT_POW_2((nbuckets_pow2));

  fprintf(stderr, "L2 size: %.2lf KB\n", L2_CACHE_SIZE / 1024.0);
  fprintf(stderr, "Bucket size: %u bytes, # buckets in hashtable: %u (actual %u)\n", 
          bucket_sz, nbuckets, nbuckets_pow2);
  fprintf(stderr, "# Partitions/hashtables: %u (%u bits)\n", 
          num_partitions, partition_bits);
  fprintf(stderr, "# Bufs: %u, # Buf elems: %u\n", num_partitions, nbuckets * REUSE);

  // Allocate each of the partition's hashtables
  hashtable_t *hts[num_partitions];
  for (uint32_t i = 0; i < num_partitions; i++) {
    allocate_hashtable(&hts[i], nbuckets);
    hts[i]->skip_bits = partition_bits;
    hts[i]->hash_mask = (hts[i]->num_buckets - 1) << hts[i]->skip_bits;
    hts[i]->skip_mask = (1 << partition_bits) - 1;
  }
  
  cpu_set_t set;
  pthread_t tid[nthreads];
  pthread_attr_t attr;
  pthread_barrier_t barrier;

  // Initialize barrier
  int rv = pthread_barrier_init(&barrier, NULL, nthreads);
  if (rv != 0) {
      printf("Couldn't create the barrier\n");
      exit(EXIT_FAILURE);
  }

  uint32_t numS = relS->num_tuples;
  const uint32_t numSthr = numS / nthreads;

  uint32_t part = 0;
  const uint32_t numPartsthr = num_partitions / nthreads;

  // Initialize per-thread args
  arg_t args[nthreads];
  pthread_attr_init(&attr);
  for (uint32_t i = 0; i < nthreads; i++){
    int cpu_idx = get_cpu_id(i);

    DEBUGMSG(1, "Assigning thread-%d to CPU-%d\n", i, cpu_idx);

    CPU_ZERO(&set);
    CPU_SET(cpu_idx, &set);
    pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &set);

    args[i].tid = i;
    args[i].hts = hts;
    args[i].num_parts = num_partitions;  
    args[i].part_bits = partition_bits;  
    args[i].barrier = &barrier;

    // Allocate per-thread partitions
    buf_thres = nbuckets * REUSE;
    args[i].bufs = (buffer_t *) malloc(sizeof(buffer_t) * num_partitions);
    for (uint32_t p = 0; p < num_partitions; p++) {
      args[i].bufs[p].buf = (tuple_t *) malloc(sizeof(tuple_t) * nbuckets * REUSE);
      args[i].bufs[p].sz = 0;
    }

    // Assign build partitions
    args[i].part_start = part;
    args[i].part_end = (i == (nthreads-1)) ? num_partitions : part + numPartsthr;
    part += numPartsthr;

    // Each thread reads all the input
    args[i].relR.num_tuples = relR->num_tuples;
    args[i].relR.tuples = relR->tuples;

    /* assing part of the relS for next thread */
    args[i].relS.num_tuples = (i == (nthreads-1)) ? numS : numSthr;
    args[i].relS.tuples = relS->tuples + numSthr * i;
    numS -= numSthr;

    rv = pthread_create(&tid[i], &attr, pmj5_thread, (void*)&args[i]);
    if (rv) {
      printf("ERROR; return code from pthread_create() is %d\n", rv);
      exit(-1);
    }
  }

  // Wait for threads and sum up results
  uint64_t result = 0;
  for (uint32_t i = 0; i < nthreads; i++){
    pthread_join(tid[i], NULL);
    result += args[i].num_results;
  }

#ifndef NO_TIMING
    /* now print the timing results: */
    print_timing(args[0].probe_time, args[0].build_time, args[0].part_time,
                relR->num_tuples, relS->num_tuples, result,
                &args[0].start, &args[0].end);
#endif

  for (uint32_t i = 0; i < nthreads; i++) {
    for (uint32_t p = 0; p < num_partitions; p++) {
      free(args[i].bufs[p].buf);
    }
    free(args[i].bufs);
  }

  for (uint32_t i = 0; i < num_partitions; i++) {
    destroy_hashtable(hts[i]);
  }

  return result;
}
