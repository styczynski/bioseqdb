module;

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

export module bioseq.codegen.namespace_materialization:manifest;

import :model;
import :validator;
import bioseq.codegen.structural_analysis;
import bioseq.codegen.support;

export namespace bioseq::codegen::namespace_materialization {

inline constexpr std::string_view template_flatten_policy =
    "template_flatten";

/// The canonical policy-neutral ownership envelope and its self-seal.
struct rendered_publication_manifest {
    std::string json;
    std::string manifest_identity;
};

/// One path whose ownership was authenticated by an artifacts manifest.
struct owned_publication_artifact {
    std::filesystem::path relative_path;
    std::string content_sha256;
    publication_artifact_class artifact_class{
        publication_artifact_class::production_cppm};
};

/// Previous publication state accepted for stale-file authorization.
struct previous_publication {
    bool present{};
    std::string policy;
    std::string name;
    std::string topology_digest;
    std::string manifest_identity;
    std::string prior_manifest_identity;
    std::vector<owned_publication_artifact> artifacts;
};

/// Authenticated configure-time provenance consumed by structural and Highway
/// generation. `content_sha256` seals the exact configure snapshot bytes.
struct configure_topology_provenance {
    std::string generator_identity;
    std::string policy;
    emission_policy emission{emission_policy::selector_api};
    std::string name;
    std::filesystem::path source_path;
    std::string source_sha256;
    std::filesystem::path output_root;
    std::string output_module;
    std::string profile;
    std::string topology_digest;
    std::string content_sha256;
    std::size_t coordinate_count{};
};

/// Verifies the cross-output binding from the PREPARE completion stamp to the
/// exact authenticated action-trace bytes. The trace also has its own semantic
/// self-seal; this digest prevents independently substituting the extra custom
/// command output before GENERATE validates that seal and its PCM joins.
inline void validate_frontend_action_trace_stamp_binding(
    const std::string_view stamp_content,
    const std::string_view action_trace_content) {
    constexpr std::string_view schema{
        "bioseq_template_flatten_prepare_stamp\n"
        "revision=1\n"};
    constexpr std::string_view prefix{
        "prepare_action_trace_sha256="};
    if (!stamp_content.starts_with(schema)) {
        throw std::runtime_error(
            "PREPARE stamp action-trace binding has the wrong schema");
    }
    std::string sealed_digest;
    std::size_t binding_count = 0U;
    std::size_t line_begin = 0U;
    while (line_begin < stamp_content.size()) {
        const std::size_t line_end = stamp_content.find(
            '\n', line_begin);
        const std::string_view line = stamp_content.substr(
            line_begin,
            line_end == std::string_view::npos
                ? stamp_content.size() - line_begin
                : line_end - line_begin);
        if (line.starts_with(prefix)) {
            ++binding_count;
            sealed_digest = line.substr(prefix.size());
        }
        if (line_end == std::string_view::npos) break;
        line_begin = line_end + 1U;
    }
    const bool has_lowercase_sha256 = sealed_digest.size() == 64U
        && std::ranges::all_of(
            sealed_digest,
            [](const char value) {
                return (value >= '0' && value <= '9')
                    || (value >= 'a' && value <= 'f');
            });
    if (binding_count != 1U
        || !has_lowercase_sha256
        || sealed_digest != bioseq::common::sha256_hex(action_trace_content)) {
        throw std::runtime_error(
            "PREPARE stamp action-trace SHA-256 binding is missing, "
            "duplicated, malformed, or stale");
    }
}

/// Returns the source spelling used for an axis role in manifests.
[[nodiscard]] inline constexpr std::string_view axis_role_name(
    axis_role role) noexcept {
    switch (role) {
        case axis_role::identity: return "identity";
        case axis_role::shard: return "shard";
        case axis_role::external: return "external";
    }
    return "unknown";
}

/// Returns the stable spelling used for an artifact kind in manifests.
[[nodiscard]] inline constexpr std::string_view artifact_kind_name(
    materialization_artifact::kind kind) noexcept {
    switch (kind) {
        case materialization_artifact::kind::declaration_probe:
            return "declaration_probe";
        case materialization_artifact::kind::api_partition:
            return "api_partition";
        case materialization_artifact::kind::implementation_unit:
            return "implementation_unit";
        case materialization_artifact::kind::selector_partition:
            return "selector_partition";
        case materialization_artifact::kind::manifest:
            return "manifest";
    }
    return "unknown";
}

/// Returns the closed ownership class spelling used by every policy.
[[nodiscard]] inline constexpr std::string_view publication_class_name(
    publication_artifact_class value) noexcept {
    switch (value) {
        case publication_artifact_class::production_cppm:
            return "production_cppm";
        case publication_artifact_class::metadata_json:
            return "metadata_json";
    }
    return "unknown";
}

/// Returns the stable spelling used for a frontend validation class.
[[nodiscard]] inline constexpr std::string_view validation_class_name(
    validation_class value) noexcept {
    switch (value) {
        case validation_class::body_free: return "body_free";
        case validation_class::body_bearing: return "body_bearing";
    }
    return "unknown";
}

/// Returns the compiler-derived linkage spelling recorded for probe entities.
[[nodiscard]] inline constexpr std::string_view linkage_name(
    structural_analysis::entity_linkage value) noexcept {
    switch (value) {
        case structural_analysis::entity_linkage::none: return "none";
        case structural_analysis::entity_linkage::internal: return "internal";
        case structural_analysis::entity_linkage::module: return "module";
        case structural_analysis::entity_linkage::external: return "external";
    }
    return "unknown";
}

/// Returns the named/global-module attachment spelling for probe entities.
[[nodiscard]] inline constexpr std::string_view attachment_name(
    structural_analysis::module_attachment value) noexcept {
    switch (value) {
        case structural_analysis::module_attachment::global_module:
            return "global_module";
        case structural_analysis::module_attachment::named_module:
            return "named_module";
    }
    return "unknown";
}

namespace detail {

namespace fs = std::filesystem;

[[nodiscard]] inline std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    constexpr std::string_view digits = "0123456789abcdef";
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
                    result.push_back(digits[(character >> 4U) & 0x0fU]);
                    result.push_back(digits[character & 0x0fU]);
                } else {
                    result.push_back(static_cast<char>(character));
                }
        }
    }
    return result;
}

inline void write_string(
    std::ostringstream& output,
    std::string_view value) {
    output << '"' << json_escape(value) << '"';
}

inline void write_source_position(
    std::ostringstream& output,
    const structural_analysis::source_position& position) {
    output << "{\"byte\":" << position.byte_offset
           << ",\"line\":" << position.line
           << ",\"column\":" << position.column << '}';
}

inline void write_source_range(
    std::ostringstream& output,
    const structural_analysis::source_range& range) {
    output << "{\"begin\":";
    write_source_position(output, range.begin);
    output << ",\"end\":";
    write_source_position(output, range.end);
    output << '}';
}

[[nodiscard]] inline std::string declaration_identity(
    const structural_analysis::declaration_record& declaration) {
    if (!declaration.source_identity.empty()) {
        return declaration.source_identity;
    }
    if (!declaration.stable_usr.empty()) return declaration.stable_usr;
    if (!declaration.qualified_name.empty()) {
        return declaration.qualified_name;
    }
    throw std::runtime_error(
        "materialization declaration has no provenance, USR, or qualified name");
}

[[nodiscard]] inline bool is_lower_sha256(std::string_view value) noexcept {
    if (value.size() != 64) return false;
    return std::ranges::all_of(value, [](const char character) {
        return (character >= '0' && character <= '9')
            || (character >= 'a' && character <= 'f');
    });
}

inline void require_sha256(
    std::string_view value,
    std::string_view field,
    bool allow_empty = false) {
    if ((allow_empty && value.empty()) || is_lower_sha256(value)) return;
    throw std::runtime_error(
        std::string{field} + " must be empty or a lowercase SHA-256 digest");
}

[[nodiscard]] inline fs::path checked_relative_path(
    const fs::path& path,
    std::string_view owner) {
    const std::string original = path.generic_string();
    if (original.contains('\\')) {
        throw std::runtime_error(
            std::string{owner} + " contains a non-canonical path separator: "
            + original);
    }
    const fs::path normalized = validate_owned_artifact_path(
        path, artifact_owner_view{owner});
    if (normalized.generic_string() != original) {
        throw std::runtime_error(
            std::string{owner} + " is not already normalized: " + original);
    }
    for (const fs::path& component : normalized) {
        const std::string spelling = component.generic_string();
        if (spelling.empty()
            || std::ranges::any_of(spelling, [](const unsigned char character) {
                   return character < 0x20U || character == 0x7fU;
               })) {
            throw std::runtime_error(
                std::string{owner} + " contains an invalid path component");
        }
    }
    return normalized;
}

[[nodiscard]] inline fs::path checked_source_identity(
    const fs::path& path,
    const std::string_view owner) {
    const fs::path identity = checked_relative_path(path, owner);
    if (identity.begin() != identity.end() && *identity.begin() == "main") {
        throw std::runtime_error(
            std::string{owner}
            + " must not begin with the non-canonical main/ prefix");
    }
    return identity;
}

[[nodiscard]] inline fs::path checked_absolute_path(
    const fs::path& path,
    std::string_view owner) {
    if (path.empty() || !path.is_absolute()
        || path.lexically_normal() != path) {
        throw std::runtime_error(
            std::string{owner} + " must be an absolute normalized path: "
            + path.generic_string());
    }
    return path;
}

[[nodiscard]] inline fs::path path_beneath_root(
    const fs::path& path,
    const fs::path& root,
    std::string_view owner) {
    const fs::path normalized_path = checked_absolute_path(path, owner);
    const fs::path relative = normalized_path.lexically_relative(root);
    if (relative.empty() || relative == "." || relative.is_absolute()) {
        throw std::runtime_error(
            std::string{owner} + " must be a file beneath the output root");
    }
    const fs::path checked = checked_relative_path(relative, owner);
    if (root / checked != normalized_path) {
        throw std::runtime_error(
            std::string{owner} + " does not have the output-root identity");
    }
    return checked;
}

/// Returns the stable source-tree identity written to generated metadata.
///
/// Runtime paths remain absolute for filesystem authorization. Serialized
/// source identities are normalized forward-slash paths relative to the
/// repository source tree.
[[nodiscard]] inline std::string canonical_source_identity(
    const generation_plan& plan) {
    const fs::path repository_root = checked_absolute_path(
        plan.repository_root, "repository root");
    const fs::path source_path = checked_absolute_path(
        plan.source_path, "materialization source");
    const fs::path relative = source_path.lexically_relative(repository_root);
    if (relative.empty() || relative == "." || relative.is_absolute()) {
        throw std::runtime_error(
            "materialization source is not beneath the repository root");
    }
    const fs::path relative_identity = checked_relative_path(
        relative, "repository-relative materialization source");
    const std::string identity =
        structural_analysis::repository_source_identity(relative_identity);
    return checked_source_identity(
               fs::path{identity},
               "canonical materialization source identity")
        .generic_string();
}

inline void validate_plan_identity(const generation_plan& plan) {
    if (plan.name.empty() || plan.profile.empty()) {
        throw std::runtime_error(
            "materialization name and template-flatten profile must be non-empty");
    }
    checked_absolute_path(plan.output_root, "materialization output root");
    const std::string source_identity = canonical_source_identity(plan);
    static_cast<void>(source_identity);
    if (plan.topology.region.source_module.source_path.lexically_normal()
        != plan.source_path.lexically_normal()) {
        throw std::runtime_error(
            "materialization source path disagrees with the source-module record");
    }
    if (plan.topology.region.source_module.provided_module.empty()
        || plan.topology.region.configured_output_module_name.empty()) {
        throw std::runtime_error(
            "materialization source and output module names must be non-empty");
    }
    if (plan.policy != plan.topology.region.policy) {
        throw std::runtime_error(
            "generation-plan emission policy disagrees with authenticated topology");
    }
    if (plan.source_sha256 != bioseq::common::sha256_hex(plan.source_text)) {
        throw std::runtime_error(
            "materialization source SHA-256 does not match the authored bytes");
    }
    require_sha256(plan.source_sha256, "source_sha256");
    require_sha256(
        plan.compiler_module_inventory_sha256,
        "compiler_module_inventory_sha256");
    if (plan.topology.canonical_digest_input.empty()
        || plan.topology.topology_digest
            != bioseq::common::sha256_hex(plan.topology.canonical_digest_input)) {
        throw std::runtime_error(
            "topology digest does not seal the canonical topology input");
    }
    require_sha256(plan.topology.topology_digest, "topology_digest");
    require_sha256(
        plan.previous_manifest_identity,
        "previous_manifest_identity",
        true);

    std::set<std::string, std::less<>> group_names;
    for (const axis_group& group : plan.topology.groups) {
        if (group.name.empty() || !group_names.insert(group.name).second
            || group.cases.empty()) {
            throw std::runtime_error(
                "topology groups require unique names and non-empty cases");
        }
        std::set<std::string, std::less<>> labels;
        for (std::size_t index = 0; index < group.cases.size(); ++index) {
            const replacement_case& replacement = group.cases[index];
            if (replacement.ordinal != index || replacement.label.empty()
                || replacement.spelling.empty()
                || !labels.insert(replacement.label).second) {
                throw std::runtime_error(
                    "topology group cases require contiguous ordinals, unique "
                    "labels, and non-empty replacement spellings");
            }
        }
        for (const std::size_t binding_index : group.binding_indices) {
            if (binding_index >= plan.topology.region.bindings.size()) {
                throw std::runtime_error(
                    "topology group references an out-of-range binding");
            }
        }
    }

    std::set<std::uint32_t> coordinate_ordinals;
    for (std::size_t index = 0;
         index < plan.topology.coordinates.size();
         ++index) {
        const materialization_coordinate& coordinate =
            plan.topology.coordinates[index];
        if (coordinate.ordinal != index
            || !coordinate_ordinals.insert(coordinate.ordinal).second
            || coordinate.identity_namespace.empty()
            || coordinate.module_name.empty()
            || coordinate.topology_digest != plan.topology.topology_digest) {
            throw std::runtime_error(
                "materialization coordinates require contiguous ordinals, "
                "stable identities, and the plan topology digest");
        }
        checked_relative_path(
            coordinate.relative_path, "materialization coordinate path");
    }
    std::set<std::uint32_t> enabled;
    for (const std::uint32_t ordinal : plan.enabled_ordinals) {
        if (ordinal >= plan.topology.coordinates.size()
            || !enabled.insert(ordinal).second) {
            throw std::runtime_error(
                "enabled materialization ordinals must be unique topology rows");
        }
    }
    std::set<std::uint32_t> representatives;
    for (const std::uint32_t ordinal :
         plan.topology.identity_representative_ordinals) {
        if (ordinal >= plan.topology.coordinates.size()
            || !representatives.insert(ordinal).second) {
            throw std::runtime_error(
                "identity representatives must be unique topology rows");
        }
    }

    for (const binding_record& binding : plan.topology.region.bindings) {
        if (binding.group.empty() || binding.cases.empty()
            || !group_names.contains(binding.group)) {
            throw std::runtime_error(
                "materialization bindings require a known group and cases");
        }
        std::set<std::string, std::less<>> labels;
        for (std::size_t index = 0; index < binding.cases.size(); ++index) {
            const replacement_case& replacement = binding.cases[index];
            if (replacement.ordinal != index || replacement.label.empty()
                || replacement.spelling.empty()
                || !labels.insert(replacement.label).second) {
                throw std::runtime_error(
                    "binding cases require contiguous ordinals, unique labels, "
                    "and non-empty replacement spellings");
            }
        }
    }

    if (plan.frontend_actions.dependency_preparation_validations != 1U
        || plan.frontend_actions.canonical_token_slice_scans != 2U
        || plan.frontend_actions.signature_donor_token_slice_scans
            != 1U + plan.declaration_probes.size()
        || plan.frontend_actions.body_free_declaration_probe_scans
            != plan.declaration_probes.size()
        || plan.frontend_actions.artifact_render_passes != 2U
        || plan.frontend_actions.body_bearing_compiler_invocations != 0U
        || plan.declaration_probes.size()
            != plan.topology.identity_representative_ordinals.size()) {
        throw std::runtime_error(
            "observed frontend action ledger disagrees with dependency "
            "validation, canonical/donor token slicing, body-free probes, "
            "rendering, or the "
            "zero body-bearing compiler-action invariant");
    }
    const std::uint32_t expected_partition_units =
        plan.policy == emission_policy::selector_api ? 1U : 0U;
    if (plan.compilation_obligations.api_partition_units
            != expected_partition_units
        || plan.compilation_obligations.selector_partition_units
            != expected_partition_units
        || plan.compilation_obligations.body_bearing_implementation_units
            != plan.enabled_ordinals.size()) {
        throw std::runtime_error(
            "CMake compilation obligations disagree with the generated API, "
            "selector, or enabled implementation topology");
    }
    for (const structural_analysis::translation_unit& probe :
         plan.declaration_probes) {
        if (probe.mode != structural_analysis::collection_mode::declaration_probe
            || probe.clang_release != plan.clang_release
            || probe.source_module.provided_module.empty()
            || (plan.policy == emission_policy::implementation_owners
                && (probe.source_module.provided_module
                        != plan.topology.region.configured_output_module_name
                    || probe.source_module.is_interface
                    || probe.source_module.is_partition))) {
            throw std::runtime_error(
                "declaration probes require body-free mode, the authenticated "
                "Clang version, and a unique module provider");
        }
        static_cast<void>(path_beneath_root(
            probe.source_path,
            plan.output_root,
            "declaration probe source"));
        require_sha256(probe.source_sha256, "declaration probe source_sha256");
        const bool has_identity = std::ranges::any_of(
            probe.declarations,
            [](const structural_analysis::declaration_record& declaration) {
                return !declaration.source_identity.empty()
                    && !declaration.source_coordinate.empty()
                    && !declaration.stable_usr.empty()
                    && !declaration.qualified_name.empty();
            });
        if (!has_identity) {
            throw std::runtime_error(
                "declaration probe contains no compiler-derived identity");
        }
    }
}

