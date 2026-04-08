
/*
Copyright 2018 Yatish Turakhia, Gill Bejerano and William Dally
Copyright 2018 Tong Dong Qiu

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <iostream>
#include <string>
#include <cstring>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <vector>
#include <stdlib.h>
#include <sys/time.h>
#include <map>
#include <thread>
#include <mutex>
#include <fstream>
#include "gact.h"
#include "fasta.h"
#include "ntcoding.h"
#include "seed_pos_table.h"
#include "ConfigFile.h"

#include <condition_variable>

#ifndef Z_COMPILE_USED
    #error "These files should be compiled using the z_compile.sh script"
#endif

// true iff both fasta files have the same name
bool same_file = false;

int NUM_BLOCKS;
int THREADS_PER_BLOCK;
int BATCH_SIZE;

// GACT scoring, put gact_sub_mat back if match_score and mismatch_score are not enough
//int gact_sub_mat[10];
int match_score;
int mismatch_score;
int gap_open;
int gap_extend;

// D-SOFT parameters
int kmer_size;
uint32_t bin_size;
uint32_t window_size;
int dsoft_threshold;
int num_seeds;
int seed_occurence_multiple;
int max_candidates;
int num_nz_bins;
bool ignore_lower;

// GACT first tile
int first_tile_size;
int first_tile_score_threshold;

//GACT extend 
int tile_size;
int tile_overlap;

//Multi-threading
int num_threads;

static std::string reference_string;
static std::string query_string;

uint32_t reference_length;
uint32_t query_length;

struct timeval start, end_time;

std::vector<long long int> reference_lengths;
std::vector<std::string> reference_seqs;

std::vector<long long int> reads_lengths;
std::vector<std::string> reads_seqs;
std::vector<std::string> rev_reads_seqs;

std::vector<std::vector<std::string> > reference_descrips;
std::vector<long long int> reference_fileposs;

std::vector<std::vector<std::string> > reads_descrips;
std::vector<long long int> reads_fileposs;

char** reads_char;
char** rev_reads_char;

std::map<int, uint32_t> chr_id_to_start_bin;
std::map<uint32_t, int> bin_to_chr_id;

SeedPosTable *sa;

std::mutex io_lock;
std::mutex convert_lock;        // for converting bases to 2b values
std::mutex sync_mutex;
std::mutex sync_mutex2;
std::condition_variable cond_var;
int t_done;

int total_calls = 0;

std::string RevComp(std::string seq) {
    std::string rc = "";
    for (int i = seq.size()-1; i >= 0; i--) {
        if (seq[i] != 'a' && seq[i] != 'A' &&
                seq[i] != 'c' && seq[i] != 'C' &&
                seq[i] != 'g' && seq[i] != 'G' &&
                seq[i] != 't' && seq[i] != 'T' &&
                seq[i] != 'n' && seq[i] != 'N') {
            std::cerr<<"Bad Nt char: "<< seq[i] <<std::endl;
            exit(1);
        }
        else {
            switch (seq[i]) {
                case 'a': rc += 't';
                          break;
                case 'A': rc += 'T';
                          break;
                case 'c': rc += 'g';
                          break;
                case 'C': rc += 'G';
                          break;
                case 'g': rc += 'c';
                          break;
                case 'G': rc += 'C';
                          break;
                case 't': rc += 'a';
                          break;
                case 'T': rc += 'A';
                          break;
                case 'n': rc += 'n';
                          break;
                case 'N': rc += 'N';
                          break;
            }
        }
    }
    return rc;
}


void PrintTileLocation (std::string read_name, \
    uint32_t candidate_hit, uint32_t last_hit_offset, char strand) {

    int         chr_id    = bin_to_chr_id[candidate_hit/bin_size];
    std::string chr       = reference_descrips[chr_id][0];
    uint32_t    start_bin = chr_id_to_start_bin[chr_id];

    std::cout << \
    "GACT_call " << \
    read_name << " " << \
    chr << " " << \
    candidate_hit - (start_bin*bin_size) << " " << \
    last_hit_offset << " " << \
    strand << std::endl;
}

// each CPU thread aligns a batch of multiple reads against the SeedTable
// the params start_read_num and last_read_num indicate which readIDs are to be aligned for each CPU thread
#ifdef GPU
void AlignReads (int start_read_num, int last_read_num, int cpu_id, GPU_storage s)
#else
void AlignReads (int start_read_num, int last_read_num, int cpu_id)
#endif
{
    std::string filename = "darwin." + to_string(cpu_id) + ".out";
    std::ofstream fout(filename);
    if(fout.is_open() == false){
        io_lock.lock();
        std::cerr << "ERROR cannot open output file" << std::endl;
        io_lock.unlock();
        return;
    }

    uint32_t log_bin_size = (uint32_t) (log2(bin_size));
    int num_bins = 1 + (reference_length >> log_bin_size);

    // candidate_hit_offset contains 64 bits
    // the high 32 bits contain the position in the ref, the low 32 bits the position in the read/query
    uint64_t* candidate_hit_offset;

    struct timeval begin, finish;

    gettimeofday(&begin, NULL);
    uint32_t* nz_bins_array = new uint32_t[num_nz_bins];
    uint64_t* bin_count_offset_array = new uint64_t[num_bins];
    candidate_hit_offset = new uint64_t[max_candidates];

    for(int i=0; i < num_bins; i++) {
        bin_count_offset_array[i] = 0;
    }


#ifdef GPU
    std::vector<GACT_call> GACT_calls_for, GACT_calls_rev;
#endif

    int num_candidates_for, num_candidates_rev;
    int total_calls_for = 0, total_calls_rev = 0;

    for (int k = start_read_num; k < last_read_num; k++) {
        int len = reads_lengths[k];

        // Forward reads
        num_candidates_for = sa->DSOFT(reads_char[k], len, num_seeds, dsoft_threshold, candidate_hit_offset, bin_count_offset_array, nz_bins_array, max_candidates);
        total_calls_for += num_candidates_for;
        for (int i = 0; i < num_candidates_for; i++) {
            int ref_pos = (candidate_hit_offset[i] >> 32);
            int chr_id = bin_to_chr_id[ref_pos/bin_size];
            uint32_t start_bin = chr_id_to_start_bin[chr_id];
            ref_pos -= start_bin*bin_size;
            int query_pos = candidate_hit_offset[i] & 0xffffffff;

            if(ref_pos > reference_lengths[chr_id]){
                ref_pos = reference_lengths[chr_id];
            }
#ifdef GPU
            // store GACT_call
            GACT_call g;
            GACT_calls_for.push_back(g);
            int idx = GACT_calls_for.size() - 1;
            GACT_calls_for[idx].ref_id = chr_id;
            GACT_calls_for[idx].query_id = k;
            GACT_calls_for[idx].ref_pos = ref_pos;
            GACT_calls_for[idx].query_pos = query_pos;
            GACT_calls_for[idx].ref_bpos = ref_pos;
            GACT_calls_for[idx].query_bpos = query_pos;
            GACT_calls_for[idx].score = 0;
            GACT_calls_for[idx].first = 1;
            GACT_calls_for[idx].reverse = 1;
#else   // perform GACT immediately
            GACT((char*)reference_seqs[chr_id].c_str(), reads_char[k], \
                reference_lengths[chr_id], len, \
                tile_size, tile_overlap, \
                ref_pos, query_pos, first_tile_score_threshold, \
                chr_id, k, false, \
                match_score, mismatch_score, gap_open, gap_extend, \
                fout);//*/
