#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "topsig-config.h"
#include "topsig-search.h"
#include "topsig-global.h"
#include "topsig-signature.h"
#include "topsig-stem.h"
#include "topsig-stop.h"
#include "topsig-thread.h"

struct Search {
  FILE *sig;
  int cache_size;
  unsigned char *cache;
  int entire_file_cached;
  int sigs_cached;

  SignatureCache *sigcache;
  
  struct {
    char method[64];
    int length;
    int density;
    int docnamelen;
    int headersize;
    
    int charmask[256];
    
    int multithreading;
    int threads;
    
    int pseudofeedback;
  } cfg;
};

struct Result {
  char *docid;
  unsigned char *signature;
  double score;
};

struct Results {
  int k;
  struct Result res[1];
};

// Initialise a search handle to be used for searching a collection multiple times with minimal delay
Search *InitSearch()
{
  Search *S = malloc(sizeof(Search));

  S->sigcache = NewSignatureCache(0, 0); 
  
  S->sig = fopen(Config("SIGNATURE-PATH"), "rb");
  if (!S->sig) {
    fprintf(stderr, "Signature file could not be loaded.\n");
    exit(1);
  }
  
  char *C = Config("SIGNATURE-CACHE-SIZE");
  if (C == NULL) {
    fprintf(stderr, "SIGNATURE-CACHE-SIZE unspecified\n");
    exit(1);
  }
  S->cache_size = atoi(C);
  
  S->cache = malloc((size_t)S->cache_size * 1024 * 1024);
  if (!S->cache) {
    fprintf(stderr, "Unable to allocate signature cache\n");
    exit(1);
  }
  
  if (lc_strcmp(Config("SEARCH-THREADING"), "multi") == 0) {
    S->cfg.multithreading = 1;
    
    S->cfg.threads = atoi(Config("SEARCH-THREADS"));
    if (S->cfg.threads <= 0) {
      fprintf(stderr, "Invalid SEARCH-THREADS value\n");
      exit(1);
    }
  } else {
    S->cfg.multithreading = 0;
  }
  
  C = Config("PSEUDO-FEEDBACK-SAMPLE");
  S->cfg.pseudofeedback = 0;
  if (C) {
    S->cfg.pseudofeedback = atoi(C);
  }
  
  // Read config info
  
  S->cfg.headersize = file_read32(S->sig); // header-size
  file_read32(S->sig); // version
  S->cfg.docnamelen = file_read32(S->sig); // maxnamelen
  S->cfg.length = file_read32(S->sig); // sig_width
  S->cfg.density = file_read32(S->sig); // sig_density
  fread(S->cfg.method, 1, 64, S->sig); // sig_method
  
  // Override the config file settings with the new values
  
  char buf[256];
  sprintf(buf, "%d", S->cfg.length);
  ConfigOverride("SIGNATURE-WIDTH", buf);
  
  sprintf(buf, "%d", S->cfg.density);
  ConfigOverride("SIGNATURE-DENSITY", buf);
  
  sprintf(buf, "%d", S->cfg.docnamelen);
  ConfigOverride("MAX-DOCNAME-LENGTH", buf);
  
  ConfigOverride("SIGNATURE-METHOD", S->cfg.method);
  
  ConfigUpdate();
  
  if (lc_strcmp(Config("CHARMASK"),"alpha")==0)
    for (int i = 0; i < 256; i++) S->cfg.charmask[i] = isalpha(i);
  if (lc_strcmp(Config("CHARMASK"),"alnum")==0)
    for (int i = 0; i < 256; i++) S->cfg.charmask[i] = isalnum(i);
  if (lc_strcmp(Config("CHARMASK"),"all")==0)
    for (int i = 0; i < 256; i++) S->cfg.charmask[i] = isprint(i);
    
  S->entire_file_cached = -1;
  
  return S;
}

static Signature *create_query_signature(Search *S, const char *query)
{
  Signature *sig = NewSignature("query");
  
  char term[TERM_MAX_LEN+1];
  const char *p = query;
  
  const char *termstart = NULL;
  do {
    if (S->cfg.charmask[(int)*p]) {
      if (!termstart) {
        termstart = p;
      }
    } else {
      if (termstart) {
        strncpy(term, termstart, p-termstart);
        term[p-termstart] = '\0';
        strtolower(term);
        Stem(term);
        
        if (!IsStopword(term)) {
          SignatureAdd(S->sigcache, sig, term, 1);
        }
        termstart = NULL;
      }
    }
    p++;
  } while (*(p-1) != '\0');
  
  return sig;
}

