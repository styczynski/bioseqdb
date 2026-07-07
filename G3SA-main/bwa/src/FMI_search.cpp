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

Authors: Sanchit Misra <sanchit.misra@intel.com>; Vasimuddin Md <vasimuddin.md@intel.com>;
*****************************************************************************************/

#include <iostream>
#include <stdio.h>
#include <cstdio>
#include "sais.h"
#include <x86intrin.h>
#include "FMI_search.h"
//#include "memcpy_bwamem.h"
//#include "constants.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "safe_str_lib.h"
#ifdef __cplusplus
}
#endif

FMI_search::FMI_search(const char *fname)
{
    //fprintf(stderr, "* Entering FMI_search\n");
    //strcpy(file_name, fname);
    strcpy_s(file_name, PATH_MAX, fname);
    reference_seq_len = 0;
    sentinel_index = 0;
    index_alloc = 0;
    sa_ls_word = NULL;
    sa_ms_byte = NULL;
    cp_occ = NULL;
    one_hot_mask_array = NULL;
}

FMI_search::~FMI_search()
{
    if(sa_ms_byte)
        _mm_free(sa_ms_byte);
    if(sa_ls_word)
        _mm_free(sa_ls_word);
    if(cp_occ)
        _mm_free(cp_occ);
    if(cp_occ2)
        _mm_free(cp_occ2);
    if(one_hot_mask_array)
        _mm_free(one_hot_mask_array);
    /* FIXME segfault. handle later..
    if(packed_bwt)
        free(packed_bwt);
        */
}

int64_t FMI_search::pac_seq_len(const char *fn_pac)
{
	FILE *fp;
	int64_t pac_len;
	uint8_t c;
	fp = xopen(fn_pac, "rb");
	err_fseek(fp, -1, SEEK_END);
	pac_len = err_ftell(fp);
	err_fread_noeof(&c, 1, 1, fp);
	err_fclose(fp);
	return (pac_len - 1) * 4 + (int)c;
}

