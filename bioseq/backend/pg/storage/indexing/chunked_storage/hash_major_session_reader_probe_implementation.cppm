/**
 * @file hash_major_session_reader_probe_implementation.cppm
 * @brief Run the parallel scan diagnostic using an open index reader
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

module bioseq.postgres.storage.indexing.hash_major_session_reader;

import bioseq.postgres.storage.indexing.hash_major_parallel_probe;

namespace bioseq::storage::indexing {

[[nodiscard]] hash_major_minimizer_parallel_scan_stats
index_storage_hash_major_minimizer_read_session::run_parallel_scan_probe(
    std::uint32_t requested_workers) {
        if (state_ != operation_state::source_authorized || active_scan_ != nullptr ||
            heap_relation_ == nullptr || !manifest_) {
            fail_("hash-major parallel scan probe is out of order");
        }
        return hash_major_minimizer_parallel_probe_detail::run_probe(
            heap_relation_, snapshot_, *manifest_, query_key_, requested_workers);
    }

} // namespace bioseq::storage::indexing
