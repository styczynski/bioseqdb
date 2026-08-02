module;

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <print>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.highway;

import :manifest;
import :model;
import :renderer;
import :validator;
import :contract_tests;
import bioseq.codegen.clang_structural;
import bioseq.codegen.support;

export namespace bioseq::codegen::highway {

namespace detail {

[[nodiscard]] inline std::map<std::string, std::string, std::less<>>
parse_options(
    std::span<std::string_view const> args,
    const std::set<std::string_view, std::less<>>& allowed) {
    std::map<std::string, std::string, std::less<>> result;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string_view option = args[index];
        if (!allowed.contains(option)) {
            throw std::runtime_error(
                "unknown Highway generator option '"
                + std::string{option} + "'");
        }
        if (index + 1 >= args.size()) {
            throw std::runtime_error(
                "missing value for Highway generator option '"
                + std::string{option} + "'");
        }
        const std::string_view value = args[++index];
        if (value.empty() || !result.emplace(option, value).second) {
            throw std::runtime_error(
                "Highway generator option '" + std::string{option}
                + "' must occur once with a non-empty value");
        }
    }
    if (result.size() != allowed.size()) {
        std::string missing;
        for (std::string_view option : allowed) {
            if (!result.contains(option)) {
                if (!missing.empty()) missing += ", ";
                missing += option;
            }
        }
        throw std::runtime_error(
            "missing required Highway generator option(s): " + missing);
    }
    return result;
}

[[nodiscard]] inline command_line parse_generate_command_line(
    std::span<std::string_view const> args) {
    static const std::set<std::string_view, std::less<>> allowed{
        "--artifact-manifest",
        "--clang-binary",
        "--clang-scan-deps",
        "--clang-scan-deps-sha256",
        "--clang-sha256",
        "--compile-database",
        "--compiler-module-inventory",
        "--compiler-module-inventory-sha256",
        "--llvm-config",
        "--llvm-config-sha256",
        "--output-root",
        "--pcm-manifest",
        "--pcm-root",
        "--source-manifest",
        "--stamp",
        "--target-manifest",
    };
    const auto options = parse_options(args, allowed);
    return command_line{
        .target_manifest_path = options.at("--target-manifest"),
        .source_manifest_path = options.at("--source-manifest"),
        .compile_database = options.at("--compile-database"),
        .pcm_manifest = options.at("--pcm-manifest"),
        .pcm_root = options.at("--pcm-root"),
        .clang_binary = options.at("--clang-binary"),
        .clang_scan_deps = options.at("--clang-scan-deps"),
        .llvm_config = options.at("--llvm-config"),
        .compiler_module_inventory =
            options.at("--compiler-module-inventory"),
        .clang_sha256 = options.at("--clang-sha256"),
        .clang_scan_deps_sha256 = options.at("--clang-scan-deps-sha256"),
        .llvm_config_sha256 = options.at("--llvm-config-sha256"),
        .compiler_module_inventory_sha256 =
            options.at("--compiler-module-inventory-sha256"),
        .output_root = options.at("--output-root"),
        .artifact_manifest = options.at("--artifact-manifest"),
        .stamp = options.at("--stamp"),
    };
}

[[nodiscard]] inline prepare_command_line parse_prepare_command_line(
    std::span<std::string_view const> args) {
    static const std::set<std::string_view, std::less<>> allowed{
        "--clang-binary",
        "--clang-scan-deps",
        "--clang-scan-deps-sha256",
        "--clang-sha256",
        "--compile-database-output",
        "--compiler-module-inventory",
        "--compiler-module-inventory-sha256",
        "--depfile",
        "--llvm-config",
        "--llvm-config-sha256",
        "--pcm-manifest",
        "--pcm-root",
        "--raw-compile-database",
        "--source-manifest",
        "--stamp",
    };
    const auto options = parse_options(args, allowed);
    return prepare_command_line{
        .source_manifest_path = options.at("--source-manifest"),
        .raw_compile_database = options.at("--raw-compile-database"),
        .clang_binary = options.at("--clang-binary"),
        .clang_scan_deps = options.at("--clang-scan-deps"),
        .llvm_config = options.at("--llvm-config"),
        .compiler_module_inventory =
            options.at("--compiler-module-inventory"),
        .clang_sha256 = options.at("--clang-sha256"),
        .clang_scan_deps_sha256 = options.at("--clang-scan-deps-sha256"),
        .llvm_config_sha256 = options.at("--llvm-config-sha256"),
        .compiler_module_inventory_sha256 =
            options.at("--compiler-module-inventory-sha256"),
        .pcm_root = options.at("--pcm-root"),
        .compile_database_output = options.at("--compile-database-output"),
        .pcm_manifest = options.at("--pcm-manifest"),
        .depfile = options.at("--depfile"),
        .stamp = options.at("--stamp"),
    };
}

[[nodiscard]] inline std::string depfile_escape(
    const std::filesystem::path& path) {
    std::string result;
    for (const char value : path.generic_string()) {
        if (value == '$') {
            result += "$$";
        } else {
            if (value == ' ' || value == '#' || value == ':' || value == '\\') {
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
    const auto unique = std::ranges::unique(
        dependencies,
        {},
        [](const std::filesystem::path& path) {
            return path.generic_string();
        });
    dependencies.erase(unique.begin(), unique.end());
    std::ostringstream output;
    output << depfile_escape(target) << ':';
    for (const std::filesystem::path& dependency : dependencies) {
        output << ' ' << depfile_escape(dependency);
    }
    output << '\n';
    return output.str();
}

[[nodiscard]] inline std::vector<std::filesystem::path> source_paths_of(
    const source_manifest& sources) {
    std::vector<std::filesystem::path> result;
    result.reserve(sources.sources.size());
    for (const source_spec& source : sources.sources) {
        result.push_back(source.path);
    }
    return result;
}

[[nodiscard]] inline int execute_prepare(prepare_command_line command) {
    validate_prepare_command_line(command);
    const source_manifest sources =
        parse_source_manifest(command.source_manifest_path);
    const std::vector<std::filesystem::path> source_paths =
        source_paths_of(sources);
    std::filesystem::create_directories(command.pcm_root);
    clang_structural::prepared_compilation_environment prepared =
        clang_structural::prepare_compilation_environment(
            source_paths,
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
            "production",
            "core");
    prepared.dependencies.push_back(command.source_manifest_path);
    write_if_different(
        command.compile_database_output, prepared.compile_database_json);
    write_if_different(command.pcm_manifest, prepared.pcm_manifest_json);
    write_if_different(
        command.depfile,
        render_depfile(command.stamp, std::move(prepared.dependencies)));
    write_if_different(
        command.stamp,
        "bioseq_highway_prepare_stamp/v1\n"
        "profile=production\n"
        "authority=core\n"
        "compilation_signature=" + prepared.compilation_signature + "\n"
        "relocatable_compilation_signature="
            + prepared.relocatable_compilation_signature + "\n"
        "compile_database_sha256="
            + sha256_hex(prepared.compile_database_json) + "\n"
        "pcm_manifest_sha256=" + sha256_hex(prepared.pcm_manifest_json) + "\n"
        "relocatable_compile_database_sha256="
            + prepared.relocatable_compile_database_sha256 + "\n"
        "relocatable_pcm_manifest_sha256="
            + prepared.relocatable_pcm_manifest_sha256 + "\n");
    touch_stamp(command.stamp);
    return 0;
}

[[nodiscard]] inline int execute(command_line command) {
    validate_command_line(command);
    target_manifest targets = parse_target_manifest(command.target_manifest_path);
    source_manifest sources = parse_source_manifest(command.source_manifest_path);
    validate_inventories(targets, sources);

    const std::vector<std::filesystem::path> source_paths =
        source_paths_of(sources);
    // The isolated scan database is a production-profile authority even
    // though its template providers are never attached to a final target.
    // Target-policy identity is sealed independently by the target manifest.
    constexpr std::string_view compiler_profile = "production";
    const clang_structural::prepared_compilation_validation prepared =
        clang_structural::validate_prepared_compilation_environment(
            source_paths,
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
            compiler_profile,
            "core");
    const clang_structural::structural_index structural =
        clang_structural::scan_translation_units_from_database(
            source_paths,
            command.compile_database,
            compiler_profile,
            command.pcm_root);
    const clang_structural::module_graph graph =
        clang_structural::scan_module_graph_from_database(
            source_paths,
            command.compile_database,
            compiler_profile,
            command.pcm_root);
    if (structural.clang_version != graph.clang_version
        || structural.compilation_signature != graph.compilation_signature) {
        throw std::runtime_error(
            "Highway structural and module scans used divergent compiler identities");
    }

    generation_plan plan{
        .target_inventory = std::move(targets),
        .source_inventory = std::move(sources),
        .clang_version = structural.clang_version,
        .compilation_signature = sha256_hex(
            "bioseq_highway_relocatable_prepared_closure/v1\n"
            + prepared.relocatable_compilation_signature + "\n"),
    };
    plan.extracted_sources =
        extract_sources(plan.source_inventory, structural, graph);
    validate_slot_callable_ownership(
        plan.source_inventory, plan.extracted_sources);
    generation_result generated = render_generation(plan);
    publish_generation(command, generated);
    return 0;
}

inline void run_contract_self_test() {
    generation_plan plan{
        .target_inventory = target_manifest{
            .policy_digest =
                "e3b0c44298fc1c149afbf4c8996fb924"
                "27ae41e4649b934ca495991b7852b855",
            .highway_version = "self-test",
            .highway_digest =
                "00000000000000000000000000000000"
                "00000000000000000000000000000000",
            .static_target_token = "HWY_EMU128",
            .targets = {
                target_spec{
                    .platform = "x86_64",
                    .slug = "emu128",
                    .token = "HWY_EMU128",
                    .name_space = "N_EMU128",
                    .bit = 1,
                },
            },
        },
        .source_inventory = source_manifest{
            .contract_module = "bioseq.highway.dispatch_contract",
            .dispatch_module = "bioseq.scheduler.dispatch_processor",
        },
        .clang_version = "self-test",
        .compilation_signature = "self-test",
    };
    const generation_result generated = render_generation(plan);
    const auto registry = std::ranges::find_if(
        generated.artifacts,
        [](const generated_artifact& artifact) {
            return artifact.kind
                == artifact_kind::dispatch_registry_partition;
        });
    const auto inventory = std::ranges::find_if(
        generated.artifacts,
        [](const generated_artifact& artifact) {
            return artifact.kind
                == artifact_kind::dispatch_inventory_implementation;
        });
    if (generated.artifacts.size() != 5
        || registry == generated.artifacts.end()
        || inventory == generated.artifacts.end()
        || registry->content.find("HWY_DYNAMIC_DISPATCH(")
            == std::string::npos
        || registry->content.find("registered_highway_dispatch_info")
            != std::string::npos
        || inventory->content.find("registered_highway_dispatch_info")
            == std::string::npos
        || generated.artifact_manifest_json.find(
               "\"path\":\"highway_registry.cppm\"")
            == std::string::npos
        || generated.artifact_manifest_json.find(
               "\"path\":\"dispatch_inventory.cppm\"")
            == std::string::npos
        || generated.artifact_manifest_json.find(
               "\"path\":\"test_support.cppm\"")
            == std::string::npos) {
        throw std::runtime_error(
            "Highway generator self-test failed deterministic topology contract");
    }
}

}  // namespace detail

inline int self_test() {
    try {
        run_highway_contract_self_tests();
        detail::run_contract_self_test();
        return 0;
    } catch (const std::exception& error) {
        std::println(stderr, "bioseq_highway_codegen: {}", error.what());
        return 1;
    }
}

inline int run(std::span<std::string_view const> args) {
    if (args.empty() || (args.front() != "prepare" && args.front() != "generate")) {
        std::println(
            stderr,
            "usage: bioseq_codegen highway <prepare|generate> [options]");
        return 2;
    }
    try {
        if (args.front() == "prepare") {
            return detail::execute_prepare(
                detail::parse_prepare_command_line(args.subspan(1)));
        }
        return detail::execute(
            detail::parse_generate_command_line(args.subspan(1)));
    } catch (const std::exception& error) {
        std::println(stderr, "bioseq_highway_codegen: {}", error.what());
        return 1;
    }
}

}  // namespace bioseq::codegen::highway
