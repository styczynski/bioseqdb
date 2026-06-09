#ifndef AGATHA_KERNEL_H
#define AGATHA_KERNEL_H

#include "gasal_kernels.h"
#include "gasal_header.h"

// This old core provides the same result as the currently LOCAL core, but lacks some optimization. Left for historical / comparative purposes.
// Deprecated code from GASAL2 (left as reference)
#define CORE_LOCAL_DEPRECATED_COMPUTE()\
		uint32_t rbase = (packed_ref_literal >> l) & 15;/*get a base from target_batch sequence */ \
		DEV_GET_SUB_SCORE_LOCAL(temp_score, qbase, rbase);/* check equality of qbase and rbase */ \
		f[m] = max(h[m]- _cudaGapOE, f[m] - _cudaGapExtend);/* whether to introduce or extend a gap in query_batch sequence */ \
		h[m] = p[m] + temp_score; /*score if qbase is aligned to rbase*/ \
		h[m] = max(h[m], f[m]);\
		h[m] = max(h[m], 0);\
		e = max(h[m - 1] - _cudaGapOE, e - _cudaGapExtend);/*whether to introduce or extend a gap in target_batch sequence */\
		h[m] = max(h[m], e);\
		max_ref_idx = (max_score < h[m]) ? ref_idx + (m-1) : max_ref_idx;\
		max_score = (max_score < h[m]) ? h[m] : max_score;\
		p[m] = h[m-1];

#define CORE_COMPUTE()\
		uint32_t rbase = (packed_ref_literal >> l) & 15;\
		DEV_GET_SUB_SCORE_GLOBAL(temp_score, qbase, rbase)\
		temp_score += p[m];\
		h[m] = max(temp_score, f[m]);\
		h[m] = max(h[m], e);\
		f[m] = max(temp_score- _cudaGapOE, f[m] - _cudaGapExtend);\
		e = max(temp_score- _cudaGapOE, e - _cudaGapExtend);\
		p[m] = h[m-1];\
		diag_idx = ((ref_idx + m-1+query_idx)&(total_shm-1))<<5;\
		antidiag_max[real_warp_id+diag_idx] = max(antidiag_max[real_warp_id+diag_idx], (h[m]<<16) +ref_idx+ m-1);\

#define CORE_COMPUTE_BOUNDARY()\
		if (query_idx + _cudaBandWidth < ref_idx + m-1 || query_idx - _cudaBandWidth > ref_idx + m-1) {\
			p[m] = h[m-1];\
		} else {\
			uint32_t rbase = (packed_ref_literal >> l) & 15;\
			DEV_GET_SUB_SCORE_GLOBAL(temp_score, qbase, rbase)\
			temp_score += p[m];\
			h[m] = max(temp_score, f[m]);\
			h[m] = max(h[m], e);\
			f[m] = max(temp_score- _cudaGapOE, f[m] - _cudaGapExtend);\
			e = max(temp_score- _cudaGapOE, e - _cudaGapExtend);\
			p[m] = h[m-1];\
			diag_idx = ((ref_idx + m-1+query_idx)&(total_shm-1))<<5;\
			antidiag_max[real_warp_id+diag_idx] = max(antidiag_max[real_warp_id+diag_idx], (h[m]<<16) +ref_idx+ m-1);\
		}

