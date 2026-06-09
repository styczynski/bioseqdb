#include "device_mem.h"

static size_t get_alignment(size_t size) {
    // Start with at least alignof(std::max_align_t)
    //size_t alignment = alignof(std::max_align_t);
    size_t alignment = alignof(max_align_t);

    // If typeSize is larger than alignof(std::max_align_t), find the next power-of-two >= typeSize
    while (alignment < size) {
        alignment <<= 1;  // Double the alignment until it's large enough
    }

    return alignment;
}

void* fetch_memory_mempool(mpool_t* mpool, uint64_t length, int typeSize) {
    uint64_t totalLength = length * typeSize;

    // Determine a proper power-of-two alignment
    size_t alignment = get_alignment(typeSize);

    // Calculate current and aligned addresses
    uintptr_t current_address = reinterpret_cast<uintptr_t>(mpool->ptr);
    uintptr_t aligned_address = (current_address + alignment - 1) & ~(alignment - 1);

    // Calculate padding to achieve alignment
    size_t padding = aligned_address - current_address;

    // Add padding to the total length needed
    totalLength += padding;

    // Check for capacity
    if (mpool->ptrIdx + totalLength > mpool->capacity) {
        printf("[err] allocated size is larger than memory pool size.\n");
        return nullptr;
    }

    // The aligned target pointer
    void* targetPtr = reinterpret_cast<void*>(aligned_address);

    // Advance the memory pool pointer and index
    mpool->ptr = static_cast<char*>(mpool->ptr) + totalLength;
    mpool->ptrIdx += totalLength;

    return targetPtr;
}

void free_tmp_memory_mempool(mpool_t* mpool) {
    mpool->ptr = mpool->flagPtr;
    mpool->ptrIdx = mpool->flagIdx;

    DEBUG_ONLY(
        printf("moved back to index: %llu, mempool size: %llu\n", mpool->ptrIdx, mpool->capacity);
    );
    return;

}

void set_tmp_memory_flag_mempool(mpool_t* mpool){
    mpool->flagPtr = mpool->ptr;
    mpool->flagIdx = mpool->ptrIdx;
    return;
}

void initialize_mempool(mpool_t* mpool){
    mpool->ptr = mpool->basePtr;
    mpool->flagPtr = mpool->basePtr;
    mpool->flagIdx = 0;
    mpool-> ptrIdx = 0;
}

void allcoate_memory_mempool(mpool_t* mpool) {
    checkCudaErrors(cudaMalloc(&mpool->basePtr, mpool->capacity * sizeof(char)));
    mpool->ptr = mpool->basePtr;
    mpool->ptrIdx = 0;
    return;
}

void allocate_memory_input(device_input_batch_t* batch, int seqLength, int ofsLength){
    check_mem();
    cudaMalloc(&batch->opt, sizeof(mm_mapopt_t));
    cudaMalloc(&batch->seqs, sizeof(char)*seqLength);
    cudaMalloc(&batch->lens, sizeof(int)*ofsLength);
    cudaMalloc(&batch->ofs, sizeof(int)*ofsLength);

    batch->totalSeqLength = seqLength;

    DEBUG_ONLY(
        std::cout<<"allocated input memory : ";
        check_mem();
    );
}

void allocate_memory_seed_mempool(device_seed_batch_t* batch, mpool_t* mpool, int seqLength, int ofsLength, int anchorLength){
    
    batch->ax = static_cast<uint64_t*>(fetch_memory_mempool(mpool, anchorLength, sizeof(uint64_t)));
    batch->ay = static_cast<uint64_t*>(fetch_memory_mempool(mpool, anchorLength, sizeof(uint64_t)));

    batch->n_anchors = static_cast<int*>(fetch_memory_mempool(mpool, ofsLength, sizeof(int)));
    batch->anchor_ofs = static_cast<int*>(fetch_memory_mempool(mpool, ofsLength, sizeof(int)));
    
    
    // below data are not used after seeding step
    set_tmp_memory_flag_mempool(mpool); // set tmp flag here
    batch->tmp_n = static_cast<int*>(fetch_memory_mempool(mpool, seqLength, sizeof(int)));
    batch->tmp_cr = static_cast<uint64_t**>(fetch_memory_mempool(mpool, seqLength, sizeof(uint64_t*)));

    batch->tmp_seed = static_cast<mm_seed_t*>(fetch_memory_mempool(mpool, seqLength, sizeof(mm_seed_t)));

    batch->minimizers = static_cast<mm128_t*>(fetch_memory_mempool(mpool, seqLength, sizeof(mm128_t)));
    batch->n_minimizers = static_cast<int*>(fetch_memory_mempool(mpool, ofsLength, sizeof(int)));

    batch->num_anchors = static_cast<int*>(fetch_memory_mempool(mpool, 1, sizeof(int)));
    checkCudaErrors(cudaMemset(batch->num_anchors, 0, sizeof(int)));

    DEBUG_ONLY(
        std::cout<<"allocated seed memory : ";
        check_mem();
    );

    return;
}

