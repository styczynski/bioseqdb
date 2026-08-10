#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
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

struct relation_state {
    std::int64_t transitions{};
    std::int64_t epoch{};
};

void wait_for_flag(
    const std::atomic<bool>& flag,
    bioseq::postgres_test::bounded_session_context& session,
    std::string_view diagnostic) {
    if (!bioseq::postgres_test::wait_until(
            [&flag]() {
                return flag.load(std::memory_order_acquire);
            },
            session.deadline(), 5ms)) {
        throw std::runtime_error(std::string(diagnostic));
    }
}

[[nodiscard]] row_pair pair_from(const bioseq::libpq::command_result& result) {
    if (result.row_count() != 1 || result.column_count() != 2
        || result.cell_is_null(0, 0) || result.cell_is_null(0, 1)) {
        throw std::runtime_error(
            "visibility isolation query returned the wrong row shape");
    }
    return {
        .rows = std::string(result.text_value(0, 0)),
        .id_sum = std::string(result.text_value(0, 1)),
    };
}

[[nodiscard]] row_pair query_pair(
    bioseq::postgres_test::bounded_session_context& session,
    std::string_view sql) {
    return pair_from(session.execute(sql));
}

void require_pair(
    const row_pair& value,
    std::string_view expected_rows,
    std::string_view expected_sum) {
    CHECK(value.rows == expected_rows);
    CHECK(value.id_sum == expected_sum);
}

void require_outcomes(
    const std::vector<
        bioseq::postgres_test::independent_session_result>& outcomes) {
    for (const auto& outcome : outcomes) {
        INFO("session=" << outcome.name << " error=" << outcome.error);
        REQUIRE(outcome.passed);
        REQUIRE_FALSE(outcome.timed_out);
    }
}

[[nodiscard]] relation_state read_relation_state(
    bioseq::postgres_test::bounded_session_context& session,
    std::string_view relation) {
    const auto result = session.execute(
        "SELECT relation_transitions, relation_epoch "
        "FROM bioseq_cache_test.relation_transition_state("
        + session.escape_literal(relation) + "::regclass)");
    if (result.row_count() != 1 || result.column_count() != 2
        || result.cell_is_null(0, 0) || result.cell_is_null(0, 1)) {
        throw std::runtime_error(
            "relation transition diagnostic returned the wrong shape");
    }
    return {
        .transitions =
            std::stoll(std::string(result.text_value(0, 0))),
        .epoch = std::stoll(std::string(result.text_value(0, 1))),
    };
}

[[nodiscard]] relation_state read_relation_state(
    bioseq::libpq::client_connection& connection,
    std::string_view relation) {
    const auto result = connection.execute(
        "SELECT relation_transitions, relation_epoch "
        "FROM bioseq_cache_test.relation_transition_state("
        + connection.escape_literal(relation) + "::regclass)");
    if (result.row_count() != 1 || result.column_count() != 2
        || result.cell_is_null(0, 0) || result.cell_is_null(0, 1)) {
        throw std::runtime_error(
            "relation transition diagnostic returned the wrong shape");
    }
    return {
        .transitions =
            std::stoll(std::string(result.text_value(0, 0))),
        .epoch = std::stoll(std::string(result.text_value(0, 1))),
    };
}

[[nodiscard]] std::string single_value(
    const bioseq::libpq::command_result& result) {
    if (result.row_count() != 1 || result.column_count() != 1
        || result.cell_is_null(0, 0)) {
        throw std::runtime_error(
            "visibility query did not return one scalar");
    }
    return std::string(result.text_value(0, 0));
}

}  // namespace

