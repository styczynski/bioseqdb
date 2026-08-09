/**
 * @file supervisor_pg_persist_stage_impl.cppm
 * @brief PostgreSQL asynchronous MPS persistence staging implementation.
 */
module;

#include "common.h"

extern "C" {
#include "miscadmin.h"
#include "storage/dsm.h"
#include "storage/dsm_impl.h"
}

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

module bioseq.postgres.scheduler.supervisor;

import bioseq.common.mlogging;
import bioseq.config;
import bioseq.instr;
import bioseq.pg_utils;
import bioseq.indexing.type;
import bioseq.storage.manager;
import bioseq.sequences.kmers.highway_contract;

namespace bioseq::parallelism::pg {

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

void ensure_persist_xact_callback_registered() {
    if (g_persist_xact_callback_registered) {
        return;
    }
    invoke_async_mps_stage_pg_leaf(
        AsyncMpsStagePgFailure::register_xact_callback,
        [] { register_persist_xact_callback_pg_leaf(); });
    // RegisterXactCallback can ERROR while allocating its callback node. Set
    // the completion bit only after the bridged primitive returned, so retry
    // never duplicates an already-installed callback.
    g_persist_xact_callback_registered = true;
}

[[nodiscard]] bool async_persist_queue_available() noexcept {
    return g_shared != nullptr;
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
                     errdetail("backend=%d", index_sketch_backend)));
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

        enqueue_pending_mps_build(
            target,
            dsm.handle,
            bytes_total,
            *requested_provider);
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
            target.index_name,
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