void FMI_search::pac2nt(const char *fn_pac, std::string &reference_seq)
{
	uint8_t *buf2;
	int64_t i, pac_size, seq_len;
	FILE *fp;

	// initialization
	seq_len = pac_seq_len(fn_pac);
    assert(seq_len > 0);
    assert(seq_len <= 0x7fffffffffL);
	fp = xopen(fn_pac, "rb");

	// prepare sequence
	pac_size = (seq_len>>2) + ((seq_len&3) == 0? 0 : 1);
	buf2 = (uint8_t*)calloc(pac_size, 1);
    assert(buf2 != NULL);
	err_fread_noeof(buf2, 1, pac_size, fp);
	err_fclose(fp);
	for (i = 0; i < seq_len; ++i) {
		int nt = buf2[i>>2] >> ((3 - (i&3)) << 1) & 3;
#if 1
        if (i < 8)
        {
            fprintf(stderr, "%d", nt);
        }
        if (i == 8)
        {
            fprintf(stderr, "\n");
        }
#endif
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

void FMI_search::build_bwt(const char *ref_file_name, char *binary_seq, int64_t ref_seq_len, int64_t *sa_bwt)
{
    uint8_t *bwt; // 2-bit packed bwt

    int64_t i;
    int64_t ref_seq_len_aligned = ((ref_seq_len + CP_BLOCK_SIZE - 1) / CP_BLOCK_SIZE) * CP_BLOCK_SIZE;
    int64_t size = ref_seq_len_aligned / 4 * sizeof(uint8_t);
    bwt = (uint8_t *)_mm_malloc(size, 64);
    memset(bwt, 0, size);
    assert_not_null(bwt, size, index_alloc);

    sentinel_index = -1;

    int idx_packed = 0;
    uint8_t packed_char = 0x00;
    for(i=0; i< ref_seq_len; i++)
    {
        if(sa_bwt[i] == 0)
        {
            packed_char = 0x3;
            fprintf(stderr, "BWT[%ld] = 4\n", i);
            sentinel_index = i;
        }
        else
        {
            char c = binary_seq[sa_bwt[i]-1];
            switch(c)
            {
                case 0: packed_char = 0x00;
                          break;
                case 1: packed_char = 0x01;
                          break;
                case 2: packed_char = 0x02;
                          break;
                case 3: packed_char = 0x03;
                          break;
                default:
                        fprintf(stderr, "ERROR! i = %ld, c = %c\n", i, c);
                        exit(EXIT_FAILURE);
            }
#if 1
            if(i < 8)
            {
                fprintf(stderr, "bwt[%d] = %d\n", i, c);
            }
#endif
        }

        idx_packed = i >> 2;
        bwt[idx_packed] = (bwt[idx_packed] << 2) | packed_char;
    }
/* Omitting the below. (ambiguous anyways when 2-bits packed)
    for(i = ref_seq_len; i < ref_seq_len_aligned; i++)
    {
        bwt[i] = DUMMY_CHAR;
    }
*/

        fprintf(stderr, "packed bwt[0..1] = %x %x\n", bwt[0], bwt[1]);
    char outname[PATH_MAX];

    strcpy_s(outname, PATH_MAX, ref_file_name);
    strcat_s(outname, PATH_MAX, ".packed.bwt");
    std::fstream outBwtStream (outname, std::ios::out | std::ios::binary);
    outBwtStream.seekg(0);	
    outBwtStream.write((char*)bwt, size);
    outBwtStream.close();
}

int FMI_search::build_fm_index(const char *ref_file_name, char *binary_seq, int64_t ref_seq_len, int64_t *sa_bwt)
{
    char outname[PATH_MAX];
#if 1
    fprintf(stderr, "[%s] ref_seq_len = %ld\n", __func__, ref_seq_len);
    fflush(stdout);


    fprintf(stderr, "count = %ld, %ld, %ld, %ld, %ld\n", count[0], count[1], count[2], count[3], count[4]);
    fprintf(stderr, "count2 = %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld\n", \
                    count2[0], count2[1], count2[2], count2[3], count2[4],\
                              count2[5], count2[6], count2[7], count2[8],\
                              count2[9], count2[10], count2[11], count2[12],\
                              count2[13], count2[14], count2[15], count2[16]);
    fflush(stdout);

// "prefix.counts"
    strcpy_s(outname, PATH_MAX, ref_file_name);
    strcat_s(outname, PATH_MAX, ".counts");
    std::fstream outCountsStream (outname, std::ios::out | std::ios::binary);
    outCountsStream.seekg(0);	

    ref_seq_len++; // to include the sentinel
    outCountsStream.write((char *)(&ref_seq_len), 1 * sizeof(int64_t));
    outCountsStream.write((char*)count, 5 * sizeof(int64_t));
    outCountsStream.write((char*)count2, 17 * sizeof(int64_t));
    outCountsStream.close();
// end of "prefix.counts"

#endif


    uint8_t *bwt;
    uint8_t *bwt2;

    int64_t i;
    int64_t ref_seq_len_aligned = ((ref_seq_len + CP_BLOCK_SIZE - 1) / CP_BLOCK_SIZE) * CP_BLOCK_SIZE;
    int64_t size = ref_seq_len_aligned * sizeof(uint8_t);
    bwt = (uint8_t *)_mm_malloc(size, 64);
    assert_not_null(bwt, size, index_alloc);
    bwt2 = (uint8_t *)_mm_malloc(size, 64);
    assert_not_null(bwt2, size, index_alloc);

    sentinel_index = -1;
    int64_t sentinel2_index = -1;

    cp_occ_size = (ref_seq_len >> CP_SHIFT) + 1;
    fprintf(stderr, "Building Occ1 and Occ2 tables each of %ld rows.\n", cp_occ_size);


    for(i=0; i< ref_seq_len; i++)
    {
        if(sa_bwt[i] == 0)
        {
            bwt[i] = 4; // <- This represents the virtual '$'
            fprintf(stderr, "BWT[%ld] = 4\n", i);
            sentinel_index = i;
        }
#if 1
        else
        {
            char c = binary_seq[sa_bwt[i]-1];
            switch(c)
            {
                case 0: bwt[i] = 0;
                          break;
                case 1: bwt[i] = 1;
                          break;
                case 2: bwt[i] = 2;
                          break;
                case 3: bwt[i] = 3;
                          break;
                default:
                        fprintf(stderr, "ERROR! i = %ld, c = %c\n", i, c);
                        exit(EXIT_FAILURE);
            }
        }

        if(sa_bwt[i] == 1)
        {
            bwt2[i] = 4;
            sentinel2_index = i;
            fprintf(stderr, "BWT2[%ld] = 4\n", i);
        }
        else
        {
            char c;
            if (sa_bwt[i] == 0)
            {
                c = binary_seq[ref_seq_len - 2];
            }
            else 
            {
                c = binary_seq[sa_bwt[i]-2];
            }
            switch(c)
            {
                case 0: bwt2[i] = 0;
                          break;
                case 1: bwt2[i] = 1;
                          break;
                case 2: bwt2[i] = 2;
                          break;
                case 3: bwt2[i] = 3;
                          break;
                default:
                        fprintf(stderr, "ERROR! (BWT2) i = %ld, c = %c\n", i, c);
                        exit(EXIT_FAILURE);
            }
        }
#endif
    }
    for(i = ref_seq_len; i < ref_seq_len_aligned; i++)
    {
        bwt[i] = DUMMY_CHAR;
        bwt2[i] = DUMMY_CHAR;
    }

    fprintf(stderr, "sentinel_index = %ld\n", sentinel_index);

#if 1
    fprintf(stderr, "CP_SHIFT = %d, CP_MASK = %d\n", CP_SHIFT, CP_MASK);
    fprintf(stderr, "sizeof CP_OCC = %ld\n", sizeof(CP_OCC));
    fflush(stdout);


    // create checkpointed occ
    cp_occ = NULL;
    cp_occ2 = NULL;

    size = cp_occ_size * sizeof(CP_OCC);
    cp_occ = (CP_OCC *)_mm_malloc(size, 64);
    assert_not_null(cp_occ, size, index_alloc);
    memset(cp_occ, 0, cp_occ_size * sizeof(CP_OCC));
    cp_occ2 = (CP_OCC2 *)_mm_malloc(cp_occ_size * sizeof(CP_OCC2), 64);
    assert_not_null(cp_occ2, size, index_alloc);
    memset(cp_occ2, 0, cp_occ_size * sizeof(CP_OCC2));

    int64_t cp_count[16];
    memset(cp_count, 0, 16 * sizeof(int64_t));
    int64_t cp_count2[16];
    memset(cp_count2, 0, 16 * sizeof(int64_t));
    for(i = 0; i < ref_seq_len; i++)
    {
        if((i & CP_MASK) == 0)
        {
            int32_t k;
            CP_OCC cpo;
            for (k=0; k<4; k++)
            {
                cpo.cp_count[k] = cp_count[k];
                cpo.one_hot_bwt_str[k] = 0;
            }
            CP_OCC2 cpo2;
            for (k=0; k<16; k++)
            {
                cpo2.cp_count[k] = cp_count2[k];
                cpo2.one_hot_bwt_str[k] = 0;
            }

			int32_t j;
			for(j = 0; j < CP_BLOCK_SIZE; j++)
			{
                for (k=0; k<4; k++)
                {
                    cpo.one_hot_bwt_str[k] = cpo.one_hot_bwt_str[k] << 1;
                }
                for (k=0; k<16; k++)
                {
                    cpo2.one_hot_bwt_str[k] = cpo2.one_hot_bwt_str[k] << 1;
                }
				uint8_t c = bwt[i + j];
				uint8_t c2 = bwt2[i + j];
                //fprintf(stderr, "c = %d\n", c);
                if(c < 4)
                {
                    cpo.one_hot_bwt_str[c] += 1;
                    if (c2 < 4)
                    {
                        cpo2.one_hot_bwt_str[c2 * 4 + c] += 1;
                    }
                }
			}
            cp_occ[i >> CP_SHIFT] = cpo;
            cp_occ2[i >> CP_SHIFT] = cpo2;
        }
        if (i != sentinel_index)
        {
            cp_count[bwt[i]]++;
            if (i != sentinel2_index)
            {
                cp_count2[bwt2[i] * 4 + bwt[i]]++;
            }
        }
    }

// "prefix.occ1"
    strcpy_s(outname, PATH_MAX, ref_file_name);
    strcat_s(outname, PATH_MAX, ".occ1");
    std::fstream outOcc1Stream (outname, std::ios::out | std::ios::binary);
    outOcc1Stream.seekg(0);	

    outOcc1Stream.write((char*)cp_occ, cp_occ_size * sizeof(CP_OCC));
    _mm_free(cp_occ); cp_occ = nullptr;
    _mm_free(bwt); bwt = nullptr;
    outOcc1Stream.close();
// end of "prefix.occ1"


// "prefix.occ2"
    strcpy_s(outname, PATH_MAX, ref_file_name);
    strcat_s(outname, PATH_MAX, ".occ2");
    std::fstream outOcc2Stream (outname, std::ios::out | std::ios::binary);
    outOcc2Stream.seekg(0);	

    outOcc2Stream.write((char*)cp_occ2, cp_occ_size * sizeof(CP_OCC2));
    _mm_free(cp_occ2); cp_occ2 = nullptr;
    _mm_free(bwt2); bwt2 = nullptr;
    outOcc2Stream.close();
// end of "prefix.occ2"
#endif

// "prefix.sa"
    strcpy_s(outname, PATH_MAX, ref_file_name);
    strcat_s(outname, PATH_MAX, ".sa.v2");
    std::fstream outSaStream (outname, std::ios::out | std::ios::binary);
    outSaStream.seekg(0);	

    outSaStream.write((const char*)(&first_base), 1 * sizeof(uint8_t));
    outSaStream.write((char *)(&sentinel_index), 1 * sizeof(int64_t));

#ifdef SA_COMPRESSION
    size = ((ref_seq_len >> SA_COMPX)+ 1)  * sizeof(uint32_t);
    uint32_t *sa_ls_word = (uint32_t *)_mm_malloc(size, 64);
    assert_not_null(sa_ls_word, size, index_alloc);
    size = ((ref_seq_len >> SA_COMPX) + 1) * sizeof(int8_t);
    int8_t *sa_ms_byte = (int8_t *)_mm_malloc(size, 64);
    assert_not_null(sa_ms_byte, size, index_alloc);
    int64_t pos = 0;
    for(i = 0; i < ref_seq_len; i++)
    {
        if ((i & SA_COMPX_MASK) == 0)
        {
            sa_ls_word[pos] = sa_bwt[i] & 0xffffffff;
            sa_ms_byte[pos] = (sa_bwt[i] >> 32) & 0xff;
            pos++;
        }
    }
    fprintf(stderr, "compressed SA length: %ld, compressed ref_seq_len__: %ld\n", pos, ref_seq_len >> SA_COMPX);
    outSaStream.write((char*)sa_ms_byte, ((ref_seq_len >> SA_COMPX) + 1) * sizeof(int8_t));
    outSaStream.write((char*)sa_ls_word, ((ref_seq_len >> SA_COMPX) + 1) * sizeof(uint32_t));
    
#else

    size = ref_seq_len * sizeof(uint32_t);
    uint32_t *sa_ls_word = (uint32_t *)_mm_malloc(size, 64);
    assert_not_null(sa_ls_word, size, index_alloc);
    size = ref_seq_len * sizeof(int8_t);
    int8_t *sa_ms_byte = (int8_t *)_mm_malloc(size, 64);
    assert_not_null(sa_ms_byte, size, index_alloc);
    for(i = 0; i < ref_seq_len; i++)
    {
        sa_ls_word[i] = sa_bwt[i] & 0xffffffff;
        sa_ms_byte[i] = (sa_bwt[i] >> 32) & 0xff;
    }
    outSaStream.write((char*)sa_ms_byte, ref_seq_len * sizeof(int8_t));
    outSaStream.write((char*)sa_ls_word, ref_seq_len * sizeof(uint32_t));
    
#endif

    fprintf(stderr, "max_occ_ind = %ld\n", i >> CP_SHIFT);    
    fflush(stdout);

    _mm_free(sa_ms_byte); sa_ms_byte = nullptr;
    _mm_free(sa_ls_word); sa_ls_word = nullptr;

    outSaStream.close();
// end of "prefix.sa"
    return 0;
}

int FMI_search::build_index() {

    char *prefix = file_name;
    uint64_t startTick;
    startTick = __rdtsc();
    index_alloc = 0;

    std::string reference_seq;
    char pac_file_name[PATH_MAX];
    strcpy_s(pac_file_name, PATH_MAX, prefix);
    strcat_s(pac_file_name, PATH_MAX, ".pac");
    //sprintf(pac_file_name, "%s.pac", prefix);

    // read from .pac to generate std::string reference_seq
    // we do not concatenate the complementary strand.
    pac2nt(pac_file_name, reference_seq); 
	int64_t pac_len = reference_seq.length();
    int status;
    int64_t size = pac_len * sizeof(char);

    // generate ACTG -> 0123 representation of the reference seq
    // and store it in .0123 file.
    // Counts tables are also computed.
    char *binary_ref_seq = (char *)_mm_malloc(size, 64);
    index_alloc += size;
    assert_not_null(binary_ref_seq, size, index_alloc);
    char binary_ref_name[PATH_MAX];
    strcpy_s(binary_ref_name, PATH_MAX, prefix);
    strcat_s(binary_ref_name, PATH_MAX, ".0123");
    //sprintf(binary_ref_name, "%s.0123", prefix);
    std::fstream binary_ref_stream (binary_ref_name, std::ios::out | std::ios::binary);
    binary_ref_stream.seekg(0);
    fprintf(stderr, "init ticks = %llu\n", __rdtsc() - startTick);
    startTick = __rdtsc();
    int64_t i;
	memset(count, 0, sizeof(int64_t) * 5);
    for(i = 0; i < pac_len; i++)
    {
        switch(reference_seq[i])
        {
            case 'A':
            binary_ref_seq[i] = 0, ++count[0];
            break;
            case 'C':
            binary_ref_seq[i] = 1, ++count[1];
            break;
            case 'G':
            binary_ref_seq[i] = 2, ++count[2];
            break;
            case 'T':
            binary_ref_seq[i] = 3, ++count[3];
            break;
            default:
            binary_ref_seq[i] = 4;
        }
    }
    first_base = (uint8_t)binary_ref_seq[0];
    //FIXME fprintf(stderr, "Last base = %d\n", (uint8_t)binary_ref_seq[(pac_len/2) - 1]);
    fprintf(stderr, "First base = %d\n", first_base);
    fprintf(stderr, "Raw count = %ld, %ld, %ld, %ld, %ld\n", count[0], count[1], count[2], count[3], count[4]);
    count[4]=count[0]+count[1]+count[2]+count[3];
    count[3]=count[0]+count[1]+count[2];
    count[2]=count[0]+count[1];
    count[1]=count[0];
    count[0]=0;
    fprintf(stderr, "count = %ld, %ld, %ld, %ld, %ld\n", count[0], count[1], count[2], count[3], count[4]);

	memset(count2, 0, sizeof(int64_t) * 17);
    for(i = 0; i < pac_len - 1; i++)
    {
        switch(reference_seq[i])
        {
            case 'A':
                switch(reference_seq[i + 1])
                {
                    case 'A': ++count2[0]; break;
                    case 'C': ++count2[1]; break;
                    case 'G': ++count2[2]; break;
                    case 'T': ++count2[3]; break;
                    default: fprintf(stderr, "Heey A\n");
                }
                break;
            case 'C':
                switch(reference_seq[i + 1])
                {
                    case 'A': ++count2[4]; break;
                    case 'C': ++count2[5]; break;
                    case 'G': ++count2[6]; break;
                    case 'T': ++count2[7]; break;
                    default: fprintf(stderr, "Heey C\n");
                }
                break;
            case 'G':
                switch(reference_seq[i + 1])
                {
                    case 'A': ++count2[8]; break;
                    case 'C': ++count2[9]; break;
                    case 'G': ++count2[10]; break;
                    case 'T': ++count2[11]; break;
                    default: fprintf(stderr, "Heey G\n");
                }
                break;
            case 'T':
                switch(reference_seq[i + 1])
                {
                    case 'A': ++count2[12]; break;
                    case 'C': ++count2[13]; break;
                    case 'G': ++count2[14]; break;
                    case 'T': ++count2[15]; break;
                    default: fprintf(stderr, "Heey T\n");
                }
                break;
            default:;
        }
    }

    fprintf(stderr, "Raw count2 = %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld\n", \
                    count2[0], count2[1], count2[2], count2[3], count2[4],\
                              count2[5], count2[6], count2[7], count2[8],\
                              count2[9], count2[10], count2[11], count2[12],\
                              count2[13], count2[14], count2[15], count2[16]);
    for (i = 16; i > 0; i--)
    {
        int64_t sum = 0;
        for (int ii = 0; ii < i; ii++)
        {
            sum += count2[ii];
        }
        count2[i] = sum;
    }
    count2[0] = 0;

    fprintf(stderr, "count2 = %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld\n", \
            count2[0], count2[1], count2[2], count2[3], count2[4],\
            count2[5], count2[6], count2[7], count2[8],\
            count2[9], count2[10], count2[11], count2[12],\
            count2[13], count2[14], count2[15], count2[16]);


    fprintf(stderr, "ref seq len = %ld\n", pac_len); // No longer 2x ref_len
    binary_ref_stream.write(binary_ref_seq, pac_len * sizeof(char));
    fprintf(stderr, "binary seq ticks = %llu\n", __rdtsc() - startTick);
    startTick = __rdtsc();

    size = (pac_len + 2) * sizeof(int64_t);

    int64_t *suffix_array=(int64_t *)_mm_malloc(size, 64);
    index_alloc += size;
    assert_not_null(suffix_array, size, index_alloc);
    startTick = __rdtsc();
	//status = saisxx<const char *, int64_t *, int64_t>(reference_seq.c_str(), suffix_array + 1, pac_len, 4);
	status = saisxx(reference_seq.c_str(), suffix_array + 1, pac_len);
	suffix_array[0] = pac_len;
    fprintf(stderr, "build suffix-array ticks = %llu\n", __rdtsc() - startTick);
    startTick = __rdtsc();

#if 1
	build_fm_index(prefix, binary_ref_seq, pac_len, suffix_array);//, count);
#else
	build_bwt(prefix, binary_ref_seq, pac_len, suffix_array);//, count);
#endif
    fprintf(stderr, "build fm-index ticks = %llu\n", __rdtsc() - startTick);
    //_mm_free(binary_ref_seq); binary_ref_seq = nullptr;
    //_mm_free(suffix_array); suffix_array = nullptr;
    return 0;
}

void FMI_search::load_index()
{
    // oneHot
    //fprintf(stderr, "Creating one_hot_mask_array\n");
    //one_hot_mask_array = (uint64_t *)_mm_malloc(64 * sizeof(uint64_t), 64);
    one_hot_mask_array = (uint64_t *)malloc(64 * sizeof(uint64_t));
    uint64_t base = 0x8000000000000000L;
    one_hot_mask_array[0] = base;
    int64_t i;
    for(i = 1; i < 64; i++)
    {
        one_hot_mask_array[i] = (one_hot_mask_array[i - 1] >> 1) | base;
    }
    //fprintf(stderr, "Created one_hot_mask_array\n");
    //fprintf(stderr, "samples: %016lx %016lx %016lx %016lx\n", one_hot_mask_array[0],  one_hot_mask_array[1],  one_hot_mask_array[2], one_hot_mask_array[63]); 


    char *ref_file_name = file_name;
    char index_file_name[PATH_MAX];
    FILE *occ1Stream = NULL;
    FILE *occ2Stream = NULL;
    FILE *saStream = NULL;
    FILE *countsStream = NULL;
    FILE *packedBwtStream = NULL;

    // Read the counts 
    strcpy_s(index_file_name, PATH_MAX, ref_file_name);
    strcat_s(index_file_name, PATH_MAX, ".counts");
    countsStream = fopen(index_file_name,"rb");
    if (countsStream == NULL)
    {
        fprintf(stderr, "ERROR! Unable to open the file: %s\n", index_file_name);
        exit(EXIT_FAILURE);
    }
    else
    {
        fprintf(stderr, "* Index file found. Loading index from %s\n", index_file_name);
    }

    err_fread_noeof(&reference_seq_len, sizeof(int64_t), 1, countsStream);
    assert(reference_seq_len > 0);
    assert(reference_seq_len <= 0x7fffffffffL);
    fprintf(stderr, "* Reference seq len for bi-index = %ld\n", reference_seq_len);

    err_fread_noeof(&count[0], sizeof(int64_t), 5, countsStream);
    err_fread_noeof(&count2[0], sizeof(int64_t), 17, countsStream);
    fclose(countsStream);

    cp_occ_size = (reference_seq_len >> CP_SHIFT) + 1;
    cp_occ = NULL;
    // Load occ1
    strcpy_s(index_file_name, PATH_MAX, ref_file_name);
    strcat_s(index_file_name, PATH_MAX, ".occ1");
    occ1Stream = fopen(index_file_name,"rb");
    if (occ1Stream == NULL)
    {
        fprintf(stderr, "ERROR! Unable to open the file: %s\n", index_file_name);
        exit(EXIT_FAILURE);
    }
    else
    {
        fprintf(stderr, "* Index file found. Loading index from %s\n", index_file_name);
    }

    if ((cp_occ = (CP_OCC *)_mm_malloc(cp_occ_size * sizeof(CP_OCC), 64)) == NULL) {
        fprintf(stderr, "ERROR! unable to allocated cp_occ memory\n");
        exit(EXIT_FAILURE);
    }
    err_fread_noeof(cp_occ, sizeof(CP_OCC), cp_occ_size, occ1Stream);
    fclose(occ1Stream);

    //fprintf(stderr, "Just loaded from file cp_occ[0] %ld %ld %ld %ld %lu %lu %lu %lu \n",\
            cp_occ[0].cp_count[0],\
            cp_occ[0].cp_count[1],\
            cp_occ[0].cp_count[2],\
            cp_occ[0].cp_count[3],\
            cp_occ[0].one_hot_bwt_str[0],\
            cp_occ[0].one_hot_bwt_str[1],\
            cp_occ[0].one_hot_bwt_str[2],\
            cp_occ[0].one_hot_bwt_str[3]);

    // Load occ2
    strcpy_s(index_file_name, PATH_MAX, ref_file_name);
    strcat_s(index_file_name, PATH_MAX, ".occ2");
    occ2Stream = fopen(index_file_name,"rb");
    if (occ2Stream == NULL)
    {
        fprintf(stderr, "ERROR! Unable to open the file: %s\n", index_file_name);
        exit(EXIT_FAILURE);
    }
    else
    {
        fprintf(stderr, "* Index file found. Loading index from %s\n", index_file_name);
    }

    if((cp_occ2 = (CP_OCC2 *)_mm_malloc(cp_occ_size * sizeof(CP_OCC2), 64)) == NULL) {
        fprintf(stderr, "ERROR! unable to allocated cp_occ2 memory\n");
        exit(EXIT_FAILURE);
    }
    err_fread_noeof(cp_occ2, sizeof(CP_OCC2), cp_occ_size, occ2Stream);
    fclose(occ2Stream);


    //fprintf(stderr, "Just loaded from file cp_occ2[0] %ld %lu \n",\
            cp_occ2[0].cp_count[0],\
            cp_occ2[0].one_hot_bwt_str[0]);

            /*
    {
        GET_OCC(0, 2, occ_id_zero, y_zero, occ_zero, one_hot_bwt_str_2_zero, match_mask_zero);
        fprintf(stderr, "* GET_OCC(0, 'G') == %ld\n", occ_zero);
    }
    {
        GET_OCC2(0, 6, occ_id_zero, y_zero, occ_zero, one_hot_bwt_str_2_zero, match_mask_zero);
        fprintf(stderr, "* GET_OCC2(0, 'CG') == %ld\n", occ_zero);
    }
    {
        GET_OCC2(0, 7, occ_id_zero, y_zero, occ_zero, one_hot_bwt_str_2_zero, match_mask_zero);
        fprintf(stderr, "* GET_OCC2(0, 'CT') == %ld\n", occ_zero);
    }
    {
        GET_OCC2(59373566, 5, occ_id_pp, y_pp, occ_pp, one_hot_bwt_str_2_pp, match_mask_pp);
        fprintf(stderr, "* GET_OCC2(59373566, 'CC') == %ld\n", occ_pp);
    }
    {
        GET_OCC2(59373567, 5, occ_id_pp, y_pp, occ_pp, one_hot_bwt_str_2_pp, match_mask_pp);
        fprintf(stderr, "* GET_OCC2(59373567, 'CC') == %ld\n", occ_pp);
    }
    {
        GET_OCC2(59373568, 5, occ_id_pp, y_pp, occ_pp, one_hot_bwt_str_2_pp, match_mask_pp);
        fprintf(stderr, "* GET_OCC2(59373568, 'CC') == %ld\n", occ_pp);
    }
    */


    strcpy_s(index_file_name, PATH_MAX, ref_file_name);
    strcat_s(index_file_name, PATH_MAX, ".sa.v2");
    saStream = fopen(index_file_name,"rb");
    if (saStream == NULL)
    {
        fprintf(stderr, "ERROR! Unable to open the file: %s\n", index_file_name);
        exit(EXIT_FAILURE);
    }
    else
    {
        fprintf(stderr, "* Index file found. Loading index from %s\n", index_file_name);
    }

    err_fread_noeof(&first_base, sizeof(uint8_t), 1, saStream);
    //fprintf(stderr, "* first base: %d\n", first_base);
    err_fread_noeof(&sentinel_index, sizeof(int64_t), 1, saStream);
    //fprintf(stderr, "* sentinel-index: %ld\n", sentinel_index);

#define USE_BWA_MEM_GPU_SA2REF
#ifndef USE_BWA_MEM_GPU_SA2REF
    // load suffix array

#ifdef SA_COMPRESSION
    int64_t reference_seq_len_ = (reference_seq_len >> SA_COMPX) + 1;
    sa_ms_byte = (int8_t *)_mm_malloc(reference_seq_len_ * sizeof(int8_t), 64);
    sa_ls_word = (uint32_t *)_mm_malloc(reference_seq_len_ * sizeof(uint32_t), 64);
    err_fread_noeof(sa_ms_byte, sizeof(int8_t), reference_seq_len_, saStream);
    err_fread_noeof(sa_ls_word, sizeof(uint32_t), reference_seq_len_, saStream);
#else
    sa_ms_byte = (int8_t *)_mm_malloc(reference_seq_len * sizeof(int8_t), 64);
    sa_ls_word = (uint32_t *)_mm_malloc(reference_seq_len * sizeof(uint32_t), 64);
    err_fread_noeof(sa_ms_byte, sizeof(int8_t), reference_seq_len, saStream);
    err_fread_noeof(sa_ls_word, sizeof(uint32_t), reference_seq_len, saStream);
#endif

    // Load the packed BWT
    strcpy_s(index_file_name, PATH_MAX, ref_file_name);
    strcat_s(index_file_name, PATH_MAX, ".packed.bwt");
    packedBwtStream = fopen(index_file_name,"rb");
    if (packedBwtStream == NULL)
    {
        fprintf(stderr, "ERROR! Unable to open the file: %s\n", index_file_name);
        exit(EXIT_FAILURE);
    }
    else
    {
        fprintf(stderr, "* Index file found. Loading index from %s\n", index_file_name);
    }

    int64_t ref_seq_len_aligned = ((reference_seq_len + CP_BLOCK_SIZE - 1) / CP_BLOCK_SIZE) * CP_BLOCK_SIZE;
    int64_t packedBwtSize = ref_seq_len_aligned / 4 * sizeof(uint8_t);
    packed_bwt = (uint8_t*)malloc(packedBwtSize);
    err_fread_noeof(packed_bwt, packedBwtSize, 1, packedBwtStream);
    fclose(packedBwtStream);
    // end of loading the packed Bwt
#endif
    fclose(saStream);

    int64_t ii = 0;
    for(ii = 0; ii < 5; ii++)// update read count structure
    {
        count[ii] = count[ii] + 1;
    }
    for(ii = 0; ii < 17; ii++)// update read count structure
    {
        count2[ii] = count2[ii] + 1;
    }
    for (ii = 4 * (3 - (int64_t)first_base); ii < 17; ii++)
    {
        count2[ii] = count2[ii] + 1;
    }
    /*
    fprintf(stderr, "* Count:\n");
    int x;
    for(x = 0; x < 5; x++)
    {
        fprintf(stderr, "%ld,\t%ld\n", x, count[x]);
    }
    fprintf(stderr, "* Count2:\n");
    for(x = 0; x < 17; x++)
    {
        fprintf(stderr, "%ld,\t%ld\n", x, count2[x]);
    }
    */

    /*
    fprintf(stderr, "* Reading other elements of the index from files %s\n",
            ref_file_name);
    bwa_idx_load_ele(ref_file_name, BWA_IDX_ALL);
    */
    //fprintf(stderr, "* Done reading Index!!\n");
}

