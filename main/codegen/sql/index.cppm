module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <print>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <llvm/Support/JSON.h>
#include <llvm/Support/Error.h>

export module bioseq.codegen.sql;

export import :contract_tests;
export import :baseline_renderer;
export import :migration;
export import :model;
export import :parser;
export import :policy;
import bioseq.codegen.sql.annotation_model;
import bioseq.codegen.sql.collector;
import bioseq.codegen.sql.graph;
import bioseq.codegen.sql.guc_contract_tests;
import bioseq.codegen.sql.physical_ledger;
import bioseq.codegen.sql.renderer;
import bioseq.codegen.safety;
import bioseq.codegen.clang_structural;
import bioseq.codegen.clang_structural.contract_tests;
import bioseq.codegen.support;

export namespace bioseq::codegen::sql {

namespace detail {

[[nodiscard]] inline bool support_owned_fmgr_output(
        std::string_view owner_key) noexcept {
    return owner_key == "vector_comparison_registry"
        || owner_key == "vector_aggregate_registry";
}

[[nodiscard]] inline std::string_view trim_cli_line(
    std::string_view value) noexcept {
    while (!value.empty()
           && (value.front() == ' ' || value.front() == '\t'
               || value.front() == '\r')) {
        value.remove_prefix(1);
    }
    while (!value.empty()
           && (value.back() == ' ' || value.back() == '\t'
               || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] inline std::vector<std::filesystem::path> read_source_list(
    const std::filesystem::path& list_path) {
    std::vector<std::filesystem::path> result;
    const std::string content = read_file(list_path);
    std::size_t begin = 0;
    while (begin <= content.size()) {
        const std::size_t newline = content.find('\n', begin);
        const std::size_t end =
            newline == std::string::npos ? content.size() : newline;
        const std::string_view line =
            trim_cli_line(std::string_view{content}.substr(
                begin, end - begin));
        if (!line.empty() && !line.starts_with('#')) {
            std::filesystem::path source{line};
            if (source.is_relative()) {
                source = list_path.parent_path() / source;
            }
            result.push_back(source.lexically_normal());
        }
        if (newline == std::string::npos) break;
        begin = newline + 1;
    }
    return result;
}

inline void normalize_sources(std::vector<std::filesystem::path>& sources) {
    for (std::filesystem::path& path : sources) {
        std::error_code error;
        const std::filesystem::path absolute =
            path.is_absolute() ? path : std::filesystem::absolute(path);
        const std::filesystem::path canonical =
            std::filesystem::weakly_canonical(absolute, error);
        path = error ? absolute.lexically_normal() : canonical;
    }
    std::ranges::sort(
        sources,
        {},
        [](const std::filesystem::path& path) {
            return path.generic_string();
        });
    if (std::ranges::adjacent_find(sources)
        != sources.end()) {
        throw std::runtime_error(
            "SQL generator source list contains duplicate source "
            "identities");
    }
}

[[nodiscard]] inline configured_entity_handle_output
parse_entity_handle_output(std::string_view value) {
    const std::size_t separator = value.find('=');
    if (separator == std::string_view::npos || separator == 0
        || separator + 1 == value.size()) {
        throw std::runtime_error(
            "--entity-handles-output requires <owner-key>=<path>");
    }
    configured_entity_handle_output result{
        .owner_key = std::string{value.substr(0, separator)},
        .path = std::filesystem::path{value.substr(separator + 1)},
    };
    for (const char character : result.owner_key) {
        if (!((character >= 'a' && character <= 'z')
              || (character >= '0' && character <= '9')
              || character == '_')) {
            throw std::runtime_error(
                "invalid configured entity-handle owner key '"
                + result.owner_key + "'");
        }
    }
    if (result.owner_key.empty()
        || (result.owner_key.front() >= '0'
            && result.owner_key.front() <= '9')) {
        throw std::runtime_error(
            "invalid configured entity-handle owner key '"
            + result.owner_key + "'");
    }
    return result;
}

[[nodiscard]] inline configured_row_codec_output
parse_row_codec_output(std::string_view value) {
    const configured_entity_handle_output parsed =
        parse_entity_handle_output(value);
    return {
        .owner_key = parsed.owner_key,
        .path = parsed.path,
    };
}

[[nodiscard]] inline configured_fmgr_entrypoint_output
parse_fmgr_entrypoint_output(std::string_view value) {
    const configured_entity_handle_output parsed =
        parse_entity_handle_output(value);
    return {
        .owner_key = parsed.owner_key,
        .path = parsed.path,
    };
}

[[nodiscard]] inline std::pair<std::string, std::string>
parse_fmgr_owner(std::string_view value) {
    const configured_entity_handle_output parsed =
        parse_entity_handle_output(value);
    const std::string source_module = parsed.path.generic_string();
    if (source_module.empty()
        || source_module.front() == '.'
        || source_module.back() == '.'
        || source_module.contains("..")
        || !std::ranges::all_of(
            source_module,
            [](char item) {
                return (item >= 'a' && item <= 'z')
                    || (item >= 'A' && item <= 'Z')
                    || (item >= '0' && item <= '9')
                    || item == '_' || item == '.' || item == ':';
            })) {
        throw std::runtime_error(
            "--fmgr-owner-module has invalid C++ module name '"
            + source_module + "'");
    }
    return {parsed.owner_key, source_module};
}

[[nodiscard]] inline std::filesystem::path
normalized_artifact_path(const std::filesystem::path& path) {
    return (path.is_absolute()
                ? path
                : std::filesystem::absolute(path))
        .lexically_normal();
}

[[nodiscard]] inline std::filesystem::path
artifact_path_identity(const std::filesystem::path& path) {
    const std::filesystem::path absolute =
        normalized_artifact_path(path);
    std::error_code error;
    if (std::filesystem::exists(absolute, error)
        && !error) {
        const std::filesystem::path canonical =
            std::filesystem::weakly_canonical(
                absolute, error);
        if (!error) return canonical;
    }
    error.clear();
    const std::filesystem::path canonical_parent =
        std::filesystem::weakly_canonical(
            absolute.parent_path(), error);
    return ((error ? absolute.parent_path() : canonical_parent)
            / absolute.filename())
        .lexically_normal();
}

inline void validate_artifact_path_aliases(
    const command_line& command,
    const std::vector<std::pair<
        std::filesystem::path,
        std::string>>& derived_outputs = {}) {
    std::map<
        std::filesystem::path,
        std::string,
        std::less<>>
        identities;
    const auto add =
        [&](const std::filesystem::path& path,
            std::string_view role) {
            if (path.empty()) return;
            const std::filesystem::path identity =
                artifact_path_identity(path);
            const auto [found, inserted] =
                identities.emplace(identity, role);
            if (!inserted) {
                throw std::runtime_error(
                    "SQL generator artifact path aliases both '"
                    + found->second + "' and '"
                    + std::string{role} + "': "
                    + identity.string());
            }
        };
    add(command.input, "legacy input");
    add(command.source_list, "source list");
    add(command.compile_database, "owned compile database");
    add(command.raw_compile_database, "raw compile database");
    add(
        command.compile_database_output,
        "owned compile database output");
    add(command.pcm_manifest, "PCM manifest");
    add(command.depfile, "PREPARE depfile");
    add(command.clang_binary, "configured Clang");
    add(command.clang_scan_deps, "configured clang-scan-deps");
    add(command.llvm_config, "configured llvm-config");
    add(
        command.compiler_module_inventory,
        "compiler module inventory");
    add(command.migration_owner, "migration owner");
    add(command.output, "SQL output");
    add(command.manifest, "SQL/stage manifest");
    add(command.golden, "checked SQL golden");
    add(command.stamp, "completion stamp");
    add(command.physical_ledger, "physical ledger input");
    add(
        command.physical_ledger_output,
        "physical ledger output");
    add(command.guc_parity_ledger, "GUC parity ledger");
    add(command.guc_output_root, "GUC output root");
    add(command.core_hash_output, "CORE seal output");
    add(command.core_manifest, "CORE manifest");
    add(command.support_manifest, "SUPPORT manifest");
    add(command.support_seal, "SUPPORT seal");
    if (!command.production_support_manifest.empty()
        || !command.production_support_seal.empty()
        || !command.test_indexes_support_manifest.empty()
        || !command.test_indexes_support_seal.empty()) {
        const bool production =
            command.compiler_profile == "production";
        const std::filesystem::path& active_manifest =
            production
            ? command.production_support_manifest
            : command.test_indexes_support_manifest;
        const std::filesystem::path& active_seal =
            production
            ? command.production_support_seal
            : command.test_indexes_support_seal;
        if (command.support_manifest.empty()
            || command.support_seal.empty()
            || active_manifest.empty()
            || active_seal.empty()
            || artifact_path_identity(command.support_manifest)
                != artifact_path_identity(active_manifest)
            || artifact_path_identity(command.support_seal)
                != artifact_path_identity(active_seal)) {
            throw std::runtime_error(
                "FINAL active SUPPORT manifest/seal roles do not match "
                "the explicit compiler-profile handoff");
        }
        add(
            production
                ? command.test_indexes_support_manifest
                : command.production_support_manifest,
            "inactive profile SUPPORT manifest");
        add(
            production
                ? command.test_indexes_support_seal
                : command.production_support_seal,
            "inactive profile SUPPORT seal");
    }
    add(command.expected_core_hash, "expected CORE seal");
    for (const std::filesystem::path& source :
         command.sources) {
        add(source, "annotated source");
    }
    for (const configured_entity_handle_output& output :
         command.entity_handle_outputs) {
        add(
            output.path,
            "entity handle " + output.owner_key);
    }
    for (const configured_row_codec_output& output :
         command.row_codec_outputs) {
        add(
            output.path,
            "row codec " + output.owner_key);
    }
    for (const configured_fmgr_entrypoint_output& output :
         command.fmgr_entrypoint_outputs) {
        add(
            output.path,
            "fmgr entrypoint " + output.owner_key);
    }
    std::set<std::filesystem::path> derived_identities;
    for (const auto& [path, role] : derived_outputs) {
        add(path, role);
        derived_identities.insert(artifact_path_identity(path));
    }
    if (!command.guc_output_root.empty()
        && !derived_outputs.empty()) {
        const std::filesystem::path guc_root =
            artifact_path_identity(command.guc_output_root);
        std::vector<std::pair<
            std::filesystem::path,
            std::string>>
            writable;
        const auto writable_add =
            [&](const std::filesystem::path& path,
                std::string_view role) {
                if (!path.empty()) {
                    writable.emplace_back(
                        artifact_path_identity(path),
                        role);
                }
            };
        writable_add(command.output, "SQL output");
        writable_add(command.manifest, "SQL/stage manifest");
        writable_add(command.stamp, "completion stamp");
        writable_add(
            command.physical_ledger_output,
            "physical ledger output");
        writable_add(
            command.core_hash_output, "CORE seal output");
        if (command.stage == generation_stage::core) {
            writable_add(command.core_manifest, "CORE manifest");
            for (const auto& output :
                 command.entity_handle_outputs) {
                writable_add(
                    output.path,
                    "entity handle " + output.owner_key);
            }
        }
        if (command.stage == generation_stage::support) {
            for (const auto& output :
                 command.row_codec_outputs) {
                writable_add(
                    output.path,
                    "row codec " + output.owner_key);
            }
            for (const auto& output :
                 command.fmgr_entrypoint_outputs) {
                if (support_owned_fmgr_output(output.owner_key)) {
                    writable_add(
                        output.path,
                        "support registry " + output.owner_key);
                }
            }
        }
        if (command.stage == generation_stage::final) {
            for (const auto& output :
                 command.fmgr_entrypoint_outputs) {
                if (!support_owned_fmgr_output(output.owner_key)) {
                    writable_add(
                        output.path,
                        "fmgr entrypoint " + output.owner_key);
                }
            }
        }
        if (command.update_golden) {
            writable_add(command.golden, "checked SQL golden");
        }
        for (const auto& [path, role] : writable) {
            const std::filesystem::path relative =
                path.lexically_relative(guc_root);
            const bool inside =
                !relative.empty() && !relative.is_absolute()
                && *relative.begin() != "..";
            if (inside
                && !derived_identities.contains(path)) {
                throw std::runtime_error(
                    "writable artifact '" + role
                    + "' is contained by the generated GUC output root: "
                    + path.string());
            }
        }
    }
}

inline void run_artifact_path_alias_contract_self_test() {
    const std::filesystem::path temporary_parent =
        std::filesystem::temp_directory_path();
    std::filesystem::path temporary_root;
    for (unsigned int attempt = 0; attempt < 1024; ++attempt) {
        const std::filesystem::path candidate =
            temporary_parent
            / ("bioseq-sql-artifact-alias-contract-"
               + std::to_string(attempt));
        std::error_code error;
        if (std::filesystem::create_directory(
                candidate, error)) {
            temporary_root = candidate;
            break;
        }
        if (error) {
            throw std::runtime_error(
                "cannot create SQL artifact-alias contract directory: "
                + error.message());
        }
    }
    if (temporary_root.empty()) {
        throw std::runtime_error(
            "cannot allocate SQL artifact-alias contract directory");
    }
    const auto cleanup = [&] {
        std::error_code error;
        std::filesystem::remove_all(temporary_root, error);
    };
    try {
        const std::filesystem::path target =
            temporary_root / "generated.sql";
        const std::filesystem::path alias =
            temporary_root / "generated-link.sql";
        write_if_different(target, "-- alias contract target\n");
        std::error_code error;
        std::filesystem::create_symlink(
            target.filename(), alias, error);
        if (error) {
            throw std::runtime_error(
                "cannot create SQL artifact-alias contract symlink: "
                + error.message());
        }
        command_line command;
        command.output = target;
        command.manifest = alias;
        bool rejected = false;
        try {
            validate_artifact_path_aliases(command);
        } catch (const std::runtime_error& failure) {
            rejected = std::string_view{failure.what()}.contains(
                "SQL generator artifact path aliases both");
        }
        if (!rejected) {
            throw std::runtime_error(
                "existing leaf-symlink SQL output alias was accepted");
        }
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
}

[[nodiscard]] inline std::map<
    std::string,
    std::filesystem::path,
    std::less<>>
validated_entity_handle_outputs(
    const command_line& command,
    const std::vector<rendered_entity_handle_module>& modules) {
    std::map<
        std::string,
        std::filesystem::path,
        std::less<>>
        configured;
    std::set<std::filesystem::path> configured_paths;
    for (const configured_entity_handle_output& output :
         command.entity_handle_outputs) {
        const std::filesystem::path path =
            normalized_artifact_path(output.path);
        if (!configured.emplace(output.owner_key, path).second) {
            throw std::runtime_error(
                "duplicate configured entity-handle output for owner '"
                + output.owner_key + "'");
        }
        if (!configured_paths.insert(path).second) {
            throw std::runtime_error(
                "more than one entity-handle owner targets configured path '"
                + output.path.string() + "'");
        }
    }
    std::map<
        std::string,
        const rendered_entity_handle_module*,
        std::less<>>
        generated;
    for (const rendered_entity_handle_module& module : modules) {
        if (!generated.emplace(module.owner_key, &module).second) {
            throw std::runtime_error(
                "duplicate generated entity-handle owner '"
                + module.owner_key + "'");
        }
    }
    for (const auto& [owner_key, module] : generated) {
        static_cast<void>(module);
        if (!configured.contains(owner_key)) {
            throw std::runtime_error(
                "generated entity-handle owner '" + owner_key
                + "' has no configure-known output");
        }
    }
    for (const auto& [owner_key, path] : configured) {
        static_cast<void>(path);
        if (!generated.contains(owner_key)) {
            throw std::runtime_error(
                "configured entity-handle output owner '" + owner_key
                + "' was not produced by the annotated source graph");
        }
    }
    return configured;
}

inline void publish_entity_handle_modules(
    const command_line& command,
    const std::vector<rendered_entity_handle_module>& modules) {
    const std::map<
        std::string,
        std::filesystem::path,
        std::less<>>
        configured =
            validated_entity_handle_outputs(command, modules);
    std::map<
        std::string,
        const rendered_entity_handle_module*,
        std::less<>>
        generated;
    for (const rendered_entity_handle_module& module : modules) {
        generated.emplace(module.owner_key, &module);
    }
    for (const auto& [owner_key, module] : generated) {
        write_if_different(
            configured.at(owner_key), module->content);
    }
}

inline void publish_row_codec_modules(
    const command_line& command,
    const std::vector<rendered_row_codec_module>& modules) {
    std::map<
        std::string,
        std::filesystem::path,
        std::less<>>
        configured;
    std::set<std::filesystem::path> configured_paths;
    for (const configured_row_codec_output& output :
         command.row_codec_outputs) {
        if (!configured.emplace(output.owner_key, output.path).second) {
            throw std::runtime_error(
                "duplicate configured row-codec output for owner '"
                + output.owner_key + "'");
        }
        if (!configured_paths.insert(
                output.path.lexically_normal()).second) {
            throw std::runtime_error(
                "more than one row-codec owner targets configured path '"
                + output.path.string() + "'");
        }
    }
    std::map<
        std::string,
        const rendered_row_codec_module*,
        std::less<>>
        generated;
    for (const rendered_row_codec_module& module : modules) {
        generated.emplace(module.owner_key, &module);
    }
    for (const auto& [owner_key, module] : generated) {
        static_cast<void>(module);
        if (!configured.contains(owner_key)) {
            throw std::runtime_error(
                "generated row-codec owner '" + owner_key
                + "' has no configure-known output");
        }
    }
    for (const auto& [owner_key, path] : configured) {
        static_cast<void>(path);
        if (!generated.contains(owner_key)) {
            throw std::runtime_error(
                "configured row-codec output owner '" + owner_key
                + "' was not produced by the annotated source graph");
        }
    }
    for (const auto& [owner_key, module] : generated) {
        write_if_different(
            configured.at(owner_key), module->content);
    }
}

inline void publish_fmgr_entrypoint_modules(
    const command_line& command,
    const std::vector<rendered_fmgr_entrypoint_module>& modules,
    bool support_owned) {
    std::map<std::string, std::filesystem::path, std::less<>>
        configured;
    std::set<std::filesystem::path> configured_paths;
    for (const configured_fmgr_entrypoint_output& output :
         command.fmgr_entrypoint_outputs) {
        if (support_owned_fmgr_output(output.owner_key)
            != support_owned) {
            continue;
        }
        if (!configured.emplace(output.owner_key, output.path).second) {
            throw std::runtime_error(
                "duplicate configured fmgr-entrypoint output for owner '"
                + output.owner_key + "'");
        }
        if (!configured_paths.insert(
                output.path.lexically_normal()).second) {
            throw std::runtime_error(
                "more than one fmgr-entrypoint owner targets configured "
                "path '" + output.path.string() + "'");
        }
    }
    std::map<
        std::string,
        const rendered_fmgr_entrypoint_module*,
        std::less<>>
        generated;
    for (const rendered_fmgr_entrypoint_module& module : modules) {
        if (support_owned_fmgr_output(module.owner_key)
            != support_owned) {
            continue;
        }
        if (!generated.emplace(module.owner_key, &module).second) {
            throw std::runtime_error(
                "duplicate generated fmgr-entrypoint owner '"
                + module.owner_key + "'");
        }
    }
    for (const auto& [owner_key, module] : generated) {
        static_cast<void>(module);
        if (!configured.contains(owner_key)) {
            throw std::runtime_error(
                "generated fmgr-entrypoint owner '" + owner_key
                + "' has no configure-known output");
        }
    }
    for (const auto& [owner_key, path] : configured) {
        static_cast<void>(path);
        if (!generated.contains(owner_key)) {
            throw std::runtime_error(
                "configured fmgr-entrypoint output owner '" + owner_key
                + "' was not produced by the annotated source graph");
        }
    }
    for (const auto& [owner_key, module] : generated) {
        write_if_different(
            configured.at(owner_key), module->content);
    }
}

inline void verify_support_fmgr_entrypoint_modules(
    const command_line& command,
    const std::vector<rendered_fmgr_entrypoint_module>& modules) {
    std::map<std::string, std::filesystem::path, std::less<>>
        configured;
    for (const configured_fmgr_entrypoint_output& output :
         command.fmgr_entrypoint_outputs) {
        if (!support_owned_fmgr_output(output.owner_key)) continue;
        if (!configured.emplace(output.owner_key, output.path).second) {
            throw std::runtime_error(
                "duplicate configured SUPPORT registry output for owner '"
                + output.owner_key + "'");
        }
    }
    std::map<
        std::string,
        const rendered_fmgr_entrypoint_module*,
        std::less<>>
        generated;
    for (const rendered_fmgr_entrypoint_module& module : modules) {
        if (!support_owned_fmgr_output(module.owner_key)) continue;
        if (!generated.emplace(module.owner_key, &module).second) {
            throw std::runtime_error(
                "duplicate FINAL registry projection for owner '"
                + module.owner_key + "'");
        }
    }
    if (configured.size() != 2 || generated.size() != 2
        || configured.size() != generated.size()) {
        throw std::runtime_error(
            "SUPPORT/FINAL vector registry owner set is incomplete");
    }
    for (const auto& [owner_key, module] : generated) {
        const auto output = configured.find(owner_key);
        if (output == configured.end()) {
            throw std::runtime_error(
                "FINAL produced unexpected SUPPORT registry owner '"
                + owner_key + "'");
        }
        if (read_file(output->second) != module->content) {
            throw std::runtime_error(
                "SUPPORT vector registry bytes differ from FINAL "
                "annotated projection for owner '" + owner_key + "'");
        }
    }
}

[[nodiscard]] inline std::filesystem::path guc_owner_output_path(
    const command_line& command,
    const rendered_guc_owner_module& owner,
    guc_profile profile) {
    return command.guc_output_root
        / std::string{guc_profile_name(profile)}
        / "owners"
        / (owner.owner_key + ".gucs.cppm");
}

[[nodiscard]] inline std::filesystem::path guc_registry_output_path(
    const command_line& command,
    guc_profile profile) {
    return command.guc_output_root
        / std::string{guc_profile_name(profile)}
        / "guc_registry.cppm";
}

[[nodiscard]] inline std::vector<std::pair<
    std::filesystem::path,
    std::string>>
guc_derived_outputs(
    const command_line& command,
    const rendered_guc_package& package) {
    std::vector<std::pair<
        std::filesystem::path,
        std::string>>
        result;
    result.emplace_back(
        guc_registry_output_path(command, package.profile),
        "GUC registry");
    for (const rendered_guc_owner_module& owner :
         package.owners) {
        result.emplace_back(
            guc_owner_output_path(
                command, owner, package.profile),
            "GUC owner " + owner.owner_key);
    }
    return result;
}

inline void publish_guc_package(
    const command_line& command,
    const rendered_guc_package& package) {
    for (const rendered_guc_owner_module& owner : package.owners) {
        write_if_different(
            guc_owner_output_path(
                command, owner, package.profile),
            owner.content);
    }
    write_if_different(
        guc_registry_output_path(command, package.profile),
        package.registry.content);
}

[[nodiscard]] inline std::string depfile_escape(
    const std::filesystem::path& path) {
    std::string result;
    for (const char value : path.generic_string()) {
        if (value == '$') {
            result += "$$";
        } else {
            if (value == ' ' || value == '#' || value == ':'
                || value == '\\') {
                result.push_back('\\');
            }
            result.push_back(value);
        }
    }
    return result;
}

[[nodiscard]] inline std::string render_depfile(
    const std::filesystem::path& target,
    std::vector<std::filesystem::path> dependencies) {
    std::ranges::sort(
        dependencies,
        {},
        [](const std::filesystem::path& path) {
            return path.generic_string();
        });
    const auto duplicate = std::ranges::unique(
        dependencies,
        {},
        [](const std::filesystem::path& path) {
            return path.generic_string();
        });
    dependencies.erase(duplicate.begin(), duplicate.end());
    std::ostringstream output;
    output << depfile_escape(target) << ":";
    for (const std::filesystem::path& dependency : dependencies) {
        output << " " << depfile_escape(dependency);
    }
    output << "\n";
    return output.str();
}

[[nodiscard]] inline std::string json_string(std::string_view value) {
    std::string result{"\""};
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        switch (character) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (character < 0x20U) {
                    result += "\\u00";
                    result.push_back(hex[character >> 4U]);
                    result.push_back(hex[character & 0x0fU]);
                } else {
                    result.push_back(static_cast<char>(character));
                }
        }
    }
    result.push_back('"');
    return result;
}

[[nodiscard]] inline std::string support_descriptor_hash(
    std::string_view compiler_profile,
    const std::vector<rendered_row_codec_module>& row_modules,
    const rendered_guc_package& guc_package,
    const std::vector<rendered_fmgr_entrypoint_module>&
        vector_registry_modules) {
    std::vector<std::string> rows;
    rows.reserve(row_modules.size());
    for (const rendered_row_codec_module& module : row_modules) {
        rows.push_back(
            "row\t" + module.owner_key + "\t" + module.source_module
            + "\t" + module.module_name + "\t"
            + sha256_hex(module.content));
    }
    for (const rendered_guc_owner_module& owner :
         guc_package.owners) {
        rows.push_back(
            "guc-owner\t" + owner.owner_key + "\t"
            + owner.source_module + "\t" + owner.module_name + "\t"
            + sha256_hex(owner.content));
    }
    rows.push_back(
        "guc-registry\t"
        + std::string{guc_profile_name(guc_package.profile)}
        + "\t" + guc_package.registry.module_name + "\t"
        + sha256_hex(guc_package.registry.content));
    for (const rendered_fmgr_entrypoint_module& module :
         vector_registry_modules) {
        if (!support_owned_fmgr_output(module.owner_key)) {
            throw std::runtime_error(
                "SUPPORT descriptor received non-registry fmgr owner '"
                + module.owner_key + "'");
        }
        rows.push_back(
            "vector-registry\t" + module.owner_key + "\t"
            + module.module_name + "\t"
            + sha256_hex(module.content));
    }
    std::ranges::sort(rows);
    std::string material =
        "bioseq_sql_support_descriptor/v1\n"
        + std::string{compiler_profile} + "\n";
    for (const std::string& row : rows) {
        material += row + "\n";
    }
    return sha256_hex(material);
}

[[nodiscard]] inline std::string render_support_manifest(
    std::string_view compiler_profile,
    std::string_view clang_version,
    std::string_view compilation_signature,
    const std::vector<rendered_row_codec_module>& row_modules,
    const guc_inventory& inventory,
    const rendered_guc_package& guc_package,
    const std::vector<rendered_fmgr_entrypoint_module>&
        vector_registry_modules) {
    const std::string descriptor = support_descriptor_hash(
        compiler_profile,
        row_modules,
        guc_package,
        vector_registry_modules);
    const guc_profile_projection guc_projection =
        make_guc_profile_projection(
            inventory, guc_package);
    std::ostringstream output;
    output
        << "{\n"
        << "  \"schema\": \"bioseq_sql_support_manifest/v1\",\n"
        << "  \"compiler_profile\": "
        << json_string(compiler_profile) << ",\n"
        << "  \"clang_version\": "
        << json_string(clang_version) << ",\n"
        << "  \"compilation_signature\": "
        << json_string(compilation_signature) << ",\n"
        << "  \"support_descriptor_sha256\": "
        << json_string(descriptor) << ",\n"
        << "  \"row_codecs\": [";
    for (std::size_t index = 0;
         index < row_modules.size();
         ++index) {
        const rendered_row_codec_module& module = row_modules[index];
        output
            << (index == 0 ? "\n" : ",\n")
            << "    {\"owner\": " << json_string(module.owner_key)
            << ", \"source_module\": "
            << json_string(module.source_module)
            << ", \"generated_module\": "
            << json_string(module.module_name)
            << ", \"content_sha256\": "
            << json_string(sha256_hex(module.content)) << "}";
    }
    output << (row_modules.empty() ? "" : "\n")
           << "  ],\n"
           << "  \"guc_profile\": "
           << json_string(guc_profile_name(guc_package.profile))
           << ",\n"
           << "  \"guc_profile_projection\": {\n"
           << "    \"schema\": "
           << "\"bioseq_guc_profile_projection/v1\",\n"
           << "    \"profile\": "
           << json_string(guc_profile_name(guc_projection.profile))
           << ",\n"
           << "    \"ledger_sha256\": "
           << json_string(guc_projection.ledger_sha256)
           << ",\n"
           << "    \"registry_content_sha256\": "
           << json_string(
                  guc_projection.registry_content_sha256)
           << ",\n"
           << "    \"owner_content_sha256\": {\n";
    std::size_t guc_owner_index = 0;
    for (const auto& [owner, content_sha256] :
         guc_projection.owner_content_sha256) {
        output << "      " << json_string(owner) << ": "
               << json_string(content_sha256)
               << (++guc_owner_index
                           == guc_projection.owner_content_sha256.size()
                       ? "\n"
                       : ",\n");
    }
    output << "    },\n"
           << "    \"registrations\": [\n";
    for (std::size_t index = 0;
         index < guc_projection.registrations.size();
         ++index) {
        const guc_profile_projection_row& row =
            guc_projection.registrations[index];
        output
            << "      {\"registration_ordinal\": "
            << row.registration_ordinal
            << ", \"name\": " << json_string(row.name)
            << ", \"owner\": " << json_string(row.owner_key)
            << ", \"source_module\": "
            << json_string(row.source_module)
            << ", \"source_sha256\": "
            << json_string(row.source_sha256)
            << ", \"descriptor_sha256\": "
            << json_string(row.descriptor_sha256) << "}"
            << (index + 1
                        == guc_projection.registrations.size()
                    ? "\n"
                    : ",\n");
    }
    output << "    ]\n"
           << "  },\n"
           << "  \"guc_registry\": {\"generated_module\": "
           << json_string(guc_package.registry.module_name)
           << ", \"content_sha256\": "
           << json_string(
                  sha256_hex(guc_package.registry.content))
           << "},\n"
           << "  \"guc_owners\": [";
    for (std::size_t index = 0;
         index < guc_package.owners.size();
         ++index) {
        const rendered_guc_owner_module& owner =
            guc_package.owners[index];
        output
            << (index == 0 ? "\n" : ",\n")
            << "    {\"owner\": " << json_string(owner.owner_key)
            << ", \"source_module\": "
            << json_string(owner.source_module)
            << ", \"generated_module\": "
            << json_string(owner.module_name)
            << ", \"content_sha256\": "
            << json_string(sha256_hex(owner.content)) << "}";
    }
    output << (guc_package.owners.empty() ? "" : "\n")
           << "  ],\n"
           << "  \"vector_registries\": [";
    for (std::size_t index = 0;
         index < vector_registry_modules.size();
         ++index) {
        const rendered_fmgr_entrypoint_module& module =
            vector_registry_modules[index];
        output
            << (index == 0 ? "\n" : ",\n")
            << "    {\"owner\": " << json_string(module.owner_key)
            << ", \"generated_module\": "
            << json_string(module.module_name)
            << ", \"content_sha256\": "
            << json_string(module.content_sha256) << "}";
    }
    output << (vector_registry_modules.empty() ? "" : "\n")
           << "  ]\n"
           << "}\n";
    return output.str();
}

[[nodiscard]] inline std::string render_support_seal(
    std::string_view compiler_profile,
    std::string_view compilation_signature,
    std::string_view support_descriptor,
    std::string_view manifest) {
    std::ostringstream output;
    output
        << "{\n"
        << "  \"schema\": \"bioseq_sql_support_seal/v1\",\n"
        << "  \"compiler_profile\": "
        << json_string(compiler_profile) << ",\n"
        << "  \"compilation_signature\": "
        << json_string(compilation_signature) << ",\n"
        << "  \"support_descriptor_sha256\": "
        << json_string(support_descriptor) << ",\n"
        << "  \"support_manifest_sha256\": "
        << json_string(sha256_hex(manifest)) << "\n"
        << "}\n";
    return output.str();
}

[[nodiscard]] inline std::string json_string_field(
    std::string_view document,
    std::string_view field) {
    const std::string needle =
        "\"" + std::string{field} + "\"";
    const std::size_t position = document.find(needle);
    if (position == std::string_view::npos
        || document.find(needle, position + needle.size())
            != std::string_view::npos) {
        throw std::runtime_error(
            "support manifest must contain exactly one string field '"
            + std::string{field} + "'");
    }
    std::size_t cursor = position + needle.size();
    while (cursor < document.size()
           && (document[cursor] == ' '
               || document[cursor] == '\t'
               || document[cursor] == '\r'
               || document[cursor] == '\n')) {
        ++cursor;
    }
    if (cursor == document.size() || document[cursor++] != ':') {
        throw std::runtime_error(
            "malformed support manifest field '"
            + std::string{field} + "'");
    }
    while (cursor < document.size()
           && (document[cursor] == ' '
               || document[cursor] == '\t'
               || document[cursor] == '\r'
               || document[cursor] == '\n')) {
        ++cursor;
    }
    if (cursor == document.size() || document[cursor++] != '"') {
        throw std::runtime_error(
            "support manifest field '" + std::string{field}
            + "' is not a string");
    }
    std::string result;
    bool escaped = false;
    for (; cursor < document.size(); ++cursor) {
        const char character = document[cursor];
        if (escaped) {
            throw std::runtime_error(
                "support manifest field '" + std::string{field}
                + "' unexpectedly contains JSON escapes");
        }
        if (character == '\\') {
            escaped = true;
            continue;
        }
        if (character == '"') return result;
        result.push_back(character);
    }
    throw std::runtime_error(
        "unterminated support manifest field '"
        + std::string{field} + "'");
}

inline void verify_row_codec_modules(
    const command_line& command,
    const std::vector<rendered_row_codec_module>& modules) {
    std::map<std::string, std::filesystem::path, std::less<>>
        configured;
    std::set<std::filesystem::path> configured_paths;
    for (const configured_row_codec_output& output :
         command.row_codec_outputs) {
        if (!configured.emplace(output.owner_key, output.path).second) {
            throw std::runtime_error(
                "duplicate configured row-codec output for owner '"
                + output.owner_key + "'");
        }
        if (!configured_paths.insert(
                output.path.lexically_normal()).second) {
            throw std::runtime_error(
                "more than one FINAL row-codec owner targets path '"
                + output.path.string() + "'");
        }
    }
    if (configured.size() != modules.size()) {
        throw std::runtime_error(
            "final SQL generation row-codec owner set differs from "
            "the SUPPORT owner set");
    }
    for (const rendered_row_codec_module& module : modules) {
        const auto found = configured.find(module.owner_key);
        if (found == configured.end()) {
            throw std::runtime_error(
                "final SQL generation produced unexpected row-codec "
                "owner '" + module.owner_key + "'");
        }
        if (read_file(found->second) != module.content) {
            throw std::runtime_error(
                "SUPPORT row-codec bytes differ from FINAL projection "
                "for owner '" + module.owner_key + "'");
        }
    }
}

inline void verify_guc_package(
    const command_line& command,
    const rendered_guc_package& package) {
    for (const rendered_guc_owner_module& owner : package.owners) {
        const std::filesystem::path path =
            guc_owner_output_path(command, owner, package.profile);
        if (read_file(path) != owner.content) {
            throw std::runtime_error(
                "SUPPORT GUC owner bytes differ from FINAL projection "
                "for owner '" + owner.owner_key + "'");
        }
    }
    if (read_file(guc_registry_output_path(command, package.profile))
        != package.registry.content) {
        throw std::runtime_error(
            "SUPPORT GUC registry bytes differ from FINAL projection");
    }
}

[[nodiscard]] inline std::string core_descriptor_hash(
    const std::vector<rendered_entity_handle_module>& modules) {
    std::vector<std::string> rows;
    rows.reserve(modules.size());
    for (const rendered_entity_handle_module& module : modules) {
        rows.push_back(
            module.owner_key + "\t" + module.source_module + "\t"
            + module.module_name + "\t"
            + (module.profile_invariant ? "invariant" : "variant")
            + "\t" + sha256_hex(module.content));
    }
    std::ranges::sort(rows);
    std::string material{"bioseq_sql_core_descriptor/v1\n"};
    for (const std::string& row : rows) {
        material += row + "\n";
    }
    return sha256_hex(material);
}

[[nodiscard]] inline std::string core_entity_handle_logical_path(
    std::string_view owner_key) {
    return "core/owners/" + std::string{owner_key}
        + ".entity_handles.cppm";
}

[[nodiscard]] inline std::string core_artifact_set_hash(
    const command_line& command,
    const std::vector<rendered_entity_handle_module>& modules) {
    static_cast<void>(
        validated_entity_handle_outputs(command, modules));
    std::vector<std::string> rows;
    rows.reserve(modules.size());
    for (const rendered_entity_handle_module& module : modules) {
        rows.push_back(
            module.owner_key + "\t" + module.source_module + "\t"
            + module.module_name + "\t"
            + (module.profile_invariant ? "invariant" : "variant")
            + "\t"
            + core_entity_handle_logical_path(module.owner_key)
            + "\t" + sha256_hex(module.content));
    }
    std::ranges::sort(rows);
    std::string material{"bioseq_sql_core_artifact_set/v1\n"};
    for (const std::string& row : rows) {
        material += row + "\n";
    }
    return sha256_hex(material);
}

[[nodiscard]] inline std::string render_core_manifest(
    std::string_view clang_version,
    std::string_view compilation_signature,
    const command_line& command,
    const std::vector<rendered_entity_handle_module>& modules) {
    std::map<
        std::string,
        const rendered_entity_handle_module*,
        std::less<>>
        generated;
    for (const rendered_entity_handle_module& module : modules) {
        generated.emplace(module.owner_key, &module);
    }
    const std::string descriptor =
        core_descriptor_hash(modules);
    const std::string artifact_set =
        core_artifact_set_hash(command, modules);
    std::ostringstream output;
    output
        << "{\n"
        << "  \"schema\": \"bioseq_sql_core_manifest/v1\",\n"
        << "  \"compiler_profile\": \"production\",\n"
        << "  \"clang_version\": " << json_string(clang_version)
        << ",\n"
        << "  \"compilation_signature\": "
        << json_string(compilation_signature) << ",\n"
        << "  \"core_descriptor_sha256\": "
        << json_string(descriptor) << ",\n"
        << "  \"core_artifact_set_sha256\": "
        << json_string(artifact_set) << ",\n"
        << "  \"module_count\": "
        << json_string(std::to_string(generated.size()))
        << ",\n"
        << "  \"modules\": [";
    std::size_t index = 0;
    for (const auto& [owner_key, module] : generated) {
        output
            << (index++ == 0 ? "\n" : ",\n")
            << "    {\"owner\": " << json_string(owner_key)
            << ", \"source_module\": "
            << json_string(module->source_module)
            << ", \"generated_module\": "
            << json_string(module->module_name)
            << ", \"configured_path\": "
            << json_string(
                   core_entity_handle_logical_path(owner_key))
            << ", \"profile\": "
            << json_string(
                   module->profile_invariant
                   ? "invariant"
                   : "variant")
            << ", \"content_sha256\": "
            << json_string(sha256_hex(module->content)) << "}";
    }
    output << (generated.empty() ? "" : "\n")
           << "  ]\n"
           << "}\n";
    return output.str();
}

[[nodiscard]] inline std::string render_core_seal(
    std::string_view descriptor,
    std::string_view compilation_signature,
    std::string_view manifest) {
    std::ostringstream output;
    output
        << "{\n"
        << "  \"schema\": \"bioseq_sql_core_seal/v1\",\n"
        << "  \"core_descriptor_sha256\": "
        << json_string(descriptor) << ",\n"
        << "  \"compilation_signature\": "
        << json_string(compilation_signature) << ",\n"
        << "  \"core_manifest_sha256\": "
        << json_string(sha256_hex(manifest)) << "\n"
        << "}\n";
    return output.str();
}

[[nodiscard]] inline bool is_sha256_hex(
    std::string_view value) noexcept {
    return value.size() == 64
        && std::ranges::all_of(
            value,
            [](const char character) {
                return (character >= '0' && character <= '9')
                    || (character >= 'a' && character <= 'f');
            });
}

[[nodiscard]] inline std::string trim_artifact(
    std::string value) {
    while (!value.empty()
           && (value.back() == ' ' || value.back() == '\t'
               || value.back() == '\r'
               || value.back() == '\n')) {
        value.pop_back();
    }
    return value;
}

inline void verify_core_artifacts(
    const command_line& command,
    std::string_view clang_version,
    const std::vector<rendered_entity_handle_module>& modules) {
    const std::string descriptor =
        core_descriptor_hash(modules);
    const std::string manifest =
        read_file(command.core_manifest);
    const std::string seal =
        read_file(command.expected_core_hash);
    if (json_string_field(manifest, "schema")
            != "bioseq_sql_core_manifest/v1"
        || json_string_field(seal, "schema")
            != "bioseq_sql_core_seal/v1") {
        throw std::runtime_error(
            "FINAL received an incompatible CORE manifest or seal");
    }
    const std::string core_signature =
        json_string_field(manifest, "compilation_signature");
    if (!is_sha256_hex(core_signature)
        || json_string_field(seal, "compilation_signature")
            != core_signature) {
        throw std::runtime_error(
            "CORE compilation signature is malformed or unsealed");
    }
    if (json_string_field(manifest, "compiler_profile")
            != "production"
        || json_string_field(manifest, "clang_version")
            != clang_version) {
        throw std::runtime_error(
            "CORE manifest compiler environment differs from FINAL");
    }
    if (json_string_field(manifest, "core_descriptor_sha256")
            != descriptor
        || json_string_field(seal, "core_descriptor_sha256")
            != descriptor) {
        throw std::runtime_error(
            "CORE descriptor hash differs from FINAL projection");
    }
    if (json_string_field(
            manifest, "core_artifact_set_sha256")
        != core_artifact_set_hash(command, modules)) {
        throw std::runtime_error(
            "CORE manifest artifact-set seal differs from FINAL "
            "projection");
    }
    const std::string expected_manifest =
        render_core_manifest(
            clang_version,
            core_signature,
            command,
            modules);
    if (manifest != expected_manifest) {
        throw std::runtime_error(
            "CORE manifest entries differ from FINAL projection");
    }
    if (json_string_field(seal, "core_manifest_sha256")
        != sha256_hex(manifest)) {
        throw std::runtime_error(
            "CORE manifest content hash differs from its seal");
    }
    const std::map<
        std::string,
        std::filesystem::path,
        std::less<>>
        configured =
            validated_entity_handle_outputs(command, modules);
    for (const rendered_entity_handle_module& module : modules) {
        if (read_file(configured.at(module.owner_key))
            != module.content) {
            throw std::runtime_error(
                "CORE entity-handle bytes differ from FINAL "
                "projection for owner '" + module.owner_key + "'");
        }
    }
}

inline void verify_support_manifest(
    const command_line& command,
    std::string_view clang_version,
    const std::vector<rendered_row_codec_module>& row_modules,
    const guc_inventory& inventory,
    const rendered_guc_package& guc_package,
    const std::vector<rendered_fmgr_entrypoint_module>&
        vector_registry_modules) {
    const std::string expected = support_descriptor_hash(
        command.compiler_profile,
        row_modules,
        guc_package,
        vector_registry_modules);
    const std::string document = read_file(command.support_manifest);
    const std::string seal = read_file(command.support_seal);
    if (json_string_field(seal, "schema")
            != "bioseq_sql_support_seal/v1") {
        throw std::runtime_error(
            "FINAL received an incompatible SUPPORT manifest or seal");
    }
    const std::string actual = json_string_field(
        document, "support_descriptor_sha256");
    const std::string support_signature =
        json_string_field(document, "compilation_signature");
    if (!is_sha256_hex(support_signature)
        || json_string_field(seal, "compilation_signature")
            != support_signature) {
        throw std::runtime_error(
            "SUPPORT compilation signature is malformed or unsealed");
    }
    if (json_string_field(document, "compiler_profile")
            != command.compiler_profile
        || json_string_field(document, "clang_version")
            != clang_version
        || json_string_field(seal, "compiler_profile")
            != command.compiler_profile) {
        throw std::runtime_error(
            "SUPPORT manifest compiler environment differs from FINAL");
    }
    if (actual != expected
        || json_string_field(seal, "support_descriptor_sha256")
            != expected) {
        throw std::runtime_error(
            "SUPPORT descriptor hash differs from FINAL projection");
    }
    const std::string expected_document =
        render_support_manifest(
            command.compiler_profile,
            clang_version,
            support_signature,
            row_modules,
            inventory,
            guc_package,
            vector_registry_modules);
    if (document != expected_document) {
        throw std::runtime_error(
            "SUPPORT manifest entries differ from FINAL projection");
    }
    if (json_string_field(seal, "support_manifest_sha256")
        != sha256_hex(document)) {
        throw std::runtime_error(
            "SUPPORT manifest content hash differs from its seal");
    }
}

[[nodiscard]] inline guc_profile_projection
load_sealed_guc_profile_projection(
    const std::filesystem::path& manifest_path,
    const std::filesystem::path& seal_path,
    const std::filesystem::path& guc_output_root,
    guc_profile expected_profile,
    std::string_view expected_clang_version) {
    if (manifest_path.empty() || seal_path.empty()
        || std::filesystem::is_symlink(manifest_path)
        || std::filesystem::is_symlink(seal_path)
        || !std::filesystem::is_regular_file(manifest_path)
        || !std::filesystem::is_regular_file(seal_path)) {
        throw std::runtime_error(
            "SUPPORT profile handoff is missing or uses a symlink");
    }
    const std::string manifest_document =
        read_file(manifest_path);
    const std::string seal_document = read_file(seal_path);
    llvm::Expected<llvm::json::Value> parsed_manifest =
        llvm::json::parse(manifest_document);
    llvm::Expected<llvm::json::Value> parsed_seal =
        llvm::json::parse(seal_document);
    if (!parsed_manifest || !parsed_seal) {
        if (!parsed_manifest) {
            llvm::consumeError(parsed_manifest.takeError());
        }
        if (!parsed_seal) {
            llvm::consumeError(parsed_seal.takeError());
        }
        throw std::runtime_error(
            "cannot parse sealed SUPPORT profile handoff");
    }
    const llvm::json::Object* root =
        parsed_manifest->getAsObject();
    const llvm::json::Object* seal =
        parsed_seal->getAsObject();
    if (root == nullptr || seal == nullptr) {
        throw std::runtime_error(
            "sealed SUPPORT profile handoff is not a JSON object");
    }
    const auto require_exact =
        [](const llvm::json::Object& object,
           std::initializer_list<std::string_view> fields,
           std::string_view role) {
            for (const std::string_view field : fields) {
                if (object.get(field) == nullptr) {
                    throw std::runtime_error(
                        std::string{role} + " is missing field '"
                        + std::string{field} + "'");
                }
            }
            if (object.size() != fields.size()) {
                throw std::runtime_error(
                    std::string{role}
                    + " contains an unknown field");
            }
        };
    const auto string_field =
        [](const llvm::json::Object& object,
           std::string_view field,
           std::string_view role) -> std::string {
            const std::optional<llvm::StringRef> value =
                object.getString(field);
            if (!value) {
                throw std::runtime_error(
                    std::string{role} + " field '"
                    + std::string{field} + "' is not a string");
            }
            return value->str();
        };
    require_exact(
        *root,
        {
            "schema",
            "compiler_profile",
            "clang_version",
            "compilation_signature",
            "support_descriptor_sha256",
            "row_codecs",
            "guc_profile",
            "guc_profile_projection",
            "guc_registry",
            "guc_owners",
            "vector_registries",
        },
        "SUPPORT manifest");
    require_exact(
        *seal,
        {
            "schema",
            "compiler_profile",
            "compilation_signature",
            "support_descriptor_sha256",
            "support_manifest_sha256",
        },
        "SUPPORT seal");
    const std::string profile_name{
        guc_profile_name(expected_profile)};
    if (string_field(*root, "schema", "SUPPORT manifest")
            != "bioseq_sql_support_manifest/v1"
        || string_field(*seal, "schema", "SUPPORT seal")
            != "bioseq_sql_support_seal/v1"
        || string_field(
               *root, "compiler_profile", "SUPPORT manifest")
            != profile_name
        || string_field(*root, "guc_profile", "SUPPORT manifest")
            != profile_name
        || string_field(
               *seal, "compiler_profile", "SUPPORT seal")
            != profile_name
        || string_field(*root, "clang_version", "SUPPORT manifest")
            != expected_clang_version
        || !is_sha256_hex(string_field(
            *root,
            "compilation_signature",
            "SUPPORT manifest"))
        || !is_sha256_hex(string_field(
            *root,
            "support_descriptor_sha256",
            "SUPPORT manifest"))
        || string_field(
               *root,
               "compilation_signature",
               "SUPPORT manifest")
            != string_field(
                *seal,
                "compilation_signature",
                "SUPPORT seal")
        || string_field(
               *root,
               "support_descriptor_sha256",
               "SUPPORT manifest")
            != string_field(
                *seal,
                "support_descriptor_sha256",
                "SUPPORT seal")
        || string_field(
               *seal,
               "support_manifest_sha256",
               "SUPPORT seal")
            != sha256_hex(manifest_document)) {
        throw std::runtime_error(
            "SUPPORT profile handoff seal, role, or toolchain differs "
            "from FINAL");
    }
    const llvm::json::Object* projection =
        root->getObject("guc_profile_projection");
    if (projection == nullptr) {
        throw std::runtime_error(
            "SUPPORT manifest has no GUC profile projection");
    }
    require_exact(
        *projection,
        {
            "schema",
            "profile",
            "ledger_sha256",
            "registry_content_sha256",
            "owner_content_sha256",
            "registrations",
        },
        "GUC profile projection");
    guc_profile_projection result{
        .profile = expected_profile,
        .ledger_sha256 = string_field(
            *projection,
            "ledger_sha256",
            "GUC profile projection"),
        .registry_content_sha256 = string_field(
            *projection,
            "registry_content_sha256",
            "GUC profile projection"),
    };
    if (string_field(
            *projection, "schema", "GUC profile projection")
            != "bioseq_guc_profile_projection/v1"
        || string_field(
               *projection, "profile", "GUC profile projection")
            != profile_name
        || !is_sha256_hex(result.ledger_sha256)
        || !is_sha256_hex(result.registry_content_sha256)) {
        throw std::runtime_error(
            "SUPPORT GUC profile projection schema or hashes are "
            "malformed");
    }
    const llvm::json::Object* owner_hashes =
        projection->getObject("owner_content_sha256");
    if (owner_hashes == nullptr) {
        throw std::runtime_error(
            "SUPPORT GUC profile projection has no owner seals");
    }
    require_exact(
        *owner_hashes,
        {"aligner", "bioseq", "scheduler", "storage"},
        "GUC profile owner seals");
    for (const std::string_view owner :
         {"aligner", "bioseq", "scheduler", "storage"}) {
        const std::string hash = string_field(
            *owner_hashes, owner, "GUC profile owner seals");
        if (!is_sha256_hex(hash)) {
            throw std::runtime_error(
                "SUPPORT GUC owner content hash is malformed");
        }
        result.owner_content_sha256.emplace(owner, hash);
    }
    const llvm::json::Array* registrations =
        projection->getArray("registrations");
    if (registrations == nullptr || registrations->empty()) {
        throw std::runtime_error(
            "SUPPORT GUC profile projection has no registrations");
    }
    std::set<std::string, std::less<>> names;
    std::optional<std::size_t> previous_ordinal;
    for (const llvm::json::Value& value : *registrations) {
        const llvm::json::Object* row = value.getAsObject();
        if (row == nullptr) {
            throw std::runtime_error(
                "SUPPORT GUC profile registration is not an object");
        }
        require_exact(
            *row,
            {
                "registration_ordinal",
                "name",
                "owner",
                "source_module",
                "source_sha256",
                "descriptor_sha256",
            },
            "GUC profile registration");
        const std::optional<std::int64_t> ordinal =
            row->getInteger("registration_ordinal");
        if (!ordinal || *ordinal < 0) {
            throw std::runtime_error(
                "SUPPORT GUC registration ordinal is malformed");
        }
        guc_profile_projection_row projected_row{
            .registration_ordinal =
                static_cast<std::size_t>(*ordinal),
            .name = string_field(
                *row, "name", "GUC profile registration"),
            .owner_key = string_field(
                *row, "owner", "GUC profile registration"),
            .source_module = string_field(
                *row,
                "source_module",
                "GUC profile registration"),
            .source_sha256 = string_field(
                *row,
                "source_sha256",
                "GUC profile registration"),
            .descriptor_sha256 = string_field(
                *row,
                "descriptor_sha256",
                "GUC profile registration"),
        };
        if ((previous_ordinal
             && projected_row.registration_ordinal
                    <= *previous_ordinal)
            || !names.insert(projected_row.name).second
            || !result.owner_content_sha256.contains(
                projected_row.owner_key)
            || !is_sha256_hex(projected_row.source_sha256)
            || !is_sha256_hex(
                projected_row.descriptor_sha256)) {
            throw std::runtime_error(
                "SUPPORT GUC profile registration order or provenance "
                "is malformed");
        }
        previous_ordinal = projected_row.registration_ordinal;
        result.registrations.push_back(
            std::move(projected_row));
    }
    const llvm::json::Object* registry =
        root->getObject("guc_registry");
    if (registry == nullptr) {
        throw std::runtime_error(
            "SUPPORT manifest has no GUC registry seal");
    }
    require_exact(
        *registry,
        {"generated_module", "content_sha256"},
        "SUPPORT GUC registry");
    const std::string expected_registry_module =
        "bioseq.sql.generated." + profile_name + ".guc_registry";
    const std::filesystem::path registry_path =
        guc_output_root / profile_name / "guc_registry.cppm";
    if (string_field(
            *registry,
            "generated_module",
            "SUPPORT GUC registry")
            != expected_registry_module
        || std::filesystem::is_symlink(registry_path)
        || !std::filesystem::is_regular_file(registry_path)
        || sha256_hex(read_file(registry_path))
            != result.registry_content_sha256
        || string_field(
            *registry,
            "content_sha256",
            "SUPPORT GUC registry")
        != result.registry_content_sha256) {
        throw std::runtime_error(
            "SUPPORT GUC profile registry identity or bytes differ "
            "from the package seal");
    }
    const llvm::json::Array* owners =
        root->getArray("guc_owners");
    if (owners == nullptr || owners->size() != 4) {
        throw std::runtime_error(
            "SUPPORT manifest does not seal four GUC owners");
    }
    std::set<std::string, std::less<>> sealed_owners;
    std::map<std::string, std::string, std::less<>>
        owner_source_modules;
    for (const llvm::json::Value& value : *owners) {
        const llvm::json::Object* owner = value.getAsObject();
        if (owner == nullptr) {
            throw std::runtime_error(
                "SUPPORT GUC owner seal is not an object");
        }
        require_exact(
            *owner,
            {
                "owner",
                "source_module",
                "generated_module",
                "content_sha256",
            },
            "SUPPORT GUC owner seal");
        const std::string owner_name = string_field(
            *owner, "owner", "SUPPORT GUC owner seal");
        const auto expected =
            result.owner_content_sha256.find(owner_name);
        const std::string source_module = string_field(
            *owner, "source_module", "SUPPORT GUC owner seal");
        const std::string expected_generated_module =
            "bioseq.sql.generated." + profile_name + "."
            + owner_name + ".gucs";
        const std::filesystem::path owner_path =
            guc_output_root / profile_name / "owners"
            / (owner_name + ".gucs.cppm");
        if (!sealed_owners.insert(owner_name).second
            || expected == result.owner_content_sha256.end()
            || source_module.empty()
            || string_field(
                   *owner,
                   "generated_module",
                   "SUPPORT GUC owner seal")
                != expected_generated_module
            || std::filesystem::is_symlink(owner_path)
            || !std::filesystem::is_regular_file(owner_path)
            || sha256_hex(read_file(owner_path))
                != expected->second
            || string_field(
                   *owner,
                   "content_sha256",
                   "SUPPORT GUC owner seal")
                != expected->second) {
            throw std::runtime_error(
                "SUPPORT GUC profile projection owner hash differs "
                "from the package seal");
        }
        owner_source_modules.emplace(
            owner_name, source_module);
    }
    for (const guc_profile_projection_row& row :
         result.registrations) {
        const auto source =
            owner_source_modules.find(row.owner_key);
        if (source == owner_source_modules.end()
            || source->second != row.source_module) {
            throw std::runtime_error(
                "SUPPORT GUC profile registration source module does "
                "not join its owner package");
        }
    }
    return result;
}

}  // namespace detail

[[nodiscard]] inline command_line parse_command_line(
    std::span<std::string_view const> args) {
    if (args.empty() || (args.front() != "generate"
                         && args.front() != "update-golden"
                         && args.front() != "migrate-baseline"
                         && args.front() != "prepare")) {
        throw std::runtime_error(
            "usage: bioseq_codegen sql <prepare|generate|update-golden> "
            "(--source <owner.cppm>... | --source-list <path>) "
            "--profile <context-profile> "
            "--pcm-root <owned-dir> --stamp <path>; prepare additionally "
            "requires --authority <support|core|final|safety>, "
            "--raw-compile-database, --clang-binary, "
            "--clang-scan-deps, --llvm-config, "
            "--compiler-module-inventory, their configure-time "
            "--*-sha256 identities, and "
            "--compile-database-output, --pcm-manifest, and --depfile; "
            "generation requires --stage <support|core|final>, "
            "--compile-database, and --pcm-manifest "
            "[--physical-ledger <path> | "
            "--physical-ledger-output <path>] "
            "[--entity-handles-output <owner-key>=<path>]... "
            "[--row-codecs-output <owner-key>=<path>]... "
            "[--fmgr-entrypoints-output <owner-key>=<path>]... "
            "[--guc-parity-ledger <path> --guc-output-root <path> "
            "--guc-active-profile <production|test_indexes>] "
            "[--require-complete-codegen] "
            "[--require-checked-physical-ledger] "
            "[--extension-name <name>] [--extension-schema <name>] "
            "[--generated-module-prefix <module>] "
            "[--fmgr-boundary-module <module>] "
            "[--fmgr-prelude <bioseq_common|postgresql>]; "
            "legacy import is isolated as "
            "sql migrate-baseline --input <legacy.sql> "
            "--migration-owner <owner.cppm> ...");
    }
    const bool migrate_baseline = args.front() == "migrate-baseline";
    const bool prepare = args.front() == "prepare";
    command_line result;
    if (prepare) {
        result.action = command_action::prepare;
    } else if (migrate_baseline) {
        result.action = command_action::migrate_baseline;
    } else if (args.front() == "update-golden") {
        result.action = command_action::update_golden;
        result.update_golden = true;
    } else {
        result.action = command_action::generate;
    }
    std::set<std::string, std::less<>> singleton_options;
    for (std::size_t index = 1; index < args.size(); ++index) {
        const std::string_view option = args[index];
        if (option == "--require-complete-codegen") {
            if (!singleton_options.emplace(option).second) {
                throw std::runtime_error(
                    "duplicate SQL generator singleton option: "
                    + std::string{option});
            }
            result.require_complete_codegen = true;
            continue;
        }
        if (option == "--require-checked-physical-ledger") {
            if (!singleton_options.emplace(option).second) {
                throw std::runtime_error(
                    "duplicate SQL generator singleton option: "
                    + std::string{option});
            }
            result.require_checked_physical_ledger = true;
            continue;
        }
        const bool repeatable =
            option == "--source"
            || option == "--entity-handles-output"
            || option == "--row-codecs-output"
            || option == "--fmgr-entrypoints-output"
            || option == "--fmgr-owner-module";
        if (!repeatable
            && !singleton_options.emplace(option).second) {
            throw std::runtime_error(
                "duplicate SQL generator singleton option: "
                + std::string{option});
        }
        if (index + 1 >= args.size()) {
            throw std::runtime_error(
                "missing value for " + std::string{option});
        }
        const std::string_view value = args[++index];
        if (option == "--input") {
            result.input = std::filesystem::path{value};
        } else if (option == "--source") {
            result.sources.emplace_back(value);
        } else if (option == "--source-list") {
            result.source_list = std::filesystem::path{value};
        } else if (option == "--authority") {
            result.prepare_authority = std::string{value};
        } else if (option == "--stage") {
            result.stage_specified = true;
            if (value == "support") {
                result.stage = generation_stage::support;
            } else if (value == "core") {
                result.stage = generation_stage::core;
            } else if (value == "final") {
                result.stage = generation_stage::final;
            } else {
                throw std::runtime_error(
                    "--stage must be support, core, or final");
            }
        } else if (option == "--profile") {
            result.compiler_profile = std::string{value};
        } else if (option == "--compile-database") {
            result.compile_database = std::filesystem::path{value};
        } else if (option == "--raw-compile-database") {
            result.raw_compile_database =
                std::filesystem::path{value};
        } else if (option == "--compile-database-output") {
            result.compile_database_output =
                std::filesystem::path{value};
        } else if (option == "--pcm-root") {
            result.pcm_root = std::filesystem::path{value};
        } else if (option == "--pcm-manifest") {
            result.pcm_manifest = std::filesystem::path{value};
        } else if (option == "--depfile") {
            result.depfile = std::filesystem::path{value};
        } else if (option == "--clang-binary") {
            result.clang_binary = std::filesystem::path{value};
        } else if (option == "--clang-scan-deps") {
            result.clang_scan_deps =
                std::filesystem::path{value};
        } else if (option == "--llvm-config") {
            result.llvm_config = std::filesystem::path{value};
        } else if (option == "--compiler-module-inventory") {
            result.compiler_module_inventory =
                std::filesystem::path{value};
        } else if (option == "--clang-sha256") {
            result.clang_sha256 = value;
        } else if (option == "--clang-scan-deps-sha256") {
            result.clang_scan_deps_sha256 = value;
        } else if (option == "--llvm-config-sha256") {
            result.llvm_config_sha256 = value;
        } else if (option
                   == "--compiler-module-inventory-sha256") {
            result.compiler_module_inventory_sha256 = value;
        } else if (option == "--migration-owner") {
            result.migration_owner = std::filesystem::path{value};
        } else if (option == "--output") {
            result.output = std::filesystem::path{value};
        } else if (option == "--manifest") {
            result.manifest = std::filesystem::path{value};
        } else if (option == "--golden") {
            result.golden = std::filesystem::path{value};
        } else if (option == "--stamp") {
            result.stamp = std::filesystem::path{value};
        } else if (option == "--physical-ledger") {
            result.physical_ledger = std::filesystem::path{value};
        } else if (option == "--physical-ledger-output") {
            result.physical_ledger_output =
                std::filesystem::path{value};
        } else if (option == "--entity-handles-output") {
            result.entity_handle_outputs.push_back(
                detail::parse_entity_handle_output(value));
        } else if (option == "--row-codecs-output") {
            result.row_codec_outputs.push_back(
                detail::parse_row_codec_output(value));
        } else if (option == "--fmgr-entrypoints-output") {
            result.fmgr_entrypoint_outputs.push_back(
                detail::parse_fmgr_entrypoint_output(value));
        } else if (option == "--fmgr-owner-module") {
            result.fmgr_owners.push_back(
                detail::parse_fmgr_owner(value));
        } else if (option == "--guc-parity-ledger") {
            result.guc_parity_ledger = std::filesystem::path{value};
        } else if (option == "--guc-output-root") {
            result.guc_output_root = std::filesystem::path{value};
        } else if (option == "--core-hash-output") {
            result.core_hash_output = std::filesystem::path{value};
        } else if (option == "--core-manifest") {
            result.core_manifest = std::filesystem::path{value};
        } else if (option == "--support-manifest") {
            result.support_manifest = std::filesystem::path{value};
        } else if (option == "--support-seal") {
            result.support_seal = std::filesystem::path{value};
        } else if (option == "--production-support-manifest") {
            result.production_support_manifest =
                std::filesystem::path{value};
        } else if (option == "--production-support-seal") {
            result.production_support_seal =
                std::filesystem::path{value};
        } else if (
            option == "--test-indexes-support-manifest") {
            result.test_indexes_support_manifest =
                std::filesystem::path{value};
        } else if (option == "--test-indexes-support-seal") {
            result.test_indexes_support_seal =
                std::filesystem::path{value};
        } else if (option == "--expected-core-hash") {
            result.expected_core_hash = std::filesystem::path{value};
        } else if (option == "--guc-active-profile") {
            result.guc_active_profile = std::string{value};
        } else if (option == "--extension-name") {
            result.extension_name = std::string{value};
        } else if (option == "--extension-schema") {
            result.extension_schema = std::string{value};
        } else if (option == "--generated-module-prefix") {
            result.generated_module_prefix = std::string{value};
        } else if (option == "--fmgr-boundary-module") {
            result.fmgr_boundary_module = std::string{value};
        } else if (option == "--fmgr-prelude") {
            result.fmgr_prelude = std::string{value};
        } else {
            throw std::runtime_error(
                "unknown SQL generator option: " + std::string{option});
        }
    }
    const bool has_legacy_input = !result.input.empty();
    const bool has_source_input =
        !result.sources.empty() || !result.source_list.empty();
    if (migrate_baseline
        && (!has_legacy_input || has_source_input
            || result.migration_owner.empty()
            || !result.entity_handle_outputs.empty()
            || !result.row_codec_outputs.empty()
            || !result.fmgr_entrypoint_outputs.empty()
            || !result.fmgr_owners.empty()
            || !result.physical_ledger.empty()
            || !result.physical_ledger_output.empty()
            || !result.guc_parity_ledger.empty()
            || !result.guc_output_root.empty()
            || !result.core_manifest.empty()
            || !result.support_seal.empty()
            || !result.production_support_manifest.empty()
            || !result.production_support_seal.empty()
            || !result.test_indexes_support_manifest.empty()
            || !result.test_indexes_support_seal.empty()
            || !result.clang_binary.empty()
            || !result.clang_scan_deps.empty()
            || !result.llvm_config.empty()
            || !result.compiler_module_inventory.empty()
            || !result.clang_sha256.empty()
            || !result.clang_scan_deps_sha256.empty()
            || !result.llvm_config_sha256.empty()
            || !result.compiler_module_inventory_sha256.empty()
            || !result.pcm_manifest.empty()
            || !result.pcm_root.empty()
            || result.require_complete_codegen
            || result.require_checked_physical_ledger)) {
        throw std::runtime_error(
            "migrate-baseline requires --input and --migration-owner and "
            "does not accept annotated sources or generated C++ outputs");
    }
    if (!migrate_baseline
        && (has_legacy_input || !has_source_input)) {
        throw std::runtime_error(
            "prepare/generate/update-golden are source-only and require "
            "--source/--source-list; --input is accepted only by the "
            "explicit migrate-baseline command");
    }
    const auto valid_context_token = [](std::string_view value) {
        return !value.empty()
            && value.front() >= 'a' && value.front() <= 'z'
            && std::ranges::all_of(
                value.substr(1),
                [](char item) {
                    return (item >= 'a' && item <= 'z')
                        || (item >= '0' && item <= '9')
                        || item == '_';
                });
    };
    if (!migrate_baseline
        && !valid_context_token(result.compiler_profile)) {
        throw std::runtime_error(
            "--profile must be a lowercase context profile identifier");
    }
    if (result.fmgr_prelude != "bioseq_common"
        && result.fmgr_prelude != "postgresql") {
        throw std::runtime_error(
            "--fmgr-prelude must be bioseq_common or postgresql");
    }
    if (!migrate_baseline) {
        const auto require_sha256 =
            [](std::string_view value, std::string_view option) {
                if (!value.empty() && !detail::is_sha256_hex(value)) {
                    throw std::runtime_error(
                        std::string{option}
                        + " must be exactly 64 lowercase hexadecimal "
                          "characters");
                }
            };
        require_sha256(result.clang_sha256, "--clang-sha256");
        require_sha256(
            result.clang_scan_deps_sha256,
            "--clang-scan-deps-sha256");
        require_sha256(
            result.llvm_config_sha256,
            "--llvm-config-sha256");
        require_sha256(
            result.compiler_module_inventory_sha256,
            "--compiler-module-inventory-sha256");
    }
    if (prepare) {
        if (result.stage_specified) {
            throw std::runtime_error(
                "prepare derives its stage from --authority and does "
                "not accept --stage");
        }
        if (!result.stage_specified
            && result.prepare_authority == "support") {
            result.stage = generation_stage::support;
        } else if (!result.stage_specified
                   && result.prepare_authority == "core") {
            result.stage = generation_stage::core;
        } else if (!result.stage_specified
                   && result.prepare_authority == "final") {
            result.stage = generation_stage::final;
        } else if (!result.stage_specified
                   && result.prepare_authority == "safety") {
            result.stage = generation_stage::final;
        } else if (result.prepare_authority != "support"
                   && result.prepare_authority != "core"
                   && result.prepare_authority != "final"
                   && result.prepare_authority != "safety") {
            throw std::runtime_error(
                "--authority must be support, core, final, or safety");
        }
        if (result.raw_compile_database.empty()
            || result.clang_binary.empty()
            || result.clang_scan_deps.empty()
            || result.compiler_module_inventory.empty()
            || result.llvm_config.empty()
            || result.clang_sha256.empty()
            || result.clang_scan_deps_sha256.empty()
            || result.llvm_config_sha256.empty()
            || result.compiler_module_inventory_sha256.empty()
            || result.compile_database_output.empty()
            || result.pcm_root.empty()
            || result.pcm_manifest.empty()
            || result.depfile.empty()
            || result.stamp.empty()) {
            throw std::runtime_error(
                "prepare requires --raw-compile-database, "
                "--clang-binary, --compile-database-output, "
                "--clang-scan-deps, --llvm-config, "
                "--compiler-module-inventory, "
                "their four --*-sha256 identities, "
                "--pcm-root, --pcm-manifest, --depfile, and --stamp");
        }
        if (!result.compile_database.empty()
            || !result.output.empty() || !result.manifest.empty()
            || !result.golden.empty()
            || !result.physical_ledger.empty()
            || !result.physical_ledger_output.empty()
            || !result.guc_parity_ledger.empty()
            || !result.guc_output_root.empty()
            || !result.core_hash_output.empty()
            || !result.core_manifest.empty()
            || !result.support_manifest.empty()
            || !result.support_seal.empty()
            || !result.production_support_manifest.empty()
            || !result.production_support_seal.empty()
            || !result.test_indexes_support_manifest.empty()
            || !result.test_indexes_support_seal.empty()
            || !result.expected_core_hash.empty()
            || !result.entity_handle_outputs.empty()
            || !result.row_codec_outputs.empty()
            || !result.fmgr_entrypoint_outputs.empty()
            || result.require_complete_codegen
            || result.require_checked_physical_ledger) {
            throw std::runtime_error(
                "prepare does not accept generation output options");
        }
        result.mode = authority_mode::annotated_cpp;
        detail::validate_artifact_path_aliases(result);
        return result;
    }
    if (!migrate_baseline
        && (!result.stage_specified
            || result.compile_database.empty()
            || result.pcm_root.empty()
            || result.pcm_manifest.empty()
            || result.clang_binary.empty()
            || result.clang_scan_deps.empty()
            || result.llvm_config.empty()
            || result.compiler_module_inventory.empty()
            || result.clang_sha256.empty()
            || result.clang_scan_deps_sha256.empty()
            || result.llvm_config_sha256.empty()
            || result.compiler_module_inventory_sha256.empty())) {
        throw std::runtime_error(
            "generate/update-golden require --stage, "
            "--compile-database, --pcm-root, --pcm-manifest, "
            "--clang-binary, --clang-scan-deps, --llvm-config, and "
            "--compiler-module-inventory plus their four "
            "--*-sha256 identities");
    }
    if (!migrate_baseline
        && !result.guc_parity_ledger.empty()
        && result.guc_active_profile != result.compiler_profile) {
        throw std::runtime_error(
            "--guc-active-profile must match --profile");
    }
    result.mode = migrate_baseline
        ? authority_mode::legacy_baseline
        : authority_mode::annotated_cpp;
    if (result.stamp.empty()) {
        throw std::runtime_error(
            "SQL generation requires --stamp");
    }
    if (!migrate_baseline
        && result.stage == generation_stage::support
        && (result.manifest.empty()
            || result.row_codec_outputs.empty()
            || result.fmgr_entrypoint_outputs.empty()
            || result.guc_parity_ledger.empty()
            || result.guc_output_root.empty())) {
        throw std::runtime_error(
            "support generation requires --manifest, "
            "--row-codecs-output, SUPPORT registry outputs, "
            "--guc-parity-ledger, and --guc-output-root");
    }
    if (!migrate_baseline
        && result.stage == generation_stage::core
        && (result.core_hash_output.empty()
            || result.core_manifest.empty()
            || result.entity_handle_outputs.empty())) {
        throw std::runtime_error(
            "core generation requires --core-hash-output, "
            "--core-manifest, and --entity-handles-output");
    }
    if (!migrate_baseline
        && result.stage == generation_stage::final
        && (result.output.empty() || result.manifest.empty())) {
        throw std::runtime_error(
            "final generation requires --output and --manifest; "
            "CORE, SUPPORT, row-codec, GUC, and fmgr artifacts are "
            "required only when selected by registered owners");
    }
    if (!migrate_baseline
        && result.stage == generation_stage::final) {
        const bool any_core =
            !result.expected_core_hash.empty()
            || !result.core_manifest.empty()
            || !result.entity_handle_outputs.empty();
        const bool complete_core =
            !result.expected_core_hash.empty()
            && !result.core_manifest.empty()
            && !result.entity_handle_outputs.empty();
        if (any_core != complete_core) {
            throw std::runtime_error(
                "FINAL CORE handoff requires expected seal, manifest, and "
                "entity-handle outputs together");
        }
        if (result.support_manifest.empty()
            != result.support_seal.empty()) {
            throw std::runtime_error(
                "FINAL active SUPPORT manifest and seal must be supplied "
                "together");
        }
        const bool any_profile_support =
            !result.production_support_manifest.empty()
            || !result.production_support_seal.empty()
            || !result.test_indexes_support_manifest.empty()
            || !result.test_indexes_support_seal.empty();
        const bool complete_profile_support =
            !result.production_support_manifest.empty()
            && !result.production_support_seal.empty()
            && !result.test_indexes_support_manifest.empty()
            && !result.test_indexes_support_seal.empty();
        if (any_profile_support != complete_profile_support) {
            throw std::runtime_error(
                "FINAL cross-profile SUPPORT handoff requires both "
                "production and test_indexes manifest/seal pairs");
        }
        if (!result.guc_parity_ledger.empty()
            && (result.support_manifest.empty()
                || !complete_profile_support)) {
            throw std::runtime_error(
                "FINAL GUC generation requires active and cross-profile "
                "SUPPORT handoffs");
        }
    }
    if (!migrate_baseline
        && result.stage == generation_stage::support
        && (!result.output.empty() || !result.golden.empty()
            || !result.core_hash_output.empty()
            || !result.core_manifest.empty()
            || !result.expected_core_hash.empty()
            || !result.support_manifest.empty()
            || !result.support_seal.empty()
            || !result.production_support_manifest.empty()
            || !result.production_support_seal.empty()
            || !result.test_indexes_support_manifest.empty()
            || !result.test_indexes_support_seal.empty()
            || !result.entity_handle_outputs.empty()
            || !result.physical_ledger.empty()
            || !result.physical_ledger_output.empty()
            || result.require_complete_codegen
            || result.require_checked_physical_ledger)) {
        throw std::runtime_error(
            "support generation accepts only SUPPORT-owned outputs");
    }
    if (!migrate_baseline
        && result.stage == generation_stage::support) {
        for (const configured_fmgr_entrypoint_output& output :
             result.fmgr_entrypoint_outputs) {
            if (!detail::support_owned_fmgr_output(
                    output.owner_key)) {
                throw std::runtime_error(
                    "SUPPORT generation cannot own FINAL fmgr output '"
                    + output.owner_key + "'");
            }
        }
    }
    if (!migrate_baseline
        && result.stage == generation_stage::core
        && (!result.output.empty() || !result.manifest.empty()
            || !result.golden.empty()
            || !result.support_manifest.empty()
            || !result.support_seal.empty()
            || !result.production_support_manifest.empty()
            || !result.production_support_seal.empty()
            || !result.test_indexes_support_manifest.empty()
            || !result.test_indexes_support_seal.empty()
            || !result.expected_core_hash.empty()
            || !result.row_codec_outputs.empty()
            || !result.fmgr_entrypoint_outputs.empty()
            || !result.guc_parity_ledger.empty()
            || !result.guc_output_root.empty()
            || !result.physical_ledger.empty()
            || !result.physical_ledger_output.empty()
            || result.require_complete_codegen
            || result.require_checked_physical_ledger)) {
        throw std::runtime_error(
            "core generation accepts only CORE-owned outputs");
    }
    if (!migrate_baseline
        && result.stage == generation_stage::final
        && !result.core_hash_output.empty()) {
        throw std::runtime_error(
            "final generation verifies but does not publish CORE outputs");
    }
    if (migrate_baseline
        && (result.output.empty() || result.manifest.empty())) {
        throw std::runtime_error(
            "migrate-baseline requires --output and --manifest");
    }
    if (result.update_golden && result.golden.empty()) {
        throw std::runtime_error(
            "update-golden is an explicit maintainer command and requires "
            "--golden");
    }
    if (result.update_golden
        && (result.stage != generation_stage::final
            || result.compiler_profile != "production")) {
        throw std::runtime_error(
            "update-golden is restricted to the production FINAL "
            "authority");
    }
    if (!migrate_baseline
        && (result.guc_parity_ledger.empty()
            != result.guc_output_root.empty())) {
        throw std::runtime_error(
            "--guc-parity-ledger and --guc-output-root must be supplied "
            "together");
    }
    if (!result.physical_ledger.empty()
        && !result.physical_ledger_output.empty()) {
        throw std::runtime_error(
            "--physical-ledger and --physical-ledger-output are "
            "mutually exclusive");
    }
    if (result.guc_active_profile != "production"
        && result.guc_active_profile != "test_indexes") {
        throw std::runtime_error(
            "--guc-active-profile must be production or test_indexes");
    }
    validate_final_physical_ledger_policy(result);
    detail::validate_artifact_path_aliases(result);
    return result;
}

[[nodiscard]] inline int self_test() {
    try {
        bioseq::codegen::clang_structural::
            run_preparation_contract_self_tests();
        detail::run_artifact_path_alias_contract_self_test();
        run_generator_contract_self_tests();
        run_guc_generator_contract_self_tests();
        bioseq::codegen::safety::run_safety_contract_self_tests();
        return 0;
    } catch (const std::exception& error) {
        std::println(stderr, "bioseq_sql_codegen: {}", error.what());
        return 1;
    }
}

[[nodiscard]] inline int run(std::span<std::string_view const> args) {
    try {
        command_line command = parse_command_line(args);
        if (command.mode == authority_mode::legacy_baseline) {
            const std::string source = read_file(command.input);
            install_script script = parse_install_script(
                source, command.extension_name);
            validate_install_script(script);
            const std::string rendered =
                render_install_script(script);
            const std::string manifest =
                render_manifest(script, rendered);
            const std::string migration_owner =
                render_migration_owner(script);
            write_if_different(
                command.migration_owner,
                migration_owner);
            const std::string publication_material =
                "bioseq_sql_migration_closure/v1\n"
                "legacy_input_sha256=" + sha256_hex(source)
                + "\nmigration_owner_sha256="
                + sha256_hex(migration_owner) + "\n";
            publish_artifacts(
                command,
                rendered,
                manifest,
                publication_material);
            return 0;
        }

        if (!command.source_list.empty()) {
            std::vector<std::filesystem::path> listed =
                detail::read_source_list(command.source_list);
            command.sources.insert(
                command.sources.end(),
                listed.begin(),
                listed.end());
        }
        detail::normalize_sources(command.sources);
        detail::validate_artifact_path_aliases(command);

        if (command.action == command_action::prepare) {
            clang_structural::prepared_compilation_environment
                prepared =
                    clang_structural::prepare_compilation_environment(
                        command.sources,
                        command.raw_compile_database,
                        command.clang_binary,
                        command.clang_scan_deps,
                        command.llvm_config,
                        command.compiler_module_inventory,
                        command.clang_sha256,
                        command.clang_scan_deps_sha256,
                        command.llvm_config_sha256,
                        command.compiler_module_inventory_sha256,
                        command.pcm_root,
                        command.compiler_profile,
                        command.prepare_authority);
            if (!command.source_list.empty()) {
                prepared.dependencies.push_back(
                    command.source_list);
            }
            write_if_different(
                command.compile_database_output,
                prepared.compile_database_json);
            write_if_different(
                command.pcm_manifest,
                prepared.pcm_manifest_json);
            write_if_different(
                command.depfile,
                detail::render_depfile(
                    command.stamp,
                    std::move(prepared.dependencies)));
            write_if_different(
                command.stamp,
                "bioseq_clang_prepare_stamp/v1\n"
                "authority=" + command.prepare_authority + "\n"
                "profile=" + command.compiler_profile + "\n"
                "compilation_signature="
                + prepared.compilation_signature + "\n"
                "relocatable_compilation_signature="
                + prepared.relocatable_compilation_signature + "\n"
                "pcm_manifest_sha256="
                + sha256_hex(prepared.pcm_manifest_json) + "\n"
                "compile_database_sha256="
                + sha256_hex(prepared.compile_database_json) + "\n"
                "relocatable_pcm_manifest_projection_sha256="
                + prepared.relocatable_pcm_manifest_sha256 + "\n"
                "relocatable_compile_database_sha256="
                + prepared.relocatable_compile_database_sha256 + "\n");
            touch_stamp(command.stamp);
            return 0;
        }

        const std::string prepared_authority =
            command.stage == generation_stage::support
            ? "support"
            : command.stage == generation_stage::core
            ? "core"
            : "final";
        const clang_structural::prepared_compilation_validation
            prepared_validation =
                clang_structural::
                    validate_prepared_compilation_environment(
                        command.sources,
                        command.compile_database,
                        command.pcm_root,
                        command.pcm_manifest,
                        command.clang_binary,
                        command.clang_scan_deps,
                        command.llvm_config,
                        command.compiler_module_inventory,
                        command.clang_sha256,
                        command.clang_scan_deps_sha256,
                        command.llvm_config_sha256,
                        command.compiler_module_inventory_sha256,
                        command.compiler_profile,
                        prepared_authority);
        collected_program program = collect_sources(
            command.sources,
            source_collection_environment{
                .compile_database = command.compile_database,
                .pcm_root = command.pcm_root,
                .compiler_profile = command.compiler_profile,
            });
        program.compilation_signature = sha256_hex(
            "bioseq_sql_relocatable_prepared_closure/v1\n"
            + prepared_validation.relocatable_compilation_signature
            + "\n");
        const bool has_guc_annotations = std::ranges::any_of(
            program.entities,
            [](const collected_entity& entity) {
                return entity.kind == entity_kind::guc;
            });
        if (has_guc_annotations
            != !command.guc_parity_ledger.empty()) {
            throw std::runtime_error(
                has_guc_annotations
                ? "annotated GUC owners require configured parity-ledger "
                  "and generated-module outputs"
                : "GUC generation was configured but the source closure "
                  "contains no annotated GUC owner");
        }

        std::optional<guc_inventory> active_gucs;
        std::optional<rendered_guc_package> active_guc_package;
        if (!command.guc_parity_ledger.empty()) {
            const guc_profile profile =
                command.compiler_profile == "test_indexes"
                ? guc_profile::test_indexes
                : guc_profile::production;
            active_gucs.emplace(collect_guc_inventory(
                program, command.guc_parity_ledger, profile));
            active_guc_package.emplace(
                render_guc_package(
                    *active_gucs,
                    command.generated_module_prefix,
                    command.fmgr_boundary_module));
            detail::validate_artifact_path_aliases(
                command,
                detail::guc_derived_outputs(
                    command, *active_guc_package));
        }
        const std::string compilation_signature =
            program.compilation_signature;
        const std::string clang_version =
            program.module_graph.clang_version;
        sql_graph graph = build_sql_graph(
            std::move(program),
            command.extension_schema,
            command.extension_name,
            command.generated_module_prefix,
            command.fmgr_boundary_module,
            command.fmgr_prelude);

        if (command.stage == generation_stage::support) {
            if (!active_guc_package) {
                throw std::runtime_error(
                    "SUPPORT generation has no active GUC package");
            }
            const std::vector<rendered_row_codec_module>
                row_codec_modules =
                    render_row_codec_modules(graph);
            const std::vector<rendered_fmgr_entrypoint_module>
                vector_registry_modules =
                    render_support_vector_registry_modules(
                        command.compiler_profile);
            const std::string support_manifest =
                detail::render_support_manifest(
                    command.compiler_profile,
                    clang_version,
                    compilation_signature,
                    row_codec_modules,
                    *active_gucs,
                    *active_guc_package,
                    vector_registry_modules);
            const std::string support_seal =
                detail::render_support_seal(
                    command.compiler_profile,
                    compilation_signature,
                    detail::support_descriptor_hash(
                        command.compiler_profile,
                        row_codec_modules,
                        *active_guc_package,
                        vector_registry_modules),
                    support_manifest);
            detail::publish_row_codec_modules(
                command, row_codec_modules);
            detail::publish_guc_package(
                command, *active_guc_package);
            detail::publish_fmgr_entrypoint_modules(
                command, vector_registry_modules, true);
            write_if_different(
                command.manifest, support_manifest);
            write_if_different(
                command.stamp, support_seal);
            touch_stamp(command.stamp);
            return 0;
        }

        std::vector<rendered_entity_handle_module>
            entity_handle_modules;
        if (command.stage == generation_stage::core
            || !command.expected_core_hash.empty()) {
            entity_handle_modules =
                render_entity_handle_modules(graph);
        }
        const std::string core_hash =
            detail::core_descriptor_hash(entity_handle_modules);
        if (command.stage == generation_stage::core) {
            const std::string core_manifest =
                detail::render_core_manifest(
                    clang_version,
                    compilation_signature,
                    command,
                    entity_handle_modules);
            const std::string core_seal =
                detail::render_core_seal(
                    core_hash,
                    compilation_signature,
                    core_manifest);
            detail::publish_entity_handle_modules(
                command, entity_handle_modules);
            write_if_different(
                command.core_manifest, core_manifest);
            write_if_different(
                command.core_hash_output, core_seal);
            write_if_different(
                command.stamp,
                "bioseq_sql_core_stamp/v1\n"
                "profile=production\n"
                "clang_version=" + clang_version + "\n"
                "compilation_signature="
                + compilation_signature + "\n"
                "core_descriptor_sha256=" + core_hash + "\n"
                "core_manifest_sha256="
                + sha256_hex(core_manifest) + "\n"
                "core_seal_sha256="
                + sha256_hex(core_seal) + "\n");
            touch_stamp(command.stamp);
            return 0;
        }

        if (!command.expected_core_hash.empty()) {
            detail::verify_core_artifacts(
                command,
                clang_version,
                entity_handle_modules);
        }
        std::vector<rendered_row_codec_module>
            row_codec_modules;
        if (!command.row_codec_outputs.empty()) {
            row_codec_modules =
                render_row_codec_modules(graph);
        }
        const std::vector<rendered_fmgr_entrypoint_module>
            fmgr_entrypoint_modules =
                render_fmgr_entrypoint_modules(
                    graph, command.fmgr_owners);
        std::vector<rendered_fmgr_entrypoint_module>
            vector_registry_modules;
        for (const rendered_fmgr_entrypoint_module& module :
             fmgr_entrypoint_modules) {
            if (detail::support_owned_fmgr_output(
                    module.owner_key)) {
                vector_registry_modules.push_back(module);
            }
        }
        if (!command.row_codec_outputs.empty()) {
            detail::verify_row_codec_modules(
                command, row_codec_modules);
        }
        std::optional<guc_profile_delta_evidence>
            guc_delta_evidence;
        if (active_gucs && active_guc_package) {
            detail::verify_guc_package(
                command, *active_guc_package);
            detail::verify_support_fmgr_entrypoint_modules(
                command, vector_registry_modules);
            detail::verify_support_manifest(
                command,
                clang_version,
                row_codec_modules,
                *active_gucs,
                *active_guc_package,
                vector_registry_modules);
            const guc_profile_projection production_guc_projection =
                detail::load_sealed_guc_profile_projection(
                    command.production_support_manifest,
                    command.production_support_seal,
                    command.guc_output_root,
                    guc_profile::production,
                    clang_version);
            const guc_profile_projection test_indexes_guc_projection =
                detail::load_sealed_guc_profile_projection(
                    command.test_indexes_support_manifest,
                    command.test_indexes_support_seal,
                    command.guc_output_root,
                    guc_profile::test_indexes,
                    clang_version);
            const guc_profile_projection active_guc_projection =
                make_guc_profile_projection(
                    *active_gucs, *active_guc_package);
            if (active_guc_projection
                != (active_guc_projection.profile
                            == guc_profile::production
                        ? production_guc_projection
                        : test_indexes_guc_projection)) {
                throw std::runtime_error(
                    "active FINAL GUC projection differs from its sealed "
                    "SUPPORT handoff");
            }
            guc_delta_evidence.emplace(
                make_guc_profile_delta_evidence(
                production_guc_projection,
                test_indexes_guc_projection));
        }

        physical_ledger_material ledger_material =
            make_physical_ledger_material({});
        if (!command.physical_ledger.empty()) {
            ledger_material = apply_checked_physical_ledger(
                graph,
                command.physical_ledger);
        }
        validate_codegen_completion_policy(
            graph, command.require_complete_codegen);
        if (!command.physical_ledger_output.empty()) {
            ledger_material = make_physical_ledger_material(
                render_physical_ledger(graph));
        }
        const rendered_sql_artifact artifact =
            render_sql_graph(graph);
        std::string manifest = render_graph_manifest(
            graph,
            artifact,
            fmgr_entrypoint_modules,
            command.extension_name == "bioseq");
        if (active_gucs && active_guc_package) {
            manifest = merge_guc_manifest(
                manifest,
                *active_gucs,
                *active_guc_package,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                &*guc_delta_evidence);
        }
        detail::publish_fmgr_entrypoint_modules(
            command, fmgr_entrypoint_modules, false);
        if (!command.physical_ledger_output.empty()) {
            write_if_different(
                command.physical_ledger_output,
                ledger_material.exact_bytes);
        }
        std::vector<std::string> fmgr_seals;
        fmgr_seals.reserve(fmgr_entrypoint_modules.size());
        for (const rendered_fmgr_entrypoint_module& module :
             fmgr_entrypoint_modules) {
            fmgr_seals.push_back(
                module.owner_key + "\t" + module.module_name + "\t"
                + sha256_hex(module.content));
        }
        std::ranges::sort(fmgr_seals);
        std::string publication_material =
            "bioseq_sql_final_closure/v1\n"
            "clang_version=" + clang_version
            + "\ncompilation_signature=" + compilation_signature
            + "\nrelocatable_prepare_signature="
            + prepared_validation.relocatable_compilation_signature;
        if (command.extension_name != "bioseq"
            || command.generated_module_prefix
                != "bioseq.sql.generated"
            || command.fmgr_boundary_module
                != "bioseq.pg_utils.sql_boundary"
            || command.fmgr_prelude != "bioseq_common") {
            publication_material +=
                "\nextension_name=" + command.extension_name
                + "\nextension_schema=" + command.extension_schema
                + "\ngenerated_module_prefix="
                + command.generated_module_prefix
                + "\nfmgr_boundary_module="
                + command.fmgr_boundary_module
                + "\nfmgr_prelude=" + command.fmgr_prelude;
        }
        publication_material +=
            "\nphysical_ledger_sha256="
            + ledger_material.sha256 + "\n";
        const auto append_artifact_hash =
            [&publication_material](
                std::string_view label,
                const std::filesystem::path& path) {
                if (path.empty()) return;
                publication_material += std::string{label} + "="
                    + sha256_hex(read_file(path)) + "\n";
            };
        append_artifact_hash(
            "support_manifest_sha256", command.support_manifest);
        append_artifact_hash(
            "support_seal_sha256", command.support_seal);
        append_artifact_hash(
            "production_support_manifest_sha256",
            command.production_support_manifest);
        append_artifact_hash(
            "production_support_seal_sha256",
            command.production_support_seal);
        append_artifact_hash(
            "test_indexes_support_manifest_sha256",
            command.test_indexes_support_manifest);
        append_artifact_hash(
            "test_indexes_support_seal_sha256",
            command.test_indexes_support_seal);
        append_artifact_hash(
            "core_manifest_sha256", command.core_manifest);
        append_artifact_hash(
            "core_seal_sha256", command.expected_core_hash);
        for (const std::string& seal : fmgr_seals) {
            publication_material +=
                "fmgr=" + seal + "\n";
        }
        publish_artifacts(
            command,
            artifact.sql,
            manifest,
            publication_material);
        return 0;
    } catch (const std::exception& error) {
        std::println(stderr, "bioseq_sql_codegen: {}", error.what());
        return 1;
    }
}

}  // namespace bioseq::codegen::sql
