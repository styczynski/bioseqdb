/**
 * @file catalog_ddl.cppm
 * @brief Create and open sequence shards and manage cached PostgreSQL resources.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/genam.h"
#include "access/xact.h"
#include "executor/spi.h"
#include "storage/lmgr.h"
#include "utils/snapmgr.h"
#include "utils/resowner.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/indexing.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_opclass_d.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_type_d.h"
#include "commands/extension.h"
#include "commands/defrem.h"
#include "commands/sequence.h"
#include "commands/vacuum.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "parser/analyze.h"
#include "parser/parser.h"
#include "rewrite/rewriteHandler.h"
#include "utils/tuplestore.h"
}

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.postgres.storage.manager:sequence_storage_catalog_ddl;

import bioseq.common.logging;
import bioseq.postgres.support;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.configuration.core;
import bioseq.postgres.configuration.storage;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.physical;
import bioseq.postgres.table.io;
import bioseq.postgres.types.compact_sequence;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.indexing.storage;
import bioseq.common.seqan_adapter;  // ceil_log2 for calculating letters per  storage unit.
import bioseq.storage.size;
import bioseq.storage.coordinates;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.storage.manager.sql_rows;
import bioseq.postgres.storage.manager.sql_row_codecs;
import bioseq.postgres.sql.generated.core.storage_manager_sequence_storage_manager.entity_handles;
// Bucket calculations and sequence_locator are independent of PostgreSQL
// so unit tests can use them.
import bioseq.storage.layout;
import :sequence_storage_catalog;
import :sequence_storage_layout_cache;



namespace bioseq::storage {

/// Advisory-lock class reserved for sequence shard allocation.
constexpr std::int64_t k_sequence_storage_advisory_lock_class = 0x42105260LL;

/// Build the transaction advisory-lock key for allocating a bucket's shards.
/// @return Key derived from database, table file, column and bucket.
[[nodiscard]] static std::int64_t
shard_alloc_lock_key(Oid db_oid, Oid main_relfilenode,
                     std::int16_t column_attno, std::int16_t bucket_idx) noexcept {
    return (static_cast<std::int64_t>(db_oid)                << 48)
         ^ (static_cast<std::int64_t>(main_relfilenode)      << 16)
         ^ (static_cast<std::int64_t>(
                static_cast<std::uint16_t>(column_attno))    << 8)
         ^  static_cast<std::int64_t>(
                static_cast<std::uint16_t>(bucket_idx));
}

/// Read the sequence alphabet and packed-unit width from a column's typmod.
/// @param parent_tupdesc Borrowed parent table descriptor.
/// @param column_attno Column number, starting at one.
/// @return Alphabet name and storage-unit width in bits.
/// @throws A PostgreSQL error for an invalid column or typmod.
sequence_column_type_parameters column_typmod_at(TupleDesc parent_tupdesc, std::int16_t column_attno) {
    if (column_attno <= 0 || column_attno > parent_tupdesc->natts) {
        storage_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            fmt::format(
                "bioseq storage: column_attno {} out of bounds (natts={})",
                column_attno, parent_tupdesc->natts));
    }
    Form_pg_attribute attr = bioseq::postgres::support::tuple_descriptor_attribute(parent_tupdesc, column_attno - 1);
    const std::int32_t typmod = attr->atttypmod;
    if (typmod < 0) {
        storage_error_as_cpp(
            ERRCODE_INVALID_TABLE_DEFINITION,
            fmt::format(
                "bioseq storage: SEQ column attno={} has no typmod; "
                "requires every bioseq.seq column to carry an "
                "(alphabet, width) typmod",
                column_attno));
    }
    const compact_sequence_stdc_type_params params =
        compact_sequence_stdc_type_params::decode(typmod);
    std::string_view name_sv = bioseq::pg_types::seq::name_of_alphabet_code(params.alphabet_code);
    if (name_sv == std::string_view{"?"}) {
        storage_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            fmt::format(
                "bioseq storage: alphabet_code={} not in "
                "pgseq_supported_alphabets",
                static_cast<unsigned>(params.alphabet_code)));
    }
    sequence_column_type_parameters out;
    out.alphabet_name.assign(name_sv.data(), name_sv.size());
    out.storage_unit_bit_width =
        static_cast<int>(params.storage_unit_byte_width) * 8;
    return out;
}

/// Choose  the shard's SQL persistence keyword from the parent table.
/// @param main_relfilenode Parent table file number.
/// @return UNLOGGED, TEMPORARY, or an empty string for a permanent or missing parent.
const char*
persistence_keyword_for_relfilenode(Oid /*db_oid*/, Oid /*spc_oid*/,
                                    Oid main_relfilenode) {
    const Oid main_relid = RelidByRelfilenumber(InvalidOid, main_relfilenode);
    if (!OidIsValid(main_relid)) {
        return "";
    }
    Relation rel = bioseq::postgres::support::try_open_postgres_relation(main_relid, AccessShareLock);
    if (rel == nullptr) {
        return "";
    }
    const char persistence = rel->rd_rel->relpersistence;
    relation_close(rel, AccessShareLock);
    switch (persistence) {
        case RELPERSISTENCE_UNLOGGED: return "UNLOGGED";
        case RELPERSISTENCE_TEMP:     return "TEMPORARY";
        default:                      return "";
    }
}

