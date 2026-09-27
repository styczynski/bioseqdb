SELECT header, bioseq.len(seq) AS bases,
  seq[:16]::text AS prefix
FROM imported ORDER BY header;

SELECT seq[128:384]::text AS short_probe
FROM imported
WHERE header LIKE 'sample_2 %' \gset

SELECT header,
  bioseq.target_start(seq) AS target_start,
  bioseq.target_end(seq) AS target_end,
  bioseq.cigar(seq) AS cigar
FROM imported
WHERE mmsearch(seq,
  :'short_probe'::bioseq.seq('dna4', 64))
ORDER BY header, target_start;
