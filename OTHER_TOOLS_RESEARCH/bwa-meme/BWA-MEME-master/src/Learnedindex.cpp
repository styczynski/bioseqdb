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
#include <stdlib.h>
#include <stdio.h>
#include "sais.h"
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <limits.h>
#include <pthread.h>
#include <errno.h>
#include <math.h>
#include <fstream>
#include <iostream>
#include <emmintrin.h>
#include "utils.h"
#include "Learnedindex.h"
#include "LearnedIndex_seeding.h"
#include "memcpy_bwamem.h"
#include <vector>
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif
#include "safe_mem_lib.h"
#include "safe_str_lib.h"
#include <snprintf_s.h>
#ifdef __cplusplus
}
#endif

#define assert_not_null(x, size, cur_alloc) \
        if (x == NULL) { fprintf(stderr, "Allocation of %0.2lf GB for " #x " failed.\nCurrent Allocation = %0.2lf GB\n", size * 1.0 /(1024*1024*1024), cur_alloc * 1.0 /(1024*1024*1024)); exit(EXIT_FAILURE); }


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


void buildSAandLEP(char* prefix, int num_threads){
    if (num_threads < 4){
        fprintf(stderr,"Warning: we recommend to use more number of threads(ex. 8, 16 or 32) to build index quickly.\n");
    }
    
    int status;
    int index_alloc  = 0;
    std::string reference_seq;
    char pac_file_name[PATH_MAX];
    strcpy_s(pac_file_name, PATH_MAX, prefix);
    strcat_s(pac_file_name, PATH_MAX, ".pac"); 
    fprintf(stderr,"[Build-Index-MEME] pac2nt function...\n");
    pac2nt_(pac_file_name, reference_seq);
    
    
    
    char binary_ref_name[PATH_MAX];
    strcpy_s(binary_ref_name, PATH_MAX, prefix);
    strcat_s(binary_ref_name, PATH_MAX, ".0123");
    std::fstream binary_ref_stream (binary_ref_name, std::ios::out | std::ios::binary);
    binary_ref_stream.seekg(0);	
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
    
    fprintf(stderr,"[Build-Index-MEME] ref seq len = %ld\n", pac_len);
    binary_ref_stream.write(binary_ref_seq, pac_len * sizeof(char));
    binary_ref_stream.close();
    assert(pac_len%2 ==0 );
    pac_len = pac_len + padded_t_len;
    for (i;i<pac_len;i++){
        binary_ref_seq[i] = 3;  
        reference_seq += "T";
    }
    fprintf(stderr,"[Build-Index-MEME] padded ref len = %ld\n", pac_len);
    //build suffix array
    size = (pac_len + 2) * sizeof(int64_t);
    int64_t *suffix_array=(int64_t *)_mm_malloc(size, 64);
    index_alloc += size;
    assert_not_null(suffix_array, size, index_alloc);


    fprintf(stderr,"[Build-Index-MEME] Building Suffix array with sais library\n");
    uint64_t query_k_mer = 32; // fix this to 32, use front 32 character for Learned Index model inference
    clock_t t;
    t = clock();
    status = saisxx(reference_seq.c_str(), suffix_array, pac_len);
    fprintf(stderr, "[Build-Index-MEME] Building Suffix array with sais library took %.2f sec\n", (float)(clock() - t) / CLOCKS_PER_SEC);
    // [    Ref      ][   Ref_complement   ][TTTTTTTTTT...T]
    
    uint64_t total_sa_num = pac_len - padded_t_len;//
    
    char filename[PATH_MAX];
    strcpy_s(filename, PATH_MAX, prefix);
    if (query_k_mer <= 32){
        strcat_s(filename, PATH_MAX, ".suffixarray_uint64");
    }
    else if (query_k_mer <= 64){
        strcat_s(filename, PATH_MAX, ".suffixarray_uint128");
    }
    
    std::ofstream sa_out(filename, std::ios_base::trunc | std::ios::binary);

    if (!sa_out.is_open()) {
      std::cerr << "unable to open " << filename << std::endl;
      exit(EXIT_FAILURE);
    }
    sa_out.write(reinterpret_cast<const char*>(&total_sa_num), sizeof(uint64_t));
    
    char pos_filename[PATH_MAX];
    // open pos_packed
    strcpy_s(pos_filename, PATH_MAX, prefix);
    strcat_s(pos_filename, PATH_MAX, ".pos_packed");
    std::ofstream pos_out(pos_filename, std::ios_base::trunc | std::ios::binary);

    if (!pos_out.is_open()) {
      std::cerr << "unable to open " << pos_filename << std::endl;
      exit(EXIT_FAILURE);
    }
    uint64_t index_build_batch_size = 10000;
    uint64_t r;
    uint64_t sa_count=0;
    uint64_t cumulative_sa = 0;

#if READ_FROM_FILE//READ_FROM_FILE
    
    strcpy_s(pos_filename, PATH_MAX, prefix);
    strcat_s(pos_filename, PATH_MAX, ".possa_packed");
    std::ofstream possa_out(pos_filename, std::ios_base::trunc | std::ios::binary);

    if (!possa_out.is_open()) {
      std::cerr << "unable to open " << pos_filename << std::endl;
      exit(EXIT_FAILURE);
    }
    fprintf(stderr,"[Build-LearnedIndexmode] Writing index files... should take a while\n");
    
    uint8_t *ref_to_sapos = (uint8_t *)_mm_malloc(5*total_sa_num*sizeof(uint8_t) , 64);
    uint8_t pos_out_batch[index_build_batch_size*5];
    uint8_t possa_out_batch[index_build_batch_size*13];
    uint8_t sa_out_batch[index_build_batch_size*8];
    for (i=0 ; i< pac_len; i += index_build_batch_size){
        int padded_t_flag = 0;
        uint64_t write_num =  i + index_build_batch_size < pac_len ? index_build_batch_size : pac_len - i;
            

        #pragma omp parallel num_threads(num_threads) shared(i, index_build_batch_size, padded_t_flag, write_num)
        {
            #pragma omp for schedule(monotonic:dynamic) 
            for (uint64_t j =i; j < i+write_num; j++){
                if (padded_t_flag) continue;
                if ( pac_len - padded_t_len <= suffix_array[j] &&  pac_len > suffix_array[j] ){
                    #pragma omp atomic
                    padded_t_flag++; // if there is padded T in current batch, we should process it with single-thread
                    continue;
                }
                // fill in ref2sa (Inverse suffix array)
                uint32_t val_ref2sa_4 = (cumulative_sa + (j-i)) >> 8;
                uint8_t val_ref2sa_1 = (cumulative_sa + (j-i)) & 0xff;
                memcpy( ref_to_sapos + suffix_array[j]*5, &val_ref2sa_4, 4);
                memcpy( ref_to_sapos + suffix_array[j]*5 + 4, &val_ref2sa_1, 1);

                // fill in suffix array in packed binary form
                uint32_t pos_val = suffix_array[j] >>8 ;
                uint8_t ls_val= suffix_array[j] & 0xff;
                memcpy(pos_out_batch + (j-i)*5, &pos_val, 4 );
                memcpy(pos_out_batch + (j-i)*5 + 4, &ls_val, 1 );

                // fill in suffix array and corresponding 64-bit suffix in packed binary form
                memcpy(possa_out_batch + (j-i)*13, &pos_val, 4 );
                memcpy(possa_out_batch + (j-i)*13 + 4, &ls_val, 1 );

                // below code generates the 64-bit suffix to be added to .suffixarray_uint64 and possa_packed file
                uint64_t binary_suffix_array = 0;
                uint64_t reverse_bit = 0;
                for (r =0 ; r < query_k_mer ; r++){
                    binary_suffix_array = binary_suffix_array << 2;
                    reverse_bit = reverse_bit <<2;
                    switch (binary_ref_seq[ (suffix_array[j]+r)%pac_len]){
                        case 0:
                            binary_suffix_array = (binary_suffix_array|0);
                            break;
                        case 1:
                            binary_suffix_array = (binary_suffix_array|1);
                            break;
                        case 2:
                            binary_suffix_array = (binary_suffix_array|2);
                            break;
                        case 3:
                            binary_suffix_array = (binary_suffix_array|3);
                            break;

                    }
                    switch (binary_ref_seq[ (suffix_array[j]+query_k_mer-r-1)%pac_len]){
                        case 0:
                            reverse_bit = (reverse_bit|0);
                            break;
                        case 1:
                            reverse_bit = (reverse_bit|1);
                            break;
                        case 2:
                            reverse_bit = (reverse_bit|2);
                            break;
                        case 3:
                            reverse_bit = (reverse_bit|3);
                            break;

                    }
                }

                memcpy(possa_out_batch + (j-i)*13+5, &reverse_bit, 8 );
                memcpy(sa_out_batch + (j-i)*8, &binary_suffix_array, 8 );

            }
            #pragma omp barrier
        }

        uint64_t padded_t_num = 0;
        if (padded_t_flag){
            for (uint64_t j =i; j < i+write_num; j++){
                if ( pac_len - padded_t_len <= suffix_array[j] &&  pac_len > suffix_array[j] ){
                    padded_t_num ++;
                    continue;
                }
                // fill in ref2sa (Inverse suffix array)
                uint32_t val_ref2sa_4 = (cumulative_sa + (j-i-padded_t_num)) >> 8;
                uint8_t val_ref2sa_1 = (cumulative_sa + (j-i-padded_t_num)) & 0xff;
                memcpy( ref_to_sapos + suffix_array[j]*5, &val_ref2sa_4, 4);
                memcpy( ref_to_sapos + suffix_array[j]*5 + 4, &val_ref2sa_1, 1);

                // fill in suffix array in packed binary form
                uint32_t pos_val = suffix_array[j] >>8 ;
                uint8_t ls_val= suffix_array[j] & 0xff;
                memcpy(pos_out_batch + (j-i-padded_t_num)*5, &pos_val, 4 );
                memcpy(pos_out_batch + (j-i-padded_t_num)*5 + 4, &ls_val, 1 );

                // fill in suffix array and corresponding 64-bit suffix in packed binary form
                memcpy(possa_out_batch + (j-i-padded_t_num)*13, &pos_val, 4 );
                memcpy(possa_out_batch + (j-i-padded_t_num)*13 + 4, &ls_val, 1 );

                // below code generates the 64-bit suffix to be added to .suffixarray_uint64 and possa_packed file   
                uint64_t binary_suffix_array = 0;
                uint64_t reverse_bit = 0;
                for (r =0 ; r < query_k_mer ; r++){
                    binary_suffix_array = binary_suffix_array << 2;
                    reverse_bit = reverse_bit <<2;
                    switch (binary_ref_seq[ (suffix_array[j]+r)%pac_len]){
                        case 0:
                            binary_suffix_array = (binary_suffix_array|0);
                            break;
                        case 1:
                            binary_suffix_array = (binary_suffix_array|1);
                            break;
                        case 2:
                            binary_suffix_array = (binary_suffix_array|2);
                            break;
                        case 3:
                            binary_suffix_array = (binary_suffix_array|3);
                            break;

                    }
                    switch (binary_ref_seq[ (suffix_array[j]+query_k_mer-r-1)%pac_len]){
                        case 0:
                            reverse_bit = (reverse_bit|0);
                            break;
                        case 1:
                            reverse_bit = (reverse_bit|1);
                            break;
                        case 2:
                            reverse_bit = (reverse_bit|2);
                            break;
                        case 3:
                            reverse_bit = (reverse_bit|3);
                            break;
                    }
                }
                memcpy(possa_out_batch + (j-i-padded_t_num)*13+5, &reverse_bit, 8 );
                memcpy(sa_out_batch + (j-i-padded_t_num)*8, &binary_suffix_array, 8 );
            }
        }
        write_num -= padded_t_num;
        cumulative_sa += write_num;

        pos_out.write(pos_out_batch, 5*write_num );
        possa_out.write(possa_out_batch, 13*write_num );
        sa_out.write(sa_out_batch, 8*write_num );

    }
    
    possa_out.close();
    sa_out.close();

    char ref_to_sapos_name[PATH_MAX];
    strcpy_s(ref_to_sapos_name, PATH_MAX, prefix);
    strcat_s(ref_to_sapos_name, PATH_MAX, ".ref2sa_packed");
    std::fstream ref2sa_stream (ref_to_sapos_name, std::ios::out | std::ios::binary);
    ref2sa_stream.seekg(0);	
    ref2sa_stream.write((char*)ref_to_sapos, 5*total_sa_num * sizeof(uint8_t));
    ref2sa_stream.close();
    _mm_free(ref_to_sapos);
    // hit_out.close();
#else // if we are going to build index at runtime, we need to save suffixarray_uint64 and pos_packed file
    uint8_t pos_out_batch[index_build_batch_size*5];
    uint8_t sa_out_batch[index_build_batch_size*8];
    t = clock();
    for (i=0 ; i< pac_len; i += index_build_batch_size){
        int padded_t_flag = 0;
        uint64_t write_num =  i + index_build_batch_size < pac_len ? index_build_batch_size : pac_len - i;

        #pragma omp parallel num_threads(num_threads) shared(i, index_build_batch_size, write_num, pos_out_batch, sa_out_batch)
        {
            #pragma omp for schedule(monotonic:dynamic) 
            for (uint64_t j =i; j < i+write_num; j++){
                if (padded_t_flag) continue;
                if ( pac_len - padded_t_len <= suffix_array[j] &&  pac_len > suffix_array[j] ){
                    #pragma omp atomic
                    padded_t_flag++; // if there is padded T in current batch, we should process it with single-thread
                    continue;
                }
                // fill in suffix array in packed binary form
                uint32_t pos_val = suffix_array[j] >>8 ;
                uint8_t ls_val= suffix_array[j] & 0xff;
                memcpy(pos_out_batch + (j-i)*5, &pos_val, 4 );
                memcpy(pos_out_batch + (j-i)*5 + 4, &ls_val, 1 );

                // below code generates the 64-bit suffix to be added to .suffixarray_uint64 and possa_packed file
                uint64_t binary_suffix_array = 0;
                for (r =0 ; r < query_k_mer ; r++){
                    binary_suffix_array = binary_suffix_array << 2;
                    switch (binary_ref_seq[ (suffix_array[j]+r)%pac_len]){
                        case 0:
                            binary_suffix_array = (binary_suffix_array|0);
                            break;
                        case 1:
                            binary_suffix_array = (binary_suffix_array|1);
                            break;
                        case 2:
                            binary_suffix_array = (binary_suffix_array|2);
                            break;
                        case 3:
                            binary_suffix_array = (binary_suffix_array|3);
                            break;
                    }
                }

                memcpy(sa_out_batch + (j-i)*8, &binary_suffix_array, 8 );

            }
            #pragma omp barrier
        }
        uint64_t padded_t_num = 0;
        if (padded_t_flag){
            for (uint64_t j =i; j < i+write_num; j++){
                if ( pac_len - padded_t_len <= suffix_array[j] &&  pac_len > suffix_array[j] ){
                    padded_t_num ++;
                    continue;
                }
                // fill in suffix array in packed binary form
                uint32_t pos_val = suffix_array[j] >>8 ;
                uint8_t ls_val= suffix_array[j] & 0xff;
                memcpy(pos_out_batch + (j-i-padded_t_num)*5, &pos_val, 4 );
                memcpy(pos_out_batch + (j-i-padded_t_num)*5 + 4, &ls_val, 1 );

                // below code generates the 64-bit suffix to be added to .suffixarray_uint64 and possa_packed file   
                uint64_t binary_suffix_array = 0;
                for (r =0 ; r < query_k_mer ; r++){
                    binary_suffix_array = binary_suffix_array << 2;
                    switch (binary_ref_seq[ (suffix_array[j]+r)%pac_len]){
                        case 0:
                            binary_suffix_array = (binary_suffix_array|0);
                            break;
                        case 1:
                            binary_suffix_array = (binary_suffix_array|1);
                            break;
                        case 2:
                            binary_suffix_array = (binary_suffix_array|2);
                            break;
                        case 3:
                            binary_suffix_array = (binary_suffix_array|3);
                            break;

                    }
                }
                memcpy(sa_out_batch + (j-i-padded_t_num)*8, &binary_suffix_array, 8 );
            }
        }
        write_num -= padded_t_num;
        pos_out.write(pos_out_batch, 5*write_num );
        sa_out.write(sa_out_batch, 8*write_num );
    }
#endif
    pos_out.close();
    sa_out.close();
    fprintf(stderr, "[Build-Index-MEME] Writing pos_packed file and suffixarray file took %.2f sec\n", (float)(clock() - t) / CLOCKS_PER_SEC);
    

    _mm_free(binary_ref_seq);
    _mm_free(suffix_array);
    

}
