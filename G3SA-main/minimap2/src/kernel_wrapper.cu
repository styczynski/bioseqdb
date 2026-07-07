#include "kernel_wrapper.h"
#include <thrust/host_vector.h>
#include <thrust/device_vector.h>
#include <thrust/sort.h>
#include <thrust/count.h>
#include <cub/cub.cuh>  


void compute_grid_offset(const int* qseq_lens, const int* rseq_lens, int num_sequences, uint64_t max_batch_size, uint64_t* offsets, int* batch_sizes, int& num_batches) {
    int current_batch_size = 0;
    uint64_t current_offset = 0;
    uint64_t current_batch_count = 0;

    for (int i = 0; i < num_sequences; ++i) {

        int qseq_len = (qseq_lens[i] + 7) & ~7; // round up to multiple of 8
        int rseq_len = (rseq_lens[i] + 7) & ~7;

        int seq_len = (qseq_len * (int)(rseq_len / DBLOCK_SIZE_D + 1)) > (rseq_len * (int)(qseq_len / DBLOCK_SIZE_D + 1))? 
            (qseq_len * (int)(rseq_len / DBLOCK_SIZE_D + 1)) : (rseq_len * (int)(qseq_len / DBLOCK_SIZE_D + 1));
        
        // Check if adding the current sequence exceeds the max batch size
        if (current_offset + seq_len > max_batch_size) {
            // Start a new batch
            batch_sizes[current_batch_count] = current_batch_size;
            current_batch_size = 0;
            current_offset = 0;
            ++current_batch_count;
        }

        // Assign offset and update batch size
        offsets[i] = current_offset;
        current_offset += seq_len;
        current_batch_size ++;
    }

    // Add the last batch if it exists
    if (current_batch_size > 0) {
        batch_sizes[current_batch_count] = current_batch_size;
        ++current_batch_count;
    }

    num_batches = current_batch_count;
}


struct is_false {
    __host__ __device__
    bool operator()(const bool &x) const {
        return !x;
    }
};

static void ksw_gen_simple_mat(int m, int8_t *mat, int8_t a, int8_t b, int8_t sc_ambi)
{
	int i, j;
	a = a < 0? -a : a;
	b = b > 0? -b : b;
	sc_ambi = sc_ambi > 0? -sc_ambi : sc_ambi;
	for (i = 0; i < m - 1; ++i) {
		for (j = 0; j < m - 1; ++j)
			mat[i * m + j] = i == j? a : b;
		mat[i * m + m - 1] = sc_ambi;
	}
	for (j = 0; j < m; ++j)
		mat[(m - 1) * m + j] = sc_ambi;
}

static void ksw_gen_ts_mat(int m, int8_t *mat, int8_t a, int8_t b, int8_t transition, int8_t sc_ambi)
{
	assert(m==5);
	ksw_gen_simple_mat(m,mat,a,b,sc_ambi);
	transition = transition > 0? -transition : transition;
	mat[0*m+2]=transition;  // A->G
	mat[1*m+3]=transition;  // C->T
	mat[2*m+0]=transition;  // G->A
	mat[3*m+1]=transition;  // T->C
}

static inline void upload_params_once(const Params& h) {
    cudaMemcpyToSymbol(d_params, &h, sizeof(h)); 
}

void gpu_initialize(int gpu_id, mm_idx_t* mi, mm_mapopt_t* h_opt, device_pointer_batch_t* device_ptrs, uint64_t max_seq_len, int batch_size){

    cudaError_t err;

    /* Bind this thread to the specified GPU */
    err = cudaSetDevice(gpu_id);
    if (err != cudaSuccess) {
        printf("[%d] Error in cudaSetDevice: %s\n", gpu_id ,cudaGetErrorString(err));
        return;
    }
    int device_id;
    cudaGetDevice(&device_id);
    printf("GPU no. %d initalization\n", device_id);
    device_ptrs->device_num = device_id;

    /* Move Minimap2 index */
    struct timespec start, end;
    clock_gettime(CLOCK_BOOTTIME, &start);
    device_ptrs->idx = transfer_index_multigpu(mi);
    clock_gettime(CLOCK_BOOTTIME, &end);
    printf(" ***** [%d] cuda kernel took %f seconds to move minimap2 index\n", device_id,
        ( end.tv_sec - start.tv_sec ) + ( end.tv_nsec - start.tv_nsec ) / 1E9);

    /* Allocate Device I/O memory - sequence data & SAM data */
    allocate_memory_input(&(device_ptrs->input), max_seq_len * batch_size, batch_size);

    /* Allocate Memory pool */
    device_ptrs->mpool.capacity = 29 * MEMPOOL_SIZE; // n GB of memory for computational use
    allcoate_memory_mempool(&(device_ptrs->mpool));

    cudaMemcpy((device_ptrs->input).opt, h_opt, sizeof(mm_mapopt_t), cudaMemcpyHostToDevice);

    check_mem();
}


void gpu_mm2_kernel_wrapper(
    char* h_seqs, int* h_lens, int* h_ofs, device_pointer_batch_t* device_ptrs, int w, int k, uint64_t max_seq_len, int batch_size,
    uint32_t* h_cigar, int* h_n_cigar, int* h_score, // output data
    uint8_t* h_dropped // fallback reads
){

    cudaError_t err; struct timespec start, end; // used for debugging
    int device_id;
    cudaGetDevice(&device_id);
    DEBUG_ONLY(
        err = cudaGetLastError();
        printf("CUDA initialize setting Error: %s\n", cudaGetErrorString(err)); 
    );

    ///////////////////////////////////////// allocating and copying target read data /////////////////////////////////////////
    
    /* input seqeunce data (Host side)*/
    DEBUG_ONLY(
        clock_gettime(CLOCK_BOOTTIME, &start);
    );

    device_input_batch_t* input_batch = &(device_ptrs->input);

    cudaMemcpy(input_batch->seqs, h_seqs, sizeof(char) * batch_size * max_seq_len, cudaMemcpyHostToDevice);
    cudaMemcpy(input_batch->lens, h_lens, sizeof(int) * batch_size, cudaMemcpyHostToDevice);
    cudaMemcpy(input_batch->ofs, h_ofs, sizeof(int) * batch_size, cudaMemcpyHostToDevice);

    err = cudaGetLastError();
    DEBUG_PRINT("debug print enabled\n");
    DEBUG_PRINT("CUDA initialize memcpy Error: %s\n", cudaGetErrorString(err));

    int tmp_n_chain_total = max_seq_len * batch_size /1000; // TODO: put total seq size
    
    // TODO: maximum extend batch size 
    int tmp_n_chain_filtered = max_seq_len * batch_size / 1000;
    int tb_batch_size = TB_BUF_SIZE;

    mm_idx_t* d_idx = device_ptrs->idx;

    device_seed_batch_t seed_batch;
    device_chain_batch_t chain_batch;
    device_extend_batch_t extend_batch;
    device_output_batch_t* output_batch = &(device_ptrs->output);

    // initialized mempool
    initialize_mempool(&(device_ptrs->mpool));

    DEBUG_ONLY(
        err = cudaGetLastError();
        printf("CUDA initialize mempool Error: %s\n", cudaGetErrorString(err)); 
    );

    allocate_memory_seed_mempool(&seed_batch, &(device_ptrs->mpool), max_seq_len * batch_size, batch_size, (uint32_t)(max_seq_len * batch_size * MEM_FACTOR));

    cudaDeviceSynchronize();

    DEBUG_ONLY(
        err = cudaGetLastError();
        printf("CUDA seed mempool Error: %s\n", cudaGetErrorString(err)); 

        cudaDeviceSynchronize();
        clock_gettime(CLOCK_BOOTTIME, &end);
        printf(" ***** cuda kernel took %f seconds to fetch memory\n",
            ( end.tv_sec - start.tv_sec ) + ( end.tv_nsec - start.tv_nsec ) / 1E9);
    );
    
    seeding_kernel_wrapper(d_idx, *input_batch, seed_batch, chain_batch, batch_size);

    free_tmp_memory_mempool(&(device_ptrs->mpool));
    allocate_memory_chain_mempool(&chain_batch, &(device_ptrs->mpool), max_seq_len * batch_size, batch_size, (uint32_t)(max_seq_len * batch_size * MEM_FACTOR), tmp_n_chain_total);
    
    /* Chaining */
    uint32_t h_num_anchors;
    cudaMemcpy(&h_num_anchors, seed_batch.num_anchors, sizeof(uint32_t), cudaMemcpyDeviceToHost);

    DEBUG_ONLY(
        printf("copied anchor num to host. Call chaining kernel. total anchor array size: %d allocated host memory size %d\n", h_num_anchors, max_seq_len * batch_size * MEM_FACTOR);
    );

    chaining_kernel_wrapper(*input_batch, seed_batch, chain_batch, extend_batch, d_idx, (h_num_anchors)/READ_SEG,
                           max_seq_len * batch_size, batch_size, h_num_anchors, batch_size);

    DEBUG_ONLY(
        printf("after chaining kernel\n");
        check_mem();
    );

    free_tmp_memory_mempool(&(device_ptrs->mpool));

    int max_ext_len = 12000; // maximum extending target length
    allocate_memory_extend_mempool(&extend_batch, &(device_ptrs->mpool), tmp_n_chain_filtered, max_ext_len, tb_batch_size, device_id);
 
    /* Extending */
 
    extending_kernel_wrapper(d_idx, *input_batch, seed_batch, chain_batch, extend_batch, max_ext_len);
    
    // collect result kernel
    int h_num_chains;
    cudaMemcpy(&h_num_chains, chain_batch.n_chain, sizeof(uint32_t), cudaMemcpyDeviceToHost);
    int h_num_extends;
    cudaMemcpy(&h_num_extends, extend_batch.n_extend, sizeof(uint32_t), cudaMemcpyDeviceToHost);

    DEBUG_ONLY(
        printf("hnum chain %d hnum extend %d\n", h_num_chains, h_num_extends);
    );

    /* Collecting results */
    // fetch memory for final results TODO: check capacity limit and free temporary pointers
    allocate_memory_output_mempool(output_batch, &(device_ptrs->mpool), tmp_n_chain_total, tmp_n_chain_total * max_seq_len, batch_size);

    DEBUG_ONLY(
        printf("output batch allocated on gpu: %d acutal: %d\n", tmp_n_chain_total, h_num_chains);
    );

    DEBUG_ONLY(
        cudaDeviceSynchronize(); err = cudaGetLastError(); printf("pre zdrop Error: %s\n", cudaGetErrorString(err));
        clock_gettime(CLOCK_BOOTTIME, &start);
    );
    

    // pass gap extend results only
    int8_t mat[25];
    ksw_gen_ts_mat(5, mat, 2, 4, 4, 1); 

    mm_test_zdrop<<<BLOCK_NUM_SHORT, THREAD_NUM_SHORT, 0, 0>>>(extend_batch.zdrop_code, extend_batch.q_lens + (h_num_chains) * 2, extend_batch.r_lens + (h_num_chains) * 2, extend_batch.qseqs + max_ext_len * (h_num_chains) * 2, extend_batch.rseqs + max_ext_len * (h_num_chains) * 2, 
              extend_batch.n_cigar + (h_num_chains) * 2, extend_batch.cigar + max_ext_len * (h_num_chains) * 2, h_num_extends, max_ext_len, mat);

    DEBUG_ONLY(cudaDeviceSynchronize(); err = cudaGetLastError(); printf("test zdrop Error: %s\n", cudaGetErrorString(err)););
    
    collect_extending_results<<<BLOCK_NUM_SHORT, THREAD_NUM_SHORT, 0, 0>>>(chain_batch.chain, extend_batch.device_res->aln_score, extend_batch.n_cigar, extend_batch.cigar, output_batch->n_cigar, output_batch->cigar, h_num_chains, max_ext_len, max_seq_len,
                                extend_batch.zdrop_code, output_batch->zdropped, output_batch->read_id);

    DEBUG_ONLY(cudaDeviceSynchronize(); err = cudaGetLastError(); printf("collect extension result error: %s\n", cudaGetErrorString(err)););

    // For complete cigar output
    cudaMemcpy(h_dropped, output_batch->zdropped, sizeof(bool) * batch_size, cudaMemcpyDeviceToHost);
    cudaMemcpy(h_n_cigar, output_batch->n_cigar, sizeof(int) * tmp_n_chain_total, cudaMemcpyDeviceToHost);
    cudaMemcpy(h_cigar, output_batch->cigar, sizeof(uint32_t) * h_num_chains * max_seq_len, cudaMemcpyDeviceToHost);
    cudaMemcpy(h_score, output_batch->read_id, sizeof(int) * tmp_n_chain_total, cudaMemcpyDeviceToHost);
    
    DEBUG_ONLY(
        cudaDeviceSynchronize(); err = cudaGetLastError(); printf("debugging mempcy result error: %s\n", cudaGetErrorString(err));
        
        clock_gettime(CLOCK_BOOTTIME, &end);

        printf(" ***** cuda kernel took %f seconds to assemble extend results\n",
            ( end.tv_sec - start.tv_sec ) + ( end.tv_nsec - start.tv_nsec ) / 1E9);
    );

    return;

}

