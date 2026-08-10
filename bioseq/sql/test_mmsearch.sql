-- Test file: exercises the goal query from
-- RESEARCH/HOW_TO_CONNECT_ALIGNER_PROMPT_4.md for both vectorisation modes,
-- plus a plain SELECT * that exercises the no-mmsearch pass-through path.

drop extension if exists bioseq cascade;
create extension bioseq;

SET bioseq.logging = on;
SET bioseq.enable_bioseq_index_scan TO TRUE;
set bioseq.enable_column_cache to on;

drop table if exists cx CASCADE;
create table cx (
    a    INT PRIMARY KEY,
    seq  bioseq.seq('dna4', 32)
) USING bioseq;

INSERT INTO cx VALUES
  (1, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'),
  (2, 'CCCCAACTTAGTCACTTCTCAAAAATTACTCTCTCATCATCATCTACTCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACACACACCCCAAA'),
  (3, 'GGGGGTCACTTCTCAAAAATTAGGGGGGGGGTAGTCACTGTAGCCTGATCGTAGCTAGGACACATATCATCATCTACTCCTATCATCATCTACTCCTA'),
  (4, 'TTTCATCATCATCATCTACTCCTATCTATCATCATCATCATCTACTCCTATCTACTCTCATCATCCTGATCGTAGCTATACTCCTACTATCCTATT'),
  (5, 'ACGTCTAGTCACTGTAGCCTGATCGTAGCTAGTGTACTCTAGTCACTGTAGCCTGATCGTAGCTAGTTGATCCCCATATCTTCACTAGTCACTGTAGCCTGATCGTAGCTAGTTTTTTTA');

-- =========================== no mmsearch — must return all 5 rows ============================
set bioseq.enable_vectorization to off;
\echo '=== SELECT * FROM cx (no mmsearch, vectorization OFF) ==='
SELECT a, bioseq.len(seq) AS seq_len FROM cx ORDER BY a;

set bioseq.enable_vectorization to on;
\echo '=== SELECT * FROM cx (no mmsearch, vectorization ON) ==='
SELECT a, bioseq.len(seq) AS seq_len FROM cx ORDER BY a;

-- =========================== Vectorization OFF ===========================
set bioseq.enable_vectorization to off;
\echo '=== vectorization OFF ==='
SELECT a, bioseq.query_start(a), bioseq.query_end(a), bioseq.target_start(a), bioseq.target_end(a), bioseq.cigar(a)
  FROM cx WHERE mmsearch(seq, 'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32))
  ORDER BY a;

-- =========================== Vectorization ON ============================
set bioseq.enable_vectorization to on;
\echo '=== vectorization ON ==='
SELECT a, bioseq.query_start(a), bioseq.query_end(a), bioseq.target_start(a), bioseq.target_end(a), bioseq.cigar(a)
  FROM cx WHERE mmsearch(seq, 'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32))
  ORDER BY a;
