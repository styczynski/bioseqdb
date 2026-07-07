#ifndef __GASAL_ALIGN_H__
#define __GASAL_ALIGN_H__

void gasal_copy_subst_scores(gasal_subst_scores *subst);

void gasal_aln_async(gasal_gpu_storage_t *gpu_storage, const uint32_t actual_query_batch_bytes, const uint32_t actual_target_batch_bytes, const uint32_t actual_n_alns, Parameters *params);

inline void gasal_kernel_launcher(int32_t kernel_block_num, int32_t kernel_thread_num, algo_type algo, comp_start start, gasal_gpu_storage_t *gpu_storage, int32_t actual_n_alns, int32_t k_band, uint32_t maximum_sequence_length);

int gasal_is_aln_async_done(gasal_gpu_storage_t *gpu_storage);

#endif
