#include "ksw.h"
#include "macro.h"
#include "gmem_alloc.h"

__device__ const kswr_t g_defr = { 0, -1, -1, -1, -1, -1, -1 };

/**
 * Initialize the query data structure
 *
 * @param size   Number of bytes used to store a score; valid valures are 1 or 2
 * @param qlen   Length of the query sequence
 * @param query  Query sequence
 * @param m      Size of the alphabet
 * @param mat    Scoring matrix in a one-dimension array
 *
 * @return       Query data structure
 */
static __device__ kswq_t* ksw_qinit(int size, int qlen, const uint8_t *query, int m, const int8_t *mat, void* d_buffer_ptr)
{
	kswq_t *q;
	int slen, a, tmp, p;

	size = size > 1? 2 : 1;
	p = 8 * (3 - size); // # values per __m128i
	slen = (qlen + p - 1) / p; // segmented length
	q = (kswq_t*)CUDAKernelMalloc(d_buffer_ptr, sizeof(kswq_t) + 256 + 16 * slen * (m + 4), 8); // a single block of memory
	q->qp = (m128i*)(((size_t)q + sizeof(kswq_t) + 15) >> 4 << 4); // align memory
	q->H0 = q->qp + slen * m;
	q->H1 = q->H0 + slen;
	q->E  = q->H1 + slen;
	q->Hmax = q->E + slen;
	q->slen = slen; q->qlen = qlen; q->size = size;
	// compute shift
	tmp = m * m;
	for (a = 0, q->shift = 127, q->mdiff = 0; a < tmp; ++a) { // find the minimum and maximum score
		if (mat[a] < (int8_t)q->shift) q->shift = mat[a];
		if (mat[a] > (int8_t)q->mdiff) q->mdiff = mat[a];
	}
	q->max = q->mdiff;
	q->shift = 256 - q->shift; // NB: q->shift is uint8_t
	q->mdiff += q->shift; // this is the difference between the min and max scores
	// An example: p=8, qlen=19, slen=3 and segmentation:
	//  {{0,3,6,9,12,15,18,-1},{1,4,7,10,13,16,-1,-1},{2,5,8,11,14,17,-1,-1}}
	if (size == 1) {
		int8_t *t = (int8_t*)q->qp;
		for (a = 0; a < m; ++a) {
			int i, k, nlen = slen * p;
			const int8_t *ma = mat + a * m;
			for (i = 0; i < slen; ++i)
				for (k = i; k < nlen; k += slen) // p iterations
					*t++ = (k >= qlen? 0 : ma[query[k]]) + q->shift;
		}
	} else {
		int16_t *t = (int16_t*)q->qp;
		for (a = 0; a < m; ++a) {
			int i, k, nlen = slen * p;
			const int8_t *ma = mat + a * m;
			for (i = 0; i < slen; ++i)
				for (k = i; k < nlen; k += slen) // p iterations
					*t++ = (k >= qlen? 0 : ma[query[k]]);
		}
	}
	return q;
}

static inline __device__ int max_8(m128i xx){
	// return the max of the 8 integers
	int16_t max;
	max = (xx.x0>xx.x1) ? xx.x0: xx.x1;
	max = (max>xx.x2) ? max : xx.x2;
	max = (max>xx.x3) ? max : xx.x3;
	max = (max>xx.x4) ? max : xx.x4;
	max = (max>xx.x5) ? max : xx.x5;
	max = (max>xx.x6) ? max : xx.x6;
	max = (max>xx.x7) ? max : xx.x7;
	return (int)max;
}

static inline __device__ m128i set_value_m128i(int16_t val){
	// set all 8 integers to val
	m128i out;
	out.x0 = out.x1 = out.x2 = out.x3 = out.x4 = out.x5 = out.x6 = out.x7 = val;
	return out;
}

static inline __device__ m128i right_shift_2bytes(m128i a){
	// shift values in a to the left by 2 bytes
	a.x7 = a.x6;
	a.x6 = a.x5;
	a.x5 = a.x4;
	a.x4 = a.x3;
	a.x3 = a.x2;
	a.x2 = a.x1;
	a.x1 = a.x0;
	a.x0 = 0;
	return a;
}

static inline __device__ m128i adds_m128i(m128i a, m128i b){
	// add elements in a and b pairwise
	m128i out;
	out.x0 = a.x0 + b.x0;
	out.x1 = a.x1 + b.x1;
	out.x2 = a.x2 + b.x2;
	out.x3 = a.x3 + b.x3;
	out.x4 = a.x4 + b.x4;
	out.x5 = a.x5 + b.x5;
	out.x6 = a.x6 + b.x6;
	out.x7 = a.x7 + b.x7;
	return out;
}

static inline __device__ m128i max_m128i(m128i a, m128i b){
	// find pairwise max elements in a and b
	m128i out;
	out.x0 = (a.x0>b.x0) ? a.x0: b.x0;
	out.x1 = (a.x1>b.x1) ? a.x1: b.x1;
	out.x2 = (a.x2>b.x2) ? a.x2: b.x2;
	out.x3 = (a.x3>b.x3) ? a.x3: b.x3;
	out.x4 = (a.x4>b.x4) ? a.x4: b.x4;
	out.x5 = (a.x5>b.x5) ? a.x5: b.x5;
	out.x6 = (a.x6>b.x6) ? a.x6: b.x6;
	out.x7 = (a.x7>b.x7) ? a.x7: b.x7;
	return out;
}

static inline __device__ m128i subs_unsigned_m128i(m128i a, m128i b){
	// subtract pairwise, bounded by 0
	m128i out;
	out.x0 = (a.x0-b.x0)>0? (a.x0-b.x0): 0;
	out.x1 = (a.x1-b.x1)>0? (a.x1-b.x1): 0;
	out.x2 = (a.x2-b.x2)>0? (a.x2-b.x2): 0;
	out.x3 = (a.x3-b.x3)>0? (a.x3-b.x3): 0;
	out.x4 = (a.x4-b.x4)>0? (a.x4-b.x4): 0;
	out.x5 = (a.x5-b.x5)>0? (a.x5-b.x5): 0;
	out.x6 = (a.x6-b.x6)>0? (a.x6-b.x6): 0;
	out.x7 = (a.x7-b.x7)>0? (a.x7-b.x7): 0;
	return out;
}

static inline __device__ int compare_gt_m128i(m128i a, m128i b){
	// compare pairwise. return 0x1 if any element in a is greater than b
	// else return 0x0
	if (a.x0>b.x0) return 0x1;
	if (a.x1>b.x1) return 0x1;
	if (a.x2>b.x2) return 0x1;
	if (a.x3>b.x3) return 0x1;
	if (a.x4>b.x4) return 0x1;
	if (a.x5>b.x5) return 0x1;
	if (a.x6>b.x6) return 0x1;
	if (a.x7>b.x7) return 0x1;
	return 0x0;
}

