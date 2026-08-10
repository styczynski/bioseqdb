/*-------------------------------------------------------------------------
 *
 * core.cppm
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
 *-------------------------------------------------------------------------
 */
module;
#include <bioseq/pg_compat.h>

#include "safe_lib.h"

#include <sys/stat.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

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
using row_mask_codec =
    bioseq::storage::metadata::sql_row_codecs::row_mask_codec;
using row_mask_row = row_mask_codec::row_type;
using options_codec =
    bioseq::storage::metadata::sql_row_codecs::options_codec;
using options_row = options_codec::row_type;
using stripe_codec =
    bioseq::storage::metadata::sql_row_codecs::stripe_codec;
using stripe_row = stripe_codec::row_type;
using chunk_group_codec =
    bioseq::storage::metadata::sql_row_codecs::chunk_group_codec;
using chunk_group_row = chunk_group_codec::row_type;
using chunk_codec =
    bioseq::storage::metadata::sql_row_codecs::chunk_codec;
using chunk_row = chunk_codec::row_type;
using seq_sub_table_codec =
    bioseq::storage::metadata::sql_row_codecs::seq_sub_table_codec;
using seq_sub_table_row = seq_sub_table_codec::row_type;

inline constexpr std::uint64_t row_mask_chunk_row_capacity = 10'000;

static_assert(
    metadata_entity_handles::row_mask_seq.schema_binding()
    == bioseq::postgres::support::sql_schema_binding::fixed);

export uint64_t lookup_storage_id(RelFileLocator relfilelocator);
export stripe_metadata * find_stripe_by_first_row_number(Relation relation,
															 uint64_t row_number,
															 Snapshot snapshot);
export uint64_t stripe_highest_row_number(stripe_metadata *stripe);
/** @brief Resolves an access-method support function for a data type.
 *
 * Falls back from the datum type to the operator class input type before
 * constructing the function manager record.
 */
export FmgrInfo *
find_index_support_function(Oid type_id, Oid access_method_id, int16_t procedure_id)
{
	FmgrInfo *function_info = nullptr;

	/* get default operator class from pg_opclass for datum type */
	Oid operator_class_id = GetDefaultOpClass(type_id, access_method_id);
	if (operator_class_id == InvalidOid)
	{
		return nullptr;
	}

	Oid operator_family_id = get_opclass_family(operator_class_id);
	if (operator_family_id == InvalidOid)
	{
		return nullptr;
	}

	Oid operator_id = get_opfamily_proc(operator_family_id, type_id, type_id, procedure_id);

	/* No operator for type_id, search for operator class type*/
	if (operator_id == InvalidOid)
	{
		Oid opcintype = get_opclass_input_type(operator_class_id);
		operator_id = get_opfamily_proc(operator_family_id, opcintype, opcintype, procedure_id);
	}

	if (operator_id != InvalidOid)
	{
		function_info =
			bioseq::postgres::support::allocate_zeroed_object<FmgrInfo>();

		/* fill in the FmgrInfo struct using the operator_id */
		fmgr_info(operator_id, function_info);
	}

	return function_info;
}

