module;

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.sql.physical_ledger;

import bioseq.codegen.sql.collector;
import bioseq.codegen.sql.graph;
import bioseq.codegen.support;

export namespace bioseq::codegen::sql {

struct physical_ledger_entry {
    std::string baseline_physical_id;
    std::size_t baseline_ordinal{};
    physical_statement_facet facet{
        physical_statement_facet::primary};
    std::string owner_identity;
};

struct physical_ledger_material {
    std::string exact_bytes;
    std::string sha256;
};

namespace physical_ledger_detail {

[[nodiscard]] inline std::string_view trim_line(
    std::string_view line) noexcept {
    while (!line.empty()
           && (line.front() == ' ' || line.front() == '\t'
               || line.front() == '\r')) {
        line.remove_prefix(1);
    }
    while (!line.empty()
           && (line.back() == ' ' || line.back() == '\t'
               || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    return line;
}

[[nodiscard]] inline physical_statement_facet parse_facet(
    std::string_view value,
    const std::filesystem::path& path,
    std::size_t line) {
    if (value == "primary") {
        return physical_statement_facet::primary;
    }
    if (value == "catalog_comment") {
        return physical_statement_facet::catalog_comment;
    }
    if (value == "base_type_shell") {
        return physical_statement_facet::base_type_shell;
    }
    if (value == "base_type_definition") {
        return physical_statement_facet::base_type_definition;
    }
    throw std::runtime_error(
        path.string() + ":" + std::to_string(line)
        + ": unknown physical-statement facet '" + std::string{value}
        + "'");
}

[[nodiscard]] inline entity_kind binding_kind(
    const normalized_entity& owner,
    physical_statement_facet facet) noexcept {
    return facet == physical_statement_facet::catalog_comment
        ? entity_kind::comment
        : owner.source.kind;
}

[[nodiscard]] inline std::map<
    std::string,
    const normalized_entity*,
    std::less<>>
owners_by_identity(const sql_graph& graph) {
    std::map<std::string, const normalized_entity*, std::less<>> result;
    for (const normalized_entity& entity : graph.entities) {
        if (!result.emplace(
                entity.stable_identity, &entity).second) {
            throw std::runtime_error(
                "physical ledger cannot index duplicate graph owner '"
                + entity.stable_identity + "'");
        }
    }
    return result;
}

}  // namespace physical_ledger_detail

[[nodiscard]] inline std::vector<physical_ledger_entry>
parse_physical_ledger(
    std::string_view content,
    const std::filesystem::path& path) {
    std::vector<physical_ledger_entry> result;
    std::size_t begin = 0;
    std::size_t line_number = 1;
    while (begin <= content.size()) {
        const std::size_t newline = content.find('\n', begin);
        const std::size_t end =
            newline == std::string::npos ? content.size() : newline;
        const std::string_view line =
            physical_ledger_detail::trim_line(
                std::string_view{content}.substr(begin, end - begin));
        if (!line.empty() && !line.starts_with('#')) {
            const std::size_t first = line.find('\t');
            const std::size_t second =
                first == std::string_view::npos
                ? std::string_view::npos
                : line.find('\t', first + 1);
            if (first == std::string_view::npos
                || second == std::string_view::npos
                || line.find('\t', second + 1)
                    != std::string_view::npos) {
                throw std::runtime_error(
                    path.string() + ":" + std::to_string(line_number)
                    + ": physical ledger rows require exactly three "
                      "tab-separated fields");
            }
            physical_ledger_entry entry{
                .baseline_physical_id =
                    std::string{line.substr(0, first)},
                .facet = physical_ledger_detail::parse_facet(
                    line.substr(first + 1, second - first - 1),
                    path,
                    line_number),
                .owner_identity =
                    std::string{line.substr(second + 1)},
            };
            entry.baseline_ordinal =
                parse_baseline_physical_ordinal(
                    entry.baseline_physical_id);
            const std::size_t expected = result.size() + 1;
            if (entry.baseline_ordinal != expected) {
                throw std::runtime_error(
                    path.string() + ":" + std::to_string(line_number)
                    + ": physical ledger must be contiguous from P001; "
                      "expected ordinal "
                    + std::to_string(expected));
            }
            if (entry.owner_identity.empty()) {
                throw std::runtime_error(
                    path.string() + ":" + std::to_string(line_number)
                    + ": physical ledger owner identity is empty");
            }
            result.push_back(std::move(entry));
        }
        if (newline == std::string::npos) break;
        begin = newline + 1;
        ++line_number;
    }
    if (result.empty()) {
        throw std::runtime_error(
            "physical ledger '" + path.string() + "' is empty");
    }
    return result;
}

/// Reads the immutable physical-order audit ledger. The format is one
/// tab-separated `PNNN`, facet, and natural stable identity per non-comment
/// line. It intentionally carries no SQL text or compatibility descriptor.
[[nodiscard]] inline std::vector<physical_ledger_entry>
read_physical_ledger(const std::filesystem::path& path) {
    const std::string content = read_file(path);
    return parse_physical_ledger(content, path);
}

/// Applies a non-owning order/provenance ledger to a natural SQL graph. During
/// migration it validates the reconciled transition graph byte-for-byte by
/// identity/facet. Once the transition owner is removed, it attaches only
/// natural-projection bindings sourced from the real C++ owners.
inline void apply_physical_ledger(
    sql_graph& graph,
    const std::vector<physical_ledger_entry>& ledger) {
    const auto owners =
        physical_ledger_detail::owners_by_identity(graph);
    const bool has_transition_ledger =
        !graph.expected_baseline_physical_statements.empty();

    std::map<std::string, std::pair<std::string, physical_statement_facet>,
             std::less<>>
        existing;
    if (has_transition_ledger) {
        for (const normalized_entity& owner : graph.entities) {
            for (const physical_statement_binding& binding :
                 effective_physical_statement_bindings(owner.source)) {
                if (!existing.emplace(
                        binding.baseline_physical_id,
                        std::pair{
                            owner.stable_identity,
                            binding.facet}).second) {
                    throw std::runtime_error(
                        "transition graph has duplicate physical ID '"
                        + binding.baseline_physical_id + "'");
                }
            }
        }
        if (existing.size() != ledger.size()) {
            throw std::runtime_error(
                "physical ledger row count "
                + std::to_string(ledger.size())
                + " differs from reconciled transition count "
                + std::to_string(existing.size()));
        }
        for (const physical_ledger_entry& entry : ledger) {
            const auto position =
                existing.find(entry.baseline_physical_id);
            if (position == existing.end()
                || position->second.first != entry.owner_identity
                || position->second.second != entry.facet) {
                throw std::runtime_error(
                    "physical ledger entry '"
                    + entry.baseline_physical_id
                    + "' disagrees with the reconciled natural graph");
            }
        }
        return;
    }

    graph.expected_baseline_physical_statements.clear();
    graph.expected_baseline_physical_statements.reserve(ledger.size());
    for (const physical_ledger_entry& entry : ledger) {
        const auto position = owners.find(entry.owner_identity);
        if (position == owners.end()) {
            throw std::runtime_error(
                "physical ledger entry '" + entry.baseline_physical_id
                + "' references missing natural owner '"
                + entry.owner_identity + "'");
        }
        normalized_entity& owner = *std::ranges::find(
            graph.entities,
            entry.owner_identity,
            &normalized_entity::stable_identity);
        const entity_kind kind =
            physical_ledger_detail::binding_kind(owner, entry.facet);
        physical_statement_binding binding{
            .kind = kind,
            .facet = entry.facet,
            .render_source =
                physical_render_source::natural_projection,
            .baseline_physical_id = entry.baseline_physical_id,
            .baseline_ordinal = entry.baseline_ordinal,
            .order_identity =
                "sql:physical-ledger:" + entry.baseline_physical_id,
            .source_module = owner.source.source_module,
            .source_path = owner.source.source_path,
            .source_sha256 = owner.source.source_sha256,
            .source_range = owner.source.source_range,
        };
        owner.source.physical_statements.push_back(binding);
        graph.expected_baseline_physical_statements.push_back(
            baseline_physical_statement{
                .baseline_physical_id =
                    entry.baseline_physical_id,
                .baseline_ordinal = entry.baseline_ordinal,
                .kind = kind,
                .order_identity = binding.order_identity,
                .source_module = binding.source_module,
                .source_path = binding.source_path,
                .source_sha256 = binding.source_sha256,
                .source_range = binding.source_range,
            });
    }
}

/// Serializes the graph's reconciled natural bindings as the non-owning
/// physical ledger that replaces the migration install owner.
[[nodiscard]] inline std::string render_physical_ledger(
    const sql_graph& graph) {
    struct owned_binding {
        const normalized_entity* owner;
        physical_statement_binding binding;
    };
    std::vector<owned_binding> bindings;
    for (const normalized_entity& owner : graph.entities) {
        for (physical_statement_binding binding :
             effective_physical_statement_bindings(owner.source)) {
            if (binding.baseline_physical_id.empty()) continue;
            if (binding.render_source
                != physical_render_source::natural_projection) {
                throw std::runtime_error(
                    "cannot publish physical ledger while '"
                    + binding.baseline_physical_id
                    + "' is not a natural projection");
            }
            bindings.push_back(owned_binding{
                .owner = &owner,
                .binding = std::move(binding),
            });
        }
    }
    std::ranges::sort(
        bindings,
        {},
        [](const owned_binding& value) {
            return value.binding.baseline_ordinal;
        });
    if (bindings.empty()) {
        throw std::runtime_error(
            "cannot publish an empty physical ledger");
    }
    std::set<std::string, std::less<>> identities;
    std::ostringstream output;
    output
        << "# bioseq SQL physical ledger v1\n"
        << "# baseline_physical_id\\tfacet\\tnatural_owner_identity\n";
    for (std::size_t index = 0; index < bindings.size(); ++index) {
        const owned_binding& value = bindings[index];
        const std::size_t expected = index + 1;
        if (value.binding.baseline_ordinal != expected
            || parse_baseline_physical_ordinal(
                   value.binding.baseline_physical_id)
                != expected) {
            throw std::runtime_error(
                "physical ledger is not contiguous at ordinal "
                + std::to_string(expected));
        }
        if (!identities.insert(
                value.binding.baseline_physical_id).second) {
            throw std::runtime_error(
                "physical ledger contains duplicate ID '"
                + value.binding.baseline_physical_id + "'");
        }
        output << value.binding.baseline_physical_id << '\t'
               << physical_statement_facet_name(
                      value.binding.facet)
               << '\t' << value.owner->stable_identity << '\n';
    }
    return output.str();
}

[[nodiscard]] inline physical_ledger_material
make_physical_ledger_material(std::string exact_bytes) {
    const std::string digest = sha256_hex(exact_bytes);
    return physical_ledger_material{
        .exact_bytes = std::move(exact_bytes),
        .sha256 = digest,
    };
}

/// Applies a checked ledger and proves that its exact bytes are the canonical
/// serialization of the resulting natural-owner bindings.
[[nodiscard]] inline physical_ledger_material
apply_checked_physical_ledger(
    sql_graph& graph,
    std::string exact_bytes,
    const std::filesystem::path& path) {
    apply_physical_ledger(
        graph,
        parse_physical_ledger(exact_bytes, path));
    const std::string canonical = render_physical_ledger(graph);
    if (canonical != exact_bytes) {
        throw std::runtime_error(
            "checked physical ledger '" + path.string()
            + "' is not in canonical generated form");
    }
    return make_physical_ledger_material(std::move(exact_bytes));
}

[[nodiscard]] inline physical_ledger_material
apply_checked_physical_ledger(
    sql_graph& graph,
    const std::filesystem::path& path) {
    return apply_checked_physical_ledger(
        graph,
        read_file(path),
        path);
}

}  // namespace bioseq::codegen::sql
