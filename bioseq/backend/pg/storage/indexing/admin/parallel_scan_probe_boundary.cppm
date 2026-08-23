/**
 * @file admin/parallel_scan_probe_boundary.cppm
 * @brief Declares cold boundaries for hash-major inspection SQL diagnostics.
 */
module;

extern "C" {
#include "postgres.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <cstdint>
#include <string_view>

export module bioseq.postgres.storage.indexing.admin.parallel_scan_probe_boundary;

import bioseq.postgres.storage.indexing.admin;
import bioseq.postgres.storage.indexing.hash_major_parallel_probe;

export namespace bioseq::storage::indexing {

/** Resolve and authenticate one READY hash-major generation for a cold SQL diagnostic. */
[[nodiscard]] hash_major_minimizer_generation_inspection
inspect_hash_major_minimizer_generation_cold_boundary(Oid rel_oid,
                                                       std::string_view index_name,
                                                       std::uint64_t sequence_rowno);

/** Run the authenticated hash-major transport probe for a cold SQL diagnostic. */
[[nodiscard]] hash_major_minimizer_parallel_scan_stats
hash_major_minimizer_parallel_scan_probe_cold_boundary(Oid rel_oid,
                                                        std::string_view index_name,
                                                        std::uint64_t sequence_rowno,
                                                        std::uint32_t requested_workers);

} // namespace bioseq::storage::indexing
