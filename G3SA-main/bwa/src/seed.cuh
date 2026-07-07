#ifndef _SEED_CUH 
#define _SEED_CUH 
#include "bwa.h"
#include "bwt.h"
#include <cub/cub.cuh>

#include <fstream>
using namespace std;
extern uint64_t proc_freq;

__global__ void preseedAndFilter(const fmIndex *devFmIndex, const mem_opt_t *d_opt, const bseq1_t *d_seqs, smem_aux_t *d_aux, kmers_bucket_t *d_kmerHashTab);


__global__ void preseedAndFilterV2(
        const fmIndex  *devFmIndex,
        const mem_opt_t *d_opt, 
        const uint8_t *d_seq,
        int *d_seq_offset,
        smem_aux_t *d_aux, 			// aux output
        kmers_bucket_t *d_kmerHashTab,
        void *d_buffer_pools);

/* find the SMEM starting at each position of the read 
	for each position, only extend to the right
	each block process a read
*/
__global__ void preseed(const fmIndex *devFmIndex, const mem_opt_t *d_opt, const bseq1_t *d_seqs, smem_aux_t *d_aux, kmers_bucket_t *d_kmerHashTab);

/* this kernel is to filter out non SMEM pre seeds
   */
__global__ void filterSeeds( const mem_opt_t *d_opt, smem_aux_t *d_aux);

// calculate necessary SMEM2 
__global__ void reseed(const fmIndex *devFmIndex, const mem_opt_t *d_opt, const bseq1_t *d_seqs, smem_aux_t *d_aux, kmers_bucket_t *d_kmerHashTab, void *d_buffer_pools);

__global__ void reseedV2(
        const fmIndex *devFmIndex,
        const mem_opt_t *d_opt, 
        uint8_t *d_seq,
        int *d_seq_offset,
        smem_aux_t *d_aux, 			// aux output
        kmers_bucket_t *d_kmerHashTab,
        void * d_buffer_pools,
        int num_reads
        );

// calculate necessary SMEM3
__global__ void reseedLastRound(
        const fmIndex *devFmIndex,
        const mem_opt_t *d_opt,
        uint8_t *d_seq,
        int *d_seq_offset,
        smem_aux_t *d_aux,
        kmers_bucket_t *d_kmerHashTab,
        int numReads);


/* for each seed:
	- calculate rbeg
	- calculate qbeg
	- calculate seed length = score
	- calculate rid
	- calculate frac_rep = l_rep/l_seq
	- sort seeds by rbeg
	1 block process all seeds of a read
	write output to d_seq_seeds
 */
__global__ void sa2ref(
	const mem_opt_t *d_opt,
    const fmIndex *devFmIndex,
	const bntseq_t *d_bns,
	const bseq1_t *d_seqs,
	smem_aux_t *d_aux,
	mem_seed_v *d_seq_seeds	// output
	);

// process reads who have less seeds
__global__ void sortSeeds_low(
	mem_seed_v *d_seq_seeds,
	mem_seed_v *seedsAllReadsSortingBuffer
	);


// process reads who have more seeds
__global__ void sortSeeds_high(
	mem_seed_v *d_seq_seeds,
	mem_seed_v *seedsAllReadsSortingBuffer
	);

/* convert all reads to bit encoding:
	A=0, C=1, G=2, T=3, N=4 (ambiguous)
	one block convert one read
	readID = blockIdx.x
 */
__global__ void to0123(const bseq1_t *d_seqs);

__global__ void separateSeeds(
	const mem_opt_t *d_opt,
	smem_aux_t *d_aux,
    void *d_buffer_pools
	);

#endif
