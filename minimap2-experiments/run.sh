#!/bin/bash
make arm_neon=1 aarch64=1 && \
./minimap2 target.fasta query.fasta -t 1
