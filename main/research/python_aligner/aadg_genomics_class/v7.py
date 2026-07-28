from gc import collect as gc_collect, disable as gc_disable
from sys import maxsize, argv as sys_argv

from time import time_ns
from typing import Dict, Any, Set, Optional, Tuple
from itertools import chain
from numpy.lib.stride_tricks import sliding_window_view
from operator import itemgetter
from collections import defaultdict

from numpy import frompyfunc, vectorize, uint8, zeros, array, concatenate, add, argmin, arange, column_stack, unique, split, empty, ix_, take, sum, diff, result_type

# Used to create kmer binary masks
MAX_KMER_SIZE = 64

# Mapping of special character used by suffix tables and similar structures
MAPPING_DOLLAR = 0
# Mapping of nucleotides
MAPPING = dict(
    C=1,
    A=2,
    T=3,
    G=4,
)
# Allphabet
ALPHABET = set(MAPPING.values())
# Alphabet with included special character
ALPHABET_DOLLAR = set([*ALPHABET, MAPPING_DOLLAR])
# Vectorized numpy function to map nucleotides from string
MAPPING_FN = vectorize(MAPPING.get, otypes=[uint8])

# Remove every nth kmer from the reference index
REFERENCE_NTH_KMER_REMOVAL = 15 # was: 20
# Load reference in chunks of that size
REFERENCE_INDEX_CHUNK_SIZE = 1000000

# Length of k-mer used to generate (k,w)-minimizer indices
KMER_LEN = 16 # was: 16
# Length of window to generate (k,w)-minimizer indices
WINDOW_LEN = 8 # was: 5

# Match from k-mer index with be resized by kmer_len times this factor
FACT_KMER_TO_RELATIVE_EXTENSION_LEN = 0.5

# Miminal length of subsequent k-mers that form a valid match
MIN_LIS_EXTENSION_WINDOW_LEN = 3
# Max distance between starting k-mer of the match and the ending k-mer of match
# Used when filtering the extended seed
# Should be higher than FACT_TARGET_TO_QUERY_MAX_RELATIVE_LENGTH
FACT_LIS_MAX_QUERY_DISTANCE = 1.3

# Max difference between query and target to concude that it's a valid match
#
# if len(t_region) > len(q_region) * FACT_TARGET_TO_QUERY_MAX_RELATIVE_LENGTH:
#   invalid_match()
#
FACT_TARGET_TO_QUERY_MAX_RELATIVE_LENGTH = 1.05
# When using BWT aligner this is the maximum extra padding that we consider valid (as a fraction of len(query))
FACT_BWT_QUERY_MAX_OFFSET = 0.04
# When using BWT aligner this is the length of query part that we consider for fast matching (as a fraction of len(query))
FACT_BWT_QUERY_FRAGMENT_SIZE = 0.1
# When using BWT aligner this is the maximum number of error that we should encounter (to speed up searching)
# The threshold is calculated as follows:
#
#    fragment_len = len(query) * FACT_BWT_QUERY_FRAGMENT_SIZE
#    max_errors = fragment_len * FACT_BWT_FRAGMENT_REL_ERRORS_THRESHOLD
#
FACT_BWT_FRAGMENT_REL_ERRORS_THRESHOLD = 0.08

# When using DP aligner (more accurate than BWT, but also slower)
# we consider pairs (kmer_len, kmer_skip)
# kmer_len is the length of k-mer we consider
# kmer_skip means we skip every n-th kmer 
# Those values can be significantlly lower than global KMER_SIZE, because we run DP aligner only in specific situations
# If no match is found we use the next configuration untill we find anything
DP_K_STEP_SEQUENCE = [(15, 11), (10, 11), (8, 5)]
# Length of the query suffix we use for the DP aligner as a fraction of query length
FACT_DP_QUERY_SUFFIX_REL_LEN = 0.4
# For DP aligner we set maxiumum exit distance 
# This value is fraction of the length of the query
FACT_DP_QUERY_REL_MAX_E_DISTANCE = 0.1111