TEST_CASE("unrelated commit rejects a pre-commit cache publication",
          "[component:cache][layer:integration]"
          "[feature:isolation][resource:postgres]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& admin = fixture.admin();
    bioseq::cache_test::execute(
        admin,
        "CREATE TABLE cache_commit_target("
        "id integer PRIMARY KEY, value text NOT NULL); "
        "CREATE TABLE cache_commit_unrelated(id integer PRIMARY KEY); "
        "INSERT INTO cache_commit_target VALUES "
        "(1, 'one'), (2, 'two'), (3, 'three'); "
        "SELECT bioseq_cache.invalidate_all(); "
        "SELECT bioseq_cache_test.request_janitor_sweep(); "
        "SELECT bioseq_cache.reset_stats()");

    std::atomic<bool> pause_armed{};
    std::atomic<bool> producer_done{};
    row_pair producer_rows;
    row_pair consumer_rows;
    row_pair hit_rows;
    std::string rejected_complete;
    std::string final_complete;
    bioseq::cache_test::cache_stats rejected_stats;
    bioseq::cache_test::cache_stats final_stats;

    std::vector<bioseq::postgres_test::independent_session_task> tasks;
    tasks.push_back({
        .name = "controller",
        .body = [&pause_armed, &producer_done, &consumer_rows,
                 &hit_rows, &rejected_complete, &final_complete,
                 &rejected_stats, &final_stats](
                    bioseq::postgres_test::bounded_session_context& session) {
            static_cast<void>(session.execute(
                "SELECT bioseq_cache_test.arm_pause("
                "'before_terminal_publish')"));
            pause_armed.store(true, std::memory_order_release);
            static_cast<void>(session.execute(
                "SELECT bioseq_cache_test.wait_until_paused("
                "'before_terminal_publish')"));
            static_cast<void>(session.execute(
                "INSERT INTO cache_commit_unrelated VALUES (1)"));
            static_cast<void>(session.execute(
                "SELECT bioseq_cache_test.release_pause("
                "'before_terminal_publish')"));
            wait_for_flag(
                producer_done, session,
                "pre-commit producer did not finish after release");

            const auto rejected = session.execute(
                "SELECT (SELECT count(*) FILTER ("
                "          WHERE state = 'COMPLETE') "
                "        FROM bioseq_cache_test.entry_states()), "
                "       produced, completed_hits, inflight_waits, "
                "       reuse_hits, fallbacks, invalidations, evictions, "
                "       logical_bytes, stored_bytes "
                "FROM bioseq_cache.stats()");
            if (rejected.row_count() != 1 || rejected.column_count() != 10) {
                throw std::runtime_error(
                    "unrelated-commit rejection stats have wrong shape");
            }
            rejected_complete =
                std::string(rejected.text_value(0, 0));
            rejected_stats = {
                .produced =
                    std::stoll(std::string(rejected.text_value(0, 1))),
                .completed_hits =
                    std::stoll(std::string(rejected.text_value(0, 2))),
                .inflight_waits =
                    std::stoll(std::string(rejected.text_value(0, 3))),
                .reuse_hits =
                    std::stoll(std::string(rejected.text_value(0, 4))),
                .fallbacks =
                    std::stoll(std::string(rejected.text_value(0, 5))),
                .invalidations =
                    std::stoll(std::string(rejected.text_value(0, 6))),
                .evictions =
                    std::stoll(std::string(rejected.text_value(0, 7))),
                .logical_bytes =
                    std::stoll(std::string(rejected.text_value(0, 8))),
                .stored_bytes =
                    std::stoll(std::string(rejected.text_value(0, 9))),
            };
            static_cast<void>(session.execute(
                "SET bioseq_cache.query_cache.mode = 'exact_query'"));
            consumer_rows = query_pair(
                session,
                "SELECT count(*), sum(id) FROM cache_commit_target");
            hit_rows = query_pair(
                session,
                "SELECT count(*), sum(id) FROM cache_commit_target");
            const auto stats = session.execute(
                "SELECT (SELECT count(*) FILTER ("
                "          WHERE state = 'COMPLETE') "
                "        FROM bioseq_cache_test.entry_states()), "
                "       produced, completed_hits, inflight_waits, "
                "       reuse_hits, fallbacks, invalidations, evictions, "
                "       logical_bytes, stored_bytes "
                "FROM bioseq_cache.stats()");
            if (stats.row_count() != 1 || stats.column_count() != 10) {
                throw std::runtime_error(
                    "unrelated-commit final stats have wrong shape");
            }
            final_complete =
                std::string(stats.text_value(0, 0));
            final_stats = {
                .produced =
                    std::stoll(std::string(stats.text_value(0, 1))),
                .completed_hits =
                    std::stoll(std::string(stats.text_value(0, 2))),
                .inflight_waits =
                    std::stoll(std::string(stats.text_value(0, 3))),
                .reuse_hits =
                    std::stoll(std::string(stats.text_value(0, 4))),
                .fallbacks =
                    std::stoll(std::string(stats.text_value(0, 5))),
                .invalidations =
                    std::stoll(std::string(stats.text_value(0, 6))),
                .evictions =
                    std::stoll(std::string(stats.text_value(0, 7))),
                .logical_bytes =
                    std::stoll(std::string(stats.text_value(0, 8))),
                .stored_bytes =
                    std::stoll(std::string(stats.text_value(0, 9))),
            };
        }});
    tasks.push_back({
        .name = "producer",
        .body = [&pause_armed, &producer_done, &producer_rows](
                    bioseq::postgres_test::bounded_session_context& session) {
            wait_for_flag(
                pause_armed, session,
                "unrelated-commit pause was not armed");
            static_cast<void>(session.execute(
                "SET bioseq_cache.query_cache.mode = 'exact_query'"));
            producer_rows = query_pair(
                session,
                "SELECT count(*), sum(id) FROM cache_commit_target");
            producer_done.store(true, std::memory_order_release);
        }});

    const auto outcomes =
        bioseq::postgres_test::run_independent_sessions(
            fixture.cluster(), tasks, 45s);
    require_outcomes(outcomes);
    require_pair(producer_rows, "3", "6");
    require_pair(consumer_rows, "3", "6");
    require_pair(hit_rows, "3", "6");
    CHECK(rejected_complete == "0");
    CHECK(rejected_stats.produced == 0);
    CHECK(rejected_stats.fallbacks == 1);
    CHECK(final_complete == "1");
    CHECK(final_stats.produced == 1);
    CHECK(final_stats.completed_hits == 1);
    CHECK(final_stats.fallbacks == 1);

}

