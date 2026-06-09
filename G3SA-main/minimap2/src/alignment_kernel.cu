// #include "agatha_kernel.h"
# include "alignment_kernel.h"
// #include "common.h"
// #include "minimap.h"
// #include "mmpriv.h"
// #include "kernel_wrapper.h"

__device__
unsigned char seq_nt4_table_d[256] = {
	0, 1, 2, 3,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 0, 4, 1,  4, 4, 4, 2,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  3, 3, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 0, 4, 1,  4, 4, 4, 2,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  3, 3, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4
};

// need to figure out what this does
__device__
int mm_get_hplen_back(const mm_idx_t *mi, uint32_t rid, uint32_t x)
{
	int64_t i, off0 = mi->seq[rid].offset, off = off0 + x;
	int c = mm_seq4_get(mi->S, off);
	for (i = off - 1; i >= off0; --i)
		if (mm_seq4_get(mi->S, i) != c) break;
	return (int)(off - i);
}

__device__
void mm_adjust_minier(const mm_idx_t *mi, uint8_t *const qseq0, uint64_t ax, uint64_t ay, int32_t *r, int32_t *q)
{
	// ignore HPC setting for now.

    // if (mi->flag & MM_I_HPC) {
	// 	const uint8_t *qseq = qseq0[a->x>>63];
	// 	int i, c;
	// 	*q = (int32_t)a->y;
	// 	for (i = *q - 1, c = qseq[*q]; i > 0; --i)
	// 		if (qseq[i] != c) break;
	// 	*q = i + 1;
	// 	c = mm_get_hplen_back(mi, a->x<<1>>33, (int32_t)a->x);
	// 	*r = (int32_t)a->x + 1 - c;
	// } else {
		*r = (int32_t)ax - (mi->k>>1);
		*q = (int32_t)ay - (mi->k>>1);
	// }
}

#define GAP_N 20 // idk how much register space we need TODO

__device__
void collect_long_gaps(int as1, int cnt1, uint64_t *ax, uint64_t* ay, int min_gap, int *n_, int* K)
{
	int i, n;
	*n_ = 0;
	for (i = 1, n = 0; i < cnt1; ++i) { // count the number of gaps longer than min_gap
		int gap = ((int32_t)ay[as1 + i] - ay[as1 + i - 1]) - ((int32_t)ax[as1 + i] - ax[as1 + i - 1]);
		if (gap < -min_gap || gap > min_gap) ++n;
	}
	if (n <= 1) return;
		// K = (int*)kmalloc(km, n * sizeof(int));
	for (i = 1, n = 0; i < cnt1; ++i) { // store the positions of long gaps
		int gap = ((int32_t)ay[as1 + i] - ay[as1 + i - 1]) - ((int32_t)ax[as1 + i] - ax[as1 + i - 1]);
		if (gap < -min_gap || gap > min_gap)
			K[n++] = i;
	}
	*n_ = n;
}

__device__
static void mm_filter_bad_seeds(int as1, int cnt1, uint64_t* ax, uint64_t* ay, int min_gap, int diff_thres, int max_ext_len, int max_ext_cnt)
{
	int max_st, max_en, n, i, k, max;
	int K[GAP_N];
	collect_long_gaps(as1, cnt1, ax, ay, min_gap, &n, K);
	if (n == 0) return; 
	max = 0, max_st = max_en = -1;
	for (k = 0;; ++k) { // traverse long gaps
		int gap, l, n_ins = 0, n_del = 0, qs, rs, max_diff = 0, max_diff_l = -1;
		if (k == n || k >= max_en) {
			if (max_en > 0)
				for (i = K[max_st]; i < K[max_en]; ++i)
					ay[as1 + i] |= MM_SEED_IGNORE;
			max = 0, max_st = max_en = -1;
			if (k == n) break;
		}
		i = K[k];
		gap = ((int32_t)ay[as1 + i] - (int32_t)ay[as1 + i - 1]) - (int32_t)(ax[as1 + i] - ax[as1 + i - 1]);
		if (gap > 0) n_ins += gap;
		else n_del += -gap;
		qs = (int32_t)ay[as1 + i - 1];
		rs = (int32_t)ax[as1 + i - 1];
		for (l = k + 1; l < n && l <= k + max_ext_cnt; ++l) {
			int j = K[l], diff;
			if ((int32_t)ay[as1 + j] - qs > max_ext_len || (int32_t)ax[as1 + j] - rs > max_ext_len) break;
			gap = ((int32_t)ay[as1 + j] - (int32_t)ay[as1 + j - 1]) - (int32_t)(ax[as1 + j] - ax[as1 + j - 1]);
			if (gap > 0) n_ins += gap;
			else n_del += -gap;
			diff = n_ins + n_del - abs(n_ins - n_del);
			if (max_diff < diff)
				max_diff = diff, max_diff_l = l;
		}
		if (max_diff > diff_thres && max_diff > max)
			max = max_diff, max_st = k, max_en = max_diff_l;
	}
}

