import sys
from Bio import SeqRecord, SeqIO

from dataclasses import dataclass

from aadg_genomics_class.monitoring.logs import LOGS
from aadg_genomics_class.monitoring.task_reporter import TaskReporter
from aadg_genomics_class import click_utils as click

from typing import Dict, Any, Set, Optional
from itertools import chain
from numpy.lib.stride_tricks import sliding_window_view

import tracemalloc

import math
import numpy as np
from typing import Iterable

@dataclass
class MinimizerIndex:
    index: Dict[int, Any]
    kmers: Set[int]

@dataclass
class RegionMatch:
    t_begin: int
    t_end: int
    q_begin: int
    q_end: int
    lis_length: int

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

COMPLEMENT_MAPPING = {
    1: 2,
    0: 3,
    3: 0,
    2: 1,
}

MAPPING_FN = np.vectorize(MAPPING.get)
COMPLEMENT_MAPPING_FN = np.vectorize(COMPLEMENT_MAPPING.get)

def format_sequences(src: Iterable[SeqRecord]):
    result = {record.id: MAPPING_FN(np.array(record.seq)) for record in src}
    return result, list(result.keys())

def iter_sequences(src: Iterable[SeqRecord]):
    return ((record.id, MAPPING_FN(np.array(record.seq))) for record in src)

def sequence_complement(seq):
     return COMPLEMENT_MAPPING_FN(seq)

def cleanup_kmer_index(
    index: MinimizerIndex,
    kmers_cutoff_f: float,
):
    keys = np.array(list(index.index.keys()))
    counts = np.array([index.index[kmer].size for kmer in keys])
    start_index = keys.size - math.floor(keys.size * kmers_cutoff_f)
    kmers_to_remove = np.lexsort((keys, counts))[start_index:]
    kmers_before = len(index.kmers)
    for kmer_pos in kmers_to_remove:
        index.index.pop(keys[kmer_pos], None)
        index.kmers.remove(keys[kmer_pos])
    kmers_after = len(index.kmers)
    LOGS.prefilter.info(f"Reduced target kmer count by prefiltering: {kmers_before} -> {kmers_after} (Eliminated {math.floor((kmers_before-kmers_after)/kmers_before*100000)/1000}% top kmers with f={kmers_cutoff_f})")


def normalize_pos(pos, len):
    return min(max(pos, 0), len-1)


