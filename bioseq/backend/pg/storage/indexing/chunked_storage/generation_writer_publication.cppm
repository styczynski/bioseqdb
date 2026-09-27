/**
 * @file generation_writer_publication.cppm
 * @brief Publish  a validated generation and update storage records for mapped files.
 */
module;
#include <bioseq/pg_compat.h>
#include <libassert/assert.hpp>

extern "C" {
#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h" // get_relname_relid
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_opclass_d.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_type_d.h"
#include "commands/sequence.h"
#include "commands/tablespace.h"
#include "executor/instrument.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "storage/buffile.h"
#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include "storage/relfilelocator.h"
#include "storage/shm_toc.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/xid8.h"

#include "common/relpath.h"
#include "miscadmin.h"
#include "port.h"
#include "port/atomics.h"
}

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
export module bioseq.postgres.storage.indexing.chunked_storage:generation_writer_publication;
import std;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.memory_budget;
import bioseq.postgres.support; // bioseq::postgres::support::try_open_postgres_relation, bioseq::postgres::support::invoke_pg_as_cpp
import bioseq.postgres.support.datums;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.storage.manager; // seq_caches, sequence_locator, format helpers
import bioseq.storage.layout;           // sequence_locator
import bioseq.storage.size;             // sequence_size_t
import bioseq.indexing.type; // sequence_index_layout, index_sub_key_t, k_max_index_chunk_size_bytes, sentinels
import bioseq.indexing.storage;           // index storage concepts
import bioseq.indexing.minimizer_hashmap; // encoded header/bucket validation
import bioseq.indexing.minimizer_fragment_image; // minimizer fragment file format
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.minimizer_fragment_compact_hash32_adapter;
import bioseq.postgres.table.io; // begin_column_read/read_next_column_row/end_column_read/column_read_state
import bioseq.postgres.storage.scanner;        // sequence access through PostgreSQL
import bioseq.storage.lazy_sequence;           // lazy_sequence
import bioseq.postgres.types.compact_sequence; // bioseq::pg_types::seq, dispatch_instance
import bioseq.sequences;                       // compact_sequence_range
import bioseq.sequences.simd;                  // decode sequence bases in batches
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.postgres.storage.indexing.residency;
import bioseq.postgres.storage.indexing.sorted_runs;
import bioseq.instrumentation.clock;
import bioseq.postgres.column.table_access;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.physical;
import bioseq.postgres.storage.indexing.sql_row_codecs;
import bioseq.postgres.storage.manager.sql_row_codecs;
import bioseq.postgres.sql.generated.core.storage_indexing_pg_chunked_storage.entity_handles;
import :catalog;
import :file_actions;
import :fragment_image;
import :generation_writer_rows;
export namespace bioseq::storage::indexing {
/// Lock the active head row and copy its bytes before replacing it.
/// Return no value when this query key has no published generation.
[[nodiscard]] inline std::optional<std::array<std::byte, minimizer_generation_head::encoded_bytes>>
read_locked_generation_head_for_descriptor_publication_pg(const std::string_view table_name,
                                                          const std::uint64_t query_key) {
    std::array<std::byte, minimizer_generation_head::encoded_bytes> bytes{};
    std::array<Oid, 5u> types{INT8OID, INT8OID, INT8OID, INT4OID, INT4OID};
    std::array<Datum, 5u> arguments{
        Int64GetDatum(static_cast<std::int64_t>(query_key)),
        Int64GetDatum(0),
        Int64GetDatum(static_cast<std::int64_t>(k_generation_head_sub_key)),
        Int32GetDatum(-1),
        Int32GetDatum(0),
    };
    const std::string owned_table_name{table_name};
    const char* quoted_table = bioseq::postgres::support::invoke_pg_as_cpp(
        [&owned_table_name] { return quote_identifier(owned_table_name.c_str()); });
    const std::string sql = fmt::format("SELECT blob FROM bioseq_internal.{} "
                                        "WHERE query_key=$1 AND generation_id=$2 AND sub_key=$3 "
                                        "AND position_segment=$4 AND chunk_no=$5 FOR UPDATE",
                                        quoted_table);
    struct operation_state {
        int connect = 0;
        int execute = 0;
        int finish = 0;
        std::uint64_t rows = 0u;
        bool table = false;
        bool is_null = true;
        bool exact_size = false;
    } state{};
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&state, &sql, &types, &arguments, &bytes] noexcept {
            state.connect = SPI_connect();
            if (state.connect != SPI_OK_CONNECT)
                return;
            state.execute = SPI_execute_with_args(sql.c_str(),
                                                  static_cast<int>(types.size()),
                                                  types.data(),
                                                  arguments.data(),
                                                  nullptr,
                                                  false,
                                                  1);
            state.rows = SPI_processed;
            state.table = SPI_tuptable != nullptr;
            if (state.execute == SPI_OK_SELECT && state.rows == 1u && state.table) {
                const Datum datum =
                    SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &state.is_null);
                if (!state.is_null) {
                    auto* detoasted = reinterpret_cast<bytea*>(PG_DETOAST_DATUM_PACKED(datum));
                    const std::size_t size = VARSIZE_ANY_EXHDR(detoasted);
                    if (size == bytes.size()) {
                        std::memcpy(bytes.data(), VARDATA_ANY(detoasted), size);
                        state.exact_size = true;
                    }
                    if (static_cast<void*>(detoasted) != DatumGetPointer(datum)) {
                        pfree(detoasted);
                    }
                }
            }
            state.finish = SPI_finish();
        });
    if (state.connect != SPI_OK_CONNECT || state.execute != SPI_OK_SELECT || state.rows > 1u ||
        (state.rows == 1u && (!state.table || state.is_null || !state.exact_size)) ||
        state.finish != SPI_OK_FINISH) {
        minimizer_generation_storage_error_as_cpp(
            fmt::format("descriptor publication head lock disagrees: connect={} "
                        "execute={} rows={} table={} null={} size={} finish={}",
                        state.connect,
                        state.execute,
                        state.rows,
                        state.table,
                        state.is_null,
                        state.exact_size,
                        state.finish));
    }
    if (state.rows == 0u)
        return std::nullopt;
    return bytes;
}

