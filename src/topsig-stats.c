#include <stdio.h>
#include <stdlib.h>
#include "uthash.h"
#include "topsig-global.h"
#include "topsig-config.h"
#include "topsig-stats.h"
#include "superfasthash.h"

int hash(const char *term)
{
  uint32_t h = SuperFastHash(term, strlen(term));
  return (int)h;
}

typedef struct {
  int t; // 32-bit hash
  unsigned int freq_docs;
  unsigned int freq_terms;
  UT_hash_handle hh;
} StatTerm;
int total_terms;

static StatTerm *termtable = NULL;
static StatTerm *termlist = NULL;
int termlist_count = 0;
int termlist_size = 0;

int TermFrequencyStats(const char *term)
{
  if (termtable == NULL) return -1;
  StatTerm *cterm;
  int term_hash = hash(term);
  HASH_FIND_INT(termtable, &term_hash, cterm);
  if (cterm)
    return cterm->freq_terms;
  else
    return 0;
}

void AddTermStat(const char *word, int count)
{
  StatTerm *cterm;
  int word_hash = hash(word);
  HASH_FIND_INT(termtable, &word_hash, cterm);
  if (!cterm) {
    if (termlist_size == 0) {
      termlist_size = atoi(Config("TERMSTATS-SIZE"));
      termlist = malloc(sizeof(StatTerm) * termlist_size);
    }
    if (termlist_count < termlist_size) {
      cterm = termlist + termlist_count;
      cterm->t = word_hash;
      cterm->freq_docs = 0;
      cterm->freq_terms = count;
      
      HASH_ADD_INT(termtable, t, cterm);
      termlist_count++;
    }
  } else {
    cterm->freq_terms += count;
  }
}

void Stats_InitCfg()
{
  if (termlist) return;
  total_terms = 0;
  char *termstats_path = Config("TERMSTATS-PATH");
  if (termstats_path) {
    FILE *fp = fopen(termstats_path, "rb");
    if (fp == NULL) return;
    fseek(fp, 0, SEEK_END);
    int records = ftell(fp) / (4 + 4 + 4);
    fseek(fp, 0, SEEK_SET);
    termlist = malloc(sizeof(StatTerm) * records);
    
    int pips_drawn = -1;
    fprintf(stderr, "\n");
    for (int i = 0; i < records; i++) {
      termlist[i].t = file_read32(fp);
      termlist[i].freq_docs = file_read32(fp);
      termlist[i].freq_terms = file_read32(fp);
      total_terms += termlist[i].freq_terms;
      StatTerm *cterm = termlist + i;
      
      HASH_ADD_INT(termtable, t, cterm);

      int pips = ((i + 1) * 10 + (records / 2)) / records;
      if (pips > pips_drawn) {
        pips_drawn = pips;
        fprintf(stderr, "Reading term stats: [");
        for (int p = 0; p < 10; p++) {
          fprintf(stderr, p < pips ? "*" : " ");
        }
        fprintf(stderr, "]");
      }

    }
    fprintf(stderr, "\n");
    
    fclose(fp);
  }
}

void WriteStats()
{
  FILE *fp = fopen(Config("TERMSTATS-PATH-OUTPUT"), "wb");
  if (!fp) {
    fprintf(stderr, "Error: unable to write termstats\n");
    exit(1);
  }
  int total_terms = 0;
  for (int i = 0; i < termlist_count; i++) {
    StatTerm *cterm = termlist + i;
    file_write32(cterm->t, fp);
    file_write32(cterm->freq_docs, fp);
    file_write32(cterm->freq_terms, fp);
    
    total_terms += cterm->freq_terms;
  }
  fclose(fp);
  
  fprintf(stderr, "\n%d unique terms\n", termlist_count);
  fprintf(stderr, "%d total terms\n", total_terms);
}
