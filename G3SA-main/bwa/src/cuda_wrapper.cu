#include "bwa.h"
#include "cuda_wrapper.h"
#include "gmem_alloc.h"
#include "macro.h"
#include "errchk.cuh"
#include "timer.h"
#include <iostream>


/* transfer index data */
static void transferIndex(
	const bwt_t *bwt, 
	const bntseq_t *bns, 
	const uint8_t *pac,
	const kmers_bucket_t *kmerHashTab,
	process_data_t *process_instance,
    unsigned long long *allocated_size)
{
		/* CUDA GLOBAL MEMORY ALLOCATION AND TRANSFER */

	// Burrows-Wheeler Transform
		// 1. bwt_t structure
	//if (bwa_verbose>=3) fprintf(stderr, "[M::%-25s] *** bwt .......... %.2f MB\n", __func__, (float)sizeof(bwt_t)/MB_SIZE);
	bwt_t* d_bwt;
	cudaMalloc((void**)&d_bwt, sizeof(bwt_t));
	cudaMemcpy(d_bwt, bwt, sizeof(bwt_t), cudaMemcpyHostToDevice);
		// 2. int array of bwt
	//if (bwa_verbose>=3) fprintf(stderr, "[M::%-25s] *** bwt_int ...... %.2f MB\n", __func__, (float)bwt->bwt_size*sizeof(uint32_t)/MB_SIZE);
	uint32_t* d_bwt_int ;
	cudaMalloc((void**)&d_bwt_int, bwt->bwt_size*sizeof(uint32_t));
	cudaMemcpy(d_bwt_int, bwt->bwt, bwt->bwt_size*sizeof(uint32_t), cudaMemcpyHostToDevice);
		// 3. int array of Suffix Array
	//if (bwa_verbose>=3) fprintf(stderr, "[M::%-25s] *** suffix array . %.2f MB \n", __func__, (float)bwt->n_sa*sizeof(bwtint_t)/MB_SIZE);
	bwtint_t* d_bwt_sa ;
	cudaMalloc((void**)&d_bwt_sa, bwt->n_sa*sizeof(bwtint_t));
	cudaMemcpy(d_bwt_sa, bwt->sa, bwt->n_sa*sizeof(bwtint_t), cudaMemcpyHostToDevice);
		// set pointers on device's memory to bwt_int and SA
	cudaMemcpy((void**)&(d_bwt->bwt), &d_bwt_int, sizeof(uint32_t*), cudaMemcpyHostToDevice);
	cudaMemcpy((void**)&(d_bwt->sa), &d_bwt_sa, sizeof(bwtint_t*), cudaMemcpyHostToDevice);


	unsigned long long total_size = sizeof(bwt_t) +\
                                    bwt->bwt_size*sizeof(uint32_t) +\
                                    bwt->n_sa*sizeof(bwtint_t) +\
                                    bns->n_seqs*sizeof(bntann1_t) +\
                                    bns->n_holes*sizeof(bntamb1_t) +\
                                    bns->l_pac*sizeof(uint8_t);
	//fprintf(stderr, "[M::%-25s] Device memory for Index ...... %.2f MB \n", __func__, (float)total_size/MB_SIZE);

	// BNS
	// First create h_bns as a copy of bns on host
	// Then allocate its member pointers on device and copy data over
	// Then copy h_bns to d_bns
	uint32_t i, size;			// loop index and length of strings
	bntseq_t* h_bns;			// host copy to modify pointers
	h_bns = (bntseq_t*)malloc(sizeof(bntseq_t));
	memcpy(h_bns, bns, sizeof(bntseq_t));
	h_bns->anns = (bntann1_t*)malloc(bns->n_seqs*sizeof(bntann1_t));
	memcpy(h_bns->ambs, bns->ambs, bns->n_holes*sizeof(bntamb1_t));
	h_bns->ambs = (bntamb1_t*)malloc(bns->n_holes*sizeof(bntamb1_t));
	memcpy(h_bns->anns, bns->anns, bns->n_seqs*sizeof(bntann1_t));

		// allocate anns.name
	for (i=0; i<bns->n_seqs; i++){
		size = strlen(bns->anns[i].name);
		// allocate this name and copy to device
		cudaMalloc((void**)&(h_bns->anns[i].name), size+1); 			// +1 for "\0"
		cudaMemcpy(h_bns->anns[i].name, bns->anns[i].name, size+1, cudaMemcpyHostToDevice);
	}
	// allocate anns.anno
	for (i=0; i<bns->n_seqs; i++){
		size = strlen(bns->anns[i].anno);
		// allocate this name and copy to device
		cudaMalloc((void**)&(h_bns->anns[i].anno), size+1); 			// +1 for "\0"
		cudaMemcpy(h_bns->anns[i].anno, bns->anns[i].anno, size+1, cudaMemcpyHostToDevice);
	}
		// now h_bns->anns has pointers of name and anno on device
		// allocate anns on device and copy data from h_bns->anns to device
	bntann1_t* temp_d_anns;
	//if (bwa_verbose>=3) fprintf(stderr, "[M::%-25s] *** bns.anns ..... %.2f MB\n", __func__, (float)bns->n_seqs*sizeof(bntann1_t)/MB_SIZE);
	cudaMalloc((void**)&temp_d_anns, bns->n_seqs*sizeof(bntann1_t));
	cudaMemcpy(temp_d_anns, h_bns->anns, bns->n_seqs*sizeof(bntann1_t), cudaMemcpyHostToDevice);
		// now assign this pointer to h_bns->anns
	h_bns->anns = temp_d_anns;

		// allocate bns->ambs on device and copy data to device
	//if (bwa_verbose>=3) fprintf(stderr, "[M::%-25s] *** bns.ambs ..... %.2f MB\n", __func__, (float)bns->n_holes*sizeof(bntamb1_t)/MB_SIZE);
	cudaMalloc((void**)&h_bns->ambs, bns->n_holes*sizeof(bntamb1_t));
	cudaMemcpy(h_bns->ambs, bns->ambs, bns->n_holes*sizeof(bntamb1_t), cudaMemcpyHostToDevice);

		// finally allocate d_bns and copy from h_bns
	//if (bwa_verbose>=3) fprintf(stderr, "[M::%-25s] *** bns .......... %.2f MB\n", __func__, (float)sizeof(bntseq_t)/MB_SIZE);
	bntseq_t* d_bns;
	cudaMalloc((void**)&d_bns, sizeof(bntseq_t));
	cudaMemcpy(d_bns, h_bns, sizeof(bntseq_t), cudaMemcpyHostToDevice);

	// PAC
	//if (bwa_verbose>=3) fprintf(stderr, "[M::%-25s] *** pac .......... %.2f MB\n", __func__, (float)bns->l_pac*sizeof(uint8_t)/MB_SIZE);
	uint8_t* d_pac ;
	cudaMalloc((void**)&d_pac, bns->l_pac/4*sizeof(uint8_t)); 		// l_pac is length of ref seq
	cudaMemcpy(d_pac, pac, bns->l_pac/4*sizeof(uint8_t), cudaMemcpyHostToDevice); 		// divide by 4 because 2-bit encoding

	// K-MER HASH TABLE
	//if (bwa_verbose>=3) fprintf(stderr, "[M::%-25s] *** kmer ......... %.2f MB\n", __func__, (float)pow4(KMER_K)*sizeof(kmers_bucket_t)/MB_SIZE);
	kmers_bucket_t* d_kmerHashTab ;
	cudaMalloc((void**)&d_kmerHashTab, pow4(KMER_K)*sizeof(kmers_bucket_t)); 		// l_pac is length of ref seq
	cudaMemcpy(d_kmerHashTab, kmerHashTab, pow4(KMER_K)*sizeof(kmers_bucket_t), cudaMemcpyHostToDevice); 		// divide by 4 because 2-bit encoding


	// output
	process_instance->d_bwt = d_bwt;
	process_instance->d_bns = d_bns;
	process_instance->d_pac = d_pac;
	process_instance->d_kmerHashTab = d_kmerHashTab;
    std::cerr << "* bwt index " << total_size / MB_SIZE << " MB\n";
}