static __device__ kswr_t ksw_i16(kswq_t *q, int tlen, const uint8_t *target, int _o_del, int _e_del, int _o_ins, int _e_ins, int xtra, void* d_buffer_ptr) // the first gap costs -(_o+_e)
{
// printf("unit test 0.0 q.qlen = %d q.slen = %d q.shift = %d\n", q->qlen, q->slen, q->shift);
// printf("unit test 0.0 tlen = %d \n", tlen);
// printf("unit test 0.0 _o_del = %d \n", _o_del);
// printf("unit test 0.0 _e_del = %d \n", _e_del);
// printf("unit test 0.0 _o_ins = %d \n", _o_ins);
// printf("unit test 0.0 ins = %d \n", _e_ins);
// printf("unit test 0.0 xtra = %d \n", xtra);

	int slen, i, m_b, n_b, te = -1, gmax = 0, minsc, endsc;
	uint64_t *b;
	m128i zero, oe_del, e_del, oe_ins, e_ins, *H0, *H1, *E, *Hmax;
	kswr_t r;

	// initialization
	r = g_defr;
	minsc = (xtra&KSW_XSUBO)? xtra&0xffff : 0x10000;
	endsc = (xtra&KSW_XSTOP)? xtra&0xffff : 0x10000;
	m_b = n_b = 0; b = 0;
	zero = set_value_m128i(0);
	oe_del = set_value_m128i(_o_del + _e_del);
	e_del = set_value_m128i(_e_del);
	oe_ins = set_value_m128i(_o_ins + _e_ins);
	e_ins = set_value_m128i(_e_ins);
	H0 = q->H0; H1 = q->H1; E = q->E; Hmax = q->Hmax;
	slen = q->slen;
	for (i = 0; i < slen; ++i) {
		*(E+i) = zero;
		*(H0+i) = zero;
		*(Hmax+i) = zero;
	}                      

	// the core loop
	for (i = 0; i < tlen; ++i) {
		int j, k, imax;
		m128i e, t, h, f = zero, max = zero, *S = q->qp + target[i] * slen; // s is the 1st score vector
		h = *(H0 + slen - 1); // h={2,5,8,11,14,17,-1,-1} in the above example
		h = right_shift_2bytes(h);

		for (j = 0; j < slen; ++j) {
// printf("j = %d\n", j);
			h = adds_m128i(h, *S++);
			e = *(E + j);
			h = max_m128i(h, e);
			h = max_m128i(h, f);
			max = max_m128i(max, h);
			*(m128i*)(H1 + j) =  h;
			e = subs_unsigned_m128i(e, e_del);
			t = subs_unsigned_m128i(h, oe_del);
			e = max_m128i(e, t);
			*(m128i*)(E + j) = e;
			f = subs_unsigned_m128i(f, e_ins);
			t = subs_unsigned_m128i(h, oe_ins);
			f = max_m128i(f, t);
			h = *(H0 + j);
		}
		for (k = 0; k < 16; ++k) {
			f = right_shift_2bytes(f);
			for (j = 0; j < slen; ++j) {
				h = *(H1 + j);
				h = max_m128i(h, f);
				*(m128i*)(H1 + j) = h;
				h = subs_unsigned_m128i(h, oe_ins);
				f = subs_unsigned_m128i(f, e_ins);
				if(!compare_gt_m128i(f, h)) goto end_loop8;
			}
		}

end_loop8:
		imax = max_8(max);
		if (imax >= minsc) {
			if (n_b == 0 || (int32_t)b[n_b-1] + 1 != i) {
				if (n_b == m_b) {
					m_b = m_b? m_b<<1 : 8;
					b = (uint64_t*)CUDAKernelRealloc(d_buffer_ptr, b, 8 * m_b, 8);
				}
				b[n_b++] = (uint64_t)imax<<32 | i;
			} else if ((int)(b[n_b-1]>>32) < imax) b[n_b-1] = (uint64_t)imax<<32 | i; // modify the last
		}
		if (imax > gmax) {
			gmax = imax; te = i;
			for (j = 0; j < slen; ++j)
				*(Hmax + j) = *(H1 + j);
			if (gmax >= endsc) break;
		}
		S = H1; H1 = H0; H0 = S;
	}
	r.score = gmax; r.te = te;
	{
		int max = -1, tmp, low, high, qlen = slen * 8;
		uint16_t *t = (uint16_t*)Hmax;
		for (i = 0, r.qe = -1; i < qlen; ++i, ++t)
			if ((int)*t > max) max = *t, r.qe = i / 8 + i % 8 * slen;
			else if ((int)*t == max && (tmp = i / 8 + i % 8 * slen) < r.qe) r.qe = tmp; 
		if (b) {
			i = (r.score + q->max - 1) / q->max;
			low = te - i; high = te + i;
			for (i = 0; i < n_b; ++i) {
				int e = (int32_t)b[i];
				if ((e < low || e > high) && (int)(b[i]>>32) > r.score2)
					r.score2 = b[i]>>32, r.te2 = e;
			}
		}
	}
// 	free(b);
	return r;
}

__device__ static inline void revseq(int l, uint8_t *s)
{
	int i, t;
	for (i = 0; i < l>>1; ++i)
		t = s[i], s[i] = s[l - 1 - i], s[l - 1 - i] = t;
}

__device__ kswr_t ksw_align2(int qlen, uint8_t *query, int tlen, uint8_t *target, int m, const int8_t *mat, int o_del, int e_del, int o_ins, int e_ins, int xtra, kswq_t **qry, void* d_buffer_ptr)
{
	int size;
	kswq_t *q;
	kswr_t r, rr;
printf("this part of code for long reads is under development \n"); __trap();
	q = (qry && *qry)? *qry : ksw_qinit((xtra&KSW_XBYTE)? 1 : 2, qlen, query, m, mat, d_buffer_ptr);
	if (qry && *qry == 0) *qry = q;
	// only using 16-bit integers func = ksw_i16;
	size = q->size;
	r = ksw_i16(q, tlen, target, o_del, e_del, o_ins, e_ins, xtra, d_buffer_ptr);
	// if (qry == 0) free(q);
	if ((xtra&KSW_XSTART) == 0 || ((xtra&KSW_XSUBO) && r.score < (xtra&0xffff))) return r;
	revseq(r.qe + 1, query); revseq(r.te + 1, target); // +1 because qe/te points to the exact end, not the position after the end
	q = ksw_qinit(size, r.qe + 1, query, m, mat, d_buffer_ptr);
	rr = ksw_i16(q, tlen, target, o_del, e_del, o_ins, e_ins, KSW_XSTOP | r.score, d_buffer_ptr);
	revseq(r.qe + 1, query); revseq(r.te + 1, target);
	// free(q);
	if (r.score == rr.score)
		r.tb = r.te - rr.te, r.qb = r.qe - rr.qe;
	return r;
}


/********************
 *** SW extension ***
 ********************/

typedef struct {
	int32_t h, e;
} eh_t;

