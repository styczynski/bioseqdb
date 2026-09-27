/**
 * @file catalog_contracts.cppm
 * @brief Index-generation identities, schema checks, and forced failures for tests.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h" // RelnameGetRelid
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_opclass_d.h"
#include "catalog/pg_type_d.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <new>

export module bioseq.postgres.storage.indexing.chunked_storage:catalog_contracts;

import std;
export import bioseq.postgres.storage.indexing.cold_index_timing;
export import bioseq.postgres.storage.indexing.executor_read_context;
export import bioseq.postgres.storage.indexing.minimizer_build_stats;
export import bioseq.postgres.storage.indexing.minimizer_read_stats;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.support; // bioseq::postgres::support::try_open_postgres_relation, bioseq::postgres::support::find_primary_key_oid
import bioseq.postgres.storage.manager; // seq_caches, sequence_locator, format helpers
import bioseq.storage.layout;           // sequence_locator
import bioseq.indexing.type; // sequence_index_layout, index_sub_key_t, k_max_index_chunk_size_bytes, sentinels
import bioseq.indexing.storage;           // index storage concepts
import bioseq.indexing.minimizer_hashmap; // encoded header/bucket validation
import bioseq.indexing.minimizer_fragment_image; // minimizer fragment byte layout
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.minimizer_fragment_compact_hash32_adapter;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.physical;
import bioseq.postgres.storage.indexing.sql_row_codecs;
import bioseq.postgres.storage.manager.sql_row_codecs;
import bioseq.postgres.sql.generated.core.storage_indexing_pg_chunked_storage.entity_handles;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::storage::indexing {
namespace indexing_entity_handles =
    bioseq::sql::generated::core::storage_indexing_pg_chunked_storage::entity_handles;
using minimizer_generation_storage_catalog_row =
    bioseq::storage::catalog_rows::minimizer_generation_storage;
using minimizer_generation_storage_codec =
    bioseq::postgres::storage::manager::sql_row_codecs::minimizer_generation_storage_codec;
using metadata_age_log_catalog_row = bioseq::storage::indexing::catalog_rows::metadata_age_log;
using metadata_age_log_codec =
    bioseq::postgres::storage::indexing::sql_row_codecs::metadata_age_log_codec;

static_assert(indexing_entity_handles::minimizer_generation_id_seq.schema_binding() ==
              bioseq::postgres::support::sql_schema_binding::fixed);

using bioseq::indexing::crc64_ecma182_accumulator;
using bioseq::indexing::derive_minimizer_position_extent_sub_key;
using bioseq::indexing::direct_minimizer_spill_storage_binding;
using bioseq::indexing::encoded_grouped_soa_generation;
using bioseq::indexing::encoded_hash_major_minimizer_head;
using bioseq::indexing::encoded_hash_major_minimizer_manifest;
using bioseq::indexing::expected_hash_major_minimizer_generation;
using bioseq::indexing::expected_minimizer_generation_identity;
using bioseq::indexing::grouped_soa_bucket_directory_entry;
using bioseq::indexing::grouped_soa_directory_view;
using bioseq::indexing::grouped_soa_minimizer_profile;
using bioseq::indexing::hash_major_minimizer_authorized_control_range;
using bioseq::indexing::hash_major_minimizer_authorized_posting_range;
using bioseq::indexing::hash_major_minimizer_consume_control_block_fn;
using bioseq::indexing::hash_major_minimizer_consume_posting_bytes_fn;
using bioseq::indexing::hash_major_minimizer_encoded_metadata;
using bioseq::indexing::hash_major_minimizer_error;
using bioseq::indexing::hash_major_minimizer_head;
using bioseq::indexing::hash_major_minimizer_logical_stream_sink_ref;
using bioseq::indexing::hash_major_minimizer_logical_stream_source_ref;
using bioseq::indexing::hash_major_minimizer_manifest;
using bioseq::indexing::hash_major_minimizer_storage_profile;
using bioseq::indexing::hash_major_minimizer_stream_kind;
using bioseq::indexing::index_chunk_address;
using bioseq::indexing::index_chunk_cache_policy;
using bioseq::indexing::index_chunk_read_stats;
using bioseq::indexing::index_sub_key_range;
using bioseq::indexing::index_sub_key_request;
using bioseq::indexing::index_sub_key_t;
using bioseq::indexing::k_directory_sub_key;
using bioseq::indexing::k_generation_head_sub_key;
using bioseq::indexing::k_hash_major_dense_seal_sub_key;
using bioseq::indexing::k_hash_major_generation_head_sub_key;
using bioseq::indexing::k_hash_major_minimizer_head_bytes;
using bioseq::indexing::k_hash_major_minimizer_manifest_bytes;
using bioseq::indexing::k_hash_major_minimizer_maximum_control_block_bytes;
using bioseq::indexing::k_hash_major_minimizer_read_slab_bytes;
using bioseq::indexing::k_hash_major_minimizer_row_payload_bytes;
using bioseq::indexing::k_max_index_chunk_size_bytes;
using bioseq::indexing::k_minimizers_per_fragment_index_name;
using bioseq::indexing::k_minimizers_per_sequence_index_name;
using bioseq::indexing::k_position_extent_sub_key_base;
using bioseq::indexing::k_symbol_counts_index_name;
using bioseq::indexing::k_temporary_run_sub_key_base;
using bioseq::indexing::minimizer_generation_encoding_error;
using bioseq::indexing::minimizer_generation_head;
using bioseq::indexing::minimizer_generation_manifest;
using bioseq::indexing::minimizer_generation_row_view;
using bioseq::indexing::minimizer_generation_row_write_receipt;
using bioseq::indexing::minimizer_hashmap_fragment_index;
using bioseq::indexing::project_minimizer_extent_sub_key;
using bioseq::indexing::sequence_index_layout;
using bioseq::indexing::unchecked_minimizer_generation_row_view;
using bioseq::indexing::validated_minimizer_generation_row_receipt;

static_assert(k_hash_major_generation_head_sub_key > k_temporary_run_sub_key_base);
static_assert(k_hash_major_generation_head_sub_key < k_hash_major_dense_seal_sub_key);
static_assert(k_hash_major_dense_seal_sub_key < k_generation_head_sub_key);

// Check the shared constants against PostgreSQL's allocation and varlena macros.
static_assert(bioseq::indexing::k_varlena_header_bytes == VARHDRSZ, "VARHDRSZ mirror drift");
static_assert(bioseq::indexing::k_postgres_max_allocation_bytes == MaxAllocSize,
              "MaxAllocSize mirror drift");

/// Counts of index vables and metadata-age rows removed by a clear operation.
struct minimizer_generation_clear_result {
    std::uint64_t dropped_relation_count = 0;
    std::uint64_t deleted_metadata_age_rows = 0;
};

/// Database, relation, column, and index names used to locate a stored generation.
struct minimizer_generation_administration_identity {
    Oid database_oid = InvalidOid;
    Oid tablespace_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    Oid user_relid = InvalidOid;
    std::int16_t column_attno = 0;
    std::string index_name;
    std::string table_name;

    /** Free the owned catalog-name strings. */
    void release_owned_strings() noexcept {
        std::string{}.swap(index_name);
        std::string{}.swap(table_name);
    }
};