/* transfer user-defined optinos */
static void transferOptions(
	const mem_opt_t *opt, 
	mem_pestat_t *pes0,
	process_data_t *process_instance,
    unsigned long long *allocated_size)
{
	// matching and mapping options (opt)
	mem_opt_t* d_opt;
	cudaMalloc((void**)&d_opt, sizeof(mem_opt_t));
	cudaMemcpy(d_opt, opt, sizeof(mem_opt_t), cudaMemcpyHostToDevice);

	// paired-end stats: only allocate on device
	mem_pestat_t* d_pes;
	if (opt->flag&MEM_F_PE){
		//fprintf(stderr, "[M::%-25s] pestat ....... %.2f MB\n", __func__, (float)4*sizeof(mem_pestat_t)/MB_SIZE);
		cudaMalloc((void**)&d_pes, 4*sizeof(mem_pestat_t));
	}

	// output
	process_instance->d_opt = d_opt;
	process_instance->d_pes = d_pes;
	process_instance->h_pes0 = pes0;
}

/* transfer index data */
static void transferFmIndex(
        process_data_t *process_instance,
        const fmIndex *idx,
    unsigned long long *allocated_size)
{
    /**
     * Reference data to transfer:
     *      count, count2
     *      cpOcc, cpOcc2
     *      oneHot, sentinelIndex, firstBase
     */
    fmIndex hostFmIndex;
    cudaError_t err;

    long long size = 0;

    // FOR BWT-2
    uint64_t *d_one_hot;
    int sizeOneHot = 64 * sizeof(uint64_t);
    size += sizeOneHot;
    CUDA_CHECK(cudaMalloc((void**)&d_one_hot, sizeOneHot));
    CUDA_CHECK(cudaMemcpy(d_one_hot, idx->oneHot, sizeOneHot, cudaMemcpyHostToDevice));

    CP_OCC *d_cp_occ;
    int64_t cp_occ_size = idx->cpOccSize;
    size += cp_occ_size*sizeof(CP_OCC);
    CUDA_CHECK(cudaMalloc((void**)&d_cp_occ, cp_occ_size*sizeof(CP_OCC)));
    CUDA_CHECK(cudaMemcpy(d_cp_occ, idx->cpOcc, cp_occ_size*sizeof(CP_OCC), cudaMemcpyHostToDevice));

    int64_t *d_count;
    int sizeCount = sizeof(int64_t) * 5;
    size += sizeCount;
    CUDA_CHECK(cudaMalloc((void**)&d_count, sizeCount));
    CUDA_CHECK(cudaMemcpy(d_count, idx->count, sizeCount, cudaMemcpyHostToDevice));


    CP_OCC2 *d_cp_occ2;
    size += cp_occ_size*sizeof(CP_OCC2);
    CUDA_CHECK(cudaMalloc((void**)&d_cp_occ2, cp_occ_size*sizeof(CP_OCC2)));
    CUDA_CHECK(cudaMemcpy(d_cp_occ2, idx->cpOcc2, cp_occ_size*sizeof(CP_OCC2), cudaMemcpyHostToDevice));

    int64_t *d_count2;
    int sizeCount2 = sizeof(int64_t) * 17;
    size += sizeCount2;
    CUDA_CHECK(cudaMalloc((void**)&d_count2, sizeCount2));
    CUDA_CHECK(cudaMemcpy(d_count2, idx->count2, sizeCount2, cudaMemcpyHostToDevice));

    uint8_t *d_first_base;
    CUDA_CHECK(cudaMalloc((void**)&d_first_base, sizeof(uint8_t)));
    CUDA_CHECK(cudaMemcpy(d_first_base, idx->firstBase, sizeof(uint8_t), cudaMemcpyHostToDevice));

    int64_t *deviceSentinelIndex;
    CUDA_CHECK(cudaMalloc((void**)&deviceSentinelIndex, sizeof(int64_t)));
    CUDA_CHECK(cudaMemcpy(deviceSentinelIndex, idx->sentinelIndex, sizeof(int64_t), cudaMemcpyHostToDevice));

    hostFmIndex.oneHot = d_one_hot;
    hostFmIndex.cpOcc = d_cp_occ;
    hostFmIndex.cpOcc2 = d_cp_occ2;
    hostFmIndex.count = d_count;
    hostFmIndex.count2 = d_count2;
    hostFmIndex.firstBase = d_first_base;
    hostFmIndex.sentinelIndex = deviceSentinelIndex;

    fmIndex *deviceFmIndex;
    CUDA_CHECK(cudaMalloc((void**)&deviceFmIndex, sizeof(fmIndex)));
    CUDA_CHECK(cudaMemcpy(deviceFmIndex, &hostFmIndex, sizeof(fmIndex), cudaMemcpyHostToDevice));

    std::cerr << "* occ2 index: " << size / MB_SIZE << " MB\n";

    // output
    process_instance->d_fmIndex = deviceFmIndex;
}




