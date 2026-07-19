module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
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
    bool require_extension{true};
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
        std::string admin_role;
        std::expected<sql_example_result, diagnostic> result =
            std::unexpected(diagnostic{});
        try {
            bioseq::libpq::connection admin(settings.admin_conninfo);
            const bioseq::libpq::result admin_identity =
                admin.exec("SELECT current_user");
            if (admin_identity.rows() != 1 || admin_identity.is_null(0, 0)) {
                throw bioseq::libpq::query_error(
                    "could not determine the private-cluster administrator role",
                    std::string{});
            }
            admin_role = std::string(admin_identity.value(0, 0));
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
            // standard cluster databases also reject PUBLIC CONNECT, so a restricted
            // role can reach only its own clone. Idempotent across serial examples.
            harden_template(admin, quoted_template);
            (void)admin.exec("REVOKE CONNECT ON DATABASE postgres FROM PUBLIC");
            (void)admin.exec("REVOKE CONNECT ON DATABASE template0 FROM PUBLIC");
            (void)admin.exec("REVOKE CONNECT ON DATABASE template1 FROM PUBLIC");
            (void)admin.exec("CREATE ROLE " + quoted_role
                + " LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS");
            (void)admin.exec("CREATE DATABASE " + quoted_clone + " TEMPLATE "
                + quoted_template + " OWNER " + quoted_role);
            (void)admin.exec("REVOKE CONNECT ON DATABASE " + quoted_clone + " FROM PUBLIC");
            (void)admin.exec("GRANT CONNECT ON DATABASE " + quoted_clone
                + " TO " + quoted_role);
            grant_extension_clone_acl(settings, admin_role, role, clone);
            // A second restricted LOGIN role that owns nothing, standing in for
            // another example's role: the clone must reject it (§13.3), proving
            // this role could not reach another example's clone either.
            (void)admin.exec("CREATE ROLE " + quoted_peer
                + " LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS");
            // §13.3 clone-ACL verification and negative connection assertions run
            // before the example; a violated security guarantee fails the example.
            if (auto violation = verify_clone_acl(admin, role, clone)) {
                result = std::unexpected(std::move(*violation));
            } else {
                std::optional<bioseq::libpq::connection> role_connection;
                try {
                    std::optional<diagnostic> preflight_violation;
                    {
                        // Positive socket/HBA control: establish the restricted
                        // role's own-clone connection before interpreting any
                        // failed foreign connection as a security denial.
                        bioseq::libpq::connection probe_connection(
                            bioseq::libpq::unix_socket_conninfo(
                                settings.socket_directory, clone, role, settings.port));
                        if (auto violation = assert_no_foreign_connect(
                                settings, role, clone, peer_role)) {
                            preflight_violation = std::move(*violation);
                        } else if (auto violation = assert_extension_clone_acl(
                                       probe_connection, settings.require_extension)) {
                            preflight_violation = std::move(*violation);
                        } else if (auto violation = assert_no_privilege_escalation(
                                       probe_connection, admin, admin_role,
                                       settings.socket_directory, suffix)) {
                            preflight_violation = std::move(*violation);
                        }
                    }  // The probe session can never leak state into tutorial SQL.
                    if (preflight_violation) {
                        result = std::unexpected(std::move(*preflight_violation));
                    } else {
                        role_connection.emplace(bioseq::libpq::unix_socket_conninfo(
                            settings.socket_directory, clone, role, settings.port));
                        result = run_example_on(*role_connection, example,
                                                /*run_teardown=*/false);
                    }
                } catch (const bioseq::libpq::query_error& error) {
                    result = std::unexpected(make_error("execute", error));
                }
                // Author teardown is permitted only after every isolation preflight
                // passed and the post-preflight role session was established. A
                // failed preflight goes straight to administrative clone cleanup.
                if (role_connection) {
                    teardown_isolated(settings, role, clone, example,
                                      &*role_connection);
                }
            }
        } catch (const bioseq::libpq::query_error& error) {
            result = std::unexpected(make_error("isolate", error));
        }
        // Cleanup always runs on a FRESH bounded admin connection so a dead or
        // timed-out provisioning connection can never leak the clone/role (§13.3,
        // §13.5). Any cleanup error or verified residue overrides tutorial success.
        if (auto cleanup = cleanup_isolated(
                settings, admin_role, role, clone, peer_role)) {
            if (!result && !result.error().message.empty()) {
                diagnostic combined = std::move(result.error());
                combined.message += "; additionally, " + cleanup->message;
                result = std::unexpected(std::move(combined));
            } else {
                result = std::unexpected(std::move(*cleanup));
            }
        }
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
            const auto type_names = resolve_type_names(connection, visible.results);
            for (const bioseq::libpq::result& set : visible.results) {
                outcome.visible_results.push_back(project_result(set, type_names));
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

    // Cleans the clone over a FRESH bounded admin connection (§13.5), including
    // rolling back any 2PC state that DROP DATABASE ... WITH (FORCE) deliberately
    // does not remove. Every action is attempted, but the result is fail-closed:
    // any error or surviving database/role becomes an isolation diagnostic.
    [[nodiscard]] static std::optional<diagnostic> cleanup_isolated(
        isolation_settings const& settings, const std::string& admin_role,
        const std::string& role, const std::string& clone,
        const std::string& peer_role) {
        if (role.empty() && clone.empty() && peer_role.empty()) {
            return std::nullopt;
        }
        std::optional<diagnostic> failure;
        auto remember_failure = [&failure](std::string operation,
                                           const bioseq::libpq::query_error& error) {
            if (failure) {
                return;
            }
            const std::string sqlstate = error.sqlstate().empty()
                ? std::string("<client-side>") : std::string(error.sqlstate());
            failure = isolation_violation("isolated cleanup " + std::move(operation)
                + " failed with SQLSTATE " + sqlstate + ": " + error.what());
        };
        try {
            bioseq::libpq::connection admin(settings.admin_conninfo);
            auto admin_command = [&admin, &remember_failure](
                                     const std::string& sql,
                                     const std::string& operation) {
                try {
                    (void)admin.exec(sql, std::chrono::seconds(5));
                    return true;
                } catch (const bioseq::libpq::query_error& error) {
                    remember_failure(operation, error);
                    return false;
                }
            };

            if (!clone.empty()) {
                const std::string quoted_clone = admin.escape_identifier(clone);
                bool clone_exists = false;
                try {
                    const bioseq::libpq::result exists = admin.exec(
                        "SELECT EXISTS (SELECT 1 FROM pg_catalog.pg_database "
                        "WHERE datname = " + admin.escape_literal(clone) + ")",
                        std::chrono::seconds(5));
                    clone_exists = exists.rows() == 1 && !exists.is_null(0, 0)
                        && exists.value(0, 0) == "t";
                } catch (const bioseq::libpq::query_error& error) {
                    remember_failure("database existence check", error);
                    clone_exists = true;  // still attempt every bounded cleanup step
                }

                if (clone_exists) {
                    (void)admin_command("REVOKE CONNECT ON DATABASE " + quoted_clone
                        + " FROM PUBLIC", "PUBLIC CONNECT revocation");
                    (void)admin_command("ALTER DATABASE " + quoted_clone
                        + " IS_TEMPLATE false", "template-flag restoration");
                    (void)admin_command("ALTER DATABASE " + quoted_clone
                        + " ALLOW_CONNECTIONS true", "connection restoration");
                    (void)admin_command("ALTER DATABASE " + quoted_clone
                        + " CONNECTION LIMIT -1", "connection-limit restoration");

                    // Open the one administrative clone session before sealing.
                    // The seal precedes the 2PC snapshot, so no new role session
                    // can prepare a transaction after discovery and race the DROP.
                    std::optional<bioseq::libpq::connection> clone_admin;
                    if (admin_role.empty()) {
                        if (!failure) {
                            failure = isolation_violation(
                                "isolated cleanup cannot open the clone without the "
                                "administrator role name");
                        }
                    } else {
                        try {
                            clone_admin.emplace(bioseq::libpq::unix_socket_conninfo(
                                settings.socket_directory, clone,
                                admin_role, settings.port));
                        } catch (const bioseq::libpq::query_error& error) {
                            remember_failure("clone administrator connection", error);
                        }
                    }
                    (void)admin_command("ALTER DATABASE " + quoted_clone
                        + " CONNECTION LIMIT 0", "clone sealing");
                    (void)admin_command("ALTER DATABASE " + quoted_clone
                        + " ALLOW_CONNECTIONS false", "clone sealing");

                    try {
                        const bioseq::libpq::result prepared = admin.exec(
                            "SELECT gid FROM pg_catalog.pg_prepared_xacts "
                            "WHERE database::text = " + admin.escape_literal(clone)
                            + " ORDER BY gid",
                            std::chrono::seconds(5));
                        if (prepared.rows() > 0 && !clone_admin) {
                            if (!failure) {
                                failure = isolation_violation(
                                    "isolated cleanup cannot roll back prepared transactions "
                                    "without a clone administrator connection");
                            }
                        } else if (clone_admin) {
                            for (int row = 0; row < prepared.rows(); ++row) {
                                if (prepared.is_null(row, 0)) {
                                    if (!failure) {
                                        failure = isolation_violation(
                                            "isolated cleanup found a NULL prepared "
                                            "transaction identifier");
                                    }
                                    continue;
                                }
                                try {
                                    (void)clone_admin->exec(
                                        "ROLLBACK PREPARED "
                                            + clone_admin->escape_literal(
                                                prepared.value(row, 0)),
                                        std::chrono::seconds(5));
                                } catch (const bioseq::libpq::query_error& error) {
                                    remember_failure(
                                        "prepared-transaction rollback", error);
                                }
                            }
                        }
                        const bioseq::libpq::result remaining = admin.exec(
                            "SELECT count(*) FROM pg_catalog.pg_prepared_xacts "
                            "WHERE database::text = " + admin.escape_literal(clone),
                            std::chrono::seconds(5));
                        if (remaining.rows() != 1 || remaining.is_null(0, 0)
                            || remaining.value(0, 0) != "0") {
                            if (!failure) {
                                failure = isolation_violation(
                                    "isolated cleanup found a surviving prepared transaction");
                            }
                        }
                    } catch (const bioseq::libpq::query_error& error) {
                        remember_failure("prepared-transaction discovery", error);
                    }

                    clone_admin.reset();
                    (void)admin_command("DROP DATABASE IF EXISTS " + quoted_clone
                        + " WITH (FORCE)", "database drop");
                }
            }
            if (!role.empty()) {
                (void)admin_command("DROP ROLE IF EXISTS "
                    + admin.escape_identifier(role), "example-role drop");
            }
            if (!peer_role.empty()) {
                (void)admin_command("DROP ROLE IF EXISTS "
                    + admin.escape_identifier(peer_role), "peer-role drop");
            }

            std::vector<std::string> residue_conditions;
            if (!clone.empty()) {
                residue_conditions.push_back(
                    "EXISTS (SELECT 1 FROM pg_catalog.pg_database WHERE datname = "
                    + admin.escape_literal(clone) + ")");
            }
            if (!role.empty()) {
                residue_conditions.push_back(
                    "EXISTS (SELECT 1 FROM pg_catalog.pg_roles WHERE rolname = "
                    + admin.escape_literal(role) + ")");
            }
            if (!peer_role.empty()) {
                residue_conditions.push_back(
                    "EXISTS (SELECT 1 FROM pg_catalog.pg_roles WHERE rolname = "
                    + admin.escape_literal(peer_role) + ")");
            }
            std::string residue_sql = "SELECT NOT (";
            for (std::size_t index = 0; index < residue_conditions.size(); ++index) {
                if (index != 0) {
                    residue_sql += " OR ";
                }
                residue_sql += residue_conditions[index];
            }
            residue_sql += ")";
            try {
                const bioseq::libpq::result clean = admin.exec(
                    residue_sql, std::chrono::seconds(5));
                if (clean.rows() != 1 || clean.is_null(0, 0)
                    || clean.value(0, 0) != "t") {
                    if (!failure) {
                        failure = isolation_violation(
                            "isolated cleanup verification found a surviving clone or role");
                    }
                }
            } catch (const bioseq::libpq::query_error& error) {
                remember_failure("residue verification", error);
            }
        } catch (const bioseq::libpq::query_error& error) {
            remember_failure("fresh administrator connection", error);
        }
        return failure;
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
    // every pre-existing private-cluster database, and the clone must reject a
    // foreign role — which proves the role could not reach another example's
    // clone. A successful connection is a security breach.
    [[nodiscard]] static std::optional<diagnostic> assert_no_foreign_connect(
        isolation_settings const& settings, const std::string& role,
        const std::string& clone, const std::string& peer_role) {
        if (role_can_connect(settings, "postgres", role)) {
            return isolation_violation(
                "restricted role could connect to the administrative database");
        }
        if (role_can_connect(settings, "template0", role)) {
            return isolation_violation(
                "restricted role could connect to template0");
        }
        if (role_can_connect(settings, "template1", role)) {
            return isolation_violation(
                "restricted role could connect to template1");
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
        } catch (const bioseq::libpq::query_error& error) {
            if (error.sqlstate() == "42501") {
                return std::nullopt;  // insufficient_privilege, as required
            }
            const std::string sqlstate = error.sqlstate().empty()
                ? std::string("<client-side>") : std::string(error.sqlstate());
            return isolation_violation(
                std::move(violation) + " denial probe failed with SQLSTATE "
                + sqlstate + " instead of 42501: " + error.what());
        }
        return isolation_violation(std::move(violation));
    }

    // A probe that unexpectedly succeeds must leave no cluster state behind.
    // Cleanup failures are themselves isolation violations and are never hidden.
    [[nodiscard]] static std::optional<diagnostic> cleanup_probe(
        bioseq::libpq::connection& connection, const std::string& sql,
        std::string operation) {
        try {
            (void)connection.exec(sql, std::chrono::seconds(10));
            return std::nullopt;
        } catch (const bioseq::libpq::query_error& error) {
            const std::string sqlstate = error.sqlstate().empty()
                ? std::string("<client-side>") : std::string(error.sqlstate());
            return isolation_violation(std::move(operation)
                + " cleanup failed with SQLSTATE " + sqlstate + ": " + error.what());
        }
    }

    // PostgreSQL 13 has no pg_database_owner predefined role. Grant the small
    // extension ACL directly inside each clone instead, which is version-neutral
    // and disappears atomically with that clone.
    static void grant_extension_clone_acl(isolation_settings const& settings,
                                          const std::string& admin_role,
                                          const std::string& role,
                                          const std::string& clone) {
        if (!settings.require_extension) {
            return;
        }
        bioseq::libpq::connection clone_admin(
            bioseq::libpq::unix_socket_conninfo(
                settings.socket_directory, clone, admin_role, settings.port));
        const std::string quoted_role = clone_admin.escape_identifier(role);
        (void)clone_admin.exec("GRANT USAGE, CREATE ON SCHEMA bioseq_internal TO "
            + quoted_role);
        (void)clone_admin.exec(
            "GRANT SELECT ON TABLE bioseq_internal.index_policy TO " + quoted_role);
        (void)clone_admin.exec(
            "GRANT SELECT, INSERT, UPDATE, DELETE ON TABLE "
            "bioseq_internal.metadata_age_log TO " + quoted_role);
    }

    // In production, prove that the extension and every ACL-tested object exist,
    // then verify the dynamic clone owner receives exactly the narrow privileges
    // needed by runnable examples. The focused plain-SQL harness opts out
    // explicitly; absence can therefore never make a production check pass.
    [[nodiscard]] static std::optional<diagnostic> assert_extension_clone_acl(
        bioseq::libpq::connection& role_connection, bool require_extension) {
        if (!require_extension) {
            return std::nullopt;
        }
        const bioseq::libpq::result acl = role_connection.exec(
            "SELECT EXISTS (SELECT 1 FROM pg_catalog.pg_extension e "
            "WHERE e.extname = 'bioseq' "
            "AND pg_catalog.to_regnamespace('bioseq_internal') IS NOT NULL "
            "AND pg_catalog.to_regclass('bioseq_internal.index_policy') IS NOT NULL "
            "AND pg_catalog.to_regclass('bioseq_internal.metadata_age_log') IS NOT NULL "
            "AND pg_catalog.to_regclass('bioseq_internal.seq_sub_table') IS NOT NULL "
            "AND pg_catalog.to_regclass("
                "'bioseq_internal.sequence_source_epoch_seq') IS NOT NULL "
            "AND pg_catalog.to_regprocedure("
                "'bioseq_internal.add_to_extension_secdef(text,text)') IS NOT NULL "
            "AND NOT pg_catalog.pg_has_role(current_user, e.extowner, 'USAGE') "
            "AND current_user = (SELECT pg_catalog.pg_get_userbyid(d.datdba) "
                "FROM pg_catalog.pg_database d "
                "WHERE d.datname = pg_catalog.current_database()) "
            "AND pg_catalog.has_schema_privilege(current_user, 'bioseq_internal', 'USAGE') "
            "AND pg_catalog.has_schema_privilege(current_user, 'bioseq_internal', 'CREATE') "
            "AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_namespace n "
                "CROSS JOIN LATERAL pg_catalog.aclexplode(n.nspacl) a "
                "WHERE n.nspname = 'bioseq_internal' AND a.grantee = 0 "
                "AND a.privilege_type IN ('USAGE', 'CREATE')) "
            "AND COALESCE(pg_catalog.has_table_privilege(current_user, "
                "pg_catalog.to_regclass('bioseq_internal.index_policy'), 'SELECT'), false) "
            "AND NOT COALESCE(pg_catalog.has_table_privilege(current_user, "
                "pg_catalog.to_regclass('bioseq_internal.index_policy'), 'INSERT'), false) "
            "AND COALESCE(pg_catalog.has_table_privilege(current_user, "
                "pg_catalog.to_regclass('bioseq_internal.metadata_age_log'), 'SELECT'), false) "
            "AND COALESCE(pg_catalog.has_table_privilege(current_user, "
                "pg_catalog.to_regclass('bioseq_internal.metadata_age_log'), 'INSERT'), false) "
            "AND COALESCE(pg_catalog.has_table_privilege(current_user, "
                "pg_catalog.to_regclass('bioseq_internal.metadata_age_log'), 'UPDATE'), false) "
            "AND COALESCE(pg_catalog.has_table_privilege(current_user, "
                "pg_catalog.to_regclass('bioseq_internal.metadata_age_log'), 'DELETE'), false) "
            "AND NOT COALESCE(pg_catalog.has_table_privilege(current_user, "
                "pg_catalog.to_regclass('bioseq_internal.seq_sub_table'), 'SELECT'), false) "
            "AND NOT COALESCE(pg_catalog.has_sequence_privilege(current_user, "
                "pg_catalog.to_regclass('bioseq_internal.sequence_source_epoch_seq'), "
                "'USAGE'), false) "
            "AND NOT COALESCE(pg_catalog.has_function_privilege(current_user, "
                "pg_catalog.to_regprocedure("
                    "'bioseq_internal.add_to_extension_secdef(text,text)'), "
                "'EXECUTE'), false) "
            "AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_proc p "
                "JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace "
                "WHERE n.nspname = 'bioseq_internal' "
                "AND pg_catalog.has_function_privilege("
                    "current_user, p.oid, 'EXECUTE')))");
        if (acl.rows() != 1 || acl.is_null(0, 0) || acl.value(0, 0) != "t") {
            return isolation_violation(
                "clone owner does not have the required least-privilege extension ACL");
        }
        return std::nullopt;
    }

    // §13.3: execute every required cluster-escape operation as the restricted
    // clone role. Each command is syntactically and operationally valid, so only
    // a genuine PostgreSQL permission rejection counts as a pass. Unexpected
    // successes are cleaned up before the isolation violation is returned.
    [[nodiscard]] static std::optional<diagnostic> assert_no_privilege_escalation(
        bioseq::libpq::connection& role_connection,
        bioseq::libpq::connection& admin, const std::string& admin_role,
        const std::string& socket_directory, const std::string& suffix) {
        const std::string probe_name = "biosite_probe_" + suffix;
        const std::string probe = role_connection.escape_identifier(probe_name);
        if (auto violation = assert_role_denied(role_connection,
                "CREATE ROLE " + probe + " LOGIN",
                "restricted role could CREATE ROLE")) {
            if (auto cleanup = cleanup_probe(admin, "DROP ROLE IF EXISTS "
                    + admin.escape_identifier(probe_name), "CREATE ROLE probe")) {
                return cleanup;
            }
            return violation;
        }
        if (auto violation = assert_role_denied(role_connection,
                "CREATE DATABASE " + probe,
                "restricted role could CREATE DATABASE")) {
            if (auto cleanup = cleanup_probe(admin, "DROP DATABASE IF EXISTS "
                    + admin.escape_identifier(probe_name) + " WITH (FORCE)",
                    "CREATE DATABASE probe")) {
                return cleanup;
            }
            return violation;
        }

        const std::string custom_guc = "biosite.probe_" + suffix;
        if (auto violation = assert_role_denied(role_connection,
                "ALTER SYSTEM SET " + custom_guc + " = '1'",
                "restricted role could ALTER SYSTEM")) {
            if (auto cleanup = cleanup_probe(admin,
                    "ALTER SYSTEM RESET " + custom_guc, "ALTER SYSTEM probe")) {
                return cleanup;
            }
            return violation;
        }

        const std::filesystem::path tablespace_directory =
            std::filesystem::path(socket_directory) / ("tablespace_" + suffix);
        if (!tablespace_directory.is_absolute()) {
            return isolation_violation(
                "private socket directory is not absolute for the tablespace denial probe");
        }
        std::error_code path_error;
        const bool directory_created =
            std::filesystem::create_directory(tablespace_directory, path_error);
        if (!directory_created || path_error) {
            return isolation_violation(
                "could not create the empty tablespace denial-probe directory");
        }
        auto tablespace_violation = assert_role_denied(role_connection,
            "CREATE TABLESPACE " + probe + " LOCATION "
                + role_connection.escape_literal(tablespace_directory.string()),
            "restricted role could CREATE TABLESPACE");
        if (tablespace_violation) {
            if (auto cleanup = cleanup_probe(admin, "DROP TABLESPACE IF EXISTS "
                    + admin.escape_identifier(probe_name), "CREATE TABLESPACE probe")) {
                return cleanup;  // Keep the referenced path intact if DROP failed.
            }
        }
        (void)std::filesystem::remove(tablespace_directory, path_error);
        if (path_error) {
            return isolation_violation(
                "could not remove the tablespace denial-probe directory");
        }
        if (tablespace_violation) {
            return tablespace_violation;
        }

        if (auto violation = assert_role_denied(role_connection,
                "CREATE SUBSCRIPTION " + probe
                    + " CONNECTION 'dbname=postgres user=biosite_probe "
                      "password=biosite_probe' "
                      "PUBLICATION biosite_probe WITH "
                      "(connect = false, create_slot = false, enabled = false, "
                      "slot_name = NONE)",
                "restricted role could CREATE SUBSCRIPTION")) {
            if (auto cleanup = cleanup_probe(role_connection,
                    "DROP SUBSCRIPTION IF EXISTS " + probe,
                    "CREATE SUBSCRIPTION probe")) {
                return cleanup;
            }
            return violation;
        }
        if (auto violation = assert_role_denied(role_connection,
                "COPY (SELECT 1) TO PROGRAM '/bin/cat >/dev/null'",
                "restricted role could COPY TO PROGRAM")) {
            return violation;
        }
        if (auto violation = assert_role_denied(role_connection,
                "SET ROLE " + role_connection.escape_identifier(admin_role),
                "restricted role could SET ROLE to the cluster administrator")) {
            if (auto cleanup = cleanup_probe(role_connection, "RESET ROLE",
                    "SET ROLE probe")) {
                return cleanup;
            }
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
        const auto type_names = resolve_type_names(connection, visible.results);
        for (const bioseq::libpq::result& set : visible.results) {
            outcome.visible_results.push_back(project_result(set, type_names));
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

    using type_name_map = std::map<std::uint32_t, std::string>;

    // Resolve every distinct PQftype OID in one query against the same clone that
    // produced the visible results. This must happen before teardown because a
    // setup phase may have created clone-local domains or composite types (§13.6).
    [[nodiscard]] static type_name_map resolve_type_names(
        bioseq::libpq::connection& connection,
        const std::vector<bioseq::libpq::result>& results) {
        std::set<std::uint32_t> oids;
        for (const bioseq::libpq::result& result : results) {
            if (!result.has_rows()) {
                continue;
            }
            for (int column = 0; column < result.columns(); ++column) {
                oids.insert(result.column_type_oid(column));
            }
        }
        if (oids.empty()) {
            return {};
        }

        std::string sql =
            "SELECT pg_catalog.format_type(requested_oid, NULL) "
            "FROM pg_catalog.unnest(ARRAY[";
        bool first = true;
        for (const std::uint32_t oid : oids) {
            if (!first) {
                sql.push_back(',');
            }
            first = false;
            sql += std::to_string(oid);
        }
        sql += "]::oid[]) AS requested(requested_oid) ORDER BY requested_oid";
        const bioseq::libpq::result resolved =
            connection.exec(sql, std::chrono::seconds(5));
        if (!resolved.has_rows()
            || resolved.rows() != static_cast<int>(oids.size())) {
            throw bioseq::libpq::query_error(
                "same-clone PostgreSQL type-name lookup returned an incomplete result",
                std::string{});
        }

        type_name_map names;
        auto oid = oids.begin();
        for (int row = 0; row < resolved.rows(); ++row, ++oid) {
            if (resolved.is_null(row, 0)) {
                throw bioseq::libpq::query_error(
                    "same-clone PostgreSQL type-name lookup returned NULL",
                    std::string{});
            }
            names.emplace(*oid, std::string(resolved.value(row, 0)));
        }
        return names;
    }

    [[nodiscard]] static sql_result_set project_result(
        const bioseq::libpq::result& result, const type_name_map& type_names) {
        sql_result_set set;
        if (result.has_rows()) {
            set.kind = sql_result_kind::rows;
            for (int column = 0; column < result.columns(); ++column) {
                const std::uint32_t type_oid = result.column_type_oid(column);
                const auto type_name = type_names.find(type_oid);
                if (type_name == type_names.end()) {
                    throw bioseq::libpq::query_error(
                        "same-clone PostgreSQL type-name lookup omitted an OID",
                        std::string{});
                }
                set.columns.push_back(sql_column{
                    .name = result.column_name(column),
                    .type_oid = type_oid,
                    .type_name = type_name->second});
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
            set.affected_rows = result.affected_rows();
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
