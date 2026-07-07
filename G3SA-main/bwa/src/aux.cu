#include "kbtree_CUDA.cuh"
#include "bwa.h"
#include "gmem_alloc.h"
#include "bwt_CUDA.cuh"
#include "bntseq.h"
#include "ksort_CUDA.h"
#include "ksw.h"
#include "kstring_CUDA.cuh"
#include <string.h>
#include "cuda_wrapper.h"
#include "macro.h"
#include "hashKMerIndex.h"
#include "seed.cuh"
#include <fstream>
#include <chrono>
using namespace std::chrono;
#include <iostream>
using namespace std;

#include <cub/cub.cuh>
#include <thrust/device_vector.h>
#include <thrust/sort.h>

#include "aux.cuh"

extern __device__ uint32_t *bwa_gen_cigar2_gpu(const int8_t mat[25], int o_del, int e_del, int o_ins, int e_ins, int w_, int64_t l_pac, const uint8_t *pac, int l_query, uint8_t *query, int64_t rb, int64_t re, int *score, int *n_cigar, int *NM, void* d_buffer_ptr);



struct compare_rbeg {
    __host__ __device__
        bool operator()(const mem_seed_t& a, const mem_seed_t& b) const {
            return a.rbeg < b.rbeg;
        }
};
struct compare_qbeg {
    __host__ __device__
        bool operator()(const mem_seed_t& a, const mem_seed_t& b) const {
            return a.qbeg < b.qbeg;
        }
};
struct compare_qend {
    __host__ __device__
        bool operator()(const mem_seed_t& a, const mem_seed_t& b) const {
            return a.qbeg + a.len < b.qbeg + b.len;
        }
};


/************************
 * Seeding and Chaining *
 ************************/
// return 1 if the seed is merged into the chain
__device__ static int test_and_merge(const mem_opt_t *opt, int64_t l_pac, mem_chain_t *c, const mem_seed_t *p, int seed_rid, void* CUDAKernel_buffer)
{
    int64_t qend, rend, x, y;
    const mem_seed_t *last = &c->seeds[c->n-1];
    qend = last->qbeg + last->len;
    rend = last->rbeg + last->len;

    if (seed_rid != c->rid) return 0; // different chr; request a new chain
    if (p->qbeg >= c->seeds[0].qbeg && p->qbeg + p->len <= qend && 
            p->rbeg >= c->seeds[0].rbeg && p->rbeg + p->len <= rend)
        return 1; // contained seed; do nothing

    if ((last->rbeg < l_pac || c->seeds[0].rbeg < l_pac) &&
            p->rbeg >= l_pac) return 0; // don't chain if on different strand

    x = p->qbeg - last->qbeg; // always non-negtive
    y = p->rbeg - last->rbeg;
    if (y >= 0 && x - y <= opt->w && y - x <= opt->w &&
            x - last->len < opt->max_chain_gap &&
            y - last->len < opt->max_chain_gap) { // grow the chain
        if (c->n == c->m) {
            c->m <<= 1;
            c->seeds = (mem_seed_t*)CUDAKernelRealloc(CUDAKernel_buffer, c->seeds, c->m * sizeof(mem_seed_t), 8);
        }
        c->seeds[c->n++] = *p;
        return 1;
    }
    return 0; // request to add a new chain
}

/* end collection of SA intervals  */

/********************
 * Filtering chains *
 ********************/


__device__ int mem_chain_weight(const mem_chain_t *c)
{
    int64_t end;
    int j, w = 0, tmp;
    for (j = 0, end = 0; j < c->n; ++j) {
        const mem_seed_t *s = &c->seeds[j];
        if (s->qbeg >= end) w += s->len;
        else if (s->qbeg + s->len > end) w += s->qbeg + s->len - end;
        end = end > s->qbeg + s->len? end : s->qbeg + s->len;
    }
    tmp = w; w = 0;
    for (j = 0, end = 0; j < c->n; ++j) {
        const mem_seed_t *s = &c->seeds[j];
        if (s->rbeg >= end) w += s->len;
        else if (s->rbeg + s->len > end) w += s->rbeg + s->len - end;
        end = end > s->rbeg + s->len? end : s->rbeg + s->len;
    }
    w = w < tmp? w : tmp;
    return w < 1<<30? w : (1<<30)-1;
}

/*********************************
 * Test if a seed is good enough *
 *********************************/


__device__ int mem_seed_sw(const mem_opt_t *opt, const bntseq_t *bns, const uint8_t *pac, int l_query, const uint8_t *query, const mem_seed_t *s, void* d_buffer_ptr)
{
    int qb, qe, rid;
    int64_t rb, re, mid, l_pac = bns->l_pac;
    uint8_t *rseq = 0;
    kswr_t x;

    if (s->len >= MEM_SHORT_LEN) return -1; // the seed is longer than the max-extend; no need to do SW
    qb = s->qbeg, qe = s->qbeg + s->len;
    rb = s->rbeg, re = s->rbeg + s->len;
    mid = (rb + re) >> 1;
    qb -= MEM_SHORT_EXT; qb = qb > 0? qb : 0;
    qe += MEM_SHORT_EXT; qe = qe < l_query? qe : l_query;
    rb -= MEM_SHORT_EXT; rb = rb > 0? rb : 0;
    re += MEM_SHORT_EXT; re = re < l_pac<<1? re : l_pac<<1;
    if (rb < l_pac && l_pac < re) {
        if (mid < l_pac) re = l_pac;
        else rb = l_pac;
    }
    if (qe - qb >= MEM_SHORT_LEN || re - rb >= MEM_SHORT_LEN) return -1; // the seed seems good enough; no need to do SW

    rseq = bns_fetch_seq_gpu(bns, pac, &rb, mid, &re, &rid, d_buffer_ptr);
    x = ksw_align2(qe - qb, (uint8_t*)query + qb, re - rb, rseq, 5, opt->mat, opt->o_del, opt->e_del, opt->o_ins, opt->e_ins, KSW_XSTART, 0, d_buffer_ptr);
    // free(rseq);
    // printf("unit test 4 x.score = %d\n", x.score);
    return x.score;
}

__device__ void mem_flt_chained_seeds(const mem_opt_t *opt, const bntseq_t *bns, const uint8_t *pac, int l_query, const uint8_t *query, int n_chn, mem_chain_t *a, void* d_buffer_ptr)
{
    double min_l = opt->min_chain_weight? MEM_HSP_COEF * opt->min_chain_weight : MEM_MINSC_COEF * log((float)l_query);
    int i, j, k, min_HSP_score = (int)(opt->a * min_l + .499);
    if (min_l > MEM_SEEDSW_COEF * l_query) return; // don't run the following for short reads
    for (i = 0; i < n_chn; ++i) {
        mem_chain_t *c = &a[i];
        for (j = k = 0; j < c->n; ++j) {
            mem_seed_t *s = &c->seeds[j];
            s->score = mem_seed_sw(opt, bns, pac, l_query, query, s, d_buffer_ptr);
            if (s->score < 0 || s->score >= min_HSP_score) {
                s->score = s->score < 0? s->len * opt->a : s->score;
                c->seeds[k++] = *s;
            }
        }
        c->n = k;
    }
}

/****************************************
 * Construct the alignment from a chain *
 ****************************************/

__device__ static inline int cal_max_gap(const mem_opt_t *opt, int qlen)
{
    int l_del = (int)((double)(qlen * opt->a - opt->o_del) / opt->e_del + 1.);
    int l_ins = (int)((double)(qlen * opt->a - opt->o_ins) / opt->e_ins + 1.);
    int l = l_del > l_ins? l_del : l_ins;
    l = l > 1? l : 1;
    return l < opt->w<<1? l : opt->w<<1;
}


__device__ void mem_chain2aln(const mem_opt_t *opt, const bntseq_t *bns, const uint8_t *pac, int l_query, const uint8_t *query, const mem_chain_t *c, mem_alnreg_v *av, void* d_buffer_ptr)
{
    int i, k, rid, max_off[2], aw[2]; // aw: actual bandwidth used in extension
    int64_t l_pac = bns->l_pac, rmax[2], tmp, max = 0;
    const mem_seed_t *s;
    uint8_t *rseq = 0;
    uint64_t *srt;

    if (c->n == 0) return;
    // get the max possible span
    rmax[0] = l_pac<<1; rmax[1] = 0;
    for (i = 0; i < c->n; ++i) {
        int64_t b, e;
        const mem_seed_t *t = &c->seeds[i];
        b = t->rbeg - (t->qbeg + cal_max_gap(opt, t->qbeg));
        e = t->rbeg + t->len + ((l_query - t->qbeg - t->len) + cal_max_gap(opt, l_query - t->qbeg - t->len));
        rmax[0] = rmax[0] < b? rmax[0] : b;
        rmax[1] = rmax[1] > e? rmax[1] : e;
        if (t->len > max) max = t->len;
    }
    rmax[0] = rmax[0] > 0? rmax[0] : 0;
    rmax[1] = rmax[1] < l_pac<<1? rmax[1] : l_pac<<1;
    if (rmax[0] < l_pac && l_pac < rmax[1]) { // crossing the forward-reverse boundary; then choose one side
        if (c->seeds[0].rbeg < l_pac) rmax[1] = l_pac; // this works because all seeds are guaranteed to be on the same strand
        else rmax[0] = l_pac;
    }
    // retrieve the reference sequence
    rseq = bns_fetch_seq_gpu(bns, pac, &rmax[0], c->seeds[0].rbeg, &rmax[1], &rid, d_buffer_ptr);

    srt = (uint64_t*)CUDAKernelMalloc(d_buffer_ptr, c->n * 8, 8);
    for (i = 0; i < c->n; ++i)
        srt[i] = (uint64_t)c->seeds[i].score<<32 | i;
    ks_introsort_64(c->n, srt, d_buffer_ptr);

    for (k = c->n - 1; k >= 0; --k) {
        mem_alnreg_t *a;
        s = &c->seeds[(uint32_t)srt[k]];

        for (i = 0; i < av->n; ++i) { // test whether extension has been made before
            mem_alnreg_t *p = &av->a[i];
            int64_t rd;
            int qd, w, max_gap;
            if (s->rbeg < p->rb || s->rbeg + s->len > p->re || s->qbeg < p->qb || s->qbeg + s->len > p->qe) continue; // not fully contained
            if (s->len - p->seedlen0 > .1 * l_query) continue; // this seed may give a better alignment
                                                               // qd: distance ahead of the seed on query; rd: on reference
            qd = s->qbeg - p->qb; rd = s->rbeg - p->rb;
            max_gap = cal_max_gap(opt, qd < rd? qd : rd); // the maximal gap allowed in regions ahead of the seed
            w = max_gap < p->w? max_gap : p->w; // bounded by the band width
            if (qd - rd < w && rd - qd < w) break; // the seed is "around" a previous hit
                                                   // similar to the previous four lines, but this time we look at the region behind
            qd = p->qe - (s->qbeg + s->len); rd = p->re - (s->rbeg + s->len);
            max_gap = cal_max_gap(opt, qd < rd? qd : rd);
            w = max_gap < p->w? max_gap : p->w;
            if (qd - rd < w && rd - qd < w) break;
        }
        if (i < av->n) { // the seed is (almost) contained in an existing alignment; further testing is needed to confirm it is not leading to a different aln
            for (i = k + 1; i < c->n; ++i) { // check overlapping seeds in the same chain
                const mem_seed_t *t;
                if (srt[i] == 0) continue;
                t = &c->seeds[(uint32_t)srt[i]];
                if (t->len < s->len * .95) continue; // only check overlapping if t is long enough; TODO: more efficient by early stopping
                if (s->qbeg <= t->qbeg && s->qbeg + s->len - t->qbeg >= s->len>>2 && t->qbeg - s->qbeg != t->rbeg - s->rbeg) break;
                if (t->qbeg <= s->qbeg && t->qbeg + t->len - s->qbeg >= s->len>>2 && s->qbeg - t->qbeg != s->rbeg - t->rbeg) break;
            }
            if (i == c->n) { // no overlapping seeds; then skip extension
                srt[k] = 0; // mark that seed extension has not been performed
                continue;
            }
        }

        // 	a = kv_pushp(type=mem_alnreg_t, v=*av);
        a = (((av->n == av->m)?
                    (av->m = (av->m? av->m<<1 : 2),
                     av->a = (mem_alnreg_t*)CUDAKernelRealloc(d_buffer_ptr, av->a, sizeof(mem_alnreg_t) * av->m, 8), 0)
                    : 0), &(av->a[av->n++]));
        memset(a, 0, sizeof(mem_alnreg_t));
        a->w = aw[0] = aw[1] = opt->w;
        a->score = a->truesc = -1;
        a->rid = c->rid;

        if (s->qbeg) { // left extension
            uint8_t *rs, *qs;		// qs is query sequence to the left of the seed, rs is ref sequence to the left of the seed
            int qle, tle, gtle, gscore;
            qs = (uint8_t*)CUDAKernelMalloc(d_buffer_ptr, s->qbeg, 1);
            for (i = 0; i < s->qbeg; ++i) qs[i] = query[s->qbeg - 1 - i];
            tmp = s->rbeg - rmax[0];
            rs = (uint8_t*)CUDAKernelMalloc(d_buffer_ptr, tmp, 1);
            for (i = 0; i < tmp; ++i) rs[i] = rseq[tmp - 1 - i];
            for (i = 0; i < MAX_BAND_TRY; ++i) {
                int prev = a->score;
                aw[0] = opt->w << i;
                a->score = ksw_extend2(s->qbeg, qs, tmp, rs, 5, opt->mat, opt->o_del, opt->e_del, opt->o_ins, opt->e_ins, aw[0], opt->pen_clip5, opt->zdrop, s->len * opt->a, &qle, &tle, &gtle, &gscore, &max_off[0], d_buffer_ptr);
                if (a->score == prev || max_off[0] < (aw[0]>>1) + (aw[0]>>2)) break;
            }
            // check whether we prefer to reach the end of the query
            if (gscore <= 0 || gscore <= a->score - opt->pen_clip5) { // local extension
                a->qb = s->qbeg - qle, a->rb = s->rbeg - tle;
                a->truesc = a->score;
            } else { // to-end extension
                a->qb = 0, a->rb = s->rbeg - gtle;
                a->truesc = gscore;
            }
            // 		free(qs); free(rs);
        } else a->score = a->truesc = s->len * opt->a, a->qb = 0, a->rb = s->rbeg;

        if (s->qbeg + s->len != l_query) { // right extension
            int qle, tle, qe, re, gtle, gscore, sc0 = a->score;
            qe = s->qbeg + s->len;
            re = s->rbeg + s->len - rmax[0];
            for (i = 0; i < MAX_BAND_TRY; ++i) {
                int prev = a->score;
                aw[1] = opt->w << i;
                a->score = ksw_extend2(l_query - qe, query + qe, rmax[1] - rmax[0] - re, rseq + re, 5, opt->mat, opt->o_del, opt->e_del, opt->o_ins, opt->e_ins, aw[1], opt->pen_clip3, opt->zdrop, sc0, &qle, &tle, &gtle, &gscore, &max_off[1], d_buffer_ptr);
                if (a->score == prev || max_off[1] < (aw[1]>>1) + (aw[1]>>2)) break;
            }
            // similar to the above
            if (gscore <= 0 || gscore <= a->score - opt->pen_clip3) { // local extension
                a->qe = qe + qle, a->re = rmax[0] + re + tle;
                a->truesc += a->score - sc0;
            } else { // to-end extension
                a->qe = l_query, a->re = rmax[0] + re + gtle;
                a->truesc += gscore - sc0;
            }
        } else a->qe = l_query, a->re = s->rbeg + s->len;

        // compute seedcov
        for (i = 0, a->seedcov = 0; i < c->n; ++i) {
            const mem_seed_t *t = &c->seeds[i];
            if (t->qbeg >= a->qb && t->qbeg + t->len <= a->qe && t->rbeg >= a->rb && t->rbeg + t->len <= a->re) // seed fully contained
                a->seedcov += t->len; // this is not very accurate, but for approx. mapQ, this is good enough
        }
        a->w = aw[0] > aw[1]? aw[0] : aw[1];
        a->seedlen0 = s->len;

        a->frac_rep = c->frac_rep;
    }
    // free(srt); free(rseq);
}


/******************************
 * De-overlap single-end hits *
 ******************************/

__device__ int mem_patch_reg(const mem_opt_t *opt, const bntseq_t *bns, const uint8_t *pac, uint8_t *query, const mem_alnreg_t *a, const mem_alnreg_t *b, int *_w, void* d_buffer_ptr)
{
    int w, score, q_s, r_s;
    double r;
    if (bns == 0 || pac == 0 || query == 0) return 0;
    if (a->rb < bns->l_pac && b->rb >= bns->l_pac) return 0; // on different strands
    if (a->qb >= b->qb || a->qe >= b->qe || a->re >= b->re) return 0; // not colinear
    w = (a->re - b->rb) - (a->qe - b->qb); // required bandwidth
    w = w > 0? w : -w; // l = abs(l)
    r = (double)(a->re - b->rb) / (b->re - a->rb) - (double)(a->qe - b->qb) / (b->qe - a->qb); // relative bandwidth
    r = r > 0.? r : -r; // r = fabs(r)

    if (a->re < b->rb || a->qe < b->qb) { // no overlap on query or on ref
        if (w > opt->w<<1 || r >= PATCH_MAX_R_BW) return 0; // the bandwidth or the relative bandwidth is too large
    } else if (w > opt->w<<2 || r >= PATCH_MAX_R_BW*2) return 0; // more permissive if overlapping on both ref and query
                                                                 // global alignment
    w += a->w + b->w;
    w = w < opt->w<<2? w : opt->w<<2;
    bwa_gen_cigar2_gpu(opt->mat, opt->o_del, opt->e_del, opt->o_ins, opt->e_ins, w, bns->l_pac, pac, b->qe - a->qb, query + a->qb, a->rb, b->re, &score, 0, 0, d_buffer_ptr);
    q_s = (int)((double)(b->qe - a->qb) / ((b->qe - b->qb) + (a->qe - a->qb)) * (b->score + a->score) + .499); // predicted score from query
    r_s = (int)((double)(b->re - a->rb) / ((b->re - b->rb) + (a->re - a->rb)) * (b->score + a->score) + .499); // predicted score from ref
    if ((double)score / (q_s > r_s? q_s : r_s) < PATCH_MIN_SC_RATIO) return 0;
    *_w = w;
    return score;
}

__device__ int mem_sort_dedup_patch(const mem_opt_t *opt, const bntseq_t *bns, const uint8_t *pac, uint8_t *query, int n, mem_alnreg_t *a, void* d_buffer_ptr)
{
    int m, i, j;
    if (n <= 1) return n;
    ks_introsort_mem_ars2(n, a, d_buffer_ptr); // sort by the END position, not START!
    for (i = 0; i < n; ++i) a[i].n_comp = 1;
    for (i = 1; i < n; ++i) {
        mem_alnreg_t *p = &a[i];
        if (p->rid != a[i-1].rid || p->rb >= a[i-1].re + opt->max_chain_gap) continue; // then no need to go into the loop below
        for (j = i - 1; j >= 0 && p->rid == a[j].rid && p->rb < a[j].re + opt->max_chain_gap; --j) {
            mem_alnreg_t *q = &a[j];
            int64_t orr, oq, mr, mq;
            int score, w;
            if (q->qe == q->qb) continue; // a[j] has been excluded
            orr = q->re - p->rb; // overlap length on the reference
            oq = q->qb < p->qb? q->qe - p->qb : p->qe - q->qb; // overlap length on the query
            mr = q->re - q->rb < p->re - p->rb? q->re - q->rb : p->re - p->rb; // min ref len in alignment
            mq = q->qe - q->qb < p->qe - p->qb? q->qe - q->qb : p->qe - p->qb; // min qry len in alignment
            if (orr > opt->mask_level_redun * mr && oq > opt->mask_level_redun * mq) { // one of the hits is redundant
                if (p->score < q->score) {
                    p->qe = p->qb;
                    break;
                } else q->qe = q->qb;
            } else if (q->rb < p->rb && (score = mem_patch_reg(opt, bns, pac, query, q, p, &w, d_buffer_ptr)) > 0) { // then merge q into p
                p->n_comp += q->n_comp + 1;
                p->seedcov = p->seedcov > q->seedcov? p->seedcov : q->seedcov;
                p->sub = p->sub > q->sub? p->sub : q->sub;
                p->csub = p->csub > q->csub? p->csub : q->csub;
                p->qb = q->qb, p->rb = q->rb;
                p->truesc = p->score = score;
                p->w = w;
                q->qb = q->qe;
            }
        }
    }
    for (i = 0, m = 0; i < n; ++i) // exclude identical hits
        if (a[i].qe > a[i].qb) {
            if (m != i) a[m++] = a[i];
            else ++m;
        }
    n = m;
    ks_introsort_mem_ars2(n, a, d_buffer_ptr);
    for (i = 1; i < n; ++i) { // mark identical hits
        if (a[i].score == a[i-1].score && a[i].rb == a[i-1].rb && a[i].qb == a[i-1].qb)
            a[i].qe = a[i].qb;
    }
    for (i = 1, m = 1; i < n; ++i) // exclude identical hits
        if (a[i].qe > a[i].qb) {
            if (m != i) a[m++] = a[i];
            else ++m;
        }
    return m;
}


