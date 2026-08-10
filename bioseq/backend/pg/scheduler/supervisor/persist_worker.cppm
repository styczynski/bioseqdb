/**
 * @file persist_worker.cppm
 * @brief PostgreSQL asynchronous persistence queue and worker dispatch.
 */
module;

#include <bioseq/pg_compat.h>

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
import bioseq.common.logging;
import bioseq.postgres.support;
import bioseq.indexing.type;

namespace bioseq::postgres::scheduler {

template <class p_function>
auto invoke_async_minimizer_sequence_worker_pg_leaf(
        async_minimizer_sequence_worker_pg_failure point,
        p_function&& function)
    -> std::invoke_result_t<std::remove_reference_t<p_function>&> {
    record_async_minimizer_sequence_worker_pg_leaf_call(point);
    try {
        return bioseq::postgres::support::invoke_pg_as_cpp(
            std::forward<p_function>(function));
    } catch (const bioseq::postgres::support::postgres_error&) {
        record_async_minimizer_sequence_worker_pg_leaf_failure(point);
        throw;
    }
}

[[nodiscard]] std::optional<persist_claim> claim_persist_job() noexcept {
    if (g_shared == nullptr) {
        return std::nullopt;
    }
    for (std::size_t slot_index = 0;
         slot_index < scheduler_shared_state::k_persist_queue_size;
         ++slot_index) {
        pg_persist_queue_slot& slot = g_shared->persist_queue[slot_index];
        if (pg_atomic_read_u32(&slot.state) != 1) {
            continue;
        }
        const auto payload_kind = static_cast<persist_payload_kind>(
            slot.payload_kind);
        if (payload_kind != persist_payload_kind::direct_minimizer_build_identity) {
            static std::array<std::uint32_t,
                scheduler_shared_state::k_persist_queue_size>
                rejected_generations{};
            const std::uint32_t generation =
                pg_atomic_read_u32(&slot.generation);
            if (rejected_generations[slot_index] != generation) {
                rejected_generations[slot_index] = generation;
                record_unsupported_persist_rejection("queue_claim");
            }
            // Do not claim unsupported DSM/file payloads. The identity queue
            // replacement reclaims administratively quarantined slots.
            continue;
        }
        std::uint32_t expected = 1;
        if (!pg_atomic_compare_exchange_u32(&slot.state, &expected, 2)) {
            continue;
        }
        persist_claim claim{};
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
        return claim;
    }
    return std::nullopt;
}

void finish_persist_claim(const persist_claim& claim) noexcept {
    if (claim.payload_kind != persist_payload_kind::direct_minimizer_build_identity) {
        record_unsupported_persist_rejection("queue_ack");
        return;
    }
    if (g_shared == nullptr || claim.slot_index >= scheduler_shared_state::k_persist_queue_size) {
        release_pinned_dsm(claim.dsm);
        return;
    }
    pg_persist_queue_slot& slot = g_shared->persist_queue[claim.slot_index];
    release_pinned_dsm(claim.dsm);
    slot.dsm = DSM_HANDLE_INVALID;
    slot.payload_bytes = 0;
    slot.payload_kind = static_cast<std::uint32_t>(
        persist_payload_kind::none);
    slot.sequence_rowno = 0;
    pg_atomic_fetch_add_u32(&slot.generation, 1);
    pg_atomic_write_u32(&slot.state, 0);
}

[[nodiscard]] bool worker_target_accepts_index_persist(
        const persist_target& target,
        std::string_view index_name) {
    if (!OidIsValid(target.user_relid)) {
        return false;
    }

    // Relation operations are independent leaves. A live SPI session is one
    // composite PostgreSQL operation and must stay inside one checked leaf from
    // connect through finish; restoring the caller context between its stages
    // corrupts PostgreSQL 17's SPI procedure context.
    Relation rel = invoke_async_minimizer_sequence_worker_pg_leaf(
        async_minimizer_sequence_worker_pg_failure::relation_open,
        [relid = target.user_relid] {
            inject_async_minimizer_sequence_worker_pg_failure_leaf(
                async_minimizer_sequence_worker_pg_failure::relation_open);
            return bioseq::postgres::support::try_open_postgres_relation(relid, AccessShareLock);
        });
    if (rel == nullptr) {
        return false;
    }

    struct relation_identity {
        Oid tablespace = InvalidOid;
        Oid relfilenode = InvalidOid;
    };
    static_assert(std::is_trivially_destructible_v<relation_identity>);
    const relation_identity identity = invoke_async_minimizer_sequence_worker_pg_leaf(
        async_minimizer_sequence_worker_pg_failure::relation_identity,
        [rel] {
            inject_async_minimizer_sequence_worker_pg_failure_leaf(
                async_minimizer_sequence_worker_pg_failure::relation_identity);
            relation_identity result{};
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
    invoke_async_minimizer_sequence_worker_pg_leaf(
        async_minimizer_sequence_worker_pg_failure::relation_close,
        [rel] {
            inject_async_minimizer_sequence_worker_pg_failure_leaf(
                async_minimizer_sequence_worker_pg_failure::relation_close);
            relation_close(rel, AccessShareLock);
        });
    if (identity.tablespace != target.tablespace_oid
        || identity.relfilenode != target.main_relfilenode) {
        return false;
    }

    const std::string owned_index_name{index_name};
    const char* const index_name_cstr = owned_index_name.c_str();
    static constexpr char k_policy_sql[] =
        "SELECT enabled, update_on_insert "
        "FROM bioseq_internal.index_policy "
        "WHERE user_relid=$1 AND main_relfilenode=$2 "
        "AND column_attno=$3 AND index_name=$4 LIMIT 1";
    struct worker_policy_spi_state {
        async_minimizer_sequence_worker_pg_failure current_stage =
            async_minimizer_sequence_worker_pg_failure::none;
        int connect_status = 0;
        int execute_status = 0;
        int finish_status = 0;
        std::uint64_t processed = 0;
        bool table_present = false;
        bool enabled = true;
        bool update_on_insert = false;
    } spi{
        .update_on_insert =
            bioseq::indexing::is_default_update_on_insert_index(index_name),
    };
    try {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&spi,
             user_relid = target.user_relid,
             main_relfilenode = target.main_relfilenode,
             column_attno = target.column_attno,
             index_name_cstr] noexcept {
            spi.current_stage = async_minimizer_sequence_worker_pg_failure::spi_connect;
            record_async_minimizer_sequence_worker_pg_leaf_call(spi.current_stage);
            inject_async_minimizer_sequence_worker_pg_failure_leaf(spi.current_stage);
            spi.connect_status = SPI_connect();
            if (spi.connect_status != SPI_OK_CONNECT) {
                ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("asynchronous minimizer-sequence worker could not connect to SPI"),
                     errdetail("SPI_connect returned status %d",
                        spi.connect_status)));
            }

            spi.current_stage = async_minimizer_sequence_worker_pg_failure::spi_execute;
            record_async_minimizer_sequence_worker_pg_leaf_call(spi.current_stage);
            inject_async_minimizer_sequence_worker_pg_failure_leaf(spi.current_stage);
            Oid argument_types[4] = {OIDOID, OIDOID, INT2OID, TEXTOID};
            Datum arguments[4] = {
                ObjectIdGetDatum(user_relid),
                ObjectIdGetDatum(main_relfilenode),
                Int16GetDatum(column_attno),
                CStringGetTextDatum(index_name_cstr),
            };
            spi.execute_status = SPI_execute_with_args(
                k_policy_sql, 4, argument_types, arguments,
                nullptr, true, 1);
            if (spi.execute_status != SPI_OK_SELECT) {
                ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("asynchronous minimizer-sequence worker policy lookup failed"),
                     errdetail("SPI_execute_with_args returned status %d",
                        spi.execute_status)));
            }
            spi.processed = static_cast<std::uint64_t>(SPI_processed);
            spi.table_present = SPI_tuptable != nullptr;
            if (spi.processed > 0) {
                spi.current_stage = async_minimizer_sequence_worker_pg_failure::spi_read;
                record_async_minimizer_sequence_worker_pg_leaf_call(spi.current_stage);
                inject_async_minimizer_sequence_worker_pg_failure_leaf(spi.current_stage);
                if (SPI_tuptable == nullptr || SPI_tuptable->vals == nullptr) {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("asynchronous minimizer-sequence worker policy row is unavailable")));
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
                         errmsg("asynchronous minimizer-sequence worker policy contains NULL"),
                         errdetail("enabled_is_null=%s update_on_insert_is_null=%s",
                            enabled_is_null ? "true" : "false",
                            update_is_null ? "true" : "false")));
                }
                spi.enabled = DatumGetBool(enabled_datum);
                spi.update_on_insert = DatumGetBool(update_datum);
            }

            spi.current_stage = async_minimizer_sequence_worker_pg_failure::spi_finish;
            record_async_minimizer_sequence_worker_pg_leaf_call(spi.current_stage);
            inject_async_minimizer_sequence_worker_pg_failure_leaf(spi.current_stage);
            spi.finish_status = SPI_finish();
            if (spi.finish_status != SPI_OK_FINISH) {
                ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("asynchronous minimizer-sequence worker could not finish SPI"),
                     errdetail("SPI_finish returned status %d",
                        spi.finish_status)));
            }
            spi.current_stage = async_minimizer_sequence_worker_pg_failure::none;
        });
    } catch (const bioseq::postgres::support::postgres_error&) {
        record_async_minimizer_sequence_worker_pg_leaf_failure(spi.current_stage);
        throw;
    }
    return spi.enabled && spi.update_on_insert;
}



