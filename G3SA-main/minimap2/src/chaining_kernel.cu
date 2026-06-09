#include "chaining_kernel.h"

#define RANGE_HEURISTIC 8

__device__ 
static inline float mg_log2_device(float x) // NB: this doesn't work when x<2
{
	union { float f; uint32_t i; } z = { x };
	float log_2 = ((z.i >> 23) & 255) - 128;
	z.i &= ~(255 << 23);
	z.i += 127 << 23;
	log_2 += (-0.34484843f * z.f + 2.02466578f) * z.f - 0.67487759f;
	return log_2;
}

__device__
inline int device_range_search(uint64_t * x, int st, int end, int max_dist_x){
    // naive version 
    while(st < end) {
        if((int32_t)x[st] + max_dist_x < (int32_t)x[end] || (x[st]>>32)!=(x[end]>>32) || x[end]==0) end--;
        else break;
    }
    return end;
}

__global__
void mm_compute_chain_range(mm_mapopt_t* opt, uint64_t* x, int* a_len, int2* r, bool* pass, int32_t* sc, int64_t* p, int n_task){

   int bid = blockIdx.x;
   int tid = threadIdx.x;
   int ofs; 
   __shared__ int n_range;
   if(tid==0) n_range = 0;

   int range_heuristic[3] = {4, 16, 256};
   int max_dist_x = opt->max_gap;
   
    for(int i = bid; i < n_task; i += gridDim.x){ // multi threaded implementation 
        ofs = i * READ_SEG;
        for(int j = tid; j < READ_SEG; j += blockDim.x){
            r[ofs + j].x = -1;
            r[ofs + j].y = 0;
        }
        for(int st = tid; st < READ_SEG; st+= blockDim.x){
            //range search heursitic (from Guo et al.)
            int end;
            for(int k = 0; k < 3; k++){
                end = st + range_heuristic[k];
                end = end >= READ_SEG? READ_SEG : end;
                if(end > st && ((int32_t)x[ofs+st] + max_dist_x < (int32_t)x[ofs+end] || (x[ofs+st]>>32)!=(x[ofs+end]>>32))) break;
            }
            // find exact range
            end = device_range_search(&x[ofs], st, end, max_dist_x);
            if(end > st) {
                r[ofs + st].x = st; 
                if(end == READ_SEG) r[ofs + st].y = READ_SEG; 
                else r[ofs + st].y = end;
                n_range++;
            }
        }
        if(tid==0){
            if(n_range == 0 || (ofs!=0 && x[ofs - 1] + max_dist_x > x[ofs] && r[ofs].y > 0)) pass[i] = true;
        }
        __syncthreads();
        bool p = pass[i];
        if(p && ofs!=0 && r[ofs + tid].x > 0 && r[ofs + tid -1].x < 0) {
            pass[i] = false;
        }
        __syncthreads();
    }
    // initialize chaining score array
    for(int i = bid; i < n_task; i += gridDim.x) {
        ofs = i * READ_SEG;
        for(int j = tid; j < READ_SEG; j += blockDim.x) {
            sc[i*READ_SEG+j] = 15;
            p[i*READ_SEG+j] = -1;
        }
    }
}
     
__global__ 
void range_exact_naive(uint64_t* x, uint64_t* y, int* offset, int* a_len, uint2* r, int* n_r, int max_dist_x, int n_task){
    
    int bid = blockIdx.x;
    int tid = threadIdx.x;

    for(int job_idx = bid; job_idx < n_task; job_idx += BLOCK_NUM) {

    int alen = a_len[job_idx];
    int ofs = offset[job_idx];

    uint64_t range[RANGE_HEURISTIC] = {0, 16, 512, 1024, 2048, 3072, 4096, MAX_ITER_CHAIN};

    for(uint64_t start = tid; start < alen; start += THREAD_NUM){
            int end = start + MAX_ITER_CHAIN >= alen? alen - 1 : start + MAX_ITER_CHAIN;

            for(int j = 0; j < RANGE_HEURISTIC; j++){
                if(start + range[j] > alen) break;
                if(x[ofs+start]>>32 != x[ofs+start+range[j]]>>32 || x[ofs+start+range[j]] > x[ofs + start] + max_dist_x){
                    end = start + range[j];
                    break;
                }
            }
            while(end > start && (x[ofs+start]>>32 != x[ofs+end]>>32 || x[ofs+end] > x[ofs + start] + max_dist_x)) end--;
            
            r[ofs + start].x = start;
            r[ofs + start].y = end;
        
            __syncthreads();
    }
    if(tid == 0) n_r[job_idx] = alen;
    }
}

__device__
__forceinline__
score_t chain_dp_score_noopt(uint64_t ai_x, uint64_t ai_y, uint64_t aj_x, uint64_t aj_y,
        float avg_qspan, int max_dist_x, int max_dist_y, int bw, int is_cdna, float chn_pen_gap, float chn_pen_skip, int id)
{

    // act: back anchor (i)
    // curr: front anchor (j)
    if(aj_x < ai_x - max_dist_x ||((aj_x>> 32&0xff) != (ai_x>> 32&0xff))) return INT32_MIN;
    bool sid = ((ai_y & MM_SEED_SEG_MASK) >> MM_SEED_SEG_SHIFT)==((aj_y & MM_SEED_SEG_MASK) >> MM_SEED_SEG_SHIFT);
    int32_t dq = (int32_t)ai_y - (int32_t)aj_y, dr, dd, dg, sc;
    if(dq <=0 || dq > max_dist_x) return INT32_MIN;
    dr = (int32_t)(ai_x - aj_x);
    if(sid && (dr == 0 || dq > max_dist_y)) return INT32_MIN;
    dd = dr > dq? dr - dq : dq - dr;
    if (sid && dd > bw) return INT32_MIN;
    // if (n_seg > 1 && !is_cdna && sidi == sidj && dr > max_dist_y) return INT32_MIN;
    dg = dr < dq? dr : dq;
    //q_span = aj_y >> 32&0xff;
    sc = MM_QSPAN < dg? MM_QSPAN : dg;

    if(dd || dg > MM_QSPAN){
        float lin_pen, log_pen;
		lin_pen = chn_pen_gap * (float)dd + chn_pen_skip * (float)dg;
		log_pen = dd >= 1? mg_log2_device(dd + 1) : 0.0f; // mg_log2() only works for dd>=2
		if (is_cdna || !sid) {
			if (!sid && dr == 0) ++sc; // possibly due to overlapping paired ends; give a minor bonus
			else if (dr > dq || !sid) sc -= (int)(lin_pen < log_pen? lin_pen : log_pen); // deletion or jump between paired ends
			else sc -= (int)(lin_pen + .5f * log_pen);
		} else sc -= (int)(lin_pen + .5f * log_pen);
    }

    return sc;
}

__device__
__forceinline__
score_t chain_dp_score_opt(uint64_t ai_x, uint64_t ai_y, uint64_t aj_x, uint64_t aj_y,
        int max_dist_x, int max_dist_y, int bw, int is_cdna, float chn_pen_gap, float chn_pen_skip)
{

    // act: back anchor (i)
    // curr: front anchor (j)
    if(aj_x < ai_x - max_dist_x ||((aj_x>> 32&0xff) != (ai_x>> 32&0xff))) return INT32_MIN;
    bool sid = ((ai_y & MM_SEED_SEG_MASK) >> MM_SEED_SEG_SHIFT)==((aj_y & MM_SEED_SEG_MASK) >> MM_SEED_SEG_SHIFT);
    int32_t dq = (int32_t)ai_y - (int32_t)aj_y, dr, dd, dg, sc;
    if(dq <=0 || dq > max_dist_x) return INT32_MIN;
    dr = (int32_t)(ai_x - aj_x);
    if(sid && (dr == 0 || dq > max_dist_y)) return INT32_MIN;
    dd = dr > dq? dr - dq : dq - dr;
    if (sid && dd > bw) return INT32_MIN;
    // if (n_seg > 1 && !is_cdna && sidi == sidj && dr > max_dist_y) return INT32_MIN;
    dg = dr < dq? dr : dq;
    //q_span = aj_y >> 32&0xff;
    sc = MM_QSPAN < dg? MM_QSPAN : dg;

    if(dd || dg > MM_QSPAN){
        float lin_pen, log_pen;
		lin_pen = chn_pen_gap * (float)dd + chn_pen_skip * (float)dg;
		log_pen = dd >= 1? mg_log2_device(dd + 1) : 0.0f; // mg_log2() only works for dd>=2
		if (is_cdna || !sid) {
			if (!sid && dr == 0) ++sc; // possibly due to overlapping paired ends; give a minor bonus
			else if (dr > dq || !sid) sc -= (int)(lin_pen < log_pen? lin_pen : log_pen); // deletion or jump between paired ends
			else sc -= (int)(lin_pen + .5f * log_pen);
		} else sc -= (int)(lin_pen + .5f * log_pen);
    }

    return sc;


}