process_data_t * device_alloc(
        int gpuid,
        pipeline_aux_t *aux
        )
{
    CUDA_CHECK(cudaSetDevice(gpuid));
    int current;
    CUDA_CHECK(cudaGetDevice(&current));
    if(current != gpuid){
        exit(1);
    }
    process_data_t *proc = new process_data_t;
    proc->gpu_no = gpuid;

    unsigned long long size;

	// dynamic allocation pool management
	proc->d_buffer_pools = CUDA_BufferInit(CUDA_MALLOC_CAP);
    std::cerr << "* device " << gpuid << " allocating " 
        << CUDA_MALLOC_CAP / MB_SIZE << " MB for dynamic allocation pool\n";

	// initialize intermediate processing memory on device
    size = (sizeof(smem_aux_t) + sizeof(mem_seed_v) + sizeof(mem_chain_v)
            + sizeof(seed_record_t) * MAX_NUM_SW_SEEDS + sizeof(mem_alnreg_v)
            + sizeof(mem_aln_v) + 20 * sizeof(int)) * MAX_BATCH_SIZE 
            + sizeof(int);
    CUDA_CHECK(cudaMalloc(&proc->d_aux, sizeof(smem_aux_t) * MAX_BATCH_SIZE));
	CUDA_CHECK(cudaMalloc(&proc->d_seq_seeds, MAX_BATCH_SIZE *sizeof(mem_seed_v)));
	CUDA_CHECK(cudaMalloc(&proc->d_chains, MAX_BATCH_SIZE *sizeof(mem_chain_v)));
	CUDA_CHECK(cudaMalloc(&proc->d_seed_records, MAX_BATCH_SIZE *MAX_NUM_SW_SEEDS*sizeof(seed_record_t)));
	CUDA_CHECK(cudaMalloc(&proc->d_Nseeds, sizeof(int)));
	CUDA_CHECK(cudaMalloc(&proc->d_regs, MAX_BATCH_SIZE *sizeof(mem_alnreg_v)));
	CUDA_CHECK(cudaMalloc(&proc->d_alns, MAX_BATCH_SIZE *sizeof(mem_aln_v)));

	CUDA_CHECK(cudaMalloc(&proc->d_offsets, MAX_BATCH_SIZE *sizeof(int)));
	CUDA_CHECK(cudaMalloc(&proc->d_rids, MAX_BATCH_SIZE *sizeof(int)));
	CUDA_CHECK(cudaMalloc(&proc->d_positions, MAX_BATCH_SIZE *sizeof(int64_t)));
	CUDA_CHECK(cudaMalloc(&proc->d_ncigars, MAX_BATCH_SIZE *sizeof(int)));
	CUDA_CHECK(cudaMalloc(&proc->d_cigars, MAX_BATCH_SIZE *sizeof(uint32_t) * MAX_N_CIGAR));
    size += MAX_BATCH_SIZE * (sizeof(int) + sizeof(int) + sizeof(int64_t) + sizeof(int) + sizeof(uint32_t) * MAX_N_CIGAR);

	CUDA_CHECK(cudaMalloc(&proc->d_sortkeys_in, MAX_BATCH_SIZE *5*sizeof(int)));
	CUDA_CHECK(cudaMalloc(&proc->d_sortkeys_out, MAX_BATCH_SIZE *5*sizeof(int)));
	CUDA_CHECK(cudaMalloc(&proc->d_seqIDs_in, MAX_BATCH_SIZE *5*sizeof(int)));
	CUDA_CHECK(cudaMalloc(&proc->d_seqIDs_out, MAX_BATCH_SIZE *5*sizeof(int)));
    if(proc->d_aux && proc->d_seq_seeds && proc->d_chains 
            && proc->d_seed_records && proc->d_Nseeds && proc->d_regs
            && proc->d_offsets && proc->d_rids && proc->d_positions && proc->d_ncigars && proc->d_cigars
            && proc->d_alns && proc->d_sortkeys_in && proc->d_sortkeys_out
            && proc->d_seqIDs_in && proc->d_seqIDs_out){
        std::cerr << "* device " << gpuid 
            << " intermediate data " << size / MB_SIZE << " MB\n";
    } else{
        std::cerr << "* device " << gpuid 
            << " intermediate data alloc failed\n";
        exit(EXIT_FAILURE);
    }


    // input on device
    size = MAX_BATCH_SIZE * sizeof(uint8_t) * MAX_LEN_READ;
    CUDA_CHECK(cudaMalloc(&proc->d_seq, size));
    std::cerr << "* device " << gpuid << " allocating " 
        << size / MB_SIZE << " MB for input seqs\n";
    if(proc->d_seq == nullptr){
        std::cerr << "Error.  device memory for copying seqs.\n";
        exit(EXIT_FAILURE);
    }
    size = sizeof(int) * (MAX_BATCH_SIZE + 1);
    CUDA_CHECK(cudaMalloc(&proc->d_seq_offset, size));
    std::cerr << "* device " << gpuid << " allocating " 
        << size / MB_SIZE << " MB for input seq offsets\n";
	if(proc->d_seq_offset == nullptr){
        std::cerr << "Error.  device memory for copying offsets.\n";
        exit(EXIT_FAILURE);
    }


    CUDA_CHECK(cudaMalloc(&proc->d_alns_offset, 
                sizeof(int) * (MAX_BATCH_SIZE + 1)));
    CUDA_CHECK(cudaMalloc(&proc->d_rid,
                sizeof(int) * (MAX_ALN_CNT + 1)));
    CUDA_CHECK(cudaMalloc(&proc->d_pos,
                sizeof(uint64_t) * (MAX_ALN_CNT + 1)));
    CUDA_CHECK(cudaMalloc(&proc->d_chunk_aln_count,
                sizeof(int)));
    if(proc->d_alns_offset == nullptr || 
            proc->d_rid == nullptr ||
            proc->d_pos == nullptr ||
            proc->d_chunk_aln_count == nullptr){
        std::cerr << "cudaMalloc err.\n";
        exit(EXIT_FAILURE);
    }


    /*
	// pinned memory for memcpy
    size = g3_opt->batch_size * sizeof(uint8_t) * MAX_LEN_READ;
    CUDA_CHECK(cudaMallocHost(&proc->h_seq, size));
    std::cerr << "* allocating " 
        << size << " bytes for input seq pinned memcpy\n";
	if(proc->h_seq == nullptr){
        std::cerr << "Error. Host pinned memory for copying seqs.\n";
        exit(EXIT_FAILURE);
    }
    size = (g3_opt->batch_size + 1) * sizeof(int);
    CUDA_CHECK(cudaMallocHost(&proc->h_seq_offset, size));
    std::cerr << "* allocating " 
        << size << " bytes for input seq offsets pinned memcpy\n";
	if(proc->h_seq_offset == nullptr){
        std::cerr << "Error. Host pinned memory for copying offsets.\n";
        exit(EXIT_FAILURE);
    }
    */

	// initialize a cuda stream for processing
	//proc->CUDA_stream = malloc(sizeof(cudaStream_t));
	//CUDA_CHECK(cudaStreamCreate((cudaStream_t*)proc->CUDA_stream));

    return proc;
}

