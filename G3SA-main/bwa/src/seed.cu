#include "bwa.h"
#include "macro.h"
#include "gmem_alloc.h"

#include "fmindex.cuh"
#include <string.h>
#include <cub/cub.cuh>
#include <chrono>
#include <immintrin.h>
#include <stdio.h>

//
// Preseed
//

// forward declaration
__device__ void preseedOnePos(const fmIndex *fmIndex, int read_len, const uint8_t *read, int m, int min_seed_len, bwtintv_t *preSeeds, kmers_bucket_t *d_kmersHashTab);

__device__ void preseedOnePos2(const fmIndex *fmIndex, int read_len, const uint8_t *read, int m, int min_seed_len, bwtintv_t *preSeeds, kmers_bucket_t *d_kmersHashTab);

__device__ void preseedOnePosBackward(const fmIndex *fmIndex, int read_len, const uint8_t *read, int m, int min_seed_len, bwtintv_t *preSeeds, kmers_bucket_t *d_kmersHashTab);

__device__ void preseedOnePos2Backward(const fmIndex *fmIndex, int read_len, const uint8_t *read, int m, int min_seed_len, bwtintv_t *preSeeds, kmers_bucket_t *d_kmersHashTab);

__global__ void preseedAndFilterV2(
        const fmIndex  *devFmIndex,
        const mem_opt_t *d_opt, 
        const uint8_t *d_seq,
        int *d_seq_offset,
        smem_aux_t *d_aux, 			// aux output
        kmers_bucket_t *d_kmerHashTab,
        void *d_buffer_pools)
{
    int seq_id = blockIdx.x;
    const uint8_t *read;
    int minSeedLen;     // option: minimum seed length
    int64_t sentinelIndex; // index: sentinel index
    int m, n; // local var
    int read_len; // read sequence length

    int seq_offset = d_seq_offset[seq_id];
    int seq_offset_next = d_seq_offset[seq_id + 1];
    read = d_seq + seq_offset;
    read_len = seq_offset_next - seq_offset;
    minSeedLen = d_opt->min_seed_len;

    __shared__ bwtintv_t sharedPreSeeds[MAX_LEN_READ];
    __shared__ uint8_t sharedRead[MAX_LEN_READ];
    for (int j = threadIdx.x; j<read_len; j+=blockDim.x)
    {
        sharedRead[j] = read[j];
        sharedPreSeeds[j].info = 0;
    }
    __syncthreads(); __syncwarp();


    // Collect preSeed = read[m..n]
    // n loop is parallelized.
    for(n = minSeedLen - 1 + threadIdx.x; n < read_len; n += blockDim.x)
    {
        preseedOnePos2Backward(devFmIndex, read_len, sharedRead, n, minSeedLen, sharedPreSeeds, d_kmerHashTab);
    }
    __syncthreads(); __syncwarp();


    // Inspect in parallel 
	__shared__ bool sharedIsSeed[MAX_LEN_READ];
    for(n = threadIdx.x; n < read_len - 1; n += blockDim.x)
    {
        // TODO do other inspection: longID, frac_Rep also here 
        sharedIsSeed[n] = (bool)(sharedPreSeeds[n].info);
		if(((sharedPreSeeds[n].info >> 32) == (sharedPreSeeds[n + 1].info >> 32)) && (sharedPreSeeds[n + 1].info != 0)) // non-super seed
        {
            sharedIsSeed[n] = 0;
        }
	}
	__syncthreads(); __syncwarp();

    // Gather sequentially
    if(threadIdx.x==0) {
        sharedIsSeed[read_len - 1] = (bool)(sharedPreSeeds[read_len - 1].info);

        int num_seeds = 0;
        for(n = 0; n < read_len; n++) {
            if(sharedIsSeed[n]) {
                num_seeds++;
            }
        }
		d_aux[blockIdx.x].mem.n = num_seeds;
        
        void *d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, blockIdx.x % 32);
        bwtintv_t *gm_seeds = (bwtintv_t*)CUDAKernelMalloc(d_buffer_ptr, sizeof(bwtintv_t) * num_seeds, sizeof(bwtintv_t));
        int j = 0;
        for(n = 0; n < read_len; n++)
        {
            if(sharedIsSeed[n])
            {
                gm_seeds[j++] = sharedPreSeeds[n];
            }
        }

        //printf("DEBUG num_seeds %d\n", num_seeds);
        d_aux[blockIdx.x].mem.a = gm_seeds;
    }
}


__global__ void preseedAndFilter(
        const fmIndex  *devFmIndex,
        const mem_opt_t *d_opt, 
        const bseq1_t *d_seqs, 
        smem_aux_t *d_aux, 			// aux output
        kmers_bucket_t *d_kmerHashTab)
{
    char *read;             // read sequence for this block
    int minSeedLen;     // option: minimum seed length
    int64_t sentinelIndex; // index: sentinel index
    int m, n; // local var
    int read_len; // read sequence length

    read = d_seqs[blockIdx.x].seq; 		// seq_id is blockIdx.x
    read_len = d_seqs[blockIdx.x].l_seq;	// read length
    minSeedLen = d_opt->min_seed_len;

    __shared__ uint8_t sharedRead[MAX_LEN_READ];
    for (int j = threadIdx.x; j<read_len; j+=blockDim.x)
    {
        sharedRead[j] = (uint8_t)read[j];
    }
    __syncthreads(); __syncwarp();


    __shared__ bwtintv_t sharedPreSeeds[MAX_LEN_READ];
    // Collect preSeed = read[m..n]
    // n loop is parallelized.
    for(n = minSeedLen - 1 + threadIdx.x; n < read_len; n += blockDim.x)
    {
        preseedOnePos2Backward(devFmIndex, read_len, sharedRead, n, minSeedLen, sharedPreSeeds, d_kmerHashTab);
    }
    __syncthreads(); __syncwarp();

    // Inspect in parallel 
	__shared__ bool sharedIsSeed[MAX_LEN_READ];
    for(n = threadIdx.x; n < read_len - 1; n += blockDim.x)
    {
        // TODO do other inspection: longID, frac_Rep also here 
        sharedIsSeed[n] = (bool)(sharedPreSeeds[n].info);
		if(((sharedPreSeeds[n].info >> 32) == (sharedPreSeeds[n + 1].info >> 32)) && (sharedPreSeeds[n + 1].info != 0)) // non-super seed
        {
            sharedIsSeed[n] = 0;
        }
	}
	__syncthreads(); __syncwarp();

    // Gather sequentially
    if(threadIdx.x==0){
        sharedIsSeed[read_len - 1] = (bool)(sharedPreSeeds[read_len - 1].info);
        bwtintv_t *seeds = d_aux[blockIdx.x].mem.a;
        int num_seeds = 0;
        for(n = 0; n < read_len; n++)
        {
            if(sharedIsSeed[n])
            {
                seeds[num_seeds] = sharedPreSeeds[n];
                num_seeds++;
            }
        }
		d_aux[blockIdx.x].mem.n = num_seeds;
    }
}


