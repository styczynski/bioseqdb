#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

namespace {

constexpr std::string_view provider_query{
    "SELECT row_id, base_id, occurrence_no, nullable_value, payload, "
    "capability_mask "
    "FROM cache_provider_rows "
    "WHERE nullable_value <= 10 "
    "ORDER BY row_id"};

constexpr std::string_view consumer_query{
    "SELECT row_id, base_id, occurrence_no, nullable_value, payload, "
    "capability_mask "
    "FROM cache_provider_rows "
    "WHERE nullable_value >= 5 "
    "ORDER BY row_id"};

constexpr std::string_view security_query{
    "SELECT row_id, base_id, occurrence_no, nullable_value, payload, "
    "capability_mask "
    "FROM cache_security_rows "
    "WHERE nullable_value <= 10 "
    "ORDER BY row_id"};

void create_standard_provider_fixture(
        bioseq::libpq::connection& connection) {
    bioseq::cache_test::exec(
        connection,
        R"SQL(
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
            SELECT bioseq_cache_test.provider_use_relation(
                'cache_provider_rows');
            SELECT bioseq_cache_test.provider_reset();
            SELECT bioseq_cache.invalidate_all();
            SELECT bioseq_cache_test.request_janitor_sweep();
            SELECT bioseq_cache.reset_stats();
        )SQL");
}

void create_descriptor_provider_fixture(
        bioseq::libpq::connection& connection) {
    bioseq::cache_test::exec(
        connection,
        R"SQL(
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
            SELECT bioseq_cache.reset_stats();
        )SQL");
}

void create_security_provider_fixture(
        bioseq::libpq::connection& connection) {
    bioseq::cache_test::exec(
        connection,
        R"SQL(
            SET bioseq.query_cache_optimisation_mode = 'reuse';
            SET bioseq.query_cache_ttl = '60 min';
            CREATE TABLE cache_security_rows(
                row_id bigint PRIMARY KEY,
                base_id bigint NOT NULL,
                occurrence_no integer NOT NULL,
                nullable_value integer,
                payload text NOT NULL,
                capability_mask bigint NOT NULL,
                UNIQUE (base_id, occurrence_no)
            );
            INSERT INTO cache_security_rows VALUES
                (1, 10, 0, NULL, 'null', 3),
                (2, 11, 0, 0,    'zero', 3),
                (3, 12, 0, 5,    'five', 3),
                (4, 13, 0, 10,   'ten', 3);
            CREATE ROLE cache_security_alice;
            CREATE ROLE cache_security_bob;
            CREATE ROLE cache_security_monitor;
            GRANT SELECT ON cache_security_rows
                TO cache_security_alice, cache_security_bob;
            GRANT USAGE ON SCHEMA bioseq_cache
                TO cache_security_alice, cache_security_monitor;
            GRANT EXECUTE ON FUNCTION bioseq_cache.entries()
                TO cache_security_alice, cache_security_monitor;
            SELECT bioseq_cache_test.provider_init();
            SELECT bioseq_cache_test.provider_use_relation(
                'cache_security_rows');
            SELECT bioseq_cache_test.provider_reset();
        )SQL");
}

void invalidate_reusable_entries(
        bioseq::libpq::connection& connection) {
    bioseq::cache_test::exec(
        connection,
        "SELECT bioseq_cache.invalidate_all(); "
        "SELECT bioseq_cache_test.request_janitor_sweep(); "
        "SELECT bioseq_cache.reset_stats()");
}

[[nodiscard]] std::int64_t provider_stat(
        bioseq::libpq::connection& connection,
        std::string_view field) {
    return bioseq::cache_test::integer(
        connection,
        "SELECT (bioseq_cache_test.provider_stats()->>"
            + connection.escape_literal(field) + ")::bigint");
}

void require_provider_references_drained(
        bioseq::libpq::connection& connection) {
    CHECK(bioseq::cache_test::integer(
              connection,
              "SELECT reader_pins + waiter_references "
              "     + admission_reservations "
              "FROM bioseq_cache_test.planner_reference_counts()")
          == 0);
    CHECK(bioseq::cache_test::integer(
              connection,
              "SELECT count(*) "
              "FROM bioseq_cache_test.entry_states() "
              "WHERE state = 'BUILDING' "
              "   OR waiter_count <> 0 "
              "   OR reader_count <> 0")
          == 0);
}

void prime_mapping_source(
        bioseq::libpq::connection& connection) {
    bioseq::cache_test::exec(
        connection,
        "SET bioseq.query_cache_optimisation_mode = 'reuse'; "
        "SELECT bioseq_cache_test.provider_set_mapping_fault('none'); "
        "SELECT bioseq_cache.invalidate_all(); "
        "SELECT bioseq_cache_test.request_janitor_sweep(); "
        "SELECT bioseq_cache_test.provider_reset(); "
        "SELECT bioseq_cache.reset_stats()");
    static_cast<void>(connection.exec(provider_query, 30s));
    bioseq::cache_test::exec(
        connection,
        "SELECT bioseq_cache_test.provider_reset(); "
        "SELECT bioseq_cache.reset_stats(); "
        "DISCARD PLANS");
}

struct ProviderCounters {
    std::int64_t extract_occurrence = 0;
    std::int64_t late_fetch_batch = 0;
    std::int64_t late_fetch_references = 0;
    std::int64_t executor_rows = 0;
    std::int64_t gated_expensive_rows = 0;
    std::int64_t boundary_column_count = 0;
    std::int64_t required_boundary_count = 0;
    std::int64_t lineage_column_count = 0;
    std::int64_t required_lineage_count = 0;
    std::int64_t recipe_required_boundary_count = 0;
};

[[nodiscard]] ProviderCounters read_provider_counters(
        bioseq::libpq::connection& connection) {
    return ProviderCounters{
        .extract_occurrence =
            provider_stat(connection, "extract_occurrence"),
        .late_fetch_batch =
            provider_stat(connection, "late_fetch_batch"),
        .late_fetch_references =
            provider_stat(connection, "late_fetch_references"),
        .executor_rows =
            provider_stat(connection, "executor_rows"),
        .gated_expensive_rows =
            provider_stat(connection, "gated_expensive_rows"),
        .boundary_column_count =
            provider_stat(connection, "last_boundary_column_count"),
        .required_boundary_count =
            provider_stat(connection, "last_required_boundary_count"),
        .lineage_column_count =
            provider_stat(connection, "last_lineage_column_count"),
        .required_lineage_count =
            provider_stat(connection, "last_required_lineage_count"),
        .recipe_required_boundary_count =
            provider_stat(
                connection,
                "last_recipe_required_boundary_count"),
    };
}

void require_structured_reusable_explain(
        bioseq::libpq::connection& connection) {
    bioseq::cache_test::exec(
        connection,
        R"SQL(
            DO $$
            DECLARE
                plan_line text;
                plan_text text := '';
                plan jsonb;
                cache_node jsonb;
                cache_nodes jsonb;
                exact_node_count bigint;
                reusable_node_count bigint;
            BEGIN
                FOR plan_line IN EXECUTE
                    'EXPLAIN (VERBOSE, COSTS OFF) '
                    'SELECT row_id, base_id, occurrence_no, '
                    'nullable_value, payload, capability_mask '
                    'FROM cache_provider_rows '
                    'WHERE nullable_value <= 10 ORDER BY row_id'
                LOOP
                    plan_text := plan_text || plan_line || E'\n';
                END LOOP;
                IF position(
                       'BioseqCacheGenericTestProvider'
                       in plan_text) = 0
                   OR plan_text !~
                       'BioseqReusableCoverageScan\) on '
                       'public\.cache_provider_rows'
                   OR position(
                       'Cache Entry Kind: ReusableCoverage'
                       in plan_text) = 0 THEN
                    RAISE EXCEPTION
                        'reusable-current text plan proof is absent: %',
                        plan_text;
                END IF;

                EXECUTE
                    'EXPLAIN (FORMAT JSON, VERBOSE, COSTS OFF) '
                    'SELECT row_id, base_id, occurrence_no, '
                    'nullable_value, payload, capability_mask '
                    'FROM cache_provider_rows '
                    'WHERE nullable_value <= 10 ORDER BY row_id'
                    INTO plan_text;
                plan := plan_text::jsonb;
                WITH RECURSIVE plan_nodes(node) AS (
                    SELECT plan->0->'Plan'
                    UNION ALL
                    SELECT child.value
                    FROM plan_nodes
                    CROSS JOIN LATERAL jsonb_array_elements(
                        COALESCE(
                            plan_nodes.node->'Plans',
                            '[]'::jsonb)) AS child
                )
                SELECT node INTO cache_node
                FROM plan_nodes
                WHERE node->>'Cache Mode' = 'reuse'
                  AND node->>'Cache Entry Kind' =
                      'ReusableCoverage'
                LIMIT 1;
                IF cache_node IS NULL
                   OR cache_node->>'Cache Mode' <> 'reuse'
                   OR cache_node->>'Planned Cache Action' IS NULL
                   OR cache_node ? 'Actual Cache Action'
                   OR length(cache_node->>'Cache Entry') <> 16
                   OR cache_node->>'Schema' <> 'public'
                   OR cache_node->>'Relation Name' <>
                       'cache_provider_rows'
                   OR cache_node->>'Alias' <>
                       'cache_provider_rows'
                   OR cache_node->>'SQL' IS NULL
                   OR cache_node->>'State' <>
                       'planned/provisional'
                   OR jsonb_typeof(
                       cache_node->'Reusable Sources') <> 'number'
                   OR (
                       cache_node->>'Reusable Sources')::bigint <> 0
                   OR jsonb_typeof(
                       cache_node->'Selected Cache Branches')
                       <> 'number'
                   OR (
                       cache_node
                           ->>'Selected Cache Branches')::bigint <> 0
                   OR jsonb_typeof(
                       cache_node->'Residual Child') <> 'object'
                   OR jsonb_typeof(
                       cache_node->'Provider') <> 'object'
                   OR cache_node->'Provider'
                       ->>'Generic Provider' IS NULL
                   OR jsonb_typeof(
                       cache_node->'Provider'
                           ->'Provider Recipe Valid') <> 'boolean'
                   OR NOT (
                       cache_node->'Provider'
                           ->>'Provider Recipe Valid')::boolean THEN
                    RAISE EXCEPTION
                        'structured reusable EXPLAIN contract '
                        'is incomplete: %',
                        plan;
                END IF;
                WITH RECURSIVE plan_nodes(node, parent_path) AS (
                    SELECT
                        plan->0->'Plan',
                        ARRAY[]::text[]
                    UNION ALL
                    SELECT
                        child.value,
                        plan_nodes.parent_path
                            || COALESCE(
                                plan_nodes.node->>'Node Type',
                                '<unknown>')
                    FROM plan_nodes
                    CROSS JOIN LATERAL jsonb_array_elements(
                        COALESCE(
                            plan_nodes.node->'Plans',
                            '[]'::jsonb)) AS child
                )
                SELECT
                    count(*) FILTER (
                        WHERE node->>'Cache Entry Kind' =
                            'ExactFinalResult'),
                    count(*) FILTER (
                        WHERE node->>'Cache Entry Kind' =
                            'ReusableCoverage'),
                    jsonb_agg(jsonb_build_object(
                        'cache_entry_kind',
                            node->>'Cache Entry Kind',
                        'node_type', node->>'Node Type',
                        'parent_path', to_jsonb(parent_path),
                        'cache_force_bypass',
                            node->>'Cache Force Bypass'))
                INTO
                    exact_node_count,
                    reusable_node_count,
                    cache_nodes
                FROM plan_nodes
                WHERE node->>'Cache Entry Kind' IN (
                    'ExactFinalResult',
                    'ReusableCoverage');
                IF exact_node_count <> 0
                   OR reusable_node_count <> 1
                   OR plan->0->'Plan'->>'Cache Entry Kind'
                       <> 'ReusableCoverage' THEN
                    RAISE EXCEPTION
                        'cold reusable plan cache-kind counts '
                        'exact=%, reusable=%: %',
                        exact_node_count,
                        reusable_node_count,
                        cache_nodes;
                END IF;
            END
            $$;
        )SQL",
        30s);
}

