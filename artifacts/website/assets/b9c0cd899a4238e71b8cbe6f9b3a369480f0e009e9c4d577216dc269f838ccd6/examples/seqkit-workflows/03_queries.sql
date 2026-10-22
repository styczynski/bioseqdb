\set ON_ERROR_STOP on

-- inspect
SELECT id, assembly, start0, end0, bioseq.len(seq) AS bp,
       seq[:12]::text AS prefix
FROM seqkit_lab.windows
ORDER BY id
LIMIT 6;

-- stats
SELECT count(*) AS sequences, sum(bioseq.len(seq)) AS total_bp,
       min(bioseq.len(seq)) AS min_bp,
       round(avg(bioseq.len(seq)), 1) AS mean_bp,
       max(bioseq.len(seq)) AS max_bp
FROM seqkit_lab.windows;

-- species_stats
SELECT species, count(*) AS windows,
       sum(bioseq.len(seq)) AS total_bp,
       min(bioseq.len(seq)) AS shortest_bp,
       max(bioseq.len(seq)) AS longest_bp
FROM seqkit_lab.windows
GROUP BY species ORDER BY species;

-- short
SELECT id, bioseq.len(seq) AS bp
FROM seqkit_lab.windows
WHERE bioseq.len(seq) < 1000
ORDER BY id;

-- gc
SELECT species, bioseq.len(seq) AS bp,
       round(100.0 * (bioseq.symbol_count(seq, 'G') + bioseq.symbol_count(seq, 'C'))
             / nullif(bioseq.len(seq) - bioseq.symbol_count(seq, 'N'), 0), 2) AS gc_percent,
       bioseq.symbol_count(seq, 'N') AS unknown_bases
FROM seqkit_lab.genomes ORDER BY species;

-- gc_filter
SELECT id, round(100.0 * (bioseq.symbol_count(seq, 'G') + bioseq.symbol_count(seq, 'C'))
             / nullif(bioseq.len(seq) - bioseq.symbol_count(seq, 'N'), 0), 2) AS gc_percent
FROM seqkit_lab.windows
WHERE 100.0 * (bioseq.symbol_count(seq, 'G') + bioseq.symbol_count(seq, 'C'))
      / nullif(bioseq.len(seq) - bioseq.symbol_count(seq, 'N'), 0) >= 45
ORDER BY id;

-- grep
SELECT id
FROM seqkit_lab.windows
WHERE seq::text LIKE '%GAATTC%'
ORDER BY id;

-- grep_names
SELECT id, bioseq.len(seq) AS bp
FROM seqkit_lab.windows
WHERE species = 'human' AND start0 < 3000
ORDER BY id;

-- subseq
SELECT id, seq[9:20]::text AS bases_10_to_20,
       seq[-10:]::text AS last_10
FROM seqkit_lab.windows
WHERE id IN ('human_01', 'mouse_01', 'zebrafish_01')
ORDER BY id;

-- reverse
SELECT id, seq[:20]::text AS forward,
       bioseq.reverse_complement(seq[:20])::text AS reverse_complement
FROM seqkit_lab.windows
WHERE id IN ('human_01', 'mouse_01', 'zebrafish_01')
ORDER BY id;

-- base_counts
SELECT species,
       bioseq.symbol_count(seq, 'A') AS a,
       bioseq.symbol_count(seq, 'C') AS c,
       bioseq.symbol_count(seq, 'G') AS g,
       bioseq.symbol_count(seq, 'T') AS t,
       bioseq.symbol_count(seq, 'N') AS n
FROM seqkit_lab.genomes ORDER BY species;

-- by_header
SELECT species, count(*) AS records
FROM seqkit_lab.windows
GROUP BY species ORDER BY species;

-- range
SELECT id, bioseq.len(seq) AS bp
FROM seqkit_lab.windows
ORDER BY id
LIMIT 5 OFFSET 3;

-- sort
SELECT id, bioseq.len(seq) AS bp
FROM seqkit_lab.windows
ORDER BY bioseq.len(seq), id
LIMIT 3;

-- sample
SELECT id
FROM seqkit_lab.windows
ORDER BY md5('2026:' || id)
LIMIT 5;

