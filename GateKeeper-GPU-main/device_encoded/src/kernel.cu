/*
 * Zulal Bingol
 * GateKeeper main functions for device side
 * kernel.cu
 * v0
 * 16-LUT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <cuda_runtime_api.h>
#include <cuda.h>

extern "C"{
#include "../include/common.h"
#include "../include/kernel.cuh"
}


// MACROS
#define cudaErrorCheck(input) {cudaCheck((input), __FILE__, __LINE__);}
#define encode(x) (x == 'A' ? 0 : (x == 'C' ? 1 : (x == 'G' ? 2 : (x == 'T' ? 3 : UNDEFINED_CHAR))))

// GLOBALS
__device__ __constant__ unsigned int msb = 0x80000000; // for intermediate amending between unsigned int's
__device__ __constant__ unsigned int lsb = 0x00000001;
__device__ __constant__ unsigned int msb_sh_1 = 0x40000000, msb_sh_2 = 0x20000000;
__device__ __constant__ unsigned int lsb_sh_1 = 0x00000002, lsb_sh_2 = 0x00000004;
__device__ __constant__ unsigned int inter_lsb = 0x00000003, inter_msb = 0xc0000000; // for correcting with lsb : 0011 and msb : 1100
__device__ __constant__ int max_iter_amend = SIZE_OF_UINT - SHIFTER_WINDOW_SIZE + 1;
__device__ __constant__ int max_iter_AND = SIZE_OF_UINT / 4; 

__device__ __constant__ int d_window_count = WINDOW_COUNT;
__device__ __constant__ int d_amended_window_count = A_WINDOW_COUNT;
__device__ __constant__ int d_total_masks = TOTAL_MASKS;
__device__ __constant__ int d_error_threshold = ERROR_THRESHOLD;
__device__ __constant__ long int d_batch_size;

//  LUT
__device__ __constant__ uint8_t LUT[16] = 
{
	0x00, 0x01, 0x02, 0x03, 0x04, 0x07, 0x06, 0x07,
	0x08, 0x0f, 0x0a, 0x0f, 0x0c, 0x0f, 0x0e, 0x0f      
};

// Single GPU vars :
const int STREAM_COUNT = 3;
cudaStream_t streams[MAX_GPU][STREAM_COUNT];
char *d_reads, *d_refs;              


inline void cudaCheck(cudaError_t error, const char *file, int line)
{
	//cudaThreadSynchronize(); //affects performance
	if (error != cudaSuccess){
		fprintf(stderr, "CudaError : %s\n", cudaGetErrorString(error));
		fprintf(stderr, " @ %s: %d\n", file, line);
		fprintf(stderr, "\n");
		cudaDeviceReset();
		exit(EXIT_FAILURE);
	}
}

extern "C"
__host__ void setDevice(int currentDevice)
{
	cudaErrorCheck( cudaSetDevice(currentDevice) );
}

extern "C"
__device__ void complete_XOR(unsigned int *out_buffer, unsigned int *read_buffer,
							unsigned int *ref_buffer)
{
	int i;
	for (i = 0; i < d_window_count; i += 2){
		out_buffer[i] = 0; //after malloc, to be safe 
		out_buffer[i] = read_buffer[i] ^ ref_buffer[i];

		if (i != d_window_count-1){
		out_buffer[i+1] = 0; //after malloc, to be safe 
		out_buffer[i+1] = read_buffer[i+1] ^ ref_buffer[i+1];
		}
	}
}

extern "C"
__device__ int count_ones(unsigned int *buffer, int window_c)
{
	int i, num_ones = 0;
	for (i = 0; i < window_c; i++){
		num_ones += __popc(buffer[i]);
	}
	return num_ones;
}

/*
 * Right-shift on array of unsigned ints, with adjustment
 */
 extern "C"
 __device__ void complete_Rshift(unsigned int *buffer, int shift_amount, unsigned int *shifted)
 {
 	int i; 
 	unsigned int prev = 0, my_prev = 0;
 	int loop_shift_amount = (SIZE_OF_UINT - shift_amount);
 	unsigned int adjustor = (UINT_MAX >> loop_shift_amount);

 	for (i = 0; i < d_window_count; i++){
 		my_prev = prev;
 		prev = buffer[i] & adjustor;
 		shifted[i] = buffer[i] >> shift_amount;
 		if (i != 0 && my_prev != 0){
 			my_prev <<= loop_shift_amount;
 			shifted[i] = shifted[i] | my_prev;
 		}
 	}
 }

