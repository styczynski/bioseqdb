module;

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

export module bioseq.codegen.sql.renderer:model;

import bioseq.codegen.sql.collector;
import bioseq.codegen.sql.graph;

export namespace bioseq::codegen::sql {

/** Derive a linked symbol for a C routine. Distinct SQL overloads receive
 * separate wrappers; only the two existing audited arity dispatchers share one.
 * Ranking the complete stable identities makes output independent of discovery
 * order and avoids author-specified aliases or a runtime type dispatcher. */
[[nodiscard]] inline std::string generated_routine_c_symbol(
        const normalized_entity& entity,
        const sql_graph& graph) {
    const auto& name = entity.source.name;
    if (name == "from_array" || name == "mmsearch") return name;
    std::size_t overload_count = 0;
    std::size_t overload_index = 0;
    for (const auto& candidate : graph.entities) {
        if ((candidate.source.kind != entity_kind::function
             && candidate.source.kind != entity_kind::procedure)
            || candidate.source.name != name) continue;
        ++overload_count;
        if (candidate.stable_identity < entity.stable_identity)
            ++overload_index;
    }
    if (overload_count < 2) return name;
    const auto symbol = name + "_overload_" + std::to_string(overload_index);
    for (const auto& candidate : graph.entities) {
        if ((candidate.source.kind == entity_kind::function
             || candidate.source.kind == entity_kind::procedure)
            && candidate.source.name == symbol) {
            throw std::runtime_error(
                "derived fmgr symbol '" + symbol
                + "' conflicts with a C++ routine name");
        }
    }
    return symbol;
}

struct rendered_statement {
    std::string id;
    std::string entity_identity;
    std::string text;
    entity_kind statement_kind{entity_kind::phase};
};

struct rendered_sql_artifact {
    std::string sql;
    std::vector<rendered_statement> statements;
};

struct rendered_entity_handle {
    std::string entity_identity;
    std::string installed_identity;
    std::string leaf_name;
    std::string schema_name;
    std::string schema_binding;
    std::string expected_relkind;
    std::string variable_name;
};

struct rendered_entity_handle_module {
    std::string owner_key;
    std::string source_module;
    std::string module_name;
    std::string cpp_namespace;
    std::string content;
    std::vector<rendered_entity_handle> handles;
    bool profile_invariant{true};
};

struct rendered_row_codec_module {
    std::string owner_key;
    std::string source_module;
    std::string module_name;
    std::string cpp_namespace;
    std::string content;
    std::vector<std::string> row_cpp_names;
    bool profile_invariant{true};
};

}  // namespace bioseq::codegen::sql