[[nodiscard]] inline bool coordinate_selects(
    const owned_declaration& declaration,
    const materialization_coordinate& coordinate) {
    for (const selection_clause& clause : declaration.clauses) {
        const axis_case_assignment* selected = nullptr;
        for (const axis_case_assignment& assignment : coordinate.assignments) {
            if (assignment.group != clause.group) continue;
            if (selected != nullptr) {
                throw std::runtime_error(
                    "coordinate repeats selection group '" + clause.group + "'");
            }
            selected = &assignment;
        }
        if (selected == nullptr) {
            throw std::runtime_error(
                "coordinate omits selection group '" + clause.group + "'");
        }
        if (std::ranges::find(
                clause.accepted_case_labels, selected->case_label)
            == clause.accepted_case_labels.end()) {
            return false;
        }
    }
    return true;
}

inline void write_string_vector(
    std::ostringstream& output,
    const std::vector<std::string>& values) {
    output << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) output << ',';
        write_string(output, values[index]);
    }
    output << ']';
}

inline void write_declaration_identity_vector(
    std::ostringstream& output,
    const std::vector<std::string>& identities) {
    write_string_vector(output, identities);
}

inline void write_declaration_probes(
    std::ostringstream& output,
    const generation_plan& plan) {
    output << "[\n";
    for (std::size_t probe_index = 0;
         probe_index < plan.declaration_probes.size();
         ++probe_index) {
        const structural_analysis::translation_unit& probe =
            plan.declaration_probes[probe_index];
        const fs::path relative_source = path_beneath_root(
            probe.source_path,
            plan.output_root,
            "declaration probe source");
        output << "    {\"source\":";
        write_string(output, relative_source.generic_string());
        output << ",\"source_sha256\":";
        write_string(output, probe.source_sha256);
        output << ",\"module\":";
        write_string(output, probe.source_module.provided_module);
        output << ",\"clang_version\":";
        write_string(output, probe.clang_release);
        output << ",\"declarations\":[";
        bool wrote_declaration = false;
        for (const structural_analysis::declaration_record& declaration :
             probe.declarations) {
            if (declaration.source_identity.empty()) continue;
            if (wrote_declaration) output << ',';
            wrote_declaration = true;
            output << "{\"provenance_id\":";
            write_string(output, declaration.source_identity);
            output << ",\"coordinate\":";
            write_string(output, declaration.source_coordinate);
            output << ",\"usr\":";
            write_string(output, declaration.stable_usr);
            output << ",\"qualified_name\":";
            write_string(output, declaration.qualified_name);
            output << ",\"linkage\":";
            write_string(output, linkage_name(declaration.linkage));
            output << ",\"attachment\":";
            write_string(output, attachment_name(declaration.attachment));
            output << '}';
        }
        output << "]}"
               << (probe_index + 1 == plan.declaration_probes.size()
                       ? "\n"
                       : ",\n");
    }
    output << "  ]";
}

enum class json_kind : std::uint8_t {
    object,
    array,
    string,
    number,
    boolean,
    null,
};

struct json_value {
    json_kind kind{json_kind::null};
    std::string scalar;
    bool boolean{};
    std::vector<std::string> member_names;
    std::vector<json_value> children;
};

class json_parser {
public:
    json_parser(std::string_view input, std::string_view owner)
        : input_{input}, owner_{owner} {}

    [[nodiscard]] json_value parse() {
        skip_whitespace();
        json_value value = parse_value();
        skip_whitespace();
        if (cursor_ != input_.size()) fail("trailing bytes after JSON value");
        return value;
    }

private:
    std::string_view input_;
    std::string owner_;
    std::size_t cursor_{};

    [[noreturn]] void fail(std::string_view message) const {
        throw std::runtime_error(
            owner_ + ": JSON byte " + std::to_string(cursor_) + ": "
            + std::string{message});
    }

    void skip_whitespace() noexcept {
        while (cursor_ < input_.size()
               && (input_[cursor_] == ' ' || input_[cursor_] == '\t'
                   || input_[cursor_] == '\n' || input_[cursor_] == '\r')) {
            ++cursor_;
        }
    }

    [[nodiscard]] char peek() const {
        if (cursor_ >= input_.size()) fail("unexpected end of input");
        return input_[cursor_];
    }

    void require(char expected) {
        if (peek() != expected) fail("unexpected token");
        ++cursor_;
    }

    [[nodiscard]] json_value parse_value() {
        switch (peek()) {
            case '{': return parse_object();
            case '[': return parse_array();
            case '"':
                return json_value{
                    .kind = json_kind::string,
                    .scalar = parse_string(),
                };
            case 't': return parse_literal("true", true);
            case 'f': return parse_literal("false", false);
            case 'n': return parse_null();
            default:
                if (peek() == '-' || (peek() >= '0' && peek() <= '9')) {
                    return parse_number();
                }
                fail("invalid JSON value");
        }
    }

    [[nodiscard]] json_value parse_object() {
        require('{');
        skip_whitespace();
        json_value result{.kind = json_kind::object};
        if (peek() == '}') {
            ++cursor_;
            return result;
        }
        while (true) {
            if (peek() != '"') fail("object member name must be a string");
            std::string name = parse_string();
            if (std::ranges::find(result.member_names, name)
                != result.member_names.end()) {
                fail("duplicate object member '" + name + "'");
            }
            skip_whitespace();
            require(':');
            skip_whitespace();
            result.member_names.push_back(std::move(name));
            result.children.push_back(parse_value());
            skip_whitespace();
            if (peek() == '}') {
                ++cursor_;
                return result;
            }
            require(',');
            skip_whitespace();
        }
    }

    [[nodiscard]] json_value parse_array() {
        require('[');
        skip_whitespace();
        json_value result{.kind = json_kind::array};
        if (peek() == ']') {
            ++cursor_;
            return result;
        }
        while (true) {
            result.children.push_back(parse_value());
            skip_whitespace();
            if (peek() == ']') {
                ++cursor_;
                return result;
            }
            require(',');
            skip_whitespace();
        }
    }

    [[nodiscard]] static std::uint32_t hex_digit(char character) {
        if (character >= '0' && character <= '9') {
            return static_cast<std::uint32_t>(character - '0');
        }
        if (character >= 'a' && character <= 'f') {
            return 10U + static_cast<std::uint32_t>(character - 'a');
        }
        if (character >= 'A' && character <= 'F') {
            return 10U + static_cast<std::uint32_t>(character - 'A');
        }
        return std::numeric_limits<std::uint32_t>::max();
    }

    [[nodiscard]] std::uint32_t parse_hex_quad() {
        if (cursor_ + 4 > input_.size()) fail("truncated Unicode escape");
        std::uint32_t value{};
        for (std::size_t index = 0; index < 4; ++index) {
            const std::uint32_t digit = hex_digit(input_[cursor_++]);
            if (digit == std::numeric_limits<std::uint32_t>::max()) {
                fail("invalid Unicode escape");
            }
            value = (value << 4U) | digit;
        }
        return value;
    }