__device__ int ksw_extend2(int qlen, const uint8_t *query, int tlen, const uint8_t *target, int m, const int8_t *mat, int o_del, int e_del, int o_ins, int e_ins, int w, int end_bonus, int zdrop, int h0, int *_qle, int *_tle, int *_gtle, int *_gscore, int *_max_off, void* d_buffer_ptr)
{
	eh_t *eh; // score array
	int8_t *qp; // query profile
	int i, j, k, oe_del = o_del + e_del, oe_ins = o_ins + e_ins, beg, end, max, max_i, max_j, max_ins, max_del, max_ie, gscore, max_off;
	// allocate memory
	qp = (int8_t*)CUDAKernelMalloc(d_buffer_ptr, qlen * m, 1);
	eh = (eh_t*)CUDAKernelCalloc(d_buffer_ptr, qlen + 1, 8, 4);
	// generate the query profile
	for (k = i = 0; k < m; ++k) {
		const int8_t *p = &mat[k * m];
		for (j = 0; j < qlen; ++j) qp[i++] = p[query[j]];
	}
	// fill the first row
	eh[0].h = h0; eh[1].h = h0 > oe_ins? h0 - oe_ins : 0;
	for (j = 2; j <= qlen && eh[j-1].h > e_ins; ++j)
		eh[j].h = eh[j-1].h - e_ins;
	// adjust $w if it is too large
	k = m * m;
	for (i = 0, max = 0; i < k; ++i) // get the max score
		max = max > mat[i]? max : mat[i];
	max_ins = (int)((double)(qlen * max + end_bonus - o_ins) / e_ins + 1.);
	max_ins = max_ins > 1? max_ins : 1;
	w = w < max_ins? w : max_ins;
	max_del = (int)((double)(qlen * max + end_bonus - o_del) / e_del + 1.);
	max_del = max_del > 1? max_del : 1;
	w = w < max_del? w : max_del; // TODO: is this necessary?
	// DP loop
	max = h0, max_i = max_j = -1; max_ie = -1, gscore = -1;
	max_off = 0;
	beg = 0, end = qlen;
	for (i = 0; i < tlen; ++i) {
		int t, f = 0, h1, m = 0, mj = -1;
		int8_t *q = &qp[target[i] * qlen];
		// apply the band and the constraint (if provided)
		if (beg < i - w) beg = i - w;
		if (end > i + w + 1) end = i + w + 1;
		if (end > qlen) end = qlen;
		// compute the first column
		if (beg == 0) {
			h1 = h0 - (o_del + e_del * (i + 1));
			if (h1 < 0) h1 = 0;
		} else h1 = 0;
		for (j = beg; j < end; ++j) {
			// At the beginning of the loop: eh[j] = { H(i-1,j-1), E(i,j) }, f = F(i,j) and h1 = H(i,j-1)
			// Similar to SSE2-SW, cells are computed in the following order:
			//   H(i,j)   = max{H(i-1,j-1)+S(i,j), E(i,j), F(i,j)}
			//   E(i+1,j) = max{H(i,j)-gapo, E(i,j)} - gape
			//   F(i,j+1) = max{H(i,j)-gapo, F(i,j)} - gape
			eh_t *p = &eh[j];
			int h, M = p->h, e = p->e; // get H(i-1,j-1) and E(i-1,j)
			p->h = h1;          // set H(i,j-1) for the next row
			M = M? M + q[j] : 0;// separating H and M to disallow a cigar like "100M3I3D20M"
			h = M > e? M : e;   // e and f are guaranteed to be non-negative, so h>=0 even if M<0
			h = h > f? h : f;
			h1 = h;             // save H(i,j) to h1 for the next column
			mj = m > h? mj : j; // record the position where max score is achieved
			m = m > h? m : h;   // m is stored at eh[mj+1]
			t = M - oe_del;
			t = t > 0? t : 0;
			e -= e_del;
			e = e > t? e : t;   // computed E(i+1,j)
			p->e = e;           // save E(i+1,j) for the next row
			t = M - oe_ins;
			t = t > 0? t : 0;
			f -= e_ins;
			f = f > t? f : t;   // computed F(i,j+1)
		}
		eh[end].h = h1; eh[end].e = 0;
		if (j == qlen) {
			max_ie = gscore > h1? max_ie : i;
			gscore = gscore > h1? gscore : h1;
		}
		if (m == 0) break;
		if (m > max) {
			max = m, max_i = i, max_j = mj;
			max_off = max_off > abs(mj - i)? max_off : abs(mj - i);
		} else if (zdrop > 0) {
			if (i - max_i > mj - max_j) {
				if (max - m - ((i - max_i) - (mj - max_j)) * e_del > zdrop) break;
			} else {
				if (max - m - ((mj - max_j) - (i - max_i)) * e_ins > zdrop) break;
			}
		}
		// update beg and end for the next round
		for (j = beg; j < end && eh[j].h == 0 && eh[j].e == 0; ++j);
		beg = j;
		for (j = end; j >= beg && eh[j].h == 0 && eh[j].e == 0; --j);
		end = j + 2 < qlen? j + 2 : qlen;
		//beg = 0; end = qlen; // uncomment this line for debugging
	}
	// free(eh); free(qp);
	if (_qle) *_qle = max_j + 1;
	if (_tle) *_tle = max_i + 1;
	if (_gtle) *_gtle = max_ie + 1;
	if (_gscore) *_gscore = gscore;
	if (_max_off) *_max_off = max_off;
	return max;
}

/* scoring of 2 characters given scoring matrix mat, and dimension m*/
__device__ static inline int score(uint8_t A, uint8_t B, const int8_t *mat, int m){
	return (int)mat[A*m+B];
}
/* SW extension executing at warp level
	BLOCKSIZE = WARPSIZE = 32
	requires at least qlen*4 bytes of shared memory
	currently implemented at 500*4 bytes of shared mem	
	return max score in the matrix, qle, tle, gtle, gscore
	NOTATIONS:
		SM_H[], SM_E: shared memory arrays for storing H and E of thread 31 for transitioning between tiles
		e, f, h     : E[i,j], F[i,j], H[i,j] to be calculated in an iteration
		e1_			: E[i-1,j] during a cell calculation
		h1_,h_1,h11 : // H[i-1,j], H[i,j-1], H[i-1,j-1]
		max_score   : the max score that a thread has found
		i_m, j_m	: the position where we found max_score
	CALCULATION:
		E[i,j] = max(H[i-1,j]-gap_open_penalty, E[i-1,j]-gap_ext_penalty)
		F[i,j] = max(H[i,j-1]-gap_open_penalty, F[i,j-1]-gap_ext_penalty)
		H[i,j] = max(0, E[i,j], F[i,j], H[i-1.j-1]+score(query[j],target[i]))
 */
#define ALL_THREADS 0xffffffff  // mask indicating all threads participate in shuffle instruction
#define max2(a,b) ((a)>(b)?(a):(b))

__device__ int ksw_extend_warp(int qlen, const uint8_t *query, int tlen, const uint8_t *target, int m, const int8_t *mat, int o_del, int e_del, int o_ins, int e_ins, int h0, int *_qle, int *_tle, int *_gtle, int *_gscore)
{
	if (qlen>KSW_MAX_QLEN){return 0;} //printf("querry length is too long %d \n", qlen); __trap();}
	__shared__	int16_t SM_H[KSW_MAX_QLEN], SM_E[KSW_MAX_QLEN];
	int e, f, h;
	int e1_;
	int h1_, h_1, h11;
	int max_score = h0;	// best score
	int i_m=-1, j_m=-1;	// position of best score
	int max_gscore = 0; // score of end-to-end alignment
	int i_gscore;	// position of best end-to-end alignment score

	// first row scoring
	for (int j=threadIdx.x; j<qlen; j+=WARPSIZE){	// j is col index
		SM_E[j] = 0;
		h = h0 - o_ins - e_ins - j*e_ins;
		SM_H[j] = (h>0)? h : 0;
	}
	// first we fill the top-left corner where we don't have enough parallelism
	f = 0;	// first column of F
	for (int anti_diag=0; anti_diag<WARPSIZE-1; anti_diag++){
		int i = threadIdx.x; 				// row index on the matrix
		int j = anti_diag - threadIdx.x;	// col index on the matrix
		// get previous cell data
		e1_ = __shfl_up_sync(ALL_THREADS, e, 1); // get e from threadIdx-1, which is E[i-1,j]
		if (threadIdx.x==0) e1_ = 0;
		h1_ = __shfl_up_sync(ALL_THREADS, h, 1); // h from threadID-1 is H[i-1,j]
		if (threadIdx.x==0) h1_ = SM_H[j];	   // but row 0 get initial scoring from shared mem
		h11 = __shfl_up_sync(ALL_THREADS, h_1, 1); // h_1 from threadID-1 is H[i-1,j-1]
		if (threadIdx.x==0 && j!=0) h11 = SM_H[j-1];	// row 0 get initial scoring from shared mem, except for first column
		if (threadIdx.x==0 && j==0) h11 = h0;			// H[-1,-1] = h0
		h_1 = h;							// H[i,j-1] from previous iteration of same thread 
		if (j==0) h_1 = h0 - o_ins - (i+1)*e_ins;		// first column score
		// calculate E[i,j], F[i,j], and H[i,j]
		if (i<tlen && j<qlen && j>=0){ 		// safety check for small matrix
			e = max2(h1_-o_del-e_del, e1_-e_del);
			f = max2(h_1-o_ins-e_ins, f-e_ins);
			h = h11 + score(target[i], query[j], mat, m);
			h = max2(0, h);
			int tmp = max2(e,f);
			h = max2(tmp, h);
			// record max scoring
			if (h>max_score){
				max_score = h; i_m = i; j_m = j;
			}
			if (j==qlen-1){	// we have hit last column
				if (h>max_gscore)	// record max to-end alignment score
					{max_gscore = h; i_gscore = i;}
			}
		}
	}

	// fill the rest of the matrix where we have enough parallelism
	int Ntile = ceil((float)tlen/WARPSIZE);
	int qlen_padded = qlen>=32? qlen : 32;	// pad qlen so that we have correct overflow for small matrix
	for (int tile_ID=0; tile_ID<Ntile; tile_ID++){	// tile loop
		int i, j;
		for (int anti_diag=WARPSIZE-1; anti_diag<qlen_padded+WARPSIZE-1; anti_diag++){	// anti-diagonal loop
			i = tile_ID*WARPSIZE + threadIdx.x;	// row index on matrix
			j = anti_diag - threadIdx.x; 		// col index
			if (j>=qlen_padded){			// when hit the end of this tile, overflow to next tile
				i = i+WARPSIZE;		// over flow to its row on the next tile
				j = j-qlen_padded;			// reset col index to the first 31 columns on next tile
			}
			// __syncwarp();
			// get previous cell data
			if (j==0) f = 0; 	// if we are processing first col, F[i,j-1] = 0. Otherwise, F[i,j-1] = f
			e1_ = __shfl_up_sync(ALL_THREADS, e, 1); 	// get e from threadIdx-1, which is E[i-1,j]
			if (threadIdx.x==0) e1_ = SM_E[j];	// thread 0 get E[i-1] from shared mem, which came from thread 31 of previous tile
			h1_ = __shfl_up_sync(ALL_THREADS, h, 1); 	// h from threadID-1 is H[i-1,j]
			if (threadIdx.x==0) h1_ = SM_H[j];	// but row 0 get initial scoring from shared mem, which came from thread 31 of previous tile
			h11 = __shfl_up_sync(ALL_THREADS, h_1, 1); // h_1 from threadID-1 is H[i-1,j-1]
			if (threadIdx.x==0 && j!=0) h11 = SM_H[j-1];	// thread 0 get H[i-1,j-1] from shared mem, which came from thread 31
			if (threadIdx.x==0 && j==0) h11 = h0 - o_ins - i*e_ins;	// first column scoring
			h_1 = h;							// H[i,j-1] from previous iteration of same thread 
			if (j==0) h_1 = h0 - o_ins - (i+1)*e_ins;	// first column score
			// calculate E[i,j], F[i,j], and H[i,j]
			if (i<tlen && j<qlen){ // j should be >=0
				e = max2(h1_-o_del-e_del, e1_-e_del);
				f = max2(h_1-o_ins-e_ins, f-e_ins);
				h = h11 + score(target[i], query[j], mat, m);
				h = max2(0, h);
				int tmp = max2(e,f);
				h = max2(tmp, h);
				// record max scoring
				if (h>max_score){
					max_score = h; i_m = i; j_m = j;
				}
				// thread 31 need to write h and e to shared memory to serve thread 0 in the next tile
				if (threadIdx.x==31){ SM_H[j] = h; SM_E[j] = e; }
				if (j==qlen-1){	// we have hit last column
					if (h>max_gscore)	// record max to-end alignment score
						{max_gscore = h; i_gscore = i;}
				}
			}
		}
	}
	// finished filling the matrix, now we find the max of max_score across the warp
	// use reduction to find the max of 32 max's
	for (int i=0; i<5; i++){
		int tmp = __shfl_down_sync(ALL_THREADS, max_score, 1<<i);
		int tmp_i = __shfl_down_sync(ALL_THREADS, i_m, 1<<i);
		int tmp_j = __shfl_down_sync(ALL_THREADS, j_m, 1<<i);
		if (max_score < tmp) {max_score = tmp; i_m = tmp_i; j_m = tmp_j;}
		tmp = __shfl_down_sync(ALL_THREADS, max_gscore, 1<<i);
		tmp_i = __shfl_down_sync(ALL_THREADS, i_gscore, 1<<i);
		if (max_gscore < tmp){max_gscore = tmp; i_gscore = tmp_i;}
	}

	// write max, i_m, j_m to global memory
	if (_qle) *_qle = j_m + 1;
	if (_tle) *_tle = i_m + 1;
	if (_gtle) *_gtle = i_gscore + 1;
	if (_gscore) *_gscore = max_gscore;
	return max_score;	// only thread 0's result is valid
}