/// Build the fragment table name for a parent column, bucket and shard.
std::string format_sub_table_name(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                                  std::int16_t column_attno, std::int16_t bucket_idx,
                                  std::int32_t shard_idx) {
    return fmt::format("seq_sub_{}_{}_{}_{}_{}_{}",
                       static_cast<unsigned>(db_oid),
                       static_cast<unsigned>(spc_oid),
                       static_cast<unsigned>(main_relfilenode),
                       static_cast<int>(column_attno),
                       static_cast<int>(bucket_idx),
                       static_cast<int>(shard_idx));
}

/// Build the sidecar table name for a parent column, bucket and shard.
std::string format_sidecar_name(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                                std::int16_t column_attno, std::int16_t bucket_idx,
                                std::int32_t shard_idx) {
    return fmt::format("idx_sidecar_{}_{}_{}_{}_{}_{}",
                       static_cast<unsigned>(db_oid),
                       static_cast<unsigned>(spc_oid),
                       static_cast<unsigned>(main_relfilenode),
                       static_cast<int>(column_attno),
                       static_cast<int>(bucket_idx),
                       static_cast<int>(shard_idx));
}

/// Read the configured shard size limit in bytes.
/// @return Zero when the configured GiB value is nonpositive.
sequence_size_t read_max_sub_table_cap_bytes() {
    const int guc = std::atomic_ref<int>(bioseq::storage::settings::sequence_shard::max_size_gib)
                        .load(std::memory_order_relaxed);
    if (guc <= 0) {
        return 0;
    }
    return static_cast<sequence_size_t>(guc) * (sequence_size_t{1} << 30);
}

}  // namespace bioseq::storage

