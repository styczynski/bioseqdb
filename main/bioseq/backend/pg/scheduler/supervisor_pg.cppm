/**
 * @file supervisor_pg.cppm
 * @brief PostgreSQL-facing scheduler supervision and stats bridge.
 */
module;
#include "common.h"

extern "C" {
#include "access/xact.h"
#include "access/parallel.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "port/atomics.h"
#include "storage/dsm.h"
#include "storage/dsm_impl.h"
#include "storage/condition_variable.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"

void StartTransactionCommand(void);
void CommitTransactionCommand(void);
void AbortCurrentTransaction(void);
}

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

export module bioseq.scheduler.supervisor_pg;
import bioseq.common.mlogging;
import bioseq.config;                  // DOC0322 §9.3 — bioseq_scheduler_*, bioseq::settings::index_async_persist, bioseq::settings::index_memory_cap_mb GUCs
import bioseq.instr;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.gpu.vulkan.runtime;
import bioseq.gpu.vulkan.shader_cache;
import bioseq.gpu.vulkan.shader_variants;
import bioseq.pg_utils;
import bioseq.indexing.type;
import bioseq.indexing.minimizer_sequence;   // DOC0345 Patch K — re-exports MinimizerSketchBackend + minimizer_sketch_backend_name (no HWY_NAMESPACE macro needed here)
import bioseq.storage.manager;
import bioseq.storage.lazy;
import bioseq.storage.scanner;
import bioseq.storage.scanner_pg;
import bioseq.types.pg_compact_sequence;
import bioseq.sequences;
import bioseq.storage.indexing_pg.chunked_storage;
import bioseq.storage.indexing_pg.handles;

namespace bioseq::parallelism::pg {

[[nodiscard]] inline bool is_standalone_backend_process() noexcept {
    return MyBackendType == B_STANDALONE_BACKEND
        && !IsPostmasterEnvironment
        && !IsUnderPostmaster;
}

struct PgSchedulerStats {
    static constexpr std::size_t kInflightSlots =
        bioseq::parallelism::SchedulerStats::kInflightSlots;
    pg_atomic_uint64 batches_scheduled;
    pg_atomic_uint64 batches_processed;
    pg_atomic_uint64 batches_in_flight;
    pg_atomic_uint64 queries_waiting;
    pg_atomic_uint64 queries_processed;
    pg_atomic_uint64 rate_window_start_ns;
    pg_atomic_uint64 rate_window_start_queries;
    pg_atomic_uint64 restart_ns;
    pg_atomic_uint64 inflight_submit_ns[kInflightSlots];
    pg_atomic_uint32 inflight_generation[kInflightSlots];
    pg_atomic_uint64 inflight_seq;
};

enum class PersistPayloadKind : std::uint32_t {
    none = 0,
    prohibited_legacy_dsm = 1,
    prohibited_legacy_file = 2,
    mps_build_identity = 3,
};

struct PgPersistQueueSlot {
    pg_atomic_uint32 state;
    pg_atomic_uint32 generation;
    Oid db_oid;
    Oid user_relid;
    Oid tablespace_oid;
    Oid main_relfilenode;
    std::int16_t column_attno;
    std::uint8_t bucket_idx;
    std::uint32_t shard_idx;
    std::uint64_t sequence_rowno;
    std::uint64_t payload_bytes;
    dsm_handle dsm;
    std::uint32_t payload_kind;
    std::int32_t requested_provider;
};

enum class RuntimeInitState : std::uint32_t {
    Pending = 0,
    Ready = 1,
    Failed = 2,
};

enum class CompileSlotState : std::uint32_t {
    Free = 0,
    Filling = 1,
    Requested = 2,
    Compiling = 3,
    DoneOk = 4,
    DoneErr = 5,
};

struct ShaderCompileSlot {
    pg_atomic_uint32 state;
    pg_atomic_uint32 generation;
    bioseq::gpu::vulkan::ShaderVariantKey key{};
    std::uint32_t requester_pid = 0;
    std::uint32_t errc = 0;
};

struct IndexPersistDsmHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t chunk_count;
    std::uint32_t index_name_len;
    std::uint64_t payload_bytes;
};

struct IndexPersistDsmChunk {
    std::int64_t sub_key;
    std::uint32_t chunk_no;
    std::uint32_t reserved;
    std::uint64_t offset;
    std::uint64_t size;
};

struct IndexPersistFileDsmHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t index_name_len;
    std::uint32_t path_len;
    std::uint64_t file_bytes;
    std::uint64_t chunk_count;
};

struct IndexBuildMpsDsmHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t index_name_len;
    std::uint8_t alphabet_code;
    std::uint8_t storage_unit_byte_width;
    std::uint16_t last_fragment_padding;
    std::int32_t pair_sort_impl;
    std::int32_t pipeline_mode;
    std::int32_t max_concurrency;
    std::int32_t index_memory_cap_mb;
    std::int32_t index_sketch_backend;  // DOC0345 Patch K — repurposed from `reserved` (keeps 72-byte size)
    std::int32_t requested_provider;
    bioseq::storage::seq_locator locator;
};
static_assert(sizeof(IndexBuildMpsDsmHeader) == 80);
static_assert(offsetof(IndexBuildMpsDsmHeader, requested_provider) == 36);
static_assert(offsetof(IndexBuildMpsDsmHeader, locator) == 40);
static_assert(std::is_trivially_copyable_v<IndexBuildMpsDsmHeader>);

struct PersistFileChunkHeader {
    std::int64_t sub_key;
    std::uint32_t chunk_no;
    std::uint32_t reserved;
    std::uint64_t size;
};

inline constexpr std::uint32_t kIndexPersistDsmMagic = 0xB1051D58u;
inline constexpr std::uint32_t kIndexPersistDsmVersion = 1;
inline constexpr std::uint32_t kIndexPersistFileDsmMagic = 0xB1051D59u;
inline constexpr std::uint32_t kIndexPersistFileDsmVersion = 1;
inline constexpr std::uint32_t kIndexBuildMpsDsmMagic = 0xB1051D5Au;
inline constexpr std::uint32_t kIndexBuildMpsDsmVersion = 4;

struct PgMpsBuildStats {
    pg_atomic_uint64 generation;
    pg_atomic_uint64 backend_pid;
    pg_atomic_uint64 db_oid;
    pg_atomic_uint64 relfilenode;
    pg_atomic_uint64 sequence_rowno;
    pg_atomic_uint64 total_run_count;
    pg_atomic_uint64 resident_runs;
    pg_atomic_uint64 file_runs;
    pg_atomic_uint64 resident_run_bytes;
    pg_atomic_uint64 resident_peak_bytes;
    pg_atomic_uint64 spilled_run_bytes;
    pg_atomic_uint64 postgres_temp_file_bytes;
    pg_atomic_uint64 postgres_temp_file_count;
    pg_atomic_uint64 merge_reader_buffer_bytes;
    pg_atomic_uint64 merge_pass_count;
    pg_atomic_uint64 spill_count_from_pressure;
    pg_atomic_uint64 resident_demotions;
    pg_atomic_uint64 position_spill_bytes;
    pg_atomic_uint64 position_spill_count;
    pg_atomic_uint64 worker_build_ns;
    pg_atomic_uint64 pipeline_mode;
    pg_atomic_uint64 max_concurrency;
    pg_atomic_uint64 index_memory_cap_mb;
    pg_atomic_uint64 lazy_worker_shareable;
};

/*
 * Cross-backend evidence for the asynchronous MPS worker's PostgreSQL leaf
 * seams.  These counters live in scheduler shared memory because the backend
 * that arms an integration failure is not the background worker that consumes
 * it.  The injection selector is one-shot: the worker atomically clears it
 * before raising the requested ERROR.
 */
struct PgAsyncMpsWorkerBoundaryStats {
    pg_atomic_uint64 relation_leaf_calls;
    pg_atomic_uint64 spi_leaf_calls;
    pg_atomic_uint64 materialize_interrupt_leaf_calls;
    pg_atomic_uint64 materialize_scanner_read_leaf_calls;
    pg_atomic_uint64 pg_leaf_failures;
    pg_atomic_uint64 injected_pg_leaf_failures;
    pg_atomic_uint64 materialize_admissions;
    pg_atomic_uint64 materialize_failure_unwinds;
    pg_atomic_uint64 materialize_admission_restored;
    pg_atomic_uint64 materialize_admission_mismatches;
    pg_atomic_uint32 last_failure_point;
    pg_atomic_uint32 armed_failure_point;
};

struct BioseqSchedulerShared {
    pg_atomic_uint32 cfg_max_concurrency;
    pg_atomic_uint32 cfg_generation;
    pg_atomic_uint32 state;
    PgSchedulerStats stats;
    PgMpsBuildStats mps_build_stats;
    PgAsyncMpsWorkerBoundaryStats async_mps_worker_boundary_stats;
    static constexpr std::size_t kPersistQueueSize = 64;
    PgPersistQueueSlot persist_queue[kPersistQueueSize];
    pg_atomic_uint32 runtime_init_state;
    pg_atomic_uint32 runtime_init_generation;
    ConditionVariable runtime_init_cv;
    pg_atomic_uint32 runtime_init_errc;
    char cache_digest_hex[33];
    pg_atomic_uint32 supervisor_pgprocno;
    pg_atomic_uint32 supervisor_generation;
    static constexpr std::size_t kCompileQueueSize = 64;
    ShaderCompileSlot compile_queue[kCompileQueueSize];
    ConditionVariable compile_done_cv;
};

inline BioseqSchedulerShared* g_shared = nullptr;
inline shmem_request_hook_type prev_shmem_request_hook = nullptr;
inline shmem_startup_hook_type prev_shmem_startup_hook = nullptr;
inline volatile sig_atomic_t got_sigterm = 0;
inline volatile sig_atomic_t got_sighup = 0;
inline bool g_persist_xact_callback_registered = false;

static_assert(alignof(BioseqSchedulerShared) <= MAXIMUM_ALIGNOF);

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

void reset_shared_stats(PgSchedulerStats& stats, std::uint64_t now_ns) {
    pg_atomic_write_u64(&stats.batches_scheduled, 0);
    pg_atomic_write_u64(&stats.batches_processed, 0);
    pg_atomic_write_u64(&stats.batches_in_flight, 0);
    pg_atomic_write_u64(&stats.queries_waiting, 0);
    pg_atomic_write_u64(&stats.queries_processed, 0);
    pg_atomic_write_u64(&stats.rate_window_start_ns, now_ns);
    pg_atomic_write_u64(&stats.rate_window_start_queries, 0);
    pg_atomic_write_u64(&stats.restart_ns, now_ns);
    pg_atomic_write_u64(&stats.inflight_seq, 0);
    for (std::size_t slot = 0; slot < PgSchedulerStats::kInflightSlots; ++slot) {
        pg_atomic_write_u64(&stats.inflight_submit_ns[slot], 0);
        pg_atomic_write_u32(&stats.inflight_generation[slot], 0);
    }
}

void scheduler_shmem_request() {
    if (prev_shmem_request_hook != nullptr) {
        prev_shmem_request_hook();
    }
    RequestAddinShmemSpace(sizeof(BioseqSchedulerShared));
}

void scheduler_shmem_startup() {
    if (prev_shmem_startup_hook != nullptr) {
        prev_shmem_startup_hook();
    }

    bool found = false;
    g_shared = static_cast<BioseqSchedulerShared*>(
        ShmemInitStruct("bioseq scheduler shared state",
                        sizeof(BioseqSchedulerShared),
                        &found));
    if (!found) {
        pg_atomic_init_u32(&g_shared->cfg_max_concurrency, 0);
        pg_atomic_init_u32(&g_shared->cfg_generation, 1);
        pg_atomic_init_u32(&g_shared->state, 1);

        pg_atomic_init_u64(&g_shared->stats.batches_scheduled, 0);
        pg_atomic_init_u64(&g_shared->stats.batches_processed, 0);
        pg_atomic_init_u64(&g_shared->stats.batches_in_flight, 0);
        pg_atomic_init_u64(&g_shared->stats.queries_waiting, 0);
        pg_atomic_init_u64(&g_shared->stats.queries_processed, 0);
        pg_atomic_init_u64(&g_shared->stats.rate_window_start_ns, 0);
        pg_atomic_init_u64(&g_shared->stats.rate_window_start_queries, 0);
        pg_atomic_init_u64(&g_shared->stats.restart_ns, 0);
        pg_atomic_init_u64(&g_shared->stats.inflight_seq, 0);
        for (std::size_t slot = 0; slot < PgSchedulerStats::kInflightSlots; ++slot) {
            pg_atomic_init_u64(&g_shared->stats.inflight_submit_ns[slot], 0);
            pg_atomic_init_u32(&g_shared->stats.inflight_generation[slot], 0);
        }
        init_mps_build_stats(g_shared->mps_build_stats);
        init_async_mps_worker_boundary_stats(
            g_shared->async_mps_worker_boundary_stats);
        for (PgPersistQueueSlot& slot : g_shared->persist_queue) {
            pg_atomic_init_u32(&slot.state, 0);
            pg_atomic_init_u32(&slot.generation, 0);
            slot.db_oid = InvalidOid;
            slot.user_relid = InvalidOid;
            slot.tablespace_oid = InvalidOid;
            slot.main_relfilenode = InvalidOid;
            slot.column_attno = 0;
            slot.bucket_idx = 0;
            slot.shard_idx = 0;
            slot.sequence_rowno = 0;
            slot.payload_bytes = 0;
            slot.dsm = DSM_HANDLE_INVALID;
            slot.payload_kind = static_cast<std::uint32_t>(
                PersistPayloadKind::none);
            slot.requested_provider =
                index_payload_storage_backend_wire_value(
                    IndexPayloadStorageBackend::postgres_relation);
        }
        pg_atomic_init_u32(
            &g_shared->runtime_init_state,
            static_cast<std::uint32_t>(RuntimeInitState::Pending));
        pg_atomic_init_u32(&g_shared->runtime_init_generation, 0);
        ConditionVariableInit(&g_shared->runtime_init_cv);
        pg_atomic_init_u32(
            &g_shared->runtime_init_errc,
            static_cast<std::uint32_t>(bioseq::gpu::vulkan::ShaderCacheErrc::Ok));
        g_shared->cache_digest_hex[0] = '\0';
        pg_atomic_init_u32(
            &g_shared->supervisor_pgprocno,
            static_cast<std::uint32_t>(INVALID_PROC_NUMBER));
        pg_atomic_init_u32(&g_shared->supervisor_generation, 0);
        for (ShaderCompileSlot& slot : g_shared->compile_queue) {
            pg_atomic_init_u32(
                &slot.state,
                static_cast<std::uint32_t>(CompileSlotState::Free));
            pg_atomic_init_u32(&slot.generation, 0);
            slot.key = bioseq::gpu::vulkan::ShaderVariantKey{};
            slot.requester_pid = 0;
            slot.errc = static_cast<std::uint32_t>(
                bioseq::gpu::vulkan::ShaderCacheErrc::Ok);
        }
        ConditionVariableInit(&g_shared->compile_done_cv);
        reset_shared_stats(g_shared->stats, static_cast<std::uint64_t>(bioseq::instr::clock::now_ns()));
    }
}

void pool_shutdown_before_exit(int, Datum) {
    bioseq::parallelism::GlobalPool::instance().shutdown_at_exit();
}

void supervisor_sigterm(SIGNAL_ARGS) {
    const int save_errno = errno;
    got_sigterm = 1;
    if (MyProc != nullptr) {
        SetLatch(&MyProc->procLatch);
    }
    errno = save_errno;
}

void supervisor_sighup(SIGNAL_ARGS) {
    const int save_errno = errno;
    got_sighup = 1;
    if (MyProc != nullptr) {
        SetLatch(&MyProc->procLatch);
    }
    errno = save_errno;
}

[[nodiscard]] unsigned effective_max_concurrency(unsigned configured) {
    unsigned concurrency = configured == 0
        ? bioseq::parallelism::default_concurrency()
        : configured;
    if (IsParallelWorker() && !bioseq::scheduler::settings::allow_pg_parallel_composition) {
        concurrency = 1;
    }
    return std::max(1u, concurrency);
}

} // namespace bioseq::parallelism::pg

