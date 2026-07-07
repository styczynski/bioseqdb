/*
 * Aligner.h
 *
 *  Created on: Dec 29, 2011
 *      Author: yongchao
 *      This code is taken from CUSHAW2 (http://cushaw2.sourceforge.net)

 SWIPE
 Smith-Waterman database searches with Inter-sequence Parallel Execution

 Copyright (C) 2008-2012 Torbjorn Rognes, University of Oslo,
 Oslo University Hospital and Sencel Bioinformatics AS

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU Affero General Public License as
 published by the Free Software Foundation, either version 3 of the
 License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU Affero General Public License for more details.

 You should have received a copy of the GNU Affero General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.

 Contact: Torbjorn Rognes <torognes@ifi.uio.no>,
 Department of Informatics, University of Oslo,
 PO Box 1080 Blindern, NO-0316 Oslo, Norway
 */

#ifndef ALIGNER_H_
#define ALIGNER_H_
#include "Defs.h"

class Aligner
{
public:
	Aligner(int matrix[32][32], int gopen, int gext, int maxQuerySize, int scoreLimit7);
	~Aligner();

	/*compute the optimal local alignment score using SSE*/
	/*using 8x16*/
	void lalignScoreQuad(uint8_t* query, int32_t qlen, uint8_t* sequences,
			int32_t* seqOffsets, int32_t numSeqs, SeqEntry* scores);

	/*using 16x8*/
	void lalignScoreDual(uint8_t* query, int32_t qlen, uint8_t* sequences,
			int32_t* seqOffsets, int32_t numSeqs, SeqEntry* scores);

	/*score matrix for SSE2*/
	int8_t* _score_matrix_7;
	int16_t* _score_matrix_16;
	int32_t _gopen;
	int32_t _gext;
	int32_t _goe;
	int32_t _scoreLimit7;
	BYTE* _dprofile;
	BYTE** _qtable;
	BYTE* _hearray;
	int32_t _maxQuerySize;
	bool _haveSSSE3;

	/*kernel function*/
	int32_t search7(BYTE** qtable, BYTE gap_open_penalty,
			BYTE gap_extend_penalty, BYTE * score_matrix, BYTE * dprofile,
			BYTE * hearray, int32_t qlen, int32_t numSeqs, int32_t* seqOffsets,
			uint8_t* sequences, SeqEntry* scores);

	void search7_overflows(WORD** qtable, WORD gap_open_penalty, WORD gap_extend_penalty,
			WORD* score_matrix, WORD* dprofile, WORD* hearray, int32_t qlen,
			int32_t numSeqs, int32_t *seqOffsets, uint8_t *sequences,
			SeqEntry* scores);

	void search16(WORD** qtable, WORD gap_open_penalty, WORD gap_extend_penalty,
			WORD* score_matrix, WORD* dprofile, WORD* hearray, int32_t qlen,
			int32_t numSeqs, int32_t *seqOffsets, uint8_t *sequences,
			SeqEntry* scores);

};
#endif /* Aligner_H_ */