__device__
static void mm_filter_bad_seeds_alt(int as1, int cnt1, uint64_t* ax, uint64_t* ay, int min_gap, int max_ext)
{
	int n, k;
	int K[GAP_N];
	collect_long_gaps(as1, cnt1, ax, ay, min_gap, &n, K);
	if (n == 0) return; 
	for (k = 0; k < n;) {
		int i = K[k], l;
		int gap1 = ((int32_t)ay[as1 + i] - (int32_t)ay[as1 + i - 1]) - ((int32_t)ax[as1 + i] - (int32_t)ax[as1 + i - 1]);
		int re1 = (int32_t)ax[as1 + i];
		int qe1 = (int32_t)ay[as1 + i];
		gap1 = gap1 > 0? gap1 : -gap1;
		for (l = k + 1; l < n; ++l) {
			int j = K[l], gap2, q_span_pre, rs2, qs2, m;
			if ((int32_t)ay[as1 + j] - qe1 > max_ext || (int32_t)ax[as1 + j] - re1 > max_ext) break;
			gap2 = ((int32_t)ay[as1 + j] - (int32_t)ay[as1 + j - 1]) - (int32_t)(ax[as1 + j] - ax[as1 + j - 1]);
			q_span_pre = ay[as1 + j - 1] >> 32 & 0xff;
			rs2 = (int32_t)ax[as1 + j - 1] + q_span_pre;
			qs2 = (int32_t)ay[as1 + j - 1] + q_span_pre;
			m = rs2 - re1 < qs2 - qe1? rs2 - re1 : qs2 - qe1;
			gap2 = gap2 > 0? gap2 : -gap2;
			if (m > gap1 + gap2) break;
			re1 = (int32_t)ax[as1 + j];
			qe1 = (int32_t)ay[as1 + j];
			gap1 = gap2;
		}
		if (l > k + 1) {
			int j, end = K[l - 1];
			for (j = K[k]; j < end; ++j)
				ay[as1 + j] |= MM_SEED_IGNORE;
			ay[as1 + end] |= MM_SEED_LONG_JOIN;
		}
		k = l;
	}
}

__device__
static void mm_fix_bad_ends(const mm_reg1_t *r, uint64_t* ax, uint64_t* ay, int bw, int min_match, int32_t *as, int32_t *cnt)
{
	int32_t i, l, m;
	*as = r->as, *cnt = r->cnt;
	if (r->cnt < 3) return;
	m = l = ay[r->as] >> 32 & 0xff;
	for (i = r->as + 1; i < r->as + r->cnt - 1; ++i) {
		int32_t lq, lr, min, max;
		int32_t q_span = ay[i] >> 32 & 0xff;
		if (ay[i] & MM_SEED_LONG_JOIN) break;
		lr = (int32_t)ax[i] - (int32_t)ax[i-1];
		lq = (int32_t)ay[i] - (int32_t)ay[i-1];
		min = lr < lq? lr : lq;
		max = lr > lq? lr : lq;
		if (max - min > l >> 1) *as = i;
		l += min;
		m += min < q_span? min : q_span;
		if (l >= bw << 1 || (m >= min_match && m >= bw) || m >= r->mlen >> 1) break;
	}
	*cnt = r->as + r->cnt - *as;
	m = l = ay[r->as + r->cnt - 1] >> 32 & 0xff;
	for (i = r->as + r->cnt - 2; i > *as; --i) {
		int32_t lq, lr, min, max;
		int32_t q_span = ay[i+1] >> 32 & 0xff;
		if (ay[i+1] & MM_SEED_LONG_JOIN) break;
		lr = (int32_t)ax[i+1] - (int32_t)ax[i];
		lq = (int32_t)ay[i+1] - (int32_t)ay[i];
		min = lr < lq? lr : lq;
		max = lr > lq? lr : lq;
		if (max - min > l >> 1) *cnt = i + 1 - *as;
		l += min;
		m += min < q_span? min : q_span;
		if (l >= bw << 1 || (m >= min_match && m >= bw) || m >= r->mlen >> 1) break;
	}
}

