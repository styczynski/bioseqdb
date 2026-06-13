/***********************************************
 * # Copyright 2009. Liu Yongchao
 * # Contact: Liu Yongchao
 * #          liuy0039@ntu.edu.sg; nkcslyc@hotmail.com
 * #
 * # GPL 2.0 applies.
 * #
 * ************************************************/

#include "GenericFunction.h"
#include<stdio.h>
#include<string.h>
#include<stdlib.h>

#define CUERR do{ cudaError_t err;      \
  if ((err = cudaGetLastError()) != cudaSuccess) {    \
      int device; \
      cudaGetDevice(&device); \
      printf("CUDA error on GPU %d: %s : %s, line %d\n", device, cudaGetErrorString(err), __FILE__, __LINE__); }}while(0);

static const enum cudaMemcpyKind kinds[] = { cudaMemcpyHostToDevice,
		cudaMemcpyDeviceToHost, cudaMemcpyDeviceToDevice };
GPUInfo* gpuInfo = 0;
GPUInfo* pInitDevice(int argc, char* argv[]) {
	int i;
	gpuInfo = (GPUInfo*) malloc(sizeof(GPUInfo));
	if (!gpuInfo) {
		fprintf(stderr, "memory allocation failed\n");
		exit(-1);
	}
	//get the number of CUDA-enabled GPUs
	gpuInfo->n_device = 0;
	cudaGetDeviceCount(&gpuInfo->n_device);
	CUERR
	if (gpuInfo->n_device <= 0) {
		fprintf(stderr, "There is no CUDA-enabled device avaiable\n");
		exit(-1);
	}
	gpuInfo->devices = (int*) malloc(sizeof(int) * gpuInfo->n_device);
	gpuInfo->props = (cudaDeviceProp*) malloc(
			sizeof(cudaDeviceProp) * gpuInfo->n_device);
	int realDevice = 0;
	for (i = 0; i < gpuInfo->n_device; i++) {
		gpuInfo->devices[realDevice] = i;
		cudaGetDeviceProperties(&gpuInfo->props[realDevice], i);
		CUERR

		/*check the compute capability*/
		if (gpuInfo->props[realDevice].regsPerBlock < 16384
				|| gpuInfo->props[realDevice].major < 3) {
			continue;
		}
		realDevice++;
	}
	gpuInfo->n_device = realDevice;

	return gpuInfo;
}
void pExitDevice(GPUInfo* info) {
	if (!info)
		return;
	if (info->devices)
		free(info->devices);
	if (info->props)
		free(info->props);
}
GPUInfo* pGetGPUInfo() {
	return gpuInfo;
}
void printGPUInfo(GPUInfo* gpuInfo)
{
	for(int realDevice = 0; realDevice < gpuInfo->n_device; ++realDevice)
	{
		fprintf(stderr, "\n---------device(%d)-------------\n", realDevice);
		fprintf(stderr, "name:%s\n", gpuInfo->props[realDevice].name);
		fprintf(stderr, "multiprocessor count:%d\n",
				gpuInfo->props[realDevice].multiProcessorCount);
		fprintf(stderr, "clock rate:%d MHz\n",
				gpuInfo->props[realDevice].clockRate);
		fprintf(stderr, "shared memory:%ld\n",
				gpuInfo->props[realDevice].sharedMemPerBlock);
		fprintf(stderr, "global  memory:%ld\n",
				gpuInfo->props[realDevice].totalGlobalMem);
		fprintf(stderr, "registers per block:%d\n",
				gpuInfo->props[realDevice].regsPerBlock);
		fprintf(stderr, "Compute capability: %d.%d\n", gpuInfo->props[realDevice].major,
					gpuInfo->props[realDevice].minor);
		fprintf(stderr, "L2 cache size: %d\n",
				gpuInfo->props[realDevice].l2CacheSize);
	
		/*calculated by MAX_TEXTURE_CACHE / (25 * 25 * sizeof(short))*/
		fprintf(stderr, "Max Query Length for Query Profile Variant: %d\n",
				(int)(gpuInfo->props[realDevice].l2CacheSize / 1250));
	}
	fprintf(stderr, "Only %d devices with compute capability >= 3.0\n",
			gpuInfo->n_device);
}
void pSetDevice(GPUInfo* info, int dev) {
	cudaSetDevice(dev);
	CUERR
}
float pGetClockRate(GPUInfo* info, int dev) {
	float frequency = info->props[dev].clockRate;
	frequency /= 1000000;

	return frequency; /*in GHz*/
}
int pGetMultiProcessorCount(GPUInfo* info, int dev) {
	return info->props[dev].multiProcessorCount;
}
int pGetRegistersPerBlock(GPUInfo* info, int dev) {
	return info->props[dev].regsPerBlock;
}
int pGetL2CacheSize(GPUInfo* info, int dev) {
	return info->props[dev].l2CacheSize;
}
void* pMallocHost(size_t size) {
	void* host;
#ifndef UNIX_EMU	
	cudaMallocHost(&host, size);
#else
	host = malloc(size);
#endif
	CUERR

	return host;
}
void pFreeHost(void*host) {
#ifndef UNIX_EMU
	cudaFreeHost(host);
#else
	if(host) free(host);
#endif
	CUERR
}
void* pMallocPitch(size_t block_size, size_t width, size_t height,
		size_t* pitch) {
	void* device;
	size_t devPitch;

	if (!pitch) {
		pitch = &devPitch;
	}

	cudaMallocPitch((void**) &device, pitch, block_size * width, height);
	CUERR

	return device;
}
void pFree(void*device) {
	cudaFree(device);
	CUERR
}
void pFreeArray(void*array) {
	cudaFreeArray((cudaArray*) array);
	CUERR
}
void pMemcpy(void*dst, const void* src, size_t count, int kind) {
	cudaMemcpy(dst, src, count, kinds[kind]);
	CUERR
}
void pMemcpy2D(void* dst, size_t dpitch, const void* src, size_t spitch,
		size_t width, size_t height, int kind) {
	cudaMemcpy2D(dst, dpitch, src, spitch, width, height, kinds[kind]);
	CUERR
}
void pMemcpyToArray(void*dst, int x, int y, const void* src, size_t count,
		int kind) {

	cudaMemcpyToArray((cudaArray*) dst, x, y, src, count, kinds[kind]);
	CUERR
}
void pMemcpy2DToArray(void* dst, int dstx, int dsty, void*src, size_t src_pitch,
		size_t width, size_t height, int kind) {
	cudaMemcpy2DToArray((cudaArray*) dst, dstx, dsty, src, src_pitch, width,
			height, kinds[kind]);
	CUERR
}
void pMemcpy2DFromArray(void*dst, size_t pitch, void*src, size_t srcx,
		size_t srcy, size_t width, size_t height, int kind) {
	cudaMemcpy2DFromArray(dst, pitch, (cudaArray*) src, srcx, srcy, width,
			height, kinds[kind]);
	CUERR
}
