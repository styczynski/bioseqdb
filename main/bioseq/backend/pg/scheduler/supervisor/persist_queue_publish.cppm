/**
 * @file persist_queue_publish.cppm
 * @brief Shared asynchronous persistence queue publication.
 */
module;

#include "common.h"

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

import bioseq.common.mlogging;
import bioseq.config;

export namespace bioseq::parallelism::pg {

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

} // namespace bioseq::parallelism::pg

namespace bioseq::parallelism::pg {

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

enum class PersistPayloadKind : std::uint32_t {
    none = 0,
    prohibited_legacy_dsm = 1,
    prohibited_legacy_file = 2,
    mps_build_identity = 3,
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

[[nodiscard]] std::uint64_t align_up_u64(
        std::uint64_t value,
        std::uint64_t alignment) noexcept {
    return alignment == 0
        ? value
        : ((value + alignment - 1) / alignment) * alignment;
}

void remove_temp_file_noexcept(std::string_view path) noexcept {
    if (path.empty()) {
        return;
    }
    std::string owned{path};
    std::remove(owned.c_str());
}

[[nodiscard]] bool publish_persist_job_to_shared_queue(
        std::span<PgPersistQueueSlot> persist_queue,
        const PersistTarget& target,
        std::string_view index_name,
        dsm_handle dsm,
        std::uint64_t payload_bytes,
        PersistPayloadKind payload_kind,
        IndexPayloadStorageBackend requested_provider) noexcept {
    if (persist_queue.empty() || dsm == DSM_HANDLE_INVALID) {
        return false;
    }
    if (payload_kind != PersistPayloadKind::mps_build_identity) {
        record_prohibited_legacy_persist_rejection("queue_publish");
        return false;
    }
    if (!is_valid_index_payload_storage_backend_wire_value(
            index_payload_storage_backend_wire_value(
                requested_provider))
        || requested_provider
            != IndexPayloadStorageBackend::postgres_relation) {
        record_prohibited_legacy_persist_rejection(
            "queue_publish_provider");
        return false;
    }
    for (PgPersistQueueSlot& slot : persist_queue) {
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
        slot.requested_provider =
            index_payload_storage_backend_wire_value(
                requested_provider);
        pg_atomic_fetch_add_u32(&slot.generation, 1);
        pg_atomic_write_u32(&slot.state, 1);
        bioseq_log<DBG>("[bioseq] index: published async persist job row={} index={} bytes={}",
                static_cast<unsigned long long>(target.sequence_rowno),
                index_name,
                static_cast<unsigned long long>(payload_bytes));
        return true;
    }
    return false;
}

} // namespace bioseq::parallelism::pg
