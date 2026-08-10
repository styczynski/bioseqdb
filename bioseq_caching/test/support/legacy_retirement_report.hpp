#pragma once

#include "legacy_migration_ledger.hpp"

#include <simdjson.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace bioseq::cache_test::retirement {

inline constexpr std::string_view receipt_schema{
    "bioseq-cache-mcp-evidence-receipt-v1"};
inline constexpr std::string_view unit_receipt_schema{
    "bioseq-cache-mcp-unit-receipt-v1"};
inline constexpr std::string_view bundle_schema{
    "bioseq-cache-retirement-gate-input-v1"};
inline constexpr std::string_view proof_schema{
    "bioseq-cache-retirement-proof-v1"};
inline constexpr std::string_view transition_schema{
    "bioseq-cache-retirement-transition-v1"};
inline constexpr std::string_view integration_schema{
    "bioseq-cache-test-v2"};
inline constexpr std::string_view benchmark_schema{
    "bioseq-cache-benchmark-v2"};

using report_role = migration::evidence_role;

enum class gate_operation {
    proof_ready,
    retired_transition,
    retired_final,
};

inline constexpr std::array<report_role, 5> required_report_roles{
    report_role::cache_integration,
    report_role::cache_bioseq_support_integration,
    report_role::cache_bioseq_production_integration,
    report_role::cache_bench,
    report_role::cache_bioseq_bench,
};

[[nodiscard]] inline constexpr std::string_view report_role_name(
    report_role role) noexcept {
    return migration::role_name(role);
}

[[nodiscard]] inline report_role parse_role(std::string_view value) {
    return migration::parse_evidence_role(value);
}

[[nodiscard]] inline constexpr bool is_integration_role(
    report_role role) noexcept {
    return role == report_role::cache_integration
        || role == report_role::cache_bioseq_support_integration
        || role == report_role::cache_bioseq_production_integration;
}

[[nodiscard]] inline constexpr migration::evidence_harness role_owner(
    report_role role) noexcept {
    return migration::role_harness(role);
}

[[nodiscard]] inline constexpr std::string_view role_mode(
    report_role role) noexcept {
    switch (role) {
    case report_role::cache_integration:
        return "cache-integration";
    case report_role::cache_bioseq_support_integration:
    case report_role::cache_bioseq_production_integration:
        return "cache-bioseq-integration";
    case report_role::cache_bench:
        return "cache-bench";
    case report_role::cache_bioseq_bench:
        return "cache-bioseq-bench";
    }
    return "unknown";
}

[[nodiscard]] inline constexpr std::string_view role_executable_name(
    report_role role) noexcept {
    switch (role) {
    case report_role::cache_integration:
        return "bioseq_cache_integration_tests";
    case report_role::cache_bioseq_support_integration:
        return "bioseq_cache_bioseq_support_integration_tests";
    case report_role::cache_bioseq_production_integration:
        return "bioseq_cache_bioseq_production_integration_tests";
    case report_role::cache_bench:
        return "bioseq_cache_bench";
    case report_role::cache_bioseq_bench:
        return "bioseq_cache_bioseq_bench";
    }
    return "unknown";
}

[[nodiscard]] inline constexpr std::string_view role_scenario(
    report_role role) noexcept {
    switch (role) {
    case report_role::cache_integration:
        return "cache-integration";
    case report_role::cache_bioseq_support_integration:
        return "cache-bioseq-support";
    case report_role::cache_bioseq_production_integration:
        return "cache-bioseq-production-exact-2gbp";
    case report_role::cache_bench:
        return "standalone";
    case report_role::cache_bioseq_bench:
        return "bioseq-2gbp";
    }
    return "unknown";
}

struct source_provenance {
    std::string revision;
    std::string scope_sha256;
    bool scoped_clean{};
};

struct evidence_receipt {
    std::filesystem::path receipt_path;
    std::string receipt_sha256;
    report_role role{};
    std::string mode;
    std::filesystem::path native_report;
    std::string native_report_sha256;
    std::filesystem::path executable;
    std::string executable_sha256;
    std::string scenario;
    source_provenance source;
    std::string migration_ledger_sha256;
    std::string evidence_contract_sha256;
    std::string migration_phase;
};

struct native_artifact_evidence {
    std::string name;
    std::filesystem::path preinstalled_stage_root;
    std::filesystem::path stage_manifest;
    std::string stage_manifest_sha256;
    std::filesystem::path control_file;
    std::filesystem::path install_sql;
    std::filesystem::path dso;
    std::filesystem::path staged_control_file;
    std::filesystem::path staged_install_sql;
    std::filesystem::path staged_dso;
    std::string control_sha256;
    std::string install_sql_sha256;
    std::string dso_sha256;
    std::string staged_control_sha256;
};

struct native_report_evidence {
    report_role role{};
    migration::evidence_harness owner{};
    std::string migration_ledger_sha256;
    std::string evidence_contract_sha256;
    std::string migration_phase;
    std::vector<migration::probe_observation> probes;
    std::filesystem::path executable;
    std::string executable_sha256;
    std::string scenario;
    std::uint64_t configured_samples{};
    std::uint64_t configured_warmup_samples{};
    std::vector<native_artifact_evidence> artifacts;
    std::vector<std::filesystem::path> successful_cleanup_roots;
};

struct unit_command_receipt {
    std::filesystem::path executable;
    std::string executable_sha256;
    std::vector<std::string> arguments;
};

struct unit_receipt {
    std::filesystem::path receipt_path;
    std::string receipt_sha256;
    source_provenance source;
    std::string migration_ledger_sha256;
    std::string evidence_contract_sha256;
    std::string migration_phase;
    std::vector<unit_command_receipt> commands;
};

struct gate_bundle {
    gate_operation operation{};
    migration::retirement_phase phase{};
    std::filesystem::path ledger;
    std::filesystem::path catch_cases;
    std::filesystem::path legacy_root;
    std::filesystem::path legacy_inventory;
    std::filesystem::path proof_receipt;
    source_provenance current_source;
    std::string retirement_transition_sha256;
    std::filesystem::path unit_receipt;
    std::vector<std::filesystem::path> evidence_receipts;
    std::filesystem::path proof_unit_receipt;
    std::vector<std::filesystem::path> proof_evidence_receipts;
    std::filesystem::path transition_receipt;
};

struct proof_report {
    report_role role{};
    std::string receipt_sha256;
    std::string native_report_sha256;
};

struct retirement_proof {
    std::string proof_source_revision;
    std::string proof_source_scope_sha256;
    std::string migration_ledger_sha256;
    std::string evidence_contract_sha256;
    std::string migration_phase;
    std::string legacy_inventory_sha256;
    std::uint64_t legacy_asset_count{};
    std::string unit_receipt_sha256;
    std::vector<proof_report> reports;
};

struct retirement_transition {
    std::string proof_receipt_sha256;
    std::string proof_source_revision;
    std::string proof_source_scope_sha256;
    std::string retired_source_revision;
    std::string retired_source_scope_sha256;
    std::string retirement_transition_sha256;
    std::string evidence_contract_sha256;
    std::string legacy_inventory_sha256;
    std::string unit_receipt_sha256;
    std::vector<proof_report> reports;
};

struct legacy_asset {
    std::filesystem::path path;
    std::string sha256;
};

struct legacy_inventory {
    std::vector<legacy_asset> assets;
};

namespace detail {

[[nodiscard]] inline bool is_lower_hex(
    std::string_view value, std::size_t size) noexcept {
    return value.size() == size
        && std::ranges::all_of(value, [](char character) {
               return (character >= '0' && character <= '9')
                   || (character >= 'a' && character <= 'f');
           });
}

inline void require_sha256(
    std::string_view value, std::string_view field) {
    if (!is_lower_hex(value, 64)) {
        throw std::runtime_error(
            std::string(field)
            + " must be a 64-character lowercase SHA-256");
    }
}

inline void require_revision(
    std::string_view value, std::string_view field) {
    if (!is_lower_hex(value, 40)) {
        throw std::runtime_error(
            std::string(field)
            + " must be a 40-character lowercase Git revision");
    }
}

[[nodiscard]] inline simdjson::dom::element load_json(
    simdjson::dom::parser& parser, const std::filesystem::path& path) {
    std::error_code error;
    const auto metadata = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(metadata)
        || std::filesystem::is_symlink(metadata)) {
        throw std::runtime_error(
            "JSON evidence is not a regular non-symlink file: "
            + path.string());
    }
    simdjson::dom::element result;
    const simdjson::error_code parse_error =
        parser.load(path.string()).get(result);
    if (parse_error != simdjson::SUCCESS) {
        throw std::runtime_error(
            "cannot parse JSON evidence " + path.string() + ": "
            + simdjson::error_message(parse_error));
    }
    return result;
}

[[nodiscard]] inline simdjson::dom::element load_json_document(
    simdjson::dom::parser& parser,
    std::string_view document,
    simdjson::padded_string& retained,
    std::string_view context) {
    retained = simdjson::padded_string(document);
    simdjson::dom::element result;
    const simdjson::error_code parse_error =
        parser.parse(retained).get(result);
    if (parse_error != simdjson::SUCCESS) {
        throw std::runtime_error(
            "cannot parse retained " + std::string(context) + ": "
            + simdjson::error_message(parse_error));
    }
    return result;
}

[[nodiscard]] inline simdjson::dom::object require_object(
    simdjson::dom::element element, std::string_view context) {
    simdjson::dom::object result;
    if (element.get(result) != simdjson::SUCCESS) {
        throw std::runtime_error(
            std::string(context) + " must be a JSON object");
    }
    std::set<std::string, std::less<>> keys;
    for (const auto field : result) {
        if (!keys.emplace(std::string(field.key)).second) {
            throw std::runtime_error(
                std::string(context)
                + " contains duplicate JSON field '"
                + std::string(field.key) + "'");
        }
    }
    return result;
}

[[nodiscard]] inline simdjson::dom::object require_object(
    simdjson::dom::object parent,
    std::string_view key,
    std::string_view context) {
    simdjson::dom::element element;
    if (parent.at_key(key).get(element) != simdjson::SUCCESS) {
        throw std::runtime_error(
            std::string(context) + " is missing object field '"
            + std::string(key) + "'");
    }
    return require_object(
        element, std::string(context) + "." + std::string(key));
}

[[nodiscard]] inline simdjson::dom::array require_array(
    simdjson::dom::object parent,
    std::string_view key,
    std::string_view context) {
    simdjson::dom::array result;
    if (parent.at_key(key).get(result) != simdjson::SUCCESS) {
        throw std::runtime_error(
            std::string(context) + " is missing array field '"
            + std::string(key) + "'");
    }
    return result;
}

[[nodiscard]] inline std::string require_string(
    simdjson::dom::object parent,
    std::string_view key,
    std::string_view context) {
    std::string_view result;
    if (parent.at_key(key).get(result) != simdjson::SUCCESS) {
        throw std::runtime_error(
            std::string(context) + " is missing string field '"
            + std::string(key) + "'");
    }
    return std::string(result);
}

[[nodiscard]] inline bool require_bool(
    simdjson::dom::object parent,
    std::string_view key,
    std::string_view context) {
    bool result{};
    if (parent.at_key(key).get(result) != simdjson::SUCCESS) {
        throw std::runtime_error(
            std::string(context) + " is missing boolean field '"
            + std::string(key) + "'");
    }
    return result;
}