# Cost when gap is opened (DP aligner uses only COST_GAP_EXTEND for gaps)
COST_GAP_OPEN = 3
# Cost when gap is extended (DP aligner uses only COST_GAP_EXTEND for gaps)
COST_GAP_EXTEND = 1
# Cost of mismatch
COST_MISMATCH = 1
# Cost of match
COST_MATCH = 0

# Globals used to cache values and speed up calculations (no need to pass around many references in call frames)
# C[a] := number of lexicographically smaller letters than a in bw/reference
_global_bwt_c = {}
# O is a dictionary with keys $,A,C,G,T, and values are arrays of counts
_global_bwt_o = {}
# D[i] := lower bound on number of differences in substring s[1:i]
_global_bwt_d = []
# mask[k] := mask used to calculate hash for k-mer of length k
_global_masks: Optional[Dict[int, int]] = None

# DP algorithm adapted from Langmead's notebooks
def align_dp_trace(D, x, y):
    ''' Backtrace edit-distance matrix D for strings x and y '''
    i, j = len(x), len(y)
    while i > 0:
        diag, vert, horz = maxsize, maxsize, maxsize
        delt = None
        if i > 0 and j > 0:
            delt = COST_MATCH if x[i-1] == y[j-1] else COST_MISMATCH
            diag = D[i-1, j-1] + delt
        if i > 0:
            vert = D[i-1, j] + COST_GAP_EXTEND
        if j > 0:
            horz = D[i, j-1] + COST_GAP_EXTEND
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
    D = zeros((len(p)+1, len(t)+1), dtype=int)
    # Note: First row gets zeros.  First column initialized as usual.
    D[1:, 0] = range(1, len(p)+1)
    for i in range(1, len(p)+1):
        for j in range(1, len(t)+1):
            delt = COST_MISMATCH if p[i-1] != t[j-1] else COST_MATCH
            D[i, j] = min(D[i-1, j-1] + delt, D[i-1, j] + COST_GAP_EXTEND, D[i, j-1] + COST_GAP_EXTEND)
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
    for (k, step) in DP_K_STEP_SEQUENCE:
        suff_len = round(len(query) * FACT_DP_QUERY_SUFFIX_REL_LEN)

        target_suffix = target[suff_len-k:]
        query_suffix = query[(len(query)-suff_len):]

        edist = round(len(query) * FACT_DP_QUERY_REL_MAX_E_DISTANCE)
        kmers = defaultdict(list)

        mask = generate_mask(k)

        uadd = frompyfunc(lambda x, y: ((x << 2) | y) & mask, 2, 1)

        # This computes values for kmers
        kmers_target = uadd.accumulate(target_suffix, dtype=object).astype(int)
        # for i in range(0, len(target)-k+1, step):
        #         kmers[sum(target[i:i+k])].append(i)
        for i in range(0, len(kmers_target), step):
            kmers[kmers_target[i]].append(i)

        hits = []
        kmers_query = uadd.accumulate(query_suffix, dtype=object).astype(int)
        #for i in range(0, len(query)-k+1, step+1):
            #for j in kmers[sum(query[i:i+k])]:
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

def merge(x: array, SA12: array, SA3: array) -> array:
    "Merge the suffixes in sorted SA12 and SA3."
    ISA = zeros((len(x),), dtype='int')
    for i in range(len(SA12)):
        ISA[SA12[i]] = i
    SA = zeros((len(x),), dtype='int')
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


def safe_idx(x: array, i: int) -> int:
    "Hack to get zero if we index beyond the end."
    return 0 if i >= len(x) else x[i]

def symbcount(x: array, asize: int) -> array:
    "Count how often we see each character in the alphabet."
    counts = zeros((asize,), dtype="int")
    for c in x:
        counts[c] += 1
    return counts

