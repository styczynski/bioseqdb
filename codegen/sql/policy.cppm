module;

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>

export module bioseq.codegen.sql:policy;

import bioseq.codegen.sql.graph;

export namespace bioseq::codegen::sql {

/**
 * Requires generated PostgreSQL function wrappers when complete code
 * generation is requested. The only allowed handwritten wrapper is
 * `bioseq_external_handler` in the external-data extension.
 */
inline void validate_codegen_completion_policy(
        const sql_graph& graph,
        bool require_complete_codegen) {
    if (!require_complete_codegen) return;

    if (graph.handwritten_fmgr_export_owners.empty()) return;
    const auto reviewed_external_owner = [](
            const handwritten_fmgr_export_owner& owner) {
        const std::string source_path =
            owner.source_path.lexically_normal().generic_string();
        return owner.macro_name == "POSTGRES_FN"
            && owner.symbol == "bioseq_external_handler"
            && owner.source_module
                == "bioseq.postgres.external.relation"
            && (source_path
                    == "bioseq/backend/pg/external/relation.cppm"
                || source_path.ends_with(
                    "/bioseq/backend/pg/external/relation.cppm"));
    };
    const auto unreviewed = std::ranges::find_if_not(
        graph.handwritten_fmgr_export_owners,
        reviewed_external_owner);
    if (graph.extension_name == "bioseq"
        && graph.handwritten_fmgr_export_owners.size() == 1U
        && unreviewed == graph.handwritten_fmgr_export_owners.end()) {
        return;
    }
    const handwritten_fmgr_export_owner& first_rejected =
        unreviewed == graph.handwritten_fmgr_export_owners.end()
        ? graph.handwritten_fmgr_export_owners.front()
        : *unreviewed;
    throw std::runtime_error(
        "complete SQL codegen permits only the reviewed external-relation "
        "handwritten fmgr export; first rejected symbol is '"
        + first_rejected.symbol + "'");
}

}  // namespace bioseq::codegen::sql
