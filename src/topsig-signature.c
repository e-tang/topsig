#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "topsig-signature.h"
#include "topsig-config.h"
#include "topsig-atomic.h"
#include "topsig-global.h"
#include "topsig-thread.h"
#include "ISAAC-rand.h"
#include "uthash.h"
#include "topsig-tmalloc.h"
#include "topsig-semaphore.h"

struct cacheterm {
  UT_hash_handle hh;

  int hits;
  char term[TERM_MAX_LEN+1];
  int S[1];
};

struct SignatureCache {
  struct cacheterm *cache_map;
  struct cacheterm **cache_list;
  int cache_pos;
  int iswriter;
};

static void initcache(); //forward declaration

struct Signature {
  char *id;
  int unique_terms;
  int document_char_length;
  int total_terms;
  int unused_4;
  int unused_5;
  int unused_6;
  int unused_7;
  int unused_8;
  
  int S[1];
};

static struct {
  int length;
  int density;
  int docnamelen;
  int termcachesize;
  int thread_mode;
  
  enum {
    TRADITIONAL,
    SKIP
  } method;
} cfg;

SignatureCache *NewSignatureCache(int iswriter, int iscached)
{
  SignatureCache *C = tmalloc(sizeof(SignatureCache));
  C->cache_map = NULL;
  
  if (iscached) {
    C->cache_list = tmalloc(sizeof(struct cacheterm *) * cfg.termcachesize);
    memset(C->cache_list, 0, sizeof(struct cacheterm *) * cfg.termcachesize);
  } else {
    C->cache_list = NULL;
  }
  
  C->cache_pos = 0;
  C->iswriter = iswriter;
  
  if (iswriter) {
    initcache();
  }
  return C;
}

void DestroySignatureCache(SignatureCache *C)
{
  if (C->cache_list) {
    for (int i = 0; i < cfg.termcachesize; i++) {
      if (C->cache_list[i]) free(C->cache_list[i]);
    }
    free(C->cache_list);
    
    HASH_CLEAR(hh, C->cache_map);
  }
  free(C);
}

Signature *NewSignature(const char *docid)
{
  size_t sigsize = sizeof(Signature) - sizeof(int);
  sigsize += cfg.length * sizeof(int);
  Signature *sig = tmalloc(sigsize);
  memset(sig, 0, sigsize);
  sig->id = tmalloc(strlen(docid) + 1);
  strcpy(sig->id, docid);
  
  return sig;
}

void SignatureFillDoubles(Signature *sig, double *array)
{
  for (int i = 0; i < cfg.length; i++) {
    sig->S[i] = array[i] > 0 ? 1 : -1;
  }
}

void SignatureDestroy(Signature *sig)
{
  tfree(sig->id);
  tfree(sig);
}

void SignaturePrint(Signature *sig)
{
  for (int i = 0; i < 128; i++) {
    printf("%d ", sig->S[i]);
  }
  printf("\n");
}

void FlattenSignature(Signature *sig, void *bsig, void *bmask)
{
  if (bsig) {
    unsigned char *out = bsig;
    // Write the supplied signature out as a flattened bitmap. Values >0 become 1, values <=0 become 0.

    for (int i = 0; i < cfg.length; i += 8) {
      unsigned char c = 0;
      for (int j = 0; j < 8; j++) {
        c |= (sig->S[i+j]>0) << (7-j);
      }
      *out++ = c;
    }
  }
  if (bmask) {
    unsigned char *out = bmask;
    // Write the supplied signature out as a flattened mask. Values !=0 become 1, values =0 become 0.

    for (int i = 0; i < cfg.length; i += 8) {
      unsigned char c = 0;
      for (int j = 0; j < 8; j++) {
        c |= (sig->S[i+j]!=0) << (7-j);
      }
      *out++ = c;
    }
  }
}

void SignatureSetValues(Signature *sig, int unique_terms, int document_char_length, int total_terms, int unused_4, int unused_5, int unused_6, int unused_7, int unused_8) {
  sig->unique_terms = unique_terms;
  sig->document_char_length = document_char_length;
  sig->total_terms = total_terms;
  sig->unused_4 = unused_4;
  sig->unused_5 = unused_5;
  sig->unused_6 = unused_6;
  sig->unused_7 = unused_7;
  sig->unused_8 = unused_8;
}

// Forward declarations for signature methods
static void sig_TRADITIONAL_add(int *, randctx *);
static void sig_SKIP_add(int *, randctx *);

// Signature term cache setup

