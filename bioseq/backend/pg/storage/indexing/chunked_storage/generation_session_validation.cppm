/**
 * @file generation_session_validation.cppm
 * @brief Defines grouped-generation opening, restart inspection, and compile-time contracts.
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
#include "catalog/namespace.h" // RelnameGetRelid
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
export module bioseq.postgres.storage.indexing.chunked_storage:generation_session_validation;
import std;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.memory_budget;
import bioseq.postgres.support; // bioseq::postgres::support::try_open_postgres_relation, bioseq::postgres::support::find_primary_key_oid
import bioseq.postgres.support.datums;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.storage.manager; // seq_caches, sequence_locator, format helpers
import bioseq.storage.layout;           // sequence_locator
import bioseq.storage.size;             // sequence_size_t
import bioseq.indexing.type; // sequence_index_layout, index_sub_key_t, k_max_index_chunk_size_bytes, sentinels
import bioseq.indexing.storage;           // the concepts
import bioseq.indexing.minimizer_hashmap; // encoded header/bucket validation
import bioseq.indexing.minimizer_fragment_image; // explicit fixed-endian minimizer fragment image image ABI
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.minimizer_fragment_compact_hash32_adapter;
import bioseq.postgres.table.io; // begin_column_read/read_next_column_row/end_column_read/column_read_state
import bioseq.postgres.storage.scanner;        // PostgreSQL-backed lazy sequence access
import bioseq.storage.lazy_sequence;           // lazy_sequence
import bioseq.postgres.types.compact_sequence; // bioseq::pg_types::seq, dispatch_instance
import bioseq.sequences;                       // compact_sequence_range
import bioseq.sequences.simd;                  // canonical bounded base decoding
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
import :fragment_image;
import :generation_writer_rows;
export import :generation_session_contract;
export import :generation_session_lifecycle;
export import :generation_session_metadata;
export import :generation_session_payload;
export import :generation_session_completion;
export namespace bioseq::storage::indexing {
/// Normal open outcome with either an owned session or a classified relation failure.
struct minimizer_generation_read_session_open_result {
    std::optional<index_storage_fragment_generation_read_session> session{};
    minimizer_generation_relation_inspection failure{
        .kind = minimizer_generation_relation_kind::malformed,
        .schema_error = minimizer_generation_schema_error::none,
    };

    [[nodiscard]] explicit operator bool() const noexcept {
        return session.has_value();
    }
};

/// Bind classification and reading to one heap descriptor and its primary
/// index. Unrelated secondary indexes are deliberately neither opened nor
/// counted.
/// A successful result owns both AccessShareLock references until finish(). An
/// unsuccessful normal result owns neither.  PostgreSQL ERROR remains an
/// exception so the transaction ResourceOwner retains its ordinary authority.
[[nodiscard]] inline minimizer_generation_read_session_open_result
open_exact_minimizer_generation_read_session_pg(std::string_view table_name,
                                                std::string_view index_name,
                                                std::uint64_t query_key,
                                                const borrowed_executor_read_context& executor_read,
                                                expected_minimizer_generation_identity expected) {
    if (!executor_read.valid()) {
        minimizer_generation_storage_error_as_cpp(
            "generation read requires a valid borrowed executor context");
    }
    minimizer_generation_read_session_open_result result{};
    try {
        result.session.emplace(table_name,
                               query_key,
                               executor_read.snapshot(),
                               expected,
                               executor_read.tablespace_oid(),
                               minimizer_generation_snapshot_registration::borrow_registered,
                               index_name);
    } catch (const minimizer_generation_relation_open_failure& failure) {
        result.failure = failure.inspection;
    }
    return result;
}

namespace minimizer_generation_compile_contract_detail {
/// Minimal sink used to instantiate and validate the exported range-read contract.
struct qualified_read_sink {
    void operator()(const index_chunk_address&, const char*, std::size_t) const noexcept {}
};
} // namespace minimizer_generation_compile_contract_detail

template index_chunk_read_stats
index_storage_fragment_generation_read_session::read_chunks_in_range<
    minimizer_generation_compile_contract_detail::qualified_read_sink>(
    std::size_t,
    index_chunk_cache_policy,
    minimizer_generation_compile_contract_detail::qualified_read_sink&&);

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

/// Read-only, PostgreSQL-owned pre-restart audit for one exact relation row,
/// column and logical minimizer index. It validates the source relfilenode and
/// attribute, the exact eight-column sidecar/PK, the address-bound row CRCs,
/// fixed head/manifest wire, every directory chunk, and all cross-object
/// metadata equations under one registered MVCC snapshot. It intentionally
/// does not activate ordinal 2 or expose a standalone SQL symbol.
[[nodiscard]] ready_minimizer_generation_identity inspect_ready_minimizer_generation_identity_pg(
    const minimizer_generation_administration_identity& identity,
    std::uint64_t sequence_rowno,
    Snapshot executor_snapshot,
    expected_minimizer_generation_identity expected) {
    const auto minimizer_generation_storage_error = [](std::string_view message) {
        minimizer_generation_storage_error_as_cpp(message);
    };
    struct source_inspection_state {
        Snapshot registered_snapshot = nullptr;
        MemoryContext work_context = nullptr;
        void* session_storage = nullptr;
        index_storage_fragment_generation_read_session* session = nullptr;
        bool session_constructed = false;
        std::optional<minimizer_generation_catalog_source_authorization> source_authorization;
    };
    auto* source = new source_inspection_state{};
    ready_minimizer_generation_identity result{};
    try {
        if (identity.database_oid != MyDatabaseId || !OidIsValid(identity.user_relid) ||
            identity.column_attno <= 0 || identity.index_name.empty() ||
            identity.table_name.empty() ||
            !bioseq::indexing::sql_bigint_can_represent(sequence_rowno) ||
            executor_snapshot == nullptr || !IsMVCCSnapshot(executor_snapshot) ||
            executor_snapshot->regd_count == 0) {
            minimizer_generation_storage_error("generation identity inspection input is invalid");
        }
        source->registered_snapshot = executor_snapshot;
        source->source_authorization.emplace(authorize_minimizer_generation_catalog_source_pg(
            identity, sequence_rowno, source->registered_snapshot));
        const minimizer_generation_source_locator& validated_source =
            source->source_authorization->source_for_diagnostics();
        source->work_context = bioseq::postgres::support::invoke_pg_as_cpp([] {
            return AllocSetContextCreate(CurrentMemoryContext,
                                         "bioseq current generation source inspection",
                                         ALLOCSET_DEFAULT_SIZES);
        });

        source->session_storage = bioseq::postgres::support::invoke_pg_as_cpp([source] {
            return MemoryContextAlloc(source->work_context,
                                      sizeof(index_storage_fragment_generation_read_session));
        });
        source->session =
            ::new (source->session_storage) index_storage_fragment_generation_read_session{
                identity.table_name,
                sequence_rowno,
                source->registered_snapshot,
                expected,
                identity.tablespace_oid,
                minimizer_generation_snapshot_registration::borrow_registered};
        source->session_constructed = true;
        const minimizer_generation_head head = source->session->read_active_head_once();
        const minimizer_generation_read_metadata& metadata =
            source->session->read_manifest_directory_once(head);
        if (metadata.manifest.alphabet_code != validated_source.alphabet_code ||
            metadata.manifest.storage_unit_byte_width != validated_source.storage_unit_byte_width ||
            metadata.manifest.target_length != validated_source.locator.logical_length) {
            minimizer_generation_storage_error("generation identity manifest/source row disagrees");
        }
        const std::uint64_t validated_source_content_checksum =
            source->source_authorization->source_content_checksum_for_diagnostics();
        const std::uint64_t validated_storage_contract_checksum =
            bioseq::indexing::compute_minimizer_generation_storage_contract_checksum(
                metadata.manifest);
        source->session->consume_source_authorization(std::move(*source->source_authorization));
        source->source_authorization.reset();
        result = ready_minimizer_generation_identity{
            .generation_id = metadata.head.generation_id,
            .source_identity = metadata.head.source_identity,
            .source_content_checksum = validated_source_content_checksum,
            .storage_contract_checksum = validated_storage_contract_checksum,
        };
        source->session->plan_control_groups(
            std::span<const minimizer_generation_payload_read_group>{});
        source->session->plan_position_groups(
            std::span<const minimizer_generation_payload_read_group>{});
        const minimizer_generation_read_stats read_stats = source->session->finish();
        if (read_stats.source_parent_rows_read != 1 || read_stats.source_catalog_rows_read != 1 ||
            read_stats.source_catalog_index_scans != 1 ||
            read_stats.source_fragment_rows_read != 0 || read_stats.source_decoded_bp != 0) {
            minimizer_generation_storage_error(
                "generation identity timed source work equation disagrees");
        }
        source->session->~index_storage_fragment_generation_read_session();
        source->session = nullptr;
        source->session_constructed = false;
        source->session_storage = nullptr;

        source->registered_snapshot = nullptr;
        MemoryContext finished_context = std::exchange(source->work_context, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_context] { MemoryContextDelete(finished_context); });
    } catch (...) {
        if (source->session_constructed && source->session != nullptr) {
            auto* cleanup_session = source->session;
            source->session = nullptr;
            source->session_constructed = false;
            cleanup_session->~index_storage_fragment_generation_read_session();
        }
        source->session_storage = nullptr;
        source->registered_snapshot = nullptr;
        // The failed statement owns the MemoryContext. Do not invoke PG
        // cleanup while propagating either a bridged PG error or a C++ error.
        source->work_context = nullptr;
        delete source;
        throw;
    }
    delete source;
    return result;
}

namespace minimizer_generation_pg_compile_contract_detail {
using publisher_type =
    ready_minimizer_generation_published_head_receipt (*)(ready_minimizer_generation_token&&);
using inspector_type =
    ready_minimizer_generation_identity (*)(const minimizer_generation_administration_identity&,
                                            std::uint64_t,
                                            Snapshot,
                                            expected_minimizer_generation_identity);

static_assert(std::same_as<decltype(&publish_ready_minimizer_generation_head_pg), publisher_type>);
static_assert(
    std::same_as<decltype(&inspect_ready_minimizer_generation_identity_pg), inspector_type>);
static_assert(requires(mapped_minimizer_fragment_image& image,
                       const mapped_minimizer_fragment_lease_guard& guard,
                       const bioseq::indexing::compact_hash32_direct_index_view& direct) {
    { image.borrow_mapping_token() } -> std::same_as<std::shared_ptr<const void>>;
    { image.advise_compact_h32_direct_metadata_readahead(direct) } -> std::same_as<void>;
    {
        image.borrow_compact_h32_selected_page_adviser(direct)
    } -> std::same_as<bioseq::indexing::compact_direct_selected_page_adviser>;
    {
        std::move(image).release_mapping_lease_guard()
    } -> std::same_as<mapped_minimizer_fragment_lease_guard>;
    { guard.borrow_mapping_token() } -> std::same_as<std::shared_ptr<const void>>;
});
static_assert(
    noexcept(std::declval<const mapped_minimizer_fragment_image&>()
                 .advise_compact_h32_direct_metadata_readahead(
                     std::declval<const bioseq::indexing::compact_hash32_direct_index_view&>())));
static_assert(noexcept(
    std::declval<const mapped_minimizer_fragment_image&>().borrow_compact_h32_selected_page_adviser(
        std::declval<const bioseq::indexing::compact_hash32_direct_index_view&>())));
static_assert(requires(index_storage_fragment_generation_row_sink& writer,
                       const encoded_grouped_soa_generation& generation,
                       const minimizer_generation_row_view& row) {
    { writer.write(row) } -> std::same_as<std::expected<void, minimizer_generation_encoding_error>>;
    {
        writer.validate_ready_generation(generation)
    } -> std::same_as<ready_minimizer_generation_token>;
});
static_assert(requires(index_storage_fragment_generation_read_session& session,
                       const minimizer_generation_head& head,
                       std::span<const minimizer_generation_payload_read_group> groups) {
    { session.read_active_head_once() } -> std::same_as<minimizer_generation_head>;
    {
        session.read_manifest_directory_once(head)
    } -> std::same_as<const minimizer_generation_read_metadata&>;
    {
        session.read_manifest_once_without_directory(head)
    } -> std::same_as<const minimizer_generation_manifest&>;
    { session.plan_control_groups(groups) };
    { session.plan_position_groups(groups) };
    {
        session.read_full_generation_payload_once(
            minimizer_generation_compile_contract_detail::qualified_read_sink{},
            minimizer_generation_compile_contract_detail::qualified_read_sink{})
    } -> std::same_as<index_chunk_read_stats>;
    { session.finish() } -> std::same_as<minimizer_generation_read_stats>;
    { session.finish_mapped_compact_direct() } -> std::same_as<minimizer_generation_read_stats>;
});

constexpr minimizer_generation_source_locator k_source_identity_golden{
    .sequence_rowno = 17,
    .locator =
        sequence_locator{
            .payload_version = sequence_locator_payload_version,
            .bucket_index = 9,
            .reserved_16 = 0,
            .shard_index = 4,
            .fragment_count = 3,
            .reserved_32 = 0,
            .logical_length = 12'345,
            .first_fragment_row = 77,
            .source_epoch = 101,
        },
    .alphabet_code = 1,
    .storage_unit_byte_width = 8,
    .padding_letters = 3,
    .storage_contract_checksum = 0x0102030405060708ULL,
};
constexpr auto k_source_identity_different_epoch = [] {
    auto source = k_source_identity_golden;
    ++source.locator.source_epoch;
    return source;
}();
static_assert(minimizer_generation_source_identity(42, k_source_identity_golden) == 101);
static_assert(minimizer_generation_source_identity(42, k_source_identity_golden) !=
              minimizer_generation_source_identity(42, k_source_identity_different_epoch));
static_assert(minimizer_generation_source_identity(42, k_source_identity_golden) ==
              minimizer_generation_source_identity(43, k_source_identity_golden));
} // namespace minimizer_generation_pg_compile_contract_detail

} // namespace bioseq::storage::indexing
