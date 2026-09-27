/**
 * @file
 * @brief Declares slice path ownership tagging without importing its implementation.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_slice_rewrite_path_tagging_contracts;

import :custom_scan_slice_path_codec_contract;

/** @brief Tag the owned Bioseq source route in an already-private path tree. */
export [[nodiscard]] Path* slice_project_tag_owned_path_tree(
    Path* path,
    const slice_project_ir_builder& builder,
    bool& tagged_source);

/** @brief Clone and tag the Bioseq source route owned by one BatchSort path. */
export [[nodiscard]] Path* batch_sort_tag_source_path(
    Path* path,
    Node* sort_expression,
    bool* tagged_out);
