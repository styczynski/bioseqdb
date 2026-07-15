/*-------------------------------------------------------------------------
 *
 * external/external_path_builder_slot.cppm
 *
 * DOC0372 — "Planner-hook ownership". An ALWAYS-owned, dependency-light
 * module holding a single nullable callback so the existing static
 * columnar planner hook (BioseqColumnSetRelPathlistHook in
 * backend/pg/table/custom_scan.cppm) can reach the external-table path
 * builder only when the feature is compiled in.
 *
 * The whole point of this module is decoupling: it imports NO external /
 * provider module and NO table-AM module, so it links with ZERO external
 * symbols when BIOSEQ_BUILD_EXTERNAL_PG is OFF (the slot is simply never
 * registered, TryAddBioseqExternalScanPaths returns false, and the hook
 * falls through to the unchanged Bioseq columnar branch).
 *
 * BUILD NOTE (DOC0372 §"Exact option and module ownership changes",
 * items 7 & 8): this translation unit MUST be compiled into the
 * *unconditional* PostgreSQL target (`postgresql_bioseq`), NOT gated by
 * BIOSEQ_BUILD_EXTERNAL_PG, because the always-compiled custom-scan hook
 * calls TryAddBioseqExternalScanPaths(). The spec places the file text
 * under backend/pg/table/; it is scaffolded here under backend/pg/external/
 * at the task's direction — the integrator must special-case it out of the
 * "filter /backend/pg/external/ from the unconditional glob" rule so it
 * still lands in the unconditional target. Nothing else in this directory
 * is unconditional.
 *
 *-------------------------------------------------------------------------
 */
module;
#include "common.h"

export module bioseq.pg.external.path_builder_slot;

/*
 * DOC0372 §3814-3825 — signature of the external path builder. The real
 * implementation lives in backend/pg/external/planner.cppm (compiled only
 * under the feature option); it compares rd_tableam to
 * GetBioseqExternalTableAmRoutine(), validates the entity reloption, adds
 * the external CustomPath, and returns true. Returning false leaves the
 * existing Bioseq columnar branch to run unchanged.
 */
export using bioseq_external_try_add_paths_fn =
	bool (*)(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte,
			 Relation relation);

/*
 * RegisterBioseqExternalPathBuilder installs the external planner callback.
 * Called once from _PG_init after the external TableAM is initialized (only
 * when BIOSEQ_BUILD_EXTERNAL_PG is ON). Registering a non-null builder when
 * one is already installed is a programming error and is rejected; passing
 * nullptr clears the slot (used only by teardown/tests).
 */
export void RegisterBioseqExternalPathBuilder(bioseq_external_try_add_paths_fn builder);

/*
 * TryAddBioseqExternalScanPaths is called by BioseqColumnSetRelPathlistHook
 * after it invokes PreviousSetRelPathlistHook and opens the relation. When
 * the slot is null (feature OFF, or ON-but-not-yet-registered) it returns
 * false and the hook continues with the existing columnar branch. When the
 * slot is set it forwards to the registered builder and returns its verdict.
 */
export bool TryAddBioseqExternalScanPaths(PlannerInfo *root, RelOptInfo *rel,
										  RangeTblEntry *rte, Relation relation);

namespace {

/* The one and only function-pointer slot. Default null == feature not wired. */
bioseq_external_try_add_paths_fn g_bioseq_external_path_builder = nullptr;

} // namespace

void
RegisterBioseqExternalPathBuilder(bioseq_external_try_add_paths_fn builder)
{
	if (builder != nullptr && g_bioseq_external_path_builder != nullptr)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("bioseq_external path builder is already registered")));
	}
	g_bioseq_external_path_builder = builder;
}

bool
TryAddBioseqExternalScanPaths(PlannerInfo *root, RelOptInfo *rel,
							  RangeTblEntry *rte, Relation relation)
{
	if (g_bioseq_external_path_builder == nullptr)
	{
		return false;
	}
	return g_bioseq_external_path_builder(root, rel, rte, relation);
}
