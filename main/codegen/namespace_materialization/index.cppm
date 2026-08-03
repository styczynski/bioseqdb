module;

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
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
        "--action-trace",
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
        .action_trace = options.at("--action-trace"),
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
        "--prepare-action-trace",
        "--prepare-stamp",
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
        .prepare_action_trace =
            options.at("--prepare-action-trace"),
        .prepare_stamp = options.at("--prepare-stamp"),
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
    std::error_code pcm_spelling_error;
    const std::filesystem::path configured_pcm_root =
        (command.pcm_root.is_absolute()
             ? command.pcm_root
             : std::filesystem::absolute(
                   command.pcm_root, pcm_spelling_error))
            .lexically_normal();
    if (pcm_spelling_error
        || configured_pcm_root != root / "pcm") {
        throw std::runtime_error(
            "--pcm-root must be the fixed pcm directory beneath --output-root");
    }
    const std::filesystem::path canonical_pcm_root =
        path_identity(command.pcm_root);
    if (path_identity(command.prepare_action_trace)
        != canonical_pcm_root / "metadata/prepare_actions.json") {
        throw std::runtime_error(
            "--prepare-action-trace must be the fixed PREPARE metadata file");
    }
    if (path_identity(command.prepare_stamp)
        != canonical_pcm_root / "metadata/prepare.stamp") {
        throw std::runtime_error(
            "--prepare-stamp must be the fixed PREPARE metadata file");
    }
    const std::filesystem::path stamp = path_identity(command.stamp);
    const std::filesystem::path depfile = path_identity(command.depfile);
    if (stamp.parent_path() != root || depfile.parent_path() != root) {
        throw std::runtime_error(
            "generation stamp and depfile must be direct children of --output-root");
    }
}

[[nodiscard]] inline std::filesystem::path absolute_spelling(
    const std::filesystem::path& value) {
    std::error_code error;
    const std::filesystem::path absolute = value.is_absolute()
        ? value
        : std::filesystem::absolute(value, error);
    if (error) {
        throw std::runtime_error(
            "cannot make PREPARE path absolute: " + value.string());
    }
    return absolute.lexically_normal();
}

struct prepare_output_authority {
    std::filesystem::path configured_root;
    std::filesystem::path canonical_root;
    std::filesystem::path configured_metadata;
    std::filesystem::path canonical_metadata;
};

[[nodiscard]] inline std::filesystem::path canonical_prepare_output(
    const prepare_output_authority& authority,
    const std::filesystem::path& configured_path) {
    return authority.canonical_metadata
        / absolute_spelling(configured_path).filename();
}

inline void validate_prepare_fixed_output_paths(
    const prepare_command_line& command,
    const prepare_output_authority& authority) {
    const std::filesystem::path configured_root =
        absolute_spelling(command.pcm_root);
    if (configured_root != authority.configured_root
        || path_identity(configured_root)
            != authority.canonical_root) {
        throw std::runtime_error(
            "PREPARE PCM root changed from its entry identity");
    }
    std::error_code error;
    const std::filesystem::file_status root_status =
        std::filesystem::symlink_status(configured_root, error);
    if (error) {
        throw std::runtime_error(
            "cannot inspect --pcm-root: "
            + configured_root.string());
    }
    if (std::filesystem::exists(root_status)
        && (!std::filesystem::is_directory(
                configured_root, error)
            || error)) {
        throw std::runtime_error(
            "--pcm-root must be a directory or a symlink to one");
    }
    const std::filesystem::path metadata =
        configured_root / "metadata";
    if (metadata != authority.configured_metadata) {
        throw std::runtime_error(
            "PREPARE metadata spelling changed from entry");
    }
    error.clear();
    const std::filesystem::file_status metadata_status =
        std::filesystem::symlink_status(metadata, error);
    if (error) {
        throw std::runtime_error(
            "cannot inspect PREPARE metadata directory: "
            + metadata.string());
    }
    if (std::filesystem::exists(metadata_status)
        && (std::filesystem::is_symlink(metadata_status)
            || !std::filesystem::is_directory(metadata_status))) {
        throw std::runtime_error(
            "PREPARE metadata directory must be a real directory");
    }
    if (path_identity(metadata)
        != authority.canonical_metadata) {
        throw std::runtime_error(
            "PREPARE metadata directory changed from its entry identity");
    }
    const std::initializer_list<std::pair<
        std::filesystem::path, std::string_view>> outputs{
        {command.compile_database_output,
         "prepare_compile_commands.json"},
        {command.pcm_manifest, "pcm_manifest.json"},
        {command.action_trace, "prepare_actions.json"},
        {command.depfile, "prepare.d"},
        {command.stamp, "prepare.stamp"},
    };
    for (const auto& [configured, filename] : outputs) {
        const std::filesystem::path output =
            absolute_spelling(configured);
        const std::filesystem::path expected =
            metadata / std::filesystem::path{std::string{filename}};
        const std::filesystem::path canonical_output =
            authority.canonical_metadata
            / std::filesystem::path{std::string{filename}};
        if (output != expected
            || path_identity(output.parent_path())
                != authority.canonical_metadata) {
            throw std::runtime_error(
                "PREPARE output must use its exact fixed metadata path: "
                + expected.string());
        }
        error.clear();
        const std::filesystem::file_status status =
            std::filesystem::symlink_status(canonical_output, error);
        if (error
            || (std::filesystem::exists(status)
                && (std::filesystem::is_symlink(status)
                    || !std::filesystem::is_regular_file(status)))) {
            throw std::runtime_error(
                "PREPARE output is a symlink or non-regular file: "
                + output.string());
        }
    }
}

