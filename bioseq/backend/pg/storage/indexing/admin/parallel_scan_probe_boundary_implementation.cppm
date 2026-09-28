/**
 * @file admin/parallel_scan_probe_boundary_implementation.cppm
 * @brief Connect SQL diagnostics to hash-major inspection and parallel-read probes.
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
#include <new>
#include <string_view>

module bioseq.postgres.storage.indexing.admin.parallel_scan_probe_boundary;

import bioseq.postgres.storage.indexing.admin.parallel_scan_probe;

namespace bioseq::storage::indexing {

hash_major_minimizer_generation_inspection
inspect_hash_major_minimizer_generation_cold_boundary(const Oid rel_oid,
                                                       const std::string_view index_name,
                                                       const std::uint64_t sequence_rowno) {
    return inspect_hash_major_minimizer_generation(rel_oid, index_name, sequence_rowno);
}

hash_major_minimizer_parallel_scan_stats
hash_major_minimizer_parallel_scan_probe_cold_boundary(const Oid rel_oid,
                                                        const std::string_view index_name,
                                                        const std::uint64_t sequence_rowno,
                                                        const std::uint32_t requested_workers) {
    return hash_major_minimizer_parallel_scan_probe_impl(
        rel_oid, index_name, sequence_rowno, requested_workers);
}

} // namespace bioseq::storage::indexing
