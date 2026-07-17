/*
* Zulal Bingol
* Helper functions for GateKeeper-GPU 
* v0
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../include/common.h"

void print_uint_array(unsigned int *array, int size)
{
	int i;
	for (i = 0; i < size; i++)
		printf("%x-", array[i]);
	printf("\n");
}

// Helper function for printing str as separated windows
void print_str_windows(char* str, int window_size, size_t str_size)
{
	int tracer = 0;
	while (tracer != str_size){
		printf("%c", str[tracer]);
		tracer++;
		if (tracer % window_size == 0)
			printf("-");
	}
	printf("\n");
}

void print_binary_uint(unsigned int *array, int size)
{
	int i, j;
	unsigned int temp;
	unsigned int sh = 1;
	sh <<= 32-1;

	for (i = 0; i < size; i++){
		temp = array[i];
		j = 0;
		while (j < 32) {
			if (temp & sh)
				printf("1");
			else
				printf("0");
			temp <<= 1;
			j++;
			// if (j == 16)
			// 	printf(".");
		}
		// printf("-");
	}
	printf("\n");
}

void slice_2D_array(unsigned int **origin, int startInd,
		    int endInd, int width, unsigned int *newArray)
{
	int i, j;
	int newInd = 0;
	// 2D-to-2D slicing (unsigned int **newArray)
	/* for (i = startInd; i <= endInd; i++){ */
	/*   for (j = 0; j < width; j++){ */
	/*     newArray[newInd][j] = origin[i][j]; */
	/*   } */
	/*   newInd++; */
	/* } */

	// 2D-to-1D slicing (slice + flatten the array)
	for (i = startInd; i <= endInd; i++){
		for (j = 0; j < width; j++){
			newArray[width * newInd + j] = origin[i][j];
		}
		newInd++;
	}
}

double findLongest(double *array, int size)
{
	double longest = array[0];
	if (size > 1){
	int i;
	for (i = 1; i < size; i++){
		if (array[i] > longest)
			longest = array[i];
		}
	}
	return longest;
}