/********************
 * Global alignment *
 ********************/
 #define MINUS_INF -0x40000000
 #define MINUS_INF16 -1000

 /* SW global executing at warp level
	BLOCKSIZE = WARPSIZE = 32
	requires at least qlen*4 bytes of shared memory
	currently implemented at 500*4 bytes of shared mem	
	return max score in the matrix, coordinates of max score, traceback matrix (we don't do traceback here because it's inefficient at warp level, should do at thread level)
	NOTATIONS:
		SM_H[], SM_E: shared memory arrays for storing H and E of thread 31 for transitioning between tiles
		e, f, h     : E[i,j], F[i,j], H[i,j] to be calculated in an iteration
		e1_			: E[i-1,j] during a cell calculation
		h1_,h_1,h11 : // H[i-1,j], H[i,j-1], H[i-1,j-1]
		max_score   : the max score that a thread has found
		i_m, j_m	: the position where we found max_score
		traceback   : traceback matrix. traceback[i,j] 	= 0 if score came from [i-1, j-1]	(cigar match)
														= 1 if score came from [i  , j-1]	(cigar insert to target)
														= 2 if score came from [i-1, j  ]	(cigar delete from target)
					traceback has to be allocated prior to calling this function, with at least tlen*qlen, row-major order
	CALCULATION:
		E[i,j] = max(H[i-1,j]-gap_open_penalty, E[i-1,j]-gap_ext_penalty)
		F[i,j] = max(H[i,j-1]-gap_open_penalty, F[i,j-1]-gap_ext_penalty)
		H[i,j] = max(0, E[i,j], F[i,j], H[i-1.j-1]+score(query[j],target[i]))
 */
