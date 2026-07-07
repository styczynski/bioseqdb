#include "tb_kernel.h"
#include "minimap.h"

#define KSW_CIGAR_MATCH  0
#define KSW_CIGAR_INS    1
#define KSW_CIGAR_DEL    2
#define KSW_CIGAR_N_SKIP 3

__device__
static inline void push_cigar(int *n_cigar, uint32_t *cigar, uint32_t op, int len)
{
	if (*n_cigar == 0 || op != (cigar[(*n_cigar) - 1]&0xf)) {
		cigar[(*n_cigar)++] = len<<4 | op;
	} else cigar[(*n_cigar)-1] += len<<4;
	return;
}

__global__ void traceback_kernel_gridded(uint8_t *unpacked_query_batch, uint8_t *unpacked_target_batch,  uint32_t *query_batch_lens, uint32_t *target_batch_lens, uint32_t *query_batch_offsets, uint32_t *target_batch_offsets, uint32_t* cigar, gasal_res_t *device_res, int n_tasks, uint32_t maximum_sequence_length, short2 *dblock_row, short2 *dblock_col, uint64_t* tb_offset, int* n_cigar, int bw) {
	int i, j;
	int d_row, d_col;
	int inner_row, inner_col;
	const uint32_t tid = (blockIdx.x * blockDim.x) + threadIdx.x;
	if (tid >= n_tasks) return;
	if (query_batch_lens[tid]<=0 && target_batch_lens[tid]<=0) return; 

	if(query_batch_lens[tid]>11000 || target_batch_lens[tid]>11000) {
		//printf("[%d]qlen %d tlen %d tb ofs %lu\n", tid, query_batch_lens[tid], target_batch_lens[tid], tb_offset[tid]);
		return;
	}

	int l, k, l2, k2;
	const short2 initHD = make_short2(MINUS_INF2, MINUS_INF2);

	int q_len = query_batch_lens[tid];
	int t_len = target_batch_lens[tid];
	i = target_batch_lens[tid]-1;
	j = query_batch_lens[tid]-1;

	if(q_len<=0 || t_len<= 0){n_cigar[tid]=0; return;}

	d_row = i/DBLOCK_SIZE_D;
	d_col = j/DBLOCK_SIZE_D;
	inner_row = i%DBLOCK_SIZE_D;
	inner_col = j%DBLOCK_SIZE_D;

	extern __shared__ short3 sharedMemory[];
	short3 *scoreMatrixRow = sharedMemory+DBLOCK_SIZE_D*threadIdx.x;
	uint8_t directionMatrix[DBLOCK_SIZE_D*DBLOCK_SIZE_D];
	
	short3 tempLeftCell;
	short3 tempDiagCell;

	int query_matched_idx = 0;
	int target_matched_idx = 0;

	int x, y;
	int direction;
	short subScore, tmpScore;
	short3 left, up, diag;

	short3 entry;
	
	uint64_t dblock_offset = tb_offset[tid];

	int dp_mtx_len = maximum_sequence_length;
	int* n = &n_cigar[tid]; *n = 0;
	int state = 0; // for minimap2 heuristics

	while (d_row >= 0 && d_col >= 0) {
		
		if(d_row == 0 && d_col == 0){
			tempDiagCell.z = 0;
		} else if(d_row == 0){
			l = d_col * DBLOCK_SIZE_D - 1;
			k = -(_cudaGapOE + (_cudaGapExtend*(l)));
			tempDiagCell.z = k;
			
		} else if(d_col == 0){
			l = d_row * DBLOCK_SIZE_D - 1;
			k = -(_cudaGapOE + (_cudaGapExtend*(l)));
			tempDiagCell.z = k;
		} else {
			tempDiagCell.z = dblock_row[dblock_offset + q_len*d_row + d_col*DBLOCK_SIZE_D - 1].x;
		}
		for (x = 0; x < DBLOCK_SIZE_D; x++) {
			if (d_row == 0) {
				// init as forward path
				l = d_col * DBLOCK_SIZE_D + x;
				k = -(_cudaGapOE + (_cudaGapExtend*(l)));
				scoreMatrixRow[x].x = k-_cudaGapOE;
				scoreMatrixRow[x].z = k;
			} else {
				scoreMatrixRow[x].x = dblock_row[dblock_offset + q_len*d_row + d_col*DBLOCK_SIZE_D + x].y;
				scoreMatrixRow[x].z = dblock_row[dblock_offset + q_len*d_row + d_col*DBLOCK_SIZE_D + x].x;
			}
		}

		
		// init first cell of col
		if (d_col == 0) {
			l = d_row * DBLOCK_SIZE_D;
			k = -(_cudaGapOE + (_cudaGapExtend*(l)));
			tempLeftCell.y = k-_cudaGapOE;
			tempLeftCell.z = k;
		} else {
			tempLeftCell.y = dblock_col[dblock_offset + t_len*d_col + d_row*DBLOCK_SIZE_D].y; 
			tempLeftCell.z = dblock_col[dblock_offset + t_len*d_col + d_row*DBLOCK_SIZE_D].x; 
		}

		// fill dynamic block
		for (x = 0; x <= inner_row; x++) {
			for (y = 0; y <= inner_col; y++) {
				left = tempLeftCell;
				up = scoreMatrixRow[y];
				diag = tempDiagCell;

				GET_SUB_SCORE(subScore, unpacked_query_batch[query_batch_offsets[tid] + DBLOCK_SIZE_D*d_col + y], unpacked_target_batch[target_batch_offsets[tid] + DBLOCK_SIZE_D*d_row + x]);
				tmpScore = diag.z + subScore;

				entry.x = up.x; // E up.z = prev. diag score, up.x = up e score
				entry.y = left.y; // F left.x = prev. diag score, left.y = left f score

				if (tmpScore < max(entry.x, entry.y)) {
					if (entry.x < entry.y) {
						directionMatrix[DBLOCK_SIZE_D*x + y] = 3; // from left cell
						entry.z = entry.y;
					} else {
						directionMatrix[DBLOCK_SIZE_D*x + y] = 2; // from upper cell
						entry.z = entry.x;
					}
				} else {
					directionMatrix[DBLOCK_SIZE_D*x + y] = 0;
					// entry.z = max(0, tmpScore); // from diagonal cell
					entry.z = tmpScore; // from diagonal cell
				}

				// set upper 2 bits -> is it a continuation of a gap?
				directionMatrix[DBLOCK_SIZE_D*x + y] |= (tmpScore -  _cudaGapOE >= entry.x - _cudaGapExtend)? 0:1<<2;
				directionMatrix[DBLOCK_SIZE_D*x + y] |= (tmpScore -  _cudaGapOE >= entry.y - _cudaGapExtend)? 0:1<<3;

				// 	// new
				entry.x = max(up.x - _cudaGapExtend, tmpScore - _cudaGapOE); // E up.z = prev. diag score, up.x = up e score
				entry.y = max(left.y - _cudaGapExtend, tmpScore - _cudaGapOE); // F left.x = prev. diag score, left.y = left f score

				tempLeftCell = entry;
				tempDiagCell = scoreMatrixRow[y];
				scoreMatrixRow[y] = entry;
			}
			
			if (d_col == 0) {

				l = d_row * DBLOCK_SIZE_D + x + 1;
				k = -(_cudaGapOE + (_cudaGapExtend*(l)));
				tempLeftCell.y = k-_cudaGapOE;
				tempLeftCell.z = k;

				l = d_row * DBLOCK_SIZE_D + x;
				k = -(_cudaGapOE + (_cudaGapExtend*(l)));
				tempDiagCell.z = k;

			} else {

				tempLeftCell.y = dblock_col[dblock_offset + t_len*d_col + d_row*DBLOCK_SIZE_D + x + 1].y;
				tempLeftCell.z = dblock_col[dblock_offset + t_len*d_col + d_row*DBLOCK_SIZE_D + x + 1].x;

				tempDiagCell.y = dblock_col[dblock_offset + t_len*d_col + d_row*DBLOCK_SIZE_D + x].y;
				tempDiagCell.z = dblock_col[dblock_offset + t_len*d_col + d_row*DBLOCK_SIZE_D + x].x;
			}
		}


		/* traceback within the grid */
		while (inner_row >= 0 && inner_col >= 0) {
			direction = directionMatrix[DBLOCK_SIZE_D*inner_row + inner_col];

			int force_state = -1;
			// cells outside the bandwidth 
			if(i > j + bw) force_state = 2; 
			if(i < j - bw) force_state = 3; 

			direction = force_state < 0? direction : 0;

			// for 1-stage gap cost
			if(state<=1) state = direction & 3;  // if requesting the H state, find state one maximizes it.
			else if(!(direction >> (state) & 1)) state = 0; // if requesting other states, _state_ stays the same if it is a continuation; otherwise, set to H

			if(state<=1) state = direction & 3;  
			if (force_state >= 0) state = force_state; 

			switch(state) {
				case 0: // matched
				case 1: // mismatched
					push_cigar(n, &cigar[query_batch_offsets[tid]], KSW_CIGAR_MATCH, 1);
					i--;
					j--;
					inner_row--;
					inner_col--;
				break;
				case 2: // from upper cell
					push_cigar(n, &cigar[query_batch_offsets[tid]], KSW_CIGAR_DEL, 1);
					i--;
					inner_row--;
				break;
				case 3: // from left cell
					push_cigar(n, &cigar[query_batch_offsets[tid]], KSW_CIGAR_INS, 1);
					j--;
					inner_col--;
				break;
			}
		}

		if (inner_row < 0) {
			if (inner_col < 0) { // diag dblock
				d_row--;
				d_col--;
				inner_row = DBLOCK_SIZE_D-1;
				inner_col = DBLOCK_SIZE_D-1;
			} else { // upper dblock
				d_row--;
				inner_row = DBLOCK_SIZE_D-1;
			}
		} else { // left dblock
			d_col--;
			inner_col = DBLOCK_SIZE_D-1;
		}
	}
	if (i >= 0) push_cigar(n, &cigar[query_batch_offsets[tid]], KSW_CIGAR_DEL, i + 1); // first deletion
	if (j >= 0) push_cigar(n, &cigar[query_batch_offsets[tid]], KSW_CIGAR_INS, j + 1); // first insertion

	uint32_t tmp;
	for (i = 0; i < (*n)>>1; ++i) { // reverse CIGAR
			tmp = cigar[query_batch_offsets[tid] + i];  // Store the original value
			cigar[query_batch_offsets[tid] + i] = cigar[query_batch_offsets[tid] + *n - 1 - i];  // Assign swapped value
			cigar[query_batch_offsets[tid] + *n - 1 - i] = tmp;  // Complete the swap
	}
}	