mm_idx_t* transfer_index(mm_idx_t* h_idx){

    mm_idx_t* d_idx;
    
    cudaMallocManaged(&d_idx, sizeof(mm_idx_t));
    cudaMemcpy(d_idx, h_idx, sizeof(mm_idx_t), cudaMemcpyHostToDevice);

    cudaMallocManaged(&d_idx->B, sizeof(mm_idx_bucket_t) * 1<<(h_idx->b));
    cudaMemcpy(d_idx->B, h_idx->B, sizeof(mm_idx_bucket_t) * 1<<(h_idx->b), cudaMemcpyHostToDevice);

    cudaMalloc(&d_idx->seq, sizeof(mm_idx_seq_t)*h_idx->n_seq);
    cudaMemcpy(d_idx->seq, h_idx->seq, sizeof(mm_idx_seq_t)*h_idx->n_seq, cudaMemcpyHostToDevice);

    uint32_t len_sum = 0;
    for(int i = 0; i < h_idx->n_seq; i++){
        len_sum += h_idx->seq[i].len;
    }
    cudaMalloc(&d_idx->S, sizeof(uint32_t)*(len_sum >> 3));
    cudaMemcpy(d_idx->S, h_idx->S, sizeof(uint32_t)*(len_sum >> 3), cudaMemcpyHostToDevice);

    for(int i = 0; i < 1<<h_idx->b; i++){
        cudaMallocManaged(&d_idx->B[i].p, sizeof(uint64_t)*h_idx->B[i].n);
        cudaMemcpy(d_idx->B[i].p, h_idx->B[i].p, sizeof(uint64_t)*h_idx->B[i].n, cudaMemcpyHostToDevice);
        
        cudaMallocManaged(&d_idx->B[i].a.a, sizeof(mm128_t)*h_idx->B[i].a.n);
        cudaMemcpy(d_idx->B[i].a.a, h_idx->B[i].a.a, sizeof(mm128_t)*h_idx->B[i].a.n, cudaMemcpyHostToDevice);
    
        cudaMallocManaged(&d_idx->B[i].h, sizeof(idxhash_t)*1);
        cudaMemcpy(d_idx->B[i].h, h_idx->B[i].h, sizeof(idxhash_t)*1, cudaMemcpyHostToDevice);

        cudaMallocManaged(&((idxhash_t*)(d_idx->B[i].h))->flags, sizeof(khint32_t)*((idxhash_t*)(h_idx->B[i].h))->n_buckets);
        cudaMemcpy(((idxhash_t*)(d_idx->B[i].h))->flags, ((idxhash_t*)(h_idx->B[i].h))->flags, sizeof(khint32_t)*((idxhash_t*)(h_idx->B[i].h))->n_buckets, cudaMemcpyHostToDevice);
    
        cudaMallocManaged(&((idxhash_t*)(d_idx->B[i].h))->vals, sizeof(uint64_t)*((idxhash_t*)(h_idx->B[i].h))->n_buckets);
        cudaMemcpy(((idxhash_t*)(d_idx->B[i].h))->vals, ((idxhash_t*)(h_idx->B[i].h))->vals, sizeof(uint64_t)*((idxhash_t*)(h_idx->B[i].h))->n_buckets, cudaMemcpyHostToDevice);

        cudaMallocManaged(&((idxhash_t*)(d_idx->B[i].h))->keys, sizeof(uint64_t)*((idxhash_t*)(h_idx->B[i].h))->n_buckets);
        cudaMemcpy(((idxhash_t*)(d_idx->B[i].h))->keys, ((idxhash_t*)(h_idx->B[i].h))->keys, sizeof(uint64_t)*((idxhash_t*)(h_idx->B[i].h))->n_buckets, cudaMemcpyHostToDevice);
        
    }

    return d_idx;

}


