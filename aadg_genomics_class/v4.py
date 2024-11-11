import sys
from Bio import SeqRecord, SeqIO

import gc
from dataclasses import dataclass
import sys
import copy
from aadg_genomics_class.monitoring.logs import LOGS
from aadg_genomics_class.monitoring.task_reporter import TaskReporter, monitor_mem_snapshot
from aadg_genomics_class import click_utils as click
from aadg_genomics_class.new_aligner2 import align_seq
from aadg_genomics_class.new_aligner2np import doit
from aadg_genomics_class.edit_check import levenshtein

from typing import Dict, Any, Set, Optional
from itertools import chain
from numpy.lib.stride_tricks import sliding_window_view

import csv
import tracemalloc

import math
import numpy as np
from typing import Iterable


score_insertion    = -2
score_deletion     = -2
score_substitution = -1
score_match        =  2
def seq_align(x, y):
  """
  input:  two strings: x and y
  output: an array with a length of |y| that contains the score for the alignment 
          between x and y
  """
  global score_insertion
  global score_deletion
  global score_substitution
  global score_match 
  row = y
  column = x 
  minLen = len(y)
  prev = [0 for i in range(minLen + 1)]
  current = [0 for i in range(minLen + 1)]


  for i in range(1, minLen + 1):
    prev[i] = prev[i-1] + score_insertion
  
  current[0] = 0
  for j in range(1, len(column) + 1):
    current[0] += score_deletion
    for i in range(1, minLen + 1):
      if row[i-1] == column[j-1]:
        try:
          current[i] = max(current[i-1] + score_insertion, prev[i-1] + score_match, prev[i] + score_deletion)
        except:
          pdb.set_trace()
      else:
        current[i] = max(current[i-1] + score_insertion, prev[i-1] + score_substitution, prev[i] + score_deletion)
    prev = copy.deepcopy(current) # for python its very import to use deepcopy here

  return current 

def partition_nw_scores(scoreL, scoreR):
  max_index = 0
  max_sum = float('-Inf')
  for i, (l, r) in enumerate(zip(scoreL, scoreR[::-1])):
    # calculate the diagonal maximum index
    if sum([l, r]) > max_sum:
      max_sum = sum([l, r])
      max_index = i
  return max_index 

def run_nw(x, y):
  global score_insertion
  global score_deletion
  global score_substitution
  global score_match 
  # M records is the score array
  # Path stores the path information, inside of Path:
  # d denotes: diagnal
  # u denotes: up
  # l denotes: left
  M = np.zeros((len(x) + 1, len(y) + 1))
  Path = np.empty((len(x) + 1, len(y) + 1), dtype=object)

  for i in range(1, len(y) + 1):
    M[0][i] = M[0][i-1] + score_insertion
    Path[0][i] = "l"
  for j in range(1, len(x) + 1):
    M[j][0] = M[j-1][0] + score_deletion
    Path[j][0] = "u"
  
  for i in range(1, len(x) + 1):
    for j in range(1, len(y) + 1):
      if x[i-1] == y[j-1]:
        M[i][j] = max(M[i-1][j-1] + score_match, M[i-1][j] + score_insertion, M[i][j-1] + score_deletion)
        if M[i][j] == M[i-1][j-1] + score_match:
          Path[i][j] =  "d"
        elif M[i][j] == M[i-1][j] + score_insertion:
          Path[i][j] = "u"
        else:
          Path[i][j] = "l"
      else:
        M[i][j] = max(M[i-1][j-1] + score_substitution, M[i-1][j] + score_insertion, M[i][j-1] + score_deletion)
        if M[i][j] == M[i-1][j-1] + score_substitution:
          Path[i][j] =  "d"
        elif M[i][j] == M[i-1][j] + score_insertion:
          Path[i][j] = "u"
        else:
          Path[i][j] = "l"

  pad_left = 0
  i = len(x)
  j = len(y)
  while Path[i][j]:
    if Path[i][j] == "d":
      pad_left = 0
      i -= 1
      j -= 1
    elif Path[i][j] == "u":
      pad_left += 1
      i -= 1
    elif Path[i][j] == "l":
      pad_left = 0
      j -= 1
  return pad_left


