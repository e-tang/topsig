#ifndef TOPSIG_SIGNATUREWRITE_H
#define TOPSIG_SIGNATUREWRITE_H

extern volatile int cache_readers;

struct Signature;
typedef struct Signature Signature;

struct SignatureCache;
typedef struct SignatureCache SignatureCache;


void Signature_InitCfg();

Signature *NewSignature(const char *docid);
void SignatureFillDoubles(Signature *, double *);
void SignatureDestroy(Signature *sig);
void SignatureAdd(SignatureCache *, Signature *, const char *term, int count);
void SignatureSetValues(Signature *sig, int terms_in_signature, int terms_in_document, int unused_3, int unused_4, int unused_5, int unused_6, int unused_7, int unused_8);
void SignatureWrite(SignatureCache *, Signature *, const char *docid);
void SignatureFlush();
void SignaturePrint(Signature *);
void FlattenSignature(Signature *, void *, void *);

SignatureCache *NewSignatureCache(int iswriter, int iscached);
void DestroySignatureCache(SignatureCache *);

#endif
