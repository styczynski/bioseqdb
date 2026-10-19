#!/usr/bin/env python3
"""Small Python sequence-mapping examples from the BioseqDB tutorial.

Generated from website/blog/writing-a-dna-read-mapper-in-python.md.
Run with Python 3; only the standard library is required.
This teaching implementation is not the historical course submission.
"""

# Example: py_aligner_alphabet
from bisect import bisect_left
from collections import defaultdict
from itertools import product
from random import Random

def dna(text):
    text = text.upper()
    if set(text) - set("ACGT"):
        raise ValueError("This small example accepts A, C, G and T only")
    return text

def reverse_complement(text):
    return dna(text).translate(str.maketrans("ACGT", "TGCA"))[::-1]

# Example: py_aligner_kmers
def rolling_kmers(text, k):
    if k < 1:
        raise ValueError("k must be positive")
    codes = {base: code for code, base in enumerate("ACGT")}
    mask = (1 << (2 * k)) - 1
    value = 0
    result = []
    for end, base in enumerate(dna(text)):
        value = ((value << 2) | codes[base]) & mask
        if end >= k - 1:
            result.append((value, end - k + 1))
    return result

# Example: py_aligner_minimizers
def minimizers(text, k, w):
    if w < 1:
        raise ValueError("w must be positive")
    kmers = rolling_kmers(text, k)
    result = []
    for start in range(len(kmers) - w + 1):
        selected = min(kmers[start:start + w])
        if not result or selected != result[-1]:
            result.append(selected)
    return result

# Example: py_aligner_anchors
def seed_index(text, k, w):
    index = defaultdict(list)
    for key, position in minimizers(text, k, w):
        index[key].append(position)
    return dict(index)

def shared_anchors(reference, query, k, w):
    ref = seed_index(reference, k, w)
    qry = seed_index(query, k, w)
    anchors = [
        (r, q)
        for key in ref.keys() & qry.keys()
        for r, q in product(ref[key], qry[key])
    ]
    # Descending q for equal r prevents a strict LIS from using one r twice.
    return sorted(set(anchors), key=lambda anchor: (anchor[0], -anchor[1]))

# Example: py_aligner_lis
def lis_chain(anchors):
    anchors = sorted(set(anchors), key=lambda anchor: (anchor[0], -anchor[1]))
    tail_values, tail_indices = [], []
    parents = [-1] * len(anchors)
    for i, (_, q) in enumerate(anchors):
        length = bisect_left(tail_values, q)
        if length:
            parents[i] = tail_indices[length - 1]
        if length == len(tail_values):
            tail_values.append(q)
            tail_indices.append(i)
        else:
            tail_values[length] = q
            tail_indices[length] = i
    chain = []
    current = tail_indices[-1] if tail_indices else -1
    while current != -1:
        chain.append(anchors[current])
        current = parents[current]
    return chain[::-1]

# Example: py_aligner_cutoff
def historical_cutoff_chain(anchors, max_diff):
    anchors = sorted(set(anchors), key=lambda anchor: (anchor[0], -anchor[1]))
    tails = [-1] * (len(anchors) + 1)
    parents = [-1] * len(anchors)
    longest = 0
    for i, (r, q) in enumerate(anchors):
        low, high = 1, longest
        while low <= high:
            middle = (low + high) // 2
            # This uses tails[low], exactly as the historical source does.
            if anchors[tails[low]][0] + max_diff < r:
                break
            if anchors[tails[middle]][1] >= q:
                high = middle - 1
            else:
                low = middle + 1
        parents[i] = tails[low - 1]
        tails[low] = i
        longest = max(longest, low)
    chain = []
    current = tails[longest] if longest else -1
    while current != -1:
        chain.append(anchors[current])
        current = parents[current]
    return chain[::-1]

