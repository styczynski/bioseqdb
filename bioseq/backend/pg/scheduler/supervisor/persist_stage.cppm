/**
 * @file scheduler/supervisor/persist_stage.cppm
 * @brief Prepare shared metadata for an asynchronous minimizer-sequence build.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "storage/dsm.h"
#include "storage/dsm_impl.h"

#include "miscadmin.h"
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

import bioseq.common.logging;
import bioseq.postgres.configuration.core;
import bioseq.instrumentation.clock;
import bioseq.postgres.support;
import bioseq.indexing.type;
import bioseq.postgres.storage.manager;
import bioseq.storage.layout;
import bioseq.sequences.kmers.highway_minimizer_contract;

namespace bioseq::postgres::scheduler {

/**
 * Shared-memory header describing how the worker should rebuild the index.
 * Keep its layout in sync with the worker parser. Save the selected Highway
 * target as an identifier so the worker can find its own kernel pointers.
 */
struct minimizer_sequence_build_dsm_header {
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
    std::int64_t highway_executed_target;
    bioseq::storage::sequence_locator locator;
};
static_assert(sizeof(minimizer_sequence_build_dsm_header) == 88);
static_assert(offsetof(minimizer_sequence_build_dsm_header, highway_executed_target) == 40);
static_assert(offsetof(minimizer_sequence_build_dsm_header, locator) == 48);
static_assert(std::is_trivially_copyable_v<minimizer_sequence_build_dsm_header>);

/** Check whether the saved backend selector names a known minimizer algorithm. */
[[nodiscard]] constexpr bool
valid_minimizer_sketch_backend_wire_value(std::int32_t backend_value) noexcept {
    using bioseq::minimizers::minimizer_sketch_backend;
    switch (static_cast<minimizer_sketch_backend>(backend_value)) {
    case minimizer_sketch_backend::scalar:
    case minimizer_sketch_backend::vulkan_segment:
    case minimizer_sketch_backend::exact_simd:
    case minimizer_sketch_backend::bounded_nthash_simd:
        return true;
    }
    return false;
}

/** Check whether this backend needs a saved Highway target to find its kernels. */
[[nodiscard]] constexpr bool
minimizer_backend_requires_highway_target(std::int32_t backend_value) noexcept {
    using bioseq::minimizers::minimizer_sketch_backend;
    const auto backend = static_cast<minimizer_sketch_backend>(backend_value);
    return backend == minimizer_sketch_backend::exact_simd ||
           backend == minimizer_sketch_backend::bounded_nthash_simd;
}

/** Check whether the job metadata is sufficient to rebuild this minimizer format. */
[[nodiscard]] constexpr bool
minimizer_backend_allowed_in_async_minimizer_sequence(std::int32_t backend_value) noexcept {
    using bioseq::minimizers::minimizer_sketch_backend;
    const auto backend = static_cast<minimizer_sketch_backend>(backend_value);
    return backend == minimizer_sketch_backend::scalar ||
           backend == minimizer_sketch_backend::vulkan_segment ||
           backend == minimizer_sketch_backend::exact_simd;
}

/** @brief Tracks whether this backend installed its transaction callback. */
inline bool g_persist_xact_callback_registered = false;

#ifdef BIOSEQ_TEST_INDEXES
/** Failure to inject once when staging a job in this backend. */
inline thread_local async_minimizer_sequence_stage_pg_failure
    g_async_minimizer_sequence_stage_pg_failure = async_minimizer_sequence_stage_pg_failure::none;
/** @brief Counts process-local staging failures raised by test injection. */
inline thread_local std::uint64_t g_async_minimizer_sequence_stage_injected_failures = 0;
#endif

/**
 * Select a staging operation to fail once in tests.
 * Return false for an invalid selector or a build without test instrumentation.
 */
[[nodiscard]] bool arm_async_minimizer_sequence_stage_pg_failure(
    async_minimizer_sequence_stage_pg_failure point) noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    if (point == async_minimizer_sequence_stage_pg_failure::none ||
        point > async_minimizer_sequence_stage_pg_failure::dsm_detach) {
        return false;
    }
    g_async_minimizer_sequence_stage_pg_failure = point;
    return true;
#else
    (void)point;
    return false;
#endif
}

/** Cancel this backend's pending staging failure injection. */
void disarm_async_minimizer_sequence_stage_pg_failure() noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    g_async_minimizer_sequence_stage_pg_failure = async_minimizer_sequence_stage_pg_failure::none;
#endif
}

/** Return the pending staging failure, or none when test instrumentation is disabled. */
[[nodiscard]] async_minimizer_sequence_stage_pg_failure
async_minimizer_sequence_stage_pg_failure_armed() noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    return g_async_minimizer_sequence_stage_pg_failure;
#else
    return async_minimizer_sequence_stage_pg_failure::none;