void require_main_tree_and_mixed_initplan_coverage(
        bioseq::libpq::connection& connection) {
    bioseq::cache_test::exec(
        connection,
        R"SQL(
            DO $$
            DECLARE
                plan_text text;
                plan jsonb;
                coverage_node_count bigint;
                enabled_coverage_count bigint;
                bypassed_coverage_count bigint;
                observed_ids bigint[];
            BEGIN
                EXECUTE
                    'EXPLAIN (FORMAT JSON, VERBOSE, COSTS OFF) '
                    'SELECT nested.row_id FROM ('
                    '  SELECT row_id FROM ONLY cache_provider_rows '
                    '  WHERE nullable_value <= 10 OFFSET 0'
                    ') AS nested ORDER BY nested.row_id'
                    INTO plan_text;
                plan := plan_text::jsonb;
                WITH RECURSIVE plan_nodes(node) AS (
                    SELECT plan->0->'Plan'
                    UNION ALL
                    SELECT child.value
                    FROM plan_nodes
                    CROSS JOIN LATERAL jsonb_array_elements(
                        COALESCE(
                            plan_nodes.node->'Plans',
                            '[]'::jsonb)) AS child
                )
                SELECT
                    count(*) FILTER (
                        WHERE node->>'Cache Entry Kind' =
                            'ReusableCoverage'),
                    count(*) FILTER (
                        WHERE node->>'Cache Entry Kind' =
                            'ReusableCoverage'
                          AND NOT (
                              node->>'Cache Force Bypass')::boolean),
                    count(*) FILTER (
                        WHERE node->>'Cache Entry Kind' =
                            'ReusableCoverage'
                          AND (
                              node->>'Cache Force Bypass')::boolean)
                INTO
                    coverage_node_count,
                    enabled_coverage_count,
                    bypassed_coverage_count
                FROM plan_nodes;
                IF coverage_node_count <> 1
                   OR enabled_coverage_count <> 1
                   OR bypassed_coverage_count <> 0
                   OR position('Subquery Scan' in plan_text) = 0 THEN
                    RAISE EXCEPTION
                        'main-tree SubqueryScan coverage marker '
                        'is wrong: %',
                        plan;
                END IF;

                EXECUTE
                    'SELECT array_agg('
                    'nested.row_id ORDER BY nested.row_id) FROM ('
                    '  SELECT row_id FROM ONLY cache_provider_rows '
                    '  WHERE nullable_value <= 10 OFFSET 0'
                    ') AS nested'
                    INTO observed_ids;
                IF observed_ids IS DISTINCT FROM
                       ARRAY[2, 3, 4, 5]::bigint[] THEN
                    RAISE EXCEPTION
                        'main-tree SubqueryScan changed rows: %',
                        observed_ids;
                END IF;

                EXECUTE
                    'SELECT array_agg('
                    'nested.row_id ORDER BY nested.row_id) FROM ('
                    '  SELECT row_id FROM ONLY cache_provider_rows '
                    '  WHERE nullable_value <= 10 OFFSET 0'
                    ') AS nested '
                    'WHERE nested.row_id <= ('
                    '  SELECT max(row_id) '
                    '  FROM ONLY cache_provider_rows '
                    '  WHERE nullable_value <= 10)'
                    INTO observed_ids;
                IF observed_ids IS DISTINCT FROM
                       ARRAY[2, 3, 4, 5]::bigint[] THEN
                    RAISE EXCEPTION
                        'mixed main/subplan coverage changed rows: %',
                        observed_ids;
                END IF;
            END
            $$;
        )SQL",
        30s);
}

