---
title: "PAM, BLOSUM and scoring matrices from 1,000 human mitochondrial genomes"
slug: scoring-matrices-from-human-genomes
description: "Understand PAM, BLOSUM, log odds and gap penalties, then use BioseqDB SQL to align 1,000 human mitochondrial genomes, count DNA differences and compare amino-acid pair frequencies with BLOSUM62."
date: '2026-10-22'
author: Piotr Styczyński
category: bioseqdb
keywords: [BioseqDB, PostgreSQL, SQL, BLOSUM, PAM, substitution matrix, log odds, mitochondrial DNA, sequence alignment, human genomics, NCBI, gap penalties, Python]
cover: /static/figures/blog/scoring-matrices/cover.svg
image_alt: "Colored nucleotide and amino-acid matrix tiles beside simplified DNA strands with a changed base pair."
draft: false
---

Two sequences can agree at most positions and still leave us with a few awkward decisions. Is a leucine opposite an isoleucine a reasonable match? Should an alignment insert a gap here, or accept three mismatches and keep going? A scoring scheme puts numbers on those choices, and changing the numbers can change the alignment you get.

PAM and BLOSUM are families of substitution matrices for proteins. They describe how amino-acid pairs occur in aligned related sequences, relative to the pairs we would expect from their individual abundance. We will work through those counts, then use BioseqDB to calculate nucleotide and protein comparisons from 1,000 human mitochondrial genome records. SQL will do the counting and matrix calculations. Python will draw the results.

A protein is a chain of amino acids. A coding region of DNA supplies its instructions in three-letter groups called codons, each specifying an amino acid or a stop signal.

Mitochondria carry a small genome of their own, separate from the nuclear chromosomes. That gives us complete mitochondrial records small enough to inspect and align in a tutorial, along with protein-coding genes that connect the DNA analysis to BLOSUM. Keep the two alphabets in mind: the A in a DNA sequence is adenine, while the A in BLOSUM is alanine, an amino acid.

## What a scoring matrix does

An alignment places two sequences into corresponding columns. A column can contain the same letter twice, two different letters, or a letter opposite a gap. Those gaps let us represent insertions and deletions, usually shortened to *indels*.

The scoring system has two parts. A substitution matrix assigns a score to each aligned letter pair, and a gap model charges for inserting or extending gaps. If we call the alignment $\mathcal A$, its score is

$$
S(\mathcal A)=
\sum_{(a,b)\in\text{paired columns of }\mathcal A}s(a,b)
-\sum_{g\in\text{gap runs of }\mathcal A}G(|g|).
$$

