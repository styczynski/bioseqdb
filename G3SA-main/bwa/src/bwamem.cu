#include "cuda_wrapper.h"
#include "gmem_alloc.h"

#include "errchk.cuh"
#include "utils_CUDA.cuh"
#include "macro.h"
#include "timer.h"
#include "myhelper.cuh"

#include "bwa.h"
#include "bwt_CUDA.cuh"
#include "bntseq.h"
#include "fastmap.h"

#include "hashKMerIndex.h"
#include "seed.cuh"
#include "aux.cuh"

#include <string.h>
#include <fstream>
#include <iostream>

#include <cub/cub.cuh>
#include <thrust/device_vector.h>
#include <thrust/sort.h>


#define PRINT(LABEL) \
	g3_opt->print_mask & BIT(LABEL)

#define CUDA_TIMER_INIT \
	cudaEvent_t timer_event_start, timer_event_stop;\
	CUDA_CHECK(cudaEventCreate(&timer_event_start));\
	CUDA_CHECK(cudaEventCreate(&timer_event_stop));

#define CUDA_TIMER_DESTROY \
	CUDA_CHECK(cudaEventDestroy(timer_event_start));\
	CUDA_CHECK(cudaEventDestroy(timer_event_stop));

#define CUDA_TIMER_START(lap) \
	lap = 0;\
	CUDA_CHECK(cudaEventRecord(timer_event_start, 0));

//CUDA_CHECK(cudaEventRecord(timer_event_start, 0));\

#define CUDA_TIMER_END(lap) \
	CUDA_CHECK(cudaEventRecord(timer_event_stop, 0));\
	CUDA_CHECK(cudaEventSynchronize(timer_event_stop));\
	CUDA_CHECK(cudaEventElapsedTime(&lap, timer_event_start, timer_event_stop));\

#define CUDA_CHECK_KERNEL_RUN()\
{\
	cudaError_t err;\
	err = cudaGetLastError();\
	if(err != cudaSuccess)\
	{\
		fprintf(stderr,"GPU %d cudaGetLastError(): %s %s %d\n", gpuid, cudaGetErrorString(err), __FILE__, __LINE__);\
	}\
	err = cudaDeviceSynchronize();\
	if(err != cudaSuccess)\
	{\
		fprintf(stderr,"GPU %d cudaDeviceSynchronize(): %s %s %d\n", gpuid, cudaGetErrorString(err), __FILE__, __LINE__);\
		/*CUDA_CHECK(cudaDeviceReset());\*/\
		/*while((err = cudaGetLastError()) != cudaSuccess) {;} \*/\
		return 1;\
	}\
}


extern float tprof[MAX_NUM_GPUS][MAX_NUM_STEPS];

/*
 *
 * Stage    |   Substage        |   Step          |   note
 * ---------------------------------------------------------------------------
 * Seeding  |   SMEM seeding    |   seed          |   seeding core
 *          |   Reseeding       |   r2            |
 *          |                   |   r3            |
 * Chaining |   B-tree chaining |   sal           |
 *          |                   |   sort_seeds    |
 *          |                   |   chain         |   chaining core
 *          |                   |   sort_chains   |
 *          |                   |   filter        |
 * Extending|   Local extending |   pairgen       |
 *          |                   |   extend        |   extending core
 *          |                   |   filter_mark   |
 *          |                   |   sort_alns     |
 *          |   Traceback       |   pairgen       |
 *          |                   |   traceback     |   traceback core (w/ NM test)
 *          |                   |   finalize      |
 */
