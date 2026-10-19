# Sequence workflows in BioseqDB and SeqKit

Runnable code for [Sequence workflows in BioseqDB and SeqKit](https://bioseqdb.com/blog/seqkit-workflows-in-bioseqdb/).

The examples download mitochondrial chromosomes from UCSC for human (`hg38`), mouse (`mm39`) and zebrafish (`danRer11`). BioseqDB stores them in PostgreSQL, cuts them into 51 windows, and runs the sequence queries from the tutorial. SeqKit runs against FASTA files exported from the same database rows.

## Before running

Use PostgreSQL with BioseqDB installed and built with `BIOSEQ_BUILD_EXTERNAL_API_CLIENTS=ON`. Published `0.1.0-alpha.1` binaries omit the external-source module. The tutorial explains how to build it and the SQL role needs access to `bioseq_external.sequences`.

Install [SeqKit](https://bioinf.shenwei.me/seqkit/download/) if you want to run the command-line comparisons. These commands were tested with SeqKit 2.14.0. Python is only needed for the charts.

```sh
git clone https://github.com/BioseqDB/examples.git
cd examples/seqkit_workflows
```

## Import, query and compare

Run this in a fresh database, or one without a `seqkit_lab` schema. Set `DATABASE_URL` to your PostgreSQL connection string first. For a local database called `bioseq_examples`, for example:

```sh
export DATABASE_URL='postgresql://localhost/bioseq_examples'
psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -f 01_import.sql
psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -f 02_export.sql
psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -f 03_queries.sql
bash seqkit.sh
```

`01_import.sql` retrieves the three `chrM` sequences through BioseqDB SQL and creates the windows. `02_export.sql` writes `genomes.fa` and `windows.fa` in your current directory. `03_queries.sql` runs the tutorial queries. Sequence data is downloaded when `01_import.sql` runs.

If SeqKit is outside your PATH, run `SEQKIT=/path/to/seqkit bash seqkit.sh`. The script writes its derived FASTA files and terminal outputs under `results/seqkit`, replacing those tutorial outputs on reruns. The repeated-name, edited-base and joined-end examples use small constructed inputs so you can see what each command changes.

The SQL examples include filtering by ID and sequence, GC content, slicing, reverse complements, concatenation, sorting, sampling, renaming and deduplication. Some commands have deliberately different semantics. SeqKit's probability sample can return a different number of records from the SQL example's fixed-size sample, and sequence matching here uses the forward strand unless the command says otherwise. The tutorial explains these choices beside the examples.

## Draw the charts

```sh
python3 -m venv .venv
.venv/bin/pip install matplotlib==3.10.7
.venv/bin/python render_charts.py --output figures
```

The renderer reads `measurements.json`, which contains the values measured from the SQL import on 19 October 2026. It produces SVG charts for sequence lengths, nucleotide composition and GC content across the windows. The SVGs have transparent backgrounds and adapt to light and dark mode through `prefers-color-scheme`.

Window coordinates are local to each assembly. GC uses A, C, G and T in its denominator, while chromosome length includes N.

## Files and sources

`queries.json` contains the SQL shown in the tutorial and in `03_queries.sql`. `seqkit-previews.json` records the corresponding terminal commands and their measured outputs. `provenance.json` and `source-provenance.json` record the UCSC source URLs and retrieval details.

The article's SQL result tables are evaluated by the website generator. To explore a different dataset, change the import and rerun the queries. `render_charts.py` reads the saved `measurements.json`, so update that file with your new SQL results before drawing charts for another dataset.

The [SeqKit tutorial](https://bioinf.shenwei.me/seqkit/tutorial/), [FAQ](https://bioinf.shenwei.me/seqkit/faq/) and [usage guide](https://bioinf.shenwei.me/seqkit/usage/) explain the original command-line workflows. See `ATTRIBUTION.txt` for the documentation attribution and MIT notice.

To remove this exercise's database objects when finished:

```sql
DROP SCHEMA seqkit_lab CASCADE;
```