def cumsum(counts: array) -> array:
    "Compute the cumulative sum from the character count."
    res = zeros((len(counts, )), dtype='int')
    acc = 0
    for i, k in enumerate(counts):
        res[i] = acc
        acc += k
    return res

def bucket_sort(x: array, asize: int,
                idx: array, offset: int = 0) -> array:
    "Sort indices in idx according to x[i + offset]."
    sort_symbs = array([safe_idx(x, i + offset) for i in idx])
    counts = symbcount(sort_symbs, asize)
    buckets = cumsum(counts)
    out = zeros((len(idx),), dtype='int')
    for i in idx:
        bucket = safe_idx(x, i + offset)
        out[buckets[bucket]] = i
        buckets[bucket] += 1
    return out

def radix3(x: array, asize: int, idx: array) -> array:
    "Sort indices in idx according to their first three letters in x."
    idx = bucket_sort(x, asize, idx, 2)
    idx = bucket_sort(x, asize, idx, 1)
    return bucket_sort(x, asize, idx)

def triplet(x: array, i: int) -> Tuple[int, int, int]:
    "Extract the triplet (x[i],x[i+1],x[i+2])."
    return safe_idx(x, i), safe_idx(x, i + 1), safe_idx(x, i + 2)

def collect_alphabet(x: array, idx: array) -> Tuple[array, int]:
    "Map the triplets starting at idx to a new alphabet."
    alpha = zeros((len(x),), dtype='int')
    value = 1
    last_trip = -1, -1, -1
    for i in idx:
        trip = triplet(x, i)
        if trip != last_trip:
            value += 1
            last_trip = trip
        alpha[i] = value
    return alpha, value - 1

def build_u(x: array, alpha: array) -> array:
    "Construct u string, using 1 as central sentinel."
    a = array([alpha[i] for i in range(1, len(x), 3)] +
                 [1] +
                 [alpha[i] for i in range(2, len(x), 3)])
    return a

def less(x: array, i: int, j: int, ISA: array) -> bool:
    "Check if x[i:] < x[j:] using the inverse suffix array for SA12."
    a: int = safe_idx(x, i)
    b: int = safe_idx(x, j)
    if a < b: return True
    if a > b: return False
    if i % 3 != 0 and j % 3 != 0: return ISA[i] < ISA[j]
    return less(x, i + 1, j + 1, ISA)

def skew_rec(x: array, asize: int) -> array:
    "skew/DC3 SA construction algorithm."

    SA12 = array([i for i in range(len(x)) if i % 3 != 0])

    SA12 = radix3(x, asize, SA12)
    new_alpha, new_asize = collect_alphabet(x, SA12)
    if new_asize < len(SA12):
        # Recursively sort SA12
        u = build_u(x, new_alpha)
        sa_u = skew_rec(u, new_asize + 2)
        m = len(sa_u) // 2
        SA12 = array([u_idx(i, m) for i in sa_u if i != m])

    if len(x) % 3 == 1:
        SA3 = array([len(x) - 1] + [i - 1 for i in SA12 if i % 3 == 1])
    else:
        SA3 = array([i - 1 for i in SA12 if i % 3 == 1])
    SA3 = bucket_sort(x, asize, SA3)
    return merge(x, SA12, SA3)

# def compute_C(totals):
#     """compute C, the number of lexicographically greater symbols in the ref"""
#     #C = {0: 0, 1: 0, 2: 0, 3: 0, MAPPING_DOLLAR: 0}
#     C = {v: 0 for v in ALPHABET_DOLLAR}
#     for k in ALPHABET:
#         for ref in ALPHABET:
#             if ref < k:
#                 C[k] += totals[ref]

#     return C


# def compute_D(s, C, Oprime, bw):
#     """compute estimated lower bounds of differences in substring s[0:i] for all  in [0,len(s)]"""
#     k = 1
#     l = len(bw)-2
#     z = 0
#     D = [0] * len(s)