    static void append_utf8(std::string& output, std::uint32_t code_point) {
        if (code_point <= 0x7fU) {
            output.push_back(static_cast<char>(code_point));
        } else if (code_point <= 0x7ffU) {
            output.push_back(static_cast<char>(0xc0U | (code_point >> 6U)));
            output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
        } else if (code_point <= 0xffffU) {
            output.push_back(static_cast<char>(0xe0U | (code_point >> 12U)));
            output.push_back(static_cast<char>(
                0x80U | ((code_point >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
        } else {
            output.push_back(static_cast<char>(0xf0U | (code_point >> 18U)));
            output.push_back(static_cast<char>(
                0x80U | ((code_point >> 12U) & 0x3fU)));
            output.push_back(static_cast<char>(
                0x80U | ((code_point >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
        }
    }

    [[nodiscard]] std::string parse_string() {
        require('"');
        std::string result;
        while (cursor_ < input_.size()) {
            const unsigned char character =
                static_cast<unsigned char>(input_[cursor_++]);
            if (character == '"') return result;
            if (character < 0x20U) fail("unescaped control character in string");
            if (character != '\\') {
                result.push_back(static_cast<char>(character));
                continue;
            }
            if (cursor_ >= input_.size()) fail("truncated string escape");
            const char escape = input_[cursor_++];
            switch (escape) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u': {
                    std::uint32_t code_point = parse_hex_quad();
                    if (code_point >= 0xd800U && code_point <= 0xdbffU) {
                        if (cursor_ + 2 > input_.size()
                            || input_[cursor_] != '\\'
                            || input_[cursor_ + 1] != 'u') {
                            fail("unpaired high surrogate");
                        }
                        cursor_ += 2;
                        const std::uint32_t low = parse_hex_quad();
                        if (low < 0xdc00U || low > 0xdfffU) {
                            fail("invalid low surrogate");
                        }
                        code_point = 0x10000U
                            + ((code_point - 0xd800U) << 10U)
                            + (low - 0xdc00U);
                    } else if (code_point >= 0xdc00U
                               && code_point <= 0xdfffU) {
                        fail("unpaired low surrogate");
                    }
                    append_utf8(result, code_point);
                    break;
                }
                default: fail("invalid string escape");
            }
        }
        fail("unterminated string");
    }

    [[nodiscard]] json_value parse_number() {
        const std::size_t begin = cursor_;
        if (input_[cursor_] == '-') ++cursor_;
        if (cursor_ >= input_.size()) fail("truncated number");
        if (input_[cursor_] == '0') {
            ++cursor_;
            if (cursor_ < input_.size() && input_[cursor_] >= '0'
                && input_[cursor_] <= '9') {
                fail("number has a leading zero");
            }
        } else {
            if (input_[cursor_] < '1' || input_[cursor_] > '9') {
                fail("invalid number");
            }
            while (cursor_ < input_.size() && input_[cursor_] >= '0'
                   && input_[cursor_] <= '9') {
                ++cursor_;
            }
        }
        if (cursor_ < input_.size() && input_[cursor_] == '.') {
            ++cursor_;
            const std::size_t fractional_begin = cursor_;
            while (cursor_ < input_.size() && input_[cursor_] >= '0'
                   && input_[cursor_] <= '9') {
                ++cursor_;
            }
            if (cursor_ == fractional_begin) fail("empty fractional part");
        }
        if (cursor_ < input_.size()
            && (input_[cursor_] == 'e' || input_[cursor_] == 'E')) {
            ++cursor_;
            if (cursor_ < input_.size()
                && (input_[cursor_] == '+' || input_[cursor_] == '-')) {
                ++cursor_;
            }
            const std::size_t exponent_begin = cursor_;
            while (cursor_ < input_.size() && input_[cursor_] >= '0'
                   && input_[cursor_] <= '9') {
                ++cursor_;
            }
            if (cursor_ == exponent_begin) fail("empty exponent");
        }
        return json_value{
            .kind = json_kind::number,
            .scalar = std::string{input_.substr(begin, cursor_ - begin)},
        };
    }

    [[nodiscard]] json_value parse_literal(
        std::string_view spelling,
        bool value) {
        if (input_.substr(cursor_, spelling.size()) != spelling) {
            fail("invalid literal");
        }
        cursor_ += spelling.size();
        return json_value{
            .kind = json_kind::boolean,
            .boolean = value,
        };
    }

    [[nodiscard]] json_value parse_null() {
        if (input_.substr(cursor_, 4) != "null") fail("invalid literal");
        cursor_ += 4;
        return json_value{.kind = json_kind::null};
    }
};

[[nodiscard]] inline const json_value& require_member(
    const json_value& object,
    std::string_view name,
    std::string_view owner) {
    if (object.kind != json_kind::object) {
        throw std::runtime_error(std::string{owner} + " must be a JSON object");
    }
    for (std::size_t index = 0; index < object.member_names.size(); ++index) {
        if (object.member_names[index] == name) return object.children[index];
    }
    throw std::runtime_error(
        std::string{owner} + " omits required member '" + std::string{name}
        + "'");
}

template<std::size_t p_size>
inline void require_exact_members(
    const json_value& object,
    const std::array<std::string_view, p_size>& expected,
    std::string_view owner) {
    if (object.kind != json_kind::object
        || object.member_names.size() != expected.size()) {
        throw std::runtime_error(
            std::string{owner} + " does not match the exact manifest schema");
    }
    for (const std::string& actual : object.member_names) {
        if (std::ranges::find(expected, actual) == expected.end()) {
            throw std::runtime_error(
                std::string{owner} + " has unknown member '" + actual + "'");
        }
    }
}

[[nodiscard]] inline std::string_view require_string(
    const json_value& value,
    std::string_view owner) {
    if (value.kind != json_kind::string) {
        throw std::runtime_error(std::string{owner} + " must be a JSON string");
    }
    return value.scalar;
}

[[nodiscard]] inline std::uint64_t require_unsigned(
    const json_value& value,
    std::string_view owner) {
    if (value.kind != json_kind::number || value.scalar.empty()
        || value.scalar.front() == '-') {
        throw std::runtime_error(
            std::string{owner} + " must be an unsigned JSON integer");
    }
    std::uint64_t result{};
    const std::from_chars_result parsed = std::from_chars(
        value.scalar.data(), value.scalar.data() + value.scalar.size(), result);
    if (parsed.ec != std::errc{}
        || parsed.ptr != value.scalar.data() + value.scalar.size()) {
        throw std::runtime_error(
            std::string{owner} + " must be an unsigned JSON integer");
    }
    return result;
}

[[nodiscard]] inline bool require_boolean(
    const json_value& value,
    std::string_view owner) {
    if (value.kind != json_kind::boolean) {
        throw std::runtime_error(std::string{owner} + " must be a JSON boolean");
    }
    return value.boolean;
}

[[nodiscard]] inline std::string seal_manifest(
    std::string_view unsigned_content,
    std::string_view identity) {
    if (!unsigned_content.ends_with("\n}\n")) {
        throw std::runtime_error(
            "canonical artifacts manifest must end with a JSON object newline");
    }
    std::string result{
        unsigned_content.substr(0, unsigned_content.size() - 3)};
    result += ",\n  \"manifest_identity\": \"";
    result += identity;
    result += "\"\n}\n";
    return result;
}

[[nodiscard]] inline std::pair<std::string, std::string>
verify_manifest_self_seal(
    std::string_view content,
    std::string_view owner) {
    constexpr std::string_view marker =
        ",\n  \"manifest_identity\": \"";
    const std::size_t marker_position = content.rfind(marker);
    if (marker_position == std::string_view::npos) {
        throw std::runtime_error(
            std::string{owner} + ": missing canonical manifest self-seal");
    }
    const std::size_t identity_begin = marker_position + marker.size();
    constexpr std::size_t identity_size = 64;
    if (identity_begin + identity_size + 4 != content.size()
        || content.substr(identity_begin + identity_size) != "\"\n}\n") {
        throw std::runtime_error(
            std::string{owner} + ": non-canonical manifest self-seal suffix");
    }
    const std::string identity{
        content.substr(identity_begin, identity_size)};
    require_sha256(identity, "manifest_identity");
    std::string unsigned_content{content.substr(0, marker_position)};
    unsigned_content += "\n}\n";
    if (bioseq::common::sha256_hex(unsigned_content) != identity) {
        throw std::runtime_error(
            std::string{owner} + ": manifest self-seal does not match its bytes");
    }
    return {std::move(unsigned_content), identity};
}

inline void replace_authenticated_identity(
    std::string& content,
    const std::string_view marker,
    const bool allow_empty,
    const bool use_last_occurrence,
    const std::string_view owner) {
    const std::size_t marker_position = use_last_occurrence
        ? content.rfind(marker)
        : content.find(marker);
    if (marker_position == std::string::npos) {
        throw std::runtime_error(
            std::string{owner}
            + ": canonical publication identity field is missing");
    }
    const std::size_t value_begin = marker_position + marker.size();
    const std::size_t value_end = content.find('"', value_begin);
    if (value_end == std::string::npos) {
        throw std::runtime_error(
            std::string{owner}
            + ": canonical publication identity field is unterminated");
    }
    const std::size_t value_size = value_end - value_begin;
    if ((!allow_empty || value_size != 0U) && value_size != 64U) {
        throw std::runtime_error(
            std::string{owner}
            + ": canonical publication identity has an invalid length");
    }
    content.replace(value_begin, value_size, "<authenticated-identity>");
}

/// Canonical renderers necessarily change the predecessor/self identities on
/// a repeat publication. Remove only those two authenticated envelope values
/// so the publisher can recognize an otherwise byte-identical no-op without
/// weakening any policy evidence or artifact digest.
[[nodiscard]] inline std::string publication_noop_projection(
    std::string_view content,
    std::string_view owner) {
    static_cast<void>(verify_manifest_self_seal(content, owner));
    std::string projected{content};
    replace_authenticated_identity(
        projected,
        "\n  \"prior_manifest_identity\": \"",
        true,
        false,
        owner);
    replace_authenticated_identity(
        projected,
        ",\n  \"manifest_identity\": \"",
        false,
        true,
        owner);
    return projected;
}

[[nodiscard]] inline previous_publication parse_publication_manifest(
    std::string_view content,
    std::string_view owner,
    const fs::path& expected_output_root,
    std::string_view expected_policy,
    std::string_view expected_topology_digest) {
    const std::pair<std::string, std::string> sealed =
        verify_manifest_self_seal(content, owner);
    const std::string& unsigned_content = sealed.first;
    const std::string& sealed_identity = sealed.second;
    static_cast<void>(unsigned_content);
    const json_value root = json_parser{content, owner}.parse();
    require_exact_members(
        root,
        std::array{
            std::string_view{"publication_schema_revision"},
            std::string_view{"policy"},
            std::string_view{"name"},
            std::string_view{"output_root"},
            std::string_view{"topology_digest"},
            std::string_view{"prior_manifest_identity"},
            std::string_view{"artifacts"},
            std::string_view{"evidence"},
            std::string_view{"manifest_identity"},
        },
        "publication manifest");

    if (require_unsigned(
            require_member(root, "publication_schema_revision", owner),
            "publication_schema_revision")
        != publication_schema_revision) {
        throw std::runtime_error(
            std::string{owner}
            + ": unsupported publication manifest schema revision");
    }
    const std::string policy{require_string(
        require_member(root, "policy", owner), "policy")};
    if (policy.empty() || policy.front() < 'a' || policy.front() > 'z'
        || !std::ranges::all_of(policy, [](const char character) {
               return (character >= 'a' && character <= 'z')
                   || (character >= '0' && character <= '9')
                   || character == '_';
           })) {
        throw std::runtime_error(
            std::string{owner} + ": publication policy is not a policy token");
    }
    if (!expected_policy.empty() && policy != expected_policy) {
        throw std::runtime_error(
            std::string{owner} + ": publication policy disagrees with the owner");
    }
    const std::string publication_name{require_string(
        require_member(root, "name", owner), "name")};
    if (publication_name.empty()) {
        throw std::runtime_error(
            std::string{owner} + ": manifest name must be non-empty");
    }
    const fs::path normalized_root = checked_absolute_path(
        expected_output_root, "expected publication output root");
    if (require_string(
            require_member(root, "output_root", owner), "output_root")
        != normalized_root.generic_string()) {
        throw std::runtime_error(
            std::string{owner} + ": publication output-root identity mismatch");
    }
    const std::string_view topology = require_string(
        require_member(root, "topology_digest", owner), "topology_digest");
    require_sha256(topology, "topology_digest");
    if (!expected_topology_digest.empty()
        && topology != expected_topology_digest) {
        throw std::runtime_error(
            std::string{owner}
            + ": publication topology digest mismatch; reconfigure the build");
    }
    const std::string_view manifest_identity = require_string(
        require_member(root, "manifest_identity", owner), "manifest_identity");
    if (manifest_identity != sealed_identity) {
        throw std::runtime_error(
            std::string{owner} + ": parsed and sealed manifest identities differ");
    }
    const std::string prior_identity{require_string(
        require_member(root, "prior_manifest_identity", owner),
        "prior_manifest_identity")};
    require_sha256(prior_identity, "prior_manifest_identity", true);
    const json_value& evidence = require_member(root, "evidence", owner);
    if (evidence.kind != json_kind::object) {
        throw std::runtime_error(
            std::string{owner} + ": policy evidence must be a JSON object");
    }
    const json_value& artifacts = require_member(root, "artifacts", owner);
    if (artifacts.kind != json_kind::array || artifacts.children.empty()) {
        throw std::runtime_error(
            std::string{owner} + ": artifacts must be a non-empty JSON array");
    }
    previous_publication result{
        .present = true,
        .policy = policy,
        .name = publication_name,
        .topology_digest = std::string{topology},
        .manifest_identity = sealed_identity,
        .prior_manifest_identity = prior_identity,
    };
    std::string previous_path;
    for (const json_value& artifact : artifacts.children) {
        require_exact_members(
            artifact,
            std::array{
                std::string_view{"path"},
                std::string_view{"sha256"},
                std::string_view{"class"},
            },
            "publication manifest artifact");
        const std::string path_text{require_string(
            require_member(artifact, "path", owner), "artifact.path")};
        const fs::path relative = checked_relative_path(
            fs::path{path_text}, "authenticated publication artifact");
        if (!previous_path.empty() && previous_path >= path_text) {
            throw std::runtime_error(
                std::string{owner}
                + ": artifact paths must be unique and strictly ordered");
        }
        previous_path = path_text;
        const std::string digest{require_string(
            require_member(artifact, "sha256", owner), "artifact.sha256")};
        require_sha256(digest, "artifact.sha256");
        const std::string_view artifact_class = require_string(
            require_member(artifact, "class", owner), "artifact.class");
        publication_artifact_class parsed_class{};
        if (artifact_class == "production_cppm") {
            parsed_class = publication_artifact_class::production_cppm;
        } else if (artifact_class == "metadata_json") {
            parsed_class = publication_artifact_class::metadata_json;
        } else {
            throw std::runtime_error(
                std::string{owner} + ": unknown publication artifact class");
        }
        if ((parsed_class == publication_artifact_class::production_cppm
             && relative.extension() != ".cppm")
            || (parsed_class == publication_artifact_class::metadata_json
                && relative.extension() != ".json")) {
            throw std::runtime_error(
                std::string{owner}
                + ": authenticated artifact extension disagrees with its class");
        }
        result.artifacts.push_back(owned_publication_artifact{
            .relative_path = relative,
            .content_sha256 = digest,
            .artifact_class = parsed_class,
        });
    }
    return result;
}

inline void validate_template_flatten_publication_manifest(
    std::string_view content,
    std::string_view owner,
    const fs::path& expected_output_root,
    std::string_view expected_topology_digest) {
    const previous_publication publication = parse_publication_manifest(
        content,
        owner,
        expected_output_root,
        template_flatten_policy,
        expected_topology_digest);
    const json_value outer = json_parser{content, owner}.parse();
    const json_value& root = require_member(outer, "evidence", owner);
    require_exact_members(
        root,
        std::array{
            std::string_view{"schema_revision"},
            std::string_view{"generator_identity"},
            std::string_view{"emission_policy"},
            std::string_view{"profile"},
            std::string_view{"source"},
            std::string_view{"source_module"},
            std::string_view{"output_module"},
            std::string_view{"frontend_action_ledger"},
            std::string_view{"cmake_compilation_obligations"},
            std::string_view{"compiler"},
            std::string_view{"declaration_probes"},
            std::string_view{"artifacts"},
        },
        "template_flatten publication evidence");
    if (require_unsigned(
            require_member(root, "schema_revision", owner),
            "evidence.schema_revision")
        != artifact_schema_revision) {
        throw std::runtime_error(
            std::string{owner}
            + ": unsupported template_flatten evidence schema revision");
    }
    if (require_string(
            require_member(root, "generator_identity", owner),
            "evidence.generator_identity")
        != materialization_generator_identity) {
        throw std::runtime_error(
            std::string{owner}
            + ": unsupported materialization generator identity");
    }
    const emission_policy emission = parse_emission_policy(require_string(
        require_member(root, "emission_policy", owner),
        "evidence.emission_policy"));
    for (const std::string_view field :
         std::array{
             std::string_view{"profile"},
             std::string_view{"source_module"},
             std::string_view{"output_module"},
         }) {
        if (require_string(require_member(root, field, owner), field).empty()) {
            throw std::runtime_error(
                std::string{owner} + ": evidence member '"
                + std::string{field} + "' must be non-empty");
        }
    }
    const std::string output_module{require_string(
        require_member(root, "output_module", owner),
        "evidence.output_module")};

    const json_value& source = require_member(root, "source", owner);
    require_exact_members(
        source,
        std::array{
            std::string_view{"path"},
            std::string_view{"sha256"},
        },
        "template_flatten evidence source");
    static_cast<void>(checked_source_identity(
        fs::path{require_string(
            require_member(source, "path", owner), "source.path")},
        "template_flatten evidence source path"));
    require_sha256(
        require_string(require_member(source, "sha256", owner), "source.sha256"),
        "source.sha256");

    const json_value& frontend_actions = require_member(
        root, "frontend_action_ledger", owner);
    require_exact_members(
        frontend_actions,
        std::array{
            std::string_view{"dependency_preparation_validations"},
            std::string_view{"canonical_token_slice_scans"},
            std::string_view{"signature_donor_token_slice_scans"},
            std::string_view{"body_free_declaration_probe_scans"},
            std::string_view{"artifact_render_passes"},
            std::string_view{"body_bearing_compiler_invocations"},
        },
        "template_flatten evidence frontend_action_ledger");
    const std::uint64_t body_free_probe_count = require_unsigned(
        require_member(
            frontend_actions,
            "body_free_declaration_probe_scans",
            owner),
        "frontend_action_ledger.body_free_declaration_probe_scans");
    const std::uint64_t signature_donor_slice_count = require_unsigned(
        require_member(
            frontend_actions,
            "signature_donor_token_slice_scans",
            owner),
        "frontend_action_ledger.signature_donor_token_slice_scans");
    if (require_unsigned(
            require_member(
                frontend_actions,
                "dependency_preparation_validations",
                owner),
            "frontend_action_ledger.dependency_preparation_validations")
            != 1U
        || require_unsigned(
               require_member(
                   frontend_actions, "canonical_token_slice_scans", owner),
               "frontend_action_ledger.canonical_token_slice_scans")
            != 2U
        || signature_donor_slice_count != 1U + body_free_probe_count
        || require_unsigned(
               require_member(
                   frontend_actions, "artifact_render_passes", owner),
               "frontend_action_ledger.artifact_render_passes")
            != 2U
        || require_unsigned(
               require_member(
                   frontend_actions,
                   "body_bearing_compiler_invocations",
                   owner),
               "frontend_action_ledger.body_bearing_compiler_invocations")
            != 0U) {
        throw std::runtime_error(
            std::string{owner}
            + ": frontend ledger must record one preparation validation, "
              "two canonical token slices, one signature-donor slice per "
              "probe plus dependency derivation, two "
              "render passes, and zero observed "
              "body-bearing compiler invocations");
    }

    const json_value& obligations = require_member(
        root, "cmake_compilation_obligations", owner);
    require_exact_members(
        obligations,
        std::array{
            std::string_view{"api_partition_units"},
            std::string_view{"selector_partition_units"},
            std::string_view{"body_bearing_implementation_units"},
        },
        "template_flatten evidence cmake_compilation_obligations");
    const std::uint64_t expected_partition_units =
        emission == emission_policy::selector_api ? 1U : 0U;
    if (require_unsigned(
            require_member(obligations, "api_partition_units", owner),
            "cmake_compilation_obligations.api_partition_units")
            != expected_partition_units
        || require_unsigned(
               require_member(obligations, "selector_partition_units", owner),
               "cmake_compilation_obligations.selector_partition_units")
            != expected_partition_units) {
        throw std::runtime_error(
            std::string{owner}
            + ": CMake partition obligations disagree with emission policy");
    }
    const std::uint64_t body_bearing_obligation_count = require_unsigned(
        require_member(
            obligations, "body_bearing_implementation_units", owner),
        "cmake_compilation_obligations.body_bearing_implementation_units");

    const json_value& compiler = require_member(root, "compiler", owner);
    require_exact_members(
        compiler,
        std::array{
            std::string_view{"clang_version"},
            std::string_view{"compilation_signature"},
            std::string_view{"compiler_module_inventory_sha256"},
        },
        "template_flatten evidence compiler");
    if (require_string(
            require_member(compiler, "clang_version", owner),
            "compiler.clang_version")
            .empty()
        || require_string(
               require_member(compiler, "compilation_signature", owner),
               "compiler.compilation_signature")
               .empty()) {
        throw std::runtime_error(
            std::string{owner}
            + ": compiler version and compilation signature must be non-empty");
    }
    require_sha256(
        require_string(
            require_member(compiler, "compiler_module_inventory_sha256", owner),
            "compiler.compiler_module_inventory_sha256"),
        "compiler.compiler_module_inventory_sha256");

    const json_value& probes = require_member(root, "declaration_probes", owner);
    if (probes.kind != json_kind::array
        || probes.children.size() != body_free_probe_count) {
        throw std::runtime_error(
            std::string{owner}
            + ": declaration probes disagree with the frontend action count");
    }
    std::set<std::tuple<std::string, std::string, std::string>>
        unmatched_probe_evidence;
    for (const json_value& probe : probes.children) {
        require_exact_members(
            probe,
            std::array{
                std::string_view{"source"},
                std::string_view{"source_sha256"},
                std::string_view{"module"},
                std::string_view{"clang_version"},
                std::string_view{"declarations"},
            },
            "template_flatten evidence declaration probe");
        const std::string probe_source = checked_relative_path(
            fs::path{require_string(
                require_member(probe, "source", owner), "probe.source")},
            "template_flatten evidence declaration probe source")
                                             .generic_string();
        const std::string probe_sha256{require_string(
            require_member(probe, "source_sha256", owner),
            "probe.source_sha256")};
        require_sha256(probe_sha256, "probe.source_sha256");
        const std::string probe_module{require_string(
            require_member(probe, "module", owner), "probe.module")};
        if (probe_module.empty()
            || (emission == emission_policy::implementation_owners
                && probe_module != output_module)
            || require_string(
                   require_member(probe, "clang_version", owner),
                   "probe.clang_version")
                   .empty()) {
            throw std::runtime_error(
                std::string{owner}
                + ": declaration probe module and Clang version are required");
        }
        if (!unmatched_probe_evidence.emplace(
                 probe_source, probe_module, probe_sha256).second) {
            throw std::runtime_error(
                std::string{owner}
                + ": declaration probe evidence must bind unique artifacts");
        }
        const json_value& declarations = require_member(
            probe, "declarations", owner);
        if (declarations.kind != json_kind::array
            || declarations.children.empty()) {
            throw std::runtime_error(
                std::string{owner}
                + ": declaration probe must carry compiler identities");
        }
        for (const json_value& declaration : declarations.children) {
            require_exact_members(
                declaration,
                std::array{
                    std::string_view{"provenance_id"},
                    std::string_view{"coordinate"},
                    std::string_view{"usr"},
                    std::string_view{"qualified_name"},
                    std::string_view{"linkage"},
                    std::string_view{"attachment"},
                },
                "template_flatten evidence probe declaration");
            const std::string_view provenance = require_string(
                require_member(declaration, "provenance_id", owner),
                "probe declaration provenance_id");
            const std::string_view coordinate = require_string(
                require_member(declaration, "coordinate", owner),
                "probe declaration coordinate");
            const std::string_view usr = require_string(
                require_member(declaration, "usr", owner),
                "probe declaration usr");
            const std::string_view qualified_name = require_string(
                require_member(declaration, "qualified_name", owner),
                "probe declaration qualified_name");
            const std::string_view linkage = require_string(
                require_member(declaration, "linkage", owner),
                "probe declaration linkage");
            const std::string_view attachment = require_string(
                require_member(declaration, "attachment", owner),
                "probe declaration attachment");
            std::uint32_t coordinate_ordinal{};
            const std::from_chars_result parsed_coordinate = std::from_chars(
                coordinate.data(),
                coordinate.data() + coordinate.size(),
                coordinate_ordinal);
            if (provenance.empty() || coordinate.empty() || usr.empty()
                || qualified_name.empty()
                || parsed_coordinate.ec != std::errc{}
                || parsed_coordinate.ptr != coordinate.data() + coordinate.size()
                || coordinate != std::to_string(coordinate_ordinal)
                || (linkage != "none" && linkage != "internal"
                    && linkage != "module" && linkage != "external")
                || (attachment != "global_module"
                    && attachment != "named_module")
                || (emission == emission_policy::implementation_owners
                    && ((linkage != "module" && linkage != "external")
                        || attachment != "named_module"))) {
                throw std::runtime_error(
                    std::string{owner}
                    + ": declaration probe identity facts are invalid");
            }
        }
    }

    const json_value& artifacts = require_member(root, "artifacts", owner);
    if (artifacts.kind != json_kind::array
        || artifacts.children.size() != publication.artifacts.size()) {
        throw std::runtime_error(
            std::string{owner}
            + ": template_flatten artifact evidence does not cover the "
              "complete publication set");
    }
    std::string previous_path;
    std::uint64_t api_partition_count = 0U;
    std::uint64_t selector_partition_count = 0U;
    std::uint64_t declaration_probe_count = 0U;
    std::uint64_t implementation_unit_count = 0U;
    std::uint64_t manifest_count = 0U;
    for (std::size_t index = 0; index < artifacts.children.size(); ++index) {
        const json_value& artifact = artifacts.children[index];
        require_exact_members(
            artifact,
            std::array{
                std::string_view{"path"},
                std::string_view{"kind"},
                std::string_view{"module"},
                std::string_view{"logical_coordinates"},
                std::string_view{"validation_class"},
            },
            "template_flatten evidence artifact");
        const std::string path_text{require_string(
            require_member(artifact, "path", owner), "artifact.path")};
        if (!previous_path.empty() && previous_path >= path_text) {
            throw std::runtime_error(
                std::string{owner}
                + ": evidence artifact paths must be strictly ordered");
        }
        previous_path = path_text;
        if (publication.artifacts[index].relative_path
            != checked_relative_path(
                fs::path{path_text}, "template_flatten evidence artifact")) {
            throw std::runtime_error(
                std::string{owner}
                + ": policy evidence path disagrees with publication ownership");
        }
        const std::string_view kind = require_string(
            require_member(artifact, "kind", owner), "artifact.kind");
        const bool known_kind = kind == "declaration_probe"
            || kind == "api_partition" || kind == "implementation_unit"
            || kind == "selector_partition" || kind == "manifest";
        if (!known_kind) {
            throw std::runtime_error(
                std::string{owner}
                + ": unknown template_flatten artifact kind");
        }
        if (kind == "api_partition") {
            ++api_partition_count;
        } else if (kind == "selector_partition") {
            ++selector_partition_count;
        } else if (kind == "declaration_probe") {
            ++declaration_probe_count;
        } else if (kind == "implementation_unit") {
            ++implementation_unit_count;
        } else if (kind == "manifest") {
            ++manifest_count;
        }
        const std::string_view module = require_string(
            require_member(artifact, "module", owner), "artifact.module");
        const bool production = publication.artifacts[index].artifact_class
            == publication_artifact_class::production_cppm;
        if ((production && (module.empty() || kind == "manifest"))
            || (!production && (!module.empty() || kind != "manifest"))) {
            throw std::runtime_error(
                std::string{owner}
                + ": template_flatten artifact evidence disagrees with its "
                  "ownership class");
        }
        if (emission == emission_policy::implementation_owners
            && kind == "implementation_unit" && module != output_module) {
            throw std::runtime_error(
                std::string{owner}
                + ": implementation-owner artifact is not attached to its configured primary module");
        }
        const json_value& coordinates = require_member(
            artifact, "logical_coordinates", owner);
        if (coordinates.kind != json_kind::array) {
            throw std::runtime_error(
                std::string{owner}
                + ": artifact.logical_coordinates must be an array");
        }
        for (const json_value& coordinate : coordinates.children) {
            static_cast<void>(require_unsigned(
                coordinate, "artifact logical coordinate"));
        }
        const std::string_view validation = require_string(
            require_member(artifact, "validation_class", owner),
            "artifact.validation_class");
        const std::string_view expected_validation =
            kind == "selector_partition" || kind == "implementation_unit"
            ? std::string_view{"body_bearing"}
            : std::string_view{"body_free"};
        if (validation != expected_validation) {
            throw std::runtime_error(
                std::string{owner}
                + ": artifact kind disagrees with its validation class");
        }
        if (kind == "declaration_probe"
            && unmatched_probe_evidence.erase(std::tuple{
                   path_text,
                   std::string{module},
                   publication.artifacts[index].content_sha256})
                != 1U) {
            throw std::runtime_error(
                std::string{owner}
                + ": declaration probe evidence does not bind its exact "
                  "artifact path, module, and digest");
        }
    }
    const std::uint64_t expected_published_probe_count =
        emission == emission_policy::selector_api ? body_free_probe_count : 0U;
    const bool probe_evidence_bound =
        emission == emission_policy::selector_api
        ? unmatched_probe_evidence.empty()
        : unmatched_probe_evidence.size() == body_free_probe_count;
    if (api_partition_count != expected_partition_units
        || selector_partition_count != expected_partition_units
        || declaration_probe_count != expected_published_probe_count
        || implementation_unit_count != body_bearing_obligation_count
        || manifest_count != 1U || !probe_evidence_bound) {
        throw std::runtime_error(
            std::string{owner}
            + ": rendered artifacts disagree with distinct CMake "
              "compilation obligations");
    }
}

struct configure_case_record {
    std::uint32_t ordinal{};
    std::string label;
    std::string replacement;
};

struct configure_binding_record {
    std::uint32_t ordinal{};
    std::string name;
    std::string group;
    axis_role role{axis_role::identity};
    std::vector<configure_case_record> cases;
};

struct configure_group_record {
    std::string name;
    axis_role role{axis_role::identity};
    std::vector<std::uint32_t> binding_ordinals;
    std::vector<configure_case_record> cases;
};

struct configure_assignment_record {
    std::string group;
    axis_role role{axis_role::identity};
    std::string case_label;
    std::uint32_t case_ordinal{};
};

struct configure_coordinate_record {
    std::uint32_t ordinal{};
    std::vector<configure_assignment_record> assignments;
    bool enabled{};
    std::optional<fs::path> source;
};

[[nodiscard]] inline bool manifest_label(std::string_view value) noexcept {
    if (value.empty() || value.front() < 'a' || value.front() > 'z') {
        return false;
    }
    return std::ranges::all_of(value, [](const char character) {
        return (character >= 'a' && character <= 'z')
            || (character >= '0' && character <= '9')
            || character == '_';
    });
}

[[nodiscard]] inline axis_role parse_manifest_axis_role(
    std::string_view value,
    std::string_view owner) {
    if (value == "identity") return axis_role::identity;
    if (value == "shard") return axis_role::shard;
    throw std::runtime_error(
        std::string{owner} + ": axis role must be identity or shard");
}

[[nodiscard]] inline std::vector<configure_case_record>
parse_configure_cases(
    const json_value& value,
    std::string_view owner) {
    if (value.kind != json_kind::array || value.children.empty()) {
        throw std::runtime_error(
            std::string{owner} + ": cases must be a non-empty JSON array");
    }
    std::vector<configure_case_record> result;
    result.reserve(value.children.size());
    std::set<std::string, std::less<>> labels;
    for (std::size_t index = 0; index < value.children.size(); ++index) {
        const json_value& record = value.children[index];
        require_exact_members(
            record,
            std::array{
                std::string_view{"ordinal"},
                std::string_view{"label"},
                std::string_view{"replacement"},
            },
            owner);
        const std::uint64_t ordinal = require_unsigned(
            require_member(record, "ordinal", owner), "case.ordinal");
        const std::string label{require_string(
            require_member(record, "label", owner), "case.label")};
        const std::string replacement{require_string(
            require_member(record, "replacement", owner),
            "case.replacement")};
        if (ordinal != index || ordinal > std::numeric_limits<std::uint32_t>::max()
            || !manifest_label(label) || replacement.empty()
            || replacement.contains('\t') || replacement.contains('\r')
            || replacement.contains('\n') || replacement.contains(';')
            || !labels.insert(label).second) {
            throw std::runtime_error(
                std::string{owner}
                + ": cases require contiguous ordinals, unique lowercase "
                  "labels, and closed-grammar replacements");
        }
        result.push_back(configure_case_record{
            .ordinal = static_cast<std::uint32_t>(ordinal),
            .label = label,
            .replacement = replacement,
        });
    }
    return result;
}

[[nodiscard]] inline bool configure_coordinate_enabled(
    const configure_coordinate_record& coordinate,
    const std::string_view profile) {
    const configure_assignment_record* alphabet = nullptr;
    const configure_assignment_record* width = nullptr;
    for (const configure_assignment_record& assignment :
         coordinate.assignments) {
        if (assignment.group == "alphabet") alphabet = &assignment;
        if (assignment.group == "width") width = &assignment;
    }
    if (profile == "dna4_uint64") {
        return (alphabet == nullptr || alphabet->case_label == "dna4")
            && (width == nullptr || width->case_label == "w8");
    }
    if (profile == "all_alphabets_uint64") {
        return (alphabet == nullptr || alphabet->case_label == "dna4"
                || alphabet->case_label == "dna5")
            && (width == nullptr || width->case_label == "w8");
    }
    return true;
}

[[nodiscard]] inline configure_topology_provenance
parse_configure_topology_provenance(
    std::string_view content,
    std::string_view owner) {
    if (content.empty() || content.contains('\0')) {
        throw std::runtime_error(
            std::string{owner} + ": configure topology is empty or contains NUL");
    }
    const json_value root = json_parser{content, owner}.parse();
    require_exact_members(
        root,
        std::array{
            std::string_view{"schema_revision"},
            std::string_view{"generator_identity"},
            std::string_view{"policy"},
            std::string_view{"emission_policy"},
            std::string_view{"name"},
            std::string_view{"source_path"},
            std::string_view{"source_sha256"},
            std::string_view{"output_root"},
            std::string_view{"output_module"},
            std::string_view{"profile"},
            std::string_view{"topology_digest"},
            std::string_view{"bindings"},
            std::string_view{"groups"},
            std::string_view{"coordinates"},
            std::string_view{"enabled_ordinals"},
            std::string_view{"enabled_sources"},
            std::string_view{"api_partition"},
            std::string_view{"selector_partition"},
        },
        "configure topology manifest");
    if (require_unsigned(
            require_member(root, "schema_revision", owner),
            "schema_revision")
        != topology_schema_revision) {
        throw std::runtime_error(
            std::string{owner}
            + ": unsupported configure topology schema revision");
    }
    const std::string parsed_generator_identity{require_string(
        require_member(root, "generator_identity", owner),
        "generator_identity")};
    const std::string policy{require_string(
        require_member(root, "policy", owner), "policy")};
    const emission_policy emission = parse_emission_policy(require_string(
        require_member(root, "emission_policy", owner),
        "emission_policy"));
    if (parsed_generator_identity != materialization_generator_identity
        || policy != template_flatten_policy) {
        throw std::runtime_error(
            std::string{owner}
            + ": configure topology generator or policy is not authoritative");
    }
    const std::string name{require_string(
        require_member(root, "name", owner), "name")};
    const fs::path source_path = checked_absolute_path(
        fs::path{require_string(
            require_member(root, "source_path", owner), "source_path")},
        "configure topology source path");
    const std::string source_sha256{require_string(
        require_member(root, "source_sha256", owner), "source_sha256")};
    const fs::path output_root = checked_absolute_path(
        fs::path{require_string(
            require_member(root, "output_root", owner), "output_root")},
        "configure topology output root");
    const std::string output_module{require_string(
        require_member(root, "output_module", owner), "output_module")};
    std::optional<fs::path> api_partition;
    const json_value& api_partition_value =
        require_member(root, "api_partition", owner);
    if (api_partition_value.kind == json_kind::string) {
        api_partition = checked_absolute_path(
            fs::path{api_partition_value.scalar},
            "configure topology API partition");
    } else if (api_partition_value.kind != json_kind::null) {
        throw std::runtime_error(
            std::string{owner}
            + ": api_partition must be an absolute path or null");
    }
    std::optional<fs::path> selector_partition;
    const json_value& selector_partition_value =
        require_member(root, "selector_partition", owner);
    if (selector_partition_value.kind == json_kind::string) {
        selector_partition = checked_absolute_path(
            fs::path{selector_partition_value.scalar},
            "configure topology selector partition");
    } else if (selector_partition_value.kind != json_kind::null) {
        throw std::runtime_error(
            std::string{owner}
            + ": selector_partition must be an absolute path or null");
    }
    const std::string profile{require_string(
        require_member(root, "profile", owner), "profile")};
    if (!template_flatten_profile_supported(profile)) {
        throw std::runtime_error(
            std::string{owner}
            + ": profile is not in the closed semantic vocabulary");
    }
    const std::string topology_digest{require_string(
        require_member(root, "topology_digest", owner),
        "topology_digest")};
    if (!manifest_label(name) || output_module.empty()) {
        throw std::runtime_error(
            std::string{owner}
            + ": configure topology name or module identity is invalid");
    }
    require_sha256(source_sha256, "source_sha256");
    require_sha256(topology_digest, "topology_digest");

    const json_value& bindings_value = require_member(root, "bindings", owner);
    if (bindings_value.kind != json_kind::array
        || bindings_value.children.empty()) {
        throw std::runtime_error(
            std::string{owner} + ": bindings must be a non-empty array");
    }
    std::vector<configure_binding_record> bindings;
    bindings.reserve(bindings_value.children.size());
    std::set<std::string, std::less<>> binding_names;
    for (std::size_t index = 0; index < bindings_value.children.size(); ++index) {
        const json_value& value = bindings_value.children[index];
        require_exact_members(
            value,
            std::array{
                std::string_view{"ordinal"},
                std::string_view{"name"},
                std::string_view{"group"},
                std::string_view{"role"},
                std::string_view{"cases"},
            },
            "configure topology binding");
        const std::uint64_t ordinal = require_unsigned(
            require_member(value, "ordinal", owner), "binding.ordinal");
        configure_binding_record binding{
            .ordinal = static_cast<std::uint32_t>(ordinal),
            .name = std::string{require_string(
                require_member(value, "name", owner), "binding.name")},
            .group = std::string{require_string(
                require_member(value, "group", owner), "binding.group")},
            .role = parse_manifest_axis_role(
                require_string(
                    require_member(value, "role", owner), "binding.role"),
                owner),
            .cases = parse_configure_cases(
                require_member(value, "cases", owner), "configure binding"),
        };
        if (ordinal != index
            || ordinal > std::numeric_limits<std::uint32_t>::max()
            || binding.name.empty() || !manifest_label(binding.group)
            || binding.name.contains('\t') || binding.name.contains('\r')
            || binding.name.contains('\n')
            || !binding_names.insert(binding.name).second) {
            throw std::runtime_error(
                std::string{owner}
                + ": bindings require contiguous ordinals, known group "
                  "spellings, and unique names");
        }
        bindings.push_back(std::move(binding));
    }

    const json_value& groups_value = require_member(root, "groups", owner);
    if (groups_value.kind != json_kind::array
        || groups_value.children.empty()) {
        throw std::runtime_error(
            std::string{owner} + ": groups must be a non-empty array");
    }
    std::vector<configure_group_record> groups;
    groups.reserve(groups_value.children.size());
    std::set<std::string, std::less<>> group_names;
    std::set<std::uint32_t> grouped_bindings;
    for (std::size_t group_index = 0;
         group_index < groups_value.children.size();
         ++group_index) {
        const json_value& value = groups_value.children[group_index];
        require_exact_members(
            value,
            std::array{
                std::string_view{"ordinal"},
                std::string_view{"role"},
                std::string_view{"name"},
                std::string_view{"binding_ordinals"},
                std::string_view{"cases"},
            },
            "configure topology group");
        const std::uint64_t group_ordinal = require_unsigned(
            require_member(value, "ordinal", owner), "group.ordinal");
        configure_group_record group{
            .name = std::string{require_string(
                require_member(value, "name", owner), "group.name")},
            .role = parse_manifest_axis_role(
                require_string(
                    require_member(value, "role", owner), "group.role"),
                owner),
            .cases = parse_configure_cases(
                require_member(value, "cases", owner), "configure group"),
        };
        if (group_ordinal != group_index
            || group_ordinal > std::numeric_limits<std::uint32_t>::max()
            || !manifest_label(group.name)
            || !group_names.insert(group.name).second) {
            throw std::runtime_error(
                std::string{owner}
                + ": configure groups require contiguous ordinals and unique "
                  "lowercase names");
        }
        const json_value& binding_ordinals_value = require_member(
            value, "binding_ordinals", owner);
        if (binding_ordinals_value.kind != json_kind::array
            || binding_ordinals_value.children.empty()) {
            throw std::runtime_error(
                std::string{owner}
                + ": each configure group requires one or more bindings");
        }
        std::uint32_t previous_binding_ordinal{};
        bool saw_binding = false;
        for (const json_value& ordinal_value :
             binding_ordinals_value.children) {
            const std::uint64_t ordinal = require_unsigned(
                ordinal_value, "group.binding_ordinal");
            if (ordinal >= bindings.size()
                || ordinal > std::numeric_limits<std::uint32_t>::max()
                || (saw_binding && ordinal <= previous_binding_ordinal)
                || !grouped_bindings.insert(
                        static_cast<std::uint32_t>(ordinal))
                        .second) {
                throw std::runtime_error(
                    std::string{owner}
                    + ": group binding ordinals must be unique, source-ordered, "
                      "and in range");
            }
            saw_binding = true;
            previous_binding_ordinal = static_cast<std::uint32_t>(ordinal);
            group.binding_ordinals.push_back(previous_binding_ordinal);
            const configure_binding_record& binding =
                bindings[previous_binding_ordinal];
            if (binding.group != group.name || binding.role != group.role
                || binding.cases.size() != group.cases.size()) {
                throw std::runtime_error(
                    std::string{owner}
                    + ": group authority disagrees with a referenced binding");
            }
            for (std::size_t case_index = 0;
                 case_index < group.cases.size();
                 ++case_index) {
                if (group.cases[case_index].ordinal
                        != binding.cases[case_index].ordinal
                    || group.cases[case_index].label
                        != binding.cases[case_index].label) {
                    throw std::runtime_error(
                        std::string{owner}
                        + ": zipped binding labels disagree with the group "
                          "case order");
                }
            }
        }
        const configure_binding_record& first_binding =
            bindings[group.binding_ordinals.front()];
        if (group.cases.size() != first_binding.cases.size()) {
            throw std::runtime_error(
                std::string{owner}
                + ": group cases do not mirror the first binding");
        }
        for (std::size_t case_index = 0;
             case_index < group.cases.size();
            ++case_index) {
            if (group.cases[case_index].ordinal
                    != first_binding.cases[case_index].ordinal
                || group.cases[case_index].label
                    != first_binding.cases[case_index].label
                || group.cases[case_index].replacement
                    != first_binding.cases[case_index].replacement) {
                throw std::runtime_error(
                    std::string{owner}
                    + ": group cases must exactly mirror the first binding");
            }
        }
        groups.push_back(std::move(group));
    }
    if (grouped_bindings.size() != bindings.size()) {
        throw std::runtime_error(
            std::string{owner}
            + ": every binding must be owned by exactly one ordered group");
    }

    std::size_t coordinate_product = 1;
    for (const configure_group_record& group : groups) {
        if (coordinate_product > 4096U / group.cases.size()) {
            throw std::runtime_error(
                std::string{owner}
                + ": configure topology exceeds the coordinate ceiling");
        }
        coordinate_product *= group.cases.size();
    }
    const json_value& coordinates_value = require_member(
        root, "coordinates", owner);
    if (coordinates_value.kind != json_kind::array
        || coordinates_value.children.size() != coordinate_product) {
        throw std::runtime_error(
            std::string{owner}
            + ": coordinate count disagrees with the ordered group product");
    }
    std::vector<configure_coordinate_record> coordinates;
    coordinates.reserve(coordinate_product);
    for (std::size_t index = 0; index < coordinate_product; ++index) {
        const json_value& value = coordinates_value.children[index];
        require_exact_members(
            value,
            std::array{
                std::string_view{"ordinal"},
                std::string_view{"assignments"},
                std::string_view{"enabled"},
                std::string_view{"source"},
            },
            "configure topology coordinate");
        const std::uint64_t ordinal = require_unsigned(
            require_member(value, "ordinal", owner), "coordinate.ordinal");
        if (ordinal != index
            || ordinal > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error(
                std::string{owner}
                + ": coordinate ordinals must be contiguous from zero");
        }
        const json_value& assignments_value = require_member(
            value, "assignments", owner);
        if (assignments_value.kind != json_kind::array
            || assignments_value.children.size() != groups.size()) {
            throw std::runtime_error(
                std::string{owner}
                + ": coordinate must assign every group exactly once");
        }
        configure_coordinate_record coordinate{
            .ordinal = static_cast<std::uint32_t>(ordinal),
            .enabled = require_boolean(
                require_member(value, "enabled", owner),
                "coordinate.enabled"),
        };
        std::size_t remainder = index;
        std::vector<std::size_t> expected_case_ordinals(groups.size());
        for (std::size_t reverse = groups.size(); reverse != 0; --reverse) {
            const std::size_t group_index = reverse - 1;
            expected_case_ordinals[group_index] =
                remainder % groups[group_index].cases.size();
            remainder /= groups[group_index].cases.size();
        }
        for (std::size_t group_index = 0;
             group_index < groups.size();
             ++group_index) {
            const json_value& assignment_value =
                assignments_value.children[group_index];
            require_exact_members(
                assignment_value,
                std::array{
                    std::string_view{"group"},
                    std::string_view{"role"},
                    std::string_view{"case_ordinal"},
                    std::string_view{"label"},
                },
                "configure topology coordinate assignment");
            const std::uint64_t case_ordinal = require_unsigned(
                require_member(
                    assignment_value, "case_ordinal", owner),
                "assignment.case_ordinal");
            configure_assignment_record assignment{
                .group = std::string{require_string(
                    require_member(assignment_value, "group", owner),
                    "assignment.group")},
                .role = parse_manifest_axis_role(
                    require_string(
                        require_member(assignment_value, "role", owner),
                        "assignment.role"),
                    owner),
                .case_label = std::string{require_string(
                    require_member(assignment_value, "label", owner),
                    "assignment.label")},
                .case_ordinal = static_cast<std::uint32_t>(case_ordinal),
            };
            const configure_group_record& group = groups[group_index];
            const configure_case_record& expected_case =
                group.cases[expected_case_ordinals[group_index]];
            if (case_ordinal > std::numeric_limits<std::uint32_t>::max()
                || assignment.group != group.name
                || assignment.role != group.role
                || assignment.case_ordinal != expected_case.ordinal
                || assignment.case_label != expected_case.label) {
                throw std::runtime_error(
                    std::string{owner}
                    + ": coordinate assignment disagrees with ordered group "
                      "authority");
            }
            coordinate.assignments.push_back(std::move(assignment));
        }
        const json_value& coordinate_source = require_member(
            value, "source", owner);
        if (coordinate_source.kind == json_kind::string) {
            coordinate.source = checked_absolute_path(
                fs::path{coordinate_source.scalar},
                "configure coordinate source");
        } else if (coordinate_source.kind != json_kind::null) {
            throw std::runtime_error(
                std::string{owner}
                + ": coordinate source must be an absolute path or null");
        }
        const bool expected_enabled =
            configure_coordinate_enabled(coordinate, profile);
        const fs::path expected_source = output_root
            / ("instance_" + std::to_string(index) + ".cppm");
        if (coordinate.enabled != expected_enabled
            || coordinate.source.has_value() != coordinate.enabled
            || (coordinate.source.has_value()
                && *coordinate.source != expected_source)) {
            throw std::runtime_error(
                std::string{owner}
                + ": coordinate source or profile enablement is invalid");
        }
        coordinates.push_back(std::move(coordinate));
    }

    std::ostringstream canonical;
    canonical << "bioseq-template-flatten-topology\n"
                 "revision=2\n";
    canonical << "output-module\t" << output_module << '\n';
    canonical << "emission-policy\t" << spell_emission_policy(emission)
              << '\n';
    for (std::size_t group_index = 0;
         group_index < groups.size();
         ++group_index) {
        const configure_group_record& group = groups[group_index];
        canonical << "group\t" << group_index << '\t'
                  << axis_role_name(group.role) << '\t' << group.name << '\n';
        for (std::size_t binding_index = 0;
             binding_index < group.binding_ordinals.size();
             ++binding_index) {
            const configure_binding_record& binding =
                bindings[group.binding_ordinals[binding_index]];
            canonical << "binding\t" << group_index << '\t'
                      << binding_index << '\t' << binding.name << '\n';
            for (const configure_case_record& replacement : binding.cases) {
                canonical << "case\t" << group_index << '\t'
                          << binding_index << '\t' << replacement.ordinal
                          << '\t' << replacement.label << '\t'
                          << replacement.replacement << '\n';
            }
        }
    }
    for (const configure_coordinate_record& coordinate : coordinates) {
        canonical << "coordinate\t" << coordinate.ordinal;
        for (const configure_assignment_record& assignment :
             coordinate.assignments) {
            canonical << '\t' << assignment.group << '='
                      << assignment.case_label;
        }
        canonical << '\n';
    }
    if (bioseq::common::sha256_hex(canonical.str()) != topology_digest) {
        throw std::runtime_error(
            std::string{owner}
            + ": topology digest does not seal ordered assignment authority");
    }

    const json_value& enabled_ordinals_value = require_member(
        root, "enabled_ordinals", owner);
    const json_value& enabled_sources_value = require_member(
        root, "enabled_sources", owner);
    if (enabled_ordinals_value.kind != json_kind::array
        || enabled_sources_value.kind != json_kind::array) {
        throw std::runtime_error(
            std::string{owner}
            + ": enabled ordinals and sources must be JSON arrays");
    }
    std::size_t enabled_index = 0;
    for (const configure_coordinate_record& coordinate : coordinates) {
        if (!coordinate.enabled) continue;
        if (enabled_index >= enabled_ordinals_value.children.size()
            || enabled_index >= enabled_sources_value.children.size()
            || require_unsigned(
                   enabled_ordinals_value.children[enabled_index],
                   "enabled ordinal")
                != coordinate.ordinal
            || require_string(
                   enabled_sources_value.children[enabled_index],
                   "enabled source")
                != coordinate.source->generic_string()) {
            throw std::runtime_error(
                std::string{owner}
                + ": enabled output lists disagree with coordinate authority");
        }
        ++enabled_index;
    }
    if (enabled_index != enabled_ordinals_value.children.size()
        || enabled_index != enabled_sources_value.children.size()) {
        throw std::runtime_error(
            std::string{owner}
            + ": enabled output lists contain unauthoritative entries");
    }
    const bool selector_api_partitions =
        emission == emission_policy::selector_api
        && api_partition == output_root / "typed_flat_api.cppm"
        && selector_partition == output_root / "typed_flat_selector.cppm";
    const bool implementation_owner_partitions =
        emission == emission_policy::implementation_owners
        && !api_partition.has_value() && !selector_partition.has_value();
    if (!selector_api_partitions && !implementation_owner_partitions) {
        throw std::runtime_error(
            std::string{owner}
            + ": configured API/selector fields disagree with emission policy");
    }

    return configure_topology_provenance{
        .generator_identity = parsed_generator_identity,
        .policy = policy,
        .emission = emission,
        .name = name,
        .source_path = source_path,
        .source_sha256 = source_sha256,
        .output_root = output_root,
        .output_module = output_module,
        .profile = profile,
        .topology_digest = topology_digest,
        .content_sha256 = bioseq::common::sha256_hex(content),
        .coordinate_count = coordinates.size(),
    };
}

[[nodiscard]] inline fs::file_status symlink_status_or_missing(
    const fs::path& path,
    std::string_view owner) {
    std::error_code error;
    const fs::file_status status = fs::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error(
            std::string{owner} + ": cannot inspect " + path.string() + ": "
            + error.message());
    }
    if (error == std::errc::no_such_file_or_directory) {
        return fs::file_status{fs::file_type::not_found};
    }
    return status;
}

inline void reject_symlink_chain(
    const fs::path& path,
    std::string_view owner) {
    fs::path current;
    for (const fs::path& component : path) {
        current /= component;
        const fs::file_status status = symlink_status_or_missing(current, owner);
        if (status.type() == fs::file_type::not_found) continue;
        if (fs::is_symlink(status)) {
            throw std::runtime_error(
                std::string{owner} + ": symlink path component is forbidden: "
                + current.string());
        }
        if (current != path && !fs::is_directory(status)) {
            throw std::runtime_error(
                std::string{owner} + ": non-directory path component: "
                + current.string());
        }
    }
}

inline void require_regular_or_missing(
    const fs::path& path,
    std::string_view owner) {
    reject_symlink_chain(path, owner);
    const fs::file_status status = symlink_status_or_missing(path, owner);
    if (status.type() != fs::file_type::not_found
        && !fs::is_regular_file(status)) {
        throw std::runtime_error(
            std::string{owner} + ": refusing non-regular file "
            + path.string());
    }
}

inline void require_write_destination(
    const fs::path& path,
    std::string_view owner) {
    require_regular_or_missing(path, owner);
    const fs::path temporary = path.string() + ".tmp";
    const fs::file_status temporary_status =
        symlink_status_or_missing(temporary, owner);
    if (temporary_status.type() != fs::file_type::not_found) {
        throw std::runtime_error(
            std::string{owner}
            + ": refusing to overwrite an unauthenticated temporary path "
            + temporary.string());
    }
}

inline void validate_physical_root(const fs::path& root) {
    checked_absolute_path(root, "publication output root");
    std::error_code error;
    const fs::path physical = fs::weakly_canonical(root, error);
    if (error || physical != root) {
        throw std::runtime_error(
            "publication output root is not its own canonical filesystem identity: "
            + root.string());
    }
    reject_symlink_chain(root, "publication output root");
    const fs::file_status status = symlink_status_or_missing(
        root, "publication output root");
    if (status.type() != fs::file_type::not_found
        && !fs::is_directory(status)) {
        throw std::runtime_error(
            "publication output root exists but is not a directory: "
            + root.string());
    }
}

/// The publication protocol uses same-directory renames so POSIX rename(2),
/// as exposed by std::filesystem on macOS and Linux, replaces one regular file
/// atomically. Never emulate replacement by deleting the live destination.
inline void atomic_replace_regular_file(
    const fs::path& source,
    const fs::path& destination,
    std::string_view owner) {
    if (source.parent_path() != destination.parent_path()) {
        throw std::runtime_error(
            std::string{owner} + ": atomic replacement must stay in one directory");
    }
    require_regular_or_missing(source, owner);
    if (!fs::is_regular_file(symlink_status_or_missing(source, owner))) {
        throw std::runtime_error(
            std::string{owner} + ": replacement source is not a regular file: "
            + source.string());
    }
    require_regular_or_missing(destination, owner);
    std::error_code error;
    fs::rename(source, destination, error);
    if (error) {
        throw std::runtime_error(
            std::string{owner} + ": cannot atomically replace "
            + destination.string() + ": " + error.message());
    }
}

/// Writes one protocol-owned staging path. The caller must have authenticated
/// that the fixed staging name is absent before installing the PREPARING
/// intent. Closing and rereading before advancing the publication state makes
/// process-crash recovery deterministic; sudden host power loss is outside
/// this protocol's durability claim.
inline void write_new_staging_file(
    const fs::path& path,
    std::string_view content,
    std::string_view owner) {
    reject_symlink_chain(path.parent_path(), owner);
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) {
        throw std::runtime_error(
            std::string{owner} + ": cannot create staging directory "
            + path.parent_path().string() + ": " + error.message());
    }
    require_regular_or_missing(path, owner);
    if (symlink_status_or_missing(path, owner).type()
        != fs::file_type::not_found) {
        throw std::runtime_error(
            std::string{owner} + ": refusing an occupied staging path "
            + path.string());
    }
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error(
                std::string{owner} + ": cannot write " + path.string());
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.flush();
        if (!output) {
            throw std::runtime_error(
                std::string{owner} + ": cannot finish writing " + path.string());
        }
    }
    if (read_artifact_bytes(path) != content) {
        throw std::runtime_error(
            std::string{owner} + ": staged bytes failed readback validation: "
            + path.string());
    }
}

inline void remove_regular_file(
    const fs::path& path,
    std::string_view owner) {
    require_regular_or_missing(path, owner);
    const fs::file_status status = symlink_status_or_missing(path, owner);
    if (status.type() == fs::file_type::not_found) return;
    std::error_code error;
    if (!fs::remove(path, error) || error) {
        throw std::runtime_error(
            std::string{owner} + ": cannot remove " + path.string() + ": "
            + error.message());
    }
}

inline void prune_empty_publication_directories(
    fs::path parent,
    const fs::path& output_root) {
    std::error_code error;
    while (parent != output_root) {
        error.clear();
        if (!fs::remove(parent, error) || error) break;
        parent = parent.parent_path();
    }
}

}  // namespace detail

/// Renders the complete ordered topology handshake for one materialization.
[[nodiscard]] inline std::string render_topology_manifest(
    const generation_plan& plan) {
    detail::validate_plan_identity(plan);
    const std::string source_identity =
        detail::canonical_source_identity(plan);
    const materialization_region& region = plan.topology.region;
    std::ostringstream output;
    output << "{\n  \"schema_revision\": " << topology_schema_revision
           << ",\n  \"generator_identity\": ";
    detail::write_string(output, materialization_generator_identity);
    output << ",\n  \"policy\": ";
    detail::write_string(output, template_flatten_policy);
    output << ",\n  \"emission_policy\": ";
    detail::write_string(output, spell_emission_policy(plan.policy));
    output << ",\n  \"name\": ";
    detail::write_string(output, plan.name);
    output << ",\n  \"source\": {\"path\":";
    detail::write_string(output, source_identity);
    output << ",\"sha256\":";
    detail::write_string(output, plan.source_sha256);
    output << "},\n  \"source_module\": {\"name\":";
    detail::write_string(output, region.source_module.provided_module);
    output << ",\"is_interface\":"
           << (region.source_module.is_interface ? "true" : "false")
           << ",\"is_partition\":"
           << (region.source_module.is_partition ? "true" : "false")
           << ",\"declaration_range\":";
    detail::write_source_range(output, region.source_module.declaration_range);
    output << ",\"imports\":";
    detail::write_string_vector(output, region.source_module.imported_modules);
    output << "},\n  \"output_module\": ";
    detail::write_string(output, region.configured_output_module_name);
    output << ",\n  \"namespace\": {\"qualified_name\":";
    detail::write_string(output, region.boundary.qualified_name);
    output << ",\"usr\":";
    detail::write_string(output, region.boundary.stable_usr);
    output << ",\"provenance_id\":";
    detail::write_string(output, region.boundary.source_identity);
    output << ",\"declaration_range\":";
    detail::write_source_range(output, region.boundary.declaration_range);
    output << ",\"body_range\":";
    detail::write_source_range(output, region.boundary.body_range);
    output << "},\n  \"support_island\": ";
    if (!region.support_island.has_value()) {
        output << "null";
    } else {
        output << "{\"qualified_name\":";
        detail::write_string(output, region.support_island->qualified_name);
        output << ",\"usr\":";
        detail::write_string(output, region.support_island->stable_usr);
        output << ",\"provenance_id\":";
        detail::write_string(output, region.support_island->source_identity);
        output << ",\"declaration_range\":";
        detail::write_source_range(
            output, region.support_island->declaration_range);
        output << ",\"body_range\":";
        detail::write_source_range(output, region.support_island->body_range);
        output << '}';
    }
    output << ",\n  \"groups\": [\n";
    for (std::size_t group_index = 0;
         group_index < plan.topology.groups.size();
         ++group_index) {
        const axis_group& group = plan.topology.groups[group_index];
        output << "    {\"name\":";
        detail::write_string(output, group.name);
        output << ",\"role\":";
        detail::write_string(output, axis_role_name(group.role));
        output << ",\"binding_indices\":[";
        for (std::size_t index = 0;
             index < group.binding_indices.size();
             ++index) {
            if (index != 0) output << ',';
            output << group.binding_indices[index];
        }
        output << "],\"normalized_first_case_default\":";
        detail::write_string(output, group.cases.front().spelling);
        output << ",\"cases\":[";
        for (std::size_t case_index = 0;
             case_index < group.cases.size();
             ++case_index) {
            const replacement_case& replacement = group.cases[case_index];
            if (case_index != 0) output << ',';
            output << "{\"ordinal\":" << replacement.ordinal
                   << ",\"label\":";
            detail::write_string(output, replacement.label);
            output << ",\"replacement\":";
            detail::write_string(output, replacement.spelling);
            output << '}';
        }
        output << "]}"
               << (group_index + 1 == plan.topology.groups.size()
                       ? "\n"
                       : ",\n");
    }
    output << "  ],\n  \"bindings\": [\n";
    for (std::size_t binding_index = 0;
         binding_index < region.bindings.size();
         ++binding_index) {
        const binding_record& binding = region.bindings[binding_index];
        if (binding.cases.empty()) {
            throw std::runtime_error(
                "materialization binding has no replacement cases");
        }
        output << "    {\"index\":" << binding_index << ",\"group\":";
        detail::write_string(output, binding.group);
        output << ",\"role\":";
        detail::write_string(output, axis_role_name(binding.role));
        output << ",\"declaration_id\":";
        detail::write_string(
            output, detail::declaration_identity(binding.declaration));
        output << ",\"probe_usr\":";
        detail::write_string(output, binding.declaration.stable_usr);
        output << ",\"provenance_id\":";
        detail::write_string(output, binding.declaration.source_identity);
        output << ",\"name\":";
        detail::write_string(output, binding.declaration.qualified_name);
        output << ",\"declared_type\":";
        detail::write_string(output, binding.declaration.explicit_declared_type);
        output << ",\"declaration_range\":";
        detail::write_source_range(
            output, binding.declaration.declaration_range);
        output << ",\"normalized_first_case_default\":";
        detail::write_string(output, binding.cases.front().spelling);
        output << ",\"cases\":[";
        for (std::size_t case_index = 0;
             case_index < binding.cases.size();
             ++case_index) {
            const replacement_case& replacement = binding.cases[case_index];
            if (case_index != 0) output << ',';
            output << "{\"ordinal\":" << replacement.ordinal
                   << ",\"label\":";
            detail::write_string(output, replacement.label);
            output << ",\"replacement\":";
            detail::write_string(output, replacement.spelling);
            output << '}';
        }
        output << "]}"
               << (binding_index + 1 == region.bindings.size()
                       ? "\n"
                       : ",\n");
    }
    output << "  ],\n  \"controlled_declarations\": [\n";
    for (std::size_t declaration_index = 0;
         declaration_index < region.controlled_declarations.size();
         ++declaration_index) {
        const owned_declaration& declaration =
            region.controlled_declarations[declaration_index];
        output << "    {\"declaration_id\":";
        detail::write_string(
            output, detail::declaration_identity(declaration.declaration));
        output << ",\"probe_usr\":";
        detail::write_string(output, declaration.declaration.stable_usr);
        output << ",\"provenance_id\":";
        detail::write_string(output, declaration.declaration.source_identity);
        output << ",\"entry_role\":";
        detail::write_string(output, declaration.entry_role);
        output << ",\"generated_api_entry\":"
               << (declaration.generated_api_entry ? "true" : "false")
               << ",\"declaration_range\":";
        detail::write_source_range(
            output, declaration.declaration.declaration_range);
        output << ",\"selection\":[";
        for (std::size_t clause_index = 0;
             clause_index < declaration.clauses.size();
             ++clause_index) {
            const selection_clause& clause = declaration.clauses[clause_index];
            if (clause_index != 0) output << ',';
            output << "{\"group\":";
            detail::write_string(output, clause.group);
            output << ",\"accepted_case_labels\":";
            detail::write_string_vector(
                output, clause.accepted_case_labels);
            output << '}';
        }
        output << "]}"
               << (declaration_index + 1
                           == region.controlled_declarations.size()
                       ? "\n"
                       : ",\n");
    }
    output << "  ],\n  \"declaration_probes\": ";
    detail::write_declaration_probes(output, plan);
    output << ",\n  \"coordinates\": [\n";
    for (std::size_t coordinate_index = 0;
         coordinate_index < plan.topology.coordinates.size();
         ++coordinate_index) {
        const materialization_coordinate& coordinate =
            plan.topology.coordinates[coordinate_index];
        std::vector<std::string> selected_declarations;
        std::vector<std::string> pruned_declarations;
        std::vector<std::string> api_entries;
        std::set<std::string, std::less<>> identities;
        for (const owned_declaration& declaration :
             region.controlled_declarations) {
            const std::string identity =
                detail::declaration_identity(declaration.declaration);
            if (!identities.insert(identity).second) {
                throw std::runtime_error(
                    "controlled declarations repeat identity '" + identity + "'");
            }
            if (detail::coordinate_selects(declaration, coordinate)) {
                selected_declarations.push_back(identity);
                if (declaration.generated_api_entry) {
                    api_entries.push_back(identity);
                }
            } else {
                pruned_declarations.push_back(identity);
            }
        }
        output << "    {\"ordinal\":" << coordinate.ordinal
               << ",\"assignments\":[";
        for (std::size_t assignment_index = 0;
             assignment_index < coordinate.assignments.size();
             ++assignment_index) {
            const axis_case_assignment& assignment =
                coordinate.assignments[assignment_index];
            if (assignment_index != 0) output << ',';
            output << "{\"group\":";
            detail::write_string(output, assignment.group);
            output << ",\"role\":";
            detail::write_string(output, axis_role_name(assignment.role));
            output << ",\"case_label\":";
            detail::write_string(output, assignment.case_label);
            output << ",\"case_ordinal\":" << assignment.case_ordinal
                   << '}';
        }
        output << "],\"identity_namespace\":";
        detail::write_string(output, coordinate.identity_namespace);
        output << ",\"module\":";
        detail::write_string(output, coordinate.module_name);
        output << ",\"path\":";
        detail::write_string(output, coordinate.relative_path.generic_string());
        output << ",\"selected_declaration_ids\":";
        detail::write_declaration_identity_vector(
            output, selected_declarations);
        output << ",\"pruned_declaration_ids\":";
        detail::write_declaration_identity_vector(
            output, pruned_declarations);
        output << ",\"owned_api_entries\":";
        detail::write_declaration_identity_vector(output, api_entries);
        output << ",\"enabled_profiles\":[";
        const std::vector<std::string> enabled_profiles =
            enabled_profiles_for(coordinate);
        for (std::size_t profile_index = 0;
             profile_index < enabled_profiles.size();
             ++profile_index) {
            if (profile_index != 0) output << ',';
            detail::write_string(output, enabled_profiles[profile_index]);
        }
        output << "]}"
               << (coordinate_index + 1 == plan.topology.coordinates.size()
                       ? "\n"
                       : ",\n");
    }
    output << "  ],\n  \"identity_representative_ordinals\": [";
    for (std::size_t index = 0;
         index < plan.topology.identity_representative_ordinals.size();
         ++index) {
        if (index != 0) output << ',';
        output << plan.topology.identity_representative_ordinals[index];
    }
    output << "],\n  \"topology_digest\": ";
    detail::write_string(output, plan.topology.topology_digest);
    output << "\n}\n";
    return output.str();
}

/// Renders the one policy-neutral ownership envelope. Policy evidence is
/// opaque to this layer after the frontend proves that it is a JSON object.
[[nodiscard]] inline rendered_publication_manifest
render_publication_manifest(const publication_manifest_request& request) {
    const std::filesystem::path output_root = detail::checked_absolute_path(
        request.output_root, "publication output root");
    detail::require_sha256(request.topology_digest, "topology_digest");
    detail::require_sha256(
        request.prior_manifest_identity, "prior_manifest_identity", true);
    if (request.policy.empty() || request.policy.front() < 'a'
        || request.policy.front() > 'z'
        || !std::ranges::all_of(request.policy, [](const char character) {
               return (character >= 'a' && character <= 'z')
                   || (character >= '0' && character <= '9')
                   || character == '_';
           })) {
        throw std::runtime_error("publication policy is not a policy token");
    }
    if (request.name.empty()) {
        throw std::runtime_error("publication name must be non-empty");
    }
    std::string evidence = request.policy_evidence_json;
    if (evidence.ends_with('\n')) evidence.pop_back();
    if (evidence.empty() || evidence.front() != '{' || evidence.back() != '}'
        || evidence.contains('\0')) {
        throw std::runtime_error(
            "publication policy evidence must be one JSON object without "
            "outer whitespace");
    }
    const detail::json_value evidence_value =
        detail::json_parser{evidence, "publication policy evidence"}.parse();
    if (evidence_value.kind != detail::json_kind::object) {
        throw std::runtime_error(
            "publication policy evidence must be a JSON object");
    }

    std::vector<const publication_artifact*> ordered;
    ordered.reserve(request.artifacts.size());
    for (const publication_artifact& artifact : request.artifacts) {
        ordered.push_back(&artifact);
    }
    std::ranges::sort(
        ordered,
        {},
        [](const publication_artifact* artifact) {
            return artifact->relative_path.generic_string();
        });
    if (ordered.empty()) {
        throw std::runtime_error(
            "publication manifest must own at least one artifact");
    }
    std::string previous_path;
    for (const publication_artifact* artifact : ordered) {
        const std::filesystem::path relative = detail::checked_relative_path(
            artifact->relative_path, "publication artifact");
        const std::string current_path = relative.generic_string();
        if (!previous_path.empty() && previous_path >= current_path) {
            throw std::runtime_error(
                "publication artifact paths must be unique");
        }
        previous_path = current_path;
        const bool production = artifact->artifact_class
            == publication_artifact_class::production_cppm;
        if ((production && relative.extension() != ".cppm")
            || (!production && relative.extension() != ".json")) {
            throw std::runtime_error(
                "publication artifact extension disagrees with its class: "
                + current_path);
        }
        const std::string digest = bioseq::common::sha256_hex(artifact->content);
        if (artifact->content_sha256 != digest) {
            throw std::runtime_error(
                "publication artifact content SHA-256 mismatch: "
                + current_path);
        }
    }

    std::ostringstream output;
    output << "{\n  \"publication_schema_revision\": "
           << publication_schema_revision << ",\n  \"policy\": ";
    detail::write_string(output, request.policy);
    output << ",\n  \"name\": ";
    detail::write_string(output, request.name);
    output << ",\n  \"output_root\": ";
    detail::write_string(output, output_root.generic_string());
    output << ",\n  \"topology_digest\": ";
    detail::write_string(output, request.topology_digest);
    output << ",\n  \"prior_manifest_identity\": ";
    detail::write_string(output, request.prior_manifest_identity);
    output << ",\n  \"artifacts\": [\n";
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        const publication_artifact& artifact = *ordered[index];
        output << "    {\"path\":";
        detail::write_string(output, artifact.relative_path.generic_string());
        output << ",\"sha256\":";
        detail::write_string(output, artifact.content_sha256);
        output << ",\"class\":";
        detail::write_string(
            output, publication_class_name(artifact.artifact_class));
        output << '}' << (index + 1 == ordered.size() ? "\n" : ",\n");
    }
    output << "  ],\n  \"evidence\": " << evidence << "\n}\n";
    const std::string unsigned_content = output.str();
    const std::string identity = bioseq::common::sha256_hex(unsigned_content);
    const std::string sealed = detail::seal_manifest(
        unsigned_content, identity);
    static_cast<void>(detail::parse_publication_manifest(
        sealed,
        "rendered publication manifest",
        output_root,
        request.policy,
        request.topology_digest));
    return rendered_publication_manifest{
        .json = sealed,
        .manifest_identity = identity,
    };
}

/// Renders and self-seals template_flatten policy evidence inside the shared
/// ownership envelope.
[[nodiscard]] inline rendered_publication_manifest render_artifact_manifest(
    const generation_plan& plan,
    const std::vector<materialization_artifact>& artifacts,
    std::string_view prior_identity) {
    detail::validate_plan_identity(plan);
    detail::require_sha256(
        prior_identity, "prior_manifest_identity", true);
    if (!plan.previous_manifest_identity.empty()
        && plan.previous_manifest_identity != prior_identity) {
        throw std::runtime_error(
            "generation plan and renderer disagree on prior manifest identity");
    }
    const std::string source_identity =
        detail::canonical_source_identity(plan);
    std::vector<const materialization_artifact*> ordered;
    ordered.reserve(artifacts.size());
    for (const materialization_artifact& artifact : artifacts) {
        ordered.push_back(&artifact);
    }
    std::ranges::sort(
        ordered,
        {},
        [](const materialization_artifact* artifact) {
            return artifact->relative_path.generic_string();
        });

    std::string previous_path;
    for (const materialization_artifact* artifact : ordered) {
        const std::filesystem::path relative = detail::checked_relative_path(
            artifact->relative_path, "rendered materialization artifact");
        const bool production =
            artifact->artifact_kind != materialization_artifact::kind::manifest;
        if ((production && relative.extension() != ".cppm")
            || (!production && relative.extension() != ".json")) {
            throw std::runtime_error(
                "rendered artifact extension disagrees with its artifact kind: "
                + relative.string());
        }
        const std::string current_path = relative.generic_string();
        if (!previous_path.empty() && previous_path >= current_path) {
            throw std::runtime_error(
                "rendered artifact paths must be unique");
        }
        previous_path = current_path;
        const std::string digest = bioseq::common::sha256_hex(artifact->content);
        if (!artifact->content_sha256.empty()
            && artifact->content_sha256 != digest) {
            throw std::runtime_error(
                "rendered artifact content SHA-256 mismatch: " + current_path);
        }
        for (const std::uint32_t ordinal :
             artifact->logical_coordinate_ordinals) {
            if (ordinal >= plan.topology.coordinates.size()) {
                throw std::runtime_error(
                    "rendered artifact references an unknown logical coordinate");
            }
        }
    }
    const std::size_t rendered_probe_count = std::ranges::count_if(
        ordered,
        [](const materialization_artifact* artifact) {
            return artifact->artifact_kind
                == materialization_artifact::kind::declaration_probe;
        });
    if (plan.policy == emission_policy::implementation_owners) {
        if (rendered_probe_count != 0U) {
            throw std::runtime_error(
                "implementation-owner declaration probes are provisional and must not be published");
        }
    } else {
        std::set<const materialization_artifact*> matched_probe_artifacts;
        for (const structural_analysis::translation_unit& probe :
             plan.declaration_probes) {
            const std::filesystem::path probe_source = detail::path_beneath_root(
                probe.source_path,
                plan.output_root,
                "declaration probe source");
            const std::vector<const materialization_artifact*>::const_iterator
                artifact = std::ranges::find_if(
                ordered,
                [&probe, &probe_source, &matched_probe_artifacts](
                    const materialization_artifact* candidate) {
                    return candidate->artifact_kind
                            == materialization_artifact::kind::declaration_probe
                        && candidate->relative_path == probe_source
                        && candidate->module_name
                            == probe.source_module.provided_module
                        && bioseq::common::sha256_hex(candidate->content)
                            == probe.source_sha256
                        && !matched_probe_artifacts.contains(candidate);
                });
            if (artifact == ordered.end()) {
                throw std::runtime_error(
                    "body-free declaration probe evidence does not match its "
                    "rendered artifact: "
                    + probe_source.string());
            }
            matched_probe_artifacts.insert(*artifact);
        }
        if (matched_probe_artifacts.size() != rendered_probe_count) {
            throw std::runtime_error(
                "body-free declaration probe evidence must bijectively cover its rendered artifacts");
        }
    }

    std::ostringstream evidence;
    evidence << "{\n  \"schema_revision\": " << artifact_schema_revision
           << ",\n  \"generator_identity\": ";
    detail::write_string(evidence, materialization_generator_identity);
    evidence << ",\n  \"emission_policy\": ";
    detail::write_string(evidence, spell_emission_policy(plan.policy));
    evidence << ",\n  \"profile\": ";
    detail::write_string(evidence, plan.profile);
    evidence << ",\n  \"source\": {\"path\":";
    detail::write_string(evidence, source_identity);
    evidence << ",\"sha256\":";
    detail::write_string(evidence, plan.source_sha256);
    evidence << "},\n  \"source_module\": ";
    detail::write_string(
        evidence, plan.topology.region.source_module.provided_module);
    evidence << ",\n  \"output_module\": ";
    detail::write_string(
        evidence, plan.topology.region.configured_output_module_name);
    evidence << ",\n  \"frontend_action_ledger\": {"
           << "\"dependency_preparation_validations\":"
           << plan.frontend_actions.dependency_preparation_validations
           << ",\"canonical_token_slice_scans\":"
           << plan.frontend_actions.canonical_token_slice_scans
           << ",\"signature_donor_token_slice_scans\":"
           << plan.frontend_actions.signature_donor_token_slice_scans
           << ",\"body_free_declaration_probe_scans\":"
           << plan.frontend_actions.body_free_declaration_probe_scans
           << ",\"artifact_render_passes\":"
           << plan.frontend_actions.artifact_render_passes
           << ",\"body_bearing_compiler_invocations\":"
           << plan.frontend_actions.body_bearing_compiler_invocations
           << "},\n  \"cmake_compilation_obligations\": {"
           << "\"api_partition_units\":"
           << plan.compilation_obligations.api_partition_units
           << ",\"selector_partition_units\":"
           << plan.compilation_obligations.selector_partition_units
           << ",\"body_bearing_implementation_units\":"
           << plan.compilation_obligations.body_bearing_implementation_units
           << "},\n  \"compiler\": {\"clang_version\":";
    detail::write_string(evidence, plan.clang_release);
    evidence << ",\"compilation_signature\":";
    detail::write_string(evidence, plan.compilation_signature);
    evidence << ",\"compiler_module_inventory_sha256\":";
    detail::write_string(evidence, plan.compiler_module_inventory_sha256);
    evidence << "},\n  \"declaration_probes\": ";
    detail::write_declaration_probes(evidence, plan);
    evidence << ",\n  \"artifacts\": [\n";
    std::vector<publication_artifact> publication;
    publication.reserve(ordered.size());
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        const materialization_artifact& artifact = *ordered[index];
        const bool production =
            artifact.artifact_kind != materialization_artifact::kind::manifest;
        evidence << "    {\"path\":";
        detail::write_string(
            evidence, artifact.relative_path.generic_string());
        evidence << ",\"kind\":";
        detail::write_string(
            evidence, artifact_kind_name(artifact.artifact_kind));
        evidence << ",\"module\":";
        detail::write_string(evidence, artifact.module_name);
        evidence << ",\"logical_coordinates\":[";
        for (std::size_t ordinal_index = 0;
             ordinal_index < artifact.logical_coordinate_ordinals.size();
             ++ordinal_index) {
            if (ordinal_index != 0) evidence << ',';
            evidence << artifact.logical_coordinate_ordinals[ordinal_index];
        }
        evidence << "],\"validation_class\":";
        detail::write_string(
            evidence, validation_class_name(artifact.validation));
        evidence << '}'
               << (index + 1 == ordered.size() ? "\n" : ",\n");
        publication.push_back(publication_artifact{
            .relative_path = artifact.relative_path,
            .content = artifact.content,
            .content_sha256 = bioseq::common::sha256_hex(artifact.content),
            .artifact_class = production
                ? publication_artifact_class::production_cppm
                : publication_artifact_class::metadata_json,
        });
    }
    evidence << "  ]\n}";
    rendered_publication_manifest rendered = render_publication_manifest(
        publication_manifest_request{
            .output_root = plan.output_root,
            .policy = std::string{template_flatten_policy},
            .name = plan.name,
            .topology_digest = plan.topology.topology_digest,
            .prior_manifest_identity = std::string{prior_identity},
            .policy_evidence_json = evidence.str(),
            .artifacts = std::move(publication),
        });
    detail::validate_template_flatten_publication_manifest(
        rendered.json,
        "rendered template_flatten publication manifest",
        plan.output_root,
        plan.topology.topology_digest);
    return rendered;
}