[[nodiscard]] inline std::uint64_t require_u64(
    simdjson::dom::object parent,
    std::string_view key,
    std::string_view context) {
    std::uint64_t result{};
    if (parent.at_key(key).get(result) != simdjson::SUCCESS) {
        throw std::runtime_error(
            std::string(context)
            + " is missing unsigned integer field '"
            + std::string(key) + "'");
    }
    return result;
}

inline void require_exact_fields(
    simdjson::dom::object object,
    std::span<const std::string_view> expected,
    std::string_view context) {
    std::set<std::string, std::less<>> actual;
    for (const auto field : object) {
        if (!actual.emplace(std::string(field.key)).second) {
            throw std::runtime_error(
                std::string(context)
                + " contains duplicate JSON field '"
                + std::string(field.key) + "'");
        }
    }
    const std::set<std::string, std::less<>> required{
        expected.begin(), expected.end()};
    if (actual != required) {
        std::ostringstream message;
        message << context << " fields differ from the exact contract";
        throw std::runtime_error(message.str());
    }
}

[[nodiscard]] inline gate_operation parse_gate_operation(
    std::string_view phase) {
    if (phase == "proof-ready") {
        return gate_operation::proof_ready;
    }
    if (phase == "retired-transition") {
        return gate_operation::retired_transition;
    }
    if (phase == "retired-final") {
        return gate_operation::retired_final;
    }
    throw std::runtime_error(
        "retirement union phase must be proof-ready, "
        "retired-transition, or retired-final");
}

[[nodiscard]] inline constexpr migration::retirement_phase lifecycle_phase(
    gate_operation operation) noexcept {
    return operation == gate_operation::proof_ready
        ? migration::retirement_phase::proof_ready
        : migration::retirement_phase::retired;
}

[[nodiscard]] inline std::string_view expected_receipt_phase(
    gate_operation operation) noexcept {
    return operation == gate_operation::retired_final
        ? "retired_with_proof" : "passing";
}

inline void validate_source_provenance(
    const source_provenance& source,
    std::string_view context) {
    require_revision(
        source.revision, std::string(context) + ".source_revision");
    require_sha256(
        source.scope_sha256,
        std::string(context) + ".source_scope_sha256");
    if (!source.scoped_clean) {
        throw std::runtime_error(
            std::string(context)
            + " was produced from a dirty source scope");
    }
}

[[nodiscard]] inline std::string json_escape(std::string_view value) {
    std::string output;
    output.reserve(value.size() + 8);
    constexpr std::string_view digits{"0123456789abcdef"};
    for (const unsigned char character : value) {
        switch (character) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (character < 0x20U) {
                output += "\\u00";
                output.push_back(digits[character >> 4U]);
                output.push_back(digits[character & 0x0fU]);
            } else {
                output.push_back(static_cast<char>(character));
            }
        }
    }
    return output;
}

}  // namespace detail

[[nodiscard]] inline bool path_is_strictly_within(
    const std::filesystem::path& path,
    const std::filesystem::path& root) {
    const std::filesystem::path relative =
        path.lexically_normal().lexically_relative(
            root.lexically_normal());
    if (relative.empty() || relative.is_absolute()) {
        return false;
    }
    const auto first = relative.begin();
    return first != relative.end() && *first != "..";
}

[[nodiscard]] inline native_artifact_evidence
load_native_artifact_evidence(
    simdjson::dom::element entry,
    std::string_view context) {
    const simdjson::dom::object object =
        detail::require_object(entry, context);
    constexpr std::array<std::string_view, 14> fields{
        "name",
        "preinstalled_stage_root",
        "stage_manifest",
        "stage_manifest_sha256",
        "control_file",
        "install_sql",
        "dso",
        "staged_control_file",
        "staged_install_sql",
        "staged_dso",
        "control_sha256",
        "install_sql_sha256",
        "dso_sha256",
        "staged_control_sha256",
    };
    detail::require_exact_fields(object, fields, context);
    native_artifact_evidence result{
        .name = detail::require_string(object, "name", context),
        .preinstalled_stage_root = detail::require_string(
            object, "preinstalled_stage_root", context),
        .stage_manifest = detail::require_string(
            object, "stage_manifest", context),
        .stage_manifest_sha256 = detail::require_string(
            object, "stage_manifest_sha256", context),
        .control_file = detail::require_string(
            object, "control_file", context),
        .install_sql = detail::require_string(
            object, "install_sql", context),
        .dso = detail::require_string(object, "dso", context),
        .staged_control_file = detail::require_string(
            object, "staged_control_file", context),
        .staged_install_sql = detail::require_string(
            object, "staged_install_sql", context),
        .staged_dso = detail::require_string(
            object, "staged_dso", context),
        .control_sha256 = detail::require_string(
            object, "control_sha256", context),
        .install_sql_sha256 = detail::require_string(
            object, "install_sql_sha256", context),
        .dso_sha256 = detail::require_string(
            object, "dso_sha256", context),
        .staged_control_sha256 = detail::require_string(
            object, "staged_control_sha256", context),
    };
    if (result.name != "bioseq_cache" && result.name != "bioseq") {
        throw std::runtime_error(
            std::string(context)
            + " names an unexpected extension artifact");
    }
    for (const auto& [path, name] :
         std::array{
             std::pair{&result.preinstalled_stage_root,
                       "preinstalled_stage_root"},
             std::pair{&result.stage_manifest, "stage_manifest"},
             std::pair{&result.control_file, "control_file"},
             std::pair{&result.install_sql, "install_sql"},
             std::pair{&result.dso, "dso"},
             std::pair{&result.staged_control_file,
                       "staged_control_file"},
             std::pair{&result.staged_install_sql,
                       "staged_install_sql"},
             std::pair{&result.staged_dso, "staged_dso"},
         }) {
        if (!path->is_absolute()
            || path->lexically_normal() != *path) {
            throw std::runtime_error(
                std::string(context) + "." + name
                + " must be a normalized absolute path");
        }
    }
    for (const auto& [digest, name] :
         std::array{
             std::pair{&result.stage_manifest_sha256,
                       "stage_manifest_sha256"},
             std::pair{&result.control_sha256, "control_sha256"},
             std::pair{&result.install_sql_sha256,
                       "install_sql_sha256"},
             std::pair{&result.dso_sha256, "dso_sha256"},
             std::pair{&result.staged_control_sha256,
                       "staged_control_sha256"},
         }) {
        detail::require_sha256(
            *digest, std::string(context) + "." + name);
    }
    if (!path_is_strictly_within(
            result.staged_control_file,
            result.preinstalled_stage_root)
        || !path_is_strictly_within(
            result.staged_install_sql,
            result.preinstalled_stage_root)) {
        throw std::runtime_error(
            std::string(context)
            + " staged control/SQL paths escape their fixed stage");
    }
    return result;
}

struct expected_native_artifact {
    std::string_view name;
    std::filesystem::path preinstalled_stage_root;
    std::filesystem::path stage_manifest;
    std::filesystem::path control_file;
    std::filesystem::path install_sql;
    std::filesystem::path dso;
};

[[nodiscard]] inline bool artifact_matches_expected(
    const native_artifact_evidence& actual,
    const expected_native_artifact& expected) {
    return actual.name == expected.name
        && actual.preinstalled_stage_root
            == expected.preinstalled_stage_root.lexically_normal()
        && actual.stage_manifest
            == expected.stage_manifest.lexically_normal()
        && actual.control_file
            == expected.control_file.lexically_normal()
        && actual.install_sql
            == expected.install_sql.lexically_normal()
        && actual.dso == expected.dso.lexically_normal();
}

inline void validate_exact_native_artifact_roster(
    std::span<const native_artifact_evidence> actual,
    std::span<const expected_native_artifact> expected,
    std::string_view context) {
    if (actual.size() != expected.size()) {
        throw std::runtime_error(
            std::string(context)
            + " artifact roster has the wrong cardinality");
    }
    for (std::size_t left = 0; left < actual.size(); ++left) {
        for (std::size_t right = left + 1U;
             right < actual.size(); ++right) {
            if (actual[left].name == actual[right].name
                && actual[left].preinstalled_stage_root
                    == actual[right].preinstalled_stage_root
                && actual[left].stage_manifest
                    == actual[right].stage_manifest
                && actual[left].control_file
                    == actual[right].control_file
                && actual[left].install_sql
                    == actual[right].install_sql
                && actual[left].dso == actual[right].dso
                && actual[left].staged_dso
                    == actual[right].staged_dso) {
                throw std::runtime_error(
                    std::string(context)
                    + " duplicates one staged artifact identity");
            }
        }
    }
    std::vector<bool> consumed(actual.size(), false);
    for (const expected_native_artifact& required : expected) {
        bool found = false;
        for (std::size_t index = 0; index < actual.size(); ++index) {
            if (!consumed[index]
                && artifact_matches_expected(actual[index], required)) {
                consumed[index] = true;
                found = true;
                break;
            }
        }
        if (!found) {
            throw std::runtime_error(
                std::string(context)
                + " artifact roster differs from its fixed invocation");
        }
    }
}

inline void validate_benchmark_artifact_roster(
    std::span<const native_artifact_evidence> artifacts,
    report_role role) {
    const std::size_t expected_count =
        role == report_role::cache_bench ? 1U : 2U;
    if (artifacts.size() != expected_count) {
        throw std::runtime_error(
            "benchmark artifact roster has the wrong cardinality");
    }
    std::set<std::string, std::less<>> names;
    for (const native_artifact_evidence& artifact : artifacts) {
        names.insert(artifact.name);
    }
    const std::set<std::string, std::less<>> expected =
        role == report_role::cache_bench
        ? std::set<std::string, std::less<>>{"bioseq_cache"}
        : std::set<std::string, std::less<>>{
              "bioseq_cache", "bioseq"};
    if (names != expected) {
        throw std::runtime_error(
            "benchmark artifact roster differs from its fixed role");
    }
}

inline void validate_shared_artifact_path_hashes(
    std::span<const native_artifact_evidence> artifacts,
    std::string_view context) {
    for (std::size_t left = 0; left < artifacts.size(); ++left) {
        for (std::size_t right = left + 1U;
             right < artifacts.size(); ++right) {
            const native_artifact_evidence& lhs = artifacts[left];
            const native_artifact_evidence& rhs = artifacts[right];
            if ((lhs.stage_manifest == rhs.stage_manifest
                 && lhs.stage_manifest_sha256
                     != rhs.stage_manifest_sha256)
                || (lhs.control_file == rhs.control_file
                    && lhs.control_sha256 != rhs.control_sha256)
                || (lhs.install_sql == rhs.install_sql
                    && lhs.install_sql_sha256
                        != rhs.install_sql_sha256)
                || (lhs.dso == rhs.dso
                    && lhs.dso_sha256 != rhs.dso_sha256)
                || (lhs.staged_control_file
                        == rhs.staged_control_file
                    && lhs.staged_control_sha256
                        != rhs.staged_control_sha256)) {
                throw std::runtime_error(
                    std::string(context)
                    + " gives one artifact path conflicting hashes");
            }
        }
    }
}

