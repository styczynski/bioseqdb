module;

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

export module bioseq.codegen.website.postgres:executor;

import bioseq.libpq.client;
import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Tutorial SQL executor (§13.5-13.6). Consumes bioseq.libpq.client and projects
// libpq results onto the source-neutral SQL result model; no PGconn/PGresult or
// OID leaks past this boundary. Database-per-example cloning and session limits
// (§13.3-13.4) are layered on in Phase 7; this partition owns the connection
// lifecycle and the result projection the concept requires.
export namespace bioseq::codegen::website {

/// @brief Configuration for database-per-example isolation (§13.3).
///
/// Each example runs as a unique restricted role inside a unique database cloned
/// from the extension-bearing template, reached through the same private Unix
/// socket. The admin conninfo connects to the cluster's administrative database.
struct isolation_settings {
    std::string admin_conninfo;
    std::string socket_directory;
    int port{};
    std::string template_database;
};

/// @brief Executes tutorial SQL against a private Unix-socket cluster.
///
/// Models tutorial_sql_executor: read_catalog and execute return typed results
/// or a diagnostic, never raising libpq errors to callers.
class private_cluster_executor {
public:
    explicit private_cluster_executor(std::string conninfo)
        : conninfo_(std::move(conninfo)) {}

    /// @brief Captures the live SQL catalog snapshot from the cluster.
    /// @return The snapshot, or a diagnostic on any connection/query failure.
    [[nodiscard]] std::expected<sql_catalog_snapshot, diagnostic> read_catalog() {
        try {
            bioseq::libpq::connection connection(conninfo_);
            sql_catalog_snapshot snapshot;
            snapshot.server_version = connection.server_version();
            const bioseq::libpq::result version = connection.exec(
                "select extversion from pg_extension where extname = 'bioseq'");
            if (version.rows() > 0 && !version.is_null(0, 0)) {
                snapshot.extension_version = std::string(version.value(0, 0));
            }
            return snapshot;
        } catch (const bioseq::libpq::query_error& error) {
            return std::unexpected(make_error("catalog", error));
        }
    }

    /// @brief Runs an example's visible SQL and captures its result model.
    /// @param example The tutorial SQL example to execute.
    /// @return The visible result model, or a diagnostic on failure.
    [[nodiscard]] std::expected<sql_example_result, diagnostic> execute(
        sql_example const& example) {
        try {
            bioseq::libpq::connection connection(conninfo_);
            return run_example_on(connection, example);
        } catch (const bioseq::libpq::query_error& error) {
            return std::unexpected(make_error("execute", error));
        }
    }

