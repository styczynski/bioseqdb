module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

export module bioseq.codegen.website.postgres:executor;

import bioseq.libpq.client;
import bioseq.codegen.website.model;
import bioseq.codegen.support;

// PostgreSQL tutorial execution projects libpq results onto the source-neutral
// website model. Native PGconn, PGresult, and catalog object identifiers stay
// behind this module boundary.
export namespace bioseq::codegen::website {

/// @brief Configures database-per-example PostgreSQL isolation.
///
/// Each example runs as a unique restricted role inside a unique database cloned
/// from the extension-bearing template, reached through the same private Unix
/// socket. The administrator connection targets the cluster control database.
struct postgres_isolation_settings {
    std::string administrator_connection_info;
    std::string socket_directory;
    int server_port{};
    std::string template_database;
    bool requires_extension{true};
    std::vector<std::string> extension_names;
    std::vector<std::string> documented_function_schemas;
    std::vector<std::string> documented_function_identities;
    std::vector<std::string> restricted_sequence_relations;
};

/// @brief Executes tutorial SQL against a private Unix-socket cluster.
///
/// Models sql_example_executor: read_catalog and execute return typed results
/// or a diagnostic, never raising libpq errors to callers.
class postgres_tutorial_executor {
public:
    /// @brief Creates an executor for an existing PostgreSQL database.
    /// @param connection_info libpq connection string for direct execution.
    explicit postgres_tutorial_executor(std::string connection_info)
        : connection_info_(std::move(connection_info)) {}

    /// @brief Captures the live SQL catalog snapshot from the cluster.
    /// @return The snapshot, or a diagnostic on any connection or command failure.
    [[nodiscard]] std::expected<sql_catalog_snapshot, diagnostic> read_catalog() {
        try {
            bioseq::libpq::client_connection connection(connection_info_);
            sql_catalog_snapshot snapshot;
            snapshot.server_version = connection.postgresql_release();
            const bioseq::libpq::command_result extension_release_rows =
                connection.execute(
                    "select extversion from pg_extension where extname = 'bioseq'");
            if (extension_release_rows.row_count() > 0
                && !extension_release_rows.cell_is_null(0, 0)) {
                snapshot.extension_version =
                    std::string(extension_release_rows.text_value(0, 0));
            }
            return snapshot;
        } catch (const bioseq::libpq::command_error& error) {
            return std::unexpected(make_command_diagnostic(
                diagnostic_operation::sql_catalog_read, error));
        }
    }

    /// @brief Runs an example's visible SQL and captures its result model.
    /// @param example The tutorial SQL example to execute.
    /// @return The visible result model, or a diagnostic on failure.
    [[nodiscard]] std::expected<sql_example_result, diagnostic> execute(
        sql_example const& example) {
        try {
            bioseq::libpq::client_connection connection(connection_info_);
            std::expected<sql_example_result, diagnostic> outcome = [&]() {
                try {
                    return execute_on_connection(
                        connection, example, /*run_teardown=*/false);
                } catch (const bioseq::libpq::command_error& error) {
                    return std::expected<sql_example_result, diagnostic>(
                        std::unexpected(make_command_diagnostic(
                            diagnostic_operation::sql_execution, error)));
                }
            }();
            if (std::optional<diagnostic> teardown =
                    run_direct_teardown(connection, example)) {
                teardown->message = "tutorial teardown failed: " + teardown->message;
                if (!outcome) {
                    outcome.error().notes.push_back(teardown->message);
                } else {
                    outcome = std::unexpected(std::move(*teardown));
                }
            }
            return outcome;
        } catch (const bioseq::libpq::command_error& error) {
            return std::unexpected(make_command_diagnostic(
                diagnostic_operation::sql_execution, error));
        }
    }

