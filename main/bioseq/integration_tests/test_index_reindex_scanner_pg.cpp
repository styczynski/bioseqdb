// DOC0279 §16.2 — bioseq.update_index over STUB-backed rows reindexes via the
// scanner_pg fragmented walk (§11.4). Pins (a) query_key == columnar rowno on the
// reindex path (each row's distinct sub_keys == its fragment count, filed under the
// rowno the query path resolves), and (b) strict assemble==recompute on the genuinely
// new scanner_pg fragmented-globalisation path (dna4, single segment).
#include "common_tests_integration.h"
#include <libpq-fe.h>
#include <chrono>
#include <set>
#include <thread>
#include <tuple>
import bioseq.integration_tests.common;

using namespace bioseq_int_tests;

namespace {
inline std::set<std::tuple<long long, long long, long long>> mmsearch_set(
        const connection_test_context& ctx, std::string_view tbl) {
    auto rows = query_rows(ctx, fmt::format(
        "SELECT id, bioseq.target_start(id), bioseq.target_end(id) FROM {} "
        "WHERE mmsearch(target, query) ORDER BY id", tbl));
    std::set<std::tuple<long long, long long, long long>> out;
    for (auto& r : rows)
        if (r[0] && r[1] && r[2])
            out.emplace(std::stoll(*r[0]), std::stoll(*r[1]), std::stoll(*r[2]));
    return out;
}

inline void require_expression_index_ids(
        const connection_test_context& ctx, std::string_view index_name,
        std::string_view sql, std::initializer_list<long long> expected_ids) {
    const auto plan = explain_text<false>(ctx, sql);
    std::string plan_text;
    for (const auto& line : plan) {
        plan_text += line;
        plan_text += '\n';
    }
    INFO(plan_text);
    REQUIRE(plan_contains(plan, std::string(index_name)));
    REQUIRE(query_int_column(ctx, sql)
            == std::vector<long long>(expected_ids));
}

inline void require_flat_mirror_equivalence(
        const connection_test_context& ctx, long long expected_rows) {
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) FROM doc0377_expr_rewrite;") == expected_rows);
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) FROM doc0377_expr_rewrite_heap;") == expected_rows);
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) FROM doc0377_expr_rewrite b "
        "FULL JOIN doc0377_expr_rewrite_heap h USING (id) "
        "WHERE b.id IS NULL OR h.id IS NULL "
        "OR bioseq.to_text(b.s) IS DISTINCT FROM bioseq.to_text(h.s) "
        "OR NOT (b.s[4:9] OPERATOR(bioseq.=) h.s[4:9]) "
        "OR NOT (bioseq.reverse(b.s[4:36]) OPERATOR(bioseq.=) "
        "        bioseq.reverse(h.s[4:36])) "
        "OR NOT ((b.s[4:36] OPERATOR(bioseq.||) b.s[16380:16412]) "
        "        OPERATOR(bioseq.=) "
        "        (h.s[4:36] OPERATOR(bioseq.||) h.s[16380:16412]));") == 0);
}
}  // namespace

TEST_CASE("DOC0279 update_index reindexes stub-backed rows via scanner_pg (query_key==rowno)",
          "[indexing][per-fragment][scanner-pg]") {
    auto ctx = fresh_context({"doc0279_rx"});
    exec_sql(ctx, "SET bioseq.update_index_on_insert = off;");   // no on-insert index; update_index only
    exec_sql(ctx,
        "SELECT bioseq.create_query_target_sample_table('doc0279_rx', 3, 5000, 100.0, 200000, "
        "0.001, 0.0005, 0.0005, 1.0, 1.0, 0.0, 90210);");

    REQUIRE(query_scalar_int(ctx,
        "SELECT bioseq.update_index('doc0279_rx'::regclass, 'minimizers_per_fragment')") == 3);

    // (a) query_key == rowno: every resolved columnar rowno has its full fragment count
    // (~7 for a 200 kbp target), filed under that rowno.
    auto rownos = query_int_column(ctx,
        "SELECT bioseq.column_row_number('doc0279_rx'::regclass, ctid) FROM doc0279_rx ORDER BY 1");
    REQUIRE(rownos.size() == 3);
    for (long long rn : rownos)
        REQUIRE(index_distinct_sub_keys(ctx, "minimizers_per_fragment", "doc0279_rx", rn) >= 7);

    // (b) strict assemble == recompute on the scanner_pg fragmented path.
    guc_scope on(ctx, "bioseq.read_sequence_index", "on");
    const auto assembled = mmsearch_set(ctx, "doc0279_rx");
    REQUIRE(assembled.size() == 3);
    exec_sql(ctx, "SET bioseq.read_sequence_index = off;");
    const auto recomputed = mmsearch_set(ctx, "doc0279_rx");
    REQUIRE(assembled == recomputed);
}

