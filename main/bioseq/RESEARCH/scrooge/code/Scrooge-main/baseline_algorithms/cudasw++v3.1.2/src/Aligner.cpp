/*
 * Aligner.cpp
 *
 *  Created on: Dec 29, 2011
 *      Author: yongchao
 This code is taken from CUSHAW2 (http://cushaw2.sourceforge.net)

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

#include "Aligner.h"
#include "AlignerInlines.h"

//#define ALIGN_DEBUG
#define cpuid(f,a,b,c,d) asm("cpuid": "=a" (a), "=b" (b), "=c" (c), "=d" (d) : "a" (f));
Aligner::Aligner(int matrix[32][32], int gopen, int gext, int maxQuerySize, int scoreLimit7) {
	/*check the SSSE3 and SSE2*/
	unsigned int a __attribute__ ((unused));
	unsigned int b __attribute__ ((unused));
	unsigned int c, d;

	cpuid(1, a, b, c, d);
	//  printf("cpuid: %08x %08x %08x %08x\n", a, b, c, d);

	/*check sse2*/
	if (!((d >> 26) & 1)) {
		fprintf(stderr, "!!!!Requiring a CPU with SSE2 support!!!!\n");
		exit(-1);
	}
	/*check SSSE3*/
	if ((c >> 9) & 1) {
		_haveSSSE3 = true;
	} else {
		_haveSSSE3 = false;
		fprintf(stderr,
				"!!!!DO NOT have SSSE3 support on the CPU, resulting in lower speed\n");
	}

	_maxQuerySize = maxQuerySize;
	_gopen = gopen;
	_gext = gext;
	_goe = _gopen + _gext;
	_scoreLimit7 = scoreLimit7;

	/*initialize parameters*/
	_score_matrix_7 = new int8_t[32 * 32];
	if (_score_matrix_7 == NULL) {
		fprintf(stderr, "Memory allocation failed in function %s line %d\n",
				__FUNCTION__, __LINE__);
		exit(-1);
	}
	_score_matrix_16 = new int16_t[32 * 32];
	if (_score_matrix_16 == NULL) {
		fprintf(stderr, "Memory allocation failed in function %s line %d\n",
				__FUNCTION__, __LINE__);
		exit(-1);
	}
	/*initialize the matrix with _smat*/
	for (int i = 0; i < 32; ++i) {
		for (int j = 0; j < 32; ++j) {
			int score = matrix[i][j];
			_score_matrix_7[(i << 5) + j] = score;
			_score_matrix_16[(i << 5) + j] = score;
		}
	}

	_dprofile = new uint8_t[4 * 16 * 32];
	if (_dprofile == NULL) {
		fprintf(stderr, "Memory allocation failed in function %s line %d\n",
				__FUNCTION__, __LINE__);
		exit(-1);
	}

	_qtable = new BYTE*[_maxQuerySize];
	if (_qtable == NULL) {
		fprintf(stderr, "Memory allocation failed in function %s line %d\n",
				__FUNCTION__, __LINE__);
		exit(-1);
	}
	_hearray = new BYTE[_maxQuerySize * 32];
	if (_hearray == NULL) {
		fprintf(stderr, "Memory allocation failed in function %s line %d\n",
				__FUNCTION__, __LINE__);
		exit(-1);
	}
}

