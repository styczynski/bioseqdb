#!/bin/bash

mkdir build 2>/dev/null >/dev/null

rm /Applications/Postgres.app/Contents/Versions/17/lib/postgresql/bioseq.dylib 2>/dev/null >/dev/null
rm /Applications/Postgres.app/Contents/Versions/17/share/postgresql/extension/bioseq.control 2>/dev/null >/dev/null
rm /Applications/Postgres.app/Contents/Versions/17/share/postgresql/extension/bioseq--1.0.sql 2>/dev/null >/dev/null

export CMAKE_EXPORT_COMPILE_COMMANDS=1 && \
cd build && \
cmake --preset dev-opt-pg .. && cmake --build . && \
cmake --build . --target install && \
./bioseq/bioseq_aligner_int_tests_exec --reporter console::out=-::colour-mode=ansi

# "/Applications/Postgres.app/Contents/Versions/17/bin/psql" -h localhost -p 5432 "bioseq" -a -f ../bioseq/sql/setup.sql && \
# ./bioseq/bioseq_aligner_tests_exec --reporter console::out=-::colour-mode=ansi "[.benchmark-evaluation]"

#./build/bioseq/bioseq_aligner_int_tests_exec --reporter console::out=-::colour-mode=ansi 
#./bioseq/bioseq_aligner_tests_exec --reporter console::out=-::colour-mode=ansi  #"[minimizers]"

#&& \ "/Applications/Postgres.app/Contents/Versions/17/bin/psql" -h localhost -p 5432 "bioseq" -a -f ../bioseq/sql/setup.sql

# DEBUG COMMAND:
# gdb -ex=r --args ./build/bioseq/bioseq_aligner_tests_exec "--args" "--reporter" "console::out=-::colour-mode=ansi" "[heheszki-2]"