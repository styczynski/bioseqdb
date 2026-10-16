---
title: Comparing butterfly genomes with BioseqDB, SQLAlchemy and Plotly
slug: comparing-butterfly-genomes-with-bioseqdb
description: Retrieve two Heliconius optix regions from NCBI with SQL, inspect their DNA composition, align them inside PostgreSQL and draw the results with Python and Plotly.
date: '2026-10-16'
author: Piotr Styczyński
category: bioseqdb
keywords: [BioseqDB, pangenomics, comparative genomics, Heliconius, optix, NCBI, PostgreSQL, Python, SQLAlchemy, Plotly, sequence alignment, GC content, genome annotation]
cover: /static/figures/blog/sequence-letters.svg
image_alt: Translucent A, C, G and T tiles in the BioseqDB colours.
draft: false
---

Two butterflies can look remarkably similar without belonging to the same species. In *Heliconius*, part of that story involves *optix*, a gene whose activity helps determine where red patterns appear on the wings. It encodes a transcription factor: a protein which helps control the activity of other genes. The DNA around it is interesting too, because changes in regulatory regions can affect where and when a gene is used. [Reed and colleagues studied this connection across Heliconius species.](https://pubmed.ncbi.nlm.nih.gov/21778360/)

In this tutorial we will align a piece of chromosome or scaffold from two *Heliconius* species that includes *optix*. We will fetch the sequences from NCBI, keep them in PostgreSQL, ask BioseqDB to find matching regions, and use SQLAlchemy and Plotly to inspect the results in Python. No separate alignment executable is needed.

I previously helped with a [butterfly genome-alignment tutorial using Minimap2 and R](https://stevenvb12.github.io/misc/2023/03/30/Minimap2-alignment.html). I wanted to work through the same kind of comparison with BioseqDB. This version uses newer NCBI assemblies and their own gene annotations, so the coordinates and results below belong to this experiment.

![A schematic comparison of Heliconius butterflies and the optix region.](/assets/fb67d2122e16debc58ea1a587c974bf0ddd51142438e3f11da4b86dc044892c2/figures/blog/butterflies/butterflies-optix.svg)

The full code, SQL, dependency lock and notebook are in [BioseqDB/examples/butterfies_alignments](https://github.com/BioseqDB/examples/tree/main/butterfies_alignments). You can also [download this tutorial’s code snapshot](/assets/fb67d2122e16debc58ea1a587c974bf0ddd51142438e3f11da4b86dc044892c2/examples/butterfly-alignments.zip). The repository does not contain downloaded genomes. The sequence imports below are SQL queries; the small Python annotation loader retrieves matching GFF files from NCBI.

## 1. A little biology before the queries

A DNA sequence is commonly written with four letters: **A**, **C**, **G** and **T**, representing adenine, cytosine, guanine and thymine. These are the bases of its nucleotides. A sequence file may also contain **N**, meaning the base is unresolved. Removing those letters would move every coordinate after them, so we keep them.

DNA has two complementary strands. A pairs with T, and C with G; the strands also run in opposite directions. To read the same region from the other strand, reverse the sequence and complement each letter. For example, the reverse complement of `AACGT` is `ACGTT`. This detail will matter quite a lot in our butterfly plots.

A genome is an organism's complete set of DNA, and an assembly is a reconstruction of it from sequencing data. A chromosome record can represent a reconstructed chromosome; a scaffold is an assembled sequence whose placement may still be incomplete. Our *H. melpomene rosina* record is chromosome 18. The *H. erato demophoon* record is an **unplaced scaffold** named `ntLink_18`; its name alone does not make it a chromosome assignment.

An alignment places two sequences in columns so that corresponding bases can be compared. Equal letters are matches, different letters are substitutions, and gaps account for insertions or deletions relative to the other sequence. Local alignment searches for matching pieces without requiring every base in both records to have a partner.

![DNA letters and a small alignment with a mismatch and a gap.](/assets/fb67d2122e16debc58ea1a587c974bf0ddd51142438e3f11da4b86dc044892c2/figures/blog/butterflies/dna-alignment.svg)

We cannot sensibly compare every position in a million-base sequence with every position in another million-base sequence. BioseqDB's `mmsearch` starts with short shared fragments, connects promising matches into chains, then aligns selected regions in more detail. This is the [seed-and-extend idea](/v0.1/tutorials/project_alignment_foundations/#seed-and-extend). My [Python read-mapper tutorial](/blog/writing-a-dna-read-mapper-in-python/) goes through the algorithms if you want to build a small version yourself.

Pangenomics considers variation across multiple genomes instead of treating one reference as the entire species. Here we start with a pairwise comparison across two species. It is a useful first exercise in comparative genomics, but the resulting ribbons are not a pangenome graph and do not describe all variation within either species.

## 2. What we will build

We will import two one-million-base windows, inspect their lengths and nucleotide counts, calculate GC content, align them, and progressively draw one match, all retained matches, and the neighbourhood of *optix*. The last figure adds current gene annotations and local GC measurements.

![NCBI sequences enter BioseqDB through SQL; SQLAlchemy retrieves results and Plotly draws them.](/assets/fb67d2122e16debc58ea1a587c974bf0ddd51142438e3f11da4b86dc044892c2/figures/blog/butterflies/sql-workflow.svg)

The division of work is fairly small: PostgreSQL stores the sequences and BioseqDB performs the alignment. SQLAlchemy sends SQL and receives rows. Plotly turns those rows into figures. The same SQL works from `psql`, so the Python layer does not hide a second alignment pipeline.

## 3. Install BioseqDB and check external access

> [!WARNING]
> **Availability on 16 October 2026.** The published `0.1.0-alpha.1` Docker images and native package build profiles omit the external-database integration used here. It is planned for the next alpha release. A runtime `SET`, an API key, or a `GRANT` cannot add a feature omitted at build time. This tutorial is validated against a source build with `BIOSEQ_BUILD_EXTERNAL_API_CLIENTS=ON`; check your installation before running the NCBI queries.

The [installation page](/v0.1/tutorials/getting_started/) covers the regular Docker and native packages. For this tutorial, use the external-enabled development build described in the [example's setup instructions](https://github.com/BioseqDB/examples/tree/main/butterfies_alignments#database-setup). Those instructions pin a public source revision and apply the NCBI query fixes included with the example. The important CMake option is:

```sh
cmake -S . -B build/butterflies -DBIOSEQ_BUILD_EXTERNAL_API_CLIENTS=ON
cmake --build build/butterflies --target postgresql_bioseq postgresql_bioseq_cache
```

These lines modify an already configured source build; the example's setup instructions include the tested toolchain and PostgreSQL setup. They are not commands to run inside the released Docker container. After installing that build and enabling the extension in a fresh database, check its SQL objects:

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

Replace `butterfly_reader` with your existing database role. This is a permission step for a build that already has the feature; it does not enable the feature in an older package. The example uses a local database account which owns the tutorial tables.

## 4. Find the assemblies by organism name

Start with metadata. Printing a chromosome's entire sequence is rarely a useful first query.

<!-- butterfly:discovery:start -->
```sql
SELECT accession, scientific_name, assembly_level, assembly_status
FROM bioseq_external.catalog
WHERE provider = 'ncbi'
  AND scientific_name = 'Heliconius erato demophoon'
ORDER BY accession;
```
<!-- butterfly:discovery:end -->

`bioseq_external.catalog` presents NCBI assembly metadata as a SQL table. The `provider` condition selects NCBI and `scientific_name` selects the organism. `accession` is the assembly identifier; `assembly_level` distinguishes such things as scaffold and chromosome assemblies. The catalog is backed by NCBI Datasets, so it is not a copy of every record returned by the NCBI Nucleotide website's free-text search.

Discovery answers what is available now. For a reproducible comparison we then choose explicit assembly and sequence versions:

| Organism | Assembly | Sequence record | Imported interval, zero-based |
| --- | --- | --- | --- |
| *H. erato demophoon* | [GCA_056362945.2](https://www.ncbi.nlm.nih.gov/datasets/genome/GCA_056362945.2/) | [JBTWUK020000023.1](https://www.ncbi.nlm.nih.gov/nuccore/JBTWUK020000023.1), unplaced scaffold | `[3700000, 4700000)` |
| *H. melpomene rosina* | [GCF_056956725.1](https://www.ncbi.nlm.nih.gov/datasets/genome/GCF_056956725.1/) | [NC_140406.1](https://www.ncbi.nlm.nih.gov/nuccore/NC_140406.1), chromosome 18 | `[600000, 1600000)` |

Each interval contains one million bases, or **1 Mb**. A **kb** is one thousand bases. We keep the deposited orientation of both records during import.

Before choosing the feature, distinguish a few terms. A **transcript** is an RNA copy made from a gene. A **coding sequence**, or CDS, is the part translated into a protein; a protein is a chain of amino acids. **Transcript orientation** means reading that sequence in the direction used by the gene, which may require a reverse complement. A **homolog** is a corresponding gene related through shared ancestry.

There is a naming wrinkle worth resolving before drawing anything. The melpomene annotation names its gene `Optix`; the erato annotation calls the corresponding candidate `AC2CFC_001707`, described as “homeobox protein SIX6”. The two transcript-oriented coding sequences have 778 identical positions out of 804 bases, and their translated proteins share 266 of 267 amino acids. Together with the local alignment, this supports treating the erato feature as the *optix* homolog here. It is our identification from the sequence evidence, not a claim that its NCBI annotation already uses that name. The notebook checks those counts again from the sequences returned by SQL.

We use zero-based, half-open coordinates throughout: `[start, end)` includes `start` and excludes `end`, so its length is `end - start`. NCBI GFF annotations use one-based inclusive coordinates; convert them with `start0 = start1 - 1`, leaving the end unchanged. [NCBI documents the GFF convention here.](https://www.ncbi.nlm.nih.gov/datasets/docs/v2/reference-docs/file-formats/annotation-files/about-ncbi-gff3/)

![Coordinate conversion from one-based annotation positions to zero-based slices and local window positions.](/assets/fb67d2122e16debc58ea1a587c974bf0ddd51142438e3f11da4b86dc044892c2/figures/blog/butterflies/coordinate-systems.svg)

## 5. Store the selected intervals

Here is our own table. `USING bioseq` gives it BioseqDB's sequence storage and search support; the other fields retain the information needed to interpret a coordinate later.

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
| `seq` | The actual DNA, stored as `bioseq.seq('dna5', 64)`; DNA5 retains A/C/G/T/N. |

The `64` selects the sequence storage width; it is not a 64-base limit. The repository also includes this field table as `schema.tex` for use in a LaTeX document.

Now import the two intervals. The external `start0` and `end0` filters request the selected part of each accession; there is no need to download the whole chromosome into Python first.

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

`INSERT ... SELECT` copies the returned sequences into our table. Once imported, the composition and alignment queries use local data. The Python runner wraps both imports in one transaction, checks that each produced exactly one row, and verifies the sequence hashes recorded in `sources.json`.

Inspect a short prefix rather than the million letters:

<!-- butterfly:inspect:start -->
```sql
SELECT key, accession, bioseq.len(seq) AS length,
       seq[0:20]::text AS first_20_bases
FROM butterflies.regions
ORDER BY key;
```
<!-- butterfly:inspect:end -->

`bioseq.len(seq)` gives the number of bases. `seq[0:20]` takes the first twenty, using the same half-open convention as our genomic windows. A coordinate returned by an alignment will be relative to this imported sequence. To get back to the NCBI record, add its stored `start0`.

## 6. Count the bases before aligning

Two records can have similar overall composition and still contain very different sequences. These checks help us understand the input; they do not establish homology by themselves.

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

`bioseq.symbol_count` counts a selected letter. **GC content** is the percentage of called bases which are G or C:

$$\mathrm{GC\%} = 100\frac{G+C}{A+C+G+T}.$$

We exclude N from the denominator and report it separately. `NULLIF` avoids dividing by zero if a sequence contains no called bases; that case is unknown GC content, not zero GC. Both windows used here happen to contain no Ns.

![Lengths of the two imported sequence windows.](/assets/fb67d2122e16debc58ea1a587c974bf0ddd51142438e3f11da4b86dc044892c2/figures/blog/butterflies/sequence-lengths.svg)

![Lengths of the complete NCBI source records, distinct from the imported windows.](/assets/fb67d2122e16debc58ea1a587c974bf0ddd51142438e3f11da4b86dc044892c2/figures/blog/butterflies/source-record-lengths.svg)

Both analysis windows are exactly 1 Mb. This is a choice of input window, not evidence that the two chromosomes have equal length. The source erato scaffold is 5,549,739 bases; the source melpomene chromosome is 16,989,447 bases.

![Measured counts of A, C, G, T and N in the two NCBI windows.](/assets/fb67d2122e16debc58ea1a587c974bf0ddd51142438e3f11da4b86dc044892c2/figures/blog/butterflies/base-counts.svg)

A and T are more frequent than C and G in both windows. Erato contains 329,927 A, 172,322 C, 170,873 G and 326,878 T bases. Melpomene contains 327,164 A, 171,599 C, 171,833 G and 329,404 T bases. The runner checks that the counts, including N, sum to the imported length.

![GC percentage of called bases in each imported sequence.](/assets/fb67d2122e16debc58ea1a587c974bf0ddd51142438e3f11da4b86dc044892c2/figures/blog/butterflies/gc-content.svg)

The GC values are close: **34.3195%** and **34.3432%**. A single percentage hides local variation, so we also calculate it in consecutive 10 kb windows. The final, shorter window would be retained if the sequence length were not an exact multiple of 10 kb.

![GC content measured in consecutive 10 kb windows along both sequences.](/assets/fb67d2122e16debc58ea1a587c974bf0ddd51142438e3f11da4b86dc044892c2/figures/blog/butterflies/gc-windows.svg)

The SQL query for that profile is below, showing its first six windows. The complete query in the repository returns all 200.

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

These curves show where the composition changes within each imported region. Peaks are not, by themselves, genes, enhancers or accessible chromatin. We need the appropriate annotations or experiments to make those claims.

## 7. Align the regions inside PostgreSQL

We will use melpomene as the **target** and erato as the **query**. Both come from the same local table, selected by their `key` fields. This listing shows the first eight results; `06_align.sql` in the repository omits the final `LIMIT` and returns every block.

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

`mmsearch(t.seq, q.seq, ...)` produces local sequence matches. Here `kmer_size => 15` uses 15-base seeds and `window_size => 10` controls their minimizer windows. A **k-mer** is a run of k consecutive bases. A **minimizer** is one representative k-mer selected from a window by a fixed ordering, which reduces the number of seeds we keep. The gap and minimum chain-score options affect which candidate chains are explored; `max_gap => 200` is not a promise that every gap in the final alignment will be at most 200 bases.

The `bioseq.*` projections describe the match associated with `t.seq` in the current result row:

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

Mapping quality, or **MAPQ**, concerns confidence in a placement among alternatives; it is different from the fraction of identical bases in an alignment. A high MAPQ does not mean the two sequences are identical.

Requesting `bioseq.cigar` matters: it asks BioseqDB for the base-level alignment rather than only a candidate chain. An `M` operation can contain both equal and unequal bases, so counting M positions is not sufficient to measure identity. Our runner independently replays every CIGAR against the fetched bases, checks `bioseq.edit`, then calculates `100 * (block_length - edits) / block_length`. This definition counts a gap column as a difference.

The validated run returned **661 blocks**, of which **592** had reverse query orientation. For the plots we keep blocks with at least 300 alignment columns, mapping quality at least 20 and identity at least 70%. That leaves **355 blocks**, including **342 reverse matches**. These are display thresholds, not a biological definition of conservation, and all 661 unfiltered rows remain in the local analysis output.

## 8. Set up Python with Poetry

Run these commands in a terminal with Python 3.11–3.13 and [Poetry](https://python-poetry.org/docs/#installation) installed:

```sh
git clone https://github.com/BioseqDB/examples.git
cd examples/butterfies_alignments
poetry install
export DATABASE_URL='postgresql+psycopg://postgres:bioseq@localhost:55432/butterflies'
poetry run butterfly-tutorial
```

Use the connection details of your external-enabled database. The password shown here is only a local example. `poetry install` installs SQLAlchemy, psycopg, Plotly, Kaleido and JupyterLab from the included lock file. If starting a separate project instead, the corresponding dependency command is:

```sh
poetry add sqlalchemy 'psycopg[binary]' plotly kaleido jupyterlab nbclient
```

Static SVG export uses Kaleido and requires Chrome; [Plotly explains that dependency here](https://plotly.com/python/static-image-export/). The database analysis can run without image export using `--no-figures`. After the first successful import, `--reuse` verifies the stored sequence hashes and repeats the analysis without fetching those sequences again.

For the notebook interface:

```sh
poetry run jupyter lab notebooks/butterfly_alignments.ipynb
```

## 9. Send the same SQL with SQLAlchemy

The SQL lives in separate files so the command-line runner and notebook use the same queries. Start a connection and read the inspection query:

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

`create_engine` describes how to connect. The `with engine.connect()` block acquires a connection and returns it to the pool when finished. `.mappings()` gives rows with column names, which is convenient for tables and figures. We disable psycopg's automatic prepared statements here because the current external-query implementation needs the accession and range values available in its custom query plan.

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

The organism string remains data, rather than becoming part of the SQL syntax. For writes, the import helper uses `engine.begin()`: successful work commits when the block ends, while an exception rolls back the transaction. [SQLAlchemy's tutorial describes these transaction patterns.](https://docs.sqlalchemy.org/en/20/tutorial/dbapi_transactions.html)

To repeat the imports from Python, use the pinned manifest and the transactional helper:

<!-- butterfly:python-import:start -->
```python id=butterfly_python_import
import json
from butterfly_tutorial.workflow import import_regions

sources = json.loads((root / "sources.json").read_text())
import_regions(engine, sources["records"])
```
<!-- butterfly:python-import:end -->

The alignment is another query through the same connection:

<!-- butterfly:python-align:start -->
```python id=butterfly_python_align
statement = text((sql_dir / "06_align.sql").read_text())
with engine.connect() as connection:
    matches = [dict(row) for row in connection.execute(statement).mappings()]
print(f"{len(matches)} alignment blocks")
```
<!-- butterfly:python-align:end -->

The `matches` list contains the same 661 rows as the SQL query. Python is receiving coordinates and alignment details, not invoking Minimap2 behind the scenes. The full runner also validates lengths, nucleotide counts, sequence hashes, CIGAR operations and the *optix* coding sequences before it draws any charts.

## 10. Draw one match first

A ribbon diagram looks complicated until we separate its parts. A sequence track is just a rectangle whose x coordinates are sequence positions. Its height and y position are display choices.

![A sequence interval drawn as a rectangle with genomic x coordinates and arbitrary display y coordinates.](/assets/fb67d2122e16debc58ea1a587c974bf0ddd51142438e3f11da4b86dc044892c2/figures/blog/butterflies/rectangle-coordinates.svg)

An alignment connects two intervals with a four-corner polygon. For a forward match, the target's left endpoint corresponds to the query's left endpoint. For a reverse match, it corresponds to the query's right endpoint.

![The four corners of an alignment ribbon, with the endpoint swap needed for a reverse match.](/assets/fb67d2122e16debc58ea1a587c974bf0ddd51142438e3f11da4b86dc044892c2/figures/blog/butterflies/ribbon-coordinates.svg)

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

The figures on this page are static SVGs. The notebook and generated HTML versions also let you zoom and hover over a match to inspect its coordinates. The complete plotting function adds species labels and axes; here is the highest-scoring retained match:

![One real alignment block connects its target and query intervals.](/assets/fb67d2122e16debc58ea1a587c974bf0ddd51142438e3f11da4b86dc044892c2/figures/blog/butterflies/first-alignment-ribbon.svg)

Its target interval is `[639721, 644608)` and its query interval is `[332091, 337300)`, with reverse orientation. The spans need not have equal lengths because the alignment includes insertions and deletions. This block has 5,307 alignment columns and 894 edits, giving about 83.15% identity under our stated definition.

## 11. From one match to the whole neighbourhood

A dot plot is a useful first view of many matches. Each line segment plots target position against query position; its slope shows orientation.

![A dot plot of the 355 retained alignments, distinguishing forward and reverse matches.](/assets/fb67d2122e16debc58ea1a587c974bf0ddd51142438e3f11da4b86dc044892c2/figures/blog/butterflies/alignment-dot-plot.svg)

The broad reverse trend agrees with the opposite orientations of the annotated *optix* regions in these deposited records. It does **not** by itself establish a biological inversion between the species: an assembly record can be deposited in either orientation.

Now draw all retained ribbons in the original coordinate systems:

![All retained alignment ribbons between the two one-million-base windows.](/assets/fb67d2122e16debc58ea1a587c974bf0ddd51142438e3f11da4b86dc044892c2/figures/blog/butterflies/alignment-ribbons.svg)

Overlapping blocks are possible, especially around repeated sequence, so summing their lengths would overstate coverage. After merging overlapping intervals, the displayed blocks cover 374,441 target bases and 387,528 query bases. These are the spans touched by the selected alignments; they are not counts of identical bases or whole-genome conservation estimates.

For a closer look at *optix*, we centre each gene on zero and reverse the erato display axis. In local window coordinates, the gene midpoints are 493,223.5 for melpomene and 495,840 for erato. The display transformations are therefore `x = target_position - 493223.5` and `x = -(query_position - 495840)`.

![The optix neighbourhood after centring the gene models and reversing the erato display direction.](/assets/fb67d2122e16debc58ea1a587c974bf0ddd51142438e3f11da4b86dc044892c2/figures/blog/butterflies/optix-centered-ribbons.svg)

![The display transformation shifts each gene midpoint to zero and reverses the query axis without changing stored coordinates.](/assets/fb67d2122e16debc58ea1a587c974bf0ddd51142438e3f11da4b86dc044892c2/figures/blog/butterflies/alignment-display-transform.svg)

Centring gene models is a display choice: their annotated transcript spans differ, so corresponding coding bases need not fall at exactly the same x position.

This transformation makes the comparison easier to read, but does not change the stored sequence or perform coordinate liftover. In the interactive notebook and HTML versions, the hover text retains the original local coordinates. When selecting a zoom, we keep blocks that overlap it, including blocks crossing its boundary; requiring both endpoints to lie inside the zoom would silently discard useful matches.

## 12. Add gene models and local GC

The annotation loader downloads the exact GFF files listed in `sources.json`, verifies their checksums, selects the appropriate accession, converts coordinates and clips features to our imported windows. There are **35 gene or pseudogene models** overlapping the erato window and **38** overlapping the melpomene window, including one pseudogene in each. A pseudogene is a gene-like sequence annotated as no longer encoding the usual functional product. A gene model is an annotation of a gene's structure; it is not a direct measurement of expression in these butterflies.

The notebook assembles these views from the validated query results. `make_figures` returns a dictionary of Plotly figures; for example, `figures["optix-neighborhood"].show()` opens this comparison.

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

![Current NCBI gene models, local GC measurements and BioseqDB alignments around optix.](/assets/fb67d2122e16debc58ea1a587c974bf0ddd51142438e3f11da4b86dc044892c2/figures/blog/butterflies/optix-neighborhood.svg)

![The current optix gene and exon models in their transcript-oriented display.](/assets/fb67d2122e16debc58ea1a587c974bf0ddd51142438e3f11da4b86dc044892c2/figures/blog/butterflies/optix-gene-models.svg)

The exon panel is a closer view of the annotations. Exons are the parts retained in a mature transcript; not every exonic base is protein-coding. The annotated erato transcript has two exons and the melpomene transcript has one in these files. Those annotation structures should not be inferred from the length of a single alignment block.

Read the three-panel neighbourhood figure in layers. The gene tracks place the matching segments in their annotated neighbourhood. The ribbons show sequence resemblance under the alignment and display settings. The GC tracks add a simple measurement made directly from the imported DNA. A gap in the ribbons can reflect sequence differences, repeats, or our search and display thresholds; it is not automatically a deleted gene.

The earlier tutorial also used transposable-element annotations and ATAC-seq tracks. A **transposable element** is a DNA element capable, or descended from an element capable, of moving within a genome. **Chromatin** is DNA together with the proteins that package it. DNA wraps around protein units called nucleosomes, and some regions are more exposed than others. **ATAC-seq** uses an enzyme called Tn5 transposase to insert sequencing adapters into accessible DNA, then maps the resulting reads. It measures accessible chromatin: regions where DNA is relatively available to the assay's transposase, in a particular biological sample. It can help investigate potential regulatory regions, but an accessibility peak is not the same as gene expression.

![A conceptual ATAC-seq workflow, from chromatin accessibility to mapped sequencing signal.](/assets/fb67d2122e16debc58ea1a587c974bf0ddd51142438e3f11da4b86dc044892c2/figures/blog/butterflies/atac-concept.svg)

This diagram explains where such an experiment could extend our comparison. It is not an ATAC-seq result from these samples. We have not transferred the old tutorial's TE or ATAC tracks onto the new assemblies; their coordinates belong to different references. The current NCBI files used here provide gene models, and the remaining tracks are measurements we can reproduce from the actual sequence windows. [The original ATAC-seq paper describes what the assay measures.](https://pmc.ncbi.nlm.nih.gov/articles/PMC3959825/)

## 13. What did we learn?

The two regions have almost the same overall GC content, yet their alignment is a collection of local blocks with gaps and competing matches. That is why composition checks and alignment answer different questions. The *optix* coding sequences are especially similar, while the surrounding DNA provides a larger and less uniform comparison. None of these plots alone identifies the regulatory change responsible for a wing pattern.

What I find useful about this workflow is that the sequences, their accession identifiers and the analysis stay connected. Selecting another region means changing explicit coordinates. Selecting another organism starts with another catalog query. The alignment returns rows which can be filtered, saved or joined with other metadata, and SQLAlchemy uses the same queries that we tested directly in PostgreSQL.

There are still choices to make: which assemblies to trust, which features correspond, which alignment settings make sense, and what a plot can support. BioseqDB does not remove those decisions. It gives us one place to carry them out and a fairly short route from an NCBI record to a reproducible Python figure.

To continue, try changing the display thresholds and compare the dot plot before interpreting an apparent break. Then select another gene neighbourhood, preserving the accession versions and coordinate conventions. Adding more genomes can turn this small pairwise exercise into a broader comparative analysis; building a pangenome representation would be a separate next step.