[[nodiscard]] inline prepare_output_authority
seal_prepare_output_authority(const prepare_command_line& command) {
    prepare_output_authority authority{
        .configured_root = absolute_spelling(command.pcm_root),
    };
    authority.canonical_root =
        path_identity(authority.configured_root);
    authority.configured_metadata =
        authority.configured_root / "metadata";
    authority.canonical_metadata =
        authority.canonical_root / "metadata";
    const std::filesystem::path configured_stamp =
        absolute_spelling(command.stamp);
    if (configured_stamp
            != authority.configured_metadata / "prepare.stamp"
        || path_identity(configured_stamp.parent_path())
            != authority.canonical_metadata) {
        throw std::runtime_error(
            "PREPARE stamp must use its exact fixed metadata path");
    }
    return authority;
}

class prepare_completion_stamp_guard {
  public:
    explicit prepare_completion_stamp_guard(
        const prepare_output_authority& authority)
        : stamp_(authority.canonical_metadata / "prepare.stamp") {
        remove_stamp(true);
    }

    prepare_completion_stamp_guard(
        const prepare_completion_stamp_guard&) = delete;
    prepare_completion_stamp_guard& operator=(
        const prepare_completion_stamp_guard&) = delete;

    ~prepare_completion_stamp_guard() {
        if (active_) remove_stamp(false);
    }

    void commit() noexcept { active_ = false; }

  private:
    void remove_stamp(bool strict) {
        std::error_code error;
        const std::filesystem::file_status status =
            std::filesystem::symlink_status(stamp_, error);
        if (error) {
            if (strict) {
                throw std::runtime_error(
                    "cannot inspect PREPARE completion stamp: "
                    + error.message());
            }
            return;
        }
        if (!std::filesystem::exists(status)) return;
        if (!std::filesystem::is_regular_file(status)
            && !std::filesystem::is_symlink(status)) {
            if (strict) {
                throw std::runtime_error(
                    "PREPARE completion stamp is non-regular");
            }
            return;
        }
        std::filesystem::remove(stamp_, error);
        if (strict && error) {
            throw std::runtime_error(
                "cannot invalidate PREPARE completion stamp: "
                + error.message());
        }
    }

    std::filesystem::path stamp_;
    bool active_{true};
};