/// Reads and authenticates the canonical configure-time topology snapshot.
[[nodiscard]] inline configure_topology_provenance
read_configure_topology_provenance(
    const std::filesystem::path& path) {
    detail::require_regular_or_missing(path, "configure topology manifest");
    const std::filesystem::file_status status =
        detail::symlink_status_or_missing(path, "configure topology manifest");
    if (status.type() == std::filesystem::file_type::not_found) {
        throw std::runtime_error(
            "configure topology manifest does not exist: " + path.string());
    }
    const std::string content = read_artifact_bytes(path);
    if (content.contains('\0')) {
        throw std::runtime_error(
            path.string() + ": configure topology manifest contains NUL");
    }
    return detail::parse_configure_topology_provenance(
        content, path.string());
}

/// Reads only the digest while retaining the strict provenance validation.
[[nodiscard]] inline std::string read_configure_topology_digest(
    const std::filesystem::path& path) {
    return read_configure_topology_provenance(path).topology_digest;
}

/// Authenticates a prior artifacts manifest before it can authorize cleanup.
/// An empty expected topology accepts any valid self-sealed digest so a
/// reconfigured axis inventory can remove stale predecessor artifacts.
namespace detail {

struct previous_publication_snapshot {
    previous_publication publication;
    std::string content;
};

enum class publication_fault_point : std::uint8_t {
    preparing_intent_durable,
    desired_artifact_staged,
    ready_intent_durable,
    desired_artifact_promoted,
    stale_artifact_removed,
    manifest_committed,
};

struct publication_fault_injection {
    publication_fault_point point{};
    std::size_t ordinal{};
};

inline thread_local std::optional<publication_fault_injection>
    publication_fault_for_testing;

inline void maybe_inject_publication_fault(
    const publication_fault_point point,
    const std::size_t ordinal = 0U) {
    if (!publication_fault_for_testing.has_value()
        || publication_fault_for_testing->point != point
        || publication_fault_for_testing->ordinal != ordinal) {
        return;
    }
    publication_fault_for_testing.reset();
    throw std::runtime_error("injected publication interruption");
}

struct publication_protocol_paths {
    std::filesystem::path output_root;
    std::filesystem::path manifest;
    std::filesystem::path manifest_relative;
    std::filesystem::path stamp;
    std::filesystem::path stamp_relative;
    std::filesystem::path transaction;
    std::filesystem::path transaction_relative;
    std::filesystem::path transaction_scratch;
    std::filesystem::path ready;
    std::filesystem::path ready_relative;
};

[[nodiscard]] inline bool publication_paths_overlap(
    const std::filesystem::path& left,
    const std::filesystem::path& right) noexcept {
    std::filesystem::path::iterator left_component = left.begin();
    std::filesystem::path::iterator right_component = right.begin();
    while (left_component != left.end() && right_component != right.end()
           && *left_component == *right_component) {
        ++left_component;
        ++right_component;
    }
    return left_component == left.end() || right_component == right.end();
}

inline void reserve_publication_path(
    std::set<std::filesystem::path>& identities,
    const std::filesystem::path& relative,
    std::string_view owner) {
    for (const std::filesystem::path& candidate : std::array{
             relative,
             std::filesystem::path{relative.string() + ".tmp"},
         }) {
        if (std::ranges::any_of(
                identities,
                [&candidate](const std::filesystem::path& existing) {
                    return publication_paths_overlap(existing, candidate);
                })) {
            throw std::runtime_error(
                std::string{owner}
                + " aliases or nests beneath another publication path: "
                + relative.string());
        }
        identities.insert(candidate);
    }
}

[[nodiscard]] inline publication_protocol_paths publication_protocol(
    const std::filesystem::path& output_root,
    const std::filesystem::path& manifest,
    const std::filesystem::path& stamp) {
    validate_physical_root(output_root);
    const std::filesystem::path manifest_relative = path_beneath_root(
        manifest, output_root, "artifact manifest destination");
    if (manifest_relative.extension() != ".json") {
        throw std::runtime_error(
            "artifact manifest destination must use .json");
    }
    const std::filesystem::path stamp_relative = path_beneath_root(
        stamp, output_root, "publication stamp destination");
    const std::filesystem::path transaction{
        manifest.string() + ".transaction"};
    const std::filesystem::path ready{manifest.string() + ".ready"};
    publication_protocol_paths result{
        .output_root = output_root,
        .manifest = manifest,
        .manifest_relative = manifest_relative,
        .stamp = stamp,
        .stamp_relative = stamp_relative,
        .transaction = transaction,
        .transaction_relative = path_beneath_root(
            transaction, output_root, "publication PREPARING intent"),
        .transaction_scratch =
            std::filesystem::path{transaction.string() + ".tmp"},
        .ready = ready,
        .ready_relative = path_beneath_root(
            ready, output_root, "publication READY intent"),
    };
    std::set<std::filesystem::path> identities;
    reserve_publication_path(
        identities, result.manifest_relative, "artifact manifest destination");
    reserve_publication_path(
        identities, result.stamp_relative, "publication stamp destination");
    reserve_publication_path(
        identities,
        result.transaction_relative,
        "publication PREPARING intent");
    reserve_publication_path(
        identities, result.ready_relative, "publication READY intent");
    return result;
}

[[nodiscard]] inline previous_publication_snapshot
read_previous_publication_snapshot(
    const std::filesystem::path& manifest_path,
    const std::filesystem::path& expected_output_root,
    std::string_view expected_policy,
    std::string_view expected_topology_digest) {
    detail::require_sha256(
        expected_topology_digest, "expected_topology_digest", true);
    const std::filesystem::file_status status =
        detail::symlink_status_or_missing(
            manifest_path, "previous publication manifest");
    if (status.type() == std::filesystem::file_type::not_found) return {};
    detail::require_regular_or_missing(
        manifest_path, "previous publication manifest");
    std::string content = read_artifact_bytes(manifest_path);
    if (content.contains('\0')) {
        throw std::runtime_error(
            manifest_path.string() + ": artifacts manifest contains NUL");
    }
    if (expected_policy == template_flatten_policy) {
        detail::validate_template_flatten_publication_manifest(
            content,
            manifest_path.string(),
            expected_output_root,
            expected_topology_digest);
    }
    previous_publication publication = detail::parse_publication_manifest(
        content, manifest_path.string(), expected_output_root, expected_policy,
        expected_topology_digest);
    return previous_publication_snapshot{
        .publication = std::move(publication),
        .content = std::move(content),
    };
}

inline void require_unchanged_previous_publication_snapshot(
    const std::filesystem::path& manifest_path,
    const previous_publication_snapshot& snapshot) {
    const std::filesystem::file_status status = symlink_status_or_missing(
        manifest_path, "revalidated previous publication manifest");
    if (!snapshot.publication.present) {
        if (status.type() != std::filesystem::file_type::not_found) {
            throw std::runtime_error(
                "previous publication manifest appeared after authentication");
        }
        return;
    }
    require_regular_or_missing(
        manifest_path, "revalidated previous publication manifest");
    if (status.type() == std::filesystem::file_type::not_found
        || read_artifact_bytes(manifest_path) != snapshot.content) {
        throw std::runtime_error(
            "previous publication manifest changed after authentication");
    }
}

inline void require_pending_predecessor(
    const previous_publication& desired,
    const previous_publication& previous) {
    if ((!previous.present && !desired.prior_manifest_identity.empty())
        || (previous.present
            && previous.manifest_identity != desired.prior_manifest_identity)
        || (previous.present && previous.name != desired.name)) {
        throw std::runtime_error(
            "pending publication is not linked to the live manifest identity");
    }
}

inline void validate_protocol_ownership_paths(
    const publication_protocol_paths& protocol,
    const previous_publication& desired,
    const previous_publication& previous) {
    std::set<std::filesystem::path> identities;
    reserve_publication_path(
        identities, protocol.manifest_relative, "artifact manifest destination");
    reserve_publication_path(
        identities, protocol.stamp_relative, "publication stamp destination");
    reserve_publication_path(
        identities,
        protocol.transaction_relative,
        "publication PREPARING intent");
    reserve_publication_path(
        identities, protocol.ready_relative, "publication READY intent");
    std::set<std::filesystem::path> desired_paths;
    for (const owned_publication_artifact& artifact : desired.artifacts) {
        reserve_publication_path(
            identities, artifact.relative_path, "desired publication artifact");
        desired_paths.insert(artifact.relative_path);
    }
    for (const owned_publication_artifact& artifact : previous.artifacts) {
        if (desired_paths.contains(artifact.relative_path)) continue;
        reserve_publication_path(
            identities,
            artifact.relative_path,
            "authenticated stale publication artifact");
    }
}

[[nodiscard]] inline std::optional<std::string> regular_file_digest(
    const std::filesystem::path& path,
    std::string_view owner) {
    require_regular_or_missing(path, owner);
    const std::filesystem::file_status status =
        symlink_status_or_missing(path, owner);
    if (status.type() == std::filesystem::file_type::not_found) {
        return std::nullopt;
    }
    return bioseq::common::sha256_hex(read_artifact_bytes(path));
}

[[nodiscard]] inline const owned_publication_artifact* find_owned_artifact(
    const previous_publication& publication,
    const std::filesystem::path& relative) noexcept {
    const std::vector<owned_publication_artifact>::const_iterator found =
        std::ranges::find_if(
        publication.artifacts,
        [&relative](const owned_publication_artifact& artifact) {
            return artifact.relative_path == relative;
        });
    return found == publication.artifacts.end() ? nullptr : &*found;
}

enum class pending_publication_state : std::uint8_t {
    preparing,
    ready,
};

struct pending_publication {
    pending_publication_state state{};
    std::filesystem::path path;
    std::string content;
    previous_publication desired;
};

[[nodiscard]] inline std::optional<pending_publication>
read_pending_publication(
    const publication_protocol_paths& protocol,
    std::string_view expected_policy) {
    const std::filesystem::path manifest_scratch{
        protocol.manifest.string() + ".tmp"};
    const std::filesystem::path stamp_scratch{protocol.stamp.string() + ".tmp"};
    const std::filesystem::path ready_scratch{protocol.ready.string() + ".tmp"};
    const std::array<
        std::pair<std::filesystem::path, std::string_view>,
        3> scratch_paths{
        std::pair{manifest_scratch, std::string_view{"manifest scratch"}},
        std::pair{stamp_scratch, std::string_view{"stamp scratch"}},
        std::pair{ready_scratch, std::string_view{"READY scratch"}},
    };
    for (const std::pair<std::filesystem::path, std::string_view>& scratch
         : scratch_paths) {
        const std::filesystem::path& path = scratch.first;
        const std::string_view owner = scratch.second;
        require_regular_or_missing(path, owner);
        if (symlink_status_or_missing(path, owner).type()
            != std::filesystem::file_type::not_found) {
            throw std::runtime_error(
                std::string{owner}
                + " is not owned by a recoverable publication state: "
                + path.string());
        }
    }

    const std::filesystem::file_status preparing_status =
        symlink_status_or_missing(
            protocol.transaction, "publication PREPARING intent");
    const std::filesystem::file_status ready_status = symlink_status_or_missing(
        protocol.ready, "publication READY intent");
    const std::filesystem::file_status scratch_status =
        symlink_status_or_missing(
            protocol.transaction_scratch,
            "publication PREPARING intent scratch");
    if (scratch_status.type() != std::filesystem::file_type::not_found) {
        require_regular_or_missing(
            protocol.transaction_scratch,
            "publication PREPARING intent scratch");
        if (preparing_status.type() != std::filesystem::file_type::not_found
            || ready_status.type() != std::filesystem::file_type::not_found) {
            throw std::runtime_error(
                "publication PREPARING scratch aliases an installed state");
        }
        // Staging never begins until this scratch has been atomically renamed
        // to PREPARING, so an isolated regular scratch is always abortable.
        remove_regular_file(
            protocol.transaction_scratch,
            "publication PREPARING intent scratch");
        return std::nullopt;
    }
    if (preparing_status.type() != std::filesystem::file_type::not_found
        && ready_status.type() != std::filesystem::file_type::not_found) {
        throw std::runtime_error(
            "ambiguous publication state contains PREPARING and READY intents");
    }
    if (preparing_status.type() == std::filesystem::file_type::not_found
        && ready_status.type() == std::filesystem::file_type::not_found) {
        return std::nullopt;
    }
    const pending_publication_state state =
        preparing_status.type() != std::filesystem::file_type::not_found
        ? pending_publication_state::preparing
        : pending_publication_state::ready;
    const std::filesystem::path& path =
        state == pending_publication_state::preparing ? protocol.transaction
                                                       : protocol.ready;
    require_regular_or_missing(path, "pending publication intent");
    std::string content = read_artifact_bytes(path);
    if (content.contains('\0')) {
        throw std::runtime_error(
            "pending publication intent contains NUL bytes");
    }
    if (expected_policy == template_flatten_policy) {
        validate_template_flatten_publication_manifest(
            content,
            "pending publication intent",
            protocol.output_root,
            std::string_view{});
    }
    previous_publication desired = parse_publication_manifest(
        content,
        "pending publication intent",
        protocol.output_root,
        expected_policy,
        std::string_view{});
    return pending_publication{
        .state = state,
        .path = path,
        .content = std::move(content),
        .desired = std::move(desired),
    };
}

inline void require_pending_content_unchanged(
    const pending_publication& pending) {
    require_regular_or_missing(pending.path, "revalidated publication intent");
    if (read_artifact_bytes(pending.path) != pending.content) {
        throw std::runtime_error(
            "pending publication intent changed after authentication");
    }
}

inline void abort_preparing_publication(
    const publication_protocol_paths& protocol,
    const pending_publication& pending,
    const previous_publication_snapshot& previous_snapshot) {
    require_pending_content_unchanged(pending);
    require_unchanged_previous_publication_snapshot(
        protocol.manifest, previous_snapshot);
    for (const owned_publication_artifact& artifact :
         pending.desired.artifacts) {
        const std::filesystem::path staging = std::filesystem::path{
            (protocol.output_root / artifact.relative_path).string() + ".tmp"};
        remove_regular_file(staging, "aborted publication staging artifact");
    }
    require_pending_content_unchanged(pending);
    require_unchanged_previous_publication_snapshot(
        protocol.manifest, previous_snapshot);
    remove_regular_file(pending.path, "aborted publication PREPARING intent");
}

inline void roll_forward_ready_publication(
    const publication_protocol_paths& protocol,
    const pending_publication& pending,
    const previous_publication_snapshot& previous_snapshot) {
    struct promotion {
        const owned_publication_artifact* desired{};
        const owned_publication_artifact* previous{};
        std::filesystem::path destination;
        std::filesystem::path staging;
    };
    std::vector<promotion> promotions;
    std::set<std::filesystem::path> desired_paths;
    for (const owned_publication_artifact& artifact :
         pending.desired.artifacts) {
        desired_paths.insert(artifact.relative_path);
        const std::filesystem::path destination =
            protocol.output_root / artifact.relative_path;
        const std::filesystem::path staging{
            destination.string() + ".tmp"};
        const std::optional<std::string> live_digest = regular_file_digest(
            destination, "READY desired publication artifact");
        const std::optional<std::string> staged_digest = regular_file_digest(
            staging, "READY staged publication artifact");
        if (live_digest == artifact.content_sha256) {
            if (staged_digest.has_value()) {
                throw std::runtime_error(
                    "READY publication has both live and staged desired bytes: "
                    + artifact.relative_path.string());
            }
            continue;
        }
        if (staged_digest != artifact.content_sha256) {
            throw std::runtime_error(
                "READY publication is missing authenticated staged bytes: "
                + artifact.relative_path.string());
        }
        const owned_publication_artifact* prior = find_owned_artifact(
            previous_snapshot.publication, artifact.relative_path);
        if (live_digest.has_value()
            && (prior == nullptr
                || prior->content_sha256 != live_digest.value())) {
            throw std::runtime_error(
                "READY publication would overwrite unowned or modified bytes: "
                + artifact.relative_path.string());
        }
        promotions.push_back(promotion{
            .desired = &artifact,
            .previous = prior,
            .destination = destination,
            .staging = staging,
        });
    }

    std::vector<const owned_publication_artifact*> stale_artifacts;
    for (const owned_publication_artifact& artifact :
         previous_snapshot.publication.artifacts) {
        if (desired_paths.contains(artifact.relative_path)) continue;
        const std::filesystem::path stale =
            protocol.output_root / artifact.relative_path;
        const std::optional<std::string> digest = regular_file_digest(
            stale, "READY stale publication artifact");
        if (digest.has_value() && digest != artifact.content_sha256) {
            throw std::runtime_error(
                "READY publication would delete modified stale bytes: "
                + artifact.relative_path.string());
        }
        stale_artifacts.push_back(&artifact);
    }

    require_pending_content_unchanged(pending);
    require_unchanged_previous_publication_snapshot(
        protocol.manifest, previous_snapshot);
    for (std::size_t ordinal = 0U; ordinal < promotions.size(); ++ordinal) {
        const promotion& item = promotions[ordinal];
        if (regular_file_digest(
                item.staging, "revalidated staged publication artifact")
            != item.desired->content_sha256) {
            throw std::runtime_error(
                "staged publication artifact changed during promotion: "
                + item.staging.string());
        }
        const std::optional<std::string> live_digest = regular_file_digest(
            item.destination, "revalidated live publication artifact");
        if (live_digest.has_value()
            && (item.previous == nullptr
                || live_digest != item.previous->content_sha256)) {
            throw std::runtime_error(
                "live publication artifact changed during promotion: "
                + item.destination.string());
        }
        atomic_replace_regular_file(
            item.staging, item.destination, "publication artifact promotion");
        if (regular_file_digest(
                item.destination, "promoted publication artifact")
            != item.desired->content_sha256) {
            throw std::runtime_error(
                "promoted publication artifact digest mismatch: "
                + item.destination.string());
        }
        maybe_inject_publication_fault(
            publication_fault_point::desired_artifact_promoted, ordinal);
    }
    for (std::size_t ordinal = 0U; ordinal < stale_artifacts.size(); ++ordinal) {
        const owned_publication_artifact& artifact = *stale_artifacts[ordinal];
        const std::filesystem::path stale =
            protocol.output_root / artifact.relative_path;
        const std::optional<std::string> digest = regular_file_digest(
            stale, "revalidated stale publication artifact");
        if (!digest.has_value()) continue;
        if (digest != artifact.content_sha256) {
            throw std::runtime_error(
                "stale publication artifact changed during deletion: "
                + stale.string());
        }
        remove_regular_file(stale, "authenticated stale publication artifact");
        prune_empty_publication_directories(
            stale.parent_path(), protocol.output_root);
        maybe_inject_publication_fault(
            publication_fault_point::stale_artifact_removed, ordinal);
    }
    require_pending_content_unchanged(pending);
    require_unchanged_previous_publication_snapshot(
        protocol.manifest, previous_snapshot);
    atomic_replace_regular_file(
        protocol.ready, protocol.manifest, "publication manifest commit");
    if (read_artifact_bytes(protocol.manifest) != pending.content) {
        throw std::runtime_error(
            "committed publication manifest bytes differ from READY intent");
    }
    maybe_inject_publication_fault(
        publication_fault_point::manifest_committed);
}

}  // namespace detail