#     for i in range(0, len(s)):
#         k = C[s[i]] + Oprime[s[i]][k-1] + 1
#         l = C[s[i]] + Oprime[s[i]][l]
#         if k > l:
#             k = 1
#             l = len(bw)-1
#             z += 1
#         D[i] = z

#     return D


# def get_D(i):
#     """enforce condition that if D[i] is set to -1, its value will be considered as 0"""
#     if i < 0:
#         return 0
#     else:
#         return _global_bwt_d[i]


# def get_O(char, index):
#     """see get_D()"""
#     if index < 0:
#         return 0
#     else:
#         return _global_bwt_o[char][index]


# def estimate_substitution_mat(ref, r):
#     """get likelihood of each substitution type over all possible alignments"""
#     mismatches = {}

#     for i in range(0, len(ref)):
#         for j in range(0, len(r)):
#             if ref[i] != r[j]:
#                 if (ref[i], r[j]) in mismatches:
#                     mismatches[(ref[i], r[j])] += 1
#                 else:
#                     mismatches[(ref[i], r[j])] = 1

#     scale = max(mismatches.values())
#     for k in mismatches:
#         mismatches[k] = float(mismatches[k])/scale

#     return mismatches

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


def inexact_recursion(s, i, diff, k, l, prev_type):
    """search bwt recursively and tolerate errors"""
    # prev_type is a parameter that marks event that happened in previous cell:
    #   insertion -> 1
    #   deletion -> 2
    #   other -> 0

    # pruning based on estimated mistakes
    if diff < (_global_bwt_d[i] if i >= 0 else 0):
        return set(), False

    # end of query condition
    temp = set()
    if i < 0:
        for j in range(k, l+1):
            temp.add((j, diff))
        return temp, len(temp) > 0

    # Insertion
    if prev_type == 1:
        sub_result, has_result = inexact_recursion(s, i-1, diff-COST_GAP_EXTEND, k, l, 1)
        if has_result:
            return sub_result, True
    else:
        sub_result, has_result = inexact_recursion(s, i-1, diff-COST_GAP_EXTEND-COST_GAP_OPEN, k, l, 1)
        if has_result:
            return sub_result, True

    for char in ALPHABET:
        temp_k = _global_bwt_c[char] + (_global_bwt_o[char][k-1]+1 if k-1 >= 0 else 1)
        temp_l = _global_bwt_c[char] + (_global_bwt_o[char][l] if l >= 0 else 0)
    
        if temp_k <= temp_l:
            # Deletion
            if prev_type == 2:
                sub_result, has_result = inexact_recursion(s, i, diff-COST_GAP_EXTEND, temp_k, temp_l, 2)
                if has_result:
                    return sub_result, True
            else:
                sub_result, has_result = inexact_recursion(s, i, diff-COST_GAP_EXTEND-COST_GAP_OPEN, temp_k, temp_l, 2)
                if has_result:
                    return sub_result, True
            if char == s[i]:
                # Match!
                sub_result, has_result = inexact_recursion(s, i-1, diff+COST_MATCH, temp_k, temp_l, 0)
                if has_result:
                    return sub_result, True
            else:
                # Mismatch
                sub_result, has_result = inexact_recursion(s, i-1, diff-COST_MISMATCH, temp_k, temp_l, 0)
                if has_result:
                    return sub_result, True
    return set(), False
    
        
