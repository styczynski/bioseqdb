/**
 * @file safety_runtime_harness_case_support.cppm
 * @brief Validates the private test cluster and loads runtime-safety cases.
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

extern char** environ;

export module bioseq.tests.integration.safety_runtime_harness_support:case_support;

import :core_support;

export namespace bioseq::tests::integration::safety_runtime_harness {

/// Read and normalize an absolute path field from a JSON object.
[[nodiscard]] fs::path require_absolute_path(
        const json& object,
        std::string_view field,
        std::string_view context) {
    fs::path result{
        require_string_field(object, field, context)};
    if (!result.is_absolute()) {
        fail(
            fmt::format("{}.{}", context, field),
            "path must be absolute");
    }
    return result.lexically_normal();
}

/// Check whether a canonicalized child remains strictly below a root.
[[nodiscard]] bool path_within(
        const fs::path& child,
        const fs::path& root) {
    const fs::path canonical_root =
        fs::canonical(root);
    const fs::path canonical_child =
        fs::weakly_canonical(child);
    const fs::path relative =
        canonical_child.lexically_relative(canonical_root);
    return !relative.empty()
        && relative != "."
        && !relative.is_absolute()
        && *relative.begin() != "..";
}

/// Check the private-cluster marker  contents, location, owner, and permissions.
void require_private_authorization(
        const fs::path& marker,
        const fs::path& expected_data_directory) {
    if (!marker.is_absolute()
        || !fs::is_regular_file(marker)
        || fs::is_symlink(marker)
        || !path_within(marker, expected_data_directory)
        || read_file(marker) != k_authorization_marker) {
        fail(
            "cluster-config.authorization_file",
            "private-cluster authorization marker is invalid");
    }
    struct stat metadata {};
    if (::stat(marker.c_str(), &metadata) != 0
        || metadata.st_uid != ::getuid()
        || (metadata.st_mode & 0777) != 0600) {
        fail(
            "cluster-config.authorization_file",
            "authorization marker must be owned by this uid with mode 0600");
    }
}

/// Check the private-cluster configuration before installing files or restarting it.
void validate_cluster_config(
        const json& config,
        std::string_view expected_profile) {
    require_exact_fields(
        config,
        {
            "schema_revision",
            "profile",
            "dedicated_safety_cluster",
            "connection",
            "private_stage_root",
            "expected_data_directory",
            "terminal_marker_directory",
            "server_log_path",
            "expected_system_identifier",
            "authorization_file",
            "installed_dso_path",
            "installed_control_dso_path",
            "installed_control_path",
            "installed_sql_path",
            "restart_command",
            "stage_manifest_path",
            "stage_manifest_sha256",
            "tool_signature_suite_sha256",
        },
        "cluster-config");
    if (require_field(
            config, "schema_revision", "cluster-config") != 1) {
        fail("cluster-config.schema_revision", "expected 1");
    }
    if (require_string_field(
            config, "profile", "cluster-config")
        != expected_profile) {
        fail("cluster-config.profile", "profile differs from executable");
    }
    const json& dedicated = require_field(
        config, "dedicated_safety_cluster", "cluster-config");
    if (!dedicated.is_boolean() || !dedicated.get<bool>()) {
        fail(
            "cluster-config.dedicated_safety_cluster",
            "destructive activation requires true");
    }
    const connection_parameters connection =
        parse_connection(config);
    const fs::path socket_directory{connection.host};
    if (!socket_directory.is_absolute()
        || !fs::is_directory(socket_directory)
        || fs::is_symlink(socket_directory)
        || connection.database != "bioseq_safety_runtime") {
        fail(
            "cluster-config.connection",
            "runtime evidence requires a local Unix socket and "
            "dedicated database");
    }
    const fs::path stage_root = require_absolute_path(
        config, "private_stage_root", "cluster-config");
    const fs::path data_directory = require_absolute_path(
        config, "expected_data_directory", "cluster-config");
    const fs::path authorization_file = require_absolute_path(
        config, "authorization_file", "cluster-config");
    const fs::path terminal_marker_directory =
        require_absolute_path(
            config,
            "terminal_marker_directory",
            "cluster-config");
    const fs::path server_log_path = require_absolute_path(
        config, "server_log_path", "cluster-config");
    if (!fs::is_directory(stage_root)
        || fs::is_symlink(stage_root)
        || !fs::is_directory(data_directory)
        || fs::is_symlink(data_directory)
        || !fs::is_directory(terminal_marker_directory)
        || fs::is_symlink(terminal_marker_directory)
        || fs::canonical(terminal_marker_directory)
            != terminal_marker_directory
        || terminal_marker_directory
            != fs::canonical(data_directory)
                / "bioseq-safety-terminal-markers"
        || !fs::is_regular_file(server_log_path)
        || fs::is_symlink(server_log_path)
        || fs::canonical(server_log_path) != server_log_path
        || server_log_path
            != fs::canonical(data_directory).parent_path()
                / "server_startup.log") {
        fail(
            "cluster-config",
            "private roots, terminal marker root, or server log differ");
    }
    struct stat marker_metadata {};
    struct stat log_metadata {};
    if (::lstat(
            terminal_marker_directory.c_str(),
            &marker_metadata) != 0
        || !S_ISDIR(marker_metadata.st_mode)
        || marker_metadata.st_uid != ::getuid()
        || (marker_metadata.st_mode & 0777) != 0700
        || ::lstat(server_log_path.c_str(), &log_metadata) != 0
        || !S_ISREG(log_metadata.st_mode)
        || log_metadata.st_uid != ::getuid()
        || log_metadata.st_nlink != 1) {
        fail(
            "cluster-config",
            "terminal marker root or server log metadata differs");
    }
    require_private_authorization(
        authorization_file, data_directory);
    for (const std::string_view field : {
             "installed_dso_path",
             "installed_control_dso_path",
             "installed_control_path",
             "installed_sql_path"}) {
        const fs::path path =
            require_absolute_path(config, field, "cluster-config");
        if (!path_within(path, stage_root)
            || fs::is_symlink(path.parent_path())) {
            fail(
                fmt::format("cluster-config.{}", field),
                "install destination escapes the private stage");
        }
    }
    const json& command = require_field(
        config, "restart_command", "cluster-config");
    if (!command.is_array()
        || command.empty()
        || command.size() > 32) {
        fail(
            "cluster-config.restart_command",
            "expected 1..32 command arguments");
    }
    for (std::size_t index = 0; index < command.size(); ++index) {
        static_cast<void>(require_string(
            command[index],
            fmt::format(
                "cluster-config.restart_command[{}]", index)));
    }
    const fs::path executable =
        command.front().get<std::string>();
    if (!executable.is_absolute()
        || !fs::is_regular_file(executable)
        || fs::is_symlink(executable)
        || !path_within(executable, stage_root)
        || ::access(executable.c_str(), X_OK) != 0) {
        fail(
            "cluster-config.restart_command[0]",
            "restart executable must be inside the private stage");
    }
    bool names_data_directory = false;
    for (const json& argument : command) {
        if (argument.get<std::string>()
            == data_directory.string()) {
            names_data_directory = true;
        }
    }
    if (!names_data_directory) {
        fail(
            "cluster-config.restart_command",
            "restart command does not name the authorized data directory");
    }
}

/// Run a previously validated command and requise a normal zero exit status.
void run_command(
        const json& command,
        std::string_view context) {
    std::vector<std::string> owned;
    owned.reserve(command.size());
    for (const json& argument : command) {
        owned.push_back(argument.get<std::string>());
    }
    std::vector<char*> arguments;
    arguments.reserve(owned.size() + 1);
    for (std::string& argument : owned) {
        arguments.push_back(argument.data());
    }
    arguments.push_back(nullptr);
    pid_t process = 0;
    const int spawn_status = posix_spawn(
        &process,
        owned.front().c_str(),
        nullptr,
        nullptr,
        arguments.data(),
        environ);
    if (spawn_status != 0) {
        fail(
            context,
            fmt::format(
                "posix_spawn failed: {}",
                std::strerror(spawn_status)));
    }
    int status = 0;
    while (::waitpid(process, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        fail(
            context,
            fmt::format(
                "waitpid failed: {}", std::strerror(errno)));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(
            context,
            WIFEXITED(status)
            ? fmt::format(
                "command exited {}", WEXITSTATUS(status))
            : "command did not exit normally");
    }
}

/// Copy an artifact, verify its SHA-256, and rename it into place.
void install_artifact(
        const fs::path& source,
        const fs::path& destination,
        std::string_view expected_sha256,
        std::string_view nonce) {
    if (!source.is_absolute()
        || !fs::is_regular_file(source)
        || fs::is_symlink(source)) {
        fail(source.string(), "source must be an absolute regular file");
    }
    if (!destination.is_absolute()
        || !fs::is_directory(destination.parent_path())) {
        fail(
            destination.string(),
            "destination parent must be an existing absolute directory");
    }
    if (fs::is_symlink(destination)
        || (fs::exists(destination)
            && !fs::is_regular_file(destination))) {
        fail(
            destination.string(),
            "destination must be absent or a regular non-symlink file");
    }
    const std::string source_sha256 = sha256_file(source);
    if (source_sha256 != expected_sha256) {
        fail(source.string(), "source hash changed before installation");
    }
    std::error_code equivalence_error;
    const bool same_file =
        fs::exists(destination)
        && fs::equivalent(
            source, destination, equivalence_error)
        && !equivalence_error;
    if (!same_file) {
        const fs::path temporary =
            destination.parent_path()
            / (destination.filename().string()
               + ".bioseq-safety." + std::string{nonce});
        if (fs::exists(temporary) || fs::is_symlink(temporary)) {
            fail(
                temporary.string(),
                "refuses to reuse an existing temporary path");
        }
        std::error_code copy_error;
        if (!fs::copy_file(
                source,
                temporary,
                fs::copy_options::none,
                copy_error)) {
            fail(
                temporary.string(),
                fmt::format(
                    "cannot copy artifact: {}",
                    copy_error.message()));
        }
        fs::permissions(
            temporary,
            fs::status(source).permissions());
        if (sha256_file(temporary) != expected_sha256) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            fail(
                temporary.string(),
                "copied artifact hash differs from source");
        }
        if (::rename(
                temporary.c_str(), destination.c_str()) != 0) {
            const int rename_error = errno;
            std::error_code ignored;
            fs::remove(temporary, ignored);
            fail(
                destination.string(),
                fmt::format(
                    "cannot atomically install artifact: {}",
                    std::strerror(rename_error)));
        }
    }
    if (sha256_file(destination) != expected_sha256) {
        fail(
            destination.string(),
            "installed artifact hash differs from requested bytes");
    }
}

/// Retry PostgreSQL connections while the restarted test server becomes ready.
[[nodiscard]] std::unique_ptr<pg_connection> connect_with_retry(
        const connection_parameters& parameters) {
    std::string last_error;
    for (int attempt = 0; attempt < 30; ++attempt) {
        try {
            return std::make_unique<pg_connection>(parameters);
        } catch (const std::runtime_error& error) {
            last_error = error.what();
            std::this_thread::sleep_for(
                std::chrono::milliseconds{200});
        }
    }
    fail(
        "postgres activation",
        fmt::format(
            "server did not become ready: {}", last_error));
}

/// Read a bounded array of SQL statements from a case definition.
[[nodiscard]] std::vector<std::string> sql_array(
        const json& object,
        std::string_view field,
        std::string_view context,
        bool required) {
    const auto found = object.find(std::string{field});
    if (found == object.end()) {
        if (required) {
            fail(
                context,
                fmt::format("missing field '{}'", field));
        }
        return {};
    }
    if (!found->is_array()
        || found->size() > k_maximum_sql_statements) {
        fail(
            fmt::format("{}.{}", context, field),
            "expected a bounded SQL string array");
    }
    std::vector<std::string> result;
    result.reserve(found->size());
    for (std::size_t index = 0; index < found->size(); ++index) {
        std::string sql = require_string(
            (*found)[index],
            fmt::format(
                "{}.{}[{}]", context, field, index));
        if (sql.size() > k_maximum_sql_bytes) {
            fail(
                fmt::format(
                    "{}.{}[{}]", context, field, index),
                "SQL statement exceeds the size bound");
        }
        result.push_back(std::move(sql));
    }
    return result;
}

/// Return whether a case identifier is safe for artifact names.
[[nodiscard]] bool case_id(std::string_view value) {
    static const std::regex pattern{
        "[a-z0-9][a-z0-9_.-]{0,127}"};
    return std::regex_match(
        value.begin(), value.end(), pattern);
}

/// Check the fault key's length and allowed characters.
[[nodiscard]] bool valid_fault_key(
        std::string_view value) noexcept {
    if (value.empty() || value.size() >= 129) {
        return false;
    }
    return std::ranges::all_of(
        value,
        [](const char character) {
            return (character >= 'a' && character <= 'z')
                || (character >= '0' && character <= '9')
                || character == '_'
                || character == '.';
        });
}

/// Check that a site ID is probe: followed by a lowercase SHA-256 digest.
[[nodiscard]] bool probe_site_id(
        std::string_view value) noexcept {
    constexpr std::string_view prefix = "probe:";
    return value.size() == prefix.size() + 64
        && value.substr(0, prefix.size()) == prefix
        && lowercase_sha256(value.substr(prefix.size()));
}

/// Expected site visits, fault triggers, and their order.
struct probe_contract_values final {
    std::uint64_t expected_visit_count;
    std::uint64_t expected_trigger_count;
    std::string_view ordering;
};

/// Get the visit and fault expectations for  a named ordering rule.
[[nodiscard]] probe_contract_values contract_for_relation(
        std::string_view relation,
        std::string_view context) {
    if (relation == "natural_at_site") {
        return {
            .expected_visit_count = 1,
            .expected_trigger_count = 0,
            .ordering = "visit_only"};
    }
    if (relation == "injected_routes_to_site") {
        return {
            .expected_visit_count = 1,
            .expected_trigger_count = 1,
            .ordering = "trigger_before_visit"};
    }
    if (relation == "injected_after_site_success") {
        return {
            .expected_visit_count = 1,
            .expected_trigger_count = 1,
            .ordering = "visit_before_trigger"};
    }
    fail(
        context,
        "relation must be natural_at_site, "
        "injected_routes_to_site, or "
        "injected_after_site_success");
}

/// Format the site/fault ordering rule as a probe_contract JSON object.
[[nodiscard]] json probe_contract_for_relation(
        std::string_view relation,
        std::string_view context) {
    const probe_contract_values values =
        contract_for_relation(relation, context);
    return json{
        {"abi_revision", std::uint64_t{2}},
        {"expected_visit_count",
         values.expected_visit_count},
        {"expected_trigger_count",
         values.expected_trigger_count},
        {"ordering", std::string{values.ordering}}};
}

/// Require probe_contract JSON to match the selected site/fault ordering rule.
void require_probe_contract(
        const json& value,
        std::string_view relation,
        std::string_view context) {
    static const std::set<std::string, std::less<>>
        allowed{
            "abi_revision",
            "expected_visit_count",
            "expected_trigger_count",
            "ordering"};
    if (!value.is_object()) {
        fail(context, "probe_contract must be an object");
    }
    for (const auto& [field, field_value] : value.items()) {
        static_cast<void>(field_value);
        if (!allowed.contains(field)) {
            fail(
                context,
                fmt::format(
                    "unknown probe_contract field '{}'",
                    field));
        }
    }
    if (value
        != probe_contract_for_relation(relation, context)) {
        fail(
            context,
            "probe_contract differs from the relation's ABI "
            "revision 2 contract");
    }
}

/// Validate a case's metadata and expected outcome, then return a copy.
[[nodiscard]] json normalized_catalog_entry(
        const json& raw,
        std::string_view context) {
    static const std::set<std::string, std::less<>> allowed{
        "case_id",
        "sql_identity",
        "site_id",
        "ledger_site_sha256",
        "invocation_key",
        "fault_key",
        "relation",
        "probe_contract",
        "postgres_severity",
        "expected"};
    if (!raw.is_object()) {
        fail(context, "catalog entry must be an object");
    }
    for (const auto& [field, value] : raw.items()) {
        static_cast<void>(value);
        if (!allowed.contains(field)) {
            fail(
                context,
                fmt::format(
                    "unknown catalog field '{}'", field));
        }
    }
    const json result = raw;
    const std::string id = require_string_field(
        result, "case_id", context);
    if (!case_id(id)) {
        fail(context, "case_id is not artifact-safe");
    }
    static_cast<void>(
        require_string_field(
            result, "sql_identity", context));
    const std::string site_id = require_string_field(
        result, "site_id", context);
    if (!probe_site_id(site_id)) {
        fail(context, "site_id is not a full ABI revision 2 probe ID");
    }
    const std::string ledger_site_sha256 =
        require_string_field(
            result, "ledger_site_sha256", context);
    if (!lowercase_sha256(ledger_site_sha256)) {
        fail(
            context,
            "ledger_site_sha256 is not a lowercase SHA-256");
    }
    static_cast<void>(
        require_string_field(
            result, "invocation_key", context));
    const std::string fault_key_value =
        require_string_field(
            result, "fault_key", context);
    if (!valid_fault_key(fault_key_value)) {
        fail(
            context,
            "fault_key must be 1-128 bytes from [a-z0-9_.]");
    }
    const std::string relation = require_string_field(
        result, "relation", context);
    require_probe_contract(
        require_field(result, "probe_contract", context),
        relation,
        fmt::format("{}.probe_contract", context));
    if (require_string_field(
            result, "postgres_severity", context)
        != "error") {
        fail(context, "terminal severity is forbidden at runtime");
    }
    const json& expected = require_field(
        result, "expected", context);
    static const std::set<std::string, std::less<>>
        expected_allowed{
            "backend_outcome",
            "destructor_sentinels",
            "cleanup_events",
            "postgres_cleanup_events",
            "sqlstate",
            "message",
            "recovery_postcondition"};
    if (!expected.is_object()) {
        fail(
            fmt::format("{}.expected", context),
            "expected an object");
    }
    for (const auto& [field, value] : expected.items()) {
        static_cast<void>(value);
        if (!expected_allowed.contains(field)) {
            fail(
                fmt::format("{}.expected", context),
                fmt::format(
                    "unknown expected field '{}'", field));
        }
    }
    const std::string backend_outcome =
        require_string_field(
            expected,
            "backend_outcome",
            fmt::format("{}.expected", context));
    if (backend_outcome != "recoverable_error"
        && backend_outcome != "backend_exit") {
        fail(
            context,
            "backend_outcome must be recoverable_error or backend_exit");
    }
    const bool backend_exit = backend_outcome == "backend_exit";
    const json& sentinels = require_field(
        expected, "destructor_sentinels",
        fmt::format("{}.expected", context));
    if (!sentinels.is_array()) {
        fail(
            fmt::format(
                "{}.expected.destructor_sentinels", context),
            "expected an array");
    }
    std::set<std::string, std::less<>> unique_sentinels;
    for (std::size_t index = 0;
         index < sentinels.size();
         ++index) {
        const std::string sentinel = require_string(
            sentinels[index],
            fmt::format(
                "{}.expected.destructor_sentinels[{}]",
                context,
                index));
        if (!unique_sentinels.insert(sentinel).second) {
            fail(context, "duplicate destructor sentinel");
        }
    }
    for (const std::string_view event_field : {
             "cleanup_events",
             "postgres_cleanup_events"}) {
        const json& events = require_field(
            expected,
            event_field,
            fmt::format("{}.expected", context));
        if (!events.is_array()) {
            fail(
                fmt::format(
                    "{}.expected.{}", context, event_field),
                "expected an array");
        }
        std::set<std::string, std::less<>> unique_events;
        for (std::size_t index = 0;
             index < events.size();
             ++index) {
            const std::string event_name = require_string(
                    events[index],
                    fmt::format(
                        "{}.expected.{}[{}]",
                        context,
                        event_field,
                        index));
            static const std::regex event_pattern{
                "[a-z0-9][a-z0-9_.]{0,63}"};
            if (!std::regex_match(
                    event_name.begin(),
                    event_name.end(),
                    event_pattern)
                || !unique_events.insert(event_name).second) {
                fail(context, "duplicate expected runtime event");
            }
        }
        if (backend_exit && !unique_events.empty()) {
            fail(
                context,
                "backend-exit cases cannot claim unobservable cleanup");
        }
        if (event_field == "postgres_cleanup_events"
            && !backend_exit) {
            if (fault_key_value
                    == k_backend_scoped_vacuum_fault_key) {
                if (!unique_events.empty()) {
                    fail(
                        context,
                        "backend-scoped VACUUM must expect no FMGR cleanup");
                }
            } else if (!unique_events.contains(
                           "fmgr_abort_observed")) {
                fail(
                    context,
                    "PostgreSQL cleanup evidence must include FMGR ABORT");
            }
        }
    }
    if (backend_exit && !unique_sentinels.empty()) {
        fail(
            context,
            "backend-exit cases cannot claim unobservable destructors");
    }
    const json& sqlstate_value = require_field(
        expected, "sqlstate",
        fmt::format("{}.expected", context));
    const json& message_value = require_field(
        expected, "message",
        fmt::format("{}.expected", context));
    if (backend_exit) {
        if (!sqlstate_value.is_null()
            || !message_value.is_null()) {
            fail(
                context,
                "backend-exit SQLSTATE/message must be null");
        }
    } else {
        const std::string sqlstate = require_string(
            sqlstate_value,
            fmt::format("{}.expected.sqlstate", context));
        if (sqlstate.size() != 5
            || !std::ranges::all_of(
                sqlstate,
                [](char character) {
                    return (character >= '0' && character <= '9')
                        || (character >= 'A'
                            && character <= 'Z');
                })) {
            fail(context, "expected SQLSTATE must have five characters");
        }
        static_cast<void>(require_string(
            message_value,
            fmt::format("{}.expected.message", context)));
    }
    static_cast<void>(require_string_field(
        expected, "recovery_postcondition",
        fmt::format("{}.expected", context)));
    return result;
}

/// Public case metadata and the SQL steps used to run it
struct case_definition final {
    json catalog;
    json execution;
};

/// Runtime cases indexed by the identifiers used in output filenames.
struct case_plan final {
    std::map<std::string, case_definition, std::less<>> cases;
};

/// Load the case plan and require it to match the hash compiled into this harness.
/// Validate each case, including its SQL steps and expected recovery rows.
[[nodiscard]] case_plan load_case_plan() {
    const fs::path path{k_case_plan_path};
    if (!path.is_absolute() || !fs::is_regular_file(path)) {
        fail(
            "case-plan", "compiled case-plan path is not a regular file");
    }
    const std::string raw = read_file(path);
    if (!lowercase_sha256(k_case_plan_sha256)
        || sha256_bytes(raw) != k_case_plan_sha256) {
        fail("case-plan", "bytes differ from compiled SHA-256");
    }
    const json plan = parse_json(raw, "case-plan");
    require_exact_fields(
        plan,
        {
            "schema_revision",
            "profile",
            "probe_abi_revision",
            "template_sha256",
            "ledger_sha256",
            "runtime_inventory_sha256",
            "sql_manifest_sha256",
            "runtime_policy_sha256",
            "cases",
        },
        "case-plan");
    if (require_field(
            plan, "schema_revision", "case-plan") != 2
        || require_field(
               plan, "probe_abi_revision", "case-plan") != 2) {
        fail("case-plan", "schema or probe ABI revision differs");
    }
    if (require_string_field(
            plan, "profile", "case-plan") != k_profile) {
        fail("case-plan.profile", "profile differs from executable");
    }
    const json& raw_cases = require_field(
        plan, "cases", "case-plan");
    if (!raw_cases.is_array() || raw_cases.empty()) {
        fail("case-plan.cases", "expected a non-empty array");
    }
    case_plan result;
    for (std::size_t index = 0;
         index < raw_cases.size();
         ++index) {
        const std::string context =
            fmt::format("case-plan.cases[{}]", index);
        const json& raw_case = raw_cases[index];
        const json catalog = normalized_catalog_entry(
            require_field(raw_case, "catalog", context),
            context + ".catalog");
        const json& execution = require_field(
            raw_case, "execution", context);
        if (!execution.is_object()) {
            fail(context + ".execution", "expected an object");
        }
        static const std::set<std::string, std::less<>>
            allowed_execution_fields{
                "target_regprocedure",
                "setup_sql",
                "invocation_sql",
                "post_error_sql",
                "recovery_sql",
                "recovery_expected_rows",
                "cleanup_sql",
                "pre_fault_control_sql",
                "pre_fault_control_recovery_sql",
                "pre_fault_control_recovery_expected_rows"};
        for (const auto& [field, value] : execution.items()) {
            static_cast<void>(value);
            if (!allowed_execution_fields.contains(field)) {
                fail(
                    context,
                    fmt::format(
                        "unknown execution field '{}'", field));
            }
        }
        const std::array<std::string_view, 3>
            pre_fault_control_fields{
                "pre_fault_control_sql",
                "pre_fault_control_recovery_sql",
                "pre_fault_control_recovery_expected_rows"};
        const std::size_t pre_fault_control_field_count =
            std::ranges::count_if(
                pre_fault_control_fields,
                [&execution](std::string_view field) {
                    return execution.contains(
                        std::string{field});
                });
        if (pre_fault_control_field_count != 0
            && pre_fault_control_field_count
                != pre_fault_control_fields.size()) {
            fail(
                context,
                "pre-fault control fields must be present together");
        }
        const bool backend_scoped_vacuum =
            require_string_field(
                catalog, "fault_key", context + ".catalog")
            == k_backend_scoped_vacuum_fault_key;
        if (backend_scoped_vacuum
            != (pre_fault_control_field_count
                == pre_fault_control_fields.size())) {
            fail(
                context,
                "backend-scoped VACUUM requires its sealed success control");
        }
        const std::string target = require_string_field(
            execution, "target_regprocedure",
            context + ".execution");
        if (target
            != require_string_field(
                catalog, "sql_identity",
                context + ".catalog")) {
            fail(
                context,
                "target_regprocedure must equal the ledger SQL identity");
        }
        static_cast<void>(sql_array(
            execution, "setup_sql",
            context + ".execution", false));
        const std::string invocation = require_string_field(
            execution, "invocation_sql",
            context + ".execution");
        if (invocation.size() > k_maximum_sql_bytes) {
            fail(context, "invocation SQL exceeds the size bound");
        }
        static_cast<void>(sql_array(
            execution, "post_error_sql",
            context + ".execution", false));
        const std::string recovery = require_string_field(
            execution, "recovery_sql",
            context + ".execution");
        if (recovery.size() > k_maximum_sql_bytes) {
            fail(context, "recovery SQL exceeds the size bound");
        }
        const auto validate_expected_rows =
            [&context](
                const json& expected_rows,
                std::string_view field) {
                if (!expected_rows.is_array()
                    || expected_rows.empty()
                    || expected_rows.size() > 64) {
                    fail(
                        context,
                        fmt::format(
                            "{} must contain 1..64 rows",
                            field));
                }
                std::optional<std::size_t> expected_columns;
                for (std::size_t row_index = 0;
                     row_index < expected_rows.size();
                     ++row_index) {
                    const json& row = expected_rows[row_index];
                    if (!row.is_array()
                        || row.empty()
                        || row.size() > 64
                        || (expected_columns.has_value()
                            && row.size()
                                != *expected_columns)) {
                        fail(
                            context,
                            fmt::format(
                                "{} must be a bounded rectangle",
                                field));
                    }
                    expected_columns = row.size();
                    for (std::size_t column = 0;
                         column < row.size();
                         ++column) {
                        if (!row[column].is_null()
                            && !row[column].is_string()) {
                            fail(
                                context,
                                fmt::format(
                                    "{} cells must be strings or null",
                                    field));
                        }
                    }
                }
            };
        validate_expected_rows(
            require_field(
                execution,
                "recovery_expected_rows",
                context + ".execution"),
            "recovery_expected_rows");
        if (backend_scoped_vacuum) {
            const std::vector<std::string> control_sql =
                sql_array(
                    execution,
                    "pre_fault_control_sql",
                    context + ".execution",
                    true);
            if (control_sql.empty()) {
                fail(
                    context,
                    "pre_fault_control_sql must not be empty");
            }
            const std::string control_recovery =
                require_string_field(
                    execution,
                    "pre_fault_control_recovery_sql",
                    context + ".execution");
            if (control_recovery.size() > k_maximum_sql_bytes) {
                fail(
                    context,
                    "pre-fault control recovery SQL exceeds the size bound");
            }
            validate_expected_rows(
                require_field(
                    execution,
                    "pre_fault_control_recovery_expected_rows",
                    context + ".execution"),
                "pre_fault_control_recovery_expected_rows");
        }
        static_cast<void>(sql_array(
            execution, "cleanup_sql",
            context + ".execution", false));
        const std::string id =
            catalog.at("case_id").get<std::string>();
        if (!result.cases.emplace(
                id,
                case_definition{
                    .catalog = catalog,
                    .execution = execution})
                 .second) {
            fail(context, "duplicate case_id");
        }
    }
    return result;
}

}  // namespace bioseq::tests::integration::safety_runtime_harness