[[nodiscard]] inline legacy_inventory load_legacy_inventory_document(
    std::string_view document) {
    std::istringstream input{std::string(document)};
    std::string line;
    if (!std::getline(input, line)) {
        throw std::runtime_error("legacy asset inventory is empty");
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (line != "legacy_path\tsha256") {
        throw std::runtime_error(
            "legacy asset inventory has an unexpected header");
    }

    legacy_inventory result;
    std::string previous_path;
    std::size_t line_number = 1;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::size_t separator = line.find('\t');
        if (separator == std::string::npos
            || line.find('\t', separator + 1U) != std::string::npos) {
            throw std::runtime_error(
                "legacy asset inventory line "
                + std::to_string(line_number)
                + " must contain exactly path and SHA-256");
        }
        const std::string path_text = line.substr(0, separator);
        const std::string sha256 = line.substr(separator + 1U);
        const std::filesystem::path asset{path_text};
        if (path_text.empty() || asset.is_absolute()
            || asset.generic_string() != path_text
            || asset.lexically_normal() != asset) {
            throw std::runtime_error(
                "legacy asset inventory line "
                + std::to_string(line_number)
                + " is not a normalized module-relative path");
        }
        for (const auto& component : asset) {
            if (component.empty() || component == "."
                || component == "..") {
                throw std::runtime_error(
                    "legacy asset inventory line "
                    + std::to_string(line_number)
                    + " contains an unsafe path component");
            }
        }
        detail::require_sha256(sha256, "legacy asset inventory SHA-256");
        if (!previous_path.empty() && path_text <= previous_path) {
            throw std::runtime_error(
                "legacy asset inventory paths must be unique and "
                "strictly sorted");
        }
        previous_path = path_text;
        result.assets.push_back(
            legacy_asset{.path = asset, .sha256 = sha256});
    }
    if (input.bad()) {
        throw std::runtime_error(
            "failed while reading legacy asset inventory");
    }
    if (result.assets.size() != 85U) {
        throw std::runtime_error(
            "legacy asset inventory must contain exactly 85 assets");
    }
    return result;
}

[[nodiscard]] inline legacy_inventory load_legacy_inventory(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "cannot read legacy asset inventory: " + path.string());
    }
    const std::string document{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
    if (input.bad()) {
        throw std::runtime_error(
            "failed while reading legacy asset inventory: "
            + path.string());
    }
    return load_legacy_inventory_document(document);
}

[[nodiscard]] inline bool has_legacy_script_or_sql_suffix(
    std::string_view path) noexcept {
    constexpr std::array<std::string_view, 6> suffixes{
        ".py", ".pyc", ".pl", ".pm", ".sql", ".sql.in"};
    return std::ranges::any_of(
        suffixes, [path](std::string_view suffix) {
            return path.ends_with(suffix);
    });
}

[[nodiscard]] inline bool is_absent_path_error(
    const std::error_code& error) noexcept {
    return error
            == std::make_error_code(
                std::errc::no_such_file_or_directory)
        || error
            == std::make_error_code(std::errc::not_a_directory);
}

inline void reject_symlink_components(
    const std::filesystem::path& root,
    const std::filesystem::path& relative) {
    std::error_code error;
    const auto root_status = std::filesystem::symlink_status(root, error);
    if (error || !std::filesystem::is_directory(root_status)
        || std::filesystem::is_symlink(root_status)) {
        throw std::runtime_error(
            "legacy module root is not a regular non-symlink directory: "
            + root.string());
    }
    std::filesystem::path current = root;
    for (const auto& component : relative) {
        current /= component;
        const auto status =
            std::filesystem::symlink_status(current, error);
        if (error) {
            if (is_absent_path_error(error)) {
                return;
            }
            throw std::runtime_error(
                "cannot inspect legacy path component "
                + current.string() + ": " + error.message());
        }
        if (!std::filesystem::exists(status)) {
            return;
        }
        if (std::filesystem::is_symlink(status)) {
            throw std::runtime_error(
                "legacy path contains a symlink component: "
                + current.string());
        }
    }
}

[[nodiscard]] inline std::set<std::string, std::less<>>
discover_legacy_candidates(const std::filesystem::path& root) {
    std::set<std::string, std::less<>> result;
    const auto record_tree =
        [&root, &result](const std::filesystem::path& relative_root) {
            reject_symlink_components(root, relative_root);
            const std::filesystem::path directory = root / relative_root;
            std::error_code status_error;
            if (!std::filesystem::exists(
                    std::filesystem::symlink_status(
                        directory, status_error))) {
                if (status_error
                    && !is_absent_path_error(status_error)) {
                    throw std::runtime_error(
                        "cannot inspect legacy candidate directory "
                        + directory.string() + ": "
                        + status_error.message());
                }
                return;
            }
            std::error_code iteration_error;
            std::filesystem::recursive_directory_iterator iterator{
                directory,
                std::filesystem::directory_options::none,
                iteration_error};
            const std::filesystem::recursive_directory_iterator end;
            while (iterator != end) {
                if (iteration_error) {
                    throw std::runtime_error(
                        "cannot enumerate legacy candidate directory "
                        + directory.string() + ": "
                        + iteration_error.message());
                }
                const auto status =
                    iterator->symlink_status(iteration_error);
                if (iteration_error) {
                    throw std::runtime_error(
                        "cannot inspect legacy candidate "
                        + iterator->path().string() + ": "
                        + iteration_error.message());
                }
                if (std::filesystem::is_symlink(status)
                    || std::filesystem::is_regular_file(status)) {
                    result.insert(
                        iterator->path()
                            .lexically_relative(root)
                            .generic_string());
                }
                iterator.increment(iteration_error);
            }
            if (iteration_error) {
                throw std::runtime_error(
                    "cannot enumerate legacy candidate directory "
                    + directory.string() + ": "
                    + iteration_error.message());
            }
        };
    record_tree("test/regress");
    record_tree("test/isolation");
    record_tree("test/tap");

    std::error_code iteration_error;
    std::filesystem::recursive_directory_iterator iterator{
        root,
        std::filesystem::directory_options::none,
        iteration_error};
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        if (iteration_error) {
            throw std::runtime_error(
                "cannot enumerate cache module for legacy candidates: "
                + iteration_error.message());
        }
        const auto status = iterator->symlink_status(iteration_error);
        if (iteration_error) {
            throw std::runtime_error(
                "cannot inspect cache-module candidate "
                + iterator->path().string() + ": "
                + iteration_error.message());
        }
        if (std::filesystem::is_symlink(status)
            || std::filesystem::is_regular_file(status)) {
            const std::string relative =
                iterator->path()
                    .lexically_relative(root)
                    .generic_string();
            if (has_legacy_script_or_sql_suffix(relative)
                || relative == "bench/sql_cases.json") {
                result.insert(relative);
            }
        }
        iterator.increment(iteration_error);
    }
    if (iteration_error) {
        throw std::runtime_error(
            "cannot enumerate cache module for legacy candidates: "
            + iteration_error.message());
    }
    return result;
}

inline void validate_legacy_inventory(
    const legacy_inventory& inventory,
    const std::filesystem::path& root,
    migration::retirement_phase phase,
    const std::function<std::string(const std::filesystem::path&)>&
        sha256_file) {
    std::set<std::string, std::less<>> expected;
    std::size_t present = 0;
    std::size_t absent = 0;
    reject_symlink_components(root, {});
    const std::filesystem::path canonical_root =
        std::filesystem::canonical(root);
    for (const legacy_asset& asset : inventory.assets) {
        const std::string relative = asset.path.generic_string();
        expected.insert(relative);
        reject_symlink_components(canonical_root, asset.path);
        const std::filesystem::path absolute = canonical_root / asset.path;
        std::error_code error;
        const auto status =
            std::filesystem::symlink_status(absolute, error);
        if (error) {
            if (is_absent_path_error(error)) {
                ++absent;
                continue;
            }
            throw std::runtime_error(
                "cannot inspect exact legacy asset "
                + absolute.string() + ": " + error.message());
        }
        if (!std::filesystem::exists(status)) {
            ++absent;
            continue;
        }
        if (!std::filesystem::is_regular_file(status)
            || std::filesystem::is_symlink(status)) {
            throw std::runtime_error(
                "exact legacy asset is not a regular non-symlink file: "
                + absolute.string());
        }
        const std::filesystem::path canonical_asset =
            std::filesystem::canonical(absolute);
        const auto relative_to_root =
            canonical_asset.lexically_relative(canonical_root);
        const std::string relative_to_root_text =
            relative_to_root.generic_string();
        if (relative_to_root.empty()
            || relative_to_root_text == ".."
            || relative_to_root_text.starts_with("../")) {
            throw std::runtime_error(
                "exact legacy asset escapes the canonical module root: "
                + absolute.string());
        }
        if (sha256_file(absolute) != asset.sha256) {
            throw std::runtime_error(
                "exact legacy asset SHA-256 changed: " + relative);
        }
        ++present;
    }
    if (present != 0U && absent != 0U) {
        throw std::runtime_error(
            "legacy asset deletion is partial; transition must be atomic");
    }

    const auto candidates = discover_legacy_candidates(canonical_root);
    if (phase == migration::retirement_phase::proof_ready) {
        if (absent != 0U || candidates != expected) {
            throw std::runtime_error(
                "proof-ready legacy candidates differ from the exact "
                "85-asset inventory");
        }
        return;
    }
    if (phase != migration::retirement_phase::retired) {
        throw std::runtime_error(
            "final inventory validator requires proof-ready or retired");
    }
    if (present != 0U || !candidates.empty()) {
        throw std::runtime_error(
            "retired cache module retains a listed or newly discovered "
            "Perl/Python/raw-SQL legacy candidate");
    }
}

[[nodiscard]] inline std::vector<std::filesystem::path>
inventory_paths(const legacy_inventory& inventory) {
    std::vector<std::filesystem::path> result;
    result.reserve(inventory.assets.size());
    for (const legacy_asset& asset : inventory.assets) {
        result.push_back(asset.path);
    }
    return result;
}