void require_nested_provider_recipe_fail_open(
        bioseq::libpq::connection& connection) {
    bioseq::cache_test::exec(
        connection,
        R"SQL(
            CREATE TABLE cache_recipe_left(
                id integer PRIMARY KEY);
            CREATE TABLE cache_recipe_right(
                id integer PRIMARY KEY);
            INSERT INTO cache_recipe_left VALUES (1), (2), (3);
            INSERT INTO cache_recipe_right VALUES (1), (2), (3);
            SELECT bioseq_cache.invalidate_all();
            SELECT bioseq_cache_test.request_janitor_sweep();
            SELECT bioseq_cache_test.provider_reset();
            SELECT bioseq_cache.reset_stats();

            DO $$
            DECLARE
                plan_text text;
                plan jsonb;
                coverage_node_count bigint;
                bypassed_coverage_count bigint;
            BEGIN
                EXECUTE
                    'EXPLAIN (FORMAT JSON, VERBOSE, COSTS OFF) '
                    'SELECT l.id, r.id '
                    'FROM cache_recipe_left l '
                    'LEFT JOIN cache_recipe_right r '
                    'ON l.id = r.id AND l.id + r.id <= '
                    '(SELECT sum(row_id) '
                    ' FROM cache_provider_rows '
                    ' WHERE nullable_value <= 10) '
                    'ORDER BY l.id'
                    INTO plan_text;
                plan := plan_text::jsonb;
                WITH RECURSIVE plan_nodes(node) AS (
                    SELECT plan->0->'Plan'
                    UNION ALL
                    SELECT child.value
                    FROM plan_nodes
                    CROSS JOIN LATERAL jsonb_array_elements(
                        COALESCE(
                            plan_nodes.node->'Plans',
                            '[]'::jsonb)) AS child
                )
                SELECT
                    count(*) FILTER (
                        WHERE node->>'Cache Entry Kind' =
                            'ReusableCoverage'),
                    count(*) FILTER (
                        WHERE node->>'Cache Entry Kind' =
                            'ReusableCoverage'
                          AND (
                              node->>'Cache Force Bypass')::boolean)
                INTO
                    coverage_node_count,
                    bypassed_coverage_count
                FROM plan_nodes;
                IF plan->0->'Plan'->>'Cache Entry Kind'
                       <> 'ExactFinalResult'
                   OR (
                       plan->0->'Plan'
                           ->>'Cache Force Bypass')::boolean
                   OR (
                       plan->0->'Plan'
                           ->>'Provider Runtime Recipes')::integer
                       <> 1
                   OR coverage_node_count <> 1
                   OR bypassed_coverage_count <> 1
                   OR position(
                       'BioseqCacheGenericTestProvider'
                       in plan_text) = 0
                   OR position('InitPlan' in plan_text) = 0
                   OR position('Join Filter' in plan_text) = 0 THEN
                    RAISE EXCEPTION
                        'nested provider recipe was not captured '
                        'from join/initplan: %',
                        plan;
                END IF;
            END
            $$;

            SELECT bioseq_cache.invalidate_all();
            SELECT bioseq_cache_test.request_janitor_sweep();
            SELECT bioseq_cache_test.provider_reset();
            SELECT bioseq_cache.reset_stats();
            SELECT bioseq_cache_test.set_exact_plan_fault(
                'dependency_finalization_declined');
            DO $$
            DECLARE
                plan_text text;
                plan jsonb;
                coverage_node_count bigint;
                bypassed_coverage_count bigint;
                first_ids integer[];
                second_ids integer[];
                cache_stats record;
                active_entries bigint;
                nested_query constant text :=
                    'SELECT array_agg('
                    'COALESCE(joined.right_id, -1) '
                    'ORDER BY joined.left_id) FROM ('
                    'SELECT l.id AS left_id, r.id AS right_id '
                    'FROM cache_recipe_left l '
                    'LEFT JOIN cache_recipe_right r '
                    'ON l.id = r.id AND l.id + r.id <= '
                    '(SELECT sum(row_id) '
                    ' FROM cache_provider_rows '
                    ' WHERE nullable_value <= 10)) AS joined';
            BEGIN
                EXECUTE
                    'EXPLAIN (FORMAT JSON, VERBOSE, COSTS OFF) '
                    'SELECT l.id FROM cache_recipe_left l '
                    'LEFT JOIN cache_recipe_right r '
                    'ON l.id = r.id AND l.id + r.id <= '
                    '(SELECT sum(row_id) '
                    ' FROM cache_provider_rows '
                    ' WHERE nullable_value <= 10) '
                    'ORDER BY l.id'
                    INTO plan_text;
                plan := plan_text::jsonb;
                WITH RECURSIVE plan_nodes(node) AS (
                    SELECT plan->0->'Plan'
                    UNION ALL
                    SELECT child.value
                    FROM plan_nodes
                    CROSS JOIN LATERAL jsonb_array_elements(
                        COALESCE(
                            plan_nodes.node->'Plans',
                            '[]'::jsonb)) AS child
                )
                SELECT
                    count(*) FILTER (
                        WHERE node->>'Cache Entry Kind' =
                            'ReusableCoverage'),
                    count(*) FILTER (
                        WHERE node->>'Cache Entry Kind' =
                            'ReusableCoverage'
                          AND (
                              node->>'Cache Force Bypass')::boolean)
                INTO
                    coverage_node_count,
                    bypassed_coverage_count
                FROM plan_nodes;
                IF plan->0->'Plan'->>'Cache Entry Kind'
                       <> 'ExactFinalResult'
                   OR NOT (
                       plan->0->'Plan'
                           ->>'Cache Force Bypass')::boolean
                   OR plan->0->'Plan'
                           ->>'Cache Bypass Reason'
                       <> 'internal nested-coverage gate finalizer'
                   OR coverage_node_count <> 1
                   OR bypassed_coverage_count <> 1
                   OR position('InitPlan' in plan_text) = 0 THEN
                    RAISE EXCEPTION
                        'nested gate finalizer plan is unsafe: %',
                        plan;
                END IF;

                EXECUTE nested_query INTO first_ids;
                EXECUTE nested_query INTO second_ids;
                SELECT * INTO cache_stats
                FROM bioseq_cache.stats();
                SELECT count(*) INTO active_entries
                FROM bioseq_cache_test.entry_states()
                WHERE state IN (
                    'CLAIMED', 'BUILDING', 'COMPLETE');
                IF first_ids IS DISTINCT FROM
                       ARRAY[1, 2, 3]
                   OR second_ids IS DISTINCT FROM first_ids
                   OR cache_stats.produced <> 0
                   OR cache_stats.completed_hits <> 0
                   OR cache_stats.inflight_waits <> 0
                   OR cache_stats.reuse_hits <> 0
                   OR cache_stats.fallbacks <> 4
                   OR active_entries <> 0 THEN
                    RAISE EXCEPTION
                        'nested gate finalizer touched cache or rows: '
                        '%, %, %, %, %',
                        first_ids,
                        second_ids,
                        row_to_json(cache_stats),
                        active_entries,
                        plan;
                END IF;
            END
            $$;
            SELECT bioseq_cache_test.set_exact_plan_fault('none');

            SELECT bioseq_cache_test.set_exact_recipe_fault(
                'expected_digest_mismatch');
            DO $$
            DECLARE
                first_ids integer[];
                second_ids integer[];
                first_provider_rows bigint;
                second_provider_rows bigint;
                cache_stats record;
                nested_query constant text :=
                    'SELECT array_agg('
                    'COALESCE(joined.right_id, -1) '
                    'ORDER BY joined.left_id) FROM ('
                    'SELECT l.id AS left_id, r.id AS right_id '
                    'FROM cache_recipe_left l '
                    'LEFT JOIN cache_recipe_right r '
                    'ON l.id = r.id AND l.id + r.id <= '
                    '(SELECT sum(row_id) '
                    ' FROM cache_provider_rows '
                    ' WHERE nullable_value <= 10)) AS joined';
            BEGIN
                EXECUTE nested_query INTO first_ids;
                first_provider_rows := (
                    bioseq_cache_test.provider_stats()
                        ->>'executor_rows')::bigint;
                EXECUTE nested_query INTO second_ids;
                second_provider_rows := (
                    bioseq_cache_test.provider_stats()
                        ->>'executor_rows')::bigint;
                SELECT * INTO cache_stats
                FROM bioseq_cache.stats();
                IF first_ids IS DISTINCT FROM
                       ARRAY[1, 2, 3]
                   OR second_ids IS DISTINCT FROM first_ids
                   OR first_provider_rows <= 0
                   OR second_provider_rows <= first_provider_rows
                   OR cache_stats.completed_hits <> 0
                   OR cache_stats.reuse_hits <> 0
                   OR cache_stats.produced <> 0
                   OR cache_stats.fallbacks <> 8 THEN
                    RAISE EXCEPTION
                        'nested recipe did not fail open through '
                        'reusable child: %, %, %, %, %',
                        first_ids,
                        second_ids,
                        first_provider_rows,
                        second_provider_rows,
                        row_to_json(cache_stats);
                END IF;
            END
            $$;
            SELECT bioseq_cache_test.set_exact_recipe_fault('none');
        )SQL",
        30s);
}