/** Source row number, locator, and sequence format checked, for  a generation. */
struct minimizer_generation_source_locator {
    std::uint64_t sequence_rowno = 0;
    sequence_locator locator{};
    std::uint8_t alphabet_code = 0;
    std::uint8_t storage_unit_byte_width = 0;
    std::uint16_t padding_letters = 0;
    std::uint64_t storage_contract_checksum = 0;

    /** Compares every persisted source identity field. */
    [[nodiscard]] bool operator==(const minimizer_generation_source_locator& other) const noexcept {
        return sequence_rowno == other.sequence_rowno &&
               locator.payload_version == other.locator.payload_version &&
               locator.bucket_index == other.locator.bucket_index &&
               locator.reserved_16 == other.locator.reserved_16 &&
               locator.shard_index == other.locator.shard_index &&
               locator.fragment_count == other.locator.fragment_count &&
               locator.reserved_32 == other.locator.reserved_32 &&
               locator.logical_length == other.locator.logical_length &&
               locator.first_fragment_row == other.locator.first_fragment_row &&
               locator.source_epoch == other.locator.source_epoch &&
               alphabet_code == other.alphabet_code &&
               storage_unit_byte_width == other.storage_unit_byte_width &&
               padding_letters == other.padding_letters &&
               storage_contract_checksum == other.storage_contract_checksum;
    }
};

class index_storage_fragment_generation_row_sink;
class index_storage_fragment_generation_read_session;
class index_storage_hash_major_minimizer_writer;

/**
 * Proof that the visible source row and all its sequence fragments were checked
 * under one snapshot. Writers and validating readers consume this move-only
 * value to bind a generation to that source and its content checksum.
 */
class minimizer_generation_source_authorization {
  private:
    minimizer_generation_source_locator source_{};
    std::uint64_t source_content_checksum_ = 0;
    Oid database_oid_ = InvalidOid;
    Oid tablespace_oid_ = InvalidOid;
    Oid main_relfilenode_ = InvalidOid;
    Oid user_relid_ = InvalidOid;
    std::int16_t column_attno_ = 0;
    std::string index_name_{};
    std::string table_name_{};
    Snapshot snapshot_ = nullptr;
    TransactionId transaction_id_ = InvalidTransactionId;
    bool valid_ = false;

    minimizer_generation_source_authorization() = default;

    /** Clear identity fields and names while leaving snapshot cleanup to the caller. */
    void invalidate_() noexcept {
        source_ = {};
        source_content_checksum_ = 0;
        database_oid_ = InvalidOid;
        tablespace_oid_ = InvalidOid;
        main_relfilenode_ = InvalidOid;
        user_relid_ = InvalidOid;
        column_attno_ = 0;
        std::string{}.swap(index_name_);
        std::string{}.swap(table_name_);
        snapshot_ = nullptr;
        transaction_id_ = InvalidTransactionId;
        valid_ = false;
    }

    friend minimizer_generation_source_authorization authorize_minimizer_generation_source_pg(
        const minimizer_generation_administration_identity&, std::uint64_t, Snapshot);
    friend class index_storage_fragment_generation_row_sink;
    friend class index_storage_fragment_generation_read_session;
    friend class index_storage_hash_major_minimizer_writer;

  public:
    minimizer_generation_source_authorization(const minimizer_generation_source_authorization&) =
        delete;
    minimizer_generation_source_authorization&
    operator=(const minimizer_generation_source_authorization&) = delete;
    minimizer_generation_source_authorization(
        minimizer_generation_source_authorization&& other) noexcept
        : source_(other.source_), source_content_checksum_(other.source_content_checksum_),
          database_oid_(other.database_oid_), tablespace_oid_(other.tablespace_oid_),
          main_relfilenode_(other.main_relfilenode_), user_relid_(other.user_relid_),
          column_attno_(other.column_attno_), index_name_(std::move(other.index_name_)),
          table_name_(std::move(other.table_name_)), snapshot_(other.snapshot_),
          transaction_id_(other.transaction_id_), valid_(other.valid_) {
        other.invalidate_();
    }
    minimizer_generation_source_authorization&
    operator=(minimizer_generation_source_authorization&& other) noexcept {
        if (this != &other) {
            invalidate_();
            source_ = other.source_;
            source_content_checksum_ = other.source_content_checksum_;
            database_oid_ = other.database_oid_;
            tablespace_oid_ = other.tablespace_oid_;
            main_relfilenode_ = other.main_relfilenode_;
            user_relid_ = other.user_relid_;
            column_attno_ = other.column_attno_;
            index_name_ = std::move(other.index_name_);
            table_name_ = std::move(other.table_name_);
            snapshot_ = other.snapshot_;
            transaction_id_ = other.transaction_id_;
            valid_ = other.valid_;
            other.invalidate_();
        }
        return *this;
    }
    ~minimizer_generation_source_authorization() = default;

    /** Return the source locator checked when this authorization was created. */
    [[nodiscard]] const minimizer_generation_source_locator&
    source_for_diagnostics() const noexcept {
        return source_;
    }

    /** Return the source checksum verified by reading its sequence fragments. */
    [[nodiscard]] std::uint64_t source_content_checksum_for_diagnostics() const noexcept {
        return source_content_checksum_;
    }
};

