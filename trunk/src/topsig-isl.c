#include "topsig-isl.h"
#include "topsig-global.h"
#include "topsig-search.h"
#include "topsig-config.h"
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
  
  printf("cfg.sig_offset = %d\n", cfg.sig_offset);
  printf("cfg.sig_record_size = %d\n", cfg.sig_record_size);
  int grandsum = 0;
  while (fread(sigcache, cfg.sig_record_size, 1, fp) > 0) {
    for (int slice = 0; slice < cfg.sig_slices; slice++) {
      int val = mem_read16(sigcache + cfg.sig_offset + 2 * slice);
      slices[slice].lookup[val].count++;
      grandsum++;
    }
  }
  printf("grand sum: %d\n", grandsum);
  
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
  for (int val = 0; val < 65536; val++) {
    printf("Val %d has %d\n", val, vlookup[val]);
  }
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
  int islfield_slices = file_read32(fp);
  int islfield_compression = file_read32(fp);
  int islfield_storagemode = file_read32(fp);
  *records = file_read32(fp);
  if (islfield_slices != cfg.sig_slices) {
    fprintf(stderr, "Error: signature file and ISL incompatible\n");
    exit(1);
  }
  
  islSlice *slices = malloc(sizeof(islSlice) * cfg.sig_slices);
  
  printf("Reading ISL [                ]\r");
  printf("Reading ISL [");
  
  for (int slice = 0; slice < cfg.sig_slices; slice++) {
    for (int val = 0; val < 65536; val++) {
      slices[slice].lookup[val].count = file_read32(fp);
      slices[slice].lookup[val].list = malloc(sizeof(int) * slices[slice].lookup[val].count);
      for (int n = 0; n < slices[slice].lookup[val].count; n++) {
        slices[slice].lookup[val].list[n] = file_read32(fp);
      }
    }
    if ((slice % (cfg.sig_slices / 16)) == (cfg.sig_slices / 16 - 1)) {
      printf(".");
    }
  }
  printf("]\n");
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

static int int_compar(const void *A, const void *B)
{
  const int *a = A;
  const int *b = B;
  
  return *a - *b;
}


