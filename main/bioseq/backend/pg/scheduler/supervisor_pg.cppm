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
#include <limits>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

export module bioseq.scheduler.supervisor_pg;
import bioseq.common.mlogging;
import bioseq.config;                  // DOC0322 §9.3 — bioseq_scheduler_*, bioseq_index_async_persist, bioseq_index_memory_cap_mb GUCs
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
import bioseq.storage.scanner_pg;
import bioseq.types.pg_compact_sequence;
import bioseq.sequences;
import bioseq.storage.indexing_pg.chunked_storage;
import bioseq.storage.indexing_pg.handles;

namespace bioseq::parallelism::pg {

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
    bioseq::storage::seq_locator locator;
};
static_assert(sizeof(IndexBuildMpsDsmHeader) == 72);

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
inline constexpr std::uint32_t kIndexBuildMpsDsmVersion = 2;

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

struct BioseqSchedulerShared {
    pg_atomic_uint32 cfg_max_concurrency;
    pg_atomic_uint32 cfg_generation;
    pg_atomic_uint32 state;
    PgSchedulerStats stats;
    PgMpsBuildStats mps_build_stats;
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
    if (IsParallelWorker() && !bioseq_scheduler_allow_pg_parallel_composition) {
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

struct PersistChunk {
    bioseq::indexing::index_sub_key_t sub_key = 0;
    std::uint32_t chunk_no = 0;
    std::span<const char> bytes{};
};

struct PendingPersistJob {
    PersistTarget target{};
    std::string index_name{};
    std::string temp_file_path{};
    dsm_handle dsm = DSM_HANDLE_INVALID;
    std::uint64_t payload_bytes = 0;
    bool published = false;
    bool file_backed = false;
    bioseq::parallelism::MemoryPermit permit{};
};

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
    dsm_unpin_segment(handle);
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
        || header.locator.version != bioseq::storage::kLocatorVersion) {
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
    return meta;
}

[[nodiscard]] bool publish_persist_job_to_shared_queue(PendingPersistJob& job) noexcept {
    if (g_shared == nullptr || job.dsm == DSM_HANDLE_INVALID) {
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
    RegisterXactCallback(bioseq_persist_xact_callback, nullptr);
    g_persist_xact_callback_registered = true;
}

[[nodiscard]] bool async_persist_queue_available() noexcept {
    return g_shared != nullptr;
}

[[nodiscard]] bool stage_async_index_persist(
        const PersistTarget& target,
        std::span<const PersistChunk> chunks) {
    const std::uint64_t stage_start_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    if (!bioseq_index_async_persist || chunks.empty() || target.index_name.empty()) {
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
}

[[nodiscard]] bool stage_async_index_persist_file(
        const PersistTarget& target,
        std::string temp_file_path,
        std::uint64_t file_bytes,
        std::uint64_t chunk_count) {
    const std::uint64_t stage_start_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    if (!bioseq_index_async_persist
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
        std::int32_t index_sketch_backend) {  // DOC0345 Patch K
    const std::uint64_t stage_start_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    if (!bioseq_index_async_persist
        || target.index_name.empty()
        || locator.version != bioseq::storage::kLocatorVersion
        || locator.num_fragments == 0
        || locator.total_bp_size == 0) {
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
    ensure_persist_xact_callback_registered();

    const std::uint64_t bytes_total =
        sizeof(IndexBuildMpsDsmHeader) + target.index_name.size();
    if (bytes_total == 0
        || bytes_total
            > static_cast<std::uint64_t>((std::numeric_limits<Size>::max)())) {
        return false;
    }

    dsm_segment* seg = dsm_create(
        static_cast<Size>(bytes_total),
        DSM_CREATE_NULL_IF_MAXSEGMENTS);
    if (seg == nullptr) {
        return false;
    }
    char* base = static_cast<char*>(dsm_segment_address(seg));
    IndexBuildMpsDsmHeader header{
        .magic = kIndexBuildMpsDsmMagic,
        .version = kIndexBuildMpsDsmVersion,
        .index_name_len = static_cast<std::uint32_t>(target.index_name.size()),
        .alphabet_code = alphabet_code,
        .storage_unit_byte_width = storage_unit_byte_width,
        .last_fragment_padding = last_fragment_padding,
        .pair_sort_impl = pair_sort_impl,
        .pipeline_mode = pipeline_mode,
        .max_concurrency = max_concurrency,
        .index_memory_cap_mb = index_memory_cap_mb,
        .index_sketch_backend = index_sketch_backend,  // DOC0345 Patch K
        .locator = locator,
    };
    std::memcpy(base, &header, sizeof(header));
    std::memcpy(base + sizeof(header), target.index_name.data(), target.index_name.size());
    dsm_pin_segment(seg);
    const dsm_handle handle = dsm_segment_handle(seg);
    dsm_detach(seg);

    PendingPersistJob job{};
    job.target = target;
    job.index_name.assign(target.index_name.begin(), target.index_name.end());
    job.target.index_name = job.index_name;
    job.dsm = handle;
    job.payload_bytes = bytes_total;
    pending_persist_jobs().push_back(std::move(job));
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
        static_cast<std::uint32_t>(std::max(1, bioseq_aligner_window_size));
    inputs.msl = bioseq::gpu::vulkan::MslVersion{3u, 2u, 0u};
    inputs.moltenvk_build = "conservative-no-device-probe";
    inputs.device_uuid = "conservative-no-device-probe";
    return inputs;
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
    if (auto command_lib = bioseq::gpu::vulkan::ensure_moltenvk_command_library(
            cache_dir, inputs.msl);
        !command_lib) {
        return std::unexpected(fmt::format(
            "moltenvk command library: {}", command_lib.error()));
    }

    if (g_shared != nullptr) {
        std::snprintf(
            g_shared->cache_digest_hex, sizeof(g_shared->cache_digest_hex),
            "%s", digest.c_str());
    }

    std::vector<bioseq::gpu::vulkan::ShaderVariantKey> keys;
    bioseq::gpu::vulkan::for_each_static_shader_variant(
        inputs.variants,
        [&keys](const bioseq::gpu::vulkan::ShaderVariantKey& key) {
            keys.push_back(key);
        });

    bioseq::parallelism::GlobalPool::instance().configure(
        bioseq::parallelism::PoolConfig{
            .max_concurrency = effective_max_concurrency(
                static_cast<unsigned>(std::max(0, bioseq_scheduler_max_concurrency))),
            .mode = bioseq::parallelism::mode_from_int(
                bioseq_scheduler_pipeline_concurrency_mode),
        });

    constexpr std::uint64_t kPeakCompileBytes = 256ull << 20;
    std::size_t cursor = 0;
    while (cursor < keys.size()) {
        const unsigned requested = static_cast<unsigned>(
            std::min<std::size_t>(4, keys.size() - cursor));
        auto wave = bioseq::parallelism::try_reserve_budgeted_wave(
            requested, kPeakCompileBytes);
        if (!wave.admitted) {
            return std::unexpected("shader cache AOT memory admission failed");
        }
        const std::size_t begin = cursor;
        const std::size_t end = std::min<std::size_t>(keys.size(), cursor + wave.fanout);
        std::mutex error_mu;
        std::string first_error;
        bioseq::parallelism::GlobalPool::instance().parallel_for(
            begin, end, 1,
            [&](std::size_t b, std::size_t e) {
                for (std::size_t i = b; i < e; ++i) {
                    auto compiled = bioseq::gpu::vulkan::ensure_compiled(
                        cache_dir, keys[i], inputs.msl);
                    if (!compiled) {
                        std::lock_guard<std::mutex> guard(error_mu);
                        if (first_error.empty()) {
                            first_error = fmt::format(
                                "{}: {}",
                                bioseq::gpu::vulkan::cache_file_stem(keys[i]),
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

void wake_supervisor_if_known() noexcept {
    if (g_shared == nullptr) {
        return;
    }
    const std::uint32_t procno = pg_atomic_read_u32(&g_shared->supervisor_pgprocno);
    if (procno_in_range(procno)) {
        SetLatch(&GetPGProcByNumber(static_cast<ProcNumber>(procno))->procLatch);
    }
}

[[noreturn]] void report_runtime_init_unavailable(std::string_view detail) {
    bioseq_report<ERR, PG>()
        .code(ERRCODE_INTERNAL_ERROR)
        .msg("Vulkan aligner execution failed")
        .detail("{}", detail)
        .hint("Selected PostgreSQL Vulkan execution requires a successful bioseq GPU runtime initialization; use the backend-local helper path or the shared-preload supervisor.");
    __builtin_unreachable();
}

void wait_for_runtime_ready() {
    static bool backend_runtime_ready = false;
    if (backend_runtime_ready) {
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
    ConditionVariablePrepareToSleep(&g_shared->runtime_init_cv);
    for (;;) {
        CHECK_FOR_INTERRUPTS();
        const auto state = static_cast<RuntimeInitState>(
            pg_atomic_read_u32(&g_shared->runtime_init_state));
        pg_read_barrier();
        if (state == RuntimeInitState::Ready) {
            const std::string digest = current_runtime_cache_digest();
            if (!digest.empty()) {
                const std::filesystem::path cache_dir =
                    runtime_cache_dir_from_digest(digest);
                const std::filesystem::path command_lib =
                    bioseq::gpu::vulkan::moltenvk_command_metallib_path(cache_dir);
                (void)::setenv(
                    "BIOSEQ_MOLTENVK_COMMAND_METALLIB",
                    command_lib.c_str(),
                    1);
                (void)::setenv(
                    "BIOSEQ_MOLTENVK_BINARY_ARCHIVE_DIR",
                    cache_dir.c_str(),
                    1);
            }
            backend_runtime_ready = true;
            ConditionVariableCancelSleep();
            return;
        }
        if (state == RuntimeInitState::Failed) {
            const std::uint32_t errc =
                pg_atomic_read_u32(&g_shared->runtime_init_errc);
            ConditionVariableCancelSleep();
            report_runtime_init_unavailable(fmt::format(
                "load_bioseq_runtime failed (errc {}); GPU unavailable", errc));
        }
        if (static_cast<std::uint64_t>(bioseq::instr::clock::now_ns()) > deadline) {
            ConditionVariableCancelSleep();
            report_runtime_init_unavailable(
                "load_bioseq_runtime unavailable (supervisor down / timeout)");
        }
        (void)ConditionVariableTimedSleep(
            &g_shared->runtime_init_cv, 1000L, PG_WAIT_EXTENSION);
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
        wake_supervisor_if_known();
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
        ConditionVariablePrepareToSleep(&g_shared->compile_done_cv);
        for (;;) {
            CHECK_FOR_INTERRUPTS();
            ShaderCompileSlot& slot = g_shared->compile_queue[slot_index];
            const auto state = static_cast<CompileSlotState>(
                pg_atomic_read_u32(&slot.state));
            const std::uint32_t observed_gen =
                pg_atomic_read_u32(&slot.generation);
            if ((state == CompileSlotState::DoneOk || state == CompileSlotState::DoneErr)
                && observed_gen != generation) {
                pg_read_barrier();
                const std::uint32_t errc = slot.errc;
                ConditionVariableCancelSleep();
                release_shader_compile_slot(slot_index);
                if (state == CompileSlotState::DoneOk) {
                    return {};
                }
                return std::unexpected(fmt::format(
                    "shader compile failed for {} (errc {})",
                    bioseq::gpu::vulkan::cache_file_stem(key), errc));
            }
            if (static_cast<std::uint64_t>(bioseq::instr::clock::now_ns()) > deadline) {
                ConditionVariableCancelSleep();
                return std::unexpected(fmt::format(
                    "shader compile timed out for {}",
                    bioseq::gpu::vulkan::cache_file_stem(key)));
            }
            (void)ConditionVariableTimedSleep(
                &g_shared->compile_done_cv, 1000L, PG_WAIT_EXTENSION);
        }
    }
};

[[nodiscard]] std::expected<std::filesystem::path, std::string>
prepare_backend_local_shader_cache_env() {
    const auto inputs = conservative_shader_inputs();
    const std::string digest = bioseq::gpu::vulkan::shader_cache_digest_hex(inputs);
    const std::filesystem::path cache_dir = runtime_cache_dir_from_digest(digest);
    if (auto command_lib = bioseq::gpu::vulkan::ensure_moltenvk_command_library(
            cache_dir, inputs.msl);
        !command_lib) {
        return std::unexpected(fmt::format(
            "moltenvk command library: {}", command_lib.error()));
    }
    const std::filesystem::path command_path =
        bioseq::gpu::vulkan::moltenvk_command_metallib_path(cache_dir);
    (void)::setenv("BIOSEQ_MOLTENVK_COMMAND_METALLIB", command_path.c_str(), 1);
    (void)::setenv("BIOSEQ_MOLTENVK_BINARY_ARCHIVE_DIR", cache_dir.c_str(), 1);
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

    std::vector<bioseq::gpu::vulkan::ShaderVariantKey> keys;
    bioseq::gpu::vulkan::for_each_static_shader_variant(
        inputs.variants,
        [&keys](const bioseq::gpu::vulkan::ShaderVariantKey& key) {
            keys.push_back(key);
        });

    auto file_size_or_zero = [](const std::filesystem::path& path) -> std::uintmax_t {
        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(path, ec);
        return ec ? 0u : size;
    };

    std::size_t failed = 0;
    for (const auto& key : keys) {
        const std::string stem = bioseq::gpu::vulkan::cache_file_stem(key);
        auto compiled = bioseq::gpu::vulkan::ensure_compiled(cache_dir, key, inputs.msl);
        if (!compiled) {
            ++failed;
            out << "variant " << stem
                << " kind=" << key.kind
                << " tile=" << key.tile_size
                << " local=" << key.local_size
                << " max_span=" << key.max_span
                << " status=failed error=" << compiled.error() << '\n';
            continue;
        }
        const auto metallib = bioseq::gpu::vulkan::cache_metallib_path(cache_dir, key);
        const auto meta = bioseq::gpu::vulkan::cache_meta_path(cache_dir, key);
        const auto archive = bioseq::gpu::vulkan::cache_binary_archive_path(cache_dir, key);
        out << "variant " << stem
            << " kind=" << key.kind
            << " tile=" << key.tile_size
            << " local=" << key.local_size
            << " max_span=" << key.max_span
            << " status=ok"
            << " metallib_bytes=" << file_size_or_zero(metallib)
            << " meta_bytes=" << file_size_or_zero(meta)
            << " archive_bytes=" << file_size_or_zero(archive)
            << '\n';
    }
    out << "RESULT: " << (failed == 0 ? "ok" : "failed")
        << " variants=" << keys.size()
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
    if (g_shared != nullptr) {
        wait_for_runtime_ready();
        const std::string digest = current_runtime_cache_digest();
        if (digest.empty()) {
            return std::unexpected("shader cache digest unavailable");
        }
        PgSupervisorCompileService service;
        return bioseq::gpu::vulkan::load_module(
            device, runtime_cache_dir_from_digest(digest), key, create_shader_module, service);
    }

    auto prepared = prepare_backend_local_shader_cache_env();
    if (!prepared) {
        return std::unexpected(prepared.error());
    }
    const auto inputs = conservative_shader_inputs();
    PgBackendLocalCompileService service{
        .cache_dir = *prepared,
        .msl = inputs.msl,
    };
    return bioseq::gpu::vulkan::load_module(
        device, *prepared, key, create_shader_module, service);
}

void install_pg_shader_provider_if_available() noexcept {
#if defined(__APPLE__)
    if (process_shared_preload_libraries_in_progress) {
        return;
    }
    static bioseq::gpu::vulkan::RuntimeShaderProvider provider{
        &pg_shader_provider_prepare_runtime,
        &pg_shader_provider_load_module};
    bioseq::gpu::vulkan::install_runtime_shader_provider(&provider);
#endif
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
        : static_cast<unsigned>(std::max(0, bioseq_scheduler_max_concurrency));
    const auto mode = bioseq::parallelism::mode_from_int(
        bioseq_scheduler_pipeline_concurrency_mode);

    if (!pool.active() || pool.adopted_generation() != generation) {
        pool.configure(bioseq::parallelism::PoolConfig{
            .max_concurrency = effective_max_concurrency(configured),
            .mode = mode,
        });
        pool.set_adopted_generation(generation);
    } else {
        pool.set_mode(mode);
    }

    const std::uint64_t index_cap = bioseq_index_memory_cap_mb > 0
        ? (static_cast<std::uint64_t>(bioseq_index_memory_cap_mb) << 20)
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
            static_cast<std::uint32_t>(std::max(0, bioseq_scheduler_max_concurrency)));
        pg_atomic_fetch_add_u32(&g_shared->cfg_generation, 1);
        reset_shared_stats(g_shared->stats, now);
    }
    auto& pool = bioseq::parallelism::GlobalPool::instance();
    pool.restart(bioseq::parallelism::PoolConfig{
        .max_concurrency = effective_max_concurrency(
            static_cast<unsigned>(std::max(0, bioseq_scheduler_max_concurrency))),
        .mode = bioseq::parallelism::mode_from_int(
            bioseq_scheduler_pipeline_concurrency_mode),
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

struct PersistClaim {
    std::size_t slot_index = 0;
    std::uint32_t generation = 0;
    PersistTarget target{};
    dsm_handle dsm = DSM_HANDLE_INVALID;
    std::uint64_t payload_bytes = 0;
};

[[nodiscard]] std::optional<PersistClaim> claim_persist_job() noexcept {
    if (g_shared == nullptr) {
        return std::nullopt;
    }
    for (std::size_t slot_index = 0;
         slot_index < BioseqSchedulerShared::kPersistQueueSize;
         ++slot_index) {
        PgPersistQueueSlot& slot = g_shared->persist_queue[slot_index];
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
        return claim;
    }
    return std::nullopt;
}

void finish_persist_claim(const PersistClaim& claim) noexcept {
    remove_file_payload_if_present_noexcept(claim.dsm);
    if (g_shared == nullptr || claim.slot_index >= BioseqSchedulerShared::kPersistQueueSize) {
        release_pinned_dsm(claim.dsm);
        return;
    }
    PgPersistQueueSlot& slot = g_shared->persist_queue[claim.slot_index];
    release_pinned_dsm(claim.dsm);
    slot.dsm = DSM_HANDLE_INVALID;
    slot.payload_bytes = 0;
    slot.sequence_rowno = 0;
    pg_atomic_fetch_add_u32(&slot.generation, 1);
    pg_atomic_write_u32(&slot.state, 0);
}

[[nodiscard]] bool worker_target_accepts_index_persist(
        const PersistTarget& target,
        std::string_view index_name) {
    Relation rel = try_safe_relation_open(target.user_relid, AccessShareLock);
    if (rel == nullptr) {
        return false;
    }
    const Oid tablespace = OidIsValid(rel->rd_rel->reltablespace)
        ? rel->rd_rel->reltablespace
        : MyDatabaseTableSpace;
    Oid relfilenode = static_cast<Oid>(rel->rd_locator.relNumber);
    if (!OidIsValid(relfilenode)) {
        relfilenode = RelationGetRelid(rel);
    }
    relation_close(rel, AccessShareLock);
    if (tablespace != target.tablespace_oid
        || relfilenode != target.main_relfilenode) {
        return false;
    }

    bool enabled = true;
    bool update_on_insert =
        bioseq::indexing::default_update_on_insert_contains(index_name);
    if (SPI_connect() == SPI_OK_CONNECT) {
        const std::string sql = fmt::format(
            "SELECT enabled, update_on_insert FROM bioseq_internal.index_policy "
            "WHERE user_relid={}::oid AND main_relfilenode={}::oid "
            "AND column_attno={} AND index_name={} LIMIT 1",
            static_cast<unsigned>(target.user_relid),
            static_cast<unsigned>(target.main_relfilenode),
            static_cast<int>(target.column_attno),
            quote_literal_cstr(std::string(index_name).c_str()));
        if (SPI_execute(sql.c_str(), true, 0) == SPI_OK_SELECT && SPI_processed > 0) {
            bool isnull = false;
            enabled = DatumGetBool(SPI_getbinval(
                SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
            update_on_insert = DatumGetBool(SPI_getbinval(
                SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull));
        }
        SPI_finish();
    }
    return enabled && update_on_insert;
}

template <class R>
struct AsyncMpsResidentTarget {
    using traits = typename compact_sequence_range<R>::traits;
    using storage_unit = typename traits::storage_unit;
    using resident_config = sequence_custom_alphabet<
        typename traits::alphabet_type,
        storage_unit,
        traits::bits_per_char>;
    using resident_range = compact_sequence_range<resident_config>;

    bioseq::parallelism::MemoryPermit permit;
    resident_range range;
    std::uint64_t packed_bytes = 0;
};

template <class R, class Scanner>
[[nodiscard]] std::optional<AsyncMpsResidentTarget<R>>
try_materialize_async_mps_worker_target(
        const bioseq::storage::lazy_compact_sequence<R, Scanner>& seq,
        std::uint64_t sequence_rowno,
        std::string_view index_name) {
    if constexpr (!Scanner::supports_fragments()) {
        return std::nullopt;
    } else {
        auto& pool = bioseq::parallelism::GlobalPool::instance();
        if (!bioseq::parallelism::task_parallel(pool.mode())
            || pool.max_concurrency() <= 1) {
            return std::nullopt;
        }

        using resident_t = AsyncMpsResidentTarget<R>;
        using storage_unit = typename resident_t::storage_unit;
        const std::uint64_t total_units64 =
            static_cast<std::uint64_t>(seq.size());
        if (total_units64
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())) {
            bioseq_log<ERR, PG>(
                   "bioseq async mps build: row {} index {} has too many "
                   "packed units to materialize in one worker",
                   static_cast<unsigned long long>(sequence_rowno),
                   std::string(index_name).c_str());
        }
        const std::size_t total_units =
            static_cast<std::size_t>(total_units64);
        const std::uint64_t packed_bytes =
            bioseq::parallelism::saturating_mul(
                total_units64,
                static_cast<std::uint64_t>(sizeof(storage_unit)));
        constexpr std::uint64_t context_reserve_bytes = std::uint64_t{8} << 20;
        const std::uint64_t reserve_bytes =
            packed_bytes
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - context_reserve_bytes
                ? (std::numeric_limits<std::uint64_t>::max)()
                : packed_bytes + context_reserve_bytes;

        auto permit =
            bioseq::parallelism::MemoryAdmissionController::instance()
                .try_reserve(reserve_bytes);
        if (!permit) {
            bioseq_log<DBG>("[bioseq] index: async mps worker row={} index={} target_bp={} "
                    "fragments={} packed_bytes={} not admitted for resident "
                    "materialization; using scanner_pg",
                    static_cast<unsigned long long>(sequence_rowno),
                    index_name,
                    static_cast<unsigned long long>(seq.bp_size()),
                    seq.number_of_fragments(),
                    static_cast<unsigned long long>(packed_bytes));
            return std::nullopt;
        }

        typename resident_t::resident_range::base_container packed(total_units);
        const std::size_t fragment_units = Scanner::fragment_storage_units;
        for (std::size_t fragment_no = 0;
             fragment_no < seq.number_of_fragments();
             ++fragment_no) {
            if ((fragment_no & std::size_t{0x3ff}) == 0) {
                CHECK_FOR_INTERRUPTS();
            }
            const std::span<const storage_unit> fragment =
                seq.scanner().read_fragment(fragment_no);
            const std::size_t dst_offset = fragment_no * fragment_units;
            if (dst_offset >= packed.size()) {
                seq.scanner().release_fragment(fragment_no);
                break;
            }
            const std::size_t copy_units =
                std::min(fragment.size(), packed.size() - dst_offset);
            std::copy_n(fragment.data(), copy_units, packed.data() + dst_offset);
            seq.scanner().release_fragment(fragment_no);
        }

        bioseq_log<DBG>("[bioseq] index: async mps worker row={} index={} target_bp={} "
                "fragments={} packed_bytes={} materialized resident target",
                static_cast<unsigned long long>(sequence_rowno),
                index_name,
                static_cast<unsigned long long>(seq.bp_size()),
                seq.number_of_fragments(),
                static_cast<unsigned long long>(packed_bytes));

        return resident_t{
            std::move(*permit),
            resident_t::resident_range::from_packed(
                std::move(packed), seq.padding()),
            packed_bytes};
    }
}

void process_mps_build_claim(
        const PersistClaim& claim,
        const char* base,
        Size mapped) {
    const std::uint64_t worker_start_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    const std::optional<MpsBuildMetadata> meta =
        parse_mps_build_metadata(base, mapped);
    if (!meta.has_value()) {
        bioseq_log<DBG>("[bioseq] index: discarded invalid async mps build metadata row={}",
                static_cast<unsigned long long>(claim.target.sequence_rowno));
        return;
    }

    PersistTarget target = claim.target;
    target.index_name = meta->index_name;
    if (!worker_target_accepts_index_persist(target, meta->index_name)) {
        bioseq_log<DBG>("[bioseq] index: discarded stale async mps build job row={} index={}",
                static_cast<unsigned long long>(target.sequence_rowno),
                meta->index_name);
        return;
    }

    MemoryContext work = AllocSetContextCreate(
        CurrentMemoryContext,
        "bioseq_async_mps_build",
        ALLOCSET_DEFAULT_SIZES);
    bioseq::storage::seq_caches* caches = bioseq::storage::alloc_seq_caches(work);
    caches->side = bioseq::storage::seq_caches::Side::Read;
    caches->lock_mode = AccessShareLock;
    caches->read_state_mctx = work;
    caches->main_database_oid = target.db_oid;
    caches->main_tablespace_oid = target.tablespace_oid;
    caches->main_relfilenode = target.main_relfilenode;

    const auto previous_sort_impl =
        bioseq::indexing::detail::pair_run_sort_impl();
    bioseq::indexing::detail::set_pair_run_sort_impl(
        static_cast<bioseq::indexing::detail::PairRunSortImpl>(
            meta->pair_sort_impl));
    const auto worker_mode =
        bioseq::parallelism::mode_from_int(meta->pipeline_mode);
    const unsigned worker_concurrency = effective_max_concurrency(
        static_cast<unsigned>(std::max(0, meta->max_concurrency)));
    bioseq::parallelism::GlobalPool::instance().configure(
        bioseq::parallelism::PoolConfig{
            .max_concurrency = worker_concurrency,
            .mode = worker_mode,
        });
    const std::uint64_t worker_index_cap_bytes =
        meta->index_memory_cap_mb > 0
            ? (static_cast<std::uint64_t>(meta->index_memory_cap_mb) << 20)
            : 0;
    bioseq::parallelism::MemoryAdmissionController::instance()
        .set_index_memory_cap_bytes(worker_index_cap_bytes);

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
    PG_TRY();
    {
        pgseq_t::dispatch_instance(
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
                bioseq::storage::with_lazy_postgres<S>(
                    target.db_oid,
                    target.tablespace_oid,
                    target.main_relfilenode,
                    target.column_attno,
                    meta->locator,
                    meta->last_fragment_padding,
                    caches,
                    work,
                    [&](auto& lazy) {
                        const bool pg_lazy_worker_shareable =
                            bioseq::storage::lazy_is_worker_shareable(lazy);
                        lazy_worker_shareable = pg_lazy_worker_shareable;
                        if (!pg_lazy_worker_shareable) {
                            if (auto resident =
                                    try_materialize_async_mps_worker_target(
                                        lazy,
                                        target.sequence_rowno,
                                        meta->index_name);
                                resident.has_value()) {
                                auto resident_lazy =
                                    bioseq::storage::make_lazy_in_memory(
                                        resident->range);
                                lazy_worker_shareable =
                                    bioseq::storage::lazy_is_worker_shareable(
                                        resident_lazy);
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
                                        store, resident_lazy, worker_backend);
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
                                store, lazy, worker_backend);
                    });
            });
        built = true;
    }
    PG_CATCH();
    {
        bioseq::indexing::detail::set_pair_run_sort_impl(previous_sort_impl);
        caches->close_all(/*commit=*/false);
        bioseq::storage::destroy_seq_caches(caches);
        MemoryContextDelete(work);
        PG_RE_THROW();
    }
    PG_END_TRY();

    bioseq::indexing::detail::set_pair_run_sort_impl(previous_sort_impl);
    caches->close_all(/*commit=*/false);
    bioseq::storage::destroy_seq_caches(caches);
    MemoryContextDelete(work);

    if (built) {
        bioseq::storage::indexing::mark_serialized_index_write_successful_pg(
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

void process_persist_claim(const PersistClaim& claim) {
    const std::uint64_t worker_start_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    StartTransactionCommand();
    PushActiveSnapshot(GetTransactionSnapshot());

    dsm_segment* seg = dsm_attach(claim.dsm);
    if (seg == nullptr) {
        PopActiveSnapshot();
        CommitTransactionCommand();
        return;
    }
    char* base = static_cast<char*>(dsm_segment_address(seg));
    const Size mapped = dsm_segment_map_length(seg);
    if (mapped >= sizeof(std::uint32_t)) {
        std::uint32_t magic = 0;
        std::memcpy(&magic, base, sizeof(magic));
        if (magic == kIndexBuildMpsDsmMagic) {
            process_mps_build_claim(claim, base, mapped);
            dsm_detach(seg);
            PopActiveSnapshot();
            CommitTransactionCommand();
            return;
        }
        if (magic == kIndexPersistFileDsmMagic) {
            process_persist_file_claim(claim, base, mapped);
            dsm_detach(seg);
            PopActiveSnapshot();
            CommitTransactionCommand();
            return;
        }
    }
    if (mapped < sizeof(IndexPersistDsmHeader)) {
        dsm_detach(seg);
        PopActiveSnapshot();
        CommitTransactionCommand();
        return;
    }
    IndexPersistDsmHeader header{};
    std::memcpy(&header, base, sizeof(header));
    if (header.magic != kIndexPersistDsmMagic
        || header.version != kIndexPersistDsmVersion
        || header.payload_bytes > mapped
        || header.index_name_len == 0) {
        dsm_detach(seg);
        PopActiveSnapshot();
        CommitTransactionCommand();
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
        PopActiveSnapshot();
        CommitTransactionCommand();
        return;
    }
    std::string index_name(
        base + sizeof(IndexPersistDsmHeader),
        base + sizeof(IndexPersistDsmHeader) + header.index_name_len);
    PersistTarget target = claim.target;
    target.index_name = index_name;
    if (!worker_target_accepts_index_persist(target, index_name)) {
        dsm_detach(seg);
        PopActiveSnapshot();
        CommitTransactionCommand();
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
            PopActiveSnapshot();
            CommitTransactionCommand();
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
    PopActiveSnapshot();
    CommitTransactionCommand();
    const std::uint64_t worker_end_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    bioseq_log<DBG>("[bioseq] index: async persist worker wrote row={} index={} chunks={} bytes={} worker_write_ns={}",
            static_cast<unsigned long long>(target.sequence_rowno),
            index_name,
            header.chunk_count,
            static_cast<unsigned long long>(header.payload_bytes),
            static_cast<unsigned long long>(worker_end_ns - worker_start_ns));
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
        process_persist_claim(*claim);
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
        static_cast<std::uint32_t>(std::max(0, bioseq_scheduler_max_concurrency)));
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
                    std::max(0, bioseq_scheduler_max_concurrency)));
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
