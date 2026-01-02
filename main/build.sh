#!/bin/bash

mkdir build 2>/dev/null >/dev/null

cd build && \
cmake .. && make && make install && \
psql -h localhost -p 5432 -a -f ../bioseq/sql/setup.sql

