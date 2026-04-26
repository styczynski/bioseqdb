/*************************************************************************************
                           The MIT License

   BWA-MEM2  (Sequence alignment using Burrows-Wheeler Transform),
   Copyright (C) 2019  Intel Corporation, Heng Li.

   Permission is hereby granted, free of charge, to any person obtaining
   a copy of this software and associated documentation files (the
   "Software"), to deal in the Software without restriction, including
   without limitation the rights to use, copy, modify, merge, publish,
   distribute, sublicense, and/or sell copies of the Software, and to
   permit persons to whom the Software is furnished to do so, subject to
   the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
   BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
   ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.

Authors: Vasimuddin Md <vasimuddin.md@intel.com>; Sanchit Misra <sanchit.misra@intel.com>.
*****************************************************************************************/

#include<stdio.h>
#include<stdlib.h>
#include<stdint.h>
#include <omp.h>
// #include <string.h>
#include "sais.h"
#include "bwa.h"
#include "FMI_search.h"
#include "LearnedIndex_seeding.h"
#include <omp.h>
#include <fstream>

#ifdef VTUNE_ANALYSIS
#include <ittnotify.h> 
#endif
#include "kseq.h"
KSEQ_DECLARE(gzFile)


#ifdef __GNUC__
#  define ffs(x) __builtin_ffsll(x)
#  define fls(x) __builtin_clzll(x)
#elif __INTEL_COMPILER
#  define ffs(x) _bit_scan_forward64(x)
#  define fls(x) __builtin_clzll(x)
#endif


// #include "kstring.h"
// #include "kvec.h"

// 10000000 * threadnum

uint64_t* SAi;
uint64_t* SAindexstart;




#define QUERY_DB_SIZE 10000000

#define PRINT_OUTPUT

#define smem_lt_2(a, b) ((a).start == (b).start ? (a).end > (b).end : (a).start < (b).start)
KSORT_INIT(mem_smem_sort_lt, mem_tl, smem_lt_2)

#define _set_pac(pac, l, c) ((pac)[(l)>>2] |= (c)<<((~(l)&3)<<1))
#define _get_pac(pac, l) ((pac)[(l)>>2]>>((~(l)&3)<<1)&3)

inline void set_forward_pivot(Learned_read_aux_t* raux, int pivot){
	raux->pivot = pivot;
	raux->l_pivot = raux->l_seq-1 - raux->pivot;
}

inline uint64_t get_sa_pos(char *sa, uint64_t pos){
    uint64_t sa_pos = *(uint32_t*)(sa + (5*pos));
	sa_pos = (sa_pos <<8) | (uint8_t)sa[ (5*pos) + 4];
    return sa_pos;
}

uint64_t tokenize2lmer(char* binary_ref_seq, int64_t pac_len, uint64_t position, int L_MER_SIZE){
    uint64_t key=0;
    int len = L_MER_SIZE, r;
    for ( r=0; r< len && position + r < pac_len ; r++){
        assert(position + r < pac_len);
        key = key << 2;
        key |= binary_ref_seq[position + r];
        // printf("char:%ld\n", binary_ref_seq[position + r]);
    }
    for (; r < len; r++){
        key = key << 2;
    }
    return key;
}


inline bool compare_read_and_ref_binary(const uint8_t* pac, const uint8_t* sa,const uint64_t pos, const Learned_read_aux_t* raux, const uint64_t sa_num,
                             const uint64_t valid_len,  uint32_t* match_len, bool* exact_match_flag){
    uint64_t sa_pos = *(uint32_t*)(sa + pos);
	sa_pos = (sa_pos << 8LLU) |  (uint8_t)sa[pos+4];
	uint64_t ref_len=sa_num - sa_pos;
	uint64_t compare; 
	uint32_t read_len= (uint32_t)std::min(ref_len,valid_len);
	uint32_t sa_pos_rest = sa_pos&3;
	uint8_t* ref_pos = pac + (sa_pos>>2);
	uint8_t* read_string;
	switch(sa_raux_buf[raux->pivot&3][sa_pos_rest])
	{
		case 1:
			read_string = raux->unpacked_queue_binary_buf_shift1 + ((raux->pivot + 3)>>2);
			break;
		case 2:
			read_string = raux->unpacked_queue_binary_buf_shift2+((raux->pivot + 2)>>2);
			break;
		case 3:
			read_string = raux->unpacked_queue_binary_buf_shift3+ ((raux->pivot + 1)>>2);
			break;
		case 0:
			read_string = raux->unpacked_queue_binary_buf_shift4+(raux->pivot>>2);
			break;
	}
	
	uint64_t ref_val = (*(uint64_t*)(ref_pos)) ;
	uint64_t read_val = (*(uint64_t*)(read_string));
	uint32_t compare_len;
	compare = ref_val^read_val;
	compare = compare >> (sa_pos_rest<<1);
	if( compare != 0 ){
		compare_len = ((ffs(compare)-1)>>1);
		if (read_len <= compare_len){
			*match_len = read_len;	
			// if ( read_len<ref_len ){ 
			if ( read_len<ref_len ){ 
				*exact_match_flag= true;
				return true;
			}else{
				//when Suffix array contains end of ref, which is padded TTTTTTTT
				*match_len = ref_len;
				return false;
			}
		}else{
			*match_len = compare_len;
			compare_len += sa_pos_rest;
			return (ref_val << (62-(compare_len<<1))) < (read_val << (62-(compare_len<<1)));
		}
	}
	for (uint64_t i=8; (i<<2) <read_len+sa_pos_rest ; i +=8){
		ref_val = (*(uint64_t*)(ref_pos+i)) ;
		read_val = (*(uint64_t*)(read_string+i)) ;
		compare = ref_val^read_val;
		if(compare != 0 ){
			compare_len = (i<<2)+ ((ffs(compare)-1)>>1) - sa_pos_rest;
			if (read_len <= compare_len){
				*match_len = read_len;	
				if (read_len<ref_len){ // if read_len == valid_len
					*exact_match_flag= true;
					return true;
				}else{
					//when Suffix array contains end of ref, which is padded TTTTTTTT
					*match_len = ref_len;
					return false;
				}
			}else{
				*match_len = compare_len;
				compare_len +=  sa_pos_rest-(i<<2);
				return (ref_val << (62-(compare_len<<1))) < (read_val << (62-(compare_len<<1)));
			}
		}
	}
	*match_len = read_len;	
	if (read_len<ref_len){ // if read_len == valid_len
		*exact_match_flag= true;
		return true;
	}else{
		//when Suffix array contains end of ref, which is padded TTTTTTTT
		*match_len = ref_len;
		return false;
	}

}
inline bool compare_read_and_ref_binary_left(const uint8_t* pac, const uint8_t* sa,const uint64_t pos, const Learned_read_aux_t* raux, const uint64_t sa_num, const uint64_t valid_len,  uint32_t* match_len, bool* exact_match_flag){
	uint64_t sa_pos = *(uint32_t*)(sa + pos);
	sa_pos = sa_pos <<8 |  sa[pos+4];
	uint64_t ref_len=sa_num - sa_pos;
	uint64_t compare; 
	uint32_t read_len= (uint32_t)std::min(ref_len,valid_len);
	uint32_t sa_pos_rest = sa_pos&3;
	uint8_t* ref_pos = pac + (sa_pos>>2);
	uint8_t* read_string;
	switch(sa_raux_buf[raux->l_pivot&3][sa_pos_rest])
	{
		case 1:
			read_string = raux->unpacked_rc_queue_binary_buf_shift1+((raux->l_pivot + 3)>>2);
			break;
		case 2:
			read_string = raux->unpacked_rc_queue_binary_buf_shift2+((raux->l_pivot + 2)>>2);
			break;
		case 3:
			read_string = raux->unpacked_rc_queue_binary_buf_shift3 + ((raux->l_pivot + 1)>>2);
			break;
		case 0:
			read_string = raux->unpacked_rc_queue_binary_buf_shift4 + (raux->l_pivot>>2);
			break;

	}
	uint64_t ref_val = (*(uint64_t*)(ref_pos)) ;
	uint64_t read_val = (*(uint64_t*)(read_string));
	uint32_t compare_len;
	compare = ref_val^read_val;
	compare = compare >> (sa_pos_rest<<1);
	if(compare != 0 ){
		compare_len = ((ffs(compare)-1)>>1);
		if (read_len <= compare_len){
			*match_len = read_len;	
			if (read_len<ref_len){ // if read_len == valid_len
				*exact_match_flag= true;
				return true;
			}else{
				//when Suffix array contains end of ref, which is padded TTTTTTTT
				*match_len = ref_len;
				return false;
			}
		}else{
			*match_len = compare_len;
			compare_len += sa_pos_rest;
			return (ref_val << (62-(compare_len<<1))) < (read_val << (62-(compare_len<<1)));
		}
	}

	for (uint64_t i=8; (i<<2) <read_len+sa_pos_rest ; i +=8){
		ref_val = (*(uint64_t*)(ref_pos+i)) ;
		read_val = (*(uint64_t*)(read_string+i)) ;
		compare = ref_val^read_val;
		if(compare != 0 ){
			compare_len = (i<<2)+ ((ffs(compare)-1)>>1) - sa_pos_rest;
			if (read_len <= compare_len){
				*match_len = read_len;	
				if (read_len<ref_len){ // if read_len == valid_len
					*exact_match_flag= true;
					return true;
				}else{
					//when Suffix array contains end of ref, which is padded TTTTTTTT
					*match_len = ref_len;
					return false;
				}
			}else{
				*match_len = compare_len;
				compare_len+= sa_pos_rest -(i<<2);
				return (ref_val << (62-(compare_len<<1))) < (read_val << (62-(compare_len<<1)));
			}
		}

	}
	*match_len = read_len;	
	if (read_len<ref_len){ // if read_len == valid_len
		*exact_match_flag= true;
		return true;
	}else{
		//when Suffix array contains end of ref, which is padded TTTTTTTT
		*match_len = ref_len;
		return false;
	}
}

