#include "seeding_kernel.h"

__device__
int khash_get_d(const idxhash_t *h, uint64_t key){
    if (h->n_buckets) {										
			khint_t k, i, last, mask, step = 0; 
			mask = h->n_buckets - 1;									
			k = idx_hash(key); i = k & mask;									
			last = i; 
			while (!__ac_isempty(h->flags, i) && (__ac_isdel(h->flags, i) || !idx_eq(h->keys[i], key))) { 
				i = (i + (++step)) & mask; \
				if (i == last) return h->n_buckets;						
			}														
			return __ac_iseither(h->flags, i)? h->n_buckets : i;		
	} else return 0;	
}

__device__
khint_t khash_end_d(const idxhash_t *h){
    return h->n_buckets;
}

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

__device__
static inline uint64_t hash64_d(uint64_t key, uint64_t mask) {

    key = (~key + (key << 21)) & mask; // key = (key << 21) - key - 1;
	key = key ^ key >> 24;
	key = ((key + (key << 3)) + (key << 8)) & mask; // key * 265
	key = key ^ key >> 14;
	key = ((key + (key << 2)) + (key << 4)) & mask; // key * 21
	key = key ^ key >> 28;
	key = (key + (key << 31)) & mask;
	return key;
    
}

__device__
const uint64_t *mm_idx_get_d(const mm_idx_t *mi, uint64_t minier, int *n)
{
	int mask = (1<<mi->b) - 1;
	khint_t k;
	mm_idx_bucket_t *b = &mi->B[minier&mask];
	idxhash_t *h = (idxhash_t*)b->h;
	*n = 0;
	if (h == 0) {
        return 0;
    }
	k = khash_get_d(h, minier>>mi->b<<1);
	if (k == khash_end_d(h)) {
        return 0;
    }
	if (khash_key_d(h, k)&1) { // special casing when there is only one k-mer
		*n = 1;
        uint64_t* result = &khash_val_d(h, k);
		return result;
	} else {
		*n = (uint32_t)khash_val_d(h, k);
		return &b->p[khash_val_d(h, k)>>32];
	}
}

