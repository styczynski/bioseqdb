#!/bin/bash

mkdir build 2>/dev/null >/dev/null

rm /Applications/Postgres.app/Contents/Versions/17/lib/postgresql/bioseq.dylib 2>/dev/null >/dev/null                                                                                                                                                                                                                                                   
rm /Applications/Postgres.app/Contents/Versions/17/share/postgresql/extension/bioseq.control 2>/dev/null >/dev/null                                                                                                                                                                                                                                        
rm /Applications/Postgres.app/Contents/Versions/17/share/postgresql/extension/bioseq--1.0.sql 2>/dev/null >/dev/null

cd build && \
cmake -G Ninja .. && cmake --build . && cmake --build . --target install && \
psql -h localhost -p 5432 -a -f ../bioseq/sql/setup.sql

