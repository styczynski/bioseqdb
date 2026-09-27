/**
 * @file core.cppm
 * @brief Shared catalog operations for columnar storage metadata.
 *
 * Tables in bioseq_internal store stripe ranges, chunk locations and row
 * counts. Readers use the same metadata for chunk filtering and row lookup
 * that VACUUM and the planner use for storage statistics.
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

/// @brief Read a physical relation's storage id from its metapage.
/// @param[in] relfilelocator Locator of a relation with initialized columnar storage.
/// @return Persistent storage id.
export uint64_t lookup_storage_id(RelFileLocator relfilelocator);
/// @brief Find the stripe with the latest start at or before a row number.
/// @param[in] relation Open columnar relation.
/// @param[in] row_number Logical row number to look up.
/// @param[in] snapshot Snapshot used to read metadata.
/// @return Metadata in the current memory context, or nullptr. Check the stripe
/// end separately to determine whether the requested row is in it.
export stripe_metadata*
find_stripe_by_first_row_number(Relation relation, uint64_t row_number, Snapshot snapshot);
/// @brief Computes the inclusive highest logical row number stored by a stripe.
/// @param[in] stripe Stripe metadata with a nonzero row count.
/// @return Inclusive final row number.
export uint64_t stripe_highest_row_number(stripe_metadata* stripe);
/** @brief Find a data type's support function in its default operator class.
 * Try the operator class's input type if the data type has no matching function.
 * Return nullptr when the operator class, family or function is missing.
 */
export FmgrInfo*
find_index_support_function(Oid type_id, Oid access_method_id, int16_t procedure_id) {
    FmgrInfo* function_info = nullptr;

    /* Find the type's default operator class for this access method. */
    Oid operator_class_id = GetDefaultOpClass(type_id, access_method_id);
    if (operator_class_id == InvalidOid) {
        return nullptr;
    }

    Oid operator_family_id = get_opclass_family(operator_class_id);
    if (operator_family_id == InvalidOid) {
        return nullptr;
    }

    Oid operator_id = get_opfamily_proc(operator_family_id, type_id, type_id, procedure_id);

    /* Try the operator class's input type when the data type has no match. */
    if (operator_id == InvalidOid) {
        Oid opcintype = get_opclass_input_type(operator_class_id);
        operator_id = get_opfamily_proc(operator_family_id, opcintype, opcintype, procedure_id);
    }

    if (operator_id != InvalidOid) {
        function_info = bioseq::postgres::support::allocate_zeroed_object<FmgrInfo>();

        /* Prepare the function for PostgreSQL calls. */
        fmgr_info(operator_id, function_info);
    }

    return function_info;
}

/// @brief Which side of a row number to search for a stripe start.
enum class row_number_lookup_mode {
    /// Find the latest stripe start at or before the requested row.
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
/// @brief Find the last occupied logical byte and highest stripe id with a dirty snapshot.
void find_highest_used_address_and_id(uint64_t storage_id,
                                      uint64_t* highest_used_address,
                                      uint64_t* highest_used_id);
/// @brief Update non-indexed stripe fields in place and return the updated metadata.
stripe_metadata*
update_stripe_metadata_row(uint64_t storage_id, uint64_t stripe_id, stripe_codec::update& update);
/// @brief Read visible stripes in ascending or descending first-row order.
List* read_data_file_stripes(uint64_t storage_id, Snapshot snapshot, ScanDirection scan_direction);
/// @brief Decode a stripe heap tuple, including its transaction state.
export stripe_metadata* build_stripe_metadata(Relation bioseq_column_stripes, HeapTuple heap_tuple);
/// @brief Reads row and deletion counts for every chunk group in one stripe.
void read_chunk_group_row_counts(uint64_t storage_id,
                                 uint64_t stripe,
                                 uint32_t chunk_group_count,
                                 uint32_t** chunk_group_row_counts,
                                 uint32_t** chunk_group_deleted_rows,
                                 Snapshot snapshot);
/// @brief Find the bioseq_internal.storageid_seq OID.
Oid column_storage_id_sequence_oid(void);
/// @brief Find the bioseq_internal.stripe OID.
export Oid column_stripe_relation_oid(void);
/// @brief Find the stripe catalog's primary-key index OID.
export Oid column_stripe_primary_key_index_oid(void);
/// @brief Find the index used to look up stripes by their first row.
Oid stripe_first_row_number_index_oid(void);
/// @brief Find the bioseq_internal.options OID.
Oid column_options_relation_oid(void);
/// @brief Find the options catalog's primary-key index OID.
Oid column_options_index_oid(void);
/// @brief Find the bioseq_internal.chunk OID.
Oid column_chunk_relation_oid(void);
/// @brief Find the bioseq_internal.chunk_group OID.
Oid column_chunk_group_relation_oid(void);
// Row-mask writers in other partitions use these catalog OIDs.
/// @brief Find the bioseq_internal.row_mask OID.
export Oid column_row_mask_relation_oid(void);
/// @brief Find the bioseq_internal.row_mask_seq OID.
Oid column_row_mask_sequence_oid(void);
/// @brief Find the chunk catalog's primary-key index OID.
Oid column_chunk_index_oid(void);
/// @brief Find the chunk-group catalog's primary-key index OID.
Oid column_chunk_group_index_oid(void);
/// @brief Find the row-mask catalog's primary-key index OID.
export Oid column_row_mask_index_oid(void);
/// @brief Find the index used to look up row masks by stripe.
Oid column_row_mask_stripe_index_oid(void);
/// @brief Find the bioseq_internal schema OID.
Oid column_namespace_oid(void);
/// @brief Find the highest logical row number in visible stripe metadata.
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
/// @brief Insert table options, replacing an existing row when requested.
bool write_column_options(Oid regclass, column_options* options, bool overwrite);
/// @brief Find the nearest stripe start on the requested side of a row number.
stripe_metadata* find_stripe_metadata_by_row_number(Relation relation,
                                                    uint64_t row_number,
                                                    Snapshot snapshot,
                                                    row_number_lookup_mode lookup_mode);
/// @brief Check that stripe counts and storage ranges agree with its write state.
void validate_stripe_metadata(stripe_metadata* stripe);

/**
 * @brief Attempt cleanup and keep the first exception for the caller to rethrow.
 * @tparam p_operation Cleanup callable taking no arguments.
 * @param[in,out] failure Previously captured exception, if any.
 * @param[in] operation Cleanup to attempt even when an earlier step failed.
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
