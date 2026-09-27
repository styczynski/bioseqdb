/**
 * @file
 * @brief Declares the reader for BatchJoin costs stored in custom_private.
 */
module;
struct Node;
using Cost = double;

export module bioseq.postgres.table:custom_scan_join_ownership_cost_contracts;

/** @brief Reads a finite BatchJoin cost from a float8 Const, reporting malformed values as errors. */
export [[nodiscard]] Cost batch_join_cost_from_node(Node* node, const char* field);