export namespace bioseq::parallelism::pg {

struct BatchTicket {
    bool shared = false;
    std::uint32_t generation = 0;
    std::size_t slot = 0;
    std::int64_t queries = 0;
    bioseq::parallelism::GlobalPool::BatchTicket local{};
};

struct SchedulerSnapshot {
    bool supervisor = false;
    std::uint32_t config_generation = 0;
    bioseq::parallelism::PipelineConcurrencyMode mode =
        bioseq::parallelism::PipelineConcurrencyMode::SINGLE;
    unsigned min_threads = 1;
    unsigned max_threads = 1;
    unsigned current_threads = 0;
    std::uint64_t batches_scheduled = 0;
    std::uint64_t batches_awaiting = 0;
    std::uint64_t batches_processed = 0;
    std::uint64_t queries_waiting = 0;
    std::uint64_t queries_processed = 0;
    double oldest_waiting_batch_age_secs = 0.0;
    double queries_per_sec = 0.0;
    double uptime_secs = 0.0;
    std::uint64_t async_persist_jobs_waiting = 0;
    std::uint64_t index_memory_reserved_bytes = 0;
};

struct AsyncMpsBuildStatsSnapshot {
    std::uint64_t generation = 0;
    bioseq::storage::indexing::LastMpsBuildStats stats{};
    std::uint64_t worker_build_ns = 0;
    std::uint64_t pipeline_mode = 0;
    std::uint64_t max_concurrency = 0;
    std::uint64_t index_memory_cap_mb = 0;
    std::uint64_t lazy_worker_shareable = 0;
};

/**
 * One-shot PostgreSQL failure points in the asynchronous MPS worker.  The
 * selector is stored in scheduler shared memory, so a normal backend can arm
 * it for the background worker without relying on process-local test state.
 */
enum class AsyncMpsWorkerPgFailure : std::uint32_t {
    none = 0,
    relation_open = 1,
    relation_identity = 2,
    relation_close = 3,
    spi_connect = 4,
    spi_execute = 5,
    spi_read = 6,
    spi_finish = 7,
    materialize_error_log = 8,
    materialize_interrupt = 9,
    materialize_scanner_read = 10,
};

inline constexpr std::uint32_t kAsyncMpsWorkerInjectedFailureFlag =
    std::uint32_t{1} << 31u;

#ifdef BIOSEQ_TEST_INDEXES
inline thread_local AsyncMpsWorkerPgFailure
    g_async_mps_worker_injected_failure = AsyncMpsWorkerPgFailure::none;
#endif

struct AsyncMpsWorkerBoundaryStatsSnapshot {
    std::uint64_t relation_leaf_calls = 0;
    std::uint64_t spi_leaf_calls = 0;
    std::uint64_t materialize_interrupt_leaf_calls = 0;
    std::uint64_t materialize_scanner_read_leaf_calls = 0;
    std::uint64_t pg_leaf_failures = 0;
    std::uint64_t injected_pg_leaf_failures = 0;
    std::uint64_t materialize_admissions = 0;
    std::uint64_t materialize_failure_unwinds = 0;
    std::uint64_t materialize_admission_restored = 0;
    std::uint64_t materialize_admission_mismatches = 0;
    AsyncMpsWorkerPgFailure last_failure_point =
        AsyncMpsWorkerPgFailure::none;
    bool last_failure_injected = false;
    AsyncMpsWorkerPgFailure armed_failure_point =
        AsyncMpsWorkerPgFailure::none;
};

struct PersistTarget {
    Oid db_oid = InvalidOid;
    Oid user_relid = InvalidOid;
    Oid tablespace_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    std::int16_t column_attno = 0;
    std::uint8_t bucket_idx = 0;
    std::uint32_t shard_idx = 0;
    std::uint64_t sequence_rowno = 0;
    std::string_view index_name{};
};

enum class AsyncMpsStagePgFailure : std::uint8_t {
    none = 0,
    register_xact_callback = 1,
    dsm_create = 2,
    dsm_pin = 3,
    dsm_detach = 4,
};

#ifdef BIOSEQ_TEST_INDEXES
inline thread_local AsyncMpsStagePgFailure
    g_async_mps_stage_pg_failure = AsyncMpsStagePgFailure::none;
inline thread_local std::uint64_t
    g_async_mps_stage_injected_failures = 0;
#endif

[[nodiscard]] bool arm_async_mps_stage_pg_failure(
        AsyncMpsStagePgFailure point) noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    if (point == AsyncMpsStagePgFailure::none
        || point > AsyncMpsStagePgFailure::dsm_detach) {
        return false;
    }
    g_async_mps_stage_pg_failure = point;
    return true;
#else
    (void)point;
    return false;
#endif
}

void disarm_async_mps_stage_pg_failure() noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    g_async_mps_stage_pg_failure = AsyncMpsStagePgFailure::none;
#endif
}

[[nodiscard]] AsyncMpsStagePgFailure
async_mps_stage_pg_failure_armed() noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    return g_async_mps_stage_pg_failure;
#else
    return AsyncMpsStagePgFailure::none;
#endif
}

[[nodiscard]] std::uint64_t
async_mps_stage_injected_failure_count() noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    return g_async_mps_stage_injected_failures;
#else
    return 0u;
#endif
}

void inject_async_mps_stage_pg_failure_leaf(
        AsyncMpsStagePgFailure point) {
#ifdef BIOSEQ_TEST_INDEXES
    if (g_async_mps_stage_pg_failure != point) {
        return;
    }
    g_async_mps_stage_pg_failure = AsyncMpsStagePgFailure::none;
    ++g_async_mps_stage_injected_failures;
    ereport(ERROR,
        (errcode(ERRCODE_INTERNAL_ERROR),
         errmsg("injected asynchronous MPS staging PostgreSQL leaf failure"),
         errdetail("failure_point=%u", static_cast<unsigned>(point))));
#else
    (void)point;
#endif
}

template <class F>
auto invoke_async_mps_stage_pg_leaf(
        AsyncMpsStagePgFailure point,
        F&& function)
    -> std::invoke_result_t<std::remove_reference_t<F>&> {
    return bioseq::pg_utils::invoke_pg_as_cpp(
        [point, &function]() ->
            std::invoke_result_t<std::remove_reference_t<F>&> {
            inject_async_mps_stage_pg_failure_leaf(point);
            return std::invoke(function);
        });
}

struct PersistChunk {
    bioseq::indexing::index_sub_key_t sub_key = 0;
    std::uint32_t chunk_no = 0;
    std::span<const char> bytes{};
};

struct PendingPersistJob {
    // `target.index_name` is deliberately empty after staging. The owned name
    // below is the only variable-length identity and is serialized into the
    // metadata-only DSM; no biological/index bytes enter this queue.
    PersistTarget target{};
    std::string index_name{};
    std::string temp_file_path{};
    dsm_handle dsm = DSM_HANDLE_INVALID;
    std::uint64_t payload_bytes = 0;
    bool published = false;
    bool file_backed = false;
    PersistPayloadKind payload_kind = PersistPayloadKind::none;
    IndexPayloadStorageBackend requested_provider =
        IndexPayloadStorageBackend::postgres_relation;
    bioseq::parallelism::MemoryPermit permit{};
};

inline std::atomic<std::uint64_t> g_prohibited_legacy_persist_rejections{0};

inline void record_prohibited_legacy_persist_rejection(
        std::string_view route) noexcept {
    g_prohibited_legacy_persist_rejections.fetch_add(
        1, std::memory_order_relaxed);
    bioseq_log<DBG>(
        "[bioseq] ERROR prohibited legacy async persistence rejected route={} total_rejections={}",
        route,
        static_cast<unsigned long long>(
            g_prohibited_legacy_persist_rejections.load(
                std::memory_order_relaxed)));
}

[[nodiscard]] std::vector<PendingPersistJob>& pending_persist_jobs() {
    static std::vector<PendingPersistJob> jobs;
    return jobs;
}

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

[[nodiscard]] std::uint64_t align_up_u64(std::uint64_t value, std::uint64_t alignment) noexcept {
    return alignment == 0
        ? value
        : ((value + alignment - 1) / alignment) * alignment;
}

void release_pinned_dsm(dsm_handle handle) noexcept {
    if (handle == DSM_HANDLE_INVALID) {
        return;
    }
    try {
        bioseq::pg_utils::invoke_pg_as_cpp(
            [handle] { dsm_unpin_segment(handle); });
    } catch (...) {
        // Transaction/worker cleanup must not longjmp across PendingPersistJob
        // owners. Supported PostgreSQL releases do not ERROR here; preserve
        // the authoritative transaction/worker outcome if that changes.
    }
}

void remove_temp_file_noexcept(std::string_view path) noexcept {
    if (path.empty()) {
        return;
    }
    std::string owned{path};
    std::remove(owned.c_str());
}

[[nodiscard]] std::string file_path_from_metadata_dsm(dsm_handle handle) {
    if (handle == DSM_HANDLE_INVALID) {
        return {};
    }
    dsm_segment* seg = dsm_attach(handle);
    if (seg == nullptr) {
        return {};
    }
    const Size mapped = dsm_segment_map_length(seg);
    std::string path;
    if (mapped >= sizeof(IndexPersistFileDsmHeader)) {
        char* base = static_cast<char*>(dsm_segment_address(seg));
        IndexPersistFileDsmHeader header{};
        std::memcpy(&header, base, sizeof(header));
        if (header.magic == kIndexPersistFileDsmMagic
            && header.version == kIndexPersistFileDsmVersion
            && header.index_name_len > 0
            && header.path_len > 0
            && sizeof(IndexPersistFileDsmHeader) + header.index_name_len
                    <= static_cast<std::uint64_t>(mapped)
            && sizeof(IndexPersistFileDsmHeader) + header.index_name_len
                    + header.path_len
                    <= static_cast<std::uint64_t>(mapped)) {
            const char* path_begin =
                base + sizeof(IndexPersistFileDsmHeader) + header.index_name_len;
            path.assign(path_begin, path_begin + header.path_len);
        }
    }
    dsm_detach(seg);
    return path;
}

void remove_file_payload_if_present_noexcept(dsm_handle handle) noexcept {
    try {
        remove_temp_file_noexcept(file_path_from_metadata_dsm(handle));
    } catch (...) {
    }
}

struct PersistFileMetadata {
    std::string index_name{};
    std::string path{};
    std::uint64_t file_bytes = 0;
    std::uint64_t chunk_count = 0;
};

struct MpsBuildMetadata {
    std::string index_name{};
    bioseq::storage::seq_locator locator{};
    std::uint8_t alphabet_code = 0;
    std::uint8_t storage_unit_byte_width = 0;
    std::uint16_t last_fragment_padding = 0;
    std::int32_t pair_sort_impl = 0;
    std::int32_t pipeline_mode = 0;
    std::int32_t max_concurrency = 0;
    std::int32_t index_memory_cap_mb = 0;
    std::int32_t index_sketch_backend = 0;  // DOC0345 Patch K
    IndexPayloadStorageBackend requested_provider =
        IndexPayloadStorageBackend::postgres_relation;
};

[[nodiscard]] std::optional<PersistFileMetadata>
parse_persist_file_metadata(const char* base, Size mapped) {
    if (mapped < sizeof(IndexPersistFileDsmHeader)) {
        return std::nullopt;
    }
    IndexPersistFileDsmHeader header{};
    std::memcpy(&header, base, sizeof(header));
    if (header.magic != kIndexPersistFileDsmMagic
        || header.version != kIndexPersistFileDsmVersion
        || header.index_name_len == 0
        || header.path_len == 0) {
        return std::nullopt;
    }
    const std::uint64_t names_offset = sizeof(IndexPersistFileDsmHeader);
    const std::uint64_t path_offset = names_offset + header.index_name_len;
    const std::uint64_t path_end = path_offset + header.path_len;
    if (path_offset < names_offset
        || path_end < path_offset
        || path_end > static_cast<std::uint64_t>(mapped)) {
        return std::nullopt;
    }
    PersistFileMetadata meta{};
    meta.index_name.assign(base + names_offset, base + path_offset);
    meta.path.assign(base + path_offset, base + path_end);
    meta.file_bytes = header.file_bytes;
    meta.chunk_count = header.chunk_count;
    return meta;
}

[[nodiscard]] std::optional<MpsBuildMetadata>
parse_mps_build_metadata(const char* base, Size mapped) {
    if (mapped < sizeof(IndexBuildMpsDsmHeader)) {
        return std::nullopt;
    }
    IndexBuildMpsDsmHeader header{};
    std::memcpy(&header, base, sizeof(header));
    if (header.magic != kIndexBuildMpsDsmMagic
        || header.version != kIndexBuildMpsDsmVersion
        || header.index_name_len == 0
        || !is_valid_index_payload_storage_backend_wire_value(
            header.requested_provider)
        || header.locator.version != bioseq::storage::kLocatorVersion
        || header.locator.source_epoch == 0) {
        return std::nullopt;
    }
    const std::uint64_t name_offset = sizeof(IndexBuildMpsDsmHeader);
    const std::uint64_t name_end = name_offset + header.index_name_len;
    if (name_end < name_offset || name_end > static_cast<std::uint64_t>(mapped)) {
        return std::nullopt;
    }
    MpsBuildMetadata meta{};
    meta.index_name.assign(base + name_offset, base + name_end);
    meta.locator = header.locator;
    meta.alphabet_code = header.alphabet_code;
    meta.storage_unit_byte_width = header.storage_unit_byte_width;
    meta.last_fragment_padding = header.last_fragment_padding;
    meta.pair_sort_impl = header.pair_sort_impl;
    meta.pipeline_mode = header.pipeline_mode;
    meta.max_concurrency = header.max_concurrency;
    meta.index_memory_cap_mb = header.index_memory_cap_mb;
    meta.index_sketch_backend = header.index_sketch_backend;  // DOC0345 Patch K
    meta.requested_provider =
        *decode_index_payload_storage_backend(header.requested_provider);
    return meta;
}

[[nodiscard]] bool publish_persist_job_to_shared_queue(PendingPersistJob& job) noexcept {
    if (g_shared == nullptr || job.dsm == DSM_HANDLE_INVALID) {
        return false;
    }
    if (job.payload_kind != PersistPayloadKind::mps_build_identity) {
        record_prohibited_legacy_persist_rejection("queue_publish");
        return false;
    }
    if (!is_valid_index_payload_storage_backend_wire_value(
            index_payload_storage_backend_wire_value(
                job.requested_provider))
        || job.requested_provider
            != IndexPayloadStorageBackend::postgres_relation) {
        record_prohibited_legacy_persist_rejection(
            "queue_publish_provider");
        return false;
    }
    for (PgPersistQueueSlot& slot : g_shared->persist_queue) {
        std::uint32_t expected = 0;
        if (!pg_atomic_compare_exchange_u32(&slot.state, &expected, 3)) {
            continue;
        }
        slot.db_oid = job.target.db_oid;
        slot.user_relid = job.target.user_relid;
        slot.tablespace_oid = job.target.tablespace_oid;
        slot.main_relfilenode = job.target.main_relfilenode;
        slot.column_attno = job.target.column_attno;
        slot.bucket_idx = job.target.bucket_idx;
        slot.shard_idx = job.target.shard_idx;
        slot.sequence_rowno = job.target.sequence_rowno;
        slot.payload_bytes = job.payload_bytes;
        slot.dsm = job.dsm;
        slot.payload_kind = static_cast<std::uint32_t>(job.payload_kind);
        slot.requested_provider =
            index_payload_storage_backend_wire_value(
                job.requested_provider);
        pg_atomic_fetch_add_u32(&slot.generation, 1);
        pg_atomic_write_u32(&slot.state, 1);
        bioseq_log<DBG>("[bioseq] index: published async persist job row={} index={} bytes={}",
                static_cast<unsigned long long>(job.target.sequence_rowno),
                job.index_name,
                static_cast<unsigned long long>(job.payload_bytes));
        return true;
    }
    return false;
}

void bioseq_persist_xact_callback(XactEvent event, void*) {
    std::vector<PendingPersistJob>& jobs = pending_persist_jobs();
    const bool commit =
        event == XACT_EVENT_COMMIT
        || event == XACT_EVENT_PARALLEL_COMMIT;
    const bool abort =
        event == XACT_EVENT_ABORT
        || event == XACT_EVENT_PARALLEL_ABORT
        || event == XACT_EVENT_PREPARE;
    if (!commit && !abort) {
        return;
    }
    if (commit) {
        for (PendingPersistJob& job : jobs) {
            if (publish_persist_job_to_shared_queue(job)) {
                job.published = true;
            } else {
                bioseq_log<DBG>("[bioseq] index: async persist publish failed row={} index={} dsm={} shared_queue_available={}",
                        static_cast<unsigned long long>(job.target.sequence_rowno),
                        job.index_name,
                        static_cast<unsigned>(job.dsm),
                        g_shared != nullptr);
                if (job.file_backed) {
                    remove_temp_file_noexcept(job.temp_file_path);
                }
                release_pinned_dsm(job.dsm);
                job.dsm = DSM_HANDLE_INVALID;
            }
            job.permit.release();
        }
    } else {
        for (PendingPersistJob& job : jobs) {
            if (!job.published) {
                if (job.file_backed) {
                    remove_temp_file_noexcept(job.temp_file_path);
                }
                release_pinned_dsm(job.dsm);
            }
            job.permit.release();
            job.dsm = DSM_HANDLE_INVALID;
        }
    }
    jobs.clear();
}

void ensure_persist_xact_callback_registered() {
    if (g_persist_xact_callback_registered) {
        return;
    }
    invoke_async_mps_stage_pg_leaf(
        AsyncMpsStagePgFailure::register_xact_callback,
        [] {
            RegisterXactCallback(
                bioseq_persist_xact_callback, nullptr);
        });
    // RegisterXactCallback can ERROR while allocating its callback node. Set
    // the completion bit only after the bridged primitive returned, so retry
    // never duplicates an already-installed callback.
    g_persist_xact_callback_registered = true;
}

[[nodiscard]] bool async_persist_queue_available() noexcept {
    return g_shared != nullptr;
}

