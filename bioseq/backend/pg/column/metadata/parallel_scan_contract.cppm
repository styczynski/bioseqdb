/**
 * @file parallel_scan_contract.cppm
 * @brief Shared-memory layout used by PostgreSQL parallel column scans.
 */
module;

#include <bioseq/pg_compat.h>
#include <cstdint>

export module bioseq.postgres.storage.parallel_scan_contract;

/** @brief Shared scan  progress and snapshot data. Store offsets so each worker
 * can access the state at its own shared-memory address. */
export struct parallel_column_scan_state
{
	pg_atomic_uint64 next_stripe_id; /* Atomically claim the next stripe, skipping gaps left by aborted writes. */
	/* Counter layout fields come before the saved snapshot bytes. */
	std::uint32_t version;
	std::uint64_t counters_offset;
	std::uint32_t entry_count;
	std::uint32_t slot_count;
	std::uint64_t slot_stride;
	char snapshot_data[FLEXIBLE_ARRAY_MEMBER];
};

/** @brief Process-local pointer to the shared scan state and saved snapshot. */
export using parallel_column_scan_handle = parallel_column_scan_state*;
