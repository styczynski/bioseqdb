/**
 * @file index_admin_update_test.cpp
 * @brief Exercises public index update, policy, and administrative drop behavior.
 */

#include <libpq-fe.h>

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;
import bioseq.tests.integration.index_admin_test_support;

using namespace bioseq_int_tests;
using namespace bioseq_int_tests::index_admin_test_support;

TEST_CASE(
    "update_index('minimizers_per_fragment') builds the per-fragment index",
    "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
  auto ctx = fresh_context({k_table});
  // seed_small_bioseq_column sets bioseq.index.insert_maintenance.enabled = on, so the
  // two default UPDATE_ON_INSERT indexes (minimizers_per_fragment,
  // symbol_counts) are populated incrementally at INSERT time.
  seed_small_bioseq_column(ctx, k_table);

  REQUIRE(index_table_row_count(ctx, "minimizers_per_fragment", k_table) > 0);
  REQUIRE(index_table_row_count(ctx, "symbol_counts", k_table) > 0);

  // Explicit rebuild of the per-fragment index returns the count of
  // sequences (re)indexed — three seeded rows.
  REQUIRE(query_scalar_int(
              ctx, fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                               "'minimizers_per_fragment');",
                               k_table)) == 3);
}

TEST_CASE("update_index rejects an unknown index name",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);

    REQUIRE(sql_fails(
        ctx,
        fmt::format("SELECT bioseq.update_index('{}'::regclass, 'NOT_A_KIND');",
                    k_table)));
}

#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
TEST_CASE("two-stack ntHash update_index accepts DNA5 without scalar fallback",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table = "index_admin_exact_dna5";
    auto ctx = fresh_context({table});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sqlf(ctx,
        "CREATE TABLE {} (pk int, seq0 bioseq.seq('dna5', 64)) USING bioseq;",
        table);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES (1, "
        "(repeat('ACGT', 256) || 'N' || repeat('TGCA', 256) || "
        "'N' || repeat('GATTACA', 128))::bioseq.seq('dna5', 64));",
        table);
    exec_sql(ctx,
        "SET bioseq.aligner.minimizer.algorithm = 'two_stack_nthash';");
    exec_sql(ctx,
        "SET bioseq.aligner.chaining.algorithm = 'simd_minimap2';");
    exec_sql(ctx,
        "SET bioseq.aligner.base_alignment.algorithm = 'block_aligner';");

    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_fragment');",
        table)) == 1);

    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_sequence');",
        table)) == 1);
}

TEST_CASE("bounded ntHash update_index publishes DNA5 for both minimizer layouts",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    struct publication_route {
        std::string_view table;
        std::string_view index_name;
    };
    constexpr std::array<publication_route, 2> routes{{
        {"index_admin_bounded_nthash_grouped_fragment", "minimizers_per_fragment"},
        {"index_admin_bounded_nthash_grouped_sequence", "minimizers_per_sequence"},
    }};

    for (const publication_route& route : routes) {
        DYNAMIC_SECTION("grouped " << route.index_name) {
            auto ctx = fresh_context({route.table});
            exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
            exec_sql(ctx, "SET bioseq.index.async_persistence.enabled = off;");
            exec_sql(ctx, "SET bioseq.index.recompute_async_persistence.enabled = off;");
            exec_sql(ctx,
                "SET bioseq.aligner.grouped_soa_generation.enabled = on;");
            exec_sql(ctx,
                "SET bioseq.aligner.minimizer.algorithm = 'bounded_nthash_simd';");
            exec_sqlf(ctx,
                "CREATE TABLE {} "
                "(id int, seq bioseq.seq('dna5', 64)) USING bioseq;",
                route.table);
            exec_sqlf(ctx,
                "INSERT INTO {} VALUES "
                "(1, (repeat('ACGTN', 13113) || 'A')::"
                "bioseq.seq('dna5', 64));",
                route.table);

            // DNA5 is now supported by the selected bounded producer. Retain
            // the locator-backed fixture and check real index publication;
            // do not require minimizer equality with another algorithm.
            REQUIRE(only_column_row_number(ctx, route.table) > 0);
            REQUIRE(query_scalar_int(ctx, fmt::format(
                "SELECT bioseq.update_index('{}'::regclass, '{}');",
                route.table, route.index_name)) == 1);
            REQUIRE(index_table_row_count(
                ctx, route.index_name, route.table) > 0);
            REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
        }
    }
}
#endif

