#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "uthash.h"

#define BUFFER_SIZE (512 * 1024)

// Document title lookup for ISSL work

typedef struct {
  char docname[32];
  char doctitle[1024];
  UT_hash_handle hh;
} doc;

doc *docs = NULL;

static void processfile(char *docid, char *filename)
{
  doc *D = malloc(sizeof(doc));
  
  strcpy(D->docname, docid);
  char *p;
  char *dp = D->doctitle;
  for (p = filename; *p != '\0'; p++) {
    *(dp++) = isprint(*p) ? *p : ' ';
  }
  *dp = '\0';
  HASH_ADD_STR(docs, docname, D);
  
  //printf("[[%s]]\n", D->doctitle);
}

static void wsjread(FILE *fp)
{
  char *doc_start;
  char *doc_end;

  char buf[BUFFER_SIZE];
  
  int buflen = fread(buf, 1, BUFFER_SIZE-1, fp);
  int doclen;
  buf[buflen] = '\0';
  
  for (;;) {
    if ((doc_start = strstr(buf, "<DOC>")) != NULL) {
      if ((doc_end = strstr(buf, "</DOC>")) != NULL) {
        doc_end += 7;
        doclen = doc_end-buf;
        //printf("Document found, %d bytes large\n", doclen);
        
        char *docid_start = strstr(buf, "<DOCNO>");
        char *docid_end = strstr(docid_start+1, "</DOCNO>");
        
        docid_start += 1;
        docid_end -= 1;
        
        docid_start += 7;
        
        int docid_len = docid_end - docid_start;
        char *docid = malloc(docid_len + 1);
        memcpy(docid, docid_start, docid_len);
        docid[docid_len] = '\0';
        
        char *title_start = strstr(buf, "<HL>");
        char *title_end = strstr(title_start+1, "</HL>");
        
        title_start += 1;
        title_end -= 0;
        
        title_start += 4;
        
        int title_len = title_end - title_start;
        char *filename = malloc(title_len + 1);
        memcpy(filename, title_start, title_len);
        filename[title_len] = '\0';
        
        processfile(docid, filename);
                
        memmove(buf, doc_end, buflen-doclen);
        buflen -= doclen;
        
        buflen += fread(buf+buflen, 1, BUFFER_SIZE-1-buflen, fp);
        buf[buflen] = '\0';
      }
    } else {
      break;
    }
  }
}

int main(int argc, char **argv)
{
  FILE *fi_in = fopen(argv[1], "r");
  FILE *fi_xml = fopen(argv[2], "rb");
  
  printf("Reading %s", argv[2]);
  wsjread(fi_xml);
  printf("...done\n");
  
  for (;;) {
    char orig_doc[32];
    char comp_doc[32];
    int doc_dist;
    if (fscanf(fi_in, "%s%s%d\n", orig_doc, comp_doc, &doc_dist) < 3) break;
    doc *D;
    
    char *orig_doc_title = "(unknown)";
    char *comp_doc_title = "(unknown)";
    
    HASH_FIND_STR(docs, orig_doc, D);
    orig_doc_title = D->doctitle;
    HASH_FIND_STR(docs, comp_doc, D);
    comp_doc_title = D->doctitle;
    
    printf("Distance: %d (\"%s\" -> \"%s\")\n", doc_dist, orig_doc_title, comp_doc_title);
  }
  
  fclose(fi_in);
  fclose(fi_xml);
  return 0;
}
