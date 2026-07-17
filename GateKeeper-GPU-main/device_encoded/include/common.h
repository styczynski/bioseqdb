#ifndef __COMMON
#define __COMMON

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define STR_PARTITION_SIZE 16 // substr size, since each char is encoded 2-bit, one window = 32bits(sizeof int)
#define SIZE_OF_UINT 32

#define ABORT_MISSION -1
#define SUCCESSFUL_MISSION 1
#define DEV_SKIP -2
#define DEBUG 1

#define EOF_ -1
#define PARTIAL_BATCH 2

#define PASS 1
#define REJECT 0
#define REJECT_EDIT -1
#define UNDEFINED_STR_EDIT -2

#define SHIFTER_WINDOW_SIZE 4
#define UNDEFINED_CHAR 4
#define UNDEFINED_STR 'u'
#define DEFINED_STR 'd'

#define ON 1
#define OFF 0

enum exe_modes{SINGLE, MULTIPLE};
enum exe_modes exe_mode;
  
int debug_mode;
int kernel_analysis_mode;
int compute_cap_major;

typedef struct results_node {
	short *filt_result;
	short *edits;
	int reader_status;
	long int batch_size;
	int loop_count; // only head
	struct results_node *next;
}RES_NODE;

void print_uint_array(unsigned int *array, int size);
void print_str_windows(char* str, int window_size, size_t str_size);
void print_binary_uint(unsigned int *array, int size);
void slice_2D_array(unsigned int **origin, int startInd, int endInd,
		    		int width, unsigned int *newArray);
double findLongest(double *array, int size);

#endif
