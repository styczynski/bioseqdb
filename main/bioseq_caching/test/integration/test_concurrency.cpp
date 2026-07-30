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

inline constexpr std::string_view cancelled_waiter_query{
    "SELECT sum(id) FROM cache_cancel_wait_rows WHERE id <= 10"};

void create_wait_rows(bioseq::libpq::connection& connection,
                      std::string_view table) {
    const std::string identifier = connection.escape_identifier(table);
    bioseq::cache_test::exec(
        connection,
        "CREATE TABLE " + identifier
            + "(id integer PRIMARY KEY, value text NOT NULL); "
              "INSERT INTO "
            + identifier
            + " SELECT i, md5(i::text) "
              "FROM generate_series(1, 100) AS g(i); "
              "SELECT bioseq_cache.invalidate_all(); "
              "SELECT bioseq_cache_test.request_janitor_sweep(); "
              "SELECT bioseq_cache.reset_stats()");
}

[[nodiscard]] bool all_passed(
    const std::vector<bioseq::postgres_test::session_task_outcome>& outcomes) {
    for (const auto& outcome : outcomes) {
        if (!outcome.passed || outcome.timed_out) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("one exact producer serves one registered waiter",
          "[cache][integration][concurrency][waiter]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "one exact producer serves one registered waiter"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& admin = fixture.admin();
    create_wait_rows(admin, "cache_same_wait_rows");

    std::atomic<bool> pauses_armed{};
    std::atomic<bool> producer_paused{};
    std::string producer_count;
    std::string producer_sum;
    std::string waiter_count;
    std::string waiter_sum;
    std::vector<bioseq::postgres_test::session_task> tasks;
    tasks.push_back(bioseq::postgres_test::session_task{
        .name = "producer",
        .body = [&pauses_armed, &producer_count, &producer_sum](
                    bioseq::postgres_test::session_context& session) {
            if (!bioseq::postgres_test::wait_until(
                    [&pauses_armed]() {
                        return pauses_armed.load(std::memory_order_acquire);
                    },
                    session.deadline(), 5ms)) {
                throw std::runtime_error(
                    "controller did not arm producer pauses");
            }
            static_cast<void>(session.exec(
                "SET application_name = 'cache_native_producer'; "
                "SET bioseq.query_cache_optimisation_mode = 'same'"));
            const auto result = session.exec(
                "SELECT count(*), sum(id) "
                "FROM cache_same_wait_rows WHERE id <= 10");
            producer_count = std::string(result.value(0, 0));
            producer_sum = std::string(result.value(0, 1));
        }});
    tasks.push_back(bioseq::postgres_test::session_task{
        .name = "waiter",
        .body = [&producer_paused, &waiter_count, &waiter_sum](
                    bioseq::postgres_test::session_context& session) {
            if (!bioseq::postgres_test::wait_until(
                    [&producer_paused]() {
                        return producer_paused.load(
                            std::memory_order_acquire);
                    },
                    session.deadline(), 5ms)) {
                throw std::runtime_error(
                    "producer pause was not observed by waiter");
            }
            static_cast<void>(session.exec(
                "SET application_name = 'cache_native_waiter'; "
                "SET bioseq.query_cache_optimisation_mode = 'same'"));
            const auto result = session.exec(
                "SELECT count(*), sum(id) "
                "FROM cache_same_wait_rows WHERE id <= 10");
            waiter_count = std::string(result.value(0, 0));
            waiter_sum = std::string(result.value(0, 1));
        }});
    tasks.push_back(bioseq::postgres_test::session_task{
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
            const auto state = session.exec(
                "SELECT state, waiters "
                "FROM bioseq_cache.entries() "
                "WHERE kind = 'ExactFinalResult'");
            if (state.rows() != 1 || state.value(0, 0) != "BUILDING"
                || state.value(0, 1) != "1") {
                throw std::runtime_error(
                    "waiter did not attach to the BUILDING generation");
            }
            static_cast<void>(session.exec(
                "SELECT bioseq_cache_test.release_pause("
                "'after_building_publish'); "
                "SELECT bioseq_cache_test.release_pause("
                "'after_waiter_registered')"));
        }});

    const auto outcomes =
        bioseq::postgres_test::run_independent_session_tasks(
            fixture.cluster(), tasks, 45s);
    REQUIRE(all_passed(outcomes));
    CHECK(producer_count == "10");
    CHECK(producer_sum == "55");
    CHECK(waiter_count == "10");
    CHECK(waiter_sum == "55");
    const auto stats = bioseq::cache_test::read_cache_stats(admin);
    CHECK(stats.produced == 1);
    CHECK(stats.completed_hits == 1);
    CHECK(stats.inflight_waits == 1);
    bioseq::postgres_test::record_migration_probe(
        "isolation.cache_same_wait.one_producer");
    bioseq::postgres_test::record_migration_probe(
        "isolation.cache_same_wait.waiter_registered");
    bioseq::postgres_test::record_migration_probe(
        "isolation.cache_same_wait.identical_results");
    bioseq::postgres_test::record_migration_probe(
        "isolation.cache_same_wait.counter_contract");
}

TEST_CASE("cancelled waiter drains without disturbing its producer",
          "[cache][integration][concurrency][cancel]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "cancelled waiter drains without disturbing its producer"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& admin = fixture.admin();
    create_wait_rows(admin, "cache_cancel_wait_rows");

    std::atomic<bool> pauses_armed{};
    std::atomic<bool> producer_paused{};
    std::string producer_sum;
    std::vector<bioseq::postgres_test::session_task> tasks;
    tasks.push_back(bioseq::postgres_test::session_task{
        .name = "producer",
        .body = [&pauses_armed, &producer_sum](
                    bioseq::postgres_test::session_context& session) {
            if (!bioseq::postgres_test::wait_until(
                    [&pauses_armed]() {
                        return pauses_armed.load(std::memory_order_acquire);
                    },
                    session.deadline(), 5ms)) {
                throw std::runtime_error(
                    "controller did not arm producer pauses");
            }
            static_cast<void>(session.exec(
                "SET application_name = 'cache_cancel_producer'; "
                "SET bioseq.query_cache_optimisation_mode = 'same'"));
            const auto result = session.exec(cancelled_waiter_query);
            producer_sum = std::string(result.value(0, 0));
        }});
    tasks.push_back(bioseq::postgres_test::session_task{
        .name = "waiter",
        .body = [&producer_paused](
                    bioseq::postgres_test::session_context& session) {
            if (!bioseq::postgres_test::wait_until(
                    [&producer_paused]() {
                        return producer_paused.load(
                            std::memory_order_acquire);
                    },
                    session.deadline(), 5ms)) {
                throw std::runtime_error("producer did not pause");
            }
            static_cast<void>(session.exec(
                "SET application_name = 'cache_cancel_waiter'; "
                "SET bioseq.query_cache_optimisation_mode = 'same'"));
            static_cast<void>(session.exec(cancelled_waiter_query));
        }});
    tasks.push_back(bioseq::postgres_test::session_task{
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
                "SELECT pg_cancel_backend(pid) "
                "FROM pg_stat_activity "
                "WHERE application_name = 'cache_cancel_waiter'");
            if (cancelled.rows() != 1 || cancelled.value(0, 0) != "t") {
                throw std::runtime_error("waiter backend was not cancelled");
            }
            static_cast<void>(session.exec(
                "SELECT bioseq_cache_test.release_pause("
                "'after_waiter_registered'); "
                "SELECT bioseq_cache_test.release_pause("
                "'after_building_publish')"));
        }});

    const auto outcomes =
        bioseq::postgres_test::run_independent_session_tasks(
            fixture.cluster(), tasks, 45s);
    REQUIRE(outcomes.size() == 3);
    CHECK(outcomes[0].passed);
    CHECK_FALSE(outcomes[0].timed_out);
    CHECK_FALSE(outcomes[1].passed);
    CHECK_FALSE(outcomes[1].timed_out);
    CHECK(outcomes[2].passed);
    CHECK(producer_sum == "55");
    CHECK(bioseq::cache_test::integer(
              admin,
              "SELECT count(*) FROM bioseq_cache_test.entry_states() "
              "WHERE waiter_count <> 0 OR reader_count <> 0")
          == 0);
    bioseq::postgres_test::record_migration_probe(
        "isolation.cache_waiter_cancel.cancelled");
    bioseq::postgres_test::record_migration_probe(
        "isolation.cache_waiter_cancel.producer_completes");
    bioseq::postgres_test::record_migration_probe(
        "isolation.cache_waiter_cancel.references_drained");
}
