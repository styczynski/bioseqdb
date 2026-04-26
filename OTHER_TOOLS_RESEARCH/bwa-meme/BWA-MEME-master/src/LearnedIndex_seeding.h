/*
 * The MIT License
 *
 * Copyright (C) 2021  Youngmok Jung, Dongsu Han.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 * Authors: Youngmok Jung <tom418@kaist.ac.kr>; Dongsu Han <dhan.ee@kaist.ac.kr>;
 */
#ifndef LEARNEDSEEDING_HPP
#define LEARNEDSEEDING_HPP

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <vector>
#include "kstring.h"
#include "ksw.h"
#include "kvec.h"
#include "ksort.h"
#include "utils.h"
#include "bwt.h"
#include "bntseq.h"
#include "bwa.h"
#include "macro.h"
#include "profiling.h"

#include "memcpy_bwamem.h"
#if (__AVX512BW__ || __AVX2__)
#include <immintrin.h>
#else
#include <smmintrin.h>  // for SSE4.1
#define __mmask8 uint8_t
#define __mmask16 uint16_t
#endif
#include "ertseeding.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "safe_mem_lib.h"
#include "safe_str_lib.h"
#include <snprintf_s.h>
#ifdef __cplusplus
}
#endif

#include <cstddef>
#include <cstdint>

/*
	1: Without 64bit key and ISA
	2: Without ISA
	3: BWA-MEME full
*/
// Change Mode 
// #define MODE 3


// No need to change below
#if MODE==1
#define LOADSUFFIX 0
#define MEM_TRADEOFF 0
#elif MODE == 2
#define LOADSUFFIX 1
#define MEM_TRADEOFF 0
#elif MODE == 3
#define LOADSUFFIX 1
#define MEM_TRADEOFF 1
#endif



#define READ_FROM_FILE 0  // set 0 to build in runtime, should not be turned on when using compact index

// Change to test last mile search method or print query length and number of last mile search
#define CURR_SEARCH_METHOD 1 // 2: exponential 1: P-RMI 0: binary
#define Count_mem_ref 0


// No need to change below
#define PREFETCH 1
#define EXPONENTIAL_SMEMSEARCH 1
#define EXPONENTIAL_EXP_START 5
#define EXPONENTIAL_EXP_POW 1
#define MEM_TRADEOFF_CACHED 1
#define MEM_TRADEOFF_USECACHE_THRESHOLD 18 // 
#define MEM_TRADEOFF_USECACHE_EXP_SEARCH_START 2
#define MEM_TRADEOFF_USECACHE_EXP_SEARCH_POW 5 // 
#define DEBUG_MODE 0
// #define COMPACT_INDEX 1 // 0 for build in runtime with non compact
#define PAD_1 1
#if LOADSUFFIX
	#define SASIZE 13
#else
	#define SASIZE 5
#endif


// /**
//  * State for each maximal-exact-match (MEM)
//  */
typedef struct {
	int start;          // MEM start position in read
	int end;            // MEM end position in read. [start, end)
	int hitbeg;         // Index into hit array
	int hitcount;       // Count of hits
	uint64_t cache_refpos; // length of cached match
} mem_tl;

// BitReverseTable256 is used to swap 2-bit encoded DNA sequence 
// e.g [A,C,G,T] -> [T,G,C,A]   [00/01/10/11] -> [11/10/01/00]
static const unsigned char BitReverseTable256[256] = 
{
#   define R2(n)     n,     n + 1*64,     n + 2*64,     n + 3*64
#   define R4(n) R2(n), R2(n + 1*16), R2(n + 2*16), R2(n + 3*16)
#   define R6(n) R4(n), R4(n + 1*4 ), R4(n + 2*4 ), R4(n + 3*4 )
    R6(0), R6(1), R6(2), R6(3)
};

typedef kvec_t(mem_tl) mem_tlv;


typedef struct {
	uint8_t* sa_pos;
	uint8_t* ref2sa;         // ref2sa
	const bwt_t* bwt;           // FM-index
	const bntseq_t* bns;        // Input reads sequences
	const uint8_t* pac;         // Reference genome (2-bit encoded)
  uint8_t* ref_string;
} Learned_index_aux_t;

