#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

namespace {

struct row_pair {
    std::string rows;
    std::string id_sum;
};

void create_rows(bioseq::libpq::connection& connection,
                 std::string_view table,
                 int row_count,
                 int repeat_count = 1) {
    const std::string identifier = connection.escape_identifier(table);
    bioseq::cache_test::exec(
        connection,
        "CREATE TABLE " + identifier
            + "(id integer PRIMARY KEY, value text NOT NULL); "
              "INSERT INTO "
            + identifier + " SELECT i, repeat(md5(i::text), "
            + std::to_string(repeat_count)
            + ") FROM generate_series(1, "
            + std::to_string(row_count)
            + ") AS g(i); "
              "SELECT bioseq_cache.invalidate_all(); "
              "SELECT bioseq_cache_test.request_janitor_sweep(); "
              "SELECT bioseq_cache.reset_stats()");
}

[[nodiscard]] row_pair pair_from(const bioseq::libpq::result& result) {
    if (result.rows() != 1 || result.columns() != 2
        || result.is_null(0, 0) || result.is_null(0, 1)) {
        throw std::runtime_error(
            "isolation query did not return one non-null row pair");
    }
    return {
        .rows = std::string(result.value(0, 0)),
        .id_sum = std::string(result.value(0, 1)),
    };
}

[[nodiscard]] row_pair query_pair(
    bioseq::postgres_test::session_context& session,
    std::string_view sql) {
    return pair_from(session.exec(sql));
}

void wait_for_flag(
    const std::atomic<bool>& flag,
    bioseq::postgres_test::session_context& session,
    std::string_view diagnostic) {
    if (!bioseq::postgres_test::wait_until(
            [&flag]() {
                return flag.load(std::memory_order_acquire);
            },
            session.deadline(), 5ms)) {
        throw std::runtime_error(std::string(diagnostic));
    }
}

void require_outcomes(
    const std::vector<
        bioseq::postgres_test::session_task_outcome>& outcomes) {
    for (const auto& outcome : outcomes) {
        INFO("session=" << outcome.name << " error=" << outcome.error);
        REQUIRE(outcome.passed);
        REQUIRE_FALSE(outcome.timed_out);
    }
}

void require_pair(
    const row_pair& value,
    std::string_view expected_rows,
    std::string_view expected_sum) {
    CHECK(value.rows == expected_rows);
    CHECK(value.id_sum == expected_sum);
}

}  // namespace

TEST_CASE("cache encoder failure fails open without exposing a partial generation",
          "[cache][integration][isolation][failure]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "cache encoder failure fails open without exposing a partial generation"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& producer = fixture.admin();
    create_rows(producer, "cache_failure_rows", 50, 8);

    bioseq::cache_test::exec(
        producer,
        "SET bioseq.query_cache_optimisation_mode = 'same'; "
        "SELECT bioseq_cache_test.fail_next_write('encoder_error')");
    const row_pair produced = pair_from(producer.exec(
        "SELECT count(*), sum(id) "
        "FROM cache_failure_rows WHERE id <= 25"));
    require_pair(produced, "25", "325");

    const auto failed_state = producer.exec(
        "SELECT count(*) FILTER (WHERE state = 'BUILDING'), "
        "       count(*) FILTER (WHERE state = 'FAILED') "
        "FROM bioseq_cache_test.entry_states()");
    REQUIRE(failed_state.rows() == 1);
    REQUIRE(failed_state.columns() == 2);
    CHECK(failed_state.value(0, 0) == "0");
    CHECK(failed_state.value(0, 1) == "1");

    auto reader = fixture.connect();
    bioseq::cache_test::exec(
        reader, "SET bioseq.query_cache_optimisation_mode = 'same'");
    const row_pair retried = pair_from(reader.exec(
        "SELECT count(*), sum(id) "
        "FROM cache_failure_rows WHERE id <= 25"));
    require_pair(retried, "25", "325");
    CHECK(bioseq::cache_test::integer(
              reader,
              "SELECT count(*) "
              "FROM bioseq_cache_test.entry_states() "
              "WHERE state = 'BUILDING'")
          == 0);

    bioseq::postgres_test::record_migration_probe(
        "isolation.cache_producer_failure.fail_open");
}

