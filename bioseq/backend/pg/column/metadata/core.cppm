/**
 * @file core.cppm
 * @brief Transactional catalog operations for columnar relation metadata.
 *
 * Manages metadata for columnar relations in separate, shared metadata
 * tables in the "bioseq" catalog schema.
 *
 *   * holds basic stripe information including data size and row counts
 *   * holds basic chunk and chunk group information like data offsets and
 *     min/max values (used for Chunk Group Filtering)
 *   * useful for fast VACUUM operations (e.g. reporting with VACUUM VERBOSE)
 *   * useful for stats/costing
 *   * maps logical row numbers to stripe IDs
 *
 * Each metadata table is opened by oid through the per-call helpers in
 * the "bioseq.foo_relid" family further down. Lookups go through index
 * scans (systable_beginscan_ordered) so they're transactional and
 * MVCC-safe.
 *
 */
module;
#include <algorithm>
#include <array>
#include <bioseq/pg_compat.h>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <sys/stat.h>
#include <type_traits>
#include <utility>
#include <vector>

#include "safe_lib.h"

export module bioseq.postgres.storage.metadata:core;

import bioseq.common.checked_arithmetic;
import bioseq.postgres.storage.physical;
import bioseq.postgres.storage.compression;
import bioseq.common.logging;
import bioseq.postgres.configuration.core;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.postgres.sql.generated.core.storage_metadata_core.entity_handles;
import bioseq.postgres.storage.metadata.sql_row_codecs;
import :types;
import :tuple_ops;

namespace metadata_entity_handles =
    bioseq::sql::generated::core::storage_metadata_core::entity_handles;
using row_mask_codec = bioseq::postgres::storage::metadata::sql_row_codecs::row_mask_codec;
using row_mask_row = row_mask_codec::row_type;
using options_codec = bioseq::postgres::storage::metadata::sql_row_codecs::options_codec;
using options_row = options_codec::row_type;
using stripe_codec = bioseq::postgres::storage::metadata::sql_row_codecs::stripe_codec;
using stripe_row = stripe_codec::row_type;
using chunk_group_codec = bioseq::postgres::storage::metadata::sql_row_codecs::chunk_group_codec;
using chunk_group_row = chunk_group_codec::row_type;
using chunk_codec = bioseq::postgres::storage::metadata::sql_row_codecs::chunk_codec;
using chunk_row = chunk_codec::row_type;
using seq_sub_table_codec =
    bioseq::postgres::storage::metadata::sql_row_codecs::seq_sub_table_codec;
using seq_sub_table_row = seq_sub_table_codec::row_type;

inline constexpr std::uint64_t row_mask_chunk_row_capacity = 10'000;

static_assert(metadata_entity_handles::row_mask_seq.schema_binding() ==
              bioseq::postgres::support::sql_schema_binding::fixed);

/// @brief Reads the persistent storage identifier for a physical relation.
/// @param[in] relfilelocator Physical relation locator whose metadata page is inspected.
/// @return Stable storage identifier recorded in the relation fork.
export uint64_t lookup_storage_id(RelFileLocator relfilelocator);
/// @brief Finds the stripe beginning at the greatest row number not above the query.
/// @param[in] relation Open Bioseq relation used to resolve its storage identity.
/// @param[in] row_number Logical row number to locate.
/// @param[in] snapshot Snapshot governing metadata visibility.
/// @return Metadata allocated in the current PostgreSQL memory context, or `nullptr`.
export stripe_metadata*
find_stripe_by_first_row_number(Relation relation, uint64_t row_number, Snapshot snapshot);
/// @brief Computes the inclusive highest logical row number stored by a stripe.
/// @param[in] stripe Stripe metadata with a nonzero row count.
/// @return Inclusive final row number.
export uint64_t stripe_highest_row_number(stripe_metadata* stripe);
/** @brief Resolves an access-method support function for a data type.
 *
 * Falls back from the datum type to the operator class input type before
 * constructing the function manager record.
 */
export FmgrInfo*
find_index_support_function(Oid type_id, Oid access_method_id, int16_t procedure_id) {
    FmgrInfo* function_info = nullptr;

    /* get default operator class from pg_opclass for datum type */
    Oid operator_class_id = GetDefaultOpClass(type_id, access_method_id);
    if (operator_class_id == InvalidOid) {
        return nullptr;
    }

    Oid operator_family_id = get_opclass_family(operator_class_id);
    if (operator_family_id == InvalidOid) {
        return nullptr;
    }

    Oid operator_id = get_opfamily_proc(operator_family_id, type_id, type_id, procedure_id);

    /* No operator for type_id, search for operator class type*/
    if (operator_id == InvalidOid) {
        Oid opcintype = get_opclass_input_type(operator_class_id);
        operator_id = get_opfamily_proc(operator_family_id, opcintype, opcintype, procedure_id);
    }

    if (operator_id != InvalidOid) {
        function_info = bioseq::postgres::support::allocate_zeroed_object<FmgrInfo>();

        /* fill in the FmgrInfo struct using the operator_id */
        fmgr_info(operator_id, function_info);
    }

    return function_info;
}