__device__
void device_idx_getseq(const mm_idx_t *mi, uint32_t rid, uint32_t st, uint32_t en, uint8_t *seq)
{
	uint64_t i, st1, en1;
	if (rid >= mi->n_seq || st >= mi->seq[rid].len) return;
	if (en > mi->seq[rid].len) en = mi->seq[rid].len;
	st1 = mi->seq[rid].offset + st;
	en1 = mi->seq[rid].offset + en;
	for (i = st1; i < en1; ++i)
		seq[i - st1] = mm_seq4_get(mi->S, i); 
	return;
}

__device__
void device_idx_getseq_rev(const mm_idx_t *mi, uint32_t rid, uint32_t st, uint32_t en, uint8_t *seq)
{
	uint64_t i, st1, en1;
	const mm_idx_seq_t *s;
	if (rid >= mi->n_seq || st >= mi->seq[rid].len) return;
	s = &mi->seq[rid];
	if (en > s->len) en = s->len;
	st1 = s->offset + (s->len - en);
	en1 = s->offset + (s->len - st);
	for (i = st1; i < en1; ++i) {
		uint8_t c = mm_seq4_get(mi->S, i);
		seq[en1 - i - 1] = c < 4? 3 - c : c;
	}
	return;
}

__device__
void device_idx_getseq2(const mm_idx_t *mi, int is_rev, uint32_t rid, uint32_t st, uint32_t en, uint8_t *seq)
{
	if (is_rev) device_idx_getseq_rev(mi, rid, st, en, seq);
	else device_idx_getseq(mi, rid, st, en, seq);
}