/// Resolves a process interruption before callers authenticate the predecessor
/// used to render their next manifest. PREPARING is abort-only; READY is
/// roll-forward-only, preserving one exact manifest identity chain even when
/// the caller's current inputs changed after the interrupted invocation.
inline void recover_interrupted_publication(
    const std::filesystem::path& output_root,
    const std::filesystem::path& artifact_manifest,
    const std::filesystem::path& stamp,
    std::string_view expected_policy) {
    const detail::publication_protocol_paths protocol =
        detail::publication_protocol(output_root, artifact_manifest, stamp);
    const std::optional<detail::pending_publication> pending =
        detail::read_pending_publication(protocol, expected_policy);
    if (!pending.has_value()) return;
    const detail::previous_publication_snapshot previous_snapshot =
        detail::read_previous_publication_snapshot(
            artifact_manifest,
            output_root,
            expected_policy,
            std::string_view{});
    detail::require_pending_predecessor(
        pending->desired, previous_snapshot.publication);
    detail::validate_protocol_ownership_paths(
        protocol, pending->desired, previous_snapshot.publication);
    if (pending->state == detail::pending_publication_state::preparing) {
        detail::abort_preparing_publication(
            protocol, *pending, previous_snapshot);
        return;
    }
    detail::roll_forward_ready_publication(
        protocol, *pending, previous_snapshot);
}