void require_prepared_nested_security_partition(
        bioseq::libpq::connection& connection) {
    bioseq::cache_test::exec(
        connection,
        R"SQL(
            SELECT bioseq_cache.invalidate_all();
            SELECT bioseq_cache_test.request_janitor_sweep();
            SELECT bioseq_cache_test.provider_reset();
            SELECT bioseq_cache.reset_stats();
            SELECT bioseq_cache_test.provider_set_security_digest(
                decode(repeat('11', 32), 'hex'));
            PREPARE cache_nested_security AS
            SELECT array_agg(
                COALESCE(joined.right_id, -1)
                ORDER BY joined.left_id)
            FROM (
                SELECT
                    l.id AS left_id,
                    r.id AS right_id
                FROM cache_recipe_left AS l
                LEFT JOIN cache_recipe_right AS r
                  ON l.id = r.id
                 AND l.id + r.id <= (
                    SELECT sum(row_id)
                    FROM cache_provider_rows
                    WHERE nullable_value <= 10)
            ) AS joined;
            EXECUTE cache_nested_security;
            EXECUTE cache_nested_security;
            SELECT bioseq_cache_test.provider_set_security_digest(
                decode(repeat('22', 32), 'hex'));
            EXECUTE cache_nested_security;
            EXECUTE cache_nested_security;
            DO $$
            DECLARE
                cache_stats record;
            BEGIN
                SELECT * INTO cache_stats
                FROM bioseq_cache.stats();
                IF cache_stats.produced <> 2
                   OR cache_stats.completed_hits <> 2
                   OR cache_stats.reuse_hits <> 0
                   OR cache_stats.fallbacks <> 4 THEN
                    RAISE EXCEPTION
                        'nested coverage semantics did not '
                        'partition exact key: %',
                        row_to_json(cache_stats);
                END IF;
            END
            $$;
            DEALLOCATE cache_nested_security;
            SELECT bioseq_cache_test.provider_clear_security_digest();
        )SQL",
        30s);
}

