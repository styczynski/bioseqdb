from gc import collect as gc_collect, disable as gc_disable
from sys import maxsize, argv as sys_argv

from time import time_ns, sleep
from typing import Dict, Any, Set, Optional, Tuple
from itertools import chain
from numpy.lib.stride_tricks import sliding_window_view
from operator import itemgetter
from collections import defaultdict

from numpy import frompyfunc, vectorize, uint8, uint32, array, concatenate, add, argmin, arange, column_stack, unique, split, empty, ix_, take

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

# Pairs of alphabet symbols (a, b) such that a > b
# Useful for O(a^2) iteration over alphabet
ALPHABET_LT_PAIRS = [(a, b) for a in ALPHABET for b in ALPHABET if b < a]

# Vectorized numpy function to map nucleotides from string
MAPPING_FN = vectorize(MAPPING.get, otypes=[uint8])

# Remove every nth kmer from the reference index
REFERENCE_NTH_KMER_REMOVAL = 15 # was: 20
# Load reference in chunks of that size
REFERENCE_INDEX_CHUNK_SIZE = 800000

# Length of k-mer used to generate (k,w)-minimizer indices
KMER_LEN = 16 # was: 16
# Length of window to generate (k,w)-minimizer indices
WINDOW_LEN = 7 # was: 5

# Match from k-mer index with be resized by kmer_len times this factor
FACT_KMER_TO_RELATIVE_EXTENSION_LEN = 0.5

# Miminal length of subsequent k-mers in LIS (longest increasing subsequence) that form a valid match
MIN_LIS_EXTENSION_WINDOW_LEN = 3
# Mimimum score for the extension window that is considered a match
MIN_LIS_EXTENSION_WINDOW_SCORE = 0.1
# Number of locally maximum scores for extension windows that we consider
MAX_ACCEPTED_LIS_EXTENSION_WINDOWS_COUNT = 5
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
FACT_BWT_FRAGMENT_REL_ERRORS_THRESHOLD = 0.10

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
                target_fragment = target_suffix[lf:rt].tolist()
                query_fragment = query_suffix.tolist()
                target_fragment_len = len(target_fragment)
                query_fragment_len = len(query_fragment)
                # Find the alignment of p to a substring of t with the fewest edits
                D = [[0] * (target_fragment_len+1) for i in range(query_fragment_len+1)]
                # Note: First row gets zeros.  First column initialized as usual.
                for i in range(1, len(D)):
                    D[i][0] = i
                for i in range(1, query_fragment_len+1):
                    for j in range(1, target_fragment_len+1):
                        delt = COST_MISMATCH if query_fragment[i-1] != target_fragment[j-1] else COST_MATCH
                        D[i][j] = min(D[i-1][j-1] + delt, D[i-1][j] + COST_GAP_EXTEND, D[i][j-1] + COST_GAP_EXTEND)
                # Find minimum edit distance in last row
                eoff, mn = min(enumerate(D[query_fragment_len]), key=itemgetter(1), default=(None, query_fragment_len + target_fragment_len))
                # eoff, mn = None, query_fragment_len + target_fragment_len
                # for j in range(target_fragment_len+1):
                #     if D[query_fragment_len][j] < mn:
                #         eoff, mn = j, D[query_fragment_len][j]
                if mn <= edist:
                    # Backtrace; note: stops as soon as it gets to first row
                    # DP algorithm adapted from Langmead's notebooks
                    # Backtrace edit-distance matrix D for strings x and y
                    y = target_fragment[:eoff]
                    i, soff = query_fragment_len, len(y)
                    while i > 0:
                        diag, vert, horz = maxsize, maxsize, maxsize
                        delt = None
                        if i > 0 and soff > 0:
                            delt = COST_MATCH if query_fragment[i-1] == y[soff-1] else COST_MISMATCH
                            diag = D[i-1][soff-1] + delt
                        if i > 0:
                            vert = D[i-1][soff] + COST_GAP_EXTEND
                        if soff > 0:
                            horz = D[i][soff-1] + COST_GAP_EXTEND
                        if diag <= vert and diag <= horz:
                            # diagonal was best
                            i -= 1; soff -= 1
                        elif vert <= horz:
                            # vertical was best; this is an insertion in x w/r/t y
                            i -= 1
                        else:
                            # horizontal was best
                            soff -= 1
                    # Return edit distance and t coordinates of aligned substring
                    soff += lf
                    eoff += lf
                    hits.append((mn, soff, eoff))
        hits.sort()
        if hits:
            return hits[0][0], hits[0][1], len(target_suffix)-hits[0][2]
        if align_mode == 1:
            return maxsize, 0, 0
    return maxsize, 0, 0