uint64_t maxMappableLength(const uint8_t* ref_string,const uint8_t* sa_pos,const uint8_t* pac, uint64_t sa_num,
                        Learned_read_aux_t* raux, uint64_t read_valid_len, const uint64_t low_, const uint64_t up_,
                        bool right_forward, uint32_t* exact_match_len){
    
    // perform binary search to find the MMP postion
    assert(up_ > low_);
    uint64_t upper_b = up_;
    uint64_t lower_b = low_;
    uint64_t n = upper_b-lower_b + 1; // `end` is inclusive.
    uint64_t middle, iter_pos;
    uint32_t  match_len, last_match_len;
    bool exact_match_flag=false;
    while (uint64_t half = (n>>1)) { //binary search
        middle = lower_b + half;
    #if PREFETCH
        _mm_prefetch(sa_pos+ ((middle+half/2)*5), _MM_HINT_T0);
        _mm_prefetch(sa_pos+((lower_b+half/2)*5), _MM_HINT_T0);
    #endif
        if (right_forward){
            lower_b = compare_read_and_ref_binary(pac, sa_pos, 5*middle, raux, sa_num,read_valid_len, &match_len,&exact_match_flag)? middle: lower_b;
        }
        else{
            lower_b = compare_read_and_ref_binary_left(pac, sa_pos, 5*middle, raux, sa_num,read_valid_len, &match_len,&exact_match_flag)? middle: lower_b;
        }
        if(exact_match_flag){
            break;
        }
        n -= half;
    }
    if (exact_match_flag){
			iter_pos = lower_b;
    }else{
        if(middle!=lower_b){
            // lookup key was smaller than middle, should check lower_b
            last_match_len = match_len;
            iter_pos = lower_b;
            if (right_forward){
                while (!compare_read_and_ref_binary(pac, sa_pos, 5*iter_pos, raux, sa_num,read_valid_len, &match_len,&exact_match_flag)&& !exact_match_flag ){
                    if (iter_pos ==0){
                        break;
                    }
                    iter_pos --;
                    last_match_len = match_len;
                }
            }else{
                while (!compare_read_and_ref_binary_left(pac, sa_pos, 5*iter_pos, raux, sa_num,read_valid_len, &match_len,&exact_match_flag)&& !exact_match_flag ){
                    if (iter_pos ==0){
                        break;
                    }
                    iter_pos --;
                    last_match_len = match_len;
                }
            }
            
            //  stops when ref is smaller than read, iter_pos should point to exact matching or bigger ref pos
            if (last_match_len > match_len){
                iter_pos++;
                match_len = last_match_len;
            }
        }
        else{
            // lookup key was bigger than middle, should check lower_b+1
            last_match_len = match_len;
            //  estimated position have smaller key than read, increase iter_pos and compare
            iter_pos =lower_b+1;
            if (right_forward){
                while (compare_read_and_ref_binary(pac, sa_pos, 5*iter_pos, raux, sa_num,read_valid_len, &match_len,&exact_match_flag)&& !exact_match_flag ){
                    if (iter_pos == sa_num - 1){
                        break;
                    }
                    iter_pos ++;
                    last_match_len = match_len;
                }
            }else{
                while (compare_read_and_ref_binary_left(pac, sa_pos, 5*iter_pos, raux, sa_num,read_valid_len, &match_len,&exact_match_flag)&& !exact_match_flag ){
                    if (iter_pos == sa_num - 1){
                        break;
                    }
                    iter_pos ++;
                    last_match_len = match_len;
                }
            }
            //  stops when ref is bigger than read or exact matches with read,
            if (last_match_len > match_len){
                iter_pos--;
                match_len = last_match_len;
            }
        }
    }

    *exact_match_len = match_len;

    return iter_pos;
                        
}