__global__
void mm_sketch_device(mm_idx_t* idx, char* seqs, int* lens, int* offset, int* rids, mm128_t* min, int* n_seed, int n_task){
    
    int bid = blockIdx.x;
    int tid = threadIdx.x;

    __shared__ char i_buffer[MM_BUFFER_SIZE];
    __shared__ mm128_t mmi_info[MM_BUFFER_SIZE];
    __shared__ mm128_t o_buffer[MM_TILE_SIZE];

    int w = idx->w;
    int k = idx->k;

    for(int job_idx = bid; job_idx < n_task; job_idx += gridDim.x) {
    
        int ofs = offset[job_idx];
        int rid = 0; // TODO : set corresponding rid
        int len = lens[job_idx];

        uint64_t kmer[2] = {0,0};
        uint64_t shift1 = 2*(k-1);
        uint64_t mask = (1ULL<<2*k) - 1; 

        int c, z;
        int kmer_span = k;
        int mmi_n = 0;

        /* initialize output buffer */
        for(int j = tid; j < MM_BUFFER_SIZE; j+= blockDim.x) o_buffer[j] = {0,0};

        while(ofs < offset[job_idx] + len) { 
    
            // read input sequence to shared memory
            for(int i = 0; i < CHUNK_SIZE + 1; i++){
                if(ofs + blockDim.x * i + tid < offset[job_idx] + len) i_buffer[blockDim.x * i + tid] = seqs[ofs + blockDim.x * i + tid];
                else i_buffer[blockDim.x * i + tid] = 0; 
                mmi_info[blockDim.x * i + tid] = {UINT64_MAX, UINT64_MAX};
            }

            __syncthreads();
    
            // 1. Extract kmer & 2 bit base encoding

            for(int i = 0; i < CHUNK_SIZE + 1; i++) {

                kmer[0] = 0;
                kmer[1] = 0;

                for(int j = 0; j < k; j++){

                    if(i * blockDim.x + j + tid < MM_BUFFER_SIZE) {
                        // 1. extract kmers
                        c = seq_nt4_table_d[(uint8_t)i_buffer[i * blockDim.x + j + tid]]; 
                        if(c >= 4) {
                            continue; 
                        }
                        kmer[0] = (kmer[0] << 2 | c) & mask;
                        kmer[1] = (kmer[1] >> 2) | (3ULL^c) << shift1;
                        if(kmer[0] == kmer[1]) continue;
                        z = kmer[0] < kmer[1]? 0 : 1; // decide strand
                    }
                }
                // 2. Write hash value to hash value array
                mmi_info[i * blockDim.x + tid].x = hash64_d(kmer[z], mask) << 8 | kmer_span; // revisit kmer span
                mmi_info[i * blockDim.x + tid].y = (uint64_t)rid<<32 | (uint32_t)(ofs - offset[job_idx] + i * blockDim.x + k + tid - 1)<<1 | z; // revisit rid (currently fixed as 0)
            }

            __syncthreads();

            // 3. Compare & update minimum values 
            mm128_t mmi;

            for(int i = 0; i < CHUNK_SIZE; i++) {
                mmi = {UINT64_MAX, UINT64_MAX};

                for(int j = 0; j < w; j++){
                    if(mmi_info[i * blockDim.x + j + tid].x <= mmi.x && (ofs + i * blockDim.x + j + tid + k - 1 <= offset[job_idx]+len)) {
                        mmi = mmi_info[i * blockDim.x + j + tid];
                    }
                }

                if(ofs + i * blockDim.x + tid + w + k - 1 <= offset[job_idx]+len)
                {
                    o_buffer[i * blockDim.x + tid] = mmi;
                }
                else {
                    if(len < w + k - 1 && i * blockDim.x + tid == 0 && mmi.x < UINT64_MAX && mmi.y < UINT64_MAX) o_buffer[i * blockDim.x + tid] = mmi;
                    else o_buffer[i * blockDim.x + tid] = {0,0};
                }

            }

            // write compact mmi info to mmi array
            // also check for identical k-mers
            if(tid == 0){
                if(o_buffer[0].x!=0 || o_buffer[0].y!=0){
                    if(mmi_n == 0) { // first minimizer in a read

                        min[offset[job_idx] + mmi_n++] = o_buffer[0]; // keep the order
                    }
                    else if(min[offset[job_idx] + mmi_n - 1].x != o_buffer[0].x || min[offset[job_idx] + mmi_n - 1].y != o_buffer[0].y) {
                        
                        for(int j = 0; j < w; j++){ // handle identical k-mers
                            if(o_buffer[0].x == mmi_info[j].x && o_buffer[0].y != mmi_info[j].y){
                                if(min[offset[job_idx] + mmi_n - 1].x != mmi_info[j].x || min[offset[job_idx] + mmi_n - 1].y < mmi_info[j].y)
                                    min[offset[job_idx] + mmi_n++] = mmi_info[j];
                            }
                        }

                        min[offset[job_idx] + mmi_n++] = o_buffer[0]; // keep the order
                    }


                }

                for(int i = 0; i < MM_TILE_SIZE; i++){
                    if(((o_buffer[i].x != o_buffer[i+1].x)||(o_buffer[i].y != o_buffer[i+1].y)) && o_buffer[i+1].x != 0){
                        // look for identical k-mers in different locations
                        for(int j = 0; j < w; j++){
                            if(o_buffer[i+1].x == mmi_info[i+j+1].x && o_buffer[i+1].y != mmi_info[i+j+1].y){
                                if(o_buffer[i].x != mmi_info[i+j+1].x || o_buffer[i].y < mmi_info[i+j+1].y)
                                    min[offset[job_idx] + mmi_n++] = mmi_info[i+j+1];
                            }
                        }
                        min[offset[job_idx] + mmi_n++] = o_buffer[i+1];
                    }   
                }
            }

            ofs += MM_TILE_SIZE;
        }
        n_seed[job_idx] = mmi_n;
    }
}

__device__ int global_num_match_ = 0;

/* query parallel implementation. TODO : consider parallelization of heap building part, if slow */

__device__ void ks_heapdown_uint64_t_device(size_t i, size_t n, uint64_t* l) {
    size_t k = i;
    uint64_t tmp = l[i];

    while ((k = (k << 1) + 1) < n) {
        // Select the larger child
        if (k != n - 1 && l[k] < l[k + 1]) ++k;
        // If parent is larger than the child, stop
        if (l[k] < tmp) break;
        // Move the child up
        l[i] = l[k];
        i = k;
    }
    // Put the original parent value in its correct position
    l[i] = tmp;
}