#define CORE_COMPUTE_APPROX_MAX()\
		if((max_ref_idx[warp_num_block]+1 == ref_idx + m-1 && max_query_idx[warp_num_block]+1 == query_idx) && ref_idx + m-1 < ref_len){ /* check max update first - probe reached max position */\ 
			if(h[m]>=h[m-1]) { /* move down */\
				max_score[warp_num_block] = h[m]; max_ref_idx[warp_num_block]++;\
			}\
			else{ /* move right */\
				max_score[warp_num_block] = h[m-1]; max_query_idx[warp_num_block]++;\
			}\
			if(max_score[warp_num_block] > global_max_score[warp_num_block]) { /* update global max */ \
				global_max_score[warp_num_block] = max_score[warp_num_block]; global_max_ref_idx[warp_num_block] = max_ref_idx[warp_num_block]; global_max_query_idx[warp_num_block] = max_query_idx[warp_num_block];\
			}\
		}\
		uint32_t rbase = (packed_ref_literal >> l) & 15;\
		DEV_GET_SUB_SCORE_GLOBAL(temp_score, qbase, rbase)\
		temp_score += p[m];\
		h[m] = max(temp_score, f[m]);\
		h[m] = max(h[m], e);\
		f[m] = max(temp_score- _cudaGapOE, f[m] - _cudaGapExtend);\
		e = max(temp_score- _cudaGapOE, e - _cudaGapExtend);\
		p[m] = h[m-1];\
		if(max_ref_idx[warp_num_block] == ref_len-1 && max_query_idx[warp_num_block]+1 == query_idx && ref_idx+m-1 == ref_len-1){ /* reached last row */\
				max_score[warp_num_block] = h[m]; max_query_idx[warp_num_block]++;\
				if(max_score[warp_num_block] > global_max_score[warp_num_block]) { /* update global max */ \
				global_max_score[warp_num_block] = max_score[warp_num_block]; global_max_ref_idx[warp_num_block] = max_ref_idx[warp_num_block]; global_max_query_idx[warp_num_block] = max_query_idx[warp_num_block];\
			}\
		}\
		if(max_ref_idx[warp_num_block]+1 == ref_idx + m-1  && max_query_idx[warp_num_block] == query_len-1 && query_idx == query_len-1){ /* reached last column */\ 
				max_score[warp_num_block] = h[m]; max_ref_idx[warp_num_block]++;\
				if(max_score[warp_num_block] > global_max_score[warp_num_block]) { /* update global max */ \
				global_max_score[warp_num_block] = max_score[warp_num_block]; global_max_ref_idx[warp_num_block] = max_ref_idx[warp_num_block]; global_max_query_idx[warp_num_block] = max_query_idx[warp_num_block];\
			}\
		}\
// check if adjacent to global max cell & update global max value

#define CORE_COMPUTE_BOUNDARY_APPROX_MAX()\
		if((max_ref_idx[warp_num_block]+1 == ref_idx + m-1 && max_query_idx[warp_num_block]+1 == query_idx)){ /* check max update first - probe reached max position */\ 
			if(h[m]>=h[m-1]) { /* move down */ \
				max_score[warp_num_block] = h[m]; max_ref_idx[warp_num_block]++;\
			}\
			else{ /* move right */ \
				max_score[warp_num_block] = h[m-1]; max_query_idx[warp_num_block]++;\
			}\
			if(max_score[warp_num_block] > global_max_score[warp_num_block]) { /* update global max */\
				global_max_score[warp_num_block] = max_score[warp_num_block]; global_max_ref_idx[warp_num_block] = max_ref_idx[warp_num_block]; global_max_query_idx[warp_num_block] = max_query_idx[warp_num_block];\
			}\
		} \
		if (query_idx + _cudaBandWidth < ref_idx + m-1 || query_idx - _cudaBandWidth > ref_idx + m-1) {\
			p[m] = h[m-1];\
		} else {\
			uint32_t rbase = (packed_ref_literal >> l) & 15;\
			DEV_GET_SUB_SCORE_GLOBAL(temp_score, qbase, rbase)\
			temp_score += p[m];\
			h[m] = max(temp_score, f[m]);\
			h[m] = max(h[m], e);\
			f[m] = max(temp_score- _cudaGapOE, f[m] - _cudaGapExtend);\
			e = max(temp_score- _cudaGapOE, e - _cudaGapExtend);\
			p[m] = h[m-1];\
			if(max_ref_idx[warp_num_block] == ref_len-1 && max_query_idx[warp_num_block]+1 == query_idx && ref_idx+m-1 == ref_len-1){ /* reached last row */\
				max_score[warp_num_block] = h[m]; max_query_idx[warp_num_block]++;\
				if(max_score[warp_num_block] > global_max_score[warp_num_block]) { /* update global max */ \
				global_max_score[warp_num_block] = max_score[warp_num_block]; global_max_ref_idx[warp_num_block] = max_ref_idx[warp_num_block]; global_max_query_idx[warp_num_block] = max_query_idx[warp_num_block];\
				}\
			}\
		if(max_ref_idx[warp_num_block]+1 == ref_idx + m-1  && max_query_idx[warp_num_block] == query_len-1 && query_idx == query_len-1){ /* reached last column */\ 
				max_score[warp_num_block] = h[m]; max_ref_idx[warp_num_block]++;\
				if(max_score[warp_num_block] > global_max_score[warp_num_block]) { /* update global max */ \
				global_max_score[warp_num_block] = max_score[warp_num_block]; global_max_ref_idx[warp_num_block] = max_ref_idx[warp_num_block]; global_max_query_idx[warp_num_block] = max_query_idx[warp_num_block];\
				}\
			}\
		}