TEST_CASE("update_index accepts text and explicit regclass for the table arg",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);

    // Quoted text form — PG implicitly casts text -> regclass via regclassin.
    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.update_index('{}', "
                            "'minimizers_per_fragment');",
                            k_table)) == 3);

    // Explicit ::regclass cast form.
    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                            "'minimizers_per_fragment');",
                            k_table)) == 3);
}

TEST_CASE("update_index rejects non-bioseq-AM tables",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view k_heap_table = "index_admin_heap_t";
    auto ctx = fresh_context({k_heap_table});

    // Plain heap-AM table with a bioseq.seq column. The chunked-storage
    // reindex path is only reachable from the bioseq columnar AM, so
    // update_index must refuse a heap relation.
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(k_heap_table),
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = false,
        },
        {seq_row{.pk = 1, .sequences = {"ACGTACGTACGTACGT"}}});

    REQUIRE(sql_fails(
        ctx,
        fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                    "'minimizers_per_fragment');",
                    k_heap_table)));
}

TEST_CASE("set_index_policy toggles update_policy and supports per-sequence on-insert",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);

    // Helper: read the current update_policy of one index from list_metadata.
    auto policy_of = [&ctx](std::string_view index_name) {
        return query_scalar(
            ctx,
            fmt::format("SELECT update_policy FROM bioseq.list_metadata() "
                        " WHERE table_name = '{}'::regclass "
                        "   AND index_name = '{}' LIMIT 1;",
                        k_table, index_name));
    };

    // symbol_counts defaults to UPDATE_ON_INSERT; flip it to UPDATE_MANUALLY.
    exec_sqlf(ctx,
              "SELECT bioseq.set_index_policy('{}'::regclass, 'symbol_counts', "
              "'UPDATE_MANUALLY');",
              k_table);
    REQUIRE(policy_of("symbol_counts") == "UPDATE_MANUALLY");

    // Flip it back to UPDATE_ON_INSERT.
    exec_sqlf(ctx,
              "SELECT bioseq.set_index_policy('{}'::regclass, 'symbol_counts', "
              "'UPDATE_ON_INSERT');",
              k_table);
    REQUIRE(policy_of("symbol_counts") == "UPDATE_ON_INSERT");

    // minimizers_per_sequence is opt-in UPDATE_ON_INSERT. This must be
    // accepted so asynchronous recompute persistence and batch load can honor
    // the table policy instead of hard-wiring minimizers_per_fragment.
    exec_sqlf(ctx,
              "SELECT bioseq.set_index_policy('{}'::regclass, "
              "'minimizers_per_fragment', 'UPDATE_MANUALLY');",
              k_table);
    exec_sqlf(ctx,
              "SELECT bioseq.set_index_policy('{}'::regclass, "
              "'minimizers_per_sequence', 'UPDATE_ON_INSERT');",
              k_table);
    REQUIRE(policy_of("minimizers_per_fragment") == "UPDATE_MANUALLY");
    REQUIRE(policy_of("minimizers_per_sequence") == "UPDATE_ON_INSERT");
}

TEST_CASE("UPDATE_ON_INSERT can build minimizers_per_sequence without per-fragment fallback",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"index_admin_sequence_minimizer_policy"});
    exec_sql(ctx, "CREATE TABLE index_admin_sequence_minimizer_policy "
                  "(header text, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(ctx, "SELECT bioseq.set_index_policy("
                  "'index_admin_sequence_minimizer_policy'::regclass, "
                  "'minimizers_per_fragment', 'UPDATE_MANUALLY');");
    exec_sql(ctx, "SELECT bioseq.set_index_policy("
                  "'index_admin_sequence_minimizer_policy'::regclass, "
                  "'minimizers_per_sequence', 'UPDATE_ON_INSERT');");
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = on;");
    exec_sql(ctx, "INSERT INTO index_admin_sequence_minimizer_policy VALUES "
                  "('minimizers_per_sequence', "
                  "repeat('ACGT', 2000)::bioseq.seq('dna4', 64));");

    REQUIRE(index_table_row_count(ctx, "minimizers_per_fragment",
                                  "index_admin_sequence_minimizer_policy") == 0);
    REQUIRE(index_table_row_count(ctx, "minimizers_per_sequence",
                                  "index_admin_sequence_minimizer_policy") > 0);
}

TEST_CASE("update_index('minimizers_per_fragment') is idempotent at the content level",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);

    // Run the rebuild twice; the per-fragment chunk row count must be stable
    // (the rebuild clears the index then repopulates from identical input).
    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                            "'minimizers_per_fragment');",
                            k_table)) == 3);
    const auto after_first =
        index_table_row_count(ctx, "minimizers_per_fragment", k_table);

    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                            "'minimizers_per_fragment');",
                            k_table)) == 3);
    const auto after_second =
        index_table_row_count(ctx, "minimizers_per_fragment", k_table);

    REQUIRE(after_first > 0);
    REQUIRE(after_first == after_second);
}

