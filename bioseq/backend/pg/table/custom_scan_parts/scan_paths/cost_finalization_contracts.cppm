/**
 * @file
 * @brief Declares column-scan read-cost estimates.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_scan_paths_cost_finalization_contracts;

/** @brief Apply parallel-worker adjustments to an existing Bioseq scan path. */
export void adjust_bioseq_column_parallel_scan_cost(Path* path);

/** @brief Estimates scan read cost from selected columns and filters that can skip stripes. */
export void cost_bioseq_column_scan(PlannerInfo* root,
                                    RelOptInfo* rel,
                                    Oid relation_id,
                                    CustomPath* cpath,
                                    int number_of_columns_read,
                                    int n_clauses);

/** @brief Estimate the per-stripe cost of reading the requested column count. */
export Cost bioseq_column_per_stripe_scan_cost(RelOptInfo* rel,
                                               Oid relation_id,
                                               int number_of_columns_read);

/** @brief Count physical stripes belonging to a Bioseq table. */
export uint64_t bioseq_column_table_stripe_count(Oid relation_id);
