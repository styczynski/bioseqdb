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


/* Include Files */
#include "SneakySnake.h"
#include "stdio.h" 


/* Function Definitions */

/*
 * Arguments    : int ReadLength
 *                const char RefSeq[ReadLength]
 *                const char ReadSeq[ReadLength]
 *                int EditThreshold
 *                int KmerSize (t)
 *                int DebugMode
 *				  int IterationNo (y)
 * Return Type  : int Accepted
 */
int SneakySnake(int ReadLength, char * RefSeq, char * ReadSeq, int EditThreshold, int KmerSize, int DebugMode, int IterationNo)
{
	int Accepted=1;
	int n;
	int e;
	int K;
	int index=0;
	int Edits=0;
	int count = 0;
	int GlobalCount=0;
	
	int KmerStart=0;
	int KmerEnd=0;
	
	int roundsNo=0;

	if (DebugMode){
		printf("%2d - ", 0);
		for (n = 0; n < (ReadLength) ; n++) {
			if (n%10==0)
				printf("_%2d       ",n);
		}
		printf("\n");
		// Main Diagonal
		////////////////////////////////////////////
		printf("%2d - ", 0);
		for (n = 0; n < (ReadLength) ; n++) {
			if (ReadSeq[n]!= RefSeq[n]) {
				printf("1");
			}
			else if (ReadSeq[n]== RefSeq[n]) {
				printf("0");
			}
		}
		printf("\n");
		////////////////////////////////////////////
		//  Upper & Lower Diagonals
		////////////////////////////////////////////
		for (e = 1; e <= EditThreshold; e++) {
			count=0;
			///////////////////////////////////////////////////
			//  Shift Read to Right-hand side (Upper Diagonals: Deletion)
			printf("%2d - ",e);
			for (n = 0; n < (ReadLength) ; n++) {
				if (n<e) 
					printf("1");
				else if (ReadSeq[n-e]!= RefSeq[n])
					printf("1");
				else if (ReadSeq[n-e]== RefSeq[n]) {
					printf("0");
				}
			}
			printf("\n");
			///////////////////////////////////////////////////
			//  Shift Read to Left-hand side (Lower Diagonals: Insertion)
			printf("%2d - ",e+EditThreshold);
			for (n =0; n < (ReadLength) ; n++) {
				if (n>ReadLength-e-1) 
					printf("1");
				else if (ReadSeq[n+e]!= RefSeq[n])
					printf("1");
				else if (ReadSeq[n+e]== RefSeq[n]) {
					printf("0");
				}					
			}
			printf("\n");
			
		}
	}
	else{
		// Go through each Kmer
		for(K=0; K < (ReadLength/KmerSize); K++){
			KmerStart= K*KmerSize;
			if (K < (ReadLength/KmerSize)-1)
				KmerEnd = (K+1)*KmerSize;
			else
				KmerEnd = ReadLength;
			
			index=(KmerStart);
			roundsNo=1;
			
			while (index<KmerEnd) {
				GlobalCount=0;
		
				// Main Diagonal
				////////////////////////////////////////////
				for (n = (index); n < (KmerEnd) ; n++) {
					if (ReadSeq[n]!= RefSeq[n]) {
						goto EXIT1;
					}
					else if (ReadSeq[n]== RefSeq[n]) {
						GlobalCount=GlobalCount+1;
					}
				}
				EXIT1:
				if (GlobalCount ==(KmerEnd-KmerStart))
					goto LOOP;
				
				////////////////////////////////////////////
				//  Upper & Lower Diagonals
				////////////////////////////////////////////
				for (e = 1; e <= EditThreshold; e++) {
					count=0;
					///////////////////////////////////////////////////
					//  Shift Read to Right-hand side (Upper Diagonals: Deletion)
					for (n = (index); n < (KmerEnd) ; n++) {
						if (n<e) 
							goto EXIT2; // fill the shifted chars with Ones
						else if (ReadSeq[n-e]!= RefSeq[n])
							goto EXIT2;
						else if (ReadSeq[n-e]== RefSeq[n]) {
							count=count+1;
						}
					}
					EXIT2:
					if (count>GlobalCount) {
						GlobalCount = count;
					}
					if (count ==(KmerEnd-KmerStart))
						goto LOOP;
					
					count=0;
					///////////////////////////////////////////////////
					//  Shift Read to Left-hand side (Lower Diagonals: Insertion)
					for (n = (index); n < (KmerEnd) ; n++) {
						if (n>ReadLength-e-1) 
							goto EXIT3;
						else if (ReadSeq[n+e]!= RefSeq[n])
							goto EXIT3;
						else if (ReadSeq[n+e]== RefSeq[n]) {
							count=count+1;
						}					
					}
					EXIT3:
					if (count>GlobalCount) {
						GlobalCount = count;
					}
					if (count ==(KmerEnd-KmerStart))
						goto LOOP;
				}
					
				//if (DebugMode==1) 
					//printf("GlobalCount: %d, index: %d \n", GlobalCount,index);
				index = index+GlobalCount; // we add one here to skip the error that causes the segmentation
				if (index<(KmerEnd)) {
					Edits=Edits+1;
					index=index+1;
				}
				if (roundsNo>IterationNo)
					goto LOOP;
				roundsNo=roundsNo+1;
				if (Edits > EditThreshold)
					return 0;
				
				////////////////////////////////////////////
				// END of Building the Hamming masks
				/////////////////////////////////////////////////
				/////////////////////////////////////////////////
				//// if not sure about the number of matches, try finding the best PATH within a Kmer

			}
			////////////////////////////////////////////////
			////////////////////////////////////////////////
			LOOP:
			/* Backtracking Step
			PrvSource    Source      # Edits
			2----------> 2-1-0-3-4   0-1-2-3-4 
			1----------> 2-1-0-3-4   1-0-1-2-3 
			0----------> 2-1-0-3-4   2-1-0-1-2   
			3----------> 2-1-0-3-4   3-2-1-0-1 
			4----------> 2-1-0-3-4   4-3-2-1-0 
			
			if (PreviousSource==0) {
				if (Source>EditThreshold) 
					Edits=abs(Source-EditThreshold);
				else 
					Edits=Source;
			}
			else if (PreviousSource<=EditThreshold && Source<=EditThreshold)
				Edits=abs(Source-PreviousSource);
			else if (PreviousSource<=EditThreshold && Source>EditThreshold)
				Edits= PreviousSource+ abs(Source-EditThreshold);
			else if (PreviousSource>EditThreshold && Source<=EditThreshold)
				Edits= Source + abs(PreviousSource-EditThreshold);
			else if (PreviousSource>EditThreshold && Source>EditThreshold)
				Edits=abs(Source-PreviousSource);
			GlobalCount=(Edits>(KmerSize-GlobalCount))?(KmerSize-abs(Edits-(KmerSize-GlobalCount))):GlobalCount;
			*/
			/////////////////////////////////////////////////
			
			//GlobalCount=GlobalCount +Edits;
			/*if (DebugMode==1) {
				printf("Global Count: %d, Edits: %d\n",GlobalCount,Edits);
			}*/
			if (Edits > EditThreshold)
				return 0;	
		}
	}
	////////////////////////////////////////////////
	////////////////////////////////////////////////
	////////////////////////////////////////////////
	////////////////////////////////////////////////
	////////////////////////////////////////////////
	
	return Accepted;
}
