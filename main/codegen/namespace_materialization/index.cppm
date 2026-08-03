module;

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
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
#include <system_error>
#include <utility>
#include <vector>

export module bioseq.codegen.namespace_materialization;

export import :manifest;
export import :model;
import :contract_tests;
import :renderer;
import :validator;
import bioseq.codegen.clang_structural;
import bioseq.codegen.support;

export namespace bioseq::codegen::namespace_materialization {

namespace detail {

[[nodiscard]] inline std::map<std::string, std::string, std::less<>>
parse_options(
    std::span<std::string_view const> args,
    const std::set<std::string_view, std::less<>>& allowed,
    std::string_view command) {
    std::map<std::string, std::string, std::less<>> result;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string_view option = args[index];
        if (!allowed.contains(option)) {
            throw std::runtime_error(
                "unknown namespace_materialization " + std::string{command}
                + " option '" + std::string{option} + "'");
        }
        if (index + 1u >= args.size()) {
            throw std::runtime_error(
                "missing value for namespace_materialization option '"
                + std::string{option} + "'");
        }
        const std::string_view value = args[++index];
        if (value.empty() || !result.emplace(option, value).second) {
            throw std::runtime_error(
                "namespace_materialization option '" + std::string{option}
                + "' must occur once with a non-empty value");
        }
    }
    if (result.size() != allowed.size()) {
        std::string missing;
        for (const std::string_view option : allowed) {
            if (result.contains(option)) continue;
            if (!missing.empty()) missing += ", ";
            missing += option;
        }
        throw std::runtime_error(
            "missing namespace_materialization " + std::string{command}
            + " option(s): " + missing);
    }
    return result;
}

[[nodiscard]] inline std::vector<std::uint32_t> parse_enabled_ordinals(
    std::string_view value) {
    if (value == "-") return {};
    std::vector<std::uint32_t> result;
    std::size_t begin = 0u;
    while (begin < value.size()) {
        const std::size_t separator = value.find(',', begin);
        const std::string_view item = value.substr(
            begin,
            separator == std::string_view::npos
                ? value.size() - begin
                : separator - begin);
        std::uint32_t ordinal{};
        const auto parsed = std::from_chars(
            item.data(), item.data() + item.size(), ordinal);
        if (item.empty() || parsed.ec != std::errc{}
            || parsed.ptr != item.data() + item.size()
            || (!result.empty() && result.back() >= ordinal)) {
            throw std::runtime_error(
                "--enabled-ordinals must be '-' or a strictly increasing comma-separated integer list");
        }
        result.push_back(ordinal);
        if (separator == std::string_view::npos) break;
        begin = separator + 1u;
    }
    return result;
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
        "--source",
        "--stamp",
    };
    const auto options = parse_options(args, allowed, "prepare");
    return prepare_command_line{
        .source = options.at("--source"),
        .signature_source = options.at("--signature-source"),
        .raw_compile_database = options.at("--raw-compile-database"),
        .clang_binary = options.at("--clang-binary"),
        .clang_scan_deps = options.at("--clang-scan-deps"),
        .llvm_config = options.at("--llvm-config"),
        .compiler_module_inventory =
            options.at("--compiler-module-inventory"),
        .clang_sha256 = options.at("--clang-sha256"),
        .clang_scan_deps_sha256 =
            options.at("--clang-scan-deps-sha256"),
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
        "--configure-topology-manifest",
        "--depfile",
        "--enabled-ordinals",
        "--expected-topology-digest",
        "--llvm-config",
        "--llvm-config-sha256",
        "--name",
        "--output-module",
        "--output-root",
        "--pcm-manifest",
        "--pcm-root",
        "--profile",
        "--repository-root",
        "--signature-source",
        "--source",
        "--stamp",
        "--topology-manifest",
    };
    const auto options = parse_options(args, allowed, "generate");
    return command_line{
        .name = options.at("--name"),
        .source = options.at("--source"),
        .repository_root = options.at("--repository-root"),
        .output_module = options.at("--output-module"),
        .profile = options.at("--profile"),
        .configure_topology_manifest =
            options.at("--configure-topology-manifest"),
        .expected_topology_digest =
            options.at("--expected-topology-digest"),
        .enabled_ordinals =
            parse_enabled_ordinals(options.at("--enabled-ordinals")),
        .signature_source = options.at("--signature-source"),
        .compile_database = options.at("--compile-database"),
        .pcm_manifest = options.at("--pcm-manifest"),
        .pcm_root = options.at("--pcm-root"),
        .clang_binary = options.at("--clang-binary"),
        .clang_scan_deps = options.at("--clang-scan-deps"),
        .llvm_config = options.at("--llvm-config"),
        .compiler_module_inventory =
            options.at("--compiler-module-inventory"),
        .clang_sha256 = options.at("--clang-sha256"),
        .clang_scan_deps_sha256 =
            options.at("--clang-scan-deps-sha256"),
        .llvm_config_sha256 = options.at("--llvm-config-sha256"),
        .compiler_module_inventory_sha256 =
            options.at("--compiler-module-inventory-sha256"),
        .output_root = options.at("--output-root"),
        .topology_manifest = options.at("--topology-manifest"),
        .artifact_manifest = options.at("--artifact-manifest"),
        .depfile = options.at("--depfile"),
        .stamp = options.at("--stamp"),
    };
}

