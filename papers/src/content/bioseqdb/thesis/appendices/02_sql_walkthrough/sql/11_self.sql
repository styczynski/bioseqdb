SELECT id,
  bioseq.target_start(seq) AS target_start,
  bioseq.target_end(seq) AS target_end,
  bioseq.score(seq) AS score,
  bioseq.cigar(seq) AS cigar
FROM samples
WHERE mmsearch(seq, seq)
ORDER BY id;