#ifdef BIOSEQ_TEST_INDEXES
TEST_CASE("DOC0377 slice reverse concat expression indexes survive full lifecycle",
          "[indexing][expression][slice][source-backed][rewrite][lifecycle][doc0377]") {
    constexpr std::string_view table = "doc0377_expr_rewrite";
    constexpr std::string_view heap_table = "doc0377_expr_rewrite_heap";
    constexpr std::string_view slice_index =
        "doc0377_expr_rewrite_slice_idx";
    constexpr std::string_view reverse_index =
        "doc0377_expr_rewrite_reverse_idx";
    constexpr std::string_view concat_index =
        "doc0377_expr_rewrite_concat_idx";
    constexpr std::string_view parallel_table =
        "doc0377_expr_parallel_probe";
    auto ctx = fresh_context({parallel_table, heap_table, table});
    exec_sql(ctx,
        "SET bioseq.update_index_on_insert=off;"
        "CREATE TABLE doc0377_expr_rewrite ("
        " id int PRIMARY KEY, s bioseq.seq('dna4',32)) USING bioseq;"
        "CREATE TABLE doc0377_expr_rewrite_heap ("
        " id int PRIMARY KEY, s bioseq.seq('dna4',32)) USING heap;"
        "INSERT INTO doc0377_expr_rewrite VALUES "
        " (1,repeat('A',40000)::bioseq.seq('dna4',32)),"
        " (2,repeat('C',40000)::bioseq.seq('dna4',32));"
        "INSERT INTO doc0377_expr_rewrite_heap VALUES "
        " (1,repeat('A',40000)::bioseq.seq('dna4',32)),"
        " (2,repeat('C',40000)::bioseq.seq('dna4',32));");

    // A duplicate-key ERROR occurs inside the TableAM's index tuple callback.
    // The next trace self-test requires default ambient TLS, proving the
    // index_tuple classification scope was restored by the PG_CATCH path.
    const auto duplicate = sql_error(ctx,
        "CREATE UNIQUE INDEX doc0377_expr_rewrite_failed_idx "
        "ON doc0377_expr_rewrite ((s[0:0]));");
    REQUIRE(duplicate.sqlstate == "23505");
    REQUIRE(query_scalar(ctx,
        "SELECT bioseq_internal.slice_trace_self_test();") == "t");

    exec_sql(ctx,
        "SELECT bioseq_internal.reset_sequence_source_counters();");
    exec_sql(ctx,
        "CREATE INDEX doc0377_expr_rewrite_slice_idx "
        "ON doc0377_expr_rewrite ((s[4:9]));");
    REQUIRE(query_scalar_int(ctx,
        "SELECT (bioseq_internal.sequence_source_counters()"
        "->>'standalone_providers_created')::bigint;") == 1);
    REQUIRE(query_scalar_int(ctx,
        "SELECT (bioseq_internal.sequence_source_counters()"
        "->>'domains_created')::bigint;") == 1);
    REQUIRE(query_scalar_int(ctx,
        "SELECT (bioseq_internal.sequence_source_counters()"
        "->>'domains_reused')::bigint;") >= 1);
    exec_sql(ctx,
        "CREATE INDEX doc0377_expr_rewrite_reverse_idx "
        "ON doc0377_expr_rewrite ((bioseq.reverse(s[4:36])));"
        "CREATE INDEX doc0377_expr_rewrite_concat_idx "
        "ON doc0377_expr_rewrite "
        "((s[4:36] OPERATOR(bioseq.||) s[16380:16412]));");

    const std::string slice_a_query =
        "SELECT id FROM doc0377_expr_rewrite WHERE s[4:9] "
        "OPERATOR(bioseq.=) repeat('A',5)::bioseq.seq('dna4',32) "
        "ORDER BY id";
    const std::string reverse_a_query =
        "SELECT id FROM doc0377_expr_rewrite WHERE "
        "bioseq.reverse(s[4:36]) OPERATOR(bioseq.=) "
        "repeat('A',32)::bioseq.seq('dna4',32) ORDER BY id";
    const std::string concat_a_query =
        "SELECT id FROM doc0377_expr_rewrite WHERE "
        "(s[4:36] OPERATOR(bioseq.||) s[16380:16412]) "
        "OPERATOR(bioseq.=) "
        "repeat('A',64)::bioseq.seq('dna4',32) ORDER BY id";
    // The ordinary BioseqScan is a CustomPath, so PostgreSQL's
    // enable_seqscan penalty does not disable it.  Turn that alternative off
    // while proving that the expression index itself is usable.
    exec_sql(ctx,
        "SET bioseq.enable_custom_scan=off;"
        "SET enable_seqscan=off;");
    require_expression_index_ids(ctx, slice_index, slice_a_query, {1});
    require_expression_index_ids(ctx, reverse_index, reverse_a_query, {1});
    require_expression_index_ids(ctx, concat_index, concat_a_query, {1});
    require_flat_mirror_equivalence(ctx, 2);

    // Every index already exists: the new G row must acquire all three keys.
    exec_sql(ctx,
        "INSERT INTO doc0377_expr_rewrite VALUES "
        "(3,repeat('G',40000)::bioseq.seq('dna4',32));"
        "INSERT INTO doc0377_expr_rewrite_heap VALUES "
        "(3,repeat('G',40000)::bioseq.seq('dna4',32));");
    require_expression_index_ids(ctx, slice_index,
        "SELECT id FROM doc0377_expr_rewrite WHERE s[4:9] "
        "OPERATOR(bioseq.=) repeat('G',5)::bioseq.seq('dna4',32) "
        "ORDER BY id", {3});
    require_expression_index_ids(ctx, reverse_index,
        "SELECT id FROM doc0377_expr_rewrite WHERE "
        "bioseq.reverse(s[4:36]) OPERATOR(bioseq.=) "
        "repeat('G',32)::bioseq.seq('dna4',32) ORDER BY id", {3});
    require_expression_index_ids(ctx, concat_index,
        "SELECT id FROM doc0377_expr_rewrite WHERE "
        "(s[4:36] OPERATOR(bioseq.||) s[16380:16412]) "
        "OPERATOR(bioseq.=) repeat('G',64)::bioseq.seq('dna4',32) "
        "ORDER BY id", {3});
    require_flat_mirror_equivalence(ctx, 3);

    // UPDATE must delete every old C key and install every new T key.
    exec_sql(ctx,
        "UPDATE doc0377_expr_rewrite SET "
        "s=repeat('T',40000)::bioseq.seq('dna4',32) WHERE id=2;"
        "UPDATE doc0377_expr_rewrite_heap SET "
        "s=repeat('T',40000)::bioseq.seq('dna4',32) WHERE id=2;");
    require_expression_index_ids(ctx, slice_index,
        "SELECT id FROM doc0377_expr_rewrite WHERE s[4:9] "
        "OPERATOR(bioseq.=) repeat('C',5)::bioseq.seq('dna4',32) "
        "ORDER BY id", {});
    require_expression_index_ids(ctx, reverse_index,
        "SELECT id FROM doc0377_expr_rewrite WHERE "
        "bioseq.reverse(s[4:36]) OPERATOR(bioseq.=) "
        "repeat('C',32)::bioseq.seq('dna4',32) ORDER BY id", {});
    require_expression_index_ids(ctx, concat_index,
        "SELECT id FROM doc0377_expr_rewrite WHERE "
        "(s[4:36] OPERATOR(bioseq.||) s[16380:16412]) "
        "OPERATOR(bioseq.=) repeat('C',64)::bioseq.seq('dna4',32) "
        "ORDER BY id", {});
    require_expression_index_ids(ctx, slice_index,
        "SELECT id FROM doc0377_expr_rewrite WHERE s[4:9] "
        "OPERATOR(bioseq.=) repeat('T',5)::bioseq.seq('dna4',32) "
        "ORDER BY id", {2});
    require_expression_index_ids(ctx, reverse_index,
        "SELECT id FROM doc0377_expr_rewrite WHERE "
        "bioseq.reverse(s[4:36]) OPERATOR(bioseq.=) "
        "repeat('T',32)::bioseq.seq('dna4',32) ORDER BY id", {2});
    require_expression_index_ids(ctx, concat_index,
        "SELECT id FROM doc0377_expr_rewrite WHERE "
        "(s[4:36] OPERATOR(bioseq.||) s[16380:16412]) "
        "OPERATOR(bioseq.=) repeat('T',64)::bioseq.seq('dna4',32) "
        "ORDER BY id", {2});
    require_flat_mirror_equivalence(ctx, 3);

    exec_sql(ctx,
        "SELECT bioseq_internal.reset_sequence_source_counters();");
    exec_sql(ctx, "REINDEX INDEX doc0377_expr_rewrite_slice_idx;");
    REQUIRE(query_scalar_int(ctx,
        "SELECT (bioseq_internal.sequence_source_counters()"
        "->>'standalone_providers_created')::bigint;") == 1);
    exec_sql(ctx,
        "REINDEX INDEX doc0377_expr_rewrite_reverse_idx;"
        "REINDEX INDEX doc0377_expr_rewrite_concat_idx;");
    require_expression_index_ids(ctx, slice_index, slice_a_query, {1});
    require_expression_index_ids(ctx, reverse_index, reverse_a_query, {1});
    require_expression_index_ids(ctx, concat_index, concat_a_query, {1});
    require_flat_mirror_equivalence(ctx, 3);

    // DOC0377 requires safe VACUUM/table-rewrite interaction, not indexed
    // CLUSTER support. Keep the access method's existing rejection explicit
    // and prove the backend remains usable before exercising the shared
    // relation_copy_for_cluster callback through VACUUM FULL.
    const auto cluster_error = sql_error(ctx,
        "CLUSTER doc0377_expr_rewrite USING "
        "doc0377_expr_rewrite_pkey;");
    REQUIRE(cluster_error.sqlstate == "0A000");
    REQUIRE(cluster_error.contains(
        "clustering columnar tables using indexes is not supported"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);

    exec_sql(ctx, "VACUUM FULL doc0377_expr_rewrite;");
    require_expression_index_ids(ctx, slice_index, slice_a_query, {1});
    require_expression_index_ids(ctx, reverse_index, reverse_a_query, {1});
    require_expression_index_ids(ctx, concat_index, concat_a_query, {1});
    require_flat_mirror_equivalence(ctx, 3);
    REQUIRE(query_scalar(ctx,
        "SELECT bioseq.to_text(s[16380:16412]) "
        "FROM doc0377_expr_rewrite WHERE id=1;")
        == std::string(32u, 'A'));
    INFO(query_scalar(ctx,
        "SELECT coalesce(jsonb_agg(jsonb_build_object("
        " 'user_relid',v.user_relid,'state',v.state,"
        " 'main_relfilenode',v.main_relfilenode,"
        " 'current_relfilenode',c.relfilenode,"
        " 'total_bp_size',v.total_bp_size,"
        " 'storage_unit_byte_width',v.storage_unit_byte_width,"
        " 'num_fragments',v.num_fragments) ORDER BY v.source_epoch)::text,"
        " '[]') FROM bioseq_internal.sequence_source_version v "
        "LEFT JOIN pg_class c ON c.oid=v.user_relid "
        "WHERE v.user_relid='doc0377_expr_rewrite'::regclass;"));
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) FROM bioseq_internal.sequence_source_version v "
        "JOIN pg_class c ON c.oid=v.user_relid "
        "WHERE v.user_relid='doc0377_expr_rewrite'::regclass "
        "AND v.state=1 AND v.main_relfilenode=c.relfilenode "
        "AND v.total_bp_size=40000 "
        // DNA4 packs four letters per byte. Each fragment contains 1024
        // storage units, whose persisted width varies by optimization profile.
        "AND v.num_fragments=(v.total_bp_size + "
        " (4096::bigint * v.storage_unit_byte_width) - 1) / "
        " (4096::bigint * v.storage_unit_byte_width);") == 3);

    // Request PostgreSQL's parallel-maintenance path.  The Bioseq relation
    // hook intentionally advertises zero parallel workers, so PostgreSQL may
    // select a successful serial build instead.  Exact DEBUG selection output
    // is the capability boundary: it is reported as unsupported, never counted
    // as parallel coverage.
    REQUIRE(query_scalar_int(ctx,
        "SELECT current_setting('max_parallel_workers')::int;") > 0);
    exec_sql(ctx,
        "CREATE TABLE doc0377_expr_parallel_probe ("
        " id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "ALTER TABLE doc0377_expr_parallel_probe SET (parallel_workers=2);"
        "INSERT INTO doc0377_expr_parallel_probe "
        "SELECT id, repeat('A',40000)::bioseq.seq('dna4',32) "
        "FROM generate_series(1,128) AS id;"
        "CREATE OR REPLACE FUNCTION public.bioseq_test_parallel_index_delay(value int) "
        "RETURNS int LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE AS $body$ "
        "BEGIN PERFORM pg_sleep(0.005); RETURN value; END $body$;");
    connection_test_context parallel_builder;
    connection_test_context parallel_observer;
    exec_sql(parallel_builder,
        "SET client_min_messages=debug1;"
        "SET max_parallel_maintenance_workers=2;"
        "SET min_parallel_table_scan_size=0;"
        "SET min_parallel_index_scan_size=0;");
    const long long builder_pid = query_scalar_int(
        parallel_builder, "SELECT pg_backend_pid();");
    const std::string parallel_sql =
        "CREATE INDEX doc0377_expr_parallel_probe_idx "
        "ON doc0377_expr_parallel_probe "
        "((bioseq.reverse(s[4:36]) OPERATOR(bioseq.||) s[16380:16412]), "
        " (public.bioseq_test_parallel_index_delay(id)));";
    parallel_builder.clear_notices();
    REQUIRE(PQsendQuery(parallel_builder.raw(), parallel_sql.c_str()) == 1);
    bool consume_ok = true;
    bool result_ready = false;
    bool saw_parallel_worker = false;
    bool saw_progress = false;
    for (unsigned attempt = 0; attempt < 2000; ++attempt) {
        const auto observed = query_one(parallel_observer, fmt::format(
            "SELECT count(*) FILTER (WHERE backend_type='parallel worker' "
            "AND leader_pid={})::text, "
            "count(*) FILTER (WHERE pid={} AND backend_type='client backend')::text "
            "FROM pg_stat_activity;", builder_pid, builder_pid));
        saw_parallel_worker = saw_parallel_worker
            || std::stoll(observed[0].value()) > 0;
        saw_progress = saw_progress || query_scalar_int(parallel_observer,
            fmt::format("SELECT count(*) FROM pg_stat_progress_create_index "
                        "WHERE pid={};", builder_pid)) > 0;
        if (PQconsumeInput(parallel_builder.raw()) == 0) {
            consume_ok = false;
            break;
        }
        if (PQisBusy(parallel_builder.raw()) == 0) {
            result_ready = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    bool got_result = false;
    bool parallel_build_succeeded = consume_ok && result_ready;
    std::string parallel_sqlstate;
    std::string parallel_message;
    if (result_ready) {
        while (PGresult* result = PQgetResult(parallel_builder.raw())) {
            got_result = true;
            const auto status = PQresultStatus(result);
            if (status != PGRES_COMMAND_OK) {
                parallel_build_succeeded = false;
                if (const char* state = PQresultErrorField(
                        result, PG_DIAG_SQLSTATE); state != nullptr) {
                    parallel_sqlstate = state;
                }
                if (const char* message = PQresultErrorMessage(result);
                    message != nullptr) {
                    parallel_message = message;
                }
            }
            PQclear(result);
        }
    }
    REQUIRE(consume_ok);
    REQUIRE(result_ready);
    REQUIRE(got_result);
    if (parallel_build_succeeded) {
        const bool selected_serially = parallel_builder.notice_contains(
            "DEBUG:  building index \"doc0377_expr_parallel_probe_idx\" "
            "on table \"doc0377_expr_parallel_probe\" serially");
        INFO("selected_serially=" << selected_serially);
        REQUIRE(saw_progress);
        REQUIRE(query_scalar_int(ctx,
            "SELECT count(*) FROM pg_index WHERE "
            "indexrelid='doc0377_expr_parallel_probe_idx'::regclass "
            "AND indisvalid AND indisready;") == 1);
        if (selected_serially) {
            REQUIRE_FALSE(saw_parallel_worker);
            WARN("parallel expression-index build explicitly unsupported: "
                 "PostgreSQL selected the Bioseq TableAM serial capability");
        } else {
            // With no exact serial-selection attestation, a live worker is
            // mandatory evidence that PostgreSQL actually selected parallel
            // maintenance rather than silently falling back.
            REQUIRE(saw_parallel_worker);
        }
    } else {
        INFO(parallel_sqlstate);
        INFO(parallel_message);
        REQUIRE((parallel_message.find(
                     "bioseq_parallelscan_estimate not implemented")
                 != std::string::npos
              || parallel_message.find(
                     "parallel scans on columnar are not supported")
                 != std::string::npos));
        REQUIRE(query_scalar_int(ctx,
            "SELECT count(*) FROM pg_class WHERE relname="
            "'doc0377_expr_parallel_probe_idx';") == 0);
        WARN(fmt::format(
            "parallel expression-index build explicitly unsupported: {} {}",
            parallel_sqlstate, parallel_message));
    }
    exec_sql(ctx,
        "DROP INDEX IF EXISTS doc0377_expr_parallel_probe_idx;"
        "DROP FUNCTION IF EXISTS public.bioseq_test_parallel_index_delay(int);");

    // The direct helper pins zero/aligned/overflow and exact MaxAlloc edges
    // in the same storage/index boundary suite.
    REQUIRE(query_scalar(ctx,
        "SELECT bioseq_internal.materialization_size_self_test();") == "t");
}
#endif
