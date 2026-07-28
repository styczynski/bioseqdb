#!/usr/bin/env python
# wagnerfischer.py: Dynamic programming Levensthein distance function 
# Kyle Gorman <gormanky@ohsu.edu>
# 
# Based on:
# 
# Robert A. Wagner and Michael J. Fischer (1974). The string-to-string 
# correction problem. Journal of the ACM 21(1):168-173.
#
# The thresholding function was inspired by BSD-licensed code from 
# Babushka, a Ruby tool by Ben Hoskings and others.
# 
# Unlike many other Levenshtein distance functions out there, this works 
# on arbitrary comparable Python objects, not just strings.

from numpy import zeros
import numpy as np
def _zeros(*shape):
    """ like this syntax better...a la MATLAB """
    return zeros(shape)


def _dist(A, B, insertion, deletion, substitution):
    D = _zeros(len(A) + 1, len(B) + 1)
    for i in range(len(A)): 
        D[i + 1][0] = D[i][0] + deletion
    for j in range(len(B)): 
        D[0][j + 1] = D[0][j] + insertion
    for i in range(len(A)): # fill out middle of matrix
        for j in range(len(B)):
            if A[i] == B[j]:
                D[i + 1][j + 1] = D[i][j] # aka, it's free. 
            else:
                D[i + 1][j + 1] = min(D[i + 1][j] + insertion,
                                      D[i][j + 1] + deletion,
                                      D[i][j]     + substitution)
    return D

def _dist_thresh(A, B, thresh, insertion, deletion, substitution):
    D = _zeros(len(A) + 1, len(B) + 1)
    for i in range(len(A)):
        D[i + 1][0] = D[i][0] + deletion
    for j in range(len(B)): 
        D[0][j + 1] = D[0][j] + insertion
    for i in range(len(A)): # fill out middle of matrix
        for j in range(len(B)):
            if A[i] == B[j]:
                D[i + 1][j + 1] = D[i][j] # aka, it's free. 
            else:
                D[i + 1][j + 1] = min(D[i + 1][j] + insertion,
                                      D[i][j + 1] + deletion,
                                      D[i][j]     + substitution)
        if min(D[i + 1]) >= thresh:
            return None, False
    return D, True

def _levenshtein(A, B, insertion, deletion, substitution):
    return _dist(A, B, insertion, deletion, substitution)[len(A)][len(B)]

def _levenshtein_ids(A, B, insertion, deletion, substitution):
    """
    Perform a backtrace to determine the optimal path. This was hard.
    """
    D = _dist(A, B, insertion, deletion, substitution)
    i = len(A) 
    j = len(B)
    ins_c = 0
    del_c = 0
    sub_c = 0
    while True:
        if i > 0:
            if j > 0:
                if D[i - 1][j] <= D[i][j - 1]: # if ins < del
                    if D[i - 1][j] < D[i - 1][j - 1]: # if ins < m/s
                        ins_c += 1
                    else:
                        if D[i][j] != D[i - 1][j - 1]: # if not m
                            sub_c += 1
                        j -= 1
                    i -= 1
                else:
                    if D[i][j - 1] <= D[i - 1][j - 1]: # if del < m/s
                        del_c += 1
                    else:
                        if D[i][j] != D[i - 1][j - 1]: # if not m
                            sub_c += 1
                        i -= 1
                    j -= 1
            else: # only insert
                ins_c += 1
                i -= 1
        elif j > 0: # only delete
            del_c += 1
            j -= 1
        else: 
            return (ins_c, del_c, sub_c)

    
def _levenshtein_thresh(A, B, thresh, insertion, deletion, substitution):
    D, ok = _dist_thresh(A, B, thresh, insertion, deletion, substitution)
    if ok:
        return D[len(A)][len(B)]
    return None

def levenshtein(A, B, thresh=None, insertion=1, deletion=1, substitution=1):
    """
    Compute levenshtein distance between iterables A and B
    These are standard cases:
    >>> print int(levenshtein('god', 'gawd'))
    2
    >>> print int(levenshtein('sitting', 'kitten'))
    3
    >>> print int(levenshtein('bana', 'banananana'))
    6
    >>> print int(levenshtein('bana', 'bana'))
    0
    >>> print int(levenshtein('banana', 'angioplastical'))
    11
    >>> print int(levenshtein('angioplastical', 'banana'))
    11
    >>> print int(levenshtein('Saturday', 'Sunday'))
    3
    
    These should return nothing:
    >>> levenshtein('banana', 'angioplasty', 5)
    >>> levenshtein('banana', 'angioplastical', 5)
    >>> levenshtein('angioplastical', 'banana', 5)
    """
    # basic checks
    if len(A) == len(B) and A == B:
        return 0       
    if len(B) > len(A):
        (A, B) = (B, A)
    if len(A) == 0:
        return len(B)
    if thresh:
        if len(A) - len(B) > thresh:
            return None
        return _levenshtein_thresh(A, B, thresh, insertion, deletion,
                                                            substitution)
    else: 
        return _levenshtein(A, B, insertion, deletion, substitution)

