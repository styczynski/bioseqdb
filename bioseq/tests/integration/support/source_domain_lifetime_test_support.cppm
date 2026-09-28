/**
 * @file
 * @brief Helpers for testing sequence-source cleanup across SQL operations.
 */
module;
#include <libpq-fe.h>

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

export module bioseq.tests.integration.source_domain_lifetime_test_support;

import std;
import bioseq.tests.integration.support;

export namespace bioseq_int_tests::source_domain_lifetime_test_support {

/** Read a sequence-source lifetime counter from the current backend. */
[[nodiscard]] std::int64_t source_counter(
        connection_test_context& ctx, std::string_view name) {
    return query_scalar_int(ctx, fmt::format(
        "SELECT (bioseq_internal.sequence_source_counters()"
        "        ->>'{}')::bigint;", name));
}

/** Reset sequence-source counters before a lifetime test. */
void reset_source_counters(connection_test_context& ctx) {
    exec_sql(ctx,
        "SELECT bioseq_internal.reset_sequence_source_counters();");
}

/** Require all tracked sequence-source resources to have been released. */
void require_source_quiescent(connection_test_context& ctx) {
    REQUIRE(source_counter(ctx, "live_registries") == 0);
    REQUIRE(source_counter(ctx, "live_providers") == 0);
    REQUIRE(source_counter(ctx, "live_domains") == 0);
    REQUIRE(source_counter(ctx, "live_leases") == 0);
    REQUIRE(source_counter(ctx, "live_cache_sets") == 0);
    REQUIRE(source_counter(ctx, "live_snapshots") == 0);
}

/** Require matching creation and reease counts, with no live source resources. */
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

/** Generate repeatable DNA4 test data with a fixed random seed. */
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

/** Read Actual Calls for  the EXPLAIN section with this mechanism and reason */
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

/** Locate the first plan line containing the requested marker. */
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

/** Count plan lines containing the requested marker. */
[[nodiscard]] std::size_t plan_line_count(
        std::span<const std::string> plan,
        std::string_view needle) {
    std::size_t count = 0u;
    for (const auto& line : plan) {
        if (line.find(needle) != std::string::npos) ++count;
    }
    return count;
}

/** COPY OUT data and flags showing whether the stream and command completed. */
struct copy_out_result {
    bool started = false;
    bool stream_ok = true;
    bool command_ok = true;
    std::string bytes;
};

/** Execute COPY OUT and drain both its byte stream and terminal result. */
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

/** Progress and SQLSTATE from an asynchronous cancellation test. */
struct async_cancel_result {
    bool sent = false;
    bool wait_observed = false;
    bool cancel_requested = false;
    bool drained = false;
    std::string sqlstate;
};

/** Try to roll back an armed prepared transaction when this object leaves scope. */
class prepared_transaction_cleanup_guard {
private:
    PGconn* connection_ = nullptr;
    std::string rollback_sql_;
    bool armed_ = false;

public:
    /** Borrow the connection and remember the prepared transaction to clean up. */
    prepared_transaction_cleanup_guard(
            PGconn* connection, std::string_view gid)
        : connection_(connection),
          rollback_sql_(fmt::format("ROLLBACK PREPARED '{}';", gid)) {}

    prepared_transaction_cleanup_guard(
        const prepared_transaction_cleanup_guard&) = delete;
    prepared_transaction_cleanup_guard& operator=(
        const prepared_transaction_cleanup_guard&) = delete;

    /** Try to roll back the prepared transaction if cleanup is still armed. */
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

    /** Enable rollback cleanup after PREPARE TRANSACTION succeeds. */
    void arm() noexcept { armed_ = true; }

    /** Disarm cleanup after an explicit rollback completes. */
    void release() noexcept { armed_ = false; }
};

/** Start a query, wait, for PgSleep, request cancellation, and  try to drain its result. */
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

}  // namespace bioseq_int_tests::source_domain_lifetime_test_support