Aligner::~Aligner() {
	delete[] _qtable;
	delete[] _dprofile;
	delete[] _score_matrix_16;
}
void Aligner::lalignScoreQuad(uint8_t* query, int qlen, uint8_t* sequences,
		int32_t* seqOffsets, int numSeqs, SeqEntry* scores) {
	if (qlen > _maxQuerySize) {
		_maxQuerySize = qlen * 2;
		if (_qtable) {
			delete[] _qtable;
		}
		_qtable = new BYTE*[_maxQuerySize];
		if (_qtable == NULL) {
			fprintf(stderr, "Memory allocation failed in function %s line %d\n",
					__FUNCTION__, __LINE__);
			exit(-1);
		}
		if (_hearray) {
			delete[] _hearray;
		}
		_hearray = new BYTE[_maxQuerySize * 32];
		if (_hearray == NULL) {
			fprintf(stderr, "Memory allocation failed in function %s line %d\n",
					__FUNCTION__, __LINE__);
			exit(-1);
		}
	}
	for (int32_t i = 0; i < qlen; ++i) {
		_qtable[i] = _dprofile + 64 * query[i];
	}

	/*8-bit data*/
	if(search7(_qtable, _goe, _gext, (BYTE*) _score_matrix_7, _dprofile,
				_hearray, qlen, numSeqs, seqOffsets, sequences, scores) >= _scoreLimit7){
		/*16-bit data*/
		search7_overflows((WORD**) _qtable, _goe, _gext, (WORD*) _score_matrix_16,
			(WORD*) _dprofile, (WORD*) _hearray, qlen, numSeqs, seqOffsets,
			sequences, scores);
	}
}
void Aligner::lalignScoreDual(uint8_t* query, int qlen, uint8_t* sequences,
		int32_t* seqOffsets, int numSeqs, SeqEntry* scores) {
	if (qlen > _maxQuerySize) {
		_maxQuerySize = qlen * 2;
		if (_qtable) {
			delete[] _qtable;
		}
		_qtable = new BYTE*[_maxQuerySize];
		if (_qtable == NULL) {
			fprintf(stderr, "Memory allocation failed in function %s line %d\n",
					__FUNCTION__, __LINE__);
			exit(-1);
		}
		if (_hearray) {
			delete[] _hearray;
		}
		_hearray = new BYTE[_maxQuerySize * 32];
		if (_hearray == NULL) {
			fprintf(stderr, "Memory allocation failed in function %s line %d\n",
					__FUNCTION__, __LINE__);
			exit(-1);
		}
	}
	for (int32_t i = 0; i < qlen; ++i) {
		_qtable[i] = _dprofile + 64 * query[i];
	}

	/*16-bit data*/
	search16((WORD**) _qtable, _goe, _gext, (WORD*) _score_matrix_16,
			(WORD*) _dprofile, (WORD*) _hearray, qlen, numSeqs, seqOffsets,
			sequences, scores);
}
int32_t Aligner::search7(BYTE** qtable, BYTE gap_open_penalty,
		BYTE gap_extend_penalty, BYTE * score_matrix, BYTE * dprofile,
		BYTE * hearray, int32_t qlen, int32_t numSeqs, int32_t *seqOffsets,
		uint8_t *sequences, SeqEntry* scores)
{

	int32_t maxScore = 0;
	__m128i S, Q, R, T, M, Z, T0;
	__m128i *hep, **qp;
	BYTE * d_begin[N16_CHANNELS];

	__m128i dseqalloc[CDEPTH];

	BYTE * dseq = (BYTE*) &dseqalloc;
	BYTE zero;

	long seq_id[N16_CHANNELS];
	long next_id = 0;
	int32_t done;

	memset(hearray, 0x80, qlen * 32);

	Z = _mm_set_epi8(0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
			0x80, 0x80, 0x80, 0x80, 0x80, 0x80);
	T0 = _mm_set_epi8(0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80);
	Q = _mm_set_epi8(gap_open_penalty, gap_open_penalty, gap_open_penalty,
			gap_open_penalty, gap_open_penalty, gap_open_penalty,
			gap_open_penalty, gap_open_penalty, gap_open_penalty,
			gap_open_penalty, gap_open_penalty, gap_open_penalty,
			gap_open_penalty, gap_open_penalty, gap_open_penalty,
			gap_open_penalty);
	R = _mm_set_epi8(gap_extend_penalty, gap_extend_penalty, gap_extend_penalty,
			gap_extend_penalty, gap_extend_penalty, gap_extend_penalty,
			gap_extend_penalty, gap_extend_penalty, gap_extend_penalty,
			gap_extend_penalty, gap_extend_penalty, gap_extend_penalty,
			gap_extend_penalty, gap_extend_penalty, gap_extend_penalty,
			gap_extend_penalty);
	zero = 0;
	done = 0;

	S = Z;

	hep = (__m128i *) hearray;
	qp = (__m128i **) qtable;

	for (int c = 0; c < N16_CHANNELS; c++)
	{
		d_begin[c] = &zero;
		seq_id[c] = -1;
	}

	int easy = 0;

	while (1)
	{
		if (easy)
		{
			// fill all channels

			for (int c = 0; c < N16_CHANNELS; c++)
			{
				for (int j = 0; j < CDEPTH; j++)
				{
					BYTE v = *(d_begin[c]);
					dseq[N16_CHANNELS * j + c] = v;
					if (v)
						d_begin[c]++;
				}
				if (!*(d_begin[c]))
					easy = 0;
			}

			if (_haveSSSE3)
			{
				dprofile_shuffle7(dprofile, score_matrix, dseq);
			}
			else
			{
				dprofile_fill7(dprofile, score_matrix, dseq);
			}

			donormal7(&S, hep, qp, &Q, &R, qlen, &Z);
		}
		else
		{
			// One or more sequences ended in the previous block
			// We have to switch over to a new sequence

			easy = 1;

			M = _mm_setzero_si128();
			T = T0;
			for (int c = 0; c < N16_CHANNELS; c++)
			{
				if (*(d_begin[c]))
				{
					// this channel has more sequence

					for (int j = 0; j < CDEPTH; j++)
					{
						BYTE v = *(d_begin[c]);
						dseq[N16_CHANNELS * j + c] = v;
						if (v)
							d_begin[c]++;
					}
					if (!*(d_begin[c]))
						easy = 0;
				}
				else
				{
					// sequence in channel c ended
					// change of sequence

					M = _mm_xor_si128(M, T);

					long cand_id = seq_id[c];

					if (cand_id >= 0)
					{
						// save score
						long score = ((BYTE*) &S)[c] - 0x80;
						scores[cand_id].value = score;
						if (score > maxScore)
						{
							maxScore = score;
						}
						done++;
					}
					if (next_id < numSeqs)
					{
						// get next sequence
						seq_id[c] = next_id;
						d_begin[c] = sequences + seqOffsets[next_id];
						next_id++;

						// fill channel
						for (int j = 0; j < CDEPTH; j++)
						{
							BYTE v = *(d_begin[c]);
							dseq[N16_CHANNELS * j + c] = v;
							if (v)
								d_begin[c]++;
						}
						if (!*(d_begin[c]))
							easy = 0;
					}
					else
					{
						// no more sequences, empty channel
						seq_id[c] = -1;
						d_begin[c] = &zero;
						for (int j = 0; j < CDEPTH; j++)
							dseq[N16_CHANNELS * j + c] = 0;
					}

				}

				T = _mm_slli_si128(T, 1);
			}

			if (done == numSeqs)
				break;

			if (_haveSSSE3)
			{
				dprofile_shuffle7(dprofile, score_matrix, dseq);
			}
			else
			{
				dprofile_fill7(dprofile, score_matrix, dseq);
			}

			domasked7(&S, hep, qp, &Q, &R, qlen, &Z, &M);
		}
	}
	return maxScore;
}
void Aligner::search7_overflows(WORD** qtable, WORD gap_open_penalty,
		WORD gap_extend_penalty, WORD * score_matrix, WORD * dprofile,
		WORD * hearray, int32_t qlen, int32_t numSeqs, int32_t *seqOffsets,
		uint8_t *sequences, SeqEntry* scores) {

	__m128i S, Q, R, T, M, Z, T0;
	__m128i *hep, **qp;
	BYTE * d_begin[N8_CHANNELS];

	__m128i dseqalloc[CDEPTH];

	BYTE * dseq = (BYTE *) &dseqalloc;
	BYTE zero;

	int32_t seq_id[N8_CHANNELS];
	int32_t next_id = 0;
	int32_t done;

	Z = _mm_set_epi16(0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000,
			0x8000);
	T0 = _mm_set_epi16(0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
			0x8000);
	Q = _mm_set_epi16(gap_open_penalty, gap_open_penalty, gap_open_penalty,
			gap_open_penalty, gap_open_penalty, gap_open_penalty,
			gap_open_penalty, gap_open_penalty);
	R = _mm_set_epi16(gap_extend_penalty, gap_extend_penalty,
			gap_extend_penalty, gap_extend_penalty, gap_extend_penalty,
			gap_extend_penalty, gap_extend_penalty, gap_extend_penalty);

	zero = 0;
	done = 0;

	S = Z;

	hep = (__m128i *) hearray;
	qp = (__m128i **) qtable;

	for (int a = 0; a < qlen; a++) {
		hep[2 * a] = Z;
		hep[2 * a + 1] = Z;
	}

	for (int c = 0; c < N8_CHANNELS; c++) {
		d_begin[c] = &zero;
		seq_id[c] = -1;
	}

	int easy = 0;

	while (1) {
		if (easy) {
			for (int c = 0; c < N8_CHANNELS; c++) {
				for (int j = 0; j < CDEPTH; j++) {
					BYTE v = *(d_begin[c]);
					dseq[N8_CHANNELS * j + c] = v;
					if (v)
						d_begin[c]++;
				}
				if (!*(d_begin[c]))
					easy = 0;
			}

			dprofile_fill16(dprofile, score_matrix, dseq);
			donormal16(&S, hep, qp, &Q, &R, qlen, &Z);

		} else {

			easy = 1;

			M = _mm_setzero_si128();
			T = T0;

			for (int c = 0; c < N8_CHANNELS; c++) {
				if (*(d_begin[c])) {
					for (int j = 0; j < CDEPTH; j++) {
						BYTE v = *(d_begin[c]);
						dseq[N8_CHANNELS * j + c] = v;
						if (v)
							d_begin[c]++;
					}

					if (!*(d_begin[c]))
						easy = 0;

				} else {
					M = _mm_xor_si128(M, T);

					long cand_id = seq_id[c];
					if (cand_id >= 0) {
						int score = ((WORD*) &S)[c] - 0x8000;
						/*save the alignment score*/
						scores[cand_id].value = score;
						done++;
					}

          /*find the next non-processed sequence*/
          for (;
              next_id < numSeqs
                  && scores[next_id].value < _scoreLimit7;
              ++next_id)
          {
            done++;
          }
					if (next_id < numSeqs) {
						seq_id[c] = next_id;
						d_begin[c] = sequences + seqOffsets[next_id];
						next_id++;

						for (int j = 0; j < CDEPTH; j++) {
							BYTE v = *(d_begin[c]);
							dseq[N8_CHANNELS * j + c] = v;
							if (v)
								d_begin[c]++;
						}
						if (!*(d_begin[c]))
							easy = 0;
					} else {
						seq_id[c] = -1;
						d_begin[c] = &zero;
						for (int j = 0; j < CDEPTH; j++)
							dseq[N8_CHANNELS * j + c] = 0;
					}
				}
				T = _mm_slli_si128(T, 2);
			}

			if (done == numSeqs) {
				break;
			}
			dprofile_fill16(dprofile, score_matrix, dseq);
			domasked16(&S, hep, qp, &Q, &R, qlen, &Z, &M);
		}
	}
}

