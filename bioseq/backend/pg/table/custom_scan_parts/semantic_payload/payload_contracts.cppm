/**
 * @file
 * @brief Declares saved scan settings and their PostgreSQL plan encoding.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_semantic_payload_payload_contracts;

import std;
import bioseq.highway.dispatch_contract;
import bioseq.indexing.type;

/** @brief Version of the saved scan-settings format. */
export inline constexpr std::int32_t k_bioseq_semantic_payload_format_version = 1;
/** @brief Number of setting fields, excluding the three header entries. */
export inline constexpr std::int32_t k_bioseq_semantic_payload_field_count = 42;

/** @brief Holds alignment parameters and index policy for a scan-settings snapshot. */
export struct frozen_scan_semantics {
    bioseq::simd::minimap2_parameters params{};
    bool read_sequence_index = false;
    bioseq::indexing::index_read_requirement index_read_requirement =
        bioseq::indexing::index_read_requirement::allow;
};

/** @brief Writes a scan-settings snapshot as PostgreSQL-owned plan nodes. */
export [[nodiscard]] List* make_bioseq_semantic_payload(
    const frozen_scan_semantics& semantics);

/** @brief Reads the unique settings snapshot, returning false if its encoding is invalid. */
export [[nodiscard]] bool decode_bioseq_semantic_payload(
    const List* custom_private,
    frozen_scan_semantics* output) noexcept;