/********************************************
 * Infer Insert-size distribution from data *
 ********************************************/


typedef struct { size_t n, m; uint64_t *a; } uint64_v;


__device__ static inline int mem_infer_dir(int64_t l_pac, int64_t b1, int64_t b2, int64_t *dist)
{
    int64_t p2;
    int r1 = (b1 >= l_pac), r2 = (b2 >= l_pac);
    p2 = r1 == r2? b2 : (l_pac<<1) - 1 - b2; // p2 is the coordinate of read 2 on the read 1 strand
    *dist = p2 > b1? p2 - b1 : b1 - p2;
    return (r1 == r2? 0 : 1) ^ (p2 > b1? 0 : 3);
}

__device__ static int cal_sub(const mem_opt_t *opt, mem_alnreg_v *r)
{
    int j;
    for (j = 1; j < r->n; ++j) { // choose unique alignment
        int b_max = r->a[j].qb > r->a[0].qb? r->a[j].qb : r->a[0].qb;
        int e_min = r->a[j].qe < r->a[0].qe? r->a[j].qe : r->a[0].qe;
        if (e_min > b_max) { // have overlap
            int min_l = r->a[j].qe - r->a[j].qb < r->a[0].qe - r->a[0].qb? r->a[j].qe - r->a[j].qb : r->a[0].qe - r->a[0].qb;
            if (e_min - b_max >= min_l * opt->mask_level) break; // significant overlap
        }
    }
    return j < r->n? r->a[j].score : opt->min_seed_len * opt->a;
}

#if 0
__device__ void mem_pestat_GPU(const mem_opt_t *opt, int64_t l_pac, int n, const mem_alnreg_v *regs, mem_pestat_t pes[4], void* d_buffer_ptr)
{
    int i, d, max;
    uint64_v isize[4];
    memset(pes, 0, 4 * sizeof(mem_pestat_t));
    memset(isize, 0, 4 * sizeof(uint64_v));
    for (i = 0; i < n>>1; ++i) {
        int dir;
        int64_t is;
        mem_alnreg_v *r[2];
        r[0] = (mem_alnreg_v*)&regs[i<<1|0];
        r[1] = (mem_alnreg_v*)&regs[i<<1|1];
        if (r[0]->n == 0 || r[1]->n == 0) continue;
        if (cal_sub(opt, r[0]) > MIN_RATIO * r[0]->a[0].score) continue;
        if (cal_sub(opt, r[1]) > MIN_RATIO * r[1]->a[0].score) continue;
        if (r[0]->a[0].rid != r[1]->a[0].rid) continue; // not on the same chr
        dir = mem_infer_dir(l_pac, r[0]->a[0].rb, r[1]->a[0].rb, &is);
        if (is && is <= opt->max_ins) {
            // kv_push(uint64_t, v=isize[dir], x=is);
            if (isize[dir].n == isize[dir].m) {
                isize[dir].m = isize[dir].m? isize[dir].m<<1 : 2;
                isize[dir].a = (uint64_t*)CUDAKernelRealloc(d_buffer_ptr, isize[dir].a, sizeof(uint64_t) * isize[dir].m, 8);
            }
            isize[dir].a[isize[dir].n++] = is;
        }
    }
    for (d = 0; d < 4; ++d) { // TODO: this block is nearly identical to the one in bwtsw2_pair.c. It would be better to merge these two.
        mem_pestat_t *r = &pes[d];
        uint64_v *q = &isize[d];
        int p25, p50, p75, x;
        if (q->n < MIN_DIR_CNT) {
            r->failed = 1;
            // free(q->a);
            continue;
        }
        ks_introsort_64(q->n, q->a, d_buffer_ptr);
        p25 = q->a[(int)(.25 * q->n + .499)];
        p50 = q->a[(int)(.50 * q->n + .499)];
        p75 = q->a[(int)(.75 * q->n + .499)];
        r->low  = (int)(p25 - OUTLIER_BOUND * (p75 - p25) + .499);
        if (r->low < 1) r->low = 1;
        r->high = (int)(p75 + OUTLIER_BOUND * (p75 - p25) + .499);
        for (i = x = 0, r->avg = 0; i < q->n; ++i)
            if (q->a[i] >= r->low && q->a[i] <= r->high)
                r->avg += q->a[i], ++x;
        r->avg /= x;
        for (i = 0, r->std = 0; i < q->n; ++i)
            if (q->a[i] >= r->low && q->a[i] <= r->high)
                r->std += (q->a[i] - r->avg) * (q->a[i] - r->avg);
        r->std = sqrt(r->std / x);
        r->low  = (int)(p25 - MAPPING_BOUND * (p75 - p25) + .499);
        r->high = (int)(p75 + MAPPING_BOUND * (p75 - p25) + .499);
        if (r->low  > r->avg - MAX_STDDEV * r->std) r->low  = (int)(r->avg - MAX_STDDEV * r->std + .499);
        if (r->high < r->avg + MAX_STDDEV * r->std) r->high = (int)(r->avg + MAX_STDDEV * r->std + .499);
        if (r->low < 1) r->low = 1;
        // free(q->a);
    }
    for (d = 0, max = 0; d < 4; ++d)
        max = max > isize[d].n? max : isize[d].n;
    for (d = 0; d < 4; ++d)
        if (pes[d].failed == 0 && isize[d].n < max * MIN_DIR_RATIO) {
            pes[d].failed = 1;
        }
}
#endif


/*****************************
 * Basic hit->SAM conversion *
 *****************************/

__device__ static inline int infer_bw(int l1, int l2, int score, int a, int q, int r)
{
    int w;
    if (l1 == l2 && l1 * a - score < (q + r - a)<<1) return 0; // to get equal alignment length, we need at least two gaps
    w = ((double)((l1 < l2? l1 : l2) * a - score - q) / r + 2.);
    if (w < abs(l1 - l2)) w = abs(l1 - l2);
    return w;
}

// __device__ static inline int get_rlen(int n_cigar, const uint32_t *cigar)
// {
// 	int k, l;
// 	for (k = l = 0; k < n_cigar; ++k) {
// 		int op = cigar[k]&0xf;
// 		if (op == 0 || op == 2)
// 			l += cigar[k]>>4;
// 	}
// 	return l;
// }

__device__ static inline void add_cigar(const mem_opt_t *opt, mem_aln_t *p, kstring_t *str, int which, void* d_buffer_ptr)
{
    int i;
    if (p->n_cigar) { // aligned
        for (i = 0; i < p->n_cigar; ++i) {
            int c = p->cigar[i]&0xf;
            if (!(opt->flag&MEM_F_SOFTCLIP) && !p->is_alt && (c == 3 || c == 4))
                c = which? 4 : 3; // use hard clipping for supplementary alignments
            kputw(p->cigar[i]>>4, str, d_buffer_ptr); kputc("MIDSH"[c], str, d_buffer_ptr);
        }
    } else kputc('*', str, d_buffer_ptr); // having a coordinate but unaligned (e.g. when copy_mate is true)
}

// __device__ static void mem_aln2sam(const mem_opt_t *opt, const bntseq_t *bns, kstring_t *str, bseq1_t *s, int n, const mem_aln_t *list, int which, const mem_aln_t *m_, void* d_buffer_ptr)
// {
// 	int i, l_name;
// 	mem_aln_t ptmp = list[which], *p = &ptmp, mtmp, *m = 0; // make a copy of the alignment to convert

// 	if (m_) mtmp = *m_, m = &mtmp;
// 	// set flag
// 	p->flag |= m? 0x1 : 0; // is paired in sequencing
// 	p->flag |= p->rid < 0? 0x4 : 0; // is mapped
// 	p->flag |= m && m->rid < 0? 0x8 : 0; // is mate mapped
// 	if (p->rid < 0 && m && m->rid >= 0) // copy mate to alignment
// 		p->rid = m->rid, p->pos = m->pos, p->is_rev = m->is_rev, p->n_cigar = 0;
// 	if (m && m->rid < 0 && p->rid >= 0) // copy alignment to mate
// 		m->rid = p->rid, m->pos = p->pos, m->is_rev = p->is_rev, m->n_cigar = 0;
// 	p->flag |= p->is_rev? 0x10 : 0; // is on the reverse strand
// 	p->flag |= m && m->is_rev? 0x20 : 0; // is mate on the reverse strand

// 	// print up to CIGAR
// 	l_name = strlen_GPU(s->name);
// 	ks_resize(str, str->l + s->l_seq + l_name + (s->qual? s->l_seq : 0) + 20, d_buffer_ptr);
// 	kputsn(s->name, l_name, str, d_buffer_ptr); kputc('\t', str, d_buffer_ptr); // QNAME
// 	kputw((p->flag&0xffff) | (p->flag&0x10000? 0x100 : 0), str, d_buffer_ptr); kputc('\t', str, d_buffer_ptr); // FLAG
// 	if (p->rid >= 0) { // with coordinate
// 		kputs(bns->anns[p->rid].name, str, d_buffer_ptr); kputc('\t', str, d_buffer_ptr); // RNAME
// 		kputl(p->pos + 1, str, d_buffer_ptr); kputc('\t', str, d_buffer_ptr); // POS
// 		kputw(p->mapq, str, d_buffer_ptr); kputc('\t', str, d_buffer_ptr); // MAPQ
// 		add_cigar(opt, p, str, which, d_buffer_ptr);
// 	} else kputsn("*\t0\t0\t*", 7, str, d_buffer_ptr); // without coordinte
// 	kputc('\t', str, d_buffer_ptr);

// 	// print the mate position if applicable
// 	if (m && m->rid >= 0) {
// 		if (p->rid == m->rid) kputc('=', str, d_buffer_ptr);
// 		else kputs(bns->anns[m->rid].name, str, d_buffer_ptr);
// 		kputc('\t', str, d_buffer_ptr);
// 		kputl(m->pos + 1, str, d_buffer_ptr); kputc('\t', str, d_buffer_ptr);
// 		if (p->rid == m->rid) {
// 			int64_t p0 = p->pos + (p->is_rev? get_rlen(p->n_cigar, p->cigar) - 1 : 0);
// 			int64_t p1 = m->pos + (m->is_rev? get_rlen(m->n_cigar, m->cigar) - 1 : 0);
// 			if (m->n_cigar == 0 || p->n_cigar == 0) kputc('0', str, d_buffer_ptr);
// 			else kputl(-(p0 - p1 + (p0 > p1? 1 : p0 < p1? -1 : 0)), str, d_buffer_ptr);
// 		} else kputc('0', str, d_buffer_ptr);
// 	} else kputsn("*\t0\t0", 5, str, d_buffer_ptr);
// 	kputc('\t', str, d_buffer_ptr);

// 	// print SEQ and QUAL
// 	if (p->flag & 0x100) { // for secondary alignments, don't write SEQ and QUAL
// 		kputsn("*\t*", 3, str, d_buffer_ptr);
// 	} else if (!p->is_rev) { // the forward strand
// 		int i, qb = 0, qe = s->l_seq;
// 		if (p->n_cigar && which && !(opt->flag&MEM_F_SOFTCLIP) && !p->is_alt) { // have cigar && not the primary alignment && not softclip all
// 			if ((p->cigar[0]&0xf) == 4 || (p->cigar[0]&0xf) == 3) qb += p->cigar[0]>>4;
// 			if ((p->cigar[p->n_cigar-1]&0xf) == 4 || (p->cigar[p->n_cigar-1]&0xf) == 3) qe -= p->cigar[p->n_cigar-1]>>4;
// 		}
// 		ks_resize(str, str->l + (qe - qb) + 1, d_buffer_ptr);
// 		for (i = qb; i < qe; ++i) str->s[str->l++] = "ACGTN"[(int)s->seq[i]];
// 		kputc('\t', str, d_buffer_ptr);
// 		if (s->qual) { // printf qual
// 			ks_resize(str, str->l + (qe - qb) + 1, d_buffer_ptr);
// 			for (i = qb; i < qe; ++i) str->s[str->l++] = s->qual[i];
// 			str->s[str->l] = 0;
// 		} else kputc('*', str, d_buffer_ptr);
// 	} else { // the reverse strand
// 		int i, qb = 0, qe = s->l_seq;
// 		if (p->n_cigar && which && !(opt->flag&MEM_F_SOFTCLIP) && !p->is_alt) {
// 			if ((p->cigar[0]&0xf) == 4 || (p->cigar[0]&0xf) == 3) qe -= p->cigar[0]>>4;
// 			if ((p->cigar[p->n_cigar-1]&0xf) == 4 || (p->cigar[p->n_cigar-1]&0xf) == 3) qb += p->cigar[p->n_cigar-1]>>4;
// 		}
// 		ks_resize(str, str->l + (qe - qb) + 1, d_buffer_ptr);
// 		for (i = qe-1; i >= qb; --i) str->s[str->l++] = "TGCAN"[(int)s->seq[i]];
// 		kputc('\t', str, d_buffer_ptr);
// 		if (s->qual) { // printf qual
// 			ks_resize(str, str->l + (qe - qb) + 1, d_buffer_ptr);
// 			for (i = qe-1; i >= qb; --i) str->s[str->l++] = s->qual[i];
// 			str->s[str->l] = 0;
// 		} else kputc('*', str, d_buffer_ptr);
// 	}

// 	// print optional tags
// 	if (p->n_cigar) {
// 		kputsn("\tNM:i: ", 6, str, d_buffer_ptr); kputw(p->NM, str, d_buffer_ptr);
// 		kputsn("\tMD:Z: ", 6, str, d_buffer_ptr); kputs((char*)(p->cigar + p->n_cigar), str, d_buffer_ptr);
// 	}
// 	if (m && m->n_cigar) { kputsn("\tMC:Z: ", 6, str, d_buffer_ptr); add_cigar(opt, m, str, which, d_buffer_ptr); }
// 	if (p->score >= 0) { kputsn("\tAS:i: ", 6, str, d_buffer_ptr); kputw(p->score, str, d_buffer_ptr); }
// 	if (p->sub >= 0) { kputsn("\tXS:i: ", 6, str, d_buffer_ptr); kputw(p->sub, str, d_buffer_ptr); }
// 	// if (bwa_rg_id[0]) { kputsn("\tRG:Z: ", 6, str, d_buffer_ptr); kputs(bwa_rg_id, str, d_buffer_ptr); }
// 	if (!(p->flag & 0x100)) { // not multi-hit
// 		for (i = 0; i < n; ++i)
// 			if (i != which && !(list[i].flag&0x100)) break;
// 		if (i < n) { // there are other primary hits; output them
// 			kputsn("\tSA:Z: ", 6, str, d_buffer_ptr);
// 			for (i = 0; i < n; ++i) {
// 				const mem_aln_t *r = &list[i];
// 				int k;
// 				if (i == which || (r->flag&0x100)) continue; // proceed if: 1) different from the current; 2) not shadowed multi hit
// 				kputs(bns->anns[r->rid].name, str, d_buffer_ptr); kputc(',', str, d_buffer_ptr);
// 				kputl(r->pos+1, str, d_buffer_ptr); kputc(',', str, d_buffer_ptr);
// 				kputc("+-"[r->is_rev], str, d_buffer_ptr); kputc(',', str, d_buffer_ptr);
// 				for (k = 0; k < r->n_cigar; ++k) {
// 					kputw(r->cigar[k]>>4, str, d_buffer_ptr); kputc("MIDSH"[r->cigar[k]&0xf], str, d_buffer_ptr);
// 				}
// 				kputc(',', str, d_buffer_ptr); kputw(r->mapq, str, d_buffer_ptr);
// 				kputc(',', str, d_buffer_ptr); kputw(r->NM, str, d_buffer_ptr);
// 				kputc(';', str, d_buffer_ptr);
// 			}
// 		}
// 		if (p->alt_sc > 0)
// 			ksprintf(str, "\tpa:f:%.3f", (float)p->score / p->alt_sc, d_buffer_ptr);
// 	}
// 	if (p->XA) {
// 		kputsn((opt->flag&MEM_F_XB)? "\tXB:Z: " : "\tXA:Z: ", 6, str, d_buffer_ptr);
// 		kputs(p->XA, str, d_buffer_ptr);
// 	}
// 	if (s->comment) { kputc('\t', str, d_buffer_ptr); kputs(s->comment, str, d_buffer_ptr); }
// 	if ((opt->flag&MEM_F_REF_HDR) && p->rid >= 0 && bns->anns[p->rid].anno != 0 && bns->anns[p->rid].anno[0] != 0) {
// 		int tmp;
// 		kputsn("\tXR:Z: ", 6, str, d_buffer_ptr);
// 		tmp = str->l;
// 		kputs(bns->anns[p->rid].anno, str, d_buffer_ptr);
// 		for (i = tmp; i < str->l; ++i) // replace TAB in the comment to SPACE
// 			if (str->s[i] == '\t') str->s[i] = ' ';
// 	}
// 	kputc('\n', str, d_buffer_ptr);
// }


/*****************************************************
 * Device functions for generating alignment results *
 *****************************************************/
typedef struct { size_t n, m; int *a; } int_v;

__device__ static inline int64_t bns_depos_GPU(const bntseq_t *bns, int64_t pos, int *is_rev)
{
    return (*is_rev = (pos >= bns->l_pac))? (bns->l_pac<<1) - 1 - pos : pos;
}

__device__ static int mem_approx_mapq_se(const mem_opt_t *opt, const mem_alnreg_t *a)
{
    int mapq, l, sub = a->sub? a->sub : opt->min_seed_len * opt->a;
    double identity;
    sub = a->csub > sub? a->csub : sub;
    if (sub >= a->score) return 0;
    l = a->qe - a->qb > a->re - a->rb? a->qe - a->qb : a->re - a->rb;
    identity = 1. - (double)(l * opt->a - a->score) / (opt->a + opt->b) / l;
    if (a->score == 0) {
        mapq = 0;
    } else if (opt->mapQ_coef_len > 0) {
        double tmp;
        tmp = l < opt->mapQ_coef_len? 1. : opt->mapQ_coef_fac / log((float)l);
        tmp *= identity * identity;
        mapq = (int)(6.02 * (a->score - sub) / opt->a * tmp * tmp + .499);
    } else {
        mapq = (int)(MEM_MAPQ_COEF * (1. - (double)sub / a->score) * log((float)a->seedcov) + .499);
        mapq = identity < 0.95? (int)(mapq * identity * identity + .499) : mapq;
    }
    if (a->sub_n > 0) mapq -= (int)(4.343 * log((float)a->sub_n+1) + .499);
    if (mapq > 60) mapq = 60;
    if (mapq < 0) mapq = 0;
    mapq = (int)(mapq * (1. - a->frac_rep) + .499);
    return mapq;
}


__device__ static inline uint64_t hash_64(uint64_t key)
{
    key += ~(key << 32);
    key ^= (key >> 22);
    key += ~(key << 13);
    key ^= (key >> 8);
    key += (key << 3);
    key ^= (key >> 15);
    key += ~(key << 27);
    key ^= (key >> 31);
    return key;
}

__device__ static void mem_mark_primary_se_core_GPU(const mem_opt_t *opt, int n, mem_alnreg_t *a, int_v *z, void* d_buffer_ptr)
{ // similar to the loop in mem_chain_flt()
    int i, k, tmp;
    tmp = opt->a + opt->b;
    tmp = opt->o_del + opt->e_del > tmp? opt->o_del + opt->e_del : tmp;
    tmp = opt->o_ins + opt->e_ins > tmp? opt->o_ins + opt->e_ins : tmp;
    z->n = 0;
    // kv_push(type=int, v=*z, x=0);
    if (z->n == z->m) {
        z->m = z->m? z->m<<1 : 2;
        z->a = (int*)CUDAKernelRealloc(d_buffer_ptr, z->a, sizeof(int) * z->m, 4);
    }
    z->a[z->n++] = 0;	
    for (i = 1; i < n; ++i) {
        for (k = 0; k < z->n; ++k) {
            int j = z->a[k];
            int b_max = a[j].qb > a[i].qb? a[j].qb : a[i].qb;
            int e_min = a[j].qe < a[i].qe? a[j].qe : a[i].qe;
            if (e_min > b_max) { // have overlap
                int min_l = a[i].qe - a[i].qb < a[j].qe - a[j].qb? a[i].qe - a[i].qb : a[j].qe - a[j].qb;
                if (e_min - b_max >= min_l * opt->mask_level) { // significant overlap
                    if (a[j].sub == 0) a[j].sub = a[i].score;
                    if (a[j].score - a[i].score <= tmp && (a[j].is_alt || !a[i].is_alt))
                        ++a[j].sub_n;
                    break;
                }
            }
        }
        if (k == z->n){
            // kv_push(int, *z, i);
            if (z->n == z->m) {
                z->m = z->m? z->m<<1 : 2;
                z->a = (int*)CUDAKernelRealloc(d_buffer_ptr, z->a, sizeof(int) * z->m, 4);
            }
            z->a[z->n++] = i;
        }
        else a[i].secondary = z->a[k];
    }
}

