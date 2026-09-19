/**
 * @file
 * @brief Declares slice IR carrier construction without importing its implementation.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_slice_path_carrier_contracts;

import std;
import :custom_scan_slice_path_codec_contract;

/** @brief Add or reuse one producer in the slice IR. */
export [[nodiscard]] std::uint32_t slice_project_add_producer(
    slice_project_ir_builder& builder,
    Node* expression,
    bool distinct_consumer = false,
    bool force_value = false);

/** @brief Associate sequence source Vars with one stable producer. */
export void slice_project_note_source_binding(
    slice_project_ir_builder& builder,
    Node* sequence_expression,
    std::uint32_t producer_id);

/** @brief Build or reuse the IR node describing one expression. */
export [[nodiscard]] std::uint32_t slice_project_build_ir_node(
    slice_project_ir_builder& builder,
    Node* expression);

/** @brief Add one materialization boundary to the slice IR. */
export void slice_project_add_boundary(
    slice_project_ir_builder& builder,
    Node* sequence_expression,
    std::uint32_t producer_id,
    materialization_mechanism mechanism,
    materialization_reason reason,
    bool preprojection);

/** @brief Discover slice operations in a target and populate their IR. */
export [[nodiscard]] bool slice_project_discover_target(
    PathTarget* target,
    slice_project_ir_builder& builder);
