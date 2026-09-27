/**
 * @file
 * @brief Declares the graph that describes lazy sequence operations in a plan.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_slice_path_carrier_contracts;

import std;
import :custom_scan_slice_path_codec_contract;

/** @brief Adds an expression to the producer graph, reusing an eligible existing entry. */
export [[nodiscard]] std::uint32_t slice_project_add_producer(
    slice_project_ir_builder& builder,
    Node* expression,
    bool distinct_consumer = false,
    bool force_value = false);

/** @brief Records which producer uses each source column, marking shared sources ambiguous. */
export void slice_project_note_source_binding(
    slice_project_ir_builder& builder,
    Node* sequence_expression,
    std::uint32_t producer_id);

/** @brief Describes an expression and its sequence operations in the producer graph. */
export [[nodiscard]] std::uint32_t slice_project_build_ir_node(
    slice_project_ir_builder& builder,
    Node* expression);

/** @brief Records a planned materialization and the earlier work it depends on. */
export void slice_project_add_boundary(
    slice_project_ir_builder& builder,
    Node* sequence_expression,
    std::uint32_t producer_id,
    materialization_mechanism mechanism,
    materialization_reason reason,
    bool preprojection);

/** @brief Finds lazy sequence operations in output expressions and records their producers. */
export [[nodiscard]] bool slice_project_discover_target(
    PathTarget* target,
    slice_project_ir_builder& builder);