__global__ 
void mm_compute_chain_short(int32_t* sc, int64_t* p, const uint64_t* anchors_x, const uint64_t* anchors_y, const uint2* range, 
                    int* offset, int* r_len, int* a_len, bool* pass, 
                    int max_dist_x, int max_dist_y, int bw, int is_cdna, float chn_pen_gap, float chn_pen_skip, 
                    int n_task, int* long_num, int* long_buf_st, int* long_buf_len, int* mid_num, int* mid_buf_st, int* mid_buf_len){

    int tid = threadIdx.x;
    int bid = blockIdx.x;

    // int max_dist_x = opt->max_gap;
    // int max_dist_y = opt->max_gap;
    // int bw = opt->bw;
    // int is_cdna = !!(opt->flag & MM_F_SPLICE);
    // float chn_pen_gap = opt->chain_gap_scale * 0.01 * mi->k;
    // float chn_pen_skip = opt->chain_skip_scale * 0.01 * mi->k;

    for(int job_idx = bid; job_idx < n_task; job_idx += gridDim.x) {
        
        if(pass[job_idx]) continue;
        int i = 0;
        int end_segid = job_idx;
        while(i < READ_SEG && end_segid == job_idx) {
            
            if(range[end_segid * READ_SEG+i].x == i && range[end_segid * READ_SEG+i].y >0) {

                int st = end_segid * READ_SEG + i;
                int end = i;

                while(range[end_segid * READ_SEG + end].y != 0){
                    if(range[end_segid * READ_SEG + end].y != READ_SEG){
                        end = range[end_segid * READ_SEG + end].y; 
                    }
                    else { // to the next segment
                        end_segid++; 
                        end = 0;
                    }
                }
                
                i = end;
                end += end_segid * READ_SEG;

                if(end-st > LONG_RANGE_CUT) {
                    if(tid==0){
                        int long_idx = atomicAdd(long_num,1);
                        long_buf_st[long_idx] = st;
                        long_buf_len[long_idx] = end - st;
                    }
                    continue;
                }
                else if(end-st > MID_RANGE_CUT){
                    if(tid==0){
                        int mid_idx = atomicAdd(mid_num,1);
                        mid_buf_st[mid_idx] = st;
                        mid_buf_len[mid_idx] = end - st;
                    }
                    continue;
                }
        
                while(st < end) {
                    int32_t range_i = (end - st < MAX_ITER_CHAIN)? end - st : MAX_ITER_CHAIN; 
                    for (int32_t j = tid; j < range_i; j += blockDim.x) {
                        if(anchors_x[st] + max_dist_x < anchors_x[st+j+1]) break;
                        int32_t score = chain_dp_score_noopt(
                                    anchors_x[st+j+1], 
                                    anchors_y[st+j+1], 
                                    anchors_x[st], 
                                    anchors_y[st], 15,
                                    max_dist_x, max_dist_y, bw, is_cdna, chn_pen_gap, 
                                    chn_pen_skip, 0);

            
                        if (score == INT32_MIN) continue;
                        score += sc[st];
                        if (score >= sc[st+j+1] && score != 15) {
                            sc[st+j+1] = score;
                            p[st+j+1] = j+1;

                        }
                    }
                    st++;
                    __syncthreads();
                }
            }
            else i++;
        }
    }
}

__device__ int job_idx_global;

__global__ 
void chain_naive_long(int32_t* sc, int64_t* p, const uint64_t* anchors_x, const uint64_t* anchors_y, const int* range_st, const int* range_len, 
                    int* offset, int* a_len, 
                    int max_dist_x, int max_dist_y, int bw, int is_cdna, float chn_pen_gap, float chn_pen_skip, int n_task){
    // const Misc blk_misc = misc;
    int tid = threadIdx.x;
    int bid = blockIdx.x;

    __shared__ int job_idx;

    if (tid == 0 && bid == 0) {
        job_idx_global = gridDim.x;
    }
    if (tid == 0) {
        job_idx = bid;
    }
    __syncthreads();

    while(job_idx < n_task){

        int32_t st = range_st[job_idx];
        int32_t end = st + range_len[job_idx];
        
        while(st < end) {
            int32_t range_i = (end - st < MAX_ITER_CHAIN)? end - st : MAX_ITER_CHAIN; 
            for (int32_t j = tid; j < range_i; j += blockDim.x) {
                if(anchors_x[st] + max_dist_x < anchors_x[st+j+1]) break;
                int32_t score = chain_dp_score_noopt(
                                    anchors_x[st+j+1], 
                                    anchors_y[st+j+1], 
                                    anchors_x[st], 
                                    anchors_y[st], 15,
                                    max_dist_x, max_dist_y, bw, is_cdna, chn_pen_gap, 
                                    chn_pen_skip, 0);

            
                if (score == INT32_MIN) continue;
                score += sc[st];
                if (score >= sc[st+j+1] && score != 15) {
                    sc[st+j+1] = score;
                    p[st+j+1] = j+1;

                }
            }
            st++;
            __syncthreads();
        }
        if (tid == 0) job_idx = atomicAdd(&job_idx_global, 1);
        __syncthreads();
    }
}

#define shmem_size THREAD_NUM_REG 
#define reg_size THREAD_NUM_REG

__global__ 
void mm_compute_chain_long_tiled(int32_t* sc, int64_t* p, const uint64_t* anchors_x, const uint64_t* anchors_y, const int* range_st, const int* range_len, 
                    int* offset, int* a_len, 
                    int max_dist_x, int max_dist_y, int bw, int is_cdna, float chn_pen_gap, float chn_pen_skip, int n_task){
    // const Misc blk_misc = misc;
    int tid = threadIdx.x;
    int bid = blockIdx.x;

    // shared memory
    __shared__ uint64_t ax_buf[shmem_size];
    __shared__ uint64_t ay_buf[shmem_size];
    __shared__ int32_t sc_buf[shmem_size];
    __shared__ int16_t p_buf[shmem_size];

    // thread local registers TODO: change to store more info?
    uint64_t ax_reg;
    uint64_t ay_reg;
    int32_t sc_reg;
    int16_t p_reg;
    int32_t st, end, r;
    int j, k;

    __shared__ int job_idx;

    if (tid == 0 && bid == 0) {
        job_idx_global = gridDim.x;
    }
    if (tid == 0) {
        job_idx = bid;
    }
    __syncthreads();

    // compute chaining score 
    while(job_idx < n_task){
        st = range_st[job_idx];
        end = st + range_len[job_idx];
        
        for(;st < end; st += shmem_size){

            r = (end - st < MAX_ITER_CHAIN + shmem_size)? end - st : MAX_ITER_CHAIN + shmem_size; 

            // fill shared memory for prologue
            for(j = tid; j <= r && j < shmem_size; j+= blockDim.x) {
                ax_buf[j] = anchors_x[st+j];
                ay_buf[j] = anchors_y[st+j];
                sc_buf[j] = sc[st+j];
                p_buf[j] = p[st+j];
            }

            __syncthreads();

            // compute prologue
            for(j = 0; j <= r && j < shmem_size; j++) { // predecessor
                for(int32_t k = j + tid; k < r && k < shmem_size - 1; k+= blockDim.x){
                    int score = chain_dp_score_opt(
                                    ax_buf[k+1], 
                                    ay_buf[k+1], 
                                    ax_buf[j], 
                                    ay_buf[j],
                                    max_dist_x, max_dist_y, bw, is_cdna, chn_pen_gap, 
                                    chn_pen_skip);    
                    
                    if (score == INT32_MIN) continue;
                    score += sc_buf[j];
                    if (score >= sc_buf[k+1] && score > MM_QSPAN) { // TODO: parent default to MAX VAL
                        sc_buf[k+1] = score;
                        p_buf[k+1] = k-j+1;
                    }
                }
                __syncthreads();
            }

            //Write prologue
            for(j = tid; j <= r && j < shmem_size; j+= blockDim.x) {
                if(p_buf[j]>=0){
                    sc[st + j] = sc_buf[j];
                    p[st + j] = p_buf[j];
                }
            }
            __syncthreads();

            // tail updates
            for(j = st + shmem_size; j <= st + r; j += blockDim.x){    
                // load to register 
                ax_reg = anchors_x[j+tid];
                ay_reg = anchors_y[j+tid];
                sc_reg = sc[j+tid];
                p_reg = p[j+tid];

                if(ax_buf[shmem_size-1] + max_dist_x < ax_reg || st + shmem_size + MAX_ITER_CHAIN < j + tid) break;

                // compute chain score 
                for(k = 0; k < shmem_size; k++){
                    int idx = (tid + k) & 0x3FF;
                    int score = chain_dp_score_opt( 
                                    ax_reg, 
                                    ay_reg,
                                    ax_buf[idx], 
                                    ay_buf[idx],
                                    max_dist_x, max_dist_y, bw, is_cdna, chn_pen_gap, 
                                    chn_pen_skip);
                    if (score == INT32_MIN) continue;
                    score += sc_buf[idx];
                    if (score >= sc_reg && score > MM_QSPAN) {
                        sc_reg = score;
                        p_reg = j + tid - (st + idx);
                    }
                }
                // write chain score 
                if(p_reg > 0 && p_reg <= MAX_ITER_CHAIN) {
                    sc[j+tid] = sc_reg;
                    p[j+tid] = p_reg;
                }
            }
            __syncthreads();

        } // in-range iteration
        if (tid == 0) job_idx = atomicAdd(&job_idx_global, 1);
        __syncthreads();
    } // range iteration
}



/* Chain post-processing */

__device__
static int64_t mg_chain_bk_end(int32_t max_drop, const int64_t *z_x, const int64_t *z_y, int32_t *f, int64_t *p, int32_t *t, int64_t k)
{
    // identical to minimap2 cpu ver.
	int64_t i = z_y[k], end_i = -1, max_i = i;
	int32_t max_s = 0;
	if (i < 0 || t[i] != 0) return i;
	do {
		int32_t s;
		t[i] = 2;
		end_i = i = (p[i] > 0)? i - p[i] : p[i]; // parent array modified 
		s = i < 0? (int32_t)z_x[k] : (int32_t)z_x[k] - f[i];
		if (s > max_s) max_s = s, max_i = i;
		else if (max_s - s > max_drop) break;
	} while (i >= 0 && t[i] == 0);
	for (i = z_y[k]; i >= 0 && i != end_i && p[i] > 0; i = i - p[i]) // reset modified t[] // parent array modified 
		t[i] = 0;
	return max_i;
}


__device__
static inline void mm_cal_fuzzy_len(mm_reg1_t *r, const uint64_t *ax, const uint64_t *ay)
{
	int i;
	r->mlen = r->blen = 0;
	if (r->cnt <= 0) return;
	r->mlen = r->blen = ay[r->as]>>32&0xff;
	for (i = r->as + 1; i < r->as + r->cnt; ++i) {
		int span = ay[i]>>32&0xff;
		int tl = (int32_t)ax[i] - (int32_t)ax[i-1];
		int ql = (int32_t)ay[i] - (int32_t)ay[i-1];
		r->blen += tl > ql? tl : ql;
		r->mlen += tl > span && ql > span? span : tl < ql? tl : ql;
	}
}

