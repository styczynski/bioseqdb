---
title: Working with sequence files in BioseqDB and SeqKit
slug: seqkit-workflows-in-bioseqdb
description: Import mitochondrial DNA from UCSC into PostgreSQL, then compare BioseqDB SQL with SeqKit for sequence statistics, GC content, motifs, slicing, sorting and FASTA housekeeping.
date: '2026-10-19'
author: Piotr Styczyński
category: bioseqdb
keywords: [BioseqDB, SeqKit, PostgreSQL, SQL, FASTA, UCSC, mitochondrial DNA, GC content, sequence analysis, bioinformatics, regular expressions]
cover: /static/figures/blog/sequence-sql-cover.svg
image_alt: Large translucent DNA letters behind SELECT, WHERE and GROUP BY statements.
draft: false
---

A FASTA header can carry quite a lot of information. The sequence has a name, somebody adds an organism, then an assembly and perhaps some coordinates, and eventually you need a little regular expression just to recover something that could have been a column in a table.

A FASTA record starts with a header line beginning with `>`, followed by the sequence letters on one or more lines.

I like having those columns. This is one reason I wanted sequence operations inside PostgreSQL when I started working on BioseqDB. You can keep the DNA next to its metadata and write a query that uses both, without exporting another intermediate file every time you change a filter.