static_assert(!std::is_default_constructible_v<minimizer_generation_source_authorization>);
static_assert(!std::is_copy_constructible_v<minimizer_generation_source_authorization>);
static_assert(std::is_nothrow_move_constructible_v<minimizer_generation_source_authorization>);

/** Check  the visible source row and its sequence checksum under a registered snapshot. */
[[nodiscard]] minimizer_generation_source_authorization authorize_minimizer_generation_source_pg(
    const minimizer_generation_administration_identity&, std::uint64_t, Snapshot);

class minimizer_generation_catalog_source_authorization;
/// Check the visible locator and source-version catalog under a registered snapshot.
[[nodiscard]] minimizer_generation_catalog_source_authorization
authorize_minimizer_generation_catalog_source_pg(
    const minimizer_generation_administration_identity&, std::uint64_t, Snapshot);

/** Allocates the next positive generation identifier from PostgreSQL. */
[[nodiscard]] std::uint64_t allocate_minimizer_generation_id_pg();

inline std::uint64_t g_last_index_merge_run_count = 0;

/// Reset backend-local build counters before a new administrative build.
inline void reset_last_minimizer_sequence_build_stats() noexcept {
    g_last_index_merge_run_count = 0;
    g_last_minimizer_sequence_build_stats = {};
}

/// Index-build and read operations where tests can force a PostgreSQL error
enum class minimizer_generation_failure_injection : std::uint8_t {
    none,
    minimizer_sequence_spill,
    minimizer_sequence_merge,
    generation_tts_create,
    generation_tuple_form,
    generation_tts_store,
    generation_tts_drop,
    generation_multi_insert,
    generation_reread,
    generation_spi_publish,
    generation_writer_close,
    generation_storage_close,
    generation_post_publish_reread,
    generation_reader_close,
};

#ifdef BIOSEQ_TEST_INDEXES
inline minimizer_generation_failure_injection g_minimizer_generation_failure_injection =
    minimizer_generation_failure_injection::none;

/// Arm one generation failure point for the next matching operation.
inline void
arm_minimizer_generation_failure_injection(minimizer_generation_failure_injection point) noexcept {
    g_minimizer_generation_failure_injection = point;
}

/// Clear any armed generation failure point.
inline void disarm_minimizer_generation_failure_injection() noexcept {
    g_minimizer_generation_failure_injection = minimizer_generation_failure_injection::none;
}

/// Raise a forced test error. Call this inside invoke_pg_as_cpp(), which
/// converts PostgreSQL errors into C++ exceptions.
inline void inject_minimizer_generation_failure_leaf(minimizer_generation_failure_injection point,
                                                     const char* point_name) {
    if (g_minimizer_generation_failure_injection != point)
        return;
    g_minimizer_generation_failure_injection = minimizer_generation_failure_injection::none;
    ereport(ERROR,
            (errcode(ERRCODE_IO_ERROR),
             errmsg("injected PostgreSQL generation failure"),
             errdetail("failure_point=%s", point_name),
             errhint("retry the failed index build in a fresh transaction")));
}

/// Raise a forced test error throogh the PostgreSQL-to-C++ exception wrapper.
inline void inject_minimizer_generation_failure_as_cpp(minimizer_generation_failure_injection point,
                                                       const char* point_name) {
    bioseq::postgres::support::invoke_pg_as_cpp(
        [point, point_name] { inject_minimizer_generation_failure_leaf(point, point_name); });
}
#else
inline void
arm_minimizer_generation_failure_injection(minimizer_generation_failure_injection) noexcept {}
inline void disarm_minimizer_generation_failure_injection() noexcept {}
inline void inject_minimizer_generation_failure_leaf(minimizer_generation_failure_injection,
                                                     const char*) {}
inline void inject_minimizer_generation_failure_as_cpp(minimizer_generation_failure_injection,
                                                       const char*) {}
#endif

/// Inline-sequence read operations where tests can  force a PostgreSQL error.
enum class inline_sequence_pg_leaf_failure : std::uint8_t {
    none,
    relation_lookup,
    tuple_read,
    detoast,
    close,
};

[[nodiscard]] constexpr const char*
inline_sequence_pg_leaf_failure_name(const inline_sequence_pg_leaf_failure point) noexcept {
    switch (point) {
    case inline_sequence_pg_leaf_failure::none:
        return "none";
    case inline_sequence_pg_leaf_failure::relation_lookup:
        return "relation_lookup";
    case inline_sequence_pg_leaf_failure::tuple_read:
        return "tuple_read";
    case inline_sequence_pg_leaf_failure::detoast:
        return "detoast";
    case inline_sequence_pg_leaf_failure::close:
        return "close";
    }
    return "invalid";
}

#ifdef BIOSEQ_TEST_INDEXES
inline inline_sequence_pg_leaf_failure g_inline_sequence_pg_leaf_failure =
    inline_sequence_pg_leaf_failure::none;

/// Set the next inline-sequence read failure to trigger.
inline void
arm_inline_sequence_pg_leaf_failure(const inline_sequence_pg_leaf_failure point) noexcept {
    g_inline_sequence_pg_leaf_failure = point;
}

/// Clear the armed inline sequence leaf failure.
inline void disarm_inline_sequence_pg_leaf_failure() noexcept {
    g_inline_sequence_pg_leaf_failure = inline_sequence_pg_leaf_failure::none;
}

/// Raise the selected inline-sequence test error once. Clear it first so
/// the next statement can test recovery without triggering the same error.
inline void inject_inline_sequence_pg_leaf_failure(const inline_sequence_pg_leaf_failure point) {
    if (g_inline_sequence_pg_leaf_failure != point) {
        return;
    }
    g_inline_sequence_pg_leaf_failure = inline_sequence_pg_leaf_failure::none;
    ereport(ERROR,
            (errcode(ERRCODE_IO_ERROR),
             errmsg("injected inline sequence storage PostgreSQL leaf failure"),
             errdetail("failure_point=%s", inline_sequence_pg_leaf_failure_name(point)),
             errhint("retry the symbol-count read in a fresh statement")));
}
#else
inline void arm_inline_sequence_pg_leaf_failure(inline_sequence_pg_leaf_failure) noexcept {}
inline void disarm_inline_sequence_pg_leaf_failure() noexcept {}
inline void inject_inline_sequence_pg_leaf_failure(inline_sequence_pg_leaf_failure) noexcept {}
#endif