namespace bioseq::storage {

/// Destroy a cache whose PostgreSQL resources have already been closed.
/// @param caches Separately allocated cache, or null for no work.
void destroy_seq_caches(seq_caches* caches) noexcept {
    if (caches == nullptr) {
        return;
    }
    caches->~seq_caches();
    pfree(caches);
}

/// Close the cache's PostgreSQL resources and free the cache
/// @param caches Allocated cache, or null.
/// @param commit Apply pendinj  shard-byte counts before closing.
void close_and_destroy_seq_caches(seq_caches* caches, bool commit) noexcept {
    if (caches == nullptr) {
        return;
    }
    close_seq_caches(*caches, commit);
    destroy_seq_caches(caches);
}

/// Close cached readers and relations under, the resource owners that opened them.
/// @param commit Apply pending shard-byte counts before closing write-side entries.
///
/// The transaction write-state cache keeps ownership of shard writers. Release
/// each cached relation with this cache's lock_mode and clear the entries.
void seq_caches::close_all(bool commit) noexcept {
    ErrorData* exact_error = nullptr;
    bool cpp_error = false;
    bool unknown_cpp_error = false;
    char detail[512] = "std::exception without detail";
    {
        try {
            for (auto& kv : entries) {
                const std::uint64_t key = kv.first;
                entry& e = kv.second;
                const std::int16_t column_attno =
                    static_cast<std::int16_t>(key >> 48);
                const std::int16_t bucket_idx =
                    static_cast<std::int16_t>((key >> 32) & 0xFFFFu);
                const std::int32_t shard_idx =
                    static_cast<std::int32_t>(key & 0xFFFFFFFFu);

                if (side == access_side::write) {
                    if (commit && e.pending_bytes_delta != 0) {
                        (void) bioseq::postgres::support::invoke_pg_as_cpp(
                            [this, column_attno, bucket_idx, shard_idx, &e] {
                            return sequence_sub_table_manager::update_shard_bytes_written(
                                main_database_oid, main_tablespace_oid,
                                main_relfilenode, column_attno, bucket_idx,
                                shard_idx, e.pending_bytes_delta,
                                cap_bytes_snapshot);
                        });
                    }
                    e.pending_bytes_delta = 0;
                    e.pending_total_payload_delta = 0;
                    // The transaction write-state cache commits this writer with the parent.
                    // Closing it here would flush it twice.
                    e.write_state = nullptr;
                } else {
                    if (e.read_state != nullptr) {
                        column_read_state* read_state = e.read_state;
                        const ResourceOwner saved_owner = CurrentResourceOwner;
                        bioseq::postgres::support::invoke_pg_as_cpp([read_state,
                                                           saved_owner,
                                                           this] {
                            if (read_resource_owner != nullptr) {
                                CurrentResourceOwner = read_resource_owner;
                            }
                            end_column_read(read_state);
                            CurrentResourceOwner = saved_owner;
                        });
                        e.read_state = nullptr;
                    }
                    e.read_snapshot_source = nullptr;
                }
                if (e.rel != nullptr) {
                    Relation relation = e.rel;
                    if (side == access_side::write) {
                        const ResourceOwner saved_owner = CurrentResourceOwner;
                        bioseq::postgres::support::invoke_pg_as_cpp(
                            [relation, saved_owner, this] {
                                CurrentResourceOwner =
                                    TopTransactionResourceOwner;
                                relation_close(relation, lock_mode);
                                CurrentResourceOwner = saved_owner;
                            });
                    } else if (read_resource_owner != nullptr) {
                        const ResourceOwner saved_owner = CurrentResourceOwner;
                        bioseq::postgres::support::invoke_pg_as_cpp(
                            [relation, saved_owner, this] {
                                CurrentResourceOwner = read_resource_owner;
                                relation_close(relation, lock_mode);
                                CurrentResourceOwner = saved_owner;
                            });
                    } else {
                        bioseq::postgres::support::invoke_pg_as_cpp(
                            [relation, this] {
                                relation_close(relation, lock_mode);
                            });
                    }
                    e.rel = nullptr;
                }
            }
            entries.clear();
        } catch (bioseq::postgres::support::postgres_error& error) {
            exact_error = error.release_error_data();
        } catch (const std::exception& error) {
            strlcpy(
                detail,
                error.what() == nullptr
                    ? "std::exception without detail"
                    : error.what(),
                sizeof(detail));
            cpp_error = true;
        } catch (...) {
            unknown_cpp_error = true;
        }
    }

    // All C++ iterators and entry references have left scope. It is now safe
    // to report the  saved error through PostgreSQL's nonlocal error handling.
    if (exact_error != nullptr) ReThrowError(exact_error);
    if (cpp_error) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq sequence-cache teardown raised a C++ exception"),
             errdetail_internal("%s", detail)));
    }
    if (unknown_cpp_error) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq sequence-cache teardown raised an unknown C++ exception")));
    }
}


/// Flush fragment write buffers so an index scan in this statement can see the rows.
/// Keep the writers, relation references and locks open.
/// @param caches Write cache, or null for no work.
/// @param current_sub_xid Subtransaction owning the write states.
void flush_seq_cache_write_states(seq_caches* caches,
                                  SubTransactionId current_sub_xid) {
    if (caches == nullptr || caches->side != seq_caches::access_side::write) return;
    for (auto& kv : caches->entries) {
        seq_caches::entry& e = kv.second;
        if (e.rel != nullptr) {
            flush_relation_write_state(e.rel->rd_locator.relNumber,
                                          current_sub_xid);
        }
    }
}

// Open, create and find sequence shards.

