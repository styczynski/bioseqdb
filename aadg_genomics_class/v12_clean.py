from gc import collect as gc_collect, disable as gc_disable
from sys import maxsize, argv as sys_argv

from time import time_ns
from itertools import chain
from numpy.lib.stride_tricks import sliding_window_view
from operator import itemgetter
from collections import defaultdict

from numpy import frompyfunc, vectorize, uint8, uint32, array, concatenate, add, argmin, arange, column_stack, unique, split, empty, ix_, take, diff
from bisect import bisect_right

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
KMER_LEN = 17 # was: 16
# Length of window to generate (k,w)-minimizer indices
WINDOW_LEN = 8 # was: 5

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
DP_K_STEP_SEQUENCE = [(15, 11), (8, 5)]
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
_global_masks = None

# Code adopted from the oriignal mapper (adapted from Langmead's notebooks)
# However what we return is tuple <score, offset_left, offset_right>
# align_mode can be:
# 1 -> (default) Interate over first parameters' tuple from DP_K_STEP_SEQUENCE and if there are no hits return nothing
# != 1 -> Interate over first parameter's tuple from DP_K_STEP_SEQUENCE and continue to the next tuple if there are no hits
def run_match_align_dp(target, query, align_mode=1):
    for (k, step) in DP_K_STEP_SEQUENCE:
        suff_len = round(len(query) * FACT_DP_QUERY_SUFFIX_REL_LEN)

        # Computing kmers quickly
        target_suffix = target[suff_len-k:]
        query_suffix = query[(len(query)-suff_len):]

        edist = round(len(query) * FACT_DP_QUERY_REL_MAX_E_DISTANCE)
        kmers = defaultdict(list)

        mask = generate_mask(k)
        uadd = frompyfunc(lambda x, y: ((x << 2) | y) & mask, 2, 1)

        # This computes values for kmers
        kmers_target = uadd.accumulate(target_suffix, dtype=object).astype(int)
        for i in range(0, len(kmers_target), step):
            kmers[kmers_target[i]].append(i)

        hits = []
        kmers_query = uadd.accumulate(query_suffix, dtype=object).astype(int)
        for i in range(0, len(kmers_query), step+1):
            for j in kmers[kmers_query[i]]:
                lf = max(0, j-i-edist)
                rt = min(len(target_suffix), j-i+len(query_suffix)+edist)
                # Take fragment of query and target, then covert them to lists
                # This will speed up lookup in the following lines
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
                            # Diagonal was best
                            i -= 1; soff -= 1
                        elif vert <= horz:
                            # Vertical was best; this is an insertion in x w/r/t y
                            i -= 1
                        else:
                            # Horizontal was best
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

# Inspired by general common and easy implementation of BWT
# rank(char) is list of number of occurrences of a char for each substring of target T[:i]
def rank(bw):
    totals = [0]*(max(ALPHABET)+1)
    ranks = [[] for i in range(max(ALPHABET)+1)]
    for char in bw:
        if char != MAPPING_DOLLAR:
            totals[char] += 1
        for t in ALPHABET:
            ranks[t].append(totals[t])
    return ranks, totals

# Recursive BWT alignment algorithm implementation
# prev_type is a parameter that marks event that happened in previous cell:
#   insertion -> 1
#   deletion -> 2
#   other -> 0
# This follows the common implementation of backtrack BWT aligner, nothing special.
def run_bwt_backtrack(s, i, diff, k, l, prev_type):

    # pruning based on estimated mistakes
    if diff < (_global_bwt_d[i] if i >= 0 else 0):
        return set(), False

    # end of query condition
    if i < 0:
        result = {(j, diff) for j in range(k, l+1)}
        return result, len(result) > 0

    # Insertion
    if prev_type == 1:
        sub_result, has_result = run_bwt_backtrack(s, i-1, diff-COST_GAP_EXTEND, k, l, 1)
        if has_result:
            return sub_result, True
    else:
        sub_result, has_result = run_bwt_backtrack(s, i-1, diff-COST_GAP_EXTEND-COST_GAP_OPEN, k, l, 1)
        if has_result:
            return sub_result, True

    for char in ALPHABET:
        temp_k = _global_bwt_c[char] + (_global_bwt_o[char][k-1]+1 if k-1 >= 0 else 1)
        temp_l = _global_bwt_c[char] + (_global_bwt_o[char][l] if l >= 0 else 0)
    
        if temp_k <= temp_l:
            # Deletion
            if prev_type == 2:
                sub_result, has_result = run_bwt_backtrack(s, i, diff-COST_GAP_EXTEND, temp_k, temp_l, 2)
                if has_result:
                    return sub_result, True
            else:
                sub_result, has_result = run_bwt_backtrack(s, i, diff-COST_GAP_EXTEND-COST_GAP_OPEN, temp_k, temp_l, 2)
                if has_result:
                    return sub_result, True
            if char == s[i]:
                # Match!
                sub_result, has_result = run_bwt_backtrack(s, i-1, diff+COST_MATCH, temp_k, temp_l, 0)
                if has_result:
                    return sub_result, True
            else:
                # Mismatch
                sub_result, has_result = run_bwt_backtrack(s, i-1, diff-COST_MISMATCH, temp_k, temp_l, 0)
                if has_result:
                    return sub_result, True
    return set(), False
    