// __device__ static mem_aln_t mem_reg2aln_GPU(const mem_opt_t *opt, const bntseq_t *bns, const uint8_t *pac, int l_query, const char *query_, const mem_alnreg_t *ar, void* d_buffer_ptr)
// {
// 	mem_aln_t a;
// 	int i, w2, tmp, qb, qe, NM, score, is_rev, last_sc = -(1<<30), l_MD;
// 	int64_t pos, rb, re;
// 	uint8_t *query;

// 	memset(&a, 0, sizeof(mem_aln_t));
// 	if (ar == 0 || ar->rb < 0 || ar->re < 0) { // generate an unmapped record
// 		a.rid = -1; a.pos = -1; a.flag |= 0x4;
// 		return a;
// 	}
// 	qb = ar->qb, qe = ar->qe;
// 	rb = ar->rb, re = ar->re;
// 	query = (uint8_t*)CUDAKernelMalloc(d_buffer_ptr, l_query, 1);
// 	for (i = 0; i < l_query; ++i) // convert to the nt4 encoding
// 		query[i] = query_[i] < 5? query_[i] : d_nst_nt4_table[(int)query_[i]];
// 	a.mapq = ar->secondary < 0? mem_approx_mapq_se(opt, ar) : 0;
// 	if (ar->secondary >= 0) a.flag |= 0x100; // secondary alignment
// 	tmp = infer_bw(qe - qb, re - rb, ar->truesc, opt->a, opt->o_del, opt->e_del);
// 	w2  = infer_bw(qe - qb, re - rb, ar->truesc, opt->a, opt->o_ins, opt->e_ins);
// 	w2 = w2 > tmp? w2 : tmp;
// 	if (w2 > opt->w) w2 = w2 < ar->w? w2 : ar->w;
// 	i = 0; a.cigar = 0;
// 	do {
// 		// free(a.cigar);
// 		w2 = w2 < opt->w<<2? w2 : opt->w<<2;
// 		a.cigar = bwa_gen_cigar2_gpu(opt->mat, opt->o_del, opt->e_del, opt->o_ins, opt->e_ins, w2, bns->l_pac, pac, qe - qb, (uint8_t*)&query[qb], rb, re, &score, &a.n_cigar, &NM, d_buffer_ptr);
// 		if (score == last_sc || w2 == opt->w<<2) break; // it is possible that global alignment and local alignment give different scores
// 		last_sc = score;
// 		w2 <<= 1;
// 	} while (++i < 3 && score < ar->truesc - opt->a);
// 	l_MD = strlen_GPU((char*)(a.cigar + a.n_cigar)) + 1;

// 	a.NM = NM;
// 	pos = bns_depos_GPU(bns, rb < bns->l_pac? rb : re - 1, &is_rev);
// 	a.is_rev = is_rev;
// 	if (a.n_cigar > 0) { // squeeze out leading or trailing deletions
// 		if ((a.cigar[0]&0xf) == 2) {
// 			pos += a.cigar[0]>>4;
// 			--a.n_cigar;
// 			cudaKernelMemmove(a.cigar + 1, a.cigar, a.n_cigar * 4 + l_MD);
// 		} else if ((a.cigar[a.n_cigar-1]&0xf) == 2) {
// 			--a.n_cigar;
// 			cudaKernelMemmove(a.cigar + a.n_cigar + 1, a.cigar + a.n_cigar, l_MD); // MD needs to be moved accordingly
// 		}
// 	}
// 	if (qb != 0 || qe != l_query) { // add clipping to CIGAR
// 		int clip5, clip3;
// 		clip5 = is_rev? l_query - qe : qb;
// 		clip3 = is_rev? qb : l_query - qe;
// 		a.cigar = (uint32_t*)CUDAKernelRealloc(d_buffer_ptr, a.cigar, 4 * (a.n_cigar + 2) + l_MD, 4);
// 		if (clip5) {
// 			cudaKernelMemmove(a.cigar, a.cigar+1, a.n_cigar * 4 + l_MD); // make room for 5'-end clipping
// 			a.cigar[0] = clip5<<4 | 3;
// 			++a.n_cigar;
// 		}
// 		if (clip3) {
// 			cudaKernelMemmove(a.cigar + a.n_cigar, a.cigar + a.n_cigar + 1, l_MD); // make room for 3'-end clipping
// 			a.cigar[a.n_cigar++] = clip3<<4 | 3;
// 		}
// 	}
// 	a.rid = bns_pos2rid_gpu(bns, pos);
// 	// assert(a.rid == ar->rid);
// 	a.pos = pos - bns->anns[a.rid].offset;
// 	a.score = ar->score; a.sub = ar->sub > ar->csub? ar->sub : ar->csub;
// 	a.is_alt = ar->is_alt; a.alt_sc = ar->alt_sc;
// 	// free(query);
// 	return a;
// }


__device__ int mem_mark_primary_se_GPU(const mem_opt_t *d_opt, int n, mem_alnreg_t *a, int id, void* d_buffer_ptr)
{
    int i, n_pri;
    int_v z = {0,0,0};
    if (n == 0) return 0;
    for (i = n_pri = 0; i < n; ++i) {
        a[i].sub = a[i].alt_sc = 0, a[i].secondary = a[i].secondary_all = -1, a[i].hash = hash_64(id+i);
        if (!a[i].is_alt) ++n_pri;
    }
    ks_introsort_mem_ars_hash(n, a, d_buffer_ptr);
    mem_mark_primary_se_core_GPU(d_opt, n, a, &z, d_buffer_ptr);
    for (i = 0; i < n; ++i) {
        mem_alnreg_t *p = &a[i];
        p->secondary_all = i; // keep the rank in the first round
        if (!p->is_alt && p->secondary >= 0 && a[p->secondary].is_alt)
            p->alt_sc = a[p->secondary].score;
    }
    if (n_pri >= 0 && n_pri < n) {
        // kv_resize(int, z, n);
        z.m = n; z.a = (int*)CUDAKernelRealloc(d_buffer_ptr, z.a, sizeof(int) * z.m, 4);
        if (n_pri > 0) ks_introsort_mem_ars_hash2(n, a, d_buffer_ptr);
        for (i = 0; i < n; ++i) z.a[a[i].secondary_all] = i;
        for (i = 0; i < n; ++i) {
            if (a[i].secondary >= 0) {
                a[i].secondary_all = z.a[a[i].secondary];
                if (a[i].is_alt) a[i].secondary = INT_MAX;
            } else a[i].secondary_all = -1;
        }
        if (n_pri > 0) { // mark primary for hits to the primary assembly only
            for (i = 0; i < n_pri; ++i) a[i].sub = 0, a[i].secondary = -1;
            mem_mark_primary_se_core_GPU(d_opt, n_pri, a, &z, d_buffer_ptr);
        }
    } else {
        for (i = 0; i < n; ++i)
            a[i].secondary_all = a[i].secondary;
    }
    // free(z.a);
    return n_pri;
}


// __device__ static void mem_reorder_primary5(int T, mem_alnreg_v *a)
// {
// 	int k, n_pri = 0, left_st = INT_MAX, left_k = -1;
// 	mem_alnreg_t t;
// 	for (k = 0; k < a->n; ++k)
// 		if (a->a[k].secondary < 0 && !a->a[k].is_alt && a->a[k].score >= T) ++n_pri;
// 	if (n_pri <= 1) return; // only one alignment
// 	for (k = 0; k < a->n; ++k) {
// 		mem_alnreg_t *p = &a->a[k];
// 		if (p->secondary >= 0 || p->is_alt || p->score < T) continue;
// 		if (p->qb < left_st) left_st = p->qb, left_k = k;
// 	}
// 	// assert(a->a[0].secondary < 0);
// 	if (left_k == 0) return; // no need to reorder
// 	t = a->a[0], a->a[0] = a->a[left_k], a->a[left_k] = t;
// 	for (k = 1; k < a->n; ++k) { // update secondary and secondary_all
// 		mem_alnreg_t *p = &a->a[k];
// 		if (p->secondary == 0) p->secondary = left_k;
// 		else if (p->secondary == left_k) p->secondary = 0;
// 		if (p->secondary_all == 0) p->secondary_all = left_k;
// 		else if (p->secondary_all == left_k) p->secondary_all = 0;
// 	}
// }

// ONLY work after mem_mark_primary_se()
// __device__ static char** mem_gen_alt(const mem_opt_t *opt, const bntseq_t *bns, const uint8_t *pac, const mem_alnreg_v *a, int l_query, const char *query, void* d_buffer_ptr) 
// {
// 	int i, k, r, *cnt, tot;
// 	kstring_t *aln = 0, str = {0,0,0};
// 	char **XA = 0, *has_alt;

// 	cnt = (int*)CUDAKernelCalloc(d_buffer_ptr, a->n, sizeof(int), 4);
// 	has_alt = (char*)CUDAKernelCalloc(d_buffer_ptr, a->n, 1, 1);
// 	for (i = 0, tot = 0; i < a->n; ++i) {
// 		// r = get_pri_idx(opt->XA_drop_ratio, a->a, i);
// 		int kk = a->a[i].secondary_all;
// 		if (kk >= 0 && a->a[i].score >= a->a[kk].score * opt->XA_drop_ratio) r = kk;
// 		else r = -1;
// 		if (r >= 0) {
// 			++cnt[r], ++tot;
// 			if (a->a[i].is_alt) has_alt[r] = 1;
// 		}
// 	}

// 	if (tot == 0) goto end_gen_alt;
// 	aln =(kstring_t*)CUDAKernelCalloc(d_buffer_ptr, a->n, sizeof(kstring_t), 8);
// 	for (i = 0; i < a->n; ++i) {
// 		mem_aln_t t;
// 		// if ((r = get_pri_idx(opt->XA_drop_ratio, a->a, i)) < 0) continue;
// 		int kk = a->a[i].secondary_all;
// 		if (kk >= 0 && a->a[i].score >= a->a[kk].score * opt->XA_drop_ratio) r = kk;
// 		else r = -1;
// 		if (r<0) continue;
// 		if (cnt[r] > opt->max_XA_hits_alt || (!has_alt[r] && cnt[r] > opt->max_XA_hits)) continue;
// 		t = mem_reg2aln_GPU(opt, bns, pac, l_query, query, &a->a[i], d_buffer_ptr);
// 		str.l = 0;
// 		kputs(bns->anns[t.rid].name, &str, d_buffer_ptr);
// 		kputc(',', &str, d_buffer_ptr); kputc("+-"[t.is_rev], &str, d_buffer_ptr); kputl(t.pos + 1, &str, d_buffer_ptr);
// 		kputc(',', &str, d_buffer_ptr);
// 		for (k = 0; k < t.n_cigar; ++k) {
// 			kputw(t.cigar[k]>>4, &str, d_buffer_ptr);
// 			kputc("MIDSHN"[t.cigar[k]&0xf], &str, d_buffer_ptr);
// 		}
// 		kputc(',', &str, d_buffer_ptr); kputw(t.NM, &str, d_buffer_ptr);
// 		if (opt->flag & MEM_F_XB) {
// 			kputc(',', &str, d_buffer_ptr);
// 			kputw(t.score, &str, d_buffer_ptr);
// 		}
// 		kputc(';', &str, d_buffer_ptr);
// // 		free(t.cigar);
// 		kputsn(str.s, str.l, &aln[r], d_buffer_ptr);
// 	}
// 	XA = (char**)CUDAKernelCalloc(d_buffer_ptr, a->n, sizeof(char*), 8);
// 	for (k = 0; k < a->n; ++k)
// 		XA[k] = aln[k].s;

// end_gen_alt:
// // 	free(has_alt); free(cnt); free(aln); free(str.s);
// 	return XA;
// }

/* alignments have been marked primary/secondary and filter out. Now write all alignments left */
// __device__ static void mem_reg2sam(const mem_opt_t *opt, const bntseq_t *bns, const uint8_t *pac, bseq1_t *s, mem_alnreg_v *a, int extra_flag, const mem_aln_t *m, void* d_buffer_ptr)
// {
// 	kstring_t str;
// 	struct { size_t n, m; mem_aln_t *a; } aa;
// 	aa.n = a->n; 
// 	aa.a = (mem_aln_t*)CUDAKernelMalloc(d_buffer_ptr, aa.n*sizeof(mem_aln_t), 8);
// 	int k, l;
// 	char **XA = 0;

// 	if (!(opt->flag & MEM_F_ALL))
// 		XA = mem_gen_alt(opt, bns, pac, a, s->l_seq, s->seq, d_buffer_ptr);
// 	str.l = str.m = 0; str.s = 0;
// 	for (k = 0; k < a->n; ++k) {
// 		mem_alnreg_t *p = &a->a[k];
// 		mem_aln_t *q = &aa.a[k];
// 		*q = mem_reg2aln_GPU(opt, bns, pac, s->l_seq, s->seq, p, d_buffer_ptr);
// 		q->XA = XA? XA[k] : 0;
// 		q->flag |= extra_flag; // flag secondary
// 		if (p->secondary >= 0) q->sub = -1; // don't output sub-optimal score
// 		if (k>0 && p->secondary < 0) // if supplementary
// 			q->flag |= (opt->flag&MEM_F_NO_MULTI)? 0x10000 : 0x800;
// 		if (!(opt->flag & MEM_F_KEEP_SUPP_MAPQ) && (k>0) && !p->is_alt && q->mapq > aa.a[0].mapq)
// 			q->mapq = aa.a[0].mapq; // lower mapq for supplementary mappings, unless -5 or -q is applied
// 	}
// 	if (aa.n == 0) { // no alignments good enough; then write an unaligned record
// 		mem_aln_t t;
// 		t = mem_reg2aln_GPU(opt, bns, pac, s->l_seq, s->seq, 0, d_buffer_ptr);
// 		t.flag |= extra_flag;
// 		mem_aln2sam(opt, bns, &str, s, 1, &t, 0, m, d_buffer_ptr);
// 	} else {
// 		for (k = 0; k < aa.n; ++k)
// 			mem_aln2sam(opt, bns, &str, s, aa.n, aa.a, k, m, d_buffer_ptr);
// 		// for (k = 0; k < aa.n; ++k) free(aa.a[k].cigar);
// 		// free(aa.a);
// 	}
// 	l = strlen_GPU(str.s); 		// length of output
// 	k = atomicAdd(&d_seq_sam_size, l+1);	// offset to output to d_seq_sam_ptr
// 	memcpy(&d_seq_sam_ptr[k], str.s, l+1);	// copy sam to output
// 	s->sam = (char*)k; 	// record offset
// 	// if (XA) {
// 	// 	for (k = 0; k < a->n; ++k) free(XA[k]);
// 	// 	free(XA);
// 	// }
// }

/**********************************************
 * Device functions for paired-end alignments *
 **********************************************/

// __device__ static int mem_pair(const mem_opt_t *opt, const bntseq_t *bns, const uint8_t *pac, const mem_pestat_t pes[4], bseq1_t s[2], mem_alnreg_v a[2], int id, int *sub, int *n_sub, int z[2], int n_pri[2], void* d_buffer_ptr)
// {
// 	pair64_v v, u;
// 	int r, i, k, y[4], ret; // y[] keeps the last hit
// 	int64_t l_pac = bns->l_pac;
// 	v.n = 0; v.m = 0; v.a = 0;
// 	u.n = 0; u.m = 0; u.a = 0;
// 	for (r = 0; r < 2; ++r) { // loop through read number
// 		for (i = 0; i < n_pri[r]; ++i) {
// 			pair64_t key;
// 			mem_alnreg_t *e = &a[r].a[i];
// 			key.x = e->rb < l_pac? e->rb : (l_pac<<1) - 1 - e->rb; // forward position
// 			key.x = (uint64_t)e->rid<<32 | (key.x - bns->anns[e->rid].offset);
// 			key.y = (uint64_t)e->score << 32 | i << 2 | (e->rb >= l_pac)<<1 | r;
// 			// kv_push(pair64_t, v=v, x=key);
// 			if (v.n == v.m) {
// 				v.m = (v).m? v.m<<1 : 2;
// 				v.a = (pair64_t*)CUDAKernelRealloc(d_buffer_ptr, v.a, sizeof(pair64_t) * v.m, 8);
// 			}
// 			v.a[v.n++] = key;
// 		}
// 	}
// 	ks_introsort_128(v.n, v.a, d_buffer_ptr);
// 	y[0] = y[1] = y[2] = y[3] = -1;
// 	//for (i = 0; i < v.n; ++i) printf("[%d]\t%d\t%c%ld\n", i, (int)(v.a[i].y&1)+1, "+-"[v.a[i].y>>1&1], (long)v.a[i].x);
// 	for (i = 0; i < v.n; ++i) {
// 		for (r = 0; r < 2; ++r) { // loop through direction
// 			int dir = r<<1 | (v.a[i].y>>1&1), which;
// 			if (pes[dir].failed) continue; // invalid orientation
// 			which = r<<1 | ((v.a[i].y&1)^1);
// 			if (y[which] < 0) continue; // no previous hits
// 			for (k = y[which]; k >= 0; --k) { // TODO: this is a O(n^2) solution in the worst case; remember to check if this loop takes a lot of time (I doubt)
// 				int64_t dist;
// 				int q;
// 				double ns;
// 				pair64_t *p;
// 				if ((v.a[k].y&3) != which) continue;
// 				dist = (int64_t)v.a[i].x - v.a[k].x;
// 				//printf("%d: %lld\n", k, dist);
// 				if (dist > pes[dir].high) break;
// 				if (dist < pes[dir].low)  continue;
// 				ns = (dist - pes[dir].avg) / pes[dir].std;
// 				q = (int)((v.a[i].y>>32) + (v.a[k].y>>32) + .721 * log(2. * erfc(fabs(ns) * M_SQRT1_2)) * opt->a + .499); // .721 = 1/log(4)
// 				if (q < 0) q = 0;
// 				// p = kv_pushp(pair64_t, u);
// 				p = (((u.n == u.m)
// 				   	? (u.m = (u.m? u.m<<1 : 2), u.a = (pair64_t*)CUDAKernelRealloc(d_buffer_ptr, u.a, sizeof(pair64_t) * u.m, 8), 0)
// 					: 0), &u.a[u.n++]);
// 				p->y = (uint64_t)k<<32 | i;
// 				p->x = (uint64_t)q<<32 | (hash_64(p->y ^ id<<8) & 0xffffffffU);
// 				//printf("[%lld,%lld]\t%d\tdist=%ld\n", v.a[k].x, v.a[i].x, q, (long)dist);
// 			}
// 		}
// 		y[v.a[i].y&3] = i;
// 	}
// 	if (u.n) { // found at least one proper pair
// 		int tmp = opt->a + opt->b;
// 		tmp = tmp > opt->o_del + opt->e_del? tmp : opt->o_del + opt->e_del;
// 		tmp = tmp > opt->o_ins + opt->e_ins? tmp : opt->o_ins + opt->e_ins;
// 		ks_introsort_128(u.n, u.a, d_buffer_ptr);
// 		i = u.a[u.n-1].y >> 32; k = u.a[u.n-1].y << 32 >> 32;
// 		z[v.a[i].y&1] = v.a[i].y<<32>>34; // index of the best pair
// 		z[v.a[k].y&1] = v.a[k].y<<32>>34;
// 		ret = u.a[u.n-1].x >> 32;
// 		*sub = u.n > 1? u.a[u.n-2].x>>32 : 0;
// 		for (i = (long)u.n - 2, *n_sub = 0; i >= 0; --i)
// 			if (*sub - (int)(u.a[i].x>>32) <= tmp) ++*n_sub;
// 	} else ret = 0, *sub = 0, *n_sub = 0;
// 	// free(u.a); free(v.a);
// 	return ret;
// }

