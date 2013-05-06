#include "topsig-issl.h"
#include "topsig-global.h"
#include "topsig-search.h"
#include "topsig-signature.h"
#include "topsig-config.h"
#include "topsig-atomic.h"
#include "topsig-thread.h"
#include "uthash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef struct {
  int count;
  int *list;
} islEntry;
typedef struct {
  islEntry lookup[65536];
} islSlice;

static struct {
  int headersize;
  int maxnamelen;
  int sig_width;
  size_t sig_record_size;
  size_t sig_offset;
  int sig_slices;
} cfg;

static void islCount(FILE *, islSlice *);
static void islAdd(FILE *, islSlice *);

void readSigHeader(FILE *fp)
{
  char sig_method[64];
  cfg.headersize = file_read32(fp); // header-size
  int version = file_read32(fp); // version
  cfg.maxnamelen = file_read32(fp); // maxnamelen
  cfg.sig_width = file_read32(fp); // sig_width
  file_read32(fp); // sig_density
  if (version >= 2) {
    file_read32(fp); // sig_seed
  }
  fread(sig_method, 1, 64, fp); // sig_method
  
  if (cfg.sig_width % 16) {
    fprintf(stderr, "Error: signature width not a multiple of 16.\n");
    fflush(stderr);
  }
  
  cfg.sig_slices = cfg.sig_width / 16;
  
  cfg.sig_offset = cfg.maxnamelen + 1;
  cfg.sig_offset += 8 * 4; // 8 32-bit ints
  cfg.sig_record_size = cfg.sig_offset + cfg.sig_width / 8;
}

void RunCreateISL()
{
  FILE *fp = fopen(Config("SIGNATURE-PATH"), "rb");
  
  readSigHeader(fp);
  
  islSlice *slices = malloc(sizeof(islSlice) * cfg.sig_slices);
  memset(slices, 0, sizeof(islSlice) * cfg.sig_slices);
  
  islCount(fp, slices); // Pass 1
  islAdd(fp, slices); // Pass 2
  
}

static void islCount(FILE *fp, islSlice *slices)
{
  unsigned char sigcache[cfg.sig_record_size];
  fseek(fp, cfg.headersize, SEEK_SET);
  
  //printf("cfg.sig_offset = %d\n", cfg.sig_offset);
  //printf("cfg.sig_record_size = %d\n", cfg.sig_record_size);
  //int grandsum = 0;
  while (fread(sigcache, cfg.sig_record_size, 1, fp) > 0) {
    for (int slice = 0; slice < cfg.sig_slices; slice++) {
      int val = mem_read16(sigcache + cfg.sig_offset + 2 * slice);
      slices[slice].lookup[val].count++;
      //grandsum++;
    }
  }
  //printf("grand sum: %d\n", grandsum);
  
  for (int slice = 0; slice < cfg.sig_slices; slice++) {
    for (int val = 0; val < 65536; val++) {
      slices[slice].lookup[val].list = malloc(sizeof(int) * slices[slice].lookup[val].count);
      slices[slice].lookup[val].count = 0;
    }
  }
}

int vlookup[65536];

static void islAdd(FILE *fp, islSlice *slices)
{
  unsigned char sigcache[cfg.sig_record_size];
  fseek(fp, cfg.headersize, SEEK_SET);
  int sig_index = 0;
  
  
  while (fread(sigcache, cfg.sig_record_size, 1, fp) > 0) {
    for (int slice = 0; slice < cfg.sig_slices; slice++) {
      int val = mem_read16(sigcache + cfg.sig_offset + 2 * slice);
      int count = slices[slice].lookup[val].count;
      slices[slice].lookup[val].list[count] = sig_index;
      slices[slice].lookup[val].count++;
      vlookup[val]++;
    }
    sig_index++;
  }
  
  FILE *fo = fopen(Config("ISL-PATH"), "wb");
  file_write32(cfg.sig_slices, fo); // slices
  file_write32(0, fo); // compression
  file_write32(0, fo); // storage mode
  file_write32(sig_index, fo); // signatures
  /*
  for (int val = 0; val < 65536; val++) {
    printf("Val %d has %d\n", val, vlookup[val]);
  }
  */
  for (int slice = 0; slice < cfg.sig_slices; slice++) {
    for (int val = 0; val < 65536; val++) {
      file_write32(slices[slice].lookup[val].count, fo);
      for (int n = 0; n < slices[slice].lookup[val].count; n++) {
        file_write32(slices[slice].lookup[val].list[n], fo);
      }
    }
  }
  fclose(fo);
}

