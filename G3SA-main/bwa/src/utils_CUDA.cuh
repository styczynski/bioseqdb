#ifndef PRINTINTERMEDIATES_CUH
#define PRINTINTERMEDIATES_CUH
#include "bwa.h"

// format: phase readID qbeg qend num_hits sa_k
__global__ void printIntv(smem_aux_t *d_intvvecs, int readID, int type);

// format: phase readID rbeg len qbeg
__global__ void printSeed(mem_seed_v *d_seedvecs, int readID, int type);

// format: phase readID rpos weight num_seeds
__global__ void printChain(mem_chain_v *d_chainvecs, int readID, int type);

// format: phase readID q_left r_left q_right r_right
__global__ void printPair(seed_record_t *d_pairs, int num_records, int type);

// format: phase readID rb re qb qe score w seedcov frac_rep seedlen0
__global__ void printReg(mem_alnreg_v *d_regvecs, int readID, int type);

// format: phase readID rname rpos cigarstring
__global__ void printAln(bntseq_t *d_bns, mem_aln_v *d_alnvecs, int readID, int type);
#endif
