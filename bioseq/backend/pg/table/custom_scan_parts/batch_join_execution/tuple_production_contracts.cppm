/**
 * @file
 * @brief Produce output rows for batched sequence joins.
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

/** Fill one output column from an alignment match. */
export void batch_join_set_placeholder_value(
    TupleTableSlot* slot,
    const batch_join_placeholder_site& site,
    const bioseq::simd::match_result& match,
    const bioseq_stub_oids& oids,
    MemoryContext output_context);

/** Combine a target match with its query row, then filter and project the output. */
export TupleTableSlot* batch_join_project_output(
    CustomScanState* node,
    batch_join_runtime& runtime,
    const bioseq::scheduler::dispatch_next_output& output);