[[nodiscard]] inline gate_bundle load_gate_bundle(
    const std::filesystem::path& path,
    std::string_view retained_document = {}) {
    simdjson::dom::parser parser;
    simdjson::padded_string retained;
    const simdjson::dom::object root = detail::require_object(
        retained_document.empty()
            ? detail::load_json(parser, path)
            : detail::load_json_document(
                  parser, retained_document, retained,
                  "retirement gate bundle"),
        "retirement gate bundle");
    constexpr std::array<std::string_view, 16> fields{
        "schema_version",
        "phase",
        "ledger",
        "catch_cases",
        "legacy_root",
        "legacy_inventory",
        "proof_receipt",
        "current_source_revision",
        "current_source_scope_sha256",
        "current_source_scoped_clean",
        "retirement_transition_sha256",
        "unit_receipt",
        "evidence_receipts",
        "proof_unit_receipt",
        "proof_evidence_receipts",
        "transition_receipt",
    };
    detail::require_exact_fields(root, fields, "retirement gate bundle");
    if (detail::require_string(
            root, "schema_version", "retirement gate bundle")
        != bundle_schema) {
        throw std::runtime_error(
            "retirement gate bundle has an unsupported schema");
    }

    const gate_operation operation =
        detail::parse_gate_operation(detail::require_string(
            root, "phase", "retirement gate bundle"));
    gate_bundle result{
        .operation = operation,
        .phase = detail::lifecycle_phase(operation),
        .ledger = detail::require_string(
            root, "ledger", "retirement gate bundle"),
        .catch_cases = detail::require_string(
            root, "catch_cases", "retirement gate bundle"),
        .legacy_root = detail::require_string(
            root, "legacy_root", "retirement gate bundle"),
        .legacy_inventory = detail::require_string(
            root, "legacy_inventory", "retirement gate bundle"),
        .proof_receipt = detail::require_string(
            root, "proof_receipt", "retirement gate bundle"),
        .current_source = {
            .revision = detail::require_string(
                root, "current_source_revision",
                "retirement gate bundle"),
            .scope_sha256 = detail::require_string(
                root, "current_source_scope_sha256",
                "retirement gate bundle"),
            .scoped_clean = detail::require_bool(
                root, "current_source_scoped_clean",
                "retirement gate bundle")},
        .retirement_transition_sha256 = detail::require_string(
            root, "retirement_transition_sha256",
            "retirement gate bundle"),
        .unit_receipt = detail::require_string(
            root, "unit_receipt", "retirement gate bundle"),
        .proof_unit_receipt = detail::require_string(
            root, "proof_unit_receipt", "retirement gate bundle"),
        .transition_receipt = detail::require_string(
            root, "transition_receipt", "retirement gate bundle")};
    detail::validate_source_provenance(
        result.current_source, "retirement gate bundle");
    for (const simdjson::dom::element entry :
         detail::require_array(
             root, "evidence_receipts", "retirement gate bundle")) {
        std::string_view receipt;
        if (entry.get(receipt) != simdjson::SUCCESS || receipt.empty()) {
            throw std::runtime_error(
                "retirement gate evidence receipt paths must be "
                "nonempty strings");
        }
        result.evidence_receipts.emplace_back(receipt);
    }
    if (result.evidence_receipts.size() != required_report_roles.size()) {
        throw std::runtime_error(
            "retirement gate requires exactly five evidence receipts");
    }
    for (const simdjson::dom::element entry :
         detail::require_array(
             root, "proof_evidence_receipts",
             "retirement gate bundle")) {
        std::string_view receipt;
        if (entry.get(receipt) != simdjson::SUCCESS || receipt.empty()) {
            throw std::runtime_error(
                "retirement gate proof evidence receipt paths must be "
                "nonempty strings");
        }
        result.proof_evidence_receipts.emplace_back(receipt);
    }
    if (result.operation == gate_operation::proof_ready) {
        if (!result.retirement_transition_sha256.empty()
            || !result.proof_unit_receipt.empty()
            || !result.proof_evidence_receipts.empty()
            || !result.transition_receipt.empty()) {
            throw std::runtime_error(
                "proof-ready gate must not claim historical or "
                "transition evidence");
        }
    } else {
        detail::require_sha256(
            result.retirement_transition_sha256,
            "retirement_transition_sha256");
        if (result.transition_receipt.empty()) {
            throw std::runtime_error(
                "retired gate requires a transition receipt path");
        }
        if (result.operation == gate_operation::retired_transition) {
            if (!result.proof_unit_receipt.empty()
                || !result.proof_evidence_receipts.empty()) {
                throw std::runtime_error(
                    "transition gate carries proof receipts in the "
                    "primary historical roster only");
            }
        } else if (result.proof_unit_receipt.empty()
                   || result.proof_evidence_receipts.size()
                       != required_report_roles.size()) {
            throw std::runtime_error(
                "retired-final gate requires the proof-sealed unit and "
                "exact five historical report receipts");
        }
    }
    return result;
}

[[nodiscard]] inline unit_receipt load_unit_receipt(
    const std::filesystem::path& path,
    std::string receipt_sha256,
    std::string_view retained_document = {}) {
    simdjson::dom::parser parser;
    simdjson::padded_string retained;
    const simdjson::dom::object root = detail::require_object(
        retained_document.empty()
            ? detail::load_json(parser, path)
            : detail::load_json_document(
                  parser, retained_document, retained,
                  "MCP unit receipt"),
        "MCP unit receipt");
    constexpr std::array<std::string_view, 10> fields{
        "schema_version",
        "mode",
        "status",
        "source_revision",
        "source_scope_sha256",
        "source_scoped_clean",
        "migration_ledger_sha256",
        "evidence_contract_sha256",
        "migration_phase",
        "commands",
    };
    detail::require_exact_fields(root, fields, "MCP unit receipt");
    if (detail::require_string(
            root, "schema_version", "MCP unit receipt")
            != unit_receipt_schema
        || detail::require_string(root, "mode", "MCP unit receipt")
            != "cache-unit"
        || detail::require_string(root, "status", "MCP unit receipt")
            != "passed") {
        throw std::runtime_error(
            "MCP unit receipt schema/mode/status is invalid");
    }
    unit_receipt result{
        .receipt_path = path,
        .receipt_sha256 = std::move(receipt_sha256),
        .source = {
            .revision = detail::require_string(
                root, "source_revision", "MCP unit receipt"),
            .scope_sha256 = detail::require_string(
                root, "source_scope_sha256", "MCP unit receipt"),
            .scoped_clean = detail::require_bool(
                root, "source_scoped_clean", "MCP unit receipt")},
        .migration_ledger_sha256 = detail::require_string(
            root, "migration_ledger_sha256", "MCP unit receipt"),
        .evidence_contract_sha256 = detail::require_string(
            root, "evidence_contract_sha256", "MCP unit receipt"),
        .migration_phase = detail::require_string(
            root, "migration_phase", "MCP unit receipt")};
    detail::require_sha256(result.receipt_sha256, "unit receipt SHA-256");
    detail::require_sha256(
        result.migration_ledger_sha256,
        "unit migration_ledger_sha256");
    detail::require_sha256(
        result.evidence_contract_sha256,
        "unit evidence_contract_sha256");
    detail::validate_source_provenance(result.source, "MCP unit receipt");
    for (const simdjson::dom::element entry :
         detail::require_array(root, "commands", "MCP unit receipt")) {
        const auto object =
            detail::require_object(entry, "MCP unit command");
        constexpr std::array<std::string_view, 3> command_fields{
            "executable", "executable_sha256", "arguments"};
        detail::require_exact_fields(
            object, command_fields, "MCP unit command");
        unit_command_receipt command{
            .executable = detail::require_string(
                object, "executable", "MCP unit command"),
            .executable_sha256 = detail::require_string(
                object, "executable_sha256", "MCP unit command")};
        detail::require_sha256(
            command.executable_sha256,
            "unit command executable_sha256");
        for (const simdjson::dom::element argument :
             detail::require_array(
                 object, "arguments", "MCP unit command")) {
            std::string_view value;
            if (argument.get(value) != simdjson::SUCCESS) {
                throw std::runtime_error(
                    "MCP unit command arguments must be strings");
            }
            command.arguments.emplace_back(value);
        }
        result.commands.push_back(std::move(command));
    }
    constexpr std::array<std::string_view, 5> required_commands{
        "bioseq_cache_unit_tests",
        "bioseq_postgres_test_support_contract_tests",
        "bioseq_cache_benchmark_report_contract_tests",
        "bioseq_durable_file_io_contract_tests",
        "bioseq_cache_legacy_inventory_tracking"};
    if (result.commands.size() != required_commands.size()) {
        throw std::runtime_error(
            "MCP unit receipt must seal exactly five C++ contract "
            "executables");
    }
    std::set<std::string, std::less<>> actual;
    for (const unit_command_receipt& command : result.commands) {
        actual.insert(command.executable.filename().string());
    }
    const std::set<std::string, std::less<>> expected{
        required_commands.begin(), required_commands.end()};
    if (actual != expected) {
        throw std::runtime_error(
            "MCP unit receipt command roster is not exact");
    }
    return result;
}

inline void validate_unit_receipt(
    const unit_receipt& receipt,
    const gate_bundle& bundle,
    std::string_view current_ledger_sha256,
    std::string_view current_evidence_contract_sha256,
    const std::function<std::string(const std::filesystem::path&)>&
        sha256_file) {
    const std::string_view phase =
        detail::expected_receipt_phase(bundle.operation);
    if (receipt.source.revision != bundle.current_source.revision
        || receipt.source.scope_sha256
            != bundle.current_source.scope_sha256
        || receipt.migration_ledger_sha256 != current_ledger_sha256
        || receipt.evidence_contract_sha256
            != current_evidence_contract_sha256
        || receipt.migration_phase != phase) {
        throw std::runtime_error(
            "cache-unit MCP receipt is stale for the current retirement "
            "gate inputs");
    }
    for (const unit_command_receipt& command : receipt.commands) {
        if (!command.executable.is_absolute()
            || sha256_file(command.executable)
                != command.executable_sha256) {
            throw std::runtime_error(
                "cache-unit MCP receipt executable identity is stale");
        }
        const std::string basename =
            command.executable.filename().string();
        if (basename == "bioseq_cache_legacy_inventory_tracking") {
            const std::vector<std::string> expected{
                "--legacy-root",
                bundle.legacy_root.string(),
                "--legacy-inventory",
                bundle.legacy_inventory.string(),
                "--phase",
                "current",
                "--inventory-only",
            };
            if (command.arguments != expected) {
                throw std::runtime_error(
                    "cache-unit legacy inventory command arguments are "
                    "not exact");
            }
        } else {
            const std::vector<std::string> expected{
                "--reporter",
                "console::out=-::colour-mode=ansi",
            };
            if (command.arguments != expected) {
                throw std::runtime_error(
                    "cache-unit Catch2 command arguments are not exact");
            }
        }
    }
}

