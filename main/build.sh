#!/bin/bash

mkdir build 2>/dev/null >/dev/null

rm /Applications/Postgres.app/Contents/Versions/17/lib/postgresql/bioseq.dylib 2>/dev/null >/dev/null
rm /Applications/Postgres.app/Contents/Versions/17/share/postgresql/extension/bioseq.control 2>/dev/null >/dev/null
rm /Applications/Postgres.app/Contents/Versions/17/share/postgresql/extension/bioseq--1.0.sql 2>/dev/null >/dev/null

export CMAKE_EXPORT_COMPILE_COMMANDS=1 && \
cd build && \
cmake --preset dev-opt .. && cmake --build . && \
cmake --build . --target install && \
./bioseq/bioseq_aligner -c ../data/target.fasta ../data/query.fasta

# Convert prof file
# NOTE: this runs from inside ./build (see `cd build` above), so the paths are
# relative to the build directory. The profiler converter produces prof.json
# which can be fed to flamegraph / speedscope / chrome trace tooling.
./vcpkg_installed/arm64-osx/tools/easy-profiler/profiler_converter ./bioseq.prof ./prof.json

# ./bioseq/bioseq_aligner_tests_exec --reporter console::out=-::colour-mode=ansi "[heheszki-2]" #"[simd-new-x]" #cmake --build . --target test_bioseq_aligner && \
#exit 0


# export CMAKE_EXPORT_COMPILE_COMMANDS=1 && \
# cd build && \
# cmake --preset dev .. && cmake --build . && \
# cmake --build . --target install && \
# ./bioseq/bioseq_aligner ../data/target.fasta ../data/query.fasta

#psql -h localhost -p 5432 -a -f ../bioseq/sql/setup.sql

# DEBUG COMMAND:
# gdb -ex=r --args ./build/bioseq/bioseq_aligner_tests_exec "--args" "--reporter" "console::out=-::colour-mode=ansi" "[heheszki-2]"