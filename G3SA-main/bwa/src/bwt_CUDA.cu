/* The MIT License

   Copyright (c) 2008 Genome Research Ltd (GRL).

   Permission is hereby granted, free of charge, to any person obtaining
   a copy of this software and associated documentation files (the
   "Software"), to deal in the Software without restriction, including
   without limitation the rights to use, copy, modify, merge, publish,
   distribute, sublicense, and/or sell copies of the Software, and to
   permit persons to whom the Software is furnished to do so, subject to
   the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
   BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
   ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.
*/

/* Contact: Heng Li <lh3@sanger.ac.uk> */

#include "bwt_CUDA.cuh"


#define OCC_INTV_SHIFT 7
#define OCC_INTERVAL   (1LL<<OCC_INTV_SHIFT)
#define OCC_INTV_MASK  (OCC_INTERVAL - 1)
   
#define bwt_occ_intv(b, k) ((b)->bwt + ((k)>>7<<4))


__device__ static inline int __occ_aux(uint64_t y, int c)
{
	// reduce nucleotide counting to bits counting
	y = ((c&2)? y : ~y) >> 1 & ((c&1)? y : ~y) & 0x5555555555555555ull;
	// count the number of 1s in y
	y = (y & 0x3333333333333333ull) + (y >> 2 & 0x3333333333333333ull);
	return ((y + (y >> 4)) & 0xf0f0f0f0f0f0f0full) * 0x101010101010101ull >> 56;
}

__device__ static bwtint_t bwt_occ_gpu(const bwt_t *bwt, bwtint_t k, ubyte_t c)
{
	bwtint_t n;
	uint32_t *p, *end;

	if (k == bwt->seq_len) return bwt->L2[c+1] - bwt->L2[c];
	if (k == (bwtint_t)(-1)) return 0;
	k -= (k >= bwt->primary); // because $ is not in bwt

	// retrieve Occ at k/OCC_INTERVAL
	n = ((bwtint_t*)(p = bwt_occ_intv(bwt, k)))[c];
	p += sizeof(bwtint_t); // jump to the start of the first BWT cell

	// calculate Occ up to the last k/32
	end = p + (((k>>5) - ((k&~OCC_INTV_MASK)>>5))<<1);
	for (; p < end; p += 2) n += __occ_aux((uint64_t)p[0]<<32 | p[1], c);

	// calculate Occ
	n += __occ_aux(((uint64_t)p[0]<<32 | p[1]) & ~((1ull<<((~k&31)<<1)) - 1), c);
	if (c == 0) n -= ~k&31; // corrected for the masked bits

	return n;
}

// an analogy to bwt_occ_gpu() but more efficient, requiring k <= l
// __device__ static void bwt_2occ_gpu(const bwt_t *bwt, bwtint_t k, bwtint_t l, ubyte_t c, bwtint_t *ok, bwtint_t *ol)
// {
// 	bwtint_t _k, _l;
// 	_k = (k >= bwt->primary)? k-1 : k;
// 	_l = (l >= bwt->primary)? l-1 : l;
// 	if (_l/OCC_INTERVAL != _k/OCC_INTERVAL || k == (bwtint_t)(-1) || l == (bwtint_t)(-1)) {
// 		*ok = bwt_occ_gpu(bwt, k, c);
// 		*ol = bwt_occ_gpu(bwt, l, c);
// 	} else {
// 		bwtint_t m, n, i, j;
// 		uint32_t *p;
// 		if (k >= bwt->primary) --k;
// 		if (l >= bwt->primary) --l;
// 		n = ((bwtint_t*)(p = bwt_occ_intv(bwt, k)))[c];
// 		p += sizeof(bwtint_t);
// 		// calculate *ok
// 		j = k >> 5 << 5;
// 		for (i = k/OCC_INTERVAL*OCC_INTERVAL; i < j; i += 32, p += 2)
// 			n += __occ_aux((uint64_t)p[0]<<32 | p[1], c);
// 		m = n;
// 		n += __occ_aux(((uint64_t)p[0]<<32 | p[1]) & ~((1ull<<((~k&31)<<1)) - 1), c);
// 		if (c == 0) n -= ~k&31; // corrected for the masked bits
// 		*ok = n;
// 		// calculate *ol
// 		j = l >> 5 << 5;
// 		for (; i < j; i += 32, p += 2)
// 			m += __occ_aux((uint64_t)p[0]<<32 | p[1], c);
// 		m += __occ_aux(((uint64_t)p[0]<<32 | p[1]) & ~((1ull<<((~l&31)<<1)) - 1), c);
// 		if (c == 0) m -= ~l&31; // corrected for the masked bits
// 		*ol = m;
// 	}
// }

