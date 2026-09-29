---
title: BioseqDB README examples
description: SQL examples from README.md.
keywords: [BioseqDB, PostgreSQL, SQL]
slots:
  hero_query: landing_hero_query
  sequence_storage: landing_sequence_storage
  similarity_search: landing_similarity_search
---

```sql runnable id=landing_hero_query timeout=60s
-- bioseq:setup
SET bioseq.index.insert_maintenance.enabled = off;
SET max_parallel_workers_per_gather = 0;
SET bioseq.aligner.vulkan.enabled = off;
CREATE TABLE samples (
  id integer PRIMARY KEY,
  upload_date date NOT NULL,
  patient_name text NOT NULL,
  age integer NOT NULL,
  seq bioseq.seq('dna4', 64) NOT NULL
) USING bioseq;
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
-- bioseq:visible
SELECT seq::text AS probe
FROM samples WHERE id = 2 ;

SELECT id, patient_name,
  bioseq.target_start(seq) AS target_start,
  bioseq.target_end(seq) AS target_end,
  bioseq.score(seq) AS score,
  bioseq.cigar(seq) AS cigar
FROM samples
WHERE age >= 30
  AND mmsearch(seq,
    (SELECT seq FROM samples WHERE id = 2)::bioseq.seq('dna4', 64));
-- bioseq:teardown
DROP TABLE samples;
```

```sql runnable id=landing_sequence_storage timeout=60s
-- bioseq:setup
SET bioseq.index.insert_maintenance.enabled = off;
SET max_parallel_workers_per_gather = 0;
SET bioseq.aligner.vulkan.enabled = off;
CREATE TABLE samples (
  id integer PRIMARY KEY,
  upload_date date NOT NULL,
  patient_name text NOT NULL,
  age integer NOT NULL,
  seq bioseq.seq('dna4', 64) NOT NULL
) USING bioseq;
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
SELECT bioseq.update_index('samples', 'minimizers_per_fragment');
-- bioseq:visible
CREATE TABLE imported (header text, seq bioseq.seq('dna4', 64) NOT NULL) USING bioseq;
SELECT bioseq.load('imported', 'readme_samples.fasta');
-- bioseq:teardown
DROP TABLE imported;
DROP TABLE samples;
```

```sql runnable id=landing_similarity_search timeout=60s
-- bioseq:setup
SET bioseq.index.insert_maintenance.enabled = off;
SET max_parallel_workers_per_gather = 0;
SET bioseq.aligner.vulkan.enabled = off;
CREATE TABLE samples (
  id integer PRIMARY KEY,
  upload_date date NOT NULL,
  patient_name text NOT NULL,
  age integer NOT NULL,
  seq bioseq.seq('dna4', 64) NOT NULL
) USING bioseq;
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
SELECT bioseq.update_index('samples', 'minimizers_per_fragment');
-- bioseq:visible
SELECT t.id AS target_id, q.id AS query_id,
  bioseq.target_start(t.seq) AS target_start,
  bioseq.target_end(t.seq) AS target_end,
  bioseq.query_start(t.seq) AS query_start,
  bioseq.query_end(t.seq) AS query_end,
  bioseq.score(t.seq) AS score,
  bioseq.cigar(t.seq) AS cigar
FROM samples AS t JOIN samples AS q
  ON mmsearch(t.seq, q.seq, aligner_options(index_read_requirement => 2))
ORDER BY target_id, query_id, target_start,
  query_start;
-- bioseq:teardown
DROP TABLE samples;
```