__device__ int ksw_global_warp(int qlen, const uint8_t *query, int tlen, const uint8_t *target, int m, const int8_t *mat, int o_del, int e_del, int o_ins, int e_ins, int *i_max, int *j_max, uint8_t *traceback){
	if (qlen>KSW_MAX_QLEN){return 0;}//printf("querry length is too long %d \n", qlen); __trap();}
	__shared__	int16_t SM_H[KSW_MAX_QLEN], SM_E[KSW_MAX_QLEN];
	int e, f, h;
	int e1_;
	int h1_, h_1, h11;
	int max_score = 0;	// best score
	int i_m=-1, j_m=-1;	// position of best score

	// first row scoring
	for (int j=threadIdx.x; j<qlen; j+=WARPSIZE){	// j is col index
		SM_E[j] = MINUS_INF16;
		SM_H[j] = 0 - o_ins - e_ins - j*e_ins;
	}

	// first we fill the top-left corner where we don't have enough parallelism
	f = MINUS_INF16;	// first column of F
	for (int anti_diag=0; anti_diag<WARPSIZE-1; anti_diag++){
		int i = threadIdx.x; 				// row index on the matrix
		int j = anti_diag - threadIdx.x;	// col index on the matrix
		__syncwarp();
		if (i<tlen && j<qlen && j>=0){ 		// safety check for small matrix
			unsigned mask = __activemask();
			// get previous cell data
			e1_ = __shfl_up_sync(mask, e, 1); // get e from threadIdx-1, which is E[i-1,j]
			if (threadIdx.x==0) e1_ = 0;
			h1_ = __shfl_up_sync(mask, h, 1); // h from threadID-1 is H[i-1,j]
			if (threadIdx.x==0) h1_ = SM_H[j];	   // but row 0 get initial scoring from shared mem
			h11 = __shfl_up_sync(mask, h_1, 1); // h_1 from threadID-1 is H[i-1,j-1]
			if (threadIdx.x==0 && j!=0) h11 = SM_H[j-1];	// row 0 get initial scoring from shared mem, except for first column
			if (threadIdx.x==0 && j==0) h11 = 0;			// H[-1,-1] = 0
			h_1 = h;							// H[i,j-1] from previous iteration of same thread 
			if (j==0) h_1 = 0 - o_ins - (i+1)*e_ins;		// first column score
			// calculate E[i,j], F[i,j], and H[i,j]
			e = max2(h1_-o_del-e_del, e1_-e_del);
			f = max2(h_1-o_ins-e_ins, f-e_ins);
			h = h11 + score(target[i], query[j], mat, m);
			// record traceback
			if (h>=e && h>=f){	// traceback = 0 (match)
				// h = h
				traceback[i*qlen+j] = 0;
			} else if (f>=e){	// traceback = 1 (insert on ref)
				h = f;
				traceback[i*qlen+j] = 1;
			} else {			// traceback = 1 (delete from ref)
				h = e;
				traceback[i*qlen+j] = 2;
			}
			// record max scoring
			if (h>max_score){
				max_score = h; i_m = i; j_m = j;
			}
		}
	}

	// fill the rest of the matrix where we have enough parallelism
	int Ntile = ceil(float(tlen/WARPSIZE));
	int qlen_padded = qlen>=32? qlen : 32;	// pad qlen so that we have correct overflow for small matrix
	for (int tile_ID=0; tile_ID<Ntile; tile_ID++){	// tile loop
		int i, j;
		for (int anti_diag=WARPSIZE-1; anti_diag<qlen_padded+WARPSIZE-1; anti_diag++){	// anti-diagonal loop
			i = tile_ID*WARPSIZE + threadIdx.x;	// row index on matrix
			j = anti_diag - threadIdx.x; 		// col index
			if (j>=qlen_padded){			// when hit the end of this tile, overflow to next tile
				i = i+WARPSIZE;		// over flow to its row on the next tile
				j = j-qlen_padded;			// reset col index to the first 31 columns on next tile
			}
			__syncwarp();
			if (i<tlen && j<qlen){ // j should be >=0
				// get previous cell data
				if (j==0) f = 0; 	// if we are processing first col, F[i,j-1] = 0. Otherwise, F[i,j-1] = f
				unsigned mask = __activemask();
				e1_ = __shfl_up_sync(mask, e, 1); 	// get e from threadIdx-1, which is E[i-1,j]
				if (threadIdx.x==0) e1_ = SM_E[j];	// thread 0 get E[i-1] from shared mem, which came from thread 31 of previous tile
				h1_ = __shfl_up_sync(mask, h, 1); 	// h from threadID-1 is H[i-1,j]
				if (threadIdx.x==0) h1_ = SM_H[j];	// but row 0 get initial scoring from shared mem, which came from thread 31 of previous tile
				h11 = __shfl_up_sync(mask, h_1, 1); // h_1 from threadID-1 is H[i-1,j-1]
				if (threadIdx.x==0 && j!=0) h11 = SM_H[j-1];	// thread 0 get H[i-1,j-1] from shared mem, which came from thread 31
				if (threadIdx.x==0 && j==0) h11 = 0 - o_ins - i*e_ins;	// first column scoring
				h_1 = h;							// H[i,j-1] from previous iteration of same thread 
				if (j==0) h_1 = 0 - o_ins - (i+1)*e_ins;	// first column score
				// calculate E[i,j], F[i,j], and H[i,j]
				e = max2(h1_-o_del-e_del, e1_-e_del);
				f = max2(h_1-o_ins-e_ins, f-e_ins);
				h = h11 + score(target[i], query[j], mat, m);
				// record traceback
				if (h>=e && h>=f){	// traceback = 0 (match)
					// h = h
					traceback[i*qlen+j] = 0;
				} else if (f>=e){	// traceback = 1 (insert on ref)
					h = f;
					traceback[i*qlen+j] = 1;
				} else {			// traceback = 1 (delete from ref)
					h = e;
					traceback[i*qlen+j] = 2;
				}
				// record max scoring
				if (h>max_score){
					max_score = h; i_m = i; j_m = j;
				}
				// thread 31 need to write h and e to shared memory to serve thread 0 in the next tile
				if (threadIdx.x==31){ SM_H[j] = h; SM_E[j] = e; }
			}
		}
	}

	// finished filling the matrix, now we find the max of max_score across the warp
	// use reduction to find the max of 32 max's
	for (int i=0; i<5; i++){
		int tmp = __shfl_down_sync(ALL_THREADS, max_score, 1<<i);
		int tmp_i = __shfl_down_sync(ALL_THREADS, i_m, 1<<i);
		int tmp_j = __shfl_down_sync(ALL_THREADS, j_m, 1<<i);
		if (max_score < tmp) {max_score = tmp; i_m = tmp_i; j_m = tmp_j;}
	}

	// write max_score, i_m, j_m to output, only thread 0's result is valid
	if (i_max) *i_max = i_m;
	if (j_max) *j_max = j_m;
	return max_score;	// only thread 0's result is valid
}


__device__ static inline uint32_t *push_cigar(int *n_cigar, int *m_cigar, uint32_t *cigar, int op, int len, void* d_buffer_ptr)
{
	if (*n_cigar == 0 || op != (cigar[(*n_cigar) - 1]&0xf)) {
		if (*n_cigar == *m_cigar) {
			*m_cigar = *m_cigar? (*m_cigar)<<1 : 4;
			cigar = (uint32_t*)CUDAKernelRealloc(d_buffer_ptr, cigar, (*m_cigar) << 2, 4);
		}
		cigar[(*n_cigar)++] = len<<4 | op;
	} else cigar[(*n_cigar)-1] += len<<4;
	return cigar;
}