#define __occ_aux4(bwt, b)											\
	((bwt)->cnt_table[(b)&0xff] + (bwt)->cnt_table[(b)>>8&0xff]		\
	 + (bwt)->cnt_table[(b)>>16&0xff] + (bwt)->cnt_table[(b)>>24])

__device__ static void bwt_occ4_gpu(const bwt_t *bwt, bwtint_t k, bwtint_t cnt[4])
{
	bwtint_t x;
	uint32_t *p, tmp, *end;
	if (k == (bwtint_t)(-1)) {
		memset(cnt, 0, 4 * sizeof(bwtint_t));
		return;
	}
	k -= (k >= bwt->primary); // because $ is not in bwt
	p = bwt_occ_intv(bwt, k);
	cudaKernelMemcpy(p, cnt, 4 * sizeof(bwtint_t));
	p += sizeof(bwtint_t); // sizeof(bwtint_t) = 4*(sizeof(bwtint_t)/sizeof(uint32_t))
	end = p + ((k>>4) - ((k&~OCC_INTV_MASK)>>4)); // this is the end point of the following loop
	for (x = 0; p < end; ++p) x += __occ_aux4(bwt, *p);
	tmp = *p & ~((1U<<((~k&15)<<1)) - 1);
	x += __occ_aux4(bwt, tmp) - (~k&15);
	cnt[0] += x&0xff; cnt[1] += x>>8&0xff; cnt[2] += x>>16&0xff; cnt[3] += x>>24;
}

// an analogy to bwt_occ4_gpu() but more efficient, requiring k <= l
__device__ static void bwt_2occ4_gpu(const bwt_t *bwt, bwtint_t k, bwtint_t l, bwtint_t cntk[4], bwtint_t cntl[4])
{
	bwtint_t _k, _l;
	_k = k - (k >= bwt->primary);
	_l = l - (l >= bwt->primary);
	if (_l>>OCC_INTV_SHIFT != _k>>OCC_INTV_SHIFT || k == (bwtint_t)(-1) || l == (bwtint_t)(-1)) {
		bwt_occ4_gpu(bwt, k, cntk);
		bwt_occ4_gpu(bwt, l, cntl);
	} else {
		bwtint_t x, y;
		uint32_t *p, tmp, *endk, *endl;
		k -= (k >= bwt->primary); // because $ is not in bwt
		l -= (l >= bwt->primary);
		p = bwt_occ_intv(bwt, k);
		cudaKernelMemcpy(p, cntk, 4 * sizeof(bwtint_t));
		p += sizeof(bwtint_t); // sizeof(bwtint_t) = 4*(sizeof(bwtint_t)/sizeof(uint32_t))
		// prepare cntk[]
		endk = p + ((k>>4) - ((k&~OCC_INTV_MASK)>>4));
		endl = p + ((l>>4) - ((l&~OCC_INTV_MASK)>>4));
		for (x = 0; p < endk; ++p) x += __occ_aux4(bwt, *p);
		y = x;
		tmp = *p & ~((1U<<((~k&15)<<1)) - 1);
		x += __occ_aux4(bwt, tmp) - (~k&15);
		// calculate cntl[] and finalize cntk[]
		for (; p < endl; ++p) y += __occ_aux4(bwt, *p);
		tmp = *p & ~((1U<<((~l&15)<<1)) - 1);
		y += __occ_aux4(bwt, tmp) - (~l&15);
		cudaKernelMemcpy(cntk, cntl, 4 * sizeof(bwtint_t));
		cntk[0] += x&0xff; cntk[1] += x>>8&0xff; cntk[2] += x>>16&0xff; cntk[3] += x>>24;
		cntl[0] += y&0xff; cntl[1] += y>>8&0xff; cntl[2] += y>>16&0xff; cntl[3] += y>>24;
	}
}