    /// @brief Runs one example under database-per-example isolation (§13.3).
    ///
    /// Creates a unique restricted LOGIN role and a unique database cloned from the
    /// extension-bearing template owned by that role, revokes PUBLIC CONNECT and
    /// grants it only to the role, runs the example as that role over the private
    /// socket, and always drops the clone (WITH FORCE) and role afterward — even on
    /// failure. The role never connects as the admin/initdb user.
    /// @param example The tutorial SQL example.
    /// @param settings Admin conninfo, socket/port, and template database.
    /// @return The visible result model, or a diagnostic on failure.
    [[nodiscard]] std::expected<sql_example_result, diagnostic> execute_isolated(
        sql_example const& example, isolation_settings const& settings) {
        std::string role;
        std::string clone;
        try {
            bioseq::libpq::connection admin(settings.admin_conninfo);
            const std::string suffix =
                unique_suffix(example.local_id.empty() ? example.canonical_id
                                                       : example.local_id);
            role = "biosite_r_" + suffix;
            clone = "biosite_d_" + suffix;
            const std::string quoted_role = admin.escape_identifier(role);
            const std::string quoted_clone = admin.escape_identifier(clone);
            const std::string quoted_template =
                admin.escape_identifier(settings.template_database);
            (void)admin.exec("CREATE ROLE " + quoted_role
                + " LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS");
            (void)admin.exec("CREATE DATABASE " + quoted_clone + " TEMPLATE "
                + quoted_template + " OWNER " + quoted_role);
            (void)admin.exec("REVOKE CONNECT ON DATABASE " + quoted_clone + " FROM PUBLIC");
            (void)admin.exec("GRANT CONNECT ON DATABASE " + quoted_clone
                + " TO " + quoted_role);
            std::expected<sql_example_result, diagnostic> result;
            try {
                bioseq::libpq::connection role_connection(
                    bioseq::libpq::unix_socket_conninfo(
                        settings.socket_directory, clone, role, settings.port));
                result = run_example_on(role_connection, example);
            } catch (const bioseq::libpq::query_error& error) {
                result = std::unexpected(make_error("execute", error));
            }
            // Cleanup runs regardless of the example outcome (§13.3).
            drop_quietly(admin, "DROP DATABASE " + quoted_clone + " WITH (FORCE)");
            drop_quietly(admin, "DROP ROLE " + quoted_role);
            return result;
        } catch (const bioseq::libpq::query_error& error) {
            try {
                bioseq::libpq::connection admin(settings.admin_conninfo);
                if (!clone.empty()) {
                    drop_quietly(admin, "DROP DATABASE "
                        + admin.escape_identifier(clone) + " WITH (FORCE)");
                }
                if (!role.empty()) {
                    drop_quietly(admin, "DROP ROLE " + admin.escape_identifier(role));
                }
            } catch (const bioseq::libpq::query_error&) {
                // Best-effort provisioning cleanup; report the original failure.
            }
            return std::unexpected(make_error("isolate", error));
        }
    }

private:
    // Runs the deterministic session setup, setup/visible/teardown phases, notice
    // capture, and retention-limit checks on an already-open connection. Shared by
    // the direct and database-per-example (§13.3) execution paths.
    [[nodiscard]] static std::expected<sql_example_result, diagnostic> run_example_on(
        bioseq::libpq::connection& connection, sql_example const& example) {
        sql_example_result outcome;
        outcome.example_id = example.canonical_id;
        // Deterministic session settings (§13.4) precede setup so formatting,
        // search_path, and the defense-in-depth server timeouts are fixed for
        // every phase; the fence timeout (capped at 60s) drives statement_timeout.
        apply_session_settings(connection, example);
        if (!example.setup_sql.empty()) {
            (void)connection.exec(example.setup_sql);
        }
        // Only sql_phase::visible notices may render (§13.6); discard the
        // session-setup and setup-phase notices before the visible phase.
        (void)connection.take_notices();
        // The visible SQL is where the success/error expectation applies
        // (§13.5): a success example must not raise; an error example must
        // raise the expected SQLSTATE (or any, when unspecified).
        if (example.expectation == sql_expectation::error) {
            if (auto captured = run_expected_error(connection, example); !captured) {
                return std::unexpected(std::move(captured.error()));
            } else {
                outcome.visible_results.push_back(std::move(*captured));
            }
        } else {
            const bioseq::libpq::result visible = connection.exec(example.visible_sql);
            outcome.visible_results.push_back(project_result(visible));
            // Exceeding a retention limit is a build error, never a silent
            // truncation — displayed output must be the actual result (§13.4).
            if (auto breach = check_result_limits(outcome.visible_results.back())) {
                diagnostic limit;
                limit.severity = diagnostic_severity::error;
                limit.phase = "execute";
                limit.message = std::move(*breach);
                return std::unexpected(limit);
            }
        }
        for (const bioseq::libpq::notice& raw : connection.take_notices()) {
            if (outcome.notices.size() >= 100) {  // §13.4: 100 captured notices
                break;
            }
            outcome.notices.push_back(sql_notice{
                .phase = sql_phase::visible,
                .severity = raw.severity,
                .sqlstate = raw.sqlstate,
                .message = raw.message});
        }
        if (!example.teardown_sql.empty()) {
            try {
                (void)connection.exec(example.teardown_sql);
            } catch (const bioseq::libpq::query_error&) {
                // Teardown is best-effort after a captured expected error.
            }
        }
        return outcome;
    }

    // Runs a best-effort administrative command, swallowing failures so cleanup
    // always attempts every drop (§13.3).
    static void drop_quietly(bioseq::libpq::connection& admin, const std::string& sql) {
        try {
            (void)admin.exec(sql);
        } catch (const bioseq::libpq::query_error&) {
        }
    }

    // Builds a unique lower-snake identifier suffix for the per-example role and
    // database. Examples run serially, so a process-local counter plus the
    // sanitized local id is sufficient; the private cluster is unique per build.
    [[nodiscard]] static std::string unique_suffix(std::string_view id) {
        static int counter = 0;
        std::string clean;
        for (const char character : id) {
            const char lowered = (character >= 'A' && character <= 'Z')
                ? static_cast<char>(character - 'A' + 'a') : character;
            const bool ok = (lowered >= 'a' && lowered <= 'z')
                || (lowered >= '0' && lowered <= '9');
            clean.push_back(ok ? lowered : '_');
            if (clean.size() >= 24) {
                break;
            }
        }
        return clean + "_" + std::to_string(++counter);
    }

