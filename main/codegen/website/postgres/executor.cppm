module;

#include <chrono>
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
        std::string peer_role;
        std::expected<sql_example_result, diagnostic> result =
            std::unexpected(diagnostic{});
        try {
            bioseq::libpq::connection admin(settings.admin_conninfo);
            const std::string suffix =
                unique_suffix(example.local_id.empty() ? example.canonical_id
                                                       : example.local_id);
            role = "biosite_r_" + suffix;
            clone = "biosite_d_" + suffix;
            peer_role = "biosite_p_" + suffix;
            const std::string quoted_role = admin.escape_identifier(role);
            const std::string quoted_clone = admin.escape_identifier(clone);
            const std::string quoted_peer = admin.escape_identifier(peer_role);
            const std::string quoted_template =
                admin.escape_identifier(settings.template_database);
            // §13.3 hardening: the extension-bearing template becomes an
            // unconnectable IS_TEMPLATE database with no PUBLIC access, and the
            // administrative database also rejects PUBLIC CONNECT, so a restricted
            // role can reach only its own clone. Idempotent across serial examples.
            harden_template(admin, quoted_template);
            (void)admin.exec("REVOKE CONNECT ON DATABASE postgres FROM PUBLIC");
            (void)admin.exec("CREATE ROLE " + quoted_role
                + " LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS");
            (void)admin.exec("CREATE DATABASE " + quoted_clone + " TEMPLATE "
                + quoted_template + " OWNER " + quoted_role);
            (void)admin.exec("REVOKE CONNECT ON DATABASE " + quoted_clone + " FROM PUBLIC");
            (void)admin.exec("GRANT CONNECT ON DATABASE " + quoted_clone
                + " TO " + quoted_role);
            // A second restricted LOGIN role that owns nothing, standing in for
            // another example's role: the clone must reject it (§13.3), proving
            // this role could not reach another example's clone either.
            (void)admin.exec("CREATE ROLE " + quoted_peer
                + " LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS");
            // §13.3 clone-ACL verification and negative connection assertions run
            // before the example; a violated security guarantee fails the example.
            if (auto violation = verify_clone_acl(admin, role, clone)) {
                result = std::unexpected(std::move(*violation));
            } else if (auto violation =
                           assert_no_foreign_connect(settings, role, clone, peer_role)) {
                result = std::unexpected(std::move(*violation));
            } else {
                std::optional<bioseq::libpq::connection> role_connection;
                try {
                    role_connection.emplace(bioseq::libpq::unix_socket_conninfo(
                        settings.socket_directory, clone, role, settings.port));
                    if (auto violation =
                            assert_no_privilege_escalation(*role_connection)) {
                        result = std::unexpected(std::move(*violation));
                    } else {
                        result = run_example_on(*role_connection, example,
                                                /*run_teardown=*/false);
                    }
                } catch (const bioseq::libpq::query_error& error) {
                    result = std::unexpected(make_error("execute", error));
                }
                // Teardown always runs (§13.5): on the live idle connection, or
                // via the clone/role recovery path when the visible phase left it
                // closed, timed out, or non-idle.
                teardown_isolated(settings, role, clone, example,
                                  role_connection ? &*role_connection : nullptr);
            }
        } catch (const bioseq::libpq::query_error& error) {
            result = std::unexpected(make_error("isolate", error));
        }
        // Cleanup always runs on a FRESH bounded admin connection so a dead or
        // timed-out provisioning connection can never leak the clone/role (§13.3,
        // §13.5). The drops are idempotent (IF EXISTS) and best-effort.
        cleanup_isolated(settings.admin_conninfo, role, clone, peer_role);
        return result;
    }