/**
 * 'Read' auxiliary data structures
 */
typedef struct {
	int min_seed_len;               // Minimum length of seed
	int l_seq;                      // Read length
	int num_hits;                   // Number of hits for each node in the ERT
    int pivot; // right, forward pivot
    int l_pivot; // left, backward pivot
	int max_pivot; // longest match start position in right extension 
	int max_l_seq; // length of longest match
	uint64_t max_refpos; // length of longest match
	int cache_pivot; // cached match start position in right extension  
	int cache_pivot_end; // length of cached match 
	uint64_t cache_refpos; // length of cached match
	int min_intv_limit;                      // Number of hits after which extension must be stopped
	char* read_name;                // Read name
	uint8_t* unpacked_queue_buf;    // Read sequence (2-bit encoded)
	uint8_t* unpacked_rc_queue_buf; // Reverse complemented read (2-bit encoded)
	uint8_t* unpacked_queue_binary_buf_shift1;    // Read sequence (2-bit encoded)
	uint8_t* unpacked_queue_binary_buf_shift2;    // Read sequence (2-bit encoded) 2-bit shifted
	uint8_t* unpacked_queue_binary_buf_shift3;    // Read sequence (2-bit encoded) 4-bit shifted
	uint8_t* unpacked_queue_binary_buf_shift4;    // Read sequence (2-bit encoded) 6-bit shifted
	uint8_t* unpacked_rc_queue_binary_buf_shift1;    // Read sequence (2-bit encoded)
	uint8_t* unpacked_rc_queue_binary_buf_shift2;    // Read sequence (2-bit encoded)
	uint8_t* unpacked_rc_queue_binary_buf_shift3;    // Read sequence (2-bit encoded)
	uint8_t* unpacked_rc_queue_binary_buf_shift4;    // Read sequence (2-bit encoded)
} Learned_read_aux_t;
#define _get_pac_bigendian(pac, l) ( BitReverseTable256[(pac)[(l)>>2]]>>((~(l)&3)<<1)&3)

/*
 * Generate key from reference sequence
 */
inline uint64_t get_key_of_ref(uint8_t* pac , uint64_t pos, uint64_t len){
    uint64_t key=0,r__=32;
    // assert(len!=0);
    for (; r__>= len; r__--){
        
        key = key << 2;
        key |= 3;
    }
    for (; r__>=0; r__--){
        // fprintf(stderr,"%llu\n",key);
        key = key << 2;
        key |= _get_pac_bigendian(pac,pos+r__);
        if (r__==0){
            break;
        }
    }
    return key;
};


/*
 * Add SMEM and (only) perform right(forward) extension
 */
uint64_t right_smem_search(const uint8_t* ref_string,const uint8_t* sa_pos,const uint8_t* pac, uint64_t sa_num,
                   Learned_read_aux_t* raux,  const uint64_t est_pos,
                    uint64_t err, uint32_t* exact_match_len, mem_tlv* smems, u64v* hits, uint32_t* ambiguous_pos);
/*
 * Doesn't perform SMEM adding, but perform extensions for both right(forward) and left(backward) direction
 */
uint64_t mem_search(const uint8_t* ref_string,const uint8_t* sa_pos,const uint8_t* pac, uint64_t sa_num,
                   Learned_read_aux_t* raux, const uint64_t est_pos,
                   uint64_t err, bool right_forward, uint32_t* exact_match_len, uint32_t* ambiguous_pos);
/*
 * Same as mem_search, but given estimated position from Inverse Suffix Array (ref2sa)
 * mem_search_tradeoff performs exponential search when estimation is not perfect
 * If whole short-read has a full exact match, exponential search (last-mile search) is unnecessary.
 */
uint64_t mem_search_tradeoff(const uint8_t* ref_string,const uint8_t* sa_pos,const uint8_t* pac, uint64_t sa_num,
                   Learned_read_aux_t* raux, const uint64_t est_pos,
                   bool right_forward, uint32_t* exact_match_len, uint32_t* ambiguous_pos, bool no_search);				   
