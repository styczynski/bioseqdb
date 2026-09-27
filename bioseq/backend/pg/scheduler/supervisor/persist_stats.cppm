/**
 * @file scheduler/supervisor/persist_stats.cppm
 * @brief Report, asynchronous index builds and test their error cleanup.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "port/atomics.h"
}

#include <cstdint>

module bioseq.postgres.scheduler.supervisor;

import bioseq.postgres.storage.indexing.chunked_storage;

namespace bioseq::postgres::scheduler {

/** Initialize the shared counters for asynchronous index-build reports to zero. */
void initialize_minimizer_sequence_build_stats(pg_minimizer_sequence_build_stats& stats) {
    pg_atomic_init_u64(&stats.generation, 0);
    pg_atomic_init_u64(&stats.backend_pid, 0);
    pg_atomic_init_u64(&stats.db_oid, 0);
    pg_atomic_init_u64(&stats.relfilenode, 0);
    pg_atomic_init_u64(&stats.sequence_rowno, 0);
    pg_atomic_init_u64(&stats.total_run_count, 0);
    pg_atomic_init_u64(&stats.resident_runs, 0);
    pg_atomic_init_u64(&stats.file_runs, 0);
    pg_atomic_init_u64(&stats.resident_run_bytes, 0);
    pg_atomic_init_u64(&stats.resident_peak_bytes, 0);
    pg_atomic_init_u64(&stats.spilled_run_bytes, 0);
    pg_atomic_init_u64(&stats.postgres_temp_file_bytes, 0);
    pg_atomic_init_u64(&stats.postgres_temp_file_count, 0);
    pg_atomic_init_u64(&stats.merge_reader_buffer_bytes, 0);
    pg_atomic_init_u64(&stats.merge_pass_count, 0);
    pg_atomic_init_u64(&stats.spill_count_from_pressure, 0);
    pg_atomic_init_u64(&stats.resident_demotions, 0);
    pg_atomic_init_u64(&stats.position_spill_bytes, 0);
    pg_atomic_init_u64(&stats.position_spill_count, 0);
    pg_atomic_init_u64(&stats.worker_build_ns, 0);
    pg_atomic_init_u64(&stats.pipeline_mode, 0);
    pg_atomic_init_u64(&stats.max_concurrency, 0);
    pg_atomic_init_u64(&stats.index_memory_cap_mb, 0);
    pg_atomic_init_u64(&stats.shareable_lazy_workers, 0);
}

/** Initialize the worker's shared error counters and  failure selectors to zero. */
void initialize_async_minimizer_sequence_worker_boundary_stats(
    pg_async_minimizer_sequence_worker_boundary_stats& stats) {
    pg_atomic_init_u64(&stats.relation_leaf_calls, 0);
    pg_atomic_init_u64(&stats.spi_leaf_calls, 0);
    pg_atomic_init_u64(&stats.materialize_interrupt_leaf_calls, 0);
    pg_atomic_init_u64(&stats.materialize_scanner_read_leaf_calls, 0);
    pg_atomic_init_u64(&stats.pg_leaf_failures, 0);
    pg_atomic_init_u64(&stats.injected_pg_leaf_failures, 0);
    pg_atomic_init_u64(&stats.materialize_admissions, 0);
    pg_atomic_init_u64(&stats.materialize_failure_unwinds, 0);
    pg_atomic_init_u64(&stats.materialize_admission_restored, 0);
    pg_atomic_init_u64(&stats.materialize_admission_mismatches, 0);
    pg_atomic_init_u32(&stats.last_failure_point, 0);
    pg_atomic_init_u32(&stats.armed_failure_point, 0);
}

/** Bit marking a recorded worker failure as deliberately injected by a test */
inline constexpr std::uint32_t k_async_minimizer_sequence_worker_injected_failure_flag =
    std::uint32_t{1} << 31u;

#ifdef BIOSEQ_TEST_INDEXES
/** Remember which test failure this worker just raised. */
inline thread_local async_minimizer_sequence_worker_pg_failure
    g_async_minimizer_sequence_worker_injected_failure =
        async_minimizer_sequence_worker_pg_failure::none;
#endif