#define CORE_COMPUTE_APPROX()\
		uint32_t rbase = (packed_ref_literal >> l) & 15;\
		DEV_GET_SUB_SCORE_GLOBAL(temp_score, qbase, rbase)\
		temp_score += p[m];\
		h[m] = max(temp_score, f[m]);\
		h[m] = max(h[m], e);\
		f[m] = max(temp_score- _cudaGapOE, f[m] - _cudaGapExtend);\
		e = max(temp_score- _cudaGapOE, e - _cudaGapExtend);\
		p[m] = h[m-1];\


#define CORE_COMPUTE_BOUNDARY_APPROX() \
		if (query_idx + _cudaBandWidth < ref_idx + m-1 || query_idx - _cudaBandWidth > ref_idx + m-1) {\
			p[m] = h[m-1];\
		} else {\
			uint32_t rbase = (packed_ref_literal >> l) & 15;\
			DEV_GET_SUB_SCORE_GLOBAL(temp_score, qbase, rbase)\
			temp_score += p[m];\
			h[m] = max(temp_score, f[m]);\
			h[m] = max(h[m], e);\
			f[m] = max(temp_score- _cudaGapOE, f[m] - _cudaGapExtend);\
			e = max(temp_score- _cudaGapOE, e - _cudaGapExtend);\
			p[m] = h[m-1];\
		}

__global__ void agatha_kernel(uint32_t *packed_query_batch, uint32_t *packed_ref_batch,  uint32_t *query_batch_lens, uint32_t *target_batch_lens, uint32_t *query_batch_offsets, uint32_t *target_batch_offsets, gasal_res_t *device_res, gasal_res_t *device_res_second, uint4 *packed_tb_matrices, int n_tasks, uint32_t max_query_len, short2 *global_buffer_top);

__global__ void agatha_sort(uint32_t *packed_query_batch, uint32_t *packed_ref_batch,  uint32_t *query_batch_lens, uint32_t *target_batch_lens, uint32_t *query_batch_offsets, uint32_t *target_batch_offsets, int n_tasks, uint32_t max_query_len, short2 *global_buffer_top);


/* kernels including traceback phase */

#define CORE_COMPUTE_TB(direction_reg)\
		uint32_t rbase = (packed_ref_literal >> l) & 15;\
		DEV_GET_SUB_SCORE_GLOBAL(temp_score, qbase, rbase)\
		temp_score += p[m];\
		uint32_t m_or_x = temp_score >= p[m] ? 0 : 1;\
		h[m] = max(temp_score, f[m]);\
		h[m] = max(h[m], e);\
		direction_reg |= h[m] == temp_score ? m_or_x << (28 - ((m - 1) << 2)) : (h[m] == f[m] ? (uint32_t)3 << (28 - ((m - 1) << 2)) : (uint32_t)2 << (28 - ((m - 1) << 2)));\
		direction_reg |= (temp_score - _cudaGapOE) >= (f[m] - _cudaGapExtend) ?  (uint32_t)0 : (uint32_t)1 << (31 - ((m - 1) << 2));\
		f[m] = max(temp_score- _cudaGapOE, f[m] - _cudaGapExtend);\
		direction_reg |= (temp_score - _cudaGapOE) >= (e - _cudaGapExtend) ?  (uint32_t)0 : (uint32_t)1 << (30 - ((m - 1) << 2));\
		e = max(temp_score- _cudaGapOE, e - _cudaGapExtend);\
		p[m] = h[m-1];\
		diag_idx = ((ref_idx + m-1+query_idx)&(total_shm-1))<<5;\
		antidiag_max[real_warp_id+diag_idx] = max(antidiag_max[real_warp_id+diag_idx], (h[m]<<16) +ref_idx+ m-1);\

