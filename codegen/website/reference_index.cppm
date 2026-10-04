module;

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module bioseq.codegen.website:reference_index;

import :doxygen_reference;
import :cpp_reference_index;
import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Adds Doxygen descriptions to the public declarations found by  Clang.
// Clang determines which entries get reference pages. Each entry receives
// a stable ID, and the merged list is sorted by that ID.
namespace bioseq::codegen::website {

/// @brief Documentation totals for one C++ module or SQL schema.
struct reference_coverage_group {
    std::string group;              // C++ module/top namespace, or "sql:<schema>"
    std::size_t public_total{};
    std::size_t documented_total{};
};

/// @brief Missing documentation and totals for the public reference pages.
struct reference_coverage_report {
    std::size_t public_total{};
    std::size_t documented_total{};
    std::vector<std::string> missing_summary;    // optional public entries with no summary
    std::vector<std::string> missing_parameters; // functions with no parameter documentation
    std::vector<std::string> missing_returns;    // value-returning functions with no @return
    std::vector<std::string> fatal_missing;      // required or tutorial-linked entries with no summary
    std::vector<reference_coverage_group> groups;   // totals by C++ module or SQL schema
};

/// @brief Returns the entity-kind name used in both IDs and page routes.
[[nodiscard]] std::string reference_kind_token(entity_kind kind) {
    switch (kind) {
    case entity_kind::namespace_: return "namespace";
    case entity_kind::module: return "module";
    case entity_kind::class_: return "class";
    case entity_kind::struct_: return "struct";
    case entity_kind::union_: return "union";
    case entity_kind::enum_: return "enum";
    case entity_kind::enum_value: return "enumerator";
    case entity_kind::concept_: return "concept";
    case entity_kind::type_alias: return "type_alias";
    case entity_kind::function: return "function";
    case entity_kind::variable: return "variable";
    case entity_kind::sql_schema: return "schema";
    case entity_kind::sql_type: return "type";
    case entity_kind::sql_function: return "function";
    case entity_kind::sql_object: return "object";
    case entity_kind::overload_group: return "overload_group";
    }
    return "entity";
}

/// @brief Builds the stable ID for a  reference entry.
///
/// IDs use `<domain>:<kind>:<body>`. Functions use their normalized signature
/// so overloads get separate IDs. Other entries use the qualified name.
/// Leading :: is removed and whitespace is collapsed.
/// @param entity Entry with its kind, qualified name, and normalized signature
/// @return The stable ID used by links and reference pages.
[[nodiscard]] std::string make_reference_entity_id(const reference_entity& entity) {
    const std::string prefix = entity.domain == entity_domain::sql ? "sql:" : "cpp:";
    const bool signature_keyed = entity.kind == entity_kind::function
        || entity.kind == entity_kind::sql_function;
    std::string_view body = (signature_keyed && !entity.normalized_signature.empty())
        ? std::string_view(entity.normalized_signature)
        : std::string_view(entity.qualified_name);
    while (body.starts_with("::")) {
        body.remove_prefix(2);
    }
    std::string normalized;
    normalized.reserve(body.size());
    bool pending_space = false;
    for (const char character : body) {
        const bool is_space = character == ' ' || character == '\t'
            || character == '\n' || character == '\r'
            || character == '\f' || character == '\v';
        if (is_space) {
            if (!normalized.empty()) {
                pending_space = true;
            }
            continue;
        }
        if (pending_space) {
            normalized.push_back(' ');
            pending_space = false;
        }
        normalized.push_back(character);
    }
    return prefix + reference_kind_token(entity.kind) + ":" + normalized;
}

namespace {

// Compare argument counts because Clang and Doxygen can spell the same
// type differently. Name and kind narrow the mtch. Source locations
// provide another rule when that still leaves several overloads.
[[nodiscard]] bool same_parameter_arity(
    const reference_entity& lhs, const reference_entity& rhs) {
    return lhs.parameters.size() == rhs.parameters.size();
}

[[nodiscard]] bool within_two_lines(
    const reference_entity& lhs, const reference_entity& rhs) {
    if (!lhs.declaration || !rhs.declaration) {
        return false;
    }
    // Clang paths may be absolute while Doxygen paths are relative.
    // Match  the filename here, then allow a two-line difference below.
    if (lhs.declaration->path.filename() != rhs.declaration->path.filename()) {
        return false;
    }
    const std::size_t a = lhs.declaration->first_line;
    const std::size_t b = rhs.declaration->first_line;
    return (a > b ? a - b : b - a) <= 2;
}

// Copies Doxygen descriptions onto the declaration identified by, Clang.
void attach_reference_content(reference_entity& target, const reference_entity& source) {
    target.summary_text = source.summary_text;
    target.detail_content = source.detail_content;
    for (parameter_description& parameter : target.parameters) {
        for (const parameter_description& documented : source.parameters) {
            if (documented.name == parameter.name && !documented.description.empty()) {
                parameter.description = documented.description;
            }
        }
    }
    for (const auto& [key, value] : source.attributes) {
        target.attributes.emplace(key, value);
    }
}

}  // namespace

/// @brief Adds Doxygen documentation to the public declarations found by Clang.
///
/// Tries a unique signature match first, then qualified name, kind, and
/// argument count, then filename and a line difference of at most two.
/// Unused Doxygen entries with a public declaration's name cause an error.
/// @param structural Public declarations collected by Clang.
/// @param documented Documentation collected by Doxygen.
/// @return The  Clang entries with matched descriptions, sorted by stable ID.
[[nodiscard]] std::vector<reference_entity> merge_cpp_reference_entities(
    const cpp_reference_index& structural,
    const std::vector<reference_entity>& documented) {
    std::vector<reference_entity> merged = structural.entities;
    for (reference_entity& entity : merged) {
        if (entity.canonical_id.empty()) {
            entity.canonical_id = make_reference_entity_id(entity);
        }
    }
    std::vector<bool> clang_matched(merged.size(), false);
    std::vector<bool> doxygen_used(documented.size(), false);

    const auto apply_rule = [
                                &clang_matched,
                                &documented,
                                &doxygen_used,
                                &merged](auto predicate) -> void {
        for (std::size_t clang = 0; clang < merged.size(); ++clang) {
            if (clang_matched[clang]) {
                continue;
            }
            std::size_t found = documented.size();
            std::size_t count = 0;
            for (std::size_t doxygen = 0; doxygen < documented.size(); ++doxygen) {
                if (!doxygen_used[doxygen]
                    && predicate(merged[clang], documented[doxygen])) {
                    found = doxygen;
                    ++count;
                }
            }
            if (count == 1) {
                attach_reference_content(merged[clang], documented[found]);
                clang_matched[clang] = true;
                doxygen_used[found] = true;
            }
        }
    };

    apply_rule([](const reference_entity& clang, const reference_entity& doxygen) {
        return !doxygen.normalized_signature.empty()
            && doxygen.normalized_signature == clang.normalized_signature;
    });
    apply_rule([](const reference_entity& clang, const reference_entity& doxygen) {
        return doxygen.qualified_name == clang.qualified_name
            && doxygen.kind == clang.kind && same_parameter_arity(clang, doxygen);
    });
    apply_rule([](const reference_entity& clang, const reference_entity& doxygen) {
        return within_two_lines(clang, doxygen);
    });

    for (std::size_t doxygen = 0; doxygen < documented.size(); ++doxygen) {
        if (doxygen_used[doxygen]) {
            continue;
        }
        for (const reference_entity& clang : merged) {
            if (clang.qualified_name == documented[doxygen].qualified_name) {
                throw std::runtime_error(
                    "documentation/signature mismatch for exported entity "
                    + clang.qualified_name + " [clang: " + clang.normalized_signature
                    + "] vs [doxygen: " + documented[doxygen].normalized_signature + "]");
            }
        }
    }

    std::ranges::sort(merged, [](const reference_entity& lhs,
                                 const reference_entity& rhs) {
        return lhs.canonical_id < rhs.canonical_id;
    });
    return merged;
}

// Finds a Doxygen note of the requested kind in the content tree.
// Parameter notes use param:<direction>, so those need a prefix match.
// Return notes use the exact kind return.
[[nodiscard]] bool detail_has_admonition(
    const std::vector<content_node>& nodes, std::string_view kind, bool prefix) {
    for (const content_node& node : nodes) {
        if (node.kind == content_node_kind::admonition) {
            if (const auto found = node.attributes.find("kind");
                found != node.attributes.end()
                && (prefix ? std::string_view(found->second).starts_with(kind)
                           : std::string_view(found->second) == kind)) {
                return true;
            }
        }
        if (detail_has_admonition(node.children, kind, prefix)) {
            return true;
        }
    }
    return false;
}

// Group SQL entries by schema and C++ entries by module.
// A C++ entry without a module uses its outermost namespace.
[[nodiscard]] std::string coverage_group_of(const reference_entity& entity) {
    if (entity.domain == entity_domain::sql) {
        const std::size_t dot = entity.qualified_name.find('.');
        return "sql:" + (dot == std::string::npos ? std::string("public")
                                                   : entity.qualified_name.substr(0, dot));
    }
    if (entity.module_name && !entity.module_name->empty()) {
        return *entity.module_name;
    }
    const std::size_t sep = entity.qualified_name.find("::");
    return sep == std::string::npos ? entity.qualified_name
                                    : entity.qualified_name.substr(0, sep);
}

/// @brief Counts documented public entries and records missing descriptions.
/// @param entities Merged public reference entries.
/// @param required_ids IDs whose documentation is required by site settings.
/// @param referenced_ids IDs linked from tutorials.
/// @return Coverage ryport with missing required summaries in fatal_missing.
[[nodiscard]] reference_coverage_report evaluate_reference_coverage(
    const std::vector<reference_entity>& entities,
    const std::vector<std::string>& required_ids,
    const std::vector<std::string>& referenced_ids) {
    const auto listed = [](const std::vector<std::string>& ids, std::string_view id) {
        return std::ranges::find(ids, id) != ids.end();
    };
    reference_coverage_report report;
    for (const reference_entity& entity : entities) {
        ++report.public_total;
        const std::string group_key = coverage_group_of(entity);
        reference_coverage_group* group = nullptr;
        for (reference_coverage_group& candidate : report.groups) {
            if (candidate.group == group_key) {
                group = &candidate;
                break;
            }
        }
        if (group == nullptr) {
            report.groups.push_back(reference_coverage_group{group_key, 0, 0});
            group = &report.groups.back();
        }
        ++group->public_total;
        if (!entity.summary_text.empty()) {
            ++report.documented_total;
            ++group->documented_total;
        } else if (listed(required_ids, entity.canonical_id)
                   || listed(referenced_ids, entity.canonical_id)) {
            report.fatal_missing.push_back(entity.canonical_id);
        } else {
            report.missing_summary.push_back(entity.canonical_id);
        }
        // Check Doxygen parameter and return sections for C++ functions.
        if (entity.domain == entity_domain::cpp
            && entity.kind == entity_kind::function) {
            if (!entity.parameters.empty()
                && !detail_has_admonition(entity.detail_content, "param:", true)) {
                report.missing_parameters.push_back(entity.canonical_id);
            }
            if (entity.return_type && *entity.return_type != "void"
                && !detail_has_admonition(entity.detail_content, "return", false)) {
                report.missing_returns.push_back(entity.canonical_id);
            }
        }
    }
    std::ranges::sort(report.fatal_missing);
    std::ranges::sort(report.missing_summary);
    std::ranges::sort(report.missing_parameters);
    std::ranges::sort(report.missing_returns);
    std::ranges::sort(report.groups, [](const reference_coverage_group& lhs,
                                        const reference_coverage_group& rhs) {
        return lhs.group < rhs.group;
    });
    return report;
}

}  // namespace bioseq::codegen::website
