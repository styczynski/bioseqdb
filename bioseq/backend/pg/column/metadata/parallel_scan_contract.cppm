/**
 * @file parallel_scan_contract.cppm
 * @brief Pointer-free PostgreSQL parallel column-scan ABI.
 */
module;

#include <bioseq/pg_compat.h>
#include <cstdint>

export module bioseq.postgres.storage.parallel_scan_contract;

/** @brief Pointer-free shared state for a parallel column scan. */
export struct parallel_column_scan_state
{
	pg_atomic_uint64 next_stripe_id; /* CAS cursor; claims may skip aborted-id gaps */
	/* The fixed instrumentation header precedes the snapshot flexible array. */
	std::uint32_t version;
	std::uint64_t counters_offset;
	std::uint32_t entry_count;
	std::uint32_t slot_count;
	std::uint64_t slot_stride;
	char snapshot_data[FLEXIBLE_ARRAY_MEMBER];
};

/** @brief Pointer handle to shared parallel-scan state with trailing snapshot storage. */
export using parallel_column_scan_handle = parallel_column_scan_state*;