[[nodiscard]] inline std::filesystem::path path_identity(
    const std::filesystem::path& value) {
    std::error_code error;
    const std::filesystem::path absolute = value.is_absolute()
        ? value
        : std::filesystem::absolute(value, error);
    if (error) {
        throw std::runtime_error(
            "cannot make path absolute: " + value.string());
    }
    const std::filesystem::path canonical =
        std::filesystem::weakly_canonical(absolute, error);
    if (error) {
        throw std::runtime_error(
            "cannot canonicalize path: " + absolute.string());
    }
    return canonical;
}

inline void require_regular_file(
    const std::filesystem::path& path,
    std::string_view role) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        throw std::runtime_error(
            std::string{role} + " is not a regular file: " + path.string());
    }
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

inline void validate_fixed_output_paths(const command_line& command) {
    const std::filesystem::path root = path_identity(command.output_root);
    if (root != command.output_root) {
        throw std::runtime_error(
            "--output-root must use its canonical absolute spelling");
    }
    if (path_identity(command.topology_manifest)
            != root / "topology.json"
        || path_identity(command.artifact_manifest)
            != root / "artifacts.json") {
        throw std::runtime_error(
            "topology/artifact manifests must be the fixed files beneath --output-root");
    }
    if (path_identity(command.pcm_root) != root / "pcm") {
        throw std::runtime_error(
            "--pcm-root must be the fixed pcm directory beneath --output-root");
    }
    const std::filesystem::path stamp = path_identity(command.stamp);
    const std::filesystem::path depfile = path_identity(command.depfile);
    if (stamp.parent_path() != root || depfile.parent_path() != root) {
        throw std::runtime_error(
            "generation stamp and depfile must be direct children of --output-root");
    }
}

