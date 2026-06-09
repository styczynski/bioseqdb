#ifndef BWAMEM_H
#define BWAMEM_H

#include "bwa.h"

#ifdef __cplusplus
extern "C"{
#endif

int bwa_align(int gpuid, process_data_t *proc, g3_opt_t *g3_opt);

#ifdef __cplusplus
}
#endif

#endif