uint64_t STAR_lookup(const uint8_t* ref_string,const uint8_t* sa_pos,const uint8_t* pac, uint64_t sa_num,
                        Learned_read_aux_t* raux, bool right_forward, uint64_t key, uint32_t* mmp_length, uint64_t L_MER_SIZE, uint32_t* ambiguous_pos ){

    //returns position of MMP and length
    
    uint64_t Nrep=0, indStartEnd[2], maxL;
    uint64_t iSA1, iSA2;
    uint64_t mmp_position=0, mmp_len=0;

    uint64_t read_valid_len;
    if (right_forward){
		read_valid_len = *ambiguous_pos - raux->pivot;
    }
    else{
        read_valid_len = *ambiguous_pos - raux->l_pivot;
    }

    //calculate full index
    uint32_t Lmax=(uint32_t)std::min(L_MER_SIZE, read_valid_len);
    uint64_t ind1=key >> (64 - 2* Lmax);
    uint64_t SAiMarkAbsentMaskC = (1LLU<<63);
    uint64_t SAiMarkAbsentMask = 0x7fffffffffffffff ;

    // assert(ind1 < (1 << (L_MER_SIZE*2) ));

    //find SAi boundaries
    uint32_t Lind=Lmax;
    
    while (Lind>0) {//check the presence of the prefix for Lind

        // printf("[SAindexstart[Lind-1]: %llu\n", SAindexstart[Lind-1]);
        iSA1=SAi[SAindexstart[Lind-1]+ind1]; // starting point for suffix array search.
        
        if ((iSA1 & SAiMarkAbsentMaskC) == 0) {//prefix exists
            break;
        } else {//this prefix does not exist, reduce Lind
            --Lind;
            ind1 = ind1 >> 2;
            // assert(Lind != 0);
        };
    };

    // printf("ind1: %lu  Lind %llu read_valid_len:%llu\n", ind1, Lind, read_valid_len);
    // printf("SAi value : %llu %llu \n", SAi[SAindexstart[Lind-1]+ind1] & SAiMarkAbsentMaskC ,SAi[SAindexstart[Lind-1]+ind1+1] & SAiMarkAbsentMaskC);

    // printf("[After]ind1: %lu  Lind %llu read_valid_len:%llu\n", ind1, Lind, read_valid_len);
    // define upper bound for suffix array range search.
    if (SAindexstart[Lind-1]+ind1+1 < SAindexstart[Lind]) {//we are not at the end of the SA
        iSA2=((SAi[SAindexstart[Lind-1]+ind1+1] ) & SAiMarkAbsentMask) - 1;
    } else {
        iSA2=sa_num-1;
    };

    if (Lind < L_MER_SIZE ) {//no need for SA search
        // printf("NoSearch up:%llu down:%llu valid len:%lu\n", iSA1, iSA2, read_valid_len);
        // very short seq, already found hits in suffix array w/o having to search the genome for extensions.
        // indStartEnd[0]=iSA1;
        // indStartEnd[1]=iSA2;
        *mmp_length=Lind;
        return iSA1;
    } else if (iSA1==iSA2) {//unique align already, just find maxL
        // printf("Unique up:%llu down:%llu valid len:%lu sa_num: %llu\n", iSA1, iSA2, read_valid_len, sa_num);

        // if ((iSA1 & mapGen.SAiMarkNmaskC)!=0) {
        //     ostringstream errOut;
        //     errOut  << "BUG: in ReadAlign::maxMappableLength2strands";
        //     exitWithError(errOut.str(), std::cerr, P.inOut->logMain, EXIT_CODE_BUG, P);
        // };
        // indStartEnd[0]=indStartEnd[1]=iSA1;
        bool exact_match_flag=false;
        if (right_forward){

            // uint64_t iter_pos = get_sa_pos(sa_pos, iSA1);
            // printf("Reference\n");
            // for (uint32_t rr=iter_pos; rr< iter_pos + raux->l_seq - raux->pivot;rr++){
            //     printf("%d",ref_string[rr]);
            // }
            // printf("\n");
            // printf("Read\n");
            // for (uint32_t rr=0; rr< raux->l_seq;rr++){
            //     if (rr == raux->pivot){
            //        printf("["); 
            //     }
            //     printf("%d",raux->unpacked_queue_buf[rr]);
            // }
            // printf("\n");
            compare_read_and_ref_binary(pac, sa_pos, 5*iSA1, raux, sa_num, read_valid_len, mmp_length,&exact_match_flag);
            // uint32_t tmp = *mmp_length;
            // exact_match_flag=false;
            // compare_read_and_ref_binary(pac, sa_pos, 5*(iSA1+1), raux, sa_num, read_valid_len, mmp_length,&exact_match_flag);
            // *mmp_length = std::max(tmp, *mmp_length);
        }
        else{
            compare_read_and_ref_binary_left(pac, sa_pos, 5*iSA1, raux, sa_num, read_valid_len, mmp_length,&exact_match_flag);
            // uint32_t tmp = *mmp_length;
            // exact_match_flag=false;
            // compare_read_and_ref_binary_left(pac, sa_pos, 5*(iSA1+1), raux, sa_num, read_valid_len, mmp_length,&exact_match_flag);
            // *mmp_length = std::max(tmp, *mmp_length);
        }
        return iSA1;

    } else {//SA search, pieceLength>maxL
        // printf("Binary search low:%llu up:%llu\n", iSA1, iSA2);
        // Nrep = maxMappableLength(mapGen, Read1, pieceStart, pieceLength, iSA1 & mapGen.SAiMarkNmask, iSA2, dirR, maxL, indStartEnd);
        return maxMappableLength(ref_string, sa_pos, pac, sa_num, raux, read_valid_len, iSA1, iSA2+1,right_forward, mmp_length );
    };
}


void STAR_getSMEMsOnePosOneThread_step1(Learned_index_aux_t* iaux, Learned_read_aux_t* raux, mem_tlv* smems, u64v* hits, bool hasN, uint64_t L_MER_SIZE , bool use_cached=false) {
	/*
		Perform extension in zigzag style
	*/
	uint64_t key;
	uint64_t mmp_position ;
	uint32_t mmp_length ;
	uint32_t next_pivot ;
	int64_t suffix_array_num = iaux->bns->l_pac*2;
	uint32_t ambiguous_pos;
	// Right extension from pivot, save next raux->pivot
	bool right_forward= true;
	// Check pivot point whether ambiguous base appears
	if (raux->unpacked_queue_buf[raux->pivot] >= 4){
		if (raux->l_seq - raux->pivot < raux->min_seed_len ){
			set_forward_pivot(raux, raux->l_seq );
		}
		else{
			set_forward_pivot(raux, raux->pivot+1 );
		}
		return;
	}
	if (raux->pivot !=0 && raux->unpacked_queue_buf[raux->pivot-1] < 4){
		// - 2. Infer Learned index and get prediction
		next_pivot = raux->l_seq;
		int search_pivot = raux->pivot;
		while (search_pivot < next_pivot){
			if (raux->unpacked_queue_buf[search_pivot] >= 4){
				if (raux->l_seq - search_pivot < raux->min_seed_len ){
					set_forward_pivot(raux, raux->l_seq );
					search_pivot = raux->l_seq;
					// set_forward_pivot(raux, raux->pivot+1 );
				}
				else{
					search_pivot += 1; 
					set_forward_pivot(raux, raux->pivot+1 );
				}
				continue;
			}
			right_forward = false;
			key = Tokenization(raux, right_forward, &ambiguous_pos, hasN);

            
            mmp_position = STAR_lookup(iaux->ref_string, iaux->sa_pos, iaux->pac, suffix_array_num, 
							raux, right_forward, key , &mmp_length, L_MER_SIZE, &ambiguous_pos);
            // printf("[LEFT]Pivot: %llu MMP_Length: %llu MMP_Pos:%llu\n",raux->pivot, mmp_length, mmp_position);

			set_forward_pivot(raux,  raux->pivot - mmp_length+1 );
			if(next_pivot - raux->pivot < raux->min_seed_len ){
				break;
			}
			right_forward = true;
			key = Tokenization(raux, right_forward, &ambiguous_pos, hasN);


            mmp_position = STAR_lookup(iaux->ref_string, iaux->sa_pos, iaux->pac, suffix_array_num, 
							raux, right_forward, key , &mmp_length, L_MER_SIZE, &ambiguous_pos);

            // printf("[RIGHT]Pivot: %llu MMP_Length: %llu MMP_Pos:%llu\n",raux->pivot, mmp_length, mmp_position);

            assert( (raux->pivot+mmp_length) > search_pivot);

			search_pivot = raux->pivot + mmp_length ;
			
			set_forward_pivot(raux,   search_pivot );
		}
	}
	else{
		key = Tokenization(raux, right_forward, &ambiguous_pos, hasN);
		// - 2. Infer Learned index and get prediction		
        
        mmp_position = STAR_lookup(iaux->ref_string, iaux->sa_pos, iaux->pac, suffix_array_num, 
							raux, right_forward, key , &mmp_length, L_MER_SIZE, &ambiguous_pos);

        // printf("[INIT-RIGHT]Pivot: %llu MMP_Length: %llu MMP_Pos:%llu\n",raux->pivot, mmp_length, mmp_position);
        
		next_pivot = raux->pivot + mmp_length;
	}
	set_forward_pivot(raux,  next_pivot );
}