[[nodiscard]] inline int execute_prepare(prepare_command_line command) {
    require_regular_file(command.source, "template_flatten source");
    require_regular_file(command.signature_source, "signature source");
    if (path_identity(command.source) == path_identity(command.signature_source)) {
        throw std::runtime_error(
            "scan-only canonical source cannot be its own compilation-signature donor");
    }
    std::filesystem::create_directories(command.pcm_root);
    clang_structural::prepared_compilation_environment prepared =
        clang_structural::prepare_dependency_compilation_environment(
            command.source,
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
    prepared.dependencies.push_back(command.source);
    prepared.dependencies.push_back(command.signature_source);
    write_if_different(
        command.compile_database_output, prepared.compile_database_json);
    write_if_different(command.pcm_manifest, prepared.pcm_manifest_json);
    write_if_different(
        command.depfile,
        render_depfile(command.stamp, std::move(prepared.dependencies)));
    write_if_different(
        command.stamp,
        "bioseq_template_flatten_prepare_stamp/v1\n"
        "profile=production\n"
        "authority=core\n"
        "compilation_signature=" + prepared.compilation_signature + "\n"
        "relocatable_compilation_signature="
            + prepared.relocatable_compilation_signature + "\n"
        "canonical_token_import_sha256="
            + prepared.canonical_token_import_sha256 + "\n"
        "compile_database_sha256="
            + sha256_hex(prepared.compile_database_json) + "\n"
        "pcm_manifest_sha256=" + sha256_hex(prepared.pcm_manifest_json)
            + "\n");
    touch_stamp(command.stamp);
    return 0;
}

inline void validate_configure_handshake(
    const command_line& command,
    const configure_topology_provenance& configure,
    std::string_view source_sha256,
    const topology_plan& topology) {
    validate_configure_topology_digest(
        topology, command.expected_topology_digest);
    if (configure.generator_version != generator_version
        || configure.policy != template_flatten_policy
        || configure.name != command.name
        || path_identity(configure.source_path) != path_identity(command.source)
        || configure.source_sha256 != source_sha256
        || path_identity(configure.output_root)
            != path_identity(command.output_root)
        || configure.output_module != command.output_module
        || configure.profile != command.profile
        || configure.topology_digest != command.expected_topology_digest
        || configure.coordinate_count != topology.coordinates.size()) {
        throw std::runtime_error(
            "configure-time and structural template_flatten topology disagree; "
            "configured=" + configure.topology_digest + ", structural="
            + topology.topology_digest + "; reconfigure the build tree");
    }
}

[[nodiscard]] inline int execute_generate(command_line command) {
    validate_fixed_output_paths(command);
    require_regular_file(command.source, "template_flatten source");
    require_regular_file(command.signature_source, "signature source");
    require_regular_file(
        command.configure_topology_manifest, "configure topology manifest");
    const std::string source = read_file(command.source);
    const std::string source_sha256 = sha256_hex(source);
    const configure_topology_provenance configure =
        read_configure_topology_provenance(
            command.configure_topology_manifest);
    const clang_structural::translation_unit sliced =
        clang_structural::scan_token_slice(command.source, source);
    materialization_region region = extract_materialization_region(
        sliced, command.output_module, source);
    topology_plan topology = build_topology_plan(std::move(region));
    validate_configure_handshake(
        command, configure, source_sha256, topology);
    validate_enabled_ordinals(
        topology, command.profile, command.enabled_ordinals);
    validate_entry_ownership(topology);

    const clang_structural::dependency_compilation_authority
        dependency_authority =
            clang_structural::derive_dependency_compilation_authority(
                {command.source},
                command.signature_source,
                command.compile_database,
                "production");
    clang_structural::validate_dependency_compilation_authority_seal(
        dependency_authority, command.compile_database);
    const clang_structural::prepared_compilation_validation prepared =
        clang_structural::validate_prepared_compilation_environment(
            dependency_authority.root_source_paths,
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
            "production",
            clang_structural::dependency_compilation_manifest_authority(
                "core", dependency_authority));
    recover_interrupted_publication(
        command.output_root,
        command.artifact_manifest,
        command.stamp,
        template_flatten_policy);
    const previous_publication previous = read_previous_publication(
        command.artifact_manifest,
        command.output_root,
        template_flatten_policy,
        {});
    generation_plan plan{
        .topology = std::move(topology),
        .repository_root = path_identity(command.repository_root),
        .source_path = path_identity(command.source),
        .output_root = path_identity(command.output_root),
        .source_text = source,
        .source_sha256 = source_sha256,
        .name = command.name,
        .profile = command.profile,
        .enabled_ordinals = command.enabled_ordinals,
        .clang_version = clang_structural::clang_api_version(),
        .compilation_signature = sha256_hex(
            "bioseq_template_flatten_relocatable_prepared_closure/v1\n"
            + prepared.relocatable_compilation_signature + "\n"),
        .compiler_module_inventory_sha256 =
            command.compiler_module_inventory_sha256,
        .previous_manifest_identity = previous.manifest_identity,
    };

    std::uint32_t artifact_render_passes = 0u;
    generation_result provisional = render_generation(plan, false);
    ++artifact_render_passes;
    std::vector<clang_structural::translation_unit> probes;
    for (const materialization_artifact& artifact : provisional.artifacts) {
        if (artifact.artifact_kind
            != materialization_artifact::kind::declaration_probe) {
            continue;
        }
        const std::filesystem::path probe_path =
            command.output_root / artifact.relative_path;
        probes.push_back(
            clang_structural::scan_declaration_probe_from_database(
                probe_path,
                artifact.content,
                command.signature_source,
                command.compile_database,
                "production",
                command.pcm_root));
    }
    validate_declaration_probes(plan.topology, probes);
    plan.declaration_probes = std::move(probes);
    plan.frontend_actions = frontend_action_ledger{
        .dependency_preparation_validations = 1u,
        // One direct structural extraction plus the canonical-source scan
        // performed while re-deriving dependency authority in this command.
        .canonical_token_slice_scans = 2u,
        // Dependency-authority derivation and every declaration probe each
        // token-scan the signature donor once.
        .signature_donor_token_slice_scans = 1u
            + static_cast<std::uint32_t>(plan.declaration_probes.size()),
        .body_free_declaration_probe_scans =
            static_cast<std::uint32_t>(plan.declaration_probes.size()),
        .artifact_render_passes = artifact_render_passes + 1u,
        .body_bearing_compiler_invocations = 0u,
    };
    plan.compilation_obligations = cmake_compilation_obligations{
        .api_partition_units = 1u,
        .selector_partition_units = 1u,
        .body_bearing_implementation_units =
            static_cast<std::uint32_t>(plan.enabled_ordinals.size()),
    };
    generation_result generated = render_generation(plan);
    ++artifact_render_passes;
    if (artifact_render_passes != plan.frontend_actions.artifact_render_passes) {
        throw std::runtime_error(
            "namespace-materialization render action ledger drifted");
    }

    std::vector<publication_artifact> publication;
    publication.reserve(generated.artifacts.size());
    for (const materialization_artifact& artifact : generated.artifacts) {
        publication.push_back(publication_artifact{
            .relative_path = artifact.relative_path,
            .content = artifact.content,
            .content_sha256 = artifact.content_sha256,
            .artifact_class = artifact.artifact_kind
                    != materialization_artifact::kind::manifest
                ? publication_artifact_class::production_cppm
                : publication_artifact_class::metadata_json,
        });
    }
    write_if_different(
        command.depfile,
        render_depfile(
            command.stamp,
            {
                command.source,
                command.signature_source,
                command.configure_topology_manifest,
                command.compile_database,
                command.pcm_manifest,
                command.compiler_module_inventory,
            }));
    publish_artifacts(publication_request{
        .output_root = command.output_root,
        .artifact_manifest = command.artifact_manifest,
        .stamp = command.stamp,
        .artifact_manifest_json = generated.artifact_manifest_json,
        .artifacts = std::move(publication),
    });
    return 0;
}

}  // namespace detail

inline int self_test() {
    try {
        run_namespace_materialization_contract_tests();
        return 0;
    } catch (const std::exception& error) {
        std::println(
            stderr,
            "bioseq_namespace_materialization_codegen: {}",
            error.what());
        return 1;
    }
}

inline int run(std::span<std::string_view const> args) {
    if (args.empty()
        || (args.front() != "prepare" && args.front() != "generate")) {
        std::println(
            stderr,
            "usage: bioseq_codegen namespace_materialization "
            "<prepare|generate> [options]");
        return 2;
    }
    try {
        if (args.front() == "prepare") {
            return detail::execute_prepare(
                detail::parse_prepare_command_line(args.subspan(1)));
        }
        return detail::execute_generate(
            detail::parse_generate_command_line(args.subspan(1)));
    } catch (const std::exception& error) {
        std::println(
            stderr,
            "bioseq_namespace_materialization_codegen: {}",
            error.what());
        return 1;
    }
}

}  // namespace bioseq::codegen::namespace_materialization
