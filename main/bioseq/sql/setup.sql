-- Drop the extension and recreate it so the updated bioseq--1.0.sql is
-- loaded. Re-creation re-populates pg_proc with fresh OIDs; the custom
-- scan never caches OIDs across queries (see SPEC §7.4.1).
drop extension if exists bioseq cascade;
create extension bioseq;

drop table if exists x CASCADE;

SET bioseq.logging = on;
SET bioseq.enable_bioseq_index_scan TO TRUE;
set bioseq.enable_column_cache to on;

-- POC table: INT4 primary key on `a` so the batch processor's map<int32_t, ...>
-- can key on it. `seq TEXT` is the target column for mmsearch; `c INT` is a
-- regular projected column unrelated to alignment.
create table x ( a INT PRIMARY KEY, seq TEXT, c INT ) using bioseq;

INSERT INTO x VALUES (
    23,  'sequence_1', 1
), (
    101, 'sequence_2', 1
), (
    69,  'sequence_3', 2
), (
    42,  'sequence_4', 2
);

-- ---- Case 1: vectorization OFF (baseline) ----
set bioseq.enable_vectorization to off;
EXPLAIN (COSTS OFF)
  SELECT a, c, bioseq.cigar(a), bioseq.query_start(a)
  FROM x
  WHERE mmsearch(seq, 'sequence_b');

SELECT a, c, bioseq.cigar(a), bioseq.query_start(a)
  FROM x
  WHERE mmsearch(seq, 'sequence_b');

-- ---- Case 2: vectorization ON; result must match Case 1 ----
set bioseq.enable_vectorization to on;
EXPLAIN (COSTS OFF)
  SELECT a, c, bioseq.cigar(a), bioseq.query_start(a)
  FROM x
  WHERE mmsearch(seq, 'sequence_b');

SELECT a, c, bioseq.cigar(a), bioseq.query_start(a)
  FROM x
  WHERE mmsearch(seq, 'sequence_b');