__device__
static inline void mm_reg_set_coor(mm_reg1_t *r, int32_t qlen, const uint64_t *ax, const uint64_t *ay, int is_qstrand)
{ // NB: r->as and r->cnt MUST BE set correctly for this function to work
    // set alignment positions
	int32_t k = r->as, q_span = (int32_t)(ay[k]>>32&0xff);
	r->rev = ax[k]>>63;
	r->rid = ax[k]<<1>>33;
	r->rs = (int32_t)ax[k] + 1 > q_span? (int32_t)ax[k] + 1 - q_span : 0; // NB: target span may be shorter, so this test is necessary
	r->re = (int32_t)ax[k + r->cnt - 1] + 1;
	if (!r->rev || is_qstrand) {
		r->qs = (int32_t)ay[k] + 1 - q_span;
		r->qe = (int32_t)ay[k + r->cnt - 1] + 1;
	} else {
		r->qs = qlen - ((int32_t)ay[k + r->cnt - 1] + 1);
		r->qe = qlen - ((int32_t)ay[k] + 1 - q_span);
	}
	mm_cal_fuzzy_len(r, ax, ay);
}

__device__
static inline uint64_t hash64(uint64_t key)
{
	key = (~key + (key << 21));
	key = key ^ key >> 24;
	key = ((key + (key << 3)) + (key << 8));
	key = key ^ key >> 14;
	key = ((key + (key << 2)) + (key << 4));
	key = key ^ key >> 28;
	key = (key + (key << 31));
	return key;
}


__global__
void chain_backtrack(int* n_a, uint64_t* g_ax, uint64_t* g_ay, int32_t* g_sc, int64_t *g_p, uint64_t* g_u, 
                    int64_t* g_zx, int64_t* g_zy, int32_t* g_t, int64_t* g_v,
                    int* offset, int* r_offset, int32_t min_cnt, int32_t min_sc, int32_t max_drop,
                    uint32_t hash, int* d_len, int is_qstrand, int* n_chain, mm_chain_t* g_c, int n_task, int* g_n_v, bool* dropped, bool final){
    // sc: score array (f)
    // p: parent array

    int id = threadIdx.x + blockIdx.x * blockDim.x;
    
    for(int job_idx = id; job_idx < n_task; job_idx += gridDim.x * blockDim.x) {
    
        int ofs = offset[job_idx];
        int r_ofs = r_offset[job_idx];
        int n = n_a[job_idx];
        uint64_t *ax = &g_ax[ofs];
        uint64_t *ay = &g_ay[ofs];
        int32_t* f = &g_sc[ofs];
        int64_t* p = &g_p[ofs]; // w.x

        int64_t* z_x = &g_zx[ofs]; // b.x
        int64_t* z_y = &g_zy[ofs]; // b.y
	    uint64_t* u = &g_u[ofs];
        int32_t* t = &g_t[ofs]; // u2
        int64_t* v = &g_v[ofs]; // w.y
    
	    int64_t i, j, k, n_v, n_z = 0;
	    int32_t n_u;

        if(job_idx==0) *n_chain = 0;


        // 1. Backtracking

        // initialize t (visited) array
        for(int j = 0; j < n; j++) t[j]=0;

	    for (i = 0, k = 0; i < n; ++i) { // populate z[] : filter anchors (== seeds) by chaining score
		    if (f[i] >= min_sc) {
                ++n_z;
                z_x[k] = (int64_t)f[i];
                z_y[k++] = i;
            }
        }

        if(n_z <= 0) { // zero seeds left : terminate alignment
            g_n_v[id] = 0;
            continue;
        }

        thrust::sort_by_key(thrust::seq, z_x, z_x + n_z, z_y); // sort anchors by score

	    for (k = n_z - 1, n_v = n_u = 0; k >= 0; --k) { // populate u[] : backtrack anchors 
		    if (t[z_y[k]] == 0) {
			    int64_t n_v0 = n_v, end_i;
			    int32_t sc;
			    end_i = mg_chain_bk_end(max_drop, z_x, z_y, f, p, t, k);
			    for (i = z_y[k]; i != end_i; i = (p[i]<0)? p[i] : i - p[i]) // parent array modified : relative index
				    v[n_v++] = i, t[i] = 1;
			    sc = i < 0? (int32_t)z_x[k] : (int32_t)z_x[k] - f[i];
			    if (sc >= min_sc && n_v > n_v0 && n_v - n_v0 >= min_cnt)
				    u[n_u++] = (uint64_t)sc << 32 | (n_v - n_v0);
			    else n_v = n_v0;
		    }
	    }

        assert(n_v <= n); // not violated in most cases; but need to be fixed
        g_n_v[id] = n_v;

        // 2. Sort & compact chaining results (compact_a in original code)

        // reuse allocated space; FIXME
        int64_t *b_x = z_x;
        int64_t *b_y = z_y;
        int64_t *w_x = p;
        int64_t *w_y = v;
        int64_t *u2 = (int64_t*)t;

        for (i = 0, k = 0; i < n_u; ++i) {
		    int32_t k0 = k, ni = (int32_t)u[i];
            for (j = 0; j < ni; ++j){
			    b_x[k] = ax[v[k0 + (ni - j - 1)]];
                b_y[k++] = ay[v[k0 + (ni - j - 1)]];
            }
	    }

        // sort u[] and a[] by the target position, such that adjacent chains may be joined
	    for (i = k = 0; i < n_u; ++i) {
		    w_x[i] = b_x[k], w_y[i] = (uint64_t)k<<32|i;
		    k += (int32_t)u[i];
	    }
    
        thrust::sort_by_key(thrust::device, w_x, w_x + n_u, w_y);
    
	    for (i = k = 0; i < n_u; ++i) {
		    int32_t j = (int32_t)w_y[i], n = (int32_t)u[j];
		    u2[i] = u[j];
		    memcpy(&ax[k], &b_x[w_y[i]>>32], n * sizeof(int64_t));
            memcpy(&ay[k], &b_y[w_y[i]>>32], n * sizeof(int64_t));
		    k += n;
        }
	    memcpy(u, u2, n_u * sizeof(int64_t));


    // 3. Generate alignment information - we only need this for the last chaining step
    // reuse z array
    if(final) { 
        //mm_reg1_t* r = &g_r[ofs];
        int qlen = d_len[job_idx];

        // pre filter n_u = 0 해야 한다. realigning data 
	    if (n_u == 0) {
            g_n_v[job_idx] = 0;
            n_a[job_idx] = 0;
            return;
        }

	    // sort by score
	    for (i = k = 0; i < n_u; ++i) {
		    uint32_t h;
		    h = (uint32_t)hash64((hash64(ax[k]) + hash64(ay[k])) ^ hash);
		    z_x[i] = u[i] ^ h; // u[i] -- higher 32 bits: chain score; lower 32 bits: number of seeds in the chain
		    z_y[i] = (uint64_t)k << 32 | (int32_t)u[i];
        
		    k += (int32_t)u[i];
	    }

        thrust::sort_by_key(thrust::seq, z_x, z_x + n_u, z_y, thrust::greater<uint64_t>()); 

        int c_ofs = atomicAdd(n_chain, (int)n_u); 

        // TODO: this is temporary. find other way to manage memory 
        g_n_v[job_idx] = n_u;
        n_a[job_idx] = c_ofs;


        for (i = 0; i < n_u; ++i) {
            mm_chain_t *c = &g_c[c_ofs + i];
            c->qlen = qlen;
            c->a_ofs = ofs;
            c->r_ofs = r_ofs;
            c->dropped = false;
            c->n_a = n_v;
            c->flag = 0;
            c->r_n = id;

            dropped[c_ofs+i] = false;

		    mm_reg1_t *ri = &c->r;
		    ri->id = i;
		    ri->parent = MM_PARENT_UNSET;
		    ri->score = ri->score0 = z_x[i] >> 32;
		    ri->hash = (uint32_t)z_x[i];
		    ri->cnt = (int32_t)z_y[i];
		    ri->as = z_y[i] >> 32;
		    ri->div = -1.0f;
		    mm_reg_set_coor(ri, qlen, ax, ay, is_qstrand);
            // printf("%d\t", ri->rid)
	    }
    }
    }
}

__device__
static inline int mm_alt_score(int score, float alt_diff_frac)
{
	if (score < 0) return score;
	score = (int)(score * (1.0 - alt_diff_frac) + .499);
	return score > 0? score : 1;
}

