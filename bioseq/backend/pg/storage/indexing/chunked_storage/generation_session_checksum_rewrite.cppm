/**
 * @file generation_session_checksum_rewrite.cppm
 * @brief Owns integration-only generation checksum rewriting.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/relation.h"
#include "access/xact.h"
#include "catalog/pg_type_d.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "storage/lockdefs.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/snapshot.h"
#include "varatt.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <new>

export module bioseq.postgres.storage.indexing.chunked_storage:generation_session_checksum_rewrite;

import std;

import bioseq.common.logging;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.storage;
import bioseq.postgres.support;
import :catalog_contracts;
import :catalog_metadata;
import :generation_session_contract;
import :generation_session_lifecycle;
import :generation_session_metadata;
import :generation_session_completion;

export namespace bioseq::storage::indexing {

/// Integration-only self-consistent metadata corruption. Both manifest and
/// head source-content fields, the head's manifest checksum, and both physical
/// row CRCs are rewritten together. Only an independent source-row decode can
/// detect the corruption.
inline void
rewrite_source_checksum_pg(Oid table_oid, std::uint64_t query_key, std::uint64_t xor_mask) {
    if (!bioseq::postgres::support::invoke_pg_as_cpp([] { return superuser(); }) || xor_mask == 0 ||
        !bioseq::indexing::sql_bigint_can_represent(query_key)) {
        minimizer_generation_storage_error_as_cpp(
            "current generation checksum corruption input is invalid");
    }
    Relation relation = bioseq::postgres::support::invoke_pg_as_cpp([table_oid] {
        return bioseq::postgres::support::try_open_postgres_relation(table_oid, AccessShareLock);
    });
    if (relation == nullptr || RelationGetNamespace(relation) != bioseq_internal_ns()) {
        if (relation != nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [relation] { relation_close(relation, AccessShareLock); });
        }
        minimizer_generation_storage_error_as_cpp(
            "current generation checksum corruption relation is invalid");
    }
    const minimizer_generation_schema_inspection schema =
        bioseq::postgres::support::invoke_pg_as_cpp(
            [relation] { return inspect_minimizer_generation_schema(relation); });
    const std::string table_name{RelationGetRelationName(relation)};
    const Oid table_tablespace_oid = relation->rd_locator.spcOid;
    bioseq::postgres::support::invoke_pg_as_cpp(
        [relation] { relation_close(relation, AccessShareLock); });
    if (!schema) {
        minimizer_generation_storage_error_as_cpp(
            "current generation checksum corruption schema is invalid");
    }

    Snapshot active_snapshot =
        bioseq::postgres::support::invoke_pg_as_cpp([] { return GetActiveSnapshot(); });
    if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
        minimizer_generation_storage_error_as_cpp(
            "current generation checksum corruption requires an active MVCC snapshot");
    }
    Snapshot registered_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
        [active_snapshot] { return RegisterSnapshot(active_snapshot); });
    minimizer_generation_head head{};
    minimizer_generation_manifest manifest{};
    {
        constexpr auto k_fixture_scheme = bioseq::indexing::minimizer_scheme_identity_for_backend(
            bioseq::indexing::minimizer_sketch_backend::scalar);
        constexpr expected_minimizer_generation_identity k_fixture_expected{
            .layout = bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local,
            .alphabet_code = bioseq::indexing::k_minimizer_persisted_dna4_alphabet_code,
            .storage_unit_byte_width = 8,
            .kmer_size = 15,
            .window_size = 10,
            .gpu_abi_revision = bioseq::indexing::k_grouped_soa_gpu_abi_revision,
            .minimizer_scheme_id = k_fixture_scheme.id,
            .minimizer_scheme_revision = k_fixture_scheme.revision,
            .producer_backend_id = bioseq::indexing::minimizer_producer_backend::scalar,
            .producer_identity_revision = bioseq::indexing::k_minimizer_producer_identity_revision,
            .max_occ_cap = bioseq::indexing::k_grouped_soa_default_max_occurrence_cap,
        };
        index_storage_fragment_generation_read_session session{
            table_name,
            query_key,
            registered_snapshot,
            k_fixture_expected,
            table_tablespace_oid,
            minimizer_generation_snapshot_registration::borrow_registered};
        head = session.read_active_head_once();
        manifest = session.read_manifest_directory_once(head).manifest;
        session.close_metadata_read();
    }
    bioseq::postgres::support::invoke_pg_as_cpp(
        [registered_snapshot] { UnregisterSnapshot(registered_snapshot); });
    manifest.source_content_checksum ^= xor_mask;
    head.source_content_checksum = manifest.source_content_checksum;
    const auto manifest_bytes = bioseq::indexing::encode_minimizer_generation_manifest(manifest);
    if (!manifest_bytes) {
        minimizer_generation_storage_error_as_cpp(
            "current generation checksum corruption could not encode manifest");
    }
    head.manifest_checksum =
        bioseq::indexing::crc64_ecma182(std::span<const std::byte>{*manifest_bytes});
    const auto head_bytes = bioseq::indexing::encode_minimizer_generation_head(head);
    if (!head_bytes) {
        minimizer_generation_storage_error_as_cpp(
            "current generation checksum corruption could not encode head");
    }
    const std::uint64_t manifest_row_checksum =
        bioseq::indexing::crc64_index_chunk_row(head.generation_id,
                                                -1,
                                                k_directory_sub_key,
                                                0,
                                                static_cast<std::uint32_t>(manifest_bytes->size()),
                                                std::span<const std::byte>{*manifest_bytes});
    const std::uint64_t head_row_checksum =
        bioseq::indexing::crc64_index_chunk_row(0,
                                                -1,
                                                k_generation_head_sub_key,
                                                0,
                                                static_cast<std::uint32_t>(head_bytes->size()),
                                                std::span<const std::byte>{*head_bytes});

    const auto make_bytea = [](std::span<const std::byte> bytes) {
        bytea* value = bioseq::postgres::support::invoke_pg_as_cpp(
            [bytes] { return reinterpret_cast<bytea*>(palloc(VARHDRSZ + bytes.size())); });
        SET_VARSIZE(value, VARHDRSZ + bytes.size());
        std::memcpy(VARDATA(value), bytes.data(), bytes.size());
        return value;
    };
    const char* quoted_table = bioseq::postgres::support::invoke_pg_as_cpp(
        [&table_name] { return quote_identifier(table_name.c_str()); });
    const std::string sql =
        fmt::format("UPDATE bioseq_internal.{} SET blob=$1,chunk_bytes=$2,checksum=$3 "
                    "WHERE query_key=$4 AND generation_id=$5 AND position_segment=-1 "
                    "AND sub_key=$6 AND chunk_no=0",
                    quoted_table);
    std::array<Oid, 6> argument_types{BYTEAOID, INT4OID, INT8OID, INT8OID, INT8OID, INT8OID};
    bytea* manifest_blob = make_bytea(std::span<const std::byte>{*manifest_bytes});
    bytea* head_blob = make_bytea(std::span<const std::byte>{*head_bytes});
    std::array<Datum, 6> manifest_arguments{
        PointerGetDatum(manifest_blob),
        Int32GetDatum(static_cast<std::int32_t>(manifest_bytes->size())),
        Int64GetDatum(checksum_to_pg_bigint(manifest_row_checksum)),
        Int64GetDatum(static_cast<std::int64_t>(query_key)),
        Int64GetDatum(static_cast<std::int64_t>(head.generation_id)),
        Int64GetDatum(static_cast<std::int64_t>(k_directory_sub_key)),
    };
    std::array<Datum, 6> head_arguments{
        PointerGetDatum(head_blob),
        Int32GetDatum(static_cast<std::int32_t>(head_bytes->size())),
        Int64GetDatum(checksum_to_pg_bigint(head_row_checksum)),
        Int64GetDatum(static_cast<std::int64_t>(query_key)),
        Int64GetDatum(0),
        Int64GetDatum(static_cast<std::int64_t>(k_generation_head_sub_key)),
    };
    struct rewrite_spi_state {
        int connect_status = 0;
        int manifest_status = 0;
        int head_status = 0;
        int finish_status = 0;
        std::uint64_t manifest_rows = 0;
        std::uint64_t head_rows = 0;
    } spi{};
    const char* sql_data = sql.c_str();
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&spi, sql_data, &argument_types, &manifest_arguments, &head_arguments] noexcept {
            spi.connect_status = SPI_connect();
            if (spi.connect_status != SPI_OK_CONNECT)
                return;
            spi.manifest_status = SPI_execute_with_args(sql_data,
                                                        static_cast<int>(argument_types.size()),
                                                        argument_types.data(),
                                                        manifest_arguments.data(),
                                                        nullptr,
                                                        false,
                                                        1);
            spi.manifest_rows = SPI_processed;
            if (spi.manifest_status == SPI_OK_UPDATE && spi.manifest_rows == 1) {
                spi.head_status = SPI_execute_with_args(sql_data,
                                                        static_cast<int>(argument_types.size()),
                                                        argument_types.data(),
                                                        head_arguments.data(),
                                                        nullptr,
                                                        false,
                                                        1);
                spi.head_rows = SPI_processed;
            }
            spi.finish_status = SPI_finish();
        });
    bioseq::postgres::support::invoke_pg_as_cpp([manifest_blob, head_blob] {
        pfree(manifest_blob);
        pfree(head_blob);
    });
    if (spi.connect_status != SPI_OK_CONNECT || spi.manifest_status != SPI_OK_UPDATE ||
        spi.manifest_rows != 1 || spi.head_status != SPI_OK_UPDATE || spi.head_rows != 1 ||
        spi.finish_status != SPI_OK_FINISH) {
        minimizer_generation_storage_error_as_cpp(
            fmt::format("current generation checksum corruption SPI session disagrees: connect={} "
                        "manifest_status={} manifest_rows={} head_status={} head_rows={} "
                        "finish={}",
                        spi.connect_status,
                        spi.manifest_status,
                        spi.manifest_rows,
                        spi.head_status,
                        spi.head_rows,
                        spi.finish_status));
    }
    bioseq::postgres::support::invoke_pg_as_cpp([] { CommandCounterIncrement(); });
}

} // namespace bioseq::storage::indexing