def run_bwt(bw, bwr, s, diff, sa):
    """find suffix array intervals with up to diff differences"""

    global _global_bwt_c, _global_bwt_o, _global_bwt_d

    # reverse ranks
    Oprime, _ = rank(bwr)

    _global_bwt_o, tot = rank(bw)

    # compute C, the number of lexicographically greater symbols in the ref
    _global_bwt_c = [0]*(max(ALPHABET)+1)
    for k in ALPHABET:
        for ref in ALPHABET:
            if ref < k:
                _global_bwt_c[k] += tot[ref]

    # compute estimated lower bounds of differences in substring s[0:i] for all  in [0,len(s)]
    k, z = 1, 0
    l = len(bw)-2
    _global_bwt_d = [0] * len(s)
    for i in range(0, len(s)):
        k = _global_bwt_c[s[i]] + Oprime[s[i]][k-1] + 1
        l = _global_bwt_c[s[i]] + Oprime[s[i]][l]
        if k > l:
            k = 1
            l = len(bw)-1
            z += 1
        _global_bwt_d[i] = z

    # call the recursive search function and return a list of SA-range tuples
    sa_index_set, _ = inexact_recursion(s, len(s)-1, diff, 0, len(bw)-1, 0)
    index_dict = {}

    for (i, j) in sa_index_set:
        # if index already exists, pick the higher diff value
        if i in index_dict:
            if index_dict[i] < j:
                index_dict[i] = j
        else:
            index_dict[i] = j

    # sort list by diff from highest to lowest
    sa_index_list = sorted(index_dict.items(), key=itemgetter(1), reverse=True)
    if len(sa_index_list) != 0:
        best_index, score = sa_index_list[0]
        return sa[best_index]+1, score
    else:
        return -1, -1

# [Ma, Ma, Mb, Mb]
# avg [7.216, 7.127, 5.668, 5.659] = 6.41749 ms
def run_match_align_bwt(q, t):
    fragment_size = round(len(q) * FACT_BWT_QUERY_FRAGMENT_SIZE)
    max_offset = round(len(q) * FACT_BWT_QUERY_MAX_OFFSET)
    fragment_offset = 0
    threshold = round(fragment_size * FACT_BWT_FRAGMENT_REL_ERRORS_THRESHOLD)
    
    off_r, off_l, realign_right = None, None, False
    for (
        is_right_offset,
        dna_fragment_start,
        dna_fragment_end,
        query_fragment_start,
        query_fragment_end,
        offset_return_pad,
        max_offset,
        thresholds,
    ) in [
        (False, 0, fragment_size*2, fragment_offset, fragment_size+fragment_offset, -1-fragment_offset, max_offset, [threshold]),
        (True, len(t)-fragment_size*2-fragment_offset, len(t)-fragment_offset, len(q)-fragment_size, len(q), -fragment_offset+1, max_offset, [threshold]),
    ]:
        if is_right_offset and off_r is not None:
            continue
        if not is_right_offset and (realign_right or off_l is not None):
            continue

        t_fragment = t[dna_fragment_start:dna_fragment_end]
        dna_fragment = concatenate((t_fragment, array([MAPPING_DOLLAR], dtype=uint8)), dtype=uint8) # t+'$'
        dna_fragment_rev = concatenate((t_fragment[::-1], array([MAPPING_DOLLAR], dtype=uint8)), dtype=uint8) # reverse(t)+'$'
        
        suffix_array = skew_rec(dna_fragment, 6)
        bwt = zeros(len(dna_fragment))
        for v_rank in range(len(dna_fragment)):
            bwt[v_rank] = (dna_fragment[suffix_array[v_rank]-1])
    
        suffix_array_rev = skew_rec(dna_fragment_rev, 6)
        bwt_rev = zeros(len(dna_fragment_rev))
        for v_rank in range(len(dna_fragment_rev)):
            bwt_rev[v_rank] = (dna_fragment_rev[suffix_array_rev[v_rank]-1])

        query_string = q[query_fragment_start:query_fragment_end]
        
        for threshold_i in range(len(thresholds)):
            threshold = thresholds[threshold_i]
            off, _ = run_bwt(bwt, bwt_rev, query_string, threshold, suffix_array)
            overflow_offset = False
            
            if off == -1:
                off = None
            elif off is not None:
                off += offset_return_pad
                if is_right_offset:
                    off = (dna_fragment_end-dna_fragment_start)-(query_fragment_end-query_fragment_start)-off
                if off > max_offset:
                    off = None
                    overflow_offset = True
            if is_right_offset:
                off_r = off
                realign_right = realign_right or overflow_offset
            else:
                off_l = off
    if off_l is None and off_r is not None and not realign_right:
        off_l = len(t) - off_r - len(q)
    return off_l, off_r, realign_right


