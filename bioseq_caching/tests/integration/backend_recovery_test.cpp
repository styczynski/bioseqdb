#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

namespace {

[[nodiscard]] bool
all_passed(const std::vector<bioseq::postgres_test::independent_session_result>& outcomes) {
    for (const auto& outcome : outcomes) {
        if (!outcome.passed || outcome.timed_out) {
            return false;
        }
    }
    return true;
}

void wait_for_flag(std::atomic<bool>& flag,
                   bioseq::postgres_test::bounded_session_context& session,
                   std::string_view message) {
    if (!bioseq::postgres_test::wait_until(
            [&flag]() { return flag.load(std::memory_order_acquire); }, session.deadline(), 5ms)) {
        throw std::runtime_error(std::string(message));
    }
}

[[nodiscard]] std::vector<std::string> ordered_ids(int count) {
    std::vector<std::string> values;
    values.reserve(static_cast<std::size_t>(count));
    for (int value = 1; value <= count; ++value) {
        values.push_back(std::to_string(value));
    }
    return values;
}

[[nodiscard]] std::vector<std::string> first_column(const bioseq::libpq::command_result& result) {
    if (result.column_count() != 1) {
        throw std::runtime_error("expected one result column");
    }
    std::vector<std::string> values;
    values.reserve(static_cast<std::size_t>(result.row_count()));
    for (int row = 0; row < result.row_count(); ++row) {
        if (result.cell_is_null(row, 0)) {
            throw std::runtime_error("ordered identifier result contains NULL");
        }
        values.emplace_back(result.text_value(row, 0));
    }
    return values;
}

template <typename p_session>
[[nodiscard]] bool session_boolean(p_session& session, std::string_view sql) {
    const auto result = session.execute(sql);
    if (result.row_count() != 1 || result.column_count() != 1 || result.cell_is_null(0, 0)) {
        throw std::runtime_error("expected one non-null Boolean result");
    }
    if (result.text_value(0, 0) == "t") {
        return true;
    }
    if (result.text_value(0, 0) == "f") {
        return false;
    }
    throw std::runtime_error("expected PostgreSQL Boolean text");
}

template <typename p_session>
[[nodiscard]] std::string session_scalar(p_session& session, std::string_view sql) {
    const auto result = session.execute(sql);
    if (result.row_count() != 1 || result.column_count() != 1 || result.cell_is_null(0, 0)) {
        throw std::runtime_error("expected one non-null scalar result");
    }
    return std::string(result.text_value(0, 0));
}

struct storage_snapshot {
    std::int64_t hot_bytes{};
    std::int64_t file_bytes{};
    std::int64_t manifests{};
    std::int64_t waiters{};
    std::int64_t readers{};
    std::int64_t pending_hot_bytes{};
    std::int64_t pending_file_bytes{};
    std::int64_t registry_waiters{};
    std::int64_t registry_readers{};
    std::int64_t planner_pins{};