mm_idx_t* transfer_index_multigpu(mm_idx_t* h_idx) {
    mm_idx_t* d_idx;

    cudaMalloc((void**)&d_idx, sizeof(mm_idx_t));
    cudaMemcpy(d_idx, h_idx, sizeof(mm_idx_t), cudaMemcpyHostToDevice);

    mm_idx_bucket_t* d_B;
    cudaMalloc((void**)&d_B, sizeof(mm_idx_bucket_t) * (1 << h_idx->b));
    cudaMemcpy(d_B, h_idx->B, sizeof(mm_idx_bucket_t) * (1 << h_idx->b), cudaMemcpyHostToDevice);

    cudaMemcpy(&(d_idx->B), &d_B, sizeof(mm_idx_bucket_t*), cudaMemcpyHostToDevice);

    mm_idx_seq_t* d_seq;
    cudaMalloc((void**)&d_seq, sizeof(mm_idx_seq_t) * h_idx->n_seq);
    cudaMemcpy(d_seq, h_idx->seq, sizeof(mm_idx_seq_t) * h_idx->n_seq, cudaMemcpyHostToDevice);

    cudaMemcpy(&(d_idx->seq), &d_seq, sizeof(mm_idx_seq_t*), cudaMemcpyHostToDevice);

    uint32_t len_sum = 0;
    for (int i = 0; i < h_idx->n_seq; i++) {
        len_sum += h_idx->seq[i].len;
    }

    uint32_t* d_S;
    cudaMalloc((void**)&d_S, sizeof(uint32_t) * (len_sum >> 3));
    cudaMemcpy(d_S, h_idx->S, sizeof(uint32_t) * (len_sum >> 3), cudaMemcpyHostToDevice);

    cudaMemcpy(&(d_idx->S), &d_S, sizeof(uint32_t*), cudaMemcpyHostToDevice);

    for (int i = 0; i < (1 << h_idx->b); i++) {
        mm_idx_bucket_t h_bucket = h_idx->B[i];

        // d_idx->B[i].p
        uint64_t* d_p;
        cudaMalloc((void**)&d_p, sizeof(uint64_t) * h_bucket.n);
        cudaMemcpy(d_p, h_bucket.p, sizeof(uint64_t) * h_bucket.n, cudaMemcpyHostToDevice);
        cudaMemcpy(&(d_B[i].p), &d_p, sizeof(uint64_t*), cudaMemcpyHostToDevice);

        // d_idx->B[i].a.a
        mm128_t* d_a;
        cudaMalloc((void**)&d_a, sizeof(mm128_t) * h_bucket.a.n);
        cudaMemcpy(d_a, h_bucket.a.a, sizeof(mm128_t) * h_bucket.a.n, cudaMemcpyHostToDevice);
        cudaMemcpy(&(d_B[i].a.a), &d_a, sizeof(mm128_t*), cudaMemcpyHostToDevice);

        // d_idx->B[i].h
        idxhash_t* d_h;
        cudaMalloc((void**)&d_h, sizeof(idxhash_t));
        cudaMemcpy(d_h, h_bucket.h, sizeof(idxhash_t), cudaMemcpyHostToDevice);

        idxhash_t* h_hash = (idxhash_t*)h_bucket.h;

        // d_h->flags
        khint32_t* d_flags;
        cudaMalloc((void**)&d_flags, sizeof(khint32_t) * h_hash->n_buckets);
        cudaMemcpy(d_flags, h_hash->flags, sizeof(khint32_t) * h_hash->n_buckets, cudaMemcpyHostToDevice);
        cudaMemcpy(&(d_h->flags), &d_flags, sizeof(khint32_t*), cudaMemcpyHostToDevice);

        // d_h->vals
        uint64_t* d_vals;
        cudaMalloc((void**)&d_vals, sizeof(uint64_t) * h_hash->n_buckets);
        cudaMemcpy(d_vals, h_hash->vals, sizeof(uint64_t) * h_hash->n_buckets, cudaMemcpyHostToDevice);
        cudaMemcpy(&(d_h->vals), &d_vals, sizeof(uint64_t*), cudaMemcpyHostToDevice);

        // d_h->keys
        uint64_t* d_keys;
        cudaMalloc((void**)&d_keys, sizeof(uint64_t) * h_hash->n_buckets);
        cudaMemcpy(d_keys, h_hash->keys, sizeof(uint64_t) * h_hash->n_buckets, cudaMemcpyHostToDevice);
        cudaMemcpy(&(d_h->keys), &d_keys, sizeof(uint64_t*), cudaMemcpyHostToDevice);

        // Update d_B[i].h pointer on the device
        cudaMemcpy(&(d_B[i].h), &d_h, sizeof(idxhash_t*), cudaMemcpyHostToDevice);
    }

    return d_idx;
}

void seeding_kernel_wrapper(mm_idx_t* idx, device_input_batch_t input_batch, device_seed_batch_t seed_batch, device_chain_batch_t chain_batch, int batch_size){

    // For debugging 
    cudaError_t err;
    cudaEvent_t start_seed, stop_seed;
    cudaEvent_t start_sort, stop_sort;
    float seed_t = 0.0; float sort_t = 0.0;

    int* d_rid;
    int* end_ofs;
    cudaMalloc(&end_ofs, sizeof(int)*batch_size);
    
    DEBUG_ONLY(
        cudaEventCreate(&start_seed); cudaEventCreate(&stop_seed); cudaEventRecord(start_seed);
    );

    /* minimizer extraction */
    mm_sketch_device<<<BLOCK_NUM, THREAD_NUM_SEED,0,0>>>(idx, input_batch.seqs, input_batch.lens, input_batch.ofs, d_rid, seed_batch.minimizers, seed_batch.n_minimizers, batch_size);

    cudaDeviceSynchronize();
    DEBUG_ONLY(
        err = cudaGetLastError();
        printf("CUDA sketch Error: %s\n", cudaGetErrorString(err)); 
    );
    
    /* finding ref-query match */
    mm_collect_seed_hits_device<<<BLOCK_NUM_SEED, THREAD_NUM_SEED,0,0>>>(idx, input_batch.opt, seed_batch.minimizers, input_batch.lens, input_batch.ofs, seed_batch.ax, seed_batch.ay, seed_batch.n_minimizers, seed_batch.n_anchors, seed_batch.tmp_seed, seed_batch.anchor_ofs, batch_size, seed_batch.num_anchors, end_ofs);

    cudaDeviceSynchronize();
    DEBUG_ONLY(
        err = cudaGetLastError();
        printf("CUDA match finding Error: %s\n", cudaGetErrorString(err));
    );

    DEBUG_ONLY(
        cudaEventRecord(stop_seed);
        cudaEventSynchronize(stop_seed);
        cudaEventElapsedTime(&seed_t, start_seed, stop_seed);
    );

    int h_num_anchors;
    cudaMemcpy(&h_num_anchors, seed_batch.num_anchors, sizeof(int), cudaMemcpyDeviceToHost);

    /* Sort seeds */
    uint64_t* sorted_ax;
    uint64_t* sorted_ay;
    cudaMalloc(&sorted_ax, sizeof(uint64_t)*(h_num_anchors));
    cudaMalloc(&sorted_ay, sizeof(uint64_t)*(h_num_anchors));

    // Sort : CUB deivce segmented sort
    int  num_items = h_num_anchors;          // e.g., 7
    int  num_segments = batch_size;       // e.g., 3
    int  *d_offsets = seed_batch.anchor_ofs;         // e.g., [0, 3, 3, 7]
    uint64_t  *d_keys_in = seed_batch.ax;         // e.g., [8, 6, 7, 5, 3, 0, 9]
    uint64_t  *d_keys_out = sorted_ax;        // e.g., [-, -, -, -, -, -, -]
    uint64_t  *d_values_in = seed_batch.ay;       // e.g., [0, 1, 2, 3, 4, 5, 6]
    uint64_t  *d_values_out = sorted_ay;      // e.g., [-, -, -, -, -, -, -]

    void     *d_temp_storage = nullptr;
    size_t   temp_storage_bytes = 0;
    cub::DeviceSegmentedRadixSort::SortPairs(
        d_temp_storage, temp_storage_bytes,
        d_keys_in, d_keys_out, d_values_in, d_values_out,
        num_items, num_segments, d_offsets, end_ofs); // d_offsets + 1 -> end offset

    // Allocate temporary storage
    checkCudaErrors(cudaMalloc(&d_temp_storage, temp_storage_bytes));
    cudaEventCreate(&start_sort);
    cudaEventCreate(&stop_sort);

    cudaEventRecord(start_sort);


    // Run sorting operation
    cub::DeviceSegmentedRadixSort::SortPairs(
        d_temp_storage, temp_storage_bytes,
        d_keys_in, d_keys_out, d_values_in, d_values_out,
        num_items, num_segments, d_offsets, end_ofs);


    cudaDeviceSynchronize();

    cudaMemcpy(seed_batch.ax, sorted_ax, sizeof(uint64_t)*(h_num_anchors), cudaMemcpyDeviceToDevice);
    cudaMemcpy(seed_batch.ay, sorted_ay, sizeof(uint64_t)*(h_num_anchors), cudaMemcpyDeviceToDevice);

    cudaDeviceSynchronize();
    cudaFree(sorted_ax);
    cudaFree(sorted_ay);
    cudaFree(d_temp_storage);
    cudaFree(end_ofs);

    DEBUG_ONLY(
        cudaEventRecord(stop_sort);
        cudaEventSynchronize(stop_sort);
        cudaEventElapsedTime(&sort_t, start_sort, stop_sort);
        err = cudaGetLastError();
        printf("CUDA sorting Error: %s\n", cudaGetErrorString(err)); 
        printf("Seeding time: Sketch & find match(%.2f), Sort(%.2f)\n", seed_t, sort_t);
        check_mem();
    );

    return;
}