__device__ int mem_matesw(const mem_opt_t *opt, const bntseq_t *bns, const uint8_t *pac, const mem_pestat_t pes[4], const mem_alnreg_t *a, int l_ms, const uint8_t *ms, mem_alnreg_v *ma, void* d_buffer_ptr)
{
    int64_t l_pac = bns->l_pac;
    int i, r, skip[4], n = 0, rid;
    for (r = 0; r < 4; ++r)
        skip[r] = pes[r].failed? 1 : 0;
    for (i = 0; i < ma->n; ++i) { // check which orinentation has been found
        int64_t dist;
        r = mem_infer_dir(l_pac, a->rb, ma->a[i].rb, &dist);
        if (dist >= pes[r].low && dist <= pes[r].high)
            skip[r] = 1;
    }
    if (skip[0] + skip[1] + skip[2] + skip[3] == 4) return 0; // consistent pair exist; no need to perform SW
    for (r = 0; r < 4; ++r) {
        int is_rev, is_larger;
        uint8_t *seq, *rev = 0, *ref = 0;
        int64_t rb, re;
        if (skip[r]) continue;
        is_rev = (r>>1 != (r&1)); // whether to reverse complement the mate
        is_larger = !(r>>1); // whether the mate has larger coordinate
        if (is_rev) {
            rev = (uint8_t*)CUDAKernelMalloc(d_buffer_ptr, l_ms, 1); // this is the reverse complement of $ms
            for (i = 0; i < l_ms; ++i) rev[l_ms - 1 - i] = ms[i] < 4? 3 - ms[i] : 4;
            seq = rev;
        } else seq = (uint8_t*)ms;
        if (!is_rev) {
            rb = is_larger? a->rb + pes[r].low : a->rb - pes[r].high;
            re = (is_larger? a->rb + pes[r].high: a->rb - pes[r].low) + l_ms; // if on the same strand, end position should be larger to make room for the seq length
        } else {
            rb = (is_larger? a->rb + pes[r].low : a->rb - pes[r].high) - l_ms; // similarly on opposite strands
            re = is_larger? a->rb + pes[r].high: a->rb - pes[r].low;
        }
        if (rb < 0) rb = 0;
        if (re > l_pac<<1) re = l_pac<<1;
        if (rb < re) ref = bns_fetch_seq_gpu(bns, pac, &rb, (rb+re)>>1, &re, &rid, d_buffer_ptr);
        if (a->rid == rid && re - rb >= opt->min_seed_len) { // no funny things happening
            kswr_t aln;
            mem_alnreg_t b;
            int tmp, xtra = KSW_XSUBO | KSW_XSTART | (l_ms * opt->a < 250? KSW_XBYTE : 0) | (opt->min_seed_len * opt->a);
            aln = ksw_align2(l_ms, seq, re - rb, ref, 5, opt->mat, opt->o_del, opt->e_del, opt->o_ins, opt->e_ins, xtra, 0, d_buffer_ptr);
            memset(&b, 0, sizeof(mem_alnreg_t));
            if (aln.score >= opt->min_seed_len && aln.qb >= 0) { // something goes wrong if aln.qb < 0
                b.rid = a->rid;
                b.is_alt = a->is_alt;
                b.qb = is_rev? l_ms - (aln.qe + 1) : aln.qb;                                                                                                                                                                              
                b.qe = is_rev? l_ms - aln.qb : aln.qe + 1; 
                b.rb = is_rev? (l_pac<<1) - (rb + aln.te + 1) : rb + aln.tb;
                b.re = is_rev? (l_pac<<1) - (rb + aln.tb) : rb + aln.te + 1;
                b.score = aln.score;
                b.csub = aln.score2;
                b.secondary = -1;
                b.seedcov = (b.re - b.rb < b.qe - b.qb? b.re - b.rb : b.qe - b.qb) >> 1;
                //				printf("*** %d, [%lld,%lld], %d:%d, (%lld,%lld), (%lld,%lld) == (%lld,%lld)\n", aln.score, rb, re, is_rev, is_larger, a->rb, a->re, ma->a[0].rb, ma->a[0].re, b.rb, b.re);
                // kv_push(mem_alnreg_t, v=*ma, x=b); // make room for a new element
                if (ma->n == ma->m) {
                    ma->m = ma->m? ma->m<<1 : 2;
                    ma->a = (mem_alnreg_t*)CUDAKernelRealloc(d_buffer_ptr, ma->a, sizeof(mem_alnreg_t) * ma->m, 8);
                }
                ma->a[ma->n++] = b;
                // move b s.t. ma is sorted
                for (i = 0; i < ma->n - 1; ++i) // find the insertion point
                    if (ma->a[i].score < b.score) break;
                tmp = i;
                for (i = ma->n - 1; i > tmp; --i) ma->a[i] = ma->a[i-1];
                ma->a[i] = b;
            }
            ++n;
        }
        if (n) ma->n = mem_sort_dedup_patch(opt, 0, 0, 0, ma->n, ma->a, d_buffer_ptr);
        // if (rev) free(rev);
        // free(ref);
    }
    return n;
}


// __device__ static int mem_sam_pe(const mem_opt_t *opt, const bntseq_t *bns, const uint8_t *pac, const mem_pestat_t pes[4], uint64_t id, bseq1_t s[2], mem_alnreg_v a[2], void* d_buffer_ptr)
// {
// 	int n = 0, i, j, z[2], o, subo, n_sub, extra_flag = 1, n_pri[2], n_aa[2];
// 	kstring_t str;
// 	mem_aln_t h[2], g[2], aa[2][2];

// 	str.l = str.m = 0; str.s = 0;
// 	memset(h, 0, sizeof(mem_aln_t) * 2);
// 	memset(g, 0, sizeof(mem_aln_t) * 2);
// 	n_aa[0] = n_aa[1] = 0;
// 	if (!(opt->flag & MEM_F_NO_RESCUE)) { // then perform SW for the best alignment
// 		mem_alnreg_v b[2];
// 		b[0].n = 0; b[0].m = 0; b[0].a = 0;
// 		b[1].n = 0; b[1].m = 0; b[1].a = 0;
// 		for (i = 0; i < 2; ++i)
// 			for (j = 0; j < a[i].n; ++j)
// 				if (a[i].a[j].score >= a[i].a[0].score  - opt->pen_unpaired){
// 					// kv_push(mem_alnreg_t, v=b[i], x=a[i].a[j]);
// 					if (b[i].n == b[i].m) {
// 						b[i].m = b[i].m? b[i].m<<1 : 2;
// 						b[i].a = (mem_alnreg_t*)CUDAKernelRealloc(d_buffer_ptr, b[i].a, sizeof(mem_alnreg_t) * b[i].m, 8);
// 					}
// 					b[i].a[b[i].n++] = a[i].a[j];
// 				}
// 		for (i = 0; i < 2; ++i)
// 			for (j = 0; j < b[i].n && j < opt->max_matesw; ++j)
// 				n += mem_matesw(opt, bns, pac, pes, &b[i].a[j], s[!i].l_seq, (uint8_t*)s[!i].seq, &a[!i], d_buffer_ptr);
// 		// free(b[0].a); free(b[1].a);
// 	}
// 	n_pri[0] = mem_mark_primary_se_GPU(opt, a[0].n, a[0].a, id<<1|0, d_buffer_ptr);
// 	n_pri[1] = mem_mark_primary_se_GPU(opt, a[1].n, a[1].a, id<<1|1, d_buffer_ptr);
// 	if (opt->flag & MEM_F_PRIMARY5) {
// 		mem_reorder_primary5(opt->T, &a[0]);
// 		mem_reorder_primary5(opt->T, &a[1]);
// 	}
// 	if (opt->flag&MEM_F_NOPAIRING) goto no_pairing;
// 	// pairing single-end hits
// 	if (n_pri[0] && n_pri[1] && (o = mem_pair(opt, bns, pac, pes, s, a, id, &subo, &n_sub, z, n_pri, d_buffer_ptr)) > 0) {
// 		int is_multi[2], q_pe, score_un, q_se[2];
// 		char **XA[2];
// 		// check if an end has multiple hits even after mate-SW
// 		for (i = 0; i < 2; ++i) {
// 			for (j = 1; j < n_pri[i]; ++j)
// 				if (a[i].a[j].secondary < 0 && a[i].a[j].score >= opt->T) break;
// 			is_multi[i] = j < n_pri[i]? 1 : 0;
// 		}
// 		if (is_multi[0] || is_multi[1]) goto no_pairing; // TODO: in rare cases, the true hit may be long but with low score
// 		// compute mapQ for the best SE hit
// 		score_un = a[0].a[0].score + a[1].a[0].score - opt->pen_unpaired;
// 		//q_pe = o && subo < o? (int)(MEM_MAPQ_COEF * (1. - (double)subo / o) * log(a[0].a[z[0]].seedcov + a[1].a[z[1]].seedcov) + .499) : 0;
// 		subo = subo > score_un? subo : score_un;
// 		q_pe = raw_mapq(o - subo, opt->a);
// 		if (n_sub > 0) q_pe -= (int)(4.343 * log((double)n_sub+1) + .499);
// 		if (q_pe < 0) q_pe = 0;
// 		if (q_pe > 60) q_pe = 60;
// 		q_pe = (int)(q_pe * (1. - .5 * (a[0].a[0].frac_rep + a[1].a[0].frac_rep)) + .499);
// 		// the following assumes no split hits
// 		if (o > score_un) { // paired alignment is preferred
// 			mem_alnreg_t *c[2];
// 			c[0] = &a[0].a[z[0]]; c[1] = &a[1].a[z[1]];
// 			for (i = 0; i < 2; ++i) {
// 				if (c[i]->secondary >= 0)
// 					c[i]->sub = a[i].a[c[i]->secondary].score, c[i]->secondary = -2;
// 				q_se[i] = mem_approx_mapq_se(opt, c[i]);
// 			}
// 			q_se[0] = q_se[0] > q_pe? q_se[0] : q_pe < q_se[0] + 40? q_pe : q_se[0] + 40;
// 			q_se[1] = q_se[1] > q_pe? q_se[1] : q_pe < q_se[1] + 40? q_pe : q_se[1] + 40;
// 			extra_flag |= 2;
// 			// cap at the tandem repeat score
// 			q_se[0] = q_se[0] < raw_mapq(c[0]->score - c[0]->csub, opt->a)? q_se[0] : raw_mapq(c[0]->score - c[0]->csub, opt->a);
// 			q_se[1] = q_se[1] < raw_mapq(c[1]->score - c[1]->csub, opt->a)? q_se[1] : raw_mapq(c[1]->score - c[1]->csub, opt->a);
// 		} else { // the unpaired alignment is preferred
// 			z[0] = z[1] = 0;
// 			q_se[0] = mem_approx_mapq_se(opt, &a[0].a[0]);
// 			q_se[1] = mem_approx_mapq_se(opt, &a[1].a[0]);
// 		}
// 		for (i = 0; i < 2; ++i) {
// 			int k = a[i].a[z[i]].secondary_all;
// 			if (k >= 0 && k < n_pri[i]) { // switch secondary and primary if both of them are non-ALT
// 				// assert(a[i].a[k].secondary_all < 0);
// 				for (j = 0; j < a[i].n; ++j)
// 					if (a[i].a[j].secondary_all == k || j == k)
// 						a[i].a[j].secondary_all = z[i];
// 				a[i].a[z[i]].secondary_all = -1;
// 			}
// 		}
// 		if (!(opt->flag & MEM_F_ALL)) {
// 			for (i = 0; i < 2; ++i)
// 				XA[i] = mem_gen_alt(opt, bns, pac, &a[i], s[i].l_seq, s[i].seq, d_buffer_ptr);
// 		} else XA[0] = XA[1] = 0;
// 		// write SAM
// 		for (i = 0; i < 2; ++i) {
// 			h[i] = mem_reg2aln_GPU(opt, bns, pac, s[i].l_seq, s[i].seq, &a[i].a[z[i]], d_buffer_ptr);
// 			h[i].mapq = q_se[i];
// 			h[i].flag |= 0x40<<i | extra_flag;
// 			h[i].XA = XA[i]? XA[i][z[i]] : 0;
// 			aa[i][n_aa[i]++] = h[i];
// 			if (n_pri[i] < a[i].n) { // the read has ALT hits
// 				mem_alnreg_t *p = &a[i].a[n_pri[i]];
// 				if (p->score < opt->T || p->secondary >= 0 || !p->is_alt) continue;
// 				g[i] = mem_reg2aln_GPU(opt, bns, pac, s[i].l_seq, s[i].seq, p, d_buffer_ptr);
// 				g[i].flag |= 0x800 | 0x40<<i | extra_flag;
// 				g[i].XA = XA[i]? XA[i][n_pri[i]] : 0;
// 				aa[i][n_aa[i]++] = g[i];
// 			}
// 		}
// 		for (i = 0; i < n_aa[0]; ++i)
// 			mem_aln2sam(opt, bns, &str, &s[0], n_aa[0], aa[0], i, &h[1], d_buffer_ptr); // write read1 hits
// 		int l_sam = strlen_GPU(str.s); 		// length of output
// 		int offset = atomicAdd(&d_seq_sam_size, l_sam+1);	// offset to output to d_seq_sam_ptr
// 		memcpy(&d_seq_sam_ptr[offset], str.s, l_sam+1);	// copy sam to output
// 		s[0].sam = (char*)offset; 	// record offset
// 		str.l = 0;
// 		for (i = 0; i < n_aa[1]; ++i)
// 			mem_aln2sam(opt, bns, &str, &s[1], n_aa[1], aa[1], i, &h[0], d_buffer_ptr); // write read2 hits
// 		l_sam = strlen_GPU(str.s); 		// length of output
// 		offset = atomicAdd(&d_seq_sam_size, l_sam+1);	// offset to output to d_seq_sam_ptr
// 		memcpy(&d_seq_sam_ptr[offset], str.s, l_sam+1);	// copy sam to output
// 		s[1].sam = (char*)offset; 	// record offset

// 		// if (strcmp(s[0].name, s[1].name) != 0) err_fatal(__func__, "paired reads have different names: \"%s\", \"%s\"\n", s[0].name, s[1].name);
// // 		// free
// // 		for (i = 0; i < 2; ++i) {
// // 			free(h[i].cigar); free(g[i].cigar);
// // 			if (XA[i] == 0) continue;
// // 			for (j = 0; j < a[i].n; ++j) free(XA[i][j]);
// // 			free(XA[i]);
// // 		}
// 	} else goto no_pairing;
// 	return n;

// no_pairing:
// 	for (i = 0; i < 2; ++i) {
// 		int which = -1;
// 		if (a[i].n) {
// 			if (a[i].a[0].score >= opt->T) which = 0;
// 			else if (n_pri[i] < a[i].n && a[i].a[n_pri[i]].score >= opt->T)
// 				which = n_pri[i];
// 		}
// 		if (which >= 0) h[i] = mem_reg2aln_GPU(opt, bns, pac, s[i].l_seq, s[i].seq, &a[i].a[which], d_buffer_ptr);
// 		else h[i] = mem_reg2aln_GPU(opt, bns, pac, s[i].l_seq, s[i].seq, 0, d_buffer_ptr);
// 	}
// 	if (!(opt->flag & MEM_F_NOPAIRING) && h[0].rid == h[1].rid && h[0].rid >= 0) { // if the top hits from the two ends constitute a proper pair, flag it.
// 		int64_t dist;
// 		int d;
// 		d = mem_infer_dir(bns->l_pac, a[0].a[0].rb, a[1].a[0].rb, &dist);
// 		if (!pes[d].failed && dist >= pes[d].low && dist <= pes[d].high) extra_flag |= 2;
// 	}
// 	mem_reg2sam(opt, bns, pac, &s[0], &a[0], 0x41|extra_flag, &h[1], d_buffer_ptr);
// 	mem_reg2sam(opt, bns, pac, &s[1], &a[1], 0x81|extra_flag, &h[0], d_buffer_ptr);
// 	// if (strcmp(s[0].name, s[1].name) != 0) err_fatal(__func__, "paired reads have different names: \"%s\", \"%s\"\n", s[0].name, s[1].name);
// 	// free(h[0].cigar); free(h[1].cigar);
// 	return n;
// }

/* find the SMEM starting at each position of the read 
   for each position, only extend to the right
   each block process a read
 */
__global__ void MEMFINDING_collect_intv_kernel(
        const mem_opt_t *d_opt, 
        const bwt_t *d_bwt, 
        const uint8_t *d_seq,
        int *d_seq_offset,
        smem_aux_t *d_aux, 			// aux output
        kmers_bucket_t *d_kmerHashTab,
        void* d_buffer_pools)
{
    // seqID = blockIdx.x
    int j;	// position on read to process
    const uint8_t *seq1 = d_seq + d_seq_offset[blockIdx.x];
    int l_seq = d_seq_offset[blockIdx.x + 1] - d_seq_offset[blockIdx.x];
    smem_aux_t* a = &d_aux[blockIdx.x];	// aux output for this read
    int min_seed_len = d_opt->min_seed_len;
    if (l_seq < min_seed_len){ 	// if the query is shorter than the seed length, no match
        if (threadIdx.x==0){
            a->mem.n = a->mem.m = 0;
            a->mem.a = 0;
        }
        return;
    }


    // cache read in shared mem
    extern __shared__ int SM[];
    uint8_t *S_seq = (uint8_t*)SM;
#pragma unroll
    for (j=threadIdx.x; j<l_seq; j+=blockDim.x)
        S_seq[j] = (uint8_t)seq1[j];

    // allocate memory for SMEM intervals
    __shared__ bwtintv_t* S_mem_a[1];
    if (threadIdx.x == 0){
        void* d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, blockIdx.x % 32);
        S_mem_a[0] = (bwtintv_t*)CUDAKernelMalloc(d_buffer_ptr, (l_seq-min_seed_len+1)*sizeof(bwtintv_t), 8);
        a->mem.m = l_seq - min_seed_len + 1;
        a->mem.n = a->mem.m;
        a->mem.a = S_mem_a[0];
    }
    __syncthreads(); __syncwarp();
    bwtintv_t *mem_a = S_mem_a[0];

    // extend to the right and find the longest seed
    // positions higher than l_seq-min_seed_len would produce unqualified seds anyways
#pragma unroll
    for (j=threadIdx.x; j<=(l_seq-min_seed_len); j+=blockDim.x){
        bwt_smem_right(d_bwt, l_seq, S_seq, j, start_width, 0, min_seed_len, mem_a, d_kmerHashTab);
    }
}


// input: mem intervals 
// output: seeds from all intervals
// parallelism: each block processes a read.
// limit:       summing up all the num_seeds from each intv
//              then allocating a memory and computing offsets
//              is serialized.
__global__ void saLookup(
        const mem_opt_t *d_opt,
        const bwt_t *d_bwt,
        const bntseq_t *d_bns,
        const uint8_t *d_seq,
        int *d_seq_offset,
        smem_aux_t *d_aux,
        mem_seed_v *d_seq_seeds,	// output
        void *d_buffer_pools
        )
{
    int seqID = blockIdx.x;
    bwtintv_t *intvs = d_aux[seqID].mem.a;
    int num_intvs = d_aux[seqID].mem.n;

    __shared__ int s_offsets[MAX_LEN_READ + 1];
    __shared__ mem_seed_t *seed_a;
    __shared__ int *offsets;
    if(threadIdx.x == 0) {
        void *buf = CUDAKernelSelectPool(d_buffer_pools, blockIdx.x % 32);
        int num_seeds;
        int k;
        offsets = (int*)s_offsets;
        if(num_intvs > MAX_LEN_READ + 1){ // realloc for outliers.
            offsets = (int*)CUDAKernelMalloc(buf, sizeof(int) * (num_intvs + 1), 8);
        }

        num_seeds = 0;
        for(k = 0; k < num_intvs; k++) { 
            offsets[k] = num_seeds;
            num_seeds += intvs[k].x[2] > d_opt->max_occ ? d_opt->max_occ : intvs[k].x[2];
        }
        offsets[k] = num_seeds;

        seed_a = d_seq_seeds[seqID].a = (mem_seed_t*)CUDAKernelMalloc(buf, num_seeds * sizeof(mem_seed_t), 8);
        d_seq_seeds[seqID].n = num_seeds;
    }
    __syncthreads(); __syncwarp();

    // collect seeds from each intervals
    int intv_id;
    int intv_size, step_size;
    bwtintv_t *intv;
    int offset;
    for(intv_id = threadIdx.x; intv_id < num_intvs; intv_id += blockDim.x) {
        intv = &intvs[intv_id];
        offset = offsets[intv_id];
        intv_size = offsets[intv_id + 1] - offset;
        step_size = intv->x[2] > d_opt->max_occ ? intv->x[2] / d_opt->max_occ : 1;

        for(int j = 0; j < intv_size; j++) {
            mem_seed_t new_seed;
            bwtint_t k = intv->x[0] + step_size * j;
            if(k<0 || k>d_bwt->seq_len){
                continue;
            }
            new_seed.rbeg = bwt_sa_gpu(d_bwt, k);
            new_seed.qbeg = M(*intv);
            new_seed.len = new_seed.score = LEN(*intv);
            new_seed.rid = bns_intv2rid_gpu(d_bns, new_seed.rbeg, new_seed.rbeg + new_seed.len);
            seed_a[offset++] = new_seed;
        }
    }
}



/* for each read, sort seeds by rbeg
   use cub::blockRadixSort
 */
