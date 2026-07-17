#ifndef __CUDA_CONFIG
#define __CUDA_CONFIG

#include <stdio.h>

void cuda_config_(int *num_blocks, long int *batch_size, int *deviceCount,
		  int deviceCount_cmd, int *maxThreadsPerBlock, int currentDevice);

#endif