/// Query-key attribute in a fragment-chunk tuple.
inline constexpr AttrNumber k_attribute_query_key = 1;
/// Sub-key attribute in a fragment-chunk tuple.
inline constexpr AttrNumber k_attribute_sub_key = 2;
/// Chunk-number attribute in a fragment-chunk tuple.
inline constexpr AttrNumber k_attribute_chunk_number = 3;
/// Logical byte-count attribute in a fragment-chunk tuple.
inline constexpr AttrNumber k_attribute_chunk_bytes = 4;
/// Payload attribute in a fragment-chunk tuple.
inline constexpr AttrNumber k_attribute_blob = 5;

/// Source-row key column in a generation tuple.
inline constexpr AttrNumber k_minimizer_generation_attribute_query_key = 1;
/// Generation ID column in a generation tuple.
inline constexpr AttrNumber k_minimizer_generation_attribute_generation_id = 2;
/// Position-segment column in a generation tuple.
inline constexpr AttrNumber k_minimizer_generation_attribute_position_segment = 3;
/// Sub-key column in a generation tuple.
inline constexpr AttrNumber k_minimizer_generation_attribute_sub_key = 4;
/// Chunk-number  column in a generation tuple.
inline constexpr AttrNumber k_minimizer_generation_attribute_chunk_number = 5;
/// Payload byte-count column in a generation tuple.
inline constexpr AttrNumber k_minimizer_generation_attribute_chunk_bytes = 6;
/// Checksum column, covering the payload and its chunk address.
inline constexpr AttrNumber k_minimizer_generation_attribute_checksum = 7;
/// Inline, uncompressed payload column in a generation tuple.
inline constexpr AttrNumber k_minimizer_generation_attribute_blob = 8;

// systable_beginscan_ordered() takes table column numbers and maps them to
// index keys. Supply them in primary-key order. Passing index positions here
// would make PostgreSQL map them again and give B-tree the wrng key order.
inline constexpr AttrNumber k_minimizer_generation_index_generation_id =
    k_minimizer_generation_attribute_generation_id;
inline constexpr AttrNumber k_minimizer_generation_index_query_key =
    k_minimizer_generation_attribute_query_key;
inline constexpr AttrNumber k_minimizer_generation_index_sub_key =
    k_minimizer_generation_attribute_sub_key;
inline constexpr AttrNumber k_minimizer_generation_index_position_segment =
    k_minimizer_generation_attribute_position_segment;
inline constexpr AttrNumber k_minimizer_generation_index_chunk_number =
    k_minimizer_generation_attribute_chunk_number;

/// Expected name, type, and heap attribute number for one generation column.
struct minimizer_generation_index_attribute_descriptor {
    AttrNumber attno = InvalidAttrNumber;
    std::string_view name;
    Oid type_oid = InvalidOid;
};

/// Required columns and types for an eight-column generation table.
inline constexpr std::array<minimizer_generation_index_attribute_descriptor, 8>
    k_minimizer_generation_index_attribute_descriptors{{
        {k_minimizer_generation_attribute_query_key, "query_key", INT8OID},
        {k_minimizer_generation_attribute_generation_id, "generation_id", INT8OID},
        {k_minimizer_generation_attribute_position_segment, "position_segment", INT4OID},
        {k_minimizer_generation_attribute_sub_key, "sub_key", INT8OID},
        {k_minimizer_generation_attribute_chunk_number, "chunk_no", INT4OID},
        {k_minimizer_generation_attribute_chunk_bytes, "chunk_bytes", INT4OID},
        {k_minimizer_generation_attribute_checksum, "checksum", INT8OID},
        {k_minimizer_generation_attribute_blob, "blob", BYTEAOID},
    }};

/// Heap attribute numbers in the exact primary-key scan order.
inline constexpr std::array<AttrNumber, 5> k_minimizer_generation_primary_key_attributes{
    k_minimizer_generation_attribute_generation_id,
    k_minimizer_generation_attribute_query_key,
    k_minimizer_generation_attribute_sub_key,
    k_minimizer_generation_attribute_position_segment,
    k_minimizer_generation_attribute_chunk_number,
};

static_assert(k_minimizer_generation_index_attribute_descriptors.front().attno == 1);
static_assert(k_minimizer_generation_index_attribute_descriptors.back().attno == 8);
static_assert(k_minimizer_generation_primary_key_attributes.size() == 5);
static_assert(k_minimizer_generation_index_generation_id ==
              k_minimizer_generation_primary_key_attributes[0]);
static_assert(k_minimizer_generation_index_query_key ==
              k_minimizer_generation_primary_key_attributes[1]);
static_assert(k_minimizer_generation_index_sub_key ==
              k_minimizer_generation_primary_key_attributes[2]);
static_assert(k_minimizer_generation_index_position_segment ==
              k_minimizer_generation_primary_key_attributes[3]);
static_assert(k_minimizer_generation_index_chunk_number ==
              k_minimizer_generation_primary_key_attributes[4]);

/// Preserve all CRC-64 bits in PostgreSQL's signed BIGINT container.
[[nodiscard]] constexpr std::int64_t checksum_to_pg_bigint(std::uint64_t checksum) noexcept {
    return std::bit_cast<std::int64_t>(checksum);
}

/// Recover the exact CRC-64 word from PostgreSQL's signed BIGINT container.
[[nodiscard]] constexpr std::uint64_t checksum_from_pg_bigint(std::int64_t checksum) noexcept {
    return std::bit_cast<std::uint64_t>(checksum);
}

static_assert(checksum_from_pg_bigint(checksum_to_pg_bigint(0)) == 0);
static_assert(
    checksum_from_pg_bigint(checksum_to_pg_bigint((std::numeric_limits<std::uint64_t>::max)())) ==
    (std::numeric_limits<std::uint64_t>::max)());
static_assert(checksum_to_pg_bigint(std::uint64_t{1} << 63u) ==
              (std::numeric_limits<std::int64_t>::min)());