def nw_align(
    target,
    query,
    score_match,
    score_mismatch,
    score_gap,
):
    """
        Implementation of Needleman-Wunsch algorithm for arbitrary Numpy array sequences.
        The algorithm expects two numeric-like sequences and scores used to compute the best alignment.
        The result is tuple of found elements:
            - t_pad_left - Where first position of target was aligned to query
            - t_pad_right - Where the last position of target was aligned to query
            - q_pad_left - Where the first position of query was aligned to target
            - q_pad_right - Where the last position of query was aligned to target
        
        For example for:

            ACTA-GT-C-AAA-
            ---CT-ACTGAA--

            We could get (0, 1, 3, 2)

            Target has 0 dashes on the left side
            Target has 1 dash on the right side
            Query has 3 dashes on the left side
            Query has 2 dashes on the right side

    @Piotr Styczyński 2023
    """
    target_len, query_len = len(target), len(query)

    # Hold pointers to allow reconstruction of a sequence later
    score_pointers = np.zeros((target_len + 1, query_len + 1))
    score_pointers[:,0], score_pointers[0,:] = 3, 4

    # Optimal scores
    optimal_scores = np.zeros((target_len + 1, query_len + 1))
    optimal_scores[:,0], optimal_scores[0,:] = np.linspace(0, -target_len * score_gap, target_len + 1), np.linspace(0, -query_len * score_gap, query_len + 1)

    # Temporary score table
    scores = np.zeros(3)

    for target_pos in range(target_len):
        for query_pos in range(query_len):
            if target[target_pos] != query[query_pos]:
                scores[0] = optimal_scores[target_pos, query_pos] - score_mismatch
            else:
                scores[0] = optimal_scores[target_pos, query_pos] + score_match
            scores[1] = optimal_scores[target_pos, query_pos+1] - score_gap
            scores[2] = optimal_scores[target_pos+1, query_pos] - score_gap
            local_max = np.max(scores)
            optimal_scores[target_pos+1, query_pos+1] = local_max
            if scores[2] == local_max:
                score_pointers[target_pos+1, query_pos+1] += 4
            if scores[1] == local_max:
                score_pointers[target_pos+1, query_pos+1] += 3
            if scores[0] == local_max:
                score_pointers[target_pos+1, query_pos+1] += 2

    # Now we need to traverse the matrix and reconstruct the array
    # We hold *_ending_space to see if our traverse sequence starts with sequence of dashes: '-'
    # If it starts with dashes (the result sequence will be reversed), that means we have padding at the start of the sequence
    # We also look for consecutive sequences of dashes using *_longest_space variable
    # At the end we will read it to see what number of dashes occurr at the end of the sequence (remember everything is reversed) 
    t_ending_space, t_ending_space_len = True, 0
    q_ending_space, q_ending_space_len = True, 0
    t_longest_space, q_longest_space = 0, 0
    target_pos, query_pos = target_len, query_len

    while target_pos > 0 or query_pos > 0:
        if score_pointers[target_pos, query_pos] in [3, 5, 7, 9]:
            t_ending_space = False
            t_longest_space = 0
            q_longest_space += 1
            if q_ending_space:
                q_ending_space_len += 1
            target_pos -= 1
        elif score_pointers[target_pos, query_pos] in [2, 5, 6, 9]:
            t_ending_space = False
            q_ending_space = False
            t_longest_space = 0
            q_longest_space = 0
            target_pos -= 1
            query_pos -= 1
        elif score_pointers[target_pos, query_pos] in [4, 6, 7, 9]:
            q_ending_space = False
            t_longest_space == 1
            q_longest_space = 0
            if t_ending_space:
                t_ending_space_len += 1
            query_pos -= 1

    # Return paddings of target and query sequence in the resulting alignment.
    # For example for:
    #  ACTA-GT-C-AAA-
    #  ---CT-ACTGAA--
    # We would get (0, 1, 3, 2)
    #   Target has 0 dashes on the left side
    #   Target has 1 dash on the right side
    #   Query has 3 dashes on the left side
    #   Query has 2 dashes on the right side
    #
    return (t_longest_space, t_ending_space_len, q_longest_space, q_ending_space_len)

def align(
    region: RegionMatch,
    target_seq,
    query_seq,
    kmer_len,
    full_query_len,
    score_match,
    score_mismatch,
    score_gap,
):
    relative_extension = kmer_len*3

    q_begin, q_end = region.q_begin-relative_extension, region.q_end+(kmer_len-1)+relative_extension
    t_begin, t_end = region.t_begin-relative_extension, min(region.t_end, region.t_begin + full_query_len)+(kmer_len-1)+relative_extension

    q_begin, q_end = normalize_pos(q_begin, len(query_seq)), normalize_pos(q_end, len(query_seq))
    t_begin, t_end = normalize_pos(t_begin, len(target_seq)), normalize_pos(t_end, len(target_seq))

    t_pad_left, t_pad_right, q_pad_left, q_pad_right = nw_align(
        target=target_seq[t_begin:t_end],
        query=query_seq[q_begin:q_end],
        score_match=score_match,
        score_mismatch=score_mismatch,
        score_gap=score_gap,
    )

    q_begin, q_end = q_begin + t_pad_left, q_end - t_pad_right
    t_begin, t_end = t_begin + q_pad_left, t_end - q_pad_right

    return (t_begin, t_end)

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
    r_seq_arr = seq_arr #sequence_complement(seq_arr) # This causes alignment problems?
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

    if len(selected_kmers_unique) > 0:
        # We zip all kmers into a dict
        result = dict(zip(chain([selected_kmers_unique[0, 0]], selected_kmers_unique[selected_kmers_unique_idx, 0]), selected_kmers_entries_split))
    else:
        # If we have no minimizers we return nothing, sorry
        result = dict()

    return MinimizerIndex(
        index=result,
        kmers=set(result.keys()),
    )

