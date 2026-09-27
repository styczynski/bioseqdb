\set ON_ERROR_STOP on
\pset pager off
\set PROBE '''CTAGTCACTGTAGCCTGATCGTAGCTAGT''::bioseq.seq(''dna4'',32)'

CREATE OR REPLACE FUNCTION public.bioseq_seq_subscript_handler(internal)
    RETURNS internal
    AS '/tmp/bioseq_subscript_poc/bioseq_subscript_poc.so', 'bioseq_seq_subscript_handler'
    LANGUAGE C;
ALTER TYPE bioseq.seq SET (SUBSCRIPT = public.bioseq_seq_subscript_handler);

\echo '=== BASELINE: bare Var column -> custom scan pushdown, mmsearch actually filters ==='
SELECT pk FROM phase_c_hoe WHERE mmsearch(seq0, :PROBE) ORDER BY pk;

\echo '=== CONTROLLED FOOTGUN: seq0[1:] is semantically identical to seq0, but is a SubscriptingRef ==='
\echo '=== custom scan requires bare Var -> skips -> always-true stub -> ALL rows pass ==='
SELECT pk FROM phase_c_hoe WHERE mmsearch(seq0[1:], :PROBE) ORDER BY pk;

\echo '=== USER EXAMPLE: mmsearch(seq0[5:], probe) -- also silently unfiltered ==='
SELECT pk FROM phase_c_hoe WHERE mmsearch(seq0[5:], :PROBE) ORDER BY pk;

\echo '=== EXPLAIN VERBOSE of the footgun query: note the SubscriptingRef stays as a Filter, no mmsearch pushdown semantics ==='
EXPLAIN (VERBOSE, COSTS off) SELECT pk FROM phase_c_hoe WHERE mmsearch(seq0[1:], :PROBE);

-- cleanup (no RESET grammar -> catalog surgery), restore pristine state
BEGIN;
UPDATE pg_type SET typsubscript = 0 WHERE oid = 'bioseq.seq'::regtype;
DELETE FROM pg_depend
 WHERE classid='pg_type'::regclass AND objid='bioseq.seq'::regtype
   AND refclassid='pg_proc'::regclass
   AND refobjid='public.bioseq_seq_subscript_handler(internal)'::regprocedure::oid;
DROP FUNCTION public.bioseq_seq_subscript_handler(internal);
COMMIT;
\echo '=== restored: typsubscript should be - ==='
SELECT typsubscript::text FROM pg_type WHERE oid='bioseq.seq'::regtype;
