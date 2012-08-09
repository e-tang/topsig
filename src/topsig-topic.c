#include <stdio.h>
#include <stdlib.h>
#include "topsig-topic.h"
#include "topsig-global.h"
#include "topsig-config.h"
#include "topsig-search.h"
#include "uthash.h"

void run_topic(Search *S, const char *topic_id, const char *topic_txt, FILE *fp)
{
  static void (*outputwriter)(FILE *fp, const char *, Results *) = NULL;
  
  outputwriter = Writer_trec;
  int num = atoi(Config("TOPIC-OUTPUT-K"));
  
  printf("Searching [%s]\n", topic_txt);fflush(stdout);
  
  Results *R = SearchCollectionQuery(S, topic_txt, num);
  
  outputwriter(fp, topic_id, R);
  
  FreeResults(R);
}

void reader_wsj(Search *S, FILE *in, FILE *out)
{
  int topicnum;
  static char topic_txt[65536];
  static char topic_id[128];
  for (;;) {
    if (fscanf(in, "%d %[^\n]\n", &topicnum, topic_txt) < 2) break;
    sprintf(topic_id, "%d", topicnum);
    run_topic(S, topic_id, topic_txt, out);
  }
}

void RunTopic()
{
  void (*topicreader)(Search *, FILE *, FILE *) = NULL;
  const char *topicpath = Config("TOPIC-PATH");
  const char *topicformat = Config("TOPIC-FORMAT");
  const char *topicoutput = Config("TOPIC-OUTPUT-PATH");
  
  if (lc_strcmp(topicformat, "wsj")==0) topicreader = reader_wsj;
  FILE *fp = fopen(topicpath, "rb");
  FILE *fo = fopen(topicoutput, "wb");
  
  if (!fp) {
    fprintf(stderr, "Failed to open topic file.\n");
    exit(1);
  }
  
  Search *S = InitSearch();
  
  topicreader(S, fp, fo);
  
  FreeSearch(S);
  fclose(fp);
}