/// Open a shard and optionally keep its relation reference in the cache.
/// @param sub_table_relid Fragment table OID.
/// @param lock_mode Lock for a newly opened relation.
/// @param cache Optional relation cache.
/// @return Borrowed cached relation, or a ielation the caller must close with lock_mode.
/// @throws A PostgreSQL error if opening fails.
[[nodiscard]] Relation
open_relation_cached(Oid sub_table_relid, LOCKMODE lock_mode,
                     seq_caches* cache,
                     std::int16_t column_attno, std::int16_t bucket_idx,
                     std::int32_t shard_idx)
{
    if (cache != nullptr) {
        const std::uint64_t key = seq_caches::key_for(column_attno, bucket_idx, shard_idx);
        auto it = cache->entries.find(key);
        if (it != cache->entries.end() && it->second.rel != nullptr) {
            return it->second.rel;
        }
        // Open  and close a cached relation under the same ResourceOwner. Write
        // caches survive statements, so their references belong to
        // TopTransactionResourceOwner. Read caches use their saved scan owner,
        // or the current owner if none was saved A mismatched owner can close
        // a reference too early or reject it during scan cleanup.
        Relation rel = nullptr;
        if (cache->side == seq_caches::access_side::write) {
            const ResourceOwner saved_owner = CurrentResourceOwner;
            rel = bioseq::postgres::support::invoke_pg_as_cpp(
                [sub_table_relid, lock_mode, saved_owner] {
                    CurrentResourceOwner = TopTransactionResourceOwner;
                    Relation result = bioseq::postgres::support::try_open_postgres_relation(
                        sub_table_relid, lock_mode);
                    CurrentResourceOwner = saved_owner;
                    return result;
                });
        } else if (cache->read_resource_owner != nullptr) {
            const ResourceOwner saved_owner = CurrentResourceOwner;
            rel = bioseq::postgres::support::invoke_pg_as_cpp(
                [sub_table_relid, lock_mode, saved_owner, cache] {
                    CurrentResourceOwner = cache->read_resource_owner;
                    Relation result = bioseq::postgres::support::try_open_postgres_relation(
                        sub_table_relid, lock_mode);
                    CurrentResourceOwner = saved_owner;
                    return result;
                });
        } else {
            rel = bioseq::postgres::support::invoke_pg_as_cpp(
                [sub_table_relid, lock_mode] {
                    return bioseq::postgres::support::try_open_postgres_relation(
                        sub_table_relid, lock_mode);
                });
        }
        if (rel == nullptr) {
            storage_error_as_cpp(
                ERRCODE_UNDEFINED_TABLE,
                fmt::format(
                    "bioseq storage: failed to open sub-table relid {}",
                    static_cast<unsigned>(sub_table_relid)));
        }
        seq_caches::entry& entry = cache->entries[key];
        entry.rel = rel;
        return rel;
    }
    Relation rel = bioseq::postgres::support::invoke_pg_as_cpp(
        [sub_table_relid, lock_mode] {
            return bioseq::postgres::support::try_open_postgres_relation(sub_table_relid, lock_mode);
        });
    if (rel == nullptr) {
        storage_error_as_cpp(
            ERRCODE_UNDEFINED_TABLE,
            fmt::format(
                "bioseq storage: failed to open sub-table relid {}",
                static_cast<unsigned>(sub_table_relid)));
    }
    return rel;
}