# Example: py_aligner_score
def window_score(chain, query_length, k):
    if len(chain) < 2 or query_length <= 0:
        return 0.0
    r, q = zip(*chain)
    if any(b <= a for a, b in zip(r, r[1:])):
        raise ValueError("Reference coordinates must strictly increase")
    if any(b <= a for a, b in zip(q, q[1:])):
        raise ValueError("Score a monotone local segment, not a restart")
    gaps_r = sum(max(b - a - k, 0) for a, b in zip(r, r[1:]))
    gaps_q = sum(max(b - a - k, 0) for a, b in zip(q, q[1:]))
    return (min(r[-1] - r[0], q[-1] - q[0])
            - max(gaps_r, gaps_q) / 2) / query_length

# Example: py_aligner_region
def candidate_region(chain, reference_length, query_length, k, padding=9):
    if not chain:
        raise ValueError("Need at least one anchor")
    r0, q0 = chain[0]
    r1, q1 = chain[-1]
    start = max(0, r0 - q0 - padding)
    # These are START coordinates; +k cancels between query and reference.
    end = min(reference_length, r1 + k + query_length - (q1 + k) + padding)
    return start, end

# Example: py_aligner_alignment
def align_substring(query, target):
    query, target = dna(query), dna(target)
    rows, cols = len(query), len(target)
    matrix = [[0] * (cols + 1) for _ in range(rows + 1)]
    for i in range(1, rows + 1):
        matrix[i][0] = i
        for j in range(1, cols + 1):
            matrix[i][j] = min(
                matrix[i - 1][j - 1] + (query[i - 1] != target[j - 1]),
                matrix[i - 1][j] + 1,
                matrix[i][j - 1] + 1,
            )
    end = min(range(cols + 1), key=matrix[rows].__getitem__)
    score = matrix[rows][end]
    i, j = rows, end
    query_line, target_line = [], []
    while i:
        if j and matrix[i][j] == matrix[i - 1][j - 1] + (query[i - 1] != target[j - 1]):
            query_line.append(query[i - 1]); target_line.append(target[j - 1])
            i -= 1; j -= 1
        elif matrix[i][j] == matrix[i - 1][j] + 1:
            query_line.append(query[i - 1]); target_line.append("-")
            i -= 1
        else:
            query_line.append("-"); target_line.append(target[j - 1])
            j -= 1
    return score, j, end, "".join(query_line[::-1]), "".join(target_line[::-1])

# Example: py_aligner_mapping
def toy_map(reference, query, k=7, w=4):
    chain = lis_chain(shared_anchors(reference, query, k, w))
    if len(chain) < 4:
        return None
    start, end = candidate_region(chain, len(reference), len(query), k)
    score, left, right, query_line, target_line = align_substring(query, reference[start:end])
    return score, start + left, start + right, query_line, target_line

# Example: py_aligner_example
rng = Random(41)
reference = "".join(rng.choices("ACGT", k=500))
query = reference[180:260]
query = query[:35] + query[36:]
result = toy_map(reference, query)
assert result is not None
assert result[:3] == (1, 180, 260)
assert result[3].replace("-", "") == query
assert result[4].replace("-", "") == reference[180:260]
print("edits, start, end:", result[:3])

# Example: py_aligner_bwt
def bwt_index(text):
    text = dna(text) + "$"
    suffix_array = sorted(range(len(text)), key=lambda i: text[i:])
    bwt = "".join(text[(i - 1) % len(text)] for i in suffix_array)
    alphabet = sorted(set(text))
    first = {base: sum(c < base for c in text) for base in alphabet}
    counts = {base: [0] for base in alphabet}
    for char in bwt:
        for base in alphabet:
            counts[base].append(counts[base][-1] + (base == char))
    return suffix_array, bwt, first, counts

def bwt_exact_positions(text, query):
    suffix_array, bwt, first, counts = bwt_index(text)
    low, high = 0, len(bwt)
    for base in reversed(dna(query)):
        if base not in first:
            return []
        low = first[base] + counts[base][low]
        high = first[base] + counts[base][high]
        if low == high:
            return []
    return sorted(suffix_array[low:high])

assert bwt_exact_positions("GATTACAGATTACA", "TACA") == [3, 10]
