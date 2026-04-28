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
#include <string.h>
#include "bwa.h"
#include "FMI_search.h"
#include "ertseeding.h"
#include <omp.h>
#ifdef VTUNE_ANALYSIS
#include <ittnotify.h> 
#endif
#include "kseq.h"
KSEQ_DECLARE(gzFile)

// #include "kstring.h"
// #include "kvec.h"

#define QUERY_DB_SIZE 10000000
#define PRINT_OUTPUT

#define smem_lt_2(a, b) ((a).start == (b).start ? (a).end > (b).end : (a).start < (b).start)
KSORT_INIT(mem_smem_sort_lt, mem_t, smem_lt_2)

int64_t run_ert(uint64_t* kmer_offsets,uint8_t* mlt_table,uint8_t* ref_string , FMI_search* fmiSearch, bseq1_t *seqs,  int batch_size, 
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
    int64_t startTick, endTick;
    int split_len = (int)(minSeedLen * 1.5 + .499);
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
        mem_v* smems = (mem_v*) malloc(  100*MAX_LINE_LEN * sizeof(mem_v));
        assert(smems != NULL);
        kv_init_base(mem_t, smems[0], 100*MAX_LINE_LEN);
        kv_init_base(uint64_t, hits[0], 100*MAX_LINE_LEN);
        int64_t myTotalSmems = 0;
        int64_t startTick = __rdtsc();

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
                int hasN = 0;
                if (strchr(seqs[j+i].seq, 'N') || strchr(seqs[j+i].seq, 'n')) {
                    hasN = 1;
                }
                // make binary read data
                uint8_t unpacked_rc_queue_buf[ERT_MAX_READ_LEN];
                assert(len <= ERT_MAX_READ_LEN);
                for (k = 0; k < len; ++k) {
                    seq[k] = seq[k] < 4? seq[k] : nst_nt4_table[(int)seq[k]]; //nst_nt4??
                    unpacked_rc_queue_buf[len - k - 1] = seq[k] < 4 ? 3 - seq[k] : 4; 
                }
                
              
                index_aux_t iaux;
                iaux.kmer_offsets = kmer_offsets;
                iaux.mlt_table = mlt_table;
                iaux.bns = fmiSearch->idx->bns;
                iaux.pac = fmiSearch->idx->pac;
                iaux.ref_string = ref_string;

                read_aux_t raux;
                raux.min_seed_len = minSeedLen;
                raux.l_seq = len;
                raux.read_name = seqs[j+i].name;
                raux.unpacked_queue_buf = (uint8_t*) seq;
                raux.unpacked_rc_queue_buf = unpacked_rc_queue_buf;

                int64_t elem_t1 = __rdtsc();
                if (hasN) {
                    get_seeds(&iaux, &raux, smems, hits);
                }
                else {
                    get_seeds_prefix(&iaux, &raux, smems, hits);
                }
                if (steps > 1 ){
                    // 2. Reseeding: Break down larger SMEMs
                    int old_n = smems->n;
                    for (k = 0; k < old_n; ++k) {
                        int qbeg = smems->a[k].start;
                        int qend = smems->a[k].end;
                        if ((qend - qbeg) < split_len || smems->a[k].hitcount > 10) {
                            continue;
                        }
                        if (hasN) {
                            reseed(&iaux, &raux, smems, 
                                (qbeg + qend) >> 1, smems->a[k].hitcount + 1, 
                                &smems->a[k].pt, hits);
                        }
                        else {
                            reseed_prefix(&iaux, &raux, smems, 
                                        (qbeg + qend) >> 1, smems->a[k].hitcount + 1, 
                                        &smems->a[k].pt, hits);
                        }
                    }
                }
                if (steps > 2){
                    // 3. Apply LAST heuristic to find out non-overlapping seeds
                    last(&iaux, &raux, smems, 20, hits);
                }
                ks_introsort(mem_smem_sort_lt, smems->n, smems->a);
                int64_t elem_t2 = __rdtsc();
                if (steps > 3){
                    // printf("%u: time:%ld\n", j+i, elem_t2- elem_t1);
                    printf("%u:\n", j+i);
                    for(l = 0; l < smems->n; l++){
                        printf("[%u,%u]", smems->a[l].start,  smems->a[l].end);
                        printf(" [");
                        for (k=0; k< smems->a[l].hitcount; k++){
                            if (smems->a[l].forward || smems->a[l].fetch_leaves) {
                                printf("%ld,", hits->a[smems->a[l].hitbeg + k]);
                            }
                            else {
                                printf("%ld,", (fmiSearch->idx->bns->l_pac << 1) - (hits->a[smems->a[l].hitbeg + k] +  smems->a[l].end - smems->a[l].start - smems->a[l].end_correction));
                            }
                        }
                        printf("]\n"); 
                    }
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
    

    
    char kmer_tbl_file_name[PATH_MAX];
    strcpy_s(kmer_tbl_file_name, PATH_MAX, argv[1]); 
    strcat_s(kmer_tbl_file_name, PATH_MAX, ".kmer_table");
    char ml_tbl_file_name[PATH_MAX];
    strcpy_s(ml_tbl_file_name, PATH_MAX, argv[1]);
    strcat_s(ml_tbl_file_name, PATH_MAX, ".mlt_table");


    FILE *kmer_tbl_fd, *ml_tbl_fd;
    kmer_tbl_fd = fopen(kmer_tbl_file_name, "rb");
    if (kmer_tbl_fd == NULL) {
        fprintf(stderr, "[M::%s::ERT] Can't open k-mer index\n.", __func__);
        exit(1);
    }
    ml_tbl_fd = fopen(ml_tbl_file_name, "rb");
    if (ml_tbl_fd == NULL) {
        fprintf(stderr, "[M::%s::ERT] Can't open multi-level tree index\n.", __func__);
        exit(1);
    }
     //
    // Read k-mer index
    //
    uint64_t* kmer_offsets = (uint64_t*) malloc(numKmers * sizeof(uint64_t));
    assert(kmer_offsets != NULL);
    if (bwa_verbose >= 3) {
        fprintf(stderr, "[M::%s::ERT] Reading kmer index to memory\n", __func__);
    }
    err_fread_noeof(kmer_offsets, sizeof(uint64_t), numKmers, kmer_tbl_fd);

    //
    // Read multi-level tree index
    //
    fseek(ml_tbl_fd, 0L, SEEK_END);
    long size = ftell(ml_tbl_fd);
    uint8_t* mlt_table = (uint8_t*) malloc(size * sizeof(uint8_t));
    assert(mlt_table != NULL);
    fseek(ml_tbl_fd, 0L, SEEK_SET);
    if (bwa_verbose >= 3) {
        fprintf(stderr, "[M::%s::ERT] Reading multi-level tree index to memory\n", __func__);
    }
    err_fread_noeof(mlt_table, sizeof(uint8_t), size, ml_tbl_fd);

    fclose(kmer_tbl_fd);
    fclose(ml_tbl_fd);

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
    uint8_t* ref_string = (uint8_t*) _mm_malloc(rlen, 64);
    assert(ref_string != NULL);
    rewind(fr);
    
    /* Reading ref. sequence */
    err_fread_noeof(ref_string, 1, rlen, fr);    



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
        total_exec_time += run_ert(kmer_offsets, mlt_table, ref_string, fmiSearch, seqs, batch_count, 
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

    if (steps<4){
        printf("[RESULT]\t%ld\t%d\t%lf\t%ld\t%ld\n",steps,numthreads, avgTicks, totalSmem, totalnumread);
    }

    
    // _mm_free(query_cum_len_ar);
    // free(enc_qdb);
  
    
    free(numTotalSmem);
    delete fmiSearch;
    return 0;
}

