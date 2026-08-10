#ifndef PIPELINE_H
#define PIPELINE_H
#include <stdint.h>
#include <string.h>
#include <ap_int.h>

#include "hls_snap_1024.H"
#include <action_memcopy.h> /* Memcopy Job definition */
#include "hw_action_hbm_memcopy_1024.H"
unsigned largest(unsigned arr[], unsigned n) ;
unsigned count_one_bit(d_bit_out_tiny_type &input_seq, unsigned seq_length);
int largest_bit(d_out_type arr, int n) ;
void NeighborhoodMap_bit(int read_length,
    d_bit_in_type & DNA_read, d_bit_in_type & DNA_ref, 
    d_bit_out_type & DNA_nsh,
    d_bit_out_type & DNA_shl_one,d_bit_out_type & DNA_shl_two,d_bit_out_type & DNA_shl_three,d_bit_out_type & DNA_shl_four,d_bit_out_type & DNA_shl_five, 
    d_bit_out_type & DNA_shr_one,d_bit_out_type & DNA_shr_two,d_bit_out_type & DNA_shr_three,d_bit_out_type & DNA_shr_four,d_bit_out_type & DNA_shr_five);
unsigned after_neighbohood(d_bit_out_tiny_type  DNA_nsh,
    d_bit_out_tiny_type  DNA_shl_one,d_bit_out_tiny_type  DNA_shl_two,d_bit_out_tiny_type  DNA_shl_three,d_bit_out_tiny_type DNA_shl_four,d_bit_out_tiny_type DNA_shl_five, 
    d_bit_out_tiny_type  DNA_shr_one,d_bit_out_tiny_type  DNA_shr_two,d_bit_out_tiny_type  DNA_shr_three,d_bit_out_tiny_type DNA_shr_four,d_bit_out_tiny_type  DNA_shr_five);
int SneakySnake_bit(int ReadLength, d_bit_in_type &ReadSeq, d_bit_in_type &RefSeq, int EditThreshold, int KmerSize);
void SneakySnake_stream(hls::stream<snap_membus_256_t> &ReadSeq_stream, hls::stream<snap_membus_256_t> &RefSeq_stream, int num_input_lines, g_count_struct &g_count);


#endif