def run_hirschberge(x, y):
  pad_left = 0
#  x is being row-wise iterated (out-most for loop)
#  y is being column-wise iterated (inner-most of the for loop)
  if len(x) == 0 or len(y) == 0:
    if len(x) == 0:
      pad_left = 0
    else:
      pad_left = len(x)
  elif len(x) == 1 or len(y) == 1:
    pad_left = run_nw(x, y)
    # concatenate into string
    #row = "".join(row)
  else:
    xlen = len(x)
    xmid = xlen//2
    ylen = len(y)

    scoreL = seq_align(x[:xmid], y)
    scoreR = seq_align(x[xmid:][::-1], y[::-1])
    ymid = partition_nw_scores(scoreL, scoreR)
    pad_left_l = run_hirschberge(x[:xmid], y[:ymid])
    pad_left_r = run_hirschberge(x[xmid:], y[ymid:])

    pad_left = pad_left_l if pad_left_l < len(x[:xmid]) else pad_left_r + pad_left_l
    #pad_right = pad_right_r if pad_right_l < len(x[:xmid]) else pad_right_r + pad_right_l

  return pad_left

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

RR_MAPPING = ["C", "A", "T", "G"]

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
    keys = np.array(list(index.keys()))
    counts = np.array([index[kmer].size for kmer in keys])
    start_index = keys.size - math.floor(keys.size * kmers_cutoff_f)
    kmers_to_remove = np.lexsort((keys, counts))[start_index:]
    kmers_before = len(index)
    for kmer_pos in kmers_to_remove:
        index.pop(keys[kmer_pos], None)
        #index.kmers.remove(keys[kmer_pos])
    kmers_after = len(index)
    LOGS.prefilter.info(f"Reduced target kmer count by prefiltering: {kmers_before} -> {kmers_after} (Eliminated {math.floor((kmers_before-kmers_after)/kmers_before*100000)/1000}% top kmers with f={kmers_cutoff_f})")


def normalize_pos(pos, len):
    return min(max(pos, 0), len)


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
    #r_kmers_min_pos = np.add(np.argmin(sliding_window_view(r_kmers, window_shape=window_len), axis=1), np.arange(0, sequence_len - window_len + 1))
    return kmers_min_pos#, r_kmers_min_pos



def get_minimizers(
    seq_arr,
    kmer_len,
    window_len,
):
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

    # Remove duplicates
    selected_kmers = selected_kmers[selected_kmers[:, 0].argsort()]
    selected_kmers = np.unique(selected_kmers, axis=0)

    # This part performs group by using the kmer value
    selected_kmers_unique_idx = np.unique(selected_kmers[:, 0], return_index=True)[1][1:]
    selected_kmers_entries_split = np.split(selected_kmers[:, 1], selected_kmers_unique_idx)

    if len(selected_kmers) > 0:
        # We zip all kmers into a dict
        result = dict(zip(chain([selected_kmers[0, 0]], selected_kmers[selected_kmers_unique_idx, 0]), selected_kmers_entries_split))
    else:
        # If we have no minimizers we return nothing, sorry
        result = dict()
    return result


def cartesian_product(*arrays):
    la = len(arrays)
    dtype = np.result_type(*arrays)
    arr = np.empty([len(a) for a in arrays] + [la], dtype=dtype)
    for i, a in enumerate(np.ix_(*arrays)):
        arr[...,i] = a
    return arr.reshape(-1, la)