/*********************
 * Bidirectional BWT *
 *********************/

__device__ static void bwt_extend_gpu(const bwt_t *bwt, const bwtintv_t *ik, bwtintv_t ok[4], int is_back)
{
	bwtint_t tk[4], tl[4];
	int i;
	bwt_2occ4_gpu(bwt, ik->x[!is_back] - 1, ik->x[!is_back] - 1 + ik->x[2], tk, tl);
	for (i = 0; i != 4; ++i) {
		ok[i].x[!is_back] = bwt->L2[i] + 1 + tk[i];
		ok[i].x[2] = tl[i] - tk[i];
	}
	ok[3].x[is_back] = ik->x[is_back] + (ik->x[!is_back] <= bwt->primary && ik->x[!is_back] + ik->x[2] - 1 >= bwt->primary);
	ok[2].x[is_back] = ok[3].x[is_back] + ok[3].x[2];
	ok[1].x[is_back] = ok[2].x[is_back] + ok[2].x[2];
	ok[0].x[is_back] = ok[1].x[is_back] + ok[1].x[2];
}

__device__ static void bwt_reverse_intvs(bwtintv_v *p)
{
	if (p->n > 1) {
		int j;
		for (j = 0; j < p->n>>1; ++j) {
			bwtintv_t tmp = p->a[p->n - 1 - j];
			p->a[p->n - 1 - j] = p->a[j];
			p->a[j] = tmp;
		}
	}
}

extern __device__ __constant__ unsigned char d_nst_nt4_table[256];
#define d_charToInt(c) (d_nst_nt4_table[(int)c])	// for device code only
#define d_intToChar(x) ("ACGTN"[(x)])
__device__ int d_hashK(const uint8_t* s){
    int out = 0;
    for (int i=0; i<KMER_K; i++){
        if (s[i]==4) return -1;
        out += s[i]*pow4(KMER_K-1-i);
    }
    return out;
}

// generate initial bwt intervals for sub-sequence of length K starting from position i
// write result to *interval, return success/failure
__device__ bool bwt_KMerHashInit(int qlen, const uint8_t *q, int i, kmers_bucket_t *d_kmersHashTab, bwtintv_lite_t *interval){
	if (i>qlen-1-KMER_K) return false;	// not enough space to the right for extension
	int hashValue = d_hashK(&q[i]);
	if (hashValue==-1) return false;	// hash N in this substring
	kmers_bucket_t entry = d_kmersHashTab[hashValue];
	interval->x0 = entry.x[0]; interval->x1 = entry.x[1]; interval->x2 = entry.x[2];
	interval->start = i;
	interval->end = i+KMER_K-1;
	return true;
}

// extend 1 to the right, write result in-place. interval is input and output. Return true if successfully extended, false otherwise
// if extend is unsuccessful, *interval is not modified
__device__ bool bwt_extend_right1(const bwt_t *bwt, int qlen, const uint8_t *q, int min_intv, uint64_t max_intv, bwtintv_lite_t *interval){
	int end = interval->end;
	if (end==qlen-1) return false;
	int nextBase = q[end+1];
	if (nextBase==4) return false; // ambiguous base
	
	bwtintv_t ik, ok[4];
	ik.x[0] = interval->x0;
	ik.x[1] = interval->x1;
	ik.x[2] = interval->x2;
	if (ik.x[2]<max_intv) return false;	// an interval small enough

	// try to extend
	int c = 3 - nextBase;	// complement
	bwt_extend_gpu(bwt, &ik, ok, 0);
	if (ok[c].x[2] < min_intv) return false; // the interval size is too small to be extended further
	// otherwise, success extend, write output to *interval
	interval->x0 = ok[c].x[0];
	interval->x1 = ok[c].x[1];
	interval->x2 = ok[c].x[2];
	interval->end = interval->end + 1;

	return true;
}