#define CORE_COMPUTE_BOUNDARY_TB(direction_reg) \
		if (query_idx + _cudaBandWidth < ref_idx + m-1 || query_idx - _cudaBandWidth > ref_idx + m-1) {\
			p[m] = h[m-1];\
		} else {\
			uint32_t rbase = (packed_ref_literal >> l) & 15;\
			DEV_GET_SUB_SCORE_GLOBAL(temp_score, qbase, rbase)\
			temp_score += p[m];\
			uint32_t m_or_x = temp_score >= p[m] ? 0 : 1;\
			h[m] = max(temp_score, f[m]);\
			h[m] = max(h[m], e);\
			direction_reg |= h[m] == temp_score ? m_or_x << (28 - ((m - 1) << 2)) : (h[m] == f[m] ? (uint32_t)3 << (28 - ((m - 1) << 2)) : (uint32_t)2 << (28 - ((m - 1) << 2)));\
			direction_reg |= (temp_score - _cudaGapOE) >= (f[m] - _cudaGapExtend) ?  (uint32_t)0 : (uint32_t)1 << (31 - ((m - 1) << 2));\
			f[m] = max(temp_score- _cudaGapOE, f[m] - _cudaGapExtend);\
			direction_reg |= (temp_score - _cudaGapOE) >= (e - _cudaGapExtend) ?  (uint32_t)0 : (uint32_t)1 << (30 - ((m - 1) << 2));\
			e = max(temp_score- _cudaGapOE, e - _cudaGapExtend);\
			p[m] = h[m-1];\
			diag_idx = ((ref_idx + m-1+query_idx)&(total_shm-1))<<5;\
			antidiag_max[real_warp_id+diag_idx] = max(antidiag_max[real_warp_id+diag_idx], (h[m]<<16) +ref_idx+ m-1);\
			}\

#define CORE_COMPUTE_APPROX_MAX_TB(direction_reg)\
		if((max_ref_idx[warp_num_block]+1 == ref_idx + m-1 && max_query_idx[warp_num_block]+1 == query_idx)){ /* check max update first - probe reached max position */\ 
			if(h[m]>h[m-1]) { /* move down */ \
				max_score[warp_num_block] = h[m]; max_ref_idx[warp_num_block]++;\
			}\
			else{ /* move right */ \
				max_score[warp_num_block] = h[m-1]; max_query_idx[warp_num_block]++;\
			}\
			if(max_score[warp_num_block] > global_max_score[warp_num_block]) { /* update global max */ \
				global_max_score[warp_num_block] = max_score[warp_num_block]; global_max_ref_idx[warp_num_block] = max_ref_idx[warp_num_block]; global_max_query_idx[warp_num_block] = max_query_idx[warp_num_block];\
			}\
		} \
		uint32_t rbase = (packed_ref_literal >> l) & 15;\
		DEV_GET_SUB_SCORE_GLOBAL(temp_score, qbase, rbase)\
		temp_score += p[m];\
		uint32_t m_or_x = temp_score >= p[m] ? 0 : 1;\
		h[m] = max(temp_score, f[m]);\
		h[m] = max(h[m], e);\
		direction_reg |= h[m] == temp_score ? m_or_x << (28 - ((m - 1) << 2)) : (h[m] == f[m] ? (uint32_t)3 << (28 - ((m - 1) << 2)) : (uint32_t)2 << (28 - ((m - 1) << 2)));\
		direction_reg |= (temp_score - _cudaGapOE) >= (f[m] - _cudaGapExtend) ?  (uint32_t)0 : (uint32_t)1 << (31 - ((m - 1) << 2));\
		f[m] = max(temp_score- _cudaGapOE, f[m] - _cudaGapExtend); \
		direction_reg |= (temp_score - _cudaGapOE) >= (e - _cudaGapExtend) ?  (uint32_t)0 : (uint32_t)1 << (30 - ((m - 1) << 2));\
		e = max(temp_score- _cudaGapOE, e - _cudaGapExtend); \
		p[m] = h[m-1];\
		if(max_ref_idx[warp_num_block] == ref_len-1 && max_query_idx[warp_num_block]+1 == query_idx){ /* reached last row */\
				max_score[warp_num_block] = h[m-1]; max_query_idx[warp_num_block]++;\
				if(max_score[warp_num_block] > global_max_score[warp_num_block]) { /* update global max */ \
				global_max_score[warp_num_block] = max_score[warp_num_block]; global_max_ref_idx[warp_num_block] = max_ref_idx[warp_num_block]; global_max_query_idx[warp_num_block] = max_query_idx[warp_num_block];\
				}\
		}\
		if(max_ref_idx[warp_num_block]+1 == ref_idx + m-1  && max_query_idx[warp_num_block] == query_len-1){ /* reached last column */\ 
				max_score[warp_num_block] = h[m]; max_ref_idx[warp_num_block]++;\
				if(max_score[warp_num_block] > global_max_score[warp_num_block]) { /* update global max */ \
				global_max_score[warp_num_block] = max_score[warp_num_block]; global_max_ref_idx[warp_num_block] = max_ref_idx[warp_num_block]; global_max_query_idx[warp_num_block] = max_query_idx[warp_num_block];\
				}\
		}\
