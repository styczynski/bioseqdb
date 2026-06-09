#ifndef CUDA_MEMMGMT_CUH
#define CUDA_MEMMGMT_CUH
#include <stdio.h>
#include <stdint.h>

typedef struct
{
	unsigned current_offset;	// current offset to the available part of the chunk
	unsigned end_offset;		// the max offset of the chunk
} CUDAKernel_mem_info;



extern __host__ void* CUDA_BufferInit(long long buffer_capacity);
// reset buffer pools
extern __host__ void CUDAResetBufferPool(void* big_pool);

/* FUNCTION TO DO MALLOC AND REALLOC WITHIN CUDA KERNELS */
// select a buffer pool from the big pool
extern __device__ void* CUDAKernelSelectPool(void* big_pool, int i);
// malloc within kernel
extern __device__ void* CUDAKernelMalloc(void* d_mem_chunk_ptr, size_t size, uint8_t align_size);
extern __device__ void* CUDAKernelCalloc(void* d_mem_chunk_ptr, size_t num, size_t size, uint8_t align_size);
// realloc within kernel
extern __device__ void* CUDAKernelRealloc(void* d_mem_chunk_ptr, void* d_current_ptr, size_t new_size, uint8_t align_size);
// memcpy within kernel
extern __device__ void cudaKernelMemcpy(void* from, void* to, size_t len);
// memmove within kernel
extern __device__ void cudaKernelMemmove(void* from, void* to, size_t len);
//debugging
extern __device__ void	printBufferInfo(void* d_buffer_pool, int pool_id);
extern void printBufferInfoHost(void* d_buffer_pools);
#endif