islSlice *readISL(const char *islPath, int *records)
{
  FILE *fp = fopen(islPath, "rb");
  
  unsigned char islheader[16];
  
  fread(islheader, 1, 16, fp);
  
  int islfield_slices = mem_read32(islheader);
  int islfield_compression = mem_read32(islheader+4);
  int islfield_storagemode = mem_read32(islheader+8);
  *records = mem_read32(islheader+12);
  if (islfield_slices != cfg.sig_slices) {
    fprintf(stderr, "Error: signature file and ISL incompatible\n");
    exit(1);
  }
  
  int *test_fields = (int *)islheader;
  int fastread = 0;

  if (islfield_slices == test_fields[0]) {
    if (islfield_compression == test_fields[1]) {
      if (islfield_storagemode == test_fields[2]) {
        if (*records == test_fields[3]) {
          fastread = 1;
        }
      }
    }
  }
  
  islSlice *slices = malloc(sizeof(islSlice) * cfg.sig_slices);
  
  fprintf(stderr, "Reading ISL [                ]\r");
  fprintf(stderr, "Reading ISL [");
  
  for (int slice = 0; slice < cfg.sig_slices; slice++) {
    for (int val = 0; val < 65536; val++) {
      slices[slice].lookup[val].count = file_read32(fp);
      slices[slice].lookup[val].list = malloc(sizeof(int) * slices[slice].lookup[val].count);
      if (fastread) {
        fread(slices[slice].lookup[val].list, 4, slices[slice].lookup[val].count, fp);
      } else {
        for (int n = 0; n < slices[slice].lookup[val].count; n++) {
          slices[slice].lookup[val].list[n] = file_read32(fp);
        }
      }
    }
    if ((slice % (cfg.sig_slices / 16)) == (cfg.sig_slices / 16 - 1)) {
      fprintf(stderr, ".");
    }
  }
  fprintf(stderr, "]\n");
  fclose(fp);
  
  return slices;
}

static inline int count_bits(unsigned int v) {
  v = v - ((v >> 1) & 0x55555555);
  v = (v & 0x33333333) + ((v >> 2) & 0x33333333);
  return (((v + (v >> 4)) & 0xF0F0F0F) * 0x1010101) >> 24;
}

static int bitcount_compar(const void *A, const void *B)
{
  const int *a = A;
  const int *b = B;
  
  return count_bits(*a) - count_bits(*b);
}

typedef struct {
  int score;
  int dist;
  int docid;
} result;

// For sorting results, highest to lowest
static int result_compar(const void *A, const void *B)
{
  const result *a = A;
  const result *b = B;
  
  if (a->dist != b->dist)
    return a->dist - b->dist;
  
  return a->docid - b->docid;
}

void rescoreResults(FILE *fp, result *results, int topk, unsigned char *sig, unsigned char *mask)
{
  int sig_bytes = cfg.sig_width / 8;
  unsigned char cursig[sig_bytes];

  for (int i = 0; i < topk; i++) {
    fseek(fp, cfg.headersize + cfg.sig_record_size * results[i].docid + cfg.sig_offset, SEEK_SET);
    fread(cursig, sig_bytes, 1, fp);
    
    results[i].dist = DocumentDistance(cfg.sig_width, sig, mask, cursig);
  }
}

result *extract_topk(result *results, int total_results, int topk)
{
  result *r = malloc(sizeof(result) * topk);
  int filled_n = 0;
  int worst_result = -1;
  
  for (int i = 0; i < total_results; i++) {
    if (filled_n < topk) {
      r[filled_n++] = results[i];
    } else {
      if (worst_result == -1) {
        worst_result = 0;
        for (int j = 1; j < topk; j++) {
          if (result_compar(&r[j], &r[worst_result]) > 0) {
            worst_result = j;
          }
        }
      }
      if (result_compar(&results[i], &r[worst_result]) < 0) {
        r[worst_result] = results[i];
        worst_result = -1;
      }
    }
  }
  //qsort(results, total_results, sizeof(result), result_compar);
  //memcpy(r, results, sizeof(result) * topk);
  return r;
}