#endif
}

/** Return this backend's injected staging failure count, or zero outside test builds. */
[[nodiscard]] std::uint64_t async_minimizer_sequence_stage_injected_failure_count() noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    return g_async_minimizer_sequence_stage_injected_failures;
#else
    return 0u;
#endif
}

/** Raise a PostgreSQL error if this operation matches the pending test failure. */
void inject_async_minimizer_sequence_stage_pg_failure_leaf(
    async_minimizer_sequence_stage_pg_failure point) {
#ifdef BIOSEQ_TEST_INDEXES
    if (g_async_minimizer_sequence_stage_pg_failure != point) {
        return;
    }
    g_async_minimizer_sequence_stage_pg_failure = async_minimizer_sequence_stage_pg_failure::none;
    ++g_async_minimizer_sequence_stage_injected_failures;
    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("injected asynchronous minimizer-sequence staging PostgreSQL leaf failure"),
             errdetail("failure_point=%u", static_cast<unsigned>(point))));
#else
    (void)point;
#endif
}

/**
 * Run a staging operation through the PostgreSQL-to-C++ error bridge.
 * Check for an injected test failure before calling the operation.
 * @tparam p_function Callable returning the operation result.
 * @param point Test failure location.
 * @param function Operation to run after checking the failure selector.
 * @return The operation's result.
 */
template <class p_function>
auto invoke_async_minimizer_sequence_stage_pg_leaf(async_minimizer_sequence_stage_pg_failure point,
                                                   p_function&& function)
    -> std::invoke_result_t<std::remove_reference_t<p_function>&> {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [point, &function]() -> std::invoke_result_t<std::remove_reference_t<p_function>&> {
            inject_async_minimizer_sequence_stage_pg_failure_leaf(point);
            return std::invoke(function);
        });
}

/**
 * Release the shared-memory segment's pin during cleanup.
 * Ignore an invalid handle and keep cleanup errors from replacing the original
 * transaction or worker error.
 */
void release_pinned_dsm(dsm_handle handle) noexcept {
    if (handle == DSM_HANDLE_INVALID) {
        return;
    }
    try {
        bioseq::postgres::support::invoke_pg_as_cpp([handle] { dsm_unpin_segment(handle); });
    } catch (...) {
        // Keep a cleanup error from skipping C++ destructors or replacing the
        // transaction or worker failure that brought us here.
    }
}

/**
 * Read a build request from mapped shared memory. Check the format and
 * algorithm selectors, locator version, nonzero source version, and name
 * bounds before copying the metadata. Return no request if a check fails.
 * @param base Start of the mapped bytes.
 * @param mapped Accessible byte count.
 */
[[nodiscard]] std::optional<minimizer_sequence_build_metadata>
parse_minimizer_sequence_build_metadata(const char* base, Size mapped) {
    if (mapped < sizeof(minimizer_sequence_build_dsm_header)) {
        return std::nullopt;
    }
    minimizer_sequence_build_dsm_header header{};
    std::memcpy(&header, base, sizeof(header));
    if (header.magic != k_minimizer_sequence_build_dsm_magic ||
        header.version != k_minimizer_sequence_build_wire_revision || header.index_name_len == 0 ||
        !bioseq::indexing::pair_run_sort_implementation_ordinal_valid(header.pair_sort_impl) ||
        !valid_minimizer_sketch_backend_wire_value(header.index_sketch_backend) ||
        !minimizer_backend_allowed_in_async_minimizer_sequence(header.index_sketch_backend) ||
        (minimizer_backend_requires_highway_target(header.index_sketch_backend) &&
         header.highway_executed_target == 0) ||
        header.locator.payload_version != bioseq::storage::sequence_locator_payload_version ||
        header.locator.source_epoch == 0) {
        return std::nullopt;
    }
    const std::uint64_t name_offset = sizeof(minimizer_sequence_build_dsm_header);
    const std::uint64_t name_end = name_offset + header.index_name_len;
    if (name_end < name_offset || name_end > static_cast<std::uint64_t>(mapped)) {
        return std::nullopt;
    }
    minimizer_sequence_build_metadata meta{};
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
    return meta;
}

/** Register the transaction callback once for this backend. */
void ensure_persist_xact_callback_registered() {
    if (g_persist_xact_callback_registered) {
        return;
    }
    invoke_async_minimizer_sequence_stage_pg_leaf(
        async_minimizer_sequence_stage_pg_failure::register_xact_callback,
        [] { register_persist_xact_callback_pg_leaf(); });
    // Mark registration complete only after PostgreSQL accepts the callback.
    // A failed allocation must leave registration available for a later retry.
    g_persist_xact_callback_registered = true;
}