void Aligner::search16(WORD** qtable, WORD gap_open_penalty,
		WORD gap_extend_penalty, WORD * score_matrix, WORD * dprofile,
		WORD * hearray, int32_t qlen, int32_t numSeqs, int32_t *seqOffsets,
		uint8_t *sequences, SeqEntry* scores) {

	__m128i S, Q, R, T, M, Z, T0;
	__m128i *hep, **qp;
	BYTE * d_begin[N8_CHANNELS];

	__m128i dseqalloc[CDEPTH];

	BYTE * dseq = (BYTE *) &dseqalloc;
	BYTE zero;

	int32_t seq_id[N8_CHANNELS];
	int32_t next_id = 0;
	int32_t done;

	Z = _mm_set_epi16(0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000,
			0x8000);
	T0 = _mm_set_epi16(0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
			0x8000);
	Q = _mm_set_epi16(gap_open_penalty, gap_open_penalty, gap_open_penalty,
			gap_open_penalty, gap_open_penalty, gap_open_penalty,
			gap_open_penalty, gap_open_penalty);
	R = _mm_set_epi16(gap_extend_penalty, gap_extend_penalty,
			gap_extend_penalty, gap_extend_penalty, gap_extend_penalty,
			gap_extend_penalty, gap_extend_penalty, gap_extend_penalty);

	zero = 0;
	done = 0;

	S = Z;

	hep = (__m128i *) hearray;
	qp = (__m128i **) qtable;

	for (int a = 0; a < qlen; a++) {
		hep[2 * a] = Z;
		hep[2 * a + 1] = Z;
	}

	for (int c = 0; c < N8_CHANNELS; c++) {
		d_begin[c] = &zero;
		seq_id[c] = -1;
	}

	int easy = 0;

	while (1) {
		if (easy) {
			for (int c = 0; c < N8_CHANNELS; c++) {
				for (int j = 0; j < CDEPTH; j++) {
					BYTE v = *(d_begin[c]);
					dseq[N8_CHANNELS * j + c] = v;
					if (v)
						d_begin[c]++;
				}
				if (!*(d_begin[c]))
					easy = 0;
			}

			dprofile_fill16(dprofile, score_matrix, dseq);
			donormal16(&S, hep, qp, &Q, &R, qlen, &Z);

		} else {

			easy = 1;

			M = _mm_setzero_si128();
			T = T0;

			for (int c = 0; c < N8_CHANNELS; c++) {
				if (*(d_begin[c])) {
					for (int j = 0; j < CDEPTH; j++) {
						BYTE v = *(d_begin[c]);
						dseq[N8_CHANNELS * j + c] = v;
						if (v)
							d_begin[c]++;
					}

					if (!*(d_begin[c]))
						easy = 0;

				} else {
					M = _mm_xor_si128(M, T);

					long cand_id = seq_id[c];
					if (cand_id >= 0) {
						int score = ((WORD*) &S)[c] - 0x8000;
						/*save the alignment score*/
						scores[cand_id].value = score;
						done++;
					}
					if (next_id < numSeqs) {
						seq_id[c] = next_id;
						d_begin[c] = sequences + seqOffsets[next_id];
						next_id++;

						for (int j = 0; j < CDEPTH; j++) {
							BYTE v = *(d_begin[c]);
							dseq[N8_CHANNELS * j + c] = v;
							if (v)
								d_begin[c]++;
						}
						if (!*(d_begin[c]))
							easy = 0;
					} else {
						seq_id[c] = -1;
						d_begin[c] = &zero;
						for (int j = 0; j < CDEPTH; j++)
							dseq[N8_CHANNELS * j + c] = 0;
					}
				}
				T = _mm_slli_si128(T, 2);
			}

			if (done == numSeqs) {
				break;
			}
			dprofile_fill16(dprofile, score_matrix, dseq);
			domasked16(&S, hep, qp, &Q, &R, qlen, &Z, &M);
		}
	}
}

