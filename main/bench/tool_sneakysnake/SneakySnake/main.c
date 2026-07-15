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
#include "stdio.h" 
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <immintrin.h>
#include "SneakySnake.h"
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <sys/sysinfo.h>
#include <sys/time.h>



/* to compile type the following 
sudo ldconfig -v
gcc -g -O3 -Wall -o main *.c -lz -lm -pthread
time ./main 0 100 100 100 /home/alser/Desktop/Filters_29_11_2016/ERR240727_1_E2_30million.txt 30000000 1 10 0
OR: use the following to check the memory leaks
valgrind --leak-check=yes --show-leak-kinds=all ./main
*/


void kt_for(int n_threads, void (*func)(void*,long,int), void *data, long n);
//void kt_for(int n_threads, int n_items, void (*func)(void*,int,int), void *data);

//void kt_pipeline(int n_threads, void *(*func)(void*, int, void*), void *shared_data, int n_steps);

typedef struct {
	FILE *fp;
	int max_lines, buf_size, n_threads;
	int DebugMode,KmerSize, ReadLength,IterationNo, TotalAccepted, EditThreshold;
	char *buf;
} pipeline_t;

typedef struct {
	int n_lines;
	int DebugMode,KmerSize, ReadLength, IterationNo, EditThreshold;
	char **lines;
	int *Accepted;
} step_t;


static void worker_for(void *_data, long i, int tid) // kt_for() callback
{
	step_t *step = (step_t*)_data;
	char *s = step->lines[i];
	int ReadLength =step->ReadLength;
	int DebugMode =step->DebugMode;
	int KmerSize =step->KmerSize; 
	int IterationNo =step->IterationNo;
	int EditThreshold =step->EditThreshold;
	
	char RefSeq[ReadLength];
	char ReadSeq[ReadLength];
	
	int n=0;
	int banded=1;
	
	//printf("%s\n",step->lines[i]);
	
	strncpy(ReadSeq, s, ReadLength);
	strncpy(RefSeq, s+ReadLength+1, ReadLength);
	
	/*for (n = 0; n < ReadLength; n++) {
		printf("%c",ReadSeq[n]);
	}
	printf("\t");
	for (n = 0; n < ReadLength; n++) {
		printf("%c",RefSeq[n]);
	}
	printf("\n");
		*/
		

	
	step->Accepted[i] = SneakySnake(ReadLength, RefSeq, ReadSeq, EditThreshold, KmerSize, DebugMode, IterationNo);
	//printf("i: %d TID:%d Accepted: %d\n",i, tid, step->Accepted[i]);

}

int main(int argc, const char * const argv[]) {
	
	if (argc!=9){
		fprintf(stderr, "missing argument..\n./main [DebugMode] [KmerSize] [ReadLength] [IterationNo] [ReadRefFile] [# of reads] [# of threads] [EditThreshold]\n");
		exit(-1);
	}
	
	FILE * fp;
	char * line = NULL;
	size_t len = 0;
	ssize_t read;
	int i;
	int loopPar;
	int Accepted=0;
	long start, end;
    struct timeval timecheck;

	//printf("Edit Distance \t CPU Time(seconds) \t Alignment_Needed \t Not_Needed \n");
	
	int threads = atoi(argv[7]);
	
	printf("This system has %d processors configured and %d processors available.\n", get_nprocs_conf(), get_nprocs());
	
	fp = fopen(argv[5], "r");
	if (!fp){
		fprintf(stderr, "Sorry, the file does not exist or you do not have access permission\n");
	}
	else {
		
        if (threads==1) {
            int ReadLength = atoi(argv[3]);
            int DebugMode = atoi(argv[1]);
            int KmerSize = atoi(argv[2]);
            int IterationNo = atoi(argv[4]);
            int EditThreshold = atoi(argv[8]);
            int Accepted =0;
            char RefSeq[ReadLength];
            char ReadSeq[ReadLength];

            int n=0;
            int banded=1;

            //printf("%s\n",step->lines[i]);
            for (i = 0; i < atoi(argv[6]); i++) {
                read = getline(&line, &len, fp);
                char *s = strdup(line);
                strncpy(ReadSeq, s, ReadLength);
                strncpy(RefSeq, s+ReadLength+1, ReadLength);

                /*for (n = 0; n < ReadLength; n++) {
                printf("%c",ReadSeq[n]);
                }
                printf("\t");
                for (n = 0; n < ReadLength; n++) {
                printf("%c",RefSeq[n]);
                }
                printf("\n");
                */

				if (SneakySnake(ReadLength, RefSeq, ReadSeq, EditThreshold, KmerSize, DebugMode, IterationNo))
					Accepted++;
				//printf("i: %d TID:%d Accepted: %d\n",i, tid, step->Accepted[i]);
                    
            }
             printf("Data: %s\tThreads: %d\tE: %d\tAccepted: %d\tRejected:%d\n", argv[5], threads, EditThreshold, Accepted, atoi(argv[6])-Accepted);
        } else { //when threads >1
            step_t *s;
            s = calloc(1, sizeof(step_t));
            s->lines = calloc(atoi(argv[6]), sizeof(char*));
            s->Accepted = calloc(atoi(argv[6]), sizeof(int));
            s->DebugMode = atoi(argv[1]);
            s->KmerSize = atoi(argv[2]);
            s->ReadLength  = atoi(argv[3]);
            s->IterationNo  = atoi(argv[4]);
            s->EditThreshold = atoi(argv[8]);

            for (i = 0; i < atoi(argv[6]); i++) {
                read = getline(&line, &len, fp);
                //printf("%s",line);
                s->lines[s->n_lines] = strdup(line);
                s->Accepted[s->n_lines] = 0;
                //free(s->lines[s->n_lines]);
                ++s->n_lines;	
            }

            gettimeofday(&timecheck, NULL);
            start = (long)timecheck.tv_sec * 1000 + (long)timecheck.tv_usec / 1000;

            for (loopPar =10; loopPar<=10;loopPar++) {
                //s->EditThreshold =(loopPar * s->ReadLength)/100;
                kt_for(threads, worker_for, s, atoi(argv[6]));
                Accepted=0;
                for (i = 0; i < atoi(argv[6]); i++) {
                    //printf("%s\n",s->lines[i]);
                    if (s->Accepted[i])
                        Accepted++;
                    ++s->n_lines;
                } 
                printf("Data: %s\tThreads: %d\tE: %d\tAccepted: %d\tRejected:%d", argv[5], threads, s->EditThreshold, Accepted, atoi(argv[6])-Accepted);
            }
            gettimeofday(&timecheck, NULL);
            end = (long)timecheck.tv_sec * 1000 + (long)timecheck.tv_usec / 1000;

            printf("\tTime: %ld milliseconds\n", (end - start));

            for (i = 0; i < atoi(argv[6]); i++) {
                free(s->lines[i]);
                ++s->n_lines;
            } 
            free(s->lines); free(s->Accepted); free(s);
        }
	}

	fclose(fp);
	return 0;
}