/*
 * Left-shift on array of unsigned ints, with adjustment
 */
 extern "C"
 __device__ void complete_Lshift(unsigned int *buffer, int shift_amount, unsigned int *shifted)
 {
 	int i; 
 	unsigned int prev = 0, my_prev = 0;
 	int loop_shift_amount = (SIZE_OF_UINT - shift_amount);
 	unsigned int adjustor = (UINT_MAX << loop_shift_amount);

 	for (i = d_window_count - 1; i >= 0; i--){
 		my_prev = prev;
 		prev = buffer[i] & adjustor;
 		shifted[i] = buffer[i] << shift_amount;
 		if ((i != d_window_count - 1) && my_prev != 0){
 			my_prev >>= loop_shift_amount;
 			shifted[i] = shifted[i] | my_prev;
 		}
 	}
 }

/*
 * Returns the number of errors in the final mask
 */
extern "C"
__device__ int finalize_AND(unsigned int amended_mask[TOTAL_MASKS][WINDOW_COUNT])
{
 	int e = 0, i;
 	int window_index;
 	unsigned int shifter_or = 0;
 	unsigned int final_window = 0, temp = 0;

	for (i = 0; i < d_amended_window_count; i++){

	// AND operation
 		final_window = amended_mask[0][i] & amended_mask[1][i]; 
 		for (window_index = 2; window_index < d_total_masks; window_index++){
 			final_window = final_window & amended_mask[window_index][i];
 			if (final_window == 0){
				 break;
			}	 
 		}

 		if (final_window != 0){

			// Difference calculation
			shifter_or = 0x0000000f;

			for (window_index = 0; window_index < max_iter_AND; window_index++){

				temp = final_window & shifter_or; // window is extracted (of size SHIFTER_WINDOW_SIZE)
				temp >>= (window_index * SHIFTER_WINDOW_SIZE);

				switch(temp){
				case 5 :  /* [0101] */
				case 6 :  /* [0110] */
				case 9 :  /* [1001] */
				case 10:  /* [1010] */
				case 11:  /* [1011] */
				case 13:  /* [1101] */
					e = e + 2;
					break;
					case 0 :
					break;
					default :
					e = e + 1;
					break;
				}
				shifter_or <<= SHIFTER_WINDOW_SIZE;
			}
		}
	}
	return e;
}

/*
* This function encodes the mask, then amends it. The amended version is recorded on amended_mask.  
*/
extern "C"
__device__ void encode_n_amend(unsigned int *mask)
{
	/* Secondary encoding on hamming mask 
	*  see original GateKeeper description
	*/
	int i, j, inner_index = 0;
	unsigned int temp1 = 0, temp2 = 0, t1, t2;
	unsigned int shifter_or, updated_result = 0;

	for (i = 0; i < d_window_count; i = i + 2){
		/* Since 2-bit will be represented by 1-bit, two consecutive elements (windows)
		   can be combined and be represented by single unsigned int */
			temp1 = mask[i];
		if (i != d_window_count-1){ //if WINDOW_COUNT is odd, mask[i+1] is NULL
			temp2 = mask[i + 1]; 
		}

		mask[inner_index] = 0;
		t1 = 1 << STR_PARTITION_SIZE;
		t2 = 1;
		j = 0;

		while(j < STR_PARTITION_SIZE){

		  // mask[i]
			if ((temp1  | (temp1 >> 1)) & 1){
				mask[inner_index] |= t1;
			}

		  // mask[i + 1] 
			if (i != d_window_count-1){
				if ((temp2  | (temp2 >> 1)) & 1){
					mask[inner_index] |= t2;
				}
				temp2 >>= 2;
				t2 <<= 1;
			}

			temp1 >>= 2;
			t1 <<= 1;
			j++;

		} //while

		/*
		* Amending procedure for one window
		*/
		if (mask[inner_index] != 0){
			temp1 = mask[inner_index]; // to be amended
			shifter_or = 0x0000000f;

			for (j = 0; j < max_iter_amend; j++){
				temp2 = temp1 & shifter_or; // window is extracted (of size SHIFTER_WINDOW_SIZE)
				temp2 >>= j;

				updated_result = (unsigned int)LUT[temp2];
				updated_result <<= j;

				temp1 = temp1 | updated_result;
				shifter_or <<= 1;
			}

			// Intermediate amending between unsigned int's : WELCOME TO HELL
			if (inner_index != 0){
				if ((mask[inner_index-1] & lsb_sh_2) && (temp1 & msb)){// case : .1.._1... (_ means between unsigned int's, . means 0 or 1)
					mask[inner_index-1] = mask[inner_index-1] | inter_lsb;
				}

				if ((mask[inner_index-1] & lsb_sh_1) && (temp1 & msb_sh_1)){ // case : ..1._.1.. (_ means between unsigned int's)
					temp1 = temp1 | msb;
					mask[inner_index-1] = mask[inner_index-1] | lsb;
				}

				if ((mask[inner_index-1] & lsb) && (temp1 & msb_sh_2)){  // case : ...1_..1. (_ means between unsigned int's)
					temp1 = temp1 | inter_msb;
				}
			}
			mask[inner_index] = temp1;
		}
		inner_index++;
	} // for : i
}