void chaining_kernel_wrapper_dp(mm_mapopt_t* opt, uint64_t* ax, uint64_t* ay, int* ofs, int* n_a, uint2* range, int* n_range, int32_t* sc, int64_t* p, bool* seg_pass, int seg_num, int batch_size,
                            int* long_range_buf_st, int* long_range_buf_len, int* mid_range_buf_st, int* mid_range_buf_len){
    
    // For debugging
    cudaError_t err;
    cudaEvent_t start_range, stop_range; cudaEvent_t start_chain, stop_chain; cudaEvent_t start_mid, stop_mid; cudaEvent_t start_long, stop_long;
    float range_t = 0.0; float chain_t = 0.0; float mid_t = 0.0; float long_t = 0.0;
    
    int* long_range_num, *mid_range_num;
    cudaMalloc(&long_range_num, sizeof(int));
    cudaMalloc(&mid_range_num, sizeof(int));
    cudaMemset(long_range_num, 0, sizeof(int));
    cudaMemset(mid_range_num, 0, sizeof(int));
    
    /*DP chaining*/
    DEBUG_ONLY(
        cudaEventCreate(&start_range);
        cudaEventCreate(&stop_range);
        cudaEventRecord(start_range);
        err = cudaGetLastError();
        printf("CUDA pre range Error: %s\n", cudaGetErrorString(err));
    );

    /* compute chaining range */
    mm_compute_chain_range<<<BLOCK_NUM_SHORT, 512, 0, 0>>>(opt, ax, n_a, (int2*)range, seg_pass, sc, p, seg_num);

    DEBUG_ONLY(
        cudaDeviceSynchronize();
        err = cudaGetLastError();
        printf("CUDA range Error: %s\n", cudaGetErrorString(err));
        cudaEventRecord(stop_range);
        cudaEventSynchronize(stop_range);
        cudaEventElapsedTime(&range_t, start_range, stop_range);

        cudaEventCreate(&start_chain);
        cudaEventCreate(&stop_chain);
        cudaEventRecord(start_chain);
    );

    /* compute short ranged chains */
    mm_compute_chain_short<<<BLOCK_NUM_SHORT, THREAD_NUM_SHORT,0, 0>>>(sc, p, ax, ay, (uint2*)range, ofs, n_range, n_a, seg_pass, 5000, 5000, 0, 0, 0.120000004768372, 0, seg_num, long_range_num, long_range_buf_st, long_range_buf_len, mid_range_num, mid_range_buf_st, mid_range_buf_len);
    
    DEBUG_ONLY(
        cudaDeviceSynchronize();
        cudaEventRecord(stop_chain);
        cudaEventSynchronize(stop_chain);
        cudaEventElapsedTime(&chain_t, start_chain, stop_chain);
        err = cudaGetLastError();
        printf("CUDA short Error: %s\n", cudaGetErrorString(err));

        cudaEventCreate(&start_mid);
        cudaEventCreate(&stop_mid);
        cudaEventRecord(start_mid);  
    );

    /* compute mid ranged chains */
    int h_mid_range_num; 
    int h_long_range_num;

    cudaMemcpy(&h_mid_range_num, mid_range_num, sizeof(int), cudaMemcpyDeviceToHost);
    cudaMemcpy(&h_long_range_num, long_range_num, sizeof(int), cudaMemcpyDeviceToHost);

    mm_compute_chain_long_tiled<<<BLOCK_NUM_MID, THREAD_NUM_MID, 0, 0>>>(sc, p, ax, ay, mid_range_buf_st, mid_range_buf_len, ofs, n_a, 5000, 5000, 0, 0, 0.120000004768372, 0, h_mid_range_num);

    DEBUG_ONLY(
        cudaDeviceSynchronize();
        err = cudaGetLastError();
        printf("CUDA mid chain Error: %s\n", cudaGetErrorString(err));  
        cudaEventRecord(stop_mid);
        cudaEventSynchronize(stop_mid);
        cudaEventElapsedTime(&mid_t, start_mid, stop_mid);

        cudaEventCreate(&start_long);
        cudaEventCreate(&stop_long);
        cudaEventRecord(start_long);        
    );

    mm_compute_chain_long_tiled<<<BLOCK_NUM, THREAD_NUM_REG, 0, 0>>>(sc, p, ax, ay, long_range_buf_st, long_range_buf_len, ofs, n_a, 5000, 5000, 0, 0, 0.120000004768372, 0, h_long_range_num);

    DEBUG_ONLY(
        cudaDeviceSynchronize();
        err = cudaGetLastError();
        printf("CUDA long chain Error: %s\n", cudaGetErrorString(err));  
        cudaEventRecord(stop_long);
        cudaEventSynchronize(stop_long);
        cudaEventElapsedTime(&long_t, start_long, stop_long);

        printf("Chaining time: Range(%.2f), Short(%.2f), Mid(%.2f), Long(%.2f)\n", range_t, chain_t, mid_t, long_t);
        printf("mid range: %d, long range: %d\n", h_mid_range_num, h_long_range_num);
    );

    cudaFree(long_range_num); cudaFree(mid_range_num);

    return;
}