void FMI_search::getSMEMsOnePosOneThread(uint8_t *enc_qdb,
                                         int16_t *query_pos_array,
                                         int32_t *min_intv_array,
                                         int32_t *rid_array,
                                         int32_t numReads,
                                         int32_t batch_size,
                                         const bseq1_t *seq_,
                                         int32_t *query_cum_len_ar,
                                         int32_t max_readlength,
                                         int32_t minSeedLen,
                                         SMEM *matchArray,
                                         int64_t *__numTotalSmem)
{
    int64_t numTotalSmem = *__numTotalSmem;
    SMEM prevArray[max_readlength];

    uint32_t i;
    // Perform SMEM for original reads
    for(i = 0; i < numReads; i++)
    {
        int x = query_pos_array[i];
        int32_t rid = rid_array[i];
        int next_x = x + 1;

        int readlength = seq_[rid].l_seq;
        int offset = query_cum_len_ar[rid];
        // uint8_t a = enc_qdb[rid * readlength + x];
        uint8_t a = enc_qdb[offset + x];
        uint8_t a2;

        if(a < 4)
        {
            SMEM smem;
            smem.rid = rid;
            smem.m = x;
            smem.n = x;
            smem.k = count[a];
            smem.l = count[3 - a];
            smem.s = count[a+1] - count[a];
            int numPrev = 0;

            int j;
            if (x == 0)
            {
                //for(j = x + 1; j < readlength; j += 2)
                for(j = x + 1; j < readlength; j += 1)
                {
                    // a = enc_qdb[rid * readlength + j];
                    a = enc_qdb[offset + j];
                    //a2 = enc_qdb[offset + j + 1];
                    next_x = j + 1;
                    //if(a < 4 && a2 < 4)
                    if(a < 4)
                    {
                        SMEM smem_ = smem;

                        // Forward extension is backward extension with the BWT of reverse complement
                        //SMEM newSmem_ = forwardExt(smem_, 3 - a);
                        smem_.k = smem.l;
                        smem_.l = smem.k;
                        //SMEM newSmem_ = backwardExt2(smem_, 3 - a2, 3 - a);
                        SMEM newSmem_ = backwardExt(smem_, 3 - a);
                        SMEM newSmem = newSmem_;
                        newSmem.k = newSmem_.l;
                        newSmem.l = newSmem_.k;
                        //newSmem.n = j + 1;
                        newSmem.n = j;

#if 0                   // Debug each 2 extend
                        fprintf(stderr, "\nSAI (k, l, s) before forward extend 2: %ld, %ld, %ld\n", smem.k, smem.l, smem.s);
                        SMEM newSmemRef_ = backwardExt(smem_, 3 - a);
                        SMEM newSmemRef2_ = backwardExt(newSmemRef_, 3 - a2);
                        fprintf(stderr, "* SAI (k, l, s) after forward extend (base=%d): %ld, %ld, %ld\n", a, newSmemRef_.l, newSmemRef_.k, newSmemRef_.s);
                        fprintf(stderr, "* SAI (k, l, s) after forward extend (base=%d): %ld, %ld, %ld\n", a2, newSmemRef2_.l, newSmemRef2_.k, newSmemRef2_.s);
                        fprintf(stderr, "SAI (k, l, s) after forward extend 2 (base=%d, %d): %ld, %ld, %ld\n", a, a2, newSmem.k, newSmem.l, newSmem.s);
                        newSmem_ = newSmemRef2_;
                        newSmem = newSmem_;
                        newSmem.k = newSmem_.l;
                        newSmem.l = newSmem_.k;
                        newSmem.n = j;
#endif
                        int32_t s_neq_mask = newSmem.s != smem.s;

                        prevArray[numPrev] = smem;
                        numPrev += s_neq_mask;
                        if(newSmem.s < min_intv_array[i])
                        {
                            next_x = j;

                            break;
                        }
                        smem = newSmem;
#ifdef ENABLE_PREFETCH
                        _mm_prefetch((const char *)(&cp_occ[(smem.k) >> CP_SHIFT]), _MM_HINT_T0);
                        _mm_prefetch((const char *)(&cp_occ[(smem.l) >> CP_SHIFT]), _MM_HINT_T0);
#endif
                    }
                    else
                    {
                        break;
                    }
                }
            }
            else 
            {
                for(j = x + 1; j < readlength; j++)
                {
                    // a = enc_qdb[rid * readlength + j];
                    a = enc_qdb[offset + j];
                    next_x = j + 1;
                    if(a < 4)
                    {
                        SMEM smem_ = smem;

                        // Forward extension is backward extension with the BWT of reverse complement

                        smem_.k = smem.l;
                        smem_.l = smem.k;
                        SMEM newSmem_ = backwardExt(smem_, 3 - a);
                        //SMEM newSmem_ = forwardExt(smem_, 3 - a);
                        SMEM newSmem = newSmem_;
                        newSmem.k = newSmem_.l;
                        newSmem.l = newSmem_.k;
                        newSmem.n = j;

                        int32_t s_neq_mask = newSmem.s != smem.s;

                        prevArray[numPrev] = smem;
                        numPrev += s_neq_mask;
                        if(newSmem.s < min_intv_array[i])
                        {
                            next_x = j;
                            break;
                        }
                        smem = newSmem;
#ifdef ENABLE_PREFETCH
                        _mm_prefetch((const char *)(&cp_occ[(smem.k) >> CP_SHIFT]), _MM_HINT_T0);
                        _mm_prefetch((const char *)(&cp_occ[(smem.l) >> CP_SHIFT]), _MM_HINT_T0);
#endif
                    }
                    else
                    {
                        break;
                    }
                }
            }
            if(smem.s >= min_intv_array[i])
            {
                prevArray[numPrev] = smem;
                numPrev++;
            }

            SMEM *prev;
            prev = prevArray;
            int p;
            for(p = 0; p < (numPrev/2); p++)
            {
                SMEM temp = prev[p];
                prev[p] = prev[numPrev - p - 1];
                prev[numPrev - p - 1] = temp;
            }

            // Backward search
            //int cur_j = readlength;
            for(j = x - 1; j >= 0; j--)
            {
                int numCurr = 0;
                int curr_s = -1;
                // a = enc_qdb[rid * readlength + j];
                a = enc_qdb[offset + j];

                if(a > 3)
                {
                    break;
                }
                for(p = 0; p < numPrev; p++)
                {
                    SMEM smem = prev[p];
                    SMEM newSmem = backwardExt(smem, a);
                    newSmem.m = j;

                    if((newSmem.s < min_intv_array[i]) && ((smem.n - smem.m + 1) >= minSeedLen))
                    {
                        //cur_j = j;
                        matchArray[numTotalSmem++] = smem;
                        break;
                    }
                    if((newSmem.s >= min_intv_array[i]) && (newSmem.s != curr_s))
                    {
                        curr_s = newSmem.s;
                        prev[numCurr++] = newSmem;
#ifdef ENABLE_PREFETCH
                        _mm_prefetch((const char *)(&cp_occ[(newSmem.k) >> CP_SHIFT]), _MM_HINT_T0);
                        _mm_prefetch((const char *)(&cp_occ[(newSmem.k + newSmem.s) >> CP_SHIFT]), _MM_HINT_T0);
#endif
                        break;
                    }
                }
                p++;
                for(; p < numPrev; p++)
                {
                    SMEM smem = prev[p];

                    SMEM newSmem = backwardExt(smem, a);
                    newSmem.m = j;


                    if((newSmem.s >= min_intv_array[i]) && (newSmem.s != curr_s))
                    {
                        curr_s = newSmem.s;
                        prev[numCurr++] = newSmem;
#ifdef ENABLE_PREFETCH
                        _mm_prefetch((const char *)(&cp_occ[(newSmem.k) >> CP_SHIFT]), _MM_HINT_T0);
                        _mm_prefetch((const char *)(&cp_occ[(newSmem.k + newSmem.s) >> CP_SHIFT]), _MM_HINT_T0);
#endif
                    }
                }
                numPrev = numCurr;
                if(numCurr == 0)
                {
                    break;
                }
            }
            if(numPrev != 0)
            {
                SMEM smem = prev[0];
                if(((smem.n - smem.m + 1) >= minSeedLen))
                {

                    matchArray[numTotalSmem++] = smem;
                }
                numPrev = 0;
            }
        }
        query_pos_array[i] = next_x;
    }
    (*__numTotalSmem) = numTotalSmem;
}