[[nodiscard]] inline evidence_receipt load_evidence_receipt(
    const std::filesystem::path& path,
    std::string receipt_sha256,
    std::string_view retained_document = {}) {
    simdjson::dom::parser parser;
    simdjson::padded_string retained;
    const simdjson::dom::object root = detail::require_object(
        retained_document.empty()
            ? detail::load_json(parser, path)
            : detail::load_json_document(
                  parser, retained_document, retained,
                  "MCP evidence receipt"),
        "MCP evidence receipt");
    constexpr std::array<std::string_view, 14> fields{
        "schema_version",
        "mode",
        "report_role",
        "native_report",
        "native_report_sha256",
        "executable",
        "executable_sha256",
        "scenario",
        "source_revision",
        "source_scope_sha256",
        "source_scoped_clean",
        "migration_ledger_sha256",
        "evidence_contract_sha256",
        "migration_phase",
    };
    detail::require_exact_fields(root, fields, "MCP evidence receipt");
    if (detail::require_string(
            root, "schema_version", "MCP evidence receipt")
        != receipt_schema) {
        throw std::runtime_error(
            "MCP evidence receipt has an unsupported schema");
    }

    evidence_receipt result{
        .receipt_path = path,
        .receipt_sha256 = std::move(receipt_sha256),
        .role = parse_role(detail::require_string(
            root, "report_role", "MCP evidence receipt")),
        .mode = detail::require_string(
            root, "mode", "MCP evidence receipt"),
        .native_report = detail::require_string(
            root, "native_report", "MCP evidence receipt"),
        .native_report_sha256 = detail::require_string(
            root, "native_report_sha256", "MCP evidence receipt"),
        .executable = detail::require_string(
            root, "executable", "MCP evidence receipt"),
        .executable_sha256 = detail::require_string(
            root, "executable_sha256", "MCP evidence receipt"),
        .scenario = detail::require_string(
            root, "scenario", "MCP evidence receipt"),
        .source = {
            .revision = detail::require_string(
                root, "source_revision", "MCP evidence receipt"),
            .scope_sha256 = detail::require_string(
                root, "source_scope_sha256", "MCP evidence receipt"),
            .scoped_clean = detail::require_bool(
                root, "source_scoped_clean", "MCP evidence receipt")},
        .migration_ledger_sha256 = detail::require_string(
            root, "migration_ledger_sha256", "MCP evidence receipt"),
        .evidence_contract_sha256 = detail::require_string(
            root, "evidence_contract_sha256", "MCP evidence receipt"),
        .migration_phase = detail::require_string(
            root, "migration_phase", "MCP evidence receipt")};
    detail::require_sha256(result.receipt_sha256, "receipt SHA-256");
    detail::require_sha256(
        result.native_report_sha256, "native_report_sha256");
    detail::require_sha256(
        result.executable_sha256, "receipt executable_sha256");
    detail::require_sha256(
        result.migration_ledger_sha256, "migration_ledger_sha256");
    detail::require_sha256(
        result.evidence_contract_sha256, "evidence_contract_sha256");
    detail::validate_source_provenance(result.source, "MCP evidence receipt");
    if (result.mode != role_mode(result.role)) {
        throw std::runtime_error(
            "MCP evidence receipt role "
            + std::string(report_role_name(result.role))
            + " is incompatible with mode " + result.mode);
    }
    if (!result.executable.is_absolute()
        || result.executable.filename().string()
            != role_executable_name(result.role)
        || result.scenario != role_scenario(result.role)) {
        throw std::runtime_error(
            "MCP evidence receipt executable/scenario is incompatible "
            "with its fixed report role");
    }
    return result;
}

[[nodiscard]] inline native_report_evidence
load_integration_report(
    const std::filesystem::path& path,
    std::string_view retained_document = {}) {
    simdjson::dom::parser parser;
    simdjson::padded_string retained;
    const simdjson::dom::object root = detail::require_object(
        retained_document.empty()
            ? detail::load_json(parser, path)
            : detail::load_json_document(
                  parser, retained_document, retained,
                  "integration report"),
        "integration report");
    if (detail::require_string(root, "schema_version", "integration report")
        != integration_schema) {
        throw std::runtime_error(
            "integration evidence must use bioseq-cache-test-v2");
    }
    const report_role role = parse_role(detail::require_string(
        root, "migration_report_role", "integration report"));
    if (!is_integration_role(role)) {
        throw std::runtime_error(
            "benchmark role appeared in an integration report");
    }
    const migration::evidence_harness owner =
        migration::parse_evidence_harness(detail::require_string(
            root, "migration_evidence_harness", "integration report"));
    if (owner != role_owner(role)) {
        throw std::runtime_error(
            "integration report role and evidence owner disagree");
    }
    if (detail::require_string(
            root, "migration_coverage", "integration report")
        != "full-role") {
        throw std::runtime_error(
            "integration report did not execute full-role migration "
            "coverage");
    }

    std::vector<std::string> reported_arguments;
    std::size_t argument_index = 0;
    for (const simdjson::dom::element entry :
         detail::require_array(root, "arguments", "integration report")) {
        const auto object =
            detail::require_object(entry, "integration report argument");
        constexpr std::array<std::string_view, 3> argument_fields{
            "name", "value", "redacted"};
        detail::require_exact_fields(
            object, argument_fields, "integration report argument");
        if (detail::require_string(
                object, "name", "integration report argument")
                != "argv[" + std::to_string(argument_index) + "]"
            || detail::require_bool(
                object, "redacted", "integration report argument")) {
            throw std::runtime_error(
                "integration report arguments are not an exact "
                "unredacted argv sequence");
        }
        reported_arguments.push_back(detail::require_string(
            object, "value", "integration report argument"));
        ++argument_index;
    }
    if (reported_arguments.empty()) {
        throw std::runtime_error(
            "integration report contains no executed argv");
    }
    constexpr std::array<std::string_view, 20> allowed_options{
        "--postgres-bin-dir",
        "--postgres-stage-root",
        "--postgres-stage-manifest",
        "--bioseq-only-postgres-bin-dir",
        "--bioseq-only-postgres-stage-root",
        "--bioseq-only-postgres-stage-manifest",
        "--cache-control",
        "--cache-install-sql",
        "--cache-dso",
        "--bioseq-control",
        "--bioseq-install-sql",
        "--bioseq-dso",
        "--provider-peer-test-dso",
        "--migration-ledger",
        "--migration-harness",
        "--migration-report-role",
        "--report",
        "--case-timeout-seconds",
        "--large-case-timeout-seconds",
        "--reporter",
    };
    std::map<std::string, std::string, std::less<>> reported_options;
    for (std::size_t index = 1; index < reported_arguments.size();
         index += 2U) {
        const std::string& option = reported_arguments[index];
        if (index + 1U >= reported_arguments.size()
            || !std::ranges::contains(allowed_options, option)
            || !reported_options.emplace(
                    option, reported_arguments[index + 1U]).second) {
            throw std::runtime_error(
                "integration report argv contains a Catch2 filter, "
                "unknown option, duplicate option, or missing value");
        }
    }
    const auto required_option =
        [&reported_options](std::string_view option) -> std::string_view {
        const auto found = reported_options.find(option);
        if (found == reported_options.end()) {
            throw std::runtime_error(
                "integration report argv omits required option "
                + std::string(option));
        }
        return found->second;
    };
    if (required_option("--migration-harness")
            != migration::harness_name(owner)
        || required_option("--migration-report-role")
            != report_role_name(role)
        || (reported_options.contains("--reporter")
            && reported_options.at("--reporter")
                != "console::out=-::colour-mode=ansi")) {
        throw std::runtime_error(
            "integration report argv is not bound to its fixed "
            "migration harness/role");
    }

    std::size_t case_count = 0;
    for (const simdjson::dom::element entry :
         detail::require_array(root, "cases", "integration report")) {
        ++case_count;
        const auto object =
            detail::require_object(entry, "integration report case");
        if (detail::require_string(
                object, "status", "integration report case")
            != "passed") {
            throw std::runtime_error(
                "integration report contains a non-passing case");
        }
    }
    if (case_count == 0) {
        throw std::runtime_error(
            "integration report contains no executed cases");
    }
    const auto cleanup =
        detail::require_object(root, "cleanup", "integration report");
    if (!detail::require_bool(
            cleanup, "succeeded", "integration report cleanup")) {
        throw std::runtime_error(
            "integration report cleanup did not succeed");
    }
    std::vector<std::filesystem::path> cleanup_roots;
    for (const simdjson::dom::element entry :
         detail::require_array(
             root, "cleanup_observations", "integration report")) {
        const auto object = detail::require_object(
            entry, "integration cleanup observation");
        if (!detail::require_bool(
                object, "succeeded",
                "integration cleanup observation")) {
            throw std::runtime_error(
                "integration report contains failed cleanup evidence");
        }
        std::filesystem::path cleanup_root = detail::require_string(
            object, "root", "integration cleanup observation");
        if (!cleanup_root.is_absolute()
            || cleanup_root.lexically_normal() != cleanup_root
            || !cleanup_root.filename().string().starts_with(
                "bioseq-pg-test-")) {
            throw std::runtime_error(
                "integration cleanup root is not a normalized "
                "absolute marker-owned path");
        }
        std::error_code parent_error;
        const std::filesystem::path expected_cleanup_parent =
            std::filesystem::canonical(
                std::filesystem::temp_directory_path(),
                parent_error);
        if (parent_error
            || !cleanup_root.is_absolute()
            || !cleanup_root.filename().string().starts_with(
                "bioseq-pg-test-")
            || cleanup_root.parent_path()
                != expected_cleanup_parent) {
            throw std::runtime_error(
                "integration cleanup root cannot be canonically "
                "resolved below the fixed temporary parent");
        }
        cleanup_roots.push_back(std::move(cleanup_root));
    }
    if (cleanup_roots.empty()) {
        throw std::runtime_error(
            "integration report contains no cleanup observations");
    }
    const auto cluster = detail::require_object(
        root, "cluster_configuration", "integration report");
    if (detail::require_string(
            cluster, "retained-supervision-state",
            "integration cluster configuration")
        != "none") {
        throw std::runtime_error(
            "integration report retained supervision state");
    }

    native_report_evidence result{
        .role = role,
        .owner = owner,
        .migration_ledger_sha256 = detail::require_string(
            root, "migration_ledger_sha256", "integration report"),
        .evidence_contract_sha256 = detail::require_string(
            root, "evidence_contract_sha256", "integration report"),
        .migration_phase = detail::require_string(
            root, "migration_phase", "integration report"),
        .executable = detail::require_string(
            root, "executable", "integration report"),
        .executable_sha256 = detail::require_string(
            root, "build_identity", "integration report"),
        .scenario = std::string(role_scenario(role))};
    result.successful_cleanup_roots = std::move(cleanup_roots);
    detail::require_sha256(
        result.migration_ledger_sha256,
        "integration migration_ledger_sha256");
    detail::require_sha256(
        result.evidence_contract_sha256,
        "integration evidence_contract_sha256");
    detail::require_sha256(
        result.executable_sha256, "integration build_identity");
    if (!result.executable.is_absolute()
        || std::filesystem::path(reported_arguments.front())
                .lexically_normal()
            != result.executable.lexically_normal()) {
        throw std::runtime_error(
            "integration report argv[0] does not name its executed "
            "absolute binary");
    }
    const auto option_path =
        [&required_option](std::string_view option) {
            return std::filesystem::path{
                required_option(option)}.lexically_normal();
        };
    const std::filesystem::path primary_stage_root =
        option_path("--postgres-stage-root");
    const std::filesystem::path primary_stage_manifest =
        option_path("--postgres-stage-manifest");
    const std::filesystem::path cache_control =
        option_path("--cache-control");
    const std::filesystem::path cache_install_sql =
        option_path("--cache-install-sql");
    const std::filesystem::path cache_dso =
        option_path("--cache-dso");
    std::vector<expected_native_artifact> expected_artifacts{
        expected_native_artifact{
            .name = "bioseq_cache",
            .preinstalled_stage_root = primary_stage_root,
            .stage_manifest = primary_stage_manifest,
            .control_file = cache_control,
            .install_sql = cache_install_sql,
            .dso = cache_dso,
        },
    };
    const bool carries_bioseq =
        role != report_role::cache_integration;
    if (carries_bioseq) {
        expected_artifacts.push_back(expected_native_artifact{
            .name = "bioseq",
            .preinstalled_stage_root = primary_stage_root,
            .stage_manifest = primary_stage_manifest,
            .control_file = option_path("--bioseq-control"),
            .install_sql = option_path("--bioseq-install-sql"),
            .dso = option_path("--bioseq-dso"),
        });
    }
    if (role
        == report_role::cache_bioseq_support_integration) {
        expected_artifacts.push_back(expected_native_artifact{
            .name = "bioseq",
            .preinstalled_stage_root =
                option_path("--bioseq-only-postgres-stage-root"),
            .stage_manifest =
                option_path("--bioseq-only-postgres-stage-manifest"),
            .control_file = option_path("--bioseq-control"),
            .install_sql = option_path("--bioseq-install-sql"),
            .dso = option_path("--bioseq-dso"),
        });
        for (std::size_t section = 0; section < 2U; ++section) {
            expected_artifacts.push_back(expected_native_artifact{
                .name = "bioseq_cache",
                .preinstalled_stage_root = primary_stage_root,
                .stage_manifest = primary_stage_manifest,
                .control_file = cache_control,
                .install_sql = cache_install_sql,
                .dso = option_path("--provider-peer-test-dso"),
            });
        }
    } else {
        for (const std::string_view forbidden :
             {"--bioseq-only-postgres-bin-dir",
              "--bioseq-only-postgres-stage-root",
              "--bioseq-only-postgres-stage-manifest",
              "--provider-peer-test-dso"}) {
            if (reported_options.contains(forbidden)) {
                throw std::runtime_error(
                    "integration report argv carries support-only "
                    "artifact inputs for the wrong fixed role");
            }
        }
    }
    if (!carries_bioseq) {
        for (const std::string_view forbidden :
             {"--bioseq-control",
              "--bioseq-install-sql",
              "--bioseq-dso"}) {
            if (reported_options.contains(forbidden)) {
                throw std::runtime_error(
                    "cache-only integration report carries Bioseq "
                    "artifact inputs");
            }
        }
    }
    for (const simdjson::dom::element entry :
         detail::require_array(root, "artifacts", "integration report")) {
        result.artifacts.push_back(load_native_artifact_evidence(
            entry, "integration report artifact"));
    }
    validate_exact_native_artifact_roster(
        result.artifacts, expected_artifacts, "integration report");
    validate_shared_artifact_path_hashes(
        result.artifacts, "integration report");
    const auto canonical_cache = std::ranges::find_if(
        result.artifacts,
        [&primary_stage_root, &primary_stage_manifest, &cache_dso](
            const native_artifact_evidence& artifact) {
            return artifact.name == "bioseq_cache"
                && artifact.preinstalled_stage_root
                    == primary_stage_root
                && artifact.stage_manifest
                    == primary_stage_manifest
                && artifact.dso == cache_dso;
        });
    if (canonical_cache == result.artifacts.end()) {
        throw std::runtime_error(
            "integration report omits its canonical cache artifact");
    }
    for (const native_artifact_evidence& artifact :
         result.artifacts) {
        const bool provider_override =
            role
                == report_role::
                    cache_bioseq_support_integration
            && artifact.name == "bioseq_cache"
            && artifact.dso
                == option_path("--provider-peer-test-dso");
        if (!provider_override) {
            if (!path_is_strictly_within(
                    artifact.staged_dso,
                    artifact.preinstalled_stage_root)) {
                throw std::runtime_error(
                    "integration report persistent staged DSO escapes "
                    "its fixed stage");
            }
            continue;
        }
        const std::filesystem::path runtime_directory =
            artifact.staged_dso.parent_path();
        const std::filesystem::path cleanup_root =
            runtime_directory.parent_path();
        if (runtime_directory.filename() != "runtime-library"
            || artifact.staged_dso.filename()
                != canonical_cache->staged_dso.filename()
            || artifact.staged_control_file
                != canonical_cache->staged_control_file
            || artifact.staged_install_sql
                != canonical_cache->staged_install_sql
            || artifact.stage_manifest_sha256
                != canonical_cache->stage_manifest_sha256
            || artifact.control_sha256
                != canonical_cache->control_sha256
            || artifact.install_sql_sha256
                != canonical_cache->install_sql_sha256
            || artifact.staged_control_sha256
                != canonical_cache->staged_control_sha256
            || !std::ranges::contains(
                result.successful_cleanup_roots, cleanup_root)) {
            throw std::runtime_error(
                "integration provider-peer artifact is not bound to "
                "one successfully cleaned runtime override");
        }
    }
    for (const simdjson::dom::element entry :
         detail::require_array(
             root, "migration_probes", "integration report")) {
        const auto object =
            detail::require_object(entry, "integration migration probe");
        result.probes.push_back(migration::probe_observation{
            .owner = owner,
            .role = role,
            .stable_id = detail::require_string(
                object, "stable_id", "integration migration probe"),
            .test_case = detail::require_string(
                object, "test_case", "integration migration probe"),
            .section = detail::require_string(
                object, "section", "integration migration probe"),
            .permutation = detail::require_string(
                object, "permutation", "integration migration probe"),
            .passed = detail::require_bool(
                object, "passed", "integration migration probe")});
    }
    return result;
}

