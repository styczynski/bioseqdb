#include "gmem_alloc.h"
#include "errchk.cuh"
#include <iostream>
#define NBUFFERPOOLS 32 	// number of buffer pools

long long each_pool_size;

__device__ unsigned cudaKernelSizeOf(void* ptr){
	/* return the size of the memory chunk that starts with ptr */
	return *(unsigned*)((char*)ptr - 4);
}

__host__ void* CUDA_BufferInit(long long buffer_capacity){
	/*
	   Allocate NBUFFERPOOLS Buffer pools.
	   First few bytes of each pool contain CUDAKernel_mem_info
	   return array of pointers to the pools
	 */
	// allocate array of pointers on host
	void** pools;
	each_pool_size = buffer_capacity / NBUFFERPOOLS;
	pools = (void**)malloc(NBUFFERPOOLS*sizeof(void*));

	// allocate NBUFFERPOOLS on device
	for (int i=0; i<NBUFFERPOOLS; i++)
		CUDA_CHECK(cudaMalloc(&pools[i], each_pool_size));

	// allocate array of pointers on device and copy the pool pointers over
	void** d_pools;
	CUDA_CHECK(cudaMalloc((void**)&d_pools, NBUFFERPOOLS*sizeof(void*)));
	if(d_pools == nullptr){
		std::cerr << "* d_pools malloc failed.\n";
		exit(1);
	}
	CUDA_CHECK(cudaMemcpy(d_pools, pools, NBUFFERPOOLS*sizeof(void*), cudaMemcpyHostToDevice));

	free(pools);
	return (void*)d_pools;
}

__host__ void CUDAResetBufferPool(void* d_buffer_pools)
{
	// first coppy the array of pool pointers to host
	void** h_pools;
	h_pools = (void**)malloc(NBUFFERPOOLS*sizeof(void*));
	CUDA_CHECK(cudaMemcpy(h_pools, (void**)d_buffer_pools, NBUFFERPOOLS*sizeof(void*), cudaMemcpyDeviceToHost));

	// reset memory info at the head of each pool
	CUDAKernel_mem_info d_pool_info;		// intermediate data on host
	for (int i = 0; i < NBUFFERPOOLS; i++){
		// find address of the start of the pool
		void* pool_addr = ((void**)h_pools)[i];
		// set base offset
		d_pool_info.current_offset = sizeof(CUDAKernel_mem_info);
		// set limit of the pool
		d_pool_info.end_offset = (unsigned)(each_pool_size);
		// copy d_pool_info to the start of the pool
		CUDA_CHECK(cudaMemcpy(pool_addr, &d_pool_info, sizeof(CUDAKernel_mem_info), cudaMemcpyHostToDevice));
	}

	free(h_pools);
}

__device__ void* CUDAKernelSelectPool(void* d_buffer_pools, int i){
	/* return pointer to the selected buffer pool */
	return ((void**)d_buffer_pools)[i];
}

__device__ void* CUDAKernelMalloc(void* d_buffer_pool, size_t size, uint8_t align_size){
	/* Malloc function to be run within kernel 
	   return pointer to a chunk of global memory
d_buffer_pool: pointer to a chunk in global memory that was allocated by CUDA_BufferInit
align_size: size of alignment of the chunk. The returned pointer is divisible by align_size (expect 1, 2, 4, 8, power of 2)
The 4 bytes before the returned pointer is the size of the chunk
	 */
	if (size==0) return 0;
	// if (size==0){printf("WARNING: thread request malloc with size=0\n"); __trap();}
	CUDAKernel_mem_info* d_pool_info = (CUDAKernel_mem_info*)d_buffer_pool;
	unsigned offset = atomicAdd(&d_pool_info->current_offset, 3+4+(align_size-1)+size); // 3+4 is padding for size + its alignment

	// enforce memory alignment
	// size pointer need to be divisible by 4
	if (offset%4)
		offset += 4 - (offset%4);
	// out pointer need to be divisible by align_size
	if ((offset+4)%align_size)
		offset += align_size - (offset+4)%align_size;

	// check if we passed the end pointer
	if (offset > d_pool_info->end_offset){
		printf("FATAL ERROR: Buffer OOM at blockID %d threadID %d\n", blockIdx.x, threadIdx.x);
		__trap();
	}
	// store size info in first 4 bytes
	unsigned* size_ptr = (unsigned*)((char*)d_buffer_pool + offset);
	*size_ptr = (unsigned)size;
	// output pointer
	void* out_ptr = (void*)((char*)d_buffer_pool + offset + 4);
	// printf("[Malloc]: block %d thread %d, offset %p\n", blockIdx.x, threadIdx.x, offset);
	return out_ptr;
}

