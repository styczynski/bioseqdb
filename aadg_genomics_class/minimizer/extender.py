import numpy as np
from typing import Tuple
from .minimizer import MinimizerIndex

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

# (t_begin, t_end, q_begin, q_end)

def find_longest_increasing_subsequence(
    matches,
):
    n = len(matches)

    if n == 0:
        return RegionMatch(
            t_begin=0,
            t_end=0,
            q_begin=0,
            q_end=0,
            lis_length=0,
        )
    
    if n == 1:
        return RegionMatch(
            t_begin=matches[0, 1],
            t_end=matches[0, 1],
            q_begin=matches[0, 0],
            q_end=matches[0, 0],
            lis_length=1,
        )

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
    
    return RegionMatch(
        t_begin=matches[current_node, 1],
        t_end=matches[increasingSub[longest_seq_len-1], 1],
        q_begin=matches[current_node, 0],
        q_end=matches[increasingSub[longest_seq_len-1], 0],
        lis_length=n,
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