/**
 * Publish the completed index build's counters and worker settings in shared
 * memory. Do nothing when this backend has no shared scheduler state.
 * @param worker_build_ns Build duration in nanoseconds.
 * @param index_memory_cap_mb Worker memory cap in MiB.
 * @param has_shareable_lazy_worker Whether the source allows parallel reads.
 */
void publish_async_minimizer_sequence_build_stats(
    const bioseq::storage::indexing::last_minimizer_sequence_build_stats& stats,
    std::uint64_t worker_build_ns,
    std::uint64_t pipeline_mode,
    std::uint64_t max_concurrency,
    std::uint64_t index_memory_cap_mb,
    bool has_shareable_lazy_worker) noexcept {
    if (g_shared == nullptr) {
        return;
    }
    pg_minimizer_sequence_build_stats& shared_stats = g_shared->minimizer_sequence_build_stats;
    pg_atomic_write_u64(&shared_stats.backend_pid, stats.backend_pid);
    pg_atomic_write_u64(&shared_stats.db_oid, stats.db_oid);
    pg_atomic_write_u64(&shared_stats.relfilenode, stats.relfilenode);
    pg_atomic_write_u64(&shared_stats.sequence_rowno, stats.sequence_rowno);
    pg_atomic_write_u64(&shared_stats.total_run_count, stats.total_run_count);
    pg_atomic_write_u64(&shared_stats.resident_runs, stats.resident_runs);
    pg_atomic_write_u64(&shared_stats.file_runs, stats.file_runs);
    pg_atomic_write_u64(&shared_stats.resident_run_bytes, stats.resident_run_bytes);
    pg_atomic_write_u64(&shared_stats.resident_peak_bytes, stats.resident_peak_bytes);
    pg_atomic_write_u64(&shared_stats.spilled_run_bytes, stats.spilled_run_bytes);
    pg_atomic_write_u64(&shared_stats.postgres_temp_file_bytes, stats.postgres_temp_file_bytes);
    pg_atomic_write_u64(&shared_stats.postgres_temp_file_count, stats.postgres_temp_file_count);
    pg_atomic_write_u64(&shared_stats.merge_reader_buffer_bytes, stats.merge_reader_buffer_bytes);
    pg_atomic_write_u64(&shared_stats.merge_pass_count, stats.merge_pass_count);
    pg_atomic_write_u64(&shared_stats.spill_count_from_pressure, stats.spill_count_from_pressure);
    pg_atomic_write_u64(&shared_stats.resident_demotions, stats.resident_demotions);
    pg_atomic_write_u64(&shared_stats.position_spill_bytes, stats.position_spill_bytes);
    pg_atomic_write_u64(&shared_stats.position_spill_count, stats.position_spill_count);
    pg_atomic_write_u64(&shared_stats.worker_build_ns, worker_build_ns);
    pg_atomic_write_u64(&shared_stats.pipeline_mode, pipeline_mode);
    pg_atomic_write_u64(&shared_stats.max_concurrency, max_concurrency);
    pg_atomic_write_u64(&shared_stats.index_memory_cap_mb, index_memory_cap_mb);
    pg_atomic_write_u64(&shared_stats.shareable_lazy_workers, has_shareable_lazy_worker ? 1u : 0u);
    pg_atomic_fetch_add_u64(&shared_stats.generation, 1);
}