/// @brief Directional relation between a row number and a stripe boundary.
enum class row_number_lookup_mode
{
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

static void insert_empty_stripe_metadata_row(uint64_t storage_id, uint64_t stripe_id,
										 uint32_t column_count, uint32_t chunk_group_row_count,
										 uint64_t first_row_number);
static void find_highest_used_address_and_id(uint64_t storage_id,
									   uint64_t *highest_used_address,
									   uint64_t *highest_used_id);
static stripe_metadata * update_stripe_metadata_row(uint64_t storage_id, uint64_t stripe_id,
												stripe_codec::update &update);
static List * read_data_file_stripes(uint64_t storage_id, Snapshot snapshot,
									 ScanDirection scan_direction);
export stripe_metadata * build_stripe_metadata(Relation bioseq_column_stripes,
											HeapTuple heap_tuple);
static void read_chunk_group_row_counts(uint64_t storage_id, uint64_t stripe,
									uint32_t chunk_group_count,
									uint32_t **chunk_group_row_counts,
									uint32_t **chunk_group_deleted_rows,
									Snapshot snapshot);
static Oid column_storage_id_sequence_oid(void);
export Oid column_stripe_relation_oid(void);
export Oid column_stripe_primary_key_index_oid(void);
static Oid stripe_first_row_number_index_oid(void);
static Oid column_options_relation_oid(void);
static Oid column_options_index_oid(void);
static Oid column_chunk_relation_oid(void);
static Oid column_chunk_group_relation_oid(void);
// Row-mask write state consumes these catalog identities across partitions.
export Oid column_row_mask_relation_oid(void);
static Oid column_row_mask_sequence_oid(void);
static Oid column_chunk_index_oid(void);
static Oid column_chunk_group_index_oid(void);
export Oid column_row_mask_index_oid(void);
static Oid column_row_mask_stripe_index_oid(void);
Oid column_namespace_oid(void);
static uint64_t find_highest_used_row_number(uint64_t storage_id);
static void delete_storage_metadata_rows(Oid metadata_table_id,
												   AttrNumber storage_id_attribute_number,
												   Oid storage_id_index_id,
												   uint64_t storage_id);
static void delete_stripe_metadata_rows(Oid metadata_table_id,
												  AttrNumber storage_id_attribute_number,
												  AttrNumber stripe_id_attr_number,
												  Oid storage_id_index_id,
												  uint64_t storage_id,
												  uint64_t stripe_id);
static bool write_column_options(Oid regclass, column_options *options, bool overwrite);
static stripe_metadata * find_stripe_metadata_by_row_number(Relation relation, uint64_t row_number,
													  Snapshot snapshot,
													  row_number_lookup_mode lookup_mode);
static void validate_stripe_metadata(stripe_metadata *stripe);

template<typename operation_type>
void record_cleanup_failure(
	std::exception_ptr& failure,
	operation_type&& operation)
{
	try
	{
		std::forward<operation_type>(operation)();
	}
	catch (...)
	{
		if (failure == nullptr)
		{
			failure = std::current_exception();
		}
	}
}


[[nodiscard]] static column_options
options_to_domain(const options_row& row)
{
	return column_options{
		.stripe_row_limit = static_cast<uint64>(row.stripe_row_limit),
		.chunk_group_row_limit =
			static_cast<uint32>(row.chunk_group_row_limit),
		.compression_algorithm = parse_compression_algorithm(NameStr(row.compression)),
		.compression_level = row.compression_level,
	};
}

/*
 * read_column_options returns the stored options for a relation,
 * GUC-derived defaults when no row exists, or std::nullopt when the options
 * catalog is unavailable during database or extension removal.
 */
export std::optional<column_options>
read_column_options(Oid regclass)
{
	ScanKeyData scan_keys[1];
	scan_keys[0] =
		options_codec::scan_key<&options_row::regclass>(regclass);

	Relation bioseq_column_options = bioseq::postgres::support::invoke_pg_as_cpp([] {
		return bioseq::postgres::support::try_open_postgres_relation(
			column_options_relation_oid(), AccessShareLock);
	});
	if (bioseq_column_options == nullptr)
	{
		/* Extension has been dropped (e.g. during DROP EXTENSION/DATABASE). */
		return std::nullopt;
	}

	Relation index = nullptr;
	SysScanDesc scan_descriptor = nullptr;
	std::optional<column_options> result;
	std::exception_ptr failure;
	try {
		index = bioseq::postgres::support::invoke_pg_as_cpp([] {
			return bioseq::postgres::support::try_open_postgres_relation(
				column_options_index_oid(), AccessShareLock);
		});
		if (index != nullptr) {
			scan_descriptor = bioseq::postgres::support::invoke_pg_as_cpp(
				[bioseq_column_options, index, &scan_keys] {
					return systable_beginscan_ordered(
						bioseq_column_options, index, nullptr, 1, scan_keys);
				});
			HeapTuple heap_tuple = bioseq::postgres::support::invoke_pg_as_cpp(
				[scan_descriptor] {
					return systable_getnext_ordered(
						scan_descriptor, ForwardScanDirection);
				});
			if (HeapTupleIsValid(heap_tuple)) {
				const options_row row = options_codec::deform(
					heap_tuple,
					RelationGetDescr(bioseq_column_options),
					CurrentMemoryContext);
				result = options_to_domain(row);
			} else {
				/* Populate options with system defaults. */
				result = column_options{
					.stripe_row_limit = static_cast<uint32_t>(
						bioseq::settings::storage::stripe::row_limit),
					.chunk_group_row_limit = static_cast<uint32_t>(
						bioseq::settings::storage::chunk_group::row_limit),
					.compression_algorithm = static_cast<bioseq::compression_algorithm>(
						static_cast<int>(bioseq::settings::storage::compression::algorithm.get_value())),
					.compression_level = bioseq::settings::storage::compression::level,
				};
			}
		}
	} catch (...) {
		failure = std::current_exception();
	}

	if (scan_descriptor != nullptr) {
		record_cleanup_failure(failure, [scan_descriptor] {
			bioseq::postgres::support::invoke_pg_as_cpp([scan_descriptor] {
				systable_endscan_ordered(scan_descriptor);
			});
		});
	}
	if (index != nullptr) {
		record_cleanup_failure(failure, [index] {
			bioseq::postgres::support::invoke_pg_as_cpp([index] {
				index_close(index, AccessShareLock);
			});
		});
	}
	record_cleanup_failure(failure, [bioseq_column_options] {
		bioseq::postgres::support::invoke_pg_as_cpp([bioseq_column_options] {
			relation_close(bioseq_column_options, AccessShareLock);
		});
	});
	if (failure != nullptr) {
		std::rethrow_exception(failure);
	}
	return result;
}



/*
 * initialize_column_options initialized the columnar table options. Meaning it writes the
 * default options to the options table if not already existing.
 */
export void
initialize_column_options(Oid regclass)
{
	/*
	 * When upgrading we retain options for all columnar tables by upgrading
	 * "bioseq.options" catalog table, so we shouldn't do anything here.
	 */
	if (IsBinaryUpgrade)
	{
		return;
	}

	column_options default_options = {
		.stripe_row_limit = static_cast<uint32_t>(bioseq::settings::storage::stripe::row_limit),
		.chunk_group_row_limit = static_cast<uint32_t>(bioseq::settings::storage::chunk_group::row_limit),
		.compression_algorithm = static_cast<bioseq::compression_algorithm>(static_cast<int>(bioseq::settings::storage::compression::algorithm.get_value())),
		.compression_level = bioseq::settings::storage::compression::level
	};

	write_column_options(regclass, &default_options, false);
}


/// @brief Replaces the authoritative column options for a relation.
export void
set_column_options(Oid regclass, column_options *options)
{
	write_column_options(regclass, options, true);
}


/** @brief Writes relation options subject to an overwrite policy.
 * @return True when the catalog row was inserted or replaced.
 */
static bool
write_column_options(Oid regclass, column_options *options, bool overwrite)
{
	/* Binary-upgrade restores own the catalog rows and must not replace them. */
	Assert(!IsBinaryUpgrade);

	bool written = false;

	NameData compression_name = { 0 };
	namestrcpy(&compression_name, compression_algorithm_name(options->compression_algorithm));

	options_codec::encoded_row encoded = options_codec::encode(
		options_row{
			.regclass = regclass,
			.chunk_group_row_limit =
				static_cast<std::int32_t>(options->chunk_group_row_limit),
			.stripe_row_limit =
				static_cast<std::int32_t>(options->stripe_row_limit),
			.compression_level =
				static_cast<std::int32_t>(options->compression_level),
			.compression = compression_name,
		},
		CurrentMemoryContext);

	/* create heap tuple and insert into catalog table */
	Relation bioseq_column_options =
		bioseq::postgres::support::open_postgres_relation(column_options_relation_oid(),
											 RowExclusiveLock);
	TupleDesc tuple_descriptor = RelationGetDescr(bioseq_column_options);

	/* find existing item to perform update if exist */
	ScanKeyData scan_keys[1] = { 0 };
	scan_keys[0] =
		options_codec::scan_key<&options_row::regclass>(regclass);

	Relation index = index_open(column_options_index_oid(), AccessShareLock);
	SysScanDesc scan_descriptor = systable_beginscan_ordered(bioseq_column_options, index, nullptr,
															1, scan_keys);

	HeapTuple heap_tuple = systable_getnext_ordered(scan_descriptor, ForwardScanDirection);
	if (HeapTupleIsValid(heap_tuple))
	{
		if (overwrite)
		{
			/* overwrite requests authoritatively replace the stored record */
			options_codec::update update{};
			update
				.set<&options_row::chunk_group_row_limit>(
					static_cast<std::int32_t>(options->chunk_group_row_limit))
				.set<&options_row::stripe_row_limit>(
					static_cast<std::int32_t>(options->stripe_row_limit))
				.set<&options_row::compression_level>(
					static_cast<std::int32_t>(options->compression_level))
				.set<&options_row::compression>(
					compression_name, CurrentMemoryContext);

			HeapTuple tuple = update.modify(
				heap_tuple, tuple_descriptor);
			CatalogTupleUpdate(bioseq_column_options, &tuple->t_self, tuple);
			written = true;
		}
	}
	else
	{
		/* inserting new record */
		HeapTuple new_tuple = encoded.form_tuple(tuple_descriptor);
		CatalogTupleInsert(bioseq_column_options, new_tuple);
		written = true;
	}

	if (written)
	{
		CommandCounterIncrement();
	}

	systable_endscan_ordered(scan_descriptor);
	index_close(index, AccessShareLock);
	relation_close(bioseq_column_options, RowExclusiveLock);

	return written;
}


/*
 * delete_column_table_options removes the columnar table options for a regclass. When
 * missing_ok is false it will throw an error when no table options can be found.
 *
 * Returns whether a record has been removed.
 */
export bool
delete_column_table_options(Oid regclass, bool missing_ok)
{
	bool result = false;

	/* Binary-upgrade restores own the catalog rows and must not delete them. */
	Assert(!IsBinaryUpgrade);

	Relation bioseq_column_options =
		bioseq::postgres::support::try_open_postgres_relation(column_options_relation_oid(),
												 RowExclusiveLock);
	if (bioseq_column_options == nullptr)
	{
		/* extension has been dropped */
		return false;
	}

	/* find existing item to remove */
	ScanKeyData scan_keys[1] = { 0 };
	scan_keys[0] =
		options_codec::scan_key<&options_row::regclass>(regclass);

	Relation index = index_open(column_options_index_oid(), AccessShareLock);
	SysScanDesc scan_descriptor = systable_beginscan_ordered(bioseq_column_options, index, nullptr,
															1, scan_keys);

	HeapTuple heap_tuple = systable_getnext_ordered(scan_descriptor, ForwardScanDirection);
	if (HeapTupleIsValid(heap_tuple))
	{
		CatalogTupleDelete(bioseq_column_options, &heap_tuple->t_self);
		CommandCounterIncrement();

		result = true;
	}
	else if (!missing_ok)
	{
		bioseq_log<logging_level::error, logging_target::postgres>( "missing options for regclass: {}", regclass);
	}

	systable_endscan_ordered(scan_descriptor);
	index_close(index, AccessShareLock);
	relation_close(bioseq_column_options, RowExclusiveLock);

	return result;
}


/*
 * save_stripe_skip_list saves chunk_list for a given stripe as rows
 * of bioseq.chunk.
 */
export void
save_stripe_skip_list(RelFileLocator relfilelocator, uint64_t stripe, stripe_skip_list *chunk_list,
				   TupleDesc tuple_descriptor)
{
	uint32_t column_index = 0;
	uint32_t chunk_index = 0;
	uint32_t column_count = chunk_list->column_count;

	uint64_t storage_id = lookup_storage_id(relfilelocator);
	Oid bioseq_column_chunk_oid = column_chunk_relation_oid();
	Relation bioseq_column_chunk = table_open(bioseq_column_chunk_oid, RowExclusiveLock);
	bioseq::storage::metadata::tuple_ops::relation_modification_state *relation_modification_state = bioseq::storage::metadata::tuple_ops::begin_relation_modification(bioseq_column_chunk);

	for (column_index = 0; column_index < column_count; column_index++)
	{
		for (chunk_index = 0; chunk_index < chunk_list->chunk_count; chunk_index++)
		{
			chunk_skip_node *chunk =
				&chunk_list->chunk_skip_nodes[column_index][chunk_index];

			std::optional<bytea*> minimum_value;
			std::optional<bytea*> maximum_value;
			if (chunk->has_min_max)
			{
				minimum_value = bioseq::postgres::support::pg_datum::from_raw(chunk->minimum_value)
					.to_bytea(TupleDescAttr(tuple_descriptor, column_index));
				maximum_value = bioseq::postgres::support::pg_datum::from_raw(chunk->maximum_value)
					.to_bytea(TupleDescAttr(tuple_descriptor, column_index));
			}
			chunk_codec::encoded_row encoded = chunk_codec::encode(
				chunk_row{
					.storage_id = static_cast<std::int64_t>(storage_id),
					.stripe_num = static_cast<std::int64_t>(stripe),
					.attr_num =
						static_cast<std::int32_t>(column_index + 1),
					.chunk_group_num =
						static_cast<std::int32_t>(chunk_index),
					.minimum_value = minimum_value,
					.maximum_value = maximum_value,
					.value_stream_offset =
						static_cast<std::int64_t>(chunk->value_stream_offset),
					.value_stream_length =
						static_cast<std::int64_t>(chunk->value_stream_length),
					.exists_stream_offset =
						static_cast<std::int64_t>(chunk->exists_stream_offset),
					.exists_stream_length =
						static_cast<std::int64_t>(chunk->exists_stream_length),
					.value_compression_type =
						static_cast<std::int32_t>(
							chunk->value_compression_algorithm),
					.value_compression_level =
						static_cast<std::int32_t>(
							chunk->value_compression_level),
					.value_decompressed_length =
						static_cast<std::int64_t>(
							chunk->value_decompressed_length),
					.value_count =
						static_cast<std::int64_t>(chunk->row_count),
				},
				CurrentMemoryContext);
			const chunk_codec::encoded_tuple_data tuple_data = encoded.tuple_data(
				RelationGetDescr(bioseq_column_chunk));

			bioseq::storage::metadata::tuple_ops::insert_catalog_tuple(
				relation_modification_state, tuple_data.values, tuple_data.nulls);
		}
	}

	bioseq::storage::metadata::tuple_ops::finish_relation_modification(relation_modification_state);
	table_close(bioseq_column_chunk, RowExclusiveLock);
}


/*
 * save_chunk_groups saves the metadata for given chunk groups in bioseq.chunk_group.
 */
export void
save_chunk_groups(RelFileLocator relfilelocator, uint64_t stripe,
				List *chunk_group_row_counts)
{
	uint64_t storage_id = lookup_storage_id(relfilelocator);
	Oid bioseq_column_chunk_group_oid = column_chunk_group_relation_oid();
	Relation bioseq_column_chunk_group = table_open(bioseq_column_chunk_group_oid, RowExclusiveLock);
	bioseq::storage::metadata::tuple_ops::relation_modification_state *relation_modification_state = bioseq::storage::metadata::tuple_ops::begin_relation_modification(bioseq_column_chunk_group);

	int chunk_id = 0;

	for (int row_count :
		 bioseq::postgres::support::postgres_integer_list{chunk_group_row_counts})
	{
		chunk_group_codec::encoded_row encoded =
			chunk_group_codec::encode(chunk_group_row{
			.storage_id = static_cast<std::int64_t>(storage_id),
			.stripe_num = static_cast<std::int64_t>(stripe),
			.chunk_group_num = static_cast<std::int32_t>(chunk_id),
			.row_count = static_cast<std::int64_t>(row_count),
		});
		const chunk_group_codec::encoded_tuple_data tuple_data = encoded.tuple_data(
			RelationGetDescr(bioseq_column_chunk_group));

		bioseq::storage::metadata::tuple_ops::insert_catalog_tuple(
			relation_modification_state, tuple_data.values, tuple_data.nulls);
		chunk_id++;
	}

	bioseq::storage::metadata::tuple_ops::finish_relation_modification(relation_modification_state);
	table_close(bioseq_column_chunk_group, RowExclusiveLock);
}


/*
 * save_empty_row_mask saves the metadata for inserted rows in bioseq.mask_row
 */
export bool
save_empty_row_mask(uint64_t storage_id, uint64_t stripe_id,
				 uint64_t stripe_start_row_number, List *chunk_group_row_counts)
{
	Oid row_mask_relation_oid = column_row_mask_relation_oid();
	Oid bioseq_column_row_mask_seq = column_row_mask_sequence_oid();
	Relation row_mask_relation = table_open(row_mask_relation_oid, RowExclusiveLock);
	bioseq::storage::metadata::tuple_ops::relation_modification_state *relation_modification_state = bioseq::storage::metadata::tuple_ops::begin_relation_modification(row_mask_relation);

	uint64_t chunk_iteration_start_row_number = stripe_start_row_number;
	uint64_t chunk_iteration_end_row_number = stripe_start_row_number -  1;

	int chunk_id = 0;

	bool chunk_inserted = true;

	for (int row_count :
		 bioseq::postgres::support::postgres_integer_list{chunk_group_row_counts})
	{
		if (!chunk_inserted)
			break;

		uint16_t row_mask_iterations =
			(row_count % row_mask_chunk_row_capacity) ?
				(row_count / row_mask_chunk_row_capacity + 1) :
				(row_count / row_mask_chunk_row_capacity);

		for(uint16_t n = 0; n < row_mask_iterations; n++)
		{
			uint16_t mask_size;

			/* Last iteration */
			if (n == (row_mask_iterations - 1))
			{
				uint16_t last_iteration_row_size = row_count % row_mask_chunk_row_capacity ?
						row_count % row_mask_chunk_row_capacity : row_mask_chunk_row_capacity;
				chunk_iteration_end_row_number += last_iteration_row_size;
				mask_size = (last_iteration_row_size) % 8 ?
								last_iteration_row_size / 8 + 1 :
								last_iteration_row_size / 8;
			}
			else
			{
				chunk_iteration_end_row_number += row_mask_chunk_row_capacity;
				mask_size = row_mask_chunk_row_capacity / 8;
			}

			bytea *initial_lookup_record =
				bioseq::postgres::support::allocate_zeroed_storage<bytea>(
					mask_size + VARHDRSZ);
			SET_VARSIZE(initial_lookup_record, mask_size + VARHDRSZ);

			int64_t next_seq_id = nextval_internal(bioseq_column_row_mask_seq, false);

			row_mask_codec::encoded_row encoded = row_mask_codec::encode(
				row_mask_row{
					.id = static_cast<std::int64_t>(next_seq_id),
					.storage_id = static_cast<std::int64_t>(storage_id),
					.stripe_id = static_cast<std::int64_t>(stripe_id),
					.chunk_id = static_cast<std::int32_t>(chunk_id),
					.start_row_number = static_cast<std::int64_t>(
						chunk_iteration_start_row_number),
					.end_row_number = static_cast<std::int64_t>(
						chunk_iteration_end_row_number),
					.deleted_rows = 0,
					.mask = initial_lookup_record,
				},
				CurrentMemoryContext);
			const row_mask_codec::encoded_tuple_data tuple_data = encoded.tuple_data(
				RelationGetDescr(row_mask_relation));

			/* A conflicting UNIQUE row means another writer owns this range. */
			PG_TRY();
			{
				bioseq::storage::metadata::tuple_ops::insert_catalog_tuple(
					relation_modification_state, tuple_data.values, tuple_data.nulls);
			}

			PG_CATCH();
			{
				FlushErrorState();
				chunk_inserted = false;
				break;
			}

			PG_END_TRY();

			chunk_iteration_start_row_number += row_mask_chunk_row_capacity;
		}

		chunk_iteration_start_row_number = chunk_iteration_end_row_number + 1;
		chunk_id++;
	}

	bioseq::storage::metadata::tuple_ops::finish_relation_modification(relation_modification_state);
	table_close(row_mask_relation, RowExclusiveLock);

	return chunk_inserted;
}


/*
 * read_stripe_skip_list fetches chunk metadata for a given stripe.
 */
export stripe_skip_list *
read_stripe_skip_list(RelFileLocator relfilelocator, uint64_t stripe, TupleDesc tuple_descriptor,
				   uint32_t chunk_count, Snapshot snapshot)
{
	int32_t column_index = 0;
	int32_t chunk_group_index = 0;
	int32_t chunk_group_row_offset_acc = 0;
	HeapTuple heap_tuple = nullptr;
	uint32_t column_count = tuple_descriptor->natts;
	ScanKeyData scan_keys[2];

	uint64_t storage_id = lookup_storage_id(relfilelocator);

	Oid bioseq_column_chunk_oid = column_chunk_relation_oid();
	Relation bioseq_column_chunk = table_open(bioseq_column_chunk_oid, AccessShareLock);
	Relation index = index_open(column_chunk_index_oid(), AccessShareLock);

	scan_keys[0] =
		chunk_codec::scan_key<&chunk_row::storage_id>(
			static_cast<std::int64_t>(storage_id));
	scan_keys[1] =
		chunk_codec::scan_key<&chunk_row::stripe_num>(
			static_cast<std::int64_t>(stripe));

	SysScanDesc scan_descriptor = systable_beginscan_ordered(bioseq_column_chunk, index,
															snapshot, 2, scan_keys);

	stripe_skip_list *chunk_list =
		bioseq::postgres::support::allocate_zeroed_object<stripe_skip_list>();
	chunk_list->chunk_count = chunk_count;
	chunk_list->column_count = column_count;
	chunk_list->chunk_skip_nodes =
		bioseq::postgres::support::allocate_zeroed_array<chunk_skip_node*>(
			column_count);
	for (column_index = 0; column_index < column_count; column_index++)
	{
		chunk_list->chunk_skip_nodes[column_index] =
			bioseq::postgres::support::allocate_zeroed_array<chunk_skip_node>(
				chunk_count);
	}

	while (HeapTupleIsValid(heap_tuple = systable_getnext_ordered(scan_descriptor,
																 ForwardScanDirection)))
	{
		const chunk_row row = chunk_codec::deform(
			heap_tuple,
			RelationGetDescr(bioseq_column_chunk),
			CurrentMemoryContext);
		const int32_t attr = row.attr_num;
		const int32_t chunk_index = row.chunk_group_num;

		if (attr <= 0 || attr > column_count)
		{
			bioseq_report<logging_level::error, logging_target::postgres>().with_message("invalid columnar chunk entry").with_detail("Attribute number out of range: {}", attr);
		}

		if (chunk_index < 0 || chunk_index >= chunk_count)
		{
			bioseq_report<logging_level::error, logging_target::postgres>().with_message("invalid columnar chunk entry").with_detail("Chunk number out of range: {}", chunk_index);
		}

		column_index = attr - 1;

		chunk_skip_node *chunk =
			&chunk_list->chunk_skip_nodes[column_index][chunk_index];
		chunk->row_count = row.value_count;
		chunk->value_stream_offset = row.value_stream_offset;
		chunk->value_stream_length = row.value_stream_length;
		chunk->exists_stream_offset = row.exists_stream_offset;
		chunk->exists_stream_length = row.exists_stream_length;
		chunk->value_compression_algorithm =
			static_cast<bioseq::compression_algorithm>(row.value_compression_type);
		chunk->value_compression_level = row.value_compression_level;
		chunk->value_decompressed_length = row.value_decompressed_length;

		if (!row.minimum_value.has_value()
			|| !row.maximum_value.has_value())
		{
			chunk->has_min_max = false;
		}
		else
		{
			bytea *min_value = *row.minimum_value;
			bytea *max_value = *row.maximum_value;

			chunk->minimum_value =
				bioseq::postgres::support::pg_datum::from_bytea(min_value, TupleDescAttr(tuple_descriptor, column_index)).raw();
			chunk->maximum_value =
				bioseq::postgres::support::pg_datum::from_bytea(max_value, TupleDescAttr(tuple_descriptor, column_index)).raw();

			chunk->has_min_max = true;
		}
	}

	systable_endscan_ordered(scan_descriptor);
	index_close(index, AccessShareLock);
	table_close(bioseq_column_chunk, AccessShareLock);

	read_chunk_group_row_counts(storage_id, stripe, chunk_count,
							&chunk_list->chunk_group_row_counts,
							&chunk_list->chunk_group_deleted_rows,
							snapshot);

	chunk_list->chunk_group_row_offsets =
		bioseq::postgres::support::allocate_zeroed_array<uint32_t>(chunk_count);

	for (chunk_group_index = 0; chunk_group_index < chunk_count; chunk_group_index++)
	{
		chunk_list->chunk_group_row_offsets[chunk_group_index] = chunk_group_row_offset_acc;
		chunk_group_row_offset_acc += chunk_list->chunk_group_row_counts[chunk_group_index];
	}

	return chunk_list;
}


/*
 * read_chunk_row_mask fetches chunk row mask for columnar relation.
 */
export bytea *
read_chunk_row_mask(RelFileLocator relfilelocator, Snapshot snapshot,
				 MemoryContext cxt,
				 uint64_t stripe_first_row_number, int row_count)
{
	if (snapshot == nullptr)
	{
		bioseq_log<logging_level::error, logging_target::postgres>(
			"read_chunk_row_mask requires the caller's registered snapshot");
	}
	HeapTuple heap_tuple = nullptr;
	ScanKeyData scan_keys[3];

	uint64_t storage_id = lookup_storage_id(relfilelocator);

	Oid row_mask_relation_oid = column_row_mask_relation_oid();
	Relation row_mask_relation = table_open(row_mask_relation_oid, AccessShareLock);
	Relation index = index_open(column_row_mask_index_oid(), AccessShareLock);

	bytea *chunk_row_mask_bytea = nullptr;
	SysScanDesc scan_descriptor = nullptr;
	{
		bioseq::postgres::support::memory_context_scope context_guard{cxt};

		Size chunk_mask_size =
			(row_count % row_mask_chunk_row_capacity) ?
				(row_count / 8 + 1) :
				(row_count / 8);

		chunk_row_mask_bytea =
			bioseq::postgres::support::allocate_zeroed_storage<bytea>(
				chunk_mask_size + VARHDRSZ);
		SET_VARSIZE(chunk_row_mask_bytea, chunk_mask_size + VARHDRSZ);

		scan_keys[0] =
			row_mask_codec::scan_key<&row_mask_row::storage_id>(
				static_cast<std::int64_t>(storage_id));
		scan_keys[1] =
			row_mask_codec::scan_key<
				&row_mask_row::start_row_number,
				bioseq::postgres::support::scan_comparison::greater_equal>(
					static_cast<std::int64_t>(stripe_first_row_number));
		scan_keys[2] =
			row_mask_codec::scan_key<
				&row_mask_row::end_row_number,
				bioseq::postgres::support::scan_comparison::less_equal>(
					static_cast<std::int64_t>(
						stripe_first_row_number + row_count - 1));

			scan_descriptor = systable_beginscan_ordered(row_mask_relation,
												index,
												snapshot, 3, scan_keys);

		Size pos = 0;
		while (HeapTupleIsValid(heap_tuple = systable_getnext_ordered(scan_descriptor,
																	 ForwardScanDirection)))
		{
			const row_mask_row row = row_mask_codec::deform(
				heap_tuple,
				RelationGetDescr(row_mask_relation),
				cxt);
			if (!row.mask.has_value() || *row.mask == nullptr)
			{
				throw bioseq::postgres::support::row_codec_error{
					"row mask catalog entry has a NULL mask"};
			}
			bytea *current_row_mask = *row.mask;

			if (row.start_row_number < 0
				|| row.end_row_number < row.start_row_number)
			{
				throw bioseq::postgres::support::row_codec_error{
					"bioseq_internal.row_mask has an invalid row range"};
			}

			const std::uint64_t logical_rows =
				static_cast<std::uint64_t>(row.end_row_number)
				- static_cast<std::uint64_t>(row.start_row_number) + 1;
			const std::uint64_t logical_bytes =
				logical_rows / 8 + (logical_rows % 8 != 0 ? 1 : 0);
			const Size remaining = chunk_mask_size - pos;
			const Size bounded_logical_bytes = static_cast<Size>(
				std::min<std::uint64_t>(logical_bytes, remaining));
			const Size copy_bytes = std::min(
				static_cast<Size>(VARSIZE_ANY_EXHDR(current_row_mask)),
				bounded_logical_bytes);

			if (copy_bytes > 0)
			{
				memcpy(VARDATA(chunk_row_mask_bytea) + pos,
					   VARDATA(current_row_mask),
					   copy_bytes);
			}

			pos += bounded_logical_bytes;
			if (pos == chunk_mask_size)
			{
				break;
			}
		}
	}

	systable_endscan_ordered(scan_descriptor);
	index_close(index, AccessShareLock);
	table_close(row_mask_relation, AccessShareLock);

	return chunk_row_mask_bytea;
}


export void flush_row_mask_cache(
	bioseq::storage::metadata::catalog_rows::row_mask *row_mask_entry)
{
	HeapTuple old_heap_tuple = nullptr;

	ScanKeyData scan_keys;

	Oid row_mask_relation_oid = column_row_mask_relation_oid();
	Relation row_mask_relation =
		table_open(row_mask_relation_oid, AccessShareLock);
	TupleDesc tuple_descriptor = RelationGetDescr(row_mask_relation);

	Relation index = index_open(column_row_mask_index_oid(), AccessShareLock);

	scan_keys =
		row_mask_codec::scan_key<&row_mask_row::id>(row_mask_entry->id);

	SysScanDesc scan_descriptor = systable_beginscan_ordered(row_mask_relation,
															index,
														nullptr, 1, &scan_keys);

	old_heap_tuple = systable_getnext_ordered(scan_descriptor, BackwardScanDirection);

	index_close(index, AccessShareLock);

	if (HeapTupleIsValid(old_heap_tuple))
	{
		row_mask_codec::update update{};
		update
			.set<&row_mask_row::deleted_rows>(
				row_mask_entry->deleted_rows)
			.set<&row_mask_row::mask>(
				row_mask_entry->mask, CurrentMemoryContext);

		HeapTuple new_heap_tuple = update.modify(
			old_heap_tuple, tuple_descriptor);

		CatalogTupleUpdate(row_mask_relation, &old_heap_tuple->t_self, new_heap_tuple);

		heap_freetuple(new_heap_tuple);
	}

	systable_endscan_ordered(scan_descriptor);
	table_close(row_mask_relation, AccessShareLock);

	CommandCounterIncrement();
}


/*
 * find_stripe_by_row_number returns stripe_metadata for the stripe whose
 * first_row_number is greater than given row_number. If no such stripe
 * exists, then returns null.
 */
export stripe_metadata *
find_next_stripe_by_row_number(Relation relation, uint64_t row_number, Snapshot snapshot)
{
	return find_stripe_metadata_by_row_number(relation, row_number, snapshot, row_number_lookup_mode::greater);
}


/*
 * find_stripe_by_row_number returns stripe_metadata for the stripe that contains
 * the row with row_number. If no such stripe exists, then returns null.
 */
export stripe_metadata *
find_stripe_by_row_number(Relation relation, uint64_t row_number, Snapshot snapshot)
{
	stripe_metadata *stripe =
		find_stripe_by_first_row_number(relation, row_number, snapshot);
	if (!stripe)
	{
		return nullptr;
	}

	if (row_number > stripe_highest_row_number(stripe))
	{
		return nullptr;
	}

	return stripe;
}


/*
 * find_stripe_by_first_row_number returns a stripe_metadata object for
 * the stripe that has the greatest first_row_number among the stripes whose
 * first_row_number is smaller than or equal to given row_number. If no such
 * stripe exists, then returns null.
 *
 * Note that this doesn't mean that found stripe certainly contains the tuple
 * with given row_number. This is because, it also needs to be verified if
 * highest row number that found stripe contains is greater than or equal to
 * given row_number. For this reason, unless that additional check is done,
 * this function is mostly useful for checking against "possible" constraint
 * violations due to concurrent writes that are not flushed by other backends
 * yet.
 */
stripe_metadata *
find_stripe_by_first_row_number(Relation relation, uint64_t row_number,
									 Snapshot snapshot)
{
	return find_stripe_metadata_by_row_number(relation, row_number, snapshot,
										 row_number_lookup_mode::less_or_equal);
}

/* find_next_stripe_for_parallel_worker returns next stripe that should be assigned
 * for worker. Approach here is to calculate module of stripe id with total number
 * of workers that are running for execution.
 */

export stripe_metadata *
find_next_stripe_for_parallel_worker(Relation relation,
								Snapshot snapshot,
								uint64_t next_stripe_id,
								uint64_t * next_higher_stripe_id)
{
	stripe_metadata *found_stripe_metadata = nullptr;

	uint64_t storage_id = column_storage_id(relation, false);
	ScanKeyData scan_keys[2];

	scan_keys[0] =
		stripe_codec::scan_key<&stripe_row::storage_id>(
			static_cast<std::int64_t>(storage_id));
	scan_keys[1] =
		stripe_codec::scan_key<
			&stripe_row::stripe_num,
			bioseq::postgres::support::scan_comparison::greater_equal>(
				static_cast<std::int64_t>(next_stripe_id));

	Relation bioseq_column_stripes = table_open(column_stripe_relation_oid(), AccessShareLock);

	Relation index = index_open(column_stripe_primary_key_index_oid(),
								AccessShareLock);

	SysScanDesc scan_descriptor = systable_beginscan_ordered(bioseq_column_stripes, index,
															snapshot, 2,
															scan_keys);

	HeapTuple heap_tuple = systable_getnext_ordered(scan_descriptor, ForwardScanDirection);
	if (HeapTupleIsValid(heap_tuple))
	{
		found_stripe_metadata = build_stripe_metadata(bioseq_column_stripes, heap_tuple);
		*next_higher_stripe_id = found_stripe_metadata->id;
	}

	systable_endscan_ordered(scan_descriptor);
	index_close(index, AccessShareLock);
	table_close(bioseq_column_stripes, AccessShareLock);

	return found_stripe_metadata;
}


/*
 * stripe_write_state_for returns write state of given stripe.
 */
export stripe_write_state
stripe_write_state_for(stripe_metadata *stripe)
{
	if (stripe->aborted)
	{
		return stripe_write_state::aborted;
	}
	else if (stripe->row_count > 0)
	{
		return stripe_write_state::flushed;
	}
	else
	{
		return stripe_write_state::in_progress;
	}
}


/*
 * stripe_highest_row_number returns row_number of the row with highest
 * row_number in given stripe.
 */
uint64_t
stripe_highest_row_number(stripe_metadata *stripe)
{
	return stripe->first_row_number + stripe->row_count - 1;
}


/*
 * find_stripe_metadata_by_row_number returns stripe_metadata for the stripe whose
 * first_row_number is less than or equal to (row_number_lookup_mode::less_or_equal), or is
 * greater than (row_number_lookup_mode::greater) given row_number by doing backward index
 * scan on stripe_first_row_number_idx.
 * If no such stripe exists, then returns null.
 */
static stripe_metadata *
find_stripe_metadata_by_row_number(Relation relation, uint64_t row_number, Snapshot snapshot,
							  row_number_lookup_mode lookup_mode)
{
	Assert(lookup_mode == row_number_lookup_mode::less_or_equal || lookup_mode == row_number_lookup_mode::greater);

	stripe_metadata *found_stripe_metadata = nullptr;

	uint64_t storage_id = column_storage_id(relation, false);
	ScanKeyData scan_keys[2];
	scan_keys[0] =
		stripe_codec::scan_key<&stripe_row::storage_id>(
			static_cast<std::int64_t>(storage_id));

	if (lookup_mode == row_number_lookup_mode::less_or_equal)
	{
		scan_keys[1] =
			stripe_codec::scan_key<
				&stripe_row::first_row_number,
				bioseq::postgres::support::scan_comparison::less_equal>(
					static_cast<std::int64_t>(row_number));
	}
	else if (lookup_mode == row_number_lookup_mode::greater)
	{
		scan_keys[1] =
			stripe_codec::scan_key<
				&stripe_row::first_row_number,
				bioseq::postgres::support::scan_comparison::greater>(
					static_cast<std::int64_t>(row_number));
	}


	Relation bioseq_column_stripes = table_open(column_stripe_relation_oid(), AccessShareLock);
	Relation index = index_open(stripe_first_row_number_index_oid(),
								AccessShareLock);
	SysScanDesc scan_descriptor = systable_beginscan_ordered(bioseq_column_stripes, index,
															snapshot, 2,
															scan_keys);

	ScanDirection scan_direction = NoMovementScanDirection;
	if (lookup_mode == row_number_lookup_mode::less_or_equal)
	{
		scan_direction = BackwardScanDirection;
	}
	else if (lookup_mode == row_number_lookup_mode::greater)
	{
		scan_direction = ForwardScanDirection;
	}
	HeapTuple heap_tuple = systable_getnext_ordered(scan_descriptor, scan_direction);
	if (HeapTupleIsValid(heap_tuple))
	{
		found_stripe_metadata = build_stripe_metadata(bioseq_column_stripes, heap_tuple);
	}

	systable_endscan_ordered(scan_descriptor);
	index_close(index, AccessShareLock);
	table_close(bioseq_column_stripes, AccessShareLock);

	return found_stripe_metadata;
}


/// @brief Rejects catalog fields that contradict a stripe's write state.
static void
validate_stripe_metadata(stripe_metadata *stripe)
{
	bool stripe_looks_in_progress =
		stripe->row_count == 0 && stripe->chunk_count == 0 &&
		stripe->file_offset == invalid_logical_offset &&
		stripe->data_length == 0;

	/*
	 * Even if stripe is flushed, file_offset and data_length might be equal
	 * to 0 for zero column tables, but those two should still be consistent
	 * with respect to each other.
	 */
	bool stripe_looks_flushed =
		stripe->row_count > 0 && stripe->chunk_count > 0 &&
		((stripe->file_offset != invalid_logical_offset &&
		  stripe->data_length > 0) ||
		 (stripe->file_offset == invalid_logical_offset &&
		  stripe->data_length == 0));

	const stripe_write_state write_state = stripe_write_state_for(stripe);
	if (write_state == stripe_write_state::flushed && stripe_looks_flushed)
	{
		/*
		 * If stripe was flushed to disk, then we expect stripe to store
		 * at least one tuple.
		 */
		return;
	}
	else if (write_state == stripe_write_state::in_progress && stripe_looks_in_progress)
	{
		/*
		 * If stripe was not flushed to disk, then values of given four
		 * fields should match the columns inserted by
		 * insert_empty_stripe_metadata_row.
		 */
		return;
	}
	else if (write_state == stripe_write_state::aborted && (stripe_looks_in_progress ||
														  stripe_looks_flushed))
	{
		/*
		 * Stripe metadata entry for an aborted write can be complete or
		 * incomplete. We might have aborted the transaction before or after
		 * inserting into stripe metadata.
		 */
		return;
	}

	bioseq_log<logging_level::error, logging_target::postgres>( "unexpected stripe state, stripe metadata entry for stripe with id={} is not consistent", stripe->id);
}


/*
 * find_highest_row_number_stripe returns stripe_metadata for the stripe that
 * has the row with highest row_number by doing backward index scan on
 * stripe_first_row_number_idx. If given relation is empty, then returns null.
 */
export stripe_metadata *
find_highest_row_number_stripe(Relation relation, Snapshot snapshot)
{
	stripe_metadata *stripe_with_highest_row_number = nullptr;

	uint64_t storage_id = column_storage_id(relation, false);
	ScanKeyData scan_keys[1];
	scan_keys[0] =
		stripe_codec::scan_key<&stripe_row::storage_id>(
			static_cast<std::int64_t>(storage_id));

	Relation bioseq_column_stripes = table_open(column_stripe_relation_oid(), AccessShareLock);
	Relation index = index_open(stripe_first_row_number_index_oid(),
								AccessShareLock);
	SysScanDesc scan_descriptor = systable_beginscan_ordered(bioseq_column_stripes, index,
															snapshot, 1, scan_keys);

	HeapTuple heap_tuple = systable_getnext_ordered(scan_descriptor, BackwardScanDirection);
	if (HeapTupleIsValid(heap_tuple))
	{
		stripe_with_highest_row_number = build_stripe_metadata(bioseq_column_stripes, heap_tuple);
	}

	systable_endscan_ordered(scan_descriptor);
	index_close(index, AccessShareLock);
	table_close(bioseq_column_stripes, AccessShareLock);

	return stripe_with_highest_row_number;
}


/*
 * read_chunk_group_row_counts returns an array of row counts of chunk groups and
 * deleted rows count for each chunk group for given stripe.
 * Arrays that are updated will be allocated in this function.
 */
static void
read_chunk_group_row_counts(uint64_t storage_id, uint64_t stripe, uint32_t chunk_group_count,
						uint32_t **chunk_group_row_counts, uint32_t **chunk_group_deleted_rows,
						Snapshot snapshot)
{
	Oid bioseq_column_chunk_group_oid = column_chunk_group_relation_oid();
	Relation bioseq_column_chunk_group = table_open(bioseq_column_chunk_group_oid, AccessShareLock);
	Relation index = index_open(column_chunk_group_index_oid(), AccessShareLock);

	std::array<ScanKeyData, 2> scan_keys{
		chunk_group_codec::scan_key<&chunk_group_row::storage_id>(
			static_cast<std::int64_t>(storage_id)),
		chunk_group_codec::scan_key<&chunk_group_row::stripe_num>(
			static_cast<std::int64_t>(stripe)),
	};

	SysScanDesc scan_descriptor =
		systable_beginscan_ordered(
			bioseq_column_chunk_group,
			index,
			snapshot,
			static_cast<int>(scan_keys.size()),
			scan_keys.data());

	HeapTuple heap_tuple = nullptr;

	*chunk_group_row_counts =
		bioseq::postgres::support::allocate_zeroed_array<uint32_t>(
			chunk_group_count);
	*chunk_group_deleted_rows =
		bioseq::postgres::support::allocate_zeroed_array<uint32_t>(
			chunk_group_count);

	/*
	 * The caller's snapshot selects the one visible version of each chunk-group
	 * row.  A tuple that is HOT-updated can still be exactly the version visible
	 * to an older REPEATABLE READ snapshot; filtering it by the physical
	 * HEAP_HOT_UPDATED flag would incorrectly hide pre-update source rows.
	 */
	while (HeapTupleIsValid(heap_tuple = systable_getnext_ordered(scan_descriptor,
																 ForwardScanDirection)))
	{
		const chunk_group_row row = chunk_group_codec::deform(
			heap_tuple,
			RelationGetDescr(bioseq_column_chunk_group),
			CurrentMemoryContext);
		const uint32_t tuple_chunk_group_index =
			static_cast<uint32_t>(row.chunk_group_num);

		if (tuple_chunk_group_index >= chunk_group_count)
		{
			bioseq_log<logging_level::warning, logging_target::postgres>( "Tuple chunk group higher than chunk group count: {}, {} (storage_id = {}, stripe_id = {})", tuple_chunk_group_index, chunk_group_count, storage_id, stripe);
			continue;
		}

		(*chunk_group_row_counts)[tuple_chunk_group_index] =
			static_cast<uint32_t>(row.row_count);

		(*chunk_group_deleted_rows)[tuple_chunk_group_index] =
			static_cast<uint32_t>(row.deleted_rows);
	}

	systable_endscan_ordered(scan_descriptor);
	index_close(index, AccessShareLock);
	table_close(bioseq_column_chunk_group, AccessShareLock);
}


/*
 * update_chunk_group_deleted_rows updates `deleted_rows` column for each chunk group.
 */
export void
update_chunk_group_deleted_rows(uint64_t storage_id, uint64_t stripe,
							uint32_t chunk_group_id, uint32_t deleted_row_number)
{

	HeapTuple old_heap_tuple = nullptr;

	Oid bioseq_column_chunk_group_oid = column_chunk_group_relation_oid();
	Relation bioseq_column_chunk_group = table_open(bioseq_column_chunk_group_oid, AccessShareLock);
	TupleDesc tuple_descriptor = RelationGetDescr(bioseq_column_chunk_group);
	Relation index = index_open(column_chunk_group_index_oid(), AccessShareLock);

	std::array<ScanKeyData, 3> scan_keys{
		chunk_group_codec::scan_key<&chunk_group_row::storage_id>(
			static_cast<std::int64_t>(storage_id)),
		chunk_group_codec::scan_key<&chunk_group_row::stripe_num>(
			static_cast<std::int64_t>(stripe)),
		chunk_group_codec::scan_key<&chunk_group_row::chunk_group_num>(
			static_cast<std::int32_t>(chunk_group_id)),
	};

	SysScanDesc scan_descriptor =
		systable_beginscan_ordered(
			bioseq_column_chunk_group,
			index,
			nullptr,
			static_cast<int>(scan_keys.size()),
			scan_keys.data());

	old_heap_tuple = systable_getnext_ordered(scan_descriptor, BackwardScanDirection);

	index_close(index, AccessShareLock);

	if (HeapTupleIsValid(old_heap_tuple))
	{
		chunk_group_codec::update update{};
		update.set<&chunk_group_row::deleted_rows>(
			static_cast<std::int64_t>(deleted_row_number));

		HeapTuple new_heap_tuple =
			update.modify(old_heap_tuple, tuple_descriptor);

		CatalogTupleUpdate(bioseq_column_chunk_group, &old_heap_tuple->t_self, new_heap_tuple);

		heap_freetuple(new_heap_tuple);
	}

	systable_endscan_ordered(scan_descriptor);
	table_close(bioseq_column_chunk_group, AccessShareLock);

	CommandCounterIncrement();
}


/*
 * insert_empty_stripe_metadata_row adds a row to bioseq.stripe for the empty
 * stripe reservation made for stripe_id.
 */
static void
insert_empty_stripe_metadata_row(uint64_t storage_id, uint64_t stripe_id, uint32_t column_count,
							 uint32_t chunk_group_row_count, uint64_t first_row_number)
{
	stripe_codec::encoded_row encoded = stripe_codec::encode(stripe_row{
		.storage_id = static_cast<std::int64_t>(storage_id),
		.stripe_num = static_cast<std::int64_t>(stripe_id),
		.file_offset = static_cast<std::int64_t>(
			invalid_logical_offset),
		.data_length = 0,
		.column_count = static_cast<std::int32_t>(column_count),
		.chunk_row_count =
			static_cast<std::int32_t>(chunk_group_row_count),
		.row_count = 0,
		.chunk_group_count = 0,
		.first_row_number = static_cast<std::int64_t>(first_row_number),
	});

	Oid bioseq_column_stripes_oid = column_stripe_relation_oid();
	Relation bioseq_column_stripes = table_open(bioseq_column_stripes_oid, RowExclusiveLock);

	bioseq::storage::metadata::tuple_ops::relation_modification_state *relation_modification_state = bioseq::storage::metadata::tuple_ops::begin_relation_modification(bioseq_column_stripes);
	const stripe_codec::encoded_tuple_data tuple_data = encoded.tuple_data(
		RelationGetDescr(bioseq_column_stripes));

	bioseq::storage::metadata::tuple_ops::insert_catalog_tuple(
		relation_modification_state, tuple_data.values, tuple_data.nulls);

	bioseq::storage::metadata::tuple_ops::finish_relation_modification(relation_modification_state);

	table_close(bioseq_column_stripes, RowExclusiveLock);
}


/*
 * list_stripes_for_relation returns a list of stripe_metadata for stripes
 * of the given relfilelocator.
 */
export List *
list_stripes_for_relation(RelFileLocator relfilelocator, ScanDirection scan_direction)
{
	uint64_t storage_id = lookup_storage_id(relfilelocator);

	return read_data_file_stripes(storage_id, GetTransactionSnapshot(), scan_direction);
}


/// @brief Returns the exact row count represented by a relation's stripes.
export uint64_t
column_table_row_count(Relation relation)
{
	uint64_t total_row_count = 0;
	List *stripe_list = list_stripes_for_relation(relation->rd_locator, ForwardScanDirection);

	for (stripe_metadata* stripe :
		 bioseq::postgres::support::postgres_list<stripe_metadata>{stripe_list})
	{
		total_row_count += stripe->row_count;
	}

	return total_row_count;
}


/*
 * deleted_rows_for_stripe returns number of deleted rows for stripe
 * of the given relfilelocator.
 */
export uint32_t
deleted_rows_for_stripe(RelFileLocator relfilelocator, uint32_t chunk_count, uint64_t stripe_id)
{
	uint64_t storage_id = lookup_storage_id(relfilelocator);

	uint32_t *chunk_group_row_counts;
	uint32_t *chunk_group_deleted_rows;
	int i;

	uint32_t deleted_rows = 0;

	read_chunk_group_row_counts(storage_id, stripe_id, chunk_count,
							&chunk_group_row_counts, &chunk_group_deleted_rows,
							GetTransactionSnapshot());

	for (i = 0; i < chunk_count; i++)
	{
		deleted_rows += chunk_group_deleted_rows[i];
	}

	pfree(chunk_group_row_counts);
	pfree(chunk_group_deleted_rows);

	return deleted_rows;
}

/*
 * decompressed_length_for_stripe returns total size of all decompressed rows and chunk
 * for given stripe
 */
export Size
decompressed_length_for_stripe(RelFileLocator relfilelocator, uint64_t stripe_id)
{
	HeapTuple heap_tuple = nullptr;
	ScanKeyData scan_keys[2];

	uint64_t storage_id = lookup_storage_id(relfilelocator);

	Oid bioseq_column_chunk_oid = column_chunk_relation_oid();
	Relation bioseq_column_chunk = table_open(bioseq_column_chunk_oid, AccessShareLock);
	Relation index = index_open(column_chunk_index_oid(), AccessShareLock);

	scan_keys[0] =
		chunk_codec::scan_key<&chunk_row::storage_id>(
			static_cast<std::int64_t>(storage_id));
	scan_keys[1] =
		chunk_codec::scan_key<&chunk_row::stripe_num>(
			static_cast<std::int64_t>(stripe_id));

	SysScanDesc scan_descriptor = systable_beginscan_ordered(bioseq_column_chunk, index,
															GetTransactionSnapshot(), 2, scan_keys);

	Size decompressed_chunk_size = 0;

	while (HeapTupleIsValid(heap_tuple = systable_getnext_ordered(scan_descriptor,
																 ForwardScanDirection)))
	{
		const chunk_row row = chunk_codec::deform(
			heap_tuple,
			RelationGetDescr(bioseq_column_chunk),
			CurrentMemoryContext);
		decompressed_chunk_size += row.value_decompressed_length;
	}

	systable_endscan_ordered(scan_descriptor);
	index_close(index, AccessShareLock);
	table_close(bioseq_column_chunk, AccessShareLock);

	return decompressed_chunk_size;
}


/*
 * find_highest_used_address returns the highest used address for the given
 * relfilelocator across all active and inactive transactions.
 *
 * This is used by truncate stage of VACUUM, and VACUUM can be called
 * for empty tables. So this doesn't throw errors for empty tables and
 * returns 0.
 */
export uint64_t
find_highest_used_address(RelFileLocator relfilelocator)
{
	uint64_t storage_id = lookup_storage_id(relfilelocator);

	uint64_t highest_used_address = 0;
	uint64_t highest_used_id = 0;
	find_highest_used_address_and_id(storage_id, &highest_used_address, &highest_used_id);

	return highest_used_address;
}


/*
 * find_highest_used_address_and_id returns the highest used address and id for
 * the given relfilelocator across all active and inactive transactions.
 */
static void
find_highest_used_address_and_id(uint64_t storage_id,
						   uint64_t *highest_used_address,
						   uint64_t *highest_used_id)
{
	SnapshotData snapshot_dirty;
	InitDirtySnapshot(snapshot_dirty);

	List *stripe_metadata_list = read_data_file_stripes(storage_id, &snapshot_dirty,
													  ForwardScanDirection);

	*highest_used_id = 0;

	/* file starts with metapage */
	*highest_used_address = usable_column_bytes_per_page;

	for (stripe_metadata* stripe :
		 bioseq::postgres::support::postgres_list<stripe_metadata>{
			 stripe_metadata_list})
	{
		uint64_t last_byte = stripe->file_offset + stripe->data_length - 1;
		*highest_used_address = std::max(*highest_used_address, last_byte);
		*highest_used_id = std::max(*highest_used_id, static_cast<uint64_t>(stripe->id));
	}
}


/*
 * reserve_empty_stripe reserves an empty stripe for given relation
 * and inserts it into bioseq.stripe. It is guaranteed that concurrent
 * writes won't overwrite the returned stripe.
 */
export empty_stripe_reservation *
reserve_empty_stripe(Relation rel, uint64_t column_count, uint64_t chunk_group_row_count,
				   uint64_t stripe_row_limit)
{
	empty_stripe_reservation *stripe_reservation =
		bioseq::postgres::support::allocate_zeroed_object<
			empty_stripe_reservation>();

	uint64_t storage_id = column_storage_id(rel, false);

	stripe_reservation->stripe_id = reserve_column_stripe_id(rel);
	stripe_reservation->stripe_first_row_number =
		reserve_column_row_numbers(rel, stripe_row_limit);

	/* The catalog reservation is the concurrency-visible owner record and is
	 * completed in place when buffered stripe data is flushed. */
	insert_empty_stripe_metadata_row(storage_id, stripe_reservation->stripe_id,
								 column_count, chunk_group_row_count,
								 stripe_reservation->stripe_first_row_number);

	return stripe_reservation;
}


/*
 * complete_stripe_reservation completes reservation of the stripe with
 * stripe_id for given size and in-place updates related stripe metadata tuple
 * to complete reservation.
 */
export stripe_metadata *
complete_stripe_reservation(Relation rel, uint64_t stripe_id, uint64_t size_bytes,
						  uint64_t row_count, uint64_t chunk_count)
{
	uint64_t res_logical_start = reserve_column_storage_bytes(rel, size_bytes);
	uint64_t storage_id = column_storage_id(rel, false);

	stripe_codec::update update{};
	update
		.set<&stripe_row::file_offset>(
			static_cast<std::int64_t>(res_logical_start))
		.set<&stripe_row::data_length>(
			static_cast<std::int64_t>(size_bytes))
		.set<&stripe_row::row_count>(
			static_cast<std::int64_t>(row_count))
		.set<&stripe_row::chunk_group_count>(
			static_cast<std::int32_t>(chunk_count));

	return update_stripe_metadata_row(storage_id, stripe_id, update);
}


/** @brief Updates non-indexed fields of one stripe catalog row in place.
 *
 * The caller must not use this path for fields covered by a catalog index.
 */
static stripe_metadata *
update_stripe_metadata_row(uint64_t storage_id, uint64_t stripe_id,
						stripe_codec::update &update)
{
	ScanKeyData scan_keys[2];
	scan_keys[0] =
		stripe_codec::scan_key<&stripe_row::storage_id>(
			static_cast<std::int64_t>(storage_id));
	scan_keys[1] =
		stripe_codec::scan_key<&stripe_row::stripe_num>(
			static_cast<std::int64_t>(stripe_id));

	Oid bioseq_column_stripes_oid = column_stripe_relation_oid();
	Relation bioseq_column_stripes = table_open(bioseq_column_stripes_oid, AccessShareLock);
	TupleDesc tuple_descriptor = RelationGetDescr(bioseq_column_stripes);

	Oid index_id = column_stripe_primary_key_index_oid();
	bool index_ok = OidIsValid(index_id);

	void *state = nullptr;
	HeapTuple old_tuple = nullptr;
	systable_inplace_update_begin(bioseq_column_stripes, index_id, index_ok,
								  /* snapshot */ nullptr,
								  2, scan_keys, &old_tuple, &state);

	if (!HeapTupleIsValid(old_tuple))
	{
		systable_inplace_update_cancel(state);
		table_close(bioseq_column_stripes, AccessShareLock);
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{
				.message = fmt::format(
					"attempted to modify an unexpected stripe, columnar "
					"storage with id={} does not have stripe with id={}",
					storage_id,
					stripe_id),
			});
	}

