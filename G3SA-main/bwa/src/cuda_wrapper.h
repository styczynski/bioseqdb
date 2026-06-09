#ifndef CUDASTREAMS_CUH
#define CUDASTREAMS_CUH
#include <stdint.h>
#include "hashKMerIndex.h"
#include "bwa.h"
#include "pipeline.h"

#ifdef __cplusplus
extern "C"{
#endif

    /* initialize a new instance of process_data_t 
        initialize and transfer constant memory on device:
            - user-defined options 
            - index 
            - memory management (no transfer)
        initialize pinned memory for reads on host
        initialize memory for reads on device
        initialize intermediate processing memory on device
        initialize a cuda stream for processing
     */
	process_data_t * device_alloc(
            int gpu_no,
            pipeline_aux_t *aux);

    void memcpy_index(
            process_data_t *instance,
            int gpuid, 
            pipeline_aux_t *aux
            );

	/* reset a process' data to prepare aligning a new batch
		reset memory management
		reset intermediate data
		reset total SAM size
	 */
	void resetProcess(process_data_t *process_data);

	/* reset transfer data to load in a new batch
		reset name, seq, comment, qual sizes
		reset n_seqs
	 */
	void resetTransfer(transfer_data_t *transfer_data);

	/* 
		transfer seqs from host into device
 	*/
	void CUDATransferSeqsIn(transfer_data_t *transfer_data);

	/* transfer SAM output from device to host
	*/
	void CUDATransferSamOut(transfer_data_t *transfer_data);


    // check if requested # of GPUs are available, exits with 1 if not.
    void check_device_count(int num_requested_gpus);

    // destruct cuda stream.
    void destruct_proc(process_data_t *proc);

    void cuda_wrapper_test();

/**
 * @brief convert current host addresses on a minibatch's transfer_data to their (future) addresses on GPU
 * assuming name, seq, comment, qual pointers on trasnfer_data still points to host memory
 *
 * @param seqs
 * @param batch_size
 * @param transfer_data transfer_data_t object where these reads reside
 */
void convert2DevAddr(transfer_data_t *transfer_data);

/**
 * @brief copy a minibatch of n_reads from superbatch to transfer_data minibatch's pinned memory, starting from firstReadId. 
 * Read info are contiguous, but name, comment, seq, qual are not
 * 
 * @param superbatch_data
 * @param transfer_data 
 * @param firstReadId 
 * @param n_reads 
 */
void copyReads2PinnedMem(superbatch_data_t *superbatch_data, transfer_data_t *transfer_data, int firstReadId, int n_reads);

// copies batch_size seqs from host (seq, seq_offset) to device memory (proc).
void memcpy_input(int batch_size, process_data_t *proc,
        uint8_t *seq, int *seq_offset);

void memcpy_output(
        aligned_chunk *ac,
        process_data_t * proc);


#ifdef __cplusplus
}
#endif

#endif