    friend bool operator==(const storage_snapshot&, const storage_snapshot&) = default;
};

[[nodiscard]] std::int64_t diagnostic_metric(std::string_view diagnostic, std::string_view label) {
    const std::size_t label_offset = diagnostic.find(label);
    if (label_offset == std::string_view::npos) {
        throw std::runtime_error("Cache Storage diagnostic is missing " + std::string(label));
    }
    const std::size_t begin = label_offset + label.size();
    std::size_t end = begin;
    while (end < diagnostic.size() && diagnostic[end] >= '0' && diagnostic[end] <= '9') {
        ++end;
    }
    std::int64_t value{};
    const auto [parsed_end, error] =
        std::from_chars(diagnostic.data() + begin, diagnostic.data() + end, value);
    if (error != std::errc{} || parsed_end != diagnostic.data() + end || begin == end) {
        throw std::runtime_error("Cache Storage diagnostic has invalid " + std::string(label));
    }
    return value;
}

[[nodiscard]] bool consume_literal(std::string_view& input, std::string_view literal) {
    if (!input.starts_with(literal)) {
        return false;
    }
    input.remove_prefix(literal.size());
    return true;
}

[[nodiscard]] bool consume_unsigned(std::string_view& input) {
    std::size_t size = 0;
    while (size < input.size() && input[size] >= '0' && input[size] <= '9') {
        ++size;
    }
    if (size == 0) {
        return false;
    }
    input.remove_prefix(size);
    return true;
}

/// Validate the established Cache Storage value consumed by native reports.
[[nodiscard]] bool storage_diagnostic_shape(std::string_view diagnostic) {
    if (!consume_literal(diagnostic, "ready") && !consume_literal(diagnostic, "unavailable")) {
        return false;
    }
    if (!consume_literal(diagnostic, "; backend=")) {
        return false;
    }
    if (!consume_literal(diagnostic, "attached") && !consume_literal(diagnostic, "not-attached")) {
        return false;
    }
    return consume_literal(diagnostic, "; hot=") && consume_unsigned(diagnostic) &&
           consume_literal(diagnostic, "/") && consume_unsigned(diagnostic) &&
           consume_literal(diagnostic, "; file=") && consume_unsigned(diagnostic) &&
           consume_literal(diagnostic, "/") && consume_unsigned(diagnostic) &&
           consume_literal(diagnostic, "; manifests=") && consume_unsigned(diagnostic) &&
           consume_literal(diagnostic, "; waiters=") && consume_unsigned(diagnostic) &&
           diagnostic.empty();
}

template <typename p_session>
[[nodiscard]] storage_snapshot read_storage_snapshot(p_session& session) {
    const auto explain = session.execute("SET bioseq_cache.query_cache.mode = 'exact_query'; "
                                         "EXPLAIN (COSTS OFF) "
                                         "SELECT id FROM entitlement_rows "
                                         "WHERE id <= 100 ORDER BY id");
    std::string storage_diagnostic;
    std::string ownership_diagnostic;
    for (int row = 0; row < explain.row_count(); ++row) {
        if (explain.cell_is_null(row, 0)) {
            continue;
        }
        const std::string_view line = explain.text_value(row, 0);
        constexpr std::string_view storage_prefix{"Cache Storage: "};
        constexpr std::string_view ownership_prefix{"Cache Storage Ownership: "};
        const std::size_t storage_offset = line.find(storage_prefix);
        const std::size_t ownership_offset = line.find(ownership_prefix);
        if (storage_offset != std::string_view::npos) {
            storage_diagnostic = std::string(line.substr(storage_offset + storage_prefix.size()));
        }
        if (ownership_offset != std::string_view::npos) {
            ownership_diagnostic =
                std::string(line.substr(ownership_offset + ownership_prefix.size()));
        }
    }
    if (storage_diagnostic.empty() || ownership_diagnostic.empty()) {
        throw std::runtime_error("EXPLAIN did not expose complete Cache Storage diagnostics");
    }
    if (!storage_diagnostic_shape(storage_diagnostic)) {
        throw std::runtime_error("Cache Storage diagnostic value changed shape");
    }
    return storage_snapshot{
        .hot_bytes = diagnostic_metric(storage_diagnostic, "hot="),
        .file_bytes = diagnostic_metric(storage_diagnostic, "file="),
        .manifests = diagnostic_metric(storage_diagnostic, "manifests="),
        .waiters = diagnostic_metric(storage_diagnostic, "waiters="),
        .readers = diagnostic_metric(ownership_diagnostic, "readers="),
        .pending_hot_bytes = diagnostic_metric(ownership_diagnostic, "pending_hot="),
        .pending_file_bytes = diagnostic_metric(ownership_diagnostic, "pending_file="),
        .registry_waiters = diagnostic_metric(ownership_diagnostic, "registry_waiters="),
        .registry_readers = diagnostic_metric(ownership_diagnostic, "registry_readers="),
        .planner_pins = diagnostic_metric(ownership_diagnostic, "planner_pins=")};
}

[[nodiscard]] bool snapshot_reclaimed(bioseq::libpq::client_connection& connection,
                                      const storage_snapshot& baseline) {
    bioseq::cache_test::execute(connection, "SELECT bioseq_cache_test.request_janitor_sweep()");
    return read_storage_snapshot(connection) == baseline;
}

} // namespace