// extend furthest to the right from a position and save that one seed
__device__ void bwt_smem_right(const bwt_t *bwt, int len, const uint8_t *q, int x, int min_intv, uint64_t max_intv, int min_seed_len, bwtintv_t *mem_a, kmers_bucket_t *d_kmersHashTab)
{
	bwtintv_t ik, ok[4];
	if (min_intv < 1) min_intv = 1; // the interval size should be at least 1
	bwt_set_intv(bwt, q[x], ik); 	// the initial interval of a single base
	// load interval for first K base from hash table
	if (x>len-1-KMER_K) return;	// not enough space to the right for extension
	int hashValue = d_hashK(&q[x]);
	if (hashValue==-1) return;	// hash N in this substring
	kmers_bucket_t ikK = d_kmersHashTab[hashValue];
	ik.x[0] = ikK.x[0]; ik.x[1] = ikK.x[1]; ik.x[2] = ikK.x[2];
	ik.info = ((uint64_t)x<<32) | ((uint64_t)(x+KMER_K-1));

	int i;
	for (i = x + KMER_K; i < len; ++i) { // forward search
		if (ik.x[2] < max_intv) { // an interval small enough
			break;
		} else if (q[i] < 4) { // an A/C/G/T base
			int c = 3 - q[i]; // complement of q[i]
			bwt_extend_gpu(bwt, &ik, ok, 0);
			if (ok[c].x[2] < min_intv) break; // the interval size is too small to be extended further
			ik = ok[c]; 	// keep going
		} else { // an ambiguous base
			break; // always terminate extension at an ambiguous base; in this case, i<len always stands
		}
	}
	ik.info = ((uint64_t)x<<32) | ((uint64_t)i);		// begin and end positions on seq

	// push the SMEM ik to mem if it is long enough
	if (!(i-x>=min_seed_len)){
		ik.info = 0;
	}
	mem_a[x] = ik;
}
// extend furthest to the left from a position and save that one seed
__device__ void bwt_smem_left(const bwt_t *bwt, int len, const uint8_t *q, int x, int min_intv, uint64_t max_intv, int min_seed_len, bwtintv_v *mem)
{
	bwtintv_t ik, ok[4];
	if (q[x] > 3) return;			// dont do N base
	if (min_intv < 1) min_intv = 1; // the interval size should be at least 1
	bwt_set_intv(bwt, q[x], ik); 	// the initial interval of a single base
	ik.info = x + 1;				// right position on seq
	int i;
	for (i=x-1; i>=0; --i) { // backward search
		if (ik.x[2] < max_intv) { // an interval small enough
			break;
		} else if (q[i] < 4) { // an A/C/G/T base
			int c = q[i]; 
			bwt_extend_gpu(bwt, &ik, ok, 1);
			if (ok[c].x[2] < min_intv) break; // the interval size is too small to be extended further
			ik = ok[c];		// keep going
		} else { // an ambiguous base
			break; // always terminate extension at an ambiguous base; in this case, i<len always stands
		}
	}
	ik.info |= (uint64_t)i<<32;		// left position on seq

	// push the SMEM ik to mem if it is long enough
	if ((uint32_t)ik.info - (ik.info>>32) >= min_seed_len){
		int n = atomicAdd(&(mem->n), 1);
		mem->a[n] = ik;
	}
}

