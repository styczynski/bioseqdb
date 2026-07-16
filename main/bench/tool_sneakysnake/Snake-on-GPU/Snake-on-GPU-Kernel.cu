/*
 * Copyright (c) <2017 - 2030>, ETH Zurich and Bilkent University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this list
 * of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or other
 *   materials provided with the distribution.
 * - Neither the names of the ETH Zurich, Bilkent University,
 *   nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without specific
 *   prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  Authors: 
  Mohammed Alser
	  mohammed.alser AT inf DOT ethz DOT ch
  Date:
  September 22nd, 2019
*/



#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <cuda.h>
#include <cuda_runtime.h>
#include "stdio.h"
#include <time.h>
#include <unistd.h>
//#include "cuPrintf.cu"

//Your reads and refs should be files with size of 4 * 32 * 512 = 65536 lines
// /usr/local/cuda-10.1/bin/nvcc -D Nuints=8 -o Snake-on-GPU Snake-on-GPU.cu
//./Snake-on-GPU `head -n 1 ../Datasets/ERR240727_1_E2_30million.txt | awk '{print length($1)}'`  ../Datasets/ERR240727_1_E2_30million.txt  `wc -l  ../Datasets/ERR240727_1_E2_30million.txt|awk '{print $1}'`


#define warp_size 32
#define SharedPartDevice 64
#define FULL_MASK 0xffffffff
//#define NBytes 8
#define NBytes Nuints
#define PRINT 0

#define Number_of_Diagonals 9 //2*e+1
//#define F_ErrorThreshold 10
#define F_ReadLength 100

// #define NBytes 8

#define BitVal(data,y) ( (data>>y) & 1)      /** Return Data.Y value   **/
#define SetBit(data,y)    data |= (1 << y)    /** Set Data.Y   to 1    **/

// __device__ int popcount(int v) {
//     v = v - ((v >> 1) & 0x55555555);                // put count of each 2 bits into those 2 bits
//     v = (v & 0x33333333) + ((v >> 2) & 0x33333333); // put count of each 4 bits into those 4 bits
//     return ((v + (v >> 4) & 0xF0F0F0F) * 0x1010101) >> 24;
// }
// int popcount_Host(int v) {
//     v = v - ((v >> 1) & 0x55555555);                // put count of each 2 bits into those 2 bits
//     v = (v & 0x33333333) + ((v >> 2) & 0x33333333); // put count of each 4 bits into those 4 bits
//     return ((v + (v >> 4) & 0xF0F0F0F) * 0x1010101) >> 24;
// }