def normalize_pos(pos, len):
    return min(max(pos, 0), len)

def generate_mask(
    kmer_len: int,
) -> int:
    global _global_masks
    if not _global_masks:
        _global_masks = dict()
        ret = 3
        for i in range(MAX_KMER_SIZE+1):
            ret = (ret << 2) | 3
            _global_masks[i] = ret
    return _global_masks[kmer_len]


def get_minimizers(
    seq_arr,
):
    sequence_len = len(seq_arr)
    mask = generate_mask(KMER_LEN)

    # Function to compute kmer value based on the previous (on the left side) kmer value and new nucleotide
    uadd = frompyfunc(lambda x, y: ((x << 2) | y) & mask, 2, 1)

    # This computes values for kmers
    kmers = uadd.accumulate(seq_arr, dtype=object).astype(int)
    kmers[:KMER_LEN-2] = 0
    del seq_arr
    
    # Do sliding window and get min kmers positions
    kmers_min_pos = add(argmin(sliding_window_view(kmers, window_shape=WINDOW_LEN), axis=1), arange(0, sequence_len - WINDOW_LEN + 1))
    
    # Now collect all selected mimumum and kmers into single table
    selected_kmers = column_stack((
        kmers[kmers_min_pos],
        kmers_min_pos,
        #np.ones(len(kmers_min_pos), dtype=bool)
    ))[KMER_LEN:]
    del kmers_min_pos
    del kmers

    # Remove duplicates
    selected_kmers = selected_kmers[selected_kmers[:, 0].argsort()]
    selected_kmers = unique(selected_kmers, axis=0)

    # This part performs group by using the kmer value
    selected_kmers_unique_idx = unique(selected_kmers[:, 0], return_index=True)[1][1:]
    selected_kmers_entries_split = split(selected_kmers[:, 1], selected_kmers_unique_idx)

    if len(selected_kmers) > 0:
        # We zip all kmers into a dict
        result = dict(zip(chain([selected_kmers[0, 0]], selected_kmers[selected_kmers_unique_idx, 0]), selected_kmers_entries_split))
    else:
        # If we have no minimizers we return nothing, sorry
        result = dict()
    return result


def cartesian_product(*arrays):
    la = len(arrays)
    dtype = result_type(*arrays)
    arr = empty([len(a) for a in arrays] + [la], dtype=dtype)
    for i, a in enumerate(ix_(*arrays)):
        arr[...,i] = a
    return arr.reshape(-1, la)

