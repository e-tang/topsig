#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include "topsig-config.h"
#include "topsig-search.h"
#include "topsig-global.h"
#include "topsig-signature.h"
#include "topsig-stem.h"
#include "topsig-stop.h"
#include "topsig-thread.h"
#include "superfasthash.h"

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
    int seed;
    
    int charmask[256];
    
    int multithreading;
    int threads;
    
    int pseudofeedback;
  } cfg;
};

struct Result {
  char *docid;
  unsigned int docid_hash;
  unsigned char *signature;
  int dist;
  int qual;
  int offset_begin;
  int offset_end;
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
  int version = file_read32(S->sig); // version
  S->cfg.docnamelen = file_read32(S->sig); // maxnamelen
  S->cfg.length = file_read32(S->sig); // sig_width
  S->cfg.density = file_read32(S->sig); // sig_density
  S->cfg.seed = 0;
  if (version >= 2) {
    S->cfg.seed = file_read32(S->sig); // sig_seed
  }
  fread(S->cfg.method, 1, 64, S->sig); // sig_method
  
  // Override the config file settings with the new values
  
  char buf[256];
  sprintf(buf, "%d", S->cfg.length);
  ConfigOverride("SIGNATURE-WIDTH", buf);
  
  sprintf(buf, "%d", S->cfg.density);
  ConfigOverride("SIGNATURE-DENSITY", buf);
  
  sprintf(buf, "%d", S->cfg.seed);
  ConfigOverride("SIGNATURE-SEED", buf);
  
  sprintf(buf, "%d", S->cfg.docnamelen);
  ConfigOverride("MAX-DOCNAME-LENGTH", buf);
  
  ConfigOverride("SIGNATURE-METHOD", S->cfg.method);
  
  ConfigUpdate();
  
  if (lc_strcmp(Config("CHARMASK"),"alpha")==0)
    for (int i = 0; i < 256; i++) S->cfg.charmask[i] = isalpha(i);
  if (lc_strcmp(Config("CHARMASK"),"alnum")==0)
    for (int i = 0; i < 256; i++) S->cfg.charmask[i] = isalnum(i);
  if (lc_strcmp(Config("CHARMASK"),"all")==0)
    for (int i = 0; i < 256; i++) S->cfg.charmask[i] = isgraph(i);
    
  S->entire_file_cached = -1;
  
  return S;
}

Signature *CreateQuerySignature(Search *S, const char *query)
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
          SignatureAdd(S->sigcache, sig, term, 1, 3);
        }
        termstart = NULL;
      }
    }
    p++;
  } while (*(p-1) != '\0');
  
  return sig;
}

int DocumentDistance_bitwise(int sigwidth, unsigned char *in_bsig, unsigned char *in_bmask, unsigned char *in_dsig)
{
  unsigned char bsig[sigwidth / 8]; memcpy(bsig, in_bsig, sigwidth / 8);
  unsigned char bmask[sigwidth / 8]; memcpy(bmask, in_bmask, sigwidth / 8);
  unsigned char dsig[sigwidth / 8]; memcpy(dsig, in_dsig, sigwidth / 8);
  
  unsigned int c = 0;
  // 64-bit optimised version, only available if unsigned long int is 64 bits and if the signature is a
  // multiple of 64 bits
  unsigned long long *query_sig = (unsigned long int *)bsig;
  unsigned long long *mask_sig = (unsigned long int *)bmask;
  unsigned long long *doc_sig = (unsigned long int *)dsig;
  unsigned long long v;
  for (int i = 0; i < sigwidth / 64; i++) {
    v = (doc_sig[i] ^ query_sig[i]) & mask_sig[i];
    v = v - ((v >> 1) & 0x5555555555555555);
    v = (v & 0x3333333333333333) + ((v >> 2) & 0x3333333333333333);
    c += (((v + (v >> 4)) & 0x0f0f0f0f0f0f0f0f) * 0x0101010101010101) >> 56;
  }
  
  return c;
}