[[nodiscard]] bool stage_async_index_persist(
        const PersistTarget& target,
        std::span<const PersistChunk> chunks) {
    (void)target;
    (void)chunks;
    record_prohibited_legacy_persist_rejection("stage_dsm_chunks");
    return false;
#if 0  // Deleted with the identity-queue replacement; retained only for archaeology.
    const std::uint64_t stage_start_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    if (!bioseq::settings::index_async_persist || chunks.empty() || target.index_name.empty()) {
        return false;
    }
    if (!async_persist_queue_available()) {
        bioseq_log<DBG>("[bioseq] index: async persist skipped row={} index={} because shared supervisor queue is unavailable",
                static_cast<unsigned long long>(target.sequence_rowno),
                target.index_name);
        return false;
    }
    ensure_persist_xact_callback_registered();
    std::uint64_t bytes_total = sizeof(IndexPersistDsmHeader);
    bytes_total += target.index_name.size();
    bytes_total = align_up_u64(bytes_total, alignof(IndexPersistDsmChunk));
    const std::uint64_t directory_offset = bytes_total;
    bytes_total += static_cast<std::uint64_t>(chunks.size()) * sizeof(IndexPersistDsmChunk);
    bytes_total = align_up_u64(bytes_total, alignof(std::max_align_t));
    const std::uint64_t payload_offset = bytes_total;
    for (const PersistChunk& chunk : chunks) {
        bytes_total = align_up_u64(bytes_total, alignof(std::max_align_t));
        bytes_total += chunk.bytes.size();
    }
    if (bytes_total == 0 || bytes_total > static_cast<std::uint64_t>((std::numeric_limits<Size>::max)())) {
        return false;
    }

    auto permit =
        bioseq::parallelism::MemoryAdmissionController::instance()
            .try_reserve(bytes_total);
    if (!permit) {
        bioseq_log<DBG>("[bioseq] index: async persist staging skipped by memory admission (bytes={})",
                static_cast<unsigned long long>(bytes_total));
        return false;
    }

    dsm_segment* seg = dsm_create(static_cast<Size>(bytes_total), DSM_CREATE_NULL_IF_MAXSEGMENTS);
    if (seg == nullptr) {
        return false;
    }
    char* base = static_cast<char*>(dsm_segment_address(seg));
    IndexPersistDsmHeader header{
        kIndexPersistDsmMagic,
        kIndexPersistDsmVersion,
        static_cast<std::uint32_t>(chunks.size()),
        static_cast<std::uint32_t>(target.index_name.size()),
        bytes_total,
    };
    std::memcpy(base, &header, sizeof(header));
    std::memcpy(base + sizeof(header), target.index_name.data(), target.index_name.size());
    auto* directory = reinterpret_cast<IndexPersistDsmChunk*>(base + directory_offset);
    std::uint64_t cursor = payload_offset;
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        const PersistChunk& chunk = chunks[i];
        cursor = align_up_u64(cursor, alignof(std::max_align_t));
        directory[i] = IndexPersistDsmChunk{
            .sub_key = static_cast<std::int64_t>(chunk.sub_key),
            .chunk_no = chunk.chunk_no,
            .reserved = 0,
            .offset = cursor,
            .size = static_cast<std::uint64_t>(chunk.bytes.size()),
        };
        if (!chunk.bytes.empty()) {
            std::memcpy(base + cursor, chunk.bytes.data(), chunk.bytes.size());
        }
        cursor += chunk.bytes.size();
    }
    dsm_pin_segment(seg);
    const dsm_handle handle = dsm_segment_handle(seg);
    dsm_detach(seg);
    const std::uint64_t dsm_ready_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());

    PendingPersistJob job{};
    job.target = target;
    job.index_name.assign(target.index_name.begin(), target.index_name.end());
    job.target.index_name = job.index_name;
    job.dsm = handle;
    job.payload_bytes = bytes_total;
    job.payload_kind = PersistPayloadKind::prohibited_legacy_dsm;
    job.permit = std::move(*permit);
    pending_persist_jobs().push_back(std::move(job));
    const std::uint64_t queued_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    bioseq_log<DBG>("[bioseq] index: staged async persist job row={} index={} chunks={} bytes={} dsm_copy_ns={} total_stage_ns={}",
            static_cast<unsigned long long>(target.sequence_rowno),
            pending_persist_jobs().back().index_name,
            chunks.size(),
            static_cast<unsigned long long>(bytes_total),
            static_cast<unsigned long long>(dsm_ready_ns - stage_start_ns),
            static_cast<unsigned long long>(queued_ns - stage_start_ns));
    return true;
#endif
}

[[nodiscard]] bool stage_async_index_persist_file(
        const PersistTarget& target,
        std::string temp_file_path,
        std::uint64_t file_bytes,
        std::uint64_t chunk_count) {
    (void)target;
    (void)file_bytes;
    (void)chunk_count;
    remove_temp_file_noexcept(temp_file_path);
    record_prohibited_legacy_persist_rejection("stage_external_file");
    return false;
#if 0  // Deleted with the identity-queue replacement; retained only for archaeology.
    const std::uint64_t stage_start_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    if (!bioseq::settings::index_async_persist
        || target.index_name.empty()
        || temp_file_path.empty()
        || chunk_count == 0) {
        remove_temp_file_noexcept(temp_file_path);
        return false;
    }
    if (!async_persist_queue_available()) {
        bioseq_log<DBG>("[bioseq] index: file async persist skipped row={} index={} chunks={} file_bytes={} because shared supervisor queue is unavailable",
                static_cast<unsigned long long>(target.sequence_rowno),
                target.index_name,
                static_cast<unsigned long long>(chunk_count),
                static_cast<unsigned long long>(file_bytes));
        remove_temp_file_noexcept(temp_file_path);
        return false;
    }
    ensure_persist_xact_callback_registered();

    std::uint64_t bytes_total = sizeof(IndexPersistFileDsmHeader);
    bytes_total += target.index_name.size();
    bytes_total += temp_file_path.size();
    if (bytes_total == 0
        || bytes_total
            > static_cast<std::uint64_t>((std::numeric_limits<Size>::max)())) {
        remove_temp_file_noexcept(temp_file_path);
        return false;
    }

    dsm_segment* seg = dsm_create(
        static_cast<Size>(bytes_total),
        DSM_CREATE_NULL_IF_MAXSEGMENTS);
    if (seg == nullptr) {
        remove_temp_file_noexcept(temp_file_path);
        return false;
    }
    char* base = static_cast<char*>(dsm_segment_address(seg));
    IndexPersistFileDsmHeader header{
        kIndexPersistFileDsmMagic,
        kIndexPersistFileDsmVersion,
        static_cast<std::uint32_t>(target.index_name.size()),
        static_cast<std::uint32_t>(temp_file_path.size()),
        file_bytes,
        chunk_count,
    };
    std::memcpy(base, &header, sizeof(header));
    std::memcpy(
        base + sizeof(header),
        target.index_name.data(),
        target.index_name.size());
    std::memcpy(
        base + sizeof(header) + target.index_name.size(),
        temp_file_path.data(),
        temp_file_path.size());
    dsm_pin_segment(seg);
    const dsm_handle handle = dsm_segment_handle(seg);
    dsm_detach(seg);

    PendingPersistJob job{};
    job.target = target;
    job.index_name.assign(target.index_name.begin(), target.index_name.end());
    job.target.index_name = job.index_name;
    job.temp_file_path = std::move(temp_file_path);
    job.dsm = handle;
    job.payload_bytes = file_bytes;
    job.file_backed = true;
    pending_persist_jobs().push_back(std::move(job));
    const std::uint64_t queued_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    bioseq_log<DBG>("[bioseq] index: staged file async persist job row={} index={} chunks={} file_bytes={} meta_bytes={} total_stage_ns={}",
            static_cast<unsigned long long>(target.sequence_rowno),
            pending_persist_jobs().back().index_name,
            static_cast<unsigned long long>(chunk_count),
            static_cast<unsigned long long>(file_bytes),
            static_cast<unsigned long long>(bytes_total),
            static_cast<unsigned long long>(queued_ns - stage_start_ns));
    return true;
#endif
}

[[nodiscard]] bool stage_async_mps_build(
        const PersistTarget& target,
        const bioseq::storage::seq_locator& locator,
        std::uint8_t alphabet_code,
        std::uint8_t storage_unit_byte_width,
        std::uint16_t last_fragment_padding,
        std::int32_t pair_sort_impl,
        std::int32_t pipeline_mode,
        std::int32_t max_concurrency,
        std::int32_t index_memory_cap_mb,
        std::int32_t index_sketch_backend,
        int requested_provider_wire_value) {  // DOC0345 Patch K / DOC0383 P5
    const auto requested_provider =
        decode_index_payload_storage_backend(
            requested_provider_wire_value);
    if (!requested_provider.has_value()) {
        bioseq::pg_utils::invoke_pg_as_cpp(
            [requested_provider_wire_value] {
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("invalid asynchronous index payload provider"),
                     errdetail("provider=%d",
                        requested_provider_wire_value)));
            });
        pg_unreachable();
    }
    // The only asynchronous producer currently constructs MPS.  Preserve the
    // caller's snapshotted provider identity, but reject mapped_file before
    // callback registration, DSM creation, catalog access, or file activity.
    if (*requested_provider
            == IndexPayloadStorageBackend::mapped_file) {
        bioseq::pg_utils::invoke_pg_as_cpp([] {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("mapped_file payload storage does not support asynchronous MPS persistence"),
                 errdetail("mapped_file is restricted to Format-5 minimizers_per_fragment generations")));
        });
        pg_unreachable();
    }
    const std::uint64_t stage_start_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    if (!bioseq::settings::index_async_persist
        || !OidIsValid(target.db_oid)
        || !OidIsValid(target.user_relid)
        || !OidIsValid(target.main_relfilenode)
        || target.column_attno <= 0
        || target.index_name.empty()
        || locator.version != bioseq::storage::kLocatorVersion
        || locator.source_epoch == 0
        || locator.num_fragments == 0
        || locator.total_bp_size == 0
        || target.bucket_idx != locator.bucket_idx
        || target.shard_idx != locator.shard_idx) {
        return false;
    }
    if (!async_persist_queue_available()) {
        bioseq_log<DBG>("[bioseq] index: async mps build skipped row={} index={} target_bp={} fragments={} because shared supervisor queue is unavailable",
                static_cast<unsigned long long>(target.sequence_rowno),
                target.index_name,
                static_cast<unsigned long long>(locator.total_bp_size),
                static_cast<unsigned long long>(locator.num_fragments));
        return false;
    }
    if (target.index_name.size()
            > (std::numeric_limits<std::uint32_t>::max)()
        || target.index_name.size()
            > (std::numeric_limits<std::uint64_t>::max)()
                - sizeof(IndexBuildMpsDsmHeader)) {
        return false;
    }
    const std::uint64_t bytes_total = sizeof(IndexBuildMpsDsmHeader)
        + static_cast<std::uint64_t>(target.index_name.size());
    if (bytes_total == 0u
        || bytes_total
            > static_cast<std::uint64_t>((std::numeric_limits<Size>::max)())) {
        return false;
    }
    ensure_persist_xact_callback_registered();

    struct StagedDsmState {
        dsm_segment* attached = nullptr;
        dsm_handle handle = DSM_HANDLE_INVALID;
        bool unpin_cleanup_required = false;
    } dsm;
    const auto cleanup_dsm_noexcept = [&dsm]() noexcept {
        if (dsm.attached != nullptr) {
            dsm_segment* attached = std::exchange(dsm.attached, nullptr);
            try {
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [attached] { dsm_detach(attached); });
            } catch (...) {
                // The mapping remains ResourceOwner-owned. Preserve the
                // staging error; transaction abort performs final cleanup.
            }
        }
        if (dsm.unpin_cleanup_required
            && dsm.handle != DSM_HANDLE_INVALID) {
            const dsm_handle handle = std::exchange(
                dsm.handle, DSM_HANDLE_INVALID);
            dsm.unpin_cleanup_required = false;
            try {
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [handle] { dsm_unpin_segment(handle); });
            } catch (...) {
                // dsm_unpin_segment is non-throwing in supported PostgreSQL,
                // but never replace the original staging failure if that
                // contract changes.
            }
        }
    };

    try {
        dsm.attached = invoke_async_mps_stage_pg_leaf(
            AsyncMpsStagePgFailure::dsm_create,
            [bytes_total] {
                return dsm_create(
                    static_cast<Size>(bytes_total),
                    DSM_CREATE_NULL_IF_MAXSEGMENTS);
            });
        if (dsm.attached == nullptr) {
            return false;
        }

        // This DSM is metadata-only: locator/source identity, selected build
        // policy and the identifier. It never contains sequence, minimizer,
        // anchor, alignment or persisted-index bytes.
        char* base = static_cast<char*>(
            dsm_segment_address(dsm.attached));
        const IndexBuildMpsDsmHeader header{
            .magic = kIndexBuildMpsDsmMagic,
            .version = kIndexBuildMpsDsmVersion,
            .index_name_len = static_cast<std::uint32_t>(
                target.index_name.size()),
            .alphabet_code = alphabet_code,
            .storage_unit_byte_width = storage_unit_byte_width,
            .last_fragment_padding = last_fragment_padding,
            .pair_sort_impl = pair_sort_impl,
            .pipeline_mode = pipeline_mode,
            .max_concurrency = max_concurrency,
            .index_memory_cap_mb = index_memory_cap_mb,
            .index_sketch_backend = index_sketch_backend,
            .requested_provider =
                index_payload_storage_backend_wire_value(
                    *requested_provider),
            .locator = locator,
        };
        std::memcpy(base, &header, sizeof(header));
        std::memcpy(
            base + sizeof(header),
            target.index_name.data(),
            target.index_name.size());

        dsm.handle = dsm_segment_handle(dsm.attached);
        // Arm cleanup before entering the leaf. This covers both the injected
        // pre-call failure and any future PostgreSQL implementation that could
        // ERROR after making the pin effective; an unpin of an unpinned handle
        // is itself bridged and swallowed while preserving the first error.
        dsm.unpin_cleanup_required = true;
        invoke_async_mps_stage_pg_leaf(
            AsyncMpsStagePgFailure::dsm_pin,
            [&dsm] { dsm_pin_segment(dsm.attached); });
        invoke_async_mps_stage_pg_leaf(
            AsyncMpsStagePgFailure::dsm_detach,
            [&dsm] { dsm_detach(dsm.attached); });
        dsm.attached = nullptr;

        PendingPersistJob job{};
        job.target = target;
        job.target.index_name = {};
        job.index_name.assign(
            target.index_name.begin(), target.index_name.end());
        job.dsm = dsm.handle;
        job.payload_bytes = bytes_total;
        job.payload_kind = PersistPayloadKind::mps_build_identity;
        job.requested_provider = *requested_provider;
        pending_persist_jobs().push_back(std::move(job));
        // The pending transaction list now owns the pin. Disarm cleanup only
        // after vector insertion (and every possible allocation) succeeded.
        dsm.handle = DSM_HANDLE_INVALID;
        dsm.unpin_cleanup_required = false;
    } catch (...) {
        cleanup_dsm_noexcept();
        throw;
    }
    const std::uint64_t queued_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    bioseq_log<DBG>("[bioseq] index: staged async mps build job row={} index={} target_bp={} fragments={} mode={} max_concurrency={} index_memory_cap_mb={} meta_bytes={} total_stage_ns={}",
            static_cast<unsigned long long>(target.sequence_rowno),
            pending_persist_jobs().back().index_name,
            static_cast<unsigned long long>(locator.total_bp_size),
            static_cast<unsigned long long>(locator.num_fragments),
            pipeline_mode,
            max_concurrency,
            index_memory_cap_mb,
            static_cast<unsigned long long>(bytes_total),
            static_cast<unsigned long long>(queued_ns - stage_start_ns));
    return true;
}

void install_scheduler_shmem_hooks() {
    prev_shmem_request_hook = shmem_request_hook;
    shmem_request_hook = scheduler_shmem_request;
    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = scheduler_shmem_startup;
}

void register_scheduler_supervisor() {
    BackgroundWorker worker;
    std::memset(&worker, 0, sizeof(worker));
    pg_snprintf(worker.bgw_name, BGW_MAXLEN, "bioseq scheduler supervisor");
    pg_snprintf(worker.bgw_type, BGW_MAXLEN, "bioseq scheduler");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = 5;
    pg_snprintf(worker.bgw_library_name, BGW_MAXLEN, "bioseq");
    pg_snprintf(worker.bgw_function_name, BGW_MAXLEN,
                "bioseq_scheduler_supervisor_main");
    worker.bgw_main_arg = Datum{0};
    RegisterBackgroundWorker(&worker);
    bioseq_log<DBG>("[bioseq] scheduler: shared supervisor background worker registered");
}

[[nodiscard]] bioseq::gpu::vulkan::ShaderCacheErrc classify_shader_cache_error(
        std::string_view error) noexcept {
    using bioseq::gpu::vulkan::ShaderCacheErrc;
    if (error.find("metal unavailable") != std::string_view::npos
        || error.find("No such file") != std::string_view::npos) {
        return ShaderCacheErrc::NoToolchain;
    }
    if (error.find("glslang") != std::string_view::npos
        || error.find("shader compile") != std::string_view::npos) {
        return ShaderCacheErrc::Glslang;
    }
    if (error.find("SPIR-V to MSL") != std::string_view::npos) {
        return ShaderCacheErrc::SpirvToMsl;
    }
    if (error.find("spawn failed") != std::string_view::npos) {
        return ShaderCacheErrc::MetalSpawn;
    }
    if (error.find("exited with status") != std::string_view::npos
        || error.find("terminated by signal") != std::string_view::npos) {
        return ShaderCacheErrc::MetalExit;
    }
    if (error.find("ABI") != std::string_view::npos
        || error.find("layout mismatch") != std::string_view::npos
        || error.find("stride") != std::string_view::npos) {
        return ShaderCacheErrc::AbiScan;
    }
    return ShaderCacheErrc::MetaIo;
}

[[nodiscard]] std::filesystem::path runtime_cache_root() {
    const char* data_dir = DataDir != nullptr ? DataDir : ".";
    return std::filesystem::path(data_dir) / "bioseq_runtime_cache";
}

[[nodiscard]] std::filesystem::path runtime_cache_dir_from_digest(
        std::string_view digest) {
    return runtime_cache_root() / std::string(digest);
}

