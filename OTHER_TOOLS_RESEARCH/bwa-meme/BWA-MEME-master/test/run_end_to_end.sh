#!/bin/bash

# numactl -m 0 -C 0-7,16-23

#filelist="file1.txt file2.txt file3.txt"

#filelist="ERR194146_1.fastq.gz ERR194146_2.fastq.gz  ERR194147_1.fastq.gz ERR194147_2.fastq.gz  ERR194158_1.fastq.gz ERR194158_2.fastq.gz  ERR194159_1.fastq.gz ERR194159_2.fastq.gz  ERR194160_1.fastq.gz ERR194160_2.fastq.gz  ERR194161_1.fastq.gz ERR194161_2.fastq.gz ERR3239276_1.fastq.gz ERR3239276_2.fastq.gz ERR3239277_1.fastq.gz ERR3239277_2.fastq.gz ERR3239278_1.fastq.gz ERR3239278_2.fastq.gz ERR3239279_1.fastq.gz ERR3239279_2.fastq.gz ERR3239280_1.fastq.gz ERR3239280_2.fastq.gz ERR3239281_1.fastq.gz ERR3239281_2.fastq.gz ERR3239282_1.fastq.gz ERR3239282_2.fastq.gz ERR3239283_1.fastq.gz ERR3239283_2.fastq.gz ERR3239284_1.fastq.gz ERR3239284_2.fastq.gz"

#filelist="ERR194146_1.fastq.gz ERR3239278_1.fastq.gz"

filelist="ERR194146_1.fastq.gz ERR3239278_1.fastq.gz ERR194147_1.fastq.gz ERR3239276_1.fastq.gz ERR194158_1.fastq.gz ERR194159_1.fastq.gz ERR194160_1.fastq.gz ERR194161_1.fastq.gz ERR3239277_1.fastq.gz ERR3239279_1.fastq.gz ERR3239280_1.fastq.gz ERR3239281_1.fastq.gz ERR3239282_1.fastq.gz ERR3239283_1.fastq.gz ERR3239284_1.fastq.gz"
mkdir -p results
for f in ${filelist};do
   echo "BWA-MEM endtoend EXP start"
    echo "Starting EXP for ERT $f"
    
    echo "" >results/ert_endtoend_results_$f.log
    #ERT
    numactl -m 1 -C 24-47 ./bwa-mem2_mode_1 mem -t 24 -o /ssd/bwamemoutput.txt -Z ~/human_ref/human_g1k_v37.fasta ~/human_ref/human_g1k_v37.fasta /ssd/$f 2>> results/ert_endtoend_results_$f.log
    numactl -m 1 -C 24-47,72-95 ./bwa-mem2_mode_1 mem -t 48 -o /ssd/bwamemoutput.txt -Z ~/human_ref/human_g1k_v37.fasta ~/human_ref/human_g1k_v37.fasta /ssd/$f 2>> results/ert_endtoend_results_$f.log
    
    echo "Starting EXP for without ISA and 64bit suffixes BWA-MEME $f"
    echo "" >results/learned_endtoend_mode_1_$f.log
    numactl -m 1 -C 24-47 ./bwa-mem2_mode_1 mem -t 24 -o /ssd/bwamemoutput.txt -7 ~/human_ref/human_g1k_v37.fasta /ssd/$f 2>> results/learned_endtoend_mode_1_$f.log
    numactl -m 1 -C 24-47,72-95 ./bwa-mem2_mode_1 mem -t 48 -o /ssd/bwamemoutput.txt -7 ~/human_ref/human_g1k_v37.fasta /ssd/$f 2>> results/learned_endtoend_mode_1_$f.log

    echo "Starting EXP for without ISA BWA-MEME $f"
    echo "" >results/learned_endtoend_mode_2_$f.log
    numactl -m 1 -C 24-47 ./bwa-mem2_mode_2 mem -t 24 -o /ssd/bwamemoutput.txt -7 ~/human_ref/human_g1k_v37.fasta /ssd/$f 2>> results/learned_endtoend_mode_2_$f.log
    numactl -m 1 -C 24-47,72-95 ./bwa-mem2_mode_2 mem -t 48 -o /ssd/bwamemoutput.txt -7 ~/human_ref/human_g1k_v37.fasta /ssd/$f 2>> results/learned_endtoend_mode_2_$f.log

    echo "Starting EXP for BWA-MEME $f"
    echo "" >results/learned_endtoend_mode_3_$f.log
    numactl -m 1 -C 24-47 ./bwa-mem2_mode_3 mem -t 24 -o /ssd/bwamemoutput.txt -7 ~/human_ref/human_g1k_v37.fasta /ssd/$f 2>> results/learned_endtoend_mode_3_$f.log
    numactl -m 1 -C 24-47,72-95 ./bwa-mem2_mode_3 mem -t 48 -o /ssd/bwamemoutput.txt -7 ~/human_ref/human_g1k_v37.fasta /ssd/$f 2>> results/learned_endtoend_mode_3_$f.log

    #FMI
    echo "Starting EXP for fmi_seeding $f"
    echo "" >results/fmi_endtoend_results_$f.log
    numactl -m 1 -C 24-47 ./bwa-mem2_mode_1 mem -t 24 -o /ssd/bwamemoutput.txt ~/human_ref/human_g1k_v37.fasta /ssd/$f 2>> results/fmi_endtoend_results_$f.log
    numactl -m 1 -C 24-47,72-95 ./bwa-mem2_mode_1 mem -t 48 -o /ssd/bwamemoutput.txt ~/human_ref/human_g1k_v37.fasta /ssd/$f 2>> results/fmi_endtoend_results_$f.log

    

    
done;
