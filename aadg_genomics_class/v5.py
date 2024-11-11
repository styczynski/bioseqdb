import sys

import gc
import sys
import copy
import os

from typing import Dict, Any, Set, Optional, Tuple
from itertools import chain
from numpy.lib.stride_tricks import sliding_window_view
from operator import itemgetter
from collections import defaultdict

import csv
import numpy as np


MASKS: Optional[Dict[int, int]] = None
MAX_KMER_SIZE = 64

MAPPING_DOLLAR = 0
MAPPING = dict(
    C=1,
    A=2,
    T=3,
    G=4,
    #a=1,
    #c=0,
    #g=3,
    #G=4,
    #t=2,
    #T=3,
)

ALPHABET = set(MAPPING.values())
ALPHABET_DOLLAR = set([*ALPHABET, MAPPING_DOLLAR])

RR_MAPPING = ["C", "A", "T", "G"]

COMPLEMENT_MAPPING = {
    1: 2,
    0: 3,
    3: 0,
    2: 1,
}

MAPPING_FN = np.vectorize(MAPPING.get, otypes=[np.uint8])
COMPLEMENT_MAPPING_FN = np.vectorize(COMPLEMENT_MAPPING.get)

# DP algorithm adapted from Langmead's notebooks
def align_dp_trace(D, x, y):
    ''' Backtrace edit-distance matrix D for strings x and y '''
    i, j = len(x), len(y)
    while i > 0:
        diag, vert, horz = sys.maxsize, sys.maxsize, sys.maxsize
        delt = None
        if i > 0 and j > 0:
            delt = 0 if x[i-1] == y[j-1] else 1
            diag = D[i-1, j-1] + delt
        if i > 0:
            vert = D[i-1, j] + 1
        if j > 0:
            horz = D[i, j-1] + 1
        if diag <= vert and diag <= horz:
            # diagonal was best
            i -= 1; j -= 1
        elif vert <= horz:
            # vertical was best; this is an insertion in x w/r/t y
            i -= 1
        else:
            # horizontal was best
            j -= 1
    # j = offset of the first (leftmost) character of t involved in the
    # alignment
    return j

def align_dp_k_edit(p, t):
    ''' Find the alignment of p to a substring of t with the fewest edits.  
        Return the edit distance and the coordinates of the substring. '''
    D = np.zeros((len(p)+1, len(t)+1), dtype=int)
    # Note: First row gets zeros.  First column initialized as usual.
    D[1:, 0] = range(1, len(p)+1)
    for i in range(1, len(p)+1):
        for j in range(1, len(t)+1):
            delt = 1 if p[i-1] != t[j-1] else 0
            D[i, j] = min(D[i-1, j-1] + delt, D[i-1, j] + 1, D[i, j-1] + 1)
    # Find minimum edit distance in last row
    mnJ, mn = None, len(p) + len(t)
    for j in range(len(t)+1):
        if D[len(p), j] < mn:
            mnJ, mn = j, D[len(p), j]
    # Backtrace; note: stops as soon as it gets to first row
    off = align_dp_trace(D, p, t[:mnJ])
    # Return edit distance and t coordinates of aligned substring
    return mn, off, mnJ


def run_match_align_dp(target, query, align_mode=1):
    for (k, step) in [(15, 11), (10, 11), (8, 5)]:
        suff_len_factor = 0.4
        suff_len_factor2 = 0.6

        print(f"align_seq(): ALIGNER_MODE {align_mode}")
        suff_len = round(len(query) * suff_len_factor)
        suff_len2 = round(len(query) * suff_len_factor2)

        target_suffix = target[suff_len-k:]
        query_suffix = query[(len(query)-suff_len):]

        edist = len(query)//9
        kmers = defaultdict(list)

        mask = generate_mask(k)

        uadd = np.frompyfunc(lambda x, y: ((x << 2) | y) & mask, 2, 1)

        # This computes values for kmers
        kmers_target = uadd.accumulate(target_suffix, dtype=object).astype(int)
        # for i in range(0, len(target)-k+1, step):
        #         kmers[np.sum(target[i:i+k])].append(i)
        for i in range(0, len(kmers_target), step):
            kmers[kmers_target[i]].append(i)

        hits = []
        kmers_query = uadd.accumulate(query_suffix, dtype=object).astype(int)
        #for i in range(0, len(query)-k+1, step+1):
            #for j in kmers[np.sum(query[i:i+k])]:
        for i in range(0, len(kmers_query), step+1):
            for j in kmers[kmers_query[i]]:
                lf = max(0, j-i-edist)
                rt = min(len(target_suffix), j-i+len(query_suffix)+edist)
                mn, soff, eoff = align_dp_k_edit(query_suffix, target_suffix[lf:rt])
                soff += lf
                eoff += lf
                if mn <= edist:
                    hits.append((mn, soff, eoff))
        hits.sort()
        if hits:
            return 0, len(target_suffix)-hits[0][2]
        if align_mode == 1:
            return 0, 0
    return 0, 0