/** Read the shared build counters, or return zero values whei shared memory is absent. */
[[nodiscard]] async_minimizer_sequence_build_stats_snapshot
snapshot_async_minimizer_sequence_build_stats() {
    async_minimizer_sequence_build_stats_snapshot snapshot_result{};
    if (g_shared == nullptr) {
        return snapshot_result;
    }
    pg_minimizer_sequence_build_stats& shared_stats = g_shared->minimizer_sequence_build_stats;
    snapshot_result.generation = pg_atomic_read_u64(&shared_stats.generation);
    snapshot_result.stats.backend_pid = pg_atomic_read_u64(&shared_stats.backend_pid);
    snapshot_result.stats.db_oid = pg_atomic_read_u64(&shared_stats.db_oid);
    snapshot_result.stats.relfilenode = pg_atomic_read_u64(&shared_stats.relfilenode);
    snapshot_result.stats.sequence_rowno = pg_atomic_read_u64(&shared_stats.sequence_rowno);
    snapshot_result.stats.total_run_count = pg_atomic_read_u64(&shared_stats.total_run_count);
    snapshot_result.stats.resident_runs = pg_atomic_read_u64(&shared_stats.resident_runs);
    snapshot_result.stats.file_runs = pg_atomic_read_u64(&shared_stats.file_runs);
    snapshot_result.stats.resident_run_bytes = pg_atomic_read_u64(&shared_stats.resident_run_bytes);
    snapshot_result.stats.resident_peak_bytes =
        pg_atomic_read_u64(&shared_stats.resident_peak_bytes);
    snapshot_result.stats.spilled_run_bytes = pg_atomic_read_u64(&shared_stats.spilled_run_bytes);
    snapshot_result.stats.postgres_temp_file_bytes =
        pg_atomic_read_u64(&shared_stats.postgres_temp_file_bytes);
    snapshot_result.stats.postgres_temp_file_count =
        pg_atomic_read_u64(&shared_stats.postgres_temp_file_count);
    snapshot_result.stats.merge_reader_buffer_bytes =
        pg_atomic_read_u64(&shared_stats.merge_reader_buffer_bytes);
    snapshot_result.stats.merge_pass_count = pg_atomic_read_u64(&shared_stats.merge_pass_count);
    snapshot_result.stats.spill_count_from_pressure =
        pg_atomic_read_u64(&shared_stats.spill_count_from_pressure);
    snapshot_result.stats.resident_demotions = pg_atomic_read_u64(&shared_stats.resident_demotions);
    snapshot_result.stats.position_spill_bytes =
        pg_atomic_read_u64(&shared_stats.position_spill_bytes);
    snapshot_result.stats.position_spill_count =
        pg_atomic_read_u64(&shared_stats.position_spill_count);
    snapshot_result.worker_build_ns = pg_atomic_read_u64(&shared_stats.worker_build_ns);
    snapshot_result.pipeline_mode = pg_atomic_read_u64(&shared_stats.pipeline_mode);
    snapshot_result.max_concurrency = pg_atomic_read_u64(&shared_stats.max_concurrency);
    snapshot_result.index_memory_cap_mb = pg_atomic_read_u64(&shared_stats.index_memory_cap_mb);
    snapshot_result.shareable_lazy_workers =
        pg_atomic_read_u64(&shared_stats.shareable_lazy_workers);
    return snapshot_result;
}

/** Read shared worker error counters and the pending  test failure selector. */
[[nodiscard]] async_minimizer_sequence_worker_boundary_stats_snapshot
snapshot_async_minimizer_sequence_worker_boundary_stats() noexcept {
    async_minimizer_sequence_worker_boundary_stats_snapshot snapshot_result{};
    if (g_shared == nullptr) {
        return snapshot_result;
    }
    pg_async_minimizer_sequence_worker_boundary_stats& shared_stats =
        g_shared->async_minimizer_sequence_worker_boundary_stats;
    snapshot_result.relation_leaf_calls = pg_atomic_read_u64(&shared_stats.relation_leaf_calls);
    snapshot_result.spi_leaf_calls = pg_atomic_read_u64(&shared_stats.spi_leaf_calls);
    snapshot_result.materialize_interrupt_leaf_calls =
        pg_atomic_read_u64(&shared_stats.materialize_interrupt_leaf_calls);
    snapshot_result.materialize_scanner_read_leaf_calls =
        pg_atomic_read_u64(&shared_stats.materialize_scanner_read_leaf_calls);
    snapshot_result.pg_leaf_failures = pg_atomic_read_u64(&shared_stats.pg_leaf_failures);
    snapshot_result.injected_pg_leaf_failures =
        pg_atomic_read_u64(&shared_stats.injected_pg_leaf_failures);
    snapshot_result.materialize_admissions =
        pg_atomic_read_u64(&shared_stats.materialize_admissions);
    snapshot_result.materialize_failure_unwinds =
        pg_atomic_read_u64(&shared_stats.materialize_failure_unwinds);
    snapshot_result.materialize_admission_restored =
        pg_atomic_read_u64(&shared_stats.materialize_admission_restored);
    snapshot_result.materialize_admission_mismatches =
        pg_atomic_read_u64(&shared_stats.materialize_admission_mismatches);
    const std::uint32_t last_failure = pg_atomic_read_u32(&shared_stats.last_failure_point);
    snapshot_result.last_failure_point = static_cast<async_minimizer_sequence_worker_pg_failure>(
        last_failure & ~k_async_minimizer_sequence_worker_injected_failure_flag);
    snapshot_result.last_failure_injected =
        (last_failure & k_async_minimizer_sequence_worker_injected_failure_flag) != 0u;
    snapshot_result.armed_failure_point = static_cast<async_minimizer_sequence_worker_pg_failure>(
        pg_atomic_read_u32(&shared_stats.armed_failure_point));
    return snapshot_result;
}

