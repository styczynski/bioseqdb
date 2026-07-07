
/*
Copyright 2018 Yatish Turakhia, Gill Bejerano and William Dally
Copyright 2018 Tong Dong Qiu

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <queue>

#ifndef GACT_H
#define GACT_H

#if defined(NOSCORE) || defined(TIME)
    #ifndef GPU
        #error NOSCORE/TIME cannot be used without GPU
    #endif
#endif

// actually declared in darwin.cpp, but gact.cpp also needs them
#ifdef GPU
    extern int NUM_BLOCKS;
    extern int THREADS_PER_BLOCK;
    extern int BATCH_SIZE;
#endif
extern int tile_size;
extern int tile_overlap;
extern int first_tile_score_threshold;


typedef struct {
    int ref_id;     // id of reference read
    int query_id;   // id of query read
    int ref_pos;    // start of next tile
    int query_pos;
    int ref_bpos;   // indicates where forward dir should start (during reverse), indicates where reverse dir ended (during forward)
    int query_bpos;
    int score;      // current score for current GACT_call
    int first_tile_score;     // score of first tile
    char first;     // 1: first tile of the GACT_call, 0: not first tile
    char reverse;   // 1: operate in reverse (towards pos = 0), 0: operate in forward direction (towards pos = len)
} GACT_call;

// implemented in cuda_header.h
struct CUDA_Stream_Holder;

typedef struct {
    uint32_t *packed_ref_seqs_d;
    uint32_t *packed_query_seqs_d;
    int32_t *ref_offsets_d;
    int32_t *query_offsets_d;
    const char *ref_seqs_d;
    const char *query_seqs_d;
    const int *ref_lens_d;
    const int *query_lens_d;
    const int *ref_poss_d;
    const int *query_poss_d;
    const char *reverses_d;
    const char *firsts_d;
    int *outs_d;
    int *matrices_d;
    CUDA_Stream_Holder *stream;
} GPU_storage;

// implemented in gact.cpp
void GACT (char *ref_str, char *query_str, \
        int ref_length, int query_length, \
        int tile_size, int tile_overlap, \
        int ref_pos, int query_pos, int first_tile_score_threshold, \
        int ref_id, int query_id, bool complement, \
        int match_score, int mismatch_score, int gap_open, int gap_extend, \
        std::ofstream &fout);

// implemented in gact.cpp
void GACT_Batch(std::vector<GACT_call> calls, int num_calls, \
    bool complement, int offset, GPU_storage *s, int match_score, \
    int mismatch_score, int gap_open, int gap_extend, \
    std::ofstream &fout);

// implemented in cuda_host.cu
void GPU_init(int tile_size, int tile_overlap, \
    int gap_open, int gap_extend, int match, int mismatch, \
    int early_terminate, std::vector<GPU_storage> *s, int num_threads);

void GPU_close(std::vector<GPU_storage> *s, int num_threads);

int* Align_Batch_GPU(std::vector<std::string> ref_seqs, \
    std::vector<std::string> query_seqs, \
    std::vector<int> ref_lens, std::vector<int> query_lens, \
    int *sub_mat, int gap_open, int gap_extend, \
    std::vector<int> ref_poss, std::vector<int> query_poss, \
    std::vector<char> reverses, std::vector<char> firsts, \
    int early_terminate, int tile_size, GPU_storage *s, \
    int num_blocks, int threads_per_block);

#endif  // GACT_H
