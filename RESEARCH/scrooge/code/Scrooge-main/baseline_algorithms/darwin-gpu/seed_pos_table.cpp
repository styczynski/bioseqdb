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

#include "seed_pos_table.h"
#include <parallel/algorithm>

SeedPosTable::SeedPosTable() {
    ref_size_ = 0;
    kmer_size_ = 0;
    window_size_ = 0;
    num_bins_ = 0;
    nz_bins_ = 0;
}

int SeedPosTable::GetKmerSize() {
    return kmer_size_;
}

bool SeedPosTable::IsPresent(uint32_t index) {
    uint32_t start_index = (index > 0) ? index_table_[index-1] : 0;
    uint32_t end_index = index_table_[index];
    return (end_index - start_index <= kmer_max_occurence_);
}

SeedPosTable::SeedPosTable(char* ref_str, uint32_t ref_length, int kmer_size, uint32_t seed_occurence_multiple, uint32_t bin_size, uint32_t window_size) {
    
    assert(kmer_size <= 15);
    assert(kmer_size > 3); 
    assert(kmer_size > window_size); 

    kmer_size_ = kmer_size;
    bin_size_  = bin_size;
    window_size_ = window_size;
    log_bin_size_ = (uint32_t) (log2(bin_size_));
    ref_size_ = ref_length;
    
    kmer_max_occurence_ = seed_occurence_multiple * (1+((ref_length) >> (2*kmer_size)));
    
    uint32_t rlen_2bit = 1+(ref_length/16);
    uint32_t* r_2bit = SeqToTwoBit(ref_str, ref_length);

    int k = kmer_size;
    int w = window_size;
    std::pair <uint64_t*, uint32_t> m_n;
    m_n = TwoBitToMinimizers(r_2bit, rlen_2bit, k, w); 
    uint64_t* minimizers = m_n.first;
    uint32_t N = m_n.second;
    
//    std::sort(minimizers, minimizers + N);
    __gnu_parallel::sort(minimizers, minimizers + N);

    index_table_size_ = ((uint32_t)1 << 2*kmer_size) + 1;
    index_table_ = new uint32_t[index_table_size_];

    pos_table_ = new uint32_t[N];

    uint32_t curr_index = 0;
    uint32_t seed, pos; 

    for (uint32_t i = 0; i < N; i++) {
        pos = ((minimizers[i] << 32) >> 32);
        seed = (minimizers[i] >> 32);
        pos_table_[i] = pos;
        if (seed > curr_index) {
            for (uint32_t s = curr_index; s < seed; s++) {
                index_table_[s] = i;
            }
            curr_index = seed;
        }
    }
    for (uint32_t i = curr_index; i < index_table_size_; i++) {
        index_table_[i] = N;
    }

    delete[] r_2bit;
    delete[] minimizers;
}

int SeedPosTable::DSOFT(char* query, uint32_t query_length, int N, int threshold, uint64_t* candidate_hit_offset, uint64_t* bin_count_offset_array, uint32_t* nz_bins_array, int max_candidates) {
    uint32_t index, offset;
    uint32_t hit, bin, curr_count, last_offset, new_count, adjacent_bin, adjacent_count;
    uint64_t hit_offset;
    uint64_t num_nz_bins = 0;
    int num_seeds = 0;
    int num_candidates = 0;

    uint32_t qlen_2bit = ((query_length+15)/16);
    uint32_t* q_2bit = SeqToTwoBit(query, query_length);

    int k = kmer_size_;
    int w = window_size_;
    std::pair <uint64_t*, uint32_t> m_n;
    m_n = QTwoBitToMinimizers(q_2bit, qlen_2bit, k, w); 
    uint64_t* minimizers = m_n.first;
    uint32_t num_min = m_n.second;
    
    for (int i = 0; i < num_min; i++) {

        offset = (minimizers[i] >> 32);
        index = ((minimizers[i] << 32) >> 32);

        if (index != (1 << 31)) {
            uint32_t start_index = (index > 0) ? index_table_[index-1] : 0;
            uint32_t end_index = index_table_[index];

            if (end_index - start_index <= kmer_max_occurence_) {
                if (num_seeds > N) {
                    break;
                }
                num_seeds++;
                for (uint32_t j = start_index; j < end_index; j++) {
                    hit = pos_table_[j];
                    assert(hit < ref_size_);
                    if (hit >= offset) {
                        bin = ((hit - offset) / bin_size_);
                        curr_count = (bin_count_offset_array[bin] >> 32);
                        last_offset = ((bin_count_offset_array[bin] << 32) >> 32);
                        if (curr_count < threshold) {
                            new_count = ((offset - last_offset > kmer_size_) || (curr_count == 0)) ? curr_count + kmer_size_ : curr_count + (offset - last_offset);
                            bin_count_offset_array[bin] = ((uint64_t) new_count << 32) + offset; 
                            if (new_count >= threshold) {
                                uint64_t hit_offset = ((uint64_t) hit << 32) + offset;
                                //                            candidate_hit_offset.push_back(hit_offset);
                                if (num_candidates >= max_candidates) {
                                    break;
                                }
                                candidate_hit_offset[num_candidates++] = hit_offset;
                            }
                            if ((curr_count == 0) && (num_nz_bins < nz_bins)) {
                                nz_bins_array[num_nz_bins] = bin;
                                num_nz_bins++;
                            }
                        }
                    }
                }
            }
        }
    }
    for (uint32_t i = 0; i < num_nz_bins; i++) {
        bin = nz_bins_array[i];
        bin_count_offset_array[bin] = 0;
    }
    delete[] q_2bit;
    delete[] minimizers;
    return num_candidates;
}

