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

#include <string>
#include <vector>
#include <map>
#include <assert.h>
#include <stdint.h>
#include <math.h>
#include "ntcoding.h"

#define nz_bins 25000000

class SeedPosTable {
    private:
        uint32_t index_table_size_;
        uint32_t ref_size_;
        uint32_t bin_size_;
        uint32_t log_bin_size_;
        int kmer_size_;
//        int shape_size_;
        int window_size_;
        uint32_t kmer_max_occurence_;
        uint32_t *index_table_;
        uint32_t *pos_table_;

        uint32_t num_bins_;
        uint64_t* bin_count_offset_array_;
        uint32_t* nz_bins_;


    public:
        SeedPosTable();
        SeedPosTable(char* ref_str, uint32_t ref_length, int kmer_size, uint32_t seed_occurence_multiple, uint32_t bin_size, uint32_t window_size);
        ~SeedPosTable();

        bool IsPresent(uint32_t index);
        int GetKmerSize();
//        void DSOFT(uint64_t* seed_offset_vector, int N, int threshold, std::vector<uint64_t>& candidate_hit_offset);
//        int DSOFT(char* query, uint32_t query_length, int N, int threshold, uint64_t* candidate_hit_offset, int max_candidates);
        int DSOFT(char* query, uint32_t query_length, int N, int threshold, uint64_t* candidate_hit_offset, uint64_t* bin_count_offset_array, uint32_t* nz_bins_array, int max_candidates);
};