# Sets up all required global indices to run the BWT aligner
# Then calls run_bwt_backtrack and collects all results
# bw -> BWT index for target
# bwr -> BWT reverse index for target
# s -> query
# diff -> maximum difference for curring recursive paths
# suffix_array -> target suffix array
def run_bwt(bw, bwr, s, diff, suffix_array):
    global _global_bwt_c, _global_bwt_o, _global_bwt_d

    # Reverse ranks
    reverse_ranks, _ = rank(bwr)
    # Forward ranks
    _global_bwt_o, tot = rank(bw)

    # Compute C, the number of lexicographically greater symbols in the ref
    _global_bwt_c = [0]*(max(ALPHABET)+1)
    for a, b in ALPHABET_LT_PAIRS:
        _global_bwt_c[a] += tot[b]

    # Compute estimated lower bounds of differences in substring s[0:i] for all  in [0,len(s)]
    k, z = 1, 0
    l = len(bw)-2
    _global_bwt_d = [0] * len(s)
    for i in range(0, len(s)):
        k = _global_bwt_c[s[i]] + reverse_ranks[s[i]][k-1] + 1
        l = _global_bwt_c[s[i]] + reverse_ranks[s[i]][l]
        if k > l:
            k = 1
            l = len(bw)-1
            z += 1
        _global_bwt_d[i] = z

    # Call the recursive search function and return a list of SA-range tuples
    sa_index_set, _ = run_bwt_backtrack(s, len(s)-1, diff, 0, len(bw)-1, 0)
    # Get possible best result
    best_index, score = max(sa_index_set, key=itemgetter(1), default=(-1, -1))
    if score < 0:
        return -1, -1
    # Map result to correct position
    return suffix_array[best_index]+1, score