[[nodiscard]] inline native_report_evidence
load_benchmark_report(
    const std::filesystem::path& path,
    std::string_view retained_document = {}) {
    simdjson::dom::parser parser;
    simdjson::padded_string retained;
    const simdjson::dom::object root = detail::require_object(
        retained_document.empty()
            ? detail::load_json(parser, path)
            : detail::load_json_document(
                  parser, retained_document, retained,
                  "benchmark report"),
        "benchmark report");
    if (detail::require_string(root, "schema_version", "benchmark report")
        != benchmark_schema) {
        throw std::runtime_error(
            "benchmark evidence must use bioseq-cache-benchmark-v2");
    }
    const auto metadata =
        detail::require_object(root, "metadata", "benchmark report");
    if (detail::require_string(metadata, "status", "benchmark metadata")
            != "passed"
        || !detail::require_string(
                metadata, "error", "benchmark metadata").empty()) {
        throw std::runtime_error("benchmark report did not pass");
    }
    const auto cleanup =
        detail::require_object(root, "cleanup", "benchmark report");
    if (!detail::require_bool(cleanup, "attempted", "benchmark cleanup")
        || !detail::require_bool(cleanup, "succeeded", "benchmark cleanup")
        || !detail::require_bool(
            cleanup, "private_root_removed", "benchmark cleanup")) {
        throw std::runtime_error(
            "benchmark report has incomplete cleanup evidence");
    }
    const auto migration_evidence = detail::require_object(
        root, "migration_evidence", "benchmark report");
    const report_role role = parse_role(detail::require_string(
        migration_evidence, "migration_report_role",
        "benchmark migration evidence"));
    if (is_integration_role(role)) {
        throw std::runtime_error(
            "integration role appeared in a benchmark report");
    }
    const migration::evidence_harness owner =
        migration::parse_evidence_harness(detail::require_string(
            migration_evidence, "migration_evidence_harness",
            "benchmark migration evidence"));
    if (owner != role_owner(role)) {
        throw std::runtime_error(
            "benchmark report role and evidence owner disagree");
    }
    if (detail::require_string(
            migration_evidence, "coverage",
            "benchmark migration evidence")
        != "full-role") {
        throw std::runtime_error(
            "benchmark report did not execute full-role migration "
            "coverage");
    }

    const auto build_identity = detail::require_object(
        metadata, "build_identity", "benchmark metadata");
    native_report_evidence result{
        .role = role,
        .owner = owner,
        .migration_ledger_sha256 = detail::require_string(
            migration_evidence, "ledger_sha256",
            "benchmark migration evidence"),
        .evidence_contract_sha256 = detail::require_string(
            migration_evidence, "evidence_contract_sha256",
            "benchmark migration evidence"),
        .migration_phase = detail::require_string(
            migration_evidence, "migration_phase",
            "benchmark migration evidence"),
        .executable = detail::require_string(
            build_identity, "executable_path",
            "benchmark build identity"),
        .executable_sha256 = detail::require_string(
            build_identity, "executable_sha256",
            "benchmark build identity"),
        .scenario = detail::require_string(
            root, "scenario", "benchmark report"),
        .configured_samples = detail::require_u64(
            metadata, "configured_samples", "benchmark metadata"),
        .configured_warmup_samples = detail::require_u64(
            metadata, "configured_warmup_samples",
            "benchmark metadata")};
    detail::require_sha256(
        result.migration_ledger_sha256,
        "benchmark migration ledger SHA-256");
    detail::require_sha256(
        result.evidence_contract_sha256,
        "benchmark evidence contract SHA-256");
    detail::require_sha256(
        result.executable_sha256,
        "benchmark executable SHA-256");
    if (result.scenario != role_scenario(role)) {
        throw std::runtime_error(
            "benchmark report scenario is incompatible with its fixed "
            "migration role");
    }
    const std::uint64_t expected_samples =
        role == report_role::cache_bench ? 7U : 3U;
    if (result.configured_samples != expected_samples
        || result.configured_warmup_samples != 1U) {
        throw std::runtime_error(
            "benchmark report sample policy differs from its fixed "
            "migration role");
    }
    for (const simdjson::dom::element entry :
         detail::require_array(
             metadata, "artifacts", "benchmark metadata")) {
        result.artifacts.push_back(load_native_artifact_evidence(
            entry, "benchmark report artifact"));
    }
    validate_benchmark_artifact_roster(result.artifacts, role);
    validate_shared_artifact_path_hashes(
        result.artifacts, "benchmark report");
    for (const native_artifact_evidence& artifact :
         result.artifacts) {
        if (!path_is_strictly_within(
                artifact.staged_dso,
                artifact.preinstalled_stage_root)) {
            throw std::runtime_error(
                "benchmark staged DSO escapes its fixed stage");
        }
    }
    for (const simdjson::dom::element entry :
         detail::require_array(
             migration_evidence, "probes",
             "benchmark migration evidence")) {
        const auto object =
            detail::require_object(entry, "benchmark migration probe");
        const std::string probe_owner = detail::require_string(
            object, "owner", "benchmark migration probe");
        const std::string probe_role = detail::require_string(
            object, "role", "benchmark migration probe");
        if (probe_owner != migration::harness_name(owner)
            || probe_role != report_role_name(role)) {
            throw std::runtime_error(
                "benchmark migration probe owner or role disagrees");
        }
        result.probes.push_back(migration::probe_observation{
            .owner = owner,
            .role = role,
            .stable_id = detail::require_string(
                object, "stable_id", "benchmark migration probe"),
            .permutation = detail::require_string(
                object, "permutation", "benchmark migration probe"),
            .origin_kind = detail::require_string(
                object, "origin_kind", "benchmark migration probe"),
            .origin = detail::require_string(
                object, "origin", "benchmark migration probe"),
            .passed = detail::require_bool(
                object, "passed", "benchmark migration probe")});
    }
    return result;
}

inline void validate_current_native_artifacts(
    const native_report_evidence& native,
    const std::function<std::string(const std::filesystem::path&)>&
        sha256_file) {
    if (native.artifacts.empty()) {
        throw std::runtime_error(
            "current native report contains no artifact identity");
    }
    for (const native_artifact_evidence& artifact :
         native.artifacts) {
        if (sha256_file(artifact.stage_manifest)
                != artifact.stage_manifest_sha256
            || sha256_file(artifact.control_file)
                != artifact.control_sha256
            || sha256_file(artifact.install_sql)
                != artifact.install_sql_sha256
            || sha256_file(artifact.dso)
                != artifact.dso_sha256
            || sha256_file(artifact.staged_control_file)
                != artifact.staged_control_sha256
            || sha256_file(artifact.staged_install_sql)
                != artifact.install_sql_sha256
            || (path_is_strictly_within(
                    artifact.staged_dso,
                    artifact.preinstalled_stage_root)
                && sha256_file(artifact.staged_dso)
                    != artifact.dso_sha256)) {
            throw std::runtime_error(
                "current native report artifact identity is stale for "
                "its fixed role");
        }
    }
}