__device__ void ks_heapmake_uint64_t_device(size_t n, uint64_t *l) {
    for (size_t i = (n >> 1) - 1; i != (size_t)(-1); --i) {
        ks_heapdown_uint64_t_device(i, n, l);
    }
}


#define MAX_MAX_HIGH_OCC 128
__device__ 
void mm_seed_select_device(int32_t n, mm_seed_t *a, int len, int max_occ, int max_max_occ, int dist){

    // query-parallel implementation
    int32_t i, last0, m;
    uint64_t b[MAX_MAX_HIGH_OCC];

    if (n == 0 || n == 1) return;
	for (i = m = 0; i < n; ++i)
		if (a[i].n > max_occ) ++m;
	if (m == 0) return; // no high-frequency k-mers; do nothing
	for (i = 0, last0 = -1; i <= n; ++i) {
		if (i == n || a[i].n <= max_occ) {
			if (i - last0 > 1) {
				int32_t ps = last0 < 0? 0 : (uint32_t)a[last0].q_pos>>1;
				int32_t pe = i == n? len : (uint32_t)a[i].q_pos>>1;
				int32_t j, k, st = last0 + 1, en = i;
				int32_t max_high_occ = (int32_t)((double)(pe - ps) / dist + .499);
				if (max_high_occ > 0) {
					if (max_high_occ > MAX_MAX_HIGH_OCC)
						max_high_occ = MAX_MAX_HIGH_OCC;
					for (j = st, k = 0; j < en && k < max_high_occ; ++j, ++k)
						b[k] = (uint64_t)a[j].n<<32 | j;
					ks_heapmake_uint64_t_device(k, b); // initialize the binomial heap
					for (; j < en; ++j) { // if there are more, choose top max_high_occ
						if (a[j].n < (int32_t)(b[0]>>32)) { // then update the heap
							b[0] = (uint64_t)a[j].n<<32 | j;
							ks_heapdown_uint64_t_device(0, k, b);
						}
					}
					for (j = 0; j < k; ++j) {
                        a[(uint32_t)b[j]].flt = 1;
                    }
				}
				for (j = st; j < en; ++j) a[j].flt ^= 1;
				for (j = st; j < en; ++j)
					if (a[j].n > max_max_occ){
						a[j].flt = 1;
                    }
			}
			last0 = i;
		}
	}
}

__device__ 
void mm_skip_seeds_device(){
    // TODO: implement skip seed device function
}


