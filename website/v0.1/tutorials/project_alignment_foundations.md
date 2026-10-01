---
title: "Sequence alignment and search strategies"
slug: project_alignment_foundations
description: "Sequence alignment and search strategies"
order: 40
keywords: [BioseqDB, project]
draft: false
---

## Alignment foundation and specification of a problem

<a id="sec-alignment-model"></a>

Alignment can be done in different ways, where the core algorithms stays the same, but some of the conbditions change. It’s important to understand what are the capabilities of standard dynamic programming algorithms and how they can be used to solve different problems.

Formally: Let $X=x_1\ldots x_n$ and $Y=y_1\ldots y_m$ be the sequences we align. An alignment is a sequence of columns $$A=((a_1,b_1),\ldots,(a_L,b_L)),
  \qquad
  (a_r,b_r)\in(\Sigma\cup\{-\})^2\setminus\{(-,-)\},$$ where first and second elements reciver contents of $X$ and $Y$ after gaps are removed:

1.  An element of shape $a_r, -$ signifies an insertion in $X$

2.  An element of shape $-, b_r$ signifies a deletion in $Y$

3.  An element of shape $m_r, m_r$ signifies a match

4.  An element of shape $a_r, b_r$ where $a_r \neq b_r$ signifies a substitution

This alignment is an optimized sequence correspondence under a scoring model. For substitution score $s(a,b)$ and gap cost $g(\ell)$, the score is $$S(A)=\sum_{\text{symbol pairs }r}s(a_r,b_r)
       -\sum_{\text{gap runs }u}g(|u|).$$ The scoring model may be formulated in different ways, but one of the simplest and mostcommon conventions is to use an affine gap cost model: <a id="eq-affine-gap-convention"></a> $$g(\ell)=q+\ell e,
  \qquad \ell\geq 1,$$ where $q$ is the opening cost and $e$ is the extension cost.

