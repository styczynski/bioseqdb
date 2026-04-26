#!/bin/bash

# numactl -m 0 -C 0-7,16-23

#filelist="file1.txt file2.txt file3.txt"

#filelist="ERR194146_1.fastq.gz ERR194146_2.fastq.gz  ERR194147_1.fastq.gz ERR194147_2.fastq.gz  ERR194158_1.fastq.gz ERR194158_2.fastq.gz  ERR194159_1.fastq.gz ERR194159_2.fastq.gz  ERR194160_1.fastq.gz ERR194160_2.fastq.gz  ERR194161_1.fastq.gz ERR194161_2.fastq.gz ERR3239276_1.fastq.gz ERR3239276_2.fastq.gz ERR3239277_1.fastq.gz ERR3239277_2.fastq.gz ERR3239278_1.fastq.gz ERR3239278_2.fastq.gz ERR3239279_1.fastq.gz ERR3239279_2.fastq.gz ERR3239280_1.fastq.gz ERR3239280_2.fastq.gz ERR3239281_1.fastq.gz ERR3239281_2.fastq.gz ERR3239282_1.fastq.gz ERR3239282_2.fastq.gz ERR3239283_1.fastq.gz ERR3239283_2.fastq.gz ERR3239284_1.fastq.gz ERR3239284_2.fastq.gz"

#filelist="ERR194146_1.fastq.gz ERR3239278_1.fastq.gz"