/* function for batch alignment processing */
__global__ 
void fill_align_info(const mm_idx_t *mi, uint64_t* g_ax, uint64_t* g_ay, mm_chain_t* chain, // reference index, anchors, chain metadata
					uint8_t* const g_qseq0, uint8_t* align_qseq, uint8_t* align_rseq, // original qseq, target qseq, target rseq
					uint32_t* q_lens, uint32_t* r_lens, uint32_t* q_ofs, uint32_t* r_ofs, // alignment information for AGATHA kernel
					int n_chain, int max_qlen, int max_rlen, bool* g_dropped, bool* g_zdropped, int flag, int* n_align) // extra informations
{
	
	int id = threadIdx.x + blockIdx.x * blockDim.x;
	
	for(int job_idx = id; job_idx < n_chain; job_idx += gridDim.x*blockDim.x){

		mm_chain_t* c = &chain[job_idx];
		mm_reg1_t* r = &(c->r);
    
		int a_ofs = c->a_ofs;
		int qlen = c->qlen; 
    	uint64_t* ax = &g_ax[a_ofs];
    	uint64_t* ay = &g_ay[a_ofs];
		uint8_t* qseq0 = &g_qseq0[c->r_ofs];
		int as1, cnt1;

		mm_mapopt_t opt1; // TODO: add global opt pointer 
    	mm_mapopt_t* opt = &opt1;
    	opt->min_ksw_len = 200;
    	opt->bw = 0;
    	opt->bw_long = 20000;
		opt->min_cnt = 3;
    	opt->max_gap = 5000;
    	opt->a = 2;
    	opt->q = 4;
    	opt->e = 2;
		opt->min_chain_score = 40;

		bool* dropped = &g_dropped[job_idx];

		int n_a = c->n_a; // number of surviving anchors
	
    	int32_t rid = ax[c->r.as]<<1>>33, rev = ax[c->r.as]>>63;
    	int32_t l, bw, bw_long, extra_flag = 0, rs0, re0, qs0, qe0;
		int32_t rs, re, qs, qe, rs1, re1, qs1, qe1;

		bw = (int)(opt->bw * 1.5 + 1.);
		bw_long = (int)(opt->bw_long * 1.5 + 1.);
		if (bw_long < bw) bw_long = bw; // TODO: move this thing to outside loop
	
		if(*dropped){ // this chain was filtered out in the previous stage
			q_ofs[job_idx] = max_qlen * job_idx;
			q_lens[job_idx] = 0;
			r_ofs[job_idx] = max_rlen * job_idx;
			r_lens[job_idx] = 0;
			q_ofs[job_idx + n_chain] = max_qlen * (job_idx + n_chain);
			q_lens[job_idx + n_chain] = 0;
			r_ofs[job_idx + n_chain] = max_rlen * (job_idx + n_chain);
			r_lens[job_idx + n_chain] = 0;
			continue;
		}

		as1 = r->as, cnt1 = r->cnt; // TODO: let's get this fixed!
		mm_fix_bad_ends(r, ax, ay, opt->bw, opt->min_chain_score * 2, &as1, &cnt1);


		/* alignment initialization */
		mm_filter_bad_seeds(as1, cnt1, ax, ay, 10, 40, opt->max_gap>>1, 10);
		mm_filter_bad_seeds_alt(as1, cnt1, ax, ay, 30, opt->max_gap>>1);

    	// might not need qseq0 if only doing the basic alignment
		mm_adjust_minier(mi, qseq0, ax[as1], ay[as1], &rs, &qs);
		mm_adjust_minier(mi, qseq0, ax[as1 + cnt1 - 1], ay[as1 + cnt1 - 1], &re, &qe);

		rs0 = (int32_t)ax[r->as] + 1 - (int32_t)(ay[r->as]>>32&0xff);
		qs0 = (int32_t)ay[r->as] + 1 - (int32_t)(ay[r->as]>>32&0xff);
		if (rs0 < 0) rs0 = 0; // this may happen when HPC is in use
		assert(qs0 >= 0); // this should never happen, or it is logic error
		rs1 = qs1 = 0;

		for (int i = r->as - 1, l = 0; i >= 0 && ax[i]>>32 == ax[r->as]>>32; --i) { // inspect nearby seeds
			int32_t x = (int32_t)ax[i] + 1 - (int32_t)(ay[i]>>32&0xff);
			int32_t y = (int32_t)ay[i] + 1 - (int32_t)(ay[i]>>32&0xff);
			if (x < rs0 && y < qs0) {
				if (++l > opt->min_cnt) {
					l = rs0 - x > qs0 - y? rs0 - x : qs0 - y;
					rs1 = rs0 - l, qs1 = qs0 - l;
					if (rs1 < 0) rs1 = 0; // not strictly necessary; better have this guard for explicit
					break;
				}
			}
		}
		if (qs > 0 && rs > 0) {
			l = qs < opt->max_gap? qs : opt->max_gap;
			qs1 = qs1 > qs - l? qs1 : qs - l;
			qs0 = qs0 < qs1? qs0 : qs1; // at least include qs0
			l += l * opt->a > opt->q? (l * opt->a - opt->q) / opt->e : 0;
			l = l < opt->max_gap? l : opt->max_gap;
			l = l < rs? l : rs;
			rs1 = rs1 > rs - l? rs1 : rs - l;
			rs0 = rs0 < rs1? rs0 : rs1;
			rs0 = rs0 < rs? rs0 : rs;
		} else rs0 = rs, qs0 = qs;
	
    	// compute re0 and qe0
		re0 = (int32_t)ax[r->as + r->cnt - 1] + 1;
		qe0 = (int32_t)ay[r->as + r->cnt - 1] + 1;
		re1 = mi->seq[rid].len, qe1 = qlen;
		for (int i = r->as + r->cnt, l = 0; i < n_a && ax[i]>>32 == ax[r->as]>>32; ++i) { // inspect nearby seeds
			int32_t x = (int32_t)ax[i] + 1;
			int32_t y = (int32_t)ay[i] + 1;
			if (x > re0 && y > qe0) {
				if (++l > opt->min_cnt) {
					l = x - re0 > y - qe0? x - re0 : y - qe0;
					re1 = re0 + l, qe1 = qe0 + l;
					break;
				}
			}
		}

		if (qe < qlen && re < (int32_t)mi->seq[rid].len) {
			l = qlen - qe < opt->max_gap? qlen - qe : opt->max_gap;
			qe1 = qe1 < qe + l? qe1 : qe + l;
			qe0 = qe0 > qe1? qe0 : qe1; // at least include qe0
			l += l * opt->a > opt->q? (l * opt->a - opt->q) / opt->e : 0;
			l = l < opt->max_gap? l : opt->max_gap;
			l = l < (int32_t)mi->seq[rid].len - re? l : mi->seq[rid].len - re;
			re1 = re1 < re + l? re1 : re + l;
			re0 = re0 > re1? re0 : re1;
		} else re0 = re, qe0 = qe;

		if (ay[r->as] & MM_SEED_SELF) {
			int max_ext = r->qs > r->rs? r->qs - r->rs : r->rs - r->qs;
			if (r->rs - rs0 > max_ext) rs0 = r->rs - max_ext;
			if (r->qs - qs0 > max_ext) qs0 = r->qs - max_ext;
			max_ext = r->qe > r->re? r->qe - r->re : r->re - r->qe;
			if (re0 - r->re > max_ext) re0 = r->re + max_ext;
			if (qe0 - r->qe > max_ext) qe0 = r->qe + max_ext;
		}


		assert(re0 > rs0);

		// local array to store gap information
		uint32_t query_ofs_buffer[MAX_GAP_NUM];
		uint32_t query_len_buffer[MAX_GAP_NUM];
		uint32_t ref_ofs_buffer[MAX_GAP_NUM];
		uint32_t ref_len_buffer[MAX_GAP_NUM];

		uint32_t* query_offset = &q_ofs[job_idx];
		uint32_t* ref_offset = &r_ofs[job_idx];
		uint32_t* query_len = &q_lens[job_idx];
		uint32_t* ref_len = &r_lens[job_idx];

		/* left extension */
		// set alignment info
		*query_offset = max_qlen * job_idx;
		*query_len = qs - qs0;
		*ref_offset = max_rlen * job_idx;
		*ref_len = rs - rs0;

		// load sequence 
		if(!rev)
			for(int i=0; i<qs-qs0;i++) {
				align_qseq[*query_offset + i] = seq_nt4_table_d[qseq0[qs - 1 - i]];
			}
		else {
			int qseq_end1 = qlen - qs0, qseq_st1 = qlen - qs;
			for(int i = 0; i < qseq_end1 - qseq_st1; i++) {
				uint8_t q = seq_nt4_table_d[(uint8_t)qseq0[qseq_st1 + i]];
				align_qseq[*query_offset + i] = q < 4? 3 - q : 4; 
			}
		}
		
		device_idx_getseq2(mi, 0, rid, rs0, rs, &align_rseq[*ref_offset]); // TODO: FIXME: ref sequence is already packed. no need to unpack & repack		
		uint8_t t;
		int len = rs - rs0;
		for(int i = 0; i < len>>1; i++) {
			t = align_rseq[*ref_offset + i];
			align_rseq[*ref_offset + i] = align_rseq[*ref_offset + len - 1 - i], 
			align_rseq[*ref_offset + len - 1 - i] = t;
		}

		/* gap filling */
		// reset alignment info pointer & var
		int n_gap = 0, gap_ofs;
		// gap filling loop
		int is_sr = 0; 
		for (int i = is_sr? cnt1 - 1 : 1; i < cnt1; ++i) { // gap filling
			if ((ay[as1+i] & (MM_SEED_IGNORE|MM_SEED_TANDEM)) && i != cnt1 - 1) continue; 
			
			int32_t re, qe;
			mm_adjust_minier(mi, qseq0, ax[as1 + i], ay[as1 + i], &re, &qe);
			re1 = re, qe1 = qe;

			if (i == cnt1 - 1 || (ay[as1+i]&MM_SEED_LONG_JOIN) || (qe - qs >= opt->min_ksw_len && re - rs >= opt->min_ksw_len)) {
				// if fragment length shorter than minimal length : skip to next anchor 
				// int j, bw1 = bw_long, zdrop_code;
				// if (ay[as1+i] & MM_SEED_LONG_JOIN)
					// bw1 = qe - p->qs > re - p->rs? qe - p->qs : re - p->rs;
				// perform normal gapped alignment
				query_ofs_buffer[n_gap] = qs;
				query_len_buffer[n_gap] = qe - qs;
				ref_ofs_buffer[n_gap] = rs;
				ref_len_buffer[n_gap] = re - rs;
				n_gap++;
				rs = re, qs = qe;
				assert(n_gap < MAX_GAP_NUM);
			}
		}																																			

		// write to global memory & fetch sequence
		gap_ofs = atomicAdd(n_align, (int)n_gap); 
		
		query_offset = &q_ofs[n_chain * 2 + gap_ofs], query_len = &q_lens[n_chain * 2 + gap_ofs];
		ref_offset = &r_ofs[n_chain * 2 + gap_ofs], ref_len = &r_lens[n_chain * 2 + gap_ofs];
		c->gap_ofs = gap_ofs;
		c->n_gap = n_gap;
		bool* zdropped = &g_zdropped[gap_ofs];
		for(int j = 0; j < n_gap; j++){
			*query_offset = max_qlen * (n_chain * 2 + gap_ofs + j);
			*query_len = query_len_buffer[j];
			*ref_offset = max_rlen * (n_chain * 2 + gap_ofs + j);
			*ref_len = ref_len_buffer[j];
			*zdropped = false; // initializing zdrop vector 

			int qs_ = query_ofs_buffer[j], qlen_ = query_len_buffer[j];
			int rs_ = ref_ofs_buffer[j], rlen_ = ref_len_buffer[j];
			if(!rev) for(int i=0; i<qlen_;i++) align_qseq[*query_offset + i] = seq_nt4_table_d[qseq0[qs_ + i]];
			else {
				int qseq_end1 = qlen - qs_, qseq_st1 = qlen - (qlen_ + qs_);
				for(int i = 0; i < qseq_end1 - qseq_st1; i++) {
					uint8_t q = seq_nt4_table_d[(uint8_t)qseq0[qseq_st1 + i]];
					align_qseq[*query_offset + qlen_ - 1 - i] = q < 4? 3 - q : 4; 
				}
			}
			device_idx_getseq2(mi, 0, rid, rs_, rs_+rlen_, &align_rseq[*ref_offset]); // FIXME: ref sequence is already packed. no need to unpack & repack	

			query_offset++, query_len++, ref_offset++, ref_len++;
		}

		// right extension
		query_offset = &q_ofs[n_chain+job_idx], query_len = &q_lens[n_chain+job_idx];
		ref_offset = &r_ofs[n_chain+job_idx], ref_len = &r_lens[n_chain+job_idx];

		if (!(*dropped) && qe < qe0 && re < re0) { // right extension FIXME: dropped chain management - dropped chain vs. zdrop must be separate
		
			*query_offset = max_qlen * (n_chain + job_idx);
			*query_len = qe0 - qe;
			*ref_offset = max_qlen * (n_chain + job_idx); 
			*ref_len = re0 - re;

			// fetch sequence
			if(!rev){
				for(int i=0; i<qe0-qe;i++) {
					align_qseq[*query_offset + i] = seq_nt4_table_d[qseq0[qe + i]];
				}
			}
			else {
				int qseq_end1 = qlen - qe, qseq_st1 = qlen - qe0;
				for(int i = 0; i < qseq_end1 - qseq_st1; i++) {
					uint8_t q = seq_nt4_table_d[(uint8_t)qseq0[qseq_st1 + i]];
					align_qseq[*query_offset + qe0 - qe - 1 - i] = q < 4? 3 - q : 4; 
				}
			}
			device_idx_getseq2(mi, 0, rid, re, re0, &align_rseq[*ref_offset]); // FIXME: ref sequence is already packed. no need to unpack & repack		
		}
		else{
			*query_offset = 0, *query_len = 0, *ref_offset = 0, *ref_len = 0;
			continue;
		}
	}

}