/// Create a fragment table and sidecar together in a subtransaction.
/// @param ddl SQL creating both tables.
/// @param sub_name Fragment table name.
/// @param sidecar_name Sidecar, table name.
/// @param new_sub_relid Receives the fragment table OID after  success.
/// @param new_sidecar_relid Receives the sidecar OID after success.
/// @throws A PostgreSQL error if creation or lookup fails.
void create_shard_relations_checked(
        StringInfo ddl,
        const char* sub_name,
        const char* sidecar_name,
        Oid& new_sub_relid,
        Oid& new_sidecar_relid) {
    auto subtransaction =
        bioseq::postgres::support::subtransaction::begin();
    bioseq::postgres::support::spi_session spi;
    const int connect_status = spi.connect();
    if (connect_status != SPI_OK_CONNECT) {
        storage_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            fmt::format(
                "bioseq storage: SPI_connect failed while allocating "
                "shard (status={})",
                connect_status));
    }
    const int execute_status =
        bioseq::postgres::support::invoke_pg_as_cpp(
            [ddl] {
                return SPI_execute(
                    ddl->data, /*read_only=*/false, /*tcount=*/0);
            });
    if (execute_status != SPI_OK_UTILITY
        && execute_status != SPI_OK_SELECT) {
        storage_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            fmt::format(
                "bioseq storage: failed to create sub-table {} and "
                "sidecar {} (SPI status={})",
                sub_name,
                sidecar_name,
                execute_status));
    }
    const int finish_status = spi.finish();
    if (finish_status != SPI_OK_FINISH) {
        storage_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            fmt::format(
                "bioseq storage: SPI_finish failed while allocating "
                "shard (status={})",
                finish_status));
    }
    const auto [created_sub_relid, created_sidecar_relid] =
        bioseq::postgres::support::invoke_pg_as_cpp(
            [sub_name, sidecar_name] {
                const Oid namespace_oid =
                    get_namespace_oid("bioseq_internal", false);
                return std::pair{
                    get_relname_relid(sub_name, namespace_oid),
                    get_relname_relid(sidecar_name, namespace_oid)};
            });
    if (!OidIsValid(created_sub_relid)
        || !OidIsValid(created_sidecar_relid)) {
        storage_error_as_cpp(
            ERRCODE_UNDEFINED_TABLE,
            "bioseq storage: sub-table or sidecar not found after CREATE");
    }
    subtransaction.commit();
    new_sub_relid = created_sub_relid;
    new_sidecar_relid = created_sidecar_relid;
}