[[nodiscard]] inline std::vector<std::filesystem::path>
current_native_absent_paths(
    const native_report_evidence& native) {
    std::vector<std::filesystem::path> result =
        native.successful_cleanup_roots;
    for (const native_artifact_evidence& artifact :
         native.artifacts) {
        if (!path_is_strictly_within(
                artifact.staged_dso,
                artifact.preinstalled_stage_root)) {
            result.push_back(artifact.staged_dso);
        }
    }
    std::ranges::sort(result);
    result.erase(
        std::unique(result.begin(), result.end()), result.end());
    return result;
}

inline void validate_receipt_and_native_report(
    const evidence_receipt& receipt,
    const native_report_evidence& native,
    const gate_bundle& bundle,
    std::string_view current_ledger_sha256,
    std::string_view current_evidence_contract_sha256,
    std::string_view actual_native_report_sha256,
    std::string_view actual_executable_sha256) {
    if (receipt.role != native.role
        || receipt.native_report_sha256 != actual_native_report_sha256) {
        throw std::runtime_error(
            "MCP receipt does not hash-bind its native report");
    }
    if (!native.executable.is_absolute()
        || receipt.executable.lexically_normal()
            != native.executable.lexically_normal()
        || native.executable.filename().string()
            != role_executable_name(receipt.role)
        || receipt.executable_sha256 != actual_executable_sha256
        || native.executable_sha256 != actual_executable_sha256
        || receipt.scenario != native.scenario
        || native.scenario != role_scenario(receipt.role)) {
        throw std::runtime_error(
            "native report executable identity is stale or incompatible "
            "with its fixed report role");
    }
    const std::string_view phase =
        detail::expected_receipt_phase(bundle.operation);
    if (receipt.source.revision != bundle.current_source.revision
        || receipt.source.scope_sha256
            != bundle.current_source.scope_sha256
        || receipt.migration_ledger_sha256 != current_ledger_sha256
        || native.migration_ledger_sha256 != current_ledger_sha256
        || receipt.evidence_contract_sha256
            != current_evidence_contract_sha256
        || native.evidence_contract_sha256
            != current_evidence_contract_sha256
        || receipt.migration_phase != phase
        || native.migration_phase != phase) {
        throw std::runtime_error(
            "evidence receipt/report provenance differs from the "
            "current retirement gate inputs");
    }
}

[[nodiscard]] inline const proof_report& proof_report_for(
    const retirement_proof& proof, report_role role) {
    const auto found = std::ranges::find_if(
        proof.reports, [role](const proof_report& report) {
            return report.role == role;
        });
    if (found == proof.reports.end()) {
        throw std::runtime_error(
            "retirement proof omits report role "
            + std::string(report_role_name(role)));
    }
    return *found;
}

inline void validate_transition_unit_receipt(
    const unit_receipt& receipt,
    const retirement_proof& proof,
    std::string_view current_evidence_contract_sha256) {
    if (receipt.receipt_sha256 != proof.unit_receipt_sha256
        || receipt.source.revision != proof.proof_source_revision
        || receipt.source.scope_sha256
            != proof.proof_source_scope_sha256
        || receipt.migration_ledger_sha256
            != proof.migration_ledger_sha256
        || receipt.evidence_contract_sha256
            != current_evidence_contract_sha256
        || receipt.evidence_contract_sha256
            != proof.evidence_contract_sha256
        || receipt.migration_phase != "passing") {
        throw std::runtime_error(
            "cache-unit receipt is not the proof-ready receipt sealed "
            "by the retirement proof");
    }
}

inline void validate_transition_receipt_and_native_report(
    const evidence_receipt& receipt,
    const native_report_evidence& native,
    const retirement_proof& proof,
    std::string_view current_evidence_contract_sha256,
    std::string_view actual_native_report_sha256) {
    const proof_report& sealed =
        proof_report_for(proof, receipt.role);
    if (receipt.receipt_sha256 != sealed.receipt_sha256
        || receipt.native_report_sha256
            != sealed.native_report_sha256
        || receipt.native_report_sha256 != actual_native_report_sha256
        || receipt.role != native.role
        || !native.executable.is_absolute()
        || receipt.executable.lexically_normal()
            != native.executable.lexically_normal()
        || native.executable.filename().string()
            != role_executable_name(receipt.role)
        || receipt.executable_sha256 != native.executable_sha256
        || receipt.scenario != native.scenario
        || native.scenario != role_scenario(receipt.role)
        || receipt.source.revision != proof.proof_source_revision
        || receipt.source.scope_sha256
            != proof.proof_source_scope_sha256
        || receipt.migration_ledger_sha256
            != proof.migration_ledger_sha256
        || native.migration_ledger_sha256
            != proof.migration_ledger_sha256
        || receipt.evidence_contract_sha256
            != current_evidence_contract_sha256
        || native.evidence_contract_sha256
            != current_evidence_contract_sha256
        || receipt.evidence_contract_sha256
            != proof.evidence_contract_sha256
        || receipt.migration_phase != "passing"
        || native.migration_phase != "passing") {
        throw std::runtime_error(
            "native report is not the proof-ready evidence sealed by "
            "the retirement proof");
    }
}

[[nodiscard]] inline retirement_proof load_retirement_proof(
    const std::filesystem::path& path,
    std::string_view retained_document = {}) {
    simdjson::dom::parser parser;
    simdjson::padded_string retained;
    const simdjson::dom::object root = detail::require_object(
        retained_document.empty()
            ? detail::load_json(parser, path)
            : detail::load_json_document(
                  parser, retained_document, retained,
                  "retirement proof receipt"),
        "retirement proof receipt");
    constexpr std::array<std::string_view, 10> fields{
        "schema_version",
        "proof_source_revision",
        "proof_source_scope_sha256",
        "migration_ledger_sha256",
        "evidence_contract_sha256",
        "migration_phase",
        "legacy_inventory_sha256",
        "legacy_asset_count",
        "unit_receipt_sha256",
        "reports",
    };
    detail::require_exact_fields(root, fields, "retirement proof receipt");
    if (detail::require_string(
            root, "schema_version", "retirement proof receipt")
        != proof_schema) {
        throw std::runtime_error(
            "retirement proof receipt has an unsupported schema");
    }
    retirement_proof result{
        .proof_source_revision = detail::require_string(
            root, "proof_source_revision", "retirement proof receipt"),
        .proof_source_scope_sha256 = detail::require_string(
            root, "proof_source_scope_sha256",
            "retirement proof receipt"),
        .migration_ledger_sha256 = detail::require_string(
            root, "migration_ledger_sha256", "retirement proof receipt"),
        .evidence_contract_sha256 = detail::require_string(
            root, "evidence_contract_sha256",
            "retirement proof receipt"),
        .migration_phase = detail::require_string(
            root, "migration_phase", "retirement proof receipt"),
        .legacy_inventory_sha256 = detail::require_string(
            root, "legacy_inventory_sha256",
            "retirement proof receipt"),
        .legacy_asset_count = detail::require_u64(
            root, "legacy_asset_count", "retirement proof receipt"),
        .unit_receipt_sha256 = detail::require_string(
            root, "unit_receipt_sha256", "retirement proof receipt")};
    detail::require_revision(
        result.proof_source_revision, "proof_source_revision");
    detail::require_sha256(
        result.proof_source_scope_sha256,
        "proof_source_scope_sha256");
    detail::require_sha256(
        result.migration_ledger_sha256,
        "proof migration_ledger_sha256");
    detail::require_sha256(
        result.evidence_contract_sha256,
        "proof evidence_contract_sha256");
    detail::require_sha256(
        result.legacy_inventory_sha256,
        "proof legacy_inventory_sha256");
    detail::require_sha256(
        result.unit_receipt_sha256,
        "proof unit_receipt_sha256");
    if (result.migration_phase != "passing") {
        throw std::runtime_error(
            "retirement proof was not created in the passing phase");
    }
    for (const simdjson::dom::element entry :
         detail::require_array(root, "reports", "retirement proof receipt")) {
        const auto object =
            detail::require_object(entry, "retirement proof report");
        constexpr std::array<std::string_view, 3> report_fields{
            "report_role", "receipt_sha256", "native_report_sha256"};
        detail::require_exact_fields(
            object, report_fields, "retirement proof report");
        proof_report report{
            .role = parse_role(detail::require_string(
                object, "report_role", "retirement proof report")),
            .receipt_sha256 = detail::require_string(
                object, "receipt_sha256", "retirement proof report"),
            .native_report_sha256 = detail::require_string(
                object, "native_report_sha256",
                "retirement proof report")};
        detail::require_sha256(
            report.receipt_sha256, "proof report receipt_sha256");
        detail::require_sha256(
            report.native_report_sha256,
            "proof report native_report_sha256");
        result.reports.push_back(std::move(report));
    }
    return result;
}