void chaining_kernel_wrapper_rmq(uint64_t* ax, uint64_t* ay, int* ofs, int* n_a, uint2* range, int* n_range, int32_t* sc, int64_t* p, bool* seg_pass, int seg_num, int batch_size,
                                int* long_range_buf_st, int* long_range_buf_len, int* mid_range_buf_st, int* mid_range_buf_len, int h_num_anchors){
    
    
    // For debugging
    cudaError_t err;
    cudaEvent_t start_range, stop_range; cudaEvent_t start_sort, stop_sort; cudaEvent_t start_chain, stop_chain; cudaEvent_t start_mid, stop_mid; cudaEvent_t start_long, stop_long;
    float sort_t = 0.0; float range_t = 0.0; float chain_t = 0.0; float mid_t = 0.0; float long_t = 0.0;
    
    int* long_range_num, *mid_range_num;
    cudaMalloc(&long_range_num, sizeof(int));
    cudaMalloc(&mid_range_num, sizeof(int));
    cudaMemset(long_range_num, 0, sizeof(int));
    cudaMemset(mid_range_num, 0, sizeof(int));
    
    /*RMQ chaining*/

    DEBUG_ONLY(
        cudaEventCreate(&start_sort);
        cudaEventCreate(&stop_sort);
        cudaEventRecord(start_sort);
    );

    int* end_ofs;
    cudaMalloc(&end_ofs, sizeof(int)*batch_size);

    //kernel to compute end ofs;
    compute_end_ofs<<<BLOCK_NUM_SHORT, THREAD_NUM_SHORT, 0, 0>>>(ofs, n_a, end_ofs, batch_size);
    
    cudaDeviceSynchronize();
    thrust::device_ptr<int> dev_ptr(end_ofs);
    auto iter = thrust::max_element(dev_ptr, dev_ptr + batch_size);
    int h_n_items = *iter;

    /* Sort seeds */
    uint64_t* sorted_ax;
    uint64_t* sorted_ay;
    cudaMalloc(&sorted_ax, sizeof(uint64_t)*(h_num_anchors));
    cudaMalloc(&sorted_ay, sizeof(uint64_t)*(h_num_anchors));
    cudaMemcpy(sorted_ax, ax, sizeof(uint64_t)*(h_num_anchors), cudaMemcpyDeviceToDevice);
    cudaMemcpy(sorted_ay, ay, sizeof(uint64_t)*(h_num_anchors), cudaMemcpyDeviceToDevice);

    // Sort : CUB deivce segmented sort
    int  num_items = h_n_items;          
    int  num_segments = batch_size;       
    int  *d_offsets = ofs;         
    uint64_t  *d_keys_in = ax ;         
    uint64_t  *d_keys_out = sorted_ax;       
    uint64_t  *d_values_in = ay;      
    uint64_t  *d_values_out = sorted_ay;      

    void     *d_temp_storage = nullptr;
    size_t   temp_storage_bytes = 0;
    cub::DeviceSegmentedRadixSort::SortPairs(
        d_temp_storage, temp_storage_bytes,
        d_keys_in, d_keys_out, d_values_in, d_values_out,
        num_items, num_segments, d_offsets, end_ofs); // d_offsets + 1 -> end offset

    // Allocate temporary storage
    checkCudaErrors(cudaMalloc(&d_temp_storage, temp_storage_bytes));

    // Run sorting operation
    cub::DeviceSegmentedRadixSort::SortPairs(
        d_temp_storage, temp_storage_bytes,
        d_keys_in, d_keys_out, d_values_in, d_values_out,
        num_items, num_segments, d_offsets, end_ofs);

    cudaDeviceSynchronize();

    cudaMemcpy(ax, sorted_ax, sizeof(uint64_t)*(h_num_anchors), cudaMemcpyDeviceToDevice);
    cudaMemcpy(ay, sorted_ay, sizeof(uint64_t)*(h_num_anchors), cudaMemcpyDeviceToDevice);

    cudaDeviceSynchronize();
    cudaFree(sorted_ax);
    cudaFree(sorted_ay);
    cudaFree(d_temp_storage);
    cudaFree(end_ofs);

    DEBUG_ONLY(
        cudaDeviceSynchronize();
        err = cudaGetLastError();
        printf("CUDA sort Error: %s\n", cudaGetErrorString(err));
        cudaEventRecord(stop_sort);
        cudaEventSynchronize(stop_sort);
        cudaEventElapsedTime(&sort_t, start_sort, stop_sort);

        cudaEventCreate(&start_range);
        cudaEventCreate(&stop_range);
        cudaEventRecord(start_range);
    );
    
    mm_compute_chain_range_rmq<<<BLOCK_NUM, 32, 0, 0>>>(ax, ofs, n_a, range, n_range, 20000, batch_size);
    //range_inexact_segment_block<<<BLOCK_NUM_SHORT, 512, 0, 0>>>(ax, n_a, (int2*)range, seg_pass, 20000, seg_num, sc, p);
    
    DEBUG_ONLY(
        cudaDeviceSynchronize();
        err = cudaGetLastError();
        printf("CUDA range Error: %s\n", cudaGetErrorString(err));
        cudaEventRecord(stop_range);
        cudaEventSynchronize(stop_range);
        cudaEventElapsedTime(&range_t, start_range, stop_range);

        cudaEventCreate(&start_chain);
        cudaEventCreate(&stop_chain);
        cudaEventRecord(start_chain);
    );
    /* compute short ranged chains */
    
    //chain_segmented_short_rmq<<<BLOCK_NUM_SHORT, THREAD_NUM_SHORT,0, 0>>>(sc, p, ax, ay, range, ofs, n_range, n_a, seg_pass, 20000, 20000, 0, 0, 0.120000004768372, 0, seg_num, long_range_num, long_range_buf_st, long_range_buf_len, mid_range_num, mid_range_buf_st, mid_range_buf_len);
    mm_compute_chain_short_rmq<<<BLOCK_NUM_SHORT, THREAD_NUM_SHORT,0, 0>>>(sc, p, ax, ay, range, ofs, n_range, n_a, 20000, 20000, 0, 0, 0.120000004768372, 0, batch_size, long_range_num, long_range_buf_st, long_range_buf_len, mid_range_num, mid_range_buf_st, mid_range_buf_len);
            
    DEBUG_ONLY(
        cudaEventRecord(stop_chain);
        cudaEventSynchronize(stop_chain);
        float chain_t = 0.0;
        cudaEventElapsedTime(&chain_t, start_chain, stop_chain);

        cudaDeviceSynchronize();
        err = cudaGetLastError();
        printf("CUDA short Error: %s\n", cudaGetErrorString(err));
    );

    int h_mid_range_num; 
    int h_long_range_num;

    cudaMemcpy(&h_mid_range_num, mid_range_num, sizeof(int), cudaMemcpyDeviceToHost);
    cudaMemcpy(&h_long_range_num, long_range_num, sizeof(int), cudaMemcpyDeviceToHost);

    // thrust::device_ptr<int32_t> lr_st(long_range_buf_st);
    // thrust::device_ptr<int32_t> lr_len(long_range_buf_len);

    // thrust::stable_sort_by_key(thrust::device, lr_len, lr_len + (h_long_range_num), lr_st, thrust::greater<int>());
            

    DEBUG_ONLY(
        cudaEventCreate(&start_mid);
        cudaEventCreate(&stop_mid);
        cudaEventRecord(start_mid);  
    );
    
    /* compute mid ranged chains */
    //mm_chain_compute_long_tiled_rmq_double<<<BLOCK_NUM_MID, THREAD_NUM_MID, 0, 0>>>(sc, p, ax, ay, mid_range_buf_st, mid_range_buf_len, ofs, n_a, 20000, 20000, 0, 0, 0.120000004768372, 0, h_mid_range_num);
    mm_chain_compute_long_tiled_rmq<<<BLOCK_NUM_MID, THREAD_NUM_MID, 0, 0>>>(sc, p, ax, ay, mid_range_buf_st, mid_range_buf_len, ofs, n_a, 20000, 20000, 0, 0, 0.120000004768372, 0, h_mid_range_num);
    
    DEBUG_ONLY(
        cudaDeviceSynchronize();
        err = cudaGetLastError();
        printf("CUDA mid chain Error: %s\n", cudaGetErrorString(err));  
        cudaEventRecord(stop_mid);
        cudaEventSynchronize(stop_mid);
        cudaEventElapsedTime(&mid_t, start_mid, stop_mid);

        cudaEventCreate(&start_long);
        cudaEventCreate(&stop_long);
        cudaEventRecord(start_long);     
    );   
            

    //mm_chain_compute_long_tiled_rmq_double<<<BLOCK_NUM, THREAD_NUM, 0, 0>>>(sc, p, ax, ay, long_range_buf_st, long_range_buf_len, ofs, n_a, 20000, 20000, 0, 0, 0.120000004768372, 0, h_long_range_num);
    mm_chain_compute_long_tiled_rmq<<<BLOCK_NUM, THREAD_NUM, 0, 0>>>(sc, p, ax, ay, long_range_buf_st, long_range_buf_len, ofs, n_a, 20000, 20000, 0, 0, 0.120000004768372, 0, h_long_range_num);

    DEBUG_ONLY(
        cudaDeviceSynchronize();
        err = cudaGetLastError();
        printf("CUDA long chain Error: %s\n", cudaGetErrorString(err));  
        cudaEventRecord(stop_long);

        cudaEventSynchronize(stop_long);
        float long_t = 0.0;
        cudaEventElapsedTime(&long_t, start_long, stop_long);

        printf("RMQ Chaining time: Range(%.2f), Short(%.2f), Mid(%.2f), Long(%.2f)\n", range_t, chain_t, mid_t, long_t);
        printf("mid range: %d, long range: %d\n", h_mid_range_num, h_long_range_num);
    );

    cudaFree(long_range_num); cudaFree(mid_range_num);

    return;
}

void chaining_kernel_wrapper(device_input_batch_t input_batch, device_seed_batch_t seed_batch, device_chain_batch_t chain_batch, device_extend_batch_t extend_batch,mm_idx_t* d_idx, int segLength,
                            int seqLength, int ofsLength, int h_num_anchors, int batch_size){
    
    // For debugging 
    cudaError_t err;
    cudaEvent_t start_post, stop_post; cudaEvent_t start_rmq, stop_rmq; cudaEvent_t start_bt2, stop_bt2; cudaEvent_t start_filter, stop_filter; 
    float post_t = 0.0; float rmq_t = 0.0; float bt2_t = 0.0; float filter_t = 0.0;
    
    /* Chaining */
    chaining_kernel_wrapper_dp(input_batch.opt, seed_batch.ax, seed_batch.ay, seed_batch.anchor_ofs, seed_batch.n_anchors, chain_batch.range, chain_batch.n_range, 
                            chain_batch.sc, chain_batch.p, chain_batch.seg_pass, segLength, batch_size,
                            chain_batch.long_range_buf_st, chain_batch.long_range_buf_len, chain_batch.mid_range_buf_st, chain_batch.mid_range_buf_len);
   
    cudaDeviceSynchronize();
    int min_cnt = 3;
    int min_sc = 40;
    int max_drop = 0;
    int max_drop_rmq = 20000;
    int is_qstrand = 0;

    DEBUG_ONLY(
        cudaEventCreate(&start_post);
        cudaEventCreate(&stop_post);
        cudaEventRecord(start_post);
    );

    mm_reg1_t* d_reg;

    mm_chain_backtrack_kernels(seed_batch.n_anchors, seed_batch.ax, seed_batch.ay, chain_batch.sc, chain_batch.p,
                                chain_batch.u, chain_batch.zx, chain_batch.zy, chain_batch.t, chain_batch.v, seed_batch.anchor_ofs,  input_batch.ofs,
                                min_cnt, min_sc, max_drop, d_reg, input_batch.lens, is_qstrand, chain_batch.n_chain, chain_batch.chain, batch_size, chain_batch.n_v, chain_batch.n_u, chain_batch.dropped, false, h_num_anchors, batch_size);
    
    cudaDeviceSynchronize();
    
    DEBUG_ONLY(
        cudaEventRecord(stop_post);
        cudaEventSynchronize(stop_post);
        cudaEventElapsedTime(&post_t, start_post, stop_post);

        err = cudaGetLastError();
        printf("CUDA backtrack Error: %s (%.2f)\n", cudaGetErrorString(err), post_t);  
        printf("after bt: "); check_mem();
    );


    int max_dist = 5000; // FIXME: different parameter for rmq?
    int max_dist_inner = 1000;
    int bw = 20000;
    int max_chn_skip = 5000;
    int cap_rmq_size = 10000;
    float chn_pen_gap = 0.12;
    float chn_pen_skip = 0.0;
    int max_rmq_size = 5000;

    DEBUG_ONLY(
        cudaEventCreate(&start_rmq);
        cudaEventCreate(&stop_rmq);
        cudaEventRecord(start_rmq);
    );

    chaining_kernel_wrapper_rmq(seed_batch.ax, seed_batch.ay, seed_batch.anchor_ofs, chain_batch.n_v, chain_batch.range, chain_batch.n_range, chain_batch.sc, chain_batch.p, chain_batch.seg_pass, segLength, batch_size,
                            chain_batch.long_range_buf_st, chain_batch.long_range_buf_len, chain_batch.mid_range_buf_st, chain_batch.mid_range_buf_len, h_num_anchors);

    cudaDeviceSynchronize();
    
    DEBUG_ONLY(
        cudaEventRecord(stop_rmq);
        cudaEventSynchronize(stop_rmq);
        cudaEventElapsedTime(&rmq_t, start_rmq, stop_rmq);
        err = cudaGetLastError();
        printf("CUDA rmq Error: %s (%.2f)\n", cudaGetErrorString(err), rmq_t);  
        printf("after rmq: "); check_mem();
    );

    cudaDeviceSynchronize();

    DEBUG_ONLY(
        cudaEventCreate(&start_bt2);
        cudaEventCreate(&stop_bt2);
        cudaEventRecord(start_bt2);
    );
   
    mm_chain_backtrack_kernels(chain_batch.n_v, seed_batch.ax, seed_batch.ay, chain_batch.sc, chain_batch.p, 
                                                              chain_batch.u, chain_batch.zx, chain_batch.zy, chain_batch.t, chain_batch.v, seed_batch.anchor_ofs, input_batch.ofs,
                                                              min_cnt, min_sc, max_drop_rmq, d_reg, input_batch.lens, is_qstrand, chain_batch.n_chain, chain_batch.chain, batch_size, seed_batch.n_anchors, chain_batch.n_u, chain_batch.dropped, true, h_num_anchors, batch_size);
   
    cudaDeviceSynchronize();

    DEBUG_ONLY(
        cudaEventRecord(stop_bt2);
        cudaEventSynchronize(stop_bt2);
        cudaEventElapsedTime(&bt2_t, start_bt2, stop_bt2);

        err = cudaGetLastError();
        printf("CUDA bt2 Error: %s (%.2f)\n", cudaGetErrorString(err), bt2_t);
        printf("after bt2: "); check_mem();
    );

    
    DEBUG_ONLY(
        cudaEventCreate(&start_filter); 
        cudaEventCreate(&stop_filter);
        cudaEventRecord(start_filter);
    );

    mm_chain_post<<<BLOCK_NUM_SHORT, THREAD_NUM_SHORT, 0, 0>>>(10000, input_batch.lens, seed_batch.n_anchors, chain_batch.n_v, seed_batch.anchor_ofs, chain_batch.chain, 
                                                            seed_batch.ax, seed_batch.ay, chain_batch.dropped, chain_batch.t, (uint64_t*)chain_batch.zx, batch_size);

    cudaDeviceSynchronize(); 

    mm_squeeze<<<BLOCK_NUM_SHORT, THREAD_NUM_SHORT, 0, 0>>>(seed_batch.n_anchors, chain_batch.n_v, seed_batch.anchor_ofs, chain_batch.chain, seed_batch.ax, seed_batch.ay, chain_batch.dropped, (uint64_t*)chain_batch.zx, batch_size);

    DEBUG_ONLY(
        cudaEventRecord(stop_filter); cudaEventSynchronize(stop_filter);
        cudaEventElapsedTime(&filter_t, start_filter, stop_filter); 
        err = cudaGetLastError();
        printf("CUDA filter Error: %s (%.2f)\n", cudaGetErrorString(err), filter_t);  
        check_mem();
    );

    return;

}

