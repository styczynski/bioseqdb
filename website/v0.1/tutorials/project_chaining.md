---
title: "Chaining seed anchors"
slug: project_chaining
description: "Chaining seed anchors"
order: 60
keywords: [BioseqDB, project]
draft: false
---

## What happens with minimizers

<a id="sec-chaining-purpose"></a>

When minimizers are generated and we take the intersection (by hash value) of the minimizer sets between the query and the reference, we get a set of seed hits which is encoded as anchors.

## Chaining geometries

<a id="sec-chaining-geometry"></a>

A seed hit is represented by an anchor $$a_i=(r_i,q_i,\sigma_i,\ell_i,g_i,\phi_i),$$ where $r_i$ and $q_i$ are inclusive seed-end coordinates on the reference and query, $\sigma_i$ is the strand, $\ell_i$ is the query span, $g_i$ identifies a query segment, and $\phi_i$ stores additional ordering flags.

Below you can see the physical representation of the anchor in memory in minimap2 and in our implementation. We could potentially use adaptive representation similar to minimizers, but anchors are fewer than minimizers, so the benefit would be limited for additional complexity of C++ templates.

```cpp
typedef struct {
    uint32_t n;                    // Size of cr array (number of matches in the reference sequence)
    uint32_t q_pos;                // Query position
    uint32_t q_span       : 31;    // Query minimizer span in symbols
    uint32_t flt          : 1;     // If the minimizer is filtered out (very frequently occuring in the reference sequence)
    uint32_t seg_id       : 31;    // Query segment identifier
    uint32_t is_tandem    : 1;     // Tandem repeat flag
    const uint64_t *cr;            // Multiple reference coordinates per anchor
  } mm_seed_t;
```

```cpp
struct anchor {
    std::uint64_t ref_position;    // Absolute reference coordinate
    std::uint64_t query_position;  // Absolute query coordinate
    std::uint32_t is_reverse;      // Reverse-strand flag encoded as zero or one
    std::uint8_t query_span;       // Query minimizer span in symbols
    std::uint8_t flags;            // Seed flags propagated by collection
    std::uint16_t segment_id;      // Query segment identifier
  };
```