void require_losing_reusable_alternative_preserves_exact(
        bioseq::libpq::connection& connection) {
    bioseq::cache_test::exec(
        connection,
        R"SQL(
            SELECT bioseq_cache.invalidate_all();
            SELECT bioseq_cache_test.request_janitor_sweep();
            SELECT bioseq_cache_test.provider_reset();
            SELECT bioseq_cache.reset_stats();
            SET random_page_cost = 100;
            SET enable_sort = off;
            DO $$
            DECLARE
                plan_text text;
                plan jsonb;
                provider_stats jsonb;
            BEGIN
                EXECUTE
                    'EXPLAIN (FORMAT JSON, VERBOSE, COSTS OFF) '
                    'SELECT row_id, base_id, occurrence_no, '
                    'nullable_value, payload, capability_mask '
                    'FROM cache_provider_rows '
                    'WHERE row_id > 0 ORDER BY row_id'
                    INTO plan_text;
                plan := plan_text::jsonb;
                provider_stats :=
                    bioseq_cache_test.provider_stats();
                IF plan->0->'Plan'->>'Cache Entry Kind'
                       <> 'ExactFinalResult'
                   OR COALESCE(
                       (
                           plan->0->'Plan'
                               ->>'Cache Force Bypass')::boolean,
                       true)
                   OR position(
                       'BioseqReusableCoverageScan'
                       in plan_text) <> 0
                   OR position('Index Scan' in plan_text) = 0
                   OR (
                       provider_stats
                           ->>'decorate_path')::bigint = 0 THEN
                    RAISE EXCEPTION
                        'losing coverage alternative suppressed '
                        'the exact path: %, %',
                        plan,
                        provider_stats;
                END IF;
            END
            $$;

            SELECT
                row_id,
                base_id,
                occurrence_no,
                nullable_value,
                payload,
                capability_mask
            FROM cache_provider_rows
            WHERE row_id > 0
            ORDER BY row_id;
            SELECT
                row_id,
                base_id,
                occurrence_no,
                nullable_value,
                payload,
                capability_mask
            FROM cache_provider_rows
            WHERE row_id > 0
            ORDER BY row_id;

            DO $$
            DECLARE
                cache_stats record;
                provider_stats jsonb;
            BEGIN
                SELECT * INTO cache_stats
                FROM bioseq_cache.stats();
                provider_stats :=
                    bioseq_cache_test.provider_stats();
                IF cache_stats.produced <> 1
                   OR cache_stats.completed_hits <> 1
                   OR cache_stats.reuse_hits <> 0
                   OR cache_stats.fallbacks <> 0
                   OR (
                       provider_stats
                           ->>'executor_rows')::bigint <> 0
                   OR (
                       provider_stats
                           ->>'extract_occurrence')::bigint <> 0 THEN
                    RAISE EXCEPTION
                        'chosen exact path did not produce and '
                        'replay cleanly: %, %',
                        row_to_json(cache_stats),
                        provider_stats;
                END IF;
            END
            $$;
            RESET enable_sort;
            RESET random_page_cost;
        )SQL",
        30s);
}

}  // namespace

TEST_CASE(
    "legacy reusable coverage capability and digest residuals stay exact",
    "[cache][integration][provider][reuse][migration]") {
    constexpr std::string_view case_name{
        "legacy reusable coverage capability and digest residuals stay exact"};
    bioseq::postgres_test::migration_probe_scope probes{
        std::string(case_name)};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    create_standard_provider_fixture(connection);

    SECTION("coverage capability and digest residual cases") {
        bioseq::postgres_test::migration_probe_scope section_probes{
            std::string(case_name),
            "coverage capability and digest residual cases"};

        bioseq::cache_test::exec(
            connection,
            "SET bioseq.query_cache_optimisation_mode = 'disabled'");
        const auto oracle = connection.exec(consumer_query, 30s);
        REQUIRE(oracle.rows() == 4);
        const auto oracle_digest =
            bioseq::postgres_test::exact_row_hash(oracle);

        invalidate_reusable_entries(connection);
        bioseq::cache_test::exec(
            connection,
            "SET bioseq.query_cache_optimisation_mode = 'reuse'; "
            "SELECT bioseq_cache_test.provider_reset()");
        require_structured_reusable_explain(connection);
        bioseq::cache_test::exec(
            connection,
            "SELECT bioseq_cache_test.provider_reset(); "
            "SELECT bioseq_cache.reset_stats()");
        const auto cold = connection.exec(provider_query, 30s);
        const auto warm = connection.exec(provider_query, 30s);
        REQUIRE(cold.rows() == 4);
        CHECK(
            bioseq::postgres_test::exact_row_hash(cold)
            == bioseq::postgres_test::exact_row_hash(warm));
        const auto current_stats =
            bioseq::cache_test::read_cache_stats(connection);
        CHECK(current_stats.produced == 1);
        CHECK(current_stats.completed_hits == 0);
        CHECK(current_stats.reuse_hits == 1);
        CHECK(current_stats.fallbacks == 0);
        CHECK(
            provider_stat(connection, "last_boundary_column_count")
            == 6);
        CHECK(
            provider_stat(connection, "last_required_boundary_count")
            == 6);
        CHECK(
            provider_stat(connection, "last_lineage_column_count")
            == 3);
        CHECK(
            provider_stat(connection, "last_required_lineage_count")
            == 3);
        CHECK(
            provider_stat(
                connection,
                "last_recipe_required_boundary_count")
            == 6);

        require_main_tree_and_mixed_initplan_coverage(connection);
        require_nested_provider_recipe_fail_open(connection);
        require_prepared_nested_security_partition(connection);
        require_losing_reusable_alternative_preserves_exact(
            connection);
        bioseq::cache_test::exec(
            connection,
            "SELECT bioseq_cache.invalidate_all(); "
            "SELECT bioseq_cache_test.request_janitor_sweep(); "
            "SELECT bioseq_cache_test.provider_reset(); "
            "SELECT bioseq_cache.reset_stats()");

        CHECK(
            bioseq::cache_test::scalar(
                connection,
                "SELECT array_agg(row_id ORDER BY row_id)::text "
                "FROM cache_provider_rows "
                "WHERE nullable_value <= 10")
            == "{2,3,4,5}");
        CHECK(
            bioseq::cache_test::scalar(
                connection,
                "SELECT array_agg(occurrence_no "
                "ORDER BY occurrence_no)::text "
                "FROM cache_provider_rows "
                "WHERE base_id = 12 AND nullable_value >= 5")
            == "{0,1}");
        const auto residual = connection.exec(consumer_query, 30s);
        CHECK(
            bioseq::postgres_test::exact_row_hash(residual)
            == oracle_digest);
        require_provider_references_drained(connection);

        bioseq::cache_test::exec(
            connection,
            R"SQL(
                CREATE TABLE cache_provider_collision(
                    LIKE cache_provider_rows INCLUDING ALL);
                INSERT INTO cache_provider_collision VALUES
                    (101, 110, 0, 5, 'other relation', 3);
                SELECT bioseq_cache_test.set_forced_coverage_domain_digest(
                    decode(repeat('42', 32), 'hex'));
                SELECT bioseq_cache_test.provider_force_domain_collision(
                    true);
                SELECT bioseq_cache_test.provider_use_relation(
                    'cache_provider_collision');
            )SQL");
        const auto collision = connection.exec(
            "SELECT row_id FROM cache_provider_collision "
            "WHERE nullable_value >= 5 ORDER BY row_id",
            30s);
        REQUIRE(collision.rows() == 1);
        CHECK(collision.value(0, 0) == "101");
        bioseq::cache_test::exec(
            connection,
            "SELECT bioseq_cache_test.clear_forced_coverage_domain_digest(); "
            "SELECT bioseq_cache_test.provider_force_domain_collision(false); "
            "SELECT bioseq_cache_test.provider_use_relation("
            "'cache_provider_rows'); "
            "DISCARD PLANS");

        constexpr std::array<std::string_view, 22>
            malformed_mapping_faults{
                "null_pointer_nonzero_count",
                "short_boundary_map",
                "long_boundary_map",
                "short_lineage_map",
                "long_lineage_map",
                "zero_consumer_attno",
                "zero_source_attno",
                "source_attno_out_of_range",
                "duplicate_consumer_attno",
                "missing_required_attno",
                "wrong_type",
                "wrong_typmod",
                "wrong_collation",
                "wrong_semantic_id",
                "same_typed_column_permutation",
                "illegal_repeated_source_attno",
                "bad_provider_private_version",
                "non_integer_persisted_map_node",
                "unknown_persisted_map_version",
                "decorated_boundary_schema_mismatch",
                "decorated_lineage_schema_mismatch",
                "provider_status_error",
            };
        for (const std::string_view fault : malformed_mapping_faults) {
            prime_mapping_source(connection);
            bioseq::cache_test::exec(
                connection,
                "SELECT bioseq_cache_test.provider_set_mapping_fault("
                    + connection.escape_literal(fault) + ")");
            const auto declined =
                connection.exec(consumer_query, 30s);
            CHECK(
                bioseq::postgres_test::exact_row_hash(declined)
                == oracle_digest);
            CHECK(
                provider_stat(connection, "map_source") >= 1);
            CHECK(
                provider_stat(connection, "executor_rows") == 4);
            CHECK(
                bioseq::cache_test::read_cache_stats(connection)
                    .reuse_hits
                == 0);
            require_provider_references_drained(connection);
            bioseq::cache_test::exec(
                connection,
                "SELECT bioseq_cache_test.provider_set_mapping_fault("
                "'none')");
        }

        prime_mapping_source(connection);
        bioseq::cache_test::exec(
            connection,
            "SELECT bioseq_cache_test.provider_set_capabilities(3); "
            "SELECT bioseq_cache_test.provider_set_projection_order("
            "'reverse'); "
            "SELECT bioseq_cache_test.provider_set_digest_fault("
            "'runtime_mismatch')");
        const auto capability_digest_decline =
            connection.exec(consumer_query, 30s);
        CHECK(
            bioseq::postgres_test::exact_row_hash(
                capability_digest_decline)
            == oracle_digest);
        CHECK(
            provider_stat(connection, "executor_rows") == 4);
        require_provider_references_drained(connection);
        bioseq::cache_test::exec(
            connection,
            "SELECT bioseq_cache_test.provider_set_digest_fault('none'); "
            "SELECT bioseq_cache_test.provider_set_projection_order("
            "'natural'); "
            "SELECT bioseq_cache_test.provider_set_capabilities(1)");
        CHECK(bioseq::cache_test::boolean(
            connection,
            "SELECT (bioseq_cache_test.provider_stats()"
            "->>'registered')::boolean"));
        CHECK(
            provider_stat(connection, "planner_bridge_calls") > 0);
        CHECK(provider_stat(connection, "executor_rows") > 0);
        bioseq::cache_test::exec(
            connection,
            "SELECT bioseq_cache_test.provider_shutdown()");
        bioseq::postgres_test::record_migration_probe("regress.cache_reuse_provider.remaining_contract");
    }
}