// check if adjacent to global max cell & update global max value

#define CORE_COMPUTE_BOUNDARY_APPROX_MAX_TB(direction_reg) \
		if((max_ref_idx[warp_num_block]+1 == ref_idx + m-1 && max_query_idx[warp_num_block]+1 == query_idx)){ /* check max update first - probe reached max position */\ 
			if(h[m]>h[m-1]) { /* move down */ \
				max_score[warp_num_block] = h[m]; max_ref_idx[warp_num_block]++;\
			}\
			else{ /* move right */ \
				max_score[warp_num_block] = h[m-1]; max_query_idx[warp_num_block]++;\
			}\
			if(max_score[warp_num_block] > global_max_score[warp_num_block]) { /* update global max */ \
				global_max_score[warp_num_block] = max_score[warp_num_block]; global_max_ref_idx[warp_num_block] = max_ref_idx[warp_num_block]; global_max_query_idx[warp_num_block] = max_query_idx[warp_num_block];\
			}\
		} \
		if (query_idx + _cudaBandWidth < ref_idx + m-1 || query_idx - _cudaBandWidth > ref_idx + m-1) {\
			p[m] = h[m-1];\
		} else {\
			uint32_t rbase = (packed_ref_literal >> l) & 15;\
			DEV_GET_SUB_SCORE_GLOBAL(temp_score, qbase, rbase)\
			temp_score += p[m];\
			uint32_t m_or_x = temp_score >= p[m] ? 0 : 1;\
			h[m] = max(temp_score, f[m]);\
			h[m] = max(h[m], e);\
			direction_reg |= h[m] == temp_score ? m_or_x << (28 - ((m - 1) << 2)) : (h[m] == f[m] ? (uint32_t)3 << (28 - ((m - 1) << 2)) : (uint32_t)2 << (28 - ((m - 1) << 2)));\
			direction_reg |= (temp_score - _cudaGapOE) >= (f[m] - _cudaGapExtend) ?  (uint32_t)0 : (uint32_t)1 << (31 - ((m - 1) << 2));\
			f[m] = max(temp_score- _cudaGapOE, f[m] - _cudaGapExtend); \
			direction_reg |= (temp_score - _cudaGapOE) >= (e - _cudaGapExtend) ?  (uint32_t)0 : (uint32_t)1 << (30 - ((m - 1) << 2));\
			e = max(temp_score- _cudaGapOE, e - _cudaGapExtend); \
			p[m] = h[m-1];\
			if(max_ref_idx[warp_num_block] == ref_len-1 && max_query_idx[warp_num_block]+1 == query_idx){ /* reached last row */\
				max_score[warp_num_block] = h[m]; max_query_idx[warp_num_block]++;\
				if(max_score[warp_num_block] > global_max_score[warp_num_block]) { /* update global max */ \
				global_max_score[warp_num_block] = max_score[warp_num_block]; global_max_ref_idx[warp_num_block] = max_ref_idx[warp_num_block]; global_max_query_idx[warp_num_block] = max_query_idx[warp_num_block];\
				}\
			}\
			if(max_ref_idx[warp_num_block]+1 == ref_idx + m-1  && max_query_idx[warp_num_block] == query_len-1){ /* reached last column */\ 
				max_score[warp_num_block] = h[m]; max_ref_idx[warp_num_block]++;\
				if(max_score[warp_num_block] > global_max_score[warp_num_block]) { /* update global max */ \
				global_max_score[warp_num_block] = max_score[warp_num_block]; global_max_ref_idx[warp_num_block] = max_ref_idx[warp_num_block]; global_max_query_idx[warp_num_block] = max_query_idx[warp_num_block];\
				}\
			}\
		}