/**
 * Select a worker operation to fail once in tests. Store the selector in shared
 * memory so the background worker can consume it. Return false when shared
 * state or test instrumentation is unavailable, or the selector is invalid.
 */
[[nodiscard]] bool arm_async_minimizer_sequence_worker_pg_failure(
    async_minimizer_sequence_worker_pg_failure point) noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    if (g_shared == nullptr || point == async_minimizer_sequence_worker_pg_failure::none ||
        point > async_minimizer_sequence_worker_pg_failure::materialize_scanner_read) {
        return false;
    }
    pg_atomic_write_u32(
        &g_shared->async_minimizer_sequence_worker_boundary_stats.armed_failure_point,
        static_cast<std::uint32_t>(point));
    return true;
#else
    (void)point;
    return false;
#endif
}

/** Cancel the worker's pending failure injection in shared memory. */
void disarm_async_minimizer_sequence_worker_pg_failure() noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    if (g_shared == nullptr) {
        return;
    }
    pg_atomic_write_u32(
        &g_shared->async_minimizer_sequence_worker_boundary_stats.armed_failure_point,
        static_cast<std::uint32_t>(async_minimizer_sequence_worker_pg_failure::none));
#endif
}

/** Return the diagnostic name of a worker failure point, or "invalid". */
[[nodiscard]] const char* async_minimizer_sequence_worker_failure_point_name(
    async_minimizer_sequence_worker_pg_failure point) noexcept {
    switch (point) {
    case async_minimizer_sequence_worker_pg_failure::none:
        return "none";
    case async_minimizer_sequence_worker_pg_failure::relation_open:
        return "relation_open";
    case async_minimizer_sequence_worker_pg_failure::relation_identity:
        return "relation_identity";
    case async_minimizer_sequence_worker_pg_failure::relation_close:
        return "relation_close";
    case async_minimizer_sequence_worker_pg_failure::spi_connect:
        return "spi_connect";
    case async_minimizer_sequence_worker_pg_failure::spi_execute:
        return "spi_execute";
    case async_minimizer_sequence_worker_pg_failure::spi_read:
        return "spi_read";
    case async_minimizer_sequence_worker_pg_failure::spi_finish:
        return "spi_finish";
    case async_minimizer_sequence_worker_pg_failure::materialize_error_log:
        return "materialize_error_log";
    case async_minimizer_sequence_worker_pg_failure::materialize_interrupt:
        return "materialize_interrupt";
    case async_minimizer_sequence_worker_pg_failure::materialize_scanner_read:
        return "materialize_scanner_read";
    }
    return "invalid";
}

/** Count a worker call to the selected PostgreSQL operation. */
void record_async_minimizer_sequence_worker_pg_leaf_call(
    async_minimizer_sequence_worker_pg_failure point) noexcept {
    if (g_shared == nullptr) {
        return;
    }
    pg_async_minimizer_sequence_worker_boundary_stats& stats =
        g_shared->async_minimizer_sequence_worker_boundary_stats;
    switch (point) {
    case async_minimizer_sequence_worker_pg_failure::relation_open:
    case async_minimizer_sequence_worker_pg_failure::relation_identity:
    case async_minimizer_sequence_worker_pg_failure::relation_close:
        pg_atomic_fetch_add_u64(&stats.relation_leaf_calls, 1);
        break;
    case async_minimizer_sequence_worker_pg_failure::spi_connect:
    case async_minimizer_sequence_worker_pg_failure::spi_execute:
    case async_minimizer_sequence_worker_pg_failure::spi_read:
    case async_minimizer_sequence_worker_pg_failure::spi_finish:
        pg_atomic_fetch_add_u64(&stats.spi_leaf_calls, 1);
        break;
    case async_minimizer_sequence_worker_pg_failure::materialize_interrupt:
        pg_atomic_fetch_add_u64(&stats.materialize_interrupt_leaf_calls, 1);
        break;
    case async_minimizer_sequence_worker_pg_failure::materialize_scanner_read:
        pg_atomic_fetch_add_u64(&stats.materialize_scanner_read_leaf_calls, 1);
        break;
    case async_minimizer_sequence_worker_pg_failure::none:
    case async_minimizer_sequence_worker_pg_failure::materialize_error_log:
        break;
    }
}

