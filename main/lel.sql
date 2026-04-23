drop table if exists hoe CASCADE;
create table hoe (
    a    INT PRIMARY KEY,
    seq  bioseq.seq('dna4', 32),
    seq2 bioseq.seq('dna4', 32)
) USING bioseq;

INSERT INTO hoe VALUES
  (1, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'::bioseq.seq('dna4', 32), 'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)),
  (2, 'CCCCAACTTAGTCACTTCTCAAAAATTACTCTCTCATCATCATCTACTCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACACACACCCCAAA'::bioseq.seq('dna4', 32), 'TCGTAGCTAGGA'::bioseq.seq('dna4', 32)),
  (3, 'GGGGGTCACTTCTCAAAAATTAGGGGGGGGGTAGTCACTGTAGCCTGATCGTAGCTAGGACACATATCATCATCTACTCCTATCATCATCTACTCCTA'::bioseq.seq('dna4', 32), 'TCATCATCTACTCCTA'::bioseq.seq('dna4', 32)),
  (4, 'TTTCATCATCATCATCTACTCCTATCTATCATCATCATCATCTACTCCTATCTACTCTCATCATCCTGATCGTAGCTATACTCCTACTATCCTATT'::bioseq.seq('dna4', 32), 'TGTAGCCTGATCGTA'::bioseq.seq('dna4', 32)),
  (5, 'ACGTCTAGTCACTGTAGCCTGATCGTAGCTAGTGTACTCTAGTCACTGTAGCCTGATCGTAGCTAGTTGATCCCCATATCTTCACTAGTCACTGTAGCCTGATCGTAGCTAGTTTTTTTA'::bioseq.seq('dna4', 32), 'CACTGTAGCCTGATCGTA'::bioseq.seq('dna4', 32));

-- self-join query
SELECT table1.a, table2.a, bioseq.target_start(table1.a), bioseq.target_end(table1.a), bioseq.cigar(table1.a) FROM hoe AS table1 CROSS JOIN hoe AS table2 ON TRUE WHERE mmsearch(table1.seq, table2.seq2);

SELECT table1.a, table2.a FROM hoe AS table1 CROSS JOIN hoe AS table2;


select * from hoe;

 EXPLAIN (ANALYZE, VERBOSE, COSTS, BUFFERS) SELECT a, bioseq.symbol_count(seq, 'A'), bioseq.query_start(a), bioseq.query_end(a), bioseq.target_start(a), bioseq.target_end(a), bioseq.cigar(a)
  FROM hoe WHERE mmsearch(seq, 'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32));

-- SELECT a, bioseq.symbol_count(seq, 'A'), bioseq.query_start(a), bioseq.query_end(a), bioseq.target_start(a), bioseq.target_end(a), bioseq.cigar(a) FROM hoe WHERE mmsearch(seq, 'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32));

EXPLAIN (ANALYZE, VERBOSE, COSTS, BUFFERS) SELECT a, bioseq.symbol_count(seq, 'A') FROM hoe;

SET bioseq.update_index_on_insert = off;
SET bioseq.read_sequence_index = on;
INSERT INTO hoe VALUES
  (6, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGAACACACATATATATTTTTTTATATTATTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'::bioseq.seq('dna4', 32)),
  (7, 'TCACTGTAGGATCGTAGCTAGTCCGTATAGCTGAACACACATATATATTTGGGGTAGCTAGTCAGATGATCATCGCTACGTACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'::bioseq.seq('dna4', 32));


EXPLAIN (ANALYZE, VERBOSE, COSTS, BUFFERS) SELECT a, bioseq.symbol_count(seq, 'A'), bioseq.query_start(a), bioseq.query_end(a), bioseq.target_start(a), bioseq.target_end(a), bioseq.cigar(a), bioseq.divergence(a) FROM hoe WHERE mmsearch(seq, 'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32));


bioseq=#  EXPLAIN (ANALYZE, VERBOSE, COSTS, BUFFERS)  SELECT * FROM hoe CROSS JOIN hoe AS h2;
                                                             QUERY PLAN                                                             
------------------------------------------------------------------------------------------------------------------------------------
 Nested Loop  (cost=0.00..1.39 rows=25 width=136) (actual time=0.259..0.285 rows=5 loops=1)
   Output: hoe.a, hoe.seq, hoe.seq2, h2.a, h2.seq, h2.seq2
   Buffers: shared hit=36
   ->  Custom Scan (BioseqScan) on public.hoe  (cost=0.00..0.53 rows=5 width=68) (actual time=0.219..0.220 rows=1 loops=1)
         Output: hoe.a, hoe.seq, hoe.seq2
         Bioseq Projected Columns: a, seq, seq2
         Buffers: shared hit=17
   ->  Materialize  (cost=0.00..0.56 rows=5 width=68) (actual time=0.034..0.057 rows=5 loops=1)
         Output: h2.a, h2.seq, h2.seq2
         Buffers: shared hit=19
         ->  Custom Scan (BioseqScan) on public.hoe h2  (cost=0.00..0.53 rows=5 width=68) (actual time=0.028..0.049 rows=5 loops=1)
               Output: h2.a, h2.seq, h2.seq2
               Bioseq Projected Columns: a, seq, seq2
               Buffers: shared hit=19
 Planning:
   Buffers: shared hit=60
 Planning Time: 1.229 ms
 Execution Time: 0.356 ms
(18 rows)


