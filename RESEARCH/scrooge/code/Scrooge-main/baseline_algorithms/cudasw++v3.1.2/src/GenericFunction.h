/***********************************************
 * # Copyright 2009. Liu Yongchao
 * # Contact: Liu Yongchao
 * #          liuy0039@ntu.edu.sg; nkcslyc@hotmail.com
 * #
 * # GPL 2.0 applies.
 * #
 * ************************************************/

#ifndef GENERIC_FUNCTION_CU_H
#define GENERIC_FUNCTION_CU_H
#include "Defs.h"

#define pMemcpyHostToDevice	0
#define pMemcpyDeviceToHost	1
#define pMemcpyDeviceToDevice	2

struct GPUInfo
{
	//device number
	int n_device;
	//device idx
	int* devices;
	//device property
	struct cudaDeviceProp* props;
};

GPUInfo* pInitDevice(int argc, char* argv[]);
void pExitDevice(GPUInfo* info);
GPUInfo* pGetGPUInfo();
void printGPUInfo(GPUInfo* info);
void pSetDevice(GPUInfo* info, int dev);
float pGetClockRate(GPUInfo* info, int dev); /*specified GPU*/
int pGetMultiProcessorCount(GPUInfo* info, int dev); /*specified GPU*/
int pGetRegistersPerBlock(GPUInfo* info, int dev); /*specified GPU*/
int pGetL2CacheSize(GPUInfo* info, int dev);

void* pMallocHost(size_t size);
void pFreeHost(void*);
void* pMallocPitch(size_t block_size, size_t width, size_t height,
		size_t*pitch);
void pFree(void*);
void pFreeArray(void*);
void pMemcpy(void*dst, const void* src, size_t count, int kind);
void pMemcpy2D(void* dst, size_t dpitch, const void* src, size_t spitch,
		size_t width, size_t height, int kind);
void pMemcpyToArray(void*dst, int x, int y, const void* src, size_t count,
		int kind);
void pMemcpy2DToArray(void* dst, int dstx, int dsty, void*src, size_t src_pitch,
		size_t width, size_t height, int kind);
void pMemcpy2DFromArray(void*dst, size_t pitch, void*src, size_t srcx,
		size_t srcy, size_t width, size_t height, int kind);

#endif
