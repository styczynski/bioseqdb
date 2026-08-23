/**
 * @file
 * @brief Declares path-lineage predicates without importing traversal implementations.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_join_ownership_carrier_lineage_contracts;

/** @brief Couples an eligible base scan with its costing and locator targets. */
export struct batch_join_bioseq_path_candidate {
    CustomPath* scan = nullptr;
    Path* cost_owner = nullptr;
    const PathTarget* locator_target = nullptr;
};

/**
 * @brief Holds a synthetic base scan and projection wrapper for eligibility tests.
 *
 * Integration-only validation seam for the exact production eligibility guard.
 * SQL cannot reliably force an unrelated expression into a base-relation
 * ProjectionPath: PostgreSQL will normally retain it in an upper projection.
 * Construct the dangerous planner shapes directly and pass them through
 * find_unparameterized_bioseq_custom_path(), which is the production entry
 * point used by BioseqBatchJoinSetJoinPathlistHook().
 */
export struct batch_join_projection_guard_fixture {
    RelOptInfo* rel = nullptr;
    CustomPath* scan = nullptr;
    ProjectionPath* projection = nullptr;
};

/** @brief Builds an exact single-relation helper PHV for projection-guard tests. */
export [[nodiscard]] PlaceHolderVar* make_batch_join_projection_guard_phv(Oid helper_oid,
                                                                          Oid result_type,
                                                                          Index relid,
                                                                          Index phid);

/** @brief Builds a synthetic projection-wrapped Bioseq path for guard tests. */
export [[nodiscard]] batch_join_projection_guard_fixture
make_batch_join_projection_guard_fixture(Index relid,
                                         List* wrapper_delta,
                                         Cost wrapper_startup,
                                         Cost wrapper_total,
                                         Cardinality wrapper_rows);

/** @brief Check whether an expression exposes a locator-backed target sequence. */
export [[nodiscard]] bool batch_join_target_expression_is_forbidden(
    Node* expression,
    const Var* target_var,
    bool allow_mmsearch_qualifier);

/** @brief Check restrictions for forbidden locator-backed target evaluation. */
export [[nodiscard]] bool batch_join_target_restrictions_are_forbidden(
    List* restrictions,
    const Var* target_var,
    bool allow_mmsearch_qualifier);

/** @brief Recursively detect parallel execution below a planner path. */
export [[nodiscard]] bool batch_join_path_contains_parallel_execution(
    const Path* path) noexcept;

/** @brief Check whether a path target contains an expression equal to a candidate. */
export [[nodiscard]] bool path_target_contains_expression(
    const PathTarget* target,
    const Expr* expression) noexcept;

/** @brief Find the cheapest unparameterized Bioseq scan below safe wrappers. */
export [[nodiscard]] batch_join_bioseq_path_candidate
find_unparameterized_bioseq_custom_path(RelOptInfo* rel,
                                        AttrNumber target_seq_attno) noexcept;

/** @brief Append safe underlying target Vars required by a wrapper target. */
export [[nodiscard]] bool batch_join_append_underlying_target_vars(
    PathTarget* destination,
    const PathTarget* source,
    Index target_relid,
    AttrNumber target_seq_attno);