/// Lock and validate the LIVE storage catalog entry for this index and query key.
/// Return no value if none exists.
[[nodiscard]] inline std::optional<minimizer_generation_storage_descriptor>
read_locked_live_descriptor_for_publication_pg(
    const minimizer_generation_administration_identity& identity, const std::uint64_t query_key) {
    NameData index_name{};
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&index_name, &identity] { namestrcpy(&index_name, identity.index_name.c_str()); });
    std::array<Oid, 7u> types{OIDOID, OIDOID, OIDOID, OIDOID, INT2OID, NAMEOID, INT8OID};
    std::array<Datum, 7u> arguments{
        ObjectIdGetDatum(identity.database_oid),
        ObjectIdGetDatum(identity.tablespace_oid),
        ObjectIdGetDatum(identity.main_relfilenode),
        ObjectIdGetDatum(identity.user_relid),
        Int16GetDatum(identity.column_attno),
        NameGetDatum(&index_name),
        Int64GetDatum(static_cast<std::int64_t>(query_key)),
    };
    static constexpr std::string_view k_select_sql{
        "SELECT * FROM bioseq_internal.minimizer_generation_storage "
        "WHERE database_oid=$1 AND tablespace_oid=$2 "
        "AND main_relfilenode=$3 AND user_relid=$4 "
        "AND column_attno=$5 AND index_name=$6 AND query_key=$7 "
        "AND state=smallint '1' FOR UPDATE"};
    struct raw_row {
        Oid database_oid = InvalidOid;
        Oid tablespace_oid = InvalidOid;
        Oid main_relfilenode = InvalidOid;
        Oid user_relid = InvalidOid;
        std::int16_t column_attno = 0;
        NameData index_name{};
        std::int64_t query_key = -1;
        std::int64_t generation_id = 0;
        std::int16_t storage_kind = -1;
        Oid payload_relation_oid = InvalidOid;
        Oid payload_database_oid = InvalidOid;
        Oid payload_tablespace_oid = InvalidOid;
        Oid payload_relfilenode = InvalidOid;
        NameData payload_relation_name{};
        std::int64_t image_bytes = 0;
        std::int64_t header_checksum = 0;
        FullTransactionId retire_full_xid = InvalidFullTransactionId;
        std::int16_t state = 0;
        bool payload_relation_oid_null = true;
        bool payload_database_oid_null = true;
        bool payload_tablespace_oid_null = true;
        bool payload_relfilenode_null = true;
        bool payload_relation_name_null = true;
        bool retire_full_xid_null = true;
        bool required_null = false;
    } row{};
    struct operation_state {
        int connect = 0;
        int execute = 0;
        int finish = 0;
        std::uint64_t rows = 0u;
        bool table = false;
    } state{};
    bioseq::postgres::support::invoke_pg_as_cpp([&state, &row, &types, &arguments] noexcept {
        state.connect = SPI_connect();
        if (state.connect != SPI_OK_CONNECT)
            return;
        state.execute = SPI_execute_with_args(k_select_sql.data(),
                                              static_cast<int>(types.size()),
                                              types.data(),
                                              arguments.data(),
                                              nullptr,
                                              false,
                                              1);
        state.rows = SPI_processed;
        state.table = SPI_tuptable != nullptr;
        if (state.execute == SPI_OK_SELECT && state.rows == 1u && state.table) {
            HeapTuple tuple = SPI_tuptable->vals[0];
            TupleDesc descriptor = SPI_tuptable->tupdesc;
            bool is_null = false;
            const auto required = [tuple, descriptor, &is_null, &row](const int attribute) {
                const Datum value = SPI_getbinval(tuple, descriptor, attribute, &is_null);
                row.required_null = row.required_null || is_null;
                return value;
            };
            row.database_oid = DatumGetObjectId(required(1));
            row.tablespace_oid = DatumGetObjectId(required(2));
            row.main_relfilenode = DatumGetObjectId(required(3));
            row.user_relid = DatumGetObjectId(required(4));
            row.column_attno = DatumGetInt16(required(5));
            row.index_name = *DatumGetName(required(6));
            row.query_key = DatumGetInt64(required(7));
            row.generation_id = DatumGetInt64(required(8));
            row.storage_kind = DatumGetInt16(required(9));
            Datum value = SPI_getbinval(tuple, descriptor, 10, &row.payload_relation_oid_null);
            if (!row.payload_relation_oid_null) {
                row.payload_relation_oid = DatumGetObjectId(value);
            }
            value = SPI_getbinval(tuple, descriptor, 11, &row.payload_database_oid_null);
            if (!row.payload_database_oid_null) {
                row.payload_database_oid = DatumGetObjectId(value);
            }
            value = SPI_getbinval(tuple, descriptor, 12, &row.payload_tablespace_oid_null);
            if (!row.payload_tablespace_oid_null) {
                row.payload_tablespace_oid = DatumGetObjectId(value);
            }
            value = SPI_getbinval(tuple, descriptor, 13, &row.payload_relfilenode_null);
            if (!row.payload_relfilenode_null) {
                row.payload_relfilenode = DatumGetObjectId(value);
            }
            value = SPI_getbinval(tuple, descriptor, 14, &row.payload_relation_name_null);
            if (!row.payload_relation_name_null) {
                row.payload_relation_name = *DatumGetName(value);
            }
            row.image_bytes = DatumGetInt64(required(15));
            row.header_checksum = DatumGetInt64(required(16));
            value = SPI_getbinval(tuple, descriptor, 17, &row.retire_full_xid_null);
            if (!row.retire_full_xid_null) {
                row.retire_full_xid = DatumGetFullTransactionId(value);
            }
            row.state = DatumGetInt16(required(18));
        }
        state.finish = SPI_finish();
    });
    if (state.connect != SPI_OK_CONNECT || state.execute != SPI_OK_SELECT || state.rows > 1u ||
        (state.rows == 1u && (!state.table || row.required_null)) ||
        state.finish != SPI_OK_FINISH) {
        minimizer_generation_storage_error_as_cpp(
            fmt::format("descriptor publication LIVE lookup disagrees: connect={} "
                        "execute={} rows={} table={} required_null={} finish={}",
                        state.connect,
                        state.execute,
                        state.rows,
                        state.table,
                        row.required_null,
                        state.finish));
    }
    if (state.rows == 0u)
        return std::nullopt;
    if (row.query_key < 0 || row.generation_id <= 0 ||
        row.image_bytes <
            static_cast<std::int64_t>(bioseq::indexing::k_minimizer_fragment_image_header_bytes) ||
        row.storage_kind !=
            static_cast<std::int16_t>(
                bioseq::indexing::k_minimizer_fragment_image_relation_backed_storage_kind)) {
        minimizer_generation_storage_error_as_cpp(
            "descriptor publication LIVE row scalar shape is invalid");
    }
    minimizer_generation_storage_descriptor result{
        .database_oid = row.database_oid,
        .tablespace_oid = row.tablespace_oid,
        .main_relfilenode = row.main_relfilenode,
        .user_relid = row.user_relid,
        .column_attno = row.column_attno,
        .index_name = NameStr(row.index_name),
        .query_key = static_cast<std::uint64_t>(row.query_key),
        .generation_id = static_cast<std::uint64_t>(row.generation_id),
        .payload_relation_oid = row.payload_relation_oid_null
                                    ? std::nullopt
                                    : std::optional<Oid>{row.payload_relation_oid},
        .payload_database_oid = row.payload_database_oid_null
                                    ? std::nullopt
                                    : std::optional<Oid>{row.payload_database_oid},
        .payload_tablespace_oid = row.payload_tablespace_oid_null
                                      ? std::nullopt
                                      : std::optional<Oid>{row.payload_tablespace_oid},
        .payload_relfilenode = row.payload_relfilenode_null
                                   ? std::nullopt
                                   : std::optional<Oid>{row.payload_relfilenode},
        .payload_relation_name =
            row.payload_relation_name_null
                ? std::nullopt
                : std::optional<std::string>{NameStr(row.payload_relation_name)},
        .image_bytes = static_cast<std::uint64_t>(row.image_bytes),
        .header_checksum = checksum_from_pg_bigint(row.header_checksum),
        .retire_full_xid =
            row.retire_full_xid_null
                ? std::nullopt
                : std::optional<std::uint64_t>{U64FromFullTransactionId(row.retire_full_xid)},
        .state = static_cast<minimizer_generation_storage_state>(row.state),
    };
    if (!exact_storage_descriptor_identity(result, identity, query_key) ||
        !has_exact_storage_descriptor_relation_shape(result) ||
        result.state != minimizer_generation_storage_state::live || result.retire_full_xid) {
        minimizer_generation_storage_error_as_cpp(
            "descriptor publication LIVE row shape disagrees");
    }
    return result;
}

