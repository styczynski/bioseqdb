\set ECHO none
\set QUIET 1
\pset format unaligned
\pset tuples_only on
\o /dev/null

SET bioseq.query_cache_optimisation_mode = 'reuse';
SET bioseq.query_cache_ttl = '60 min';

CREATE TABLE cache_provider_binding_rows(
    row_id bigint PRIMARY KEY,
    base_id bigint NOT NULL,
    occurrence_no integer NOT NULL,
    nullable_value integer,
    payload text NOT NULL,
    capability_mask bigint NOT NULL,
    UNIQUE (base_id, occurrence_no)
);
INSERT INTO cache_provider_binding_rows VALUES
    (1, 10, 0, NULL, 'null', 3),
    (2, 11, 0, 5,    'five', 3),
    (3, 12, 0, 10,   'ten', 3);

SELECT bioseq_cache_test.provider_init();
SELECT bioseq_cache_test.provider_use_relation(
    'cache_provider_binding_rows');
SELECT bioseq_cache_test.provider_reset();
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache_test.request_janitor_sweep();

SELECT row_id
FROM cache_provider_binding_rows
WHERE nullable_value >= 5
ORDER BY row_id;

SELECT count(*) AS before_count
FROM bioseq_cache.entries()
WHERE kind = 'ReusableCoverage' AND state = 'COMPLETE'
\gset single_before_

SELECT b.row_id
FROM (VALUES (1)) AS prefix_row(marker)
CROSS JOIN ONLY cache_provider_binding_rows AS b
WHERE b.nullable_value >= 5
ORDER BY b.row_id;

SELECT count(*) AS after_count
FROM bioseq_cache.entries()
WHERE kind = 'ReusableCoverage' AND state = 'COMPLETE'
\gset single_after_

SELECT a.row_id, b.row_id
FROM ONLY cache_provider_binding_rows AS a
CROSS JOIN ONLY cache_provider_binding_rows AS b
WHERE a.nullable_value >= 5 AND b.nullable_value >= 5
ORDER BY a.row_id, b.row_id;

SELECT count(*) AS before_count
FROM bioseq_cache.entries()
WHERE kind = 'ReusableCoverage' AND state = 'COMPLETE'
\gset self_before_

SELECT a.row_id, b.row_id
FROM (VALUES (1)) AS prefix_row(marker)
CROSS JOIN ONLY cache_provider_binding_rows AS a
CROSS JOIN ONLY cache_provider_binding_rows AS b
WHERE a.nullable_value >= 5 AND b.nullable_value >= 5
ORDER BY a.row_id, b.row_id;

SELECT count(*) AS after_count
FROM bioseq_cache.entries()
WHERE kind = 'ReusableCoverage' AND state = 'COMPLETE'
\gset self_after_

\o
SELECT
    :single_before_before_count::bigint
        = :single_after_after_count::bigint AS shifted_single_stable,
    :self_before_before_count::bigint
        = :self_after_after_count::bigint AS shifted_self_join_stable,
    :self_before_before_count::bigint
        >= :single_after_after_count::bigint + 2
        AS self_join_occurrences_distinct;
\o /dev/null

\set descriptor_fault raw_producer_var
\ir ../../../bioseq_caching/test/regress/sql/cache_reuse_provider_descriptor_fault_case.sql
\set descriptor_fault foreign_mixed_var
\ir ../../../bioseq_caching/test/regress/sql/cache_reuse_provider_descriptor_fault_case.sql
\set descriptor_fault wrong_node_type
\ir ../../../bioseq_caching/test/regress/sql/cache_reuse_provider_descriptor_fault_case.sql
\set descriptor_fault malformed_syntax
\ir ../../../bioseq_caching/test/regress/sql/cache_reuse_provider_descriptor_fault_case.sql
\set descriptor_fault binding_signature_mismatch
\ir ../../../bioseq_caching/test/regress/sql/cache_reuse_provider_descriptor_fault_case.sql
\unset descriptor_fault

\o
SELECT bioseq_cache_test.set_coverage_descriptor_fault('none');
SELECT bioseq_cache_test.provider_shutdown();
DROP TABLE cache_provider_binding_rows;
