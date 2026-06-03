/**
 * @file index_storage_handles_pg.cppm
 * @brief Lightweight Postgres index-storage handle factories and active-read
 *        policy resolver. Kept separate from index_admin_pg.cppm so scheduler
 *        pipeline code does not import the broad admin/update_index registry.
 */
module;
#include "common.h"
#include "columnar/columnar.h"
#include "access/relation.h"
#include "utils/memutils.h"
#include <cstdint>
#include <string>
#include <string_view>

export module bioseq_storage_indexing_handles;

import mlogging;
import pg_utils;
import bioseq_storage;
import bioseq_indexing.type;
import bioseq_storage_indexing_pg;

export namespace bioseq::storage::indexing {

using bioseq::indexing::SequenceIndexType;
using bioseq::indexing::index_sub_key_t;
using bioseq::indexing::DIRECTORY_SUB_KEY;

struct RelationIdentity { Oid db_oid, tablespace_oid, relfilenode; };

[[nodiscard]] inline RelationIdentity index_relation_identity(Oid user_relid) {
    RelationIdentity id{ MyDatabaseId, InvalidOid, InvalidOid };
    Relation r = try_safe_relation_open(user_relid, AccessShareLock);
    if (r != nullptr) {
        id.tablespace_oid = OidIsValid(r->rd_rel->reltablespace) ? r->rd_rel->reltablespace : MyDatabaseTableSpace;
        id.relfilenode = (Oid)r->rd_locator.relNumber;
        if (!OidIsValid(id.relfilenode)) id.relfilenode = RelationGetRelid(r);
        relation_close(r, AccessShareLock);
    }
    return id;
}

[[nodiscard]] inline MemoryContext make_handle_ctx(MemoryContext parent, const char* /*tag*/) {
    return AllocSetContextCreate(parent ? parent : CurrentMemoryContext, "bioseq_index_handle", ALLOCSET_SMALL_SIZES);
}

[[nodiscard]] inline IndexStorageFragmentPg open_fragment_storage(
        Oid user_relid, Oid db, Oid spc, Oid relfn, std::int16_t attno, std::string_view index_name,
        const seq_locator& loc, std::uint64_t sequence_rowno, seq_caches* caches, MemoryContext mctx) {
    IndexStorageFragmentPg s;
    s.db_oid_=db; s.tablespace_oid_=spc; s.main_relfilenode_=relfn; s.user_relid_=user_relid;
    s.read_caches_=caches; s.column_attno_=attno; s.index_name_=index_name;
    s.bucket_idx_=(std::uint8_t)loc.bucket_idx; s.shard_idx_=loc.shard_idx; s.query_key_=sequence_rowno;
    s.own_mctx_ = make_handle_ctx(mctx, "idx_fragment_handle");
    s.run_buffer_override_bytes_ = bioseq_index_run_buffer_bytes > 0 ? (std::size_t)bioseq_index_run_buffer_bytes : 0;
    return s;
}

[[nodiscard]] inline IndexStorageSequencePg open_sequence_storage(
        Oid user_relid, Oid db, Oid spc, Oid relfn, std::int16_t attno, std::string_view index_name,
        const seq_locator& loc, std::uint64_t sequence_rowno, seq_caches* caches, MemoryContext mctx) {
    IndexStorageSequencePg s;
    s.db_oid_=db; s.tablespace_oid_=spc; s.main_relfilenode_=relfn; s.user_relid_=user_relid;
    s.read_caches_=caches; s.column_attno_=attno; s.index_name_=index_name;
    s.bucket_idx_=(std::uint8_t)loc.bucket_idx; s.shard_idx_=loc.shard_idx; s.query_key_=sequence_rowno;
    s.own_mctx_ = make_handle_ctx(mctx, "idx_sequence_handle");
    return s;
}

[[nodiscard]] inline IndexStorageTablePg open_table_storage(
        Oid user_relid, Oid db, Oid spc, Oid relfn, std::int16_t attno, std::string_view index_name,
        seq_caches* caches, MemoryContext mctx) {
    IndexStorageTablePg s;
    s.db_oid_=db; s.tablespace_oid_=spc; s.main_relfilenode_=relfn; s.user_relid_=user_relid;
    s.read_caches_=caches; s.column_attno_=attno; s.index_name_=index_name;
    s.own_mctx_ = make_handle_ctx(mctx, "idx_table_handle");
    return s;
}

[[nodiscard]] inline std::string resolve_active_read_index(Oid user_relid, Oid relfn, std::int16_t attno) {
    std::string out{ bioseq::indexing::DEFAULT_ACTIVE_READ_INDEX };
    if (SPI_connect() != SPI_OK_CONNECT) return out;
    std::string sql = fmt::format(
        "SELECT index_name FROM bioseq_internal.index_policy "
        "WHERE user_relid={}::oid AND main_relfilenode={}::oid AND column_attno={} AND is_active_read AND enabled LIMIT 1",
        (unsigned)user_relid, (unsigned)relfn, (int)attno);
    if (SPI_execute(sql.c_str(), true, 0) == SPI_OK_SELECT && SPI_processed > 0) {
        char* nm = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
        if (nm) out = std::string(nm);
    }
    SPI_finish();
    return out;
}

}  // namespace bioseq::storage::indexing