[[nodiscard]] std::expected<void, std::string>
publish_runtime_cache_env(const std::filesystem::path& cache_dir) {
    // No exact MoltenVK command-source AOT capture exists yet. Never publish
    // the old no-op placeholder as immutable command evidence; Runtime's
    // recognized command-source interpose must fail closed instead.
    if (::unsetenv("BIOSEQ_MOLTENVK_COMMAND_METALLIB") != 0) {
        const int saved_errno = errno;
        return std::unexpected(fmt::format(
            "unsetenv BIOSEQ_MOLTENVK_COMMAND_METALLIB failed: errno {} ({})",
            saved_errno, std::strerror(saved_errno)));
    }
    if (::setenv(
            "BIOSEQ_MOLTENVK_BINARY_ARCHIVE_DIR", cache_dir.c_str(), 1) != 0) {
        const int saved_errno = errno;
        return std::unexpected(fmt::format(
            "setenv BIOSEQ_MOLTENVK_BINARY_ARCHIVE_DIR failed: errno {} ({})",
            saved_errno, std::strerror(saved_errno)));
    }
    const std::filesystem::path sealed_manifest =
        bioseq::gpu::vulkan::sealed_shader_cache_manifest_path(cache_dir);
    if (::setenv(
            "BIOSEQ_GPU_SEALED_CACHE_MANIFEST", sealed_manifest.c_str(), 1) != 0) {
        const int saved_errno = errno;
        return std::unexpected(fmt::format(
            "setenv BIOSEQ_GPU_SEALED_CACHE_MANIFEST failed: errno {} ({})",
            saved_errno, std::strerror(saved_errno)));
    }
    return {};
}

[[nodiscard]] std::string current_runtime_cache_digest() {
    if (g_shared == nullptr) {
        return {};
    }
    char local[sizeof(g_shared->cache_digest_hex)]{};
    std::memcpy(local, g_shared->cache_digest_hex, sizeof(local));
    local[sizeof(local) - 1] = '\0';
    return std::string(local);
}

[[nodiscard]] bioseq::gpu::vulkan::ShaderCacheBuildInputs conservative_shader_inputs() {
    bioseq::gpu::vulkan::ShaderCacheBuildInputs inputs{};
    inputs.variants.max_workgroup_invocations = 256u;
    inputs.variants.max_shared_memory_bytes = 32u * 1024u;
    inputs.variants.default_window_size =
        static_cast<std::uint32_t>(std::max(1, bioseq::aligner::settings::window_size));
    inputs.msl = bioseq::gpu::vulkan::MslVersion{3u, 2u, 0u};
    inputs.moltenvk_build = "conservative-no-device-probe";
    inputs.device_uuid = "conservative-no-device-probe";
    return inputs;
}

[[nodiscard]] std::expected<
    std::vector<bioseq::gpu::vulkan::ShaderVariantKey>, std::string>
exact_aot_shader_keys(
        const bioseq::gpu::vulkan::ShaderCacheBuildInputs& inputs) {
    std::vector<bioseq::gpu::vulkan::ShaderVariantKey> keys;
    bioseq::gpu::vulkan::for_each_static_shader_variant(
        inputs.variants,
        [&keys](const bioseq::gpu::vulkan::ShaderVariantKey& key) {
            keys.push_back(key);
        });
    return bioseq::gpu::vulkan::canonical_sealed_shader_cache_keys(keys);
}

[[nodiscard]] bioseq::gpu::vulkan::SealedShaderCacheProcessStore&
process_shader_seed_store() noexcept {
    static bioseq::gpu::vulkan::SealedShaderCacheProcessStore store;
    return store;
}

[[nodiscard]] std::expected<std::shared_ptr<
    const bioseq::gpu::vulkan::SealedShaderCacheSeedImage>, std::string>
load_process_shader_seed_once(
    const std::filesystem::path& cache_dir,
    std::string_view digest,
    std::span<const bioseq::gpu::vulkan::ShaderVariantKey> keys)
{
    return process_shader_seed_store().load_once(cache_dir, digest, keys);
}

[[nodiscard]] std::shared_ptr<
    const bioseq::gpu::vulkan::SealedShaderCacheSeedImage>
process_shader_seed_snapshot() {
    return process_shader_seed_store().snapshot();
}

[[nodiscard]] std::expected<void, std::string>
install_verified_runtime_archive_configuration(
    std::shared_ptr<
        const bioseq::gpu::vulkan::SealedShaderCacheSeedImage> seed)
{
    if (!seed || !seed->accepted()) {
        return std::unexpected(
            "runtime archive configuration has no accepted process seed image");
    }
    auto configuration = bioseq::gpu::vulkan::
        make_runtime_metal_archive_configuration_image(std::move(seed));
    if (!configuration) {
        return std::unexpected(fmt::format(
            "prepare admitted Metal archive configuration: {}",
            configuration.error()));
    }
    auto installed =
        bioseq::gpu::vulkan::install_runtime_metal_archive_configuration(
            std::move(*configuration));
    if (!installed) {
        return std::unexpected(fmt::format(
            "install sealed Metal archive configuration: {}",
            installed.error()));
    }
    return {};
}

[[nodiscard]] std::expected<void, std::string> run_shader_cache_aot_phase() {
#if !defined(__APPLE__)
    return {};
#else
    const auto inputs = conservative_shader_inputs();
    const std::string digest = bioseq::gpu::vulkan::shader_cache_digest_hex(inputs);
    const std::filesystem::path cache_dir = runtime_cache_dir_from_digest(digest);
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    if (ec) {
        return std::unexpected(fmt::format(
            "create shader cache dir {} failed: {}",
            cache_dir.string(), ec.message()));
    }
    auto keys = exact_aot_shader_keys(inputs);
    if (!keys) {
        return std::unexpected(fmt::format(
            "canonicalize exact AOT shader keys: {}", keys.error()));
    }
    const std::filesystem::path manifest_path =
        bioseq::gpu::vulkan::sealed_shader_cache_manifest_path(cache_dir);
    const bool manifest_exists = std::filesystem::exists(manifest_path, ec);
    if (ec) {
        return std::unexpected(fmt::format(
            "stat sealed shader cache manifest {} failed: {}",
            manifest_path.string(), ec.message()));
    }
    if (manifest_exists) {
        auto sealed =
            bioseq::gpu::vulkan::load_validated_sealed_shader_cache_manifest(
                cache_dir, digest, *keys);
        if (!sealed) {
            return std::unexpected(fmt::format(
                "existing sealed shader cache is invalid and will not be repaired: {}",
                sealed.error()));
        }
        if (g_shared != nullptr) {
            std::snprintf(
                g_shared->cache_digest_hex, sizeof(g_shared->cache_digest_hex),
                "%s", digest.c_str());
        }
        return {};
    }

    bioseq::parallelism::GlobalPool::instance().configure(
        bioseq::parallelism::PoolConfig{
            .max_concurrency = effective_max_concurrency(
                static_cast<unsigned>(std::max(0, bioseq::scheduler::settings::max_concurrency))),
            .mode = bioseq::parallelism::mode_from_int(
                static_cast<int>(bioseq::scheduler::settings::pipeline_concurrency_mode.value())),
        });

    constexpr std::uint64_t kPeakCompileBytes = 256ull << 20;
    std::size_t cursor = 0;
    while (cursor < keys->size()) {
        const unsigned requested = static_cast<unsigned>(
            std::min<std::size_t>(4, keys->size() - cursor));
        auto wave = bioseq::parallelism::try_reserve_budgeted_wave(
            requested, kPeakCompileBytes);
        if (!wave.admitted) {
            return std::unexpected("shader cache AOT memory admission failed");
        }
        const std::size_t begin = cursor;
        const std::size_t end = std::min<std::size_t>(
            keys->size(), cursor + wave.fanout);
        std::mutex error_mu;
        std::string first_error;
        bioseq::parallelism::GlobalPool::instance().parallel_for(
            begin, end, 1,
            [&](std::size_t b, std::size_t e) {
                for (std::size_t i = b; i < e; ++i) {
                    auto compiled =
                        bioseq::gpu::vulkan::ensure_compiled_for_sealed_seed(
                        cache_dir, (*keys)[i], inputs.msl);
                    if (!compiled) {
                        std::lock_guard<std::mutex> guard(error_mu);
                        if (first_error.empty()) {
                            first_error = fmt::format(
                                "{}: {}",
                                bioseq::gpu::vulkan::cache_file_stem((*keys)[i]),
                                compiled.error());
                        }
                    }
                }
            });
        if (!first_error.empty()) {
            return std::unexpected(first_error);
        }
        cursor = end;
    }
    auto sealed = bioseq::gpu::vulkan::seal_shader_cache_manifest(
        cache_dir, digest, *keys);
    if (!sealed) {
        return std::unexpected(fmt::format(
            "seal exact shader AOT seed: {}", sealed.error()));
    }
    if (g_shared != nullptr) {
        std::snprintf(
            g_shared->cache_digest_hex, sizeof(g_shared->cache_digest_hex),
            "%s", digest.c_str());
    }
    return {};
#endif
}

[[nodiscard]] std::expected<void, std::string> load_bioseq_runtime() {
    return run_shader_cache_aot_phase();
}

void publish_runtime_init_result(std::expected<void, std::string> result) noexcept {
    if (g_shared == nullptr) {
        return;
    }
    if (result) {
        pg_atomic_write_u32(
            &g_shared->runtime_init_errc,
            static_cast<std::uint32_t>(bioseq::gpu::vulkan::ShaderCacheErrc::Ok));
    } else {
        g_shared->cache_digest_hex[0] = '\0';
        pg_atomic_write_u32(
            &g_shared->runtime_init_errc,
            static_cast<std::uint32_t>(classify_shader_cache_error(result.error())));
        bioseq_log<WRN, PG>("[bioseq] runtime init failed: {}", result.error());
    }
    pg_write_barrier();
    pg_atomic_write_u32(
        &g_shared->runtime_init_state,
        static_cast<std::uint32_t>(
            result ? RuntimeInitState::Ready : RuntimeInitState::Failed));
    ConditionVariableBroadcast(&g_shared->runtime_init_cv);
}

[[nodiscard]] bool procno_in_range(std::uint32_t procno) noexcept {
    return procno != static_cast<std::uint32_t>(INVALID_PROC_NUMBER)
        && ProcGlobal != nullptr
        && procno < static_cast<std::uint32_t>(ProcGlobal->allProcCount);
}

[[nodiscard]] std::expected<std::filesystem::path, std::string>
prepare_backend_local_shader_cache_env();

void wake_supervisor_if_known() {
    if (g_shared == nullptr) {
        return;
    }
    const std::uint32_t procno = pg_atomic_read_u32(&g_shared->supervisor_pgprocno);
    if (procno_in_range(procno)) {
		Latch* latch =
			&GetPGProcByNumber(static_cast<ProcNumber>(procno))->procLatch;
		bioseq::pg_utils::invoke_pg_as_cpp(
			[latch] { SetLatch(latch); });
    }
}

inline void pg_check_for_interrupts_as_cpp() {
	bioseq::pg_utils::invoke_pg_as_cpp(
		[] { CHECK_FOR_INTERRUPTS(); });
}

inline void pg_condition_variable_prepare_to_sleep_as_cpp(
		ConditionVariable* condition) {
	bioseq::pg_utils::invoke_pg_as_cpp(
		[condition] { ConditionVariablePrepareToSleep(condition); });
}

inline void pg_condition_variable_cancel_sleep_as_cpp() {
	bioseq::pg_utils::invoke_pg_as_cpp(
		[] { ConditionVariableCancelSleep(); });
}

inline void pg_condition_variable_timed_sleep_as_cpp(
		ConditionVariable* condition,
		const long timeout_ms) {
	bioseq::pg_utils::invoke_pg_as_cpp([condition, timeout_ms] {
		(void)ConditionVariableTimedSleep(
			condition, timeout_ms, PG_WAIT_EXTENSION);
	});
}

[[noreturn]] void report_runtime_init_unavailable(std::string_view detail) {
	// Own the rendered detail in the ordinary C++ frame.  The PG leaf captures
	// only stable pointers and therefore cannot longjmp across this string.
	const std::string diagnostic{detail};
	const char* diagnostic_cstr = diagnostic.c_str();
	bioseq::pg_utils::invoke_pg_as_cpp([diagnostic_cstr] {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg_internal("%s", "Vulkan aligner execution failed"),
			 errdetail_internal("%s", diagnostic_cstr),
			 errhint("%s",
				"Selected PostgreSQL Vulkan execution requires a successful "
				"bioseq GPU runtime initialization; use the backend-local helper "
				"path or the shared-preload supervisor.")));
	});
	pg_unreachable();
}

void wait_for_runtime_ready() {
    static bool backend_runtime_ready = false;
    if (backend_runtime_ready) {
        return;
    }
    // PostgreSQL single-user mode has no postmaster child and launches no
    // registered background workers.  Waiting on preload-owned shared state
    // would therefore time out forever.  The process is not forked, so the
    // normal in-process Vulkan/compiler path is safe and needs no supervisor
    // archive handoff.
    if (is_standalone_backend_process()) {
        backend_runtime_ready = true;
        return;
    }
    if (g_shared == nullptr) {
        auto prepared = prepare_backend_local_shader_cache_env();
        if (!prepared) {
            report_runtime_init_unavailable(fmt::format(
                "backend-local GPU runtime preparation failed: {}",
                prepared.error()));
        }
        backend_runtime_ready = true;
        return;
    }

    constexpr std::uint64_t kDeadlineNs = 120ull * 1'000'000'000ull;
    const std::uint64_t deadline =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns()) + kDeadlineNs;
	bool sleep_prepared = false;
	auto cancel_sleep = [&sleep_prepared] {
		if (sleep_prepared) {
			pg_condition_variable_cancel_sleep_as_cpp();
			sleep_prepared = false;
		}
	};
	try {
		pg_condition_variable_prepare_to_sleep_as_cpp(
			&g_shared->runtime_init_cv);
		sleep_prepared = true;
		for (;;) {
			pg_check_for_interrupts_as_cpp();
			const auto state = static_cast<RuntimeInitState>(
				pg_atomic_read_u32(&g_shared->runtime_init_state));
			pg_read_barrier();
			if (state == RuntimeInitState::Ready) {
				const std::string digest = current_runtime_cache_digest();
#if defined(__APPLE__)
				if (digest.empty()) {
					cancel_sleep();
					report_runtime_init_unavailable(
						"load_bioseq_runtime reported ready without a shader-cache digest");
				}
#endif
				if (!digest.empty()) {
					const std::filesystem::path cache_dir =
						runtime_cache_dir_from_digest(digest);
					const auto inputs = conservative_shader_inputs();
					auto keys = exact_aot_shader_keys(inputs);
					if (!keys) {
						const std::string diagnostic = fmt::format(
							"canonicalize exact AOT shader keys: {}",
							keys.error());
						cancel_sleep();
						report_runtime_init_unavailable(diagnostic);
					}
					auto seed = load_process_shader_seed_once(
						cache_dir, digest, *keys);
					if (!seed) {
						const std::string diagnostic = fmt::format(
							"supervisor shader cache seal validation failed: {}",
							seed.error());
						cancel_sleep();
						report_runtime_init_unavailable(diagnostic);
					}
					if (auto installed =
							install_verified_runtime_archive_configuration(
								*seed);
						!installed) {
						const std::string diagnostic = installed.error();
						cancel_sleep();
						report_runtime_init_unavailable(diagnostic);
					}
					if (auto published = publish_runtime_cache_env(cache_dir);
						!published) {
						const std::string diagnostic = published.error();
						cancel_sleep();
						report_runtime_init_unavailable(diagnostic);
					}
				}
				backend_runtime_ready = true;
				cancel_sleep();
				return;
			}
			if (state == RuntimeInitState::Failed) {
				const std::uint32_t errc =
					pg_atomic_read_u32(&g_shared->runtime_init_errc);
				const std::string diagnostic = fmt::format(
					"load_bioseq_runtime failed (errc {}); GPU unavailable", errc);
				cancel_sleep();
				report_runtime_init_unavailable(diagnostic);
			}
			if (static_cast<std::uint64_t>(bioseq::instr::clock::now_ns())
				> deadline) {
				cancel_sleep();
				report_runtime_init_unavailable(
					"load_bioseq_runtime unavailable (supervisor down / timeout)");
			}
			pg_condition_variable_timed_sleep_as_cpp(
				&g_shared->runtime_init_cv, 1000L);
		}
	} catch (...) {
		if (sleep_prepared) {
			try {
				cancel_sleep();
			} catch (...) {
				// Preserve the initiating PostgreSQL/C++ error.  A valid prepared
				// condition-variable cancellation is not expected to fail.
			}
		}
		throw;
	}
}

struct CompileClaim {
    std::size_t slot_index = 0;
    std::uint32_t generation = 0;
    bioseq::gpu::vulkan::ShaderVariantKey key{};
};

[[nodiscard]] std::optional<CompileClaim> try_claim_shader_compile_slot() noexcept {
    if (g_shared == nullptr) {
        return std::nullopt;
    }
    for (std::size_t slot_index = 0;
         slot_index < BioseqSchedulerShared::kCompileQueueSize;
         ++slot_index) {
        ShaderCompileSlot& slot = g_shared->compile_queue[slot_index];
        std::uint32_t expected =
            static_cast<std::uint32_t>(CompileSlotState::Requested);
        if (!pg_atomic_compare_exchange_u32(
                &slot.state, &expected,
                static_cast<std::uint32_t>(CompileSlotState::Compiling))) {
            continue;
        }
        pg_read_barrier();
        return CompileClaim{
            .slot_index = slot_index,
            .generation = pg_atomic_read_u32(&slot.generation),
            .key = slot.key,
        };
    }
    return std::nullopt;
}

void release_shader_compile_slot(std::size_t slot_index) noexcept;
[[nodiscard]] bool try_release_completed_shader_compile_slot(
	std::size_t slot_index,
	CompileSlotState completed_state) noexcept;

