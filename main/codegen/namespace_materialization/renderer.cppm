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
import bioseq.codegen.clang_structural;
import bioseq.codegen.support;

export namespace bioseq::codegen::namespace_materialization {

[[nodiscard]] inline std::string apply_exact_edits(
    std::string source,
    std::vector<source_edit> edits,
    std::size_t source_origin = 0u);

namespace detail {

[[nodiscard]] inline bool nonempty_range(
    const clang_structural::source_range& range) noexcept {
    return range.end.byte_offset > range.begin.byte_offset;
}

[[nodiscard]] inline const clang_structural::source_range& declaration_range(
    const clang_structural::declaration_record& declaration) noexcept {
    return nonempty_range(declaration.declaration_range)
        ? declaration.declaration_range
        : declaration.range;
}

[[nodiscard]] inline const clang_structural::source_range& attribute_range(
    const clang_structural::attribute_record& attribute) noexcept {
    return nonempty_range(attribute.complete_range)
        ? attribute.complete_range
        : attribute.range;
}

[[nodiscard]] inline bool policy_attribute(
    std::string_view qualified_name) noexcept {
    return qualified_name == "bioseq::template_flatten"
        || qualified_name == "bioseq::template_flatten_when"
        || qualified_name == "bioseq::template_flatten_entry"
        || qualified_name == "bioseq::template_flatten_provenance";
}

[[nodiscard]] inline bool contains(
    const clang_structural::source_range& outer,
    const clang_structural::source_range& inner) noexcept {
    return outer.begin.byte_offset <= inner.begin.byte_offset
        && inner.end.byte_offset <= outer.end.byte_offset;
}

[[nodiscard]] inline std::string slice(
    std::string_view source,
    const clang_structural::source_range& range,
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
    const clang_structural::declaration_record& declaration,
    const clang_structural::source_range& containing_range) {
    std::vector<source_edit> edits;
    for (const clang_structural::attribute_record& attribute
         : declaration.attributes) {
        if (!policy_attribute(attribute.qualified_name)) continue;
        const clang_structural::source_range& range = attribute_range(attribute);
        if (!contains(containing_range, range)) continue;
        edits.push_back(source_edit{
            .begin_byte = range.begin.byte_offset,
            .end_byte = range.end.byte_offset,
            .provenance_id = declaration.provenance_id,
            .purpose = "strip policy attribute " + attribute.qualified_name,
        });
    }
    return edits;
}

[[nodiscard]] inline std::string module_identifier(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 1u);
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
        : std::string{value.substr(0u, separator)};
}

[[nodiscard]] inline std::string global_module_fragment(
    const generation_plan& plan) {
    const std::size_t end =
        plan.topology.region.source_module.declaration_range.begin.byte_offset;
    if (end > plan.source_text.size()) {
        throw std::runtime_error(
            "source module declaration range is outside the authored source");
    }
    std::string result = plan.source_text.substr(0u, end);
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

inline void render_imports(
    std::ostringstream& output,
    const generation_plan& plan,
    const bool output_primary,
    const bool api_partition) {
    std::set<std::string, std::less<>> emitted;
    if (output_primary) {
        output << "import "
               << plan.topology.region.configured_output_module_name << ";\n";
        emitted.insert(plan.topology.region.configured_output_module_name);
    }
    if (api_partition) output << "import :typed_flat_api;\n";
    for (const std::string& imported : neutral_imports(plan)) {
        if (emitted.insert(imported).second) {
            output << "import " << imported << ";\n";
        }
    }
    output << '\n';
}

[[nodiscard]] inline std::string provenance_attribute(
    const clang_structural::declaration_record& declaration,
    const materialization_coordinate& coordinate) {
    return "[[bioseq::template_flatten_provenance(\""
        + cpp_escape(declaration.provenance_id) + "\", \""
        + std::to_string(coordinate.ordinal) + "\")]]";
}

[[nodiscard]] inline std::string provenance_marker(
    const clang_structural::declaration_record& declaration,
    const materialization_coordinate& coordinate) {
    return provenance_attribute(declaration, coordinate) + "\n";
}

[[nodiscard]] inline bool append_inline_provenance(
    std::vector<source_edit>& edits,
    const clang_structural::declaration_record& declaration,
    const clang_structural::source_range& containing_range,
    const materialization_coordinate& coordinate) {
    if (declaration.kind != clang_structural::declaration_kind::record
        && declaration.kind != clang_structural::declaration_kind::enum_) {
        return false;
    }
    if (!nonempty_range(declaration.name_range)
        || !contains(containing_range, declaration.name_range)) {
        throw std::runtime_error(
            "record/enum probe name range is outside its declaration head");
    }
    edits.push_back(source_edit{
        .begin_byte = declaration.name_range.begin.byte_offset,
        .end_byte = declaration.name_range.begin.byte_offset,
        .replacement = provenance_attribute(declaration, coordinate) + " ",
        .provenance_id = declaration.provenance_id,
        .purpose = "place record/enum probe provenance",
    });
    return true;
}

[[nodiscard]] inline std::string render_binding_declaration(
    const generation_plan& plan,
    const binding_record& binding,
    const materialization_coordinate& coordinate,
    const bool synthetic_provenance) {
    const clang_structural::source_range& range =
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
        .provenance_id = binding.declaration.provenance_id,
        .purpose = "select binding case",
    });
    std::string rendered = apply_exact_edits(
        slice(plan.source_text, range, "binding declaration"),
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
    const clang_structural::source_range& range =
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
            .provenance_id = entry.declaration.provenance_id,
            .purpose = "selector entry name",
        });
    }
    std::string rendered = apply_exact_edits(
        slice(plan.source_text, range, "entry declaration head"),
        std::move(edits),
        range.begin.byte_offset);
    if (synthetic_provenance && !inline_provenance) {
        rendered = provenance_marker(entry.declaration, coordinate) + rendered;
    }
    return rendered;
}

