#ifndef ALIGNMENT_KERNEL_H
#define ALIGNMENT_KERNEL_H

#include "common.h"
#include "minimap.h"
#include "mmpriv.h"
#include "chaining_kernel.h"


__global__
void mm_test_zdrop(uint8_t* zdrop_code, uint32_t* g_qlen, uint32_t* g_rlen, uint8_t* g_qseq, uint8_t* g_tseq, int* g_n_cigar, uint32_t *g_cigar, int n_task, int max_seq_len, int8_t* g_mat);

__global__
void collect_extending_results(mm_chain_t* g_c, int* dp_score, int* n_cigar, uint32_t* cigar, int* n_cigar_output, uint32_t* cigar_output, int n_chain, int max_frag_len, int max_seq_len, uint8_t* zdrop_code, uint8_t* zdropped, int* read_id);

__global__ 
void fill_align_info(const mm_idx_t *mi, uint64_t* g_ax, uint64_t* g_ay, mm_chain_t* chain, // reference index, anchors, chain metadata
					uint8_t* const g_qseq0, uint8_t* align_qseq, uint8_t* align_rseq, // original qseq, target qseq, target rseq
					uint32_t* q_lens, uint32_t* r_lens, uint32_t* q_ofs, uint32_t* r_ofs, // alignment information for AGATHA kernel
					int n_chain, int max_qlen, int max_rlen, bool* g_dropped, bool* g_zdropped, int flag, int* n_align); // extra informations



#endif