static double calculate_document_score(Search *S, unsigned char *bsig, unsigned char *bmask, unsigned char *dsig)
{
  unsigned int c = 0;
  if ((sizeof(unsigned long int) == 8) && (S->cfg.length % 64 == 0)) {
    // 64-bit optimised version, only available if unsigned long int is 64 bits and if the signature is a
    // multiple of 64 bits
    unsigned long int *query_sig = (unsigned long int *)bsig;
    unsigned long int *mask_sig = (unsigned long int *)bmask;
    unsigned long int *doc_sig = (unsigned long int *)dsig;
    unsigned long int v;
    for (int i = 0; i < S->cfg.length / 64; i++) {
      v = (doc_sig[i] ^ query_sig[i]) & mask_sig[i];
      v = v - ((v >> 1) & 0x5555555555555555);
      v = (v & 0x3333333333333333) + ((v >> 2) & 0x3333333333333333);
      c += (((v + (v >> 4)) & 0x0f0f0f0f0f0f0f0f) * 0x0101010101010101) >> 56;
    }
  } else if ((sizeof(unsigned int) == 4) && (S->cfg.length % 32 == 0)) {
    // 32-bit optimised version, only available if unsigned int is 32 bits and if the signature is a multiple
    // of 32 bits
    unsigned int *query_sig = (unsigned int *)bsig;
    unsigned int *mask_sig = (unsigned int *)bmask;
    unsigned int *doc_sig = (unsigned int *)dsig;
    unsigned int v;
    
    for (int i = 0; i < S->cfg.length / 32; i++) {
      v = (doc_sig[i] ^ query_sig[i]) & mask_sig[i];
      v = v - ((v >> 1) & 0x55555555);
      v = (v & 0x33333333) + ((v >> 2) & 0x33333333);
      c += (((v + (v >> 4)) & 0xF0F0F0F) * 0x1010101) >> 24;
      
    }
  } else {
    fprintf(stderr, "UNIMPLEMENTED\n");
    exit(1);
  }
  
  double score = 1000.0 - (double)(c) * 1000.0 / (double)(S->cfg.length);
  return score;
}

int result_compar(const void *a, const void *b)
{
  const struct Result *A, *B;
  A = a;
  B = b;
  
  if (A->score < B->score) return 1;
  if (A->score > B->score) return -1;
  return 0;
}

void ApplyBlindFeedback(Search *S, Results *R, int sample)
{
  double dsig[S->cfg.length];
  memset(&dsig, 0, S->cfg.length * sizeof(double));
  double sample_2 = ((double)sample) / 2.0;
  
  for (int i = 0; i < sample; i++) {
    double di = i;
    for (int j = 0; j < S->cfg.length; j++) {
      dsig[j] += exp(-di*di/sample_2) * ((R->res[i].signature[j/8] & (1 << (7 - (j%8))))>0?1.0:-1.0); 
    }
  }
  
  Signature *sig = NewSignature("query");
  SignatureFillDoubles(sig, dsig);
  
  unsigned char bsig[S->cfg.length / 8];
  unsigned char bmask[S->cfg.length / 8];
  
  FlattenSignature(sig, bsig, bmask);
    
  for (int i = 0; i < R->k; i++) {
    R->res[i].score = calculate_document_score(S, bsig, bmask, R->res[i].signature);
  }
  qsort(R->res, R->k, sizeof(R->res[0]), result_compar);

  SignatureDestroy(sig);
}

void MergeResults(Results *base, Results *add)
{
  struct Result res[base->k + add->k];
  for (int i = 0; i < base->k; i++) {
    res[i] = base->res[i];
  }
  for (int i = 0; i < add->k; i++) {
    res[i+base->k] = add->res[i];
  }
  qsort(res, base->k + add->k, sizeof(res[0]), result_compar);
  
  for (int i = base->k; i < base->k + add->k; i++) {
    free(res[i].docid);
    free(res[i].signature);
  }
  free(add);
  
  for (int i = 0; i < base->k; i++) {
    base->res[i] = res[i];
  }
}

