\set ON_ERROR_STOP on
\pset pager off
\echo '############ Pure-SQL (no C) slice/element/concat via seq_out/seq_in text roundtrip ############'

CREATE SCHEMA IF NOT EXISTS poc;

-- element -> text (1-based). Decodes whole seq (O(n)); a C handler would be O(1).
CREATE OR REPLACE FUNCTION poc.element(s bioseq.seq, i int) RETURNS text
  AS $$ SELECT substr(bioseq.seq_out(s)::text, i, 1) $$ LANGUAGE sql IMMUTABLE STRICT;

-- slice -> text (1-based inclusive; NULL bound = open end)
CREATE OR REPLACE FUNCTION poc.slice(s bioseq.seq, lo int, hi int) RETURNS text AS $$
  SELECT substr(bioseq.seq_out(s)::text,
                COALESCE(lo,1),
                CASE WHEN hi IS NULL THEN 2147483647 ELSE GREATEST(hi,0) - COALESCE(lo,1) + 1 END)
$$ LANGUAGE sql IMMUTABLE;

\echo '--- element/slice on the canonical probe (CTAGTCACTGTAGCCTGATCGTAGCTAGT) ---'
SELECT poc.element('CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4',32), 2)         AS elem2,
       poc.slice  ('CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4',32), 4, 9)       AS slice_4_9,
       poc.slice  ('CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4',32), NULL, 5)     AS prefix_5,
       poc.slice  ('CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4',32), 10, NULL)    AS suffix_10;

\echo '--- slice re-encoded back to a real bioseq.seq (text -> seq) roundtrips losslessly ---'
SELECT (poc.slice('CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4',32), 4, 9))::bioseq.seq('dna4',32) AS reencoded;

\echo '############ Concat operator || (materialising; re-encodes at dna4/32 for the demo) ############'
CREATE OR REPLACE FUNCTION poc.concat(a bioseq.seq, b bioseq.seq) RETURNS bioseq.seq AS $$
  SELECT (bioseq.seq_out(a)::text || bioseq.seq_out(b)::text)::bioseq.seq('dna4',32)
$$ LANGUAGE sql IMMUTABLE STRICT;
DROP OPERATOR IF EXISTS || (bioseq.seq, bioseq.seq);
CREATE OPERATOR || (LEFTARG=bioseq.seq, RIGHTARG=bioseq.seq, FUNCTION=poc.concat);

\echo '--- the user "sum of slices" example: q1[4:5] || q2[5:8] (1-based inclusive) ---'
\echo '--- q1=AAAACCCC -> [4:5]=AC ; q2=GGGGTTTT -> [5:8]=TTTT ; concat=ACTTTT ---'
SELECT ( (poc.slice('AAAACCCC'::bioseq.seq('dna4',32),4,5))::bioseq.seq('dna4',32)
         || (poc.slice('GGGGTTTT'::bioseq.seq('dna4',32),5,8))::bioseq.seq('dna4',32) ) AS rope;

\echo '--- concat is associative and returns a real seq usable by bioseq.len / mmsearch ---'
SELECT bioseq.len( 'ACGT'::bioseq.seq('dna4',32) || 'TTTTGGGG'::bioseq.seq('dna4',32) ) AS concat_len;

\echo '############ cleanup ############'
DROP OPERATOR IF EXISTS || (bioseq.seq, bioseq.seq);
DROP SCHEMA poc CASCADE;
\echo 'poc schema dropped'