int64_t run_Star(uint8_t* sa_position, uint8_t* ref_string , FMI_search* fmiSearch, bseq1_t *seqs, int batch_size, 
                int64_t* numTotalSmem, int64_t*workTicks, int64_t num_batches,
                int32_t numReads,int64_t numthreads, int64_t total_size, int steps, int rid_start, uint64_t L_MER_SIZE ){
    int64_t i, r;
    int max_readlength = seqs[0].l_seq;
    int min_readlength = seqs[0].l_seq;

    uint8_t *enc_qdb=(uint8_t *)malloc(numReads * max_readlength * sizeof(uint8_t));
    assert(enc_qdb !=NULL);
    int64_t cind,st;
    for (st=0; st < numReads; st++) {
        cind=st*max_readlength;
        for(r = 0; r < max_readlength; ++r) {
            switch(seqs[st].seq[r])
            {
                case 'A': enc_qdb[r+cind]=0;
                          break;
                case 'C': enc_qdb[r+cind]=1;
                          break;
                case 'G': enc_qdb[r+cind]=2;
                          break;
                case 'T': enc_qdb[r+cind]=3;
                          break;
                default: enc_qdb[r+cind]=4;
            }
        }
    }

    int32_t minSeedLen = 19;
    int split_len = (int)(minSeedLen * 1.5 + .499);

    int64_t startTick, endTick;

    startTick = __rdtsc();
    memset(numTotalSmem, 0, num_batches * sizeof(int64_t));
    
    
#ifdef VTUNE_ANALYSIS
    __itt_resume();
#endif
#pragma omp parallel num_threads(numthreads)
    {
        int tid = omp_get_thread_num();
        if(tid == 0)
            fprintf(stderr,"Running %d threads\n", omp_get_num_threads());

        u64v* hits = (u64v*) malloc( 100*MAX_LINE_LEN * sizeof(u64v));
        mem_tlv* smems = (mem_tlv*) malloc(  100*MAX_LINE_LEN * sizeof(mem_tlv));
        assert(smems != NULL);
        kv_init_base(mem_tl, smems[0], 100*MAX_LINE_LEN);
        kv_init_base(uint64_t, hits[0], 100*MAX_LINE_LEN);
        int64_t myTotalSmems = 0;
        int64_t startTick = __rdtsc();
        uint8_t unpacked_rc_queue_buf[LEARNED_MAX_READ_LEN];

        uint8_t unpacked_queue_binary_buf_shift1[LEARNED_MAX_READ_LEN/4+1];
        uint8_t unpacked_queue_binary_buf_shift2[LEARNED_MAX_READ_LEN/4+1];
        uint8_t unpacked_queue_binary_buf_shift3[LEARNED_MAX_READ_LEN/4+1];
        uint8_t unpacked_queue_binary_buf_shift4[LEARNED_MAX_READ_LEN/4+1];

        uint8_t unpacked_rc_queue_binary_buf_shift1[LEARNED_MAX_READ_LEN/4+1];
        uint8_t unpacked_rc_queue_binary_buf_shift2[LEARNED_MAX_READ_LEN/4+1];
        uint8_t unpacked_rc_queue_binary_buf_shift3[LEARNED_MAX_READ_LEN/4+1];
        uint8_t unpacked_rc_queue_binary_buf_shift4[LEARNED_MAX_READ_LEN/4+1];

#pragma omp for schedule(dynamic)
        
        for(i = 0; i < numReads; i += batch_size)
        {
            int j,k,l;
            int64_t st1 = __rdtsc();
            int32_t batch_count = batch_size;
            if((i + batch_count) > numReads) batch_count = numReads - i;
            
            int32_t batch_id = i/batch_size;
            //printf("%d] i = %d, batch_count = %d, batch_size = %d\n", tid, i, batch_count, batch_size);
            //fflush(stdout);

            for (j=0; j<batch_count; j++){
                smems->n = 0;
                hits->n = 0;
                //iterate batch and inference learned_index to find SMEM
                // printf("%d] seq=%d, i = %d, batch_count = %d, batch_size = %d\n", tid, j*max_readlength+i*max_readlength,i, batch_count, batch_size);
                uint8_t *seq = &enc_qdb[j*max_readlength+i*max_readlength];
                int len = seqs[j+i].l_seq;
                bool hasN = false;
                if (strchr(seqs[j+i].seq, 'N') || strchr(seqs[j+i].seq, 'n')) {
                    hasN = true;
                }
               


                Learned_read_aux_t raux;

                assert(len <= LEARNED_MAX_READ_LEN);
#if 0
                for (k = 0; k < len; ++k) {
                    seq[k] = seq[k] < 4? seq[k] : nst_nt4_table[(int)seq[k]]; //nst_nt4??
                    unpacked_rc_queue_buf[len - k - 1] = seq[k] < 4 ? 3 - seq[k] : 4; 
                }
#else
                //8-bit representation to 2-bit representation
                uint8_t set_bit=0;
                uint8_t set_rc_bit=0;
                for (k=0; k < len; ++k) {
                    unpacked_rc_queue_buf[len - k - 1] = seq[k] < 4 ? 3 - seq[k] : 4; 

                    set_bit = set_bit<<2;
                    set_rc_bit = set_rc_bit <<2;

                    set_bit |= seq[k] < 4? seq[k] : 0;
                    set_rc_bit |=  seq[len-1 -k] < 4 ? 3 - seq[len-1-k] : 0; 
                    if ((k&3) == 0){
                        unpacked_queue_binary_buf_shift1[k>>2] = BitReverseTable256[set_bit];
                        unpacked_rc_queue_binary_buf_shift1[k>>2] = BitReverseTable256[set_rc_bit];
                    }
                    else if((k&3) == 1){
                        unpacked_queue_binary_buf_shift2[k>>2] = BitReverseTable256[set_bit];
                        unpacked_rc_queue_binary_buf_shift2[k>>2] = BitReverseTable256[set_rc_bit];
                    }
                    else if((k&3) == 2){
                        unpacked_queue_binary_buf_shift3[k>>2] = BitReverseTable256[set_bit];
                        unpacked_rc_queue_binary_buf_shift3[k>>2] = BitReverseTable256[set_rc_bit];
                    }
                    else if((k&3) == 3){
                        unpacked_queue_binary_buf_shift4[k>>2] = BitReverseTable256[set_bit];
                        unpacked_rc_queue_binary_buf_shift4[k>>2] = BitReverseTable256[set_rc_bit];
                    }
                }
                for (;k < len+4;++k){
                    set_bit = set_bit<<2;
                    set_rc_bit = set_rc_bit <<2;
                    if ((k&3) == 0){
                        unpacked_queue_binary_buf_shift1[k>>2] = BitReverseTable256[set_bit];
                        unpacked_rc_queue_binary_buf_shift1[k>>2] = BitReverseTable256[set_rc_bit];
                    }
                    else if((k&3) == 1){
                        unpacked_queue_binary_buf_shift2[k>>2] = BitReverseTable256[set_bit];
                        unpacked_rc_queue_binary_buf_shift2[k>>2] = BitReverseTable256[set_rc_bit];
                    }
                    else if((k&3) == 2){
                        unpacked_queue_binary_buf_shift3[k>>2] = BitReverseTable256[set_bit];
                        unpacked_rc_queue_binary_buf_shift3[k>>2] = BitReverseTable256[set_rc_bit];
                    }
                    else if((k&3) == 3){
                        unpacked_queue_binary_buf_shift4[k>>2] = BitReverseTable256[set_bit];
                        unpacked_rc_queue_binary_buf_shift4[k>>2] = BitReverseTable256[set_rc_bit];
                    }
                    raux.unpacked_queue_binary_buf_shift1 = unpacked_queue_binary_buf_shift1;
                    raux.unpacked_queue_binary_buf_shift2 = unpacked_queue_binary_buf_shift2;
                    raux.unpacked_queue_binary_buf_shift3 = unpacked_queue_binary_buf_shift3;
                    raux.unpacked_queue_binary_buf_shift4 = unpacked_queue_binary_buf_shift4;

                    raux.unpacked_rc_queue_binary_buf_shift1 = unpacked_rc_queue_binary_buf_shift1;
                    raux.unpacked_rc_queue_binary_buf_shift2 = unpacked_rc_queue_binary_buf_shift2;
                    raux.unpacked_rc_queue_binary_buf_shift3 = unpacked_rc_queue_binary_buf_shift3;
                    raux.unpacked_rc_queue_binary_buf_shift4 = unpacked_rc_queue_binary_buf_shift4;

                }
#endif
                Learned_index_aux_t iaux;
                iaux.sa_pos = sa_position;

                iaux.bns = fmiSearch->idx->bns;
                iaux.pac = fmiSearch->idx->pac;
                iaux.ref_string = ref_string;
                
                raux.min_seed_len = minSeedLen;
                raux.l_seq = len;
                raux.max_l_seq = 0;
                raux.read_name = seqs[j+i].name;
                raux.unpacked_queue_buf = (uint8_t*) seq;
                raux.unpacked_rc_queue_buf = unpacked_rc_queue_buf;
                raux.min_intv_limit = 1;

                
                set_forward_pivot(&raux, 0);
                // loop until raux->pivot reaches end of read
                while (raux.pivot < raux.l_seq){
                    // seeding step 1
                    STAR_getSMEMsOnePosOneThread_step1(&iaux, &raux, smems, hits, hasN,L_MER_SIZE);
                    printf("pivot: %llu lseq: %llu\n", raux.pivot, raux.l_seq);
                }

                
                numTotalSmem[batch_id] += smems->n;
            }
            
            int64_t et1 = __rdtsc();
            workTicks[tid] += (et1 - st1);
        }

        int64_t endTick = __rdtsc();
        // fprintf(stderr,"%d] %lld ticks, workTicks = %lld\n", tid, endTick - startTick, workTicks[tid]);
        kv_destroy(hits[0]);
        kv_destroy(smems[0]);
        free(hits);
        free(smems);
    }
#ifdef VTUNE_ANALYSIS
    __itt_pause();
#endif 
    endTick = __rdtsc();
    if (enc_qdb)free(enc_qdb);
    
    return endTick - startTick;
}