# Entrypoint to the BWT aligner
# q -> query
# t -> target
# Returns <offset_left, offset_right, should_realign_right>
# Where should_realign_right means that all right offsets exceeded valid max_offset value
# This in hand suggests that we should use different approach to align the end of the sequence.
def run_match_align_bwt(q, t):
    fragment_size = round(len(q) * FACT_BWT_QUERY_FRAGMENT_SIZE)
    max_offset = round(len(q) * FACT_BWT_QUERY_MAX_OFFSET)
    fragment_offset = 0
    threshold = round(fragment_size * FACT_BWT_FRAGMENT_REL_ERRORS_THRESHOLD)
    
    off_r, off_l, realign_right = None, None, False
    # We iterate over parameters to make code easier to experiment with in different combintations
    for (
        is_right_offset,       # Indicates if we need to reverse indices. Also indicates if we set the result in off_l or off_r
        dna_fragment_start,    # Start of the target for lookup
        dna_fragment_end,      # End of the target for lookup
        query_fragment_start,  # Start of the query for lookup
        query_fragment_end,    # End of the query for lookup
        offset_return_pad,     # Assume the result was pos+offset_return_pad instead of pos (just adds extra padding before final calculations)
        max_offset,            # Max offset that is considered valid. If not valid we set overflow_offset (and then realign_right)
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

        # Translates regions of target and reverse target to build indices
        # We need lists as they're more performant (than numpy arrays for a lot of lookups)
        dna_fragment = t_fragment.tolist()
        dna_fragment_rev = t_fragment[::-1].tolist()

        dna_fragment.append(MAPPING_DOLLAR)
        dna_fragment_rev.append(MAPPING_DOLLAR)

        # Create suffix array (and BWT lookuop table)
        # Native aprpoach much better than skew algorithm for such short sequences
        suffix_array = [t[1] for t in sorted((dna_fragment[i:],i) for i in range(len(dna_fragment)))]
        bwt = [0]*len(dna_fragment)
        for v_rank in range(len(dna_fragment)):
            bwt[v_rank] = (dna_fragment[suffix_array[v_rank]-1])
    
        # Naive construction of suffix array for reverse target (and BWT lookuop table)
        suffix_array_rev = [t[1] for t in sorted((dna_fragment_rev[i:],i) for i in range(len(dna_fragment_rev)))]
        bwt_rev = [0]*len(dna_fragment_rev)
        for v_rank in range(len(dna_fragment_rev)):
            bwt_rev[v_rank] = (dna_fragment_rev[suffix_array_rev[v_rank]-1])

        query_string = q[query_fragment_start:query_fragment_end]
        
        # Start lookups using available thresholds
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
    # Makes possible to calcualte left offset if we already have the right one
    # (the guess would be good enough if the offset is small and valid)
    if off_l is None and off_r is not None and not realign_right:
        off_l = len(t) - off_r - len(q)
    return off_l, off_r, realign_right


# Makes sure 0 <= pos <= len
def normalize_pos(pos, len):
    return min(max(pos, 0), len)

# Generate hash mask for given kmer length
# The mask will be just 2*k last bits on for given k-mer len: k
# e.g mask(3) = b...000000111111
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


# Extract (k,w)-minimzers for given array
# Note that for performance reasons we use this function only for queries (not reference sequence itself)
def get_minimizers(
    seq_arr,
):
    sequence_len = len(seq_arr)
    mask = generate_mask(KMER_LEN)

    # Function to compute kmer value based on the previous (on the left side) kmer value and new nucleotide
    uadd = frompyfunc(lambda x, y: ((x << 2) | y) & mask, 2, 1)

    # This computes values for kmers
    # uadd/accumulate combintation is pretty performant and I found no better way to speed things up
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


# Claculate cartesian product of matches k-mers coorginates arrays (uint32)
# Formal definition: if a in A and b in B and c in C then (a,b,c) in pos_cartesian_product(A,B,C)
def pos_cartesian_product(*arrays):
    la = len(arrays)
    arr = empty([len(a) for a in arrays] + [la], dtype=uint32)
    for i, a in enumerate(ix_(*arrays)):
        arr[...,i] = a
    return arr.reshape(-1, la)

# Entrypoint to the aligner
def run_aligner_pipeline(
    reference_file_path: str,
    reads_file_path: str,
    output_file_path: str,
):
    # Thisable garbage collection, this should make memory usages a little bit better
    gc_disable()

    # Used to calculate the running time
    moment_start = time_ns()

    # Will contain fragments of reference that will be merged later
    target_seq_l = []
    # If the reference was all loaded
    ref_loaded = False
    # Buffer to accumulate reference file lines until it overflows buffer
    all_seq = ""
    # Total bases loaded for reference thus far
    all_seq_len = 0
    # Total offset for current reference chunk
    index_offset = 0

    # (k,w)-minimizer index for reference sequence
    ref_index = dict()
    # This part loads reference in chunks
    # Biopython slowed this part a lot and far better approach is just manually loading file line-by-line
    # We accumulate at least REFERENCE_INDEX_CHUNK_SIZE bases and then process them and merge into ref_index structure
    with open(reference_file_path) as ref_fh:
        for line in chain(ref_fh, [">"]):
            # Just a header
            if line[0] != '>':
                fasta_line = line.rstrip()
                all_seq += fasta_line
                all_seq_len  += len(fasta_line)
            # Processing of chunk
            if (all_seq_len >= REFERENCE_INDEX_CHUNK_SIZE or line[0] == '>') and all_seq_len > 0:
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

                # Move all counters and buffers to the right states
                index_offset += all_seq_len
                all_seq_len = 0
                all_seq = ""

                # Not sure if it helps here?
                del selected_kmers_unique_idx
                del selected_kmers_entries_split
                del selected_kmers
                gc_collect()
            if line[0] == '>':
                if ref_loaded:
                    break
                ref_loaded = True
                continue

    # Merge all chunks into one big numpy array 
    # This array (target_seq) will be now storing all the reference
    if len(target_seq_l) == 1:
        target_seq = target_seq_l[0]
    else:
        target_seq = concatenate(target_seq_l, axis=0, dtype=uint8)
    del target_seq_l

    # Don't know if that does anything?
    for i in range(3):
        gc_collect()

    # Otuput buffer to store file lines before doing write (to speed things up)
    output_buf = []
    # Reading FASTA file with queries
    with open(output_file_path, 'w') as output_file:
        # Stores loaded query content and its id
        query_id = ""
        query_seq = ""
        with open(reads_file_path) as reads_fh:
            for line in chain(reads_fh, [">"]):
                if line[0] == '>' and len(query_seq) > 0:
                    query_seq = MAPPING_FN(array(list(query_seq)))
                    # Make sure we catch all exceptions
                    try:
                        # Extract minimzers from the query
                        query_len = len(query_seq)
                        max_diff = round(query_len*FACT_LIS_MAX_QUERY_DISTANCE)
                        min_index_query = get_minimizers(
                            query_seq,
                        )
                        gc_collect()

                        # Finding common k-mers hashes
                        common_kmers = []
                        for key in min_index_query:
                            if key in ref_index:
                                common_kmers.append(key)
                                

                        # Find common kmers between (k,w)-minimizers structures
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
                        # Translate to list to speed up lookups
                        # This table for 20M reference and 1K queries shouldn't be that large
                        matches = matches[1:].tolist()
                        n = len(matches)
                        del min_index_query
                        gc_collect()
                        
                        relative_extension = round(KMER_LEN * FACT_KMER_TO_RELATIVE_EXTENSION_LEN) + 1
                        lis_accepted = False
                        match_start_t, match_end_t, match_start_q, match_end_q = 0, 0, 0, 0

                        if n < MIN_LIS_EXTENSION_WINDOW_LEN:
                            # We cannot formulate sensible matching segment from matched k-mers so we do nothing
                            pass
                        else:
                            # Heuristic LIS algorithm
                            # Instead of normal LIS alogirthm
                            # We do the following each time we perform binary search:
                            # If the start of the search window is too far (max_diff) in terms of target coordinates
                            # we save the value there (ending the binary search)
                            longest_seq_len = 0
                            parent = [maxsize]*(n+1)
                            increasingSub = [maxsize]*(n+1)
                            for i in range(n):
                                start = 1
                                end = longest_seq_len
                                while start <= end:
                                    middle = (start + end) // 2
                                    if matches[increasingSub[start]][0] + max_diff < matches[i][0]:
                                        break
                                    elif matches[increasingSub[middle]][1] >= matches[i][1]:
                                        end = middle - 1
                                    else:
                                        start = middle + 1    
                                parent[i] = increasingSub[start-1]
                                increasingSub[start] = i
                                if start > longest_seq_len:
                                    longest_seq_len = start

                            current_node = increasingSub[longest_seq_len]
                            lis_t = [0]*longest_seq_len
                            lis_q = [0]*longest_seq_len
                            lis_t[longest_seq_len-1] = matches[current_node][0]
                            lis_q[longest_seq_len-1] = matches[current_node][1]
                            for j in range(longest_seq_len-1, 0, -1):
                                current_node = parent[current_node]
                                lis_t[j-1] = matches[current_node][0]
                                lis_q[j-1] = matches[current_node][1]

                            scores = dict()
                            score_bucket_div = round(query_len * FACT_BWT_QUERY_FRAGMENT_SIZE)
                            score_1, score_2, score_3 = -max_diff, -max_diff, -max_diff

                            # Now we formulate a sliding window
                            # For each position i in all the lis table
                            # We do the following:
                            # 1. Find position in lis starting form i that has the lowest target position that is not further away from i than max_diff
                            # 2. Calculate the score for the widnow as min(|match_t|, |match_q|) - max(spaces(match_t), spaces(amtch_q))
                            #    Where spaces(seq) = sum[over each match Mi: i=0,1,...n-1 for sequence seq]{ max(M[i+1] - M[i], KMER_LEN) - KMER_LEN }
                            # 3. Select score that forms local naxima so one of two scenarios happend:
                            #    a.) score[i] < score[i+1] < score [i+2] -> we select score[i+1]
                            #    b.) score[i] < score[i+1] < score [i+2] and i+2 == len(lis) - 1 -> we select score[i+2]
                            # 4. Sort all selected lcoal maxima in descending order and take MAX_ACCEPTED_LIS_EXTENSION_WINDOWS_COUNT first ones
                            start = 0
                            end = bisect_right(lis_t, lis_t[0] + max_diff - lis_q[0]) - 1
                            spaces = 0
                            spaces_q = 0
                            for i in range(1, end+1):
                                spaces += max(lis_t[i] - lis_t[i-1], KMER_LEN) - KMER_LEN
                                spaces_q += max(lis_q[i] - lis_q[i-1], KMER_LEN) - KMER_LEN
                            for start in range(0, longest_seq_len):
                                if start+1 < longest_seq_len:
                                    spaces -= max(lis_t[start+1] - lis_t[start], KMER_LEN) - KMER_LEN
                                    spaces_q -= max(lis_q[start+1] - lis_q[start], KMER_LEN) - KMER_LEN
                                new_end = bisect_right(lis_t, lis_t[start] + max_diff - lis_q[start], lo=end) - 1
                                for i in range(end+1, new_end+1):
                                    spaces += max(lis_t[i] - lis_t[i-1], KMER_LEN) - KMER_LEN
                                    spaces_q += max(lis_q[i] - lis_q[i-1], KMER_LEN) - KMER_LEN
                                end = new_end
                                wnd_len = end - start + 1
                                if wnd_len > MIN_LIS_EXTENSION_WINDOW_LEN:
                                    estimated_matches_q = lis_q[end] - lis_q[start]
                                    estimated_matches_t = lis_t[end] - lis_t[start]
                                    score = (min(estimated_matches_q, estimated_matches_t) - max(spaces, spaces_q)/2)/query_len
                                    score_1 = score_2
                                    score_2 = score_3
                                    score_3 = score

                                    if score_2 > score_1 and score_2 > score_3:
                                        # Local maximum
                                        if score_2 > MIN_LIS_EXTENSION_WINDOW_SCORE:
                                            score_bucket = lis_t[end] // score_bucket_div
                                            if (score_bucket not in scores) or (score_bucket in scores and score_2 > scores[score_bucket][0]):
                                                scores[score_bucket] = (score_2, lis_t[start], lis_t[end], lis_q[start], lis_q[end])
                                    if end >= longest_seq_len - 1:
                                        break
                            if score_3 > score_2 and score_3 > score_1:
                                if score_3 > MIN_LIS_EXTENSION_WINDOW_SCORE:
                                    score_bucket = lis_t[end] // score_bucket_div
                                    if (score_bucket not in scores) or (score_bucket in scores and score_3 > scores[score_bucket][0]):
                                        scores[score_bucket] = (score_3, lis_t[start], lis_t[end], lis_q[start], lis_q[end])
                                
                            scores = sorted(scores.values(), reverse=True)[:MAX_ACCEPTED_LIS_EXTENSION_WINDOWS_COUNT]
                            if len(scores) > 1:
                                # If we have multiple accepted then we use call to DP aligner to select the most suitable one
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
                            elif len(scores) == 0:
                                # If there are no scores, we rely on the last resort method
                                # We calcualte normal LIS (longest increasing subsequence)
                                # Then we use sliding window and determine the score for the windows as:
                                #    min(|match_q|, |match_t|)^2 - sum(diff(match_t))
                                # The scoring method is not perfect but is good enough
                                longest_seq_len = 0
                                parent = [maxsize]*(n+1)
                                increasingSub = [maxsize]*(n+1)
                                for i in range(n):
                                    start = 1
                                    end = longest_seq_len
                                    while start <= end:
                                        middle = (start + end) // 2
                                        if matches[increasingSub[middle]][1] < matches[i][1]:
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
                                match_score = -max_diff
                                for i in range(longest_seq_len):
                                    # Find end of the window (target position)
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
                                if match_score > -max_diff:
                                    lis_accepted = True
                                else:
                                    # We've tried our best, but there's no much after all attempts
                                    pass
                            else:
                                _, match_start_t, match_end_t, match_start_q, match_end_q = scores[0]
                                lis_accepted = True
                            
                            del scores
                            del lis_t
                            del lis_q
                        
                        del matches
                        gc_collect()

                        # Flip to True if we're absolutely sure we found a good match position
                        can_submit = False
                        if not lis_accepted or abs(match_end_t - match_start_t) > max_diff + relative_extension:
                            # We cannot submit sequence if the lis_accepted is not set to True or the length of target match is far greater than what we allow
                            pass
                        else:
                            # We have sensible match so the last step is to correctly align it
                            q_begin, q_end = 0, len(query_seq)
                            t_begin, t_end = match_start_t - match_start_q - relative_extension, match_end_t + (len(query_seq)-match_end_q) + relative_extension

                            q_begin, q_end = normalize_pos(q_begin, len(query_seq)), normalize_pos(q_end, len(query_seq))
                            t_begin, t_end = normalize_pos(t_begin, len(target_seq)), normalize_pos(t_end, len(target_seq))

                            # We loop here because we want to do the following:
                            # 1. Try BWT aligner
                            # 2. If the BWT aligner returns realign_right (should_realign_right) set to true, then we also run DP aligner
                            #    (This happens when we're too unsure about right match position)
                            # 3. If the aligned match is not longer than |query|xFACT_TARGET_TO_QUERY_MAX_RELATIVE_LENGTH, then we do nothing more
                            # 4. We try to set starting position based on the end position we calcualted (do something like start = end - |query| - some_extra_padding)
                            # 5. We go through the [1-3] sequence one more time
                            for i in range(2):
                                realign_mode = 0
                                # Fast but often inaccurate
                                # should_realign_right - means that wefound good position for an end, but it was offset by a large amount of bases
                                # If may signify that there are many errors accumualted there and we shoud invoke better aligner
                                t_begin_pad, t_end_pad, should_realign_right = run_match_align_bwt(
                                    query_seq,
                                    target_seq[t_begin:t_end],
                                )
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
                                    # Slower but better-quality aligner
                                    _, _, t_end_pad = run_match_align_dp(
                                        target_seq[t_begin:t_end],
                                        query_seq,
                                        align_mode=realign_mode,
                                    )
                                    t_begin_pad = 0 # Overwrite left offset as we care only about the right side for now
                                    gc_collect()

                                if not should_realign_right and t_begin_pad is None:
                                    t_begin_pad = relative_extension
                                if not should_realign_right and t_end_pad is None:
                                    t_end_pad = relative_extension

                                if t_begin_pad is not None:
                                    t_begin += t_begin_pad
                                if t_end_pad is not None:
                                    t_end -= t_end_pad

                                if abs(t_end-t_begin) > len(query_seq)*FACT_TARGET_TO_QUERY_MAX_RELATIVE_LENGTH:
                                    # We have a clear problem with this alignment (as it's too big to make sense)
                                    # In that case we will try to estimate starting position from the end and try aligning one more time
                                    t_begin = t_end - len(query_seq) - relative_extension
                                    continue
                                else:
                                    can_submit = True
                                    break

                            if not can_submit:
                                # Cannot submit! Problem with matching
                                pass
                            else:
                                # Write output data
                                output_buf.append(f"{query_id}\t{t_begin}\t{t_end}\n")
                                if len(output_buf) > 1500:
                                    output_file.writelines(output_buf)
                                    output_buf = []
                                    gc_collect()
                    except Exception as e:
                        # Print exception and ignores it so we don't interrupt all processing if something unknown happens
                        print(e)
                if line[0] == '>':
                    # Load another query header
                    query_seq = ""
                    query_id = line[1:].strip()
                else:
                    query_seq += line.rstrip()
        # Dump all results to the file
        if len(output_buf) > 0:
            output_file.writelines(output_buf)
        # Print final execution message with time data
        moment_end = time_ns()
        print(f"Wrote records to {output_file_path} in {((moment_end-moment_start) // 10000000)/100} sec.")

def run_alignment_cli():
    run_aligner_pipeline(
        reference_file_path=sys_argv[1],
        reads_file_path=sys_argv[2],
        output_file_path="output.txt",
    )

if __name__ == '__main__':
    run_alignment_cli()