TEST_CASE("snapshot predating commit cannot claim a generation",
          "[component:cache][layer:integration]"
          "[feature:isolation][resource:postgres]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& admin = fixture.admin();
    bioseq::cache_test::execute(
        admin,
        "SELECT bioseq_cache.invalidate_all(); "
        "SELECT bioseq_cache_test.request_janitor_sweep()");
    REQUIRE(single_value(admin.execute(
        "SELECT count(*)::text FROM bioseq_cache.entries()")) == "0");
    bioseq::cache_test::execute(
        admin,
        "CREATE TABLE cache_snapshot_claim_rows("
        "id integer PRIMARY KEY, value text NOT NULL); "
        "INSERT INTO cache_snapshot_claim_rows VALUES "
        "(1, 'one'), (2, 'two'), (3, 'three'); "
        "SELECT bioseq_cache.invalidate_all(); "
        "SELECT bioseq_cache_test.request_janitor_sweep(); "
        "SELECT bioseq_cache.reset_stats()");

    std::atomic<bool> pause_armed{};
    std::atomic<bool> old_snapshot_done{};
    std::atomic<bool> fresh_queries_allowed{};
    std::atomic<bool> consumer_done{};
    row_pair old_snapshot_rows;
    row_pair fresh_rows;
    row_pair hit_rows;
    std::string rejected_entries;
    bioseq::cache_test::cache_stats rejected_stats;
    bioseq::cache_test::cache_stats final_stats;

    std::vector<bioseq::postgres_test::independent_session_task> tasks;
    tasks.push_back({
        .name = "controller",
        .body = [&pause_armed, &old_snapshot_done,
                 &fresh_queries_allowed, &consumer_done,
                 &rejected_entries, &rejected_stats, &final_stats](
                    bioseq::postgres_test::bounded_session_context& session) {
            static_cast<void>(session.execute(
                "SELECT bioseq_cache_test.arm_pause("
                "'before_visibility_ticket')"));
            pause_armed.store(true, std::memory_order_release);
            static_cast<void>(session.execute(
                "SELECT bioseq_cache_test.wait_until_paused("
                "'before_visibility_ticket')"));
            static_cast<void>(session.execute(
                "INSERT INTO cache_snapshot_claim_rows "
                "VALUES (4, 'committed')"));
            static_cast<void>(session.execute(
                "SELECT bioseq_cache_test.release_pause("
                "'before_visibility_ticket')"));
            wait_for_flag(
                old_snapshot_done, session,
                "old-snapshot claim query did not finish");
            const auto rejected = session.execute(
                "SELECT (SELECT count(*) "
                "        FROM bioseq_cache_test.entry_states()), "
                "       produced, completed_hits, inflight_waits, "
                "       reuse_hits, fallbacks, invalidations, evictions, "
                "       logical_bytes, stored_bytes "
                "FROM bioseq_cache.stats()");
            if (rejected.row_count() != 1 || rejected.column_count() != 10) {
                throw std::runtime_error(
                    "snapshot-claim rejection stats have wrong shape");
            }
            rejected_entries =
                std::string(rejected.text_value(0, 0));
            rejected_stats.produced =
                std::stoll(std::string(rejected.text_value(0, 1)));
            rejected_stats.completed_hits =
                std::stoll(std::string(rejected.text_value(0, 2)));
            rejected_stats.fallbacks =
                std::stoll(std::string(rejected.text_value(0, 5)));
            fresh_queries_allowed.store(
                true, std::memory_order_release);
            wait_for_flag(
                consumer_done, session,
                "fresh snapshot-claim queries did not finish");
            const auto stats = session.execute(
                "SELECT produced, completed_hits, fallbacks "
                "FROM bioseq_cache.stats()");
            if (stats.row_count() != 1 || stats.column_count() != 3) {
                throw std::runtime_error(
                    "snapshot-claim final stats have wrong shape");
            }
            final_stats.produced =
                std::stoll(std::string(stats.text_value(0, 0)));
            final_stats.completed_hits =
                std::stoll(std::string(stats.text_value(0, 1)));
            final_stats.fallbacks =
                std::stoll(std::string(stats.text_value(0, 2)));
        }});
    tasks.push_back({
        .name = "old_snapshot_consumer",
        .body = [&pause_armed, &old_snapshot_done,
                 &fresh_queries_allowed, &consumer_done,
                 &old_snapshot_rows, &fresh_rows, &hit_rows](
                    bioseq::postgres_test::bounded_session_context& session) {
            wait_for_flag(
                pause_armed, session,
                "snapshot-claim pause was not armed");
            static_cast<void>(session.execute(
                "SET bioseq_cache.query_cache.mode = 'exact_query'"));
            old_snapshot_rows = query_pair(
                session,
                "SELECT count(*), sum(id) "
                "FROM cache_snapshot_claim_rows");
            old_snapshot_done.store(true, std::memory_order_release);
            wait_for_flag(
                fresh_queries_allowed, session,
                "snapshot-claim rejection state was not sampled");
            fresh_rows = query_pair(
                session,
                "SELECT count(*), sum(id) "
                "FROM cache_snapshot_claim_rows");
            hit_rows = query_pair(
                session,
                "SELECT count(*), sum(id) "
                "FROM cache_snapshot_claim_rows");
            consumer_done.store(true, std::memory_order_release);
        }});

    const auto outcomes =
        bioseq::postgres_test::run_independent_sessions(
            fixture.cluster(), tasks, 45s);
    require_outcomes(outcomes);
    require_pair(old_snapshot_rows, "3", "6");
    require_pair(fresh_rows, "4", "10");
    require_pair(hit_rows, "4", "10");
    CHECK(rejected_entries == "0");
    CHECK(rejected_stats.produced == 0);
    CHECK(rejected_stats.completed_hits == 0);
    CHECK(rejected_stats.fallbacks == 1);
    CHECK(final_stats.produced == 1);
    CHECK(final_stats.completed_hits == 1);
    CHECK(final_stats.fallbacks == 1);

}