def merge(x: np.array, SA12: np.array, SA3: np.array) -> np.array:
    "Merge the suffixes in sorted SA12 and SA3."
    ISA = np.zeros((len(x),), dtype='int')
    for i in range(len(SA12)):
        ISA[SA12[i]] = i
    SA = np.zeros((len(x),), dtype='int')
    idx = 0
    i, j = 0, 0
    while i < len(SA12) and j < len(SA3):
        if less(x, SA12[i], SA3[j], ISA):
            SA[idx] = SA12[i]
            idx += 1
            i += 1
        else:
            SA[idx] = SA3[j]
            idx += 1
            j += 1
    if i < len(SA12):
        SA[idx:len(SA)] = SA12[i:]
    elif j < len(SA3):
        SA[idx:len(SA)] = SA3[j:]
    return SA


def u_idx(i: int, m: int) -> int:
    "Map indices in u back to indices in the original string."
    if i < m:
        return 1 + 3 * i
    else:
        return 2 + 3 * (i - m - 1)


def safe_idx(x: np.array, i: int) -> int:
    "Hack to get zero if we index beyond the end."
    return 0 if i >= len(x) else x[i]

def symbcount(x: np.array, asize: int) -> np.array:
    "Count how often we see each character in the alphabet."
    counts = np.zeros((asize,), dtype="int")
    for c in x:
        counts[c] += 1
    return counts

def cumsum(counts: np.array) -> np.array:
    "Compute the cumulative sum from the character count."
    res = np.zeros((len(counts, )), dtype='int')
    acc = 0
    for i, k in enumerate(counts):
        res[i] = acc
        acc += k
    return res

def bucket_sort(x: np.array, asize: int,
                idx: np.array, offset: int = 0) -> np.array:
    "Sort indices in idx according to x[i + offset]."
    sort_symbs = np.array([safe_idx(x, i + offset) for i in idx])
    counts = symbcount(sort_symbs, asize)
    buckets = cumsum(counts)
    out = np.zeros((len(idx),), dtype='int')
    for i in idx:
        bucket = safe_idx(x, i + offset)
        out[buckets[bucket]] = i
        buckets[bucket] += 1
    return out

def radix3(x: np.array, asize: int, idx: np.array) -> np.array:
    "Sort indices in idx according to their first three letters in x."
    idx = bucket_sort(x, asize, idx, 2)
    idx = bucket_sort(x, asize, idx, 1)
    return bucket_sort(x, asize, idx)

def triplet(x: np.array, i: int) -> Tuple[int, int, int]:
    "Extract the triplet (x[i],x[i+1],x[i+2])."
    return safe_idx(x, i), safe_idx(x, i + 1), safe_idx(x, i + 2)

def collect_alphabet(x: np.array, idx: np.array) -> Tuple[np.array, int]:
    "Map the triplets starting at idx to a new alphabet."
    alpha = np.zeros((len(x),), dtype='int')
    value = 1
    last_trip = -1, -1, -1
    for i in idx:
        trip = triplet(x, i)
        if trip != last_trip:
            value += 1
            last_trip = trip
        alpha[i] = value
    return alpha, value - 1

def build_u(x: np.array, alpha: np.array) -> np.array:
    "Construct u string, using 1 as central sentinel."
    a = np.array([alpha[i] for i in range(1, len(x), 3)] +
                 [1] +
                 [alpha[i] for i in range(2, len(x), 3)])
    return a

