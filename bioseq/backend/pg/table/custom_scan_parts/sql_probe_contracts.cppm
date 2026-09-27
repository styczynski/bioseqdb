/**
 * @file
 * @brief Declares SQL tests of custom-scan planning and match-helper binding.
 */
export module bioseq.postgres.table:custom_scan_sql_probe_contracts;

import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.table.sql_rows;

export namespace bioseq::table::sql_routines {

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::volatile_,
    .parallel = bioseq::sql::parallel_safety::unsafe,
})]] [[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/** @brief Test which base projections BatchJoin accepts and which path supplies their costs. */
bioseq::postgres::support::named_output<bioseq::table::batch_join_projection_guard_probe_row>
    batch_join_projection_guard_probe();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::volatile_,
    .parallel = bioseq::sql::parallel_safety::unsafe,
})]] [[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Test match-helper propagation through copied planner paths.
 *
 * Checks Projection, Material, Memoize, Unique, and Append wrappers, including
 * unchanged originals and siblings. Also checks rejection of duplicate owners,
 * unknown wrappers, and hidden mmsearch filters.
 */
bioseq::postgres::support::named_output<bioseq::table::match_helper_wrapper_propagation_probe_row>
    match_helper_wrapper_propagation_probe();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::volatile_,
    .parallel = bioseq::sql::parallel_safety::unsafe,
})]] [[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/** @brief Test match-helper binding after setrefs changes table numbers and tuple-slot references. */
bioseq::postgres::support::named_output<bioseq::table::base_match_executor_translation_probe_row>
    base_match_executor_translation_probe();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::volatile_,
    .parallel = bioseq::sql::parallel_safety::unsafe,
})]] [[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Read match-owner finalization scope counters, optionally resetting them  first.
 *
 * Test builds use these counters to check cleanup after PostgreSQL errors.
 */
bioseq::postgres::support::named_output<
    bioseq::table::match_owner_finalization_scope_diagnostics_row>
    match_owner_finalization_scope_diagnostics(bool reset);

} // namespace bioseq::table::sql_routines