TEST_CASE("snapshot predating commit cannot replay stale COMPLETE generation",
          "[component:cache][layer:integration]"
          "[feature:isolation][resource:postgres]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& admin = fixture.admin();
    bioseq::cache_test::execute(
        admin,
        "CREATE TABLE cache_snapshot_lookup_rows("
        "id integer PRIMARY KEY, value text NOT NULL); "
        "INSERT INTO cache_snapshot_lookup_rows VALUES "
        "(1, 'one'), (2, 'two'), (3, 'three'); "
        "SELECT bioseq_cache.invalidate_all(); "
        "SELECT bioseq_cache_test.request_janitor_sweep(); "
        "SET bioseq_cache.query_cache.mode = 'exact_query'");
    require_pair(
        pair_from(admin.execute(
            "SELECT count(*), sum(id) "
            "FROM cache_snapshot_lookup_rows")),
        "3", "6");
    bioseq::cache_test::execute(
        admin, "SELECT bioseq_cache.reset_stats()");

    std::atomic<bool> pause_armed{};
    std::atomic<bool> old_snapshot_done{};
    std::atomic<bool> fresh_queries_allowed{};
    std::atomic<bool> consumer_done{};
    row_pair old_snapshot_rows;
    row_pair fresh_rows;
    row_pair hit_rows;
    std::string rejected_complete;
    bioseq::cache_test::cache_stats rejected_stats;
    bioseq::cache_test::cache_stats final_stats;

    std::vector<bioseq::postgres_test::independent_session_task> tasks;
    tasks.push_back({
        .name = "controller",
        .body = [&pause_armed, &old_snapshot_done,
                 &fresh_queries_allowed, &consumer_done,
                 &rejected_complete, &rejected_stats, &final_stats](
                    bioseq::postgres_test::bounded_session_context& session) {
            static_cast<void>(session.execute(
                "SELECT bioseq_cache_test.arm_pause("
                "'before_visibility_ticket')"));
            pause_armed.store(true, std::memory_order_release);
            static_cast<void>(session.execute(
                "SELECT bioseq_cache_test.wait_until_paused("
                "'before_visibility_ticket')"));
            static_cast<void>(session.execute(
                "INSERT INTO cache_snapshot_lookup_rows "
                "VALUES (4, 'committed')"));
            static_cast<void>(session.execute(
                "SELECT bioseq_cache_test.release_pause("
                "'before_visibility_ticket')"));
            wait_for_flag(
                old_snapshot_done, session,
                "old-snapshot lookup query did not finish");
            const auto rejected = session.execute(
                "SELECT (SELECT count(*) FILTER ("
                "          WHERE state = 'COMPLETE') "
                "        FROM bioseq_cache_test.entry_states()), "
                "       produced, completed_hits, fallbacks "
                "FROM bioseq_cache.stats()");
            if (rejected.row_count() != 1 || rejected.column_count() != 4) {
                throw std::runtime_error(
                    "snapshot-lookup rejection stats have wrong shape");
            }
            rejected_complete =
                std::string(rejected.text_value(0, 0));
            rejected_stats.produced =
                std::stoll(std::string(rejected.text_value(0, 1)));
            rejected_stats.completed_hits =
                std::stoll(std::string(rejected.text_value(0, 2)));
            rejected_stats.fallbacks =
                std::stoll(std::string(rejected.text_value(0, 3)));
            fresh_queries_allowed.store(
                true, std::memory_order_release);
            wait_for_flag(
                consumer_done, session,
                "fresh snapshot-lookup queries did not finish");
            const auto stats = session.execute(
                "SELECT produced, completed_hits, fallbacks "
                "FROM bioseq_cache.stats()");
            if (stats.row_count() != 1 || stats.column_count() != 3) {
                throw std::runtime_error(
                    "snapshot-lookup final stats have wrong shape");
            }
            final_stats.produced =
                std::stoll(std::string(stats.text_value(0, 0)));
            final_stats.completed_hits =
                std::stoll(std::string(stats.text_value(0, 1)));
            final_stats.fallbacks =
                std::stoll(std::string(stats.text_value(0, 2)));
        }});
    tasks.push_back({
        .name = "old_snapshot_consumer",
        .body = [&pause_armed, &old_snapshot_done,
                 &fresh_queries_allowed, &consumer_done,
                 &old_snapshot_rows, &fresh_rows, &hit_rows](
                    bioseq::postgres_test::bounded_session_context& session) {
            wait_for_flag(
                pause_armed, session,
                "snapshot-lookup pause was not armed");
            static_cast<void>(session.execute(
                "SET bioseq_cache.query_cache.mode = 'exact_query'"));
            old_snapshot_rows = query_pair(
                session,
                "SELECT count(*), sum(id) "
                "FROM cache_snapshot_lookup_rows");
            old_snapshot_done.store(true, std::memory_order_release);
            wait_for_flag(
                fresh_queries_allowed, session,
                "snapshot-lookup rejection state was not sampled");
            fresh_rows = query_pair(
                session,
                "SELECT count(*), sum(id) "
                "FROM cache_snapshot_lookup_rows");
            hit_rows = query_pair(
                session,
                "SELECT count(*), sum(id) "
                "FROM cache_snapshot_lookup_rows");
            consumer_done.store(true, std::memory_order_release);
        }});

    const auto outcomes =
        bioseq::postgres_test::run_independent_sessions(
            fixture.cluster(), tasks, 45s);
    require_outcomes(outcomes);
    require_pair(old_snapshot_rows, "3", "6");
    require_pair(fresh_rows, "4", "10");
    require_pair(hit_rows, "4", "10");
    CHECK(rejected_complete == "0");
    CHECK(rejected_stats.produced == 0);
    CHECK(rejected_stats.completed_hits == 0);
    CHECK(rejected_stats.fallbacks == 1);
    CHECK(final_stats.produced == 1);
    CHECK(final_stats.completed_hits == 1);
    CHECK(final_stats.fallbacks == 1);

}