Here $s(a,b)$ is the substitution score and $G(|g|)$ is the cost of a gap run of length $|g|$. The algorithm searches for a high-scoring arrangement under that rule. A global alignment consumes both full sequences, whereas a local alignment can select matching regions within them. The [alignment foundations chapter](/v0.1/tutorials/project_alignment_foundations/#sec-alignment-model) gives the formal definitions and shows the different boundary conditions.

Why use biological scores at all? Related proteins can accumulate substitutions while retaining a similar structure and function. A scoring matrix lets an alignment retain evidence from those changes instead of relying entirely on identical letters. We can then assess the resulting score against a chance model, which helps identify similarities consistent with common ancestry. The significance also depends on sequence length, composition and the size of the database being searched.

## Start with an identity matrix

The smallest useful example rewards a match with $+1$ and charges $-1$ for a mismatch:

$$
s(a,b)=
\begin{cases}
+1,&a=b,\\
-1,&a\ne b.
\end{cases}
$$

Some versions give mismatches zero instead. Either way, all mismatches receive the same treatment. For DNA this is a useful starting point, and simple match/mismatch schemes appear in many nucleotide aligners. It can also score proteins, although we lose the distinction between chemically similar and very different amino acids.

Leucine and isoleucine, written L and I, have similar hydrophobic side chains, which tend to avoid water. Replacing one with the other can preserve the local environment of a protein. Glycine and tryptophan, G and W, are a different pairing: glycine is small and flexible, while tryptophan has a bulky aromatic side chain. Their exchange can demand a much larger structural adjustment.

We call the first kind of replacement *conservative* and the second *radical*. The surrounding protein still matters. A substitution that is tolerated at one position can disrupt another, especially in a binding site or tightly packed core. A general substitution matrix averages observations across many positions and protein families.

![An identity matrix gives all mismatches the same score, while BLOSUM62 distinguishes amino-acid pairs.](/assets/b9c0cd899a4238e71b8cbe6f9b3a369480f0e009e9c4d577216dc269f838ccd6/figures/blog/scoring-matrices/identity-and-blosum.svg)

*The letters in the protein examples are amino-acid codes. BLOSUM62 rewards L/I with +2, while G/W receives -2.*

## From observed pairs to log odds

Suppose $Q_{ab}$ is the probability of an ordered pair $(a,b)$ in our aligned related sequences. Let $p_a$ and $p_b$ be the background frequencies of those two residues. Under an independent chance model, the expected pair frequency is $p_ap_b$.

The odds ratio compares the two:

$$
O_{ab}=\frac{Q_{ab}}{p_ap_b}.
$$

An odds ratio above one means the pair appears more frequently in the related-sequence alignments than the background frequencies predict. Below one means it appears less frequently. With a model that combines evidence from individual columns, taking a logarithm turns multiplication into addition:

$$
s_{ab}=\log_2\left(\frac{Q_{ab}}{p_ap_b}\right),
\qquad
\log_2\prod_r O_{a_rb_r}=\sum_r\log_2 O_{a_rb_r}.
$$

This is convenient for alignment algorithms: they can add a score as they advance through each paired column. Positive entries favor an alignment, negative entries reduce its score, and zero means that the observed and expected frequencies agree.

BLOSUM62 stores integer scores in half-bit units. Before rounding, its scoring equation has the form used in the [ForensicMCQ BLOSUM guide](https://forensicmcq.com/blosum-matrices-in-forensic-biology-a-step-by-step-guide-to-protein-alignment-scoring/):

$$
S_{ij}=2\log_2\left(\frac{\text{observed frequency}}{\text{expected frequency}}\right).
$$

For a hypothetical pair observed with frequency $0.04$ and expected with frequency $0.01$, the calculation is

$$
2\log_2(0.04/0.01)=2\log_2(4)=2\times2=+4.
$$

Those numbers illustrate the arithmetic. Actual BLOSUM62 entries come from its measured pair frequencies. For example, the aligned protein fragments `ALK` and `AID` score

$$
S(\mathrm{ALK},\mathrm{AID})=
S(\mathrm A,\mathrm A)+S(\mathrm L,\mathrm I)+S(\mathrm K,\mathrm D)
=4+2+(-1)=5.
$$

There are no gaps in that example, so we can simply add the three matrix entries. Longer alignments use the same operation repeatedly. Here are both calculations in SQL:

<!-- scoring:toy-logodds:start -->
```sql
SELECT round(2 * log(2::numeric, 0.04 / 0.01)) AS half_bit_score;

SELECT sum(score) AS alignment_score
FROM (VALUES ('A', 'A', 4), ('L', 'I', 2), ('K', 'D', -1))
     AS aligned(first_residue, second_residue, score);
```
<!-- scoring:toy-logodds:end -->

## PAM: estimating change over evolutionary distance

Margaret Dayhoff and her colleagues developed the PAM approach during the 1960s and 1970s. Their original data came from 71 groups of closely related proteins, with alignments above roughly 85% identity. At that distance, ancestral reconstruction and counting changes along a tree are easier than they would be for highly divergent sequences.

PAM stands for *Point Accepted Mutation*. An accepted amino-acid replacement is one retained in an evolving lineage. It may preserve protein function well enough to persist, without giving the organism any particular advantage.

One PAM unit represents an evolutionary interval with an expected one accepted replacement per 100 amino-acid positions. It is a measure of accumulated change. The amount of elapsed time needed to accumulate it depends on the proteins and lineages under study.

The construction starts with global alignments of related proteins. From these alignments, a phylogenetic tree and likely ancestral sequences let us infer replacements along branches. Counts of those replacements, together with residue abundance, provide estimates of relative mutability, $m_a$: how readily each amino acid changes in the fitted model. The probabilities are then normalized to the small amount of change represented by one PAM unit.

This gives a PAM1 transition matrix, $M(1)$. An entry describes the probability of moving from one amino acid to another over that unit of evolutionary distance. The diagonal contains the probabilities of retaining the same residue.

To obtain a model for a longer distance, multiply the transition matrix by itself:

$$
M(n)=M(1)^n.
$$

For PAM250, that is $M(1)^{250}$. Each multiplication sums over possible intermediate residues, so a site can change several times and can return to its starting state. After calculating the transition probabilities at the chosen distance, we convert them into log-odds alignment scores using the background frequencies.

The distinction between these two matrices matters: the matrix being raised to a power contains probabilities. The familiar printed PAM scoring table contains logarithms of odds.

This also explains how sequences can be more than 100 PAM units apart. A site can undergo several replacements. PAM250 means 250 expected replacements per 100 positions, and the original model gives approximately 20% identity at that distance because repeated changes and reversions hide some of that history. Counting different letters at the endpoints only reveals the differences that remain.

Lower-numbered matrices, such as PAM40, describe relatively little change. Higher-numbered matrices, such as PAM250, are useful when searching for more distant protein relationships.

![PAM250 amino-acid scores, divided by three to display log odds in bits.](/assets/b9c0cd899a4238e71b8cbe6f9b3a369480f0e009e9c4d577216dc269f838ccd6/figures/blog/scoring-matrices/pam250.svg)

The PAM250 plot puts the amino acids on both axes. Each cell scores one pair. Dividing its integer entries by three expresses them in bits, which lets us compare their colors with the BLOSUM62 plot below.

### What PAM assumes

The original observations cover a small collection of closely related protein families. Extrapolating them over longer distances assumes that the same replacement process continues to apply. An error in a rare replacement estimate can therefore affect many later calculations.

Protein positions also have different constraints. A residue buried in the core, one exposed to solvent and one involved in catalysis can tolerate different changes, even when they start with the same amino acid. A single general matrix averages across those environments. Rate differences between proteins and changes in constraints over time add further variation.

These limitations explain the interest in larger datasets and alternative models. They also matter when interpreting an alignment that extends beyond a credible homologous region: the matrix and gap costs may keep adding weakly favorable columns after the biological correspondence has ended.

## BLOSUM: count replacements in conserved blocks

Steven and Jorja Henikoff introduced BLOSUM, short for BLOcks SUbstitution Matrix, in 1992. Their starting point was a collection of conserved, ungapped protein alignment blocks, drawn from hundreds of protein families. The [original paper](https://pmc.ncbi.nlm.nih.gov/articles/PMC50453/) describes about 2,000 blocks representing more than 500 groups.

A block is a short region for which the correspondence between residues is sufficiently reliable to count pairs column by column. Conserved motifs help locate these regions. Selecting ungapped columns also keeps the substitution estimate separate from the problem of deciding where insertions and deletions belong.

The family can contain both close and distant relatives. That makes it possible to observe replacement patterns over a broad range of divergence directly, instead of deriving every distance from a PAM1-style transition model.

### First reduce the weight of near duplicates

Imagine a protein family represented by 90 nearly identical mammalian sequences and a handful of more distant sequences. Counting every pair equally would let those near duplicates dominate the result.

BLOSUM groups sufficiently similar sequence segments into clusters and weights each cluster as one sequence when counting pairs between clusters. For BLOSUM80, the threshold is 80% identity within the block. BLOSUM62 uses 62%. Sequences at or above the threshold are grouped together, and their contributions are averaged.

The number therefore describes how redundancy was handled during construction. A BLOSUM62 alignment can have much less than 62% identity. It also follows that lowering the threshold groups more similar sequences together and shifts the comparison toward more distant relationships.

For the human cohort later in this tutorial, we will retain the sample counts because we want to describe variation within that cohort. The resulting log-odds matrix uses the same observed-versus-expected calculation. Its sampling rule and evolutionary scale will be visible alongside the results.

### Count the pairs in one column

The [Columbia course presentation](https://www.cs.columbia.edu/4761/assignments/assignment1/reference1) walks through this construction with small examples. We can express the unweighted counting step directly in terms of residue counts, which is also convenient in SQL.

Take one alignment column containing `A A A A I L`. There are four alanines, one isoleucine and one leucine. Every distinct pair of sequences contributes one pair of letters.

The number of A/A pairs is $4\times3/2=6$. There are four A/I pairs, four A/L pairs and one I/L pair. The total is 15, exactly the number of ways to choose two of the six sequences. Grouping the letters first gives the counts directly:

<!-- scoring:toy-column-counts:start -->
```sql
WITH residues(letter) AS (
    VALUES ('A'), ('A'), ('A'), ('A'), ('I'), ('L')
), counts AS (
    SELECT letter, count(*) AS n FROM residues GROUP BY letter
), pairs AS (
    SELECT a.letter AS first_residue, b.letter AS second_residue,
           CASE WHEN a.letter = b.letter THEN a.n * (a.n - 1) / 2
                ELSE a.n * b.n END AS observations
    FROM counts a JOIN counts b ON a.letter <= b.letter
)
SELECT * FROM pairs WHERE observations > 0
ORDER BY first_residue, second_residue;
```
<!-- scoring:toy-column-counts:end -->

![Six letters in one column produce 15 unordered sequence pairs, grouped by letter combination.](/assets/b9c0cd899a4238e71b8cbe6f9b3a369480f0e009e9c4d577216dc269f838ccd6/figures/blog/scoring-matrices/column-pair-counts.svg)

For residue $a$ occurring $n_{a,r}$ times in column $r$, the general formulas are

$$
c^{(r)}_{aa}=\binom{n_{a,r}}{2}
=\frac{n_{a,r}(n_{a,r}-1)}{2},
\qquad
c^{(r)}_{ab}=n_{a,r}n_{b,r}\quad(a<b).
$$

Add the counts across all usable columns and normalize:

$$
c_{ab}=\sum_r c^{(r)}_{ab},\qquad
T=\sum_r\binom{n_r}{2},\qquad
q_{ab}=\frac{c_{ab}}{T},
$$

where $n_r$ is the number of usable residues in column $r$. If every column contains the same $n$ sequences and there are $w$ columns, this becomes $T=wn(n-1)/2$. The column-specific version lets us exclude unresolved bases without pretending that every position has the same coverage.

### Keep ordered and unordered pairs straight

We counted A/B and B/A together. For these *unordered* frequencies, the residue background and chance expectations are

$$
p_a=q_{aa}+\frac12\sum_{b\ne a}q_{ab},
$$

$$
e_{aa}=p_a^2,\qquad
e_{ab}=2p_ap_b\quad(a\ne b).
$$

The factor of two accounts for the two possible orders of an unlike pair. With this convention, the score calculation is

$$
s_{ab}=\log_2\frac{q_{ab}}{e_{ab}},\qquad
S_{ab}=\operatorname{round}(2s_{ab}).
$$

The result agrees with the earlier ordered-pair equation: an off-diagonal ordered frequency is half its unordered counterpart. Keeping that factor consistent prevents a systematic error in every mismatch score.

A pair that never occurs despite a positive expected frequency has an unsmoothed score of negative infinity. If a residue itself is absent, its background frequency is zero and the corresponding odds are undefined. Small datasets can have both kinds of cells. We will preserve their counts and mark unobserved cells in the plots, rather than displaying them as a neutral score.

### What the BLOSUM family gives us

BLOSUM80 emphasizes closer relationships, while BLOSUM45 is commonly used for more distant ones. Each matrix is estimated from observations under its clustering threshold. BLOSUM62 became a widely used default for protein searches, including BLASTP.

The larger and more varied training collection was an advantage over the original PAM dataset, and the Henikoffs reported improved alignment and search results in their comparisons. Performance still depends on the sequences and the scoring scheme used with the matrix.

The blocks emphasize conserved regions. They provide reliable alignment columns, but they also select a particular part of protein sequence space. A matrix learned from those regions averages over many functions and structural environments. Short domains can present another difficulty: a permissive matrix aimed at distant relationships may accumulate too little information over a short alignment, or extend a good domain match into neighboring unrelated sequence.

![BLOSUM62 amino-acid scores, divided by two to display log odds in bits.](/assets/b9c0cd899a4238e71b8cbe6f9b3a369480f0e009e9c4d577216dc269f838ccd6/figures/blog/scoring-matrices/blosum62.svg)

The diagonal rewards identical residues. Off the diagonal, pairs such as L/I retain a positive score because they occur more frequently than their background abundances predict. Both reference matrix plots use the same bit scale.

## Comparing PAM and BLOSUM

| Property | PAM | BLOSUM |
| --- | --- | --- |
| Starting observations | Closely related protein families in the original dataset | Conserved ungapped blocks from a broader set of families |
| Alignment context | Global alignments and reconstructed evolutionary changes | Local blocks and weighted residue-pair counts |
| Construction | An explicit transition model | Direct observed pair frequencies |
| Other family members | Extrapolate transition probabilities from PAM1 | Repeat estimation with another clustering threshold |
| Closer relationships | Lower numbers, such as PAM40 | Higher numbers, such as BLOSUM80 |
| More distant relationships | Higher numbers, such as PAM250 | Lower numbers, such as BLOSUM45 |

One way to compare matrices is their average information per aligned pair, also called relative entropy:

$$
H=\sum_{a\le b}q_{ab}\log_2\frac{q_{ab}}{e_{ab}}.
$$

Approximate comparisons based on information content put PAM120 near BLOSUM80, PAM160 near BLOSUM60-62, and PAM250 near BLOSUM45. These pairings describe a similar overall scale of discrimination. Individual entries and the alignments they favor can differ.

Score units need attention too. Standard BLOSUM62 entries are half bits, while the standard PAM250 table is in thirds of a bit. Divide by two or three respectively when placing their values on a common color scale.

### Choose for the sequences and the alignment length

A *shallow* matrix targets relatively similar sequences. Low PAM numbers and high BLOSUM numbers belong toward this end of their respective families. A *deep* matrix allows more divergence, as with PAM250 or BLOSUM45. For protein searches, shallow models can target roughly 50-90% identity, while deep models can target similarities around 20-30%, depending on the matrix and gap settings.

If we expect close orthologs, a shallow matrix can focus the search on that range. Orthologs are genes whose relationship follows a speciation event. If we are looking for distant relatives across very different organisms, a deeper matrix can retain useful evidence from conservative replacements.

Length changes the choice. A short domain has fewer positions over which to accumulate a score. [Pearson's discussion of matrix selection](https://pmc.ncbi.nlm.nih.gov/articles/PMC3848038/) explains why shallow matrices can work better for short comparisons and how gap penalties affect the useful range of a matrix.

An initial alignment and its identity can help establish the scale of the problem, especially when close relatives are expected. Inspect the aligned region as well: a high overall identity does not reveal whether an endpoint has extended into the wrong domain. BLOSUM62 is a practical starting point for many protein searches, with the matrix changed when the expected divergence or region length gives a reason to do so.

Specialized matrices offer another option when the sequence environment differs from the mixture represented in general protein datasets. JTTtm was developed for transmembrane segments. [PHAT](https://academic.oup.com/bioinformatics/article/16/9/760/307630), short for predicted hydrophobic and transmembrane, uses observations from hydrophobic and transmembrane regions. Their relevance comes from the different residue composition and structural constraints of membrane-spanning sequence.

## Gap costs change what the matrix can do

Suppose an alignment can avoid several unfavorable substitutions by inserting a gap. Whether that helps its total score depends on both the substitution matrix and the gap cost. Low costs make it cheap to add gaps. High costs can keep unlike residues opposite one another even when an insertion or deletion is a reasonable explanation.

A constant penalty charges the same amount for every gap run:

$$
G_{\mathrm{constant}}(L)=c.
$$

A linear penalty charges for every missing residue:

$$
G_{\mathrm{linear}}(L)=eL.
$$

That is easy to compute, but it gives a five-residue deletion the same total cost as five separate one-residue gaps. An affine model distinguishes opening a gap from extending it:

$$
G_{\mathrm{affine}}(L)=o+eL.
$$

Here $o$ is the opening component and $e$ the extension cost. With $o=5$ and $e=1$, a one-residue gap costs 6 and a five-residue gap costs 10. Five separate one-residue gaps cost 30. This favors a contiguous indel over several scattered gap openings, reflecting the possibility that a whole stretch was inserted or deleted in one event.

Some software writes the same family of functions as $o'+e(L-1)$. Its opening parameter includes the first extension cost, so $o'=o+e$ makes the conventions agree. Check which convention an implementation uses before transferring parameter values. BioseqDB's [alignment scoring definition](/v0.1/tutorials/project_alignment_foundations/#eq-affine-gap-convention) uses $o+eL$.

For long indels, a two-piece affine model can use a cheaper extension slope once a gap grows beyond a crossover point:

$$
G_{\mathrm{dual}}(L)=\min(o_1+e_1L,\ o_2+e_2L).
$$

![Constant, linear, affine and two-piece affine curves charge differently for gaps of increasing length.](/assets/b9c0cd899a4238e71b8cbe6f9b3a369480f0e009e9c4d577216dc269f838ccd6/figures/blog/scoring-matrices/gap-penalties.svg)

*The second affine branch can charge more to open a gap but less for each additional base. It becomes the cheaper branch for long gaps.*

Affine scoring remains a common choice because dynamic programming can evaluate it with a small set of states per matrix cell. Two-piece affine costs and other models extend that idea when the alignment problem calls for different treatment of long gaps.

## Where these scores appear in seed-and-extend aligners

Modern nucleotide aligners frequently begin with exact matches. In [my explanation on Bioinformatics Stack Exchange](https://bioinformatics.stackexchange.com/questions/23310/how-and-why-do-alignment-algorithms-work/23781#23781), I separated the process into finding short matching pieces, joining them into chains and optionally calculating a detailed alignment. It helps to keep the scores at those stages separate.

First, an aligner extracts seeds, such as k-mers or selected combinations of short substrings, and finds matching seeds in the reference. Each matching seed provides an anchor with coordinates in both sequences. The [Python read-mapper tutorial](/blog/writing-a-dna-read-mapper-in-python/) works through this construction with code.

Chaining then finds anchors that can plausibly belong to the same alignment. It rewards added matching coverage and penalizes awkward jumps between anchor coordinates. If the query advances 100 bases while the reference advances 120, that difference suggests a gap that the chaining score should account for. The [chaining chapter](/v0.1/tutorials/project_chaining/#sec-chaining-score-model) describes these scores and the algorithms used to evaluate them.

![Seeds provide exact anchors, chaining joins their coordinates, and base alignment resolves the intervening letters.](/assets/b9c0cd899a4238e71b8cbe6f9b3a369480f0e009e9c4d577216dc269f838ccd6/figures/blog/scoring-matrices/seed-chain-alignment.svg)

*Anchor chaining can locate a corresponding region before a base-by-base alignment has been calculated.*

Minimap2 uses a concave gap cost in its chaining calculation. Its long-gap refinements can use the [minigraph chaining approach with range-minimum-query data structures](/v0.1/tutorials/project_chaining/#rmq-re-chaining-in-the-pipeline), which helps reconnect anchors across long indels. This is a geometric calculation on anchors. The two-piece affine cost described above belongs to its later base-level alignment.

At the end of chaining, an aligner already has approximate mapping coordinates. Minimap2 can output those without calculating a CIGAR string. Requesting base-level alignment makes it resolve the letters and gaps between anchors, using its KSW2 dynamic-programming implementation. The [minimap2 manual](https://lh3.github.io/minimap2/minimap2.html) distinguishes the chaining and alignment controls, and the [base-level alignment chapter](/v0.1/tutorials/project_final_alignment_postprocessing_and_preprocessing/#sec-final-alignment-role) covers the corresponding BioseqDB implementations.

Several optimizations reduce that work. Banding restricts which part of the dynamic-programming grid is evaluated. X-drop and Z-drop stop an extension when its score falls sufficiently far below a previous best. Z-drop allows a larger drop when the path moves farther from that best-scoring cell's diagonal, which helps it continue across a gap. [Block Aligner](https://pmc.ncbi.nlm.nih.gov/articles/PMC10457662/) evaluates selected regions of the grid as the alignment advances. Its [source code](https://github.com/Daniel-Liu-c0deb0t/block-aligner) shows how those blocks shift, grow and shrink.

These nucleotide alignments use nucleotide scoring rules. Protein searches have a different alphabet and can use BLOSUM during both candidate selection and detailed alignment. For example, protein word-neighborhood seeding can accept a high-scoring word that differs from the query. The [MMseqs2 user guide](https://mmseqs.com/latest/userguide.pdf) describes its protein-search stages. The seed-and-extend structure tells us how the search is organized, while the alphabet and scoring choices tell us what each stage considers similar.

## Count substitutions in 1,000 human mitochondrial genomes

We can carry the same calculation through a real dataset. I chose 1,000 complete human mitochondrial genomes from the Danish cohort published by [Li and colleagues](https://pmc.ncbi.nlm.nih.gov/articles/PMC4350597/). Mitochondria have their own small genome, about 16,600 bases long, which makes the download manageable while giving us enough samples to see repeated differences at individual positions.

The study deposited 2,000 mitochondrial sequences. We take records in accession order and retain the first 1,000 whose GenBank title explicitly says "complete genome". This excludes 11 partial records and gives a list beginning at `KF161060.1` and ending at `KF162070.1`. Each has a different isolate identifier. The examples repository includes that list and its metadata, so the sample selection stays the same when somebody reruns the tutorial.

These are mitochondrial genomes from one Danish study. Their frequencies describe that cohort, including its sampling and shared ancestry. A common variant carried by 400 people contributes 400 observations relative to the reference, even if those people inherited it from the same ancestor. That distinction will matter when we interpret the matrix.

Our reference is the [revised Cambridge Reference Sequence, `NC_012920.1`](https://www.ncbi.nlm.nih.gov/nuccore/NC_012920.1). We will align each sample to it, walk the CIGAR operations to recover corresponding positions, then count the letters in each column. The reference's gene annotations also tell us which groups of three bases form codons in its 13 protein-coding genes.

| Input | Role in the analysis |
| --- | --- |
| 1,000 complete mitochondrial sample records | The observations used to estimate the cohort frequencies |
| `NC_012920.1` | A coordinate system for alignment and coding-region annotations |
| 13 annotated coding regions | Corresponding codons for the amino-acid comparison |
| NCBI BLOSUM62 joint frequencies and scores | The protein reference matrix |
| NCBI PAM250 scores | A second historical matrix to inspect on the same bit scale |

The DNA calculation uses the four letters A, C, G and T. For the comparison with BLOSUM62 we translate the coding regions and use the 20 amino-acid alphabet. In BLOSUM, A means alanine, C cysteine, G glycine and T threonine. Keeping those two alphabets in separate tables makes the comparisons much easier to read.

### Install BioseqDB and get the example files

Follow the [installation guide](/v0.1/tutorials/getting_started/) to create a PostgreSQL database with BioseqDB. This tutorial uses its NCBI integration to download sequences through SQL.

> [!WARNING]
> **Availability on 16 October 2026.** The published `0.1.0-alpha.1` Docker images and native package builds omit the external-database integration used here (due to my oversight). It is planned for the next alpha release, but you can obviously compile source from Github directly. Currently using runtime `SET`, an API key, or a `GRANT` won't work as the code isn't present in the binary - when building one must use `BIOSEQ_BUILD_EXTERNAL_API_CLIENTS=ON` to enable this feature.

The [source-build instructions in the butterfly example](https://github.com/BioseqDB/examples/tree/main/butterfies_alignments#database-setup) use the same external module. Check that it is present before starting the download:

<!-- scoring:preflight:start -->
```sql
SELECT to_regnamespace('bioseq_external') IS NOT NULL AS external_schema,
       to_regclass('bioseq_external.sequences') IS NOT NULL AS remote_sequences;
```
<!-- scoring:preflight:end -->

Both columns should be `true`. Your database role also needs `USAGE` on `bioseq_external` and `SELECT` on `bioseq_external.sequences`, which the database owner can grant.

Get the SQL files, accession metadata and plotting code from [BioseqDB/examples/scoring_matrices](https://github.com/BioseqDB/examples/tree/main/scoring_matrices), or download the [complete example bundle](/assets/b9c0cd899a4238e71b8cbe6f9b3a369480f0e009e9c4d577216dc269f838ccd6/examples/scoring-matrices.zip):

```sh
git clone https://github.com/BioseqDB/examples.git
cd examples/scoring_matrices
export DATABASE_URL=bioseq_examples
```

Set `DATABASE_URL` to your database name. For a remote server, set `PGHOST`, `PGPORT` and `PGUSER` for your connection. Use a fresh database or one without a `scoring` schema. The pipeline keeps its intermediate tables in that schema so we can inspect each step.

### Download and inspect the sequences

Our table keeps the accession, its role and the sequence itself:

<!-- scoring:schema:start -->
```sql
CREATE SCHEMA scoring;
CREATE TABLE scoring.genomes (
    accession text NOT NULL,
    role text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
```
<!-- scoring:schema:end -->

`USING bioseq` selects BioseqDB's table storage. `bioseq.seq('dna5', 64)` stores DNA with the extra symbol N for an unresolved base. The `64` selects 64-bit packed storage units, and a sequence can occupy many of them. The [sequence representation chapter](/v0.1/tutorials/project_sequence_abstractions/) goes through the details.

Here are the requests for the reference and one sample:

<!-- scoring:import-one:start -->
```sql
INSERT INTO scoring.genomes (accession, role, seq)
SELECT 'NC_012920.1', 'reference', sequence::bioseq.seq('dna5', 64)
FROM bioseq_external.sequences
WHERE provider = 'ncbi' AND accession = 'NC_012920.1';

INSERT INTO scoring.genomes (accession, role, seq)
SELECT 'KF161060.1', 'sample', sequence::bioseq.seq('dna5', 64)
FROM bioseq_external.sequences
WHERE provider = 'ncbi' AND accession = 'KF161060.1';

SELECT accession, role, bioseq.len(seq) AS bases
FROM scoring.genomes ORDER BY role, accession;
```
<!-- scoring:import-one:end -->

The external table reads the NCBI response, and `INSERT ... SELECT` saves the sequence locally. The complete import repeats that request for the accession list and the reference. It pauses between requests and skips accessions already stored, so an interrupted download can resume.

Run the full SQL workflow with:

<!-- scoring:run-cohort:start -->
```sh
bash run.sh
```
<!-- scoring:run-cohort:end -->

The script downloads the records, checks the sample count, aligns the genomes and writes the result tables to `results/`. All the sequence processing runs in PostgreSQL. We will use Python after those CSV files exist, to draw the figures.

<!-- scoring:inspect:start -->
```sql
SELECT role, count(*) AS genomes,
       min(bioseq.len(seq)) AS shortest,
       max(bioseq.len(seq)) AS longest,
       sum(bioseq.len(seq)) AS total_bases,
       sum(length(seq::text) - length(replace(seq::text, 'N', ''))) AS unknown_n
FROM scoring.genomes GROUP BY role ORDER BY role;

SELECT accession, bioseq.len(seq) AS bases,
       length(seq::text) - length(replace(seq::text, 'N', '')) AS unknown_n
FROM scoring.genomes WHERE role = 'sample'
ORDER BY accession LIMIT 5;
```
<!-- scoring:inspect:end -->

The website evaluates the SQL listings against a saved copy of these same input records. The downloadable workflow fetches them from NCBI. This keeps the displayed results tied to the complete cohort while letting us rebuild the page without requesting the same thousand sequences every time.

### Measure N before excluding it

N records uncertainty about a nucleotide. Treating it as a fifth biological base would turn missing observations into apparent substitutions, so we count it separately and use A/C/G/T as the callable alphabet.

<!-- scoring:ambiguity:start -->
```sql
SELECT count(*) AS sample_genomes,
       sum(bioseq.len(seq)) AS total_bases,
       sum(length(seq::text) - length(replace(seq::text, 'N', ''))) AS unknown_n,
       round(100.0 * sum(length(seq::text) - length(replace(seq::text, 'N', '')))
             / sum(bioseq.len(seq)), 4) AS percent_unknown
FROM scoring.genomes WHERE role = 'sample';
```
<!-- scoring:ambiguity:end -->

The 1,000 samples contain 16,568,713 bases in total. Their 8,691 Ns account for 0.0525% of those bases. The shortest record has 16,561 bases and the longest has 16,575.

![Sequence length and number of unresolved N bases in each of the 1,000 sample records.](/assets/b9c0cd899a4238e71b8cbe6f9b3a369480f0e009e9c4d577216dc269f838ccd6/figures/blog/scoring-matrices/record-quality.svg)

Each point is one accession. Sequence lengths help us spot a truncated input, while the second panel shows how much information each record will contribute after N is excluded. The reference also contains a historical N at position 3107. We keep that coordinate during alignment and exclude it when comparing nucleotide identities.

### Align corresponding positions

An insertion changes the relationship between the two coordinate systems. If a sample has an extra base after position 309, comparing the raw strings at position 310, 311 and so on would shift every later comparison. We need the alignment path.

The query below asks BioseqDB for alignments and reads the coordinates and CIGAR attached to each match:

<!-- scoring:align-one:start -->
```sql
SELECT q.accession,
       bioseq.target_start(t.seq)::integer AS target_start,
       bioseq.target_end(t.seq)::integer AS target_end,
       bioseq.query_start(t.seq)::integer AS query_start,
       bioseq.query_end(t.seq)::integer AS query_end,
       bioseq.is_reverse(t.seq) AS is_reverse,
       bioseq.cigar(t.seq)::text AS cigar,
       bioseq.edit(t.seq) AS edits,
       bioseq.score(t.seq) AS score,
       bioseq.is_primary(t.seq) AS is_primary
FROM scoring.genomes t
JOIN scoring.genomes q ON mmsearch(t.seq, q.seq, aligner_options(
    kmer_size => 15, window_size => 10,
    max_gap => 200, min_chain_score => 40))
WHERE t.role = 'reference' AND q.role = 'sample' AND q.accession = 'KF161060.1';
```
<!-- scoring:align-one:end -->

`mmsearch` finds a match between the reference sequence and a sample. The `aligner_options` values set the minimizer and chaining parameters for these closely related genomes. Here the seeds are 15-base k-mers, with minimizers selected from windows of 10 consecutive k-mers. We request `max_gap = 200`. The default 500-base bandwidth raises the effective chaining distance to 500 bases, while alignment end extensions remain capped at 200. A minimum chain score of 40 filters weak candidates. `bioseq.target_start` and `bioseq.target_end` locate the reference interval, while the query accessors locate the corresponding sample interval. These intervals are zero-based and half-open.

`bioseq.cigar` describes how the two intervals line up. An `M` consumes a base from both sequences and can contain either a match or a mismatch. An `I` consumes a sample base without advancing the reference, and a `D` advances the reference without consuming a sample base. The letters themselves determine which `M` positions differ.

The pipeline retains the best forward primary alignment for each accession and checks that it consumes the full sample and covers at least 99.9% of the reference. A failed check stops the run. In this cohort 999 alignments cover the whole reference. One leaves seven terminal reference positions uncovered, which we record as gaps. The [butterfly alignment tutorial](/blog/comparing-butterfly-genomes-with-bioseqdb/) explains these coordinate fields with a genomic ribbon plot, and the [final-alignment chapter](/v0.1/tutorials/project_final_alignment_postprocessing_and_preprocessing/) describes how BioseqDB obtains the CIGAR.

### Turn the CIGAR into aligned columns

`sql/05_columns.sql` splits each CIGAR into numbered runs. Two running sums track how far we have moved in the reference and sample. For a run consuming both sequences we copy the sample substring, for a deletion we write a gap, and for an insertion we retain its position in the CIGAR table but leave it out of the reference-column string.

This gives every sample a string with the reference's length. Character 500 now means the sample base aligned to reference position 500. We can group those strings by position and base:

<!-- scoring:aligned-pairs:start -->
```sql
SELECT p.pos::integer AS position1, p.base, count(*) AS samples
FROM scoring.projected_sequences s
CROSS JOIN LATERAL regexp_split_to_table(s.aligned, '')
    WITH ORDINALITY AS p(base, pos)
GROUP BY p.pos, p.base
ORDER BY p.pos, p.base LIMIT 12;

SELECT base, sum(n) AS aligned_calls,
       base IN ('A', 'C', 'G', 'T') AS included_in_matrix
FROM scoring.site_counts
GROUP BY base ORDER BY base;
```
<!-- scoring:aligned-pairs:end -->

An inserted sample base has no reference column, so we report insertions separately. Of the 8,691 raw Ns, 75 occur in inserted sequence and 8,616 remain in the projected columns. The 1,250 gap calls consist of 1,243 deleted bases and the seven uncovered terminal reference positions. This reference-anchored arrangement makes the count at each position easy to inspect.

![Numbers of sample differences, N calls and gaps along the mitochondrial reference.](/assets/b9c0cd899a4238e71b8cbe6f9b3a369480f0e009e9c4d577216dc269f838ccd6/figures/blog/scoring-matrices/genome-differences.svg)

The upper panel counts samples whose callable base differs from the reference. A tall line means many samples carry another base at that coordinate. The lower panel separates N calls from gaps, which helps distinguish a well-observed variant from a region with missing information.

## Build the nucleotide matrix in SQL

At a column containing four As, one C and one G, there are six A/A pairs, four A/C pairs, four A/G pairs and one C/G pair. We can compute those counts from the letter totals without joining every sample to every other sample.

For the SQL implementation we count both orientations of each pair. Thus A/C and C/A each receive four observations, and A/A receives $4(4-1)=12$. This ordered convention gives us a symmetric $4\times4$ table and the simple expected frequency $p_a p_b$.

<!-- scoring:dna-pairs:start -->
```sql
SELECT a.base AS base_from,b.base AS base_to,
       sum(CASE WHEN a.base=b.base THEN a.n*(a.n-1)
                ELSE a.n*b.n END)::bigint AS pairs
FROM scoring.site_counts a JOIN scoring.site_counts b USING(position1)
JOIN scoring.reference_bases r USING(position1)
WHERE r.base IN ('A','C','G','T') AND a.base IN ('A','C','G','T') AND b.base IN ('A','C','G','T')
GROUP BY a.base,b.base
ORDER BY base_from, base_to;
```
<!-- scoring:dna-pairs:end -->

For a diagonal cell we use $n_a(n_a-1)$, which removes a sample's comparison with itself. For different letters we use $n_a n_b$. Summing those counts across columns produces the same totals as enumerating every distinct sample pair, while each column needs at most 16 combinations of letter counts.

With 1,000 samples there are $\binom{1000}{2}=499{,}500$ unordered sample pairs at a fully callable position. That number falls wherever N or a gap removes a sample from the column. The denominator in the query comes from the retained counts themselves.

Now divide each pair count by the total, calculate the marginal frequencies and take the log odds:

<!-- scoring:dna-matrix:start -->
```sql
WITH frequencies AS (
    SELECT *, pairs::numeric / sum(pairs) OVER () AS q
    FROM scoring.nucleotide_pairs
), marginals AS (
    SELECT base_from AS base, sum(q) AS p
    FROM frequencies GROUP BY base_from
)
SELECT f.base_from, f.base_to, pairs,
       round(q, 6) AS q,
       round(a.p * b.p, 6) AS expected,
       CASE WHEN q > 0 THEN
           round(2 * log(2::numeric, q / (a.p * b.p)), 2)
       END AS score_half_bits
FROM frequencies f
JOIN marginals a ON a.base = f.base_from
JOIN marginals b ON b.base = f.base_to
ORDER BY base_from, base_to;
```
<!-- scoring:dna-matrix:end -->

`q` is the observed ordered pair frequency. The two marginal frequencies give `expected = p_a * p_b`, and `2 * log(2, q / expected)` expresses the score in half-bit units, the convention used by BLOSUM62. The chart divides that value by two so its color scale is in bits.

![Four-by-four nucleotide log-odds matrix calculated from the 1,000 mitochondrial genomes.](/assets/b9c0cd899a4238e71b8cbe6f9b3a369480f0e009e9c4d577216dc269f838ccd6/figures/blog/scoring-matrices/nucleotide-scores.svg)

All 16 nucleotide pairs occur in the data. The diagonal entries are positive, while off-diagonal scores are negative because differences between these closely related samples are much rarer than chance pairing of their base frequencies. Across the reference-aligned columns, 1,565 positions contain more than one observed canonical base.

These samples are closely related, so the diagonal contains most observations. The matrix describes their aligned base-pair frequencies, including unchanged positions. The numbers would change if we selected another population, another genomic region or a more divergent collection.

### Separate the destination from the opportunity to differ

There are two useful ways to read an A-to-G count. We can divide by all callable sample observations at reference A positions, which tells us how often an A position differs as G. Or we can divide by all differences from reference A, which tells us how much of that difference spectrum goes to G rather than C or T.

Writing $C_{a\to b}$ for the reference-to-sample count, the first fraction is

$$
D_{a\to b}=\frac{C_{a\to b}}{\sum_{c\in\{A,C,G,T\}}C_{a\to c}}.
$$

For a destination among differences, with $b\ne a$, we use

$$
R_{a\to b}=\frac{C_{a\to b}}{\sum_{c\ne a}C_{a\to c}}.
$$

Both are useful, provided the denominator is clear. This query reports the counts and both percentages:

<!-- scoring:dna-ratios:start -->
```sql
SELECT reference_base, sample_base, calls,
       round(100 * fraction_of_calls, 4) AS percent_of_callable_positions,
       round(100 * fraction_of_differences, 2) AS percent_of_differences
FROM scoring.reference_spectrum
WHERE reference_base <> sample_base
ORDER BY reference_base, sample_base;
```
<!-- scoring:dna-ratios:end -->

![Four stacked bars showing destinations among differences from each reference nucleotide.](/assets/b9c0cd899a4238e71b8cbe6f9b3a369480f0e009e9c4d577216dc269f838ccd6/figures/blog/scoring-matrices/nucleotide-destinations.svg)

Each bar sums to 100% of the differences from its reference letter. A/G and C/T exchanges are called transitions, while exchanges between those two chemical groups are transversions. The chart lets us see those patterns without the much larger number of unchanged bases filling almost the whole plot.

For reference A, 98.70% of the observed differences are G. The corresponding transition fractions are 91.17% for C to T, 98.48% for G to A and 98.95% for T to C. There are 23,206 differences in 16,559,133 callable reference/sample comparisons, about 0.1401%.

The arrow here means reference base to sample base. Inferring which state was ancestral would require an evolutionary analysis, so the plotted quantities are frequencies of observed differences relative to rCRS.

We can also use the cohort matrix itself, treating each sample as a possible first partner rather than choosing rCRS. The same normalization gives the following proportions:

<!-- scoring:dna-partners:start -->
```sql
SELECT base_from, base_to,
       round(100 * fraction_of_differences, 2) AS percent_of_differences
FROM scoring.nucleotide_matrix
WHERE base_from <> base_to
ORDER BY base_from, base_to;
```
<!-- scoring:dna-partners:end -->

![Partner nucleotides among differing pairs across all 1,000 samples.](/assets/b9c0cd899a4238e71b8cbe6f9b3a369480f0e009e9c4d577216dc269f838ccd6/figures/blog/scoring-matrices/nucleotide-partners.svg)

Here A/G accounts for 94.23% of differing pairs with A, and C/T accounts for 94.26% of those with C. The frequencies change because all sample pairs contribute. A variant that is common in the cohort but differs from rCRS receives different weight in these two summaries.

## Translate the coding regions for the BLOSUM comparison

Our nucleotide matrix has four letters. BLOSUM62 has 20 amino acids, so the next table starts from the reference's annotated coding sequences rather than from arbitrary three-base chunks of the genome.

`sql/07_proteins.sql` stores each gene's interval, strand and annotated reference translation. Twelve coding regions lie on the forward strand. ND6 is on the reverse strand, so its projected bases are reverse-complemented before reading its codons. The translation table is [NCBI genetic code 2](https://www.ncbi.nlm.nih.gov/Taxonomy/Utils/wprintgc.cgi#SG2), the vertebrate mitochondrial code.

That choice changes real amino-acid calls. For example, TGA encodes tryptophan in this code, while ATA encodes methionine. The script also handles the annotated initiation position and uses the reference protein length to leave incomplete terminal stop codons outside the amino-acid table.

Before translating a sample's coding region, we exclude that whole region if its alignment is incomplete or contains an insertion or deletion. A premature stop, an invalid initiation codon or an unresolved initiation codon also removes the region from the protein pair counts. The remaining codons give a conservative set of directly corresponding amino-acid positions. We count ambiguous codons separately, and keep the per-gene exclusions visible:

<!-- scoring:protein-quality:start -->
```sql
SELECT c.gene, count(*) AS sample_cds,
       count(*) FILTER (WHERE has_indel) AS indel_cds,
       count(*) FILTER (WHERE incompletely_aligned) AS incomplete_cds,
       count(*) FILTER (WHERE q.premature_stop) AS stop_cds,
       count(*) FILTER (WHERE q.invalid_start) AS invalid_start_cds,
       count(*) FILTER (WHERE q.ambiguous_start) AS ambiguous_start_cds,
       count(*) FILTER (WHERE NOT has_indel AND NOT incompletely_aligned
                       AND NOT q.premature_stop AND NOT q.invalid_start
                       AND NOT q.ambiguous_start) AS retained_cds
FROM scoring.cds_quality c
LEFT JOIN scoring.protein_cds_quality q USING (accession, gene)
GROUP BY c.gene ORDER BY c.gene;

SELECT count(*) AS eligible_codons,
       count(*) FILTER (WHERE aa IS NULL) AS ambiguous_codons,
       count(*) FILTER (WHERE aa IS NOT NULL AND aa <> '*') AS callable_codons,
       count(*) FILTER (WHERE aa = 'N') AS asparagine_codons
FROM scoring.amino_acids a
JOIN scoring.protein_cds_quality q USING (accession, gene)
WHERE NOT q.premature_stop AND NOT q.invalid_start AND NOT q.ambiguous_start;
```
<!-- scoring:protein-quality:end -->

![Retained and excluded sample coding regions for the 13 mitochondrial protein-coding genes.](/assets/b9c0cd899a4238e71b8cbe6f9b3a369480f0e009e9c4d577216dc269f838ccd6/figures/blog/scoring-matrices/protein-coverage.svg)

The code checks its translation of the reference against all 13 annotated NCBI proteins. This catches strand, frame and genetic-code mistakes before they reach the matrix.

An N in a DNA sequence means an unresolved base. An N in an amino-acid sequence means asparagine, which is one of our 20 ordinary residues. We therefore exclude ambiguous DNA codons while retaining amino-acid N.

### Count amino-acid pairs

The counting query follows the nucleotide version, grouping by gene and codon position first. This listing shows the leucine row; the script computes all 400 cells:

<!-- scoring:protein-pairs:start -->
```sql
WITH observed AS (
    SELECT a.aa AS aa_from,b.aa AS aa_to,
       sum(CASE WHEN a.aa=b.aa THEN a.n*(a.n-1) ELSE a.n*b.n END)::bigint AS pairs
    FROM scoring.protein_site_counts a JOIN scoring.protein_site_counts b USING(gene,codon1)
    GROUP BY a.aa,b.aa
), alphabet AS (SELECT unnest(string_to_array('A R N D C Q E G H I L K M F P S T W Y V',' ')) AS aa)
SELECT a.aa AS aa_from,b.aa AS aa_to,coalesce(o.pairs,0)::bigint AS pairs
FROM alphabet a CROSS JOIN alphabet b
LEFT JOIN observed o ON o.aa_from=a.aa AND o.aa_to=b.aa
WHERE a.aa = 'L' ORDER BY b.aa;
```
<!-- scoring:protein-pairs:end -->

Longer proteins contribute more columns because we pool the accepted codons across all 13 genes. Within each column, every retained sample has equal weight. The resulting cohort frequencies therefore reflect both the protein lengths and the number of callable samples at each position.

<!-- scoring:protein-matrix:start -->
```sql
SELECT aa_from, aa_to, pairs,
       round(score_half_bits, 2) AS score_half_bits,
       round(100 * fraction_of_differences, 4) AS percent_of_differences
FROM scoring.protein_matrix
WHERE aa_from = 'L'
ORDER BY aa_to;
```
<!-- scoring:protein-matrix:end -->

![Twenty-by-twenty amino-acid log-odds matrix from corresponding mitochondrial coding positions.](/assets/b9c0cd899a4238e71b8cbe6f9b3a369480f0e009e9c4d577216dc269f838ccd6/figures/blog/scoring-matrices/protein-scores.svg)

The slashed cells mark pairs we never observed. For a zero observed frequency, the unsmoothed log odds is $-\infty$. The SQL leaves those scores empty rather than replacing them with an arbitrary finite penalty. Smoothing would require an explicit prior and its weight, especially for rare pairs.

All 13 genes retain coding regions from the cohort. The start-codon filter removes 14 sample/gene combinations: one ND5 region starts with GTA, and 13 other regions have an unresolved first codon. Among the retained regions, 1,540 ambiguous codons are excluded, leaving 3,784,719 callable amino-acid observations. Amino-acid N remains present in 163,715 of those observations.

There are 317 positions with more than one observed amino acid. Of the 400 ordered matrix cells, 284 have no observed pair, which is why much of this heatmap is slashed. A thousand closely related samples still provide limited evidence about many possible amino-acid exchanges.

This calculation uses BLOSUM's log-odds idea with our cohort's frequencies. Actual BLOSUM62 construction includes clustering at its identity threshold to control redundant sequences. These human mitochondrial proteins are so similar that applying that threshold would merge much of our cohort, leaving very little independent between-cluster evidence. Equal sample weights instead answer the question we asked here: which aligned residue pairs occur in these thousand records?

### Compare the differing amino-acid partners

The original BLOSUM62 joint probabilities are available in [NCBI's BLAST source](https://github.com/ncbi/ncbi-cxx-toolkit-public/blob/main/src/algo/blast/composition_adjustment/matrix_frequency_data.c). They retain the frequency information that is lost when scores are rounded to integers. The example bundle includes those reference tables and their source information.

For each amino acid $a$, we remove its identical partner from the denominator and normalize the remaining ordered joint frequencies:

$$
P(b\mid a,\ b\ne a)=\frac{Q_{ab}}{\sum_{c\ne a}Q_{ac}}.
$$

Apply that equation to both the cohort and BLOSUM62, and each row with observed differences becomes a distribution over differing partners. For example, the L row asks how much of the leucine pairing with a different residue involves isoleucine, valine and the other amino acids.

<!-- scoring:protein-ratios:start -->
```sql
SELECT p.aa_from, p.aa_to,
       round(100 * p.fraction_of_differences, 4) AS cohort_percent,
       round(100 * b.fraction_of_differences, 4) AS blosum62_percent
FROM scoring.protein_matrix p
JOIN scoring.reference_protein_matrix b USING (aa_from, aa_to)
WHERE b.matrix = 'BLOSUM62'
  AND p.aa_from = 'L' AND p.aa_to <> 'L'
ORDER BY p.aa_to;
```
<!-- scoring:protein-ratios:end -->

![Conditional amino-acid partner frequencies calculated from NCBI's original BLOSUM62 probabilities.](/assets/b9c0cd899a4238e71b8cbe6f9b3a369480f0e009e9c4d577216dc269f838ccd6/figures/blog/scoring-matrices/protein-partners-blosum62.svg)

![Conditional amino-acid partner frequencies calculated from the mitochondrial cohort.](/assets/b9c0cd899a4238e71b8cbe6f9b3a369480f0e009e9c4d577216dc269f838ccd6/figures/blog/scoring-matrices/protein-partners-cohort.svg)

Both plots use the same alphabet and percentage scale. Open either matrix to inspect it at full size. Read across a row to compare the destinations. The underlying joint frequencies are symmetric, while these conditional rows can differ between directions because their denominators differ. In this cohort W, tryptophan, has no observed differing partner, so its denominator is zero and the whole row is undefined, shown by slashes.

The leucine row gives a visible example. In the cohort, isoleucine accounts for 51.30% of differing partners of leucine and phenylalanine for 44.57%. The corresponding BLOSUM62 proportions are 18.43% and 8.77%. Alanine pairs with threonine in 88.61% of its differing cohort pairs, compared with 7.05% in BLOSUM62.

The average log odds is also different: 3.985 bits per aligned pair for the cohort proteins, compared with 0.698 bits for BLOSUM62. That larger value reflects how concentrated the cohort observations are on identical and a few differing pairs. It describes these frequencies, rather than measuring how well the matrix would perform in a search for distant proteins.

BLOSUM62 pools conserved blocks from many protein families at much greater divergence. Our matrix pools a single cohort's mitochondrial proteins, most of which sit in membrane complexes. The two collections give different evidence about amino-acid pairing. The observed differences are a reason to examine the source of a scoring matrix before applying it to a particular alignment task.

## Plot the SQL results with Python

At this point the CSV files contain the counts, marginal frequencies, log odds and conditional fractions. Python reads them and chooses how to display the axes and colors.

```sh
python3 -m venv .venv
.venv/bin/pip install matplotlib==3.10.7
.venv/bin/python render_charts.py --results results --output figures
```

<!-- scoring:python-plots:start -->
```python
from pathlib import Path
from matplotlib.colors import TwoSlopeNorm
import numpy as np
from render_charts import DNA, SCORE_MAP, figure, matrix, rows, save

records = rows(Path('results/nucleotide_matrix.csv'))
scores = matrix(records, DNA, 'base_from', 'base_to', 'score_half_bits')
fig, ax = figure(width=7, height=6)
edges = np.arange(len(DNA) + 1) - 0.5
image = ax.pcolormesh(edges, edges, scores, cmap=SCORE_MAP,
                      norm=TwoSlopeNorm(vcenter=0), shading='flat')
ax.invert_yaxis()
ax.set_xticks(range(4), DNA)
ax.set_yticks(range(4), DNA)
ax.set_xlabel('Partner nucleotide')
ax.set_ylabel('First nucleotide')
fig.colorbar(image, ax=ax, label='Log-odds score (half bits)')
fig.tight_layout()
save(fig, Path('nucleotide-matrix.svg'),
     'Human mitochondrial nucleotide scores',
     'Log-odds scores from aligned nucleotide pairs in the 1,000-genome cohort.')
```
<!-- scoring:python-plots:end -->

The complete plotter writes the matrix heatmaps, nucleotide bars, quality plots and coordinate profiles shown here. Its SVG backgrounds are transparent, with separate text and accent colors for light and dark mode. Each plot keeps its numerical scale in its legend.

### From pairwise differences to a distance table

A substitution score adds evidence across an alignment. To describe how much two of these samples differ, we can also calculate their mismatch fraction directly:

$$
d_p=\frac{\text{differing aligned canonical bases}}{\text{aligned positions callable in both samples}}.
$$

The pipeline calculates this for eight selected accessions, using the projected alignment columns and removing N or gaps separately for each sample pair:

<!-- scoring:distances:start -->
```sql
WITH chosen AS MATERIALIZED (
    SELECT p.accession,r.pos::integer AS position1,r.base
    FROM scoring.projected_sequences p
    CROSS JOIN LATERAL regexp_split_to_table(p.aligned,'') WITH ORDINALITY r(base,pos)
    WHERE p.accession IN (SELECT 'KF'||(161060+142*i)||'.1' FROM generate_series(0,7) x(i))
), counts AS (
    SELECT a.accession AS sample_from,b.accession AS sample_to,
           count(*) AS compared_sites,count(*) FILTER(WHERE a.base<>b.base) AS differences
    FROM chosen a JOIN chosen b USING(position1)
    JOIN scoring.reference_bases r USING(position1)
    WHERE a.base IN ('A','C','G','T') AND b.base IN ('A','C','G','T')
      AND r.base IN ('A','C','G','T')
    GROUP BY a.accession,b.accession
)
SELECT *,round(differences::numeric/compared_sites, 6) AS p_distance FROM counts
ORDER BY sample_from, sample_to;
```
<!-- scoring:distances:end -->

![Pairwise callable-site mismatch percentages for eight sample mitochondrial genomes.](/assets/b9c0cd899a4238e71b8cbe6f9b3a369480f0e009e9c4d577216dc269f838ccd6/figures/blog/scoring-matrices/sample-distances.svg)

This distance table is the kind of input a distance-based tree method can use. A phylogenetic analysis can then choose a substitution model, estimate branch lengths and test how strongly the data support the relationships. Models that correct for repeated changes become more relevant as divergence grows. A raw BLOSUM alignment score alone does not specify that evolutionary distance: alignment length, composition and the scoring settings all affect it.

## What to try with the tables

The 1,000 complete mitochondrial records give us a nucleotide matrix with a strong transition pattern and a protein matrix concentrated on a small set of observed exchanges. We have also kept the missing observations visible: 8,691 raw nucleotide Ns, alignment gaps and the coding-region exclusions all have their own counts.

PAM and BLOSUM both turn substitution evidence into additive alignment scores, with different choices about their input alignments and how they represent divergence. Gap penalties then determine how those substitution scores compete with opening or extending an insertion or deletion. That combination decides which alignment path wins.

In our database the counts remain beside the accession list, coding annotations and alignment paths. You can select one mitochondrial gene, change which sample records contribute, or inspect the coordinates behind a particular matrix cell with another SQL query. Recalculate its marginal frequencies as well as its pair counts, then rerun the plotter to see how the result changes.

## References and further reading

The scoring-matrix explanations follow the topics in [Scoring Matrices in Bioinformatics: PAM, BLOSUM, Log-Odds, Gap Penalties](https://microbenotes.com/scoring-matrices-bioinformatics/). The [ForensicMCQ guide](https://forensicmcq.com/blosum-matrices-in-forensic-biology-a-step-by-step-guide-to-protein-alignment-scoring/) supplies the small log-odds and alignment-score examples, and the [Columbia course presentation](https://www.cs.columbia.edu/4761/assignments/assignment1/reference1) is useful for working through the column counts by hand.

For the original construction, read [Henikoff and Henikoff, 1992](https://pmc.ncbi.nlm.nih.gov/articles/PMC50453/). [Pearson, 2013](https://pmc.ncbi.nlm.nih.gov/articles/PMC3848038/) discusses matrix selection and alignment length. [Trivedi and Nagarajaram, 2020](https://pmc.ncbi.nlm.nih.gov/articles/PMC7586916/) covers general and specialized protein matrices, and [Mount, 2008](https://doi.org/10.1101/pdb.top39) discusses using BLOSUM in sequence alignments.

The cohort records come from [Li and colleagues' study of 2,000 Danish mitochondrial genomes](https://pmc.ncbi.nlm.nih.gov/articles/PMC4350597/). NCBI provides the [reference sequence and its annotations](https://www.ncbi.nlm.nih.gov/nuccore/NC_012920.1), the [vertebrate mitochondrial genetic code](https://www.ncbi.nlm.nih.gov/Taxonomy/Utils/wprintgc.cgi#SG2), and the [reference matrix frequencies](https://github.com/ncbi/ncbi-cxx-toolkit-public/blob/main/src/algo/blast/composition_adjustment/matrix_frequency_data.c).
