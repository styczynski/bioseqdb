-- Drop the extension and recreate it so the updated bioseq--1.0.sql is
-- loaded. Re-creation re-populates pg_proc with fresh OIDs; the custom
-- scan never caches OIDs across queries (see SPEC §7.4.1).
drop extension if exists bioseq cascade;
create extension bioseq;

drop table if exists x CASCADE;

SET bioseq.logging = on;
SET bioseq.enable_bioseq_index_scan TO TRUE;

-- POC table: no PRIMARY KEY / no index, so the planner cannot choose
-- an index scan over our CustomScan path.
create table x ( a INT, seq TEXT, c INT ) using bioseq;

INSERT INTO x VALUES (
    23,  'sequence_1', 1
), (
    101, 'sequence_2', 1
), (
    69,  'sequence_3', 2
), (
    42,  'sequence_4', 2
);

set bioseq.enable_column_cache to on;
set bioseq.enable_vectorization to off;

-- First EXPLAIN to verify that the custom scan (BioseqScan) is chosen.
EXPLAIN (COSTS OFF) select a, c, bioseq.score(a) from x where mmsearch(seq, 'sequence_b');

-- The goal query.
select a, c, bioseq.score(a) from x where mmsearch(seq, 'sequence_b');
