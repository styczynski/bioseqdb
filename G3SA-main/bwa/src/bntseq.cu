#include "bntseq.h"
#include "gmem_alloc.h"

#define _set_pac(pac, l, c) ((pac)[(l)>>2] |= (c)<<((~(l)&3)<<1))
#define _get_pac(pac, l) ((pac)[(l)>>2]>>((~(l)&3)<<1)&3)

__device__ int bns_pos2rid_gpu(const bntseq_t *bns, int64_t pos_f)
{
	int left, mid, right;
	if (pos_f >= bns->l_pac) return -1;
	left = 0; mid = 0; right = bns->n_seqs;
	while (left < right) { // binary search
		mid = (left + right) >> 1;
		if (pos_f >= bns->anns[mid].offset) {
			if (mid == bns->n_seqs - 1) break;
			if (pos_f < bns->anns[mid+1].offset) break; // bracketed
			left = mid + 1;
		} else right = mid;
	}
	return mid;
}

__device__ static inline int64_t bns_depos_gpu(const bntseq_t *bns, int64_t pos, int *is_rev)
{
	return (*is_rev = (pos >= bns->l_pac))? (bns->l_pac<<1) - 1 - pos : pos;
}


__device__ int bns_intv2rid_gpu(const bntseq_t *bns, int64_t rb, int64_t re)
{
	int is_rev, rid_b, rid_e;
	if (rb < bns->l_pac && re > bns->l_pac) return -2;
	rid_b = bns_pos2rid_gpu(bns, bns_depos_gpu(bns, rb, &is_rev));
	rid_e = rb < re? bns_pos2rid_gpu(bns, bns_depos_gpu(bns, re - 1, &is_rev)) : rid_b;
	return rid_b == rid_e? rid_b : -1;
}


__device__ uint8_t *bns_get_seq_gpu(int64_t l_pac, const uint8_t *pac, int64_t beg, int64_t end, int64_t *len, void* d_buffer_ptr)
{
	uint8_t *seq = 0;
	if (end < beg) end ^= beg, beg ^= end, end ^= beg; // if end is smaller, swap
	if (end > l_pac<<1) end = l_pac<<1;
	if (beg < 0) beg = 0;
	if (beg >= l_pac || end <= l_pac) {
		int64_t k, l = 0;
		*len = end - beg;
		seq = (uint8_t*)CUDAKernelMalloc(d_buffer_ptr, end - beg, 1);
		if (beg >= l_pac) { // reverse strand
			int64_t beg_f = (l_pac<<1) - 1 - end;
			int64_t end_f = (l_pac<<1) - 1 - beg;
			for (k = end_f; k > beg_f; --k)
				seq[l++] = 3 - _get_pac(pac, k);
		} else { // forward strand
			for (k = beg; k < end; ++k)
				seq[l++] = _get_pac(pac, k);
		}
	} else *len = 0; // if bridging the forward-reverse boundary, return nothing
	return seq;
}

__device__ uint8_t *bns_fetch_seq_gpu(const bntseq_t *bns, const uint8_t *pac, int64_t *beg, int64_t mid, int64_t *end, int *rid, void* d_buffer_ptr)
{
	int64_t far_beg, far_end, len;
	int is_rev;
	uint8_t *seq;

	if (*end < *beg) *end ^= *beg, *beg ^= *end, *end ^= *beg; // if end is smaller, swap
	// assert(*beg <= mid && mid < *end);
	*rid = bns_pos2rid_gpu(bns, bns_depos_gpu(bns, mid, &is_rev));
	far_beg = bns->anns[*rid].offset;
	far_end = far_beg + bns->anns[*rid].len;
	if (is_rev) { // flip to the reverse strand
		int64_t tmp = far_beg;
		far_beg = (bns->l_pac<<1) - far_end;
		far_end = (bns->l_pac<<1) - tmp;
	}
	*beg = *beg > far_beg? *beg : far_beg;
	*end = *end < far_end? *end : far_end;
	seq = bns_get_seq_gpu(bns->l_pac, pac, *beg, *end, &len, d_buffer_ptr);
	// if (seq == 0 || *end - *beg != len) {
	// 	fprintf(stderr, "[E::%s] begin=%ld, mid=%ld, end=%ld, len=%ld, seq=%p, rid=%d, far_beg=%ld, far_end=%ld\n",
	// 			__func__, (long)*beg, (long)mid, (long)*end, (long)len, seq, *rid, (long)far_beg, (long)far_end);
	// }
	// assert(seq && *end - *beg == len); // assertion failure should never happen
	return seq;
}
