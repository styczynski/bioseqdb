\set ECHO none
\set QUIET 1
\pset format unaligned
\pset tuples_only on
\o /dev/null

SET bioseq.query_cache_optimisation_mode = 'reuse';
SET bioseq.query_cache_ttl = '60 min';

CREATE TABLE cache_provider_rows(
    row_id bigint PRIMARY KEY,
    base_id bigint NOT NULL,
    occurrence_no integer NOT NULL,
    nullable_value integer,
    payload text NOT NULL,
    capability_mask bigint NOT NULL,
    UNIQUE (base_id, occurrence_no)
);
INSERT INTO cache_provider_rows VALUES
    (1, 10, 0, NULL, 'null', 3),
    (2, 11, 0, 0,    'zero', 3),
    (3, 12, 0, 5,    'first match', 3),
    (4, 12, 1, 5,    'second match', 3),
    (5, 13, 0, 10,   'ten', 3),
    (6, 14, 0, 20,   'twenty', 3);

SELECT bioseq_cache_test.provider_init();
SELECT bioseq_cache_test.provider_use_relation('cache_provider_rows');
SELECT bioseq_cache_test.provider_reset();
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();

DO $$
DECLARE
    plan_line text;
    plan_text text := '';
BEGIN
    FOR plan_line IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF) '
        'SELECT row_id, base_id, occurrence_no, nullable_value, payload, capability_mask '
        'FROM cache_provider_rows WHERE nullable_value <= 10'
    LOOP
        plan_text := plan_text || plan_line || E'\n';
    END LOOP;
    IF position('BioseqCacheGenericTestProvider' in plan_text) = 0 THEN
        RAISE EXCEPTION 'generic provider CustomScan was not chosen: %', plan_text;
    END IF;
END
$$;

DO $$
DECLARE
    ids bigint[];
    occurrences integer[];
BEGIN
    SELECT array_agg(row_id ORDER BY row_id)
    INTO ids
    FROM cache_provider_rows
    WHERE nullable_value <= 10;
    IF ids <> ARRAY[2,3,4,5]::bigint[] THEN
        RAISE EXCEPTION 'nullable truth-table P1 mismatch: %', ids;
    END IF;

    SELECT array_agg(occurrence_no ORDER BY occurrence_no)
    INTO occurrences
    FROM cache_provider_rows
    WHERE base_id = 12 AND nullable_value >= 5;
    IF occurrences <> ARRAY[0,1]::integer[] THEN
        RAISE EXCEPTION 'one-to-many occurrence identity was lost: %', occurrences;
    END IF;
END
$$;

-- Build a P1 generation, then consume the overlapping P2 region.  P2 must
-- return overlap plus residual exactly once under SQL three-valued logic.
SELECT row_id, base_id, occurrence_no, nullable_value, payload, capability_mask
FROM cache_provider_rows
WHERE nullable_value <= 10
ORDER BY row_id;

DO $$
DECLARE
    ids bigint[];
BEGIN
    SELECT array_agg(row_id ORDER BY row_id)
    INTO ids
    FROM cache_provider_rows
    WHERE nullable_value >= 5;
    IF ids <> ARRAY[3,4,5,6]::bigint[] THEN
        RAISE EXCEPTION 'overlap/residual bag mismatch: %', ids;
    END IF;
END
$$;

-- Full-key equality must survive a forced digest collision across relations.
CREATE TABLE cache_provider_collision(LIKE cache_provider_rows INCLUDING ALL);
INSERT INTO cache_provider_collision VALUES
    (101, 110, 0, 5, 'other relation', 3);
SELECT bioseq_cache_test.set_forced_coverage_domain_digest(
    decode(repeat('42', 32), 'hex'));
SELECT bioseq_cache_test.provider_force_domain_collision(true);
SELECT bioseq_cache_test.provider_use_relation('cache_provider_collision');
DO $$
DECLARE
    ids bigint[];
BEGIN
    SELECT array_agg(row_id ORDER BY row_id)
    INTO ids
    FROM cache_provider_collision
    WHERE nullable_value >= 5;
    IF ids <> ARRAY[101]::bigint[] THEN
        RAISE EXCEPTION 'forced digest collision crossed domain keys: %', ids;
    END IF;
END
$$;
SELECT bioseq_cache_test.clear_forced_coverage_domain_digest();
SELECT bioseq_cache_test.provider_force_domain_collision(false);
SELECT bioseq_cache_test.provider_use_relation('cache_provider_rows');

-- Every malformed mapping is fail-open: the retained provider path must
-- still return the disabled-mode bag, including provider ERROR.
DO $$
DECLARE
    fault text;
    ids bigint[];
    faults constant text[] := ARRAY[
        'null_pointer_nonzero_count',
        'short_boundary_map',
        'long_boundary_map',
        'short_lineage_map',
        'long_lineage_map',
        'zero_consumer_attno',
        'zero_source_attno',
        'source_attno_out_of_range',
        'duplicate_consumer_attno',
        'missing_required_attno',
        'wrong_type',
        'wrong_typmod',
        'wrong_collation',
        'wrong_semantic_id',
        'same_typed_column_permutation',
        'illegal_repeated_source_attno',
        'bad_provider_private_version',
        'non_integer_persisted_map_node',
        'unknown_persisted_map_version',
        'decorated_boundary_schema_mismatch',
        'decorated_lineage_schema_mismatch',
        'provider_error'
    ];
BEGIN
    FOREACH fault IN ARRAY faults LOOP
        PERFORM bioseq_cache_test.provider_set_mapping_fault(fault);
        SELECT array_agg(row_id ORDER BY row_id)
        INTO ids
        FROM cache_provider_rows
        WHERE nullable_value >= 5;
        IF ids <> ARRAY[3,4,5,6]::bigint[] THEN
            RAISE EXCEPTION 'mapping fault % changed the result bag: %', fault, ids;
        END IF;
    END LOOP;
    PERFORM bioseq_cache_test.provider_set_mapping_fault('none');
END
$$;

-- Capability-superset rejection and plan/runtime digest mismatches also
-- expand to the complete residual without changing rows.
SELECT bioseq_cache_test.provider_set_capabilities(3);
SELECT bioseq_cache_test.provider_set_projection_order('reverse');
SELECT bioseq_cache_test.provider_set_digest_fault('runtime_mismatch');
DO $$
DECLARE
    ids bigint[];
BEGIN
    SELECT array_agg(row_id ORDER BY row_id)
    INTO ids
    FROM cache_provider_rows
    WHERE nullable_value >= 5;
    IF ids <> ARRAY[3,4,5,6]::bigint[] THEN
        RAISE EXCEPTION 'capability/digest fail-open mismatch: %', ids;
    END IF;
END
$$;
SELECT bioseq_cache_test.provider_set_digest_fault('none');
SELECT bioseq_cache_test.provider_set_projection_order('natural');
SELECT bioseq_cache_test.provider_set_capabilities(1);

DO $$
DECLARE
    stats jsonb := bioseq_cache_test.provider_stats();
BEGIN
    IF NOT (stats->>'registered')::boolean
       OR (stats->>'planner_bridge_calls')::bigint = 0
       OR (stats->>'executor_rows')::bigint = 0 THEN
        RAISE EXCEPTION 'generic provider callbacks did not execute: %', stats;
    END IF;
END
$$;

SELECT bioseq_cache_test.provider_shutdown();

\o
SELECT 'cache_reuse_provider: ok';
