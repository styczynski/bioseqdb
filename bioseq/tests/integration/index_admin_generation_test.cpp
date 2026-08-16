/**
 * @file index_admin_generation_test.cpp
 * @brief Exercises generation audit, invalidation, clearing, and coordinator contracts.
 */

#include <tests/support/postgres.hpp>
#include <bioseq/pg_build_profile.h>
#include <algorithm>
#include <chrono>
#include <libpq-fe.h>
#include <thread>
#include <utility>

import bioseq.tests.integration.support;
import bioseq.tests.integration.index_admin_test_support;

using namespace bioseq_int_tests;
using namespace bioseq_int_tests::index_admin_test_support;

TEST_CASE("cold no-index audit is metadata-only and detects physical minimizer relations",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"index_admin_no_index_absence"});
    exec_sql(ctx,
        "CREATE TABLE index_admin_no_index_absence "
        "(id int, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(ctx,
        "SET bioseq.index.insert_maintenance.enabled=off; "
        "INSERT INTO index_admin_no_index_absence VALUES "
        "(1, repeat('ACGT', 4096)::bioseq.seq('dna4', 64));");

    const auto absent = query_one(ctx,
        "SELECT target_rows::text, is_target_cardinality_metadata_only::int::text, "
        "source_parent_rows_read::text, source_fragment_rows_read::text, "
        "source_decoded_bp::text, physical_index_relations::text, "
        "persisted_index_rows::text, generation_objects::text "
        "FROM bioseq.inspect_cold_no_index_absence("
        "'index_admin_no_index_absence'::regclass, 'minimizers_per_fragment');");
    REQUIRE(absent.size() == 8);
    REQUIRE(absent[0].value() == "1");
    REQUIRE(absent[1].value() == "1");
    REQUIRE(absent[2].value() == "0");
    REQUIRE(absent[3].value() == "0");
    REQUIRE(absent[4].value() == "0");
    REQUIRE(absent[5].value() == "0");
    REQUIRE(absent[6].value() == "0");
    REQUIRE(absent[7].value() == "0");

    const auto before_sidecars = query_rows(ctx,
        "SELECT c.relname FROM pg_class c JOIN pg_namespace n "
        "ON n.oid=c.relnamespace WHERE n.nspname='bioseq_internal' "
        "AND c.relkind='r' AND c.relname LIKE 'seqidx\\_mpf\\_%' "
        "ESCAPE '\\' ORDER BY c.relname;");
    exec_sql(ctx,
        "SELECT bioseq.update_index('index_admin_no_index_absence'::regclass, "
        "'minimizers_per_fragment');");
    const auto present = query_one(ctx,
        "SELECT physical_index_relations::text, persisted_index_rows::text, "
        "generation_objects::text "
        "FROM bioseq.inspect_cold_no_index_absence("
        "'index_admin_no_index_absence'::regclass, 'minimizers_per_fragment');");
    REQUIRE(present.size() == 3);
    REQUIRE(std::stoll(present[0].value()) > 0);
    REQUIRE(std::stoll(present[1].value()) > 0);
    // The default fragment writer does not publish a generation head;
    // the audit must count its physical rows without inventing one.
    REQUIRE(present[2].value() == "0");

    const auto after_sidecars = query_rows(ctx,
        "SELECT c.relname FROM pg_class c JOIN pg_namespace n "
        "ON n.oid=c.relnamespace WHERE n.nspname='bioseq_internal' "
        "AND c.relkind='r' AND c.relname LIKE 'seqidx\\_mpf\\_%' "
        "ESCAPE '\\' ORDER BY c.relname;");
    std::vector<std::string> prior_names;
    for (const auto& row : before_sidecars) {
        prior_names.push_back(row[0].value());
    }
    std::size_t truncated_relations = 0;
    for (const auto& row : after_sidecars) {
        const std::string& name = row[0].value();
        if (std::ranges::find(prior_names, name) != prior_names.end()) continue;
        REQUIRE(std::ranges::all_of(name, [](const char character) {
            return (character >= 'a' && character <= 'z')
                || (character >= '0' && character <= '9')
                || character == '_';
        }));
        exec_sqlf(ctx, "TRUNCATE bioseq_internal.{};", name);
        ++truncated_relations;
    }
    REQUIRE(truncated_relations > 0);
    const auto empty_but_present = query_one(ctx,
        "SELECT physical_index_relations::text, persisted_index_rows::text, "
        "generation_objects::text "
        "FROM bioseq.inspect_cold_no_index_absence("
        "'index_admin_no_index_absence'::regclass, 'minimizers_per_fragment');");
    REQUIRE(std::stoll(empty_but_present[0].value()) > 0);
    REQUIRE(empty_but_present[1].value() == "0");
    REQUIRE(empty_but_present[2].value() == "0");

    const auto unsupported = sql_error(ctx,
        "SELECT * FROM bioseq.inspect_cold_no_index_absence("
        "'index_admin_no_index_absence'::regclass, 'symbol_counts');");
    REQUIRE(unsupported.sqlstate == "55000");
    REQUIRE(unsupported.contains(
        "minimizer fragment or sequence policy"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("sequence-source invalidation is terminal across parent update and delete",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"index_admin_source_terminal"});
    exec_sql(ctx,
        "CREATE TABLE index_admin_source_terminal "
        "(id int PRIMARY KEY, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(ctx,
        "SET bioseq.index.insert_maintenance.enabled=off; "
        "INSERT INTO index_admin_source_terminal VALUES "
        "(1, 'ACGTACGT'::bioseq.seq('dna4', 64));");
    const auto first_epoch = query_one(ctx,
        "SELECT source_epoch::text FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='index_admin_source_terminal'::regclass AND state=1;");
    REQUIRE(first_epoch.size() == 1);

    exec_sql(ctx,
        "UPDATE index_admin_source_terminal SET "
        "seq='TGCATGCA'::bioseq.seq('dna4', 64) WHERE id=1;");
    const auto after_update = query_one(ctx, fmt::format(
        "SELECT count(*) FILTER (WHERE state=1)::text, "
        "count(*) FILTER (WHERE state=2)::text, "
        "max(state) FILTER (WHERE source_epoch={})::text "
        "FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='index_admin_source_terminal'::regclass;",
        first_epoch[0].value()));
    REQUIRE(after_update[0].value() == "1");
    REQUIRE(after_update[1].value() == "1");
    REQUIRE(after_update[2].value() == "2");

    exec_sql(ctx, "DELETE FROM index_admin_source_terminal WHERE id=1;");
    const auto after_delete = query_one(ctx,
        "SELECT count(*) FILTER (WHERE state=1)::text, "
        "count(*) FILTER (WHERE state=2)::text "
        "FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='index_admin_source_terminal'::regclass;");
    REQUIRE(after_delete[0].value() == "0");
    REQUIRE(after_delete[1].value() == "2");
}