def rank(bw):
    """rank(char) := list of number of occurrences of a char for each substring R[:i] (reference)"""
    totals = [0]*(max(ALPHABET)+1)
    ranks = [[] for i in range(max(ALPHABET)+1)]
    for char in bw:
        if char != MAPPING_DOLLAR: # '$':
            totals[char] += 1
        for t in ALPHABET:
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
    if i < 0:
        result = {(j, diff) for j in range(k, l+1)}
        return result, len(result) > 0

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
    for a, b in ALPHABET_LT_PAIRS:
        _global_bwt_c[a] += tot[b]

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
    # get best result
    best_index, score = max(sa_index_set, key=itemgetter(1), default=(-1, -1))
    if score < 0:
        return -1, -1
    # map result to correct position
    return sa[best_index]+1, score

# [Ma, Ma, Mb, Mb]
# avg [1.877, 1.908, 0.635, 0.568] = 1.2469 ms
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

        dna_fragment = t_fragment.tolist()
        dna_fragment_rev = t_fragment[::-1].tolist()

        dna_fragment.append(MAPPING_DOLLAR)
        dna_fragment_rev.append(MAPPING_DOLLAR)

        #dna_fragment = concatenate((t_fragment, array([MAPPING_DOLLAR], dtype=uint8)), dtype=uint8) # t+'$'
        #dna_fragment_rev = concatenate((t_fragment[::-1], array([MAPPING_DOLLAR], dtype=uint8)), dtype=uint8) # reverse(t)+'$'
        
        suffix_array = [t[1] for t in sorted((dna_fragment[i:],i) for i in range(len(dna_fragment)))] #skew_rec(dna_fragment, 6)
        bwt = [0]*len(dna_fragment) #zeros(len(dna_fragment), dtype=uint8)
        for v_rank in range(len(dna_fragment)):
            bwt[v_rank] = (dna_fragment[suffix_array[v_rank]-1])
    
        suffix_array_rev = [t[1] for t in sorted((dna_fragment_rev[i:],i) for i in range(len(dna_fragment_rev)))] # skew_rec(dna_fragment_rev, 6)
        bwt_rev = [0]*len(dna_fragment_rev) #zeros(len(dna_fragment_rev), dtype=uint8)
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
    ))[KMER_LEN:].astype(uint32)
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


def pos_cartesian_product(*arrays):
    la = len(arrays)
    arr = empty([len(a) for a in arrays] + [la], dtype=uint32)
    for i, a in enumerate(ix_(*arrays)):
        arr[...,i] = a
    return arr.reshape(-1, la)