__device__ int ksw_global2(int qlen, const uint8_t *query, int tlen, const uint8_t *target, int m, const int8_t *mat, int o_del, int e_del, int o_ins, int e_ins, int w, int *n_cigar_, uint32_t **cigar_, void* d_buffer_ptr)
{
	eh_t *eh;
	int8_t *qp; // query profile
	int i, j, k, oe_del = o_del + e_del, oe_ins = o_ins + e_ins, score, n_col;
	uint8_t *z; // backtrack matrix; in each cell: f<<4|e<<2|h; in principle, we can halve the memory, but backtrack will be a little more complex
	if (n_cigar_) *n_cigar_ = 0;
	// allocate memory
	n_col = qlen < 2*w+1? qlen : 2*w+1; // maximum #columns of the backtrack matrix
	z = n_cigar_ && cigar_? (uint8_t*)CUDAKernelMalloc(d_buffer_ptr, (long)n_col * tlen, 1) : 0;
	qp = (int8_t*)CUDAKernelMalloc(d_buffer_ptr, qlen * m, 1);
	eh = (eh_t*)CUDAKernelCalloc(d_buffer_ptr, qlen + 1, 8, 4);
	// generate the query profile
	for (k = i = 0; k < m; ++k) {
		const int8_t *p = &mat[k * m];
		for (j = 0; j < qlen; ++j) qp[i++] = p[query[j]];
	}
	// fill the first row
	eh[0].h = 0; eh[0].e = MINUS_INF;
	for (j = 1; j <= qlen && j <= w; ++j)
		eh[j].h = -(o_ins + e_ins * j), eh[j].e = MINUS_INF;
	for (; j <= qlen; ++j) eh[j].h = eh[j].e = MINUS_INF; // everything is -inf outside the band
	// DP loop
	for (i = 0; i < tlen; ++i) { // target sequence is in the outer loop
		int32_t f = MINUS_INF, h1, beg, end, t;
		int8_t *q = &qp[target[i] * qlen];
		beg = i > w? i - w : 0;
		end = i + w + 1 < qlen? i + w + 1 : qlen; // only loop through [beg,end) of the query sequence
		h1 = beg == 0? -(o_del + e_del * (i + 1)) : MINUS_INF;
		if (n_cigar_ && cigar_) {
			uint8_t *zi = &z[(long)i * n_col];
			for (j = beg; j < end; ++j) {
				// At the beginning of the loop: eh[j] = { H(i-1,j-1), E(i,j) }, f = F(i,j) and h1 = H(i,j-1)
				// Cells are computed in the following order:
				//   M(i,j)   = H(i-1,j-1) + S(i,j)
				//   H(i,j)   = max{M(i,j), E(i,j), F(i,j)}
				//   E(i+1,j) = max{M(i,j)-gapo, E(i,j)} - gape
				//   F(i,j+1) = max{M(i,j)-gapo, F(i,j)} - gape
				// We have to separate M(i,j); otherwise the direction may not be recorded correctly.
				// However, a CIGAR like "10M3I3D10M" allowed by local() is disallowed by global().
				// Such a CIGAR may occur, in theory, if mismatch_penalty > 2*gap_ext_penalty + 2*gap_open_penalty/k.
				// In practice, this should happen very rarely given a reasonable scoring system.
				eh_t *p = &eh[j];
				int32_t h, m = p->h, e = p->e;
				uint8_t d; // direction
				p->h = h1;
				m += q[j];
				d = m >= e? 0 : 1;
				h = m >= e? m : e;
				d = h >= f? d : 2;
				h = h >= f? h : f;
				h1 = h;
				t = m - oe_del;
				e -= e_del;
				d |= e > t? 1<<2 : 0;
				e  = e > t? e    : t;
				p->e = e;
				t = m - oe_ins;
				f -= e_ins;
				d |= f > t? 2<<4 : 0; // if we want to halve the memory, use one bit only, instead of two
				f  = f > t? f    : t;
				zi[j - beg] = d; // z[i,j] keeps h for the current cell and e/f for the next cell
			}
		} else {
			for (j = beg; j < end; ++j) {
				eh_t *p = &eh[j];
				int32_t h, m = p->h, e = p->e;
				p->h = h1;
				m += q[j];
				h = m >= e? m : e;
				h = h >= f? h : f;
				h1 = h;
				t = m - oe_del;
				e -= e_del;
				e  = e > t? e : t;
				p->e = e;
				t = m - oe_ins;
				f -= e_ins;
				f  = f > t? f : t;
			}
		}
		eh[end].h = h1; eh[end].e = MINUS_INF;
	}
	score = eh[qlen].h;
	if (n_cigar_ && cigar_) { // backtrack
		int n_cigar = 0, m_cigar = 10, which = 0;
		uint32_t *cigar = (uint32_t*)CUDAKernelMalloc(d_buffer_ptr, m_cigar*sizeof(uint32_t), 4);
		uint32_t tmp;
		i = tlen - 1; k = (i + w + 1 < qlen? i + w + 1 : qlen) - 1; // (i,k) points to the last cell
		while (i >= 0 && k >= 0) {
			which = z[(long)i * n_col + (k - (i > w? i - w : 0))] >> (which<<1) & 3;
			if (which == 0)      cigar = push_cigar(&n_cigar, &m_cigar, cigar, 0, 1, d_buffer_ptr), --i, --k;
			else if (which == 1) cigar = push_cigar(&n_cigar, &m_cigar, cigar, 2, 1, d_buffer_ptr), --i;
			else                 cigar = push_cigar(&n_cigar, &m_cigar, cigar, 1, 1, d_buffer_ptr), --k;
		}
		if (i >= 0) cigar = push_cigar(&n_cigar, &m_cigar, cigar, 2, i + 1, d_buffer_ptr);
		if (k >= 0) cigar = push_cigar(&n_cigar, &m_cigar, cigar, 1, k + 1, d_buffer_ptr);
		for (i = 0; i < n_cigar>>1; ++i) // reverse CIGAR
			tmp = cigar[i], cigar[i] = cigar[n_cigar-1-i], cigar[n_cigar-1-i] = tmp;
		*n_cigar_ = n_cigar, *cigar_ = cigar;
	}
	// free(eh); free(qp); free(z);
	return score;
}
/*	Very naive implementation of banded Smith-Waterman extension
	Wastes some (perhaps many) threads over the boundary of the band.
*/ 
// FUTURE: Maybe I could increase the max query length? Because we don't need to store the E, H of entire row
__device__ int ksw_extend_warp2(int qlen, const uint8_t *query, int tlen, const uint8_t *target, int m, const int8_t *mat, int o_del, int e_del, int o_ins, int e_ins, int w, int end_bonus, int h0, int *_qle, int *_tle, int *_gtle, int *_gscore)
{
	if (qlen>KSW_MAX_QLEN){return 0;}//printf("%s:%d) querry length is too long %d \n", __FILE__, __LINE__, qlen); __trap();}
	__shared__	int16_t SM_H[KSW_MAX_QLEN], SM_E[KSW_MAX_QLEN], SM_done[1];
	int oe_del = o_del + e_del;
	int oe_ins = o_ins + e_ins;
	int e, f, h;
	int e1_;
	int h1_, h_1, h11;
	int max_score = h0;	// best score
	int i_m=-1, j_m=-1;	// position of best score
	int max_gscore = 0; // score of end-to-end alignment
	int i_gscore;	// position of best end-to-end alignment score
	int max_=0, max_ins, max_del, k;

	int h_sum = 0, e_sum = 0;

/* PART I */
	// first row scoring
	SM_H[0] = h0;
	for (int j=threadIdx.x+1; j<qlen+1; j+=WARPSIZE){	// j is col index
		SM_E[j] = 0;
		h = h0 - oe_ins - (j-1)*e_ins;
		SM_H[j] = (h>0)? h : 0;
	}
	// SM_done[0] = 0;

/* PART 2 */
	k = m*m;
	for (int i = 0; i < k; i++) {
		max_ = max_ > mat[i] ? max_ : mat[i];
	}
	max_ins = (int)((double)(qlen * max_ + end_bonus - o_ins) / e_ins + 1.);
	max_ins = max_ins > 1? max_ins : 1;
	w = w < max_ins? w : max_ins;
	max_del = (int)((double)(qlen * max_ + end_bonus - o_del) / e_del + 1.);
	max_del = max_del > 1? max_del : 1;
	w = w < max_del? w : max_del; // TODO: is this necessary?

/* PART 3 */
	// first we fill the top-left corner where we don't have enough parallelism
	for (int anti_diag=0; anti_diag<WARPSIZE-1; anti_diag++){
		int i = threadIdx.x; 				// row index on the matrix
		int j = anti_diag - threadIdx.x;	// col index on the matrix
		int M, t;

		e1_ = __shfl_up_sync(ALL_THREADS, e, 1);
		h1_ = __shfl_up_sync(ALL_THREADS, h, 1);
		h11 = __shfl_up_sync(ALL_THREADS, h_1, 1);
		h_1 = h;

		if (j == 0) {
			h_1 = h0 - (o_del + e_del*(i+1));
			if (h_1 < 0) h_1 = 0;
			f = 0;	// first column of F
		} //else h_1 = 0;

		if (threadIdx.x == 0) {
			e1_ = SM_E[j+1];
			h1_ = SM_H[j+1];
			h11 = SM_H[j];
		}

		if ((0 <= i) && (i < tlen) && (0 <= j) && (j < qlen) && (i-w<=j) && (j<=i+w+1)) {
			M = h11;
			M = M ? M + score(target[i], query[j], mat, m) : 0;
			h = max2(M, e1_);
			h = max2(h, f);
			if (h > max_score) {
				max_score = h;
				i_m = i;
				j_m = j;
			}
			else if (h == max_score) {
				if ((i == i_m) && (j > j_m))  {
					i_m = i;
					j_m = j;
				}
				else if ((i_m < 0) && (j_m < 0)) {
					i_m = i;
					j_m = j;
				}
			}

			t = M - oe_del;
			t = max2(t, 0);
			e = e1_ - e_del;
			e = max2(e, t);

			t = M - oe_ins;
			t = max2(t, 0);
			f -= e_ins;
			f = max2(f, t);

			if (threadIdx.x==WARPSIZE-1) {
				SM_H[j] = h;
				SM_E[j] = e;
			}
			// h_sum += h;
			// e_sum += e;
		}
		else {
			h = 0;
			e = 0;
			f = 0;
		}

	}

	// fill the rest of the matrix where we have enough parallelism
	int Ntile = ceil((float)tlen/WARPSIZE);
	int qlen_padded = qlen>=32? qlen : 32;	// pad qlen so that we have correct overflow for small matrix
	for (int tile_ID=0; tile_ID<Ntile; tile_ID++){	// tile loop
		int i, j;
		i = tile_ID*WARPSIZE + threadIdx.x;	// row index on matrix
		j = (WARPSIZE-1) - threadIdx.x; 		// col index
		for (int anti_diag=0; anti_diag<(tile_ID+1)*WARPSIZE+w+2 && anti_diag<qlen_padded; anti_diag++){	// anti-diagonal loop
			if ((j>=qlen_padded) || j>(tile_ID+1)*WARPSIZE+w+1){			// when hit the end of this tile, overflow to next tile
				// if ((h_sum == 0) && (e_sum == 0)) {			// early termination
				// 	SM_done[0] = 1;
				// }
				i = i+WARPSIZE;		// over flow to its row on the next tile
				j = 0;
				// h_sum = 0;
				// e_sum = 0;
			}

			int M, t;

			e1_ = __shfl_up_sync(ALL_THREADS, e, 1);
			h1_ = __shfl_up_sync(ALL_THREADS, h, 1);
			h11 = __shfl_up_sync(ALL_THREADS, h_1, 1);
			h_1 = h;

			if (j == 0) {
				h_1 = h0 - (o_del + e_del*(i+1));
				if (h_1 < 0) h_1 = 0;
				f = 0;	// first column of F
			} //else h_1 = 0;

			if (threadIdx.x == 0) {
				e1_ = SM_E[j];
				h1_ = SM_H[j+1];
				h11 = SM_H[j];
			}

			if ((0 <= i) && (i < tlen) && (0 <= j) && (j < qlen) && (i-w<=j)) {
				M = h11;
				M = M ? M + score(target[i], query[j], mat, m) : 0;
				h = max2(M, e1_);
				h = max2(h, f);

				if (h > max_score) {
					max_score = h;
					i_m = i;
					j_m = j;
				}
				else if (h == max_score) {
					if ((i == i_m))  {
						i_m = i;
						j_m = j;
					}
					else if ((i_m < 0) && (j_m < 0)) {
						i_m = i;
						j_m = j;
					}
				}

				t = M - oe_del;
				t = max2(t, 0);
				e = e1_ - e_del;
				e = max2(e, t);

				t = M - oe_ins;
				t = max2(t, 0);
				f -= e_ins;
				f = max2(f, t);

				if (threadIdx.x==WARPSIZE-1) {
					SM_H[j] = h_1;
					SM_E[j] = e;
				}
				// h_sum += h;
				// e_sum += e;
			}
			// if (SM_done[0] == 1) {		// early termination
			// 	break;
			// }
			j++;

		}
	}
	// finished filling the matrix, now we find the max of max_score across the warp
	// use reduction to find the max of 32 max's
	for (int i=0; i<5; i++){
		int tmp = __shfl_down_sync(ALL_THREADS, max_score, 1<<i);
		int tmp_i = __shfl_down_sync(ALL_THREADS, i_m, 1<<i);
		int tmp_j = __shfl_down_sync(ALL_THREADS, j_m, 1<<i);
		if (max_score < tmp) {max_score = tmp; i_m = tmp_i; j_m = tmp_j;}
		else if (max_score == tmp) {
			if (tmp_i < i_m) {
				i_m = tmp_i;
				j_m = tmp_j;
			}
			else if ((tmp_i == i_m) && (tmp_j > j_m))  {
				i_m = tmp_i;
				j_m = tmp_j;
			}
		}
		tmp = __shfl_down_sync(ALL_THREADS, max_gscore, 1<<i);
		tmp_i = __shfl_down_sync(ALL_THREADS, i_gscore, 1<<i);
		if (max_gscore < tmp){max_gscore = tmp; i_gscore = tmp_i;}
	}

	// write max, i_m, j_m to global memory
	if (_qle) *_qle = j_m + 1;
	if (_tle) *_tle = i_m + 1;
	if (_gtle) *_gtle = i_gscore + 1;
	if (_gscore) *_gscore = max_gscore;
	return max_score;	// only thread 0's result is valid
}