void finish_shader_compile_slot(
        const CompileClaim& claim,
        bioseq::gpu::vulkan::ShaderCacheErrc errc,
        bool ok) noexcept {
    if (g_shared == nullptr
        || claim.slot_index >= BioseqSchedulerShared::kCompileQueueSize) {
        return;
    }
    ShaderCompileSlot& slot = g_shared->compile_queue[claim.slot_index];
    slot.errc = static_cast<std::uint32_t>(errc);
    pg_atomic_fetch_add_u32(&slot.generation, 1);
    pg_write_barrier();
    pg_atomic_write_u32(
        &slot.state,
        static_cast<std::uint32_t>(
            ok ? CompileSlotState::DoneOk : CompileSlotState::DoneErr));
	pg_read_barrier();
	// A cancelled waiter marks its request orphaned.  Whichever side observes
	// the cancellation after the Done publication owns returning the slot to the
	// queue; this prevents cancellation from permanently exhausting the queue.
	if (slot.requester_pid == 0) {
		(void)try_release_completed_shader_compile_slot(
			claim.slot_index,
			ok ? CompileSlotState::DoneOk : CompileSlotState::DoneErr);
	}
}

void release_shader_compile_slot(std::size_t slot_index) noexcept {
    if (g_shared == nullptr || slot_index >= BioseqSchedulerShared::kCompileQueueSize) {
        return;
    }
    ShaderCompileSlot& slot = g_shared->compile_queue[slot_index];
    slot.key = bioseq::gpu::vulkan::ShaderVariantKey{};
    slot.requester_pid = 0;
    slot.errc = static_cast<std::uint32_t>(bioseq::gpu::vulkan::ShaderCacheErrc::Ok);
    pg_atomic_fetch_add_u32(&slot.generation, 1);
    pg_write_barrier();
    pg_atomic_write_u32(
        &slot.state,
        static_cast<std::uint32_t>(CompileSlotState::Free));
}

[[nodiscard]] bool try_release_completed_shader_compile_slot(
		const std::size_t slot_index,
		const CompileSlotState completed_state) noexcept {
	if (g_shared == nullptr
		|| slot_index >= BioseqSchedulerShared::kCompileQueueSize
		|| (completed_state != CompileSlotState::DoneOk
			&& completed_state != CompileSlotState::DoneErr)) {
		return false;
	}
	ShaderCompileSlot& slot = g_shared->compile_queue[slot_index];
	std::uint32_t expected = static_cast<std::uint32_t>(completed_state);
	if (!pg_atomic_compare_exchange_u32(
			&slot.state,
			&expected,
			static_cast<std::uint32_t>(CompileSlotState::Filling))) {
		return false;
	}
	release_shader_compile_slot(slot_index);
	return true;
}

void abandon_shader_compile_request(
		const std::size_t slot_index,
		const std::uint32_t generation) noexcept {
	if (g_shared == nullptr
		|| slot_index >= BioseqSchedulerShared::kCompileQueueSize) {
		return;
	}
	ShaderCompileSlot& slot = g_shared->compile_queue[slot_index];
	const std::uint32_t observed_generation =
		pg_atomic_read_u32(&slot.generation);
	const auto observed_state = static_cast<CompileSlotState>(
		pg_atomic_read_u32(&slot.state));
	std::uint32_t expected = static_cast<std::uint32_t>(
		CompileSlotState::Requested);
	if (observed_generation == generation
		&& observed_state == CompileSlotState::Requested
		&& pg_atomic_compare_exchange_u32(
			&slot.state,
			&expected,
			static_cast<std::uint32_t>(CompileSlotState::Filling))) {
		release_shader_compile_slot(slot_index);
		return;
	}

	if (observed_generation == generation
		&& observed_state == CompileSlotState::Compiling) {
		slot.requester_pid = 0;
		pg_write_barrier();
		const auto observed = static_cast<CompileSlotState>(
			pg_atomic_read_u32(&slot.state));
		if (observed == CompileSlotState::DoneOk
			|| observed == CompileSlotState::DoneErr) {
			(void)try_release_completed_shader_compile_slot(
				slot_index, observed);
		}
		return;
	}
	if (observed_state == CompileSlotState::DoneOk
		|| observed_state == CompileSlotState::DoneErr) {
		slot.requester_pid = 0;
		pg_write_barrier();
		(void)try_release_completed_shader_compile_slot(
			slot_index, observed_state);
	}
}

[[nodiscard]] std::expected<std::pair<std::size_t, std::uint32_t>, std::string>
publish_shader_compile_request(
        const bioseq::gpu::vulkan::ShaderVariantKey& key) {
    if (g_shared == nullptr) {
        return std::unexpected("shader compile queue unavailable");
    }
    for (std::size_t slot_index = 0;
         slot_index < BioseqSchedulerShared::kCompileQueueSize;
         ++slot_index) {
        ShaderCompileSlot& slot = g_shared->compile_queue[slot_index];
        std::uint32_t expected =
            static_cast<std::uint32_t>(CompileSlotState::Free);
        if (!pg_atomic_compare_exchange_u32(
                &slot.state, &expected,
                static_cast<std::uint32_t>(CompileSlotState::Filling))) {
            continue;
        }
        slot.key = key;
        slot.requester_pid = static_cast<std::uint32_t>(MyProcPid);
        slot.errc = static_cast<std::uint32_t>(bioseq::gpu::vulkan::ShaderCacheErrc::Ok);
        const std::uint32_t generation =
            pg_atomic_fetch_add_u32(&slot.generation, 1) + 1u;
        pg_write_barrier();
        pg_atomic_write_u32(
            &slot.state,
            static_cast<std::uint32_t>(CompileSlotState::Requested));
		try {
			wake_supervisor_if_known();
		} catch (...) {
			abandon_shader_compile_request(slot_index, generation);
			throw;
		}
        return std::pair<std::size_t, std::uint32_t>{slot_index, generation};
    }
    return std::unexpected("shader compile queue full");
}

struct PgSupervisorCompileService {
    [[nodiscard]] std::expected<void, std::string> ensure_variant_compiled(
            bioseq::gpu::vulkan::ShaderVariantKey key) {
        auto published = publish_shader_compile_request(key);
        if (!published) {
            return std::unexpected(published.error());
        }
        const auto [slot_index, generation] = *published;
        constexpr std::uint64_t kDeadlineNs = 120ull * 1'000'000'000ull;
        const std::uint64_t deadline =
            static_cast<std::uint64_t>(bioseq::instr::clock::now_ns()) + kDeadlineNs;
		bool sleep_prepared = false;
		auto cancel_sleep = [&sleep_prepared] {
			if (sleep_prepared) {
				pg_condition_variable_cancel_sleep_as_cpp();
				sleep_prepared = false;
			}
		};
		try {
			pg_condition_variable_prepare_to_sleep_as_cpp(
				&g_shared->compile_done_cv);
			sleep_prepared = true;
			for (;;) {
				pg_check_for_interrupts_as_cpp();
				ShaderCompileSlot& slot = g_shared->compile_queue[slot_index];
				const auto state = static_cast<CompileSlotState>(
					pg_atomic_read_u32(&slot.state));
				const std::uint32_t observed_gen =
					pg_atomic_read_u32(&slot.generation);
				if ((state == CompileSlotState::DoneOk
						|| state == CompileSlotState::DoneErr)
					&& observed_gen != generation) {
					pg_read_barrier();
					const std::uint32_t errc = slot.errc;
					cancel_sleep();
					(void)try_release_completed_shader_compile_slot(
						slot_index, state);
					if (state == CompileSlotState::DoneOk) {
						return {};
					}
					return std::unexpected(fmt::format(
						"shader compile failed for {} (errc {})",
						bioseq::gpu::vulkan::cache_file_stem(key), errc));
				}
				if (static_cast<std::uint64_t>(bioseq::instr::clock::now_ns())
					> deadline) {
					const std::string diagnostic = fmt::format(
						"shader compile timed out for {}",
						bioseq::gpu::vulkan::cache_file_stem(key));
					cancel_sleep();
					abandon_shader_compile_request(slot_index, generation);
					return std::unexpected(diagnostic);
				}
				pg_condition_variable_timed_sleep_as_cpp(
					&g_shared->compile_done_cv, 1000L);
			}
		} catch (...) {
			if (sleep_prepared) {
				try {
					cancel_sleep();
				} catch (...) {
				}
			}
			abandon_shader_compile_request(slot_index, generation);
			throw;
		}
    }
};

[[nodiscard]] std::expected<std::filesystem::path, std::string>
prepare_backend_local_shader_cache_env() {
    const auto inputs = conservative_shader_inputs();
    const std::string digest = bioseq::gpu::vulkan::shader_cache_digest_hex(inputs);
    const std::filesystem::path cache_dir = runtime_cache_dir_from_digest(digest);
    auto keys = exact_aot_shader_keys(inputs);
    if (!keys) {
        return std::unexpected(fmt::format(
            "canonicalize exact AOT shader keys: {}", keys.error()));
    }
    const std::filesystem::path manifest_path =
        bioseq::gpu::vulkan::sealed_shader_cache_manifest_path(cache_dir);
    std::error_code ec;
    const bool manifest_exists = std::filesystem::exists(manifest_path, ec);
    if (ec) {
        return std::unexpected(fmt::format(
            "stat sealed shader cache manifest {} failed: {}",
            manifest_path.string(), ec.message()));
    }
    if (manifest_exists) {
        auto seed = load_process_shader_seed_once(cache_dir, digest, *keys);
        if (!seed) {
            return std::unexpected(fmt::format(
                "existing sealed shader cache is invalid and will not be repaired: {}",
                seed.error()));
        }
        if (auto installed = install_verified_runtime_archive_configuration(
                *seed);
            !installed) {
            return std::unexpected(installed.error());
        }
        if (auto published = publish_runtime_cache_env(cache_dir); !published) {
            return std::unexpected(published.error());
        }
        return cache_dir;
    }

    for (const auto& key : *keys) {
        auto compiled = bioseq::gpu::vulkan::ensure_compiled_for_sealed_seed(
            cache_dir, key, inputs.msl);
        if (!compiled) {
            return std::unexpected(fmt::format(
                "backend-local exact AOT compile {}: {}",
                bioseq::gpu::vulkan::cache_file_stem(key),
                compiled.error()));
        }
    }
    auto sealed = bioseq::gpu::vulkan::seal_shader_cache_manifest(
        cache_dir, digest, *keys);
    if (!sealed) {
        return std::unexpected(fmt::format(
            "seal backend-local exact shader AOT seed: {}", sealed.error()));
    }
    auto seed = load_process_shader_seed_once(cache_dir, digest, *keys);
    if (!seed) {
        return std::unexpected(fmt::format(
            "load newly sealed backend-local shader seed: {}", seed.error()));
    }
    if (auto installed = install_verified_runtime_archive_configuration(
            *seed);
        !installed) {
        return std::unexpected(installed.error());
    }
    if (auto published = publish_runtime_cache_env(cache_dir); !published) {
        return std::unexpected(published.error());
    }
    return cache_dir;
}

struct PgBackendLocalCompileService {
    std::filesystem::path cache_dir;
    bioseq::gpu::vulkan::MslVersion msl;

    [[nodiscard]] std::expected<void, std::string> ensure_variant_compiled(
            bioseq::gpu::vulkan::ShaderVariantKey key) {
        return bioseq::gpu::vulkan::ensure_compiled(cache_dir, key, msl);
    }
};

[[nodiscard]] std::string warmup_backend_local_shader_cache_report() {
    std::ostringstream out;
#if !defined(__APPLE__)
    out << "pid=" << static_cast<long>(MyProcPid)
        << " platform=non-apple\n"
        << "RESULT: ok variants=0 failed=0\n";
    return out.str();
#else
    const auto inputs = conservative_shader_inputs();
    auto prepared = prepare_backend_local_shader_cache_env();
    if (!prepared) {
        out << "pid=" << static_cast<long>(MyProcPid) << '\n'
            << "RESULT: failed\n"
            << "error=" << prepared.error() << '\n';
        return out.str();
    }

    const std::filesystem::path& cache_dir = *prepared;
    out << "pid=" << static_cast<long>(MyProcPid)
        << " supervisor=" << (g_shared != nullptr ? 1 : 0)
        << " cache_dir=" << cache_dir.string() << '\n';
    if (const char* env = std::getenv("BIOSEQ_MOLTENVK_COMMAND_METALLIB");
        env != nullptr) {
        out << "env BIOSEQ_MOLTENVK_COMMAND_METALLIB=" << env << '\n';
    }
    if (const char* env = std::getenv("BIOSEQ_MOLTENVK_BINARY_ARCHIVE_DIR");
        env != nullptr) {
        out << "env BIOSEQ_MOLTENVK_BINARY_ARCHIVE_DIR=" << env << '\n';
    }

    auto keys = exact_aot_shader_keys(inputs);
    if (!keys) {
        out << "RESULT: failed\n"
            << "error=canonicalize exact AOT shader keys: "
            << keys.error() << '\n';
        return out.str();
    }
    const std::string digest = cache_dir.filename().string();
    const auto seed = process_shader_seed_snapshot();
    if (!seed || !seed->accepted()
        || seed->manifest().cache_digest != digest
        || seed->manifest().keys != *keys) {
        out << "RESULT: failed\n"
            << "error=process-owned shader seed unavailable after preparation\n";
        return out.str();
    }
    const auto& sealed = seed->manifest();
    out << "sealed_manifest="
        << bioseq::gpu::vulkan::sealed_shader_cache_manifest_path(cache_dir).string()
        << " seed_bytes=" << sealed.seed_size
        << " manifest_checksum=" << sealed.manifest_checksum
        << " seed_load_count=" << seed->receipt().load_count()
        << " seed_load_ns=" << seed->receipt().load_ns() << '\n';

    std::size_t failed = 0;
    for (const auto& key : *keys) {
        const std::string stem = bioseq::gpu::vulkan::cache_file_stem(key);
        auto compatible =
            bioseq::gpu::vulkan::sealed_shader_cache_key_view(*seed, key);
        if (!compatible) {
            ++failed;
            out << "variant " << stem
                << " kind=" << key.kind
                << " tile=" << key.tile_size
                << " local=" << key.local_size
                << " max_span=" << key.max_span
                << " status=failed error=" << compatible.error() << '\n';
            continue;
        }
        const auto key_iterator = std::find(
            sealed.keys.begin(), sealed.keys.end(), key);
        const std::uint32_t key_index = static_cast<std::uint32_t>(
            std::distance(sealed.keys.begin(), key_iterator));
        const auto* metallib = bioseq::gpu::vulkan::
            sealed_shader_cache_detail::find_artifact_image(
                seed->artifacts(),
                bioseq::gpu::vulkan::SealedShaderCacheArtifactKind::
                    shader_metallib,
                key_index, stem + ".metallib");
        const auto* meta = bioseq::gpu::vulkan::
            sealed_shader_cache_detail::find_artifact_image(
                seed->artifacts(),
                bioseq::gpu::vulkan::SealedShaderCacheArtifactKind::shader_meta,
                key_index, stem + ".meta");
        std::ostringstream archive_bytes;
        bool first_archive = true;
        for (const auto& archive : seed->artifacts()) {
            if (archive.descriptor.key_index != key_index
                || archive.descriptor.kind
                    != bioseq::gpu::vulkan::SealedShaderCacheArtifactKind::
                        metal_binary_archive) {
                continue;
            }
            if (!first_archive) {
                archive_bytes << ',';
            }
            first_archive = false;
            archive_bytes << archive.descriptor.filename << ':'
                << archive.bytes.size();
        }
        out << "variant " << stem
            << " kind=" << key.kind
            << " tile=" << key.tile_size
            << " local=" << key.local_size
            << " max_span=" << key.max_span
            << " status=ok"
            << " metallib_bytes=" << (metallib ? metallib->bytes.size() : 0u)
            << " meta_bytes=" << (meta ? meta->bytes.size() : 0u)
            << " archive_bytes=" << archive_bytes.str()
            << '\n';
    }
    out << "RESULT: " << (failed == 0 ? "ok" : "failed")
        << " variants=" << keys->size()
        << " failed=" << failed << '\n';
    return out.str();
#endif
}

[[nodiscard]] std::expected<void, std::string>
pg_shader_provider_prepare_runtime() {
    if (g_shared != nullptr) {
        wait_for_runtime_ready();
        return {};
    }
    auto prepared = prepare_backend_local_shader_cache_env();
    if (!prepared) {
        return std::unexpected(prepared.error());
    }
    return {};
}

[[nodiscard]] std::expected<bioseq::gpu::vulkan::ShaderModuleLoad, std::string>
pg_shader_provider_load_module(
        VkDevice device,
        const bioseq::gpu::vulkan::ShaderVariantKey& key,
    PFN_vkCreateShaderModule create_shader_module) {
    const auto inputs = conservative_shader_inputs();
    auto keys = exact_aot_shader_keys(inputs);
    if (!keys) {
        return std::unexpected(fmt::format(
            "canonicalize exact AOT shader keys: {}", keys.error()));
    }
    if (g_shared != nullptr) {
        wait_for_runtime_ready();
    } else if (!process_shader_seed_snapshot()) {
        auto prepared = prepare_backend_local_shader_cache_env();
        if (!prepared) return std::unexpected(prepared.error());
    }
    const auto seed = process_shader_seed_snapshot();
    if (!seed || !seed->accepted()
        || seed->manifest().keys != *keys) {
        return std::unexpected(
            "process-owned sealed shader seed is unavailable or incompatible");
    }
    return bioseq::gpu::vulkan::load_sealed_cached_module(
        device, *seed, key, create_shader_module);
}

