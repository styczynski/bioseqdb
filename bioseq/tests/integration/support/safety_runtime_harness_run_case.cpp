/**
 * @file safety_runtime_harness_run_case.cpp
 * @brief Runs runtime-safety cases and records their PostgreSQL results.
 */

module;

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/ranges.h>
#include <libpq-fe.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <spawn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

module bioseq.tests.integration.safety_runtime_harness_support;

import :scenario_support;

namespace bioseq::tests::integration::safety_runtime_harness {

/// Check the request against the installed test build, run its runtime-safety
/// case, and verify cleanup and recovery before writing the result file.
[[nodiscard]] int run_case(
        const command_line& command) {
    require_exact_options(command, {"--request", "--output"});
    const fs::path request_path =
        fs::absolute(require_option(command, "--request"))
            .lexically_normal();
    const fs::path output =
        fs::absolute(require_option(command, "--output"))
            .lexically_normal();
    const std::string request_raw = read_file(request_path);
    const std::string request_sha256 =
        sha256_bytes(request_raw);
    const json request =
        parse_json(request_raw, "request");
    require_exact_fields(
        request,
        {
            "schema_revision",
            "profile",
            "case_id",
            "sql_identity",
            "site_id",
            "ledger_site_sha256",
            "invocation_key",
            "fault_key",
            "relation",
            "probe_contract",
            "case_plan_path",
            "case_plan_sha256",
            "ledger_path",
            "ledger_sha256",
            "sql_manifest_path",
            "sql_manifest_sha256",
            "active_dso_path",
            "active_dso_sha256",
            "test_executable_sha256",
            "producer_executable_path",
            "producer_executable_sha256",
            "cluster_config_path",
            "cluster_config_sha256",
            "stage_manifest_path",
            "stage_manifest_sha256",
            "tool_signature_suite_sha256",
            "runtime_control_dso_sha256",
            "activation_receipt_path",
            "activation_receipt_sha256",
            "catalog_path",
            "catalog_sha256",
            "catalog_entry_sha256",
        },
        "request");
    if (require_field(
            request, "schema_revision", "request") != 2
        || require_string_field(
               request, "profile", "request") != k_profile
        || require_string_field(
               request,
               "case_plan_sha256",
               "request")
            != k_case_plan_sha256) {
        fail(
            "request",
            "schema, profile, or compiled case-plan binding differs");
    }
    const std::string case_id_value =
        require_string_field(request, "case_id", "request");
    const case_plan plan = load_case_plan();
    const auto plan_case = plan.cases.find(case_id_value);
    if (plan_case == plan.cases.end()) {
        fail("request", "case is absent from compiled plan");
    }
    const json& planned_catalog =
        plan_case->second.catalog;
    const json& execution = plan_case->second.execution;
    require_equal_field(
        request, planned_catalog, "sql_identity", "request");
    require_equal_field(
        request, planned_catalog, "site_id", "request");
    require_equal_field(
        request,
        planned_catalog,
        "ledger_site_sha256",
        "request");
    require_equal_field(
        request, planned_catalog, "invocation_key", "request");
    require_equal_field(
        request, planned_catalog, "fault_key", "request");
    require_equal_field(
        request, planned_catalog, "relation", "request");
    require_equal_field(
        request, planned_catalog, "probe_contract", "request");

    const std::string executable_sha256 =
        self_sha256(command);
    if (require_string_field(
            request,
            "test_executable_sha256",
            "request")
        != executable_sha256) {
        fail(
            "request",
            "test executable hash differs from running bytes");
    }
    verify_file_binding(
        request,
        "case_plan_path",
        "case_plan_sha256",
        "case plan");
    if (fs::canonical(fs::path{
            require_string_field(
                request, "case_plan_path", "request")})
        != fs::canonical(fs::path{k_case_plan_path})) {
        fail(
            "case plan",
            "request does not name the executable's compiled plan");
    }
    verify_file_binding(
        request,
        "producer_executable_path",
        "producer_executable_sha256",
        "runtime producer");
    verify_file_binding(
        request,
        "ledger_path",
        "ledger_sha256",
        "safety ledger");
    verify_file_binding(
        request,
        "sql_manifest_path",
        "sql_manifest_sha256",
        "SQL manifest");
    verify_file_binding(
        request,
        "catalog_path",
        "catalog_sha256",
        "case catalog");
    verify_file_binding(
        request,
        "cluster_config_path",
        "cluster_config_sha256",
        "cluster config");
    const fs::path request_manifest_path{
        require_string_field(
            request, "sql_manifest_path", "request")};
    const sql_manifest_contract manifest =
        load_sql_manifest();
    if (fs::canonical(request_manifest_path)
            != fs::canonical(fs::path{k_sql_manifest})
        || require_string_field(
               request,
               "sql_manifest_sha256",
               "request")
            != manifest.sha256) {
        fail(
            "SQL manifest",
            "request is not bound to the executable's compiled manifest");
    }

    const fs::path active_dso{
        require_string_field(
            request, "active_dso_path", "request")};
    const std::string active_dso_sha256 =
        require_string_field(
            request, "active_dso_sha256", "request");
    if (!active_dso.is_absolute()
        || !fs::is_regular_file(active_dso)
        || fs::is_symlink(active_dso)
        || !lowercase_sha256(active_dso_sha256)
        || sha256_file(active_dso) != active_dso_sha256) {
        fail(
            "active DSO",
            "path or independently observed bytes differ");
    }

    const fs::path config_path{
        require_string_field(
            request, "cluster_config_path", "request")};
    const std::string config_raw = read_file(config_path);
    const json config =
        parse_json(config_raw, "cluster-config");
    validate_cluster_config(config, k_profile);
    if (require_absolute_path(
            config, "installed_dso_path", "cluster-config")
            .lexically_normal()
        != active_dso.lexically_normal()) {
        fail(
            "active DSO",
            "cluster configuration names a different installed path");
    }
    const std::string config_sha256 =
        sha256_bytes(config_raw);
    if (config_sha256
        != require_string_field(
            request, "cluster_config_sha256", "request")) {
        fail("cluster config", "bytes changed after request publication");
    }
    const activation_receipt activation =
        load_activation_receipt(
            request,
            active_dso,
            active_dso_sha256,
            config_sha256,
            executable_sha256,
            config,
            manifest);

    const fs::path catalog_path{
        require_string_field(
            request, "catalog_path", "request")};
    const json catalog = load_json(
        catalog_path, "catalog");
    if (require_string_field(
            catalog,
            "test_executable_sha256",
            "catalog")
        != executable_sha256) {
        fail("catalog", "test executable binding differs");
    }
    const json& published_entry =
        catalog_entry(catalog, case_id_value);
    if (published_entry != planned_catalog) {
        fail(
            "catalog",
            "published case differs from compiled case plan");
    }
    const std::string catalog_entry_sha256 =
        sha256_bytes(canonical_json(planned_catalog));
    if (catalog_entry_sha256
        != require_string_field(
            request, "catalog_entry_sha256", "request")) {
        fail("catalog", "catalog-entry SHA-256 differs");
    }
    const json& expected = require_field(
        planned_catalog, "expected", "catalog-entry");
    const std::string expected_backend_outcome =
        require_string_field(
            expected,
            "backend_outcome",
            "catalog-entry.expected");
    const bool backend_exit =
        expected_backend_outcome == "backend_exit";

    const fs::path ledger_path{
        require_string_field(
            request, "ledger_path", "request")};
    const json ledger =
        load_json(ledger_path, "ledger");
    const std::string site_id =
        require_string_field(request, "site_id", "request");
    const std::string ledger_site_sha256 =
        require_string_field(
            request, "ledger_site_sha256", "request");
    static_cast<void>(
        resolve_ledger_site(
            ledger,
            planned_catalog,
            ledger_site_sha256));
    if (require_string_field(
            ledger, "sql_manifest_sha256", "ledger")
        != require_string_field(
            request, "sql_manifest_sha256", "request")) {
        fail(
            "ledger", "SQL manifest binding differs from request");
    }

    const connection_parameters parameters =
        parse_connection(config);
    pg_connection connection{parameters};
    const json cluster = query_cluster_identity(connection);
    validate_private_cluster_identity(cluster, config);
    static const std::array<std::string_view, 4>
        cluster_fields{
            "cluster_system_identifier",
            "cluster_data_directory",
            "postmaster_start_time",
            "server_version_num"};
    for (const std::string_view field : cluster_fields) {
        if (require_field(
                cluster, field, "cluster-identity")
            != require_field(
                activation.value,
                field,
                "activation-receipt")) {
            fail(
                "cluster-identity",
                fmt::format("{} differs from activation", field));
        }
    }
    if (sha256_file(active_dso) != active_dso_sha256) {
        fail(
            "active DSO",
            "bytes changed before case execution");
    }
    const std::uint64_t extension_oid =
        verify_installed_catalog(connection, manifest);
    create_guc_contract_binding(
        connection, activation.control_dso_path);
    const installed_guc_profile guc_profile =
        verify_installed_guc_profile(connection, manifest.gucs);
    if (extension_oid
        != require_field(
               activation.value,
               "extension_oid",
               "activation-receipt")
               .get<std::uint64_t>()) {
        fail(
            "installed C routine catalog",
            "extension OID differs from activation");
    }
    if (guc_profile.counts.total
            != parse_unsigned(
                require_field(
                    activation.value,
                    "bioseq_guc_count",
                    "activation-receipt")
                    .dump(),
                "activation-receipt.bioseq_guc_count")
        || guc_profile.counts.test_only
            != parse_unsigned(
                require_field(
                    activation.value,
                    "bioseq_test_only_guc_count",
                    "activation-receipt")
                    .dump(),
                "activation-receipt.bioseq_test_only_guc_count")
        || guc_profile.properties
            != require_field(
                activation.value,
                "bioseq_guc_properties",
                "activation-receipt")
        || guc_profile.properties_sha256
            != require_string_field(
                activation.value,
                "bioseq_guc_properties_sha256",
                "activation-receipt")) {
        fail(
            "installed GUC profile",
            "observed counts differ from activation");
    }
    if (sha256_file(activation.control_dso_path)
            != activation.control_dso_sha256
        || activation.control_dso_sha256
            != require_string_field(
                activation.value,
                "control_dso_sha256",
                "activation-receipt")) {
        fail(
            "safety runtime control DSO",
            "helper bytes changed before case execution");
    }
    create_probe_bindings(
        connection, activation.control_dso_path);
    require_idle(connection, "probe binding setup");

    const std::vector<std::string> setup_sql =
        sql_array(
            execution, "setup_sql", "case.execution", false);
    const std::vector<std::string> post_error_sql =
        sql_array(
            execution,
            "post_error_sql",
            "case.execution",
            false);
    const std::vector<std::string> cleanup_sql =
        sql_array(
            execution, "cleanup_sql", "case.execution", false);
    execute_sql_list(
        connection,
        setup_sql,
        "case setup",
        backend_exit);
    if (
        backend_exit
        && connection.transaction_status() != PQTRANS_INTRANS) {
        fail(
            "case setup",
            "backend-exit setup must leave one rollback-owned transaction");
    }
    if (verify_installed_catalog(connection, manifest)
        != extension_oid) {
        fail(
            "case setup",
            "setup changed the exact extension catalog binding");
    }
    if (execution.contains("pre_fault_control_sql")) {
        const std::vector<std::string> control_sql =
            sql_array(
                execution,
                "pre_fault_control_sql",
                "case.execution",
                true);
        execute_sql_list(
            connection,
            control_sql,
            "pre-fault success control");
        const pg_result control_recovery_result =
            connection.execute(
                require_string_field(
                    execution,
                    "pre_fault_control_recovery_sql",
                    "case.execution"));
        const json observed_control_rows =
            result_rows_json(
                rows(
                    control_recovery_result,
                    "pre-fault success-control recovery"));
        require_idle(
            connection,
            "pre-fault success-control recovery");
        if (observed_control_rows
            != require_field(
                execution,
                "pre_fault_control_recovery_expected_rows",
                "case.execution")) {
            fail(
                "pre-fault success-control recovery",
                "exact result rows differ from the compiled expectation");
        }
        if (verify_installed_catalog(connection, manifest)
            != extension_oid) {
            fail(
                "pre-fault success control",
                "control changed the exact extension catalog binding");
        }
    }

    const std::uint64_t target_oid =
        resolve_target_oid(
            connection,
            require_string_field(
                execution,
                "target_regprocedure",
                "case.execution"));
    const std::string token = random_token();
    const std::string fault_key =
        require_string_field(
            planned_catalog, "fault_key", "catalog-entry");
    const std::string relation =
        require_string_field(
            planned_catalog, "relation", "catalog-entry");
    const json& probe_contract =
        require_field(
            planned_catalog,
            "probe_contract",
            "catalog-entry");
    const fs::path marker_directory = require_absolute_path(
        config, "terminal_marker_directory", "cluster-config");
    const fs::path server_log_path = require_absolute_path(
        config, "server_log_path", "cluster-config");
    const fs::path marker_path =
        marker_directory
        / fmt::format("terminal-{}.marker", token);
    require_terminal_marker_absent(marker_path);
    const std::optional<std::string_view> expected_guard_kind =
        terminal_guard_kind(fault_key);
    if (backend_exit != expected_guard_kind.has_value()) {
        fail(
            "terminal causality",
            "backend-exit case and terminal guard fault differ");
    }
    const std::optional<server_log_checkpoint> log_checkpoint =
        backend_exit
        ? std::optional<server_log_checkpoint>{
              capture_server_log(server_log_path)}
        : std::nullopt;
    const std::array<std::string, 8> arm_parameters{
        std::to_string(target_oid),
        token,
        site_id,
        fault_key,
        relation,
        std::string{k_profile},
        fs::canonical(active_dso).string(),
        fs::canonical(marker_directory).string()};
    require_success(
        connection.execute_parameters(
            "SELECT pg_temp.bioseq_safety_runtime_probe_arm("
            "$1::oid, $2::text, $3::text, $4::text, "
            "$5::text, $6::text, $7::text, $8::text);",
            arm_parameters),
        "arm safety runtime probe");
    if (backend_exit) {
        if (connection.transaction_status() != PQTRANS_INTRANS) {
            fail(
                "arm safety runtime probe",
                "terminal probe arm escaped its rollback transaction");
        }
    } else {
        require_idle(connection, "arm safety runtime probe");
    }

    const std::string invocation_sql =
        require_string_field(
            execution, "invocation_sql", "case.execution");
    const std::uint64_t initial_backend_pid =
        static_cast<std::uint64_t>(connection.backend_pid());
    if (initial_backend_pid == 0) {
        fail("case invocation", "initial backend PID is invalid");
    }
    const pg_result invocation =
        connection.execute(invocation_sql);
    std::optional<std::string> observed_sqlstate;
    std::optional<std::string> observed_message;
    std::string observed_backend_outcome;
    std::string post_invocation_connection_status;
    std::string invocation_result_status;
    std::optional<json> terminal_marker_evidence;
    std::optional<json> terminal_server_log_evidence;
    if (backend_exit) {
        if (connection.connection_status() != CONNECTION_BAD) {
            fail(
                "case invocation",
                "terminal cleanup guard did not close the backend connection");
        }
        observed_backend_outcome = "backend_exit";
        post_invocation_connection_status = "bad";
        invocation_result_status = "connection_lost";
        terminal_marker_evidence.emplace(
            read_terminal_marker(
                marker_path,
                token,
                site_id,
                fault_key,
                *expected_guard_kind,
                initial_backend_pid));
        terminal_server_log_evidence.emplace(
            wait_for_terminal_server_log(
                server_log_path,
                *log_checkpoint,
                initial_backend_pid));
    } else {
        if (invocation.get() == nullptr
            || PQresultStatus(invocation.get())
                != PGRES_FATAL_ERROR
            || connection.connection_status()
                != CONNECTION_OK) {
            fail(
                "case invocation",
                "expected one recoverable PostgreSQL ERROR");
        }
        const char* severity = PQresultErrorField(
            invocation.get(), PG_DIAG_SEVERITY_NONLOCALIZED);
        const char* sqlstate = PQresultErrorField(
            invocation.get(), PG_DIAG_SQLSTATE);
        const char* message = PQresultErrorField(
            invocation.get(), PG_DIAG_MESSAGE_PRIMARY);
        if (severity == nullptr
            || std::string_view{severity} != "ERROR"
            || sqlstate == nullptr
            || message == nullptr) {
            fail(
                "case invocation",
                "PostgreSQL ERROR fields are incomplete");
        }
        observed_sqlstate.emplace(sqlstate);
        observed_message.emplace(message);
        observed_backend_outcome = "recoverable_error";
        post_invocation_connection_status = "ok";
        invocation_result_status = "postgres_error";
        require_idle(connection, "case invocation recovery");
        if (*observed_sqlstate
                != require_string_field(
                    expected,
                    "sqlstate",
                    "catalog-entry.expected")
            || *observed_message
                != require_string_field(
                    expected,
                    "message",
                    "catalog-entry.expected")) {
            fail(
                "case invocation",
                "SQLSTATE or primary message differs from compiled "
                "expectation");
        }
        require_terminal_marker_absent(marker_path);
    }

    std::unique_ptr<pg_connection> replacement_connection;
    const pg_connection* active_connection = &connection;
    json recovery_cluster = cluster;
    std::uint64_t recovery_backend_pid = initial_backend_pid;
    if (backend_exit) {
        replacement_connection =
            std::make_unique<pg_connection>(parameters);
        active_connection = replacement_connection.get();
        recovery_backend_pid = static_cast<std::uint64_t>(
            active_connection->backend_pid());
        if (recovery_backend_pid == 0
            || recovery_backend_pid == initial_backend_pid
            || active_connection->connection_status()
                != CONNECTION_OK) {
            fail(
                "backend-exit recovery",
                "replacement backend identity is not distinct and healthy");
        }
        recovery_cluster =
            query_cluster_identity(*active_connection);
        validate_private_cluster_identity(
            recovery_cluster, config);
        for (const std::string_view field : cluster_fields) {
            if (require_field(
                    recovery_cluster,
                    field,
                    "recovery-cluster-identity")
                != require_field(
                    activation.value,
                    field,
                    "activation-receipt")) {
                fail(
                    "backend-exit recovery",
                    fmt::format(
                        "{} differs from activation",
                        field));
            }
        }
        if (verify_installed_catalog(
                *active_connection, manifest)
            != extension_oid) {
            fail(
                "backend-exit recovery",
                "replacement backend has a different extension catalog");
        }
        create_guc_contract_binding(
            *active_connection,
            activation.control_dso_path);
        const installed_guc_profile recovery_guc_profile =
            verify_installed_guc_profile(
                *active_connection, manifest.gucs);
        if (recovery_guc_profile.counts.total
                != guc_profile.counts.total
            || recovery_guc_profile.counts.test_only
                != guc_profile.counts.test_only
            || recovery_guc_profile.properties
                != guc_profile.properties
            || recovery_guc_profile.properties_sha256
                != guc_profile.properties_sha256) {
            fail(
                "backend-exit recovery",
                "replacement backend has a different GUC profile");
        }
    }

    execute_sql_list(
        *active_connection, post_error_sql, "post-error SQL");
    const std::array<std::string, 1> token_parameter{token};
    std::optional<probe_observation> observation;
    const std::set<std::string, std::less<>>
        expected_sentinels = string_set(
            require_field(
                expected,
                "destructor_sentinels",
                "catalog-entry.expected"),
            "catalog-entry.expected.destructor_sentinels");
    const std::set<std::string, std::less<>>
        expected_cleanup = string_set(
            require_field(
                expected,
                "cleanup_events",
                "catalog-entry.expected"),
            "catalog-entry.expected.cleanup_events");
    const std::set<std::string, std::less<>>
        expected_postgres_cleanup = string_set(
            require_field(
                expected,
                "postgres_cleanup_events",
                "catalog-entry.expected"),
            "catalog-entry.expected.postgres_cleanup_events");
    if (backend_exit) {
        if (!expected_sentinels.empty()
            || !expected_cleanup.empty()
            || !expected_postgres_cleanup.empty()) {
            fail(
                "backend-exit evidence",
                "terminal cases cannot claim backend-local snapshot events");
        }
    } else {
        const pg_result snapshot_result =
            connection.execute_parameters(
                "SELECT pg_temp.bioseq_safety_runtime_probe_snapshot("
                "$1::text);",
                token_parameter);
        const auto snapshot_row =
            one_row(snapshot_result, "probe snapshot");
        observation.emplace(
            parse_probe_observation(
                nonnull_cell(
                    snapshot_row, 0, "probe snapshot")));
        verify_probe_observation(
            *observation,
            k_profile,
            active_dso,
            site_id,
            fault_key,
            relation,
            probe_contract,
            expected_sentinels,
            expected_cleanup,
            expected_postgres_cleanup);
        require_idle(connection, "probe snapshot");
        require_success(
            connection.execute_parameters(
                "SELECT pg_temp.bioseq_safety_runtime_probe_disarm("
                "$1::text);",
                token_parameter),
            "disarm safety runtime probe");
        require_idle(connection, "disarm safety runtime probe");
    }

    const std::string recovery_sql =
        require_string_field(
            execution, "recovery_sql", "case.execution");
    const pg_result recovery_result =
        active_connection->execute(recovery_sql);
    const auto recovery_rows =
        rows(recovery_result, "recovery probe");
    require_idle(*active_connection, "recovery probe");
    const json observed_recovery_rows =
        result_rows_json(recovery_rows);
    const json& expected_recovery_rows =
        require_field(
            execution,
            "recovery_expected_rows",
            "case.execution");
    if (observed_recovery_rows
        != expected_recovery_rows) {
        fail(
            "recovery probe",
            "exact result rows differ from the compiled expectation");
    }
    execute_sql_list(
        *active_connection, cleanup_sql, "case cleanup");
    if (sha256_file(active_dso) != active_dso_sha256) {
        fail(
            "active DSO",
            "bytes changed during case execution");
    }
    if (sha256_file(activation.control_dso_path)
        != activation.control_dso_sha256) {
        fail(
            "safety runtime control DSO",
            "helper bytes changed during case execution");
    }

    json destructor_events = json::array();
    json cleanup_events = json::array();
    json postgres_cleanup_events = json::array();
    if (observation.has_value()) {
        for (const auto& [name, count] :
             observation->destructor_events) {
            destructor_events.push_back(
                json{{"name", name}, {"count", count}});
        }
        for (const auto& [name, count] :
             observation->cleanup_events) {
            cleanup_events.push_back(
                json{{"name", name}, {"count", count}});
        }
        for (const auto& [name, count] :
             observation->postgres_cleanup_events) {
            postgres_cleanup_events.push_back(
                json{{"name", name}, {"count", count}});
        }
    }
    const bool connection_replaced = backend_exit;
    const std::string observation_mode =
        backend_exit ? "backend_exit" : "backend_snapshot";
    json terminal_causality = nullptr;
    bool terminal_guard_observed = false;
    if (backend_exit) {
        if (!terminal_marker_evidence.has_value()
            || !terminal_server_log_evidence.has_value()
            || recovery_cluster.at("postmaster_start_time")
                != cluster.at("postmaster_start_time")
            || recovery_cluster.at("cluster_system_identifier")
                != cluster.at("cluster_system_identifier")) {
            fail(
                "terminal causality",
                "durable marker, server log, or postmaster identity "
                "proof is incomplete");
        }
        terminal_causality = json{
            {"marker", *terminal_marker_evidence},
            {"server_log", *terminal_server_log_evidence},
            {"marker_directory", marker_directory.string()},
            {"server_log_path", server_log_path.string()},
            {"postmaster_identity_unchanged", true},
            {"fatal_panic_signal_core_rejected", true}};
        terminal_guard_observed = true;
    }
    const json result{
        {"schema_revision", 2},
        {"probe_abi_revision", 2},
        {"profile", k_profile},
        {"case_id", case_id_value},
        {"sql_identity",
         require_field(request, "sql_identity", "request")},
        {"site_id", site_id},
        {"ledger_site_sha256", ledger_site_sha256},
        {"invocation_key",
         require_field(request, "invocation_key", "request")},
        {"fault_key",
         require_field(request, "fault_key", "request")},
        {"relation", relation},
        {"probe_contract", probe_contract},
        {"case_plan_sha256", k_case_plan_sha256},
        {"ledger_sha256",
         require_field(request, "ledger_sha256", "request")},
        {"sql_manifest_sha256",
         require_field(
             request, "sql_manifest_sha256", "request")},
        {"active_dso_sha256", active_dso_sha256},
        {"test_executable_sha256", executable_sha256},
        {"producer_executable_sha256",
         require_field(
             request,
             "producer_executable_sha256",
             "request")},
        {"activation_receipt_sha256", activation.sha256},
        {"bioseq_guc_properties",
         guc_profile.properties},
        {"bioseq_guc_properties_sha256",
         guc_profile.properties_sha256},
        {"cluster_config_sha256", config_sha256},
        {"request_sha256", request_sha256},
        {"catalog_sha256",
         require_field(request, "catalog_sha256", "request")},
        {"catalog_entry_sha256", catalog_entry_sha256},
        {"execution_backend", "postgresql"},
        {"exercised_terminal_severity", false},
        {"backend_outcome",
         json{
             {"expected", expected_backend_outcome},
             {"observed", observed_backend_outcome},
             {"initial_backend_pid", initial_backend_pid},
             {"recovery_backend_pid", recovery_backend_pid},
             {"initial_connection_status", "ok"},
             {"post_invocation_connection_status",
              post_invocation_connection_status},
             {"invocation_result_status",
              invocation_result_status},
             {"terminal_guard_observed",
              terminal_guard_observed}}},
        {"terminal_causality", terminal_causality},
        {"destructor_events", std::move(destructor_events)},
        {"cleanup_events", std::move(cleanup_events)},
        {"postgres_cleanup_events",
         std::move(postgres_cleanup_events)},
        {"postgres_error",
         backend_exit
             ? json{
                   {"severity", nullptr},
                   {"sqlstate", nullptr},
                   {"message", nullptr}}
             : json{
                   {"severity", "ERROR"},
                   {"sqlstate", *observed_sqlstate},
                   {"message", *observed_message}}},
        {"fault_evidence",
         json{
             {"observation_mode", observation_mode},
             {"probe_abi_revision", 2},
             {"site_id",
              observation.has_value()
                  ? observation->site_id
                  : site_id},
             {"fault_key",
              observation.has_value()
                  ? observation->fault_key
                  : fault_key},
             {"relation",
              observation.has_value()
                  ? observation->relation
                  : relation},
             {"probe_contract", probe_contract},
             {"invocation_count",
              observation.has_value()
                  ? observation->start_count
                  : 0},
             {"fmgr_start_count",
              observation.has_value()
                  ? observation->start_count
                  : 0},
             {"fmgr_end_count",
              observation.has_value()
                  ? observation->end_count
                  : 0},
             {"fmgr_abort_count",
              observation.has_value()
                  ? observation->abort_count
                  : 0},
             {"site_visit_count",
              observation.has_value()
                  ? observation->site_visit_count
                  : 0},
             {"fault_trigger_count",
              observation.has_value()
                  ? observation->fault_trigger_count
                  : 0},
             {"site_visit_sequence",
              observation.has_value()
                  ? observation->site_visit_sequence
                  : 0},
             {"fault_trigger_sequence",
              observation.has_value()
                  ? observation->fault_trigger_sequence
                  : 0},
             {"invalid_count",
              observation.has_value()
                  ? observation->invalid_count
                  : 0},
             {"overflow_count",
              observation.has_value()
                  ? observation->overflow_count
                  : 0},
             {"inside_target",
              observation.has_value()
                  ? observation->inside_target
                  : false}}},
        {"recovery_probe",
         json{
             {"postcondition",
              require_field(
                  expected,
                  "recovery_postcondition",
                  "catalog-entry.expected")},
             {"passed", true},
             {"transaction_status", "idle"},
             {"nonempty_result", !recovery_rows.empty()},
             {"expected_rows", expected_recovery_rows},
             {"observed_rows", observed_recovery_rows},
             {"result_row_count", recovery_rows.size()}}},
        {"cluster_evidence",
         json{
             {"cluster_system_identifier",
              recovery_cluster.at(
                  "cluster_system_identifier")},
             {"cluster_data_directory",
              recovery_cluster.at(
                  "cluster_data_directory")},
             {"postmaster_start_time",
              recovery_cluster.at("postmaster_start_time")},
             {"activation_nonce",
              activation.value.at("activation_nonce")},
             {"server_version_num",
              recovery_cluster.at("server_version_num")},
             {"backend_pid", initial_backend_pid},
             {"initial_backend_pid", initial_backend_pid},
             {"recovery_backend_pid", recovery_backend_pid},
             {"connection_replaced", connection_replaced},
             {"active_dso_path",
              fs::canonical(active_dso).string()},
             {"mapped_target_path",
              observation.has_value()
                  ? fs::canonical(
                        observation->mapped_target_path).string()
                  : fs::canonical(active_dso).string()},
             {"control_dso_path",
              activation.control_dso_path.string()},
             {"control_dso_sha256",
              activation.control_dso_sha256}}}};
    write_atomic(output, canonical_json(result));
    return 0;
}

}  // namespace bioseq::tests::integration::safety_runtime_harness
