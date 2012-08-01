#ifndef TOPSIG_DOCUMENT_H
#define TOPSIG_DOCUMENT_H

typedef struct {
  char *docid;
  char *data;
  int data_length;
  void *p;
} Document;

Document *NewDocument(const char *docid, const char *data);
void FreeDocument(Document *doc);

#endif
