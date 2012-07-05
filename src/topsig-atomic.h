#ifndef TOPSIG_ATOMIC_H
#define TOPSIG_ATOMIC_H

// Atomic operations for threading sync
// These should work under gcc and icc at least, new things can be defined
// in for other platforms as necessary


static inline int atomic_add(volatile int *i, int j) {
  return __sync_fetch_and_add(i,j);
}
static inline int atomic_sub(volatile int *i, int j) {
  return __sync_fetch_and_sub(i,j);
}


#endif