int DocumentDistance_popcnt(int sigwidth, unsigned char *in_bsig, unsigned char *in_bmask, unsigned char *in_dsig)
{
  unsigned char bsig[sigwidth / 8]; memcpy(bsig, in_bsig, sigwidth / 8);
  unsigned char bmask[sigwidth / 8]; memcpy(bmask, in_bmask, sigwidth / 8);
  unsigned char dsig[sigwidth / 8]; memcpy(dsig, in_dsig, sigwidth / 8);
  
  unsigned int c = 0;
  // 64-bit optimised version, only available if unsigned long int is 64 bits and if the signature is a
  // multiple of 64 bits
  unsigned long long *query_sig = (unsigned long int *)bsig;
  unsigned long long *mask_sig = (unsigned long int *)bmask;
  unsigned long long *doc_sig = (unsigned long int *)dsig;
  unsigned long long v;
  for (int i = 0; i < sigwidth / 64; i++) {
    v = (doc_sig[i] ^ query_sig[i]) & mask_sig[i];
    c += __builtin_popcountll(v);
  }
  
  return c;
}

int DocumentDistance_popcnt2(int sigwidth, unsigned char *in_bsig, unsigned char *in_bmask, unsigned char *in_dsig)
{
  unsigned long long c = 0;
  unsigned long long *query_sig = (unsigned long int *)in_bsig;
  unsigned long long *mask_sig = (unsigned long int *)in_bmask;
  unsigned long long *doc_sig = (unsigned long int *)in_dsig;
  unsigned long long buf[sigwidth/64];

  // 64-bit optimised version, only available if unsigned long int is 64 bits and if the signature is a
  // multiple of 64 bits
  unsigned long long v;
  for (int i = 0; i < sigwidth / 64; i++) {
    c += __builtin_popcountll((doc_sig[i] ^ query_sig[i]) & mask_sig[i]);
  }
  //for (int i = 0; i < sigwidth / 64; i++) {
  //   c += __builtin_popcountll(buf[i]);
  //}

  return c;
}

int DocumentDistance_popcnt3(int sigwidth, unsigned char *in_bsig, unsigned char *in_bmask, unsigned char *in_dsig)
{
  int c = 0;
  unsigned long long *query_sig = (unsigned long int *)in_bsig;
  unsigned long long *mask_sig = (unsigned long int *)in_bmask;
  unsigned long long *doc_sig = (unsigned long int *)in_dsig;
  for (int i = 0; i < sigwidth / 64; i++) {
    c += __builtin_popcountll((doc_sig[i] ^ query_sig[i]) & mask_sig[i]);
  }
  
  return c;
}

int DocumentDistance_ssse3_unrl(int sigwidth, unsigned char *in_bsig, unsigned char *in_bmask, unsigned char *in_dsig)
{
  unsigned long long bsig[sigwidth / 64]; memcpy(bsig, in_bsig, sigwidth / 64);
  unsigned long long bmask[sigwidth / 64]; memcpy(bmask, in_bmask, sigwidth / 64);
  unsigned long long dsig[sigwidth / 64]; memcpy(dsig, in_dsig, sigwidth / 64);
  
  unsigned long long c = 0;
  // 64-bit optimised version, only available if unsigned long int is 64 bits and if the signature is a
  // multiple of 64 bits
  
  unsigned long long *query_sig = (unsigned long int *)bsig;
  unsigned long long *mask_sig = (unsigned long int *)bmask;
  unsigned long long *doc_sig = (unsigned long int *)dsig;
  unsigned long long v;
  for (int i = 0; i < sigwidth / 64; i++) {
    query_sig[i] = (doc_sig[i] ^ query_sig[i]) & mask_sig[i];
  }
  
  return ssse3_popcount3(query_sig, sigwidth / 128);
}


