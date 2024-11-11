# Implementation based on https://www.cs.helsinki.fi/u/tpkarkka/publications/jacm05-revised.pdf and https://mailund.dk/posts/skew-python-go/

Mtarget="AGATCCTGTTCTGGTCAGCAGGGTGGTGACCAGAAAACAAGTTCTTCTGGTCTCCTACCTCAGTTGCAAGAAACTCAAAACTCCATTTTAAACTTTAAGCCTTATAGTATCTTCTGTTTGAGTATTTAACAAGCATTTGTTTTTCCTTGAAAATATATCCAGCCAAGACCTTATGAAGAATGTGAGCTAAAATTACGGTATTTTACTTGCCTGGAAACAGTACTTCTCAAATGTTAATGTGCATACAGGTGACTTGAGTATCTCATTAAAACGTAGATTTAGATTGTGTCTCCGGGGTGAGGTCTGAGAAACTTCATTTCTATAAAGTGATGTCAAAGTTACTGGTCTGGGGACCATGTTTTGGTGGCAAGTTGCAAGACCCCAGAGTTTCTACCCTAATGATTTATTCAATGACTCTTAGAGGTGTTATCAATCTGTTTTTAAAGCCAGGGACTTTGCCCAGGGGAAAAATGCATGCATACACACACACACACACACACACACACACACACACCCCTATGCGTACAATTTCGGAGCAATTGTGCATTCATTCAGTGAATACCTATTTAATGCACACCAAGTATCTTTCCGGGTGCTAATGACAGAGTGAGGAACAAGATAGCAAAGATGCCTGCCTTGTGGAGCTTTCATTATACTGTTGGTTGGAAGACAAACTAAGTAAATAAAGCAGGCATGTCAGCTATGATACATGCCTTAGGACAGTGTACTGCAGCCACGTGATACAGGGATTGGGTGGAGGAAGGTTTGAAGTGGCTCTTTTAAATTGTTCTGTCTGTGGAGGCATCTTGGCCAGGAACCTGAATGCCAGTATCTGGGGAAAAGCATCCTAGGTGGTAAGTTCGGAGCACCTGAAGCAGAAATGAGTTTAGTGTTTTCAAAAGTTAGAGAGAACTGGTATGGGAGGAACAGAGCGAGTGGAGGAGAGAGCAAAAGGTGAAATCCCAGAGGTAGAAGGGCCTGATCCTACAGGAGCTTGTAGGCCATGACAAGGAGGCTGCCTGCACTTGACCGAGCCCATGCTTAGATCCTAAAGGAGCCATGGCCTCCATTTAAGAACTCCAGACAGAGAATC"
Mquery="AGAAAACCAGTTCTTCTGGTCTCCTACCTCAGTTGCAAGAAACCCAAAACTCCATTTTAAACTTTCAGCCTTATAGTATCTTCTGTTTGAGTATTTAACAAGCAGTTGTTTTTCCTTGAAAATAACCAGCCAAGGCCTTATGACGAATGTCAGCTAAAATTACGGTATTTTACTTGCCTGGAAACAGTACTTCTCAAAAGTTAATGTGCATAGTGGTAACTTGAGTATCTATCATTAAAACGTAGATTTAGATTGTGTCTCCGGGGTGAGGTCGGAGAAACTTCATCTCTATAAAGTGATGTCAAAGTTACTGGTCTGGGGACCATGTTTTGGTGGCAAGTTGCAAGAGCCGAGAGTTGCTACCCTAATGATTTATTCAATGACTCTTAGAGGTGTTATCAATCTGTTTTTAAAGGCAGGGACTTTGCCCAGGGGAAAAACGCATGCATACACACACACACACACACACACACACACACACACCCCTATGCGGACAATTTAGGAGCAATTATGCATTCATTCAGTGAATACATATTTAATGCACACCAAGTATCTTTCCGTGTGCTAATGACAGAGTGAGGAACAGTATAGCAAAGATGCCTGCCTCGTGGAGATATCATTATAACTGTTGGTTGGAAGACAACTAAGTAAATAAAGCAGGCAAGTCAGCTATGATACATGCCTTAGGACAGTGTACTGCAGCCACGTGATTCAGGGATTGGGTGGAGGAAGGTTTGAAGTGGCTCTTTTAATGTTCTGTCTGTGGAGGCATCTTGGCCAGGAACCTGAATGCCGGTATCTGGGTAAAAGCATCCTAGGTGGTAAGTTCGGAGCACCTGAAGAGAAATGAGATTAGTGAATTCAAAAGTTAGAGAGCACTGGTATGGGAGGAACAGAGCTAGTGGAGGAGAGAGCAAAAGTGAAATCCCAGAGGTAGAAGGGCCTGATCCTACAGGAGCTTGTAGGCCATGACAAAGAGGCTGCCTGCACTTGACCGAGC"


