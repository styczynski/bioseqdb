#ifndef _KSW_CUDA_H
#define _KSW_CUDA_H

#include "stdint.h"
#include "bwa.h"

#define KSW_XBYTE  0x10000
#define KSW_XSTOP  0x20000
#define KSW_XSUBO  0x40000
#define KSW_XSTART 0x80000
#define KSW_MAX_QLEN 500

typedef	struct m128i {
	// set of 8 16-bit integers
	int16_t x0, x1, x2, x3, x4, x5, x6, x7;
} m128i;

typedef struct kswq_t {
	m128i *qp, *H0, *H1, *E, *Hmax;
	int qlen, slen;
	uint8_t shift, mdiff, max, size;
} kswq_t;

typedef struct {
	int score; // best score
	int te, qe; // target end and query end
	int score2, te2; // second best score and ending position on the target
	int tb, qb; // target start and query start
} kswr_t;

__device__ kswr_t ksw_align2(int qlen, uint8_t *query, int tlen, uint8_t *target, int m, const int8_t *mat, int o_del, int e_del, int o_ins, int e_ins, int xtra, kswq_t **qry, void* d_buffer_ptr);

__device__ int ksw_extend2(int qlen, const uint8_t *query, int tlen, const uint8_t *target, int m, const int8_t *mat, int o_del, int e_del, int o_ins, int e_ins, int w, int end_bonus, int zdrop, int h0, int *_qle, int *_tle, int *_gtle, int *_gscore, int *_max_off, void* d_buffer_ptr);

/* SW extension function to be executed at warp level. bandwidth not applied */
__device__ int ksw_extend_warp(int qlen, const uint8_t *query, int tlen, const uint8_t *target, int m, const int8_t *mat, int o_del, int e_del, int o_ins, int e_ins, int h0, int *_qle, int *_tle, int *_gtle, int *_gscore);
/* SW global function to be executed at warp level. bandwidth not applied */
__device__ int ksw_global_warp(int qlen, const uint8_t *query, int tlen, const uint8_t *target, int m, const int8_t *mat, int o_del, int e_del, int o_ins, int e_ins, int *i_max, int *j_max, uint8_t *traceback);

__device__ int ksw_global2(int qlen, const uint8_t *query, int tlen, const uint8_t *target, int m, const int8_t *mat, int o_del, int e_del, int o_ins, int e_ins, int w, int *n_cigar_, uint32_t **cigar_, void* d_buffer_ptr);

__device__ int ksw_extend_warp2(int qlen, const uint8_t *query, int tlen, const uint8_t *target, int m, const int8_t *mat, int o_del, int e_del, int o_ins, int e_ins, int w, int end_bonus, int h0, int *_qle, int *_tle, int *_gtle, int *_gscore);
__device__ int ksw_global3(int qlen, const uint8_t *query, int tlen, const uint8_t *target, int m, const int8_t *mat, int o_del, int e_del, int o_ins, int e_ins, int w, int *n_cigar_, uint32_t **cigar_, void* d_buffer_ptr);
#endif
