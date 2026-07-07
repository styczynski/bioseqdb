#!/bin/bash
filename="${1:-./test/input.fastq}"
samfilename="${filename%.*}.sam"
reffilename="${2:-./test/GCA_000005845.2_ASM584v2_genomic.fna}"
./g3.exe mem -p 4 -g 1 -i 1 -z 1 -F 350 -Z 72000 -o $samfilename $reffilename $filename
