/**
 * @file
 * @brief Declares scan costing and filter selection for columnar paths.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_scan_paths_admission_contracts;

import bioseq.postgres.table.types;

/** @brief Remove matching paths from both complete and partial path lists. */
export void remove_paths_by_predicate(
    RelOptInfo* rel,
    path_predicate remove_path_predicate);

/** @brief Report whether a planner path is not an IndexPath. */
export bool is_not_index_path(Path* path);

/** @brief Create and cost a nonparallel sequential path for a columnar relation. */
export Path* create_bioseq_column_seq_scan_path(
    PlannerInfo* root,
    RelOptInfo* rel,
    Oid relation_id);

/** @brief Re-cost sequential and index paths admitted for a columnar relation. */
export void cost_bioseq_column_paths(
    PlannerInfo* root,
    RelOptInfo* rel,
    Oid relation_id);

/** @brief Returns candidate storage filters and mmsearch calls for this base relation. */
export List* filter_pushdown_clauses(
    PlannerInfo* root,
    RelOptInfo* rel,
    List* input_clauses);

/** @brief Collect join clauses that are eligible for base-relation pushdown. */
export List* find_pushdown_join_clauses(PlannerInfo* root, RelOptInfo* rel);

/** @brief Finds additional outer relations that could supply scan parameters. */
export Relids find_candidate_relids(
    PlannerInfo* root,
    RelOptInfo* rel,
    List* join_clauses);

/** @brief Limits the number of outer dependencies considered for each scan path */
export int choose_depth_limit(int n_candidates);
