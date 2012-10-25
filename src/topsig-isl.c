#include "topsig-isl.h"
#include "topsig-global.h"
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

void RunCreateISL()
{
  FILE *fp = fopen(Config("SIGNATURE-PATH"), "rb");
  char sig_method[64];
  cfg.headersize = file_read32(fp); // header-size
  int version = file_read32(fp); // version
  cfg.maxnamelen = file_read32(fp); // maxnamelen
  int sig_width = file_read32(fp); // sig_width
  file_read32(fp); // sig_density
  if (version >= 2) {
    file_read32(fp); // sig_seed
  }
  fread(sig_method, 1, 64, fp); // sig_method
  
  if (sig_width % 16) {
    fprintf(stderr, "Error: signature width not a multiple of 16.\n");
    fflush(stderr);
  }
  
  cfg.sig_slices = sig_width / 16;
  
  islSlice *slices = malloc(sizeof(islSlice) * cfg.sig_slices);
  memset(slices, 0, sizeof(islSlice) * cfg.sig_slices);
  
  cfg.sig_offset = cfg.maxnamelen + 1;
  cfg.sig_offset += 8 * 4; // 8 32-bit ints
  cfg.sig_record_size = cfg.sig_offset + cfg.sig_width / 8;
  
  islCount(fp, slices); // Pass 1
  islAdd(fp, slices); // Pass 2
  
}

static void islCount(FILE *fp, islSlice *slices)
{
  unsigned char sigcache[cfg.sig_record_size];
  fseek(fp, cfg.headersize, SEEK_SET);
  
  while (fread(sigcache, cfg.sig_record_size, 1, fp) > 0) {
    for (int slice = 0; slice < cfg.sig_slices; slice++) {
      int val = mem_read16(sigcache + cfg.sig_offset + 2 * slice);
      slices[slice].lookup[val].count++;
    }
  }
  
  for (int slice = 0; slice < cfg.sig_slices; slice++) {
    for (int val = 0; val < 65536; val++) {
      slices[slice].lookup[val].list = malloc(sizeof(int) * slices[slice].lookup[val].count);
      slices[slice].lookup[val].count = 0;
    }
  }
}

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
    }
    sig_index++;
  }
  
  FILE *fo = fopen(Config("ISL-PATH"), "wb");
  file_write32(cfg.sig_slices, fo); // slices
  file_write32(0, fo); // compression
  file_write32(0, fo); // storage mode
  for (int slice = 0; slice < cfg.sig_slices; slice++) {
    for (int val = 0; val < 65536; val++) {
      for (int n = 0; n < slices[slice].lookup[val].count; n++) {
        file_write32(slices[slice].lookup[val].list[n], fo);
      }
    }
  }
  fclose(fo);
}
