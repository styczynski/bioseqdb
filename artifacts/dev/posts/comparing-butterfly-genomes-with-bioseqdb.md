Two butterflies can look remarkably similar without belonging to the same species. For *Heliconius*, part of the story why that happens, involves *optix*, a gene whose activity helps determine where red patterns appear on the wings. It encodes a transcription factor - so a protein which helps control the activity of other genes, that in very simplified terms works like sompphisticated switch, because changes in regulatory regions can affect not only if, but where and when a gene is used. [Reed and colleagues studied this connection across Heliconius species.](https://pubmed.ncbi.nlm.nih.gov/21778360/)

I decided to write this tutorial to showcase very simple example of analysis that one can do for single selected organism. As similar topics were always an interest of mine, I dedicated few last years to create a software that combines power of usual relational databases and software used commonly in compuatational biology to perform alignments (one can view them as problem of searching text in the database and understanding text similarity in specific biological way).
In this tutorial we will align a piece of chromosome or scaffold from two *Heliconius* species that includes *optix*. We will fetch the sequences from NCBI, keep them in PostgreSQL, ask BioseqDB (the database software I created and do little self-promotion :)) to find matching regions, and use SQLAlchemy and Plotly to inspect the results in Python. We won't need any separate alignment executable or tools.

I inspired myself by excellent blog post:  [butterfly genome-alignment tutorial using Minimap2 and R](https://stevenvb12.github.io/misc/2023/03/30/Minimap2-alignment.html). I wanted to work through the same kind of comparison with BioseqDB. This version uses newer NCBI assemblies and their own gene annotations, so the coordinates and results below will be different obviously!

![A schematic comparison of Heliconius butterflies and the optix region.](https://miro.medium.com/v2/resize:fit:1600/format:webp/1*ZLFOarRfLMBlL9p5iLB7HQ.png)

You can find all the full code, SQL, dependencies and Python notebook in [BioseqDB/examples/butterfies_alignments](https://github.com/BioseqDB/examples/tree/main/butterfies_alignments). You can also [find this tutorial’s code snapshot on the website](https://bioseqdb.com/blog/comparing-butterfly-genomes-with-bioseqdb/), but please note that neither provide FASTA files as I rely on NCBI datasets.

## 1. Very short biological introduction on what we will be doing

A DNA sequence is commonly written with four letters: **A**, **C**, **G** and **T**, representing adenine, cytosine, guanine and thymine. These are the bases of its nucleotides. A sequence file may also contain **N**, meaning the base is unresolved. Removing those letters would move every coordinate after them, so we keep them for simplicity.

DNA has two complementary strands - A pairs with T, and C with G and the strands also run in opposite directions. To read the same region from the other strand, we need to reverse the sequence and complement each letter. For example, the reverse complement of `AACGT` is `ACGTT`. I mentioning this, because this detail will matter quite a lot in our butterfly plots.

A genome is an organism's complete set of DNA, and an assembly is a reconstruction of it from sequencing data collected from some instruments (this process goes into "wet lab" so you ask your fellow collegues to run Illumina machine, which yields many many parts of the sequence you are interested in along with some noise. I won't go into details now how exactly this is done, but we have our sequence already in the dataset!).
A chromosome record can represent a reconstructed chromosome, while a scaffold is an assembled sequence whose placement may still be incomplete. We will be usiong record for two species - our *H. melpomene rosina* record is chromosome 18, while the *H. erato demophoon* record is an **unplaced scaffold** named `ntLink_18`

Now the core of any genomic/metagenomic analysis pipeline is the problem of alignment. An alignment is a process that places two sequences in columns so that corresponding bases (nucleotites A,C,T,G) can be compared. Equal letters are matches, different letters are substitutions, and gaps mean insertions or deletions relative to the other sequence. This allows us to understand for example where specific nucleotides were changed during evolutionary processes (how mutations arise, where etc.)

![DNA letters and a small alignment with a mismatch and a gap.](https://miro.medium.com/v2/resize:fit:1600/format:webp/1*OTwW8WJ4GCLdAf7BRihTow.png)

We cannot sensibly compare every position in a million-base sequence with every position in another million-base sequence (that is a lot of text, so our alignment algorithm would be ridiciously slow!). Thankfully in BioseqDB's I provided search function`mmsearch`. Internally it starts with short shared fragments, connects promising matches into chains, then aligns selected regions in more detail. This approach is called the [seed-and-extend idea](https://bioseqdb.com/v0.1/tutorials/project_alignment_foundations/#seed-and-extend) and it allows more efficient processing so you can align ridiciously long sequences with no issue. If you are interested in writing your own aligner, my [Python read-mapper tutorial](https://bioseqdb.com/blog/writing-a-dna-read-mapper-in-python/) goes through the algorithms specifically written in very simple Python.

Here we start analysis with a pairwise comparison across two species, caluclating alignemnt of fragments per every pair of sequences. It is a useful first exercise in comparative genomics.

## 2. Setting things up!

We will import two one-million-base sequence fragments, inspect their lengths and nucleotide counts, calculate GC content, align them, and progressively draw one match, all retained matches, and the neighbourhood of *optix*.

![NCBI sequences enter BioseqDB through SQL; SQLAlchemy retrieves results and Plotly draws them.](https://miro.medium.com/v2/resize:fit:1600/format:webp/1*LqXeflPMZjSk68tutBTz5Q.png)

PostgreSQL via BioseqDB extension stores the sequences and BioseqDB performs the alignment. We will write Python code using SQLAlchemy, that will send SQL and receive rows, which later will be used by Plotly to constrtuct some figures. You can do that same thing with plain SQL client like `psql` as every step of data processing will happen inside the database itself.

## 3. Install BioseqDB and check external access

> **Warning**
> **Availability on 16 October 2026.** The published `0.1.0-alpha.1` Docker images and native package builds omit the external-database integration used here (due to my oversight). It is planned for the next alpha release, but you can obviously compile source from Github directly. Currently using runtime `SET`, an API key, or a `GRANT` won't work as the code isn't present in the binary - when building one must use `BIOSEQ_BUILD_EXTERNAL_API_CLIENTS=ON` to enable this feature.

The [installation page](https://bioseqdb.com/v0.1/tutorials/getting_started/) covers the regular Docker and native packages. For this tutorial, use the external-enabled development build described in the [example's setup instructions](https://github.com/BioseqDB/examples/tree/main/butterfies_alignments#database-setup):

```sh
cmake -S . -B build/butterflies -DBIOSEQ_BUILD_EXTERNAL_API_CLIENTS=ON
cmake --build build/butterflies --target postgresql_bioseq postgresql_bioseq_cache
```

These lines modify an already configured source build to enable the external clients module, which is responsible for directly interfacing with NCBI database. Obviously a workaround at this time, would be directly downloading datasets by yourself and loading FASTA using `bioseq.load` function.
After installing that build and enabling the extension in a fresh database, check its SQL objects:

```sql
SELECT to_regnamespace('bioseq_external') IS NOT NULL AS external_schema,
       to_regclass('bioseq_external.catalog') IS NOT NULL AS catalog,
       to_regclass('bioseq_external.sequences') IS NOT NULL AS sequences;
```

**Result · 1 row**

| external_schema | catalog | sequences |
| --- | --- | --- |
| t | t | t |

All three checks should be true. If the tables exist but a query reports permission denied, ask the database owner to grant access to your tutorial role:

```sql
GRANT USAGE ON SCHEMA bioseq_external TO butterfly_reader;
GRANT SELECT ON bioseq_external.catalog, bioseq_external.sequences
TO butterfly_reader;
```

Replace `butterfly_reader` with your existing database role.

## 4. Search NCBI via SQL

We will start with some metadata to make sure we refer to correct organism etc. The following query will use NCBI provider in BioseqDB and send requests to puiblic NCBI API to search organisms for you (all in SQL - how amazing it is instead using hard to understand CLIs?)

```sql
SELECT accession, scientific_name, assembly_level, assembly_status
FROM bioseq_external.catalog
WHERE provider = 'ncbi'
  AND scientific_name = 'Heliconius erato demophoon'
ORDER BY accession;
```

**Result · 2 rows**

| accession | scientific_name | assembly_level | assembly_status |
| --- | --- | --- | --- |
| GCA_018249695.1 | Heliconius erato demophoon | Contig | current |
| GCA_056362945.2 | Heliconius erato demophoon | Scaffold | current |

 The `provider` conditionis important as it selects NCBI and BioseqDB supports manyh other public databases of sequences. Field `scientific_name` selects the organism, in this case our butterfly species and naturally all seasrch fields are compatible what official NCBI API provides. Field `accession` is the assembly identifier and it's common name for most of public data sources. The last important one - `assembly_level` - is a field that distinguishes such things as scaffold and chromosome assemblies (there may be different kind of objects in the database so we would like to see what's there!)

When you run this query, you *can probably* get different results as the database is updated. For this comparison, I selected these two assemblies and sequence windows:

| Organism | Assembly | Sequence record | Imported interval, zero-based |
| --- | --- | --- | --- |
| *H. erato demophoon* | [GCA_056362945.2](https://www.ncbi.nlm.nih.gov/datasets/genome/GCA_056362945.2/) | [JBTWUK020000023.1](https://www.ncbi.nlm.nih.gov/nuccore/JBTWUK020000023.1), unplaced scaffold | `[3700000, 4700000)` |
| *H. melpomene rosina* | [GCF_056956725.1](https://www.ncbi.nlm.nih.gov/datasets/genome/GCF_056956725.1/) | [NC_140406.1](https://www.ncbi.nlm.nih.gov/nuccore/NC_140406.1), chromosome 18 | `[600000, 1600000)` |

Each interval contains one million bases, or **1 Mb** (in this notation **kb** is one thousand bases and **1Mb** means sequence is one million A/C/T/G/maybe others characters-long).

We use zero-based, half-open coordinates throughout: `[start, end)` includes `start` and excludes `end`, so its length is `end - start`. NCBI GFF annotations use one-based inclusive coordinates; convert them with `start0 = start1 - 1`, leaving the end unchanged. [NCBI documents the GFF convention here.](https://www.ncbi.nlm.nih.gov/datasets/docs/v2/reference-docs/file-formats/annotation-files/about-ncbi-gff3/)

![Coordinate conversion from one-based annotation positions to zero-based slices and local window positions.](https://miro.medium.com/v2/resize:fit:1600/format:webp/1*3exiaQ4QR3XyrbRFAh6Rhw.png)

## 5. Now the storage!

Here is our own table that we can create as in ordinary SQL you should be familiar with. `USING bioseq` gives it BioseqDB's sequence storage and search support. The other fields are tehre just to save the information needed to interpret a coordinates later.

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

`INSERT ... SELECT` copies the returned sequences into our table. Once imported, the composition and alignment queries use local data.
Let's inspect a short prefix to see what's there out of curiousity:

```sql
SELECT key, accession, bioseq.len(seq) AS length,
       seq[0:20]::text AS first_20_bases
FROM butterflies.regions
ORDER BY key;
```

**Result · 2 rows**

| key | accession | length | first_20_bases |
| --- | --- | --- | --- |
| erato | JBTWUK020000023.1 | 1000000 | TCGCGTGAATGCAAATGCGG |
| melpomene | NC_140406.1 | 1000000 | ATATCAAATACCAATGTTAG |

`bioseq.len(seq)` gives the number of bases. `seq[0:20]` takes the first twenty, using the same half-open convention as our genomic windows. A coordinate returned by an alignment will be relative to this imported sequence. To get back to the NCBI record, add its stored `start0`.

## 6. Let's do some exploration and count bases!

Two records can have similar overall composition and still contain very different sequences. I don't want to read entire million of bases as this would crowd up our screen, so lets stick to some meta properties, like the sequence lengths and counts for every nucleotide:

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

**Result · 2 rows**

```text
Row 1
key: erato
accession: JBTWUK020000023.1
organism: Heliconius erato demophoon
interval_start: 3700000
length: 1000000
a: 329927
c: 172322
g: 170873
t: 326878
n: 0
gc_percent: 34.3195

Row 2
key: melpomene
accession: NC_140406.1
organism: Heliconius melpomene rosina
interval_start: 600000
length: 1000000
a: 327164
c: 171599
g: 171833
t: 329404
n: 0
gc_percent: 34.3432
```

`bioseq.symbol_count` counts a selected letter and **GC content** is the percentage of called bases which are G or C:

{% katex %}
\mathrm{GC\%} = 100\frac{G+C}{A+C+G+T}.
{% endkatex %}

We exclude N from the denominator as N represents an unresolved base and in practice it can be anything of A/C/T/G, but we never know for sure. Usually Ns are small part of the sequence overall and we are not interested in them so much. `NULLIF` here avoids dividing by zero if a sequence contains no called bases. That case is unknown GC content, not zero GC. Lucily for us, both windows we saved and used here happen to contain no Ns (hooray!)

![Lengths of the two imported sequence windows.](https://miro.medium.com/v2/resize:fit:1600/format:webp/1*l-fu4X4ql8kbJ2k25xX6Mw.png)

![Lengths of the complete NCBI source records, distinct from the imported windows.](https://miro.medium.com/v2/resize:fit:1600/format:webp/1*NgoJ4xT7ppRjLTMauxsNfw.png)

Both analysis windows are exactly 1 Mb. The source erato scaffold is 5,549,739 bases, the source melpomene chromosome is 16,989,447 bases.

![Measured counts of A, C, G, T and N in the two NCBI windows.](https://miro.medium.com/v2/resize:fit:1600/format:webp/1*oI_HA7OYqYEZKmNx-5ZBxQ.png)

A and T are more frequent than C and G in both windows. Erato contains 329,927 A, 172,322 C, 170,873 G and 326,878 T bases. Melpomene contains 327,164 A, 171,599 C, 171,833 G and 329,404 T bases.

![GC percentage of called bases in each imported sequence.](https://miro.medium.com/v2/resize:fit:1600/format:webp/1*qLzIztOYS81HKlQLHlsTng.png)

The GC values are close: **34.3195%** and **34.3432%**, but note that this isn't reflecting their similarity as we don't measure here any position-based properties.

![GC content measured in consecutive 10 kb windows along both sequences.](https://miro.medium.com/v2/resize:fit:1600/format:webp/1*EWQGsttIXef_PzPMQuj7EQ.png)

More interesting is measuring GC content in windows of specified length, because they we have more than only one single data point per entire sequence of interest (they are long so any mutations will average to very similar total GC content percentage. Using smaller windows on sequences allow us to see some regions where "something happens" and the structure of sequence changes - all this without actually comparing all nucleotides themselves on our screen  - in practice this can be used as very primitive tool to quickly search for regions of interest)

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

**Result · 6 rows**

| accession | start | end | gc_percent | n_fraction |
| --- | --- | --- | --- | --- |
| JBTWUK020000023.1 | 0 | 10000 | 33.6800 | 0.000000000000000000000000 |
| JBTWUK020000023.1 | 10000 | 20000 | 34.1000 | 0.000000000000000000000000 |
| JBTWUK020000023.1 | 20000 | 30000 | 35.7900 | 0.000000000000000000000000 |
| JBTWUK020000023.1 | 30000 | 40000 | 35.7600 | 0.000000000000000000000000 |
| JBTWUK020000023.1 | 40000 | 50000 | 33.2900 | 0.000000000000000000000000 |
| JBTWUK020000023.1 | 50000 | 60000 | 35.6600 | 0.000000000000000000000000 |

These curves show where the composition changes within each imported region - peaks/valleyhs or other interesting shapes are not, by themselves, genes, enhancers or anything else and we need the appropriate annotations or experiments in "wet labs" to check, but nevertheless it's some information we can use in our analysis.

## 7. Align the regions inside PostgreSQL

We will use melpomene as the **target** and erato as the **query**. Both come from the same local table, selected by their `key` fields. This listing shows the first eight result (`06_align.sql` in the repository omits the final `LIMIT` and returns every block. I used `LIMIT` here just for simpler illustrative purposes)

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

**Result · 8 rows**

```text
Row 1
target_accession: NC_140406.1
query_accession: JBTWUK020000023.1
target_start: 639721
target_end: 644608
query_start: 332091
query_end: 337300
is_reverse: t
score: 5874
cigar: 27M4D14M2I9M2I39M1I29M3I15M1I3M1D8M3I53M4D3M1D12M1D24M7D2M1D8M1I3M4I15M1D14M8I89M2D78M16D5M5D3M4D4M6D1M4D6M1I13M2D1M2D1M1D32M4D15M1I33M2I6M8I5M3I1M1I17M3D17M12I4M7I5M1I10M7I2M4I6M2D5M1I6M5I12M5I5M5I4M4I10M6I5M11I2M8I1M4I5M10I4M12I3M41I3M1I2M5I4M1I3M1I1M1I114M6I41M3I13M5I89M1I29M1I9M3I9M5I6M3I11M1D1M2D5M1D4M1I20M2I2M1I21M2I3M1D47M4D71M3I3M2I46M2D101M3D67M6D65M2I103M12I7M1I4M1I65M60I5M19I6M32I3M37I5M3I1M3I26M8I4M1I21M1I29M1I2M2D36M2I37M1I687M3I2217M1D1M1D3M1D15M1D1M2I12M1D15M
edits: 894
block_length: 5307
mapq: 60
is_primary: t

Row 2
target_accession: NC_140406.1
query_accession: JBTWUK020000023.1
target_start: 243317
target_end: 247028
query_start: 793769
query_end: 797544
is_reverse: t
score: 5190
cigar: 32M3I11M1I2738M3I21M23D3M2D4M18D1M5D1M6D5M6D5M15D13M6D11M2I11M3I3M8I25M2I125M4D8M1I17M1I4M1D27M3I14M22I3M18I12M5I3M5I4M1D11M3I1M11I3M5I6M4I2M3I1M10I4M1D1M3I5M1I1M1I2M4I5M6I1M6I10M1I2M1I9M15I2M10I22M1D3M3I13M1I1M1I4M1D6M1D59M1I8M5D12M2D34M2D7M1I5M2I36M6D228M
edits: 484
block_length: 3881
mapq: 60
is_primary: t

Row 3
target_accession: NC_140406.1
query_accession: JBTWUK020000023.1
target_start: 436485
target_end: 440921
query_start: 582051
query_end: 586426
is_reverse: t
score: 4902
cigar: 55M2I4M2D399M1I2M1D977M1D36M4I6M1D2M3D34M2D2M5D6M2D4M3D21M4I32M2D1M2D13M1I3M1I30M4I44M2I3M2D13M1D77M1I4M2D6M1D2M3D29M1D5M1D6M2D10M2I15M1I1M2I6M2D11M1I9M2D20M2I2M3I5M1D4M1D2M1I1M1I3M1D3M2I257M2I9M3I37M1D27M3D89M3D1M7D13M5D13M12D3M1I14M1D42M2I102M2D4M2D21M1D39M6I18M6D5M2D2M1D5M3D6M18D87M1D5M2D5M11I11M12I1191M3D50M4I9M7I5M2I24M5I1M3I72M2I2M2I5M3I11M2I2M1I1M1I12M2I4M4D4M2D3M1D4M3D2M2D5M1D20M6D2M4D3M15D2M4D5M8D107M
edits: 728
block_length: 4542
mapq: 60
is_primary: t

Row 4
target_accession: NC_140406.1
query_accession: JBTWUK020000023.1
target_start: 735657
target_end: 740842
query_start: 214190
query_end: 219875
is_reverse: t
score: 4868
cigar: 5M2D10M4I194M3I4M10I6M1I30M10I3M9I7M36I5M62I4M55I4M8I2M8I56M7I8M1I58M2I71M4I28M8I20M1D12M8D15M2I1M1D19M1I16M1D49M17D3M5D3M9D6M5D8M9D2M5D2M1D62M6I5M1I11M3I2M1I2M7I2M7I6M9I11M2I2M1I5M4I17M5I3M7I4M2I4M1I2M2I9M1I7M2D3M3D11M5I4M1I1M2I7M7I4M3I5M12I4M1D8M21I115M1I1M3I94M1I235M6D44M2I16M18I2M1I2M24I62M1I5M5D4M1D4M5I2M1I25M1D34M1I48M2I8M2I5M1D49M1D4M3I36M1I114M1D91M1I10M1D210M3I38M1I16M1D30M2I5M4D10M4D49M1I252M3I31M2I150M1I14M2I138M3I5M1I151M3I9M1I8M12I34M10I1M2I22M2I93M2D1M1D22M1I35M4I27M2I25M1I140M1D33M2I12M1D73M1D63M2I51M3I23M9I2M1I4M2I44M1D102M5I25M4I2M5I4M10I55M14I10M6I4M3I40M2D5M1D6M4D3M1I5M3I23M1D9M2D6M1I5M1I3M3I8M1I3M2D4M2D4M5I5M2I2M1I2M1I3M1I7M6I9M5I1M5I4M3I13M4D14M2D4M3I13M2D5M1D88M6I44M1I1M2D31M8D132M1I94M6I90M17D4M1D41M4I3M4I12M7I4M7I63M3D8M1I2M1I16M1D21M3I116M2D58M1I14M11I2M1I7M1D3M21I24M1I3M1I9M14I16M3D24M
edits: 1297
block_length: 5849
mapq: 60
is_primary: t

Row 5
target_accession: NC_140406.1
query_accession: JBTWUK020000023.1
target_start: 782603
target_end: 787530
query_start: 156816
query_end: 162219
is_reverse: t
score: 4858
cigar: 20M3I41M7I8M4I26M1D3M1I10M1I2M3D38M1D222M1I134M1D16M1I98M3D2M2D5M2D9M2D81M2I8M1D1M1D9M1I2M2I7M1I8M4I35M11I5M2I2M3I3M5D5M3D7M10I15M1I2M2I9M2I5M10I5M3I15M1D2M4I79M7I215M1I46M9I19M1I19M1I44M1D61M5D145M3I57M1I6M1D3M1I6M1D2M1I50M18I9M1I1M1I24M12I4M7I2M2I4M4I4M1I65M8I6M2I7M2D5M1I3M1I69M2D1M2D148M6I165M10I38M1D9M3I1M2I33M1D52M2D33M4I22M1I14M1D16M4D109M7I1M1I3M1I12M8I1M1I16M4I32M1D434M2I80M1I12M3I23M1D70M1I1M1I37M2D60M2D278M1I73M16I99M1D49M1D7M1D11M3I4M1I3M1I2M1I4M1I2M1D5M6I6M1D6M6I3M1I11M3I9M2I4M3I5M5I1M4I7M3I8M3D6M3D2M1D10M2D4M3I17M6I10M2I2M1D3M5I2M2I1M2I8M2I21M1I2M1I1M4I6M2I1M3I12M1I7M4I5M1I7M1I6M5I5M1I3M1I2M11I1M1I6M9I154M14I5M2D3M4D56M1D14M3I2M3I7M2I2M2I2M2I3M11I3M5I3M13I13M7I1M6I13M4I4M2I10M1I11M2I2M10I11M3I2M1I4M18I7M6I2M7I2M1I4M5I2M3I4M3I2M4I1M1I2M2I10M8I9M9I1M2I4M26I4M6I2M1I1M1I3M1D2M7I11M1I25M1I1M1I11M1D29M4D257M1I34M7I2M1I43M6I59M
edits: 1109
block_length: 5486
mapq: 60
is_primary: t

Row 6
target_accession: NC_140406.1
query_accession: JBTWUK020000023.1
target_start: 123169
target_end: 127102
query_start: 931912
query_end: 935729
is_reverse: t
score: 4824
cigar: 23M4D1M1D3M2D14M1D21M1D7M3D45M1I15M3D7M1D19M3D2M13D2M4D47M1I13M3D56M2I23M1D43M4I9M9I90M1I86M1I179M1I2M1I14M7D2M1D678M1I1M1I32M1D14M3I4M1D9M1I7M1I76M7I21M3I29M1I62M3I7M4I2M3I30M4D21M1I66M16D5M7D8M8D6M40D4M19D347M1D120M1I1M1I36M2I7M2D5M4D9M1D7M2D2M6D61M8D61M2D15M1D204M5I5M2I64M3D1M1D726M2I15M1D94M1I34M1I5M1D1M1D11M1I2M1I2M2I85M1I4M3D2M4D2M1D14M
edits: 567
block_length: 4003
mapq: 60
is_primary: t

Row 7
target_accession: NC_140406.1
query_accession: JBTWUK020000023.1
target_start: 421610
target_end: 427235
query_start: 593427
query_end: 599306
is_reverse: t
score: 4738
cigar: 35M2I9M2D36M1D4M1I1M1I76M1I10M9I99M1D30M2I18M5D562M1D95M4D54M9D8M1D1M1D10M2I4M3I19M3D3M1D3M1D46M1D10M14D129M1I3M1D25M8D3M1D475M1D21M1D70M1D4M2I2M2I16M3I81M1I38M9I9M1D4M2I39M4I166M3D1M5D33M2D46M2D6M6D1M1D7M1I4M1D2M1D12M1D10M5I6M5D12M1D3M4D3M3I10M4D3M1D2M4I11M1I3M4D6M1I3M2D3M1D6M2D2M11D72M1I10M3I21M2I78M4D62M3D19M3I8M9I8M14I2M3I62M33I4M24I7M36I5M3I1M4I3M19I3M22I4M32I2M1I2M10I7M4I2M13I3M11I7M20I5M38I18M1I4M2I992M1D7M3I8M1I21M2D6M11I3M3I31M1D1M1D4M2D12M2D1M1I2M2D50M10D14M7D19M20D7M5D1M2D4M7D3M2D4M4D115M1I1M3I20M2I4M2I76M2D1M1D1M1D13M4I9M6D1M5D14M8D4M15D5M1D3M2D3M3I6M3I8M2D6M1I2M2D5M3D6M1I2M7D13M1I56M11I10M6I88M8I13M5I9M4I9M3I6M4I5M1I3M1I7M3I5M7I5M9I3M11I5M22I9M3I9M8I4M6I4M13I7M1D3M3I16M1I3M3I2M3I2M2I1M4I3M4I3M8I4M6I2M1I2M4I8M3I3M1I16M1I8M1I1M1I2M1I3M2I5M1I4M2I11M21I3M4I3M1I3M2I3M14I2M2I6M1D5M1D2M1I4M11I238M2D6M16D4M23D1M6D1M5D3M1D3M19D6M5D9M5D3M10D1M2D5M18D9M1D2M2D3M4D5M9D6M5D4M4D4M1D195M
edits: 1509
block_length: 6266
mapq: 60
is_primary: t

Row 8
target_accession: NC_140406.1
query_accession: JBTWUK020000023.1
target_start: 409490
target_end: 413107
query_start: 608855
query_end: 612268
is_reverse: t
score: 4586
cigar: 68M30I1M1I2M1I7M1I24M3D8M1D772M7D15M1I3M4D11M6D8M1I10M7D92M2D13M6D23M4I10M3I63M3D8M2I69M1I1M1I5M4D24M2D8M16D227M2I2M4I947M19D3M1D6M2D4M1D42M4D2M4D2M45D6M49D4M37D3M3D1M1D3M10D6M23D55M1D17M1I2M1I5M2D54M1I39M4I679M
edits: 539
block_length: 3676
mapq: 60
is_primary: t
```

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

```python
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

```python
statement = text((sql_dir / "04_inspect.sql").read_text())
with engine.connect() as connection:
    preview = connection.execute(statement).mappings().all()
for row in preview:
    print(dict(row))
```

I use `create_engine`to tell SQLAlchemy how and where to connect and later the `with engine.connect()` block acquires a connection and returns it to the pool when finished. `.mappings()` gives rows with column names, which is convenient for tables and figures. We disable psycopg's automatic prepared statements here because the current external-query implementation needs the accession and range values available in its custom query plan.

The first two returned rows are:

```text
{'key': 'erato', 'accession': 'JBTWUK020000023.1', 'length': 1000000, 'first_20_bases': 'TCGCGTGAATGCAAATGCGG'}
{'key': 'melpomene', 'accession': 'NC_140406.1', 'length': 1000000, 'first_20_bases': 'ATATCAAATACCAATGTTAG'}
```

For a changing value, bind a parameter instead of building SQL with string interpolation:

```python
statement = text((sql_dir / "01_discover.sql").read_text())
with engine.connect() as connection:
    assemblies = connection.execute(
        statement, {"organism": "Heliconius erato demophoon"}
    ).mappings().all()
print([dict(row) for row in assemblies])
```

If you want to read more about SQLAlchemy transactions and what is `engine.connect()` you are welcome to read more here: [SQLAlchemy's tutorial describes these transaction patterns.](https://docs.sqlalchemy.org/en/20/tutorial/dbapi_transactions.html)

To repeat the imports from Python, use the transactional helper:

```python
import json
from butterfly_tutorial.workflow import import_regions

sources = json.loads((root / "sources.json").read_text())
import_regions(engine, sources["records"])
```

The alignment is SQL, so it's another query through the same connection (nothing special here really!)

```python
statement = text((sql_dir / "06_align.sql").read_text())
with engine.connect() as connection:
    matches = [dict(row) for row in connection.execute(statement).mappings()]
print(f"{len(matches)} alignment blocks")
```

The `matches` list contains the same 661 rows as the SQL query. The full runner in the GitHub repo also validates lengths, nucleotide counts, sequence hashes, CIGAR operations and the *optix* coding sequences before, but I'm not going to present it all here as this is just some additional boilerplate not very relevant to the analysis itself.

## 10. First drawing!

There's a think called ribbon diagram, where a sequence track is just a rectangle whose x coordinates are sequence positions and its height and y position are display choices:

![A sequence interval drawn as a rectangle with genomic x coordinates and arbitrary display y coordinates.](https://miro.medium.com/v2/resize:fit:1600/format:webp/1*9VIPGYOqTasVilFMLc6mAg.png)

An alignment connects two intervals with a four-corner polygon. For a forward match, the target's left endpoint corresponds to the query's left endpoint. For a reverse match, it corresponds to the query's right endpoint. No rocket science here, right? Only butterfly science! Huh.

![The four corners of an alignment ribbon, with the endpoint swap needed for a reverse match.](https://miro.medium.com/v2/resize:fit:1600/format:webp/1*b4tu7j2ca2XEsTIugCKqUA.png)

Here is the small plotting operation behind a ribbon:

```python
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

The figures on this page are static images, but the notebook and generated HTML versions also let you zoom and hover over a match to inspect its coordinates and it feel much cooler to see them update after your code changes. You can give it a try.

![One real alignment block connects its target and query intervals.](https://miro.medium.com/v2/resize:fit:1600/format:webp/1*jdgGvt09eVpqQMuyqAJA_A.png)

Its target interval is `[639721, 644608)` and its query interval is `[332091, 337300)`, with reverse orientation. The spans need not have equal lengths because the alignment includes insertions and deletions. This block has 5,307 alignment columns and 894 edits, giving about 83.15% identity.

## 11. We need more plots! Mooore!

One plot won't satisfy my needs for colourful graphs. We can use a dot plot. A dot plot is a useful first view of many matches - each line segment plots target position against query position and its slope shows orientation.

![A dot plot of the 355 retained alignments, distinguishing forward and reverse matches.](https://miro.medium.com/v2/resize:fit:1600/format:webp/1*0Z1Wl-8ACQgYLNdEQ_v7gw.png)

The broad reverse trend agrees with the opposite orientations of the annotated *optix* regions in records, but it does **not** by itself mean a biological inversion between the species and somehow they reversed everyrthing just by randon accident - an assembly record can be deposited in either orientation to NCBI so it's likely that.

Now draw all retained ribbons in the original coordinate systems:

![All retained alignment ribbons between the two one-million-base windows.](https://miro.medium.com/v2/resize:fit:1600/format:webp/1*levy578wMAFUsk5Iht0JjA.png)

Overlapping blocks are possible, especially around repeated sequence, so summing their lengths would overstate coverage. After merging overlapping intervals, the displayed blocks cover 374,441 target bases and 387,528 query bases. These are the spans touched by the selected alignments.

For a closer look at *optix*, we centre each gene on zero and reverse the erato display axis. In local window coordinates, the gene midpoints are 493,223.5 for melpomene and 495,840 for erato. The display transformations are therefore `x = target_position - 493223.5` and `x = -(query_position - 495840)`.

![The optix neighbourhood after centring the gene models and reversing the erato display direction.](https://miro.medium.com/v2/resize:fit:1600/format:webp/1*XozUpxJF7WFrubrqZoTUqQ.png)

![The display transformation shifts each gene midpoint to zero and reverses the query axis without changing stored coordinates.](https://miro.medium.com/v2/resize:fit:1600/format:webp/1*9KpyqeRdpHmM_n_UdfhWcg.png)

We need to make a choice how to  center the genome and it's compltely arbitrary - their annotated transcript spans differ, so corresponding coding bases need not fall at exactly the same x position. This transformation makes the comparison easier to read. In the interactive notebook and HTML versions, the hover text retains the original local coordinates. When selecting a zoom, we keep blocks that overlap it, including blocks crossing its boundary, requiring both endpoints to lie inside the zoom would silently discard useful matches. If you have better idea how to draw this, go ahead!

## 12. Add gene models

The erato feature `AC2CFC_001707` is annotated as homeobox protein SIX6; I treat it as the *optix* homolog here because its coding sequence shares 778/804 bases and its protein 266/267 amino acids with melpomene Optix.

The annotation loader on GitHub downloads the exact GFF files listed in `sources.json`, selects the appropriate accession, converts coordinates and clips features to our imported windows. There are **35 gene or pseudogene models** overlapping the erato window and **38** overlapping the melpomene window, including one pseudogene in each. A pseudogene is a gene-like sequence annotated as no longer encoding the usual functional product. A gene model is an annotation of a gene's structure.

The notebook assembles these views and `make_figures` returns a dictionary of Plotly figures.

```python
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

![Current NCBI gene models, local GC measurements and BioseqDB alignments around optix.](https://miro.medium.com/v2/resize:fit:1600/format:webp/1*okAjCJPHhgsPRT_QXnkhKQ.png)

![The current optix gene and exon models in their transcript-oriented display.](https://miro.medium.com/v2/resize:fit:1600/format:webp/1*bv6Dzku8eUaSkJDPQbiJtg.png)

The exon panel is a closer view of the annotations. Exons are the parts retained in a mature transcript, The annotated erato transcript has two exons and the melpomene transcript has one in these files and I put everything into one simple graphic. The gene here tracks place the matching segments in their annotated neighbourhood. The ribbons show sequence resemblance under the alignment and the last one - GC tracks add a simple measurement of GC content made directly from the imported DNA.

## 13. What did we learn?

The two regions have almost the same overall GC content, yet their alignment is a collection of local blocks with gaps and competing matches. The *optix* coding sequences are especially similar, while the surrounding DNA provides a larger and less uniform comparison. None of these plots alone identifies the regulatory change responsible for a wing pattern, but hey, we got some cool plots!

What I find useful about this workflow is that the sequences, their accession identifiers and the analysis stay all withing SQL. Selecting another region means changing explicit coordinates in your code without the need to look for CLI docs in the internet or using different tools blues together via bash scripts. You can even search NCBI via SQL layer that directly uses their API. I didn't show it here but you can export the table into usual FASTA as well.

To continue, try changing the display thresholds and compare the dot plot before interpreting an apparent break. Then select another gene neighbourhood, preserving the accession versions and coordinate conventions. Adding more genomes can turn this small pairwise exercise into a broader comparative analysis. with even more cool charts!

---

[Please check my OS project - BioseqDB](https://bioseqdb.com/).

[Read this post on BioseqDB](https://bioseqdb.com/blog/comparing-butterfly-genomes-with-bioseqdb/) · [Read on Medium](https://medium.com/@styczynski/comparing-butterfly-genomes-with-bioseqdb-sqlalchemy-and-plotly-62dda57c6d9b) · [Follow BioseqDB on LinkedIn](https://www.linkedin.com/company/bioseqdb/).
