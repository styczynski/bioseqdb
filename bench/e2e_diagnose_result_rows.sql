-- Read-only investigation of a failed retained q1024 fixture, never a timing
-- sample. Run only after its MCP job is terminal and the host queue is held.
-- psql variables: target, queries, seed, chain, align, k, w, lookup_route.
-- Preserve all returned matches: classify extras rather than hiding them.
\set ON_ERROR_STOP on
BEGIN READ ONLY;
SET LOCAL statement_timeout = '15min';
SET LOCAL lock_timeout = '5s';
SET LOCAL max_parallel_workers_per_gather = 0;
SET LOCAL bioseq.planner.custom_scan.enabled = on;
SET LOCAL bioseq.planner.index_scan.enabled = on;
SET LOCAL bioseq.vectorization.enabled = on;
SET LOCAL bioseq.scheduler.pipeline.mode = 'task_parallel';
SET LOCAL bioseq.aligner.minimizer.algorithm = :'seed';
SET LOCAL bioseq.aligner.chaining.algorithm = :'chain';
SET LOCAL bioseq.aligner.base_alignment.algorithm = :'align';
SET LOCAL bioseq.aligner.minimizer.kmer_size = :k;
SET LOCAL bioseq.aligner.minimizer.window_size = :w;
SET LOCAL bioseq.aligner.grouped_soa_generation.enabled = on;
SET LOCAL bioseq.aligner.vulkan.enabled = on;
SET LOCAL bioseq.aligner.vulkan.seed_lookup.enabled = off;
SET LOCAL bioseq.aligner.vulkan.index_cache.enabled = on;
SET LOCAL bioseq.aligner.vulkan.fused_frontend.enabled = off;
SET LOCAL bioseq.aligner.vulkan.alignment_extension.enabled = off;
SET LOCAL bioseq.aligner.vulkan.index_build.enabled = off;
SET LOCAL bioseq.aligner.fragment_minimizer.lookup_route = :'lookup_route';
SET LOCAL bioseq.column_cache.enabled = on;
SET LOCAL bioseq.index.async_persistence.enabled = off;
SET LOCAL bioseq.index.recompute_async_persistence.enabled = off;
SET LOCAL bioseq.index.insert_maintenance.enabled = off;
SET LOCAL bioseq.index.sequence_read.enabled = on;
SET LOCAL bioseq.index.sequence_minimizer_sort.algorithm = 'std_sort';

WITH matches AS MATERIALIZED (
    SELECT q.id, t.header,
           bioseq.target_start(header) AS tstart,
           bioseq.target_end(header) AS tend,
           bioseq.query_start(header) AS qstart,
           bioseq.query_end(header) AS qend,
           bioseq.is_reverse(header) AS rev,
           bioseq.has_cigar(header) AS has_cigar,
           bioseq.cigar(header) AS cigar,
           bioseq.mapq(header) AS mapq,
           bioseq.is_primary(header) AS is_primary
    FROM :"target" t JOIN :"queries" q
      ON mmsearch(t.seq, q.seq, aligner_options(index_read_requirement => 2))
    ORDER BY q.id, t.header
), counts AS (
    SELECT id, count(*) AS rows,
           count(*) FILTER (WHERE is_primary) AS primary_rows,
           count(*) FILTER (WHERE NOT is_primary) AS secondary_rows
    FROM matches GROUP BY id
), extras AS (
    SELECT * FROM counts WHERE rows <> 1 ORDER BY id
)
SELECT jsonb_build_object(
    'purpose', 'diagnostic_only_not_a_benchmark_result',
    'query_count', (SELECT count(*) FROM :"queries"),
    'query_ids_distinct', (SELECT count(DISTINCT id) FROM :"queries"),
    'result_rows', (SELECT count(*) FROM matches),
    'matched_query_ids', (SELECT count(*) FROM counts),
    'primary_rows', (SELECT count(*) FROM matches WHERE is_primary),
    'secondary_rows', (SELECT count(*) FROM matches WHERE NOT is_primary),
    'null_primary_flags', (SELECT count(*) FROM matches WHERE is_primary IS NULL),
    'queries_with_multiple_primaries',
        (SELECT count(*) FROM counts WHERE primary_rows > 1),
    'extra_query_counts', (SELECT jsonb_agg(to_jsonb(e)) FROM extras e),
    'extra_row_examples', (SELECT jsonb_agg(to_jsonb(e)) FROM (
        SELECT m.id, m.header, m.tstart, m.tend, m.qstart, m.qend,
               m.rev, m.has_cigar, left(m.cigar, 200) AS cigar_prefix,
               m.mapq, m.is_primary
        FROM matches m JOIN extras e USING (id)
        ORDER BY m.id, m.tstart, m.qstart LIMIT 24
    ) e)
);
ROLLBACK;
