/**
 * @file supervisor_pg_persist_stats_impl.cppm
 * @brief Asynchronous MPS persistence statistics and failure instrumentation.
 */
module;

#include "common.h"

extern "C" {
#include "port/atomics.h"
}

#include <cstdint>

module bioseq.postgres.scheduler.supervisor;

import bioseq.storage.indexing_pg.chunked_storage;

namespace bioseq::parallelism::pg {

void init_mps_build_stats(PgMpsBuildStats& stats) {
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
    pg_atomic_init_u64(&stats.lazy_worker_shareable, 0);
}

void init_async_mps_worker_boundary_stats(
        PgAsyncMpsWorkerBoundaryStats& stats) {
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
inline constexpr std::uint32_t kAsyncMpsWorkerInjectedFailureFlag =
    std::uint32_t{1} << 31u;

#ifdef BIOSEQ_TEST_INDEXES
inline thread_local AsyncMpsWorkerPgFailure
    g_async_mps_worker_injected_failure = AsyncMpsWorkerPgFailure::none;
#endif

void publish_async_mps_build_stats(
        const bioseq::storage::indexing::LastMpsBuildStats& stats,
        std::uint64_t worker_build_ns,
        std::uint64_t pipeline_mode,
        std::uint64_t max_concurrency,
        std::uint64_t index_memory_cap_mb,
        bool lazy_worker_shareable) noexcept {
    if (g_shared == nullptr) {
        return;
    }
    PgMpsBuildStats& dst = g_shared->mps_build_stats;
    pg_atomic_write_u64(&dst.backend_pid, stats.backend_pid);
    pg_atomic_write_u64(&dst.db_oid, stats.db_oid);
    pg_atomic_write_u64(&dst.relfilenode, stats.relfilenode);
    pg_atomic_write_u64(&dst.sequence_rowno, stats.sequence_rowno);
    pg_atomic_write_u64(&dst.total_run_count, stats.total_run_count);
    pg_atomic_write_u64(&dst.resident_runs, stats.resident_runs);
    pg_atomic_write_u64(&dst.file_runs, stats.file_runs);
    pg_atomic_write_u64(&dst.resident_run_bytes, stats.resident_run_bytes);
    pg_atomic_write_u64(&dst.resident_peak_bytes, stats.resident_peak_bytes);
    pg_atomic_write_u64(&dst.spilled_run_bytes, stats.spilled_run_bytes);
    pg_atomic_write_u64(&dst.postgres_temp_file_bytes, stats.postgres_temp_file_bytes);
    pg_atomic_write_u64(&dst.postgres_temp_file_count, stats.postgres_temp_file_count);
    pg_atomic_write_u64(&dst.merge_reader_buffer_bytes, stats.merge_reader_buffer_bytes);
    pg_atomic_write_u64(&dst.merge_pass_count, stats.merge_pass_count);
    pg_atomic_write_u64(&dst.spill_count_from_pressure, stats.spill_count_from_pressure);
    pg_atomic_write_u64(&dst.resident_demotions, stats.resident_demotions);
    pg_atomic_write_u64(&dst.position_spill_bytes, stats.position_spill_bytes);
    pg_atomic_write_u64(&dst.position_spill_count, stats.position_spill_count);
    pg_atomic_write_u64(&dst.worker_build_ns, worker_build_ns);
    pg_atomic_write_u64(&dst.pipeline_mode, pipeline_mode);
    pg_atomic_write_u64(&dst.max_concurrency, max_concurrency);
    pg_atomic_write_u64(&dst.index_memory_cap_mb, index_memory_cap_mb);
    pg_atomic_write_u64(&dst.lazy_worker_shareable, lazy_worker_shareable ? 1u : 0u);
    pg_atomic_fetch_add_u64(&dst.generation, 1);
}

[[nodiscard]] AsyncMpsBuildStatsSnapshot async_mps_build_stats_snapshot() {
    AsyncMpsBuildStatsSnapshot out{};
    if (g_shared == nullptr) {
        return out;
    }
    PgMpsBuildStats& src = g_shared->mps_build_stats;
    out.generation = pg_atomic_read_u64(&src.generation);
    out.stats.backend_pid = pg_atomic_read_u64(&src.backend_pid);
    out.stats.db_oid = pg_atomic_read_u64(&src.db_oid);
    out.stats.relfilenode = pg_atomic_read_u64(&src.relfilenode);
    out.stats.sequence_rowno = pg_atomic_read_u64(&src.sequence_rowno);
    out.stats.total_run_count = pg_atomic_read_u64(&src.total_run_count);
    out.stats.resident_runs = pg_atomic_read_u64(&src.resident_runs);
    out.stats.file_runs = pg_atomic_read_u64(&src.file_runs);
    out.stats.resident_run_bytes = pg_atomic_read_u64(&src.resident_run_bytes);
    out.stats.resident_peak_bytes = pg_atomic_read_u64(&src.resident_peak_bytes);
    out.stats.spilled_run_bytes = pg_atomic_read_u64(&src.spilled_run_bytes);
    out.stats.postgres_temp_file_bytes = pg_atomic_read_u64(&src.postgres_temp_file_bytes);
    out.stats.postgres_temp_file_count = pg_atomic_read_u64(&src.postgres_temp_file_count);
    out.stats.merge_reader_buffer_bytes = pg_atomic_read_u64(&src.merge_reader_buffer_bytes);
    out.stats.merge_pass_count = pg_atomic_read_u64(&src.merge_pass_count);
    out.stats.spill_count_from_pressure = pg_atomic_read_u64(&src.spill_count_from_pressure);
    out.stats.resident_demotions = pg_atomic_read_u64(&src.resident_demotions);
    out.stats.position_spill_bytes = pg_atomic_read_u64(&src.position_spill_bytes);
    out.stats.position_spill_count = pg_atomic_read_u64(&src.position_spill_count);
    out.worker_build_ns = pg_atomic_read_u64(&src.worker_build_ns);
    out.pipeline_mode = pg_atomic_read_u64(&src.pipeline_mode);
    out.max_concurrency = pg_atomic_read_u64(&src.max_concurrency);
    out.index_memory_cap_mb = pg_atomic_read_u64(&src.index_memory_cap_mb);
    out.lazy_worker_shareable = pg_atomic_read_u64(&src.lazy_worker_shareable);
    return out;
}

[[nodiscard]] AsyncMpsWorkerBoundaryStatsSnapshot
async_mps_worker_boundary_stats_snapshot() noexcept {
    AsyncMpsWorkerBoundaryStatsSnapshot out{};
    if (g_shared == nullptr) {
        return out;
    }
    PgAsyncMpsWorkerBoundaryStats& src =
        g_shared->async_mps_worker_boundary_stats;
    out.relation_leaf_calls =
        pg_atomic_read_u64(&src.relation_leaf_calls);
    out.spi_leaf_calls = pg_atomic_read_u64(&src.spi_leaf_calls);
    out.materialize_interrupt_leaf_calls =
        pg_atomic_read_u64(&src.materialize_interrupt_leaf_calls);
    out.materialize_scanner_read_leaf_calls =
        pg_atomic_read_u64(&src.materialize_scanner_read_leaf_calls);
    out.pg_leaf_failures = pg_atomic_read_u64(&src.pg_leaf_failures);
    out.injected_pg_leaf_failures =
        pg_atomic_read_u64(&src.injected_pg_leaf_failures);
    out.materialize_admissions =
        pg_atomic_read_u64(&src.materialize_admissions);
    out.materialize_failure_unwinds =
        pg_atomic_read_u64(&src.materialize_failure_unwinds);
    out.materialize_admission_restored =
        pg_atomic_read_u64(&src.materialize_admission_restored);
    out.materialize_admission_mismatches =
        pg_atomic_read_u64(&src.materialize_admission_mismatches);
    const std::uint32_t last_failure =
        pg_atomic_read_u32(&src.last_failure_point);
    out.last_failure_point = static_cast<AsyncMpsWorkerPgFailure>(
        last_failure & ~kAsyncMpsWorkerInjectedFailureFlag);
    out.last_failure_injected =
        (last_failure & kAsyncMpsWorkerInjectedFailureFlag) != 0u;
    out.armed_failure_point = static_cast<AsyncMpsWorkerPgFailure>(
        pg_atomic_read_u32(&src.armed_failure_point));
    return out;
}

[[nodiscard]] bool arm_async_mps_worker_pg_failure(
        AsyncMpsWorkerPgFailure point) noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    if (g_shared == nullptr
        || point == AsyncMpsWorkerPgFailure::none
        || point > AsyncMpsWorkerPgFailure::materialize_scanner_read) {
        return false;
    }
    pg_atomic_write_u32(
        &g_shared->async_mps_worker_boundary_stats.armed_failure_point,
        static_cast<std::uint32_t>(point));
    return true;
#else
    (void)point;
    return false;
#endif
}

void disarm_async_mps_worker_pg_failure() noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    if (g_shared == nullptr) {
        return;
    }
    pg_atomic_write_u32(
        &g_shared->async_mps_worker_boundary_stats.armed_failure_point,
        static_cast<std::uint32_t>(AsyncMpsWorkerPgFailure::none));
#endif
}