#endif
        }   // end for all num_candidates_for seed hits


        // Reverse complement reads
        num_candidates_rev = sa->DSOFT(rev_reads_char[k], len, num_seeds, dsoft_threshold, candidate_hit_offset, bin_count_offset_array, nz_bins_array, max_candidates);
        total_calls_rev += num_candidates_rev;
        for (int i = 0; i < num_candidates_rev; i++) {
            int ref_pos = (candidate_hit_offset[i] >> 32);
            int chr_id = bin_to_chr_id[ref_pos/bin_size];
            uint32_t start_bin = chr_id_to_start_bin[chr_id];
            ref_pos -= start_bin*bin_size;
            int query_pos = candidate_hit_offset[i] & 0xffffffff;

            if(ref_pos > reference_lengths[chr_id]){
                ref_pos = reference_lengths[chr_id];
            }
#ifdef GPU
            // store GACT_call
            GACT_call g;
            GACT_calls_rev.push_back(g);
            int idx = GACT_calls_rev.size() - 1;
            GACT_calls_rev[idx].ref_id = chr_id;
            GACT_calls_rev[idx].query_id = k;
            GACT_calls_rev[idx].ref_pos = ref_pos;
            GACT_calls_rev[idx].query_pos = query_pos;
            GACT_calls_rev[idx].ref_bpos = ref_pos;
            GACT_calls_rev[idx].query_bpos = query_pos;
            GACT_calls_rev[idx].score = 0;
            GACT_calls_rev[idx].first = 1;
            GACT_calls_rev[idx].reverse = 1;
#else   // perform GACT immediately
            GACT((char*)reference_seqs[chr_id].c_str(), rev_reads_char[k], \
                reference_lengths[chr_id], len, \
                tile_size, tile_overlap, \
                ref_pos, query_pos, first_tile_score_threshold, \
                chr_id, k, true, \
                match_score, mismatch_score, gap_open, gap_extend, \
                fout);//*/
#endif
        }   // end for all num_candidates_rev seed hits
    }   // end for every read assigned to this CPU thread

    io_lock.lock();
    printf("num_candidates: %d %d\n", total_calls_for, total_calls_rev);
    io_lock.unlock();

