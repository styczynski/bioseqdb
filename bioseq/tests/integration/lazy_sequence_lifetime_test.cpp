/**
 * @file lazy_sequence_lifetime_test.cpp
 * @brief Exercises persistent ropes, source faults, catalogs, and SQL packing.
 */

#include <cstdio>
#include <libpq-fe.h>

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;
import bioseq.tests.integration.lazy_sequence_slicing_acceptance_test_support;

using namespace bioseq_int_tests;
using namespace bioseq_int_tests::lazy_sequence_slicing_acceptance_test_support;

TEST_CASE("persistent ropes survive PLpgSQL transfer heap flatten and ERROR unwind",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context({"sequence_acceptance_rope_heap"});
    exec_sql(ctx,
        "CREATE FUNCTION pg_temp.sequence_acceptance_build_rope(n int) "
        "RETURNS bioseq.seq LANGUAGE plpgsql VOLATILE AS $$ "
        "DECLARE value bioseq.seq := ''::bioseq.seq; "
        "BEGIN "
        "  FOR i IN 1..n LOOP "
        "    value := value OPERATOR(bioseq.||) "
        "      substr('ACGT', ((i - 1) % 4) + 1, 1); "
        "  END LOOP; "
        "  RETURN value; "
        "END $$; "
        "CREATE FUNCTION pg_temp.sequence_acceptance_build_rope_error(n int) "
        "RETURNS bioseq.seq LANGUAGE plpgsql VOLATILE AS $$ "
        "DECLARE value bioseq.seq; "
        "BEGIN "
        "  value := pg_temp.sequence_acceptance_build_rope(n); "
        "  RAISE EXCEPTION 'injected rope failure'; "
        "END $$;");

    const auto expanded = query_one(
        ctx,
        "SELECT bioseq.len(s), "
        "       bioseq_internal.sequence_piece_count(s), "
        "       s[4093:4101]::text, "
        "       bioseq.reverse(bioseq.reverse(s)) "
        "         OPERATOR(bioseq.=) s "
        "FROM LATERAL ("
        " SELECT pg_temp.sequence_acceptance_build_rope(8192) AS s OFFSET 0"
    ") AS built;");
    REQUIRE(expanded[0] == "8192");
    REQUIRE(expanded[1].has_value());
    REQUIRE(std::stoll(expanded[1].value()) <= 16);
    REQUIRE(expanded[2] == "CGTACGTA");
    REQUIRE(expanded[3] == "t");

    exec_sql(ctx,
        "PREPARE sequence_acceptance_rope_plan(int) AS "
        " SELECT bioseq.len(pg_temp.sequence_acceptance_build_rope($1));");
    REQUIRE(query_scalar_int(
        ctx, "EXECUTE sequence_acceptance_rope_plan(4097);") == 4097);
    exec_sql(ctx, "DEALLOCATE sequence_acceptance_rope_plan;");

    const auto injected = sql_error(
        ctx, "SELECT pg_temp.sequence_acceptance_build_rope_error(4097);");
    REQUIRE(injected.sqlstate == "P0001");
    REQUIRE(query_scalar_int(
        ctx,
        "SELECT bioseq.len(pg_temp.sequence_acceptance_build_rope(4097));") == 4097);

    exec_sql(ctx,
        "CREATE TABLE sequence_acceptance_rope_heap (value bioseq.seq); "
        "INSERT INTO sequence_acceptance_rope_heap "
        "VALUES (pg_temp.sequence_acceptance_build_rope(8192));");
    const auto flattened = query_one(
        ctx,
        "SELECT bioseq.len(value), value[4093:4101]::text "
        "FROM sequence_acceptance_rope_heap;");
    REQUIRE(flattened[0] == "8192");
    REQUIRE(flattened[1] == "CGTACGTA");
}

#ifdef BIOSEQ_TEST_INDEXES

