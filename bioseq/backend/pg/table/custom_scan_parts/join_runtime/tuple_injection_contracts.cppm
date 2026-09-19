/**
 * @file
 * @brief Defines execution-time placeholder injection context without callback implementations.
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
 * @brief Carries placeholder and owner metadata through PostgreSQL's mutator callback.
 *
 * Passed to the execution-time injector with the exact scan-owned placeholder,
 * catalog, relation, and finalized match-owner identities.
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

/** @brief Applies execution-time placeholder injection to an expression tree. */
export [[nodiscard]] Node* var_injector(Node* node,
                                        var_injector_context* injector_context);

/** @brief Updates all installed placeholder Const nodes from one match result. */
export void populate_placeholder_consts(
    placeholder_constants& consts,
    scheduler_match_result_reference match);