/** Record  where a PostgreSQL operation failed and whether a test caused it. */
void record_async_minimizer_sequence_worker_pg_leaf_failure(
    async_minimizer_sequence_worker_pg_failure point) noexcept {
    if (g_shared == nullptr) {
        return;
    }
    pg_async_minimizer_sequence_worker_boundary_stats& stats =
        g_shared->async_minimizer_sequence_worker_boundary_stats;
    bool injected = false;
#ifdef BIOSEQ_TEST_INDEXES
    if (g_async_minimizer_sequence_worker_injected_failure == point) {
        injected = true;
        g_async_minimizer_sequence_worker_injected_failure =
            async_minimizer_sequence_worker_pg_failure::none;
    }
#endif
    pg_atomic_fetch_add_u64(&stats.pg_leaf_failures, 1);
    const std::uint32_t encoded_failure =
        static_cast<std::uint32_t>(point) |
        (injected ? k_async_minimizer_sequence_worker_injected_failure_flag : 0u);
    pg_atomic_write_u32(&stats.last_failure_point, encoded_failure);
}

/**
 * Consume the matching shared test selector and raise a PostgreSQL error once.
 * @pre Call inside invoke_pg_as_cpp without C++ objects that need destruction
 * between the error and that bridge.
 */
void inject_async_minimizer_sequence_worker_pg_failure_leaf(
    async_minimizer_sequence_worker_pg_failure point) {
#ifdef BIOSEQ_TEST_INDEXES
    if (g_shared == nullptr) {
        return;
    }
    pg_async_minimizer_sequence_worker_boundary_stats& stats =
        g_shared->async_minimizer_sequence_worker_boundary_stats;
    std::uint32_t expected = static_cast<std::uint32_t>(point);
    if (!pg_atomic_compare_exchange_u32(
            &stats.armed_failure_point,
            &expected,
            static_cast<std::uint32_t>(async_minimizer_sequence_worker_pg_failure::none))) {
        return;
    }
    pg_atomic_fetch_add_u64(&stats.injected_pg_leaf_failures, 1);
    g_async_minimizer_sequence_worker_injected_failure = point;
    ereport(
        ERROR,
        (errcode(ERRCODE_INTERNAL_ERROR),
         errmsg("injected asynchronous minimizer-sequence worker PostgreSQL leaf failure"),
         errdetail("failure_point=%s", async_minimizer_sequence_worker_failure_point_name(point))));
#else
    (void)point;
#endif
}

/** Count a worker target copy that received a memory reservation. */
void record_async_minimizer_sequence_materialize_admission() noexcept {
    if (g_shared != nullptr) {
        pg_atomic_fetch_add_u64(
            &g_shared->async_minimizer_sequence_worker_boundary_stats.materialize_admissions, 1);
    }
}

/**
 * Count a failed target copy and whether cleanup restored its memory budget
 * @param admission_restored Reserved bytes equal their value before the copy.
 */
void record_async_minimizer_sequence_materialize_failure_unwind(bool admission_restored) noexcept {
    if (g_shared == nullptr) {
        return;
    }
    pg_async_minimizer_sequence_worker_boundary_stats& stats =
        g_shared->async_minimizer_sequence_worker_boundary_stats;
    pg_atomic_fetch_add_u64(&stats.materialize_failure_unwinds, 1);
    pg_atomic_fetch_add_u64(admission_restored ? &stats.materialize_admission_restored
                                               : &stats.materialize_admission_mismatches,
                            1);
}

} // namespace bioseq::postgres::scheduler
