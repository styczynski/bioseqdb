/**
 * @file
 * @brief Declares checks for finding a BioseqScan that can supply a BatchJoin target.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_join_ownership_carrier_lineage_contracts;

/** @brief A candidate BioseqScan, with its cost path and the output expressions its parent needs. */
export struct batch_join_bioseq_path_candidate {
    CustomPath* scan = nullptr;
    Path* cost_owner = nullptr;
    const PathTarget* locator_target = nullptr;
};

/**
 * @brief A test BioseqScan with a projection  above it.
 *
 * PostgreSQL usually keeps unrelated expressions in an upper projection, so SQL
 * cannot reliably produce the base projection shapes these tests need. Build
 * them directly and check them with find_unparameterized_bioseq_custom_path(),
 * the same function used by BioseqBatchJoinSetJoinPathlistHook().
 */
export struct batch_join_projection_guard_fixture {
    RelOptInfo* rel = nullptr;
    CustomPath* scan = nullptr;
    ProjectionPath* projection = nullptr;
};

/** @brief Builds a match helper inside a single-relation PlaceHolderVar for projection tests. */
export [[nodiscard]] PlaceHolderVar* make_batch_join_projection_guard_phv(Oid helper_oid,
                                                                          Oid result_type,
                                                                          Index relid,
                                                                          Index phid);

/** @brief Builds a test BioseqScan with extra expressions in a projection above it. */
export [[nodiscard]] batch_join_projection_guard_fixture
make_batch_join_projection_guard_fixture(Index relid,
                                         List* wrapper_delta,
                                         Cost wrapper_startup,
                                         Cost wrapper_total,
                                         Cardinality wrapper_rows);

/** @brief Checks whether an expression exposes the target sequence to ordinary SQL evaluation. */
export [[nodiscard]] bool batch_join_target_expression_is_forbidden(
    Node* expression,
    const Var* target_var,
    bool allow_mmsearch_qualifier);

/** @brief Rejects malformed restrictions and restrictions that  expose the target sequence to ordinary SQL evaluation. */
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

/** @brief Finds the cheapest serial BioseqScan that needs no outer-row parameters, allowing a safe projection above it. */
export [[nodiscard]] batch_join_bioseq_path_candidate
find_unparameterized_bioseq_custom_path(RelOptInfo* rel,
                                        AttrNumber target_seq_attno) noexcept;

/** @brief Adds target columns needed by a parent projection to the child output, rejecting unsafe references. */
export [[nodiscard]] bool batch_join_append_underlying_target_vars(
    PathTarget* destination,
    const PathTarget* source,
    Index target_relid,
    AttrNumber target_seq_attno);
