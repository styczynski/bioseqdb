/*
MIT License

Copyright (c) 2018 Yatish Turakhia, Gill Bejerano and William Dally

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#define A_NT 0
#define C_NT 1
#define G_NT 2
#define T_NT 3
#define N_NT 4

#include <stdint.h>
#include <iostream>
#include <assert.h>
#include <vector>

#define FIND_MIN(x, y) (x < y) ? x : y
#define FIND_MAX(x, y) (x > y) ? x : y


using namespace std;

uint32_t NtChar2Int (char nt);
uint32_t KmerToIndex(std::string kmer);

uint32_t* SeqToTwoBit (char* seq, uint32_t seq_len);
std::pair<uint64_t*, uint32_t> TwoBitToMinimizers (uint32_t* s_2bit, uint32_t s_len, int k, int w);
std::pair<uint64_t*, uint32_t> QTwoBitToMinimizers (uint32_t* s_2bit, uint32_t s_len, int k, int w);