private:
    // Runs the deterministic session setup, setup/visible phases, and notice
    // capture on an already-open connection. Shared by the direct and
    // database-per-example (§13.3) execution paths. `run_teardown` runs the
    // teardown phase inline for the direct path; the isolated path passes false
    // and owns teardown so it can add the §13.5 clone/role recovery.
    [[nodiscard]] static std::expected<sql_example_result, diagnostic> run_example_on(
        bioseq::libpq::connection& connection, sql_example const& example,
        bool run_teardown = true) {
        sql_example_result outcome;
        outcome.example_id = example.canonical_id;
        // Every phase shares one client deadline — the validated fence timeout
        // capped at 60s (§13.5) — independent of the server statement_timeout the
        // deterministic session settings also install.
        const std::chrono::milliseconds fence = fence_deadline(example);
        // Deterministic session settings (§13.4) precede setup so formatting,
        // search_path, and the defense-in-depth server timeouts are fixed for
        // every phase; the fence timeout (capped at 60s) drives statement_timeout.
        apply_session_settings(connection, example);
        if (!example.setup_sql.empty()) {
            (void)connection.exec(example.setup_sql, fence);
        }
        // Only sql_phase::visible notices may render (§13.6); discard the
        // session-setup and setup-phase notices before the visible phase.
        (void)connection.take_notices();
        // The visible SQL is where the success/error expectation applies
        // (§13.5): a success example must not raise; an error example must
        // raise the expected SQLSTATE (or any, when unspecified). Either way
        // every result set the phase produced is captured in execution order
        // (§13.4, §13.6) — a multi-statement phase yields more than one.
        if (example.expectation == sql_expectation::error) {
            if (auto failure = run_expected_error(connection, example, fence, outcome)) {
                return std::unexpected(std::move(*failure));
            }
        } else {
            // The retention limits bound chunked accumulation as it streams; a
            // breach is still reported by check_visible_limits below with its
            // author-facing message rather than a silent truncation.
            const bioseq::libpq::multi_result visible =
                connection.exec_bounded_multi(example.visible_sql, fence, website_limits());
            if (visible.error) {
                // A success example must not raise a server error (§10.7).
                return std::unexpected(make_error("execute", *visible.error));
            }
            for (const bioseq::libpq::result& set : visible.results) {
                outcome.visible_results.push_back(project_result(set));
            }
            // Exceeding a retention limit is a build error, never a silent
            // truncation — displayed output must be the actual result (§13.4).
            if (auto breach = check_visible_limits(outcome.visible_results)) {
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
        if (run_teardown && !example.teardown_sql.empty()) {
            try {
                (void)connection.exec(example.teardown_sql, fence);
            } catch (const bioseq::libpq::query_error&) {
                // Teardown is best-effort after a captured expected error.
            }
        }
        return outcome;
    }

    // The per-phase client deadline: the validated fence timeout capped at 60s,
    // independent of the server statement_timeout (§13.5).
    [[nodiscard]] static std::chrono::milliseconds fence_deadline(
        const sql_example& example) {
        long long timeout_ms = example.timeout.count();
        if (timeout_ms <= 0 || timeout_ms > 60'000) {
            timeout_ms = 60'000;
        }
        return std::chrono::milliseconds(timeout_ms);
    }

    // The §13.4 visible-result retention limits, handed to the nonblocking state
    // machine so chunked accumulation stays bounded; check_visible_limits owns the
    // matching author-facing messages.
    [[nodiscard]] static bioseq::libpq::execution_limits website_limits() {
        return bioseq::libpq::execution_limits{
            .max_rows = 200,
            .max_columns = 100,
            .max_cell_bytes = 64 * 1024,
            .max_total_bytes = 2 * 1024 * 1024,
            .max_result_sets = 10};
    }

    // Runs a best-effort administrative command under the fixed five-second
    // cleanup deadline (§13.5), swallowing failures so cleanup always attempts
    // every drop (§13.3).
    static void drop_quietly(bioseq::libpq::connection& admin, const std::string& sql) {
        try {
            (void)admin.exec(sql, std::chrono::seconds(5));
        } catch (const bioseq::libpq::query_error&) {
        }
    }

    // Drops the per-example clone, role, and negative-assertion peer role over a
    // FRESH bounded admin connection (§13.5): the provisioning connection may have
    // died or timed out, so cleanup must not depend on it. Every drop is
    // idempotent (IF EXISTS) and best-effort; the clone is dropped first (WITH
    // FORCE) so the owning role has no dependent objects left when it is dropped.
    static void cleanup_isolated(const std::string& admin_conninfo,
                                 const std::string& role, const std::string& clone,
                                 const std::string& peer_role) {
        if (role.empty() && clone.empty() && peer_role.empty()) {
            return;
        }
        try {
            bioseq::libpq::connection admin(admin_conninfo);
            if (!clone.empty()) {
                drop_quietly(admin, "DROP DATABASE IF EXISTS "
                    + admin.escape_identifier(clone) + " WITH (FORCE)");
            }
            if (!role.empty()) {
                drop_quietly(admin, "DROP ROLE IF EXISTS "
                    + admin.escape_identifier(role));
            }
            if (!peer_role.empty()) {
                drop_quietly(admin, "DROP ROLE IF EXISTS "
                    + admin.escape_identifier(peer_role));
            }
        } catch (const bioseq::libpq::query_error&) {
            // A fresh admin connection could not be opened; the private cluster is
            // torn down at build end regardless, so this is the bounded last try.
        }
    }

    // §13.3 template hardening: marks the extension-bearing template as an
    // IS_TEMPLATE database that rejects new connections and PUBLIC access, so only
    // admin-driven clones can derive from it. Idempotent, so it is safe to repeat
    // before each serial example; a superuser admin still clones it (no connection
    // to the template is needed, exactly like template0).
    static void harden_template(bioseq::libpq::connection& admin,
                                const std::string& quoted_template) {
        (void)admin.exec("REVOKE CONNECT ON DATABASE " + quoted_template + " FROM PUBLIC");
        (void)admin.exec("ALTER DATABASE " + quoted_template + " IS_TEMPLATE true");
        (void)admin.exec("ALTER DATABASE " + quoted_template + " ALLOW_CONNECTIONS false");
    }

    // Builds an isolation-breach diagnostic. A violated §13.3 assertion is a
    // security guarantee, not a warning, so it fails the example outright.
    [[nodiscard]] static diagnostic isolation_violation(std::string message) {
        diagnostic violation;
        violation.severity = diagnostic_severity::error;
        violation.phase = "isolate";
        violation.message = std::move(message);
        return violation;
    }

    // §13.3 step 8: the clone's pg_database.datacl must grant CONNECT to the
    // owning role and to nobody else — in particular PUBLIC must not retain
    // CONNECT. aclexplode with grantee 0 is the PUBLIC pseudo-role.
    [[nodiscard]] static std::optional<diagnostic> verify_clone_acl(
        bioseq::libpq::connection& admin, const std::string& role,
        const std::string& clone) {
        const bioseq::libpq::result acl = admin.exec(
            "SELECT "
            "COALESCE(bool_or(g.grantee = 0 AND g.privilege_type = 'CONNECT'), false), "
            "COALESCE(bool_or(g.grantee = (SELECT oid FROM pg_roles WHERE rolname = "
                + admin.escape_literal(role)
                + ") AND g.privilege_type = 'CONNECT'), false) "
            "FROM pg_database d LEFT JOIN LATERAL aclexplode(d.datacl) g ON true "
            "WHERE d.datname = " + admin.escape_literal(clone));
        const bool public_connect = acl.rows() > 0 && acl.value(0, 0) == "t";
        const bool role_connect = acl.rows() > 0 && acl.value(0, 1) == "t";
        if (public_connect) {
            return isolation_violation("clone database still grants CONNECT to PUBLIC");
        }
        if (!role_connect) {
            return isolation_violation(
                "clone database does not grant CONNECT to the example role");
        }
        return std::nullopt;
    }

    // Attempts a restricted connection over the private socket and reports whether
    // it was accepted; the §13.3 negative assertions require every such attempt to
    // be rejected.
    [[nodiscard]] static bool role_can_connect(isolation_settings const& settings,
                                               const std::string& database,
                                               const std::string& role) {
        try {
            bioseq::libpq::connection probe(bioseq::libpq::unix_socket_conninfo(
                settings.socket_directory, database, role, settings.port));
            return true;
        } catch (const bioseq::libpq::query_error&) {
            return false;
        }
    }

    // §13.3 negative connection assertions: the example role must be rejected by
    // the administrative database and the extension template, and the clone must
    // reject a foreign role — which proves the role could not reach another
    // example's clone. A successful connection is a security breach.
    [[nodiscard]] static std::optional<diagnostic> assert_no_foreign_connect(
        isolation_settings const& settings, const std::string& role,
        const std::string& clone, const std::string& peer_role) {
        if (role_can_connect(settings, "postgres", role)) {
            return isolation_violation(
                "restricted role could connect to the administrative database");
        }
        if (role_can_connect(settings, settings.template_database, role)) {
            return isolation_violation(
                "restricted role could connect to the extension template");
        }
        if (role_can_connect(settings, clone, peer_role)) {
            return isolation_violation(
                "a foreign role could connect to the example clone");
        }
        return std::nullopt;
    }

    // Runs a command as the restricted role and asserts it is rejected. A success
    // is an isolation breach (the role gained a cluster-level privilege it lacks).
    [[nodiscard]] static std::optional<diagnostic> assert_role_denied(
        bioseq::libpq::connection& role_connection, const std::string& sql,
        std::string violation) {
        try {
            (void)role_connection.exec(sql, std::chrono::seconds(10));
        } catch (const bioseq::libpq::query_error&) {
            return std::nullopt;  // rejected as required
        }
        return isolation_violation(std::move(violation));
    }

    // §13.3: tutorial SQL must not be able to CREATE ROLE or CREATE DATABASE.
    // Both run as the restricted role on its own clone and must be denied.
    [[nodiscard]] static std::optional<diagnostic> assert_no_privilege_escalation(
        bioseq::libpq::connection& role_connection) {
        const std::string probe = role_connection.escape_identifier("biosite_probe");
        if (auto violation = assert_role_denied(role_connection,
                "CREATE ROLE " + probe + " LOGIN",
                "restricted role could CREATE ROLE")) {
            return violation;
        }
        if (auto violation = assert_role_denied(role_connection,
                "CREATE DATABASE " + probe,
                "restricted role could CREATE DATABASE")) {
            return violation;
        }
        return std::nullopt;
    }

    // Runs the teardown phase for an isolated example (§13.5). On the happy path
    // the original example connection is still idle and teardown runs on it. If
    // the visible phase left the connection closed, timed out, or non-idle, a
    // fresh admin connection first restores the clone/role to a reconnectable
    // state, then a new role connection reapplies session settings and runs
    // teardown once. Teardown is best-effort — a failure never blocks the
    // subsequent DROP DATABASE/DROP ROLE, and it is skipped entirely when the
    // database is dropped without ever reconnecting.
    static void teardown_isolated(isolation_settings const& settings,
                                  const std::string& role, const std::string& clone,
                                  sql_example const& example,
                                  bioseq::libpq::connection* role_connection) {
        if (example.teardown_sql.empty()) {
            return;
        }
        const std::chrono::milliseconds fence = fence_deadline(example);
        if (role_connection != nullptr && role_connection->is_idle()) {
            try {
                (void)role_connection->exec(example.teardown_sql, fence);
            } catch (const bioseq::libpq::query_error&) {
                // Best-effort; the clone is dropped next regardless.
            }
            return;
        }
        // Recovery: restore the clone/role so the example role can reconnect,
        // then run teardown once on a fresh role connection (§13.5).
        try {
            bioseq::libpq::connection admin(settings.admin_conninfo);
            const std::string quoted_clone = admin.escape_identifier(clone);
            const std::string quoted_role = admin.escape_identifier(role);
            drop_quietly(admin, "ALTER DATABASE " + quoted_clone + " ALLOW_CONNECTIONS true");
            drop_quietly(admin, "ALTER DATABASE " + quoted_clone + " CONNECTION LIMIT -1");
            drop_quietly(admin, "ALTER DATABASE " + quoted_clone + " IS_TEMPLATE false");
            drop_quietly(admin, "ALTER ROLE " + quoted_role + " LOGIN CONNECTION LIMIT -1");
            drop_quietly(admin, "REVOKE ALL ON DATABASE " + quoted_clone + " FROM PUBLIC");
            drop_quietly(admin, "GRANT CONNECT ON DATABASE " + quoted_clone
                + " TO " + quoted_role);
        } catch (const bioseq::libpq::query_error&) {
            return;  // Could not restore; cleanup drops the clone/role next.
        }
        try {
            bioseq::libpq::connection reconnect(bioseq::libpq::unix_socket_conninfo(
                settings.socket_directory, clone, role, settings.port));
            apply_session_settings(reconnect, example);
            (void)reconnect.exec(example.teardown_sql, fence);
        } catch (const bioseq::libpq::query_error&) {
            // Best-effort; the clone is dropped next regardless.
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

    // Runs the visible SQL of an error example. The result sets completed before
    // PostgreSQL stopped are retained in execution order, then the terminating
    // error is rendered as the final result (§10.7, §13.6). A success (no error),
    // a wrong SQLSTATE, or a retention breach is a failed expectation. Returns a
    // diagnostic on failure, or nullopt after appending the results to `outcome`.
    [[nodiscard]] static std::optional<diagnostic> run_expected_error(
        bioseq::libpq::connection& connection, sql_example const& example,
        std::chrono::milliseconds fence, sql_example_result& outcome) {
        const bioseq::libpq::multi_result visible =
            connection.exec_bounded_multi(example.visible_sql, fence, website_limits());
        if (!visible.error) {
            diagnostic unexpected_success;
            unexpected_success.severity = diagnostic_severity::error;
            unexpected_success.phase = "execute";
            unexpected_success.message =
                "example expected an error but the visible SQL succeeded";
            return unexpected_success;
        }
        const std::string sqlstate(visible.error->sqlstate());
        if (example.expected_sqlstate && *example.expected_sqlstate != sqlstate) {
            diagnostic mismatch;
            mismatch.severity = diagnostic_severity::error;
            mismatch.phase = "execute";
            mismatch.code = sqlstate;
            mismatch.message = "expected SQLSTATE " + *example.expected_sqlstate
                + " but got " + sqlstate + ": " + visible.error->what();
            return mismatch;
        }
        // Retain the results that completed before PostgreSQL stopped, in order.
        for (const bioseq::libpq::result& set : visible.results) {
            outcome.visible_results.push_back(project_result(set));
        }
        if (auto breach = check_visible_limits(outcome.visible_results)) {
            diagnostic limit;
            limit.severity = diagnostic_severity::error;
            limit.phase = "execute";
            limit.message = std::move(*breach);
            return limit;
        }
        sql_result_set captured;
        captured.kind = sql_result_kind::error;
        captured.sqlstate = sqlstate;
        captured.primary_message = visible.error->what();
        captured.detail = std::string(visible.error->detail());
        captured.hint = std::string(visible.error->hint());
        outcome.visible_results.push_back(std::move(captured));
        return std::nullopt;
    }

    // Enforces the §13.4 retained/rendered-result limits across a visible phase:
    // at most 10 result sets, 200 rows and 100 columns per set, 64 KiB per cell,
    // and 2 MiB of total payload. Returns an author-facing message on the first
    // breach so the build fails with actionable guidance instead of rendering a
    // silently truncated result.
    [[nodiscard]] static std::optional<std::string> check_visible_limits(
        const std::vector<sql_result_set>& sets) {
        constexpr std::size_t max_result_sets = 10;
        constexpr std::size_t max_rows = 200;
        constexpr std::size_t max_columns = 100;
        constexpr std::size_t max_cell_bytes = 64 * 1024;
        constexpr std::size_t max_total_bytes = 2 * 1024 * 1024;
        if (sets.size() > max_result_sets) {
            return "visible phase produced more than 10 result sets; split the example";
        }
        std::size_t total = 0;
        for (const sql_result_set& set : sets) {
            if (set.columns.size() > max_columns) {
                return "visible result exceeds 100 columns; select an explicit column list";
            }
            if (set.rows.size() > max_rows) {
                return "visible result exceeds 200 rows; add a deterministic LIMIT";
            }
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
