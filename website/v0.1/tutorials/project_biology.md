---
title: "Biological foundations of sequence analysis"
slug: project_biology
description: "Biological foundations of sequence analysis"
order: 30
keywords: [BioseqDB, project]
draft: false
---

## Biological context

<a id="sec-biology-sequences"></a>

"Significant scrutiny and attention have been given to genome-sequencing costs and how they are calculated since the beginning of the field of genomics in the late 1980s". \[[22](../bibliography/#ref-nhgri_sequencing_cost_2026)\] NHGRI (National Human Genome Research Institute) has carefully tracked costs per genome and expose this data publically.

In the last 15 years, there has been seen a massive development of sequencing technologies. Companies like Applied Biosystems, Pacific Biosciences, Illumina, Roche have developed number of sequencing platforms, e.g. SOLiD, GS FLX, and PacBio. These tools are able to generate an enormous amount of data in very short period of time. As an example, Illumina NextSeq can generate up to 360 Gbp (Gbp means here sequence of size of $10^9$ symbols) in approximately 2 days. \[[14](../bibliography/#ref-illumina_website)\]

Although those platforms vary technologically, the general idea of sequencing is as follow:

1.  DNA is fragmented through some specific physical and chemical methods, then adapter sequences are appended to the ends of the fragments.

2.  Fragments of unwanted size are removed

3.  The entire set of fragments is physically fixed on chemical bead.

4.  Amplification, where we duplicate fragments, is performed for example through PCR emulsion

5.  A combination of optical devices and fluorophores (light-sensitive chemicals) is used to make reads (reading of a fragment).

With new advancements in sequencing technologies, the cost of sequencing a genome has been decreasing significantly to the point where most popular open databases store more than 10s of petabytes of data. To make this data usesable researchers often need to perform search operations:

- For bioinvestigative resarch: does this genomic sequence already contains known genes? Does it contain a known mutation? Is it a known pathogen?

- For evolutionary biology: is this genomic sequence a known evolutionary lineage? What is the genetic distance between this sequence and a known sequence?

- For legal research: is this DNA sample from known individual? Does it represent known kinship to someone else?

This search operation is using an operation called "sequence alignment" which is the core concept of this project.

### DNA, ribonucleic acid (RNA), and protein alphabets

DNA consists of nucleotides with 4 types of bases: adenine (A), cytosine (C), guanine (G) and thymine (T). Their symbols form an alphabet that I will operate on: $$\Sigma_{\mathrm{DNA4}} =
  \{\mathtt{A},\mathtt{C},\mathtt{G},\mathtt{T}\}.$$

Four symbols are sufficient for a DNA string with no unknown positions, but in practice sequencing may leave some nucleotides unresolved (for example, due to noise). The symbol $\mathtt{N}$ represents an unknown nucleotide at a specific position.

RNA uses the same alphabet as DNA except that $\mathtt{T}$ is replaced by $\mathtt{U}$. Many RNA molecules are transcribed from DNA. \[[24](../bibliography/#ref-crick_central_dogma_1970)\]

A protein sequence is a sequence that uses 20 common amino-acid symbols. Its alphabet is larger, but we can construct a possible DNA coding sequence for it. This sequence is generally not unique because several codons can encode the same amino acid.

In BioseqDB I will primarily focus on DNA4 and DNA5 support. Other alphabets for proteins or RNA are currently not supported.

| Sequence type | Symbols (alphabet) |
|:---|:---|
| DNA4 | $\mathtt{A,C,G,T}$ |
| RNA4 | $\mathtt{A,C,G,U}$ |
| DNA5 | $\mathtt{A,C,G,T,N}$ |
| RNA5 | $\mathtt{A,C,G,U,N}$ |
| PROT20 | Twenty amino-acid symbols, each encoded by a triplet of DNA bases |

*Biological alphabets*

<a id="tab-biological-alphabets"></a>

## Alignment operation

Due to nature of biological sequences, search must be able to match fragments that are non-exact, thus alignment operation is similar to popular fuzzy-search operations on normal text. The alignment can be viewed as a processes that optimises gaps (insertions and deletions) in a way to optimise similarity metric. High similarity score between parts of sequences can indicate that they share same evolutionary origin or have a common purpose. By comparing newly found sequences to already researched ones, we can deduce lots of relevant biological data. \[[20](../bibliography/#ref-zpp_bio)\]

We will consider finite nonemptystrings over the DNA4 alphabet. A gap is a symbol indicated by `-`. An alignment is a process of adding gaps to the sequences so that they are of the same length and at the same time, there is no index where a gap occurs in both sequences.

As an example, let us present an alignment of $GATCAGATA$ and $CATTCAATCA$. One of the possible alignments is:

<a id="fig-biology-alignment-example"></a>

![01 biology diagram](/static/figures/project/chapters-01-biology-tikz-01.svg)

*Alignment of the target sequence `GATCAGATA` and query sequence `CATTCAATCA`. Orthogonal connectors map each ungapped sequence position to its aligned column; gaps are intentionally unboxed.*

There are 4 cases that can occur during alignments:

1.  Insertion - a gap has been added to the first sequence

2.  Deletion - a gap has been added to the second sequence

3.  Mismatch - letters at a given index are different

4.  Match - letters at a given index are the same

## Technical considerations

A typical workflow for sequence analysis includes $3$ data formats - FASTA, FASTQ, and SAM/BAM. Reference genome, stored in FASTA, is aligned with millions of reads in the FASTQ file and the result of alignment is saved as SAM/BAM file. The reason for the reference genome being in FASTA and reads in FASTQ is that we assume that the reference genome is correct, but reads produced by sequencers can contain various errors. Moreover, we’re more likely to score alignments with high-quality bases higher than the ones with low-quality.

### FASTA

FASTA is a text-based file format for storing nucleotide sequences. It is one of the most widely used formats in bioinformatics. The current standard specification is given by NCBI. Below is a description of the usually encountered version.  
It consists of a series of entries separated by a new line. Each entry starts with a single-line header. The header starts with `>` and contains various metadata about the following sequence, like the description of the sequence, library accession number, and submission date. After the header comes the sequence itself represented with standard IUB/IUPAC acid codes, split into lines of length usually not exceeding $80$ characters. Aside from A, C, T, G commonly encountered is code N, which means any of A/C/T/G. Lowercase letters are treated the same as uppercase. The usual extension is .fasta and .fa. An example FASTA file:

```text
>PartOfHumanGenome|HG_9357/2022|2022-03-01
ACTTTGAGCTAGCATCGATTGGATCAGATGCAT
ACTCATCGGCAGTCGCATCAGCTTGGACTACGT
>PartOfMouseGenome|MG_5417/2019|2019-08-14
ACTATAGGAGCAGANNNNNNNNCTAGAGACATC
TCTGCGTNTNTTCCGGACGTACGTAGCATAGTC
```

The advantages of this format are simplicity and human readability. The downside is that it is not optimal in terms of size. To adhere to this issue, compression software is used, like gzip.

### FASTQ

FASTQ is an extension of the FASTA format. It enhances FASTA by adding quality scores of sequences. It is a standard output format of sequencers.  
Each entry consists of $4$ lines. The first line starts with `@` and contains metadata about the sequence. The second line contains the sequence itself. The third line starts with `+` and may contain some additional metadata. The fourth line contains quality scores, represented by ASCII characters with codes from $33$ to $126$, one character per nucleotide. The file extension used is .fq and .fastq. Again, gzip can be used to reduce file size. An example FASTQ file:

```text
@HumanGenomeSequencerRead
ACTACTAGACGG
+Genome20200301
*!]^#[$*@*!(
```

The quality score is usually a phread score, meaning $Q$ satisfies $Q = -10 \log_{10} p$, where $p$ is the probability of a corresponding nucleotide being incorrect. ASCII codes from $33$ to $126$ are mapped from $0$ to $93$.

### CIGAR

CIGAR is an acronym for Concise Idiosyncratic Gapped Alignment Report and has been originally defined by the Exonerate alignment program. The CIGAR format is designed to contain the minimal information necessary for the reconstruction of an alignment. This format does not include nucleotides. \[[19](../bibliography/#ref-cigar_nbis_explained)\]

- M - match or mismatch

- D - deletion

- I - insertion

- S - soft clipping. Can occur only at the beginning or at the end of the alignment. Indicates that nucleotides are clipped from the alignment.

- H - hard clipping. The same as soft clipping, except that the clipped part is not present in the SEQ field of the SAM file.

- N - skipped region. Used for representing introns (part of the gene that is non-coded) in the alignment.

- = - match

- X - mismatch