__global__
void mm_chain_post( // mm_mapopt_t *opt, 
            int max_chain_gap_ref, int* qlen, int *n_regs, int* c_ofs, int* a_ofs, mm_chain_t *g_c, uint64_t *ax, uint64_t* ay, bool* g_dropped, int* g_w, uint64_t* g_cov, int n_task)
{
    // TODO: add opt 
    
    int sub_diff = 8;
    float alt_diff_frac = 0.150000;
    float mask_level = 0.5;
    int mask_len = 2147483647;
    int id = threadIdx.x + blockIdx.x * blockDim.x;

    for(int job_idx = id; job_idx < n_task; job_idx += gridDim.x * blockDim.x) {

        
        int ofs = c_ofs[job_idx];
        int arr_ofs = a_ofs[job_idx];
        mm_chain_t* c = &g_c[ofs];
        bool* dropped = &g_dropped[ofs];
        int n = n_regs[job_idx];

        if(n <= 0) {
            continue;
            *dropped = true; //??
        }

        int hard_mask_level = 0;
        int* w = &g_w[arr_ofs];
        uint64_t* cov = &g_cov[arr_ofs];

	// if (!(opt->flag & MM_F_ALL_CHAINS)) { // don't choose primary mapping(s)
        if (true) {
		    // mm_set_parent 
            int i, j, k;
	        if (n <= 0) continue;
            for (i = 0; i < n; ++i) c[i].r.id = i; 
            w[0] = 0, c[0].r.parent = 0;
            for (i = 1, k = 1; i < n; ++i) {
		        mm_reg1_t *ri = &(c[i].r);
		        int si = ri->qs, ei = ri->qe, n_cov = 0, uncov_len = 0;
		        if (hard_mask_level) goto skip_uncov;
		        for (j = 0; j < k; ++j) { // traverse existing primary hits to find overlapping hits
			        mm_reg1_t *rp = &(c[w[j]].r);
			        int sj = rp->qs, ej = rp->qe;
			        if (ej <= si || sj >= ei) continue;
			        if (sj < si) sj = si;
			        if (ej > ei) ej = ei;
			        cov[n_cov++] = (uint64_t)sj<<32 | ej;
		        }
		        if (n_cov == 0) {
			        goto set_parent_test; // no overlapping primary hits; then i is a new primary hit
		        } else if (n_cov > 0) { // there are overlapping primary hits; find the length not covered by existing primary hits
			        int j, x = si;
			        thrust::sort(thrust::seq, cov, cov + n_cov); // ????
			        for (j = 0; j < n_cov; ++j) {
				        if ((int)(cov[j]>>32) > x) uncov_len += (cov[j]>>32) - x;
				        x = (int32_t)cov[j] > x? (int32_t)cov[j] : x;
			        }
			        if (ei > x) uncov_len += ei - x;
		        }
        skip_uncov:
		    for (j = 0; j < k; ++j) { // traverse existing primary hits again
			    mm_reg1_t *rp = &(c[w[j]].r);
			    int sj = rp->qs, ej = rp->qe, min, max, ol;
			    if (ej <= si || sj >= ei) continue; // no overlap
			    min = ej - sj < ei - si? ej - sj : ei - si;
			    max = ej - sj > ei - si? ej - sj : ei - si;
			    ol = si < sj? (ei < sj? 0 : ei < ej? ei - sj : ej - sj) : (ej < si? 0 : ej < ei? ej - si : ei - si); // overlap length; TODO: this can be simplified
			    if ((float)ol / min - (float)uncov_len / max > mask_level && uncov_len <= mask_len) { // then this is a secondary hit
				    int cnt_sub = 0, sci = ri->score;
				    ri->parent = rp->parent;
				    if (!rp->is_alt && ri->is_alt) sci = mm_alt_score(sci, alt_diff_frac);
				    rp->subsc = rp->subsc > sci? rp->subsc : sci;
				    if (ri->cnt >= rp->cnt) cnt_sub = 1;
				    if (rp->p && ri->p && (rp->rid != ri->rid || rp->rs != ri->rs || rp->re != ri->re || ol != min)) { // the last condition excludes identical hits after DP
					    sci = ri->p->dp_max;
					    if (!rp->is_alt && ri->is_alt) sci = mm_alt_score(sci, alt_diff_frac);
					    rp->p->dp_max2 = rp->p->dp_max2 > sci? rp->p->dp_max2 : sci;
					    if (rp->p->dp_max - ri->p->dp_max <= sub_diff) cnt_sub = 1;
				    }
				    if (cnt_sub) ++rp->n_sub;
				    break;
			    }
		    }
        set_parent_test:
		    if (j == k) w[k++] = i, ri->parent = i, ri->n_sub = 0;
	}

    // mm_select_sub

    float pri_ratio = 0.80;
    int min_diff = 30;
    int best_n = 5;
    int check_strand = 1;
    int min_strand_sc = 4000;

    if (pri_ratio > 0.0f && n > 0) {
		int n_2nd = 0;
		for (i = k = 0; i < n; ++i) {
			int p = c[i].r.parent;
			if (p == i || c[i].r.inv) { // primary or inversion
				c[k++] = c[i];
			} else if ((c[i].r.score >= c[p].r.score * pri_ratio || c[i].r.score + min_diff >= c[p].r.score) && n_2nd < best_n) {
				if (!(c[i].r.qs == c[p].r.qs && c[i].r.qe == c[p].r.qe && c[i].r.rid == c[p].r.rid && c[i].r.rs == c[p].r.rs && c[i].r.re == c[p].r.re)) // not identical hits
					c[k++] = c[i], ++n_2nd;
				// else if (c[i].r.p) free(r[i].p);
			} else if (check_strand && n_2nd < best_n && c[i].r.score > min_strand_sc && c[i].r.rev != c[p].r.rev) {
				c[i].r.strand_retained = 1;
				c[k++] = c[i], ++n_2nd;
			} // else if (r[i].p) free(r[i].p);
		}
		if (k != n) {
            // mm_sync_regs(km, k, r); // removing hits requires sync()
            int* tmp = &w[0];
            for (i = 0; i < n; ++i) tmp[i] = -1;
	        for (i = 0; i < k; ++i)
		        if (c[i].r.id >= 0) tmp[c[i].r.id] = i;
	        for (i = 0; i < n; ++i) {
		        mm_reg1_t *r = &c[i].r;
		        r->id = i;
		        if (r->parent == MM_PARENT_TMP_PRI)
			        r->parent = i;
		        else if (r->parent >= 0 && tmp[r->parent] >= 0)
			        r->parent = tmp[r->parent];
		        else r->parent = MM_PARENT_UNSET;
	        }
	        // mm_set_sam_pri(n_regs, regs); // TODO: add this function for complete sam output value
        }
        // deactivate discarded chain 
        for(i = k; i < n; i++){
            c[i].dropped = true; 
            dropped[i] = true;
        }
        n = k;
	}
        
	}
    }
}

/* RMQ chaining - no opt */

__device__
int32_t chain_rmq_score(const uint64_t aix, const uint64_t aiy, const uint64_t ajx, const uint64_t ajy, float chn_pen_gap, float chn_pen_skip, int max_dist, int32_t *exact, int32_t *width)
{
    int32_t dq = (int32_t)aiy - (int32_t)ajy, dr, dd, dg, q_span, sc;
    if(dq > max_dist) return INT32_MIN;
	dr = (int32_t)(aix - ajx);
	*width = dd = dr > dq? dr - dq : dq - dr;
	dg = dr < dq? dr : dq;
	q_span = ajy>>32&0xff;
	sc = q_span < dg? q_span : dg;
	if (exact) *exact = (dd == 0 && dg <= q_span);
	if (dd || dq > q_span) {
		float lin_pen, log_pen;
		lin_pen = chn_pen_gap * (float)dd + chn_pen_skip * (float)dg;
		log_pen = dd >= 1? mg_log2_device(dd + 1) : 0.0f; // mg_log2() only works for dd>=2
		sc -= (int)(lin_pen + .5f * log_pen);
	}
	return sc;
}

__device__
float chain_rmq_score_cmp_flt(const uint64_t aix, const uint64_t aiy, const uint64_t ajx, const uint64_t ajy, float chn_pen_gap, float chn_pen_skip, int max_dist, int32_t *exact, int32_t *width)
{
    int32_t dq = (int32_t)aiy - (int32_t)ajy, dr, dd, dg, q_span;
    float sc;
    if(dq > max_dist) return (float)INT32_MIN;
    if((int32_t)aiy <= (int32_t)ajy) return (float)INT32_MIN;
    //if(aiy <= ajy) return 0;
    if((aix>>32) != (ajx>>32)) return (float)INT32_MIN;
    if((int32_t)aix == (int32_t)ajx) return (float)INT32_MIN;
    dr = ((int32_t)aix - (int32_t)ajx);
	*width = dd = dr > dq? dr - dq : dq - dr;
	dg = dr < dq? dr : dq;
	q_span = ajy>>32&0xff;
    sc = 0;
    sc += 0.5 * chn_pen_gap * ((int32_t)ajx + (int32_t)ajy);
	if (exact) *exact = (dd == 0 && dg <= q_span);
    // sc += 0.5 * chn_pen_gap * ((int32_t)ajx + (int32_t)ajy);
	return sc;
}

__device__
double chain_rmq_score_cmp_double(const uint64_t aix, const uint64_t aiy, const uint64_t ajx, const uint64_t ajy, float chn_pen_gap, float chn_pen_skip, int max_dist, int32_t *exact, int32_t *width)
{
    int32_t dq = (int32_t)aiy - (int32_t)ajy, dr, dd, dg, q_span;
    double sc;
    if(dq > max_dist) return (double)INT32_MIN;
    if((int32_t)aiy <= (int32_t)ajy) return (double)INT32_MIN;
    //if(aiy <= ajy) return 0;
    if((aix>>32) != (ajx>>32)) return (double)INT32_MIN;
    if((int32_t)aix == (int32_t)ajx) return (double)INT32_MIN;
    dr = ((int32_t)aix - (int32_t)ajx);
	*width = dd = dr > dq? dr - dq : dq - dr;
	dg = dr < dq? dr : dq;
	q_span = ajy>>32&0xff;
    sc = 0;
    sc += 0.5 * chn_pen_gap * ((int32_t)ajx + (int32_t)ajy);
	if (exact) *exact = (dd == 0 && dg <= q_span);
    // sc += 0.5 * chn_pen_gap * ((int32_t)ajx + (int32_t)ajy);
	return sc;
}


__global__
void mm_compute_chain_range_rmq(uint64_t* x, int* offset, int* a_len, uint2* r, int* n_r, int max_dist_x, int n_task){


   int bid = blockIdx.x;
   int tid = threadIdx.x;

   for(int job_idx = bid; job_idx < n_task; job_idx += BLOCK_NUM) {

    int ofs = offset[job_idx];
    int alen = a_len[job_idx];
    
    bool front_link;
    bool back_link;

    __shared__ int st_cnt, end_cnt;
    __shared__ uint64_t corner[2];

    st_cnt = 0;
    end_cnt = 0;
    corner[0] = 0;
    corner[1] = 0;


   // goal: use thread local regs and shuffle instruction instead of shared memory
   uint64_t a, buf; // thread local buffer to store anchor

    for(int i = 0; i < alen; i += 32) {
       back_link = false;
       front_link = false;

       corner[1] = x[ofs + i + 32];

       a = x[ofs + i + tid];

       buf = __shfl_up_sync(0xFFFFFFFF, a, 1, 32);
       if(tid == 0){
        if(a < corner[0] + max_dist_x) front_link = true;
       }
       else if(a < buf + max_dist_x) front_link = true;
       if(i + tid ==0) front_link = false;

       buf = __shfl_down_sync(0xFFFFFFFF, a, 1, 32);
       if(tid == warpSize - 1){
        if(a > corner[1] - max_dist_x || corner[1] < max_dist_x) back_link = true;
        corner[0] = a;
       }
       else if(a > buf - max_dist_x || buf < max_dist_x) back_link = true;

        if(back_link && !front_link && i+tid < alen) {
            int a = atomicAdd(&st_cnt, 1);
            r[ofs + a].x = i + tid;
        }

        if((front_link && !back_link && i+tid < alen) || ((front_link && back_link) && i+tid==alen-1)) {
            int a = atomicAdd(&end_cnt, 1);
            r[ofs + a].y = i + tid;
        }    
        __syncwarp();
   }

   n_r[job_idx] = st_cnt;
   
   }
}