def cartesian_product(*arrays):
    la = len(arrays)
    dtype = np.result_type(*arrays)
    arr = np.empty([len(a) for a in arrays] + [la], dtype=dtype)
    for i, a in enumerate(np.ix_(*arrays)):
        arr[...,i] = a
    return arr.reshape(-1, la)

def _longest_increasing_subsequence_impl(
    n,
    matches,
):
    #print(matches)
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
                kmer_entries_target[kmer_entries_target[:,1] == True][:,0],
                kmer_entries_query[kmer_entries_query[:,1] == True][:,0],
            ),
            cartesian_product(
                kmer_entries_target[kmer_entries_target[:,1] == False][:,0],
                kmer_entries_query[kmer_entries_query[:,1] == False][:,0],
            )),
            axis=0,
        )
    matches = np.sort(matches[1:], axis=0)
    region_match = find_longest_increasing_subsequence(matches)
    
    return region_match

MAX_REF_SIZE = 22000000


def load_nucl_to_np(nucl_seq: str, kmer_len: int):
    print(f"SEQ=[[{nucl_seq[:100]}]]")
    seq_len = len(nucl_seq)
    mask = generate_mask(kmer_len)
    kmer_hash = 0
    kmers = []
    w1, w2, w3, w4, w5 = 0, 0, 0, 0, 0
    for pos in range(0, kmer_len):
        kmer_hash = ((kmer_hash << 2) | MAPPING[nucl_seq[pos]]) & mask
    w1 = kmer_hash
    for pos in range(kmer_len, seq_len):
        kmer_hash = ((kmer_hash << 2) | MAPPING[nucl_seq[pos]]) & mask
        w5 = w4
        w4 = w3
        w3 = w2
        w2 = w1
        w1 = kmer_hash
        kmers.append(min(w1, w2, w3, w4, w5))
    return np.array(kmers)

def run_aligner_pipeline(
    reference_file_path: str,
    reads_file_path: str,
    output_file_path: str,
    kmer_len: int,
    window_len: int,
    kmers_cutoff_f: float,
    score_match: int,
    score_mismatch: int,
    score_gap: int,
):
    tracemalloc.start()
    np.set_printoptions(threshold=sys.maxsize)
    LOGS.cli.info(f"Invoked CLI with the following args: {' '.join(sys.argv)}")
    with TaskReporter("Sequence read alignnment") as reporter:
        # Fast read
        with reporter.task("Load target sequence"):
            ref_seq = np.empty(MAX_REF_SIZE)
            ref_loaded = False
            all_seq = ""
            with open(reference_file_path) as ref_fh:
                for line in ref_fh:
                    if line[0] == '>':
                        if ref_loaded:
                            break
                        ref_loaded = True
                        continue
                    else:
                        all_seq += line.rstrip()
            
        with reporter.task("Create minimizer target index") as target_index_task:
            ref_seq_p = load_nucl_to_np(all_seq, kmer_len)
            print(len(ref_seq_p))
            print("OK")




@click.command()
@click.argument('target-fasta', help="Target sequence FASTA file path")
@click.argument('query-fasta', help="Query sequences FASTA file path")
@click.argument('output', default="output.txt", help="Output file path")
@click.option('--kmer-len', default=15, show_default=True)
@click.option('--window-len', default=5, show_default=True)
@click.option('--f', default=0.001, show_default=True, help="Portion of top frequent kmers to be removed from the index (must be in range 0 to 1 inclusive)")
@click.option('--score-match', default=1, show_default=True)
@click.option('--score-mismatch', default=1, show_default=True)
@click.option('--score-gap', default=1, show_default=True)
def run_alignment_cli(target_fasta, query_fasta, output, kmer_len, window_len, f, score_match, score_mismatch, score_gap):
    run_aligner_pipeline(
        reference_file_path=target_fasta,
        reads_file_path=query_fasta,
        output_file_path=output,
        kmer_len=kmer_len,
        window_len=window_len,
        kmers_cutoff_f=f,
        score_match=score_match,
        score_mismatch=score_mismatch,
        score_gap=score_gap,
    )
    

if __name__ == '__main__':
    run_alignment_cli()