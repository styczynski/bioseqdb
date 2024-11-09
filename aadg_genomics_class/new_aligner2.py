# aligner 2.0

#target="AGATCCTGTTCTGGTCAGCAGGGTGGTGACCAGAAAACAAGTTCTTCTGGTCTCCTACCTCAGTTGCAAGAAACTCAAAACTCCATTTTAAACTTTAAGCCTTATAGTATCTTCTGTTTGAGTATTTAACAAGCATTTGTTTTTCCTTGAAAATATATCCAGCCAAGACCTTATGAAGAATGTGAGCTAAAATTACGGTATTTTACTTGCCTGGAAACAGTACTTCTCAAATGTTAATGTGCATACAGGTGACTTGAGTATCTCATTAAAACGTAGATTTAGATTGTGTCTCCGGGGTGAGGTCTGAGAAACTTCATTTCTATAAAGTGATGTCAAAGTTACTGGTCTGGGGACCATGTTTTGGTGGCAAGTTGCAAGACCCCAGAGTTTCTACCCTAATGATTTATTCAATGACTCTTAGAGGTGTTATCAATCTGTTTTTAAAGCCAGGGACTTTGCCCAGGGGAAAAATGCATGCATACACACACACACACACACACACACACACACACACCCCTATGCGTACAATTTCGGAGCAATTGTGCATTCATTCAGTGAATACCTATTTAATGCACACCAAGTATCTTTCCGGGTGCTAATGACAGAGTGAGGAACAAGATAGCAAAGATGCCTGCCTTGTGGAGCTTTCATTATACTGTTGGTTGGAAGACAAACTAAGTAAATAAAGCAGGCATGTCAGCTATGATACATGCCTTAGGACAGTGTACTGCAGCCACGTGATACAGGGATTGGGTGGAGGAAGGTTTGAAGTGGCTCTTTTAAATTGTTCTGTCTGTGGAGGCATCTTGGCCAGGAACCTGAATGCCAGTATCTGGGGAAAAGCATCCTAGGTGGTAAGTTCGGAGCACCTGAAGCAGAAATGAGTTTAGTGTTTTCAAAAGTTAGAGAGAACTGGTATGGGAGGAACAGAGCGAGTGGAGGAGAGAGCAAAAGGTGAAATCCCAGAGGTAGAAGGGCCTGATCCTACAGGAGCTTGTAGGCCATGACAAGGAGGCTGCCTGCACTTGACCGAGCCCATGCTTAGATCCTAAAGGAGCCATGGCCTCCATTTAAGAACTCCAGACAGAGAATC"
#query="AGAAAACCAGTTCTTCTGGTCTCCTACCTCAGTTGCAAGAAACCCAAAACTCCATTTTAAACTTTCAGCCTTATAGTATCTTCTGTTTGAGTATTTAACAAGCAGTTGTTTTTCCTTGAAAATAACCAGCCAAGGCCTTATGACGAATGTCAGCTAAAATTACGGTATTTTACTTGCCTGGAAACAGTACTTCTCAAAAGTTAATGTGCATAGTGGTAACTTGAGTATCTATCATTAAAACGTAGATTTAGATTGTGTCTCCGGGGTGAGGTCGGAGAAACTTCATCTCTATAAAGTGATGTCAAAGTTACTGGTCTGGGGACCATGTTTTGGTGGCAAGTTGCAAGAGCCGAGAGTTGCTACCCTAATGATTTATTCAATGACTCTTAGAGGTGTTATCAATCTGTTTTTAAAGGCAGGGACTTTGCCCAGGGGAAAAACGCATGCATACACACACACACACACACACACACACACACACACCCCTATGCGGACAATTTAGGAGCAATTATGCATTCATTCAGTGAATACATATTTAATGCACACCAAGTATCTTTCCGTGTGCTAATGACAGAGTGAGGAACAGTATAGCAAAGATGCCTGCCTCGTGGAGATATCATTATAACTGTTGGTTGGAAGACAACTAAGTAAATAAAGCAGGCAAGTCAGCTATGATACATGCCTTAGGACAGTGTACTGCAGCCACGTGATTCAGGGATTGGGTGGAGGAAGGTTTGAAGTGGCTCTTTTAATGTTCTGTCTGTGGAGGCATCTTGGCCAGGAACCTGAATGCCGGTATCTGGGTAAAAGCATCCTAGGTGGTAAGTTCGGAGCACCTGAAGAGAAATGAGATTAGTGAATTCAAAAGTTAGAGAGCACTGGTATGGGAGGAACAGAGCTAGTGGAGGAGAGAGCAAAAGTGAAATCCCAGAGGTAGAAGGGCCTGATCCTACAGGAGCTTGTAGGCCATGACAAAGAGGCTGCCTGCACTTGACCGAGC"
#queryCUT="AGCAGTTGTTTTTCCTTGAAAATAACCAGCCAAGGCCTTATGACGAATGTCAGCTAAAATTACGGTATTTTACTTGCCTGGAAACAGTACTTCTCAAAAGTTAATGTGCATAGTGGTAACTTGAGTATCTATCATTAAAACGTAGATTTAGATTGTGTCTCCGGGGTGAGGTCGGAGAAACTTCATCTCTATAAAGTGATGTCAAAGTTACTGGTCTGGGGACCATGTTTTGGTGGCAAGTTGCAAGAGCCGAGAGTTGCTACCCTAATGATTTATTCAATGACTCTTAGAGGTGTTATCAATCTGTTTTTAAAGGCAGGGACTTTGCCCAGGGGAAAAACGCATGCATACACACACACACACACACACACACACACACACACCCCTATGCGGACAATTTAGGAGCAATTATGCATTCATTCAGTGAATACATATTTAATGCACACCAAGTATCTTTCCGTGTGCTAATGACAGAGTGAGGAACAGTATAGCAAAGATGCCTGCCTCGTGGAGATATCATTATAACTGTTGGTTGGAAGACAACTAAGTAAATAAAGCAGGCAAGTCAGCTATGATACATGCCTTAGGACAGTGTACTGCAGCCACGTGATTCAGGGATTGGGTGGAGGAAGGTTTGAAGTGGCTCTTTTAATGTTCTGTCTGTGGAGGCATCTTGGCCAGGAACCTGAATGCCGGTATCTGGGTAAAAGCATCCTAGGTGGTAAGTTCGGAGCACCTGAAGAGAAATGAGATTAGTGAATTCAAAAGTTAGAGAGCACTGGTATGGGAGGAACAGAGCTAGTGGAGGAGAGAGCAAAAGTGAAATCCCAGAGGTAGAAGGGCCTGATCCTACAGGAGCTTGTAGGCCATGACAAAGAGGCTGCCTGCACTTGACCGAGC"
expected_offsets = (31, 58)