__device__
static void mm_append_cigar(int* n_cigar, uint32_t* cigar, int n_cigar_new, uint32_t *cigar_new) 
{
	if (*n_cigar == 0) {
		memcpy(cigar, cigar_new, n_cigar_new * 4);
		*n_cigar = n_cigar_new;
		return;
	}
	if (*n_cigar > 0 && (cigar[*n_cigar-1]&0xf) == (cigar_new[0]&0xf)) { // same CIGAR op at the boundary
		cigar[*n_cigar-1] += (cigar_new[0]>>4)<<4;
		if (n_cigar_new > 1) memcpy(cigar + *n_cigar, cigar_new + 1, (n_cigar_new - 1) * 4);
		*n_cigar += n_cigar_new - 1;
	} else {
		memcpy(cigar + *n_cigar, cigar_new, n_cigar_new * 4);
		*n_cigar += n_cigar_new;
	}
}

__global__
void collect_extending_results(mm_chain_t* g_c, int* dp_score, int* n_cigar, uint32_t* cigar, int* n_cigar_output, uint32_t* cigar_output, int n_chain, int max_frag_len, int max_seq_len, uint8_t* zdrop_code, uint8_t* zdropped, int* read_id){
	
	int id = threadIdx.x + blockIdx.x * blockDim.x;

	for(int job_idx = id; job_idx < n_chain; job_idx += gridDim.x * blockDim.x) {
		mm_chain_t* c = &g_c[job_idx];
		int read_idx = c->r_n; // retrieve original read id
		read_id[job_idx] = read_idx;

		
		int frag_idx = c->gap_ofs;
		if(c->dropped) { // this is filtered chain
			n_cigar_output[job_idx] = 0;
			read_id[job_idx] = -1;
			continue; 
		}

		for(; frag_idx < c->gap_ofs + c->n_gap; frag_idx++){

			if(zdrop_code[frag_idx]){ // this drop required re-alignment
				zdropped[read_idx] = 1;
				break;
			}
		}
		if(zdropped[read_idx] == 1) continue;
		
		frag_idx = job_idx;
		int score = 0;
		int cigar_len = 0;

		// left extension result
		if(n_cigar[frag_idx]>0){
			score += dp_score[frag_idx];
			mm_append_cigar(&cigar_len, &cigar_output[job_idx * max_seq_len], n_cigar[frag_idx], &cigar[frag_idx * max_frag_len]);
		}
		
		frag_idx = 2 * n_chain + c->gap_ofs;
		
		// gap filling result 
		for(int j = 0; j < c->n_gap; j++){
			if(n_cigar[frag_idx + j]>0){
				score += dp_score[frag_idx + j];
				mm_append_cigar(&cigar_len, &cigar_output[job_idx * max_seq_len], n_cigar[frag_idx + j], &cigar[(frag_idx + j) * max_frag_len]);
			}
			assert(cigar_len < max_seq_len);
		}
		

		// right extension result
		frag_idx = n_chain + job_idx;
		
		if(n_cigar[frag_idx]>0){
			score += dp_score[frag_idx];
			mm_append_cigar(&cigar_len, &cigar_output[job_idx * max_seq_len], n_cigar[frag_idx], &cigar[frag_idx * max_frag_len]);
		}

	
		assert(cigar_len < max_seq_len);
		n_cigar_output[job_idx] = cigar_len;
		c->r.score = score; // we need this for mapping quality calculation; but not now

		// TODO: fix cigar, update extra
	}
}