__global__ void preseed(
        const fmIndex  *devFmIndex,
        const mem_opt_t *d_opt, 
        const bseq1_t *d_seqs, 
        smem_aux_t *d_aux, 			// aux output
        kmers_bucket_t *d_kmerHashTab)
{
    // Extract input and output variable locations
    char *read;             // read sequence for this block
    smem_aux_t *auxOutput;  // preSeed output
    int minSeedLen;     // option: minimum seed length
    int64_t sentinelIndex; // index: sentinel index
    int m, n; // local var
    int read_len; // read sequence length

    read = d_seqs[blockIdx.x].seq; 		// seq_id is blockIdx.x
    read_len = d_seqs[blockIdx.x].l_seq;	// read length
    auxOutput = &d_aux[blockIdx.x];
    minSeedLen = d_opt->min_seed_len;

    // Prepare shared and global memory to use
    __shared__ uint8_t sharedRead[MAX_LEN_READ];
    __shared__ bwtintv_t *sharedPreSeeds;

    for (int j = threadIdx.x; j<read_len; j+=blockDim.x)
    {
        sharedRead[j] = (uint8_t)read[j];
    }
    if (threadIdx.x == 0){
        sharedPreSeeds = auxOutput->mem.a;
        auxOutput->mem.n = 0;
    }
    __syncthreads(); __syncwarp();



    // Collect preSeed = read[m..n]
    for (int j = threadIdx.x; j<MAX_LEN_READ; j+=blockDim.x)
    {
        sharedPreSeeds[j].x[0] = 0;
        sharedPreSeeds[j].x[1] = 0;
        sharedPreSeeds[j].x[2] = 0;
        sharedPreSeeds[j].info = 0;
    }
    __syncthreads(); __syncwarp();

#ifdef FORWARD
    // m loop is parallelized.
    for (m = threadIdx.x; m <= read_len - minSeedLen; m += blockDim.x){
#ifdef STRIDED
        preseedOnePos2(devFmIndex, read_len, sharedRead, m, minSeedLen, sharedPreSeeds, d_kmerHashTab);
#else
        preseedOnePos(devFmIndex, read_len, sharedRead, m, minSeedLen, sharedPreSeeds, d_kmerHashTab);
#endif
    }
#else
    // n loop is parallelized.
    //for(n = read_len - 1 - threadIdx.x; n >= minSeedLen - 1; n -= blockDim.x)
    for(n = minSeedLen - 1 + threadIdx.x; n < read_len; n += blockDim.x)
    {
#ifdef STRIDED
        preseedOnePos2Backward(devFmIndex, read_len, sharedRead, n, minSeedLen, sharedPreSeeds, d_kmerHashTab);
#else
        preseedOnePosBackward(devFmIndex, read_len, sharedRead, n, minSeedLen, sharedPreSeeds, d_kmerHashTab);
#endif
    }
#endif
}


// Collect a preSeed = read[m..n]
__device__ void preseedOnePos(const fmIndex *fmIndex, int read_len, const uint8_t *read, int m, int min_seed_len, bwtintv_t *preSeeds, kmers_bucket_t *d_kmersHashTab)
{
    uint64_t *oneHot = fmIndex->oneHot;
    int64_t *count = fmIndex->count;
    int64_t *count2 = fmIndex->count2;
    CP_OCC *cpOcc = fmIndex->cpOcc;
    CP_OCC2 *cpOcc2 = fmIndex->cpOcc2;
    int64_t sentinelIndex = *(fmIndex->sentinelIndex);
    uint8_t firstBase = *(fmIndex->firstBase);


    bwtintv_t smem, nextSmem;
    int min_intv = 1;
    if (read[m] >= 4)
    {
        preSeeds[m].info = 0;
        return;
    }
    smem.x[0] = count[read[m]];
    smem.x[1] = count[3 - read[m]];
    smem.x[2] = count[read[m] + 1] - count[read[m]];

    int n;
    for(n = m; n < read_len - 1; ++n) // try extending read[n + 1]
    {
        if(read[n + 1] >= 4) // always terminate at ambiguous base 'N'
        {
            break;
        }
        bwtintv_t oldSmem = smem;
        FORWARD_EXT(smem, read[n + 1]);
            if(smem.x[2] < min_intv)
            {
                smem = oldSmem;
                break;
            }
    }

    if((n - m + 1 >= min_seed_len) && (smem.x[2] >= min_intv)) // verify then add read[m..n]
    {
        smem.info =  INFO(m, n);
    }
    else
    {
        smem.info = 0;
    }

    preSeeds[m] =  smem;
    return;
}

// Collect a preseed = read[m..n]
__device__ void preseedOnePos2(const fmIndex *fmIndex, int read_len, const uint8_t *read, int m, int minSeedLen, bwtintv_t *preseeds, kmers_bucket_t *d_kmersHashTab)
{
    uint64_t *oneHot = fmIndex->oneHot;
    int64_t *count = fmIndex->count;
    int64_t *count2 = fmIndex->count2;
    CP_OCC *cpOcc = fmIndex->cpOcc;
    CP_OCC2 *cpOcc2 = fmIndex->cpOcc2;
    int64_t sentinelIndex = *(fmIndex->sentinelIndex);
    uint8_t firstBase = *(fmIndex->firstBase);


    bwtintv_t smem, oldSmem;
    int min_intv = 1;
    int minIntervalSize = 1;
    if (read[m] >= 4)
    {
        preseeds[m].info = 0;
        return;
    }
    smem.x[0] = count[read[m]];
    smem.x[1] = count[3 - read[m]];
    smem.x[2] = count[read[m] + 1] - count[read[m]];

    int n;
    uint8_t base0, base1;
    for(n = m; n < read_len - 2; n+=2) // extend to the most forward
    {
        base0 = read[n + 1];
        base1 = read[n + 2];
        oldSmem = smem;

        if(base1 >= 4)  // 'N' check
        {
            if(base0 < 4)
            {
                FORWARD_EXT(smem, base0);
                    if(smem.x[2] >= minIntervalSize)
                    {
                        n++;
                    }
                    else
                    {
                        smem = oldSmem;
                    }
            }
            break;
        }
        if(base0 >= 4)
        {
            break;
        }

        FORWARD_EXT2(smem, base0, base1);
            if(smem.x[2] < minIntervalSize)
            {
                smem = oldSmem;
                FORWARD_EXT(smem, base0);
                    if(smem.x[2] >= minIntervalSize)
                    {
                        n++;
                        break;
                    }
                    else
                    {
                        smem = oldSmem;
                        break;
                    }
            }
    }
    if(n == read_len - 2)
    {
        oldSmem = smem;
        FORWARD_EXT(smem, read[read_len - 1]);
            if(smem.x[2] < minIntervalSize)
            {
                smem = oldSmem;
            }
            else
            {
                n++;
            }
    }

    if(n - m + 1 >= minSeedLen) // interval size is always >= minIntervalSize
    {
        smem.info =  INFO(m, n);
    }
    else
    {
        smem.info = 0;
    }

    preseeds[m] =  smem;
    return;
}

