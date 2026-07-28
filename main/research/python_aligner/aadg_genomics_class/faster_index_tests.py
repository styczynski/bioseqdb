import sys
#from Bio import SeqRecord, SeqIO

import gc
from dataclasses import dataclass
import sys

from typing import Dict, Any, Set, Optional
from itertools import chain
from numpy.lib.stride_tricks import sliding_window_view

import time
#import csv
#import tracemalloc

#import math
import numpy as np
from typing import Iterable

MASKS: Optional[Dict[int, int]] = None
MAX_KMER_SIZE = 64

MAPPING = dict(
    A=1,
    a=1,
    c=0,
    C=0,
    g=3,
    G=3,
    t=2,
    T=2,
)

RR_MAPPING = ["C", "A", "T", "G"]

COMPLEMENT_MAPPING = {
    1: 2,
    0: 3,
    3: 0,
    2: 1,
}

MAPPING_FN = np.vectorize(MAPPING.get)
COMPLEMENT_MAPPING_FN = np.vectorize(COMPLEMENT_MAPPING.get)

def generate_mask(
    kmer_len: int,
) -> int:
    global MASKS
    if not MASKS:
        MASKS = dict()
        ret = 3
        for i in range(MAX_KMER_SIZE+1):
            ret = (ret << 2) | 3
            MASKS[i] = ret
    return MASKS[kmer_len]

def doit():
    kmer_len = 18
    window_len = 8
    reference_path = "./data_big/reference20M.fasta" #"./data/reference.fasta" #"./data_big/reference20M.fasta"

    # Seq loading
    # reference_records, reference_ids = format_sequences(SeqIO.parse("./data_big/reference20M.fasta", "fasta"))
    # seq_arr = reference_records[reference_ids[0]]

    ref_loaded = False
    all_seq = ""
    all_seq_len = 0
    CHUNK_SIZE = 1000000

    ref_index = dict()
    with open(reference_path) as ref_fh:
        for line in chain(ref_fh, [">"]):
            if line[0] != '>':
                fasta_line = line.rstrip()
                all_seq += fasta_line
                all_seq_len  += len(fasta_line)
            if (all_seq_len >= CHUNK_SIZE or line[0] == '>') and all_seq_len > 0:
                print(f"PROCESS CHUNK {all_seq_len}")
                # all_seq_len = 0
                # all_seq = 0

                seq_arr = MAPPING_FN(np.array(list(all_seq)))
                del all_seq

                # Target index building
                sequence_len = len(seq_arr)
                mask = generate_mask(kmer_len)

                # Function to compute kmer value based on the previous (on the left side) kmer value and new nucleotide
                uadd = np.frompyfunc(lambda x, y: ((x << 2) | y) & mask, 2, 1)

                # This computes values for kmers
                kmers = uadd.accumulate(seq_arr, dtype=object).astype(int)
                kmers[:kmer_len-2] = 0
                del seq_arr
                
                # Do sliding window and get min kmers positions
                kmers_min_pos = np.add(np.argmin(sliding_window_view(kmers, window_shape=window_len), axis=1), np.arange(0, sequence_len - window_len + 1))
                
                # Now collect all selected mimumum and kmers into single table
                selected_kmers = np.column_stack((
                    kmers[kmers_min_pos],
                    kmers_min_pos,
                    #np.ones(len(kmers_min_pos), dtype=bool)
                ))[kmer_len:]
                del kmers_min_pos
                del kmers
                gc.collect()

                # Remove duplicates
                selected_kmers = selected_kmers[selected_kmers[:, 0].argsort()]
                selected_kmers = np.unique(selected_kmers, axis=0)

                # This part performs group by using the kmer value
                selected_kmers_unique_idx = np.unique(selected_kmers[:, 0], return_index=True)[1][1:]
                selected_kmers_entries_split = np.split(selected_kmers[:, 1], selected_kmers_unique_idx)

                if len(selected_kmers) > 0:
                    # We zip all kmers into a dict
                    i = 0
                    for k, v in zip(chain([selected_kmers[0, 0]], selected_kmers[selected_kmers_unique_idx, 0]), selected_kmers_entries_split):
                        i += 1
                        if i >= 20 and len(v) == 1:
                            i = 0
                            continue
                        if k in ref_index:
                            ref_index[k] = np.concatenate((ref_index[k], v), axis=0)
                        else:
                            ref_index[k] = v
                else:
                    # If we have no minimizers we return nothing, sorry
                    pass

                all_seq_len = 0
                all_seq = ""
                print(f"PROCESSED ENTIRE CHUNK!")
                del selected_kmers_unique_idx
                del selected_kmers_entries_split
                gc.collect()
            if line[0] == '>':
                if ref_loaded:
                    break
                ref_loaded = True
                continue
    print("INDEX READY")
    return ref_index

def testing():
    ref_index = doit()
    print(len(ref_index))
    all_len = 0
    ones_len = 0
    for v in ref_index.values():
        all_len += len(v)
        if len(v) == 1:
            ones_len += 1
    print(all_len)
    print(ones_len)
    time.sleep(5)
    time.sleep(5)
    print("DONE! :D")

if __name__ == '__main__':
    testing()