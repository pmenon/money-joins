#ifndef JUDY_JOIN_H
#define JUDY_JOIN_H

#include "types.h" /* relation_t */

/** 
 * @param relR input relation R - inner relation
 * @param relS input relation S - outer relation
 * 
 * @return number of result tuples
 */
int64_t JudyJoin(relation_t *relR, relation_t *relS, int nthreads);

#endif /* JUDY_JOIN_H */