// Collect a preSeed = read[m..n]
__device__ void preseedOnePosBackward(const fmIndex *fmIndex, int read_len, const uint8_t *read, int n, int minSeedLen, bwtintv_t *preseeds, kmers_bucket_t *d_kmersHashTab)
{
    uint64_t *oneHot = fmIndex->oneHot;
    int64_t *count = fmIndex->count;
    int64_t *count2 = fmIndex->count2;
    CP_OCC *cpOcc = fmIndex->cpOcc;
    CP_OCC2 *cpOcc2 = fmIndex->cpOcc2;
    int64_t sentinelIndex = *(fmIndex->sentinelIndex);
    uint8_t firstBase = *(fmIndex->firstBase);


    bwtintv_t smem, oldSmem;
    int min_intv = 1;
    int minIntervalSize = 1;
    if (read[n] >= 4)
    {
        preseeds[n].info = 0;
        return;
    }
    smem.x[0] = count[read[n]];
    smem.x[1] = count[3 - read[n]];
    smem.x[2] = count[read[n] + 1] - count[read[n]];

    int m;
    uint8_t base0;

    // Elongate
    for(m = n; m >= 1; m--)
    {
        base0 = read[m - 1];
        if(base0 >= 4)
        {
            break;
        }

        oldSmem = smem;
        BACKWARD_EXT_B(smem, base0);
            if(smem.x[2] < minIntervalSize)
            {
                smem = oldSmem;
                break;
            }
    }

    // Check
    if(n - m + 1 >= minSeedLen)
    {
        smem.info = INFO(m, n);
    }
    else
    {
        smem.info = 0;
    }

    // Collect
    preseeds[n] = smem;

    return;
}

// Collect a preseed = read[m..n]
__device__ void preseedOnePos2Backward(const fmIndex *fmIndex, int read_len, const uint8_t *read, int n, int minSeedLen, bwtintv_t *preseeds, kmers_bucket_t *d_kmersHashTab)
{
    uint64_t *oneHot = fmIndex->oneHot;
    int64_t *count = fmIndex->count;
    int64_t *count2 = fmIndex->count2;
    CP_OCC *cpOcc = fmIndex->cpOcc;
    CP_OCC2 *cpOcc2 = fmIndex->cpOcc2;
    int64_t sentinelIndex = *(fmIndex->sentinelIndex);
    uint8_t firstBase = *(fmIndex->firstBase);

    bwtintv_t smem, oldSmem;
    int minIntervalSize = 1;
    if (read[n] >= 4)
    {
        preseeds[n].info = 0;
        return;
    }
    smem.x[0] = count[read[n]];
    smem.x[1] = count[3 - read[n]];
    smem.x[2] = count[read[n] + 1] - count[read[n]];

    int m;
    uint8_t base0, base1;

    // Extend
    for(m = n; m > 1; m -= 2) {   // at the start, smem == read[m, n]
        oldSmem = smem;
        base0 = read[m - 1];
        base1 = read[m - 2];
        if(base1 >= 4) {
            if(base0 < 4) {
                BACKWARD_EXT_B(smem, base0);
                if(smem.x[2] < minIntervalSize) {
                    smem = oldSmem;
                } else {
                    m--;
                }
            }
            break; // smem == read[m,n]
        }

        if(base0 >= 4) {
            break; // smem == read[m,n]
        }

        BACKWARD_EXT2_B(smem, base0, base1);
        if(smem.x[2] < minIntervalSize) {
            smem = oldSmem;
            BACKWARD_EXT_B(smem, base0);
            if(smem.x[2] < minIntervalSize) {
                smem = oldSmem;
            } else {
                m--;
            }
            break;
        }
    }
    if(m == 1) {
        oldSmem = smem;
        if(read[0] < 4) {
            BACKWARD_EXT_B(smem, read[0]); 
            if(smem.x[2] < minIntervalSize) {
                smem = oldSmem;
            } else {
                m--;
            }
        }
    }

    if(n - m + 1 >= minSeedLen) {
        smem.info = INFO(m, n);
    } else {
        smem.info = 0;
    }

    preseeds[n] = smem;
    return;
}

//
//  Filter Seed
// 

__device__ void removeForward(const mem_opt_t *d_opt, smem_aux_t *d_aux);
__device__ void removeBackward(const mem_opt_t *d_opt, smem_aux_t *d_aux);

__global__ void filterSeeds(
	const mem_opt_t *d_opt,
	smem_aux_t *d_aux
	)
{
        removeForward(d_opt, d_aux);
}

__device__ void removeForward(
	const mem_opt_t *d_opt,
	smem_aux_t *d_aux
	)
{
	// seq_id = blockIdx.x
	bwtintv_t *preSeeds = d_aux[blockIdx.x].mem.a;
	int numPreSeeds = d_aux[blockIdx.x].mem.n;
    int pos; // = m = qbeg
    __syncthreads(); __syncwarp();

	__shared__ int sharedIsSeed[MAX_LEN_READ];
    for(pos = threadIdx.x; pos < numPreSeeds; pos += blockDim.x)
    {
        sharedIsSeed[pos] = (int)(preSeeds[pos].info);
		if((pos > 0) && (uint32_t)preSeeds[pos].info==(uint32_t)preSeeds[pos-1].info) // non-super seed
        {
            sharedIsSeed[pos] = 0;
        }
	}
	__syncthreads(); __syncwarp();

    if(threadIdx.x==0){
        bwtintv_t *seeds = preSeeds;
        int num_seeds = 0;
        for(pos = 0; pos < numPreSeeds; pos++)
        {
            if(sharedIsSeed[pos])
            {
                seeds[num_seeds] = seeds[pos];
                num_seeds++;
            }
        }
		d_aux[blockIdx.x].mem.n = num_seeds;
		d_aux[blockIdx.x].mem.a = seeds;
    }
}

__device__ void removeBackward(
	const mem_opt_t *d_opt,
	smem_aux_t *d_aux
	)
{
	// seq_id = blockIdx.x
	bwtintv_t *preseeds = d_aux[blockIdx.x].mem.a;
	int numPreSeeds = d_aux[blockIdx.x].mem.n;

    // Test in parallel
    int n;
	__shared__ int sharedIsSeed[MAX_LEN_READ];
    for(n = threadIdx.x; n < numPreSeeds - 1; n += blockDim.x)
    {
        sharedIsSeed[n] = (int)(preseeds[n].info);
		if(((preseeds[n].info >> 32) == (preseeds[n + 1].info >> 32)) && (preseeds[n + 1].info != 0)) // non-super seed
        {
            sharedIsSeed[n] = 0;
        }
	}
	__syncthreads(); __syncwarp();

    // Gather sequentially
    if(threadIdx.x==0){
        bwtintv_t *seeds = preseeds;
        int num_seeds = 0;
        for(n = 0; n < numPreSeeds; n++)
        {
            if(sharedIsSeed[n])
            {
                seeds[num_seeds] = seeds[n];
                num_seeds++;
            }
        }
		d_aux[blockIdx.x].mem.n = num_seeds;
		d_aux[blockIdx.x].mem.a = seeds;
    }
}


//
// Re-Seed
//

