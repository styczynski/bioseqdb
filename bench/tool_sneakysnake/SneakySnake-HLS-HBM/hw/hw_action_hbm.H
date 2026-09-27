#ifndef HW_ACTION_HBM_H
#define HW_ACTION_HBM_H

/*
 * Copyright 2017 International Business Machines
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdint.h>
#include <string.h>
#include <ap_int.h>

#include "hls_snap_1024.H"
#include <action_memcopy.h> /* Memcopy Job definition */
// we read a line of 100 char each of 2 bits therefor the lenght is 200
#define READ_LENGTH 100
#define LENGTH 200
#define MANY_ACC 1
#define TOTAL_READS 30000
#define MAX_NB_OF_BYTES_READ	(TOTAL_READS*256*2/8)
#define LCL_MEM_MAX_SIZE	(256 * 1024 * 1024)  // HBM is 256MB each
#define MAX_NB_OF_WORDS_READ_256    (MAX_NB_OF_BYTES_READ/BPERDW_256)
#define MAX_NB_OF_WORDS_READ_512    (MAX_NB_OF_BYTES_READ/BPERDW_512)
#define MAX_NB_OF_WORDS_READ_1024	(MAX_NB_OF_BYTES_READ/BPERDW_1024)
#define HBM_MAX_NB_OF_LINES_READ 2* TOTAL_READS //since each read is 100*2 and we pad to 256 bits
#define  CAPI_NB_OF_LINES_READ 2*TOTAL_READS*256/512
#define ELEMENTS_PER_CACHELINE 512/8
#define TOTAL_OUT_CACHELINE TOTAL_READS/ELEMENTS_PER_CACHELINE
#define OUTPUT_ELEMENTS TOTAL_READS/MANY_ACC
//---------------------------------------------------------------------
typedef ap_uint<2>  d_in_type; 
typedef ap_uint<2>  d_out_type;
typedef ap_uint<2>  d_final_out_type; 

typedef ap_uint<LENGTH>  d_bit_in_type; 
typedef ap_uint<LENGTH/2>  d_bit_out_type; 
typedef ap_uint<8>  d_bit_out_tiny_type; 
typedef struct  {
			d_final_out_type global_count[TOTAL_READS/MANY_ACC];
			
		} g_count_struct;


typedef struct {
	CONTROL Control;	/*  16 bytes */
	memcopy_job_t Data;	/* up to 108 bytes */
	uint8_t padding[SNAP_HLS_JOBSIZE - sizeof(memcopy_job_t)];
} action_reg;

// typedef struct  {
// 			   mat_elmt_t  ccol[ROWS] [COLUMNS][DEPTH];
// 			   mat_elmt_t  dcol[ROWS][COLUMNS][DEPTH];
// 			// float  trans_upos[COLUMNS][DEPTH];

// 		} out_struct;

#endif  /* __ACTION_HLS_HBM_MEMCOPY_H__ */
