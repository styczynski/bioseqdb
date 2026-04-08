
/*
Copyright 2018 Yatish Turakhia, Gill Bejerano and William Dally
Copyright 2018 Tong Dong Qiu

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <iostream>
#include <queue>
#include "ntcoding.h"


#define INF (1 << 30)
#define MAX_TILE_SIZE 2049

typedef int AlnOp;
enum AlnOperands {ZERO_OP, DELETE_OP, INSERT_OP, MATCH_OP};
enum states {Z, D, I, M};



using namespace std;

std::queue<int> AlignWithBT(char* ref_seq, long long int ref_len, \
	char* query_seq, long long int query_len, \
	int match_score, int mismatch_score, int gap_open, int gap_extend, \
	int query_pos, int ref_pos, bool reverse, bool first, int early_terminate);

std::vector<std::queue<int> > Align_Batch(std::vector<std::string> ref_seqs, \
	std::vector<std::string> query_seqs, \
	std::vector<int> ref_lens, std::vector<int> query_lens, \
	int match_score, int mismatch_score, int gap_open, int gap_extend, \
	std::vector<int> ref_poss_b, std::vector<int> query_poss_b, \
	std::vector<char> reverses, std::vector<char> firsts, \
	int early_terminate);
