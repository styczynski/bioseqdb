module;

#include <stdexcept>
#include <string>

export module bioseq.codegen.sql:policy;

import bioseq.codegen.sql.graph;

export namespace bioseq::codegen::sql {

/**
 * Enforce compiler-owned FMgr registration when complete code generation is
 * requested.
 */
inline void validate_codegen_completion_policy(
        const sql_graph& graph,
        bool require_complete_codegen) {
    if (!require_complete_codegen) return;

    if (!graph.handwritten_fmgr_export_owners.empty()) {
        throw std::runtime_error(
            "complete SQL codegen requires zero handwritten fmgr export "
            "owners; first remaining symbol is '"
            + graph.handwritten_fmgr_export_owners.front().symbol + "'");
    }
}

}  // namespace bioseq::codegen::sql