void FMI_search::getSMEMsAllPosOneThread(uint8_t *enc_qdb,
        int32_t *min_intv_array,
        int32_t *rid_array,
        int32_t numReads,
        int32_t batch_size,
        const bseq1_t *seq_,
        int32_t *query_cum_len_ar,
        int32_t max_readlength,
        int32_t minSeedLen,
        SMEM *matchArray,
        int64_t *__numTotalSmem)
{
    int16_t *query_pos_array = (int16_t *)_mm_malloc(numReads * sizeof(int16_t), 64);

    int32_t i;
    for(i = 0; i < numReads; i++)
        query_pos_array[i] = 0;

    int32_t numActive = numReads;
    (*__numTotalSmem) = 0;

    do
    {
        int32_t head = 0;
        int32_t tail = 0;
        for(head = 0; head < numActive; head++)
        {
            int readlength = seq_[rid_array[head]].l_seq;
            if(query_pos_array[head] < readlength)
            {
                rid_array[tail] = rid_array[head];
                query_pos_array[tail] = query_pos_array[head];
                min_intv_array[tail] = min_intv_array[head];
                tail++;             
            }               
        }
        getSMEMsOnePosOneThread(enc_qdb,
                query_pos_array,
                min_intv_array,
                rid_array,
                tail,
                batch_size,
                seq_,
                query_cum_len_ar,
                max_readlength,
                minSeedLen,
                matchArray,
                __numTotalSmem);
        numActive = tail;
    } while(numActive > 0);

    _mm_free(query_pos_array);
}

