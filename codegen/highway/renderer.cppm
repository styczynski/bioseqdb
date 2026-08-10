module;

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.highway:renderer;

import :manifest;
import :model;
import bioseq.codegen.structural_analysis;
import bioseq.codegen.namespace_materialization;
import bioseq.codegen.support;

export namespace bioseq::codegen::highway {

namespace detail {

[[nodiscard]] inline std::string generated_module_name(
    const target_spec& target) {
    return "bioseq.highway.generated." + target.platform + "."
        + target.slug;
}

[[nodiscard]] inline std::vector<
    namespace_materialization::materialization_coordinate>
build_target_coordinates(const target_manifest& inventory) {
    std::vector<namespace_materialization::materialization_coordinate>
        coordinates;
    coordinates.reserve(inventory.targets.size());
    for (const target_spec& target : inventory.targets) {
        const auto ordinal = static_cast<std::uint32_t>(coordinates.size());
        namespace_materialization::materialization_coordinate coordinate{
            .ordinal = ordinal,
            .assignments = {
                namespace_materialization::axis_case_assignment{
                    .group = "highway_target",
                    .role = namespace_materialization::axis_role::external,
                    .case_label = target.token,
                    .case_ordinal = ordinal,
                },
            },
            .identity_namespace = target.target_namespace,
            .module_name = generated_module_name(target),
            .relative_path = std::filesystem::path{target.platform}
                / target.slug / "index.cppm",
            .topology_digest = inventory.policy_digest,
        };
        namespace_materialization::validate_coordinate_axis_domain(
            coordinate);
        coordinates.push_back(std::move(coordinate));
    }
    return coordinates;
}

[[nodiscard]] inline const target_spec& static_target(
    const target_manifest& inventory) {
    const auto found = std::ranges::find(
        inventory.targets,
        inventory.static_target_token,
        &target_spec::token);
    if (found == inventory.targets.end()) {
        throw std::runtime_error(
            "renderer cannot resolve the configured Highway static target '"
            + inventory.static_target_token + "'");
    }
    return *found;
}

[[nodiscard]] inline std::string policy_block(
    const target_manifest& inventory) {
    std::string result;
    for (const auto& [name, value] : inventory.policy_definitions) {
        result += "#define " + name + " " + value + "\n";
    }
    return result;
}

[[nodiscard]] inline std::string fixed_target_fragment(
    const target_manifest& inventory,
    const target_spec& target) {
    std::ostringstream output;
    output << "module;\n\n"
           << policy_block(inventory)
           << "#include <hwy/detect_targets.h>\n"
           << "#undef HWY_TARGET\n"
           << "#define HWY_TARGET " << target.token << "\n"
           << "#include <cstdint>\n"
           << "#include <hwy/aligned_allocator.h>\n"
           << "#include <hwy/highway.h>\n\n";
    return output.str();
}

[[nodiscard]] inline std::string debug_assert_fragment(
    const extracted_source& source) {
    if (!source.body.contains("DEBUG_ASSERT")) {
        return {};
    }
    return
        "#ifdef NDEBUG\n"
        "#ifndef DEBUG_ASSERT\n"
        "#define DEBUG_ASSERT(...) (void)0\n"
        "#endif\n"
        "#else\n"
        "#include <libassert/assert.hpp>\n"
        "#endif\n\n";
}

[[nodiscard]] inline std::string dispatch_fragment(
    const target_manifest& inventory) {
    std::ostringstream output;
    output << "module;\n\n"
           << policy_block(inventory)
           << "#include <hwy/highway.h>\n"
           << "#include <cstdint>\n"
           << "#include <stdexcept>\n\n";
    return output.str();
}

[[nodiscard]] inline std::uint64_t compiled_target_mask(
    const target_manifest& inventory) noexcept {
    std::uint64_t result = 0;
    for (const target_spec& target : inventory.targets) {
        result |= target.bit;
    }
    return result;
}

[[nodiscard]] inline std::string diagnostic_source_path(
    const std::filesystem::path& source) {
    return structural_analysis::repository_source_identity(source);
}

[[nodiscard]] inline std::string body_with_highway_attributes(
    const extracted_source& source) {
    std::vector<namespace_materialization::source_edit> edits;
    edits.reserve(source.callables.size());
    for (const callable_insertion& callable : source.callables) {
        const bool lambda = callable.kind == "lambda";
        if (callable.body_relative_offset >= source.body.size()) {
            throw std::runtime_error(
                source.source.path.string()
                + ": stale HWY_ATTR insertion geometry while rendering");
        }
        std::string attribute{"HWY_ATTR "};
        if (lambda) {
            const bool valid_preceding =
                !callable.lambda_preceding_token.empty()
                && callable.body_relative_offset
                    >= callable.lambda_preceding_token.size()
                && source.body.compare(
                    callable.body_relative_offset
                        - callable.lambda_preceding_token.size(),
                    callable.lambda_preceding_token.size(),
                    callable.lambda_preceding_token) == 0;
            const bool valid_following =
                !callable.lambda_following_token.empty()
                && callable.lambda_following_relative_offset
                    < source.body.size()
                && source.body.compare(
                    callable.lambda_following_relative_offset,
                    callable.lambda_following_token.size(),
                    callable.lambda_following_token) == 0;
            if (!valid_preceding || !valid_following) {
                throw std::runtime_error(
                    source.source.path.string()
                    + ": stale lambda declarator tokens while rendering");
            }
            attribute = " HWY_ATTR";
            const unsigned char next = static_cast<unsigned char>(
                source.body[callable.body_relative_offset]);
            if (!std::isspace(next)) attribute += ' ';
        }
        edits.push_back(namespace_materialization::source_edit{
            .begin_byte = callable.body_relative_offset,
            .end_byte = callable.body_relative_offset,
            .replacement = std::move(attribute),
            .source_identity = callable.qualified_name,
            .purpose = "insert HWY_ATTR on " + callable.qualified_name,
        });
    }
    return namespace_materialization::apply_exact_edits(
        source.body, std::move(edits));
}

[[nodiscard]] inline std::string replace_target_namespace(
    std::string expression,
    std::string_view target_namespace) {
    constexpr std::string_view marker = "{target_namespace}";
    const std::size_t position = expression.find(marker);
    if (position == std::string::npos
        || expression.find(marker, position + marker.size())
            != std::string::npos) {
        throw std::runtime_error(
            "slot expression lost its unique target namespace placeholder");
    }
    expression.replace(position, marker.size(), target_namespace);
    return expression;
}

inline void replace_all(
    std::string& text,
    const std::string_view needle,
    const std::string_view replacement) {
    if (needle.empty()) return;
    std::size_t position = 0u;
    while ((position = text.find(needle, position)) != std::string::npos) {
        text.replace(position, needle.size(), replacement);
        position += replacement.size();
    }
}

[[nodiscard]] inline std::string render_probe_implementation(
    const generation_plan& plan,
    const target_spec& target,
    const namespace_materialization::materialization_coordinate& coordinate,
    const probe_spec& probe) {
    std::string body = read_artifact_bytes(probe.path);
    replace_all(
        body, "@BIOSEQ_HIGHWAY_PROBE_PRIMARY_MODULE@", coordinate.module_name);
    replace_all(
        body, "@BIOSEQ_HIGHWAY_PROBE_TARGET_TOKEN@", target.token);
    replace_all(
        body,
        "@BIOSEQ_HIGHWAY_PROBE_TARGET_NAMESPACE@",
        coordinate.identity_namespace);
    replace_all(body, "@BIOSEQ_HIGHWAY_PROBE_KEY@", probe.key);
    if (body.contains("@BIOSEQ_HIGHWAY_PROBE_")
        || body.contains("export module")
        || !body.contains("module " + coordinate.module_name + ";")
        || body.contains("HWY_DYNAMIC_DISPATCH")
        || body.contains("HWY_STATIC_DISPATCH")
        || body.contains("HWY_EXPORT(")) {
        throw std::runtime_error(
            "unit probe template '" + probe.key
            + "' did not materialize as a private ordinary implementation "
              "unit of the fixed-target primary");
    }
    std::ostringstream output;
    output << fixed_target_fragment(plan.target_inventory, target)
           << "// Generated unit probe from "
           << diagnostic_source_path(probe.path) << "; do not edit.\n"
           << body;
    if (body.empty() || body.back() != '\n') output << '\n';
    return output.str();
}

[[nodiscard]] inline std::string render_minimizer_representation_instantiation(
    const generation_plan& plan,
    const target_spec& target,
    const namespace_materialization::materialization_coordinate& coordinate,
    const minimizer_representation_instance_spec& instance,
    const source_spec& source) {
    const std::string prefix = instance.algorithm == "minimap2_reference"
        ? "minimap2_reference_repr_"
        : "bounded_nthash_repr_";
    const std::string algorithm_prefix = instance.algorithm + "_";
    if (!instance.owner_key.starts_with(algorithm_prefix)) {
        throw std::runtime_error(
            "minimizer representation owner lost its algorithm prefix: "
            + instance.owner_key);
    }
    const std::string identity =
        instance.owner_key.substr(algorithm_prefix.size());
    std::ostringstream output;
    output << fixed_target_fragment(plan.target_inventory, target)
           << "// Generated bounded minimizer representation owner; do not edit.\n"
           << "module " << coordinate.module_name << ";\n\n"
           << "import std;\n"
           << "import bioseq.common.seqan_adapter;\n"
           << "import bioseq.sequences.kmers.highway_minimizer_contract;\n"
           << "import :" << source.partition << ";\n\n"
           << "namespace bioseq::simd::" << coordinate.identity_namespace
           << " {\n\n";
    for (const std::string& storage : instance.storage_types) {
        const std::string cpp_storage = storage == "u32"
            ? "::std::uint32_t"
            : "::std::uint64_t";
        output
            << "[[nodiscard]] HWY_ATTR bool " << prefix << identity << "_"
            << storage << "(\n"
            << "    ::bioseq::minimizers::minimizer_kernel_invocation& invocation,\n"
            << "    ::bioseq::minimizers::minimizer_kernel_result& result)\n"
            << "{\n    return ";
        if (instance.algorithm == "minimap2_reference") {
            output << "minimizer_kernel_detail::run_reference<\n"
                   << "        " << instance.representation_type
                   << ", ::seq::dna4, " << cpp_storage
                   << ">(invocation, result);\n";
        } else {
            output << "bounded_nthash_kernel_detail::run_bounded_nthash<\n"
                   << "        " << instance.representation_type << ", "
                   << cpp_storage << ">(invocation, result);\n";
        }
        output << "}\n\n";
    }
    output << "}  // namespace bioseq::simd::"
           << coordinate.identity_namespace << "\n";
    return output.str();
}

inline constexpr std::string_view alignment_family_prefix = "alignment_";

[[nodiscard]] inline bool is_alignment_family(
    const std::string_view family) noexcept {
    return family.starts_with(alignment_family_prefix);
}

[[nodiscard]] inline std::uint8_t alignment_alphabet_code(
    const std::string_view family) {
    if (!is_alignment_family(family)
        || family.size() == alignment_family_prefix.size()) {
        throw std::runtime_error(
            "typed alignment slot family must be named alignment_<code>: "
            + std::string{family});
    }

    std::uint32_t code = 0u;
    for (const char character : family.substr(
             alignment_family_prefix.size())) {
        if (character < '0' || character > '9') {
            throw std::runtime_error(
                "typed alignment slot family has a non-numeric alphabet code: "
                + std::string{family});
        }
        code = code * 10u
             + static_cast<std::uint32_t>(character - '0');
        if (code > 255u) {
            throw std::runtime_error(
                "typed alignment slot family alphabet code exceeds uint8_t: "
                + std::string{family});
        }
    }
    return static_cast<std::uint8_t>(code);
}

/**
 * Extract the sole simple type argument from a target callable template-id.
 *
 * Alignment function pointers are alphabet-specific, so the generated binder
 * materializes one strongly typed alignment_kernels<Alphabet> object per
 * scheduler alphabet. Restricting the registry spelling to a qualified type
 * name keeps manifest data from becoming arbitrary generated C++.
 */
[[nodiscard]] inline std::string alignment_alphabet_type(
    const slot_spec& slot) {
    const std::size_t marker =
        slot.expression.find("{target_namespace}");
    const std::size_t open = slot.expression.find('<', marker);
    const std::size_t close = slot.expression.rfind('>');
    if (marker == std::string::npos
        || open == std::string::npos
        || close != slot.expression.size() - 1u
        || open + 1u == close
        || slot.expression.find('<', open + 1u) != std::string::npos
        || slot.expression.find('>', open + 1u) != close) {
        throw std::runtime_error(
            "typed alignment slot '" + slot.family_field + "."
            + slot.slot_field
            + "' must end in one simple alphabet template argument");
    }

    std::string alphabet =
        slot.expression.substr(open + 1u, close - open - 1u);
    for (const char character : alphabet) {
        const bool identifier_character =
            (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '_' || character == ':';
        if (!identifier_character) {
            throw std::runtime_error(
                "typed alignment slot '" + slot.family_field + "."
                + slot.slot_field
                + "' alphabet must be a qualified type name");
        }
    }
    return alphabet;
}

struct alignment_family_binding final {
    std::uint8_t alphabet_code{};
    std::string alphabet_type;
    std::map<std::string, const slot_spec*, std::less<>> members;
};

inline constexpr std::array<std::string_view, 6>
    alignment_required_members{
        "align_ksw2_single_affine",
        "align_ksw2_dual_affine",
        "align_ksw2_batched",
        "prepare_block_global_gap_certificate",
        "try_block_certified_gap",
        "align_block",
    };

[[nodiscard]] inline std::vector<alignment_family_binding>
collect_alignment_families(
    const std::map<
        std::string,
        std::vector<const slot_spec*>,
        std::less<>>& families) {
    std::vector<alignment_family_binding> result;
    std::set<std::uint8_t> alphabet_codes;
    for (const auto& [family, slots] : families) {
        if (!is_alignment_family(family)) continue;

        alignment_family_binding binding{
            .alphabet_code = alignment_alphabet_code(family),
        };
        if (!alphabet_codes.insert(binding.alphabet_code).second) {
            throw std::runtime_error(
                "duplicate typed alignment alphabet code "
                + std::to_string(binding.alphabet_code));
        }

        for (const slot_spec* slot : slots) {
            if (slot->slot_field == "@"
                || !binding.members.emplace(slot->slot_field, slot).second) {
                throw std::runtime_error(
                    "typed alignment family '" + family
                    + "' must use unique member slots");
            }
            const std::string alphabet = alignment_alphabet_type(*slot);
            if (binding.alphabet_type.empty()) {
                binding.alphabet_type = alphabet;
            } else if (binding.alphabet_type != alphabet) {
                throw std::runtime_error(
                    "typed alignment family '" + family
                    + "' mixes alphabet template arguments");
            }
        }

        if (binding.members.size() != alignment_required_members.size()) {
            throw std::runtime_error(
                "typed alignment family '" + family
                + "' must bind exactly six function slots");
        }
        for (const std::string_view member : alignment_required_members) {
            if (!binding.members.contains(member)) {
                throw std::runtime_error(
                    "typed alignment family '" + family
                    + "' is missing required slot '" + std::string{member}
                    + "'");
            }
        }
        result.push_back(std::move(binding));
    }

    std::ranges::sort(
        result,
        {},
        [](const alignment_family_binding& binding) {
            return binding.alphabet_code;
        });
    return result;
}

inline constexpr std::string_view minimizer_case_family_prefix =
    "minimizer_case_";

[[nodiscard]] inline bool is_minimizer_case_family(
    const std::string_view family) noexcept {
    return family.starts_with(minimizer_case_family_prefix);
}

struct minimizer_required_member final {
    std::string_view slot;
    std::string_view owner;
};

inline constexpr std::array<minimizer_required_member, 3>
    minimizer_required_members{{
        {"minimap2_reference", "minimizer_minimap2_reference"},
        {"two_stack_nthash", "minimizer_two_stack_nthash"},
        {"bounded_nthash", "minimizer_bounded_nthash"},
    }};

struct minimizer_case_binding final {
    std::uint8_t alphabet_code{};
    std::uint8_t storage_unit_bytes{};
    std::string alphabet_type;
    std::string storage_unit_type;
    std::map<std::string, const slot_spec*, std::less<>> members;
};

[[nodiscard]] inline std::uint8_t parse_minimizer_case_number(
    const std::string_view spelling,
    const std::string_view field,
    const std::string_view family) {
    if (spelling.empty()
        || (spelling.size() > 1u && spelling.front() == '0')) {
        throw std::runtime_error(
            "typed minimizer family '" + std::string{family}
            + "' has a non-canonical " + std::string{field});
    }

    std::uint32_t result = 0u;
    for (const char character : spelling) {
        if (character < '0' || character > '9') {
            throw std::runtime_error(
                "typed minimizer family '" + std::string{family}
                + "' has a non-numeric " + std::string{field});
        }
        result = result * 10u
               + static_cast<std::uint32_t>(character - '0');
        if (result > 255u) {
            throw std::runtime_error(
                "typed minimizer family '" + std::string{family} + "' "
                + std::string{field} + " exceeds uint8_t");
        }
    }
    return static_cast<std::uint8_t>(result);
}

[[nodiscard]] inline std::pair<std::uint8_t, std::uint8_t>
minimizer_case_coordinates(const std::string_view family) {
    if (!is_minimizer_case_family(family)) {
        throw std::runtime_error(
            "typed minimizer slot family must be named "
            "minimizer_case_<alphabet-code>_<storage-bytes>: "
            + std::string{family});
    }
    const std::string_view suffix =
        family.substr(minimizer_case_family_prefix.size());
    const std::size_t separator = suffix.find('_');
    if (separator == std::string_view::npos
        || separator == 0u
        || separator + 1u == suffix.size()
        || suffix.find('_', separator + 1u) != std::string_view::npos) {
        throw std::runtime_error(
            "typed minimizer slot family must be named "
            "minimizer_case_<alphabet-code>_<storage-bytes>: "
            + std::string{family});
    }

    const std::uint8_t alphabet_code = parse_minimizer_case_number(
        suffix.substr(0u, separator), "alphabet code", family);
    const std::uint8_t storage_unit_bytes = parse_minimizer_case_number(
        suffix.substr(separator + 1u), "storage byte width", family);
    if (storage_unit_bytes == 0u || storage_unit_bytes > 8u
        || (storage_unit_bytes & (storage_unit_bytes - 1u)) != 0u) {
        throw std::runtime_error(
            "typed minimizer family '" + std::string{family}
            + "' storage byte width must be a power of two no greater "
              "than eight");
    }
    return {alphabet_code, storage_unit_bytes};
}

[[nodiscard]] inline bool qualified_type_name(
    const std::string_view spelling) noexcept {
    std::string_view remaining = spelling;
    if (remaining.starts_with("::")) remaining.remove_prefix(2u);
    if (remaining.empty()) return false;

    while (!remaining.empty()) {
        const std::size_t separator = remaining.find("::");
        const std::string_view component = remaining.substr(0u, separator);
        if (component.empty()
            || !std::ranges::all_of(component, [](const char character) {
                   return (character >= 'a' && character <= 'z')
                       || (character >= 'A' && character <= 'Z')
                       || (character >= '0' && character <= '9')
                       || character == '_';
               })
            || !((component.front() >= 'a' && component.front() <= 'z')
                 || (component.front() >= 'A' && component.front() <= 'Z')
                 || component.front() == '_')) {
            return false;
        }
        if (separator == std::string_view::npos) return true;
        remaining.remove_prefix(separator + 2u);
    }
    return false;
}

[[nodiscard]] inline std::pair<std::string, std::string>
minimizer_case_types(const slot_spec& slot) {
    const std::string expected_suffix =
        "{target_namespace}::" + slot.slot_field + "_case<";
    const std::size_t marker = slot.expression.find(expected_suffix);
    if (!slot.expression.starts_with('&')
        || marker == std::string::npos
        || slot.expression.find(
               expected_suffix, marker + expected_suffix.size())
            != std::string::npos
        || !slot.expression.ends_with('>')) {
        throw std::runtime_error(
            "typed minimizer slot '" + slot.family_field + "."
            + slot.slot_field + "' must name its matching *_case<Alphabet, "
              "StorageUnit> target callable");
    }

    const std::size_t arguments_begin = marker + expected_suffix.size();
    const std::string_view arguments{
        slot.expression.data() + arguments_begin,
        slot.expression.size() - arguments_begin - 1u};
    const std::size_t comma = arguments.find(',');
    if (comma == std::string_view::npos
        || comma == 0u
        || comma + 1u == arguments.size()
        || arguments.find(',', comma + 1u) != std::string_view::npos
        || arguments.find_first_of("<>") != std::string_view::npos) {
        throw std::runtime_error(
            "typed minimizer slot '" + slot.family_field + "."
            + slot.slot_field
            + "' must have exactly two simple type arguments");
    }

    const std::string alphabet{arguments.substr(0u, comma)};
    const std::string storage{arguments.substr(comma + 1u)};
    if (!qualified_type_name(alphabet) || !qualified_type_name(storage)) {
        throw std::runtime_error(
            "typed minimizer slot '" + slot.family_field + "."
            + slot.slot_field
            + "' template arguments must be qualified type names");
    }
    return {alphabet, storage};
}

[[nodiscard]] inline std::vector<minimizer_case_binding>
collect_minimizer_cases(
    const std::map<
        std::string,
        std::vector<const slot_spec*>,
        std::less<>>& families) {
    std::vector<minimizer_case_binding> result;
    std::set<std::pair<std::uint8_t, std::uint8_t>> coordinates;
    std::map<std::uint8_t, std::string> alphabet_types;
    std::map<std::string, std::uint8_t, std::less<>> alphabet_codes;
    std::map<std::uint8_t, std::string> storage_types;

    for (const auto& [family, slots] : families) {
        if (!is_minimizer_case_family(family)) continue;

        const auto [alphabet_code, storage_unit_bytes] =
            minimizer_case_coordinates(family);
        if (!coordinates.emplace(
                alphabet_code, storage_unit_bytes).second) {
            throw std::runtime_error(
                "duplicate typed minimizer case for alphabet code "
                + std::to_string(alphabet_code) + " and storage byte width "
                + std::to_string(storage_unit_bytes));
        }

        minimizer_case_binding binding{
            .alphabet_code = alphabet_code,
            .storage_unit_bytes = storage_unit_bytes,
        };
        for (const slot_spec* slot : slots) {
            const auto required = std::ranges::find(
                minimizer_required_members,
                slot->slot_field,
                &minimizer_required_member::slot);
            if (required == minimizer_required_members.end()
                || slot->owner_key != required->owner
                || !binding.members.emplace(
                        slot->slot_field, slot).second) {
                throw std::runtime_error(
                    "typed minimizer family '" + family
                    + "' must use each required algorithm member exactly once "
                      "and from its registered owner");
            }

            const auto [alphabet, storage] = minimizer_case_types(*slot);
            if (binding.alphabet_type.empty()) {
                binding.alphabet_type = alphabet;
                binding.storage_unit_type = storage;
            } else if (binding.alphabet_type != alphabet
                       || binding.storage_unit_type != storage) {
                throw std::runtime_error(
                    "typed minimizer family '" + family
                    + "' mixes template arguments");
            }
        }

        if (binding.members.size() != minimizer_required_members.size()) {
            throw std::runtime_error(
                "typed minimizer family '" + family
                + "' must bind exactly three algorithm members");
        }
        for (const minimizer_required_member& member
             : minimizer_required_members) {
            if (!binding.members.contains(member.slot)) {
                throw std::runtime_error(
                    "typed minimizer family '" + family
                    + "' is missing required member '"
                    + std::string{member.slot} + "'");
            }
        }

        const std::string expected_storage_type =
            "::std::uint"
            + std::to_string(
                static_cast<unsigned int>(storage_unit_bytes) * 8u)
            + "_t";
        if (binding.storage_unit_type != expected_storage_type) {
            throw std::runtime_error(
                "typed minimizer family '" + family
                + "' storage type does not match its topology byte width");
        }

        const auto [alphabet_found, inserted_alphabet] =
            alphabet_types.emplace(alphabet_code, binding.alphabet_type);
        if (!inserted_alphabet
            && alphabet_found->second != binding.alphabet_type) {
            throw std::runtime_error(
                "typed minimizer alphabet code "
                + std::to_string(alphabet_code)
                + " maps to more than one alphabet type");
        }
        const auto [code_found, inserted_code] =
            alphabet_codes.emplace(binding.alphabet_type, alphabet_code);
        if (!inserted_code && code_found->second != alphabet_code) {
            throw std::runtime_error(
                "typed minimizer alphabet type '" + binding.alphabet_type
                + "' maps to more than one alphabet code");
        }
        const auto [storage_found, inserted_storage] =
            storage_types.emplace(
                storage_unit_bytes, binding.storage_unit_type);
        if (!inserted_storage
            && storage_found->second != binding.storage_unit_type) {
            throw std::runtime_error(
                "typed minimizer storage byte width "
                + std::to_string(storage_unit_bytes)
                + " maps to more than one storage type");
        }
        result.push_back(std::move(binding));
    }

    if (result.empty()) return result;
    if (families.contains("minimizers")) {
        throw std::runtime_error(
            "typed minimizer cases may not coexist with a second root-table "
            "minimizer slot registry");
    }
    if (result.size() != 8u || alphabet_types.size() != 2u
        || storage_types.size() != 4u) {
        throw std::runtime_error(
            "typed minimizer registry must be the exact source-owned "
            "two-alphabet by four-storage-width matrix");
    }
    for (const auto& [alphabet_code, alphabet] : alphabet_types) {
        static_cast<void>(alphabet);
        for (const auto& [storage_unit_bytes, storage] : storage_types) {
            static_cast<void>(storage);
            if (!coordinates.contains(
                    {alphabet_code, storage_unit_bytes})) {
                throw std::runtime_error(
                    "typed minimizer registry is not a closed Cartesian matrix");
            }
        }
    }

    std::ranges::sort(
        result,
        {},
        [](const minimizer_case_binding& binding) {
            return std::pair{
                binding.alphabet_code, binding.storage_unit_bytes};
        });
    return result;
}

struct alphabet_identity_binding final {
    std::uint8_t alphabet_code{};
    std::string alphabet_type;
};

/**
 * Reconcile every typed consumer against one generated alphabet identity axis.
 *
 * Alignment and minimizer records are separately useful renderer fixtures, so
 * either family may stand alone. When both are present (the production
 * registry), their code/type maps must be identical before any erased
 * alignment pointer or minimizer wrapper is emitted.
 */
[[nodiscard]] inline std::vector<alphabet_identity_binding>
collect_alphabet_identities(
    const std::vector<alignment_family_binding>& alignment_families,
    const std::vector<minimizer_case_binding>& minimizer_cases) {
    using code_map = std::map<std::uint8_t, std::string>;
    using type_map = std::map<std::string, std::uint8_t, std::less<>>;
    code_map alignment_by_code;
    type_map alignment_by_type;
    code_map minimizer_by_code;
    type_map minimizer_by_type;

    const auto insert = [](
        code_map& by_code,
        type_map& by_type,
        const std::uint8_t code,
        const std::string& type,
        const std::string_view owner) {
        const auto [code_entry, inserted_code] = by_code.emplace(code, type);
        if (!inserted_code && code_entry->second != type) {
            throw std::runtime_error(
                std::string{owner} + " alphabet code "
                + std::to_string(code) + " maps to multiple C++ types");
        }
        const auto [type_entry, inserted_type] = by_type.emplace(type, code);
        if (!inserted_type && type_entry->second != code) {
            throw std::runtime_error(
                std::string{owner} + " alphabet type '" + type
                + "' maps to multiple scheduler codes");
        }
    };

    for (const alignment_family_binding& family : alignment_families) {
        insert(
            alignment_by_code,
            alignment_by_type,
            family.alphabet_code,
            family.alphabet_type,
            "typed alignment");
    }
    for (const minimizer_case_binding& binding : minimizer_cases) {
        insert(
            minimizer_by_code,
            minimizer_by_type,
            binding.alphabet_code,
            binding.alphabet_type,
            "typed minimizer");
    }

    if (!alignment_by_code.empty() && !minimizer_by_code.empty()
        && alignment_by_code != minimizer_by_code) {
        throw std::runtime_error(
            "typed alignment and minimizer registries disagree on the "
            "source-owned alphabet identity axis");
    }

    const code_map& selected = !minimizer_by_code.empty()
        ? minimizer_by_code
        : alignment_by_code;
    std::vector<alphabet_identity_binding> result;
    result.reserve(selected.size());
    for (const auto& [code, type] : selected) {
        result.push_back(alphabet_identity_binding{
            .alphabet_code = code,
            .alphabet_type = type,
        });
    }
    return result;
}

[[nodiscard]] inline std::string minimizer_wrapper_name(
    const std::string_view member) {
    return "minimizer_dispatch_" + std::string{member};
}

[[nodiscard]] inline std::string minimizer_callable_expression(
    const slot_spec& slot,
    const std::string_view target_namespace) {
    std::string expression = replace_target_namespace(
        slot.expression,
        target_namespace);
    if (!expression.starts_with('&')) {
        throw std::runtime_error(
            "typed minimizer callable lost its address-of marker");
    }
    expression.erase(0u, 1u);
    return expression;
}

inline void render_minimizer_wrapper(
    std::ostringstream& output,
    const std::vector<minimizer_case_binding>& cases,
    const minimizer_required_member& member,
    const namespace_materialization::materialization_coordinate& coordinate) {
    if (cases.empty()) return;

    output
        << "[[nodiscard]] HWY_ATTR "
           "::bioseq::minimizers::minimizer_kernel_result "
        << minimizer_wrapper_name(member.slot) << "(\n"
        << "    ::bioseq::minimizers::minimizer_kernel_invocation& "
           "invocation) {\n"
        << "    switch (invocation.sequence.alphabet_code) {\n";

    std::size_t begin = 0u;
    while (begin < cases.size()) {
        const std::uint8_t alphabet_code = cases[begin].alphabet_code;
        output << "        case "
               << static_cast<unsigned int>(alphabet_code) << "u:\n"
               << "            switch ("
                  "invocation.sequence.storage_unit_bytes) {\n";
        std::size_t end = begin;
        while (end < cases.size()
               && cases[end].alphabet_code == alphabet_code) {
            const minimizer_case_binding& binding = cases[end];
            const slot_spec* slot = binding.members.find(member.slot)->second;
            output << "                case "
                   << static_cast<unsigned int>(binding.storage_unit_bytes)
                   << "u:\n"
                   << "                    return "
                   << minimizer_callable_expression(
                          *slot, coordinate.identity_namespace)
                   << "(invocation);\n";
            ++end;
        }
        output << "                default:\n"
               << "                    break;\n"
               << "            }\n"
               << "            break;\n";
        begin = end;
    }
    output
        << "        default:\n"
        << "            break;\n"
        << "    }\n"
        << "    ::bioseq::minimizers::minimizer_kernel_result result{};\n"
        << "    result.stats.algorithm = "
           "::bioseq::minimizers::minimizer_kernel_algorithm::"
        << member.slot << ";\n"
        << "    result.stats.target = "
           "static_cast<::std::int64_t>(HWY_TARGET);\n"
        << "    result.status = "
           "::bioseq::minimizers::minimizer_kernel_status::"
           "invalid_configuration;\n"
        << "    return result;\n"
        << "}\n\n";
}

[[nodiscard]] inline std::string_view kernel_enumerator_for_owner(
    std::string_view owner) {
    if (owner == "minimizer_minimap2_reference") {
        return "minimizer_minimap2_reference";
    }
    if (owner == "minimizer_two_stack_nthash") {
        return "minimizer_two_stack_nthash";
    }
    if (owner == "minimizer_bounded_nthash") {
        return "minimizer_bounded_nthash";
    }
    if (owner == "chaining") return "chaining_chain_anchors_simd";
    if (owner == "prefilter") {
        return "prefilter_sneaky_snake_simd_batch";
    }
    if (owner == "ksw2") return "alignment_ksw2";
    if (owner == "ksw2_batched") return "alignment_ksw2_batched";
    if (owner == "block_aligner") return "alignment_block_aligner";
    throw std::runtime_error(
        "Highway table slot owner '" + std::string{owner}
        + "' has no highway_kernel mapping");
}

[[nodiscard]] inline std::string population_name(
    const std::string_view owner) {
    return "populate_" + std::string{owner};
}

[[nodiscard]] inline std::string alignment_population_name(
    const std::string_view owner,
    const std::uint8_t alphabet_code) {
    return population_name(owner) + "_alignment_"
        + std::to_string(static_cast<unsigned int>(alphabet_code));
}

[[nodiscard]] inline std::map<
    std::string,
    std::vector<const slot_spec*>,
    std::less<>>
collect_slot_families(const source_manifest& inventory) {
    std::map<std::string, std::vector<const slot_spec*>, std::less<>> families;
    for (const slot_spec& slot : inventory.slots) {
        families[slot.family_field].push_back(&slot);
    }
    return families;
}

[[nodiscard]] inline std::set<std::string, std::less<>>
collect_slot_owners(const source_manifest& inventory) {
    std::set<std::string, std::less<>> owners;
    for (const slot_spec& slot : inventory.slots) {
        owners.insert(slot.owner_key);
    }
    return owners;
}

[[nodiscard]] inline std::string render_primary(
    const source_manifest& inventory,
    const namespace_materialization::materialization_coordinate& coordinate) {
    const auto families = collect_slot_families(inventory);
    const std::vector<alignment_family_binding> alignment_families =
        collect_alignment_families(families);
    const std::set<std::string, std::less<>> owners =
        collect_slot_owners(inventory);

    std::ostringstream output;
    output << "// Generated by bioseq_codegen highway; do not edit.\n"
           << "export module " << coordinate.module_name << ";\n\n"
           << "import " << inventory.contract_module << ";\n";
    if (!alignment_families.empty()) {
        output << "import bioseq.common.seqan_adapter;\n"
               << "import bioseq.sequences;\n";
    }
    output << "\n"
           << "export namespace bioseq::simd_dispatch::"
           << coordinate.identity_namespace << " {\n"
           << "[[nodiscard]] const highway_target_binding* "
              "bind_highway_target();\n"
           << "}  // namespace bioseq::simd_dispatch::"
           << coordinate.identity_namespace << "\n\n"
           << "namespace bioseq::simd_dispatch::"
           << coordinate.identity_namespace << "::binding_detail {\n";

    for (const std::string& owner : owners) {
        const bool has_root_slots = std::ranges::any_of(
            inventory.slots,
            [&owner](const slot_spec& slot) {
                return slot.owner_key == owner
                    && !is_alignment_family(slot.family_field);
            });
        if (has_root_slots) {
            output << "void " << population_name(owner)
                   << "(::bioseq::simd_dispatch::highway_kernel_table& table) "
                      "noexcept;\n";
        }
        for (const alignment_family_binding& family : alignment_families) {
            const bool owns_family_member = std::ranges::any_of(
                family.members,
                [&owner](const auto& member) {
                    return member.second->owner_key == owner;
                });
            if (owns_family_member) {
                output << "void "
                       << alignment_population_name(
                              owner, family.alphabet_code)
                       << "(::bioseq::simd::alignment_kernels<"
                       << family.alphabet_type << ">& table) noexcept;\n";
            }
        }
    }
    output << "}  // namespace bioseq::simd_dispatch::"
           << coordinate.identity_namespace << "::binding_detail\n";
    return output.str();
}

[[nodiscard]] inline std::string render_partition(
    const generation_plan& plan,
    const target_spec& target,
    const namespace_materialization::materialization_coordinate& coordinate,
    const extracted_source& source,
    const std::map<std::string, const source_spec*, std::less<>>& by_key) {
    std::ostringstream output;
    output << fixed_target_fragment(plan.target_inventory, target)
           << debug_assert_fragment(source)
           << "// Generated from " << source.source.key << "; do not edit.\n"
           << "module " << coordinate.module_name << ":"
           << source.source.partition << ";\n\n";
    for (const std::string& neutral : source.neutral_imports) {
        output << "import " << neutral << ";\n";
    }
    for (const std::string& dependency : source.source.dependencies) {
        const auto found = by_key.find(dependency);
        if (found == by_key.end()) {
            throw std::runtime_error(
                "renderer cannot resolve Highway dependency '" + dependency + "'");
        }
        output << "import :" << found->second->partition << ";\n";
    }
    if (!source.neutral_imports.empty() || !source.source.dependencies.empty()) {
        output << '\n';
    }
    output << "namespace " << source.enclosing_namespace << "::"
           << coordinate.identity_namespace << " {\n"
           << "#line " << source.body_first_line << " \""
           << escape_cpp_string_literal(diagnostic_source_path(source.source.path)) << "\"\n"
           << body_with_highway_attributes(source);
    if (source.body.empty() || source.body.back() != '\n') output << '\n';
    output << "}  // namespace " << source.enclosing_namespace << "::"
           << coordinate.identity_namespace << "\n";
    return output.str();
}

[[nodiscard]] inline std::string render_kernel_instantiation(
    const generation_plan& plan,
    const target_spec& target,
    const namespace_materialization::materialization_coordinate& coordinate,
    const std::string_view owner,
    const source_spec& source) {
    const auto families = collect_slot_families(plan.source_inventory);
    const std::vector<alignment_family_binding> alignment_families =
        collect_alignment_families(families);
    const auto minimizer_member = std::ranges::find(
        minimizer_required_members,
        owner,
        &minimizer_required_member::owner);

    std::vector<const slot_spec*> owned_slots;
    for (const slot_spec& slot : plan.source_inventory.slots) {
        if (slot.owner_key == owner) owned_slots.push_back(&slot);
    }
    const bool owns_typed_minimizer =
        minimizer_member != minimizer_required_members.end()
        && std::ranges::any_of(
            owned_slots,
            [](const slot_spec* slot) {
                return is_minimizer_case_family(slot->family_field);
            });
    std::ranges::sort(
        owned_slots,
        {},
        [](const slot_spec* slot) {
            return std::pair{slot->family_field, slot->slot_field};
        });

    std::ostringstream output;
    output << fixed_target_fragment(plan.target_inventory, target)
           << "// Generated concrete bindings for " << owner
           << "; do not edit.\n"
           << "module " << coordinate.module_name << ";\n\n"
           << "import " << plan.source_inventory.contract_module << ";\n";
    if (owns_typed_minimizer
        || std::ranges::any_of(
            owned_slots,
            [](const slot_spec* slot) {
                return is_alignment_family(slot->family_field);
            })) {
        output << "import bioseq.common.seqan_adapter;\n"
               << "import bioseq.sequences;\n";
    }
    output << "import :" << source.partition << ";\n\n"
           << "namespace bioseq::simd_dispatch::"
           << coordinate.identity_namespace << "::binding_detail {\n\n";

    if (owns_typed_minimizer) {
        render_minimizer_wrapper(
            output,
            collect_minimizer_cases(families),
            *minimizer_member,
            coordinate);
    }

    const bool has_root_slots = std::ranges::any_of(
        owned_slots,
        [](const slot_spec* slot) {
            return !is_alignment_family(slot->family_field);
        });
    if (has_root_slots) {
        output << "HWY_ATTR void " << population_name(owner)
               << "(::bioseq::simd_dispatch::highway_kernel_table& table) "
                  "noexcept {\n";
        if (owns_typed_minimizer) {
            output << "    table.minimizers." << minimizer_member->slot
                   << " = &" << minimizer_wrapper_name(minimizer_member->slot)
                   << ";\n";
        }
        for (const slot_spec* slot : owned_slots) {
            if (is_alignment_family(slot->family_field)
                || is_minimizer_case_family(slot->family_field)) {
                continue;
            }
            if (slot->slot_field == "@") {
                output << "    table." << slot->family_field << " = ";
            } else {
                output << "    table." << slot->family_field << "."
                       << slot->slot_field << " = ";
            }
            output << replace_target_namespace(
                          slot->expression, coordinate.identity_namespace)
                   << ";\n";
        }
        output << "}\n\n";
    }

    for (const alignment_family_binding& family : alignment_families) {
        std::vector<const slot_spec*> owned_members;
        for (const auto& [member, slot] : family.members) {
            static_cast<void>(member);
            if (slot->owner_key == owner) owned_members.push_back(slot);
        }
        if (owned_members.empty()) continue;
        std::ranges::sort(
            owned_members,
            {},
            [](const slot_spec* slot) { return slot->slot_field; });
        output << "HWY_ATTR void "
               << alignment_population_name(owner, family.alphabet_code)
               << "(::bioseq::simd::alignment_kernels<"
               << family.alphabet_type << ">& table) noexcept {\n";
        for (const slot_spec* slot : owned_members) {
            output << "    table." << slot->slot_field << " = "
                   << replace_target_namespace(
                          slot->expression, coordinate.identity_namespace)
                   << ";\n";
        }
        output << "}\n\n";
    }

    output << "}  // namespace bioseq::simd_dispatch::"
           << coordinate.identity_namespace << "::binding_detail\n";
    return output.str();
}

[[nodiscard]] inline std::string render_binder(
    const generation_plan& plan,
    const target_spec& target,
    const namespace_materialization::materialization_coordinate& coordinate) {
    const auto families = collect_slot_families(plan.source_inventory);
    const std::vector<alignment_family_binding> alignment_families =
        collect_alignment_families(families);
    const std::vector<minimizer_case_binding> minimizer_cases =
        collect_minimizer_cases(families);
    const std::vector<alphabet_identity_binding> alphabet_identities =
        collect_alphabet_identities(alignment_families, minimizer_cases);

    std::ostringstream output;
    output << fixed_target_fragment(plan.target_inventory, target)
           << "// Generated target binding; do not edit.\n"
           << "module " << coordinate.module_name << ";\n\n"
           << "import " << plan.source_inventory.contract_module << ";\n";
    for (const slot_spec& slot : plan.source_inventory.slots) {
        if (is_alignment_family(slot.family_field)
            || is_minimizer_case_family(slot.family_field)) {
            output << "import bioseq.common.seqan_adapter;\n"
                   << "import bioseq.sequences;\n";
            break;
        }
    }
    output << "\nnamespace bioseq::simd_dispatch::"
           << coordinate.identity_namespace
           << " {\n\n";

    for (const alignment_family_binding& family : alignment_families) {
        const auto code = static_cast<unsigned int>(family.alphabet_code);
        std::set<std::string, std::less<>> family_owners;
        for (const auto& [member, slot] : family.members) {
            static_cast<void>(member);
            family_owners.insert(slot->owner_key);
        }
        output << "HWY_ATTR ::bioseq::simd::alignment_kernels<"
               << family.alphabet_type << "> make_alignment_" << code
               << "() noexcept {\n"
               << "    ::bioseq::simd::alignment_kernels<"
               << family.alphabet_type << "> table{};\n";
        for (const std::string& owner : family_owners) {
            output << "    binding_detail::"
                   << alignment_population_name(owner, family.alphabet_code)
                   << "(table);\n";
        }
        output << "    return table;\n"
               << "}\n\n";
    }

    output << "HWY_ATTR highway_kernel_table make_highway_kernel_table() noexcept {\n"
           << "    highway_kernel_table table{};\n";

    std::set<std::string, std::less<>> available_kernel_ids;
    for (const slot_spec& slot : plan.source_inventory.slots) {
        available_kernel_ids.emplace(kernel_enumerator_for_owner(slot.owner_key));
    }
    if (available_kernel_ids.empty()) {
        output << "    table.available_kernels = {};\n";
    } else {
        output << "    table.available_kernels =\n";
        std::size_t index = 0;
        for (const std::string& kernel : available_kernel_ids) {
            output << (index++ == 0 ? "        " : "        | ")
                   << "highway_kernel_mask_for(highway_kernel::" << kernel << ")\n";
        }
        output << "        ;\n";
    }

    if (!alphabet_identities.empty()) {
        output
            << "    static const ::bioseq::simd_dispatch::"
               "highway_alphabet_identity_entry alphabet_identities[]{\n";
        for (const alphabet_identity_binding& identity
             : alphabet_identities) {
            output
                << "        {.type_identity = "
                   "::bioseq::simd_dispatch::"
                   "highway_alphabet_identity_for<"
                << identity.alphabet_type << ">(), .alphabet_code = "
                << static_cast<unsigned int>(identity.alphabet_code)
                << "u},\n";
        }
        output << "    };\n"
               << "    table.alphabet_identities = alphabet_identities;\n";
    }

    for (const alignment_family_binding& family : alignment_families) {
        const auto code = static_cast<unsigned int>(family.alphabet_code);
        output << "    static const ::bioseq::simd::alignment_kernels<"
               << family.alphabet_type << "> alignment_" << code
               << " = make_alignment_" << code << "();\n";
    }
    if (!alignment_families.empty()) {
        output << "    static const ::bioseq::simd::alignment_table_entry "
                  "alignment_by_alphabet[]{\n";
        for (const alignment_family_binding& family : alignment_families) {
            const auto code = static_cast<unsigned int>(family.alphabet_code);
            output << "        {.alphabet_code = " << code
                   << "u, .typed_table_complete = "
                      "::bioseq::simd::is_alignment_kernel_table_complete("
                      "alignment_"
                   << code << "), .typed_table = &alignment_" << code
                   << "},\n";
        }
        output << "    };\n"
               << "    table.alignment_by_alphabet = "
                  "alignment_by_alphabet;\n";
    }
    for (const std::string& owner : collect_slot_owners(
             plan.source_inventory)) {
        const bool has_root_slots = std::ranges::any_of(
            plan.source_inventory.slots,
            [&owner](const slot_spec& slot) {
                return slot.owner_key == owner
                    && !is_alignment_family(slot.family_field);
            });
        if (has_root_slots) {
            output << "    binding_detail::" << population_name(owner)
                   << "(table);\n";
        }
    }
    output << "    return table;\n"
           << "}\n\n"
           << "HWY_ATTR const highway_target_binding* bind_highway_target() {\n"
           << "    static const highway_kernel_table kernels = "
              "make_highway_kernel_table();\n"
           << "    if (!kernels.is_complete()) {\n"
           << "        return nullptr;\n"
           << "    }\n"
           << "    static const highway_target_binding binding{\n"
           << "        .target = HWY_TARGET,\n"
           << "        .target_name = hwy::TargetName(HWY_TARGET),\n"
           << "        .kernels = &kernels,\n"
           << "    };\n"
           << "    return &binding;\n"
           << "}\n\n"
           << "}  // namespace bioseq::simd_dispatch::"
           << coordinate.identity_namespace
           << "\n";
    return output.str();
}

[[nodiscard]] inline std::string render_dispatch_registry_partition(
    const generation_plan& plan,
    const std::vector<namespace_materialization::materialization_coordinate>&
        coordinates) {
    std::ostringstream output;
    output << dispatch_fragment(plan.target_inventory)
           << "// Generated single Highway dispatch registry; do not edit.\n"
           << "module " << plan.source_inventory.dispatch_module
           << ":highway_registry;\n\n"
           << "import " << plan.source_inventory.dispatch_module << ";\n"
           << "import " << plan.source_inventory.contract_module << ";\n";
    for (const auto& coordinate : coordinates) {
        output << "import " << coordinate.module_name << ";\n";
    }
    output << "\nnamespace bioseq::simd_dispatch {\n\n"
           << "HWY_EXPORT(bind_highway_target);\n\n"
           << "const highway_target_binding& select_highway_target_once(\n"
              "    const highway_dispatch_plan& plan) {\n"
           << "    if (plan.planned_target == 0\n"
              "        || (plan.runnable_targets & plan.planned_target) == 0) {\n"
           << "        throw std::logic_error{\n"
              "            \"Highway dispatch plan has no runnable planned target\"};\n"
           << "    }\n"
           << "    hwy::GetChosenTarget().Update(\n"
              "        plan.cpu_supported_targets);\n"
           << "    const auto* binding = "
              "HWY_DYNAMIC_DISPATCH(bind_highway_target)();\n"
           << "    if (binding == nullptr || binding->target == 0\n"
              "        || binding->target_name == nullptr\n"
              "        || binding->kernels == nullptr\n"
              "        || !binding->kernels->is_complete()) {\n"
           << "        throw std::logic_error{"
              "\"Highway target binder returned an incomplete table\"};\n"
           << "    }\n"
           << "    if (binding->target != plan.planned_target) {\n"
           << "        throw std::logic_error{\n"
              "            \"Highway dynamic dispatch disagrees with the planned target\"};\n"
           << "    }\n"
           << "    return *binding;\n"
           << "}\n\n"
           << "}  // namespace bioseq::simd_dispatch\n";
    return output.str();
}

[[nodiscard]] inline std::string render_dispatch_inventory(
    const generation_plan& plan) {
    const std::uint64_t compiled_mask =
        compiled_target_mask(plan.target_inventory);
    std::ostringstream output;
    output << "module;\n\n"
           << "#include <cstdint>\n"
           << "#include <stdexcept>\n\n"
           << "// Generated cold Highway inventory; do not edit.\n"
           << "module " << plan.source_inventory.contract_module << ";\n\n";
    for (const target_spec& target : plan.target_inventory.targets) {
        output << "import " << generated_module_name(target) << ";\n";
    }
    output << "\nnamespace bioseq::simd_dispatch {\n\n"
           << "highway_dispatch_plan registered_highway_dispatch_plan() noexcept {\n"
           << "    return make_highway_dispatch_plan(static_cast<std::int64_t>("
           << compiled_mask << "ULL));\n"
           << "}\n\n"
           << "const highway_target_binding& registered_highway_target_binding(\n"
              "    const std::int64_t exact_target) {\n"
           << "    const auto target = static_cast<std::uint64_t>(exact_target);\n"
           << "    const highway_dispatch_plan plan =\n"
              "        registered_highway_dispatch_plan();\n"
           << "    if (exact_target <= 0 || (target & (target - 1ULL)) != 0\n"
              "        || (plan.compiled_targets & exact_target) == 0\n"
              "        || (plan.runnable_targets & exact_target) == 0) {\n"
           << "        throw std::logic_error{\n"
              "            \"Highway exact-target rebind requires one compiled runnable bit\"};\n"
           << "    }\n"
           << "    const highway_target_binding* binding = nullptr;\n"
           << "    switch (target) {\n";
    for (const target_spec& target : plan.target_inventory.targets) {
        output << "        case " << target.bit << "ULL:\n"
               << "            binding = " << target.target_namespace
               << "::bind_highway_target();\n"
               << "            break;\n";
    }
    output << "        default:\n"
              "            throw std::logic_error{\n"
              "                \"Highway exact-target rebind bit is not registered\"};\n"
              "    }\n"
           << "    if (binding == nullptr || binding->target != exact_target\n"
              "        || binding->target_name == nullptr\n"
              "        || binding->kernels == nullptr\n"
              "        || !binding->kernels->is_complete()) {\n"
           << "        throw std::logic_error{\n"
              "            \"Highway exact-target binder returned an invalid table\"};\n"
           << "    }\n"
           << "    return *binding;\n"
           << "}\n\n"
           << "}  // namespace bioseq::simd_dispatch\n";
    return output.str();
}

[[nodiscard]] inline std::string render_test_support(
    const generation_plan& plan,
    const std::vector<namespace_materialization::materialization_coordinate>&
        coordinates) {
    const target_spec& target = static_target(plan.target_inventory);
    const auto target_position = std::ranges::find(
        plan.target_inventory.targets,
        target.token,
        &target_spec::token);
    const std::size_t target_index = static_cast<std::size_t>(
        target_position - plan.target_inventory.targets.begin());
    if (coordinates.size() != plan.target_inventory.targets.size()) {
        throw std::runtime_error(
            "fixed-target test support received an incomplete coordinate set");
    }
    const auto& coordinate = coordinates.at(target_index);
    std::ostringstream output;
    output << "module;\n\n"
           << "#include <stdexcept>\n\n"
           << "// Generated fixed-target test binding; do not edit.\n"
           << "export module bioseq.highway.test_support;\n\n"
           << "import " << plan.source_inventory.contract_module << ";\n"
           << "import " << coordinate.module_name << ";\n\n"
           << "export namespace bioseq::simd_dispatch {\n\n"
           << "[[nodiscard]] const highway_target_binding& "
              "highway_test_target_binding() {\n"
           << "    const highway_target_binding* binding = "
           << coordinate.identity_namespace << "::bind_highway_target();\n"
           << "    if (binding == nullptr || binding->kernels == nullptr) {\n"
           << "        throw std::logic_error{"
              "\"generated fixed-target test binding is incomplete\"};\n"
           << "    }\n"
           << "    if (!binding->kernels->is_complete()) {\n"
           << "        throw std::logic_error{"
              "\"generated fixed-target test table is incomplete\"};\n"
           << "    }\n"
           << "    return *binding;\n"
           << "}\n\n"
           << "[[nodiscard]] const highway_kernel_table& "
              "highway_test_kernel_table() {\n"
           << "    return *highway_test_target_binding().kernels;\n"
           << "}\n\n"
           << "}  // namespace bioseq::simd_dispatch\n";
    return output.str();
}

[[nodiscard]] inline std::size_t count_text(
    std::string_view content,
    std::string_view needle) noexcept {
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = content.find(needle, position)) != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

inline void validate_rendered_artifact(
    const generated_artifact& artifact,
    const generation_plan& plan) {
    if (artifact.relative_path.extension() != ".cppm") {
        throw std::runtime_error(
            "Highway generator attempted non-.cppm output: "
            + artifact.relative_path.string());
    }
    static constexpr std::array<std::string_view, 5> universally_forbidden{
        "hwy/foreach_target.h",
        "HWY_BEFORE_NAMESPACE",
        "HWY_AFTER_NAMESPACE",
        "HWY_STATIC_DISPATCH",
        "[[bioseq::simd]]",
    };
    for (std::string_view spelling : universally_forbidden) {
        if (artifact.content.contains(spelling)) {
            throw std::runtime_error(
                artifact.relative_path.string()
                + ": generated artifact contains forbidden '"
                + std::string{spelling} + "'");
        }
    }
    if (artifact.kind == artifact_kind::dispatch_registry_partition) {
        if (count_text(artifact.content, "HWY_EXPORT(") != 1
            || count_text(artifact.content, "HWY_DYNAMIC_DISPATCH(") != 1
            || artifact.content.contains(
                "registered_highway_dispatch_plan")) {
            throw std::runtime_error(
                "generated dispatch registry partition must own exactly one "
                "export/dispatch and no cold inventory");
        }
    } else if (artifact.content.contains("HWY_EXPORT(")
               || artifact.content.contains("HWY_DYNAMIC_DISPATCH(")) {
        throw std::runtime_error(
            artifact.relative_path.string()
            + ": dispatch macros escaped the single generated registry");
    }
    if (artifact.kind
        == artifact_kind::dispatch_inventory_implementation) {
        if (!artifact.content.contains(
                "registered_highway_dispatch_plan() noexcept")
            || !artifact.content.contains(
                "registered_highway_target_binding(")
            || artifact.content.contains("select_highway_target_once")
            || artifact.content.contains("HWY_EXPORT(")
            || artifact.content.contains("HWY_DYNAMIC_DISPATCH(")
            || artifact.content.contains("HWY_STATIC_DISPATCH(")) {
            throw std::runtime_error(
                "generated dispatch inventory must contain cold metadata and "
                "the direct exact-target rebinder only");
        }
    }
    if (artifact.kind == artifact_kind::primary
        && (artifact.content.contains("#include <hwy/")
            || artifact.content.contains("HWY_TARGET")
            || artifact.content.contains("import :"))) {
        throw std::runtime_error(
            artifact.relative_path.string()
            + ": target primary is not a neutral minimal BMI");
    }
    if (artifact.kind == artifact_kind::binder
        && artifact.content.contains("import :")) {
        throw std::runtime_error(
            artifact.relative_path.string()
            + ": target binder must not import template partitions");
    }
    if (artifact.kind == artifact_kind::kernel_instantiation
        && (!artifact.content.contains("#define HWY_TARGET ")
            || !artifact.content.contains("import :")
            || !artifact.content.contains("HWY_ATTR void populate_")
            || artifact.content.contains("bind_highway_target"))) {
        throw std::runtime_error(
            artifact.relative_path.string()
            + ": kernel instantiation must own one target/partition binding");
    }
    if (artifact.kind == artifact_kind::minimizer_representation_instantiation
        && (!artifact.content.contains("#define HWY_TARGET ")
            || !artifact.content.contains("import :minimizer_")
            || !artifact.content.contains("_repr_")
            || artifact.content.contains("populate_")
            || artifact.content.contains("bind_highway_target"))) {
        throw std::runtime_error(
            artifact.relative_path.string()
            + ": minimizer representation owner must remain one bounded "
              "ordinary target implementation");
    }
    if (artifact.kind == artifact_kind::unit_probe_implementation
        && (!artifact.content.contains("#define HWY_TARGET ")
            || !artifact.content.contains("import :")
            || artifact.content.contains("export module")
            || artifact.content.contains("bind_highway_target"))) {
        throw std::runtime_error(
            artifact.relative_path.string()
            + ": target-private probe escaped its ordinary implementation "
              "ownership boundary");
    }
    if (artifact.kind == artifact_kind::unit_probe_implementation
        && !artifact.content.contains("BIOSEQ_TEST_INDEXES")) {
        throw std::runtime_error(
            artifact.relative_path.string()
            + ": unit probe lacks the BIOSEQ_TEST_INDEXES seam guard");
    }
}

}  // namespace detail

[[nodiscard]] inline generation_result render_generation(
    const generation_plan& plan) {
    generation_result result;
    const std::vector<namespace_materialization::materialization_coordinate>
        coordinates = detail::build_target_coordinates(
            plan.target_inventory);
    std::map<std::string, const source_spec*, std::less<>> sources_by_key;
    for (const extracted_source& source : plan.extracted_sources) {
        sources_by_key.emplace(source.source.key, &source.source);
    }

    for (std::size_t target_index = 0u;
         target_index < plan.target_inventory.targets.size();
         ++target_index) {
        const target_spec& target =
            plan.target_inventory.targets[target_index];
        const auto& coordinate = coordinates[target_index];
        const std::filesystem::path root =
            coordinate.relative_path.parent_path();
        result.artifacts.push_back(generated_artifact{
            .relative_path = coordinate.relative_path,
            .module_name = coordinate.module_name,
            .target_token = target.token,
            .kind = artifact_kind::primary,
            .content = detail::render_primary(
                plan.source_inventory, coordinate),
        });
        result.artifacts.push_back(generated_artifact{
            .relative_path = root / "binder.cppm",
            .module_name = coordinate.module_name,
            .target_token = target.token,
            .kind = artifact_kind::binder,
            .content = detail::render_binder(plan, target, coordinate),
        });
        for (const std::string& owner : detail::collect_slot_owners(
                 plan.source_inventory)) {
            const auto source = sources_by_key.find(owner);
            if (source == sources_by_key.end()) {
                throw std::runtime_error(
                    "renderer cannot resolve Highway slot owner '" + owner
                    + "' to a generated source partition");
            }
            result.artifacts.push_back(generated_artifact{
                .relative_path = root / "instances" / (owner + ".cppm"),
                .module_name = coordinate.module_name,
                .target_token = target.token,
                .source_key = owner,
                .kind = artifact_kind::kernel_instantiation,
                .content = detail::render_kernel_instantiation(
                    plan,
                    target,
                    coordinate,
                    owner,
                    *source->second),
            });
        }
        for (const minimizer_representation_instance_spec& instance :
             plan.source_inventory.minimizer_representation_instances) {
            const auto source = sources_by_key.find(instance.source_key);
            if (source == sources_by_key.end()) {
                throw std::runtime_error(
                    "renderer cannot resolve minimizer representation owner '"
                    + instance.owner_key + "' source '" + instance.source_key
                    + "'");
            }
            result.artifacts.push_back(generated_artifact{
                .relative_path = root / "instances"
                    / ("minimizer_representation_" + instance.owner_key + ".cppm"),
                .module_name = coordinate.module_name,
                .target_token = target.token,
                .source_key = instance.source_key,
                .kind = artifact_kind::minimizer_representation_instantiation,
                .content = detail::render_minimizer_representation_instantiation(
                    plan, target, coordinate, instance, *source->second),
            });
        }
        for (const extracted_source& source : plan.extracted_sources) {
            result.artifacts.push_back(generated_artifact{
                .relative_path = root / (source.source.partition + ".cppm"),
                .module_name = coordinate.module_name + ":"
                    + source.source.partition,
                .target_token = target.token,
                .source_key = source.source.key,
                .kind = artifact_kind::partition,
                .content = detail::render_partition(
                    plan, target, coordinate, source, sources_by_key),
            });
        }
        if (target.token == plan.target_inventory.static_target_token) {
            for (const probe_spec& probe :
                 plan.source_inventory.unit_probes) {
                result.artifacts.push_back(generated_artifact{
                    .relative_path = root / "probes"
                        / ("unit_" + probe.key + ".cppm"),
                    .module_name = coordinate.module_name,
                    .target_token = target.token,
                    .source_key = probe.key,
                    .kind = artifact_kind::unit_probe_implementation,
                    .content = detail::render_probe_implementation(
                        plan, target, coordinate, probe),
                });
            }
        }
    }
    result.artifacts.push_back(generated_artifact{
        .relative_path = "highway_registry.cppm",
        .module_name =
            plan.source_inventory.dispatch_module + ":highway_registry",
        .kind = artifact_kind::dispatch_registry_partition,
        .content = detail::render_dispatch_registry_partition(
            plan, coordinates),
    });
    result.artifacts.push_back(generated_artifact{
        .relative_path = "dispatch_inventory.cppm",
        .module_name = plan.source_inventory.contract_module,
        .kind = artifact_kind::dispatch_inventory_implementation,
        .content = detail::render_dispatch_inventory(plan),
    });
    result.artifacts.push_back(generated_artifact{
        .relative_path = "test_support.cppm",
        .module_name = "bioseq.highway.test_support",
        .target_token = plan.target_inventory.static_target_token,
        .kind = artifact_kind::test_support,
        .content = detail::render_test_support(plan, coordinates),
    });
    std::ranges::sort(
        result.artifacts,
        {},
        [](const generated_artifact& artifact) {
            return artifact.relative_path.generic_string();
        });
    std::set<std::filesystem::path> paths;
    std::set<std::string, std::less<>> providers;
    for (const generated_artifact& artifact : result.artifacts) {
        if (!paths.insert(artifact.relative_path).second) {
            throw std::runtime_error(
                "duplicate generated Highway output path: "
                + artifact.relative_path.string());
        }
        if (artifact.kind == artifact_kind::primary
            || artifact.kind == artifact_kind::partition
            || artifact.kind
                == artifact_kind::dispatch_registry_partition
            || artifact.kind == artifact_kind::test_support) {
            if (!providers.insert(artifact.module_name).second) {
                throw std::runtime_error(
                    "duplicate generated Highway module provider: "
                    + artifact.module_name);
            }
        }
        detail::validate_rendered_artifact(artifact, plan);
    }
    result.policy_evidence_json =
        render_policy_evidence(plan, result.artifacts);
    result.publication_topology_digest = bioseq::common::sha256_hex(
        "bioseq_highway_publication_topology\n"
        "revision=1\n"
        + result.policy_evidence_json);
    return result;
}

[[nodiscard]] inline std::vector<
    namespace_materialization::publication_artifact>
make_publication_artifacts(
    const generation_result& result) {
    std::vector<namespace_materialization::publication_artifact> publication;
    publication.reserve(result.artifacts.size());
    for (const generated_artifact& artifact : result.artifacts) {
        publication.push_back(
            namespace_materialization::publication_artifact{
                .relative_path = artifact.relative_path,
                .content = artifact.content,
                .content_sha256 = bioseq::common::sha256_hex(artifact.content),
                .artifact_class = namespace_materialization::
                    publication_artifact_class::production_cppm,
            });
    }
    return publication;
}

[[nodiscard]] inline namespace_materialization::rendered_publication_manifest
render_highway_publication_manifest(
    const std::filesystem::path& output_root,
    std::string_view prior_manifest_identity,
    const generation_plan& plan,
    const generation_result& result,
    const std::vector<namespace_materialization::publication_artifact>&
        publication) {
    const std::string expected_policy_evidence =
        render_policy_evidence(plan, result.artifacts);
    const std::string expected_topology_digest = bioseq::common::sha256_hex(
        "bioseq_highway_publication_topology\n"
        "revision=1\n"
        + expected_policy_evidence);
    if (result.policy_evidence_json != expected_policy_evidence
        || result.publication_topology_digest != expected_topology_digest
        || publication.size() != result.artifacts.size()) {
        throw std::runtime_error(
            "Highway publication adapter received stale policy evidence");
    }
    for (std::size_t index = 0; index < publication.size(); ++index) {
        const generated_artifact& generated = result.artifacts[index];
        const namespace_materialization::publication_artifact& published =
            publication[index];
        if (published.relative_path != generated.relative_path
            || published.content != generated.content
            || published.content_sha256 != bioseq::common::sha256_hex(generated.content)
            || published.artifact_class
                != namespace_materialization::publication_artifact_class::
                    production_cppm) {
            throw std::runtime_error(
                "Highway publication closure disagrees with its policy evidence");
        }
    }
    return namespace_materialization::render_publication_manifest(
        namespace_materialization::publication_manifest_request{
            .output_root = output_root,
            .policy = std::string{publication_policy},
            .name = std::string{publication_name},
            .topology_digest = result.publication_topology_digest,
            .prior_manifest_identity =
                std::string{prior_manifest_identity},
            .policy_evidence_json = result.policy_evidence_json,
            .artifacts = publication,
        });
}

}  // namespace bioseq::codegen::highway