/// Retire the old storage  entry and insert the new LIVE entry when supplied.
/// Record the retiring transaction so cleanup can wait for older snapshots.
/// Both changes belong to the caller's transaction.
inline void apply_minimizer_generation_storage_transition_pg(
    const minimizer_generation_administration_identity& identity,
    const std::uint64_t query_key,
    const std::optional<minimizer_generation_storage_descriptor>& old_live,
    const std::optional<minimizer_generation_storage_descriptor>& new_live) {
    if (!old_live && !new_live)
        return;
    if ((old_live && (!exact_storage_descriptor_identity(*old_live, identity, query_key) ||
                      old_live->state != minimizer_generation_storage_state::live ||
                      old_live->retire_full_xid)) ||
        (new_live &&
         (!exact_storage_descriptor_identity(*new_live, identity, query_key) ||
          new_live->state != minimizer_generation_storage_state::live ||
          new_live->retire_full_xid || !has_exact_storage_descriptor_relation_shape(*new_live) ||
          !bioseq::indexing::sql_bigint_can_represent(new_live->generation_id) ||
          !bioseq::indexing::sql_bigint_can_represent(new_live->image_bytes)))) {
        minimizer_generation_storage_error_as_cpp(
            "generation storage transition binding is invalid");
    }

    NameData index_name{};
    NameData payload_relation_name{};
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&index_name, &payload_relation_name, &identity, &new_live] {
            namestrcpy(&index_name, identity.index_name.c_str());
            if (new_live && new_live->payload_relation_name) {
                namestrcpy(&payload_relation_name, new_live->payload_relation_name->c_str());
            }
        });
    std::array<Oid, 9u> update_types{
        OIDOID, OIDOID, OIDOID, OIDOID, INT2OID, NAMEOID, INT8OID, INT8OID, XID8OID};
    std::array<Datum, 9u> update_arguments{
        ObjectIdGetDatum(identity.database_oid),
        ObjectIdGetDatum(identity.tablespace_oid),
        ObjectIdGetDatum(identity.main_relfilenode),
        ObjectIdGetDatum(identity.user_relid),
        Int16GetDatum(identity.column_attno),
        NameGetDatum(&index_name),
        Int64GetDatum(static_cast<std::int64_t>(query_key)),
        Int64GetDatum(old_live ? static_cast<std::int64_t>(old_live->generation_id) : 0),
        old_live ? FullTransactionIdGetDatum(bioseq::postgres::support::invoke_pg_as_cpp(
                       [] { return GetTopFullTransactionId(); }))
                 : FullTransactionIdGetDatum(InvalidFullTransactionId),
    };
    static constexpr std::string_view k_update_sql{
        "UPDATE bioseq_internal.minimizer_generation_storage "
        "SET retire_full_xid=$9, state=smallint '2' "
        "WHERE database_oid=$1 AND tablespace_oid=$2 "
        "AND main_relfilenode=$3 AND user_relid=$4 "
        "AND column_attno=$5 AND index_name=$6 AND query_key=$7 "
        "AND generation_id=$8 AND state=smallint '1' "
        "AND retire_full_xid IS NULL"};

    std::array<Oid, k_storage_descriptor_attribute_count> insert_types{OIDOID,
                                                                       OIDOID,
                                                                       OIDOID,
                                                                       OIDOID,
                                                                       INT2OID,
                                                                       NAMEOID,
                                                                       INT8OID,
                                                                       INT8OID,
                                                                       INT2OID,
                                                                       OIDOID,
                                                                       OIDOID,
                                                                       OIDOID,
                                                                       OIDOID,
                                                                       NAMEOID,
                                                                       INT8OID,
                                                                       INT8OID,
                                                                       XID8OID,
                                                                       INT2OID};
    std::array<Datum, k_storage_descriptor_attribute_count> insert_arguments{};
    std::array<char, k_storage_descriptor_attribute_count> insert_nulls{};
    if (new_live) {
        insert_arguments = {
            ObjectIdGetDatum(new_live->database_oid),
            ObjectIdGetDatum(new_live->tablespace_oid),
            ObjectIdGetDatum(new_live->main_relfilenode),
            ObjectIdGetDatum(new_live->user_relid),
            Int16GetDatum(new_live->column_attno),
            NameGetDatum(&index_name),
            Int64GetDatum(static_cast<std::int64_t>(new_live->query_key)),
            Int64GetDatum(static_cast<std::int64_t>(new_live->generation_id)),
            Int16GetDatum(static_cast<std::int16_t>(
                bioseq::indexing::k_minimizer_fragment_image_relation_backed_storage_kind)),
            new_live->payload_relation_oid ? ObjectIdGetDatum(*new_live->payload_relation_oid) : 0,
            new_live->payload_database_oid ? ObjectIdGetDatum(*new_live->payload_database_oid) : 0,
            new_live->payload_tablespace_oid ? ObjectIdGetDatum(*new_live->payload_tablespace_oid)
                                             : 0,
            new_live->payload_relfilenode ? ObjectIdGetDatum(*new_live->payload_relfilenode) : 0,
            new_live->payload_relation_name ? NameGetDatum(&payload_relation_name) : 0,
            Int64GetDatum(static_cast<std::int64_t>(new_live->image_bytes)),
            Int64GetDatum(checksum_to_pg_bigint(new_live->header_checksum)),
            0,
            Int16GetDatum(static_cast<std::int16_t>(minimizer_generation_storage_state::live)),
        };
        insert_nulls.fill(' ');
        for (const std::size_t attribute : {9u, 10u, 11u, 12u, 13u}) {
            const bool present = attribute == 9u    ? new_live->payload_relation_oid.has_value()
                                 : attribute == 10u ? new_live->payload_database_oid.has_value()
                                 : attribute == 11u ? new_live->payload_tablespace_oid.has_value()
                                 : attribute == 12u ? new_live->payload_relfilenode.has_value()
                                                    : new_live->payload_relation_name.has_value();
            if (!present)
                insert_nulls[attribute] = 'n';
        }
        insert_nulls[16] = 'n';
    }
    static constexpr std::string_view k_insert_sql{
        "INSERT INTO bioseq_internal.minimizer_generation_storage "
        "(database_oid,tablespace_oid,main_relfilenode,user_relid,"
        "column_attno,index_name,query_key,generation_id,provider,"
        "payload_relation_oid,payload_database_oid,payload_tablespace_oid,"
        "payload_relfilenode,payload_relation_name,image_bytes,"
        "header_checksum,retire_full_xid,state) VALUES "
        "($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,"
        "$17,$18)"};
    struct operation_state {
        int connect = 0;
        int update = 0;
        int insert = 0;
        int finish = 0;
        std::uint64_t updated = 0u;
        std::uint64_t inserted = 0u;
    } state{};
    bioseq::postgres::support::invoke_pg_as_cpp([&state,
                                                 &update_types,
                                                 &update_arguments,
                                                 &insert_types,
                                                 &insert_arguments,
                                                 &insert_nulls,
                                                 &old_live,
                                                 &new_live] noexcept {
        state.connect = SPI_connect();
        if (state.connect != SPI_OK_CONNECT)
            return;
        if (old_live) {
            state.update = SPI_execute_with_args(k_update_sql.data(),
                                                 static_cast<int>(update_types.size()),
                                                 update_types.data(),
                                                 update_arguments.data(),
                                                 nullptr,
                                                 false,
                                                 0);
            state.updated = SPI_processed;
            if (state.update != SPI_OK_UPDATE || state.updated != 1u) {
                state.finish = SPI_finish();
                return;
            }
        }
        if (new_live) {
            state.insert = SPI_execute_with_args(k_insert_sql.data(),
                                                 static_cast<int>(insert_types.size()),
                                                 insert_types.data(),
                                                 insert_arguments.data(),
                                                 insert_nulls.data(),
                                                 false,
                                                 0);
            state.inserted = SPI_processed;
        }
        state.finish = SPI_finish();
    });
    if (state.connect != SPI_OK_CONNECT ||
        (old_live && (state.update != SPI_OK_UPDATE || state.updated != 1u)) ||
        (new_live && (state.insert != SPI_OK_INSERT || state.inserted != 1u)) ||
        state.finish != SPI_OK_FINISH) {
        minimizer_generation_storage_error_as_cpp(
            fmt::format("generation storage transition disagrees: connect={} "
                        "update={} updated={} insert={} inserted={} finish={}",
                        state.connect,
                        state.update,
                        state.updated,
                        state.insert,
                        state.inserted,
                        state.finish));
    }
}

