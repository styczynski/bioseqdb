\set ON_ERROR_STOP on
CREATE EXTENSION IF NOT EXISTS bioseq;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;

INSERT INTO seqkit_lab.genomes (assembly, species, seq)
SELECT 'hg38', 'human', sequence::bioseq.seq('dna5', 64)
FROM bioseq_external.sequences
WHERE provider = 'ucsc' AND assembly_database = 'hg38'
  AND chromosome = 'chrM' AND start0 = 0 AND end0 = 16569;

INSERT INTO seqkit_lab.genomes (assembly, species, seq)
SELECT 'mm39', 'mouse', sequence::bioseq.seq('dna5', 64)
FROM bioseq_external.sequences
WHERE provider = 'ucsc' AND assembly_database = 'mm39'
  AND chromosome = 'chrM' AND start0 = 0 AND end0 = 16299;

INSERT INTO seqkit_lab.genomes (assembly, species, seq)
SELECT 'danRer11', 'zebrafish', sequence::bioseq.seq('dna5', 64)
FROM bioseq_external.sequences
WHERE provider = 'ucsc' AND assembly_database = 'danRer11'
  AND chromosome = 'chrM' AND start0 = 0 AND end0 = 16596;

CREATE TABLE seqkit_lab.windows (
    id text NOT NULL,
    species text NOT NULL,
    assembly text NOT NULL,
    start0 integer NOT NULL,
    end0 integer NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.windows
SELECT species || '_' || lpad((start0 / 1000 + 1)::text, 2, '0'),
       species, assembly, start0,
       least(start0 + 1000, bioseq.len(seq)),
       seq[start0:least(start0 + 1000, bioseq.len(seq))]
FROM seqkit_lab.genomes
CROSS JOIN LATERAL generate_series(0, bioseq.len(seq) - 1, 1000) AS p(start0);
