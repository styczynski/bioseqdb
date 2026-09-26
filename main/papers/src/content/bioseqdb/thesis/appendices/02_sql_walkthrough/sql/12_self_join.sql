SELECT t.id AS target_id, q.id AS query_id,
  bioseq.target_start(t.seq) AS target_start,
  bioseq.target_end(t.seq) AS target_end,
  bioseq.query_start(t.seq) AS query_start,
  bioseq.query_end(t.seq) AS query_end,
  bioseq.score(t.seq) AS score,
  bioseq.cigar(t.seq) AS cigar
FROM samples AS t JOIN samples AS q
  ON mmsearch(t.seq, q.seq)
ORDER BY target_id, query_id, target_start,
  query_start;
