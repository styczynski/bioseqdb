/**
 * @file minimizer_build_stats.cppm
 * @brief Statistics from the latest minimizer sequence-index build in this backend.
 */
module;

#include <cstdint>

export module bioseq.postgres.storage.indexing.minimizer_build_stats;

export namespace bioseq::storage::indexing {

/**
 * @brief Memory use, temporary files and merge work from the latest local index build.
 *
 * Index storage fills these counters. Administrative calls reset them, and SQL
 * diagnostics read the values from the same PostgreSQL backend process.
 */
struct last_minimizer_sequence_build_stats {
    std::uint64_t backend_pid = 0;
    std::uint64_t db_oid = 0;
    std::uint64_t relfilenode = 0;
    std::uint64_t sequence_rowno = 0;
    std::uint64_t total_run_count = 0;
    std::uint64_t resident_runs = 0;
    std::uint64_t file_runs = 0;
    std::uint64_t resident_run_bytes = 0;
    std::uint64_t resident_peak_bytes = 0;
    std::uint64_t spilled_run_bytes = 0;
    std::uint64_t postgres_temp_file_bytes = 0;
    std::uint64_t postgres_temp_file_count = 0;
    std::uint64_t merge_reader_buffer_bytes = 0;
    std::uint64_t merge_pass_count = 0;
    std::uint64_t spill_count_from_pressure = 0;
    std::uint64_t resident_demotions = 0;
    std::uint64_t position_spill_bytes = 0;
    std::uint64_t position_spill_count = 0;
};

/// Statistics from the current or most recent build in this PostgreSQL backend.
inline last_minimizer_sequence_build_stats
    g_last_minimizer_sequence_build_stats{};

} // namespace bioseq::storage::indexing