int DocumentDistance_ssse3_unrl2(int sigwidth, unsigned char *in_bsig, unsigned char *in_bmask, unsigned char *in_dsig)
{
  unsigned long long c = 0;
  // 64-bit optimised version, only available if unsigned long int is 64 bits and if the signature is a
  // multiple of 64 bits
  
  unsigned long long *query_sig = (unsigned long int *)in_bsig;
  unsigned long long *mask_sig = (unsigned long int *)in_bmask;
  unsigned long long *doc_sig = (unsigned long int *)in_dsig;
  unsigned long long v;
  unsigned long long data[sigwidth / 64];
  for (int i = 0; i < sigwidth / 64; i++) {
    data[i] = (doc_sig[i] ^ query_sig[i]) & mask_sig[i];
  }
  
  return ssse3_popcount3(data, sigwidth / 128);
}

int DocumentDistance_ssse3_unrl3(int sigwidth, unsigned char *in_bsig, unsigned char *in_bmask, unsigned char *in_dsig)
{
  // 64-bit optimised version, only available if unsigned long int is 64 bits and if the signature is a
  // multiple of 64 bits
  
  unsigned long long *query_sig = (unsigned long int *)in_bsig;
  unsigned long long *mask_sig = (unsigned long int *)in_bmask;
  unsigned long long *doc_sig = (unsigned long int *)in_dsig;
  unsigned long long v;
  unsigned long long data[sigwidth / 64];
  for (int i = 0; i < sigwidth / 64; i++) {
    data[i] = (doc_sig[i] ^ query_sig[i]) & mask_sig[i];
  }
  
  return ssse3_popcount3(data, sigwidth / 128);
}

int DocumentDistance_old(int sigwidth, unsigned char *in_bsig, unsigned char *in_bmask, unsigned char *in_dsig)
{
  unsigned char bsig[sigwidth / 8]; memcpy(bsig, in_bsig, sigwidth / 8);
  unsigned char bmask[sigwidth / 8]; memcpy(bmask, in_bmask, sigwidth / 8);
  unsigned char dsig[sigwidth / 8]; memcpy(dsig, in_dsig, sigwidth / 8);
  
  unsigned int c = 0;
  #ifdef IS64BIT
  if ((sizeof(unsigned long long) == 8) && (sigwidth % 64 == 0)) {
    // 64-bit optimised version, only available if unsigned long int is 64 bits and if the signature is a
    // multiple of 64 bits
    unsigned long long *query_sig = (unsigned long int *)bsig;
    unsigned long long *mask_sig = (unsigned long int *)bmask;
    unsigned long long *doc_sig = (unsigned long int *)dsig;
    unsigned long long v;
    for (int i = 0; i < sigwidth / 64; i++) {
      v = (doc_sig[i] ^ query_sig[i]) & mask_sig[i];
      v = v - ((v >> 1) & 0x5555555555555555);
      v = (v & 0x3333333333333333) + ((v >> 2) & 0x3333333333333333);
      c += (((v + (v >> 4)) & 0x0f0f0f0f0f0f0f0f) * 0x0101010101010101) >> 56;
    }
  } else
  #endif /* 64BIT */
  if ((sizeof(unsigned int) == 4) && (sigwidth % 32 == 0)) {
    // 32-bit optimised version, only available if unsigned int is 32 bits and if the signature is a multiple
    // of 32 bits
    unsigned int *query_sig = (unsigned int *)bsig;
    unsigned int *mask_sig = (unsigned int *)bmask;
    unsigned int *doc_sig = (unsigned int *)dsig;
    unsigned int v;
    
    for (int i = 0; i < sigwidth / 32; i++) {
      v = (doc_sig[i] ^ query_sig[i]) & mask_sig[i];
      v = v - ((v >> 1) & 0x55555555);
      v = (v & 0x33333333) + ((v >> 2) & 0x33333333);
      c += (((v + (v >> 4)) & 0xF0F0F0F) * 0x1010101) >> 24;
      
    }
  } else {
    fprintf(stderr, "UNIMPLEMENTED\n");
    exit(1);
  }
  
  return c;
}

int DocumentDistance(int sigwidth, unsigned char *in_bsig, unsigned char *in_bmask, unsigned char *in_dsig) {
  return DocumentDistance_bitwise(sigwidth,in_bsig, in_bmask,in_dsig);
}