// process reads who have less seeds
__global__ void sortSeedsLowDim(
        mem_seed_v *d_seq_seeds,
        void *d_buffer_pools
        )
{
    // seqID = blockIdx.x
    int n_seeds = d_seq_seeds[blockIdx.x].n;
    if (n_seeds==0) return;
    if (n_seeds>SORTSEEDSLOW_MAX_NSEEDS) return;

    mem_seed_t *seed_arrA = d_seq_seeds[blockIdx.x].a;

    // Specialize BlockRadixSort
    typedef cub::BlockRadixSort<int64_t, SORTSEEDSLOW_BLOCKDIMX, SORTSEEDSLOW_NKEYS_THREAD, int> BlockRadixSort;
    // Allocate shared mem
    __shared__ typename BlockRadixSort::TempStorage temp_storage;

    // Block sort variables
    int64_t thread_keys[SORTSEEDSLOW_NKEYS_THREAD];
    int thread_values[SORTSEEDSLOW_NKEYS_THREAD];
    int old_pos, new_pos;

    __shared__ mem_seed_t* s_seed_arrB; // new bucket
    if (threadIdx.x==0){
        void *d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, blockIdx.x%32);
        s_seed_arrB = (mem_seed_t*)CUDAKernelMalloc(d_buffer_ptr, n_seeds*sizeof(mem_seed_t), 8);
    }
    __syncthreads(); __syncwarp();

    // (1/2) sort by (effectively qe) (len)
    for(int i=0; i<SORTSEEDSLOW_NKEYS_THREAD; i++){
        old_pos = threadIdx.x*SORTSEEDSLOW_NKEYS_THREAD+i;
        if(old_pos < n_seeds){
            thread_keys[i] = seed_arrA[old_pos].len;
            thread_values[i] = old_pos;	
        } else{ // pad with INT64_MAX
            thread_keys[i] = INT64_MAX;
            thread_values[i] = -1;
        }   
    }
    BlockRadixSort(temp_storage).Sort(thread_keys, thread_values); // it is stable

    for(int i=0; i<SORTSEEDSLOW_NKEYS_THREAD; i++){ //reorder 
        new_pos = threadIdx.x * SORTSEEDSLOW_NKEYS_THREAD + i;
        if(new_pos >= n_seeds) break;
        if(thread_values[i]==-1){
            printf("Error: sorting result incorrect. SeqID=%d\n", blockIdx.x);
            __trap();
        }
        s_seed_arrB[new_pos] = seed_arrA[thread_values[i]];
    }
    __syncthreads(); __syncwarp();
    __syncwarp();

    // (2/2) sort by qb
    for(int i=0; i<SORTSEEDSLOW_NKEYS_THREAD; i++){
        old_pos = threadIdx.x*SORTSEEDSLOW_NKEYS_THREAD+i;
        if(old_pos < n_seeds){
            thread_keys[i] = s_seed_arrB[old_pos].qbeg;
            thread_values[i] = old_pos;	
        } else{ // pad with INT64_MAX
            thread_keys[i] = INT64_MAX;
            thread_values[i] = -1;
        }   
    }
    BlockRadixSort(temp_storage).Sort(thread_keys, thread_values); // it is stable

    for(int i=0; i<SORTSEEDSLOW_NKEYS_THREAD; i++){ //reorder 
        new_pos = threadIdx.x * SORTSEEDSLOW_NKEYS_THREAD + i;
        if(new_pos >= n_seeds) break;
        if(thread_values[i]==-1){
            printf("Error: sorting result incorrect. SeqID=%d\n", blockIdx.x);
            __trap();
        }
        seed_arrA[new_pos] = s_seed_arrB[thread_values[i]];
    }
}


// process reads who have more seeds
__global__ void sortSeedsHighDim(
        mem_seed_v *d_seq_seeds,
        void *d_buffer_pools
        )
{
    // seqID = blockIdx.x
    int n_seeds = d_seq_seeds[blockIdx.x].n;
    if (n_seeds<=SORTSEEDSLOW_MAX_NSEEDS) return;

    mem_seed_t *seed_arrA = d_seq_seeds[blockIdx.x].a;

    // Specialize BlockRadixSort
    typedef cub::BlockRadixSort<int64_t, SORTSEEDSHIGH_BLOCKDIMX, SORTSEEDSHIGH_NKEYS_THREAD, int> BlockRadixSort;
    // Allocate shared mem
    __shared__ typename BlockRadixSort::TempStorage temp_storage;

    // Block sort variables
    int64_t thread_keys[SORTSEEDSHIGH_NKEYS_THREAD];
    int thread_values[SORTSEEDSHIGH_NKEYS_THREAD];
    int old_pos, new_pos;

    __shared__ mem_seed_t* s_seed_arrB; // new bucket
    if (threadIdx.x==0){
        void *d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, blockIdx.x%32);
        s_seed_arrB = (mem_seed_t*)CUDAKernelMalloc(d_buffer_ptr, n_seeds*sizeof(mem_seed_t), 8);
    }
    __syncthreads(); __syncwarp();
    __syncwarp();

    // (1/2) sort by qe
    for(int i=0; i<SORTSEEDSHIGH_NKEYS_THREAD; i++){
        old_pos = threadIdx.x*SORTSEEDSHIGH_NKEYS_THREAD+i;
        if(old_pos < n_seeds){
            thread_keys[i] = seed_arrA[old_pos].qbeg + seed_arrA[old_pos].len;
            thread_values[i] = old_pos;	
        } else{ // pad with INT64_MAX
            thread_keys[i] = INT64_MAX;
            thread_values[i] = -1;
        }   
    }
    BlockRadixSort(temp_storage).Sort(thread_keys, thread_values); // it is stable

    for(int i=0; i<SORTSEEDSHIGH_NKEYS_THREAD; i++){ //reorder 
        new_pos = threadIdx.x * SORTSEEDSHIGH_NKEYS_THREAD + i;
        if(new_pos >= n_seeds) break;
        if(thread_values[i]==-1){
            printf("Error: sorting result incorrect. SeqID=%d\n", blockIdx.x);
            __trap();
        }
        s_seed_arrB[new_pos] = seed_arrA[thread_values[i]];
    }
    __syncthreads(); __syncwarp();
    __syncwarp();

    // (2/2) sort by qb
    for(int i=0; i<SORTSEEDSHIGH_NKEYS_THREAD; i++){
        old_pos = threadIdx.x*SORTSEEDSHIGH_NKEYS_THREAD+i;
        if(old_pos < n_seeds){
            thread_keys[i] = s_seed_arrB[old_pos].qbeg;
            thread_values[i] = old_pos;	
        } else{ // pad with INT64_MAX
            thread_keys[i] = INT64_MAX;
            thread_values[i] = -1;
        }   
    }
    BlockRadixSort(temp_storage).Sort(thread_keys, thread_values); // it is stable

    for(int i=0; i<SORTSEEDSHIGH_NKEYS_THREAD; i++){ //reorder 
        new_pos = threadIdx.x * SORTSEEDSHIGH_NKEYS_THREAD + i;
        if(new_pos >= n_seeds) break;
        if(thread_values[i]==-1){
            printf("Error: sorting result incorrect. SeqID=%d\n", blockIdx.x);
            __trap();
        }
        seed_arrA[new_pos] = s_seed_arrB[thread_values[i]];
    }
}


/* find the smallest seed on seeds such that its rbeg>=rbeg_lower_bound*/
__device__ inline static int search_lower_bound_rbeg(mem_seed_t *seeds, int seedID, int64_t rbeg_lower_bound){
    int lower = 0;		// lower bound on binary search
    int upper = seedID;
    int mid = seedID/2;
    while (lower < upper-1){
        if (seeds[mid].rbeg < rbeg_lower_bound) lower = mid;
        else upper = mid;
        mid = (lower + upper)/2;
    }
    if (seeds[lower].rbeg >= rbeg_lower_bound) return lower;
    else return upper;
}
/* seed chaining by using the parallel nearest neighbor search algorithm 
   a block process all seeds of one read
Notations:
- preceding_seed[j] = i means that seed j is preceded by seed i on a chain (i<j)
- prededing_seed[j] = j means that seed j is the first seed on a chain
- preceding_seed[j] = -1 means seed is discarded
- suceeding_seed[i] = INT_MAX means that seed i has no suceeding seed
 */
__global__ void SEEDCHAINING_chain_kernel(
        const mem_opt_t *d_opt,
        const bntseq_t *d_bns,
        const uint8_t *d_seq,
        int *d_seq_offset,
        mem_seed_v *d_seq_seeds,
        mem_chain_v *d_chains,	// output
        void *d_buffer_pools
        )
{
    // seqID = blockIdx.x
    int n_seeds = d_seq_seeds[blockIdx.x].n;
    if (n_seeds==0){
        d_chains[blockIdx.x].n = 0;
        return;
    }
    mem_seed_t *seed_a = d_seq_seeds[blockIdx.x].a;	// seed array
    int seq_id = blockIdx.x;
    int seq_offset = d_seq_offset[seq_id];
    int seq_offset_next = d_seq_offset[seq_id + 1];
    int l_seq = seq_offset_next - seq_offset;

    __shared__ int16_t S_preceding_seed[SORTSEEDSHIGH_MAX_NSEEDS];
    __shared__ int S_suceeding_seed[SORTSEEDSHIGH_MAX_NSEEDS];
    if (threadIdx.x==0) S_preceding_seed[0] = 0;	// seed 0 always head of a chain
    for (int seedID=threadIdx.x; seedID<n_seeds&&seedID<SORTSEEDSHIGH_MAX_NSEEDS; seedID+=blockDim.x) S_suceeding_seed[seedID] = INT_MAX;	// initial: no chain yet

    // for each seed (except 0), find nearest preceding chainable seed
    int max_chain_gap = d_opt->max_chain_gap;
    int bandwidth_gap = d_opt->w;
    int64_t l_pac = d_bns->l_pac;
    for (int j=threadIdx.x+1; j<n_seeds&&j<SORTSEEDSHIGH_MAX_NSEEDS; j+=blockDim.x){
        if (seed_a[j].rid==-1){
            S_preceding_seed[j] = -1; continue;
        } else S_preceding_seed[j] = j;
        int64_t rbeg_j = seed_a[j].rbeg;
        int64_t rbeg_lower_bound = seed_a[j].rbeg - l_seq - max_chain_gap;
        int seedID_lower_bound = search_lower_bound_rbeg(seed_a, j, rbeg_lower_bound);
        for (int i=j-1; i>=seedID_lower_bound; i--){
            // test condition 1
            if (seed_a[i].rid != seed_a[j].rid) break;	// no need to test further
                                                        // test condition 2
            int64_t rbeg_i = seed_a[i].rbeg;
            if (rbeg_i<l_pac && rbeg_j>=l_pac) break; // no need to test further
                                                      // condition 4 is already satisfied by the lower bound
                                                      // test conditions 3, 5-7
            if (seed_a[j].qbeg >= seed_a[i].qbeg &&
                    seed_a[j].qbeg - seed_a[i].qbeg - seed_a[i].len < max_chain_gap &&
                    rbeg_j - rbeg_i - seed_a[i].len < max_chain_gap &&
                    (seed_a[j].qbeg-seed_a[i].qbeg) - (rbeg_j-rbeg_i) <= bandwidth_gap &&
                    (rbeg_j-rbeg_i) - (seed_a[j].qbeg-seed_a[i].qbeg) <= bandwidth_gap)
            {
                S_preceding_seed[j] = i;
                atomicMin(&S_suceeding_seed[i], j);
                S_suceeding_seed[i] = j;
                break;	// stop at the nearest preceding seed
            }
        }
    }
    __syncthreads(); __syncwarp();
    // check the pairs of suceeding-preceeding. if unmatch, make seed head of chain
    for (int j=threadIdx.x+1; j<n_seeds&&j<SORTSEEDSHIGH_MAX_NSEEDS; j+=blockDim.x){
        if (S_preceding_seed[j]==-1) continue;
        if (S_suceeding_seed[S_preceding_seed[j]] != j) // not match
            S_preceding_seed[j] = j;	// make seed j head of chain
    }

    __syncthreads(); __syncwarp();
    __syncwarp();
    if(threadIdx.x == 0){
        for(int k=0; k<n_seeds; k++){
            if(S_preceding_seed[k] == k){
                printf("chain head seed: %d %ld %d %d\n", blockIdx.x,\
                        seed_a[k].rbeg, seed_a[k].len, seed_a[k].qbeg);
            }
        }
    }

    // now create the chains based on the doubly linked-lists that we found
    __shared__ int S_n_chains[1];
    __shared__ mem_chain_t* S_chain_a[1];
    if (threadIdx.x==0) {
        void *d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, blockIdx.x%32);
        S_chain_a[0] = (mem_chain_t*)CUDAKernelMalloc(d_buffer_ptr, n_seeds*sizeof(mem_chain_t), 8);
        S_n_chains[0] = 0;
    }
    __syncthreads(); __syncwarp();
    void *d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, (blockIdx.x+threadIdx.x)%32);
    mem_chain_t *chain_a = S_chain_a[0];
    for (int i=threadIdx.x; i<n_seeds&&i<SORTSEEDSHIGH_MAX_NSEEDS; i+=blockDim.x){	// i = seedID
        if (S_preceding_seed[i] == i){	// seed i is head of chain
                                        // start a new chain
            int chainID = atomicAdd(&S_n_chains[0], 1);
            chain_a[chainID].pos = seed_a[i].rbeg;
            chain_a[chainID].rid = seed_a[i].rid;
            chain_a[chainID].is_alt = !!(d_bns->anns[seed_a[i].rid].is_alt);
            // initialize seed array on this chain
            int chain_m = 9;	// amount of pre-allocated memory for seeds array
            int chain_n = 1;	// number of seed in this new chain
            mem_seed_t *chain_seeds = (mem_seed_t*)CUDAKernelMalloc(d_buffer_ptr, chain_m*sizeof(mem_seed_t), 8);	// seeds array for this chain
            chain_seeds[0] = seed_a[i];	// first seed on chain
            int l = i;	// counting suceeding seeds
                        // add suceeding seeds
            while (S_suceeding_seed[l]<INT_MAX){
                l = S_suceeding_seed[l];
                if (chain_n==chain_m){	// need to expand memory allocation
                    chain_m = chain_m<<1;
                    chain_seeds = (mem_seed_t*)CUDAKernelRealloc(d_buffer_ptr, chain_seeds, chain_m*sizeof(mem_seed_t), 8);
                }
                chain_seeds[chain_n++] = seed_a[l];
            }
            chain_a[chainID].n = chain_n;
            chain_a[chainID].m = chain_m;
            chain_a[chainID].seeds = chain_seeds;
        }
    }
    __syncthreads(); __syncwarp();

    // write output
    if (threadIdx.x==0){
        d_chains[blockIdx.x].n = S_n_chains[0];
        d_chains[blockIdx.x].a = chain_a;
    }

    // if (threadIdx.x==0)printf("seqID=%d, seeds=%p\n", blockIdx.x, d_chains[blockIdx.x].a[0].seeds);

    //if (threadIdx.x==0)
    //for (int i=0; i<d_chains[blockIdx.x].n; i++)
    //for (int j=0; j<d_chains[blockIdx.x].a[i].n; j++)
    //printf("new: seqID=%d, chainID=%d, seedID=%d, qbeg=%d, len=%d rbeg=%ld\n", blockIdx.x, i, j, d_chains[blockIdx.x].a[i].seeds[j].qbeg, d_chains[blockIdx.x].a[i].seeds[j].len, d_chains[blockIdx.x].a[i].seeds[j].rbeg);

}


// Each CUDA thread computes all chains of each read sequence.
//
// from the sorted seeds of the read.
// 
//
__global__ void BTreeChaining(
        int batch_size,
        const mem_opt_t *d_opt,
        const bntseq_t *d_bns,
        const uint8_t *d_seq,
        int *d_seq_offset,
        mem_seed_v *d_seq_seeds,
        mem_chain_v *d_chains,	// output
        void *d_buffer_pools
        )
{
    int seqID = blockIdx.x * blockDim.x + threadIdx.x;
    if(seqID >= batch_size) return;
    int n_seeds = d_seq_seeds[seqID].n;
    if (n_seeds==0){
        d_chains[seqID].n = 0;
        return;
    }
    mem_seed_t *seed_a = d_seq_seeds[seqID].a;	// seed array
    //int l_seq = d_seqs[seqID].l_seq;

    kbtree_chn_t *tree;
    mem_chain_v chain;
    chain.n = 0; chain.m = 0, chain.a = 0;
    // if the query is shorter ~
    void* d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, blockIdx.x % 32);	// set buffer pool
    tree = kb_init_chn(512, d_buffer_ptr);

    for(int i = 0; i < n_seeds; i++){
        mem_seed_t s = seed_a[i];
        int start_new_chain = 0;
        mem_chain_t tmp, *lower, *upper;
        tmp.pos = s.rbeg;
        if(kb_size(tree)){
            kb_intervalp_chn(tree, &tmp, &lower, &upper); // find the closest chain

            if(!lower || !test_and_merge(d_opt, d_bns->l_pac, lower, &s, s.rid, d_buffer_ptr)){
                start_new_chain = 1;
            }
        } else{
            start_new_chain = 1;
        }

        if(start_new_chain){
            if(s.rid<0 || s.rid >= 455){ //FIXME hardcoded err handling
                //printf("sth wrong: s.rid = %d\n", s.rid);
                continue;
            }
            tmp.n = 1; tmp.m = SEEDS_PER_CHAIN;
            tmp.seeds = (mem_seed_t*)CUDAKernelCalloc(d_buffer_ptr, tmp.m, sizeof(mem_seed_t), 8);
            tmp.seeds[0] = s;
            tmp.rid = s.rid;
            tmp.is_alt = !!d_bns->anns[s.rid].is_alt;
            kb_putp_chn(tree, &tmp, d_buffer_ptr);
        }
    }

    chain.m = kb_size(tree);
    chain.a = (mem_chain_t*)CUDAKernelRealloc(d_buffer_ptr, chain.a, sizeof(mem_chain_t) * chain.m, 8);

    __kb_traverse(tree, &chain, d_buffer_ptr);
    // kb_destroy(chn, tree);

    // write output
    d_chains[seqID] = chain;
}



/* sort chains of each read by weight 
   shared-mem is pre-allocated to 3072*int
   assume that max(n_chn) is 3072
 */
__global__ void sortChainsDecreasingWeight(mem_chain_v* d_chains, void* d_buffer_pools){
    // if (blockIdx.x!=3921) return;
    // int seqID = blockIdx.x;
    int n_chn = d_chains[blockIdx.x].n;
    if (n_chn==0 || n_chn > 3072) return;
    void* d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, blockIdx.x%32);
    mem_chain_t* a = d_chains[blockIdx.x].a;	// array of chains

    extern __shared__ int SM[];			// shared mem, pre-allocated
    mem_chain_t** new_a_SM = (mem_chain_t**)SM;	// new array of chains on global mem
    uint16_t* w = (uint16_t*)&new_a_SM[1];		// array of weights
    uint16_t* new_i = (uint16_t*)&w[MAX_N_CHAIN]; // array of sorted chain index

    // calculate weight of each chain
    int n_iter = MAX_N_CHAIN/SORTCHAIN_BLOCKDIMX;
    for (int k=0; k<n_iter; k++){
        int i = k*blockDim.x + threadIdx.x;	// chainID to work on
        if (i<n_chn)
            w[i] = mem_chain_weight(&a[i]);
        else
            w[i] = 0;
    }
    __syncthreads(); __syncwarp();
    uint32_t thread_keys[NKEYS_EACH_THREAD];	// weight array on each thread
    int thread_values[NKEYS_EACH_THREAD];		// chain's index array before sorting
    for (int k=0; k<NKEYS_EACH_THREAD; k++){
        thread_values[k] = threadIdx.x*NKEYS_EACH_THREAD+k;
        thread_keys[k] = w[threadIdx.x*NKEYS_EACH_THREAD+k];
    }
    __syncthreads(); __syncwarp();
    // sort weights
    typedef cub::BlockRadixSort<uint32_t, SORTCHAIN_BLOCKDIMX, NKEYS_EACH_THREAD, int> BlockRadixSort;
    BlockRadixSort().SortDescending(thread_keys, thread_values);
    // transfer sorted index array (thread_values) to shared mem
    for (int k=0; k<NKEYS_EACH_THREAD; k++){
        new_i[threadIdx.x*NKEYS_EACH_THREAD+k] = thread_values[k];
    }

    // export output
    if (threadIdx.x==0){
        *new_a_SM = (mem_chain_t*)CUDAKernelMalloc(d_buffer_ptr, n_chn*sizeof(mem_chain_t), 8);
        d_chains[blockIdx.x].a = *new_a_SM;
    }
    __syncthreads(); __syncwarp();
    mem_chain_t* new_a = *new_a_SM;
    for (int k=0; k<n_iter; k++){
        int i = k*blockDim.x + threadIdx.x;	// chainID to work on
        if (i<n_chn){
            new_a[i] = a[new_i[i]];
            new_a[i].w = w[new_i[i]];
        }
    }
}


/* each block takes care of 1 read, do pairwise comparison of chains 
   max number of chain is MAX_N_CHAIN
Notations:
kept=0: definitely drop
kept=3: definitely keep
kept=1: not sure yet
 */
