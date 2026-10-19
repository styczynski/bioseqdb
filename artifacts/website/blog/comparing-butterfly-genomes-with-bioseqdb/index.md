---
title: Comparing butterfly genomes with BioseqDB, SQLAlchemy and Plotly
slug: comparing-butterfly-genomes-with-bioseqdb
description: Retrieve two Heliconius optix regions from NCBI with SQL, inspect their DNA composition, align them inside PostgreSQL and draw the results with Python and Plotly.
date: '2026-10-16'
author: Piotr Styczyński
category: bioseqdb
keywords: [BioseqDB, pangenomics, comparative genomics, Heliconius, optix, NCBI, PostgreSQL, Python, SQLAlchemy, Plotly, sequence alignment, GC content, genome annotation]
cover: /static/figures/blog/butterfly-gc-cover.svg
image_alt: Teal and blue butterfly silhouettes with red wing marks above fine GC-content traces from the two sequence windows.
draft: false
---

Two butterflies can look remarkably similar without belonging to the same species. For *Heliconius*, part of the story why that happens, involves *optix*, a gene whose activity helps determine where red patterns appear on the wings. It encodes a transcription factor - so a protein which helps control the activity of other genes, that in very simplified terms works like sompphisticated switch, because changes in regulatory regions can affect not only if, but where and when a gene is used. [Reed and colleagues studied this connection across Heliconius species.](https://pubmed.ncbi.nlm.nih.gov/21778360/)

I decided to write this tutorial to showcase very simple example of analysis that one can do for single selected organism. As similar topics were always an interest of mine, I dedicated few last years to create a software that combines power of usual relational databases and software used commonly in compuatational biology to perform alignments (one can view them as problem of searching text in the database and understanding text similarity in specific biological way).
In this tutorial we will align a piece of chromosome or scaffold from two *Heliconius* species that includes *optix*. We will fetch the sequences from NCBI, keep them in PostgreSQL, ask BioseqDB (the database software I created and do little self-promotion :)) to find matching regions, and use SQLAlchemy and Plotly to inspect the results in Python. We won't need any separate alignment executable or tools.

