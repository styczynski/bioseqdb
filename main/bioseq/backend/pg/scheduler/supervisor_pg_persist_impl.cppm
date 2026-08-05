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
import bioseq.sequences.kmers.highway_contract;

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
    std::int64_t highway_executed_target;
    bioseq::storage::seq_locator locator;
};
static_assert(sizeof(IndexBuildMpsDsmHeader) == 88);
static_assert(offsetof(IndexBuildMpsDsmHeader, requested_provider) == 36);
static_assert(offsetof(IndexBuildMpsDsmHeader, highway_executed_target) == 40);
static_assert(offsetof(IndexBuildMpsDsmHeader, locator) == 48);
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

[[nodiscard]] constexpr bool minimizer_backend_requires_highway_target(
        std::int32_t backend_value) noexcept {
    using bioseq::minimizers::MinimizerSketchBackend;
    const auto backend = static_cast<MinimizerSketchBackend>(backend_value);
    return backend == MinimizerSketchBackend::ExactSimd
        || backend == MinimizerSketchBackend::PaperNtHashSimd;
}

[[nodiscard]] constexpr bool minimizer_backend_allowed_in_async_mps(
        std::int32_t backend_value) noexcept {
    using bioseq::minimizers::MinimizerSketchBackend;
    const auto backend = static_cast<MinimizerSketchBackend>(backend_value);
    return backend == MinimizerSketchBackend::Scalar
        || backend == MinimizerSketchBackend::VulkanSegment
        || backend == MinimizerSketchBackend::ExactSimd;
}

inline bool g_persist_xact_callback_registered = false;


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

void record_prohibited_legacy_persist_rejection(
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
        || (minimizer_backend_requires_highway_target(
                header.index_sketch_backend)
            && header.highway_executed_target == 0)
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
    meta.highway_executed_target = header.highway_executed_target;
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
        std::int64_t highway_executed_target,
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
    if (!minimizer_backend_allowed_in_async_mps(index_sketch_backend)) {
        return false;
    }
    if (minimizer_backend_requires_highway_target(index_sketch_backend)
        && highway_executed_target == 0) {
        bioseq::pg_utils::invoke_pg_as_cpp(
            [index_sketch_backend, highway_executed_target] {
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("asynchronous MPS build has no selected Highway target"),
                     errdetail("backend=%d target=%lld; the queued worker cannot redispatch or change algorithms",
                        index_sketch_backend,
                        static_cast<long long>(highway_executed_target))));
            });
        pg_unreachable();
    }
    (void)rebind_minimizer_kernels_for_backend(
        index_sketch_backend, highway_executed_target);
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
        // policy, and the already-selected Highway target bit. Function/table
        // pointers remain process-local and never cross this boundary.
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
            .highway_executed_target = highway_executed_target,
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

} // namespace bioseq::parallelism::pg
