module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

export module bioseq.codegen.highway:validator;

import :model;
import bioseq.codegen.clang_structural;
import bioseq.codegen.namespace_materialization;
import bioseq.codegen.support;

export namespace bioseq::codegen::highway {

namespace detail {

[[nodiscard]] inline bool identifier(std::string_view value) noexcept {
    if (value.empty()
        || !((value.front() >= 'a' && value.front() <= 'z')
             || (value.front() >= 'A' && value.front() <= 'Z')
             || value.front() == '_')) {
        return false;
    }
    return std::ranges::all_of(value, [](char character) {
        return (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '_';
    });
}

[[nodiscard]] inline bool domain_name(std::string_view value) noexcept {
    if (value.empty() || value.front() < 'a' || value.front() > 'z') {
        return false;
    }
    return std::ranges::all_of(value, [](const char character) {
        return (character >= 'a' && character <= 'z')
            || (character >= '0' && character <= '9')
            || character == '_';
    });
}

[[nodiscard]] inline bool module_name(std::string_view value) noexcept {
    if (value.empty() || value.front() == '.' || value.back() == '.'
        || value.contains("..")) {
        return false;
    }
    std::size_t begin = 0;
    while (begin < value.size()) {
        const std::size_t separator = value.find_first_of(".:", begin);
        const std::string_view component = value.substr(
            begin,
            separator == std::string_view::npos
                ? value.size() - begin
                : separator - begin);
        if (!identifier(component)) return false;
        if (separator == std::string_view::npos) break;
        if (value[separator] == ':'
            && (separator + 1 == value.size()
                || value[separator + 1] == ':')) {
            return false;
        }
        begin = separator + 1;
    }
    return true;
}

[[nodiscard]] inline bool lowercase_hex_digest(std::string_view value) noexcept {
    return value.size() == 64
        && std::ranges::all_of(value, [](char character) {
               return (character >= '0' && character <= '9')
                   || (character >= 'a' && character <= 'f');
           });
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
    return error ? absolute.lexically_normal() : canonical;
}

[[nodiscard]] inline bool path_beneath(
    const std::filesystem::path& candidate,
    const std::filesystem::path& root) {
    const std::filesystem::path relative =
        path_identity(candidate).lexically_relative(path_identity(root));
    return !relative.empty() && relative != "."
        && std::ranges::none_of(
            relative,
            [](const std::filesystem::path& component) {
                return component == ".." || component == ".";
            });
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

[[nodiscard]] inline std::size_t occurrence_count(
    std::string_view text,
    std::string_view needle) noexcept {
    if (needle.empty()) return 0;
    std::size_t result = 0;
    std::size_t cursor = 0;
    while ((cursor = text.find(needle, cursor)) != std::string_view::npos) {
        ++result;
        cursor += needle.size();
    }
    return result;
}

/// Return whether `character` is whitespace permitted inside C++ spellings.
[[nodiscard]] inline bool ascii_space(const char character) noexcept {
    return character == ' ' || character == '\t' || character == '\r'
        || character == '\n';
}

/// Remove insignificant ASCII whitespace before validating a slot expression.
[[nodiscard]] inline std::string compact_cpp_spelling(
    const std::string_view spelling) {
    std::string result;
    result.reserve(spelling.size());
    for (const char character : spelling) {
        if (!ascii_space(character)) result.push_back(character);
    }
    return result;
}

/// Return whether `character` can occur after the first identifier character.
[[nodiscard]] inline bool identifier_character(const char character) noexcept {
    return (character >= 'a' && character <= 'z')
        || (character >= 'A' && character <= 'Z')
        || (character >= '0' && character <= '9') || character == '_';
}

/// Extract a simple terminal identifier while ignoring nested template scopes.
[[nodiscard]] inline std::string_view terminal_identifier(
    const std::string_view qualified_name) noexcept {
    std::size_t separator = std::string_view::npos;
    std::size_t angle_depth = 0u;
    for (std::size_t index = 0; index < qualified_name.size(); ++index) {
        if (qualified_name[index] == '<') {
            ++angle_depth;
        } else if (qualified_name[index] == '>') {
            if (angle_depth != 0u) --angle_depth;
        } else if (angle_depth == 0u && qualified_name[index] == ':'
                   && index + 1u < qualified_name.size()
                   && qualified_name[index + 1u] == ':') {
            separator = index;
            ++index;
        }
    }
    const std::string_view terminal = separator == std::string_view::npos
        ? qualified_name
        : qualified_name.substr(separator + 2u);
    std::size_t length = 0;
    while (length < terminal.size()
           && identifier_character(terminal[length])) {
        ++length;
    }
    if (length == 0u || !identifier(terminal.substr(0u, length))) return {};
    return terminal.substr(0u, length);
}

struct parsed_slot_callable final {
    std::string qualified_name;
    std::string terminal_name;
};

/// Parse the fail-closed address-of callable form accepted by SLOT_RECORDS.
[[nodiscard]] inline parsed_slot_callable parse_slot_callable(
    const slot_spec& slot) {
    std::string expression = compact_cpp_spelling(slot.expression);
    const std::string label = slot.family_field + "." + slot.slot_field;
    if (!expression.starts_with('&')) {
        throw std::runtime_error(
            "Highway slot '" + label
            + "' must be an address-of qualified callable expression");
    }
    expression.erase(0u, 1u);
    if (expression.starts_with("::")) expression.erase(0u, 2u);

    constexpr std::string_view target_marker = "{target_namespace}";
    const std::size_t marker = expression.find(target_marker);
    if (marker == std::string::npos
        || expression.find(target_marker, marker + target_marker.size())
            != std::string::npos
        || (marker != 0u
            && (marker < 2u || expression.substr(marker - 2u, 2u) != "::"))) {
        throw std::runtime_error(
            "Highway slot '" + label
            + "' has no unique qualified target-namespace component");
    }

    const std::size_t suffix_begin = marker + target_marker.size();
    if (expression.substr(suffix_begin, 2u) != "::") {
        throw std::runtime_error(
            "Highway slot '" + label
            + "' target namespace must qualify a callable");
    }
    const std::string_view suffix{expression.data() + suffix_begin + 2u,
                                  expression.size() - suffix_begin - 2u};
    if (suffix.empty()) {
        throw std::runtime_error(
            "Highway slot '" + label + "' has no terminal callable");
    }

    std::size_t terminal_begin = 0u;
    std::size_t angle_depth = 0u;
    for (std::size_t index = 0; index < suffix.size(); ++index) {
        if (suffix[index] == '<') {
            ++angle_depth;
        } else if (suffix[index] == '>') {
            if (angle_depth == 0u) {
                throw std::runtime_error(
                    "Highway slot '" + label
                    + "' has an unbalanced callable template-id");
            }
            --angle_depth;
        } else if (angle_depth == 0u && suffix[index] == ':'
                   && index + 1u < suffix.size()
                   && suffix[index + 1u] == ':') {
            terminal_begin = index + 2u;
            ++index;
        }
    }
    if (angle_depth != 0u) {
        throw std::runtime_error(
            "Highway slot '" + label
            + "' has an unbalanced callable template-id");
    }

    const std::string_view terminal = suffix.substr(terminal_begin);
    const std::string_view name = terminal_identifier(terminal);
    if (name.empty()) {
        throw std::runtime_error(
            "Highway slot '" + label
            + "' terminal callable must be a simple identifier");
    }
    const std::string_view remainder = terminal.substr(name.size());
    if (!remainder.empty()) {
        if (!remainder.starts_with('<') || !remainder.ends_with('>')) {
            throw std::runtime_error(
                "Highway slot '" + label
                + "' terminal callable may only add a template argument list");
        }
        std::size_t depth = 0u;
        for (std::size_t index = 0; index < remainder.size(); ++index) {
            const char character = remainder[index];
            if (character == '<') {
                ++depth;
            } else if (character == '>') {
                if (depth == 0u) {
                    throw std::runtime_error(
                        "Highway slot '" + label
                        + "' has an unbalanced callable template-id");
                }
                --depth;
                if (depth == 0u && index + 1u != remainder.size()) {
                    throw std::runtime_error(
                        "Highway slot '" + label
                        + "' has tokens after its callable template-id");
                }
            } else if (character == ';' || character == '{'
                       || character == '}') {
                throw std::runtime_error(
                    "Highway slot '" + label
                    + "' has forbidden tokens in its callable template-id");
            }
        }
        if (depth != 0u) {
            throw std::runtime_error(
                "Highway slot '" + label
                + "' has an unbalanced callable template-id");
        }
    }

    return parsed_slot_callable{
        .qualified_name = std::move(expression),
        .terminal_name = std::string{name},
    };
}

[[nodiscard]] inline std::string callable_kind_name(
    clang_structural::callable_kind kind) {
    using enum clang_structural::callable_kind;
    switch (kind) {
        case function: return "function";
        case member_function: return "member_function";
        case constructor: return "constructor";
        case destructor: return "destructor";
        case operator_function: return "operator_function";
        case conversion_function: return "conversion_function";
        case lambda: return "lambda";
    }
    throw std::runtime_error("unknown structural callable kind");
}

inline void validate_no_forbidden_source_macros(
    const std::filesystem::path& path,
    std::string_view source) {
    static constexpr std::array<std::string_view, 18> forbidden{
        "HWY_TARGET_INCLUDE",
        "hwy/foreach_target.h",
        "HWY_ONCE",
        "HWY_BEFORE_NAMESPACE",
        "HWY_AFTER_NAMESPACE",
        "HWY_EXPORT",
        "HWY_DYNAMIC_DISPATCH",
        "HWY_DYNAMIC_POINTER",
        "HWY_STATIC_DISPATCH",
        "HWY_TARGETS",
        "GetChosenTarget",
        "SupportedTargets",
        "SupportedAndGeneratedTargets",
        "SetSupportedTargetsForTest",
        "DisableTargets",
        "select_highway_target_once",
        "registered_highway_dispatch_info",
        "STATIC_FACADE",
    };
    for (std::string_view spelling : forbidden) {
        if (source.contains(spelling)) {
            throw std::runtime_error(
                path.string() + ": registered Highway source contains forbidden '"
                + std::string{spelling} + "'");
        }
    }

    const auto trim_left = [](std::string_view value) noexcept {
        const std::size_t first = value.find_first_not_of(" \t");
        return first == std::string_view::npos
            ? std::string_view{}
            : value.substr(first);
    };
    const auto directive_argument = [&](std::string_view line,
                                        std::string_view directive) noexcept {
        line = trim_left(line);
        if (line.empty() || line.front() != '#') return std::string_view{};
        line = trim_left(line.substr(1u));
        if (!line.starts_with(directive)) return std::string_view{};
        line.remove_prefix(directive.size());
        if (!line.empty() && line.front() != ' ' && line.front() != '\t') {
            return std::string_view{};
        }
        return trim_left(line);
    };

    for (std::size_t begin = 0u; begin <= source.size();) {
        const std::size_t end = source.find_first_of("\r\n", begin);
        const std::string_view line = source.substr(
            begin,
            end == std::string_view::npos ? source.size() - begin : end - begin);

        const std::string_view include = directive_argument(line, "include");
        if (!include.empty()
            && (include.front() == '<' || include.front() == '"')
            && include.contains(".cppm")) {
            throw std::runtime_error(
                path.string()
                + ": registered Highway source may not textually include a .cppm");
        }

        std::string_view definition = directive_argument(line, "define");
        const std::size_t name_end = definition.find_first_of(" \t(");
        const std::string_view name = definition.substr(0u, name_end);
        if (name == "module" || name == "target") {
            throw std::runtime_error(
                path.string() + ": registered Highway source contains forbidden '#define "
                + std::string{name} + "'");
        }

        if (end == std::string_view::npos) break;
        begin = end + 1u;
        if (begin < source.size() && source[end] == '\r'
            && source[begin] == '\n') {
            ++begin;
        }
    }
}

inline void validate_slot_expression(const slot_spec& slot) {
    if (slot.expression.empty() || slot.expression.contains('\n')
        || slot.expression.contains('\r') || slot.expression.contains('\t')) {
        throw std::runtime_error(
            "Highway slot '" + slot.family_field + "." + slot.slot_field
            + "' has an empty or multiline expression");
    }
    if (occurrence_count(slot.expression, "{target_namespace}") != 1) {
        throw std::runtime_error(
            "Highway slot '" + slot.family_field + "." + slot.slot_field
            + "' expression must contain {target_namespace} exactly once");
    }
    static constexpr std::array<std::string_view, 11> forbidden{
        "HWY_DYNAMIC_DISPATCH",
        "HWY_DYNAMIC_POINTER",
        "HWY_STATIC_DISPATCH",
        "HWY_EXPORT",
        "HWY_BEFORE_NAMESPACE",
        "HWY_AFTER_NAMESPACE",
        "foreach_target",
        "GetChosenTarget",
        "SupportedTargets",
        "select_highway_target_once",
        "registered_highway_dispatch_info",
    };
    static_assert(!forbidden.back().empty());
    for (std::string_view spelling : forbidden) {
        if (slot.expression.contains(spelling)) {
            throw std::runtime_error(
                "Highway slot expression contains forbidden '"
                + std::string{spelling} + "'");
        }
    }
}

}  // namespace detail

[[nodiscard]] inline domain_topology_manifest_input
parse_domain_topology_manifest_argument(const std::string_view value) {
    const std::size_t separator = value.find('=');
    if (separator == std::string_view::npos || separator == 0u
        || separator + 1u == value.size()) {
        throw std::runtime_error(
            "Highway domain topology must use <name>=<absolute-manifest-path>");
    }
    domain_topology_manifest_input result{
        .name = std::string{value.substr(0u, separator)},
        .path = std::filesystem::path{
            std::string{value.substr(separator + 1u)}},
    };
    if (!detail::domain_name(result.name)) {
        throw std::runtime_error(
            "invalid Highway domain topology name '" + result.name + "'");
    }
    if (!result.path.is_absolute()) {
        throw std::runtime_error(
            "Highway domain topology path must be absolute: "
            + result.path.string());
    }
    return result;
}

[[nodiscard]] inline std::vector<domain_topology_provenance>
load_domain_topology_manifests(
    std::vector<domain_topology_manifest_input>& inputs) {
    std::set<std::string, std::less<>> names;
    std::set<std::filesystem::path> lexical_paths;
    for (const domain_topology_manifest_input& input : inputs) {
        if (!detail::domain_name(input.name)
            || !names.insert(input.name).second) {
            throw std::runtime_error(
                "duplicate or invalid Highway domain topology name '"
                + input.name + "'");
        }
        if (!input.path.is_absolute()) {
            throw std::runtime_error(
                "Highway domain topology path must be absolute: "
                + input.path.string());
        }
        if (!lexical_paths.insert(input.path.lexically_normal()).second) {
            throw std::runtime_error(
                "duplicate Highway domain topology path: "
                + input.path.string());
        }
    }

    std::vector<domain_topology_provenance> result;
    result.reserve(inputs.size());
    std::vector<std::filesystem::path> physical_paths;
    for (domain_topology_manifest_input& input : inputs) {
        // Parse the original physical spelling first. The shared topology
        // authority rejects symlink chains and unknown schemas; canonicalizing
        // before this call would conceal path aliases.
        const auto topology =
            ::bioseq::codegen::namespace_materialization::
                read_configure_topology_provenance(
                input.path);
        input.path = detail::path_identity(input.path);
        for (const std::filesystem::path& existing : physical_paths) {
            std::error_code error;
            const bool equivalent =
                std::filesystem::equivalent(input.path, existing, error);
            if (error) {
                throw std::runtime_error(
                    "cannot authenticate Highway domain topology path: "
                    + input.path.string());
            }
            if (equivalent) {
                throw std::runtime_error(
                    "duplicate Highway domain topology path: "
                    + input.path.string());
            }
        }
        detail::require_regular_file(
            input.path, "domain topology manifest");
        physical_paths.push_back(input.path);
        result.push_back(domain_topology_provenance{
            .name = input.name,
            .path = input.path,
            .topology_digest = topology.topology_digest,
            .content_sha256 = topology.content_sha256,
        });
    }
    return result;
}

inline void validate_domain_topology_authority(
    const std::vector<domain_topology_expectation>& expected,
    const std::vector<domain_topology_provenance>& actual,
    const std::string_view owner) {
    std::set<std::string, std::less<>> names;
    std::set<std::filesystem::path> paths;
    for (const domain_topology_expectation& topology : expected) {
        if (!detail::domain_name(topology.name)
            || !names.insert(topology.name).second
            || !topology.path.is_absolute()
            || detail::path_identity(topology.path) != topology.path
            || !paths.insert(topology.path).second
            || !detail::lowercase_hex_digest(topology.topology_digest)
            || !detail::lowercase_hex_digest(topology.content_sha256)) {
            throw std::runtime_error(
                std::string{owner}
                + ": invalid ordered domain-topology authority record '"
                + topology.name + "'");
        }
    }
    if (expected.size() != actual.size()) {
        throw std::runtime_error(
            std::string{owner}
            + ": domain-topology authority count disagrees with CLI inputs");
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const domain_topology_expectation& configured = expected[index];
        const domain_topology_provenance& observed = actual[index];
        if (configured.name != observed.name
            || configured.path != observed.path
            || configured.topology_digest != observed.topology_digest
            || configured.content_sha256 != observed.content_sha256) {
            throw std::runtime_error(
                std::string{owner} + ": domain-topology authority '"
                + configured.name
                + "' changed or was reordered; reconfigure is required");
        }
    }
}

inline void validate_command_line(command_line& command) {
    command.target_manifest_path = detail::path_identity(command.target_manifest_path);
    command.source_manifest_path = detail::path_identity(command.source_manifest_path);
    command.compile_database = detail::path_identity(command.compile_database);
    command.pcm_manifest = detail::path_identity(command.pcm_manifest);
    command.pcm_root = detail::path_identity(command.pcm_root);
    command.clang_binary = detail::path_identity(command.clang_binary);
    command.clang_scan_deps = detail::path_identity(command.clang_scan_deps);
    command.llvm_config = detail::path_identity(command.llvm_config);
    command.compiler_module_inventory =
        detail::path_identity(command.compiler_module_inventory);
    command.output_root = detail::path_identity(command.output_root);
    command.artifact_manifest = detail::path_identity(command.artifact_manifest);
    command.stamp = detail::path_identity(command.stamp);

    detail::require_regular_file(command.target_manifest_path, "target manifest");
    detail::require_regular_file(command.source_manifest_path, "source manifest");
    detail::require_regular_file(command.compile_database, "compile database");
    detail::require_regular_file(command.pcm_manifest, "PCM manifest");
    detail::require_regular_file(command.clang_binary, "Clang binary");
    detail::require_regular_file(command.clang_scan_deps, "clang-scan-deps binary");
    detail::require_regular_file(command.llvm_config, "llvm-config binary");
    detail::require_regular_file(
        command.compiler_module_inventory, "compiler module inventory");
    std::error_code error;
    if (!std::filesystem::is_directory(command.pcm_root, error) || error) {
        throw std::runtime_error(
            "PCM root is not a directory: " + command.pcm_root.string());
    }
    if (command.output_root == command.artifact_manifest
        || command.output_root == command.stamp
        || command.artifact_manifest == command.stamp
        || !detail::path_beneath(command.artifact_manifest, command.output_root)
        || !detail::path_beneath(command.stamp, command.output_root)) {
        throw std::runtime_error(
            "artifact manifest and stamp must be distinct files beneath the owned output root");
    }
    if (command.artifact_manifest.extension() != ".json") {
        throw std::runtime_error("Highway artifact manifest must use .json");
    }
    if (!detail::lowercase_hex_digest(command.clang_sha256)
        || !detail::lowercase_hex_digest(command.clang_scan_deps_sha256)
        || !detail::lowercase_hex_digest(command.llvm_config_sha256)
        || !detail::lowercase_hex_digest(
            command.compiler_module_inventory_sha256)) {
        throw std::runtime_error(
            "Highway generation requires lowercase SHA-256 tool identities");
    }
}

inline void validate_prepare_command_line(prepare_command_line& command) {
    if (!command.signature_source.is_absolute()) {
        throw std::runtime_error(
            "Highway signature source must be an absolute path");
    }
    command.source_manifest_path =
        detail::path_identity(command.source_manifest_path);
    command.signature_source =
        detail::path_identity(command.signature_source);
    command.raw_compile_database =
        detail::path_identity(command.raw_compile_database);
    command.clang_binary = detail::path_identity(command.clang_binary);
    command.clang_scan_deps = detail::path_identity(command.clang_scan_deps);
    command.llvm_config = detail::path_identity(command.llvm_config);
    command.compiler_module_inventory =
        detail::path_identity(command.compiler_module_inventory);
    command.pcm_root = detail::path_identity(command.pcm_root);
    command.compile_database_output =
        detail::path_identity(command.compile_database_output);
    command.pcm_manifest = detail::path_identity(command.pcm_manifest);
    command.depfile = detail::path_identity(command.depfile);
    command.stamp = detail::path_identity(command.stamp);

    detail::require_regular_file(command.source_manifest_path, "source manifest");
    detail::require_regular_file(command.signature_source, "signature source");
    detail::require_regular_file(command.raw_compile_database, "raw compile database");
    detail::require_regular_file(command.clang_binary, "Clang binary");
    detail::require_regular_file(command.clang_scan_deps, "clang-scan-deps binary");
    detail::require_regular_file(command.llvm_config, "llvm-config binary");
    detail::require_regular_file(
        command.compiler_module_inventory, "compiler module inventory");
    if (!detail::lowercase_hex_digest(command.clang_sha256)
        || !detail::lowercase_hex_digest(command.clang_scan_deps_sha256)
        || !detail::lowercase_hex_digest(command.llvm_config_sha256)
        || !detail::lowercase_hex_digest(
            command.compiler_module_inventory_sha256)) {
        throw std::runtime_error(
            "Highway preparation requires lowercase SHA-256 tool identities");
    }
    const std::array<std::filesystem::path, 4> outputs{
        command.compile_database_output,
        command.pcm_manifest,
        command.depfile,
        command.stamp,
    };
    std::set<std::filesystem::path> unique_outputs;
    const std::filesystem::path authority_root = command.pcm_root.parent_path();
    for (const std::filesystem::path& output : outputs) {
        if (!detail::path_beneath(output, authority_root)
            || !unique_outputs.insert(output).second) {
            throw std::runtime_error(
                "Highway preparation outputs must be distinct files beneath the PCM authority root");
        }
    }
    if (command.compile_database_output.extension() != ".json"
        || command.pcm_manifest.extension() != ".json") {
        throw std::runtime_error(
            "Highway owned compile database and PCM manifest must use .json");
    }
}

inline void validate_complete_table_target_policy(
    const target_manifest& targets) {
    const bool contains_scalar = std::ranges::any_of(
        targets.targets,
        [](const target_spec& target) {
            return target.token == "HWY_SCALAR";
        });
    const bool contains_emu128 = std::ranges::any_of(
        targets.targets,
        [](const target_spec& target) {
            return target.token == "HWY_EMU128";
        });
    if (contains_scalar) {
        throw std::runtime_error(
            "Highway target inventory must exclude HWY_SCALAR because the "
            "complete kernel table requires at least eight SIMD lanes");
    }
    if (!contains_emu128) {
        throw std::runtime_error(
            "Highway target inventory must retain HWY_EMU128 as the portable "
            "complete-table fallback");
    }
}

inline void validate_inventories(
    const target_manifest& targets,
    const source_manifest& sources) {
    if (!detail::lowercase_hex_digest(targets.policy_digest)
        || !detail::lowercase_hex_digest(targets.highway_digest)
        || targets.highway_version.empty()) {
        throw std::runtime_error(
            "Highway target manifest has invalid policy/Highway provenance");
    }

    validate_domain_topology_authority(
        targets.domain_topologies,
        sources.domain_topologies,
        "Highway target/source manifests");

    std::string policy_block;
    std::set<std::string, std::less<>> policy_names;
    for (const auto& [name, value] : targets.policy_definitions) {
        if (!detail::identifier(name) || value.contains('\n')
            || value.contains('\r') || value.contains('\t')
            || value.contains('#') || !policy_names.insert(name).second) {
            throw std::runtime_error(
                "invalid or duplicate Highway policy definition '" + name + "'");
        }
        policy_block += "#define " + name + " " + value + "\n";
    }
    if (sha256_hex(policy_block) != targets.policy_digest) {
        throw std::runtime_error(
            "Highway policy digest does not match the canonical definition block");
    }

    std::map<std::uint64_t, std::string> namespaces_by_bit;
    std::map<std::string, std::uint64_t, std::less<>> bits_by_namespace;
    std::set<std::string, std::less<>> tokens;
    std::set<std::string, std::less<>> target_modules;
    for (const target_spec& target : targets.targets) {
        if (!detail::identifier(target.platform) || !detail::identifier(target.slug)
            || !detail::identifier(target.token)
            || !detail::identifier(target.name_space)
            || !target.token.starts_with("HWY_")
            || !target.name_space.starts_with("N_")
            || target.bit == 0u
            || (target.bit & (target.bit - 1U)) != 0) {
            throw std::runtime_error(
                "invalid Highway target record for token '" + target.token + "'");
        }
        if (!namespaces_by_bit.emplace(target.bit, target.name_space).second) {
            throw std::runtime_error(
                "Highway target inventory contains a duplicate or conflicting bit");
        }
        if (!bits_by_namespace.emplace(target.name_space, target.bit).second) {
            throw std::runtime_error(
                "Highway target inventory contains a duplicate or conflicting namespace");
        }
        if (!tokens.insert(target.token).second
            || !target_modules.insert(target.platform + "." + target.slug).second) {
            throw std::runtime_error(
                "duplicate Highway target token or generated module identity");
        }
    }
    validate_complete_table_target_policy(targets);
    if (!detail::identifier(targets.static_target_token)
        || !tokens.contains(targets.static_target_token)) {
        throw std::runtime_error(
            "Highway static target is absent from the generated inventory: '"
            + targets.static_target_token + "'");
    }

    if (!detail::module_name(sources.contract_module)
        || !detail::module_name(sources.dispatch_module)
        || sources.contract_module == sources.dispatch_module) {
        throw std::runtime_error("invalid Highway contract or dispatch module name");
    }

    std::map<std::string, const source_spec*, std::less<>> by_key;
    std::set<std::filesystem::path> paths;
    std::set<std::string, std::less<>> modules;
    std::set<std::string, std::less<>> partitions;
    for (const source_spec& source : sources.sources) {
        if (!detail::identifier(source.key) || !detail::identifier(source.partition)
            || !detail::identifier(source.family)
            || !detail::module_name(source.module_name)
            || source.path.extension() != ".cppm" || !source.path.is_absolute()) {
            throw std::runtime_error(
                "invalid registered Highway source '" + source.key + "'");
        }
        detail::require_regular_file(source.path, "registered Highway source");
        const std::filesystem::path identity = detail::path_identity(source.path);
        if (!identity.generic_string().contains("/main/bioseq/")) {
            throw std::runtime_error(
                "registered Highway source is outside main/bioseq: "
                + source.path.string());
        }
        if (!by_key.emplace(source.key, &source).second
            || !paths.insert(identity).second
            || !modules.insert(source.module_name).second
            || !partitions.insert(source.partition).second) {
            throw std::runtime_error(
                "duplicate Highway source key, path, module, or partition for '"
                + source.key + "'");
        }
        std::set<std::string, std::less<>> local_dependencies;
        for (const std::string& dependency : source.dependencies) {
            if (!detail::identifier(dependency) || dependency == source.key
                || !local_dependencies.insert(dependency).second) {
                throw std::runtime_error(
                    "invalid or duplicate dependency '" + dependency
                    + "' of Highway source '" + source.key + "'");
            }
        }
    }
    for (const source_spec& source : sources.sources) {
        for (const std::string& dependency : source.dependencies) {
            if (!by_key.contains(dependency)) {
                throw std::runtime_error(
                    "Highway source '" + source.key
                    + "' depends on unknown key '" + dependency + "'");
            }
        }
    }

    enum class visit_state : std::uint8_t { unseen, active, complete };
    std::map<std::string, visit_state, std::less<>> states;
    std::function<void(const source_spec&)> visit = [&](const source_spec& source) {
        visit_state& state = states[source.key];
        if (state == visit_state::active) {
            throw std::runtime_error(
                "cycle in Highway source dependencies at '" + source.key + "'");
        }
        if (state == visit_state::complete) return;
        state = visit_state::active;
        for (const std::string& dependency : source.dependencies) {
            visit(*by_key.at(dependency));
        }
        state = visit_state::complete;
    };
    for (const source_spec& source : sources.sources) visit(source);

    std::set<std::pair<std::string, std::string>> actual_slots;
    std::set<std::string, std::less<>> whole_families;
    std::set<std::string, std::less<>> partial_families;
    for (const slot_spec& slot : sources.slots) {
        if (!by_key.contains(slot.owner_key)
            || !detail::identifier(slot.family_field)
            || (slot.slot_field != "@" && !detail::identifier(slot.slot_field))
            || !actual_slots.emplace(slot.family_field, slot.slot_field).second) {
            throw std::runtime_error(
                "invalid, duplicate, or unowned Highway table slot '"
                + slot.family_field + "." + slot.slot_field + "'");
        }
        detail::validate_slot_expression(slot);
        (slot.slot_field == "@" ? whole_families : partial_families)
            .insert(slot.family_field);
    }
    for (const std::string& family : whole_families) {
        if (partial_families.contains(family)) {
            throw std::runtime_error(
                "Highway table family '" + family
                + "' mixes whole-family and member initializers");
        }
    }

    std::set<std::pair<std::string, std::string>> required_slots;
    for (const required_slot_spec& slot : sources.required_slots) {
        if (!detail::identifier(slot.family_field)
            || (slot.slot_field != "@" && !detail::identifier(slot.slot_field))
            || !required_slots.emplace(slot.family_field, slot.slot_field).second) {
            throw std::runtime_error(
                "invalid or duplicate required Highway table slot");
        }
    }
    if (!required_slots.empty() && required_slots != actual_slots) {
        throw std::runtime_error(
            "Highway table ownership does not exactly cover required slots");
    }
    if (sources.require_complete_table
        && (required_slots.empty() || actual_slots != required_slots)) {
        throw std::runtime_error(
            "complete Highway table generation requires exact non-empty slot coverage");
    }
}

[[nodiscard]] inline std::vector<extracted_source> extract_sources(
    const source_manifest& inventory,
    const clang_structural::structural_index& structural,
    const clang_structural::module_graph& graph) {
    std::map<std::filesystem::path,
             const clang_structural::translation_unit*> units;
    for (const auto& unit : structural.translation_units) {
        if (!units.emplace(detail::path_identity(unit.source_path), &unit).second) {
            throw std::runtime_error(
                "structural scan returned a duplicate translation unit");
        }
    }
    std::map<std::filesystem::path,
             const clang_structural::module_graph_unit*> graph_units;
    for (const auto& unit : graph.units) {
        if (!graph_units.emplace(
                detail::path_identity(std::filesystem::path{unit.source_path}),
                &unit).second) {
            throw std::runtime_error("module scan returned a duplicate source unit");
        }
    }
    std::map<std::string, const source_spec*, std::less<>> sources_by_module;
    for (const source_spec& source : inventory.sources) {
        sources_by_module.emplace(source.module_name, &source);
    }

    std::vector<extracted_source> result;
    result.reserve(inventory.sources.size());
    for (const source_spec& source : inventory.sources) {
        const std::filesystem::path identity = detail::path_identity(source.path);
        const auto unit_position = units.find(identity);
        const auto graph_position = graph_units.find(identity);
        if (unit_position == units.end() || graph_position == graph_units.end()) {
            throw std::runtime_error(
                source.path.string()
                + ": missing structural or module-graph scan result");
        }
        const clang_structural::translation_unit& unit = *unit_position->second;
        const clang_structural::module_graph_unit& graph_unit = *graph_position->second;
        if (unit.module_name != source.module_name
            || graph_unit.provided_module != source.module_name) {
            throw std::runtime_error(
                source.path.string()
                + ": module declaration does not match source registry '"
                + source.module_name + "'");
        }
        if (unit.marked_namespaces.size() != 1) {
            throw std::runtime_error(
                source.path.string()
                + ": registered source must contain exactly one marked namespace");
        }
        const clang_structural::marked_namespace_record& marked =
            unit.marked_namespaces.front();
        if (marked.marker.qualified_name != "bioseq::simd"
            || marked.marker.vendor_namespace != "bioseq"
            || marked.marker.name != "simd"
            || !marked.marker.argument_text.empty()
            || marked.marker.placement
                != clang_structural::attribute_placement::namespace_head
            || !marked.qualified_name.ends_with("::target")
            || marked.has_explicit_export
            || marked.crosses_preprocessor_region) {
            throw std::runtime_error(
                source.path.string()
                + ": invalid [[bioseq::simd]] namespace contract");
        }
        const std::string content = read_file(source.path);
        detail::validate_no_forbidden_source_macros(source.path, content);
        const auto exact_text = [&](const clang_structural::source_range& range) {
            const std::size_t begin = range.begin.byte_offset;
            const std::size_t end = range.end.byte_offset;
            if (begin > end || end > content.size()) return std::string{};
            return content.substr(begin, end - begin);
        };
        if (exact_text(marked.keyword_range) != "namespace"
            || exact_text(marked.name_range) != "target") {
            throw std::runtime_error(
                source.path.string()
                + ": marked namespace must use the simple name 'target'");
        }
        const clang_structural::lexical_token* previous_token = nullptr;
        for (const clang_structural::lexical_token& token : unit.tokens) {
            const std::size_t offset = token.range.begin.byte_offset;
            if (token.range.end.byte_offset
                <= marked.keyword_range.begin.byte_offset) {
                previous_token = &token;
            }
            if (offset >= marked.name_range.end.byte_offset
                && offset < marked.open_brace_range.begin.byte_offset) {
                throw std::runtime_error(
                    source.path.string()
                    + ": marked namespace must not use nested-name shorthand");
            }
        }
        if (previous_token != nullptr && previous_token->spelling == "inline") {
            throw std::runtime_error(
                source.path.string() + ": marked namespace must not be inline");
        }
        const std::size_t body_begin = marked.body_range.begin.byte_offset;
        const std::size_t body_end = marked.body_range.end.byte_offset;
        if (body_begin > body_end || body_end > content.size()
            || marked.open_brace_range.end.byte_offset != body_begin
            || marked.close_brace_range.begin.byte_offset != body_end) {
            throw std::runtime_error(
                source.path.string() + ": invalid marked namespace byte geometry");
        }
        const std::size_t suffix_size = std::string_view{"::target"}.size();
        extracted_source extracted{
            .source = source,
            .enclosing_namespace = marked.qualified_name.substr(
                0, marked.qualified_name.size() - suffix_size),
            .body = content.substr(body_begin, body_end - body_begin),
            .body_first_line = marked.body_range.begin.line,
            .source_sha256 = unit.source_sha256,
        };

        std::set<std::size_t> insertion_offsets;
        for (const clang_structural::callable_insertion_record& callable
             : marked.callables) {
            const std::size_t insertion =
                callable.hwy_attr_insertion.byte_offset;
            const bool lambda = callable.kind
                == clang_structural::callable_kind::lambda;
            const bool valid_lambda = !lambda
                || (insertion
                        >= callable.declaration_range.begin.byte_offset
                    && insertion <= callable.body_range.begin.byte_offset);
            const bool valid_named_callable = lambda
                || (insertion >= callable.declaration_range.begin.byte_offset
                    && insertion < callable.body_range.begin.byte_offset);
            if (insertion < body_begin || insertion >= body_end
                || !valid_lambda || !valid_named_callable
                || !insertion_offsets.insert(insertion).second) {
                throw std::runtime_error(
                    source.path.string()
                    + ": callable has no unique proven HWY_ATTR insertion point");
            }
            extracted.callables.push_back(callable_insertion{
                .body_relative_offset = insertion - body_begin,
                .qualified_name = callable.qualified_name,
                .kind = detail::callable_kind_name(callable.kind),
            });
        }
        std::ranges::sort(
            extracted.callables,
            {},
            [](const callable_insertion& callable) {
                return callable.body_relative_offset;
            });

        std::set<std::string, std::less<>> actual_target_dependencies;
        std::set<std::string, std::less<>> neutral_imports;
        const auto consume_import = [&](const std::string& imported_module) {
            if (const auto target = sources_by_module.find(imported_module);
                target != sources_by_module.end()) {
                actual_target_dependencies.insert(target->second->key);
            } else {
                if (imported_module.starts_with("bioseq.highway.sources.")) {
                    throw std::runtime_error(
                        source.path.string()
                        + ": imports unregistered target source module '"
                        + imported_module + "'");
                }
                if (imported_module != source.module_name) {
                    neutral_imports.insert(imported_module);
                }
            }
        };
        for (const std::string& imported : graph_unit.exported_imports) {
            consume_import(imported);
        }
        for (const std::string& imported : graph_unit.internal_imports) {
            consume_import(imported);
        }
        const std::set<std::string, std::less<>> declared_dependencies{
            source.dependencies.begin(), source.dependencies.end()};
        if (actual_target_dependencies != declared_dependencies) {
            const auto describe = [](const auto& values) {
                std::string result{"["};
                for (const std::string& value : values) {
                    if (result.size() != 1u) result += ",";
                    result += value;
                }
                result += "]";
                return result;
            };
            throw std::runtime_error(
                source.path.string()
                + ": target imports do not exactly match registered DEPENDS keys"
                + " (declared=" + describe(declared_dependencies)
                + ", observed=" + describe(actual_target_dependencies) + ")");
        }
        extracted.neutral_imports.assign(
            neutral_imports.begin(), neutral_imports.end());
        result.push_back(std::move(extracted));
    }
    std::ranges::sort(
        result,
        {},
        [](const extracted_source& source) { return source.source.key; });
    return result;
}

/// Prove every table slot names a callable extracted from its registered owner.
inline void validate_slot_callable_ownership(
    const source_manifest& inventory,
    const std::vector<extracted_source>& extracted_sources) {
    std::map<std::string, const extracted_source*, std::less<>> by_owner;
    for (const extracted_source& source : extracted_sources) {
        if (!by_owner.emplace(source.source.key, &source).second) {
            throw std::runtime_error(
                "duplicate extracted Highway source owner '"
                + source.source.key + "'");
        }
    }

    for (const slot_spec& slot : inventory.slots) {
        const auto owner = by_owner.find(slot.owner_key);
        if (owner == by_owner.end()) {
            throw std::runtime_error(
                "Highway slot '" + slot.family_field + "." + slot.slot_field
                + "' has no extracted owner '" + slot.owner_key + "'");
        }

        const detail::parsed_slot_callable parsed =
            detail::parse_slot_callable(slot);
        const std::string expected_prefix =
            owner->second->enclosing_namespace + "::{target_namespace}::";
        if (!parsed.qualified_name.starts_with(expected_prefix)) {
            throw std::runtime_error(
                "Highway slot '" + slot.family_field + "." + slot.slot_field
                + "' callable is outside registered owner '" + slot.owner_key
                + "' namespace '" + owner->second->enclosing_namespace + "'");
        }

        std::set<std::string, std::less<>> owner_callables;
        for (const callable_insertion& callable : owner->second->callables) {
            const std::string_view terminal =
                detail::terminal_identifier(callable.qualified_name);
            if (!terminal.empty()) owner_callables.emplace(terminal);
        }
        if (!owner_callables.contains(parsed.terminal_name)) {
            std::string available;
            for (const std::string& callable : owner_callables) {
                if (!available.empty()) available += ", ";
                available += callable;
            }
            if (available.empty()) available = "<none>";
            throw std::runtime_error(
                "Highway slot '" + slot.family_field + "." + slot.slot_field
                + "' terminal callable '" + parsed.terminal_name
                + "' is not structurally defined by owner '" + slot.owner_key
                + "' (available: " + available + ")");
        }
    }
}

}  // namespace bioseq::codegen::highway
