#ifndef HOST_MEM_H
#define HOST_MEM_H

#include <fstream>
#include "common.h"
#include "minimap.h"
#include "mmpriv.h"
//#include <cuda_runtime.h>
#include "/usr/local/cuda-12.1/targets/x86_64-linux/include/cuda_runtime.h"

struct host_input_buffer {
    char* h_seqs;
    int* h_lens;
    int* h_ofs;
};

struct host_output_buffer{
    int* h_score; 
    int* h_n_cigar;
    uint32_t* h_cigar;
};

void allocate_host_input_memory(char** h_seqs, int** h_lens, int** h_ofs, int max_seq_len, int batch_size, int num_device);
void allocate_host_output_memory(int** h_score, int** h_n_cigar, uint32_t** h_cigar, int max_seq_len, int batch_size, int num_device);
void fill_host_batch(mm_bseq_file_t ** fp, char* h_seqs, int* h_lens, int* h_ofs, int max_seq_len, int batch_size, int num_device);
void write_output_batch(std::ofstream& fout, const host_output_buffer& out, int batch_id, int chunk_size, int max_cigar_len, int batch_size);

   
#endif