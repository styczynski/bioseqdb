#include "bntseq.h"
#include "gmem_alloc.h"
#include "ksw.h"
#include "kstring_CUDA.cuh"

// Generate CIGAR when the alignment end points are known
__device__ uint32_t *bwa_gen_cigar2_gpu(const int8_t mat[25], int o_del, int e_del, int o_ins, int e_ins, int w_, int64_t l_pac, const uint8_t *pac, int l_query, uint8_t *query, int64_t rb, int64_t re, int *score, int *n_cigar, int *NM, void* d_buffer_ptr)
{
	uint32_t *cigar = 0;
	uint8_t tmp, *rseq;
	int i;
	int64_t rlen;
	kstring_t str;
	const char *int2base;

	if (n_cigar) *n_cigar = 0;
	if (NM) *NM = -1;
	if (l_query <= 0 || rb >= re || (rb < l_pac && re > l_pac)) return 0; // reject if negative length or bridging the forward and reverse strand
	rseq = bns_get_seq_gpu(l_pac, pac, rb, re, &rlen, d_buffer_ptr);
	if (re - rb != rlen) goto ret_gen_cigar; // possible if out of range
	if (rb >= l_pac) { // then reverse both query and rseq; this is to ensure indels to be placed at the leftmost position
		for (i = 0; i < l_query>>1; ++i)
			tmp = query[i], query[i] = query[l_query - 1 - i], query[l_query - 1 - i] = tmp;
		for (i = 0; i < rlen>>1; ++i)
			tmp = rseq[i], rseq[i] = rseq[rlen - 1 - i], rseq[rlen - 1 - i] = tmp;
	}
	if (l_query == re - rb && w_ == 0) { // no gap; no need to do DP
		// UPDATE: we come to this block now... FIXME: due to an issue in mem_reg2aln(), we never come to this block. This does not affect accuracy, but it hurts performance.
		if (n_cigar) {
			cigar = (uint32_t*)CUDAKernelMalloc(d_buffer_ptr, 4, 4);
			cigar[0] = l_query<<4 | 0;
			*n_cigar = 1;
		}
		for (i = 0, *score = 0; i < l_query; ++i)
			*score += mat[rseq[i]*5 + query[i]];
	} else {
		int w, max_gap, max_ins, max_del, min_w;
		// set the band-width
		max_ins = (int)((double)(((l_query+1)>>1) * mat[0] - o_ins) / e_ins + 1.);
		max_del = (int)((double)(((l_query+1)>>1) * mat[0] - o_del) / e_del + 1.);
		max_gap = max_ins > max_del? max_ins : max_del;
		max_gap = max_gap > 1? max_gap : 1;
		w = (max_gap + abs((int)rlen - l_query) + 1) >> 1;
		w = w < w_? w : w_;
		min_w = abs((int)rlen - l_query) + 3;
		w = w > min_w? w : min_w;
		// NW alignment
		*score = ksw_global2(l_query, query, rlen, rseq, 5, mat, o_del, e_del, o_ins, e_ins, w, n_cigar, &cigar, d_buffer_ptr);
	}
	if (NM && n_cigar) {// compute NM and MD
		int k, x, y, u, n_mm = 0, n_gap = 0;
		str.l = str.m = *n_cigar * 4; str.s = (char*)cigar; // append MD to CIGAR
		int2base = rb < l_pac? "ACGTN" : "TGCAN";
		for (k = 0, x = y = u = 0; k < *n_cigar; ++k) {
			int op, len;
			cigar = (uint32_t*)str.s;
			op  = cigar[k]&0xf, len = cigar[k]>>4;
			if (op == 0) { // match
				for (i = 0; i < len; ++i) {
					if (query[x + i] != rseq[y + i]) {
						kputw(u, &str, d_buffer_ptr);
						kputc(int2base[rseq[y+i]], &str, d_buffer_ptr);
						++n_mm; u = 0;
					} else ++u;
				}
				x += len; y += len;
			} else if (op == 2) { // deletion
				if (k > 0 && k < *n_cigar - 1) { // don't do the following if D is the first or the last CIGAR
					kputw(u, &str, d_buffer_ptr); kputc('^', &str, d_buffer_ptr);
					for (i = 0; i < len; ++i)
						kputc(int2base[rseq[y+i]], &str, d_buffer_ptr);
					u = 0; n_gap += len;
				}
				y += len;
			} else if (op == 1) x += len, n_gap += len; // insertion
		}
		kputw(u, &str, d_buffer_ptr); kputc(0, &str, d_buffer_ptr);
		*NM = n_mm + n_gap;
		cigar = (uint32_t*)str.s;
	}
	if (rb >= l_pac) // reverse back query
		for (i = 0; i < l_query>>1; ++i)
			tmp = query[i], query[i] = query[l_query - 1 - i], query[l_query - 1 - i] = tmp;

ret_gen_cigar:
	// free(rseq);
	return cigar;
}
