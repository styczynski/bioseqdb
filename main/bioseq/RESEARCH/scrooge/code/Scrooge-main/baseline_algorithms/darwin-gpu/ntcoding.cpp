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

#include "ntcoding.h"
#include <stdlib.h>
#include <cstring>
#include <limits>

uint32_t NtChar2Int (char nt) {
    switch(nt) {
        case 'a':
        case 'A': return A_NT;
        case 'c':
        case 'C': return C_NT;
        case 'g':
        case 'G': return G_NT;
        case 't':
        case 'T': return T_NT;
        case 'n':
        case 'N': return N_NT;
        default: return N_NT;
    }
}

uint32_t KmerToIndex(std::string kmer) {
    uint32_t index = 0; 
    char nt;
    for (int i = 0; i < kmer.length(); i++) {
        nt = (kmer.at(i));
        index = (index << 2) + NtChar2Int(nt);
    }
    return index;
}

static inline int NtToTwoBit (char nt) {
    switch (nt) {
        case 'a':
        case 'A': return 0;
        case 'c':
        case 'C': return 1;
        case 'g':
        case 'G': return 2;
        case 't':
        case 'T': return 3;
        default : return 0;
    }
    return 0;
}
void GenerateShapePos(std::string shape);

// Integer hash function developed by Thomas Wang 
// See <http://web.archive.org/web/20071223173210/http://www.concentric.net/~Ttwang/tech/inthash.htm>
static inline uint32_t hash32(uint32_t key, int k)
{
    uint32_t m = (1 << 2*k) -1;
    key = (~key + (key << 21)) & m;
    key = key ^ (key >> 24);
    key = ((key + (key << 3)) + (key << 8)) & m;
    key = key ^ (key >> 14);
    key = ((key + (key << 2)) + (key << 4)) & m;
    key = key ^ (key >> 28);
    key = (key + (key << 31)) & m;
    return key;
}

uint32_t* SeqToTwoBit (char* seq, uint32_t seq_len) {
    uint32_t len_2bit = 1+(seq_len/16);
    uint32_t* s_2bit = (uint32_t*) calloc(len_2bit, sizeof(uint32_t));

    char c;
    int idx, j_max;
    for (uint32_t i = 0; i < seq_len; i+=16) {
        j_max = FIND_MIN(16, seq_len-i);
        idx = i/16;
        for (int j = 0; j < j_max; j++) {
            c = seq[i+j]; 
            s_2bit[idx] += (NtToTwoBit(c) << 2*j);
        }
    }
    return s_2bit; 

}

static inline uint32_t Min_Window (uint32_t* window, int size) {
    uint32_t min = std::numeric_limits<uint32_t>::max();
    for (int i = 0; i < size; i++) {
        if (min > window[i]) {
            min = window[i];
        }
    }
    return min;
}

static inline uint32_t GetSeedAtPos (uint32_t* s_2bit, uint32_t pos, int k) {
    int idx, shift;
    uint32_t m = (1 << 2*k) - 1;
    uint32_t seed;
    idx = pos/16;
    shift = pos%16;
    uint64_t concat = (((uint64_t) s_2bit[idx+1]) << 32) + s_2bit[idx];
    seed = (concat >> 2*shift) & m;
    return seed;
}

std::pair<uint64_t*, uint32_t> TwoBitToMinimizers (uint32_t* s_2bit, uint32_t s_len, int k, int w) {
    uint32_t* window = (uint32_t*) calloc(w, sizeof(uint32_t));
    uint64_t last_m = 0;
    uint32_t last_p = 0;
    uint64_t m;

    uint32_t N = 0;

    uint64_t* minimizers = (uint64_t*) calloc(s_len*16, sizeof(uint64_t));
    for (int p = 0; p < w-1; p++) {
        window[p] = hash32(GetSeedAtPos(s_2bit, p, k), k);
    }

    for (uint32_t p = w-1; p < 16*s_len - k - w; p++) {
        window[p%w] = hash32(GetSeedAtPos(s_2bit, p, k), k);
        m = Min_Window(window, w);
        if ((m != last_m) || (p - last_p >= w)) {
            minimizers[N++] = (m << 32) + p;
            last_m = m;
            last_p = p;
        }
    }
    
    uint64_t* new_minimizers = (uint64_t*) malloc(N*sizeof(uint64_t));
    memcpy(new_minimizers, minimizers, N*sizeof(uint64_t));
    delete[] minimizers;
    return std::pair <uint64_t*, uint32_t> (new_minimizers, N);
}

std::pair<uint64_t*, uint32_t> QTwoBitToMinimizers (uint32_t* s_2bit, uint32_t s_len, int k, int w) {
    uint32_t* window = (uint32_t*) calloc(w, sizeof(uint32_t));
    uint64_t last_m = 0;
    uint32_t last_p = 0;
    uint64_t m;

    uint32_t N = 0;

    uint64_t* minimizers = (uint64_t*) calloc(s_len*16, sizeof(uint64_t));
    for (int p = 0; p < w-1; p++) {
        window[p] = hash32(GetSeedAtPos(s_2bit, p, k), k);
    }

    for (uint32_t p = w-1; p < 16*s_len - k - w; p++) {
        window[p%w] = hash32(GetSeedAtPos(s_2bit, p, k), k);
        m = Min_Window(window, w);
        if ((m != last_m) || (p - last_p >= w)) {
            minimizers[N++] = ((uint64_t) p << 32) + m;
            last_m = m;
            last_p = p;
        }
    }
    
    uint64_t* new_minimizers = (uint64_t*) malloc(N*sizeof(uint64_t));
    memcpy(new_minimizers, minimizers, N*sizeof(uint64_t));
    delete[] minimizers;
    return std::pair <uint64_t*, uint32_t> (new_minimizers, N);
}

