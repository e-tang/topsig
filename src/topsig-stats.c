#include <stdio.h>
#include <stdlib.h>
#include "uthash.h"
#include "topsig-global.h"
#include "topsig-config.h"
#include "topsig-stats.h"

#define DOCSTATS_TERMLEN 32

typedef struct {
  char t[DOCSTATS_TERMLEN];
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
  HASH_FIND_STR(termtable, term, cterm);
  if (cterm)
    return cterm->freq_terms;
  else
    return 0;
}

void AddTermStat(const char *word, int count)
{
  if (strlen(word) >= DOCSTATS_TERMLEN) return;

  StatTerm *cterm;
  HASH_FIND_STR(termtable, word, cterm);
  if (!cterm) {
    if (termlist_size == 0) {
      termlist_size = atoi(Config("TERMSTATS-SIZE"));
      termlist = malloc(sizeof(StatTerm) * termlist_size);
    }
    if (termlist_count < termlist_size) {
      cterm = termlist + termlist_count;
      strcpy(cterm->t, word);
      cterm->freq_docs = 0;
      cterm->freq_terms = count;
      
      HASH_ADD_STR(termtable, t, cterm);
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
    int records = ftell(fp) / (32 + 4 + 4);
    fseek(fp, 0, SEEK_SET);
    termlist = malloc(sizeof(StatTerm) * records);
    
    int pips_drawn = -1;
    printf("\n");
    for (int i = 0; i < records; i++) {
      fread(termlist[i].t, DOCSTATS_TERMLEN, 1, fp);
      termlist[i].freq_docs = file_read32(fp);
      termlist[i].freq_terms = file_read32(fp);
      total_terms += termlist[i].freq_terms;
      StatTerm *cterm = termlist + i;
      
      HASH_ADD_STR(termtable, t, cterm);

      int pips = ((i + 1) * 10 + (records / 2)) / records;
      if (pips > pips_drawn) {
        pips_drawn = pips;
        printf("\rReading term stats: [");
        for (int p = 0; p < 10; p++) {
          printf(p < pips ? "*" : " ");
        }
        printf("]");
      }

    }
    printf("\n");
    
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
    fwrite(cterm->t, DOCSTATS_TERMLEN, 1, fp);
    file_write32(cterm->freq_docs, fp);
    file_write32(cterm->freq_terms, fp);
    
    total_terms += cterm->freq_terms;
  }
  fclose(fp);
  
  printf("\n%d unique terms\n", termlist_count);
  printf("%d total terms\n", total_terms);
}
