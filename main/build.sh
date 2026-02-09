#!/bin/bash

mkdir build 2>/dev/null >/dev/null

rm /Applications/Postgres.app/Contents/Versions/17/lib/postgresql/bioseq.dylib 2>/dev/null >/dev/null                                                                                                                                                                                                                                                   
rm /Applications/Postgres.app/Contents/Versions/17/share/postgresql/extension/bioseq.control 2>/dev/null >/dev/null                                                                                                                                                                                                                                        
rm /Applications/Postgres.app/Contents/Versions/17/share/postgresql/extension/bioseq--1.0.sql 2>/dev/null >/dev/null

cd build && \
cmake --preset dev .. && cmake --build . && \
cmake --build . --target test_bioseq_aligner && \
exit 0 && \
cmake --build . --target install && \
./bioseq/bioseq_aligner ../data/target.fasta ../data/query.fasta
#psql -h localhost -p 5432 -a -f ../bioseq/sql/setup.sql

