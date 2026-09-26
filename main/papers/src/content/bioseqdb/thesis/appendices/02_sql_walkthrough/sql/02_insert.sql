WITH dna AS (
  SELECT string_agg(translate(md5(i::text),
    '0123456789abcdef',
    'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
  FROM generate_series(1, 64) AS g(i)
)
INSERT INTO samples
SELECT 1, DATE '2026-09-01', 'Anna', 34,
  (s || s)::bioseq.seq('dna4', 64) FROM dna
UNION ALL
SELECT 2, DATE '2026-09-02', 'Ben', 52,
  substring(s FROM 257 FOR 512)
    ::bioseq.seq('dna4', 64) FROM dna
UNION ALL
SELECT 3, DATE '2026-09-03', 'Cara', 29,
  s::bioseq.seq('dna4', 64) FROM dna;