// NOTE: $max_intv is not currently used in BWA-MEM
__device__ int bwt_smem1a_gpu(const bwt_t *bwt, int len, const uint8_t *q, int x, int min_intv, uint64_t max_intv, bwtintv_v *mem, bwtintv_v *tmpvec[2], void* d_buffer_ptr)
{
	int i, j, c, ret;
	bwtintv_t ik, ok[4];
	bwtintv_v *swap;

	mem->n = 0;
	if (q[x] > 3) return x + 1;
	if (min_intv < 1) min_intv = 1; // the interval size should be at least 1
	// prev = tmpvec[0]; // use the temporary vector if provided
	// curr = tmpvec[1];
	bwt_set_intv(bwt, q[x], ik); // the initial interval of a single base
	ik.info = x + 1;

	for (i = x + 1, tmpvec[1]->n = 0; i < len; ++i) { // forward search
		if (ik.x[2] < max_intv) { // an interval small enough
			// push ik to curr. kv_push(bwtintv_t, *curr, ik, d_buffer_ptr);
			if  (tmpvec[1]->n == tmpvec[1]->m) {
				tmpvec[1]->m = tmpvec[1]->m? tmpvec[1]->m<<1 : 2;
				tmpvec[1]->a = (bwtintv_t*)CUDAKernelRealloc(d_buffer_ptr, tmpvec[1]->a, sizeof(bwtintv_t) * tmpvec[1]->m, 8);
			}
			tmpvec[1]->a[tmpvec[1]->n++] = ik;
			break;
		} else if (q[i] < 4) { // an A/C/G/T base
			c = 3 - q[i]; // complement of q[i]
			bwt_extend_gpu(bwt, &ik, ok, 0);
			if (ok[c].x[2] != ik.x[2]) { // change of the interval size
				// push ik to curr. kv_push(bwtintv_t, v=*curr, x=ik, d_buffer_ptr);
				if (tmpvec[1]->n == tmpvec[1]->m){
					tmpvec[1]->m = tmpvec[1]->m? tmpvec[1]->m<<1 : 2;
					tmpvec[1]->a = (bwtintv_t*)CUDAKernelRealloc(d_buffer_ptr, tmpvec[1]->a, sizeof(bwtintv_t) * tmpvec[1]->m, 8);
				}
				tmpvec[1]->a[tmpvec[1]->n++] = ik;
				if (ok[c].x[2] < min_intv) break; // the interval size is too small to be extended further
			}
			ik = ok[c]; ik.info = i + 1;
		} else { // an ambiguous base
			// kv_push(bwtintv_t, v=*curr, x=ik, d_buffer_ptr);
			if (tmpvec[1]->n == tmpvec[1]->m) {
				tmpvec[1]->m = tmpvec[1]->m? tmpvec[1]->m<<1 : 2;
				tmpvec[1]->a = (bwtintv_t*)CUDAKernelRealloc(d_buffer_ptr, tmpvec[1]->a, sizeof(bwtintv_t) * tmpvec[1]->m, 8);
			}
			tmpvec[1]->a[tmpvec[1]->n++] = ik;
			break; // always terminate extension at an ambiguous base; in this case, i<len always stands
		}
	}
	if (i == len) {
		// kv_push(bwtintv_t, *curr, ik, d_buffer_ptr); // push the last interval if we reach the end	
		if (tmpvec[1]->n == tmpvec[1]->m) {
			tmpvec[1]->m = tmpvec[1]->m? tmpvec[1]->m<<1 : 2;
			tmpvec[1]->a = (bwtintv_t*)CUDAKernelRealloc(d_buffer_ptr, tmpvec[1]->a, sizeof(bwtintv_t) * tmpvec[1]->m, 8);
		}
		tmpvec[1]->a[tmpvec[1]->n++] = ik;
	}
	bwt_reverse_intvs(tmpvec[1]); // s.t. smaller intervals (i.e. longer matches) visited first
	ret = tmpvec[1]->a[0].info; // this will be the returned value
	swap = tmpvec[1]; tmpvec[1] = tmpvec[0]; tmpvec[0] = swap;

	for (i = x - 1; i >= -1; --i) { // backward search for MEMs
		c = i < 0? -1 : q[i] < 4? q[i] : -1; // c==-1 if i<0 or q[i] is an ambiguous base
		for (j = 0, tmpvec[1]->n = 0; j < tmpvec[0]->n; ++j) {
			bwtintv_t *p = &tmpvec[0]->a[j];
			if (c >= 0 && ik.x[2] >= max_intv) bwt_extend_gpu(bwt, p, ok, 1);
			if (c < 0 || ik.x[2] < max_intv || ok[c].x[2] < min_intv) { // keep the hit if reaching the beginning or an ambiguous base or the intv is small enough
				if (tmpvec[1]->n == 0) { // test curr->n>0 to make sure there are no longer matches
					if (mem->n == 0 || i + 1 < mem->a[mem->n-1].info>>32) { // skip contained matches
						ik = *p; ik.info |= (uint64_t)(i + 1)<<32;
						// kv_push(bwtintv_t, *mem, ik, d_buffer_ptr);
						if (mem->n == mem->m) {
							mem->m = mem->m? mem->m<<1 : 2;
							mem->a = (bwtintv_t*)CUDAKernelRealloc(d_buffer_ptr, mem->a, sizeof(bwtintv_t) * mem->m, 8);
						}
						mem->a[mem->n++] = ik;
					}
				} // otherwise the match is contained in another longer match
			} else if (tmpvec[1]->n == 0 || ok[c].x[2] != tmpvec[1]->a[tmpvec[1]->n-1].x[2]) {
				ok[c].info = p->info;
				// kv_push(bwtintv_t, *curr, ok[c], d_buffer_ptr);
				if (tmpvec[1]->n == tmpvec[1]->m) {
					tmpvec[1]->m = tmpvec[1]->m? tmpvec[1]->m<<1 : 2;
					tmpvec[1]->a = (bwtintv_t*)CUDAKernelRealloc(d_buffer_ptr, tmpvec[1]->a, sizeof(bwtintv_t) * tmpvec[1]->m, 8);
				}
				tmpvec[1]->a[tmpvec[1]->n++] = ok[c];
			}
		}
		if (tmpvec[1]->n == 0) break;
		swap = tmpvec[1]; tmpvec[1] = tmpvec[0]; tmpvec[0] = swap;
	}
	bwt_reverse_intvs(mem); // s.t. sorted by the start coordinate

	// if (tmpvec == 0 || tmpvec[0] == 0) free(a[0].a);
	// if (tmpvec == 0 || tmpvec[1] == 0) free(a[1].a);
	return ret;
}

