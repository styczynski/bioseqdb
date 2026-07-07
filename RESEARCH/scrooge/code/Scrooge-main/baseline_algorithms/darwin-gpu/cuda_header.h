
/*
Copyright 2018 Yatish Turakhia, Gill Bejerano and William Dally
Copyright 2018 Tong Dong Qiu

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <stdio.h>

#ifndef CUDA_HEADER
#define CUDA_HEADER


struct CUDA_Stream_Holder {
    cudaStream_t stream;  
};

#ifdef GPU
__constant__ int _tile_size;
__constant__ int _tile_overlap;
__constant__ int _gap_open;
__constant__ int _gap_extend;
__constant__ int _match;
__constant__ int _mismatch;
__constant__ int _early_terminate;

#define NTHREADS (gridDim.x*blockDim.x)

#define __X NTHREADS
#define __Y NTHREADS

// for GASAL: DELETE_OP means a query_base is 'aligned' to a gap

typedef int AlnOp;
enum AlnOperands {ZERO_OP, DELETE_OP, INSERT_OP, MATCH_OP};
enum states {Z, D, I, M};

#define INF (1 << 29)

#define MAX_SEQ_LEN 324

__global__ void gasal_pack_kernel( \
    uint32_t* unpacked_query_batch, uint32_t* unpacked_target_batch, \
    uint32_t *packed_query_batch, uint32_t* packed_target_batch, \
    int query_batch_tasks_per_thread, int target_batch_tasks_per_thread, \
    uint32_t total_query_batch_regs, uint32_t total_target_batch_regs) {

    int32_t i;
    const int32_t tid = (blockIdx.x * blockDim.x) + threadIdx.x;//thread ID
    uint32_t n_threads = gridDim.x * blockDim.x;

    for (i = 0; i < query_batch_tasks_per_thread &&  (((i*n_threads)<<1) + (tid<<1) < total_query_batch_regs); ++i) {
        uint32_t *query_addr = &(unpacked_query_batch[(i*n_threads)<<1]);
        uint32_t reg1 = query_addr[(tid << 1)]; //load 4 bases of the query sequence from global memory
        uint32_t reg2 = query_addr[(tid << 1) + 1]; //load  another 4 bases
        uint32_t packed_reg = 0;
        packed_reg |= (reg1 & 15) << 28;        // ---
        packed_reg |= ((reg1 >> 8) & 15) << 24; //    |
        packed_reg |= ((reg1 >> 16) & 15) << 20;//    |
        packed_reg |= ((reg1 >> 24) & 15) << 16;//    |
        packed_reg |= (reg2 & 15) << 12;        //     > pack sequence
        packed_reg |= ((reg2 >> 8) & 15) << 8;  //    |
        packed_reg |= ((reg2 >> 16) & 15) << 4; //    |
        packed_reg |= ((reg2 >> 24) & 15);      //----
        uint32_t *packed_query_addr = &(packed_query_batch[i*n_threads]);
        packed_query_addr[tid] = packed_reg; //write 8 bases of packed query sequence to global memory
    }
    for (i = 0; i < target_batch_tasks_per_thread &&  (((i*n_threads)<<1) + (tid<<1)) < total_target_batch_regs; ++i) {
        uint32_t *target_addr = &(unpacked_target_batch[(i * n_threads)<<1]);
        uint32_t reg1 = target_addr[(tid << 1)]; //load 4 bases of the target sequence from global memory
        uint32_t reg2 = target_addr[(tid << 1) + 1]; //load  another 4 bases
        uint32_t packed_reg = 0;
        packed_reg |= (reg1 & 15) << 28;        // ---
        packed_reg |= ((reg1 >> 8) & 15) << 24; //    |
        packed_reg |= ((reg1 >> 16) & 15) << 20;//    |
        packed_reg |= ((reg1 >> 24) & 15) << 16;//    |
        packed_reg |= (reg2 & 15) << 12;        //     > pack sequence
        packed_reg |= ((reg2 >> 8) & 15) << 8;  //    |
        packed_reg |= ((reg2 >> 16) & 15) << 4; //    |
        packed_reg |= ((reg2 >> 24) & 15);      //----
        uint32_t *packed_target_addr = &(packed_target_batch[i * n_threads]);
        packed_target_addr[tid] = packed_reg; //write 8 bases of packed target sequence to global memory
    }

} // end gasal_pack_kernel()

__global__ void gasal_local_kernel( \
uint32_t *packed_query_batch, uint32_t *packed_target_batch, \
const int32_t *query_batch_lens, const int32_t *target_batch_lens, \
int32_t *query_offsets, int32_t *target_offsets, \
const int *query_poss, const int *ref_poss, \
int *out, \
const char *firsts, char *dir_matrix)
    {
        int32_t i, j, k, m, l;
        int32_t e, z;
        int32_t maxHH = 0;//initialize the maximum score to zero
        int32_t subScore;
        int32_t ridx;
        short3 HD;
        short3 initHD = make_short3(0, 0, 0);
        int32_t maxXY_x = 0;
        int32_t maxXY_y = 0;
        const uint32_t tid = (blockIdx.x * blockDim.x) + threadIdx.x;//thread ID
        int32_t query_len = query_batch_lens[tid];
        int32_t ref_len = target_batch_lens[tid];
        if(ref_len == -1){return;}
        const int query_pos = query_poss[tid];
        const int ref_pos = ref_poss[tid];
        const int row_len = _tile_size + 2;
#ifdef NOSCORE
        out += 5*tid;
#else
        out += (_tile_size * 2 * tid);
#endif
        packed_query_batch += tid;
        packed_target_batch += tid;
        int pos_score;        // score of last calculated corner
        const char first = firsts[tid];
        uint32_t query_batch_regs = (query_len >> 3) + (query_len&7 ? 1 : 0);//number of 32-bit words holding query_batch sequence
        uint32_t target_batch_regs = (ref_len >> 3) + (ref_len&7 ? 1 : 0);//number of 32-bit words holding target_batch sequence
        //-----arrays for saving intermediate values------
        short3 global[MAX_SEQ_LEN];
        int32_t h[9];
        int32_t f[9];
        int32_t p[9];
        int32_t d[9];
        //--------------------------------------------
        for (i = 0; i < MAX_SEQ_LEN; i++) {
            global[i] = initHD;
        }
        dir_matrix += tid;
        for (int i = 0; i < ref_len + 2; i++) {
            dir_matrix[(i*row_len)*__X] = ZERO_OP;
        }
        for (int j = 0; j < query_len + 2; j++) {
            dir_matrix[j*__X] = ZERO_OP;
        }
        dir_matrix += (_tile_size+3)*__X;
        for (i = 0; i < target_batch_regs; i++) { //target_batch sequence in rows
            for (m = 0; m < 9; m++) {
                    h[m] = 0;
                    f[m] = 0;
                    p[m] = 0;
                    d[m] = 0;
            }
            register uint32_t gpac = packed_target_batch[i*__Y];//load 8 packed bases from target_batch sequence
            ridx = 0;
            for (j = 0; j < query_batch_regs; j++) { //query_batch sequence in columns
                register uint32_t rpac = packed_query_batch[j*__Y];//load 8 bases from query_batch sequence

                //--------------compute a tile of 8x8 cells-------------------
                    for (k = 28; k >= 0; k -= 4) {
                        uint32_t rbase = (rpac >> k) & 15;//get a base from query_batch sequence
                        //-----load intermediate values--------------
                        HD = global[ridx];
                        h[0] = HD.x;
                        e = HD.y;
                        z = HD.z;
                        //-------------------------------------------
                        int ii = i*8;
                        int jj = j*8+7-k/4;
#pragma unroll 8
                        for (l = 28, m = 1; m < 9; l -= 4, m++, ii++) {
                            uint32_t gbase = (gpac >> l) & 15;//get a base from target_batch sequence
                            int ins_open = z + _gap_open;
                            int ins_extend = e + _gap_extend;
                            int del_open = d[m] + _gap_open;
                            int del_extend = f[m] + _gap_extend;
                            AlnOp tmp = ZERO_OP;
                            //int tmp2 = 0;
                            int tmp2 = 1<<31; //min int
                            subScore = (rbase == gbase) ? _match : _mismatch;
                            int match = p[m] + subScore;
                            z = match;
                            d[m] = match;

                            if(match > tmp2){
                                tmp2 = match;
                                tmp = MATCH_OP;
                            }
                            e = max(ins_open, ins_extend);
                            if(e > tmp2){
                                tmp2 = e;
                                tmp = INSERT_OP;
                            }
                            f[m] = max(del_open, del_extend);
                            if(f[m] > tmp2){
                                tmp2 = f[m];
                                tmp = DELETE_OP;
                            }

                            tmp += (ins_open >= ins_extend) ? (2 << INSERT_OP) : 0;
                            tmp += (del_open >= del_extend) ? (2 << DELETE_OP) : 0;

                            h[m] = tmp2;

                            // maximum score tie resolvement: keep element with highest i, if they have the same i, keep element with highest j
                            // interestingly, using a simpler max score tie resolvement system does not seem to impact the runtime
                            if(h[m] == maxHH){  
                                if(ii > maxXY_y){
                                    maxXY_y = ii;
                                    maxXY_x = jj;
                                    maxHH = h[m];
                                }
                                if(ii == maxXY_y){
                                    if(jj > maxXY_x){
                                        maxXY_y = ii;
                                        maxXY_x = jj;
                                        maxHH = h[m];
                                    }
                                }
                            }
                            if(h[m] > maxHH){
                                maxXY_y = ii;
                                maxXY_x = jj;
                                maxHH = h[m]; 
                            }

                            p[m] = h[m-1];

                            // store tracebackpointer for this element
                            int idx = ii*row_len + jj;
                            idx *= __X;
                            dir_matrix[idx] = tmp;

                            // might be able to remove this and use out[0] = h[m-1]
                            if(ii == ref_pos-1 && jj == query_pos-1){
                                pos_score = h[m];
                            }
                        }
                        //----------save intermediate values------------
                        HD.x = h[m-1];
                        HD.y = e;
                        HD.z = z;
                        global[ridx] = HD;
                        //---------------------------------------------
                        ridx++;
                    } // end of 8x8 tile
                //-------------------------------------------------------
            } // end for all query bases
        } // end for all target bases

#ifndef NOSCORE
        i = 5;
#endif
        int i_curr = ref_pos-1, j_curr = query_pos-1;
        int i_steps = 0, j_steps = 0;

        if(first){
            i_curr = maxXY_y;
            j_curr = maxXY_x;
            out[0] = maxHH;
            out[3] = i_curr+1;
            out[4] = j_curr+1;
        }else{
            out[0] = pos_score;
            //out[0] = h[m-1];
        }

        int idx = i_curr*row_len + j_curr;
        idx *= __X;
        char state = dir_matrix[idx] % 4;

    while (state != Z) {
        if ((i_steps >= _early_terminate) || (j_steps >= _early_terminate)) { // || (i_steps - j_steps > 30) || (i_steps - j_steps < -30)) {
            break;
        }
#ifndef NOSCORE
        out[i++] = state;
#endif

        if (state == M) {
            int idx = ((i_curr-1)*row_len+j_curr-1);
            idx *= __X;
            state = dir_matrix[idx] % 4;//*/
            i_curr--;
            j_curr--;
            i_steps++;
            j_steps++;
        }
        else if (state == I) {
            state = (dir_matrix[(i_curr*row_len+j_curr)*__X] & (2 << INSERT_OP)) ? M : I;
            i_curr--;
            i_steps++;
        }
        else if (state == D) {
            state = (dir_matrix[(i_curr*row_len+j_curr)*__X] & (2 << DELETE_OP)) ? M : D;
            j_curr--;
            j_steps++;
        }
    };