static int get_document_quality(Search *S, unsigned char *signature_header_vals)
{
  // the 'quality' field is the 4th field in the header
  return mem_read32(signature_header_vals + (3 * 4));
}

static int get_document_offset_begin(Search *S, unsigned char *signature_header_vals)
{
  // the 'offset_begin' field is the 5th field in the header
  return mem_read32(signature_header_vals + (4 * 4));
}
static int get_document_offset_end(Search *S, unsigned char *signature_header_vals)
{
  // the 'offset_end' field is the 6th field in the header
  return mem_read32(signature_header_vals + (5 * 4));
}

int result_compar(const void *a, const void *b)
{
  const struct Result *A, *B;
  A = a;
  B = b;
  
  if (A->dist > B->dist) return 1;
  if (A->dist < B->dist) return -1;
  if (A->qual < B->qual) return 1;
  if (A->qual > B->qual) return -1;
  return strcmp(A->docid, B->docid);
  //if (A->qual > B->qual) return -1;
  //return 0;
}

void ApplyBlindFeedback(Search *S, Results *R, int sample)
{
    double dsig[S->cfg.length];
    memset(&dsig, 0, S->cfg.length * sizeof(double));
    double sample_2 = ((double)sample) * 8;
    
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
    
    int rerank_k = atoi(Config("PSEUDO-FEEDBACK-RERANK"));
    
    for (int i = 0; i < rerank_k; i++) {
        R->res[i].dist = DocumentDistance(S->cfg.length, bsig, bmask, R->res[i].signature);
    }
    qsort(R->res, rerank_k, sizeof(R->res[0]), result_compar);
    
    SignatureDestroy(sig);    
}

void ApplyFeedback(Search *S, Results *R, const char *feedback, int k)
{   
    Signature *sig = CreateQuerySignature(S, feedback);
    
    if (R->k < k) k = R->k;
    
    unsigned char bsig[S->cfg.length / 8];
    unsigned char bmask[S->cfg.length / 8];
    FlattenSignature(sig, bsig, bmask);
        
    for (int i = 0; i < k; i++) {
        R->res[i].dist = DocumentDistance(S->cfg.length, bsig, bmask, R->res[i].signature);
    }
    qsort(R->res, k, sizeof(R->res[0]), result_compar);
    
    SignatureDestroy(sig);    
}