/// Reason a stored generation table or primary key failed schema validation
enum class minimizer_generation_schema_error : std::uint8_t {
    none,
    not_heap,
    attribute_count,
    attribute_number,
    attribute_dropped,
    attribute_name,
    attribute_type,
    attribute_options,
    attribute_nullable,
    blob_not_plain,
    wrong_namespace,
    wrong_relation_name,
    wrong_heap_persistence,
    wrong_index_persistence,
    wrong_heap_tablespace,
    wrong_index_tablespace,
    wrong_heap_fillfactor,
    wrong_index_fillfactor,
    primary_index_count,
    primary_index_not_unique,
    primary_index_not_immediate,
    primary_index_not_ready,
    primary_index_key_count,
    primary_index_key_order,
    primary_index_not_btree,
    primary_index_partial,
    primary_index_expression,
    primary_index_opclass,
    primary_index_collation,
    primary_index_options,
};

/// Schema-check  result and the primary-index OID when validation succeeds.
struct minimizer_generation_schema_inspection {
    minimizer_generation_schema_error error = minimizer_generation_schema_error::none;
    Oid primary_index_oid = InvalidOid;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == minimizer_generation_schema_error::none && OidIsValid(primary_index_oid);
    }
};

/// Raise a storage error as a C++ exception carrying PostgreSQL error details.
[[noreturn]] void minimizer_generation_storage_error(std::string_view message);
/// Convert a generation-storage error into a PostgreSQL-backed C++ exception.
[[noreturn]] void minimizer_generation_storage_error_as_cpp(std::string_view message);

[[nodiscard]] constexpr std::string_view
minimizer_generation_schema_error_name(minimizer_generation_schema_error error) noexcept {
    switch (error) {
    case minimizer_generation_schema_error::none:
        return "none";
    case minimizer_generation_schema_error::not_heap:
        return "relation is not heap AM";
    case minimizer_generation_schema_error::attribute_count:
        return "attribute count";
    case minimizer_generation_schema_error::attribute_number:
        return "attribute number";
    case minimizer_generation_schema_error::attribute_dropped:
        return "dropped attribute";
    case minimizer_generation_schema_error::attribute_name:
        return "attribute name";
    case minimizer_generation_schema_error::attribute_type:
        return "attribute type";
    case minimizer_generation_schema_error::attribute_options:
        return "attribute options";
    case minimizer_generation_schema_error::attribute_nullable:
        return "nullable attribute";
    case minimizer_generation_schema_error::blob_not_plain:
        return "blob storage is not PLAIN";
    case minimizer_generation_schema_error::wrong_namespace:
        return "relation namespace";
    case minimizer_generation_schema_error::wrong_relation_name:
        return "relation name";
    case minimizer_generation_schema_error::wrong_heap_persistence:
        return "heap is not UNLOGGED";
    case minimizer_generation_schema_error::wrong_index_persistence:
        return "primary index is not UNLOGGED";
    case minimizer_generation_schema_error::wrong_heap_tablespace:
        return "heap tablespace";
    case minimizer_generation_schema_error::wrong_index_tablespace:
        return "primary index tablespace";
    case minimizer_generation_schema_error::wrong_heap_fillfactor:
        return "heap fillfactor";
    case minimizer_generation_schema_error::wrong_index_fillfactor:
        return "primary index fillfactor";
    case minimizer_generation_schema_error::primary_index_count:
        return "primary index count";
    case minimizer_generation_schema_error::primary_index_not_unique:
        return "primary index is not unique";
    case minimizer_generation_schema_error::primary_index_not_immediate:
        return "primary index is deferred";
    case minimizer_generation_schema_error::primary_index_not_ready:
        return "primary index is not ready/valid/live";
    case minimizer_generation_schema_error::primary_index_key_count:
        return "primary index key count";
    case minimizer_generation_schema_error::primary_index_key_order:
        return "primary index key order";
    case minimizer_generation_schema_error::primary_index_not_btree:
        return "primary index is not btree";
    case minimizer_generation_schema_error::primary_index_partial:
        return "primary index is partial";
    case minimizer_generation_schema_error::primary_index_expression:
        return "primary index has expressions";
    case minimizer_generation_schema_error::primary_index_opclass:
        return "primary index opclass";
    case minimizer_generation_schema_error::primary_index_collation:
        return "primary index collation";
    case minimizer_generation_schema_error::primary_index_options:
        return "primary index ordering/opclass options";
    }
    return "unknown";
}

/// Optional schema, table name, and tablespace required by a caller.
struct minimizer_generation_schema_expectation {
    Oid namespace_oid = InvalidOid;
    std::string_view relation_name{};
    Oid tablespace_oid = InvalidOid;
};