	HeapTuple modified_tuple = update.modify(old_tuple, tuple_descriptor);

	systable_inplace_update_finish(state, modified_tuple);

	/*
	 * build_stripe_metadata expects a real heap tuple with MVCC fields, which
	 * is what old_tuple is — the in-place update preserved its header.
	 */
	stripe_metadata *modified_stripe_metadata = build_stripe_metadata(bioseq_column_stripes,
																 modified_tuple);

	CommandCounterIncrement();

	heap_freetuple(modified_tuple);
	table_close(bioseq_column_stripes, AccessShareLock);

	/* return stripe_metadata object built from modified tuple */
	return modified_stripe_metadata;
}


/*
 * read_data_file_stripes reads the stripe list for a given storage_id
 * in the given snapshot.
 */
static List *
read_data_file_stripes(uint64_t storage_id, Snapshot snapshot, ScanDirection scan_direction)
{
	bioseq::postgres::support::postgres_list<stripe_metadata> stripe_metadata_list;
	ScanKeyData scan_keys[1];
	HeapTuple heap_tuple;

	scan_keys[0] =
		stripe_codec::scan_key<&stripe_row::storage_id>(
			static_cast<std::int64_t>(storage_id));

	Oid bioseq_column_stripes_oid = column_stripe_relation_oid();

	Relation bioseq_column_stripes = table_open(bioseq_column_stripes_oid, AccessShareLock);
	Relation index = index_open(stripe_first_row_number_index_oid(),
								AccessShareLock);

	SysScanDesc scan_descriptor = systable_beginscan_ordered(bioseq_column_stripes, index,
															snapshot, 1,
															scan_keys);

	while (HeapTupleIsValid(heap_tuple = systable_getnext_ordered(scan_descriptor,
																 scan_direction)))
	{
		stripe_metadata *stripe = build_stripe_metadata(bioseq_column_stripes, heap_tuple);
		stripe_metadata_list.append(stripe);
	}

	systable_endscan_ordered(scan_descriptor);
	index_close(index, AccessShareLock);
	table_close(bioseq_column_stripes, AccessShareLock);

	return stripe_metadata_list.release();
}


