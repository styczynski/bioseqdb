/**
 * @file minimizer_build_stats.cppm
 * @brief Backend-local statistics shape for one minimizer-sequence index build.
 */
export module bioseq.postgres.storage.indexing.minimizer_build_stats;

import std;

export namespace bioseq::storage::indexing {

/**
 * @brief Captures retained, spilled, and merged runs from the latest local build.
 *
 * Storage updates the counters, administration resets them, and SQL reads them
 * from the same PostgreSQL backend process.
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

} // namespace bioseq::storage::indexing