#ifndef NOSCORE
    out[i] = -1;
#endif
    out[1] = i_steps;
    out[2] = j_steps;

    return;
} // end gasal_local_kernel()


// error checking code taken from: https://codeyarns.com/2011/03/02/how-to-do-error-checking-in-cuda/
#define cudaSafeCall( err ) __cudaSafeCall( err, __FILE__, __LINE__ )
#define cudaCheckError()    __cudaCheckError( __FILE__, __LINE__ )
inline void __cudaSafeCall( cudaError err, const char *file, const int line )
  {
      if ( cudaSuccess != err )
      {
          //std::cerr << "cudaSafeCall() failed at " << file << ":" << line << ":" << cudaGetErrorString(err) << std::endl;
          printf("\ncudaSafeCall() failed at %s: %d: %s\n\n", file, line, cudaGetErrorString(err));
          exit( -1 );
      }
  }
inline void __cudaCheckError( const char *file, const int line )
  {
      cudaError err = cudaGetLastError();
      if ( cudaSuccess != err )
      {
          //std::cerr << "cudaCheckError() failed at " << file << ":" << line << ":" << cudaGetErrorString(err) << std::endl;
          printf("\ncudaSafeCall() failed at %s: %d: %s\n\n", file, line, cudaGetErrorString(err));
          exit( -1 );
      }
  }
// end of error checking code
#endif // GPU

#endif // CUDA_HEADER
