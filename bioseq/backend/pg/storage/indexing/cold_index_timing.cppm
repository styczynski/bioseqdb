/**
 * @file cold_index_timing.cppm
 * @brief Measure time and PostgreSQL buffer reads while opening an index
 */
module;

extern "C" {
#include "postgres.h"

#include "executor/instrument.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <cstdint>

export module bioseq.postgres.storage.indexing.cold_index_timing;

import bioseq.instrumentation.clock;
import bioseq.postgres.storage.indexing.minimizer_read_stats;

export namespace bioseq::storage::indexing {

/// Read the clock in nanoseconds for measuring elapsed time.
[[nodiscard]] inline std::uint64_t cold_index_now_ns() noexcept {
    return bioseq::instrumentation::clock::now_ns();
}

/// Add the elapsed time, treating a backward clock reading as zero.
inline void add_cold_index_elapsed(cold_index_preparation_stats& stats,
                                   std::uint64_t& destination,
                                   std::uint64_t begin_ns) noexcept {
    const std::uint64_t end_ns = cold_index_now_ns();
    stats.add(destination, end_ns >= begin_ns ? end_ns - begin_ns : 0);
}

/// Add shared-buffer hits and reads since the saved PostgreSQL counters.
inline void add_cold_index_buffer_usage(cold_index_preparation_stats& stats,
                                        const BufferUsage& begin) noexcept {
    BufferUsage delta{};
    BufferUsageAccumDiff(&delta, &pgBufferUsage, &begin);
    if (delta.shared_blks_hit < 0 || delta.shared_blks_read < 0) {
        stats.are_stats_saturated = true;
        return;
    }
    stats.add(stats.shared_blks_hit, static_cast<std::uint64_t>(delta.shared_blks_hit));
    stats.add(stats.shared_blks_read, static_cast<std::uint64_t>(delta.shared_blks_read));
}

} // namespace bioseq::storage::indexing
