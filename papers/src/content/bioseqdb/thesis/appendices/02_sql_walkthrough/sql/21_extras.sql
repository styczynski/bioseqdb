SELECT id,
  bioseq.symbol_count(seq, 'A') AS adenines,
  (seq[:8] OPERATOR(bioseq.||) seq[-8:])
    ::text AS joined_ends,
  seq[0]::text AS first_letter,
  bioseq.reverse(seq[:8])::text AS reversed,
  bioseq.reverse_complement(seq[:8])::text
    AS reverse_complement,
  bioseq.reverse(bioseq.reverse_complement(
    seq[:8]))::text AS complemented,
  bioseq.edit(seq) AS edits,
  bioseq.cigar(seq) AS cigar
FROM samples
WHERE age >= 30
  AND mmsearch(seq,
    :'probe'::bioseq.seq('dna4', 64))
  AND bioseq.edit(seq) < 5
ORDER BY id;