inline void finish_persist_claim_transaction(
        Snapshot registered_snapshot) {
    PopActiveSnapshot();
    UnregisterSnapshot(registered_snapshot);
    CommitTransactionCommand();
}


void process_minimizer_sequence_identity_claim(const persist_claim& claim) {
    if (claim.payload_kind != persist_payload_kind::direct_minimizer_build_identity) {
        record_unsupported_persist_rejection("worker_dispatch");
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("unsupported asynchronous persistence payload is prohibited"),
             errdetail("only the minimizer-sequence identity build payload is accepted")));
    }
    StartTransactionCommand();
    Snapshot registered_snapshot =
        RegisterSnapshot(GetTransactionSnapshot());
    PushActiveSnapshot(registered_snapshot);
    dsm_segment* segment = dsm_attach(claim.dsm);
    if (segment == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("minimizer-sequence identity build payload could not be attached")));
    }
    char* base = static_cast<char*>(dsm_segment_address(segment));
    const Size mapped = dsm_segment_map_length(segment);
    std::uint32_t magic = 0;
    if (mapped >= sizeof(magic)) {
        std::memcpy(&magic, base, sizeof(magic));
    }
    if (magic != k_minimizer_sequence_build_dsm_magic) {
        dsm_detach(segment);
        record_unsupported_persist_rejection("worker_magic");
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("unsupported asynchronous persistence DSM payload is prohibited"),
             errdetail("magic=%u expected_minimizer_sequence_identity_magic=%u",
                magic, k_minimizer_sequence_build_dsm_magic)));
    }
    process_minimizer_sequence_build_claim(
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
        bioseq_log<logging_level::debug>("[bioseq] index: discarded async persist job for db={} because worker is connected to db={}",
                static_cast<unsigned>(claim->target.db_oid),
                static_cast<unsigned>(g_persist_worker_connected_db));
        finish_persist_claim(*claim);
        return;
    }

    PG_TRY();
    {
        process_minimizer_sequence_identity_claim(*claim);
    }
    PG_CATCH();
    {
        AbortCurrentTransaction();
        FlushErrorState();
        bioseq_log<logging_level::debug>("[bioseq] index: async persist worker dropped failed job row={}",
                static_cast<unsigned long long>(claim->target.sequence_rowno));
    }
    PG_END_TRY();
    finish_persist_claim(*claim);
}


}  // namespace bioseq::postgres::scheduler