TEST_CASE("terminated cache producer wakes its waiter without BUILDING leak",
          "[component:cache][layer:integration]"
          "[feature:recovery][resource:postgres]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        bioseq::cache_test::cluster_options(false, bioseq::cache_test::preload_order::cache_only)};
    auto& admin = fixture.admin();
    bioseq::cache_test::execute(admin,
                                "CREATE TABLE crash_rows("
                                "id integer PRIMARY KEY, value text); "
                                "INSERT INTO crash_rows "
                                "SELECT i, md5(i::text) "
                                "FROM generate_series(1, 100) AS g(i); "
                                "SELECT bioseq_cache.reset_stats()");

    std::atomic<bool> pause_armed{};
    std::atomic<bool> producer_paused{};
    std::string waiter_count;
    std::string waiter_sum;
    std::vector<bioseq::postgres_test::independent_session_task> tasks;
    tasks.push_back(bioseq::postgres_test::independent_session_task{
        .name = "backend-crash-producer",
        .body = [&pause_armed](bioseq::postgres_test::bounded_session_context& session) {
            wait_for_flag(pause_armed, session, "backend-crash producer pause was not armed");
            static_cast<void>(session.execute("SET application_name = 'cache_crash_producer'; "
                                              "SET bioseq_cache.query_cache.mode = 'exact_query'; "
                                              "SELECT count(*), sum(id) "
                                              "FROM crash_rows WHERE id <= 10"));
        }});
    tasks.push_back(bioseq::postgres_test::independent_session_task{
        .name = "backend-crash-waiter",
        .body = [&producer_paused, &waiter_count, &waiter_sum](
                    bioseq::postgres_test::bounded_session_context& session) {
            if (!bioseq::postgres_test::wait_until(
                    [&producer_paused]() {
                        return producer_paused.load(std::memory_order_acquire);
                    },
                    session.deadline(),
                    5ms)) {
                throw std::runtime_error("producer did not reach its pause");
            }
            const auto result =
                session.execute("SET application_name = 'cache_crash_waiter'; "
                                "SET bioseq_cache.query_cache.mode = 'exact_query'; "
                                "SELECT count(*), sum(id) "
                                "FROM crash_rows WHERE id <= 10");
            if (result.row_count() != 1 || result.column_count() != 2) {
                throw std::runtime_error("waiter result has the wrong shape");
            }
            waiter_count = std::string(result.text_value(0, 0));
            waiter_sum = std::string(result.text_value(0, 1));
        }});
    tasks.push_back(bioseq::postgres_test::independent_session_task{
        .name = "backend-crash-controller",
        .body = [&pause_armed,
                 &producer_paused](bioseq::postgres_test::bounded_session_context& session) {
            static_cast<void>(session.execute("SELECT bioseq_cache_test.arm_pause("
                                              "'after_building_publish')"));
            pause_armed.store(true, std::memory_order_release);
            static_cast<void>(session.execute("SELECT bioseq_cache_test.wait_until_paused("
                                              "'after_building_publish')"));
            producer_paused.store(true, std::memory_order_release);
            if (!bioseq::postgres_test::wait_until(
                    [&session]() {
                        return session_boolean(session,
                                               "SELECT EXISTS ("
                                               "SELECT 1 "
                                               "FROM bioseq_cache_test.entry_states() "
                                               "WHERE state = 'BUILDING' "
                                               "AND waiter_count > 0)");
                    },
                    session.deadline(),
                    10ms)) {
                throw std::runtime_error("waiter did not register against the producer");
            }
            if (!session_boolean(session,
                                 "SELECT pg_terminate_backend(("
                                 "SELECT producer_pid "
                                 "FROM bioseq_cache_test.entry_states() "
                                 "WHERE state = 'BUILDING' "
                                 "ORDER BY generation LIMIT 1))")) {
                throw std::runtime_error("producer backend termination was rejected");
            }
            static_cast<void>(session.execute("SELECT bioseq_cache_test.release_pause("
                                              "'after_building_publish')"));
        }});

    const auto outcomes =
        bioseq::postgres_test::run_independent_sessions(fixture.cluster(), tasks, 45s);
    REQUIRE(outcomes.size() == 3);
    CHECK_FALSE(outcomes[0].passed);
    CHECK_FALSE(outcomes[0].timed_out);
    CHECK(outcomes[1].passed);
    CHECK_FALSE(outcomes[1].timed_out);
    CHECK(outcomes[2].passed);
    CHECK(waiter_count == "10");
    CHECK(waiter_sum == "55");
    CHECK(bioseq::cache_test::integer(admin,
                                      "SELECT count(*) "
                                      "FROM bioseq_cache_test.entry_states() "
                                      "WHERE state = 'BUILDING'") == 0);
    CHECK(bioseq::cache_test::integer(admin, "SELECT count(*) FROM crash_rows WHERE id <= 10") ==
          10);
}

