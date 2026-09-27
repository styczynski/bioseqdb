/**
 * @file
 * @brief Declares source-column tagging for lazy sequence access.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_slice_rewrite_path_tagging_contracts;

import :custom_scan_slice_path_codec_contract;

/** @brief Tag BioseqScan sources in a path tree that the caller has already copied. */
export [[nodiscard]] Path* slice_project_tag_owned_path_tree(
    Path* path,
    const slice_project_ir_builder& builder,
    bool& tagged_source);

/** @brief Copy a BatchSort input path and tag its sequence source for lazy access. */
export [[nodiscard]] Path* batch_sort_tag_source_path(
    Path* path,
    Node* sort_expression,
    bool* tagged_out);
