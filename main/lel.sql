drop table if exists hoe CASCADE;
create table hoe (
    a    INT PRIMARY KEY,
    seq  bioseq.seq('dna4', 32)
) USING bioseq;

INSERT INTO hoe VALUES
  (1, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'::bioseq.seq('dna4', 32)),
  (2, 'CCCCAACTTAGTCACTTCTCAAAAATTACTCTCTCATCATCATCTACTCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACACACACCCCAAA'::bioseq.seq('dna4', 32)),
  (3, 'GGGGGTCACTTCTCAAAAATTAGGGGGGGGGTAGTCACTGTAGCCTGATCGTAGCTAGGACACATATCATCATCTACTCCTATCATCATCTACTCCTA'::bioseq.seq('dna4', 32)),
  (4, 'TTTCATCATCATCATCTACTCCTATCTATCATCATCATCATCTACTCCTATCTACTCTCATCATCCTGATCGTAGCTATACTCCTACTATCCTATT'::bioseq.seq('dna4', 32)),
  (5, 'ACGTCTAGTCACTGTAGCCTGATCGTAGCTAGTGTACTCTAGTCACTGTAGCCTGATCGTAGCTAGTTGATCCCCATATCTTCACTAGTCACTGTAGCCTGATCGTAGCTAGTTTTTTTA'::bioseq.seq('dna4', 32));

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