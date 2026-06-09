#include "pack_rc_seqs.h"

__global__ void gasal_pack_kernel(uint32_t* unpacked_query_batch, uint32_t* unpacked_target_batch, uint32_t *packed_query_batch, uint32_t* packed_target_batch, int query_batch_tasks_per_thread, int target_batch_tasks_per_thread, uint32_t total_query_batch_regs, uint32_t total_target_batch_regs) \
{

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

}


__global__ void	gasal_reversecomplement_kernel(uint32_t *packed_query_batch,uint32_t *packed_target_batch, uint32_t *query_batch_lens, uint32_t *target_batch_lens, uint32_t *query_batch_offsets, uint32_t *target_batch_offsets, uint8_t *query_op, uint8_t *target_op, uint32_t  n_tasks)
{

	const uint32_t tid = (blockIdx.x * blockDim.x) + threadIdx.x;//thread ID

	if (tid >= n_tasks) return;
	if (query_op[tid] == 0 && target_op[tid] == 0) return;		// if there's nothing to do (op=0, meaning sequence is Forward Natural), just exit the kernel ASAP. 


	uint32_t packed_target_batch_idx = target_batch_offsets[tid] >> 3;//starting index of the target_batch sequence
	uint32_t packed_query_batch_idx = query_batch_offsets[tid] >> 3;//starting index of the query_batch sequence
	uint32_t read_len = query_batch_lens[tid];
	uint32_t ref_len = target_batch_lens[tid];
	uint32_t query_batch_regs = (read_len >> 3) + (read_len&7 ? 1 : 0);//number of 32-bit words holding sequence of query_batch
	uint32_t target_batch_regs = (ref_len >> 3) + (ref_len&7 ? 1 : 0);//number of 32-bit words holding sequence of target_batch

	uint32_t query_batch_regs_to_swap = (query_batch_regs >> 1) + (query_batch_regs & 1); // that's (query_batch_regs / 2) + 1 if it's odd, + 0 otherwise. Used for reverse (we start a both ends, and finish at the center of the sequence)
	uint32_t target_batch_regs_to_swap = (target_batch_regs >> 1) + (target_batch_regs & 1); // that's (target_batch_regs / 2) + 1 if it's odd, + 0 otherwise. Used for reverse (we start a both ends, and finish at the center of the sequence)


	// variables used dependent on target and query: 

	uint8_t *op = NULL;
	uint32_t *packed_batch = NULL;
	uint32_t *batch_regs = NULL;
	uint32_t *batch_regs_to_swap = NULL;
	uint32_t *packed_batch_idx = NULL;

	// avoid useless code duplicate thanks to pointers to route the data flow where it should be, twice.
	// The kernel is already generic. Later on this can be used to split the kernel into two using templates...
	#pragma unroll 2
	for (int p = QUERY; p <= TARGET; p++)
	{
		switch(p)
		{
			case QUERY:
				op = query_op;
				packed_batch = packed_query_batch;
				batch_regs = &query_batch_regs;
				batch_regs_to_swap = &query_batch_regs_to_swap;
				packed_batch_idx = &packed_query_batch_idx;
				break;
			case TARGET:
				op = target_op;
				packed_batch = packed_target_batch;
				batch_regs = &target_batch_regs;
				batch_regs_to_swap = &target_batch_regs_to_swap;
				packed_batch_idx = &packed_target_batch_idx;
				break;
			default:
			break;
		}

		if (*(op + tid) & 0x01) // reverse
		{
			// deal with N's : read last word, find how many N's, store that number as offset, and pad with that many for the last 
			uint8_t nbr_N = 0;
			for (int j = 0; j < 32; j = j + 4)
			{
				nbr_N += (((*(packed_batch + *(packed_batch_idx) + *(batch_regs)-1) & (0x0F << j)) >> j) == N_CODE);
			}
			
			//printf("KERNEL_DEBUG: nbr_N=%d\n", nbr_N);


			nbr_N = nbr_N << 2; // we operate on nibbles so we will need to do our shifts 4 bits by 4 bits, so 4*nbr_N

			for (uint32_t i = 0; i < *(batch_regs_to_swap); i++) // reverse all words. There's a catch with the last word (in the middle of the sequence), see final if.
			{
				/* This  is the current operation flow:\
					- Read the first 32-bits word on HEAD
					- Combine the reads of 2 last 32-bits words on tail to create the 32-bits word WITHOUT N's 
					- Swap them 
					- Write them at the correct places. Remember we're building 32-bits words across two 32-bits words on tail. 
					So we have to take care of which bits are to be written on tail, too.

				You progress through both heads and tails that way, until you reach the center of the sequence. 
				When you reach it, you actually don't write one of the words to avoid overwrite.
				*/
				uint32_t rpac_1 = *(packed_batch + *(packed_batch_idx) + i); //load 8 packed bases from head
				uint32_t rpac_2 = ((*(packed_batch + *(packed_batch_idx) + *(batch_regs)-2 - i)) << (32-nbr_N)) | ((*(packed_batch + *(packed_batch_idx) + *(batch_regs)-1 - i)) >> nbr_N);


				uint32_t reverse_rpac_1 = 0;
				uint32_t reverse_rpac_2 = 0;


				#pragma unroll 8
				for(int k = 28; k >= 0; k = k - 4)		// reverse 32-bits word... is pragma-unrolled. 
				{
					reverse_rpac_1 |= ((rpac_1 & (0x0F << k)) >> (k)) << (28-k);
					reverse_rpac_2 |= ((rpac_2 & (0x0F << k)) >> (k)) << (28-k);
				}
				// last swap operated manually, because of its irregular size (32 - 4*nbr_N bits, hence 8 - nbr_N nibbles)


				uint32_t to_queue_1 = (reverse_rpac_1 << nbr_N) | ((*(packed_batch + *(packed_batch_idx) + *(batch_regs)-1 - i)) & ((1<<nbr_N) - 1));
				uint32_t to_queue_2 = ((*(packed_batch + *(packed_batch_idx) + *(batch_regs)-2 - i)) & (0xFFFFFFFF - ((1<<nbr_N) - 1))) | (reverse_rpac_1 >> (32-nbr_N));

			
				//printf("KERNEL DEBUG: rpac_1 Word before reverse: %x, after: %x, split into %x + %x \n", rpac_1, reverse_rpac_1, to_queue_2, to_queue_1 );
				//printf("KERNEL DEBUG: rpac_2 Word before reverse: %x, after: %x\n", rpac_2, reverse_rpac_2 );


				*(packed_batch + *(packed_batch_idx) + i) = reverse_rpac_2;
				(*(packed_batch + *(packed_batch_idx) + *(batch_regs)-1 - i)) = to_queue_1;
				if (i!=*(batch_regs_to_swap)-1)
					(*(packed_batch + *(packed_batch_idx) + *(batch_regs)-2 - i)) = to_queue_2;


			} // end for
		} // end if(reverse)

		if (*(op+tid) & 0x02) // complement
		{
			for (uint32_t i = 0; i < *(batch_regs); i++) // reverse all words. There's a catch with the last word (in the middle of the sequence), see final if.
			{
				uint32_t rpac = *(packed_batch + *(packed_batch_idx) + i); //load 8 packed bases from head
				uint32_t nucleotide = 0;

				#pragma unroll 8
				for(int k = 28; k >= 0; k = k - 4)		// complement 32-bits word... is pragma-unrolled. 
				{
					nucleotide = (rpac & (0x0F << k)) >> (k);
					switch(nucleotide)
					{
						case A_PAK:
							nucleotide = T_PAK;
							break;
						case C_PAK:
							nucleotide = G_PAK;
							break;
						case T_PAK:
							nucleotide = A_PAK;
							break;
						case G_PAK:
							nucleotide = C_PAK;
							break;
						default:
							break;
					}
					rpac = (rpac & (0xFFFFFFFF - (0x0F << k))) | nucleotide << k;
				}

				//printf("KERNEL DEBUG: Word read : %x, after complement: %x\n", *(packed_batch + *(packed_batch_idx) + i), rpac);

				*(packed_batch + *(packed_batch_idx) + i) = rpac;

			} // end for
		} // end if(complement)



	}

	return;
}