/*
 * Same as right_smem_search, but given estimated position from Inverse Suffix Array (ref2sa)
 * right_smem_search_tradeoff performs exponential search when estimation is not perfect
 * If whole short-read has a full exact match, exponential search (last-mile search) is unnecessary.
 */
uint64_t right_smem_search_tradeoff(const uint8_t* ref_string,const uint8_t* sa_pos,const uint8_t* pac, uint64_t sa_num,
                   Learned_read_aux_t* raux,  const uint64_t est_pos,
                    uint32_t* exact_match_len, mem_tlv* smems, u64v* hits, uint32_t* ambiguous_pos, bool no_search);				   
/*
 * Same as bwtSeedStrategyAllPosOneThread in BWA-MEM2
 */
void Learned_bwtSeedStrategyAllPosOneThread(Learned_index_aux_t* iaux, Learned_read_aux_t* raux, mem_tlv* smems, u64v* hits, bool hasN);

/*
 * Same as Learned_bwtSeedStrategyAllPosOneThread, but assumes using 
 * Inverse Suffix Array
 */
void Learned_bwtSeedStrategyAllPosOneThread_mem_tradeoff(Learned_index_aux_t* iaux, Learned_read_aux_t* raux, mem_tlv* smems, u64v* hits, bool hasN);



void Learned_getSMEMsOnePosOneThread_no_smem(Learned_index_aux_t* iaux, Learned_read_aux_t* raux, mem_tlv* smems, u64v* hits, bool hasN, bool use_cached=false);

/*
 * Same as getSMEMsAllPosOneThread in BWA-MEM2
 * Does not integrate step2 in seeding, getSMEMsOnePosOne
 * Find all seeds that cover each point of the short read
 */
void Learned_getSMEMsAllPosOneThread_step1only(Learned_index_aux_t* iaux, Learned_read_aux_t* raux, mem_tlv* smems, u64v* hits, bool hasN, int split_len, int split_width);

/*
 * Same as getSMEMsAllPosOneThread in BWA-MEM2
 * Integrates step2 in seeding
 * Find all seeds that cover each point of the short read
 * Also find shorter seeds if seed is longer (split_len) and have few matches (split_width)
 */
void Learned_getSMEMsAllPosOneThread(Learned_index_aux_t* iaux, Learned_read_aux_t* raux, mem_tlv* smems, u64v* hits, bool hasN, int split_len, int split_width);

/*
 * Given a pivot point, find all SMEMs that cover the pivot point
 */
void Learned_getSMEMsOnePosOneThread(Learned_index_aux_t* iaux, Learned_read_aux_t* raux, mem_tlv* smems, u64v* hits, bool hasN, bool use_cached=false);

/*
 * Given a pivot point, find all SMEMs until zigzag style extension terminates
 * Uses less or equal number of extension compared to Learned_getSMEMsOnePosOneThread
 */
void Learned_getSMEMsOnePosOneThread_step1(Learned_index_aux_t* iaux, Learned_read_aux_t* raux, mem_tlv* smems, u64v* hits, bool hasN, bool use_cached=false);

/*
 * sa_raux_buf is used to select properly aligned read among unpacked_queue_binary_buf_shift[1,2,3,4]
 */
extern uint8_t sa_raux_buf[4][4];

/*
 * Tokenize input read into 64-bit key
 * Check if "N" ambiguous base exists and substitue clip bases after "N"
 * If input read is shorter than 32-base, pad with arbitrary base
 * If input read is longer, use only the first 32-base
 */
inline uint64_t Tokenization( Learned_read_aux_t* raux, bool right_forward, uint32_t* ambiguous_pos, bool hasN);


/*
 * Load learned-index parameters
 */
bool learned_index_load(char const* dataPath, char const* dataPath2,char const* dataPath3, double suffix_array_num);

void learned_index_cleanup();

/*
 * Learned-index lookup function
 */
inline uint64_t learned_index_lookup(uint64_t key, size_t* err);
extern double L0_PARAMETER0;
extern double L0_PARAMETER1;
extern double SA_NUM;
extern char* L1_PARAMETERS;
extern char* L2_PARAMETERS;


#endif
