/**
 * @file supervisor_pg_persist_impl.cppm
 * @brief PostgreSQL asynchronous persistence staging and worker implementation.
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
#include <array>
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
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

module bioseq.scheduler.supervisor_pg;
import bioseq.common.mlogging;
import bioseq.config;
import bioseq.instr;
import bioseq.parallelism.memory_admission;
import bioseq.pg_utils;
import bioseq.indexing.type;
import bioseq.storage.manager;
import bioseq.sequences;
import bioseq.storage.indexing_pg.chunked_storage;
import bioseq.storage.indexing_pg.handles;

namespace bioseq::parallelism::pg {

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
    std::int32_t index_sketch_backend;
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

[[nodiscard]] constexpr bool valid_minimizer_sketch_backend_wire_value(
        std::int32_t backend_value) noexcept {
    using bioseq::minimizers::MinimizerSketchBackend;
    switch (static_cast<MinimizerSketchBackend>(backend_value)) {
        case MinimizerSketchBackend::Scalar:
        case MinimizerSketchBackend::VulkanSegment:
        case MinimizerSketchBackend::ExactSimd:
        case MinimizerSketchBackend::PaperNtHashSimd:
            return true;
    }
    return false;
}

[[nodiscard]] constexpr bool minimizer_backend_allowed_in_async_mps(
        std::int32_t backend_value) noexcept {
    using bioseq::minimizers::MinimizerSketchBackend;
    const auto backend = static_cast<MinimizerSketchBackend>(backend_value);
    return backend == MinimizerSketchBackend::Scalar
        || backend == MinimizerSketchBackend::VulkanSegment;
}

inline bool g_persist_xact_callback_registered = false;

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
        || !valid_minimizer_sketch_backend_wire_value(
            header.index_sketch_backend)
        || !minimizer_backend_allowed_in_async_mps(
            header.index_sketch_backend)
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
    meta.index_sketch_backend = header.index_sketch_backend;
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
    if (!valid_minimizer_sketch_backend_wire_value(index_sketch_backend)) {
        bioseq::pg_utils::invoke_pg_as_cpp(
            [index_sketch_backend] {
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("asynchronous MPS build has an invalid minimizer backend"),
                     errdetail("backend=%d", index_sketch_backend))));
            });
        pg_unreachable();
    }
    // Generated Highway tables are process-local and may only be obtained by
    // the one scheduler-owned dispatch root. An asynchronous PostgreSQL worker
    // cannot redispatch or reconstruct a table from an architecture enum, so
    // SIMD recomputes remain foreground-only and are not queued for MPS
    // persistence. Scalar/Vulkan work carries no process-local function table.
    if (!minimizer_backend_allowed_in_async_mps(index_sketch_backend)) {
        return false;
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

        // This DSM is metadata-only: locator/source identity and selected
        // build policy. Process-local Highway tables and architecture enums
        // never cross this boundary.
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