void RunSearchISL()
{
  FILE *fp = fopen(Config("SIGNATURE-PATH"), "rb");
  int topk = atoi(Config("SEARCH-DOC-TOPK")) + 1;
  result results[topk];
  for (int i = 0; i < topk; i++) {
    results[i].score = -1;
    results[i].docid = -1;
  }
  
  readSigHeader(fp);
  
  int records;
  islSlice *slices = readISL(Config("ISL-PATH"), &records);
  printf("Total records: %d\n", records);
  
  int dirty_n = 0;
  int dirty[500000];
  
  int compare_doc = atoi(Config("SEARCH-DOC"));
  fseek(fp, cfg.headersize + cfg.sig_record_size * compare_doc, SEEK_SET);
  unsigned char sigcache[cfg.sig_record_size];
  fread(sigcache, cfg.sig_record_size, 1, fp);
  
  printf("Searching for documents similar to %s\n", sigcache);
  
  int *scores = malloc(sizeof(int) * records);
  memset(scores, 0, sizeof(int) * records);
  
  int *scoresd = malloc(sizeof(int) * records);
  memset(scoresd, 0, sizeof(int) * records);
  
  int *bitmask = malloc(sizeof(int) * 65536);
  for (int i = 0; i < 65536; i++) {
    bitmask[i] = i;
  }
  qsort(bitmask, 65536, sizeof(int), bitcount_compar);
  
  int topk_lowest = 0;  
  int topk_2ndlowest = 0;
  int max_dist = atoi(Config("ISL-MAX-DIST"));
  for (int m = 0; m < 65536; m++) {
    int dist = count_bits(bitmask[m]);
    if (dist > max_dist) {
      fprintf(stderr, "Early exit, exceeded max dist of %d\n", max_dist);
      break;
    }
    //printf("\nM: %5d. Dist: %2d\n------------------------\n", m, dist);
    if (results[topk_lowest].score + (16-dist)*cfg.sig_slices <= results[topk_2ndlowest].score) {
      fprintf(stderr, "Early exit @ dist %d\n", dist);
      break;
    }
    
    dirty_n = 0;
    int sum = 0;
    for (int slice = 0; slice < cfg.sig_slices; slice++) {
      int val = mem_read16(sigcache + cfg.sig_offset + 2 * slice) ^ bitmask[m];
      sum+=slices[slice].lookup[val].count;
      //printf("Slice %d: %d  (total: %d)\n", slice+1, slices[slice].lookup[val].count, sum);
      for (int n = 0; n < slices[slice].lookup[val].count; n++) {
        int d = slices[slice].lookup[val].list[n];
        scores[d] += 16-dist;
        if (scoresd[d] == 0) scoresd[d] = dist + 1;
        dirty[dirty_n++] = d;
      }
    }
    
    qsort(dirty, dirty_n, sizeof(int), int_compar);
    
    int last_d = -1;
    for (int i = 0; i < dirty_n; i++) {
      int d = dirty[i];
      if (d == last_d) continue;
      
      if (scores[d] > results[topk_lowest].score) {
        int dup = -1;

        for (int j = 0; j < topk; j++) {
          if (d == results[j].docid) {
            dup = j;
            break;
          }
        }

        if (dup != -1) {
          results[dup].score = scores[d];
        } else {
          results[topk_lowest].score = scores[d];
          results[topk_lowest].docid = d;
        }
          
        for (int j = 0; j < topk; j++) {
          if (results[j].score < results[topk_lowest].score) {
            topk_lowest = j;
          }
        }
        if (topk_2ndlowest == topk_lowest) {
          topk_2ndlowest = topk_lowest == 0 ? 1 : 0;
        }
        for (int j = 0; j < topk; j++) {
          if (results[j].score < results[topk_2ndlowest].score) {
            if (j == topk_lowest) continue;
            topk_2ndlowest = j;
          }
        }

      }
    }
    
  }
  
  int sig_bytes = cfg.sig_width / 8;
  unsigned char cursig[sig_bytes];
  unsigned char curmask[sig_bytes];
  
  memset(curmask, 0xFF, sig_bytes);
  
  for (int i = 0; i < topk; i++) {
    fseek(fp, cfg.headersize + cfg.sig_record_size * results[i].docid + cfg.sig_offset, SEEK_SET);
    fread(cursig, sig_bytes, 1, fp);
    results[i].dist = DocumentDistance(cfg.sig_width, sigcache + cfg.sig_offset, curmask, cursig);
  }
  
  
  qsort(results, topk, sizeof(result), result_compar);
  
  for (int i = 0; i < topk-1; i++) {
    char docname[cfg.maxnamelen + 1];
    fseek(fp, cfg.headersize + cfg.sig_record_size * results[i].docid, SEEK_SET);
    fread(docname, 1, cfg.maxnamelen + 1, fp);

    printf("%02d. (%05d) %s  Dist: %d (first seen at %d)\n", i+1, results[i].docid, docname, results[i].dist, scoresd[results[i].docid] - 1);
  }

  /*
  int maxscore = -1;
  scores[compare_doc] = 0;
  for (int i = 0; i < records; i++) {
    if (scores[i] > maxscore) maxscore = scores[i];
  }
  for (int i = 0; i < records; i++) {
    if (scores[i] == maxscore) {
      char docname[cfg.maxnamelen + 1];
      printf("Document %d is at max score %d\n", i, maxscore);
      fseek(fp, cfg.headersize + cfg.sig_record_size * i, SEEK_SET);
      fread(docname, 1, cfg.maxnamelen + 1, fp);
      printf("Document %d id: %s\n\n", i, docname);
      
    }
  }
  */
  
  
  // Histogram display
  
  int hist[17];
  for (int i = 0; i < records; i++) {
    hist[scoresd[i]-1]++;
  }
  for (int i = 0; i < 17; i++) {
    printf("%2d: %d\n", i, hist[i]);
  }
  
  
  fclose(fp);
}

