/** @file Safety runtime activation command family. */

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

namespace bioseq::tests::integration::safety_runtime_harness {

/// Install sealed artifacts, restart PostgreSQL, and emit an activation receipt.
[[nodiscard]] int activate(
        const command_line& command) {
    require_exact_options(
        command,
        {"--profile", "--cluster-config", "--dso", "--output"});
    const std::string profile =
        require_option(command, "--profile");
    if (profile != k_profile) {
        fail("activation", "requested profile differs from executable");
    }
    const fs::path config_path =
        fs::absolute(
            require_option(command, "--cluster-config"))
            .lexically_normal();
    const fs::path requested_dso =
        fs::absolute(require_option(command, "--dso"))
            .lexically_normal();
    const fs::path output =
        fs::absolute(require_option(command, "--output"))
            .lexically_normal();
    const std::string config_raw = read_file(config_path);
    const json config =
        parse_json(config_raw, "cluster-config");
    validate_cluster_config(config, profile);
    if (!fs::is_regular_file(requested_dso)
        || fs::is_symlink(requested_dso)) {
        fail(
            "activation", "requested DSO must be a regular non-symlink file");
    }
    const fs::path control_source{k_control_file};
    const fs::path sql_source{k_sql_file};
    const fs::path control_dso_source{k_control_dso};
    const sql_manifest_contract manifest =
        load_sql_manifest();
    if (!control_source.is_absolute()
        || !sql_source.is_absolute()
        || !control_dso_source.is_absolute()
        || !fs::is_regular_file(control_source)
        || !fs::is_regular_file(sql_source)
        || !fs::is_regular_file(control_dso_source)
        || fs::is_symlink(control_source)
        || fs::is_symlink(sql_source)
        || fs::is_symlink(control_dso_source)) {
        fail(
            "activation",
            "compiled control, SQL, manifest, or helper inputs "
            "are unavailable");
    }
    const fs::path installed_dso = require_absolute_path(
        config, "installed_dso_path", "cluster-config");
    const fs::path installed_control_dso = require_absolute_path(
        config, "installed_control_dso_path", "cluster-config");
    const fs::path installed_control = require_absolute_path(
        config, "installed_control_path", "cluster-config");
    const fs::path installed_sql = require_absolute_path(
        config, "installed_sql_path", "cluster-config");
    const std::string nonce = random_token();
    const std::string dso_sha256 =
        sha256_file(requested_dso);
    const std::string control_sha256 =
        sha256_file(control_source);
    const std::string sql_sha256 =
        sha256_file(sql_source);
    const std::string control_dso_sha256 =
        sha256_file(control_dso_source);
    if (sql_sha256 != manifest.script_sha256) {
        fail(
            "activation",
            "generated SQL differs from its manifest");
    }
    install_artifact(
        requested_dso,
        installed_dso,
        dso_sha256,
        nonce.substr(0, 16));
    install_artifact(
        control_source,
        installed_control,
        control_sha256,
        nonce.substr(16, 16));
    install_artifact(
        sql_source,
        installed_sql,
        sql_sha256,
        nonce.substr(32, 16));
    install_artifact(
        control_dso_source,
        installed_control_dso,
        control_dso_sha256,
        nonce.substr(48, 16));
    run_command(
        require_field(
            config, "restart_command", "cluster-config"),
        "cluster restart");
    const connection_parameters parameters =
        parse_connection(config);
    std::unique_ptr<pg_connection> connection =
        connect_with_retry(parameters);
    const json initial_cluster =
        query_cluster_identity(*connection);
    validate_private_cluster_identity(
        initial_cluster, config);
    require_success(
        connection->execute(
            "DROP EXTENSION IF EXISTS bioseq CASCADE;"),
        "drop extension");
    require_success(
        connection->execute(
            "DROP SCHEMA IF EXISTS bioseq_internal CASCADE;"),
        "drop internal schema");
    require_success(
        connection->execute(
            "DROP SCHEMA IF EXISTS bioseq CASCADE;"),
        "drop public schema");
    require_success(
        connection->execute("CREATE EXTENSION bioseq;"),
        "create extension");
    require_success(
        connection->execute(
            "LOAD " + sql_literal(installed_dso.string()) + ";"),
        "load requested DSO");
    if (sha256_file(installed_dso) != dso_sha256) {
        fail(
            "activation", "active DSO bytes changed after PostgreSQL load");
    }
    const json cluster = query_cluster_identity(*connection);
    validate_private_cluster_identity(cluster, config);
    create_guc_contract_binding(
        *connection, installed_control_dso);
    const installed_guc_profile guc_profile =
        verify_installed_guc_profile(*connection, manifest.gucs);
    const std::uint64_t extension_oid =
        verify_installed_catalog(*connection, manifest);
    const json receipt{
        {"schema_revision", 1},
        {"profile", profile},
        {"case_plan_sha256", k_case_plan_sha256},
        {"test_executable_sha256", self_sha256(command)},
        {"requested_dso_sha256", dso_sha256},
        {"cluster_config_sha256", sha256_bytes(config_raw)},
        {"active_dso_path",
         fs::absolute(installed_dso).lexically_normal().string()},
        {"active_dso_sha256", dso_sha256},
        {"cluster_system_identifier",
         cluster.at("cluster_system_identifier")},
        {"cluster_data_directory",
         cluster.at("cluster_data_directory")},
        {"postmaster_start_time",
         cluster.at("postmaster_start_time")},
        {"activation_nonce", nonce},
        {"server_version_num",
         cluster.at("server_version_num")},
        {"extension_oid",
         extension_oid},
        {"bioseq_guc_count",
         guc_profile.counts.total},
        {"bioseq_test_only_guc_count",
         guc_profile.counts.test_only},
        {"bioseq_guc_properties",
         guc_profile.properties},
        {"bioseq_guc_properties_sha256",
         guc_profile.properties_sha256},
        {"installed_control_sha256", control_sha256},
        {"installed_sql_sha256", sql_sha256},
        {"sql_manifest_sha256", manifest.sha256},
        {"sql_script_sha256", manifest.script_sha256},
        {"control_dso_path",
         fs::canonical(installed_control_dso).string()},
        {"control_dso_sha256", control_dso_sha256}};
    write_atomic(output, canonical_json(receipt));
    return 0;
}

}  // namespace bioseq::tests::integration::safety_runtime_harness