/// @brief Directional relation between a row number and a stripe boundary.
enum class row_number_lookup_mode {
    /// Selects the stripe starting at or before the requested row.
    less_or_equal,

    /// Selects the first stripe starting after the requested row.
    greater
};

/// @brief Inserts the initial catalog row for a newly allocated stripe.
void insert_empty_stripe_metadata_row(uint64_t storage_id,
                                      uint64_t stripe_id,
                                      uint32_t column_count,
                                      uint32_t chunk_group_row_count,
                                      uint64_t first_row_number);
/// @brief Finds the greatest occupied byte address and stripe identifier for storage.
void find_highest_used_address_and_id(uint64_t storage_id,
                                      uint64_t* highest_used_address,
                                      uint64_t* highest_used_id);
/// @brief Applies a codec update and returns the resulting stripe metadata.
stripe_metadata*
update_stripe_metadata_row(uint64_t storage_id, uint64_t stripe_id, stripe_codec::update& update);
/// @brief Reads all visible stripes for storage in the requested index order.
List* read_data_file_stripes(uint64_t storage_id, Snapshot snapshot, ScanDirection scan_direction);
/// @brief Decodes a stripe catalog tuple into current-context metadata.
export stripe_metadata* build_stripe_metadata(Relation bioseq_column_stripes, HeapTuple heap_tuple);
/// @brief Reads row and deletion counts for every chunk group in one stripe.
void read_chunk_group_row_counts(uint64_t storage_id,
                                 uint64_t stripe,
                                 uint32_t chunk_group_count,
                                 uint32_t** chunk_group_row_counts,
                                 uint32_t** chunk_group_deleted_rows,
                                 Snapshot snapshot);
/// @brief Resolves the storage-identifier sequence OID.
Oid column_storage_id_sequence_oid(void);
/// @brief Resolves the stripe catalog relation OID.
export Oid column_stripe_relation_oid(void);
/// @brief Resolves the stripe primary-key index OID.
export Oid column_stripe_primary_key_index_oid(void);
/// @brief Resolves the stripe first-row index OID.
Oid stripe_first_row_number_index_oid(void);
/// @brief Resolves the column-options catalog relation OID.
Oid column_options_relation_oid(void);
/// @brief Resolves the column-options primary-key index OID.
Oid column_options_index_oid(void);
/// @brief Resolves the chunk catalog relation OID.
Oid column_chunk_relation_oid(void);
/// @brief Resolves the chunk-group catalog relation OID.
Oid column_chunk_group_relation_oid(void);
// Row-mask write state consumes these catalog identities across partitions.
/// @brief Resolves the row-mask catalog relation OID.
export Oid column_row_mask_relation_oid(void);
/// @brief Resolves the row-mask identifier sequence OID.
Oid column_row_mask_sequence_oid(void);
/// @brief Resolves the chunk primary-key index OID.
Oid column_chunk_index_oid(void);
/// @brief Resolves the chunk-group primary-key index OID.
Oid column_chunk_group_index_oid(void);
/// @brief Resolves the row-mask primary-key index OID.
export Oid column_row_mask_index_oid(void);
/// @brief Resolves the row-mask stripe index OID.
Oid column_row_mask_stripe_index_oid(void);
/// @brief Resolves the internal metadata schema OID.
Oid column_namespace_oid(void);
/// @brief Finds the greatest logical row number present in storage.
uint64_t find_highest_used_row_number(uint64_t storage_id);
/// @brief Deletes catalog rows belonging to one storage identifier.
void delete_storage_metadata_rows(Oid metadata_table_id,
                                  AttrNumber storage_id_attribute_number,
                                  Oid storage_id_index_id,
                                  uint64_t storage_id);
/// @brief Deletes catalog rows belonging to one stripe within storage.
void delete_stripe_metadata_rows(Oid metadata_table_id,
                                 AttrNumber storage_id_attribute_number,
                                 AttrNumber stripe_id_attr_number,
                                 Oid storage_id_index_id,
                                 uint64_t storage_id,
                                 uint64_t stripe_id);
/// @brief Writes relation options with optional replacement of an existing row.
bool write_column_options(Oid regclass, column_options* options, bool overwrite);
/// @brief Finds a stripe relative to a logical row-number boundary.
stripe_metadata* find_stripe_metadata_by_row_number(Relation relation,
                                                    uint64_t row_number,
                                                    Snapshot snapshot,
                                                    row_number_lookup_mode lookup_mode);
/// @brief Validates stripe count and address invariants before use.
void validate_stripe_metadata(stripe_metadata* stripe);

/**
 * @brief Runs cleanup while preserving the first exception for later propagation.
 * @tparam p_operation Nullary cleanup callable.
 * @param[in,out] failure First captured exception, if any.
 * @param[in] operation Cleanup operation to attempt.
 */
template <typename p_operation>
void record_cleanup_failure(std::exception_ptr& failure, p_operation&& operation) {
    try {
        std::forward<p_operation>(operation)();
    } catch (...) {
        if (failure == nullptr) {
            failure = std::current_exception();
        }
    }
}