TEST_CASE("producer cancellation wakes waiter and permits a fresh claim",
          "[cache][integration][isolation][concurrency][cancel]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "producer cancellation wakes waiter and permits a fresh claim"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& admin = fixture.admin();
    create_rows(admin, "cache_producer_cancel_rows", 100);

    std::atomic<bool> pauses_armed{};
    std::atomic<bool> producer_paused{};
    row_pair waiter_rows;
    std::string producer_sqlstate;
    std::vector<bioseq::postgres_test::session_task> tasks;
    tasks.push_back({
        .name = "controller",
        .body = [&pauses_armed, &producer_paused](
                    bioseq::postgres_test::session_context& session) {
            static_cast<void>(session.exec(
                "SELECT bioseq_cache_test.arm_pause("
                "'after_building_publish'); "
                "SELECT bioseq_cache_test.arm_pause("
                "'after_waiter_registered')"));
            pauses_armed.store(true, std::memory_order_release);
            static_cast<void>(session.exec(
                "SELECT bioseq_cache_test.wait_until_paused("
                "'after_building_publish')"));
            producer_paused.store(true, std::memory_order_release);
            static_cast<void>(session.exec(
                "SELECT bioseq_cache_test.wait_until_paused("
                "'after_waiter_registered')"));
            const auto cancelled = session.exec(
                "SELECT pg_cancel_backend(producer_pid) "
                "FROM bioseq_cache_test.entry_states() "
                "WHERE state = 'BUILDING'");
            if (cancelled.rows() != 1
                || cancelled.columns() != 1
                || cancelled.value(0, 0) != "t") {
                throw std::runtime_error(
                    "elected producer backend was not cancelled");
            }
            static_cast<void>(session.exec(
                "SELECT bioseq_cache_test.release_pause("
                "'after_building_publish'); "
                "SELECT bioseq_cache_test.release_pause("
                "'after_waiter_registered')"));
        }});
    tasks.push_back({
        .name = "producer",
        .body = [&pauses_armed, &producer_sqlstate](
                    bioseq::postgres_test::session_context& session) {
            wait_for_flag(
                pauses_armed, session,
                "producer cancellation pauses were not armed");
            static_cast<void>(session.exec(
                "SET application_name = "
                "'cache_native_cancelled_producer'; "
                "SET bioseq.query_cache_optimisation_mode = 'same'"));
            try {
                static_cast<void>(session.exec(
                    "SELECT count(*), sum(id) "
                    "FROM cache_producer_cancel_rows WHERE id <= 10"));
            } catch (const bioseq::libpq::query_error& error) {
                producer_sqlstate = std::string(error.sqlstate());
                if (error.sqlstate() == "57014") {
                    return;
                }
                throw;
            }
            throw std::runtime_error(
                "cancelled producer query unexpectedly succeeded");
        }});
    tasks.push_back({
        .name = "waiter",
        .body = [&producer_paused, &waiter_rows](
                    bioseq::postgres_test::session_context& session) {
            wait_for_flag(
                producer_paused, session,
                "waiter did not observe the producer pause");
            static_cast<void>(session.exec(
                "SET bioseq.query_cache_optimisation_mode = 'same'"));
            waiter_rows = query_pair(
                session,
                "SELECT count(*), sum(id) "
                "FROM cache_producer_cancel_rows WHERE id <= 10");
        }});

    const auto outcomes =
        bioseq::postgres_test::run_independent_session_tasks(
            fixture.cluster(), tasks, 45s);
    require_outcomes(outcomes);
    CHECK(producer_sqlstate == "57014");
    require_pair(waiter_rows, "10", "55");

    auto retry = fixture.connect();
    bioseq::cache_test::exec(
        retry, "SET bioseq.query_cache_optimisation_mode = 'same'");
    const row_pair retried = pair_from(retry.exec(
        "SELECT count(*), sum(id) "
        "FROM cache_producer_cancel_rows WHERE id <= 10"));
    require_pair(retried, "10", "55");
    bioseq::cache_test::exec(
        admin, "SELECT bioseq_cache_test.request_janitor_sweep()");

    const auto state = admin.exec(
        "SELECT state, waiter_count, reader_count "
        "FROM bioseq_cache_test.entry_states() "
        "WHERE state <> 'RETIRED'");
    REQUIRE(state.rows() == 1);
    CHECK(state.value(0, 0) == "COMPLETE");
    CHECK(state.value(0, 1) == "0");
    CHECK(state.value(0, 2) == "0");
    const auto stats = bioseq::cache_test::read_cache_stats(admin);
    CHECK(stats.produced == 1);
    CHECK(stats.completed_hits == 0);
    CHECK(stats.inflight_waits == 1);
    CHECK(stats.fallbacks == 1);

    bioseq::postgres_test::record_migration_probe(
        "isolation.cache_producer_cancel.retry");
}

