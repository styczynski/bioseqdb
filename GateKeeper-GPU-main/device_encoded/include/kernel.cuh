#ifndef __KERNEL
#define __KERNEL

#include <stdint.h>
#include <stdlib.h>
#include <cuda_runtime.h>

#include "../include/common.h"

char *h_read_buffer[MAX_GPU], *h_ref_buffer[MAX_GPU]; 
float gpu_time[MAX_GPU];

#ifdef __cplusplus
extern "C"
#endif
__host__ void setDevice(int currentDevice);

#ifdef __cplusplus
extern "C"
#endif
__global__ void doGateKeeper(char *reads, char *refs, short *d_filt_result,
			     short *d_edits);

#ifdef __cplusplus
extern "C"
#endif
__host__ void run_singleGPU_unified(long int batch_size, int numBlocks, int maxThreadPerBlock, RES_NODE *new_node,
				    int deviceIndex);

#ifdef __cplusplus
extern "C"
#endif
__host__ void singleDeviceAlloc(long int batch_size, int deviceIndex);

#ifdef __cplusplus
extern "C"
#endif
__host__ void clearSingleDevice(RES_NODE *res_head, int deviceIndex);

#endif
