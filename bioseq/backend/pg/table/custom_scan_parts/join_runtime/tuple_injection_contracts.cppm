/**
 * @file
 * @brief Declares the state and functions that fill scan expressions with match results.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_join_runtime_tuple_injection_contracts;

import bioseq.postgres.scheduler.dispatch_buffer.output_contract;
import :custom_scan_runtime_placeholder_contracts;
import :custom_scan_oid_expression_support_oid_catalog_core_contracts;

using scheduler_match_result_reference =
    decltype(*bioseq::scheduler::dispatch_next_output{}.match);

/**
 * @brief Identifies the match results that this scan may substitute into expressions.
 *
 * The expression walker uses the target expression and finalized helper bindings
 * to choose replacements. It records symbol-count requests beside the match
 * placeholders so the executor can fill both for each output row.
 */
export struct var_injector_context {
    placeholder_constants* consts;
    const bioseq_stub_oids* oids;
    Relation current_relation;
    Index match_sequence_varno;
    AttrNumber match_sequence_attno;
    Node* match_target_recipe;
    List* match_target_lineage;
    List* match_helper_sites;
    int stable_match_id;
};

/** @brief Replaces bound match helpers and supported symbol-count calls with writable Const nodes. */
export [[nodiscard]] Node* var_injector(Node* node,
                                        var_injector_context* injector_context);

/** @brief Fills the installed match placeholders from one alignment result. */
export void populate_placeholder_consts(
    placeholder_constants& consts,
    scheduler_match_result_reference match);