/*
 * build_stripe_metadata builds a stripe_metadata object from given heap tuple.
 *
 * NB: heap_tuple must be a proper heap tuple with MVCC fields.
 */
stripe_metadata *
build_stripe_metadata(Relation bioseq_column_stripes, HeapTuple heap_tuple)
{
	Assert(RelationGetRelid(bioseq_column_stripes) == column_stripe_relation_oid());

	const stripe_row row = stripe_codec::deform(
		heap_tuple,
		RelationGetDescr(bioseq_column_stripes),
		CurrentMemoryContext);

	stripe_metadata *stripe =
		bioseq::postgres::support::allocate_zeroed_object<stripe_metadata>();
	stripe->id = row.stripe_num;
	stripe->file_offset = row.file_offset;
	stripe->data_length = row.data_length;
	stripe->column_count = row.column_count;
	stripe->chunk_count = row.chunk_group_count;
	stripe->chunk_group_row_count = row.chunk_row_count;
	stripe->row_count = row.row_count;
	stripe->first_row_number =
		static_cast<std::uint64_t>(row.first_row_number);

	/*
	 * If there is unflushed data in a parent transaction, then we would
	 * have already thrown an error before starting to scan the table.. If
	 * the data is from an earlier subxact that committed, then it would
	 * have been flushed already. For this reason, we don't care about
	 * subtransaction id here.
	 */
	TransactionId entry_xmin = HeapTupleHeaderGetXmin(heap_tuple->t_data);
	stripe->aborted = !TransactionIdIsInProgress(entry_xmin) &&
								  TransactionIdDidAbort(entry_xmin);
	stripe->inserted_by_current_transaction =
		TransactionIdIsCurrentTransactionId(entry_xmin);

	validate_stripe_metadata(stripe);

	return stripe;
}