__device__ void reseedOnePos(const fmIndex *fmIndex, const uint8_t *sharedRead, \
        int read_len, int pos, int minSeedLen, int minInterval,\
        bwtintv_t* p, int *num_seeds2, void *d_buffer_pools);

__device__ void reseedThirdRound(const fmIndex *fmIndex, const uint8_t *sharedRead, \
        int read_len, int minSeedLen, int maxIntervalSize,\
        bwtintv_t *seeds, int *num_seeds3);


__global__ void reseedV2(
        const fmIndex *devFmIndex,
        const mem_opt_t *d_opt, 
        uint8_t *d_seq,
        int *d_seq_offset,
        smem_aux_t *d_aux, 			// aux output
        kmers_bucket_t *d_kmerHashTab,
        void * d_buffer_pools,
        int num_reads
        )
{
    int seq_id = blockDim.x * blockIdx.x + threadIdx.x;
    if(seq_id >= num_reads)
    {
        return;
    }

    int num_smem, max_num_seed3;
    int min_seed_len, min_seed_intv;
    int read_len, pivot_pos;
    int num_allseeds, cap_allseeds;
#define LM_CAP 16 // HARD CODED HEURISTIC
    bwtintv_t lm_seeds[LM_CAP]; 
    bwtintv_t lm_leps[LM_CAP]; 
    void *d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, blockIdx.x%32);
    bwtintv_t *seeds;
    bwtintv_t *leps;
    int num_seeds, num_leps;
    int cap_seeds, cap_leps;

    bwtintv_t *smem;
    int split_len, split_intv;
    int k;
    int seq_offset, seq_offset_next;
    seq_offset = d_seq_offset[seq_id];
    seq_offset_next = d_seq_offset[seq_id + 1];
    read_len = seq_offset_next - seq_offset;
    const uint8_t *read = d_seq + seq_offset;

    uint64_t *oneHot = devFmIndex->oneHot; // DO NOT change names for macros
    int64_t *count = devFmIndex->count;
    int64_t *count2 = devFmIndex->count;
    CP_OCC *cpOcc = devFmIndex->cpOcc;
    CP_OCC2 *cpOcc2 = devFmIndex->cpOcc2;
    int64_t sentinelIndex = *(devFmIndex->sentinelIndex);

    num_smem = d_aux[seq_id].mem.n;
    split_len = (int)(d_opt->min_seed_len * d_opt->split_factor \
                        + .499); // default: 19 * 1.5 + .499 = 28
    split_intv = d_opt->split_width; // default: 10
    min_seed_len = d_opt->min_seed_len;

    {
        if(num_smem == 0) {
            return;
        }
        bwtintv_t *new_allseeds;
        num_allseeds = num_smem;
        cap_allseeds = num_smem << 1;
        new_allseeds = (bwtintv_t*)CUDAKernelMalloc(d_buffer_ptr,\
                sizeof(bwtintv_t) * cap_allseeds, sizeof(bwtintv_t));
        for(int l=0; l<num_allseeds; l++)
        {
            new_allseeds[l] = d_aux[seq_id].mem.a[l];
        }
        d_aux[seq_id].mem.a = new_allseeds;
        //d_aux[seq_id].mem.a[num_allseeds++].x[2] = 0; // separates smem | seed2
    }

    for(k = 0; k < num_smem; k++)
    {
        smem = d_aux[seq_id].mem.a + k;
        if(PLEN(smem) < split_len || smem->x[2] > split_intv) // not long
        {
            continue;
        }
        // reseed
        min_seed_intv = smem->x[2] + 1;
        pivot_pos = (PM(smem) + PN(smem) + 1) >> 1;
        if(pivot_pos > 77){ // FIXME
            //printf("pivot_pos %d\n", pivot_pos);
            continue;
        }
        if(read[pivot_pos] >=4)
        {
            continue;
        }
        num_seeds = num_leps = 0;
        seeds = lm_seeds; // if num_{seeds, leps} exceeds LM_CAP, spill all to gm.
        leps = lm_leps;
        cap_seeds = cap_leps = LM_CAP;

        // 1. collect LEPs
        int m, n;
        bwtintv_t seed, oldSeed;
        seed.x[0] = count[read[pivot_pos]];
        seed.x[1] = count[3 - read[pivot_pos]];
        seed.x[2] = count[read[pivot_pos] + 1] - count[read[pivot_pos]];
        seed.info = INFO(pivot_pos, pivot_pos);

        for(n = pivot_pos; n < read_len - 1; n++) // collect lep = read[pivot_pos..n]
        {
#ifdef DEBUG_RESEED
            printf("lep? seed [pivot..n] = [%d..%d], s=%d\n", pivot_pos, n, seed.x[2]);
#endif
            oldSeed = seed;
            if(read[n + 1] >= 4)
            {
                break;
            }

            FORWARD_EXT(seed, read[n + 1]);
            seed.info = INFO(pivot_pos, n + 1);

            if(seed.x[2] < oldSeed.x[2])
            {
                if(num_leps == cap_leps)
                {
                    bwtintv_t *new_leps;
                    new_leps = (bwtintv_t*)CUDAKernelMalloc(d_buffer_ptr,\
                            sizeof(bwtintv_t) * cap_leps << 1, sizeof(bwtintv_t));
                    for(int l=0; l<num_leps; l++)
                    {
                        new_leps[l] = leps[l];
                    }
                    cap_leps = cap_leps << 1;
                    leps = new_leps;
                }
                leps[num_leps++] = oldSeed;
                if(seed.x[2] < min_seed_intv)
                {
                    break;
                }
            }
        }
        if(seed.x[2] >= min_seed_intv)
        {
            seed.info = INFO(pivot_pos, n);
            if(num_leps == cap_leps)
            {
                bwtintv_t *new_leps;
                new_leps = (bwtintv_t*)CUDAKernelMalloc(d_buffer_ptr,\
                        sizeof(bwtintv_t) * cap_leps << 1, sizeof(bwtintv_t));
                for(int l=0; l<num_leps; l++)
                {
                    new_leps[l] = leps[l];
                }
                cap_leps = cap_leps << 1;
                leps = new_leps;
            }
            leps[num_leps++] = seed;
        }

        // reverse LEPs
        for(int j = 0; j < (num_leps >> 1); j++)
        {
            bwtintv_t temp = leps[j];
            leps[j] = leps[num_leps - 1 - j];
            leps[num_leps - 1 - j] = temp;
        }

        for(int j = 0; j < num_leps; j++)
        {
            bwtintv_t temp = leps[j];
#ifdef DEBUG_RESEED
            printf("before bwd: LEP %2d: (%lu, %lu, %lu, %u, %u)\n", j, temp.x[0], temp.x[1], temp.x[2], M(temp), N(temp));
#endif
        }


        // 2.backward Ext LEPs
        bwtintv_t longestLep, oldLongestLep;
        bwtintv_t lep;
        int currInterval;
        uint8_t base;
        // A loop invariant: at the start of each loop,
        // all leps satisfy the minimum intv size. i.e. lep.x[2] >= min_seed_intv.
        bwtintv_t oldLep;
        for(m = pivot_pos; m > 0; m--) // collect seed = read[m..n]
        {
#ifdef DEBUG_RESEED
            printf("m-1 = %d, read[m-1]=%d\n", m-1, read[m-1]);
            for(int j = 0; j < num_leps; j++)
            {
                printf("LEP %2d: (%lu, %lu, %lu, %u, %u)\n", j, leps[j].x[0], leps[j].x[1], leps[j].x[2], M(leps[j]), N(leps[j]));
            }
#endif

            base = read[m - 1];
            if(base >= 4)
            {
                break;
            }

            int num_old_leps = num_leps;
            currInterval = min_seed_intv;
            int j = 0; num_leps = 0;
            bool collected = false;
            while(j < num_old_leps) {
                oldLep = lep = leps[j];
                BACKWARD_EXT(lep, base);
                if(lep.x[2] >= currInterval) {
                    lep.info = INFO(m-1, N(lep));
                    leps[num_leps++] = lep;
                    currInterval = lep.x[2];
                } else {
                    if(!collected && LEN(oldLep) >= min_seed_len) {
                        // add oldLep to seeds
                        if(num_seeds == cap_seeds)
                        {
                            bwtintv_t *new_seeds = (bwtintv_t*)CUDAKernelMalloc(d_buffer_ptr,\
                                    sizeof(bwtintv_t) * cap_seeds << 1, sizeof(bwtintv_t));
                            cap_seeds = cap_seeds << 1;
                            for(int l=0; l<num_seeds; l++) {
                                new_seeds[l] = seeds[l];
                            }
                            seeds = new_seeds;
                        }
                        seeds[num_seeds++] = oldLep;

                        collected = true;
                    }
                }
                j++;
            }
            if(num_leps == 0) {
                break;
            }
        } 
        if(num_leps > 0) {
            oldLep = leps[0];
            if(LEN(oldLep) >= min_seed_len) {
                seeds[num_seeds++] = oldLep;
            }
        }
        // collected all seed2s from this seed


        if(cap_allseeds - num_allseeds < num_seeds)
        {
            bwtintv_t *new_allseeds;
            new_allseeds = (bwtintv_t*)CUDAKernelMalloc(d_buffer_ptr,\
                    sizeof(bwtintv_t) * (num_allseeds + num_seeds) << 1, sizeof(bwtintv_t));
            for(int l=0; l<num_allseeds; l++)
            {
                new_allseeds[l] = d_aux[seq_id].mem.a[l];
            }
            cap_allseeds = (num_allseeds + num_seeds) << 1;
            d_aux[seq_id].mem.a = new_allseeds;
        }

        for(int l=0; l<num_seeds; l++)
        {
            d_aux[seq_id].mem.a[num_allseeds++] = seeds[l];
        }
    }

    // reallocate memory to store reseeded seeds
    max_num_seed3 = read_len / (d_opt->min_seed_len + 1) + 1;
    if(num_allseeds + max_num_seed3 + 1 > cap_allseeds) 
    {
        bwtintv_t *new_allseeds;
        new_allseeds = (bwtintv_t*)CUDAKernelMalloc(d_buffer_ptr,\
                sizeof(bwtintv_t) * (num_allseeds + max_num_seed3 + 1), sizeof(bwtintv_t));
        for(int l=0; l<num_allseeds; l++)
        {
            new_allseeds[l] = d_aux[seq_id].mem.a[l];
        }
        d_aux[seq_id].mem.a = new_allseeds;

        // separates seed2 | seed3
        //d_aux[seq_id].mem.a[num_allseeds++].x[2] = 0;
    }
    d_aux[seq_id].mem.n = num_allseeds;
}