#ifdef GPU
    gettimeofday(&finish, NULL);
    long useconds = finish.tv_usec - begin.tv_usec;
    long seconds = finish.tv_sec - begin.tv_sec;
    long mseconds = ((seconds) * 1000 + useconds/1000.0) + 0.5;
    io_lock.lock();
    std::cout << "Time finding seeds: " << mseconds <<" msec" << std::endl;
    io_lock.unlock();
    gettimeofday(&begin, NULL);

    int N, M, first, last;
    N = reference_seqs.size();
    M = N / num_threads;
    first = cpu_id * M;
    if(cpu_id == num_threads - 1){
        last = N;
    }else{
        last = (cpu_id + 1) * M;
    }

    // change bases from chars to 2 bit values
    for(int i = first; i < last; ++i){
        convert_lock.lock();
        string old = reference_seqs[i];
        convert_lock.unlock();
        for(int j = 0; j < old.length(); ++j){
            switch(old[j]){
                case 'A':
                old[j] = 0;
                break;
                case 'C':
                old[j] = 1;
                break;
                case 'T':
                old[j] = 2;
                break;
                case 'G':
                old[j] = 3;
            }
        }
        convert_lock.lock();
        reference_seqs[i] = old;
        convert_lock.unlock();
    }

    // reads_seqs and rev_reads_seqs have the same number of reads
    if(reads_seqs.size() != rev_reads_seqs.size()){
        printf("ERROR reads and rev_reads have a different number of entries\n");
    }

    N = reads_seqs.size();
    M = N / num_threads;
    first = cpu_id * M;
    if(cpu_id == num_threads - 1){
        last = N;
    }else{
        last = (cpu_id + 1) * M;
    }

    for(int i = first; i < last; ++i){
        convert_lock.lock();
        string old = reads_seqs[i];
        convert_lock.unlock();
        for(int j = 0; j < old.length(); ++j){
            switch(old[j]){
                case 'A':
                old[j] = 0;
                break;
                case 'C':
                old[j] = 1;
                break;
                case 'T':
                old[j] = 2;
                break;
                case 'G':
                old[j] = 3;
            }
        }
        convert_lock.lock();
        reads_seqs[i] = old;
        convert_lock.unlock();
    }
    for(int i = first; i < last; ++i){
        convert_lock.lock();
        string old = rev_reads_seqs[i];
        convert_lock.unlock();
        for(int j = 0; j < old.length(); ++j){
            switch(old[j]){
                case 'A':
                old[j] = 0;
                break;
                case 'C':
                old[j] = 1;
                break;
                case 'T':
                old[j] = 2;
                break;
                case 'G':
                old[j] = 3;
            }
        }
        convert_lock.lock();
        rev_reads_seqs[i] = old;
        convert_lock.unlock();
    }
    gettimeofday(&finish, NULL);

    useconds = finish.tv_usec - begin.tv_usec;
    seconds = finish.tv_sec - begin.tv_sec;
    mseconds = ((seconds) * 1000 + useconds/1000.0) + 0.5;
    io_lock.lock();
    std::cout << "Time converting bases: " << mseconds <<" msec" << std::endl;
    io_lock.unlock();

    // synchronize all threads
    std::unique_lock<std::mutex> sync_lock(sync_mutex2);
    bool last_one = false;
    sync_mutex.lock();
    t_done++;
    if(t_done == num_threads){
        last_one = true;
        cond_var.notify_all();
    }
    sync_mutex.unlock();

    if(last_one == false){
        cond_var.wait(sync_lock, []{return t_done==num_threads;});
    }
    sync_lock.unlock();

    gettimeofday(&begin, NULL);

    gettimeofday(&begin, NULL);

    std::cout << "GACT forward" << endl;
    // start aligning the GACTcalls
    GACT_Batch(GACT_calls_for, total_calls_for, false, 0, &s, \
        match_score, mismatch_score, gap_open, gap_extend, fout);
    std::cout << "GACT backward" << endl;

    GACT_Batch(GACT_calls_rev, total_calls_rev, true, total_calls_for, &s, \
        match_score, mismatch_score, gap_open, gap_extend, fout);
    std::cout << "Done with GACT" << endl;

    gettimeofday(&finish, NULL);

    useconds = finish.tv_usec - begin.tv_usec;
    seconds = finish.tv_sec - begin.tv_sec;
    mseconds = ((seconds) * 1000 + useconds/1000.0) + 0.5;
    int alignments = GACT_calls_rev.size() + GACT_calls_for.size();
    io_lock.lock();
    total_calls += alignments;
    //std::cout << "Time GACT calling: " << mseconds <<" msec" << std::endl;
    //std::cout << "For " << alignments <<" alignments" << std::endl;
    //std::cout << "At " << 1000.0*alignments/mseconds << " alignments/second: " << std::endl;
    printf("Time GACT calling: %ld msec\n", mseconds);
    printf("For %d alignments\n", total_calls);
    printf("At %f alignments/second\n", 1000.0*total_calls/mseconds);
