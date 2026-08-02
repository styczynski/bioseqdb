module;

#include <algorithm>
#include <array>
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
import bioseq.codegen.namespace_materialization;
import bioseq.codegen.support;

export namespace bioseq::codegen::highway {

namespace detail {

[[nodiscard]] inline std::string generated_module_name(
    const target_spec& target) {
    return "bioseq.highway.generated." + target.platform + "." + target.slug;
}

[[nodiscard]] inline std::filesystem::path generated_target_root(
    const target_spec& target) {
    return std::filesystem::path{target.platform} / target.slug;
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
           << "#include <hwy/highway.h>\n\n";
    return output.str();
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
    const std::string value = source.generic_string();
    const std::size_t main = value.rfind("/main/bioseq/");
    if (main != std::string::npos) {
        return value.substr(main + 1);
    }
    return source.filename().generic_string();
}

[[nodiscard]] inline std::string body_with_highway_attributes(
    const extracted_source& source) {
    std::string result = source.body;
    for (auto callable = source.callables.rbegin();
         callable != source.callables.rend();
         ++callable) {
        const bool lambda = callable->kind == "lambda";
        if (callable->body_relative_offset >= result.size()
            || (lambda && result[callable->body_relative_offset] != '{')) {
            throw std::runtime_error(
                source.source.path.string()
                + ": stale HWY_ATTR insertion geometry while rendering");
        }
        result.insert(
            callable->body_relative_offset,
            lambda ? " HWY_ATTR" : "HWY_ATTR ");
    }
    return result;
}

[[nodiscard]] inline std::string replace_target_namespace(
    std::string expression,
    std::string_view name_space) {
    constexpr std::string_view marker = "{target_namespace}";
    const std::size_t position = expression.find(marker);
    if (position == std::string::npos
        || expression.find(marker, position + marker.size())
            != std::string::npos) {
        throw std::runtime_error(
            "slot expression lost its unique target namespace placeholder");
    }
    expression.replace(position, marker.size(), name_space);
    return expression;
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
 * materializes one strongly typed AlignmentKernels<Alphabet> object per
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
        "ksw2_z2",
        "ksw2_d2",
        "ksw2_batched",
        "block_prepare",
        "block_try_certified_gap",
        "block_align",
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

[[nodiscard]] inline std::string_view kernel_enumerator_for_owner(
    std::string_view owner) {
    if (owner == "minimizer_reference") {
        return "minimizer_reference_minimap_int";
    }
    if (owner == "minimizer_two_stack") {
        return "minimizer_two_stack_nthash";
    }
    if (owner == "minimizer_paper") {
        return "minimizer_paper_nthash";
    }
    if (owner == "indexing") return "indexing_grouped32_validate";
    if (owner == "chaining") return "chaining_chain_anchors_simd";
    if (owner == "prefilter") {
        return "prefilter_sneaky_snake_simd_batch";
    }
    if (owner == "ksw2") return "alignment_ksw2";
    if (owner == "ksw2_batched") return "alignment_ksw2_batched";
    if (owner == "block_aligner") return "alignment_block_aligner";
    throw std::runtime_error(
        "Highway table slot owner '" + std::string{owner}
        + "' has no HighwayKernelId mapping");
}

[[nodiscard]] inline std::string render_primary(
    const source_manifest& inventory,
    const target_spec& target) {
    const std::string module = generated_module_name(target);
    std::ostringstream output;
    output << "// Generated by bioseq_codegen highway; do not edit.\n"
           << "export module " << module << ";\n\n"
           << "import " << inventory.contract_module << ";\n\n"
           << "export namespace bioseq::simd_dispatch::"
           << target.name_space << " {\n"
           << "[[nodiscard]] const HighwayTargetBinding* "
              "BindHighwayTarget() noexcept;\n"
           << "}  // namespace bioseq::simd_dispatch::"
           << target.name_space << "\n";
    return output.str();
}

[[nodiscard]] inline std::string render_partition(
    const generation_plan& plan,
    const target_spec& target,
    const extracted_source& source,
    const std::map<std::string, const source_spec*, std::less<>>& by_key) {
    const std::string module = generated_module_name(target);
    std::ostringstream output;
    output << fixed_target_fragment(plan.target_inventory, target)
           << "// Generated from " << source.source.key << "; do not edit.\n"
           << "module " << module << ":" << source.source.partition << ";\n\n";
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
           << target.name_space << " {\n"
           << "#line " << source.body_first_line << " \""
           << cpp_escape(diagnostic_source_path(source.source.path)) << "\"\n"
           << body_with_highway_attributes(source);
    if (source.body.empty() || source.body.back() != '\n') output << '\n';
    output << "}  // namespace " << source.enclosing_namespace << "::"
           << target.name_space << "\n";
    return output.str();
}

[[nodiscard]] inline std::string render_binder(
    const generation_plan& plan,
    const target_spec& target) {
    const std::string module = generated_module_name(target);
    std::ostringstream output;
    output << fixed_target_fragment(plan.target_inventory, target)
           << "// Generated target binding; do not edit.\n"
           << "module " << module << ";\n\n"
           << "import " << plan.source_inventory.contract_module << ";\n";
    for (const slot_spec& slot : plan.source_inventory.slots) {
        if (is_alignment_family(slot.family_field)) {
            output << "import bioseq.sequences;\n";
            break;
        }
    }
    for (const extracted_source& source : plan.extracted_sources) {
        output << "import :" << source.source.partition << ";\n";
    }
    output << "\nnamespace bioseq::simd_dispatch::" << target.name_space
           << " {\n\n"
           << "HWY_ATTR HighwayKernelTable MakeHighwayKernelTable() noexcept {\n"
           << "    HighwayKernelTable table{};\n";

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
                   << "highway_kernel_set(HighwayKernelId::" << kernel << ")\n";
        }
        output << "        ;\n";
    }