__global__ 
void mm_compute_chain_short_rmq(int32_t* sc, int64_t* p, const uint64_t* anchors_x, const uint64_t* anchors_y, const uint2* range, 
                    int* offset, int* r_len, int* a_len, 
                    int max_dist_x, int max_dist_y, int bw, int is_cdna, float chn_pen_gap, float chn_pen_skip, int n_task, int* long_num, int* long_buf_st, int* long_buf_len, int* mid_num, int* mid_buf_st, int* mid_buf_len){
    
    int tid = threadIdx.x;
    int bid = blockIdx.x;

    for(int job_idx = bid; job_idx < n_task; job_idx += gridDim.x) {

    int start_idx = offset[job_idx];
    int end_idx = start_idx + r_len[job_idx];

    // init f and p
    for (int i = start_idx + tid; i < start_idx + a_len[job_idx]; i += blockDim.x) {
        sc[i] = INT32_MIN;
        //reinterpret_cast<float&>(sc[i]) = (float) INT32_MIN;
        p[i] = -1;
    }
    
    __syncthreads();

    for (size_t i = start_idx; i < end_idx; i++) {
            int32_t st = start_idx + range[i].x;
            int32_t end = start_idx + range[i].y;

            if(end-st > LONG_RANGE_CUT) {
                if(tid==0){
                    int long_idx = atomicAdd(long_num,1);
                    long_buf_st[long_idx] = st;
                    long_buf_len[long_idx] = end - st;
                }
                continue;
            } 
            else if(end-st > MID_RANGE_CUT){
                if(tid==0){
                    int mid_idx = atomicAdd(mid_num,1);
                    mid_buf_st[mid_idx] = st;
                    mid_buf_len[mid_idx] = end - st;
                }
                continue;
            }

            sc[st] = MM_QSPAN; // for the very first anchor in range
            p[st] = -1;

            __syncthreads();

            while(st < end) {
                int exact, width;

                int32_t range_i = (end - st < MAX_ITER_CHAIN)? end - st : MAX_ITER_CHAIN; 
                for (int32_t j = tid; j < range_i; j += blockDim.x) {
                    if(anchors_x[st] + max_dist_x < anchors_x[st+j+1]) break;

                    double score = chain_rmq_score_cmp_double(
                                        anchors_x[st+j+1], 
                                        anchors_y[st+j+1], 
                                        anchors_x[st], 
                                        anchors_y[st], 
                                        chn_pen_gap, 
                                    chn_pen_skip, max_dist_y, &exact, &width);

                    if (score == (double)INT32_MIN) {
                        continue; 
                    }
                    
                    score += static_cast<double>(sc[st]); // int MM_QSPAN 써 있는 경우에 대해 처리 해야 함

                    if (score >= reinterpret_cast<float&>(sc[st+j+1])) {
                        reinterpret_cast<float&>(sc[st+j+1]) = static_cast<float>(score);
                        p[st+j+1] = j+1;
                    }
                }
                __syncthreads();
                st++;

                if(tid==0){
                    // rewrite score as int32_t (final score)
                    if(p[st]>0){
                        int32_t score_ = chain_rmq_score(
                                        anchors_x[st], 
                                        anchors_y[st], 
                                        anchors_x[st - p[st]], 
                                        anchors_y[st - p[st]], 
                                        chn_pen_gap, 
                                    chn_pen_skip, max_dist_y, &exact, &width);
                        
                        score_ += sc[st - p[st]];
                        if(score_ > MM_QSPAN) {
                            sc[st] = score_;
                        }else {
                            sc[st] = MM_QSPAN;
                            p[st] = -1;
                        }
                    }else{
                        sc[st] = MM_QSPAN;
                        p[st] = -1;
                    }
                }
                __syncthreads();
            }
            __syncthreads();
        }
        __syncthreads();
    }
}

__global__ 
void mm_chain_compute_long_tiled_rmq(int32_t* sc, int64_t* p, const uint64_t* anchors_x, const uint64_t* anchors_y, const int* range_st, const int* range_len, 
                    int* offset, int* a_len, 
                    int max_dist_x, int max_dist_y, int bw, int is_cdna, float chn_pen_gap, float chn_pen_skip, int n_task){
    // const Misc blk_misc = misc;
    int tid = threadIdx.x;
    int bid = blockIdx.x;

    // shared memory
    __shared__ uint64_t ax_buf[shmem_size];
    __shared__ uint64_t ay_buf[shmem_size];
    __shared__ int32_t sc_buf[shmem_size];
    __shared__ int16_t p_buf[shmem_size];

    // thread local registers TODO: change to store more info?
    uint64_t ax_reg;
    uint64_t ay_reg;
    int32_t sc_reg;
    int16_t p_reg;
    int32_t st, end, r;
    int j, k;

    __shared__ int job_idx;

    if (tid == 0 && bid == 0) {
        job_idx_global = gridDim.x;
    }
    if (tid == 0) {
        job_idx = bid;
    }
    __syncthreads();

    // compute chaining score 
    while(job_idx < n_task){
        st = range_st[job_idx];
        end = st + range_len[job_idx];
        int exact, width;
    
        sc[st] = MM_QSPAN; // for the very first anchor in range
        p[st] = -1; 
        
        for(;st < end; st += shmem_size){

            r = (end - st < MAX_ITER_CHAIN + shmem_size)? end - st : MAX_ITER_CHAIN + shmem_size; 

            // fill shared memory for prologue
            for(j = tid; j <= r && j < shmem_size; j+= blockDim.x) {
                ax_buf[j] = anchors_x[st+j];
                ay_buf[j] = anchors_y[st+j];
                sc_buf[j] = sc[st+j];
                p_buf[j] = p[st+j];
            }

            __syncthreads();


            // compute prologue
            for(j = 0; j <= r && j < shmem_size; j++) { // predecessor

                if(tid == 0) {// write final score
                    if(p_buf[j]>0){
                        if(j - p_buf[j] >= 0){
                            int32_t score_ = chain_rmq_score(
                                    ax_buf[j], 
                                    ay_buf[j], 
                                    ax_buf[j - p_buf[j]], 
                                    ay_buf[j - p_buf[j]], // TODO: what if parent anchor not in buffer range?
                                    chn_pen_gap, 
                                    chn_pen_skip, max_dist_y, &exact, &width);
                            
                            score_ += sc_buf[j - p_buf[j]];
                            if(score_ > MM_QSPAN) {
                                sc_buf[j] = score_;
                            }else {
                                sc_buf[j] = MM_QSPAN;
                                p_buf[j] = -1;
                            }
                        }
                        else{
                            // if parent anchor is not in buffer; turn to gmem (tmp sol)
                            int32_t score_ = chain_rmq_score(
                                    ax_buf[j], 
                                    ay_buf[j], 
                                    anchors_x[st + j - p_buf[j]],
                                    anchors_y[st + j - p_buf[j]],
                                    chn_pen_gap, 
                                    chn_pen_skip, max_dist_y, &exact, &width);

                            score_ += sc[st + j - p_buf[j]];
                            if(score_ > MM_QSPAN) {
                                sc_buf[j] = score_;
                            }else {
                                sc_buf[j] = MM_QSPAN;
                                p_buf[j] = -1;
                            }
                        }
                    }else {
                        sc_buf[j] = MM_QSPAN;
                        p_buf[j] = -1;
                    }
                    
                }
                __syncthreads();
                for(int32_t k = j + tid; k < r && k < shmem_size - 1; k+= blockDim.x){

                    float score = chain_rmq_score_cmp_flt(
                                    ax_buf[k+1], 
                                    ay_buf[k+1], 
                                    ax_buf[j], 
                                    ay_buf[j],
                                    chn_pen_gap, 
                                    chn_pen_skip, 
                                    max_dist_y, &exact, &width);
            
                    if (score == (float)INT32_MIN) {
                        continue; // TODO: plz change this: to what?
                    }
                
                    score += static_cast<float>(sc_buf[j]); 

                    if (score >= reinterpret_cast<float&>(sc_buf[k+1]) && score > MM_QSPAN) {
                        reinterpret_cast<float&>(sc_buf[k+1]) = (score);
                        p_buf[k+1] = k-j+1;
                    }
                }
                __syncthreads();
              
            }

            //Write prologue (final integer scores)
            for(j = tid; j <= r && j < shmem_size; j+= blockDim.x) {
                if(p_buf[j]>=0){
                    sc[st + j] = sc_buf[j];
                    p[st + j] = p_buf[j];
                }
                else{
                    sc[st + j] = MM_QSPAN;
                    p[st + j] = -1;
                }
            }
            __syncthreads();

            // tail updates
            for(j = st + shmem_size; j <= st + r; j += blockDim.x){    
                // load to register 
                ax_reg = anchors_x[j+tid];
                ay_reg = anchors_y[j+tid];
                sc_reg = sc[j+tid];
                p_reg = p[j+tid];

                if(ax_buf[shmem_size-1] + max_dist_x < ax_reg || st + shmem_size + MAX_ITER_CHAIN < j + tid) break;

                // compute chain score
                for(k = 0; k < shmem_size; k++){

                    int idx = (tid + k) & 0x3FF;
                    float score = chain_rmq_score_cmp_flt(
                                    ax_reg, 
                                    ay_reg,
                                    ax_buf[idx], 
                                    ay_buf[idx],
                                    chn_pen_gap, 
                                    chn_pen_skip, 
                                    max_dist_y, &exact, &width);
                    if (score == (float)INT32_MIN) {
                        continue; 
                    }
                    score += static_cast<float>(sc_buf[idx]);

                    if (score >= reinterpret_cast<float&>(sc_reg) && score > MM_QSPAN) {
                        reinterpret_cast<float&>(sc_reg) = (score);
                        p_reg = j + tid - (st + idx);
                    }

                }
                // write chain score 
                if(p_reg > 0 && p_reg <= MAX_ITER_CHAIN) {
                    sc[j+tid] = sc_reg;
                    p[j+tid] = p_reg; // float score written to gmem
                }
            }
            __syncthreads();

        } // in-range iteration
        if (tid == 0) job_idx = atomicAdd(&job_idx_global, 1);
        __syncthreads();
    } // range iteration
}