TEST_CASE("update_index on an empty bioseq table returns 0 and list_metadata "
          "still lists kinds",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    // Create an empty USING bioseq table (no rows).
    exec_sqlf(ctx,
              "CREATE TABLE {} (pk int, seq0 bioseq.seq('dna4', 32)) "
              "USING bioseq;",
              k_table);

    // No rows -> no sequences (re)indexed -> 0.
    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                            "'minimizers_per_fragment');",
                            k_table)) == 0);

    // list_metadata still lists the three production kinds with
    // storage_size '0 bytes'.
    const auto rows = query_rows(
        ctx,
        fmt::format("SELECT index_name, storage_size "
                    "FROM bioseq.list_metadata() "
                    " WHERE table_name = '{}'::regclass "
                    "   AND index_name <> 'test_table_rowcount' "
                    " ORDER BY index_name;",
                    k_table));
    REQUIRE(rows.size() == 3);
    REQUIRE(rows[0][0].value() == "minimizers_per_fragment");
    REQUIRE(rows[1][0].value() == "minimizers_per_sequence");
    REQUIRE(rows[2][0].value() == "symbol_counts");
    for (const auto& row : rows) {
        REQUIRE(row.size() == 2);
        REQUIRE(row[1].value() == "0 bytes");
    }
}

TEST_CASE("drop_all_index_tables applies the exact table selector and fires "
          "drop events",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context();
    exec_sql(ctx, "BEGIN;");
    exec_sql(ctx,
        "CREATE TABLE public.native_drop_event_log ("
        "object_type text, schema_name text, object_name text);");
    exec_sql(ctx,
        "CREATE FUNCTION public.capture_native_index_drops() "
        "RETURNS event_trigger LANGUAGE plpgsql AS $capture$ "
        "BEGIN "
        "  INSERT INTO public.native_drop_event_log "
        "      (object_type, schema_name, object_name) "
        "  SELECT object_type, schema_name, object_name "
        "  FROM pg_event_trigger_dropped_objects(); "
        "END "
        "$capture$;");
    exec_sql(ctx,
        "CREATE EVENT TRIGGER capture_native_index_drops "
        "ON sql_drop WHEN TAG IN ('DROP TABLE') "
        "EXECUTE FUNCTION public.capture_native_index_drops();");

    exec_sql(ctx,
        "CREATE TABLE bioseq_internal.seqidx_native_plain (value int);"
        "CREATE TABLE bioseq_internal.seqidxtab_native_plain (value int);"
        "CREATE TABLE bioseq_internal.\"seqidx_native Ω\"\"quoted\" "
        "    (value int);"
        "CREATE VIEW public.native_drop_dependency AS "
        "    SELECT value FROM bioseq_internal.seqidx_native_plain;"
        "CREATE VIEW bioseq_internal.seqidx_native_view_survives AS "
        "    SELECT 1 AS value;"
        "CREATE TABLE bioseq_internal.seqidx (value int);"
        "CREATE TABLE bioseq_internal.seqidxnear (value int);"
        "CREATE TABLE bioseq_internal.seqidxtab (value int);"
        "CREATE TABLE bioseq_internal.seqidxtabnear (value int);"
        "CREATE TABLE bioseq_internal.seqid (value int);"
        "CREATE TABLE bioseq_internal.seqid_ (value int);"
        "CREATE TABLE bioseq_internal.seqid_native_near (value int);");

    exec_sql(ctx, "SELECT bioseq.drop_all_index_tables();");
    exec_sql(ctx, "SELECT bioseq.drop_all_index_tables();");

    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) "
        "FROM pg_catalog.pg_class AS c "
        "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
        "WHERE n.nspname = 'bioseq_internal' "
        "AND c.relname::text IN ("
        "  'seqidx_native_plain',"
        "  'seqidxtab_native_plain',"
        "  'seqidx_native Ω\"quoted'"
        ");") == 0);
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) "
        "FROM pg_catalog.pg_class AS c "
        "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
        "WHERE n.nspname = 'bioseq_internal' "
        "AND c.relname::text IN ("
        "  'seqidx', 'seqidxnear', 'seqidxtab', 'seqidxtabnear',"
        "  'seqid', 'seqid_', 'seqid_native_near'"
        ");") == 7);
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) "
        "FROM pg_catalog.pg_class AS c "
        "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
        "WHERE n.nspname = 'bioseq_internal' "
        "AND c.relname = 'seqidx_native_view_survives' "
        "AND c.relkind = 'v';") == 1);
    REQUIRE(query_scalar_int(ctx,
        "SELECT (to_regclass('public.native_drop_dependency') IS NULL)::int;"
        ) == 1);
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) FROM public.native_drop_event_log "
        "WHERE object_type = 'table' "
        "AND schema_name = 'bioseq_internal' "
        "AND object_name IN ("
        "  'seqidx_native_plain',"
        "  'seqidxtab_native_plain',"
        "  'seqidx_native Ω\"quoted'"
        ");") == 3);
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) FROM public.native_drop_event_log "
        "WHERE object_type = 'view' "
        "AND schema_name = 'public' "
        "AND object_name = 'native_drop_dependency';") == 1);
    exec_sql(ctx, "ROLLBACK;");
}