/* For windowed encoding */
extern "C"
__device__ char encode_2bit(char *str, size_t str_size, unsigned int *str_windows)
{
	if (str_size <= STR_PARTITION_SIZE){
		return UNDEFINED_STR;    
	}

	else {

		int i;
		int window_index = 0;
		unsigned int ch;
		int inner_len = 0;
		str_windows[0] = 0;

		for (i = 0; i < str_size; i++){

			ch = encode(str[i]);
			if (ch == UNDEFINED_CHAR){
				return UNDEFINED_STR;
			}

			str_windows[window_index] |= ch;
			inner_len++;

			if (inner_len == STR_PARTITION_SIZE) { // last char in window 
				window_index++;
				str_windows[window_index] = 0;
				inner_len = 0;
			}
			else { // if not the last char in current window
				if (i !=  str_size - 1){
					str_windows[window_index] <<= 2;
				}
				else { // incomplete window => str_size % STR_PARTITION_SIZE 
					str_windows[window_index] <<= 2 * (STR_PARTITION_SIZE - inner_len);
					break;
				}
			} 
		}
	return DEFINED_STR;
	}
}

extern "C"
__device__ void onlyEncode(unsigned int *mask){

	int inner_index = 0, i, j;
	unsigned int temp1 = 0, temp2 = 0, t1, t2;

	for (i = 0; i < WINDOW_COUNT; i = i + 2){

		/* Since 2-bit will be represented by 1-bit, two consecutive elements (windows)
		* can be combined and be represented by single unsigned int 
		*/
		temp1 = mask[i];
		if (i != WINDOW_COUNT-1){ //if WINDOW_COUNT is odd, mask[i+1] is NULL
			temp2 = mask[i + 1]; 
		}

		mask[inner_index] = 0;
		t1 = 1 << STR_PARTITION_SIZE;
		t2 = 1;
		j = 0;

		while(j < STR_PARTITION_SIZE){

			// mask[i]
			if ((temp1  | (temp1 >> 1)) & 1){
				mask[inner_index] |= t1;
			}

			// mask[i + 1] 
			if (i != WINDOW_COUNT-1){
				if ((temp2  | (temp2 >> 1)) & 1){
					mask[inner_index] |= t2;
				}
				temp2 >>= 2;
				t2 <<= 1;
			}
			temp1 >>= 2;
			t1 <<= 1;
			j++;
		} //while
		inner_index++;
	}
}

extern "C"
__device__ void onlyAmend(unsigned int *mask){

	unsigned int temp1, temp2, shifter_or, updated_result = 0;
	int inner_index, j;

	for (inner_index = 0; inner_index < A_WINDOW_COUNT; inner_index++){

		/*
		* Amending procedure for one window
		*/
		if (mask[inner_index] != 0){
			temp1 = mask[inner_index]; // to be amended
			shifter_or = 0x0000000f;

			for (j = 0; j < max_iter_amend; j++){
				temp2 = temp1 & shifter_or; // window is extracted (of size SHIFTER_WINDOW_SIZE)
				temp2 >>= j;

				updated_result = (unsigned int)LUT[temp2];
				updated_result <<= j;

				temp1 = temp1 | updated_result;
				shifter_or <<= 1;
			}

			// Intermediate amending between unsigned int's : WELCOME TO HELL
			if (inner_index != 0){
				if ((mask[inner_index-1] & lsb_sh_2) && (temp1 & msb)){// case : .1.._1... (_ means between unsigned int's, . means 0 or 1)
					mask[inner_index-1] = mask[inner_index-1] | inter_lsb;
				}

				if ((mask[inner_index-1] & lsb_sh_1) && (temp1 & msb_sh_1)){ // case : ..1._.1.. (_ means between unsigned int's)
					temp1 = temp1 | msb;
				mask[inner_index-1] = mask[inner_index-1] | lsb;
				}

				if ((mask[inner_index-1] & lsb) && (temp1 & msb_sh_2)){  // case : ...1_..1. (_ means between unsigned int's)
					temp1 = temp1 | inter_msb;
				}
			}
			mask[inner_index] = temp1;
		}
	}
}