/********************
 * Global alignment *
 ********************/
 #define MINUS_INF -0x40000000
 #define MINUS_INF16 -1000

 /* SW global executing at warp level
	BLOCKSIZE = WARPSIZE = 32
	requires at least qlen*4 bytes of shared memory
	currently implemented at 500*4 bytes of shared mem	
	return max score in the matrix, coordinates of max score, traceback matrix (we don't do traceback here because it's inefficient at warp level, should do at thread level)
	NOTATIONS:
		SM_H[], SM_E: shared memory arrays for storing H and E of thread 31 for transitioning between tiles
		e, f, h     : E[i,j], F[i,j], H[i,j] to be calculated in an iteration
		e1_			: E[i-1,j] during a cell calculation
		h1_,h_1,h11 : // H[i-1,j], H[i,j-1], H[i-1,j-1]
		max_score   : the max score that a thread has found
		i_m, j_m	: the position where we found max_score
		traceback   : traceback matrix. traceback[i,j] 	= 0 if score came from [i-1, j-1]	(cigar match)
														= 1 if score came from [i  , j-1]	(cigar insert to target)
														= 2 if score came from [i-1, j  ]	(cigar delete from target)
					traceback has to be allocated prior to calling this function, with at least tlen*qlen, row-major order
	CALCULATION:
		E[i,j] = max(H[i-1,j]-gap_open_penalty, E[i-1,j]-gap_ext_penalty)
		F[i,j] = max(H[i,j-1]-gap_open_penalty, F[i,j-1]-gap_ext_penalty)
		H[i,j] = max(0, E[i,j], F[i,j], H[i-1.j-1]+score(query[j],target[i]))
 */

