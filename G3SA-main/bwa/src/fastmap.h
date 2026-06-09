#ifndef FASTMAP_H
#define FASTMAP_H

#include <iostream>
#include <fstream>
#include <string>
#include "bwa.h"
#include "zlib.h"
#include "kseq.h"
KSEQ_DECLARE(gzFile)


void *kopen(const char *fn, int *_fd);
int kclose(void *a);

#endif