void SignatureAdd(SignatureCache *C, Signature *sig, const char *term, int count)
{
  //printf("SignatureAdd() in\n");fflush(stdout);
  int sigarray[cfg.length];
  memset(sigarray, 0, cfg.length * sizeof(int));
  
  int cached = 0;
  
  struct cacheterm *ct;
  
  if (cfg.termcachesize > 0) {
    HASH_FIND_STR(C->cache_map, term, ct);
    if (ct) {
      cached = 1;
      memcpy(sigarray, ct->S, cfg.length * sizeof(int));
      //_cache_hit++;
    }
  }
  
  if (!cached) {
    // Seed the random number generator with the term used
    randctx R;
    memset(R.randrsl, 0, sizeof(R.randrsl));
    strcpy((char *)(R.randrsl), term);
    randinit(&R, TRUE);
    
    switch (cfg.method) {
      case TRADITIONAL:
        sig_TRADITIONAL_add(sigarray, &R);
        break;
      case SKIP:
        sig_SKIP_add(sigarray, &R);
        break;
      default:
        break;
    }
    
    if ((cfg.termcachesize > 0) && (C->cache_list != NULL)) {
      struct cacheterm *newterm = NULL;
      
      if (C->cache_list[C->cache_pos] == NULL) {
        C->cache_list[C->cache_pos] = tmalloc(sizeof(struct cacheterm) - sizeof(int) + cfg.length * sizeof(int));
        newterm = C->cache_list[C->cache_pos];
      } else {
        newterm = C->cache_list[C->cache_pos];
        HASH_DEL(C->cache_map, newterm);
      }
      C->cache_pos = (C->cache_pos + 1) % cfg.termcachesize;
      newterm->hits = 0;
      strcpy(newterm->term, term);
      memcpy(newterm->S, sigarray, cfg.length * sizeof(int));
      
      HASH_ADD_STR(C->cache_map, term, newterm);
    }
  }
  
  for (int i = 0; i < cfg.length; i++) {
    sig->S[i] += sigarray[i] * count;
  }
  //printf("SignatureAdd() out\n");fflush(stdout);
}


static void sig_TRADITIONAL_add(int *sig, randctx *R) {
    int pos = 0;
    int set; // number of bits to set
    int max_set = cfg.length/cfg.density/2; // half the number of bits
    
    // set half the bits to +1
    for (set=0;set<max_set;) {
        pos = rand(R)%cfg.length;
        if (!sig[pos]) {
            // here if not set already
            sig[pos] = 1;
            ++set;
        }
    }
    // set half the bits to -1
    for (set=0;set<max_set;) {
        pos = rand(R)%cfg.length;
        if (!sig[pos]) {
            // here if not set already
            sig[pos] = -1;
            ++set;
        }
    }
}

static void sig_SKIP_add(int *sig, randctx *R) {
    int pos = 0;
    int set;
    int max_set = cfg.length/cfg.density; // number of bits to set
    for (set=0;set<max_set;) {
        unsigned int r = rand(R);
        unsigned int skip = r % (cfg.density * 2 - 1) + 1;
        pos = (pos+skip)%cfg.length; //wrap around
        if (!sig[pos]) {
            // here if not set already
                sig[pos] += (r / (cfg.density * 2 - 1)) % 2 ? 1 : -1;
                ++set;
        }
    }
}

#define SIGCACHESIZE 1024
static volatile struct {
  Signature *sigs[SIGCACHESIZE];
  FILE *fp;
  struct {
    int available;
    int complete;
    int written;
  } state;
} cache;
TSemaphore sem_cachefree;
TSemaphore sem_cacheused[SIGCACHESIZE];

static void initcache()
{
  cache.fp = fopen(Config("SIGNATURE-PATH"), "wb");
  tsem_init(&sem_cachefree, 0, SIGCACHESIZE);
  for (int i = 0; i < SIGCACHESIZE; i++) {
    tsem_init(&sem_cacheused[i], 0, 0);
  }
  
  // Write out the signature file header
  // SIGNATURE FILE FORMAT is:
  // (int = 32-bit little endian)
  // int header-size (in bytes, including this)
  // int version
  // int maxnamelen
  // int sig-width
  // int sig-density
  // char[64] sig-method (null-terminated)
  
  int header_size = 5 * 4 + 64;
  int version = 1;
  int maxnamelen = cfg.docnamelen;
  int sig_width = cfg.length;
  int sig_density = cfg.density;
  char sig_method[64] = {0};
  
  strncpy(sig_method, Config("SIGNATURE-METHOD"), 64);
  
  file_write32(header_size, cache.fp);
  file_write32(version, cache.fp);
  file_write32(maxnamelen, cache.fp);
  file_write32(sig_width, cache.fp);
  file_write32(sig_density, cache.fp);
  fwrite(sig_method, 1, 64, cache.fp);
}

