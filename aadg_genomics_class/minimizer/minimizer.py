"""
  Utilities to operate on (k, w)-minimizers

  @Piotr Styczyński 2023 <piotr@styczynski.in>
  MIT LICENSE
  Algorithms for genomic data analysis | AADG | MIM UW | Bioinformatyka
"""
import numpy as np
from itertools import chain
from numpy.lib.stride_tricks import sliding_window_view
from dataclasses import dataclass
from typing import Dict, Any, Set, Optional
from aadg_genomics_class.sequences import sequence_complement
from numba import njit

MASKS: Optional[Dict[int, int]] = None
MAX_KMER_SIZE = 64

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

@dataclass
class MinimizerIndex:
    index: Dict[int, Any]
    kmers: Set[int]

@njit(cache=True)
def _get_kmers_min_pos(
    sequence_len,
    mask,
    r_seq_arr,
    kmers,
    r_kmers,
    window_len,
    kmer_len,
):
    kmers_min_pos = np.add(np.argmin(sliding_window_view(kmers, window_shape=window_len), axis=1), np.arange(0, sequence_len - window_len + 1))
    r_kmers_min_pos = np.add(np.argmin(sliding_window_view(r_kmers, window_shape=window_len), axis=1), np.arange(0, sequence_len - window_len + 1))
    return kmers_min_pos, r_kmers_min_pos


def get_minimizers(
    seq_arr,
    kmer_len,
    window_len,
) -> MinimizerIndex:

    sequence_len = len(seq_arr)
    r_seq_arr = sequence_complement(seq_arr)
    mask = generate_mask(kmer_len)

    # Function to compute kmer value based on the previous (on the left side) kmer value and new nucleotide
    uadd = np.frompyfunc(lambda x, y: ((x << 2) | y) & mask, 2, 1)

    # This computes values for kmers
    kmers = uadd.accumulate(seq_arr, dtype=object).astype(int)
    kmers[:kmer_len-2] = 0

    r_kmers = uadd.accumulate(r_seq_arr, dtype=object).astype(int)
    r_kmers[:kmer_len-2] = 0

    # Do sliding window and get min kmers positions
    kmers_min_pos, r_kmers_min_pos = _get_kmers_min_pos(
        sequence_len=sequence_len,
        mask=mask,
        r_seq_arr=r_seq_arr,
        kmers=kmers,
        r_kmers=r_kmers,
        window_len=window_len,
        kmer_len=kmer_len,
    )

    # Select min from kmer and r_kmer
    select_min_from_kmer_r_kmer = np.argmin(np.column_stack((
        r_kmers[r_kmers_min_pos],
        kmers[kmers_min_pos],
    )), axis=1).astype(dtype=bool)

    # Now collect all selected mimumum and kmers into single table
    selected_kmers = np.column_stack((
        np.where(select_min_from_kmer_r_kmer, kmers[kmers_min_pos], r_kmers[r_kmers_min_pos]),
        np.where(select_min_from_kmer_r_kmer, kmers_min_pos, r_kmers_min_pos),
        select_min_from_kmer_r_kmer,
    ))[kmer_len:]

    # Remove duplicates
    selected_kmers = selected_kmers[selected_kmers[:, 0].argsort()]

    # This part performs group by using the kmer value
    selected_kmers_unique = np.unique(selected_kmers, axis=0)
    selected_kmers_unique_idx = np.unique(selected_kmers_unique[:, 0], return_index=True)[1][1:]
    selected_kmers_entries_split = np.split(selected_kmers_unique[:, 1:], selected_kmers_unique_idx)

    if len(a) > 0:
        # We zip all kmers into a dict
        result = dict(zip(chain([selected_kmers_unique[0, 0]], selected_kmers_unique[selected_kmers_unique_idx, 0]), selected_kmers_entries_split))
    else:
        # If we have no minimizers we return nothing, sorry
        result = dict()

    return MinimizerIndex(
        index=result,
        kmers=set(result.keys()),
    )