def less(x: np.array, i: int, j: int, ISA: np.array) -> bool:
    "Check if x[i:] < x[j:] using the inverse suffix array for SA12."
    a: int = safe_idx(x, i)
    b: int = safe_idx(x, j)
    if a < b: return True
    if a > b: return False
    if i % 3 != 0 and j % 3 != 0: return ISA[i] < ISA[j]
    return less(x, i + 1, j + 1, ISA)

def skew_rec(x: np.array, asize: int) -> np.array:
    "skew/DC3 SA construction algorithm."

    SA12 = np.array([i for i in range(len(x)) if i % 3 != 0])

    SA12 = radix3(x, asize, SA12)
    new_alpha, new_asize = collect_alphabet(x, SA12)
    if new_asize < len(SA12):
        # Recursively sort SA12
        u = build_u(x, new_alpha)
        sa_u = skew_rec(u, new_asize + 2)
        m = len(sa_u) // 2
        SA12 = np.array([u_idx(i, m) for i in sa_u if i != m])

    if len(x) % 3 == 1:
        SA3 = np.array([len(x) - 1] + [i - 1 for i in SA12 if i % 3 == 1])
    else:
        SA3 = np.array([i - 1 for i in SA12 if i % 3 == 1])
    SA3 = bucket_sort(x, asize, SA3)
    return merge(x, SA12, SA3)

# DUŻO SYFU
C = {}
O = {}
D = []

# rewards/penalties
gap_open = 3
gap_ext = 1
mismatch = 1
match = 0

# option switches
sub_mat = {}

num_prunes = 0
# insertion -> 1
# delection -> 2
# match -> 0
# mismatch -> 3
# start -> 4

def compute_C(totals):
    """compute C, the number of lexicographically greater symbols in the ref"""
    #C = {0: 0, 1: 0, 2: 0, 3: 0, MAPPING_DOLLAR: 0}
    C = {v: 0 for v in ALPHABET_DOLLAR}
    for k in ALPHABET:
        for ref in ALPHABET:
            if ref < k:
                C[k] += totals[ref]

    return C


def compute_D(s, C, Oprime, bw):
    """compute estimated lower bounds of differences in substring s[0:i] for all  in [0,len(s)]"""
    k = 1
    l = len(bw)-2
    z = 0
    D = [0] * len(s)

    for i in range(0, len(s)):
        k = C[s[i]] + Oprime[s[i]][k-1] + 1
        l = C[s[i]] + Oprime[s[i]][l]
        # k = C[s[i]] + 1
        # l = C[s[i]]
        if k > l:
            k = 1
            l = len(bw)-1
            z += 1
        D[i] = z

    return D


def get_D(i):
    """enforce condition that if D[i] is set to -1, its value will be considered as 0"""
    if i < 0:
        return 0
    else:
        return D[i]


def get_O(char, index):
    """see get_D()"""
    if index < 0:
        return 0
    else:
        return O[char][index]


def inexact_recursion(s, i, diff, k, l, prev_type):
    """search bwt recursively and tolerate errors"""
    
    global num_prunes

    # pruning based on estimated mistakes
    if diff < get_D(i):
        num_prunes += 1
        return set()

    # end of query condition
    temp = set()
    if i < 0:
        for j in range(k, l+1):
            temp.add((j, diff))
        return temp

    # search
    sa_idx = set()  # set of suffix array indices at which a match starts
    
    # Insertion
    if prev_type == 1:
        sa_idx = sa_idx.union(inexact_recursion(s, i-1, diff-gap_ext, k, l, 1))
    else:
        sa_idx = sa_idx.union(inexact_recursion(s, i-1, diff-gap_ext-gap_open, k, l, 1))

    for char in ALPHABET:
        temp_k = C[char] + get_O(char, k-1) + 1
        temp_l = C[char] + get_O(char, l)
    
        if temp_k <= temp_l:
            # Deletion
            if prev_type == 2:
                sa_idx = sa_idx.union(inexact_recursion(s, i, diff-gap_ext, temp_k, temp_l, 2))
            else:
                sa_idx = sa_idx.union(inexact_recursion(s, i, diff-gap_ext-gap_open, temp_k, temp_l, 2))
            if char == s[i]:
                # Match!
                sa_idx = sa_idx.union(inexact_recursion(s, i-1, diff+match, temp_k, temp_l, 0))
                
            else:
                # Mismatch
                if sub_mat:
                    sa_idx = sa_idx.union(inexact_recursion(s, i-1, diff-mismatch*sub_mat[(s[i], char)],
                                                            temp_k, temp_l, 3))
                else:
                    sa_idx = sa_idx.union(inexact_recursion(s, i-1, diff-mismatch, temp_k, temp_l, 3))

    return sa_idx