TEST_CASE("producer SQL error propagates through waiter retry",
          "[cache][integration][isolation][concurrency][sqlstate]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "producer SQL error propagates through waiter retry"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& admin = fixture.admin();
    create_rows(admin, "cache_sql_error_rows", 50);

    std::atomic<bool> pauses_armed{};
    std::atomic<bool> producer_paused{};
    std::string producer_sqlstate;
    std::string waiter_sqlstate;
    std::vector<bioseq::postgres_test::session_task> tasks;
    tasks.push_back({
        .name = "controller",
        .body = [&pauses_armed, &producer_paused](
                    bioseq::postgres_test::session_context& session) {
            static_cast<void>(session.exec(
                "SELECT bioseq_cache_test.arm_pause("
                "'after_building_publish'); "
                "SELECT bioseq_cache_test.arm_pause("
                "'after_waiter_registered')"));
            pauses_armed.store(true, std::memory_order_release);
            static_cast<void>(session.exec(
                "SELECT bioseq_cache_test.wait_until_paused("
                "'after_building_publish')"));
            producer_paused.store(true, std::memory_order_release);
            static_cast<void>(session.exec(
                "SELECT bioseq_cache_test.wait_until_paused("
                "'after_waiter_registered')"));
            static_cast<void>(session.exec(
                "SELECT bioseq_cache_test.release_pause("
                "'after_building_publish'); "
                "SELECT bioseq_cache_test.release_pause("
                "'after_waiter_registered')"));
        }});
    const auto failing_body =
        [](const std::atomic<bool>& gate,
           std::string_view gate_failure,
           std::string& observed_sqlstate,
           bioseq::postgres_test::session_context& session) {
            wait_for_flag(gate, session, gate_failure);
            static_cast<void>(session.exec(
                "SET bioseq.query_cache_optimisation_mode = 'same'"));
            try {
                static_cast<void>(session.exec(
                    "SELECT sum(100 / (id - 25)) "
                    "FROM cache_sql_error_rows"));
            } catch (const bioseq::libpq::query_error& error) {
                observed_sqlstate = std::string(error.sqlstate());
                if (error.sqlstate() == "22012") {
                    return;
                }
                throw;
            }
            throw std::runtime_error(
                "division-by-zero isolation query unexpectedly succeeded");
        };
    tasks.push_back({
        .name = "producer",
        .body = [&pauses_armed, &producer_sqlstate, &failing_body](
                    bioseq::postgres_test::session_context& session) {
            failing_body(
                pauses_armed,
                "SQL-error producer pause was not armed",
                producer_sqlstate, session);
        }});
    tasks.push_back({
        .name = "waiter",
        .body = [&producer_paused, &waiter_sqlstate, &failing_body](
                    bioseq::postgres_test::session_context& session) {
            failing_body(
                producer_paused,
                "SQL-error waiter did not observe producer pause",
                waiter_sqlstate, session);
        }});

    const auto outcomes =
        bioseq::postgres_test::run_independent_session_tasks(
            fixture.cluster(), tasks, 45s);
    require_outcomes(outcomes);
    CHECK(producer_sqlstate == "22012");
    CHECK(waiter_sqlstate == "22012");

    auto recovery = fixture.connect();
    bioseq::cache_test::exec(
        recovery, "SET bioseq.query_cache_optimisation_mode = 'same'");
    const row_pair recovered = pair_from(recovery.exec(
        "SELECT count(*), sum(id) "
        "FROM cache_sql_error_rows WHERE id <= 10"));
    require_pair(recovered, "10", "55");
    bioseq::cache_test::exec(
        admin, "SELECT bioseq_cache_test.request_janitor_sweep()");
    const auto state = admin.exec(
        "SELECT state, waiter_count, reader_count "
        "FROM bioseq_cache_test.entry_states() "
        "WHERE state <> 'RETIRED'");
    REQUIRE(state.rows() == 1);
    CHECK(state.value(0, 0) == "COMPLETE");
    CHECK(state.value(0, 1) == "0");
    CHECK(state.value(0, 2) == "0");

    bioseq::postgres_test::record_migration_probe(
        "isolation.cache_sql_error.sqlstate");
}