inline void ensure_prepare_metadata_directory(
    const prepare_command_line& command,
    const prepare_output_authority& authority) {
    std::error_code error;
    std::filesystem::create_directories(
        authority.canonical_root, error);
    if (error) {
        throw std::runtime_error(
            "cannot create PREPARE PCM root: " + error.message());
    }
    const std::filesystem::path& metadata =
        authority.canonical_metadata;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(metadata, error);
    if (error) {
        throw std::runtime_error(
            "cannot inspect PREPARE metadata directory: "
            + error.message());
    }
    if (!std::filesystem::exists(status)) {
        if (!std::filesystem::create_directory(metadata, error)
            || error) {
            throw std::runtime_error(
                "cannot create PREPARE metadata directory: "
                + error.message());
        }
    } else if (std::filesystem::is_symlink(status)
               || !std::filesystem::is_directory(status)) {
        throw std::runtime_error(
            "PREPARE metadata directory changed before creation");
    }
    validate_prepare_fixed_output_paths(command, authority);
}

inline void write_prepare_output(
    const prepare_command_line& command,
    const prepare_output_authority& authority,
    const std::filesystem::path& configured_path,
    std::string_view content) {
    validate_prepare_fixed_output_paths(command, authority);
    const std::filesystem::path path =
        canonical_prepare_output(authority, configured_path);
    if (std::filesystem::is_regular_file(path)
        && read_file(path) == content) {
        return;
    }
    static std::atomic_uint64_t sequence{0};
    std::filesystem::path temporary;
    std::ofstream output;
    for (std::size_t attempt = 0; attempt < 256; ++attempt) {
        temporary = path.parent_path()
            / (path.filename().string() + ".stage-"
               + std::to_string(
                   sequence.fetch_add(
                       1, std::memory_order_relaxed)));
        output.open(
            temporary,
            std::ios::binary | std::ios::noreplace);
        if (output) break;
        output.clear();
        if (!std::filesystem::exists(temporary)) {
            throw std::runtime_error(
                "cannot create staged PREPARE output: "
                + temporary.string());
        }
    }
    if (!output) {
        throw std::runtime_error(
            "cannot allocate unique staged PREPARE output for "
            + path.string());
    }
    output.write(
        content.data(),
        static_cast<std::streamsize>(content.size()));
    output.close();
    if (!output) {
        std::filesystem::remove(temporary);
        throw std::runtime_error(
            "cannot finish staged PREPARE output: "
            + temporary.string());
    }
    validate_prepare_fixed_output_paths(command, authority);
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary);
        throw std::runtime_error(
            "cannot atomically publish PREPARE output: "
            + error.message());
    }
    validate_prepare_fixed_output_paths(command, authority);
}

struct prepare_file_snapshot {
    std::filesystem::path configured_spelling;
    std::filesystem::path identity;
    std::string sha256;

    bool operator==(const prepare_file_snapshot&) const = default;
};

[[nodiscard]] inline std::vector<prepare_file_snapshot>
capture_prepare_file_snapshot(
    std::vector<std::filesystem::path> paths,
    std::string_view role) {
    std::ranges::sort(
        paths,
        {},
        [](const std::filesystem::path& path) {
            return absolute_spelling(path).generic_string();
        });
    paths.erase(
        std::ranges::unique(
            paths,
            {},
            [](const std::filesystem::path& path) {
                return absolute_spelling(path).generic_string();
            }).begin(),
        paths.end());
    std::vector<prepare_file_snapshot> result;
    result.reserve(paths.size());
    for (const std::filesystem::path& path : paths) {
        const std::filesystem::path spelling =
            absolute_spelling(path);
        require_regular_file(spelling, role);
        const std::filesystem::path identity =
            path_identity(spelling);
        result.push_back(prepare_file_snapshot{
            .configured_spelling = spelling,
            .identity = identity,
            .sha256 = sha256_hex(read_file(identity)),
        });
    }
    return result;
}

[[nodiscard]] inline bool same_preparation_identity(
    const clang_structural::preparation_module_identity& left,
    const clang_structural::preparation_module_identity& right) {
    return left.module_name == right.module_name
        && path_identity(left.source_path)
            == path_identity(right.source_path)
        && left.source_sha256 == right.source_sha256;
}