/// Check the generation table layout and its single primary key. Require
/// the stored column order, uncompressed inline payloads, and the expected
/// B-tree key order and storage settings.
[[nodiscard]] inline minimizer_generation_schema_inspection
inspect_minimizer_generation_schema(Relation relation,
                                    cold_index_preparation_stats* cold_stats = nullptr,
                                    minimizer_generation_schema_expectation expected = {},
                                    Oid permitted_auxiliary_index_oid = InvalidOid) {
    if (relation == nullptr || relation->rd_rel->relam != HEAP_TABLE_AM_OID ||
        relation->rd_rel->relkind != RELKIND_RELATION) {
        return {.error = minimizer_generation_schema_error::not_heap};
    }
    if (OidIsValid(expected.namespace_oid) &&
        RelationGetNamespace(relation) != expected.namespace_oid) {
        return {.error = minimizer_generation_schema_error::wrong_namespace};
    }
    if (!expected.relation_name.empty() &&
        std::string_view{RelationGetRelationName(relation)} != expected.relation_name) {
        return {.error = minimizer_generation_schema_error::wrong_relation_name};
    }
    if (relation->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED) {
        return {.error = minimizer_generation_schema_error::wrong_heap_persistence};
    }
    if (RelationGetFillFactor(relation, HEAP_DEFAULT_FILLFACTOR) != 100) {
        return {.error = minimizer_generation_schema_error::wrong_heap_fillfactor};
    }
    if (OidIsValid(expected.tablespace_oid) &&
        relation->rd_locator.spcOid != expected.tablespace_oid) {
        return {.error = minimizer_generation_schema_error::wrong_heap_tablespace};
    }
    TupleDesc tuple_desc = RelationGetDescr(relation);
    if (tuple_desc == nullptr ||
        tuple_desc->natts !=
            static_cast<int>(k_minimizer_generation_index_attribute_descriptors.size())) {
        return {.error = minimizer_generation_schema_error::attribute_count};
    }
    for (std::size_t index = 0; index < k_minimizer_generation_index_attribute_descriptors.size();
         ++index) {
        const minimizer_generation_index_attribute_descriptor& expected =
            k_minimizer_generation_index_attribute_descriptors[index];
        Form_pg_attribute attribute = bioseq::postgres::support::tuple_descriptor_attribute(tuple_desc, static_cast<int>(index));
        if (attribute->attnum != expected.attno) {
            return {.error = minimizer_generation_schema_error::attribute_number};
        }
        if (attribute->attisdropped) {
            return {.error = minimizer_generation_schema_error::attribute_dropped};
        }
        if (std::string_view{NameStr(attribute->attname)} != expected.name) {
            return {.error = minimizer_generation_schema_error::attribute_name};
        }
        if (attribute->atttypid != expected.type_oid) {
            return {.error = minimizer_generation_schema_error::attribute_type};
        }
        if (attribute->atttypmod != -1 || OidIsValid(attribute->attcollation) ||
            attribute->atthasdef || attribute->atthasmissing || attribute->attidentity != '\0' ||
            attribute->attgenerated != '\0' || attribute->attcompression != '\0') {
            return {.error = minimizer_generation_schema_error::attribute_options};
        }
        if (!attribute->attnotnull) {
            return {.error = minimizer_generation_schema_error::attribute_nullable};
        }
        if (attribute->attstorage != TYPSTORAGE_PLAIN) {
            return {.error = minimizer_generation_schema_error::blob_not_plain};
        }
    }

    Oid primary_index_oid = InvalidOid;
    std::uint32_t index_count = 0;
    std::uint32_t primary_index_count = 0;
    minimizer_generation_schema_error primary_error = minimizer_generation_schema_error::none;
    List* index_list = RelationGetIndexList(relation);
    ListCell* cell = nullptr;
    foreach (cell, index_list) {
        const Oid index_oid = lfirst_oid(cell);
        // A corruption test  may temporarily add a donor index to copy its parsed
        // predicate or expression. Ignore only that named index during validation.
        // Normal callers require the primary key to be the table's only index.
        if (index_oid == permitted_auxiliary_index_oid)
            continue;
        ++index_count;
        Relation index_relation = index_open(index_oid, AccessShareLock);
        if (cold_stats != nullptr) {
            cold_stats->add(cold_stats->sidecar_index_opens, 1);
            cold_stats->add(cold_stats->lock_acquires, 1);
        }
        if (index_relation->rd_index != nullptr && index_relation->rd_index->indisprimary) {
            ++primary_index_count;
            primary_index_oid = index_oid;
            const Form_pg_index index_form = index_relation->rd_index;
            if (!index_form->indisunique) {
                primary_error = minimizer_generation_schema_error::primary_index_not_unique;
            } else if (index_form->indnullsnotdistinct) {
                primary_error = minimizer_generation_schema_error::primary_index_options;
            } else if (!index_form->indimmediate) {
                primary_error = minimizer_generation_schema_error::primary_index_not_immediate;
            } else if (!index_form->indisvalid || !index_form->indisready ||
                       !index_form->indislive) {
                primary_error = minimizer_generation_schema_error::primary_index_not_ready;
            } else if (index_form->indnatts !=
                           static_cast<std::int16_t>(
                               k_minimizer_generation_primary_key_attributes.size()) ||
                       index_form->indnkeyatts !=
                           static_cast<std::int16_t>(
                               k_minimizer_generation_primary_key_attributes.size())) {
                primary_error = minimizer_generation_schema_error::primary_index_key_count;
            } else if (index_relation->rd_rel->relam != BTREE_AM_OID) {
                primary_error = minimizer_generation_schema_error::primary_index_not_btree;
            } else if (index_relation->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED) {
                primary_error = minimizer_generation_schema_error::wrong_index_persistence;
            } else if (index_relation->rd_locator.spcOid != relation->rd_locator.spcOid ||
                       (OidIsValid(expected.tablespace_oid) &&
                        index_relation->rd_locator.spcOid != expected.tablespace_oid)) {
                primary_error = minimizer_generation_schema_error::wrong_index_tablespace;
            } else if (RelationGetFillFactor(index_relation, BTREE_DEFAULT_FILLFACTOR) != 90) {
                primary_error = minimizer_generation_schema_error::wrong_index_fillfactor;
            } else if (RelationGetIndexPredicate(index_relation) != NIL) {
                primary_error = minimizer_generation_schema_error::primary_index_partial;
            } else if (RelationGetIndexExpressions(index_relation) != NIL) {
                primary_error = minimizer_generation_schema_error::primary_index_expression;
            } else {
                constexpr std::array<Oid, 5> k_expected_opclasses{
                    INT8_BTREE_OPS_OID,
                    INT8_BTREE_OPS_OID,
                    INT8_BTREE_OPS_OID,
                    INT4_BTREE_OPS_OID,
                    INT4_BTREE_OPS_OID,
                };
                for (std::size_t key = 0;
                     key < k_minimizer_generation_primary_key_attributes.size();
                     ++key) {
                    if (index_form->indkey.values[key] !=
                        k_minimizer_generation_primary_key_attributes[key]) {
                        primary_error = minimizer_generation_schema_error::primary_index_key_order;
                        break;
                    }
                    if (get_index_column_opclass(index_oid, static_cast<int>(key + 1u)) !=
                        k_expected_opclasses[key]) {
                        primary_error = minimizer_generation_schema_error::primary_index_opclass;
                        break;
                    }
                    if (index_relation->rd_indcollation == nullptr ||
                        OidIsValid(index_relation->rd_indcollation[key])) {
                        primary_error = minimizer_generation_schema_error::primary_index_collation;
                        break;
                    }
                    if (index_relation->rd_indoption == nullptr ||
                        index_relation->rd_indoption[key] != 0 ||
                        (index_relation->rd_opcoptions != nullptr &&
                         index_relation->rd_opcoptions[key] != nullptr)) {
                        primary_error = minimizer_generation_schema_error::primary_index_options;
                        break;
                    }
                }
            }
        }
        index_close(index_relation, AccessShareLock);
        if (cold_stats != nullptr) {
            cold_stats->add(cold_stats->lock_releases, 1);
        }
    }
    list_free(index_list);
    if (index_count != 1 || primary_index_count != 1) {
        return {.error = minimizer_generation_schema_error::primary_index_count};
    }
    if (primary_error != minimizer_generation_schema_error::none) {
        return {.error = primary_error};
    }
    return {
        .error = minimizer_generation_schema_error::none,
        .primary_index_oid = primary_index_oid,
    };
}