[[nodiscard]] const char* async_mps_worker_failure_point_name(
        AsyncMpsWorkerPgFailure point) noexcept {
    switch (point) {
    case AsyncMpsWorkerPgFailure::none:
        return "none";
    case AsyncMpsWorkerPgFailure::relation_open:
        return "relation_open";
    case AsyncMpsWorkerPgFailure::relation_identity:
        return "relation_identity";
    case AsyncMpsWorkerPgFailure::relation_close:
        return "relation_close";
    case AsyncMpsWorkerPgFailure::spi_connect:
        return "spi_connect";
    case AsyncMpsWorkerPgFailure::spi_execute:
        return "spi_execute";
    case AsyncMpsWorkerPgFailure::spi_read:
        return "spi_read";
    case AsyncMpsWorkerPgFailure::spi_finish:
        return "spi_finish";
    case AsyncMpsWorkerPgFailure::materialize_error_log:
        return "materialize_error_log";
    case AsyncMpsWorkerPgFailure::materialize_interrupt:
        return "materialize_interrupt";
    case AsyncMpsWorkerPgFailure::materialize_scanner_read:
        return "materialize_scanner_read";
    }
    return "invalid";
}

void record_async_mps_worker_pg_leaf_call(
        AsyncMpsWorkerPgFailure point) noexcept {
    if (g_shared == nullptr) {
        return;
    }
    PgAsyncMpsWorkerBoundaryStats& stats =
        g_shared->async_mps_worker_boundary_stats;
    switch (point) {
    case AsyncMpsWorkerPgFailure::relation_open:
    case AsyncMpsWorkerPgFailure::relation_identity:
    case AsyncMpsWorkerPgFailure::relation_close:
        pg_atomic_fetch_add_u64(&stats.relation_leaf_calls, 1);
        break;
    case AsyncMpsWorkerPgFailure::spi_connect:
    case AsyncMpsWorkerPgFailure::spi_execute:
    case AsyncMpsWorkerPgFailure::spi_read:
    case AsyncMpsWorkerPgFailure::spi_finish:
        pg_atomic_fetch_add_u64(&stats.spi_leaf_calls, 1);
        break;
    case AsyncMpsWorkerPgFailure::materialize_interrupt:
        pg_atomic_fetch_add_u64(
            &stats.materialize_interrupt_leaf_calls, 1);
        break;
    case AsyncMpsWorkerPgFailure::materialize_scanner_read:
        pg_atomic_fetch_add_u64(
            &stats.materialize_scanner_read_leaf_calls, 1);
        break;
    case AsyncMpsWorkerPgFailure::none:
    case AsyncMpsWorkerPgFailure::materialize_error_log:
        break;
    }
}

