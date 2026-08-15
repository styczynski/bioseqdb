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

export uint64_t lookup_storage_id(RelFileLocator relfilelocator);
export stripe_metadata*
find_stripe_by_first_row_number(Relation relation, uint64_t row_number, Snapshot snapshot);
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
    /*
     * Find the stripe whose first_row_number is less than or equal to given
     * input row_number.
     */
    less_or_equal,

    /*
     * Find the stripe whose first_row_number is greater than input row_number.
     */
    greater
};

void insert_empty_stripe_metadata_row(uint64_t storage_id,
                                      uint64_t stripe_id,
                                      uint32_t column_count,
                                      uint32_t chunk_group_row_count,
                                      uint64_t first_row_number);
void find_highest_used_address_and_id(uint64_t storage_id,
                                      uint64_t* highest_used_address,
                                      uint64_t* highest_used_id);
stripe_metadata*
update_stripe_metadata_row(uint64_t storage_id, uint64_t stripe_id, stripe_codec::update& update);
List* read_data_file_stripes(uint64_t storage_id, Snapshot snapshot, ScanDirection scan_direction);
export stripe_metadata* build_stripe_metadata(Relation bioseq_column_stripes, HeapTuple heap_tuple);
void read_chunk_group_row_counts(uint64_t storage_id,
                                 uint64_t stripe,
                                 uint32_t chunk_group_count,
                                 uint32_t** chunk_group_row_counts,
                                 uint32_t** chunk_group_deleted_rows,
                                 Snapshot snapshot);
Oid column_storage_id_sequence_oid(void);
export Oid column_stripe_relation_oid(void);
export Oid column_stripe_primary_key_index_oid(void);
Oid stripe_first_row_number_index_oid(void);
Oid column_options_relation_oid(void);
Oid column_options_index_oid(void);
Oid column_chunk_relation_oid(void);
Oid column_chunk_group_relation_oid(void);
// Row-mask write state consumes these catalog identities across partitions.
export Oid column_row_mask_relation_oid(void);
Oid column_row_mask_sequence_oid(void);
Oid column_chunk_index_oid(void);
Oid column_chunk_group_index_oid(void);
export Oid column_row_mask_index_oid(void);
Oid column_row_mask_stripe_index_oid(void);
Oid column_namespace_oid(void);
uint64_t find_highest_used_row_number(uint64_t storage_id);
void delete_storage_metadata_rows(Oid metadata_table_id,
                                  AttrNumber storage_id_attribute_number,
                                  Oid storage_id_index_id,
                                  uint64_t storage_id);
void delete_stripe_metadata_rows(Oid metadata_table_id,
                                 AttrNumber storage_id_attribute_number,
                                 AttrNumber stripe_id_attr_number,
                                 Oid storage_id_index_id,
                                 uint64_t storage_id,
                                 uint64_t stripe_id);
bool write_column_options(Oid regclass, column_options* options, bool overwrite);
stripe_metadata* find_stripe_metadata_by_row_number(Relation relation,
                                                    uint64_t row_number,
                                                    Snapshot snapshot,
                                                    row_number_lookup_mode lookup_mode);
void validate_stripe_metadata(stripe_metadata* stripe);

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