void install_pg_shader_provider_if_available() noexcept {
#if defined(__APPLE__)
    if (process_shared_preload_libraries_in_progress
        || is_standalone_backend_process()) {
        return;
    }
    static bioseq::gpu::vulkan::RuntimeShaderProvider provider{
        &pg_shader_provider_prepare_runtime,
        &pg_shader_provider_load_module};
    bioseq::gpu::vulkan::install_runtime_shader_provider(&provider);
#endif
}

/**
 * The only supported PostgreSQL entry point for constructing/accessing the
 * process-local Vulkan Runtime singleton.
 *
 * MoltenVK consumes the command-metallib and binary-archive environment when
 * the singleton creates its device/pipeline state.  Installing the PostgreSQL
 * shader provider or publishing the digest-derived cache directory afterwards
 * is too late for that backend process.  Keep this ordering in one exported
 * function so status/diagnostic SQL cannot poison a later selected GPU scan.
 *
 * Every PostgreSQL-capable operation below the API is an invoke_pg_as_cpp leaf.
 * Consequently this function may throw PostgresErrorAsCpp, but only after that
 * leaf has completed PG_END_TRY.  Callers must be ordinary C++ orchestration or
 * catch at a PostgreSQL callback/UDF boundary after abandoning their C++ state.
 */
[[nodiscard]] bioseq::gpu::vulkan::Runtime&
ensure_pg_vulkan_runtime_ready() {
    install_pg_shader_provider_if_available();
    wait_for_runtime_ready();
    return bioseq::gpu::vulkan::Runtime::instance();
}

void service_pending_shader_compiles() {
    if (g_shared == nullptr) {
        return;
    }
    const std::string digest = current_runtime_cache_digest();
    if (digest.empty()) {
        return;
    }
    const std::filesystem::path cache_dir = runtime_cache_dir_from_digest(digest);
    const auto inputs = conservative_shader_inputs();
    int done = 0;
    while (done < 4) {
        auto claim = try_claim_shader_compile_slot();
        if (!claim) {
            break;
        }
        auto compiled = bioseq::gpu::vulkan::ensure_compiled(
            cache_dir, claim->key, inputs.msl);
        finish_shader_compile_slot(
            *claim,
            compiled
                ? bioseq::gpu::vulkan::ShaderCacheErrc::Ok
                : classify_shader_cache_error(compiled.error()),
            compiled.has_value());
        ++done;
    }
    if (done > 0) {
        ConditionVariableBroadcast(&g_shared->compile_done_cv);
    }
}

[[nodiscard]] bool supervisor_available() noexcept {
    return g_shared != nullptr;
}

void backend_sync() {
    static bool exit_hook_registered = false;
    if (!exit_hook_registered) {
        before_shmem_exit(pool_shutdown_before_exit, Datum{0});
        exit_hook_registered = true;
    }

    auto& pool = bioseq::parallelism::GlobalPool::instance();
    const std::uint32_t generation = g_shared != nullptr
        ? pg_atomic_read_u32(&g_shared->cfg_generation)
        : 0;
    const unsigned configured = g_shared != nullptr
        ? pg_atomic_read_u32(&g_shared->cfg_max_concurrency)
        : static_cast<unsigned>(std::max(0, bioseq::scheduler::settings::max_concurrency));
    const auto mode = bioseq::parallelism::mode_from_int(
        static_cast<int>(bioseq::scheduler::settings::pipeline_concurrency_mode.value()));

    if (!pool.active() || pool.adopted_generation() != generation) {
        pool.configure(bioseq::parallelism::PoolConfig{
            .max_concurrency = effective_max_concurrency(configured),
            .mode = mode,
        });
        pool.set_adopted_generation(generation);
    } else {
        pool.set_mode(mode);
    }

    const std::uint64_t index_cap = bioseq::settings::index_memory_cap_mb > 0
        ? (static_cast<std::uint64_t>(bioseq::settings::index_memory_cap_mb) << 20)
        : 0;
    bioseq::parallelism::MemoryAdmissionController::instance()
        .set_index_memory_cap_bytes(index_cap);
    install_pg_shader_provider_if_available();
}

[[nodiscard]] BatchTicket begin_batch(std::int64_t queries) noexcept {
    if (queries < 0) {
        queries = 0;
    }
    BatchTicket ticket{};
    if (g_shared == nullptr) {
        ticket.local = bioseq::parallelism::GlobalPool::instance().begin_batch(queries);
        ticket.queries = queries;
        return ticket;
    }

    PgSchedulerStats& stats = g_shared->stats;
    ticket.shared = true;
    ticket.generation = pg_atomic_read_u32(&g_shared->cfg_generation);
    ticket.queries = queries;
    const std::uint64_t seq = pg_atomic_fetch_add_u64(&stats.inflight_seq, 1);
    ticket.slot = static_cast<std::size_t>(seq % PgSchedulerStats::kInflightSlots);
    const std::uint64_t now = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    pg_atomic_write_u64(&stats.inflight_submit_ns[ticket.slot], now);
    pg_atomic_write_u32(&stats.inflight_generation[ticket.slot], ticket.generation);
    pg_atomic_fetch_add_u64(&stats.batches_scheduled, 1);
    pg_atomic_fetch_add_u64(&stats.batches_in_flight, 1);
    pg_atomic_fetch_add_u64(&stats.queries_waiting, static_cast<std::uint64_t>(queries));
    return ticket;
}

void end_batch(BatchTicket ticket, bool completed) noexcept {
    if (!ticket.shared) {
        if (completed) {
            bioseq::parallelism::GlobalPool::instance().end_batch(ticket.local);
        } else {
            bioseq::parallelism::GlobalPool::instance().cancel_batch(ticket.local);
        }
        return;
    }
    if (g_shared == nullptr
        || pg_atomic_read_u32(&g_shared->cfg_generation) != ticket.generation) {
        return;
    }
    PgSchedulerStats& stats = g_shared->stats;
    pg_atomic_fetch_sub_u64(&stats.batches_in_flight, 1);
    pg_atomic_fetch_sub_u64(&stats.queries_waiting, static_cast<std::uint64_t>(ticket.queries));
    if (completed) {
        pg_atomic_fetch_add_u64(&stats.batches_processed, 1);
        pg_atomic_fetch_add_u64(&stats.queries_processed, static_cast<std::uint64_t>(ticket.queries));
    }
    if (ticket.slot < PgSchedulerStats::kInflightSlots) {
        pg_atomic_write_u64(&stats.inflight_submit_ns[ticket.slot], 0);
        pg_atomic_write_u32(&stats.inflight_generation[ticket.slot], 0);
    }
}

void restart_cluster() {
    backend_sync();
    const std::uint64_t now = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    if (g_shared != nullptr) {
        pg_atomic_write_u32(
            &g_shared->cfg_max_concurrency,
            static_cast<std::uint32_t>(std::max(0, bioseq::scheduler::settings::max_concurrency)));
        pg_atomic_fetch_add_u32(&g_shared->cfg_generation, 1);
        reset_shared_stats(g_shared->stats, now);
    }
    auto& pool = bioseq::parallelism::GlobalPool::instance();
    pool.restart(bioseq::parallelism::PoolConfig{
        .max_concurrency = effective_max_concurrency(
            static_cast<unsigned>(std::max(0, bioseq::scheduler::settings::max_concurrency))),
        .mode = bioseq::parallelism::mode_from_int(
            static_cast<int>(bioseq::scheduler::settings::pipeline_concurrency_mode.value())),
    });
    pool.reset_stats(now);
}

[[nodiscard]] SchedulerSnapshot snapshot() {
    backend_sync();
    auto& pool = bioseq::parallelism::GlobalPool::instance();
    SchedulerSnapshot out{};
    out.supervisor = g_shared != nullptr;
    out.config_generation = g_shared != nullptr
        ? pg_atomic_read_u32(&g_shared->cfg_generation)
        : pool.adopted_generation();
    out.mode = pool.mode();
    out.min_threads = 1;
    out.max_threads = pool.max_concurrency();
    out.current_threads = pool.live_workers();
    out.index_memory_reserved_bytes =
        bioseq::parallelism::MemoryAdmissionController::instance().reserved_bytes();

    const std::uint64_t now = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    if (g_shared != nullptr) {
        PgSchedulerStats& stats = g_shared->stats;
        out.batches_scheduled = pg_atomic_read_u64(&stats.batches_scheduled);
        out.batches_processed = pg_atomic_read_u64(&stats.batches_processed);
        out.batches_awaiting = pg_atomic_read_u64(&stats.batches_in_flight);
        out.queries_waiting = pg_atomic_read_u64(&stats.queries_waiting);
        out.queries_processed = pg_atomic_read_u64(&stats.queries_processed);
        std::uint64_t oldest = 0;
        for (std::size_t slot = 0; slot < PgSchedulerStats::kInflightSlots; ++slot) {
            const std::uint64_t submit_ns =
                pg_atomic_read_u64(&stats.inflight_submit_ns[slot]);
            if (submit_ns != 0 && (oldest == 0 || submit_ns < oldest)) {
                oldest = submit_ns;
            }
        }
        out.oldest_waiting_batch_age_secs = oldest == 0
            ? 0.0
            : static_cast<double>(now - oldest) / 1'000'000'000.0;
        const std::uint64_t start_ns = pg_atomic_read_u64(&stats.rate_window_start_ns);
        const std::uint64_t start_queries = pg_atomic_read_u64(&stats.rate_window_start_queries);
        const double elapsed = start_ns == 0
            ? 0.0
            : static_cast<double>(now - start_ns) / 1'000'000'000.0;
        if (elapsed > 0.0 && out.queries_processed >= start_queries) {
            out.queries_per_sec =
                static_cast<double>(out.queries_processed - start_queries) / elapsed;
        }
        const std::uint64_t restart_ns = pg_atomic_read_u64(&stats.restart_ns);
        out.uptime_secs = restart_ns == 0
            ? 0.0
            : static_cast<double>(now - restart_ns) / 1'000'000'000.0;
        for (PgPersistQueueSlot& slot : g_shared->persist_queue) {
            const std::uint32_t state = pg_atomic_read_u32(&slot.state);
            if (state == 1 || state == 2) {
                ++out.async_persist_jobs_waiting;
            }
        }
        return out;
    }

    SchedulerStats& stats = pool.stats();
    out.batches_scheduled = stats.batches_scheduled.load(std::memory_order_acquire);
    out.batches_processed = stats.batches_processed.load(std::memory_order_acquire);
    out.batches_awaiting = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, stats.batches_in_flight.load(std::memory_order_acquire)));
    out.queries_waiting = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, stats.queries_waiting.load(std::memory_order_acquire)));
    out.queries_processed = stats.queries_processed.load(std::memory_order_acquire);
    const std::uint64_t restart_ns = stats.restart_ns.load(std::memory_order_acquire);
    out.uptime_secs = restart_ns == 0
        ? 0.0
        : static_cast<double>(now - restart_ns) / 1'000'000'000.0;
    return out;
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

template <class F>
auto invoke_async_mps_worker_pg_leaf(
        AsyncMpsWorkerPgFailure point,
        F&& function)
    -> std::invoke_result_t<std::remove_reference_t<F>&> {
    record_async_mps_worker_pg_leaf_call(point);
    try {
        return bioseq::pg_utils::invoke_pg_as_cpp(
            std::forward<F>(function));
    } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
        record_async_mps_worker_pg_leaf_failure(point);
        throw;
    }
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

struct PersistClaim {
    std::size_t slot_index = 0;
    std::uint32_t generation = 0;
    PersistTarget target{};
    dsm_handle dsm = DSM_HANDLE_INVALID;
    std::uint64_t payload_bytes = 0;
    PersistPayloadKind payload_kind = PersistPayloadKind::none;
    IndexPayloadStorageBackend requested_provider =
        IndexPayloadStorageBackend::postgres_relation;
};

[[nodiscard]] std::optional<PersistClaim> claim_persist_job() noexcept {
    if (g_shared == nullptr) {
        return std::nullopt;
    }
    for (std::size_t slot_index = 0;
         slot_index < BioseqSchedulerShared::kPersistQueueSize;
         ++slot_index) {
        PgPersistQueueSlot& slot = g_shared->persist_queue[slot_index];
        if (pg_atomic_read_u32(&slot.state) != 1) {
            continue;
        }
        const auto payload_kind = static_cast<PersistPayloadKind>(
            slot.payload_kind);
        const auto requested_provider =
            decode_index_payload_storage_backend(
                slot.requested_provider);
        if (payload_kind != PersistPayloadKind::mps_build_identity
            || !requested_provider.has_value()) {
            static std::array<std::uint32_t,
                BioseqSchedulerShared::kPersistQueueSize>
                rejected_generations{};
            const std::uint32_t generation =
                pg_atomic_read_u32(&slot.generation);
            if (rejected_generations[slot_index] != generation) {
                rejected_generations[slot_index] = generation;
                record_prohibited_legacy_persist_rejection(
                    requested_provider.has_value()
                        ? "queue_claim" : "queue_provider_decode");
            }
            // Fail closed: do not claim and therefore never acknowledge a
            // legacy DSM/file payload. Identity-queue replacement will
            // reclaim administratively quarantined slots.
            continue;
        }
        std::uint32_t expected = 1;
        if (!pg_atomic_compare_exchange_u32(&slot.state, &expected, 2)) {
            continue;
        }
        PersistClaim claim{};
        claim.slot_index = slot_index;
        claim.generation = pg_atomic_read_u32(&slot.generation);
        claim.target.db_oid = slot.db_oid;
        claim.target.user_relid = slot.user_relid;
        claim.target.tablespace_oid = slot.tablespace_oid;
        claim.target.main_relfilenode = slot.main_relfilenode;
        claim.target.column_attno = slot.column_attno;
        claim.target.bucket_idx = slot.bucket_idx;
        claim.target.shard_idx = slot.shard_idx;
        claim.target.sequence_rowno = slot.sequence_rowno;
        claim.dsm = slot.dsm;
        claim.payload_bytes = slot.payload_bytes;
        claim.payload_kind = payload_kind;
        claim.requested_provider = *requested_provider;
        return claim;
    }
    return std::nullopt;
}

void finish_persist_claim(const PersistClaim& claim) noexcept {
    if (claim.payload_kind != PersistPayloadKind::mps_build_identity) {
        record_prohibited_legacy_persist_rejection("queue_ack");
        return;
    }
    if (g_shared == nullptr || claim.slot_index >= BioseqSchedulerShared::kPersistQueueSize) {
        release_pinned_dsm(claim.dsm);
        return;
    }
    PgPersistQueueSlot& slot = g_shared->persist_queue[claim.slot_index];
    release_pinned_dsm(claim.dsm);
    slot.dsm = DSM_HANDLE_INVALID;
    slot.payload_bytes = 0;
    slot.payload_kind = static_cast<std::uint32_t>(
        PersistPayloadKind::none);
    slot.requested_provider =
        index_payload_storage_backend_wire_value(
            IndexPayloadStorageBackend::postgres_relation);
    slot.sequence_rowno = 0;
    pg_atomic_fetch_add_u32(&slot.generation, 1);
    pg_atomic_write_u32(&slot.state, 0);
}

