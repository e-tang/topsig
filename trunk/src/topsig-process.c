#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "uthash.h"
#include "topsig-process.h"
#include "topsig-config.h"
#include "topsig-stem.h"
#include "topsig-stop.h"
#include "topsig-signature.h"
#include "topsig-global.h"
#include "topsig-thread.h"
#include "topsig-progress.h"

// All code here should be sufficiently thread safe and reentrant as it
// may be run in multiple threads. This applies to all called code too.

static struct {
  int initialised;
  int charmask[256];
  struct {
    enum {SPLIT_NONE, SPLIT_HARD, SPLIT_SENTENCE} type;
    int min;
    int max;
  } split;
  enum {FILTER_NONE, FILTER_XML} filter;
} cfg;

typedef struct {
  int len;
} docinfo;

typedef struct {
  char term[TERM_MAX_LEN+1];
  int termlen;
  int count;
  
  UT_hash_handle hh;
} docterm;

static docterm *addterm(docterm *termlist, char *term, int termlen, int *docterms)
{
  strtolower(term);
  Stem(term);
  
  if (IsStopword(term)) {
    return termlist;
  }
  
  docterm *dterm = NULL;
  
  HASH_FIND_STR(termlist, term, dterm);
  if (dterm) {
    dterm->count++;
  } else {
    docterm *newterm = malloc(sizeof(docterm));
    strcpy(newterm->term, term);
    newterm->count = 1;
    newterm->termlen = termlen;
    HASH_ADD_STR(termlist, term, newterm);
    
    *docterms = *docterms + 1;
  }
    
  return termlist;
}

// Create signature from the given document term set, then free it
static docterm *createsig(SignatureCache *C, docterm *doc, docterm *lastdoc, char *id, docinfo *info)
{
  // To ensure that signatures too small are not output, this uses
  // a delayed write mechanism that only outputs the previous
  // set of documents, merging the previous and the current
  // set if conditions are met.
  
  // For thread safety, the caller has to track doc and lastdoc.
  // This function returns 'doc' normally, or NULL in the case of
  // a merge.
  
  if (lastdoc == NULL) return doc;
    
  int merge = 0;
  if (HASH_COUNT(lastdoc) < cfg.split.min) {
    if (HASH_COUNT(lastdoc) + HASH_COUNT(doc) < cfg.split.max) {
      merge = 1;
    }
  }
  
  // Definitely output all of lastdoc- these must be part of the
  // written signature
  
  Signature *sig = NewSignature(id);
    
  docterm *curr, *tmp;
  
  int unique_terms = 0;
  int total_terms = 0;

  HASH_ITER(hh, lastdoc, curr, tmp) {
    unique_terms += 1;
    total_terms += curr->count;
    SignatureAdd(C, sig, curr->term, curr->count);
    HASH_DEL(lastdoc, curr);
    free(curr);
  }
  if (merge) {
    HASH_ITER(hh, doc, curr, tmp) {
      unique_terms += 1;
      total_terms += curr->count;
      SignatureAdd(C, sig, curr->term, curr->count);
      HASH_DEL(doc, curr);
      free(curr);
    }
    doc = NULL;
  }
  
  SignatureSetValues(sig, unique_terms, info->len, total_terms, 0, 0, 0, 0, 0);
  SignatureWrite(C, sig, id);
  return doc;
}


// Process the supplied file, then free both strings when done
void ProcessFile(SignatureCache *C, char *identifier, char *filedat)
{
  char cterm[1024];
  int cterm_len = 0;
  docinfo info;
  
  ProgressTick(identifier);

  docterm *doc = NULL;
  docterm *lastdoc = NULL;
  int docterms = 0;
  
  info.len = strlen(filedat);
  
  // XML filter vars
  int xml_inelement = 0;
  int xml_intag = 0;
  
  for (char *p = filedat; *p != '\0'; p++) {
    int filterok = 1;
    switch (cfg.filter) {
      case FILTER_NONE:
      default:
        break;
      case FILTER_XML:
        if (*p == '&') xml_inelement = 1;
        if (*p == '<') xml_intag = 1;
        if (xml_inelement && (*p == ';')) {
          xml_inelement = 0;
          filterok = 0;
        }
        if (xml_intag && (*p == '>')) {
          xml_intag = 0;
          filterok = 0;
        }
        //printf("%d %d\n", xml_intag, xml_inelement);
        if (xml_intag || xml_inelement) filterok = 0;
        break;
    }
    if (cfg.charmask[(int)*p] && filterok) {
      
      if (cterm_len < 1023) {
        cterm[cterm_len++] = *p;
      }
    } else {
      cterm[cterm_len] = '\0';
      if (cterm_len > 0) {
        if (cterm_len <= TERM_MAX_LEN) {
          doc=addterm(doc, cterm, cterm_len, &docterms);
          cterm_len = 0;
        } else {
          // Reset the term length value but don't add the term. We could
          // just clip this term, but why bother?
          cterm_len = 0;
        }
      }
    }
    if ((*p == '.') && (cfg.split.type == SPLIT_SENTENCE) && (docterms >= cfg.split.min)) {
      // Sentence split
      doc = createsig(C, doc, lastdoc, identifier, &info);
      lastdoc = doc;
      docterms = 0;
      doc = NULL;
    }
    if ((cfg.split.type != SPLIT_NONE) && (docterms >= cfg.split.max)) {
      doc = createsig(C, doc, lastdoc, identifier, &info);
      lastdoc = doc;
      docterms = 0;
      doc = NULL;
    }
  }
  if (cterm_len > 0) {
    doc=addterm(doc, cterm, cterm_len, &docterms);
    cterm_len = 0;
  }
  createsig(C, doc, lastdoc, identifier, &info);
  createsig(C, NULL, doc, identifier, &info);
  docterms = 0;
  doc = NULL;
  lastdoc = NULL;
  free(identifier);
  free(filedat);
  //printf("ProcessFile() out\n");fflush(stdout);
}

void Process_InitCfg()
{
  int alpha = lc_strcmp(Config("CHARMASK"),"alpha")==0 ? 1 : 0;
  int alnum = lc_strcmp(Config("CHARMASK"),"alnum")==0 ? 1 : 0;
  int all = lc_strcmp(Config("CHARMASK"),"all")==0 ? 1 : 0;
  
  for (int i = 0; i < 256; i++) {
    cfg.charmask[i] = (isalpha(i) && alpha) || (isalnum(i) && alnum) || (isprint(i) && all);
  }
  
  cfg.split.type = SPLIT_NONE;
  if (lc_strcmp(Config("SPLIT-TYPE"),"hard")==0) cfg.split.type = SPLIT_HARD;
  if (lc_strcmp(Config("SPLIT-TYPE"),"sentence")==0) cfg.split.type = SPLIT_SENTENCE;
  if (cfg.split.type != 0) {
    cfg.split.max = atoi(Config("SPLIT-MAX"));
    cfg.split.min = atoi(Config("SPLIT-MIN"));
  }
  
  cfg.filter = FILTER_NONE;
  if (lc_strcmp(Config("TARGET-FORMAT-FILTER"),"xml")==0) cfg.filter = FILTER_XML;
  
  cfg.initialised = 1;
}