/// Publish a fully reread generation in the transaction that built it
/// Check the ready token and relation identity, then replace the actie head.
/// For mapped fragment files, retire the previous storage entry and add the new one.
/// Consume  the token and return the saved head with any replaced generation ID.
[[nodiscard]] ready_minimizer_generation_published_head_receipt
publish_ready_minimizer_generation_head_pg(ready_minimizer_generation_token&& token) {
    const auto minimizer_generation_storage_error = [](std::string_view message) {
        minimizer_generation_storage_error_as_cpp(message);
    };
    struct publication_state {
        Relation relation = nullptr;
        Relation descriptor_relation = nullptr;
        bytea* blob = nullptr;
        std::string sql;
    };
    auto* publication = new publication_state{};
    std::array<std::byte, minimizer_generation_head::encoded_bytes> returned_head_bytes{};
    std::array<std::byte, minimizer_generation_head::encoded_bytes> previous_head_bytes{};
    bool returned_valid = false;
    bool previous_present = false;
    std::optional<minimizer_generation_head> decoded_previous_head{};

    try {
        if (!token.valid_ || !bioseq::indexing::sql_bigint_can_represent(token.query_key_) ||
            token.generation_id_ == 0 || token.identity_.database_oid != MyDatabaseId ||
            !TransactionIdIsValid(token.transaction_id_) ||
            token.transaction_id_ != bioseq::postgres::support::invoke_pg_as_cpp(
                                         [] { return GetCurrentTransactionIdIfAny(); }) ||
            token.advisory_lock_key_ !=
                index_advisory_xact_lock_key(token.identity_.database_oid,
                                             token.identity_.tablespace_oid,
                                             token.identity_.main_relfilenode,
                                             token.identity_.column_attno,
                                             token.identity_.index_name)) {
            minimizer_generation_storage_error("READY publication token is invalid");
        }
        bioseq::postgres::support::invoke_pg_as_cpp([&token] {
            acquire_index_advisory_xact_lock(token.identity_.database_oid,
                                             token.identity_.tablespace_oid,
                                             token.identity_.main_relfilenode,
                                             token.identity_.column_attno,
                                             token.identity_.index_name);
        });
        publication->relation = bioseq::postgres::support::invoke_pg_as_cpp([publication, &token] {
            return bioseq::postgres::support::try_open_postgres_relation(
                token.sidecar_relation_oid_, RowExclusiveLock);
        });
        if (publication->relation == nullptr ||
            RelationGetNamespace(publication->relation) != bioseq_internal_ns() ||
            std::string_view{RelationGetRelationName(publication->relation)} !=
                token.identity_.table_name ||
            publication->relation->rd_locator.spcOid != token.sidecar_locator_.spcOid ||
            publication->relation->rd_locator.dbOid != token.sidecar_locator_.dbOid ||
            publication->relation->rd_locator.relNumber != token.sidecar_locator_.relNumber) {
            minimizer_generation_storage_error(
                "READY publication relation identity changed after reread");
        }
        const minimizer_generation_schema_inspection schema =
            bioseq::postgres::support::invoke_pg_as_cpp([publication, &token] {
                return inspect_minimizer_generation_schema(
                    publication->relation,
                    nullptr,
                    minimizer_generation_schema_expectation{
                        .namespace_oid = bioseq_internal_ns(),
                        .relation_name = token.identity_.table_name,
                        .tablespace_oid = token.identity_.tablespace_oid,
                    });
            });
        if (!schema) {
            minimizer_generation_storage_error("READY publication schema changed after reread");
        }
        auto decoded_token_head =
            bioseq::indexing::decode_minimizer_generation_head(token.head_bytes_);
        if (!decoded_token_head || decoded_token_head->generation_id != token.generation_id_ ||
            decoded_token_head->source_identity != token.source_identity_ ||
            decoded_token_head->source_content_checksum != token.source_content_checksum_ ||
            decoded_token_head->storage_contract_checksum != token.storage_contract_checksum_ ||
            token.head_row_checksum_ != bioseq::indexing::crc64_index_chunk_row(
                                            0,
                                            -1,
                                            k_generation_head_sub_key,
                                            0,
                                            static_cast<std::uint32_t>(token.head_bytes_.size()),
                                            token.head_bytes_)) {
            minimizer_generation_storage_error("READY publication token head disagrees");
        }

        const auto locked_previous_head = read_locked_generation_head_for_descriptor_publication_pg(
            token.identity_.table_name, token.query_key_);
        previous_present = locked_previous_head.has_value();
        if (locked_previous_head) {
            previous_head_bytes = *locked_previous_head;
            auto decoded_previous_head_result =
                bioseq::indexing::decode_minimizer_generation_head(previous_head_bytes);
            if (!decoded_previous_head_result ||
                decoded_previous_head_result->generation_id == 0u ||
                decoded_previous_head_result->generation_id == token.generation_id_) {
                minimizer_generation_storage_error(
                    "READY publication previous head is malformed or names the new generation");
            }
            decoded_previous_head = *decoded_previous_head_result;
        }

        const bool fragment_index =
            token.identity_.index_name == k_minimizers_per_fragment_index_name;
        const bool new_grouped_soa =
            decoded_token_head->layout ==
            bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local;
        if (!fragment_index && token.file_receipt_) {
            minimizer_generation_storage_error(
                "non-fragment generation carried a minimizer fragment image file receipt");
        }
        if (fragment_index) {
            const Oid descriptor_oid = bioseq::postgres::support::invoke_pg_as_cpp([] {
                return get_relname_relid("minimizer_generation_storage", bioseq_internal_ns());
            });
            if (!OidIsValid(descriptor_oid)) {
                minimizer_generation_storage_error(
                    "generation storage descriptor catalog is missing");
            }
            publication->descriptor_relation =
                bioseq::postgres::support::invoke_pg_as_cpp([publication, descriptor_oid] {
                    return bioseq::postgres::support::try_open_postgres_relation(descriptor_oid,
                                                                                 RowExclusiveLock);
                });
            if (publication->descriptor_relation == nullptr ||
                RelationGetNamespace(publication->descriptor_relation) != bioseq_internal_ns() ||
                std::string_view{RelationGetRelationName(publication->descriptor_relation)} !=
                    "minimizer_generation_storage") {
                minimizer_generation_storage_error(
                    "generation storage descriptor catalog identity changed");
            }
            bioseq::storage::validate_minimizer_generation_storage_catalogs_pg();

            auto old_live =
                read_locked_live_descriptor_for_publication_pg(token.identity_, token.query_key_);
            const bool old_grouped_soa =
                decoded_previous_head &&
                decoded_previous_head->layout ==
                    bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local;
            if (old_grouped_soa != old_live.has_value() ||
                (old_live && old_live->generation_id != decoded_previous_head->generation_id)) {
                minimizer_generation_storage_error(
                    "active head and LIVE generation descriptor disagree");
            }
            if (old_live &&
                (old_live->payload_relation_oid !=
                     std::optional<Oid>{token.sidecar_relation_oid_} ||
                 old_live->payload_database_oid !=
                     std::optional<Oid>{token.sidecar_locator_.dbOid} ||
                 old_live->payload_tablespace_oid !=
                     std::optional<Oid>{token.sidecar_locator_.spcOid} ||
                 old_live->payload_relfilenode !=
                     std::optional<Oid>{static_cast<Oid>(token.sidecar_locator_.relNumber)} ||
                 old_live->payload_relation_name !=
                     std::optional<std::string>{token.identity_.table_name})) {
                minimizer_generation_storage_error(
                    "previous relation-backed descriptor locator changed");
            }
            if (new_grouped_soa != token.file_receipt_.has_value()) {
                minimizer_generation_storage_error(
                    "new minimizer-fragment grouped-SoA head and file receipt disagree");
            }
            std::optional<minimizer_generation_storage_descriptor> new_live;
            if (token.file_receipt_) {
                const minimizer_fragment_image_file_receipt& file = *token.file_receipt_;
                if (file.file_bytes < bioseq::indexing::k_minimizer_fragment_image_header_bytes ||
                    !bioseq::indexing::sql_bigint_can_represent(file.file_bytes)) {
                    minimizer_generation_storage_error(
                        "new minimizer fragment image file receipt cannot be cataloged");
                }
                new_live = minimizer_generation_storage_descriptor{
                    .database_oid = token.identity_.database_oid,
                    .tablespace_oid = token.identity_.tablespace_oid,
                    .main_relfilenode = token.identity_.main_relfilenode,
                    .user_relid = token.identity_.user_relid,
                    .column_attno = token.identity_.column_attno,
                    .index_name = token.identity_.index_name,
                    .query_key = token.query_key_,
                    .generation_id = token.generation_id_,
                    .payload_relation_oid = std::optional<Oid>{token.sidecar_relation_oid_},
                    .payload_database_oid = std::optional<Oid>{token.sidecar_locator_.dbOid},
                    .payload_tablespace_oid = std::optional<Oid>{token.sidecar_locator_.spcOid},
                    .payload_relfilenode =
                        std::optional<Oid>{static_cast<Oid>(token.sidecar_locator_.relNumber)},
                    .payload_relation_name = std::optional<std::string>{token.identity_.table_name},
                    .image_bytes = file.file_bytes,
                    .header_checksum = file.header_crc64,
                    .retire_full_xid = std::nullopt,
                    .state = minimizer_generation_storage_state::live,
                };
            }
            apply_minimizer_generation_storage_transition_pg(
                token.identity_, token.query_key_, old_live, new_live);
        }

        publication->blob = bioseq::postgres::support::invoke_pg_as_cpp([publication, &token] {
            return reinterpret_cast<bytea*>(palloc(VARHDRSZ + token.head_bytes_.size()));
        });
        SET_VARSIZE(publication->blob, VARHDRSZ + token.head_bytes_.size());
        std::memcpy(VARDATA(publication->blob), token.head_bytes_.data(), token.head_bytes_.size());
        std::array<Oid, 8> argument_types{
            INT8OID,
            INT8OID,
            INT4OID,
            INT8OID,
            INT4OID,
            INT4OID,
            INT8OID,
            BYTEAOID,
        };
        std::array<Datum, 8> arguments{
            Int64GetDatum(static_cast<std::int64_t>(token.query_key_)),
            Int64GetDatum(0),
            Int32GetDatum(-1),
            Int64GetDatum(static_cast<std::int64_t>(k_generation_head_sub_key)),
            Int32GetDatum(0),
            Int32GetDatum(static_cast<std::int32_t>(token.head_bytes_.size())),
            Int64GetDatum(checksum_to_pg_bigint(token.head_row_checksum_)),
            PointerGetDatum(publication->blob),
        };
        const char* quoted_table = bioseq::postgres::support::invoke_pg_as_cpp(
            [&token] { return quote_identifier(token.identity_.table_name.c_str()); });
        publication->sql =
            fmt::format("INSERT INTO bioseq_internal.{} "
                        "(query_key,generation_id,position_segment,sub_key,chunk_no,"
                        "chunk_bytes,checksum,blob) VALUES "
                        "($1,$2,$3,$4,$5,$6,$7,$8) "
                        "ON CONFLICT "
                        "(generation_id,query_key,sub_key,position_segment,chunk_no) "
                        "DO UPDATE SET blob=EXCLUDED.blob, "
                        "chunk_bytes=EXCLUDED.chunk_bytes, checksum=EXCLUDED.checksum "
                        "RETURNING blob",
                        quoted_table);
        struct publish_spi_state {
            int connect_status = 0;
            int execute_status = 0;
            int finish_status = 0;
            std::uint64_t processed = 0;
            bool table_present = false;
            bool returned_is_null = true;
            bool returned_size_valid = false;
        } spi{};
        const char* publication_sql = publication->sql.c_str();
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&spi, publication_sql, &argument_types, &arguments, &returned_head_bytes] noexcept {
                spi.connect_status = SPI_connect();
                if (spi.connect_status != SPI_OK_CONNECT)
                    return;
                spi.execute_status = SPI_execute_with_args(publication_sql,
                                                           static_cast<int>(argument_types.size()),
                                                           argument_types.data(),
                                                           arguments.data(),
                                                           nullptr,
                                                           false,
                                                           1);
                inject_minimizer_generation_failure_leaf(
                    minimizer_generation_failure_injection::generation_spi_publish,
                    "generation_spi_publish");
                spi.processed = SPI_processed;
                spi.table_present = SPI_tuptable != nullptr;
                if (spi.execute_status == SPI_OK_INSERT_RETURNING && spi.processed == 1 &&
                    spi.table_present) {
                    const Datum returned_blob = SPI_getbinval(
                        SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &spi.returned_is_null);
                    if (!spi.returned_is_null) {
                        auto* detoasted =
                            reinterpret_cast<bytea*>(PG_DETOAST_DATUM_PACKED(returned_blob));
                        const std::size_t bytes = VARSIZE_ANY_EXHDR(detoasted);
                        if (bytes == returned_head_bytes.size()) {
                            std::memcpy(returned_head_bytes.data(), VARDATA_ANY(detoasted), bytes);
                            spi.returned_size_valid = true;
                        }
                        if (static_cast<void*>(detoasted) != DatumGetPointer(returned_blob)) {
                            pfree(detoasted);
                        }
                    }
                }
                spi.finish_status = SPI_finish();
            });
        returned_valid = spi.returned_size_valid;
        if (spi.connect_status != SPI_OK_CONNECT || spi.execute_status != SPI_OK_INSERT_RETURNING ||
            spi.processed != 1 || !spi.table_present || spi.returned_is_null ||
            !spi.returned_size_valid || spi.finish_status != SPI_OK_FINISH) {
            minimizer_generation_storage_error(
                fmt::format("READY publication SPI session disagrees: connect={} "
                            "execute={} rows={} table={} null={} size={} finish={}",
                            spi.connect_status,
                            spi.execute_status,
                            spi.processed,
                            spi.table_present,
                            spi.returned_is_null,
                            spi.returned_size_valid,
                            spi.finish_status));
        }
        bytea* finished_blob = std::exchange(publication->blob, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp([finished_blob] { pfree(finished_blob); });
        if (publication->descriptor_relation != nullptr) {
            Relation finished_descriptor = std::exchange(publication->descriptor_relation, nullptr);
            bioseq::postgres::support::invoke_pg_as_cpp(
                [finished_descriptor] { relation_close(finished_descriptor, RowExclusiveLock); });
        }
        Relation finished_relation = std::exchange(publication->relation, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_relation] { relation_close(finished_relation, RowExclusiveLock); });
        auto returned_head =
            bioseq::indexing::decode_minimizer_generation_head(returned_head_bytes);
        if (!returned_valid || !returned_head ||
            !bioseq::indexing::is_fixed_wire_returned_exactly(token.head_bytes_,
                                                              returned_head_bytes) ||
            *returned_head != *decoded_token_head) {
            minimizer_generation_storage_error(
                "READY publication did not return the byte-exact validated head");
        }
        std::optional<std::uint64_t> replaced_generation_id;
        if (previous_present) {
            const auto previous_head =
                bioseq::indexing::decode_minimizer_generation_head(previous_head_bytes);
            if (!previous_head || previous_head->generation_id == 0 ||
                previous_head->generation_id == returned_head->generation_id) {
                minimizer_generation_storage_error(
                    "READY publication previous head is malformed or names the new generation");
            }
            replaced_generation_id = previous_head->generation_id;
        }
        ready_minimizer_generation_published_head_receipt receipt{
            .table_name = token.identity_.table_name,
            .query_key = token.query_key_,
            .head = *returned_head,
            .head_bytes = returned_head_bytes,
            .replaced_generation_id = replaced_generation_id,
        };
        token.invalidate();
        std::string{}.swap(publication->sql);
        delete publication;
        return receipt;
    } catch (const bioseq::postgres::support::postgres_error&) {
        // Let transaction abort reclaim PostgreSQL resources while the original
        // error propagates through C++ cleanup.
        publication->blob = nullptr;
        publication->descriptor_relation = nullptr;
        publication->relation = nullptr;
        token.invalidate();
        delete publication;
        throw;
    } catch (...) {
        std::exception_ptr failure = std::current_exception();
        publication->blob = nullptr;
        publication->descriptor_relation = nullptr;
        publication->relation = nullptr;
        token.invalidate();
        delete publication;
        std::rethrow_exception(failure);
    }
    delete publication;
    pg_unreachable();
}

} // namespace bioseq::storage::indexing
