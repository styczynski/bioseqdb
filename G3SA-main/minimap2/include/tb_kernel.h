#ifndef TB_KERNEL_H
#define TB_KERNEL_H

#define N_VALUE_T 4
#define N_PENALTY 1

#define GET_SUB_SCORE(subScore, query, target) \
	subScore = (query == target) ? _cudaMatchScore : -_cudaMismatchScore;\
	subScore = ((query == N_VALUE_T) || (target == N_VALUE_T)) ? -N_PENALTY : subScore;\
	
#include "gasal_kernels.h"
#include "gasal_header.h"
#include "common.h"

__global__ void traceback_kernel_gridded(uint8_t *unpacked_query_batch, uint8_t *unpacked_target_batch,  uint32_t *query_batch_lens, uint32_t *target_batch_lens, uint32_t *query_batch_offsets, uint32_t *target_batch_offsets, uint32_t* cigar, gasal_res_t *device_res, int n_tasks, uint32_t maximum_sequence_length, short2 *dblock_row, short2 *dblock_col, uint64_t* tb_offset, int* n_cigar, int bw);


#endif