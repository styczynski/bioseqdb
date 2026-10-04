---
title: "Minimizer extraction"
slug: project_minimizers
description: "Minimizer extraction"
order: 50
keywords: [BioseqDB, project]
draft: false
---

## Minimizer sketching

<a id="sec-minimizers-role"></a>

The first computational stage of the BioSeqDB aligner needs to compute $\operatorname{Seed}(X)$. A target seed becomes a key in an index and then query seed is looked up in that index, and each occurrence (so shared seed between query and target) becomes an anchor for the chaining stage described in Section [chaining purpose](../project_chaining/#sec-chaining-purpose).

We expect the seed set to be sparse enough to reduce index and lookup costs, but dense enough that related sequences have common seeds. As described before minimap2 and BioseqDB operate on minimizers. I didn’t specifically added support for "true" cannonical minimizers in a sense that I adhered to the minimap2 approach i.e taking minimum of hashes of k-mer and its reverse complement.

Formally, let $S=s_0s_1\ldots s_{n-1}$ be a nucleotide sequence, so the $k$-mer beginning at position $i$ (indexing starting from zero) is $$K_i=S[i,i+k).$$ For DNA, we now encode the forward and reverse-complement strands via some method, which is usually just a bit-packed representation where each symbol is encoded in a few bits: $$F_i=\operatorname{enc}(K_i),
  \qquad
  R_i=\operatorname{enc}(\overline{K_i}).$$ The canonical $k$-mer then uses the minimum of both encoded fragments (this is the same approach as in minimap2). $$C_i=\min(F_i,R_i),
  \qquad
  z_i=\mathbf{1}[R_i<F_i],$$ where $z_i$ saves a bit indicating which strand was selected. Now the hash function $H$ is applied to the chosen strand representation: $$x_i=(h_i,p_i,z_i),
  \qquad
  h_i=H(C_i).$$ The position $p_i$ may denote the first or last base of $K_i$ depending on the software. BioSeqDB’s minimap-compatible schemes save the last position of the $k$-mer, similarly to minimap2 \[[21](../bibliography/#ref-minimap2_paper)\].

For a window of $w$ consecutive candidates, $$W_j=(x_j,x_{j+1},\ldots,x_{j+w-1}),
  \qquad
  m_j=\min_{x\in W_j}h(x).$$ The expected density is theoretically $2/(w+1)$ minimizers per candidate position. \[[88](../bibliography/#ref-shur_minimum_density_2025)\], however practically it might vary. Minimap2 for example uses HPC (homopolymer-compressed) encoding which means that repeated letters are compressed so we don’t extract repeated minimizers for sections that are larege single-letter segments. In BioseqDB I do not support HPC compression.

There are also other practical and theoretical consideratiosn affecting minimizer extraction like different formulations of hashing functions or heuristics to avoid having very low minimizer density in specific repeated regions, but I won’t go into very specific details.

Figure [minimizer walkthrough](../project_minimizers/#fig-minimizer-walkthrough) shows what happens to one $k$-mer through hashing, selection of window that its belongs to and dedupliction of hashes.

<a id="fig-minimizer-walkthrough"></a>

![05 minimizers diagram](/static/figures/project/chapters-05-minimizers-tikz-01.svg)

*Minimizer extraction for $k=5$ and $w=4$ with illustrative hashes.*

The hashing function can affect heavily minimizer desnity:

<a id="fig-minimizer-density-traces"></a>

![05 minimizers diagram](/static/figures/project/chapters-05-minimizers-tikz-02.svg)

*Two minimizer traces for $w=4$. Thin brackets mark candidate windows, and orange dots mark their selected minima. The left trace reuses positions 3 and 7. The right trace selects a new position after each slide. Everything depends on numeric properties of used hashing scheme*

I wanted to emphasize that the topic of seed-and-extend is largely based on measuring practical stochastic properties of minimizers distribution and properties and in BioseqDB I only focus on the simplest approaches where in most cases I use the same hashing scheme as minimap2 to guarantee almost exact sensitivity and in one case discussed later incorporate use of rolling hash (nthash).

Figure [seed hash table](../project_minimizers/#fig-seed-hash-table) uses fixed four-base seeds on both sides. The symbols $h_1$, $h_2$, and $h_3$ are some computed hashes. The picture shows that in general we want to build one hash table, compare query to the hashes in the hash table and always filter out the most occuring hashes (as this means that this hash does not ultimately distinguish the part that we maching and will cause lots of false positives not contributing to the final result)

<a id="fig-seed-hash-table"></a>

![05 minimizers diagram](/static/figures/project/chapters-05-minimizers-tikz-03.svg)

*Reference-index construction and query lookup. Positions are seed starts in this example. A usable seed index stores some seed metadata like position and strand*

Frequent seeds can produce very large occurrence lists. BioSeqDB can exclude seeds whose target frequency exceeds a configured threshold before anchors are constructed. This is common practice in other software as well: minimap2 can remove most common minimizers for specific sequence by using percentage threshold (`--q-occ-frac` $\in[0,1]$)

## Methods of minimizer generation

### Scalar rescan

The most straightforward algorithm to select smallest hash in a given sliding window is to implement scalar rescan. We keep track of current minimum until it leaves the window. Then we need to do "rescan" part which calculates new minimum from scratch. Additionally we perform deduplication of adjacent hashes, so if the minimum is unchanged, we don’t emit new minimizer. However if there’s new equal hash on different position, then we treat that as a new minimizer.

This approach is implemented by minimap2.

<a id="fig-rewrite-minimap-walkthrough"></a>

![05 minimizers diagram](/static/figures/project/chapters-05-minimizers-tikz-04.svg)

*Minimizer extraction with illustrative $k=5$, $w=4$ hashes. Each row is one window of four adjacent $k$-mers. Orange marks the selected position. The hash value 2 remains selected while the window moves, so those windows do not require repeated output.*

Equal hashes at different positions are not duplicate records. We wanmt to keep both to guarantee higher minimizer density.

<a id="fig-rewrite-minimap-ties"></a>

![05 minimizers diagram](/static/figures/project/chapters-05-minimizers-tikz-05.svg)

*Why a hash value alone does not define the output. Both orange cells have hash 3, but they refer to different $k$-mer positions. Minimap2 keeps the rightmost equal position as its saved minimum and has explicit rules for writing other tied identities.*

Minimap2 stores the most recent $w$ candidates in a circular buffer It also stores the current minimum and the array slot containing it.

<a id="fig-rewrite-minimap-rescan"></a>

![05 minimizers diagram](/static/figures/project/chapters-05-minimizers-tikz-06.svg)

*The two costs (cheap/expensive) of one minimap2 step. Gray cells are already in the ring, blue is the entering candidate, and orange is the saved or newly found minimum. A normal cheap step performs one comparison, but when the saved minimum leaves, the algorithm reads the complete window again (rescan)*

Minimap2 stores kmers and apply hash function each time from scratch.

#### Scalar minimap2-style rescan

<a id="alg-rewrite-minimap-rescan"></a>

1.  Store $k$-mer binary representation and its reverse complement and update them each step. Calculate hash for both of them and take the minimum.

2.  Write this hash into the next slot of the $w$-slot circular buffer.

3.  If new hash is less than or equal to the saved minimum, emit new minimizer.

4.  Else, if the overwritten slot held the current minimum, emit new minimizer and scan all $w$ slots from oldest to newest, and save the rightmost record having the smallest hash as new minimum.

5.  At the end of the sequence, write the remaining saved minimum from buffer as final minimizers.

## A1: Naive SIMD rescan

<a id="sec-rewrite-naive-simd"></a>

Let’s assume we have scalar linear iteration over data. The first though is to unroll the loop so each element of a SIMD vector corresponds to one iteration. This approach however works only if each iteration step is independent. If we have any condition that requires knowledege of results of previous step the SIMD vector’s elements will be dependent on each other and won’t give us any advantage. The other approach would be to threat each element not as consecutive input items, but as separate entities handling different parts of the input. Each lane will handle separate parts of iteration, so we will avoid data dependency of vector’s elements on each other and replace it with the dependency between current state vector and previous state vector. In this document, I will define a SIMD lane as one numbered element of a vector register.

Each SIMD lane will load fragments of input data:

<a id="fig-rewrite-naive-simd-rescan"></a>

![05 minimizers diagram](/static/figures/project/chapters-05-minimizers-tikz-07.svg)

*Why direct SIMD rescan is costly. Each lane follows a different sequence piece. A nonzero expiry mask enters one shared loop over all $w$ vector slots. Masking protects lanes 1–3 from changing their answers, but it does not remove their loads and comparisons.*

Note that algorithm fundamentally stays the same, we only now divided input sequence into chunks and translated each operation (hashing, comparisons) into SIMD instructions:

### Naive SIMDified minimap2 rescan algorithm

<a id="alg-simd-minimap-rescan"></a>

1.  Split input sequence into chunks each corresponding to the lane of SIMD vector. Each lane handles different chunk (chunks overlap slightly to make sure we don’t omit any window)

2.  Store $k$-mer binary representations and its reverse complements and update them each step for each lane. Calculate hashes for both of them and take the minimum for each lane.

3.  Write all hashes for each laneinto the next slot of the $w$-slot circular buffer (this buffer now stores SIMD vectors instead of single hashes).

4.  If the new hash vector is less than equal to the minimum vector element-wise, extract corresponding elements and emit new minimizers.

5.  Else (new hash vector is greater then minimum vector on each position), if the overwritten slot held the current minimum for any lane, emit new minimizers and scan all $w$ slots from oldest to newest, and save the new minimums vector.

6.  At the end of the sequence, emit all remaining minimums as final minimizers.

As you can see this algorithm works well only if new hash values are greater than current minimum on each position. If not, we still have branching. This is problematic as vectorized algorithms works well if the branching is minimal and CPU pipeline isn’t penalized for wrong branch prediction. Even if we assume the predictor is perfect, we still do expensive rescan for each lane if at least one needs to be updated. Assume one lane needs a rescan with probability $1/w$, then a group of $L$ independent lanes enters the shared slow path with probability $$1-\left(1-\frac1w\right)^L.$$ For $w=10$ and $L=4$, this is about $34.4\%$. This means that we will need to duplicate work much more often than in scalar case.

## A2: Two-stack algorithm

<a id="sec-rewrite-sea-simd-minimizers"></a>

In different approach we can preserve prefix and suffix minima to calculate the window minima wrom both of them. SIMD-minimizers paper introduces this approach as well as usage of rolling hashes (nthash) instead of calculating hash each time from scratch. \[[36](../bibliography/#ref-groot_koerkamp_simd_minimizers_2025)\]

### One window minimum from two saved values

Split the hash stream into blocks of $w$ values. Within each block, let $P_i$ be the smallest value from the block start through position $i$, and let $Q_i$ be the smallest value from position $i$ through the block end. A window starting at $j$ and ending at $e=j+w-1$ crosses one block boundary and has minimum $$\min(Q_j,P_e).$$ The suffix values are recalculated in a fixed $w$-step loop each $w$ steps. This means that we still have "rescan" part, but it becames unconditional.

<a id="fig-two-stack-minimum"></a>

![05 minimizers diagram](/static/figures/project/chapters-05-minimizers-tikz-08.svg)

*Sliding minima from blockwise prefixes and suffixes. The two shaded crossing windows each use one suffix value from the starting block and one prefix value from the ending block.*

> [!NOTE]
> **Fixed-width SIMD vectors**
>
> The SIMD-Minimizers use fixed-vector size architecture, where the SIMD vector is set to be uint32 times 8. The split per lane is the same as discussed previously. On Neon architecture only 128-bit-wide register is available, so the 256 fixed vector generates separate store intructions.

<a id="fig-vector-minimizer-pipeline"></a>

![05 minimizers diagram](/static/figures/project/chapters-05-minimizers-tikz-09.svg)

*Vectorized minimizer extraction: Each chunk owns a disjoint range and reads enough context to complete its windows.*

Note that in this setup we can store full minimum vectors and later use matrix transposition to deduplicate them (in case that minimum stays the same over window): At first, one SIMD vector contains the selected positions for several far-apart streams at the same step. After several steps, the SIMD-Minimizers paper algorithm rearranges that output matrix so one row contains consecutive windows from one stream. Consecutive windows often select the same position. Once those positions are adjacent in a row, the paper compares neighbors and keeps only positions that changed. At the end duplicates for windows needs to be removed.

<a id="fig-transpose-dedup"></a>

![05 minimizers diagram](/static/figures/project/chapters-05-minimizers-tikz-10.svg)

*How transposition of the results is done to get consecutive positions in each vector. Then SIMD-Minimizers do deduplication for four vectors*

## A3: Chronological SIMD

<a id="sec-rewrite-v8"></a>

Looking at A1 we observed that the main problem was that far-apart sequence chunks can have different characterisistics which cause us to do lots of additional work. Instead of splitting sequence that way, we can work on just one part of neighbouring positions.

We will try to calculate minimums for all local windows i.e calculate result for $X[p, p+1, ..., p+k+w-1]$ (this is window of size $w$ for kmers of size $k$). Physically we only have $L$ SIMD lanes so we will call that a group and advance only by $L$ positions at the time (i.e from $X[p, p+1, ..., p+k+w-1]$ to $X[p+L, p+L+1, ..., p+k+w-1+L]$).

Potentially we can have more than one group per call. It offers some advantages discussed later. If the group number is higher, then we just execute their operations serially and perform result emission only after all groups execute.

The idea for one group is as follows:

1.  We load $L$ new sequence positions into $L$ SIMD lanes each one is shifted by $1$ using load instructions

2.  For each lane we compute minimum along with strand direction and information which position it is. We call that new candidates: $x_0,\ldots,x_{L-1}$

3.  We iterate through all candidates i.e $D\gets T\mathbin\Vert(x_0,\ldots,x_{L-1})$ where $T$ are previous $w-1$ candidates and calculate new minimums. This gives us $M$ which are $L$ minimums for entire parts of sequence: $$\begin{aligned}
          \{&\operatorname{Seed}(X[p,p+1,\ldots,p+k+w-1]),\\
            &\operatorname{Seed}(X[p+1,p+2,\ldots,p+k+w]),\ldots,\\
            &\operatorname{Seed}(X[p+L,p+L+1,\ldots,p+k+w+L])\}.
        \end{aligned}$$ Now this along last previous result $c^{*}$ forms $P$: $\mathbf P\gets(c^\star,M_0,\ldots,M_{L-2})$

4.  For each lane $\ell$ we compare $P_{\ell}$ with $x_{\ell}$. If $h(x_{\ell}) \leq P_{\ell}$ then $P_{\ell}$ is a new minimizer and we emit it. Otherwise if $\text{pos}(P_{\ell}) = \text{pos}(x_{\ell})$ then we see $P_{\ell}$ expired so we emit it.

5.  If the expiry happens then we go through all $w$ windows and check if their minimum has the same value but different position. This only requires checking $m_0, m_1, ..., m_{w-1}$. If yes then we do slow path: extract the ties in order (hence the name of the algorithm: chronological)

<a id="fig-rewrite-chronological-groups"></a>

![05 minimizers diagram](/static/figures/project/chapters-05-minimizers-tikz-11.svg)

*I illustrated here working sets and execution of the chronological adjacent-window algorithm. One group combines previous $w-1$ candidates with $L$ new candidates, then loads $w$ shifted vectors, and reduces them componentwise to the rightmost minima of $L$ adjacent windows. Orange cells represent selected minimums (candidates for potential minimizer emit). A solid orange cell is the rightmost equal minimum selected by the source’s $\leq$ comparison and a dashed cell is an earlier equal minimum.*

The figure [rewrite chronological groups](../project_minimizers/#fig-rewrite-chronological-groups) illustrates calculation of window minima for $L=4$ lanes: $x_0, x_1, ... x_{L-1}$. The entire algorithm looks as follows:

### One iteration: build $L$ $k$-mers and rescan $L$ adjacent windows

<a id="alg-rewrite-generic-chronological"></a>

1.  **Carry:** $C$ = last $k-1$ bases, $T$ = last $w-1$ candidates, $c^\star$ = preceding window minimum

2.  While the next $L$ candidate bases are available

3.  $U\gets$ next $L$ bases

4.  $Y\gets C\mathbin\Vert U$

5.  $\mathbf F,\mathbf R\gets\mathbf0$

6.  For $t=0,\ldots,k-1$

7.  $\mathbf z\gets\operatorname{load}_L(Y[t\,..\,t+L))$

8.  $\mathbf F\gets F\mathbin\Vert z$

9.  $\mathbf R\gets\overline{z}\mathbin\Vert R$

10. End For

11. ForAll lanes $\ell$

12. $a_\ell\gets H(\min(F_\ell,R_\ell))$

13. If $F_\ell=R_\ell$ or $a_\ell=h_{\max}$

14. $x_\ell\gets\infty$

15. Else

16. $x_\ell\gets\operatorname{pack}
              (a_\ell,\operatorname{position}_\ell,[R_\ell<F_\ell])$

17. End If

18. End For

19. $D\gets T\mathbin\Vert(x_0,\ldots,x_{L-1})$

20. $\mathbf M\gets\infty$

21. For $s=0,\ldots,w-1$

22. $\mathbf V\gets D[s\,..\,s+L)$

23. $\mathbf M\gets
            \operatorname{select}(h(\mathbf V)\leq h(\mathbf M),
                                  \mathbf V,\mathbf M)$

24. End For

25. Hence $M_\ell$ is the rightmost minimum of the complete window ending at $x_\ell$

26. $\mathbf P\gets(c^\star,M_0,\ldots,M_{L-2})$

27. ForAll lanes $\ell$

28. $e\gets\emptyset$

29. If $x_\ell=P_\ell$

30. $e\gets\operatorname{event}^{\mathrm{NEW}}(P_\ell)$

31. End If

32. If $\operatorname{pos}(P_\ell)=\operatorname{pos}(x_\ell)$

33. $e\gets\operatorname{event}^{\mathrm{EXPIRED}}(P_\ell)$ (expiration)

34. End If

35. If $e$ is <span class="smallcaps">Expired</span>

36. Check all $w$ windows for an equal hash on a different position

37. $e\gets\operatorname{event}^{\mathrm{TIE}}(P_\ell)$

38. End If

39. If $e$ is <span class="smallcaps">Tie</span>

40. Reread the $w$ records only to construct ordered output (slow path)

41. End If

42. End For

43. $C\gets Y[L\,..\,L+k-1)$

44. $T\gets D[L\,..\,L+w-1)$

45. $c^\star\gets M_{L-1}$

46. End While

47. If $0<q<L$ bases remain

48. Run the same body once with $q$ active lanes and sentinels elsewhere

49. Both loops still execute $k$ and $w$ steps

50. $C\gets Y[q\,..\,q+k-1)$, $T\gets D[q\,..\,q+w-1)$, $c^\star\gets M_{q-1}$

51. End If

## Bioseq prototype

We established different approaches to calculate minimizers in previous sections.Now we want to discuss practical considerations when it comes to implementing those algorithms. The work started by exploring minimap2 source code and understanding its behaviour. The first step was to port minimap2 code into modern C++. This was required to have a stable code where we could test and benchmark different algorithms. I encountered several key problems that lagged implementation by considerable number of months:

1.  Minimap2 code uses direct C arrays access for lots of different data structures. Minimizers are encoded as uint128 type represented by a struct with two uint64 fields with abstract filed names. Circular buffers are represented by single raw pointer and require macros for any operations. The encoding and semantics of fields can sometimes change even though underlying uint128 representation stays the same (it’s the same field name). This complicates any manual static analysis of the code I conducted.

2.  Minimap2 uses raw instrinsics for SIMD operations which makes translation to generic SIMD very tedious.

3.  Some parts of minimap2 hash logic are undocumented either in official paper or in-code.

The porting involved several iterations over the minimap2 source to understand how index is split, how parallelization works and how exactly minimizers are represented. After achieving runnable pipeline (with seeding, chaining, alignment and estimator) I started to work specifically on the minimizer extraction part as it’s first in the pipeline.

| Label  | Main changeWhat it means for the minimizer algorithm               |
|:-------|:-------------------------------------------------------------------|
| V1–V6  | naive SIMD rescan approach equivalent to A1                        |
| V7–V8  | hybrid algorithm A1 where we use two-stack for minimum calculation |
| V8.5   | defer minimap2 hashing to later stages                             |
| V9-V10 | hand-written input and matrix loop                                 |
| V11    | two independent Highway lane groups                                |
| V12    | force the per-base body inline                                     |
| V13    | fixes of estimator                                                 |

*History of the first prototypes*

<a id="tab-rewrite-version-history"></a>

## Seeding prototype V1–V6: Naive SIMD rescan

<a id="sec-v6-rescan"></a>

The first approach (apart from direct minimap2 scalar rescan already existing in the ported code) was implementation of A1 SIMD rescan.

As correctly predicted our benchmark (Table [v6 phase](../project_minimizers/#tab-v6-phase)) showed that the scalar `mm_sketch` took 12.5 ms on a 903 kbp pair, while the “vectorised” `mm_sketch_simd` took 24 ms – a $2\times$ slowdown. This was caused by the fact that any minimum going outside the window causes rescan of all lanes.

| Phase                   | scalar (ms) | V6 SIMD (ms) | V6 over scalar |
|:------------------------|------------:|-------------:|---------------:|
| `mm_sketch`             |       12.50 |        14.94 |      $+19\,\%$ |
| `mm_sketch_simd`        |           0 |        23.88 |              — |
| `target_index_build`    |        16.0 |         14.3 |      $-11\,\%$ |
| `collect_seed_hits`     |         6.5 |         11.0 |      $+69\,\%$ |
| `mg_lchain_dp`          |        16.0 |          0.0 |   — (replaced) |
| `mg_lchain_simd`        |         0.0 |          8.9 |              — |
| `mm_est_err`            |         0.7 |          7.2 |     $+929\,\%$ |
| `mm_align_skel.`        |        87.5 |         86.4 |       $-1\,\%$ |
| Pipeline total per pair |       158.4 |        170.7 |       $+8\,\%$ |

*Per-stange timing breakdown measured by me. The naive SIMD path (V6) is $+8\,\%$ *slower* than scalar despite four lanes of parallelism*

<a id="tab-v6-phase"></a>

The regressions on `collect_seed_hits` and `mm_est_err` were caused by SIMD chain algorithms producing a "slightly" different anchor distribution than scalar. This problem is not directly relevant to the minimizer extraction for now, but I mention it later.

## Seeding prototype V8: Implementing A2 algorithm

<a id="sec-v8-two-stack"></a>

Algorithm [two stack](../project_minimizers/#alg-two-stack) shows pseudocode for algorithm (A2) inspired by SIMD-Minimizers paper \[[36](../bibliography/#ref-groot_koerkamp_simd_minimizers_2025)\].

### V8 `collect_minimizers_two_stack_simd` (per-step body)

<a id="alg-two-stack"></a>

1.  **state**: $\textit{prefix\_min}[L]$, $\textit{ring}[w][L]$, $\textit{step}$, $\textit{ring\_idx}$

2.  $\textit{hash} \gets \verb|rolling_kmer_hash|.\textsc{Next}(d, c_\text{in}, c_\text{out})$

3.  $\textit{strand}, \textit{palindrome} \gets \textsc{StrandMask}(\textit{hash})$

4.  $\textit{valid} \gets \neg\textit{palindrome}$

5.  $\textit{hs} \gets \textsc{IfThenElse}(\textit{strand}, \textit{hash}\,|\,1, \textit{hash}\,\&\,{\sim}1)$ (Set strand bit)

6.  $\textit{elem} \gets \textsc{IfThenElse}(\textit{valid}, ((\textit{hash}\,\&\,\textbf{0xffff\_0000})\,|\,\textit{pos\_local}), \textbf{0xffff\_ffff})$

7.  $\textit{ring}[\textit{ring\_idx}] \gets \textit{elem}$ (single ring buffer slot per lane)

8.  $\textit{better} \gets \textit{elem} < \textit{prefix\_min}$

9.  $\textit{prefix\_min} \gets \textsc{IfThenElse}(\textit{better}, \textit{elem}, \textit{prefix\_min})$

10. $\textit{ring\_idx} \mathrel{+}= 1$, $\textit{step} \mathrel{+}= 1$, $\textit{pos\_local} \mathrel{+}= 1$

11. If $\textit{ring\_idx} = w$ (rebuild suffix)

12. $\textit{ring}[i] \gets \min(\textit{ring}[i..w-1])$ **for all** $i$ ($O(w)$ iteration)

13. $\textit{prefix\_min} \gets \textit{elem}$

14. $\textit{ring\_idx} \gets 0$

15. End If

16. $\textit{window\_min} \gets \min(\textit{prefix\_min}, \textit{ring}[\textit{ring\_idx}])$

17. If $\textit{step} \ge k+w-1$ **and** $\textit{window\_min}.\text{pos} \ne \textit{last\_emit\_pos}$

18. SaveWindowMinimum(*window_min*)

19. $\textit{last\_emit\_pos} \gets \textit{window\_min}.\text{pos}$

20. End If

The V8 numbers were already encouraging: `mm_sketch_simd` dropped from V6’s 24 ms to 17 ms at 903 kbp – a 30 % reduction on the minimizer-only phase, which translated into the SIMD pipeline being only 1.4 % behind scalar overall (down from V6’s 8 % behind).

Note that in this prototype we weren’t using nthash rolling hash, but the original minimap2 hash function.

### Seeding prototype V8.5: Deferring hash calculation

<a id="sec-v8-defer-scramble"></a>

Till this time we were using minimap2 hash function for minimizers2, but the observation was that we can use cheaper hash function for ordering values inside the inner loop and then hash the emitted values. If the hash function used will be significantly cheaper we can have proper speedup. The cheap inner hash needs only one property: be a bijection on the $2k$-bit kmer space, so that $\textit{lex}(\textit{kmer}_1) = \textit{lex}(\textit{kmer}_2) \iff
\textit{kmer}_1 = \textit{kmer}_2$. We use the “lexicographic-canonical hash” – the bit-packed $\min(\textit{forward}, \textit{rev\_compl})$ but multiplied by Knuth’s golden-ratio constant $0x9E3779B9$ \[[87](../bibliography/#ref-knuth_taocp_v3)\].

The arithmetic of the saving: $$\underbrace{2.25 \cdot 10^5}_{\text{iters / Mbp}}
\;\times\;
\underbrace{(7\,\text{stages} - 2\,\text{stages})}_{\text{ops saved per iter}}
\;\approx\;
1.1 \cdot 10^6\,\text{SIMD ops saved}$$

The measured saving was $\sim 5$ ms per 1 Mbp pair on `mm_sketch_simd`, taking the cost from V8’s 17 ms down to 12 ms.

## Seeding prototype V9–V11: Stream interleaving and forced inline

<a id="sec-v11-v12"></a>

A close reading of the assembly created by Apple Clang LLVM revealed inner loop has unfriendly data dependency chain:

`shift_fwd `$\to$` or `$\to$` and `$\to$` Min(fwd, rc) `$\to$` Mul(lex, golden)`.

The Apple M1 has *four* 128-bit NEON pipes that issue independently, so a single 4-lane stream wastes three out of four pipes for any given cycle of the dependency chain. This feature is called ILP (Instruction Level Parallelism).

This is limitation that wasn’t observed in the original SIMD minimizers paper as Rust code used fixed-size SIMD vectors, where we use adaptive vector size equal to possible physical register size. The possible optimisation here is to keep *two* independent 4-lane streams in flight simultaneously and interleave their operations in source order:

```cpp
shift_fwd_lo, shift_fwd_hi = ShiftLeft(state_fwd_lo / hi)
state_fwd_lo, state_fwd_hi = And(Or(shift_fwd_, bp_), kmer_mask)
state_rc_lo,  state_rc_hi  = Or(ShiftRight(state_rc_), new_high_)
lex_lo,       lex_hi       = Min(state_fwd_, state_rc_)
hash_lo,      hash_hi      = And(Mul(lex_, golden), hash_mask)
elem_lo,      elem_hi      = IfThenElse(valid_, hash_|pos_, max)
```

I chose 2 streams, rather than 4 execution streams, because the per-stream state plus the shared ring buffer for $w=10$ requires $\sim 26$ NEON registers per stream. If I wanted to double the number of streams, it could make the ring buffer spill to the stack, losing the ILP benefit.

For the test case of length 903 kbp, as seen in Figure [phase evolution](../project_minimizers/#fig-phase-evolution), I got a $\sim 4$ ms reduction in the runtime of the `mm_sketch_simd` function (from 12 ms to 8 ms).

### Seeding prototype V12: Forced inline

V11 performance was better than for earlier prototypes. However, I wasn’t satisfied. The measured speedup was less than the $2\times$ that I could optimistically expect from ILP. I spent the next weeks investigating the performance gap here and finally discovered the issue by manually inspecting the disassembled binary code. I used `xcrun llvm-objdump -d build/bioseq/bioseq_bench` on the V11 prototype to find the underlying problem. I shared this case to emphasize how the development of BioseqDB was constantly blocked by implementation details, C++ API details and hardware problems. This is only one out of thousands of issues I needed to address during development of BioseqDB:

```asm
ldp q0, q1, [sp, #0x150]       // load bp_row from stack
ushr.4s v2, v1, #0x2           // extract per-BP value
movi.4s v1, #0x3
stp q0, q2, [sp, #0x150]       // store back
and.16b v0, v0, v1
and.16b v1, v2, v1
add x0, sp, #0x5, lsl #12      // prepare lambda's captured-this
add x0, x0, #0x770
movi.2d v2, #0xffffffffffffffff
add x1, x22, #0x8
bl 0x1002acca4 <step_one_bp_pair lambda>   // the offending call!
```

Despite me using `[[gnu::always_inline]]`, Clang translated the lambda to a function call, so each call needs to "pay" for:

- Function-call prologue/epilogue (`stp x28-x30 [sp,...]`).

- Captured state (all referenced variables outside lambda) - this is very promblematic because each call needs to load state from stack and store it back

- There’s no ILP happening as each lambda call is its own function frame

Figure [phase evolution](../project_minimizers/#fig-phase-evolution) shows full evolution of my prototypes. I thought it would be interesting to showcase how they behaved. The scalar part involves unavoidable SIMD overhead of collecting results into map, scalar scramble hashing (minimap2 hash) and saving output in the bit structure.

<a id="fig-phase-evolution"></a>

![05 minimizers diagram](/static/figures/project/chapters-05-minimizers-python-01.svg)

*Evolution of the minimizer-phase cost (`mm_sketch` wrapper plus `mm_sketch_simd` inner) at 903 kbp across the iteration rounds discussed inuujj §[v6 rescan](../project_minimizers/#sec-v6-rescan)–§[v11 v12](../project_minimizers/#sec-v11-v12). Numbers are means of three runs at `-O3 -DNDEBUG`. V8 is the two-stack port, V8.5 defers the minimap2 hash to emit time, V11 adds dual-stream interleaving, and V12 forces inlining of the per-step body via a free-function template. The dashed horizontal line is the scalar `mm_sketch` baseline. Even with some optimisations still SIMD routine is slower than scalar execution.*

### Seeding prototype V13: Existmator fix

<a id="sec-minimizer-divergence-history"></a>

For each chain, minimap2 estimates the per-base sequence divergence from a Poisson model on the minimizer-set intersection \[[21](../bibliography/#ref-minimap2_paper)\]: $$\hat{\epsilon} = \frac{1}{k} \log \frac{n}{m}$$ where $n$ is the number of minimizers that are part of chains and $m$ is the number of all query minimizers between first match and last match. Computing $n$ requires *knowing which positions are query minimizers*, which is a set membership test against the query’s minimizer-position set.

The divergence estimator is used to measure the quality of the seeding. If the divergence is too high, the seeding is not good and the chain is not valid.

When porting minimap2 code we had hidden one important detail visible on table [v6 phase](../project_minimizers/#tab-v6-phase). The `mm_est_err` section had significant 9-times slowdown. This is because we are using separate chunks for each lane, but the output minimizers were stored in the buffer. Minimap2 normally receives its `mini_pos` array already sorted (because of how scalar loop works). This means that in our case we did the most naive approach:

1.  Perform `std::sort` on the array to sort minimizers by positions

2.  Perform `std::lower_bound` on the array to test membership

This caused severe slowdown, which we mistakenly attributed to seeding algorithm itself. More on divergence is described in the postprocessing sections.

Figure [phase stack v13](../project_minimizers/#fig-phase-stack-v13) stacks the per-phase cost at 903 kbp for V12-pre versus V13. The two large gains are `mm_est_err` (from 7.88 ms to 0.85 ms) and the V12 `mm_sketch` stack (from 14.99 ms to 9.23 ms, already covered in previous section.

<a id="fig-phase-stack-v13"></a>

![05 minimizers diagram](/static/figures/project/chapters-05-minimizers-python-02.svg)

*Per-phase composition of the per-pair pipeline at 903 kbp we can see that V13 reduced exploded divergence phase*

### Seeding prototype V14: directly use nthash

So far we utilised minimap2 hash function directly. One other approach was replacing this hash function directly. This should have given us measurable speedup. For nthash we can construct LUT (lookup table) accessed via SIMD instruction.

- `mm_sketch_simd` cost *increased* by 2 ms (the table lookup used NEON `tbl.16b` instruction and hashing required two rotates, and some numbers XORs per stream per sequence symbol).

- `collect_seed_hits` dropped by *0.4 ms* (this isn’t significant enough).

My investigation pointed me to the fact that I was trying to optimise incorrect region of computations. The benchmark during all experiments evolved and improved with each iteration. This time the measurement didn’t report that most of the time was spent on inserting minimizers into the index.

The gap in performance didn’t come from the minimizer hash distribution (or hashing speed itself) but from the usage of `std::unordered_map`. The prototype’s target index used STL’s default hash table, while the C minimap2 path uses an open-addressing `khash_t<uint64_t>`. This shows another problem with directly porting minimap2 into C++. `std::unordered_map` is known problem with standard library \[[13](../bibliography/#ref-stack_overflow_answer_cpp_maps)\] that we overlooked during implementation similarily to how `std::sort` was used for divergence estimation. The cleanest solution here was implementing indexing structures as flat hash maps directly.

### Seeding prototype V15: Smaller minimizer space

<a id="sec-v15-kmer-collapse-binary"></a>

In all of the experiments we used adjustable minimizer represetations with hash width of 16 bits (minimizer representation is explained in separate section). However smaller represetations mean more efficient algorithms. Existing iterations of seeding algorithm all used adjustable SIMD vector widths provided by C++ Highway library. This means that narrower input type will automtically increase number of available lanes $L$ thus increasing potential performance in all algorithms. However having smaller type means that we need to either get rid of strand information (1 bit) or use narrower position representation (this is not always possible) or use narrower hash representation. We conducted early experiment testing V12 prototype with narrower hash of 14 bits.

|  | V12 (pre) | V12 (post-Goal-2) | delta |
|:---|---:|---:|:---|
| mm_sketch_simd | 9.0 ms | 9.5 ms | +0.5 ms (insignificant change) |
| target_index_build | 24.7 ms | 4.9 ms | $-19.8$ ms ($\sim85\%$ fewer keys) |
| collect_seed_hits | 12.3 ms | 842.0 ms | +830 ms (70x regression) |
| mg_lchain_simd | 8.9 ms | 299.9 ms | +291 ms (34x regression) |
| mm_align_skeleton | 87.2 ms | 4811.0 ms | +4724 ms (56x regression) |

> [!WARNING]
> **Hash-space collision amplification**
>
> The kmer space stored in `target_index` hash buckets collapsed to $\sim 16\,000$ distinct keys for $168\,000$ query minimizers. Each query had $\sim 10$ collisions per bucket, meaning the chain-anchor count exploded, and the alignment had to walk through all of them. This means that we sped up index lookups significantly (less memory accesses required), but at the same time the more costly parts of the pipeline get more false positives that need to be verified.

### Seeding prototypes summary

The combination of the V12 inlinening fixes, the V13 estimator fix and replacing `boost::unordered_map` (marked as ${V13}^{\ast}$) yielded finally observable improvements. We didn’t want to implement new hashing scheme at this time, only see the impact of using different hash storage.

<a id="fig-speedup-summary"></a>

![05 minimizers diagram](/static/figures/project/chapters-05-minimizers-python-03.svg)

*Wall-clock delta versus scalar for the three SIMD configurations, expressed as percent (negative = faster). ${V13}^{\ast}$ now performs better than scalar, however this is nowhere near results reported in SIMD Minimizers paper.*

The summary table:

| Length  | scalar (ms) | V13 (ms) | V13 vs scalar | V13 vs V6 SIMD |
|:--------|------------:|---------:|--------------:|---------------:|
| 47 kbp  |       14.56 |    13.78 |    $-5.4\,\%$ |     $-9.4\,\%$ |
| 49 kbp  |       14.94 |    14.02 |    $-6.2\,\%$ |    $-11.8\,\%$ |
| 52 kbp  |       15.09 |    13.82 |    $-8.4\,\%$ |    $-13.9\,\%$ |
| 903 kbp |      188.93 |   172.38 |    $-8.8\,\%$ |    $-14.5\,\%$ |
| 5.0 Mbp |      1171.0 |   1113.3 |    $-4.9\,\%$ |     $-7.8\,\%$ |

*Total measured time for alignment (mean of 3 runs from `-O3 -DNDEBUG`*

<a id="tab-final-numbers"></a>

## GPU acceleration for seeding

G3SA  \[[44](../bibliography/#ref-g3sa_paper)\] provides implementation of multiple useful kernels including one specific to minimizer generation phase.<sup><a href="#project-note-1">1</a></sup> [G3SA seeding kernel · `mm_sketch_device 83:1`](https://github.com/sunwookim028/G3SA/blob/main/minimap2/src/seeding_kernel.cu#L83)

G3SA implements the *A3 adjacent-window computation graph*, in CUDA SIMT. Let $L=32$ be the number of CUDA threads. For group $r\in\{0,1,2,3\}$ and thread $t\in[0,L)$, define $$j^\star_{r,t}
    = \max\underset{0\leq j<w}\{{\arg\min}\}\;
        h(C_{rL+t+j}),
  \qquad
  W_{rL+t}=C_{rL+t+j^\star_{r,t}} .$$ The outer maximum keeps the rightmost record among equal minimum hashes. The vector $(W_{rL},\ldots,W_{rL+L-1})$ is the componentwise reduction of $w$ shifted candidate vectors. This is the same exact computation as Algorithm [rewrite generic chronological](../project_minimizers/#alg-rewrite-generic-chronological), but CUDA threads replace SIMD lanes. One difference is that there’s no some global state preserved so it’s need to be recomputed at initialisation.

The computation is divided according to the following geometry: $$L=32,\qquad G=4,\qquad
  n_{\rm owned}=LG=128,\qquad n_{\rm shared}=L(G+1)=160.$$ We have 128 blocks of 32 threads. Block $b$ handles reads $b,b+128,b+256,\ldots$, one read at a time. For each read it advances by $n_{\rm owned}=128$ window starts. In one advance, thread $t$ attempts candidate slots $t,32+t,\ldots,128+t$ and then reduces the four windows beginning at $t,32+t,64+t,$ and $96+t$. Figure [g3sa minimizer kernel](../project_minimizers/#fig-g3sa-minimizer-kernel) shows this transpose between candidate construction and window reduction.

<a id="fig-g3sa-minimizer-kernel"></a>

![05 minimizers diagram](/static/figures/project/chapters-05-minimizers-tikz-12.svg)

*G3SA algorithm reconstructed from the CUDA code. This is similar to A3, but has specific work split per thread/warp (standrd CUDA SIMT model)*

## BioseqDB GPU seeding

<a id="sec-vulkan-minimizer-scheduling"></a>

The BioseqDB minimizer generation uses minimap2’s scalar rescan approach, but ported to GPU. The input is divided into tiles, where each invocation handles just one tile (when talking about GPU shaders, Vulkan’s workgroup correspond to CUDA thread block, while subgroup is a direct equivalent of CUDA warp, which are different concepts of dividing work).

In algorithms below I assume $Q$ in entire queries batch (for short queries combines multiple queries into one) and let $\mathcal T=(t_0,\ldots,t_{T-1})$ be the list of tiles to be processed. The processing is done in $G=\min(\lceil T/64\rceil,G_{\max})$ kernel workgroups (where $G_{\max}$ is the workgroup limit defined by device).

<a id="fig-vulkan-tile-dispatch"></a>

![05 minimizers diagram](/static/figures/project/chapters-05-minimizers-tikz-13.svg)

*How tiles are processed in the GPU algorithm. Each of the $G$ workgroups has 64 invocations and one invocation $g$ processes one tile at a time, then advances by $64\mathbin{\cdot}G$*

When we collect results we need to either: have fixed memory offsets per tile and perform another host pass to extract results or we can do lazy approach where we execute pipeline twice. First we use count mode to caculate where to place results and then do a second pass to actually write them. This makes pipeline do twice as much work, but avoids any additional host pass and we can directly use output results. The limtiations of our work is that we didn’t measure the difference between these approaches or between G3SA approach.

The core part is OrderedSketchTile which runs the rescan algorithm in count mode and writes Count. The $\text{Count}[j]$ is the number of output records for tile $j$. PrefixScanBlock reads Count. Each of its $B$ single-invocation workgroups scans at most $S=256$ entries. It writes local offsets into Offset and one total per block into Block. Two stages (calls) of PrefixScanAdd are used to calculate global offsets.

The positive value $A$ is the pessimistic output capacity planned by the caller using heuristic. <span class="smallcaps">HostAndDevice</span> creates an $A$-record device output before count. If the actual pipeline overflows, then we return error.

### Vulkan scheduling in BioseqDB

<a id="alg-vulkan-sketch-host-schedule"></a>

1.  Input: packed query batch $Q$, $k$, $w$, device group limit $G_{\max}$

2.  Input: positive planned output capacity $A$

3.  Input: policy $P\in\{\textsc{HostAndDevice},\textsc{DeviceOnly}\}$

4.  $\mathcal T\gets\texttt{PlanTiles}(Q,k,w,G_{\max})$

5.  $T\gets|\mathcal T|$

6.  return without a GPU dispatch **if** $T=0$

7.  $G\gets\min(\lceil T/64\rceil,G_{\max}),\quad S\gets256,
             \quad B\gets\lceil T/S\rceil$

8.  `PrepareDeviceInputs`($Q,\mathcal T$)

9.  allocate device buffers *Count*$[T]$, *Offset*$[T]$, and *Block*$[B]$

10. **if** $P=\textsc{HostAndDevice}$ **then** $\mathit{Output}\gets\texttt{HostCreateDeviceOutput}(A)$

11. `BeginCommandBuffer`()

12. `RecordHostToGPUBarrier`()

13. $\mathit{countTask}\gets
             \texttt{RecordOrderedSketchTile}(\textsc{Count},Q,\mathcal T,k,w,G,
                                            \mathit{Count})$

14. `AwaitGPU`($\mathit{countTask}$)

15. $\mathit{localTask}\gets
             \texttt{RecordPrefixScanBlock}(\mathit{Count},\mathit{Offset},
                                          \mathit{Block},T,S,B)$

16. `AwaitGPU`($\mathit{localTask}$)

17. $\mathit{blockTask}\gets
             \texttt{RecordPrefixScanAdd}(0,\mathit{Block},B)$

18. `AwaitGPU`($\mathit{blockTask}$)

19. $\mathit{offsetTask}\gets
             \texttt{RecordPrefixScanAdd}(1,\mathit{Offset},\mathit{Block},T,S)$

20. If $P=\textsc{DeviceOnly}$

21. `SubmitAndHostAwait`($\mathit{offsetTask}$)

22. $N_{\rm out}\gets
               \mathit{Offset}[T-1]+\mathit{Count}[T-1]$

23. return overflow **if** $N_{\rm out}>A$

24. $\mathit{Output}\gets\texttt{HostCreateDeviceOutput}(N_{\rm out})$

25. `BeginCommandBuffer`()

26. `RecordHostToGPUBarrier`()

27. End If

28. `AwaitGPU`($\mathit{offsetTask}$)

29. $\mathit{emitTask}\gets
             \texttt{RecordOrderedSketchTile}(\textsc{Emit},Q,\mathcal T,k,w,G,
                                            \mathit{Offset},\mathit{Output})$

30. `SubmitAndHostAwait`($\mathit{emitTask}$)

31. If $P=\textsc{HostAndDevice}$

32. $N_{\rm out}\gets
               \mathit{Offset}[T-1]+\mathit{Count}[T-1]$

33. return overflow **if** $N_{\rm out}>A$

34. $\mathit{HostOutput}\gets
               \texttt{CopyMapped}(\mathit{Output}[0\ldots N_{\rm out}))$

35. Else

36. $\mathit{HostOutput}\gets\varnothing$

37. End If

38. return $(\mathit{Output},\mathit{HostOutput},N_{\rm out})$

### `RecordOrderedSketchTile` call

<a id="alg-vulkan-ordered-invocation"></a>

1.  Procedure `RecordOrderedSketchTile`($g,\mathit{mode}$)

2.  Input: global invocation ID $0\leq g<64\mathbin{\cdot}G$

3.  Input: dispatch-wide $\mathit{mode}\in\{\textsc{Count},\textsc{Emit}\}$

4.  Input: bound $Q,\mathcal T,T,k,w,G,\mathit{Count}$ from Algorithm [vulkan sketch host schedule](../project_minimizers/#alg-vulkan-sketch-host-schedule)

5.  Input: bound *Offset* and *Output* in <span class="smallcaps">Emit</span> mode

6.  $j\gets g$

7.  While $j<T$

8.  $\mathit{localCount}\gets0$

9.  ForAll record $e$ streamed by `RunScalarRescanTile`($Q,\mathcal T[j],k,w$)

10. If $\mathit{mode}=\textsc{Emit}$

11. $\mathit{Output}[\mathit{Offset}[j]
                     +\mathit{localCount}]\gets e$

12. End If

13. $\mathit{localCount}\gets\mathit{localCount}+1$

14. End For

15. If $\mathit{mode}=\textsc{Count}$

16. $\mathit{Count}[j]\gets\mathit{localCount}$

17. End If

18. $j\gets j+64\mathbin{\cdot}G$

19. End While

20. End Procedure

## Packed minimizer representation

<a id="sec-packed-minimizer-representation"></a>

When running BioseqDB minimizer generation we need to have a way to store the results in a format that can be resused in other stages of the pipeline.

Minimap2’s `mm_sketch` emits each minimizer in an `mm128_t`, two `uint64_t` words named `x` and `y`. The C++26 declaration below shows only interpretation of the fields, while minimap2’s itself uses binary shifts and masks to set and extract specific values (which made its manual code analysis very challenging).

<a id="lst-minimap-minimizer-cpp-decl"></a>

```cpp
struct MinimapSketchFields {
  std::uint64_t kmer_hash : 56; // logical $k$-mer hash
  std::uint64_t span      :  8; // input-base span
  std::uint64_t strand    :  1; // canonical orientation bit
  std::uint64_t last_pos  : 31; // zero-based final-base position
  std::uint64_t rid       : 32; // reference/query-segment ID
};
```

Logical fields packed into a minimap2 minimizer.

Minimap2’s C code construction is done via: $$\begin{aligned}
  x&=(\mathit{hash}\ll 8)\mathbin{|}\mathit{span},\\
  y&=(\mathit{rid}\ll 32)\mathbin{|}
     (\mathit{last\_pos}\ll 1)\mathbin{|}\mathit{strand}.
\end{aligned}$$

> [!NOTE]
> **Homopolymer compression**
>
> Normally span equals $k$ (or is $< k$ for first $k-1$ bases). Minimap2 suppors homopolymer compression (HPC) togglable by command line option `-H` which collapses strings of one repeating character into single character occurence. If HPC is enabled, span can have values $0 \leq span \leq 255$. If span is increased beyond 255 (there are 255 same characters in the input sequence next to each other), then minimizer is completely abandoned until new symbol is found.

<a id="info-minimap-hpc"></a>

The `rid` value identifies a reference during indexing and a query segment during query sketching. Note that the ID itself isn’t useful during seeding, chaining or alignment, it’s only useful at the end when reporting results.

When porting the core of minimap2 algorithms we observed the following facts:

1.  The span contains no useful information as HPC is disabled by default and we don’t have to support it at this time. For HPC disabled it can only carry information if the k-mer was at the start of the sequence (this can be checked via *last_pos* field)

2.  *rid* is completely useless during most of the pipeline

3.  *last_pos* which is the end position of the k-mer is only 31 bits which introduces requirement to split long sequences before using minimap2.

4.  *kmer_hash* is wide, but using more bits for hash than binary representation of k-mer is not giving any advantage (for example for DNA4 alpohabet if we encode each symbol in 2 bits then k=15 requires 30 bits for k-mer representation. Hashing 30-bits into 56-bit wide hash doesn’t give any advantage as huge part of hashing space won’t be used)

BioSeqDB’s minimizer producers and collectors use `minimizer_representation<L,K>`. Every form stores the hash, zero-based final position, and strand.

The longest form still uses *span* set to $k$, but it’s not used for anything other than just padding of fields. Here $L$ is the number of position bits and $K$ is the $k$-mer length. This $L$ is unrelated to the SIMD lane count used earlier in the chapter.

The type first reserves $$H=\min(2K,56)$$ hash bits. Let $R=H+L+1$, where the last $1$ is just the strand bit. The type uses the first width $W\in\{16,32,64\}$ for which $R\leq W$. Every bit above the position and strand fields then belongs to the hash, so the stored hash field has $W-L-1$ bits. If $R>64$, the record uses two 64-bit values: one word contains a 56-bit hash and an 8-bit span, while the other word contains the position, the strand bit (and any unused high bits).

<a id="fig-minimizer-representation-layout"></a>

![05 minimizers diagram](/static/figures/project/chapters-05-minimizers-tikz-14.svg)

*Bit fields in four `minimizer_representation<L,K>` instantiations, shown from high bits on the left to low bits on the right. Blue is the hash, orange is the zero-based final position, green is the one-bit strand, and purple is the span in the two-word form. The highlighted $\langle28,15\rangle$ record is the default 64-bit sketch-data form.*

The $L$-bit field represents final positions from zero through $2^L-1$, so it can describe a local sequence span of at most $2^L$ bases. For $K=15$, values through $L=33$ still fit in 64 bits, while $L=34$ requires the two-word form. The $\langle28,15\rangle$ form has a 35-bit hash field, but its minimap2 integer hash occupies only 30 bits.

Currently the main parameter that drives used representation width is the number of position bits $L$ (this is compared against *kmer_size_buckets* array). Theoretically the template can produce a two-byte record, but this bucket isn’t suppored at this moment (such small width make’s it problematic to adjust as we need to have very short hash, which isn’t ideal as shorter k-mers are more prone to collisions or limit the size of sequence to less than 256, which would be very specific optimisation) The $\langle2,2\rangle$ on the figure is just the demonstrative example to show that template is supported even in very degenerated edge cases (we don’t actually use special form for sequences with less than 4 bases as it would make very little sense).

## BioseqDB CPU minimizer algorithms

<a id="sec-configured-cpu-minimizers"></a>

The PostgreSQL setting `bioseq.aligner.minimizer.algorithm` chooses the minimizer implementation. Its current default is `bounded_nthash_simd`. Table [current cpu minimizers](../project_minimizers/#tab-current-cpu-minimizers) maps the four CPU values to the algorithm families described earlier. Additionally we allow dispatching Vulkan kernel.

| **Setting value** | **Current CPU route** |
|:---|:---|
| `scalar` | Scalar rescan with the minimap2 integer hash. |
| `simd_minimap2_reference` | A1 with the minimap2 integer hash for DNA4 and a registered rank-based SIMD route for DNA5. |
| `bounded_nthash_simd` | A2 with 32-bit ntHash for DNA4 and DNA5. A specialized $k=15,w=10$ kernel accompanies the generic runtime kernel. |
| `two_stack_nthash` | Normally A3 chronological SIMD with the minimap2 integer hash. |

*CPU minimizer implementations exposed by the PostgreSQL setting.*

<a id="tab-current-cpu-minimizers"></a>

> [!NOTE]
> **Position locality**
>
> In case of generating minimizers we can operate on local coordinates meaning that our minimizer representation can just track position relative to the sequence fragment. Sequence fragmentation in storage is described in later chapters. However for next steps of processing (chaining) we use global coordinates. It would be possible to take advantage of that locality for example by building index that stores only minimizers for some shorter sequence fragments (this is also described in later chapters), but for simplicity even in that case we just map local coordinates afterwards to global ones at all times. This have the cost of anchors being wider in memory (as we need to use int64 for global coordinates).

<a id="project-note-1"></a>

**1.** Inspected code from official G3SA repository that supplies CUDA kernels along with CPU host-side code, <https://github.com/sunwookim028/G3SA>
