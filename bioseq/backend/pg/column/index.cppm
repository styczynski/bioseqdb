/**
 * @file index.cppm
 * @brief Initialize Bioseq's table access method, custom scans and PostgreSQL hooks.
 */
module;

#include <bioseq/pg_compat.h>

// Clang 22 reduced-BMI codegen needs allocation declarations reachable in the
// module that instantiates the imported logging template below.
#include <new>

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

/// @brief Register Bioseq's table access method, custom scans and planner hooks.
/// During shared-library preload, also register scheduler shared memory and
/// its supervisor process.
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
	// Register custom scans here to keep the table access method independent
	// of the scan implementations that use it.
	bioseq_customscan_init();
	install_column_planner_hooks();
	/* Register sequence sorting for ORDER BY bioseq.seq through the
	 * create_upper_paths_hook. */
	initialize_batch_sort_scan();
	/* Register the relation-cache callback. PostgreSQL detaches rd_amcache
  * before calling it. The next cache lookup rebuilds the metadata. */
	CacheRegisterRelcacheCallback(bioseq::storage::invalidate_table_access_cache,
								  PointerGetDatum(nullptr));
	/* Index-cache callbacks register on the first lookup or store. Each
	 * completed registration is remembered, so a later transaction can retry
	 * after an error without registering the same callback twice. */

	/* Orphan cleanup runs from SQL because it needs an active SPI transaction. */
}
