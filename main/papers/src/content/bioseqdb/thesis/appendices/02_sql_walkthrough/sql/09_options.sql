SELECT id,
  bioseq.target_start(seq) AS target_start,
  bioseq.target_end(seq) AS target_end,
  bioseq.score(seq) AS score,
  bioseq.cigar(seq) AS cigar
FROM samples
WHERE age >= 30 AND mmsearch(seq,
  :'probe'::bioseq.seq('dna4', 64),
  aligner_options(
    kmer_size => 15, window_size => 10,
    minimizer_algorithm => 4,
    chaining_algorithm => 1,
    base_alignment_algorithm => 1));
