SELECT id, bioseq.score(seq) AS score,
  bioseq.cigar(seq) AS cigar
FROM samples
WHERE mmsearch(seq,
  :'probe'::bioseq.seq('dna4', 64),
  aligner_options(
    minimizer_algorithm => 0,
    chaining_algorithm => 0,
    base_alignment_algorithm => 0,
    index_read_requirement => 1))
ORDER BY id;