TEST_CASE("prefix index clearing selects the exact prefix and preserves "
          "DROP TABLE CASCADE behavior",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table = "native_prefix_selector_source";
    auto ctx = fresh_context({table});
    seed_small_bioseq_column(ctx, table);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_fragment');", table)) == 3);

    const std::string sidecar_name = query_scalar(ctx,
        "SELECT c.relname "
        "FROM pg_catalog.pg_class AS c "
        "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
        "WHERE n.nspname = 'bioseq_internal' "
        "AND c.relkind = 'r' "
        "AND starts_with(c.relname::text, 'seqidx_mpf_') "
        "ORDER BY c.oid LIMIT 1;");
    constexpr std::string_view physical_name_stem = "seqidx_mpf_";
    constexpr std::size_t identity_hash_hex_size = 16;
    const std::size_t clear_prefix_size =
        physical_name_stem.size() + identity_hash_hex_size + 1;
    REQUIRE(sidecar_name.size() > clear_prefix_size);
    const std::string selected_prefix =
        sidecar_name.substr(0, clear_prefix_size);
    REQUIRE(selected_prefix.back() == '_');
    const std::string selected_name =
        fmt::format("{}native Ω\"quoted", selected_prefix);
    const std::string selected_identifier =
        fmt::format("\"{}native Ω\"\"quoted\"", selected_prefix);
    const std::string near_miss_name =
        fmt::format("{}x_native_near", selected_prefix.substr(
            0, selected_prefix.size() - 1));

    exec_sql(ctx, "BEGIN;");
    exec_sqlf(ctx,
        "CREATE TABLE bioseq_internal.{} (value int);"
        "CREATE VIEW public.native_prefix_dependency AS "
        "SELECT value FROM bioseq_internal.{};"
        "CREATE TABLE bioseq_internal.{} (value int);",
        selected_identifier,
        selected_identifier,
        near_miss_name);

    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_fragment');", table)) == 3);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT (to_regclass(format("
        "'bioseq_internal.%I', '{}')) IS NULL)::int;",
        selected_name)) == 1);
    REQUIRE(query_scalar_int(ctx,
        "SELECT (to_regclass('public.native_prefix_dependency') "
        "IS NULL)::int;") == 1);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT (to_regclass('bioseq_internal.{}') "
        "IS NOT NULL)::int;",
        near_miss_name)) == 1);
    exec_sql(ctx, "ROLLBACK;");
}

TEST_CASE("drop_all_index_tables preserves PostgreSQL ownership checks",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context();
    exec_sql(ctx, "BEGIN;");
    exec_sql(ctx,
        "CREATE ROLE native_drop_owner NOLOGIN;"
        "CREATE ROLE native_drop_caller NOLOGIN;"
        "CREATE TABLE bioseq_internal.seqidx_native_owned (value int);"
        "ALTER TABLE bioseq_internal.seqidx_native_owned "
        "    OWNER TO native_drop_owner;"
        "GRANT USAGE ON SCHEMA bioseq, bioseq_internal "
        "    TO native_drop_caller;"
        "GRANT EXECUTE ON FUNCTION bioseq.drop_all_index_tables() "
        "    TO native_drop_caller;"
        "SET LOCAL ROLE native_drop_caller;");
    const auto denied =
        sql_error(ctx, "SELECT bioseq.drop_all_index_tables();");
    exec_sql(ctx, "ROLLBACK;");

    REQUIRE(denied.sqlstate == "42501");
    REQUIRE(denied.contains("must be owner"));
}