[[nodiscard]] inline retirement_transition load_retirement_transition(
    const std::filesystem::path& path,
    std::string_view retained_document = {}) {
    simdjson::dom::parser parser;
    simdjson::padded_string retained;
    const simdjson::dom::object root = detail::require_object(
        retained_document.empty()
            ? detail::load_json(parser, path)
            : detail::load_json_document(
                  parser, retained_document, retained,
                  "retirement transition receipt"),
        "retirement transition receipt");
    constexpr std::array<std::string_view, 11> fields{
        "schema_version",
        "proof_receipt_sha256",
        "proof_source_revision",
        "proof_source_scope_sha256",
        "retired_source_revision",
        "retired_source_scope_sha256",
        "retirement_transition_sha256",
        "evidence_contract_sha256",
        "legacy_inventory_sha256",
        "unit_receipt_sha256",
        "reports",
    };
    detail::require_exact_fields(
        root, fields, "retirement transition receipt");
    if (detail::require_string(
            root, "schema_version", "retirement transition receipt")
        != transition_schema) {
        throw std::runtime_error(
            "retirement transition receipt has an unsupported schema");
    }
    retirement_transition result{
        .proof_receipt_sha256 = detail::require_string(
            root, "proof_receipt_sha256",
            "retirement transition receipt"),
        .proof_source_revision = detail::require_string(
            root, "proof_source_revision",
            "retirement transition receipt"),
        .proof_source_scope_sha256 = detail::require_string(
            root, "proof_source_scope_sha256",
            "retirement transition receipt"),
        .retired_source_revision = detail::require_string(
            root, "retired_source_revision",
            "retirement transition receipt"),
        .retired_source_scope_sha256 = detail::require_string(
            root, "retired_source_scope_sha256",
            "retirement transition receipt"),
        .retirement_transition_sha256 = detail::require_string(
            root, "retirement_transition_sha256",
            "retirement transition receipt"),
        .evidence_contract_sha256 = detail::require_string(
            root, "evidence_contract_sha256",
            "retirement transition receipt"),
        .legacy_inventory_sha256 = detail::require_string(
            root, "legacy_inventory_sha256",
            "retirement transition receipt"),
        .unit_receipt_sha256 = detail::require_string(
            root, "unit_receipt_sha256",
            "retirement transition receipt")};
    detail::require_sha256(
        result.proof_receipt_sha256,
        "transition proof_receipt_sha256");
    detail::require_revision(
        result.proof_source_revision,
        "transition proof_source_revision");
    detail::require_sha256(
        result.proof_source_scope_sha256,
        "transition proof_source_scope_sha256");
    detail::require_revision(
        result.retired_source_revision,
        "transition retired_source_revision");
    detail::require_sha256(
        result.retired_source_scope_sha256,
        "transition retired_source_scope_sha256");
    detail::require_sha256(
        result.retirement_transition_sha256,
        "transition retirement_transition_sha256");
    detail::require_sha256(
        result.evidence_contract_sha256,
        "transition evidence_contract_sha256");
    detail::require_sha256(
        result.legacy_inventory_sha256,
        "transition legacy_inventory_sha256");
    detail::require_sha256(
        result.unit_receipt_sha256,
        "transition unit_receipt_sha256");
    for (const simdjson::dom::element entry :
         detail::require_array(
             root, "reports", "retirement transition receipt")) {
        const auto object = detail::require_object(
            entry, "retirement transition report");
        constexpr std::array<std::string_view, 3> report_fields{
            "report_role", "receipt_sha256", "native_report_sha256"};
        detail::require_exact_fields(
            object, report_fields, "retirement transition report");
        proof_report report{
            .role = parse_role(detail::require_string(
                object, "report_role",
                "retirement transition report")),
            .receipt_sha256 = detail::require_string(
                object, "receipt_sha256",
                "retirement transition report"),
            .native_report_sha256 = detail::require_string(
                object, "native_report_sha256",
                "retirement transition report")};
        detail::require_sha256(
            report.receipt_sha256,
            "transition report receipt_sha256");
        detail::require_sha256(
            report.native_report_sha256,
            "transition report native_report_sha256");
        result.reports.push_back(std::move(report));
    }
    return result;
}

inline void validate_exact_report_roster(
    std::span<const evidence_receipt> receipts) {
    if (receipts.size() != required_report_roles.size()) {
        throw std::runtime_error(
            "retirement union requires exactly five report receipts");
    }
    std::set<report_role> actual;
    for (const evidence_receipt& receipt : receipts) {
        if (!actual.insert(receipt.role).second) {
            throw std::runtime_error(
                "retirement union duplicates report role "
                + std::string(report_role_name(receipt.role)));
        }
    }
    const std::set<report_role> expected{
        required_report_roles.begin(), required_report_roles.end()};
    if (actual != expected) {
        throw std::runtime_error(
            "retirement union does not contain the exact report roster");
    }
}

inline void validate_proof_against_current_contract(
    const retirement_proof& proof,
    std::span<const evidence_receipt> current_receipts,
    std::string_view current_evidence_contract_sha256,
    std::string_view current_inventory_sha256,
    std::uint64_t current_asset_count) {
    if (proof.evidence_contract_sha256
            != current_evidence_contract_sha256
        || proof.legacy_inventory_sha256 != current_inventory_sha256
        || proof.legacy_asset_count != current_asset_count) {
        throw std::runtime_error(
            "retirement proof does not bind the current semantic ledger "
            "and exact legacy inventory");
    }
    if (proof.reports.size() != required_report_roles.size()) {
        throw std::runtime_error(
            "retirement proof does not contain five report seals");
    }
    std::set<report_role> roles;
    for (const proof_report& report : proof.reports) {
        if (!roles.insert(report.role).second) {
            throw std::runtime_error(
                "retirement proof duplicates a report role");
        }
    }
    if (roles != std::set<report_role>{
                     required_report_roles.begin(),
                     required_report_roles.end()}) {
        throw std::runtime_error(
            "retirement proof report roster is incomplete");
    }
    validate_exact_report_roster(current_receipts);
}

inline void validate_transition_against_chain(
    const retirement_transition& transition,
    std::string_view actual_proof_receipt_sha256,
    const retirement_proof& proof,
    const gate_bundle& bundle,
    std::string_view current_evidence_contract_sha256,
    std::string_view current_inventory_sha256) {
    if (transition.proof_receipt_sha256
            != actual_proof_receipt_sha256
        || transition.proof_source_revision
            != proof.proof_source_revision
        || transition.proof_source_scope_sha256
            != proof.proof_source_scope_sha256
        || transition.retired_source_revision
            != bundle.current_source.revision
        || transition.retired_source_scope_sha256
            != bundle.current_source.scope_sha256
        || transition.retirement_transition_sha256
            != bundle.retirement_transition_sha256
        || transition.evidence_contract_sha256
            != current_evidence_contract_sha256
        || transition.evidence_contract_sha256
            != proof.evidence_contract_sha256
        || transition.legacy_inventory_sha256
            != current_inventory_sha256
        || transition.legacy_inventory_sha256
            != proof.legacy_inventory_sha256
        || transition.unit_receipt_sha256
            != proof.unit_receipt_sha256) {
        throw std::runtime_error(
            "retirement transition receipt does not bind the exact "
            "proof/deletion/source contract");
    }
    if (transition.reports.size() != required_report_roles.size()
        || proof.reports.size() != required_report_roles.size()) {
        throw std::runtime_error(
            "retirement transition receipt report roster is incomplete");
    }
    std::map<report_role, std::pair<std::string, std::string>> expected;
    std::map<report_role, std::pair<std::string, std::string>> actual;
    for (const proof_report& report : proof.reports) {
        if (!expected.emplace(
                report.role,
                std::pair{
                    report.receipt_sha256,
                    report.native_report_sha256}).second) {
            throw std::runtime_error(
                "retirement proof duplicates a transition report role");
        }
    }
    for (const proof_report& report : transition.reports) {
        if (!actual.emplace(
                report.role,
                std::pair{
                    report.receipt_sha256,
                    report.native_report_sha256}).second) {
            throw std::runtime_error(
                "retirement transition duplicates a report role");
        }
    }
    if (actual != expected) {
        throw std::runtime_error(
            "retirement transition receipt does not bind every "
            "proof-sealed report");
    }
}

[[nodiscard]] inline std::string render_retirement_transition(
    std::string_view proof_receipt_sha256,
    const retirement_proof& proof,
    const gate_bundle& bundle,
    std::string_view inventory_sha256) {
    detail::require_sha256(
        proof_receipt_sha256, "transition proof receipt SHA-256");
    detail::require_sha256(
        bundle.retirement_transition_sha256,
        "transition Git contract SHA-256");
    detail::require_sha256(
        inventory_sha256, "transition inventory SHA-256");
    if (proof.reports.size() != required_report_roles.size()) {
        throw std::runtime_error(
            "transition rendering requires five proof report seals");
    }
    std::map<report_role, const proof_report*> ordered;
    for (const proof_report& report : proof.reports) {
        if (!ordered.emplace(report.role, &report).second) {
            throw std::runtime_error(
                "transition rendering found duplicate proof roles");
        }
    }
    std::ostringstream output;
    output
        << "{\n"
        << "  \"schema_version\": \"" << transition_schema << "\",\n"
        << "  \"proof_receipt_sha256\": \""
        << proof_receipt_sha256 << "\",\n"
        << "  \"proof_source_revision\": \""
        << proof.proof_source_revision << "\",\n"
        << "  \"proof_source_scope_sha256\": \""
        << proof.proof_source_scope_sha256 << "\",\n"
        << "  \"retired_source_revision\": \""
        << bundle.current_source.revision << "\",\n"
        << "  \"retired_source_scope_sha256\": \""
        << bundle.current_source.scope_sha256 << "\",\n"
        << "  \"retirement_transition_sha256\": \""
        << bundle.retirement_transition_sha256 << "\",\n"
        << "  \"evidence_contract_sha256\": \""
        << proof.evidence_contract_sha256 << "\",\n"
        << "  \"legacy_inventory_sha256\": \""
        << inventory_sha256 << "\",\n"
        << "  \"unit_receipt_sha256\": \""
        << proof.unit_receipt_sha256 << "\",\n"
        << "  \"reports\": [\n";
    for (std::size_t index = 0;
         index < required_report_roles.size(); ++index) {
        const report_role role = required_report_roles[index];
        const proof_report& report = *ordered.at(role);
        output
            << "    {\"report_role\": \"" << report_role_name(role)
            << "\", \"receipt_sha256\": \""
            << report.receipt_sha256
            << "\", \"native_report_sha256\": \""
            << report.native_report_sha256 << "\"}"
            << (index + 1U == required_report_roles.size()
                    ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    return output.str();
}

[[nodiscard]] inline std::string render_retirement_proof(
    const source_provenance& source,
    std::string_view migration_ledger_sha256,
    std::string_view evidence_contract_sha256,
    std::string_view inventory_sha256,
    std::uint64_t asset_count,
    const unit_receipt& unit,
    std::span<const evidence_receipt> receipts) {
    detail::validate_source_provenance(source, "proof source");
    detail::require_sha256(
        migration_ledger_sha256, "proof migration ledger SHA-256");
    detail::require_sha256(
        evidence_contract_sha256, "proof evidence contract SHA-256");
    detail::require_sha256(
        inventory_sha256, "proof inventory SHA-256");
    validate_exact_report_roster(receipts);
    detail::require_sha256(
        unit.receipt_sha256, "proof cache-unit receipt SHA-256");

    std::map<report_role, const evidence_receipt*> ordered;
    for (const evidence_receipt& receipt : receipts) {
        ordered.emplace(receipt.role, &receipt);
    }
    std::ostringstream output;
    output
        << "{\n"
        << "  \"schema_version\": \"" << proof_schema << "\",\n"
        << "  \"proof_source_revision\": \""
        << detail::json_escape(source.revision) << "\",\n"
        << "  \"proof_source_scope_sha256\": \""
        << detail::json_escape(source.scope_sha256) << "\",\n"
        << "  \"migration_ledger_sha256\": \""
        << migration_ledger_sha256 << "\",\n"
        << "  \"evidence_contract_sha256\": \""
        << evidence_contract_sha256 << "\",\n"
        << "  \"migration_phase\": \"passing\",\n"
        << "  \"legacy_inventory_sha256\": \""
        << inventory_sha256 << "\",\n"
        << "  \"legacy_asset_count\": " << asset_count << ",\n"
        << "  \"unit_receipt_sha256\": \""
        << unit.receipt_sha256 << "\",\n"
        << "  \"reports\": [\n";
    for (std::size_t index = 0;
         index < required_report_roles.size(); ++index) {
        const report_role role = required_report_roles[index];
        const evidence_receipt& receipt = *ordered.at(role);
        output
            << "    {\"report_role\": \"" << report_role_name(role)
            << "\", \"receipt_sha256\": \""
            << receipt.receipt_sha256
            << "\", \"native_report_sha256\": \""
            << receipt.native_report_sha256 << "\"}"
            << (index + 1U == required_report_roles.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    return output.str();
}

}  // namespace bioseq::cache_test::retirement