/// Allocate a shard after taking the bucket's transaction advisory lock.
/// Reuse a suitable shard if another writer created one before the lock was acquired.
/// @param starting_shard_idx First candidate shard number.
/// @param lock_mode Lock for the selected shard.
/// @param cache Optional relation cache.
/// @return Open fragment table.
/// @throws A PostgreSQL error if allocation or opening fails.
[[nodiscard]] Relation
allocate_new_shard_and_open(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                            std::int16_t column_attno, TupleDesc parent_tupdesc,
                            std::int16_t bucket_idx, std::int32_t starting_shard_idx,
                            LOCKMODE lock_mode, seq_caches* cache)
{
    const std::int64_t lock_key =
        shard_alloc_lock_key(db_oid, main_relfilenode, column_attno, bucket_idx);
    bioseq::postgres::support::invoke_pg_as_cpp([lock_key] {
        (void) DirectFunctionCall1(
            pg_advisory_xact_lock_int8, Int64GetDatum(lock_key));
    });

    if (auto rechecked = bioseq::postgres::support::invoke_pg_as_cpp(
            [db_oid, spc_oid, main_relfilenode, column_attno,
             bucket_idx] {
            return sequence_sub_table_manager::lookup_active_shard(
                db_oid, spc_oid, main_relfilenode,
                column_attno, bucket_idx);
        });
        rechecked.has_value() && rechecked->shard_idx >= starting_shard_idx)
    {
        return open_relation_cached(rechecked->sub_table_relid, lock_mode, cache,
                                    column_attno, bucket_idx, rechecked->shard_idx);
    }

    const sequence_column_type_parameters tm = bioseq::postgres::support::invoke_pg_as_cpp(
        [parent_tupdesc, column_attno] {
            return column_typmod_at(parent_tupdesc, column_attno);
        });
    const char* const persistence_kw = bioseq::postgres::support::invoke_pg_as_cpp(
        [db_oid, spc_oid, main_relfilenode] {
            return persistence_keyword_for_relfilenode(
                db_oid, spc_oid, main_relfilenode);
        });

    const std::string sub_name = format_sub_table_name(
        db_oid, spc_oid, main_relfilenode, column_attno, bucket_idx, starting_shard_idx);
    const std::string sidecar_name = format_sidecar_name(
        db_oid, spc_oid, main_relfilenode, column_attno, bucket_idx, starting_shard_idx);

    // Create the fragment table  and sidecar when first needed. Track them in
    // seq_sub_table so storage drop hooks can remove them. Registering tables
    // through ALTER EXTENSION here requires nested SPI and SECURITY DEFINER
    // resorce owners, which can break tuple-descriptor cleanup.
    StringInfo ddl = bioseq::postgres::support::invoke_pg_as_cpp(
        [persistence_kw, &sub_name, &tm, &sidecar_name] {
        StringInfo result = makeStringInfo();
        appendStringInfo(result,
            "CREATE %s TABLE IF NOT EXISTS bioseq_internal.%s "
                "(seq bioseq.seq('%s', %d)) USING bioseq;\n"
            "CREATE %s TABLE IF NOT EXISTS bioseq_internal.%s "
                "(sub_rowno BIGINT PRIMARY KEY, blob BYTEA NOT NULL);",
            persistence_kw, quote_identifier(sub_name.c_str()),
            tm.alphabet_name.c_str(),
            tm.storage_unit_bit_width,
            persistence_kw, quote_identifier(sidecar_name.c_str()));
        return result;
    });

    Oid new_sub_relid = InvalidOid;
    Oid new_sidecar_relid = InvalidOid;

    // The helper owns the subtransaction and restores memory and resource
    // owners after commit or rollback. The name strings stay in this C++ frame.
    create_shard_relations_checked(
        ddl, sub_name.c_str(), sidecar_name.c_str(),
        new_sub_relid, new_sidecar_relid);

    sequence_sub_table_row row =
        bioseq::postgres::support::invoke_pg_as_cpp(
            [db_oid, spc_oid, main_relfilenode, column_attno, bucket_idx,
             starting_shard_idx, new_sub_relid, new_sidecar_relid] {
        return sequence_sub_table_manager::insert_shard(
            db_oid, spc_oid, main_relfilenode,
            column_attno, bucket_idx, starting_shard_idx,
            new_sub_relid, new_sidecar_relid);
    });

    if (row.sub_table_relid != new_sub_relid) {
        // Another writer registered a different shard. Drop the unused tables
        // created here. Orphan cleanup can find leftovers after a  failure
        StringInfo drop = bioseq::postgres::support::invoke_pg_as_cpp(
            [&sub_name, &sidecar_name] {
            StringInfo result = makeStringInfo();
            appendStringInfo(result,
                "DROP TABLE bioseq_internal.%s; "
                "DROP TABLE bioseq_internal.%s;",
                quote_identifier(sub_name.c_str()),
                quote_identifier(sidecar_name.c_str()));
            return result;
        });
        /// Save SPI status codes for checking after the cleanup call.
        struct race_drop_spi_state {
            int connect_status = 0;
            int execute_status = 0;
            int finish_status = 0;
        } spi{};
        const char* drop_sql = drop->data;
        bioseq::postgres::support::invoke_pg_as_cpp([&spi, drop_sql] noexcept {
            spi.connect_status = SPI_connect();
            if (spi.connect_status != SPI_OK_CONNECT) return;
            spi.execute_status = SPI_execute(
                drop_sql, /*read_only=*/false, /*tcount=*/0);
            spi.finish_status = SPI_finish();
        });
        if (spi.connect_status != SPI_OK_CONNECT
            || spi.execute_status != SPI_OK_UTILITY
            || spi.finish_status != SPI_OK_FINISH) {
            bioseq::postgres::support::invoke_pg_as_cpp([&spi] {
                ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("bioseq race-loser shard cleanup failed"),
                     errdetail("SPI_connect_status=%d SPI_execute_status=%d "
                               "SPI_finish_status=%d",
                               spi.connect_status, spi.execute_status,
                               spi.finish_status)));
            });
            pg_unreachable();
        }
    }

    return open_relation_cached(row.sub_table_relid, lock_mode, cache,
                                column_attno, bucket_idx, row.shard_idx);
}

/// Open the active shard, or allocate one after the highest existing shard number.
/// @param parent_tupdesc Descriptor used to create a shard.
/// @param lock_mode Lock for the selected shard.
/// @param cache Optional relation cache.
/// @return Open fragment table.
/// @throws A PostgreSQL error if lookup or allocation fails.
[[nodiscard]] Relation
open_or_create_shard(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                     std::int16_t column_attno, TupleDesc parent_tupdesc,
                     std::int16_t bucket_idx,
                     LOCKMODE lock_mode, seq_caches* cache)
{
    if (auto active = bioseq::postgres::support::invoke_pg_as_cpp(
            [db_oid, spc_oid, main_relfilenode, column_attno,
             bucket_idx] {
            return sequence_sub_table_manager::lookup_active_shard(
                db_oid, spc_oid, main_relfilenode,
                column_attno, bucket_idx);
        });
        active.has_value())
    {
        return open_relation_cached(active->sub_table_relid, lock_mode, cache,
                                    column_attno, bucket_idx, active->shard_idx);
    }
    // All existing shards may be full. Choose the next number after the
    // highest shard for this column and bucket so its catalog key stays unique.
    std::int32_t next_shard_idx = 0;
    const auto shards = bioseq::postgres::support::invoke_pg_as_cpp(
        [db_oid, spc_oid, main_relfilenode] {
        return sequence_sub_table_manager::list_shards_for_main(
            db_oid, spc_oid, main_relfilenode);
    });
    for (const auto& r : shards) {
        if (r.column_attno == column_attno && r.bucket_idx == bucket_idx
            && r.shard_idx >= next_shard_idx) {
            next_shard_idx = r.shard_idx + 1;
        }
    }
    return allocate_new_shard_and_open(db_oid, spc_oid, main_relfilenode,
                                       column_attno, parent_tupdesc, bucket_idx,
                                       next_shard_idx,
                                       lock_mode, cache);
}

