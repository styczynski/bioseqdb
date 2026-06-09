#include "gasal.h"
#include "args_parser.h"
#include "interfaces.h"
#include "res.h"


// Function for general resizing
template <typename T>
T* cudaHostRealloc(void *source, int new_size, int old_size) 
{
	cudaError_t err;
	T* destination = NULL;
	if (new_size < old_size)
	{
		fprintf(stderr, "[GASAL ERROR] cudoHostRealloc: invalid sizes. New size < old size (%d < %d)", new_size, old_size);
		exit(EXIT_FAILURE);
	}
	CHECKCUDAERROR(cudaHostAlloc(&destination, new_size * sizeof(T), cudaHostAllocMapped));
	//fprintf(stderr, "\ndest=%p\tsrc=%p", destination, source);
	CHECKCUDAERROR(cudaMemcpy(destination, source, old_size * sizeof(T), cudaMemcpyHostToHost));
	CHECKCUDAERROR(cudaFreeHost(source));
	return destination;
};

// Realloc new fields when more alignments are added. 
void gasal_host_alns_resize(gasal_gpu_storage_t *gpu_storage, int new_max_alns, Parameters *params)
{
	/*  // Don't reallocate the extensible batches. They're extensible.
		gpu_storage->extensible_host_unpacked_query_batch = gasal_host_batch_new(host_max_query_batch_bytes, 0);
		gpu_storage->extensible_host_unpacked_target_batch = gasal_host_batch_new(host_max_target_batch_bytes, 0);
	*/
	/*  // don't realloc gpu-sided batches as they will be taken care of before aligning.
		CHECKCUDAERROR(cudaMalloc(&(gpu_storage->unpacked_query_batch), gpu_max_query_batch_bytes * sizeof(uint8_t)));
		CHECKCUDAERROR(cudaMalloc(&(gpu_storage->unpacked_target_batch), gpu_max_target_batch_bytes * sizeof(uint8_t)));
	*/

	fprintf(stderr, "[GASAL WARNING] Resizing gpu_storage from %d sequences to %d sequences... ", gpu_storage->host_max_n_alns,new_max_alns);
	// don't care about realloc'ing gpu-sided fields as they will be taken care of before aligning.

	gpu_storage->host_query_op =  cudaHostRealloc<uint8_t>((void*) gpu_storage->host_query_op, new_max_alns, gpu_storage->host_max_n_alns);
	gpu_storage->host_target_op =  cudaHostRealloc<uint8_t>((void*) gpu_storage->host_target_op, new_max_alns, gpu_storage->host_max_n_alns);
	
	gpu_storage->host_query_batch_lens = cudaHostRealloc<uint32_t>((void*) gpu_storage->host_query_batch_lens, new_max_alns, gpu_storage->host_max_n_alns);
	gpu_storage->host_target_batch_lens = cudaHostRealloc<uint32_t>((void*) gpu_storage->host_target_batch_lens, new_max_alns, gpu_storage->host_max_n_alns);
	//fprintf(stderr, "_lens done ");

	gpu_storage->host_query_batch_offsets = cudaHostRealloc<uint32_t>((void*) gpu_storage->host_query_batch_offsets, new_max_alns, gpu_storage->host_max_n_alns);
	gpu_storage->host_target_batch_offsets = cudaHostRealloc<uint32_t>((void*) gpu_storage->host_target_batch_offsets, new_max_alns, gpu_storage->host_max_n_alns);
	//fprintf(stderr, "_offsets done ");
	
	gasal_res_destroy_host(gpu_storage->host_res);
	gpu_storage->host_res = gasal_res_new_host(new_max_alns, params);
	gpu_storage->device_cpy = gasal_res_new_device_cpy(new_max_alns, params);
	gpu_storage->device_res = gasal_res_new_device(gpu_storage->device_cpy);

	gpu_storage->host_res_second = NULL;
	gpu_storage->device_cpy_second = NULL;
	gpu_storage->device_res_second = NULL;
	
	
	//fprintf(stderr, "_res done ");

	gpu_storage->host_max_n_alns = new_max_alns;
	//gpu_storage->gpu_max_n_alns = gpu_max_n_alns;
	fprintf(stderr, " done. This can harm performance.\n");
}

// operation (Reverse/complement) filler.
void gasal_op_fill(gasal_gpu_storage_t *gpu_storage_t, uint8_t *data, uint32_t nbr_seqs_in_stream, data_source SRC)
{
	uint8_t *host_op = NULL;
	switch(SRC)
	{
		case QUERY:
			host_op = (gpu_storage_t->host_query_op);
		break;
		case TARGET:
			host_op = (gpu_storage_t->host_target_op);
		break;
		default:
		break;
	}
	memcpy(host_op, data, nbr_seqs_in_stream);
}

void gasal_set_device(int gpu_select, bool isPrintingProp)
{
	/* 
	Select GPU
	*/
	if (isPrintingProp)
	{
		int num_devices, device;
		cudaGetDeviceCount(&num_devices);
		fprintf(stderr, "Found %d GPUs\n", num_devices);
		if (gpu_select  > num_devices-1)
		{
			fprintf(stderr, "Error: can't select device %d when only %d devices are selected (range from 0 to %d)\n", gpu_select, num_devices, num_devices-1);
			exit(EXIT_FAILURE);
		}
		if (num_devices > 0) {
			cudaDeviceProp properties;
			for (device = 0; device < num_devices; device++) {
					cudaGetDeviceProperties(&properties, device);
					fprintf(stderr, "\tGPU %d: %s\n", device, properties.name);
			}
			cudaGetDeviceProperties(&properties, gpu_select);
			fprintf(stderr, "Selected device %d : %s\n", gpu_select, properties.name);
			cudaSetDevice(gpu_select);
		}
	} else {
		// silently select device
		cudaSetDevice(gpu_select);
	}

}