__global__
void reseedParallel(
        const fmIndex *devFmIndex,
        const mem_opt_t *d_opt, 
        const bseq1_t *d_seqs, 
        smem_aux_t *d_aux, 			// aux output
        kmers_bucket_t *d_kmerHashTab,
        void * d_buffer_pools
        )
{
    int seq_id = blockIdx.x;
    bwtintv_t *seeds = d_aux[seq_id].mem.a; 
    int minSeedLen = d_opt->min_seed_len;
    int splitLen = (int)(d_opt->min_seed_len * d_opt->split_factor + .499);
    int splitWidth = d_opt->split_width;
    int read_len = d_seqs[seq_id].l_seq;

    int seedID;
    //bwtintv_t *seeds2 = d_aux[seq_id].mem1.a; 
    int num_seeds = d_aux[seq_id].mem.n;
    if(num_seeds > MAX_LEN_READ)
    {
        return;
    }

    __shared__ uint8_t sharedRead[MAX_LEN_READ];
    __shared__ int sharedNumSeeds2[MAX_LEN_READ];
    __shared__ bwtintv_t* sharedSeed2Heads[MAX_LEN_READ];

	for(int j = threadIdx.x; j<read_len; j+=blockDim.x)
    {
		sharedRead[j] = (uint8_t)d_seqs[seq_id].seq[j];
    }
	__syncthreads(); __syncwarp();


    // check if each seed is a long seed, then re-seed if so, in parallel.
    int seedLen;
    bool isLong;
    int minIntervals, pos, count;
    for(seedID = threadIdx.x; seedID < num_seeds; seedID += blockDim.x)
    {
        seedLen = (uint32_t)(seeds[seedID].info) - (uint32_t)(seeds[seedID].info >> 32);
        isLong = seedLen >= splitLen && seeds[seedID].x[2] <= splitWidth;
        if(!isLong)
        {
            continue;
        }

        minIntervals = seeds[seedID].x[2] + 1;
        pos = (int)(((uint32_t)(seeds[seedID].info) + (uint32_t)(seeds[seedID].info >> 32)) >> 1);
        count = 0;
        //bwtintv_t *mem;
		//void *d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, blockIdx.x%32);
		//bwtintv_t *p = (bwtintv_t*)CUDAKernelMalloc(d_buffer_ptr, sizeof(bwtintv_t) * 64, 8);
		bwtintv_t *p = (bwtintv_t*)malloc(sizeof(bwtintv_t) * 64);
        reseedOnePos(devFmIndex, sharedRead, read_len, pos, minSeedLen, minIntervals, p, &count, d_buffer_pools);
        sharedNumSeeds2[seedID] = count;
        sharedSeed2Heads[seedID] = (bwtintv_t*)p;
    }
    __syncthreads(); __syncwarp(); 

    bwtintv_t *seeds2;
    if(threadIdx.x == 0)
    {
        for(seedID = 0; seedID < num_seeds; seedID++)
        {
            count = sharedNumSeeds2[seedID];
            seeds2 = sharedSeed2Heads[seedID];
            for(int j = 0; j < count; j++)
            {
                //printf("[BID %3d] Collected reseeds from seed %4d mem[%2d]: \
                        (%lu, %lu, %lu, %u, %u)\n", blockIdx.x, seedID, j,\
                        mem[j].x[0], mem[j].x[1], mem[j].x[2], M(mem[j]), N(mem[j]));
                d_aux[blockIdx.x].mem.a[num_seeds++] = seeds2[j];
            }
        }
        d_aux[blockIdx.x].mem.n = num_seeds;
    }
}


