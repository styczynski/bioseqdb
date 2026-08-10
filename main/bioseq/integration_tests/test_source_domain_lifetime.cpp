// DOC0377 §§7.1-7.6/17.4: shared source-domain ownership and lifetime matrix.

#include <tests/support/postgres.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <libpq-fe.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import bioseq.integration_tests.common;

using namespace bioseq_int_tests;

#ifdef BIOSEQ_TEST_INDEXES
namespace {

[[nodiscard]] std::int64_t source_counter(
        connection_test_context& ctx, std::string_view name) {
    return query_scalar_int(ctx, fmt::format(
        "SELECT (bioseq_internal.sequence_source_counters()"
        "        ->>'{}')::bigint;", name));
}

void reset_source_counters(connection_test_context& ctx) {
    exec_sql(ctx,
        "SELECT bioseq_internal.reset_sequence_source_counters();");
}

void require_source_quiescent(connection_test_context& ctx) {
    REQUIRE(source_counter(ctx, "live_registries") == 0);
    REQUIRE(source_counter(ctx, "live_providers") == 0);
    REQUIRE(source_counter(ctx, "live_domains") == 0);
    REQUIRE(source_counter(ctx, "live_leases") == 0);
    REQUIRE(source_counter(ctx, "live_cache_sets") == 0);
    REQUIRE(source_counter(ctx, "live_snapshots") == 0);
}

void require_source_resources_balanced(connection_test_context& ctx) {
    REQUIRE(source_counter(ctx, "registries_created")
            == source_counter(ctx, "registries_destroyed"));
    REQUIRE(source_counter(ctx, "providers_created")
            == source_counter(ctx, "providers_destroyed"));
    REQUIRE(source_counter(ctx, "domains_created")
            == source_counter(ctx, "domains_destroyed"));
    REQUIRE(source_counter(ctx, "cache_sets_created")
            == source_counter(ctx, "cache_sets_destroyed"));
    REQUIRE(source_counter(ctx, "snapshots_registered")
            == source_counter(ctx, "snapshots_unregistered"));
    REQUIRE(source_counter(ctx, "leases_acquired")
            == source_counter(ctx, "leases_released"));
    require_source_quiescent(ctx);
}

[[nodiscard]] std::string source_domain_high_entropy_dna4(
        std::size_t length) {
    constexpr std::string_view bases = "ACGT";
    std::string result(length, 'A');
    std::uint64_t state = 0x6a09e667f3bcc909ull;
    for (char& base : result) {
        state ^= state << 13u;
        state ^= state >> 7u;
        state ^= state << 17u;
        base = bases[static_cast<std::size_t>(state & 3u)];
    }
    return result;
}

[[nodiscard]] std::optional<unsigned long long>
source_boundary_actual_calls(
        std::span<const std::string> plan,
        std::string_view mechanism,
        std::string_view reason) {
    bool mechanism_matches = false;
    bool reason_matches = false;
    for (const auto& line : plan) {
        if (line.find("var_") != std::string::npos
            && line.find(" = ") != std::string::npos) {
            mechanism_matches = false;
            reason_matches = false;
        }
        if (line.find(fmt::format("Mechanism: {}", mechanism))
            != std::string::npos) {
            mechanism_matches = true;
        }
        if (line.find(fmt::format("Reason: {}", reason))
            != std::string::npos) {
            reason_matches = true;
        }
        if (mechanism_matches && reason_matches) {
            const auto marker = line.find("Actual Calls:");
            if (marker != std::string::npos) {
                return std::stoull(line.substr(
                    marker + std::string_view("Actual Calls:").size()));
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::size_t> plan_line_index(
        std::span<const std::string> plan,
        std::string_view needle) {
    for (std::size_t index = 0; index < plan.size(); ++index) {
        if (plan[index].find(needle) != std::string::npos) {
            return index;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::size_t plan_line_count(
        std::span<const std::string> plan,
        std::string_view needle) {
    std::size_t count = 0u;
    for (const auto& line : plan) {
        if (line.find(needle) != std::string::npos) ++count;
    }
    return count;
}

struct copy_out_result {
    bool started = false;
    bool stream_ok = true;
    bool command_ok = true;
    std::string bytes;
};

[[nodiscard]] copy_out_result run_copy_out(
        connection_test_context& ctx, std::string_view sql) {
    copy_out_result output;
    PGresult* initial = PQexec(ctx.raw(), std::string(sql).c_str());
    if (initial == nullptr) {
        output.stream_ok = false;
        output.command_ok = false;
        return output;
    }
    output.started = PQresultStatus(initial) == PGRES_COPY_OUT;
    output.command_ok = output.started;
    PQclear(initial);
    if (!output.started) return output;

    for (;;) {
        char* buffer = nullptr;
        const int received = PQgetCopyData(ctx.raw(), &buffer, 0);
        if (received > 0) {
            output.bytes.append(buffer, static_cast<std::size_t>(received));
            PQfreemem(buffer);
            continue;
        }
        if (buffer != nullptr) PQfreemem(buffer);
        if (received == -1) break;
        output.stream_ok = false;
        break;
    }
    while (PGresult* result = PQgetResult(ctx.raw())) {
        output.command_ok = output.command_ok
            && PQresultStatus(result) == PGRES_COMMAND_OK;
        PQclear(result);
    }
    return output;
}

struct async_cancel_result {
    bool sent = false;
    bool wait_observed = false;
    bool cancel_requested = false;
    bool drained = false;
    std::string sqlstate;
};

class prepared_transaction_cleanup_guard {
private:
    PGconn* connection_ = nullptr;
    std::string rollback_sql_;
    bool armed_ = false;

public:
    prepared_transaction_cleanup_guard(
            PGconn* connection, std::string_view gid)
        : connection_(connection),
          rollback_sql_(fmt::format("ROLLBACK PREPARED '{}';", gid)) {}

    prepared_transaction_cleanup_guard(
        const prepared_transaction_cleanup_guard&) = delete;
    prepared_transaction_cleanup_guard& operator=(
        const prepared_transaction_cleanup_guard&) = delete;

    ~prepared_transaction_cleanup_guard() {
        if (!armed_ || connection_ == nullptr
            || PQstatus(connection_) != CONNECTION_OK) {
            return;
        }
        if (PQtransactionStatus(connection_) != PQTRANS_IDLE) {
            if (PGresult* result = PQexec(connection_, "ROLLBACK;")) {
                PQclear(result);
            }
        }
        if (PGresult* result = PQexec(connection_, rollback_sql_.c_str())) {
            PQclear(result);
        }
    }

    void arm() noexcept { armed_ = true; }
    void release() noexcept { armed_ = false; }
};

[[nodiscard]] async_cancel_result cancel_sleeping_query(
        connection_test_context& worker,
        connection_test_context& controller,
        std::string_view sql) {
    async_cancel_result output;
    const long long worker_pid = query_scalar_int(
        worker, "SELECT pg_backend_pid();");
    output.sent = PQsendQuery(worker.raw(), std::string(sql).c_str()) == 1;
    if (!output.sent) return output;
    for (unsigned attempt = 0; attempt < 500; ++attempt) {
        const auto state = query_one(controller, fmt::format(
            "SELECT state, coalesce(wait_event,'') "
            "FROM pg_stat_activity WHERE pid={};", worker_pid));
        if (state.size() == 2 && state[0].value_or("") == "active"
            && state[1].value_or("") == "PgSleep") {
            output.wait_observed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    output.cancel_requested = query_scalar(
        controller,
        fmt::format("SELECT pg_cancel_backend({})::text;", worker_pid))
        == "true";

    bool consume_ok = true;
    for (unsigned attempt = 0;
         attempt < 1000 && PQisBusy(worker.raw()) != 0; ++attempt) {
        if (PQconsumeInput(worker.raw()) == 0) {
            consume_ok = false;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (consume_ok && PQisBusy(worker.raw()) == 0) {
        bool got_result = false;
        while (PGresult* result = PQgetResult(worker.raw())) {
            got_result = true;
            if (const char* state = PQresultErrorField(
                    result, PG_DIAG_SQLSTATE);
                state != nullptr) {
                output.sqlstate = state;
            }
            PQclear(result);
        }
        output.drained = got_result;
    }
    return output;
}

} // namespace

TEST_CASE("DOC0377 thousands of locator rows share one binding domain",
          "[seq][slice][source-domain][lifetime]") {
    auto ctx = fresh_context({"doc0377_source_many"});
    exec_sql(ctx,
        "CREATE TABLE doc0377_source_many "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO doc0377_source_many "
        "SELECT g, repeat('ACGT',1024)::bioseq.seq('dna4',32) "
        "FROM generate_series(1,1500) AS g;");

    constexpr auto aggregate_sql =
        "SELECT sum(bioseq.len(s[101:133])) "
        "FROM doc0377_source_many;";
    const auto aggregate_plan = explain_text<false>(ctx, aggregate_sql);
    REQUIRE(plan_contains(
        aggregate_plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE_FALSE(plan_contains(
        aggregate_plan, "Custom Scan (VectorAggNode)"));

    reset_source_counters(ctx);
    REQUIRE(query_scalar_int(ctx, aggregate_sql) == 48000);

    REQUIRE(source_counter(ctx, "registries_created") == 1);
    REQUIRE(source_counter(ctx, "scan_providers_created") == 1);
    REQUIRE(source_counter(ctx, "domains_created") == 1);
    REQUIRE(source_counter(ctx, "domains_reused") >= 1499);
    REQUIRE(source_counter(ctx, "cache_sets_created") == 1);
    REQUIRE(source_counter(ctx, "snapshots_registered") == 1);
    REQUIRE(source_counter(ctx, "cache_sets_destroyed") == 1);
    REQUIRE(source_counter(ctx, "snapshots_unregistered") == 1);
    REQUIRE(source_counter(ctx, "live_registries") == 0);
    REQUIRE(source_counter(ctx, "live_domains") == 0);
    REQUIRE(source_counter(ctx, "live_cache_sets") == 0);
    REQUIRE(source_counter(ctx, "live_snapshots") == 0);
}

TEST_CASE("DOC0377 BatchSort retains exact source routes without flattening tuples",
          "[seq][slice][source-domain][batch-sort][lifetime]") {
    auto ctx = fresh_context({"doc0377_source_batch_sort"});
    exec_sql(ctx,
        "CREATE TABLE doc0377_source_batch_sort "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO doc0377_source_batch_sort VALUES "
        " (1, repeat('T',4096)::bioseq.seq('dna4',32)),"
        " (2, repeat('A',4096)::bioseq.seq('dna4',32)),"
        " (3, repeat('AC',2048)::bioseq.seq('dna4',32)),"
        " (4, NULL);"
        "SET bioseq.enable_sequence_sort_scan = on;"
        "SET bioseq.aligner.vulkan_enabled = off;");

    const auto plan = explain_text<false>(ctx,
        "SELECT id FROM doc0377_source_batch_sort "
        "ORDER BY s NULLS LAST;");
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchSortScan)"));

    reset_source_counters(ctx);
    const auto ids = query_int_column(ctx,
        "SELECT id FROM doc0377_source_batch_sort "
        "ORDER BY s NULLS LAST;");
    REQUIRE(ids == std::vector<long long>{2, 3, 1, 4});
    REQUIRE(source_counter(ctx, "registries_created") == 1);
    REQUIRE(source_counter(ctx, "providers_created") == 1);
    REQUIRE(source_counter(ctx, "scan_providers_created") == 1);
    REQUIRE(source_counter(ctx, "domains_created") == 1);
    REQUIRE(source_counter(ctx, "domains_reused") == 2);
    REQUIRE(source_counter(ctx, "cache_sets_created") == 1);
    REQUIRE(source_counter(ctx, "snapshots_registered") == 1);
    REQUIRE(source_counter(ctx, "scanner_entries_created") == 3);
    REQUIRE(source_counter(ctx, "leases_acquired")
            == source_counter(ctx, "leases_released"));
    REQUIRE(source_counter(ctx, "provider_closed_with_upper_leases") >= 1);
    REQUIRE(source_counter(ctx, "last_upper_lease_after_provider_end") >= 1);
    require_source_resources_balanced(ctx);
}

TEST_CASE("DOC0377 repeated source leaves reuse one callback scanner entry",
          "[seq][slice][source-domain][scanner-cache]") {
    auto ctx = fresh_context({"doc0377_source_repeat"});
    exec_sql(ctx,
        "CREATE TABLE doc0377_source_repeat "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO doc0377_source_repeat VALUES "
        " (1, repeat('ACGT',4096)::bioseq.seq('dna4',32));");

    reset_source_counters(ctx);
    REQUIRE(query_scalar(ctx,
        "SELECT bioseq.to_text("
        " s[0:16] OPERATOR(bioseq.||) bioseq.reverse(s[0:16]) "
        " OPERATOR(bioseq.||) s[0:16]) "
        "FROM doc0377_source_repeat;")
        == "ACGTACGTACGTACGTTGCATGCATGCATGCAACGTACGTACGTACGT");
    REQUIRE(source_counter(ctx, "domains_created") == 1);
    REQUIRE(source_counter(ctx, "scanner_entries_created") == 1);
    REQUIRE(source_counter(ctx, "scanner_entry_hits") >= 2);
}

TEST_CASE("DOC0377 cached readers survive serial per-tuple resets",
          "[seq][slice][source-domain][scanner-cache][memory-context]") {
    constexpr std::string_view table = "doc0377_source_reader_context";
    auto ctx = fresh_context({table});
    exec_sql(ctx,
        "SET max_parallel_workers_per_gather=0;"
        "SET bioseq.update_index_on_insert=off;"
        "CREATE TABLE doc0377_source_reader_context "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO doc0377_source_reader_context "
        "SELECT g, repeat('ACGT',2048)::bioseq.seq('dna4',32) "
        "FROM generate_series(1,8) AS g;");

    constexpr auto sql =
        "SELECT bioseq.to_text(s[0:64]) "
        "FROM doc0377_source_reader_context;";
    const auto planned = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(planned, "Custom Scan (BioseqSliceProject)"));
    REQUIRE_FALSE(plan_contains(planned, "Gather"));

    rowset_t expected;
    expected.reserve(8u);
    for (int row = 0; row < 8; ++row) {
        expected.push_back({
            std::optional<std::string>{
                "ACGTACGTACGTACGTACGTACGTACGTACGT"
                "ACGTACGTACGTACGTACGTACGTACGTACGT"},
        });
    }
    for (int execution = 0; execution < 2; ++execution) {
        reset_source_counters(ctx);
        REQUIRE(query_rows(ctx, sql) == expected);
        REQUIRE(source_counter(ctx, "domains_created") == 1);
        REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
        require_source_resources_balanced(ctx);
    }
}

TEST_CASE("DOC0377 SliceProject keeps one lowest owner across ordering and Gather",
          "[seq][slice][planner][ordered][parallel][owner]") {
    constexpr std::string_view table = "doc0377_slice_ordered_owner";
    constexpr std::string_view owner =
        "Custom Scan (BioseqSliceProject)";
    auto ctx = fresh_context({table});
    REQUIRE(query_scalar_int(ctx,
        "SELECT current_setting('max_parallel_workers')::int;") > 0);
    exec_sql(ctx,
        "SET bioseq.update_index_on_insert=off;"
        "SET max_parallel_workers_per_gather=0;"
        "CREATE TABLE doc0377_slice_ordered_owner ("
        " pk int, s bioseq.seq('dna4',32)"
        ") USING bioseq WITH (autovacuum_enabled=false);"
        "INSERT INTO doc0377_slice_ordered_owner "
        "SELECT g, repeat('A',4161-g)::bioseq.seq('dna4',32) "
        "FROM generate_series(64,1,-1) AS g;");

    std::vector<long long> lengths_by_pk;
    rowset_t length_pk_rows;
    lengths_by_pk.reserve(64u);
    length_pk_rows.reserve(64u);
    for (long long pk = 1; pk <= 64; ++pk) {
        const long long length = 4161 - pk;
        lengths_by_pk.push_back(length);
        length_pk_rows.push_back({
            std::optional<std::string>{std::to_string(length)},
            std::optional<std::string>{std::to_string(pk)},
        });
    }
    auto lengths_multiset = lengths_by_pk;
    std::sort(lengths_multiset.begin(), lengths_multiset.end());

    const auto require_single_owner = [owner](const auto& plan) {
        REQUIRE(plan_line_count(plan, owner) == 1u);
        const auto index = plan_line_index(plan, owner);
        REQUIRE(index.has_value());
        return *index;
    };
    const auto require_exact_multiset = [&ctx, &lengths_multiset](
            std::string_view sql) {
        auto actual = query_int_column(ctx, sql);
        std::sort(actual.begin(), actual.end());
        REQUIRE(actual == lengths_multiset);
    };

    constexpr auto unordered_sql =
        "SELECT bioseq.len(s) FROM doc0377_slice_ordered_owner;";
    const auto unordered_plan = explain_text<false>(ctx, unordered_sql);
    (void)require_single_owner(unordered_plan);
    REQUIRE_FALSE(plan_contains(unordered_plan, "Sort"));
    REQUIRE_FALSE(plan_contains(unordered_plan, "Gather"));
    require_exact_multiset(unordered_sql);

    exec_sql(ctx,
        "SET bioseq.enable_parallel_execution=on;"
        "SET max_parallel_workers_per_gather=2;"
        "SET min_parallel_table_scan_size=0;"
        "SET parallel_setup_cost=0;"
        "SET parallel_tuple_cost=0;"
        "SET parallel_leader_participation=off;"
        "DO $$ BEGIN "
        " IF EXISTS (SELECT 1 FROM pg_settings "
        "            WHERE name='debug_parallel_query') THEN "
        "   PERFORM set_config('debug_parallel_query','on',false);"
        " ELSIF EXISTS (SELECT 1 FROM pg_settings "
        "               WHERE name='force_parallel_mode') THEN "
        "   PERFORM set_config('force_parallel_mode','on',false);"
        " END IF; END $$;");
    const auto parallel_plan = explain_text<false>(ctx, unordered_sql);
    const std::size_t parallel_owner = require_single_owner(parallel_plan);
    const auto gather = plan_line_index(parallel_plan, "Gather");
    REQUIRE(gather.has_value());
    REQUIRE(*gather < parallel_owner);
    REQUIRE_FALSE(plan_contains(parallel_plan, "Single Copy: true"));
    REQUIRE_FALSE(plan_contains(parallel_plan, "Sort"));
    require_exact_multiset(unordered_sql);

    exec_sql(ctx,
        "SET max_parallel_workers_per_gather=0;"
        "DO $$ BEGIN "
        " IF EXISTS (SELECT 1 FROM pg_settings "
        "            WHERE name='debug_parallel_query') THEN "
        "   PERFORM set_config('debug_parallel_query','off',false);"
        " ELSIF EXISTS (SELECT 1 FROM pg_settings "
        "               WHERE name='force_parallel_mode') THEN "
        "   PERFORM set_config('force_parallel_mode','off',false);"
        " END IF; END $$;");

    constexpr auto length_order_sql =
        "SELECT bioseq.len(s) FROM doc0377_slice_ordered_owner ORDER BY 1;";
    const auto length_order_plan = explain_text<false>(ctx, length_order_sql);
    const std::size_t length_order_owner =
        require_single_owner(length_order_plan);
    const auto length_sort = plan_line_index(length_order_plan, "Sort");
    REQUIRE(length_sort.has_value());
    REQUIRE(*length_sort < length_order_owner);
    REQUIRE_FALSE(plan_contains(length_order_plan, "Gather"));
    REQUIRE(query_int_column(ctx, length_order_sql) == lengths_multiset);

    constexpr auto length_pk_sql =
        "SELECT bioseq.len(s),pk FROM doc0377_slice_ordered_owner "
        "ORDER BY pk;";
    const auto length_pk_plan = explain_text<false>(ctx, length_pk_sql);
    const std::size_t length_pk_owner = require_single_owner(length_pk_plan);
    const auto length_pk_sort = plan_line_index(length_pk_plan, "Sort");
    REQUIRE(length_pk_sort.has_value());
    REQUIRE(*length_pk_sort < length_pk_owner);
    REQUIRE_FALSE(plan_contains(length_pk_plan, "Gather"));
    REQUIRE(query_rows(ctx, length_pk_sql) == length_pk_rows);

    constexpr auto resjunk_sql =
        "SELECT bioseq.len(s) FROM doc0377_slice_ordered_owner ORDER BY pk;";
    const auto resjunk_plan = explain_text<false>(ctx, resjunk_sql);
    const std::size_t resjunk_owner = require_single_owner(resjunk_plan);
    const auto resjunk_sort = plan_line_index(resjunk_plan, "Sort");
    REQUIRE(resjunk_sort.has_value());
    REQUIRE(*resjunk_sort < resjunk_owner);
    REQUIRE_FALSE(plan_contains(resjunk_plan, "Gather"));
    REQUIRE(query_int_column(ctx, resjunk_sql) == lengths_by_pk);
}

TEST_CASE("DOC0377 nested two-provider projection preserves upper leases",
          "[seq][slice][source-domain][route]") {
    auto ctx = fresh_context(
        {"doc0377_source_left", "doc0377_source_right"});
    exec_sql(ctx,
        "CREATE TABLE doc0377_source_left "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "CREATE TABLE doc0377_source_right "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO doc0377_source_left VALUES "
        " (1, repeat('ACGT',2048)::bioseq.seq('dna4',32));"
        "INSERT INTO doc0377_source_right VALUES "
        " (1, repeat('TGCA',2048)::bioseq.seq('dna4',32));");
    const auto plan = explain_text<false>(ctx,
        "SELECT (l.s[0:16] OPERATOR(bioseq.||) r.s[0:16])[2:30] "
        "FROM doc0377_source_left l "
        "JOIN doc0377_source_right r USING (id);");
    REQUIRE(plan_contains(plan, "BioseqSliceProject"));

    reset_source_counters(ctx);
    REQUIRE(query_scalar(ctx,
        "SELECT bioseq.to_text("
        " (l.s[0:16] OPERATOR(bioseq.||) r.s[0:16])[2:30]) "
        "FROM doc0377_source_left l "
        "JOIN doc0377_source_right r USING (id);")
        == "GTACGTACGTACGTTGCATGCATGCATG");
    REQUIRE(source_counter(ctx, "registries_created") == 2);
    REQUIRE(source_counter(ctx, "scan_providers_created") == 2);
    REQUIRE(source_counter(ctx, "domains_created") == 2);
    REQUIRE(source_counter(ctx, "cache_sets_created") == 2);
    REQUIRE(source_counter(ctx, "snapshots_registered") == 2);
    REQUIRE(source_counter(ctx, "leases_acquired") >= 4);
    REQUIRE(source_counter(ctx, "leases_acquired")
            == source_counter(ctx, "leases_released"));
    REQUIRE(source_counter(ctx, "provider_closed_with_upper_leases") >= 2);
    REQUIRE(source_counter(ctx, "last_upper_lease_after_provider_end") >= 2);
    REQUIRE(source_counter(ctx, "live_registries") == 0);
    REQUIRE(source_counter(ctx, "live_leases") == 0);
}

TEST_CASE("DOC0377 three-source concat retains arbitrary upper routes",
          "[seq][slice][concat][join][source-domain][route][error]") {
    auto ctx = fresh_context({
        "doc0377_source_three_left",
        "doc0377_source_three_middle",
        "doc0377_source_three_right",
    });
    exec_sql(ctx,
        "CREATE TABLE doc0377_source_three_left "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "CREATE TABLE doc0377_source_three_middle "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "CREATE TABLE doc0377_source_three_right "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO doc0377_source_three_left VALUES "
        " (1, repeat('ACGT',2048)::bioseq.seq('dna4',32));"
        "INSERT INTO doc0377_source_three_middle VALUES "
        " (1, repeat('TGCA',2048)::bioseq.seq('dna4',32));"
        "INSERT INTO doc0377_source_three_right VALUES "
        " (1, repeat('GATC',2048)::bioseq.seq('dna4',32));");

    constexpr const char* three_source_expression =
        "((l.s[0:16] OPERATOR(bioseq.||) m.s[0:16]) "
        " OPERATOR(bioseq.||) r.s[0:16])";
    const auto plan = explain_text<false>(ctx, fmt::format(
        "SELECT {} FROM doc0377_source_three_left AS l "
        "JOIN doc0377_source_three_middle AS m USING (id) "
        "JOIN doc0377_source_three_right AS r USING (id);",
        three_source_expression));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_line_count(plan, "Custom Scan (BioseqScan)") == 3u);

    reset_source_counters(ctx);
    REQUIRE(query_scalar(ctx, fmt::format(
        "SELECT bioseq.to_text({}) "
        "FROM doc0377_source_three_left AS l "
        "JOIN doc0377_source_three_middle AS m USING (id) "
        "JOIN doc0377_source_three_right AS r USING (id);",
        three_source_expression))
        == "ACGTACGTACGTACGTTGCATGCATGCATGCAGATCGATCGATCGATC");
    REQUIRE(source_counter(ctx, "registries_created") == 3);
    REQUIRE(source_counter(ctx, "scan_providers_created") == 3);
    REQUIRE(source_counter(ctx, "domains_created") == 3);
    REQUIRE(source_counter(ctx, "leases_acquired") >= 6);
    REQUIRE(source_counter(ctx, "provider_closed_with_upper_leases") >= 3);
    REQUIRE(source_counter(ctx, "last_upper_lease_after_provider_end") >= 3);
    require_source_resources_balanced(ctx);

    reset_source_counters(ctx);
    exec_sql(ctx,
        "SELECT bioseq_internal.arm_sequence_source_failure("
        " 'read_before_scanner');");
    const auto injected = sql_error(ctx, fmt::format(
        "SELECT bioseq.to_text({}) "
        "FROM doc0377_source_three_left AS l "
        "JOIN doc0377_source_three_middle AS m USING (id) "
        "JOIN doc0377_source_three_right AS r USING (id);",
        three_source_expression));
    REQUIRE(injected.sqlstate == "XX000");
    REQUIRE(injected.contains(
        "injected PostgreSQL sequence source-domain failure"));
    exec_sql(ctx,
        "SELECT bioseq_internal.disarm_sequence_source_failure();");
    REQUIRE(source_counter(ctx, "fault_injections") == 1);
    REQUIRE(source_counter(ctx, "read_fault_injections") == 1);
    REQUIRE(source_counter(ctx, "scan_providers_created") == 3);
    require_source_resources_balanced(ctx);
}

TEST_CASE("DOC0377 expression index build uses one standalone source owner",
          "[seq][slice][source-domain][index]") {
    auto ctx = fresh_context({"doc0377_source_index"});
    exec_sql(ctx,
        "CREATE TABLE doc0377_source_index "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO doc0377_source_index "
        "SELECT g, repeat('ACGT',1024)::bioseq.seq('dna4',32) "
        "FROM generate_series(1,128) AS g;");

    reset_source_counters(ctx);
    exec_sql(ctx,
        "CREATE INDEX doc0377_source_index_expr "
        "ON doc0377_source_index ((s[4:20]));");
    REQUIRE(source_counter(ctx, "standalone_providers_created") == 1);
    REQUIRE(source_counter(ctx, "domains_created") == 1);
    REQUIRE(source_counter(ctx, "domains_reused") >= 127);
    REQUIRE(source_counter(ctx, "cache_sets_created") == 1);
    REQUIRE(source_counter(ctx, "snapshots_registered") == 1);
    REQUIRE(source_counter(ctx, "live_registries") == 0);
}

TEST_CASE("DOC0377 cursor-held source registries promote or abort with subxact",
          "[seq][slice][source-domain][subxact]") {
    auto ctx = fresh_context({"doc0377_source_subxact"});
    exec_sql(ctx,
        "CREATE TABLE doc0377_source_subxact "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO doc0377_source_subxact VALUES "
        " (1, repeat('ACGT',2048)::bioseq.seq('dna4',32));");
    // Commit setup writes before entering the subtransaction lifecycle under test.
    exec_sql(ctx, "BEGIN;");

    reset_source_counters(ctx);
    exec_sql(ctx,
        "SAVEPOINT source_commit;"
        "DECLARE source_commit_cursor NO SCROLL CURSOR FOR "
        " SELECT bioseq.to_text(s[0:8]) "
        " FROM doc0377_source_subxact;"
        "FETCH 1 FROM source_commit_cursor;"
        "RELEASE SAVEPOINT source_commit;");
    REQUIRE(source_counter(ctx, "subxact_promotions") >= 2);
    REQUIRE(source_counter(ctx, "live_registries") == 1);
    exec_sql(ctx, "CLOSE source_commit_cursor;");
    REQUIRE(source_counter(ctx, "live_registries") == 0);

    reset_source_counters(ctx);
    exec_sql(ctx,
        "SAVEPOINT source_abort;"
        "DECLARE source_abort_cursor NO SCROLL CURSOR FOR "
        " SELECT bioseq.to_text(s[0:8]) "
        " FROM doc0377_source_subxact;"
        "FETCH 1 FROM source_abort_cursor;"
        "ROLLBACK TO SAVEPOINT source_abort;");
    REQUIRE(source_counter(ctx, "subxact_aborts") >= 1);
    REQUIRE(source_counter(ctx, "cleanup_abort") >= 1);
    REQUIRE(source_counter(ctx, "live_registries") == 0);
    REQUIRE(source_counter(ctx, "live_domains") == 0);
    exec_sql(ctx, "RELEASE SAVEPOINT source_abort; COMMIT;");
}

TEST_CASE("DOC0377 empty source registry reaches real prepared commit",
          "[seq][slice][source-domain][prepared-xact][acceptance]") {
    auto owner = fresh_context({"doc0377_source_prepared_marker"});
    connection_test_context resolver;

    const auto max_prepared = query_scalar_int(
        owner,
        "SELECT current_setting('max_prepared_transactions')::bigint;");
    INFO("DOC0377 prepared-transaction acceptance requires "
         "max_prepared_transactions > 0");
    REQUIRE(max_prepared > 0);

    exec_sql(owner,
        "CREATE TABLE doc0377_source_prepared_marker "
        " (id bigint PRIMARY KEY);");
    const auto owner_pid = query_scalar_int(owner, "SELECT pg_backend_pid();");
    const auto nonce = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const auto gid = fmt::format(
        "doc0377_empty_registry_{}_{}", owner_pid, nonce);
    const auto prepare_sql = fmt::format(
        "PREPARE TRANSACTION '{}';", gid);
    const auto commit_prepared_sql = fmt::format(
        "COMMIT PREPARED '{}';", gid);
    const auto prepared_count_sql = fmt::format(
        "SELECT count(*) FROM pg_catalog.pg_prepared_xacts "
        "WHERE gid='{}' AND database=current_database();", gid);
    prepared_transaction_cleanup_guard cleanup{resolver.raw(), gid};

    reset_source_counters(owner);
    exec_sql(owner, "BEGIN;");
    exec_sql(owner,
        "INSERT INTO doc0377_source_prepared_marker VALUES (377);");
    exec_sql(owner,
        "DO $$ BEGIN "
        "  PERFORM bioseq_internal.sequence_source_pre_prepare_probe(); "
        "  RAISE EXCEPTION 'DOC0377 PRE_PREPARE probe did not reject'; "
        "EXCEPTION WHEN feature_not_supported THEN "
        "  IF SQLERRM NOT LIKE "
        "    'cannot PREPARE TRANSACTION with live bioseq source domains%' "
        "  THEN RAISE; END IF; "
        "END $$;");
    REQUIRE(source_counter(owner, "pre_prepare_rejections") == 1);
    require_source_quiescent(owner);

    // PREPARE and COMMIT PREPARED must each be their own top-level command.
    exec_sql(owner, prepare_sql);
    cleanup.arm();
    REQUIRE(PQtransactionStatus(owner.raw()) == PQTRANS_IDLE);
    require_source_quiescent(owner);
    REQUIRE(query_scalar_int(resolver, prepared_count_sql) == 1);
    REQUIRE(query_scalar_int(
        resolver,
        "SELECT count(*) FROM doc0377_source_prepared_marker;") == 0);
    REQUIRE(PQtransactionStatus(resolver.raw()) == PQTRANS_IDLE);

    exec_sql(resolver, commit_prepared_sql);
    cleanup.release();
    REQUIRE(PQtransactionStatus(resolver.raw()) == PQTRANS_IDLE);
    REQUIRE(query_scalar_int(resolver, prepared_count_sql) == 0);
    REQUIRE(query_scalar_int(
        resolver,
        "SELECT count(*) FROM doc0377_source_prepared_marker "
        "WHERE id=377;") == 1);
    require_source_quiescent(owner);
    exec_sql(resolver, "DROP TABLE doc0377_source_prepared_marker;");
}

TEST_CASE("DOC0377 source bindings obey MVCC epoch rewrite and relcache invalidation",
          "[seq][slice][source-domain][mvcc][epoch][rewrite][invalidation]") {
    constexpr std::string_view table = "doc0377_source_mvcc";
    auto setup = fresh_context({table});
    exec_sql(setup,
        "CREATE TABLE doc0377_source_mvcc "
        " (id int PRIMARY KEY, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO doc0377_source_mvcc VALUES "
        " (1, repeat('ACGT',4096)::bioseq.seq('dna4',32));");

    connection_test_context reader;
    connection_test_context writer;
    reset_source_counters(reader);
    exec_sql(reader, "BEGIN ISOLATION LEVEL REPEATABLE READ;");
    REQUIRE(query_scalar(reader,
        "SELECT bioseq.to_text(s[0:8]) "
        "FROM doc0377_source_mvcc WHERE id=1;") == "ACGTACGT");

    exec_sql(writer,
        "UPDATE doc0377_source_mvcc "
        "SET s=repeat('TGCA',4096)::bioseq.seq('dna4',32) "
        "WHERE id=1;");
    REQUIRE(query_scalar(reader,
        "SELECT bioseq.to_text(s[0:8]) "
        "FROM doc0377_source_mvcc WHERE id=1;") == "ACGTACGT");
    exec_sql(reader, "COMMIT;");
    REQUIRE(query_scalar(reader,
        "SELECT bioseq.to_text(s[0:8]) "
        "FROM doc0377_source_mvcc WHERE id=1;") == "TGCATGCA");

    exec_sql(reader,
        "PREPARE doc0377_source_read AS "
        "SELECT bioseq.to_text(s[0:8]) "
        "FROM doc0377_source_mvcc WHERE id=1;");
    REQUIRE(query_scalar(reader, "EXECUTE doc0377_source_read;")
            == "TGCATGCA");
    exec_sql(writer,
        "ALTER TABLE doc0377_source_mvcc "
        "ADD COLUMN invalidation_marker int DEFAULT 7;");
    REQUIRE(query_scalar(reader, "EXECUTE doc0377_source_read;")
            == "TGCATGCA");
    exec_sql(writer, "VACUUM FULL doc0377_source_mvcc;");
    REQUIRE(query_scalar(reader, "EXECUTE doc0377_source_read;")
            == "TGCATGCA");
    exec_sql(reader, "DEALLOCATE doc0377_source_read;");

    REQUIRE(source_counter(reader, "domains_created") >= 6);
    REQUIRE(source_counter(reader, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(reader);
}

TEST_CASE("DOC0377 attribute snapshot and callback session keys remain distinct",
          "[seq][slice][source-domain][binding-key][scanner-cache][rescan]") {
    auto ctx = fresh_context({"doc0377_source_binding"});
    exec_sql(ctx,
        "CREATE TABLE doc0377_source_binding "
        " (id int, left_seq bioseq.seq('dna4',32), "
        "  right_seq bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO doc0377_source_binding "
        "SELECT g, repeat('ACGT',2048)::bioseq.seq('dna4',32), "
        "          repeat('TGCA',2048)::bioseq.seq('dna4',32) "
        "FROM generate_series(1,4) AS g;");

    reset_source_counters(ctx);
    REQUIRE(query_scalar(ctx,
        "SELECT bioseq.to_text(left_seq[0:8] "
        "       OPERATOR(bioseq.||) right_seq[0:8]) "
        "FROM doc0377_source_binding WHERE id=1;")
        == "ACGTACGTTGCATGCA");
    REQUIRE(source_counter(ctx, "registries_created") == 1);
    REQUIRE(source_counter(ctx, "domains_created") == 2);
    REQUIRE(source_counter(ctx, "cache_sets_created") == 2);
    REQUIRE(source_counter(ctx, "snapshots_registered") == 2);
    REQUIRE(source_counter(ctx, "scanner_entries_created") == 2);
    require_source_resources_balanced(ctx);

    exec_sql(ctx,
        "CREATE FUNCTION pg_temp.doc0377_read_source_text(value bioseq.seq) "
        "RETURNS text LANGUAGE plpgsql VOLATILE STRICT AS $$ "
        "BEGIN RETURN bioseq.to_text(value); END $$;");
    constexpr std::string_view callback_sql =
        "SELECT g, pg_temp.doc0377_read_source_text(("
        " SELECT s.left_seq[0:8] "
        " FROM doc0377_source_binding AS s "
        " WHERE s.id=g LIMIT 1"
        ")) AS fragment FROM generate_series(1,4) AS g;";
    const auto callback_plan = explain_text<false>(ctx, callback_sql);
    REQUIRE(plan_contains(callback_plan, "SubPlan"));
    REQUIRE(plan_contains(
        callback_plan, "Custom Scan (BioseqSliceProject)"));

    reset_source_counters(ctx);
    const auto rows = query_rows(ctx, callback_sql);
    REQUIRE(rows.size() == 4);
    for (const auto& row : rows) {
        REQUIRE(row.size() == 2);
        REQUIRE(row[1].value_or("") == "ACGTACGT");
    }
    REQUIRE(source_counter(ctx, "registries_created") == 1);
    REQUIRE(source_counter(ctx, "domains_created") == 1);
    REQUIRE(source_counter(ctx, "domains_reused") >= 3);
    REQUIRE(source_counter(ctx, "cache_sets_created") == 1);
    REQUIRE(source_counter(ctx, "snapshots_registered") == 1);
    REQUIRE(source_counter(ctx, "scanner_entries_created") == 4);
    REQUIRE(source_counter(ctx, "facade_source_stream_callbacks") == 4);
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);
}

TEST_CASE("DOC0377 LIMIT Portal cancellation and xact release phases quiesce exactly",
          "[seq][slice][source-domain][limit][portal][cancel][resource-owner]") {
    constexpr std::string_view table = "doc0377_source_control_flow";
    auto setup = fresh_context({table});
    exec_sql(setup,
        "CREATE TABLE doc0377_source_control_flow "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO doc0377_source_control_flow "
        "SELECT g, repeat('ACGT',2048)::bioseq.seq('dna4',32) "
        "FROM generate_series(1,64) AS g;");

    reset_source_counters(setup);
    const auto limited = query_rows(setup,
        "SELECT id, bioseq.to_text(s[101:109]) "
        "FROM doc0377_source_control_flow ORDER BY id LIMIT 2;");
    REQUIRE(limited.size() == 2);
    REQUIRE(source_counter(setup, "domains_created") == 1);
    REQUIRE(source_counter(setup, "cleanup_commit") == 1);
    REQUIRE(source_counter(setup, "release_before_locks") == 1);
    REQUIRE(source_counter(setup, "release_locks") == 1);
    REQUIRE(source_counter(setup, "release_after_locks") == 1);
    require_source_resources_balanced(setup);

    reset_source_counters(setup);
    exec_sql(setup,
        "BEGIN;"
        "DECLARE doc0377_source_portal NO SCROLL CURSOR FOR "
        " SELECT id, bioseq.to_text(s[0:8]) "
        " FROM doc0377_source_control_flow ORDER BY id;");
    REQUIRE(query_rows(setup,
        "FETCH 3 FROM doc0377_source_portal;").size() == 3);
    REQUIRE(source_counter(setup, "live_registries") == 1);
    REQUIRE(source_counter(setup, "live_providers") == 1);
    exec_sql(setup, "CLOSE doc0377_source_portal; COMMIT;");
    REQUIRE(source_counter(setup, "release_before_locks")
            == source_counter(setup, "domains_created"));
    REQUIRE(source_counter(setup, "release_locks")
            == source_counter(setup, "domains_created"));
    REQUIRE(source_counter(setup, "release_after_locks")
            == source_counter(setup, "domains_created"));
    require_source_resources_balanced(setup);

    reset_source_counters(setup);
    exec_sql(setup,
        "BEGIN;"
        "DECLARE doc0377_source_abort_portal NO SCROLL CURSOR FOR "
        " SELECT id, bioseq.to_text(s[0:8]) "
        " FROM doc0377_source_control_flow ORDER BY id;");
    REQUIRE(query_rows(setup,
        "FETCH 1 FROM doc0377_source_abort_portal;").size() == 1);
    REQUIRE(source_counter(setup, "live_registries") == 1);
    exec_sql(setup, "ROLLBACK;");
    REQUIRE(source_counter(setup, "cleanup_abort")
            == source_counter(setup, "domains_created"));
    REQUIRE(source_counter(setup, "release_before_locks")
            == source_counter(setup, "domains_created"));
    REQUIRE(source_counter(setup, "release_locks")
            == source_counter(setup, "domains_created"));
    REQUIRE(source_counter(setup, "release_after_locks")
            == source_counter(setup, "domains_created"));
    require_source_resources_balanced(setup);

    connection_test_context worker;
    connection_test_context controller;
    exec_sql(worker,
        "CREATE FUNCTION pg_temp.doc0377_hold_source(value bioseq.seq) "
        "RETURNS bigint LANGUAGE plpgsql VOLATILE STRICT AS $$ "
        "DECLARE held bioseq.seq; BEGIN "
        " held := value; "
        " PERFORM pg_sleep(0.25); RETURN bioseq.len(held); END $$;");
    constexpr std::string_view cancel_sql =
        "/* doc0377 source cancel */ "
        "SELECT pg_temp.doc0377_hold_source(("
        " SELECT t.s[0:64] "
        " FROM doc0377_source_control_flow AS t "
        " WHERE t.id=1 LIMIT 1"
        "));";
    const auto cancel_plan = explain_text<false>(worker, cancel_sql);
    REQUIRE(plan_contains(cancel_plan, "Custom Scan (BioseqSliceProject)"));
    reset_source_counters(worker);
    const auto cancelled = cancel_sleeping_query(
        worker, controller, cancel_sql);
    REQUIRE(cancelled.sent);
    REQUIRE(cancelled.wait_observed);
    REQUIRE(cancelled.cancel_requested);
    REQUIRE(cancelled.drained);
    REQUIRE(cancelled.sqlstate == "57014");
    REQUIRE(source_counter(worker, "domains_created") == 1);
    REQUIRE(source_counter(worker, "cleanup_abort")
            == source_counter(worker, "domains_created"));
    REQUIRE(source_counter(worker, "release_before_locks")
            == source_counter(worker, "domains_created"));
    REQUIRE(source_counter(worker, "release_locks")
            == source_counter(worker, "domains_created"));
    REQUIRE(source_counter(worker, "release_after_locks")
            == source_counter(worker, "domains_created"));
    REQUIRE(source_counter(worker, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(worker);
    REQUIRE(query_scalar_int(worker, "SELECT 1;") == 1);
}

TEST_CASE("DOC0377 retained source control fails closed after transaction end",
          "[seq][slice][source-domain][lifetime][xact-end][error]") {
    auto ctx = fresh_context();
    reset_source_counters(ctx);

    /* The existing native probe retains a real source-domain control across
     * the production PRE_COMMIT cleanup callback, then accepts success only
     * after the production read guard reports SQLSTATE 55000 and the exact
     * transaction-ended diagnostic. */
    REQUIRE(query_scalar(ctx,
        "SELECT bioseq_internal.sequence_source_fault_contract_probe();")
        == "t");
    REQUIRE(source_counter(ctx, "refcount_faults") == 1);
    REQUIRE(source_counter(ctx, "lease_faults") == 1);
    REQUIRE(source_counter(ctx, "registries_created") == 1);
    REQUIRE(source_counter(ctx, "registries_destroyed") == 1);
    REQUIRE(source_counter(ctx, "domains_created") == 1);
    REQUIRE(source_counter(ctx, "domains_destroyed") == 1);
    REQUIRE(source_counter(ctx, "cleanup_commit") == 1);
    REQUIRE(source_counter(ctx, "cleanup_abort") == 0);
    REQUIRE(source_counter(ctx, "release_before_locks") == 1);
    REQUIRE(source_counter(ctx, "release_locks") == 1);
    REQUIRE(source_counter(ctx, "release_after_locks") == 1);
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("DOC0377 nested BatchJoin SliceProject shares and releases source routes",
          "[seq][slice][source-domain][batch-join][route][limit]") {
    constexpr std::string_view target = "doc0377_source_batch_target";
    constexpr std::string_view query = "doc0377_source_batch_query";
    auto ctx = fresh_context({target, query});
    exec_sql(ctx,
        "SET bioseq.update_index_on_insert=off;"
        "SET bioseq.read_sequence_index=off;"
        "SET bioseq.index_async_persist=off;"
        "SET bioseq.recompute_async_persist=off;"
        "SET bioseq.enable_vectorization=off;"
        "SET bioseq.aligner.minimizer_algorithm='scalar';"
        "SET bioseq.aligner.chaining_algorithm='scalar_mm2';"
        "SET bioseq.aligner.base_alignment_algorithm='ksw2';"
        "SET bioseq.aligner.vulkan_enabled=off;"
        "CREATE TABLE doc0377_source_batch_target "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "CREATE TABLE doc0377_source_batch_query "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;");

    const std::string target_sequence =
        source_domain_high_entropy_dna4(16384u);
    const std::string query_sequence = target_sequence.substr(4096u, 4096u);
    exec_sqlf(ctx,
        "INSERT INTO doc0377_source_batch_target VALUES "
        " (1, '{}'::bioseq.seq('dna4',32));"
        "INSERT INTO doc0377_source_batch_query VALUES "
        " (1, '{}'::bioseq.seq('dna4',32));",
        target_sequence, query_sequence);

    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) "
        "FROM doc0377_source_batch_target AS t "
        "JOIN doc0377_source_batch_query AS q "
        "ON mmsearch(t.s, q.s, "
        " aligner_options(index_read_requirement => 1));") == 1);
    require_source_resources_balanced(ctx);

    constexpr auto sql =
        "SELECT bioseq.len((t.s[0:16] OPERATOR(bioseq.||) q.s[0:16])[4:28]) "
        "FROM doc0377_source_batch_target AS t "
        "JOIN doc0377_source_batch_query AS q "
        "ON mmsearch(t.s[0:], q.s[0:], "
        " aligner_options(index_read_requirement => 1)) "
        "LIMIT 1;";
    const auto plan = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));

    reset_source_counters(ctx);
    REQUIRE(query_scalar_int(ctx, sql) == 24);
    REQUIRE(source_counter(ctx, "registries_created") >= 2);
    REQUIRE(source_counter(ctx, "domains_created") >= 2);
    REQUIRE(source_counter(ctx, "leases_acquired") >= 2);
    REQUIRE(source_counter(ctx, "provider_closed_with_upper_leases") >= 1);
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);
}

TEST_CASE("DOC0377 materialized spill aggregate COPY generated and parallel routes are safe",
          "[seq][slice][source-domain][spill][aggregate][copy][generated]"
          "[materialized-view][parallel]") {
    constexpr std::string_view source = "doc0377_source_boundaries";
    constexpr std::string_view generated = "doc0377_source_generated";
    auto ctx = fresh_context({generated, source});
    exec_sql(ctx,
        "SET work_mem='64kB';"
        "CREATE TABLE doc0377_source_boundaries "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO doc0377_source_boundaries "
        "SELECT g, repeat('ACGT',2048)::bioseq.seq('dna4',32) "
        "FROM generate_series(1,96) AS g;");

    reset_source_counters(ctx);
    REQUIRE(query_scalar_int(ctx,
        "SELECT sum(bioseq.len(s[101:133])) "
        "FROM doc0377_source_boundaries;") == 3072);
    require_source_resources_balanced(ctx);

    reset_source_counters(ctx);
    constexpr auto spill_sql =
        "WITH fragments AS MATERIALIZED ("
        " SELECT id, s[0:4096] AS fragment "
        " FROM doc0377_source_boundaries"
        ") SELECT sum(bioseq.len(fragment)) FROM fragments;";
    const auto spill_plan = explain_text<true>(ctx, spill_sql);
    REQUIRE((plan_contains(spill_plan, "temp written=")
             || plan_contains(spill_plan, "temp read=")));
    require_source_resources_balanced(ctx);

    reset_source_counters(ctx);
    REQUIRE(query_scalar_int(ctx, spill_sql) == 393216);
    REQUIRE(source_counter(ctx, "domains_created") == 1);
    require_source_resources_balanced(ctx);

    constexpr auto ordered_aggregate_sql =
        "SELECT sum(bioseq.len(fragment)) "
        "FROM ("
        " SELECT unnest(array_agg(s[0:128] ORDER BY id)) AS fragment "
        " FROM doc0377_source_boundaries"
        ") AS aggregate_state;";
    const auto ordered_aggregate_plan =
        explain_text<false>(ctx, ordered_aggregate_sql);
    REQUIRE(plan_line_count(ordered_aggregate_plan,
        "Custom Scan (BioseqSliceProject)") == 1u);
    const auto ordered_owner = plan_line_index(
        ordered_aggregate_plan, "Custom Scan (BioseqSliceProject)");
    const auto ordered_sort = plan_line_index(
        ordered_aggregate_plan, "Sort");
    REQUIRE(ordered_owner.has_value());
    REQUIRE(ordered_sort.has_value());
    REQUIRE(*ordered_owner < *ordered_sort);
    REQUIRE(plan_contains(ordered_aggregate_plan,
        "Sort Key: doc0377_source_boundaries.id"));
    REQUIRE_FALSE(plan_contains(
        ordered_aggregate_plan, "Custom Scan (VectorAggNode)"));

    reset_source_counters(ctx);
    REQUIRE(query_scalar_int(ctx, ordered_aggregate_sql) == 12288);
    REQUIRE(source_counter(ctx, "domains_created") == 1);
    require_source_resources_balanced(ctx);

    reset_source_counters(ctx);
    const auto copied = run_copy_out(ctx,
        "COPY (SELECT id, bioseq.to_text(s[0:8]) "
        "      FROM doc0377_source_boundaries "
        "      WHERE id<=3 ORDER BY id) TO STDOUT");
    REQUIRE(copied.started);
    REQUIRE(copied.stream_ok);
    REQUIRE(copied.command_ok);
    REQUIRE(copied.bytes ==
        "1\tACGTACGT\n2\tACGTACGT\n3\tACGTACGT\n");
    REQUIRE(source_counter(ctx, "domains_created") == 1);
    require_source_resources_balanced(ctx);

    reset_source_counters(ctx);
    const auto copied_binary = run_copy_out(ctx,
        "COPY (SELECT id, s[0:64] "
        "      FROM doc0377_source_boundaries "
        "      WHERE id<=3 ORDER BY id) TO STDOUT (FORMAT binary)");
    REQUIRE(copied_binary.started);
    REQUIRE(copied_binary.stream_ok);
    REQUIRE(copied_binary.command_ok);
    REQUIRE(copied_binary.bytes.size() > 21);
    REQUIRE(copied_binary.bytes.compare(
        0, 11, std::string("PGCOPY\n\xff\r\n\0", 11)) == 0);
    REQUIRE(copied_binary.bytes.compare(
        copied_binary.bytes.size() - 2, 2,
        std::string("\xff\xff", 2)) == 0);
    REQUIRE(source_counter(ctx, "domains_created") == 1);
    require_source_resources_balanced(ctx);

    reset_source_counters(ctx);
    exec_sql(ctx,
        "CREATE TABLE doc0377_source_generated "
        " (id int, s bioseq.seq('dna4',32), "
        "  prefix text GENERATED ALWAYS AS "
        "    (bioseq.to_text(s[0:8])) STORED) USING bioseq;"
        "INSERT INTO doc0377_source_generated(id,s) "
        "SELECT id, s[0:] FROM doc0377_source_boundaries WHERE id<=2;");
    REQUIRE(query_scalar(ctx,
        "SELECT string_agg(prefix,',' ORDER BY id) "
        "FROM doc0377_source_generated;") == "ACGTACGT,ACGTACGT");
    REQUIRE(source_counter(ctx, "domains_created") >= 1);
    require_source_resources_balanced(ctx);

    reset_source_counters(ctx);
    exec_sql(ctx,
        "CREATE MATERIALIZED VIEW doc0377_source_mv AS "
        "SELECT id, s[0:128]::bioseq.seq('dna4',32) AS fragment "
        "FROM doc0377_source_boundaries WHERE id<=4;");
    REQUIRE(query_scalar_int(ctx,
        "SELECT sum(bioseq.len(fragment)) FROM doc0377_source_mv;") == 512);
    REQUIRE(source_counter(ctx, "domains_created") >= 1);
    require_source_resources_balanced(ctx);

    const long long max_workers = query_scalar_int(ctx,
        "SELECT current_setting('max_parallel_workers')::int;");
    if (max_workers > 0) {
        exec_sql(ctx,
            "SET max_parallel_workers_per_gather=2;"
            "SET min_parallel_table_scan_size=0;"
            "SET parallel_setup_cost=0;"
            "SET parallel_tuple_cost=0;"
            "SET parallel_leader_participation=off;"
            "DO $$ BEGIN "
            " IF EXISTS (SELECT 1 FROM pg_settings "
            "            WHERE name='debug_parallel_query') THEN "
            "   PERFORM set_config('debug_parallel_query','on',false);"
            " ELSIF EXISTS (SELECT 1 FROM pg_settings "
            "               WHERE name='force_parallel_mode') THEN "
            "   PERFORM set_config('force_parallel_mode','on',false);"
            " END IF; END $$;");
        constexpr auto parallel_sql =
            "SELECT sum(bioseq.len(s[0:64])) "
            "FROM doc0377_source_boundaries;";
        const auto parallel_plan = explain_text<false>(ctx, parallel_sql);
        REQUIRE(plan_contains(parallel_plan, "Gather"));

        /* Leader participation is disabled, so these actual counters can
         * reach the leader only through SliceProject's DSM worker slots.  A
         * full 96-call aggregate is process-shared evidence that the workers
         * collectively executed every source-backed slice and conversion. */
        constexpr auto worker_materialization_sql =
            "SELECT bioseq.to_text(s[0:64]) "
            "FROM doc0377_source_boundaries;";
        reset_source_counters(ctx);
        const auto worker_plan = explain_text<true>(
            ctx, worker_materialization_sql);
        REQUIRE(plan_contains(worker_plan, "Gather"));
        REQUIRE(plan_contains(
            worker_plan, "Custom Scan (BioseqSliceProject)"));
        REQUIRE(plan_line_index(worker_plan, "Gather")
                < plan_line_index(
                    worker_plan, "Custom Scan (BioseqSliceProject)"));
        REQUIRE(source_boundary_actual_calls(
            worker_plan, "text", "SQL conversion") == 96u);
        REQUIRE_FALSE(plan_contains(worker_plan, "Incomplete Workers"));
        REQUIRE_FALSE(plan_contains(worker_plan, "Counter Overflow: true"));
        REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
        require_source_resources_balanced(ctx);

        reset_source_counters(ctx);
        REQUIRE(query_scalar_int(ctx, parallel_sql) == 6144);
        REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
        require_source_resources_balanced(ctx);
    }
}

TEST_CASE("DOC0377 ordinary CTAS owns transformed source values",
          "[seq][slice][source-domain][ctas][materialization]") {
    constexpr std::string_view source = "doc0377_source_ctas_input";
    constexpr std::string_view destination = "doc0377_source_ctas_output";
    auto ctx = fresh_context({destination, source});
    exec_sql(ctx,
        "SET max_parallel_workers_per_gather=0;"
        "SET bioseq.update_index_on_insert=off;"
        "CREATE TABLE doc0377_source_ctas_input "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO doc0377_source_ctas_input VALUES "
        " (7, repeat('ACGT',4096)::bioseq.seq('dna4',32));");

    constexpr auto ctas_sql =
        "CREATE TABLE doc0377_source_ctas_output USING heap AS "
        "SELECT id, "
        " (s[4093:4101] OPERATOR(bioseq.||) "
        "  bioseq.reverse(s[8190:8194]))::bioseq.seq('dna4',32) "
        "  AS fragment "
        "FROM doc0377_source_ctas_input WHERE id=7;";
    reset_source_counters(ctx);
    exec_sql(ctx,
        "SELECT bioseq_internal.slice_payload_allocation_probe(0);");
    const auto ctas_plan = explain_text<true>(ctx, ctas_sql);
    REQUIRE(plan_contains(
        ctas_plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(ctas_plan, "Slice Materialization:"));
    /* CTAS is planned as CMD_SELECT, so its declared result-datum boundaries
     * are the two client serialization formats.  The CTAS receiver invokes
     * neither; core heap ownership performs exactly one packed-full executor
     * copy instead.  Keep both halves exact so a client-output detour or a
     * second payload copy cannot masquerade as durable ownership. */
    REQUIRE(plan_line_count(ctas_plan, "Mechanism: text") == 1u);
    REQUIRE(plan_line_count(ctas_plan, "Mechanism: binary") == 1u);
    REQUIRE(plan_line_count(ctas_plan, "Reason: client output") == 2u);
    REQUIRE(source_boundary_actual_calls(
        ctas_plan, "text", "client output") == 0u);
    REQUIRE(source_boundary_actual_calls(
        ctas_plan, "binary", "client output") == 0u);
    REQUIRE(query_scalar_int(ctx,
        "SELECT bioseq_internal.slice_payload_allocation_probe(1);") == 1);
    REQUIRE(query_scalar_int(ctx,
        "SELECT bioseq_internal.slice_payload_allocation_probe(2);") == 1);
    REQUIRE(query_scalar_int(ctx,
        "SELECT bioseq_internal.slice_payload_allocation_probe(3);") == 0);
    REQUIRE(query_scalar_int(ctx,
        "SELECT bioseq_internal.slice_payload_allocation_probe(4);") == 0);
    REQUIRE(source_counter(ctx, "domains_created") >= 1);
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);

    /* The durable heap tuple must no longer depend on the provider, relation,
     * statement registry, or snapshot that produced the lazy expression. */
    exec_sql(ctx, "DROP TABLE doc0377_source_ctas_input;");
    reset_source_counters(ctx);
    const auto durable = query_one(ctx,
        "SELECT bioseq.len(fragment)::text, "
        "       bioseq.to_text(fragment), "
        "       encode(bioseq.seq_send(fragment),'hex'), "
        "       encode(bioseq.seq_send("
        "         'CGTACGTACATG'::bioseq.seq('dna4',32)),'hex') "
        "FROM doc0377_source_ctas_output WHERE id=7;");
    REQUIRE(durable.size() == 4u);
    REQUIRE(durable[0] == "12");
    REQUIRE(durable[1] == "CGTACGTACATG");
    REQUIRE(durable[2].has_value());
    REQUIRE(durable[2] == durable[3]);
    REQUIRE(source_counter(ctx, "domains_created") == 0);
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);
}

TEST_CASE("DOC0377 caught late binding errors release sliced source ownership",
          "[seq][slice][cast][concat][source-domain][error-atomic][admission]") {
    constexpr std::string_view table = "doc0377_source_error_atomic";
    auto ctx = fresh_context({table});
    exec_sql(ctx,
        "SET max_parallel_workers_per_gather=0;"
        "SET bioseq.update_index_on_insert=off;"
        "CREATE TABLE doc0377_source_error_atomic "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO doc0377_source_error_atomic VALUES "
        " (1, repeat('ACGT',4096)::bioseq.seq('dna4',32));"
        "CREATE FUNCTION pg_temp.doc0377_source_runtime_text(value text) "
        "RETURNS text LANGUAGE plpgsql VOLATILE STRICT AS $$ "
        "BEGIN RETURN value; END $$;");

    /* Expression evaluation opens the source-backed left edge before the
     * volatile RHS conversion.  The level-0 public profile's runtime input
     * parser rejects N while constructing the intermediate unbound sequence.
     * Richer profiles admit DNA5, then resolve_concat_type rejects the bound
     * DNA4/DNA5 pair before literal-ledger construction.  The exact
     * source-domain counts below prove that neither error happened before the
     * provider-backed left edge was opened. */
#if BIOSEQ_OPTTIMIZATION_LEVEL == 0
    constexpr std::string_view late_rhs =
        "pg_temp.doc0377_source_runtime_text('N')"
        "::bioseq.seq('dna4',32)";
    constexpr std::string_view expected_sqlstate = "22P02";
    constexpr std::string_view expected_message =
        "invalid symbol at byte 0 in unbound bioseq.seq";
    constexpr std::string_view expected_condition =
        "invalid_text_representation";
#else
    constexpr std::string_view late_rhs =
        "pg_temp.doc0377_source_runtime_text('N')"
        "::bioseq.seq('dna5',32)";
    constexpr std::string_view expected_sqlstate = "42804";
    constexpr std::string_view expected_message =
        "cannot concatenate bioseq alphabets dna4 and dna5";
    constexpr std::string_view expected_condition = "datatype_mismatch";
#endif
    const auto late_error_query = fmt::format(
        "SELECT sliced OPERATOR(bioseq.||) "
        "       {} "
        "FROM ("
        " SELECT s[17:49] AS sliced "
        " FROM doc0377_source_error_atomic WHERE id=1 OFFSET 0"
        ") AS source_row;",
        late_rhs);
    const auto planned = explain_text<false>(ctx, late_error_query);
    REQUIRE(plan_contains(
        planned, "Custom Scan (BioseqSliceProject)"));

    reset_source_counters(ctx);
    const auto preflight = sql_error(ctx, late_error_query);
    REQUIRE(preflight.sqlstate == expected_sqlstate);
    REQUIRE(preflight.contains(expected_message));
    REQUIRE(source_counter(ctx, "domains_created") == 1);
    REQUIRE(source_counter(ctx, "domains_destroyed") == 1);
    require_source_resources_balanced(ctx);

    const auto admission_baseline = query_scalar_int(ctx,
        "SELECT index_memory_reserved_bytes "
        "FROM bioseq.scheduler_stats();");
    reset_source_counters(ctx);
    exec_sql(ctx, fmt::format(
        "DO $doc0377$ "
        "DECLARE iteration integer; "
        "BEGIN "
        "  FOR iteration IN 1..32 LOOP "
        "    BEGIN "
        "      PERFORM sliced OPERATOR(bioseq.||) "
        "              {} "
        "      FROM ("
        "       SELECT s[17:49] AS sliced "
        "       FROM doc0377_source_error_atomic WHERE id=1 OFFSET 0"
        "      ) AS source_row; "
        "      RAISE EXCEPTION 'expected late sequence binding error'; "
        "    EXCEPTION WHEN {} THEN "
        "      NULL; "
        "    END; "
        "  END LOOP; "
        "END $doc0377$;",
        late_rhs, expected_condition));

    REQUIRE(source_counter(ctx, "domains_created") == 32);
    REQUIRE(source_counter(ctx, "domains_destroyed") == 32);
    REQUIRE(source_counter(ctx, "refcount_faults") == 0);
    REQUIRE(source_counter(ctx, "lease_faults") == 0);
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);
    REQUIRE(query_scalar_int(ctx,
        "SELECT index_memory_reserved_bytes "
        "FROM bioseq.scheduler_stats();") == admission_baseline);
}

TEST_CASE("DOC0377 BSS3 keeps child resnos distinct from physical attnos",
          "[seq][slice][source-domain][bss3][namespace]") {
    constexpr std::string_view table = "doc0377_bss3_attno_namespace";
    auto ctx = fresh_context({table});
    exec_sql(ctx,
        "SET bioseq.update_index_on_insert=off;"
        "CREATE TABLE doc0377_bss3_attno_namespace ("
        " pad bioseq.seq('dna4',32),"
        " id int,"
        " wanted bioseq.seq('dna4',32)"
        ") USING bioseq;"
        "INSERT INTO doc0377_bss3_attno_namespace VALUES ("
        " repeat('A',4096)::bioseq.seq('dna4',32),"
        " 7,"
        " repeat('TGCA',1024)::bioseq.seq('dna4',32)"
        ");");

    /* The scan projects only wanted: child resno 1 therefore names physical
     * attribute 3, while physical attribute 1 is a different SEQ locator.
     * Confusing those namespaces returns the pad slice (AAAAAAAA). */
    constexpr auto sql =
        "SELECT bioseq.to_text(wanted[0:8]) "
        "FROM doc0377_bss3_attno_namespace;";
    const auto planned = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(planned, "Custom Scan (BioseqSliceProject)"));

    reset_source_counters(ctx);
    REQUIRE(query_scalar(ctx, sql) == "TGCATGCA");
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);

    reset_source_counters(ctx);
    const auto analyzed = explain_text<true>(ctx, sql);
    REQUIRE(plan_contains(analyzed, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(source_boundary_actual_calls(
        analyzed, "text", "SQL conversion") == 1u);
    REQUIRE_FALSE(plan_contains(analyzed, "Counter Overflow: true"));
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);
}

TEST_CASE("DOC0377 parallel lazy providers adopt the flushed scan snapshot",
          "[seq][slice][source-domain][parallel][snapshot][pending-write]") {
    constexpr std::string_view table = "doc0377_parallel_source_snapshot";
    auto ctx = fresh_context({table});
    REQUIRE(query_scalar_int(ctx,
        "SELECT current_setting('max_parallel_workers')::int;") > 0);
    exec_sql(ctx,
        "SET bioseq.update_index_on_insert=off;"
        "SET bioseq.enable_parallel_execution=on;"
        "SET bioseq.min_parallel_processes=2;"
        "SET max_parallel_workers_per_gather=2;"
        "SET min_parallel_table_scan_size=0;"
        "SET parallel_setup_cost=0;"
        "SET parallel_tuple_cost=0;"
        "SET parallel_leader_participation=off;"
        "DO $$ BEGIN "
        " IF EXISTS (SELECT 1 FROM pg_settings "
        "            WHERE name='debug_parallel_query') THEN "
        "   PERFORM set_config('debug_parallel_query','on',false);"
        " ELSIF EXISTS (SELECT 1 FROM pg_settings "
        "               WHERE name='force_parallel_mode') THEN "
        "   PERFORM set_config('force_parallel_mode','on',false);"
        " END IF; END $$;");
    exec_sql(ctx,
        "BEGIN;"
        "CREATE TABLE doc0377_parallel_source_snapshot "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq "
        " WITH (autovacuum_enabled=false);"
        "INSERT INTO doc0377_parallel_source_snapshot "
        "SELECT g, repeat('TGCA',2048)::bioseq.seq('dna4',32) "
        "FROM generate_series(1,64) AS g;"
        "COMMIT;");
    exec_sql(ctx,
        "BEGIN;"
        "INSERT INTO doc0377_parallel_source_snapshot "
        "SELECT 1000+g, repeat('ACGT',2048)::bioseq.seq('dna4',32) "
        "FROM generate_series(1,64) AS g;");

    constexpr auto sql =
        "SELECT id,bioseq.to_text(s[0:8]) "
        "FROM doc0377_parallel_source_snapshot;";
    const auto planned = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(planned, "Gather"));
    REQUIRE(plan_contains(planned, "Workers Planned: 2"));
    REQUIRE_FALSE(plan_contains(planned, "Single Copy: true"));
    REQUIRE(plan_line_count(
        planned, "Custom Scan (BioseqSliceProject)") == 1u);
    const auto gather = plan_line_index(planned, "Gather");
    const auto owner = plan_line_index(
        planned, "Custom Scan (BioseqSliceProject)");
    REQUIRE(gather.has_value());
    REQUIRE(owner.has_value());
    REQUIRE(*gather < *owner);

    rowset_t expected;
    expected.reserve(128u);
    for (int id = 1; id <= 64; ++id) {
        expected.push_back({
            std::optional<std::string>{std::to_string(id)},
            std::optional<std::string>{"TGCATGCA"},
        });
    }
    for (int id = 1001; id <= 1064; ++id) {
        expected.push_back({
            std::optional<std::string>{std::to_string(id)},
            std::optional<std::string>{"ACGTACGT"},
        });
    }
    std::sort(expected.begin(), expected.end());
    reset_source_counters(ctx);
    auto actual = query_rows(ctx, sql);
    std::sort(actual.begin(), actual.end());
    REQUIRE(actual == expected);
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);
    exec_sql(ctx, "COMMIT;");
}

TEST_CASE("DOC0377 parallel stripe claiming skips aborted same-relation gaps",
          "[seq][slice][source-domain][parallel][dsm][stripe-gap]") {
    constexpr std::string_view table = "doc0377_parallel_stripe_gaps";
    auto ctx = fresh_context({table});
    exec_sql(ctx,
        "SET bioseq.update_index_on_insert=off;"
        "SET bioseq.stripe_row_limit=1000;"
        "SET bioseq.chunk_group_row_limit=1000;"
        "CREATE TABLE doc0377_parallel_stripe_gaps "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;");

    /* Stripe ids are relation-local.  A first row reserves the next id in the
     * relation metapage, which is not rolled back with its MVCC metadata row.
     * Burn ids 2 and 4 in aborted transactions so the reader must claim
     * visible stripes 1, 3 and 5 without duplicating a gap successor. */
    exec_sql(ctx,
        "INSERT INTO doc0377_parallel_stripe_gaps "
        "SELECT g, repeat('ACGT',16)::bioseq.seq('dna4',32) "
        "FROM generate_series(1,1000) AS g;");
    exec_sql(ctx,
        "BEGIN;"
        "INSERT INTO doc0377_parallel_stripe_gaps VALUES (-1,NULL);"
        "ROLLBACK;");
    exec_sql(ctx,
        "INSERT INTO doc0377_parallel_stripe_gaps "
        "SELECT g, repeat('TGCA',16)::bioseq.seq('dna4',32) "
        "FROM generate_series(1001,2000) AS g;");
    exec_sql(ctx,
        "BEGIN;"
        "INSERT INTO doc0377_parallel_stripe_gaps VALUES (-2,NULL);"
        "ROLLBACK;");
    exec_sql(ctx,
        "INSERT INTO doc0377_parallel_stripe_gaps "
        "SELECT g, repeat('GATT',16)::bioseq.seq('dna4',32) "
        "FROM generate_series(2001,3000) AS g;");

    REQUIRE(query_int_column(ctx,
        "SELECT stripe_num FROM bioseq.stripe "
        "WHERE relation='doc0377_parallel_stripe_gaps'::regclass "
        "ORDER BY stripe_num;") == std::vector<long long>{1, 3, 5});
    REQUIRE(query_scalar_int(ctx,
        "SELECT current_setting('max_parallel_workers')::int;") > 0);
    exec_sql(ctx,
        "SET bioseq.enable_parallel_execution=on;"
        "SET max_parallel_workers_per_gather=2;"
        "SET min_parallel_table_scan_size=0;"
        "SET parallel_setup_cost=0;"
        "SET parallel_tuple_cost=0;"
        "SET parallel_leader_participation=off;"
        "DO $$ BEGIN "
        " IF EXISTS (SELECT 1 FROM pg_settings "
        "            WHERE name='debug_parallel_query') THEN "
        "   PERFORM set_config('debug_parallel_query','on',false);"
        " ELSIF EXISTS (SELECT 1 FROM pg_settings "
        "               WHERE name='force_parallel_mode') THEN "
        "   PERFORM set_config('force_parallel_mode','on',false);"
        " END IF; END $$;");

    constexpr auto exact_sql =
        "SELECT id,bioseq.to_text(s[0:8]) "
        "FROM doc0377_parallel_stripe_gaps ORDER BY id;";
    const auto planned = explain_text<false>(ctx, exact_sql);
    REQUIRE(plan_contains(planned, "Gather"));
    REQUIRE(plan_contains(planned, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_line_index(planned, "Gather")
            < plan_line_index(
                planned, "Custom Scan (BioseqSliceProject)"));

    rowset_t expected;
    expected.reserve(3000u);
    for (long long id = 1; id <= 3000; ++id) {
        const std::string prefix = id <= 1000
            ? "ACGTACGT" : id <= 2000 ? "TGCATGCA" : "GATTGATT";
        expected.push_back({
            std::optional<std::string>{std::to_string(id)},
            std::optional<std::string>{prefix},
        });
    }

    reset_source_counters(ctx);
    const auto analyzed = explain_text<true>(ctx,
        "SELECT bioseq.to_text(s[0:8]) "
        "FROM doc0377_parallel_stripe_gaps;");
    REQUIRE(plan_contains(analyzed, "Gather"));
    REQUIRE(plan_contains(
        analyzed, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(source_boundary_actual_calls(
        analyzed, "text", "SQL conversion") == 3000u);
    REQUIRE_FALSE(plan_contains(analyzed, "Incomplete Workers"));
    REQUIRE_FALSE(plan_contains(analyzed, "Counter Overflow: true"));
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);

    for (int execution = 0; execution < 2; ++execution) {
        reset_source_counters(ctx);
        REQUIRE(query_rows(ctx, exact_sql) == expected);
        REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
        require_source_resources_balanced(ctx);
    }
}

TEST_CASE("DOC0377 parallel worker ERROR preserves DSM cleanup and original error",
          "[seq][slice][source-domain][parallel][dsm][error]") {
    constexpr std::string_view source = "doc0377_slice_worker_error";
    auto ctx = fresh_context({source});
    exec_sql(ctx,
        "CREATE TABLE doc0377_slice_worker_error "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO doc0377_slice_worker_error "
        "SELECT g, repeat('ACGT',2048)::bioseq.seq('dna4',32) "
        "FROM generate_series(1,256) AS g;");
    REQUIRE(query_scalar_int(ctx,
        "SELECT current_setting('max_parallel_workers')::int;") > 0);
    exec_sql(ctx,
        "SET max_parallel_workers_per_gather=2;"
        "SET min_parallel_table_scan_size=0;"
        "SET parallel_setup_cost=0;"
        "SET parallel_tuple_cost=0;"
        "SET parallel_leader_participation=off;"
        "DO $$ BEGIN "
        " IF EXISTS (SELECT 1 FROM pg_settings "
        "            WHERE name='debug_parallel_query') THEN "
        "   PERFORM set_config('debug_parallel_query','on',false);"
        " ELSIF EXISTS (SELECT 1 FROM pg_settings "
        "               WHERE name='force_parallel_mode') THEN "
        "   PERFORM set_config('force_parallel_mode','on',false);"
        " END IF; END $$;");

    constexpr auto error_sql =
        "SELECT bioseq.to_text(s[0:64]) "
        "FROM doc0377_slice_worker_error "
        "WHERE bioseq_internal.slice_trace_worker_failure_probe(id);";
    const auto planned = explain_text<false>(ctx, error_sql);
    REQUIRE(plan_contains(planned, "Gather"));
    REQUIRE(plan_contains(
        planned, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(
        planned, "slice_trace_worker_failure_probe"));

    reset_source_counters(ctx);
    const auto injected = sql_error(ctx, error_sql);
    REQUIRE(injected.sqlstate == "XX000");
    REQUIRE(injected.contains(
        "injected DOC0377 SliceProject parallel worker failure"));
    require_source_resources_balanced(ctx);
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);

    /* A fresh forced-parallel cycle must attach, publish and detach cleanly
     * after the failed cycle.  With leader participation disabled, all 256
     * successful conversion deltas must arrive through worker DSM slots. */
    reset_source_counters(ctx);
    const auto recovered = explain_text<true>(ctx,
        "SELECT bioseq.to_text(s[0:64]) "
        "FROM doc0377_slice_worker_error;");
    REQUIRE(plan_contains(recovered, "Gather"));
    REQUIRE(plan_contains(
        recovered, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(source_boundary_actual_calls(
        recovered, "text", "SQL conversion") == 256u);
    REQUIRE_FALSE(plan_contains(recovered, "Incomplete Workers"));
    REQUIRE_FALSE(plan_contains(recovered, "Counter Overflow: true"));
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);
}

TEST_CASE("DOC0377 production source routes never invoke PostgreSQL off backend",
          "[seq][slice][source-domain][thread-tripwire][scheduler][index-admin]"
          "[index-chunked][batch-sort][custom-scan]") {
    constexpr std::string_view table = "doc0377_source_routes";
    auto ctx = fresh_context({table});
    exec_sql(ctx,
        "SET bioseq.update_index_on_insert=off;"
        "SET bioseq.read_sequence_index=off;"
        "SET bioseq.index_async_persist=off;"
        "SET bioseq.recompute_async_persist=off;"
        "SET bioseq.aligner.minimizer_algorithm='scalar';"
        "SET bioseq.aligner.chaining_algorithm='scalar_mm2';"
        "SET bioseq.aligner.base_alignment_algorithm='ksw2';"
        "SET bioseq.aligner.vulkan_enabled=off;"
        "SET bioseq.scheduler.pipeline_concurrency_mode='task_parallel';"
        "CREATE TABLE doc0377_source_routes "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;");
    const std::string route_target_sequence =
        source_domain_high_entropy_dna4(65536u);
    const std::string route_query_sequence =
        route_target_sequence.substr(32768u, 128u);
    exec_sqlf(ctx,
        "INSERT INTO doc0377_source_routes VALUES "
        " (1, '{}'::bioseq.seq('dna4',32)),"
        " (2, repeat('T',65536)::bioseq.seq('dna4',32));",
        route_target_sequence);

    reset_source_counters(ctx);
    REQUIRE(query_scalar(ctx,
        "SELECT bioseq.to_text(s[32765:32773]) "
        "FROM doc0377_source_routes WHERE id=1;")
        == route_target_sequence.substr(32765u, 8u));
    REQUIRE(source_counter(ctx, "scan_providers_created") == 1);
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);

    /* Exercise all four lazy-facade cursor orientations on a non-centered,
     * non-palindromic subrange.  The planner proof and source counters pin the
     * diagnostic to a real provider-backed expanded value; the probe flattens
     * only a separate compact oracle. */
    constexpr auto orientation_sql =
        "SELECT bioseq_internal.sequence_facade_orientation_probe("
        "s[32749:32786]) FROM doc0377_source_routes WHERE id=1;";
    const auto orientation_plan = explain_text<false>(ctx, orientation_sql);
    REQUIRE(plan_contains(
        orientation_plan, "Custom Scan (BioseqSliceProject)"));
    reset_source_counters(ctx);
    REQUIRE(query_scalar(ctx, orientation_sql) == "t");
    REQUIRE(source_counter(ctx, "scan_providers_created") == 1);
    REQUIRE(source_counter(ctx, "domains_created") >= 1);
    REQUIRE(source_counter(ctx, "scanner_entries_created") >= 1);
    REQUIRE(source_counter(ctx, "facade_source_stream_callbacks") > 0);
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);

    exec_sql(ctx, "SET bioseq.enable_sequence_sort_scan=on;");
    const auto sort_plan = explain_text<false>(ctx,
        "SELECT id FROM doc0377_source_routes ORDER BY s;");
    REQUIRE(plan_contains(sort_plan, "BioseqBatchSortScan"));
    reset_source_counters(ctx);
    REQUIRE(query_int_column(ctx,
        "SELECT id FROM doc0377_source_routes ORDER BY s;")
        == std::vector<long long>{1, 2});
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);

    reset_source_counters(ctx);
    REQUIRE(query_scalar_int(ctx,
        "SELECT bioseq.update_index('doc0377_source_routes'::regclass, "
        " 'minimizers_per_fragment');") == 2);
    REQUIRE(source_counter(ctx, "standalone_providers_created") >= 1);
    REQUIRE(source_counter(ctx, "domains_created") >= 1);
    REQUIRE(source_counter(ctx, "scanner_entries_created") >= 1);
    REQUIRE(source_counter(ctx, "facade_source_stream_callbacks") > 0);
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);

    reset_source_counters(ctx);
    const auto matches = query_int_column(ctx, fmt::format(
        "SELECT id FROM doc0377_source_routes "
        "WHERE mmsearch(s, "
        " '{}'::bioseq.seq('dna4',32), "
        " aligner_options(index_read_requirement => 1)) "
        "ORDER BY id;",
        route_query_sequence));
    REQUIRE(matches == std::vector<long long>{1});
    REQUIRE(source_counter(ctx, "domains_created") >= 1);
    REQUIRE(source_counter(ctx, "scanner_entries_created") >= 1);
    REQUIRE(source_counter(ctx, "facade_source_stream_callbacks") > 0);

    /* The source callbacks above are backend-thread-only.  This immutable
     * execution snapshot proves the selected scalar tuple completed without
     * fallback or unattested work while the task-parallel pool exposed at
     * least two CPU slots.  The cross-thread return-value probe below is the
     * authoritative worker-thread tripwire evidence; the backend-local TLS
     * counter is not used as a substitute for it. */
    const auto task_evidence = query_one(ctx,
        "SELECT effective_cpu_workers::text, requested_backend, "
        "       actual_backend, completed::text, partial::text, "
        "       fully_drained::text, attempted_alignment_jobs::text, "
        "       fully_attested_alignment_jobs::text, "
        "       unattested_alignment_jobs::text, eligible_jobs::text, "
        "       executed_scalar_cpu_jobs::text, "
        "       executed_simd_cpu_jobs::text, "
        "       executed_vulkan_gpu_jobs::text, fallback_jobs::text, "
        "       backend_declines::text, backend_fallbacks::text "
        "FROM bioseq.last_alignment_execution_stats() "
        "WHERE stage='query_sketch';");
    REQUIRE(task_evidence.size() == 16u);
    for (const auto& value : task_evidence) REQUIRE(value.has_value());
    REQUIRE(std::stoll(*task_evidence[0]) >= 2);
    REQUIRE(*task_evidence[1] == "scalar_cpu");
    REQUIRE(*task_evidence[2] == "scalar_cpu");
    REQUIRE(*task_evidence[3] == "true");
    REQUIRE(*task_evidence[4] == "false");
    REQUIRE(*task_evidence[5] == "true");
    REQUIRE(std::stoll(*task_evidence[6]) > 0);
    REQUIRE(*task_evidence[7] == *task_evidence[6]);
    REQUIRE(*task_evidence[8] == "0");
    REQUIRE(std::stoll(*task_evidence[9]) > 0);
    REQUIRE(*task_evidence[10] == *task_evidence[9]);
    for (const std::size_t zero : {11u, 12u, 13u, 14u, 15u}) {
        REQUIRE(*task_evidence[zero] == "0");
    }
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 0);
    require_source_resources_balanced(ctx);

    reset_source_counters(ctx);
    /* The SQL boolean carries the status written by the spawned std::thread
     * after join.  The TLS counter increment is deliberately replayed on the
     * backend thread and remains only a local diagnostic assertion. */
    REQUIRE(query_scalar(
        ctx, "SELECT bioseq_internal.sequence_source_wrong_thread_probe();")
        == "t");
    REQUIRE(source_counter(ctx, "wrong_thread_rejections") == 1);
    REQUIRE(source_counter(ctx, "fault_injections") == 0);
    REQUIRE(source_counter(ctx, "cache_sets_created") == 0);
    REQUIRE(source_counter(ctx, "snapshots_registered") == 0);
    require_source_quiescent(ctx);
}

TEST_CASE("DOC0377 locator columns never trust physical minmax metadata",
          "[seq][source-domain][locator][minmax][pruning][group][distinct]"
          "[hash][btree][forged-metadata]") {
    constexpr std::string_view table = "doc0377_locator_minmax";
    auto ctx = fresh_context({table});
    exec_sql(ctx,
        "CREATE TABLE doc0377_locator_minmax "
        " (id int, label text, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO doc0377_locator_minmax VALUES "
        " (1,'a',repeat('A',8192)::bioseq.seq('dna4',32)),"
        " (2,'c',repeat('C',8192)::bioseq.seq('dna4',32)),"
        " (3,'g1',repeat('G',8192)::bioseq.seq('dna4',32)),"
        " (4,'t',repeat('T',8192)::bioseq.seq('dna4',32)),"
        " (5,'a2',repeat('A',8192)::bioseq.seq('dna4',32)),"
        " (6,'g2',repeat('G',8192)::bioseq.seq('dna4',32));");

    const long long sequence_chunks = query_scalar_int(ctx,
        "SELECT count(*) FROM bioseq.chunk "
        "WHERE relation='doc0377_locator_minmax'::regclass "
        "AND attr_num=3;");
    REQUIRE(sequence_chunks > 0);
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) FROM bioseq.chunk "
        "WHERE relation='doc0377_locator_minmax'::regclass "
        "AND attr_num=3 "
        "AND (minimum_value IS NOT NULL OR maximum_value IS NOT NULL);") == 0);
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) FROM bioseq.chunk "
        "WHERE relation='doc0377_locator_minmax'::regclass "
        "AND attr_num=2 "
        "AND minimum_value IS NOT NULL AND maximum_value IS NOT NULL;") > 0);

    const std::string a = "repeat('A',8192)::bioseq.seq('dna4',32)";
    const std::string c = "repeat('C',8192)::bioseq.seq('dna4',32)";
    const std::string g = "repeat('G',8192)::bioseq.seq('dna4',32)";
    REQUIRE(query_int_column(ctx, fmt::format(
        "SELECT id FROM {} WHERE s OPERATOR(bioseq.<) {} ORDER BY id;",
        table, c)) == std::vector<long long>{1, 5});
    REQUIRE(query_int_column(ctx, fmt::format(
        "SELECT id FROM {} WHERE s OPERATOR(bioseq.=) {} ORDER BY id;",
        table, g)) == std::vector<long long>{3, 6});
    REQUIRE(query_int_column(ctx, fmt::format(
        "SELECT id FROM {} WHERE s OPERATOR(bioseq.>) {} ORDER BY id;",
        table, g)) == std::vector<long long>{4});
    const rowset_t expected_groups{{"A"}, {"C"}, {"G"}, {"T"}};
    REQUIRE(query_rows(ctx,
        "SELECT bioseq.to_text(s[0:1]) "
        "FROM doc0377_locator_minmax GROUP BY s ORDER BY s;")
        == expected_groups);
    REQUIRE(query_rows(ctx,
        "SELECT bioseq.to_text(s[0:1]) FROM ("
        " SELECT DISTINCT s FROM doc0377_locator_minmax"
        ") AS distinct_sequences ORDER BY 1;") == expected_groups);

    exec_sql(ctx,
        "SET enable_nestloop=off; SET enable_mergejoin=off; "
        "SET enable_hashjoin=on;");
    const auto hash_plan = explain_text<false>(ctx,
        "SELECT l.id, r.id FROM doc0377_locator_minmax AS l "
        "JOIN doc0377_locator_minmax AS r "
        "ON l.s OPERATOR(bioseq.=) r.s;");
    REQUIRE(plan_contains(hash_plan, "Hash Join"));
    REQUIRE(query_int_column(ctx,
        "SELECT l.id * 10 + r.id FROM doc0377_locator_minmax AS l "
        "JOIN doc0377_locator_minmax AS r "
        "ON l.s OPERATOR(bioseq.=) r.s ORDER BY 1;")
        == std::vector<long long>{11, 15, 22, 33, 36, 44, 51, 55, 63, 66});

    /* Forge hasMinMax=true with structurally valid varlena bytes borrowed from
     * the text column. The seq reader must ignore them before comparison. The
     * transaction-local catalog mutation is rolled back after the read. */
    exec_sql(ctx,
        "BEGIN;"
        "WITH donor AS ("
        " SELECT storage_id, stripe_num, chunk_group_num, "
        "        minimum_value, maximum_value "
        " FROM bioseq_internal.chunk WHERE attr_num=2"
        ") UPDATE bioseq_internal.chunk AS sequence_chunk "
        "SET minimum_value=donor.minimum_value, "
        "    maximum_value=donor.maximum_value "
        "FROM donor "
        "WHERE sequence_chunk.storage_id=donor.storage_id "
        "AND sequence_chunk.stripe_num=donor.stripe_num "
        "AND sequence_chunk.chunk_group_num=donor.chunk_group_num "
        "AND sequence_chunk.attr_num=3 "
        "AND sequence_chunk.storage_id="
        "    bioseq.get_storage_id('doc0377_locator_minmax'::regclass);"
        "SET LOCAL enable_indexscan=off;"
        "SET LOCAL enable_bitmapscan=off;");
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) FROM bioseq.chunk "
        "WHERE relation='doc0377_locator_minmax'::regclass "
        "AND attr_num=3 "
        "AND minimum_value IS NOT NULL AND maximum_value IS NOT NULL;")
        == sequence_chunks);
    REQUIRE(query_int_column(ctx, fmt::format(
        "SELECT id FROM {} WHERE s OPERATOR(bioseq.>) {} ORDER BY id;",
        table, c)) == std::vector<long long>{3, 4, 6});
    exec_sql(ctx, "ROLLBACK;");
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) FROM bioseq.chunk "
        "WHERE relation='doc0377_locator_minmax'::regclass "
        "AND attr_num=3 "
        "AND (minimum_value IS NOT NULL OR maximum_value IS NOT NULL);") == 0);

    exec_sql(ctx,
        "RESET enable_nestloop; RESET enable_mergejoin; RESET enable_hashjoin;"
        "CREATE INDEX doc0377_locator_minmax_btree "
        "ON doc0377_locator_minmax USING btree(s);"
        "SET enable_seqscan=off;");
    REQUIRE(query_int_column(ctx, fmt::format(
        "SELECT id FROM {} WHERE s OPERATOR(bioseq.=) {} ORDER BY id;",
        table, a)) == std::vector<long long>{1, 5});
}

