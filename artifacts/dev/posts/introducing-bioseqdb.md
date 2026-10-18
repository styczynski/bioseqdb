BioseqDB is a PostgreSQL extension for storing, indexing and searching biological sequences with SQL. Combine genomic matches with relational data using compact sequence types and columnar storage!

I wanted to keep sequences and their metadata together. If a sample has an organism, collection date or patient identifier, those values should be ordinary columns beside its DNA. Searching for a similar sequence should fit into the same query as filtering those columns.

![Load FASTA or FASTQ and metadata into a BioseqDB table, then combine metadata filters and sequence alignment in one SQL query.](https://cdn-images-1.medium.com/max/1600/1*ja1m2JVtHRKvvt0bKfxQ2A.png)

## How it started

This project required several years of exploration and experiments. I began without a formal background in computational biology, thus learning the field was part of the research.

- **2021–2022:** Working with large GISAID datasets made FASTA files hard to manage. I proposed the database idea in May 2021 and directed a team prototype that connected BWA-MEM to PostgreSQL in May 2022.
- **2023–2024:** I wrote a [Python aligner](https://bioseqdb.com/blog/writing-a-dna-read-mapper-in-python/) and then improved it, learning how the algorithms work along the way.
- **December 2025–2026:** The current BioseqDB began with minimap2 and columnar storage. Custom seeding and chaining, indexes, sequence slices and end-to-end benchmarks followed.

The current BioseqDB is a separate project, designed and written by me. It contains no code or intellectual property from the early BWA-MEM integration experiments. The [introduction and full timeline](https://bioseqdb.com/v0.1/tutorials/project_introduction/#sec-introduction-timeline) have more of the story.

## Two queries to get a feel for it

These examples use the three synthetic samples from the [SQL walkthrough](https://bioseqdb.com/v0.1/tutorials/getting_started/#2-insert-patient-data). The `samples` table has ordinary metadata columns and a `bioseq.seq` column. `USING bioseq` selects the sequence-aware storage.

First, read metadata and a small piece of DNA. `bioseq.len` gives the sequence length; `[:16]` takes its first sixteen bases.

```sql
SELECT patient_name, bioseq.len(seq) AS bases,
  seq[:16]::text AS prefix
FROM samples
WHERE age >= 30
ORDER BY age DESC;
```

**Result · 2 rows**

| patient_name | bases | prefix |
| --- | --- | --- |
| Ben | 512 | ACAAAAAGGGGCTTTC |
| Anna | 4096 | AAAGAGTAGATCGTAG |

Now search those samples for Ben's sequence, stored in row 2. `mmsearch` performs the alignment; `bioseq.score` and `bioseq.cigar` describe each match.

```sql
SELECT t.patient_name,
  bioseq.target_start(t.seq) AS start,
  bioseq.score(t.seq) AS score,
  bioseq.cigar(t.seq) AS cigar
FROM samples AS t, samples AS q
WHERE t.age >= 30 AND q.id = 2
  AND mmsearch(t.seq, q.seq::bioseq.seq('dna4', 64))
ORDER BY t.id, start;
```

**Result · 3 rows**

| patient_name | start | score | cigar |
| --- | --- | --- | --- |
| Anna | 256 | 1024 | 512M |
| Anna | 2304 | 1024 | 512M |
| Ben | 0 | 1024 | 512M |

Anna has two occurrences and Ben has one. These are exact matches, so each CIGAR is `512M`. Alignment can also handle differences between sequences: substitutions, insertions and deletions. The [alignment chapter](https://bioseqdb.com/v0.1/tutorials/project_alignment_foundations/) explains how seeding, chaining and base alignment fit together.

## Where it fits beside existing tools

BioseqDB builds on ideas from existing aligners, especially minimap2. The difference I wanted was the workflow: keep the data in PostgreSQL and combine sequence search with SQL filters and joins.

| Tool | Main focus |
|:---|:---|
| [minimap2](https://github.com/lh3/minimap2) | Mapping reads and aligning nucleotide sequences against a reference. |
| [BLAST](https://blast.ncbi.nlm.nih.gov/Blast.cgi) | Finding local sequence similarity in sequence databases. |
| [Winnowmap](https://github.com/marbl/Winnowmap) | Long-read mapping, including repetitive regions. |
| [LexicMap](https://bioinf.shenwei.me/LexicMap/) | Nucleotide alignment against large collections of prokaryotic genomes. |
| BioseqDB | Storing sequences beside relational metadata and running alignment inside SQL. |

In my [benchmarks](https://bioseqdb.com/benchmarks/), the preferred CPU configuration was typically 3–8× faster than minimap2 when a prepared reference index was available. Without that index, BioseqDB was usually slower than minimap2. These tests used 1,024 synthetic queries against one 2 or 4 Gbp reference, not a collection of real genomes. Several external tools also needed the reference split into smaller records, so the [measurement details](https://bioseqdb.com/v0.1/tutorials/project_benchmarking/#benchmarking-methodology) matter when comparing them.

There is still work to do. The current GPU paths were slower than the CPU paths in these measurements, and indexing has [known limitations](https://bioseqdb.com/v0.1/tutorials/project_conclusion/). This first release is experimental software.

## Try the Docker image

The image already includes PostgreSQL and BioseqDB. This is the Ubuntu 26.04 / PostgreSQL 19 prerelease image from the [quick start](https://bioseqdb.com/v0.1/tutorials/getting_started/#docker-quick-start):

```sh
docker run -d --name bioseqdb-demo \
  -e POSTGRES_PASSWORD=demo-change-me \
  -p 127.0.0.1:55432:5432 \
  styczynski/bioseqdb:0.1.0-alpha.1-pg19-ubuntu26.04
```

Wait until PostgreSQL is ready, then open its SQL prompt:

```sh
until docker exec bioseqdb-demo pg_isready -h 127.0.0.1 -U postgres; do sleep 1; done
docker exec -it -u postgres bioseqdb-demo psql -U postgres -d postgres
```

Follow the [walkthrough](https://bioseqdb.com/v0.1/tutorials/getting_started/#sql-walkthrough) to create the sample table, load a FASTA file and try the queries above. I would like to hear how it works with your data: [open an issue](https://github.com/styczynski/bioseqdb/issues) or [contact me](https://bioseqdb.com/contact/).

---

[Please check my OS project - BioseqDB](https://bioseqdb.com/).

[Read this post on BioseqDB](https://bioseqdb.com/blog/introducing-bioseqdb/) · [Read on Medium](https://medium.com/@styczynski/introducing-bioseqdb-28587b24fa5d) · [Follow BioseqDB on LinkedIn](https://www.linkedin.com/company/bioseqdb/).