TEST_CASE(
    "legacy reusable descriptor fault table declines safely",
    "[cache][integration][provider][fault][descriptor][migration]") {
    constexpr std::string_view case_name{
        "legacy reusable descriptor fault table declines safely"};
    bioseq::postgres_test::migration_probe_scope probes{
        std::string(case_name)};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    create_descriptor_provider_fixture(connection);

    SECTION("all seven descriptor faults") {
        bioseq::postgres_test::migration_probe_scope section_probes{
            std::string(case_name),
            "all seven descriptor faults"};
        constexpr std::array<std::string_view, 7> descriptor_faults{
            "raw_producer_var",
            "foreign_mixed_var",
            "wrong_node_type",
            "malformed_syntax",
            "binding_signature_mismatch",
            "plan_private_short",
            "plan_private_bad_version",
        };
        for (const std::string_view fault : descriptor_faults) {
            bioseq::cache_test::exec(
                connection,
                "SET bioseq.query_cache_optimisation_mode = 'reuse'; "
                "SELECT bioseq_cache_test.set_coverage_descriptor_fault("
                "'none'); "
                "SELECT bioseq_cache.invalidate_all(); "
                "SELECT bioseq_cache_test.request_janitor_sweep(); "
                "SELECT bioseq_cache_test.provider_reset(); "
                "SELECT bioseq_cache.reset_stats()");
            static_cast<void>(connection.exec(
                "SELECT count(*) "
                "FROM cache_provider_binding_rows "
                "WHERE nullable_value >= 5",
                30s));
            bioseq::cache_test::exec(
                connection,
                "SELECT bioseq_cache_test.provider_reset(); "
                "SELECT bioseq_cache.reset_stats(); "
                "SELECT bioseq_cache_test.set_coverage_descriptor_fault("
                    + connection.escape_literal(fault) + ")");
            const auto declined = connection.exec(
                "SELECT count(*) "
                "FROM cache_provider_binding_rows "
                "WHERE nullable_value >= 6",
                30s);
            REQUIRE(declined.rows() == 1);
            CHECK(declined.value(0, 0) == "1");
            CHECK(
                bioseq::cache_test::read_cache_stats(connection)
                    .reuse_hits
                == 0);
            CHECK(
                provider_stat(connection, "executor_rows") == 1);
            require_provider_references_drained(connection);
            bioseq::cache_test::exec(
                connection,
                "SELECT bioseq_cache_test.set_coverage_descriptor_fault("
                "'none')");
        }
        bioseq::cache_test::exec(
            connection,
            "SELECT bioseq_cache_test.provider_shutdown()");
        bioseq::postgres_test::record_migration_probe("regress.cache_reuse_provider_binding.descriptor_fault_table");
    }
}

