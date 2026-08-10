#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
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
                 std::string_view extra_columns = {},
                 std::string_view select_tail = {}) {
    const std::string identifier = connection.escape_identifier(table);
    const std::string columns = extra_columns.empty()
        ? "(id integer PRIMARY KEY, value text NOT NULL)"
        : std::string(extra_columns);
    const std::string values = select_tail.empty()
        ? "SELECT i, md5(i::text) "
        : std::string(select_tail);
    bioseq::cache_test::exec(
        connection,
        "CREATE TABLE " + identifier + columns + "; INSERT INTO "
            + identifier + " " + values + "FROM generate_series(1, "
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
            "waiter isolation query returned the wrong row shape");
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

void require_pair(
    const row_pair& value,
    std::string_view expected_rows,
    std::string_view expected_sum) {
    CHECK(value.rows == expected_rows);
    CHECK(value.id_sum == expected_sum);
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

}  // namespace

TEST_CASE("TTL zero preserves only the registered waiter entitlement",
          "[cache][integration][isolation][ttl][concurrency]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "TTL zero preserves only the registered waiter entitlement"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& admin = fixture.admin();
    create_rows(admin, "cache_ttl_zero_rows", 20);

    std::atomic<bool> pauses_armed{};
    std::atomic<bool> producer_paused{};
    std::atomic<bool> producer_done{};
    std::atomic<bool> waiter_done{};
    row_pair producer_rows;
    row_pair waiter_rows;
    row_pair newcomer_rows;
    bool waiter_entitled{};
    bool generation_reclaimed{};
    bioseq::cache_test::cache_stats before_waiter_stats;
    bioseq::cache_test::cache_stats final_stats;

    std::vector<bioseq::postgres_test::session_task> tasks;
    tasks.push_back({
        .name = "controller",
        .body = [&pauses_armed, &producer_paused, &producer_done,
                 &waiter_done, &newcomer_rows, &waiter_entitled,
                 &generation_reclaimed, &before_waiter_stats,
                 &final_stats](
                    bioseq::postgres_test::session_context& session) {
            static_cast<void>(session.exec(
                "SELECT bioseq_cache_test.arm_pause("
                "'before_terminal_publish'); "
                "SELECT bioseq_cache_test.arm_pause("
                "'after_waiter_registered')"));
            pauses_armed.store(true, std::memory_order_release);
            static_cast<void>(session.exec(
                "SELECT bioseq_cache_test.wait_until_paused("
                "'before_terminal_publish')"));
            producer_paused.store(true, std::memory_order_release);
            static_cast<void>(session.exec(
                "SELECT bioseq_cache_test.wait_until_paused("
                "'after_waiter_registered')"));
            static_cast<void>(session.exec(
                "SELECT bioseq_cache_test.release_pause("
                "'before_terminal_publish')"));
            wait_for_flag(
                producer_done, session,
                "TTL-zero producer did not reach terminal publication");
            static_cast<void>(session.exec(
                "SELECT bioseq_cache_test.request_janitor_sweep()"));
            const auto entitled = session.exec(
                "SELECT count(*) = 1, "
                "       count(*) FILTER ("
                "         WHERE state = 'COMPLETE' "
                "           AND waiters = 1 AND readers = 0) = 1 "
                "FROM bioseq_cache.entries() "
                "WHERE kind = 'ExactFinalResult' "
                "  AND sql LIKE '%cache_ttl_zero_rows%'");
            waiter_entitled =
                entitled.rows() == 1
                && entitled.value(0, 0) == "t"
                && entitled.value(0, 1) == "t";

            static_cast<void>(session.exec(
                "SET bioseq.query_cache_optimisation_mode = 'same'; "
                "SET bioseq.query_cache_ttl = 0"));
            newcomer_rows = query_pair(
                session,
                "SELECT count(*), sum(id) FROM cache_ttl_zero_rows");
            const auto interim = session.exec(
                "SELECT produced, completed_hits, inflight_waits, "
                "       reuse_hits, fallbacks, invalidations, evictions, "
                "       logical_bytes, stored_bytes "
                "FROM bioseq_cache.stats()");
            if (interim.rows() != 1 || interim.columns() != 9) {
                throw std::runtime_error(
                    "TTL-zero interim stats returned the wrong shape");
            }
            before_waiter_stats = {
                .produced = std::stoll(std::string(interim.value(0, 0))),
                .completed_hits =
                    std::stoll(std::string(interim.value(0, 1))),
                .inflight_waits =
                    std::stoll(std::string(interim.value(0, 2))),
                .reuse_hits =
                    std::stoll(std::string(interim.value(0, 3))),
                .fallbacks =
                    std::stoll(std::string(interim.value(0, 4))),
                .invalidations =
                    std::stoll(std::string(interim.value(0, 5))),
                .evictions =
                    std::stoll(std::string(interim.value(0, 6))),
                .logical_bytes =
                    std::stoll(std::string(interim.value(0, 7))),
                .stored_bytes =
                    std::stoll(std::string(interim.value(0, 8))),
            };
            static_cast<void>(session.exec(
                "SELECT bioseq_cache_test.release_pause("
                "'after_waiter_registered')"));
            wait_for_flag(
                waiter_done, session,
                "TTL-zero waiter did not consume its entitlement");
            static_cast<void>(session.exec(
                "SELECT bioseq_cache_test.request_janitor_sweep()"));
            const auto remaining = session.exec(
                "SELECT count(*) "
                "FROM bioseq_cache.entries() "
                "WHERE kind = 'ExactFinalResult' "
                "  AND sql LIKE '%cache_ttl_zero_rows%'");
            generation_reclaimed =
                remaining.rows() == 1
                && remaining.value(0, 0) == "0";
            const auto final = session.exec(
                "SELECT produced, completed_hits, inflight_waits, "
                "       reuse_hits, fallbacks, invalidations, evictions, "
                "       logical_bytes, stored_bytes "
                "FROM bioseq_cache.stats()");
            if (final.rows() != 1 || final.columns() != 9) {
                throw std::runtime_error(
                    "TTL-zero final stats returned the wrong shape");
            }
            final_stats = {
                .produced = std::stoll(std::string(final.value(0, 0))),
                .completed_hits =
                    std::stoll(std::string(final.value(0, 1))),
                .inflight_waits =
                    std::stoll(std::string(final.value(0, 2))),
                .reuse_hits =
                    std::stoll(std::string(final.value(0, 3))),
                .fallbacks =
                    std::stoll(std::string(final.value(0, 4))),
                .invalidations =
                    std::stoll(std::string(final.value(0, 5))),
                .evictions =
                    std::stoll(std::string(final.value(0, 6))),
                .logical_bytes =
                    std::stoll(std::string(final.value(0, 7))),
                .stored_bytes =
                    std::stoll(std::string(final.value(0, 8))),
            };
        }});
    tasks.push_back({
        .name = "producer",
        .body = [&pauses_armed, &producer_done, &producer_rows](
                    bioseq::postgres_test::session_context& session) {
            wait_for_flag(
                pauses_armed, session,
                "TTL-zero pauses were not armed");
            static_cast<void>(session.exec(
                "SET bioseq.query_cache_optimisation_mode = 'same'; "
                "SET bioseq.query_cache_ttl = 0"));
            producer_rows = query_pair(
                session,
                "SELECT count(*), sum(id) FROM cache_ttl_zero_rows");
            producer_done.store(true, std::memory_order_release);
        }});
    tasks.push_back({
        .name = "waiter",
        .body = [&producer_paused, &waiter_done, &waiter_rows](
                    bioseq::postgres_test::session_context& session) {
            wait_for_flag(
                producer_paused, session,
                "TTL-zero waiter did not observe producer pause");
            static_cast<void>(session.exec(
                "SET bioseq.query_cache_optimisation_mode = 'same'; "
                "SET bioseq.query_cache_ttl = 0"));
            waiter_rows = query_pair(
                session,
                "SELECT count(*), sum(id) FROM cache_ttl_zero_rows");
            waiter_done.store(true, std::memory_order_release);
        }});

    const auto outcomes =
        bioseq::postgres_test::run_independent_session_tasks(
            fixture.cluster(), tasks, 45s);
    require_outcomes(outcomes);
    require_pair(producer_rows, "20", "210");
    require_pair(waiter_rows, "20", "210");
    require_pair(newcomer_rows, "20", "210");
    CHECK(waiter_entitled);
    CHECK(generation_reclaimed);
    CHECK(before_waiter_stats.produced == 2);
    CHECK(before_waiter_stats.completed_hits == 0);
    CHECK(before_waiter_stats.inflight_waits == 1);
    CHECK(before_waiter_stats.fallbacks == 0);
    CHECK(final_stats.produced == 2);
    CHECK(final_stats.completed_hits == 1);
    CHECK(final_stats.inflight_waits == 1);
    CHECK(final_stats.fallbacks == 0);

    bioseq::postgres_test::record_migration_probe(
        "isolation.cache_ttl_zero.entitled_waiter");
}

TEST_CASE("ten waiters share one exact generation",
          "[cache][integration][isolation][concurrency][ten-waiters]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "ten waiters share one exact generation"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& admin = fixture.admin();
    create_rows(admin, "cache_ten_waiter_rows", 100);

    std::atomic<bool> pauses_armed{};
    std::atomic<bool> producer_paused{};
    std::atomic<bool> producer_done{};
    row_pair producer_rows;
    std::array<row_pair, 10> waiter_rows;
    std::string observed_building;
    std::string observed_waiters;

    std::vector<bioseq::postgres_test::session_task> tasks;
    tasks.reserve(12);
    tasks.push_back({
        .name = "controller",
        .body = [&pauses_armed, &producer_paused, &producer_done,
                 &observed_building, &observed_waiters](
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
            const bool registered =
                bioseq::postgres_test::wait_until(
                    [&session, &observed_building,
                     &observed_waiters]() {
                        const auto state = session.exec(
                            "SELECT count(*) FILTER ("
                            "         WHERE state = 'BUILDING'), "
                            "       coalesce(max(waiter_count), 0) "
                            "FROM bioseq_cache_test.entry_states()");
                        if (state.rows() != 1
                            || state.columns() != 2) {
                            return false;
                        }
                        observed_building =
                            std::string(state.value(0, 0));
                        observed_waiters =
                            std::string(state.value(0, 1));
                        return observed_building == "1"
                            && observed_waiters == "10";
                    },
                    session.deadline(), 10ms);
            if (!registered) {
                throw std::runtime_error(
                    "ten waiters did not register on one generation");
            }
            static_cast<void>(session.exec(
                "SELECT bioseq_cache_test.release_pause("
                "'after_building_publish')"));
            wait_for_flag(
                producer_done, session,
                "ten-waiter producer did not complete");
            static_cast<void>(session.exec(
                "SELECT bioseq_cache_test.release_pause("
                "'after_waiter_registered')"));
        }});
    tasks.push_back({
        .name = "producer",
        .body = [&pauses_armed, &producer_done, &producer_rows](
                    bioseq::postgres_test::session_context& session) {
            wait_for_flag(
                pauses_armed, session,
                "ten-waiter pauses were not armed");
            static_cast<void>(session.exec(
                "SET bioseq.query_cache_optimisation_mode = 'same'"));
            producer_rows = query_pair(
                session,
                "SELECT count(*), sum(id) "
                "FROM cache_ten_waiter_rows WHERE id <= 10");
            producer_done.store(true, std::memory_order_release);
        }});
    for (std::size_t index = 0; index < waiter_rows.size(); ++index) {
        tasks.push_back({
            .name = "waiter_" + std::to_string(index + 1),
            .body = [index, &producer_paused, &waiter_rows](
                        bioseq::postgres_test::session_context& session) {
                wait_for_flag(
                    producer_paused, session,
                    "ten-waiter session did not observe producer pause");
                static_cast<void>(session.exec(
                    "SET bioseq.query_cache_optimisation_mode = 'same'"));
                waiter_rows[index] = query_pair(
                    session,
                    "SELECT count(*), sum(id) "
                    "FROM cache_ten_waiter_rows WHERE id <= 10");
            }});
    }

    const auto outcomes =
        bioseq::postgres_test::run_independent_session_tasks(
            fixture.cluster(), tasks, 60s);
    require_outcomes(outcomes);
    require_pair(producer_rows, "10", "55");
    for (const row_pair& waiter : waiter_rows) {
        require_pair(waiter, "10", "55");
    }
    CHECK(observed_building == "1");
    CHECK(observed_waiters == "10");
    const auto stats = bioseq::cache_test::read_cache_stats(admin);
    CHECK(stats.produced == 1);
    CHECK(stats.completed_hits == 10);
    CHECK(stats.inflight_waits == 10);

    bioseq::postgres_test::record_migration_probe(
        "isolation.cache_ten_waiters.single_producer");
}

TEST_CASE("unrelated COMPLETE key remains isolated from BUILDING transition",
          "[cache][integration][isolation][concurrency][key-isolation]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "unrelated COMPLETE key remains isolated from BUILDING transition"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& admin = fixture.admin();
    create_rows(
        admin, "cache_unrelated_completion_rows", 20,
        "(id integer PRIMARY KEY, bucket integer NOT NULL, "
        "value text NOT NULL)",
        "SELECT i, CASE WHEN i <= 10 THEN 1 ELSE 2 END, "
        "md5(i::text) ");
    bioseq::cache_test::exec(
        admin, "SET bioseq.query_cache_optimisation_mode = 'same'");
    const row_pair seeded = pair_from(admin.exec(
        "SELECT count(*), sum(id) "
        "FROM cache_unrelated_completion_rows WHERE bucket = 2"));
    require_pair(seeded, "10", "155");

    std::atomic<bool> pauses_armed{};
    std::atomic<bool> producer_paused{};
    std::atomic<bool> producer_done{};
    row_pair producer_rows;
    row_pair waiter_rows;
    row_pair unrelated_hit;
    std::string observed_building;
    std::string observed_complete;
    std::string observed_waiters;

    std::vector<bioseq::postgres_test::session_task> tasks;
    tasks.push_back({
        .name = "controller",
        .body = [&pauses_armed, &producer_paused, &producer_done,
                 &unrelated_hit, &observed_building,
                 &observed_complete, &observed_waiters](
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
                "SET bioseq.query_cache_optimisation_mode = 'same'"));
            unrelated_hit = query_pair(
                session,
                "SELECT count(*), sum(id) "
                "FROM cache_unrelated_completion_rows WHERE bucket = 2");
            const auto state = session.exec(
                "SELECT count(*) FILTER (WHERE state = 'BUILDING'), "
                "       count(*) FILTER (WHERE state = 'COMPLETE'), "
                "       coalesce(sum(waiter_count), 0) "
                "FROM bioseq_cache_test.entry_states()");
            if (state.rows() != 1 || state.columns() != 3) {
                throw std::runtime_error(
                    "unrelated-completion state returned wrong shape");
            }
            observed_building = std::string(state.value(0, 0));
            observed_complete = std::string(state.value(0, 1));
            observed_waiters = std::string(state.value(0, 2));
            static_cast<void>(session.exec(
                "SELECT bioseq_cache_test.release_pause("
                "'after_building_publish')"));
            wait_for_flag(
                producer_done, session,
                "unrelated-key producer did not complete");
            static_cast<void>(session.exec(
                "SELECT bioseq_cache_test.release_pause("
                "'after_waiter_registered')"));
        }});
    tasks.push_back({
        .name = "producer",
        .body = [&pauses_armed, &producer_done, &producer_rows](
                    bioseq::postgres_test::session_context& session) {
            wait_for_flag(
                pauses_armed, session,
                "unrelated-key pauses were not armed");
            static_cast<void>(session.exec(
                "SET bioseq.query_cache_optimisation_mode = 'same'"));
            producer_rows = query_pair(
                session,
                "SELECT count(*), sum(id) "
                "FROM cache_unrelated_completion_rows WHERE bucket = 1");
            producer_done.store(true, std::memory_order_release);
        }});
    tasks.push_back({
        .name = "waiter",
        .body = [&producer_paused, &waiter_rows](
                    bioseq::postgres_test::session_context& session) {
            wait_for_flag(
                producer_paused, session,
                "unrelated-key waiter did not observe producer pause");
            static_cast<void>(session.exec(
                "SET bioseq.query_cache_optimisation_mode = 'same'"));
            waiter_rows = query_pair(
                session,
                "SELECT count(*), sum(id) "
                "FROM cache_unrelated_completion_rows WHERE bucket = 1");
        }});

    const auto outcomes =
        bioseq::postgres_test::run_independent_session_tasks(
            fixture.cluster(), tasks, 45s);
    require_outcomes(outcomes);
    require_pair(producer_rows, "10", "55");
    require_pair(waiter_rows, "10", "55");
    require_pair(unrelated_hit, "10", "155");
    CHECK(observed_building == "1");
    CHECK(observed_complete == "1");
    CHECK(observed_waiters == "1");
    const auto stats = bioseq::cache_test::read_cache_stats(admin);
    CHECK(stats.produced == 2);
    CHECK(stats.completed_hits == 2);
    CHECK(stats.inflight_waits == 1);

    bioseq::postgres_test::record_migration_probe(
        "isolation.cache_unrelated_completion.key_isolation");
}
