/**
 * @file
 * @brief Declares the serialized BatchJoin cost ABI without planner implementation fanout.
 */
module;
struct Node;
using Cost = double;

export module bioseq.postgres.table:custom_scan_join_ownership_cost_contracts;

/** @brief Decode a finite planner Cost from the BatchJoin private-data ABI. */
export [[nodiscard]] Cost batch_join_cost_from_node(Node* node, const char* field);