-- locate
SELECT id, strpos(seq::text, 'GAATTC') - 1 AS first_start0
FROM seqkit_lab.windows
WHERE seq::text LIKE '%GAATTC%'
ORDER BY id;

-- mutate
SELECT id, seq[:20]::text AS before,
       overlay(seq[:20]::text placing 'T' from 5 for 1) AS after
FROM seqkit_lab.windows ORDER BY id LIMIT 3;

-- replace
SELECT id, 'mito_' || id AS renamed
FROM seqkit_lab.windows ORDER BY id LIMIT 3;

-- concat
WITH left_part AS (
  SELECT id, seq[:10] AS seq
  FROM seqkit_lab.windows ORDER BY id LIMIT 3
), right_part AS (
  SELECT id, seq[-10:] AS seq
  FROM seqkit_lab.windows ORDER BY id LIMIT 3
)
SELECT l.id,
       (l.seq OPERATOR(bioseq.||) r.seq)::text AS joined
FROM left_part l JOIN right_part r USING (id)
ORDER BY l.id;

-- deduplicate
WITH original AS (
  SELECT seq[:20] AS seq FROM seqkit_lab.windows WHERE id = 'human_01'
), duplicated AS (
  SELECT 1 AS ordinal, 'copy' AS id, seq FROM original
  UNION ALL
  SELECT 2, 'copy', seq[:19] OPERATOR(bioseq.||) 'A'::bioseq.seq('dna5', 64) FROM original
  UNION ALL
  SELECT 3, 'other', seq FROM original
)
SELECT DISTINCT ON (seq) id, seq::text
FROM duplicated ORDER BY seq, ordinal;

-- rename
WITH original AS (
  SELECT seq[:20] AS seq FROM seqkit_lab.windows WHERE id = 'human_01'
), duplicated AS (
  SELECT 1 AS ordinal, 'copy' AS id, seq FROM original
  UNION ALL
  SELECT 2, 'copy', seq[:19] OPERATOR(bioseq.||) 'A'::bioseq.seq('dna5', 64) FROM original
  UNION ALL
  SELECT 3, 'other', seq FROM original
), numbered AS (
  SELECT ordinal, id,
         row_number() OVER (PARTITION BY id ORDER BY ordinal) AS copy
  FROM duplicated
)
SELECT CASE WHEN copy = 1 THEN id ELSE id || '_' || copy END AS unique_id
FROM numbered ORDER BY ordinal;

-- window_gc
SELECT species, id, start0, end0,
       round(100.0 * (bioseq.symbol_count(seq, 'G') + bioseq.symbol_count(seq, 'C'))
             / nullif(bioseq.len(seq) - bioseq.symbol_count(seq, 'N'), 0), 4) AS gc_percent
FROM seqkit_lab.windows ORDER BY species, start0;

-- ids
SELECT id, bioseq.len(seq) AS bp
FROM seqkit_lab.windows
WHERE id IN ('human_01', 'mouse_02')
ORDER BY id;

-- ambiguity
SELECT id, bioseq.symbol_count(seq, 'N') AS unknown_bases
FROM seqkit_lab.windows
WHERE bioseq.symbol_count(seq, 'N') > 0
ORDER BY id;

-- regex
SELECT id FROM seqkit_lab.windows
WHERE seq::text ~ 'TATA[AT]AA'
ORDER BY id;

-- prefix
SELECT id, seq[:12]::text AS first_12
FROM seqkit_lab.windows ORDER BY id LIMIT 3;

-- split
SELECT id, bioseq.len(seq) AS bp
FROM seqkit_lab.windows WHERE species = 'human'
ORDER BY id LIMIT 3;

-- exclude
SELECT count(*) AS retained
FROM seqkit_lab.windows
WHERE id NOT IN ('human_01', 'mouse_02');

-- shuffle
SELECT id FROM seqkit_lab.windows
ORDER BY md5('2026:' || id);

-- gc_windows
SELECT id,
       round(100.0 * (bioseq.symbol_count(seq, 'G') + bioseq.symbol_count(seq, 'C'))
             / nullif(bioseq.len(seq) - bioseq.symbol_count(seq, 'N'), 0), 2) AS gc_percent
FROM seqkit_lab.windows ORDER BY id LIMIT 6;