__device__ int bwt_seed_strategy1_gpu(const bwt_t *bwt, int len, const uint8_t *q, int x, int min_len, int max_intv, bwtintv_t *mem)
{
	int i, c;
	bwtintv_t ik, ok[4];

	memset(mem, 0, sizeof(bwtintv_t));
	if (q[x] > 3) return x + 1;
	bwt_set_intv(bwt, q[x], ik); // the initial interval of a single base
	for (i = x + 1; i < len; ++i) { // forward search
		if (q[i] < 4) { // an A/C/G/T base
			c = 3 - q[i]; // complement of q[i]
			bwt_extend_gpu(bwt, &ik, ok, 0);
			if (ok[c].x[2] < max_intv && i - x >= min_len) {
				*mem = ok[c];
				mem->info = (uint64_t)x<<32 | (i + 1);
				return i + 1;
			}
			ik = ok[c];
		} else return i + 1;
	}
	return len;
}

#define bwt_bwt(b, k) ((b)->bwt[((k)>>7<<4) + sizeof(bwtint_t) + (((k)&0x7f)>>4)])
#define bwt_B0(b, k) (bwt_bwt(b, k)>>((~(k)&0xf)<<1)&3)

__device__ static inline bwtint_t bwt_invPsi(const bwt_t *bwt, bwtint_t k) // compute inverse CSA
{
	bwtint_t x = k - (k > bwt->primary);
	//x = bwt_B0(bwt, x);
    int64_t x_ = bwt_bwt(bwt, x);
    uint8_t bwt_base_x = (x_ >> (((~x) & 0xf) << 1)) & 3;
    /**/
	x = bwt->L2[bwt_base_x] + bwt_occ_gpu(bwt, k, bwt_base_x);
	return k == bwt->primary? 0 : x;
}

__device__ bwtint_t bwt_sa_gpu(const bwt_t *bwt, bwtint_t k)
{
	bwtint_t sa = 0, mask = bwt->sa_intv - 1;
	while (k & mask) {
		++sa;
		k = bwt_invPsi(bwt, k);
	}
	/* without setting bwt->sa[0] = -1, the following line should be
	   changed to (sa + bwt->sa[k/bwt->sa_intv]) % (bwt->seq_len + 1) */
	return sa + bwt->sa[k/bwt->sa_intv];
}