TEST_CASE("file write failure fails open and terminalizes the exact claim",
          "[component:cache][layer:integration]"
          "[feature:recovery][resource:postgres]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        bioseq::cache_test::cluster_options(false, bioseq::cache_test::preload_order::cache_only)};
    auto& admin = fixture.admin();
    bioseq::cache_test::execute(admin,
                                "CREATE TABLE file_rows("
                                "id integer PRIMARY KEY, payload text); "
                                "INSERT INTO file_rows "
                                "SELECT i, repeat(md5(i::text), 1024) "
                                "FROM generate_series(1, 128) AS g(i); "
                                "SELECT bioseq_cache.reset_stats()");

    std::atomic<bool> pause_armed{};
    std::atomic<bool> producer_paused{};
    std::vector<std::string> producer_rows;
    std::vector<std::string> waiter_rows;
    std::string failed_key;
    std::string failed_generation;
    std::string failed_claim_sequence;
    std::vector<bioseq::postgres_test::independent_session_task> tasks;
    tasks.push_back(bioseq::postgres_test::independent_session_task{
        .name = "file-failure-producer",
        .body = [&pause_armed,
                 &producer_rows](bioseq::postgres_test::bounded_session_context& session) {
            wait_for_flag(pause_armed, session, "file-failure producer pause was not armed");
            const auto result =
                session.execute("SET application_name = 'cache_file_failure_producer'; "
                                "SET bioseq_cache.query_cache.mode = 'exact_query'; "
                                "SELECT id FROM file_rows "
                                "WHERE id <= 10 ORDER BY id");
            producer_rows = first_column(result);
        }});
    tasks.push_back(bioseq::postgres_test::independent_session_task{
        .name = "file-failure-waiter",
        .body = [&producer_paused,
                 &waiter_rows](bioseq::postgres_test::bounded_session_context& session) {
            if (!bioseq::postgres_test::wait_until(
                    [&producer_paused]() {
                        return producer_paused.load(std::memory_order_acquire);
                    },
                    session.deadline(),
                    5ms)) {
                throw std::runtime_error("producer did not reach its pause");
            }
            const auto result =
                session.execute("SET application_name = 'cache_file_failure_waiter'; "
                                "SET bioseq_cache.query_cache.mode = 'exact_query'; "
                                "SELECT id FROM file_rows "
                                "WHERE id <= 10 ORDER BY id");
            waiter_rows = first_column(result);
        }});
    tasks.push_back(bioseq::postgres_test::independent_session_task{
        .name = "file-failure-controller",
        .body = [&pause_armed,
                 &producer_paused,
                 &failed_key,
                 &failed_generation,
                 &failed_claim_sequence](bioseq::postgres_test::bounded_session_context& session) {
            static_cast<void>(session.execute("SELECT bioseq_cache_test.arm_pause("
                                              "'after_building_publish')"));
            pause_armed.store(true, std::memory_order_release);
            static_cast<void>(session.execute("SELECT bioseq_cache_test.wait_until_paused("
                                              "'after_building_publish')"));
            const auto token = session.execute("SELECT entry_key, generation, claim_sequence "
                                               "FROM bioseq_cache_test.entry_states() "
                                               "WHERE state = 'BUILDING' "
                                               "ORDER BY generation LIMIT 1");
            if (token.row_count() != 1 || token.column_count() != 3) {
                throw std::runtime_error("BUILDING producer token is not visible");
            }
            failed_key = std::string(token.text_value(0, 0));
            failed_generation = std::string(token.text_value(0, 1));
            failed_claim_sequence = std::string(token.text_value(0, 2));
            producer_paused.store(true, std::memory_order_release);
            if (!bioseq::postgres_test::wait_until(
                    [&session]() {
                        return session_boolean(session,
                                               "SELECT EXISTS ("
                                               "SELECT 1 "
                                               "FROM bioseq_cache_test.entry_states() "
                                               "WHERE state = 'BUILDING' "
                                               "AND waiter_count = 1)");
                    },
                    session.deadline(),
                    10ms)) {
                throw std::runtime_error("waiter did not attach before write failure");
            }
            static_cast<void>(session.execute("SELECT bioseq_cache_test.fail_next_write('enospc'); "
                                              "SELECT bioseq_cache_test.release_pause("
                                              "'after_building_publish')"));
        }});

    const auto outcomes =
        bioseq::postgres_test::run_independent_sessions(fixture.cluster(), tasks, 45s);
    REQUIRE(all_passed(outcomes));
    CHECK(producer_rows == ordered_ids(10));
    CHECK(waiter_rows == ordered_ids(10));
    CHECK(bioseq::cache_test::integer(admin,
                                      "SELECT count(*) "
                                      "FROM bioseq_cache_test.entry_states() "
                                      "WHERE state = 'BUILDING'") == 0);
    REQUIRE_FALSE(failed_key.empty());
    const std::string failed_state_sql = "SELECT state "
                                         "FROM bioseq_cache_test.entry_states() "
                                         "WHERE entry_key = " +
                                         admin.escape_literal(failed_key) +
                                         " AND generation = " + failed_generation +
                                         " AND claim_sequence = " + failed_claim_sequence;
    CHECK(bioseq::cache_test::scalar(admin, failed_state_sql) == "FAILED");
    const auto stats = bioseq::cache_test::read_cache_stats(admin);
    CHECK(stats.inflight_waits >= 1);
    CHECK(stats.fallbacks >= 1);
}