def run_aligner_pipeline(
    reference_file_path: str,
    reads_file_path: str,
    output_file_path: str,
):
    gc_disable()
    moment_start = time_ns()
    moments_l = []
    print(f"Invoked CLI with the following args: {' '.join(sys_argv)}")

    target_seq = None
    ref_loaded = False
    all_seq = ""
    all_seq_len = 0
    index_offset = 0

    ref_index = dict()
    with open(reference_file_path) as ref_fh:
        for line in chain(ref_fh, [">"]):
            if line[0] != '>':
                fasta_line = line.rstrip()
                all_seq += fasta_line
                all_seq_len  += len(fasta_line)
            if (all_seq_len >= REFERENCE_INDEX_CHUNK_SIZE or line[0] == '>') and all_seq_len > 0:
                # Transform loaded string chunk into a sequence
                seq_arr = MAPPING_FN(array(list(all_seq)))
                if target_seq is None:
                   target_seq = seq_arr
                else:
                   target_seq = concatenate((target_seq, seq_arr), axis=0, dtype=uint8)
                del all_seq

                # Target index building
                sequence_len = len(seq_arr)
                mask = generate_mask(KMER_LEN)

                # Function to compute kmer value based on the previous (on the left side) kmer value and new nucleotide
                uadd = frompyfunc(lambda x, y: ((x << 2) | y) & mask, 2, 1)

                # This computes values for kmers
                kmers = uadd.accumulate(seq_arr, dtype=object).astype(int)
                kmers[:KMER_LEN-2] = 0
                del seq_arr
                
                # Do sliding window and get min kmers positions
                kmers_min_pos = add(argmin(sliding_window_view(kmers, window_shape=WINDOW_LEN), axis=1), arange(0, sequence_len - WINDOW_LEN + 1))
                
                # Now collect all selected mimumum and kmers into single table
                selected_kmers = column_stack((
                    kmers[kmers_min_pos],
                    kmers_min_pos,
                    #np.ones(len(kmers_min_pos), dtype=bool)
                ))[KMER_LEN:]
                del kmers_min_pos
                del kmers
                gc_collect()

                # Remove duplicates
                selected_kmers = selected_kmers[selected_kmers[:, 0].argsort()]
                selected_kmers = unique(selected_kmers, axis=0)

                # Shift all indices according to what we loaded already
                selected_kmers[:,1] += index_offset

                # This part performs group by using the kmer value
                selected_kmers_unique_idx = unique(selected_kmers[:, 0], return_index=True)[1][1:]
                selected_kmers_entries_split = split(selected_kmers[:, 1], selected_kmers_unique_idx)

                if len(selected_kmers) > 0:
                    # We zip all kmers into a dict
                    i = 0
                    for k, v in zip(chain([selected_kmers[0, 0]], selected_kmers[selected_kmers_unique_idx, 0]), selected_kmers_entries_split):
                        i += 1
                        # Remove every n-th kmer from index
                        if i >= REFERENCE_NTH_KMER_REMOVAL and len(v) == 1:
                            i = 0
                            continue
                        # This part merges index for a chunk into index for the entire reference sequence
                        # this way (by loading in chunks) we spare some memory
                        if k in ref_index:
                            ref_index[k] = concatenate((ref_index[k], v), axis=0)
                        else:
                            ref_index[k] = v
                else:
                    # If we have no minimizers we return nothing, sorry
                    pass

                index_offset += all_seq_len
                all_seq_len = 0
                all_seq = ""

                del selected_kmers_unique_idx
                del selected_kmers_entries_split
                gc_collect()
            if line[0] == '>':
                if ref_loaded:
                    break
                ref_loaded = True
                continue

    output_buf = []
    with open(output_file_path, 'w') as output_file:
        query_id = ""
        query_seq = ""
        with open(reads_file_path) as reads_fh:
            for line in chain(reads_fh, [">"]):
                if line[0] == '>' and len(query_seq) > 0:
                    query_seq = MAPPING_FN(array(list(query_seq)))
                    # Process
                    
                    try:
                        max_diff = round(len(query_seq)*FACT_LIS_MAX_QUERY_DISTANCE)
                        min_index_query = get_minimizers(
                            query_seq,
                        )

                        common_kmers = []
                        for key in min_index_query:
                            if key in ref_index:
                                common_kmers.append(key)

                        matches = array([[-1, -1]])
                        for kmer in common_kmers:
                            kmer_entries_target, kmer_entries_query = ref_index[kmer], min_index_query[kmer]
                            matches = concatenate((
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

                        if n == 0:
                            pass
                        elif n == 1:
                            match_score, match_start_t, match_end_t, match_start_q, match_end_q = 0, matches[0, 0], matches[0, 0], matches[0, 1], matches[0, 1]
                        else:
                            longest_seq_len = 0
                            parent = [maxsize]*(n+1)
                            increasingSub = [maxsize]*(n+1)
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

                            lis = take(matches, q, axis=0)
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
                                lis_ext_window_len = end - i
                                if lis_ext_window_len > MIN_LIS_EXTENSION_WINDOW_LEN:
                                    estimated_matches_q = (lis[start, 1] if start < longest_seq_len else max_diff) - lis[i, 1]
                                    estimated_matches_t = (lis[start, 0] if start < longest_seq_len else lis[start-1, 0]) - lis[i, 0]
                                    score = min(estimated_matches_q, estimated_matches_t)*min(estimated_matches_q, estimated_matches_t) - sum(diff(lis[i:start, 0], axis=0))
                                    
                                    if score > match_score:
                                        match_end_index_pos = max(i, min(start-1, longest_seq_len-1))
                                        match_score, match_start_t, match_end_t, match_start_q, match_end_q = score, lis[i, 0], lis[match_end_index_pos, 0], lis[i, 1], lis[match_end_index_pos, 1]
                                    if start == longest_seq_len:
                                        break

                        relative_extension = round(KMER_LEN * FACT_KMER_TO_RELATIVE_EXTENSION_LEN) + 1

                        can_submit = False
                        if abs(match_end_t - match_start_t) > max_diff + relative_extension:
                            pass
                        else:
                            q_begin, q_end = 0, len(query_seq)
                            t_begin, t_end = match_start_t - match_start_q - relative_extension, match_end_t + (len(query_seq)-match_end_q) + relative_extension

                            q_begin, q_end = normalize_pos(q_begin, len(query_seq)), normalize_pos(q_end, len(query_seq))
                            t_begin, t_end = normalize_pos(t_begin, len(target_seq)), normalize_pos(t_end, len(target_seq))

                            for i in range(2):
                                realign_mode = 0
                                _bwt_start = time_ns()
                                t_begin_pad, t_end_pad, should_realign_right = run_match_align_bwt(
                                    query_seq,
                                    target_seq[t_begin:t_end],
                                )
                                moments_l.append((time_ns()-_bwt_start) // 1000000)
                                output_buf.append(f"{query_id} BWT {t_begin_pad} {t_end_pad}\n")
                                    
                                if should_realign_right:
                                    realign_mode = 1
                                if abs(t_end-(t_end_pad or 0)-t_begin-(t_begin_pad or 0)) > len(query_seq)*FACT_TARGET_TO_QUERY_MAX_RELATIVE_LENGTH:
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

                                if not should_realign_right and t_begin_pad is None:
                                    t_begin_pad = relative_extension
                                if not should_realign_right and t_end_pad is None:
                                    t_end_pad = relative_extension

                                if t_begin_pad is not None:
                                    t_begin += t_begin_pad
                                if t_end_pad is not None:
                                    t_end -= t_end_pad

                                if abs(t_end-t_begin) > len(query_seq)*FACT_TARGET_TO_QUERY_MAX_RELATIVE_LENGTH:
                                    # Problem
                                    t_begin = t_end - len(query_seq) - relative_extension
                                    continue
                                else:
                                    can_submit = True
                                    break

                            if not can_submit:
                                # Cannot submit! Problem with matching
                                pass
                            else:
                                output_buf.append(f"{query_id}\t{t_begin}\t{t_end}\n")
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
        
        moment_end = time_ns()
        print(f"Wrote records to {output_file_path} in {((moment_end-moment_start) // 10000000)/100} sec.")
        print(f"BWT AVERAGE RUNNING TIME = {sum(moments_l)/len(moments_l)} ms")
        #os._exit(0) # Faster exit than normally

def run_alignment_cli():
    run_aligner_pipeline(
        reference_file_path=sys_argv[1],
        reads_file_path=sys_argv[2],
        output_file_path="output.txt",
    )

if __name__ == '__main__':
    run_alignment_cli()
