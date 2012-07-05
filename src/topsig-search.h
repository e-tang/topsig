#ifndef TOPSIG_SEARCH_H
#define TOPSIG_SEARCH_H

#include "topsig-signature.h"

struct Search;
typedef struct Search Search;

struct Results;
typedef struct Results Results;

Search *InitSearch();
Results *SearchCollection(Search *, Signature *sig, const int topk);
Results *SearchCollectionQuery(Search *S, const char *query, const int topk);
void PrintResults(Results *, int topk);
void FreeResults(Results *);
void FreeSearch(Search *);

Results *FindHighestScoring(Search *S, const int start, const int count, const int topk, unsigned char *bsig, unsigned char *bmask);

void MergeResults(Results *, Results *);

void Writer_trec(FILE *out, const char *topic_id, Results *R);

#endif