void allocate_memory_chain_mempool(device_chain_batch_t* batch, mpool_t* mpool,int seqLength, int ofsLength, int anchorLength, int chainLength){

    batch->n_chain = static_cast<int*>(fetch_memory_mempool(mpool, 1, sizeof(int)));
    checkCudaErrors(cudaMemset(batch->n_chain, 0, sizeof(int)));

    batch->chain = static_cast<mm_chain_t*>(fetch_memory_mempool(mpool, chainLength, sizeof(mm_chain_t)));
    batch->dropped = static_cast<bool*>(fetch_memory_mempool(mpool, chainLength, sizeof(bool)));
    
    set_tmp_memory_flag_mempool(mpool); 

    batch->range = static_cast<uint2*>(fetch_memory_mempool(mpool, anchorLength, sizeof(uint2)));
    batch->n_range = static_cast<int*>(fetch_memory_mempool(mpool, ofsLength, sizeof(int)));
    batch->seg_pass = static_cast<bool*>(fetch_memory_mempool(mpool, seqLength, sizeof(bool)));

    batch->sc = static_cast<int32_t*>(fetch_memory_mempool(mpool, anchorLength, sizeof(int32_t)));
    batch->p = static_cast<int64_t*>(fetch_memory_mempool(mpool, anchorLength, sizeof(int64_t)));

    batch->long_range_buf_len = static_cast<int*>(fetch_memory_mempool(mpool, seqLength, sizeof(int)));
    batch->long_range_buf_st = static_cast<int*>(fetch_memory_mempool(mpool, seqLength, sizeof(int)));
    batch->mid_range_buf_len = static_cast<int*>(fetch_memory_mempool(mpool, seqLength, sizeof(int)));
    batch->mid_range_buf_st = static_cast<int*>(fetch_memory_mempool(mpool, seqLength, sizeof(int)));
    
    batch->n_v = static_cast<int*>(fetch_memory_mempool(mpool, ofsLength, sizeof(int)));
    batch->n_u = static_cast<int*>(fetch_memory_mempool(mpool, ofsLength, sizeof(int)));

    batch->u = static_cast<uint64_t*>(fetch_memory_mempool(mpool, anchorLength, sizeof(uint64_t)));
    batch->zx = static_cast<int64_t*>(fetch_memory_mempool(mpool, anchorLength, sizeof(int64_t)));
    batch->zy = static_cast<int64_t*>(fetch_memory_mempool(mpool, anchorLength, sizeof(int64_t)));
    batch->t = static_cast<int32_t*>(fetch_memory_mempool(mpool, anchorLength, sizeof(int32_t)));
    batch->v = static_cast<int64_t*>(fetch_memory_mempool(mpool, anchorLength, sizeof(int64_t)));

    DEBUG_ONLY(
        printf("allocated chain size: %d", chainLength);
        std::cout<<"allocated chain memory : ";
        check_mem();
    );

    return;
}