__global__
void mm_collect_seed_hits_device(mm_idx_t* idx, mm_mapopt_t* opt, mm128_t* mmi, int* lens, int* offset, uint64_t* out_x, uint64_t* out_y, int* n_seed, int* n_match, mm_seed_t* g_tmp_seed, int* anchor_offset, int n_task, int32_t* global_num_match, int* end_ofs){
    
    int tid = threadIdx.x;
    int bid = blockIdx.x;

    __shared__ int cnt;
    __shared__ int w_ofs_end;
    __shared__ int w_ofs;

    if(bid == 0 && tid == 0) *global_num_match = 0;

    uint32_t q_pos, q_span;
    uint32_t flt, seg_id, is_tandem;

    for(int job_idx = bid; job_idx < n_task; job_idx += gridDim.x) {

        int r_ofs = offset[job_idx]; // sequence offset
        int len = lens[job_idx]; // sequence length 
        int n_mmi = n_seed[job_idx]; // # of minimizers 

        int n;
        const uint64_t* cr;

        int i = 0, j;
        mm128_t q_mmi;

        cnt = 0;

        /* 1st iteration: Get match number & match data pointers */

        while(i < n_mmi){
            q_mmi = mmi[r_ofs + i + tid];

            /* Query reference (target) index */
            cr = mm_idx_get_d(idx, q_mmi.x>>8, &n);
        
            g_tmp_seed[r_ofs + i + tid].n = n;
            g_tmp_seed[r_ofs + i + tid].cr = (uint64_t*)cr;

            g_tmp_seed[r_ofs + i + tid].q_pos = (uint32_t) q_mmi.y;
            g_tmp_seed[r_ofs + i + tid].q_span = q_mmi.x & 0xff;
            g_tmp_seed[r_ofs + i + tid].seg_id = q_mmi.y >> 32;
            g_tmp_seed[r_ofs + i + tid].is_tandem = 0; 
            g_tmp_seed[r_ofs + i + tid].flt = 0;

            __syncthreads();
            
            i += THREAD_NUM_SEED;
        }
        __syncthreads();

        /* seed filtering */
        if(tid ==0) {
            int max_occ = opt->mid_occ;
            int max_max_occ = opt->max_max_occ;
            int dist = opt->occ_dist;
            mm_seed_select_device(n_mmi, &g_tmp_seed[r_ofs], len, max_occ, max_max_occ, dist);
        }

        i = 0;
        while(i < n_mmi){

            if(!(g_tmp_seed[r_ofs + i + tid].flt) && i + tid < n_mmi){
                n = g_tmp_seed[r_ofs + i + tid].n;
            }
            else{
                n = 0;
            }

            for (int offset = 16; offset > 0; offset /= 2)
                n += __shfl_down_sync(FULL_MASK, n, offset);
            if(tid==0) cnt += n;

            i += THREAD_NUM_SEED;

        }
        
        int cnt_ = (cnt % READ_SEG == 0)? cnt : ((cnt / READ_SEG) + 1) * READ_SEG;
        if(tid == 0) {
            n_match[job_idx] = cnt;
            /* Get anchor offset value using atomic operation */
            w_ofs = atomicAdd(global_num_match, cnt_);
            /* write anchor offset */
            anchor_offset[job_idx] = w_ofs;
            end_ofs[job_idx] = w_ofs + cnt;
            w_ofs_end = w_ofs + cnt_;
        }
        __syncthreads();

        /* 2nd iteration: Fetch matching reference (target) positions */
        for(i = 0; i < n_mmi; i++){
            if(g_tmp_seed[r_ofs + i].flt) continue;

            n = g_tmp_seed[r_ofs + i].n;
            cr = g_tmp_seed[r_ofs + i].cr;
            q_mmi = mmi[r_ofs + i];

            /* Fill seed information */ 
            q_pos = (uint32_t) q_mmi.y;
            q_span = q_mmi.x & 0xff;
            seg_id = q_mmi.y >> 32;
            is_tandem = 0; flt = 0;

            uint64_t tmp;

            if(i!=0){
                tmp = mmi[r_ofs + i - 1].x;
                if (q_mmi.x>>8 == tmp>>8) is_tandem = 1;
            }
            if(i!=n_mmi-1){
                tmp = mmi[r_ofs + i + 1].x;
		        if (q_mmi.x>>8 == tmp>>8) is_tandem = 1;
            }

            /* Write anchor information */
            for(j = tid; j < n; j += blockDim.x){
                int32_t is_self, rpos = (uint32_t)cr[j] >> 1;
                uint64_t *px, *py;
			    // if (skip_seed(opt->flag, r[k], q, qname, qlen, mi, &is_self)) continue; // TODO: skip seed device function
			    px = &out_x[w_ofs + j]; 
                py = &out_y[w_ofs + j];

                if ((cr[j]&1) == (q_pos&1)) { // forward strand
				    *px = (cr[j]&0xffffffff00000000ULL) | rpos;
				    *py = (uint64_t)q_span << 32 | q_pos >> 1;
                } 
                else if (!(opt->flag & MM_F_QSTRAND)) { // reverse strand and not in the query-strand mode
                    *px = 1ULL<<63 | (cr[j]&0xffffffff00000000ULL) | rpos;
				    *py = (uint64_t)q_span << 32 | (len - ((q_pos>>1) + 1 - q_span) - 1);
			    } 
                else { // reverse strand; query-strand
				    int32_t len = idx->seq[cr[j]>>32].len; 
				    *px = 1ULL<<63 | (cr[j]&0xffffffff00000000ULL) | (len - (rpos + 1 - q_span) - 1); // coordinate only accurate for non-HPC seeds
				    *py = (uint64_t)q_span << 32 | q_pos >> 1;
			    }
			    *py |= (uint64_t)seg_id << MM_SEED_SEG_SHIFT;
			    if (is_tandem) *py |= MM_SEED_TANDEM;
			    if (is_self) *py |= MM_SEED_SELF;

            }
            __syncthreads();
            w_ofs += n;
        }
        // initialize dummy anchors
        for(j = w_ofs + tid; j < w_ofs_end; j += blockDim.x){
            out_x[j] = (uint64_t) 0;
            out_y[j] = (uint64_t) 0;
        }
    }
    __syncthreads();
}