/*
 * delete_metadata_rows removes the rows with given relfilelocator from columnar
 * metadata tables.
 */
export void
delete_metadata_rows(RelFileLocator relfilelocator)
{
	/* Binary-upgrade restore order does not guarantee catalog availability. */
	if (IsBinaryUpgrade)
	{
		return;
	}

	uint64_t storage_id = lookup_storage_id(relfilelocator);

	delete_storage_metadata_rows(column_stripe_relation_oid(),
										   stripe_codec::attribute_number<
											   &stripe_row::storage_id>(),
										   column_stripe_primary_key_index_oid(),
										   storage_id);
	delete_storage_metadata_rows(column_chunk_group_relation_oid(),
										   chunk_group_codec::attribute_number<
											   &chunk_group_row::storage_id>(),
										   column_chunk_group_index_oid(),
										   storage_id);
	delete_storage_metadata_rows(column_chunk_relation_oid(),
										   chunk_codec::attribute_number<
											   &chunk_row::storage_id>(),
										   column_chunk_index_oid(),
										   storage_id);
	delete_storage_metadata_rows(column_row_mask_relation_oid(),
										   row_mask_codec::attribute_number<
											   &row_mask_row::storage_id>(),
										   column_row_mask_index_oid(),
										   storage_id);
}


/*
 * delete_metadata_rows_for_stripe removes the rows with given relfilelocator and
 * stripe id from columnar metadata tables.
 */