def estimate_substitution_mat(ref, r):
    """get likelihood of each substitution type over all possible alignments"""
    mismatches = {}

    for i in range(0, len(ref)):
        for j in range(0, len(r)):
            if ref[i] != r[j]:
                if (ref[i], r[j]) in mismatches:
                    mismatches[(ref[i], r[j])] += 1
                else:
                    mismatches[(ref[i], r[j])] = 1

    scale = max(mismatches.values())
    for k in mismatches:
        mismatches[k] = float(mismatches[k])/scale

    return mismatches

def rank(bw):
    """rank(char) := list of number of occurrences of a char for each substring R[:i] (reference)"""
    totals = {}
    ranks = {}

    for char in ALPHABET:
        if (char not in totals) and (char != MAPPING_DOLLAR): # '$':
            totals[char] = 0
            ranks[char] = []

    for char in bw:
        if char != MAPPING_DOLLAR: # '$':
            totals[char] += 1
        for t in totals.keys():
            ranks[t].append(totals[t])

    return ranks, totals

def inexact_search(bw, bwr, s, diff):
    """find suffix array intervals with up to diff differences"""

    global C, O, D, num_prunes
    # totals, ranks
    # O is a dictionary with keys $,A,C,G,T, and values are arrays of counts
    O, tot = rank(bw)

    # reverse ranks
    Oprime, junk = rank(bwr)
    #Oprime = None

    # C[a] := number of lexicographically smaller letters than a in bw/reference
    C = compute_C(tot)

    # D[i] := lower bound on number of differences in substring s[1:i]
    D = compute_D(s, C, Oprime, bw)

    # call the recursive search function and return a list of SA-range tuples
    sa_index_set = inexact_recursion(s, len(s)-1, diff, 0, len(bw)-1, 4)
    index_dict = {}

    for (i, j) in sa_index_set:
        # if index already exists, pick the higher diff value
        if i in index_dict:
            if index_dict[i] < j:
                index_dict[i] = j
                num_prunes += 1

        else:
            index_dict[i] = j

    # sort list by diff from highest to lowest
    return sorted(index_dict.items(), key=itemgetter(1), reverse=True) 


def best_match_position(bw, bwr, s, diff, sa):
    sa_index_list = inexact_search(bw, bwr, s, diff)
    if len(sa_index_list) != 0:
        best_index, score = sa_index_list[0]
        return sa[best_index]+1, score
    else:
        return -1, -1

def run_match_align_bwt(q, t):
    # q = Mquery
    # t = Mtarget
    # ???????????????????????????????????????????
    dna_string = np.concatenate((t, np.array([MAPPING_DOLLAR], dtype=np.uint8)), dtype=np.uint8) # t+'$'
    dna_string_r = np.concatenate((t[::-1], np.array([MAPPING_DOLLAR], dtype=np.uint8)), dtype=np.uint8) # reverse(t)+'$'
    query_string = q[:100]
    query_string_r = q[len(q)-100:]

    threshold = 8
    max_offset = 20

    suffix_array = skew_rec(dna_string, 6)
    bwt = np.zeros(len(dna_string))
    for v_rank in range(len(dna_string)):
        bwt[v_rank] = (dna_string[suffix_array[v_rank]-1])

    suffix_array_r = skew_rec(dna_string_r, 6)
    bwt_r = np.zeros(len(dna_string_r))
    for v_rank in range(len(dna_string_r)):
        bwt_r[v_rank] = (dna_string_r[suffix_array_r[v_rank]-1])

    # We have SA and BWT
    # print("NOW SEARCH!!!!!")
    (off_l, _) = best_match_position(bwt, bwt_r, query_string, threshold, suffix_array)
    (off_r, _) = best_match_position(bwt, bwt_r, query_string_r, threshold, suffix_array)

    realign_right = False
    off_l = None if off_l == -1 else off_l-1
    if off_r == -1:
        off_r = None
    else:
        possible_offset = len(t)-100-off_r
        if abs(possible_offset) < max_offset:
            off_r = possible_offset
        else:
            off_r = None
            realign_right = True

    return off_l, off_r, realign_right
    

