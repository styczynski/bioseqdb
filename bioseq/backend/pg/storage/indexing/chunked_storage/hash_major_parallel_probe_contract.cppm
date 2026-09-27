/**
 * @file hash_major_parallel_probe_contract.cppm
 * @brief Results from checks of minimizer indexes grouped by hash.
 */
module;

#include <cstdint>

export module bioseq.postgres.storage.indexing.hash_major_parallel_probe_contract;

import bioseq.indexing.hash_major_minimizer_index;

export namespace bioseq::storage::indexing {
using bioseq::indexing::hash_major_minimizer_stream_kind;

/// Row count, byte count and checksum for one index stream read from the catalog.
struct hash_major_minimizer_catalog_stream_receipt {
    hash_major_minimizer_stream_kind kind = hash_major_minimizer_stream_kind::directory;
    std::uint64_t exact_catalog_rows = 0;
    std::uint64_t exact_catalog_bytes = 0;
    std::uint64_t exact_catalog_stream_crc64 = 0;
};

/// Row counts, checksums and timings for diagnostic scans.
/// Query readers do not use these results.
struct hash_major_minimizer_parallel_scan_stats {
    std::uint32_t requested_workers = 0;
    std::uint32_t launched_workers = 0;
    std::uint64_t generation_id = 0;
    std::uint64_t query_key = 0;
    std::uint64_t expected_rows = 0;
    std::uint64_t scanned_rows = 0;
    std::uint64_t expected_bytes = 0;
    std::uint64_t scanned_bytes = 0;
    std::uint64_t duplicate_rows = 0;
    std::uint64_t missing_rows = 0;
    std::uint64_t corrupt_rows = 0;
    std::uint64_t elapsed_ns = 0;
    std::uint64_t checksum_digest = 0;
    std::uint64_t receipt_digest = 0;
    std::uint64_t image_bytes = 0;
    std::uint64_t image_build_ns = 0;
    std::uint64_t mmap_elapsed_ns = 0;
    std::uint64_t mmap_scanned_rows = 0;
    std::uint64_t mmap_scanned_bytes = 0;
    std::uint64_t mmap_checksum_digest = 0;
    std::uint64_t mmap_receipt_digest = 0;
    bool has_leader_participation = false;
    bool mmap_advice_applied = false;
};

} // namespace bioseq::storage::indexing
