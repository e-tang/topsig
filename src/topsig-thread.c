#include <stdio.h>
#include <stdlib.h>
#include "topsig-thread.h"

// NO_THREADING is an optional mode to disable the pthread dependency.
// Naturally attempting to do a multithreaded run will fail if this is the case.

#ifdef NO_THREADING

void ProcessFile_Threaded(char *arg_filename, char *arg_filedat)
{
  fprintf(stderr, "Error: Threading disabled. Change INDEX-THREADING to single or compile in threading support.\n");
  exit(1);
}
void Flush_Threaded() { SignatureFlush(); }
void ThreadYield(){}

Results *FindHighestScoring_Threaded(Search *S, const int start, const int count, const int topk, unsigned char *bsig, unsigned char *bmask)
{
  fprintf(stderr, "Error: Threading disabled. Change SEARCH-THREADING to single or compile in threading support.\n");
  exit(1);
  return NULL;
}

struct {
  void (*func)(void);
} CallOnce_list[CALLONCE_BUFFER]; // will be init with {NULL}

void CallOnce(void (*func)())
{
  for (int i = 0; i < CALLONCE_BUFFER; i++) {
    if (CallOnce_list[i].func == func) {
      return;
    }
    if (CallOnce_list[i].func == NULL) {
      CallOnce_list[i].func = func;
      func();
      return;
    }
  }
}

#else
#endif /* NO_THREADING */

#include <string.h>
#include <pthread.h>
#include <sched.h>
#include "topsig-config.h"
#include "topsig-process.h"
#include "topsig-atomic.h"
#include "topsig-global.h"
#include "topsig-tmalloc.h"
#include "topsig-search.h"
#include "topsig-signature.h"
#include "topsig-semaphore.h"

#define JOB_POOL 512

TSemaphore sem_jobs_ready;
TSemaphore sem_job_avail[JOB_POOL];

struct thread_job {
  enum {EMPTY, READY, TAKEN} state;
  int owner;
  char *filename;
  char *filedat;
};

volatile int jobs_start = -1;
volatile int jobs_end;
volatile int current_jobs;
volatile int jobs_complete;
volatile int threads_running;
volatile int finishup;
volatile struct thread_job jobs[JOB_POOL];

int threadpool_size = 0;
pthread_t *threadpool;
SignatureCache **threadcache;

void ThreadYield()
{
  sched_yield();
}

void *start_work_writer(void *sigcache_ptr)
{
  SignatureCache *C = sigcache_ptr;
  for (;;) {
    if (finishup) break;
    SignatureFlush();
    ThreadYield();
  }
  return NULL;
}

void *start_work(void *sigcache_ptr)
{
  SignatureCache *C = sigcache_ptr;
  int threadID = atomic_add(&threads_running, 1);
  char threadID_s[4];
  
  for (;;) {
    tsem_wait(&sem_jobs_ready);
    if (finishup) break;
    
    int currjob = atomic_add(&jobs_start, 1) % JOB_POOL;

    jobs[currjob].state = TAKEN;
    jobs[currjob].owner = threadID;
    ProcessFile(C, jobs[currjob].filename, jobs[currjob].filedat);
    
    jobs[currjob].state = EMPTY;
  
    atomic_add(&jobs_complete, 1);
    atomic_sub(&current_jobs, 1);
    tsem_post(&sem_job_avail[currjob]);
  }
  
  //printf("%d> __DONE__\n", my_tid);
  
  atomic_sub(&threads_running, 1);
  //pthread_exit(NULL);
  return NULL;
}