    std::map<std::string, std::vector<const slot_spec*>, std::less<>> families;
    for (const slot_spec& slot : plan.source_inventory.slots) {
        families[slot.family_field].push_back(&slot);
    }
    const std::vector<alignment_family_binding> alignment_families =
        collect_alignment_families(families);
    for (const alignment_family_binding& family : alignment_families) {
        const auto code = static_cast<unsigned int>(family.alphabet_code);
        output << "    static const ::bioseq::simd::AlignmentKernels<"
               << family.alphabet_type << "> alignment_" << code << "{\n";
        for (const std::string_view member : alignment_required_members) {
            const slot_spec* slot = family.members.find(member)->second;
            output << "        ." << member << " = "
                   << replace_target_namespace(
                          slot->expression, target.name_space)
                   << ",\n";
        }
        output << "    };\n";
    }
    if (!alignment_families.empty()) {
        output << "    static const ::bioseq::simd::AlignmentTableEntry "
                  "alignment_by_alphabet[]{\n";
        for (const alignment_family_binding& family : alignment_families) {
            const auto code = static_cast<unsigned int>(family.alphabet_code);
            output << "        {.alphabet_code = " << code
                   << "u, .typed_table = &alignment_" << code << "},\n";
        }
        output << "    };\n"
               << "    table.alignment_by_alphabet = "
                  "alignment_by_alphabet;\n";
    }
    for (const auto& [family, slots] : families) {
        if (is_alignment_family(family)) continue;
        if (slots.size() == 1 && slots.front()->slot_field == "@") {
            output << "    table." << family << " = "
                   << replace_target_namespace(
                          slots.front()->expression, target.name_space)
                   << ";\n";
            continue;
        }
        for (const slot_spec* slot : slots) {
            output << "    table." << family << "." << slot->slot_field
                   << " = "
                   << replace_target_namespace(slot->expression, target.name_space)
                   << ";\n";
        }
    }
    output << "    return table;\n"
           << "}\n\n"
           << "HWY_ATTR const HighwayTargetBinding* BindHighwayTarget() noexcept {\n"
           << "    static const HighwayKernelTable kernels = "
              "MakeHighwayKernelTable();\n"
           << "    if (!kernels.complete()) {\n"
           << "        return nullptr;\n"
           << "    }\n"
           << "    static const HighwayTargetBinding binding{\n"
           << "        .target = HWY_TARGET,\n"
           << "        .target_name = hwy::TargetName(HWY_TARGET),\n"
           << "        .kernels = &kernels,\n"
           << "    };\n"
           << "    return &binding;\n"
           << "}\n\n"
           << "}  // namespace bioseq::simd_dispatch::" << target.name_space
           << "\n";
    return output.str();
}

[[nodiscard]] inline std::string render_dispatch_registry_partition(
    const generation_plan& plan) {
    std::ostringstream output;
    output << dispatch_fragment(plan.target_inventory)
           << "// Generated single Highway dispatch registry; do not edit.\n"
           << "module " << plan.source_inventory.dispatch_module
           << ":highway_registry;\n\n"
           << "import " << plan.source_inventory.dispatch_module << ";\n"
           << "import " << plan.source_inventory.contract_module << ";\n";
    for (const target_spec& target : plan.target_inventory.targets) {
        output << "import " << generated_module_name(target) << ";\n";
    }
    output << "\nnamespace bioseq::simd_dispatch {\n\n"
           << "HWY_EXPORT(BindHighwayTarget);\n\n"
           << "const HighwayTargetBinding& select_highway_target_once(\n"
              "    const HighwayDispatchInfo& plan) {\n"
           << "    if (plan.planned_target == 0\n"
              "        || (plan.runnable_targets & plan.planned_target) == 0) {\n"
           << "        throw std::logic_error{\n"
              "            \"Highway dispatch plan has no runnable planned target\"};\n"
           << "    }\n"
           << "    hwy::GetChosenTarget().Update(\n"
              "        plan.cpu_supported_targets);\n"
           << "    const auto* binding = "
              "HWY_DYNAMIC_DISPATCH(BindHighwayTarget)();\n"
           << "    if (binding == nullptr || binding->target == 0\n"
              "        || binding->target_name == nullptr\n"
              "        || binding->kernels == nullptr\n"
              "        || !binding->kernels->complete()) {\n"
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
           << "\n"
           << "// Generated cold Highway inventory; do not edit.\n"
           << "module " << plan.source_inventory.contract_module << ";\n\n"
           << "namespace bioseq::simd_dispatch {\n\n"
           << "HighwayDispatchInfo registered_highway_dispatch_info() noexcept {\n"
           << "    return highway_dispatch_info(static_cast<std::int64_t>("
           << compiled_mask << "ULL));\n"
           << "}\n\n"
           << "}  // namespace bioseq::simd_dispatch\n";
    return output.str();
}

[[nodiscard]] inline std::string render_test_support(
    const generation_plan& plan) {
    const target_spec& target = static_target(plan.target_inventory);
    std::ostringstream output;
    output << "module;\n\n"
           << "#include <stdexcept>\n\n"
           << "// Generated fixed-target test binding; do not edit.\n"
           << "export module bioseq.highway.test_support;\n\n"
           << "import " << plan.source_inventory.contract_module << ";\n"
           << "import " << generated_module_name(target) << ";\n\n"
           << "export namespace bioseq::simd_dispatch {\n\n"
           << "[[nodiscard]] const HighwayTargetBinding& "
              "highway_test_target_binding() {\n"
           << "    const HighwayTargetBinding* binding = "
           << target.name_space << "::BindHighwayTarget();\n"
           << "    if (binding == nullptr || binding->kernels == nullptr) {\n"
           << "        throw std::logic_error{"
              "\"generated fixed-target test binding is incomplete\"};\n"
           << "    }\n"
           << "    if (!binding->kernels->complete()) {\n"
           << "        throw std::logic_error{"
              "\"generated fixed-target test table is incomplete\"};\n"
           << "    }\n"
           << "    return *binding;\n"
           << "}\n\n"
           << "[[nodiscard]] const HighwayKernelTable& "
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
            "Highway generator attempted non-.cppm production output: "
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
                "registered_highway_dispatch_info")) {
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
        // This intentionally duplicates the renderer's tiny allowed grammar.
        // A future renderer edit must satisfy this independent fail-closed
        // contract: one module attachment, one literal-mask metadata call,
        // and no imports, binders, selectors, or other declarations.
        std::ostringstream expected;
        expected << "module;\n\n"
                 << "#include <cstdint>\n\n"
                 << "// Generated cold Highway inventory; do not edit.\n"
                 << "module " << plan.source_inventory.contract_module
                 << ";\n\n"
                 << "namespace bioseq::simd_dispatch {\n\n"
                 << "HighwayDispatchInfo "
                    "registered_highway_dispatch_info() noexcept {\n"
                 << "    return highway_dispatch_info("
                    "static_cast<std::int64_t>("
                 << compiled_target_mask(plan.target_inventory)
                 << "ULL));\n"
                 << "}\n\n"
                 << "}  // namespace bioseq::simd_dispatch\n";
        if (artifact.content != expected.str()) {
            throw std::runtime_error(
                "generated dispatch inventory must contain exactly one "
                "literal-mask cold metadata definition");
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
}

}  // namespace detail