TEST_CASE("source-domain faults unwind every partial lifetime stage",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context({"sequence_acceptance_source_faults"});
    exec_sql(ctx,
        "CREATE TABLE sequence_acceptance_source_faults "
        " (id int, value bioseq.seq('dna4',32)) USING bioseq; "
        "INSERT INTO sequence_acceptance_source_faults VALUES "
        " (1, repeat('ACGT',4096)::bioseq.seq('dna4',32));");

    const auto unknown = sql_error(
        ctx,
        "SELECT bioseq_internal.arm_sequence_source_failure("
        " 'not_a_failure_point');");
    REQUIRE(unknown.sqlstate == "22023");

    const auto translated_cpp = sql_error(
        ctx,
        "SELECT bioseq_internal.arm_sequence_source_failure("
        " 'diagnostic_std_exception');");
    REQUIRE(translated_cpp.sqlstate == "XX000");
    REQUIRE(translated_cpp.contains(
        "bioseq sequence callback raised a C++ exception"));
    REQUIRE(translated_cpp.contains(
        "injected sequence-source diagnostic C++ exception"));

    constexpr std::array failure_points{
        "provider_after_registry_control",
        "provider_after_provider_control",
        "domain_after_control",
        "domain_after_context",
        "domain_after_owner",
        "domain_after_snapshot",
        "domain_after_caches",
        "close_before_caches",
        "close_before_snapshot",
        "close_before_before_locks",
        "close_before_locks",
        "close_before_after_locks",
    };
    for (const char* failure_point : failure_points) {
        CAPTURE(failure_point);
        exec_sql(ctx,
            "SELECT bioseq_internal.reset_sequence_source_counters();");
        exec_sql(ctx, fmt::format(
            "SELECT bioseq_internal.arm_sequence_source_failure('{}');",
            failure_point));
        const auto injected = sql_error(
            ctx,
            "SELECT bioseq.to_text(value[4093:4101]) "
            "FROM sequence_acceptance_source_faults WHERE id=1;");
        REQUIRE(injected.sqlstate == "XX000");
        REQUIRE(injected.contains(
            "injected PostgreSQL sequence source-domain failure"));
        exec_sql(ctx,
            "SELECT bioseq_internal.disarm_sequence_source_failure();");
        const auto counters = query_one(
            ctx,
            "SELECT (c->>'fault_injections')::bigint, "
            "       (c->>'live_registries')::bigint "
            "       + (c->>'live_providers')::bigint "
            "       + (c->>'live_domains')::bigint "
            "       + (c->>'live_leases')::bigint "
            "       + (c->>'live_cache_sets')::bigint "
            "       + (c->>'live_snapshots')::bigint "
            "FROM (SELECT "
            " bioseq_internal.sequence_source_counters() AS c) AS q;");
        REQUIRE(counters[0] == "1");
        REQUIRE(counters[1] == "0");
    }

    exec_sql(ctx,
        "SELECT bioseq_internal.reset_sequence_source_counters();");
    REQUIRE(query_scalar(
        ctx,
        "SELECT bioseq_internal.sequence_source_wrong_thread_probe();")
        == "t");
    const auto wrong_thread = query_one(
        ctx,
        "SELECT (c->>'wrong_thread_rejections')::bigint, "
        "       (c->>'fault_injections')::bigint, "
        "       (c->>'live_registries')::bigint "
        "       + (c->>'live_providers')::bigint "
        "       + (c->>'live_domains')::bigint "
        "       + (c->>'live_cache_sets')::bigint "
        "       + (c->>'live_snapshots')::bigint "
        "FROM (SELECT "
        " bioseq_internal.sequence_source_counters() AS c) AS q;");
    REQUIRE(wrong_thread[0] == "1");
    REQUIRE(wrong_thread[1] == "0");
    REQUIRE(wrong_thread[2] == "0");

    exec_sql(ctx,
        "SELECT bioseq_internal.reset_sequence_source_counters();");
    REQUIRE(query_scalar(
        ctx,
        "SELECT bioseq_internal.sequence_source_fault_contract_probe();")
        == "t");
    const auto pinned_faults = query_one(
        ctx,
        "SELECT (c->>'refcount_faults')::bigint, "
        "       (c->>'lease_faults')::bigint "
        "FROM (SELECT "
        " bioseq_internal.sequence_source_counters() AS c) AS q;");
    REQUIRE(pinned_faults[0] == "1");
    REQUIRE(pinned_faults[1] == "1");

    exec_sql(ctx,
        "SELECT bioseq_internal.reset_sequence_source_counters(); "
        "DO $$ BEGIN "
        "  PERFORM bioseq_internal.sequence_source_pre_prepare_probe(); "
        "  RAISE EXCEPTION 'PRE_PREPARE probe did not reject'; "
        "EXCEPTION WHEN feature_not_supported THEN "
        "  IF SQLERRM NOT LIKE "
        "    'cannot PREPARE TRANSACTION with live bioseq source domains%' "
        "  THEN RAISE; END IF; "
        "END $$;");
    const auto pre_prepare = query_one(
        ctx,
        "SELECT (c->>'pre_prepare_rejections')::bigint, "
        "       (c->>'live_registries')::bigint "
        "       + (c->>'live_domains')::bigint "
        "       + (c->>'live_cache_sets')::bigint "
        "       + (c->>'live_snapshots')::bigint "
        "FROM (SELECT "
        " bioseq_internal.sequence_source_counters() AS c) AS q;");
    REQUIRE(pre_prepare[0] == "1");
    REQUIRE(pre_prepare[1] == "0");
}

