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

Authors: Youngmok Jung <tom418@kaist.ac.kr>; Dongsu Han <dhan.ee@kaist.ac.kr>.
*****************************************************************************************/

#include<stdio.h>
#include<stdlib.h>
#include<stdint.h>
#include <omp.h>
#include <string.h>
#include "bwa.h"
#include "FMI_search.h"
#include "LearnedIndex_seeding.h"
#include <omp.h>
#ifdef VTUNE_ANALYSIS
#include <ittnotify.h> 
#endif
#include "kseq.h"
KSEQ_DECLARE(gzFile)

// #include "kstring.h"
// #include "kvec.h"

// 10000000 * threadnum


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


int64_t run_learned(uint8_t* sa_position,uint8_t* ref2sa,uint8_t* ref_string , FMI_search* fmiSearch, bseq1_t *seqs, int batch_size, 
                int64_t* numTotalSmem, int64_t*workTicks, int64_t num_batches,
                int32_t numReads,int64_t numthreads, int64_t total_size, int steps, int rid_start ){
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
#if MEM_TRADEOFF
                iaux.ref2sa = ref2sa;
#endif
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
                    Learned_getSMEMsOnePosOneThread_no_smem(&iaux, &raux, smems, hits, hasN);
                    // printf("pivot: %llu lseq: %llu\n", raux.pivot, raux.l_seq);
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


int main(int argc, char **argv) {
#ifdef VTUNE_ANALYSIS
    __itt_pause();
#endif
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
    
    uint64_t suffixarray_num;
    
    char sa_pos_file_name[PATH_MAX];
    strcpy_s(sa_pos_file_name, PATH_MAX, argv[1]);
    #if LOADSUFFIX
    strcat_s(sa_pos_file_name, PATH_MAX, ".possa_packed");
    #else
    strcat_s(sa_pos_file_name, PATH_MAX, ".pos_packed");
    #endif

    FILE *sa_pos_fd;
    sa_pos_fd = fopen(sa_pos_file_name, "rb");
    if (sa_pos_fd == NULL) {
        fprintf(stderr, "[M::%s::LEARNED] Can't open suffix array position index File\n.", __func__);
        exit(1);
    }
    fseek(sa_pos_fd, 0, SEEK_END); 
    suffixarray_num = ftell(sa_pos_fd) / SASIZE;
    rewind(sa_pos_fd);


    if (bwa_verbose >= 3) {
        fprintf(stderr, "[M::%s::LEARNED] Reading RMI data to memory\n", __func__);
    }
    char learned_index_param_l0[PATH_MAX];
    strcpy_s(learned_index_param_l0, PATH_MAX, argv[1]); 
    strcat_s(learned_index_param_l0, PATH_MAX, ".suffixarray_uint64_L0_PARAMETERS");

    char learned_index_param_L2[PATH_MAX];
    strcpy_s(learned_index_param_L2, PATH_MAX, argv[1]); 
    strcat_s(learned_index_param_L2, PATH_MAX, ".suffixarray_uint64_L2_PARAMETERS");

    char learned_index_param_L1[PATH_MAX];
    strcpy_s(learned_index_param_L1, PATH_MAX, argv[1]); 
    strcat_s(learned_index_param_L1, PATH_MAX, ".suffixarray_uint64_L1_PARAMETERS");

    if (!learned_index_load(learned_index_param_l0, learned_index_param_L1,learned_index_param_L2, (double)suffixarray_num)){
        fprintf(stderr, "[M::%s::LEARNED] Can't load learned-index model, read_path:%s.\n", __func__, learned_index_param_L1);
        exit(1);
    }
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
    
 // 40 bit (5 byte) of sa_pos, 64bit (8 byte) of Key
    uint8_t* sa_position= (uint8_t*) _mm_malloc( SASIZE * suffixarray_num * sizeof(uint8_t), 64);
    #if MEM_TRADEOFF
    uint8_t* ref2sa = (uint8_t*) _mm_malloc(suffixarray_num*5* sizeof(uint8_t), 64);
    #endif

#if READ_FROM_FILE
    fprintf(stderr, "[M::%s::LEARNED] Reading Mode\n", __func__);
    
    
    if (sa_pos_fd == NULL) {
        fprintf(stderr, "[M::%s::LEARNED] Can't open suffix array position index File\n.", __func__);
        exit(1);
    }
    
    assert(sa_position != NULL);
    if (bwa_verbose >= 3) {
        fprintf(stderr, "[M::%s::LEARNED] Reading kmer POSSA File to memory\n", __func__);
    }
    err_fread_noeof(sa_position, sizeof(uint8_t), SASIZE * suffixarray_num, sa_pos_fd);

    #if MEM_TRADEOFF
    char ref_to_sapos_name[PATH_MAX];
    strcpy_s(ref_to_sapos_name, PATH_MAX, argv[1]);
    strcat_s(ref_to_sapos_name, PATH_MAX, ".ref2sa_packed");
    assert(ref2sa!= NULL);
    FILE *ref2sa_fd;
    ref2sa_fd = fopen(ref_to_sapos_name, "rb");
    if (ref2sa_fd == NULL) {
        fprintf(stderr, "[M::%s::LEARNED] Can't open ref2sa index\n.", __func__);
        exit(1);
    }
    if (bwa_verbose >= 3) {
        fprintf(stderr, "[M::%s::LEARNED] Reading ref2sa index File to memory\n", __func__);
    }
    err_fread_noeof(ref2sa, sizeof(uint8_t), 5*suffixarray_num, ref2sa_fd);
    fclose(ref2sa_fd);

    fclose(sa_pos_fd);
    #endif
#else
    fprintf(stderr, "[M::%s::LEARNED] Runtime Index-build Mode\n", __func__);
    if (bwa_verbose >= 3) {
        fprintf(stderr, "[M::%s::LEARNED] Reading pos_packed File to memory\n", __func__);
    }
    strcpy_s(sa_pos_file_name, PATH_MAX, argv[1]); 
    strcat_s(sa_pos_file_name, PATH_MAX, ".pos_packed");
    // FILE *sa_pos_fd;
    fclose(sa_pos_fd);
    sa_pos_fd = fopen(sa_pos_file_name, "rb");
    if (sa_pos_fd == NULL) {
        fprintf(stderr, "Can't open suffix array position index File\n.", __func__);
        exit(1);
    }
    assert(sa_position != NULL);
    double rtime = realtime();
    err_fread_noeof(sa_position, sizeof(uint8_t), 5*suffixarray_num, sa_pos_fd);
    fclose(sa_pos_fd); 

#if LOADSUFFIX
    if (bwa_verbose >= 3) {
        fprintf(stderr, "[M::%s::LEARNED] Generating Key data and ref2sa data in memory\n", __func__);
    }

    uint64_t index_build_batch_size = 256;
    uint64_t rr,end, start = suffixarray_num-1;
    while( start >  index_build_batch_size*8*2 ){
        end = start;
        start = (start >> 1) + 2;
#pragma omp parallel num_threads(16) shared(rr,start,end, index_build_batch_size)
{
#pragma omp for schedule(monotonic:dynamic) 
        for ( rr=start+1; rr <= end ; rr += index_build_batch_size){
            for (uint64_t idx=rr; idx < rr+index_build_batch_size &&idx<=end; idx++){
                memcpy(sa_position+idx*SASIZE, sa_position+idx*5, 5);
                uint64_t sa_pos_val = *(uint32_t*)(sa_position+idx*5);
                sa_pos_val = sa_pos_val <<8 | sa_position[idx*5+4];
                #if LOADSUFFIX
                *(uint64_t*)(sa_position + idx * SASIZE + 5) = get_key_of_ref(fmiSearch->idx->pac,sa_pos_val, suffixarray_num - sa_pos_val );
                #endif
         #if MEM_TRADEOFF
                *(uint32_t*)(ref2sa+sa_pos_val*5) |= (uint32_t) (idx>>8);
                *(ref2sa+sa_pos_val*5+4) = (uint8_t)(idx & 0xff);
         #endif
   
            }
            // fprintf(stderr,"start: %llu r: %llu\n",start,r);
            
        }
#pragma omp barrier
    
}
    }
    rr = start;
    for ( ; rr >=0 ; rr-= 1){
        memcpy(sa_position+rr*SASIZE, sa_position+rr*5, 5);
        uint64_t sa_pos_val = *(uint32_t*)(sa_position+rr*5);
        sa_pos_val = sa_pos_val <<8 | sa_position[rr*5+4];
        #if LOADSUFFIX
        *(uint64_t*)(sa_position + rr * SASIZE + 5) = get_key_of_ref(fmiSearch->idx->pac,sa_pos_val, suffixarray_num - sa_pos_val );
        #endif
        #if MEM_TRADEOFF
        *(uint32_t*)(ref2sa+sa_pos_val*5) |= (uint32_t) (rr>>8);
        *(ref2sa+sa_pos_val*5+4) = (uint8_t)(rr & 0xff);
        #endif
    
        if (rr ==0){
            break;
        }
    }
#else
    fprintf(stderr, "Loading-index took %.3f sec\n", realtime() - rtime);
#endif

#endif 


    int batch_size=0, batch_count = 0;
    batch_size=atoi(argv[3]);
    assert(batch_size > 0);
    
    int64_t numthreads=atoi(argv[4]);
    assert(numthreads > 0);
    assert(numthreads <= omp_get_max_threads());
    

    bseq1_t *seqs;


    // seqs = bseq_read_one_fasta_file(QUERY_DB_SIZE, &numReads, fp, &total_size);
    // if(seqs == NULL)
    // {
    //     printf("ERROR! seqs = NULL\n");
    //     exit(EXIT_FAILURE);
    // }
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
    #if MEM_TRADEOFF
        total_exec_time += run_learned(sa_position, ref2sa, ref_string, fmiSearch, seqs, batch_count, 
    #else
        total_exec_time += run_learned(sa_position, NULL, ref_string, fmiSearch, seqs, batch_count, 
    #endif
                                        numTotalSmem, workTicks,num_batches, numReads, 
                                        numthreads,total_size, steps, rid_start );
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

