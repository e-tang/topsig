#ifndef TOPSIG_THREAD_H
#define TOPSIG_THREAD_H

#include "topsig-search.h"

#ifdef NO_THREADING
#define CALL_ONCE_CONTROL(y) int y = 0
#define CALL_ONCE(x,y) do {if (!y) {y = 1; x();}} while(0);
#else
#include <pthread.h>

#define CALL_ONCE_CONTROL(y) pthread_once_t y = PTHREAD_ONCE_INIT
#define CALL_ONCE(x,y) pthread_once(&y,x)
#endif

// Common
void ThreadYield();

#define CALLONCE_BUFFER 16 // At least one for every different function passed to this
void CallOnce(void (*func)(void));

// Threaded indexing
void ProcessFile_Threaded(char *, char *);
void Flush_Threaded();

// Threaded searching
Results *FindHighestScoring_Threaded(Search *, const int, const int, const int, unsigned char *, unsigned char *, int);

#endif