int64_t pac_seq_len_(const char *fn_pac)
{
	FILE *fp;
	int64_t pac_len;
	uint8_t c;
	fp = xopen(fn_pac, "rb");
	err_fseek(fp, -1, SEEK_END);
	pac_len = err_ftell(fp);
	err_fread_noeof(&c, 1, 1, fp);
	err_fclose(fp);
	assert(c >= 0 && c <= 255);
	return (pac_len - 1) * 4 + (int)c;
}


void pac2nt_(const char *fn_pac, std::string &reference_seq)
{
	uint8_t *buf2;
	int64_t i, pac_size, seq_len;
	FILE *fp;

	// initialization
	seq_len = pac_seq_len_(fn_pac);
	assert(seq_len > 0);
	assert(seq_len <= 0x7fffffffffffffffL);
	fp = xopen(fn_pac, "rb");

	// prepare sequence
	pac_size = (seq_len>>2) + ((seq_len&3) == 0? 0 : 1);
	buf2 = (uint8_t*)calloc(pac_size, 1);
	assert(buf2 != NULL);
	err_fread_noeof(buf2, 1, pac_size, fp);
	err_fclose(fp);
	for (i = 0; i < seq_len; ++i) {
		int nt = buf2[i>>2] >> ((3 - (i&3)) << 1) & 3;
        switch(nt)
        {
            case 0:
                reference_seq += "A";
            break;
            case 1:
                reference_seq += "C";
            break;
            case 2:
                reference_seq += "G";
            break;
            case 3:
                reference_seq += "T";
            break;
            default:
                fprintf(stderr, "ERROR! Value of nt is not in 0,1,2,3!");
                exit(EXIT_FAILURE);
        }
	}
    for(i = seq_len - 1; i >= 0; i--)
    {
        char c = reference_seq[i];
        switch(c)
        {
            case 'A':
                reference_seq += "T";
            break;
            case 'C':
                reference_seq += "G";
            break;
            case 'G':
                reference_seq += "C";
            break;
            case 'T':
                reference_seq += "A";
            break;
        }
    }
	free(buf2);
}


/*
    Finds next index and lmer value in suffix array
    @return pac_len when there is no index found
*/
void findnextindex(char * binary_ref_seq, uint64_t suffixarray_num, int64_t pac_len, char* suffix_array, uint64_t* current_lmer, uint64_t* cur_index, int L_MER_SIZE){
    uint64_t prev_lmer = *current_lmer;
    uint64_t prev_idx = *cur_index;

    // Exponential search and binary search to find different lmer value
    uint64_t up=0, low=0;

    while ( *cur_index < suffixarray_num && prev_lmer == *current_lmer)
    {
        *cur_index = (*cur_index) + 1;
        *current_lmer = tokenize2lmer(binary_ref_seq, pac_len, get_sa_pos(suffix_array, *cur_index), L_MER_SIZE);
        // printf("%d %lld\n",*cur_index, *current_lmer);
        // assert(*current_lmer >= prev_lmer);

    }
    return;
}

void load_star_index(uint64_t L_MER_SIZE, char** argv){
    {
        SAindexstart = malloc((L_MER_SIZE+1)*sizeof(uint64_t));
        SAindexstart[0]=0;
        for (uint64_t ii=1;ii<=L_MER_SIZE;ii++) {//L-mer indices starts
            SAindexstart[ii] = SAindexstart[ii-1] + ( 1LLU<<(2*ii) );
            // printf("ii:%llu ", SAindexstart[ii]);
        }; 
        // printf("\n");
        uint64_t nSAi = SAindexstart[L_MER_SIZE];
        
        SAi = malloc(nSAi* sizeof(uint64_t));

        char lmer_file_name[PATH_MAX];
        strcpy_s(lmer_file_name, PATH_MAX, argv[1]);
        strcat_s(lmer_file_name, PATH_MAX, ".lmer_star");
        FILE *lmers_fd;
        lmers_fd = fopen(lmer_file_name, "rb");
        if (lmers_fd == NULL) {
            fprintf(stderr, "[Load STAR index] Can't read file lmer table %s\n.", lmer_file_name);
            exit(1);
        }
        err_fread_noeof(SAi, sizeof(uint64_t), nSAi, lmers_fd);
        fclose(lmers_fd);
    }
}