int64_t FMI_search::bwtSeedStrategyAllPosOneThread(uint8_t *enc_qdb,
                                                   int32_t *max_intv_array,
                                                   int32_t numReads,
                                                   const bseq1_t *seq_,
                                                   int32_t *query_cum_len_ar,
                                                   int32_t minSeedLen,
                                                   SMEM *matchArray)
{
    int32_t i;

    int64_t numTotalSeed = 0;

    for(i = 0; i < numReads; i++)
    {
        int readlength = seq_[i].l_seq;
        int16_t x = 0;
        while(x < readlength)
        {
            int next_x = x + 1;

            // Forward search
            SMEM smem;
            smem.rid = i;
            smem.m = x;
            smem.n = x;
            
            int offset = query_cum_len_ar[i];
            uint8_t a = enc_qdb[offset + x];
            // uint8_t a = enc_qdb[i * readlength + x];

            if(a < 4)
            {
                smem.k = count[a];
                smem.l = count[3 - a];
                smem.s = count[a+1] - count[a];


                int j;
                for(j = x + 1; j < readlength; j++)
                {
                    next_x = j + 1;
                    // a = enc_qdb[i * readlength + j];
                    a = enc_qdb[offset + j];
                    if(a < 4)
                    {
                        SMEM smem_ = smem;

                        // Forward extension is backward extension with the BWT of reverse complement
                        smem_.k = smem.l;
                        smem_.l = smem.k;
                        SMEM newSmem_ = backwardExt(smem_, 3 - a);
                        //SMEM smem = backwardExt(smem, 3 - a);
                        //smem.n = j;
                        SMEM newSmem = newSmem_;
                        newSmem.k = newSmem_.l;
                        newSmem.l = newSmem_.k;
                        newSmem.n = j;
                        smem = newSmem;
#ifdef ENABLE_PREFETCH
                        _mm_prefetch((const char *)(&cp_occ[(smem.k) >> CP_SHIFT]), _MM_HINT_T0);
                        _mm_prefetch((const char *)(&cp_occ[(smem.l) >> CP_SHIFT]), _MM_HINT_T0);
#endif


                        if((smem.s < max_intv_array[i]) && ((smem.n - smem.m + 1) >= minSeedLen))
                        {

                            if(smem.s > 0)
                            {
                                matchArray[numTotalSeed++] = smem;
                            }
#if 0
                            else
                            {
                                    fprintf(stderr, "Leaving without seed (k, l, s, m, n) = (%ld, %ld, %ld, %lu, %lu)\n", smem.k, smem.l, smem.s, smem.m, smem.n);
                            }
#endif
                            break;
                        }
                    }
                    else
                    {

                        break;
                    }
                }
#if 0
                if (j == readlength)
                {
                    fprintf(stderr, "Reached the end without seed (k, l, s, m, n) = (%ld, %ld, %ld, %lu, %lu)\n", smem.k, smem.l, smem.s, smem.m, smem.n);
                }
#endif

            }
            x = next_x;
        }
    }
    return numTotalSeed;
}


