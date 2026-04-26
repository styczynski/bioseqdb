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
#include <omp.h>

#ifdef VTUNE_ANALYSIS
#include <ittnotify.h> 
#endif

#include "kseq.h"
KSEQ_DECLARE(gzFile)

// #include "kstring.h"
// #include "kvec.h"

// #define intv_lt1(a, b) ((((uint64_t)(a).m) <<32 | ((uint64_t)(a).n)) < (((uint64_t)(b).m) <<32 | ((uint64_t)(b).n)))  // trial
// KSORT_INIT(mem_intv1, SMEM, intv_lt1)  // debug

#define QUERY_DB_SIZE 10000000
#define PRINT_OUTPUT

int64_t run_fmi(FMI_search* fmiSearch, bseq1_t *seqs, SMEM ** batchStart, int batch_size, 
                int64_t* numTotalSmem, int64_t*workTicks, int64_t num_batches,
                int32_t numReads,int numthreads, int64_t total_size, int steps, int rid_start ){
    int32_t *query_cum_len_ar = (int32_t *)_mm_malloc(numReads * sizeof(int32_t), 64);
    assert(query_cum_len_ar !=NULL);
    int64_t i;
    SMEM *matchArray[numthreads];
    memset(matchArray, 0, numthreads * sizeof(SMEM*));
    int max_readlength = seqs[0].l_seq;
    int min_readlength = seqs[0].l_seq;

    uint8_t *enc_qdb=(uint8_t *)malloc(numReads * max_readlength * sizeof(uint8_t));
    assert(enc_qdb !=NULL);
    int64_t cind,st;
    int64_t r;
    for (st=0; st < numReads; st++) {
        query_cum_len_ar[st] = st * max_readlength;
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

    startTick = __rdtsc();
    memset(numTotalSmem, 0, num_batches * sizeof(int64_t));
    memset(batchStart, 0, num_batches * sizeof(int64_t));
    
    
    int64_t perThreadQuota = std::max(numReads / numthreads, 12);
#ifdef VTUNE_ANALYSIS
    __itt_resume();
#endif
#pragma omp parallel num_threads(numthreads)
    {
        
        int tid = omp_get_thread_num();
        if(tid == 0)
            fprintf(stderr,"Running %d threads\n", omp_get_num_threads());

        int16_t *query_pos_ar = (int16_t*) _mm_malloc(20 * batch_size* max_readlength*sizeof(int16_t),64);
        int32_t *rid_array = (int32_t *)_mm_malloc(20*max_readlength * batch_size * sizeof(int32_t),64);
        int32_t *min_intv_array = (int32_t *) _mm_malloc(20 * numReads * sizeof(int32_t),64);
        int64_t matchArrayAlloc = perThreadQuota * 100;
        matchArray[tid] = (SMEM *)malloc(20*matchArrayAlloc * sizeof(SMEM));
        int64_t myTotalSmems = 0;
        int64_t startTick = __rdtsc();
        
#pragma omp for schedule(dynamic)
        
        for(i = 0; i < numReads; i += batch_size)
        {
            int32_t j;
            
            
            
            // memset(query_pos_ar,0, 20 * batch_size* max_readlength *sizeof(int16_t));
            int64_t pos = 0;
            int64_t num_smem1 = 0, num_smem2 = 0, num_smem3 = 0;
            int split_len = (int)(minSeedLen * 1.5 + .499);
            int64_t st1 = __rdtsc();
            int32_t batch_count = batch_size;
            if((i + batch_count) > numReads) batch_count = numReads - i;

            for(j = 0; j < i+batch_count; j++)
            {
                min_intv_array[j] = 1;
            }

            for(j = 0; j < batch_count; j++)
            {
                rid_array[j] = j;
            }
            int32_t batch_id = i/batch_size;
            //printf("%d] i = %d, batch_count = %d, batch_size = %d\n", tid, i, batch_count, batch_size);
            //fflush(stdout);
            
            if((matchArrayAlloc - myTotalSmems) < (batch_size * max_readlength))
            {
                
                matchArrayAlloc *= 2;
                matchArray[tid] = (SMEM *)realloc(matchArray[tid], 20*matchArrayAlloc * sizeof(SMEM)); 
            }
            
            fmiSearch->getSMEMsAllPosOneThread(enc_qdb + i * max_readlength,
                    min_intv_array,
                    rid_array,
                    batch_count,
                    batch_size,
                    seqs + i,
                    query_cum_len_ar,
                    max_readlength,
                    minSeedLen,
                    matchArray[tid] + myTotalSmems,
                    &num_smem1);
            
            
            
            numTotalSmem[batch_id] = num_smem1 + num_smem2 + num_smem3;
            
            batchStart[batch_id] = matchArray[tid] + myTotalSmems;

            // pos = 0;
            // int64_t smem_ptr = 0;
            // for (int l=0; l<nseq && pos < numTotalSmem[batch_id] - 1; l++) {
            //     pos = smem_ptr - 1;
            //     do {
            //         pos++;
            //     } while (pos < numTotalSmem[batch_id] - 1 && (matchArray[tid] + myTotalSmems)[pos].rid == (matchArray[tid] + myTotalSmems)[pos + 1].rid);
            //     int64_t n = pos + 1 - smem_ptr;
                
            //     if (n > 0)
            //         ks_introsort(mem_intv1, n, matchArray[tid] + myTotalSmems+smem_ptr);
            //     smem_ptr = pos + 1;
            // }

            
            int64_t et1 = __rdtsc();
            workTicks[tid] += (et1 - st1);
        }

        int64_t endTick = __rdtsc();
        
        _mm_free(query_pos_ar);
        _mm_free(rid_array);
        _mm_free(min_intv_array);
        free(matchArray[tid]);
    }

 #ifdef VTUNE_ANALYSIS
    __itt_pause();
#endif    
    endTick = __rdtsc();
    
    // if(query_cum_len_ar)_mm_free(query_cum_len_ar);
    _mm_free(query_cum_len_ar);
    // for(int tid = 0; tid < numthreads; tid++)
    // {
    //     if (matchArray[tid])free(matchArray[tid]);
    // }
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
    fmiSearch->load_index();
    
    int batch_size=0, batch_count = 0;
    batch_size=atoi(argv[3]);
    assert(batch_size > 0);
    
    int numthreads=atoi(argv[4]);
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

    int64_t *numTotalSmem = (int64_t *) _mm_malloc(num_batches * sizeof(int64_t),64);
    SMEM **batchStart = (SMEM **) _mm_malloc(num_batches * sizeof(SMEM *),64);

    
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
        total_exec_time += run_fmi(fmiSearch, seqs, batchStart, batch_count, 
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
                batchStart = (SMEM **) realloc(batchStart, num_batches * sizeof(SMEM *));
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
    // for(int tid = 0; tid < numthreads; tid++)
    // {
    //     free(matchArray[tid]);
    // }
    
    free(numTotalSmem);
    free(batchStart);
    // free(min_intv_array);
    delete fmiSearch;
    return 0;
}

