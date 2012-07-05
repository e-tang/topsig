#ifndef TOPSIG_PROCESS_H
#define TOPSIG_PROCESS_H

#include "topsig-signature.h"

void Process_InitCfg();
void ProcessFile(SignatureCache *, char *, char *);

#endif
