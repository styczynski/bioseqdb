/*-------------------------------------------------------------------------
 *
 * columnar.cppm
 *
 * Module entry point for the bioseq columnar extension. Installs the
 * table-access-method, planner, storage, and executor hooks.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"


extern "C" {
#include "miscadmin.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "catalog/namespace.h"
}

export module bioseq.column;

import bioseq.common.mlogging;
import bioseq.table;                      // bioseq_tableam_init, bioseq_customscan_init
import bioseq.table.am_cache;             // bioseq_am_cache_invalidate
import bioseq.storage.manager;
import bioseq.scheduler.supervisor_pg;
import bioseq.column.planner_hook;        // DOC0322 §9.4 — exported bioseq_planner_init
import bioseq.table.batch_sort_scan;      // exported bioseq_batch_sort_init (sequence ORDER BY)
#if BIOSEQ_EXTERNAL_PG
import bioseq.pg.external.relation;        // DOC0372 — bioseq_external_tableam_init
import bioseq.pg.external.scan;            // DOC0372 — bioseq_external_scan_init
#endif

/*
 * Install the extension's table access method and planner/executor hooks.
 */
export void
bioseq_init()
{
	bioseq_log<DBG>("[bioseq] pg: bioseq_init() entered");
	if (process_shared_preload_libraries_in_progress) {
		bioseq::parallelism::pg::install_scheduler_shmem_hooks();
		bioseq::parallelism::pg::register_scheduler_supervisor();
	}
	bioseq::parallelism::pg::install_pg_shader_provider_if_available();
	bioseq_tableam_init();
	// DOC0322 §7.5 — the customscan init call moved out of bioseq_tableam_init
	// to here so :access_method no longer imports :custom_scan (the bioseq_table
	// partition graph stays a DAG). bioseq_customscan_init is exported from
	// :custom_scan and imported directly here so the bioseq.table aggregate
	// does not need to re-export the heavy custom scan partition on GCC.
	bioseq_customscan_init();
#if BIOSEQ_EXTERNAL_PG
	/* DOC0372 — external logical relations. Register the external table access
	 * method's reloption kind and the external CustomScan (methods + path
	 * builder slot). Gated by BIOSEQ_EXTERNAL_PG, which is defined only when the
	 * provider stack is compiled into the extension; without it the always-owned
	 * path-builder slot stays null and the columnar hook falls through. */
	bioseq_external_tableam_init();
	bioseq_external_scan_init();
#endif
	bioseq_planner_init();
	/* Outer custom scan that sorts sequences for `ORDER BY <bioseq.seq>`,
	 * replacing PostgreSQL's own Sort via a create_upper_paths_hook. */
	bioseq_batch_sort_init();
	/* DOC0279 §12.1 — the legacy MetadataKindRegistry / sidecar kind
	 * registration is gone; the set of indexes is the compile-time
	 * pg_supported_indexes::list_types() registry (no runtime init). */

	/* DOC0260 §11.3 step 2 — register relcache invalidation callback
	 * for the per-Relation BioseqAmCache slot installed on rd_amcache.
	 * Postgres clears rd_amcache on a full relcache flush, but per-relid
	 * invalidations still need our callback so the small_vector heap
	 * spill is freed before the slot is dropped. */
	CacheRegisterRelcacheCallback(bioseq::storage::bioseq_am_cache_invalidate,
								  PointerGetDatum(NULL));
	/* Index residency callbacks install lazily before the first cache
	 * lookup/store. No entry exists before that point, and keeping registration
	 * out of `_PG_init` lets a later SQL transaction resume safely if only a
	 * prefix of the three callback leaves registered before an ERROR. */

	/* DOC0260 §11.3 step 3 — orphan reaper. We deliberately do NOT call
	 * `ReapOrphanSeqSubTables()` directly from `_PG_init` because there is
	 * no active transaction at module-load time and the reaper uses SPI
	 * (which requires `IsTransactionState() == true`). Operators run the
	 * sweep via the `bioseq.reap_orphan_seq_sub_tables()` SQL function
	 * declared in `bioseq--1.0.sql`. */
}