__device__ void* CUDAKernelCalloc(void* d_buffer_pool, size_t num, size_t size, uint8_t align_size){
	/* Calloc function to be run within kernel 
	   allocate num blocks, each with size "size"
	   return pointer to first block
d_buffer_pool: pointer to a chunk in global memory that was allocated by CUDA_BufferInit
	 */
	if (size==0) return 0;
	void* outptr = CUDAKernelMalloc(d_buffer_pool, num*size, align_size);

	char *p = (char*)outptr;
	while(p < outptr + num*size) *p++ = 0;

	return outptr;
}

__device__ void* CUDAKernelRealloc(void* d_buffer_pool, void* d_current_ptr, size_t new_size, uint8_t align_size){
	/* Realloc function to be run within kernel.
d_buffer_pool: pointer to a chunk in global memory that was allocated by CUDA_BufferInit. If this is null, only do malloc
d_current_ptr: pointer to current memory block
old_size: size of current 
if new_size<old_size, simply change the size value *(d_current_ptr-4)
otherwise, allocate a bigger chunk and copy over
	 */
	if (d_current_ptr == 0){
		return CUDAKernelMalloc(d_buffer_pool, new_size, align_size);
	}

	unsigned old_size = *(unsigned*)((char*)d_current_ptr - 4);

	if (old_size < new_size){
		void* out_ptr	= CUDAKernelMalloc(d_buffer_pool, new_size, align_size);
		cudaKernelMemcpy(d_current_ptr, out_ptr, old_size);
		return out_ptr;
	} else {
		unsigned* size_ptr = (unsigned*)((char*)d_current_ptr - 4);
		*size_ptr = new_size;
		return d_current_ptr;
	}
}

__device__ void cudaKernelMemcpy(void* from, void* to, size_t len){
	/* a memcpy function that can be called within cuda kernel */
	// int i =0;	// byte counter
	// char *to_ptr, *from_ptr;
	// to_ptr = (char*)to;
	// from_ptr = (char*)from;

	// // // copy 4 bytes at a time 
	// // for (; i<len; i+=sizeof(int))
	// // 	((int*)to)[i/sizeof(int)] = ((int*)from)[i/sizeof(int)];

	// // copy 1 byte at a time
	// for (; i<len; i+=1)
	// 	to_ptr[i] = from_ptr[i];
	memcpy(to, from, len);
}

__device__ void cudaKernelMemmove(void* from, void* to, size_t len){
	/* a memmove function that can be called within cuda kernel */
	int i;	// byte counter
	if (from < to){
		// reverse copy
		for (i = len-1; i >= 0; i-=sizeof(char))
			((char*)to)[i] = ((char*)from)[i];
	} else {
		// forward copy
		for (i = 0; i < len; i+=sizeof(char))
			((char*)to)[i] = ((char*)from)[i];
	}	
}

/* print buffer info from host */
__global__ void printBufferInfoHost_kernel(void* d_buffer_pools, float* d_usage){
	int i = threadIdx.x;
	void* d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, i);
	CUDAKernel_mem_info* d_pool_info = (CUDAKernel_mem_info*)d_buffer_ptr;
	d_usage[i] =  (float)d_pool_info->current_offset/d_pool_info->end_offset;			
}

void printBufferInfoHost(void* d_buffer_pools){
	float *h_usage, *d_usage;
	h_usage = (float*)malloc(NBUFFERPOOLS*sizeof(float));
	cudaMalloc((void**)&d_usage, NBUFFERPOOLS*sizeof(float));
	printBufferInfoHost_kernel <<< 1, NBUFFERPOOLS >>> (d_buffer_pools, d_usage);
	cudaMemcpy(h_usage, d_usage, NBUFFERPOOLS*sizeof(float), cudaMemcpyDeviceToHost);
	for (int i=0; i<NBUFFERPOOLS; i++)
		fprintf(stderr, "pool %2d: %.2f used\n", i, h_usage[i]);
}