void extending_kernel_wrapper(mm_idx_t* d_idx, device_input_batch_t input_batch, device_seed_batch_t seed_batch, device_chain_batch_t chain_batch, device_extend_batch_t extend_batch, int max_ext_len){
    /* Extending */

    cudaError_t err;
    cudaEvent_t start_preal, stop_preal; cudaEvent_t start_al, stop_al; cudaEvent_t start_al1, stop_al1;
    float preal_t = 0.0; float align_t1 = 0.0; float align_t = 0.0;

    gasal_res_t *device_res_second; // not used
    uint4 *packed_tb_matrices; // not used also
    short2 *global_buffer_top;
    
    int agatha_block_num = 256; // TODO: parameterize
    int agatha_thread_num = 256;

    int max_qlen = max_ext_len; 
    int max_rlen = max_ext_len;
   
    DEBUG_ONLY(
        cudaEventCreate(&start_preal); 
        cudaEventCreate(&stop_preal); 
        cudaEventRecord(start_preal);
    );

    int h_n_chain;
    cudaMemcpy(&h_n_chain, chain_batch.n_chain, sizeof(int), cudaMemcpyDeviceToHost);
 
    fill_align_info<<<BLOCK_NUM_SHORT , THREAD_NUM_SHORT, 0, 0>>>
                    (d_idx, seed_batch.ax, seed_batch.ay, chain_batch.chain, // reference index, anchors, chain metadata
					(uint8_t*)input_batch.seqs, extend_batch.qseqs, extend_batch.rseqs, // original qseq, target qseq, target rseq
					extend_batch.q_lens, extend_batch.r_lens, extend_batch.q_ofs, extend_batch.r_ofs, // alignment information for AGATHA kernel
					h_n_chain, max_qlen, max_rlen, chain_batch.dropped, extend_batch.zdropped, 0, extend_batch.n_extend); // extra informations

    cudaDeviceSynchronize();
    int h_n_extend;
    cudaMemcpyAsync(&h_n_extend, extend_batch.n_extend, sizeof(int), cudaMemcpyDeviceToHost);

    DEBUG_ONLY(
        err = cudaGetLastError();
        printf("CUDA fill align info Error: %s\n", cudaGetErrorString(err));  
        printf("number of gap filling alignments: %d\n", h_n_extend);
    );
    
    int h_align_total = h_n_extend + (h_n_chain) *2;
    int query_batch_tasks_per_thread = max_qlen * ((int)h_align_total)/(THREAD_NUM * 4);
    int target_batch_tasks_per_thread = max_rlen * ((int)h_align_total)/(THREAD_NUM * 4);
    uint32_t total_query_batch_regs = max_qlen * ((int)h_align_total)/4;
    uint32_t total_target_batch_regs = max_rlen * ((int)h_align_total)/4;

    gasal_pack_kernel<<<BLOCK_NUM,THREAD_NUM>>>((uint32_t*)extend_batch.qseqs, (uint32_t*)extend_batch.rseqs, extend_batch.d_qseqs_packed, extend_batch.d_rseqs_packed, 
                    query_batch_tasks_per_thread, target_batch_tasks_per_thread, total_query_batch_regs, total_target_batch_regs); 

    cudaDeviceSynchronize();

    /* offset setting draft - compute on CPU */
    int num_sequences = h_n_extend;
    int base_offset = h_n_chain*2;
    uint64_t max_batch_size = TB_BUF_SIZE * max_qlen * max_rlen / DBLOCK_SIZE;

    int* h_qseq_lens = new int[num_sequences]();
    int* h_rseq_lens = new int[num_sequences]();
    uint64_t* h_offsets = new uint64_t[num_sequences]();
    int* h_batch_sizes = new int[num_sequences](); // alloc by gap alignment number
    int h_num_batches = 0;

    int slice_width = 3;
    int actual_n_alns_left = h_n_chain;
    int actual_n_alns_right = h_n_chain;
    int actual_n_alns_gap = h_n_extend;

    int smem_size = (agatha_thread_num/32)*((32*(8*(slice_width+1)))+28)*sizeof(int32_t);

    cudaMemcpy(h_rseq_lens, extend_batch.r_lens + base_offset, num_sequences * sizeof(int), cudaMemcpyDeviceToHost);
    cudaMemcpy(h_qseq_lens, extend_batch.q_lens + base_offset, num_sequences * sizeof(int), cudaMemcpyDeviceToHost);

    compute_grid_offset(h_qseq_lens, h_rseq_lens, num_sequences, max_batch_size, h_offsets, h_batch_sizes, h_num_batches);

    cudaMemcpy(extend_batch.tb_buf_ofs + base_offset, h_offsets, num_sequences * sizeof(uint64_t), cudaMemcpyHostToDevice);
    cudaDeviceSynchronize();

    DEBUG_ONLY(
        cudaEventRecord(stop_preal); cudaEventSynchronize(stop_preal); 
        cudaEventElapsedTime(&preal_t, start_preal, stop_preal);
        std::cout << " pre-alignment time : " << preal_t << " ms " << std::endl;

        std::cout<< "\nmax buffer size: "<<max_batch_size;
        std::cout << "\nBatch sizes: ";
        for (int i = 0; i < h_num_batches; ++i) {
            std::cout << h_batch_sizes[i] << " ";
        }
        std::cout << "\nNumber of batches: " << h_num_batches << std::endl;
        std::cout << "\nbaseoffset: " << base_offset << std::endl;
    );

    DEBUG_ONLY(
        cudaEventCreate(&start_al); cudaEventCreate(&stop_al);
        cudaEventCreate(&start_al1); cudaEventCreate(&stop_al1);
    );

    ////////////////// Initial alignment - left extension //////////////////// 

    DEBUG_ONLY(
        err = cudaGetLastError();
        printf("Compute offset error: %s\n", cudaGetErrorString(err)); 
        cudaEventRecord(start_al1);
    );

    // gap left extension
    bool simd_approx = true;
    int simd_ext_batch_size = max_batch_size * 4 / (16 * max_ext_len);

    int lr_batch_ofs = 0;
    int lr_batch_size = min(simd_ext_batch_size, h_n_chain);

    for(lr_batch_ofs = 0; lr_batch_ofs < actual_n_alns_left; lr_batch_ofs += lr_batch_size) {
        
        if(lr_batch_ofs + lr_batch_size > actual_n_alns_left) {
            lr_batch_size = actual_n_alns_left - lr_batch_ofs;
        }

        if(simd_approx){
            // gap left extension
            mm2_kswz_extension_simd<<<(lr_batch_size / 32) + 1, 32, 0, 0>>>((char*)extend_batch.qseqs, (char*)extend_batch.rseqs, extend_batch.q_lens + lr_batch_ofs, extend_batch.r_lens + lr_batch_ofs, extend_batch.q_ofs + lr_batch_ofs, extend_batch.r_ofs + lr_batch_ofs,
                (uint8_t*)extend_batch.tb_matrices, extend_batch.device_res, extend_batch.n_cigar + lr_batch_ofs, extend_batch.cigar, lr_batch_size, max_qlen, true);
            // gap right extension
            mm2_kswz_extension_simd<<<(lr_batch_size / 32) + 1, 32, 0, 0>>>((char*)extend_batch.qseqs, (char*)extend_batch.rseqs, extend_batch.q_lens + h_n_chain + lr_batch_ofs, extend_batch.r_lens + h_n_chain + lr_batch_ofs, extend_batch.q_ofs + h_n_chain + lr_batch_ofs, extend_batch.r_ofs + h_n_chain + lr_batch_ofs,
                (uint8_t*)extend_batch.tb_matrices, extend_batch.device_res, extend_batch.n_cigar + h_n_chain + lr_batch_ofs, extend_batch.cigar, lr_batch_size, max_qlen, false);
        }else{
            // gap left extension
            mm2_kswz_extension<<<(lr_batch_size / 32) + 1, 32, 0, 0>>>((char*)extend_batch.qseqs, (char*)extend_batch.rseqs, extend_batch.q_lens + lr_batch_ofs, extend_batch.r_lens + lr_batch_ofs, extend_batch.q_ofs + lr_batch_ofs, extend_batch.r_ofs + lr_batch_ofs,
                extend_batch.tb_matrices, extend_batch.device_res, extend_batch.n_cigar + lr_batch_ofs, extend_batch.cigar, lr_batch_size, max_qlen, true);
            // gap right extension
            mm2_kswz_extension<<<(lr_batch_size / 32) + 1, 32, 0, 0>>>((char*)extend_batch.qseqs, (char*)extend_batch.rseqs, extend_batch.q_lens + h_n_chain + lr_batch_ofs, extend_batch.r_lens + h_n_chain + lr_batch_ofs, extend_batch.q_ofs + h_n_chain + lr_batch_ofs, extend_batch.r_ofs + h_n_chain + lr_batch_ofs,
                extend_batch.tb_matrices, extend_batch.device_res, extend_batch.n_cigar + h_n_chain + lr_batch_ofs, extend_batch.cigar, lr_batch_size, max_qlen, false);
        }
    }

    DEBUG_ONLY(
        cudaEventRecord(stop_al1); 
        cudaEventSynchronize(stop_al1); 
        cudaEventElapsedTime(&align_t1, start_al1, stop_al1);
        std::cout << " Gap left/right extension time : " << align_t1 << " ms " << std::endl;

        cudaDeviceSynchronize(); 
        err = cudaGetLastError(); 
        printf("mm2 gap ext Error: %s\n", cudaGetErrorString(err));
        cudaEventRecord(start_al);

        printf("actual n alns left: %d n chain : %d acutall n alns gap: %d n align %d\n\n", actual_n_alns_left, h_n_chain, actual_n_alns_gap, h_n_extend);
    );

    int batch_ofs = 0;
    for(int batch_idx = 0; batch_idx < h_num_batches; batch_idx++) {
        int gap_ext_batch_size = h_batch_sizes[batch_idx];

        DEBUG_ONLY(
            if(batch_ofs + gap_ext_batch_size > actual_n_alns_gap) gap_ext_batch_size = actual_n_alns_gap - batch_ofs;
            printf("tb batch size: %d\n", gap_ext_batch_size);
        );
    
        agatha_sort<<<agatha_block_num, agatha_thread_num, 0, 0>>>
        (extend_batch.d_qseqs_packed, extend_batch.d_rseqs_packed, 
        &(extend_batch.q_lens[base_offset+batch_ofs]), &(extend_batch.r_lens[base_offset+batch_ofs]), extend_batch.q_ofs+(actual_n_alns_left*2+batch_ofs), extend_batch.r_ofs+(actual_n_alns_left*2+batch_ofs), gap_ext_batch_size, max_qlen, 
        extend_batch.global_buffer_top); 

        DEBUG_ONLY(
            cudaDeviceSynchronize(); 
            err = cudaGetLastError();
            printf("CUDA gap fill sort Error: %s\n", cudaGetErrorString(err)); 
        );
        
        agatha_kernel_approx_gridded_tb<<<agatha_block_num, agatha_thread_num, smem_size, 0>>>
        (extend_batch.d_qseqs_packed, extend_batch.d_rseqs_packed,
        extend_batch.q_lens+(actual_n_alns_left*2 + batch_ofs), extend_batch.r_lens+(actual_n_alns_left*2 + batch_ofs), extend_batch.q_ofs+(actual_n_alns_left*2 + batch_ofs), extend_batch.r_ofs+(actual_n_alns_left*2 + batch_ofs), extend_batch.device_res, device_res_second,
        extend_batch.dblock_row, extend_batch.dblock_col, gap_ext_batch_size, max_qlen, extend_batch.global_buffer_top, extend_batch.tb_buf_ofs + (actual_n_alns_left*2 + batch_ofs), extend_batch.zdropped, BW_GAPFILL); 
        
        DEBUG_ONLY(
            cudaDeviceSynchronize(); err = cudaGetLastError(); printf("agatha gap fill forward Error: %s\n", cudaGetErrorString(err));
        );

        int tb_threadnum = 64;
        traceback_kernel_gridded<<<gap_ext_batch_size/tb_threadnum + 1, tb_threadnum, tb_threadnum*DBLOCK_SIZE*6, 0>>>
        ((uint8_t*)(extend_batch.qseqs), (uint8_t*)(extend_batch.rseqs), 
        extend_batch.q_lens+(actual_n_alns_left*2 + batch_ofs), extend_batch.r_lens+(actual_n_alns_left*2 + batch_ofs), extend_batch.q_ofs+(actual_n_alns_left*2 + batch_ofs), extend_batch.r_ofs+(actual_n_alns_left*2 + batch_ofs),
        extend_batch.cigar, extend_batch.device_res, gap_ext_batch_size, max_qlen, extend_batch.dblock_row, extend_batch.dblock_col, extend_batch.tb_buf_ofs + (actual_n_alns_left*2 + batch_ofs), extend_batch.n_cigar + (actual_n_alns_left*2 + batch_ofs), BW_GAPFILL);

        DEBUG_ONLY(
            cudaDeviceSynchronize(); err = cudaGetLastError(); printf("agatha gap fill traceback error: %s\n", cudaGetErrorString(err));
        );

        batch_ofs += gap_ext_batch_size;

        DEBUG_ONLY(
        cudaEventRecord(stop_al); cudaEventSynchronize(stop_al); float align_t = 0.0;
        cudaEventElapsedTime(&align_t, start_al, stop_al); std::cout << " gap filling time : " << align_t << " ms " << std::endl;;
        );
    }

    DEBUG_ONLY(
        cudaDeviceSynchronize(); err = cudaGetLastError(); printf("agatha gap align Error: %s\n", cudaGetErrorString(err));

        cudaEventRecord(stop_al); cudaEventSynchronize(stop_al); 
        cudaEventElapsedTime(&align_t, start_al, stop_al); std::cout << " gap filling time : " << align_t << " ms " << std::endl;
    );

    delete[] h_qseq_lens;  
    delete[] h_rseq_lens;   
    delete[] h_offsets;    
    delete[] h_batch_sizes; 

    return;    
}