//__global__ void SneakySnake(uint* F_ReadSeq, uint* F_RefSeq, uint* Ftest_ReadSeq, uint* Ftest_RefSeq, int F_Number_of_Uints_inside_each_read, int F_Number_of_Reads_inside_each_cacheline, int F_Number_of_warps_inside_each_block,  int F_Number_of_blocks_inside_each_kernel, int* F_Results, int* Ftest_Results, int F_ErrorThreshold)
__global__ void SneakySnake(uint* F_ReadSeq, uint* F_RefSeq, int F_Number_of_Uints_inside_each_read, int F_Number_of_Reads_inside_each_cacheline, int F_Number_of_warps_inside_each_block,  int F_Number_of_blocks_inside_each_kernel, int* Ftest_Results, int F_ErrorThreshold, int NumReads2)
{

	// __shared__ uint SharedMemRefSeq[SharedPartDevice];

	int tid = threadIdx.x + blockIdx.x * blockDim.x;
	if(tid>=NumReads2)
		return;
	//printf("%d",tid);

  // const int NBytes = 8;
  uint ReadsPerThread[NBytes];
  uint RefsPerThread[NBytes];

  #pragma unroll
  for (int i = 0; i < NBytes; i++)
  {
      ReadsPerThread[i] = F_ReadSeq[tid*8 + i];
      RefsPerThread[i] = F_RefSeq[tid*8 + i];

#if PRINT
			if(tid == 0)
			{
				printf("Read=%08x\t Ref=%08x\n", ReadsPerThread[i], RefsPerThread[i] );
			}
#endif

  }


////////////////////////////////////// Test: Reading from global memory to a reg, write back to global mem /////////////////////////////
  // uint readedReadSeq;
	// readedReadSeq = F_ReadSeq[tid];
	// Ftest_ReadSeq[tid] = readedReadSeq;
  //
  // uint readedRefSeq;
  // readedRefSeq = F_RefSeq[tid];
  // Ftest_RefSeq[tid] = readedRefSeq;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////



  // if ((tid%1024) < SharedPartDevice)
	// {
	// 	SharedMemRefSeq[(tid%1024)] = F_RefSeq[(tid%1024)];
	// 	Ftest_RefSeq[(tid%1024)] = SharedMemRefSeq[(tid%1024)];
	// }

	// __syncthreads();


  /////////////////////////////////////////////////////////////////////////////
  Ftest_Results[tid] = 1;

  uint ReadCompTmp = 0x00000000;
  uint RefCompTmp = 0x00000000;
  uint DiagonalResult = 0x00000000;

  uint ReadTmp1 = 0x00000000;
  uint ReadTmp2 = 0x00000000;

  uint RefTmp1 = 0x00000000;
  uint RefTmp2 = 0x00000000;

  uint CornerCase = 0x00000000;

  //int localCounter[(2*F_ErrorThreshold)+1] = {0};
  int localCounter= 0;
  int localCounterMax=0;
  // int globalCounter_ref = 0; //just to find out which one of 8 uints we are using.
  int globalCounter = 0;

  int Max_leading_zeros = 0;

  // int Is_Edit = 0;
  // int Which_diag = 0;
  //int localErr = 0;
  int AccumulatedErrs = 0;

  // int ShiftValue_2Ref = 0;
  int ShiftValue = 0;
  // int ShiftCorrection = 0;

  int Diagonal = 0;

  // uint tmpEdits = 0x00000000;

  // int j_ref = 0;
  int j = 0; //specifying the j-th uint that we are reading in each read-ref comparison (can be from 0 to 7)

  // int TmpPrint = 0;

  while ( (j < 7) && (globalCounter < 200))
  {

#if PRINT
          if(tid == 0)
          {
            printf("#############\n");
          }
#endif

    Diagonal = 0;


    RefTmp1 = RefsPerThread[j] << ShiftValue; //SharedMemRefSeq[ ( ((tid%1024)%8) * 8) + j_ref] << ShiftValue_2Ref;
    RefTmp2 = RefsPerThread[j + 1] >>  32 - ShiftValue; // SharedMemRefSeq[ ( ((tid%1024)%8) * 8) + j_ref+1] >>  32 - ShiftValue_2Ref;

#if PRINT
          if(tid == 0)
          {
            printf("#Refs:\n");

            printf("RefNSh=%08x \t Sh=%d \t FTmp1=%08x\n", RefsPerThread[j], ShiftValue, RefTmp1);

            printf("RefNSh=%08x \t Sh=%d \t FTmp2=%08x\n\n", RefsPerThread[j + 1], 32 - ShiftValue, RefTmp2);
          }
#endif

    ReadTmp1 = ReadsPerThread[j] << ShiftValue;
    ReadTmp2 = ReadsPerThread[j + 1] >>  32 - ShiftValue;

#if PRINT
          if(tid == 0)
          {
            printf("#Main: j=%d\n", j);

            printf("NShJ=%08x \t Sh=%d \t RTmp1=%08x\n", ReadsPerThread[j], ShiftValue, ReadTmp1);

            printf("NShJ+1=%08x \t Sh=%d \t RTmp2=%08x\n", ReadsPerThread[j+1], 32 - ShiftValue, ReadTmp2);
          }
#endif

    ReadCompTmp = ReadTmp1 | ReadTmp2;
    RefCompTmp = RefTmp1 | RefTmp2;
    DiagonalResult = ReadCompTmp ^ RefCompTmp;
   // localCounter[Diagonal] = __clz(DiagonalResult);
	localCounterMax = __clz(DiagonalResult);

#if PRINT
			    if(tid == 0)
          {
            printf("RC=%08x \nFC=%08x \nDR=%08x \nD=%d \tLC[%d]=%d\n\n", ReadCompTmp, RefCompTmp, DiagonalResult, Diagonal, Diagonal, localCounterMax);
          }
#endif

//////////////////// Upper diagonals /////////////////////

#if PRINT
          if(tid == 0)
          {
              printf("#upper\n");
          }
#endif

    for(int e = 1; e <= F_ErrorThreshold; e++)
    {
      Diagonal += 1;
      CornerCase = 0x00000000;
      if (  (j == 0)  &&  (  (ShiftValue - (2*e))  < 0 )  )
      {
        ReadTmp1 = ReadsPerThread[j] >> ( (2*e) - ShiftValue );
        ReadTmp2 = 0x00000000;

        ReadCompTmp = ReadTmp1 | ReadTmp2;
        RefCompTmp = RefTmp1 | RefTmp2;

        DiagonalResult = ReadCompTmp ^ RefCompTmp;

        CornerCase = 0x00000000;
        for(int Ci = 0; Ci < (2*e) - ShiftValue; Ci++)
        {
            SetBit(CornerCase, 31 - Ci);
        }

				DiagonalResult  = DiagonalResult | CornerCase;
        localCounter = __clz(DiagonalResult);

      }
      else if ( (ShiftValue - (2*e) ) < 0 )
      {
        ReadTmp1 = ReadsPerThread[j-1] << 32 - ( (2*e) - ShiftValue );
        ReadTmp2 = ReadsPerThread[j] >> (2*e) - ShiftValue;

        ReadCompTmp = ReadTmp1 | ReadTmp2;
        RefCompTmp = RefTmp1 | RefTmp2;

        DiagonalResult = ReadCompTmp ^ RefCompTmp;

        localCounter = __clz(DiagonalResult);

      }
      else
      {
        ReadTmp1 = ReadsPerThread[j] <<  ShiftValue - (2*e);
        ReadTmp2 = ReadsPerThread[j+1] >> 32 - (ShiftValue - (2*e) ) ;

        ReadCompTmp = ReadTmp1 | ReadTmp2;
        RefCompTmp = RefTmp1 | RefTmp2;

        DiagonalResult = ReadCompTmp ^ RefCompTmp;

        localCounter = __clz(DiagonalResult);

      }
		if (localCounter>localCounterMax)
			localCounterMax=localCounter;
#if PRINT
			if(tid == 0)
			{
					printf("j=%d, e=%d, sh=%d\n", j, e, ShiftValue - (2*e));
					printf("NShJ=%08x \t RTmp1=%08x\n", ReadsPerThread[j], ReadTmp1);
					printf("NShJ+1=%08x \t RTmp2=%08x\n", ReadsPerThread[j+1], ReadTmp2);
					printf("CC=%08x\n", CornerCase);
					printf("RC=%08x \nFC=%08x \nDR=%08x \nDN=%d \t LC[%d] = %d\n\n", ReadCompTmp, RefCompTmp, DiagonalResult, Diagonal, Diagonal, localCounter);
					printf("\n");
			}
#endif

    }


/*
    sh = shift
    up = upper diagonal
    RC = ReadCompTmp
    FC = RefCompTmp
    D = DiagonalResult
    DN = diagonal
    LC = localCounter
*/

//////////////////// Lower diagonals /////////////////////

#if PRINT
		        if(tid == 0)
            {
                printf("#lower\n");
            }
#endif

    for(int e = 1; e <= F_ErrorThreshold; e++)
    {
		Diagonal += 1;
		CornerCase = 0x00000000;
		if ( j<5)//  ( (globalCounter + ShiftValue + (2*e) + 32) < 200) )
		{
			//printf("HI1");
			if ( (ShiftValue + (2*e) )  < 32)
			{
			  ReadTmp1 = ReadsPerThread[j] << ShiftValue + (2*e);
			  ReadTmp2 = ReadsPerThread[j+1] >> 32 - ( ShiftValue + (2*e) );
						// ReadTmp2 = 0x00000000;

						ReadCompTmp = ReadTmp1 | ReadTmp2;
						RefCompTmp = RefTmp1 | RefTmp2;

						DiagonalResult = ReadCompTmp ^ RefCompTmp;
			  localCounter = __clz(DiagonalResult);

			}
			else
			{
				//printf("HI2");
			  ReadTmp1 = ReadsPerThread[j+1] << ( ShiftValue + (2*e) ) % 32;
			  ReadTmp2 = ReadsPerThread[j+2] >>  32 - ( ( ShiftValue + (2*e) ) % 32 );

			  ReadCompTmp = ReadTmp1 | ReadTmp2;
			  RefCompTmp = RefTmp1 | RefTmp2;

			  DiagonalResult = 0xffffffff;//ReadCompTmp ^ RefCompTmp;

			  DiagonalResult = ReadCompTmp ^ RefCompTmp;

			  localCounter = __clz(DiagonalResult);
			}
		}
		else
		{
			//printf("HI3");
			ReadTmp1 = ReadsPerThread[j] << ShiftValue + (2*e);
			ReadTmp2 = ReadsPerThread[j+1] >>   32 - ( ShiftValue + (2*e) );

			ReadCompTmp = ReadTmp1 | ReadTmp2;
			RefCompTmp = RefTmp1 | RefTmp2;
			DiagonalResult = ReadCompTmp ^ RefCompTmp;

			CornerCase = 0x00000000;
			if ((globalCounter+32)>200 ) {
			
				for(int Ci = ((globalCounter+32)-200); Ci < (((globalCounter+32)-200)+ 2*e); Ci++)
				{
				  SetBit(CornerCase, Ci);
				}
			}

			else if ((globalCounter+32)>=(200- (2*e))){
			
				for(int Ci = 0; Ci < (2*e); Ci++)
				{
				  SetBit(CornerCase, Ci);
				}
			}
			DiagonalResult = DiagonalResult | CornerCase;
			
			localCounter = __clz(DiagonalResult);
      }
	  
	  if (localCounter>localCounterMax)
			localCounterMax=localCounter;

#if PRINT
			if(tid == 0)
			{
				// printf("aaaaaaaaaa\n");
				printf("j=%d, e=%d, Sh=%d\n", j, e, ShiftValue + (2*e));
				printf("NShJ=%08x \t RTmp1=%08x\n", ReadsPerThread[j], ReadTmp1);
				printf("NShJ+1=%08x \t RTmp2=%08x\n", ReadsPerThread[j+1], ReadTmp2);
				printf("CC=%08x\n", CornerCase);
				printf("RC=%08x \nFC=%08x \nDR=%08x \nDN=%d \t LC[%d] = %d\n\n", ReadCompTmp, RefCompTmp, DiagonalResult, Diagonal, Diagonal, localCounter);
				printf("\n");
			}
#endif

    }

    /*
    CC = CornerCase
        sh = shift
        up = upper diagonal
        RC = ReadCompTmp
        FC = RefCompTmp
        D = DiagonalResult
        DN = diagonal
        LC = localCounter
    */

    Max_leading_zeros = 0;
    // Is_Edit = 0;
    // Which_diag = 0;


	if ( (j == 6) && ( ((localCounterMax/2)*2) >= 8)  )
	{
		Max_leading_zeros = 8;
		// if ( (Max_leading_zeros != 8) && (tmp > 0) )
		// {
		//     Is_Edit = 1;
		//     Which_diag = tmp;
		// }
		break;
	}
	else if( ((localCounterMax/2)*2) > Max_leading_zeros)
	{
		Max_leading_zeros = ((localCounterMax/2)*2);
		// if (tmp > 0)
		// {
		//     Is_Edit = 1;
		//     Which_diag = tmp;
		// }
	}
    

    // int Considered_Edits = 0;
		//
    // if ( (Is_Edit == 1) && (Which_diag <= (Number_of_Diagonals/2) ) )
    // {
    //   Considered_Edits = Which_diag;
    // }
    // else if(Is_Edit == 1)
    // {
    //   Considered_Edits = Which_diag - (Number_of_Diagonals/2);
    // }


    if ( ( (Max_leading_zeros/2) < 16) && (j < 5) )
    {
      AccumulatedErrs += 1;
    }
    else if (  (j == 6) && ( (Max_leading_zeros/2) < 4) )
    {
      AccumulatedErrs += 1;
    }
    // else if ( ( (Max_leading_zeros/2) == 16) && (Is_Edit == 1) && (j < 5) )
    // {
    //   AccumulatedErrs += 1;
    // }

#if PRINT
            if(tid == 0)
            {
              printf("Diag finished\n");
							printf("Max=%d \n AccErr=%d\n", Max_leading_zeros, AccumulatedErrs);
              // printf("Is_Edit=%d \t Wh=%d \t Max=%d \n AccErr=%d\n", Is_Edit, Which_diag, Max_leading_zeros, AccumulatedErrs);
            }
#endif

    if(AccumulatedErrs > F_ErrorThreshold)
    {
      Ftest_Results[tid] = 0;
      break;
    }


    if(ShiftValue + Max_leading_zeros + 2 >= 32)
    {
      j += 1;
      // j_ref += 1;
    }

    // ShiftValue_2Ref = (ShiftValue_2Ref + Max_leading_zeros + 2) %32;
    if (Max_leading_zeros == 32)
    {
			globalCounter += Max_leading_zeros;
    }
    else
    {
        ShiftValue = ((ShiftValue + Max_leading_zeros + 2) % 32);
				globalCounter += (Max_leading_zeros + 2);
    }



#if PRINT
            if(tid == 0)
            {
              printf("GC=%d\n", globalCounter);
              printf("Expected shift = %d\n\n\n", ShiftValue);
            }
#endif

  }
// __syncthreads();
}