TEST_CASE(
    "legacy serialized mapping corruption table declines safely",
    "[cache][integration][provider][fault][mapping][migration]") {
    constexpr std::string_view case_name{
        "legacy serialized mapping corruption table declines safely"};
    bioseq::postgres_test::migration_probe_scope probes{
        std::string(case_name)};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    create_standard_provider_fixture(connection);

    SECTION("all serialized mapping corruptions") {
        bioseq::postgres_test::migration_probe_scope section_probes{
            std::string(case_name),
            "all serialized mapping corruptions"};
        constexpr std::array<std::string_view, 16>
            serialized_mapping_faults{
                "persisted_wrong_top_tag",
                "persisted_short_top",
                "persisted_long_top",
                "persisted_bad_magic",
                "persisted_bad_version",
                "persisted_count_overflow",
                "persisted_wrong_entry_tag",
                "persisted_short_entry",
                "persisted_long_entry",
                "persisted_bad_semantic_size",
                "persisted_bad_fingerprint_size",
                "persisted_bad_entry_metadata",
                "persisted_source_fingerprint_mismatch",
                "persisted_consumer_fingerprint_mismatch",
                "persisted_decorated_fingerprint_mismatch",
                "persisted_map_fingerprint_mismatch",
            };
        bioseq::cache_test::exec(
            connection,
            "SET plan_cache_mode = force_generic_plan");
        for (const std::string_view fault :
             serialized_mapping_faults) {
            bioseq::cache_test::exec(
                connection,
                "SELECT bioseq_cache_test.provider_set_mapping_fault("
                "'none'); "
                "SELECT bioseq_cache.invalidate_all(); "
                "SELECT bioseq_cache_test.request_janitor_sweep(); "
                "SELECT bioseq_cache_test.provider_reset(); "
                "SELECT bioseq_cache.reset_stats(); "
                "DISCARD PLANS");
            static_cast<void>(connection.exec(
                "SELECT array_agg(row_id ORDER BY row_id) "
                "FROM cache_provider_rows "
                "WHERE nullable_value <= 10",
                30s));
            bioseq::cache_test::exec(
                connection,
                "PREPARE bioseq_mapping_probe AS "
                "SELECT array_agg(row_id ORDER BY row_id)::bigint[] "
                "FROM cache_provider_rows "
                "WHERE nullable_value >= 5");
            static_cast<void>(connection.exec(
                "EXPLAIN (FORMAT JSON, VERBOSE, COSTS OFF) "
                "EXECUTE bioseq_mapping_probe",
                30s));
            const ProviderCounters before =
                read_provider_counters(connection);
            CHECK(before.boundary_column_count == 6);
            CHECK(before.required_boundary_count == 1);
            CHECK(before.lineage_column_count == 3);
            CHECK(before.required_lineage_count == 3);
            CHECK(before.recipe_required_boundary_count == 1);

            bioseq::cache_test::exec(
                connection,
                "SELECT bioseq_cache.reset_stats(); "
                "SELECT bioseq_cache_test.provider_set_mapping_fault("
                    + connection.escape_literal(fault) + ")");
            const auto declined =
                connection.exec("EXECUTE bioseq_mapping_probe", 30s);
            REQUIRE(declined.rows() == 1);
            CHECK(declined.value(0, 0) == "{3,4,5,6}");
            const auto stats =
                bioseq::cache_test::read_cache_stats(connection);
            CHECK(stats.produced == 1);
            CHECK(stats.completed_hits == 0);
            CHECK(stats.reuse_hits == 0);
            CHECK(stats.fallbacks == 0);
            const ProviderCounters after =
                read_provider_counters(connection);
            CHECK(
                after.extract_occurrence
                    - before.extract_occurrence
                == 4);
            CHECK(
                after.late_fetch_batch - before.late_fetch_batch
                == 0);
            CHECK(
                after.late_fetch_references
                    - before.late_fetch_references
                == 0);
            CHECK(
                after.executor_rows - before.executor_rows
                == 4);
            CHECK(
                after.gated_expensive_rows
                    - before.gated_expensive_rows
                == 4);
            require_provider_references_drained(connection);
            bioseq::cache_test::exec(
                connection,
                "SELECT bioseq_cache_test.provider_set_mapping_fault("
                "'none'); "
                "DEALLOCATE bioseq_mapping_probe");
        }
        bioseq::cache_test::exec(
            connection,
            "RESET plan_cache_mode; "
            "SELECT bioseq_cache.invalidate_all(); "
            "SELECT bioseq_cache_test.request_janitor_sweep(); "
            "SELECT bioseq_cache_test.provider_reset(); "
            "SELECT bioseq_cache.reset_stats(); "
            "SELECT bioseq_cache_test.provider_shutdown()");
        bioseq::postgres_test::record_migration_probe("regress.cache_reuse_provider.serialized_mapping_corruption_table");
    }
}

