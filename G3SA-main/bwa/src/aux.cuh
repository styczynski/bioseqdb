#ifndef _AUX_CUH
#define _AUX_CUH

#include "bwa.h"
#include "gmem_alloc.h"
#include "bwt_CUDA.cuh"
#include "bntseq.h"
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
        void* d_buffer_pools);

/* convert all reads to bit encoding:
   A=0, C=1, G=2, T=3, N=4 (ambiguous)
   one block convert one read
   readID = blockIdx.x
 */
__global__ void PREPROCESS_convert_bit_encoding_kernel(const bseq1_t *d_seqs);



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
        );

/* for each read, sort seeds by rbeg
   use cub::blockRadixSort
 */
// process reads who have less seeds
__global__ void sortSeedsLowDim(
        mem_seed_v *d_seq_seeds,
        void *d_buffer_pools
        );


// process reads who have more seeds
__global__ void sortSeedsHighDim(
        mem_seed_v *d_seq_seeds,
        void *d_buffer_pools
        );


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
        );


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
        );


/* sort chains of each read by weight 
   shared-mem is pre-allocated to 3072*int
   assume that max(n_chn) is 3072
 */
__global__ void sortChainsDecreasingWeight(mem_chain_v* d_chains, void* d_buffer_pools);



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
        void* d_buffer_pools);


__global__ void CHAINFILTERING_flt_chained_seeds_kernel(
        const mem_opt_t *d_opt, const bntseq_t *d_bns, const uint8_t *d_pac, const bseq1_t *d_seqs,
        mem_chain_v *d_chains, 	// input and output
        int n,		// number of seqs
        void* d_buffer_pools
        );


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
        );

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
        );

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
        );

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
        );

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
        );


/*
   reorder alignments: sorting by increasing is_alt, then decreasing score
   run at thread level: each thread process all aligments of a read
 */
__global__ void sortRegions(
        mem_alnreg_v *d_regs,
        int n_seqs,
        void *d_buffer_pools
        );

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
        void* d_buffer_pools);


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
        void* d_buffer_pools);

/*
   this kernel reverse both the query and reference for alns whose position is on the reverse strand
   this is to ensure indels to be placed at the leftmost position
   each block process one aln
 */
__global__ void FINALIZEALN_reverseSeq_kernel(seed_record_t *d_seed_records, mem_aln_v *d_alns, void *d_buffer_pools);

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
        );


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
        );

/* execute at aln level 
   calculate pos, rid & fix cigar: remove leading or trailing del, add clipping
   TODO gen sam
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
        );


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
        const uint8_t *d_seq,
        int *d_seq_offset,
        mem_aln_v *d_alns,
        seed_record_t *d_seed_records,
        int Nseeds,
        void *d_buffer_pools
        );

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
        const uint8_t *d_seq,
        int *d_seq_offset,
        int n_seqs,
        char* d_seq_sam_ptr, int *d_seq_sam_size
        );

#define chn_beg(ch) ((ch).seeds->qbeg)
#define chn_end(ch) ((ch).seeds[(ch).n-1].qbeg + (ch).seeds[(ch).n-1].len)
#define MEM_SHORT_EXT 50
#define MEM_SHORT_LEN 200
#define MEM_HSP_COEF 1.1f
#define MEM_MINSC_COEF 5.5f
#define MEM_SEEDSW_COEF 0.05f
#define MAX_BAND_TRY  2
#define PATCH_MAX_R_BW 0.05f
#define PATCH_MIN_SC_RATIO 0.90f
#define MIN_RATIO     0.8
#define MIN_DIR_CNT   10
#define MIN_DIR_RATIO 0.05
#define OUTLIER_BOUND 2.0
#define MAPPING_BOUND 3.0
#define MAX_STDDEV    4.0
#define raw_mapq(diff, a) ((int)(6.02 * (diff) / (a) + .499))
#define start_width 1
#define SORTSEEDSHIGH_MAX_NSEEDS 	2048
#define SORTSEEDSHIGH_NKEYS_THREAD	16
#define SORTSEEDSHIGH_BLOCKDIMX		128
#define SORTSEEDSLOW_MAX_NSEEDS 	64
#define SORTSEEDSLOW_NKEYS_THREAD	2
#define SORTSEEDSLOW_BLOCKDIMX		32
#define SEEDS_PER_CHAIN 4
#define MAX_N_CHAIN 		4096
#define NKEYS_EACH_THREAD	16
#define SORTCHAIN_BLOCKDIMX	128
#define CHAIN_FLT_BLOCKSIZE 256
#define GET_KEPT(i) (chn_info_SM[i]&0x3) 		// last 2 bits
#define SET_KEPT(i, val) (chn_info_SM[i]&=0b11111100)|=val
#define GET_IS_ALT(i) ((chn_info_SM[i]&0x4)>>2) 	// 3rd bit
#define SET_IS_ALT(i, val) (chn_info_SM[i]&=0b11111011)|=(val<<2)
#define MAX_N_ALN 3072	// max number of alignments allowed per read
#define GLOBALSW_BANDWITH_CUTOFF 500
#endif