__device__ 
void reseedOnePos(const fmIndex *fmIndex, const uint8_t *read, \
        int read_len, int pos, int minSeedLen, int minInterval,\
        bwtintv_t* p, int *num_seeds2, void *d_buffer_pools)
{
    bwtintv_t *seeds = p;

    if(read[pos] >= 4)
    {
        *num_seeds2 = 0;
        return;
    }

    uint64_t *oneHot = fmIndex->oneHot;
    int64_t *count = fmIndex->count;
    int64_t *count2 = fmIndex->count;
    CP_OCC *cpOcc = fmIndex->cpOcc;
    CP_OCC2 *cpOcc2 = fmIndex->cpOcc2;
    int64_t sentinelIndex = *(fmIndex->sentinelIndex);

    int numLeps = 0;
#define MAX_NUM_LEP 32
    bwtintv_t leps[MAX_NUM_LEP];

    // collect LEPs
    int m, n;
    bwtintv_t seed, oldSeed;
    seed.x[0] = count[read[pos]];
    seed.x[1] = count[3 - read[pos]];
    seed.x[2] = count[read[pos] + 1] - count[read[pos]];
    seed.info = INFO(pos, pos);

    for(n = pos; n < read_len - 1; n++) // collect lep = read[pos..n]
    {
        oldSeed = seed;
        if(read[n + 1] >= 4)
        {
            break;
        }

        FORWARD_EXT(seed, read[n + 1]);
        seed.info = INFO(pos, n + 1);

        if(seed.x[2] < oldSeed.x[2])
        {
            leps[numLeps++] = oldSeed;
        }
        if(seed.x[2] < minInterval)
        {
            break;
        }
    }
    if(n == read_len - 1)
    {
        seed.info = INFO(pos, n);
        leps[numLeps++] = seed;
    }

    // reverse LEPs
    for(int j = 0; j < (numLeps >> 1); j++)
    {
        bwtintv_t temp = leps[j];
        leps[j] = leps[numLeps - 1 - j];
        leps[numLeps - 1 - j] = temp;
    }


    int num_seeds = 0;
    // backward Ext LEPs
    int numOld;
    bwtintv_t longestLep, oldLongestLep;
    bwtintv_t lep;
    int currInterval;
    uint8_t base;
    for(m = pos; m > 0; m--) // collect seed = read[m..n]
    {
        base = read[m - 1];
        if(base >= 4)
        {
            break;
        }

        oldLongestLep = longestLep = leps[0];
        BACKWARD_EXT(longestLep, base);
        numOld = numLeps;
        numLeps = 0;
        if(longestLep.x[2] >= minInterval)
        {
            longestLep.info = INFO(m - 1, N(longestLep));
            leps[numLeps++] = longestLep;
            currInterval = longestLep.x[2];
        }
        else
        {
            if(LEN(oldLongestLep) >= minSeedLen && oldLongestLep.x[2] >= minInterval)
            {
                oldLongestLep.info = INFO(m, N(longestLep));
                if(num_seeds == 64)
                {
                    return;
                }
                seeds[num_seeds++] = oldLongestLep;
            }
            currInterval = -1;
        }
        for(int j = 1; j < numOld; j++)
        {
            lep = leps[j];
            BACKWARD_EXT(lep, base);
            if(lep.x[2] > currInterval)
            {
                lep.info = INFO(m - 1, N(lep));
                leps[numLeps++] = lep;
                currInterval = lep.x[2];
            }
        }
        if(numLeps == 0)
        {
            break;
        }
    }
    if(numLeps > 0)
    {
        if(LEN(leps[0]) >= minSeedLen && leps[0].x[2] >= minInterval)
        {
            // NO INFO calc is necessary (since mSL > 1, etc)
            if(num_seeds == 64)
            {
                return;
            }
            seeds[num_seeds++] = leps[0];
        }
    }
    /*
    printf("All seed2s\n");
    for(int j=0; j < num_seeds; j++)
    {
        printf("lep[%d] (%lu, %lu, %lu, %d, %d)\n", \
                j, seeds[j].x[0], seeds[j].x[1], seeds[j].x[2], M(seeds[j]), N(seeds[j]));
    }
    */
    *num_seeds2 = num_seeds;
}

__device__ void reseedThirdRound(const fmIndex *fmIndex, const uint8_t *read, \
        int read_len, int minSeedLen, int maxIntervalSize,\
        bwtintv_t *seeds, int *num_seeds3)
{
    uint64_t *oneHot = fmIndex->oneHot;
    int64_t *count = fmIndex->count;
    int64_t *count2 = fmIndex->count;
    CP_OCC *cpOcc = fmIndex->cpOcc;
    CP_OCC2 *cpOcc2 = fmIndex->cpOcc2;
    int64_t sentinelIndex = *(fmIndex->sentinelIndex);

    int m = 0;
    int n = 0;
    int num_seeds = 0;

    while(m < read_len)
    {
        int next_m = m + 1;

        bwtintv_t seed;
        uint8_t base = (uint8_t)read[m];

        if(base < 4)
        {
            seed.x[0] = count[base];
            seed.x[1] = count[3 - base];
            seed.x[2] = count[base + 1] - count[base];

            for(n = m + 1; n < read_len; n++)
            {
                next_m = n + 1;
                base = (uint8_t)read[n];
                if(base < 4)
                {
                    FORWARD_EXT(seed, base);
                    if((seed.x[2] < maxIntervalSize) && (n-m+1) >= minSeedLen)
                    {
                        if(seed.x[2] > 0)
                        {
                            seed.info = INFO(m,n);
                            seeds[num_seeds++] = seed;
                        }
                        break;
                    }
                }
                else
                {
                    break;
                }
            }
        }
        m = next_m;
    }
    *num_seeds3 = num_seeds;
}

//
// SA -> Rbeg
//


#if 0
/**** TODO
    *   flatten intervals
    *   calc frac_rep
    *
    *
    */