/// Open the, fragment table selectd  by a sequence locator.
/// @param loc Sequence bucket and shard.
/// @param lock_mode Lock for a new cache entry.
/// @param cache Optional relation cache.
/// @return Open fragment table.
/// @throws A PostgreSQL error if the shard registration or table is missing.
[[nodiscard]] Relation
open_sub_table_for_locator(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                           std::int16_t column_attno, const sequence_locator& loc,
                           LOCKMODE lock_mode, seq_caches* cache)
{
    if (cache != nullptr) {
        const std::uint64_t key = seq_caches::key_for(
            column_attno,
            static_cast<std::int16_t>(loc.bucket_index),
            static_cast<std::int32_t>(loc.shard_index));
        auto it = cache->entries.find(key);
        if (it != cache->entries.end() && it->second.rel != nullptr) {
            return it->second.rel;
        }
    }
    auto row = sequence_sub_table_manager::lookup_shard(db_oid, spc_oid, main_relfilenode,
        column_attno, static_cast<std::int16_t>(loc.bucket_index),
        static_cast<std::int32_t>(loc.shard_index));
    if (!row.has_value()) {
        bioseq_log<logging_level::error, logging_target::postgres>(
               "bioseq storage: locator references nonexistent shard "
               "(db={}, spc={}, relfilenode={}, attno={}, bucket={}, shard={})",
               static_cast<unsigned>(db_oid),
               static_cast<unsigned>(spc_oid),
               static_cast<unsigned>(main_relfilenode),
               static_cast<int>(column_attno),
               static_cast<unsigned>(loc.bucket_index),
               loc.shard_index);
    }
    return open_relation_cached(row->sub_table_relid, lock_mode, cache,
                                column_attno,
                                static_cast<std::int16_t>(loc.bucket_index),
                                static_cast<std::int32_t>(loc.shard_index));
}

namespace fragment_registry_detail {

/// Required column number, name and type in the fragment registry.
struct attribute_contract {
    /// Column number, starting at one.
    AttrNumber attno = InvalidAttrNumber;
    /// Required column name.
    std::string_view name{};
    /// Required PostgreSQL type OID.
    Oid type_oid = InvalidOid;
};

inline constexpr std::array<attribute_contract, 11> k_attribute_contracts{{
    {1, "database_oid", OIDOID},
    {2, "tablespace_oid", OIDOID},
    {3, "main_relfilenode", OIDOID},
    {4, "column_attno", INT2OID},
    {5, "bucket_idx", INT2OID},
    {6, "shard_idx", INT4OID},
    {7, "sub_table_relid", OIDOID},
    {8, "sidecar_relid", OIDOID},
    {9, "bytes_written", INT8OID},
    {10, "is_full", BOOLOID},
    {11, "total_payload_bytes", INT8OID},
}};

/// Report an unexpected fragment registry schema.
/// @param detail Failed check.
/// @throws A C++ exception carrying the PostgreSQL error.
[[noreturn]] inline void contract_error(const std::string_view detail) {
    const std::string owned{detail};
    bioseq::postgres::support::invoke_pg_as_cpp([&owned] {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq fragment registry catalog contract changed"),
             errdetail("%s", owned.c_str())));
    });
    pg_unreachable();
}