void allocate_memory_extend_mempool(device_extend_batch_t* batch, mpool_t* mpool, int n_chain, int maxSeqLength, int tb_batch_size, int gpu_id){
    int n_gap = 2;// hard estimation of number of gap filling extensions
    int n_align = n_chain * (2 + n_gap);
    uint64_t seqArrayLength = maxSeqLength * (uint64_t)n_align;
   
    batch->qseqs = static_cast<uint8_t*>(fetch_memory_mempool(mpool, seqArrayLength, sizeof(uint8_t)));
    batch->rseqs = static_cast<uint8_t*>(fetch_memory_mempool(mpool, seqArrayLength, sizeof(uint8_t)));
    batch->q_lens = static_cast<uint32_t*>(fetch_memory_mempool(mpool, n_align, sizeof(uint32_t)));
    batch->r_lens = static_cast<uint32_t*>(fetch_memory_mempool(mpool, n_align, sizeof(uint32_t)));
    batch->q_ofs = static_cast<uint32_t*>(fetch_memory_mempool(mpool, n_align, sizeof(uint32_t)));
    batch->r_ofs = static_cast<uint32_t*>(fetch_memory_mempool(mpool, n_align, sizeof(uint32_t)));

    batch->zdropped = static_cast<bool*>(fetch_memory_mempool(mpool, n_align, sizeof(bool)));
    cudaMemset(batch->zdropped, 0, n_align * sizeof(bool));
    batch->n_cigar = static_cast<int*>(fetch_memory_mempool(mpool, n_align, sizeof(int)));
    batch->cigar = static_cast<uint32_t*>(fetch_memory_mempool(mpool, seqArrayLength, sizeof(uint32_t)));

    batch->d_qseqs_packed = static_cast<uint32_t*>(fetch_memory_mempool(mpool, seqArrayLength/8, sizeof(uint32_t)));
    batch->d_rseqs_packed = static_cast<uint32_t*>(fetch_memory_mempool(mpool, seqArrayLength/8, sizeof(uint32_t)));
    
    checkCudaErrors(cudaMallocManaged(&batch->device_res, sizeof(gasal_res_t))); 
    cudaMemAdvise(batch->device_res, sizeof(gasal_res_t), cudaMemAdviseSetPreferredLocation, gpu_id);
    cudaMemAdvise(batch->device_res, sizeof(gasal_res_t), cudaMemAdviseSetAccessedBy, gpu_id);

    batch->device_res->aln_score = static_cast<int*>(fetch_memory_mempool(mpool, n_align, sizeof(int)));
    batch->device_res->query_batch_end = static_cast<int*>(fetch_memory_mempool(mpool, n_align, sizeof(int)));
    batch->device_res->target_batch_end = static_cast<int*>(fetch_memory_mempool(mpool, n_align, sizeof(int)));

    batch->tb_buf_size = static_cast<uint32_t*>(fetch_memory_mempool(mpool, n_align, sizeof(int32_t)));
    batch->tb_buf_ofs = static_cast<uint64_t*>(fetch_memory_mempool(mpool, n_align, sizeof(int64_t)));

    batch->n_extend = static_cast<int*>(fetch_memory_mempool(mpool, 1, sizeof(int)));
    checkCudaErrors(cudaMemset(batch->n_extend, 0, sizeof(int)));
    batch->actual_n_alns_left = static_cast<int*>(fetch_memory_mempool(mpool, 1, sizeof(int)));

    int agatha_block_num = 256;
    int agatha_thread_num = 256;
    batch->global_buffer_top = static_cast<short2*>(fetch_memory_mempool(mpool, maxSeqLength*(agatha_thread_num/8)*agatha_block_num*3+n_align, sizeof(short2)));
    
    DEBUG_ONLY(
        std::cout<<"n_chain: "<<n_chain<<" n_align:" <<n_align<<"\t"<<seqArrayLength<<"\n";
        printf("allocated extension memory : ");
        check_mem();
    );
    
    batch->dblock_row = static_cast<short2*>(fetch_memory_mempool(mpool, (uint64_t)2 * tb_batch_size * maxSeqLength * maxSeqLength / DBLOCK_SIZE, sizeof(short2)));
    batch->dblock_col = (short2*) (batch->dblock_row + ((uint64_t)tb_batch_size * maxSeqLength * (maxSeqLength/ DBLOCK_SIZE)));
    batch->tb_matrices = (uint32_t*)batch->dblock_row;
    batch->tb_batch_size = tb_batch_size;
    batch->zdrop_code = static_cast<uint8_t*>(fetch_memory_mempool(mpool, n_align, sizeof(uint8_t)));

    DEBUG_ONLY(
        printf("tbmatrix size.. : %llu, tb batch size %d maxseqlength %d \n", (uint64_t) (2 * (uint64_t)tb_batch_size * maxSeqLength * maxSeqLength / DBLOCK_SIZE),
        tb_batch_size, maxSeqLength);
        std::cout<<"allocated tb matrix : ";
        check_mem();
    );

    return;
}

void allocate_memory_output_mempool(device_output_batch_t* batch, mpool_t* mpool, int n_results, int cigarLen, int batch_size){

    batch->aln_score = static_cast<int*>(fetch_memory_mempool(mpool, n_results, sizeof(int)));
    batch->n_cigar = static_cast<int*>(fetch_memory_mempool(mpool, n_results, sizeof(int)));
    batch->cigar = static_cast<uint32_t*>(fetch_memory_mempool(mpool, cigarLen, sizeof(uint32_t)));
    checkCudaErrors(cudaMalloc(&batch->n_align, sizeof(int)));

    batch->zdropped = static_cast<uint8_t*>(fetch_memory_mempool(mpool, batch_size, sizeof(uint8_t)));
    batch->read_id = static_cast<int*>(fetch_memory_mempool(mpool, n_results, sizeof(int)));

    checkCudaErrors(cudaMemset(batch->zdropped, 0, sizeof(uint8_t)*batch_size));
    checkCudaErrors(cudaMemset(batch->read_id, -1, sizeof(int)*batch_size));
    checkCudaErrors(cudaMemset(batch->n_cigar, 0, sizeof(int)*n_results));
    
    DEBUG_ONLY(
        std::cout<<"allocated output memory : ";
        check_mem();
    );

    return;
}

void free_memory_input();

void free_memory_seed(device_seed_batch_t* batch);
void free_memory_chain();
void free_memory_extend();
void free_memory_output();