export void
delete_metadata_rows_for_stripe(RelFileLocator relfilelocator, uint64_t stripe_id)
{
	/* Binary-upgrade restore order does not guarantee catalog availability. */
	if (IsBinaryUpgrade)
	{
		return;
	}

	uint64_t storage_id = lookup_storage_id(relfilelocator);

	delete_stripe_metadata_rows(
		column_stripe_relation_oid(),
		stripe_codec::attribute_number<&stripe_row::storage_id>(),
		stripe_codec::attribute_number<&stripe_row::stripe_num>(),
		column_stripe_primary_key_index_oid(),
		storage_id, stripe_id);
	delete_stripe_metadata_rows(
		column_chunk_group_relation_oid(),
		chunk_group_codec::attribute_number<
			&chunk_group_row::storage_id>(),
		chunk_group_codec::attribute_number<
			&chunk_group_row::stripe_num>(),
		column_chunk_group_index_oid(),
		storage_id, stripe_id);
	delete_stripe_metadata_rows(
		column_chunk_relation_oid(),
		chunk_codec::attribute_number<&chunk_row::storage_id>(),
		chunk_codec::attribute_number<&chunk_row::stripe_num>(),
		column_chunk_index_oid(),
		storage_id, stripe_id);
	delete_stripe_metadata_rows(
		column_row_mask_relation_oid(),
		row_mask_codec::attribute_number<&row_mask_row::storage_id>(),
		row_mask_codec::attribute_number<&row_mask_row::stripe_id>(),
		column_row_mask_stripe_index_oid(),
		storage_id, stripe_id);
}


/*
 * delete_storage_metadata_rows removes the rows with given
 * storage_id from given columnar metadata table.
 */
static void
delete_storage_metadata_rows(Oid metadata_table_id,
									   AttrNumber storage_id_attribute_number,
									   Oid storage_id_index_id, uint64_t storage_id)
{
	ScanKeyData scan_keys[1];
	ScanKeyInit(&scan_keys[0], storage_id_attribute_number, BTEqualStrategyNumber,
				F_INT8EQ, UInt64GetDatum(storage_id));

	Relation metadata_table =
		bioseq::postgres::support::try_open_postgres_relation(
			metadata_table_id, AccessShareLock);
	if (metadata_table == nullptr)
	{
		/* extension has been dropped */
		return;
	}

	Relation index = index_open(storage_id_index_id, AccessShareLock);

	SysScanDesc scan_descriptor = systable_beginscan_ordered(metadata_table, index, nullptr,
															1, scan_keys);

	bioseq::storage::metadata::tuple_ops::relation_modification_state *relation_modification_state = bioseq::storage::metadata::tuple_ops::begin_relation_modification(metadata_table);

	HeapTuple heap_tuple;
	while (HeapTupleIsValid(heap_tuple = systable_getnext_ordered(scan_descriptor,
																 ForwardScanDirection)))
	{
		bioseq::storage::metadata::tuple_ops::delete_catalog_tuple(relation_modification_state, heap_tuple);
	}

	systable_endscan_ordered(scan_descriptor);

	bioseq::storage::metadata::tuple_ops::finish_relation_modification(relation_modification_state);

	index_close(index, AccessShareLock);
	table_close(metadata_table, AccessShareLock);
}


/*
 * delete_stripe_metadata_rows removes the rows in columnar
 * metadata table that match storage_id and stripe_id.
 */
static void
delete_stripe_metadata_rows(Oid metadata_table_id,
									  AttrNumber storage_id_attribute_number,
									  AttrNumber stripe_id_attr_number,
									  Oid storage_id_index_id,
									  uint64_t storage_id,
									  uint64_t stripe_id)
{
	ScanKeyData scan_keys[2];
	ScanKeyInit(&scan_keys[0], storage_id_attribute_number, BTEqualStrategyNumber,
				F_INT8EQ, UInt64GetDatum(storage_id));
	ScanKeyInit(&scan_keys[1], stripe_id_attr_number, BTEqualStrategyNumber,
				F_INT8EQ, UInt64GetDatum(stripe_id));

	Relation metadata_table =
		bioseq::postgres::support::try_open_postgres_relation(
			metadata_table_id, RowShareLock);
	if (metadata_table == nullptr)
	{
		/* extension has been dropped */
		return;
	}

	Relation index = index_open(storage_id_index_id, RowShareLock);
	SysScanDesc scan_descriptor = systable_beginscan_ordered(metadata_table, index, nullptr,
															2, scan_keys);

	bioseq::storage::metadata::tuple_ops::relation_modification_state *relation_modification_state = bioseq::storage::metadata::tuple_ops::begin_relation_modification(metadata_table);

	HeapTuple heap_tuple;
	while (HeapTupleIsValid(heap_tuple = systable_getnext_ordered(scan_descriptor,
																 ForwardScanDirection)))
	{
		bioseq::storage::metadata::tuple_ops::delete_catalog_tuple(relation_modification_state, heap_tuple);
	}

	systable_endscan_ordered(scan_descriptor);

	bioseq::storage::metadata::tuple_ops::finish_relation_modification(relation_modification_state);

	index_close(index, RowShareLock);
	table_close(metadata_table, RowShareLock);
}


/* Resolve bioseq_internal.storageid_seq, or return InvalidOid if missing. */
static Oid
column_storage_id_sequence_oid(void)
{
	return metadata_entity_handles::storageid_seq.resolve_oid(
		bioseq::postgres::support::catalog_missing_policy::return_invalid);
}


/*
 * column_stripe_relation_oid returns relation id of bioseq.stripe.
 * Resolved per call so catalog recreation observes the current OID.
 */
Oid
column_stripe_relation_oid(void)
{
	return get_relname_relid("stripe", column_namespace_oid());
}


/*
 * column_stripe_primary_key_index_oid returns relation id of bioseq.stripe_pkey.
 * Resolved per call so catalog recreation observes the current OID.
 */
Oid
column_stripe_primary_key_index_oid(void)
{
	return get_relname_relid("stripe_pkey", column_namespace_oid());
}


/*
 * stripe_first_row_number_index_oid returns relation id of
 * bioseq.stripe_first_row_number_idx.
 * Resolved per call so catalog recreation observes the current OID.
 */
static Oid
stripe_first_row_number_index_oid(void)
{
	return get_relname_relid("stripe_first_row_number_idx", column_namespace_oid());
}


/*
 * column_options_relation_oid returns relation id of bioseq.options.
 */
static Oid
column_options_relation_oid(void)
{

	return get_relname_relid("options", column_namespace_oid());
}


/*
 * column_options_index_oid returns relation id of bioseq.options_pkey.
 */
static Oid
column_options_index_oid(void)
{
	bioseq_log<logging_level::debug>("[bioseq] pg: column_options_index_oid() entered");
	return get_relname_relid("options_pkey", column_namespace_oid());
}


/*
 * column_chunk_relation_oid returns relation id of bioseq.chunk.
 * Resolved per call so catalog recreation observes the current OID.
 */
static Oid
column_chunk_relation_oid(void)
{
	bioseq_log<logging_level::debug>("[bioseq] pg: column_chunk_relation_oid() entered");
	return get_relname_relid("chunk", column_namespace_oid());
}


/*
 * column_chunk_group_relation_oid returns relation id of bioseq.chunk_group.
 * Resolved per call so catalog recreation observes the current OID.
 */