__global__ void sa2ref(
	const mem_opt_t *d_opt,
    const fmIndex  *devFmIndex,
    const bntseq_t *d_bns,
	const bseq1_t *d_seqs,
	smem_aux_t *d_aux,
	mem_seed_v *seedsAllReads	// output
	)
{
    int seq_id = blockIdx.x;
    bwtintv_t *fatSeeds = d_aux[seq_id].mem.a;
    int numFatSeeds = d_aux[seq_id].mem.n;
    uint64_t maxIntervalSize = d_opt->max_occ;

    bwtintv_t *seeds = d_aux[seq_id].mem1.a;

    __shared__ uint64_t sharedCounts[MAX_NUM_SEEDS];
    __shared__ int sharedSteps[MAX_NUM_SEEDS];
    __shared__ uint64_t sharedOffsets[MAX_NUM_SEEDS];
    __shared__ int sharedNumSeeds;

    int fatSeedID;
    uint64_t intervalSize, count;
    int step;
    for(fatSeedID = threadIdx.x; fatSeedID < numFatSeeds; fatSeedID += blockDim.x)
    {
        step = 1;
        count = fatSeeds[fatSeedID].x[2];
        if(count > maxIntervalSize)
        {
            step = count / maxIntervalSize;
            count = maxIntervalSize;
        }
        sharedCounts[fatSeedID] = count;
        sharedSteps[fatSeedID] = step;
    }
    __syncthreads(); __syncwarp();

    // sequentially calculate the array of offsets
    if(threadIdx.x == 0)
    {
        sharedNumSeeds = 0;
        for(fatSeedID = 0; fatSeedID < numFatSeeds; fatSeedID++)
        {
            sharedOffsets[fatSeedID] = sharedNumSeeds;
            sharedNumSeeds += sharedCounts[fatSeedID];
        }
        seedsAllReads[blockIdx.x].n = sharedNumSeeds;
    }
    __syncthreads(); __syncwarp();

    // flatten each seed into singletons in parallel
    mem_seed_t *memSeeds = seedsAllReads[blockIdx.x].a;

    for(fatSeedID = threadIdx.x; fatSeedID < numFatSeeds; fatSeedID += blockDim.x)
    {
        bwtintv_t fatSeed = fatSeeds[fatSeedID];
        uint64_t k0 = fatSeed.x[0];
        uint64_t info = fatSeed.info;
        uint32_t qbeg = (uint32_t)(info>>32);
        uint32_t len = (uint32_t)info - qbeg;
        mem_seed_t memSeed;
        memSeed.qbeg = qbeg;
        memSeed.len = memSeed.score = len;
        memSeed.frac_rep = 0; //FIXME

        int offset = sharedOffsets[fatSeedID];
        step = sharedSteps[fatSeedID];
        count = sharedCounts[fatSeedID];
        for(int j = 0; j < count; j++)
        {
            uint64_t k = k0 + step * j;
            //memSeed.rbeg = bwt_sa_gpu(d_bwt, k);
            int64_t rbeg = 240830; // FIXME
            sa_lookup(devFmIndex, k, &rbeg);   // compressed sa as in BWA-MEM v1 and BWA-MEM-GPU.
            memSeed.rbeg = rbeg;
            memSeed.rid = bns_intv2rid_CUDA(d_bns, rbeg, rbeg + len);

            memSeeds[offset + j] = memSeed;
        }
    }
}


/* for each read, sort seeds by rbeg
	use cub::blockRadixSort
 */
// process reads who have less seeds
__global__ void sortSeeds_low(
	mem_seed_v *seedsAllReads,
	mem_seed_v *seedsAllReadsSortingBuffer
	)
{
	// seq_id = blockIdx.x
	int n_seeds = seedsAllReads[blockIdx.x].n;
	if (n_seeds==0) return;
	if (n_seeds>SORTSEEDSLOW_MAX_NSEEDS) return;

	mem_seed_t *seed_a = seedsAllReads[blockIdx.x].a;
	// declare sorting variables
	int64_t thread_keys[SORTSEEDSLOW_NKEYS_THREAD];	// this will contain rbeg
	int thread_values[SORTSEEDSLOW_NKEYS_THREAD];	// this will contain original seedID
	// load data
	for (int i=0; i<SORTSEEDSLOW_NKEYS_THREAD; i++){
		int seedID = threadIdx.x*SORTSEEDSLOW_NKEYS_THREAD+i;
		if (seedID < n_seeds) // load true data
			thread_keys[i] = seed_a[seedID].rbeg;
		else	// pad with INT64_MAX
			thread_keys[i] = INT64_MAX;
		if (seedID < n_seeds) thread_values[i] = seedID;	// original seedID
		else thread_values[i] = -1;
	}

	// Specialize BlockRadixSort
	typedef cub::BlockRadixSort<int64_t, SORTSEEDSLOW_BLOCKDIMX, SORTSEEDSLOW_NKEYS_THREAD, int> BlockRadixSort;
	// Allocate shared mem
	__shared__ typename BlockRadixSort::TempStorage temp_storage;
	// sort keys
	BlockRadixSort(temp_storage).Sort(thread_keys, thread_values);

	// reorder seeds to a new array
	mem_seed_t *seed_a_buffer = seedsAllReadsSortingBuffer[blockIdx.x].a;
	for (int i=0; i<SORTSEEDSLOW_NKEYS_THREAD; i++){
		int seedID = threadIdx.x*SORTSEEDSLOW_NKEYS_THREAD+i;
		if (seedID>=n_seeds) break;
		if (thread_values[i]==-1) {
			printf("Error: sorting result incorrect. SeqID=%d\n", blockIdx.x);
			__trap();
		}
		// copy to new array
		seed_a_buffer[seedID] = seed_a[thread_values[i]];
	}
    mem_seed_t *temp = seed_a;
	seedsAllReads[blockIdx.x].a = seed_a_buffer;
	seedsAllReadsSortingBuffer[blockIdx.x].a = temp;
}


// process reads who have more seeds
__global__ void sortSeeds_high(
	mem_seed_v *seedsAllReads,
	mem_seed_v *seedsAllReadsSortingBuffer
	)
{
	// seq_id = blockIdx.x
	int n_seeds = seedsAllReads[blockIdx.x].n;
	if (n_seeds<=SORTSEEDSLOW_MAX_NSEEDS) return;

	mem_seed_t *seed_a = seedsAllReads[blockIdx.x].a;
	// declare sorting variables
	int64_t thread_keys[SORTSEEDSHIGH_NKEYS_THREAD];	// this will contain rbeg
	int thread_values[SORTSEEDSHIGH_NKEYS_THREAD];	// this will contain original seedID
	// load data
	for (int i=0; i<SORTSEEDSHIGH_NKEYS_THREAD; i++){
		int seedID = threadIdx.x*SORTSEEDSHIGH_NKEYS_THREAD+i;
		if (seedID < n_seeds) // load true data
			thread_keys[i] = seed_a[seedID].rbeg;
		else	// pad with INT64_MAX
			thread_keys[i] = INT64_MAX;
		if (seedID < n_seeds) thread_values[i] = seedID;	// original seedID
		else thread_values[i] = -1;
	}

	// Specialize BlockRadixSort
	typedef cub::BlockRadixSort<int64_t, SORTSEEDSHIGH_BLOCKDIMX, SORTSEEDSHIGH_NKEYS_THREAD, int> BlockRadixSort;
	// Allocate shared mem
	__shared__ typename BlockRadixSort::TempStorage temp_storage;
	// sort keys
	BlockRadixSort(temp_storage).Sort(thread_keys, thread_values);

	// reorder seeds to a new array
	mem_seed_t *seed_a_buffer = seedsAllReadsSortingBuffer[blockIdx.x].a;
	for (int i=0; i<SORTSEEDSHIGH_NKEYS_THREAD; i++){
		int seedID = threadIdx.x*SORTSEEDSHIGH_NKEYS_THREAD+i;
		if (seedID>=n_seeds) break;
		if (thread_values[i]==-1) {
			printf("Error: sorting result incorrect. SeqID=%d\n", blockIdx.x);
			__trap();
		}
		// copy to new array
		seed_a_buffer[seedID] = seed_a[thread_values[i]];
	}
    mem_seed_t *temp = seed_a;
	seedsAllReads[blockIdx.x].a = seed_a_buffer;
	seedsAllReadsSortingBuffer[blockIdx.x].a = temp;
}
#endif

