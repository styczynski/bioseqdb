#!/bin/bash
numactl -m 0 -C 0-7,16-23 ./learned_seeding_big_read  ~/human_ref/human_g1k_v37.fasta  ~/illumina_platinum/ERR194147/ERR194147_1.fastq.gz 5000 16 3
