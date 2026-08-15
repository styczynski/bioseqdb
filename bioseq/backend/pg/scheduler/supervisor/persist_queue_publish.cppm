/**
 * @file persist_queue_publish.cppm
 * @brief Shared asynchronous persistence queue publication.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "port/atomics.h"
#include "storage/dsm_impl.h"
}

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

export module bioseq.postgres.scheduler.supervisor:persist_queue_publish;

import bioseq.common.logging;

export namespace bioseq::postgres::scheduler {

/** @brief Identifies one sequence index eligible for asynchronous persistence. */
struct persist_target {
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

} // namespace bioseq::postgres::scheduler

namespace bioseq::postgres::scheduler {

/** @brief Shared-memory record used to publish one persistence job. */
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

/** @brief Identifies the validated payload representation stored in DSM. */
enum class persist_payload_kind : std::uint32_t {
    none = 0,
    direct_minimizer_build_identity = 3,
};

inline std::atomic<std::uint64_t> g_unsupported_persist_rejections{0};

/**
 * @brief Records a rejected persistence route without crossing a C boundary.
 * @param[in] route Stable route name used in the diagnostic log.
 */
void record_unsupported_persist_rejection(
        std::string_view route) noexcept {
    g_unsupported_persist_rejections.fetch_add(
        1, std::memory_order_relaxed);
    bioseq_log<logging_level::debug>(
        "[bioseq] unsupported asynchronous persistence rejected route={} total_rejections={}",
        route,
        static_cast<unsigned long long>(
            g_unsupported_persist_rejections.load(
                std::memory_order_relaxed)));
}

/**
 * @brief Rounds a value up to the requested alignment.
 * @return The aligned value or the input when alignment is zero.
 */
[[nodiscard]] std::uint64_t align_up_u64(
        std::uint64_t value,
        std::uint64_t alignment) noexcept {
    return alignment == 0
        ? value
        : ((value + alignment - 1) / alignment) * alignment;
}

/** @brief Removes a temporary payload file when a path is present. */
void remove_temp_file_noexcept(std::string_view path) noexcept {
    if (path.empty()) {
        return;
    }
    std::string owned{path};
    std::remove(owned.c_str());
}

/**
 * @brief Publishes one validated DSM payload into the shared persistence queue.
 * @return True when an empty slot accepted the complete job record.
 */
[[nodiscard]] bool publish_persist_job_to_shared_queue(
        std::span<pg_persist_queue_slot> persist_queue,
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
        std::uint32_t expected = 0;
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
        bioseq_log<logging_level::debug>("[bioseq] index: published async persist job row={} index={} bytes={}",
                static_cast<unsigned long long>(target.sequence_rowno),
                index_name,
                static_cast<unsigned long long>(payload_bytes));
        return true;
    }
    return false;
}

} // namespace bioseq::postgres::scheduler