void build_star_index(uint64_t L_MER_SIZE, char** argv){
    uint64_t startTick;
    int status;
    int index_alloc  = 0;
    
    printf("[Build-STARmode] pac2nt function...\n");
    std::string reference_seq;
    char pac_file_name[PATH_MAX];
    strcpy_s(pac_file_name, PATH_MAX, argv[2]);
    strcat_s(pac_file_name, PATH_MAX, ".pac"); 
    pac2nt_(pac_file_name, reference_seq);

    int64_t i;
    int64_t pac_len = reference_seq.length();
    int64_t max_a_len = 0, max_t_len = 0, tmp_a_len = 0, tmp_t_len = 0;

    for(i = 0; i < pac_len; i++)
    {
        switch(reference_seq[i])
        {
            case 'A':
                tmp_a_len += 1;
                tmp_t_len = 0;
                break;
            case 'C':
                tmp_a_len = 0;
                tmp_t_len = 0;
                break;
            case 'G':
                tmp_a_len = 0;
                tmp_t_len = 0;
                break;
            case 'T':
                tmp_a_len = 0;
                tmp_t_len += 1;
                break;
            default:
                assert(0);
        }
        if (tmp_a_len > max_a_len){
            max_a_len = tmp_a_len;
        }
        if (tmp_t_len > max_t_len){
            max_t_len = tmp_t_len;
        }
    }

    int64_t padded_t_len = std::max(max_a_len+1,max_t_len+1);
    
    int64_t size = (reference_seq.length() + padded_t_len) * sizeof(char);
    char *binary_ref_seq = (char *)_mm_malloc(size , 64);
    index_alloc += size;
    assert_not_null(binary_ref_seq, size, index_alloc);

    

    for(i = 0; i < pac_len; i++)
    {
        switch(reference_seq[i])
        {
            case 'A':
                binary_ref_seq[i] = 0;
                break;
            case 'C':
                binary_ref_seq[i] = 1;
                break;
            case 'G':
                binary_ref_seq[i] = 2;
                break;
            case 'T':
                binary_ref_seq[i] = 3;
                break;
            default:
                assert(0);
                binary_ref_seq[i] = 4;
        }
    }
    
    printf("[Build-STARmode] ref seq len = %ld\n", pac_len);
    assert(pac_len%2 ==0 );
    pac_len = pac_len + padded_t_len;
    for (i;i<pac_len;i++){
        binary_ref_seq[i] = 3;  
        reference_seq += "T";
    }
    printf("[Build-STARmode] padded ref len = %ld\n", pac_len);
    //build suffix array
    char sa_pos_file_name[PATH_MAX];
    strcpy_s(sa_pos_file_name, PATH_MAX, argv[2]);
    strcat_s(sa_pos_file_name, PATH_MAX, ".pos_packed");
    FILE *sa_pos_fd;
    uint64_t suffixarray_num;

    sa_pos_fd = fopen(sa_pos_file_name, "rb");
    if (sa_pos_fd == NULL) {
        fprintf(stderr, "[calculate_index] Can't open suffix array position index\n.", __func__);
        exit(1);
    }
    
    fseek(sa_pos_fd, 0, SEEK_END); 
    suffixarray_num = ftell(sa_pos_fd) / 5;
    rewind(sa_pos_fd);
    
    #if 0
    char sa_file_name[PATH_MAX];
    strcpy_s(sa_file_name, PATH_MAX, argv[2]);
    strcat_s(sa_file_name, PATH_MAX, ".suffixarray_uint64");
    // youngmok: Read size from suffix array file, first 64bit is number of suffix array
    std::ifstream in(sa_file_name, std::ios::binary);
    if (!in.is_open()) {
        fprintf(stderr, "[M::%s::LEARNED] Can't open suffix array file\n.", __func__);
        exit(EXIT_FAILURE);
    }
    
    in.read(reinterpret_cast<char*>(&suffixarray_num), sizeof(uint64_t));
    in.close();
    #endif
    
    char* suffix_array = (char*) malloc(suffixarray_num* 5);
    index_alloc += suffixarray_num* 5;
    assert_not_null(suffix_array, suffixarray_num* 5, index_alloc);
    startTick = __rdtsc();

    printf("[Build-STARmode] Read Suffix array read:%d real:%ld\n", suffixarray_num, pac_len);
    // status = saisxx(reference_seq.c_str(), suffix_array, pac_len);
    
    err_fread_noeof(suffix_array, sizeof(uint8_t), 5*suffixarray_num, sa_pos_fd);
    fclose(sa_pos_fd);
    /*
    L-MER:        1  2  3  4  5  6  7  8  9  10 11 12 13 14 15
    2^2L in rows [ ][ ][ ][ ][ ][ ][ ][ ][ ][ ][ ][ ][ ][ ][ ]
                 [ ][ ]       ...   
                 [ ][ ]
              4  [ ][ ]
                    [ ] .
                    [ ] .
                    [ ] .
                    [ ]
                    [ ]
                    [ ]
                    [ ]
                    [ ]
                    [ ]
                    [ ]
                    [ ]
              16    [ ]

    Each cell contains the starting point of LMER, and a bit-flag that indicates existance of L-MER
    */
    

    printf("[Build-STARmode] Populate Lmer-table start indices\n");
    // adopted from STAR/genomeSAindex.cpp
    uint64_t* genomeSAindexStart = malloc((L_MER_SIZE+1)*sizeof(uint64_t));
    genomeSAindexStart[0]=0;
    for (uint64_t ii=1;ii<=L_MER_SIZE;ii++) {//L-mer indices starts
        genomeSAindexStart[ii] = genomeSAindexStart[ii-1] + ( 1LLU<<(2*ii) );
        printf("ii:%llu ", genomeSAindexStart[ii]);
    }; 
    printf("\n");
    uint64_t nSAi = genomeSAindexStart[L_MER_SIZE];
    uint64_t SAiMarkAbsentMaskC = 1LLU<<63;

    

    uint64_t SAiMarkAbsentUnmask = 0x7fffffffffffffff;
    // List of LMER tables 
    uint64_t *lmer_table = (uint64_t *) malloc( nSAi * sizeof(uint64_t));
    if (lmer_table == NULL){
        fprintf(stderr, "[Build STAR index] Can't allocate memory for lmer table list %ld\n.", __func__, nSAi);
        exit(1);
    }
    printf("[Build-STARmode] Populate %d size L-MER table by iterating all suffixes\n", nSAi);
    // populate LMER table
    uint64_t* ind0= (uint64_t *) malloc(L_MER_SIZE*sizeof(uint64_t));

    for (uint64_t ii=0; ii<L_MER_SIZE; ii++) {
        ind0[ii]=-1;//this is needed in case "AAA...AAA",i.e. indPref=0 is not present in the genome for some lengths
    };


    uint64_t isaStep= suffixarray_num/(1llu<<(2*L_MER_SIZE))+1;
    uint64_t isa=0;
    uint64_t indFull = tokenize2lmer(binary_ref_seq, pac_len, get_sa_pos(suffix_array, isa), L_MER_SIZE);
    while (isa<=suffixarray_num-1) {//for all suffixes
        for (uint64_t iL=0; iL < L_MER_SIZE; iL++) {//calculate index
            uint64_t indPref = indFull >> (2*(L_MER_SIZE-1-iL));
            if ( indPref > ind0[iL] || isa==0 ) {//new && good index, record it
                // SAi.writePacked(genomeSAindexStart[iL]+indPref, isa);
                assert( 0 <= genomeSAindexStart[iL]+indPref);
                assert( genomeSAindexStart[L_MER_SIZE] > genomeSAindexStart[iL]+indPref);

                lmer_table[genomeSAindexStart[iL]+indPref] = isa;
                assert( (isa & SAiMarkAbsentMaskC) == 0);
                for (uint64_t ii=ind0[iL]+1; ii<indPref; ii++) {//index is not present, record to the last present suffix
                    // SAi.writePacked(genomeSAindexStart[iL]+ii, isa | mapGen.SAiMarkAbsentMaskC);
                    assert( 0 <= genomeSAindexStart[iL]+ii);
                    assert( genomeSAindexStart[L_MER_SIZE] > genomeSAindexStart[iL]+ii);
                    lmer_table[genomeSAindexStart[iL]+ii] = (isa|SAiMarkAbsentMaskC);
                };
                ind0[iL]=indPref;
                // printf("L:%d SAi:%llu indfull: %llu\n", iL, isa, indPref);
            } else if ( indPref < ind0[iL] ) {
                assert(0);
                // errOut << "BUG: next index is smaller than previous, EXITING\n" <<flush;
                // exitWithError(errOut.str(),std::cerr, P.inOut->logMain, EXIT_CODE_INPUT_FILES, P);
            };
        };
        //find next index not equal to the current one
        // funSAiFindNextIndex(G, SA, isaStep, isa, indFull, iL4, mapGen);//indFull and iL4 have been already defined at the previous step
        findnextindex(binary_ref_seq, suffixarray_num, pac_len, suffix_array, &indFull, &isa, L_MER_SIZE);

    };//isa cycle
    
    printf("[Build-STARmode] Populate empty L-MER table in the last L-MERS \n");
    for (uint64_t iL=0; iL < L_MER_SIZE; iL++) {//fill up unfilled indexes
    	for (uint64_t ii=genomeSAindexStart[iL]+ind0[iL]+1; ii<genomeSAindexStart[iL+1]; ii++) {
    		// SAi.writePacked(ii, suffixarray_num | mapGen.SAiMarkAbsentMaskC);
            assert( 0 <= ii);
            assert( genomeSAindexStart[L_MER_SIZE] > ii);
            lmer_table[ii] = suffixarray_num | SAiMarkAbsentMaskC;
    	};
    };


    // validate talbe
    // for (uint64_t iL=0; iL < L_MER_SIZE; iL++) {//fill up unfilled indexes
    // 	for (uint64_t ii=genomeSAindexStart[iL]; ii<genomeSAindexStart[iL+1]; ii++) {
    // 		// SAi.writePacked(ii, suffixarray_num | mapGen.SAiMarkAbsentMaskC);
    //         assert( 0 <= ii);
    //         assert( genomeSAindexStart[L_MER_SIZE] > ii);
    //         if (ii >genomeSAindexStart[iL]){
    //             assert( (lmer_table[ii]&SAiMarkAbsentUnmask ) >= (lmer_table[ii-1]&SAiMarkAbsentUnmask) );
    //             if ( (lmer_table[ii] &SAiMarkAbsentUnmask) == (lmer_table[ii-1]&SAiMarkAbsentUnmask ) ){
                    
    //                 if ( (lmer_table[ii]&SAiMarkAbsentMaskC ) == 0 && (lmer_table[ii-1]&SAiMarkAbsentMaskC ==0) ){
    //                     printf("\nii: %llu ii val: %lu ii-1 val:%llu ii-2 val:%llu start:%llu end:%llu\n", ii, (lmer_table[ii] &SAiMarkAbsentUnmask), (lmer_table[ii-1] &SAiMarkAbsentUnmask), (lmer_table[ii-2] &SAiMarkAbsentUnmask), genomeSAindexStart[iL], genomeSAindexStart[iL+1]);
                            
    //                 }
                    
    //                 assert( (lmer_table[ii]&SAiMarkAbsentMaskC ) | (lmer_table[ii-1]&SAiMarkAbsentMaskC)  );
    //             }
    //         }
            
    // 	};
    // };

    printf("[Build-STARmode] Saving LMER table as file \n");

    {
        char lmer_file_name[PATH_MAX];
        strcpy_s(lmer_file_name, PATH_MAX, argv[2]);
        strcat_s(lmer_file_name, PATH_MAX, ".lmer_star");
        FILE *lmers_fd;
        lmers_fd = fopen(lmer_file_name, "wb");
        if (lmers_fd == NULL) {
            fprintf(stderr, "[Build STAR index] Can't write file for lmer table \n.", __func__);
            exit(1);
        }
        err_fwrite(lmer_table, sizeof(uint64_t), nSAi, lmers_fd);
        fclose(lmers_fd);
    }

    printf("[Build-STARmode] Free memories\n");
    free(ind0); //free
    free(suffix_array);
    free(genomeSAindexStart);
    free(lmer_table);
    _mm_free(binary_ref_seq);
}