__device__
static inline void update_max_zdrop(int32_t score, int i, int j, int32_t *max, int *max_i, int *max_j, int e, int *max_zdrop, int pos[2][2])
{
	if (score < *max) {
		int li = i - *max_i;
		int lj = j - *max_j;
		int diff = li > lj? li - lj : lj - li;
		int z = *max - score - diff * e;
		if (z > *max_zdrop) {
			*max_zdrop = z;
			pos[0][0] = *max_i, pos[0][1] = i;
			pos[1][0] = *max_j, pos[1][1] = j;
		}
	} else *max = score, *max_i = i, *max_j = j;
}


__constant__ int32_t _cudaGapO = 4; /*gap open penalty*/
__constant__ int32_t _cudaGapOE = 6; /*sum of gap open and extension penalties*/
__constant__ int32_t _cudaGapExtend = 2; /*sum of gap extend*/
__constant__ int32_t _cudaZThreshold = 400; /*(AGAThA) zdrop threshold*/

__device__
static void ksw_gen_simple_mat(int m, int8_t *mat, int8_t a, int8_t b, int8_t sc_ambi)
{
	int i, j;
	a = a < 0? -a : a;
	b = b > 0? -b : b;
	sc_ambi = sc_ambi > 0? -sc_ambi : sc_ambi;
	for (i = 0; i < m - 1; ++i) {
		for (j = 0; j < m - 1; ++j)
			mat[i * m + j] = i == j? a : b;
		mat[i * m + m - 1] = sc_ambi;
	}
	for (j = 0; j < m; ++j)
		mat[(m - 1) * m + j] = sc_ambi;
}
__device__
static void ksw_gen_ts_mat(int m, int8_t *mat, int8_t a, int8_t b, int8_t transition, int8_t sc_ambi)
{
	assert(m==5);
	ksw_gen_simple_mat(m,mat,a,b,sc_ambi);
	transition = transition > 0? -transition : transition;
	mat[0*m+2]=transition;  // A->G
	mat[1*m+3]=transition;  // C->T
	mat[2*m+0]=transition;  // G->A
	mat[3*m+1]=transition;  // T->C
}


