/**
 * @file persist_queue_publish.cppm
 * @brief Publish committed index-build jobs for  the background worker
 */
module;

extern "C" {
#include "postgres.h"

#include "storage/dsm_impl.h"

#include "port/atomics.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

export module bioseq.postgres.scheduler.supervisor:persist_queue_publish;

export import bioseq.postgres.scheduler.persist_target;
import bioseq.common.logging;

namespace bioseq::postgres::scheduler {

/**
 * Shared queue slot for one index-build job.
 * A publisher claims a free slot before filling its fields, then marks it
 * ready for a worker. The generation changes whenever the elot is reused.
 */
struct pg_persist_queue_slot {
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
};

/** Identify the build-metadata format stored in the shared-memory segment. */
enum class persist_payload_kind : std::uint32_t {
    none = 0,
    direct_minimizer_build_identity = 3,
};

/** @brief Counts process-local rejections of unsupported persistence payloads. */
inline std::atomic<std::uint64_t> g_unsupported_persist_rejections{0};

/** Count and log a rejected persistence request under the given route name. */
void record_unsupported_persist_rejection(std::string_view route) noexcept {
    g_unsupported_persist_rejections.fetch_add(1, std::memory_order_relaxed);
    bioseq_log<logging_level::debug>(
        "[bioseq] unsupported asynchronous persistence rejected route={} total_rejections={}",
        route,
        static_cast<unsigned long long>(
            g_unsupported_persist_rejections.load(std::memory_order_relaxed)));
}

/** Round up to the requested byte alignment. Zero leaves the value, unchanged. */
[[nodiscard]] std::uint64_t align_up_u64(std::uint64_t value, std::uint64_t alignment) noexcept {
    return alignment == 0 ? value : ((value + alignment - 1) / alignment) * alignment;
}

/** Try to  remove a temporary payload file, ignoring an empty path or removal error. */
void remove_temp_file_noexcept(std::string_view path) noexcept {
    if (path.empty()) {
        return;
    }
    auto owned = std::string{path};
    (void)::remove(owned.c_str());
}

/**
 * Publish a build request in a free shared queue slot.
 * Success transfers responsibility for the pinned metadata segment to the
 * worker. Return false when the queue is full or unavailable, the handle is
 * invalid, or the payload format is unsupported.
 * @param persist_queue Shared queue slots.
 * @param target Table, column, and sequence identity to copy.
 * @param index_name Index name used only in the diagnostic log.
 * @param dsm Pinned segment transferred to the worker on  success.
 * @param payload_bytes Size of the mapped metadata in bytes.
 * @param payload_kind Format stored in the segment.
 */
[[nodiscard]] bool
publish_persist_job_to_shared_queue(std::span<pg_persist_queue_slot> persist_queue,
                                    const persist_target& target,
                                    std::string_view index_name,
                                    dsm_handle dsm,
                                    std::uint64_t payload_bytes,
                                    persist_payload_kind payload_kind) noexcept {
    if (persist_queue.empty() || dsm == DSM_HANDLE_INVALID) {
        return false;
    }
    if (payload_kind != persist_payload_kind::direct_minimizer_build_identity) {
        record_unsupported_persist_rejection("queue_publish");
        return false;
    }
    for (pg_persist_queue_slot& slot : persist_queue) {
        auto expected = std::uint32_t{0};
        if (!pg_atomic_compare_exchange_u32(&slot.state, &expected, 3)) {
            continue;
        }
        slot.db_oid = target.db_oid;
        slot.user_relid = target.user_relid;
        slot.tablespace_oid = target.tablespace_oid;
        slot.main_relfilenode = target.main_relfilenode;
        slot.column_attno = target.column_attno;
        slot.bucket_idx = target.bucket_idx;
        slot.shard_idx = target.shard_idx;
        slot.sequence_rowno = target.sequence_rowno;
        slot.payload_bytes = payload_bytes;
        slot.dsm = dsm;
        slot.payload_kind = static_cast<std::uint32_t>(payload_kind);
        pg_atomic_fetch_add_u32(&slot.generation, 1);
        pg_atomic_write_u32(&slot.state, 1);
        bioseq_log<logging_level::debug>(
            "[bioseq] index: published async persist job row={} index={} bytes={}",
            static_cast<unsigned long long>(target.sequence_rowno),
            index_name,
            static_cast<unsigned long long>(payload_bytes));
        return true;
    }
    return false;
}

} // namespace bioseq::postgres::scheduler
