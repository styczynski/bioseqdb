/*-------------------------------------------------------------------------
 *
 * index.cppm
 *
 * Module entry point for the bioseq columnar extension. Installs the
 * table-access-method, planner, storage, and executor hooks.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <bioseq/pg_compat.h>


extern "C" {
#include "miscadmin.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "catalog/namespace.h"
}

export module bioseq.postgres.column;

import bioseq.common.logging;
import bioseq.postgres.table;
import bioseq.postgres.table.am_cache;
import bioseq.postgres.storage.manager;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.column.planner_hook;
import bioseq.postgres.table.batch_sort_scan;

/// @brief Installs the Bioseq extension's table, planner, and executor hooks.
export void
initialize_bioseq_extension()
{
	bioseq_log<logging_level::debug>("[bioseq] pg: initialize_bioseq_extension() entered");
	if (process_shared_preload_libraries_in_progress) {
		bioseq::postgres::scheduler::install_scheduler_shmem_hooks();
		bioseq::postgres::scheduler::register_scheduler_supervisor();
	}
	bioseq::postgres::scheduler::install_pg_shader_provider_if_available();
	bioseq_tableam_init();
	// Custom-scan registration stays at the aggregate boundary so the table-AM
	// implementation does not depend on its custom-scan consumer.
	bioseq_customscan_init();
	install_column_planner_hooks();
	/* Outer custom scan that sorts sequences for `ORDER BY <bioseq.seq>`,
	 * replacing PostgreSQL's own Sort via a create_upper_paths_hook. */
	initialize_batch_sort_scan();
	/* Register relcache invalidation for the per-Relation BioseqAmCache slot.
	 * Postgres clears rd_amcache on a full relcache flush, but per-relid
	 * invalidations still need our callback so the small_vector heap
	 * spill is freed before the slot is dropped. */
	CacheRegisterRelcacheCallback(bioseq::storage::invalidate_table_access_cache,
								  PointerGetDatum(nullptr));
	/* Index residency callbacks install lazily before the first cache
	 * lookup/store. No entry exists before that point, and keeping registration
	 * out of `_PG_init` lets a later SQL transaction resume safely if only a
	 * prefix of the three callback leaves registered before an ERROR. */

	/* Orphan cleanup is intentionally SQL-triggered: module initialization has
	 * no active transaction, while the reaper requires SPI transaction state. */
}
