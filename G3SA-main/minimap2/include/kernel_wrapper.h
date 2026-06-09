#include <stdint.h>
#include <vector>

#include "common.h"
#include "minimap.h"
#include "mmpriv.h"
#include "khash.h"
#include "krmq_cuda.h"
#include "seeding_kernel.h"
#include "device_mem.h"
#include "params.h"
#include "alignment_kernel.h"
#include "agatha_kernel.h"
#include "tb_kernel.h"
#include "pack_rc_seqs.h"
#include "debug.h"
			
mm_idx_t* transfer_index(mm_idx_t* mi);

mm_idx_t* transfer_index_multigpu(mm_idx_t* h_idx);

void* fetch_memory_mempool(void** memPoolPtr, int length, int typeSize, uint64_t* ptrIndex);

void gpu_initialize(int gpu_id, mm_idx_t* mi, mm_mapopt_t* opt, device_pointer_batch_t* device_ptrs, uint64_t max_seq_len, int batch_size);

void gpu_mm2_kernel_wrapper(//int* rid, 
    char* h_seqs, int* h_lens, int* h_ofs, device_pointer_batch_t* device_ptrs, int w, int k, uint64_t max_seq_len, int batch_size,
    uint32_t* h_cigar, int* h_n_cigar, int* h_score, // output data
    uint8_t* h_dropped // fallback reads
);

void seeding_kernel_wrapper(mm_idx_t* idx, device_input_batch_t input_batch, device_seed_batch_t seed_batch, device_chain_batch_t chain_batch, int batch_size);

#ifdef __CUDACC__

void chaining_kernel_wrapper_dp(mm_mapopt_t* opt, uint64_t* ax, uint64_t* ay, int* ofs, int* n_a, uint2* range, int* n_range, int32_t* sc, int64_t* p, bool* seg_pass, int seg_num, int batch_size,
                            int* long_range_buf_st, int* long_range_buf_len, int* mid_range_buf_st, int* mid_range_buf_len);
							
void chaining_kernel_wrapper_rmq(uint64_t* ax, uint64_t* ay, int* ofs, int* n_a, uint2* range, int* n_range, int32_t* sc, int64_t* p, bool* seg_pass, int seg_num, int batch_size,
                            int* long_range_buf_st, int* long_range_buf_len, int* mid_range_buf_st, int* mid_range_buf_len, int h_num_anchors);


void chaining_kernel_wrapper(device_input_batch_t input_batch, device_seed_batch_t seed_batch, device_chain_batch_t chain_batch, device_extend_batch_t extend_batch,mm_idx_t* d_idx, int segLength,
                            int seqLength, int ofsLength, int h_num_anchors, int batch_size);

void extending_kernel_wrapper(mm_idx_t* d_idx, device_input_batch_t input_batch, device_seed_batch_t seed_batch, device_chain_batch_t chain_batch, device_extend_batch_t extend_batch, int max_ext_len);

void mm_chain_backtrack_kernels(int* n_a, uint64_t* g_ax, uint64_t* g_ay, int32_t* g_sc, int64_t *g_p, uint64_t* g_u, 
    int64_t* g_zx, int64_t* g_zy, int32_t* g_t, int64_t* g_v,
    int* offset, int* r_offset, int32_t min_cnt, int32_t min_sc, int32_t max_drop,
    mm_reg1_t *g_r, int* d_len, int is_qstrand, int* n_chain, mm_chain_t* g_c, int n_task, int* g_n_v, int* g_n_u, bool* dropped, bool final, int anchorLength, int batch_size);


#endif // __CUDACC__