void rescoreResults_buffer(unsigned char *fp_sig_buffer, result *results, int topk, unsigned char *sig, unsigned char *mask)
{
  for (int i = 0; i < topk; i++) {
    unsigned char *cursig = fp_sig_buffer + (cfg.sig_record_size * results[i].docid + cfg.sig_offset);
    
    results[i].dist = DocumentDistance(cfg.sig_width, sig, mask, cursig);
  }
}

typedef struct {
  char docname[32];
  int docindex;
  UT_hash_handle hh;
} docPosition;

void ExperimentalRerankTopFile()
{
  Search *S = InitSearch();

  FILE *fp_sig = fopen(Config("SIGNATURE-PATH"), "rb");
  
  readSigHeader(fp_sig);
  
  docPosition *docPosLut = NULL;
  
  unsigned char sigcache[cfg.sig_record_size];
  int doc_index = 0;
  while (fread(sigcache, cfg.sig_record_size, 1, fp_sig) > 0) {
    docPosition *D = malloc(sizeof(docPosition));
    strcpy(D->docname, (char *)sigcache);
    D->docindex = doc_index;
    
    //printf("Added doc %s\n", D->docname);
    HASH_ADD_STR(docPosLut, docname, D);
    doc_index++;
  }
  
  
  int sig_bytes = cfg.sig_width / 8;
  unsigned char cursig[sig_bytes];
  unsigned char curmask[sig_bytes];
  unsigned char docsig[sig_bytes];
  
  FILE *fp_input = fopen(Config("RERANK-INPUT"), "r");
  FILE *fp_topic = fopen(Config("RERANK-TOPIC"), "r");
  FILE *fp_output = fopen(Config("RERANK-OUTPUT"), "w");
  
  int cur_topic_id = -1;
  char cur_topic_name[65536];
  
  Signature *sig = NULL;
  
  for (;;) {
    int in_topic_id;
    char in_q0[3];
    char in_doc_id[4096];
    int in_rank;
    int in_score;
    char in_runname[4096];
    
    if (fscanf(fp_input, "%d %s %s %d %d %s\n", &in_topic_id, in_q0, in_doc_id, &in_rank, &in_score, in_runname) < 6) break;
    
    while (cur_topic_id < in_topic_id) {
      fscanf(fp_topic, "%d %[^\n]\n", &cur_topic_id, cur_topic_name);
      printf("Switched to topic %d [%s]\n", cur_topic_id, cur_topic_name);
      
      if (sig) {
        SignatureDestroy(sig);
        sig = NULL;
      }
      sig = CreateQuerySignature(S, cur_topic_name);
      FlattenSignature(sig, cursig, curmask);
    }
    if (cur_topic_id != in_topic_id) {
      fprintf(stderr, "ERROR: TOPIC %d NOT FOUND IN TOPIC FILE\n", in_topic_id);
      exit(1);
    }
    
    docPosition *D;
    HASH_FIND_STR(docPosLut, in_doc_id, D);
    if (D) {
      fseek(fp_sig, cfg.headersize + cfg.sig_record_size * D->docindex + cfg.sig_offset, SEEK_SET);
      fread(docsig, sig_bytes, 1, fp_sig);
      int dist = DocumentDistance(cfg.sig_width, docsig, curmask, cursig);
      
      fprintf(fp_output, "%d %s %s %d %d %s\n", in_topic_id, in_q0, in_doc_id, dist, 100000-dist, in_runname);
    } else {
      fprintf(stderr, "ERROR: DOCUMENT %s NOT FOUND IN SIGNATURE\n", in_doc_id);
      exit(1);
    }
  }
  SignatureDestroy(sig);
  
  fclose(fp_input);
  fclose(fp_topic);
  fclose(fp_output);
  fclose(fp_sig);
}

static inline void isslsumadd(int *scores, const islEntry *e, int dist)
{
  for (int n = 0; n < e->count; n++) {
    atomic_add(scores+e->list[n], 16-dist);
  }
}

