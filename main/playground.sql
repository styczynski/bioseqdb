drop table if exists samples CASCADE;
create table samples (
    a    INT PRIMARY KEY,
    seq  bioseq.seq('dna4', 32),
    seq2 bioseq.seq('dna4', 32)
) USING bioseq;

CREATE INDEX samples_index ON x USING btree(c);

create table normal (
  a    INT PRIMARY KEY,
  seq TEXT
);

INSERT INTO samples VALUES
(1, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'::bioseq.seq('dna4', 32), 'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)),
(2, 'CCCCAACTTAGTCACTTCTCAAAAATTACTCTCTCATCATCATCTACTCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACACACACCCCAAA'::bioseq.seq('dna4', 32), 'TCGTAGCTAGGA'::bioseq.seq('dna4', 32)),
(3, 'GGGGGTCACTTCTCAAAAATTAGGGGGGGGGTAGTCACTGTAGCCTGATCGTAGCTAGGACACATATCATCATCTACTCCTATCATCATCTACTCCTA'::bioseq.seq('dna4', 32), 'TCATCATCTACTCCTA'::bioseq.seq('dna4', 32)),
(4, 'TTTCATCATCATCATCTACTCCTATCTATCATCATCATCATCTACTCCTATCTACTCTCATCATCCTGATCGTAGCTATACTCCTACTATCCTATT'::bioseq.seq('dna4', 32), 'TGTAGCCTGATCGTA'::bioseq.seq('dna4', 32));

INSERT INTO normal VALUES
(1, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'),
(2, 'CCCCAACTTAGTCACTTCTCAAAAATTACTCTCTCATCATCATCTACTCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACACACACCCCAAA'),
(3, 'GGGGGTCACTTCTCAAAAATTAGGGGGGGGGTAGTCACTGTAGCCTGATCGTAGCTAGGACACATATCATCATCTACTCCTATCATCATCTACTCCTA'),
(4, 'TTTCATCATCATCATCTACTCCTATCTATCATCATCATCATCTACTCCTATCTACTCTCATCATCCTGATCGTAGCTATACTCCTACTATCCTATT');


EXPLAIN (ANALYZE, VERBOSE, COSTS, BUFFERS) SELECT table1.a, bioseq.cigar(table1.a), bioseq.target_start(a), bioseq.target_end(a) FROM samples AS table1 WHERE mmsearch(table1.seq, 'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32));

EXPLAIN (ANALYZE, VERBOSE, COSTS, BUFFERS) SELECT table1.a, table2.a FROM samples AS table1 CROSS JOIN samples AS table2;
EXPLAIN (ANALYZE, VERBOSE, COSTS, BUFFERS)  SELECT table1.a, table2.a FROM normal AS table1 CROSS JOIN normal AS table2;

SELECT table1.a, table2.a, bioseq.cigar(table1.a) FROM samples AS table1 LEFT JOIN samples AS table2 ON table1.a = table2.a WHERE mmsearch(table1.seq, table2.seq);

EXPLAIN (ANALYZE, VERBOSE, COSTS, BUFFERS) SELECT table1.a, table2.a, bioseq.cigar(table1.a) FROM samples AS table1 LEFT JOIN samples AS table2 ON table1.a = table2.a WHERE mmsearch(table1.seq, table2.seq);
EXPLAIN (ANALYZE, VERBOSE, COSTS, BUFFERS)  SELECT table1.a, table2.a FROM samples AS table1 CROSS JOIN samples AS table2;

EXPLAIN (ANALYZE, VERBOSE, COSTS, BUFFERS)  SELECT table1.a, table2.a, bioseq.cigar(table1.a) FROM samples AS table1 LEFT JOIN samples AS table2 ON table1.a = table2.a WHERE mmsearch(table1.seq, 'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32));


-- cross join match

SELECT table1.a, table2.a, bioseq.cigar(table1.a) FROM samples AS table1 CROSS JOIN samples AS table2 WHERE mmsearch(table1.seq, table2.seq);
EXPLAIN (ANALYZE, VERBOSE, COSTS, BUFFERS) SELECT table1.a, table2.a, bioseq.cigar(table1.a) FROM samples AS table1 CROSS JOIN samples AS table2 WHERE mmsearch(table1.seq, table2.seq);
-- SELECT table1.a, table2.a, bioseq.target_start(table1.a), bioseq.target_end(table1.a), bioseq.cigar(table1.a) FROM samples AS table1 CROSS JOIN samples AS table2 WHERE mmsearch(table1.seq, table2.seq);

SELECT MIN(bioseq.target_start(a)), MAX(bioseq.target_end(a)) FROM samples WHERE mmsearch(seq, 'A'::bioseq.seq('dna4', 32));

--SELECT table1.a, table2.a, bioseq.cigar(table1.a) FROM samples AS table1 LEFT JOIN samples AS table2 ON table1.a = table2.a WHERE mmsearch(table1.seq, table2.seq);

EXPLAIN (ANALYZE, VERBOSE, COSTS, BUFFERS) SELECT table1.a, bioseq.cigar(table1.a) FROM samples AS table1 WHERE mmsearch(table1.seq, table1.seq);
SELECT table1.a, bioseq.cigar(table1.a) FROM samples AS table1 WHERE mmsearch(table1.seq, table1.seq2);

SELECT table1.a, bioseq.cigar(table1.a) FROM samples AS table1 CROSS JOIN samples AS table2 WHERE mmsearch(table1.seq, table2.seq) AND table1.a = table2.a;

