module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.namespace_materialization:renderer;

import :manifest;
import :model;
import :validator;
import bioseq.codegen.structural_analysis;
import bioseq.codegen.support;

export namespace bioseq::codegen::namespace_materialization {

namespace detail {

[[nodiscard]] inline bool policy_attribute(
    std::string_view qualified_name) noexcept {
    return qualified_name == "bioseq::template_flatten"
        || qualified_name == "bioseq::template_flatten_when"
        || qualified_name == "bioseq::template_flatten_entry"
        || qualified_name == "bioseq::template_flatten_provenance";
}

[[nodiscard]] inline std::string render_slice(
    std::string_view source,
    const structural_analysis::source_range& range,
    std::string_view role) {
    if (range.begin.byte_offset > range.end.byte_offset
        || range.end.byte_offset > source.size()) {
        throw std::runtime_error(
            "cannot render " + std::string{role}
            + ": structural range is outside the authored source");
    }
    return std::string{source.substr(
        range.begin.byte_offset,
        range.end.byte_offset - range.begin.byte_offset)};
}

[[nodiscard]] inline std::vector<source_edit> policy_attribute_edits(
    const structural_analysis::declaration_record& declaration,
    const structural_analysis::source_range& containing_range) {
    std::vector<source_edit> edits;
    for (const structural_analysis::attribute_record& attribute
         : declaration.attributes) {
        if (!policy_attribute(attribute.qualified_name)) continue;
        const structural_analysis::source_range& range = attribute_range(attribute);
        if (!contains(containing_range, range)) continue;
        edits.push_back(source_edit{
            .begin_byte = range.begin.byte_offset,
            .end_byte = range.end.byte_offset,
            .source_identity = declaration.source_identity,
            .purpose = "strip policy attribute " + attribute.qualified_name,
        });
    }
    return edits;
}

[[nodiscard]] inline std::string module_identifier(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 1U);
    if (value.empty() || (value.front() >= '0' && value.front() <= '9')) {
        result.push_back('_');
    }
    for (const char character : value) {
        const bool valid = (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9') || character == '_';
        result.push_back(valid ? character : '_');
    }
    return result;
}

[[nodiscard]] inline std::string parent_namespace(std::string_view value) {
    const std::size_t separator = value.rfind("::");
    return separator == std::string_view::npos
        ? std::string{}
        : std::string{value.substr(0U, separator)};
}

[[nodiscard]] inline std::string global_module_fragment(
    const generation_plan& plan) {
    const std::size_t end =
        plan.topology.region.source_module.declaration_range.begin.byte_offset;
    if (end > plan.source_text.size()) {
        throw std::runtime_error(
            "source module declaration range is outside the authored source");
    }
    std::string result = plan.source_text.substr(0U, end);
    if (!result.empty() && result.back() != '\n') result.push_back('\n');
    return result;
}

[[nodiscard]] inline std::vector<std::string> neutral_imports(
    const generation_plan& plan) {
    std::vector<std::string> result;
    std::set<std::string, std::less<>> seen;
    for (const std::string& imported
         : plan.topology.region.source_module.imported_modules) {
        if (imported == plan.topology.region.configured_output_module_name
            || imported == plan.topology.region.source_module.provided_module
            || !seen.insert(imported).second) {
            continue;
        }
        result.push_back(imported);
    }
    return result;
}

[[nodiscard]] inline std::string_view coordinate_case(
    const materialization_coordinate& coordinate,
    std::string_view group) noexcept {
    const auto assignment = std::ranges::find(
        coordinate.assignments, group, &axis_case_assignment::group);
    return assignment == coordinate.assignments.end()
        ? std::string_view{}
        : std::string_view{assignment->case_label};
}

[[nodiscard]] inline bool scheduler_root_coordinate(
    const generation_plan& plan,
    const materialization_coordinate& coordinate) noexcept {
    return plan.topology.region.configured_output_module_name
            == scheduler_output_module
        && coordinate_case(coordinate, "shape") == "root";
}

[[nodiscard]] inline bool scheduler_scanner_coordinate(
    const generation_plan& plan,
    const materialization_coordinate& coordinate) noexcept {
    return plan.topology.region.configured_output_module_name
            == scheduler_output_module
        && coordinate_case(coordinate, "shape") == "scanner_pg";
}

[[nodiscard]] inline std::string scheduler_implementation_global_module_fragment(
    const generation_plan& plan,
    const materialization_coordinate& coordinate) {
    std::string fragment = global_module_fragment(plan);
    const bool root = scheduler_root_coordinate(plan, coordinate);
    const bool scanner = scheduler_scanner_coordinate(plan, coordinate);
    if (!root && !scanner) return fragment;

    std::vector<std::string_view> removable_lines{
        "#include <array>\n",
        "#include <limits>\n",
        "#include <map>\n",
        "#include <memory>\n",
        "#include <stdexcept>\n",
    };
    if (root || scanner) {
        removable_lines.insert(
            removable_lines.end(),
            {"#include <cstddef>\n",
             "#include <cstdint>\n",
             "#include <optional>\n",
             "#include <utility>\n",
             "#include <vector>\n"});
    }
    std::vector<source_edit> edits;
    edits.reserve(removable_lines.size() + (scanner ? 1U : 0U));
    if (scanner) {
        constexpr std::string_view scanner_postgres_block{
            "extern \"C\" {\n"
            "#include \"postgres.h\"\n"
            "#include \"access/attnum.h\"\n"
            "}\n"
            "\n"
            "#undef Min\n"
            "#undef Max\n"
            "#undef snprintf\n"
            "#undef vsnprintf\n"
            "\n"};
        const std::size_t begin = fragment.find(scanner_postgres_block);
        if (begin == std::string::npos
            || fragment.find(
                   scanner_postgres_block,
                   begin + scanner_postgres_block.size())
                != std::string::npos) {
            throw std::runtime_error(
                "scheduler scanner PostgreSQL GMF anchor is absent or ambiguous");
        }
        constexpr std::string_view scanner_palloc_anchor{
            "extern \"C\" {\n"
            "#include \"c.h\"\n"
            "#include \"utils/palloc.h\"\n"
            "}\n"
            "\n"};
        edits.push_back(source_edit{
            .begin_byte = begin,
            .end_byte = begin + scanner_postgres_block.size(),
            .replacement = std::string{scanner_palloc_anchor},
            .source_identity =
                plan.topology.region.boundary.source_identity,
            .purpose =
                "narrow scheduler scanner PostgreSQL GMF block to palloc anchor",
        });
    }
    for (const std::string_view line : removable_lines) {
        const std::size_t begin = fragment.find(line);
        if (begin == std::string::npos
            || fragment.find(line, begin + line.size()) != std::string::npos) {
            throw std::runtime_error(
                "scheduler implementation GMF standard-header anchor is absent or ambiguous: "
                + std::string{line});
        }
        edits.push_back(source_edit{
            .begin_byte = begin,
            .end_byte = begin + line.size(),
            .source_identity = plan.topology.region.boundary.source_identity,
            .purpose = "strip scheduler implementation GMF standard header",
        });
    }
    return apply_exact_edits(
        std::move(fragment), std::move(edits), 0U);
}

[[nodiscard]] inline bool scheduler_root_std_entity(
    const std::string_view name) noexcept {
    return name == "addressof" || name == "array"
        || name == "exchange" || name == "forward"
        || name == "logic_error" || name == "map"
        || name == "move" || name == "nullopt"
        || name == "numeric_limits" || name == "optional"
        || name == "shared_ptr"
        || name == "size_t" || name == "uint8_t"
        || name == "uint16_t" || name == "uint64_t"
        || name == "vector";
}

[[nodiscard]] inline std::string rewrite_scheduler_root_std_qualifiers(
    const generation_plan& plan,
    const materialization_coordinate& coordinate,
    std::string source) {
    if (!scheduler_root_coordinate(plan, coordinate)) return source;
    const structural_analysis::translation_unit unit =
        structural_analysis::scan_token_slice(coordinate.relative_path, source);
    std::vector<source_edit> edits;
    for (std::size_t index = 0U; index < unit.tokens.size(); ++index) {
        const structural_analysis::lexical_token& token = unit.tokens[index];
        if (token.spelling != "std") continue;
        if (index + 2U >= unit.tokens.size()
            || unit.tokens[index + 1U].spelling != "::"
            || !scheduler_root_std_entity(unit.tokens[index + 2U].spelling)) {
            throw std::runtime_error(
                "scheduler root body contains an unaudited std qualifier");
        }
        edits.push_back(source_edit{
            .begin_byte = token.range.begin.byte_offset,
            .end_byte = token.range.end.byte_offset,
            .replacement = "typed_support_detail::root_std",
            .source_identity = coordinate.relative_path.generic_string(),
            .purpose = "bind scheduler root standard entity through light owner",
        });
    }
    return apply_exact_edits(
        std::move(source), std::move(edits), 0U);
}

[[nodiscard]] inline bool scheduler_scanner_std_entity(
    const std::string_view name) noexcept {
    return name == "forward" || name == "move"
        || name == "optional" || name == "shared_ptr"
        || name == "size_t"
        || name == "uint8_t" || name == "uint16_t"
        || name == "vector";
}

[[nodiscard]] inline std::string rewrite_scheduler_scanner_qualifiers(
    const generation_plan& plan,
    const materialization_coordinate& coordinate,
    std::string source) {
    if (!scheduler_scanner_coordinate(plan, coordinate)) return source;

    const structural_analysis::translation_unit unit =
        structural_analysis::scan_token_slice(
            coordinate.relative_path, source);
    std::vector<source_edit> edits;

    for (std::size_t index = 0U; index < unit.tokens.size(); ++index) {
        const structural_analysis::lexical_token& token = unit.tokens[index];

        if (token.spelling == "std") {
            if (index + 2U >= unit.tokens.size()
                || unit.tokens[index + 1U].spelling != "::"
                || !scheduler_scanner_std_entity(
                    unit.tokens[index + 2U].spelling)) {
                throw std::runtime_error(
                    "scheduler scanner body contains an unaudited std qualifier");
            }
            edits.push_back(source_edit{
                .begin_byte = token.range.begin.byte_offset,
                .end_byte = token.range.end.byte_offset,
                .replacement = "typed_support_detail::scanner_std",
                .source_identity =
                    coordinate.relative_path.generic_string(),
                .purpose =
                    "bind scheduler scanner standard entity through light owner",
            });
            continue;
        }

        if (token.spelling == "uint8_t"
            && !(index >= 2U
                 && unit.tokens[index - 2U].spelling == "std"
                 && unit.tokens[index - 1U].spelling == "::")) {
            edits.push_back(source_edit{
                .begin_byte = token.range.begin.byte_offset,
                .end_byte = token.range.end.byte_offset,
                .replacement =
                    "typed_support_detail::scanner_std::uint8_t",
                .source_identity =
                    coordinate.relative_path.generic_string(),
                .purpose =
                    "bind scheduler scanner uint8_t through light owner",
            });
            continue;
        }

        if (token.spelling == "AttrNumber") {
            edits.push_back(source_edit{
                .begin_byte = token.range.begin.byte_offset,
                .end_byte = token.range.end.byte_offset,
                .replacement =
                    "typed_support_detail::scanner_attr_number",
                .source_identity =
                    coordinate.relative_path.generic_string(),
                .purpose =
                    "bind scheduler scanner AttrNumber through light owner",
            });
            continue;
        }

        if (token.spelling == "Datum") {
            edits.push_back(source_edit{
                .begin_byte = token.range.begin.byte_offset,
                .end_byte = token.range.end.byte_offset,
                .replacement =
                    "typed_support_detail::scanner_datum",
                .source_identity =
                    coordinate.relative_path.generic_string(),
                .purpose =
                    "bind scheduler scanner Datum through light owner",
            });
        }
    }

    return apply_exact_edits(
        std::move(source), std::move(edits), 0U);
}

[[nodiscard]] inline bool
scheduler_sibling_declaration_required(const generation_plan& plan,
                                       const materialization_coordinate& coordinate,
                                       const owned_declaration& declaration) {
    if (plan.topology.region.configured_output_module_name != scheduler_output_module) {
        return true;
    }

    const std::string_view shape = coordinate_case(coordinate, "shape");
    if (shape == "root") {
        return declaration.entry_role == "scanner_pg" ||
               declaration.entry_role == "inmemory_standard" ||
               declaration.entry_role == "inmemory_segmented" ||
               declaration.entry_role == "inmemory_chunked" ||
               declaration.entry_role == "inmemory_chunked_segmented";
    }
    if (shape == "inmemory") {
        return declaration.entry_role == "inmemory_standard" ||
               declaration.entry_role == "inmemory_segmented" ||
               declaration.entry_role == "inmemory_chunked" ||
               declaration.entry_role == "inmemory_chunked_segmented";
    }
    if (shape == "scanner_pg" || shape == "inmemory_standard" || shape == "inmemory_segmented" ||
        shape == "inmemory_chunked" || shape == "inmemory_chunked_segmented") {
        return false;
    }
    throw std::runtime_error(
        "scheduler implementation has an unaudited shape for local API declarations");
}

/**
 * Checks which imports the selected scheduler body needs.
 *
 * The source contains all seven bodies for analysis. Copying every import
 * into each generated file would load compiled modules for unrelated bodies.
 */
[[nodiscard]] inline bool scheduler_body_import_required(
    const generation_plan& plan,
    const materialization_coordinate& coordinate,
    std::string_view imported) noexcept {
    if (plan.topology.region.configured_output_module_name
        != scheduler_output_module) {
        return true;
    }

    const std::string_view shape = coordinate_case(coordinate, "shape");
    if (shape.empty()) return true;

    if (imported
        == "bioseq.postgres.storage.indexing.chunked_storage") {
        return false;
    }
    if (imported
        == "bioseq.postgres.storage.indexing.fragment_handle") {
        return false;
    }
    if (imported
        == "bioseq.postgres.scheduler.dispatch_processor.source_provider_promotion") {
        return shape == "root" || shape == "scanner_pg";
    }
    if (imported
        == "bioseq.postgres.scheduler.dispatch_processor.fragment_storage_opener") {
        return shape == "scanner_pg";
    }

    if (imported == "bioseq.storage.layout") {
        return shape == "root" || shape == "scanner_pg";
    }

    if (imported == "bioseq.sequences") {
        return true;
    }

    if (imported
            == "bioseq.postgres.scheduler.dispatch_processor.typed_support.row_adapters"
        || imported
            == "bioseq.postgres.scheduler.dispatch_processor.typed_support.resident_target_materialization"
        || imported
            == "bioseq.postgres.scheduler.dispatch_processor.typed_support.generic_rows"
        || imported
            == "bioseq.postgres.scheduler.dispatch_processor.typed_support.cpu_batch_route"
        || imported
            == "bioseq.postgres.scheduler.dispatch_processor.typed_support.root_coordination") {
        return shape == "root";
    }
    if (imported ==
        "bioseq.postgres.scheduler.dispatch_processor.typed_support.source_provider_adapters") {
        return shape == "root";
    }
    if (imported == "bioseq.postgres.scheduler.dispatch_processor.typed_support.scanner_row") {
        return shape == "scanner_pg";
    }
    if (imported
        == "bioseq.postgres.scheduler.dispatch_processor.typed_support.policy_stats") {
        return shape == "root" || shape == "inmemory";
    }
    if (imported
        == "bioseq.postgres.scheduler.dispatch_processor.typed_support.pg_alignment_errors") {
        return shape == "inmemory_standard"
            || shape == "inmemory_chunked";
    }
    if (imported
        == "bioseq.postgres.scheduler.dispatch_processor.checked_alignment_calls") {
        return shape == "inmemory_segmented"
            || shape == "inmemory_chunked_segmented";
    }

    if (shape == "scanner_pg") return false;

    if (shape == "root") {
        return imported == "bioseq.postgres.support"
            || imported == "bioseq.sequences"
            || imported == "bioseq.pipeline.pipe"
            || imported == "bioseq.gpu.vulkan.runtime"
            || imported == "bioseq.postgres.types.compact_sequence"
            || imported == "bioseq.instrumentation.configuration"
            || imported == "bioseq.instrumentation.descriptor"
            || imported == "bioseq.instrumentation.profiler_state"
            || imported == "bioseq.instrumentation.region"
            || imported == "bioseq.instrumentation.runtime"
            || imported == "bioseq.storage.size"
            || imported == "bioseq.storage.layout"
            || imported == "bioseq.postgres.storage.scanner"
            || imported == "bioseq.parallelism.pool";
    }
    return true;
}

inline void render_imports(
    std::ostringstream& output,
    const generation_plan& plan,
    const bool output_primary,
    const bool api_partition,
    const materialization_coordinate* coordinate = nullptr) {
    std::set<std::string, std::less<>> emitted;
    if (output_primary) {
        output << "import "
               << plan.topology.region.configured_output_module_name << ";\n";
        emitted.insert(plan.topology.region.configured_output_module_name);
    }
    if (api_partition) output << "import :typed_flat_api;\n";
    for (const std::string& imported : neutral_imports(plan)) {
        if (coordinate != nullptr
            && !scheduler_body_import_required(
                plan, *coordinate, imported)) {
            continue;
        }
        if (emitted.insert(imported).second) {
            output << "import " << imported << ";\n";
        }
    }
    output << '\n';
}

/** Imports the module that owns the declarations copied into a probe. */
inline void render_signature_donor_import(
    std::ostringstream& output,
    const generation_plan& plan) {
    output << "import "
           << plan.topology.region.configured_output_module_name
           << ";\n\n";
}

[[nodiscard]] inline std::string provenance_attribute(
    const structural_analysis::declaration_record& declaration,
    const materialization_coordinate& coordinate) {
    return "[[bioseq::template_flatten_provenance(\""
        + escape_cpp_string_literal(declaration.source_identity) + "\", \""
        + std::to_string(coordinate.ordinal) + "\")]]";
}

[[nodiscard]] inline std::string provenance_marker(
    const structural_analysis::declaration_record& declaration,
    const materialization_coordinate& coordinate) {
    return provenance_attribute(declaration, coordinate) + "\n";
}

[[nodiscard]] inline bool append_inline_provenance(
    std::vector<source_edit>& edits,
    const structural_analysis::declaration_record& declaration,
    const structural_analysis::source_range& containing_range,
    const materialization_coordinate& coordinate) {
    if (declaration.kind != structural_analysis::declaration_kind::record
        && declaration.kind != structural_analysis::declaration_kind::enumeration
        && declaration.kind != structural_analysis::declaration_kind::alias) {
        return false;
    }
    if (!nonempty_range(declaration.name_range)
        || !contains(containing_range, declaration.name_range)) {
        throw std::runtime_error(
            "record/enum/alias probe name range is outside its declaration head");
    }
    const std::size_t insertion_offset =
        declaration.kind == structural_analysis::declaration_kind::alias
        ? declaration.name_range.end.byte_offset
        : declaration.name_range.begin.byte_offset;
    edits.push_back(source_edit{
        .begin_byte = insertion_offset,
        .end_byte = insertion_offset,
        .replacement = declaration.kind
                == structural_analysis::declaration_kind::alias
            ? " " + provenance_attribute(declaration, coordinate)
            : provenance_attribute(declaration, coordinate) + " ",
        .source_identity = declaration.source_identity,
        .purpose = "place record/enum/alias probe provenance",
    });
    return true;
}

[[nodiscard]] inline std::string render_binding_declaration(
    const generation_plan& plan,
    const binding_record& binding,
    const materialization_coordinate& coordinate,
    const bool synthetic_provenance) {
    const structural_analysis::source_range& range =
        declaration_range(binding.declaration);
    std::vector<source_edit> edits =
        policy_attribute_edits(binding.declaration, range);
    const bool inline_provenance = synthetic_provenance
        && append_inline_provenance(
            edits, binding.declaration, range, coordinate);
    edits.push_back(source_edit{
        .begin_byte = binding.declaration.body_or_initializer_range.begin.byte_offset,
        .end_byte = binding.declaration.body_or_initializer_range.end.byte_offset,
        .replacement = replacement_for(binding, coordinate).spelling,
        .source_identity = binding.declaration.source_identity,
        .purpose = "select binding case",
    });
    std::string rendered = apply_exact_edits(
        render_slice(plan.source_text, range, "binding declaration"),
        std::move(edits),
        range.begin.byte_offset);
    if (synthetic_provenance && !inline_provenance) {
        rendered = provenance_marker(binding.declaration, coordinate) + rendered;
    }
    if (!rendered.empty() && rendered.back() != '\n') rendered.push_back('\n');
    return rendered;
}

[[nodiscard]] inline std::string render_entry_head(
    const generation_plan& plan,
    const owned_declaration& entry,
    const materialization_coordinate& coordinate,
    const bool synthetic_provenance,
    std::string_view replacement_name = {}) {
    const structural_analysis::source_range& range =
        entry.declaration.declaration_head_range;
    std::vector<source_edit> edits =
        policy_attribute_edits(entry.declaration, range);
    const bool inline_provenance = synthetic_provenance
        && append_inline_provenance(
            edits, entry.declaration, range, coordinate);
    if (!replacement_name.empty()) {
        if (!contains(range, entry.declaration.name_range)) {
            throw std::runtime_error(
                "entry name range is outside its declaration head");
        }
        edits.push_back(source_edit{
            .begin_byte = entry.declaration.name_range.begin.byte_offset,
            .end_byte = entry.declaration.name_range.end.byte_offset,
            .replacement = std::string{replacement_name},
            .source_identity = entry.declaration.source_identity,
            .purpose = "selector entry name",
        });
    }
    std::string rendered = apply_exact_edits(
        render_slice(plan.source_text, range, "entry declaration head"),
        std::move(edits),
        range.begin.byte_offset);
    if (synthetic_provenance && !inline_provenance) {
        rendered = provenance_marker(entry.declaration, coordinate) + rendered;
    }
    return rendered;
}

[[nodiscard]] inline const materialization_coordinate& coordinate_at(
    const topology_plan& topology,
    const std::uint32_t ordinal) {
    if (ordinal >= topology.coordinates.size()
        || topology.coordinates[ordinal].ordinal != ordinal) {
        throw std::runtime_error(
            "materialization coordinate ordinals are not contiguous");
    }
    return topology.coordinates[ordinal];
}

[[nodiscard]] inline bool enabled(
    const generation_plan& plan,
    const std::uint32_t ordinal) {
    return std::ranges::find(plan.enabled_ordinals, ordinal)
        != plan.enabled_ordinals.end();
}

[[nodiscard]] inline const owned_declaration& root_entry(
    const generation_plan& plan) {
    const std::vector<owned_declaration>::const_iterator result =
        std::ranges::find_if(
        plan.topology.region.controlled_declarations,
        [](const owned_declaration& declaration) {
            return declaration.generated_api_entry
                && declaration.entry_role == "root";
        });
    if (result == plan.topology.region.controlled_declarations.end()) {
        throw std::runtime_error(
            "typed_flat_selector requires a template_flatten_entry role 'root'");
    }
    return *result;
}

[[nodiscard]] inline std::vector<const materialization_coordinate*>
enabled_root_identities(const generation_plan& plan) {
    const owned_declaration& root = root_entry(plan);
    std::map<std::string, const materialization_coordinate*, std::less<>> result;
    for (const std::uint32_t ordinal : plan.enabled_ordinals) {
        const materialization_coordinate& coordinate =
            coordinate_at(plan.topology, ordinal);
        if (coordinate_matches(coordinate, root.clauses)) {
            const std::pair<
                std::map<
                    std::string,
                    const materialization_coordinate*,
                    std::less<>>::iterator,
                bool> insertion =
                result.emplace(identity_key(coordinate), &coordinate);
            if (!insertion.second) {
                throw std::runtime_error(
                    "enabled identity has multiple root entry owners");
            }
        }
    }
    std::vector<const materialization_coordinate*> identities;
    identities.reserve(result.size());
    for (const std::pair<
             const std::string,
             const materialization_coordinate*>& entry : result) {
        identities.push_back(entry.second);
    }
    std::ranges::sort(
        identities,
        {},
        [](const materialization_coordinate* coordinate) {
            return coordinate->ordinal;
        });
    return identities;
}

[[nodiscard]] inline const axis_case_assignment& assignment_for(
    const materialization_coordinate& coordinate,
    std::string_view group) {
    const std::vector<axis_case_assignment>::const_iterator assignment =
        std::ranges::find(
        coordinate.assignments, group, &axis_case_assignment::group);
    if (assignment == coordinate.assignments.end()) {
        throw std::runtime_error(
            "coordinate is missing identity group '" + std::string{group} + "'");
    }
    return *assignment;
}

[[nodiscard]] inline std::string selector_expression(
    std::string_view group) {
    if (group == "alphabet") return "alphabet_code";
    if (group == "width") return "storage_unit_byte_width";
    return std::string{group} + "_code";
}

[[nodiscard]] inline std::string selector_case_spelling(
    const generation_plan& plan,
    const axis_group& group,
    const materialization_coordinate& coordinate) {
    if (group.binding_indices.empty()) {
        throw std::runtime_error("selector identity group has no binding");
    }
    const binding_record& binding =
        plan.topology.region.bindings.at(group.binding_indices.front());
    const axis_case_assignment& assignment =
        assignment_for(coordinate, group.name);
    if (binding.declaration.kind == structural_analysis::declaration_kind::alias) {
        return std::to_string(assignment.case_ordinal);
    }
    return replacement_for(binding, coordinate).spelling;
}

[[nodiscard]] inline std::string call_arguments(
    const owned_declaration& root) {
    if (root.declaration.parameters.empty()) {
        throw std::runtime_error(
            "root declaration has no structural parameter records; selector forwarding cannot be proven");
    }
    std::ostringstream output;
    for (std::size_t index = 0;
         index < root.declaration.parameters.size();
         ++index) {
        const structural_analysis::parameter_record& parameter =
            root.declaration.parameters[index];
        if (parameter.name.empty()) {
            throw std::runtime_error(
                "root entry parameters must be named for exact selector forwarding");
        }
        if (index != 0U) output << ", ";
        output << parameter.name;
    }
    return output.str();
}

inline void render_selector_switch(
    std::ostringstream& output,
    const generation_plan& plan,
    const owned_declaration& root,
    const std::vector<const axis_group*>& identity_groups,
    const std::vector<const materialization_coordinate*>& candidates,
    const std::size_t group_index,
    const std::size_t indent) {
    const std::string padding(indent, ' ');
    if (group_index == identity_groups.size()) {
        if (candidates.size() != 1U) {
            throw std::runtime_error(
                "selector terminal does not identify exactly one root coordinate");
        }
        const materialization_coordinate& coordinate = *candidates.front();
        const bool returns_void = root.declaration.return_type == "void";
        output << padding;
        if (!returns_void) output << "return ";
        output << "::" << coordinate.identity_namespace << "::"
               << root.declaration.name << '(' << call_arguments(root) << ");\n";
        if (returns_void) output << padding << "return;\n";
        return;
    }
    const axis_group& group = *identity_groups[group_index];
    output << padding << "switch (" << selector_expression(group.name)
           << ") {\n";
    for (const replacement_case& replacement : group.cases) {
        std::vector<const materialization_coordinate*> selected;
        for (const materialization_coordinate* coordinate : candidates) {
            if (assignment_for(*coordinate, group.name).case_label
                == replacement.label) {
                selected.push_back(coordinate);
            }
        }
        if (selected.empty()) continue;
        output << padding << "    case "
               << selector_case_spelling(plan, group, *selected.front())
               << ": {\n";
        render_selector_switch(
            output,
            plan,
            root,
            identity_groups,
            selected,
            group_index + 1U,
            indent + 8U);
        output << padding << "    }\n";
    }
    output << padding << "}\n";
}

[[nodiscard]] inline materialization_artifact make_artifact(
    materialization_artifact::kind kind,
    std::filesystem::path relative_path,
    std::string module_name,
    std::vector<std::uint32_t> coordinates,
    validation_class validation,
    std::string content) {
    if (relative_path.extension() != ".cppm") {
        throw std::runtime_error(
            "namespace materialization C++ output must use .cppm: "
            + relative_path.string());
    }
    if (!content.ends_with('\n')) content.push_back('\n');
    const std::string content_digest = bioseq::common::sha256_hex(content);
    return materialization_artifact{
        .artifact_kind = kind,
        .relative_path = std::move(relative_path),
        .module_name = std::move(module_name),
        .logical_coordinate_ordinals = std::move(coordinates),
        .validation = validation,
        .content = std::move(content),
        .content_sha256 = content_digest,
    };
}

}  // namespace detail

[[nodiscard]] std::string apply_exact_edits(
    std::string source,
    std::vector<source_edit> edits,
    const std::size_t source_origin) {
    std::ranges::sort(
        edits,
        [](const source_edit& left, const source_edit& right) {
            if (left.begin_byte != right.begin_byte) {
                return left.begin_byte < right.begin_byte;
            }
            return left.end_byte < right.end_byte;
        });
    const std::size_t source_end = source_origin + source.size();
    std::size_t previous_end = source_origin;
    for (const source_edit& edit : edits) {
        if (edit.begin_byte > edit.end_byte || edit.begin_byte < source_origin
            || edit.end_byte > source_end) {
            throw std::runtime_error(
                "exact source edit '" + edit.purpose
                + "' is outside its authenticated source slice");
        }
        if (edit.begin_byte < previous_end) {
            throw std::runtime_error(
                "overlapping exact source edits near provenance '"
                + edit.source_identity + "'");
        }
        previous_end = edit.end_byte;
    }
    for (std::vector<source_edit>::reverse_iterator iterator = edits.rbegin();
         iterator != edits.rend();
         ++iterator) {
        source.replace(
            iterator->begin_byte - source_origin,
            iterator->end_byte - iterator->begin_byte,
            iterator->replacement);
    }
    return source;
}

[[nodiscard]] inline std::string render_api_partition(
    const generation_plan& plan) {
    std::ostringstream output;
    output << detail::global_module_fragment(plan)
           << "// Generated by " << materialization_generator_identity << ".\n"
           << "module "
           << plan.topology.region.configured_output_module_name
           << ":typed_flat_api;\n\n";
    detail::render_signature_donor_import(output, plan);
    for (const std::uint32_t representative
         : plan.topology.identity_representative_ordinals) {
        const materialization_coordinate& coordinate =
            detail::coordinate_at(plan.topology, representative);
        output << "namespace " << coordinate.identity_namespace << " {\n\n";
        for (const binding_record& binding : plan.topology.region.bindings) {
            if (binding.role == axis_role::identity
                && binding.declaration.kind
                    == structural_analysis::declaration_kind::alias) {
                output << detail::render_binding_declaration(
                    plan, binding, coordinate, false)
                       << '\n';
            }
        }
        for (const owned_declaration& declaration
             : plan.topology.region.controlled_declarations) {
            if (!declaration.generated_api_entry) continue;
            output << detail::render_entry_head(
                plan, declaration, coordinate, false)
                   << ";\n\n";
        }
        output << "}  // namespace " << coordinate.identity_namespace << "\n\n";
    }
    return output.str();
}

[[nodiscard]] inline std::string render_implementation_unit(
    const generation_plan& plan,
    const materialization_coordinate& coordinate) {
    const structural_analysis::source_range& body_range =
        plan.topology.region.boundary.body_range;
    std::vector<source_edit> edits;
    for (const binding_record& binding : plan.topology.region.bindings) {
        const structural_analysis::source_range& range =
            detail::declaration_range(binding.declaration);
        if (plan.policy == emission_policy::selector_api
            && binding.role == axis_role::identity
            && binding.declaration.kind
                == structural_analysis::declaration_kind::alias) {
            edits.push_back(source_edit{
                .begin_byte = range.begin.byte_offset,
                .end_byte = range.end.byte_offset,
                .source_identity = binding.declaration.source_identity,
                .purpose = "remove API-owned identity alias",
            });
            continue;
        }
        std::vector<source_edit> binding_edits =
            detail::policy_attribute_edits(binding.declaration, range);
        edits.insert(
            edits.end(),
            std::make_move_iterator(binding_edits.begin()),
            std::make_move_iterator(binding_edits.end()));
        edits.push_back(source_edit{
            .begin_byte = binding.declaration.body_or_initializer_range.begin.byte_offset,
            .end_byte = binding.declaration.body_or_initializer_range.end.byte_offset,
            .replacement = replacement_for(binding, coordinate).spelling,
            .source_identity = binding.declaration.source_identity,
            .purpose = "select coordinate binding",
        });
    }
    for (const owned_declaration& declaration
         : plan.topology.region.controlled_declarations) {
        const structural_analysis::source_range& range =
            detail::declaration_range(declaration.declaration);
        if (!coordinate_matches(coordinate, declaration.clauses)) {
            edits.push_back(source_edit{
                .begin_byte = range.begin.byte_offset,
                .end_byte = range.end.byte_offset,
                .source_identity = declaration.declaration.source_identity,
                .purpose = "prune declaration for coordinate",
            });
            continue;
        }
        std::vector<source_edit> marker_edits =
            detail::policy_attribute_edits(declaration.declaration, range);
        edits.insert(
            edits.end(),
            std::make_move_iterator(marker_edits.begin()),
            std::make_move_iterator(marker_edits.end()));
    }
    std::string body = apply_exact_edits(
        detail::render_slice(
            plan.source_text, body_range, "materialization body"),
        std::move(edits),
        body_range.begin.byte_offset);
    if (body.contains("[[bioseq::template_flatten")) {
        throw std::runtime_error(
            "a template_flatten policy marker escaped into a production unit");
    }
    body = detail::rewrite_scheduler_root_std_qualifiers(
        plan, coordinate, std::move(body));
    body = detail::rewrite_scheduler_scanner_qualifiers(
        plan, coordinate, std::move(body));
    std::ostringstream output;
    const bool scheduler_implementation =
        plan.topology.region.configured_output_module_name
        == detail::scheduler_output_module;
    const bool scheduler_local_api_context =
        scheduler_implementation
        && plan.policy == emission_policy::selector_api;
    output << detail::scheduler_implementation_global_module_fragment(
                  plan, coordinate)
           << "module " << plan.topology.region.configured_output_module_name
           << ";\n\n";
    detail::render_imports(
        output,
        plan,
        false,
        plan.policy == emission_policy::selector_api
            && !scheduler_local_api_context,
        &coordinate);
    const std::string_view rendered_namespace =
        plan.policy == emission_policy::implementation_owners
        ? std::string_view{plan.topology.region.boundary.qualified_name}
        : std::string_view{coordinate.identity_namespace};
    output << "namespace " << rendered_namespace << " {\n";
    if (scheduler_local_api_context) {
        for (const binding_record& binding : plan.topology.region.bindings) {
            if (binding.role == axis_role::identity
                && binding.declaration.kind
                    == structural_analysis::declaration_kind::alias) {
                output << detail::render_binding_declaration(
                    plan, binding, coordinate, false)
                       << '\n';
            }
        }
        for (const owned_declaration& declaration : plan.topology.region.controlled_declarations) {
            if (!declaration.generated_api_entry ||
                coordinate_matches(coordinate, declaration.clauses) ||
                !detail::scheduler_sibling_declaration_required(plan, coordinate, declaration)) {
                continue;
            }
            std::string sibling_head =
                detail::render_entry_head(plan, declaration, coordinate, false);
            sibling_head = detail::rewrite_scheduler_root_std_qualifiers(
                plan, coordinate, std::move(sibling_head));
            output << sibling_head << ";\n\n";
        }
    }
    output << body;
    if (!body.empty() && body.back() != '\n')
        output << '\n';
    output << "}  // namespace " << rendered_namespace << "\n";
    return output.str();
}

[[nodiscard]] inline std::string render_declaration_probe(
    const generation_plan& plan,
    const materialization_coordinate& identity) {
    std::ostringstream output;
    if (plan.policy == emission_policy::implementation_owners) {
        output << detail::global_module_fragment(plan)
               << "module "
               << plan.topology.region.configured_output_module_name
               << ";\n\n";
        detail::render_imports(output, plan, false, false);
        output << "namespace "
               << plan.topology.region.boundary.qualified_name << " {\n\n";

        // Declare the identity bindings once for this probe. Shard bindings are
        // local to the file and let us copy the anonymous support namespace intact.
        for (const binding_record& binding : plan.topology.region.bindings) {
            output << detail::render_binding_declaration(
                plan, binding, identity, false)
                   << '\n';
        }
        if (plan.topology.region.support_island.has_value()) {
            const structural_analysis::source_range& support_range =
                plan.topology.region.support_island->declaration_range;
            output << detail::render_slice(
                plan.source_text,
                support_range,
                "implementation-owner support island");
            if (support_range.end.byte_offset > support_range.begin.byte_offset
                && plan.source_text[support_range.end.byte_offset - 1U]
                    != '\n') {
                output << '\n';
            }
            output << '\n';
        }

        const std::string identity_key = detail::identity_key(identity);
        std::size_t declaration_count = 0U;
        for (const materialization_coordinate& coordinate
             : plan.topology.coordinates) {
            if (detail::identity_key(coordinate) != identity_key) continue;
            for (const owned_declaration& declaration
                 : plan.topology.region.controlled_declarations) {
                if (!declaration.declaration.specifiers.is_definition
                    || declaration.declaration.linkage
                        == structural_analysis::entity_linkage::internal
                    || declaration.declaration.linkage
                        == structural_analysis::entity_linkage::none
                    || !coordinate_matches(
                        coordinate, declaration.clauses)) {
                    continue;
                }
                output << detail::render_entry_head(
                    plan, declaration, coordinate, true)
                       << ";\n\n";
                ++declaration_count;
            }
        }
        if (declaration_count == 0U) {
            throw std::runtime_error(
                "implementation-owner identity has no module-safe declaration probe heads");
        }
        output << "}  // namespace "
               << plan.topology.region.boundary.qualified_name << "\n";
        return output.str();
    }

    const std::string module =
        "bioseq.codegen.probes."
        + detail::module_identifier(plan.name) + ".t"
        + plan.topology.topology_digest.substr(0U, 12U) + ".identity_"
        + std::to_string(identity.ordinal);
    output << detail::global_module_fragment(plan)
           << "export module " << module << ";\n\n";
    // The probe imports the primary module to check declarations. Importing
    // every implementation dependency would repeat large system-header tables
    // and can exhaust Clang's source-location space before it parses a declaration.
    detail::render_signature_donor_import(output, plan);
    const std::string identity_key = detail::identity_key(identity);
    for (const materialization_coordinate& coordinate
         : plan.topology.coordinates) {
        if (detail::identity_key(coordinate) != identity_key) continue;
        output << "namespace " << coordinate.identity_namespace << " {\n"
               << "namespace bioseq_flatten_probe_" << coordinate.ordinal
               << " {\n\n";
        for (const binding_record& binding : plan.topology.region.bindings) {
            output << detail::render_binding_declaration(
                plan, binding, coordinate, true)
                   << '\n';
        }
        for (const owned_declaration& declaration
             : plan.topology.region.controlled_declarations) {
            if (!declaration.generated_api_entry
                || !coordinate_matches(coordinate, declaration.clauses)) {
                continue;
            }
            output << detail::render_entry_head(
                plan, declaration, coordinate, true)
                   << ";\n\n";
        }
        output << "}  // namespace bioseq_flatten_probe_"
               << coordinate.ordinal << "\n"
               << "}  // namespace " << coordinate.identity_namespace
               << "\n\n";
    }
    return output.str();
}

/// Generates the runtime selector in the validated specialization order.
[[nodiscard]] inline std::string render_selector_partition(
    const generation_plan& plan) {
    const owned_declaration& root = detail::root_entry(plan);
    const std::vector<const materialization_coordinate*> identities =
        detail::enabled_root_identities(plan);
    if (identities.empty()) {
        throw std::runtime_error(
            "selected profile has no enabled root identity coordinates");
    }
    std::vector<const axis_group*> identity_groups;
    for (const axis_group& group : plan.topology.groups) {
        if (group.role == axis_role::identity) identity_groups.push_back(&group);
    }
    if (identity_groups.empty()) {
        throw std::runtime_error(
            "typed_flat_selector requires at least one identity group");
    }
    const materialization_coordinate& representative = *identities.front();
    const bool scheduler_selector =
        plan.topology.region.configured_output_module_name
            == detail::scheduler_output_module
        && std::ranges::any_of(
            identity_groups,
            [](const axis_group* group) { return group->name == "alphabet"; })
        && std::ranges::any_of(
            identity_groups,
            [](const axis_group* group) { return group->name == "width"; });
    std::ostringstream output;
    output << detail::global_module_fragment(plan);
    if (scheduler_selector) {
        output << "#include <libassert/assert.hpp>\n\n";
    }
    output
           << "// Generated by " << materialization_generator_identity << ".\n"
           << "module "
           << plan.topology.region.configured_output_module_name
           << ":typed_flat_selector;\n\n";
    detail::render_signature_donor_import(output, plan);
    output << "import :typed_flat_api;\n";
    if (scheduler_selector) {
        output << "import bioseq.common.logging;\n";
    }
    output << '\n';
    const std::string owner = detail::parent_namespace(
        plan.topology.region.boundary.qualified_name);
    if (!owner.empty()) output << "namespace " << owner << " {\n\n";
    output << detail::render_entry_head(
                  plan,
                  root,
                  representative,
                  false,
                  "dispatch_process_sequences_typed")
           << "\n{\n";
    detail::render_selector_switch(
        output,
        plan,
        root,
        identity_groups,
        identities,
        0U,
        4U);
    if (scheduler_selector) {
        output << "    ASSERT(false, fmt::format(\n"
               << "        \"BatchProcessor::process_sequences invalid typmod alphabet={} width={}\",\n"
               << "        static_cast<unsigned>(alphabet_code),\n"
               << "        static_cast<unsigned>(storage_unit_byte_width)));\n";
        if (root.declaration.return_type == "void") {
            output << "    return;\n";
        } else {
            output << "    return {};\n";
        }
    } else {
        output << "    __builtin_unreachable();\n";
    }
    output << "}\n";
    if (!owner.empty()) output << "\n}  // namespace " << owner << "\n";
    return output.str();
}

/// Generates C++ files and optional manifests from a validated generation plan.
[[nodiscard]] inline generation_result render_generation(
    generation_plan plan,
    const bool render_manifests = true) {
    if (plan.policy != plan.topology.region.policy) {
        throw std::runtime_error(
            "generation-plan emission policy disagrees with authenticated topology");
    }
    validate_enabled_ordinals(
        plan.topology, plan.profile, plan.enabled_ordinals);
    validate_entry_ownership(plan.topology);
    generation_result result;
    if (plan.policy == emission_policy::selector_api) {
        result.artifacts.push_back(detail::make_artifact(
            materialization_artifact::kind::api_partition,
            "typed_flat_api.cppm",
            plan.topology.region.configured_output_module_name
                + ":typed_flat_api",
            plan.topology.identity_representative_ordinals,
            validation_class::body_free,
            render_api_partition(plan)));
        result.artifacts.push_back(detail::make_artifact(
            materialization_artifact::kind::selector_partition,
            "typed_flat_selector.cppm",
            plan.topology.region.configured_output_module_name
                + ":typed_flat_selector",
            plan.enabled_ordinals,
            validation_class::body_bearing,
            render_selector_partition(plan)));
    }
    if (plan.policy == emission_policy::selector_api || !render_manifests) {
        for (const std::uint32_t representative
             : plan.topology.identity_representative_ordinals) {
            const materialization_coordinate& identity =
                detail::coordinate_at(plan.topology, representative);
            std::vector<std::uint32_t> covered;
            const std::string key = detail::identity_key(identity);
            for (const materialization_coordinate& coordinate
                 : plan.topology.coordinates) {
                if (detail::identity_key(coordinate) == key) {
                    covered.push_back(coordinate.ordinal);
                }
            }
            result.artifacts.push_back(detail::make_artifact(
                materialization_artifact::kind::declaration_probe,
                std::filesystem::path{"pcm/declaration_probes"}
                    / ("identity_" + std::to_string(representative) + ".cppm"),
                plan.policy == emission_policy::implementation_owners
                    ? plan.topology.region.configured_output_module_name
                    : "bioseq.codegen.probes."
                        + detail::module_identifier(plan.name) + ".t"
                        + plan.topology.topology_digest.substr(0U, 12U)
                        + ".identity_" + std::to_string(representative),
                std::move(covered),
                validation_class::body_free,
                render_declaration_probe(plan, identity)));
        }
    }
    for (const std::uint32_t ordinal : plan.enabled_ordinals) {
        const materialization_coordinate& coordinate =
            detail::coordinate_at(plan.topology, ordinal);
        result.artifacts.push_back(detail::make_artifact(
            materialization_artifact::kind::implementation_unit,
            coordinate.relative_path,
            plan.topology.region.configured_output_module_name,
            {coordinate.ordinal},
            validation_class::body_bearing,
            render_implementation_unit(plan, coordinate)));
    }
    if (!render_manifests) {
        std::ranges::sort(
            result.artifacts,
            {},
            [](const materialization_artifact& artifact) {
                return artifact.relative_path.generic_string();
            });
        return result;
    }
    result.topology_manifest_json = render_topology_manifest(plan);
    result.artifacts.push_back(materialization_artifact{
        .artifact_kind = materialization_artifact::kind::manifest,
        .relative_path = "topology.json",
        .validation = validation_class::body_free,
        .content = result.topology_manifest_json,
        .content_sha256 = bioseq::common::sha256_hex(result.topology_manifest_json),
    });
    std::ranges::sort(
        result.artifacts,
        {},
        [](const materialization_artifact& artifact) {
            return artifact.relative_path.generic_string();
        });
    std::set<std::filesystem::path> paths;
    for (const materialization_artifact& artifact : result.artifacts) {
        if (!paths.insert(artifact.relative_path).second) {
            throw std::runtime_error(
                "duplicate namespace-materialization output path: "
                + artifact.relative_path.string());
        }
        if (artifact.content.contains("[[bioseq::template_flatten")
            && artifact.artifact_kind
                != materialization_artifact::kind::declaration_probe) {
            throw std::runtime_error(
                artifact.relative_path.string()
                + ": template_flatten marker escaped into production output");
        }
    }
    rendered_publication_manifest rendered_manifest = render_artifact_manifest(
        plan, result.artifacts, plan.previous_manifest_identity);
    result.artifact_manifest_json = std::move(rendered_manifest.json);
    result.manifest_identity = std::move(rendered_manifest.manifest_identity);
    return result;
}

}  // namespace bioseq::codegen::namespace_materialization
