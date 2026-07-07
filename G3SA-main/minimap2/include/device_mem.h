#ifndef __DEVICE_MEM_H__
#define __DEVICE_MEM_H__

#include "minimap.h"
#include "gasal.h"
#include "chaining_kernel.h"
#include "debug.h"
#include <iostream>

const long long int MEMPOOL_SIZE = 1024*1024*1024; // 1GB of memory

static void check_mem(){
    cudaDeviceSynchronize();
    size_t free_memory = 0;
    size_t total_memory = 0;
    cudaError_t status = cudaMemGetInfo(&free_memory, &total_memory);

    if (status != cudaSuccess) {
        std::cerr << "Error: " << cudaGetErrorString(status) << std::endl;
        return;
    }

    std::cout << "[ Free memory: " << free_memory / (1024.0 * 1024.0) << " MB || ";
    std::cout << "Total memory: " << total_memory / (1024.0 * 1024.0) << " MB ]" << std::endl;
}

#define checkCudaErrors(call)                                 \
  do {                                                        \
    cudaError_t err = call;                                   \
    if (err != cudaSuccess) {                                 \
      printf("CUDA error at %s %d: %s\n", __FILE__, __LINE__, \
             cudaGetErrorString(err));                        \
      exit(EXIT_FAILURE);                                     \
    }                                                         \
  } while (0)

typedef struct{
    char* seqs;
    int* lens;
    int* ofs;

    mm_mapopt_t* opt;

    int totalSeqLength;
} device_input_batch_t;

typedef struct{
	mm128_t* minimizers;
    int* n_minimizers;

    uint64_t* ax;
    uint64_t* ay;
    int* n_anchors;
    int* anchor_ofs;

    int* tmp_n;
    uint64_t** tmp_cr;
    mm_seed_t* tmp_seed;

    int32_t* num_anchors;
} device_seed_batch_t;

typedef struct{
    uint64_t* ax;
    uint64_t* ay;

    uint2* range;
    int* n_range;
    bool* seg_pass;

    int32_t* sc;
    int64_t* p;

    int *long_range_buf_st, *long_range_buf_len;
    int *mid_range_buf_st, *mid_range_buf_len;

    uint64_t* u;
    int64_t* zx;
    int64_t* zy;
    int32_t* t;
    int64_t* v;
    int* n_v;
    int* n_u;

    mm_chain_t* chain;
    bool* dropped;
    int* n_chain;
    
} device_chain_batch_t;

typedef struct{
	uint8_t *qseqs, *rseqs;
    uint32_t *q_lens, *r_lens, *q_ofs, *r_ofs;

    bool* zdropped;
    int* n_cigar;
    uint32_t* cigar;

    uint32_t *d_qseqs_packed, *d_rseqs_packed;
    gasal_res_t* device_res;

    gasal_res_t *device_res_second; // not used
    uint4 *packed_tb_matrices; // not used also
    short2 *global_buffer_top;

    uint32_t* tb_matrices;
    short2 * dblock_row, *dblock_col;

    int* n_extend;
    int* actual_n_alns_left;
    int actual_n_alns_right;
    int actual_n_alns_gap;
    int tb_batch_size;

    /* for dynamic traceback offset - on work */
    uint32_t* tb_buf_size;
    uint64_t* tb_buf_ofs;

    uint8_t* zdrop_code;

} device_extend_batch_t;

typedef struct{
    int* aln_score;
    int* n_cigar;
    uint32_t* cigar;

    uint8_t* zdropped;
    int* read_id;

    int* n_align;
} device_output_batch_t;

typedef struct{
    void* basePtr;
    void* ptr;
    void* flagPtr; // pointer for freeing tmp metadata

    uint64_t ptrIdx;
    uint64_t flagIdx;
    uint64_t capacity;
} mpool_t;

/* For multi batch, multi GPU implementation */
typedef struct{
    // These pointers are allocated only once in the initial setup
    mm_idx_t* idx;
    device_input_batch_t input;
    device_output_batch_t output;
    mpool_t mpool;
    int device_num;
} device_pointer_batch_t;


static size_t get_alignment(size_t size);

void* fetch_memory_mempool(mpool_t* mpool, uint64_t length, int typeSize);

void free_tmp_memory_mempool(mpool_t* mpool);

void set_tmp_memory_flag_mempool(mpool_t* mpool);

void initialize_mempool(mpool_t* mpool);

void allcoate_memory_mempool(mpool_t* mpool);

void allocate_memory_input(device_input_batch_t* batch, int seqLength, int ofsLength);
void allocate_memory_seed_mempool(device_seed_batch_t* batch, mpool_t* mpool, int seqLength, int ofsLength, int anchorLength);
void allocate_memory_chain_mempool(device_chain_batch_t* batch, mpool_t* mpool,int seqLength, int ofsLength, int anchorLength, int chainLength);
void allocate_memory_extend_mempool(device_extend_batch_t* batch, mpool_t* mpool, int n_chain, int maxSeqLength, int tb_batch_size, int gpu_id);
void allocate_memory_output_mempool(device_output_batch_t* batch, mpool_t* mpool, int n_results, int cigarLen, int batch_size);


void free_memory_input();
void free_memory_seed(device_seed_batch_t* batch);
void free_memory_chain();
void free_memory_extend();
void free_memory_output();
#endif