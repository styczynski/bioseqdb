/**
 * @file utils.cppm
 * @brief Planner-expression utilities shared by Bioseq table scans.
 */
module;

#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:utils;

import bioseq.common.logging;
import bioseq.postgres.support;

/**
 * @brief Collects physical attributes required by a scan's projections and qualifiers.
 * @param executor_scan_state PostgreSQL scan executor state.
 * @param additional_expressions Optional expressions owned by a wrapping custom scan.
 * @return A PostgreSQL-owned zero-based attribute bitmap.
 * @throws PostgreSQL ERROR When an unsupported system attribute is referenced.
 */
export [[nodiscard]] Bitmapset* collect_needed_column_attributes(ScanState* executor_scan_state,
                                                                 List* additional_expressions) {
    TupleTableSlot* slot = executor_scan_state->ss_ScanTupleSlot;
    const int attribute_count = slot->tts_tupleDescriptor->natts;
    Plan* plan = executor_scan_state->ps.plan;
    constexpr int traversal_flags =
        PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
    List* variables =
        list_concat(pull_var_clause(reinterpret_cast<Node*>(plan->targetlist), traversal_flags),
                    pull_var_clause(reinterpret_cast<Node*>(plan->qual), traversal_flags));

    if (additional_expressions != nullptr) {
        variables = list_concat(
            variables,
            pull_var_clause(reinterpret_cast<Node*>(additional_expressions), traversal_flags));
    }

    bioseq::postgres::support::postgres_bitmapset attributes_needed;
    for (Var* variable : bioseq::postgres::support::postgres_list<Var>(variables)) {
        if (variable->varattno == SelfItemPointerAttributeNumber ||
            variable->varattno == TableOidAttributeNumber) {
            continue;
        }
        if (variable->varattno < SelfItemPointerAttributeNumber) {
            bioseq_report<logging_level::error, logging_target::postgres>()
                .with_sql_error_code(ERRCODE_FEATURE_NOT_SUPPORTED)
                .with_message("MIN / MAX TransactionID or CommandID not supported for BioseqScan");
        }

        if (variable->varattno == 0) {
            attributes_needed.add_range(0, attribute_count - 1);
            break;
        }
        attributes_needed.add(variable->varattno - 1);
    }
    return attributes_needed.release();
}
