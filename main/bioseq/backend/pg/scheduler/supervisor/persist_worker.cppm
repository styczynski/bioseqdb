/**
 * @file persist_worker.cppm
 * @brief PostgreSQL asynchronous persistence queue and worker dispatch.
 */
module;

#include "common.h"

extern "C" {
#include "access/xact.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/dsm.h"
#include "storage/dsm_impl.h"
#include "utils/snapmgr.h"

void StartTransactionCommand(void);
void CommitTransactionCommand(void);
void AbortCurrentTransaction(void);
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

module bioseq.postgres.scheduler.supervisor;

import :persist_queue_publish;
import bioseq.common.mlogging;
import bioseq.config;
import bioseq.pg_utils;
import bioseq.indexing.type;

namespace bioseq::parallelism::pg {

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


}  // namespace bioseq::parallelism::pg
