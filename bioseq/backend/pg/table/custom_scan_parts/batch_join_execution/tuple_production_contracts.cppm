/**
 * @file
 * @brief Declares batched-join tuple production without importing its implementation.
 */
module;
struct CustomScanState;
struct TupleTableSlot;
struct MemoryContextData;
using MemoryContext = MemoryContextData*;

export module bioseq.postgres.table:custom_scan_batch_join_execution_tuple_production_contracts;

import bioseq.pipeline.minimap_port.match_result_contract;
import bioseq.postgres.scheduler.dispatch_buffer.api;

import :custom_scan_batch_join_execution_runtime_state_contracts;
import :custom_scan_oid_expression_support_oid_catalog_core_contracts;

/** @brief Writes one match-derived placeholder into a combined output slot. */
export void batch_join_set_placeholder_value(
    TupleTableSlot* slot,
    const batch_join_placeholder_site& site,
    const bioseq::simd::match_result& match,
    const bioseq_stub_oids& oids,
    MemoryContext output_context);

/** @brief Combines one target match with its query row and evaluates the projection. */
export TupleTableSlot* batch_join_project_output(
    CustomScanState* node,
    batch_join_runtime& runtime,
    const bioseq::scheduler::dispatch_next_output& output);