Results *FindHighestScoring(Search *S, const int start, const int count, const int topk, unsigned char *bsig, unsigned char *bmask)
{
  //printf("FindHighestScoring()\n");
  //printf("S\n");fflush(stdout);
  Results *R = malloc(sizeof(Results) - sizeof(struct Result) + sizeof(struct Result)*topk);
  R->k = topk;
  for (int i = 0; i < topk; i++) {
    R->res[i].docid = malloc(S->cfg.docnamelen + 1);
    R->res[i].signature = malloc(S->cfg.length / 8);
    R->res[i].score = -1.0;
    R->res[i].docid[0] = '_';
    R->res[i].docid[1] = '\0';
  }
  
  // Calculate the size of each signature record and the offsets to the docid and signature strings
  size_t sig_record_size = S->cfg.docnamelen + 1;
  sig_record_size += 8 * 4; // 8 32-bit ints
  size_t docid_offset = 0;
  size_t sig_offset = sig_record_size;
  sig_record_size += S->cfg.length / 8;
  
  volatile int last_lowest_set = 0;
    
  double last_lowest = -3.0;
  int i;
  for (i = start; i < start+count; i++) {
    unsigned char *signature = S->cache + sig_record_size * i + sig_offset;
    
    double score = calculate_document_score(S, bsig, bmask, signature);
    
    const char *docid = (const char *)(S->cache + sig_record_size * i + docid_offset);
    
    int lowest_j = 0;
    int duplicate_found = -1;
    if (score > last_lowest) {
      for (int j = 0; j < topk; j++) {
        if (strncmp(R->res[j].docid, docid, S->cfg.docnamelen) == 0) {
          duplicate_found = j;
          break;
        }
        if (R->res[j].score < R->res[lowest_j].score) {
          last_lowest = R->res[j].score;
          last_lowest_set++;
          lowest_j = j;
        }
      }
    }
    if (duplicate_found == -1) {
      if (score > R->res[lowest_j].score) {
        R->res[lowest_j].score = score;
        strncpy(R->res[lowest_j].docid, docid, S->cfg.docnamelen);
        memcpy(R->res[lowest_j].signature, signature, S->cfg.length / 8);
      }
    } else {
      if (score > R->res[duplicate_found].score) {
        R->res[duplicate_found].score = score;
      }
    }
  }
  //printf("E\n");fflush(stdout);
  return R;
}

Results *SearchCollection(Search *S, Signature *sig, const int topk)
{
  Results *R = NULL;

  unsigned char bsig[S->cfg.length / 8];
  unsigned char bmask[S->cfg.length / 8];
  
  FlattenSignature(sig, bsig, bmask);
  
  // Calculate the size of each signature record
  size_t sig_record_size = S->cfg.docnamelen + 1;
  sig_record_size += 8 * 4; // 8 32-bit ints
  sig_record_size += S->cfg.length / 8;
  
  // Determine the maximum number of signatures that can fit in the allocated cache
  size_t max_cached_sigs = S->cache_size * 1024 * 1024 / sig_record_size;
  
  int reached_end = 0;
  while (!reached_end) {
    if (S->entire_file_cached != 1) {
      // Read as many signatures from the file as possible
      printf("Reading from signature file... ");fflush(stdout);
      size_t sigs_read = fread(S->cache, sig_record_size, max_cached_sigs, S->sig);
      printf("done\n");fflush(stdout);      
      if (S->entire_file_cached == -1) {
        if (sigs_read < max_cached_sigs) {
          // As this is the first attempt at reading, we can assume that everything was read
          // and this means the signatures file will not need to be read in the future.
          S->entire_file_cached = 1;
          reached_end = 1;
        } else {
          // Everything was read so it is assumed that the signatures file is bigger than
          // the cache.
          S->entire_file_cached = 0;
        }
      } else {
        if (S->entire_file_cached == 0) {
          if (sigs_read < max_cached_sigs) {
            // The end of the file has been reached so rewind for the next read
            fseek(S->sig, S->cfg.headersize, SEEK_SET);
            reached_end = 1;
          }
        }
      }
      S->sigs_cached = sigs_read;
    } else {
      reached_end = 1;
    }
    
    Results *result;
    if (S->cfg.multithreading == 0) {
      result = FindHighestScoring(S, 0, S->sigs_cached, topk, bsig, bmask);
    } else {
      result = FindHighestScoring_Threaded(S, 0, S->sigs_cached, topk, bsig, bmask, S->cfg.threads);
    }
    
    if (R) {
      MergeResults(R, result);
    } else {
      R = result;
    }
  }
  
  qsort(R->res, topk, sizeof(R->res[0]), result_compar);
  
  if (S->cfg.pseudofeedback > 0) {
    ApplyBlindFeedback(S, R, S->cfg.pseudofeedback);
  }
    
  return R;
}

Results *SearchCollectionQuery(Search *S, const char *query, const int topk)
{
  Signature *sig = create_query_signature(S, query);
  //SignaturePrint(sig);

  Results *R = SearchCollection(S, sig, topk);
  SignatureDestroy(sig);
  return R;
}

void PrintResults(Results *R, int k)
{
  for (int i = 0; i < k; i++) {
    printf("%d. %s (%.6f)\n", i+1, R->res[i].docid, R->res[i].score);
  }
}

void Writer_trec(FILE *out, const char *topic_id, Results *R)
{
  for (int i = 0; i < R->k; i++) {
    fprintf(out, "%s Q0 %s %d %d %s\n", topic_id, R->res[i].docid, i+1, 1000000-i, "Topsig");
  }
}

void FreeResults(Results *R)
{
  for (int i = 0; i < R->k; i++) {
    free(R->res[i].docid);
    free(R->res[i].signature);
  }
  free(R);
}

void FreeSearch(Search *S)
{
  fclose(S->sig);
  DestroySignatureCache(S->sigcache);
  free(S->cache);
  free(S);
}
