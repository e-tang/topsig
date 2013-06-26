#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "uthash.h"

#define DOC_NUMS 100000

typedef struct {
  char docname[32];
  int score;
  int plag;
  UT_hash_handle hh;
} doc;

typedef struct {
  int topicnum;
  doc **docs;
  UT_hash_handle hh;
} topiclist;

doc doclist[DOC_NUMS];
int doclist_n = 0;

int compar(const void *a, const void *b)
{
  const doc *A = a;
  const doc *B = b;
  
  return B->score - A->score;
}

int main(int argc, char **argv)
{
  int threshold = 8;
  float score_pct_threshold = 0.02f;
  topiclist *topiclisthash = NULL;
  
  if (argc < 3) {
    fprintf(stderr, "Usage: {result file} {top file}\n");
  } else {
    FILE *fp = fopen(argv[1], "r");
    
    doc *dochash = NULL;
    int total_scores = 0;
    for (;;) {
      //1 Q0 WSJ900816-0060 1 1000000 Topsig 104
      int topicnum;
      char q0[16];
      char docname[32];
      int rank;
      int score;
      char runname[32];
      int dist;
      if (fscanf(fp, "%d %s %s %d %d %s %d\n", &topicnum, q0, docname, &rank, &score, runname, &dist) < 7) break;
      
      topiclist *T;
      HASH_FIND_INT(topiclisthash, &topicnum, T);
      if (!T) {
        T = malloc(sizeof(topiclist));
        T->topicnum = topicnum;
        T->docs = malloc(sizeof(doc *) * threshold);
        HASH_ADD_INT(topiclisthash, topicnum, T);
      }
      
      if (rank <= threshold) {
        doc *D;
        HASH_FIND_STR(dochash, docname, D);
        if (D) {
          D->score++;
        } else {
          if (doclist_n == DOC_NUMS) {
            fprintf(stderr, "Out of list space. Increase DOC_NUMS (currently %d)\n", DOC_NUMS);
          }
          D = &doclist[doclist_n++];
          strcpy(D->docname, docname);
          D->score = 1;
          HASH_ADD_STR(dochash, docname, D);
        }
        T->docs[rank-1] = D;
        total_scores++;
      }
    }
    qsort(doclist, doclist_n, sizeof(doc), compar);
    int report_results = doclist_n;
    if (report_results > 10) report_results = 10;
    for (int i = 0; i < report_results; i++) {
      doclist[i].plag = score_pct_threshold * total_scores <= doclist[i].score ? 1 : 0;
      printf("%s %d %s\n", doclist[i].docname, doclist[i].score, doclist[i].plag ? "(*)" : "");
    }
    fclose(fp);
    
    fp = fopen(argv[2], "r");
    
    for (;;) {
      int topicnum;
      char topic[1024];
      if (fscanf(fp, "%d %[^\n]\n", &topicnum, topic) < 2) break;
      topiclist *T;
      HASH_FIND_INT(topiclisthash, &topicnum, T);
      
      int plag = 0;

      if (T) {
        plag = 1;
        for (int i = 0; i < threshold; i++) {
          if (T->docs[i]->plag) plag = 2;
        }
      }
      
      printf("%d %s\n", plag, topic);
    }
    
    fclose(fp);
  }
  return 0;
}