/*
 * Single GPU execution with unified memory for multiple devices
 */
 __host__ void run_singleGPU_unified(long int batch_size, int numBlocks, int maxThreadPerBlock, RES_NODE *new_node,
 	int deviceIndex)
{  

	// Resetting Intermediate Arrays
	cudaErrorCheck( cudaMemcpyToSymbol(d_batch_size, &batch_size, sizeof(long int), 0, cudaMemcpyHostToDevice) );
	// Needs to be reset before each run because of remaining batch complications

	// Prefetching Data
 	if (compute_cap_major >= 6){
 		const unsigned int sum_size = sizeof(char) * READ_LENGTH * batch_size;
 		cudaMemAdvise(h_read_buffer[deviceIndex], sum_size, cudaMemAdviseSetPreferredLocation, deviceIndex);
 		cudaMemAdvise(h_ref_buffer[deviceIndex], sum_size, cudaMemAdviseSetPreferredLocation, deviceIndex);

 		cudaErrorCheck( cudaMemPrefetchAsync ( h_read_buffer[deviceIndex], sum_size, deviceIndex, streams[deviceIndex][0]) );
 		cudaErrorCheck( cudaMemPrefetchAsync ( h_ref_buffer[deviceIndex], sum_size, deviceIndex, streams[deviceIndex][1]) );
 	}

	// New Node filt
 	new_node -> next = NULL;
 	new_node -> filt_result = NULL;

	// new_node -> filt_result is a host array but is allocated in pinned format instead of pageable format
 	cudaErrorCheck( cudaMallocManaged((void**)&(new_node -> filt_result), sizeof(short) * batch_size) );
 	cudaErrorCheck( cudaMemset((void *)new_node -> filt_result, -1, batch_size * sizeof(short)) );

	// new_node -> edits
 	cudaErrorCheck( cudaMallocManaged((void**)&(new_node -> edits), sizeof(short) * batch_size) );

	// Kernel Call
 	if (kernel_analysis_mode == ON){
 		cudaEvent_t start, stop;
 		cudaEventCreate(&start);
 		cudaEventCreate(&stop);
 		float milliseconds = 0;

 		cudaEventRecord(start);
 		doGateKeeper <<< numBlocks, maxThreadPerBlock  >>> (h_read_buffer[deviceIndex],h_ref_buffer[deviceIndex], new_node -> filt_result, new_node -> edits);
 		cudaEventRecord(stop);

 		cudaEventSynchronize(stop);
 		cudaEventElapsedTime(&milliseconds, start, stop);
 		gpu_time[deviceIndex] += milliseconds;
 	}
 	else {
 		doGateKeeper <<< numBlocks, maxThreadPerBlock  >>> (h_read_buffer[deviceIndex],h_ref_buffer[deviceIndex], new_node -> filt_result, new_node -> edits);
 		cudaErrorCheck( cudaDeviceSynchronize() );
 	}

}


/*
 * Makes allocations on device for single GPU
 */
 __host__ void singleDeviceAlloc(long int batch_size, int deviceIndex)
 {
 	int i;
 	long long int sum_size = sizeof(char) * READ_LENGTH * batch_size;

  	// Stream creation
 	for (i = 0; i < STREAM_COUNT; i++){
 		cudaErrorCheck( cudaStreamCreate(&streams[deviceIndex][i]) );
 	}

	// Device and Host Memory Alloc for GPUs
	// unified memory alloc
  	cudaErrorCheck( cudaMallocManaged((void**)&h_read_buffer[deviceIndex], sum_size) );
  	cudaErrorCheck( cudaMallocManaged((void**)&h_ref_buffer[deviceIndex], sum_size) );
	cudaErrorCheck( cudaDeviceSynchronize() );
}