/// Validate the already-open eneration table and primary key used by a reader.
/// Keeping those same descriptors for later scans avoids opening and validating
/// a second copy of the index.
[[nodiscard]] inline minimizer_generation_schema_inspection
inspect_bound_minimizer_generation_schema(Relation relation,
                                          Relation primary_index_relation,
                                          minimizer_generation_schema_expectation expected = {}) {
    if (relation == nullptr || relation->rd_rel->relam != HEAP_TABLE_AM_OID ||
        relation->rd_rel->relkind != RELKIND_RELATION) {
        return {.error = minimizer_generation_schema_error::not_heap};
    }
    if (OidIsValid(expected.namespace_oid) &&
        RelationGetNamespace(relation) != expected.namespace_oid) {
        return {.error = minimizer_generation_schema_error::wrong_namespace};
    }
    if (!expected.relation_name.empty() &&
        std::string_view{RelationGetRelationName(relation)} != expected.relation_name) {
        return {.error = minimizer_generation_schema_error::wrong_relation_name};
    }
    if (relation->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED) {
        return {.error = minimizer_generation_schema_error::wrong_heap_persistence};
    }
    if (RelationGetFillFactor(relation, HEAP_DEFAULT_FILLFACTOR) != 100) {
        return {.error = minimizer_generation_schema_error::wrong_heap_fillfactor};
    }
    if (OidIsValid(expected.tablespace_oid) &&
        relation->rd_locator.spcOid != expected.tablespace_oid) {
        return {.error = minimizer_generation_schema_error::wrong_heap_tablespace};
    }
    TupleDesc tuple_desc = RelationGetDescr(relation);
    if (tuple_desc == nullptr ||
        tuple_desc->natts !=
            static_cast<int>(k_minimizer_generation_index_attribute_descriptors.size())) {
        return {.error = minimizer_generation_schema_error::attribute_count};
    }
    for (std::size_t index = 0; index < k_minimizer_generation_index_attribute_descriptors.size();
         ++index) {
        const minimizer_generation_index_attribute_descriptor& expected =
            k_minimizer_generation_index_attribute_descriptors[index];
        Form_pg_attribute attribute = bioseq::postgres::support::tuple_descriptor_attribute(tuple_desc, static_cast<int>(index));
        if (attribute->attnum != expected.attno) {
            return {.error = minimizer_generation_schema_error::attribute_number};
        }
        if (attribute->attisdropped) {
            return {.error = minimizer_generation_schema_error::attribute_dropped};
        }
        if (std::string_view{NameStr(attribute->attname)} != expected.name) {
            return {.error = minimizer_generation_schema_error::attribute_name};
        }
        if (attribute->atttypid != expected.type_oid) {
            return {.error = minimizer_generation_schema_error::attribute_type};
        }
        if (attribute->atttypmod != -1 || OidIsValid(attribute->attcollation) ||
            attribute->atthasdef || attribute->atthasmissing || attribute->attidentity != '\0' ||
            attribute->attgenerated != '\0' || attribute->attcompression != '\0') {
            return {.error = minimizer_generation_schema_error::attribute_options};
        }
        if (!attribute->attnotnull) {
            return {.error = minimizer_generation_schema_error::attribute_nullable};
        }
        if (attribute->attstorage != TYPSTORAGE_PLAIN) {
            return {.error = minimizer_generation_schema_error::blob_not_plain};
        }
    }

    List* index_list = RelationGetIndexList(relation);
    const bool exactly_one_index = list_length(index_list) == 1;
    list_free(index_list);
    if (!exactly_one_index || primary_index_relation == nullptr ||
        primary_index_relation->rd_index == nullptr) {
        return {.error = minimizer_generation_schema_error::primary_index_count};
    }
    const bool exact_primary =
        bioseq::postgres::support::relation_immediate_primary_key_index(relation) ==
            RelationGetRelid(primary_index_relation);
    if (!exact_primary ||
        primary_index_relation->rd_index->indrelid != RelationGetRelid(relation) ||
        !primary_index_relation->rd_index->indisprimary) {
        return {.error = minimizer_generation_schema_error::primary_index_count};
    }

    const Form_pg_index index_form = primary_index_relation->rd_index;
    if (!index_form->indisunique) {
        return {.error = minimizer_generation_schema_error::primary_index_not_unique};
    }
    if (index_form->indnullsnotdistinct) {
        return {.error = minimizer_generation_schema_error::primary_index_options};
    }
    if (!index_form->indimmediate) {
        return {.error = minimizer_generation_schema_error::primary_index_not_immediate};
    }
    if (!index_form->indisvalid || !index_form->indisready || !index_form->indislive) {
        return {.error = minimizer_generation_schema_error::primary_index_not_ready};
    }
    if (index_form->indnatts !=
            static_cast<std::int16_t>(k_minimizer_generation_primary_key_attributes.size()) ||
        index_form->indnkeyatts !=
            static_cast<std::int16_t>(k_minimizer_generation_primary_key_attributes.size())) {
        return {.error = minimizer_generation_schema_error::primary_index_key_count};
    }
    if (primary_index_relation->rd_rel->relam != BTREE_AM_OID) {
        return {.error = minimizer_generation_schema_error::primary_index_not_btree};
    }
    if (primary_index_relation->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED) {
        return {.error = minimizer_generation_schema_error::wrong_index_persistence};
    }
    if (primary_index_relation->rd_locator.spcOid != relation->rd_locator.spcOid ||
        (OidIsValid(expected.tablespace_oid) &&
         primary_index_relation->rd_locator.spcOid != expected.tablespace_oid)) {
        return {.error = minimizer_generation_schema_error::wrong_index_tablespace};
    }
    if (RelationGetFillFactor(primary_index_relation, BTREE_DEFAULT_FILLFACTOR) != 90) {
        return {.error = minimizer_generation_schema_error::wrong_index_fillfactor};
    }
    if (RelationGetIndexPredicate(primary_index_relation) != NIL) {
        return {.error = minimizer_generation_schema_error::primary_index_partial};
    }
    if (RelationGetIndexExpressions(primary_index_relation) != NIL) {
        return {.error = minimizer_generation_schema_error::primary_index_expression};
    }
    constexpr std::array<Oid, 5> k_expected_opclasses{
        INT8_BTREE_OPS_OID,
        INT8_BTREE_OPS_OID,
        INT8_BTREE_OPS_OID,
        INT4_BTREE_OPS_OID,
        INT4_BTREE_OPS_OID,
    };
    const Oid primary_index_oid = RelationGetRelid(primary_index_relation);
    for (std::size_t key = 0; key < k_minimizer_generation_primary_key_attributes.size(); ++key) {
        if (index_form->indkey.values[key] != k_minimizer_generation_primary_key_attributes[key]) {
            return {.error = minimizer_generation_schema_error::primary_index_key_order};
        }
        if (get_index_column_opclass(primary_index_oid, static_cast<int>(key + 1u)) !=
            k_expected_opclasses[key]) {
            return {.error = minimizer_generation_schema_error::primary_index_opclass};
        }
        if (primary_index_relation->rd_indcollation == nullptr ||
            OidIsValid(primary_index_relation->rd_indcollation[key])) {
            return {.error = minimizer_generation_schema_error::primary_index_collation};
        }
        if (primary_index_relation->rd_indoption == nullptr ||
            primary_index_relation->rd_indoption[key] != 0 ||
            (primary_index_relation->rd_opcoptions != nullptr &&
             primary_index_relation->rd_opcoptions[key] != nullptr)) {
            return {.error = minimizer_generation_schema_error::primary_index_options};
        }
    }
    return {
        .error = minimizer_generation_schema_error::none,
        .primary_index_oid = primary_index_oid,
    };
}