import sys
import numpy as np
from typing import Tuple


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

from operator import itemgetter

# def skew_dna(x: str) -> np.array:
#     str_to_int = {
#         "$": 0,  # End of string
#         "A": 1,
#         "C": 2,
#         "G": 3,
#         "N": 4,  # Unknown nucleotide
#         "T": 5,
#     }
#     return skew_rec(np.array([str_to_int[y] for y in x]), 6)

# defined below
C = {}
O = {}
D = []

# rewards/penalties
gap_open = 3
gap_ext = 1
mismatch = 1
match = 0

# option switches
NO_INDELS = False
sub_mat = {}

num_prunes = 0

alphabet = {1, 2, 3, 5}

# insertion -> 1
# delection -> 2
# match -> 0
# mismatch -> 3
# start -> 4

def compute_C(totals):
    """compute C, the number of lexicographically greater symbols in the ref"""
    C = {1: 0, 2: 0, 3: 0, 5: 0, 0: 0}
    for k in alphabet:
        for ref in alphabet:
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
    
    if not NO_INDELS:
        # Insertion
        if prev_type == 1:
            sa_idx = sa_idx.union(inexact_recursion(s, i-1, diff-gap_ext, k, l, 1))
        else:
            sa_idx = sa_idx.union(inexact_recursion(s, i-1, diff-gap_ext-gap_open, k, l, 1))

    for char in alphabet:
        temp_k = C[char] + get_O(char, k-1) + 1
        temp_l = C[char] + get_O(char, l)
    
        if temp_k <= temp_l:
            if not NO_INDELS:
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

    for char in alphabet:
        if (char not in totals) and (char != 0): # '$':
            totals[char] = 0
            ranks[char] = []

    for char in bw:
        if char != 0: # '$':
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

def setup_ref(t):
    str_to_int = {
        "$": 0,  # End of string
        "A": 1,
        "C": 2,
        "G": 3,
        "N": 4,  # Unknown nucleotide
        "T": 5,
    }
    seq_t = t+"$"
    dna_string = np.array([str_to_int[y] for y in seq_t])
    

def doit(q, t):
    # q = Mquery
    # t = Mtarget
    seq_t = t+"$"
    seq_tr = t[::-1]+"$"
    seq_q = q[:100]
    seq_q_r = q[len(q)-100:]

    str_to_int = {
        "$": 0,  # End of string
        "A": 1,
        "C": 2,
        "G": 3,
        "N": 4,  # Unknown nucleotide
        "T": 5,
    }
    threshold = 8
    max_offset = 20
    dna_string = np.array([str_to_int[y] for y in seq_t])
    dna_string_r = np.array([str_to_int[y] for y in seq_tr])
    query_string = np.array([str_to_int[y] for y in seq_q])
    query_string_r = np.array([str_to_int[y] for y in seq_q_r])

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
    

if __name__ == "__main__":
    np.set_printoptions(threshold=sys.maxsize)
    print(doit(Mquery, Mtarget))