[SeqKit's tutorial](https://bioinf.shenwei.me/seqkit/tutorial/) has plenty of useful examples of the work people do with sequence files. I went through it and the [FAQ](https://bioinf.shenwei.me/seqkit/faq/) and picked a set of tasks to try in SQL: checking lengths, counting bases, finding motifs, selecting records and dealing with names. Below, BioseqDB is on the left and SeqKit is on the right. On a phone, the SQL comes first.

We will use the same sequences in both tools. These examples were checked against SeqKit 2.14.0 and an external-enabled BioseqDB development build. The tables under the SQL listings are produced by the website builder, which executes the queries against PostgreSQL. This is a workflow comparison, with no timing benchmark attached to it.

## 1. Get a small dataset from UCSC

For this exercise I chose three mitochondrial chromosomes: human, mouse and zebrafish. Mitochondria have their own DNA, separate from the much larger nuclear chromosomes, so we get complete chromosome records small enough to inspect without waiting for a large download. These three records total 49,464 bases.

Our source is the [UCSC Genome Browser API](https://genome.ucsc.edu/goldenPath/help/api.html). BioseqDB's external tables can fetch sequence intervals from UCSC directly through SQL. We will use assembly names `hg38`, `mm39` and `danRer11`, each with chromosome `chrM`.

> [!WARNING]
> **Availability on 16 October 2026.** The published `0.1.0-alpha.1` Docker images and native package builds omit the external-database integration used here (due to my oversight). It is planned for the next alpha release, but you can obviously compile source from Github directly. Currently using runtime `SET`, an API key, or a `GRANT` won't work as the code isn't present in the binary - when building one must use `BIOSEQ_BUILD_EXTERNAL_API_CLIENTS=ON` to enable this feature.

Start with the [installation instructions](/v0.1/tutorials/getting_started/). The source-build procedure in the [butterfly example](https://github.com/BioseqDB/examples/tree/main/butterfies_alignments#database-setup) also applies here. In an already configured source checkout, enable the external module before building and installing:

```sh
cmake -S . -B build/seqkit -DBIOSEQ_BUILD_EXTERNAL_API_CLIENTS=ON
cmake --build build/seqkit --target postgresql_bioseq postgresql_bioseq_cache
```

Connect to a fresh database where that build is installed, run <code>CREATE EXTENSION bioseq;</code>, then check for the external sequence table:

<!-- seqkit:preflight:start -->
```sql runnable id=seqkit_preflight timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
-- bioseq:visible
SELECT to_regclass('bioseq_external.sequences') IS NOT NULL
       AS external_sequences_available;
-- bioseq:teardown
SELECT 1;
```
<!-- seqkit:preflight:end -->

The result must be `true`. If the table exists but access is denied, your database owner needs to grant your role `USAGE` on `bioseq_external` and `SELECT` on `bioseq_external.sequences`. A permission change cannot enable a module omitted from the binary.

Create a schema for this exercise and a table for the chromosome records:

<!-- seqkit:schema:start -->
```sql runnable id=seqkit_schema timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
-- bioseq:visible
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
-- bioseq:teardown
DROP SCHEMA seqkit_lab CASCADE;
```
<!-- seqkit:schema:end -->

| Column | What we put there |
| --- | --- |
| `assembly` | UCSC assembly identifier, such as `hg38` |
| `species` | A short label for grouping and plotting |
| `seq` | The DNA sequence, stored as a BioseqDB value |

<code>USING bioseq</code> selects BioseqDB's table storage. The `dna5` alphabet accepts A, C, G, T and N, where N means an unresolved base. The `64` is the sequence type's block-size parameter, not a limit of 64 bases. The [sequence representation documentation](/v0.1/tutorials/project_sequence_abstractions/) explains the storage details.

Now fetch the three records. Each `INSERT ... SELECT` reads a remote interval and saves it locally. Subsequent analysis will use the saved rows.

<!-- seqkit:import:start -->
```sql runnable id=seqkit_import timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
-- bioseq:visible
INSERT INTO seqkit_lab.genomes (assembly, species, seq)
SELECT 'hg38', 'human', sequence::bioseq.seq('dna5', 64)
FROM bioseq_external.sequences
WHERE provider = 'ucsc' AND assembly_database = 'hg38'
  AND chromosome = 'chrM' AND start0 = 0 AND end0 = 16569;

INSERT INTO seqkit_lab.genomes (assembly, species, seq)
SELECT 'mm39', 'mouse', sequence::bioseq.seq('dna5', 64)
FROM bioseq_external.sequences
WHERE provider = 'ucsc' AND assembly_database = 'mm39'
  AND chromosome = 'chrM' AND start0 = 0 AND end0 = 16299;

INSERT INTO seqkit_lab.genomes (assembly, species, seq)
SELECT 'danRer11', 'zebrafish', sequence::bioseq.seq('dna5', 64)
FROM bioseq_external.sequences
WHERE provider = 'ucsc' AND assembly_database = 'danRer11'
  AND chromosome = 'chrM' AND start0 = 0 AND end0 = 16596;

SELECT assembly, species, bioseq.len(seq) AS bp
FROM seqkit_lab.genomes ORDER BY species;
-- bioseq:teardown
DROP SCHEMA seqkit_lab CASCADE;
```
<!-- seqkit:import:end -->

UCSC's `start0` is zero-based and `end0` is exclusive. The human interval `[0, 16569)` therefore contains 16,569 bases, including the first and last base of that record. These coordinates refer to each assembly separately.

![Lengths of the three mitochondrial chromosome records retrieved from UCSC.](/static/figures/blog/seqkit-workflows/chromosome-lengths.svg)

The records differ by only a few hundred bases. We have one reference record per species, so this plot says nothing about variation within a species. It is useful here as a quick check that the import returned the complete requested intervals.

## 2. Make records we can filter and export

Three rows would make a rather short filtering exercise. We will split each chromosome into non-overlapping windows of 1,000 bases and keep the remaining shorter piece at the end. These windows are chunks made for this tutorial. They are not genes or sequencing reads.

<!-- seqkit:windows:start -->
```sql runnable id=seqkit_windows timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.snapshot (
    header text, seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
SELECT bioseq.load('seqkit_lab.snapshot', 'seqkit_ucsc_chrM.fasta');
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.genomes
SELECT metadata.assembly, metadata.species, snapshot.seq
FROM seqkit_lab.snapshot AS snapshot
JOIN (VALUES ('hg38', 'human'), ('mm39', 'mouse'),
             ('danRer11', 'zebrafish')) AS metadata(assembly, species)
  ON snapshot.header = metadata.assembly;
-- bioseq:visible
CREATE TABLE seqkit_lab.windows (
    id text NOT NULL,
    species text NOT NULL,
    assembly text NOT NULL,
    start0 integer NOT NULL,
    end0 integer NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.windows
SELECT species || '_' || lpad((start0 / 1000 + 1)::text, 2, '0'),
       species, assembly, start0,
       least(start0 + 1000, bioseq.len(seq)),
       seq[start0:least(start0 + 1000, bioseq.len(seq))]
FROM seqkit_lab.genomes
CROSS JOIN LATERAL generate_series(0, bioseq.len(seq) - 1, 1000) AS p(start0);

SELECT species, count(*) AS windows FROM seqkit_lab.windows
GROUP BY species ORDER BY species;
-- bioseq:teardown
DROP SCHEMA seqkit_lab CASCADE;
```
<!-- seqkit:windows:end -->

`generate_series` supplies the start positions. The slice `seq[start0:end0]` uses the same zero-based, half-open convention as the UCSC request, and `least` stops the final window at the end of the chromosome. We retain the coordinates because a motif position inside `human_05` is otherwise easy to confuse with a position on the original chromosome.

Each row has an `id`, `species`, `assembly`, `start0`, `end0` and `seq`. Here are the first six:

<!-- seqkit:inspect:start -->
```sql runnable id=seqkit_inspect timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.snapshot (
    header text, seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
SELECT bioseq.load('seqkit_lab.snapshot', 'seqkit_ucsc_chrM.fasta');
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.genomes
SELECT metadata.assembly, metadata.species, snapshot.seq
FROM seqkit_lab.snapshot AS snapshot
JOIN (VALUES ('hg38', 'human'), ('mm39', 'mouse'),
             ('danRer11', 'zebrafish')) AS metadata(assembly, species)
  ON snapshot.header = metadata.assembly;
CREATE TABLE seqkit_lab.windows (
    id text NOT NULL,
    species text NOT NULL,
    assembly text NOT NULL,
    start0 integer NOT NULL,
    end0 integer NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.windows
SELECT species || '_' || lpad((start0 / 1000 + 1)::text, 2, '0'),
       species, assembly, start0,
       least(start0 + 1000, bioseq.len(seq)),
       seq[start0:least(start0 + 1000, bioseq.len(seq))]
FROM seqkit_lab.genomes
CROSS JOIN LATERAL generate_series(0, bioseq.len(seq) - 1, 1000) AS p(start0);
-- bioseq:visible
SELECT id, assembly, start0, end0, bioseq.len(seq) AS bp,
       seq[:12]::text AS prefix
FROM seqkit_lab.windows
ORDER BY id
LIMIT 6;
-- bioseq:teardown
DROP SCHEMA seqkit_lab CASCADE;
```
<!-- seqkit:inspect:end -->

For the command-line comparisons, [install SeqKit](https://bioinf.shenwei.me/seqkit/download/) and export these same rows. Run the following in `psql`. The `\o` command writes to a file on the client machine, so you do not need PostgreSQL's server-file permissions.

```sql
\pset format unaligned
\pset tuples_only on
\o genomes.fa
SELECT '>' || assembly || E'\n' || seq::text
FROM seqkit_lab.genomes ORDER BY assembly;
\o windows.fa
SELECT '>' || id || ' species=' || species || ' assembly=' || assembly
       || ' start0=' || start0 || ' end0=' || end0 || E'\n' || seq::text
FROM seqkit_lab.windows ORDER BY id;
\o
```

For example, a header will start with `>human_01 species=human assembly=hg38 start0=0 end0=1000`. SeqKit uses the first whitespace-delimited part as the ID by default. The SQL table already has each field separately.

You can [download all the SQL, shell commands and chart code](/static/examples/seqkit-workflows.zip). The public workflow fetches its input from UCSC. For repeatable website builds, later listings load a pinned copy of those same three records in hidden setup code, rather than requesting UCSC again for every small example. The download includes the retrieval details and sequence hashes.

## 3. Check lengths before doing anything clever

A missing record or a truncated import can ruin all the later analysis. Start by counting the rows and their bases.

<!-- seqkit:stats:start -->
<div class="tool-comparison" role="group" aria-label="Stats comparison">
<div class="tool-comparison-panel">
<p class="tool-comparison-label">BioseqDB</p>

```sql runnable id=seqkit_stats timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.snapshot (
    header text, seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
SELECT bioseq.load('seqkit_lab.snapshot', 'seqkit_ucsc_chrM.fasta');
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.genomes
SELECT metadata.assembly, metadata.species, snapshot.seq
FROM seqkit_lab.snapshot AS snapshot
JOIN (VALUES ('hg38', 'human'), ('mm39', 'mouse'),
             ('danRer11', 'zebrafish')) AS metadata(assembly, species)
  ON snapshot.header = metadata.assembly;
CREATE TABLE seqkit_lab.windows (
    id text NOT NULL,
    species text NOT NULL,
    assembly text NOT NULL,
    start0 integer NOT NULL,
    end0 integer NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.windows
SELECT species || '_' || lpad((start0 / 1000 + 1)::text, 2, '0'),
       species, assembly, start0,
       least(start0 + 1000, bioseq.len(seq)),
       seq[start0:least(start0 + 1000, bioseq.len(seq))]
FROM seqkit_lab.genomes
CROSS JOIN LATERAL generate_series(0, bioseq.len(seq) - 1, 1000) AS p(start0);
-- bioseq:visible
SELECT count(*) AS sequences, sum(bioseq.len(seq)) AS total_bp,
       min(bioseq.len(seq)) AS min_bp,
       round(avg(bioseq.len(seq)), 1) AS mean_bp,
       max(bioseq.len(seq)) AS max_bp
FROM seqkit_lab.windows;
-- bioseq:teardown
DROP SCHEMA seqkit_lab CASCADE;
```

</div>
<div class="tool-comparison-panel">
<p class="tool-comparison-label">SeqKit</p>

```sh
seqkit stats -T windows.fa
```

```text
file	format	type	num_seqs	sum_len	min_len	avg_len	max_len
windows.fa	FASTA	DNA	51	49464	299	969.9	1000
```

</div>
</div>
<!-- seqkit:stats:end -->

Both tools report 51 records and 49,464 bases. The shortest record is 299 bases, which is the final mouse window. The mean is about 970 bases because all three final windows are shorter than 1,000.

To find those short records, use the sequence length as a filter:

<!-- seqkit:short:start -->
<div class="tool-comparison" role="group" aria-label="Short comparison">
<div class="tool-comparison-panel">
<p class="tool-comparison-label">BioseqDB</p>

```sql runnable id=seqkit_short timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.snapshot (
    header text, seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
SELECT bioseq.load('seqkit_lab.snapshot', 'seqkit_ucsc_chrM.fasta');
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.genomes
SELECT metadata.assembly, metadata.species, snapshot.seq
FROM seqkit_lab.snapshot AS snapshot
JOIN (VALUES ('hg38', 'human'), ('mm39', 'mouse'),
             ('danRer11', 'zebrafish')) AS metadata(assembly, species)
  ON snapshot.header = metadata.assembly;
CREATE TABLE seqkit_lab.windows (
    id text NOT NULL,
    species text NOT NULL,
    assembly text NOT NULL,
    start0 integer NOT NULL,
    end0 integer NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.windows
SELECT species || '_' || lpad((start0 / 1000 + 1)::text, 2, '0'),
       species, assembly, start0,
       least(start0 + 1000, bioseq.len(seq)),
       seq[start0:least(start0 + 1000, bioseq.len(seq))]
FROM seqkit_lab.genomes
CROSS JOIN LATERAL generate_series(0, bioseq.len(seq) - 1, 1000) AS p(start0);
-- bioseq:visible
SELECT id, bioseq.len(seq) AS bp
FROM seqkit_lab.windows
WHERE bioseq.len(seq) < 1000
ORDER BY id;
-- bioseq:teardown
DROP SCHEMA seqkit_lab CASCADE;
```

</div>
<div class="tool-comparison-panel">
<p class="tool-comparison-label">SeqKit</p>

```sh
seqkit seq --max-len 999 windows.fa | seqkit fx2tab -n -i -l
```

```text
human_17	569
mouse_17	299
zebrafish_17	596
```

</div>
</div>
<!-- seqkit:short:end -->

`bioseq.len` counts sequence symbols. SeqKit's `--max-len` (`-M`) accepts an inclusive maximum length, so `-M 999` corresponds to our SQL condition `< 1000`.

## 4. Count bases and calculate GC content

GC content is the percentage of known DNA bases that are G or C. A sequence with 30 Gs and 20 Cs among 100 known bases has 50% GC content. It is a common first description of a sequence, although a similar GC percentage does not establish that two regions are related.

First count the letters in each complete chromosome:

<!-- seqkit:base_counts:start -->
```sql runnable id=seqkit_base_counts timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.snapshot (
    header text, seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
SELECT bioseq.load('seqkit_lab.snapshot', 'seqkit_ucsc_chrM.fasta');
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.genomes
SELECT metadata.assembly, metadata.species, snapshot.seq
FROM seqkit_lab.snapshot AS snapshot
JOIN (VALUES ('hg38', 'human'), ('mm39', 'mouse'),
             ('danRer11', 'zebrafish')) AS metadata(assembly, species)
  ON snapshot.header = metadata.assembly;
CREATE TABLE seqkit_lab.windows (
    id text NOT NULL,
    species text NOT NULL,
    assembly text NOT NULL,
    start0 integer NOT NULL,
    end0 integer NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.windows
SELECT species || '_' || lpad((start0 / 1000 + 1)::text, 2, '0'),
       species, assembly, start0,
       least(start0 + 1000, bioseq.len(seq)),
       seq[start0:least(start0 + 1000, bioseq.len(seq))]
FROM seqkit_lab.genomes
CROSS JOIN LATERAL generate_series(0, bioseq.len(seq) - 1, 1000) AS p(start0);
-- bioseq:visible
SELECT species,
       bioseq.symbol_count(seq, 'A') AS a,
       bioseq.symbol_count(seq, 'C') AS c,
       bioseq.symbol_count(seq, 'G') AS g,
       bioseq.symbol_count(seq, 'T') AS t,
       bioseq.symbol_count(seq, 'N') AS n
FROM seqkit_lab.genomes ORDER BY species;
-- bioseq:teardown
DROP SCHEMA seqkit_lab CASCADE;
```
<!-- seqkit:base_counts:end -->

![Counts of A, C, G and T in the three mitochondrial records, with the unresolved-base count shown separately.](/static/figures/blog/seqkit-workflows/base-composition.svg)

There is one N in the human record and none in the other two. Dropping it would change the coordinate system, so we leave it in the stored sequence. For GC content, however, we exclude N from the denominator, matching SeqKit's calculation on these `dna5` records.

<!-- seqkit:gc:start -->
<div class="tool-comparison" role="group" aria-label="Gc comparison">
<div class="tool-comparison-panel">
<p class="tool-comparison-label">BioseqDB</p>

```sql runnable id=seqkit_gc timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.snapshot (
    header text, seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
SELECT bioseq.load('seqkit_lab.snapshot', 'seqkit_ucsc_chrM.fasta');
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.genomes
SELECT metadata.assembly, metadata.species, snapshot.seq
FROM seqkit_lab.snapshot AS snapshot
JOIN (VALUES ('hg38', 'human'), ('mm39', 'mouse'),
             ('danRer11', 'zebrafish')) AS metadata(assembly, species)
  ON snapshot.header = metadata.assembly;
CREATE TABLE seqkit_lab.windows (
    id text NOT NULL,
    species text NOT NULL,
    assembly text NOT NULL,
    start0 integer NOT NULL,
    end0 integer NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.windows
SELECT species || '_' || lpad((start0 / 1000 + 1)::text, 2, '0'),
       species, assembly, start0,
       least(start0 + 1000, bioseq.len(seq)),
       seq[start0:least(start0 + 1000, bioseq.len(seq))]
FROM seqkit_lab.genomes
CROSS JOIN LATERAL generate_series(0, bioseq.len(seq) - 1, 1000) AS p(start0);
-- bioseq:visible
SELECT species, bioseq.len(seq) AS bp,
       round(100.0 * (bioseq.symbol_count(seq, 'G') + bioseq.symbol_count(seq, 'C'))
             / nullif(bioseq.len(seq) - bioseq.symbol_count(seq, 'N'), 0), 2) AS gc_percent,
       bioseq.symbol_count(seq, 'N') AS unknown_bases
FROM seqkit_lab.genomes ORDER BY species;
-- bioseq:teardown
DROP SCHEMA seqkit_lab CASCADE;
```

</div>
<div class="tool-comparison-panel">
<p class="tool-comparison-label">SeqKit</p>

```sh
seqkit fx2tab -n -i -l -g -C N genomes.fa
```

```text
danRer11	16596	39.93	0
hg38	16569	44.36	1
mm39	16299	36.74	0
```

</div>
</div>
<!-- seqkit:gc:end -->

`bioseq.symbol_count` counts one letter. The `100.0` makes this a percentage calculation with a decimal result, and `nullif(..., 0)` avoids dividing by zero if a future record contains only Ns. With alphabets containing other ambiguity symbols, the denominator should explicitly count A, C, G and T instead.

The whole human mitochondrial record has 44.36% GC, the mouse record 36.74%, and zebrafish 39.93%. Now calculate the same statistic per window:

<!-- seqkit:gc_windows:start -->
<div class="tool-comparison" role="group" aria-label="Gc windows comparison">
<div class="tool-comparison-panel">
<p class="tool-comparison-label">BioseqDB</p>

```sql runnable id=seqkit_gc_windows timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.snapshot (
    header text, seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
SELECT bioseq.load('seqkit_lab.snapshot', 'seqkit_ucsc_chrM.fasta');
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.genomes
SELECT metadata.assembly, metadata.species, snapshot.seq
FROM seqkit_lab.snapshot AS snapshot
JOIN (VALUES ('hg38', 'human'), ('mm39', 'mouse'),
             ('danRer11', 'zebrafish')) AS metadata(assembly, species)
  ON snapshot.header = metadata.assembly;
CREATE TABLE seqkit_lab.windows (
    id text NOT NULL,
    species text NOT NULL,
    assembly text NOT NULL,
    start0 integer NOT NULL,
    end0 integer NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.windows
SELECT species || '_' || lpad((start0 / 1000 + 1)::text, 2, '0'),
       species, assembly, start0,
       least(start0 + 1000, bioseq.len(seq)),
       seq[start0:least(start0 + 1000, bioseq.len(seq))]
FROM seqkit_lab.genomes
CROSS JOIN LATERAL generate_series(0, bioseq.len(seq) - 1, 1000) AS p(start0);
-- bioseq:visible
SELECT id,
       round(100.0 * (bioseq.symbol_count(seq, 'G') + bioseq.symbol_count(seq, 'C'))
             / nullif(bioseq.len(seq) - bioseq.symbol_count(seq, 'N'), 0), 2) AS gc_percent
FROM seqkit_lab.windows ORDER BY id LIMIT 6;
-- bioseq:teardown
DROP SCHEMA seqkit_lab CASCADE;
```

</div>
<div class="tool-comparison-panel">
<p class="tool-comparison-label">SeqKit</p>

```sh
seqkit head -n 6 windows.fa | seqkit fx2tab -n -i -g
```

```text
human_01	45.60
human_02	44.00
human_03	42.70
human_04	47.75
human_05	42.60
human_06	43.90
```

</div>
</div>
<!-- seqkit:gc_windows:end -->

![GC percentage along each mitochondrial chromosome, measured in the tutorial's non-overlapping windows.](/static/figures/blog/seqkit-workflows/gc-windows.svg)

Each point sits at the midpoint of a window. The final window is shorter, and the lines only connect neighbouring measurements to make the plot readable. The three chromosomes have not been aligned, so matching horizontal positions across species do not imply matching genes. For a sequence alignment and coordinate-aware comparison, see the [butterfly tutorial](/blog/comparing-butterfly-genomes-with-bioseqdb/).

## 5. Select records by ID and by their contents

The SeqKit FAQ starts several workflows with a list of wanted IDs. SQL's `IN` expression is enough for a short list:

<!-- seqkit:ids:start -->
<div class="tool-comparison" role="group" aria-label="Ids comparison">
<div class="tool-comparison-panel">
<p class="tool-comparison-label">BioseqDB</p>

```sql runnable id=seqkit_ids timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.snapshot (
    header text, seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
SELECT bioseq.load('seqkit_lab.snapshot', 'seqkit_ucsc_chrM.fasta');
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.genomes
SELECT metadata.assembly, metadata.species, snapshot.seq
FROM seqkit_lab.snapshot AS snapshot
JOIN (VALUES ('hg38', 'human'), ('mm39', 'mouse'),
             ('danRer11', 'zebrafish')) AS metadata(assembly, species)
  ON snapshot.header = metadata.assembly;
CREATE TABLE seqkit_lab.windows (
    id text NOT NULL,
    species text NOT NULL,
    assembly text NOT NULL,
    start0 integer NOT NULL,
    end0 integer NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.windows
SELECT species || '_' || lpad((start0 / 1000 + 1)::text, 2, '0'),
       species, assembly, start0,
       least(start0 + 1000, bioseq.len(seq)),
       seq[start0:least(start0 + 1000, bioseq.len(seq))]
FROM seqkit_lab.genomes
CROSS JOIN LATERAL generate_series(0, bioseq.len(seq) - 1, 1000) AS p(start0);
-- bioseq:visible
SELECT id, bioseq.len(seq) AS bp
FROM seqkit_lab.windows
WHERE id IN ('human_01', 'mouse_02')
ORDER BY id;
-- bioseq:teardown
DROP SCHEMA seqkit_lab CASCADE;
```

</div>
<div class="tool-comparison-panel">
<p class="tool-comparison-label">SeqKit</p>

```sh
printf 'human_01\nmouse_02\n' > wanted.txt
seqkit grep -f wanted.txt windows.fa | seqkit fx2tab -n -i -l
```

```text
human_01	1000
mouse_02	1000
```

</div>
</div>
<!-- seqkit:ids:end -->

If you have thousands of IDs, put them in a table and join on `id`. The SeqKit equivalent accepts a file with `grep -f ids.txt`. To exclude the two IDs instead, reverse the condition:

<!-- seqkit:exclude:start -->
<div class="tool-comparison" role="group" aria-label="Exclude comparison">
<div class="tool-comparison-panel">
<p class="tool-comparison-label">BioseqDB</p>

```sql runnable id=seqkit_exclude timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.snapshot (
    header text, seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
SELECT bioseq.load('seqkit_lab.snapshot', 'seqkit_ucsc_chrM.fasta');
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.genomes
SELECT metadata.assembly, metadata.species, snapshot.seq
FROM seqkit_lab.snapshot AS snapshot
JOIN (VALUES ('hg38', 'human'), ('mm39', 'mouse'),
             ('danRer11', 'zebrafish')) AS metadata(assembly, species)
  ON snapshot.header = metadata.assembly;
CREATE TABLE seqkit_lab.windows (
    id text NOT NULL,
    species text NOT NULL,
    assembly text NOT NULL,
    start0 integer NOT NULL,
    end0 integer NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.windows
SELECT species || '_' || lpad((start0 / 1000 + 1)::text, 2, '0'),
       species, assembly, start0,
       least(start0 + 1000, bioseq.len(seq)),
       seq[start0:least(start0 + 1000, bioseq.len(seq))]
FROM seqkit_lab.genomes
CROSS JOIN LATERAL generate_series(0, bioseq.len(seq) - 1, 1000) AS p(start0);
-- bioseq:visible
SELECT count(*) AS retained
FROM seqkit_lab.windows
WHERE id NOT IN ('human_01', 'mouse_02');
-- bioseq:teardown
DROP SCHEMA seqkit_lab CASCADE;
```

</div>
<div class="tool-comparison-panel">
<p class="tool-comparison-label">SeqKit</p>

```sh
printf 'human_01\nmouse_02\n' > wanted.txt
seqkit grep -v -f wanted.txt -C windows.fa
```

```text
49
```

</div>
</div>
<!-- seqkit:exclude:end -->

Both retain 49 records. This also covers the contaminated-read exclusion pattern in the SeqKit tutorial, provided another step has already identified which records to remove. The filtering itself does not detect contamination.

We can also find the window containing the unresolved base:

<!-- seqkit:ambiguity:start -->
<div class="tool-comparison" role="group" aria-label="Ambiguity comparison">
<div class="tool-comparison-panel">
<p class="tool-comparison-label">BioseqDB</p>

```sql runnable id=seqkit_ambiguity timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.snapshot (
    header text, seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
SELECT bioseq.load('seqkit_lab.snapshot', 'seqkit_ucsc_chrM.fasta');
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.genomes
SELECT metadata.assembly, metadata.species, snapshot.seq
FROM seqkit_lab.snapshot AS snapshot
JOIN (VALUES ('hg38', 'human'), ('mm39', 'mouse'),
             ('danRer11', 'zebrafish')) AS metadata(assembly, species)
  ON snapshot.header = metadata.assembly;
CREATE TABLE seqkit_lab.windows (
    id text NOT NULL,
    species text NOT NULL,
    assembly text NOT NULL,
    start0 integer NOT NULL,
    end0 integer NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.windows
SELECT species || '_' || lpad((start0 / 1000 + 1)::text, 2, '0'),
       species, assembly, start0,
       least(start0 + 1000, bioseq.len(seq)),
       seq[start0:least(start0 + 1000, bioseq.len(seq))]
FROM seqkit_lab.genomes
CROSS JOIN LATERAL generate_series(0, bioseq.len(seq) - 1, 1000) AS p(start0);
-- bioseq:visible
SELECT id, bioseq.symbol_count(seq, 'N') AS unknown_bases
FROM seqkit_lab.windows
WHERE bioseq.symbol_count(seq, 'N') > 0
ORDER BY id;
-- bioseq:teardown
DROP SCHEMA seqkit_lab CASCADE;
```

</div>
<div class="tool-comparison-panel">
<p class="tool-comparison-label">SeqKit</p>

```sh
seqkit grep -s -P -r -p '[^ACGT]' windows.fa | seqkit fx2tab -n -i -C N
```

```text
human_04	1
```

</div>
</div>
<!-- seqkit:ambiguity:end -->

The result is `human_04`. On the SeqKit side, `-s` searches the sequence rather than its ID, `-r` enables a regular expression and `-P` searches only the positive strand. `[^ACGT]` means any character outside those four letters. Our SQL test is equivalent for this `dna5` dataset, whose only possible extra symbol is N.

A motif is a short sequence pattern we want to find inside a longer sequence. For a literal motif, try `GAATTC`:

<!-- seqkit:grep:start -->
<div class="tool-comparison" role="group" aria-label="Grep comparison">
<div class="tool-comparison-panel">
<p class="tool-comparison-label">BioseqDB</p>

```sql runnable id=seqkit_grep timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.snapshot (
    header text, seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
SELECT bioseq.load('seqkit_lab.snapshot', 'seqkit_ucsc_chrM.fasta');
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.genomes
SELECT metadata.assembly, metadata.species, snapshot.seq
FROM seqkit_lab.snapshot AS snapshot
JOIN (VALUES ('hg38', 'human'), ('mm39', 'mouse'),
             ('danRer11', 'zebrafish')) AS metadata(assembly, species)
  ON snapshot.header = metadata.assembly;
CREATE TABLE seqkit_lab.windows (
    id text NOT NULL,
    species text NOT NULL,
    assembly text NOT NULL,
    start0 integer NOT NULL,
    end0 integer NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.windows
SELECT species || '_' || lpad((start0 / 1000 + 1)::text, 2, '0'),
       species, assembly, start0,
       least(start0 + 1000, bioseq.len(seq)),
       seq[start0:least(start0 + 1000, bioseq.len(seq))]
FROM seqkit_lab.genomes
CROSS JOIN LATERAL generate_series(0, bioseq.len(seq) - 1, 1000) AS p(start0);
-- bioseq:visible
SELECT id
FROM seqkit_lab.windows
WHERE seq::text LIKE '%GAATTC%'
ORDER BY id;
-- bioseq:teardown
DROP SCHEMA seqkit_lab CASCADE;
```

</div>
<div class="tool-comparison-panel">
<p class="tool-comparison-label">SeqKit</p>

```sh
seqkit grep -s -P -p GAATTC windows.fa | seqkit seq -n -i
```

```text
human_05
human_06
human_13
mouse_02
mouse_04
mouse_05
zebrafish_09
zebrafish_15
```

</div>
</div>
<!-- seqkit:grep:end -->

Eight windows contain it. SQL's `%` matches any text before or after the motif, while SeqKit's `grep -s` performs the corresponding sequence search. We keep `-P` because an ordinary SQL text search examines the stored strand. SeqKit also checks the reverse complement by default.

A variable position in a motif can be written as a regular expression. `TATA[AT]AA` allows A or T in the fifth position:

<!-- seqkit:regex:start -->
<div class="tool-comparison" role="group" aria-label="Regex comparison">
<div class="tool-comparison-panel">
<p class="tool-comparison-label">BioseqDB</p>

```sql runnable id=seqkit_regex timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.snapshot (
    header text, seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
SELECT bioseq.load('seqkit_lab.snapshot', 'seqkit_ucsc_chrM.fasta');
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.genomes
SELECT metadata.assembly, metadata.species, snapshot.seq
FROM seqkit_lab.snapshot AS snapshot
JOIN (VALUES ('hg38', 'human'), ('mm39', 'mouse'),
             ('danRer11', 'zebrafish')) AS metadata(assembly, species)
  ON snapshot.header = metadata.assembly;
CREATE TABLE seqkit_lab.windows (
    id text NOT NULL,
    species text NOT NULL,
    assembly text NOT NULL,
    start0 integer NOT NULL,
    end0 integer NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.windows
SELECT species || '_' || lpad((start0 / 1000 + 1)::text, 2, '0'),
       species, assembly, start0,
       least(start0 + 1000, bioseq.len(seq)),
       seq[start0:least(start0 + 1000, bioseq.len(seq))]
FROM seqkit_lab.genomes
CROSS JOIN LATERAL generate_series(0, bioseq.len(seq) - 1, 1000) AS p(start0);
-- bioseq:visible
SELECT id FROM seqkit_lab.windows
WHERE seq::text ~ 'TATA[AT]AA'
ORDER BY id;
-- bioseq:teardown
DROP SCHEMA seqkit_lab CASCADE;
```

</div>
<div class="tool-comparison-panel">
<p class="tool-comparison-label">SeqKit</p>

```sh
seqkit grep -s -P -r -p 'TATA[AT]AA' windows.fa | seqkit seq -n -i
```

```text
human_07
human_13
mouse_01
mouse_02
mouse_03
mouse_05
mouse_08
mouse_11
mouse_12
zebrafish_01
```

</div>
</div>
<!-- seqkit:regex:end -->

This matches ten windows. The FAQ also demonstrates IUPAC degenerate symbols with SeqKit's `-d` option. Here we use an explicit character class so the two listings have the same meaning.

These searches operate within each window. A motif crossing a window boundary will be missed, and we are not wrapping a search around the end of the circular mitochondrial chromosome. For chromosome-wide motif analysis, query `seqkit_lab.genomes` directly and decide how to handle that circular boundary.

## 6. Locate a motif and take a slice

Sometimes the ID of a matching record is not enough. We need a coordinate.

<!-- seqkit:locate:start -->
<div class="tool-comparison" role="group" aria-label="Locate comparison">
<div class="tool-comparison-panel">
<p class="tool-comparison-label">BioseqDB</p>

```sql runnable id=seqkit_locate timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.snapshot (
    header text, seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
SELECT bioseq.load('seqkit_lab.snapshot', 'seqkit_ucsc_chrM.fasta');
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.genomes
SELECT metadata.assembly, metadata.species, snapshot.seq
FROM seqkit_lab.snapshot AS snapshot
JOIN (VALUES ('hg38', 'human'), ('mm39', 'mouse'),
             ('danRer11', 'zebrafish')) AS metadata(assembly, species)
  ON snapshot.header = metadata.assembly;
CREATE TABLE seqkit_lab.windows (
    id text NOT NULL,
    species text NOT NULL,
    assembly text NOT NULL,
    start0 integer NOT NULL,
    end0 integer NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.windows
SELECT species || '_' || lpad((start0 / 1000 + 1)::text, 2, '0'),
       species, assembly, start0,
       least(start0 + 1000, bioseq.len(seq)),
       seq[start0:least(start0 + 1000, bioseq.len(seq))]
FROM seqkit_lab.genomes
CROSS JOIN LATERAL generate_series(0, bioseq.len(seq) - 1, 1000) AS p(start0);
-- bioseq:visible
SELECT id, strpos(seq::text, 'GAATTC') - 1 AS first_start0
FROM seqkit_lab.windows
WHERE seq::text LIKE '%GAATTC%'
ORDER BY id;
-- bioseq:teardown
DROP SCHEMA seqkit_lab CASCADE;
```

</div>
<div class="tool-comparison-panel">
<p class="tool-comparison-label">SeqKit</p>

```sh
seqkit locate -P -p GAATTC windows.fa
```

```text
seqID	patternName	pattern	strand	start	end	matched
human_05	GAATTC	GAATTC	+	121	126	GAATTC
human_06	GAATTC	GAATTC	+	274	279	GAATTC
human_13	GAATTC	GAATTC	+	640	645	GAATTC
mouse_02	GAATTC	GAATTC	+	750	755	GAATTC
mouse_04	GAATTC	GAATTC	+	803	808	GAATTC
mouse_05	GAATTC	GAATTC	+	13	18	GAATTC
zebrafish_09	GAATTC	GAATTC	+	778	783	GAATTC
zebrafish_15	GAATTC	GAATTC	+	449	454	GAATTC
```

</div>
</div>
<!-- seqkit:locate:end -->

`strpos` returns the first occurrence using PostgreSQL's one-based string positions. Subtracting one converts it to a zero-based offset inside the window. SeqKit reports one-based inclusive start and end coordinates, so a SQL offset of 120 corresponds to SeqKit start 121. Add the window's `start0` to get the chromosome offset.

There happens to be one `GAATTC` occurrence in each of these eight matching windows. This short SQL expression finds only the first occurrence, whereas `seqkit locate` can return every occurrence. Do not use it as an all-sites counter for a different dataset.

Taking the first twelve bases is shorter to write:

<!-- seqkit:prefix:start -->
<div class="tool-comparison" role="group" aria-label="Prefix comparison">
<div class="tool-comparison-panel">
<p class="tool-comparison-label">BioseqDB</p>

```sql runnable id=seqkit_prefix timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.snapshot (
    header text, seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
SELECT bioseq.load('seqkit_lab.snapshot', 'seqkit_ucsc_chrM.fasta');
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.genomes
SELECT metadata.assembly, metadata.species, snapshot.seq
FROM seqkit_lab.snapshot AS snapshot
JOIN (VALUES ('hg38', 'human'), ('mm39', 'mouse'),
             ('danRer11', 'zebrafish')) AS metadata(assembly, species)
  ON snapshot.header = metadata.assembly;
CREATE TABLE seqkit_lab.windows (
    id text NOT NULL,
    species text NOT NULL,
    assembly text NOT NULL,
    start0 integer NOT NULL,
    end0 integer NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.windows
SELECT species || '_' || lpad((start0 / 1000 + 1)::text, 2, '0'),
       species, assembly, start0,
       least(start0 + 1000, bioseq.len(seq)),
       seq[start0:least(start0 + 1000, bioseq.len(seq))]
FROM seqkit_lab.genomes
CROSS JOIN LATERAL generate_series(0, bioseq.len(seq) - 1, 1000) AS p(start0);
-- bioseq:visible
SELECT id, seq[:12]::text AS first_12
FROM seqkit_lab.windows ORDER BY id LIMIT 3;
-- bioseq:teardown
DROP SCHEMA seqkit_lab CASCADE;
```

</div>
<div class="tool-comparison-panel">
<p class="tool-comparison-label">SeqKit</p>

```sh
seqkit head -n 3 windows.fa | seqkit subseq -r 1:12 | seqkit fx2tab -Q -i
```

```text
human_01	GATCACAGGTCT
human_02	CCAGTTGACACA
human_03	CGAGCCTGGTGA
```

</div>
</div>
<!-- seqkit:prefix:end -->

BioseqDB's `seq[:12]` selects offsets 0 through 11. SeqKit's `1:12` names the same bases using inclusive positions. Similarly, `seq[-10:]` takes the last ten bases, while `seq[9:20]` takes bases 10 through 20.

To inspect the opposite strand, reverse the sequence and complement A with T and C with G:

<!-- seqkit:reverse:start -->
<div class="tool-comparison" role="group" aria-label="Reverse comparison">
<div class="tool-comparison-panel">
<p class="tool-comparison-label">BioseqDB</p>

```sql runnable id=seqkit_reverse timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.snapshot (
    header text, seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
SELECT bioseq.load('seqkit_lab.snapshot', 'seqkit_ucsc_chrM.fasta');
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.genomes
SELECT metadata.assembly, metadata.species, snapshot.seq
FROM seqkit_lab.snapshot AS snapshot
JOIN (VALUES ('hg38', 'human'), ('mm39', 'mouse'),
             ('danRer11', 'zebrafish')) AS metadata(assembly, species)
  ON snapshot.header = metadata.assembly;
CREATE TABLE seqkit_lab.windows (
    id text NOT NULL,
    species text NOT NULL,
    assembly text NOT NULL,
    start0 integer NOT NULL,
    end0 integer NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.windows
SELECT species || '_' || lpad((start0 / 1000 + 1)::text, 2, '0'),
       species, assembly, start0,
       least(start0 + 1000, bioseq.len(seq)),
       seq[start0:least(start0 + 1000, bioseq.len(seq))]
FROM seqkit_lab.genomes
CROSS JOIN LATERAL generate_series(0, bioseq.len(seq) - 1, 1000) AS p(start0);
-- bioseq:visible
SELECT id, seq[:20]::text AS forward,
       bioseq.reverse_complement(seq[:20])::text AS reverse_complement
FROM seqkit_lab.windows
WHERE id IN ('human_01', 'mouse_01', 'zebrafish_01')
ORDER BY id;
-- bioseq:teardown
DROP SCHEMA seqkit_lab CASCADE;
```

</div>
<div class="tool-comparison-panel">
<p class="tool-comparison-label">SeqKit</p>

```sh
seqkit grep -r -p '_01$' windows.fa | seqkit subseq -r 1:20 | seqkit seq -t DNA -r -p | seqkit fx2tab -Q -i
```

```text
human_01	AGGGTGATAGACCTGTGATC
mouse_01	TGTTATTAAGCTACATTAAC
zebrafish_01	TATAAATTGTCGCCGGCCGT
```

</div>
</div>
<!-- seqkit:reverse:end -->

The query uses only the first twenty bases of the first window from each species so that the result fits on screen. SeqKit's `-r -p` options apply the same reverse-complement operation. Reversing alone changes their order without complementing A to T or C to G.

## 7. Group by species, then choose an order

The [FAQ's header-splitting example](https://bioinf.shenwei.me/seqkit/faq/#how-to-split-fasta-sequences-according-to-information-in-header) extracts a group name from each FASTA header and writes separate files. We already have a `species` column, so we can inspect the groups directly:

<!-- seqkit:by_header:start -->
<div class="tool-comparison" role="group" aria-label="By header comparison">
<div class="tool-comparison-panel">
<p class="tool-comparison-label">BioseqDB</p>

```sql runnable id=seqkit_by_header timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.snapshot (
    header text, seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
SELECT bioseq.load('seqkit_lab.snapshot', 'seqkit_ucsc_chrM.fasta');
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.genomes
SELECT metadata.assembly, metadata.species, snapshot.seq
FROM seqkit_lab.snapshot AS snapshot
JOIN (VALUES ('hg38', 'human'), ('mm39', 'mouse'),
             ('danRer11', 'zebrafish')) AS metadata(assembly, species)
  ON snapshot.header = metadata.assembly;
CREATE TABLE seqkit_lab.windows (
    id text NOT NULL,
    species text NOT NULL,
    assembly text NOT NULL,
    start0 integer NOT NULL,
    end0 integer NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.windows
SELECT species || '_' || lpad((start0 / 1000 + 1)::text, 2, '0'),
       species, assembly, start0,
       least(start0 + 1000, bioseq.len(seq)),
       seq[start0:least(start0 + 1000, bioseq.len(seq))]
FROM seqkit_lab.genomes
CROSS JOIN LATERAL generate_series(0, bioseq.len(seq) - 1, 1000) AS p(start0);
-- bioseq:visible
SELECT species, count(*) AS records
FROM seqkit_lab.windows
GROUP BY species ORDER BY species;
-- bioseq:teardown
DROP SCHEMA seqkit_lab CASCADE;
```

</div>
<div class="tool-comparison-panel">
<p class="tool-comparison-label">SeqKit</p>

```sh
seqkit split --by-id --id-regexp '^([^_]+)_' --out-dir by-species --force windows.fa
seqkit stats -T by-species/*.fa
```

```text
file	format	type	num_seqs	sum_len	min_len	avg_len	max_len
by-species/windows.part_human.fa	FASTA	DNA	17	16569	569	974.6	1000
by-species/windows.part_mouse.fa	FASTA	DNA	17	16299	299	958.8	1000
by-species/windows.part_zebrafish.fa	FASTA	DNA	17	16596	596	976.2	1000
```

</div>
</div>
<!-- seqkit:by_header:end -->

There are seventeen windows per species. To work with the human group, filter the column. The SeqKit command below writes three files by extracting the prefix before the underscore in each ID.

<!-- seqkit:split:start -->
<div class="tool-comparison" role="group" aria-label="Split comparison">
<div class="tool-comparison-panel">
<p class="tool-comparison-label">BioseqDB</p>

```sql runnable id=seqkit_split timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.snapshot (
    header text, seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
SELECT bioseq.load('seqkit_lab.snapshot', 'seqkit_ucsc_chrM.fasta');
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.genomes
SELECT metadata.assembly, metadata.species, snapshot.seq
FROM seqkit_lab.snapshot AS snapshot
JOIN (VALUES ('hg38', 'human'), ('mm39', 'mouse'),
             ('danRer11', 'zebrafish')) AS metadata(assembly, species)
  ON snapshot.header = metadata.assembly;
CREATE TABLE seqkit_lab.windows (
    id text NOT NULL,
    species text NOT NULL,
    assembly text NOT NULL,
    start0 integer NOT NULL,
    end0 integer NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.windows
SELECT species || '_' || lpad((start0 / 1000 + 1)::text, 2, '0'),
       species, assembly, start0,
       least(start0 + 1000, bioseq.len(seq)),
       seq[start0:least(start0 + 1000, bioseq.len(seq))]
FROM seqkit_lab.genomes
CROSS JOIN LATERAL generate_series(0, bioseq.len(seq) - 1, 1000) AS p(start0);
-- bioseq:visible
SELECT id, bioseq.len(seq) AS bp
FROM seqkit_lab.windows WHERE species = 'human'
ORDER BY id LIMIT 3;
-- bioseq:teardown
DROP SCHEMA seqkit_lab CASCADE;
```

</div>
<div class="tool-comparison-panel">
<p class="tool-comparison-label">SeqKit</p>

```sh
seqkit split --by-id --id-regexp '^([^_]+)_' --out-dir by-species --force windows.fa
seqkit head -n 3 by-species/windows.part_human.fa | seqkit fx2tab -n -i -l
```

```text
human_01	1000
human_02	1000
human_03	1000
```

</div>
</div>
<!-- seqkit:split:end -->

The SQL listing previews three human rows. It does not create three files: subsequent SQL queries can use `WHERE species = 'human'` without a file split. When you need a FASTA file for another program, repeat the export query from section 2 with that condition.

For length sorting, the three short tail windows make the result easy to check:

<!-- seqkit:sort:start -->
<div class="tool-comparison" role="group" aria-label="Sort comparison">
<div class="tool-comparison-panel">
<p class="tool-comparison-label">BioseqDB</p>

```sql runnable id=seqkit_sort timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.snapshot (
    header text, seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
SELECT bioseq.load('seqkit_lab.snapshot', 'seqkit_ucsc_chrM.fasta');
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.genomes
SELECT metadata.assembly, metadata.species, snapshot.seq
FROM seqkit_lab.snapshot AS snapshot
JOIN (VALUES ('hg38', 'human'), ('mm39', 'mouse'),
             ('danRer11', 'zebrafish')) AS metadata(assembly, species)
  ON snapshot.header = metadata.assembly;
CREATE TABLE seqkit_lab.windows (
    id text NOT NULL,
    species text NOT NULL,
    assembly text NOT NULL,
    start0 integer NOT NULL,
    end0 integer NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.windows
SELECT species || '_' || lpad((start0 / 1000 + 1)::text, 2, '0'),
       species, assembly, start0,
       least(start0 + 1000, bioseq.len(seq)),
       seq[start0:least(start0 + 1000, bioseq.len(seq))]
FROM seqkit_lab.genomes
CROSS JOIN LATERAL generate_series(0, bioseq.len(seq) - 1, 1000) AS p(start0);
-- bioseq:visible
SELECT id, bioseq.len(seq) AS bp
FROM seqkit_lab.windows
ORDER BY bioseq.len(seq), id
LIMIT 3;
-- bioseq:teardown
DROP SCHEMA seqkit_lab CASCADE;
```

</div>
<div class="tool-comparison-panel">
<p class="tool-comparison-label">SeqKit</p>

```sh
seqkit sort -l windows.fa | seqkit head -n 3 | seqkit fx2tab -n -i -l
```

```text
mouse_17	299
human_17	569
zebrafish_17	596
```

</div>
</div>
<!-- seqkit:sort:end -->

SQL adds `id` as a tie-breaker. The other 48 records all have length 1,000, so sorting by length alone does not define their relative order. A SQL table has no inherent row order, even if it was loaded from an ordered FASTA file.

That last detail matters when selecting a range of records:

<!-- seqkit:range:start -->
<div class="tool-comparison" role="group" aria-label="Range comparison">
<div class="tool-comparison-panel">
<p class="tool-comparison-label">BioseqDB</p>

```sql runnable id=seqkit_range timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.snapshot (
    header text, seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
SELECT bioseq.load('seqkit_lab.snapshot', 'seqkit_ucsc_chrM.fasta');
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.genomes
SELECT metadata.assembly, metadata.species, snapshot.seq
FROM seqkit_lab.snapshot AS snapshot
JOIN (VALUES ('hg38', 'human'), ('mm39', 'mouse'),
             ('danRer11', 'zebrafish')) AS metadata(assembly, species)
  ON snapshot.header = metadata.assembly;
CREATE TABLE seqkit_lab.windows (
    id text NOT NULL,
    species text NOT NULL,
    assembly text NOT NULL,
    start0 integer NOT NULL,
    end0 integer NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.windows
SELECT species || '_' || lpad((start0 / 1000 + 1)::text, 2, '0'),
       species, assembly, start0,
       least(start0 + 1000, bioseq.len(seq)),
       seq[start0:least(start0 + 1000, bioseq.len(seq))]
FROM seqkit_lab.genomes
CROSS JOIN LATERAL generate_series(0, bioseq.len(seq) - 1, 1000) AS p(start0);
-- bioseq:visible
SELECT id, bioseq.len(seq) AS bp
FROM seqkit_lab.windows
ORDER BY id
LIMIT 5 OFFSET 3;
-- bioseq:teardown
DROP SCHEMA seqkit_lab CASCADE;
```

</div>
<div class="tool-comparison-panel">
<p class="tool-comparison-label">SeqKit</p>

```sh
seqkit range -r 4:8 windows.fa | seqkit fx2tab -n -i -l
```

```text
human_04	1000
human_05	1000
human_06	1000
human_07	1000
human_08	1000
```

</div>
</div>
<!-- seqkit:range:end -->

These are records four through eight in ID order. `OFFSET 3` skips the first three and `LIMIT 5` takes the next five. SeqKit's `range` selects records in file order, which agrees here because we exported with `ORDER BY id`. It does not slice bases out of a sequence.

## 8. Rename, edit and join small sequence fragments

Adding a prefix to a name is a string operation:

<!-- seqkit:replace:start -->
<div class="tool-comparison" role="group" aria-label="Replace comparison">
<div class="tool-comparison-panel">
<p class="tool-comparison-label">BioseqDB</p>

```sql runnable id=seqkit_replace timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.snapshot (
    header text, seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
SELECT bioseq.load('seqkit_lab.snapshot', 'seqkit_ucsc_chrM.fasta');
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.genomes
SELECT metadata.assembly, metadata.species, snapshot.seq
FROM seqkit_lab.snapshot AS snapshot
JOIN (VALUES ('hg38', 'human'), ('mm39', 'mouse'),
             ('danRer11', 'zebrafish')) AS metadata(assembly, species)
  ON snapshot.header = metadata.assembly;
CREATE TABLE seqkit_lab.windows (
    id text NOT NULL,
    species text NOT NULL,
    assembly text NOT NULL,
    start0 integer NOT NULL,
    end0 integer NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.windows
SELECT species || '_' || lpad((start0 / 1000 + 1)::text, 2, '0'),
       species, assembly, start0,
       least(start0 + 1000, bioseq.len(seq)),
       seq[start0:least(start0 + 1000, bioseq.len(seq))]
FROM seqkit_lab.genomes
CROSS JOIN LATERAL generate_series(0, bioseq.len(seq) - 1, 1000) AS p(start0);
-- bioseq:visible
SELECT id, 'mito_' || id AS renamed
FROM seqkit_lab.windows ORDER BY id LIMIT 3;
-- bioseq:teardown
DROP SCHEMA seqkit_lab CASCADE;
```

</div>
<div class="tool-comparison-panel">
<p class="tool-comparison-label">SeqKit</p>

```sh
seqkit head -n 3 windows.fa | seqkit replace -p '^' -r 'mito_' | seqkit seq -n -i
```

```text
mito_human_01
mito_human_02
mito_human_03
```

</div>
</div>
<!-- seqkit:replace:end -->

`||` concatenates ordinary PostgreSQL text here. For more complicated header changes, PostgreSQL has `regexp_replace`. For a lookup table of old and new names, use a join. SeqKit's `replace` also supports regular expressions and key-value replacement files.

We can make a copy with its fifth base set to T:

<!-- seqkit:mutate:start -->
<div class="tool-comparison" role="group" aria-label="Mutate comparison">
<div class="tool-comparison-panel">
<p class="tool-comparison-label">BioseqDB</p>

```sql runnable id=seqkit_mutate timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.snapshot (
    header text, seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
SELECT bioseq.load('seqkit_lab.snapshot', 'seqkit_ucsc_chrM.fasta');
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.genomes
SELECT metadata.assembly, metadata.species, snapshot.seq
FROM seqkit_lab.snapshot AS snapshot
JOIN (VALUES ('hg38', 'human'), ('mm39', 'mouse'),
             ('danRer11', 'zebrafish')) AS metadata(assembly, species)
  ON snapshot.header = metadata.assembly;
CREATE TABLE seqkit_lab.windows (
    id text NOT NULL,
    species text NOT NULL,
    assembly text NOT NULL,
    start0 integer NOT NULL,
    end0 integer NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.windows
SELECT species || '_' || lpad((start0 / 1000 + 1)::text, 2, '0'),
       species, assembly, start0,
       least(start0 + 1000, bioseq.len(seq)),
       seq[start0:least(start0 + 1000, bioseq.len(seq))]
FROM seqkit_lab.genomes
CROSS JOIN LATERAL generate_series(0, bioseq.len(seq) - 1, 1000) AS p(start0);
-- bioseq:visible
SELECT id, seq[:20]::text AS before,
       overlay(seq[:20]::text placing 'T' from 5 for 1) AS after
FROM seqkit_lab.windows ORDER BY id LIMIT 3;
-- bioseq:teardown
DROP SCHEMA seqkit_lab CASCADE;
```

</div>
<div class="tool-comparison-panel">
<p class="tool-comparison-label">SeqKit</p>

```sh
seqkit head -n 3 windows.fa | seqkit subseq -r 1:20 | seqkit mutate -p 5:T | seqkit fx2tab -Q -i
```

```text
human_01	GATCTCAGGTCTATCACCCT
human_02	CCAGTTGACACAAAATAGAC
human_03	CGAGTCTGGTGATAGCTGGT
```

</div>
</div>
<!-- seqkit:mutate:end -->

This is an editing example, not a claim about a biological mutation. PostgreSQL's text function `overlay` uses one-based positions, so `from 5 for 1` agrees with `seqkit mutate -p 5:T`. We return the modified twenty-base text beside the original. No stored sequence is updated. Cast a complete edited string back to `bioseq.seq('dna5', 64)` when you want to store it as DNA.

SeqKit's `concat` deserves a separate example because it joins sequences with matching IDs across files. Appending FASTA files with `cat` does something else.

<!-- seqkit:concat:start -->
<div class="tool-comparison" role="group" aria-label="Concat comparison">
<div class="tool-comparison-panel">
<p class="tool-comparison-label">BioseqDB</p>

```sql runnable id=seqkit_concat timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.snapshot (
    header text, seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
SELECT bioseq.load('seqkit_lab.snapshot', 'seqkit_ucsc_chrM.fasta');
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.genomes
SELECT metadata.assembly, metadata.species, snapshot.seq
FROM seqkit_lab.snapshot AS snapshot
JOIN (VALUES ('hg38', 'human'), ('mm39', 'mouse'),
             ('danRer11', 'zebrafish')) AS metadata(assembly, species)
  ON snapshot.header = metadata.assembly;
CREATE TABLE seqkit_lab.windows (
    id text NOT NULL,
    species text NOT NULL,
    assembly text NOT NULL,
    start0 integer NOT NULL,
    end0 integer NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.windows
SELECT species || '_' || lpad((start0 / 1000 + 1)::text, 2, '0'),
       species, assembly, start0,
       least(start0 + 1000, bioseq.len(seq)),
       seq[start0:least(start0 + 1000, bioseq.len(seq))]
FROM seqkit_lab.genomes
CROSS JOIN LATERAL generate_series(0, bioseq.len(seq) - 1, 1000) AS p(start0);
-- bioseq:visible
WITH left_part AS (
  SELECT id, seq[:10] AS seq
  FROM seqkit_lab.windows ORDER BY id LIMIT 3
), right_part AS (
  SELECT id, seq[-10:] AS seq
  FROM seqkit_lab.windows ORDER BY id LIMIT 3
)
SELECT l.id,
       (l.seq OPERATOR(bioseq.||) r.seq)::text AS joined
FROM left_part l JOIN right_part r USING (id)
ORDER BY l.id;
-- bioseq:teardown
DROP SCHEMA seqkit_lab CASCADE;
```

</div>
<div class="tool-comparison-panel">
<p class="tool-comparison-label">SeqKit</p>

```sh
seqkit head -n 3 windows.fa > trio.fa
seqkit subseq -r 1:10 trio.fa > left.fa
seqkit subseq -r -10:-1 trio.fa > right.fa
seqkit concat left.fa right.fa | seqkit sort | seqkit fx2tab -Q -i
```

```text
human_01	GATCACAGGTGTAAAAAACT
human_02	CCAGTTGACAACAAACCTAC
human_03	CGAGCCTGGTTGTTGGATCA
```

</div>
</div>
<!-- seqkit:concat:end -->

Here `left.fa` contains the first ten bases of the first three human windows, and `right.fa` contains their last ten bases with the same IDs. The download's shell script prepares those files. The SQL common table expressions prepare the same two inputs, then `JOIN ... USING (id)` pairs them. <code>OPERATOR(bioseq.&#124;&#124;)</code> concatenates the BioseqDB sequence values. These joined fragments deliberately omit the middle 980 bases of each window.

Duplicate names are another issue. `seqkit rename` adds suffixes to repeated IDs, while `seqkit rmdup -s` compares sequence contents. They solve different problems. The download's shell script creates a three-record fixture named `duplicates.fa`: two records share the ID `copy`, the second has an edited final base, and the third has ID `other` but repeats the first sequence exactly.

For that input, `seqkit rename duplicates.fa` returns IDs `copy`, `copy_2`, `other`. In SQL, number the rows within each ID using `row_number() OVER (PARTITION BY id ORDER BY ordinal)` and append the number after the first occurrence. Keep an input-order column such as `ordinal` if the identity of the first record matters.

To remove repeated sequence contents, `seqkit rmdup -s -P -i duplicates.fa` retains the two distinct sequences. PostgreSQL's `DISTINCT ON (seq)` does the same forward-strand grouping, with `ORDER BY seq, ordinal` choosing the first copy. `-P` is deliberate: without it, SeqKit also treats reverse complements as duplicates. The complete runnable queries for both operations are in the download.

## 9. Shuffle and take a reproducible subset

For an exploratory subset in SQL, you can write `ORDER BY random() LIMIT 5`. Re-running it changes the sample. A small tutorial is easier to follow when the displayed IDs stay put, so here I sort a hash of a fixed label and each ID:

<!-- seqkit:sample:start -->
<div class="tool-comparison" role="group" aria-label="Sample comparison">
<div class="tool-comparison-panel">
<p class="tool-comparison-label">BioseqDB</p>

```sql runnable id=seqkit_sample timeout=60s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.snapshot (
    header text, seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
SELECT bioseq.load('seqkit_lab.snapshot', 'seqkit_ucsc_chrM.fasta');
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.genomes
SELECT metadata.assembly, metadata.species, snapshot.seq
FROM seqkit_lab.snapshot AS snapshot
JOIN (VALUES ('hg38', 'human'), ('mm39', 'mouse'),
             ('danRer11', 'zebrafish')) AS metadata(assembly, species)
  ON snapshot.header = metadata.assembly;
CREATE TABLE seqkit_lab.windows (
    id text NOT NULL,
    species text NOT NULL,
    assembly text NOT NULL,
    start0 integer NOT NULL,
    end0 integer NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.windows
SELECT species || '_' || lpad((start0 / 1000 + 1)::text, 2, '0'),
       species, assembly, start0,
       least(start0 + 1000, bioseq.len(seq)),
       seq[start0:least(start0 + 1000, bioseq.len(seq))]
FROM seqkit_lab.genomes
CROSS JOIN LATERAL generate_series(0, bioseq.len(seq) - 1, 1000) AS p(start0);
-- bioseq:visible
SELECT id
FROM seqkit_lab.windows
ORDER BY md5('2026:' || id)
LIMIT 5;
-- bioseq:teardown
DROP SCHEMA seqkit_lab CASCADE;
```

</div>
<div class="tool-comparison-panel">
<p class="tool-comparison-label">SeqKit</p>

```sh
seqkit sample -p 0.1 -s 11 windows.fa | seqkit seq -n -i
```

```text
human_01
human_11
mouse_10
zebrafish_08
zebrafish_09
zebrafish_16
```

</div>
</div>
<!-- seqkit:sample:end -->

Changing `'2026:'` changes the SQL ordering. This is a deterministic hash-based subset, not a promise of a statistically designed sample. Remove `LIMIT 5` to obtain a repeatable permutation of all rows. SeqKit has `shuffle -s 23 windows.fa` for a seeded shuffle and `sample -p 0.1 -s 11 windows.fa` for a seeded fraction sample.

The two programs use different selection methods, so their chosen IDs need not agree. The SQL example returns exactly five rows. SeqKit's `-p 0.1` includes each record with the requested sampling probability and returned six rows on this input with seed 11. For sampling a fixed number from large files, read the [SeqKit sample documentation](https://bioinf.shenwei.me/seqkit/usage/#sample) and its notes on the count option.

## A short command map

These are the cases I would keep nearby when moving between a FASTA workflow and SQL. The examples above contain the coordinate and ordering details that a compact table cannot show.

| Task | BioseqDB / PostgreSQL expression | SeqKit command |
| --- | --- | --- |
| Count records and inspect lengths | `count(*)`, `min(bioseq.len(seq))`, `max(bioseq.len(seq))` | stats |
| Reverse-complement DNA or filter length | `bioseq.reverse_complement(seq)`, `WHERE bioseq.len(seq) < 1000` | seq |
| Keep IDs or sequences matching a pattern | `WHERE id IN (...)`, `WHERE seq::text LIKE '%GAATTC%'` | grep |
| Select records by position in an ordered input | `ORDER BY id LIMIT 5 OFFSET 3` | range |
| Order records by sequence length | `ORDER BY bioseq.len(seq), id` | sort |
| Change record order | `ORDER BY random()` or a documented deterministic hash | shuffle |
| Select a subset | `ORDER BY random() LIMIT 5` for five rows | sample |
| Separate groups encoded in headers | Filter a metadata column, export groups if files are needed | split |
| Change names with a pattern or mapping | Text concatenation, `regexp_replace`, or a metadata join | replace |
| Make repeated IDs distinct | `row_number() OVER (PARTITION BY id ORDER BY ordinal)` plus a suffix | rename |
| Edit a sequence copy | `overlay(seq::text placing 'T' from 5 for 1)` | mutate |
| Join fragments sharing an ID | `JOIN ... USING (id)` and <code>OPERATOR(bioseq.&#124;&#124;)</code> | concat |
| Keep one copy of each stored sequence | `DISTINCT ON (seq)` with an explicit choice of retained row | rmdup |

The FAQ topics covered here include sequence statistics, GC content, extraction by ID, ambiguous bases, motif positions, duplicate sequences, length sorting, header groups, header replacement and matching-ID concatenation. SeqKit's tutorial also covers FASTQ quality, paired reads, live BAM monitoring, annotated feature extraction and other file-oriented work. This DNA-only exercise does not replace those workflows.

For me the useful difference appears when the next question involves metadata. Once the sequences are in `seqkit_lab.windows`, adding a species condition or joining a list of interesting regions is an ordinary SQL query. The data stays beside the information explaining where it came from. SeqKit remains convenient when I need to inspect or transform a file from the terminal, and the FASTA export means I can use it on exactly the same records.

To continue with actual sequence similarity rather than exact motifs, use the [BioseqDB alignment tutorial](/blog/comparing-butterfly-genomes-with-bioseqdb/). The queries there return matching intervals you can plot, along with the species and coordinate columns used throughout the analysis.

## Sources and reproducibility

The workflow choices were informed by [Wei Shen's SeqKit tutorial](https://bioinf.shenwei.me/seqkit/tutorial/), [FAQ](https://bioinf.shenwei.me/seqkit/faq/) and [command reference](https://bioinf.shenwei.me/seqkit/usage/). The examples here use a fresh UCSC dataset and independently checked SQL queries. The code download includes the upstream documentation's MIT notice in `ATTRIBUTION.txt`.

Sequence provenance is recorded in [the UCSC retrieval manifest](/static/examples/seqkit-workflows/provenance.json), including the assembly identifiers, request URLs and SHA-256 hashes. [UCSC's API reference](https://genome.ucsc.edu/goldenPath/help/api.html) describes the sequence endpoint and coordinates. Chart data comes from the same imported rows, and the download includes the measured values and plotting script.
