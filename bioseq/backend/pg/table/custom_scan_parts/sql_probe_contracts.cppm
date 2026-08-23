/**
 * @file
 * @brief Declares SQL-visible custom-scan diagnostic probes.
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
/** @brief Reports BatchJoin base ProjectionPath eligibility and wrapper-cost ownership decisions. */
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
/** @brief Reports match-helper ownership and copy-on-write propagation through Projection,
   Material, Memoize, Unique, and Append paths. */
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
/** @brief Reports whether carrier translation accepts exact setrefs offsets and slots and rejects
   attribute, alias, slot, or range mismatches. */
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
/** @brief Returns and optionally resets counters for match-owner finalization scopes across
   PostgreSQL errors. */
bioseq::postgres::support::named_output<
    bioseq::table::match_owner_finalization_scope_diagnostics_row>
    match_owner_finalization_scope_diagnostics(bool reset);

} // namespace bioseq::table::sql_routines
