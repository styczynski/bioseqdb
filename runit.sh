#!/bin/bash
ulimit -m 1058 1058;
poetry run python3 aadg_genomics_class/v2.py data_big/reference20M.fasta data_big/reads20Mb.fasta 

poetry run memray run -o runprofile1.bin aadg_genomics_class/v2.py data_big/reference20M.fasta data_big/reads20Mb.fasta 