import numpy as np
from typing import Tuple
from .minimizer import MinimizerIndex
import numba as nb

def cartesian_product(*arrays):
    la = len(arrays)
    dtype = np.result_type(*arrays)
    arr = np.empty([len(a) for a in arrays] + [la], dtype=dtype)
    for i, a in enumerate(np.ix_(*arrays)):
        arr[...,i] = a
    return arr.reshape(-1, la)

from dataclasses import dataclass

@dataclass
class RegionMatch:
    t_begin: int
    t_end: int
    q_begin: int
    q_end: int
    lis_length: int

@nb.njit(cache=True)
def _longest_increasing_subsequence_impl(
    n,
    matches,
):
    if n == 0:
        return 0, 0, 0, 0, 0
    if n == 1:
        return matches[0, 1], matches[0, 1], matches[0, 0], matches[0, 0], 1
    longest_seq_len = 0
    parent = [999999999]*(n+1)
    increasingSub = [999999999]*(n+1)
    for i in range(n):
        start = 1
        end = longest_seq_len
        while start <= end:
            middle = (start + end) // 2
            if matches[increasingSub[middle], 1] < matches[i, 1]:
                start = middle + 1
            else:
                end = middle - 1
        parent[i] = increasingSub[start-1]
        increasingSub[start] = i

        if start > longest_seq_len:
            longest_seq_len = start

    current_node = increasingSub[longest_seq_len]
    for j in range(longest_seq_len-1, 0, -1):
        current_node = parent[current_node]
    return matches[current_node, 1], matches[increasingSub[longest_seq_len-1], 1], matches[current_node, 0], matches[increasingSub[longest_seq_len-1], 0], n,

def find_longest_increasing_subsequence(
    matches,
):
    n = len(matches)
    t_begin, t_end, q_begin, q_end, lis_length = _longest_increasing_subsequence_impl(
        n=n,
        matches=matches,
    )
    return RegionMatch(
        t_begin=t_begin,
        t_end=t_end,
        q_begin=q_begin,
        q_end=q_end,
        lis_length=lis_length,
    )

def extend(
    target_minimizer_index: MinimizerIndex,
    query_minimizer_index: MinimizerIndex,
) -> RegionMatch:

    common_kmers = target_minimizer_index.kmers.intersection(query_minimizer_index.kmers)
    target_idx = target_minimizer_index.index
    query_idx = query_minimizer_index.index

    matches = np.array([[-1, -1]])
    for kmer in common_kmers:
        kmer_entries_target, kmer_entries_query = target_idx[kmer], query_idx[kmer]
        matches = np.concatenate((
            matches,
            cartesian_product(
                kmer_entries_query[kmer_entries_query[:,1] == True][:,0],
                kmer_entries_target[kmer_entries_target[:,1] == True][:,0],
            ),
            cartesian_product(
                kmer_entries_query[kmer_entries_query[:,1] == False][:,0],
                kmer_entries_target[kmer_entries_target[:,1] == False][:,0],
            )),
            axis=0,
        )
    matches = np.sort(matches[1:], axis=0)
    region_match = find_longest_increasing_subsequence(matches)
    
    return region_match