if __debug__:
    _global_tracking_data = defaultdict(lambda: dict(moments=[], last_start=None, metric_type='time'))
    
    def track_block(name, end=False):
        global _global_tracking_data
        cur_time = time_ns()
        _global_tracking_data[name]['metric_type'] = 'time'
        if not end:
            _global_tracking_data[name]['last_start'] = cur_time
        elif _global_tracking_data[name]['last_start'] is not None:
            _global_tracking_data[name]['moments'].append(cur_time - _global_tracking_data[name]['last_start'])
            _global_tracking_data[name]['last_start'] = None

    def track_counter(name, diff=1):
        global _global_tracking_data
        _global_tracking_data[name]['metric_type'] = 'counter'
        _global_tracking_data[name]['moments'].append(diff)

    def print_track_info():
        global _global_tracking_data
        import json
        for key in _global_tracking_data.keys():
            samples_data = _global_tracking_data[key]
            metric_type = samples_data['metric_type']
            t_samples_count = len(samples_data['moments'])
            t_total = sum(samples_data['moments'])
            if metric_type == 'time':
                json_dump = json.dumps(dict(
                    name=key,
                    t_samples_count=t_samples_count,
                    t_min=(min(samples_data['moments'], default=0) // 10000) / 100,
                    t_max=(max(samples_data['moments'], default=0) // 10000) / 100,
                    t_avg=((t_total/t_samples_count if t_samples_count > 0 else 0) // 10000) / 100,
                    t_total=(t_total // 10000) / 100,
                ))
            elif metric_type == 'counter':
                json_dump = json.dumps(dict(
                    name=key,
                    t_samples_count=t_samples_count,
                    t_min=(min(samples_data['moments'], default=0)),
                    t_max=(max(samples_data['moments'], default=0)),
                    t_avg=((t_total/t_samples_count if t_samples_count > 0 else 0)),
                    t_total=t_total,
                ))
            print(f"TRACK {json_dump}")

def run_aligner_pipeline(
    reference_file_path: str,
    reads_file_path: str,
    output_file_path: str,
):
    gc_disable()
    if __debug__:
        print(f"Debug mode active. Invoked CLI with the following args: {' '.join(sys_argv)}")
        print("If this message is visible then __debug__ was set to True")
        print("Execution will be slower and performance data will be collected")
        print("Please use PYTHONOPTIMIZE=1 or python3 -O flag to disable debug mode")
        track_block('total_execution')

    moment_start = time_ns()

    target_seq_l = []
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
                if __debug__:
                    track_block('ref_chunk_building')
                # Transform loaded string chunk into a sequence
                seq_arr = MAPPING_FN(array(list(all_seq)))
                target_seq_l.append(seq_arr)
                if len(target_seq_l) > 5:
                    target_seq_l = [concatenate(target_seq_l, axis=0, dtype=uint8)]
                    gc_collect()
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
                ))[KMER_LEN:].astype(uint32)
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
                            ref_index[k] = concatenate((ref_index[k], v), axis=0, dtype=uint32)
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
                del selected_kmers
                gc_collect()
                if __debug__:
                    track_block('ref_chunk_building', end=True)
            if line[0] == '>':
                if ref_loaded:
                    break
                ref_loaded = True
                continue

    # tl = 0
    # for k in ref_index:
    #     tl += len(ref_index[k])
    # print(f"REF_INDEX_TL = {tl}")
    # import sys
    # sys.exit(0)
    if len(target_seq_l) == 1:
        target_seq = target_seq_l[0]
    else:
        target_seq = concatenate(target_seq_l, axis=0, dtype=uint8)
    del target_seq_l
    for i in range(3):
        gc_collect()

    output_buf = []
    with open(output_file_path, 'w') as output_file:
        query_id = ""
        query_seq = ""
        with open(reads_file_path) as reads_fh:
            for line in chain(reads_fh, [">"]):
                if line[0] == '>' and len(query_seq) > 0:
                    query_seq = MAPPING_FN(array(list(query_seq)))
                    # Process
                    
                    if True: #query_id == 'read_444':
                        try:
                            if __debug__:
                                track_block('read_index_building')
                            query_len = len(query_seq)
                            max_diff = round(query_len*FACT_LIS_MAX_QUERY_DISTANCE)
                            min_index_query = get_minimizers(
                                query_seq,
                            )
                            gc_collect()
                            if __debug__:
                                track_block('read_index_building', end=True)
                                track_block('read_lis')

                            common_kmers = []
                            for key in min_index_query:
                                if key in ref_index:
                                    common_kmers.append(key)
                                    

                            #matches = sorted([(pt, pq) for kmer in common_kmers for pt in ref_index[kmer] for pq in min_index_query[kmer]]
                            matches = array([[-1, -1]])
                            for kmer in common_kmers:
                                kmer_entries_target, kmer_entries_query = ref_index[kmer], min_index_query[kmer]
                                matches = concatenate((
                                    matches,
                                    pos_cartesian_product(
                                        kmer_entries_target,
                                        kmer_entries_query,
                                    )),
                                    axis=0,
                                )
                            del common_kmers
                            matches = matches[matches[:, 0].argsort()]
                            matches = matches[1:].tolist()
                            n = len(matches)
                            if __debug__:
                                track_counter('extend_matches_count', n)
                            del min_index_query
                            gc_collect()
                            #print("ALL_MATCHES")
                            #print(matches)
                            # for i in range(len(matches)):
                            #     print(matches[i])
                            
                            relative_extension = round(KMER_LEN * FACT_KMER_TO_RELATIVE_EXTENSION_LEN) + 1
                            lis_accepted = False
                            match_start_t, match_end_t, match_start_q, match_end_q = 0, 0, 0, 0

                            if n < MIN_LIS_EXTENSION_WINDOW_LEN:
                                if __debug__:
                                    track_block('read_lis', end=True)
                                pass
                            else:
                                longest_seq_len = 0
                                parent = [maxsize]*(n+1)
                                increasingSub = [maxsize]*(n+1)
                                for i in range(n):
                                    start = 1
                                    end = longest_seq_len
                                    while start <= end:
                                        middle = (start + end) // 2
                                        if matches[increasingSub[middle]][1] >= matches[i][1] or matches[increasingSub[start]][0] + max_diff < matches[i][0]:
                                            end = middle - 1
                                        else:
                                            start = middle + 1    
                                    parent[i] = increasingSub[start-1]
                                    increasingSub[start] = i
                                    if start > longest_seq_len:
                                        longest_seq_len = start

                                current_node = increasingSub[longest_seq_len]
                                q = [current_node]*longest_seq_len 
                                for j in range(longest_seq_len-1, 0, -1):
                                    current_node = parent[current_node]
                                    q[j-1] = current_node

                                if __debug__:
                                    track_block('read_lis', end=True)
                                    track_block('read_lis_cutoff')

                                lis = take(matches, q, axis=0)
                                if __debug__:
                                    track_counter('lis_length', len(lis))
                                # lis_len = len(lis)
                                # if lis_len >= MIN_LIS_EXTENSION_WINDOW_LEN:
                                #     match_start_t, match_end_t, match_start_q, match_end_q = lis[0, 0], lis[lis_len-1, 0], lis[0, 1], lis[lis_len-1, 1]
                                #     if abs(match_end_t - match_start_t) < max_diff + relative_extension:
                                #         lis_accepted = True
                                # if not lis_accepted:
                                #     # Backup lis!
                                #     match_score = -max_diff
                                #     longest_seq_len = 0
                                #     parent = [maxsize]*(n+1)
                                #     increasingSub = [maxsize]*(n+1)
                                #     for i in range(n):
                                #         start = 1
                                #         end = longest_seq_len
                                #         while start <= end:
                                #             middle = (start + end) // 2
                                #             if matches[increasingSub[middle]][1] < matches[i][1]:
                                #                 start = middle + 1
                                #             else:
                                #                 end = middle - 1
                                #         parent[i] = increasingSub[start-1]
                                #         increasingSub[start] = i

                                #         if start > longest_seq_len:
                                #             longest_seq_len = start

                                #     current_node = increasingSub[longest_seq_len]
                                #     q = [current_node]*longest_seq_len 
                                #     for j in range(longest_seq_len-1, 0, -1):
                                #         current_node = parent[current_node]
                                #         q[j-1] = current_node

                                #lis = take(matches, q, axis=0)
                                scores = []
                                score_1, score_2, score_3 = -max_diff, -max_diff, -max_diff
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
                                    # Window is i till end
                                    lis_ext_window_len = end - i
                                    if lis_ext_window_len > MIN_LIS_EXTENSION_WINDOW_LEN:
                                        window_src = lis[i:start, :].tolist()
                                        window = [window_src[0]]
                                        diff_sum = 0
                                        for i in range(1, len(window_src)):
                                            t1, q1 = window[len(window)-1]
                                            t2, q2 = window_src[i]
                                            if t2-t1 < KMER_LEN and q2-q1 < KMER_LEN:
                                                continue
                                            diff_sum += t2-t1
                                            window.append([t2, q2])
                                        #print(f"window {len(window_src)} -> {len(window)}")
                                        #print(window)
                                        
                                        estimated_matches_q = window[len(window)-1][1] - window[0][1] #(lis[start, 1] if start < longest_seq_len else max_diff) - lis[i, 1]
                                        estimated_matches_t = window[len(window)-1][0] - window[0][0] #(lis[start, 0] if start < longest_seq_len else lis[start-1, 0]) - lis[i, 0]
                                        score = (min(estimated_matches_q, estimated_matches_t) - diff_sum/KMER_LEN)/query_len
                                        score_1 = score_2
                                        score_2 = score_3
                                        score_3 = score

                                        if score_2 > score_1 and score_2 > score_3:
                                            # Local maximum
                                            if score_2 > MIN_LIS_EXTENSION_WINDOW_SCORE:
                                                scores.append((score_2, window_src[0][0], window_src[len(window_src)-1][0], window_src[0][1], window_src[len(window_src)-1][1]))
                                        # if score > match_score:
                                        #     match_score, match_start_t, match_end_t, match_start_q, match_end_q = score, window[0][0], window[len(window)-1][0], window[0][1], window[len(window)-1][1]
                                        #     lis_accepted = True

                                            #print(f"score={match_score} start={match_start_t}")
                                        if start == longest_seq_len:
                                            break
                                
                                if score_3 > score_2 and score_3 > score_1:
                                    if score_3 > MIN_LIS_EXTENSION_WINDOW_SCORE:
                                        scores.append((score_3, window_src[0][0], window_src[len(window_src)-1][0], window_src[0][1], window_src[len(window_src)-1][1]))
                                    
                                scores = sorted(scores, reverse=True)[:MAX_ACCEPTED_LIS_EXTENSION_WINDOWS_COUNT]
                                # print(f"max_score={match_score} l={longest_seq_len}")
                                #print(f"{query_id} -> {scores}")
                                if __debug__:
                                    track_counter('lis_accepted_scores', len(scores))
                                if len(scores) > 1:
                                    # More matches to be handled!
                                    min_cdp_score = maxsize
                                    for (_, cmatch_start_t, cmatch_end_t, cmatch_start_q, cmatch_end_q) in scores:
                                        q_begin, q_end = 0, len(query_seq)
                                        t_begin, t_end = cmatch_start_t - cmatch_start_q - relative_extension, cmatch_end_t + (len(query_seq)-cmatch_end_q) + relative_extension
                                        q_begin, q_end = normalize_pos(q_begin, len(query_seq)), normalize_pos(q_end, len(query_seq))
                                        t_begin, t_end = normalize_pos(t_begin, len(target_seq)), normalize_pos(t_end, len(target_seq))
                                        cdp_score, _, ct_end_pad = run_match_align_dp(
                                            target_seq[t_begin:t_end],
                                            query_seq,
                                            align_mode=2,
                                        )
                                        if ct_end_pad is not None:
                                            t_end -= ct_end_pad
                                        if cdp_score < min_cdp_score:
                                            min_cdp_score = cdp_score
                                            match_start_t, match_end_t, match_start_q, match_end_q = t_begin, t_end, q_begin, q_end
                                    if min_cdp_score != maxsize:
                                        lis_accepted = True
                                else:
                                    _, match_start_t, match_end_t, match_start_q, match_end_q = scores[0]
                                    lis_accepted = True
                                
                                del scores
                                del lis
                                if __debug__:
                                    track_block('read_lis_cutoff', end=True)
                            
                            del matches
                            gc_collect()

                            can_submit = False
                            if not lis_accepted or abs(match_end_t - match_start_t) > max_diff + relative_extension:
                                #print(f"{query_id} -> UNMAPPED; diff_to_much?={abs(match_end_t - match_start_t)}>{max_diff + relative_extension}  and lis_accepted={lis_accepted}")
                                pass
                            else:
                                q_begin, q_end = 0, len(query_seq)
                                t_begin, t_end = match_start_t - match_start_q - relative_extension, match_end_t + (len(query_seq)-match_end_q) + relative_extension

                                q_begin, q_end = normalize_pos(q_begin, len(query_seq)), normalize_pos(q_end, len(query_seq))
                                t_begin, t_end = normalize_pos(t_begin, len(target_seq)), normalize_pos(t_end, len(target_seq))

                                for i in range(2):
                                    realign_mode = 0
                                    if __debug__:
                                        track_block('read_bwt')
                                    t_begin_pad, t_end_pad, should_realign_right = run_match_align_bwt(
                                        query_seq,
                                        target_seq[t_begin:t_end],
                                    )
                                    if __debug__:
                                        track_block('read_bwt', end=True)
                                        if t_begin_pad is not None:
                                            track_counter('read_bwt_pad_begin')
                                        if t_end_pad is not None:
                                            track_counter('read_bwt_pad_end')
                                    gc_collect()
                                        
                                    if should_realign_right:
                                        realign_mode = 1
                                    if abs(t_end-(t_end_pad or 0)-t_begin-(t_begin_pad or 0)) > len(query_seq)*FACT_TARGET_TO_QUERY_MAX_RELATIVE_LENGTH:
                                        realign_mode = 2
                                        if t_begin_pad is not None:
                                            t_begin += t_begin_pad
                                        if t_end_pad is not None:
                                            t_end -= t_end_pad

                                    if realign_mode > 0:
                                        if __debug__:
                                            track_block('read_dp')
                                        _, _, t_end_pad = run_match_align_dp(
                                            target_seq[t_begin:t_end],
                                            query_seq,
                                            align_mode=realign_mode,
                                        )
                                        if __debug__:
                                            track_block('read_dp', end=True)
                                        t_begin_pad = 0 # TODO: ?????
                                        gc_collect()
                                        # print(f"MATCH DP() run in {total_dp // 1000000} ms for query_id={query_id}")
                                        # print(f"result = {t_begin_pad} x {t_end_pad}")
                                        # import sys
                                        # sys.exit(1)

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
                                    if len(output_buf) > 25:
                                        output_file.writelines(output_buf)
                                        output_buf = []
                                        gc_collect()
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
        if __debug__:
            track_block('total_execution', end=True)
            print_track_info()
        #os._exit(0) # Faster exit than normally

def run_alignment_cli():
    run_aligner_pipeline(
        reference_file_path=sys_argv[1],
        reads_file_path=sys_argv[2],
        output_file_path="output.txt",
    )

if __name__ == '__main__':
    run_alignment_cli()