__global__ void CHAINFILTERING_filter_kernel(
        const mem_opt_t *opt, 
        mem_chain_v *d_chains, 	// input and output
        void* d_buffer_pools)
{
    int i, j, n_chn, n_iter;
    // int seqID = blockIdx.x;
    n_chn = d_chains[blockIdx.x].n;
    mem_chain_t* a = d_chains[blockIdx.x].a;	// chains vector
    if (n_chn == 0) return; // no need to filter
    if (threadIdx.x>=n_chn) return;	// don't run padded threads
    if (n_chn>MAX_N_CHAIN){
        //printf("ABORT n_chn(%d) > MAX_N_CHAIN(%d)\n", n_chn, MAX_N_CHAIN);
        return;
        //__trap();
    }

    extern __shared__ int SM[];		// dynamic shared mem
    uint16_t* chn_beg_SM = (uint16_t*)SM; 	// start of chains
    uint16_t* chn_end_SM = &chn_beg_SM[MAX_N_CHAIN];	// end of chains
    uint16_t* chn_w_SM = &chn_end_SM[MAX_N_CHAIN];		// weight of chains
    uint8_t* chn_info_SM = (uint8_t*)&chn_w_SM[MAX_N_CHAIN]; // chains' kept and alt information

    // load data in SM
    n_iter = ceil((float)n_chn/blockDim.x);
    for (int k=0; k<n_iter; k++){
        i = k*blockDim.x+threadIdx.x; // chainID to work on
        if (i<n_chn){
            chn_beg_SM[i] = chn_beg(a[i]);
            chn_end_SM[i] = chn_end(a[i]);
            chn_w_SM[i] = a[i].w;
            chn_info_SM[i] = 0;
            if (i!=0) SET_KEPT(i,1);	// kept = 1
            else SET_KEPT(i,3);			// chain 0 always kept
            SET_IS_ALT(i, a[i].is_alt);
        }
    }
    __syncthreads(); __syncwarp();

    // pairwise compare algorithm
    for (int k=0; k<n_iter; k++){	// each thread anchor on n_iter chains
        i = k*blockDim.x+threadIdx.x; // anchor chain
        while(GET_KEPT(i)==1) {
            for (j=0; j<i; j++){
                if (GET_KEPT(j)==0) continue; 	// chain already drop, don't compare with it
                                                // do comparisons
                int b_max = chn_beg_SM[j] > chn_beg_SM[i]? chn_beg_SM[j] : chn_beg_SM[i];
                int e_min = chn_end_SM[j] < chn_end_SM[i]? chn_end_SM[j] : chn_end_SM[i];
                if (e_min > b_max && (!GET_IS_ALT(i) || GET_IS_ALT(j))) { // have overlap; don't consider ovlp where the kept chain is ALT while the current chain is primary
                    int li = chn_end_SM[i] - chn_beg_SM[i];
                    int lj = chn_end_SM[j] - chn_beg_SM[j];
                    int min_l = li < lj? li : lj;
                    if (e_min - b_max >= min_l * opt->mask_level && min_l < opt->max_chain_gap) { // significant overlap
                                                                                                  // if (a[j].first < 0) a[j].first = i; // keep the first shadowed hit s.t. mapq can be more accurate
                        if (chn_w_SM[i]<chn_w_SM[j]*opt->drop_ratio && chn_w_SM[j]-chn_w_SM[i]>=opt->min_seed_len<<1){
                            if (GET_KEPT(j)==1) break;	// we don't know final decision yet, wait for next iteration
                            else{	// kept[i]=3, definitely drop chain i
                                SET_KEPT(i, 0);
                                break;
                            } 
                        }
                    }
                }
            }
            if (j==i)	// this means that chain i not significant overlap with any
                SET_KEPT(i, 3);
        } // keep looping until the kept outcome is certain
    }
    __syncthreads(); __syncwarp();

    // do accounting of which chain is kept 
    uint16_t* new_n_chn = chn_w_SM;		// chn_w_SM  now hold new n_chn
    uint16_t* old_index = chn_beg_SM;	// chn_beg_SM now hold index to the old chain
    mem_chain_t** new_a_SM = (mem_chain_t**)chn_end_SM;	// chn_end_SM now hold pointer to new_a
    if (threadIdx.x==0){
        new_n_chn[0] = 0;							
        for (j=0; j<n_chn; j++){
            if (GET_KEPT(j)==3){
                old_index[new_n_chn[0]++] = j;
            }
        }
        void* d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, blockIdx.x%32);
        *new_a_SM = (mem_chain_t*)CUDAKernelMalloc(d_buffer_ptr, new_n_chn[0]*sizeof(mem_chain_t), 8);
    }
    __syncthreads(); __syncwarp();
    // save to global data
    n_chn = new_n_chn[0];
    mem_chain_t* new_a = *new_a_SM;
    n_iter = ceil((float)n_chn/blockDim.x);
    for (int k=0; k<n_iter; k++){
        j = k*blockDim.x+threadIdx.x; // chainID to work on
        if (j<n_chn)
            new_a[j] = a[old_index[j]];
    }
    if (threadIdx.x==0){
        d_chains[blockIdx.x].n = n_chn;
        d_chains[blockIdx.x].a = new_a;
    }
}


__global__ void CHAINFILTERING_flt_chained_seeds_kernel(
        const mem_opt_t *d_opt, const bntseq_t *d_bns, const uint8_t *d_pac, const bseq1_t *d_seqs,
        mem_chain_v *d_chains, 	// input and output
        int n,		// number of seqs
        void* d_buffer_pools
        )
{
    // Gatekeeping.
    int i = blockIdx.x*blockDim.x + threadIdx.x;		// ID of the read to process
    if (i>=n) return;
    void* d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, threadIdx.x % 32);	// set buffer pool	
    mem_chain_t* a = d_chains[i].a;
    int n_chn = d_chains[i].n;
    uint8_t* query = (uint8_t*)d_seqs[i].seq;
    int l_query = d_seqs[i].l_seq;

    double min_l = d_opt->min_chain_weight? MEM_HSP_COEF * d_opt->min_chain_weight : MEM_MINSC_COEF * log((float)l_query);
    int j, k, min_HSP_score = (int)(d_opt->a * min_l + .499);
    if (min_l > MEM_SEEDSW_COEF * l_query) return; 

    for (i = 0; i < n_chn; ++i) {
        mem_chain_t *c = &a[i];
        for (j = k = 0; j < c->n; ++j) {
            mem_seed_t *s = &c->seeds[j];
            s->score = mem_seed_sw(d_opt, d_bns, d_pac, l_query, query, s, d_buffer_ptr);
            if (s->score < 0 || s->score >= min_HSP_score) {
                s->score = s->score < 0? s->len * d_opt->a : s->score;
                c->seeds[k++] = *s;
            }
        }
        c->n = k;
    }
}


/* preprocessing 1 for SW extension 
   count the number of seeds for each read and write to global records, allocate output regs vector
 */

// Each CUDA thread sums up the number of seeds in all chains
// of each read sequence. Same seed could be counted multiple times
// as it can be contained in multiple chains. These sums of seeds per
// read are atomically sumed up in *d_Nseeds.
//
// Then, concatenate all seeds in all chains in each read (= SWseeds)
// (including duplicates) into a preallocated 1D array in the 
// global memory region (d_seed_records).
//
// A mem_alnreg_v vector is allocated to contain all SWseed extensions 
// for each read.
__global__ void SWSeed(
        mem_chain_v *d_chains, 
        mem_alnreg_v *d_regs,
        seed_record_t *d_seed_records,
        int *d_Nseeds,	// total seed count across all reads
        int n_seqs,	// number of reads
        void* d_buffer_pools
        )
{
    int seqID = blockIdx.x*blockDim.x+threadIdx.x;	// ID of the read to process
    if (seqID>n_seqs) return;
    int chn_n = d_chains[seqID].n;					// n_chains of this read
    mem_chain_t* chn_a = d_chains[seqID].a;			// chain array of this read

    if (chn_n==0){
        d_regs[seqID].n = 0;
        return;
    }
    void* d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, threadIdx.x%32);

    // count number of seeds for this read
    int n_seeds = 0;
    for (int i=0; i<chn_n; i++)	// loop through chains
        n_seeds = n_seeds + chn_a[i].n;

    if (n_seeds==0){
        d_regs[seqID].n = 0;
        return;
    }

    // write seed record to global d_seed_records
    int start = atomicAdd(d_Nseeds, n_seeds);
    int j = 0;	// start+j will be the offset on d_seed_records, j is regID
    for (int i=0; i<chn_n; i++){	// i is chainID
        if (chn_a[i].n==0) continue;
        // start+j == batch-level
        for (int k=0; k<chn_a[i].n; k++){
            d_seed_records[start+j].seqID = seqID; //input idx
            d_seed_records[start+j].chainID = (uint16_t)i;// chain idx
            d_seed_records[start+j].seedID = (uint16_t)k; //chain-level
            d_seed_records[start+j].regID = (uint16_t)j; //input-level
            j++;
        }	
    }

    // allocate regs vector
    d_regs[seqID].n = d_regs[seqID].m = n_seeds;
    //printf("swseed %d %d\n", seqID, n_seeds);
    d_regs[seqID].a = (mem_alnreg_t*)CUDAKernelCalloc(d_buffer_ptr, n_seeds, sizeof(mem_alnreg_t), 8);
}

/* preprocessing 2 for SW extension: 
   each thread process 1 seed
   prepare target and query strings for SW extension
 */
// Each CUDA thread generates extension pairs for each SWseed,
// operating on the global d_seed_records array.
// 
// Each d_seed_records entry is appended with the sequence pairs
// for both left and right extension of the SWseed.
__global__ void ExtendingPairGenerate(
        const mem_opt_t *d_opt,
        bntseq_t *d_bns,
        uint8_t *d_pac,
        uint8_t *d_seq,
        int *d_seq_offset,
        mem_chain_v *d_chains, 
        mem_alnreg_v *d_regs,
        seed_record_t *d_seed_records,
        int *d_Nseeds,	// total seed count across all reads
        int n_seqs,	// number of reads
        void* d_buffer_pools
        )
{
    int i = blockIdx.x*blockDim.x+threadIdx.x;	// seed index on d_seed_records
    if (i>=d_Nseeds[0]) return;
    void* d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, threadIdx.x%32);
    // pull seed info
    int seqID = d_seed_records[i].seqID;
    int chainID = (int)d_seed_records[i].chainID;
    int seedID = (int)d_seed_records[i].seedID;
    int regID = (int)d_seed_records[i].regID;	// location of output on d_regs[seqID].a
                                                // pull seq info
    int seq_offset = d_seq_offset[seqID];
    int seq_offset_next = d_seq_offset[seqID + 1];
    int l_seq = seq_offset_next - seq_offset;
    uint8_t *seq = d_seq + seq_offset;
    // pull seed
    mem_chain_t *chain = &(d_chains[seqID].a[chainID]);
    mem_seed_t *seed = &(chain->seeds[seedID]);

    // get the max possible span of this chain on ref
    int64_t l_pac = d_bns->l_pac, rmax[2], max = 0;
    rmax[0] = l_pac<<1; rmax[1] = 0;
    for (int k = 0; k < chain->n; ++k) {	// sweeping through seeds on this chain
        int64_t b, e;
        int64_t rbeg = chain->seeds[k].rbeg;
        int32_t qbeg = chain->seeds[k].qbeg;
        int32_t len = chain->seeds[k].len;
        b = rbeg - (qbeg + cal_max_gap(d_opt, qbeg));
        e = rbeg + len + ((l_seq - qbeg - len) + cal_max_gap(d_opt, l_seq - qbeg - len));
        rmax[0] = rmax[0] < b? rmax[0] : b;
        rmax[1] = rmax[1] > e? rmax[1] : e;
        if (len > max) max = len;
    }
    rmax[0] = rmax[0] > 0? rmax[0] : 0;
    rmax[1] = rmax[1] < l_pac<<1? rmax[1] : l_pac<<1;
    if (rmax[0] < l_pac && l_pac < rmax[1]) { // crossing the forward-reverse boundary; then choose one side
        if (chain->seeds[0].rbeg < l_pac) rmax[1] = l_pac; // this works because all seeds are guaranteed to be on the same strand
        else rmax[0] = l_pac;
    }
    // retrieve the reference sequence
    int rid;
    uint8_t *rseq = bns_fetch_seq_gpu(d_bns, d_pac, &rmax[0], chain->seeds[0].rbeg, &rmax[1], &rid, d_buffer_ptr);
    d_regs[seqID].a[regID].rid = rid; 	// write rid to regs output
                                        // create read and ref strings for SW
    int64_t rbeg = seed->rbeg;
    int32_t qbeg = seed->qbeg;
    int32_t slen = seed->len;
    uint8_t *qs, *rs;
    int qlen, rlen;
    // left extension
    qlen = qbeg;
    //printf("[%s][bid %4d][tid %4d] qlen left = %d\n", __func__, blockIdx.x, threadIdx.x, qlen);
    rlen = rbeg - rmax[0];
    qs = (qlen>0)? (uint8_t*)CUDAKernelMalloc(d_buffer_ptr, qlen, 1) : 0;
    rs = (qlen>0)? (uint8_t*)CUDAKernelMalloc(d_buffer_ptr, rlen, 1) : 0;
    if (qlen>0) {for (int r=0; r<qlen; ++r) qs[r] = seq[qlen-1-r];}
    if (qlen>0) {for (int r=0; r<rlen; ++r) rs[r] = rseq[rlen-1-r];}
    // write this to seed records
    d_seed_records[i].read_left = qs;
    d_seed_records[i].readlen_left = (uint16_t)qlen;
    d_seed_records[i].ref_left = rs;
    d_seed_records[i].reflen_left = (uint16_t)rlen;
    // right extension
    qlen = l_seq - qbeg - slen;
    //printf("[%s][bid %4d][tid %4d] qlen right = %d\n", __func__, blockIdx.x, threadIdx.x, qlen);
    rlen = rmax[1] - rbeg - slen;
    qs = seq + qbeg	+ slen;
    rs = rseq + rbeg-rmax[0] + slen ;
    // write this to seed records
    d_seed_records[i].read_right = (qlen>0)? qs : 0;
    d_seed_records[i].readlen_right = (uint16_t)qlen;
    d_seed_records[i].ref_right = (qlen>0)? rs: 0;
    d_seed_records[i].reflen_right = (uint16_t)rlen;
    //printf("[%s] seed record #%6d. SW input pair lengths: left=(%6d, %6d), right=(%6d, %6d)\n", \
    __func__, i, d_seed_records[i].readlen_left, d_seed_records[i].reflen_left, \
        d_seed_records[i].readlen_right, d_seed_records[i].reflen_right);
}

/* SW extension
REQUIREMENT: BLOCKSIZE = WARPSIZE = 32
Each block perform 2 SW extensions on 1 seed
 */
__global__ void localExtending_baseline(
        const mem_opt_t *d_opt,
        mem_chain_v *d_chains, 
        seed_record_t *d_seed_records,
        mem_alnreg_v* d_regs,		// output array
        int *d_Nseeds
        )
{
    if (blockDim.x!=32) {printf("wrong blocksize config \n"); __trap();}
    int i = blockIdx.x;	// seed index on d_seed_records
    if (i>=d_Nseeds[0]) return;
    // pull seed info
    int seqID = (int)d_seed_records[i].seqID;
    int chainID = (int)d_seed_records[i].chainID;
    int seedID = (int)d_seed_records[i].seedID;
    int regID = (int)d_seed_records[i].regID;	// location of output on d_regs[seqID].a
                                                // prepare output registers
    int score;		// local extension score
    int gscore; 	// to-end extension score
    int qle, tle; 	// length of query and target after extension
    int gtle;		// length of the target if query is aligned to the end
    int query_end;	// endpoint on query after extension
    int64_t ref_end;// endpoint on ref after extension

    // left extension
    int h0 = d_chains[seqID].a[chainID].seeds[seedID].len * d_opt->a; 	// initial score = seedlength*a
    uint8_t *query = d_seed_records[i].read_left;
    int qlen = (int)d_seed_records[i].readlen_left;
    uint8_t *target = d_seed_records[i].ref_left;
    int tlen = (int)d_seed_records[i].reflen_left;
    if (qlen>0){
        score = ksw_extend_warp(qlen, query, tlen, target, 5, d_opt->mat, d_opt->o_del, d_opt->e_del, d_opt->o_ins, d_opt->e_ins, h0, &qle, &tle, &gtle, &gscore);
        // check whether we prefer to reach the end of the query
        if (gscore<=0 || gscore<=(score-d_opt->pen_clip5)){	// use local extension
            query_end = d_chains[seqID].a[chainID].seeds[seedID].qbeg - qle;
            ref_end   = d_chains[seqID].a[chainID].seeds[seedID].rbeg - tle;
        } else { // use to-end extension
            query_end = 0;
            ref_end   = d_chains[seqID].a[chainID].seeds[seedID].rbeg - gtle;
        }
    } else {score = h0; query_end = 0; ref_end = d_chains[seqID].a[chainID].seeds[seedID].rbeg; }
    // write output to global mem
    if (threadIdx.x==0){
        d_regs[seqID].a[regID].score = score;
        d_regs[seqID].a[regID].qb = query_end;
        d_regs[seqID].a[regID].rb = ref_end;
    }

    // right extension
    h0 = score;
    query = d_seed_records[i].read_right;
    qlen = (int)d_seed_records[i].readlen_right;
    target = d_seed_records[i].ref_right;
    tlen = (int)d_seed_records[i].reflen_right;
    if (qlen>0){
        score = ksw_extend_warp(qlen, query, tlen, target, 5, d_opt->mat, d_opt->o_del, d_opt->e_del, d_opt->o_ins, d_opt->e_ins, h0, &qle, &tle, &gtle, &gscore);
        // check whether we prefer to reach the end of the query
        if (gscore<=0 || gscore<=(score-d_opt->pen_clip3)){	// use local extension
            query_end = d_chains[seqID].a[chainID].seeds[seedID].qbeg + d_chains[seqID].a[chainID].seeds[seedID].len + qle;
            ref_end   = d_chains[seqID].a[chainID].seeds[seedID].rbeg + d_chains[seqID].a[chainID].seeds[seedID].len + tle;
        } else { // use to-end extension
            query_end = d_chains[seqID].a[chainID].seeds[seedID].qbeg + d_chains[seqID].a[chainID].seeds[seedID].len + qlen;
            ref_end   = d_chains[seqID].a[chainID].seeds[seedID].rbeg + d_chains[seqID].a[chainID].seeds[seedID].len + gtle;
        }
    } else {
        score = h0; 
        query_end = d_chains[seqID].a[chainID].seeds[seedID].qbeg + d_chains[seqID].a[chainID].seeds[seedID].len; 
        ref_end = d_chains[seqID].a[chainID].seeds[seedID].rbeg + d_chains[seqID].a[chainID].seeds[seedID].len; 
    }

    // compute seed coverage
    int nseeds = d_chains[seqID].a[chainID].n; // number of seeds in chain 
    int seedcov = 0;
    for (int s=0; s<nseeds; s++){
        mem_seed_t *t = &(d_chains[seqID].a[chainID].seeds[s]);
        if (t->qbeg >= d_regs[seqID].a[regID].qb && t->qbeg + t->len <= d_regs[seqID].a[regID].qe && t->rbeg >= d_regs[seqID].a[regID].rb && t->rbeg + t->len <= d_regs[seqID].a[regID].re) // seed fully contained
            seedcov += t->len;
    }
    // write output to global mem
    if (threadIdx.x==0){
        d_regs[seqID].a[regID].score = score;
        d_regs[seqID].a[regID].qe = query_end;
        d_regs[seqID].a[regID].re = ref_end;
        d_regs[seqID].a[regID].w = 0;	// indicate that we didn't use bandwidth
        d_regs[seqID].a[regID].seedlen0 = d_chains[seqID].a[chainID].seeds[seedID].len;
        d_regs[seqID].a[regID].frac_rep = d_chains[seqID].a[chainID].frac_rep;
        d_regs[seqID].a[regID].seedcov = seedcov;
    }
}

/* SW extension
REQUIREMENT: BLOCKSIZE = WARPSIZE = 32
Each block perform 2 SW extensions on 1 seed
 */