A gap of size one costs then $q+e$. Some scientific papers mentioning $q+(\ell-1)e$ use a different parameter convention. Affine scoring charges one opening cost per gap run and can be evaluated with a fixed number of states per matrix cell \[[28](../bibliography/#ref-gotoh_1982_affine)\].

This scoring model isn’t sufficient to define entire problem:

1.  Global alignment consumes both complete sequences \[[29](../bibliography/#ref-needleman_wunsch_1970)\]

2.  Local alignment may restart at zero and selects the best pair of substrings \[[30](../bibliography/#ref-smith_waterman_1981)\]

3.  Semi-global and overlap goals consume selected ends while allowing specified prefix/postfix to remain unaligned

4.  Extension begins from a fixed anchor and chooses an endpoint under a stopping rule

5.  Clipping will just finish on the rightmost gap and keep postfix unaligned

6.  Local-extension fixes the start but permits an interior endpoint

Figure [alignment boundary modes](../project_alignment_foundations/#fig-alignment-boundary-modes) shows schematic examples of these modes.

![02 alignment foundations diagram](/static/figures/project/chapters-02-alignment-foundations-tikz-01.svg)

*Eight alignment boundary modes. Yellow lines depcict possible tracebacks. Black dots mark fixed endpoints (for example for global alignment we always align entire sequence so endpoints are 0 and len(Y)-1), yellow dots mark selected free endpoints, and bold border segments mark penalized or anchored boundaries (usually the border is the length of sequence, but under certain conditions we might have limits on the alignment length and coordinates). In each lower schematic, the solid arrow is the reference and the yellow dashed track is the query. These panels showcase how different libraries/algorithms handle different goals across number of scientific resarch*

<a id="fig-alignment-boundary-modes"></a>

Various practical implementations also use special stopping criteria, which usualy can be upper bound on the $S(A)$ score.

## Dynamic programming definition

<a id="sec-exact-dp"></a>

Let $H_{i,j}$ be the best score after consuming $i$ symbols of $X$ and $j$ symbols of $Y$. For a simpler linear gap cost model $d$, we can define the algorithm’s state matrix as: <a id="eq-linear-gap-dp"></a> $$H_{i,j}=\max\bigl\{
    H_{i-1,j-1}+s(x_i,y_j),
    H_{i-1,j}-d,
    H_{i,j-1}-d
  \bigr\}.$$ This is the intuitive definition based on 3 possible options (replacement or two variants of gap). Free-end alignment (like local) change the score values for the cells from which traceback may begin.

For affine scoring model we need to hold main state and one state for each gap orientation. Let $I_{i,j}$ and $D_{i,j}$ end in gaps in $X$ and $Y$, respectively, and let $M_{i,j}$ end in a symbol pair. Under Equation [affine gap convention](../project_alignment_foundations/#eq-affine-gap-convention), one formulation is <a id="eq-affine-dp"></a> $$\begin{aligned}
  M_{i,j} &= H_{i-1,j-1}+s(x_i,y_j),\\
  I_{i,j} &= \max\{H_{i,j-1}-q-e,\ I_{i,j-1}-e\},\\
  D_{i,j} &= \max\{H_{i-1,j}-q-e,\ D_{i-1,j}-e\},\\
  H_{i,j} &= \max\{M_{i,j},I_{i,j},D_{i,j}\}.
\end{aligned}$$ Using $H$ to open a gap permits a transition from any previous state. Local alignment also includes zero in the final maximum.

##### Affine-gap alignment with an explicit boundary contract

<a id="alg-affine-dp"></a>

1.  Input: sequences $X[1\ldots n]$, $Y[1\ldots m]$, scores $s,q,e$, and a boundary contract $B$

2.  Output: score, endpoints, and an optional CIGAR string

3.  initialize $H$, $I$, and $D$ according to $B$

4.  For $i\gets 1$ to $n$

5.  For $j\gets 1$ to $m$

6.  update $M_{i,j}$, $I_{i,j}$, $D_{i,j}$, and $H_{i,j}$ with Equation [affine dp](../project_alignment_foundations/#eq-affine-dp)

7.  store the predecessor of each winning state when traceback is requested

8.  End For

9.  End For

10. select the endpoint permitted by $B$

11. follow predecessors to an allowed start and run-length encode the operations

The algorithm state matrix can be used to retrieve the edit path as CIGAR string. This is the standard compact representation of gapped alignments described in chapter 1\[[83](../bibliography/#ref-sam_format_specification)\]

Affine model is some scenarios might not be enough as it doesn’t distinguish between short and long gaps. The two-piece affine model can support such feature without complicating the scoring function too much: <a id="eq-two-piece-affine-model"></a> $$g_2(\ell)=\min(q_1+\ell e_1,\ q_2+\ell e_2)$$ This model permits different cost slopes for short and long gaps. In our software we use single affine model for the fastest alignment modes (adaptive block), while KSW2 variants support dual affine model.

For large datasets evaluating entire large $O(nm)$ matrix can make significant practical difference:

- We can evaluate alignments only close to diagonal. Being close to diagonal means that there aren’t many large gaps between matching fragments. Designing algorithms around diagonals can make us skip evaluation of many far cells that do not contribute to the result.

- In case of features that do not require traceback (CIGAR construction requires traceback, but score or coordinates of best match do not) we can just store relevant slice in memory and avoid allocating large memory region.

- We can introduce X-drop stop condition: stop when score falls below best score seen so far by more than X

- We can introduce Z-drop stop condition: the same as X-drop, but if the distance from diagonal is too high then stop as well (this means that we stop when there are too many gaps cummulatively) <a id="lst-xdrop-zdrop-strategies"></a>

Smart evaluation startegy can combine all these approaches. KSW2 and Block aligner for example use all of them. Block aligner \[[38](../bibliography/#ref-block_aligner_paper)\] evaluates only the most promising rectangular region in the matrix while KSW2 \[[31](../bibliography/#ref-suzuki_kasahara_2018)\] focuses on diagonal-centered evaluation.

## Seed and extend

<a id="sec-seed-chain-extend"></a>

Applying local dynamic programming between a query of length $m$ and a reference of length $N$ costs $O(Nm)$. Most reference positions don’t contribute to the result. Long-read mappers therefore organize search into three stages \[[21](../bibliography/#ref-minimap2_paper)\].

First step is to use divide and conquer approach and split sequence into representatives known as seeds:

<a id="eq-seeding-def"></a> $$\begin{aligned}
  \operatorname{Seed}(x_1,\ldots,x_n)
    &= \bigl\{(s_1,i_1,j_1),\ldots,(s_k,i_k,j_k)\bigr\},\\
    &\text{where }1\leq i_a,j_a\leq n
      \quad\text{for all }a\in\{1,\ldots,k\}.
\end{aligned}$$

Seed $(s_a, i_a, j_a)$ is some represenative of the sequence substring $x_a = x_{i_a}x_{i_a+1}...x_{j_a}$. This can be implemented in different ways, this definition is the most abtract one. If a seed occurs in both query and reference, then we can consulde that two represented substrings are very similar. Using some concrete seeding function we can obtain intersection for query and reference: <a id="eq-matching-seeds"></a> $$S_{X\cap Y}=\left\{
    (s,i_x,j_x,i_y,j_y)\;\middle|\;
    \begin{aligned}
      (s,i_x,j_x)&\in\operatorname{Seed}(X),\\
      (s,i_y,j_y)&\in\operatorname{Seed}(Y)
    \end{aligned}
  \right\}.$$

1.  **Seeding** Generate $Seed(X)$ and $Seed(Y)$ and obtain $S_{X \cap Y}$

2.  **Chaining** As matching seeds will cover only parts of both sequences that match without errors, we still need to fill-in the gaps between them. We connect seeds from $S_{X \cap Y}$ into chains using similar dynamic programming approach as in usual aligner. This gives us some approximate alignments.

3.  **Extension** Perform usual alignment algorithm on the approximate fragments to obtain final results.

**Minimizers** are one of many possible seeding model definitions. Given parameters $w$ and $k$, a window $W$ of length $l = w + k - 1$ contains $w$ consecutive $k$-mers. The minimizer of the window is the smallest $k$-mer (subsequence of length $k$) in the window. For random minimizers, $k$-mers are ordered by a pseudo-random order, usually given by comparing hashes of the $k$-mers. In case of ties, the leftmost smallest $k$-mer is chosen. \[[32](../bibliography/#ref-roberts_minimizers_2004)\] Overlapping windows often retain the same representative, which is emitted once.

**Canonical minimizers:** DNA is double-stranded and majority of sequencing technologies do not distinguish these two strands. Genomic sequences thus need to satisy additional constraint: sequence and its reverse complement ($A \leftrightarrow T, C \leftrightarrow G$) should be handled in the same way by the software (they should be indistinguisable) In our case this means that for minimizer of a window $W$ at position $p$ we need to have minimizer $\overline{W^{c}}$ at position $w-1-p = |W| - k - p$. \[[36](../bibliography/#ref-groot_koerkamp_simd_minimizers_2025)\] In most of the software due to simplicity truly cannonical minimizers aren’t used. Instead we take minimum of hashes of $k$-mer and its reverse complement: $m^{\text{cannonical}} = \min(m, \overline{m^{c}})$

![02 alignment foundations diagram](/static/figures/project/chapters-02-alignment-foundations-tikz-02.svg)

*The seed–chain–extend approach in minimap2*

<a id="fig-seed-chain-extend"></a>

In cases of both chaining and base alignment we operate on very similar score definitions like in Equation [linear gap dp](../project_alignment_foundations/#eq-linear-gap-dp) or Equation [affine dp](../project_alignment_foundations/#eq-affine-dp). The algorithms are conceptually similar, but they have different methods of terminating the search, scoring function which punish gaps harder (base aliugnment doesn’t need to care about large gaps that can occur in chaining stage). I provide overview of literature and my own implementations in next chapters.

## Limitations of existing tools

<a id="sec-limitations-existing-tools"></a>

Most of the current most popular tools implement variations of the seed and extend approach described in the previous section. However the algorithm outlined here is heavily underspecified to be directly implemented as a computer program:

- $S_{X \cap Y}$ can be implemented as interection of two hash maps where $Seed(X)$ is realized as map from seed to its start/end: $S \rightarrow \natural^2$ where seed itself is represented by it’s hash $S = \mathbb{Z}$. This opens few important questions: what hashing strategy to choose? Which map implementation to choose?

- As $Seed(X)$ can be computed before alignment starts we can store it in memory and persist it to disk to speed up processing in the future. Which binary format we want to choose?

- Anchors as tuples will be implemented as some concrete binary structure and authors need to specify their representation (do we store them as AOS or SOA? What are widths of elements and their types?). Each decision will imapct performance as there can be millions of anchors per long sequence and additional bytes sum up to MBs of additional memory to be processed (and slowdowns).

- Finally, what is the most effective way to parallelize this computation (the most straightforward approach is to split input sequences $X$ and $Y$ into chunks)

Most of the current state of the art tools choose to:

1.  File-based operation

2.  Suboptimal or non-extensible internal data structures

3.  Optimise only selected parts of the algorithm like hash function properties or one specific stage.

This leads to different consequences for the user.

**File-based operation:** Operating directly on the disk with simplistic wire format means that seed index is usually one giant binary blob not prepared for highly-concurrent access and interface doesn’t offer modern tools like buffer pooling or lazy loading. The format of indexes are largely undocumented by authors. Missing of lazy loading feature is the most hurting to the user. If index is large it will be loaded entirely into memory and can overwhelm SWAP space. This degrades performance by several orders of magnitude and can be avoided by splitting sequences into smaller chunks. User doesn’t know what is the exact relationship between sequence length and required memory space, so this process takes time and involves guessing. Additionally index implementation sometimes offer complex parameters to adjust its partitioning (like in case of MMseqs2), but entire responsibility is still shifted to the user and that leads to user frustration andrecurring OOM issues or severly degraded performance. \[[17](../bibliography/#ref-mmseqs2_issue_338)\] \[[18](../bibliography/#ref-mmseqs2_issue_517)\] Software not only treats indexes as files, but it operates on FASTA files directly. This means the user cannot easily sort/split/extract or filter records without using external tools.

**Suboptimal or non-extensible internal data structures:** Minimap2 uses 31 bits to encode anchor’s position meaning that practically entire human genome (\>3Gbps) cannot be aligned at once and require splitting via other tools before any operation is performed. LexiMap (not using seed and extend approach) limits maximum genome size to 268Mbp \[[15](../bibliography/#ref-lexicmap_faqs)\]. Additionally software is implemented without proper abstractions - minimap2 code is non-easily extensible C code, while G3SA doesn’t offer extension points for new types of indexes or sequences interfaces.

**Selective optimisations:** Optimisations are usually fixed for one single platform (AVX512 or SSE) and either get severe performance penalties (scalar fallbacks) or user cannot use them at all. SIMD support is quite popular even though it supports one or two selected platforms, but GPU support exists usually only for NVIDIA graphics cards. This leads to a difficult situation for the user as they need to choose between several versions of the same tool (\[[11](../bibliography/#ref-mm2_fast_paper)\], \[[10](../bibliography/#ref-mm2_gpu_paper)\]), while better, more modern software like G3SA exists purely on NVIDIA hardware \[[44](../bibliography/#ref-g3sa_paper)\] Modern hardware like MacBook Pro M3 with arm64 architecture won’t be able to utilise any integrated GPU acceleration. While minimap2 offers compatibility layer to port SSE code to NEON, it does not vectorize all operations and code ported from SSE doesn’t account for lane widths and instruction level parallelism available in M2-M4 chips. \[[9](../bibliography/#ref-simd_opt_website_jake)\]

I wanted to address these limitations by implementing BioseqDB, Postgres extension that will provide the following features:

- Tight integration with Postgres meaning that user can operate on sequences (loaded FASTA files) directly using SQL rather than resorting to external toolkits.

- Support for indexes and storage structures designed with parallelism and lazy-loading from the start, meaning that software can be run on very RAM-constrained device and still perform well. At the same time design will incorporate assumptiuon about arbitrary large sequences so the user will be able to operate on \>3Gbps records without any additional steps.

- Provide layer of abstractions written using latest C++26 standard, so the algorithms can be reused in separate software or extended to different types of new sequences and indexes.

- Provide optimised kernels for SIMD and GPU architectures written using architecture-agnostic, but performant code.

Tight integration with Postgres also means that as a side effect, we can utilise some benefits of persistent state: deferring expesive operations, keeping large indexing structures in memory in case use reruns query and more effective memory-disk caching schemes.
