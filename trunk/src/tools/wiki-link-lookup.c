#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

int main(int argc, char **argv)
{
  FILE *fi_in = fopen(argv[1], "r");
  
  printf("<html><body>\n");
  for (;;) {
    char orig_doc[32];
    char comp_doc[32];
    int doc_dist;
    if (fscanf(fi_in, "%s%s%d\n", orig_doc, comp_doc, &doc_dist) < 3) break;
    
    int orig_doc_id = atoi(orig_doc);
    int comp_doc_id = atoi(comp_doc);
    
    if ((orig_doc_id != 0) && (comp_doc_id != 0)) {
      printf("<p>Distance %d: <a href=\"http://en.wikipedia.org/wiki/?curid=%d\">%d</a> -> <a href=\"http://en.wikipedia.org/wiki/?curid=%d\">%d</a></p>\n", doc_dist, orig_doc_id, orig_doc_id, comp_doc_id, comp_doc_id);
    }
  }
  printf("</body></html>\n");
  
  fclose(fi_in);
  return 0;
}
