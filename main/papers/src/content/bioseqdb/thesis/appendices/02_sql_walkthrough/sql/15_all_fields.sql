SELECT t.id AS target_id, q.header,
  bioseq.score(t.seq) AS score,
  bioseq.dp_score(t.seq) AS dp_score,
  bioseq.dp_max_score(t.seq) AS dp_max_score,
  bioseq.chain_score(t.seq) AS chain_score,
  bioseq.secondary_chain_score(t.seq)
    AS secondary_chain_score,
  bioseq.query_len(t.seq) AS query_len,
  bioseq.target_len(t.seq) AS target_len,
  bioseq.query_start(t.seq) AS query_start,
  bioseq.query_end(t.seq) AS query_end,
  bioseq.target_start(t.seq) AS target_start,
  bioseq.target_end(t.seq) AS target_end,
  bioseq.num_matches(t.seq) AS num_matches,
  bioseq.block_length(t.seq) AS block_length,
  bioseq.edit(t.seq) AS edits,
  bioseq.mapq(t.seq) AS mapq,
  bioseq.seed_count(t.seq) AS seed_count,
  bioseq.is_reverse(t.seq) AS is_reverse,
  bioseq.is_primary(t.seq) AS is_primary,
  bioseq.has_cigar(t.seq) AS has_cigar,
  bioseq.cigar(t.seq) AS cigar,
  bioseq.divergence(t.seq) AS divergence
FROM samples AS t JOIN imported AS q
  ON mmsearch(t.seq, q.seq,
    aligner_options(index_read_requirement => 2))
ORDER BY score DESC, target_id, q.header,
  target_start, query_start;