void record_async_mps_worker_pg_leaf_failure(
        AsyncMpsWorkerPgFailure point) noexcept {
    if (g_shared == nullptr) {
        return;
    }
    PgAsyncMpsWorkerBoundaryStats& stats =
        g_shared->async_mps_worker_boundary_stats;
    bool injected = false;
#ifdef BIOSEQ_TEST_INDEXES
    if (g_async_mps_worker_injected_failure == point) {
        injected = true;
        g_async_mps_worker_injected_failure =
            AsyncMpsWorkerPgFailure::none;
    }
#endif
    pg_atomic_fetch_add_u64(&stats.pg_leaf_failures, 1);
    const std::uint32_t encoded_failure =
        static_cast<std::uint32_t>(point) |
        (injected ? kAsyncMpsWorkerInjectedFailureFlag : 0u);
    pg_atomic_write_u32(
        &stats.last_failure_point, encoded_failure);
}

/* Must be called from one invoke_pg_as_cpp callback, never from C++ scope. */
void inject_async_mps_worker_pg_failure_leaf(
        AsyncMpsWorkerPgFailure point) {
#ifdef BIOSEQ_TEST_INDEXES
    if (g_shared == nullptr) {
        return;
    }
    PgAsyncMpsWorkerBoundaryStats& stats =
        g_shared->async_mps_worker_boundary_stats;
    std::uint32_t expected = static_cast<std::uint32_t>(point);
    if (!pg_atomic_compare_exchange_u32(
            &stats.armed_failure_point,
            &expected,
            static_cast<std::uint32_t>(AsyncMpsWorkerPgFailure::none))) {
        return;
    }
    pg_atomic_fetch_add_u64(&stats.injected_pg_leaf_failures, 1);
    g_async_mps_worker_injected_failure = point;
    ereport(ERROR,
        (errcode(ERRCODE_INTERNAL_ERROR),
         errmsg("injected asynchronous MPS worker PostgreSQL leaf failure"),
         errdetail("failure_point=%s",
            async_mps_worker_failure_point_name(point))));
#else
    (void)point;
#endif
}


void record_async_mps_materialize_admission() noexcept {
    if (g_shared != nullptr) {
        pg_atomic_fetch_add_u64(
            &g_shared->async_mps_worker_boundary_stats.materialize_admissions,
            1);
    }
}

void record_async_mps_materialize_failure_unwind(
        bool admission_restored) noexcept {
    if (g_shared == nullptr) {
        return;
    }
    PgAsyncMpsWorkerBoundaryStats& stats =
        g_shared->async_mps_worker_boundary_stats;
    pg_atomic_fetch_add_u64(&stats.materialize_failure_unwinds, 1);
    pg_atomic_fetch_add_u64(
        admission_restored
            ? &stats.materialize_admission_restored
            : &stats.materialize_admission_mismatches,
        1);
}

}  // namespace bioseq::parallelism::pg
