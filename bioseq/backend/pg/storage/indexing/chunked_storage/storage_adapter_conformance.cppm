/**
 * @file storage_adapter_conformance.cppm
 * @brief Compile-time layout conformance gates and parallel worker entry point.
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
 * @brief Runs the dynamic-shared-memory hash-major parallel scan probe worker.
 * @param[in] segment Attached PostgreSQL dynamic shared-memory segment.
 * @param[in] toc Shared table of contents containing the validated probe state.
 */
extern "C" PGDLLEXPORT void bioseq_hash_major_parallel_scan_probe_worker_main(dsm_segment* segment,
                                                                              shm_toc* toc) {
    bioseq::storage::indexing::hash_major_minimizer_parallel_scan_probe_worker_main_pg(segment,
                                                                                       toc);
}