def levenshtein_ids(A, B, insertion=1, deletion=1, substitution=1):
    """
    Compute number of insertions deletions, and substitutions for an 
    optimal alignment.
    There may be more than one, in which case we disfavor substitution.
    >>> print levenshtein_ids('sitting', 'kitten')
    (1, 0, 2)
    >>> print levenshtein_ids('banat', 'banap')
    (0, 0, 1)
    >>> print levenshtein_ids('Saturday', 'Sunday')
    (2, 0, 1)
    """
    # basic checks
    if len(A) == len(B) and A == B:
        return (0, 0, 0)
    if len(B) > len(A):
        (A, B) = (B, A)
    if len(A) == 0:
        return len(B)
    else: 
        return _levenshtein_ids(A, B, insertion, deletion, substitution)


if __name__ == '__main__':
    Mquery = "GTAGATATGTGGCTTTATTTCTGAGGACTCTGTTCTGTTCCGTTGATCGATCTCTGTTTTGGTACCAGTATCATGCTGTTTTGGTTACTGGAGCCTGTTAGCATAATTTGAAGTCAGGTAGTGTGATGCCTCCAGCTTTGTTCTTTTAGCTTAGGATAGACTTGGCAATCCGACCTCTTTTATGGTTCCACTATCAACTTTAAAGTATTTTTTACCACTTTTGTGAAGAACGTCATTGTTCAGCTTGATGGGGGTGGCCTTGTTTCTCTAAATTACCTTGGGCAGTAAGGCCATTCTCACGATATTGATTCGTCCTACCCATGAGCATGGAATGTTCTTCTATTTGTTGGGTGCTCGTATATTTCCATGAGCAGTGTGGTTCGTAGTTCTCCTTGAAGAGGTCCTCACATCCCTTGTAAGTTGGATTCCTTGGTATTTTATTCTCTTTTAAGCAATTGTGAATAGGACTTCACCCATGATTTGGCTCTCGGTTTCTCTGTTGTTGTATAAGAATGCTTGAGATATTAGTACATTGATTTTGCATCATGAGCCTTTGCTGAAGTTGCTTATCGGTTTAAGGACATTTTGGCCTGATACGGTGGATGGGTCTGTCTAGATACAATAATGTCGTCTGCAAACAGGGACAATGTGACTTCCTATTTTCCTAATTGAATACCCTTTATTTCCTTCTCCTGCCTGAATGCCCTGGGCTGAACTTCCAACGCTATGTTGAATAGGAGTGGGGAGACAGGGGATCCCTGTCCCGTGTACCAGTTTAAAAAGGGAATGCTTCCAGTTTTTGCCCATTCAGTATGATTATGGCTGTGGGTTTGTCAAAGATTGCTCTTATTATTTTGAGATACATCCCATCAATACCTAATTTATTGAGAGTTTTTAGCATGAAGGGTTCTTAAATTTTGTCAAACGCTTTTTCTTCATCTAGTGAGCTAATCACGTGGTTTTTGTCTTTGGCTCTGTTATATATGCTGGATTACATTTA"
    Mtarget = "CAGATAGTTGTAGATATGCGGCGTTATTTCTGAGGGCTCTGTTCTGTTCCATTGATCTATATCTCTGTTTTGGTACCAGTACCATGCTGTTTTGGTTACTGTAGCCTTGTAGTATAGTTTGAAGTCAGGTAGTGTGATGCCTCCAGCTTTGTTCTTTTGGCTTAGGATTGACTTGGCGATGCGGGCTCTTTTTTGGTTCCATATGAACTTTAAAGTAGTTTTTTCCAATTCTGTGAAGAAAGTCATTGGTAGCTTGATGGGGATGGCATTGAATCTGTAAATTACCTTGGGCAGTATGGCCATTTTCACGATATTGATTCTTCCTACCCATGAGCATGGAATGTTCTTCCATTTGTTTGTCTCCTCTTTTATTTCCTTGAGCAGTGGTTTGTAGTTCTCCTTGAAGAGGTCCTTCACATCCCTTGTAAGTTGGATTCCTAGGTATTTTATTCTCTTTGAAGCAATTATGAATGGGAGTTCACCCATGATTTGGCTCTCTGTTTGTCTGTTGTTGGTGTATAAGAATGCTTGTGACTTTTGTACATTGATTTTGTATCCTGAGACTTTGCTGAAGTTGCTTATCAGCTTAAGGAGATTTTGGGCTGAGACGATGGGGTTTTCTAGATAAACAATCATGTCGTCTGCAAACAGGGACAATTTGACTTCCTCTTTTCCTAATTGAATACCCTTTATTTCCTTCTCCTGCCTGATTGCCCTGGCCAGAACTTCCAACACTATGTTGAATAGGAGCGGTGAGAGAGGGCATCCCTGTCTTGTGCCAGTTTTCAAAGGGAATGCTTCCAGTTTTTGCCCATTCAGTATGATATTGGCTGTGGGTTTGTCATAGATAGCTCTTATTATTTTGAAATACGTCCCATCAATACCTAATTTATTGAGAGTTTTTAGCATGAAGGGTTGTTGAATTTTGTCAAAGGCTTTTTCTGCATCTATTGAGATAATCATGTGGCTTTTGTCTTTGGCTCTGTTTATATGCTGGATTACATTTATCGATTTGCGTATA"
    print(levenshtein(Mquery, Mtarget))