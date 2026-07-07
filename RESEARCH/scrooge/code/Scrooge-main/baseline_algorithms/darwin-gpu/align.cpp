
/*
Copyright 2018 Yatish Turakhia, Gill Bejerano and William Dally
Copyright 2018 Tong Dong Qiu

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "align.h"
#include <stdio.h>


std::vector<std::queue<int> > Align_Batch(std::vector<std::string> ref_seqs, \
    std::vector<std::string> query_seqs, \
    std::vector<int> ref_lens, std::vector<int> query_lens, \
    int match_score, int mismatch_score, int gap_open, int gap_extend, \
    std::vector<int> ref_poss_b, std::vector<int> query_poss_b, \
    std::vector<char> reverses, std::vector<char> firsts, int early_terminate){

    std::vector<std::queue<int> > result;

    int BATCH_SIZE = ref_seqs.size();

    for(int j = 0; j < BATCH_SIZE; ++j){
        char *ref_seq = (char*)ref_seqs[j].c_str();
        char *query_seq = (char*)query_seqs[j].c_str();
        long long int ref_len = ref_lens[j];
        long long int query_len = query_lens[j];
        int ref_pos = ref_poss_b[j];
        int query_pos = query_poss_b[j];
        bool reverse = (reverses[j] == 1);
        bool first = (firsts[j] == 1);

        std::queue<int> BT_states;

        if(ref_len == -1){
            // push empty queue, otherwise GACT_Batch will misinterpret BT_statess
            result.push_back(BT_states);
            continue;
        }

        BT_states = AlignWithBT(ref_seq, ref_len, query_seq, query_len, \
            match_score, mismatch_score, gap_open, gap_extend, \
            query_pos, ref_pos, reverse, first, early_terminate);

        result.push_back(BT_states);
    }

    return result;
}




//return the output of the int queue
std::queue<int> AlignWithBT(char* ref_seq, long long int ref_len, \
    char* query_seq, long long int query_len, \
    int match_score, int mismatch_score, int gap_open, int gap_extend, \
    int query_pos, int ref_pos, bool reverse, bool first, int early_terminate) {

    // terminate the prgram if the R or Q length is greater than the tile size
    assert(ref_len < MAX_TILE_SIZE);
    assert(query_len < MAX_TILE_SIZE);

    //2 copies of the vectors for the previous/new values 
    //max of all other
    int h_matrix_wr[MAX_TILE_SIZE + 1];
    //matches
    int m_matrix_wr[MAX_TILE_SIZE + 1];
    //vector for insertion penalties
    int i_matrix_wr[MAX_TILE_SIZE + 1];
    //vector for deletion penalties
    int d_matrix_wr[MAX_TILE_SIZE + 1];

    int h_matrix_rd[MAX_TILE_SIZE + 1];
    int m_matrix_rd[MAX_TILE_SIZE + 1];
    int i_matrix_rd[MAX_TILE_SIZE + 1];
    int d_matrix_rd[MAX_TILE_SIZE + 1];

    //operands for alignment in the matrix - according to Smith Waterman's rules
    std::vector< std::vector<AlnOp> > dir_matrix(MAX_TILE_SIZE+1, std::vector<AlnOp>(MAX_TILE_SIZE+1,0));

    for (int i = 0; i < query_len + 1; i++) {
        h_matrix_rd[i] = 0;
        m_matrix_rd[i] = 0;
        i_matrix_rd[i] = -INF;
        d_matrix_rd[i] = -INF;
     
        h_matrix_wr[i] = 0;
        m_matrix_wr[i] = 0;
        i_matrix_wr[i] = -INF;
        d_matrix_wr[i] = -INF;
    }
    
    //initialize the operands int he direction matrix for the first row and first
    //column
    for (int i = 0; i < ref_len + 1; i++) {
        dir_matrix[i][0] = ZERO_OP;
    }

    for (int j = 0; j < query_len + 1; j++) {
        dir_matrix[0][j] = ZERO_OP;
    }

    int max_score = 0; 
    int pos_score = 0; 
    int max_i = 0; 
    int max_j = 0; 

    for (int i = 1; i < ref_len + 1; i++) {
        for (int k = 1; k < MAX_TILE_SIZE + 1; k++) {
                m_matrix_rd[k] = m_matrix_wr[k];
                h_matrix_rd[k] = h_matrix_wr[k];
                i_matrix_rd[k] = i_matrix_wr[k];
                d_matrix_rd[k] = d_matrix_wr[k];
        }

        //j - row number; i - column number
        for (int j = 1; j < query_len + 1; j++) {
            // reverse indicates the direction of the alignment
            // 1: towards position = 0, 0: towards position = length
#ifdef BATCH
            char ref_nt = (reverse) ? ref_seq[i-1] : ref_seq[ref_len-i];
            char query_nt = (reverse) ? query_seq[j-1] : query_seq[query_len-j];
#else
            char ref_nt = (reverse) ? ref_seq[ref_len-i] : ref_seq[i-1];
            char query_nt = (reverse) ? query_seq[query_len-j] : query_seq[j-1];
#endif

            int match = (query_nt == ref_nt) ? match_score : mismatch_score;

            //columnwise calculations
            // find out max value
            if (m_matrix_rd[j-1] > i_matrix_rd[j-1] && m_matrix_rd[j-1] > d_matrix_rd[j-1]) {
                    m_matrix_wr[j] = m_matrix_rd[j-1] + match;
            } else if (i_matrix_rd[j-1] > d_matrix_rd[j-1]) {
                    m_matrix_wr[j] = i_matrix_rd[j-1] + match;
            } else {
                    m_matrix_wr[j] = d_matrix_rd[j-1] + match;
            }
            if (m_matrix_wr[j] < 0) {
                    m_matrix_wr[j] = 0;
            }

            int ins_open   = m_matrix_rd[j] + gap_open;
            int ins_extend = i_matrix_rd[j] + gap_extend;
            int del_open   = m_matrix_wr[j-1] + gap_open;
            int del_extend = d_matrix_wr[j-1] + gap_extend;

            i_matrix_wr[j] = (ins_open > ins_extend) ? ins_open : ins_extend;

            d_matrix_wr[j] = (del_open > del_extend) ? del_open : del_extend;

            int max1 = m_matrix_wr[j] > i_matrix_wr[j] ? m_matrix_wr[j] : i_matrix_wr[j];
            int max2 = d_matrix_wr[j] > 0 ? d_matrix_wr[j] : 0;
            h_matrix_wr[j] = max1 > max2 ? max1 : max2;

            (dir_matrix)[i][j] = ((m_matrix_wr[j] >= i_matrix_wr[j]) ? \
                ((m_matrix_wr[j] >= d_matrix_wr[j]) ? MATCH_OP : DELETE_OP) : \
                ((i_matrix_wr[j] >= d_matrix_wr[j]) ? INSERT_OP : DELETE_OP));

            if ((m_matrix_wr[j] <= 0) && (i_matrix_wr[j] <= 0) && (d_matrix_wr[j] <= 0)) {
                (dir_matrix)[i][j] = ZERO_OP;
            }

            (dir_matrix)[i][j] += (ins_open >= ins_extend) ? (2 << INSERT_OP) : 0;
            (dir_matrix)[i][j] += (del_open >= del_extend) ? (2 << DELETE_OP) : 0;

            if (h_matrix_wr[j] >= max_score) {
                max_score = h_matrix_wr[j];
                max_i = i;
                max_j = j;
            }

            if ((i == ref_pos) && (j == query_pos)) {
                pos_score = h_matrix_wr[j];
            }
        } // end for every query base
    } // end for every ref base

    std::queue<int> BT_states;
    int i_curr=ref_pos, j_curr=query_pos;
    int i_steps = 0, j_steps = 0;

    int open = 0;
    if (first) {
        i_curr = max_i;
        j_curr = max_j;
        BT_states.push(max_score);
        BT_states.push(i_curr);
        BT_states.push(j_curr);
    }
    else {
        BT_states.push(pos_score);
    }

    int state = dir_matrix[i_curr][j_curr] % 4;
    int i = 4;

    while (state != Z) {
        if ((i_steps >= early_terminate) || (j_steps >= early_terminate)) { // || (i_steps - j_steps > 30) || (i_steps - j_steps < -30)) {
            break;
        }
        BT_states.push(state);
        i++;
        if (state == M) {
            char t = dir_matrix[i_curr-1][j_curr-1];
            state = (dir_matrix[i_curr-1][j_curr-1] % 4);
            i_curr--;
            j_curr--;
            i_steps++;
            j_steps++;
        }
        else if (state == I) {
            char t = dir_matrix[i_curr][j_curr];
            state = (dir_matrix[i_curr][j_curr] & (2 << INSERT_OP)) ? M : I;
            i_curr--;
            i_steps++;
        }
        else if (state == D) {
            char t = dir_matrix[i_curr][j_curr];
            state = (dir_matrix[i_curr][j_curr] & (2 << DELETE_OP)) ? M : D;
            j_curr--;
            j_steps++;
        }
    }; // end while

    return BT_states;
}


