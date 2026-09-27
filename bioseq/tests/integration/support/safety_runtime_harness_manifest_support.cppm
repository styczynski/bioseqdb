/**
 * @file safety_runtime_harness_manifest_support.cppm
 * @brief Provide command, cluster-identity, manifest, and catalog support.
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

export module bioseq.tests.integration.safety_runtime_harness_support:manifest_support;

import :core_support;
import :case_support;

export namespace bioseq::tests::integration::safety_runtime_harness {

/// Parsed protocol command, options, and resolved executable identity.
struct command_line final {
    std::string command;
    std::map<std::string, std::string, std::less<>> options;
    fs::path executable;
};

/// Parse and validate the strict runtime-evidence command-line protocol.
[[nodiscard]] command_line parse_command_line(
        int argc,
        char** argv) {
    if (argc < 2 || argv == nullptr || argv[0] == nullptr) {
        fail("command-line", "missing protocol command");
    }
    const char* protocol =
        std::getenv("BIOSEQ_SAFETY_RUNTIME_PROTOCOL");
    if (protocol == nullptr
        || std::string_view{protocol} != "2") {
        fail(
            "command-line",
            "BIOSEQ_SAFETY_RUNTIME_PROTOCOL=2 is required");
    }
    const std::string command{argv[1]};
    static const std::set<std::string, std::less<>>
        supported{
            "--bioseq-safety-runtime-activate",
            "--bioseq-safety-runtime-describe",
            "--bioseq-safety-runtime-run",
            "--bioseq-safety-runtime-self-test"};
    if (!supported.contains(command)) {
        fail("command-line", "unsupported protocol command");
    }
    if ((argc - 2) % 2 != 0) {
        fail(
            "command-line",
            "options must be --name value pairs");
    }
    std::map<std::string, std::string, std::less<>> options;
    for (int index = 2; index < argc; index += 2) {
        if (argv[index] == nullptr
            || argv[index + 1] == nullptr
            || !std::string_view{argv[index]}.starts_with("--")) {
            fail("command-line", "malformed option pair");
        }
        if (!options.emplace(
                argv[index], argv[index + 1]).second) {
            fail(
                "command-line",
                fmt::format(
                    "duplicate option {}", argv[index]));
        }
    }
    fs::path executable = fs::absolute(argv[0]).lexically_normal();
    if (!fs::is_regular_file(executable)) {
        fail(
            "command-line", "executable path is not a regular file");
    }
    return command_line{
        .command = command,
        .options = std::move(options),
        .executable = std::move(executable)};
}

/// Return a required nonempty command-line option value.
[[nodiscard]] std::string require_option(
        const command_line& command,
        std::string_view name) {
    const auto found =
        command.options.find(std::string{name});
    if (found == command.options.end() || found->second.empty()) {
        fail(
            "command-line",
            fmt::format("missing option {}", name));
    }
    return found->second;
}

/// Require a protocol command to contain exactly the allowed options.
void require_exact_options(
        const command_line& command,
        std::initializer_list<std::string_view> expected) {
    std::set<std::string, std::less<>> exact;
    for (const std::string_view option : expected) {
        exact.insert(std::string{option});
    }
    std::set<std::string, std::less<>> observed;
    for (const auto& [option, value] : command.options) {
        static_cast<void>(value);
        observed.insert(option);
    }
    if (observed != exact) {
        fail(
            "command-line",
            "protocol command has missing or extra options");
    }
}

/// Hash the currently executing harness binary.
[[nodiscard]] std::string self_sha256(
        const command_line& command) {
    return sha256_file(command.executable);
}

/// Query the PostgreSQL identity fields bound into runtime evidence.
[[nodiscard]] json query_cluster_identity(
        const pg_connection& connection) {
    const pg_result result = connection.execute(
        "SELECT system_identifier::text, "
        "       current_setting('data_directory'), "
        "       pg_postmaster_start_time()::text, "
        "       current_setting('server_version_num'), "
        "       pg_backend_pid()::text, "
        "       pg_catalog.inet_server_addr()::text, "
        "       pg_catalog.current_database()::text, "
        "       pg_catalog.current_user::text, "
        "       role.rolsuper::text, "
        "       pg_catalog.pg_is_in_recovery()::text "
        "  FROM pg_catalog.pg_control_system() "
        "  JOIN pg_catalog.pg_roles AS role "
        "    ON role.rolname = pg_catalog.current_user;");
    const auto row = one_row(result, "cluster identity");
    if (row.size() != 10) {
        fail("cluster identity", "unexpected result shape");
    }
    const std::string data_directory =
        nonnull_cell(row, 1, "cluster identity");
    if (!fs::path{data_directory}.is_absolute()) {
        fail(
            "cluster identity",
            "PostgreSQL data directory is not absolute");
    }
    const std::uint64_t server_version =
        parse_unsigned(
            nonnull_cell(row, 3, "cluster identity"),
            "cluster identity server version");
    if (server_version / 10000 != 17) {
        fail(
            "cluster identity",
            "runtime evidence requires PostgreSQL 17");
    }
    return json{
        {"cluster_system_identifier",
         nonnull_cell(row, 0, "cluster identity")},
        {"cluster_data_directory",
         fs::path{data_directory}.lexically_normal().string()},
        {"postmaster_start_time",
         nonnull_cell(row, 2, "cluster identity")},
        {"server_version_num", server_version},
        {"backend_pid",
         parse_unsigned(
             nonnull_cell(row, 4, "cluster identity"),
             "cluster identity backend PID")},
        {"server_address",
         row[5].has_value()
             ? json{*row[5]}
             : json{nullptr}},
        {"database",
         nonnull_cell(row, 6, "cluster identity")},
        {"user",
         nonnull_cell(row, 7, "cluster identity")},
        {"superuser",
         nonnull_cell(row, 8, "cluster identity") == "true"},
        {"in_recovery",
         nonnull_cell(row, 9, "cluster identity") == "true"}};
}

/// Require the connected server to be the authorized private cluster.
void validate_private_cluster_identity(
        const json& cluster,
        const json& config) {
    const fs::path expected_data_directory =
        fs::canonical(require_absolute_path(
            config,
            "expected_data_directory",
            "cluster-config"));
    const fs::path observed_data_directory =
        fs::canonical(fs::path{require_string_field(
            cluster,
            "cluster_data_directory",
            "cluster-identity")});
    if (observed_data_directory != expected_data_directory
        || require_string_field(
               cluster,
               "cluster_system_identifier",
               "cluster-identity")
            != require_string_field(
                config,
                "expected_system_identifier",
                "cluster-config")
        || !require_field(
                cluster, "server_address", "cluster-identity")
                .is_null()
        || require_string_field(
               cluster, "database", "cluster-identity")
            != "bioseq_safety_runtime"
        || require_string_field(
               cluster, "user", "cluster-identity")
            != parse_connection(config).user
        || require_field(
               cluster, "superuser", "cluster-identity")
            != true
        || require_field(
               cluster, "in_recovery", "cluster-identity")
            != false) {
        fail(
            "cluster-identity",
            "connection is not the authorized local private cluster");
    }
    require_private_authorization(
        require_absolute_path(
            config, "authorization_file", "cluster-config"),
        expected_data_directory);
}

/// Validated SQL manifest inventories needed by activation and case runs.
struct sql_manifest_contract final {
    std::string sha256;
    std::string script_sha256;
    guc_manifest_contract gucs;
    std::map<std::string, std::string, std::less<>>
        symbol_by_identity;
    std::set<std::string, std::less<>> symbols;
    std::set<std::string, std::less<>> finfo_symbols;
};

/// Extract and validate the generated GUC portion of the SQL manifest.
[[nodiscard]] guc_manifest_contract load_guc_manifest_contract(
        const json& manifest) {
    constexpr std::string_view context = "SQL manifest.guc_generation";
    const json& generation =
        require_field(manifest, "guc_generation", "SQL manifest");
    require_exact_fields(
        generation,
        {
            "description_authority",
            "owner_facets",
            "profile",
            "profile_delta_validation",
            "registration_count",
            "registrations",
            "registry_content_sha256",
            "registry_module",
        },
        context);
    const std::string profile =
        require_string_field(generation, "profile", context);
    const guc_profile_counts expected =
        expected_guc_profile(profile);
    if (profile != k_profile
        || require_string_field(
               generation, "description_authority", context)
            != "cleaned_doxygen_only") {
        fail(context, "profile or description authority differs");
    }

    const std::string registry_module = require_string_field(
        generation, "registry_module", context);
    const std::string registry_sha256 = require_string_field(
        generation, "registry_content_sha256", context);
    if (registry_module
            != fmt::format(
                "bioseq.postgres.sql.generated.{}.guc_registry", profile)
        || !lowercase_sha256(registry_sha256)) {
        fail(context, "registry module or content SHA-256 differs");
    }

    const json& owner_facets =
        require_field(generation, "owner_facets", context);
    static constexpr std::array<std::string_view, 4> owner_order{
        "bioseq", "scheduler", "storage", "aligner"};
    static constexpr std::array<std::uint64_t, 4> production_counts{
        26, 3, 1, 43};
    if (!owner_facets.is_array()
        || owner_facets.size() != owner_order.size()) {
        fail(context, "owner-facet inventory differs");
    }
    std::map<std::string, json, std::less<>> facets;
    for (std::size_t index = 0; index < owner_facets.size(); ++index) {
        const json& facet = owner_facets[index];
        const std::string facet_context = fmt::format(
            "{}.owner_facets[{}]", context, index);
        require_exact_fields(
            facet,
            {
                "owner",
                "source_module",
                "generated_module",
                "content_sha256",
                "registration_count",
            },
            facet_context);
        const std::string owner =
            require_string_field(facet, "owner", facet_context);
        const std::string expected_source =
            owner == "bioseq"
            ? "bioseq.postgres.configuration.core"
            : fmt::format(
                  "bioseq.postgres.configuration.{}", owner);
        const std::uint64_t count = parse_unsigned(
            require_field(
                facet, "registration_count", facet_context).dump(),
            fmt::format("{}.registration_count", facet_context));
        const std::uint64_t expected_count =
            production_counts[index]
            + ((profile == "test_indexes" && owner == "bioseq")
                   ? 3U
                   : 0U);
        if (owner != owner_order[index]
            || require_string_field(
                   facet, "source_module", facet_context)
                != expected_source
            || require_string_field(
                   facet, "generated_module", facet_context)
                != fmt::format(
                    "bioseq.postgres.sql.generated.{}.{}.gucs",
                    profile,
                    owner)
            || !lowercase_sha256(require_string_field(
                facet, "content_sha256", facet_context))
            || count != expected_count
            || !facets.emplace(owner, facet).second) {
            fail(facet_context, "owner-facet authority differs");
        }
    }

    const json& delta =
        require_field(generation, "profile_delta_validation", context);
    require_exact_fields(
        delta,
        {
            "production_registry_content_sha256",
            "test_indexes_registry_content_sha256",
            "added_in_test_indexes",
            "removed_from_test_indexes",
            "changed_in_test_indexes",
            "owner_descriptor_profile_invariant",
            "owner_artifacts",
        },
        "SQL manifest GUC profile delta");
    const json expected_added = json::array({
        "bioseq.testing.chunked_scanner.enabled",
        "bioseq.testing.in_memory_scanner.enabled",
        "bioseq.testing.persisted_index.failure_mode",
    });
    if (require_field(
            delta,
            "added_in_test_indexes",
            "SQL manifest GUC profile delta")
            != expected_added
        || require_field(
               delta,
               "removed_from_test_indexes",
               "SQL manifest GUC profile delta")
            != json::array()
        || require_field(
               delta,
               "changed_in_test_indexes",
               "SQL manifest GUC profile delta")
            != json::array()) {
        fail(
            "SQL manifest GUC profile delta",
            "the profile delta differs");
    }
    const std::string production_registry_sha =
        require_string_field(
            delta,
            "production_registry_content_sha256",
            "SQL manifest GUC profile delta");
    const std::string test_registry_sha =
        require_string_field(
            delta,
            "test_indexes_registry_content_sha256",
            "SQL manifest GUC profile delta");
    if (!lowercase_sha256(production_registry_sha)
        || !lowercase_sha256(test_registry_sha)
        || registry_sha256
            != (profile == "production"
                    ? production_registry_sha
                    : test_registry_sha)) {
        fail(
            "SQL manifest GUC profile delta",
            "current registry hash is not closed by the profile proof");
    }
    const json& invariants = require_field(
        delta,
        "owner_descriptor_profile_invariant",
        "SQL manifest GUC profile delta");
    require_exact_fields(
        invariants,
        {"bioseq", "scheduler", "storage", "aligner"},
        "SQL manifest GUC owner invariants");
    const json& owner_artifacts = require_field(
        delta,
        "owner_artifacts",
        "SQL manifest GUC profile delta");
    require_exact_fields(
        owner_artifacts,
        {"bioseq", "scheduler", "storage", "aligner"},
        "SQL manifest GUC owner artifacts");
    for (const std::string_view owner : owner_order) {
        const json& invariant =
            require_field(
                invariants, owner, "SQL manifest GUC owner invariants");
        if (!invariant.is_boolean()
            || invariant.get<bool>() != (owner != "bioseq")) {
            fail(
                "SQL manifest GUC owner invariants",
                "owner profile-invariance proof differs");
        }
        const json& artifacts = require_field(
            owner_artifacts,
            owner,
            "SQL manifest GUC owner artifacts");
        require_exact_fields(
            artifacts,
            {
                "production_content_sha256",
                "test_indexes_content_sha256",
            },
            "SQL manifest GUC owner artifact");
        const std::string production_sha = require_string_field(
            artifacts,
            "production_content_sha256",
            "SQL manifest GUC owner artifact");
        const std::string test_sha = require_string_field(
            artifacts,
            "test_indexes_content_sha256",
            "SQL manifest GUC owner artifact");
        if (!lowercase_sha256(production_sha)
            || !lowercase_sha256(test_sha)
            || require_string_field(
                   facets.at(std::string{owner}),
                   "content_sha256",
                   "SQL manifest GUC owner facet")
                != (profile == "production"
                        ? production_sha
                        : test_sha)) {
            fail(
                "SQL manifest GUC owner artifact",
                "owner facet is not closed by the profile proof");
        }
    }

    const json& registrations =
        require_field(generation, "registrations", context);
    const std::uint64_t registration_count = parse_unsigned(
        require_field(
            generation, "registration_count", context).dump(),
        "SQL manifest.guc_generation.registration_count");
    if (!registrations.is_array()
        || registrations.size() != registration_count
        || registration_count != expected.total) {
        fail(context, "registration count differs");
    }
    std::vector<json> sealed_registrations;
    sealed_registrations.reserve(registrations.size());
    std::set<std::string, std::less<>> names;
    std::map<std::string, std::uint64_t, std::less<>> owner_counts;
    std::optional<std::uint64_t> previous_ordinal;
    for (std::size_t index = 0; index < registrations.size(); ++index) {
        const json& registration = registrations[index];
        const std::string registration_context =
            fmt::format("{}.registrations[{}]", context, index);
        require_exact_fields(
            registration,
            {
                "registration_ordinal",
                "name",
                "owner",
                "member",
                "kind",
                "cpp_storage",
                "storage_kind",
                "source_module",
                "source_path",
                "source_sha256",
                "default_expression",
                "minimum_expression",
                "maximum_expression",
                "context",
                "flags",
                "unit",
                "hooks",
                "feature_predicate",
                "enum_default",
                "enum_rows_name_wire_condition_alias_hidden_compiled",
                "generated_live_authority",
            },
            registration_context);
        const json& generated_live_authority = require_field(
            registration,
            "generated_live_authority",
            registration_context);
        const std::uint64_t ordinal = parse_unsigned(
            require_field(
                registration,
                "registration_ordinal",
                registration_context).dump(),
            fmt::format(
                "{}.registration_ordinal", registration_context));
        const std::string name = require_string_field(
            registration, "name", registration_context);
        const std::string member = require_string_field(
            registration, "member", registration_context);
        const std::string owner = require_string_field(
            registration, "owner", registration_context);
        const std::string expected_prefix = owner == "bioseq"
            ? "bioseq."
            : fmt::format("bioseq.{}.", owner);
        const std::string expected_source_path = owner == "bioseq"
            ? "bioseq/backend/pg/configuration/core.cppm"
            : fmt::format(
                  "bioseq/backend/pg/configuration/{}.cppm", owner);
        const std::string kind = require_string_field(
            registration, "kind", registration_context);
        static_cast<void>(expected_pg_guc_kind(kind));
        static_cast<void>(expected_pg_guc_context(
            require_string_field(
                registration, "context", registration_context)));
        if ((previous_ordinal.has_value()
                && ordinal <= *previous_ordinal)
            || !name.starts_with(expected_prefix)
            || name.size() == expected_prefix.size()
            || member.empty()
            || !names.insert(name).second
            || !facets.contains(owner)
            || require_string_field(
                   registration, "storage_kind", registration_context)
                != expected_guc_storage_kind(kind)
            || require_string_field(
                   registration, "source_module", registration_context)
                != require_string_field(
                    facets.at(owner),
                    "source_module",
                    registration_context)
            || require_string_field(
                   registration, "source_path", registration_context)
                != expected_source_path
            || !lowercase_sha256(require_string_field(
                registration, "source_sha256", registration_context))) {
            fail(
                registration_context,
                "registration identity, order, or ownership differs");
        }
        previous_ordinal = ordinal;
        ++owner_counts[owner];
        const json& flags =
            require_field(registration, "flags", registration_context);
        std::set<std::string, std::less<>> unique_flags;
        if (!flags.is_array()) {
            fail(registration_context, "flags are not an array");
        }
        for (const json& flag : flags) {
            const std::string value =
                require_string(flag, registration_context);
            if ((value != "no_show_all"
                 && value != "not_in_sample")
                || !unique_flags.insert(value).second) {
                fail(
                    registration_context,
                    "flags contain an unsupported or duplicate value");
            }
        }
        const json& hooks =
            require_field(registration, "hooks", registration_context);
        if (!hooks.is_array() || hooks.size() != 3) {
            fail(registration_context, "hook tuple differs");
        }
        for (const json& hook : hooks) {
            if (!hook.is_null() && !hook.is_string()) {
                fail(registration_context, "hook value is malformed");
            }
        }
        const bool test_only =
            name == "bioseq.testing.chunked_scanner.enabled"
            || name == "bioseq.testing.in_memory_scanner.enabled"
            || name == "bioseq.testing.persisted_index.failure_mode";
        const std::string feature = require_string_field(
            registration, "feature_predicate", registration_context);
        if (feature
                != (test_only
                        ? "defined(BIOSEQ_TEST_INDEXES)"
                        : "true")
            || (test_only && profile != "test_indexes")) {
            fail(
                registration_context,
                "registration profile authority differs");
        }
        static_cast<void>(require_string_field(
            registration, "default_expression", registration_context));
        static_cast<void>(require_string_field(
            registration, "cpp_storage", registration_context));
        const json& minimum = require_field(
            registration, "minimum_expression", registration_context);
        const json& maximum = require_field(
            registration, "maximum_expression", registration_context);
        const bool bounded = kind == "integer" || kind == "real";
        if ((bounded
                && (!minimum.is_string() || !maximum.is_string()))
            || (!bounded
                && (!minimum.is_null() || !maximum.is_null()))) {
            fail(
                registration_context,
                "minimum/maximum authority differs for the GUC kind");
        }
        const json& unit =
            require_field(registration, "unit", registration_context);
        if (!unit.is_null()
            && (!unit.is_string()
                || unit.get<std::string>() != "mb"
                || kind != "integer")) {
            fail(registration_context, "unit authority differs");
        }
        const json& enum_rows = require_field(
            registration,
            "enum_rows_name_wire_condition_alias_hidden_compiled",
            registration_context);
        const json& enum_default = require_field(
            registration, "enum_default", registration_context);
        if (!enum_rows.is_array()
            || (kind == "enum"
                && (enum_rows.empty()
                    || !enum_default.is_string()))
            || (kind != "enum"
                && (!enum_rows.empty() || !enum_default.is_null()))) {
            fail(registration_context, "enum authority differs");
        }
        std::set<std::string, std::less<>> enum_names;
        for (const json& row : enum_rows) {
            if (!row.is_array() || row.size() != 6
                || !row[0].is_string()
                || !row[1].is_string()
                || !row[2].is_string()
                || (!row[3].is_null() && !row[3].is_string())
                || !row[4].is_boolean()
                || !row[5].is_boolean()
                || !enum_names.insert(
                        row[0].get<std::string>()).second) {
                fail(registration_context, "enum row is malformed");
            }
        }
        validate_generated_live_guc_authority(
            registration,
            generated_live_authority,
            fmt::format(
                "{}.generated_live_authority",
                registration_context));
        sealed_registrations.push_back(registration);
    }
    for (std::size_t index = 0; index < owner_order.size(); ++index) {
        if (owner_counts[std::string{owner_order[index]}]
            != production_counts[index]
                + ((profile == "test_indexes"
                    && owner_order[index] == "bioseq")
                       ? 3U
                       : 0U)) {
            fail(context, "owner registration counts differ");
        }
    }
    const std::size_t test_profile_name_count =
        static_cast<std::size_t>(names.contains(
            "bioseq.testing.chunked_scanner.enabled"))
        + static_cast<std::size_t>(names.contains(
            "bioseq.testing.in_memory_scanner.enabled"))
        + static_cast<std::size_t>(names.contains(
            "bioseq.testing.persisted_index.failure_mode"));
    if (test_profile_name_count
        != (profile == "test_indexes" ? 3U : 0U)) {
        fail(context, "test-profile GUC delta differs");
    }
    return guc_manifest_contract{
        .profile = profile,
        .registry_module = registry_module,
        .registry_content_sha256 = registry_sha256,
        .profile_delta_validation_sha256 =
            sha256_bytes(canonical_json(delta)),
        .owner_facets = std::move(facets),
        .registrations = std::move(sealed_registrations)};
}

/// Read one nonempty, duplicate-free string inventory from the SQL manifest.
[[nodiscard]] std::set<std::string, std::less<>>
manifest_string_set(
        const json& manifest,
        std::string_view field) {
    const json& values =
        require_field(manifest, field, "SQL manifest");
    if (!values.is_array() || values.empty()) {
        fail(
            fmt::format("SQL manifest.{}", field),
            "expected a non-empty array");
    }
    std::set<std::string, std::less<>> result;
    for (std::size_t index = 0; index < values.size(); ++index) {
        const std::string value = require_string(
            values[index],
            fmt::format(
                "SQL manifest.{}[{}]", field, index));
        if (!result.insert(value).second) {
            fail(
                fmt::format("SQL manifest.{}", field),
                "contains a duplicate");
        }
    }
    return result;
}

/// Load the generated SQL manifest and close all symbol inventories.
[[nodiscard]] sql_manifest_contract load_sql_manifest() {
    const fs::path manifest_path{k_sql_manifest};
    const fs::path sql_path{k_sql_file};
    if (!manifest_path.is_absolute()
        || !sql_path.is_absolute()
        || !fs::is_regular_file(manifest_path)
        || !fs::is_regular_file(sql_path)
        || fs::is_symlink(manifest_path)
        || fs::is_symlink(sql_path)) {
        fail(
            "SQL manifest",
            "compiled SQL artifact paths are unavailable");
    }
    const std::string raw = read_file(manifest_path);
    const json manifest = parse_json(raw, "SQL manifest");
    require_known_fields(
        manifest,
        {
            "schema_revision",
            "authority_mode",
            "compiler_profile",
            "clang_version",
            "compilation_signature",
            "extension_name",
            "extension_schema",
            "generated_module_prefix",
            "fmgr_boundary_module",
            "script_sha256",
            "statement_count",
            "statements",
            "entity_count",
            "public_entity_count",
            "public_stable_identities",
            "public_installed_identities",
            "routine_entity_count",
            "c_routine_identity_count",
            "c_link_symbol_inventory_count",
            "c_finfo_symbol_inventory_count",
            "c_wrapper_entity_count",
            "generated_wrapper_symbol_count",
            "handwritten_fmgr_export_symbol_count",
            "handwritten_fmgr_export_external_symbol_count",
            "handwritten_fmgr_export_owner_count",
            "c_symbol_sharing_group_count",
            "generated_cpp_profile_invariant",
            "entity_handle_count",
            "entity_handle_module_count",
            "entity_handle_modules",
            "row_codec_count",
            "row_codec_module_count",
            "row_codec_modules",
            "routine_boundary_contract_count",
            "routine_boundary_contracts",
            "result_shape_contract_count",
            "result_shape_contracts",
            "row_codec_contract_count",
            "row_codec_contracts",
            "c_routine_identities",
            "c_link_symbols",
            "c_finfo_symbols",
            "generated_wrapper_symbols",
            "handwritten_fmgr_export_symbols",
            "handwritten_fmgr_export_external_symbols",
            "fmgr_entrypoint_module_count",
            "fmgr_entrypoint_modules",
            "handwritten_fmgr_export_owners",
            "c_symbol_owners",
            "entity_counts_by_kind",
            "entities",
            "guc_generation",
        },
        "SQL manifest");
    const json& schema_revision =
        require_field(manifest, "schema_revision", "SQL manifest");
    if (!schema_revision.is_number_integer()
        || schema_revision != 1
        || require_string_field(
               manifest, "authority_mode", "SQL manifest")
            != "annotated_cpp"
        || require_string_field(
               manifest, "compiler_profile", "SQL manifest")
            != k_profile) {
        fail(
            "SQL manifest",
            "schema revision, authority, or profile differs");
    }
    const std::string script_sha256 =
        require_string_field(
            manifest, "script_sha256", "SQL manifest");
    if (!lowercase_sha256(script_sha256)
        || sha256_file(sql_path) != script_sha256) {
        fail(
            "SQL manifest",
            "script_sha256 differs from generated SQL bytes");
    }
    const std::set<std::string, std::less<>> identities =
        manifest_string_set(
            manifest, "c_routine_identities");
    const std::set<std::string, std::less<>> symbols =
        manifest_string_set(manifest, "c_link_symbols");
    const std::set<std::string, std::less<>> finfo =
        manifest_string_set(manifest, "c_finfo_symbols");
    std::set<std::string, std::less<>> expected_finfo;
    for (const std::string& symbol : symbols) {
        expected_finfo.insert("pg_finfo_" + symbol);
    }
    if (finfo != expected_finfo) {
        fail(
            "SQL manifest",
            "finfo inventory differs from the exact C symbol set");
    }
    std::map<std::string, std::string, std::less<>>
        symbol_by_identity;
    const json& owners = require_field(
        manifest, "c_symbol_owners", "SQL manifest");
    if (!owners.is_array()
        || owners.size() != symbols.size()) {
        fail(
            "SQL manifest.c_symbol_owners",
            "owner inventory does not close over symbols");
    }
    std::set<std::string, std::less<>> owner_symbols;
    for (std::size_t index = 0; index < owners.size(); ++index) {
        const json& owner = owners[index];
        const std::string context =
            fmt::format(
                "SQL manifest.c_symbol_owners[{}]", index);
        const std::string symbol =
            require_string_field(owner, "symbol", context);
        if (!owner_symbols.insert(symbol).second
            || !symbols.contains(symbol)
            || require_string_field(
                   owner, "finfo_symbol", context)
                != "pg_finfo_" + symbol
            || require_field(
                   owner, "generated_wrapper", context)
                != true) {
            fail(
                context,
                "symbol owner is duplicate or not generated");
        }
        const json& routine_identities =
            require_field(
                owner, "routine_identities", context);
        if (!routine_identities.is_array()
            || routine_identities.empty()) {
            fail(
                context,
                "routine identity group is empty");
        }
        for (std::size_t identity_index = 0;
             identity_index < routine_identities.size();
             ++identity_index) {
            const std::string identity = require_string(
                routine_identities[identity_index],
                fmt::format(
                    "{}.routine_identities[{}]",
                    context,
                    identity_index));
            if (!identities.contains(identity)
                || !symbol_by_identity.emplace(
                        identity, symbol).second) {
                fail(
                    context,
                    "routine identity owner differs from inventory");
            }
        }
    }
    if (owner_symbols != symbols
        || symbol_by_identity.size() != identities.size()) {
        fail(
            "SQL manifest",
            "exact identity/symbol ownership closure differs");
    }
    guc_manifest_contract gucs =
        load_guc_manifest_contract(manifest);
    return sql_manifest_contract{
        .sha256 = sha256_bytes(raw),
        .script_sha256 = script_sha256,
        .gucs = std::move(gucs),
        .symbol_by_identity = std::move(symbol_by_identity),
        .symbols = symbols,
        .finfo_symbols = finfo};
}

/// Verify the installed extension catalog against the sealed SQL manifest.
[[nodiscard]] std::uint64_t verify_installed_catalog(
        const pg_connection& connection,
        const sql_manifest_contract& manifest) {
    json identities = json::array();
    for (const auto& [identity, symbol] :
         manifest.symbol_by_identity) {
        static_cast<void>(symbol);
        identities.push_back(identity);
    }
    const std::array<std::string, 1> parameters{
        identities.dump()};
    const pg_result exact =
        connection.execute_parameters(
            "WITH expected AS ("
            " SELECT identity, ordinal"
            " FROM pg_catalog.jsonb_array_elements_text($1::jsonb)"
            "      WITH ORDINALITY AS value(identity, ordinal)"
            "), resolved AS ("
            " SELECT identity, ordinal,"
            "        pg_catalog.to_regprocedure(identity) AS oid"
            " FROM expected"
            ")"
            " SELECT resolved.identity, p.oid::text, p.prosrc,"
            "        p.probin, language.lanname::text,"
            "        extension.oid::text, p.prokind::text"
            " FROM resolved"
            " LEFT JOIN pg_catalog.pg_proc AS p"
            "   ON p.oid = resolved.oid"
            " LEFT JOIN pg_catalog.pg_language AS language"
            "   ON language.oid = p.prolang"
            " LEFT JOIN pg_catalog.pg_depend AS dependency"
            "   ON dependency.classid = 'pg_catalog.pg_proc'::regclass"
            "  AND dependency.objid = p.oid"
            "  AND dependency.objsubid = 0"
            "  AND dependency.refclassid ="
            "      'pg_catalog.pg_extension'::regclass"
            "  AND dependency.deptype = 'e'"
            " LEFT JOIN pg_catalog.pg_extension AS extension"
            "   ON extension.oid = dependency.refobjid"
            "  AND extension.extname = 'bioseq'"
            " ORDER BY resolved.ordinal;",
            parameters);
    const auto catalog_rows =
        rows(exact, "installed C routine catalog");
    if (catalog_rows.size()
        != manifest.symbol_by_identity.size()) {
        fail(
            "installed C routine catalog",
            "resolved row count differs from manifest");
    }
    std::set<std::uint64_t> expected_oids;
    std::optional<std::uint64_t> extension_oid;
    std::size_t row_index = 0;
    for (const auto& [identity, symbol] :
         manifest.symbol_by_identity) {
        const auto& row = catalog_rows[row_index++];
        if (row.size() != 7
            || nonnull_cell(
                   row, 0, "installed C routine catalog")
                != identity
            || nonnull_cell(
                   row, 2, "installed C routine catalog")
                != symbol
            || nonnull_cell(
                   row, 3, "installed C routine catalog")
                != "$libdir/bioseq"
            || nonnull_cell(
                   row, 4, "installed C routine catalog")
                != "c"
            || (nonnull_cell(
                    row, 6, "installed C routine catalog")
                    != "f"
                && nonnull_cell(
                    row, 6, "installed C routine catalog")
                    != "p")) {
            fail(
                "installed C routine catalog",
                "identity, symbol, language, kind, or library differs");
        }
        const std::uint64_t oid = parse_unsigned(
            nonnull_cell(
                row, 1, "installed C routine catalog"),
            "installed C routine OID");
        const std::uint64_t owner_oid = parse_unsigned(
            nonnull_cell(
                row, 5, "installed C routine catalog"),
            "installed extension OID");
        if (!expected_oids.insert(oid).second
            || (extension_oid.has_value()
                && *extension_oid != owner_oid)) {
            fail(
                "installed C routine catalog",
                "duplicate routine OID or extension owner");
        }
        extension_oid = owner_oid;
    }
    const pg_result extension_routines = connection.execute(
        "SELECT p.oid::text"
        " FROM pg_catalog.pg_proc AS p"
        " JOIN pg_catalog.pg_language AS language"
        "   ON language.oid = p.prolang"
        " JOIN pg_catalog.pg_depend AS dependency"
        "   ON dependency.classid = 'pg_catalog.pg_proc'::regclass"
        "  AND dependency.objid = p.oid"
        "  AND dependency.objsubid = 0"
        "  AND dependency.refclassid ="
        "      'pg_catalog.pg_extension'::regclass"
        "  AND dependency.deptype = 'e'"
        " JOIN pg_catalog.pg_extension AS extension"
        "   ON extension.oid = dependency.refobjid"
        " WHERE extension.extname = 'bioseq'"
        "   AND language.lanname = 'c'"
        "   AND p.prokind IN ('f','p')"
        " ORDER BY p.oid;");
    const auto extension_rows =
        rows(extension_routines, "extension C routine OIDs");
    std::set<std::uint64_t> observed_oids;
    for (const auto& row : extension_rows) {
        if (!observed_oids.insert(parse_unsigned(
                nonnull_cell(
                    row, 0, "extension C routine OIDs"),
                "extension C routine OID")).second) {
            fail(
                "extension C routine OIDs",
                "contains a duplicate");
        }
    }
    if (observed_oids != expected_oids
        || !extension_oid.has_value()) {
        fail(
            "installed C routine catalog",
            "extension-owned C OID set differs from manifest");
    }
    const pg_result languages = connection.execute(
        "SELECT count(*) FILTER (WHERE l.lanname = 'plpgsql')::text"
        " FROM pg_catalog.pg_proc AS p"
        " JOIN pg_catalog.pg_language AS l ON l.oid = p.prolang"
        " JOIN pg_catalog.pg_depend AS d"
        "   ON d.classid = 'pg_catalog.pg_proc'::regclass"
        "  AND d.objid = p.oid AND d.objsubid = 0"
        " JOIN pg_catalog.pg_extension AS e"
        "   ON d.refclassid = 'pg_catalog.pg_extension'::regclass"
        "  AND d.refobjid = e.oid AND d.deptype = 'e'"
        " WHERE e.extname = 'bioseq';");
    const auto language_row =
        one_row(languages, "installed language catalog");
    if (nonnull_cell(
            language_row, 0, "installed language catalog") != "0") {
        fail(
            "installed language catalog",
            "extension still owns a PL/pgSQL routine");
    }
    return *extension_oid;
}

}  // namespace bioseq::tests::integration::safety_runtime_harness