TEST_CASE("timed out waiter drains while producer completes",
          "[cache][integration][isolation][concurrency][timeout]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "timed out waiter drains while producer completes"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& admin = fixture.admin();
    create_rows(admin, "cache_wait_timeout_rows", 100);

    std::atomic<bool> pauses_armed{};
    std::atomic<bool> producer_paused{};
    row_pair producer_rows;
    std::string waiter_sqlstate;
    std::string waiter_error;
    std::vector<bioseq::postgres_test::session_task> tasks;
    tasks.push_back({
        .name = "controller",
        .body = [&pauses_armed, &producer_paused](
                    bioseq::postgres_test::session_context& session) {
            static_cast<void>(session.exec(
                "SELECT bioseq_cache_test.arm_pause("
                "'after_building_publish'); "
                "SELECT bioseq_cache_test.arm_pause("
                "'after_waiter_registered')"));
            pauses_armed.store(true, std::memory_order_release);
            static_cast<void>(session.exec(
                "SELECT bioseq_cache_test.wait_until_paused("
                "'after_building_publish')"));
            producer_paused.store(true, std::memory_order_release);
            static_cast<void>(session.exec(
                "SELECT bioseq_cache_test.wait_until_paused("
                "'after_waiter_registered')"));
            static_cast<void>(session.exec("SELECT pg_sleep(1.2)"));
            const bool waiter_drained =
                bioseq::postgres_test::wait_until(
                    [&session]() {
                        const auto state = session.exec(
                            "SELECT count(*) "
                            "FROM bioseq_cache_test.entry_states() "
                            "WHERE state = 'BUILDING' "
                            "AND waiter_count = 0");
                        return state.rows() == 1
                            && state.value(0, 0) == "1";
                    },
                    session.deadline(), 10ms);
            if (!waiter_drained) {
                throw std::runtime_error(
                    "statement-timeout waiter reference did not drain");
            }
            static_cast<void>(session.exec(
                "SELECT bioseq_cache_test.release_pause("
                "'after_waiter_registered'); "
                "SELECT bioseq_cache_test.release_pause("
                "'after_building_publish')"));
        }});
    tasks.push_back({
        .name = "producer",
        .body = [&pauses_armed, &producer_rows](
                    bioseq::postgres_test::session_context& session) {
            wait_for_flag(
                pauses_armed, session,
                "timeout producer pauses were not armed");
            static_cast<void>(session.exec(
                "SET bioseq.query_cache_optimisation_mode = 'same'"));
            producer_rows = query_pair(
                session,
                "SELECT count(*), sum(id) "
                "FROM cache_wait_timeout_rows WHERE id <= 10");
        }});
    tasks.push_back({
        .name = "waiter",
        .body = [&producer_paused, &waiter_sqlstate, &waiter_error](
                    bioseq::postgres_test::session_context& session) {
            wait_for_flag(
                producer_paused, session,
                "timeout waiter did not observe producer pause");
            static_cast<void>(session.exec(
                "SET statement_timeout = '1 s'; "
                "SET bioseq.query_cache_optimisation_mode = 'same'"));
            try {
                static_cast<void>(session.exec(
                    "SELECT count(*), sum(id) "
                    "FROM cache_wait_timeout_rows WHERE id <= 10"));
            } catch (const bioseq::libpq::query_error& error) {
                waiter_sqlstate = std::string(error.sqlstate());
                waiter_error = error.what();
                if (error.sqlstate() == "57014") {
                    return;
                }
                throw;
            }
            throw std::runtime_error(
                "statement-timeout waiter unexpectedly succeeded");
        }});

    const auto outcomes =
        bioseq::postgres_test::run_independent_session_tasks(
            fixture.cluster(), tasks, 45s);
    require_outcomes(outcomes);
    require_pair(producer_rows, "10", "55");
    CHECK(waiter_sqlstate == "57014");
    CHECK(waiter_error.find("statement timeout") != std::string::npos);

    auto reader = fixture.connect();
    bioseq::cache_test::exec(
        reader, "SET bioseq.query_cache_optimisation_mode = 'same'");
    const row_pair replayed = pair_from(reader.exec(
        "SELECT count(*), sum(id) "
        "FROM cache_wait_timeout_rows WHERE id <= 10"));
    require_pair(replayed, "10", "55");
    const auto stats = bioseq::cache_test::read_cache_stats(admin);
    CHECK(stats.produced == 1);
    CHECK(stats.completed_hits == 1);
    CHECK(stats.inflight_waits == 1);
    CHECK(bioseq::cache_test::integer(
              admin,
              "SELECT count(*) "
              "FROM bioseq_cache_test.entry_states() "
              "WHERE waiter_count <> 0 OR reader_count <> 0")
          == 0);

    bioseq::postgres_test::record_migration_probe(
        "isolation.cache_wait_timeout.reference_drained");
}
