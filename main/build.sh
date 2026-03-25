#!/bin/bash

mkdir build 2>/dev/null >/dev/null

rm /Applications/Postgres.app/Contents/Versions/17/lib/postgresql/bioseq.dylib 2>/dev/null >/dev/null                                                                                                                                                                                                                                                   
rm /Applications/Postgres.app/Contents/Versions/17/share/postgresql/extension/bioseq.control 2>/dev/null >/dev/null                                                                                                                                                                                                                                        
rm /Applications/Postgres.app/Contents/Versions/17/share/postgresql/extension/bioseq--1.0.sql 2>/dev/null >/dev/null

export CMAKE_EXPORT_COMPILE_COMMANDS=1 && \
cd build && \
cmake --preset prod .. && cmake --build . && \
./bioseq/bioseq_aligner_tests_exec --reporter console::out=-::colour-mode=ansi "[heheszki]" #"[simd-new-x]" #cmake --build . --target test_bioseq_aligner && \
exit 0 && \
cmake --build . --target install && \
./bioseq/bioseq_aligner ../data/target.fasta ../data/query.fasta

# export CMAKE_EXPORT_COMPILE_COMMANDS=1 && \
# cd build && \
# cmake --preset dev .. && cmake --build . && \
# cmake --build . --target install && \
# ./bioseq/bioseq_aligner ../data/target.fasta ../data/query.fasta

#psql -h localhost -p 5432 -a -f ../bioseq/sql/setup.sql

