#!/bin/bash

./convert.sh 2
head -n 150 reads.fasta | tail -n 128 > t
mv t reads.fasta
head -n 350 reference.fasta | tail -n 203 > t
mv t reference.fasta
./run.sh 1 1 1


#head -n 150 reads.fasta | tail -n 128 > t
#head -n 350 reference.fasta | tail -n 203 > t

