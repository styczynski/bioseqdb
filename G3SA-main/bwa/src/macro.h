#ifndef _MACRO_H
#define _MACRO_H

#define CUDA_MALLOC_CAP 10000000000

#define MAX_BATCH_SIZE 80000 // per GPU.
#define MAX_ALN_CNT 160000

#define MAX_NUM_GPUS 8

#define NUM_BLOCKS 128
#define BLOCKDIM 256



// constants
#define WARPSIZE 32

#define MB_SIZE (1<<20)
#define GB_SIZE (1<<30)

#define SB_MAX_COUNT 1000000                   // max number of reads



// Constant values
#define MAX_LEN_READ 320 // max. length of input short read
#define SEQ_MAXLEN MAX_LEN_READ// max length of a seq we want to process
#define AVG_NUM_SEEDS 8

#define MAX_NUM_SW_SEEDS 512 // max. number of seed counts in all chains of a read, including dups.

#define MAX_N_CIGAR 16

//
// Compile options
//

//
// Index building & Seeding options
//#define FORWARD // preseeding option // THIS MUST NOT BE DEFINED
#define STRIDED // THIS MUST BE DEFINED

#define SA_COMPRESSION
//#define INDEX_ONLY
#define STUDYING_HG38
//#define FORWARD_STRAND_ONLY

//
#define BACKWARD_EXT(seed, base)\
        backwardExt(sentinelIndex, &seed, base, &seed, oneHot, cpOcc, count);

// that is, in the "base1 <- base0 <- seed" direction.
#define BACKWARD_EXT2(seed, base0, base1)\
        backwardExt2(sentinelIndex, &seed, base0, base1, &seed, oneHot, cpOcc, count, cpOcc2, count2, &firstBase);

#define BACKWARD_EXT_B(seed, base)\
        backwardExtBackward(sentinelIndex, &seed, base, &seed, oneHot, cpOcc, count);

// that is, in the "base1 <- base0 <- seed" direction.
#define BACKWARD_EXT2_B(seed, base0, base1)\
        backwardExt2Backward(sentinelIndex, &seed, base0, base1, &seed, oneHot, cpOcc, count, cpOcc2, count2, &firstBase);

#define FORWARD_EXT(seed, base);\
            {uint64_t temp = seed.x[0];\
            seed.x[0] = seed.x[1];\
            seed.x[1] = temp;\
            BACKWARD_EXT(seed, 3 - (base));\
            temp = seed.x[0];\
            seed.x[0] = seed.x[1];\
            seed.x[1] = temp;}

#define FORWARD_EXT2(seed, base0, base1);\
            {uint64_t temp = seed.x[0];\
            seed.x[0] = seed.x[1];\
            seed.x[1] = temp;\
            BACKWARD_EXT2(seed, 3 - (base0), 3 - (base1));\
            temp = seed.x[0];\
            seed.x[0] = seed.x[1];\
            seed.x[1] = temp;}
    
#define LEN(seed) ((uint32_t)((seed).info) - (uint32_t)(((seed).info)>>32))
#define N(seed) ((uint32_t)((seed).info) - 1)
#define M(seed) ((uint32_t)((seed).info >> 32))
#define INFO(m, n) ((((uint64_t)(m)) << 32) | (uint64_t)(n + 1))
#define PLEN(seed) ((uint32_t)(seed->info) - (uint32_t)((seed->info)>>32))
#define PN(seed) ((uint32_t)(seed->info) - 1)
#define PM(seed) ((uint32_t)(seed->info >> 32))

#define BAM2LEN(bam)    ((int)(bam>>4))
#define BAM2OP(bam)     ((char)("MIDSH"[(int)bam&0xf])) 
#endif