def normalize_pos(pos, len):
    return min(max(pos, 0), len)


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

def run_aligner_pipeline(
    reference_file_path: str,
    reads_file_path: str,
    output_file_path: str,
    kmer_len: int,
    window_len: int,
):
    gc.disable()
    #tracemalloc.start()
    np.set_printoptions(threshold=sys.maxsize)
    print(f"Invoked CLI with the following args: {' '.join(sys.argv)}")
    
    expected_coords = {}
    with open('./data_big/reads20Mb.txt', mode ='r')as file:
        csvFile = csv.reader(file, delimiter='\t')
        expected_coords = {line[0]: (int(line[1]), int(line[2])) for line in csvFile}
    if kmer_len > MAX_KMER_SIZE:
        kmer_len = MAX_KMER_SIZE

    target_seq = None
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
                if target_seq is None:
                   target_seq = seq_arr
                else:
                   target_seq = np.concatenate((target_seq, seq_arr), axis=0, dtype=np.uint8)
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
    output_buf = []
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
                    if True: #query_id == 'read_0':
                        try:
                            max_diff = round(len(query_seq)*1.3)
                            min_index_query = get_minimizers(
                                query_seq,
                                kmer_len=kmer_len,
                                window_len=window_len,
                            )

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

                            relative_extension = kmer_len // 2 + 1

                            if abs(match_end_t - match_start_t) > max_diff + relative_extension:
                                # FAILED MAPPING!
                                #print(f"Failed sequence, reason: {match_start_t} - {match_end_t} ({abs(match_end_t - match_start_t)})")
                                output_buf.append(f"{query_id} status=FAIL\n")
                            else:
                                q_begin, q_end = 0, len(query_seq)
                                t_begin, t_end = match_start_t - match_start_q - relative_extension, match_end_t + (len(query_seq)-match_end_q) + relative_extension

                                q_begin, q_end = normalize_pos(q_begin, len(query_seq)), normalize_pos(q_end, len(query_seq))
                                t_begin, t_end = normalize_pos(t_begin, len(target_seq)), normalize_pos(t_end, len(target_seq))

                                realign_mode = 0
                                t_begin_pad, t_end_pad, should_realign_right = run_match_align_bwt(
                                    query_seq,
                                    target_seq[t_begin:t_end],
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
                                    t_begin_pad, t_end_pad = run_match_align_dp(
                                        target_seq[t_begin:t_end],
                                        query_seq,
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

                                if query_id in expected_coords:
                                   diff_start = expected_coords[query_id][0]-t_begin
                                   diff_end = expected_coords[query_id][1]-t_end
                                   #print(f"TOTAL DIFF: {max(abs(diff_start), abs(diff_end))}")
                                   status = "OK" if max(abs(diff_start), abs(diff_end)) < 20 else "BAD"
                                   qual = "AA" if abs(diff_start)+abs(diff_end) < 10 else ("AB" if abs(diff_start)+abs(diff_end) < 20 else ("C" if max(abs(diff_start), abs(diff_end)) < 20 else "D"))
                                   #output_buf.append
                                   output_file.write(f"{query_id} status={status} qual={qual} diff=<{diff_start}, {diff_end}>  | {t_begin} {t_end} | pad: {t_begin_pad}, {t_end_pad} | {'REALIGNED'+realign_mode if should_realign_right else ''} \n")
                                else:
                                    output_buf.append(f"{query_id} {t_begin} {t_end}\n")
                        except Exception as e:
                            # TODO?
                            print(e)
                            raise e
                if line[0] == '>':
                    # Process end
                    query_seq = ""
                    query_id = line[1:].strip()
                else:
                    query_seq += line.rstrip()
        if len(output_buf) > 0:
            output_file.writelines(output_buf)
        print(f"Wrote records to {output_file_path}")
        #os._exit(0) # Faster exit than normally

def run_alignment_cli():
    run_aligner_pipeline(
        reference_file_path=sys.argv[1],
        reads_file_path=sys.argv[2],
        output_file_path="output.txt",
        kmer_len=18,
        window_len=8,
    )

if __name__ == '__main__':
    run_alignment_cli()
