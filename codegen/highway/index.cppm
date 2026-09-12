module;

#include <algorithm>
#include <array>
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
import :tests.contract;
import bioseq.codegen.structural_analysis;
import bioseq.codegen.namespace_materialization;
import bioseq.codegen.support;

export namespace bioseq::codegen::highway {

namespace detail {

struct parsed_options final {
    std::map<std::string, std::string, std::less<>> singleton;
    std::vector<std::string> domain_topology_manifests;
};

[[nodiscard]] inline parsed_options
parse_options(
    std::span<std::string_view const> args,
    const std::set<std::string_view, std::less<>>& allowed) {
    parsed_options result;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string_view option = args[index];
        constexpr std::string_view domain_option =
            "--domain-topology-manifest";
        if (!allowed.contains(option) && option != domain_option) {
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
        if (value.empty()) {
            throw std::runtime_error(
                "Highway generator option '" + std::string{option}
                + "' requires a non-empty value");
        }
        if (option == domain_option) {
            result.domain_topology_manifests.emplace_back(value);
        } else if (!result.singleton.emplace(option, value).second) {
            throw std::runtime_error(
                "Highway generator option '" + std::string{option}
                + "' must occur once");
        }
    }
    if (result.singleton.size() != allowed.size()) {
        std::string missing;
        for (std::string_view option : allowed) {
            if (!result.singleton.contains(option)) {
                if (!missing.empty()) missing += ", ";
                missing += option;
            }
        }
        throw std::runtime_error(
            "missing required Highway generator option(s): " + missing);
    }
    return result;
}

[[nodiscard]] inline std::vector<domain_topology_manifest_input>
parse_domain_topology_arguments(const std::vector<std::string>& values) {
    std::vector<domain_topology_manifest_input> result;
    result.reserve(values.size());
    for (const std::string& value : values) {
        result.push_back(parse_domain_topology_manifest_argument(value));
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
    const parsed_options parsed = parse_options(args, allowed);
    const auto& options = parsed.singleton;
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
        .domain_topology_manifests =
            parse_domain_topology_arguments(
                parsed.domain_topology_manifests),
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
        "--signature-source",
        "--source-manifest",
        "--stamp",
    };
    const parsed_options parsed = parse_options(args, allowed);
    const auto& options = parsed.singleton;
    return prepare_command_line{
        .source_manifest_path = options.at("--source-manifest"),
        .signature_source = options.at("--signature-source"),
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
        .domain_topology_manifests =
            parse_domain_topology_arguments(
                parsed.domain_topology_manifests),
    };
}

[[nodiscard]] inline minimizer_registry_command_line
parse_minimizer_registry_command_line(
    std::span<std::string_view const> args) {
    static const std::set<std::string_view, std::less<>> allowed{
        "--manifest-output",
        "--profile",
        "--records-output",
        "--stamp",
        "--visitor-output",
    };
    const parsed_options parsed = parse_options(args, allowed);
    const auto& options = parsed.singleton;
    return minimizer_registry_command_line{
        .profile = parse_minimizer_registry_profile(options.at("--profile")),
        .records_output = options.at("--records-output"),
        .visitor_output = options.at("--visitor-output"),
        .manifest_output = options.at("--manifest-output"),
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

[[nodiscard]] inline std::string domain_topology_identity(
    const std::vector<domain_topology_provenance>& topologies) {
    std::string material =
        "bioseq-highway-domain-topologies\n"
        "revision=1\n";
    for (std::size_t index = 0; index < topologies.size(); ++index) {
        const domain_topology_provenance& topology = topologies[index];
        material += std::to_string(index) + "\t" + topology.name + "\t"
            + topology.topology_digest + "\t" + topology.content_sha256
            + "\n";
    }
    return bioseq::common::sha256_hex(material);
}

[[nodiscard]] inline int execute_prepare(prepare_command_line command) {
    validate_prepare_command_line(command);
    const std::vector<domain_topology_provenance> domain_topologies =
        load_domain_topology_manifests(
            command.domain_topology_manifests);
    const source_manifest sources =
        parse_source_manifest(command.source_manifest_path);
    validate_domain_topology_authority(
        sources.domain_topologies,
        domain_topologies,
        "Highway prepare");
    const std::vector<std::filesystem::path> source_paths =
        source_paths_of(sources);
    for (const std::filesystem::path& source_path : source_paths) {
        if (std::filesystem::equivalent(
                source_path, command.signature_source)) {
            throw std::runtime_error(
                "Highway signature donor must not be a canonical target source");
        }
    }
    std::filesystem::create_directories(command.pcm_root);
    // Retarget the neutral attached-primary command to every registered
    // canonical module, then build their complete module graph only inside
    // the codegen-owned scratch PCM authority. No authored scan target or
    // linked object provider is involved.
    structural_analysis::dependency_compilation_environment prepared =
        structural_analysis::prepare_compilation_environment(
            source_paths,
            command.signature_source,
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
    prepared.dependencies.push_back(command.signature_source);
    if (!sources.minimizer_registry_manifest_path.empty()) {
        prepared.dependencies.push_back(
            sources.minimizer_registry_manifest_path);
    }
    for (const domain_topology_provenance& topology : domain_topologies) {
        prepared.dependencies.push_back(topology.path);
    }
    publish_artifact_if_changed(
        command.compile_database_output, prepared.compile_database_json);
    publish_artifact_if_changed(command.pcm_manifest, prepared.pcm_manifest_json);
    publish_artifact_if_changed(
        command.depfile,
        render_depfile(command.stamp, std::move(prepared.dependencies)));
    publish_artifact_if_changed(
        command.stamp,
        "bioseq_highway_prepare_stamp\n"
        "revision=2\n"
        "profile=production\n"
        "authority=core\n"
        "signature_source_sha256="
            + bioseq::common::sha256_hex(read_artifact_bytes(command.signature_source)) + "\n"
        "compilation_signature=" + prepared.compilation_signature + "\n"
        "relocatable_compilation_signature="
            + prepared.relocatable_compilation_signature + "\n"
        "compile_database_sha256="
            + bioseq::common::sha256_hex(prepared.compile_database_json) + "\n"
        "pcm_manifest_sha256=" + bioseq::common::sha256_hex(prepared.pcm_manifest_json) + "\n"
        "relocatable_compile_database_sha256="
            + prepared.relocatable_compile_database_sha256 + "\n"
        "relocatable_pcm_manifest_sha256="
            + prepared.relocatable_pcm_manifest_sha256 + "\n"
        "domain_topologies_sha256="
            + domain_topology_identity(domain_topologies) + "\n");
    mark_artifact_complete(command.stamp);
    return 0;
}

[[nodiscard]] inline int execute_minimizer_registry(
    minimizer_registry_command_line command) {
    const std::array outputs{
        command.records_output,
        command.visitor_output,
        command.manifest_output,
        command.stamp,
    };
    std::set<std::filesystem::path> unique_outputs;
    for (const std::filesystem::path& output : outputs) {
        if (!output.is_absolute() || output.empty()
            || !unique_outputs.insert(output.lexically_normal()).second) {
            throw std::runtime_error(
                "minimizer registry outputs must be distinct absolute paths");
        }
    }
    if (command.records_output.extension() != ".cppm"
        || command.visitor_output.extension() != ".cppm"
        || command.manifest_output.extension() != ".tsv") {
        throw std::runtime_error(
            "minimizer registry requires .cppm module outputs and a .tsv manifest");
    }

    const minimizer_registry_spec registry =
        make_minimizer_registry(command.profile);
    validate_minimizer_registry(registry);
    const std::string manifest = render_minimizer_registry_manifest(registry);
    const std::string identity = bioseq::common::sha256_hex(manifest);
    const std::string records = render_minimizer_physical_records_module(
        registry, identity);
    const std::string visitor = render_minimizer_physical_registry_module(
        registry, identity);
    publish_artifact_if_changed(command.records_output, records);
    publish_artifact_if_changed(command.visitor_output, visitor);
    publish_artifact_if_changed(command.manifest_output, manifest);
    publish_artifact_if_changed(
        command.stamp,
        "bioseq_minimizer_registry_stamp\n"
        "revision=1\n"
        "profile=" + std::string{minimizer_registry_profile_name(command.profile)}
            + "\nmanifest_identity=" + identity
            + "\nrecords_sha256=" + bioseq::common::sha256_hex(records)
            + "\nvisitor_sha256=" + bioseq::common::sha256_hex(visitor) + "\n");
    mark_artifact_complete(command.stamp);
    return 0;
}

[[nodiscard]] inline int execute(command_line command) {
    validate_command_line(command);
    std::vector<domain_topology_provenance> domain_topologies =
        load_domain_topology_manifests(
            command.domain_topology_manifests);
    target_manifest targets = parse_target_manifest(command.target_manifest_path);
    source_manifest sources = parse_source_manifest(command.source_manifest_path);
    validate_inventories(targets, sources);
    validate_domain_topology_authority(
        targets.domain_topologies,
        domain_topologies,
        "Highway generate");

    const std::vector<std::filesystem::path> source_paths =
        source_paths_of(sources);
    // The isolated database contains donor-derived commands and scratch BMIs
    // for the registered canonical module graph. These artifacts are codegen
    // authority only (never CMake-attached or linked); structural collection
    // retains compiler-proven callable geometry for generated HWY_ATTR sites.
    constexpr std::string_view compiler_profile = "production";
    const structural_analysis::dependency_compilation_validation prepared =
        structural_analysis::validate_dependency_compilation_environment(
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
    validate_highway_prepared_closure(sources, prepared);
    const structural_analysis::structural_index structural =
        structural_analysis::scan_translation_units_from_database(
            source_paths,
            command.compile_database,
            compiler_profile,
            command.pcm_root);
    const structural_analysis::module_graph graph =
        structural_analysis::scan_module_graph_from_database(
            source_paths,
            command.compile_database,
            compiler_profile,
            command.pcm_root);

    generation_plan plan{
        .target_inventory = std::move(targets),
        .source_inventory = std::move(sources),
        .clang_release = structural.clang_release,
        .compilation_signature = bioseq::common::sha256_hex(
            "bioseq_highway_relocatable_prepared_closure\n"
            "revision=2\n"
            + prepared.relocatable_compilation_signature + "\n"
            + "domain_topologies_sha256="
            + domain_topology_identity(domain_topologies) + "\n"),
        .domain_topologies = std::move(domain_topologies),
    };
    plan.extracted_sources =
        extract_sources(plan.source_inventory, structural, graph);
    validate_slot_callable_ownership(
        plan.source_inventory, plan.extracted_sources);
    namespace_materialization::recover_interrupted_publication(
        command.output_root,
        command.artifact_manifest,
        command.stamp,
        publication_policy);
    const namespace_materialization::previous_publication previous =
        namespace_materialization::read_previous_publication(
            command.artifact_manifest,
            command.output_root,
            publication_policy,
            {});
    generation_result generated = render_generation(plan);
    std::vector<namespace_materialization::publication_artifact> publication =
        make_publication_artifacts(generated);
    const namespace_materialization::rendered_publication_manifest manifest =
        render_highway_publication_manifest(
            command.output_root,
            previous.manifest_identity,
            plan,
            generated,
            publication);
    namespace_materialization::publish_artifacts(
        namespace_materialization::publication_request{
            .output_root = command.output_root,
            .artifact_manifest = command.artifact_manifest,
            .stamp = command.stamp,
            .artifact_manifest_json = manifest.json,
            .artifacts = std::move(publication),
        });
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
                    .target_namespace = "N_EMU128",
                    .bit = 1,
                },
            },
        },
        .source_inventory = source_manifest{
            .contract_module = "bioseq.highway.dispatch_contract",
            .dispatch_module = "bioseq.scheduler.dispatch_processor",
        },
        .clang_release = "self-test",
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
        || registry->content.find("registered_highway_dispatch_plan")
            != std::string::npos
        || inventory->content.find("registered_highway_dispatch_plan")
            == std::string::npos
        || inventory->content.find("registered_highway_target_binding")
            == std::string::npos
        || inventory->content.find(
               "import bioseq.highway.generated.x86_64.emu128;")
            == std::string::npos
        || inventory->content.find("hwy::") != std::string::npos
        || generated.policy_evidence_json.find(
               "\"path\":\"highway_registry.cppm\"")
            == std::string::npos
        || generated.policy_evidence_json.find(
               "\"path\":\"dispatch_inventory.cppm\"")
            == std::string::npos
        || generated.policy_evidence_json.find(
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
    if (args.empty()
        || (args.front() != "prepare" && args.front() != "generate"
            && args.front() != "minimizer-registry")) {
        std::println(
            stderr,
            "usage: bioseq_codegen highway "
            "<minimizer-registry|prepare|generate> [options] "
            "[--domain-topology-manifest <name>=<absolute-path>]...");
        return 2;
    }
    try {
        if (args.front() == "prepare") {
            return detail::execute_prepare(
                detail::parse_prepare_command_line(args.subspan(1)));
        }
        if (args.front() == "minimizer-registry") {
            return detail::execute_minimizer_registry(
                detail::parse_minimizer_registry_command_line(
                    args.subspan(1)));
        }
        return detail::execute(
            detail::parse_generate_command_line(args.subspan(1)));
    } catch (const std::exception& error) {
        std::println(stderr, "bioseq_highway_codegen: {}", error.what());
        return 1;
    }
}

}  // namespace bioseq::codegen::highway