void MergeResults(Results *base, Results *add)
{
  int duplicates_ok = atoi(Config("DUPLICATES_OK"));
  struct Result res[base->k + add->k];
  for (int i = 0; i < base->k; i++) {
    res[i] = base->res[i];
  }
  for (int i = 0; i < add->k; i++) {
    res[i+base->k] = add->res[i];
  }
  
  for (int i = 0; i < base->k + add->k; i++) {
    for (int j = i + 1; j < base->k + add->k; j++) {
      if (res[j].docid_hash == res[i].docid_hash && strcmp(res[j].docid, res[i].docid) == 0 && !duplicates_ok) {
        // Punish the lowest ranker to reduce its position in the merged set
        if ((res[i].dist < res[j].dist) || (res[i].dist == res[j].dist && res[i].qual > res[j].qual)) {
          res[j].dist = INT_MAX;
        } else {
          res[i].dist = INT_MAX;
        }
      }
    }
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
    R->res[i].dist = INT_MAX;
    R->res[i].docid[0] = '_';
    R->res[i].docid[1] = '\0';
  }
  
  // Calculate the size of each signature record and the offsets to the docid and signature strings
  size_t sig_record_size = S->cfg.docnamelen + 1;
  sig_record_size += 8 * 4; // 8 32-bit ints
  size_t docid_offset = 0;
  size_t sig_offset = sig_record_size;
  int sig_length_bytes = S->cfg.length / 8;
  sig_record_size += sig_length_bytes;
  
  int last_lowest_dist = INT_MAX;
  int last_lowest_qual = -1;
  int i;
  int duplicates_ok = atoi(Config("DUPLICATES_OK"));
  for (i = start; i < start+count; i++) {
    unsigned char *signature_header = S->cache + sig_record_size * i;
    unsigned char *signature_header_vals = signature_header + S->cfg.docnamelen + 1;
    unsigned char *signature = S->cache + sig_record_size * i + sig_offset;
    
    int dist = DocumentDistance(S->cfg.length, bsig, bmask, signature);
    int qual = get_document_quality(S, signature_header_vals);
    int offset_begin = get_document_offset_begin(S, signature_header_vals);
    int offset_end = get_document_offset_end(S, signature_header_vals);
    
    const char *docid = (const char *)(signature_header + docid_offset);
    unsigned int docid_hash = SuperFastHash(docid, strlen(docid));
    
    int lowest_j = 0;
    int duplicate_found = -1;
    if ((dist < last_lowest_dist) || ((dist == last_lowest_dist) && (qual > last_lowest_qual))) {
      for (int j = 0; j < topk; j++) {
        if (R->res[j].docid_hash == docid_hash && strncmp(R->res[j].docid, docid, S->cfg.docnamelen) == 0 && !duplicates_ok) {
          duplicate_found = j;
          break;
        }
        if (j != lowest_j && result_compar(&R->res[j], &R->res[lowest_j])==1) {
          last_lowest_dist = R->res[j].dist;
          last_lowest_qual = R->res[j].qual;
          lowest_j = j;
        }
      }
    }
    if (duplicate_found == -1) {
      if ((dist < R->res[lowest_j].dist) || ((dist == R->res[lowest_j].dist) && (qual > R->res[lowest_j].qual))) {
        R->res[lowest_j].docid_hash = docid_hash;
        R->res[lowest_j].dist = dist;
        R->res[lowest_j].qual = qual;
        R->res[lowest_j].offset_begin = offset_begin;
        R->res[lowest_j].offset_end = offset_end;
        strncpy(R->res[lowest_j].docid, docid, S->cfg.docnamelen + 1);
        memcpy(R->res[lowest_j].signature, signature, S->cfg.length / 8);
      }
    } else {
      if ((dist < R->res[duplicate_found].dist) || ((dist == R->res[duplicate_found].dist) && (qual > R->res[duplicate_found].qual))) {
        R->res[duplicate_found].docid_hash = docid_hash;
        R->res[duplicate_found].dist = dist;
        R->res[duplicate_found].qual = qual;
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
      fprintf(stderr, "Reading from signature file... ");fflush(stderr);
      size_t sigs_read = fread(S->cache, sig_record_size, max_cached_sigs, S->sig);
      fprintf(stderr, "done\n");fflush(stderr);      
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
  Signature *sig = CreateQuerySignature(S, query);
  //SignaturePrint(sig);

  Results *R = SearchCollection(S, sig, topk);
  SignatureDestroy(sig);
  return R;
}

void PrintResults(Results *R, int k)
{
  for (int i = 0; i < k; i++) {
    printf("%d. %s (%d)\n", i+1, R->res[i].docid, R->res[i].dist);
  }
}

void Writer_trec(FILE *out, const char *topic_id, Results *R)
{
  for (int i = 0; i < R->k; i++) {
    fprintf(out, "%s Q0 %s %d %d %s %d %d %d\n", topic_id, R->res[i].docid, i+1, 1000000-i, "Topsig", R->res[i].dist, R->res[i].offset_begin, R->res[i].offset_end);
  }
}

static void freeresult(struct Result *R)
{
  free(R->docid);
  free(R->signature);
}
void FreeResults(Results *R)
{
  for (int i = 0; i < R->k; i++) {
    freeresult(&R->res[i]);
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

const char *GetResult(Results *R, int N)
{
  return R->res[N].docid;
}

void RemoveResult(Results *R, int N)
{
  freeresult(&R->res[N]);
  for (int i = N; i < R->k - 1; i++) {
    R->res[i] = R->res[i+1];
  }
  R->k--;
}