TEST_CASE("published generation inspection rejects invalid inputs before access",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"index_admin_inspect_invalid", "index_admin_inspect_multi"});
    exec_sql(ctx,
        "CREATE TABLE index_admin_inspect_invalid "
        "(id int, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(ctx,
        "INSERT INTO index_admin_inspect_invalid VALUES "
        "(1, repeat('ACGT', 4096)::bioseq.seq('dna4', 64));");

    auto err = sql_error(ctx,
        "SELECT * FROM bioseq.inspect_ready_minimizer_generation("
        "'index_admin_inspect_invalid'::regclass, 'symbol_counts', 0);");
    REQUIRE(err.sqlstate == "22023");
    REQUIRE(err.contains("requires a minimizer index"));

    err = sql_error(ctx,
        "SELECT * FROM bioseq.inspect_ready_minimizer_generation("
        "'index_admin_inspect_invalid'::regclass, "
        "'minimizers_per_fragment', -1);");
    REQUIRE(err.sqlstate == "22023");
    REQUIRE(err.contains("query key is negative"));

    err = sql_error(ctx,
        "SELECT * FROM bioseq.inspect_ready_minimizer_generation("
        "'index_admin_inspect_invalid'::regclass, "
        "'minimizers_per_fragment', 99);");
    REQUIRE(err.sqlstate == "P0002");
    REQUIRE(err.contains("source row is not visible"));

    exec_sql(ctx,
        "CREATE TABLE index_admin_inspect_multi "
        "(id int, a bioseq.seq('dna4', 64), b bioseq.seq('dna4', 64)) "
        "USING bioseq;");
    err = sql_error(ctx,
        "SELECT * FROM bioseq.inspect_ready_minimizer_generation("
        "'index_admin_inspect_multi'::regclass, "
        "'minimizers_per_fragment', 0);");
    REQUIRE(err.sqlstate == "22023");
    REQUIRE(err.contains("exactly one bioseq.seq column"));

    exec_sql(ctx, "CREATE TABLE index_admin_inspect_heap(id int);");
    err = sql_error(ctx,
        "SELECT * FROM bioseq.inspect_ready_minimizer_generation("
        "'index_admin_inspect_heap'::regclass, 'minimizers_per_fragment', 0);");
    REQUIRE(err.sqlstate == "42809");
    REQUIRE(err.contains("requires a USING bioseq table"));
    exec_sql(ctx, "DROP TABLE index_admin_inspect_heap;");

    // Every ERROR path must release its registered snapshot/read state and leave
    // the libpq session usable.
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("generation clear savepoint restores relation head and exact age row",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table = "index_admin_generation_clear_rollback";
    auto ctx = fresh_context({table});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sqlf(ctx,
        "CREATE TABLE {} (id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES "
        "(1, (repeat('ACGT', 16391) || 'A')::bioseq.seq('dna4', 64));",
        table);
    const auto rowno = only_column_row_number(ctx, table);
    const auto built = query_one(ctx, fmt::format(
        "SELECT * FROM "
        "bioseq_internal.build_ready_minimizer_fragment_generation("
        "'{}'::regclass, 'minimizers_per_fragment', {}, 'scalar');",
        table, rowno));
    REQUIRE(built.size() == 45);
    const std::string sidecar = built[0].value();
    const std::size_t rollback_shard_separator = sidecar.rfind('_');
    REQUIRE(rollback_shard_separator != std::string::npos);
    const std::size_t rollback_bucket_separator = sidecar.rfind(
        '_', rollback_shard_separator - 1);
    REQUIRE(rollback_bucket_separator != std::string::npos);
    const std::string rollback_prefix = sidecar.substr(
        0, rollback_bucket_separator + 1);
    const std::string rollback_lookalike =
        rollback_prefix + "255_4294967295";
    exec_sqlf(ctx,
        "CREATE TABLE bioseq_internal.{} ("
        "query_key bigint NOT NULL, generation_id bigint NOT NULL, "
        "position_segment integer NOT NULL, sub_key bigint NOT NULL, "
        "chunk_no integer NOT NULL, chunk_bytes integer NOT NULL, "
        "checksum bigint NOT NULL, blob bytea NOT NULL, "
        "PRIMARY KEY "
        "(query_key,generation_id,position_segment,sub_key,chunk_no)) "
        "USING heap;",
        rollback_lookalike);
    exec_sqlf(ctx,
        "ALTER TABLE bioseq_internal.{} "
        "ALTER COLUMN blob SET STORAGE PLAIN;",
        rollback_lookalike);
    const long long rollback_lookalike_oid = query_scalar_int(ctx,
        fmt::format(
            "SELECT to_regclass('bioseq_internal.{}')::oid::bigint;",
            rollback_lookalike));
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM pg_class c JOIN pg_namespace n "
        "ON n.oid=c.relnamespace WHERE n.nspname='bioseq_internal' "
        "AND c.relkind='r' "
        "AND left(c.relname::text, length('{}'))='{}';",
        rollback_prefix, rollback_prefix)) == 2);

    exec_sqlf(ctx,
        "INSERT INTO bioseq_internal.metadata_age_log "
        "(user_relid, main_relfilenode, column_attno, index_name, last_updated) "
        "SELECT '{}'::regclass, pg_relation_filenode('{}'::regclass), "
        "(a.attnum + v.attno_delta)::smallint, "
        "v.index_name, v.last_updated "
        "FROM pg_attribute a CROSS JOIN (VALUES "
        "('minimizers_per_fragment'::text, "
        " '2026-07-12 10:11:12.123456+00'::timestamptz, 0::smallint), "
        "('minimizers_per_sequence'::text, "
        " '2026-07-12 10:11:13.123456+00'::timestamptz, 0::smallint), "
        "('minimizers_per_fragment'::text, "
        " '2026-07-12 10:11:14.123456+00'::timestamptz, 1::smallint)) "
        "AS v(index_name,last_updated,attno_delta) "
        "WHERE a.attrelid='{}'::regclass "
        "AND a.attname='seq' AND NOT a.attisdropped "
        "ON CONFLICT (user_relid, main_relfilenode, column_attno, index_name) "
        "DO UPDATE SET last_updated=EXCLUDED.last_updated;",
        table, table, table);

    const auto warmed = query_one(ctx, fmt::format(
        "SELECT * FROM bioseq_internal.inspect_generation_storage_transition("
        "'{}'::regclass, 'minimizers_per_fragment', {}, "
        "'warm_generation_residency');",
        table, rowno));
    REQUIRE(warmed.size() == 7);
    REQUIRE(warmed[0].has_value());
    REQUIRE(warmed[2] == warmed[0]);
    REQUIRE(warmed[4].value() == "t");
    REQUIRE(warmed[5].value() == "t");
    REQUIRE(warmed[6].value() == "t");

    const rowset_t head_before = query_rows(ctx, fmt::format(
        "SELECT generation_id::text, position_segment::text, "
        "sub_key::text, chunk_no::text, chunk_bytes::text, "
        "checksum::text, encode(blob, 'hex') "
        "FROM bioseq_internal.{} WHERE query_key={} "
        "AND generation_id=0 ORDER BY position_segment, sub_key, chunk_no;",
        sidecar, rowno));
    REQUIRE(head_before.size() == 1);
    const rowset_t age_before = query_rows(ctx, fmt::format(
        "SELECT user_relid::text, main_relfilenode::text, "
        "column_attno::text, index_name, last_updated::text "
        "FROM bioseq_internal.metadata_age_log "
        "WHERE user_relid='{}'::regclass "
        "ORDER BY main_relfilenode, column_attno, index_name;",
        table));
    // The three explicit rows cover the exact per-fragment age plus the
    // per-sequence and hostile adjacent-attno per-fragment collateral. Their rollback image must be preserved
    // byte-for-byte below.
    REQUIRE(age_before.size() == 3);

    const auto cleared = query_one(ctx, fmt::format(
        "SELECT * FROM bioseq_internal.inspect_generation_storage_transition("
        "'{}'::regclass, 'minimizers_per_fragment', {}, "
        "'clear_generation_savepoint_rollback');",
        table, rowno));
    REQUIRE(cleared.size() == 7);
    REQUIRE(cleared[0] == warmed[0]);
    REQUIRE(cleared[1] == warmed[0]);
    REQUIRE(cleared[2] == warmed[0]);
    REQUIRE(cleared[3].value() == "t");
    REQUIRE(cleared[4].value() == "t");
    REQUIRE(cleared[5].value() == "f");
    REQUIRE(cleared[6].value() == "f");
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT to_regclass('bioseq_internal.{}')::oid::bigint;", sidecar)) ==
        std::stoll(warmed[0].value()));
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT to_regclass('bioseq_internal.{}')::oid::bigint;",
        rollback_lookalike)) == rollback_lookalike_oid);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM pg_class c JOIN pg_namespace n "
        "ON n.oid=c.relnamespace WHERE n.nspname='bioseq_internal' "
        "AND c.relkind='r' "
        "AND left(c.relname::text, length('{}'))='{}';",
        rollback_prefix, rollback_prefix)) == 2);
    REQUIRE(query_rows(ctx, fmt::format(
        "SELECT generation_id::text, position_segment::text, "
        "sub_key::text, chunk_no::text, chunk_bytes::text, "
        "checksum::text, encode(blob, 'hex') "
        "FROM bioseq_internal.{} WHERE query_key={} "
        "AND generation_id=0 ORDER BY position_segment, sub_key, chunk_no;",
        sidecar, rowno)) == head_before);
    REQUIRE(query_rows(ctx, fmt::format(
        "SELECT user_relid::text, main_relfilenode::text, "
        "column_attno::text, index_name, last_updated::text "
        "FROM bioseq_internal.metadata_age_log "
        "WHERE user_relid='{}'::regclass "
        "ORDER BY main_relfilenode, column_attno, index_name;",
        table)) == age_before);
    exec_sqlf(ctx, "DROP TABLE bioseq_internal.{};", rollback_lookalike);
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("committed generation clear drops every sidecar and evicts another backend",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table = "index_admin_generation_clear_commit";
    auto setup = fresh_context({table});
    exec_sql(setup, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sqlf(setup,
        "CREATE TABLE {} (id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table);
    exec_sqlf(setup,
        "INSERT INTO {} VALUES "
        "(1, repeat('ACGT', 8198)::bioseq.seq('dna4', 64)), "
        "(2, (repeat('TGCA', 16391) || 'T')::bioseq.seq('dna4', 64));",
        table);
    const auto rows = query_rows(setup, fmt::format(
        "SELECT bioseq.column_row_number('{}'::regclass, ctid) "
        "FROM {} ORDER BY id;", table, table));
    REQUIRE(rows.size() == 2);
    const long long rowno_a = std::stoll(rows[0][0].value());
    const long long rowno_b = std::stoll(rows[1][0].value());
    const auto built_a = query_one(setup, fmt::format(
        "SELECT * FROM "
        "bioseq_internal.build_ready_minimizer_fragment_generation("
        "'{}'::regclass, 'minimizers_per_fragment', {}, 'scalar');",
        table, rowno_a));
    const auto built_b = query_one(setup, fmt::format(
        "SELECT * FROM "
        "bioseq_internal.build_ready_minimizer_fragment_generation("
        "'{}'::regclass, 'minimizers_per_fragment', {}, 'scalar');",
        table, rowno_b));
    REQUIRE(built_a.size() == 45);
    REQUIRE(built_b.size() == 45);
    const std::string sidecar_a = built_a[0].value();
    const std::string sidecar_b = built_b[0].value();
    REQUIRE(sidecar_a != sidecar_b);
    const std::size_t shard_separator = sidecar_a.rfind('_');
    REQUIRE(shard_separator != std::string::npos);
    const std::size_t bucket_separator = sidecar_a.rfind(
        '_', shard_separator - 1);
    REQUIRE(bucket_separator != std::string::npos);
    const std::string exact_prefix = sidecar_a.substr(
        0, bucket_separator + 1);
    REQUIRE(sidecar_b.starts_with(exact_prefix));
    const std::string commit_lookalike =
        exact_prefix + "255_4294967295";
    exec_sqlf(setup,
        "CREATE TABLE bioseq_internal.{} ("
        "query_key bigint NOT NULL, generation_id bigint NOT NULL, "
        "position_segment integer NOT NULL, sub_key bigint NOT NULL, "
        "chunk_no integer NOT NULL, chunk_bytes integer NOT NULL, "
        "checksum bigint NOT NULL, blob bytea NOT NULL, "
        "PRIMARY KEY "
        "(query_key,generation_id,position_segment,sub_key,chunk_no)) "
        "USING heap;",
        commit_lookalike);
    exec_sqlf(setup,
        "ALTER TABLE bioseq_internal.{} "
        "ALTER COLUMN blob SET STORAGE PLAIN;",
        commit_lookalike);
    const long long commit_lookalike_oid = query_scalar_int(setup,
        fmt::format(
            "SELECT to_regclass('bioseq_internal.{}')::oid::bigint;",
            commit_lookalike));
    REQUIRE(query_scalar_int(setup, fmt::format(
        "SELECT count(*) FROM pg_class c JOIN pg_namespace n "
        "ON n.oid=c.relnamespace WHERE n.nspname='bioseq_internal' "
        "AND c.relkind IN ('r','p') "
        "AND left(c.relname::text, length('{}'))='{}';",
        exact_prefix, exact_prefix)) == 3);

    exec_sqlf(setup,
        "INSERT INTO bioseq_internal.metadata_age_log "
        "(user_relid, main_relfilenode, column_attno, index_name, last_updated) "
        "SELECT '{}'::regclass, pg_relation_filenode('{}'::regclass), "
        "(a.attnum + v.attno_delta)::smallint, "
        "v.index_name, v.last_updated "
        "FROM pg_attribute a CROSS JOIN (VALUES "
        "('minimizers_per_fragment'::text, "
        " '2026-07-12 11:12:13.654321+00'::timestamptz, 0::smallint), "
        "('minimizers_per_sequence'::text, "
        " '2026-07-12 11:12:14.654321+00'::timestamptz, 0::smallint), "
        "('minimizers_per_fragment'::text, "
        " '2026-07-12 11:12:15.654321+00'::timestamptz, 1::smallint)) "
        "AS v(index_name,last_updated,attno_delta) "
        "WHERE a.attrelid='{}'::regclass "
        "AND a.attname='seq' AND NOT a.attisdropped "
        "ON CONFLICT (user_relid, main_relfilenode, column_attno, index_name) "
        "DO UPDATE SET last_updated=EXCLUDED.last_updated;",
        table, table, table);

    const rowset_t collateral_age_before = query_rows(setup, fmt::format(
        "SELECT main_relfilenode::text, column_attno::text, "
        "index_name, last_updated::text "
        "FROM bioseq_internal.metadata_age_log m "
        "WHERE m.user_relid='{}'::regclass AND ("
        "m.index_name<>'minimizers_per_fragment' OR "
        "m.column_attno<>(SELECT attnum FROM pg_attribute "
        "WHERE attrelid='{}'::regclass AND attname='seq' "
        "AND NOT attisdropped)) "
        "ORDER BY main_relfilenode, column_attno, index_name;",
        table, table));
    // The exact per-sequence row and hostile adjacent-attno per-fragment row are collateral.
    REQUIRE(collateral_age_before.size() == 2);

    connection_test_context session_a;
    connection_test_context session_b;
    const long long session_a_pid = query_scalar_int(
        session_a, "SELECT pg_backend_pid();");
    REQUIRE(session_a_pid != query_scalar_int(
        session_b, "SELECT pg_backend_pid();"));
    const auto warmed = query_one(session_a, fmt::format(
        "SELECT * FROM bioseq_internal.inspect_generation_storage_transition("
        "'{}'::regclass, 'minimizers_per_fragment', {}, "
        "'warm_generation_residency');",
        table, rowno_a));
    REQUIRE(warmed.size() == 7);
    REQUIRE(warmed[4].value() == "t");
    REQUIRE(warmed[5].value() == "t");
    REQUIRE(warmed[6].value() == "t");

    exec_sql(session_b, "BEGIN;");
    const auto cleared = query_one(session_b, fmt::format(
        "SELECT * FROM bioseq_internal.inspect_generation_storage_transition("
        "'{}'::regclass, 'minimizers_per_fragment', {}, "
        "'clear_generation');",
        table, rowno_a));
    exec_sql(session_b, "COMMIT;");
    REQUIRE(cleared.size() == 7);
    REQUIRE(cleared[0].has_value());
    REQUIRE(cleared[1] == cleared[0]);
    REQUIRE_FALSE(cleared[2].has_value());
    REQUIRE_FALSE(cleared[3].has_value());
    REQUIRE(cleared[4].value() == "f");
    REQUIRE(cleared[5].value() == "f");
    REQUIRE(cleared[6].value() == "f");

    const auto probed = query_one(session_a, fmt::format(
        "SELECT * FROM bioseq_internal.inspect_generation_storage_transition("
        "'{}'::regclass, 'minimizers_per_fragment', {}, "
        "'probe_residency');",
        table, rowno_a));
    REQUIRE(probed.size() == 7);
    REQUIRE_FALSE(probed[0].has_value());
    REQUIRE_FALSE(probed[1].has_value());
    REQUIRE_FALSE(probed[2].has_value());
    REQUIRE_FALSE(probed[3].has_value());
    REQUIRE(probed[4].value() == "f");
    REQUIRE(probed[5].value() == "f");
    REQUIRE(probed[6].value() == "f");
    REQUIRE(query_scalar_int(session_a, "SELECT pg_backend_pid();") ==
        session_a_pid);

    REQUIRE(query_scalar_int(session_b, fmt::format(
        "SELECT count(*) FROM pg_class c JOIN pg_namespace n "
        "ON n.oid=c.relnamespace WHERE n.nspname='bioseq_internal' "
        "AND c.relkind IN ('r','p') "
        "AND left(c.relname::text, length('{}'))='{}';",
        exact_prefix, exact_prefix)) == 1);
    REQUIRE(query_scalar_int(session_b, fmt::format(
        "SELECT to_regclass('bioseq_internal.{}')::oid::bigint;",
        commit_lookalike)) == commit_lookalike_oid);
    REQUIRE(query_scalar_int(session_b, fmt::format(
        "SELECT count(*) FROM bioseq_internal.metadata_age_log "
        "WHERE user_relid='{}'::regclass "
        "AND index_name='minimizers_per_fragment' "
        "AND column_attno=(SELECT attnum FROM pg_attribute "
        "WHERE attrelid='{}'::regclass AND attname='seq' "
        "AND NOT attisdropped);", table, table)) == 0);
    REQUIRE(query_rows(session_b, fmt::format(
        "SELECT main_relfilenode::text, column_attno::text, "
        "index_name, last_updated::text "
        "FROM bioseq_internal.metadata_age_log m "
        "WHERE m.user_relid='{}'::regclass AND ("
        "m.index_name<>'minimizers_per_fragment' OR "
        "m.column_attno<>(SELECT attnum FROM pg_attribute "
        "WHERE attrelid='{}'::regclass AND attname='seq' "
        "AND NOT attisdropped)) "
        "ORDER BY main_relfilenode, column_attno, index_name;",
        table, table)) == collateral_age_before);
    REQUIRE(query_scalar_int(session_b, fmt::format(
        "SELECT count(*) FROM pg_class WHERE oid IN ("
        "to_regclass('bioseq_internal.{}'), "
        "to_regclass('bioseq_internal.{}'));",
        sidecar_a, sidecar_b)) == 0);

    const auto absent = query_one(session_b, fmt::format(
        "SELECT * FROM bioseq_internal.inspect_generation_storage_transition("
        "'{}'::regclass, 'minimizers_per_fragment', {}, "
        "'clear_generation_absent');",
        table, rowno_a));
    REQUIRE(absent.size() == 7);
    REQUIRE_FALSE(absent[0].has_value());
    REQUIRE_FALSE(absent[1].has_value());
    REQUIRE_FALSE(absent[2].has_value());
    REQUIRE_FALSE(absent[3].has_value());
    REQUIRE(absent[4].value() == "f");
    REQUIRE(absent[5].value() == "f");
    REQUIRE(absent[6].value() == "f");
    REQUIRE(query_scalar_int(session_b, fmt::format(
        "SELECT to_regclass('bioseq_internal.{}')::oid::bigint;",
        commit_lookalike)) == commit_lookalike_oid);
    REQUIRE(query_rows(session_b, fmt::format(
        "SELECT main_relfilenode::text, column_attno::text, "
        "index_name, last_updated::text "
        "FROM bioseq_internal.metadata_age_log m "
        "WHERE m.user_relid='{}'::regclass "
        "ORDER BY main_relfilenode, column_attno, index_name;",
        table)).size() == 2); // exact-attno per-sequence + adjacent-attno per-fragment
    exec_sqlf(session_b,
        "DROP TABLE bioseq_internal.{};", commit_lookalike);
    REQUIRE(query_scalar_int(session_b, "SELECT 1;") == 1);
}

TEST_CASE("fragment-minimizer coordinator executes scalar and SIMD PostgreSQL scanners",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table = "index_admin_fragment_minimizer_coordinator";
    auto ctx = fresh_context({table});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sqlf(ctx,
        "CREATE TABLE {} (id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES "
        "(1, (repeat('ACGT', 16391) || 'A')::bioseq.seq('dna4', 64));",
        table);
    const auto rowno = only_column_row_number(ctx, table);
    const rowset_t metadata_age_before = query_rows(ctx, fmt::format(
        "SELECT user_relid::text, main_relfilenode::text, "
        "column_attno::text, index_name, last_updated::text "
        "FROM bioseq_internal.metadata_age_log "
        "WHERE user_relid='{}'::regclass "
        "AND index_name='minimizers_per_fragment' "
        "ORDER BY main_relfilenode, column_attno, index_name;",
        table));

    constexpr std::array<std::string_view, 3> backends{
        "scalar", "exact_simd", "bounded_nthash_simd"};
    std::array<row_t, backends.size()> observations{};
    long long previous_generation = 0;
    for (std::size_t backend_index = 0;
         backend_index < backends.size(); ++backend_index) {
        const std::string_view backend = backends[backend_index];
        row_t& observed = observations[backend_index];
        observed = query_one(ctx, fmt::format(
            "SELECT * FROM "
            "bioseq_internal.build_ready_minimizer_fragment_generation("
            "'{}'::regclass, 'minimizers_per_fragment', {}, '{}');",
            table, rowno, backend));
        REQUIRE(observed.size() == 45);
        REQUIRE(!observed[0]->empty());
        REQUIRE(observed[1].value() == "2");
        REQUIRE(observed[2].value() == std::to_string(rowno));
        REQUIRE(observed[3]->size() == 16);
        REQUIRE(std::stoll(observed[4].value()) >= 3);
        REQUIRE(observed[5].value() == "65565");
        REQUIRE(observed[6].value() == backend);
        REQUIRE(observed[7].value() == "5");
        REQUIRE(observed[8].value() == "t");
        REQUIRE(observed[9].value() == "t");
        const long long generation = std::stoll(observed[10].value());
        REQUIRE(generation > previous_generation);
        previous_generation = generation;
        REQUIRE(observed[11]->size() == 16);
        REQUIRE(observed[12]->size() == 16);
        REQUIRE(observed[13]->size() == 16);
        REQUIRE(observed[14].value() == observed[5].value());
        REQUIRE(observed[15].value() == "15");
        REQUIRE(observed[16].value() == "10");
        REQUIRE(std::stoll(observed[19].value()) > 0);
        REQUIRE(std::stoll(observed[20].value()) > 0);
        REQUIRE(std::stoll(observed[21].value()) > 0);
        REQUIRE(std::stoll(observed[22].value()) > 0);
        REQUIRE(std::stoll(observed[23].value()) > 0);
        REQUIRE(std::stoll(observed[24].value()) > 0);
        REQUIRE(std::stoll(observed[25].value())
            >= std::stoll(observed[24].value()));
        REQUIRE(std::stoll(observed[26].value()) ==
            std::stoll(observed[27].value()) + 3);
        REQUIRE(observed[28].value() == "0");
        REQUIRE(observed[32].value() == "0");
        REQUIRE(observed[33].value() == "t");
        REQUIRE(observed[43].value() == "0");
        REQUIRE(observed[44].value() == "0");

        if (backend == "scalar") {
            REQUIRE(observed[17].value() == "0");
            REQUIRE(observed[18].value() == "0");
            REQUIRE(std::stoll(observed[29].value()) > 0);
            REQUIRE(observed[30].value() == "0");
            REQUIRE(observed[31].value() == "0");
            REQUIRE(observed[34].value() == "f");
            REQUIRE(observed[35].value() == "f");
            REQUIRE(observed[38].value() == "f");
            REQUIRE(observed[40].value() == "f");
        } else if (backend == "exact_simd") {
            REQUIRE(observed[17].value() == "0");
            REQUIRE(observed[18].value() == "0");
            REQUIRE(observed[29].value() == "0");
            REQUIRE(std::stoll(observed[30].value()) > 0);
            REQUIRE(observed[31].value() == "0");
            REQUIRE(observed[34].value() == "t");
            REQUIRE(observed[35].value() == "t");
            REQUIRE(std::stoll(observed[36].value()) > 0);
            REQUIRE(observed[37].value() == "0");
            REQUIRE(observed[38].value() == "f");
        } else {
            REQUIRE(observed[17].value() == "1");
            REQUIRE(observed[18].value() == "1");
            REQUIRE(observed[29].value() == "0");
            REQUIRE(observed[30].value() == "0");
            REQUIRE(std::stoll(observed[31].value()) > 0);
            REQUIRE(observed[34].value() == "f");
            REQUIRE(observed[38].value() == "t");
            REQUIRE(observed[39].value() == "ok");
            REQUIRE(observed[40].value() == "t");
            REQUIRE(std::stoll(observed[41].value()) > 0);
            REQUIRE(observed[42].value() == "0");
        }

        const auto persisted = query_one(ctx, fmt::format(
            "SELECT count(*) FILTER (WHERE generation_id=0)::text, "
            "count(*) FILTER (WHERE generation_id={})::text "
            "FROM bioseq_internal.{} WHERE query_key={};",
            generation, observed[0].value(), rowno));
        REQUIRE(persisted.size() == 2);
        REQUIRE(persisted[0].value() == "1");
        REQUIRE(std::stoll(persisted[1].value()) > 1);
    }

    for (std::size_t identity_column : {3u, 4u, 5u, 11u, 12u}) {
        REQUIRE(observations[1][identity_column]
            == observations[0][identity_column]);
        REQUIRE(observations[2][identity_column]
            == observations[0][identity_column]);
    }
    REQUIRE(observations[2][17] != observations[0][17]);
    REQUIRE(query_rows(ctx, fmt::format(
        "SELECT user_relid::text, main_relfilenode::text, "
        "column_attno::text, index_name, last_updated::text "
        "FROM bioseq_internal.metadata_age_log "
        "WHERE user_relid='{}'::regclass "
        "AND index_name='minimizers_per_fragment' "
        "ORDER BY main_relfilenode, column_attno, index_name;",
        table)) == metadata_age_before);
    const auto fmgr_error = sql_error(ctx, fmt::format(
        "SELECT * FROM "
        "bioseq_internal.build_ready_minimizer_fragment_generation("
        "'{}'::regclass, 'minimizers_per_fragment', {}, 'invalid');",
        table, rowno));
    REQUIRE(fmgr_error.contains("backend is invalid"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("PostgreSQL full-generation reader rejects same-scheme producer relabelling",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table =
        "index_admin_same_scheme_producer_rejection";
    auto ctx = fresh_context({table});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sqlf(ctx,
        "CREATE TABLE {} (id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES "
        "(1, (repeat('ACGT', 16391) || 'A')::bioseq.seq('dna4', 64));",
        table);
    const auto rowno = only_column_row_number(ctx, table);
    const auto build = [&ctx, table, rowno](std::string_view backend) {
        return query_one(ctx, fmt::format(
            "SELECT * FROM "
            "bioseq_internal.build_ready_minimizer_fragment_generation("
            "'{}'::regclass, 'minimizers_per_fragment', {}, '{}');",
            table, rowno, backend));
    };
    const auto read = [table, rowno](std::string_view backend) {
        return fmt::format(
            "SELECT * FROM "
            "bioseq_internal.scan_grouped_soa_minimizer_generation_test("
            "'{}'::regclass, 'minimizers_per_fragment', {}, "
            "15::smallint, 10::smallint, 5::smallint, '{}');",
            table, rowno, backend);
    };
    const auto expect_producer_rejection =
        [&ctx, &read](std::string_view backend) {
        const auto error = sql_error(ctx, read(backend));
        REQUIRE(error.contains(
            "READY generation minimizer storage contract disagrees"));
        REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
    };

    const row_t scalar = build("scalar");
    REQUIRE(scalar[6].value() == "scalar");
    REQUIRE(query_one(ctx, read("scalar")).size() == 16u);
    // Scalar, two-stack ntHash and Vulkan use the same biological minimizer scheme.
    // Exact producer identity is nevertheless mandatory, so neither request may
    // consume a generation produced by Scalar.
    expect_producer_rejection("exact_simd");
    expect_producer_rejection("vulkan_segment");

    const row_t exact = build("exact_simd");
    REQUIRE(exact[6].value() == "exact_simd");
    REQUIRE(query_one(ctx, read("exact_simd")).size() == 16u);
    expect_producer_rejection("scalar");
    expect_producer_rejection("vulkan_segment");
}

TEST_CASE("sequence-minimizer coordinator publishes PostgreSQL spill generations",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table = "index_admin_sequence_minimizer_coordinator";
    auto ctx = fresh_context({table});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sqlf(ctx,
        "CREATE TABLE {} (id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES "
        "(1, (repeat('ACGT', 16391) || 'A')::bioseq.seq('dna4', 64));",
        table);
    const auto rowno = only_column_row_number(ctx, table);
    const rowset_t metadata_age_before = query_rows(ctx, fmt::format(
        "SELECT user_relid::text, main_relfilenode::text, "
        "column_attno::text, index_name, last_updated::text "
        "FROM bioseq_internal.metadata_age_log "
        "WHERE user_relid='{}'::regclass "
        "ORDER BY main_relfilenode, column_attno, index_name;",
        table));

    std::vector<std::string_view> backends{
        "scalar", "exact_simd", "bounded_nthash_simd"};
    const bool hardware_vulkan_available = query_scalar(
        ctx,
        "SELECT is_available AND NOT is_software_device "
        "FROM bioseq.vulkan_runtime_status();") == "t";
    if (hardware_vulkan_available) {
        backends.push_back("vulkan_segment");
    }

    std::vector<row_t> observations;
    observations.reserve(backends.size());
    long long previous_generation = 0;
    for (const std::string_view backend : backends) {
        row_t observed = query_one(ctx, fmt::format(
            "SELECT * FROM "
            "bioseq_internal.build_ready_minimizer_sequence_generation("
            "'{}'::regclass, 'minimizers_per_sequence', {}, '{}');",
            table, rowno, backend));
        REQUIRE(observed.size() == 72);
        REQUIRE(!observed[0]->empty());
        REQUIRE(observed[1].value() == "2");
        REQUIRE(observed[2].value() == std::to_string(rowno));
        REQUIRE(observed[3]->size() == 16);
        REQUIRE(std::stoll(observed[4].value()) >= 3);
        REQUIRE(observed[5].value() == "65565");
        REQUIRE(observed[6].value() == backend);
        REQUIRE(observed[7].value() == "5");
        REQUIRE(observed[8].value() == "t");
        REQUIRE(observed[9].value() == "t");
        const long long generation = std::stoll(observed[10].value());
        REQUIRE(generation > previous_generation);
        previous_generation = generation;
        REQUIRE(observed[11]->size() == 16);
        REQUIRE(observed[12]->size() == 16);
        REQUIRE(observed[13]->size() == 16);
        REQUIRE(observed[14] == observed[5]);
        REQUIRE(observed[15].value() == "15");
        REQUIRE(observed[16].value() == "10");
        if (backend == "bounded_nthash_simd") {
            REQUIRE(observed[17].value() == "1");
            REQUIRE(observed[18].value() == "1");
        } else {
            REQUIRE(observed[17].value() == "0");
            REQUIRE(observed[18].value() == "0");
        }
        REQUIRE(std::stoll(observed[19].value()) > 0);
        REQUIRE(std::stoll(observed[20].value()) > 0);
        REQUIRE(std::stoll(observed[21].value()) > 0);
        REQUIRE(std::stoll(observed[22].value()) > 0);
        REQUIRE(std::stoll(observed[23].value()) > 0);
        REQUIRE(std::stoll(observed[24].value()) > 0);
        REQUIRE(std::stoll(observed[25].value()) > 0);

        const long long spill_runs = std::stoll(observed[26].value());
        const long long spilled_events = std::stoll(observed[27].value());
        const long long spill_mutation = std::stoll(observed[30].value());
        const long long replay_scans = std::stoll(observed[53].value());
        const long long replay_reroutes = std::stoll(observed[55].value());
        const long long replay_final_mutation =
            std::stoll(observed[64].value());
        REQUIRE(spill_runs > 0);
        REQUIRE(spilled_events > 0);
        REQUIRE(observed[25].value() == observed[27].value());
        REQUIRE(std::stoll(observed[28].value()) > 0);
        REQUIRE(std::stoll(observed[29].value()) > 0);
        REQUIRE(observed[31].value() == "1");
        REQUIRE(observed[32].value() == "1");
        REQUIRE(std::stoll(observed[33].value()) > 0);
        REQUIRE(std::stoll(observed[34].value()) > 0);
        REQUIRE(std::stoll(observed[34].value())
            <= std::stoll(observed[33].value()));
        REQUIRE(std::stoll(observed[35].value()) > 0);
        REQUIRE(observed[36].value() == "0");
        REQUIRE(observed[41].value() == "t");

        REQUIRE(replay_scans == replay_reroutes + 3);
        REQUIRE(std::stoll(observed[54].value()) == replay_scans);
        // fragment-minimizer replays the immutable PostgreSQL spill three times: once
        // for planning, once for control emission, and once for position
        // emission. All three scans must complete over the same payload.
        REQUIRE(observed[56].value() == "3");
        REQUIRE(std::stoll(observed[57].value()) == replay_reroutes);
        REQUIRE(observed[58] == observed[57]);
        REQUIRE(observed[59].value() == "0");
        REQUIRE(observed[60].value() == observed[27].value());
        REQUIRE(observed[61].value() == observed[27].value());
        REQUIRE(observed[62].value() == observed[20].value());
        REQUIRE(observed[63].value() == observed[29].value());
        REQUIRE(replay_final_mutation == spill_mutation + replay_reroutes);
        REQUIRE(std::stoll(observed[65].value()) >= 2 * spilled_events);
        REQUIRE(observed[66].value() == observed[27].value());
        REQUIRE(observed[67].value() == observed[20].value());
        REQUIRE(observed[68].value() == observed[63].value());
        REQUIRE(observed[69].value() == observed[64].value());
        REQUIRE(std::stoll(observed[70].value())
            == std::stoll(observed[69].value()) + 1);
        REQUIRE(observed[71].value() == "t");

        REQUIRE(observed[50].value() == "0");
        if (backend == "scalar") {
            REQUIRE(std::stoll(observed[37].value()) > 0);
            REQUIRE(observed[38].value() == "0");
            REQUIRE(observed[39].value() == "0");
            REQUIRE(observed[40].value() == "0");
            REQUIRE(observed[42].value() == "f");
            REQUIRE(observed[46].value() == "f");
            REQUIRE(observed[51].value() == "0");
            REQUIRE(observed[52].value() == "0");
        } else if (backend == "exact_simd") {
            REQUIRE(observed[37].value() == "0");
            REQUIRE(std::stoll(observed[38].value()) > 0);
            REQUIRE(observed[39].value() == "0");
            REQUIRE(observed[40].value() == "0");
            REQUIRE(observed[42].value() == "t");
            REQUIRE(observed[43].value() == "t");
            REQUIRE(std::stoll(observed[44].value()) > 0);
            REQUIRE(observed[45].value() == "0");
            REQUIRE(observed[46].value() == "f");
        } else if (backend == "bounded_nthash_simd") {
            REQUIRE(observed[37].value() == "0");
            REQUIRE(observed[38].value() == "0");
            REQUIRE(std::stoll(observed[39].value()) > 0);
            REQUIRE(observed[40].value() == "0");
            REQUIRE(observed[42].value() == "f");
            REQUIRE(observed[46].value() == "t");
            REQUIRE(observed[47].value() == "ok");
            REQUIRE(observed[48].value() == "t");
            REQUIRE(std::stoll(observed[49].value()) > 0);
        } else {
            REQUIRE(backend == "vulkan_segment");
            REQUIRE(observed[37].value() == "0");
            REQUIRE(observed[38].value() == "0");
            REQUIRE(observed[39].value() == "0");
            REQUIRE(std::stoll(observed[40].value()) > 0);
            REQUIRE(observed[42].value() == "f");
            REQUIRE(observed[46].value() == "f");
            REQUIRE(std::stoll(observed[51].value()) > 0);
            REQUIRE(std::stoll(observed[52].value()) > 0);
        }

        const auto persisted = query_one(ctx, fmt::format(
            "SELECT count(*) FILTER (WHERE generation_id=0)::text, "
            "count(*) FILTER (WHERE generation_id={})::text "
            "FROM bioseq_internal.{} WHERE query_key={};",
            generation, observed[0].value(), rowno));
        REQUIRE(persisted.size() == 2);
        REQUIRE(persisted[0].value() == "1");
        REQUIRE(std::stoll(persisted[1].value()) > 1);
        observations.emplace_back(std::move(observed));
    }

    REQUIRE(observations.size() >= 3);
    for (std::size_t observation = 1;
         observation < observations.size(); ++observation) {
        for (const std::size_t identity_column : {3u, 4u, 5u, 11u, 12u}) {
            REQUIRE(observations[observation][identity_column]
                == observations[0][identity_column]);
        }
    }
    REQUIRE(query_rows(ctx, fmt::format(
        "SELECT user_relid::text, main_relfilenode::text, "
        "column_attno::text, index_name, last_updated::text "
        "FROM bioseq_internal.metadata_age_log "
        "WHERE user_relid='{}'::regclass "
        "ORDER BY main_relfilenode, column_attno, index_name;",
        table)) == metadata_age_before);
    const auto fmgr_error = sql_error(ctx, fmt::format(
        "SELECT * FROM "
        "bioseq_internal.build_ready_minimizer_sequence_generation("
        "'{}'::regclass, 'minimizers_per_sequence', {}, 'invalid');",
        table, rowno));
    REQUIRE(fmgr_error.contains("backend is invalid"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}