TEST_CASE("waiter entitlement pins TTL-zero storage and drains on every exit",
          "[component:cache][layer:integration]"
          "[feature:recovery][resource:postgres]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        bioseq::cache_test::cluster_options(false, bioseq::cache_test::preload_order::cache_only)};
    auto& admin = fixture.admin();
    bioseq::cache_test::execute(admin,
                                "CREATE TABLE entitlement_rows("
                                "id integer PRIMARY KEY, value text); "
                                "INSERT INTO entitlement_rows "
                                "SELECT i, md5(i::text) "
                                "FROM generate_series(1, 100) AS g(i); "
                                "SELECT bioseq_cache.reset_stats()");
    const storage_snapshot baseline = read_storage_snapshot(admin);

    std::atomic<bool> pauses_armed{};
    std::atomic<bool> producer_at_terminal{};
    std::vector<std::string> cancelled_cycle_producer_rows;
    storage_snapshot entitled{};
    storage_snapshot aggressively_swept{};
    storage_snapshot reclaimed{};
    bool published_waiter_owned{};
    bool terminated_waiter_drained{};
    std::vector<bioseq::postgres_test::independent_session_task> cancelled_tasks;
    cancelled_tasks.push_back(bioseq::postgres_test::independent_session_task{
        .name = "entitlement-cancel-producer",
        .body = [&pauses_armed, &cancelled_cycle_producer_rows](
                    bioseq::postgres_test::bounded_session_context& session) {
            wait_for_flag(pauses_armed, session, "cancelled-cycle pauses were not armed");
            const auto result =
                session.execute("SET application_name = 'cache_entitlement_producer'; "
                                "SET bioseq_cache.query_cache.mode = 'exact_query'; "
                                "SET bioseq_cache.query_cache.completed_result_ttl_ms = 0; "
                                "SELECT id FROM entitlement_rows "
                                "WHERE id <= 100 ORDER BY id");
            cancelled_cycle_producer_rows = first_column(result);
        }});
    cancelled_tasks.push_back(bioseq::postgres_test::independent_session_task{
        .name = "entitlement-cancel-waiter",
        .body = [&producer_at_terminal](bioseq::postgres_test::bounded_session_context& session) {
            if (!bioseq::postgres_test::wait_until(
                    [&producer_at_terminal]() {
                        return producer_at_terminal.load(std::memory_order_acquire);
                    },
                    session.deadline(),
                    5ms)) {
                throw std::runtime_error("producer did not reach terminal publication");
            }
            static_cast<void>(
                session.execute("SET application_name = 'cache_entitlement_waiter'; "
                                "SET bioseq_cache.query_cache.mode = 'exact_query'; "
                                "SET bioseq_cache.query_cache.completed_result_ttl_ms = 0; "
                                "SELECT id FROM entitlement_rows "
                                "WHERE id <= 100 ORDER BY id"));
        }});
    cancelled_tasks.push_back(bioseq::postgres_test::independent_session_task{
        .name = "entitlement-cancel-controller",
        .body = [&pauses_armed,
                 &producer_at_terminal,
                 &baseline,
                 &entitled,
                 &aggressively_swept,
                 &reclaimed,
                 &published_waiter_owned,
                 &terminated_waiter_drained](
                    bioseq::postgres_test::bounded_session_context& session) {
            static_cast<void>(session.execute("SELECT bioseq_cache_test.arm_pause("
                                              "'before_terminal_publish'); "
                                              "SELECT bioseq_cache_test.arm_pause("
                                              "'after_waiter_registered')"));
            pauses_armed.store(true, std::memory_order_release);
            static_cast<void>(session.execute("SELECT bioseq_cache_test.wait_until_paused("
                                              "'before_terminal_publish')"));
            producer_at_terminal.store(true, std::memory_order_release);
            static_cast<void>(session.execute("SELECT bioseq_cache_test.wait_until_paused("
                                              "'after_waiter_registered'); "
                                              "SELECT bioseq_cache_test.release_pause("
                                              "'before_terminal_publish')"));
            if (!bioseq::postgres_test::wait_until(
                    [&session]() {
                        return session_boolean(session,
                                               "SELECT count(*) = 1 "
                                               "FROM bioseq_cache_test.entry_states() "
                                               "WHERE state = 'COMPLETE' "
                                               "AND waiter_count = 1 "
                                               "AND reader_count = 0");
                    },
                    session.deadline(),
                    10ms)) {
                throw std::runtime_error("published waiter entitlement is not visible");
            }
            published_waiter_owned = true;
            entitled = read_storage_snapshot(session);
            static_cast<void>(session.execute("SELECT bioseq_cache_test.request_janitor_sweep()"));
            std::this_thread::sleep_for(1200ms);
            aggressively_swept = read_storage_snapshot(session);

            const std::string waiter_pid =
                session_scalar(session,
                               "SELECT pid::text FROM pg_stat_activity "
                               "WHERE application_name = 'cache_entitlement_waiter'");
            if (!session_boolean(session, "SELECT pg_terminate_backend(" + waiter_pid + ")")) {
                throw std::runtime_error("waiter backend termination was rejected");
            }
            terminated_waiter_drained = bioseq::postgres_test::wait_until(
                [&session, &waiter_pid]() {
                    return session_boolean(session,
                                           "WITH sweep AS MATERIALIZED ("
                                           "SELECT "
                                           "bioseq_cache_test.request_janitor_sweep()) "
                                           "SELECT NOT EXISTS ("
                                           "SELECT 1 FROM pg_stat_activity "
                                           "WHERE pid = " +
                                               waiter_pid +
                                               ") "
                                               "AND NOT EXISTS ("
                                               "SELECT 1 "
                                               "FROM bioseq_cache_test.entry_states() "
                                               "WHERE waiter_count <> 0) "
                                               "FROM sweep");
                },
                session.deadline(),
                20ms);
            static_cast<void>(session.execute("SELECT bioseq_cache_test.release_pause("
                                              "'after_waiter_registered')"));
            if (!bioseq::postgres_test::wait_until(
                    [&session, &baseline, &reclaimed]() {
                        static_cast<void>(
                            session.execute("SELECT "
                                            "bioseq_cache_test.request_janitor_sweep()"));
                        reclaimed = read_storage_snapshot(session);
                        return reclaimed == baseline;
                    },
                    session.deadline(),
                    20ms)) {
                throw std::runtime_error("cancelled waiter storage was not reclaimed");
            }
        }});

    const auto cancelled_outcomes =
        bioseq::postgres_test::run_independent_sessions(fixture.cluster(), cancelled_tasks, 60s);
    REQUIRE(cancelled_outcomes.size() == 3);
    CHECK(cancelled_outcomes[0].passed);
    CHECK_FALSE(cancelled_outcomes[0].timed_out);
    CHECK_FALSE(cancelled_outcomes[1].passed);
    CHECK_FALSE(cancelled_outcomes[1].timed_out);
    CHECK(cancelled_outcomes[2].passed);
    CHECK(cancelled_cycle_producer_rows == ordered_ids(100));
    CHECK(published_waiter_owned);
    CHECK(entitled.manifests == baseline.manifests + 1);
    CHECK(entitled.waiters == baseline.waiters + 1);
    CHECK(entitled.hot_bytes + entitled.file_bytes > baseline.hot_bytes + baseline.file_bytes);
    CHECK(aggressively_swept.manifests == entitled.manifests);
    CHECK(aggressively_swept.waiters == entitled.waiters);
    CHECK(terminated_waiter_drained);
    CHECK(reclaimed == baseline);
    CHECK(bioseq::cache_test::integer(admin,
                                      "SELECT count(*) "
                                      "FROM bioseq_cache_test.entry_states() "
                                      "WHERE waiter_count <> 0 OR reader_count <> 0") == 0);

    bioseq::cache_test::execute(admin, "SELECT bioseq_cache.reset_stats()");
    std::atomic<bool> success_pauses_armed{};
    std::atomic<bool> success_producer_at_terminal{};
    std::vector<std::string> success_producer_rows;
    std::vector<std::string> success_waiter_rows;
    bool successful_entitlement_owned{};
    bool successful_sweep_preserved{};
    std::vector<bioseq::postgres_test::independent_session_task> success_tasks;
    success_tasks.push_back(bioseq::postgres_test::independent_session_task{
        .name = "entitlement-success-producer",
        .body = [&success_pauses_armed,
                 &success_producer_rows](bioseq::postgres_test::bounded_session_context& session) {
            wait_for_flag(success_pauses_armed, session, "successful-cycle pauses were not armed");
            const auto result =
                session.execute("SET application_name = "
                                "'cache_entitlement_success_producer'; "
                                "SET bioseq_cache.query_cache.mode = 'exact_query'; "
                                "SET bioseq_cache.query_cache.completed_result_ttl_ms = 0; "
                                "SELECT id FROM entitlement_rows "
                                "WHERE id <= 25 ORDER BY id");
            success_producer_rows = first_column(result);
        }});
    success_tasks.push_back(bioseq::postgres_test::independent_session_task{
        .name = "entitlement-success-waiter",
        .body = [&success_producer_at_terminal,
                 &success_waiter_rows](bioseq::postgres_test::bounded_session_context& session) {
            if (!bioseq::postgres_test::wait_until(
                    [&success_producer_at_terminal]() {
                        return success_producer_at_terminal.load(std::memory_order_acquire);
                    },
                    session.deadline(),
                    5ms)) {
                throw std::runtime_error("success producer did not reach terminal publication");
            }
            const auto result =
                session.execute("SET application_name = "
                                "'cache_entitlement_success_waiter'; "
                                "SET bioseq_cache.query_cache.mode = 'exact_query'; "
                                "SET bioseq_cache.query_cache.completed_result_ttl_ms = 0; "
                                "SELECT id FROM entitlement_rows "
                                "WHERE id <= 25 ORDER BY id");
            success_waiter_rows = first_column(result);
        }});
    success_tasks.push_back(bioseq::postgres_test::independent_session_task{
        .name = "entitlement-success-controller",
        .body = [&success_pauses_armed,
                 &success_producer_at_terminal,
                 &successful_entitlement_owned,
                 &successful_sweep_preserved](
                    bioseq::postgres_test::bounded_session_context& session) {
            static_cast<void>(session.execute("SELECT bioseq_cache_test.arm_pause("
                                              "'before_terminal_publish'); "
                                              "SELECT bioseq_cache_test.arm_pause("
                                              "'after_waiter_registered')"));
            success_pauses_armed.store(true, std::memory_order_release);
            static_cast<void>(session.execute("SELECT bioseq_cache_test.wait_until_paused("
                                              "'before_terminal_publish')"));
            success_producer_at_terminal.store(true, std::memory_order_release);
            static_cast<void>(session.execute("SELECT bioseq_cache_test.wait_until_paused("
                                              "'after_waiter_registered'); "
                                              "SELECT bioseq_cache_test.release_pause("
                                              "'before_terminal_publish')"));
            successful_entitlement_owned = bioseq::postgres_test::wait_until(
                [&session]() {
                    return session_boolean(session,
                                           "SELECT count(*) = 1 "
                                           "FROM bioseq_cache_test.entry_states() "
                                           "WHERE state = 'COMPLETE' "
                                           "AND waiter_count = 1 "
                                           "AND reader_count = 0");
                },
                session.deadline(),
                10ms);
            static_cast<void>(session.execute("SELECT bioseq_cache_test.request_janitor_sweep()"));
            successful_sweep_preserved =
                session_boolean(session,
                                "SELECT count(*) = 1 "
                                "FROM bioseq_cache_test.entry_states() "
                                "WHERE state = 'COMPLETE' AND waiter_count = 1");
            static_cast<void>(session.execute("SELECT bioseq_cache_test.release_pause("
                                              "'after_waiter_registered')"));
        }});

    const auto success_outcomes =
        bioseq::postgres_test::run_independent_sessions(fixture.cluster(), success_tasks, 45s);
    REQUIRE(all_passed(success_outcomes));
    CHECK(successful_entitlement_owned);
    CHECK(successful_sweep_preserved);
    CHECK(success_producer_rows == ordered_ids(25));
    CHECK(success_waiter_rows == ordered_ids(25));
    const auto entitlement_stats = bioseq::cache_test::read_cache_stats(admin);
    CHECK(entitlement_stats.produced == 1);
    CHECK(entitlement_stats.completed_hits == 1);
    CHECK(entitlement_stats.inflight_waits == 1);

    const auto post_complete =
        admin.execute("SET bioseq_cache.query_cache.mode = 'exact_query'; "
                      "SET bioseq_cache.query_cache.completed_result_ttl_ms = 0; "
                      "SELECT id FROM entitlement_rows "
                      "WHERE id <= 25 ORDER BY id");
    CHECK(first_column(post_complete) == ordered_ids(25));
    const auto post_complete_stats = bioseq::cache_test::read_cache_stats(admin);
    CHECK(post_complete_stats.produced == 2);
    CHECK(post_complete_stats.completed_hits == 1);
    REQUIRE(bioseq::postgres_test::wait_until(
        [&admin, &baseline]() { return snapshot_reclaimed(admin, baseline); },
        std::chrono::steady_clock::now() + 10s,
        20ms));
    CHECK(bioseq::cache_test::integer(admin,
                                      "SELECT count(*) "
                                      "FROM bioseq_cache_test.entry_states() "
                                      "WHERE waiter_count <> 0 OR reader_count <> 0") == 0);
    const auto source = admin.execute("SELECT count(*), sum(id) FROM entitlement_rows");
    REQUIRE(source.row_count() == 1);
    CHECK(source.text_value(0, 0) == "100");
    CHECK(source.text_value(0, 1) == "5050");
}
