#include "hashKMer.hpp"
#include <iostream>
#include <string.h>
#include <random>
#include <cstdlib>
#include "bwa.h"
#include "loadKMerIndex.hpp"
#include "datadump.hpp"
#include <stdio.h>
#include <locale.h>


int main(int argc, char const *argv[])
{
#define FASTA_PATH 1
#define OUTPUT_PATH 2
    if(argc != 3) {
        fprintf(stderr, "usage: <prog> <fasta> <output-path>\n");
        return 1;
    } else {
        fprintf(stderr, "building hash with K = %d from input %s to store at %s\n", KMER_K, argv[FASTA_PATH], argv[OUTPUT_PATH]);
    }

    setlocale(LC_ALL, ""); /* use user selected locale */
    std::random_device rd; // obtain a random number from hardware
    std::mt19937 gen(rd()); // seed the generator
    std::uniform_int_distribution<> distr(0, 3); // define the range

    // load bwt index from disk
    bwaidx_t *idx;
    if ((idx = bwa_idx_load(argv[FASTA_PATH], BWA_IDX_BWT|BWA_IDX_BNS)) == 0) {
        std::cerr << "can't load bwt index!" << std::endl;
        return 1;
    }
    
    // create hash table
    kmers_bucket_t *hashTable = createHashKTable(idx->bwt);

    // dump hash table to binary file
    dumpArray(hashTable, pow4(KMER_K), argv[OUTPUT_PATH]);

    // print hash table to std out
    kmers_bucket_t *hashTable2 = loadKMerIndex(argv[OUTPUT_PATH]);

    for (int i=0; i<pow4(KMER_K); i++)
        printf("%9s %'14lu %'14lu %'14lu\n", inverseHashK(i), hashTable2[i].x[0], hashTable2[i].x[1], hashTable2[i].x[2]);

    /* code */
    return 0;
}