TEST_CASE("overlapping relation transitions advance epochs independently",
          "[component:cache][layer:integration]"
          "[feature:isolation][resource:postgres]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& admin = fixture.admin();
    bioseq::cache_test::execute(
        admin,
        "CREATE TABLE cache_overlapping_transition_rows("
        "id integer PRIMARY KEY, value text NOT NULL); "
        "INSERT INTO cache_overlapping_transition_rows VALUES "
        "(1, 'one'), (2, 'two'); "
        "SET bioseq_cache.query_cache.mode = 'exact_query'");
    CHECK(bioseq::cache_test::scalar(
              admin,
              // Keep the cacheable expression identical to the current contract.
              // Libpq already exposes the text[] value as wire text; an
              // explicit ::text adds stable array_out() and fails the exact
              // cache's immutable-expression admission gate.
              "SELECT array_agg(value ORDER BY id) "
              "FROM cache_overlapping_transition_rows")
          == "{one,two}");
    bioseq::cache_test::execute(
        admin,
        "SELECT bioseq_cache.invalidate_all(); "
        "SELECT bioseq_cache_test.request_janitor_sweep(); "
        "SELECT bioseq_cache.reset_stats()");
    const relation_state baseline = read_relation_state(
        admin, "cache_overlapping_transition_rows");

    std::atomic<int> writer_a_pid{};
    std::atomic<int> writer_b_pid{};
    std::atomic<bool> pauses_armed{};
    std::atomic<bool> writer_a_done{};
    std::atomic<bool> writer_b_done{};
    relation_state both_published;
    relation_state one_published;
    relation_state finished;
    std::string during_transition;
    std::string after_transitions;
    std::string warm_hit;
    std::string during_entries;
    bioseq::cache_test::cache_stats during_stats;
    bioseq::cache_test::cache_stats final_stats;

    std::vector<bioseq::postgres_test::independent_session_task> tasks;
    tasks.push_back({
        .name = "controller",
        .body = [&writer_a_pid, &writer_b_pid, &pauses_armed,
                 &writer_a_done, &writer_b_done, &both_published,
                 &one_published, &finished, &during_transition,
                 &after_transitions, &warm_hit, &during_entries,
                 &during_stats, &final_stats](
                    bioseq::postgres_test::bounded_session_context& session) {
            const bool registered =
                bioseq::postgres_test::wait_until(
                    [&writer_a_pid, &writer_b_pid]() {
                        return writer_a_pid.load(
                                   std::memory_order_acquire) > 0
                            && writer_b_pid.load(
                                   std::memory_order_acquire) > 0;
                    },
                    session.deadline(), 5ms);
            if (!registered) {
                throw std::runtime_error(
                    "overlapping-transition writers did not register");
            }
            const int pid_a =
                writer_a_pid.load(std::memory_order_acquire);
            const int pid_b =
                writer_b_pid.load(std::memory_order_acquire);
            static_cast<void>(session.execute(
                "SELECT bioseq_cache_test.arm_backend_pause("
                "'after_transition_publish', "
                + std::to_string(pid_a)
                + "); SELECT bioseq_cache_test.arm_backend_pause("
                "'after_transition_publish', "
                + std::to_string(pid_b) + ")"));
            pauses_armed.store(true, std::memory_order_release);
            static_cast<void>(session.execute(
                "SELECT bioseq_cache_test.wait_until_backend_paused("
                "'after_transition_publish', "
                + std::to_string(pid_a)
                + "); SELECT "
                "bioseq_cache_test.wait_until_backend_paused("
                "'after_transition_publish', "
                + std::to_string(pid_b) + ")"));
            both_published = read_relation_state(
                session, "cache_overlapping_transition_rows");

            static_cast<void>(session.execute(
                "SELECT bioseq_cache_test.release_backend_pause("
                "'after_transition_publish', "
                + std::to_string(pid_a) + ")"));
            wait_for_flag(
                writer_a_done, session,
                "first overlapping transition did not commit");
            one_published = read_relation_state(
                session, "cache_overlapping_transition_rows");
            static_cast<void>(session.execute(
                "SET bioseq_cache.query_cache.mode = 'exact_query'"));
            during_transition = single_value(session.execute(
                "SELECT array_agg(value ORDER BY id) "
                "FROM cache_overlapping_transition_rows"));
            const auto interim = session.execute(
                "SELECT (SELECT count(*) "
                "        FROM bioseq_cache_test.entry_states()), "
                "       produced, completed_hits, fallbacks "
                "FROM bioseq_cache.stats()");
            if (interim.row_count() != 1 || interim.column_count() != 4) {
                throw std::runtime_error(
                    "overlapping-transition interim stats have wrong shape");
            }
            during_entries =
                std::string(interim.text_value(0, 0));
            during_stats.produced =
                std::stoll(std::string(interim.text_value(0, 1)));
            during_stats.completed_hits =
                std::stoll(std::string(interim.text_value(0, 2)));
            during_stats.fallbacks =
                std::stoll(std::string(interim.text_value(0, 3)));

            static_cast<void>(session.execute(
                "SELECT bioseq_cache_test.release_backend_pause("
                "'after_transition_publish', "
                + std::to_string(pid_b) + ")"));
            wait_for_flag(
                writer_b_done, session,
                "second overlapping transition did not commit");
            finished = read_relation_state(
                session, "cache_overlapping_transition_rows");
            after_transitions = single_value(session.execute(
                "SELECT array_agg(value ORDER BY id) "
                "FROM cache_overlapping_transition_rows"));
            warm_hit = single_value(session.execute(
                "SELECT array_agg(value ORDER BY id) "
                "FROM cache_overlapping_transition_rows"));
            const auto final = session.execute(
                "SELECT produced, completed_hits, fallbacks "
                "FROM bioseq_cache.stats()");
            if (final.row_count() != 1 || final.column_count() != 3) {
                throw std::runtime_error(
                    "overlapping-transition final stats have wrong shape");
            }
            final_stats.produced =
                std::stoll(std::string(final.text_value(0, 0)));
            final_stats.completed_hits =
                std::stoll(std::string(final.text_value(0, 1)));
            final_stats.fallbacks =
                std::stoll(std::string(final.text_value(0, 2)));
        }});
    const auto writer_body =
        [&pauses_armed](
            int row_id,
            std::string_view value,
            std::atomic<int>& pid,
            std::atomic<bool>& done,
            bioseq::postgres_test::bounded_session_context& session) {
            const auto pid_result =
                session.execute("SELECT pg_backend_pid()");
            pid.store(
                std::stoi(single_value(pid_result)),
                std::memory_order_release);
            wait_for_flag(
                pauses_armed, session,
                "backend-targeted transition pauses were not armed");
            static_cast<void>(session.execute(
                "BEGIN", bioseq::libpq::transaction_policy::allow_open));
            static_cast<void>(session.execute(
                "UPDATE cache_overlapping_transition_rows SET value = "
                    + session.escape_literal(value)
                    + " WHERE id = " + std::to_string(row_id),
                bioseq::libpq::transaction_policy::allow_open));
            static_cast<void>(session.execute(
                "COMMIT",
                bioseq::libpq::transaction_policy::require_idle));
            done.store(true, std::memory_order_release);
        };
    tasks.push_back({
        .name = "writer_a",
        .body = [&writer_a_pid, &writer_a_done, &writer_body](
                    bioseq::postgres_test::bounded_session_context& session) {
            writer_body(
                1, "a", writer_a_pid, writer_a_done, session);
        }});
    tasks.push_back({
        .name = "writer_b",
        .body = [&writer_b_pid, &writer_b_done, &writer_body](
                    bioseq::postgres_test::bounded_session_context& session) {
            writer_body(
                2, "b", writer_b_pid, writer_b_done, session);
        }});

    const auto outcomes =
        bioseq::postgres_test::run_independent_sessions(
            fixture.cluster(), tasks, 60s);
    require_outcomes(outcomes);
    CHECK(both_published.transitions == 2);
    CHECK(both_published.epoch == baseline.epoch);
    CHECK(one_published.transitions == 1);
    CHECK(one_published.epoch == baseline.epoch + 1);
    CHECK(during_transition == "{a,two}");
    CHECK(during_entries == "0");
    CHECK(during_stats.produced == 0);
    CHECK(during_stats.completed_hits == 0);
    CHECK(during_stats.fallbacks == 1);
    CHECK(finished.transitions == 0);
    CHECK(finished.epoch == baseline.epoch + 2);
    CHECK(after_transitions == "{a,b}");
    CHECK(warm_hit == "{a,b}");
    CHECK(final_stats.produced == 1);
    CHECK(final_stats.completed_hits == 1);
    CHECK(final_stats.fallbacks == 1);

}