/*
int64_t FMI_search::bwtSeedStrategyAllPosOneThreadDONE(uint8_t *enc_qdb,
        int32_t *max_intv_array,
        int32_t numReads,
        const bseq1_t *seq_,
        int32_t *query_cum_len_ar,
        int32_t minSeedLen,
        SMEM *matchArray)
{
    int32_t i;

    int64_t numTotalSeed = 0;

    for(i = 0; i < numReads; i++)
    {
        int readlength = seq_[i].l_seq;
        int16_t x = 0;
        while(x < readlength)
        {
            int next_x = x + 2;

            // Forward search
            SMEM smem;
            smem.rid = i;
            smem.m = x;
            smem.n = x + 1;

            int offset = query_cum_len_ar[i];
            uint8_t a = enc_qdb[offset + x];
            // uint8_t a = enc_qdb[i * readlength + x];
            uint8_t a2 = enc_qdb[offset + x + 1];

            if(a < 4 && a2 < 4)
            {
                uint8_t first_pair = a * 4 + a2;
                uint8_t first_pair_complement = (3 - a2) * 4 + (3 - a);
                smem.k = count2[first_pair];
                smem.l = count2[first_pair_complement];
                smem.s = count2[first_pair + 1] - count2[first_pair];

                int64_t first_pair_mask = (first_pair == (3 - first_base) * 4 - 1);
                smem.s -= first_pair_mask;

                int j;
                int next_j;
                for(j = x + 2; j < readlength - 1;)
                {
                    next_x = j + 2;
                    next_j = j + 2;
                    // a = enc_qdb[i * readlength + j];
                    a = enc_qdb[offset + j];
                    a2 = enc_qdb[offset + j + 1];
                    if(a < 4 && a2 < 4)
                    {
                        SMEM smem_ = smem;

                        // Forward extension is backward extension with the BWT of reverse complement
                        smem_.k = smem.l;
                        smem_.l = smem.k;
                        SMEM newSmem_ = backwardExt2(smem_, 3 - a2, 3 - a);
                        SMEM newSmem = newSmem_;
                        newSmem.k = newSmem_.l;
                        newSmem.l = newSmem_.k;
                        newSmem.n = j + 1;
                        smem = newSmem;
#ifdef ENABLE_PREFETCH
                        _mm_prefetch((const char *)(&cp_occ[(smem.k) >> CP_SHIFT]), _MM_HINT_T0);
                        _mm_prefetch((const char *)(&cp_occ[(smem.l) >> CP_SHIFT]), _MM_HINT_T0);
#endif
                        if(smem.s < max_intv_array[i])
                        {
                            // try single base extending and add that if smem.s is small enough
                            if((smem.n - smem.m) >= minSeedLen) // minSeedLen: default == 20
                            {
                                SMEM newSmemSingle_ = backwardExt(smem_, 3 - a);
                                SMEM newSmemSingle = newSmemSingle_;
                                newSmemSingle.k = newSmemSingle_.l;
                                newSmemSingle.l = newSmemSingle_.k;
                                newSmemSingle.n = j;

                                if(newSmemSingle.s < max_intv_array[i])
                                {
                                    if(newSmemSingle.s > 0)
                                    {
                                        matchArray[numTotalSeed++] = newSmemSingle;
                                    }
                                    // update next x
                                    next_x = j + 1;
                                    break;
                                }
                            }

                            if((smem.n - smem.m + 1) >= minSeedLen)
                            {
                                if(smem.s > 0)
                                {
                                    matchArray[numTotalSeed++] = smem;
                                }
                                break;
                            }
                        }

                        j = next_j;
                    }
                    else
                    {
                        // try single base extending
                        if ((a < 4) && ((smem.n - smem.m + 1) >= (minSeedLen - 1))) // a2 = 'N'
                        {
                            SMEM smem_ = smem;
                            smem_.k = smem.l;
                            smem_.l = smem.k;
                            SMEM newSmem_ = backwardExt(smem_, 3 - a);
                            SMEM newSmem = newSmem_;
                            newSmem.k = newSmem_.l;
                            newSmem.l = newSmem_.k;
                            newSmem.n = j;
                            smem = newSmem;
                            if((smem.s < max_intv_array[i]) && smem.s > 0)
                            {
                                matchArray[numTotalSeed++] = smem;
                            }
                            break;
                        }

                        if(a2 < 4) // a2 != 'N'
                        {
                            next_x = j + 1;
                            break;
                        }

                        break;
                    }
                }
                if(j == readlength - 1)
                {
                    //next_x = j + 1;
                    a = enc_qdb[offset + j];
                    if (a < 4)
                    {
                        SMEM smem_ = smem;
                        // Forward extension is backward extension with the BWT of reverse complement
                        smem_.k = smem.l;
                        smem_.l = smem.k;
                        SMEM newSmem_ = backwardExt(smem_, 3 - a);
                        SMEM newSmem = newSmem_;
                        newSmem.k = newSmem_.l;
                        newSmem.l = newSmem_.k;
                        newSmem.n = j;
                        smem = newSmem;
                        if((smem.s < max_intv_array[i]) && ((smem.n - smem.m + 1) >= minSeedLen) && (smem.s > 0))
                        {
                            matchArray[numTotalSeed++] = smem;
                        }
#if 0
                        else
                        {
                            fprintf(stderr, "Leaving without seed (k, l, s, m, n) = (%ld, %ld, %ld, %lu, %lu)\n", smem.k, smem.l, smem.s, smem.m, smem.n);
                        }
#endif
                    }
                    break;
                }
            }
            else
            {
                if (a2 < 4)
                {
                    next_x = x + 1;
                }
            }
            x = next_x;
        }
    }
    return numTotalSeed;
}
*/


void FMI_search::getSMEMs(uint8_t *enc_qdb,
        int32_t numReads,
        int32_t batch_size,
        int32_t readlength,
        int32_t minSeedLen,
        int32_t nthreads,
        SMEM *matchArray,
        int64_t *numTotalSmem)
{
    SMEM *prevArray = (SMEM *)_mm_malloc(nthreads * readlength * sizeof(SMEM), 64);
    SMEM *currArray = (SMEM *)_mm_malloc(nthreads * readlength * sizeof(SMEM), 64);


    // #pragma omp parallel num_threads(nthreads)
    {
        int tid = 0; //omp_get_thread_num();   // removed omp
        numTotalSmem[tid] = 0;
        SMEM *myPrevArray = prevArray + tid * readlength;
        SMEM *myCurrArray = prevArray + tid * readlength;

        int32_t perThreadQuota = (numReads + (nthreads - 1)) / nthreads;
        int32_t first = tid * perThreadQuota;
        int32_t last  = (tid + 1) * perThreadQuota;
        if(last > numReads) last = numReads;
        SMEM *myMatchArray = matchArray + first * readlength;

        uint32_t i;
        // Perform SMEM for original reads
        for(i = first; i < last; i++)
        {
            int x = readlength - 1;
            int numPrev = 0;
            int numSmem = 0;

            while (x >= 0)
            {
                // Forward search
                SMEM smem;
                smem.rid = i;
                smem.m = x;
                smem.n = x;
                uint8_t a = enc_qdb[i * readlength + x];

                if(a > 3)
                {
                    x--;
                    continue;
                }
                smem.k = count[a];
                smem.l = count[3 - a];
                smem.s = count[a+1] - count[a];

                int j;
                for(j = x + 1; j < readlength; j++)
                {
                    a = enc_qdb[i * readlength + j];
                    if(a < 4)
                    {
                        SMEM smem_ = smem;

                        // Forward extension is backward extension with the BWT of reverse complement
                        smem_.k = smem.l;
                        smem_.l = smem.k;
                        SMEM newSmem_ = backwardExt(smem_, 3 - a);
                        SMEM newSmem = newSmem_;
                        newSmem.k = newSmem_.l;
                        newSmem.l = newSmem_.k;
                        newSmem.n = j;

                        if(newSmem.s != smem.s)
                        {
                            myPrevArray[numPrev] = smem;
                            numPrev++;
                        }
                        smem = newSmem;
                        if(newSmem.s == 0)
                        {
                            break;
                        }
                    }
                    else
                    {
                        myPrevArray[numPrev] = smem;
                        numPrev++;
                        break;
                    }
                }
                if(smem.s != 0)
                {
                    myPrevArray[numPrev++] = smem;
                }

                SMEM *curr, *prev;
                prev = myPrevArray;
                curr = myCurrArray;

                int p;
                for(p = 0; p < (numPrev/2); p++)
                {
                    SMEM temp = prev[p];
                    prev[p] = prev[numPrev - p - 1];
                    prev[numPrev - p - 1] = temp;
                }

                int next_x = x - 1;

                // Backward search
                int cur_j = readlength;
                for(j = x - 1; j >= 0; j--)
                {
                    int numCurr = 0;
                    int curr_s = -1;
                    a = enc_qdb[i * readlength + j];
                    //fprintf(stderr, "a = %d\n", a);
                    if(a > 3)
                    {
                        next_x = j - 1;
                        break;
                    }
                    for(p = 0; p < numPrev; p++)
                    {
                        SMEM smem = prev[p];
                        SMEM newSmem = backwardExt(smem, a);
                        newSmem.m = j;

                        if(newSmem.s == 0)
                        {
                            if((numCurr == 0) && (j < cur_j))
                            {
                                cur_j = j;
                                if((smem.n - smem.m + 1) >= minSeedLen)
                                    myMatchArray[numTotalSmem[tid] + numSmem++] = smem;
                            }
                        }
                        if((newSmem.s != 0) && (newSmem.s != curr_s))
                        {
                            curr_s = newSmem.s;
                            curr[numCurr++] = newSmem;
                        }
                    }
                    SMEM *temp = prev;
                    prev = curr;
                    curr = temp;
                    numPrev = numCurr;
                    if(numCurr == 0)
                    {
                        next_x = j;
                        break;
                    }
                    else
                    {
                        next_x = j - 1;
                    }
                }
                if(numPrev != 0)
                {
                    SMEM smem = prev[0];
                    if((smem.n - smem.m + 1) >= minSeedLen)
                        myMatchArray[numTotalSmem[tid] + numSmem++] = smem;
                    numPrev = 0;
                }
                x = next_x;
            }
            numTotalSmem[tid] += numSmem;
        }
    }

    _mm_free(prevArray);
    _mm_free(currArray);
}


int compare_smem(const void *a, const void *b)
{
    SMEM *pa = (SMEM *)a;
    SMEM *pb = (SMEM *)b;

    if(pa->rid < pb->rid)
        return -1;
    if(pa->rid > pb->rid)
        return 1;

    if(pa->m < pb->m)
        return -1;
    if(pa->m > pb->m)
        return 1;
    if(pa->n > pb->n)
        return -1;
    if(pa->n < pb->n)
        return 1;
    return 0;
}

void FMI_search::sortSMEMs(SMEM *matchArray,
        int64_t numTotalSmem[],
        int32_t numReads,
        int32_t readlength,
        int nthreads)
{
    int tid;
    int32_t perThreadQuota = (numReads + (nthreads - 1)) / nthreads;
    for(tid = 0; tid < nthreads; tid++)
    {
        int32_t first = tid * perThreadQuota;
        SMEM *myMatchArray = matchArray + first * readlength;
        qsort(myMatchArray, numTotalSmem[tid], sizeof(SMEM), compare_smem);
    }
}