void ProcessFile_Threaded(char *arg_filename, char *arg_filedat)
{
  if (jobs_start == -1) {
    threads_running = 0;
    memset((void *)jobs, 0, sizeof(jobs));
    
    tsem_init(&sem_jobs_ready, 0, 0);
    for (int i = 0; i < JOB_POOL; i++) {
      tsem_init(&sem_job_avail[i], 0, 1);
    }

    threadpool_size = atoi(Config("INDEX-THREADS"))+1;
    threadpool = tmalloc(sizeof(pthread_t) * threadpool_size);
    threadcache = tmalloc(sizeof(SignatureCache *) * threadpool_size);

    jobs_start = 0;
    jobs_end = 0;
    current_jobs = 0;
    jobs_complete = 0;
    finishup = 0;

    threadcache[0] = NewSignatureCache(2, 0);
    pthread_create(threadpool+0, NULL, start_work_writer, threadcache[0]);
    for (int i = 1; i < threadpool_size; i++) {
      threadcache[i] = NewSignatureCache(0, 1);
      pthread_create(threadpool+i, NULL, start_work, threadcache[i]);
    }
  }
  int currjob = atomic_add(&jobs_end, 1) % JOB_POOL;

  tsem_wait(&sem_job_avail[currjob]);

  jobs[currjob].filename = arg_filename;
  jobs[currjob].filedat = arg_filedat;
  jobs[currjob].state = READY;

  atomic_add(&current_jobs, 1);
  tsem_post(&sem_jobs_ready);
  //printf("Job %d ready.\n", currjob);
  
}

void Flush_Threaded()
{
  if (jobs_start == -1) { // single-threaded
    SignatureFlush();
    return;
  } else {
    int jobs_ready = 1;
    while (jobs_ready > 0) {
      tsem_getvalue(&sem_jobs_ready, &jobs_ready);
      ThreadYield();
    }
    if (jobs_start != -1) {
      finishup = 1;
      
      for (int i = 0; i < threadpool_size; i++) {
        tsem_post(&sem_jobs_ready);
      }
      for (int i = 0; i < threadpool_size; i++) {
        pthread_join(threadpool[i], NULL);
      }
      SignatureFlush();
    }
    return;
  }
}

struct searchthread_params {
  Search *S;
  int start;
  int count;
  int topk;
  unsigned char *bsig;
  unsigned char *bmask;
};

void *FindHighestScoring_work(void *param)
{
  struct searchthread_params *P = param;
  
  return FindHighestScoring(P->S, P->start, P->count, P->topk, P->bsig, P->bmask);
}


Results *FindHighestScoring_Threaded(Search *S, const int start, const int count, const int topk, unsigned char *bsig, unsigned char *bmask, int threadcount)
{
  pthread_t searchthreads[threadcount];
  struct searchthread_params params[threadcount];
  for (int i = 0; i < threadcount; i++) {
    params[i].S = S;
    params[i].topk = topk;
    params[i].bsig = bsig;
    params[i].bmask = bmask;
    
    params[i].start = (count / threadcount * i) + start;
    params[i].count = count / threadcount;
    if (i == (threadcount - 1)) {
      // Add remaining to the last thread
      params[i].count += count - (count / threadcount * threadcount);
    }
    pthread_create(searchthreads+i, NULL, FindHighestScoring_work, params+i);
  }
  
  Results *R = NULL;
  Results *newR = NULL;
  for (int i = 0; i < threadcount; i++) {
    void *newR_void = newR;
    pthread_join(searchthreads[i], &newR_void);
    newR = newR_void;
    if (R) {
      MergeResults(R, newR);
    } else {
      R = newR;
    }
  }
  return R;
}

struct {
  void (*func)(void);
  pthread_once_t control;
} CallOnce_list[CALLONCE_BUFFER];

void CallOnce(void (*func)(void))
{
  for (int i = 0; i < CALLONCE_BUFFER; i++) {
    if (CallOnce_list[i].func == func) {
      pthread_once(&CallOnce_list[i].control, CallOnce_list[i].func);
      return;
    }
    if (CallOnce_list[i].func == NULL) {
      CallOnce_list[i].func = func;
      pthread_once(&CallOnce_list[i].control, CallOnce_list[i].func);
      return;
    }
  }
}