#define CORE_COMPUTE_APPROX_TB(direction_reg) \
		uint32_t rbase = (packed_ref_literal >> l) & 15;\
		DEV_GET_SUB_SCORE_GLOBAL(temp_score, qbase, rbase)\
		temp_score += p[m];\
		uint32_t m_or_x = temp_score >= p[m] ? 0 : 1;\
		h[m] = max(temp_score, f[m]);\
		h[m] = max(h[m], e);\
		direction_reg |= h[m] == temp_score ? m_or_x << (28 - ((m - 1) << 2)) : (h[m] == f[m] ? (uint32_t)3 << (28 - ((m - 1) << 2)) : (uint32_t)2 << (28 - ((m - 1) << 2)));\
		direction_reg |= (temp_score - _cudaGapOE) >= (f[m] - _cudaGapExtend) ?  (uint32_t)0 : (uint32_t)1 << (31 - ((m - 1) << 2));\
		f[m] = max(temp_score- _cudaGapOE, f[m] - _cudaGapExtend);\
		direction_reg |= (temp_score - _cudaGapOE) >= (e - _cudaGapExtend) ?  (uint32_t)0 : (uint32_t)1 << (30 - ((m - 1) << 2));\
		e = max(temp_score- _cudaGapOE, e - _cudaGapExtend);\
		p[m] = h[m-1];\


#define CORE_COMPUTE_BOUNDARY_APPROX_TB(direction_reg) \
		if (query_idx + _cudaBandWidth < ref_idx + m-1 || query_idx - _cudaBandWidth > ref_idx + m-1) { \
			p[m] = h[m-1];\
		} else { \
			uint32_t rbase = (packed_ref_literal >> l) & 15;\
			DEV_GET_SUB_SCORE_GLOBAL(temp_score, qbase, rbase) \
			temp_score += p[m]; \
			uint32_t m_or_x = temp_score >= p[m] ? 0 : 1;\
			h[m] = max(temp_score, f[m]); \
			h[m] = max(h[m], e); \
			direction_reg |= h[m] == temp_score ? m_or_x << (28 - ((m - 1) << 2)) : (h[m] == f[m] ? (uint32_t)3 << (28 - ((m - 1) << 2)) : (uint32_t)2 << (28 - ((m - 1) << 2)));\
			direction_reg |= (temp_score - _cudaGapOE) >= (f[m] - _cudaGapExtend) ?  (uint32_t)0 : (uint32_t)1 << (31 - ((m - 1) << 2));\
			f[m] = max(temp_score- _cudaGapOE, f[m] - _cudaGapExtend); \
			direction_reg |= (temp_score - _cudaGapOE) >= (e - _cudaGapExtend) ?  (uint32_t)0 : (uint32_t)1 << (30 - ((m - 1) << 2));\
			e = max(temp_score- _cudaGapOE, e - _cudaGapExtend); \
			p[m] = h[m-1]; \
		}


__global__ void agatha_kernel_approx_gridded_tb(uint32_t *packed_query_batch, uint32_t *packed_ref_batch,  uint32_t *query_batch_lens, uint32_t *target_batch_lens, uint32_t *query_batch_offsets, uint32_t *target_batch_offsets, gasal_res_t *device_res, gasal_res_t *device_res_second, short2 *dblock_row, short2 *dblock_col, int n_tasks, uint32_t max_query_len, short2 *global_buffer_top,
								uint64_t* tb_ofs, bool* dropped, int bw);
__global__
void mm2_kswz_extension(char* qseqs, char* tseqs, uint32_t* qseq_len, uint32_t* tseq_len, uint32_t* qseq_ofs, uint32_t* tseq_ofs,
						uint32_t* packed_tb_matrix, gasal_res_t *device_res, int* n_cigar, uint32_t* cigar, int n_task, uint32_t max_query_len, bool left);
__global__
void mm2_kswz_extension_simd(char* qseqs, char* tseqs, uint32_t* qseq_len, uint32_t* tseq_len, uint32_t* qseq_ofs, uint32_t* tseq_ofs,
						uint8_t* tb_matrix, gasal_res_t *device_res, int* n_cigar, uint32_t* cigar, int n_task, uint32_t max_query_len, bool left);

#endif