static inline void isslsum(int *scores, const unsigned char *sigcache, const int *bitmask, int first_issl, int last_issl, const islSlice *slices)
{
  for (int m = first_issl; m <= last_issl; m++) {
    int dist = count_bits(bitmask[m]);
    
    //int sum = 0;
    for (int slice = 0; slice < cfg.sig_slices; slice++) {
      int val = mem_read16(sigcache + cfg.sig_offset + 2 * slice) ^ bitmask[m];
      //sum += slices[slice].lookup[val].count;
      isslsumadd(scores, &slices[slice].lookup[val], dist);

    }
  }
}

typedef struct {
  int **scores;
  unsigned char const* const* sigcaches;
  int sigcaches_n;
  const int *bitmask;
  int first_issl;
  int last_issl;
  const islSlice *slices;
} ISSLSumInput;

static void *isslsum_void(void *input)
{
  ISSLSumInput *i = (ISSLSumInput *)input;
  for (int n = 0; n < i->sigcaches_n; n++) {
    isslsum(i->scores[n], i->sigcaches[n], i->bitmask, i->first_issl, i->last_issl, i->slices);
  }
  return NULL;
}

static inline result *allocscores(const int *scores, int records)
{
  result *results = malloc(sizeof(result) * records);
  for (int i = 0; i < records; i++) {
    results[i].docid = i;
    results[i].score = scores[i];
    results[i].dist = cfg.sig_width - scores[i];
  }
  return results;
}