[[nodiscard]] bool worker_target_accepts_index_persist(
        const PersistTarget& target,
        std::string_view index_name) {
    if (!OidIsValid(target.user_relid)) {
        return false;
    }

    // Relation operations are independent leaves. A live SPI session is one
    // composite PostgreSQL operation and must stay inside one checked leaf from
    // connect through finish; restoring the caller context between its stages
    // corrupts PostgreSQL 17's SPI procedure context.
    Relation rel = invoke_async_mps_worker_pg_leaf(
        AsyncMpsWorkerPgFailure::relation_open,
        [relid = target.user_relid] {
            inject_async_mps_worker_pg_failure_leaf(
                AsyncMpsWorkerPgFailure::relation_open);
            return try_safe_relation_open(relid, AccessShareLock);
        });
    if (rel == nullptr) {
        return false;
    }

    struct RelationIdentity {
        Oid tablespace = InvalidOid;
        Oid relfilenode = InvalidOid;
    };
    static_assert(std::is_trivially_destructible_v<RelationIdentity>);
    const RelationIdentity identity = invoke_async_mps_worker_pg_leaf(
        AsyncMpsWorkerPgFailure::relation_identity,
        [rel] {
            inject_async_mps_worker_pg_failure_leaf(
                AsyncMpsWorkerPgFailure::relation_identity);
            RelationIdentity result{};
            result.tablespace = OidIsValid(rel->rd_rel->reltablespace)
                ? rel->rd_rel->reltablespace
                : MyDatabaseTableSpace;
            result.relfilenode =
                static_cast<Oid>(rel->rd_locator.relNumber);
            if (!OidIsValid(result.relfilenode)) {
                result.relfilenode = RelationGetRelid(rel);
            }
            return result;
        });
    invoke_async_mps_worker_pg_leaf(
        AsyncMpsWorkerPgFailure::relation_close,
        [rel] {
            inject_async_mps_worker_pg_failure_leaf(
                AsyncMpsWorkerPgFailure::relation_close);
            relation_close(rel, AccessShareLock);
        });
    if (identity.tablespace != target.tablespace_oid
        || identity.relfilenode != target.main_relfilenode) {
        return false;
    }

    const std::string owned_index_name{index_name};
    const char* const index_name_cstr = owned_index_name.c_str();
    static constexpr char kPolicySql[] =
        "SELECT enabled, update_on_insert "
        "FROM bioseq_internal.index_policy "
        "WHERE user_relid=$1 AND main_relfilenode=$2 "
        "AND column_attno=$3 AND index_name=$4 LIMIT 1";
    struct WorkerPolicySpiState {
        AsyncMpsWorkerPgFailure current_stage =
            AsyncMpsWorkerPgFailure::none;
        int connect_status = 0;
        int execute_status = 0;
        int finish_status = 0;
        std::uint64_t processed = 0;
        bool table_present = false;
        bool enabled = true;
        bool update_on_insert = false;
    } spi{
        .update_on_insert =
            bioseq::indexing::default_update_on_insert_contains(index_name),
    };
    try {
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&spi,
             user_relid = target.user_relid,
             main_relfilenode = target.main_relfilenode,
             column_attno = target.column_attno,
             index_name_cstr] noexcept {
            spi.current_stage = AsyncMpsWorkerPgFailure::spi_connect;
            record_async_mps_worker_pg_leaf_call(spi.current_stage);
            inject_async_mps_worker_pg_failure_leaf(spi.current_stage);
            spi.connect_status = SPI_connect();
            if (spi.connect_status != SPI_OK_CONNECT) {
                ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("asynchronous MPS worker could not connect to SPI"),
                     errdetail("SPI_connect returned status %d",
                        spi.connect_status)));
            }

            spi.current_stage = AsyncMpsWorkerPgFailure::spi_execute;
            record_async_mps_worker_pg_leaf_call(spi.current_stage);
            inject_async_mps_worker_pg_failure_leaf(spi.current_stage);
            Oid argument_types[4] = {OIDOID, OIDOID, INT2OID, TEXTOID};
            Datum arguments[4] = {
                ObjectIdGetDatum(user_relid),
                ObjectIdGetDatum(main_relfilenode),
                Int16GetDatum(column_attno),
                CStringGetTextDatum(index_name_cstr),
            };
            spi.execute_status = SPI_execute_with_args(
                kPolicySql, 4, argument_types, arguments,
                nullptr, true, 1);
            if (spi.execute_status != SPI_OK_SELECT) {
                ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("asynchronous MPS worker policy lookup failed"),
                     errdetail("SPI_execute_with_args returned status %d",
                        spi.execute_status)));
            }
            spi.processed = static_cast<std::uint64_t>(SPI_processed);
            spi.table_present = SPI_tuptable != nullptr;
            if (spi.processed > 0) {
                spi.current_stage = AsyncMpsWorkerPgFailure::spi_read;
                record_async_mps_worker_pg_leaf_call(spi.current_stage);
                inject_async_mps_worker_pg_failure_leaf(spi.current_stage);
                if (SPI_tuptable == nullptr || SPI_tuptable->vals == nullptr) {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("asynchronous MPS worker policy row is unavailable")));
                }
                bool enabled_is_null = false;
                bool update_is_null = false;
                const Datum enabled_datum = SPI_getbinval(
                    SPI_tuptable->vals[0], SPI_tuptable->tupdesc,
                    1, &enabled_is_null);
                const Datum update_datum = SPI_getbinval(
                    SPI_tuptable->vals[0], SPI_tuptable->tupdesc,
                    2, &update_is_null);
                if (enabled_is_null || update_is_null) {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("asynchronous MPS worker policy contains NULL"),
                         errdetail("enabled_is_null=%s update_on_insert_is_null=%s",
                            enabled_is_null ? "true" : "false",
                            update_is_null ? "true" : "false")));
                }
                spi.enabled = DatumGetBool(enabled_datum);
                spi.update_on_insert = DatumGetBool(update_datum);
            }

            spi.current_stage = AsyncMpsWorkerPgFailure::spi_finish;
            record_async_mps_worker_pg_leaf_call(spi.current_stage);
            inject_async_mps_worker_pg_failure_leaf(spi.current_stage);
            spi.finish_status = SPI_finish();
            if (spi.finish_status != SPI_OK_FINISH) {
                ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("asynchronous MPS worker could not finish SPI"),
                     errdetail("SPI_finish returned status %d",
                        spi.finish_status)));
            }
            spi.current_stage = AsyncMpsWorkerPgFailure::none;
        });
    } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
        record_async_mps_worker_pg_leaf_failure(spi.current_stage);
        throw;
    }
    return spi.enabled && spi.update_on_insert;
}

template <sequence_alphabet Alphabet>
struct AsyncMpsResidentTarget {
    bioseq::storage::admitted_packed_region<Alphabet> owner;
    std::uint64_t packed_bytes = 0;
};

template <sequence_alphabet Alphabet>
[[nodiscard]] std::optional<AsyncMpsResidentTarget<Alphabet>>
try_materialize_async_mps_worker_target(
        bioseq::storage::basic_lazy_sequence_view<Alphabet> seq,
        std::uint8_t requested_width,
        std::uint64_t sequence_rowno,
        std::string_view index_name) {
    auto& pool = bioseq::parallelism::GlobalPool::instance();
    if (!bioseq::parallelism::task_parallel(pool.mode())
        || pool.max_concurrency() <= 1) {
        return std::nullopt;
    }
    const auto access = bioseq::storage::plan_sequence_access(
        seq,
        bioseq::storage::sequence_access_requirement::contiguous_resident,
        0u,
        seq.bp_size(),
        requested_width);
    if (access.route
        == bioseq::storage::sequence_access_route::borrow_resident) {
        return std::nullopt;
    }
    constexpr std::uint64_t context_reserve_bytes = std::uint64_t{8} << 20;
    const std::uint64_t reserve_bytes = access.admission_bytes
            > (std::numeric_limits<std::uint64_t>::max)()
                - context_reserve_bytes
        ? (std::numeric_limits<std::uint64_t>::max)()
        : access.admission_bytes + context_reserve_bytes;
    auto& admission =
        bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t reserved_before = admission.reserved_bytes();
    auto permit = admission.try_reserve(reserve_bytes);
    if (!permit) {
        bioseq_log<DBG>(
            "[bioseq] index: async mps worker row={} index={} target_bp={} "
            "admitted_bytes={} not admitted for resident materialization; "
            "using rank stream",
            static_cast<unsigned long long>(sequence_rowno), index_name,
            static_cast<unsigned long long>(seq.bp_size()),
            static_cast<unsigned long long>(access.admission_bytes));
        return std::nullopt;
    }
    record_async_mps_materialize_admission();
    try {
        invoke_async_mps_worker_pg_leaf(
            AsyncMpsWorkerPgFailure::materialize_error_log,
            [] {
                inject_async_mps_worker_pg_failure_leaf(
                    AsyncMpsWorkerPgFailure::materialize_error_log);
            });
        invoke_async_mps_worker_pg_leaf(
            AsyncMpsWorkerPgFailure::materialize_interrupt,
            [] {
                inject_async_mps_worker_pg_failure_leaf(
                    AsyncMpsWorkerPgFailure::materialize_interrupt);
                CHECK_FOR_INTERRUPTS();
            });
        invoke_async_mps_worker_pg_leaf(
            AsyncMpsWorkerPgFailure::materialize_scanner_read,
            [] {
                inject_async_mps_worker_pg_failure_leaf(
                    AsyncMpsWorkerPgFailure::materialize_scanner_read);
            });
        auto owner = bioseq::storage::materialize_packed_region(
            seq, 0u, seq.bp_size(), requested_width, std::move(*permit));
        const auto& descriptor = owner.descriptor();
        const std::uint64_t packed_bytes =
            static_cast<std::uint64_t>(descriptor.unit_count)
            * descriptor.storage_unit_byte_width;
        bioseq_log<DBG>(
            "[bioseq] index: async mps worker row={} index={} target_bp={} "
            "packed_bytes={} materialized resident target",
            static_cast<unsigned long long>(sequence_rowno), index_name,
            static_cast<unsigned long long>(seq.bp_size()),
            static_cast<unsigned long long>(packed_bytes));
        return AsyncMpsResidentTarget<Alphabet>{
            std::move(owner), packed_bytes};
    } catch (...) {
        permit->release();
        const std::uint64_t reserved_after = admission.reserved_bytes();
        record_async_mps_materialize_failure_unwind(
            reserved_after == reserved_before);
        throw;
    }
}

void process_mps_build_claim_cpp(
        const PersistClaim& claim,
        const char* base,
        Size mapped,
        Snapshot registered_snapshot) {
    const std::uint64_t worker_start_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    const std::optional<MpsBuildMetadata> meta =
        parse_mps_build_metadata(base, mapped);
    if (!meta.has_value()) {
        bioseq_log<DBG>("[bioseq] index: discarded invalid async mps build metadata row={}",
                static_cast<unsigned long long>(claim.target.sequence_rowno));
        return;
    }
    if (meta->requested_provider != claim.requested_provider) {
        bioseq::pg_utils::invoke_pg_as_cpp([] {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("asynchronous MPS provider identity disagrees across queue and DSM")));
        });
    }
    if (meta->requested_provider
            != IndexPayloadStorageBackend::postgres_relation) {
        bioseq::pg_utils::invoke_pg_as_cpp([] {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("mapped_file payload storage does not support MPS persistence"),
                 errdetail("the queued job is rejected without provider fallback")));
        });
    }

    PersistTarget target = claim.target;
    target.index_name = meta->index_name;
    if (!worker_target_accepts_index_persist(target, meta->index_name)) {
        bioseq_log<DBG>("[bioseq] index: discarded stale async mps build job row={} index={}",
                static_cast<unsigned long long>(target.sequence_rowno),
                meta->index_name);
        return;
    }

    MemoryContext work = nullptr;
    bioseq::storage::seq_caches* caches = nullptr;

    const auto previous_sort_override =
        bioseq::indexing::detail::current_pair_run_sort_override();
    bool sort_impl_changed = false;
    const auto worker_mode =
        bioseq::parallelism::mode_from_int(meta->pipeline_mode);
    const unsigned worker_concurrency = effective_max_concurrency(
        static_cast<unsigned>(std::max(0, meta->max_concurrency)));
    const std::uint64_t worker_index_cap_bytes =
        meta->index_memory_cap_mb > 0
            ? (static_cast<std::uint64_t>(meta->index_memory_cap_mb) << 20)
            : 0;

    bool built = false;
    bool lazy_worker_shareable = false;
    bioseq_log<DBG>("[bioseq] index: async mps build worker starting row={} index={} target_bp={} mode={} max_concurrency={} index_memory_cap_mb={} sort_impl={}",
            static_cast<unsigned long long>(target.sequence_rowno),
            meta->index_name,
            static_cast<unsigned long long>(meta->locator.total_bp_size),
            std::string_view(bioseq::parallelism::to_string(worker_mode)),
            static_cast<unsigned long long>(worker_concurrency),
            meta->index_memory_cap_mb,
            meta->pair_sort_impl);
    // DOC0345 Patch K — the foreground recompute path chose the backend; the
    // async worker's (pre-existing) storage-dispatch carries it into the
    // per-storage `build`/`rebuild` call below.
    const auto worker_backend =
        static_cast<bioseq::indexing::MinimizerSketchBackend>(
            meta->index_sketch_backend);
    bioseq_log<DBG>(
        "[bioseq] index: async mps build worker backend={}",
        bioseq::indexing::minimizer_sketch_backend_name(worker_backend));
    const auto abandon_pg_resources_noexcept = [&]() noexcept {
        if (caches != nullptr) {
            // PostgreSQL owns every relation/read-state after an ERROR. Null
            // those borrowed handles, but still run the vector destructor so
            // its ordinary heap allocation cannot leak until backend exit.
            for (auto& [key, entry] : caches->entries) {
                (void)key;
                entry.rel = nullptr;
                entry.write_state = nullptr;
                entry.read_state = nullptr;
                entry.read_snapshot_source = nullptr;
                entry.pending_bytes_delta = 0;
                entry.pending_total_payload_delta = 0;
            }
            caches->entries.clear();
            caches->~seq_caches();
            caches = nullptr;
        }
        // The transaction owns the palloc block and child context.
        work = nullptr;
    };
    try {
        work = bioseq::pg_utils::invoke_pg_as_cpp([] {
            return AllocSetContextCreate(
                CurrentMemoryContext,
                "bioseq_async_mps_build",
                ALLOCSET_DEFAULT_SIZES);
        });
        caches = bioseq::pg_utils::invoke_pg_as_cpp(
            [work] { return bioseq::storage::alloc_seq_caches(work); });
        caches->side = bioseq::storage::seq_caches::Side::Read;
        caches->lock_mode = AccessShareLock;
        caches->read_state_mctx = work;
        caches->main_database_oid = target.db_oid;
        caches->main_tablespace_oid = target.tablespace_oid;
        caches->main_relfilenode = target.main_relfilenode;

        bioseq::indexing::detail::set_pair_run_sort_impl(
            static_cast<bioseq::indexing::detail::PairRunSortImpl>(
                meta->pair_sort_impl));
        sort_impl_changed = true;
        bioseq::parallelism::GlobalPool::instance().configure(
            bioseq::parallelism::PoolConfig{
                .max_concurrency = worker_concurrency,
                .mode = worker_mode,
            });
        bioseq::parallelism::MemoryAdmissionController::instance()
            .set_index_memory_cap_bytes(worker_index_cap_bytes);

        if (worker_backend
            == bioseq::indexing::MinimizerSketchBackend::PaperNtHashSimd) {
            bioseq::pg_utils::invoke_pg_as_cpp([] {
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("asynchronous paper_nthash_simd persistence requires the scheme-tagged READY-generation format"),
                     errdetail("legacy MPS payloads do not record a minimizer-scheme identity; the queued build is rejected without fallback")));
            });
        }
        bioseq::pg_types::seq::dispatch_instance(
            meta->alphabet_code,
            meta->storage_unit_byte_width,
            [&]<class S>() {
                auto store = bioseq::storage::indexing::open_fragment_storage(
                    target.user_relid,
                    target.db_oid,
                    target.tablespace_oid,
                    target.main_relfilenode,
                    target.column_attno,
                    meta->index_name,
                    meta->locator,
                    target.sequence_rowno,
                    caches,
                    work);
                try {
                    const bioseq::storage::postgres_lazy_sequence_binding<S>
                        source_binding{
                            target.db_oid, target.tablespace_oid,
                            target.main_relfilenode, target.column_attno,
                            meta->locator, meta->last_fragment_padding,
                            registered_snapshot, caches, work};
                    bioseq::storage::with_lazy_postgres<S>(
                        source_binding,
                        [&lazy_worker_shareable, meta, &target, &store,
                         worker_backend](auto source_view) {
                            const bool pg_lazy_worker_shareable =
                                bioseq::storage::lazy_is_worker_shareable(
                                    source_view);
                            lazy_worker_shareable = pg_lazy_worker_shareable;
                            if (!pg_lazy_worker_shareable) {
                                if (auto resident =
                                        try_materialize_async_mps_worker_target(
                                            source_view,
                                            meta->storage_unit_byte_width,
                                            target.sequence_rowno,
                                            meta->index_name);
                                    resident.has_value()) {
                                    const auto resident_view =
                                        resident->owner.view();
                                    lazy_worker_shareable =
                                        bioseq::storage::lazy_is_worker_shareable(
                                            resident_view);
                                    bioseq_log<DBG>("[bioseq] index: async mps worker row={} index={} "
                                            "using resident target packed_bytes={} "
                                            "lazy_worker_shareable={}",
                                            static_cast<unsigned long long>(
                                                target.sequence_rowno),
                                            meta->index_name,
                                            static_cast<unsigned long long>(
                                                resident->packed_bytes),
                                            lazy_worker_shareable);
                                    bioseq::indexing::MinimizerSequenceMergedIndex<
                                        bioseq::storage::indexing::IndexStorageFragmentPg>::rebuild(
                                            store, resident_view, worker_backend);
                                    return;
                                }
                            }
                            bioseq_log<DBG>("[bioseq] index: async mps worker row={} index={} "
                                    "using scanner_pg target lazy_worker_shareable={}",
                                    static_cast<unsigned long long>(
                                        target.sequence_rowno),
                                    meta->index_name,
                                    lazy_worker_shareable);
                            bioseq::indexing::MinimizerSequenceMergedIndex<
                                bioseq::storage::indexing::IndexStorageFragmentPg>::rebuild(
                                    store, source_view, worker_backend);
                        });
                    store.close_checked();
                } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
                    store.abandon_after_pg_error_noexcept();
                    throw;
                } catch (...) {
                    try {
                        store.close_checked();
                    } catch (...) {
                        store.abandon_after_pg_error_noexcept();
                    }
                    throw;
                }
            });
        built = true;
        bioseq::pg_utils::invoke_pg_as_cpp(
            [caches] { caches->close_all(/*commit=*/false); });
        bioseq::storage::seq_caches* finished_caches =
            std::exchange(caches, nullptr);
        bioseq::pg_utils::invoke_pg_as_cpp(
            [finished_caches] {
                bioseq::storage::destroy_seq_caches(finished_caches);
            });
        MemoryContext finished_work = std::exchange(work, nullptr);
        bioseq::pg_utils::invoke_pg_as_cpp(
            [finished_work] { MemoryContextDelete(finished_work); });
        bioseq::indexing::detail::restore_pair_run_sort_override(
            previous_sort_override);
        sort_impl_changed = false;
    } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
        if (sort_impl_changed) {
            bioseq::indexing::detail::restore_pair_run_sort_override(
                previous_sort_override);
            sort_impl_changed = false;
        }
        abandon_pg_resources_noexcept();
        throw;
    } catch (...) {
        if (sort_impl_changed) {
            bioseq::indexing::detail::restore_pair_run_sort_override(
                previous_sort_override);
            sort_impl_changed = false;
        }
        abandon_pg_resources_noexcept();
        throw;
    }

    if (built) {
        bioseq::storage::indexing::
            mark_serialized_index_write_successful_pg(
                target.user_relid,
                target.main_relfilenode,
                target.column_attno,
                meta->index_name);
    }

    const std::uint64_t worker_end_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    const auto stats = bioseq::storage::indexing::g_last_mps_build_stats;
    publish_async_mps_build_stats(
        stats,
        worker_end_ns - worker_start_ns,
        static_cast<std::uint64_t>(worker_mode),
        static_cast<std::uint64_t>(worker_concurrency),
        static_cast<std::uint64_t>(std::max(0, meta->index_memory_cap_mb)),
        lazy_worker_shareable);
    bioseq_log<DBG>("[bioseq] index: async mps build worker built row={} index={} target_bp={} worker_build_ns={} mode={} max_concurrency={} lazy_worker_shareable={} runs={} resident_runs={} file_runs={} resident_peak_bytes={} spilled_run_bytes={} postgres_temp_file_bytes={} merge_passes={} position_spill_bytes={}",
            static_cast<unsigned long long>(target.sequence_rowno),
            meta->index_name,
            static_cast<unsigned long long>(meta->locator.total_bp_size),
            static_cast<unsigned long long>(worker_end_ns - worker_start_ns),
            std::string_view(bioseq::parallelism::to_string(worker_mode)),
            static_cast<unsigned long long>(worker_concurrency),
            lazy_worker_shareable,
            static_cast<unsigned long long>(stats.total_run_count),
            static_cast<unsigned long long>(stats.resident_runs),
            static_cast<unsigned long long>(stats.file_runs),
            static_cast<unsigned long long>(stats.resident_peak_bytes),
            static_cast<unsigned long long>(stats.spilled_run_bytes),
            static_cast<unsigned long long>(stats.postgres_temp_file_bytes),
            static_cast<unsigned long long>(stats.merge_pass_count),
            static_cast<unsigned long long>(stats.position_spill_bytes));
}

