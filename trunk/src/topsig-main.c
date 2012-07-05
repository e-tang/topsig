#include <stdio.h>
#include <string.h>
#include "topsig-config.h"
#include "topsig-index.h"
#include "topsig-query.h"
#include "topsig-topic.h"

void usage();

int main(int argc, const char **argv)
{
  if (argc == 1) {
    usage();
    return 0;
  }
  
  ConfigFile("config.txt");
  ConfigCLI(argc, argv);

  ConfigUpdate();

  if (strcmp(argv[1], "index")==0) {
    RunIndex();
    return 0;
  }
  
  if (strcmp(argv[1], "query")==0) {
    RunQuery();
    return 0;
  }
  
  if (strcmp(argv[1], "topic")==0) {
    RunTopic();
    return 0;
  }
  
  usage();
  return 0;
}

void usage()
{
  fprintf(stderr, "Usage: ./topsig [mode] {options}\n");
  fprintf(stderr, "Valid options for [mode] are:\n");
  fprintf(stderr, "  index\n");
  fprintf(stderr, "  query\n");
  fprintf(stderr, "  topic\n\n");
  fprintf(stderr, "Configuration information is by default read from\n");
  fprintf(stderr, "config.txt in the current working directory.\n");
  fprintf(stderr, "Additional configuration files can be added through\n");
  fprintf(stderr, "the -config [path] option.\n");
}