/** Check whether this backend has attached the shared persistence queue. */
[[nodiscard]] bool async_persist_queue_available() noexcept {
    return g_shared != nullptr;
}

/**
 * Save an index-build request with the current transaction.
 * Copy the source locator and build settings into pinned shared memory so a
 * worker can rebuild the index after commit. Unsupported requests or unavailable
 * queue resources return false. Invalid algorithm settings raise an error.
 * @param target Table, column, index, and sequence identity.
 * @param locator Stored sequence location used by the worker.
 * @param alphabet_code Sequence alphabet code.
 * @param storage_unit_byte_width Packed storage width in bytes.
 * @param last_fragment_padding Unused symbol positions in the last fragment.
 * @param pair_sort_impl Pair sorter selector.
 * @param pipeline_mode Worker concurrency mode.
 * @param max_concurrency Worker thread limit.
 * @param index_memory_cap_mb Worker memory cap in MiB.
 * @param index_sketch_backend Minimizer algorithm selector.
 * @param highway_executed_target Highway target selected before staging.
 * @return True after the transaction list takes ownership of the pinned segment.
 */
[[nodiscard]] bool
stage_async_minimizer_sequence_build(const persist_target& target,
                                     const bioseq::storage::sequence_locator& locator,
                                     std::uint8_t alphabet_code,
                                     std::uint8_t storage_unit_byte_width,
                                     std::uint16_t last_fragment_padding,
                                     std::int32_t pair_sort_impl,
                                     std::int32_t pipeline_mode,
                                     std::int32_t max_concurrency,
                                     std::int32_t index_memory_cap_mb,
                                     std::int32_t index_sketch_backend,
                                     std::int64_t highway_executed_target) {
    if (!bioseq::indexing::pair_run_sort_implementation_ordinal_valid(pair_sort_impl)) {
        bioseq::postgres::support::invoke_pg_as_cpp([pair_sort_impl] {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("asynchronous minimizer-sequence build has an invalid pair sorter"),
                     errdetail("sorter=%d", pair_sort_impl)));
        });
        pg_unreachable();
    }
    if (!valid_minimizer_sketch_backend_wire_value(index_sketch_backend)) {
        bioseq::postgres::support::invoke_pg_as_cpp([index_sketch_backend] {
            ereport(
                ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("asynchronous minimizer-sequence build has an invalid minimizer backend"),
                 errdetail("backend=%d", index_sketch_backend)));
        });
        pg_unreachable();
    }
    if (!minimizer_backend_allowed_in_async_minimizer_sequence(index_sketch_backend)) {
        return false;
    }
    if (minimizer_backend_requires_highway_target(index_sketch_backend) &&
        highway_executed_target == 0) {
        bioseq::postgres::support::invoke_pg_as_cpp([index_sketch_backend,
                                                     highway_executed_target] {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("asynchronous minimizer-sequence build has no selected Highway target"),
                     errdetail("backend=%d target=%lld; the queued worker cannot redispatch or "
                               "change algorithms",
                               index_sketch_backend,
                               static_cast<long long>(highway_executed_target))));
        });
        pg_unreachable();
    }
    (void)rebind_minimizer_kernels_for_backend(index_sketch_backend, highway_executed_target);
    const std::uint64_t stage_start_ns =
        static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
    if (!bioseq::settings::index::async_persistence::enabled || !OidIsValid(target.db_oid) ||
        !OidIsValid(target.user_relid) || !OidIsValid(target.main_relfilenode) ||
        target.column_attno <= 0 || target.index_name.empty() ||
        locator.payload_version != bioseq::storage::sequence_locator_payload_version ||
        locator.source_epoch == 0 || locator.fragment_count == 0 || locator.logical_length == 0 ||
        target.bucket_idx != locator.bucket_index || target.shard_idx != locator.shard_index) {
        return false;
    }
    if (!async_persist_queue_available()) {
        bioseq_log<logging_level::debug>(
            "[bioseq] index: async minimizer-sequence build skipped row={} index={} target_bp={} "
            "fragments={} because shared supervisor queue is unavailable",
            static_cast<unsigned long long>(target.sequence_rowno),
            target.index_name,
            static_cast<unsigned long long>(locator.logical_length),
            static_cast<unsigned long long>(locator.fragment_count));
        return false;
    }
    if (target.index_name.size() > (std::numeric_limits<std::uint32_t>::max)() ||
        target.index_name.size() > (std::numeric_limits<std::uint64_t>::max)() -
                                       sizeof(minimizer_sequence_build_dsm_header)) {
        return false;
    }
    const std::uint64_t bytes_total = sizeof(minimizer_sequence_build_dsm_header) +
                                      static_cast<std::uint64_t>(target.index_name.size());
    if (bytes_total == 0u ||
        bytes_total > static_cast<std::uint64_t>((std::numeric_limits<Size>::max)())) {
        return false;
    }
    ensure_persist_xact_callback_registered();

    struct staged_dsm_state {
        dsm_segment* attached = nullptr;
        dsm_handle handle = DSM_HANDLE_INVALID;
        bool unpin_cleanup_required = false;
    } dsm;
    const auto cleanup_dsm_noexcept = [&dsm]() noexcept {
        if (dsm.attached != nullptr) {
            dsm_segment* attached = std::exchange(dsm.attached, nullptr);
            try {
                bioseq::postgres::support::invoke_pg_as_cpp([attached] { dsm_detach(attached); });
            } catch (...) {
                // PostgreSQL still owns the mapping and will release it on abort.
                // Keep the original staging error.
            }
        }
        if (dsm.unpin_cleanup_required && dsm.handle != DSM_HANDLE_INVALID) {
            const dsm_handle handle = std::exchange(dsm.handle, DSM_HANDLE_INVALID);
            dsm.unpin_cleanup_required = false;
            try {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [handle] { dsm_unpin_segment(handle); });
            } catch (...) {
                // Keep an unpin failure from replacing the original staging error.
            }
        }
    };

    try {
        dsm.attached = invoke_async_minimizer_sequence_stage_pg_leaf(
            async_minimizer_sequence_stage_pg_failure::dsm_create, [bytes_total] {
                return dsm_create(static_cast<Size>(bytes_total), DSM_CREATE_NULL_IF_MAXSEGMENTS);
            });
        if (dsm.attached == nullptr) {
            return false;
        }

        // Save source identity and build settings, including the selected Highway
        // target. The worker resolves its own function pointers.
        char* base = static_cast<char*>(dsm_segment_address(dsm.attached));
        const minimizer_sequence_build_dsm_header header{
            .magic = k_minimizer_sequence_build_dsm_magic,
            .version = k_minimizer_sequence_build_wire_revision,
            .index_name_len = static_cast<std::uint32_t>(target.index_name.size()),
            .alphabet_code = alphabet_code,
            .storage_unit_byte_width = storage_unit_byte_width,
            .last_fragment_padding = last_fragment_padding,
            .pair_sort_impl = pair_sort_impl,
            .pipeline_mode = pipeline_mode,
            .max_concurrency = max_concurrency,
            .index_memory_cap_mb = index_memory_cap_mb,
            .index_sketch_backend = index_sketch_backend,
            .highway_executed_target = highway_executed_target,
            .locator = locator,
        };
        std::memcpy(base, &header, sizeof(header));
        std::memcpy(base + sizeof(header), target.index_name.data(), target.index_name.size());

        dsm.handle = dsm_segment_handle(dsm.attached);
        // Request unpin cleanup before calling PostgreSQL, covering an error
        // during pinning as well as the injected test failure before the call.
        dsm.unpin_cleanup_required = true;
        invoke_async_minimizer_sequence_stage_pg_leaf(
            async_minimizer_sequence_stage_pg_failure::dsm_pin,
            [&dsm] { dsm_pin_segment(dsm.attached); });
        invoke_async_minimizer_sequence_stage_pg_leaf(
            async_minimizer_sequence_stage_pg_failure::dsm_detach,
            [&dsm] { dsm_detach(dsm.attached); });
        dsm.attached = nullptr;

        enqueue_pending_minimizer_sequence_build(target, dsm.handle, bytes_total);
        // The transaction list owns the pin after insertion succeeds. Keep local
        // cleanup armed until any allocation needed by the insertion has finished.
        dsm.handle = DSM_HANDLE_INVALID;
        dsm.unpin_cleanup_required = false;
    } catch (...) {
        cleanup_dsm_noexcept();
        throw;
    }
    const std::uint64_t queued_ns =
        static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
    bioseq_log<logging_level::debug>(
        "[bioseq] index: staged async minimizer-sequence build job row={} index={} target_bp={} "
        "fragments={} mode={} max_concurrency={} index_memory_cap_mb={} meta_bytes={} "
        "total_stage_ns={}",
        static_cast<unsigned long long>(target.sequence_rowno),
        target.index_name,
        static_cast<unsigned long long>(locator.logical_length),
        static_cast<unsigned long long>(locator.fragment_count),
        pipeline_mode,
        max_concurrency,
        index_memory_cap_mb,
        static_cast<unsigned long long>(bytes_total),
        static_cast<unsigned long long>(queued_ns - stage_start_ns));
    return true;
}

} // namespace bioseq::postgres::scheduler