/*
SMEM FMI_search::backwardExtVERBOSE(SMEM smem, uint8_t a)
{
#if 1
    if(((smem.n - smem.m + 1) % 2) == 0)
    {
        fprintf(stderr, "Entered backwardExt:\n");
        fprintf(stderr, "P = (k, l, s, m, n) = (%ld, %ld, %ld, %lu, %lu), yP, y = %d\n", smem.k, smem.l, smem.s, smem.m, smem.n, a);
    }
#endif
    //beCalls++;
    uint8_t b;

    int64_t k[4], l[4], s[4];
    for(b = 0; b < 4; b++)
    {
        int64_t sp = (int64_t)(smem.k) - 1;
        int64_t ep = (int64_t)(smem.k) + (int64_t)(smem.s) - 1;
#if 1
        //GET_OCC(pp, c, occ_id_pp, y_pp, occ_pp, one_hot_bwt_str_c_pp, match_mask_pp) 
        int64_t occ_id_sp = sp >> CP_SHIFT; 
        int64_t y_sp = sp & CP_MASK; 
        int64_t occ_sp = cp_occ[occ_id_sp].cp_count[b]; 
        uint64_t one_hot_bwt_str_c_sp = cp_occ[occ_id_sp].one_hot_bwt_str[b]; 
        uint64_t match_mask_sp = one_hot_bwt_str_c_sp & one_hot_mask_array[y_sp]; 
        occ_sp += _mm_countbits_64(match_mask_sp);
#else
        GET_OCC(sp, b, occ_id_sp, y_sp, occ_sp, one_hot_bwt_str_c_sp, match_mask_sp);
#endif
        GET_OCC(ep, b, occ_id_ep, y_ep, occ_ep, one_hot_bwt_str_c_ep, match_mask_ep);
        k[b] = count[b] + occ_sp;
        s[b] = occ_ep - occ_sp;
#if 0
        if (b == a)
        {
            fprintf(stderr, "base: %d, count[base] %lu, occ_sp %lu, occ_ep %lu\n", b, count[b], occ_sp, occ_ep);
            fprintf(stderr, "sp: %ld, occ_sp = cp_occ[occ_id_sp].cp_count[base] %ld\n", sp, occ_sp);
        }
#endif
    }

    int64_t sentinel_offset = 0;
    if((smem.k <= sentinel_index) && ((smem.k + smem.s) > sentinel_index)) sentinel_offset = 1;
    l[3] = smem.l + sentinel_offset;
    l[2] = l[3] + s[3];
    l[1] = l[2] + s[2];
    l[0] = l[1] + s[1];

    smem.k = k[a];
    smem.l = l[a];
    smem.s = s[a];
#if 0
    fprintf(stderr, "Returning from backwardExt:\n");
    fprintf(stderr, "(k, l, s, m, n) = (%ld, %ld, %ld, %lu, %lu)\n", smem.k, smem.l, smem.s, smem.m, smem.n);
#endif
    return smem;
}
*/



SMEM FMI_search::backwardExt(SMEM smem, uint8_t a)
{
    //beCalls++;
    uint8_t b;

    int64_t k[4], l[4], s[4];
    for(b = 0; b < 4; b++)
    {
        int64_t sp = (int64_t)(smem.k) - 1;
        int64_t ep = (int64_t)(smem.k) + (int64_t)(smem.s) - 1;
#if 1
        //GET_OCC(pp, c, occ_id_pp, y_pp, occ_pp, one_hot_bwt_str_c_pp, match_mask_pp) 
        int64_t occ_id_sp = sp >> CP_SHIFT; 
        int64_t y_sp = sp & CP_MASK; 
        int64_t occ_sp = cp_occ[occ_id_sp].cp_count[b]; 
        uint64_t one_hot_bwt_str_c_sp = cp_occ[occ_id_sp].one_hot_bwt_str[b]; 
        uint64_t match_mask_sp = one_hot_bwt_str_c_sp & one_hot_mask_array[y_sp]; 
        occ_sp += _mm_countbits_64(match_mask_sp);
#else
        GET_OCC(sp, b, occ_id_sp, y_sp, occ_sp, one_hot_bwt_str_c_sp, match_mask_sp);
#endif
        GET_OCC(ep, b, occ_id_ep, y_ep, occ_ep, one_hot_bwt_str_c_ep, match_mask_ep);
        k[b] = count[b] + occ_sp;
        s[b] = occ_ep - occ_sp;
#if 0
        if (b == a)
        {
            fprintf(stderr, "base: %d, count[base] %lu, occ_sp %lu, occ_ep %lu\n", b, count[b], occ_sp, occ_ep);
            fprintf(stderr, "sp: %ld, occ_sp = cp_occ[occ_id_sp].cp_count[base] %ld\n", sp, occ_sp);
        }
#endif
    }

    int64_t sentinel_offset = 0;
    if((smem.k <= sentinel_index) && ((smem.k + smem.s) > sentinel_index)) sentinel_offset = 1;
    l[3] = smem.l + sentinel_offset;
    l[2] = l[3] + s[3];
    l[1] = l[2] + s[2];
    l[0] = l[1] + s[1];

    smem.k = k[a];
    smem.l = l[a];
    smem.s = s[a];
    return smem;
}

/*
// P -> xyP
SMEM FMI_search::backwardExt2(SMEM smem, uint8_t x, uint8_t y)
{
#if 0
    fprintf(stderr, "Entered backwardExt2:\n");
    fprintf(stderr, "P = (k, l, s, m, n) = (%ld, %ld, %ld, %lu, %lu), xyP, x = %d, y = %d\n", smem.k, smem.l, smem.s, smem.m, smem.n, x, y);
#endif
    //beCalls++;
    uint8_t b;
    uint8_t basePair = x * 4 + y;

    int64_t k[16], l[16], s[16];
    for(b = 0; b < 16; b++)
    {
        int64_t sp = (int64_t)(smem.k) - 1;
        int64_t ep = (int64_t)(smem.k) + (int64_t)(smem.s) - 1;
        GET_OCC2(sp, b, occ_id_sp, y_sp, occ_sp, one_hot_bwt_str_c_sp, match_mask_sp);
        GET_OCC2(ep, b, occ_id_ep, y_ep, occ_ep, one_hot_bwt_str_c_ep, match_mask_ep);
        k[b] = count2[b] + occ_sp;
        s[b] = occ_ep - occ_sp;
#if 0
        fprintf(stderr, "* k[%2d] = %ld\n", b, k[b]);
        fprintf(stderr, "* s[%2d] = %ld\n", b, s[b]);
#endif
    }

    int64_t sentinel_offset = 0;
    if((smem.k <= sentinel_index) && ((smem.k + smem.s) > sentinel_index)) 
    {
        sentinel_offset = 1;
    }
    int64_t sentinel_offset2 = 0;
    SMEM check2 = backwardExt(smem, first_base);
    if ((check2.k <= sentinel_index) && ((check2.k + check2.s) > sentinel_index))
    {
        sentinel_offset2 = 1;
    }
    //fprintf(stderr, "sentinel_index = %ld, sentinel_offset = %ld, sentinel_offset2 = %ld\n", sentinel_index, sentinel_offset, sentinel_offset2);

#define AA_ 0
#define AC_ 1
#define AG_ 2
#define AT_ 3
#define CA_ 4
#define CC_ 5
#define CG_ 6
#define CT_ 7
#define GA_ 8
#define GC_ 9
#define GG_ 10
#define GT_ 11
#define TA_ 12
#define TC_ 13
#define TG_ 14
#define TT_ 15
    l[TT_] = smem.l + sentinel_offset;
    l[GT_] = l[TT_] + s[TT_];
    l[CT_] = l[GT_] + s[GT_];
    l[AT_] = l[CT_] + s[CT_];
    l[TG_] = l[AT_] + s[AT_];
    l[GG_] = l[TG_] + s[TG_];
    l[CG_] = l[GG_] + s[GG_];
    l[AG_] = l[CG_] + s[CG_];
    l[TC_] = l[AG_] + s[AG_];
    l[GC_] = l[TC_] + s[TC_];
    l[CC_] = l[GC_] + s[GC_];
    l[AC_] = l[CC_] + s[CC_];
    l[TA_] = l[AC_] + s[AC_];
    l[GA_] = l[TA_] + s[TA_];
    l[CA_] = l[GA_] + s[GA_];
    l[AA_] = l[CA_] + s[CA_];


    for (int jjj = first_base; jjj >= 0; jjj--)
    {
        for (int iii = 0; iii < 4; iii++)
        {
            b = iii * 4 + jjj;
            l[b] += sentinel_offset2;
        }
    }

    smem.k = k[basePair];
    smem.l = l[basePair];
    smem.s = s[basePair];
#if 0
    fprintf(stderr, "Returning from backwardExt2:\n");
    fprintf(stderr, "(k, l, s, m, n) = (%ld, %ld, %ld, %lu, %lu)\n", smem.k, smem.l, smem.s, smem.m, smem.n);
#endif
    return smem;
}
*/

int64_t FMI_search::get_sa_entry(int64_t pos)
{
    int64_t sa_entry = sa_ms_byte[pos];
    sa_entry = sa_entry << 32;
    sa_entry = sa_entry + sa_ls_word[pos];
    return sa_entry;
}

void FMI_search::get_sa_entries(int64_t *posArray, int64_t *coordArray, uint32_t count, int32_t nthreads)
{
    uint32_t i;
    // #pragma omp parallel for num_threads(nthreads)
    for(i = 0; i < count; i++)
    {
        int64_t pos = posArray[i];
        int64_t sa_entry = sa_ms_byte[pos];
        sa_entry = sa_entry << 32;
        sa_entry = sa_entry + sa_ls_word[pos];
        //_mm_prefetch((const char *)(sa_ms_byte + pos + SAL_PFD), _MM_HINT_T0);
        coordArray[i] = sa_entry;
    }
}

void FMI_search::get_sa_entries(SMEM *smemArray, int64_t *coordArray, int32_t *coordCountArray, uint32_t count, int32_t max_occ)
{
    uint32_t i;
    int32_t totalCoordCount = 0;
    for(i = 0; i < count; i++)
    {
        int32_t c = 0;
        SMEM smem = smemArray[i];
        int64_t hi = smem.k + smem.s;
        int64_t step = (smem.s > max_occ) ? smem.s / max_occ : 1;
        int64_t j;
        for(j = smem.k; (j < hi) && (c < max_occ); j+=step, c++)
        {
            int64_t pos = j;
            int64_t sa_entry = sa_ms_byte[pos];
            sa_entry = sa_entry << 32;
            sa_entry = sa_entry + sa_ls_word[pos];
            //_mm_prefetch((const char *)(sa_ms_byte + pos + SAL_PFD * step), _MM_HINT_T0);
            coordArray[totalCoordCount + c] = sa_entry;
        }
        coordCountArray[i] = c;
        totalCoordCount += c;
    }
}