__global__
void mm_test_zdrop(uint8_t* zdrop_code, uint32_t* g_qlen, uint32_t* g_rlen, uint8_t* g_qseq, uint8_t* g_tseq, int* g_n_cigar, uint32_t *g_cigar, int n_task, int max_seq_len, int8_t* g_mat){
	
	int tid = threadIdx.x;
	int bid = blockIdx.x;
	int id = tid + blockDim.x * bid;

	int8_t mat[25];
    ksw_gen_ts_mat(5, mat, 2, 4, 4, 1);

	int test_id;

	for(int job_idx = id; job_idx < n_task; job_idx += blockDim.x * gridDim.x){

		// forced zdrop : temporary solution
		uint64_t max_sw_mat = 100000000;
		if((uint64_t)g_qlen[job_idx] * g_rlen[job_idx] > max_sw_mat){
			zdrop_code[job_idx] = 1;
			continue;
		}

		uint32_t k;
		int32_t score = 0, max = INT32_MIN, max_i = -1, max_j = -1, i = 0, j = 0, max_zdrop = 0;
		int pos[2][2] = {{-1, -1}, {-1, -1}}, q_len, t_len;

		int n_cigar = g_n_cigar[job_idx];
		uint32_t* cigar = &g_cigar[job_idx * max_seq_len];
		uint8_t* tseq = &g_tseq[job_idx * max_seq_len];
		uint8_t* qseq = &g_qseq[job_idx * max_seq_len];

		// find the score and the region where score drops most along diagonal
		for (k = 0, score = 0; k < n_cigar; ++k) {
			uint32_t l, op = cigar[k]&0xf, len = cigar[k]>>4;
			if (op == MM_CIGAR_MATCH) {
				for (l = 0; l < len; ++l) {
					score += mat[tseq[i + l] * 5 + qseq[j + l]];
					update_max_zdrop(score, i+l, j+l, &max, &max_i, &max_j, _cudaGapExtend, &max_zdrop, pos);
				}
				i += len, j += len;
			} else if (op == MM_CIGAR_INS || op == MM_CIGAR_DEL || op == MM_CIGAR_N_SKIP) {
				score -= _cudaGapO + _cudaGapExtend * len;
				if (op == MM_CIGAR_INS) j += len;
				else i += len;
				update_max_zdrop(score, i, j, &max, &max_i, &max_j, _cudaGapExtend, &max_zdrop, pos);
			}
		}
	
		if(max_zdrop > _cudaZThreshold) zdrop_code[job_idx] = 1;
		else zdrop_code[job_idx] = 0;
	}
}
