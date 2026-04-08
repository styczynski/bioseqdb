/*
 * Defs.h
 *
 *  Created on: Nov 22, 2012
 *      Author: yongchao
 */

#ifndef DEFS_H_
#define DEFS_H_
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <stdint.h>
#include <vector>
#include <omp.h>
#include <pthread.h>
using namespace std;

struct DatabaseHash
{
	int cx;
	int cy;
	int length;
	int alignedLen;
};

template<class T> class Database
{
public:
	Database(T* arg_array, int arg_width, int arg_height) {
		array = arg_array;
		width = arg_width;
		height = arg_height;
	}
	int width;
	int height;
	T* array;
};

struct SeqEntry
{
	int idx;
	int value;
};

/*for SSE2 implementations*/
typedef unsigned int UINT32;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef BYTE VECTOR[16];

#define DEFAULT_GAPO					10
#define DEFAULT_GAPE					2
#define DEFAULT_MIN_SCORE			100
#define DEFAULT_TOPSCORE_NUM	10
#define MAX_PATH_LENGTH				4095

#define MAX_AMINO_ACIDS     	23
#define DUMMY_AMINO_ACID   		(MAX_AMINO_ACIDS + 1)
#define MATRIX_SIZE						(MAX_AMINO_ACIDS + 2)

/*threshold for GPU and CPU computing*/
#ifndef MAX_SEQ_LENGTH_THRESHOLD
#define MAX_SEQ_LENGTH_THRESHOLD    	3072
#endif

/*the value that subject sequences must be aligned to*/
#define DB_SEQ_LENGTH_ALIGNED					4

/*the value that query sequences must be aligned to*/
#define QUERY_SEQ_LENGTH_ALIGNED				8

/*packed dummy*/
#define DUMMY_QUAD_AMINO_ACIDS		((DUMMY_AMINO_ACID << 24) | (DUMMY_AMINO_ACID << 16) | (DUMMY_AMINO_ACID << 8) | DUMMY_AMINO_ACID)

#define DB_MAX_DIVERGENCE				1

#endif /* DEFS_H_ */