TEST_CASE(
    "legacy reusable provider security namespace and redaction remain exact",
    "[cache][integration][provider][security][migration]") {
    constexpr std::string_view case_name{
        "legacy reusable provider security namespace and redaction remain exact"};
    bioseq::postgres_test::migration_probe_scope probes{
        std::string(case_name)};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    create_security_provider_fixture(connection);

    SECTION("provider semantic namespace and redaction") {
        bioseq::postgres_test::migration_probe_scope section_probes{
            std::string(case_name),
            "provider semantic namespace and redaction"};

        invalidate_reusable_entries(connection);
        bioseq::cache_test::exec(
            connection, "SET ROLE cache_security_alice");
        const auto alice_cold =
            connection.exec(security_query, 30s);
        const auto alice_warm =
            connection.exec(security_query, 30s);
        bioseq::cache_test::exec(connection, "RESET ROLE");
        REQUIRE(alice_cold.rows() == 3);
        REQUIRE(alice_warm.rows() == 3);
        CHECK(
            bioseq::postgres_test::exact_row_hash(alice_cold)
            == bioseq::postgres_test::exact_row_hash(alice_warm));

        bioseq::cache_test::exec(
            connection, "SET ROLE cache_security_bob");
        const auto bob_cold =
            connection.exec(security_query, 30s);
        const auto bob_warm =
            connection.exec(security_query, 30s);
        bioseq::cache_test::exec(connection, "RESET ROLE");
        REQUIRE(bob_cold.rows() == 3);
        REQUIRE(bob_warm.rows() == 3);
        CHECK(
            bioseq::postgres_test::exact_row_hash(bob_cold)
            == bioseq::postgres_test::exact_row_hash(bob_warm));
        const auto role_stats =
            bioseq::cache_test::read_cache_stats(connection);
        CHECK(role_stats.produced == 2);
        CHECK(role_stats.reuse_hits == 2);
        CHECK(role_stats.fallbacks == 0);

        bioseq::cache_test::exec(
            connection, "SET ROLE cache_security_alice");
        CHECK(bioseq::cache_test::boolean(
            connection,
            R"SQL(
                SELECT own_rows = 1
                   AND scalar_rows = 2
                   AND foreign_rows = 1
                   AND leaked_rows = 0
                FROM (
                    SELECT
                        count(*) FILTER (
                            WHERE kind = 'ReusableCoverage'
                              AND state = 'COMPLETE'
                              AND owner_role =
                                  'cache_security_alice'::regrole
                              AND producer_pid IS NOT NULL
                              AND sql IS NOT NULL
                              AND producer_plan IS NOT NULL
                              AND dependencies
                                  ->>'canonical_plan_status' = 'Planned'
                              AND dependencies
                                  ->>'producer_plan_status' = 'Planned'
                        ) AS own_rows,
                        count(*) FILTER (
                            WHERE kind = 'ReusableCoverage'
                              AND state = 'COMPLETE'
                              AND entry_key IS NOT NULL
                              AND age IS NOT NULL
                              AND rows IS NOT NULL
                              AND logical_bytes IS NOT NULL
                              AND stored_bytes IS NOT NULL
                        ) AS scalar_rows,
                        count(*) FILTER (
                            WHERE kind = 'ReusableCoverage'
                              AND state = 'COMPLETE'
                              AND owner_role IS NULL
                              AND producer_pid IS NULL
                              AND sql IS NULL
                              AND producer_plan IS NULL
                              AND dependencies IS NULL
                        ) AS foreign_rows,
                        count(*) FILTER (
                            WHERE kind = 'ReusableCoverage'
                              AND state = 'COMPLETE'
                              AND owner_role IS NULL
                              AND (
                                  producer_pid IS NOT NULL
                                  OR sql IS NOT NULL
                                  OR producer_plan IS NOT NULL
                                  OR dependencies IS NOT NULL)
                        ) AS leaked_rows
                    FROM bioseq_cache.entries()
                ) AS observed
            )SQL"));
        bioseq::cache_test::exec(connection, "RESET ROLE");

        bioseq::cache_test::exec(
            connection, "SET ROLE cache_security_monitor");
        CHECK(bioseq::cache_test::boolean(
            connection,
            R"SQL(
                SELECT scalar_rows = 2 AND leaked_rows = 0
                FROM (
                    SELECT
                        count(*) FILTER (
                            WHERE kind = 'ReusableCoverage'
                              AND state = 'COMPLETE'
                              AND entry_key IS NOT NULL
                              AND age IS NOT NULL
                              AND rows IS NOT NULL
                              AND logical_bytes IS NOT NULL
                              AND stored_bytes IS NOT NULL
                        ) AS scalar_rows,
                        count(*) FILTER (
                            WHERE kind = 'ReusableCoverage'
                              AND state = 'COMPLETE'
                              AND (
                                  owner_role IS NOT NULL
                                  OR producer_pid IS NOT NULL
                                  OR sql IS NOT NULL
                                  OR producer_plan IS NOT NULL
                                  OR dependencies IS NOT NULL)
                        ) AS leaked_rows
                    FROM bioseq_cache.entries()
                ) AS observed
            )SQL"));
        bioseq::cache_test::exec(connection, "RESET ROLE");

        bioseq::cache_test::exec(
            connection, "SET ROLE pg_read_all_stats");
        CHECK(
            bioseq::cache_test::integer(
                connection,
                "SELECT count(*) "
                "FROM bioseq_cache.entries() "
                "WHERE kind = 'ReusableCoverage' "
                "  AND state = 'COMPLETE' "
                "  AND owner_role IS NOT NULL "
                "  AND producer_pid IS NOT NULL "
                "  AND sql IS NOT NULL "
                "  AND producer_plan IS NOT NULL "
                "  AND dependencies IS NOT NULL")
            == 2);
        bioseq::cache_test::exec(connection, "RESET ROLE");

        bioseq::cache_test::exec(
            connection,
            "REVOKE EXECUTE ON FUNCTION bioseq_cache.entries() "
            "FROM cache_security_alice, cache_security_monitor; "
            "REVOKE USAGE ON SCHEMA bioseq_cache "
            "FROM cache_security_alice, cache_security_monitor; "
            "DROP ROLE cache_security_monitor");

        invalidate_reusable_entries(connection);
        bioseq::cache_test::exec(
            connection, "SET row_security = on");
        const auto row_security_on_cold =
            connection.exec(security_query, 30s);
        const auto row_security_on_warm =
            connection.exec(security_query, 30s);
        bioseq::cache_test::exec(
            connection, "SET row_security = off");
        const auto row_security_off_cold =
            connection.exec(security_query, 30s);
        const auto row_security_off_warm =
            connection.exec(security_query, 30s);
        CHECK(
            bioseq::postgres_test::exact_row_hash(
                row_security_on_cold)
            == bioseq::postgres_test::exact_row_hash(
                row_security_on_warm));
        CHECK(
            bioseq::postgres_test::exact_row_hash(
                row_security_off_cold)
            == bioseq::postgres_test::exact_row_hash(
                row_security_off_warm));
        const auto row_security_stats =
            bioseq::cache_test::read_cache_stats(connection);
        CHECK(row_security_stats.produced == 2);
        CHECK(row_security_stats.reuse_hits == 2);
        CHECK(row_security_stats.fallbacks == 0);

        invalidate_reusable_entries(connection);
        bioseq::cache_test::exec(
            connection,
            "SELECT bioseq_cache_test.provider_set_security_digest("
            "decode(repeat('11', 32), 'hex'))");
        const auto digest_a_cold =
            connection.exec(security_query, 30s);
        const auto digest_a_warm =
            connection.exec(security_query, 30s);
        bioseq::cache_test::exec(
            connection,
            "SELECT bioseq_cache_test.provider_set_security_digest("
            "decode(repeat('22', 32), 'hex'))");
        const auto digest_b_cold =
            connection.exec(security_query, 30s);
        const auto digest_b_warm =
            connection.exec(security_query, 30s);
        CHECK(
            bioseq::postgres_test::exact_row_hash(digest_a_cold)
            == bioseq::postgres_test::exact_row_hash(digest_a_warm));
        CHECK(
            bioseq::postgres_test::exact_row_hash(digest_b_cold)
            == bioseq::postgres_test::exact_row_hash(digest_b_warm));
        const auto digest_stats =
            bioseq::cache_test::read_cache_stats(connection);
        CHECK(digest_stats.produced == 2);
        CHECK(digest_stats.reuse_hits == 2);
        CHECK(digest_stats.fallbacks == 0);

        bioseq::cache_test::exec(
            connection,
            "RESET row_security; "
            "SELECT bioseq_cache_test.provider_clear_security_digest(); "
            "SELECT bioseq_cache_test.provider_shutdown(); "
            "REVOKE SELECT ON cache_security_rows "
            "FROM cache_security_alice, cache_security_bob; "
            "DROP ROLE cache_security_alice; "
            "DROP ROLE cache_security_bob");
        bioseq::postgres_test::record_migration_probe("regress.cache_reuse_security.semantic_namespace_and_redaction");
    }
}