def estimate_distance(
      a_arr,
      b_arr,
):
    a = get_minimizers(a_arr, 10, 2)
    b = get_minimizers(b_arr, 10, 2)
    result = 0
    for key in a:
        if key in b:
            result += 1
    return result

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
    gc.disable()
    #tracemalloc.start()
    np.set_printoptions(threshold=sys.maxsize)
    LOGS.cli.info(f"Invoked CLI with the following args: {' '.join(sys.argv)}")
    
    expected_coords = {}
    with open('./data_big/reads20Mb.txt', mode ='r')as file:
        csvFile = csv.reader(file, delimiter='\t')
        expected_coords = {line[0]: (int(line[1]), int(line[2])) for line in csvFile}

    with TaskReporter("Sequence read alignnment") as reporter:

        with reporter.task("Load target sequence"):
            reference_records, reference_ids = format_sequences(SeqIO.parse(reference_file_path, "fasta"))

        if kmer_len > MAX_KMER_SIZE:
            kmer_len = MAX_KMER_SIZE

        with reporter.task("Create minimizer target index") as target_index_task:
                ref_loaded = False
                all_seq = ""
                all_seq_len = 0
                index_offset = 0
                CHUNK_SIZE = 1000000 # Chunk size should be around 1000000

                ref_index = dict()
                with open(reference_file_path) as ref_fh:
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

                            # Shift all indices according to what we loaded already
                            selected_kmers[:,1] += index_offset

                            # This part performs group by using the kmer value
                            selected_kmers_unique_idx = np.unique(selected_kmers[:, 0], return_index=True)[1][1:]
                            selected_kmers_entries_split = np.split(selected_kmers[:, 1], selected_kmers_unique_idx)

                            if len(selected_kmers) > 0:
                                # We zip all kmers into a dict
                                i = 0
                                for k, v in zip(chain([selected_kmers[0, 0]], selected_kmers[selected_kmers_unique_idx, 0]), selected_kmers_entries_split):
                                    i += 1
                                    # TODO: REMOVE SOME FROM INDEX
                                    # if i >= 20 and len(v) == 1:
                                    #     i = 0
                                    #     continue
                                    if k in ref_index:
                                        ref_index[k] = np.concatenate((ref_index[k], v), axis=0)
                                    else:
                                        ref_index[k] = v
                            else:
                                # If we have no minimizers we return nothing, sorry
                                pass

                            index_offset += all_seq_len
                            all_seq_len = 0
                            all_seq = ""
                            print(f"PROCESSED ENTIRE CHUNK! offset={index_offset}")
                            del selected_kmers_unique_idx
                            del selected_kmers_entries_split
                            gc.collect()
                        if line[0] == '>':
                            if ref_loaded:
                                break
                            ref_loaded = True
                            continue

        gc_collect_cnt = 300
        with open(output_file_path, 'w') as output_file:
            query_id = ""
            query_seq = ""
            with open(reads_file_path) as reads_fh:
                for line in chain(reads_fh, [">"]):
                    if line[0] == '>' and len(query_seq) > 0:
                        query_seq = MAPPING_FN(np.array(list(query_seq)))
                        # Process
                        if gc_collect_cnt > 299:
                           gc_collect_cnt = 0
                           gc.collect()
                        gc_collect_cnt += 1
                        # if query_id not in ['read_937', 'read_961', 'read_972', 'read_96', 'read_126', 'read_394', 'read_561', 'read_693', 'read_771', 'read_794', 'read_817', 'read_903', 'read_910', 'read_937', 'read_972', 'read_961']:
                        #    continue
                        with reporter.task(f"Load query '{query_id}'") as query_task:
                            try:
                                max_diff = round(len(query_seq)*1.3)
                                
                                with query_task.task('Get minimizers'):
                                    min_index_query = get_minimizers(
                                        query_seq,
                                        kmer_len=kmer_len,
                                        window_len=window_len,
                                    )

                                with query_task.task('Extend'):
                                    common_kmers = []
                                    for key in min_index_query:
                                        if key in ref_index:
                                            common_kmers.append(key)

                                    matches = np.array([[-1, -1]])
                                    for kmer in common_kmers:
                                        kmer_entries_target, kmer_entries_query = ref_index[kmer], min_index_query[kmer]
                                        matches = np.concatenate((
                                            matches,
                                            cartesian_product(
                                                kmer_entries_target,
                                                kmer_entries_query,
                                            )),
                                            axis=0,
                                        )
                                    matches = matches[matches[:, 0].argsort()]
                                    matches = matches[1:]
                                    n = len(matches)
                                    
                                    match_score, match_start_t, match_end_t, match_start_q, match_end_q = -max_diff, 0, 0, 0, 0

                                    # print("ALL MATCH:")
                                    # print(matches)
                                    # print("END")

                                    if n == 0:
                                        pass
                                    elif n == 1:
                                        match_score, match_start_t, match_end_t, match_start_q, match_end_q = 0, matches[0, 0], matches[0, 0], matches[0, 1], matches[0, 1]
                                    else:
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
                                        q = [current_node]*longest_seq_len 
                                        for j in range(longest_seq_len-1, 0, -1):
                                            current_node = parent[current_node]
                                            q[j-1] = current_node

                                        lis = np.take(matches, q, axis=0)
                                        for i in range(longest_seq_len):
                                            start = i
                                            end = longest_seq_len
                                            while start <= end:
                                                middle = (start + end) // 2
                                                if middle == longest_seq_len:
                                                    start = longest_seq_len
                                                    break
                                                if lis[middle, 0] < lis[i, 0] + max_diff - lis[i, 1]:
                                                    start = middle + 1
                                                else:
                                                    end = middle - 1
                                            # Window is i till end
                                            # print(f"Start from {i} (till {start} whcih has value") #[{lis[start, 0]}, {lis[start, 1]}])
                                            estimated_matches_q = (lis[start, 1] if start < longest_seq_len else max_diff) - lis[i, 1]
                                            estimated_matches_t = (lis[start, 0] if start < longest_seq_len else lis[start-1, 0]) - lis[i, 0]
                                            score = min(estimated_matches_q, estimated_matches_t)*min(estimated_matches_q, estimated_matches_t) - np.sum(np.diff(lis[i:start, 0], axis=0))
                                            # print(lis[i:start])
                                            # print(f"LAST ELEMENT IS {lis[i:start][-1]} where start={start} and l-1={longest_seq_len-1}")
                                            # print(f"score = {score}")
                                            if score > match_score:
                                                match_end_index_pos = max(i, min(start-1, longest_seq_len-1))
                                                match_score, match_start_t, match_end_t, match_start_q, match_end_q = score, lis[i, 0], lis[match_end_index_pos, 0], lis[i, 1], lis[match_end_index_pos, 1]
                                                #print(f"ACCEPTED SCORE: {match_start_t} - {match_end_t}")
                                            if start == longest_seq_len:
                                                break

                                    #print(f"SCORE: Match score is {match_score}")
                                    #print(f"SCORE: Match around {match_start_t} - {match_end_t}")
                                    #sys.exit(1)

                                    # q_begin, q_end, t_begin, t_end, list_length

                                with query_task.task('Align'):
                                    #print(f"Alignment around: {match_start_t} - {match_end_t} (query: {match_start_q} - {match_end_q})")
                                    #monitor_mem_snapshot('CHECKPOINT_1')
                                    # t_begin, t_end = align(
                                    #     region=region_match,
                                    #     target_seq=reference_records[reference_ids[0]],
                                    #     query_seq=query_seq,
                                    #     kmer_len=kmer_len,
                                    #     full_query_len=len(query_seq),
                                    #     score_match=score_match,
                                    #     score_mismatch=score_mismatch,
                                    #     score_gap=score_gap,
                                    # )

                                    relative_extension = kmer_len // 2 + 1

                                    if abs(match_end_t - match_start_t) > max_diff + relative_extension:
                                       # FAILED MAPPING!
                                       print(f"Failed sequence, reason: {match_start_t} - {match_end_t} ({abs(match_end_t - match_start_t)})")
                                       output_file.write(f"{query_id} status=FAIL\n")
                                       continue

                                    target_seq = reference_records[reference_ids[0]]

                                    q_begin, q_end = 0, len(query_seq)
                                    t_begin, t_end = match_start_t - match_start_q - relative_extension, match_end_t + (len(query_seq)-match_end_q) + relative_extension
                                    # q_begin, q_end = match_start_q-relative_extension, match_end_q+(kmer_len-1)+relative_extension
                                    # t_begin, t_end = match_start_t-relative_extension, min(match_end_t, match_start_t + len(query_seq))+(kmer_len-1)+relative_extension

                                    q_begin, q_end = normalize_pos(q_begin, len(query_seq)), normalize_pos(q_end, len(query_seq))
                                    t_begin, t_end = normalize_pos(t_begin, len(target_seq)), normalize_pos(t_end, len(target_seq))

                                    #print(f"PRE-ALIGNED: {t_begin} - {t_end} (query: {q_begin} - {q_end})")
                                    # for record in SeqIO.parse(reference_file_path, "fasta"):
                                    #     found_pos = str(record.seq).find("".join([RR_MAPPING[i] for i in target_seq[t_begin:t_end].tolist()]))
                                    #     print(f"ACTUAL TARGET POS VALIDATED: {found_pos}")
                                    #     break

                                    # print("TARGET!!!!")
                                    # print("".join([RR_MAPPING[i] for i in target_seq[t_begin:t_end].tolist()]))
                                    # print("QUERY!!!")
                                    # print("".join([RR_MAPPING[i] for i in query_seq[100:q_end].tolist()]))

                                    # t_pad_left, t_pad_right, q_pad_left, q_pad_right = nw_align(
                                    #     target=target_seq[t_begin:t_end],
                                    #     query=query_seq[q_begin:q_end],
                                    #     score_match=score_match,
                                    #     score_mismatch=score_mismatch,
                                    #     score_gap=score_gap,
                                    # )

                                    # q_begin, q_end = q_begin + t_pad_left, q_end - t_pad_right
                                    # t_begin, t_end = t_begin + q_pad_left, t_end - q_pad_right

                                    if False:
                                        t_begin_pad = run_hirschberge(target_seq[t_begin:t_end], query_seq[q_begin:])
                                        t_end_pad = run_hirschberge(target_seq[t_begin+t_begin_pad:t_end][::-1], query_seq[:q_end][::-1])
                                        t_begin += t_begin_pad
                                        t_end -= t_end_pad

                                    # with query_task.task('Align Method=REF'):
                                    #     t_begin_pad, t_end_pad = align_seq(
                                    #         "".join([RR_MAPPING[i] for i in target_seq[t_begin:t_end].tolist()]),
                                    #         "".join([RR_MAPPING[i] for i in query_seq.tolist()])
                                    #     )
                                    realign_mode = 0
                                    with query_task.task('Align Method=BWT'):
                                        t_begin_pad, t_end_pad, should_realign_right = doit(
                                            "".join([RR_MAPPING[i] for i in query_seq.tolist()]),
                                            "".join([RR_MAPPING[i] for i in target_seq[t_begin:t_end].tolist()]),
                                        )
                                        
                                    if should_realign_right:
                                       realign_mode = 1
                                    if abs(t_end-(t_end_pad or 0)-t_begin-(t_begin_pad or 0)) > len(query_seq)*1.05:
                                       realign_mode = 2
                                       if t_begin_pad is not None:
                                         t_begin += t_begin_pad
                                       if t_end_pad is not None:
                                         t_end -= t_end_pad

                                    if realign_mode > 0:
                                    #    print(f"HMM? SHOULD REALIGN!!!! :000 ALIGN_MODE={realign_mode}")
                                       with query_task.task('Align Method=REF'):
                                            # print("TARGET!!!!")
                                            # print("".join([RR_MAPPING[i] for i in target_seq[t_begin:t_end].tolist()]))
                                            # print("QUERY!!!")
                                            # print("".join([RR_MAPPING[i] for i in query_seq.tolist()]))

                                            t_begin_pad, t_end_pad = align_seq(
                                                "".join([RR_MAPPING[i] for i in target_seq[t_begin:t_end].tolist()]),
                                                "".join([RR_MAPPING[i] for i in query_seq.tolist()]),
                                                align_mode=realign_mode,
                                            )

                                    if t_begin_pad is not None:
                                        t_begin += t_begin_pad
                                    if t_end_pad is not None:
                                        t_end -= t_end_pad


                                    # print("TARGET!!!!")
                                    # print("".join([RR_MAPPING[i] for i in target_seq[t_begin:t_end].tolist()]))
                                    # print("QUERY!!!")
                                    # print("".join([RR_MAPPING[i] for i in query_seq.tolist()]))


                                    #print(f"ALIGNED: {t_begin} - {t_end} (pd: {t_begin_pad}, {t_end_pad} query: {q_begin} - {q_end})")

                                       
                                    # sys.exit(1)

                                    # print("TARGET!!!!")
                                    # print("".join([RR_MAPPING[i] for i in target_seq[t_begin:t_end].tolist()]))
                                    # print("QUERY!!!")
                                    # print("".join([RR_MAPPING[i] for i in query_seq.tolist()]))

                                    #est_edit_dist = estimate_distance(target_seq[t_begin:t_end], query_seq) #levenshtein("".join([RR_MAPPING[i] for i in target_seq[t_begin:t_end].tolist()]), "".join([RR_MAPPING[i] for i in query_seq.tolist()]))
                                    # est_edit_dist = levenshtein(
                                    #    "".join([RR_MAPPING[i] for i in target_seq[t_begin:t_end].tolist()]),
                                    #    "".join([RR_MAPPING[i] for i in query_seq.tolist()]),
                                    #    177, 2, 2, 1
                                    # )
                                    est_edit_dist = 0
                                    if est_edit_dist is None:
                                       est_edit_dist = 178

                                if query_id in expected_coords:
                                   diff_start = expected_coords[query_id][0]-t_begin
                                   diff_end = expected_coords[query_id][1]-t_end
                                   #print(f"TOTAL DIFF: {max(abs(diff_start), abs(diff_end))}")
                                   status = "OK" if max(abs(diff_start), abs(diff_end)) < 20 else "BAD"
                                   qual = "AA" if abs(diff_start)+abs(diff_end) < 10 else ("AB" if abs(diff_start)+abs(diff_end) < 20 else ("C" if max(abs(diff_start), abs(diff_end)) < 20 else "D"))
                                   output_file.write(f"{'FUCK' if est_edit_dist >= 177999 else 'X'} | {est_edit_dist} | {query_id} status={status} qual={qual} diff=<{diff_start}, {diff_end}>  | {t_begin} {t_end} | pad: {t_begin_pad}, {t_end_pad} | {'REALIGNED'+realign_mode if should_realign_right else ''} \n")
                                else:
                                    output_file.write(f"{query_id} {t_begin} {t_end}\n")
                            except Exception as e:
                                query_task.fail(e)
                    if line[0] == '>':
                        # Process end
                        query_seq = ""
                        query_id = line[1:].strip()
                    else:
                        query_seq += line.rstrip()
            LOGS.cli.info(f"Wrote records to {output_file_path}")


@click.command()
@click.argument('target-fasta', help="Target sequence FASTA file path")
@click.argument('query-fasta', help="Query sequences FASTA file path")
@click.argument('output', default="output.txt", help="Output file path")
# First attempt:
# opt2 (15, 5)
# opt3 (20, 15)
# TU BYŁO (20, 15)
@click.option('--kmer-len', default=18, show_default=True)
@click.option('--window-len', default=8, show_default=True)
@click.option('--f', default=0.001, show_default=True, help="Portion of top frequent kmers to be removed from the index (must be in range 0 to 1 inclusive)")
@click.option('--score-match', default=1, show_default=True)
@click.option('--score-mismatch', default=5, show_default=True)
@click.option('--score-gap', default=10, show_default=True)
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