static Oid
column_chunk_group_relation_oid(void)
{
	bioseq_log<logging_level::debug>("[bioseq] pg: column_chunk_group_relation_oid() entered");
	return get_relname_relid("chunk_group", column_namespace_oid());
}


/*
 * column_row_mask_relation_oid returns relation id of bioseq.row_mask.
 */
Oid
column_row_mask_relation_oid(void)
{
	bioseq_log<logging_level::debug>("[bioseq] pg: column_row_mask_relation_oid() entered");
	return get_relname_relid("row_mask", column_namespace_oid());
}


/* Resolve bioseq_internal.row_mask_seq, or return InvalidOid if missing. */
static Oid
column_row_mask_sequence_oid(void)
{
	bioseq_log<logging_level::debug>("[bioseq] pg: column_row_mask_sequence_oid() entered");
	return metadata_entity_handles::row_mask_seq.resolve_oid(
		bioseq::postgres::support::catalog_missing_policy::return_invalid);
}



/*
 * column_chunk_index_oid returns relation id of bioseq.chunk_pkey.
 * Resolved per call so catalog recreation observes the current OID.
 */
static Oid
column_chunk_index_oid(void)
{
	bioseq_log<logging_level::debug>("[bioseq] pg: column_chunk_index_oid() entered");
	return get_relname_relid("chunk_pkey", column_namespace_oid());
}


/*
 * column_chunk_group_index_oid returns relation id of bioseq.chunk_group_pkey.
 * Resolved per call so catalog recreation observes the current OID.
 */
static Oid
column_chunk_group_index_oid(void)
{
	bioseq_log<logging_level::debug>("[bioseq] pg: column_chunk_group_index_oid() entered");
	return get_relname_relid("chunk_group_pkey", column_namespace_oid());
}


/*
 * column_row_mask_index_oid returns relation id
 * of bioseq.row_mask_pkey
 */
Oid
column_row_mask_index_oid(void)
{
	bioseq_log<logging_level::debug>("[bioseq] pg: column_row_mask_index_oid() entered");
	return get_relname_relid("row_mask_pkey", column_namespace_oid());
}

/*
 * column_row_mask_stripe_index_oid returns relation id
 * of bioseq.bioseq_row_mask_stripe_unique
 */
static Oid
column_row_mask_stripe_index_oid(void)
{
	bioseq_log<logging_level::debug>("[bioseq] pg: column_row_mask_stripe_index_oid() entered");
	return get_relname_relid("row_mask_stripe_unique", column_namespace_oid());
}


/*
 * column_namespace_oid returns namespace id of the schema we store columnar
 * related tables.
 */
Oid
column_namespace_oid(void)
{
	Oid nspace = get_namespace_oid("bioseq_internal", false);
	bioseq_log<logging_level::debug>("[bioseq] pg: column_namespace_oid() looking up bioseq_internal namespace");
	bioseq_log<logging_level::debug>("[bioseq] pg: column_namespace_oid() returning oid");
	return nspace;
}


/*
 * lookup_storage_id reads storage metapage to find the storage ID for the given relfilelocator. It returns
 * false if the relation doesn't have a meta page yet.
 */
uint64_t
lookup_storage_id(RelFileLocator relfilelocator)
{
	bioseq_log<logging_level::debug>("[bioseq] pg: lookup_storage_id() entered");
	Oid relation_oid = RelidByRelfilenumber(
		relfilelocator.spcOid, relfilelocator.relNumber);

	Relation relation = bioseq::postgres::support::open_postgres_relation(
		relation_oid, AccessShareLock);
	uint64_t storage_id = column_storage_id(relation, false);
	table_close(relation, AccessShareLock);

	return storage_id;
}


/*
 * allocate_column_storage_id - create a new, unique storage id and return
 * it.
 */
export uint64_t
allocate_column_storage_id()
{
	bioseq_log<logging_level::debug>("[bioseq] pg: allocate_column_storage_id() entered");
	return nextval_internal(column_storage_id_sequence_oid(), false);
}


/** @brief Ensures the relation metapage uses the current column storage format.
 *
 * Empty relations receive a new metapage. Existing incompatible metapages are
 * reconstructed from the authoritative stripe metadata before their format
 * identity is updated.
 */
export void
ensure_current_column_storage_format(Relation rel, bool is_upgrade)
{
	bioseq_log<logging_level::debug>("[bioseq] pg: ensure_current_column_storage_format() entered");
	if (column_storage_uses_current_format(rel))
	{
		return;
	}

	BlockNumber nblocks = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);
	bioseq_log<logging_level::debug>("[bioseq] pg: ensure_current_column_storage_format() checking nblocks<2");
	if (nblocks < 2)
	{
		initialize_column_storage(rel->rd_smgr, allocate_column_storage_id());
		return;
	}

	uint64_t storage_id = column_storage_id(rel, true);

	uint64_t highest_id;
	uint64_t highest_offset;
	find_highest_used_address_and_id(storage_id, &highest_offset, &highest_id);

	const uint64_t highest_row_number =
		find_highest_used_row_number(storage_id);
	const std::optional<uint64_t> next_stripe_id =
		bioseq::common::checked_add(highest_id, static_cast<uint64_t>(1));
	const std::optional<uint64_t> next_row_number =
		bioseq::common::checked_add(
			highest_row_number, static_cast<uint64_t>(1));

	std::optional<uint64_t> next_logical_offset = first_logical_offset;
	if (highest_id != 0)
	{
		if (highest_offset < first_logical_offset)
		{
			bioseq::postgres::support::raise_sql_error(
				ERRCODE_DATA_CORRUPTED,
				{
					.message = fmt::format(
						"cannot reconstruct column storage counters for relation {}",
						rel->rd_id),
					.detail = fmt::format(
						"Stripe metadata ends at logical offset {}, below the "
						"first valid offset {}.",
						highest_offset,
						first_logical_offset),
				});
		}
		next_logical_offset = bioseq::common::checked_add(
			highest_offset, static_cast<uint64_t>(1));
	}

	if (!next_stripe_id.has_value() || !next_row_number.has_value()
		|| !next_logical_offset.has_value())
	{
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_PROGRAM_LIMIT_EXCEEDED,
			{
				.message = fmt::format(
					"cannot reconstruct column storage counters for relation {}",
					rel->rd_id),
				.detail = "A reconstructed stripe, row, or logical-offset "
						  "counter would overflow its 64-bit storage field.",
			});
	}

	update_column_storage_format(
		rel,
		is_upgrade,
		*next_stripe_id,
		*next_row_number,
		*next_logical_offset);
}


/*
 * find_highest_used_row_number returns the highest used row_number for given
 * storage_id. Returns invalid_column_row_number if storage with
 * storage_id has no stripes.
 * Note that normally we would use column_storage_reserved_row_number
 * to decide that. However, this function is designed to be used when
 * building the metapage itself during upgrades.
 */
static uint64_t
find_highest_used_row_number(uint64_t storage_id)
{
	uint64_t highest_row_number = invalid_column_row_number;

	List *stripe_metadata_list = read_data_file_stripes(storage_id,
													  GetTransactionSnapshot(),
													  ForwardScanDirection);
	for (stripe_metadata* stripe :
		 bioseq::postgres::support::postgres_list<stripe_metadata>{
			 stripe_metadata_list})
	{
		highest_row_number = std::max(highest_row_number,
							   stripe_highest_row_number(stripe));
	}

	return highest_row_number;
}

/*
 * RewriteMetadataRowWithNewValues rewrites a given metadata entry
 * for a storage_id and a stripe_id in place with a new offset,
 * row_count, size_bytes, and chunk_count.
 *
 * This is used in the vacuum UDF to fill any existing holes
 * if possible.
 */
export stripe_metadata *
rewrite_stripe_metadata(Relation rel, uint64_t stripe_id,
              uint64_t size_bytes, uint64_t file_offset, uint64_t row_count, uint64_t chunk_count)
{
	uint64_t storage_id = column_storage_id(rel, false);

	stripe_codec::update update{};
	update
		.set<&stripe_row::file_offset>(
			static_cast<std::int64_t>(file_offset))
		.set<&stripe_row::data_length>(
			static_cast<std::int64_t>(size_bytes))
		.set<&stripe_row::row_count>(
			static_cast<std::int64_t>(row_count))
		.set<&stripe_row::chunk_group_count>(
			static_cast<std::int32_t>(chunk_count));

	return update_stripe_metadata_row(storage_id, stripe_id, update);
}

// bioseq_internal.seq_sub_table access uses the systable API so MVCC and
// transactional semantics match the other metadata catalogs.

export struct sequence_sub_table_row {
	Oid             database_oid        = InvalidOid;
	Oid             tablespace_oid      = InvalidOid;
	Oid             main_relfilenode    = InvalidOid;
	int16_t         column_attno        = 0;
	int16_t         bucket_idx          = 0;
	int32_t         shard_idx           = 0;
	Oid             sub_table_relid     = InvalidOid;
	Oid             sidecar_relid       = InvalidOid;
	std::uint64_t   bytes_written       = 0;
	bool            is_full             = false;
	std::uint64_t   total_payload_bytes = 0;

	sequence_sub_table_row() = default;

	sequence_sub_table_row(HeapTuple heap_tuple, TupleDesc td) {
		const seq_sub_table_row row =
			seq_sub_table_codec::deform(
				heap_tuple, td, CurrentMemoryContext);
		database_oid = row.database_oid;
		tablespace_oid = row.tablespace_oid;
		main_relfilenode = row.main_relfilenode;
		column_attno = row.column_attno;
		bucket_idx = row.bucket_idx;
		shard_idx = row.shard_idx;
		sub_table_relid = row.sub_table_relid;
		sidecar_relid = row.sidecar_relid;
		bytes_written =
			static_cast<std::uint64_t>(row.bytes_written);
		is_full = row.is_full;
		total_payload_bytes =
			static_cast<std::uint64_t>(row.total_payload_bytes);
	}
};

export struct sequence_sub_table_manager {
	// Relation-id resolvers (trivial, hot) — drop the redundant SubTableMetadata
	// prefix; it is obvious these resolve seq_sub_table relations.
	[[gnu::always_inline]] static inline Oid relid() {
		return get_relname_relid("seq_sub_table", column_namespace_oid());
	}
	[[gnu::always_inline]] static inline Oid pkey_index_relid() {
		return get_relname_relid("seq_sub_table_pkey", column_namespace_oid());
	}
	[[gnu::always_inline]] static inline Oid active_shard_index_relid() {
		return get_relname_relid("seq_sub_table_active_shard_idx", column_namespace_oid());
	}

	[[nodiscard]] static std::optional<sequence_sub_table_row>
	lookup_active_shard(Oid database_oid, Oid tablespace_oid, Oid main_relfilenode,
	                    int16_t column_attno, int16_t bucket_idx)
	{
		const Oid rel_oid = relid();
		if (!OidIsValid(rel_oid)) {
			return std::nullopt;
		}
		Relation rel = bioseq::postgres::support::open_postgres_relation(
			rel_oid, AccessShareLock);
		TupleDesc td = RelationGetDescr(rel);

		const Oid idx_oid = active_shard_index_relid();
		const bool idx_ok = OidIsValid(idx_oid);

		std::array<ScanKeyData, 5> keys{
			seq_sub_table_codec::scan_key<
				&seq_sub_table_row::database_oid>(database_oid),
			seq_sub_table_codec::scan_key<
				&seq_sub_table_row::tablespace_oid>(tablespace_oid),
			seq_sub_table_codec::scan_key<
				&seq_sub_table_row::main_relfilenode>(main_relfilenode),
			seq_sub_table_codec::scan_key<
				&seq_sub_table_row::column_attno>(column_attno),
			seq_sub_table_codec::scan_key<
				&seq_sub_table_row::bucket_idx>(bucket_idx),
		};

		SysScanDesc scan = systable_beginscan(
			rel, idx_oid, idx_ok, nullptr,
			static_cast<int>(keys.size()), keys.data());
		std::optional<sequence_sub_table_row> result;
		HeapTuple tup = nullptr;
		while (HeapTupleIsValid(tup = systable_getnext(scan))) {
			sequence_sub_table_row row(tup, td);
			if (!row.is_full) {
				result = row;
				break;
			}
		}
		systable_endscan(scan);
		table_close(rel, AccessShareLock);
		return result;
	}