void memcpy_index(
        process_data_t *instance,
        int gpuid, 
        pipeline_aux_t *aux
        )
{
    TIMER_INIT();
    TIMER_START();
    CUDA_CHECK(cudaSetDevice(gpuid));
    int current;
    CUDA_CHECK(cudaGetDevice(&current));
    if(current != gpuid){
        std::cerr << "GPU " << gpuid << "  " << "* device_alloc: cudaSetDevice is wrong" << std::endl;
        exit(1);
    }
    unsigned long long size;

	// user-defined options
	transferOptions(aux->opt, aux->pes0, instance, &size);
    
	// transfer index data
	transferIndex(aux->idx->bwt, aux->idx->bns, aux->idx->pac, 
            aux->kmerHashTab, instance, &size);

    transferFmIndex(instance, &(aux->loadedIndex), &size);
    TIMER_END(0, "");
    tprof[gpuid][GPU_SETUP] = duration.count() / 1000;
}


// no pinned memcpy for now.
void memcpy_input(int batch_size, process_data_t *proc,
        uint8_t *seq, int *seq_offset)
{
    int total_seq_len = seq_offset[batch_size];
    size_t size;
    size = sizeof(uint8_t) * total_seq_len;
    CUDA_CHECK(cudaMemcpy(proc->d_seq, seq, size, cudaMemcpyHostToDevice));
    size = sizeof(int) * (batch_size + 1);
    CUDA_CHECK(cudaMemcpy(proc->d_seq_offset, seq_offset, size,
            cudaMemcpyHostToDevice));
    proc->batch_size = batch_size;

    // reset
    int zero_int = 0;
    CUDA_CHECK(cudaMemcpy(proc->d_Nseeds, &zero_int, sizeof(int), cudaMemcpyHostToDevice));
    CUDAResetBufferPool(proc->d_buffer_pools);
}