int main(int argc, char **argv) {
#ifdef VTUNE_ANALYSIS
    __itt_pause();
#endif

    uint64_t L_MER_SIZE = 15;
    if(strcmp(argv[1], "index") == 0 )
    {
        printf("Build index for Star aligner style Suffix array intervals\n");
        printf("./star_aligner_seeding index <REFERENCE FILE ex) ref.fa> <L-MER>\n");
        build_star_index(L_MER_SIZE, argv);
        printf("[DONE] Build index for Star aligner style Suffix array intervals\n");
        return 0;
    }
    // else if( strcmp(argv[1], "mmp") == 0 ){
    //     star_mmp_lkup(L_MER_SIZE, argv);
    // }


     if(argc<6)
    {
        printf("Need six arguments : ref_file query_set batch_size  n_threads steps\n");
        return 1;
    }
    int steps=0, i;
    steps=atoi(argv[5]);

    int32_t numReads = 0;
    int64_t total_size = 0;
    gzFile fp = gzopen(argv[2], "r");
	if (fp == 0)
	{
		fprintf(stderr, "[E::%s] fail to open file `%s'.\n", __func__, argv[2]);
        exit(EXIT_FAILURE);
	}
    FMI_search *fmiSearch = new FMI_search(argv[1]);
    fmiSearch->load_index_other_elements(BWA_IDX_BNS | BWA_IDX_PAC);
    
    /* add reverse-complement binary reference in pac */
    // fmiSearch->idx->pac = (uint8_t*) realloc(fmiSearch->idx->pac, fmiSearch->idx->bns->l_pac/2+1);
    int64_t ll_pac = (fmiSearch->idx->bns->l_pac * 2 + 3) / 4 * 4;
    if (ll_pac > 0x10000) fmiSearch->idx->pac = realloc(fmiSearch->idx->pac, ll_pac/4);
    memset_s(fmiSearch->idx->pac + (fmiSearch->idx->bns->l_pac+3)/4, (ll_pac - (fmiSearch->idx->bns->l_pac+3)/4*4) / 4, 0);
    // for (int64_t l = fmiSearch->idx->bns->l_pac - 1; l >= 0; --l, ++fmiSearch->idx->bns->l_pac){
    for (int64_t l = fmiSearch->idx->bns->l_pac - 1; l >= 0; --l){
			_set_pac(fmiSearch->idx->pac, (fmiSearch->idx->bns->l_pac <<1) - l-1, 3-_get_pac(fmiSearch->idx->pac, l));
    }//add reverse complement
    for (int64_t l=0; l<ll_pac/4; l++){
        // swap 8 bit to match endianess
        fmiSearch->idx->pac[l] = BitReverseTable256[fmiSearch->idx->pac[l]];
    }
    char sa_file_name[PATH_MAX];
    strcpy_s(sa_file_name, PATH_MAX, argv[1]); 
    strcat_s(sa_file_name, PATH_MAX, ".suffixarray_uint64");
    
    std::ifstream in(sa_file_name, std::ios::binary);
    if (!in.is_open()) {
        fprintf(stderr, "[M::%s::LEARNED] Can't open suffix array file\n.", __func__);
        exit(EXIT_FAILURE);
    }
    uint64_t suffixarray_num;
    in.read(reinterpret_cast<char*>(&suffixarray_num), sizeof(uint64_t));
    in.close();
    
    char binary_seq_file[PATH_MAX];
    strcpy_s(binary_seq_file, PATH_MAX, argv[1]);
    strcat_s(binary_seq_file, PATH_MAX, ".0123");
    //sprintf(binary_seq_file, "%s.0123", argv[optind]);
    
    fprintf(stderr, "* Binary seq file = %s\n", binary_seq_file);
    FILE *fr = fopen(binary_seq_file, "r");
    
    if (fr == NULL) {
        fprintf(stderr, "Error: can't open %s input file\n", binary_seq_file);
        exit(EXIT_FAILURE);
    }
    
    int64_t rlen = 0;
    fseek(fr, 0, SEEK_END); 
    rlen = ftell(fr);
    uint8_t* ref_string = (uint8_t*) _mm_malloc(rlen, 128);
    assert(ref_string != NULL);
    rewind(fr);
    
    /* Reading ref. sequence */
    err_fread_noeof(ref_string, 1, rlen, fr);
    fclose(fr);

    int batch_size=0, batch_count = 0;
    batch_size=atoi(argv[3]);
    assert(batch_size > 0);
    
    int64_t numthreads=atoi(argv[4]);
    assert(numthreads > 0);
    assert(numthreads <= omp_get_max_threads());

    char sa_pos_file_name[PATH_MAX];
    strcpy_s(sa_pos_file_name, PATH_MAX, argv[1]);
    strcat_s(sa_pos_file_name, PATH_MAX, ".pos_packed");
    
    FILE *sa_pos_fd;
    sa_pos_fd = fopen(sa_pos_file_name, "rb");
    if (sa_pos_fd == NULL) {
        fprintf(stderr, "[M::%s::STAR] Can't open suffix array position index File\n.", __func__);
        exit(1);
    }
    uint8_t* sa_position= (uint8_t*) _mm_malloc( 5 * suffixarray_num * sizeof(uint8_t), 64);
    assert(sa_position != NULL);
    if (bwa_verbose >= 3) {
        fprintf(stderr, "[M::%s::STAR] Reading suffixarray File [%s] to memory\n", __func__, sa_pos_file_name);
    }
    err_fread_noeof(sa_position, sizeof(uint8_t), 5 * suffixarray_num, sa_pos_fd);
    fclose(sa_pos_fd);


    if (bwa_verbose >= 3) {
        fprintf(stderr, "[M::%s::STAR] Reading LMER table to memory\n", __func__);
    }
    load_star_index(L_MER_SIZE, argv);

    bseq1_t *seqs;

    kseq_t* ks = kseq_init(fp);
    seqs = bseq_read_orig(QUERY_DB_SIZE*numthreads,  &numReads, ks, NULL, &total_size);
    // seqs = bseq_read_one_fasta_file(QUERY_DB_SIZE, &numReads, fp, &total_size);
    batch_count = batch_size;
    if (batch_count >= numReads){
        batch_count = numReads;
    }
    assert(batch_count!=0);
    int64_t num_batches = (numReads + batch_count - 1 ) / batch_count;

    int64_t *numTotalSmem = (int64_t *) malloc(num_batches * sizeof(int64_t));

    
    int64_t workTicks[numthreads];
    memset(workTicks, 0, numthreads * sizeof(int64_t));

    int max_readlength = seqs[0].l_seq;
    int min_readlength = seqs[0].l_seq;
    
    
    int64_t total_exec_time= 0;
    int32_t batch_id = 0;
    int64_t totalSmem = 0;
    int64_t totalnumread= 0;
    int64_t totalreadsize= 0;
    int32_t lastnumReads = 0;
    int lastnum_batches;
    int rid_start =0;
    while(numReads>0){
        
        lastnumReads=numReads;
        lastnum_batches = num_batches;
        totalnumread+= numReads;
        totalreadsize += total_size;
   
        total_exec_time += run_Star(sa_position, ref_string, fmiSearch, seqs, batch_count, 
                                        numTotalSmem, workTicks,num_batches, numReads, 
                                        numthreads,total_size, steps, rid_start , L_MER_SIZE);
        fprintf(stderr,"Processed %ld, Total exec time: %lld ticks, TotalReads = %d, max_readlength = %d, total_size = %lld\n",numReads,total_exec_time,  totalnumread, max_readlength, totalreadsize);
        rid_start += numReads;
        for(batch_id = 0; batch_id < num_batches; batch_id++)
        {
            totalSmem += numTotalSmem[batch_id];
        }
        // printf("Before free seqs\n");
        for (i=0;i<numReads;i++ ){
            // if(seqs[i]){
            //     if(seqs[i].name) free(seqs[i].name);
            //     if(seqs[i].comment) free(seqs[i].comment);
            //     if(seqs[i].seq) free(seqs[i].seq);
            //     if(seqs[i].qual) free(seqs[i].qual);
            //     if(seqs[i].sam) free(seqs[i].sam);
                
            // }
            // printf("numread:%d i=%d\n",numReads, i);
            free(seqs[i].name);
            free(seqs[i].comment);
            free(seqs[i].seq);
            free(seqs[i].qual);
            // free(seqs[i].sam);
        }
        free(seqs);
        
        // printf("Paasss\n");
        numReads =0;
        total_size =0;
        // kseq_t* ks = kseq_init(fp);
        seqs = bseq_read_orig(QUERY_DB_SIZE*numthreads,  &numReads, ks, NULL, &total_size);
        // seqs = bseq_read_one_fasta_file(QUERY_DB_SIZE, &numReads, fp, &total_size);
        if (seqs){
            batch_count=batch_size;
            if (batch_count >= numReads){
                batch_count = numReads;
            }
            assert(batch_count!=0);
            num_batches = (numReads + batch_count - 1 ) / batch_count;
            assert(num_batches!=0);
            // printf("%d %d %d\n", num_batches * sizeof(int64_t) ,num_batches * sizeof(SMEM *),numReads * sizeof(int32_t) );
            if (lastnum_batches < num_batches){
                // printf("%d %d %d\n", num_batches * sizeof(int64_t) ,num_batches * sizeof(SMEM *),numReads * sizeof(int32_t) );
                numTotalSmem = (int64_t *) realloc(numTotalSmem, num_batches * sizeof(int64_t));
            }
            
            

            }
        else{
            fprintf(stderr,"[Done]TotalReads = %d\n", totalnumread);    
            break;
        }

    }
    gzclose(fp);

    int64_t sumTicks = 0;
    int64_t maxTicks = 0;
    for(i = 0; i < numthreads; i++)
    {
        sumTicks += workTicks[i];
        if(workTicks[i] > maxTicks) maxTicks = workTicks[i];
    }
    double avgTicks = (sumTicks * 1.0) / numthreads;


    fprintf(stderr,"avgTicks = %lf, maxTicks = %ld, load imbalance = %lf\n", avgTicks, maxTicks, maxTicks/avgTicks);

    fprintf(stderr,"Consumed: %ld cycles\n", total_exec_time);
    
    fprintf(stderr,"totalSmems = %ld\n", totalSmem);
    if (steps>3){
        printf("[RESULT]\t%ld\t%d\t%lf\t%ld\t%ld\n",steps,numthreads, avgTicks, totalSmem, totalnumread);
    }


    
    // free(enc_qdb);
  
    
    free(numTotalSmem);
    delete fmiSearch;
    return 0;
}