#if 0
// sa_compression
int64_t FMI_search::get_sa_entry_compressed(int64_t pos, int tid)
{
    if ((pos & SA_COMPX_MASK) == 0) {

#if  SA_COMPRESSION
        int64_t sa_entry = sa_ms_byte[pos >> SA_COMPX];
#else
        int64_t sa_entry = sa_ms_byte[pos];     // simulation
#endif

        sa_entry = sa_entry << 32;

#if  SA_COMPRESSION
        sa_entry = sa_entry + sa_ls_word[pos >> SA_COMPX];
#else
        sa_entry = sa_entry + sa_ls_word[pos];   // simulation
#endif

        return sa_entry;        
    }
    else {
        // tprof[MEM_CHAIN][tid] ++;
        int64_t offset = 0; 
        int64_t sp = pos;
        while(true)
        {
            int64_t occ_id_pp_ = sp >> CP_SHIFT;
            int64_t y_pp_ = CP_BLOCK_SIZE - (sp & CP_MASK) - 1; 
            uint64_t *one_hot_bwt_str = cp_occ[occ_id_pp_].one_hot_bwt_str;
            uint8_t b;

            if((one_hot_bwt_str[0] >> y_pp_) & 1)
                b = 0;
            else if((one_hot_bwt_str[1] >> y_pp_) & 1)
                b = 1;
            else if((one_hot_bwt_str[2] >> y_pp_) & 1)
                b = 2;
            else if((one_hot_bwt_str[3] >> y_pp_) & 1)
                b = 3;
            else
                b = 4;

            if (b == 4) {
                return offset;
            }

            GET_OCC(sp, b, occ_id_sp, y_sp, occ_sp, one_hot_bwt_str_c_sp, match_mask_sp);

            sp = count[b] + occ_sp;

            offset ++;
            // tprof[ALIGN1][tid] ++;
            if ((sp & SA_COMPX_MASK) == 0) break;
        }
        // assert((reference_seq_len >> SA_COMPX) - 1 >= (sp >> SA_COMPX));
#if  SA_COMPRESSION
        int64_t sa_entry = sa_ms_byte[sp >> SA_COMPX];
#else
        int64_t sa_entry = sa_ms_byte[sp];      // simultion
#endif

        sa_entry = sa_entry << 32;

#if  SA_COMPRESSION
        sa_entry = sa_entry + sa_ls_word[sp >> SA_COMPX];
#else
        sa_entry = sa_entry + sa_ls_word[sp];      // simulation
#endif

        sa_entry += offset;
        return sa_entry;
    }
}

void FMI_search::get_sa_entries(SMEM *smemArray, int64_t *coordArray, int32_t *coordCountArray, uint32_t count, int32_t max_occ, int tid)
{

    uint32_t i;
    int32_t totalCoordCount = 0;
    for(i = 0; i < count; i++)
    {
        int32_t c = 0;
        SMEM smem = smemArray[i];
        int64_t hi = smem.k + smem.s;
        int64_t step = (smem.s > max_occ) ? smem.s / max_occ : 1;
        int64_t j;
        for(j = smem.k; (j < hi) && (c < max_occ); j+=step, c++)
        {
            int64_t pos = j;
            int64_t sa_entry = get_sa_entry_compressed(pos, tid);
            coordArray[totalCoordCount + c] = sa_entry;
        }
        // coordCountArray[i] = c;
        *coordCountArray += c;
        totalCoordCount += c;
    }
}

// SA_COPMRESSION w/ PREFETCH
int64_t FMI_search::call_one_step(int64_t pos, int64_t &sa_entry, int64_t &offset)
{
    if ((pos & SA_COMPX_MASK) == 0) {        
        sa_entry = sa_ms_byte[pos >> SA_COMPX];        
        sa_entry = sa_entry << 32;        
        sa_entry = sa_entry + sa_ls_word[pos >> SA_COMPX];        
        // return sa_entry;
        return 1;
    }
    else {
        // int64_t offset = 0; 
        int64_t sp = pos;

        int64_t occ_id_pp_ = sp >> CP_SHIFT;
        int64_t y_pp_ = CP_BLOCK_SIZE - (sp & CP_MASK) - 1; 
        uint64_t *one_hot_bwt_str = cp_occ[occ_id_pp_].one_hot_bwt_str;
        uint8_t b;

        if((one_hot_bwt_str[0] >> y_pp_) & 1)
            b = 0;
        else if((one_hot_bwt_str[1] >> y_pp_) & 1)
            b = 1;
        else if((one_hot_bwt_str[2] >> y_pp_) & 1)
            b = 2;
        else if((one_hot_bwt_str[3] >> y_pp_) & 1)
            b = 3;
        else
            b = 4;
        if (b == 4) {
            sa_entry = 0;
            return 1;
        }

        GET_OCC(sp, b, occ_id_sp, y_sp, occ_sp, one_hot_bwt_str_c_sp, match_mask_sp);

        sp = count[b] + occ_sp;

        offset ++;
        if ((sp & SA_COMPX_MASK) == 0) {

            sa_entry = sa_ms_byte[sp >> SA_COMPX];        
            sa_entry = sa_entry << 32;
            sa_entry = sa_entry + sa_ls_word[sp >> SA_COMPX];

            sa_entry += offset;
            // return sa_entry;
            return 1;
        }
        else {
            sa_entry = sp;
            return 0;
        }
    } // else
}

void FMI_search::get_sa_entries_prefetch(SMEM *smemArray, int64_t *coordArray,
        int64_t *coordCountArray, int64_t count,
        const int32_t max_occ, int tid, int64_t &id_)
{

    // uint32_t i;
    int32_t totalCoordCount = 0;
    int32_t mem_lim = 0, id = 0;

    for(int i = 0; i < count; i++)
    {
        int32_t c = 0;
        SMEM smem = smemArray[i];
        mem_lim += smem.s;
    }

    int64_t *pos_ar = (int64_t *) _mm_malloc( mem_lim * sizeof(int64_t), 64);
    int64_t *map_ar = (int64_t *) _mm_malloc( mem_lim * sizeof(int64_t), 64);

    for(int i = 0; i < count; i++)
    {
        int32_t c = 0;
        SMEM smem = smemArray[i];
        int64_t hi = smem.k + smem.s;
        int64_t step = (smem.s > max_occ) ? smem.s / max_occ : 1;
        int64_t j;
        for(j = smem.k; (j < hi) && (c < max_occ); j+=step, c++)
        {
            int64_t pos = j;
            pos_ar[id]  = pos;
            map_ar[id++] = totalCoordCount + c;
            // int64_t sa_entry = get_sa_entry_compressed(pos, tid);
            // coordArray[totalCoordCount + c] = sa_entry;
        }
        //coordCountArray[i] = c;
        *coordCountArray += c;
        totalCoordCount += c;
    }

    id_ += id;

    const int32_t sa_batch_size = 20;
    int64_t working_set[sa_batch_size], map_pos[sa_batch_size];;
    int64_t offset[sa_batch_size] = {-1};

    int i = 0, j = 0;    
    while(i<id && j<sa_batch_size)
    {
        int64_t pos =  pos_ar[i];
        working_set[j] = pos;
        map_pos[j] = map_ar[i];
        offset[j] = 0;

        if (pos & SA_COMPX_MASK == 0) {
            _mm_prefetch(&sa_ms_byte[pos >> SA_COMPX], _MM_HINT_T0);
            _mm_prefetch(&sa_ls_word[pos >> SA_COMPX], _MM_HINT_T0);
        }
        else {
            int64_t occ_id_pp_ = pos >> CP_SHIFT;
            _mm_prefetch(&cp_occ[occ_id_pp_], _MM_HINT_T0);
        }
        i++;
        j++;
    }

    int lim = j, all_quit = 0;
    while (all_quit < id)
    {

        for (int k=0; k<lim; k++)
        {
            int64_t sp = 0, pos = 0;
            bool quit;
            if (offset[k] >= 0) {
                quit = call_one_step(working_set[k], sp, offset[k]);
            }
            else
                continue;

            if (quit) {
                coordArray[map_pos[k]] = sp;
                all_quit ++;

                if (i < id)
                {
                    pos = pos_ar[i];
                    working_set[k] = pos;
                    map_pos[k] = map_ar[i++];
                    offset[k] = 0;

                    if (pos & SA_COMPX_MASK == 0) {
                        _mm_prefetch(&sa_ms_byte[pos >> SA_COMPX], _MM_HINT_T0);
                        _mm_prefetch(&sa_ls_word[pos >> SA_COMPX], _MM_HINT_T0);
                    }
                    else {
                        int64_t occ_id_pp_ = pos >> CP_SHIFT;
                        _mm_prefetch(&cp_occ[occ_id_pp_], _MM_HINT_T0);
                    }
                }
                else
                    offset[k] = -1;
            }
            else {
                working_set[k] = sp;
                if (sp & SA_COMPX_MASK == 0) {
                    _mm_prefetch(&sa_ms_byte[sp >> SA_COMPX], _MM_HINT_T0);
                    _mm_prefetch(&sa_ls_word[sp >> SA_COMPX], _MM_HINT_T0);
                }
                else {
                    int64_t occ_id_pp_ = sp >> CP_SHIFT;
                    _mm_prefetch(&cp_occ[occ_id_pp_], _MM_HINT_T0);
                }                
            }
        }
    }

    _mm_free(pos_ar);
    _mm_free(map_ar);
}
#endif