[[nodiscard]] inline generation_result render_generation(
    const generation_plan& plan) {
    generation_result result;
    std::map<std::string, const source_spec*, std::less<>> sources_by_key;
    for (const extracted_source& source : plan.extracted_sources) {
        sources_by_key.emplace(source.source.key, &source.source);
    }

    for (const target_spec& target : plan.target_inventory.targets) {
        const std::filesystem::path root = detail::generated_target_root(target);
        const std::string module = detail::generated_module_name(target);
        result.artifacts.push_back(generated_artifact{
            .relative_path = root / "index.cppm",
            .module_name = module,
            .target_token = target.token,
            .kind = artifact_kind::primary,
            .content = detail::render_primary(plan.source_inventory, target),
        });
        result.artifacts.push_back(generated_artifact{
            .relative_path = root / "binder.cppm",
            .module_name = module,
            .target_token = target.token,
            .kind = artifact_kind::binder,
            .content = detail::render_binder(plan, target),
        });
        for (const extracted_source& source : plan.extracted_sources) {
            result.artifacts.push_back(generated_artifact{
                .relative_path = root / (source.source.partition + ".cppm"),
                .module_name = module + ":" + source.source.partition,
                .target_token = target.token,
                .source_key = source.source.key,
                .kind = artifact_kind::partition,
                .content = detail::render_partition(
                    plan, target, source, sources_by_key),
            });
        }
    }
    result.artifacts.push_back(generated_artifact{
        .relative_path = "highway_registry.cppm",
        .module_name =
            plan.source_inventory.dispatch_module + ":highway_registry",
        .kind = artifact_kind::dispatch_registry_partition,
        .content = detail::render_dispatch_registry_partition(plan),
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
        .content = detail::render_test_support(plan),
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
    result.publication_topology_digest = sha256_hex(
        "bioseq_highway_publication_topology/v1\n"
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
                .content_sha256 = sha256_hex(artifact.content),
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
    const std::string expected_topology_digest = sha256_hex(
        "bioseq_highway_publication_topology/v1\n"
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
            || published.content_sha256 != sha256_hex(generated.content)
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