/// Whether a generation  table is missing, valid, or malformed.
enum class minimizer_generation_relation_kind : std::uint8_t {
    missing,
    current_exact,
    malformed,
};

/// Table classification with the schema mismatch, when one was found.
struct minimizer_generation_relation_inspection {
    minimizer_generation_relation_kind kind = minimizer_generation_relation_kind::malformed;
    minimizer_generation_schema_error schema_error = minimizer_generation_schema_error::none;
};

/// Report a missing or malformed generation table to the read-session caller
/// The binder releases open descriptors before, this C++ failure escapes.
/// PostgreSQL errors use postgres_error and transaction cleanup.
struct minimizer_generation_relation_open_failure {
    minimizer_generation_relation_inspection inspection{};
};

/// Corrupt a primary key for an integration test by copying a donor index's
/// parsed predicate or expression into pg_index. This requires a superuser.
/// Tests must  roll back the transaction after checking that readers reject it.
inline void copy_primary_index_shape_pg(Oid table_oid, Oid donor_index_oid, bool copy_predicate) {
    if (!superuser()) {
        minimizer_generation_storage_error(
            "current generation index-shape corruption requires superuser");
    }
    Relation target =
        bioseq::postgres::support::try_open_postgres_relation(table_oid, AccessShareLock);
    if (target == nullptr) {
        minimizer_generation_storage_error(
            "current generation index-shape target relation is missing");
    }
    const minimizer_generation_schema_inspection schema =
        inspect_minimizer_generation_schema(target, nullptr, {}, donor_index_oid);
    relation_close(target, AccessShareLock);
    if (!schema) {
        minimizer_generation_storage_error(
            "current generation index-shape target schema is not initially valid");
    }
    Relation donor =
        bioseq::postgres::support::try_open_postgres_relation(donor_index_oid, AccessShareLock);
    if (donor == nullptr || donor->rd_index == nullptr || donor->rd_index->indrelid != table_oid ||
        (copy_predicate ? RelationGetIndexPredicate(donor) == NIL
                        : RelationGetIndexExpressions(donor) == NIL)) {
        if (donor != nullptr)
            relation_close(donor, AccessShareLock);
        minimizer_generation_storage_error(
            "current generation index-shape donor does not carry the requested shape");
    }
    relation_close(donor, AccessShareLock);

    Relation catalog = table_open(IndexRelationId, RowExclusiveLock);
    HeapTuple primary_tuple =
        SearchSysCacheCopy1(INDEXRELID, ObjectIdGetDatum(schema.primary_index_oid));
    HeapTuple donor_tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(donor_index_oid));
    if (!HeapTupleIsValid(primary_tuple) || !HeapTupleIsValid(donor_tuple)) {
        if (HeapTupleIsValid(primary_tuple))
            heap_freetuple(primary_tuple);
        if (HeapTupleIsValid(donor_tuple))
            ReleaseSysCache(donor_tuple);
        table_close(catalog, RowExclusiveLock);
        minimizer_generation_storage_error(
            "current generation index-shape catalog tuple is missing");
    }
    const AttrNumber shape_attno = copy_predicate ? Anum_pg_index_indpred : Anum_pg_index_indexprs;
    bool donor_is_null = false;
    const Datum donor_shape =
        heap_getattr(donor_tuple, shape_attno, RelationGetDescr(catalog), &donor_is_null);
    if (donor_is_null) {
        heap_freetuple(primary_tuple);
        ReleaseSysCache(donor_tuple);
        table_close(catalog, RowExclusiveLock);
        minimizer_generation_storage_error(
            "current generation index-shape donor catalog value is NULL");
    }
    std::array<Datum, Natts_pg_index> values{};
    std::array<bool, Natts_pg_index> nulls{};
    std::array<bool, Natts_pg_index> replaces{};
    values[shape_attno - 1] = datumCopy(donor_shape, false, -1);
    replaces[shape_attno - 1] = true;
    HeapTuple modified = heap_modify_tuple(
        primary_tuple, RelationGetDescr(catalog), values.data(), nulls.data(), replaces.data());
    CatalogTupleUpdate(catalog, &primary_tuple->t_self, modified);
    heap_freetuple(modified);
    pfree(DatumGetPointer(values[shape_attno - 1]));
    heap_freetuple(primary_tuple);
    ReleaseSysCache(donor_tuple);
    table_close(catalog, RowExclusiveLock);
    CommandCounterIncrement();
    CacheInvalidateRelcacheByRelid(table_oid);
    CacheInvalidateRelcacheByRelid(schema.primary_index_oid);
}



} // namespace bioseq::storage::indexing