void RunSearchISLTurbo()
{
  FILE *fp_sig = fopen(Config("SIGNATURE-PATH"), "rb");
  FILE *fp_src = fp_sig;
  
  if (Config("SOURCE-SIGNATURE-PATH")) {
    fp_src = fopen(Config("SOURCE-SIGNATURE-PATH"), "rb");
  } else {
    fp_src = fopen(Config("SIGNATURE-PATH"), "rb");
  }
  
  int topk = atoi(Config("SEARCH-DOC-TOPK")) + 1;
  
  readSigHeader(fp_sig);
  int fpsig_sig_width = cfg.sig_width;
  readSigHeader(fp_src);
  if (fpsig_sig_width != cfg.sig_width) {
    fprintf(stderr, "Error: source and signature sigfiles differ in width. %d v %d\n", fpsig_sig_width, cfg.sig_width);
    exit(1);
  }
  
  int records;
  islSlice *slices = readISL(Config("ISL-PATH"), &records);
  fprintf(stderr, "Total records: %d\n", records);
  
  fseek(fp_sig, 0, SEEK_END);
  int fp_sig_count = (ftell(fp_sig) - cfg.headersize) / cfg.sig_record_size;
  
  if (fp_sig_count != records) {
    fprintf(stderr, "Error: the search signature file and the index contain a different number of signatures\n");
    exit(1);
  }
  fseek(fp_sig, cfg.headersize, SEEK_SET);
  unsigned char *fp_sig_buffer = malloc((long)cfg.sig_record_size * (long)records);
  if (!fp_sig_buffer) {
    fprintf(stderr, "Error: unable to allocate the %ld bytes needed to hold the signature file\n", (long)cfg.sig_record_size * (long)records);
    exit(1);
  }
  fread(fp_sig_buffer, cfg.sig_record_size, records, fp_sig);
  fclose(fp_sig);
  
  fseek(fp_src, 0, SEEK_END);
  int fp_src_count = (ftell(fp_src) - cfg.headersize) / cfg.sig_record_size;
  fseek(fp_src, 0, SEEK_SET);
  
  int compare_doc = -1;
  int compare_doc_first = 0;
  int compare_doc_last = fp_src_count - 1;
  if (Config("SEARCH-DOC")) {
    compare_doc_first = atoi(Config("SEARCH-DOC"));
    compare_doc_last = atoi(Config("SEARCH-DOC"));
  }
  if (Config("SEARCH-DOC-FIRST")) {
    compare_doc_first = atoi(Config("SEARCH-DOC-FIRST"));
  }
  if (Config("SEARCH-DOC-LAST")) {
    compare_doc_last = atoi(Config("SEARCH-DOC-LAST"));
  }


  int *bitmask = malloc(sizeof(int) * 65536);
  
  int max_dist = atoi(Config("ISL-MAX-DIST"));
  
  for (int i = 0; i < 65536; i++) {
    bitmask[i] = i;
  }
  qsort(bitmask, 65536, sizeof(int), bitcount_compar);
  
  int mlength = 65536;

  for (int i = 0; i < 65536; i++) {
    int dist = count_bits(bitmask[i]);
    if (dist > max_dist) {
      mlength = i;
      break;
    }
  }
  
  int threads = 1;
  if (Config("SEARCH-DOC-THREADS")) {
    threads = atoi(Config("SEARCH-DOC-THREADS"));
  }
  
  int lookahead = 1;
  if (Config("SEARCH-DOC-LOOKAHEAD")) {
    lookahead = atoi(Config("SEARCH-DOC-LOOKAHEAD"));
  }
  
  int **scores = malloc(sizeof(int *) * lookahead);
  unsigned char **sigcaches = malloc(sizeof(unsigned char *) * lookahead);

  for (int i = 0; i < lookahead; i++) {
    scores[i] = malloc(sizeof(int) * records);
    sigcaches[i] = malloc(sizeof(unsigned char) * cfg.sig_record_size);
  }
  
  int sigcaches_filled = 0;
  fseek(fp_src, cfg.headersize + cfg.sig_record_size * compare_doc_first, SEEK_SET);
  
  for (compare_doc = compare_doc_first; compare_doc <= compare_doc_last; compare_doc++) {
    memset(scores[sigcaches_filled], 0, sizeof(scores[0]) * records);

    fread(sigcaches[sigcaches_filled++], cfg.sig_record_size, 1, fp_src);
    
    if ((sigcaches_filled == lookahead) || (compare_doc == compare_doc_last)) {
    
      if (threads == 1) {
        for (int i = 0; i < sigcaches_filled; i++) {
          isslsum(scores[i], sigcaches[i], bitmask, 0, mlength-1, slices);
        }
      } else {
        void *inputs[threads];
        for (int cthread = 0; cthread < threads; cthread++) {
          int mstart = cthread * mlength / threads;
          int mend = (1+cthread) * mlength / threads - 1;
          
          ISSLSumInput *input = malloc(sizeof(ISSLSumInput));
          
          input->scores = scores;
          input->sigcaches = (unsigned char const * const * )sigcaches;
          input->sigcaches_n = sigcaches_filled;
          input->bitmask = bitmask;
          input->first_issl = mstart;
          input->last_issl = mend;
          input->slices = slices;
          
          inputs[cthread] = input;
          //isslsum(scores, sigcache, bitmask, mstart, mend, slices);
        }
        DivideWork(inputs, isslsum_void, threads);
        for (int cthread = 0; cthread < threads; cthread++) {
          free(inputs[cthread]);
        }
      }
      
      for (int sc = 0; sc < sigcaches_filled; sc++) {
        result *results = allocscores(scores[sc], records);
        
        result *topresults = extract_topk(results, records, topk);
        
        int sig_bytes = cfg.sig_width / 8;
        unsigned char curmask[sig_bytes];
        memset(curmask, 0xFF, sig_bytes);
        
        rescoreResults_buffer(fp_sig_buffer, topresults, topk, sigcaches[sc] + cfg.sig_offset, curmask);
        
        qsort(topresults, topk, sizeof(result), result_compar);
        
        for (int i = 0; i < topk-1; i++) {
          char *docname = (char *)fp_sig_buffer + (cfg.sig_record_size * topresults[i].docid);

          //printf("%02d. (%05d) %s  Dist: %d (first seen at %d)\n", i+1, results[i].docid, docname, results[i].dist, scoresd[results[i].docid] - 1);
          //printf("%s Q0 %s 1 1 topsig\n", sigcache, docname);
          //printf("%s DIST %03d XIST Q0 %s 1 1 topsig\n", sigcache, results[i].dist, docname);
          
          // Norm format:
          //printf("%s %s %d\n", sigcaches[sc], docname, topresults[i].dist);
          
          // TREC format:
          printf("%s Q0 %s %d %d TopISSL\n", sigcaches[sc], docname, i+1, 1000000 - i);
        }
        free(results);
        free(topresults);
        
        sigcaches_filled = 0;
      }
    }
  }
  
  fclose(fp_src);
}