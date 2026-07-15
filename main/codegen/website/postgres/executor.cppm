module;

#include <cstddef>
#include <expected>
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
            sql_example_result outcome;
            outcome.example_id = example.canonical_id;
            if (!example.setup_sql.empty()) {
                (void)connection.exec(example.setup_sql);
            }
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
                const bioseq::libpq::result visible =
                    connection.exec(example.visible_sql);
                outcome.visible_results.push_back(project_result(visible));
            }
            if (!example.teardown_sql.empty()) {
                try {
                    (void)connection.exec(example.teardown_sql);
                } catch (const bioseq::libpq::query_error&) {
                    // Teardown is best-effort after a captured expected error.
                }
            }
            for (const bioseq::libpq::notice& raw : connection.take_notices()) {
                outcome.notices.push_back(sql_notice{
                    .phase = sql_phase::visible,
                    .severity = raw.severity,
                    .sqlstate = raw.sqlstate,
                    .message = raw.message});
            }
            return outcome;
        } catch (const bioseq::libpq::query_error& error) {
            return std::unexpected(make_error("execute", error));
        }
    }

private:
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
