/**
 * @file storage_adapter_conformance.cppm
 * @brief PostgreSQL worker entry point for the parallel index scan diagnostic.
 */
module;
#include <bioseq/pg_compat.h>

extern "C" {
#include "storage/dsm.h"
#include "storage/shm_toc.h"
}

export module bioseq.postgres.storage.indexing.chunked_storage:storage_adapter_conformance;
import bioseq.postgres.storage.indexing.hash_major_session_admin;
/**
 * @brief Run one worker of the parallel index scan diagnostic.
 * @param[in] segment PostgreSQL shared-memory segment attached to this worker.
 * @param[in] toc Table of contents used to find the shared scan state.
 */
extern "C" PGDLLEXPORT void bioseq_hash_major_parallel_scan_probe_worker_main(dsm_segment* segment,
                                                                              shm_toc* toc) {
    bioseq::storage::indexing::hash_major_minimizer_parallel_scan_probe_worker_main_pg(segment,
                                                                                       toc);
}
