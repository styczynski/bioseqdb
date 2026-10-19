SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.aligner.vulkan.enabled = off;
CREATE SCHEMA seqkit_lab;
CREATE TABLE seqkit_lab.snapshot (
    header text, seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
SELECT bioseq.load('seqkit_lab.snapshot', 'seqkit_ucsc_chrM.fasta');
CREATE TABLE seqkit_lab.genomes (
    assembly text NOT NULL,
    species text NOT NULL,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
INSERT INTO seqkit_lab.genomes
SELECT metadata.assembly, metadata.species, snapshot.seq
FROM seqkit_lab.snapshot AS snapshot
JOIN (VALUES ('hg38', 'human'), ('mm39', 'mouse'),
             ('danRer11', 'zebrafish')) AS metadata(assembly, species)
  ON snapshot.header = metadata.assembly;
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
