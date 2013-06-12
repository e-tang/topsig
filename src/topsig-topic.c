#include <stdio.h>
#include <stdlib.h>
#include "topsig-topic.h"
#include "topsig-global.h"
#include "topsig-config.h"
#include "topsig-search.h"

void run_topic(Search *S, const char *topic_id, const char *topic_txt, const char *topic_refine, FILE *fp)
{
  static void (*outputwriter)(FILE *fp, const char *, Results *) = NULL;
  
  outputwriter = Writer_trec;
  int num = atoi(Config("TOPIC-OUTPUT-K"));
    
  Results *R = NULL;
  if (lc_strcmp(Config("TOPIC-REFINE-INVERT"), "true")!=0) {
    R = SearchCollectionQuery(S, topic_txt, num);
    if (topic_refine && atoi(Config("TOPIC-REFINE-K"))>0) {
      ApplyFeedback(S, R, topic_refine, atoi(Config("TOPIC-REFINE-K")));
    }
  } else {
    R = SearchCollectionQuery(S, topic_refine, num);
    if (topic_txt && atoi(Config("TOPIC-REFINE-K"))>0) {
      ApplyFeedback(S, R, topic_txt, atoi(Config("TOPIC-REFINE-K")));
    }
  }

  
  outputwriter(fp, topic_id, R);
  
  FreeResults(R);
}

void reader_wsj(Search *S, FILE *in, FILE *out)
{
  int topicnum;
  static char topic_txt[65536];
  static char topic_id[128];
  for (;;) {
    if (fscanf(in, "%s %[^\n]\n", topic_id, topic_txt) < 2) break;
    run_topic(S, topic_id, topic_txt, NULL, out);
  }
}

void reader_filelist_rf(Search *S, FILE *in, FILE *out)
{
  int topicnum;
  static char topic_fname[512];
  static char topic_fquery[2048];
  static char topic_id[128];
  for (;;) {
    if (fscanf(in, "%s %[^\n]\n", topic_id, topic_fname) < 2) break;
    //sprintf(topic_id, "%d", topicnum);
    FILE *fp = fopen(topic_fname, "rb");
    fscanf(fp, "%[^\n]\n", topic_fquery);
    fseek(fp, 0, SEEK_END);
    size_t filelen = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *topic_txt = malloc(filelen + 1);
    fread(topic_txt, 1, filelen, fp);
    fclose(fp);
    topic_txt[filelen] = '\0';
    run_topic(S, topic_id, topic_fquery, topic_txt, out);
    free(topic_txt);
  }
}


void RunTopic()
{
  void (*topicreader)(Search *, FILE *, FILE *) = NULL;
  const char *topicpath = Config("TOPIC-PATH");
  const char *topicformat = Config("TOPIC-FORMAT");
  const char *topicoutput = Config("TOPIC-OUTPUT-PATH");
  
  if (lc_strcmp(topicformat, "wsj")==0) topicreader = reader_wsj;
  if (lc_strcmp(topicformat, "filelist_rf")==0) topicreader = reader_filelist_rf;
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
