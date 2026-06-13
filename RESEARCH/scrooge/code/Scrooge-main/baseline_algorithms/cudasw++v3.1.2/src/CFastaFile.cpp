/***********************************************
 * # Copyright 2009. Liu Yongchao
 * # Contact: Liu Yongchao
 * #          liuy0039@ntu.edu.sg; nkcslyc@hotmail.com
 * #
 * # GPL 2.0 applies.
 * #
 * ************************************************/

#include "CFastaFile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char CFastaFile::amino_acids[MAX_AMINO_ACIDS] = { 'A', 'B', 'C', 'D', 'E',
		'F', 'G', 'H', 'I', 'K', 'L', 'M', 'N', 'P', 'Q', 'R', 'S', 'T', 'V',
		'W', 'X', 'Y', 'Z' };

const int CFastaFile::amino_acids_trans[256] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 10, 11, 12, 13, 0,
		14, 15, 16, 17, 18, 0, 19, 20, 21, 22, 23, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4,
		5, 6, 7, 8, 9, 0, 10, 11, 12, 13, 0, 14, 15, 16, 17, 18, 0, 19, 20, 21,
		22, 23, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
CFastaFile::CFastaFile() {
	file = 0;
	pos = 0;
}
CFastaFile::~CFastaFile() {
	if (file) {
		fclose(file);
	}
}
int CFastaFile::open(char *fileName) {
	file = fopen(fileName, "rb");
	if (!file) {
		fprintf(stderr, "Opening file (%s) failed\n", fileName);
		goto out;
	}

	pos = 0;
	fseek(file, 0, SEEK_SET);
	return 1;
	out: return 0;
}
void CFastaFile::close() {
	if (file) {
		fclose(file);
	}
	file = 0;
}
unsigned char* CFastaFile::getSeqName() {
	return name;
}
unsigned char * CFastaFile::nextSeq(int *length, int* alignedLength, int pad) {
	int i, ch;
	unsigned char *p;

	//get the first meaningful line
	while (1) {
		if (feof(file)) {
			goto err;
		}
		if (!fgets((char*) buf, MAX_SEQ_LENGTH, file)) {
			goto err;
		}
		if (buf[0] != '\r' && buf[0] != '\n') {
			break;
		}
	}

	//check the sequence name mark '>'
	if (buf[0] != '>') {
		fprintf(stderr, "invalid fasta file format (line: %d)\n", __LINE__);
		goto err;
	}
	//read the sequence name
	p = buf;
	//remove the newline at the end of the buffer
	for (; *p && (*p != '\r' && *p != '\n'); p++)
		;
	*p = '\0';

	if (p == buf) {
		fprintf(stderr, "p == buf\n");
		goto err;
	}
	//copy the sequence name;
	strncpy((char*) name, (const char*) buf + 1, MAX_NAME_LENGTH);
	name[MAX_NAME_LENGTH] = '\0';

	//read the sequence
	pos = 0;
	while (1) {
		ch = fgetc(file);
		//check whether it is the end of file
		if (ch == EOF) {
			if (pos == 0) {
				fprintf(stderr, "invalid fasta file format (line: %d)\n",
						__LINE__);
				goto err;
			} else {
				//fprintf(stderr, "reaching the end of file\n");
				goto finish;
			}
		}
		//check whether it is the end of the sequence
		if (ch == '>') {
			if (pos == 0) {
				fprintf(stderr, "invalid fasta file format (line: %d)\n",
						__LINE__);
				goto err;
			} else {
				ungetc(ch, file);
				goto finish;
			}
		} else if (ch == '\n' || ch == '\r') {
			continue;
		}
		//
		ungetc(ch, file);
		//read a line of symbols
		if (!fgets(((char*) buf) + pos, MAX_SEQ_LENGTH, file)) {
			fprintf(stderr, "it is impossible\n");
			goto err;
		}
		//remove the newline
		p = buf + pos;
		for (; *p && (*p != '\r' && *p != '\n'); p++) {
			ch = amino_acids_trans[*p];
			if (ch == 0) {
				//fprintf(stderr, "invalid symbol: %c\n", *p);
				ch = DUMMY_AMINO_ACID;
			}
			*p = ch;
			pos++;
		}
	}

	finish: *length = pos;
	*alignedLength = pos;
	if (pad > 1) {
		int aligned = (pos + pad - 1) / pad;
		aligned *= pad;
		for (i = *length; i < aligned; i++) {
			buf[i] = DUMMY_AMINO_ACID;
		}
		*alignedLength = aligned;
	}
	return buf;

	err: *length = 0;
	*alignedLength = 0;
	return 0;
}
