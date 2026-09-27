/**
 * @file
 * @brief Declares validation of the match-owner descriptors saved in a plan.
 */
module;
struct CustomScan;
struct List;

export module bioseq.postgres.table:custom_scan_match_analysis_match_owner_descriptor_contracts;

/** @brief Validates a saved match-owner descriptor and returns the matching plan-node ID. */
export [[nodiscard]] int validate_finalized_match_owner_descriptor_value(
    CustomScan* scan,
    List* descriptor);

/** @brief Reads and validates a match-owner descriptor from CustomScan private data. */
export [[nodiscard]] int validate_finalized_match_owner_descriptor(
    CustomScan* scan,
    int descriptor_index);
