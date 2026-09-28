/**
 * @file path_builder_slot.cppm
 * @brief Provides the optional planner callback for external relations.
 */
module;

#include <bioseq/pg_compat.h>

export module bioseq.postgres.external.path_builder_slot;

export namespace bioseq::postgres::external {

/**
 * @brief Planner callback that may replace a relation's paths.
 * @return `true` when the relation was recognized and its path list handled.
 */
using path_builder_callback =
	bool (*)(PlannerInfo *root, RelOptInfo *relation_info,
			 RangeTblEntry *range_table_entry, Relation relation);

} // namespace bioseq::postgres::external

namespace bioseq::postgres::external {

namespace {

/** @brief Process-local callback installed by the optional provider extension. */
path_builder_callback registered_path_builder = nullptr;

} // namespace

} // namespace bioseq::postgres::external

export namespace bioseq::postgres::external {

/**
 * @brief Installs the external table planner callback.
 *
 * Passing null clears the callback. Registering a non-null callback while
 * one is already installed raises an error Builds without external providers
 * leave this slot empty.
 */
void register_path_builder(path_builder_callback builder)
{
	if (builder != nullptr && registered_path_builder != nullptr)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("bioseq_external path builder is already registered")));
	}
	registered_path_builder = builder;
}

/**
 * @brief Invokes the registered path builder when one is available.
 * @return The builder's result, or `false` when external scanning is not
 * installed.
 */
[[nodiscard]] bool try_add_scan_paths(
	PlannerInfo *root, RelOptInfo *relation_info,
	RangeTblEntry *range_table_entry, Relation relation)
{
	if (registered_path_builder == nullptr)
	{
		return false;
	}
	return registered_path_builder(root, relation_info, range_table_entry,
								   relation);
}

} // namespace bioseq::postgres::external