/// Check the  registry table's identity, storage and columns.
/// @param catalog Open registry.
/// @param expected_catalog_oid Required table OID.
/// @param expected_namespace_oid Required schema OID.
/// @throws A PostgreSQL error on a mismatch.
inline void validate_catalog(
        Relation catalog,
        const Oid expected_catalog_oid,
        const Oid expected_namespace_oid) {
    if (catalog == nullptr
        || RelationGetRelid(catalog) != expected_catalog_oid
        || RelationGetNamespace(catalog) != expected_namespace_oid
        || catalog->rd_rel == nullptr
        || catalog->rd_rel->relkind != RELKIND_RELATION
        || catalog->rd_rel->relam != HEAP_TABLE_AM_OID
        || catalog->rd_rel->relpersistence != RELPERSISTENCE_PERMANENT) {
        contract_error("relation identity, persistence, or heap access method changed");
    }
    TupleDesc descriptor = RelationGetDescr(catalog);
    if (descriptor == nullptr
        || descriptor->natts
            != static_cast<int>(k_attribute_contracts.size())) {
        contract_error("attribute count changed");
    }
    for (const attribute_contract& expected : k_attribute_contracts) {
        const Form_pg_attribute attribute = bioseq::postgres::support::tuple_descriptor_attribute(
            descriptor, expected.attno - 1);
        if (attribute->attnum != expected.attno
            || attribute->attisdropped
            || !attribute->attnotnull
            || attribute->atttypid != expected.type_oid
            || std::string_view{NameStr(attribute->attname)}
                != expected.name) {
            contract_error("fixed attribute ordinal, name, type, or NOT NULL contract changed");
        }
    }
}

/// Open and check the fragment registry.
/// @param allow_missing_during_extension_teardown Allow absence during extension removal
/// @return Relation to close with AccessShareLock, or null for permitted absence.
/// @throws A PostgreSQL error if the required registry is missing or invalid.
[[nodiscard]] inline Relation open_catalog(
        const bool allow_missing_during_extension_teardown) {
    const Oid namespace_oid = get_namespace_oid("bioseq_internal", true);
    if (!OidIsValid(namespace_oid)) {
        if (internal_catalog_detail::absence_is_benign(
                allow_missing_during_extension_teardown)) return nullptr;
        internal_catalog_detail::missing_outside_teardown(
            "bioseq_internal.seq_sub_table");
    }
    const Oid catalog_oid = get_relname_relid(
        "seq_sub_table", namespace_oid);
    if (!OidIsValid(catalog_oid)) {
        if (internal_catalog_detail::absence_is_benign(
                allow_missing_during_extension_teardown)) return nullptr;
        internal_catalog_detail::missing_outside_teardown(
            "bioseq_internal.seq_sub_table");
    }
    Relation catalog = bioseq::postgres::support::try_open_postgres_relation(catalog_oid, AccessShareLock);
    if (catalog == nullptr) {
        if (internal_catalog_detail::absence_is_benign(
                allow_missing_during_extension_teardown)) return nullptr;
        internal_catalog_detail::missing_outside_teardown(
            "bioseq_internal.seq_sub_table");
    }
    validate_catalog(catalog, catalog_oid, namespace_oid);
    return catalog;
}

} // namespace fragment_registry_detail

/// Check whether SnapshotSelf can see a registration for this fragment table.
/// @param relid Fragment  table OID.
/// @param allow_missing_during_extension_teardown Allow absence during extension removal.
/// @return True if the registry contains the table.
/// @throws A PostgreSQL error if the required registry is missing or invalid.
bool is_registered_sequence_fragment_relation(
        Oid relid,
        const bool allow_missing_during_extension_teardown) {
    if (!OidIsValid(relid)) {
        return false;
    }
    Relation catalog = fragment_registry_detail::open_catalog(
        allow_missing_during_extension_teardown);
    if (catalog == nullptr) {
        return false;
    }
    // Schema validation identifies sub_table_relid as column 7. Scan the heap
    // so this check aso works when a secondary index is missing or damaged.
    ScanKeyData key;
    ScanKeyInit(&key, 7, BTEqualStrategyNumber, F_OIDEQ,
        ObjectIdGetDatum(relid));
    SysScanDesc scan = systable_beginscan(
        catalog, InvalidOid, /*indexOK=*/false, SnapshotSelf, 1, &key);
    const bool found = HeapTupleIsValid(systable_getnext(scan));
    systable_endscan(scan);
    table_close(catalog, AccessShareLock);
    return found;
}



}  // namespace bioseq::storage