__global__ void localExtending(
        const mem_opt_t *d_opt,
        mem_chain_v *d_chains, 
        seed_record_t *d_seed_records,
        mem_alnreg_v* d_regs,		// output array
        int *d_Nseeds
        )
{
    if (blockDim.x!=32) {printf("wrong blocksize config \n"); __trap();}
    int i = blockIdx.x;	// seed index on d_seed_records
    if (i>=d_Nseeds[0]) return;
    // pull seed info
    int seqID = (int)d_seed_records[i].seqID;
    int chainID = (int)d_seed_records[i].chainID;
    int seedID = (int)d_seed_records[i].seedID;
    int regID = (int)d_seed_records[i].regID;	// location of output on d_regs[seqID].a
                                                // prepare output registers
    int score;		// local extension score
    int gscore; 	// to-end extension score
    int qle, tle; 	// length of query and target after extension
    int gtle;		// length of the target if query is aligned to the end
    int query_end;	// endpoint on query after extension
    int64_t ref_end;// endpoint on ref after extension

    int w, end_bonus;
    w = d_opt->w;
    end_bonus = d_opt->pen_clip5;

    // left extension
    int h0 = d_chains[seqID].a[chainID].seeds[seedID].len * d_opt->a; 	// initial score = seedlength*a
    uint8_t *query = d_seed_records[i].read_left;
    int qlen = (int)d_seed_records[i].readlen_left;
    uint8_t *target = d_seed_records[i].ref_left;
    int tlen = (int)d_seed_records[i].reflen_left;
    if (qlen>0){
        score = ksw_extend_warp2(qlen, query, tlen, target, 5, d_opt->mat, d_opt->o_del, d_opt->e_del, d_opt->o_ins, d_opt->e_ins, w, end_bonus, h0, &qle, &tle, &gtle, &gscore);
        // check whether we prefer to reach the end of the query
        if (gscore<=0 || gscore<=(score-d_opt->pen_clip5)){	// use local extension
            query_end = d_chains[seqID].a[chainID].seeds[seedID].qbeg - qle;
            ref_end   = d_chains[seqID].a[chainID].seeds[seedID].rbeg - tle;
        } else { // use to-end extension
            query_end = 0;
            ref_end   = d_chains[seqID].a[chainID].seeds[seedID].rbeg - gtle;
        }
    } else {score = h0; query_end = 0; ref_end = d_chains[seqID].a[chainID].seeds[seedID].rbeg; }
    // write output to global mem
    if (threadIdx.x==0){
        d_regs[seqID].a[regID].score = score;
        d_regs[seqID].a[regID].qb = query_end;
        d_regs[seqID].a[regID].rb = ref_end;
    }

    // right extension
    h0 = score;
    query = d_seed_records[i].read_right;
    qlen = (int)d_seed_records[i].readlen_right;
    target = d_seed_records[i].ref_right;
    tlen = (int)d_seed_records[i].reflen_right;
    if (qlen>0){
        end_bonus = d_opt->pen_clip3;
        score = ksw_extend_warp2(qlen, query, tlen, target, 5, d_opt->mat, d_opt->o_del, d_opt->e_del, d_opt->o_ins, d_opt->e_ins, w, end_bonus, h0, &qle, &tle, &gtle, &gscore);
        // check whether we prefer to reach the end of the query
        if (gscore<=0 || gscore<=(score-d_opt->pen_clip3)){	// use local extension
            query_end = d_chains[seqID].a[chainID].seeds[seedID].qbeg + d_chains[seqID].a[chainID].seeds[seedID].len + qle;
            ref_end   = d_chains[seqID].a[chainID].seeds[seedID].rbeg + d_chains[seqID].a[chainID].seeds[seedID].len + tle;
        } else { // use to-end extension
            query_end = d_chains[seqID].a[chainID].seeds[seedID].qbeg + d_chains[seqID].a[chainID].seeds[seedID].len + qlen;
            ref_end   = d_chains[seqID].a[chainID].seeds[seedID].rbeg + d_chains[seqID].a[chainID].seeds[seedID].len + gtle;
        }
    } else {
        score = h0; 
        query_end = d_chains[seqID].a[chainID].seeds[seedID].qbeg + d_chains[seqID].a[chainID].seeds[seedID].len; 
        ref_end = d_chains[seqID].a[chainID].seeds[seedID].rbeg + d_chains[seqID].a[chainID].seeds[seedID].len; 
    }

    // compute seed coverage
    int nseeds = d_chains[seqID].a[chainID].n; // number of seeds in chain 
    int seedcov = 0;
    for (int s=0; s<nseeds; s++){
        mem_seed_t *t = &(d_chains[seqID].a[chainID].seeds[s]);
        if (t->qbeg >= d_regs[seqID].a[regID].qb && t->qbeg + t->len <= d_regs[seqID].a[regID].qe && t->rbeg >= d_regs[seqID].a[regID].rb && t->rbeg + t->len <= d_regs[seqID].a[regID].re) // seed fully contained
            seedcov += t->len;
    }
    // write output to global mem
    if (threadIdx.x==0){
        d_regs[seqID].a[regID].score = score;
        d_regs[seqID].a[regID].qe = query_end;
        d_regs[seqID].a[regID].re = ref_end;
        d_regs[seqID].a[regID].w = 0;	// indicate that we didn't use bandwidth
        d_regs[seqID].a[regID].seedlen0 = d_chains[seqID].a[chainID].seeds[seedID].len;
        d_regs[seqID].a[regID].frac_rep = d_chains[seqID].a[chainID].frac_rep;
        d_regs[seqID].a[regID].seedcov = seedcov;
    }
}

/* post-processing SW kernel:
   - filter out reference-overlapped alignments 
   - also discard alignments whose score < opt->T
   - compute seedcov
   - check if alignment is alt
   gridDim = n_seqs
 */
/* pairwise compare alignments
   - mark primary/secondary alignments
   - alignment is primary if:
   + it has no query-overlap with other alignments
   + it is not alt and has no query-overlap with non-alt alignment
   | it is not alt | it is alt
   no q-overlap or higher-score		|	primary 	| 	primary
   q-overlap-lowerscore with non-alt 	|	secondary	|	secondary
   q-overlap-lowerscore with alt 		|	primary		|	secondary

   - mark whether alignments will be written using mem_alnreg_t.w
   - alignments will not be written if:
   + it is secondary and is alt
   + it is secondary and MEM_F_ALL flag is not up
   + it is secondary and its score < its primary's score*opt->drop_ratio
   - reorder reg_v, bring written aln to front and modify n
 */
__global__ void filterRegions(
        const mem_opt_t *d_opt,
        const bntseq_t *d_bns,
        mem_chain_v *d_chains, 		// input chains
        mem_alnreg_v* d_regs,		// output array
        void* d_buffer_pools
        )
{
    // seqID = blockIdx.x
    int n = d_regs[blockIdx.x].n;	// n alignments
    mem_alnreg_t *a = d_regs[blockIdx.x].a;

    // filter out reference-overlapped alignments
    __shared__ char S_kept_aln[MAX_N_ALN];	// array to keep track of which alignment to keep
    int n_iter = ceil((float)n/blockDim.x);
    for (int iter=0; iter<n_iter; iter++){
        int i = iter*blockDim.x + threadIdx.x; // anchor point
        if (i>=n) break;
        if (a[i].score < d_opt->T) S_kept_aln[i] = 0;
        else S_kept_aln[i] = 1;
    }
    __syncthreads(); __syncwarp();
    for (int iter=0; iter<n_iter; iter++){
        int i = iter*blockDim.x + threadIdx.x; // anchor point
        if (i>n) break;
        if (S_kept_aln[i]==0) continue;	// alignment discarded due to low score
        for (int j=i+1; j<n; j++){ // j is reference point
            if (S_kept_aln[j]==0) continue;	// don't compare with discarded alignment
            if (a[i].rid != a[j].rid) continue;	// no overlap if not on same strain
            int64_t ai_rb, ai_re, aj_rb, aj_re;
            ai_rb = a[i].rb; ai_re = a[i].re;
            aj_rb = a[j].rb; aj_re = a[j].re;
            if (ai_rb<aj_rb && ai_re<aj_rb) continue;	// no overlap
            if (aj_rb<ai_rb && aj_re<ai_rb) continue;	// no overlap
                                                        // compare anchor and reference
            int _or, _oq, _mr, _mq;	// overlap length on ref, on read, min ref len, min read len
            int rli, rlj;				// reference length of alignment i and j
            _or = (int)(ai_rb<aj_rb? ai_re-aj_rb : aj_re-ai_rb);
            _oq = a[i].qb<a[j].qb? a[i].qe-a[j].qb : a[j].qe - a[i].qb;
            rli = (int)(ai_re - ai_rb); rlj = (int)(aj_re - aj_rb);
            _mr = rli<rlj ? rli : rlj;
            _mq = (a[i].qe-a[i].qb < a[j].qe-a[j].qb)? a[i].qe-a[i].qb : a[j].qe-a[j].qb;
            if (_or>d_opt->mask_level_redun*_mr && _oq>d_opt->mask_level_redun*_mq)	{// large overlap, discard one with lower score
                if (a[i].score < a[j].score) {S_kept_aln[i] = 0; break;} 
                else S_kept_aln[j] = 0;
            }
        }
    }

    // remove discarded alignments
    int m = 0;
    if (threadIdx.x==0){
        for (int i=0; i<n; i++){
            if (S_kept_aln[i]){
                if (i!=m)
                    d_regs[blockIdx.x].a[m] = d_regs[blockIdx.x].a[i];
                m++;
            }
        }
        d_regs[blockIdx.x].n = m;
    }
    __syncthreads(); __syncwarp();
    // check is_alt
    for (int iter=0; iter<n_iter; iter++){
        int i = iter*blockDim.x + threadIdx.x; // anchor point
        if (i>m) break;
        mem_alnreg_t *p = &a[i];
        if (p->rid >= 0 && d_bns->anns[p->rid].is_alt)
            p->is_alt = 1;
    }
    __syncthreads(); __syncwarp();
    { // primary marking TODO
        // seqID = blockIdx.x
        int n = d_regs[blockIdx.x].n;	// n alignments
        if (n==0) return;
        mem_alnreg_t *a = d_regs[blockIdx.x].a;

        // mark primary/secondary 	//TODO : test shared mem cache for alignment endpoints	// TODO: implement sub-score
        __shared__ int16_t S_secondary[MAX_N_ALN];	// array to keep track of primary/secondary. -1=primary, otherwise indicate the primary alignment shadowing this alignment
        int n_iter = ceil((float)n/blockDim.x);
        for (int iter=0; iter<n_iter; iter++){
            int i = iter*blockDim.x + threadIdx.x; // anchor point
            if (i>n) break;
            S_secondary[i] = -1;		// at start, all are primary
        }
        __syncthreads();
        for (int iter=0; iter<n_iter; iter++){
            int i = iter*blockDim.x + threadIdx.x; // anchor point
            if (i>=n) break;
            for (int j=i+1; j<n; j++){	// reference point
                if (S_secondary[i]>=0) continue;	// don't anchor on secondary alignments
                if (S_secondary[j]>=0) continue;	// don't compare with secondary alignments
                int ai_qb, ai_qe, aj_qb, aj_qe, qb_max, qe_min;
                ai_qb = a[i].qb; ai_qe = a[i].qe;
                aj_qb = a[j].qb; aj_qe = a[j].qe;
                qb_max = ai_qb>aj_qb ? ai_qb : aj_qb;
                qe_min = ai_qe<aj_qe ? ai_qe : aj_qe;
                if (qe_min > qb_max){	// have overlap
                    int min_l = ai_qe - ai_qb < aj_qe - aj_qb ? ai_qe - ai_qb : aj_qe - aj_qb;	// smaller length
                    if (qe_min - qb_max >= min_l * d_opt->mask_level){ // found significant overlap
                        if (a[i].score > a[j].score){
                            if (a[i].is_alt && a[j].is_alt) S_secondary[j] = i;	// mark j secondary if they are both alt
                            else if (!(a[i].is_alt)) S_secondary[j] = i;		// mark j secondary if i is not alt
                        } else {
                            if (a[j].is_alt && a[i].is_alt) S_secondary[i] = j;	// mark i secondary if they are both alt
                            else if (!(a[j].is_alt)) S_secondary[i] = j;		// mark i secondary if j is not alt
                        }
                    }
                }
            }
        }

        // discard secondary alignments whose score too low or is alt
        __shared__ int new_n[1];		// new n of alignments
        __shared__ mem_alnreg_t* new_a[1];	// new array of alignments
        if (threadIdx.x==0) {
            void* d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, blockIdx.x%32);
            new_a[0] = (mem_alnreg_t*)CUDAKernelMalloc(d_buffer_ptr, n*sizeof(mem_alnreg_t), 8);
            new_n[0] = 0;
        }
        __syncthreads();
        for (int iter=0; iter<n_iter; iter++){
            int i = iter*blockDim.x + threadIdx.x; // anchor point
            if (i>=n) break;
            if (S_secondary[i]>=0 && (a[i].is_alt || !(d_opt->flag&MEM_F_ALL))) continue;	// discard
            if (S_secondary[i]>=0 && a[i].score<a[S_secondary[i]].score*d_opt->drop_ratio) continue; // discard
                                                                                                     // write kept alignment
            int k = atomicAdd(&new_n[0], 1);
            new_a[0][k] = a[i]; 
            new_a[0][k].secondary = (int)S_secondary[i];
        }
        __syncthreads();
        if (threadIdx.x==0){
            d_regs[blockIdx.x].a = new_a[0];
            d_regs[blockIdx.x].n = new_n[0];
        }
    }
}


/*
   reorder alignments: sorting by increasing is_alt, then decreasing score
   run at thread level: each thread process all aligments of a read
 */
__global__ void sortRegions(
        mem_alnreg_v *d_regs,
        int n_seqs,
        void *d_buffer_pools
        )
{
    int seqID = blockIdx.x*blockDim.x+threadIdx.x;
    if (seqID>=n_seqs) return;
    int n_alns = d_regs[seqID].n;
    mem_alnreg_t *a = d_regs[seqID].a;
    //printf("[%s] read #%d has %d alns after the local SW and filtering\n", __func__, seqID, n_alns);

    if (n_alns==1) return;
    else if (n_alns==2){
        if (a[0].is_alt > a[1].is_alt){	// swap so that non-alt alignment is first
            void *d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, threadIdx.x%32);
            mem_alnreg_t *new_a = (mem_alnreg_t*)CUDAKernelMalloc(d_buffer_ptr, 2*sizeof(mem_alnreg_t), 8);
            memcpy(&new_a[0], &a[1], sizeof(mem_alnreg_t));
            memcpy(&new_a[1], &a[0], sizeof(mem_alnreg_t));
            // save new array
            d_regs[seqID].a = new_a;
        } else if (a[0].score < a[1].score){	// swap so that higher score is first
            void *d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, threadIdx.x%32);
            mem_alnreg_t *new_a = (mem_alnreg_t*)CUDAKernelMalloc(d_buffer_ptr, 2*sizeof(mem_alnreg_t), 8);
            memcpy(&new_a[0], &a[1], sizeof(mem_alnreg_t));
            memcpy(&new_a[1], &a[0], sizeof(mem_alnreg_t));
            // save new array
            d_regs[seqID].a = new_a;
        }
        return;
    } else {
        __shared__ mem_alnreg_t S_tmp[1];	// to avoid using more registers
                                            // perform bubble sort, probably not too bad. I don't want to use other sorts to avoid recursion in GPU
        bool swapped;
        for (int i=0; i<n_alns-1; i++){
            swapped = false;
            for (int j=0; j<n_alns-1-i; j++){
                // swap if smaller is_alt is later
                if (a[j].is_alt > a[j+1].is_alt){
                    memcpy(S_tmp, &a[j], sizeof(mem_alnreg_t));
                    memcpy(&a[j], &a[j+1], sizeof(mem_alnreg_t));
                    memcpy(&a[j+1], S_tmp, sizeof(mem_alnreg_t));
                    swapped = true;
                    // swap if higher score is later
                } else if (a[j].score < a[j+1].score){
                    memcpy(S_tmp, &a[j], sizeof(mem_alnreg_t));
                    memcpy(&a[j], &a[j+1], sizeof(mem_alnreg_t));
                    memcpy(&a[j+1], S_tmp, sizeof(mem_alnreg_t));
                    swapped = true;
                }
            }
            if (swapped==false) break;	// stop early if no swap was found
        }
    }
}

/* prepare ref sequence for global SW
   allocate mem_aln_v array for each read
   write to d_seed_records just like SW extension,
   - seqID
   - regID: index on d_regs and d_alns
   if read has no good alignment, write an unmapped record:
   - rid = -1
   - pos = -1
   - flag = 0x4
 */
__global__ void FINALIZEALN_preprocessing1_kernel(
        int batch_size,
        mem_alnreg_v* d_regs,
        mem_aln_v * d_alns,
        seed_record_t *d_seed_records,
        int *d_Nseeds,
        void* d_buffer_pools)
{
    int seqID = blockIdx.x*blockDim.x + threadIdx.x;
    if(seqID >= batch_size) return;
    // allocate mem_aln_t array
    int n_aln = d_regs[seqID].n;

    // first create record on d_alns
    void *d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, threadIdx.x%32);
    d_alns[seqID].n = n_aln;
    // legit records if n_aln>0
    if (n_aln>0) d_alns[seqID].a = (mem_aln_t*)CUDAKernelCalloc(d_buffer_ptr, n_aln, sizeof(mem_aln_t), 8);
    // create unmapped records otherwise
    else {
        d_alns[seqID].a = (mem_aln_t*)CUDAKernelCalloc(d_buffer_ptr, 1, sizeof(mem_aln_t), 8);
        d_alns[seqID].a[0].rid = -1;
        d_alns[seqID].a[0].pos = -1;
        d_alns[seqID].a[0].flag = 0x4;
    }
    // atomic add n_seeds at block level
    __shared__ int S_block_nseeds[1];	// total seeds in this block
    __shared__ int S_block_offset[1];	// block's offset on d_seed_records
    if (threadIdx.x==0) S_block_nseeds[0] = 0;
    __syncthreads();
    int thread_offset;
    // create an unmapped record if no good aln
    if (n_aln<=0) n_aln = 1;
    thread_offset = atomicAdd(&S_block_nseeds[0], n_aln);
    __syncthreads();
    if (threadIdx.x==0) S_block_offset[0] = atomicAdd(d_Nseeds, S_block_nseeds[0]);
    __syncthreads();

    for (int i=0; i<n_aln; i++){
        int offset = S_block_offset[0] + thread_offset + i;
        d_seed_records[offset].seqID = seqID;
        d_seed_records[offset].regID = i;	// alnID
    }
}


/* run at aln level 
   prepare seqs for SW global
   - .read_right: query
   - .readlen_right: lquery
   - .ref_right: reference
   - .reflen_right: lref
   - .readlen_left: bandwidth
   - .reflen_left: whether cigar should be reversed (1) or not (0)
   calculate bandwidth for SW global
   store l_ref*w to d_sortkeys_in and seqID to d_seqIDs_in
 */

__global__ void FINALIZEALN_preprocessing2_kernel(
        const mem_opt_t *d_opt,
        uint8_t *d_seq,
        int *d_seq_offset,
        const uint8_t *d_pac,
        const bntseq_t *d_bns,
        mem_alnreg_v* d_regs,
        mem_aln_v * d_alns,
        seed_record_t *d_seed_records,
        int Nseeds,
        int *d_sortkeys_in,	// for sorting
        int *d_seqIDs_in,	// for sorting
        void* d_buffer_pools)
{
    int offset = blockIdx.x*blockDim.x + threadIdx.x;
    if (offset>=Nseeds) return;
    int seqID = d_seed_records[offset].seqID;
    int alnID = d_seed_records[offset].regID;
    if (d_alns[seqID].a[alnID].rid == -1) return; // ignore unmapped records

    void *d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, threadIdx.x%32);
    // prepare SW sequences
    int64_t rb = d_regs[seqID].a[alnID].rb; 
    int64_t re = d_regs[seqID].a[alnID].re; 
    int qb = d_regs[seqID].a[alnID].qb;
    int qe = d_regs[seqID].a[alnID].qe;
    uint8_t *query;
    int seq_offset = d_seq_offset[seqID];
    query = d_seq + seq_offset + qb;
    int l_query = qe - qb;
    int64_t rlen;
    int64_t l_pac = d_bns->l_pac;
    uint8_t *rseq = bns_get_seq_gpu(l_pac, d_pac, rb, re, &rlen, d_buffer_ptr);	
    // calculate bandwidth
    int w;
    if (l_query == re-rb){ w=0; }	// no gap, no need to do DP
    else{
        int a = d_opt->a;
        int o_del = d_opt->o_del;
        int e_del = d_opt->e_del;
        int o_ins = d_opt->o_ins;
        int e_ins = d_opt->e_ins;
        int tmp;
        // inferred bandwidth
        w   = infer_bw(l_query, re-rb, d_regs[seqID].a[alnID].score, a, o_del, e_del);
        tmp = infer_bw(l_query, re-rb, d_regs[seqID].a[alnID].score, a, o_ins, e_ins);
        w = w>tmp? w : tmp;
        // global bandwidth
        int max_gap, max_ins, max_del;
        max_ins = (int)((double)(((l_query+1)>>1) * a - o_ins) / e_ins + 1.);
        max_del = (int)((double)(((l_query+1)>>1) * a - o_del) / e_del + 1.);
        max_gap = max_ins > max_del? max_ins : max_del;
        max_gap = max_gap > 1? max_gap : 1;
        tmp = (max_gap + abs((int)rlen - l_query) + 1) >> 1;
        w = w<tmp? w : tmp;
        tmp = abs((int)rlen - l_query) + 3;
        w = w>tmp? w : tmp;
        // w = ((l_query*d_opt->a-d_regs[seqID].a[alnID].score) - d_opt->o_ins)/d_opt->e_ins + 1;
        // tmp = ((l_query*d_opt->a-d_regs[seqID].a[alnID].score) - d_opt->o_del)/d_opt->e_del + 1;
        // w = tmp<w? tmp : w;
        // if (w<0) w=0;
    }
    // save these info to d_seed_records for next kernel
    d_seed_records[offset].read_right = query;		// query for SW
    d_seed_records[offset].readlen_right = l_query;	
    d_seed_records[offset].ref_right = rseq;		// target for SW
    d_seed_records[offset].reflen_right = rlen;
    if (rb>=l_pac) d_seed_records[offset].reflen_left = 1; // signal that cigar need to be reversed
    else d_seed_records[offset].reflen_left = 0;
    d_seed_records[offset].readlen_left = (uint16_t)w;	// bandwidth
    d_sortkeys_in[offset] = w*rlen;		// for sorting
    d_seqIDs_in[offset] = offset;		// for sorting
}

/*
   this kernel reverse both the query and reference for alns whose position is on the reverse strand
   this is to ensure indels to be placed at the leftmost position
   each block process one aln
 */