__global__ 
void mm_chain_compute_long_tiled_rmq_double(int32_t* sc, int64_t* p, const uint64_t* anchors_x, const uint64_t* anchors_y, const int* range_st, const int* range_len, 
                    int* offset, int* a_len, 
                    int max_dist_x, int max_dist_y, int bw, int is_cdna, float chn_pen_gap, float chn_pen_skip, int n_task){
    // const Misc blk_misc = misc;
    int tid = threadIdx.x;
    int bid = blockIdx.x;

    // shared memory
    __shared__ uint64_t ax_buf[shmem_size];
    __shared__ uint64_t ay_buf[shmem_size];
    __shared__ int32_t sc_buf[shmem_size];
    __shared__ int16_t p_buf[shmem_size];

    // thread local registers TODO: change to store more info?
    uint64_t ax_reg;
    uint64_t ay_reg;
    int32_t sc_reg;
    int16_t p_reg;
    int32_t st, end, r;
    int j, k;

    __shared__ int job_idx;

    if (tid == 0 && bid == 0) {
        job_idx_global = gridDim.x;
    }
    if (tid == 0) {
        job_idx = bid;
    }
    __syncthreads();

    // compute chaining score 
    while(job_idx < n_task){
        st = range_st[job_idx];
        end = st + range_len[job_idx];
        int exact, width;
    
        sc[st] = MM_QSPAN; // for the very first anchor in range
        p[st] = -1; 
        
        for(;st < end; st += shmem_size){

            r = (end - st < MAX_ITER_CHAIN + shmem_size)? end - st : MAX_ITER_CHAIN + shmem_size; 

            // fill shared memory for prologue
            for(j = tid; j <= r && j < shmem_size; j+= blockDim.x) {
                ax_buf[j] = anchors_x[st+j];
                ay_buf[j] = anchors_y[st+j];
                sc_buf[j] = sc[st+j];
                p_buf[j] = p[st+j];
            }

            __syncthreads();


            // compute prologue
            for(j = 0; j <= r && j < shmem_size; j++) { // predecessor

                if(tid == 0) {// write final score
                    if(p_buf[j]>0){
                        if(j - p_buf[j] >= 0){
                            int32_t score_ = chain_rmq_score(
                                    ax_buf[j], 
                                    ay_buf[j], 
                                    ax_buf[j - p_buf[j]], 
                                    ay_buf[j - p_buf[j]], // TODO: what if parent anchor not in buffer range?
                                    chn_pen_gap, 
                                    chn_pen_skip, max_dist_y, &exact, &width);
                            
                            score_ += sc_buf[j - p_buf[j]];
                            if(score_ > MM_QSPAN) {
                                sc_buf[j] = score_;
                            }else {
                                sc_buf[j] = MM_QSPAN;
                                p_buf[j] = -1;
                            }
                        }
                        else{
                            // if parent anchor is not in buffer; turn to gmem (tmp sol)
                            int32_t score_ = chain_rmq_score(
                                    ax_buf[j], 
                                    ay_buf[j], 
                                    anchors_x[st + j - p_buf[j]],
                                    anchors_y[st + j - p_buf[j]],
                                    chn_pen_gap, 
                                    chn_pen_skip, max_dist_y, &exact, &width);

                            score_ += sc[st + j - p_buf[j]];
                            if(score_ > MM_QSPAN) {
                                sc_buf[j] = score_;
                            }else {
                                sc_buf[j] = MM_QSPAN;
                                p_buf[j] = -1;
                            }
                        }
                    }else {
                        sc_buf[j] = MM_QSPAN;
                        p_buf[j] = -1;
                    }
                    
                }
                __syncthreads();
                for(int32_t k = j + tid; k < r && k < shmem_size - 1; k+= blockDim.x){

                    double score = chain_rmq_score_cmp_double(
                                    ax_buf[k+1], 
                                    ay_buf[k+1], 
                                    ax_buf[j], 
                                    ay_buf[j],
                                    chn_pen_gap, 
                                    chn_pen_skip, 
                                    max_dist_y, &exact, &width);

            
                    if (score == (double)INT32_MIN) {
                        continue; 
                    }
                
                    score += static_cast<double>(sc_buf[j]); 

                    if (score >= reinterpret_cast<float&>(sc_buf[k+1]) && score > MM_QSPAN) {
                        reinterpret_cast<float&>(sc_buf[k+1]) = static_cast<float>(score);
                        p_buf[k+1] = k-j+1;
                    }
                }
                __syncthreads();
              
            }

            //Write prologue (final integer scores)
            for(j = tid; j <= r && j < shmem_size; j+= blockDim.x) {
                if(p_buf[j]>=0){
                    sc[st + j] = sc_buf[j];
                    p[st + j] = p_buf[j];
                }
                else{
                    sc[st + j] = MM_QSPAN;
                    p[st + j] = -1;
                }
            }
            __syncthreads();

            // tail updates
            for(j = st + shmem_size; j <= st + r; j += blockDim.x){    
                // load to register 
                ax_reg = anchors_x[j+tid];
                ay_reg = anchors_y[j+tid];
                sc_reg = sc[j+tid];
                p_reg = p[j+tid];

                if(ax_buf[shmem_size-1] + max_dist_x < ax_reg || st + shmem_size + MAX_ITER_CHAIN < j + tid) break;

                // compute chain score
                for(k = 0; k < shmem_size; k++){

                    int idx = (tid + k) & 0x3FF;
                    
                    double score = chain_rmq_score_cmp_double(
                                    ax_reg, 
                                    ay_reg,
                                    ax_buf[idx], 
                                    ay_buf[idx],
                                    chn_pen_gap, 
                                    chn_pen_skip, 
                                    max_dist_y, &exact, &width);
                    
                    if (score == (double)INT32_MIN) {
                        continue; 
                    }
                    score += static_cast<float>(sc_buf[idx]);

                    if (score >= reinterpret_cast<float&>(sc_reg) && score > MM_QSPAN) {
                        reinterpret_cast<float&>(sc_reg) = static_cast<float>(score);
                        p_reg = j + tid - (st + idx);
                    }

                }
                // write chain score 
                if(p_reg > 0 && p_reg <= MAX_ITER_CHAIN) {
                    sc[j+tid] = sc_reg;
                    p[j+tid] = p_reg; // float score written to gmem
                }
            }
            __syncthreads();

        } // in-range iteration
        if (tid == 0) job_idx = atomicAdd(&job_idx_global, 1);
        __syncthreads();
    } // range iteration
}


/* Chain backtracking kernels */

__global__ void mm_filter_anchors(int* n_a, int* offset, int32_t min_sc, int32_t* g_sc, int64_t* g_zx, int64_t* g_zy, 
                                int* ofs_out, int32_t* g_t,  int* g_n_z, int* g_n_v, int n_task){

    int id = threadIdx.x + blockIdx.x * blockDim.x;
    for(int job_idx = id; job_idx < n_task; job_idx += gridDim.x * blockDim.x) {
    
        int ofs = offset[job_idx];
        int n = n_a[job_idx];
        int32_t* f = &g_sc[ofs];

        int64_t* z_x = &g_zx[ofs]; // b.x
        int64_t* z_y = &g_zy[ofs]; // b.y
        int32_t* t = &g_t[ofs]; // u2
    
	    int64_t i, j, k, n_z = 0;

        // initialize t (visited) array
        for(j = 0; j < n; j++) t[j]=0;

	    for (i = 0, k = 0; i < n; ++i) {// populate z[]
		    if (f[i] >= min_sc) {
                ++n_z;
                z_x[k] = (int64_t)f[i];
                z_y[k++] = i;
            }
        }
    
        g_n_z[job_idx] = n_z;
        ofs_out[job_idx] = ofs + n_z;

        if(n_z <= 0) {
            g_n_v[id] = 0;
            continue;
        }

    }
}

__global__ 
void mm_chain_backtrack(int* n_a, uint64_t* g_ax, uint64_t* g_ay, int32_t* g_sc, int64_t *g_p, uint64_t* g_u, 
    int64_t* g_zx, int64_t* g_zy, int32_t* g_t, int64_t* g_v,
    int* offset, int32_t min_cnt, int32_t min_sc, int32_t max_drop,
    int n_task, int* g_n_v, int* g_n_u, int* g_n_z, int* ofs_end){

    int id = threadIdx.x + blockIdx.x * blockDim.x;
    for(int job_idx = id; job_idx < n_task; job_idx += gridDim.x * blockDim.x) {

        int ofs = offset[job_idx];

        int n_z = g_n_z[job_idx];
        uint64_t* u = &g_u[ofs];
        int32_t* t = &g_t[ofs]; // u2
        int64_t* v = &g_v[ofs]; // w.y
        int n_v, n_u, i, j, k;
        int n = n_a[job_idx];

        int64_t* z_x = &g_zx[ofs]; // b.x
        int64_t* z_y = &g_zy[ofs]; // b.y
        uint64_t *ax = &g_ax[ofs];
        uint64_t *ay = &g_ay[ofs];
        int32_t* f = &g_sc[ofs];
        int64_t* p = &g_p[ofs]; // w.x


        for (k = n_z - 1, n_v = n_u = 0; k >= 0; --k) { // populate u[]
            if (t[z_y[k]] == 0) {
                int64_t n_v0 = n_v, end_i;
                int32_t sc;
                end_i = mg_chain_bk_end(max_drop, z_x, z_y, f, p, t, k);
                for (i = z_y[k]; i != end_i; i = (p[i]<0)? p[i] : i - p[i]) // parent array modified
                    v[n_v++] = i, t[i] = 1;
                sc = i < 0? (int32_t)z_x[k] : (int32_t)z_x[k] - f[i];
                if (sc >= min_sc && n_v > n_v0 && n_v - n_v0 >= min_cnt)
                    u[n_u++] = (uint64_t)sc << 32 | (n_v - n_v0);
                else n_v = n_v0;
            }
        }

        assert(n_v <= n); // not violated in most cases; but need to be fixed
        g_n_v[job_idx] = n_v;
        g_n_u[job_idx] = n_u;

        // // 2. Sort & compact chaining results

        // reusing allocated space

        int64_t *b_x = z_x;
        int64_t *b_y = z_y;
        int64_t *w_x = p;
        int64_t *w_y = v;
        int ni;

        for (i = 0, k = 0; i < n_u; ++i) {
            int32_t k0 = k, ni = (int32_t)u[i];
            for (j = 0; j < ni; ++j){
                b_x[k] = ax[v[k0 + (ni - j - 1)]];
                b_y[k++] = ay[v[k0 + (ni - j - 1)]];
            }
        } // This could also be parallelized

        // sort u[] and a[] by the target position, such that adjacent chains may be joined
        for (i = k = 0; i < n_u; ++i) {
            w_x[i] = b_x[k], w_y[i] = (uint64_t)k<<32|i;
            k += (int32_t)u[i];
        } // This could also be parallelized

        ofs_end[job_idx] = ofs + n_u;
    }
}

