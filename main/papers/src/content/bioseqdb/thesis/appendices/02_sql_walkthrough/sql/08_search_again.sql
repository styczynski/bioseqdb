SELECT id, bioseq.cigar(seq) AS cigar
FROM samples
WHERE age >= 30
  AND mmsearch(seq,
    :'probe'::bioseq.seq('dna4', 64));
