/**
 * @file
 * @brief Holds the writable constants used to project match results and symbol counts.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_runtime_placeholder_contracts;

import std;

export {
    /**
     * @brief Connects a symbol_count call to its result constant and source column
     *
     * Resolve  the letter's rank when the executor rewrites the target list. Use
     * that rank to read a stored count or count the sequence when needed.
     */
    struct symbol_count_call_site {
        Const* placeholder = nullptr;
        int rank = -1;
        char letter = '\0';
        int16_t column_attno = 0; // sequence column whose symbols are counted
        std::string column_name;  // column name used in rewrite diagnostics
    };

    /**
     * @brief Holds the writable Const nodes used in the scan's output expressions.
     *
     * The first scan call allocates these nodes and replaces bound helper calls
     * with them. Before projection, fill their values from the current match or
     * row. Each rewritten symbol_count call has its own site, so different letters
     * and columns can  be requested together.
     */
    struct placeholder_constants {
        Const* dp_score_node = nullptr; // bioseq.score / bioseq.dp_score
        Const* query_len_node = nullptr;
        Const* query_start_node = nullptr;
        Const* query_end_node = nullptr;
        Const* target_len_node = nullptr;
        Const* target_start_node = nullptr;
        Const* target_end_node = nullptr;
        Const* num_matches_node = nullptr;
        Const* block_length_node = nullptr;
        Const* edit_distance_node = nullptr;
        Const* mapq_node = nullptr;
        Const* chain_score_node = nullptr;
        Const* dp_max_score_node = nullptr;
        Const* seed_count_node = nullptr;
        Const* secondary_chain_score_node = nullptr;
        Const* is_reverse_node = nullptr;
        Const* is_primary_node = nullptr;
        Const* has_cigar_node = nullptr;
        Const* divergence_node = nullptr;
        Const* cigar_node = nullptr;
        std::vector<symbol_count_call_site> symbol_count_sites;
    };
}
