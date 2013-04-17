#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "uthash.h"
#include "../topsig-porterstemmer.h"

#define BUFFER_SIZE (512*1024)
#define TERM_LEN 31

typedef struct {
  char txt[TERM_LEN+1];
  float tf_a;
  float tf_b;
  UT_hash_handle hh;
} attribute;

typedef struct {
  char txt[TERM_LEN+1];
  int count;
  UT_hash_handle hh;
} term;

typedef struct {
  char *doctitle;
  term *termsearch;
  int uterms;
  int tterms;
  UT_hash_handle hh;
} doc;

doc docs[200000];
doc *docsearch = NULL;

int docs_n = 0;

int tc_total = 0;
int tc_uniq = 0;

void processterm(int docid, char *w)
{
  int newlen = stem_ts2(w, strlen(w)-1) + 1;
  w[newlen] = '\0';
  
  if (newlen > TERM_LEN) return;
  
  term *T;
  docs[docid].tterms++;
  tc_total++;
  HASH_FIND_STR(docs[docid].termsearch, w, T);
  if (T) {
    T->count++;
  } else {
    T = malloc(sizeof(term));
    strcpy(T->txt, w);
    T->count = 1;
    HASH_ADD_STR(docs[docid].termsearch, txt, T);
    docs[docid].uterms++;
    tc_uniq++;
  }
}

void processfile(char *doctitle, char *document)
{
  int docid = docs_n;
  docs[docid].doctitle = doctitle;
  docs[docid].termsearch = NULL;
  docs[docid].uterms = 0;
  docs[docid].tterms = 0;
  docs_n++;
  
  if ((docid % 1000)==0) printf("%d/%d\n", docid, 173252);
  
  doc *D = docs+docid;
  HASH_ADD_STR(docsearch, doctitle, D);
  
  char term[TERM_LEN+1];
  int term_n = 0;
  const char *p = document;
  for (;;) {
    if (isalpha(*p)) {
      term[term_n++] = tolower(*p);
    } else {
      if (term_n > 0) {
        term[term_n] = '\0';
        processterm(docid, term);
        term_n = 0;
      }
    }
    
    if (*p == '\0') break;
    p++;
  }
}

static void AR_wsj(FILE *fp)
{
  int archiveSize;
  char *doc_start;
  char *doc_end;

  char buf[BUFFER_SIZE];
  
  int buflen = fread(buf, 1, BUFFER_SIZE-1, fp);
  int doclen;
  buf[buflen] = '\0';
  
  for (;;) {
    if ((doc_start = strstr(buf, "<TEXT>")) != NULL) {
      if ((doc_end = strstr(buf, "</TEXT>")) != NULL) {
        doc_end += 8;
        doclen = doc_end-buf;
        //printf("Document found, %d bytes large\n", doclen);
        
        char *title_start = strstr(buf, "<DOCNO>");
        char *title_end = strstr(buf, "</DOCNO>");
        
        title_start += 1;
        title_end -= 1;
        
        title_start += 7;
        
        int title_len = title_end - title_start;
        char *filename = malloc(title_len + 1);
        memcpy(filename, title_start, title_len);
        filename[title_len] = '\0';
                
        archiveSize = (doc_end-8)-(doc_start + 7);

        char *filedat = malloc(archiveSize + 1);
        memcpy(filedat, doc_start + 7, archiveSize);
        filedat[archiveSize] = '\0';
        
        processfile(filename, filedat);
                
        memmove(buf, doc_end, buflen-doclen);
        buflen -= doclen;
        
        buflen += fread(buf+buflen, 1, BUFFER_SIZE-1-buflen, fp);
        buf[buflen] = '\0';
        
        // STOP EARLY -- TESTING
        if (docs_n >= 10000) break;
      }
    } else {
      break;
    }
  }
}


int main(int argc, char **argv)
{
  FILE *fp = fopen(argv[1], "rb");
  AR_wsj(fp);
  fclose(fp);
  
  printf("Collected %d documents with %d unique terms (%d total)\n", docs_n, tc_uniq, tc_total);
  
  int i;
  int j;
  for (i = 0; i < docs_n; i++) {
    for (j = 0; j < docs_n; j++) {
      printf("Comparing %s with %s\n", docs[i].doctitle, docs[j].doctitle);
      attribute *attribute_set = NULL;
      
      term *i_current, *i_tmp;
      float max_a = 0.0;
      HASH_ITER(hh, docs[i].termsearch, i_current, i_tmp) {
        attribute *A;
        HASH_FIND_STR(attribute_set, i_current->txt, A);
        if (!A) {
          A = malloc(sizeof(attribute));
          A->tf_a = 0.0f;
          A->tf_b = 0.0f;
          strcpy(A->txt, i_current->txt);
          HASH_ADD_STR(attribute_set, txt, A);
          //printf("add A %s\n", A->txt);
        }
        A->tf_a = (float)i_current->count;
        if (A->tf_a > max_a) max_a = A->tf_a;
      }
      term *j_current, *j_tmp;
      
      float max_b = 0.0;
      HASH_ITER(hh, docs[j].termsearch, j_current, j_tmp) {
        attribute *A;
        HASH_FIND_STR(attribute_set, j_current->txt, A);
        if (!A) {
          A = malloc(sizeof(attribute));
          A->tf_a = 0.0f;
          A->tf_b = 0.0f;
          strcpy(A->txt, j_current->txt);
          HASH_ADD_STR(attribute_set, txt, A);
          //printf("add B %s\n", A->txt);
        }
        A->tf_b = (float)j_current->count;
        if (A->tf_b > max_b) max_b = A->tf_b;
      }
      
      attribute *a_current, *a_tmp;
      double numer = 0.0;
      double denom_a = 0.0;
      double denom_b = 0.0;
      HASH_ITER(hh, attribute_set, a_current, a_tmp) {
        //printf("%s %f %f ND %f %f\n", a_current->txt, a_current->tf_a, a_current->tf_b, numer, denom);
        numer += a_current->tf_a/max_a * a_current->tf_b/max_b;
        denom_a += a_current->tf_a/max_a * a_current->tf_a/max_a;
        denom_b += a_current->tf_b/max_b * a_current->tf_b/max_b;
        
        HASH_DEL(attribute_set, a_current);
        free(a_current);
      }
      double denom = sqrt(denom_a + denom_b);
      printf("Similarity: %f\n", numer/denom);
    }
  }
  
  return 0;
}
