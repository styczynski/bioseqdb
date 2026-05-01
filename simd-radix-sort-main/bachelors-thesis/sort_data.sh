#!/bin/bash
for filename in data/int*.dat; do
    cat $filename | (sed -u 1q; sort -V -k 2 -r) > $filename.sorted
    cp $filename.sorted $filename
    rm $filename.sorted
done
for filename in data/uint*.dat; do
    cat $filename | (sed -u 1q; sort -V -k 2 -r) > $filename.sorted
    cp $filename.sorted $filename
    rm $filename.sorted
done
for filename in data/float*.dat; do
    cat $filename | (sed -u 1q; sort -V -k 2 -r) > $filename.sorted
    cp $filename.sorted $filename
    rm $filename.sorted
done
for filename in data/double*.dat; do
    cat $filename | (sed -u 1q; sort -V -k 2 -r) > $filename.sorted
    cp $filename.sorted $filename
    rm $filename.sorted
done