void check_device_count(int num_requested_gpus)
{
    int num_available_gpus;
    CUDA_CHECK(cudaGetDeviceCount(&num_available_gpus));
    if(num_available_gpus < num_requested_gpus){
        std::cerr << "!! invalid request of " << num_requested_gpus 
            << " GPUs where only " << num_available_gpus << " GPUs are available.";
        exit(1);
    } else{
        std::cerr << "* using " << num_requested_gpus << 
            " GPUs out of " << num_available_gpus << " available GPUs.\n";
    }
}

void destruct_proc(process_data_t *proc)
{
    //cudaStreamDestroy(*(cudaStream_t*)proc->CUDA_stream);
}

void cuda_wrapper_test()
{
    std::chrono::high_resolution_clock::time_point start, end;
    std::chrono::duration<long long, std::micro> duration;
    start = std::chrono::high_resolution_clock::now();

    cudaFree(0);

    end = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    std::cerr << "* cuda init: " << duration.count() / 1000 << " ms" << std::endl;
}


void memcpy_output(
        aligned_chunk *ac,
        process_data_t * proc)
{
    // copy output from device to host
    // proc->d_offsets, proc->d_rids, proc->d_positions, proc->d_ncigars, proc->d_cigars.
    // ac->offsets, ac->rids, ac->positions, ac->ncigars, ac->cigars
    ac->offsets.resize(ac->chunk_size + 1); // + 1 for the total count
    CUDA_CHECK(cudaMemcpy(ac->offsets.data(),
                proc->d_offsets,
                sizeof(int) * (ac->chunk_size + 1),
                cudaMemcpyDeviceToHost));
    int batch_alns_count = ac->offsets[ac->chunk_size];
    ac->rids.resize(batch_alns_count);
    ac->positions.resize(batch_alns_count);
    ac->ncigars.resize(batch_alns_count);
    ac->cigars.resize(batch_alns_count * MAX_N_CIGAR);

    CUDA_CHECK(cudaMemcpy(ac->rids.data(),
                proc->d_rids,
                sizeof(int) * batch_alns_count,
                cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(ac->positions.data(),
                proc->d_positions,
                sizeof(int64_t) * batch_alns_count,
                cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(ac->ncigars.data(),
                proc->d_ncigars,
                sizeof(int) * batch_alns_count,
                cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(ac->cigars.data(),
                proc->d_cigars,
                sizeof(uint32_t) * batch_alns_count * MAX_N_CIGAR,
                cudaMemcpyDeviceToHost));
}


#if 0

/* copy sam output to host */
void CUDATransferSamOut(transfer_data_t *transfer_data){
    int gpuid = transfer_data->gpu_no;
    if(cudaSetDevice(transfer_data->gpu_no) != cudaSuccess){
        std::cerr << "CUDATransferSamOut: cudaSetDevice failed" << std::endl;
        exit(1);
    }
    int current;
    cudaGetDevice(&current);
    if(current != gpuid){
         std::cerr << "GPU " << gpuid << "  " << "CUDATransferSamOut: cudaSetDevice is wrong" << std::endl;
        return;
    }
	cudaStream_t *transfer_stream = (cudaStream_t*)(transfer_data->CUDA_stream);
	CUDA_CHECK(cudaMemcpyAsync(transfer_data->h_seqs, transfer_data->d_seqs, transfer_data->batch_size*sizeof(bseq1_t), cudaMemcpyDeviceToHost, *transfer_stream));
	// transfer all SAM from device to host
	// first find the total size of all SAM's
	int sam_size;
	CUDA_CHECK(cudaMemcpyAsync(&sam_size, transfer_data->d_seq_sam_size, sizeof(int), cudaMemcpyDeviceToHost, *transfer_stream));
	// now copy
	CUDA_CHECK(cudaMemcpyAsync(transfer_data->h_seq_sam_ptr, transfer_data->d_seq_sam_ptr, sam_size, cudaMemcpyDeviceToHost, *transfer_stream));

	cudaStreamSynchronize(*transfer_stream);

	// after GPU processing, seqs[i].sam are offset. Now we need to convert this offset to actual location
	bseq1_t *seqs = transfer_data->h_seqs;
	char *sam = transfer_data->h_seq_sam_ptr;
	for (int i=0; i<transfer_data->batch_size; i++)
		seqs[i].sam = sam + (long)seqs[i].sam;
}
#endif