__global__ void mm_chain_backtrack_parallel(int* n_a, uint64_t* g_ax, uint64_t* g_ay, int32_t* g_sc, int64_t *g_p, uint64_t* g_u, 
                    int64_t* g_zx, int64_t* g_zy, int32_t* g_t, int64_t* g_v,
                    int* offset, int32_t min_cnt, int32_t min_sc, int32_t max_drop,
                    int n_task, int* g_n_v, int* g_n_u, int* g_n_z, int* ofs_end){

    int bid = blockIdx.x;
    int tid = threadIdx.x;
    for(int job_idx = bid; job_idx < n_task; job_idx += gridDim.x) {

        int ofs = offset[job_idx];
        
        int n_z = g_n_z[job_idx];
        uint64_t* u = &g_u[ofs];
        int32_t* t = &g_t[ofs]; // u2
        int64_t* v = &g_v[ofs]; // w.y
        int n_v, n_u, i, j, k;
        int n = n_a[job_idx];

        int64_t* z_x = &g_zx[ofs]; // b.x
        int64_t* z_y = &g_zy[ofs]; // b.y
        uint64_t *ax = &g_ax[ofs];
        uint64_t *ay = &g_ay[ofs];
        int32_t* f = &g_sc[ofs];
        int64_t* p = &g_p[ofs]; // w.x

        //if(tid==0){
        for (k = n_z - 1, n_v = n_u = 0; k >= 0; --k) { // populate u[]
		    if (t[z_y[k]] == 0) {
			    int64_t n_v0 = n_v, end_i;
			    int32_t sc;
			    end_i = mg_chain_bk_end(max_drop, z_x, z_y, f, p, t, k);
			    for (i = z_y[k]; i != end_i; i = (p[i]<0)? p[i] : i - p[i]) // parent array modified
				    v[n_v++] = i, t[i] = 1;
			    sc = i < 0? (int32_t)z_x[k] : (int32_t)z_x[k] - f[i];
			    if (sc >= min_sc && n_v > n_v0 && n_v - n_v0 >= min_cnt)
				    u[n_u++] = (uint64_t)sc << 32 | (n_v - n_v0);
			    else n_v = n_v0;
		    }
	    }

       assert(n_v <= n); // not violated in most cases; but need to be fixed
        g_n_v[job_idx] = n_v;
        g_n_u[job_idx] = n_u;

        //return;

        // // 2. Sort & compact chaining results

        // reusing allocated space
        
        int64_t *b_x = z_x;
        int64_t *b_y = z_y;
        int64_t *w_x = p;
        int64_t *w_y = v;
        int64_t *u2 = (int64_t*)t;
        int ni;

        __syncthreads();

        for (i = 0, k = 0; i < n_u; ++i) {
		    int32_t k0 = k, ni = (int32_t)u[i];
            for(j = tid; j < ni; j += blockDim.x){
                b_x[k + j] = ax[v[k0 + (ni-j-1)]];
                b_y[k + j] = ay[v[k0 + (ni-j-1)]];
            }
            k += ni;
            __syncthreads();
	    } 

        if(tid ==0) {
        // preparing to sort u[] and a[] by the target position, such that adjacent chains may be joined
	    
        for (i = k = 0; i < n_u; ++i) {
		    w_x[i] = b_x[k], w_y[i] = (uint64_t)k<<32|i;
		    k += (int32_t)u[i];
	    } // This could also be parallelized but not significant

        ofs_end[job_idx] = ofs + n_u;
        }
    }
}


__global__ void mm_set_chain(int* g_na, int n_task,  uint64_t* g_ax, uint64_t* g_ay, int* offset, int* ofs_end, int32_t* g_sc, int64_t *g_p, uint64_t* g_u, 
    int64_t* g_zx, int64_t* g_zy, int32_t* g_t, int64_t* g_v){

    int tid = threadIdx.x;
    int bid = blockIdx.x;
    for(int job_idx = bid; job_idx < n_task; job_idx += gridDim.x) {
        int i,k,j;
        int ofs = offset[job_idx];
        int n_u = ofs_end[job_idx] - ofs;
        int64_t *b_x = &g_zx[ofs];
        int64_t *b_y = &g_zy[ofs];
        int64_t *w_x = &g_p[ofs];
        int64_t *w_y = &g_v[ofs];
        int64_t *u2 = (int64_t*)(&g_t[ofs]);
        uint64_t* u = &g_u[ofs];
        uint64_t *ax = &g_ax[ofs];
        uint64_t *ay = &g_ay[ofs];
        int n_a = g_na[job_idx]; 

        for (i = k = 0; i < n_u; ++i) {
            int32_t j = (int32_t)w_y[i], n = (int32_t)u[j];
            if(tid==0) u2[i] = u[j];
            for(int x = tid; x < n; x += blockDim.x){
            ax[k+x] = b_x[(w_y[i]>>32)+x]; // fill ax and ay with zero for rmq chaining
            ay[k+x] = b_y[(w_y[i]>>32)+x];
            }
            k += n;
        }
        __syncthreads();

        for(int x = tid + k; x < n_a; x += blockDim.x){
            ax[x] = (uint64_t)0;
            ay[x] = (uint64_t)0;
        }

        __syncthreads();

        for(int x = tid; x < n_u; x += blockDim.x){
            u[x] = u2[x];
        }
        __syncthreads();
    }
}

__global__ void mm_gen_regs(int* n_a, uint64_t* g_ax, uint64_t* g_ay, uint64_t* g_u, 
                    int64_t* g_zx, int64_t* g_zy, int64_t* g_v, int* g_n_v, int* ofs_end, uint32_t hash,
                    int* offset, int* r_offset, int* d_len, int is_qstrand, int* n_chain, mm_chain_t* g_c, int n_task, bool* dropped){

    int id = threadIdx.x + blockIdx.x * blockDim.x;
    for(int job_idx = id; job_idx < n_task; job_idx += gridDim.x * blockDim.x) {

        int i, k;
        int ofs = offset[job_idx];
        int r_ofs = r_offset[job_idx];
        int n_v = g_n_v[job_idx];
        int n_u = ofs_end[job_idx] - ofs;
        uint64_t *ax = &g_ax[ofs];
        uint64_t *ay = &g_ay[ofs];
        int qlen = d_len[job_idx];

        int64_t* z_x = &g_zx[ofs]; 
        int64_t* z_y = &g_zy[ofs]; 

        uint64_t* u = &g_u[ofs];

        // pre filter n_u = 0 realigning data 
	    if (n_u == 0) {
            g_n_v[job_idx] = 0;
            n_a[job_idx] = 0;
            continue;
        }
        

	    // sort by score
	    for (i = k = 0; i < n_u; ++i) {
		    uint32_t h;
		    h = (uint32_t)hash64((hash64(ax[k]) + hash64(ay[k])) ^ hash);
		    z_x[i] = u[i] ^ h; // u[i] -- higher 32 bits: chain score; lower 32 bits: number of seeds in the chain
		    z_y[i] = (uint64_t)k << 32 | (int32_t)u[i];
        
		    k += (int32_t)u[i];
	    }
        //continue;
        thrust::sort_by_key(thrust::seq, z_x, z_x + n_u, z_y, thrust::greater<uint64_t>()); 

        
        int c_ofs = atomicAdd(n_chain, (int)n_u); 
 
        g_n_v[job_idx] = n_u;
        n_a[job_idx] = c_ofs;

       
        for (i = 0; i < n_u; ++i) {
            mm_chain_t *c = &g_c[c_ofs + i];
            c->qlen = qlen;
            c->a_ofs = ofs;
            c->r_ofs = r_ofs;
            c->dropped = false;
            c->n_a = n_v;
            c->flag = 0;
            c->r_n = job_idx;

            dropped[c_ofs+i] = false;

		    mm_reg1_t *ri = &c->r;
		    ri->id = i;
		    ri->parent = MM_PARENT_UNSET;
		    ri->score = ri->score0 = z_x[i] >> 32;
		    ri->hash = (uint32_t)z_x[i];
		    ri->cnt = (int32_t)z_y[i];
		    ri->as = z_y[i] >> 32;
		    ri->div = -1.0f;
		    mm_reg_set_coor(ri, qlen, ax, ay, is_qstrand);
	    }
    }
}

static __device__ __forceinline__
uint32_t __ac_Wang_hash(uint32_t key) {
    key += ~(key << 15);
    key ^=  (key >> 10);
    key +=  (key << 3);
    key ^=  (key >> 6);
    key += ~(key << 11);
    key ^=  (key >> 16);
    return key;
}