TEST_CASE("source read ERROR crosses every sequence callback boundary",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context({
        "sequence_acceptance_source_read_faults",
        "sequence_acceptance_source_read_heap",
    });
    exec_sql(ctx,
        "CREATE TABLE sequence_acceptance_source_read_faults "
        " (id int, value bioseq.seq('dna4',32)) USING bioseq; "
        "INSERT INTO sequence_acceptance_source_read_faults VALUES "
        " (1, repeat('ACGT',4096)::bioseq.seq('dna4',32)), "
        " (2, repeat('TGCA',4096)::bioseq.seq('dna4',32)); "
        "CREATE TABLE sequence_acceptance_source_read_heap "
        " (value bioseq.seq('dna4',32));");
    exec_sql(ctx,
        "SET bioseq.planner.sequence_sort_scan.enabled = on; "
        "SET bioseq.aligner.vulkan.enabled = off;");

    const std::array<const char*, 5> callback_queries{
        /* One canonical-view callback. */
        "SELECT bioseq.to_text(value[4093:4101]) "
        "FROM sequence_acceptance_source_read_faults WHERE id=1;",
        /* Two-view compare callback. */
        "SELECT a.value[4093:4101] OPERATOR(bioseq.=) "
        "       b.value[4093:4101] "
        "FROM sequence_acceptance_source_read_faults AS a "
        "CROSS JOIN sequence_acceptance_source_read_faults AS b "
        "WHERE a.id=1 AND b.id=2;",
        /* One-rank scalar callback. */
        "SELECT value[4097]::text "
        "FROM sequence_acceptance_source_read_faults WHERE id=1;",
        /* PostgreSQL heap datumCopy invokes the EOH flatten callback. */
        "INSERT INTO sequence_acceptance_source_read_heap "
        "SELECT value[4093:4101] "
        "FROM sequence_acceptance_source_read_faults WHERE id=1;",
        /* BatchSort canonical-view admission/comparison callback. */
        "SELECT id FROM sequence_acceptance_source_read_faults ORDER BY value;",
    };
    for (const char* query : callback_queries) {
        CAPTURE(query);
        exec_sql(ctx,
            "SELECT bioseq_internal.reset_sequence_source_counters(); "
            "SELECT bioseq_internal.arm_sequence_source_failure("
            " 'read_before_scanner');");
        const auto injected = sql_error(ctx, query);
        REQUIRE(injected.sqlstate == "XX000");
        REQUIRE(injected.contains(
            "injected PostgreSQL sequence source-domain failure"));
        exec_sql(ctx,
            "SELECT bioseq_internal.disarm_sequence_source_failure();");
        const auto counters = query_one(
            ctx,
            "SELECT (c->>'fault_injections')::bigint, "
            "       (c->>'read_fault_injections')::bigint, "
            "       (c->>'live_registries')::bigint "
            "       + (c->>'live_providers')::bigint "
            "       + (c->>'live_domains')::bigint "
            "       + (c->>'live_leases')::bigint "
            "       + (c->>'live_cache_sets')::bigint "
            "       + (c->>'live_snapshots')::bigint "
            "FROM (SELECT "
            " bioseq_internal.sequence_source_counters() AS c) AS q;");
        REQUIRE(counters[0] == "1");
        REQUIRE(counters[1] == "1");
        REQUIRE(counters[2] == "0");
    }
    REQUIRE(query_scalar_int(
        ctx, "SELECT count(*) FROM sequence_acceptance_source_read_heap;") == 0);
}

#endif

