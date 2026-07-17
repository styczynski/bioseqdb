/*
* Zulal Bingol
* Setting up CUDA configuration 
* depending on window count 
*/

#include <stdio.h>
#include <cuda_runtime_api.h>
#include <cuda.h>
#include <math.h>
#include "../include/common.h"

void cuda_config_(int *num_blocks, long int *batch_size, int *device_count, int deviceCount_cmd,
	int *maxThreadsPerBlock, int currentDevice){

	cudaError_t error_id = cudaGetDeviceCount(device_count);
	int device;
	struct cudaDeviceProp deviceProp;
	size_t free_= 0, least_free = 0, total_= 0;
	int block_burden;
	size_t temp_pitch, device_pitch;
	size_t thread_burden = 0;
	unsigned long long global_mem;
	int maxNumBlocks;
	int compute_cap_minor;

	if (error_id != cudaSuccess){
		printf("cudaGetDeviceCount returned %d\n-> %s\n", (int)error_id, cudaGetErrorString(error_id));
		printf("Result = FAIL\n");
		exit(EXIT_FAILURE);
	}

	if ((*device_count) == 0){
		printf("No available GPU device found, exiting...\n");
		exit(EXIT_FAILURE);
	}

	if ((*device_count) < deviceCount_cmd){
		printf("Only %d devices available. Parameters are being adjusted accordingly.\n", (*device_count));
	} 
	else{
		(*device_count) = deviceCount_cmd; 
	}

	if (exe_mode == MULTIPLE){
		for (device = 0; device < (*device_count); device++){

			cudaSetDevice(device);
			cudaGetDeviceProperties(&deviceProp, device);
			cudaMemGetInfo(&free_, &total_);
			if (device == 0 | free_ < least_free){
				least_free = free_;
				maxNumBlocks = deviceProp.maxGridSize[0];
				compute_cap_major = deviceProp.major;
				compute_cap_minor = deviceProp.minor;
			}

			//printf("\nDevice %d: \"%s\"\n", device, deviceProp.name);

			//printf("  Total amount of shared memory per block:       %lu bytes\n", deviceProp.sharedMemPerBlock);
			//printf("  Total number of registers available per block: %d\n", deviceProp.regsPerBlock);

			//printf("  Maximum number of threads per block:           %d\n", deviceProp.maxThreadsPerBlock);
			//printf("  Maximum number of threads per multiprocessor:  %d\n", deviceProp.maxThreadsPerMultiProcessor);

			//printf("  Max dimension size of a grid size    (x,y,z): (%d, %d, %d)\n",
			//   deviceProp.maxGridSize[0],
			//	   deviceProp.maxGridSize[1],
			//	   deviceProp.maxGridSize[2]);

		}
	}
	else { // exe_mode = SINGLE
		cudaSetDevice(currentDevice);
		cudaGetDeviceProperties(&deviceProp, currentDevice);
		cudaMemGetInfo(&least_free, &total_);
		maxNumBlocks = deviceProp.maxGridSize[0];
		compute_cap_major = deviceProp.major;
		compute_cap_minor = deviceProp.minor;
	}

	temp_pitch = sizeof(unsigned int) * WINDOW_COUNT; // for thread burden calc in 2D array in device
	device_pitch = deviceProp.textureAlignment;
	if (temp_pitch < device_pitch){
		temp_pitch = device_pitch;
	}

	// SINGLE THREAD BURDEN CALCULATION
	thread_burden = 2 * READ_LENGTH * sizeof(char); // raw data transfered to GPU (read + ref segment) 
	thread_burden += 2 * temp_pitch; // read_buffer + ref_buffer
	thread_burden += temp_pitch * TOTAL_MASKS; // masks + a.masks
	thread_burden += 2 * temp_pitch; // shifted_temps

	*maxThreadsPerBlock = deviceProp.maxThreadsPerBlock;
	block_burden = (*maxThreadsPerBlock) * thread_burden;
	*num_blocks = least_free / block_burden;
	if ((*num_blocks) > maxNumBlocks){
		(*num_blocks) = maxNumBlocks;
	}
	*batch_size = (*num_blocks) * (*maxThreadsPerBlock);

	if (debug_mode == ON){
		printf("---Cuda Config. Report: Compute Capability = %d.%d \n", compute_cap_major, compute_cap_minor);
		printf("---Cuda Config. Report: Free Memory = %zu bytes\n", least_free);
		printf("---Cuda Config. Report: Free Memory = %zu bytes\n", least_free);
		printf("---Cuda Config. Report: Max thread burden = %zu bytes\n", thread_burden);
		printf("---Cuda Config. Report: Max block burden = %d bytes\n", block_burden);
		printf("---Cuda Config. Report: Batch size = %ld\n", *batch_size);
		printf("---Cuda Config. Report: Num blocks = %d / %d\n", *num_blocks, maxNumBlocks);
		/* printf("Global mem per device = %llu bytes\n", global_mem); */
		/* printf("Total appr. thread burden = %zu bytes\n", thread_burden); */
		/* printf("\tIf all devices are used, %d simulataneous operations\n\n", (*totalOps_perDevice) * (*device_count)); */
	}
  
}
