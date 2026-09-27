/**
 * @file
 * @brief Shared fixtures and plan checks for Bioseq joins.
 */
module;
#include <libpq-fe.h>
#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

export module bioseq.tests.integration.join_test_support;

import std;
import bioseq.tests.integration.support;

export namespace bioseq_int_tests::join_test_support {

/** Table used by single-relation join fixtures. */
inline constexpr std::string_view k_hoe_join = "hoe_join";
/** Left table used by cross-relation join fixtures. */
inline constexpr std::string_view k_hoe_join_left = "hoe_join_left";
/** Right table used by cross-relation join fixtures. */
inline constexpr std::string_view k_hoe_join_right = "hoe_join_right";
/** Target table for joins whose sequence operands are expressions. */
inline constexpr std::string_view k_transformed_join_target = "transformed_join_target";
/** Query table for joins whose sequence operands are expressions. */
inline constexpr std::string_view k_transformed_join_queries = "transformed_join_queries";

/** Select scalar alignment algorithms for joins with transformed sequences. */
void configure_transformed_join(connection_test_context& ctx) {
    exec_sql(ctx,
             "SET max_parallel_workers_per_gather = 0;"
             "SET bioseq.index.sequence_read.enabled = off;"
             "SET bioseq.aligner.vulkan.enabled = off;"
             "SET bioseq.aligner.minimizer.algorithm = 'scalar';"
             "SET bioseq.aligner.chaining.algorithm = 'scalar_minimap2';"
             "SET bioseq.aligner.base_alignment.algorithm = 'ksw2';"
             "SET bioseq.aligner.chaining.minimum_score = 20;");
}

/** Create alignment targets and a query table containing a NULL operand. */
void seed_transformed_join_fixture(connection_test_context& ctx) {
    seed_hoe_fixture(ctx, k_transformed_join_target);
    exec_sql(ctx,
             fmt::format("CREATE TEMP TABLE {} ("
                         "id int PRIMARY KEY, target_pk int NOT NULL, "
                         "seq bioseq.seq('dna4',32));"
                         "INSERT INTO {} VALUES "
                         "(1, 1, {}), "
                         "(2, 1, NULL::bioseq.seq('dna4',32)), "
                         "(3, 1, {});",
                         k_transformed_join_queries,
                         k_transformed_join_queries,
                         hoe_probe_literal(),
                         hoe_probe_literal()));
    configure_transformed_join(ctx);
}

/** Check whether a standard plan filter or index condition still calls mmsearch. */
[[nodiscard]] bool plan_has_executable_mmsearch(const std::vector<std::string>& plan) {
    constexpr std::array<std::string_view, 5u> executable_prefixes{"Filter: mmsearch",
                                                                   "Join Filter: mmsearch",
                                                                   "One-Time Filter: mmsearch",
                                                                   "Index Cond: mmsearch",
                                                                   "Recheck Cond: mmsearch"};
    for (const auto& line : plan) {
        for (const auto prefix : executable_prefixes) {
            if (line.find(prefix) != std::string::npos)
                return true;
        }
    }
    return false;
}

/** Check that mmsearch has no duplicate owners or leftover executable conditions. */
void require_owned_qual_audit(const std::vector<std::string>& plan) {
    constexpr std::array<std::string_view, 5u> properties{
        "Bioseq Owned RestrictInfo Path Copies",
        "Bioseq Executable Mmsearch Qual Copies",
        "Bioseq Vector Mmsearch Qual Copies",
        "Bioseq Storage Mmsearch Qual Copies",
        "Bioseq Descendant Mmsearch Owner Copies"};
    for (const auto property : properties) {
        const auto value = find_explain_property(plan, property);
        INFO(property);
        REQUIRE(value.has_value());
        REQUIRE(*value == "0");
    }
    REQUIRE(plan_contains(plan, "Bioseq Residual Mmsearch Qual: absent"));
    REQUIRE_FALSE(plan_has_executable_mmsearch(plan));
}

/** Results and lock-wait observations from a concurrent update. */
struct async_update_outcome {
    /** Whether libpq accepted the asynchronous statement. */
    bool sent = false;
    /** Whether the worker reached a PostgreSQL lock wait. */
    bool lock_wait_observed = false;
    /** Whether libpq input consumption remained successful. */
    bool consume_ok = true;
    /** Whether the worker returned a command result. */
    bool got_command = false;
    /** Whether the worker returned a tuple result. */
    bool got_tuple_result = false;
    /** Whether every returned result was successful. */
    bool result_ok = true;
    /** Total rows returned by tuple results. */
    std::size_t returned_rows = 0u;
    /** Combined libpq diagnostics for failed results. */
    std::string error;
};

/** Hold a row lock, start a competing update, then release the lock and collect its result. */
[[nodiscard]] async_update_outcome run_locked_update(connection_test_context& blocker,
                                                     connection_test_context& worker,
                                                     connection_test_context& observer,
                                                     const std::string& blocker_update,
                                                     const std::string& worker_update) {
    async_update_outcome outcome{};
    exec_sql(blocker, "BEGIN;");
    exec_sql(blocker, blocker_update);
    const long long worker_pid = query_scalar_int(worker, "SELECT pg_backend_pid();");
    outcome.sent = PQsendQuery(worker.raw(), worker_update.c_str()) == 1;
    if (outcome.sent) {
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            const long long waiting =
                query_scalar_int(observer,
                                 fmt::format("SELECT count(*) FROM pg_stat_activity "
                                             "WHERE pid={} AND wait_event_type='Lock';",
                                             worker_pid));
            if (waiting == 1) {
                outcome.lock_wait_observed = true;
                break;
            }
            if (PQconsumeInput(worker.raw()) == 0) {
                outcome.consume_ok = false;
                break;
            }
            if (PQisBusy(worker.raw()) == 0)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    /* Release the row lock before reading the worker result so PostgreSQL can
     * recheck the updated row and finish the waiting statement. */
    exec_sql(blocker, "COMMIT;");
    if (!outcome.sent)
        return outcome;
    for (unsigned attempt = 0; attempt < 1000 && PQisBusy(worker.raw()) != 0; ++attempt) {
        if (PQconsumeInput(worker.raw()) == 0) {
            outcome.consume_ok = false;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (PQisBusy(worker.raw()) != 0)
        return outcome;
    while (PGresult* result = PQgetResult(worker.raw())) {
        const ExecStatusType status = PQresultStatus(result);
        if (status == PGRES_COMMAND_OK) {
            outcome.got_command = true;
        } else if (status == PGRES_TUPLES_OK) {
            outcome.got_tuple_result = true;
            outcome.returned_rows += static_cast<std::size_t>(PQntuples(result));
        } else {
            outcome.result_ok = false;
            const char* message = PQresultErrorMessage(result);
            if (message != nullptr)
                outcome.error += message;
        }
        PQclear(result);
    }
    return outcome;
}

} // namespace bioseq_int_tests::join_test_support