inline void require_same_dependency_authority(
    const clang_structural::dependency_compilation_authority& expected,
    const clang_structural::dependency_compilation_authority& actual,
    std::string_view role) {
    const bool roots_match =
        expected.root_source_paths.size()
            == actual.root_source_paths.size()
        && std::ranges::equal(
            expected.root_source_paths,
            actual.root_source_paths,
            [](const std::filesystem::path& left,
               const std::filesystem::path& right) {
                return path_identity(left) == path_identity(right);
            });
    const bool canonical_match =
        expected.canonical_inputs.size()
            == actual.canonical_inputs.size()
        && std::ranges::equal(
            expected.canonical_inputs,
            actual.canonical_inputs,
            same_preparation_identity);
    if (!roots_match
        || !same_preparation_identity(
            expected.signature_donor, actual.signature_donor)
        || !canonical_match
        || expected.canonical_token_import_sha256
            != actual.canonical_token_import_sha256) {
        throw std::runtime_error(
            "PREPARE dependency compilation authority changed "
            + std::string{role});
    }
}

struct prepare_entry_authority {
    clang_structural::dependency_compilation_authority dependency;
    prepare_file_snapshot raw_compile_database;
    prepare_file_snapshot clang_binary;
    prepare_file_snapshot clang_scan_deps;
    prepare_file_snapshot llvm_config;
    prepare_file_snapshot compiler_module_inventory;
};

[[nodiscard]] inline prepare_file_snapshot capture_prepare_file(
    const std::filesystem::path& path,
    std::string_view role) {
    return capture_prepare_file_snapshot({path}, role).front();
}

inline void require_expected_prepare_hash(
    const prepare_file_snapshot& file,
    std::string_view expected,
    std::string_view role) {
    if (expected.size() != 64 || file.sha256 != expected) {
        throw std::runtime_error(
            "PREPARE " + std::string{role}
            + " hash differs from configured authority");
    }
}

[[nodiscard]] inline prepare_entry_authority
seal_prepare_entry_authority(
    const prepare_command_line& command,
    const prepare_output_authority& output_authority) {
    validate_prepare_fixed_output_paths(
        command, output_authority);
    prepare_entry_authority authority{
        .dependency =
            clang_structural::derive_dependency_compilation_authority(
                {command.source},
                command.signature_source,
                command.raw_compile_database,
                "production"),
        .raw_compile_database = capture_prepare_file(
            command.raw_compile_database, "raw compile database"),
        .clang_binary = capture_prepare_file(
            command.clang_binary, "clang binary"),
        .clang_scan_deps = capture_prepare_file(
            command.clang_scan_deps, "clang-scan-deps binary"),
        .llvm_config = capture_prepare_file(
            command.llvm_config, "llvm-config binary"),
        .compiler_module_inventory = capture_prepare_file(
            command.compiler_module_inventory,
            "compiler module inventory"),
    };
    require_expected_prepare_hash(
        authority.clang_binary,
        command.clang_sha256,
        "clang binary");
    require_expected_prepare_hash(
        authority.clang_scan_deps,
        command.clang_scan_deps_sha256,
        "clang-scan-deps binary");
    require_expected_prepare_hash(
        authority.llvm_config,
        command.llvm_config_sha256,
        "llvm-config binary");
    require_expected_prepare_hash(
        authority.compiler_module_inventory,
        command.compiler_module_inventory_sha256,
        "compiler module inventory");
    return authority;
}

inline void revalidate_prepare_entry_authority(
    const prepare_command_line& command,
    const prepare_output_authority& output_authority,
    const prepare_entry_authority& expected,
    const clang_structural::prepared_compilation_environment*
        prepared = nullptr) {
    validate_prepare_fixed_output_paths(
        command, output_authority);
    const std::vector<prepare_file_snapshot> actual_files =
        capture_prepare_file_snapshot(
            {
                command.raw_compile_database,
                command.clang_binary,
                command.clang_scan_deps,
                command.llvm_config,
                command.compiler_module_inventory,
            },
            "entry authority file");
    std::vector<prepare_file_snapshot> expected_files{
        expected.raw_compile_database,
        expected.clang_binary,
        expected.clang_scan_deps,
        expected.llvm_config,
        expected.compiler_module_inventory,
    };
    std::ranges::sort(
        expected_files,
        {},
        &prepare_file_snapshot::configured_spelling);
    if (actual_files != expected_files) {
        throw std::runtime_error(
            "PREPARE raw compile database or tool authority changed");
    }
    const clang_structural::dependency_compilation_authority actual =
        clang_structural::derive_dependency_compilation_authority(
            {command.source},
            command.signature_source,
            command.raw_compile_database,
            "production");
    require_same_dependency_authority(
        expected.dependency, actual, "after entry sealing");
    if (prepared != nullptr) {
        if (prepared->canonical_token_import_sha256
                != expected.dependency.canonical_token_import_sha256
            || prepared->authority
                != clang_structural::dependency_compilation_manifest_authority(
                    "core", expected.dependency)
            || prepared->dependency_root_sources.size()
                != expected.dependency.root_source_paths.size()
            || !std::ranges::equal(
                prepared->dependency_root_sources,
                expected.dependency.root_source_paths,
                [](const std::filesystem::path& left,
                   const std::filesystem::path& right) {
                    return path_identity(left)
                        == path_identity(right);
                })) {
            throw std::runtime_error(
                "PREPARE environment differs from its sealed dependency authority");
        }
    }
}

