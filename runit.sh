#!/bin/bash
# ulimit -m 1058 1058;
# poetry run python3 aadg_genomics_class/v2.py data_big/reference20M.fasta data_big/reads20Mb.fasta 
# poetry run memray run -o runprofilev51.bin aadg_genomics_class/v5.py data_big/reference20M.fasta data_big/reads20Mb.fasta 
# poetry run memray run -o indexrp1.bin aadg_genomics_class/faster_index_tests.py

# poetry run python3 aadg_genomics_class/v5.py data_big/reference20M.fasta data_big/reads20Mb.fasta &
# PID="$!"
# while sleep 0.1;do ps -p $PID -o rss= >> procdata.txt;done

INPUT_FILE_T="b"
TEST_SUFF="cleanv6final"

rm -rfd profile${TEST_SUFF}${INPUT_FILE_T}.bin   2> /dev/null && \
rm -rfd memray-flamegraph-profile${TEST_SUFF}${INPUT_FILE_T}.html 2> /dev/null

poetry run memray run -o profile${TEST_SUFF}${INPUT_FILE_T}.bin  aadg_genomics_class/v6_clean.py data_big/reference20M.fasta data_big/reads20M${INPUT_FILE_T}.fasta    
poetry run memray flamegraph profile${TEST_SUFF}${INPUT_FILE_T}.bin 
open memray-flamegraph-profile${TEST_SUFF}${INPUT_FILE_T}.html
poetry run python3 aadg_genomics_class/checker.py data_big/reads20M${INPUT_FILE_T}.txt > report${TEST_SUFF}${INPUT_FILE_T}.txt
open report${TEST_SUFF}${INPUT_FILE_T}.txt