TEST_CASE("fresh install catalogs expose exact public objects",
          "[component:bioseq][layer:integration][feature:extension_lifecycle][resource:postgres]") {
    connection_test_context admin;
    const auto database = fmt::format(
        "bioseq_sequence_acceptance_fresh_install_{}",
        query_scalar_int(admin, "SELECT pg_backend_pid();"));
    const auto cleanup_sql = fmt::format(
        "DROP DATABASE IF EXISTS {} WITH (FORCE);", database);
    database_cleanup_guard cleanup{admin.raw(), cleanup_sql.c_str()};
    exec_sql(admin, cleanup_sql);
    exec_sql(admin, fmt::format("CREATE DATABASE {};", database));

    row_t catalog;
    row_t runtime;
    {
        connection_test_context ctx(database);
        exec_sql(ctx, "CREATE EXTENSION bioseq;");
        catalog = query_one(
            ctx,
            "SELECT to_regtype('bioseq.seq') IS NOT NULL, "
            "       to_regtype('bioseq.letter') IS NOT NULL, "
            "       (SELECT typsubscript <> 0 FROM pg_catalog.pg_type "
            "        WHERE oid='bioseq.seq'::regtype), "
            "       (SELECT typlen=2 FROM pg_catalog.pg_type "
            "        WHERE oid='bioseq.letter'::regtype), "
            "       to_regprocedure('bioseq.len(bioseq.seq)') "
            "         IS NOT NULL, "
            "       to_regprocedure('bioseq.reverse(bioseq.seq)') "
            "         IS NOT NULL, "
            "       to_regprocedure("
            "         'bioseq.reverse_complement(bioseq.seq)') "
            "         IS NOT NULL, "
            "       to_regprocedure('bioseq.to_text(bioseq.seq)') "
            "         IS NOT NULL, "
            "       to_regprocedure('bioseq.to_array(bioseq.seq)') "
            "         IS NOT NULL, "
            "       to_regprocedure("
            "         'bioseq.from_array(bioseq.letter[],integer)') "
            "         IS NOT NULL, "
            "       to_regoperator('bioseq.||(bioseq.seq,bioseq.seq)') "
            "         IS NOT NULL, "
            "       to_regoperator('bioseq.||(bioseq.seq,text)') "
            "         IS NOT NULL, "
            "       to_regoperator('bioseq.||(text,bioseq.seq)') "
            "         IS NOT NULL, "
            "       to_regoperator('bioseq.||(bioseq.seq,character varying)') "
            "         IS NOT NULL, "
            "       to_regoperator('bioseq.||(character varying,bioseq.seq)') "
            "         IS NOT NULL, "
            "       to_regoperator('bioseq.||(bioseq.seq,text[])') "
            "         IS NOT NULL, "
            "       to_regoperator('bioseq.||(text[],bioseq.seq)') "
            "         IS NOT NULL, "
            "       to_regoperator("
            "         'bioseq.||(bioseq.seq,bioseq.letter[])') "
            "         IS NOT NULL, "
            "       to_regoperator("
            "         'bioseq.||(bioseq.letter[],bioseq.seq)') "
            "         IS NOT NULL, "
            "       to_regoperator('bioseq.=(bioseq.seq,bioseq.seq)') "
            "         IS NOT NULL, "
            "       to_regoperator("
            "         'bioseq.=(bioseq.letter,bioseq.letter)') "
            "         IS NOT NULL, "
            "       (SELECT count(*)=9 FROM pg_catalog.pg_operator o "
            "        JOIN pg_catalog.pg_namespace n ON n.oid=o.oprnamespace "
            "        WHERE n.nspname='bioseq' AND o.oprname='||'), "
            "       (SELECT count(*)=2 FROM pg_catalog.pg_cast c "
            "        WHERE c.castsource='bioseq.seq'::regtype "
            "          AND c.casttarget IN ('text'::regtype,'varchar'::regtype) "
            "          AND c.castcontext='a'), "
            "       (SELECT count(*)=1 FROM pg_catalog.pg_cast c "
            "        WHERE c.castsource='bioseq.seq'::regtype "
            "          AND c.casttarget='bioseq.seq'::regtype "
            "          AND c.castcontext='a'), "
            "       (SELECT count(*)=1 FROM pg_catalog.pg_cast c "
            "        WHERE c.castsource='bioseq.letter'::regtype "
            "          AND c.casttarget='bioseq.letter'::regtype "
            "          AND c.castcontext='a'), "
            "       (SELECT count(*)=1 FROM pg_catalog.pg_cast c "
            "        WHERE c.castsource='bioseq.letter'::regtype "
            "          AND c.casttarget='text'::regtype "
            "          AND c.castcontext='e'), "
            "       (SELECT count(*)=4 FROM pg_catalog.pg_opclass c "
            "        JOIN pg_catalog.pg_namespace n ON n.oid=c.opcnamespace "
            "        WHERE n.nspname='bioseq' AND c.opcname IN "
            "          ('seq_ops','seq_hash_ops',"
            "           'letter_btree_ops','letter_hash_ops')), "
            "       to_regprocedure("
            "         'bioseq.seq_cast(bioseq.seq,integer,boolean)') "
            "         IS NOT NULL, "
            "       to_regprocedure("
            "         'bioseq.letter(bioseq.letter,integer,boolean)') "
            "         IS NOT NULL, "
            "       to_regprocedure("
            "         'bioseq.letter_send(bioseq.letter)') IS NOT NULL, "
            "       to_regprocedure("
            "         'bioseq.letter_cmp(bioseq.letter,bioseq.letter)') "
            "         IS NOT NULL, "
            "       to_regprocedure("
            "         'bioseq.letter_hash(bioseq.letter)') IS NOT NULL, "
            "       to_regprocedure("
            "         'bioseq.seq_hash(bioseq.seq)') IS NOT NULL, "
            "       to_regprocedure("
            "         'bioseq.array_length(bioseq.seq,integer)') "
            "         IS NOT NULL, "
            "       to_regprocedure("
            "         'bioseq.cardinality(bioseq.seq)') IS NOT NULL, "
            "       (SELECT bool_and(p.proisstrict) "
            "        FROM pg_catalog.pg_operator o "
            "        JOIN pg_catalog.pg_namespace n ON n.oid=o.oprnamespace "
            "        JOIN pg_catalog.pg_proc p ON p.oid=o.oprcode "
            "        WHERE n.nspname='bioseq' AND o.oprname='||'), "
            "       pg_typeof('AC'::bioseq.seq('dna4',8)) "
            "         = 'bioseq.seq'::regtype, "
            "       pg_typeof('A'::bioseq.letter('dna4')) "
            "         = 'bioseq.letter'::regtype;");

        exec_sql(ctx, "SET search_path TO pg_catalog, bioseq, public;");
        runtime = query_one(
            ctx,
            "SELECT pg_typeof('AC'::bioseq.seq('dna4',8) || 'GT') "
            "         = 'bioseq.seq'::regtype, "
            "       ('AC'::bioseq.seq('dna4',8) || "
            "         'GT'::bioseq.seq('dna4',64))::text, "
            "       ('AC'::bioseq.seq('dna4',8) || text 'GT')::text, "
            "       (text 'GT' || "
            "         'AC'::bioseq.seq('dna4',8))::text, "
            "       ('AC'::bioseq.seq('dna4',8) || varchar 'GT')::text, "
            "       (varchar 'GT' || "
            "         'AC'::bioseq.seq('dna4',8))::text, "
            "       ('AC'::bioseq.seq('dna4',8) || "
            "         ARRAY['G','T']::text[])::text, "
            "       (ARRAY['G','T']::text[] || "
            "         'AC'::bioseq.seq('dna4',8))::text, "
            "       ('AC'::bioseq.seq('dna4',8) || ARRAY["
            "         'G'::bioseq.letter('dna4'),"
            "         'T'::bioseq.letter('dna4')])::text, "
            "       (ARRAY['G'::bioseq.letter('dna4'),"
            "               'T'::bioseq.letter('dna4')] || "
            "         'AC'::bioseq.seq('dna4',8))::text, "
            "       ('ACGTA'::bioseq.seq('dna4',8))[1:4]::text, "
            "       bioseq.reverse_complement("
            "         'ACGT'::bioseq.seq('dna4',8))::text, "
            "       ('ACGT'::bioseq.seq('dna4',8))[2]::text;");
    }
    exec_sql(admin, fmt::format("DROP DATABASE {};", database));
    cleanup.release();

    for (const auto& value : catalog) {
        REQUIRE(value == "t");
    }
    const std::array runtime_expected{
        "t", "ACGT", "ACGT", "GTAC", "ACGT", "GTAC",
        "ACGT", "GTAC", "ACGT", "GTAC", "CGT", "ACGT", "G"};
    REQUIRE(runtime.size() == runtime_expected.size());
    for (std::size_t index = 0; index < runtime_expected.size(); ++index) {
        REQUIRE(runtime[index] == runtime_expected[index]);
    }
}

TEST_CASE("SQL packing is identical across tails and slice splits",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context();
    exec_sql(
        ctx,
        "CREATE TEMP TABLE sequence_acceptance_packing_flatten ("
        " case_id integer PRIMARY KEY,"
        " expected_text text NOT NULL,"
        " expected_binary bytea NOT NULL,"
        " expected_length bigint NOT NULL,"
        " value bioseq.seq NOT NULL);");

    // The dna4_uint64 profile deliberately exercises the widest packing so
    // fast local iteration still crosses the largest tail boundary.
    require_sequence_acceptance_sql_packing_profile(ctx, "dna4", 64u, 2u, "ACGT");

#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    require_sequence_acceptance_sql_packing_profile(ctx, "dna5", 64u, 3u, "ACGTN");
#endif

#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES)
    for (const std::uint32_t storage_width_bits : {8u, 16u, 32u}) {
        require_sequence_acceptance_sql_packing_profile(
            ctx, "dna4", storage_width_bits, 2u, "ACGT");
        require_sequence_acceptance_sql_packing_profile(
            ctx, "dna5", storage_width_bits, 3u, "ACGTN");
    }
#endif
}