io_lock.unlock();
#endif

    delete[] bin_count_offset_array;
    delete[] nz_bins_array;
    delete[] candidate_hit_offset;
    fout.close();
}

int main(int argc, char *argv[]) {

    if (argc < 3) {
        fprintf(stderr, "Usage: ./reference_guided <REFERENCE>.fasta <READS>.fasta [NUM_BLOCKS THREADS_PER_BLOCK]\n");
        exit(1);
    }

    ConfigFile cfg("params.cfg");

    // GACT scoring
    /*gact_sub_mat[0] = cfg.Value("GACT_scoring", "sub_AA");
    gact_sub_mat[1] = cfg.Value("GACT_scoring", "sub_AC");
    gact_sub_mat[2] = cfg.Value("GACT_scoring", "sub_AG");
    gact_sub_mat[3] = cfg.Value("GACT_scoring", "sub_AT");
    gact_sub_mat[4] = cfg.Value("GACT_scoring", "sub_CC");
    gact_sub_mat[5] = cfg.Value("GACT_scoring", "sub_CG");
    gact_sub_mat[6] = cfg.Value("GACT_scoring", "sub_CT");
    gact_sub_mat[7] = cfg.Value("GACT_scoring", "sub_GG");
    gact_sub_mat[8] = cfg.Value("GACT_scoring", "sub_GT");
    gact_sub_mat[9] = cfg.Value("GACT_scoring", "sub_TT");*/
    match_score     = cfg.Value("GACT_scoring", "match");
    mismatch_score  = cfg.Value("GACT_scoring", "mismatch");
    gap_open        = cfg.Value("GACT_scoring", "gap_open");
    gap_extend      = cfg.Value("GACT_scoring", "gap_extend");

    // D-SOFT parameters
    kmer_size               = cfg.Value("DSOFT_params", "seed_size");
    bin_size                = cfg.Value("DSOFT_params", "bin_size");
    window_size             = cfg.Value("DSOFT_params", "window_size");
    dsoft_threshold         = cfg.Value("DSOFT_params", "threshold");
    num_seeds               = cfg.Value("DSOFT_params", "num_seeds");
    seed_occurence_multiple = cfg.Value("DSOFT_params", "seed_occurence_multiple");
    max_candidates          = cfg.Value("DSOFT_params", "max_candidates");
    num_nz_bins             = cfg.Value("DSOFT_params", "num_nz_bins");

    // GACT first tile
    first_tile_size            = cfg.Value("GACT_first_tile", "first_tile_size");
    first_tile_score_threshold = cfg.Value("GACT_first_tile", "first_tile_score_threshold");

    // GACT extend
    tile_size    = cfg.Value("GACT_extend", "tile_size");
    tile_overlap = cfg.Value("GACT_extend", "tile_overlap");

    // Multi-threading
    //num_threads = cfg.Value("Multithreading", "num_threads");
    num_threads = std::stoi(argv[3], nullptr);

    std::string reference_filename(argv[1]);
    std::string reads_filename(argv[2]);
    if(reference_filename.compare(reads_filename) == 0){
        same_file = true;
    }
    printf("same_file: %d\n", same_file);
#ifdef GPU
    NUM_BLOCKS = std::stoi(argv[4], nullptr);
    THREADS_PER_BLOCK = std::stoi(argv[5], nullptr);
    BATCH_SIZE = NUM_BLOCKS * THREADS_PER_BLOCK;
#endif

#ifdef GPU
    printf("Using GPU, batch_size: %d * %d = %d, CPU threads: %d\n", NUM_BLOCKS, THREADS_PER_BLOCK, BATCH_SIZE, num_threads);
#else
    std::cout << "Running on cpu, CPU threads: " << num_threads << std::endl;
#endif

    printf("Scores: match = %d, mismatch = %d, gap_open = %d, gap_extend = %d\n", match_score, mismatch_score, gap_open, gap_extend);
    printf("Minimizer window size: %d\n", window_size);

    int num_kmer = num_seeds;
    int kmer_count_threshold = dsoft_threshold;

    // LOAD REFERENCE
    std::cout << "\nLoading reference genome ...\n";
    gettimeofday(&start, NULL);

    ParseFastaFile(reference_filename, reference_descrips, reference_seqs, reference_lengths, reference_fileposs);

    reference_string = "";

    int curr_bin = 0;

    for (int i=0; i < reference_seqs.size(); i++) {
        chr_id_to_start_bin[i] =  curr_bin;
        reference_string += reference_seqs[i];
        for (int j = 0; j < (reference_seqs[i].length() / bin_size); j++) {
            bin_to_chr_id[curr_bin++] = i;
        }
        if (reference_seqs[i].length() % bin_size > 0) {
            reference_string += std::string((bin_size - (reference_seqs[i].length() % bin_size)), 'N');
            bin_to_chr_id[curr_bin++] = i;
        }
        //std::cout << i << " " << reference_descrips[i][0] << " length: " << reference_lengths[i] << std::endl;
    }

    reference_length = reference_string.length();
    std::cout << "Reference length: " << (unsigned int) reference_length << ", " << reference_seqs.size() << " pieces" << std::endl;


    gettimeofday(&end_time, NULL);
    long useconds = end_time.tv_usec - start.tv_usec;
    long seconds = end_time.tv_sec - start.tv_sec;
    long mseconds = ((seconds) * 1000 + useconds/1000.0) + 0.5;
    std::cout << "Time elapsed (loading reference genome): " << mseconds <<" msec" << std::endl;

    // LOAD READS
    std::cout << "\nLoading reads ...\n";
    gettimeofday(&start, NULL);

    ParseFastaFile(reads_filename, reads_descrips, reads_seqs, reads_lengths, reads_fileposs);

    int num_reads = reads_seqs.size();
    for (int i = 0; i < num_reads; i++) {
        std::string rev_read = RevComp(reads_seqs[i]);
        rev_reads_seqs.push_back(rev_read);
        //std::cout << "Read " << i << ", length: " << rev_read.size() << std::endl;
    }

    std::cout << "Number of reads: " << num_reads << std::endl;

    gettimeofday(&end_time, NULL);
    useconds = end_time.tv_usec - start.tv_usec;
    seconds = end_time.tv_sec - start.tv_sec;
    mseconds = ((seconds) * 1000 + useconds/1000.0) + 0.5;
    std::cout << "Time elapsed (loading reads): " << mseconds <<" msec" << std::endl;

    char* reference_char = (char*) reference_string.c_str();

    reads_char = new char*[num_reads];
    rev_reads_char = new char*[num_reads];
    for (int i =0; i < num_reads; i++) {
        reads_char[i] = (char*) reads_seqs[i].c_str();
        rev_reads_char[i] = (char*) rev_reads_seqs[i].c_str();
    }

    // CONSTRUCT SEED POSITION TABLE
    std::cout << "\nConstructing seed position table ...\n";
    gettimeofday(&start, NULL);

    sa = new SeedPosTable(reference_char, reference_length, kmer_size, seed_occurence_multiple, bin_size, window_size);

    gettimeofday(&end_time, NULL);

    useconds = end_time.tv_usec - start.tv_usec;
    seconds = end_time.tv_sec - start.tv_sec;
    mseconds = ((seconds) * 1000 + useconds/1000.0) + 0.5;
    std::cout << "Time elapsed (seed position table construction): " << mseconds <<" msec" << std::endl;

    uint64_t* seed_offset_vector = new uint64_t[num_kmer];
    uint32_t index = 0;
    int last_N_pos = -1;
    uint64_t sum = 0;
    uint64_t total_num_seeds = 0;
    char nt;

    // initialize shared variable for sync after base conversion
    t_done = 0;

    // GPU init
#ifdef GPU
    std::vector<GPU_storage> s;
    GPU_init(tile_size, tile_overlap, gap_open, gap_extend, match_score, mismatch_score, tile_size-tile_overlap, &s, num_threads);
#endif

    // RUN D-SOFT TO MAP READS
    gettimeofday(&start, NULL);

    std::cout << "\nFinding candidate bin locations for each read: " << std::endl;

    std::vector<std::thread> align_threads;
    int reads_per_thread = ceil(1.0*num_reads/num_threads);
    int i = 0;
    for (int k = 0; k < num_reads; k+=reads_per_thread, i++) {
        int last_read = (k+reads_per_thread > num_reads) ? num_reads : k+reads_per_thread;
#ifdef GPU
        align_threads.push_back(std::thread(AlignReads, k, last_read, i, s[i]));
#else
        align_threads.push_back(std::thread(AlignReads, k, last_read, i));
#endif
    }
    std::cout << align_threads.size() << " threads created\n";
    std::cout << "Synchronizing all threads...\n";
    for (auto& th : align_threads) th.join();

    gettimeofday(&end_time, NULL);

    useconds = end_time.tv_usec - start.tv_usec;
    seconds = end_time.tv_sec - start.tv_sec;
    mseconds = ((seconds) * 1000 + useconds/1000.0) + 0.5;
    std::cout << "Time elapsed (seed table querying + aligning): " << mseconds <<" msec" << std::endl;

#ifdef GPU
    GPU_close(&s, num_threads);
#endif

    return 0;
}

