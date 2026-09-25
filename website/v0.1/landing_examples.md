---
title: Bioseq landing examples
description: Executed examples embedded in the landing page.
keywords: [Bioseq, PostgreSQL, biological sequences]
slots:
  hero_query: landing_hero_query
  sequence_storage: landing_sequence_storage
  similarity_search: landing_similarity_search
---

# Bioseq landing examples

This file supplies the runnable examples for the landing page. The private site
build executes each example against the installed Bioseq extension and renders
the captured rows in the named landing-page slot. This file is not emitted as a
page or search result.

Each block declares its hidden `setup`/`teardown` and its `visible` query with the
`-- bioseq:setup` / `-- bioseq:visible` / `-- bioseq:teardown` markers, exactly as
tutorial runnable examples do.

## Hero query

The hero SELECT shows how a single `mmsearch()` predicate turns an ordinary query
into a sequence search that still projects normal metadata columns.

```sql runnable id=landing_hero_query timeout=10s
-- bioseq:setup
SET bioseq.index.insert_maintenance.enabled = off;
CREATE TABLE landing_hero_sequences (
  id INT PRIMARY KEY,
  accession TEXT NOT NULL,
  seq bioseq.seq('dna4', 64)
) USING bioseq;
INSERT INTO landing_hero_sequences VALUES
  (1042, 'NR_046018.2', 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'::bioseq.seq('dna4', 64)),
  (2087, 'NC_000913.3', 'ACGTACGTACGTACGTACGTACGTACGTACGT'::bioseq.seq('dna4', 64)),
  (3765, 'CP041828.1',  'GGGGCCCCTTTTAAAAGGGGCCCCTTTTAAAA'::bioseq.seq('dna4', 64));
-- bioseq:visible
SELECT id, accession
FROM landing_hero_sequences
WHERE public.mmsearch(seq, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'::bioseq.seq('dna4', 64))
ORDER BY id;
-- bioseq:teardown
DROP TABLE landing_hero_sequences;
```

## Load and index

This slot executes the supplied load/index story against a build-owned FASTA
fixture. It creates the same kind of `USING bioseq` table, leaves legacy insert-time
maintenance disabled, maps UniProt header fields into typed metadata columns with
[[sql:bioseq.load]], inserts one manual record, publishes the grouped index after
those statements commit, then searches the combined data.

```sql runnable id=landing_sequence_storage timeout=20s
-- bioseq:setup
-- bioseq:visible
CREATE TABLE run_reads (
  row_no        bigserial PRIMARY KEY,
  read_id       text NOT NULL,
  sample_id     text NOT NULL,
  run_name      name NOT NULL DEFAULT 'run_042',
  collected_on  date NOT NULL DEFAULT CURRENT_DATE,
  seq           bioseq.seq('dna4', 64)
) USING bioseq;

SET bioseq.index.insert_maintenance.enabled = off;

SELECT bioseq.load(
  'run_reads'::regclass,
  'landing_reads.fasta',
  'uni_prot',
  ARRAY[
    ('read_id', 'accession')::bioseq.column_mapping,
    ('sample_id', 'organism_name')::bioseq.column_mapping
  ]
);

INSERT INTO run_reads (read_id, sample_id, run_name, seq)
VALUES (
  'manual_read_001',
  'validation_batch',
  'run_042',
  'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'::bioseq.seq('dna4', 64)
);

SELECT bioseq.update_index('run_reads'::regclass, 'minimizers_per_fragment');

SELECT read_id,
       sample_id,
       bioseq.target_start(row_no) AS match_start,
       bioseq.target_end(row_no)   AS match_end
FROM run_reads
WHERE public.mmsearch(
        seq,
        'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'::bioseq.seq('dna4', 64)
      )
ORDER BY collected_on DESC, read_id;
-- bioseq:teardown
DROP TABLE run_reads;
```

## Filter and sort

This slot runs a metadata-filtered [[sql:public.mmsearch]] query over a
`USING bioseq` table. The visible SQL creates and fills a small table, then
combines organism, date, sequence-length, and sequence-match predicates. A
hidden `EXPLAIN` check confirms that the final predicate uses `BioseqScan`.

```sql runnable id=landing_similarity_search timeout=20s
-- bioseq:setup
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.vectorization.enabled = on;
DO $$
DECLARE
  plan_json jsonb;
BEGIN
  CREATE TABLE landing_filter_plan (
    id bigint PRIMARY KEY,
    accession text,
    organism text,
    collected_on date,
    quality_score double precision,
    seq bioseq.seq('dna4', 64)
  ) USING bioseq;
  EXECUTE 'EXPLAIN (FORMAT JSON) '
          'SELECT id FROM landing_filter_plan '
          'WHERE organism = ''Homo sapiens'' '
          'AND collected_on >= DATE ''2025-01-01'' '
          'AND bioseq.len(seq) >= 1000 '
          'AND public.mmsearch(seq, '
          '''TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA''::bioseq.seq(''dna4'', 64))'
    INTO plan_json;
  IF plan_json #>> '{0,Plan,"Node Type"}' IS DISTINCT FROM 'Custom Scan'
     OR plan_json #>> '{0,Plan,"Custom Plan Provider"}' IS DISTINCT FROM 'BioseqScan' THEN
    RAISE EXCEPTION 'landing filter/sort: expected BioseqScan custom scan, got %', plan_json;
  END IF;
  DROP TABLE landing_filter_plan;
END $$;
-- bioseq:visible
CREATE TABLE reference_reads (
  id                bigint PRIMARY KEY,
  accession         text,
  organism          text,
  collected_on      date,
  quality_score     double precision,
  seq               bioseq.seq('dna4', 64)
) USING bioseq;

INSERT INTO reference_reads VALUES
  (5521, 'NR_046018.2', 'Homo sapiens', DATE '2026-06-10', 99.5,
   (repeat('A', 600) || 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA' || repeat('C', 600))::bioseq.seq('dna4', 64)),
  (4410, 'NC_000913.3', 'Homo sapiens', DATE '2025-11-04', 97.0,
   (repeat('C', 520) || 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA' || repeat('G', 540))::bioseq.seq('dna4', 64)),
  (6193, 'CP041828.1', 'Homo sapiens', DATE '2025-02-18', 94.5,
   (repeat('G', 500) || 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA' || repeat('T', 700))::bioseq.seq('dna4', 64)),
  (7102, 'FILTERED.1', 'Mus musculus', DATE '2026-07-01', 100.0,
   (repeat('T', 600) || 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA' || repeat('A', 600))::bioseq.seq('dna4', 64));

SELECT id,
       accession,
       organism,
       bioseq.len(seq) AS bases
FROM reference_reads
WHERE organism = 'Homo sapiens'
  AND collected_on >= DATE '2025-01-01'
  AND bioseq.len(seq) >= 1000
  AND public.mmsearch(
        seq,
        'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'::bioseq.seq('dna4', 64)
      )
ORDER BY quality_score DESC, accession
LIMIT 25;
-- bioseq:teardown
DROP TABLE reference_reads;
```