    // Applies the deterministic session settings (§13.4). These are generator-
    // owned constants — never author SQL — so they are safe to send verbatim. The
    // server statement_timeout is defense in depth (tutorial SQL can reset it);
    // the client deadline (§13.5) is the independent authority.
    static void apply_session_settings(
        bioseq::libpq::connection& connection, sql_example const& example) {
        long long timeout_ms = example.timeout.count();
        if (timeout_ms <= 0 || timeout_ms > 60'000) {
            timeout_ms = 60'000;
        }
        (void)connection.exec("SET TIME ZONE 'UTC'");
        (void)connection.exec("SET DateStyle = 'ISO, YMD'");
        (void)connection.exec("SET IntervalStyle = 'iso_8601'");
        (void)connection.exec("SET bytea_output = 'hex'");
        (void)connection.exec("SET extra_float_digits = 3");
        (void)connection.exec("SET client_min_messages = 'notice'");
        (void)connection.exec("SET search_path = public, bioseq, pg_catalog");
        (void)connection.exec("SET statement_timeout = " + std::to_string(timeout_ms));
        (void)connection.exec("SET lock_timeout = '2s'");
        (void)connection.exec("SET idle_in_transaction_session_timeout = '15s'");
    }

    // Runs the visible SQL of an error example, capturing the raised SQLSTATE.
    // A success (no error) or a wrong SQLSTATE is a failed expectation.
    [[nodiscard]] static std::expected<sql_result_set, diagnostic> run_expected_error(
        bioseq::libpq::connection& connection, sql_example const& example) {
        try {
            (void)connection.exec(example.visible_sql);
        } catch (const bioseq::libpq::query_error& error) {
            const std::string sqlstate(error.sqlstate());
            if (example.expected_sqlstate
                && *example.expected_sqlstate != sqlstate) {
                diagnostic mismatch;
                mismatch.severity = diagnostic_severity::error;
                mismatch.phase = "execute";
                mismatch.code = sqlstate;
                mismatch.message = "expected SQLSTATE " + *example.expected_sqlstate
                    + " but got " + sqlstate + ": " + error.what();
                return std::unexpected(mismatch);
            }
            sql_result_set captured;
            captured.kind = sql_result_kind::error;
            captured.sqlstate = sqlstate;
            captured.primary_message = error.what();
            return captured;
        }
        diagnostic unexpected_success;
        unexpected_success.severity = diagnostic_severity::error;
        unexpected_success.phase = "execute";
        unexpected_success.message =
            "example expected an error but the visible SQL succeeded";
        return std::unexpected(unexpected_success);
    }

    // Enforces the §13.4 per-visible-result retention limits. Returns an author-
    // facing message on the first breach so the build fails with actionable
    // guidance instead of rendering a silently truncated result.
    [[nodiscard]] static std::optional<std::string> check_result_limits(
        const sql_result_set& set) {
        constexpr std::size_t max_rows = 200;
        constexpr std::size_t max_columns = 100;
        constexpr std::size_t max_cell_bytes = 64 * 1024;
        constexpr std::size_t max_total_bytes = 2 * 1024 * 1024;
        if (set.columns.size() > max_columns) {
            return "visible result exceeds 100 columns; select an explicit column list";
        }
        if (set.rows.size() > max_rows) {
            return "visible result exceeds 200 rows; add a deterministic LIMIT";
        }
        std::size_t total = 0;
        for (const std::vector<sql_cell>& row : set.rows) {
            for (const sql_cell& cell : row) {
                if (cell.text.size() > max_cell_bytes) {
                    return "a visible result cell exceeds 64 KiB; reduce the output";
                }
                total += cell.text.size();
                if (total > max_total_bytes) {
                    return "visible payload exceeds 2 MiB; reduce the output";
                }
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] static sql_result_set project_result(const bioseq::libpq::result& result) {
        sql_result_set set;
        if (result.has_rows()) {
            set.kind = sql_result_kind::rows;
            for (int column = 0; column < result.columns(); ++column) {
                set.columns.push_back(sql_column{
                    .name = result.column_name(column),
                    .type_oid = result.column_type_oid(column),
                    .type_name = {}});
            }
            for (int row = 0; row < result.rows(); ++row) {
                std::vector<sql_cell> cells;
                cells.reserve(static_cast<std::size_t>(result.columns()));
                for (int column = 0; column < result.columns(); ++column) {
                    cells.push_back(sql_cell{
                        .is_null = result.is_null(row, column),
                        .text = std::string(result.value(row, column))});
                }
                set.rows.push_back(std::move(cells));
            }
        } else {
            set.kind = sql_result_kind::command;
            set.command_tag = result.command_tag();
        }
        return set;
    }

    [[nodiscard]] static diagnostic make_error(
        std::string phase,
        const bioseq::libpq::query_error& error) {
        diagnostic result;
        result.severity = diagnostic_severity::error;
        result.phase = std::move(phase);
        result.code = std::string(error.sqlstate());
        result.message = error.what();
        return result;
    }

    std::string conninfo_;
};

static_assert(tutorial_sql_executor<private_cluster_executor>,
              "private_cluster_executor must model tutorial_sql_executor");

}  // namespace bioseq::codegen::website