__global__ void mm_gen_regs_dp(int* n_a, uint64_t* g_ax, uint64_t* g_ay, uint64_t* g_u, 
    int64_t* g_zx, int64_t* g_zy, int64_t* g_v, int* g_n_v, int* g_n_u, int* ofs_end, 
    int* offset, int* r_offset, int* d_len, int is_qstrand, int* n_chain, mm_chain_t* g_c, int n_task, bool* dropped){

    int id = threadIdx.x + blockIdx.x * blockDim.x;

    int rmq_rescue_size = 1000;
    float rmq_rescue_ratio = 0.10; 

    for(int job_idx = id; job_idx < n_task; job_idx += gridDim.x * blockDim.x) {

        int i, k;
        int ofs = offset[job_idx];
        int r_ofs = r_offset[job_idx];
        int n = n_a[job_idx];
        int* n_v = &g_n_v[job_idx];
        int n_u = ofs_end[job_idx] - ofs; // maybe we don't have to explicitly store n u? lets see
        uint64_t *ax = &g_ax[ofs];
        uint64_t *ay = &g_ay[ofs];
        int qlen = d_len[job_idx];

        int64_t* z_x = &g_zx[ofs]; 
        int64_t* z_y = &g_zy[ofs]; 

        uint64_t* u = &g_u[ofs];

        // pre filter n_u = 0 realigning data 
        if (n_u == 0) {
            g_n_v[job_idx] = 0;
            n_a[job_idx] = 0;
            continue;
        }
        int tmp_n_v = *n_v;
        *n_v = 0;
        *n_v |= (tmp_n_v << 1);

        if(*n_v > 0){ // filter out anchors that don't need rmq chaining . . .
            int32_t st = (int32_t)ay[0];
            int32_t en = (int32_t)ay[(int32_t)u[0] - 1];

            if (!(qlen - (en - st) > rmq_rescue_size || en - st > qlen * rmq_rescue_ratio) || n_u == 1) {
               
                *n_v |= 1;
                // only for chains that has been filtered out
                int opt_seed = 11; //TODO: fix this to read from global opt
                uint32_t hash = 0;
                hash ^= __ac_Wang_hash(qlen) + __ac_Wang_hash(opt_seed);
                hash  = __ac_Wang_hash(hash);
                // sort by score
                for (i = k = 0; i < n_u; ++i) {
                    uint32_t h;
                    h = (uint32_t)hash64((hash64(ax[k]) + hash64(ay[k])) ^ hash);
                    z_x[i] = u[i] ^ h; // u[i] -- higher 32 bits: chain score; lower 32 bits: number of seeds in the chain
                    z_y[i] = (uint64_t)k << 32 | (int32_t)u[i];

                    k += (int32_t)u[i];
                }
                
                thrust::sort_by_key(thrust::seq, z_x, z_x + n_u, z_y, thrust::greater<uint64_t>()); 

                int c_ofs = atomicAdd(n_chain, (int)n_u); 

                g_n_u[job_idx] = n_u;
                n_a[job_idx] = c_ofs;

                for (i = 0; i < n_u; ++i) {
                    mm_chain_t *c = &g_c[c_ofs + i];
                    c->qlen = qlen;
                    c->a_ofs = ofs;
                    c->r_ofs = r_ofs;
                    c->dropped = false;
                    c->n_a = ((*n_v) >> 1);
                    c->flag = 0;
                    c->r_n = job_idx;

                    dropped[c_ofs+i] = false;

                    mm_reg1_t *ri = &c->r;
                    ri->id = i;
                    ri->parent = MM_PARENT_UNSET;
                    ri->score = ri->score0 = z_x[i] >> 32;
                    ri->hash = (uint32_t)z_x[i];
                    ri->cnt = (int32_t)z_y[i];
                    ri->as = z_y[i] >> 32;
                    ri->div = -1.0f;
                    mm_reg_set_coor(ri, qlen, ax, ay, is_qstrand);
                }
            }
        }
    }
}

__global__ void mm_gen_regs1(int* n_a, uint64_t* g_ax, uint64_t* g_ay, uint64_t* g_u, 
                    int64_t* g_zx, int64_t* g_zy, int64_t* g_v, int* g_n_v, int* ofs_end,
                    int* offset, int* r_offset, int* d_len, int is_qstrand, int* n_chain, mm_chain_t* g_c, int n_task, bool* dropped){

    int tid = threadIdx.x;
    int bid = blockIdx.x;
    int id = threadIdx.x + blockIdx.x * blockDim.x;

    for(int job_idx = id; job_idx < n_task; job_idx += gridDim.x * blockDim.x) {

        int i, k;
        int ofs = offset[job_idx];
        int r_ofs = r_offset[job_idx];
        int n = n_a[job_idx];
        int n_v = g_n_v[job_idx];
        int n_u = ofs_end[job_idx] - ofs;
        uint64_t *ax = &g_ax[ofs];
        uint64_t *ay = &g_ay[ofs];
        int qlen = d_len[job_idx];

        int64_t* z_x = &g_zx[ofs]; 
        int64_t* z_y = &g_zy[ofs]; 

        uint64_t* u = &g_u[ofs];

        int opt_seed = 11; //TODO: fix this to read from global opt


        // pre filter n_u = 0 realigning data 
	    if (n_u == 0) {
            g_n_v[job_idx] = 0;
            n_a[job_idx] = 0;
            continue;
        }
        
        uint32_t hash = 0;
        hash ^= __ac_Wang_hash(qlen) + __ac_Wang_hash(opt_seed);
	    hash  = __ac_Wang_hash(hash);

	    // sort by score
	    for (i = k = 0; i < n_u; ++i) {
		    uint32_t h;
		    h = (uint32_t)hash64((hash64(ax[k]) + hash64(ay[k])) ^ hash);
		    z_x[i] = u[i] ^ h; // u[i] -- higher 32 bits: chain score; lower 32 bits: number of seeds in the chain
		    z_y[i] = (uint64_t)k << 32 | (int32_t)u[i];
		    k += (int32_t)u[i];
	    }
    }
}

__global__ void mm_gen_regs2(int* n_a, uint64_t* g_ax, uint64_t* g_ay, uint64_t* g_u, 
    int64_t* g_zx, int64_t* g_zy, int64_t* g_v, int* g_n_v, int* g_n_u, int* ofs_end, 
    int* offset, int* r_offset, int* d_len, int is_qstrand, int* n_chain, mm_chain_t* g_c, int n_task, bool* dropped){

    int tid = threadIdx.x;
    int bid = blockIdx.x;
    int id = threadIdx.x + blockIdx.x * blockDim.x;
    for(int job_idx = id; job_idx < n_task; job_idx += gridDim.x * blockDim.x) {

        int i, k;
        int ofs = offset[job_idx];
        int r_ofs = r_offset[job_idx];
        int n = n_a[job_idx];
        int n_v = g_n_v[job_idx];
        int n_u = ofs_end[job_idx] - ofs;
        uint64_t *ax = &g_ax[ofs];
        uint64_t *ay = &g_ay[ofs];
        int qlen = d_len[job_idx];

        int64_t* z_x = &g_zx[ofs]; 
        int64_t* z_y = &g_zy[ofs]; 

        uint64_t* u = &g_u[ofs];



        int c_ofs = atomicAdd(n_chain, (int)n_u); 

        g_n_v[job_idx] = n_u;
        n_a[job_idx] = c_ofs;


        for (i = 0; i < n_u; ++i) {
            mm_chain_t *c = &g_c[c_ofs + i];
            c->qlen = qlen;
            c->a_ofs = ofs;
            c->r_ofs = r_ofs;
            c->dropped = false;
            c->n_a = n_v;
            c->flag = 0;
            c->r_n = job_idx;

            dropped[c_ofs+i] = false;

            mm_reg1_t *ri = &c->r;
            ri->id = i;
            ri->parent = MM_PARENT_UNSET;
            ri->score = ri->score0 = z_x[i] >> 32;
            ri->hash = (uint32_t)z_x[i];
            ri->cnt = (int32_t)z_y[i];
            ri->as = z_y[i] >> 32;
            ri->div = -1.0f;
            mm_reg_set_coor(ri, qlen, ax, ay, is_qstrand);
        }
    }
}

__global__
void mm_squeeze(int *n_regs, int* c_ofs, int* a_ofs, mm_chain_t *g_c, uint64_t *g_ax, uint64_t* g_ay, bool* g_dropped, uint64_t* buf, int n_task){
	
	int tid = threadIdx.x;
    int bid = blockIdx.x;
    int id = threadIdx.x + blockIdx.x * blockDim.x;

    for(int job_idx = id; job_idx < n_task; job_idx += gridDim.x * blockDim.x) {
		
        // offsets
        int ofs = c_ofs[job_idx];
        int arr_ofs = a_ofs[job_idx];
        // n regs (n chains)
        int* n = &n_regs[job_idx];
        // reg/chain info
		mm_chain_t* c = &g_c[ofs];
        bool* dropped = &g_dropped[ofs];
        uint64_t* keys = &buf[arr_ofs]; 
        uint64_t* ax = &g_ax[arr_ofs];
        uint64_t* ay = &g_ay[arr_ofs];

        // scan chain array and count non-dropped chains
        int k = 0;
        for(int i = 0; i < *n; i++){
            if(dropped[i]) continue;
            mm_reg1_t *r = &(c[i].r);
            if(r->cnt > 0) {
                keys[k++] = ((uint64_t)(uint32_t)r->as << 32) | (uint32_t)i; // stable by i
            }
        }
        if (k == 0) continue;

        // sort
        thrust::sort(thrust::seq, keys, keys + k);


        // rearrange anchor array and reg information
        // 4) compact anchors and update r->as
        int as_dst = 0;
        for (int t = 0; t < k; ++t) {
            uint32_t idx = (uint32_t)keys[t];
            mm_reg1_t *r = &(c[idx].r);
            if (r->as != as_dst) {
                memcpy(&ax[as_dst], &ax[r->as], r->cnt * sizeof(uint64_t));
                memcpy(&ay[as_dst], &ay[r->as], r->cnt * sizeof(uint64_t));
            }
            r->as = as_dst;
            as_dst += r->cnt;
        }
	}
}

__global__ 
void compute_end_ofs(int* ofs, int* num, int* end_ofs, int n_task){
    int id = threadIdx.x + blockIdx.x * blockDim.x;
    for(int job_idx = id; job_idx < n_task; job_idx += gridDim.x * blockDim.x) {
        if(job_idx > n_task) continue;
        int n_anchors = num[job_idx];
        if(n_anchors == 0){ // no chain
            end_ofs[job_idx] = ofs[job_idx] + 1;
        }
        else{
            if(n_anchors&1){ // filtered - no rmq
                num[job_idx] = 0;
                int sort_n = n_anchors >> 1;
                end_ofs[job_idx] = ofs[job_idx]+ 1;
            } 
            else{ // sorting target 
                num[job_idx] = n_anchors >> 1;
                int sort_n = n_anchors >> 1;
                end_ofs[job_idx] = ofs[job_idx] + sort_n;
            }
        }
    }
}