[[nodiscard]] inline std::string identity_key(
    const materialization_coordinate& coordinate) {
    std::ostringstream result;
    for (const axis_case_assignment& assignment : coordinate.assignments) {
        if (assignment.role == axis_role::identity) {
            result << assignment.group << '=' << assignment.case_label << '\n';
        }
    }
    return result.str();
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
    const auto result = std::ranges::find_if(
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
            const auto [_, inserted] =
                result.emplace(identity_key(coordinate), &coordinate);
            if (!inserted) {
                throw std::runtime_error(
                    "enabled identity has multiple root entry owners");
            }
        }
    }
    std::vector<const materialization_coordinate*> identities;
    identities.reserve(result.size());
    for (const auto& [_, coordinate] : result) identities.push_back(coordinate);
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
    const auto assignment = std::ranges::find(
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
    if (binding.declaration.kind == clang_structural::declaration_kind::alias) {
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
        const clang_structural::parameter_record& parameter =
            root.declaration.parameters[index];
        if (parameter.name.empty()) {
            throw std::runtime_error(
                "root entry parameters must be named for exact selector forwarding");
        }
        if (index != 0u) output << ", ";
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
        if (candidates.size() != 1u) {
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
            group_index + 1u,
            indent + 8u);
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
    const std::string content_digest = sha256_hex(content);
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

[[nodiscard]] inline std::string apply_exact_edits(
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
                + edit.provenance_id + "'");
        }
        previous_end = edit.end_byte;
    }
    for (auto iterator = edits.rbegin(); iterator != edits.rend(); ++iterator) {
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
           << "// Generated by " << generator_version << ".\n"
           << "module "
           << plan.topology.region.configured_output_module_name
           << ":typed_flat_api;\n\n";
    detail::render_imports(output, plan, true, false);
    for (const std::uint32_t representative
         : plan.topology.identity_representative_ordinals) {
        const materialization_coordinate& coordinate =
            detail::coordinate_at(plan.topology, representative);
        output << "namespace " << coordinate.identity_namespace << " {\n\n";
        for (const binding_record& binding : plan.topology.region.bindings) {
            if (binding.role == axis_role::identity
                && binding.declaration.kind
                    == clang_structural::declaration_kind::alias) {
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
    const clang_structural::source_range& body_range =
        plan.topology.region.boundary.body_range;
    std::vector<source_edit> edits;
    for (const binding_record& binding : plan.topology.region.bindings) {
        const clang_structural::source_range& range =
            detail::declaration_range(binding.declaration);
        if (binding.role == axis_role::identity
            && binding.declaration.kind
                == clang_structural::declaration_kind::alias) {
            edits.push_back(source_edit{
                .begin_byte = range.begin.byte_offset,
                .end_byte = range.end.byte_offset,
                .provenance_id = binding.declaration.provenance_id,
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
            .provenance_id = binding.declaration.provenance_id,
            .purpose = "select coordinate binding",
        });
    }
    for (const owned_declaration& declaration
         : plan.topology.region.controlled_declarations) {
        const clang_structural::source_range& range =
            detail::declaration_range(declaration.declaration);
        if (!coordinate_matches(coordinate, declaration.clauses)) {
            edits.push_back(source_edit{
                .begin_byte = range.begin.byte_offset,
                .end_byte = range.end.byte_offset,
                .provenance_id = declaration.declaration.provenance_id,
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
        detail::slice(plan.source_text, body_range, "materialization body"),
        std::move(edits),
        body_range.begin.byte_offset);
    if (body.contains("[[bioseq::template_flatten")) {
        throw std::runtime_error(
            "a template_flatten policy marker escaped into a production unit");
    }
    std::ostringstream output;
    output << detail::global_module_fragment(plan)
           << "module " << plan.topology.region.configured_output_module_name
           << ";\n\n";
    detail::render_imports(output, plan, false, true);
    if (plan.topology.region.configured_output_module_name
        == "bioseq.scheduler.dispatch_processor") {
        output << "import :cpu_preparation_support;\n\n";
    }
    output << "namespace " << coordinate.identity_namespace << " {\n"
           << body;
    if (!body.empty() && body.back() != '\n') output << '\n';
    output << "}  // namespace " << coordinate.identity_namespace << "\n";
    return output.str();
}

[[nodiscard]] inline std::string render_declaration_probe(
    const generation_plan& plan,
    const materialization_coordinate& identity) {
    std::ostringstream output;
    const std::string module =
        "bioseq.codegen.probes."
        + detail::module_identifier(plan.name) + ".t"
        + plan.topology.topology_digest.substr(0u, 12u) + ".identity_"
        + std::to_string(identity.ordinal);
    output << detail::global_module_fragment(plan)
           << "export module " << module << ";\n\n";
    detail::render_imports(output, plan, true, false);
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
    std::ostringstream output;
    output << detail::global_module_fragment(plan)
           << "// Generated by " << generator_version << ".\n"
           << "module "
           << plan.topology.region.configured_output_module_name
           << ":typed_flat_selector;\n\n";
    detail::render_imports(output, plan, true, true);
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
        0u,
        4u);
    const bool scheduler_selector =
        plan.topology.region.configured_output_module_name
            == "bioseq.scheduler.dispatch_processor"
        && std::ranges::any_of(
            identity_groups,
            [](const axis_group* group) { return group->name == "alphabet"; })
        && std::ranges::any_of(
            identity_groups,
            [](const axis_group* group) { return group->name == "width"; });
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

[[nodiscard]] inline generation_result render_generation(
    generation_plan plan,
    const bool render_manifests = true) {
    validate_enabled_ordinals(
        plan.topology, plan.profile, plan.enabled_ordinals);
    validate_entry_ownership(plan.topology);
    generation_result result;
    result.artifacts.push_back(detail::make_artifact(
        materialization_artifact::kind::api_partition,
        "typed_flat_api.cppm",
        plan.topology.region.configured_output_module_name + ":typed_flat_api",
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
            "bioseq.codegen.probes." + detail::module_identifier(plan.name)
                + ".t" + plan.topology.topology_digest.substr(0u, 12u)
                + ".identity_" + std::to_string(representative),
            std::move(covered),
            validation_class::body_free,
            render_declaration_probe(plan, identity)));
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
        .content_sha256 = sha256_hex(result.topology_manifest_json),
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
    rendered_artifact_manifest rendered_manifest = render_artifact_manifest(
        plan, result.artifacts, plan.previous_manifest_identity);
    result.artifact_manifest_json = std::move(rendered_manifest.json);
    result.manifest_identity = std::move(rendered_manifest.manifest_identity);
    return result;
}

}  // namespace bioseq::codegen::namespace_materialization