// Implementation of improved traceback from Graduation Project of 2023-2
#define GRIDSIZE 16
__device__ int ksw_global3(int qlen, const uint8_t *query, int tlen, const uint8_t *target, int m, const int8_t *mat, int o_del, int e_del, int o_ins, int e_ins, int w, int *n_cigar_, uint32_t **cigar_, void* d_buffer_ptr)
{
	__shared__ int32_t SM_h[2*GRIDSIZE-1];	// shared memory to store border values during traceback
	__shared__ int32_t SM_e[GRIDSIZE];	// shared memory to store border values during traceback
	__shared__ int32_t SM_f[GRIDSIZE];	// shared memory to store border values during traceback
	uint8_t direction[(GRIDSIZE)*(GRIDSIZE)];

	eh_t *eh;
	int8_t *qp; // query profile
	int i, j, k, oe_del = o_del + e_del, oe_ins = o_ins + e_ins, score;
	if (n_cigar_) *n_cigar_ = 0;
	
	int gridIdx_i, gridIdx_j, gridIdx;
	unsigned long n_gridcol = max((int)ceil((float)(qlen)/(GRIDSIZE)), 0);
	unsigned long n_gridrow = max((int)ceil((float)(tlen)/(GRIDSIZE)), 0);
	unsigned long n_grid = n_gridcol*n_gridrow;

	int32_t *border_h = (int32_t*)CUDAKernelMalloc(d_buffer_ptr, (2*GRIDSIZE+1)*n_grid*sizeof(int32_t), 1);
	int32_t *border_e = (int32_t*)CUDAKernelMalloc(d_buffer_ptr, (GRIDSIZE+1)*n_grid*sizeof(int32_t), 1);
	int32_t *border_f = (int32_t*)CUDAKernelMalloc(d_buffer_ptr, (GRIDSIZE+1)*n_grid*sizeof(int32_t), 1);

	qp = (int8_t*)CUDAKernelMalloc(d_buffer_ptr, qlen * m, 1);
	eh = (eh_t*)CUDAKernelCalloc(d_buffer_ptr, qlen + 1, 8, 4);
	
	// generate the query profile
	for (k = i = 0; k < m; ++k) {
		const int8_t *p = &mat[k * m];
		for (j = 0; j < qlen; ++j) qp[i++] = p[query[j]];
	}
	// fill the first row
	eh[0].h = 0; eh[0].e = MINUS_INF;
	for (j = 1; j <= qlen && j <= w; ++j)
		eh[j].h = -(o_ins + e_ins * j), eh[j].e = MINUS_INF;
	for (; j <= qlen; ++j) eh[j].h = eh[j].e = MINUS_INF; // everything is -inf outside the band
	// DP loop
    if(target != 0) {
	for (i = 0; i < tlen; ++i) { // target sequence is in the outer loop
		// int32_t f = MINUS_INF, h1, beg, end, t;
		int8_t f = MINUS_INF, h1, beg, end, t;
		int8_t *q = &qp[target[i] * qlen];
		beg = i > w? i - w : 0;
		end = i + w + 1 < qlen? i + w + 1 : qlen; // only loop through [beg,end) of the query sequence
		
		h1 = beg == 0? -(o_del + e_del * (i + 1)) : MINUS_INF;
		if (n_cigar_ && cigar_) {
			for (j = beg; j < end; ++j) {
				// At the beginning of the loop: eh[j] = { H(i-1,j-1), E(i,j) }, f = F(i,j) and h1 = H(i,j-1)
				// Cells are computed in the following order:
				//   M(i,j)   = H(i-1,j-1) + S(i,j)
				//   H(i,j)   = max{M(i,j), E(i,j), F(i,j)}
				//   E(i+1,j) = max{M(i,j)-gapo, E(i,j)} - gape
				//   F(i,j+1) = max{M(i,j)-gapo, F(i,j)} - gape
				// We have to separate M(i,j); otherwise the direction may not be recorded correctly.
				// However, a CIGAR like "10M3I3D10M" allowed by local() is disallowed by global().
				// Such a CIGAR may occur, in theory, if mismatch_penalty > 2*gap_ext_penalty + 2*gap_open_penalty/k.
				// In practice, this should happen very rarely given a reasonable scoring system.

				eh_t *p = &eh[j];
				int32_t h, m = p->h, e = p->e;
				uint8_t d; // direction
				// save score if the cell is on the grid border
				if ((i % GRIDSIZE == 0) || (j % GRIDSIZE == 0)) {
					// idx of the grid
					gridIdx_i = i/GRIDSIZE;
					gridIdx_j = j/GRIDSIZE;
					gridIdx = (gridIdx_i*n_gridcol + gridIdx_j);
					int borderIdx;	// idx of the cell within the grid
					if ((i%GRIDSIZE != 0) && (j%GRIDSIZE == 0)) {		// column
						border_f[gridIdx*(GRIDSIZE) + (i%GRIDSIZE)] = f;
						borderIdx = (i%GRIDSIZE)+GRIDSIZE-1;
					}
					else if ((i%GRIDSIZE == 0) && (j%GRIDSIZE != 0)) {		// row
						border_e[gridIdx*(GRIDSIZE) + (j%GRIDSIZE)] = e;
						borderIdx = (j%GRIDSIZE);
					}
					else {	// corner
						border_f[gridIdx*(GRIDSIZE) + (i%GRIDSIZE)] = f;
						border_e[gridIdx*(GRIDSIZE) + (j%GRIDSIZE)] = e;
						
						borderIdx = (j%GRIDSIZE);
					}

					border_h[gridIdx*(2*GRIDSIZE-1) + borderIdx] = m;
				}
				p->h = h1;
				m += q[j];
				d = m >= e? 0 : 1;
				h = m >= e? m : e;
				d = h >= f? d : 2;
				h = h >= f? h : f;
				h1 = h;

				t = m - oe_del;
				e -= e_del;
				d |= e > t? 1<<2 : 0;
				e  = e > t? e    : t;
				p->e = e;
				t = m - oe_ins;
				f -= e_ins;
				d |= f > t? 2<<4 : 0; // if we want to halve the memory, use one bit only, instead of two
				f  = f > t? f    : t;
			}
			
		} else {
			for (j = beg; j < end; ++j) {
				eh_t *p = &eh[j];
				int32_t h, m = p->h, e = p->e;
				p->h = h1;
				m += q[j];
				h = m >= e? m : e;
				h = h >= f? h : f;
				h1 = h;
				t = m - oe_del;
				e -= e_del;
				e  = e > t? e : t;
				p->e = e;
				t = m - oe_ins;
				f -= e_ins;
				f  = f > t? f : t;
			}
		}
		eh[end].h = h1; eh[end].e = MINUS_INF;
		
	}
    }
	score = eh[qlen].h;

	if (n_cigar_ && cigar_) { // backtrack
		int n_cigar = 0, m_cigar = 10, which = 0;
		uint32_t *cigar = (uint32_t*)CUDAKernelMalloc(d_buffer_ptr, m_cigar*sizeof(uint32_t), 4);
		uint32_t tmp;

		// k = j;
		i = tlen - 1; k = (i + w + 1 < qlen? i + w + 1 : qlen) - 1; // (i,k) points to the last cell

		gridIdx_i = (i/GRIDSIZE);
		gridIdx_j = (k/GRIDSIZE);

		int cornerIdx_i;
		int cornerIdx_j;
		int gridIdx;

		while (i >= 0 && k >= 0) {
			cornerIdx_i = gridIdx_i*GRIDSIZE;
			cornerIdx_j = gridIdx_j*GRIDSIZE;
			gridIdx = gridIdx_i*n_gridcol + gridIdx_j;
			// load global data to shared mem
			for (int idx = 0; idx < 2*GRIDSIZE-1; idx++) {
				SM_h[idx] = border_h[(gridIdx)*(2*GRIDSIZE-1) + idx];
			}

			for (int idx = 0; idx < GRIDSIZE; idx++) {
				SM_e[idx] = border_e[gridIdx*(GRIDSIZE)+idx];
			}
			for (int idx = 0; idx < GRIDSIZE; idx++) {
				SM_f[idx] = border_f[gridIdx*(GRIDSIZE)+idx];
			}

			// grid filling
			int i_, j_;
			for (i_ = 0; i_+cornerIdx_i <= i; i_++) {
				int8_t *q = &qp[target[i_+cornerIdx_i] * qlen];

				int32_t h;
				int32_t h11 = SM_h[0];						// H[i-1, j-1]
				int32_t h10 = SM_h[1];						// H[i-1, j]
				int32_t h01 = SM_h[i_ + GRIDSIZE];		// H[i,   j-1]
				int32_t e = SM_e[0];
				int32_t f = SM_f[i_];

				for (j_ = 0; j_+cornerIdx_j <= k; j_++) {
					if ((i_+cornerIdx_i+w <= j_+cornerIdx_j) || (i_+cornerIdx_i > j_+cornerIdx_j + w)) {
						f = MINUS_INF;
						h11 = SM_h[j_+1];
						continue;
					}
					if (i_+cornerIdx_i == j_+cornerIdx_j + w) {f = MINUS_INF;}
					h10 = SM_h[j_+1];
					e = SM_e[j_];


					int32_t m = h11;
					uint8_t d; // direction
					m += q[j_+cornerIdx_j];
					d = m >= e? 0 : 1;
					h = m >= e? m : e;
					d = h >= f? d : 2;
					h = h >= f? h : f;
					
					// update h11, h01 for next iteration
					SM_h[j_] = h01;
					h11 = h10;
					h01 = h;

					int t = m - oe_del;
					e -= e_del;
					d |= e > t? 1<<2 : 0;
					e  = e > t? e    : t;
					SM_e[j_] = e;
					t = m - oe_ins;
					f -= e_ins;
					d |= f > t? 2<<4 : 0; // if we want to halve the memory, use one bit only, instead of two
					f  = f > t? f    : t;
					direction[(i_)*GRIDSIZE + (j_)] = d;
					
				}
			}

			// grid traceback
			i_ = i - cornerIdx_i;
			j_ = k - cornerIdx_j;
			
			while((i_ >= 0) && (j_ >= 0) && (i_+cornerIdx_i >= 0) && (j_+cornerIdx_j >= 0)) {
				which = direction[(i_)*GRIDSIZE + (j_)] >> (which<<1) & 3;
				if (which == 0)      cigar = push_cigar(&n_cigar, &m_cigar, cigar, 0, 1, d_buffer_ptr), --i_, --j_;
				else if (which == 1) cigar = push_cigar(&n_cigar, &m_cigar, cigar, 2, 1, d_buffer_ptr), --i_;
				else                 cigar = push_cigar(&n_cigar, &m_cigar, cigar, 1, 1, d_buffer_ptr), --j_;
			}

			
			i = i_ + cornerIdx_i;
			k = j_ + cornerIdx_j;

			if (i_ < 0) {gridIdx_i--;}
			if (j_ < 0) {gridIdx_j--;}
		}

		if (i >= 0) cigar = push_cigar(&n_cigar, &m_cigar, cigar, 2, i, d_buffer_ptr);
		if (k >= 0) cigar = push_cigar(&n_cigar, &m_cigar, cigar, 1, k, d_buffer_ptr);
		for (i = 0; i < n_cigar>>1; ++i) // reverse CIGAR
			tmp = cigar[i], cigar[i] = cigar[n_cigar-1-i], cigar[n_cigar-1-i] = tmp;
		*n_cigar_ = n_cigar, *cigar_ = cigar;
	}
	return score;
}
