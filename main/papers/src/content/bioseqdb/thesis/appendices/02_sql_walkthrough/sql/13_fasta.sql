COPY (
  SELECT line FROM samples
  CROSS JOIN LATERAL (VALUES
    (0, '>sample_' || id || ' synthetic DNA'),
    (1, seq::text)
  ) AS fasta(part, line)
  ORDER BY id, part
) TO '/tmp/bioseq-walkthrough.fasta';

CREATE TABLE imported (
  header text,
  seq bioseq.seq('dna4', 64) NOT NULL
) USING bioseq;

SELECT bioseq.load('imported',
  '/tmp/bioseq-walkthrough.fasta');