#!/usr/bin/env python3
# -*- coding: utf-8 -*-

from collections import defaultdict
import sys
import numpy

# DP algorithm adapted from Langmead's notebooks
def _trace(D, x, y):
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
def _kEditDp(p, t):
    ''' Find the alignment of p to a substring of t with the fewest edits.  
        Return the edit distance and the coordinates of the substring. '''
    D = numpy.zeros((len(p)+1, len(t)+1), dtype=int)
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
    off = _trace(D, p, t[:mnJ])
    # Return edit distance and t coordinates of aligned substring
    return mn, off, mnJ

# example index
class simpleIndex:
    def __init__(self, text, k, step):
        self.k = k
        self.text = text
        self.kmers = defaultdict(list)
        for i in range(0, len(text)-k+1, step):
            self.kmers[text[i:i+k]].append(i)
    def query(self, pattern, edist, step):
        hits = []
        for i in range(0, len(pattern)-self.k+1, step):
            for j in self.kmers[pattern[i:i+self.k]]:
                lf = max(0, j-i-edist)
                rt = min(len(self.text), j-i+len(pattern)+edist)
                mn, soff, eoff = _kEditDp(pattern, self.text[lf:rt])
                soff += lf
                eoff += lf
                if mn<=edist:
                    hits.append((mn, soff, eoff))
        hits.sort()
        return hits

def run_align(target, query):
    index = simpleIndex(str(target), 20, 11)

    print(len(target))
    print(len(query))

    hits = index.query(str(query), len(query)//9, 12)
    if hits:
        return hits[0][1], len(target)-hits[0][2]
    return 0, 0

# def align_seq(target, query):
#     return run_align(target, query)

def align_seq(target, query):
    pref_len_factor = 0.42
    suff_len_factor = 0.42
    pad = 25

    pref_len = round(len(query) * pref_len_factor)
    suff_len = round(len(query) * suff_len_factor)

    l, _ = run_align(target[:len(target)-pref_len+pad], query[:pref_len])
    _, r = run_align(target[suff_len+l-pad:], query[(len(query)-suff_len):])
    return l, r