__global__ void FINALIZEALN_reverseSeq_kernel(seed_record_t *d_seed_records, mem_aln_v *d_alns, void *d_buffer_pools)
{
    // ID = blockIdx.x;
    if (d_seed_records[blockIdx.x].reflen_left==0) return; // no need to reverse
    int seqID = d_seed_records[blockIdx.x].seqID;
    int alnID = d_seed_records[blockIdx.x].regID;		// index on aln.a
    if (d_alns[seqID].a[alnID].rid == -1) return; // ignore unmapped records

    uint8_t *query = d_seed_records[blockIdx.x].read_right;
    int l_query = (int)d_seed_records[blockIdx.x].readlen_right;
    uint8_t *target = d_seed_records[blockIdx.x].ref_right;
    int l_target = (int)d_seed_records[blockIdx.x].reflen_right;
    // allocate memory for reversed sequences
    __shared__ uint8_t* S_new_array[1];
    if (threadIdx.x==0){
        void *d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, blockIdx.x%32);
        S_new_array[0] = (uint8_t*)CUDAKernelMalloc(d_buffer_ptr, l_query+l_target, 1);
    }
    __syncthreads();
    uint8_t *new_query = S_new_array[0];
    uint8_t *new_target = &new_query[l_query];

    // reverse query
    for (int i=threadIdx.x; i<l_query>>1; i+=blockDim.x)
        new_query[l_query-1-i] = query[i];

    // reverse reference
    for (int i=threadIdx.x; i<l_target>>1; i+=blockDim.x) {
        if(target != 0) {
            new_target[l_target-1-i] = target[i];
        }
    }
}

// #define GLOBALSW_BANDWITH_CUTOFF 17
/* run at aln level:
   - perform global SW, calculate cigar and score
   - store score, cigar, n_cigar to d_alns
 */
__global__ void traceback_baseline(
        const mem_opt_t *d_opt,
        seed_record_t *d_seed_records,
        int Nseeds,
        mem_aln_v *d_alns,
        int *d_seqIDs_out,
        void *d_buffer_pools
        )
{
    int ID = blockIdx.x*blockDim.x + threadIdx.x;	// ID on d_seed_records
    if (ID>=Nseeds) return;
    ID = d_seqIDs_out[ID];							// map to new ID after sorting
    int seqID = d_seed_records[ID].seqID;
    int alnID = d_seed_records[ID].regID;		// index on aln.a
    if (d_alns[seqID].a[alnID].rid == -1) return; // ignore unmapped records

    int bandwidth = (int)d_seed_records[ID].readlen_left;
    // if (bandwidth>=GLOBALSW_BANDWITH_CUTOFF) {printf("seqID %d alnID %d bandwidth too large: %d\n", seqID, alnID, bandwidth); __trap();};
    if (bandwidth>=GLOBALSW_BANDWITH_CUTOFF) bandwidth = GLOBALSW_BANDWITH_CUTOFF;
    uint8_t *query = d_seed_records[ID].read_right;
    int l_query = (int)d_seed_records[ID].readlen_right;
    uint8_t *target = d_seed_records[ID].ref_right;
    int l_target = (int)d_seed_records[ID].reflen_right;
    void *d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, threadIdx.x%32);
    // calculate cigar and score
    uint32_t *cigar; int n_cigar, score;
    if (bandwidth==0){
        cigar = (uint32_t*)CUDAKernelMalloc(d_buffer_ptr, 4, 4);
        cigar[0] = l_query<<4 | 0;
        n_cigar = 1;
        for (int i = 0, score = 0; i < l_query; ++i)
            score += d_opt->mat[target[i]*5 + query[i]];
    } else {
        score = ksw_global2(l_query, query, l_target, target, 5, d_opt->mat, d_opt->o_del, d_opt->e_del, d_opt->o_ins, d_opt->e_ins, bandwidth, &n_cigar, &cigar, d_buffer_ptr);
    }
    // calculate NM
    int NM;
    {
        int k, x, y, u, n_mm = 0, n_gap = 0;
        kstring_t str; str.l = str.m = n_cigar*4; str.s = (char*)cigar;
        const char *int2base = (d_seed_records[ID].reflen_left==0)? "ACGTN" : "TGCAN";
        for (k = 0, x = y = u = 0; k < n_cigar; ++k) {
            int op, len;
            cigar = (uint32_t*)str.s;
            op  = cigar[k]&0xf, len = cigar[k]>>4;
            if (op == 0) { // match
                for (int i = 0; i < len; ++i) {
                    if (query[x + i] != target[y + i]) {
                        kputw(u, &str, d_buffer_ptr);
                        kputc(int2base[target[y+i]], &str, d_buffer_ptr);
                        ++n_mm; u = 0;
                    } else ++u;
                }
                x += len; y += len;
            } else if (op == 2) { // deletion
                if (k > 0 && k < n_cigar - 1) { // don't do the following if D is the first or the last CIGAR
                    kputw(u, &str, d_buffer_ptr); kputc('^', &str, d_buffer_ptr);
                    for (int i = 0; i < len; ++i)
                        kputc(int2base[target[y+i]], &str, d_buffer_ptr);
                    u = 0; n_gap += len;
                }
                y += len;
            } else if (op == 1) x += len, n_gap += len; // insertion
        }
        kputw(u, &str, d_buffer_ptr); kputc(0, &str, d_buffer_ptr);
        NM = n_mm + n_gap;
        cigar = (uint32_t*)str.s;
    }
    // write output
    d_alns[seqID].a[alnID].cigar = cigar;
    d_alns[seqID].a[alnID].n_cigar = n_cigar;
    d_alns[seqID].a[alnID].score = score;
    d_alns[seqID].a[alnID].NM = NM;
}


/* run at aln level:
   - perform global SW, calculate cigar and score
   - store score, cigar, n_cigar to d_alns
 */
__global__ void traceback(
        const mem_opt_t *d_opt,
        seed_record_t *d_seed_records,
        int Nseeds,
        mem_aln_v *d_alns,
        int *d_seqIDs_out,
        void *d_buffer_pools
        )
{
    int ID = blockIdx.x*blockDim.x + threadIdx.x;	// ID on d_seed_records
    if (ID>=Nseeds) return;
    ID = d_seqIDs_out[ID];							// map to new ID after sorting
    int seqID = d_seed_records[ID].seqID;
    int alnID = d_seed_records[ID].regID;		// index on aln.a
    if (d_alns[seqID].a[alnID].rid == -1) return; // ignore unmapped records

    int bandwidth = (int)d_seed_records[ID].readlen_left;
    // if (bandwidth>=GLOBALSW_BANDWITH_CUTOFF) {printf("seqID %d alnID %d bandwidth too large: %d\n", seqID, alnID, bandwidth); __trap();};
    if (bandwidth>=GLOBALSW_BANDWITH_CUTOFF) bandwidth = GLOBALSW_BANDWITH_CUTOFF;
    uint8_t *query = d_seed_records[ID].read_right;
    int l_query = (int)d_seed_records[ID].readlen_right;
    uint8_t *target = d_seed_records[ID].ref_right;
    int l_target = (int)d_seed_records[ID].reflen_right;
    void *d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, threadIdx.x%32);
    // calculate cigar and score
    uint32_t *cigar; int n_cigar, score;
    if (bandwidth==0){
        cigar = (uint32_t*)CUDAKernelMalloc(d_buffer_ptr, 4, 4);
        cigar[0] = l_query<<4 | 0;
        n_cigar = 1;
        for (int i = 0, score = 0; i < l_query; ++i)
            score += d_opt->mat[target[i]*5 + query[i]];
    } else {
        score = ksw_global3(l_query, query, l_target, target, 5, d_opt->mat, d_opt->o_del, d_opt->e_del, d_opt->o_ins, d_opt->e_ins, bandwidth, &n_cigar, &cigar, d_buffer_ptr);
    }
    // calculate NM
    int NM;
    {
        int k, x, y, u, n_mm = 0, n_gap = 0;
        kstring_t str; str.l = str.m = n_cigar*4; str.s = (char*)cigar;
        const char *int2base = (d_seed_records[ID].reflen_left==0)? "ACGTN" : "TGCAN";
        for (k = 0, x = y = u = 0; k < n_cigar; ++k) {
            int op, len;
            cigar = (uint32_t*)str.s;
            op  = cigar[k]&0xf, len = cigar[k]>>4;
            if (op == 0) { // match
                for (int i = 0; i < len; ++i) {
                    if (query[x + i] != target[y + i]) {
                        kputw(u, &str, d_buffer_ptr);
                        kputc(int2base[target[y+i]], &str, d_buffer_ptr);
                        ++n_mm; u = 0;
                    } else ++u;
                }
                x += len; y += len;
            } else if (op == 2) { // deletion
                if (k > 0 && k < n_cigar - 1) { // don't do the following if D is the first or the last CIGAR
                    kputw(u, &str, d_buffer_ptr); kputc('^', &str, d_buffer_ptr);
                    for (int i = 0; i < len; ++i)
                        kputc(int2base[target[y+i]], &str, d_buffer_ptr);
                    u = 0; n_gap += len;
                }
                y += len;
            } else if (op == 1) x += len, n_gap += len; // insertion
        }
        kputw(u, &str, d_buffer_ptr); kputc(0, &str, d_buffer_ptr);
        NM = n_mm + n_gap;
        cigar = (uint32_t*)str.s;
    }
    // write output
    d_alns[seqID].a[alnID].cigar = cigar;
    d_alns[seqID].a[alnID].n_cigar = n_cigar;
    d_alns[seqID].a[alnID].score = score;
    d_alns[seqID].a[alnID].NM = NM;
}

/* execute at aln level 
   calculate pos, rid & fix cigar: remove leading or trailing del, add clipping
 */
__global__ void finalize(
        const mem_opt_t *d_opt,
        const bntseq_t *d_bns,
        const uint8_t *d_seq,
        int *d_seq_offset,
        mem_alnreg_v *d_regs,
        mem_aln_v *d_alns,
        seed_record_t *d_seed_records,
        int Nseeds,
        void *d_buffer_pools,
        int *d_offsets,
        int *d_rids,
        int64_t *d_positions,
        int *d_ncigars,
        uint32_t *d_cigars
        )
{
    int ID = blockIdx.x*blockDim.x + threadIdx.x;
    if (ID>=Nseeds) return;
    int seqID = d_seed_records[ID].seqID;
    int alnID = d_seed_records[ID].regID;
    if (d_alns[seqID].a[alnID].rid == -1) return; // ignore unmapped records

    mem_aln_t *a = &d_alns[seqID].a[alnID];	// output address
    mem_alnreg_t *ar = &d_regs[seqID].a[alnID];
    // mapq
    if (ar->secondary<0) a->mapq = mem_approx_mapq_se(d_opt, ar);
    else a->mapq = 0;
    // calculate pos
    int is_rev; int64_t pos;
    pos = bns_depos_GPU(d_bns, d_seed_records[ID].reflen_left==0? ar->rb : ar->re-1, &is_rev);

    // fix cigar
    // squeeze out leading or trailing deletions
    int n_cigar = a->n_cigar;
    uint32_t *cigar = a->cigar;
    if (n_cigar > 0) {
        if ((cigar[0]&0xf) == 2) {	// leading del
            pos += cigar[0]>>4;
            --n_cigar;
            cigar = &cigar[1];
        } else if ((cigar[n_cigar-1]&0xf) == 2) {	// trailing del
            --n_cigar;
        }
    }
    // add clipping to cigar
    int seq_offset = d_seq_offset[seqID];
    int seq_offset_next = d_seq_offset[seqID + 1];
    int l_query = seq_offset_next - seq_offset;

    int qb = ar->qb; int qe = ar->qe; 
    if (qb != 0 || qe != l_query) {
        void *d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, threadIdx.x%32);
        int clip5, clip3;
        clip5 = is_rev? l_query - qe : qb;
        clip3 = is_rev? qb : l_query - qe;
        uint32_t *new_cigar = (uint32_t*)CUDAKernelMalloc(d_buffer_ptr, 4 * (n_cigar + 2), 4);
        if (clip5) {
            new_cigar[0] = clip5<<4 | 3;
            // copy the existing cigars to [1] location
            memcpy(&new_cigar[1], cigar, n_cigar*4);
            ++n_cigar;
        } else // copy the existing cigars to [0] location
            memcpy(new_cigar, cigar, n_cigar*4);
        if (clip3) {
            new_cigar[n_cigar++] = clip3<<4 | 3;
        }
        // save new cigars to global mem
        a->n_cigar = n_cigar;
        a->cigar = new_cigar;
    }

    // calculate rid, is_alt
    a->rid = bns_pos2rid_gpu(d_bns, pos);
    a->pos = pos - d_bns->anns[a->rid].offset;
    a->is_rev = is_rev;
    a->is_alt = ar->is_alt;
    a->alt_sc = ar->alt_sc;
    a->sub = ar->sub>ar->csub? ar->sub : ar->csub;

    // flag and sub
    int flag = 0;
    if (ar->secondary>=0) {
        a->sub = -1; // don't output sub-optimal score
        flag |= 0x100;
    } else if (alnID>0){	// supplementary
        flag |= (d_opt->flag&MEM_F_NO_MULTI)? 0x10000 : 0x800;
    }
    if (ar->rid<0) flag |= 0x4;		// flag if unmapped
    if (is_rev) flag |= 0x10;		// is on reverse strand
    a->flag = flag;


    // Calculate offsets for different arrays 
    int basic_offset = d_offsets[seqID] + alnID;
    d_rids[basic_offset] = a->rid;
    d_positions[basic_offset] = a->pos;
    d_ncigars[basic_offset] = a->n_cigar;
   /* printf("[%s] basic_offset=%d d_rids[%d]=%d d_positions[%d]=%ld d_ncigars[%d]=%d\n", 
           __func__, basic_offset, basic_offset, d_rids[basic_offset],
           basic_offset, d_positions[basic_offset],
           basic_offset, d_ncigars[basic_offset]); */

    // Calculate separate offset for cigar array
    int cigar_offset = basic_offset * MAX_N_CIGAR;
    for(int kk=0; kk<a->n_cigar; kk++)
        d_cigars[cigar_offset + kk] = a->cigar[kk];
}

#if 0
/* convert aln to SAM strings
   run at aln level
   assume that reads are not paired
outputs:
- d_aln->XA = &SAM_string
- d_aln->rid = len(SAM_string)
 */
__global__ void SAMGEN_aln2sam_finegrain_kernel(
        const mem_opt_t *d_opt,
        const bntseq_t *d_bns,
        const bseq1_t *d_seqs,
        mem_aln_v *d_alns,
        seed_record_t *d_seed_records,
        int Nseeds,
        void *d_buffer_pools
        )
{
    int ID = blockIdx.x*blockDim.x + threadIdx.x;
    if (ID>=Nseeds) return;	// don't run padded threads in last block
    int seqID = d_seed_records[ID].seqID;
    int alnID = d_seed_records[ID].regID;
    mem_aln_t *a = &d_alns[seqID].a[alnID];
    // prepare enough memory for SAM string
    void *d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, threadIdx.x%32);
    kstring_t str;
    str.l = 0; str.m = 512; str.s = (char*)CUDAKernelMalloc(d_buffer_ptr, str.m, 1);
    // 1. QNAME
    int l_name = strlen_GPU(d_seqs[seqID].name);
    kputsn(d_seqs[seqID].name, l_name, &str, d_buffer_ptr); kputc('\t', &str, d_buffer_ptr);
    // 2. FLAG
    kputw((a->flag&0xffff)|(a->flag&0x10000? 0x100 : 0), &str, d_buffer_ptr); kputc('\t', &str, d_buffer_ptr);

    if (a->rid >= 0) { // with coordinate
                       // 3. RNAME
        kputs(d_bns->anns[a->rid].name, &str, d_buffer_ptr); kputc('\t', &str, d_buffer_ptr);
        // 4. POS
        kputl(a->pos + 1, &str, d_buffer_ptr); kputc('\t', &str, d_buffer_ptr); // POS
                                                                                // 5. MAPQ
        kputw(a->mapq, &str, d_buffer_ptr); kputc('\t', &str, d_buffer_ptr); // MAPQ
                                                                             // 6. CIGAR
        add_cigar(d_opt, a, &str, alnID, d_buffer_ptr);
    } else kputsn("*\t0\t0\t*", 7, &str, d_buffer_ptr); // without coordinte
    kputc('\t', &str, d_buffer_ptr);

    // MATE POSITION (7, 8, 9) NOT APPLICABLE
    kputsn("*\t0\t0", 5, &str, d_buffer_ptr); kputc('\t', &str, d_buffer_ptr);

    // 10,11. SEQ and QUAL
    if (a->flag & 0x100) { // for secondary alignments, don't write SEQ and QUAL
        kputsn("*\t*", 3, &str, d_buffer_ptr);
    } else if (!a->is_rev) { // the forward strand
                             // SEQ
        int i, qb = 0, qe = d_seqs[seqID].l_seq;
        if (a->n_cigar && alnID && !(d_opt->flag&MEM_F_SOFTCLIP) && !a->is_alt) { // have cigar && not the primary alignment && not softclip all
            if ((a->cigar[0]&0xf) == 4 || (a->cigar[0]&0xf) == 3) qb += a->cigar[0]>>4;
            if ((a->cigar[a->n_cigar-1]&0xf) == 4 || (a->cigar[a->n_cigar-1]&0xf) == 3) qe -= a->cigar[a->n_cigar-1]>>4;
        }
        ks_resize(&str, str.l + (qe - qb) + 1, d_buffer_ptr);
        for (i = qb; i < qe; ++i) str.s[str.l++] = "ACGTN"[(int)d_seqs[seqID].seq[i]];
        kputc('\t', &str, d_buffer_ptr);
        // QUAL
        if (d_seqs[seqID].qual) {
            ks_resize(&str, str.l + (qe - qb) + 1, d_buffer_ptr);
            memcpy(&str.s[str.l], &d_seqs[seqID].qual[qb], qe-qb); str.l+=qe-qb;
            str.s[str.l] = 0;
        } else kputc('*', &str, d_buffer_ptr);
    } else { // the reverse strand
        int i, qb = 0, qe = d_seqs[seqID].l_seq;
        if (a->n_cigar && alnID && !(d_opt->flag&MEM_F_SOFTCLIP) && !a->is_alt) {
            if ((a->cigar[0]&0xf) == 4 || (a->cigar[0]&0xf) == 3) qe -= a->cigar[0]>>4;
            if ((a->cigar[a->n_cigar-1]&0xf) == 4 || (a->cigar[a->n_cigar-1]&0xf) == 3) qb += a->cigar[a->n_cigar-1]>>4;
        }
        ks_resize(&str, str.l + (qe - qb) + 1, d_buffer_ptr);
        for (i = qe-1; i >= qb; --i) str.s[str.l++] = "TGCAN"[(int)d_seqs[seqID].seq[i]];
        kputc('\t', &str, d_buffer_ptr);
        if (d_seqs[seqID].qual) { // printf qual
            ks_resize(&str, str.l + (qe - qb) + 1, d_buffer_ptr);
            for (i = qe-1; i >= qb; --i) str.s[str.l++] = d_seqs[seqID].qual[i];
            str.s[str.l] = 0;
        } else kputc('*', &str, d_buffer_ptr);
    }
    kputc('\n', &str, d_buffer_ptr);

    // save output
    d_alns[seqID].a[alnID].XA = str.s;
    d_alns[seqID].a[alnID].rid = str.l;
}

/* concatenate all SAM strings from a read's alns and write to SAM output location 
   at this point, d_aln->a->XA is SAM string, d_aln->a->rid is len(SAM string)
   Copy all SAM strings to d_seq_sam_ptr[] as follows:
   - atomicAdd on d_seq_sam_size to reserve a location on the d_seq_sam_ptr array (this array is allocated with page-lock for faster transfer)
   - copy SAM to the reserved location
   - save the offset to seq->SAM for retrieval on host
   - NOTE: the NULL-terminating character is also a part of the SAM string
 */
__global__ void SAMGEN_concatenate_kernel(
        mem_aln_v *d_alns,
        bseq1_t *d_seqs,
        int n_seqs,
        char* d_seq_sam_ptr, int *d_seq_sam_size
        )
{
    int seqID = blockIdx.x*blockDim.x + threadIdx.x;
    if (seqID>=n_seqs) return;
    int n_aln = d_alns[seqID].n;
    if (n_aln<=0) n_aln = 1;	// to process unmapped records
    mem_aln_t *a = d_alns[seqID].a;
    // calculate total length of SAM strings
    // then add them up at block level
    __shared__ int S_total_l_sams[1];
    if (threadIdx.x==0) S_total_l_sams[0] = 0;
    __syncthreads(); __syncwarp();
    int l_sams = 0;
    for (int i=0; i<n_aln; i++)
        l_sams+=a[i].rid;
    l_sams++;	// add 1 for the terminating NULL
    int thread_offset = atomicAdd(&S_total_l_sams[0], l_sams);
    __syncthreads(); __syncwarp();
    // now add the block's total to d_seq_sam_size
    __shared__ int S_block_offset[1];
    if (threadIdx.x==0) S_block_offset[0] = atomicAdd(d_seq_sam_size, S_total_l_sams[0]);
    __syncthreads(); __syncwarp();
    int offset = S_block_offset[0] + thread_offset;	// actual offset on d_seq_sam_ptr
                                                    // record offset to sam
    d_seqs[seqID].sam = (char*)(offset);
    // copy all SAM strings to the designated location on d_seq_sam_ptr
    for (int i=0; i<n_aln; i++){
        int l_sam = a[i].rid;	// length of this aln's SAM
        char* sam = a[i].XA;	// SAM string of this aln
        memcpy(&d_seq_sam_ptr[offset], sam, l_sam);	// transfer this aln's SAM, excluding the terminating NULL
        offset+= l_sam;			// increment for next aln
    }
    d_seq_sam_ptr[offset] = 0;	// NULL-terminating char
}
#endif
