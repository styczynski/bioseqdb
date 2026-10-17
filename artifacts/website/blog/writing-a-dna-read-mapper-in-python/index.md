---
title: Learning to map DNA reads by writing an aligner in Python
slug: writing-a-dna-read-mapper-in-python
description: Build a small read mapper step by step, from rolling k-mers and minimizers to chaining and local alignment, then examine the code and measurements from my early BioseqDB prototype.
date: '2026-10-12'
author: Piotr Styczyński
category: bioseqdb
keywords: [BioseqDB, Python, NumPy, sequence alignment, read mapping, minimizers, k-mers, chaining, longest increasing subsequence, dynamic programming, Burrows-Wheeler transform, minimap2, bioinformatics]
cover: /static/figures/blog/sequence-letters.svg
image_alt: Small translucent DNA letter tiles in the BioseqDB colours.
draft: false
---

Before the current BioseqDB, I wrote a read mapper in Python. It was an assignment for [Algorithms for Analysis of Genomic Data at MIMUW](https://informatorects.uw.edu.pl/pl/courses/view?prz_kod=1000-718ADG), and a useful excuse to stop treating an aligner as a black box. I wanted to understand how a short, imperfect sequence can find its place in a much longer one without comparing it with every possible position.

Python suited what I wanted to learn. A poor algorithm has fewer places to hide when every unnecessary iteration is expensive. A quadratic loop that looks harmless on a tiny sequence becomes a serious problem on millions of nucleotides, and I had a time limit to meet. Moving the same loop into a NumPy-looking expression did not always help either.

This tutorial follows that experiment, but starts with a smaller program you can run and understand. We will build seeds, turn them into matching coordinates, connect those coordinates and check a promising region with dynamic programming. Then I will explain the less tidy parts of the submitted program: distance cutoffs, endpoint searches, memory compromises and the measurements which made me keep or remove an optimization.

The [historical mapper.py](https://github.com/styczynski/bioseqdb/blob/ca3651cc63ebc58f2bb6b948ce4d3cab7d6e44ea/research/python_aligner/mapper.py) is 938 lines. This is the last version before the research directory was removed, and it is identical to the earlier version linked from my notes. The short implementation below is a teaching version, with a few mistakes in the old representation deliberately corrected. It does not reproduce the competition timings.

![The historical mapping pipeline goes from reference chunks and shared seeds through candidate regions to endpoint refinement and coordinates.](/assets/08f71d3f6028ef4c3cfdb19818e8f52f73661d43eff049cb1218516682c7e6eb/figures/blog/python-aligner/mapper-pipeline.svg)

## The task and its rather concrete limits

The assignment was to map reads of roughly 1,000 bases onto a reference of at most 20 million bases. Reads came from the reference, but contained substitutions and single-base insertions or deletions, with an error probability between 5% and 10%. The requested output was just a read identifier and its two reference coordinates, separated by tabs.

The rules matter here. This was a Python 3 task. NumPy, SciPy, Biopython and suffix-array construction code were allowed; an existing mapper or alignment library, multiprocessing, JIT compilation and calling a program written in another language were not. The program had to stay below 1 GB of memory. Its runtime allowance was `5 + r/10` minutes for `r` reads: 105 minutes for 1,000 reads, which I remembered as roughly two hours.

At least 80% of reads had to be mapped, with no more than 1% incorrect mappings. A reported endpoint could be at most 20 bases away from the true endpoint and still count as correct. That last condition is quite different from asking for a complete, base-by-base alignment with a CIGAR string.

I optimized the program around finding one useful region for a read. The assignment itself did not promise that the reference had no repeats, or that every read had only one possible placement. The old implementation also searches the forward strand only. Those are limitations of the experiment, not properties of genomic data.

## 1. What are we trying to align?

An alignment places two sequences in columns. A column can contain two equal bases, two different bases, or a base opposite a gap. Gaps let us explain insertions and deletions instead of shifting every remaining comparison after the first difference.

For example, the following query is missing one base from the reference fragment:

```text
reference fragment  A C G T A C G T
query               A C G - A C G T
```

Mapping asks where the read belongs. Alignment explains the differences at that location. The two problems are related, but the first can often be answered without computing every alignment column. BioseqDB can expose those columns through `bioseq.cigar`; this early mapper only returned coordinates.

![An illustrative alignment shows a substitution, an inserted base and a deleted base in separate columns.](/assets/08f71d3f6028ef4c3cfdb19818e8f52f73661d43eff049cb1218516682c7e6eb/figures/blog/python-aligner/alignment-edits.svg)

A dynamic-programming matrix for a query of length `m` and a reference of length `n` needs `O(mn)` work. For one 1,000-base read and a 20-million-base reference, that is about 20 billion cells before considering the other reads. The useful question is therefore: can we cheaply find a small region which deserves that work?

This is the idea behind [seed-and-extend](/v0.1/tutorials/project_alignment_foundations/#seed-and-extend). Find short exact matches, use their positions to propose a region, and spend the expensive work there.

## 2. Run the small version

The Python blocks below belong to one file and use only the standard library. Copy them in order, or [download the complete Python example](/assets/08f71d3f6028ef4c3cfdb19818e8f52f73661d43eff049cb1218516682c7e6eb/examples/python-aligner.py) and run:

```sh
python3 python-aligner.py
```

The website build extracts these blocks into that download, executes them and checks the algorithms against small independent examples. This makes the displayed program and the downloadable one the same source. It is an educational mapper for small inputs; use the historical file to study the original experiment.

We use zero-based, half-open coordinates: `[start, end)` includes `start` and excludes `end`. Seed positions below are **starts** of k-mers. The historical file used their **ends**, so mixing coordinates from the two programs would introduce an offset.

First, make the alphabet explicit. Rejecting an ambiguous base is acceptable for this little example; a general mapper needs a deliberate policy for it. The reverse-complement helper shows the other strand, although we will only map the forward strand here.

```python id=py_aligner_alphabet
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
```

## 3. Turn overlapping k-mers into integers

A k-mer is a run of `k` bases. `ACGTAC` has four 3-mers: `ACG`, `CGT`, `GTA` and `TAC`. Keeping all of them as newly allocated Python strings works, but it is unnecessary.

Use two bits per base: A is 0, C is 1, G is 2 and T is 3. Shift the previous value two bits left, append the new base, then mask away everything older than `k` bases. The mask has exactly `2*k` bits.

```python id=py_aligner_kmers
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
```

For `ACGTAC` with `k=3`, this gives `(6, 0), (27, 1), (44, 2), (49, 3)`: integer key followed by start position. Leading A bases are fine because the length is fixed. Python integers also have enough bits for a 17-mer; a 32-bit integer does not, because 17 bases need 34 bits.

![Overlapping three-base windows become two-bit integer keys; shifting and masking retains the latest three bases.](/assets/08f71d3f6028ef4c3cfdb19818e8f52f73661d43eff049cb1218516682c7e6eb/figures/blog/python-aligner/rolling-kmers.svg)

There is an awkward detail in my original code. It encoded C, A, T and G as 1, 2, 3 and 4, then used two-bit shifts. Four needs three bits, so different strings can produce the same key: `AG` and `TG` both become 12. The original mask also retained more bits than intended, and selected keys were eventually cast to `uint32`. The code could still find useful candidates, but that is not an exact two-bit encoding. The version above fixes those details rather than asking you to repeat them.

These integers preserve lexicographic order. They are not the mixed hash values used by a production minimizer sketch. That choice is useful for seeing what happens by hand, and it also biases which strings become minima.

## 4. Keep minimizers instead of every k-mer

The reference index does not need a separate entry for every overlapping k-mer. Take a window of `w` consecutive k-mers and select its smallest key. Move the window one position and repeat. If the same occurrence remains the minimum, store it once.

Here `k` is measured in bases and `w` in k-mers. A complete minimizer window covers `k+w-1` bases. With `k=3` and `w=2`, our four keys produce three windows. Their minima are 6, 27 and 44.

```python id=py_aligner_minimizers
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
```

The tuple comparison makes ties choose the leftmost position. On a run of A bases, moving the window removes the old leftmost minimum and selects the next one, so repeated sequence can still produce many seeds. Sparse sampling does not make repeats disappear.

This implementation scans each window directly and therefore uses `O(nw)` work for fixed-size keys, plus `O(n)` stored k-mers. It is intentionally short. A monotonic deque can maintain the window minimum with amortized constant work per incoming key; the [minimizer chapter](/v0.1/tutorials/project_minimizers/) goes further into the design choices.

![Three overlapping windows select three minima from the keys 6, 27, 44 and 49.](/assets/08f71d3f6028ef4c3cfdb19818e8f52f73661d43eff049cb1218516682c7e6eb/figures/blog/python-aligner/minimizer-windows.svg)

If two sequences contain the same unchanged window of bases, this deterministic rule chooses a shared seed from it. An error may disrupt several adjacent k-mers, so larger `k` makes exact seeds harder to preserve. Smaller `k` creates more accidental matches. In the final historical program I used `k=17` and `w=8`; the tiny examples here use smaller values so we can inspect them.

## 5. Join seed positions to get anchors

Build a dictionary from a seed key to all of its reference positions. Sketch the query with the same parameters. Every shared key gives pairs `(reference_position, query_position)`, which I will call anchors.

```python id=py_aligner_anchors
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
```

A key occurring three times in the reference and twice in the query creates six anchors. This cross product matters: even if the whole read has one useful placement, its short fragments can be common. The original program allocated these products with NumPy, and a frequent seed could make them large. Our example has no frequency cap either, so it is not suitable for a repeat-rich genome.

At the correct location, anchors tend to form a rising line. A substitution removes some matches; an insertion or deletion shifts the diagonal. Random matches make scattered points elsewhere. We want to connect points which look like parts of the same read.

## 6. Start with a longest increasing subsequence

After sorting anchors by reference position, look for a strictly increasing sequence of query positions. This is the **longest increasing subsequence**, or LIS. It maximizes the number of compatible anchors.

For each chain length, keep the smallest query coordinate which can end a chain of that length. A smaller endpoint leaves more room for later anchors. Binary search locates the length to extend; parent pointers let us recover the chosen chain after the scan.

```python id=py_aligner_lis
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
```

Sorting equal reference coordinates by descending query coordinate is deliberate. A strictly increasing query subsequence can then use at most one anchor at a particular reference coordinate. Sorting both coordinates ascending would allow an impossible chain through two points at the same reference position.

The core loop costs `O(a log a)` for `a` anchors. This is a considerable improvement over comparing every anchor with every earlier anchor, but the optimization target is still quite crude. LIS counts points. It does not know whether the gap between two points is ten bases or ten million.

![An anchor plot contrasts a long increasing path across distant regions with a compact local cluster.](/assets/08f71d3f6028ef4c3cfdb19818e8f52f73661d43eff049cb1218516682c7e6eb/figures/blog/python-aligner/anchors-and-locality.svg)

## 7. Stop distant anchors from taking over the chain

A global LIS can spend its length on a weak region before reaching the useful one. The slides from the project show why I started looking for several local candidates instead of trusting one globally increasing path.

In the submitted code, I modified the binary search itself. If a stored reference endpoint was too far behind the current anchor, the search stopped early. The cutoff was approximately `1.3 * query_length` reference bases.

```python id=py_aligner_cutoff
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
```

Notice that the distance test uses `tails[low]`, not `tails[middle]`. That is what the historical file does. Once this early break is added, the tail array no longer necessarily stores the smallest possible endpoint for every chain length. The parent chain still increases in query coordinates, but the heuristic can miss a longer chain. Treat it as a way to propose local windows, not as a new proof of an optimal LIS algorithm.

For example, ordinary LIS connects `(0, 0)` and `(100, 10)`. With a distance cutoff of 30, the modified version keeps only `(100, 10)`. It gives up the longer chain to avoid crossing that reference gap.

The intent is easier to read as a short outline:

```pseudocode
For each anchor in reference order
    search the stored chain endings
        If the old reference position is too far away
            stop searching and replace an earlier ending
        Else compare query coordinates as in LIS
    remember the predecessor
reconstruct the backbone
inspect local windows of that backbone
keep a few promising windows for refinement
```

This was useful for the assignment, but today I would separate candidate-region selection from the ordinary LIS data structure more clearly. A shortcut hidden inside binary search is difficult to reason about, especially when it changes the invariant which justified that search.

### Score covered distance, and charge for gaps

The next question is which local window deserves refinement. The historical score rewards the smaller of the reference and query spans and subtracts half of the larger uncovered-gap total. A gap between seed starts contributes `max(distance-k, 0)` uncovered bases.

```python id=py_aligner_score
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
```

For four anchors spaced ten bases apart on both axes, `k=17` and a 100-base query, the score is `0.3`. Stretch the reference gaps to thirty bases while keeping the query gaps at ten and it drops to `0.105`. This is a ranking heuristic, not percentage identity or a probability that the mapping is correct.

![Two four-anchor windows with the same query span receive different scores because the sparse window has uncovered reference gaps.](/assets/08f71d3f6028ef4c3cfdb19818e8f52f73661d43eff049cb1218516682c7e6eb/figures/blog/python-aligner/candidate-gap-score.svg)

The readable function recomputes one monotone window directly. The old code maintained rolling gap totals and searched for local maxima; its bookkeeping has edge cases I would not use as a teaching definition. Its candidate selection kept scores strictly above `0.1`, required more than three anchors in that branch, and retained at most five candidates. When no candidate survived, it tried ordinary LIS with a different local-window score. When several survived, dynamic programming helped choose between them.

## 8. Refine a small region with dynamic programming

A seed at reference position `r` and query position `q` suggests that the read starts near `r-q`. The first and last chain anchors therefore give rough bounds. Add some padding because errors near the ends can move those estimates.

```python id=py_aligner_region
def candidate_region(chain, reference_length, query_length, k, padding=9):
    if not chain:
        raise ValueError("Need at least one anchor")
    r0, q0 = chain[0]
    r1, q1 = chain[-1]
    start = max(0, r0 - q0 - padding)
    # These are START coordinates; +k cancels between query and reference.
    end = min(reference_length, r1 + k + query_length - (q1 + k) + padding)
    return start, end
```

We now align the full query against a substring of that small reference window. This is a semiglobal edit-distance problem: unmatched reference bases before or after the read are free, but every query base must be accounted for. A substitution, insertion or deletion costs one.

Initialize the first row to zero, allowing the alignment to begin anywhere in the target. Initialize the first column to the cost of deleting the query prefix. At the end, choose the cheapest cell in the last row, rather than insisting that the alignment reaches the final target base.

```python id=py_aligner_alignment
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
```

For `ACGTT` against `GGACGCTAA`, the result begins `(1, 2, 7)`: one edit, starting at reference position 2 and ending before position 7. The remaining return values show the aligned strings. When several alignments have the same cost, this implementation consistently chooses one; it does not enumerate all alternatives.

The matrix still costs `O(mL)` time and memory, where `L` is the candidate-window length. That is acceptable for a small demonstration. It would be wasteful to keep a Python matrix for every read against an entire genome, and even a bad chain can make this toy window much too large. Production implementations use additional limits and more compact alignment techniques.

## 9. Put the pieces together

Our small mapper follows one ordinary LIS chain, requires four anchors, extracts a candidate and aligns the whole query there. It deliberately leaves out the historical multi-candidate and BWT paths so the first working program is easy to follow.

```python id=py_aligner_mapping
def toy_map(reference, query, k=7, w=4):
    chain = lis_chain(shared_anchors(reference, query, k, w))
    if len(chain) < 4:
        return None
    start, end = candidate_region(chain, len(reference), len(query), k)
    score, left, right, query_line, target_line = align_substring(query, reference[start:end])
    return score, start + left, start + right, query_line, target_line
```

Use a deterministic synthetic reference and delete one base from a fragment. The expected coordinates are known because we chose them ourselves:

```python id=py_aligner_example
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
```

```text
edits, start, end: (1, 180, 260)
```

The reference interval spans 80 bases; the query contains 79. The gap in the returned alignment explains the difference. Change the example to a substitution or an insertion and inspect both the coordinates and the alignment strings, not just the score.

This program can also return `None` if it finds fewer than four anchors. It does not try the reverse complement, rank all repeated placements, assign mapping quality or reject a high edit score. In other words, we have built the core of a small mapper, not a replacement for minimap2. The independent build checks include short sequences, repeated minima, equal-coordinate anchors and edit-distance comparisons against exhaustive substring searches.

## 10. Why the historical mapper also used BWT

The assignment only needed approximate endpoints. Running a full alignment for every candidate was not always necessary, so the original program tried to locate short fragments from the beginning and end of the read using a Burrows–Wheeler-transform index over small target windows.

BWT backward search represents the current matches as an interval in suffix-array order. Extending the query one character to the left narrows that interval using character counts. Here is a tiny **exact** version to make that operation visible:

```python id=py_aligner_bwt
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
```

`bwt_exact_positions("GATTACAGATTACA", "TACA")` returns `[3, 10]`. The suffix sorting above materializes strings, so it is intentionally for small examples. The historical search added insertion, deletion and substitution branches and pruning, and could stop after a successful branch. It was not an exhaustive guarantee of the best approximate alignment.

The final mapper used fragments covering about 10% of each query end, with an error budget of about 10% of that fragment length. If endpoint estimates were poor, a dynamic-programming fallback searched a larger suffix fragment, about 40% of the query. It also checked the resulting interval length and could retry before omitting a read.

There was a surprising practical result here. On these small endpoint windows, sorting suffixes in a straightforward way could beat a more sophisticated suffix-array construction with more temporary objects. A better asymptotic bound does not remove setup costs. It also does not justify using this suffix sorter for the whole reference.

## Where minimap2 goes further

Minimap2 follows a seed–chain–align design, but its chaining is not plain LIS. It rewards new seed coverage and penalizes gaps and differences between reference and query distances. Backtracking identifies candidate chains, and base alignment can fill the gaps and extend their ends. It also handles alternative placements and strand information. The [minimap2 paper](https://pmc.ncbi.nlm.nih.gov/articles/PMC6137996/) describes this architecture.

Its [sketch implementation](https://github.com/lh3/minimap2/blob/v2.28/sketch.c) also differs from our little minimizer function: it tracks forward and reverse-complement encodings, records strand and endpoint information, mixes the selected encoding into a hash and has its own tie handling. Our leftmost lexicographic minimum is a teaching choice, not a byte-compatible reimplementation.

The [chaining implementation](https://github.com/lh3/minimap2/blob/v2.28/lchain.c) searches earlier anchors with limits on how much work it performs. This is the more general direction: reward useful coverage, penalize implausible gaps, and bound the search. My Python cutoff was a much rougher attempt to keep distant matches from dominating a candidate.

## NumPy was useful, but not everywhere

I used NumPy extensively in the prototype: compact base arrays, bulk comparisons, sorting, selecting window minima and joining seed positions. But some of the most interesting experiments were the ones where putting data into an array made the program slower.

The original rolling-key computation used `frompyfunc` around a Python lambda and accumulated into an object array. [NumPy documents that object-array result](https://numpy.org/doc/stable/reference/generated/numpy.frompyfunc.html). The expression looks like an array operation, but the callback still executes Python code. Converting the result with `astype`, stacking columns, sorting and converting back to lists add allocations and representation changes.

A sliding-window view is different: it shares memory with the source array rather than copying every window. The later reduction still has work to do. For a direct minimum across every window, that work grows with both the sequence length and the window width; [the NumPy documentation](https://numpy.org/doc/stable/reference/generated/numpy.lib.stride_tricks.sliding_window_view.html) explicitly discusses that cost.

The local experiment files contain comparisons of list-based and NumPy-based dynamic programming. They belong to an **earlier v10 configuration with k=16 and w=7**, not the final `mapper.py` with k=17 and w=8. Keep them separate from the course results below.

![Local dynamic-programming timings compare lists and NumPy for two datasets, with the small number of calls shown.](/assets/08f71d3f6028ef4c3cfdb19818e8f52f73661d43eff049cb1218516682c7e6eb/figures/blog/python-aligner/local-dp-implementations.svg)

Only four or five DP calls were measured in each of these runs. The NumPy variant spent about 2.2–2.3 times as long in that phase. That is evidence about these implementations and inputs, not a general rule that lists beat NumPy. A Python loop which indexes one array cell at a time can pay scalar-conversion and indexing costs without getting the benefit of a compiled bulk operation.

![Local endpoint-search measurements compare straightforward suffix sorting with list and NumPy variants of skew suffix-array construction.](/assets/08f71d3f6028ef4c3cfdb19818e8f52f73661d43eff049cb1218516682c7e6eb/figures/blog/python-aligner/local-bwt-implementations.svg)

The BWT comparison tells a similar, size-dependent story. These were small local windows, where a simple implementation had little setup to pay for. The experiment changes more than one representation, so the whole-program timing cannot tell us exactly how much time was caused by allocation, copying or the suffix-array algorithm itself.

![Recorded local phase totals show where time went in the earlier prototype.](/assets/08f71d3f6028ef4c3cfdb19818e8f52f73661d43eff049cb1218516682c7e6eb/figures/blog/python-aligner/local-runtime-phases.svg)

The useful habit I took from this was to measure the phase I was changing. If reference indexing dominates a run, a large relative improvement in a rarely used fallback may barely change the total. If the next stage immediately converts an array back into Python objects, I need to include that conversion in the comparison.

## The final course results

The course report was updated on 18 December 2024. The comparison below covers ten submissions I considered valid general-purpose implementations under the course rules. Each was tested on six datasets containing 1,000 reads of roughly 1,000 bases. The references were a 1 Mb yeast fragment, a 5 Mb bacterial fragment and a 20 Mb yeast fragment. Suffix `a` denotes about 5% read errors and `b` about 10%; 20% of those errors were indels. `PS` is my submission.

The published PDF contains plots rather than a numeric runtime table. The redrawn runtime and memory values below are **approximate readings of those plots**; the downloadable data records that limitation. The mapping counts are read from the corresponding count plots. These are course-server measurements, not a controlled comparison with modern production aligners.

![Course runtimes for ten eligible submissions across three reference sizes at approximately five percent read errors; PS is the fastest and is highlighted.](/assets/08f71d3f6028ef4c3cfdb19818e8f52f73661d43eff049cb1218516682c7e6eb/figures/blog/python-aligner/course-runtime-a.svg)

![Course runtimes for the same submissions and reference sizes at approximately ten percent read errors.](/assets/08f71d3f6028ef4c3cfdb19818e8f52f73661d43eff049cb1218516682c7e6eb/figures/blog/python-aligner/course-runtime-b.svg)

PS was the fastest eligible submission on all six tests. It was tens to hundreds of times faster than the other nine submissions, depending on the test. Its six runs took roughly 0.19–0.57 minutes each, or about 11–34 seconds for 1,000 reads.

The result I was particularly happy with was combining those runtimes with a mapping for every read. Across the six datasets, PS left **zero reads unmapped** and had **eight incorrect mappings out of 6,000**: 1, 3, 0, 0, 4 and 0 in the report's order. That is about 99.87% correctly mapped overall under the assignment's endpoint tolerance. It does not mean every base of those reads was perfectly aligned.

![PS maps all six thousand reads, with per-dataset incorrect counts of one, three, zero, zero, four and zero.](/assets/08f71d3f6028ef4c3cfdb19818e8f52f73661d43eff049cb1218516682c7e6eb/figures/blog/python-aligner/course-ps-quality.svg)

The memory chart also matters. PS approached the 1 GB limit on the 20 Mb reference, so this was not a design with unlimited room to grow. The reference was processed in roughly 800,000-base chunks while building the sketch, but the program still retained reference data. Chunking reduced temporary work; it did not turn the mapper into a constant-memory streaming algorithm.

![PS reported memory rises with reference size and approaches the assignment's one-gigabyte limit.](/assets/08f71d3f6028ef4c3cfdb19818e8f52f73661d43eff049cb1218516682c7e6eb/figures/blog/python-aligner/course-ps-memory.svg)

The old chunk boundaries had no overlap, so seeds crossing a boundary could be lost. The reference sketch also skipped selected singleton seed groups after a counter threshold was reached. Those were practical compromises in this implementation, and they should be visible when interpreting its measurements rather than hidden behind the final runtime.

The [measurement data and provenance](/assets/08f71d3f6028ef4c3cfdb19818e8f52f73661d43eff049cb1218516682c7e6eb/figures/blog/python-aligner/measurements.json) describe the chart inputs. The [course results report](https://moodle.mimuw.edu.pl/pluginfile.php/330449/mod_page/content/7/results_rmap1.pdf) may require a university login; I used my downloaded copy. Local profiling data and the slides are in [the project sources](https://github.com/styczynski/bioseqdb/tree/b6aff3a0d8637bf1f01f392a485e9c76ad9465e5/papers/src/content/aadg/project1).

## What I kept for BioseqDB

The exercise made the stages much less mysterious. Seeds reduce the search space. Chaining asks whether their positions tell a plausible story. Refinement checks the details which exact fragments cannot explain. Each stage needs a useful limit, because a repetitive seed or an overlong candidate can undo all the savings from the previous stage.

I also learned to be suspicious of optimizations which only look fast in the source. Python made that lesson fairly immediate: object arrays still contain objects, a suffix-array algorithm still has setup costs, and converting between representations is work even when it occupies one line.

The current BioseqDB is a separate implementation with a much broader job: storing sequences alongside relational data and making alignment available through SQL. If you want to follow that next step, start with the [alignment foundations](/v0.1/tutorials/project_alignment_foundations/), then the [chaining chapter](/v0.1/tutorials/project_chaining/), or run the [SQL walkthrough](/v0.1/tutorials/getting_started/). The Python mapper was an early way to learn the mechanics well enough to start asking those larger questions.
