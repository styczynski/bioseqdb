---
title: "Final alignment and postprocessing"
slug: project_final_alignment_postprocessing_and_preprocessing
description: "Final alignment and postprocessing"
order: 70
keywords: [BioseqDB, project]
draft: false
---

## Base-level alignment

<a id="sec-final-alignment-role"></a> <a id="sec-final-alignment-cpu-contracts"></a>

In this short chapter I want to discuss last parts of the pipeline’s algorithms. The first one is the base-level alignment. Chaining will tell us approximetely where two sequences match, but it isn’t sufficient to tell which exact symbols correspond to which. The base-level alignment performs the dynamic programming specified in the introductory sections [exact dp](../project_alignment_foundations/#sec-exact-dp) as now instead of matching entire target, we can just process target subsequence of size similar to the query size.

> [!NOTE]
> **Conditional CIGAR execution**
>
> BioseqDB doesn’t call base-level alignment if no `bioseq.cigar()` is requested [xdrop zdrop strategies](../project_alignment_foundations/#lst-xdrop-zdrop-strategies), so then `bioseq.score()` only reflects the chaining score. If `bioseq.cigar()` call is present in the SQL query, then score represents results of the final alignment (thus score can differ depending on the query). This is explained in the walkthrough appendix [tutorial](../getting_started/).

In BioseqDB I used existing implementation of KSW2 (the algorithm by the author of minimap2 that uses AVX SIMD C intrinsics to implement DP calculations) from minimap2 and Block Aligner \[[21](../bibliography/#ref-minimap2_paper), [38](../bibliography/#ref-block_aligner_paper)\]. The DP formulations follow what was described already easlier [exact dp](../project_alignment_foundations/#sec-exact-dp), where KSW2 provides various flavours of functions in its API primarily left/right extension and local mode [alignment boundary modes](../project_alignment_foundations/#fig-alignment-boundary-modes). KSW2 uses banded alignment where the maximum distances are fixed (for more details on that see chaining for maximum diagonal distance (bandwith) [chaining dotplot](../project_chaining/#fig-chaining-dotplot)) The block aligner is more complex and uses moving square blocks to adaptively set this size of "bandwith", where we compute scores in the score matrix.

*CPU aligners incorporated into BioseqDB.*

<a id="tab-final-aligner-comparison"></a>

|  | **KSW2** | **Block Aligner** |
|:---|:---|:---|
| DP matrix where scores are calculated | Fixed diagonal band | Moving square that grows and shrinks repeatedly |
| Gap cost function (single-affine/dual-affine) | $\min(q_1+e_1\ell,q_2+e_2\ell)$ | $q_1+e_1\ell$ |
| Special stop conditions (see below) | Z-drop | X-drop after two consecutive low-scoring steps |
| Parameters that affect sizes of searched regions | "bandwith" is the maximum distance from the diagonal for the diagonal band | Block size, which is the initial size of the square |

I previously introduced X-drop and Z-drop concepts [xdrop zdrop strategies](../project_alignment_foundations/#lst-xdrop-zdrop-strategies), but formally they can be formulated as follows. Let’s say $S^\star$ is the best score reached at $(i^\star,j^\star)$ and we test this best score against the current score $S$ at $(i,j)$ (meaning that we are matching $i\text{th}$ symbol of target vs. $j\text{th}$ symbol of query): <a id="eq-zdrop"></a> $$\begin{aligned}
  \text{X-drop:}\quad&S^\star-S>X,\\
  \text{Z-drop:}\quad&S^\star-S>Z+e_{g}\left|(i-i^\star)-(j-j^\star)\right|.
\end{aligned}$$ $e_{g}$ here is gap-extension penalty, which in this context means that we can disard all symbols from the best score to the current position by introducing a gap. This means that effectively we just stop earlier if the distance from the diagonal gets to large, so practically Z-drop is more restrictive for correct setup.

## How Block Aligner follows the alignment

<a id="sec-final-alignment-block"></a>

Block Aligner moves a $B\times B$ square through the scoring matrix, and internally compute strips of width $S=8$ visible on the figure [block adaptive reconstruction](../project_final_alignment_postprocessing_and_preprocessing/#fig-block-adaptive-reconstruction) below. When the algorithm fails to improve best score for number of steps, then it decides to grow the block as seen on image below [block adaptive reconstruction](../project_final_alignment_postprocessing_and_preprocessing/#fig-block-adaptive-reconstruction)

<a id="fig-block-adaptive-reconstruction"></a>

![01 base level alignment diagram](/static/figures/project/chapters-07-final-alignment-postprocessing-and-preprocessing-01-base-level-alignment-tikz-01.svg)

*Illustration of 3 steps that Block Aligner repeatedly performs. Growth computes the lower rectangle $\Omega_1$ before the right rectangle $\Omega_2$.*

The pseudocode below is directly taken from Block Aligner paper \[[38](../bibliography/#ref-block_aligner_paper)\]. For more details please consult the projects README \[[39](../bibliography/#ref-block_aligner_readme)\]. I use here original paper notation, where $q$ is the query and $p$ the target sequence aligner to. $G^c_{open},G^c_{close},G^r_{open},G_{ext}$ are the gap penalties. Function $\mathrm{COMPUTE\_RECT}((r,c),(w,h))$ is used to compute all the scores in the given rectangle given as parameters, using SIMD version of regular DP scoring formulation.

The important thing in the paper is: $[r,r+h)\times[c,c+w)$; $Y=\lfloor B/S\rfloor-1$ which defines threshold, when block assumes the score cannot improve and decides to grow. Note that here $S$ was used by paper’s authors as the width of the strip, not the actual score computed.

<a id="alg-adaptive-block-alignment"></a>

Pseudocode for the Block Aligner algorithm.

```pseudocode
Procedure `ALIGN`($q,p,X,B_{min},B_{max},G^c_{open},G^c_{close},G^r_{open},G_{ext}$)
  $S\gets8$, $\mathit{max\_score}\gets0$, $\mathit{dir}\gets\textsc{GROW}$, $B\gets B_{min}$, $B_{prev}\gets0$, $i\gets0$, $j\gets0$
  Initialize $D$, $D'$, $C$, $R$, and the trace array
  While true // One step of ligner. Note that $i$, $j$, $B$, and $\mathit{dir}$ have already been updated.
    If $\mathit{dir}=\textsc{RIGHT}$ // Block position $(i,j-S)\mapsto(i,j)$
      $\mathit{curr\_score}\gets\verb|COMPUTE_RECT|((i,j+B-S),(S,B))$
    Else If $\mathit{dir}=\textsc{DOWN}$ // Block position $(i-S,j)\mapsto(i,j)$
      $\mathit{curr\_score}\gets\verb|COMPUTE_RECT|((i+B-S,j),(B,S))$
    Else If $\mathit{dir}=\textsc{GROW}$
      $\mathit{curr\_score}\gets\verb|COMPUTE_RECT|((i+B_{prev},j),(B_{prev},B-B_{prev}))$ // Grow down
      $\mathit{curr\_score}\gets\verb|COMPUTE_RECT|((i,j+B_{prev}),(B-B_{prev},B))$ // Grow right
    End If

    If $\mathit{curr\_score}>\mathit{max\_score}$
      Update $\mathit{max\_score}$ with $\mathit{curr\_score}$ and its store its position
      Save $i$, $j$, and the block right and bottom border scores as checkpoint
    End If

    **if** $\mathit{curr\_score}<\mathit{max\_score}-X$ for 2 consecutive steps, **then break** // X-drop
    **if** $i+B>|q|$ **and** $j+B>|p|$, **then break** // Reached end of DP matrix
    **if** $i+B>|q|$ **or** $j+B>|p|$, **then** shift $i$, $j$ by $S$ to avoid out of bounds **and continue**

    If $B<B_{max}$ and $Y$ steps pass without improving $\mathit{max\_score}$ // Block grow criteria
      $B_{prev}=B$, $B\gets2B$, $\mathit{dir}\gets\textsc{GROW}$
      Restore $i$, $j$, and block border scores from previous checkpoint
      **continue**
    End If

    If $B>B_{min}$ and $\mathit{max\_score}$ is in the bottom-right corner region of block // Block shrink criteria
      $B\gets B/2$, $i\gets i+B$, $j\gets j+B$
      Save $i$, $j$, and the block right and bottom border scores as checkpoint
    End If

    $\mathit{dir}\gets\textsc{RIGHT}$ or <span class="smallcaps">DOWN</span> based on comparing right and bottom border cells
    Increment $i$, $j$ by $S$ based on $\mathit{dir}$
  End While
  Return the trace array, and **if** X-drop, **then** $\mathit{max\_score}$, **else** $D[|q|][|p|]$
End Procedure
```

## Postprocessing

<a id="sec-alignment-postprocessing"></a>

After the alignment is done, BioseqDB computes CIGAR string, extracts all values returned by the final aligner (like exact coordinates, score etc.), and also calculates some additional values (if they are requested by the user in query):

- **Divergence** Matching minimizers give an estimate of the fraction of differing bases

- **Mapping quality (MAPQ)** This estimates how subjectively good are the mapping locations (are we confident enough that this is a match)

Minimap defines diverges exactly as mentioned before in Section [minimizer divergence history](../project_minimizers/#sec-minimizer-divergence-history): <a id="eq-postprocessing-divergence"></a> $$\widehat{\epsilon}=\frac{1}{k}\ln\frac{n}{m},
  \qquad
  \widehat{d}=1-\exp(-\widehat{\epsilon})
            =1-\left(\frac{m}{n}\right)^{1/k}.$$ where $n$ is the number of minimizers that are part of chains and $m$ is the number of all query minimizers between first match and last match BioseqDB similarily to what minimap2 does, returns $\widehat{d}$, which is approximation of $\widehat{\epsilon}$ assuming low sequence divergence. To obtain $n$ in BioseqDB I uise cumulative counts of minimizers by position, so subtracting two counts gives the number in an interval without sorting the emitted minimizers (this avoids cirtical problem I found earlier, which made this step thousands of time slower - Section [minimizer divergence history](../project_minimizers/#sec-minimizer-divergence-history)).

For MAPQ, if two reference locations give nearly equal alignment scores, BioseqDB assigns low MAPQ, because both alignments may match well and we cannot be confident enough that this location is the correct one.

The maximum MAPQ in minimap2 is 60 and it’s similar to usual phred score scale used in base calling (for example in SAM formats), where $P=10^{-Q/10}$ \[[83](../bibliography/#ref-sam_format_specification)\] equal to 60 means 1 in million chance for error.

BioseqDB uses a direct port of minimap2’s heuristic \[[74](../bibliography/#ref-minimap2_mapq_implementation)\]. The equation is given below. Please note that alignment score is different to score given by chaining stage: <a id="eq-mapq-final"></a> $$\begin{aligned}
    \mathrm{MAPQ}\approx\operatorname{clip}_{C_{\min},C_{\max}}\Bigl(
      &\min\!\left\{
        C_1Ip(1-\rho^{C_{\mathrm{power}}})\ln\frac{D_{\mathrm{pri}}}{a},\,
        C_2I^{C_{\mathrm{power}}}\frac{D_{\mathrm{pri}}-D_{\mathrm{sec}}}{a}
      \right\}\\
      &{}-C_3\ln(N+1)
    \Bigr).
  \end{aligned}$$

- $D_{\mathrm{pri}}$: best DP score returned by the aligner for the primary chain.

- $D_{\mathrm{sec}}$: highest DP score returned by the aligner among secondary chains competing with the primary chain for an overlapping region of the query.

- $I$: factor of how many bases we matched (matching bases divided by alignment block length)

- $p$: MAPQ multiplier factor calculated as $p=\operatorname{clip}_{1,\infty}(\frac{s_{\mathrm{pri}}}{100}, \frac{N_{\mathrm{pri}}}{10})$, where ${s}_{\mathrm{pri}}$ is the chaining score of primary chain and ${N}_{\mathrm{pri}}$ number of anchors in that chain. It’s used to penalize short or repetitive chains.

- $\rho=(D_{\mathrm{sec}}/D_{\mathrm{pri}})
      \max(s_{\mathrm{sec}},s_{\min})/s_{\mathrm{orig}}$: ratio combining alignment and chaining scores. Here $s_{\mathrm{sec}}$ is the best secondary chaining score, $s_{\min}$ is the configurable fixed parameter used by floor for the secondary chaining score and $s_{\mathrm{pri}}$ is the primary chaining score.

- $N$: number of secondary chains for that fragment.

- $\operatorname{clip}_{C_{\min},C_{\max}}$ shorthand notation for $min(max(x, C_{\min}), C_{\max})$

- BisoeqDB defaults to minimap2’s defaults: $C_1=40$, $C_2=6.02$, $C_3=4.343$, $C_{\mathrm{power}}=2$, $C_{\min}=0$, $C_{\max}=60$.

> [!NOTE]
> **MAPQ implementation details**
>
> In minimap2 it’s possible to disable base alignment and when that happens certain adjustments happen, for example $D_{\mathrm{sec}}/D_{\mathrm{pri}}$ becomes one, instead of $D$ we use $s$ where possible. The origin of the formula is empirical and unfortunately I didn’t go into details understanding completely its structure. Some details can be found in original minimap2 paper \[[21](../bibliography/#ref-minimap2_paper)\].

## SneakySnake prefiltering

<a id="sec-prefilter-role"></a> <a id="sec-prefilter-sneakysnake"></a>

We discussed the postprocessing step, but there’s also corresponding preprocessing step in the pipeline. I didn’t separate it into its own chapter, because the feature is exerimental and uses external dependency.

A prefilter is specialized algorithm to reject sequence quickly before doing any alignment work. In BioseqDB I decided to use SneakySnake algorithm directly. Good prefilter should offer the following properties:

- It should quickly evaluate query and sequence parts and compute estimate of edit distance.

- The estimated edit distance should be lower than actual edit distance to prevent situation in which prefilter rejects valid candidates.

In BioseqDB I used SneakySnake \[[41](../bibliography/#ref-sneakysnake_paper), [40](../bibliography/#ref-sneakysnake_readme)\], where the core idea is to translate approximate string matching problem into a chip maze problem (formally SNR - Single Net Routing). I won’t describe the algorithm in detail here, as I only use SneakySnake as a dependency and it wasn’t evalauted in the final benchmarks.

The prefilter can be evaluated if users provides special predicate [prefilter condition](../project_postgres_custom_scan/#lst-prefilter-condition). In that case BioseqDB for case where the sequence is filtered-out won’t fetch the minimizers or recompute them meaning the rest of the pipeline will be entirely skipped (to be precise the order of the filter evaluation is as follows: non-sequence user filters, then prefilter):

```sql runnable id=project_chapters-07_final_alignment_postprocessing_and_preprocessing-02_prefiltering_0 timeout=30s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
CREATE TABLE tutorial_mmsearch (pk integer, seq0 bioseq.seq('dna4', 64)) USING bioseq;
INSERT INTO tutorial_mmsearch VALUES (1, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA');
-- bioseq:visible
SELECT pk, bioseq.edit(seq0) AS edits
FROM tutorial_mmsearch
WHERE public.mmsearch(seq0, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'::bioseq.seq('dna4', 64))
  AND bioseq.edit(seq0) < 5
ORDER BY pk;
-- bioseq:teardown
DROP TABLE tutorial_mmsearch;
RESET bioseq.index.insert_maintenance.enabled;
RESET max_parallel_workers_per_gather;
```

The SNR problem is constructed as follows: For input query/target $Q,R\in\Sigma^m$ and given maximum edit distance $E$, SneakySnake constructs a matrix $Z \in \{0,1\}^{(2E+1)\times m}$ The idea is that the matrix covers one sequence (columns), while the rows cover only $2E+1$ relative positions in the second sequence. We then compare each position in the query and in target and if they match we put 0, otherwise we put 1 in the $Z$ matrix. Now $Z$ matrix represents "obstacles" for 1s and empty spaces for 0s. The SNR problem needs to find a path from left to right (possibly going up and down and to the left, but never to the right), where path is better if it contains less obstacles.

The benefit here is that traverse of the matrix is $O((2E+1)*m)$, which for low $E$ can be very fast. You must note however that I directly plugged SneakySnake into BioseqDB lazy-sequence adapters without proper benchmarking, which means that I don’t know how matching quality/performance is affected by calling this prefilter.

Let’s define $Z$ matrix formally. First index $i$ is up to $2E+1$ and second index $j$ is position in $R$: $1\leq j\leq m$: $$\delta_i=\begin{cases}
  -i,&1\leq i\leq E,\\
  0,&i=E+1,\\
  i-E-1,&E+1<i\leq2E+1.
 \end{cases}
 \qquad
 Z_{i,j}=\begin{cases}
  0,&1\leq j+\delta_i\leq m\ \land\ Q_{j+\delta_i}=R_j,\\
  1,&\text{otherwise}.
 \end{cases}$$ Here, $\delta_i$ just formally defines relative lookups i.e if we look to the left of position in $R$ up to $E$ positions or to the right up to $E$ positions. In paper authors want to look at the left side first, for example $(-1, -2, -3, 0, +1, +2, +3)$, so $\delta_i$ looks more complicated than just $i-E$.

<a id="fig-prefilter-chip-maze"></a>

![02 prefiltering diagram](/static/figures/project/chapters-07-final-alignment-postprocessing-and-preprocessing-02-prefiltering-tikz-01.svg)

*Example similar to original figure from paper: Fig. 2(a,b) in [Alser, Shahroodi, Gómez-Luna, Alkan, Mutlu](../bibliography/#ref-sneakysnake_paper) ([2020](../bibliography/#ref-sneakysnake_paper)) for $R=\mathtt{GGTGCAGAGCTC}$, $Q=\mathtt{GGTGAGAGTTGT}$, and $E=3$. (a) Comparing $R_6$ with $Q_3,\ldots,Q_9$ constructs column 6 (blue in (b)). The first three results follow this $\delta_i$ reversed order.. (b) The net (best path) crosses three circled obstacles, so the pair passes the filter. The SneakySnake algorithm uses special notion of "checkpoints" so we don’t perform path updates until obstacle is met after last checkpoint.*
