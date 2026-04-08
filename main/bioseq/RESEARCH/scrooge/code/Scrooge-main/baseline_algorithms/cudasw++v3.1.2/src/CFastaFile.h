/***********************************************
 * # Copyright 2009. Liu Yongchao
 * # Contact: Liu Yongchao
 * #          liuy0039@ntu.edu.sg; nkcslyc@hotmail.com
 * #
 * # GPL 2.0 applies.
 * #
 * ************************************************/

#ifndef _CFASTA_FILE_H_
#define _CFASTA_FILE_H_
#include <stdio.h>
#include "CParams.h"

#define MAX_NAME_LENGTH				1023
#define MAX_SEQ_LENGTH				(256 * 1024)

class CFastaFile
{
public:
	CFastaFile();
	~CFastaFile();

	int open(char *file);
	void close();
	unsigned char* getSeqName();
	unsigned char * nextSeq(int *length, int* alignedLength, int pad);

private:
	static const char amino_acids[MAX_AMINO_ACIDS];
	static const int amino_acids_trans[256];

	unsigned char name[MAX_NAME_LENGTH + 1];
	unsigned char buf[MAX_SEQ_LENGTH + 1];
	int pos;

	FILE* file;

};
#endif