	[[nodiscard]] static std::optional<sequence_sub_table_row>
	lookup_shard(Oid database_oid, Oid tablespace_oid, Oid main_relfilenode,
	             int16_t column_attno, int16_t bucket_idx, int32_t shard_idx)
	{
		const Oid rel_oid = relid();
		if (!OidIsValid(rel_oid)) {
			return std::nullopt;
		}
		Relation rel = bioseq::postgres::support::open_postgres_relation(
			rel_oid, AccessShareLock);
		TupleDesc td = RelationGetDescr(rel);

		const Oid idx_oid = pkey_index_relid();
		const bool idx_ok = OidIsValid(idx_oid);

		std::array<ScanKeyData, 6> keys{
			seq_sub_table_codec::scan_key<
				&seq_sub_table_row::database_oid>(database_oid),
			seq_sub_table_codec::scan_key<
				&seq_sub_table_row::tablespace_oid>(tablespace_oid),
			seq_sub_table_codec::scan_key<
				&seq_sub_table_row::main_relfilenode>(main_relfilenode),
			seq_sub_table_codec::scan_key<
				&seq_sub_table_row::column_attno>(column_attno),
			seq_sub_table_codec::scan_key<
				&seq_sub_table_row::bucket_idx>(bucket_idx),
			seq_sub_table_codec::scan_key<
				&seq_sub_table_row::shard_idx>(shard_idx),
		};

		SysScanDesc scan = systable_beginscan(
			rel, idx_oid, idx_ok, nullptr,
			static_cast<int>(keys.size()), keys.data());
		std::optional<sequence_sub_table_row> result;
		HeapTuple tup = systable_getnext(scan);
		if (HeapTupleIsValid(tup)) {
			result = sequence_sub_table_row(tup, td);
		}
		systable_endscan(scan);
		table_close(rel, AccessShareLock);
		return result;
	}

	[[nodiscard]] static sequence_sub_table_row
	insert_shard(Oid database_oid, Oid tablespace_oid, Oid main_relfilenode,
	             int16_t column_attno, int16_t bucket_idx, int32_t shard_idx,
	             Oid sub_table_relid, Oid sidecar_relid)
	{
		const Oid rel_oid = relid();
		if (!OidIsValid(rel_oid)) {
			bioseq::postgres::support::raise_sql_error(
				ERRCODE_INTERNAL_ERROR,
				{
					.message =
						"bioseq storage: seq_sub_table catalog not present",
				});
		}

		// Race-loss handling — if the unique partial index already accepted a
		// row for the same (db, spc, relfilenode, attno, bucket) with
		// is_full=FALSE, return the winner instead of trying to insert a duplicate.
		{
			std::optional<sequence_sub_table_row> existing = lookup_active_shard(
				database_oid, tablespace_oid, main_relfilenode,
				column_attno, bucket_idx);
			if (existing.has_value() && existing->shard_idx == shard_idx) {
				return *existing;
			}
			if (existing.has_value() && existing->shard_idx != shard_idx) {
				return *existing;
			}
		}

		Relation rel = bioseq::postgres::support::open_postgres_relation(
			rel_oid, RowExclusiveLock);
		TupleDesc td = RelationGetDescr(rel);

		const seq_sub_table_row catalog_row{
			.database_oid = database_oid,
			.tablespace_oid = tablespace_oid,
			.main_relfilenode = main_relfilenode,
			.column_attno = column_attno,
			.bucket_idx = bucket_idx,
			.shard_idx = shard_idx,
			.sub_table_relid = sub_table_relid,
			.sidecar_relid = sidecar_relid,
			.bytes_written = 0,
			.is_full = false,
			.total_payload_bytes = 0,
		};
		HeapTuple tup = seq_sub_table_codec::encode(
			catalog_row, CurrentMemoryContext).form_tuple(td);
		CatalogTupleInsert(rel, tup);
		heap_freetuple(tup);
		table_close(rel, RowExclusiveLock);
		CommandCounterIncrement();

		sequence_sub_table_row row;
		row.database_oid       = database_oid;
		row.tablespace_oid     = tablespace_oid;
		row.main_relfilenode   = main_relfilenode;
		row.column_attno       = column_attno;
		row.bucket_idx         = bucket_idx;
		row.shard_idx          = shard_idx;
		row.sub_table_relid    = sub_table_relid;
		row.sidecar_relid      = sidecar_relid;
		row.bytes_written      = 0;
		row.is_full            = false;
		row.total_payload_bytes = 0;
		return row;
	}

	static sequence_sub_table_row
	update_shard_bytes_written(Oid database_oid, Oid tablespace_oid, Oid main_relfilenode,
	                           int16_t column_attno, int16_t bucket_idx, int32_t shard_idx,
	                           std::uint64_t delta_bytes,
	                           std::uint64_t cap_bytes)
	{
		const Oid rel_oid = relid();
		const Oid idx_oid = pkey_index_relid();
		if (!OidIsValid(rel_oid)) {
			bioseq::postgres::support::raise_sql_error(
				ERRCODE_INTERNAL_ERROR,
				{
					.message =
						"bioseq storage: seq_sub_table catalog not present",
				});
		}

		Relation rel = bioseq::postgres::support::open_postgres_relation(
			rel_oid, RowExclusiveLock);
		TupleDesc td = RelationGetDescr(rel);

		std::array<ScanKeyData, 6> keys{
			seq_sub_table_codec::scan_key<
				&seq_sub_table_row::database_oid>(database_oid),
			seq_sub_table_codec::scan_key<
				&seq_sub_table_row::tablespace_oid>(tablespace_oid),
			seq_sub_table_codec::scan_key<
				&seq_sub_table_row::main_relfilenode>(main_relfilenode),
			seq_sub_table_codec::scan_key<
				&seq_sub_table_row::column_attno>(column_attno),
			seq_sub_table_codec::scan_key<
				&seq_sub_table_row::bucket_idx>(bucket_idx),
			seq_sub_table_codec::scan_key<
				&seq_sub_table_row::shard_idx>(shard_idx),
		};

		SysScanDesc scan = systable_beginscan(
			rel, idx_oid, OidIsValid(idx_oid), nullptr,
			static_cast<int>(keys.size()), keys.data());
		HeapTuple old_tuple = systable_getnext(scan);
		if (!HeapTupleIsValid(old_tuple)) {
			systable_endscan(scan);
			table_close(rel, RowExclusiveLock);
			bioseq::postgres::support::raise_sql_error(
				ERRCODE_INTERNAL_ERROR,
				{
					.message = fmt::format(
						"bioseq storage: cannot update missing sub-table "
						"shard (db={}, spc={}, relfilenode={}, attno={}, "
						"bucket={}, shard={})",
						static_cast<unsigned>(database_oid),
						static_cast<unsigned>(tablespace_oid),
						static_cast<unsigned>(main_relfilenode),
						static_cast<int>(column_attno),
						static_cast<int>(bucket_idx),
						static_cast<int>(shard_idx)),
				});
		}

		sequence_sub_table_row current(old_tuple, td);
		const std::uint64_t new_bytes_written =
			current.bytes_written + delta_bytes;
		const std::uint64_t new_total_payload =
			current.total_payload_bytes + delta_bytes;
		// is_full is driven only by a real byte cap. The zero-cap
		// shard-per-insert sentinel is implemented entirely by insert_payload
		// rollover (which, when the active shard already holds a row, marks it full
		// and allocates the next shard, passing cap_bytes=1 so this branch flips
		// is_full). We deliberately do NOT mark a shard full here merely because
		// cap_bytes==0: the per-commit byte flush from close_all must leave a
		// single-row shard ACTIVE so the next insert's rollover is the one (and
		// only) place that advances the shard index — keeping the full-marking and
		// the new-shard creation in one transaction, exactly like the normal
		// byte-cap rollover.
		const bool now_full = current.is_full
			|| (cap_bytes != 0 && new_bytes_written >= cap_bytes);

		seq_sub_table_codec::update update{};
		update
			.set<&seq_sub_table_row::bytes_written>(
				static_cast<std::int64_t>(new_bytes_written))
			.set<&seq_sub_table_row::total_payload_bytes>(
				static_cast<std::int64_t>(new_total_payload))
			.set<&seq_sub_table_row::is_full>(now_full);

		HeapTuple new_tuple = update.modify(old_tuple, td);
		CatalogTupleUpdate(rel, &old_tuple->t_self, new_tuple);
		systable_endscan(scan);
		heap_freetuple(new_tuple);
		table_close(rel, RowExclusiveLock);
		CommandCounterIncrement();

		current.bytes_written       = new_bytes_written;
		current.total_payload_bytes = new_total_payload;
		current.is_full             = now_full;
		return current;
	}

	[[nodiscard]] static std::vector<sequence_sub_table_row>
	list_shards_for_main(Oid database_oid, Oid tablespace_oid, Oid main_relfilenode)
	{
		std::vector<sequence_sub_table_row> out;
		const Oid rel_oid = bioseq::postgres::support::invoke_pg_as_cpp(
			[] { return relid(); });
		if (!OidIsValid(rel_oid)) {
			return out;
		}
		Relation rel = bioseq::postgres::support::invoke_pg_as_cpp(
			[rel_oid] {
				return bioseq::postgres::support::try_open_postgres_relation(
					rel_oid, AccessShareLock);
			});
		if (rel == nullptr) {
			return out;
		}
		TupleDesc td = RelationGetDescr(rel);

		std::array<ScanKeyData, 3> keys{};
		SysScanDesc scan = nullptr;
		std::exception_ptr failure;
		try {
			keys = {
				seq_sub_table_codec::scan_key<
					&seq_sub_table_row::database_oid>(database_oid),
				seq_sub_table_codec::scan_key<
					&seq_sub_table_row::tablespace_oid>(tablespace_oid),
				seq_sub_table_codec::scan_key<
					&seq_sub_table_row::main_relfilenode>(
						main_relfilenode),
			};
			scan = bioseq::postgres::support::invoke_pg_as_cpp([rel, &keys] {
				return systable_beginscan(
					rel, InvalidOid, false, nullptr,
					static_cast<int>(keys.size()), keys.data());
			});
			for (;;) {
				HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp(
					[scan] { return systable_getnext(scan); });
				if (!HeapTupleIsValid(tuple)) break;
				sequence_sub_table_row row = bioseq::postgres::support::invoke_pg_as_cpp(
					[tuple, td] { return sequence_sub_table_row(tuple, td); });
				out.push_back(std::move(row));
			}
		} catch (...) {
			failure = std::current_exception();
		}
		if (scan != nullptr) {
			record_cleanup_failure(failure, [scan] {
				bioseq::postgres::support::invoke_pg_as_cpp(
					[scan] { systable_endscan(scan); });
			});
		}
		record_cleanup_failure(failure, [rel] {
			bioseq::postgres::support::invoke_pg_as_cpp(
				[rel] { table_close(rel, AccessShareLock); });
		});
		if (failure != nullptr) std::rethrow_exception(failure);
		return out;
	}

	static void
	delete_shards_for_main(Oid database_oid, Oid tablespace_oid, Oid main_relfilenode)
	{
		const Oid rel_oid = relid();
		if (!OidIsValid(rel_oid)) {
			return;
		}
		Relation rel = bioseq::postgres::support::try_open_postgres_relation(
			rel_oid, RowExclusiveLock);
		if (rel == nullptr) {
			return;
		}

		std::array<ScanKeyData, 3> keys{
			seq_sub_table_codec::scan_key<
				&seq_sub_table_row::database_oid>(database_oid),
			seq_sub_table_codec::scan_key<
				&seq_sub_table_row::tablespace_oid>(tablespace_oid),
			seq_sub_table_codec::scan_key<
				&seq_sub_table_row::main_relfilenode>(main_relfilenode),
		};

		SysScanDesc scan = systable_beginscan(
			rel, InvalidOid, false, nullptr,
			static_cast<int>(keys.size()), keys.data());
		HeapTuple tup = nullptr;
		while (HeapTupleIsValid(tup = systable_getnext(scan))) {
			CatalogTupleDelete(rel, &tup->t_self);
		}
		systable_endscan(scan);
		table_close(rel, RowExclusiveLock);
		CommandCounterIncrement();
	}
};