filelist="ERR194146_1.fastq.gz ERR3239278_1.fastq.gz ERR194147_1.fastq.gz ERR3239276_1.fastq.gz ERR194158_1.fastq.gz ERR194159_1.fastq.gz ERR194160_1.fastq.gz ERR194161_1.fastq.gz ERR3239277_1.fastq.gz ERR3239279_1.fastq.gz ERR3239280_1.fastq.gz ERR3239281_1.fastq.gz ERR3239282_1.fastq.gz ERR3239283_1.fastq.gz ERR3239284_1.fastq.gz"
mkdir -p results
for f in ${filelist};do
    echo "Seeding throughput EXP start"
    echo "Starting EXP for ERT $f"
    #ERT
    echo "" >results/ert_seeding_exp_$f.log
    echo "" >results/ert_seeding_result_$f.log
    #numactl -m 1 -C 24 ./ert_seeding_big_read   ~/human_ref/human_g1k_v37.fasta  /ssd/$f 100 1 3 2>> results/ert_seeding_exp_$f.log >> results/ert_seeding_result_$f.log
    #numactl -m 1 -C 24-31 ./ert_seeding_big_read ~/human_ref/human_g1k_v37.fasta  /ssd/$f 100 8 3 2>> results/ert_seeding_exp_$f.log >> results/ert_seeding_result_$f.log
    #numactl -m 1 -C 24-39 ./ert_seeding_big_read ~/human_ref/human_g1k_v37.fasta  /ssd/$f 100 16 3 2>> results/ert_seeding_exp_$f.log >> results/ert_seeding_result_$f.log
    numactl -m 1 -C 24-47 ./ert_seeding_big_read ~/human_ref/human_g1k_v37.fasta  /ssd/$f 100 24 3 2>> results/ert_seeding_exp_$f.log >> results/ert_seeding_result_$f.log
    numactl -m 1 -C 24-47,72-95 ./ert_seeding_big_read ~/human_ref/human_g1k_v37.fasta  /ssd/$f 100 48 3 2>> results/ert_seeding_exp_$f.log >> results/ert_seeding_result_$f.log
    
    echo "Starting EXP for without ISA and 64bit suffixes BWA-MEME $f"
    # no memtradeoff
    echo "" >results/learned_seeding_result_mode_1_$f.log
    echo "" >results/learned_seeding_exp_mode_1_$f.log
    #numactl -m 1 -C 24 ./learned_seeding_big_read_mode_1   ~/human_ref/human_g1k_v37.fasta  /ssd/$f 100 1 3 2>> results/learned_seeding_exp_mode_1_$f.log >> results/learned_seeding_result_mode_1_$f.log
    #numactl -m 1 -C 24-31 ./learned_seeding_big_read_mode_1 ~/human_ref/human_g1k_v37.fasta  /ssd/$f 100 8 3 2>> results/learned_seeding_exp_mode_1_$f.log >> results/learned_seeding_result_mode_1_$f.log
    #numactl -m 1 -C 24-39 ./learned_seeding_big_read_mode_1 ~/human_ref/human_g1k_v37.fasta  /ssd/$f 100 16 3 2>> results/learned_seeding_exp_mode_1_$f.log >> results/learned_seeding_result_mode_1_$f.log
    numactl -m 1 -C 24-47 ./learned_seeding_big_read_mode_1 ~/human_ref/human_g1k_v37.fasta  /ssd/$f 100 24 3 2>> results/learned_seeding_exp_mode_1_$f.log >> results/learned_seeding_result_mode_1_$f.log
    numactl -m 1 -C 24-47,72-95 ./learned_seeding_big_read_mode_1 ~/human_ref/human_g1k_v37.fasta  /ssd/$f 100 48 3 2>> results/learned_seeding_exp_mode_1_$f.log >> results/learned_seeding_result_mode_1_$f.log

    echo "Starting EXP for without ISA BWA-MEME $f"
    # sa key accel
    echo "" >results/learned_seeding_result_mode_2_$f.log
    echo "" >results/learned_seeding_exp_mode_2_$f.log
    #numactl -m 1 -C 24 ./learned_seeding_big_read_mode_2   ~/human_ref/human_g1k_v37.fasta  /ssd/$f 100 1 3 2>> results/learned_seeding_exp_mode_2_$f.log >> results/learned_seeding_result_mode_2_$f.log
    #numactl -m 1 -C 24-31 ./learned_seeding_big_read_mode_2 ~/human_ref/human_g1k_v37.fasta  /ssd/$f 100 8 3 2>> results/learned_seeding_exp_mode_2_$f.log >> results/learned_seeding_result_mode_2_$f.log
    #numactl -m 1 -C 24-39 ./learned_seeding_big_read_mode_2 ~/human_ref/human_g1k_v37.fasta  /ssd/$f 100 16 3 2>> results/learned_seeding_exp_mode_2_$f.log >> results/learned_seeding_result_mode_2_$f.log
    numactl -m 1 -C 24-47 ./learned_seeding_big_read_mode_2 ~/human_ref/human_g1k_v37.fasta  /ssd/$f 100 24 3 2>> results/learned_seeding_exp_mode_2_$f.log >> results/learned_seeding_result_mode_2_$f.log
    numactl -m 1 -C 24-47,72-95 ./learned_seeding_big_read_mode_2 ~/human_ref/human_g1k_v37.fasta  /ssd/$f 100 48 3 2>> results/learned_seeding_exp_mode_2_$f.log >> results/learned_seeding_result_mode_2_$f.log

    echo "Starting EXP for BWA-MEME $f"
    # memtradeoff
    echo "" >results/learned_seeding_result_mode_3_$f.log
    echo "" >results/learned_seeding_exp_mode_3_$f.log
    #numactl -m 1 -C 24 ./learned_seeding_big_read_mode_3   ~/human_ref/human_g1k_v37.fasta  /ssd/$f 100 1 3 2>> results/learned_seeding_exp_mode_3_$f.log >> results/learned_seeding_result_mode_3_$f.log
    #numactl -m 1 -C 24-31 ./learned_seeding_big_read_mode_3 ~/human_ref/human_g1k_v37.fasta  /ssd/$f 100 8 3 2>> results/learned_seeding_exp_mode_3_$f.log >> results/learned_seeding_result_mode_3_$f.log
    #numactl -m 1 -C 24-39 ./learned_seeding_big_read_mode_3 ~/human_ref/human_g1k_v37.fasta  /ssd/$f 100 16 3 2>> results/learned_seeding_exp_mode_3_$f.log >> results/learned_seeding_result_mode_3_$f.log
    numactl -m 1 -C 24-47 ./learned_seeding_big_read_mode_3 ~/human_ref/human_g1k_v37.fasta  /ssd/$f 100 24 3 2>> results/learned_seeding_exp_mode_3_$f.log >> results/learned_seeding_result_mode_3_$f.log
    numactl -m 1 -C 24-47,72-95 ./learned_seeding_big_read_mode_3 ~/human_ref/human_g1k_v37.fasta  /ssd/$f 100 48 3 2>> results/learned_seeding_exp_mode_3_$f.log >> results/learned_seeding_result_mode_3_$f.log

    echo "Starting EXP for FMI $f"
    #FMI
    echo "" >results/fmi_seeding_result_$f.log
    echo "" >results/fmi_seeding_exp_$f.log
    #numactl -m 1 -C 24 ./fmi_seeding_big_read   ~/human_ref/human_g1k_v37.fasta  /ssd/$f 5000 1 3 2>> results/fmi_seeding_exp_$f.log >> results/fmi_seeding_result_$f.log
    #numactl -m 1 -C 24-31 ./fmi_seeding_big_read ~/human_ref/human_g1k_v37.fasta  /ssd/$f 5000 8 3 2>> results/fmi_seeding_exp_$f.log >> results/fmi_seeding_result_$f.log
    #numactl -m 1 -C 24-39 ./fmi_seeding_big_read ~/human_ref/human_g1k_v37.fasta  /ssd/$f 5000 16 3 2>> results/fmi_seeding_exp_$f.log >> results/fmi_seeding_result_$f.log
    numactl -m 1 -C 24-47 ./fmi_seeding_big_read ~/human_ref/human_g1k_v37.fasta  /ssd/$f 5000 24 3 2>> results/fmi_seeding_exp_$f.log >> results/fmi_seeding_result_$f.log
    numactl -m 1 -C 24-47,72-95 ./fmi_seeding_big_read ~/human_ref/human_g1k_v37.fasta  /ssd/$f 5000 48 3 2>> results/fmi_seeding_exp_$f.log >> results/fmi_seeding_result_$f.log
    
done;