TEST_CASE("DOC0377 mmsearch ownership respects RLS barriers and effectful siblings",
          "[seq][source-domain][rls][security-barrier][leaky][volatile]"
          "[mmsearch][fail-closed]") {
    constexpr std::string_view table = "doc0377_source_security";
    auto ctx = fresh_context({table});
    const auto canonical_rows = hoe_seed_rows();
    const seq_typmod dna4{.alphabet = "dna4", .width = 32};
    const std::string positive_target = format_literal(
        canonical_rows.front().sequences.front(), dna4);
    const std::string alternate_target = format_literal(
        canonical_rows[1].sequences.front(), dna4);
    exec_sql(ctx, fmt::format(
        "SET bioseq.update_index_on_insert=on;"
        "SET bioseq.read_sequence_index=off;"
        "SET bioseq.index_async_persist=off;"
        "SET bioseq.recompute_async_persist=off;"
        "SET bioseq.enable_vectorization=off;"
        "SET bioseq.aligner.minimizer_algorithm='scalar';"
        "SET bioseq.aligner.chaining_algorithm='scalar_mm2';"
        "SET bioseq.aligner.base_alignment_algorithm='ksw2';"
        "SET bioseq.aligner.min_chain_score=20;"
        "SET bioseq.aligner.vulkan_enabled=off;"
        "SET bioseq.scheduler.pipeline_concurrency_mode='task_parallel';"
        "CREATE TABLE doc0377_source_security "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO doc0377_source_security VALUES "
        " (1,{}),(2,{}),(3,{});"
        "CREATE FUNCTION pg_temp.doc0377_nonleaky(i int) "
        "RETURNS bool LANGUAGE sql IMMUTABLE LEAKPROOF "
        "AS 'SELECT i > 0';"
        "CREATE FUNCTION pg_temp.doc0377_leaky(i int) "
        "RETURNS bool LANGUAGE plpgsql STABLE "
        "AS $$ BEGIN RETURN i > 0; END $$;"
        "CREATE FUNCTION pg_temp.doc0377_volatile(i int) "
        "RETURNS bool LANGUAGE plpgsql VOLATILE "
        "AS $$ BEGIN RETURN i > 0; END $$;"
        "CREATE TEMP VIEW doc0377_source_security_barrier "
        "WITH (security_barrier=true) AS "
        "SELECT id,s FROM doc0377_source_security "
        "WHERE pg_temp.doc0377_leaky(id);",
        positive_target, alternate_target, positive_target));

    const std::string mmsearch_call = fmt::format(
        "mmsearch(s, {}, aligner_options(index_read_requirement => 1))",
        positive_target);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM {} WHERE pg_temp.doc0377_nonleaky(id) "
        "AND {};", table, mmsearch_call)) >= 1);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM {} WHERE pg_temp.doc0377_leaky(id) "
        "AND {};", table, mmsearch_call)) >= 1);

    const auto volatile_error = sql_error(ctx, fmt::format(
        "SELECT id FROM {} WHERE {} "
        "AND pg_temp.doc0377_volatile(id);", table, mmsearch_call));
    REQUIRE(volatile_error.sqlstate == "0A000");
    REQUIRE((volatile_error.contains("not owned")
             || volatile_error.contains("no Bioseq executor owner")));

    REQUIRE(query_int_column(ctx,
        "SELECT id FROM doc0377_source_security_barrier "
        "WHERE bioseq.len(s[0:8])=8 ORDER BY id;")
        == std::vector<long long>{1, 2, 3});
    const auto barrier_error = sql_error(ctx, fmt::format(
        "SELECT id FROM doc0377_source_security_barrier WHERE {};",
        mmsearch_call));
    REQUIRE(barrier_error.sqlstate == "0A000");

    exec_sql(ctx,
        "BEGIN;"
        "CREATE ROLE doc0377_source_rls_role NOLOGIN;"
        "GRANT USAGE ON SCHEMA bioseq TO doc0377_source_rls_role;"
        "GRANT SELECT ON doc0377_source_security "
        "TO doc0377_source_rls_role;"
        "ALTER TABLE doc0377_source_security ENABLE ROW LEVEL SECURITY;"
        "ALTER TABLE doc0377_source_security FORCE ROW LEVEL SECURITY;"
        "CREATE POLICY doc0377_source_odd_rows "
        "ON doc0377_source_security FOR SELECT "
        "TO doc0377_source_rls_role USING ((id % 2)=1);"
        "SET LOCAL ROLE doc0377_source_rls_role;");
    REQUIRE(query_int_column(ctx,
        "SELECT id FROM doc0377_source_security "
        "WHERE bioseq.len(s[0:8])=8 ORDER BY id;")
        == std::vector<long long>{1, 3});
    const auto rls_plan = explain_text<false>(ctx,
        "SELECT bioseq.to_text(s[0:8]) "
        "FROM doc0377_source_security ORDER BY id;");
    REQUIRE_FALSE(plan_contains(rls_plan, "BioseqSliceProject"));
    exec_sql(ctx, "SAVEPOINT doc0377_rls_mmsearch;");
    const auto rls_error = sql_error(ctx, fmt::format(
        "SELECT id FROM doc0377_source_security WHERE {};",
        mmsearch_call));
    REQUIRE(rls_error.sqlstate == "0A000");
    exec_sql(ctx,
        "ROLLBACK TO SAVEPOINT doc0377_rls_mmsearch;"
        "RELEASE SAVEPOINT doc0377_rls_mmsearch;"
        "RESET ROLE; ROLLBACK;");
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}
#endif