TEST_CASE("drop_all_index_tables rolls back an earlier drop when the second "
          "selected drop fails",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context();
    exec_sql(ctx, "BEGIN;");
    exec_sql(ctx,
        "CREATE TABLE public.native_second_drop_counter (call_no int NOT NULL);"
        "INSERT INTO public.native_second_drop_counter VALUES (0);"
        "CREATE TABLE bioseq_internal.seqidx_native_atomic_a (value int);"
        "CREATE TABLE bioseq_internal.seqidx_native_atomic_b (value int);");
    exec_sql(ctx,
        "CREATE FUNCTION public.fail_second_native_index_drop() "
        "RETURNS event_trigger LANGUAGE plpgsql AS $failure$ "
        "DECLARE next_call int; "
        "BEGIN "
        "  IF EXISTS ("
        "    SELECT 1 FROM pg_event_trigger_dropped_objects() "
        "    WHERE original AND object_type = 'table' "
        "    AND schema_name = 'bioseq_internal' "
        "    AND object_name IN ("
        "      'seqidx_native_atomic_a', 'seqidx_native_atomic_b')) "
        "  THEN "
        "    UPDATE public.native_second_drop_counter "
        "    SET call_no = call_no + 1 "
        "    RETURNING call_no INTO next_call; "
        "    IF next_call = 2 THEN "
        "      RAISE EXCEPTION 'forced second selected drop failure' "
        "      USING ERRCODE = 'XX000'; "
        "    END IF; "
        "  END IF; "
        "END "
        "$failure$;");
    exec_sql(ctx,
        "CREATE EVENT TRIGGER fail_second_native_index_drop "
        "ON sql_drop WHEN TAG IN ('DROP TABLE') "
        "EXECUTE FUNCTION public.fail_second_native_index_drop();");

    const long long first_oid = query_scalar_int(ctx,
        "SELECT 'bioseq_internal.seqidx_native_atomic_a'::regclass"
        "::oid::bigint;");
    const long long second_oid = query_scalar_int(ctx,
        "SELECT 'bioseq_internal.seqidx_native_atomic_b'::regclass"
        "::oid::bigint;");
    exec_sql(ctx, "SAVEPOINT before_native_index_drop;");
    const auto forced_failure =
        sql_error(ctx, "SELECT bioseq.drop_all_index_tables();");
    exec_sql(ctx, "ROLLBACK TO SAVEPOINT before_native_index_drop;");

    REQUIRE(forced_failure.sqlstate == "XX000");
    REQUIRE(forced_failure.contains("forced second selected drop failure"));
    REQUIRE(query_scalar_int(ctx,
        "SELECT 'bioseq_internal.seqidx_native_atomic_a'::regclass"
        "::oid::bigint;") == first_oid);
    REQUIRE(query_scalar_int(ctx,
        "SELECT 'bioseq_internal.seqidx_native_atomic_b'::regclass"
        "::oid::bigint;") == second_oid);
    REQUIRE(query_scalar_int(ctx,
        "SELECT call_no FROM public.native_second_drop_counter;") == 0);
    exec_sql(ctx, "ROLLBACK;");
}

TEST_CASE("prefix index clearing rejects a wrong relation kind and rolls back "
          "prior drops",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table = "native_prefix_rollback_source";
    auto ctx = fresh_context({table});
    seed_small_bioseq_column(ctx, table);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_fragment');", table)) == 3);

    const std::string sidecar_name = query_scalar(ctx,
        "SELECT c.relname "
        "FROM pg_catalog.pg_class AS c "
        "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
        "WHERE n.nspname = 'bioseq_internal' "
        "AND c.relkind = 'r' "
        "AND starts_with(c.relname::text, 'seqidx_mpf_') "
        "ORDER BY c.oid LIMIT 1;");
    const long long original_oid = query_scalar_int(ctx, fmt::format(
        "SELECT 'bioseq_internal.{}'::regclass::oid::bigint;",
        sidecar_name));
    const long long original_rows = query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM bioseq_internal.{};", sidecar_name));
    REQUIRE(original_rows > 0);
    const std::string wrong_kind_name =
        fmt::format("{}_wrong_relkind", sidecar_name);

    exec_sql(ctx, "BEGIN;");
    exec_sqlf(ctx,
        "CREATE VIEW bioseq_internal.{} AS SELECT 1 AS value;",
        wrong_kind_name);
    const auto wrong_kind = sql_error(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_fragment');", table));
    exec_sql(ctx, "ROLLBACK;");

    REQUIRE(wrong_kind.sqlstate == "42809");
    REQUIRE(wrong_kind.contains("is not a table"));
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT 'bioseq_internal.{}'::regclass::oid::bigint;",
        sidecar_name)) == original_oid);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM bioseq_internal.{};",
        sidecar_name)) == original_rows);
}
