\set ON_ERROR_STOP off
\pset pager off

-- 1) Register the standalone handler (absolute .so path; no EXTENSION needed)
CREATE OR REPLACE FUNCTION public.bioseq_seq_subscript_handler(internal)
    RETURNS internal
    AS '/tmp/bioseq_subscript_poc/bioseq_subscript_poc.so', 'bioseq_seq_subscript_handler'
    LANGUAGE C;

-- 2) Attach native [] subscripting to the EXISTING bioseq.seq type
ALTER TYPE bioseq.seq SET (SUBSCRIPT = public.bioseq_seq_subscript_handler);

SELECT typname, typsubscript::regproc
FROM pg_type WHERE typname='seq' AND typnamespace='bioseq'::regnamespace;

\echo '--- element s[i] (1-based) : expect T at pos2, G at pos4 ---'
SELECT ('CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4',32))[2] AS pos2,
       ('CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4',32))[4] AS pos4;

\echo '--- range s[4:9] : expect GTCACT ---'
SELECT ('CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4',32))[4:9] AS slice_4_9;

\echo '--- prefix s[:5] : expect CTAGT ; suffix s[10:] : expect TAGCCTGATCGTAGCTAGT ---'
SELECT ('CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4',32))[:5]  AS prefix_5,
       ('CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4',32))[10:] AS suffix_10;

\echo '--- result type is bioseq.seq, and len() works on it ---'
SELECT pg_typeof( ('CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4',32))[4:9] ) AS rtype,
       bioseq.len( ('CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4',32))[4:9] ) AS rlen;

\echo '--- element out-of-range -> NULL (array semantics) ---'
SELECT ('ACGT'::bioseq.seq('dna4',32))[99] AS oob;

\echo '--- dna5 / width 8 : width derivation from header ; [2:4] of ACGTN -> CGT ---'
SELECT ('ACGTN'::bioseq.seq('dna5',8))[2:4] AS dna5_slice,
       pg_typeof(('ACGTN'::bioseq.seq('dna5',8))[2:4]) AS dna5_rtype;

\echo '--- chained: slice of a slice ; concat via re-encode of two element-slices ---'
SELECT ( ('CTAGTCACTGTAGCC'::bioseq.seq('dna4',32))[3:10] )[2:4] AS slice_of_slice;

\echo '--- column context: subscript a real table column ---'
CREATE TEMP TABLE _subs(pk int, s bioseq.seq('dna4',32));
INSERT INTO _subs VALUES (1,'AAAACCCCGGGGTTTT'), (2,'CTAGTCACTGTAGCCTGATCG');
SELECT pk, s AS full, s[1:4] AS first4, s[5:] AS from5, s[3] AS third
FROM _subs ORDER BY pk;

\echo '--- cleanup: detach + drop handler so live extension is left pristine ---'
ALTER TYPE bioseq.seq RESET (SUBSCRIPT);
DROP FUNCTION public.bioseq_seq_subscript_handler(internal);
SELECT typname, typsubscript::text FROM pg_type WHERE typname='seq' AND typnamespace='bioseq'::regnamespace;