#if 0 
//NOT INUSE
/* this kernel is to filter out dups and count the actual number of seeds
	also spread out the seeds inside the same bwt interval
	output: d_aux such that each interval has length 1, aux->x[1] = l_rep
	if a bwt interval has length > opt->max_occ, only take max_occ seeds from it
	each warp process all seeds of a seq
 */
#define WARPSIZE 32
__global__ void separateSeeds(
	const mem_opt_t *d_opt,
	smem_aux_t *d_aux,
    void* d_buffer_pools
	)
{
	// seq_id = blockIdx.x
	bwtintv_t *mem_a = d_aux[blockIdx.x].mem.a;
	int n_mem = d_aux[blockIdx.x].mem.n;
	int max_occ = d_opt->max_occ;	// max length of an interval that we can count

	__shared__ uint32_t S_l_rep[1];		// repetitive length on read
	if (threadIdx.x==0) S_l_rep[0] = 0;
	__syncthreads();

	// write down to SM the number of seeds each mem as 
	__shared__ uint16_t S_nseeds[MAX_NUM_SEEDS];
	int n_iter = MAX_NUM_SEEDS/WARPSIZE;
	for (int i=0; i<n_iter; i++){
		int memID = i*WARPSIZE + threadIdx.x;
		if (memID>=n_mem) break;
		//if (mem_a[memID].info==0) {S_nseeds[memID] = 0; continue;}	// bad seed
		//if (memID>0 && (uint32_t)mem_a[memID].info==(uint32_t)mem_a[memID-1].info) S_nseeds[memID] = 0;	// duplicate
		//else {
			if (mem_a[memID].x[2] > max_occ) {
				S_nseeds[memID] = (uint16_t)max_occ;
				uint64_t info = mem_a[memID].info;
				uint32_t length = (uint32_t)info - (uint32_t)(info>>32);
				atomicAdd(&S_l_rep[0], length);
			}
			else S_nseeds[memID] = (uint16_t)mem_a[memID].x[2];
		//}
	}
	__syncthreads();
	// add total n_seeds and allocate new mem_a with this total
	int Sum = 0; int Sum32;
	for (int i=0; i<n_iter; i++){
		if (i*WARPSIZE>=n_mem) break;
		int memID = i*WARPSIZE + threadIdx.x;
		if (memID<n_mem) Sum32 = S_nseeds[memID];
		else Sum32 = 0;
		for (int offset=WARPSIZE/2; offset>0; offset/=2)
			Sum32 += __shfl_down_sync(0xffffffff, Sum32, offset);
		Sum += Sum32;
	}


	// now thread 0 has the correct Sum, allocate new mem_a on thread 0
	__shared__ uint16_t S_total_nseeds[1];
	__shared__ bwtintv_t* S_new_a[1];
	if (threadIdx.x==0){
		//void *d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, blockIdx.x%32);
		S_total_nseeds[0] = Sum;
		//bwtintv_t* new_a;
		//if (Sum==0) new_a = 0;
		//else {
			//new_a = (bwtintv_t*)CUDAKernelMalloc(d_buffer_ptr, Sum*sizeof(bwtintv_t), 8);
			//S_new_a[0] = new_a;
		//}
		// save this info to the original d_aux array
        if(Sum > MAX_NUM_SEEDS)
        {
		    void *d_buffer_ptr = CUDAKernelSelectPool(d_buffer_pools, blockIdx.x%32);
            d_aux[blockIdx.x].mem.a = (bwtintv_t*)CUDAKernelMalloc(d_buffer_ptr, Sum * sizeof(bwtintv_t), 8);
        }
        else
        {
            d_aux[blockIdx.x].mem.a = d_aux[blockIdx.x].mem1.a;
        }
		d_aux[blockIdx.x].mem.n = Sum;
        S_new_a[0] = d_aux[blockIdx.x].mem.a;

        //printf("block [%d] num total flattened seeds %d\n", blockIdx.x, Sum);
	}
	__syncthreads();
	Sum = S_total_nseeds[0];
	if (Sum==0) return;	// no seed
	bwtintv_t *new_a = S_new_a[0];
	// now write data to new_a from each thread
	int cumulative_total = 0; int memID = 0; int next_non0_memID;
	while (S_nseeds[memID]==0) memID++;	// find first non-0 memID
	if (Sum>S_nseeds[memID]){
		next_non0_memID = memID+1;
		while (S_nseeds[next_non0_memID]==0) next_non0_memID++;
	}
	n_iter = Sum/WARPSIZE + 1;
	for (int i=0; i<n_iter; i++){
		int seedID = i*WARPSIZE + threadIdx.x;
		if (seedID>=Sum) break;
		while (cumulative_total+S_nseeds[memID]<=seedID){
			cumulative_total+=S_nseeds[memID];
			memID = next_non0_memID; next_non0_memID++;
			if (cumulative_total+S_nseeds[memID]<Sum)	// find next non-0 memID
				while (S_nseeds[next_non0_memID]==0) next_non0_memID++;
		}
		int step;
		int intv_ID;	// index on mem_a[memID].x interval
		if (S_nseeds[memID]<max_occ) step = 1;
		else step = mem_a[memID].x[2] / max_occ;
		intv_ID = (seedID - cumulative_total)*step;
		bwtintv_t p;	// create a new point to write
		p.x[0] = mem_a[memID].x[0] + intv_ID;
		p.x[1] = (bwtint_t)S_l_rep[0];	// we will not need this later, so we use it to store l_rep
		p.x[2] = 1;		// only a single seed in this interval
		p.info = mem_a[memID].info;	// same match interval on read
		new_a[seedID] = p;	// write to global mem
	}
}
#endif

__global__ void reseedLastRound(
        const fmIndex *devFmIndex,
        const mem_opt_t *d_opt,
        uint8_t *d_seq,
        int *d_seq_offset,
        smem_aux_t *d_aux,
        kmers_bucket_t *d_kmerHashTab,
        int numReads)
{
    int seq_id = blockIdx.x * blockDim.x + threadIdx.x;
    if(seq_id >= numReads)
    { 
        return;
    }

    int num_seeds, num_seeds3;
    num_seeds = d_aux[seq_id].mem.n;
    bwtintv_t *seeds3 = d_aux[seq_id].mem.a + num_seeds;

    const uint8_t *read;
    int read_len;

    int seq_offset = d_seq_offset[seq_id];
    int seq_offset_next = d_seq_offset[seq_id + 1];
    read = d_seq + seq_offset;
    read_len = seq_offset_next - seq_offset;

    int minSeedLen = d_opt->min_seed_len + 1;
    int maxIntervalSize = d_opt->max_mem_intv;
    reseedThirdRound(devFmIndex, read, read_len, minSeedLen, maxIntervalSize, \
            seeds3, &num_seeds3);
    d_aux[seq_id].mem.n = num_seeds + num_seeds3;
}
