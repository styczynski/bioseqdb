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

This file is the runnable-example source for the marketing landing page (§15.2).
It is parsed into the private build plan, executed against the installed Bioseq
extension, and its captured result rows are rendered into the landing template's
named slots. It is never emitted as a page or search result.

Each block declares its hidden `setup`/`teardown` and its `visible` query with the
`-- bioseq:setup` / `-- bioseq:visible` / `-- bioseq:teardown` markers, exactly as
tutorial runnable examples do.

## Hero query

The hero SELECT shows how a single `mmsearch()` predicate turns an ordinary query
into a sequence search that still projects normal metadata columns.

```sql runnable id=landing_hero_query timeout=10s
-- bioseq:setup
SET bioseq.update_index_on_insert = on;
CREATE TABLE landing_hero_sequences (
  id INT PRIMARY KEY,
  accession TEXT NOT NULL,
  seq bioseq.seq('dna4', 64)
) USING bioseq;
INSERT INTO landing_hero_sequences VALUES
  (1042, 'NR_046018.2', 'CTAGTCACTGTAGCCTGATCGTAGCTAGCACG'::bioseq.seq('dna4', 64)),
  (2087, 'NC_000913.3', 'ACGTACGTACGTACGTACGTACGTACGTACGT'::bioseq.seq('dna4', 64)),
  (3765, 'CP041828.1',  'GGGGCCCCTTTTAAAAGGGGCCCCTTTTAAAA'::bioseq.seq('dna4', 64));
-- bioseq:visible
SELECT id, accession
FROM landing_hero_sequences
WHERE mmsearch(seq, 'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 64))
ORDER BY id;
-- bioseq:teardown
DROP TABLE landing_hero_sequences;
```

## Sequence storage

A `bioseq.seq` column records its alphabet and maximum length in the type modifier,
so sequences are stored beside their metadata and read back with the ordinary
sequence functions [[sql:bioseq.len]] and
[[sql:bioseq.symbol_count]].

```sql runnable id=landing_sequence_storage timeout=10s
-- bioseq:setup
CREATE TABLE landing_reads (
  id INT PRIMARY KEY,
  label TEXT NOT NULL,
  sequence bioseq.seq('dna4', 32)
);
INSERT INTO landing_reads VALUES
  (1, 'read-a', 'ACGTACGTACGTACGT'),
  (2, 'read-b', 'TTTTAAAAGGGGCCCC'),
  (3, 'read-c', 'AAAAAAAAAAAAAAAA');
-- bioseq:visible
SELECT id,
       label,
       sequence::text AS sequence,
       bioseq.len(sequence) AS length,
       bioseq.symbol_count(sequence, 'A') AS a_count
FROM landing_reads
ORDER BY id;
-- bioseq:teardown
DROP TABLE landing_reads;
```

## Similarity search

The similarity-search slot is a real [[sql:public.mmsearch]] example over a
`USING bioseq` columnar table. The hidden setup asserts, with an
`EXPLAIN (FORMAT JSON)` check inside a `DO` block, that the exact visible predicate
plans as the `BioseqScan` custom scan, so a planner regression cannot silently
publish the always-true stub result. The three-row fixture and the query/plan
contract are adapted from the integration suite.

```sql runnable id=landing_similarity_search timeout=10s
-- bioseq:setup
SET bioseq.update_index_on_insert = on;
SET bioseq.enable_vectorization = on;
CREATE TABLE landing_similarity (
  pk INT PRIMARY KEY,
  seq0 bioseq.seq('dna4', 32)
) USING bioseq;
INSERT INTO landing_similarity VALUES
  (1, 'CTAGTCACTGTAGCCTGATCGTAGCTAGC'::bioseq.seq('dna4', 32)),
  (2, 'ACGTACGTACGTACGTACGTACGTACGTACGT'::bioseq.seq('dna4', 32)),
  (3, 'TGCATGCATGCATGCATGCATGCATGCATGCA'::bioseq.seq('dna4', 32));
DO $$
DECLARE
  plan_json jsonb;
BEGIN
  EXECUTE 'EXPLAIN (FORMAT JSON) SELECT pk, seq0 FROM landing_similarity WHERE mmsearch(seq0, ''CTAGTCACTGTAGCCTGATCGTAGCTAGT''::bioseq.seq(''dna4'', 32))'
    INTO plan_json;
  IF plan_json #>> '{0,Plan,"Node Type"}' IS DISTINCT FROM 'Custom Scan'
     OR plan_json #>> '{0,Plan,"Custom Plan Provider"}' IS DISTINCT FROM 'BioseqScan' THEN
    RAISE EXCEPTION 'landing similarity_search: expected BioseqScan custom scan, got %', plan_json;
  END IF;
END $$;
-- bioseq:visible
SELECT pk,
       seq0::text AS sequence
FROM landing_similarity
WHERE mmsearch(seq0, 'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32))
ORDER BY pk;
-- bioseq:teardown
DROP TABLE landing_similarity;
```
