#include <stdio.h>
#include <stdlib.h>
#include "topsig-thread.h"

// For some reason, malloc/free aren't quite thread-safe
// and it is difficult to get thread-safe versions on Windows
// since mingw gcc doesn't have the -pthreads option that would
// make them thread-safe. These are implementations that should
// be able to recover even if the calls fail.

void *tmalloc(size_t mem)
{
  void *r = malloc(mem);
  while (r == NULL) {
    ThreadYield();
    r = malloc(mem);
  }
  
  return r;
}

void tfree(void *p)
{
  free(p);
}