I inspired myself by excellent blog post:  [butterfly genome-alignment tutorial using Minimap2 and R](https://stevenvb12.github.io/misc/2023/03/30/Minimap2-alignment.html). I wanted to work through the same kind of comparison with BioseqDB. This version uses newer NCBI assemblies and their own gene annotations, so the coordinates and results below will be different obviously!

![A schematic comparison of Heliconius butterflies and the optix region.](/assets/e5b8991aec67c818395da19c1865cd3c9c09c1e01f8bb9111f647613934ea6d3/figures/blog/butterflies/butterflies-optix.svg)

You can find all the full code, SQL, dependencies and Python notebook in [BioseqDB/examples/butterfies_alignments](https://github.com/BioseqDB/examples/tree/main/butterfies_alignments). You can also [download this tutorial’s code snapshot](/assets/e5b8991aec67c818395da19c1865cd3c9c09c1e01f8bb9111f647613934ea6d3/examples/butterfly-alignments.zip), but please note that neither provide FASTA files as I rely on NCBI datasets.

## 1. Very short biological introduction on what we will be doing

A DNA sequence is commonly written with four letters: **A**, **C**, **G** and **T**, representing adenine, cytosine, guanine and thymine. These are the bases of its nucleotides. A sequence file may also contain **N**, meaning the base is unresolved. Removing those letters would move every coordinate after them, so we keep them for simplicity.

DNA has two complementary strands - A pairs with T, and C with G and the strands also run in opposite directions. To read the same region from the other strand, we need to reverse the sequence and complement each letter. For example, the reverse complement of `AACGT` is `ACGTT`. I mentioning this, because this detail will matter quite a lot in our butterfly plots.

A genome is an organism's complete set of DNA, and an assembly is a reconstruction of it from sequencing data collected from some instruments (this process goes into "wet lab" so you ask your fellow collegues to run Illumina machine, which yields many many parts of the sequence you are interested in along with some noise. I won't go into details now how exactly this is done, but we have our sequence already in the dataset!).
A chromosome record can represent a reconstructed chromosome, while a scaffold is an assembled sequence whose placement may still be incomplete. We will be usiong record for two species - our *H. melpomene rosina* record is chromosome 18, while the *H. erato demophoon* record is an **unplaced scaffold** named `ntLink_18`

Now the core of any genomic/metagenomic analysis pipeline is the problem of alignment. An alignment is a process that places two sequences in columns so that corresponding bases (nucleotites A,C,T,G) can be compared. Equal letters are matches, different letters are substitutions, and gaps mean insertions or deletions relative to the other sequence. This allows us to understand for example where specific nucleotides were changed during evolutionary processes (how mutations arise, where etc.)

![DNA letters and a small alignment with a mismatch and a gap.](/assets/e5b8991aec67c818395da19c1865cd3c9c09c1e01f8bb9111f647613934ea6d3/figures/blog/butterflies/dna-alignment.svg)

We cannot sensibly compare every position in a million-base sequence with every position in another million-base sequence (that is a lot of text, so our alignment algorithm would be ridiciously slow!). Thankfully in BioseqDB's I provided search function`mmsearch`. Internally it starts with short shared fragments, connects promising matches into chains, then aligns selected regions in more detail. This approach is called the [seed-and-extend idea](/v0.1/tutorials/project_alignment_foundations/#seed-and-extend) and it allows more efficient processing so you can align ridiciously long sequences with no issue. If you are interested in writing your own aligner, my [Python read-mapper tutorial](/blog/writing-a-dna-read-mapper-in-python/) goes through the algorithms specifically written in very simple Python.

Here we start analysis with a pairwise comparison across two species, caluclating alignemnt of fragments per every pair of sequences. It is a useful first exercise in comparative genomics.

## 2. Setting things up!

We will import two one-million-base sequence fragments, inspect their lengths and nucleotide counts, calculate GC content, align them, and progressively draw one match, all retained matches, and the neighbourhood of *optix*.

![NCBI sequences enter BioseqDB through SQL; SQLAlchemy retrieves results and Plotly draws them.](/assets/e5b8991aec67c818395da19c1865cd3c9c09c1e01f8bb9111f647613934ea6d3/figures/blog/butterflies/sql-workflow.svg)

PostgreSQL via BioseqDB extension stores the sequences and BioseqDB performs the alignment. We will write Python code using SQLAlchemy, that will send SQL and receive rows, which later will be used by Plotly to constrtuct some figures. You can do that same thing with plain SQL client like `psql` as every step of data processing will happen inside the database itself.

## 3. Install BioseqDB and check external access

> [!WARNING]
> **Availability on 16 October 2026.** The published `0.1.0-alpha.1` Docker images and native package builds omit the external-database integration used here (due to my oversight). It is planned for the next alpha release, but you can obviously compile source from Github directly. Currently using runtime `SET`, an API key, or a `GRANT` won't work as the code isn't present in the binary - when building one must use `BIOSEQ_BUILD_EXTERNAL_API_CLIENTS=ON` to enable this feature.

The [installation page](/v0.1/tutorials/getting_started/) covers the regular Docker and native packages. For this tutorial, use the external-enabled development build described in the [example's setup instructions](https://github.com/BioseqDB/examples/tree/main/butterfies_alignments#database-setup):

```sh
cmake -S . -B build/butterflies -DBIOSEQ_BUILD_EXTERNAL_API_CLIENTS=ON
cmake --build build/butterflies --target postgresql_bioseq postgresql_bioseq_cache
```

These lines modify an already configured source build to enable the external clients module, which is responsible for directly interfacing with NCBI database. Obviously a workaround at this time, would be directly downloading datasets by yourself and loading FASTA using `bioseq.load` function.
After installing that build and enabling the extension in a fresh database, check its SQL objects:

<!-- butterfly:preflight:start -->
```sql
SELECT to_regnamespace('bioseq_external') IS NOT NULL AS external_schema,
       to_regclass('bioseq_external.catalog') IS NOT NULL AS catalog,
       to_regclass('bioseq_external.sequences') IS NOT NULL AS sequences;
```
<!-- butterfly:preflight:end -->

All three checks should be true. If the tables exist but a query reports permission denied, ask the database owner to grant access to your tutorial role:

```sql
GRANT USAGE ON SCHEMA bioseq_external TO butterfly_reader;
GRANT SELECT ON bioseq_external.catalog, bioseq_external.sequences
TO butterfly_reader;
```

Replace `butterfly_reader` with your existing database role.

## 4. Search NCBI via SQL

We will start with some metadata to make sure we refer to correct organism etc. The following query will use NCBI provider in BioseqDB and send requests to puiblic NCBI API to search organisms for you (all in SQL - how amazing it is instead using hard to understand CLIs?)

<!-- butterfly:discovery:start -->
```sql
SELECT accession, scientific_name, assembly_level, assembly_status
FROM bioseq_external.catalog
WHERE provider = 'ncbi'
  AND scientific_name = 'Heliconius erato demophoon'
ORDER BY accession;
```
<!-- butterfly:discovery:end -->

 The `provider` conditionis important as it selects NCBI and BioseqDB supports manyh other public databases of sequences. Field `scientific_name` selects the organism, in this case our butterfly species and naturally all seasrch fields are compatible what official NCBI API provides. Field `accession` is the assembly identifier and it's common name for most of public data sources. The last important one - `assembly_level` - is a field that distinguishes such things as scaffold and chromosome assemblies (there may be different kind of objects in the database so we would like to see what's there!)

When you run this query, you *can probably* get different results as the database is updated. For this comparison, I selected these two assemblies and sequence windows:

| Organism | Assembly | Sequence record | Imported interval, zero-based |
| --- | --- | --- | --- |
| *H. erato demophoon* | [GCA_056362945.2](https://www.ncbi.nlm.nih.gov/datasets/genome/GCA_056362945.2/) | [JBTWUK020000023.1](https://www.ncbi.nlm.nih.gov/nuccore/JBTWUK020000023.1), unplaced scaffold | `[3700000, 4700000)` |
| *H. melpomene rosina* | [GCF_056956725.1](https://www.ncbi.nlm.nih.gov/datasets/genome/GCF_056956725.1/) | [NC_140406.1](https://www.ncbi.nlm.nih.gov/nuccore/NC_140406.1), chromosome 18 | `[600000, 1600000)` |

Each interval contains one million bases, or **1 Mb** (in this notation **kb** is one thousand bases and **1Mb** means sequence is one million A/C/T/G/maybe others characters-long).

We use zero-based, half-open coordinates throughout: `[start, end)` includes `start` and excludes `end`, so its length is `end - start`. NCBI GFF annotations use one-based inclusive coordinates; convert them with `start0 = start1 - 1`, leaving the end unchanged. [NCBI documents the GFF convention here.](https://www.ncbi.nlm.nih.gov/datasets/docs/v2/reference-docs/file-formats/annotation-files/about-ncbi-gff3/)

![Coordinate conversion from one-based annotation positions to zero-based slices and local window positions.](/assets/e5b8991aec67c818395da19c1865cd3c9c09c1e01f8bb9111f647613934ea6d3/figures/blog/butterflies/coordinate-systems.svg)

## 5. Now the storage!

Here is our own table that we can create as in ordinary SQL you should be familiar with. `USING bioseq` gives it BioseqDB's sequence storage and search support. The other fields are tehre just to save the information needed to interpret a coordinates later.

<!-- butterfly:schema:start -->
```sql
CREATE SCHEMA IF NOT EXISTS butterflies;

CREATE TABLE IF NOT EXISTS butterflies.regions (
    key text NOT NULL,
    accession text NOT NULL,
    organism text NOT NULL,
    assembly text NOT NULL,
    start0 bigint NOT NULL,
    end0 bigint NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
```
<!-- butterfly:schema:end -->

| Field | Meaning |
| --- | --- |
| `key` | Short name used in this example: `erato` or `melpomene`. |
| `accession` | Versioned NCBI sequence record, not the assembly identifier. |
| `organism` | Full scientific name, including the subspecies. |
| `assembly` | Assembly accession used to select the matching annotations. |
| `start0`, `end0` | Boundaries of the imported window on the original record. |
| `seq` | The actual DNA, stored as `bioseq.seq('dna5', 64)` |

Oner important gotcha: you might be familiar with SQL `VARCHAR` where the number signifies how long the text can be. Here it's not the maximum bound on sequence length, but because of my decisions, this it how the sequence is encoded. It can be encoded on units of 8-bits, 16-bits, 32-bits or 64-bits and by default I recommend using the 64. If you try to plug here some other number you will gen an error.

Now we import the two intervals. The external `start0` and `end0` filters request the selected part of each accession sequence. Beatuful thing is that every step of the process so far happens inside the database, so you don't have to import anything in Python or use BioPython at all. Th SQL is self-explanatory (at least this is what I think - we just do previous query but along with `INSERT INTO` statement so it's persisted in the table we created):

<!-- butterfly:import:start -->
```sql
INSERT INTO butterflies.regions
    (key, accession, organism, assembly, start0, end0, seq)
SELECT 'erato', 'JBTWUK020000023.1', 'Heliconius erato demophoon', 'GCA_056362945.2', 3700000, 4700000,
       sequence::bioseq.seq('dna5', 64)
FROM bioseq_external.sequences
WHERE provider = 'ncbi'
  AND accession = 'JBTWUK020000023.1'
  AND start0 = 3700000
  AND end0 = 4700000;

INSERT INTO butterflies.regions
    (key, accession, organism, assembly, start0, end0, seq)
SELECT 'melpomene', 'NC_140406.1', 'Heliconius melpomene rosina', 'GCF_056956725.1', 600000, 1600000,
       sequence::bioseq.seq('dna5', 64)
FROM bioseq_external.sequences
WHERE provider = 'ncbi'
  AND accession = 'NC_140406.1'
  AND start0 = 600000
  AND end0 = 1600000;
```
<!-- butterfly:import:end -->

`INSERT ... SELECT` copies the returned sequences into our table. Once imported, the composition and alignment queries use local data.
Let's inspect a short prefix to see what's there out of curiousity:

<!-- butterfly:inspect:start -->
```sql
SELECT key, accession, bioseq.len(seq) AS length,
       seq[0:20]::text AS first_20_bases
FROM butterflies.regions
ORDER BY key;
```
<!-- butterfly:inspect:end -->

`bioseq.len(seq)` gives the number of bases. `seq[0:20]` takes the first twenty, using the same half-open convention as our genomic windows. A coordinate returned by an alignment will be relative to this imported sequence. To get back to the NCBI record, add its stored `start0`.

## 6. Let's do some exploration and count bases!

Two records can have similar overall composition and still contain very different sequences. I don't want to read entire million of bases as this would crowd up our screen, so lets stick to some meta properties, like the sequence lengths and counts for every nucleotide:

<!-- butterfly:composition:start -->
```sql
WITH counts AS (
    SELECT key, accession, organism, start0 AS interval_start,
           bioseq.len(seq) AS length,
           bioseq.symbol_count(seq, 'A') AS a,
           bioseq.symbol_count(seq, 'C') AS c,
           bioseq.symbol_count(seq, 'G') AS g,
           bioseq.symbol_count(seq, 'T') AS t,
           bioseq.symbol_count(seq, 'N') AS n
    FROM butterflies.regions
)
SELECT *, round(100.0 * (g + c) / NULLIF(a + c + g + t, 0), 4)
          AS gc_percent
FROM counts
ORDER BY key;
```
<!-- butterfly:composition:end -->

`bioseq.symbol_count` counts a selected letter and **GC content** is the percentage of called bases which are G or C:

$$\mathrm{GC\%} = 100\frac{G+C}{A+C+G+T}.$$

We exclude N from the denominator as N represents an unresolved base and in practice it can be anything of A/C/T/G, but we never know for sure. Usually Ns are small part of the sequence overall and we are not interested in them so much. `NULLIF` here avoids dividing by zero if a sequence contains no called bases. That case is unknown GC content, not zero GC. Lucily for us, both windows we saved and used here happen to contain no Ns (hooray!)

![Lengths of the two imported sequence windows.](/assets/e5b8991aec67c818395da19c1865cd3c9c09c1e01f8bb9111f647613934ea6d3/figures/blog/butterflies/sequence-lengths.svg)

![Lengths of the complete NCBI source records, distinct from the imported windows.](/assets/e5b8991aec67c818395da19c1865cd3c9c09c1e01f8bb9111f647613934ea6d3/figures/blog/butterflies/source-record-lengths.svg)

Both analysis windows are exactly 1 Mb. The source erato scaffold is 5,549,739 bases, the source melpomene chromosome is 16,989,447 bases.

![Measured counts of A, C, G, T and N in the two NCBI windows.](/assets/e5b8991aec67c818395da19c1865cd3c9c09c1e01f8bb9111f647613934ea6d3/figures/blog/butterflies/base-counts.svg)

A and T are more frequent than C and G in both windows. Erato contains 329,927 A, 172,322 C, 170,873 G and 326,878 T bases. Melpomene contains 327,164 A, 171,599 C, 171,833 G and 329,404 T bases.

![GC percentage of called bases in each imported sequence.](/assets/e5b8991aec67c818395da19c1865cd3c9c09c1e01f8bb9111f647613934ea6d3/figures/blog/butterflies/gc-content.svg)

The GC values are close: **34.3195%** and **34.3432%**, but note that this isn't reflecting their similarity as we don't measure here any position-based properties.

![GC content measured in consecutive 10 kb windows along both sequences.](/assets/e5b8991aec67c818395da19c1865cd3c9c09c1e01f8bb9111f647613934ea6d3/figures/blog/butterflies/gc-windows.svg)

More interesting is measuring GC content in windows of specified length, because they we have more than only one single data point per entire sequence of interest (they are long so any mutations will average to very similar total GC content percentage. Using smaller windows on sequences allow us to see some regions where "something happens" and the structure of sequence changes - all this without actually comparing all nucleotides themselves on our screen  - in practice this can be used as very primitive tool to quickly search for regions of interest)

<!-- butterfly:gc-windows:start -->
```sql
WITH windows AS (
    SELECT r.accession, w.start,
           least(w.start + 10000, bioseq.len(r.seq)) AS "end",
           r.seq[w.start:least(w.start + 10000, bioseq.len(r.seq))]
               AS piece
    FROM butterflies.regions AS r
    CROSS JOIN LATERAL generate_series(
        0, bioseq.len(r.seq) - 1, 10000) AS w(start)
), counts AS (
    SELECT accession, start, "end", bioseq.len(piece) AS length,
           bioseq.symbol_count(piece, 'G') + bioseq.symbol_count(piece, 'C') AS gc,
           bioseq.symbol_count(piece, 'N') AS n
    FROM windows
)
SELECT accession, start, "end",
       round(100.0 * gc / NULLIF(length - n, 0), 4) AS gc_percent,
       1.0 * n / length AS n_fraction
FROM counts
ORDER BY accession, start
LIMIT 6;
```
<!-- butterfly:gc-windows:end -->

These curves show where the composition changes within each imported region - peaks/valleyhs or other interesting shapes are not, by themselves, genes, enhancers or anything else and we need the appropriate annotations or experiments in "wet labs" to check, but nevertheless it's some information we can use in our analysis.

## 7. Align the regions inside PostgreSQL

We will use melpomene as the **target** and erato as the **query**. Both come from the same local table, selected by their `key` fields. This listing shows the first eight result (`06_align.sql` in the repository omits the final `LIMIT` and returns every block. I used `LIMIT` here just for simpler illustrative purposes)

<!-- butterfly:alignment:start -->
```sql
SELECT t.accession AS target_accession,
       q.accession AS query_accession,
       bioseq.target_start(t.seq) AS target_start,
       bioseq.target_end(t.seq) AS target_end,
       bioseq.query_start(t.seq) AS query_start,
       bioseq.query_end(t.seq) AS query_end,
       bioseq.is_reverse(t.seq) AS is_reverse,
       bioseq.score(t.seq) AS score,
       bioseq.cigar(t.seq) AS cigar,
       bioseq.edit(t.seq) AS edits,
       bioseq.block_length(t.seq) AS block_length,
       bioseq.mapq(t.seq) AS mapq,
       bioseq.is_primary(t.seq) AS is_primary
FROM butterflies.regions AS t
JOIN butterflies.regions AS q
  ON mmsearch(t.seq, q.seq, aligner_options(
      kmer_size => 15, window_size => 10,
      max_gap => 200, max_gap_ref => 200, min_chain_score => 40))
WHERE t.key = 'melpomene' AND q.key = 'erato'
ORDER BY score DESC, target_start, query_start
LIMIT 8;
```
<!-- butterfly:alignment:end -->

`mmsearch(t.seq, q.seq, ...)` is SQL function you can use to produce local sequence matches (perform sequence alignment). Here `kmer_size => 15` uses 15-base seeds and `window_size => 10` controls their minimizer windows (I don't want to go too deep into details, but a **k-mer** is a run of k consecutive bases and a **minimizer** is one representative k-mer selected from a window by a fixed ordering, which reduces the number of seeds we keep. The longer k-mers can increase performance by reducing sensitivity, but here we use BioseqDB's default k-mer size 15 and window size 10, so we will stick to them. They are presented here to let you know what you can change. These two values can be omitted when the session defaults are unchanged, but keep the gap options below to reproduce these results).
The gap and minimum chain-score options affect which candidate chains are explored and can do the same - improve performance at the cost of sensitivity. Here `max_gap => 200` differs from the default 5000; `max_gap_ref` is accepted but is not yet used by the current chaining implementation.

The `bioseq.*` projections describe the match we get from aligning of`t.seq` in the current result row:

| Projection | What we use it for |
| --- | --- |
| `target_start`, `target_end` | Half-open interval on the imported melpomene sequence. |
| `query_start`, `query_end` | Half-open interval on the imported erato sequence, in its deposited orientation. |
| `is_reverse` | Whether the query aligns as its reverse complement. |
| `cigar` | Base-level alignment operations: for example, `M` consumes a base from each sequence, `I` from the query, and `D` from the target. |
| `edit` | Number of substitutions and inserted/deleted bases in that alignment. |
| `block_length` | Alignment-column count, including gap columns. |
| `score` | Alignment score under the scoring scheme; not a percentage. |
| `mapq`, `is_primary` | Estimated placement confidence and primary/secondary information used when inspecting competing matches. |

Mapping quality, or **MAPQ**, concerns confidence in a placement among alternatives. It is different from the fraction of identical bases in an alignment, it says us how many different regions where there for some part of the query, so a high MAPQ does not mean the two sequences are identical, but means that for each region we were aligning there weren't many regions that can match as well so result is likely around correct place.

Requesting `bioseq.cigar` asks BioseqDB for the base-level alignment rather than only an estimates of positions (other base-level projections, including `bioseq.edit`, also request this step; without them the start/end of query/target might change! ASking for cigar improves the accuracy to the positions of individual bases). In a CIGAR string, `M` consumes a base from each sequence and can include both matches and mismatches; `I` marks inserted query bases and `D` marks deleted target bases. The numbers are run lengths. Use `bioseq.edit()` to retrieve the number of substitutions and inserted/deleted bases.

The validated run returned **661 blocks**, of which **592** had reverse query orientation (the aligners consider reverse strand which means that we not only search for some sequence X, but also it's reverse-complement. In very simple terms this is because of how DNA work and the reverse complement of ACTG is CAGT). For the plots we keep blocks with at least 300 alignment columns, mapping quality at least 20 and identity at least 70%. That leaves **355 blocks**, including **342 reverse matches**.

## 8. Set up Python with Poetry

Run these commands in a terminal with Python 3.11–3.13 and [Poetry](https://python-poetry.org/docs/#installation) installed:

```sh
git clone https://github.com/BioseqDB/examples.git
cd examples/butterfies_alignments
poetry install
export DATABASE_URL='postgresql+psycopg://postgres:bioseq@localhost:55432/butterflies'
poetry run butterfly-tutorial
```

Use the connection details of your external-enabled database. The password shown here is only a local example. `poetry install` installs SQLAlchemy, psycopg, Plotly, Kaleido and JupyterLab. If starting a separate project instead, the corresponding dependency command is:

```sh
poetry add sqlalchemy 'psycopg[binary]' plotly kaleido jupyterlab nbclient
```

Static SVG export uses Plotly with Kaleido and requires Chrome [Plotly explains that dependency here](https://plotly.com/python/static-image-export/). The database analysis can run without image export using `--no-figures`. After the first successful import, you can use `--reuse` to repeat the analysis without fetching those sequences again.

For the notebook interface:

```sh
poetry run jupyter lab notebooks/butterfly_alignments.ipynb
```

## 9. Mastering SQL now

You don't have to manually enter SQL into the database, you can do that the same thing from Python via the industry-standardised SQLAlchemy library:

<!-- butterfly:python-inspect:start -->
```python id=butterfly_python_connection
import os
from pathlib import Path
from sqlalchemy import create_engine, text

root = Path.cwd()
if not (root / "sql").is_dir():
    root = root.parent
sql_dir = root / "sql"
engine = create_engine(
    os.environ["DATABASE_URL"],
    connect_args={"prepare_threshold": None},
)
```

```python id=butterfly_python_inspect
statement = text((sql_dir / "04_inspect.sql").read_text())
with engine.connect() as connection:
    preview = connection.execute(statement).mappings().all()
for row in preview:
    print(dict(row))
```
<!-- butterfly:python-inspect:end -->

I use `create_engine`to tell SQLAlchemy how and where to connect and later the `with engine.connect()` block acquires a connection and returns it to the pool when finished. `.mappings()` gives rows with column names, which is convenient for tables and figures. We disable psycopg's automatic prepared statements here because the current external-query implementation needs the accession and range values available in its custom query plan.

The first two returned rows are:

<!-- butterfly:python-output:start -->
```text
{'key': 'erato', 'accession': 'JBTWUK020000023.1', 'length': 1000000, 'first_20_bases': 'TCGCGTGAATGCAAATGCGG'}
{'key': 'melpomene', 'accession': 'NC_140406.1', 'length': 1000000, 'first_20_bases': 'ATATCAAATACCAATGTTAG'}
```
<!-- butterfly:python-output:end -->

For a changing value, bind a parameter instead of building SQL with string interpolation:

<!-- butterfly:python-discover:start -->
```python id=butterfly_python_discover
statement = text((sql_dir / "01_discover.sql").read_text())
with engine.connect() as connection:
    assemblies = connection.execute(
        statement, {"organism": "Heliconius erato demophoon"}
    ).mappings().all()
print([dict(row) for row in assemblies])
```
<!-- butterfly:python-discover:end -->

If you want to read more about SQLAlchemy transactions and what is `engine.connect()` you are welcome to read more here: [SQLAlchemy's tutorial describes these transaction patterns.](https://docs.sqlalchemy.org/en/20/tutorial/dbapi_transactions.html)

To repeat the imports from Python, use the transactional helper:

<!-- butterfly:python-import:start -->
```python id=butterfly_python_import
import json
from butterfly_tutorial.workflow import import_regions

sources = json.loads((root / "sources.json").read_text())
import_regions(engine, sources["records"])
```
<!-- butterfly:python-import:end -->

The alignment is SQL, so it's another query through the same connection (nothing special here really!)

<!-- butterfly:python-align:start -->
```python id=butterfly_python_align
statement = text((sql_dir / "06_align.sql").read_text())
with engine.connect() as connection:
    matches = [dict(row) for row in connection.execute(statement).mappings()]
print(f"{len(matches)} alignment blocks")
```
<!-- butterfly:python-align:end -->

The `matches` list contains the same 661 rows as the SQL query. The full runner in the GitHub repo also validates lengths, nucleotide counts, sequence hashes, CIGAR operations and the *optix* coding sequences before, but I'm not going to present it all here as this is just some additional boilerplate not very relevant to the analysis itself.

## 10. First drawing!

There's a think called ribbon diagram, where a sequence track is just a rectangle whose x coordinates are sequence positions and its height and y position are display choices:

![A sequence interval drawn as a rectangle with genomic x coordinates and arbitrary display y coordinates.](/assets/e5b8991aec67c818395da19c1865cd3c9c09c1e01f8bb9111f647613934ea6d3/figures/blog/butterflies/rectangle-coordinates.svg)

An alignment connects two intervals with a four-corner polygon. For a forward match, the target's left endpoint corresponds to the query's left endpoint. For a reverse match, it corresponds to the query's right endpoint. No rocket science here, right? Only butterfly science! Huh.

![The four corners of an alignment ribbon, with the endpoint swap needed for a reverse match.](/assets/e5b8991aec67c818395da19c1865cd3c9c09c1e01f8bb9111f647613934ea6d3/figures/blog/butterflies/ribbon-coordinates.svg)

Here is the small plotting operation behind a ribbon:

<!-- butterfly:python-ribbon:start -->
```python id=butterfly_python_ribbon
import plotly.graph_objects as go

hit = matches[0]
left, right = hit["query_start"], hit["query_end"]
if hit["is_reverse"]:
    left, right = right, left
figure = go.Figure(go.Scatter(
    x=[hit["target_start"], hit["target_end"], right, left],
    y=[1, 1, 0, 0], fill="toself", mode="lines",
    line={"color": "#65b9dd"},
    fillcolor="rgba(101,185,221,0.25)",
))
figure.update_layout(xaxis_title="Position in imported window (bp)",
                     yaxis={"tickvals": [0, 1], "ticktext": ["Erato", "Melpomene"]})
figure.show()
```
<!-- butterfly:python-ribbon:end -->

The figures on this page are static SVGs, but the notebook and generated HTML versions also let you zoom and hover over a match to inspect its coordinates and it feel much cooler to see them update after your code changes. You can give it a try.

![One real alignment block connects its target and query intervals.](/assets/e5b8991aec67c818395da19c1865cd3c9c09c1e01f8bb9111f647613934ea6d3/figures/blog/butterflies/first-alignment-ribbon.svg)

Its target interval is `[639721, 644608)` and its query interval is `[332091, 337300)`, with reverse orientation. The spans need not have equal lengths because the alignment includes insertions and deletions. This block has 5,307 alignment columns and 894 edits, giving about 83.15% identity.

## 11. We need more plots! Mooore!

One plot won't satisfy my needs for colourful graphs. We can use a dot plot. A dot plot is a useful first view of many matches - each line segment plots target position against query position and its slope shows orientation.

![A dot plot of the 355 retained alignments, distinguishing forward and reverse matches.](/assets/e5b8991aec67c818395da19c1865cd3c9c09c1e01f8bb9111f647613934ea6d3/figures/blog/butterflies/alignment-dot-plot.svg)

The broad reverse trend agrees with the opposite orientations of the annotated *optix* regions in records, but it does **not** by itself mean a biological inversion between the species and somehow they reversed everyrthing just by randon accident - an assembly record can be deposited in either orientation to NCBI so it's likely that.

Now draw all retained ribbons in the original coordinate systems:

![All retained alignment ribbons between the two one-million-base windows.](/assets/e5b8991aec67c818395da19c1865cd3c9c09c1e01f8bb9111f647613934ea6d3/figures/blog/butterflies/alignment-ribbons.svg)

Overlapping blocks are possible, especially around repeated sequence, so summing their lengths would overstate coverage. After merging overlapping intervals, the displayed blocks cover 374,441 target bases and 387,528 query bases. These are the spans touched by the selected alignments.

For a closer look at *optix*, we centre each gene on zero and reverse the erato display axis. In local window coordinates, the gene midpoints are 493,223.5 for melpomene and 495,840 for erato. The display transformations are therefore `x = target_position - 493223.5` and `x = -(query_position - 495840)`.

![The optix neighbourhood after centring the gene models and reversing the erato display direction.](/assets/e5b8991aec67c818395da19c1865cd3c9c09c1e01f8bb9111f647613934ea6d3/figures/blog/butterflies/optix-centered-ribbons.svg)

![The display transformation shifts each gene midpoint to zero and reverses the query axis without changing stored coordinates.](/assets/e5b8991aec67c818395da19c1865cd3c9c09c1e01f8bb9111f647613934ea6d3/figures/blog/butterflies/alignment-display-transform.svg)

We need to make a choice how to  center the genome and it's compltely arbitrary - their annotated transcript spans differ, so corresponding coding bases need not fall at exactly the same x position. This transformation makes the comparison easier to read. In the interactive notebook and HTML versions, the hover text retains the original local coordinates. When selecting a zoom, we keep blocks that overlap it, including blocks crossing its boundary, requiring both endpoints to lie inside the zoom would silently discard useful matches. If you have better idea how to draw this, go ahead!

## 12. Add gene models

The erato feature `AC2CFC_001707` is annotated as homeobox protein SIX6; I treat it as the *optix* homolog here because its coding sequence shares 778/804 bases and its protein 266/267 amino acids with melpomene Optix.

The annotation loader on GitHub downloads the exact GFF files listed in `sources.json`, selects the appropriate accession, converts coordinates and clips features to our imported windows. There are **35 gene or pseudogene models** overlapping the erato window and **38** overlapping the melpomene window, including one pseudogene in each. A pseudogene is a gene-like sequence annotated as no longer encoding the usual functional product. A gene model is an annotation of a gene's structure.

The notebook assembles these views and `make_figures` returns a dictionary of Plotly figures.

<!-- butterfly:python-analysis:start -->
```python id=butterfly_python_analysis
from butterfly_tutorial.workflow import analyze
from butterfly_tutorial.annotations import load_annotations, load_optix_exons
from butterfly_tutorial.plots import make_figures

analysis = analyze(engine, sources["records"])
annotation_cache = root / "output" / "downloads"
genes = load_annotations(sources["records"], annotation_cache)
exons = load_optix_exons(sources["records"], annotation_cache)
figures = make_figures(
    analysis["sequences"], analysis["plotted_matches"],
    analysis["gc_windows"], genes, optix_exons=exons,
)
print(json.dumps(analysis["summary"], indent=2))
```
<!-- butterfly:python-analysis:end -->

![Current NCBI gene models, local GC measurements and BioseqDB alignments around optix.](/assets/e5b8991aec67c818395da19c1865cd3c9c09c1e01f8bb9111f647613934ea6d3/figures/blog/butterflies/optix-neighborhood.svg)

![The current optix gene and exon models in their transcript-oriented display.](/assets/e5b8991aec67c818395da19c1865cd3c9c09c1e01f8bb9111f647613934ea6d3/figures/blog/butterflies/optix-gene-models.svg)

The exon panel is a closer view of the annotations. Exons are the parts retained in a mature transcript, The annotated erato transcript has two exons and the melpomene transcript has one in these files and I put everything into one simple graphic. The gene here tracks place the matching segments in their annotated neighbourhood. The ribbons show sequence resemblance under the alignment and the last one - GC tracks add a simple measurement of GC content made directly from the imported DNA.

## 13. What did we learn?

The two regions have almost the same overall GC content, yet their alignment is a collection of local blocks with gaps and competing matches. The *optix* coding sequences are especially similar, while the surrounding DNA provides a larger and less uniform comparison. None of these plots alone identifies the regulatory change responsible for a wing pattern, but hey, we got some cool plots!

What I find useful about this workflow is that the sequences, their accession identifiers and the analysis stay all withing SQL. Selecting another region means changing explicit coordinates in your code without the need to look for CLI docs in the internet or using different tools blues together via bash scripts. You can even search NCBI via SQL layer that directly uses their API. I didn't show it here but you can export the table into usual FASTA as well.

To continue, try changing the display thresholds and compare the dot plot before interpreting an apparent break. Then select another gene neighbourhood, preserving the accession versions and coordinate conventions. Adding more genomes can turn this small pairwise exercise into a broader comparative analysis. with even more cool charts!