// Trivial PostgreSQL ABI seam. The implementation above has already unwound
// every C++ owner and restored sort/admission state before this function
// rethrows the exact ErrorData or creates one terminal diagnostic.
void process_mps_build_claim(
        const PersistClaim& claim,
        const char* base,
        Size mapped,
        Snapshot registered_snapshot) {
    ErrorData* exact_error = nullptr;
    char cpp_error_detail[512] = "unknown C++ exception";
    try {
        process_mps_build_claim_cpp(
            claim, base, mapped, registered_snapshot);
        return;
    } catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
        exact_error = error.release_error_data();
    } catch (const std::exception& error) {
        strlcpy(cpp_error_detail,
            error.what() == nullptr ? "std::exception without detail"
                                    : error.what(),
            sizeof(cpp_error_detail));
    } catch (...) {
    }
    if (exact_error != nullptr) {
        ReThrowError(exact_error);
    }
    ereport(ERROR,
        (errcode(ERRCODE_INTERNAL_ERROR),
         errmsg("asynchronous MPS build raised a C++ exception"),
         errdetail_internal("%s", cpp_error_detail)));
    pg_unreachable();
}

#if 0  // Prohibited external-file persistence; identity queue replacement removes this.
void process_persist_file_claim(
        const PersistClaim& claim,
        const char* base,
        Size mapped) {
    const std::uint64_t worker_start_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    const std::optional<PersistFileMetadata> meta =
        parse_persist_file_metadata(base, mapped);
    if (!meta.has_value()) {
        bioseq_log<DBG>("[bioseq] index: discarded invalid file async persist metadata row={}",
                static_cast<unsigned long long>(claim.target.sequence_rowno));
        return;
    }

    PersistTarget target = claim.target;
    target.index_name = meta->index_name;
    if (!worker_target_accepts_index_persist(target, meta->index_name)) {
        remove_temp_file_noexcept(meta->path);
        bioseq_log<DBG>("[bioseq] index: discarded stale file async persist job row={} index={}",
                static_cast<unsigned long long>(target.sequence_rowno),
                meta->index_name);
        return;
    }

    std::ifstream input(meta->path, std::ios::binary);
    if (!input) {
        remove_temp_file_noexcept(meta->path);
        bioseq_log<DBG>("[bioseq] index: discarded missing file async persist payload row={} index={} path={}",
                static_cast<unsigned long long>(target.sequence_rowno),
                meta->index_name,
                meta->path);
        return;
    }

    std::vector<char> payload;
    std::uint64_t bytes_read = 0;
    std::uint64_t chunks_written = 0;
    for (std::uint64_t i = 0; i < meta->chunk_count; ++i) {
        PersistFileChunkHeader chunk{};
        input.read(reinterpret_cast<char*>(&chunk), sizeof(chunk));
        if (!input) {
            bioseq_log<ERR, PG>(
                   "bioseq async persist: truncated file chunk header at chunk {}",
                   static_cast<unsigned long long>(i));
        }
        bytes_read += sizeof(chunk);
        if (chunk.size > bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES) {
            bioseq_log<ERR, PG>(
                   "bioseq async persist: file chunk too large (sub_key={} chunk={} bytes={})",
                   static_cast<long long>(chunk.sub_key),
                   static_cast<unsigned>(chunk.chunk_no),
                   static_cast<unsigned long long>(chunk.size));
        }

        payload.assign(static_cast<std::size_t>(chunk.size), char{0});
        if (chunk.size != 0) {
            input.read(payload.data(), static_cast<std::streamsize>(chunk.size));
            if (!input) {
                bioseq_log<ERR, PG>(
                       "bioseq async persist: truncated file chunk payload at chunk {}",
                       static_cast<unsigned long long>(i));
            }
        }
        bytes_read += chunk.size;
        bioseq::storage::indexing::write_serialized_index_chunk_pg(
            target.user_relid,
            target.db_oid,
            target.tablespace_oid,
            target.main_relfilenode,
            target.column_attno,
            meta->index_name,
            target.bucket_idx,
            target.shard_idx,
            target.sequence_rowno,
            static_cast<bioseq::indexing::index_sub_key_t>(chunk.sub_key),
            chunk.chunk_no,
            std::span<const char>(payload.data(), payload.size()));
        ++chunks_written;
    }
    if (bytes_read != meta->file_bytes) {
        bioseq_log<ERR, PG>(
               "bioseq async persist: file byte count mismatch (expected={} actual={})",
               static_cast<unsigned long long>(meta->file_bytes),
               static_cast<unsigned long long>(bytes_read));
    }
    bioseq::storage::indexing::mark_serialized_index_write_successful_pg(
        target.user_relid,
        target.main_relfilenode,
        target.column_attno,
        meta->index_name);
    input.close();
    remove_temp_file_noexcept(meta->path);

    const std::uint64_t worker_end_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    bioseq_log<DBG>("[bioseq] index: async persist file worker wrote row={} index={} chunks={} bytes={} worker_write_ns={}",
            static_cast<unsigned long long>(target.sequence_rowno),
            meta->index_name,
            static_cast<unsigned long long>(chunks_written),
            static_cast<unsigned long long>(meta->file_bytes),
            static_cast<unsigned long long>(worker_end_ns - worker_start_ns));
}
#endif

inline void finish_persist_claim_transaction(
        Snapshot registered_snapshot) {
    PopActiveSnapshot();
    UnregisterSnapshot(registered_snapshot);
    CommitTransactionCommand();
}

#if 0  // Prohibited legacy chunk/file DSM consumer retained only for archaeology.
void process_persist_claim(const PersistClaim& claim) {
    const std::uint64_t worker_start_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    StartTransactionCommand();
    Snapshot registered_snapshot =
        RegisterSnapshot(GetTransactionSnapshot());
    PushActiveSnapshot(registered_snapshot);

    dsm_segment* seg = dsm_attach(claim.dsm);
    if (seg == nullptr) {
        finish_persist_claim_transaction(registered_snapshot);
        return;
    }
    char* base = static_cast<char*>(dsm_segment_address(seg));
    const Size mapped = dsm_segment_map_length(seg);
    if (mapped >= sizeof(std::uint32_t)) {
        std::uint32_t magic = 0;
        std::memcpy(&magic, base, sizeof(magic));
        if (magic == kIndexBuildMpsDsmMagic) {
            process_mps_build_claim(
                claim, base, mapped, registered_snapshot);
            dsm_detach(seg);
            finish_persist_claim_transaction(registered_snapshot);
            return;
        }
        if (magic == kIndexPersistFileDsmMagic) {
            process_persist_file_claim(claim, base, mapped);
            dsm_detach(seg);
            finish_persist_claim_transaction(registered_snapshot);
            return;
        }
    }
    if (mapped < sizeof(IndexPersistDsmHeader)) {
        dsm_detach(seg);
        finish_persist_claim_transaction(registered_snapshot);
        return;
    }
    IndexPersistDsmHeader header{};
    std::memcpy(&header, base, sizeof(header));
    if (header.magic != kIndexPersistDsmMagic
        || header.version != kIndexPersistDsmVersion
        || header.payload_bytes > mapped
        || header.index_name_len == 0) {
        dsm_detach(seg);
        finish_persist_claim_transaction(registered_snapshot);
        return;
    }
    const std::uint64_t directory_offset = align_up_u64(
        sizeof(IndexPersistDsmHeader) + header.index_name_len,
        alignof(IndexPersistDsmChunk));
    const std::uint64_t directory_bytes =
        static_cast<std::uint64_t>(header.chunk_count) * sizeof(IndexPersistDsmChunk);
    if (directory_offset > header.payload_bytes
        || directory_bytes > header.payload_bytes - directory_offset) {
        dsm_detach(seg);
        finish_persist_claim_transaction(registered_snapshot);
        return;
    }
    std::string index_name(
        base + sizeof(IndexPersistDsmHeader),
        base + sizeof(IndexPersistDsmHeader) + header.index_name_len);
    PersistTarget target = claim.target;
    target.index_name = index_name;
    if (!worker_target_accepts_index_persist(target, index_name)) {
        dsm_detach(seg);
        finish_persist_claim_transaction(registered_snapshot);
        bioseq_log<DBG>("[bioseq] index: discarded stale async persist job row={} index={}",
                static_cast<unsigned long long>(target.sequence_rowno),
                index_name);
        return;
    }

    const auto* directory =
        reinterpret_cast<const IndexPersistDsmChunk*>(base + directory_offset);
    for (std::uint32_t i = 0; i < header.chunk_count; ++i) {
        const IndexPersistDsmChunk& chunk = directory[i];
        if (chunk.offset > header.payload_bytes
            || chunk.size > header.payload_bytes - chunk.offset) {
            dsm_detach(seg);
            finish_persist_claim_transaction(registered_snapshot);
            return;
        }
        bioseq::storage::indexing::write_serialized_index_chunk_pg(
            target.user_relid,
            target.db_oid,
            target.tablespace_oid,
            target.main_relfilenode,
            target.column_attno,
            index_name,
            target.bucket_idx,
            target.shard_idx,
            target.sequence_rowno,
            static_cast<bioseq::indexing::index_sub_key_t>(chunk.sub_key),
            chunk.chunk_no,
            std::span<const char>(base + chunk.offset, static_cast<std::size_t>(chunk.size)));
    }
    bioseq::storage::indexing::mark_serialized_index_write_successful_pg(
        target.user_relid,
        target.main_relfilenode,
        target.column_attno,
        index_name);
    dsm_detach(seg);
    finish_persist_claim_transaction(registered_snapshot);
    const std::uint64_t worker_end_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    bioseq_log<DBG>("[bioseq] index: async persist worker wrote row={} index={} chunks={} bytes={} worker_write_ns={}",
            static_cast<unsigned long long>(target.sequence_rowno),
            index_name,
            header.chunk_count,
            static_cast<unsigned long long>(header.payload_bytes),
            static_cast<unsigned long long>(worker_end_ns - worker_start_ns));
}
#endif

void process_mps_identity_claim(const PersistClaim& claim) {
    if (claim.payload_kind != PersistPayloadKind::mps_build_identity) {
        record_prohibited_legacy_persist_rejection("worker_dispatch");
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("legacy asynchronous persistence payload is prohibited"),
                errdetail("only the temporary MPS identity build payload is accepted until the PostgreSQL identity queue replacement lands")));
    }
    if (claim.requested_provider
            != IndexPayloadStorageBackend::postgres_relation) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("mapped_file payload storage does not support MPS persistence"),
             errdetail("the requested provider was captured by the foreground job and is rejected before worker catalog access")));
    }
    StartTransactionCommand();
    Snapshot registered_snapshot =
        RegisterSnapshot(GetTransactionSnapshot());
    PushActiveSnapshot(registered_snapshot);
    dsm_segment* segment = dsm_attach(claim.dsm);
    if (segment == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("MPS identity build payload could not be attached")));
    }
    char* base = static_cast<char*>(dsm_segment_address(segment));
    const Size mapped = dsm_segment_map_length(segment);
    std::uint32_t magic = 0;
    if (mapped >= sizeof(magic)) {
        std::memcpy(&magic, base, sizeof(magic));
    }
    if (magic != kIndexBuildMpsDsmMagic) {
        dsm_detach(segment);
        record_prohibited_legacy_persist_rejection("worker_magic");
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("legacy asynchronous persistence DSM payload is prohibited"),
             errdetail("magic=%u expected_mps_identity_magic=%u",
                magic, kIndexBuildMpsDsmMagic)));
    }
    process_mps_build_claim(
        claim, base, mapped, registered_snapshot);
    dsm_detach(segment);
    finish_persist_claim_transaction(registered_snapshot);
}

inline Oid g_persist_worker_connected_db = InvalidOid;

void process_one_persist_job_if_ready() {
    auto claim = claim_persist_job();
    if (!claim) {
        return;
    }
    if (!OidIsValid(claim->target.db_oid)
        || claim->dsm == DSM_HANDLE_INVALID) {
        finish_persist_claim(*claim);
        return;
    }
    if (!OidIsValid(g_persist_worker_connected_db)) {
        BackgroundWorkerInitializeConnectionByOid(claim->target.db_oid, InvalidOid, 0);
        g_persist_worker_connected_db = claim->target.db_oid;
    } else if (g_persist_worker_connected_db != claim->target.db_oid) {
        bioseq_log<DBG>("[bioseq] index: discarded async persist job for db={} because worker is connected to db={}",
                static_cast<unsigned>(claim->target.db_oid),
                static_cast<unsigned>(g_persist_worker_connected_db));
        finish_persist_claim(*claim);
        return;
    }

    PG_TRY();
    {
        process_mps_identity_claim(*claim);
    }
    PG_CATCH();
    {
        AbortCurrentTransaction();
        FlushErrorState();
        bioseq_log<DBG>("[bioseq] index: async persist worker dropped failed job row={}",
                static_cast<unsigned long long>(claim->target.sequence_rowno));
    }
    PG_END_TRY();
    finish_persist_claim(*claim);
}

} // namespace bioseq::parallelism::pg

extern "C" PGDLLEXPORT void bioseq_scheduler_supervisor_main(Datum) {
    using namespace bioseq::parallelism::pg;

    pqsignal(SIGTERM, supervisor_sigterm);
    pqsignal(SIGHUP, supervisor_sighup);
    BackgroundWorkerUnblockSignals();

    while (g_shared == nullptr && !got_sigterm) {
        WaitLatch(MyLatch,
                  WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                  1000L,
                  PG_WAIT_EXTENSION);
        ResetLatch(MyLatch);
    }
    if (g_shared == nullptr) {
        proc_exit(0);
    }

    pg_atomic_write_u32(
        &g_shared->supervisor_pgprocno,
        static_cast<std::uint32_t>(getProcNo_compat(MyProc)));
    pg_atomic_fetch_add_u32(&g_shared->supervisor_generation, 1);
    pg_atomic_write_u32(
        &g_shared->runtime_init_state,
        static_cast<std::uint32_t>(RuntimeInitState::Pending));
    pg_atomic_fetch_add_u32(&g_shared->runtime_init_generation, 1);
    publish_runtime_init_result(load_bioseq_runtime());

    pg_atomic_write_u32(
        &g_shared->cfg_max_concurrency,
        static_cast<std::uint32_t>(std::max(0, bioseq::scheduler::settings::max_concurrency)));
    pg_atomic_write_u32(&g_shared->state, 1);
    pg_atomic_fetch_add_u32(&g_shared->cfg_generation, 1);
    reset_shared_stats(
        g_shared->stats,
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns()));

    while (!got_sigterm) {
        const int rc =
            WaitLatch(MyLatch,
                      WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                      1000L,
                      PG_WAIT_EXTENSION);
        if ((rc & WL_LATCH_SET) != 0) {
            ResetLatch(MyLatch);
        }
        CHECK_FOR_INTERRUPTS();
        if (got_sighup) {
            got_sighup = 0;
            ProcessConfigFile(PGC_SIGHUP);
            pg_atomic_write_u32(
                &g_shared->cfg_max_concurrency,
                static_cast<std::uint32_t>(
                    std::max(0, bioseq::scheduler::settings::max_concurrency)));
            pg_atomic_fetch_add_u32(&g_shared->cfg_generation, 1);
        }

        service_pending_shader_compiles();
        process_one_persist_job_if_ready();

        PgSchedulerStats& stats = g_shared->stats;
        const std::uint64_t now =
            static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        const std::uint64_t start_ns =
            pg_atomic_read_u64(&stats.rate_window_start_ns);
        if (start_ns == 0 || now - start_ns > 5'000'000'000ull) {
            pg_atomic_write_u64(&stats.rate_window_start_ns, now);
            pg_atomic_write_u64(
                &stats.rate_window_start_queries,
                pg_atomic_read_u64(&stats.queries_processed));
        }
    }

    pg_atomic_write_u32(&g_shared->state, 0);
    pg_atomic_write_u32(
        &g_shared->supervisor_pgprocno,
        static_cast<std::uint32_t>(INVALID_PROC_NUMBER));
    ConditionVariableBroadcast(&g_shared->runtime_init_cv);
    ConditionVariableBroadcast(&g_shared->compile_done_cv);
    proc_exit(0);
}