[[nodiscard]] inline previous_publication read_previous_publication(
    const std::filesystem::path& manifest_path,
    const std::filesystem::path& expected_output_root,
    std::string_view expected_policy,
    std::string_view expected_topology_digest) {
    return detail::read_previous_publication_snapshot(
               manifest_path,
               expected_output_root,
               expected_policy,
               expected_topology_digest)
        .publication;
}

/// Atomically publishes the complete desired set and removes only stale paths
/// authenticated by the immediately preceding manifest identity.
inline void publish_artifacts(const publication_request& request) {
    const detail::publication_protocol_paths protocol =
        detail::publication_protocol(
            request.output_root, request.artifact_manifest, request.stamp);

    const previous_publication desired_manifest =
        detail::parse_publication_manifest(
            request.artifact_manifest_json,
            "desired artifacts manifest",
            request.output_root,
            std::string_view{},
            std::string_view{});
    if (desired_manifest.policy == template_flatten_policy) {
        detail::validate_template_flatten_publication_manifest(
            request.artifact_manifest_json,
            "desired artifacts manifest",
            request.output_root,
            desired_manifest.topology_digest);
    }

    // Defense in depth for direct callers. Production generators recover
    // before reading the predecessor so changed inputs can render from the
    // manifest identity committed by a pending READY transaction.
    recover_interrupted_publication(
        request.output_root,
        request.artifact_manifest,
        request.stamp,
        desired_manifest.policy);

    const detail::previous_publication_snapshot previous_snapshot =
        detail::read_previous_publication_snapshot(
        request.artifact_manifest,
        request.output_root,
        desired_manifest.policy,
        std::string_view{});
    const previous_publication& previous = previous_snapshot.publication;
    const bool idempotent_republication = previous.present
        && detail::publication_noop_projection(
               previous_snapshot.content,
               "previous artifacts manifest")
            == detail::publication_noop_projection(
                request.artifact_manifest_json,
                "desired artifacts manifest");
    detail::require_pending_predecessor(desired_manifest, previous);

    std::vector<owned_publication_artifact> requested;
    requested.reserve(request.artifacts.size());
    std::set<std::filesystem::path> requested_paths;
    for (const publication_artifact& artifact : request.artifacts) {
        const std::filesystem::path relative = detail::checked_relative_path(
            artifact.relative_path, "desired publication artifact");
        if (relative == protocol.manifest_relative) {
            throw std::runtime_error(
                "artifacts.json must be published through its dedicated field");
        }
        const bool production = artifact.artifact_class
            == publication_artifact_class::production_cppm;
        if ((production && relative.extension() != ".cppm")
            || (!production && relative.extension() != ".json")) {
            throw std::runtime_error(
                "publication artifact extension disagrees with its class: "
                + relative.string());
        }
        if (!requested_paths.insert(relative).second) {
            throw std::runtime_error(
                "duplicate desired publication path: " + relative.string());
        }
        const std::string digest = bioseq::common::sha256_hex(artifact.content);
        if (artifact.content_sha256 != digest) {
            throw std::runtime_error(
                "desired publication content digest mismatch: "
                + relative.string());
        }
        requested.push_back(owned_publication_artifact{
            .relative_path = relative,
            .content_sha256 = digest,
            .artifact_class = artifact.artifact_class,
        });
    }
    std::ranges::sort(
        requested,
        {},
        [](const owned_publication_artifact& artifact) {
            return artifact.relative_path.generic_string();
        });
    if (requested.size() != desired_manifest.artifacts.size()) {
        throw std::runtime_error(
            "desired artifacts manifest does not own the complete publication set");
    }
    for (std::size_t index = 0; index < requested.size(); ++index) {
        const owned_publication_artifact& actual = requested[index];
        const owned_publication_artifact& recorded =
            desired_manifest.artifacts[index];
        if (actual.relative_path != recorded.relative_path
            || actual.content_sha256 != recorded.content_sha256
            || actual.artifact_class != recorded.artifact_class) {
            throw std::runtime_error(
                "desired artifacts manifest disagrees with publication artifact '"
                + actual.relative_path.string() + "'");
        }
    }
    detail::validate_protocol_ownership_paths(
        protocol, desired_manifest, previous);

    detail::require_write_destination(
        request.artifact_manifest, "artifact manifest destination");
    detail::require_regular_or_missing(
        request.stamp, "publication stamp destination");
    for (const publication_artifact& artifact : request.artifacts) {
        const std::filesystem::path destination =
            request.output_root / artifact.relative_path;
        detail::require_write_destination(
            destination, "desired publication artifact");
        const std::filesystem::file_status status =
            detail::symlink_status_or_missing(
                destination, "desired publication artifact");
        if (status.type() == std::filesystem::file_type::not_found) continue;
        const std::string existing_digest = bioseq::common::sha256_hex(read_artifact_bytes(destination));
        if (existing_digest == artifact.content_sha256) continue;
        const std::vector<owned_publication_artifact>::const_iterator
            prior_owner = std::ranges::find_if(
            previous.artifacts,
            [&artifact](const owned_publication_artifact& candidate) {
                return candidate.relative_path == artifact.relative_path;
            });
        if (prior_owner == previous.artifacts.end()
            || prior_owner->content_sha256 != existing_digest) {
            throw std::runtime_error(
                "refusing to overwrite an unowned or modified publication artifact: "
                + destination.string());
        }
    }

    for (const owned_publication_artifact& artifact : previous.artifacts) {
        if (requested_paths.contains(artifact.relative_path)) continue;
        const std::filesystem::path stale =
            request.output_root / artifact.relative_path;
        detail::require_regular_or_missing(
            stale, "authenticated stale publication artifact");
        const std::filesystem::file_status status =
            detail::symlink_status_or_missing(
                stale, "authenticated stale publication artifact");
        if (status.type() != std::filesystem::file_type::not_found
            && bioseq::common::sha256_hex(read_artifact_bytes(stale)) != artifact.content_sha256) {
            throw std::runtime_error(
                "refusing to delete a modified stale publication artifact: "
                + stale.string());
        }
    }

    detail::require_unchanged_previous_publication_snapshot(
        request.artifact_manifest, previous_snapshot);
    if (idempotent_republication) {
        mark_artifact_complete(request.stamp);
        return;
    }

    detail::write_new_staging_file(
        protocol.transaction_scratch,
        request.artifact_manifest_json,
        "publication PREPARING intent scratch");
    detail::atomic_replace_regular_file(
        protocol.transaction_scratch,
        protocol.transaction,
        "publication PREPARING intent installation");
    detail::maybe_inject_publication_fault(
        detail::publication_fault_point::preparing_intent_durable);

    std::size_t staged_ordinal = 0U;
    for (const publication_artifact& artifact : request.artifacts) {
        const std::filesystem::path destination =
            request.output_root / artifact.relative_path;
        if (detail::regular_file_digest(
                destination, "publication artifact staging preflight")
            == artifact.content_sha256) {
            continue;
        }
        const std::filesystem::path staging{destination.string() + ".tmp"};
        detail::write_new_staging_file(
            staging, artifact.content, "publication artifact staging");
        detail::maybe_inject_publication_fault(
            detail::publication_fault_point::desired_artifact_staged,
            staged_ordinal++);
    }

    detail::require_unchanged_previous_publication_snapshot(
        request.artifact_manifest, previous_snapshot);
    if (read_artifact_bytes(protocol.transaction) != request.artifact_manifest_json) {
        throw std::runtime_error(
            "publication PREPARING intent changed during staging");
    }
    detail::atomic_replace_regular_file(
        protocol.transaction,
        protocol.ready,
        "publication READY state installation");
    detail::maybe_inject_publication_fault(
        detail::publication_fault_point::ready_intent_durable);
    const detail::pending_publication ready{
        .state = detail::pending_publication_state::ready,
        .path = protocol.ready,
        .content = request.artifact_manifest_json,
        .desired = desired_manifest,
    };
    detail::roll_forward_ready_publication(
        protocol, ready, previous_snapshot);
    mark_artifact_complete(request.stamp);
}

}  // namespace bioseq::codegen::namespace_materialization