Figure [chaining dotplot](../project_chaining/#fig-chaining-dotplot) shows viasual interpretation. Valid anchors that can be used for chaining lie below and to the left of the current anchor. A chain can move between nearby diagonals to represent an insertion or deletion, but large diagonal changes receive a penalty or are not considered at all.

<a id="fig-chaining-dotplot"></a>

![01 introduction diagram](/static/figures/project/chapters-06-chaining-01-introduction-tikz-01.svg)

*Chaining selects growing, approximately collinear path so we can get closer to the final result. Base-level alignment (later stage of the pipeline) will later resolve gaps between anchors.*

> [!NOTE]
> **Why we look for anchors that are colinear?**
>
> In the perfect scenario without gaps $r_i-q_i$ is constant. An increasing diagonal coordinate indicates that in reference we matched more symbols than in the query (meaning insertion/deletion has occured). Chaining needs to specify costs for abandoning the main diagonal too much (intuitevely we don’t want the case in which we introduce very large gap in the reference sequence just to match one or two symbols in the query, because it’s improbable that some very large fragment was replaced during evolutionary processes by just a few bases)

As we would like to have some well defined order of anchors, we sort them by strand and reference coordinate first, then by the additional metadata: <a id="eq-chaining-order"></a> $$\begin{split}
 K_r(a_i) &=
   (\sigma_i \ll 63)\mathbin{\vert}
   \bigl(r_i\mathbin{\&}(2^{63}-1)\bigr),\\
 K_q(a_i) &=
   (g_i\ll48)\mathbin{\vert}(\phi_i\ll40)
   \mathbin{\vert}(\ell_i\ll32)
   \mathbin{\vert}(q_i\bmod 2^{32}).
\end{split}$$

Comparison is lexicographic in $K_r$, $K_q$, and the full $q_i$. The hashing doesn’t need full 64 bits for query position i.e if query coordinates $q_i$ exceed $2^{32}$ we still have correct order as the leftmostpart is still the fragment number $g_i$ (fragments won’t be larger than $2^{32}$). Fragmentation of sequences is discussed later in the storage chapter.

The ordering is required, because the chaining wants to process anchors in sequential order (increasing query positions), while we build chains of anchors using reference positions.

## Intuitive interpretation

<a id="sec-chaining-interpretation"></a>

We need to first understand how intuitively human would align sequences. First we choose a random fragment of query and use text search in our editor to find that exact fragment. The longer fragment is better, because it’s less likely that we found it just because of random subsitutions ocurring. At first we won’t find it, but we repeatadly delete chacters from the fragment start or end until match is found. Each match corresponds to possible location in the target sequence. If we repat the process enough for other fragments of query we will establih the most probable locations of query in the target sequence (by seeing groups of matches). Throughout this chapter we will never gave any reason to treat query and target asymetrically as scoring functions don’t distinguish between both inputs. The distinction was rather coming from their role in the alignment flow (the larger set of sequences can be called the set of references or targets, while the smaller one could be called queries).

> [!NOTE]
> **Longest increasing sequence**
>
> If the reference coordinates are increasing, then naive approach would be finding LIS (longest increasing subsequence) of query coordinates. You can think of the chaining as "improving" this simplistic approach by changing query coordinates to more complex scoring function which can handle gaps and other complex cases.

The sorting mentioned in section [chaining geometry](../project_chaining/#sec-chaining-geometry) is required, because what we would like to intuitively do is check where the first reference fragment can be placed in query sequence and when we move to the next anchor (increasing reference positon), the position of query should increase too (logically if the middle of query matches somewhere, then we should see the end of it, not the beggining). This introduces some emergent assymetry: it will be very likely to see one anchor in query matched with multiple anchors in referece (query can occur multiple times in reference), however what happens if one reference fragment is matched to multiple query fragments? This situation means that is hard to distinguish which part of the query is best to match:

- $\text{Reference}\gets XYZVW, \text{Query}\gets YZV$ - it’s immediately obvious that for $V$ in the reference we found the end of query

- $\text{Reference}\gets XYZVW, \text{Query}\gets VXVXVXV$ - we don’t know what part (is this start or the end of query, or the middle part?) we should consider without looking at neighbours of $V$.

> [!WARNING]
> **Emergent assymetry**
>
> In the following chapters I will present new optimisations that are not discussed in the field of computational biology. The emergent assymetry will play important role when talking about the experimental results in separate chapter [chaining cpu experimental analysis](../project_chaining/#sec-chaining-cpu-experimental-analysis).

## Chaining components

We defined the anchors that are input to chaining pipeline stage, however chaining itself isn’t a monolith. It can be viewed as composition of several steps.

> [!NOTE]
> **Chaining pipeline**
>
> Although chaining can be decomposed into several steps that are dispatched to differrent functions, in BioseqDB this is an internal implementation detail and we don’t allow users to alter for example which backtracking backend is used (although we could expose parts of chaining as composable components in the future)

Forward chaining (which is the core component) produces the score array $f$ and predecessor array $p$. Second most important step is backtracking (as typically in case of DP algorithms which fill some state matrices) reads the sorted anchors, $f$, and $p$. The backtracking writes theflat output anchor array supplied by the caller, denoted by $A$ ($A$ can be viewed as type `anchor A[]`).

The chaining can be fully decomposed into the following steps:

1.  **Forward chaining** reads the sorted anchors and writes the temporary $f$ and $p$ arrays.

2.  **Backtracking** reads these inputs and writes the accepted chain records plus full anchor copies in $A$.

3.  **Region generation** converts each chain record into one alignment region while retaining $A$.

4.  **Region selection** filters the regions while retaining $A$.

5.  **Base-level alignment** reads the selected regions, their slices of $A$, the query and target sequences, and the alignment parameters. It fills the base-level fields in the regions supplying exact start/end coordinates, match qualities etc. <a id="list-chaining-components"></a>

BioseqDB represents every chain record with the following structure:

```cpp
struct chain {
    int32_t score        = 0;    // Accepted-chain score
    int32_t anchor_count = 0;    // Anchors in the chain
    int32_t anchor_start = 0;    // Start of the chain slice in A
  };
```

Where `anchor_start` is the index of the first anchor in the chain in the array $A$.

For a non-empty record $c$, its anchors occupy consecutive positions in forward order: $$A[c.\verb|anchor_start|],\ldots,
  A[c.\verb|anchor_start|+c.\verb|anchor_count|-1].$$

The array $A$ is constructed using $f$ and $p$ arrays. For every anchor $i$, the two stored values mean:

$f_i$  
the score of the best chain that the selected forward algorithm found with anchor $i$ at its end.

$p_i$  
the index of the anchor immediately before $i$ in that chain. The value is $-1$ when the best chain found for $i$ starts at $i$

For example, $$p_7=5,\qquad p_5=2,\qquad p_2=-1
  \quad\Longrightarrow\quad
  7\to5\to2\to-1.$$

## Chaining algorithms

<a id="sec-chaining-algorithm-families"></a>

Similar to the minimizer chapter here we will discuss different approaches to chaining. The algorithm itself is based on dynamic programming scoring, but the details differ.

### Shared pair score

<a id="sec-chaining-score-model"></a>

Let’s say we have two anchors with coordinates $q_j, q_i$ and $r_j, r_i$ on the query and reference. Let’s assume the minimizer span is always $k$. We now can say that after incorporating $(r_j, r_i)$ anchor we will cover $q_i-q_j$ symbols on the query and $r_i-r_j$ symbols on the reference, however those two jumps can be interpreted as taking some symbols from query and assuming they form replacement and the rest forms a gap. For example let’s say $q_i-q_j > r_i-r_j$. This means that we match more symbols in query than in reference, so indicates some deletion in the reference. So we can say $r_i-r_j$ symbols are replaced (or stay the same) while the leftover $(q_i-q_j)-(r_i-r_j)$ symbols form this deletion. This thinking is symmetrical so we just only need to use minimum and absolute value to make it more generic.

Now we introduced $g(j,i)$ (the advance shared by the reference and query) and $\delta(j,i)$ (the remaining advance in whichever sequence moves farther). As k-mers represent hashed fragment of length $k$ we can assume that in our match we have exactly $\min\{q_i-q_j, r_i-r_j, k\}$ new symbols that matched. Hence we now define our first primitive score as $\min\{q_i-q_j, r_i-r_j, k\} = \min\{g(j,i), k\}$.

This scoring has some problems with the most important one being that it doesn’t penalize the case when we have a large gap in the reference sequence just to match one or two symbols in the query. This is improbable that some very large fragment was replaced during evolutionary processes by just a few bases. For that reason we will use function $\left\lfloor\text{Penalty}(\delta, g)\right\rfloor$. The penalty function should satisfy the following properties:

- There’s no penalty for doing nothing at all: $\text{Penalty}(0, 0) = 0$

- For $g \leq k \land \delta = 0$ there’s no gap so there’s shouldn’t be any penalty: $\text{Penalty}(0, g) = 0$

- Penalty should be monotonically increasing with respect to $\delta$: $\text{Penalty}(\delta, g) \leq \text{Penalty}(\delta+1, g)$

- Penalty should be increasing slower than linearly with respect to $g$: $\text{Penalty}(\delta, g+1) < \text{Penalty}(\delta, g) + 1$. This guarantees that increasing $g$ without touching $\delta$ benefits overall score (we prefer replacement region over gap region)

- For small $g$ and $\delta$ our penalty function should prefer increase in $g$ rather than $\delta$. This is equivalent of saying that we want to stay close to the diagonal and reduce the gap size introduced: $\text{Penalty}(\delta+1, g) \geq \text{Penalty}(\delta, g+1)$

- For large $g$ and $\delta$ our penalty function should be asymptotically the same for both $\delta$ and $g$. This is equivalent of saying that if we encounter very large gaps, then it doesn’t matter if we increase the matching region between two fragments or increase the gap as both fragments are very dissimilar: $O_{\delta}(\text{Penalty}(\delta, g = \text{const})) = O_{g}(\text{Penalty}(\delta = \text{const}, g))$

> [!NOTE]
> **Symmetry**
>
> Using $g$ and $\delta$ as only parameters for penalty function guarantees that scoring is symmetric. Symmetry makes sure that gap costs are the same for both query and reference.

Our final score is then something of the following structure: $$\begin{gathered}
\text{Score}(j,i) = \min\{g(j,i), k\} - \left\lfloor\text{Penalty}(\delta(j,i), g(j,i))\right\rfloor \\
= \min\{g(j,i), k\} - \left\lfloor \alpha * g(j,i) + \beta * \delta(j,i) + \gamma * f(\delta(j,i)) \right\rfloor \\
0 \leq \alpha, \beta, \gamma \leq 1
\end{gathered}$$

<a id="fig-chaining-distance-examples"></a>

![01 introduction diagram](/static/figures/project/chapters-06-chaining-01-introduction-tikz-02.svg)

*Three anchor displacements for $k=5$. The blue brackets are the actual reference and query advances. The green diagonal step advances both coordinates by $g=\min(d_r,d_q)$. The orange horizontal step is the remaining $\delta=|d_r-d_q|$. It would be vertical when $d_q>d_r$. Panel (c) also shows that the reward stops at $k$, even though $g>k$.*

The minimap2 scoring definition $E$ looks exactly as what we have just specified: <a id="eq-chaining-distances"></a> $$\begin{aligned}
 d_q(j,i) &= q_i-q_j, & d_r(j,i) &= r_i-r_j,\\
 \delta(j,i) &= \lvert d_r(j,i)-d_q(j,i)\rvert,
 & g(j,i) &= \min\{d_r(j,i),d_q(j,i)\}.
\end{aligned}$$

<a id="eq-chaining-link-score"></a> $$E(j,i)=\min\{\ell_j,g(j,i)\}-P(j,i),$$ where <a id="eq-chaining-penalty"></a> $$P(j,i)=
\begin{cases}
0,
  & \delta(j,i)=0\ \land\ g(j,i)\leq\ell_j,\\[2pt]
\left\lfloor
  \lambda_g\delta(j,i)+\lambda_s g(j,i)
  +\dfrac{1}{2}\operatorname{mglog}_{2}\!\bigl(\delta(j,i)+1\bigr)
\right\rfloor,
  & \text{otherwise}.
\end{cases}$$ The function $\operatorname{mglog}_{2}$ is minimap2’s polynomial approximation of $\log_2$. So far in the abstract definition of scoring we didn’t mention diagonal, but nonnegative coefficient $\lambda_g$ can be viewed as diagonal-gap penalty (further away from diagonal we penalize more), where the nonnegative coefficient $\lambda_s$ is the penalty for distance. The value $\ell_j$ is the query span of predecessor $j$. (In our example above we used $k$ for all k-mers sizes, but as mentioned before minimap2 with specific settings can compress regions of same repeating symbols so we just replace $k$ with kmer span $\ell_j$. All the logic stays the same) [Minimap2 chaining link score · `comput_sc 113`](https://github.com/lh3/minimap2/blob/master/lchain.c#L113)

> [!NOTE]
> **Score units**
>
> In this definition we view scoring function as expressed in number of matched symbols (not some arbitrary large numbers), so the coefficients must be set accordingly so the values are scaled correctly. In the dynamic programming formulation we then can take maximum of $E$ value and $\ell_i$ to make sure that in the worst case scenario we assume that we at least matched the length of the minimizer. There are no formal limitations on this normalization, but it makes reasoning about the scores easier.

It’s worth noting that defined scoring is also symmetrical in terms of coordinates i.e the score doesn’t take $r_i, q_i$ positions as parameters so reversing coordinate systems doesn’t change the outcome. However, in dynamic programming we often want to use the locally close predecessor as the best one.

We see that scoring is not easy to work with as it depends on both anchors. If we used a linear gap scoring similar to what we discussed in alignment foundations chapter [alignment model](../project_alignment_foundations/#sec-alignment-model) we would get a score that is not dependent on both anchors. Note that the following is true due to trivial relationship between arithmetic average, distance and minimum: $$\begin{aligned}
  \phi(j, i) = \frac{d_q(j,i) + d_r(j,i)}{2} \\
  \delta(j,i) + 2*g(j,i) = \phi(j,i)
\end{aligned}$$ So we can simplify our scoring function to be only dependent on $\phi(j,i)$ (I replaced $min(\ell_j, g(j,i))$ with just $\ell_j$ and replaced penalty function with a simple linear function dependent on $\phi(j,i)$): $$\begin{aligned}
  E(j,i) = \ell_j - \lambda_{\phi} * \phi(j,i)
\end{aligned}$$ Now if we assume that every anchor has constant span (as most of them will have length $k$), then we can define: <a id="eq-global-score-definition-chaining-rmq"></a> $$\begin{aligned}
  f_j = \max_{i < j}\{f_i+E(j,i)\}, \\
  \text{GlobalScore}(i) &= f_i + \lambda_{\phi} * (q_i + r_i)
\end{aligned}$$ We can observe that: $$\begin{aligned}
  f_i+E(j,i) > f_k+E(k,i) \iff \\
  f_i + \ell_j - \lambda_{\phi} * \phi(j,i) > f_k + \ell_k - \lambda_{\phi} * \phi(k,i) \iff \\
  \text{GlobalScore}(i) + \ell_j > \text{GlobalScore}(k) + \ell_k \iff \\
  \text{GlobalScore}(i) > \text{GlobalScore}(k)
\end{aligned}$$ This means that we can compute $\text{GlobalScore}(i)$ independently for each anchor and use it to calculate global score. This is what was done in minimap2 as described in minigraph paper \[[7](../bibliography/#ref-minimap2_rmq_scoring_minigraph)\]. The problem is that the new linear gap scoring is very simplistic and does not exhibit some of the properties I discussed earlier.

<a id="fig-chaining-near-far-distance"></a>

![01 introduction diagram](/static/figures/project/chapters-06-chaining-01-introduction-tikz-03.svg)

*A close and a far predecessor for the same current anchor $a_i=(10,9)$: we can see that both candidates lie on the diagonal $r-q=1$, so both have $\delta=0$. The close link has $g=3$, but the far link has $g=8$.*

## B1: minimap2 dynamic programming

<a id="sec-chaining-b1"></a>

As our scoring model $E(i,j)$ depends only on two anchors we can define recursive equations for dynamic programming: <a id="eq-chaining-b1-recurrence"></a> $$f_i=\max\!\left(
   \ell_i,
   \max_{j\in\mathcal{C}_i}\{f_j+E(j,i)\}
 \right),
 \qquad
 p_i=\operatorname*{arg\,max}_{j\in\mathcal{C}_i}\{f_j+E(j,i)\}.$$ Where $\mathcal{C}_i$ is the set of valid predecessors (selected by being close to diagonal - this is form of specifying cutoff condition so we don’t need to check all possible predecessors all the time). Maximum function is used here to make sure that in worst case if our definition of $E(i,j)$ yield very low score we can say that we matched at least length of the minimizer span.

If no predecessor improves the base value of score $\ell_i$, then $p_i=-1$.

We will call iteration over $\mathcal{C}_i$ a scan. Minimap2 does this scan by visiting anchors in order from newest to oldest.

After sorting, minimap2 numbers the anchors $0,1,\ldots,n-1$. When the DP algorithm reaches anchor $i$, the scores $f_0,\ldots,f_{i-1}$ and predecessor indices $p_0,\ldots,p_{i-1}$ have already been calculated. So minimap2 starts a new chain at $i$ with $$f_i=\ell_i \qquad\text{and}\qquad p_i=-1,$$ Minimap2 tests earlier anchors in the order $j=i-1,i-2,\ldots$ (larger index is tested first). For every pair that passes the distance checks, it calculates $$c_j=f_j+E(j,i).$$ If $c_j>f_i$, anchor $j$ becomes the new predecessor: $f_i\gets c_j$ and $p_i\gets j$ (strict inequality is important here so we avoid using older anchors with the same score).

Trying every earlier anchor would make this dynamic program expensive. Minimap2 applies the following strategies to limit the work performed:

- `max_iter` limits how many previous anchors we scan: this is the number of highest-numbered array positions before $i$.

- `max_skip` is the limit on skip counter $s$. It starts at zero for every new $i$ and is increased by one on marked anchor that fails to improve the score and decreased by one on marked anchor that improves the score. If skip counter exceeds limit we finish scan loop early and do not scan other previous anchors.

- `m` is an extra candidate that is tested after the scan loop. It is the anchor with the highest score among all previously tested anchors (this is a measure to prevent skipping good chains due to too aggressive skip counter limit)

- `CanScore(j,i)` checks the anchor distances to make sure the anchor lies within the band (close to diagonal). It uses $D_x$ and $D_y$ for limits on reference and query distances respectively and $B$ for distance from diagonal.

Minimap2 contains separate rules for multi-segment and cDNA input. The key algorithm stays the same and differences are very subtle:

- There are differences in handling scoring case of $g_i=g_j$

- Multi-segment non-cDNA input adds special requirement $d_r\leq D_y$

- cDNA input does not force $D_y\geq B$

- There are differences in penatly factors for scoring function $E(j,i)$

In description below I assume we talk about default case of one-segment, non-cDNA input.

> [!NOTE]
> **Different kinds of input**
>
> Multi-segment input is in case of Minimap2 described as a situation in which single query maps to different fragments of the reference section. I do not support this use case in BioseqDB out of the box at this time. This situation is also known as split-read or supplementary alignment and normally occurs due to due to structural variations, translocations, or chimeric artifacts in the genome. cDNA in this case describes the same situation, but has slightly different scoring parameters. Even though that can be interesting, we don’t want to describe these modes here since those are very specialized heuristics

### Skipping with counters and marked anchors

As discussed above minimap2 scores only valid pairs of anchors: <a id="eq-chaining-b1-pair-checks"></a> $$0<d_r(j,i)\leq D_x,\qquad
  0<d_q(j,i)\leq\min(D_x,D_y),\qquad
  \delta(j,i)\leq B,$$ where $B=\verb|bw|$ where all parameters like $\verb|bw|$ are minimap2’s CLI arguments. $$D_x=\max(\verb|max_dist_x|,B),\qquad
  D_y=\max(\verb|max_dist_y|,B).$$

After a candidate $j$ passes this checks, we calculate $E(i,j)$ score and then update $p_j$ if we see better score. We want to that $p_j$ was updated in scan for $i$. This can be understood as building chains in parallel where we want to save information about predecessor for each anchor: $$t_{p_j}\gets i \qquad\text{when }p_j\geq0.$$ If some time later in other scan we see that $t_{p_j}=i$ then we know that $p_j$ was updated in scan for $i$ (we already tested candidate with a larger anchor). If marked anchor $x$ fails to improve $f_i$, then $s$ increases by one. A failed unmarked anchor leaves $s$ unchanged. An improvement subtracts one from a positive $s$. It does not necessarily reset the counter to zero. The loop stops only when $s>\verb|max_skip|$

> [!NOTE]
> **Interpretation**
>
> This marking with skip counter can be intuitevely understood as a situation in which we try to improve the best chain by adding new anchor. If repeatedly this chain fails to improve the score, we don’t want to waste more time on it.

We don’t perform this marking when this is candidate thatraises $s$ above `max_skip` (in that case we stop the scan loop early).

Minimap2 keeps the loop index in $e$. If the skip test stopped the loop while it was processing $j$, then $e=j$. $e$ is used to check if we can score anchor $m$ (if $m<e$ then we can).

$m$ is additional index saved by minimap2 on the side, which we call the *saved high-score anchor*. The saved value is an anchor index whose previously calculated $f_m$ is large. If $m$ is absent or has left the reference-distance window (is too far), minimap2 first rebuilds it by finding the largest $f_j$ in the current `max_iter` set.

This saved anchor can then remain saved after $m<i-\verb|max_iter|$. Each time we check $e<m$ (see if we already scored this anchor) and if not we calculate $E(m,i)$ and do update checks similar to other anchors. $m$ is introduced to make sure to take into consideration anchor that could be potentially a little bit further away. Minimap2 doesn’t use priority queue or list here, because chaining accuracy is already good enough and this approach is much simpler.

<a id="fig-chaining-b1-scan"></a>

![02 cpu diagram](/static/figures/project/chapters-06-chaining-02-cpu-tikz-01.svg)

*(a) The reference and query limits and diagonal band decide whether a pair can be scored or not. (b) If distance checks pass, then we perform scans in a loop. The picture shows one pass of the scan for the current anchor $i=8$. The numbers inside the circles are anchor indices. The orange arrow from $j$ to $p_j$ shows the predecessor previously saved for anchor $j$. After $j$ is scored without stopping, the arrow’s destination receives the current mark $i=8$. The numbered steps visit anchors $7,6,\ldots,1$. The small $s$ boxes show the single skip counter for current anchor 8. Step 2 doesn’t affect $s$ as this anchor doesn’t have a predecessor in this scan pass (it’s unmarked). Step 5 improves the score and changes $s=2$ to $s=1$. Anchor 0 is already outside the main loop because $\verb|max_iter|=7$, so we don’t scan it, but we check it later with $m=0$. Step 7 stops with $e=1$, so saved $m=0$ satisfies $m<e$ and is checked once. The score of $m=0$ equal to 17 wins, but in some other scan other anchor could have a better score.*

##### Minimap2’s ordered predecessor scan for one query segment

<a id="alg-chaining-b1"></a>

1.  Input: sorted anchors $a_0,\ldots,a_{n-1}$; ordinary non-cDNA mode; configured distance limits, $B=\verb|bw|$, `max_iter`, and `max_skip`

2.  Output: chain scores $f_i$ and predecessor indices $p_i$

3.  $D_x\gets\max(\verb|max_dist_x|,B)$

4.  $D_y\gets\max(\verb|max_dist_y|,B)$

5.  Function `CanScore`($j,i$)

6.  $d_q\gets q_i-q_j,\quad d_r\gets r_i-r_j$

7.  If $a_j,a_i$ use different references or strands

8.  return false

9.  End If

10. If $d_r\leq0$ **or** $d_r>D_x$

11. return false

12. End If

13. If $d_q\leq0$ **or** $d_q>D_x$ **or** $d_q>D_y$

14. return false

15. End If

16. If $\lvert d_r-d_q\rvert>B$

17. return false

18. End If

19. return true

20. End Function

21. $t_j\gets0$ for every $j$; $m\gets-1$; $st\gets0$ ($m$ is one saved anchor index)

22. For $i\gets0$ **to** $n-1$

23. While $st<i$ and ($a_{st}$ has a different reference or strand from $a_i$, or $r_i-r_{st}>D_x$)

24. $st\gets st+1$

25. End While

26. $st\gets\max(st,i-\verb|max_iter|)$ (hard lower bound for the main scan)

27. $f_i\gets\ell_i,\quad p_i\gets-1,\quad s\gets0,\quad
                e\gets st-1$

28. For $j\gets i-1$ **down to** $st$

29. If `CanScore`($j,i$)

30. $c\gets f_j+E(j,i)$

31. If $c>f_i$

32. $f_i\gets c,\quad p_i\gets j$

33. If $s>0$

34. $s\gets s-1$

35. End If

36. ElsIf $t_j=i$

37. $s\gets s+1$

38. If $s>\verb|max_skip|$

39. $e\gets j$

40. **break**

41. End If

42. End If

43. If $p_j\geq0$

44. $t_{p_j}\gets i$ (mark one older anchor for this scan)

45. End If

46. End If

47. End For

48. If $m<0$, or $a_m$ has a different reference or strand from $a_i$, or $r_i-r_m>D_x$

49. $m\gets-1$

50. For $j\gets i-1$ **down to** $st$

51. If $m<0$ **or** $f_j>f_m$

52. $m\gets j$

53. End If

54. End For

55. End If

56. If $m\geq0$ and $m<e$

57. If `CanScore`($m,i$)

58. $c\gets f_m+E(m,i)$

59. If $c>f_i$

60. $f_i\gets c,\quad p_i\gets m$

61. End If

62. End If

63. End If

64. If $m<0$, or ($a_m,a_i$ use the same reference and strand, $r_i-r_m\leq D_x$, and $f_i>f_m$)

65. $m\gets i$

66. End If

67. End For

> [!NOTE]
> **Algorithm details**
>
> We check if $a_{st}$ has a different reference or strand from $a_i$, because anchors in minimap2 can originate from different refences. In BioseqDB this won’t happen, but we still need to check for strand direction. Also please note that formally we use different values for marking $t_{p_j} \gets i$, but it’s main goal is just to detect visited anchors for the same scan pass (like in binary map, but assigning $i$ just means that we don’t have to clear it.)

## BioseqDB SIMD DP: exact and bounded batch

<a id="sec-chaining-simd-forward"></a>

For one target anchor $a_i$, scalar B1 tests possible predecessors one at a time. During the sacan it starts at $a_{i-1}$ and walks towards the lower end of the search window. SIMD implementations both implement the same outer loop, but the scan itself is rewritten: We place each predecessor candidate in batches of size $L$ (each batch is one SIMD vector) - one vector call can calculate as many as $L$ values of $E(j,i)$. We have two kinds of SIMD algorithm:

1.  **Exact SIMD B1 called `exact_visited`** It it the most naive implementation: it calculates up to $L$ pair scores together in a SIMD vector, but $F$, $P$, and $s$ are still single scalars as in original B1.

2.  **Bounded batch SIMD B1 called `bounded_batch_``approximate`.** Algorithm removes the mark array completely, while scalar skip counter in now counting batches. Lane $\lambda$ keeps its own temporary best score $H_\lambda$ and predecessor $P_\lambda$. A shared counter $u$ records how many batches in a row failed to improve any lane (the algorithm stops when $u$ reaches its batch limit $U$). We take the largest score across the lane winners and test the saved anchor $m$ once.

We will discuss bounded batch SIMD B1 first, as it is more interesting one.

### Candidates in SIMD vectors

The candidates are placed in batches. So now in loops we use $b=0$ instead of $j$ where $0$ is the first batch, $b=1$ is the next older batch, and so on. Within one batch, $\lambda\in\{0,\ldots,L-1\}$ is the SIMD lane number. The candidate placed in that lane is $j_{b,\lambda}$.

First we introduce the most boring thing - coordinates mapping: <a id="eq-chaining-simd-batch-map"></a> $$\begin{aligned}
  \mathrm{end}_b   &= i-bL,
  &\qquad \mathrm{begin}_b &= \max(st_i,\mathrm{end}_b-L),\\
  a_b              &= \mathrm{end}_b-\mathrm{begin}_b,
  &\qquad j_{b,\lambda} &= \mathrm{begin}_b+\lambda .
\end{aligned}$$ Here $\mathrm{end}_b$ is the position immediately after the batch, $\mathrm{begin}_b$ is its first candidate, and $a_b$ is its number of active lanes. As candidates are split into batches, the last one can be partial (so $a_b$ will be used to mask out the inactive lanes).

For lane $\lambda$, the pair scorer first calculates $\epsilon_{b,\lambda}=E(j_{b,\lambda},i)$. The code then loads the earlier chain score $f_{j_{b,\lambda}}$ and adds it to $\epsilon_{b,\lambda}$. The complete proposed chain score is $C_{b,\lambda}$, and $\mathbf C_b$ is the vector containing one such total per lane. A rejected candidate or an inactive lane receives $-\infty$.

Figure [chaining simd lane placement](../project_chaining/#fig-chaining-simd-lane-placement) depicts that now instead of single candidate being checked, we check batches:

- $C_{b,\lambda}$ holds total score for the candidates which corresponds to $c$ in original B1 algorithm

- $\mathbf j_b$ holds position of the candidate which corresponds to $j$ in original B1 algorithm

- $\mathbf H^{(b)}$ holds the best score for the batch which corresponds to $f_i$ in original B1 algorithm (note that now we hold vector instead of scalar, so at the end we need to select minimum value)

- $\mathbf P^{(b)}$ is shown later on the next figure and holds the index for $\mathbf H^{(b)}$

Note that only $\mathbf H^{(b)}$ and $\mathbf P^{(b)}$ are presisted between scans. The superscript $(-1)$ over $H$ means that we got this value from previous scan.

<a id="fig-chaining-simd-lane-placement"></a>

![02 cpu diagram](/static/figures/project/chapters-06-chaining-02-cpu-tikz-02.svg)

*How predecessor candidates are placed in four SIMD lanes. The lane number $\lambda$ selects the corresponding column in all three vectors. Blue boxes are candidate indices, orange boxes are calculated scores, and green boxes are best scores (the scores are improved vertically). When a lane improves its new maximum has an orange outline. Batch 3 is partial (we exhausted all candidates), so its two dashed lanes are inactive and their totals are $-\infty$.*

We operate on batches so we need to adjust our definition of skip counter. Let $S=\verb|max_chain_skip|$: <a id="eq-chaining-batch-limit"></a> $$U=\max\!\left\{1,\left\lfloor\frac{S}{L}\right\rfloor+1\right\}$$

As a helper we introduce $I_{b,\lambda}$ which is just binary mask checking lane-wise which $C$ value is better than $H$, so we update skip counter according to the following rules: <a id="eq-chaining-b5-counter"></a> $$u_b=
 \begin{cases}
   0,&\text{if any }I_{b,\lambda}\text{ is true},\\
   u_{b-1}+1,&\text{otherwise.}
 \end{cases}$$ When $u_b\geq U$ the algorithm stops. In original B1 we had guarantee that we will stop after $S$ skipped candidates, but here we stop after $U$ such that: $S<LU\leq S+L$. This doesn’t however bother us at all, as this is only very minor implementation detail user doesn’t need to see.

The figure below depcits two kinds of SIMD B1 algorithms. The first one (a) wasn’t discussed in detail as it only puts the candidates in a single SIMD vector to speed up only few operations like finding the maximum, where the rest of the code stays unchanged. The second parts of figure (b) is more interesting as it shows the approach we discussed with vector batching.

<a id="fig-chaining-b5-batches"></a>

![02 cpu diagram](/static/figures/project/chapters-06-chaining-02-cpu-tikz-03.svg)

*Here I compare the state updates for the two SIMD B1 algorithms: In (a), exact B1 first calculates four totals together, then the remaining code is scalar and we process each lane in a loop. The bounded algorithm is presented in (b). It’s significantly smarter as now each lane represents a separate chain and we keep its best $(H,P)$ values in separate vectors. The downward arrow shows how each lane keeps this pair while the scan moves through older batches. We start with $\mathbf H^{(-1)}=[8,8,8,8]$ and $\mathbf P^{(-1)}=[-1,-1,-1,-1]$. Each row shows both vectors after its batch. All four lanes change in batch 0. Batch 1 updates only lane 0 to the new best pair $(H_0,P_0)=(13,8)$. Batch 2 leaves both vectors unchanged. Now $u=1=U$, so I omit candidates 0 to 3 (drawn in the bottom gray area). Taking the horizontal maximum across the four lane scores selects $(H^{(2)}_1,P^{(2)}_1)=(16,13)$ from lane 1 (the green bracket and outlines). Note that we still score the saved candidate $m=2$, shown in the orange box, separately in one lane. Its score is $C_m=17$, so it now replaces our selected result with $(17,2)$. On the diagram, green boxes contain scores and blue boxes contain their predecessor indexes. I use orange borders in two places: around $C$ to mark a new improvement (maximum), and around $H$ and $P$ to show which stored pair was updated.*

After the last scored batch, the algorithm takes horizontal maximum of a vector once more: <a id="eq-chaining-b5-horizontal-reduction"></a> $$f_i=\max_\lambda H_\lambda,
 \qquad
 p_i=\max\{P_\lambda:H_\lambda=f_i\}.$$

The algorithm is described in detail in Algorithm [chaining b5](../project_chaining/#alg-chaining-b5).

##### Bounded SIMD B1

<a id="alg-chaining-b5"></a>

1.  Input: target $i$, window $[st_i,i)$, SIMD width $L$, $S=\verb|max_chain_skip|>0$, $\verb|max_chain_iter|>0$, saved index $m$

2.  Output: $f_i,p_i$

3.  $U\gets\max\{1,\lfloor S/L\rfloor+1\}$ (stop limit for entire vector)

4.  $\mathbf H\gets[\ell_i]^L,\quad
             \mathbf P\gets[-1]^L,\quad u\gets0$ (one temporary winner per lane)

5.  $o\gets st_i$ ($o$ bounds the interval skipped at limit $U$, initially $[st_i,o)$ is empty)

6.  For $b\gets0,1,\ldots$ while $\mathrm{end}_b>st_i$

7.  calculate $\mathrm{begin}_b,\mathrm{end}_b,a_b,\mathbf j_b$ using Equation [chaining simd batch map](../project_chaining/#eq-chaining-simd-batch-map)

8.  $\mathbf A\gets[\lambda<a_b]_{\lambda=0}^{L-1}$ (mask unused lanes in the oldest partial batch)

9.  $\boldsymbol\epsilon\gets
               \texttt{PairScoreBatch}(\mathbf j_b,i,\mathbf A)$ ($E(j,i)$, or $-\infty$ for a rejected pair)

10. $\mathbf C\gets
               \texttt{Select}(\boldsymbol\epsilon\neq-\infty,
                 \mathbf f[\mathbf j_b]+\boldsymbol\epsilon,-\infty)$ (total candidate scores)

11. $\mathbf I\gets(\mathbf C>\mathbf H)$ (strict lane-wise score improvements)

12. $\mathbf H\gets\texttt{Select}(\mathbf I,\mathbf C,\mathbf H)$

13. $\mathbf P\gets\texttt{Select}(\mathbf I,\mathbf j_b,\mathbf P)$ (older candidates with equal scores do not replace)

14. If $\texttt{Any}(\mathbf I)$

15. $u\gets0$

16. Else

17. $u\gets u+1$

18. End If

19. If $u\geq U$ and $\mathrm{begin}_b>st_i$

20. $o\gets \mathrm{begin}_b$ (batch loop omits $[st_i,o)$)

21. **break**

22. End If

23. End For

24. $f_i\gets\texttt{HorizontalMax}(\mathbf H)$

25. $\mathbf T\gets\texttt{Select}(\mathbf H=f_i,\mathbf P,-1)$

26. $p_i\gets\texttt{HorizontalMax}(\mathbf T)$ (If scores are equal, we choose the largest predecessor index)

27. If $f_i=\ell_i$

28. $p_i\gets-1$

29. End If

30. If $m<st_i$ or $m$ is outside the current reference and strand window

31. $m\gets\operatorname*{arg\,max}_{st_i\leq j<i}(f_j,j)$, or $-1$ if this interval is empty (We compare score first and then index)

32. End If

33. If $st_i\leq m<o$

34. $\epsilon_m\gets\texttt{PairScoreOneSIMDLane}(m,i)$ ($-\infty$ when the pair is rejected)

35. If $\epsilon_m\neq-\infty$

36. $c\gets f_m+\epsilon_m$

37. If $c>f_i$

38. $f_i\gets c,\quad p_i\gets m$

39. End If

40. End If

41. End If

42. If $m<0$ or $f_i\geq f_m$

43. $m\gets i$ (Update special anchor $m$, choosing the newer anchor on a tie)

44. End If

### Summary of forward chaining algorithms for CPU

<a id="sec-chaining-mm2-fast-forward"></a>

mm2-fast is another version of minimap2 where specific parts were accelerated. The chaining algorithm in BioseqDB was inspired by mm2-fast (but note that as in case of other scientific work, mm2-fast provided only AVX2/AVX512 kernel, not universal code like in case of BioseqDB). There are some slight differences in high-level algorithm design:

- mm2-fast doesn’t seem to have counter of anchors that does not improve the best score in the current batch (In BioseqDB bounded SIMD B1 we introduced such counter and in the pseudocode it’s called $U$)

- mm2-fast uses scalar code to check for the lane winner, while BioseqDB bounded SIMD B1 uses SIMD instructions

- mm2-fast fallbacks to scalar code for fewer anchors than lane count

*Summary of all forward chaining implementations for CPU*

<a id="tab-chaining-mm2-fast-forward"></a>

| **Method** | **How candidates are processed** | **When the scan stops** | **How the result is selected** |
|:---|:---|:---|:---|
| minimap2 B1 | Score one predecessor $j$ at a time. After each candidate, update the shared best pair and scalar skip/mark state | At the end of the chain, or when the scalar `max_skip` rule fires while $j$ decreases | The shared $(f_i,p_i)$ is already final |
| BioseqDB exact SIMD B1 | Score one predecessor per active lane. Inspect a batch in decreasing $j$ order when it may improve the best score or locate the exact `max_skip` stop | At the same candidate as scalar B1 | The shared $(f_i,p_i)$ is already final |
| mm2-fast | Score one predecessor per active lane. Each lane keeps its best pair across blocks | At the end of chain as there is no `max_skip` | Scalar code scans the lane winners |
| BioseqDB bounded batch SIMD B1 | Score one predecessor per active lane. Each lane keeps its best pair, and one counter tracks batches with no lane-local improvement | At the end of the chain, or after $U$ such consecutive batches | Choose the best lane result |

> [!WARNING]
> **Experimental justification**
>
> Note that although I was inspired by mm2-fast I still decided to apply the $U$ limit with a counter (for counting anchors that do not improve score). This is not justified for now. Experimental data supporting this decision is presented in a separate section

## Reverse chaining (backtracking)

The forward pass has already produced one score $f_i$ and one predecessor $p_i$ for every anchor. Backtracking follows the predecessor links and turns them into the chain records and flat anchor array $A$ defined at the start of this chapter.

### Formal definition for backtracking

Anchors have their score assigned: $f_i$. We would like to start by considering the highest scores as they are the best chains according to our score function. We sort the anchors in descending order of their scores: $f_{a_0} \geq f_{a_1} \geq \ldots \geq f_{a_{n-1}}$.

Now each anchor is the chain end so we need to find its start. We will traverse anchors according to $p_i$ i.e $i \rightarrow p_i \rightarrow p_{p_i} \rightarrow \ldots \rightarrow -1$. This traversal doesn’t have to go till the end of the chain. Usually there will be some part of the chain that is too low quality (not contributing to the score significantly).

We define function $\Delta_e(b)$ as the score assigned to the anchors followed from $e$ up to, but not including, $b$: <a id="eq-chaining-backtrack-contribution"></a> $$\Delta_e(b)=
  \begin{cases}
    f_e, & b=-1,\\
    f_e-f_b, & b\geq0.
  \end{cases}$$

The value of $\Delta_e(b)$ will be used to determine the best cut point for each anchor: $$\text{Cut}([\Delta_e(e), \Delta_e(p_e), \Delta_e(p_{p_e}), \ldots, \Delta_e(-1)]) \in [e, p_e, p_{p_e}, \ldots, -1]$$

The backtracking is then defined as: $$\text{Backtrack}(e) = \{c = (a_i, p_{a_i}, \ldots \text{Cut}(a_i)) | a_i \in [a_0, a_1, \ldots, a_{n-1}]\ \land f_{a_i} \geq F_{\text{min}} \land |c| \geq C_{\text{min}}\}$$ The backtracking function finds best cutoff point and filters anchors for minimal score $F_{\text{min}}$ and minimal chain length $C_{\text{min}}$.

### Minimap2 backtracking

Minimap2 uses two types of markings. The array $t$ (similar to forward chaining), records which anchors have already been marked:

1.  0 means unmarked: we didn’t visited this anchor yet

2.  1 means permanently marked: we won’t consider this anchor as end of the chain, because it’s already part of some found best chain. However anchors marked as 1 still can be part of the chain. This marking prevents a situation in which we would find chain, then it’s exact subchain etc.

3.  2 means temporarily marked while one path is tested: this marking is used for $\text{Cut}$ function and cleared after the path is tested. It purpose was to detect cycles (see cycle note below)

> [!NOTE]
> **$t=2$**
>
> Minimap2’s implementation could remove usage of $t=2$ markings, because under usual conditions there will be never a cycle of chains (anchor will always point to other anchor will lower position due to how forward chaining works). In listings below I provide the pseudocode corresponding to the actual current implementation, not some optimized one.

Minimap2 cut function marks visited anchors with $t=2$ and stops on any anchor with non-zero $t$ mark. We remember the best cut with the largest value of $\Delta_e$. If a later cut is worse than the best cut by more than `max_drop`, the search stops and returns the best cut seen so far. After everything we clean the $t=2$ temporary markings.

Figure [chaining minimap2 backtracking](../project_chaining/#fig-chaining-minimap2-backtracking) shows three interations of the main loop: we detect cuts for three anchors $a_{11}, a_{10}, a_9$. The example uses $\verb|min_sc|=40$ (this is minimap’s minimum score $F_{\text{min}}$), $\verb|min_cnt|=2$ (this is minimap’s minimum chain length $C_{\text{min}}$), and $\verb|max_drop|=5$ (maimum allowed score drop between best cut and current anchor in the chain). A $\Delta$ value inside anchor $a_b$ is the score that would result if $a_b$ was the selected cut. Orange boxes are anchors where either cut happens or we stop the search for the cut. Additionally I added "marked" label to $a_5$ to depict the fact that $a_5$ was marked by first $e=11$ chain, so it cannot be end of the chain (it’s still selected for $e=10$, because it’s start of the chain) Green boxes are anchors that are copied into the resulting array (everything excluding the cut anchor itself).

The minimap2 backtracking uses the same technique as BioseqDB Vulkan minimizer seeding: in the first pass we only count the number of copied anchors, while in the second pass we store them in the resulting array. This $\mathrm{phase}\in\langle\mathrm{count},\mathrm{write}\rangle$ iteration is depicted in (c) panel. The last step of minimap2’s backtracking depicted in panel (d) is reversing the chains (so the start is on the left rather than on the right side) and sorting by reference coordinates of the chain’s starts, so the chains are in natural left-to-right order.

<a id="fig-chaining-minimap2-backtracking"></a>

![03 backtracking diagram](/static/figures/project/chapters-06-chaining-03-backtracking-tikz-01.svg)

*Minimap2 scalar backtracking on three anchors. Panel (b) shows that the anchor that triggers `max_drop` doesn’t have to be the cut that is returned. Panel (c) shows the separate count and write passes. Panel (d) shows the final forward anchor order.*

Algorithm [chaining minimap2 boundary](../project_chaining/#alg-chaining-minimap2-boundary) shows the code for the cut search loop.

##### Minimap2 cut selection function for anchor $e$

<a id="alg-chaining-minimap2-boundary"></a>

1.  Input: unmarked end $e$, scores $f$, predecessors $p$, marks $t$

2.  Input: drop limit $D=\verb|max_drop|$

3.  Output: cut $b$, which is excluded from the new chain

4.  Function `ChooseCut`($e,f,p,t,D$)

5.  $i\gets e,\quad b\gets e,\quad \delta_{\mathrm{best}}\gets0$

6.  Repeat

7.  $t_i\gets2$ (temporary mark)

8.  $i\gets p_i$

9.  If $i<0$

10. $\delta\gets f_e$

11. Else

12. $\delta\gets f_e-f_i$

13. End If

14. If $\delta>\delta_{\mathrm{best}}$

15. $\delta_{\mathrm{best}}\gets\delta,\quad b\gets i$

16. ElsIf $\delta_{\mathrm{best}}-\delta>D$

17. **break**

18. End If

19. Until $i<0\ \lor\ (i\geq0\ \land\ t_i\neq0)$

20. $h\gets i,\quad j\gets e$ ($h$ is where this search stopped)

21. While $j\geq0$ and $j\neq h$

22. $t_j\gets0,\quad j\gets p_j$ (clear only temporary marks)

23. End While

24. return $b$

25. End Function

Algorithm [chaining minimap2 backtrack](../project_chaining/#alg-chaining-minimap2-backtrack) shows also how two passes for count/write are integrated. Minimap2 uses first pass to count all anchors i.e $N_C$ (this value is used to allocate array $U$). Array $U$ holds anchor count for a chain in lower 32 bits and score in higher 32 bits (this packing is defined by $\text{Pack}$ function). $U$ is then chain-reversed into $B_\gamma$ array, which undergoes sorting by reference coordinates $\text{ReferencePos}(B_\gamma[0])$ The final array $A$ contains all anchors, while output array $U$ contains packed `uint_64t` metadata for chain (count and score).

##### Minimap2 scalar backtracking and final chain order

<a id="alg-chaining-minimap2-backtrack"></a>

1.  Input: input anchors $a_0,\ldots,a_{n-1}$, scores $f$, predecessors $p$

2.  Input: limits $S=\verb|min_sc|$, $K=\verb|min_cnt|$, and $D=\verb|max_drop|$

3.  Output: packed minimap2 records $U$ and anchor array $A$ with each chain in forward order and chains sorted by reference start

4.  define $\texttt{Pack}(s,c)=(\operatorname{uint64}(s)\ll32)\mathbin{\vert}c$

5.  $Z\gets[(f_i,i)\mid 0\leq i<n\text{ and }f_i\geq S]$

6.  sort $Z$ by increasing score only (read it backward below)

7.  use temporary array $V[0\ldots n)$ (reuse an existing $n$-entry array)

8.  For $\mathrm{phase}\in
            \langle\mathrm{count},\mathrm{write}\rangle$

9.  $t[0\ldots n)\gets0,\quad N_C\gets0,\quad N_V\gets0$ ($t$ marks anchors, $N_C$ counts chains, $N_V$ counts retained indices)

10. For $k\gets |Z|-1$ **down to** $0$

11. $e\gets Z_k.\mathrm{index}$

12. If $t_e=0$

13. $b\gets\texttt{ChooseCut}(e,f,p,t,D)$

14. $N_V^{\mathrm{start}}\gets N_V,\quad i\gets e$ (cursor before tracing this end)

15. While $i\neq b$

16. If $\mathrm{phase}=\mathrm{write}$

17. $V[N_V]\gets i$

18. End If

19. $N_V\gets N_V+1,\quad t_i\gets1,\quad i\gets p_i$

20. End While

21. $c\gets N_V-N_V^{\mathrm{start}}$

22. If $b<0$

23. $s\gets f_e$

24. Else

25. $s\gets f_e-f_b$

26. End If

27. If $s\geq S$ and $c>0$ and $c\geq K$

28. If $\mathrm{phase}=\mathrm{write}$

29. $U[N_C]\gets\texttt{Pack}(s,c)$

30. End If

31. $N_C\gets N_C+1$

32. Else

33. $N_V\gets N_V^{\mathrm{start}}$ (discard indices but keep the marks)

34. End If

35. End If

36. End For

37. If $\mathrm{phase}=\mathrm{count}$

38. allocate $U[0\ldots N_C)$

39. End If

40. End For

41. $o\gets0$

42. For $\gamma\gets0$ **to** $N_C-1$

43. $c_\gamma\gets\operatorname{uint32}(U[\gamma])$ (low 32 bits store the anchor count)

44. $B_\gamma\gets
              [a_{V[o+c_\gamma-1]},\ldots,a_{V[o]}]$ (reverse one chain)

45. $o\gets o+c_\gamma$

46. End For

47. $\pi\gets
            \operatorname*{argsort}_{0\leq\gamma<N_C}\text{ReferencePos}(B_\gamma[0])$ (increasing $\text{ReferencePos}$)

48. $U_{\mathrm{old}}\gets U,\quad U\gets[\ ],\quad A\gets[\ ]$

49. For $m\gets0$ **to** $N_C-1$

50. $\gamma\gets\pi_m$

51. append $U_{\mathrm{old}}[\gamma]$ to $U$

52. append every anchor in $B_\gamma$ to $A$

53. End For

54. return $(U,A)$

> [!NOTE]
> **Start positions**
>
> Minimap2 stores only the score and anchor count, but it does not store an anchor start. The start of a chain is the sum of the counts of all earlier entries in $U$ and later code uses that sum. I were initially very suprised and confused by summing coordinates, but this is the explanation.

### BioseqDB SIMD backtracking

Exact SIMD B1 and bounded SIMD B1 call the same SIMD backtracking function. Its main differences from minimap2 are the following.

1.  Minimap2 follows and marks one end at a time. BioseqDB handles the end with largest $(f_e,e)$ alone, then collects up to $L$ later paths together.

2.  Minimap2 can cut a path with `max_drop`. BioseqDB follows a path until $-1$ or an anchor that is already marked.

3.  Minimap2 repeats the complete backtracking pass so it can count chains before allocating $U$. BioseqDB runs one pass and appends accepted chains to output arrays that grow as needed.

4.  Minimap2 does not specify an anchor order for equal scores. BioseqDB tries the higher end position first on a score tie.

5.  Minimap2 finally sorts complete chains by $\text{ReferencePos}$ of their first anchor. BioseqDB leaves accepted chains in the order in which it processes their ends.

In Bioseq chaining we consider 3 different operations:

1.  Single-path: This is a special case for consequitive anchor indices. If anchor has a chain built from consecutive anchor indices, then we can follow only one path at the time (SIMD vector contains only fragment of one chain) and mark anchors in bulk.

2.  Multi-path: This is expected case of SIMD-ified algorithm. In SIMD vector we store multiple anchors and follow each of the chains in parallel.

3.  Multi-path marking: This is the step that we do after multi-path - we need to mark all anchors and we do that by transposing the matrix created in multi-path step, where now each of $L$ lanes contains consecutive indices for one chain. (In reality we don’t transpose any matrix, it’s here only as illustration as in the code we put chains directly into final path rows)

In BioseqDB we only have binary marks: $$t_i=0\quad\text{means unclaimed},\qquad
  t_i=1\quad\text{means claimed}.$$ There is no temporary value $2$, which was obsolete as mentioned earlier. A mark is never cleared, including when the marked prefix later fails the score or count test.

If $L$ consecutive links satisfy $p_j=j-1$, then we run single-path algorithm version, where one vector comparison checks the $L$ links. Formally this condition is specified as: $$\operatorname{Run}_L(j,c)\iff
  j\geq L-1\ \land\ c\leq n-L\ \land\
  \bigwedge_{u=0}^{L-1}p_{j-u}=j-u-1.$$

> [!NOTE]
> **Chain order heuristic**
>
> At first it might not be intuitive why we are interested in chains of consecutive anchors. We can think of inverse of that condition. If the anchors are not consecutive, then it means that for some anchor there exists alternative very good match i.e for one target position there are multiple competing chainings for different query positions that fight over few sibling anchors. This means that fragment in query is not easily recognisable as query is repetitive and each repetition is equally good (very low divergence rates). For now we will just assume that this condition $\operatorname{Run}_L(j,c)$ is non-trivial algorithm specialisation that is discussed in detail in separate chapter.

If the $\operatorname{Run}_L(j,c)$ condition is met then we use only two store operations to mark $L$ anchors of that single chain (we pay the cost of checking the condition, but the we benefit by using less instructions). If not "multi-path" version follows one chain per lane. This concept is illustrated in Figure [chaining simd backtracking](../project_chaining/#fig-chaining-simd-backtracking) section (b). At first each lane contains consecutive indices which are highest-scoring anchors used as ends. Then we iterate trhough all of them until we reach the stop condition. Each lane $\lambda$ follows one chain and can stop earlier than others. We don’t want to explicitly hold this structure as matrix and transpose it. Instead for iteration (depth) $d$ we directly store (scatter)lanes: $$V_{\lambda,d}\quad\overrightarrow{\text{memory address}}\quad \text{Address}(V)+(\lambda n+d)*\text{sizeof}(V_{0,0}).$$ where $n$ constant is the arbitrary number of SIMD vectors we want to store ($d < n$)

Figure [chaining simd backtracking](../project_chaining/#fig-chaining-simd-backtracking) illustrates the algorithm with number of lanes: $L=4$, minimum score $F_{\text{min}}$: $S=30$, and minimum chain length $C_{\text{min}}$: $K=2$. The relevant scores for the example are: $$f_{19}=100,\quad f_{18}=90,\quad f_{17}=85,\quad f_{16}=80,\quad
  f_{15}=75,\quad f_9=40,\quad f_{11}=55,\quad f_{10}=50.$$

- First we start with $f_{19}$ as this is the highest-scoring anchor. We look for $p[19]$ and load $[p[19], p[18], p[17], p[16]] = [9, 8, 7, 6]$ into SIMD vector. This is compared to vector ${[p[19], p[19], p[19], p[19]] - [0, 1, 2, 3]}$, which in this case are equal. This means (toegether with bound checks) that $\operatorname{Run}_L(19,0)$ is true. Hence we run the single-path algorithm. I specifically split the second vector in the comparison to illustrate how SIMD vectors are constructed and compared in the real code. Then the single-path (a) will use store operation to mark $L$ anchors as visited and reverse the indices and store them in the result vector.

- For first path (a) when we encounter $1$ (the end of the array of anchors) we fallback to multi-path behaviour (this is the simplest way to avoid out-of-bounds access. Otherwise we would need to implement complicated masking). This is observed in (a) as $1$ and $0$ being in separate fragments rather than single one.

- The multi-path is illustrated in (b). The next highest scoring anchor after $f_{19}$ is $f_{18}$ with score $90$. We look for $p[18]$ and load $[p[18], p[17], p[16], p[15]] = [14, 13, 12, 10]$ into SIMD vector. The comparison in this case will yield false as $p[15] \neq 11$. This means that we follow each index one by one. Each time we load markings for the index and check what lanes were already marked (this forms a SIMD binary mask). If all lanes are masked-out then we can stop iteration completely. At each iteration we store lanes values in correct indices in our $V$ array.

- After multi-path (b) runs we still needs to filter the chains by score and length.

<a id="fig-chaining-simd-backtracking"></a>

![03 backtracking diagram](/static/figures/project/chapters-06-chaining-03-backtracking-tikz-02.svg)

*BioseqDB SIMD backtracking with four $L$ lanes. Panel (a) shows the special handling of the first path. If we detect $Run_L$ is true, then we use entire SIMD vector just for this one chain (SIMD vector contains anchors for $L=4$ different chains). Panel (b) shows the other case when $Run_L$ if false. The SIMD vector changes with each iteration $d=0,1,2,3$ and it’s used to store (scatter) anchors indices into the buffer (on the right size). Panel (c) reads one row at a time from the buffer starting at highest-scoring anchors first (now the SIMD vector as in case of Panel (a) contains one chain), we check for marked nodes via $\text{FirstMarked}$ function. If there are no marked anchors detected then we iterate this processing multiple times going through the chain. After reaching either end or marked node, we save the chain if it’s score is greater than required threshold and it’s length is sufficient.*

The accepted output in this example is $$\begin{aligned}
 C={}&[(100,11,0),(50,3,11),(30,2,14),(40,3,16)],\\
 A={}&[a_0,\ldots,a_9,a_{19}\mid a_{11},a_{14},a_{18}
       \mid a_{13},a_{17}\mid a_{10},a_{12},a_{16}].
\end{aligned}$$ Each record in $C$ is $(\text{score},\text{count},\text{start})$. For $a_{15}$ (in the Panel (c)) we mark it before testing its thresholds, but then the next anchor has too low score and is rejected. Because the chain length is just $1$ which is lower than minimum required length, we don’t copy that chain into $A$.

In the listing below, I present the code for this backtracking algorithm.

In the pseudocode we use $Run_L$ to set $g$ which is either $L$ or $1$ depending if we choose multi-path or single-path iteration. Additionally, we didn’t define $FirstMarked$ function in the pseudocode as it’s just helper to determine left-most marked anchor in the row (limited to only $g$ entries): $$\operatorname{FirstMarked}(t,V_{\lambda},o,g)=
  \min\bigl(\{u\mid 0\leq u<g\land
    t_{V_{\lambda,o+u}}=1\}\cup\{g\}\bigr).$$ It returns $g$ when none of the $g$ entries is marked. On the pseudocode listing for row $\lambda$ of $V$ denoted as $V_\lambda$:

- $\ell_\lambda$ is the number of stored entries in that row

- $b_\lambda$ is the row’s stop anchor

- $\alpha_\lambda$ says whether its collection lane is active (this is SIMD mask to determine if we need to terminate the chain if we either reach $-1$ or already marked anchor)

##### BioseqDB SIMD path collection and backtracking

<a id="alg-chaining-simd-backtrack"></a>

1.  Input: anchors $a[0\ldots n)$, scores $f$, predecessors $p$, thresholds $S,K$, width $L$, and empty outputs $C,A$

2.  Function `ChooseCutsSIMD`($\mathbf e,p,t,V,n$)

3.  $q\gets|\mathbf e|,\quad \mathbf j\gets\mathbf e,\quad
          \boldsymbol\ell\gets\mathbf0,\quad \mathbf b\gets-\mathbf1,\quad
          \boldsymbol\alpha\gets\mathbf1$

4.  For $d\gets0,\ldots,n-1$ while (Note that all things detnoted by $\lambda$ are evaluated for $0\leq\lambda<q$ in one SIMD vector) $\sum_{\lambda=0}^{q-1}\alpha_\lambda\neq0$

5.  $h_\lambda\gets
            [\alpha_\lambda=1\land t_{j_\lambda}=1],\quad
            \eta_\lambda\gets\alpha_\lambda\land\neg h_\lambda$ (check if we reached marked anchor in the lane)

6.  $b_\lambda\gets j_\lambda$ if $h_\lambda=1$, and $(V_{\lambda,d},\ell_\lambda)\gets
            (j_\lambda,\ell_\lambda+1)$ if $\eta_\lambda=1$ (scatter)

7.  $j_\lambda\gets
            \begin{cases}p_{j_\lambda},&\eta_\lambda=1\\-1,&\text{otherwise}
            \end{cases},\quad \alpha_\lambda\gets[j_\lambda\geq0]$

8.  End For

9.  **assert** $\sum_{\lambda=0}^{q-1}\alpha_\lambda = 0$ (All of the lanes should terminate chains)

10. return $(\boldsymbol\ell,\mathbf b)$

11. End Function

12. Function `BacktrackSIMD`($a,f,p,S,K,L,C,A$)

13. allocate $V\in\operatorname{int32}^{L\times n}$, and set $t[0\ldots n)\gets0$ ($0$: unclaimed, $1$: claimed)

14. $Z\gets\operatorname{sort}_{\uparrow}
          [(f_i,i)\mid0\leq i<n\land f_i\geq S],\quad k\gets|Z|-1$

15. If $k\geq0$

16. $e\gets Z_k.\mathrm{index},\quad j\gets e,\quad c\gets0$

17. While $j\geq0\land c<n$

18. $g\gets
              \begin{cases}L,&\operatorname{Run}_L(j,c)\\
              1,&\text{otherwise}\end{cases}$

19. $(V_{0,c+u},t_{j-u})\gets(j-u,1)$ for $0\leq u<g$, then $(j,c)\gets(p_{j-g+1},c+g)$

20. End While

21. **assert** $j < 0$

22. **if** $f_e\geq S\land c\geq K$ **then append** $(f_e,c,|A|)$ to $C$ and $(a_{V_{0,c-1}},\ldots,a_{V_{0,0}})$ to $A$

23. $k\gets k-1$

24. End If

25. While $k\geq0$

26. $q\gets\min(L,k+1),\quad
            \mathbf e\gets(e_0,\ldots,e_{q-1}),\quad
            e_\lambda\gets Z_{k-\lambda}.\mathrm{index}$

27. **if** $(S>0\lor K>0)\land
            \bigwedge_{\lambda=0}^{q-1}t_{e_\lambda}=1$ **then** $k\gets k-q$ and **continue**

28. $(\boldsymbol\ell,\mathbf b)\gets
            \texttt{ChooseCutsSIMD}(\mathbf e,p,t,V,n)$

29. For $\lambda\gets0$ **to** $q-1$

30. $c\gets0,\quad o\gets0$ ($c$ holds length of chain and $o$ is the offset in the $A$ where we store it)

31. While $o<\ell_\lambda$

32. $g\gets\min(L,\ell_\lambda-o),\quad
                r\gets\operatorname{FirstMarked}(t,V_\lambda,o,g)$

33. $t_{V_{\lambda,o+u}}\gets1$ for $0\leq u<r$, and $c\gets c+r$

34. **if** $r<g$ **then** $b_\lambda\gets V_{\lambda,o+r}$ and **break**, **otherwise** $o\gets o+g$

35. End While

36. $s\gets
              \begin{cases}f_{e_\lambda},&b_\lambda<0\\
              f_{e_\lambda}-f_{b_\lambda},&b_\lambda\geq0\end{cases}$

37. **if** $s\geq S\land c\geq K$ **then append** $(s,c,|A|)$ to $C$ and $(a_{V_{\lambda,c-1}},\ldots,a_{V_{\lambda,0}})$ to $A$

38. End For

39. $k\gets k-q$

40. End While

41. return $(C,A)$

42. End Function

> [!NOTE]
> **Minor improvements**
>
> Note that we check $\bigwedge_{\lambda=0}^{q-1}t_{e_\lambda}=1$ for the first iteration to check if we have chains that all go through other chains that were already all marked. This could be implemented via SIMD masks operations, but in my code I count the true bits and compare their count. This means there are still some very minor optimisation opportunities that could be explored

## Statistical modelling of CPU forward and backward passes

<a id="sec-chaining-runl-repeat-experiment"></a> <a id="sec-chaining-cpu-experimental-analysis"></a>

The first loop in Algorithm [chaining simd backtrack](../project_chaining/#alg-chaining-simd-backtrack) can process $L$ anchors at once only when $\operatorname{Run}_L(j,c)$ is true. We also use update counter $U$ in Algorithm [chaining b5](../project_chaining/#alg-chaining-b5) to terminate loop if the consecutive updates don’t improve any SIMD lane. At the same time minimap2 uses in its algorithm [chaining minimap2 backtracking](../project_chaining/#fig-chaining-minimap2-backtracking) to terminate backtracking if the score drop is too large.

We never explained any proper justification for using or not using these heuristics. This section talks about random synthetic data and intuitive explanation behind the choices. The subsections talk about:

- Preparing synthetic data (randomly generated sequences with insertions/deletions) and specially constructed examples aimed at absuing the scoring function and thresholds

- Answering if counting number of updates that do not improve score in forward chaining algorithm [chaining b5](../project_chaining/#alg-chaining-b5) improve performance

- Answering if the anchors during backtracking [chaining simd backtrack](../project_chaining/#alg-chaining-simd-backtrack) are just continous sequences of indices incremented by 1 ($\operatorname{Run}_L(j,c)$ condition)

- Answering if max drop would help us increase performance of SIMD backtracking algorithm or not [chaining minimap2 backtracking](../project_chaining/#fig-chaining-minimap2-backtracking)

> [!WARNING]
> **Synthetic data**
>
> Primary modelling here uses synthetic data built from randomly generated sequences assuming default substitution rate of $0.001$ and insertion/deleteion probability of $0.0005$. I prepared special cases of sequence construction using gaps or equal regions to see what happens in cases where extremally large gaps occur as secondary method of analysis. Overall the analysis is intended to motivate the approach and formulate conditions under which an optimisation’s cost exceeds its benefit.

### Problematic chains

As mentioned in section [chaining interpretation](../project_chaining/#sec-chaining-interpretation) there is a case of emergent assymetry: if one anchor matches with multiple anchors in the query, then we cannot easily tell which part of the query is best to match. The following sections demonstrates what happens in this scenario. The special query/target constructions are prepared in that asymmetry in mind.

#### Reference and query structures

<a id="fig-chaining-runl-inputs"></a>

![04 cpu expiermental analysis diagram](/static/figures/project/chapters-06-chaining-04-cpu-expiermental-analysis-tikz-01.svg)

*Panel (a) presents mosaic, with $A'=A$ so $T = Q = U_0\,A\,U_1\,A'_{\delta}\,U_2 \text{where} A' = \text{Randomize}_{\delta}(A)$. Panel (b) is degraded mosaic, so the $A$ and $A'$ are independently generated, so the divergence between them should be very high, used as control example. Panel (c) is shows spaced copy, which is contructed in the following way $T=S\,N^G,
\qquad
Q=(S\,N^G)^R,
\qquad
\lvert S\rvert=4096,
\qquad
G=16384$ Panel (d) is the Minimizer-island comb. I construct it by assuming that every $c_k$ and $r_{k,\ell}$ is an isolated 25-base minimizer island. Then $r_{k,\ell}$ are grouped together with 13-$N$ intervals to create $R_k$ groups. The construction is calculated in such a way that $|d_r - d_q|$ is lower for $c_k$ anchors than for $R_k$ anchors. Panel (e) is wide-gap spaced-copy, where we insert gaps between 16 30-base pieces of query with absurdly large 500-base gaps.*

In our experiments we use $\delta$ as input parameter for divergence (controlling substituiton rates). The exact substituion count (number of replaced bases in entire sequence) can be expressed as: $$s_{\delta} = \sum_{\text{fragment} \in \text{query}} \left\lfloor\delta\lvert A\rvert+\frac{1}{2}\right\rfloor$$ The $+\frac{1}{2}$ is just used to round the result to the nearest integer. The sum goes over all fragments we can change (this depends on exact construction as some of them need to contain specific exact regions)

Figure [chaining runl inputs](../project_chaining/#fig-chaining-runl-inputs) depicts all of my constructions:

- Random reference and query - parameters to experiments are $|Q|$ and $|T|$, number of occurences of query $1 \leq N$. I first generate random target, then place $N$ query occurences in it and use divergence to create substitution in the occurences.

- \(A\) Mosaic - reference and query with identical bytes. $A$ is randomly generated and I perform substitutions on it. This is example of exact duplication of long fragment. The specific $A$ length was choosen to be very long to generate long, very similar chains.

- \(B\) Degraded mosaic - this is version of mosaic, where we use two regions that aren’t equal and do not perform any substitutions on them. This is control case, where the generation of both $A$ regions is independent. Meaning the expected divergence should be very high (as sequences are random).

- \(C\) Spaced copy - we insert very long blocks $N^G$ between parts of reference, where $N$ denotes symbol $N$ (in dna4 alphabet it denotes unknown symbol). Normal processing just ignores these blocks (minimap2 doesn’t generate minimizers for them), but they distrupt anchor positions. You can think of this construction as very special case of gapped repeatitions of same sequence, where instead of random gap-filler we use symbol $N$. That kind of contruction shouldn’t occur in real-world example, it’s here to illustrate that we can utilize $N$ symbol to sabotage chaining.

- \(D\) Minimizer-island comb - we also use $N$ symbol as filler between inslands. As we use default minimap2 parameters including $w=10, k=15$, we adjusted $|c_k|$ to be $w+k=25$ bases, so these fragments generate maximum of two non-N minimizers. $R_k$ groups are internally built from the same 25-symbol "islands" separated by $13$ $N$ bases ($13$ was arbitrary choice). This creates very interesting scenario where we can control which minimizers occur where. Then we adjust wider gaps between "islands" with tricky criteria. As DP algorithm selects one "next" best anchor to "connect" chain to, we adjust the gaps so the difference between target and query distances are large for the anchors just at the edge of $R_k$ but smaller for $ck_kj$. You can look at the example and see that if we try to chain $c_1$ to $c_0$ the $\delta(c_0, c_1) = |d_r - d_q| = 3475-3375 = 100$ but $\delta$ for $R_1$ will be $\delta(c_0, R_1) = (3475+475) - (3375 + 25) = 550$. We exploit the feature of scoring function to convince the minimap2 that $c_k$ is "better" match being closer to diagonal than $R_k$. This will create long chain starting at $R_k$ and going through all $c_k$ anchors (ommiting all other $R$)

- \(E\) This example is more practical one, where we don’t overuse $N$ symbols and divide the query to add random gaps between its pieces. However the gaps are very large compared to the "islands" so this example is "exaggerated" as well.

> [!NOTE]
> **Overuse of $N$ symbol**
>
> Usually $N$ is used only to distinbguish processing errors (from the sequencing hardware) and do not occur with high quantities. The construction of the examples use them as gap-fillers to have easier control over occurence of minimizers and their behavior. Cases (A) and (E) are closer to real-world examples, where repetitions of genomes happen frequently.

Constructions (a), (b) and (c) are primary using the same idea. After collecting and sorting anchors the anchor table looks like this for spaced copy (sorted by target coordinate):

<a id="fig-spaced-copy-anchor-order"></a>

![04 cpu expiermental analysis diagram](/static/figures/project/chapters-06-chaining-04-cpu-expiermental-analysis-tikz-02.svg)

*Coordinate-sorted spaced-copy anchors for the concrete case $R=3$. As the query contains exact $R$ copies we will have "layers" ($t=0,1,2...$) of the same anchors with the same reference position. Anchor $a_{t,v}$ is the anchor for $v$-th copy in the query. Colored arrows follow mark the next anchor in the chain. Naturally each anchor points to the next anchor in the same $v$-th copy. This means that each anchor will just point $R$ anchors forward.*

The construction (d) - minimizer-island comb was created by adjusting gap sizes between query and target islands: $$\begin{array}{c|cc}
\text{transition} & \text{target }N\text{-run} & \text{query }N\text{-run}\\
\hline
P\to G & 1475 & 975\\
G\to c_0 & 1250 & 975\\
c_0\to c_1 & 3475 & 3375\\
c_k\to R_k & 475 & 25\ (k\text{ odd}),\ 925\ (k\text{ even})\\
R_k\to c_{k+1} & 1531 & 2081\ (k\text{ odd}),\ 981\ (k\text{ even}).
\end{array}$$

> [!NOTE]
> **Default minimap2 parameters**
>
> As we discussed due to larger shifts between target and query coordinates, the $R_k$ anchors will have larger pentalty due to higher distance from diagonal i.e $\delta$. Minimap2 by default uses max bandwith of $B = \text{bw} = 500$. Every anchor in $R_k$ and $c_{k+1}$ differ by 550 between the target and query distances, so those anchors fail the bandwidth check. This means that the best chain will be going through: $R_{m},c_{m},c_{m-1},...c_0,G,P$ as depicted in the figure [chaining runl inputs](../project_chaining/#fig-chaining-runl-inputs) The original dimentions of constrution can be calculated from the following formula ($r_{k,\ell}$ is the $\ell$-th anchor in $R_k$), $L$ is the count of "islands" in $R_k$:

#### Experiment design

Minimap2 2.31-r1302 was run with the `map-ont` preset (default one). I added instrumentation to record the score and predecessor arrays.

- Let $L\in\{2,4,8,16,32,64\}$ be the number of anchors viewed at the same time similar to our SIMD algorithms.

- Let $\operatorname{Run}_L(j,c) \iff j\geq L-1 \land c\leq n-L \land \bigwedge_{u=0}^{L-1}p_{j-u}=j-u-1$ be the condition used in BioseqDB SIMD algorithms.

- Let $B_L$ be the number of evaluations for which $\operatorname{Run}_L$ is true

- Let $E_L$ the number of times the first-path loop evaluates $\operatorname{Run}_L$

- Let $P_L$ the number that satisfy both bounds $j\geq L-1$ and $c\leq n-L$.

- Let $q$ be the number of anchors on the first predecessor path.

- Let $\operatorname{Fire}_L$ be the observed rate at which my heuristic fires

- Let $\operatorname{Contiguous}_L$ be the the ratio $\operatorname{Fire}_L$ but with bounds checks excluded (we measure firing rate but only after bounds checks)

- Let $\operatorname{Coverage}_L$ be the fraction of path anchors consumed by vector batches (this is a metric that answers the question "How many anchors we can cover by SIMD batches")

The exact definitions are as follows: $$\operatorname{Fire}_L=\frac{B_L}{E_L},
  \qquad
  \operatorname{Contiguous}_L=\frac{B_L}{P_L},
  \qquad
  \operatorname{Coverage}_L=\frac{LB_L}{q}.$$

*First-path branch-firing rate $100B_L/E_L$ in percent. The Mosaic, Degraded mosaic, and $R=1$ Spaced copy rows show medians over 3 randomly generated inputs using fixed seeds. For $R\geq2$, the Spaced copy row covers the tested powers of two from 2 through 64, all of which gave zero. The Minimizer-island-comb values are deterministic and agree with the recorded counters.*

<a id="tab-chaining-runl-firing"></a>

| Input | $L=2$ | $L=4$ | $L=8$ | $L=16$ | $L=32$ | $L=64$ |
|:---|---:|---:|---:|---:|---:|---:|
| \(a\) Mosaic | 1.17 | 0.59 | 0.29 | 0.14 | 0.07 | 0.02 |
| \(b\) Degraded mosaic (independent $A'$) | 100.00 | 99.94 | 99.74 | 99.24 | 97.01 | 91.12 |
| \(c\) Spaced copy, $R=1$ | 100.00 | 98.97 | 94.12 | 88.89 | 80.00 | 66.67 |
| \(c\) Spaced copy, $R\geq2$ | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
| \(d\) Minimizer-island comb, $m=2$ | 95.74 | 84.62 | 45.45 | 29.41 | 6.67 | 0.00 |
| \(d\) Minimizer-island comb, $m=1024$ | 4.21 | 2.10 | 0.96 | 0.48 | 0.19 | 0.00 |

We can make the following observations:

- For minimizer-island the prefix will contain some $C > 0$ anchors, meaning that we will get only continuous parts in $R_m$ at the end and in the sequence prefix: $B_L \simeq \left\lfloor\frac{39}{L}\right\rfloor + \left\lfloor\frac{C}{L}\right\rfloor, q = m + C$ This means that for $L = const:$ $\operatorname{Fire}_L, \operatorname{Coverage}_L < \theta(1/m)$ and for large enough $L > 39 \land m \rightarrow \infty$ they will approach $0$.

- For spaced copy the anchors are always separated by $R$ so $\operatorname{Fire}_L$ should be zero for all $R > 0$

- Degraded mosaic is just simulating two random fragments, so $\operatorname{Fire}_L$ should be almost $100\%$ with high measured divergence.

- Mosaic in this case was simulated with zero divergence, so the $\operatorname{Fire}_L$ should measure anchors only in the fillers between equal parts i.e $0\% < \operatorname{Fire}_L < \frac{|U_i|*3}{|U_i|*3+|A|} < 4\%$

The table [chaining runl firing](../project_chaining/#tab-chaining-runl-firing) confirms our assumptions.

Now we would like to see how divergence $\delta$ affects the mosaic rates. Table [chaining runl mosaic divergence](../project_chaining/#tab-chaining-runl-mosaic-divergence) shows selected data points from Mosaic runs at $\delta$. $s_{\delta}$ is the number of substitutions made to the target sequence. We can see that the higher number of same-target anchor alternatives (multiple query positions per anchor), the worse results we get (as predicted).

*Few most important data points for mosaic construction for different values of $\delta$ and degraded mosaic (should have effective high divergence). Each entry in the last three columns id the median followed by the observed minimum and maximum over three inputs generated from fixed random-number-generator seeds, in percent.*

<a id="tab-chaining-runl-mosaic-divergence"></a>

|  |  |  |  |  |  |
|:---|---:|---:|:---|:---|:---|
| Case | $s_{\delta}$ | $100\widehat{\delta}$ | Path anchors withsame-target alternatives | $100\operatorname{Fire}_4$ | $100\operatorname{Coverage}_4$ |
|  |  | (%) | (%) | (%) | (%) |
| $\delta=0$ | 0 | 0.000 | 97.69 \[97.64, 97.69\] | 0.59 \[0.58, 0.59\] | 2.31 \[2.29, 2.33\] |
| $\delta=0.0025$ | 82 | 0.250 | 93.51 \[93.40, 93.52\] | 1.06 \[1.01, 1.18\] | 4.12 \[3.91, 4.57\] |
| $\delta=0.0050$ | 164 | 0.500 | 89.55 \[89.47, 89.68\] | 1.56 \[1.47, 1.79\] | 5.96 \[5.64, 6.79\] |
| $\delta=0.0100$ | 328 | 1.001 | 82.24 \[82.19, 82.29\] | 2.61 \[2.57, 2.80\] | 9.69 \[9.55, 10.34\] |
| $\delta=0.0200$ | 655 | 1.999 | 69.33 \[68.93, 69.42\] | 5.14 \[5.08, 5.30\] | 17.82 \[17.64, 18.28\] |
| $\delta=0.0500$ | 1638 | 4.999 | 41.41 \[41.33, 41.63\] | 14.97 \[14.97, 15.38\] | 41.32 \[41.32, 42.09\] |
| $\delta=0.1000$ | 3277 | 10.001 | 17.23 \[16.81, 17.64\] | 37.99 \[37.64, 38.72\] | 71.02 \[70.71, 71.65\] |
| $\delta=0.2000$ | 6554 | 20.001 | 3.04 \[2.66, 3.36\] | 79.41 \[77.58, 81.93\] | 93.91 \[93.26, 94.77\] |
| Independent $A'$ |  | 75.043 | 0.00 \[0.00, 0.00\] | 99.94 \[99.90, 100.00\] | 99.98 \[99.98, 100.00\] |

Let’s consider that we are looking at mosaic with divergence rate $\delta = 0$, so $A = A'$. Then we have $h$ pairwise-disjoint "blocking" minimizer windows $W_1, \ldots, W_h$. Each $W_i$ has some minimizer that now forms an anchor that makes $Run_L(j,c)$ evaluation false. $W_i$ covers $k+w-1$ bases. Each of the windows needs to receive at least one subsitution so they’re changed and do not generate one anchor with two different positions. So with probability $\frac{(s_{\delta})_h}{(M)_h}$ all $h$ windows will receive at least one substitution (for $(x)_h = x(x-1)\cdots(x-h+1)$). We can choose one base from every window with $b^h$ ways, so this gives us very loose upper bound on the probability: $$\begin{aligned}
  \Pr_{\pi}\!\left[
    \operatorname{Run}_L(j,c)\text{ changes from false to true}
  \right]
  &\leq
  \Pr_{\pi}\!\left[
    S_{\delta}\cap W_i\neq\varnothing
    \text{ for every }i
  \right] \\
  &\leq
  b^h\frac{(s_{\delta})_h}{(M)_h}
  \leq
  (b\widehat{\delta})^h .
\end{aligned}$$ Obiously disjoint-window model is a bad assumption as substitution can affect several overlapping windows and conversely with high divergence we can spontaneusly get random matches with other fragments like $U_i$ (although this is unlikely). However we can loosely bound the probability by $\theta((k\widehat{\delta})^h)$. This gives us idea that the worst sequences are the one with perfect repeats (low divergence).

Figure [chaining runl mosaic divergence](../project_chaining/#fig-chaining-runl-mosaic-divergence) shows the results for mosaic for different levels of divergence (the degraded mosaic - as control sample - is show separately as a gray rectangle on the right side of chart)

<a id="fig-chaining-runl-mosaic-divergence"></a>

![04 cpu expiermental analysis diagram](/static/figures/project/chapters-06-chaining-04-cpu-expiermental-analysis-tikz-03.svg)

*The lighter colours represent minimum/maximum while the darker ones represent median. As we can see the chart confirms that the $Run_L$ firing rates are tied to number of anchors with same-target positions which are competing for the same anchor. Additionally we observe that divergence levels of at least 12% mitigate the negative effect of highly-repetive query.*

As we can see from Figure [chaining runl mosaic divergence](../project_chaining/#fig-chaining-runl-mosaic-divergence), table [chaining runl mosaic divergence](../project_chaining/#tab-chaining-runl-mosaic-divergence) and table [chaining runl firing](../project_chaining/#tab-chaining-runl-firing) the most inconvenient case is the high number of repetitions in the query. These repetitions can be mitigated by increased divergence levels. This was intuitive consulsion that we now sucessfully confirmed with the experiments.

#### Random-sequence subsitutions

I also wanted to measure effects of random sequences (not specially constructed) on the $Run_L$ firing rates:

- We generate random sequences query/target parameterized by their lengths: $|T|$ and $|Q|$. We choose $N$ positions in target to be the query and replace its fragments accordingly. By default we test each such occurence base with probability $0.001$ to be substituted, $0.0005$ to be inserted/deleted.

- Here I also use the default minimap2 parameters: $k=15$, $w=10$, occurrence filtering, and chaining scores.

<a id="fig-chaining-runl-random-occurrences"></a>

![04 cpu expiermental analysis diagram](/static/figures/project/chapters-06-chaining-04-cpu-expiermental-analysis-tikz-04.svg)

*Branch firing rate $100\operatorname{Fire}_L$ for different values of $L$ and $N$ for random-sequence occurrences. As we can observe the $N$ doesn’t affect the firing rate as expected. The light colours represent middle 50% of inputs. As we observe as $L$ gets larger it’s more likely to disrupt the condition randomly so the standard deviation should be higher.*

Figure [chaining runl random occurrences](../project_chaining/#fig-chaining-runl-random-occurrences) was used to just confirm that random generation is correct and $N$ doesn’t affect the probability (it can affect probability if the target size is too small and query occurences are too crowded and start to overlap). Figure [chaining max drop runl](../project_chaining/#fig-chaining-max-drop-runl) shows more interesting data. I tested $Fire_L$ (panel a) and $Reduction_L$ (panel b)for different valujes of $\delta$ and $L$.

<a id="fig-chaining-max-drop-runl"></a>

![04 cpu expiermental analysis diagram](/static/figures/project/chapters-06-chaining-04-cpu-expiermental-analysis-python-01.svg)

*Each data point represents sum of samples (averaged rates) over N=2,3,4...,4000 randomized experiments. The samples for $N=1$ were marked with circles as we expect that they will be significantly higher (as there’s one ocurrence)*

> [!NOTE]
> **Firing rate**
>
> Naturally the firing rate for $L=4$ is higher, but $Reduction_L$ is initially higher for $L=16$ meaning that on the hardware supporting more vectors (longer SIMD vectors), we exchange more condition checks (it’s harder to guarantee $Run_L(i,j)$), but for more saved instructions (we can skip more $L$ anchors). In practice we should test this on different hardware as the ideal loop reduction on panel (b) doesn’t involve real costs of the instructions in cycles or cache latency etc.

### Score distribution for random cases

If we plot the anchor count versus the cummulative chain score, we should get a curve that is roughly linear (this follows from formulation of the scoring function). The divergence should break long chains and make the curve score axis more narrow. This can be observed on Figure [chaining max drop score counts](../project_chaining/#fig-chaining-max-drop-score-counts). I supply the charts to show how random inputs can behave as supplementary resources to the theoretical chapters. I also provide figure [chaining max drop score divergence](../project_chaining/#fig-chaining-max-drop-score-divergence) to better illustrate "narrowing" of the chart

<a id="fig-chaining-max-drop-score-counts"></a>

![04 cpu expiermental analysis diagram](/static/figures/project/chapters-06-chaining-04-cpu-expiermental-analysis-python-02.svg)

*Results from randomized sequences for slightly different query lengths (997-1002 bases). The orange line is the minimum score threshold for chain (minimap default of $\verb|min_sc|=40$)*

<a id="fig-chaining-max-drop-score-divergence"></a>

![04 cpu expiermental analysis diagram](/static/figures/project/chapters-06-chaining-04-cpu-expiermental-analysis-python-03.svg)

*Cummulative results for all the experiments previously described (query lenghts 997-1002 bases). In the chart I take 6-input median (median over all 6 experiments with slightly different query lengths)*

### Does the bounded SIMD $U$ limit help?

<a id="sec-chaining-u-batch-experiment"></a>

We analyzed the $Run_L$ heuristis for chaining backtracking. Now I would like to apply similar measurements to determine how much limit on batch iteration $U$ help us.

As reminder $U$ is the maximum number of batches that consecutively do not improve the score for any chain during forward chaining [chaining b5](../project_chaining/#alg-chaining-b5) Minimap2 uses skip heuristic while we treat batch as a whole.

For SIMD width $L$, let $E_L$ be the number of executed batches (same as in previous sections), $Z_L$ the executed batches for which no lane satisfies $C_\lambda>H_\lambda$ (no score improvement), and $Q_L$ the batches in the complete window without the $U$ stop. Similarily we define the firing rate and reduction rate: $$\operatorname{NoUpdate}_L=Z_L/E_L,
 \qquad
 \operatorname{Reduction}^{U}_L=(Q_L-E_L)/Q_L.$$

The measurements on Figure [chaining u batch reduction](../project_chaining/#fig-chaining-u-batch-reduction) are pooled (summed over random-seed experiments), while Table [chaining u batch rates](../project_chaining/#tab-chaining-u-batch-rates) contains the separate runs for $0\%$ and $10\%$ divergence rates.

<a id="fig-chaining-u-batch-reduction"></a>

![u batch chart diagram](/static/figures/project/chapters-06-chaining-04-cpu-experimental-analysis-u-batch-chart-tikz-01.svg)

*Pooled $U$-only reduction at each divergence for (a) random inputs and (b) sum over mosaic and degraded mosaic*

*$\operatorname{NoUpdate}_L/\operatorname{Reduction}^{U}_L$ (percent). Random uses $N=4000$, a 4.5-Mbase target, 997–1002-base queries, and the same divergence levels as presented previously. Spaced copy uses $R\in\{1,2,4,8,16,32,64\}$. The Minimizer-island comb uses $m\in\{1,2,4,\ldots,1024\}$. Defaults are $k=15$, $w=10$, $S=25$, and `max_chain_iter`=5000.*

<a id="tab-chaining-u-batch-rates"></a>

As we can observe the firing rate is high for most of the cases. This is different to $Run_L$ predicate, because in this case we don’t care if we follow different chains as the same time (this is purpose of $U$ limit). We only care about the fact that at some point the chain will end and we won’t be able to improve it. The boundary limits like $D$ checking the distances to diagonal will kick in, but we can resonably expect that we will always have some anchor nearby. This means that $U$ heuristic is very useful for forward chaining. My constructions became problematic only for very high $L$ where it’s very likely that some chain will improve anyway.

### BioseqDB `max_drop` analysis

<a id="sec-chaining-max-drop-analysis"></a>

The last part to analyse is if the $max\_drop$ heuristic can be useful for backtracking [chaining minimap2 backtracking](../project_chaining/#fig-chaining-minimap2-backtracking). As reminder, the BioseqDB backtracking procedure follows predecessor links until it reaches the sentinel $p_j=-1$ or an anchor that was already assigned to an earlier chain. Minimap2 can stop a predecessor scan earlier when its running score falls sufficiently far below the best score seen on that scan.

Formally let $f_i$ be minimap2’s dynamic-programming score for the best chain ending at anchor $i$. The minimap2 finds an eligible chain end $e$, let $B$ be the number of steps made by the BioseqDB-style baseline scan, which stops only at $p_j=-1$ or at a previously marked anchor and let $b_m$ be the boundary reached after predecessor step $m$, and define $$s_m=
  \begin{cases}
    f_e-f_{b_m}, & b_m\geq0,\\
    f_e, & b_m=-1,
  \end{cases}
  \qquad
  M_m=\max(0,s_1,\ldots,s_m).$$ Define the score drop at step $m$ as $$d_m=M_m-s_m,
  \qquad
  \Delta(e)=\max_{1\leq m\leq B}d_m.$$ Thus $d_m$ is the distance from the best score already seen in this backtrack to the score at the current step. The value $\Delta(e)$ is the largest such drop in the scan from end $e$. Minimap2 predicate will want to terminate the execution when $d_m>D$.

I tried measuring how frequently minimap2 fires the predicate. Table [chaining max drop length screen](../project_chaining/#tab-chaining-max-drop-length-screen) contains very dissapointing results. By default max drop treshold is $D=500$, but we didn’t even observed that. The max delta is always zero because the score always improves along the chain.

| $T$ (Mbp) | query length (bp) |       $B$ | $\Delta_{\max}$ |     |     |     |     |     |
|----------:|------------------:|----------:|----------------:|----:|----:|----:|----:|----:|
|     1.125 |           247–252 |    25 705 |               0 |     |     |     |     |     |
|     1.125 |          997–1002 |   109 715 |               0 |     |     |     |     |     |
|     1.125 |         3997–4002 |   440 525 |               0 |     |     |     |     |     |
|     1.125 |       15997–16002 | 1 757 774 |               0 |     |     |     |     |     |
|     4.500 |           247–252 |    25 705 |               0 |     |     |     |     |     |
|     4.500 |          997–1002 |   109 715 |               0 |     |     |     |     |     |
|     4.500 |         3997–4002 |   440 525 |               0 |     |     |     |     |     |
|     4.500 |       15997–16002 | 1 757 741 |               0 |     |     |     |     |     |
|    18.000 |           247–252 |    25 670 |               0 |     |     |     |     |     |
|    18.000 |          997–1002 |   109 715 |               0 |     |     |     |     |     |
|    18.000 |         3997–4002 |   440 525 |               0 |     |     |     |     |     |
|    18.000 |       15997–16002 | 1 757 741 |               0 |     |     |     |     |     |

*Default-$D=500$ target- and query-length screen*

<a id="tab-chaining-max-drop-length-screen"></a>

Thankfully the wide-gap construction can help in this case. The 19,980-base target contains the same parts in the same order, but we insert a random 500-base. Figure [chaining runl inputs](../project_chaining/#fig-chaining-runl-inputs)(e) shows this construction. This means that we can get lots of anchors that doesn’t match to improve the score at all. The "requested substituiton" rate refers to substitutions in the separated target fragments.

For panel (a) [chaining max drop long chain accounting](../project_chaining/#fig-chaining-max-drop-long-chain-accounting) I calculate how many steps we perform compared to the place where we could terminate execution (lower percentage means that the heuristic won’t save anything). I don’t mark the anchors so we always follow chains right to the end. Panel (b) [chaining max drop long chain accounting](../project_chaining/#fig-chaining-max-drop-long-chain-accounting) demonstrates that when marking chains the panel (a) don’t show full truth. Max drop will save us scanned anchors, but because we won’t mark the chains the actual work is larger (later we need to scan the anchors).

<a id="fig-chaining-max-drop-long-chain-accounting"></a>

![04 cpu expiermental analysis diagram](/static/figures/project/chapters-06-chaining-04-cpu-expiermental-analysis-python-04.svg)

*Scan-step accounting for Wide-gap spaced-copy at default $D=500$. Its construction and exact dimensions are shown in Figure [chaining runl inputs](../project_chaining/#fig-chaining-runl-inputs)(e). Panel (a) counts the part of each fixed-start baseline scan after we reach max drop for first time. The dashed value is $\sum S/\sum B$ over all 246 observations. Panel (b) applies every cutoff and updates the marks, which increases the complete scan work. These are logical scan steps, not instructions, elapsed time, or a measured slowdown.*

After consulting relevant work we see that max drop heuristic isn’t used to reduce number of scanned anchors, but rather to prevent chaining unrelated fragments of sequences. The minimap2 will try to chain large gaps as unrelated fragments, which increases amount of work performed. This however is reported to increase matching accuracy \[[8](../bibliography/#ref-minimap2_why_max_drop)\]

In BioseqDB I didn’t implement this heuristic in the SIMD algorithm at this point.

### Conlusions

> [!WARNING]
> **Final conlusions**
>
> From the experimental samples we can observe that in the random scenario:
> 
> - $Run_L$ is beneficial to performance increase
> 
> - $U$ limit is very beneficial to performance increase
> 
> - $max\_drop$ increases work done scalarily by minimap2, but according to literature increases quality of the alignment
> 
> Thus I decided to leave these heuristics in, while the $max\_drop$ can be implemented in the future (as observed on the random cases, it only even triggers in case of very large gaps, so it requires careful testing on real-world datasets as well as estimating how SIMD implementation will penalize the execution times)

## BioseqDB SIMD performance comparison

I perfomed intermediate testing to verify if predictions coming from synthetic results can be correct.

The test used $|T|=500Mbps$ and $|Q|=5Mbps$ sequences with just one $N=1$ occurence (this means that this is very optimistic case of random scenario)

<a id="fig-chaining-mode-times"></a>

![05 cpu temporary results diagram](/static/figures/project/chapters-06-chaining-05-cpu-temporary-results-tikz-01.svg)

*Recorded times for the three CPU modes*

The figure above depicts only two runs and presents only quick prototyping test measurements to judge if we are going in right direction. Please note that on bottom chart B1 and B5 are similar, because they use the same backtracking implementation (this is the same algorithm).

## Range minimum query chaining

<a id="sec-chaining-rmq-idea"></a>

The chaining score model is described in Section [chaining score model](../project_chaining/#sec-chaining-score-model) gives an ability to better express relationships between anchors we want to optimize. However it’s more complex to calculate than simple linear scoring model. As noted in equation [global score definition chaining rmq](../project_chaining/#eq-global-score-definition-chaining-rmq), we can compute $\text{GlobalScore}(i)$ independently for each anchor author of minimap2 did in minigraph paper \[[7](../bibliography/#ref-minimap2_rmq_scoring_minigraph)\]. The problem then is reduced to finding the maximum among all values in continous segment of anchors.

Range minimum query \[[4](../bibliography/#ref-cp_algorithms_rmq)\] (RMQ) algorithm is used to calculate the linear score $h(j)$ for anchor, while minimap2 later calculates actual score $f_j+E(j,i)$ (and if the RMQ result is not sufficiently "good", then it tries again with different parameters) <a id="eq-chaining-rmq-h"></a> $$h(j)=f_j+\frac{\lambda_g}{2}(r_j+q_j).$$

### Minimap2 RMQ

<a id="sec-chaining-minimap2-rmq"></a>

I didn’t initially found papers going into details of minimap2 RMQ’s implementation, so here I present detailed description of the algorithm. The minigraph paper \[[7](../bibliography/#ref-minimap2_rmq_scoring_minigraph)\] describes only the high-level approach, without going into details of the implementation. Minimap2’s RMQ is implemented using two AVL trees \[[5](../bibliography/#ref-cp_algorithms_segment_tree)\]. First of them is used to query anchors in the interval $0<r_i-r_j\leq D$. Each time minimap2 removes nodes from the tree if either they go outside of this interval or node count reaches $C$ (the total limit of nodes per tree). The second tree has the same structure, but is used to query anchors in the interval $0<r_i-r_j\leq D_{\mathrm{in}}$ with the intent to have $D_{\mathrm{in}} \leq D$ so if we get an anchor candidate that is not sufficiently "good", we can then perform another query on second tree, where we will get scores for smaller neighbourhood of an anchor. I will call this second lookup the "local scan". Minimap2 $D_{\mathrm{in}}$ can be set to $0$ to disable second AVL tree entirely.

While the original anchor array is sorted by increasing reference coordinates $r_i$, the AVL trees order the node primarily by increasing query coordinate, then by increasing anchor index in the array (pairs $(q_j, j)$). As we want to prevent chaining anchors with the same reference coordinate, we will not add single anchor to the trees (a "reference group" is the continous sequence in the anchor array that has the same reference position, but different query positions). We will add entire reference group to the tree at once. This is implemented via $\Call{InsertCompletedReferenceGroup}{T,a,f,i}$ pseudocode function which checks if current anchor reference coordinate is different to the saved one.

The trees also need to remove anchors that go outside reference coordinates i.e $0<r_i-r_j\leq D_{\mathrm{in}}, D$. This is implemented via $\Call{RemoveExpiredEntries}{T,a,i,D,C}$ pseudocode function. Such operation could be implemented by visiting each node in the entire tree, however internally minimap2 saves pointer (cursor) to the first anchor in the window and moves it when going to next anchor. This results in better reemoval theoretical complexity.

The last complex operation is local scan. After best candidate $j^{*}$ is selected from the main tree $T$, we calculate it’s score $c_{j^\star}=f_{j^\star}+E(j^\star,i)$ and accept it only when the anchor is "exact": <a id="eq-chaining-rmq-exact"></a> $$\delta(j^\star,i)=0
  \quad\land\quad
  g(j^\star,i)\leq\ell_{j^\star}.$$ meaning that two anchors advcance equally and no more than k-mer span (as discussed earlier $k = \ell_{j^\star}$ for HPC disabled [minimap hpc](../project_minimizers/#info-minimap-hpc)). If that’s not the case, then minimap2 runs the local scan. The scan goes through all anchors in the query coordinate interval $q_i-D_{\mathrm{in}}\leq q_j\leq q_i-1$. This is implemented by first finding the first anchor in the tree with largest $q_i$ value that is strictly less than $q_j$ and then iterating candidates in decreasing order of $q_j$ value (this is ordinary tree traversal). The scan itself is implemented via $\Call{ScanLocalInterval}{T_{\mathrm{in}},i,D_{\mathrm{in}},B,S,f,p,t}$ pseudocode function and is very similar to forward chaining loop in minimap2 scalar algorithm [chaining b1](../project_chaining/#alg-chaining-b1). The only difference is that in the forward chaining loop without RMQ minimap2 iteration was driven by $max\_iter$ limit for anchors not sorted by query coordinate, while in the RMQ scan we don’t have $max\_iter$ limit (the minimap2 skip counter is used in the same way - it stops iteration on anchors that don’t improve score several times in a row).

> [!NOTE]
> **Theoretical complexity**
>
> In the previous forward chaining loop [chaining b1](../project_chaining/#alg-chaining-b1) we iterated over all anchors and bounded the number of iterations by $max\_iter$ limit. In the simplest calcualtion we can say that number of candidates is bounded by $\Theta(n)$ for one anchor, so entire pass does $\Theta(n^2)$ work. In case of RMQ each insertion and deletion (inserting reference groups and removing expired anchors) is amortized to $O(\log n)$ work for each anchor (each anchor can be added and removed at most once), so if the number of the candidates is the same, then overall work is bounded by $\Theta(n \log n + n^2)$. The RMQ is also inherently less friendlier to efficient pipelining and cache locality as we will need to jump across the tree frequently and it uses linear scoring model which can be less accurate. The benefit comes from the fact that RMQ can be more efficient with larger boundary limits, so it can be used for cases of long sequences with large insertions/deletions with lots of noise. \[[7](../bibliography/#ref-minimap2_rmq_scoring_minigraph)\]

Figure [chaining minimap2 rmq](../project_chaining/#fig-chaining-minimap2-rmq) illustrates how RMQ chaining works.

<a id="fig-chaining-minimap2-rmq"></a> <a id="fig-chaining-rmq-contrast"></a>

![06 rmq diagram](/static/figures/project/chapters-06-chaining-06-rmq-tikz-01.svg)

*Minimap2 RMQ for one target. (a) We maintain the range of reference coordinates in the array to know where the window starts (what is the anchor with lowest reference coordinate that still fits in the $r_i-D$ window). (b) Select the best anchor according to linear scoring model $j^\star$; “max $h$” is the largest value in a node’s subtree. (c) If the selected anchor $j^\star$ is not exact (Equation [chaining rmq exact](../project_chaining/#eq-chaining-rmq-exact)), then we run the local scan. The blue rectangle is the local interval the green band satisfies $\delta\leq B$, orange crosses mark rejected candidates, including one inside the local interval. A scanned link (blue dot) replaces the current result only if both checks below pass. The local scan can stop early and do not iterate over all anchors in the local interval if for example skip counter $S$ is reached.*

Algorithm [chaining minimap2 rmq](../project_chaining/#alg-chaining-minimap2-rmq) shows how RMQ forward chaining is implemented inside minimap2. <span class="smallcaps">RangeBest</span> function returns the anchor with the largest $h(j)$ in the interval, or $\bot$ if the interval contains no retained anchor, while <span class="smallcaps">CompleteLink</span> function calculates $E(j,i)$ from Equations [chaining link score](../project_chaining/#eq-chaining-link-score)–[chaining penalty](../project_chaining/#eq-chaining-penalty), together with $\delta$ and the exact (local scan) condition in Equation [chaining rmq exact](../project_chaining/#eq-chaining-rmq-exact).

##### Minimap2 RMQ forward chaining

<a id="alg-chaining-minimap2-rmq"></a>

1.  Input: sorted anchors $a_0,\ldots,a_{n-1}$, distance $D$, inner distance $D_{\mathrm{in}}$, bandwidth $B$, tree cap $C$, skip limit $S$

2.  Output: scores $f$ and predecessor indices $p$

3.  Function `Minimap2RMQForward`($a,D,D_{\mathrm{in}},B,C,S$)

4.  $D\gets\max(D,B),\quad
          D_{\mathrm{in}}\gets\min(D,\max(0,D_{\mathrm{in}}))$

5.  $T\gets\varnothing,\quad T_{\mathrm{in}}\gets\varnothing$ (augmented AVL trees)

6.  $t\gets(0,\ldots,0)$ (B1 marks for the optional local scan)

7.  For $i\gets0$ **to** $n-1$

8.  `InsertCompletedReferenceGroup`($T,a,f,i$)

9.  If $D_{\mathrm{in}}>0$

10. `InsertCompletedReferenceGroup`($T_{\mathrm{in}},a,f,i$)

11. End If

12. `RemoveExpiredEntries`($T,a,i,D,C$)

13. If $D_{\mathrm{in}}>0$

14. `RemoveExpiredEntries`($T_{\mathrm{in}},a,i,D_{\mathrm{in}},C$)

15. End If

16. $f_i\gets\ell_i,\quad p_i\gets-1$

17. $j^\star\gets
            \texttt{RangeBest}(T,(q_i-D,\mathrm{INT32\_MAX}),(q_i,0))$

18. If $j^\star\neq\bot$

19. $(e,\delta,\mathrm{exact})\gets
              \texttt{CompleteLink}(j^\star,i)$ ($e=E(j^\star,i)$)

20. If $\delta\leq B\land f_{j^\star}+e>f_i$

21. $f_i\gets f_{j^\star}+e,\quad p_i\gets j^\star$

22. End If

23. If $\neg\mathrm{exact}\land T_{\mathrm{in}}\neq\varnothing
                 \land q_i>0$

24. $(f_i,p_i)\gets
                \texttt{ScanLocalInterval}(T_{\mathrm{in}},i,D_{\mathrm{in}},B,S,f,p,t)$ (Algorithm [chaining rmq local scan](../project_chaining/#alg-chaining-rmq-local-scan))

25. End If

26. End If

27. End For

28. return $f,p$

29. End Function

Algorithm [chaining rmq local scan](../project_chaining/#alg-chaining-rmq-local-scan) shows how local scan is implemented. I didn’t want to show other tree maintenance operations as there aren’t many valuable details to discuss (this is usual AVL tree maintenance).

Similarily to minimap2 forward chaining we use $t_j$ array to mark visited candidates i.e mark $t_j=i$ means that a candidate already tested for target $i$ points to $j$ through its saved predecessor. It must be noted that the for loop in the local scan algorithm [chaining rmq local scan](../project_chaining/#alg-chaining-rmq-local-scan) utilises the fact that anchors are sorted by query coordinate so we can just go to the leftmost node in the interval and iterate each leaf from left to right (I’m reiterating this to showcase that the for loop hides this complexity in hte pseudocode).

##### Minimap2 local scan after the primary RMQ query

<a id="alg-chaining-rmq-local-scan"></a>

1.  Input: maintained inner tree $T_{\mathrm{in}}$, target $i$, local limit $D_{\mathrm{in}}$, bandwidth $B$, skip limit $S$

2.  Input: completed earlier scores and predecessors $f,p$, marks $t$, and current result $(f_i,p_i)$

3.  Output: updated $(f_i,p_i)$ and marks $t$

4.  Function `ScanLocalInterval`($T_{\mathrm{in}},i,D_{\mathrm{in}},B,S,f,p,t$)

5.  $s\gets0$

6.  For retained $j\in T_{\mathrm{in}}$ with $q_i-D_{\mathrm{in}}\leq q_j\leq q_i-1$, by decreasing $(q_j,j)$

7.  $c\gets f_j+E(j,i),\quad
            \delta\gets|d_r(j,i)-d_q(j,i)|$

8.  If $\delta\leq B$

9.  If $c>f_i$

10. $f_i\gets c,\quad p_i\gets j,\quad s\gets\max(0,s-1)$

11. ElsIf $t_j=i$

12. $s\gets s+1$

13. If $s>S$

14. **break**

15. End If

16. End If

17. If $p_j\geq0$

18. $t_{p_j}\gets i$

19. End If

20. End If

21. End For

22. return $(f_i,p_i)$

23. End Function

### RMQ re-chaining in the pipeline

<a id="sec-chaining-rmq-rechaining"></a>

Normally Minimaps2 disables RMQ for chaining by default, the manual recommends`--rmq` for alignment through long insertions and deletions \[[90](../bibliography/#ref-minimap2_rmq_manual)\], but at the same time RMQ is cheaper so minimap2 can use it to perform "re-chaining".

> [!NOTE]
> **RMQ re-chaining**
>
> *RMQ re-chaining* means running chaining again with a larger bandwidth to try to connect chains across long gaps. This is optional step that runs in minimap2 after the forward chaining and chaining backtracking - after step 3 [chaining components](../project_chaining/#list-chaining-components). Minimap2 uses array $A$ which is sorted by radix sort by reference coordinates again and provided to RMQ chaining algorithm. Please note that this means that minimap2 does some duplicate work - first it groups anchors by the chain they belong to (placing them in $A$ array), but rechaining sorts anchors again. Minimap2 also discards $f$ and $p$ arrays from first chaining step. RMQ re-chaining will then work on the subset of original input anchors from seeding (not every anchor will form a chain that will be accepted by its length and score limits [chaining minimap2 backtrack](../project_chaining/#alg-chaining-minimap2-backtrack)). As RMQ re-chaining is special case not enabled by default in minimap2, it wasn’t in scope of this project - I didn’t measure how many anchors are retained and how RMQ re-chaining affects quality of alignments and performance.

> [!WARNING]
> **BioseqDB does not support re-chaining**
>
> I don’t provide any specialized stage in pipeline for re-chaining. Some of the software in literature support increasingly precise queries to improve alignments iteratively\[[6](../bibliography/#ref-mmseqs2_gpu_paper)\]. This can be implemented in the future.

Normally in minimap2 RMQ re-chaining will be only enabled iff all conditions are satisfied:

- RMQ re-chaining is enabled only for specific presets (assembly presets), so user have to select specific preset or use `--rmq` flag explicitly

- There’s more than one chain (easy to satisfy)

- There’s only 1 query in the mapping call

- Either query length minus span of first chain is greater than 1000 bases or span of first chain is greater than 10% of query length (very long insertions and deletions)

The conditions above were created by me after inspection of source code, so the list might not be complete, but it illustrates why re-chaining wasn’t first priority for me.

### RMQ in BioseqDB

<a id="sec-chaining-bioseqdb-rmq-summary"></a>

In BioseqDB I implemented the same algorithm as minimap2 with some differences:

*Differences between minimap2 and BioseqDB RMQ*

<a id="tab-chaining-bioseqdb-rmq-differences"></a>

|  | **Minimap2** | **BioseqDB** |
|:---|:---|:---|
| Local scan | Optional scan with complete scores and the B1 skip rule | Local scan is implemented by disabled by default by setting $D_{\mathrm{in}}=0$ |
| Query interval | Composite bounds in Algorithm [chaining minimap2 rmq](../project_chaining/#alg-chaining-minimap2-rmq) | Includes both $q_i-D$ and $q_i$ |
| Bandwidth check | Additionally rejects anchor when $\delta>B$ | Does not check that |
| RMQ re-chaining | At most one extra forward run and backtracking step with a larger bandwidth | No any re-chaining stage is implemented |
| Backtracking | Uses the minimap2 backtracking algorithm [chaining minimap2 backtrack](../project_chaining/#alg-chaining-minimap2-backtrack) | Performs scalar BioseqDB backtracking which is similar algorithm to minimap2, however we could potentially use SIMD backtracking (backtracking isn’t separate pipeline stage, so SIMD chaining selects both forward/backtrack in SIMD, while RMQ has it’s own scalar backtracking) |

> [!WARNING]
> **Other algorithmic limitations**
>
> There are some other limitations:
> 
> - Including $q_j=q_i$ allows a link with no query advance. BioseqDB searches from $(q_i-D,\mathrm{INT64\_MIN})$ to $(q_i,\mathrm{INT64\_MAX})$, so every anchor at either query boundary is eligible for selection (it should have minor impact on performance)
> 
> - Omitting the bandwidth reject i.e $\delta>B$ allows a link farther from the diagonal than the configured $B$ (this was ommited due to simpler code and RMQ having lesser priority than oder algorithms)
> 
> I didn’t measure effects of these limtations neither on performance nor on quality of alignments.

> [!NOTE]
> **Future work**
>
> The backtracking for RMQ can be removed and I could use SIMD backtracking. This would require separating forward/backward chaining into separate pipeline stages. Moreover in the future I can use specialized GPU acceleartion for RMQ (as noted before G3SA contains kernels for RMQ which weren’t used). There exists some liteature \[[91](../bibliography/#ref-gpu_rmq_kreis_2026)\] discussing GPU acceleration for RMQ problems using ray tracing and CUDA cores (one limitation is that in the paper the RMQ base array is static, so it would require additional design work to design GPU-friendly algorithm).

## GPU chaining

<a id="sec-chaining-b2"></a>

### G3SA algorithm

The G3SA paper divides GPU kernel into 4 stages:

1.  **First forward-scoring pass:** Either use short kernel (specialized for short sequences) or tiled kernel (tiled kernel is just dividing sequence into smaller chunks and applying modified short kernel). This stage computes total scores as normal chaining $f_j + E(j,i)$.

2.  **First backtracking and packing:** G3SA applies minimap2’s cut and marking rule, reverses accepted paths, and packs their anchors into a buffer.

3.  **Conditional ranking:** If the difference between sizes of query and target is big (see Figure [chaining pipelines](../project_chaining/#fig-chaining-pipelines)), then kernel compares simplified $h(j)$ and calculates one complete link score for the best canidate (similar to how minimap2 does RMQ rescoring mentioned earlier)

4.  **Final backtracking:** This is done via minimap2 scalar backtracking algorithm

> [!NOTE]
> **G3SA AVL code**
>
> The repository contains CUDA AVL support code and a commented experimental entry point. The active chaining pipeline does not call that entry point. Its second pass uses array-based short and tiled kernels.

<a id="fig-chaining-pipelines"></a>

![07 gpu diagram](/static/figures/project/chapters-06-chaining-07-gpu-tikz-01.svg)

*Simplified depictions of G3SA pipeline*

The main difference between G3SA is that it uses specialised paths depending on data size and it calculates rows instead of columns in the score matrix for anchors: the first kernel instead of looking at previous anchors and updating itself, looks at next anchors and updates them [chaining work split](../project_chaining/#fig-chaining-work-split).

<a id="fig-chaining-work-split"></a>

![07 gpu diagram](/static/figures/project/chapters-06-chaining-07-gpu-tikz-02.svg)

*Different approaches (transposed) of minimap2 and G3SA*

After long analysis I decided to implement the simpler approach. Instead of writing multiple kernels, just naively port existing chaining logic to Vulkan API.

### BioseqDB Vulkan tiled DP

<a id="sec-chaining-vulkan"></a>

BioseqDB’s `vulkan_tiled_dynamic_programming` is the usual chaining algorithm that fixes one anchor $i$ and examines earlier predecessors in batches of 32.

The host first sets $$D=\max\{\verb|max_gap|,\verb|bandwidth|\}.$$ and sorts anchors to filter them by adjacent reference gap greater than $D$. For a fixed target, lane $u$ first scores predecessor $i-u-1$. The next iteration moves all lanes 32 positions to the back. Every active lane calculates the complete value $f_j+E(j,i)$ to then select one pair $(\text{score},j)$.

##### BioseqDB Vulkan forward chaining for one hard range

<a id="alg-chaining-vulkan"></a>

1.  Input: sorted range $a_b,\ldots,a_{e-1}$, distance $D$, scan limit $K$, skip parameter $S$

2.  Output: scores $f$ and absolute predecessor indices $p$

3.  $U\gets\infty$ if $S\leq0$, otherwise $U\gets\lfloor S/32\rfloor+1$

4.  For $i\gets b$ **to** $e-1$

5.  $f_i\gets\ell_i,\quad p_i\gets-1$

6.  $o\gets1,\quad u\gets0$

7.  While $o\leq K\land i-o\geq b$

8.  $B\gets$ the longest prefix of $(i-o,\ldots,i-o-31)$ with $j\geq b$, $i-j\leq K$, and $r_i-r_j\leq D$

9.  If $B=\varnothing$

10. **break**

11. End If

12. ForAll $j\in B$ in 32 lanes

13. $c_j\gets f_j+E(j,i)$ (invalid links use $-\infty$)

14. End For

15. $j^\star\gets
            \operatorname*{arg\,max}_{j\in B}(c_j,j)$ (larger $j$ breaks a score tie)

16. $c\gets c_{j^\star}$

17. If $c>f_i$

18. $f_i\gets c,\quad p_i\gets j^\star,\quad u\gets0$

19. ElsIf $S>0$

20. $u\gets u+1$

21. If $u\geq U$

22. **break**

23. End If

24. End If

25. If $|B|<32$

26. **break**

27. End If

28. $o\gets o+32$

29. End While

30. $\operatorname{await}(\mathrm{targetUpdates}_i)$ (make $f_i,p_i$ visible before target $i+1$)

31. End For

## Selecting primary and secondary chains

<a id="sec-chaining-primary-secondary"></a>

After chains are collected, we must note that several chains can naturally match the same part of a query in differents parts of the target. Sometimes there can be significant overlap between better chain and slightly worse one. For this reason minimap2 needs to classify chains into two groups: primary chains and secondary chains and it reports all primary chains but limit reported secondary chains to some set runtime limit \[[16](../bibliography/#ref-mmseqs2_issue_323)\]. Minimap considers chains from the highest score to the lowest:

- Chains are classified either as *primary* (best one) or *secondary*

- Specific chain becomes *secondary* when there’s existing *primary* that covers enough of the same query interval

- Otherise, chain is classified as *primary*

BioseqDB uses the same classification logic as minimap2. Below, in Algorithm [chaining primary secondary](../project_chaining/#alg-chaining-primary-secondary), I show how this classification is formally done:

- $c$ is a chain (iterated by decreasing score)

- $I_c=[q_c^{\mathrm{start}},q_c^{\mathrm{end}})$ is the query interval of chain $c$ (start of first anchor and end of last anchor). We will intersect these intervals to calcualte how many positions chain covers in the query.

- $s_c$ is chain score

- The value $u$ counts symbols (bases) of $c$ not covered by *any* accepted primary chain.

- Previously $p$ was specifically parent of an anchor. In this case it represents primary chain.

- $\pi(c)$ is the parent chain of $c$ (I defined previously parents for anchors, but this is extended notation to represent parent of first anchor in chain)

- We calculate (for current $c$ and all primary chains $p$) $\frac{|I_c\cap I_p|}{\min(|I_c|,|I_p|)}-\frac{\bigl|I_c\setminus\bigcup_{p\in P}I_p\bigr|}{\max(|I_c|,|I_p|)}$, where first part of the expression represents how much of the $c$ is already covered by $p$ and second part represents how much of original coverage we get. If this coverage factor is greater than overlap threshold $\lambda$, then we classify this chain $c$ as secondary. By default $\lambda=0.5$.

- There’s also technically length limit set to $L=2^{31}-1$ by default to prevent overflows, but it’s also configurable variable.

The algorithm for classification was taken from minimap2 paper and its source code \[[21](../bibliography/#ref-minimap2_paper)\]:

##### Primary and secondary chain selection in minimap2

<a id="alg-chaining-primary-secondary"></a>

1.  Input: chains $C$, query intervals $I_c$, scores $s_c$, limits $\lambda,L$

2.  Output: ordered primary list $P$ and parent $\pi(c)$ for every chain

3.  $P\gets[\,]$

4.  For $c\in C$ in decreasing order of $s_c$

5.  $u\gets\bigl|I_c\setminus\bigcup_{p\in P}I_p\bigr|,\quad \pi(c)\gets c$ (initially primary)

6.  For $p\in P$ (in decreasing score order)

7.  $o\gets|I_c\cap I_p|$

8.  $a\gets\min(|I_c|,|I_p|),\quad b\gets\max(|I_c|,|I_p|)$

9.  If $o>0\land o/a-u/b>\lambda\land u\leq L$

10. $\pi(c)\gets p$ (secondary alternative to $p$)

11. **break**

12. End If

13. End For

14. If $\pi(c)=c$

15. append $c$ to $P$

16. End If

17. End For

> [!NOTE]
> **Second classification**
>
> Similarily to minimap2, BioseqDB also calls the same calsification after base alignment is done (first classification is done after chaining). This is done, because base alignment can provide better scores for chains, so we can then report primary chains first to the user (there’s `bioseq.is_primary` to check if the chain was marked as primary - to differentiate between some hits with the same scores, but different coverage)