/*
 * Free's all device memory
 */
 __host__ void clearSingleDevice(RES_NODE *res_head, int deviceIndex)
 {
 	int i;
 	RES_NODE *current;

 	cudaErrorCheck( cudaDeviceSynchronize() );

	// Streams
 	for (i = 0; i < STREAM_COUNT; i++){
 		cudaErrorCheck( cudaStreamDestroy(streams[deviceIndex][i]) );
 	}

	// Results linked list
 	while (res_head != NULL)
 	{
 		current = res_head;
 		res_head = res_head -> next;
		if (current -> filt_result != NULL){
			cudaErrorCheck( cudaFree(current -> filt_result) );
		}
		if (current -> edits != NULL){
			cudaErrorCheck( cudaFree(current -> edits) );
		}
 		free(current);
 	}

  	cudaErrorCheck( cudaFree(h_read_buffer[deviceIndex]) );
  	cudaErrorCheck( cudaFree(h_ref_buffer[deviceIndex]) );
	cudaErrorCheck( cudaDeviceReset() );

	if (debug_mode == DEBUG){
		printf("---Device %d is cleaned.\n", deviceIndex);
	}
}

/*
* GateKeeper procedure for a single read and ref segment pair
*/
__global__ void doGateKeeper( char *reads, char *refs, short *d_filt_result, short *d_edits)
{

	int id = blockIdx.x * blockDim.x + threadIdx.x;
	char *read = &reads[id * READ_LENGTH];
	char *ref = &refs[id * READ_LENGTH];
	unsigned int read_buffer[WINDOW_COUNT];
	unsigned int ref_buffer[WINDOW_COUNT];
	char read_str_status = encode_2bit(read, READ_LENGTH, read_buffer);
	char ref_str_status = encode_2bit(ref, READ_LENGTH, ref_buffer);

	if (read_str_status == UNDEFINED_STR || ref_str_status == UNDEFINED_STR){ // undefined read or ref
		d_filt_result[id] = PASS;
		d_edits[id] = UNDEFINED_STR_EDIT;
	}
  
	else { // defined comparison 

		int i, e = 0;

		/* Memory allocation for masks, E : error threshold
		* masks[0] = Hamming Mask
		* masks[1...E] = Deletion Masks
		* masks[E+1...2E] = Insertion Masks
		*/
		unsigned int d_masks[TOTAL_MASKS][WINDOW_COUNT] = {0};
		unsigned int shifted_temp_R[WINDOW_COUNT] = {0};
		unsigned int shifted_temp_L[WINDOW_COUNT] = {0};

		// GATEKEEPER Operation
		complete_XOR(d_masks[0], read_buffer, ref_buffer); // Hamming Mask
		onlyEncode(d_masks[0]);
		e = count_ones(d_masks[0], A_WINDOW_COUNT);

		if (e <= d_error_threshold && d_filt_result[id] == -1){
			d_filt_result[id] = PASS;
			d_edits[id] = e;
		}
		else if (d_error_threshold == 0 && d_filt_result[id] == -1){
			d_filt_result[id] = REJECT;
			d_edits[id] = REJECT_EDIT;
		}
		else {
			onlyAmend(d_masks[0]);

			for (i = 1; i <= d_error_threshold; i++){

				if (i == 1){
					complete_Rshift(read_buffer, 2, shifted_temp_R);  /* since shift_amount is increased by one in each iteration, shifted_temp variable is iteratively
									shifted instead of incrementaly. */
					complete_Lshift(read_buffer, 2, shifted_temp_L);
				}
				else {
					complete_Rshift(shifted_temp_R, 2, shifted_temp_R); // 2 : since 2-bit encoded buffer is shifted
					complete_Lshift(shifted_temp_L, 2, shifted_temp_L);
				}

				// del
				complete_XOR(d_masks[i], shifted_temp_R, ref_buffer);
				encode_n_amend(d_masks[i]);
				d_masks[i][0] |= (msb >> (i-1)); // compensating for leading zeros
				
				// ins
				complete_XOR(d_masks[i + ERROR_THRESHOLD], shifted_temp_L, ref_buffer);
				encode_n_amend(d_masks[i + ERROR_THRESHOLD]);
				d_masks[i + ERROR_THRESHOLD][A_WINDOW_COUNT-1] |= (lsb << (i-1)); // compensating for trailing zeros
			}

			e = finalize_AND(d_masks);

			if (e <= d_error_threshold && d_filt_result[id] == -1){
				d_filt_result[id] = PASS;
				d_edits[id] = e;
			}
			else {
				d_filt_result[id] = REJECT;
				d_edits[id] = REJECT_EDIT;
			}

		} // else : num_ones <= d_error_threshold

	} // else : defined comparison

}