void mm_chain_backtrack_kernels(int* n_a, uint64_t* g_ax, uint64_t* g_ay, int32_t* g_sc, int64_t *g_p, uint64_t* g_u, 
    int64_t* g_zx, int64_t* g_zy, int32_t* g_t, int64_t* g_v,
    int* offset, int* r_offset, int32_t min_cnt, int32_t min_sc, int32_t max_drop,
    mm_reg1_t *g_r, int* d_len, int is_qstrand, int* n_chain, mm_chain_t* g_c, int n_task, int* g_n_v, int* g_n_u, bool* dropped, bool final, int anchorLength, int batch_size){

    // For debugging 
    struct timespec start, end;

    DEBUG_ONLY(
        clock_gettime(CLOCK_BOOTTIME, &start);
    );

    int64_t *bufx, *bufy;
    cudaMalloc(&bufx, sizeof(uint64_t) * anchorLength);
    cudaMalloc(&bufy, sizeof(uint64_t) * anchorLength);
    int* ofs_end;
    cudaMalloc(&ofs_end, sizeof(int)*batch_size);
    int* num_elements;
    cudaMalloc(&num_elements, sizeof(int)*batch_size);

    DEBUG_ONLY(
        clock_gettime(CLOCK_BOOTTIME, &end);
        printf(" ***** cuda kernel took %f seconds for inital malloc\n",
            ( end.tv_sec - start.tv_sec ) + ( end.tv_nsec - start.tv_nsec ) / 1E9);
        clock_gettime(CLOCK_BOOTTIME, &start);
    );

    mm_filter_anchors<<<BLOCK_NUM_SHORT, THREAD_NUM_SHORT, 0, 0>>>(n_a, offset, min_sc, g_sc, g_zx, g_zy, 
                ofs_end, g_t, num_elements, g_n_v, n_task); // filter out anchors by score

    cudaDeviceSynchronize();

    DEBUG_ONLY(
        clock_gettime(CLOCK_BOOTTIME, &end);
        printf(" ***** cuda kernel took %f seconds to filter anchors\n",
        ( end.tv_sec - start.tv_sec ) + ( end.tv_nsec - start.tv_nsec ) / 1E9);
        clock_gettime(CLOCK_BOOTTIME, &start);
        check_mem();
    );

    // Sort : CUB deivce segmented sort
    int  num_items = anchorLength;          // e.g., 7
    int  num_segments = batch_size;       // e.g., 3
    int  *d_offsets = offset;         // e.g., [0, 3, 3, 7]
    int64_t  *d_keys_in = g_zx ;         // e.g., [8, 6, 7, 5, 3, 0, 9]
    int64_t  *d_keys_out = bufx;        // e.g., [-, -, -, -, -, -, -]
    int64_t  *d_values_in = g_zy;       // e.g., [0, 1, 2, 3, 4, 5, 6]
    int64_t  *d_values_out = bufy;      // e.g., [-, -, -, -, -, -, -]

    void     *d_temp_storage = nullptr;
    size_t   temp_storage_bytes = 0;
    cub::DeviceSegmentedRadixSort::SortPairs(
    d_temp_storage, temp_storage_bytes,
    d_keys_in, d_keys_out, d_values_in, d_values_out,
    num_items, num_segments, d_offsets, ofs_end); // d_offsets + 1 -> end offset

    // Allocate temporary storage
    checkCudaErrors(cudaMalloc(&d_temp_storage, temp_storage_bytes));

    // Run sorting operation
    cub::DeviceSegmentedRadixSort::SortPairs(
    d_temp_storage, temp_storage_bytes,
    d_keys_in, d_keys_out, d_values_in, d_values_out,
    num_items, num_segments, d_offsets, ofs_end);

    cudaDeviceSynchronize();

    DEBUG_ONLY(
        clock_gettime(CLOCK_BOOTTIME, &end);
        printf(" ***** cuda kernel took %f seconds to sort1\n",
        ( end.tv_sec - start.tv_sec ) + ( end.tv_nsec - start.tv_nsec ) / 1E9);
        clock_gettime(CLOCK_BOOTTIME, &start);
        check_mem();
    );

    mm_chain_backtrack_parallel<<<BLOCK_NUM_SHORT, THREAD_NUM_SHORT, 0, 0>>>(n_a, g_ax, g_ay, g_sc, g_p, g_u, 
        bufx, bufy, g_t, g_v,
        offset, min_cnt, min_sc, max_drop,
        n_task, g_n_v, g_n_u, num_elements, ofs_end); // backtrack filterd anchors

    cudaDeviceSynchronize();
    
    DEBUG_ONLY(
        clock_gettime(CLOCK_BOOTTIME, &end);
        printf(" ***** cuda kernel took %f seconds to backtrack\n",
        ( end.tv_sec - start.tv_sec ) + ( end.tv_nsec - start.tv_nsec ) / 1E9);
        clock_gettime(CLOCK_BOOTTIME, &start);
        check_mem();
    );

    num_items = anchorLength;          // e.g., 7
    num_segments = batch_size;       // e.g., 3
    d_offsets = offset;         // e.g., [0, 3, 3, 7]
    d_keys_in = g_p ;         // e.g., [8, 6, 7, 5, 3, 0, 9]
    d_keys_out = g_zx;        // e.g., [-, -, -, -, -, -, -]
    d_values_in = g_v;       // e.g., [0, 1, 2, 3, 4, 5, 6]
    d_values_out = g_zy;      // e.g., [-, -, -, -, -, -, -]

    // sort chain by ref pos
    cub::DeviceSegmentedRadixSort::SortPairs(
    d_temp_storage, temp_storage_bytes,
    (uint64_t*)d_keys_in, (uint64_t*)d_keys_out, (uint64_t*)d_values_in, (uint64_t*)d_values_out,
    num_items, num_segments, d_offsets, ofs_end);

    cudaDeviceSynchronize();

    DEBUG_ONLY(
        clock_gettime(CLOCK_BOOTTIME, &end);
        printf(" ***** cuda kernel took %f seconds to sort2\n",
        ( end.tv_sec - start.tv_sec ) + ( end.tv_nsec - start.tv_nsec ) / 1E9);
        clock_gettime(CLOCK_BOOTTIME, &start);
        check_mem();
    );

    mm_set_chain<<<BLOCK_NUM_SHORT, THREAD_NUM_SHORT, 0, 0>>>(n_a, n_task,  g_ax, g_ay, offset, ofs_end, g_sc, g_zx, g_u, 
        bufx, bufy, g_t, g_zy); // set anchor information within chain

    cudaDeviceSynchronize();

    DEBUG_ONLY(
        clock_gettime(CLOCK_BOOTTIME, &end);
        printf(" ***** cuda kernel took %f seconds to set anchors\n",
        ( end.tv_sec - start.tv_sec ) + ( end.tv_nsec - start.tv_nsec ) / 1E9);
        clock_gettime(CLOCK_BOOTTIME, &start);
        check_mem();
    );

    // generate chain data 
    if(final){

        mm_gen_regs1<<<BLOCK_NUM_SHORT, THREAD_NUM_SHORT, 0, 0>>>(n_a, g_ax, g_ay, g_u, 
            bufx, bufy, g_zy, g_n_v, ofs_end, 
            offset, r_offset, d_len, is_qstrand, n_chain, g_c, n_task, dropped);

        num_items = anchorLength;          // e.g., 7
        num_segments = batch_size;       // e.g., 3
        d_offsets = offset;         // e.g., [0, 3, 3, 7]
        d_keys_in = bufx;         // e.g., [8, 6, 7, 5, 3, 0, 9]
        d_keys_out = g_zx;        // e.g., [-, -, -, -, -, -, -]
        d_values_in = bufy;       // e.g., [0, 1, 2, 3, 4, 5, 6]
        d_values_out = g_zy;      // e.g., [-, -, -, -, -, -, -]

        // sort chain by ref pos
        cub::DeviceSegmentedRadixSort::SortPairsDescending(
            d_temp_storage, temp_storage_bytes,
            (uint64_t*)d_keys_in, (uint64_t*)d_keys_out, (uint64_t*)d_values_in, (uint64_t*)d_values_out,
            num_items, num_segments, d_offsets, ofs_end);

        cudaDeviceSynchronize();

        mm_gen_regs2<<<BLOCK_NUM_SHORT, THREAD_NUM_SHORT, 0, 0>>>(n_a, g_ax, g_ay, g_u, 
            g_zx, g_zy, bufy, g_n_v, g_n_u, ofs_end, 
            offset, r_offset, d_len, is_qstrand, n_chain, g_c, n_task, dropped);
    
        cudaDeviceSynchronize();
    }
    else{ // Filter out anchors after DP chain, and generate regs
        mm_gen_regs_dp<<<BLOCK_NUM_SHORT, THREAD_NUM_SHORT, 0, 0>>>(n_a, g_ax, g_ay, g_u, 
            bufx, bufy, g_zy, g_n_v, g_n_u, ofs_end, 
            offset, r_offset, d_len, is_qstrand, n_chain, g_c, n_task, dropped);

        cudaDeviceSynchronize();
    }

    cudaDeviceSynchronize();

    DEBUG_ONLY(
        clock_gettime(CLOCK_BOOTTIME, &end);
        printf(" ***** cuda kernel took %f seconds to gen regs\n",
        ( end.tv_sec - start.tv_sec ) + ( end.tv_nsec - start.tv_nsec ) / 1E9);
        check_mem();

        clock_gettime(CLOCK_BOOTTIME, &start);
    );

    cudaFree(bufx);
    cudaFree(bufy);
    cudaFree(ofs_end);
    cudaFree(num_elements);
    cudaFree(d_temp_storage);

    DEBUG_ONLY(
        clock_gettime(CLOCK_BOOTTIME, &end);
        printf(" ***** cuda kernel took %f seconds to free memory\n",
        ( end.tv_sec - start.tv_sec ) + ( end.tv_nsec - start.tv_nsec ) / 1E9);
    );

}