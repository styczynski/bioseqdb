/**
 * @file
 * @brief Declares finalized match-owner descriptor validation without carrier fanout.
 */
module;
struct CustomScan;
struct List;

export module bioseq.postgres.table:custom_scan_match_analysis_match_owner_descriptor_contracts;

/** @brief Validate a finalized owner descriptor and return its stable plan-node id. */
export [[nodiscard]] int validate_finalized_match_owner_descriptor_value(
    CustomScan* scan,
    List* descriptor);

/** @brief Validate an owner descriptor stored at a CustomScan private-data index. */
export [[nodiscard]] int validate_finalized_match_owner_descriptor(
    CustomScan* scan,
    int descriptor_index);
