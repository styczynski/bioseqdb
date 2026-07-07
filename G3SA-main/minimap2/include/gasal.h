#ifndef __GASAL_H__
#define __GASAL_H__


#include <stdlib.h>
#include <stdint.h>

#include "/usr/local/cuda-12.1/targets/x86_64-linux/include/cuda_runtime.h"

#ifndef HOST_MALLOC_SAFETY_FACTOR
#define HOST_MALLOC_SAFETY_FACTOR 5
#endif

#define CHECKCUDAERROR(error) \
		do{\
			err = error;\
			if (cudaSuccess != err ) { \
				fprintf(stderr, "[GASAL CUDA ERROR:] %s(CUDA error no.=%d). Line no. %d in file %s\n", cudaGetErrorString(err), err,  __LINE__, __FILE__); \
				exit(EXIT_FAILURE);\
			}\
		}while(0)\


inline int CudaCheckKernelLaunch()
{
	cudaError err = cudaGetLastError();
	if ( cudaSuccess != err )
	{
		return -1;
	}

	return 0;
}


enum comp_start{
	WITHOUT_START,
	WITH_START,
	WITH_TB
};

// Generic enum for ture/false. Using this instead of bool to generalize templates out of Int values for secondBest. 
// Can be usd more generically, for example for WITH_/WITHOUT_START.
enum Bool{
	FALSE,
	TRUE
};

enum data_source{
	NONE,
	QUERY,
	TARGET,
	BOTH
};

enum algo_type{
	UNKNOWN,
	GLOBAL,
	SEMI_GLOBAL,
	LOCAL,
	MICROLOCAL,
	BANDED,
	KSW
};

enum operation_on_seq{
	FORWARD_NATURAL,
	REVERSE_NATURAL,
	FORWARD_COMPLEMENT,
	REVERSE_COMPLEMENT,
};

// data structure of linked list to allow extension of memory on host side.
struct host_batch{
	uint8_t *data;
	uint32_t page_size;
	uint32_t data_size;
	uint32_t offset;
	int is_locked;
	struct host_batch* next;
};
typedef struct host_batch host_batch_t;

// Data structure to hold results. Can be instantiated for host or device memory (see res.cpp)
struct gasal_res{
	int32_t *aln_score;
	int32_t *query_batch_end;
	int32_t *target_batch_end;
	int32_t *query_batch_start;
	int32_t *target_batch_start;
	uint8_t *cigar;
	uint32_t *n_cigar_ops;
};
typedef struct gasal_res gasal_res_t;

//stream data
typedef struct {
	uint8_t *unpacked_query_batch;
	uint8_t *unpacked_target_batch;
	uint32_t *packed_query_batch;
	uint32_t *packed_target_batch;
	uint32_t *query_batch_offsets;
	uint32_t *target_batch_offsets;
	uint32_t *query_batch_lens;
	uint32_t *target_batch_lens;

	uint32_t *host_seed_scores;
	uint32_t *seed_scores;
	
	host_batch_t *extensible_host_unpacked_query_batch;
	host_batch_t *extensible_host_unpacked_target_batch;

	uint8_t *host_query_op;
	uint8_t *host_target_op;
	uint8_t *query_op;
	uint8_t *target_op;

	uint32_t *host_query_batch_offsets;
	uint32_t *host_target_batch_offsets;
	uint32_t *host_query_batch_lens;
	uint32_t *host_target_batch_lens;

	gasal_res_t *host_res; // the results that can be read on host - THE STRUCT IS ON HOST SIDE, ITS CONTENT IS ON HOST SIDE.
	gasal_res_t *device_cpy; // a struct that contains the pointers to the device side - THE STRUCT IS ON HOST SIDE, but the CONTENT is malloc'd on and points to the DEVICE SIDE
	gasal_res_t *device_res; // the results that are written on device - THE STRUCT IS ON DEVICE SIDE, ITS CONTENT POINTS TO THE DEVICE SIDE.

	gasal_res_t *host_res_second; 
	gasal_res_t *device_res_second; 
	gasal_res_t *device_cpy_second;

	uint32_t gpu_max_query_batch_bytes;
	uint32_t gpu_max_target_batch_bytes;

	uint32_t host_max_query_batch_bytes;
	uint32_t host_max_target_batch_bytes;
	
	uint32_t gpu_max_n_alns;
	uint32_t host_max_n_alns;
	uint32_t current_n_alns;

	uint64_t packed_tb_matrix_size;
	uint4 *packed_tb_matrices;

	//for AGAThA
	int32_t slice_width;
	uint32_t maximum_sequence_length;
	short2 *global_buffer;
	short2 *host_buffer;


	cudaStream_t str;
	int is_free;
	int id; //this can be useful in cases where a gasal_gpu_storage only contains PARTS of an alignment (like a seed-extension...), to gather results.

} gasal_gpu_storage_t;

//vector of streams
typedef struct {
	int n;
	gasal_gpu_storage_t *a;
}gasal_gpu_storage_v;


//match/mismatch and gap penalties
typedef struct{
	int32_t match;
	int32_t mismatch;
	int32_t gap_open;
	int32_t gap_extend;
	int32_t slice_width;
	int32_t z_threshold;
	int32_t band_width;
} gasal_subst_scores;


#endif