static void dumpsignature(Signature *sig)
{
  // Write out the signature header
  
  // Each signature has a header consisting of the document id (as a null-terminated string of maximum length defined in config)
  // and 8 signed 32-bit little-endian integer values (to allow room for expansion)
  //int unique_terms;
  //int document_char_length;
  //int total_terms;
  //int unused_4;
  //int unused_5;
  //int unused_6;
  //int unused_7;
  //int unused_8;
  int docnamelen = cfg.docnamelen;
  //printf("Testing printf\n");
  //printf("Testing printf with val %d\n", 166);
  //printf("docnamelen: %d\n", docnamelen);
  char sigheader[cfg.docnamelen+1];
  //printf("Sizeof %d\n", sizeof(sigheader));
  
  //strncpy((char *)sigheader, sig->id, cfg.docnamelen);
  strcpy(sigheader, sig->id);
  sigheader[cfg.docnamelen] = '\0'; // clip
  
  fwrite(sigheader, 1, sizeof(sigheader), cache.fp);

  file_write32(sig->unique_terms, cache.fp);
  file_write32(sig->document_char_length, cache.fp);
  file_write32(sig->total_terms, cache.fp);
  file_write32(sig->unused_4, cache.fp);
  file_write32(sig->unused_5, cache.fp);
  file_write32(sig->unused_6, cache.fp);
  file_write32(sig->unused_7, cache.fp);
  file_write32(sig->unused_8, cache.fp);
  
  unsigned char bsig[cfg.length / 8];
  FlattenSignature(sig, bsig, NULL);
  fwrite(bsig, 1, cfg.length / 8, cache.fp);
}

void SignatureWrite(SignatureCache *C, Signature *sig, const char *docid)
{
  // Write the signature to a file. This takes some care as
  // this needs to be a thread-safe function. SignatureFlush
  // will be called at the end to write out any remaining
  // signatures
    
  // If the cache is filled, spinlock. Hopefully this will never
  // be necessary
  //printf("S\n");
  tsem_wait(&sem_cachefree);
    
  int available = atomic_add(&cache.state.available, 1);
  //printf("T %d\n", available);
  //printf("SignatureWrite() generating %d\n", available%SIGCACHESIZE);
  cache.sigs[available % SIGCACHESIZE] = sig;
  atomic_add(&cache.state.complete, 1);
  tsem_post(&sem_cacheused[available % SIGCACHESIZE]);

  if (C->iswriter) {
    SignatureFlush();
  }
}

void SignatureFlush()
{
  //printf("SignatureFlush()\n");
  while (tsem_trywait(&sem_cacheused[cache.state.written%SIGCACHESIZE]) == 0) {
    dumpsignature(cache.sigs[cache.state.written%SIGCACHESIZE]);
    SignatureDestroy(cache.sigs[cache.state.written%SIGCACHESIZE]);
    atomic_add(&cache.state.written, 1);
    tsem_post(&sem_cachefree);
  };
}

void Signature_InitCfg()
{
  char *C = Config("SIGNATURE-WIDTH");
  if (C == NULL) {
    fprintf(stderr, "SIGNATURE-WIDTH unspecified\n");
    exit(1);
  }
  cfg.length = atoi(C);
  
  C = Config("SIGNATURE-DENSITY");
  if (C == NULL) {
    fprintf(stderr, "SIGNATURE-DENSITY unspecified\n");
    exit(1);
  }
  cfg.density = atoi(C);
  
  C = Config("MAX-DOCNAME-LENGTH");
  if (C == NULL) {
    fprintf(stderr, "MAX-DOCNAME-LENGTH unspecified\n");
    exit(1);
  }
  cfg.docnamelen = atoi(C);
  
  C = Config("TERM-CACHE-SIZE");
  if (C) {
    cfg.termcachesize = atoi(C);
  } else {
    cfg.termcachesize = 0;
  }
  
  C = Config("INDEX-THREADING");
  if (C && strcmp(C, "multi") == 0) {
    cfg.thread_mode = 1;
  } else {
    cfg.thread_mode = 0;
  }
  
  C = Config("SIGNATURE-METHOD");
  if (C == NULL) {
    fprintf(stderr, "SIGNATURE-METHOD unspecified\n");
    exit(1);
  }
  if (lc_strcmp(C, "TRADITIONAL")==0) cfg.method = TRADITIONAL;
  if (lc_strcmp(C, "SKIP")==0) cfg.method = SKIP;
}
