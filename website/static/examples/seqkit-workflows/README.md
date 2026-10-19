# Sequence workflows in BioseqDB and SeqKit

Code for https://bioseqdb.com/blog/seqkit-workflows-in-bioseqdb/.

Use PostgreSQL with BioseqDB installed and built with
`BIOSEQ_BUILD_EXTERNAL_API_CLIENTS=ON`. Published 0.1.0-alpha.1 binaries omit
that module; see the blog's availability warning and installation links.
The SQL role needs access to the `bioseq_external.sequences` table.

Run in a fresh database or one without a `seqkit_lab` schema:

```sh
psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -f 01_import.sql
psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -f 02_export.sql
psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -f 03_queries.sql
bash seqkit.sh
```

`01_import.sql` downloads UCSC chrM from hg38, mm39 and danRer11 through
BioseqDB SQL, then makes 51 non-overlapping windows. `02_export.sql` writes
`genomes.fa` and `windows.fa` in your current directory. These are the same
rows used by the SQL examples. No raw sequence data is included in this bundle.

Install SeqKit from https://bioinf.shenwei.me/seqkit/download/.
The examples were tested with v2.14.0. `SEQKIT=/path/to/seqkit bash seqkit.sh`
also works when the executable is outside your PATH.

The shell script writes derived FASTA fixtures and outputs under
`results/seqkit`. It replaces files in that tutorial directory on reruns.
The repeated-name, edited-base and joined-end fixtures are constructed teaching
examples, not observations about biological mutations or duplicated regions.

For chart reproduction:

```sh
python3 -m venv .venv
.venv/bin/pip install matplotlib==3.10.7
.venv/bin/python render_charts.py --output figures
```

`measurements.json` contains the values measured from the SQL import on
19 October 2026. `provenance.json` records API URLs and sequence hashes.
`seqkit-previews.json` holds measured terminal outputs used by the website.
`queries.json` is the canonical source for displayed SQL and `03_queries.sql`.
The website generator executes the SQL independently for the displayed tables.

The figures have transparent backgrounds and a light palette selected with
`prefers-color-scheme`. Their window coordinates are local to each assembly,
not alignment coordinates. GC excludes N; chromosome lengths include N.

To remove only this exercise's database objects when finished:

```sql
DROP SCHEMA seqkit_lab CASCADE;
```

See ATTRIBUTION.txt for upstream documentation sources and its MIT notice.