inline void require_prepare_document(
    const prepare_command_line& command,
    const prepare_output_authority& authority,
    const std::filesystem::path& configured_path,
    std::string_view expected_content,
    std::string_view role) {
    validate_prepare_fixed_output_paths(command, authority);
    const std::filesystem::path path =
        canonical_prepare_output(authority, configured_path);
    require_regular_file(path, role);
    if (read_file(path) != expected_content) {
        throw std::runtime_error(
            "published PREPARE " + std::string{role}
            + " changed before completion");
    }
}

[[nodiscard]] inline int execute_prepare(prepare_command_line command) {
    const prepare_output_authority output_authority =
        seal_prepare_output_authority(command);
    prepare_completion_stamp_guard stamp_guard{output_authority};
    validate_prepare_fixed_output_paths(command, output_authority);
    require_regular_file(command.source, "template_flatten source");
    require_regular_file(command.signature_source, "signature source");
    if (path_identity(command.source)
        == path_identity(command.signature_source)) {
        throw std::runtime_error(
            "scan-only canonical source cannot be its own compilation-signature donor");
    }
    ensure_prepare_metadata_directory(command, output_authority);
    const prepare_entry_authority entry_authority =
        seal_prepare_entry_authority(command, output_authority);
    clang_structural::prepared_compilation_environment prepared =
        clang_structural::prepare_dependency_compilation_environment(
            entry_authority.dependency.canonical_inputs.front()
                .source_path,
            entry_authority.dependency.signature_donor.source_path,
            entry_authority.raw_compile_database.identity,
            entry_authority.clang_binary.identity,
            entry_authority.clang_scan_deps.identity,
            entry_authority.llvm_config.identity,
            entry_authority.compiler_module_inventory.identity,
            command.clang_sha256,
            command.clang_scan_deps_sha256,
            command.llvm_config_sha256,
            command.compiler_module_inventory_sha256,
            output_authority.canonical_root,
            "production",
            "core");
    invoke_prepare_publication_test_checkpoint(
        prepare_publication_checkpoint::after_environment_built);
    revalidate_prepare_entry_authority(
        command, output_authority, entry_authority, &prepared);
    const std::string action_trace =
        clang_structural::render_preparation_action_trace(prepared);
    prepared.dependencies.push_back(command.source);
    prepared.dependencies.push_back(command.signature_source);
    const std::string depfile = render_depfile(
        command.stamp, prepared.dependencies);
    write_prepare_output(
        command,
        output_authority,
        command.compile_database_output, prepared.compile_database_json);
    write_prepare_output(
        command,
        output_authority,
        command.pcm_manifest,
        prepared.pcm_manifest_json);
    invoke_prepare_publication_test_checkpoint(
        prepare_publication_checkpoint::after_db_manifest_publish);
    revalidate_prepare_entry_authority(
        command, output_authority, entry_authority, &prepared);
    const std::filesystem::path compile_database =
        canonical_prepare_output(
            output_authority,
            command.compile_database_output);
    const std::filesystem::path pcm_manifest =
        canonical_prepare_output(
            output_authority, command.pcm_manifest);
    const std::string manifest_authority =
        clang_structural::dependency_compilation_manifest_authority(
            "core", entry_authority.dependency);
    clang_structural::validate_dependency_compilation_authority_seal(
        entry_authority.dependency, compile_database);
    const clang_structural::prepared_compilation_validation validated =
        clang_structural::validate_prepared_compilation_environment(
            entry_authority.dependency.root_source_paths,
            compile_database,
            output_authority.canonical_root,
            pcm_manifest,
            entry_authority.clang_binary.identity,
            entry_authority.clang_scan_deps.identity,
            entry_authority.llvm_config.identity,
            entry_authority.compiler_module_inventory.identity,
            command.clang_sha256,
            command.clang_scan_deps_sha256,
            command.llvm_config_sha256,
            command.compiler_module_inventory_sha256,
            "production",
            manifest_authority);
    if (validated.compilation_signature
            != prepared.compilation_signature
        || validated.relocatable_compilation_signature
            != prepared.relocatable_compilation_signature
        || validated.compiler_profile != prepared.compiler_profile
        || validated.authority != prepared.authority
        || validated.owned_compile_database_sha256
            != sha256_hex(prepared.compile_database_json)
        || validated.manifest_sha256
            != sha256_hex(prepared.pcm_manifest_json)
        || validated.clang_binary_sha256
            != prepared.clang_binary_sha256
        || validated.clang_scan_deps_sha256
            != prepared.clang_scan_deps_sha256
        || validated.llvm_config_sha256
            != prepared.llvm_config_sha256
        || validated.compiler_module_inventory_sha256
            != prepared.compiler_module_inventory_sha256) {
        throw std::runtime_error(
            "on-disk PREPARE database or manifest differs from the built environment");
    }
    const std::vector<prepare_file_snapshot>
        completion_authority_snapshot =
            capture_prepare_file_snapshot(
                prepared.completion_authority_files,
                "completion authority dependency");
    write_prepare_output(
        command, output_authority, command.action_trace, action_trace);
    static_cast<void>(
        clang_structural::validate_preparation_action_trace(
            canonical_prepare_output(
                output_authority, command.action_trace),
            entry_authority.dependency,
            validated));
    write_prepare_output(
        command,
        output_authority,
        command.depfile,
        depfile);
    invoke_prepare_publication_test_checkpoint(
        prepare_publication_checkpoint::before_completion_stamp);
    revalidate_prepare_entry_authority(
        command, output_authority, entry_authority, &prepared);
    if (capture_prepare_file_snapshot(
            prepared.completion_authority_files,
            "completion authority dependency")
        != completion_authority_snapshot) {
        throw std::runtime_error(
            "PREPARE dependency identity or bytes changed before completion");
    }
    require_prepare_document(
        command,
        output_authority,
        command.compile_database_output,
        prepared.compile_database_json,
        "compile database");
    require_prepare_document(
        command,
        output_authority,
        command.pcm_manifest,
        prepared.pcm_manifest_json,
        "PCM manifest");
    require_prepare_document(
        command,
        output_authority,
        command.action_trace,
        action_trace,
        "action trace");
    require_prepare_document(
        command,
        output_authority,
        command.depfile,
        depfile,
        "depfile");
    const std::string stamp =
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
            + "\nprepare_action_trace_sha256="
            + sha256_hex(action_trace)
            + "\n";
    write_prepare_output(
        command,
        output_authority,
        command.stamp,
        stamp);
    stamp_guard.commit();
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
    require_regular_file(
        command.prepare_action_trace, "PREPARE action trace");
    require_regular_file(command.prepare_stamp, "PREPARE stamp");
    const std::string preparation_action_trace_document =
        read_file(command.prepare_action_trace);
    validate_preparation_action_trace_stamp_binding(
        read_file(command.prepare_stamp),
        preparation_action_trace_document);
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
    const clang_structural::preparation_action_trace_validation
        preparation_actions =
            clang_structural::validate_preparation_action_trace(
                command.prepare_action_trace,
                dependency_authority,
                prepared);
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
        .body_bearing_compiler_invocations =
            preparation_actions.summary
                .canonical_semantic_frontend_actions,
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
                command.prepare_action_trace,
                command.prepare_stamp,
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

int execute_prepare_contract(prepare_command_line command) {
    return detail::execute_prepare(std::move(command));
}

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
