SELECT seq::text AS probe
FROM samples WHERE id = 2 \gset

SELECT id, patient_name,
  bioseq.target_start(seq) AS target_start,
  bioseq.target_end(seq) AS target_end,
  bioseq.score(seq) AS score,
  bioseq.cigar(seq) AS cigar
FROM samples
WHERE age >= 30
  AND mmsearch(seq,
    :'probe'::bioseq.seq('dna4', 64));