    /// @brief Runs one example in a dedicated PostgreSQL database and role.
    ///
    /// Creates a unique restricted LOGIN role and a unique database cloned from the
    /// extension-bearing template owned by that role, revokes PUBLIC CONNECT and
    /// grants it only to the role, runs the example as that role over the private
    /// socket, and always drops the clone (WITH FORCE) and role afterward — even on
    /// failure. The role never connects as the admin/initdb user.
    /// @param example The tutorial SQL example.
    /// @param settings Administrator connection, socket, port, and template database.
    /// @return The visible result model, or a diagnostic on failure.
    [[nodiscard]] std::expected<sql_example_result, diagnostic>
    execute_in_isolated_database(
        sql_example const& example,
        postgres_isolation_settings const& settings) {
        std::string role;
        std::string clone;
        std::string peer_role;
        std::string admin_role;
        std::expected<sql_example_result, diagnostic> result =
            std::unexpected(diagnostic{});
        try {
            bioseq::libpq::client_connection admin(
                settings.administrator_connection_info);
            const bioseq::libpq::command_result administrator_identity =
                admin.execute("SELECT current_user");
            if (administrator_identity.row_count() != 1
                || administrator_identity.cell_is_null(0, 0)) {
                throw bioseq::libpq::command_error(
                    "could not determine the private-cluster administrator role",
                    std::string{});
            }
            admin_role = std::string(administrator_identity.text_value(0, 0));
            const std::string suffix =
                make_unique_object_suffix(
                    example.local_id.empty() ? example.canonical_id
                                             : example.local_id);
            role = "biosite_r_" + suffix;
            clone = "biosite_d_" + suffix;
            peer_role = "biosite_p_" + suffix;
            const std::string quoted_role = admin.escape_identifier(role);
            const std::string quoted_clone = admin.escape_identifier(clone);
            const std::string quoted_peer = admin.escape_identifier(peer_role);
            const std::string quoted_template =
                admin.escape_identifier(settings.template_database);
            // The extension-bearing template becomes an
            // unconnectable IS_TEMPLATE database with no PUBLIC access, and the
            // standard cluster databases also reject PUBLIC CONNECT, so a restricted
            // role can reach only its own clone. Idempotent across serial examples.
            seal_template_database(admin, quoted_template);
            (void)admin.execute("REVOKE CONNECT ON DATABASE postgres FROM PUBLIC");
            (void)admin.execute("REVOKE CONNECT ON DATABASE template0 FROM PUBLIC");
            (void)admin.execute("REVOKE CONNECT ON DATABASE template1 FROM PUBLIC");
            (void)admin.execute("CREATE ROLE " + quoted_role
                + " LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS");
            (void)admin.execute("CREATE DATABASE " + quoted_clone + " TEMPLATE "
                + quoted_template + " OWNER " + quoted_role);
            (void)admin.execute("REVOKE CONNECT ON DATABASE " + quoted_clone + " FROM PUBLIC");
            (void)admin.execute("GRANT CONNECT ON DATABASE " + quoted_clone
                + " TO " + quoted_role);
            grant_extension_example_privileges(
                settings, admin_role, role, clone);
            // A second restricted LOGIN role that owns nothing, standing in for
            // another example's role: the clone must reject it, demonstrating
            // that roles cannot cross example-database boundaries.
            (void)admin.execute("CREATE ROLE " + quoted_peer
                + " LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS");
            // Clone-ACL verification and negative connection assertions run
            // before the example. A violated security guarantee fails the example.
            if (std::optional<diagnostic> violation =
                    check_clone_connection_acl(admin, role, clone)) {
                result = std::unexpected(std::move(*violation));
            } else {
                std::optional<bioseq::libpq::client_connection> role_connection;
                try {
                    std::optional<diagnostic> preflight_violation;
                    {
                        // Positive socket/HBA control: establish the restricted
                        // role's own-clone connection before interpreting any
                        // failed foreign connection as a security denial.
                        bioseq::libpq::client_connection probe_connection(
                            bioseq::libpq::make_unix_socket_conninfo(
                                settings.socket_directory, clone, role,
                                settings.server_port));
                        if (std::optional<diagnostic> violation =
                                check_database_connection_isolation(
                                    settings, role, clone, peer_role)) {
                            preflight_violation = std::move(*violation);
                        } else if (std::optional<diagnostic> violation =
                                       check_extension_example_privileges(
                                           probe_connection, settings)) {
                            preflight_violation = std::move(*violation);
                        } else if (std::optional<diagnostic> violation =
                                       check_cluster_privilege_isolation(
                                           probe_connection, admin, admin_role,
                                           settings.socket_directory, suffix)) {
                            preflight_violation = std::move(*violation);
                        }
                    }  // The probe session can never leak state into tutorial SQL.
                    if (preflight_violation) {
                        result = std::unexpected(std::move(*preflight_violation));
                    } else {
                        role_connection.emplace(bioseq::libpq::make_unix_socket_conninfo(
                            settings.socket_directory, clone, role,
                            settings.server_port));
                        result = execute_on_connection(
                            *role_connection, example,
                            /*run_teardown=*/false);
                    }
                } catch (const bioseq::libpq::command_error& error) {
                    result = std::unexpected(make_command_diagnostic(
                        diagnostic_operation::sql_execution, error));
                }
                // Author teardown is permitted only after every isolation preflight
                // passed and the post-preflight role session was established. A
                // failed preflight goes straight to administrative clone cleanup.
                if (role_connection) {
                    run_isolated_teardown(
                        settings, role, clone, example, &*role_connection);
                }
            }
        } catch (const bioseq::libpq::command_error& error) {
            result = std::unexpected(make_command_diagnostic(
                diagnostic_operation::database_isolation, error));
        }
        // Cleanup always runs on a fresh bounded administrator connection so a
        // dead provisioning connection cannot leak a clone or role. Cleanup errors
        // and verified residue override tutorial success.
        if (std::optional<diagnostic> cleanup = cleanup_isolated_database(
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
    /// @brief Executes the configured SQL steps on an already-open connection.
    /// @param connection PostgreSQL connection that owns the example session.
    /// @param example Parsed tutorial example.
    /// @param run_teardown Whether teardown runs on the same connection.
    /// @return Captured visible output, or the first execution diagnostic.
    [[nodiscard]] static std::expected<sql_example_result, diagnostic>
    execute_on_connection(
        bioseq::libpq::client_connection& connection, sql_example const& example,
        bool run_teardown = true) {
        sql_example_result outcome;
        outcome.example_id = example.canonical_id;
        // Setup, visible SQL, and teardown share one client deadline capped at
        // sixty seconds, independent of the server statement_timeout that the
        // deterministic session settings also install.
        const std::chrono::milliseconds deadline = execution_deadline(example);
        // Deterministic session settings precede setup so formatting,
        // search_path, and server timeouts are fixed for
        // every command. The bounded example timeout drives statement_timeout.
        configure_tutorial_session(connection, example);
        if (!example.setup_sql.empty()) {
            (void)connection.execute(example.setup_sql, deadline);
        }
        // Only notices from visible SQL may render. Discard session-configuration
        // and setup notices before running it.
        (void)connection.drain_notices();
        // The visible SQL is where the success/error expectation applies. A
        // success example must not raise. An error example must raise the expected
        // SQLSTATE (or any error, when unspecified). Completed result sets remain
        // in execution order.
        if (example.expectation == sql_expectation::error) {
            if (std::optional<diagnostic> failure = capture_expected_error(
                    connection, example, deadline, outcome)) {
                return std::unexpected(std::move(*failure));
            }
        } else {
            // The retention limits bound chunked accumulation as it streams. A
            // breach is still reported by check_result_limits below with its
            // author-facing message rather than a silent truncation.
            const bioseq::libpq::command_batch_result visible_batch =
                connection.execute_batch_with_limits(
                    example.visible_sql, deadline, visible_result_limits());
            if (visible_batch.error) {
                // A success example must not raise a server error.
                return std::unexpected(
                    make_command_diagnostic(
                        diagnostic_operation::sql_execution,
                        *visible_batch.error));
            }
            const sql_type_name_by_oid type_names =
                load_sql_type_names(connection, visible_batch.completed);
            for (const bioseq::libpq::command_result& command :
                 visible_batch.completed) {
                outcome.visible_results.push_back(
                    make_result_set(command, type_names));
            }
            // Exceeding a retention limit is a build error, never a silent
            // truncation: displayed output must be the actual result.
            if (std::optional<std::string> breach =
                    check_result_limits(outcome.visible_results)) {
                diagnostic limit;
                limit.severity = diagnostic_severity::error;
                limit.operation = diagnostic_operation::sql_execution;
                limit.message = std::move(*breach);
                return std::unexpected(limit);
            }
        }
        for (const bioseq::libpq::server_notice& raw : connection.drain_notices()) {
            if (outcome.notices.size() >= 100) {
                break;
            }
            outcome.notices.push_back(sql_notice{
                .stage = sql_execution_stage::visible,
                .severity = raw.severity,
                .sqlstate = raw.sql_state,
                .message = raw.message});
        }
        if (run_teardown && !example.teardown_sql.empty()) {
            try {
                (void)connection.execute(example.teardown_sql, deadline);
            } catch (const bioseq::libpq::command_error&) {
                // Teardown failure is ignored after a captured expected error.
            }
        }
        return outcome;
    }

    /// @brief Runs author teardown after every direct execution outcome.
    ///
    /// A failed visible statement can leave an explicit transaction aborted.
    /// Roll it back before teardown so objects created by earlier autocommit
    /// setup statements are still removed. A teardown error fails the example.
    /// When execution also failed, its diagnostic remains the primary error.
    /// @param connection Direct tutorial connection that owns the session.
    /// @param example Example whose teardown SQL should run.
    /// @return A teardown diagnostic when rollback or cleanup fails.
    [[nodiscard]] static std::optional<diagnostic> run_direct_teardown(
        bioseq::libpq::client_connection& connection,
        sql_example const& example) {
        if (example.teardown_sql.empty()) {
            return std::nullopt;
        }
        const std::chrono::milliseconds deadline = execution_deadline(example);
        try {
            if (!connection.is_transaction_idle()) {
                (void)connection.execute("ROLLBACK", deadline);
            }
            (void)connection.execute(example.teardown_sql, deadline);
        } catch (const bioseq::libpq::command_error& error) {
            return make_command_diagnostic(
                diagnostic_operation::sql_execution, error);
        }
        return std::nullopt;
    }

    /// @brief Returns the bounded client deadline for one tutorial SQL step.
    /// @param example Tutorial example whose timeout supplies the requested bound.
    /// @return A positive duration no greater than sixty seconds.
    [[nodiscard]] static std::chrono::milliseconds execution_deadline(
        const sql_example& example) {
        long long timeout_ms = example.timeout.count();
        if (timeout_ms <= 0 || timeout_ms > 60'000) {
            timeout_ms = 60'000;
        }
        return std::chrono::milliseconds(timeout_ms);
    }

    /// @brief Returns the bounded retention policy for visible tutorial results.
    /// @return Limits shared by streaming collection and post-collection checks.
    [[nodiscard]] static bioseq::libpq::result_limits visible_result_limits() {
        return bioseq::libpq::result_limits{
            .max_rows = 200,
            .max_columns = 100,
            .max_cell_bytes = 64 * 1024,
            .max_total_bytes = 2 * 1024 * 1024,
            .max_result_sets = 10};
    }

    /// @brief Attempts one bounded administrative command without propagating errors.
    /// @param admin Administrative connection used for cleanup.
    /// @param sql Command to execute within the fixed cleanup deadline.
    static void try_cleanup_command(
        bioseq::libpq::client_connection& admin,
        const std::string& sql) {
        try {
            (void)admin.execute(sql, std::chrono::seconds(5));
        } catch (const bioseq::libpq::command_error&) {
        }
    }

    /// @brief Records the first bounded-cleanup failure.
    static void record_cleanup_failure(
        std::optional<diagnostic>& failure,
        std::string operation,
        const bioseq::libpq::command_error& error) {
        if (failure) {
            return;
        }
        const std::string sql_state = error.sql_state().empty()
            ? std::string("<client-side>") : std::string(error.sql_state());
        failure = make_isolation_error(
            "isolated cleanup " + std::move(operation)
            + " failed with SQLSTATE " + sql_state + ": " + error.what());
    }

    /// @brief Runs one cleanup command while retaining its first failure.
    /// @return `true` when PostgreSQL accepts the command.
    [[nodiscard]] static bool run_cleanup_command(
        bioseq::libpq::client_connection& admin,
        std::optional<diagnostic>& failure,
        const std::string& sql,
        const std::string& operation) {
        try {
            (void)admin.execute(sql, std::chrono::seconds(5));
            return true;
        } catch (const bioseq::libpq::command_error& error) {
            record_cleanup_failure(failure, operation, error);
            return false;
        }
    }

    /// @brief Removes every database and role created for an isolated example.
    /// @param settings Private-cluster connection settings.
    /// @param admin_role Cluster administrator role used for clone recovery.
    /// @param role Restricted example role.
    /// @param clone Per-example database.
    /// @param peer_role Restricted role used by negative connection checks.
    /// @return An error if cleanup fails or leaves cluster state behind.
    [[nodiscard]] static std::optional<diagnostic> cleanup_isolated_database(
        postgres_isolation_settings const& settings,
        const std::string& admin_role,
        const std::string& role, const std::string& clone,
        const std::string& peer_role) {
        if (role.empty() && clone.empty() && peer_role.empty()) {
            return std::nullopt;
        }
        std::optional<diagnostic> failure;
        try {
            bioseq::libpq::client_connection admin(
                settings.administrator_connection_info);

            if (!clone.empty()) {
                const std::string quoted_clone = admin.escape_identifier(clone);
                bool clone_exists = false;
                try {
                    const bioseq::libpq::command_result exists = admin.execute(
                        "SELECT EXISTS (SELECT 1 FROM pg_catalog.pg_database "
                        "WHERE datname = " + admin.escape_literal(clone) + ")",
                        std::chrono::seconds(5));
                    clone_exists = exists.row_count() == 1 && !exists.cell_is_null(0, 0)
                        && exists.text_value(0, 0) == "t";
                } catch (const bioseq::libpq::command_error& error) {
                    record_cleanup_failure(
                        failure, "database existence check", error);
                    clone_exists = true;  // still attempt every bounded cleanup step
                }

                if (clone_exists) {
                    (void)run_cleanup_command(admin, failure,
                        "REVOKE CONNECT ON DATABASE " + quoted_clone
                        + " FROM PUBLIC", "PUBLIC CONNECT revocation");
                    (void)run_cleanup_command(admin, failure,
                        "ALTER DATABASE " + quoted_clone
                        + " IS_TEMPLATE false", "template-flag restoration");
                    (void)run_cleanup_command(admin, failure,
                        "ALTER DATABASE " + quoted_clone
                        + " ALLOW_CONNECTIONS true", "connection restoration");
                    (void)run_cleanup_command(admin, failure,
                        "ALTER DATABASE " + quoted_clone
                        + " CONNECTION LIMIT -1", "connection-limit restoration");

                    // Open the one administrative clone session before sealing.
                    // The seal precedes the 2PC snapshot, so no new role session
                    // can prepare a transaction after discovery and race the DROP.
                    std::optional<bioseq::libpq::client_connection> clone_admin;
                    if (admin_role.empty()) {
                        if (!failure) {
                            failure = make_isolation_error(
                                "isolated cleanup cannot open the clone without the "
                                "administrator role name");
                        }
                    } else {
                        try {
                            clone_admin.emplace(bioseq::libpq::make_unix_socket_conninfo(
                                settings.socket_directory, clone,
                                admin_role, settings.server_port));
                        } catch (const bioseq::libpq::command_error& error) {
                            record_cleanup_failure(
                                failure, "clone administrator connection", error);
                        }
                    }
                    (void)run_cleanup_command(admin, failure,
                        "ALTER DATABASE " + quoted_clone
                        + " CONNECTION LIMIT 0", "clone sealing");
                    (void)run_cleanup_command(admin, failure,
                        "ALTER DATABASE " + quoted_clone
                        + " ALLOW_CONNECTIONS false", "clone sealing");

                    try {
                        const bioseq::libpq::command_result prepared = admin.execute(
                            "SELECT gid FROM pg_catalog.pg_prepared_xacts "
                            "WHERE database::text = " + admin.escape_literal(clone)
                            + " ORDER BY gid",
                            std::chrono::seconds(5));
                        if (prepared.row_count() > 0 && !clone_admin) {
                            if (!failure) {
                                failure = make_isolation_error(
                                    "isolated cleanup cannot roll back prepared transactions "
                                    "without a clone administrator connection");
                            }
                        } else if (clone_admin) {
                            for (std::size_t row = 0;
                                 row < prepared.row_count(); ++row) {
                                if (prepared.cell_is_null(row, 0)) {
                                    if (!failure) {
                                        failure = make_isolation_error(
                                            "isolated cleanup found a NULL prepared "
                                            "transaction identifier");
                                    }
                                    continue;
                                }
                                try {
                                    (void)clone_admin->execute(
                                        "ROLLBACK PREPARED "
                                            + clone_admin->escape_literal(
                                                prepared.text_value(row, 0)),
                                        std::chrono::seconds(5));
                                } catch (const bioseq::libpq::command_error& error) {
                                    record_cleanup_failure(failure,
                                        "prepared-transaction rollback", error);
                                }
                            }
                        }
                        const bioseq::libpq::command_result remaining = admin.execute(
                            "SELECT count(*) FROM pg_catalog.pg_prepared_xacts "
                            "WHERE database::text = " + admin.escape_literal(clone),
                            std::chrono::seconds(5));
                        if (remaining.row_count() != 1 || remaining.cell_is_null(0, 0)
                            || remaining.text_value(0, 0) != "0") {
                            if (!failure) {
                                failure = make_isolation_error(
                                    "isolated cleanup found a surviving prepared transaction");
                            }
                        }
                    } catch (const bioseq::libpq::command_error& error) {
                        record_cleanup_failure(
                            failure, "prepared-transaction discovery", error);
                    }

                    clone_admin.reset();
                    (void)run_cleanup_command(admin, failure,
                        "DROP DATABASE IF EXISTS " + quoted_clone
                        + " WITH (FORCE)", "database drop");
                }
            }
            if (!role.empty()) {
                (void)run_cleanup_command(admin, failure, "DROP ROLE IF EXISTS "
                    + admin.escape_identifier(role), "example-role drop");
            }
            if (!peer_role.empty()) {
                (void)run_cleanup_command(admin, failure, "DROP ROLE IF EXISTS "
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
                const bioseq::libpq::command_result clean = admin.execute(
                    residue_sql, std::chrono::seconds(5));
                if (clean.row_count() != 1 || clean.cell_is_null(0, 0)
                    || clean.text_value(0, 0) != "t") {
                    if (!failure) {
                        failure = make_isolation_error(
                            "isolated cleanup verification found a surviving clone or role");
                    }
                }
            } catch (const bioseq::libpq::command_error& error) {
                record_cleanup_failure(failure, "residue verification", error);
            }
        } catch (const bioseq::libpq::command_error& error) {
            record_cleanup_failure(
                failure, "fresh administrator connection", error);
        }
        return failure;
    }

    /// @brief Seals the extension template against ordinary connections.
    /// @param admin Administrative cluster connection.
    /// @param quoted_template Escaped template-database identifier.
    static void seal_template_database(
        bioseq::libpq::client_connection& admin,
        const std::string& quoted_template) {
        (void)admin.execute("REVOKE CONNECT ON DATABASE " + quoted_template + " FROM PUBLIC");
        (void)admin.execute("ALTER DATABASE " + quoted_template + " IS_TEMPLATE true");
        (void)admin.execute("ALTER DATABASE " + quoted_template + " ALLOW_CONNECTIONS false");
    }

    /// @brief Builds a fatal isolation diagnostic.
    /// @param message Actionable security or cleanup failure message.
    /// @return An error-severity diagnostic attributed to database isolation.
    [[nodiscard]] static diagnostic make_isolation_error(std::string message) {
        diagnostic violation;
        violation.severity = diagnostic_severity::error;
        violation.operation = diagnostic_operation::database_isolation;
        violation.message = std::move(message);
        return violation;
    }

    /// @brief Checks that only the owning example role may connect to its clone.
    /// @param admin Administrative connection used to inspect `pg_database`.
    /// @param role Restricted role that owns the clone.
    /// @param clone Per-example database name.
    /// @return An isolation error when the connection ACL is too broad or incomplete.
    [[nodiscard]] static std::optional<diagnostic> check_clone_connection_acl(
        bioseq::libpq::client_connection& admin, const std::string& role,
        const std::string& clone) {
        const bioseq::libpq::command_result acl = admin.execute(
            "SELECT "
            "COALESCE(bool_or(g.grantee = 0 AND g.privilege_type = 'CONNECT'), false), "
            "COALESCE(bool_or(g.grantee = (SELECT oid FROM pg_roles WHERE rolname = "
                + admin.escape_literal(role)
                + ") AND g.privilege_type = 'CONNECT'), false) "
            "FROM pg_database d LEFT JOIN LATERAL aclexplode(d.datacl) g ON true "
            "WHERE d.datname = " + admin.escape_literal(clone));
        const bool public_connect = acl.row_count() > 0 && acl.text_value(0, 0) == "t";
        const bool role_connect = acl.row_count() > 0 && acl.text_value(0, 1) == "t";
        if (public_connect) {
            return make_isolation_error(
                "clone database still grants CONNECT to PUBLIC");
        }
        if (!role_connect) {
            return make_isolation_error(
                "clone database does not grant CONNECT to the example role");
        }
        return std::nullopt;
    }

    /// @brief Checks whether a restricted role can connect to one database.
    /// @return `true` only when libpq establishes the requested connection.
    [[nodiscard]] static bool can_role_connect(
        postgres_isolation_settings const& settings,
        const std::string& database,
        const std::string& role) {
        try {
            bioseq::libpq::client_connection probe(bioseq::libpq::make_unix_socket_conninfo(
                settings.socket_directory, database, role,
                settings.server_port));
            return true;
        } catch (const bioseq::libpq::command_error&) {
            return false;
        }
    }

    /// @brief Checks cross-database and cross-role connection isolation.
    /// @return The first unexpected connection as an isolation error.
    [[nodiscard]] static std::optional<diagnostic>
    check_database_connection_isolation(
        postgres_isolation_settings const& settings,
        const std::string& role,
        const std::string& clone, const std::string& peer_role) {
        if (can_role_connect(settings, "postgres", role)) {
            return make_isolation_error(
                "restricted role could connect to the administrative database");
        }
        if (can_role_connect(settings, "template0", role)) {
            return make_isolation_error(
                "restricted role could connect to template0");
        }
        if (can_role_connect(settings, "template1", role)) {
            return make_isolation_error(
                "restricted role could connect to template1");
        }
        if (can_role_connect(settings, settings.template_database, role)) {
            return make_isolation_error(
                "restricted role could connect to the extension template");
        }
        if (can_role_connect(settings, clone, peer_role)) {
            return make_isolation_error(
                "a foreign role could connect to the example clone");
        }
        return std::nullopt;
    }

    /// @brief Requires a restricted-role command to fail with insufficient privilege.
    /// @param role_connection Connection authenticated as the restricted role.
    /// @param sql Security-sensitive command to attempt.
    /// @param violation Message returned if the command succeeds.
    /// @return No value for SQLSTATE 42501. Otherwise an isolation error.
    [[nodiscard]] static std::optional<diagnostic> check_command_denied(
        bioseq::libpq::client_connection& role_connection, const std::string& sql,
        std::string violation) {
        try {
            (void)role_connection.execute(sql, std::chrono::seconds(10));
        } catch (const bioseq::libpq::command_error& error) {
            if (error.sql_state() == "42501") {
                return std::nullopt;  // insufficient_privilege, as required
            }
            const std::string sql_state = error.sql_state().empty()
                ? std::string("<client-side>") : std::string(error.sql_state());
            return make_isolation_error(
                std::move(violation) + " denial probe failed with SQLSTATE "
                + sql_state + " instead of 42501: " + error.what());
        }
        return make_isolation_error(std::move(violation));
    }

    /// @brief Removes state created by a security-sensitive command attempt.
    /// @return An isolation error if the bounded cleanup command fails.
    [[nodiscard]] static std::optional<diagnostic> cleanup_security_probe(
        bioseq::libpq::client_connection& connection, const std::string& sql,
        std::string operation) {
        try {
            (void)connection.execute(sql, std::chrono::seconds(10));
            return std::nullopt;
        } catch (const bioseq::libpq::command_error& error) {
            const std::string sql_state = error.sql_state().empty()
                ? std::string("<client-side>") : std::string(error.sql_state());
            return make_isolation_error(std::move(operation)
                + " cleanup failed with SQLSTATE " + sql_state + ": " + error.what());
        }
    }

    /// @brief Grants the clone owner only the extension privileges examples need.
    /// @param settings Private-cluster and extension policy.
    /// @param admin_role Cluster administrator used to enter the clone.
    /// @param role Restricted example role receiving privileges.
    /// @param clone Per-example database name.
    static void grant_extension_example_privileges(
        postgres_isolation_settings const& settings,
        const std::string& admin_role,
        const std::string& role,
        const std::string& clone) {
        if (!settings.requires_extension) {
            return;
        }
        bioseq::libpq::client_connection clone_admin(
            bioseq::libpq::make_unix_socket_conninfo(
                settings.socket_directory, clone, admin_role,
                settings.server_port));
        const std::string quoted_role = clone_admin.escape_identifier(role);
        (void)clone_admin.execute("GRANT USAGE, CREATE ON SCHEMA bioseq_internal TO "
            + quoted_role);
        (void)clone_admin.execute(
            "GRANT SELECT ON TABLE bioseq_internal.index_policy TO " + quoted_role);
        (void)clone_admin.execute(
            "GRANT SELECT, INSERT, UPDATE, DELETE ON TABLE "
            "bioseq_internal.metadata_age_log TO " + quoted_role);
        (void)clone_admin.execute(
            "GRANT pg_read_all_stats TO " + quoted_role);
        for (const std::string& schema :
             settings.documented_function_schemas) {
            (void)clone_admin.execute(
                "GRANT USAGE ON SCHEMA "
                + clone_admin.escape_identifier(schema)
                + " TO " + quoted_role);
        }
        for (const std::string& identity :
             settings.documented_function_identities) {
            const std::size_t signature = identity.find(':', 4);
            if (!identity.starts_with("sql:")
                || signature == std::string::npos
                || signature + 1 >= identity.size()) {
                throw std::runtime_error(
                    "invalid documented SQL function identity '"
                    + identity + "'");
            }
            (void)clone_admin.execute(
                "GRANT EXECUTE ON FUNCTION "
                + identity.substr(signature + 1)
                + " TO " + quoted_role);
        }
    }

    /// @brief Checks the clone owner's least-privilege extension access.
    /// @param role_connection Connection authenticated as the clone owner.
    /// @param settings Extension policy and restricted sequence relations.
    /// @return An isolation error when an object is missing or access is too broad.
    [[nodiscard]] static std::optional<diagnostic>
    check_extension_example_privileges(
        bioseq::libpq::client_connection& role_connection,
        postgres_isolation_settings const& settings) {
        if (!settings.requires_extension) {
            return std::nullopt;
        }
        if (settings.restricted_sequence_relations.empty()) {
            return make_isolation_error(
                "generated SQL manifest supplied no internal sequence ACL targets");
        }
        if (settings.extension_names.empty()
            || settings.documented_function_schemas.empty()
            || settings.documented_function_identities.empty()) {
            return make_isolation_error(
                "generated SQL manifest set supplied incomplete extension ACL targets");
        }
        std::string extension_acl_checks;
        for (const std::string& extension_name : settings.extension_names) {
            extension_acl_checks +=
                "AND EXISTS (SELECT 1 FROM pg_catalog.pg_extension x "
                "WHERE x.extname = "
                + role_connection.escape_literal(extension_name)
                + " AND NOT pg_catalog.pg_has_role("
                  "current_user, x.extowner, 'USAGE')) ";
        }
        std::string schema_acl_checks;
        for (const std::string& schema :
             settings.documented_function_schemas) {
            const std::string literal =
                role_connection.escape_literal(schema);
            schema_acl_checks +=
                "AND pg_catalog.to_regnamespace(" + literal
                + ") IS NOT NULL "
                  "AND pg_catalog.has_schema_privilege("
                  "current_user, " + literal + ", 'USAGE') ";
        }
        std::string function_acl_checks;
        for (const std::string& identity :
             settings.documented_function_identities) {
            const std::size_t signature = identity.find(':', 4);
            if (!identity.starts_with("sql:")
                || signature == std::string::npos
                || signature + 1 >= identity.size()) {
                return make_isolation_error(
                    "invalid documented SQL function identity '"
                    + identity + "'");
            }
            const std::string literal = role_connection.escape_literal(
                identity.substr(signature + 1));
            function_acl_checks +=
                "AND pg_catalog.to_regprocedure(" + literal
                + ") IS NOT NULL "
                  "AND pg_catalog.has_function_privilege("
                  "current_user, pg_catalog.to_regprocedure(" + literal
                + "), 'EXECUTE') ";
        }
        std::string sequence_acl_checks;
        for (const std::string& qualified_name :
             settings.restricted_sequence_relations) {
            const std::string literal =
                role_connection.escape_literal(qualified_name);
            sequence_acl_checks +=
                "AND pg_catalog.to_regclass(" + literal
                + ") IS NOT NULL "
                  "AND NOT COALESCE(pg_catalog.has_sequence_privilege("
                  "current_user, pg_catalog.to_regclass(" + literal
                + "), 'USAGE'), false) ";
        }
        const bioseq::libpq::command_result acl = role_connection.execute(
            "SELECT EXISTS (SELECT 1 FROM pg_catalog.pg_extension e "
            "WHERE e.extname = 'bioseq' "
            "AND pg_catalog.to_regnamespace('bioseq_internal') IS NOT NULL "
            "AND pg_catalog.to_regclass('bioseq_internal.index_policy') IS NOT NULL "
            "AND pg_catalog.to_regclass('bioseq_internal.metadata_age_log') IS NOT NULL "
            "AND pg_catalog.to_regclass('bioseq_internal.seq_sub_table') IS NOT NULL "
                + extension_acl_checks
                + schema_acl_checks
                + function_acl_checks +
            "AND pg_catalog.pg_has_role("
                "current_user, 'pg_read_all_stats', 'MEMBER') "
                + sequence_acl_checks +
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
            "AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_proc p "
                "JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace "
                "WHERE n.nspname = 'bioseq_internal' "
                "AND pg_catalog.has_function_privilege("
                    "current_user, p.oid, 'EXECUTE')))");
        if (acl.row_count() != 1 || acl.cell_is_null(0, 0) || acl.text_value(0, 0) != "t") {
            return make_isolation_error(
                "clone owner does not have the required least-privilege extension ACL");
        }
        return std::nullopt;
    }

    /// @brief Checks that the clone role cannot perform cluster-wide operations.
    /// @return The first privilege or cleanup violation, if any.
    [[nodiscard]] static std::optional<diagnostic>
    check_cluster_privilege_isolation(
        bioseq::libpq::client_connection& role_connection,
        bioseq::libpq::client_connection& admin, const std::string& admin_role,
        const std::string& socket_directory, const std::string& suffix) {
        const std::string probe_name = "biosite_probe_" + suffix;
        const std::string probe = role_connection.escape_identifier(probe_name);
        if (std::optional<diagnostic> violation = check_command_denied(
                role_connection,
                "CREATE ROLE " + probe + " LOGIN",
                "restricted role could CREATE ROLE")) {
            if (std::optional<diagnostic> cleanup = cleanup_security_probe(
                    admin,
                    "DROP ROLE IF EXISTS " + admin.escape_identifier(probe_name),
                    "CREATE ROLE probe")) {
                return cleanup;
            }
            return violation;
        }
        if (std::optional<diagnostic> violation = check_command_denied(
                role_connection,
                "CREATE DATABASE " + probe,
                "restricted role could CREATE DATABASE")) {
            if (std::optional<diagnostic> cleanup = cleanup_security_probe(
                    admin,
                    "DROP DATABASE IF EXISTS "
                        + admin.escape_identifier(probe_name) + " WITH (FORCE)",
                    "CREATE DATABASE probe")) {
                return cleanup;
            }
            return violation;
        }

        const std::string custom_guc = "biosite.probe_" + suffix;
        if (std::optional<diagnostic> violation = check_command_denied(
                role_connection,
                "ALTER SYSTEM SET " + custom_guc + " = '1'",
                "restricted role could ALTER SYSTEM")) {
            if (std::optional<diagnostic> cleanup = cleanup_security_probe(admin,
                    "ALTER SYSTEM RESET " + custom_guc, "ALTER SYSTEM probe")) {
                return cleanup;
            }
            return violation;
        }

        const std::filesystem::path tablespace_directory =
            std::filesystem::path(socket_directory) / ("tablespace_" + suffix);
        if (!tablespace_directory.is_absolute()) {
            return make_isolation_error(
                "private socket directory is not absolute for the tablespace denial probe");
        }
        std::error_code path_error;
        const bool directory_created =
            std::filesystem::create_directory(tablespace_directory, path_error);
        if (!directory_created || path_error) {
            return make_isolation_error(
                "could not create the empty tablespace denial-probe directory");
        }
        std::optional<diagnostic> tablespace_violation = check_command_denied(
            role_connection,
            "CREATE TABLESPACE " + probe + " LOCATION "
                + role_connection.escape_literal(tablespace_directory.string()),
            "restricted role could CREATE TABLESPACE");
        if (tablespace_violation) {
            if (std::optional<diagnostic> cleanup = cleanup_security_probe(
                    admin,
                    "DROP TABLESPACE IF EXISTS "
                        + admin.escape_identifier(probe_name),
                    "CREATE TABLESPACE probe")) {
                return cleanup;  // Keep the referenced path intact if DROP failed.
            }
        }
        (void)std::filesystem::remove(tablespace_directory, path_error);
        if (path_error) {
            return make_isolation_error(
                "could not remove the tablespace denial-probe directory");
        }
        if (tablespace_violation) {
            return tablespace_violation;
        }

        if (std::optional<diagnostic> violation = check_command_denied(
                role_connection,
                "CREATE SUBSCRIPTION " + probe
                    + " CONNECTION 'dbname=postgres user=biosite_probe "
                      "password=biosite_probe' "
                      "PUBLICATION biosite_probe WITH "
                      "(connect = false, create_slot = false, enabled = false, "
                      "slot_name = NONE)",
                "restricted role could CREATE SUBSCRIPTION")) {
            if (std::optional<diagnostic> cleanup = cleanup_security_probe(
                    role_connection,
                    "DROP SUBSCRIPTION IF EXISTS " + probe,
                    "CREATE SUBSCRIPTION probe")) {
                return cleanup;
            }
            return violation;
        }
        if (std::optional<diagnostic> violation = check_command_denied(
                role_connection,
                "COPY (SELECT 1) TO PROGRAM '/bin/cat >/dev/null'",
                "restricted role could COPY TO PROGRAM")) {
            return violation;
        }
        if (std::optional<diagnostic> violation = check_command_denied(
                role_connection,
                "SET ROLE " + role_connection.escape_identifier(admin_role),
                "restricted role could SET ROLE to the cluster administrator")) {
            if (std::optional<diagnostic> cleanup = cleanup_security_probe(
                    role_connection, "RESET ROLE", "SET ROLE probe")) {
                return cleanup;
            }
            return violation;
        }
        return std::nullopt;
    }

    /// @brief Runs author teardown before administrative clone cleanup.
    /// @param settings Private-cluster connection settings.
    /// @param role Restricted example role.
    /// @param clone Per-example database.
    /// @param example Example whose teardown SQL may run.
    /// @param role_connection Existing example session, if still usable.
    static void run_isolated_teardown(
        postgres_isolation_settings const& settings,
        const std::string& role,
        const std::string& clone,
        sql_example const& example,
        bioseq::libpq::client_connection* role_connection) {
        if (example.teardown_sql.empty()) {
            return;
        }
        const std::chrono::milliseconds deadline = execution_deadline(example);
        if (role_connection != nullptr
            && role_connection->is_transaction_idle()) {
            try {
                (void)role_connection->execute(example.teardown_sql, deadline);
            } catch (const bioseq::libpq::command_error&) {
                // The clone is dropped next even when teardown fails.
            }
            return;
        }
        // Recovery: restore the clone/role so the example role can reconnect,
        // then run teardown once on a fresh role connection.
        try {
            bioseq::libpq::client_connection admin(
                settings.administrator_connection_info);
            const std::string quoted_clone = admin.escape_identifier(clone);
            const std::string quoted_role = admin.escape_identifier(role);
            try_cleanup_command(
                admin, "ALTER DATABASE " + quoted_clone + " ALLOW_CONNECTIONS true");
            try_cleanup_command(
                admin, "ALTER DATABASE " + quoted_clone + " CONNECTION LIMIT -1");
            try_cleanup_command(
                admin, "ALTER DATABASE " + quoted_clone + " IS_TEMPLATE false");
            try_cleanup_command(
                admin, "ALTER ROLE " + quoted_role + " LOGIN CONNECTION LIMIT -1");
            try_cleanup_command(
                admin, "REVOKE ALL ON DATABASE " + quoted_clone + " FROM PUBLIC");
            try_cleanup_command(admin, "GRANT CONNECT ON DATABASE " + quoted_clone
                + " TO " + quoted_role);
        } catch (const bioseq::libpq::command_error&) {
            return;  // Cleanup drops the clone and role when restoration fails.
        }
        try {
            bioseq::libpq::client_connection reconnect(bioseq::libpq::make_unix_socket_conninfo(
                settings.socket_directory, clone, role, settings.server_port));
            configure_tutorial_session(reconnect, example);
            (void)reconnect.execute(example.teardown_sql, deadline);
        } catch (const bioseq::libpq::command_error&) {
            // The clone is dropped next even when teardown fails.
        }
    }

    /// @brief Builds a unique lower-snake suffix for transient database objects.
    /// @param entity_id Stable tutorial-example identity.
    /// @return A bounded PostgreSQL-safe suffix with a process-local counter.
    [[nodiscard]] static std::string make_unique_object_suffix(
        std::string_view entity_id) {
        static int counter = 0;
        std::string clean;
        for (const char character : entity_id) {
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

    /// @brief Applies deterministic rendering and bounded-execution settings.
    /// @param connection Tutorial PostgreSQL session.
    /// @param example Example whose timeout bounds `statement_timeout`.
    static void configure_tutorial_session(
        bioseq::libpq::client_connection& connection, sql_example const& example) {
        long long timeout_ms = example.timeout.count();
        if (timeout_ms <= 0 || timeout_ms > 60'000) {
            timeout_ms = 60'000;
        }
        (void)connection.execute("SET TIME ZONE 'UTC'");
        (void)connection.execute("SET DateStyle = 'ISO, YMD'");
        (void)connection.execute("SET IntervalStyle = 'iso_8601'");
        (void)connection.execute("SET bytea_output = 'hex'");
        (void)connection.execute("SET extra_float_digits = 3");
        (void)connection.execute("SET client_min_messages = 'notice'");
        (void)connection.execute("SET search_path = public, bioseq, pg_catalog");
        (void)connection.execute("SET statement_timeout = " + std::to_string(timeout_ms));
        (void)connection.execute("SET lock_timeout = '2s'");
        (void)connection.execute("SET idle_in_transaction_session_timeout = '15s'");
    }

    /// @brief Captures visible results followed by an expected PostgreSQL error.
    /// @return A diagnostic for success, mismatched SQLSTATE, or result overflow.
    [[nodiscard]] static std::optional<diagnostic> capture_expected_error(
        bioseq::libpq::client_connection& connection, sql_example const& example,
        std::chrono::milliseconds deadline, sql_example_result& outcome) {
        const bioseq::libpq::command_batch_result visible_batch =
            connection.execute_batch_with_limits(
                example.visible_sql, deadline, visible_result_limits());
        if (!visible_batch.error) {
            diagnostic unexpected_success;
            unexpected_success.severity = diagnostic_severity::error;
            unexpected_success.operation = diagnostic_operation::sql_execution;
            unexpected_success.message =
                "example expected an error but the visible SQL succeeded";
            return unexpected_success;
        }
        const std::string sql_state(visible_batch.error->sql_state());
        if (example.expected_sqlstate && *example.expected_sqlstate != sql_state) {
            diagnostic mismatch;
            mismatch.severity = diagnostic_severity::error;
            mismatch.operation = diagnostic_operation::sql_execution;
            mismatch.code = sql_state;
            mismatch.message = "expected SQLSTATE " + *example.expected_sqlstate
                + " but got " + sql_state + ": " + visible_batch.error->what();
            return mismatch;
        }
        // Retain the results that completed before PostgreSQL stopped, in order.
        const sql_type_name_by_oid type_names =
            load_sql_type_names(connection, visible_batch.completed);
        for (const bioseq::libpq::command_result& command :
             visible_batch.completed) {
            outcome.visible_results.push_back(
                make_result_set(command, type_names));
        }
        if (std::optional<std::string> breach =
                check_result_limits(outcome.visible_results)) {
            diagnostic limit;
            limit.severity = diagnostic_severity::error;
            limit.operation = diagnostic_operation::sql_execution;
            limit.message = std::move(*breach);
            return limit;
        }
        sql_result_set captured;
        captured.shape = result_shape::error;
        captured.sqlstate = sql_state;
        captured.primary_message = visible_batch.error->what();
        captured.detail = std::string(visible_batch.error->detail());
        captured.hint = std::string(visible_batch.error->hint());
        outcome.visible_results.push_back(std::move(captured));
        return std::nullopt;
    }

    /// @brief Enforces retained and rendered payload limits.
    /// @param sets Visible result sets collected for one example.
    /// @return An actionable message for the first exceeded limit.
    [[nodiscard]] static std::optional<std::string> check_result_limits(
        const std::vector<sql_result_set>& sets) {
        constexpr std::size_t max_result_sets = 10;
        constexpr std::size_t max_rows = 200;
        constexpr std::size_t max_columns = 100;
        constexpr std::size_t max_cell_bytes = 64 * 1024;
        constexpr std::size_t max_total_bytes = 2 * 1024 * 1024;
        if (sets.size() > max_result_sets) {
            return "visible SQL produced more than 10 result sets; split the example";
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

    using sql_type_name_by_oid = std::map<std::uint32_t, std::string>;

    /// @brief Resolves result-column type names inside the producing database.
    /// @param connection Session that produced the result sets.
    /// @param command_results Native result sets whose column OIDs need names.
    /// @return A complete mapping from every observed type OID to display name.
    [[nodiscard]] static sql_type_name_by_oid load_sql_type_names(
        bioseq::libpq::client_connection& connection,
        const std::vector<bioseq::libpq::command_result>& command_results) {
        std::set<std::uint32_t> oids;
        for (const bioseq::libpq::command_result& command : command_results) {
            if (!command.contains_rows()) {
                continue;
            }
            for (std::size_t column = 0;
                 column < command.column_count(); ++column) {
                oids.insert(command.column_type_oid(column));
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
        const bioseq::libpq::command_result resolved =
            connection.execute(sql, std::chrono::seconds(5));
        if (!resolved.contains_rows()
            || resolved.row_count() != oids.size()) {
            throw bioseq::libpq::command_error(
                "same-clone PostgreSQL type-name lookup returned an incomplete result",
                std::string{});
        }

        sql_type_name_by_oid names;
        std::set<std::uint32_t>::const_iterator oid = oids.begin();
        for (std::size_t row = 0; row < resolved.row_count(); ++row, ++oid) {
            if (resolved.cell_is_null(row, 0)) {
                throw bioseq::libpq::command_error(
                    "same-clone PostgreSQL type-name lookup returned NULL",
                    std::string{});
            }
            names.emplace(*oid, std::string(resolved.text_value(row, 0)));
        }
        return names;
    }

    /// @brief Projects a native libpq result onto the website SQL result model.
    /// @param command Native command or row result.
    /// @param type_names Same-database type-name mapping.
    /// @return A source-neutral result set ready for rendering.
    [[nodiscard]] static sql_result_set make_result_set(
        const bioseq::libpq::command_result& command,
        const sql_type_name_by_oid& type_names) {
        sql_result_set set;
        if (command.contains_rows()) {
            set.shape = result_shape::rows;
            for (std::size_t column = 0;
                 column < command.column_count(); ++column) {
                const std::uint32_t type_oid = command.column_type_oid(column);
                const sql_type_name_by_oid::const_iterator type_name =
                    type_names.find(type_oid);
                if (type_name == type_names.end()) {
                    throw bioseq::libpq::command_error(
                        "same-clone PostgreSQL type-name lookup omitted an OID",
                        std::string{});
                }
                set.columns.push_back(sql_column{
                    .name = command.column_name(column),
                    .type_oid = type_oid,
                    .type_name = type_name->second});
            }
            for (std::size_t row = 0; row < command.row_count(); ++row) {
                std::vector<sql_cell> cells;
                cells.reserve(command.column_count());
                for (std::size_t column = 0;
                     column < command.column_count(); ++column) {
                    cells.push_back(sql_cell{
                        .is_null = command.cell_is_null(row, column),
                        .text = std::string(command.text_value(row, column))});
                }
                set.rows.push_back(std::move(cells));
            }
        } else {
            set.shape = result_shape::command;
            set.command_tag = command.command_tag();
            set.affected_rows = command.affected_row_count();
        }
        return set;
    }

    /// @brief Projects a libpq command error onto the website diagnostic model.
    [[nodiscard]] static diagnostic make_command_diagnostic(
        diagnostic_operation operation,
        const bioseq::libpq::command_error& error) {
        diagnostic result;
        result.severity = diagnostic_severity::error;
        result.operation = operation;
        result.code = std::string(error.sql_state());
        result.message = error.what();
        return result;
    }

    std::string connection_info_;
};

static_assert(sql_example_executor<postgres_tutorial_executor>,
              "postgres_tutorial_executor must model sql_example_executor");

}  // namespace bioseq::codegen::website