int bwa_align(int gpuid, process_data_t *proc, g3_opt_t *g3_opt)
{
	float step_lap;
	void *d_temp_storage;
	size_t temp_storage_size;
	int batch_size, num_seeds_to_extend;
	int batch_num_alns;
	if((batch_size = proc->batch_size) == 0){
		return 0;
	}
	CUDA_TIMER_INIT;
	// (1/3) Seeding
	// SMEM seeding (Seeding 1/2)
	CUDA_TIMER_START(step_lap);
	preseedAndFilterV2 <<< batch_size, 320, 0, 0 >>> (
			proc->d_fmIndex,
			proc->d_opt, proc->d_seq, proc->d_seq_offset,
			proc->d_aux, proc->d_kmerHashTab, proc->d_buffer_pools);
	CUDA_CHECK_KERNEL_RUN();
	CUDA_TIMER_END(step_lap);
	tprof[gpuid][S_SMEM] += step_lap;
	if(PRINT(_SMEM) || PRINT(_ALL_SEEDING)){
		for(int readID = 0; readID < batch_size; readID++)
		{
			printIntv<<<1, WARPSIZE, 0, 0>>>(proc->d_aux, readID, _SMEM);
			CUDA_CHECK_KERNEL_RUN();
		}
	}
	// Reseeding (Seeding 2/2)
	CUDA_TIMER_START(step_lap);
	reseedV2 <<< ceil((float)batch_size / WARPSIZE) == 0 ? 1 
		: ceil((float)batch_size / WARPSIZE), WARPSIZE, 0, 0 >>>(
				proc->d_fmIndex, proc->d_opt, proc->d_seq, proc->d_seq_offset,
				proc->d_aux, proc->d_kmerHashTab, 
				proc->d_buffer_pools, batch_size);
	CUDA_CHECK_KERNEL_RUN();
	CUDA_TIMER_END(step_lap);
	tprof[gpuid][S_R2] += step_lap;
	// Reseeding 3rd Round
	CUDA_TIMER_START(step_lap);
	reseedLastRound <<< ceil((float)batch_size / WARPSIZE) == 0 ? 1 
		: ceil((float)batch_size / WARPSIZE), WARPSIZE, 0, 0 >>>(
				proc->d_fmIndex, proc->d_opt, proc->d_seq,
			       	proc->d_seq_offset,
				proc->d_aux, proc->d_kmerHashTab, batch_size);
	CUDA_CHECK_KERNEL_RUN();
	CUDA_TIMER_END(step_lap);
	tprof[gpuid][S_R3] += step_lap;
	if(PRINT(_INTV) || PRINT(_ALL_SEEDING)){
		for(int readID = 0; readID < batch_size; readID++)
		{
			printIntv<<<1, WARPSIZE, 0, 0>>>(proc->d_aux, readID, _INTV);
			CUDA_CHECK_KERNEL_RUN();
		}
	}
	// (2/3) Chaining
	// B-tree chaining (Chaining 1/1)
	CUDA_TIMER_START(step_lap);
	saLookup <<< batch_size, 128, 0, 0 >>> (
			proc->d_opt, proc->d_bwt, proc->d_bns,
		       	proc->d_seq, proc->d_seq_offset, proc->d_aux,
			proc->d_seq_seeds,
			proc->d_buffer_pools);
	CUDA_CHECK_KERNEL_RUN();
	CUDA_TIMER_END(step_lap);
	tprof[gpuid][C_SAL] += step_lap;
	if(PRINT(_SEED) || PRINT(_ALL_CHAINING)){
		for(int readID = 0; readID < batch_size; readID++)
		{
			printSeed<<<1, WARPSIZE, 0, 0>>>(proc->d_seq_seeds, readID, _SEED);
			CUDA_CHECK_KERNEL_RUN();
		}
	}
	CUDA_TIMER_START(step_lap);
	// sort translated seeds in rbeg order
	sortSeedsLowDim 
		<<< batch_size, SORTSEEDSLOW_BLOCKDIMX, 0, 0 >>> (
				proc->d_seq_seeds,
				proc->d_buffer_pools);
	CUDA_CHECK_KERNEL_RUN();
	sortSeedsHighDim 
		<<< batch_size, SORTSEEDSHIGH_BLOCKDIMX, 0, 0 >>> (
				proc->d_seq_seeds,
				proc->d_buffer_pools);
	CUDA_CHECK_KERNEL_RUN();
	CUDA_TIMER_END(step_lap);
	tprof[gpuid][C_SORT_SEEDS] += step_lap;
	if(PRINT(_STSEED) || PRINT(_ALL_CHAINING)){
		for(int readID = 0; readID < batch_size; readID++)
		{
			printSeed<<<1, WARPSIZE, 0, 0>>>(proc->d_seq_seeds, readID, _STSEED);
			CUDA_CHECK_KERNEL_RUN();
		}
	}
	CUDA_TIMER_START(step_lap);
#if 0
#define SEEDCHAINING_CHAIN_BLOCKDIMX 256
	SEEDCHAINING_chain_kernel <<< batch_size, SEEDCHAINING_CHAIN_BLOCKDIMX, 0, 0 >>> (
			proc->d_opt, proc->d_bns, proc->d_seq, proc->d_seq_offset, proc->d_seq_seeds,
			proc->d_chains,	// output
			proc->d_buffer_pools);
#else
	BTreeChaining 
		<<< ceil((float)batch_size / WARPSIZE) == 0 ? 1 : ceil((float)batch_size / WARPSIZE), WARPSIZE, 0, 0 >>>(
				batch_size,
				proc->d_opt, proc->d_bns,
			       	proc->d_seq, proc->d_seq_offset, proc->d_seq_seeds,
				proc->d_chains,	// output
				proc->d_buffer_pools);
#endif
	CUDA_CHECK_KERNEL_RUN();
	CUDA_TIMER_END(step_lap);
	tprof[gpuid][C_CHAIN] += step_lap;
	if(PRINT(_CHAIN) || PRINT(_ALL_CHAINING)){
		for(int readID = 0; readID < batch_size; readID++){
			printChain<<<1, WARPSIZE, 0, 0>>>(proc->d_chains, readID, _CHAIN);
			CUDA_CHECK_KERNEL_RUN();
		}
	}
	// sort before filtering
	CUDA_TIMER_START(step_lap);
	sortChainsDecreasingWeight 
		<<< batch_size, SORTCHAIN_BLOCKDIMX, 
		MAX_N_CHAIN*2*sizeof(uint16_t)+sizeof(mem_chain_t**), 0>>> 
			(proc->d_chains, proc->d_buffer_pools);
	CUDA_CHECK_KERNEL_RUN();
	CUDA_TIMER_END(step_lap);
	tprof[gpuid][C_SORT_CHAINS] += step_lap;
	if(PRINT(_STCHAIN) || PRINT(_ALL_CHAINING)){
		for(int readID = 0; readID < batch_size; readID++)
		{
			printChain<<<1, WARPSIZE, 0, 0>>>(proc->d_chains, readID, _STCHAIN);
			CUDA_CHECK_KERNEL_RUN();
		}
	}
	// filter chains
	CUDA_TIMER_START(step_lap);
	CHAINFILTERING_filter_kernel <<< batch_size, CHAIN_FLT_BLOCKSIZE, MAX_N_CHAIN*(3*sizeof(uint16_t)+sizeof(uint8_t)), 0 >>> (
			proc->d_opt, 
			proc->d_chains, 	// input and output
			proc->d_buffer_pools);
	CUDA_CHECK_KERNEL_RUN();
	CUDA_TIMER_END(step_lap);
	tprof[gpuid][C_FILTER] += step_lap;
	if(PRINT(_FTCHAIN) || PRINT(_ALL_CHAINING)){
		for(int readID = 0; readID < batch_size; readID++)
		{
			printChain<<<1, WARPSIZE, 0, 0>>>(proc->d_chains, readID, _FTCHAIN);
			CUDA_CHECK_KERNEL_RUN();
		}
	}
	// (3/3) Extending
	// Extending -> Local extending (1/2)
	CUDA_TIMER_START(step_lap);
	SWSeed <<< ceil((float)batch_size / WARPSIZE) == 0 ? 1 : ceil((float)batch_size / WARPSIZE), WARPSIZE, 0, 0 >>>(
			proc->d_chains, proc->d_regs,
		       	proc->d_seed_records, proc->d_Nseeds, batch_size,
			proc->d_buffer_pools
			);
	CUDA_CHECK_KERNEL_RUN();
	CUDA_CHECK(cudaMemcpy(&num_seeds_to_extend, proc->d_Nseeds, sizeof(int), cudaMemcpyDeviceToHost));

	if(num_seeds_to_extend==0){
		proc->n_processed += proc->batch_size;
		return 0;
	}
	// pair gen
	ExtendingPairGenerate <<< ceil((float)num_seeds_to_extend/32.0), 32, 0, 0 >>> (
			proc->d_opt, proc->d_bns, proc->d_pac, proc->d_seq, proc->d_seq_offset,
			proc->d_chains, proc->d_regs, proc->d_seed_records, proc->d_Nseeds,
			batch_size, proc->d_buffer_pools
			);
	CUDA_CHECK_KERNEL_RUN();
	CUDA_TIMER_END(step_lap);
	tprof[gpuid][E_PAIRGEN] += step_lap;
	if(PRINT(_DETAIL)){
		std::cerr << "GPU " << gpuid << "  " << "# local extending pairs: " << num_seeds_to_extend << std::endl;
	}
	if(PRINT(_EXPAIR) || PRINT(_ALL_EXTENDING)){
		printPair<<<1, WARPSIZE, 0, 0>>>(proc->d_seed_records, num_seeds_to_extend, _EXPAIR);
		CUDA_CHECK_KERNEL_RUN();
	}
	// local ext (Smith-Waterman)
	CUDA_TIMER_START(step_lap);
	localExtending <<< num_seeds_to_extend, WARPSIZE, 0, 0 >>> (
			proc->d_opt,
			proc->d_chains, 		// input chains
			proc->d_seed_records,
			proc->d_regs,		// output array
			proc->d_Nseeds);
	CUDA_CHECK_KERNEL_RUN();
	CUDA_TIMER_END(step_lap);
	tprof[gpuid][E_EXTEND] += step_lap;
	if(PRINT(_REGION) || PRINT(_ALL_EXTENDING)){
		for(int readID = 0; readID < batch_size; readID++)
		{
			printReg<<<1, WARPSIZE, 0, 0>>>(proc->d_regs, readID, _REGION);
			CUDA_CHECK_KERNEL_RUN();
		}
	}
	// filter regions (which are extension results)
	CUDA_TIMER_START(step_lap);
	filterRegions <<< batch_size, 320, 0, 0 >>> (
			proc->d_opt, proc->d_bns,
			proc->d_chains, proc->d_regs, proc->d_buffer_pools
			);
	CUDA_CHECK_KERNEL_RUN();
	CUDA_TIMER_END(step_lap);
	tprof[gpuid][E_FILTER_MARK] += step_lap;
	if(PRINT(_FTREGION) || PRINT(_ALL_EXTENDING)){
		for(int readID = 0; readID < batch_size; readID++)
		{
			printReg<<<1, WARPSIZE, 0, 0>>>(proc->d_regs, readID, _FTREGION);
			CUDA_CHECK_KERNEL_RUN();
		}
	}
	// sort regions before traceback
	CUDA_TIMER_START(step_lap);
	sortRegions <<< ceil((float)batch_size / WARPSIZE) == 0 ? 1 : ceil((float)batch_size / WARPSIZE), WARPSIZE, 0, 0 >>>(
			proc->d_regs, batch_size, proc->d_buffer_pools);
	CUDA_CHECK_KERNEL_RUN();
	CUDA_TIMER_END(step_lap);
	tprof[gpuid][E_SORT_ALNS] += step_lap;
	if(PRINT(_STREGION) || PRINT(_ALL_EXTENDING)){
		for(int readID = 0; readID < batch_size; readID++)
		{
			printReg<<<1, WARPSIZE, 0, 0>>>(proc->d_regs, readID, _STREGION);
			CUDA_CHECK_KERNEL_RUN();
		}
	}
	// TODO  cleanup these
	// compute offsets to the result alignments array.
	compute_offsets_on_host(proc, batch_size);

	// launch verifier
	// verify_scan_kernel<<<1,1>>>(proc->d_regs, proc->d_offsets, batch_size);
	// cudaDeviceSynchronize();

	// Extending -> Traceback (2/2)
	CUDA_TIMER_START(step_lap);
	CUDA_CHECK(cudaMemset(proc->d_Nseeds, 0, sizeof(int)));
	// preproc
	FINALIZEALN_preprocessing1_kernel <<< ceil((float)batch_size / WARPSIZE) == 0 ? 1 : ceil((float)batch_size / WARPSIZE), WARPSIZE, 0, 0 >>>(
			batch_size,
			proc->d_regs, proc->d_alns, proc->d_seed_records, proc->d_Nseeds,
			proc->d_buffer_pools);
	CUDA_CHECK_KERNEL_RUN();

	CUDA_CHECK(cudaMemcpy(&batch_num_alns, proc->d_Nseeds, sizeof(int), cudaMemcpyDeviceToHost));
	if(batch_num_alns==0){
		proc->n_processed += proc->batch_size;
		return 0;
	}
	// preproc
	FINALIZEALN_preprocessing2_kernel <<< ceil((float)batch_num_alns / WARPSIZE) == 0 ? 1 : ceil((float)batch_num_alns / WARPSIZE), WARPSIZE, 0, 0 >>>(
			proc->d_opt, proc->d_seq, proc->d_seq_offset,
			proc->d_pac, proc->d_bns,
			proc->d_regs, proc->d_alns, proc->d_seed_records, batch_num_alns,
			proc->d_sortkeys_in,	// sortkeys_in = bandwidth * rlen
			proc->d_seqIDs_in,
			proc->d_buffer_pools);
	CUDA_CHECK_KERNEL_RUN();
	// preproc: reverse query and target if aln position is on reverse strand
	FINALIZEALN_reverseSeq_kernel <<< batch_num_alns, WARPSIZE, 0, 0 >>> (proc->d_seed_records, proc->d_alns, proc->d_buffer_pools);
	CUDA_CHECK_KERNEL_RUN();
	// preproc: sort
	// determine temporary storage requirement
	d_temp_storage = NULL;
	temp_storage_size = 0;
	CUDA_CHECK(cub::DeviceRadixSort::SortPairs(d_temp_storage, temp_storage_size, proc->d_sortkeys_in, proc->d_sortkeys_out, proc->d_seqIDs_in, proc->d_seqIDs_out, batch_num_alns, 0, 8*sizeof(int), 0));
	// Allocate temporary storage
	CUDA_CHECK(cudaMalloc(&d_temp_storage, temp_storage_size));
	// perform radix sort
	CUDA_CHECK(cub::DeviceRadixSort::SortPairsDescending(d_temp_storage, temp_storage_size, proc->d_sortkeys_in, proc->d_sortkeys_out, proc->d_seqIDs_in, proc->d_seqIDs_out, batch_num_alns, 0, 8*sizeof(int), 0));
	cudaFree(d_temp_storage);
	CUDA_TIMER_END(step_lap);
	tprof[gpuid][E_T_PAIRGEN] += step_lap;
	if(PRINT(_TBPAIR) || PRINT(_ALL_EXTENDING)){
		printPair<<<1, WARPSIZE, 0, 0>>>(proc->d_seed_records, batch_num_alns, _TBPAIR);
		CUDA_CHECK_KERNEL_RUN();
	}

	if(PRINT(_DETAIL)){
		std::cerr << "GPU " << gpuid << "  " << "# traceback pairs: " << batch_num_alns << std::endl;
	}
	// traceback (with Needlman-Wunsch test)
	CUDA_TIMER_START(step_lap);
	traceback<<< ceil((float)batch_num_alns / WARPSIZE) == 0 ? 1 : ceil((float)batch_num_alns / WARPSIZE), WARPSIZE, 0, 0 >>>(
			proc->d_opt,
			proc->d_seed_records, batch_num_alns, proc->d_alns, proc->d_seqIDs_out,
			proc->d_buffer_pools
			);
	CUDA_CHECK_KERNEL_RUN();
	CUDA_TIMER_END(step_lap);
	tprof[gpuid][E_TRACEBACK] += step_lap;
	if(PRINT(_ALIGNMENT) || PRINT(_ALL_EXTENDING)){
		for(int readID = 0; readID < batch_size; readID++)
		{
			printAln<<<1, WARPSIZE, 0, 0>>>(proc->d_bns, proc->d_alns, readID, _ALIGNMENT);
			CUDA_CHECK_KERNEL_RUN();
		}
	}
	// pack to memcpy to host
	CUDA_TIMER_START(step_lap);
	finalize<<< ceil((float)batch_num_alns / WARPSIZE) == 0 ? 1 : ceil((float)batch_num_alns / WARPSIZE), WARPSIZE, 0, 0 >>>(
			proc->d_opt, proc->d_bns, proc->d_seq, proc->d_seq_offset,
			proc->d_regs, proc->d_alns, proc->d_seed_records, batch_num_alns,
			proc->d_buffer_pools,
			proc->d_offsets,
			proc->d_rids,
			proc->d_positions,
			proc->d_ncigars,
			proc->d_cigars
			);
	CUDA_CHECK_KERNEL_RUN();
	CUDA_TIMER_END(step_lap);
	tprof[gpuid][E_FINALIZE] += step_lap;
	if(PRINT(_RESULT) || PRINT(_ALL_EXTENDING)){
		for(int readID = 0; readID < batch_size; readID++)
		{
			printAln<<<1, WARPSIZE, 0, 0>>>(proc->d_bns, proc->d_alns, readID, _RESULT);
			CUDA_CHECK_KERNEL_RUN();
		}
	}
	// TODO cleanup these
	// verify if results are correctly written into the arrays
	// verify_writing_kernel<<<1,1>>>(proc->d_offsets, batch_size,
	// proc->d_rids, proc->d_positions, proc->d_ncigars, proc->d_cigars);
	// cudaDeviceSynchronize();

	//printBufferInfoHost(proc->d_buffer_pools);
	CUDA_TIMER_DESTROY;
	return 0;
}
