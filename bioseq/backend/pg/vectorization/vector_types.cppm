/*-------------------------------------------------------------------------
 *
 * vectorization/vector_types.cppm
 *
 * Owns the vectorised data model used by the columnar custom scan and the
 * exported helpers that build/populate it. A vector_tuple_table_slot_storage stores one
 * vector_column_storage per attribute, each of which packs up to
 * `vector_column_capacity` values for batched evaluation. The storage records
 * are shared directly across modules, so their field order and types are part
 * of the private vector-function boundary. The helpers allocate slots,
 * write incoming tuples, extract scalar tuples, and reset batches.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <bioseq/pg_compat.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

export module bioseq.postgres.vectorization.types;

import bioseq.postgres.support.datums;
import bioseq.postgres.support;
import bioseq.postgres.support.runtime_diagnostics;
import bioseq.parallelism.memory_admission;

/** Maximum scalar rows stored in one vector batch. */
export constexpr std::uint32_t vector_column_capacity = 10'000;

/*
 * Varlena values need ownership that is independent of the input slot. Keep
 * that distinction explicit instead of inferring it from stored_by_value: a
 * varlena lane contains an owned Datum pointer, while fixed-width values are
 * stored inline (using PostgreSQL's by-value flag when extracted).
 */
export enum class vector_storage_kind : std::uint8_t
{
	inline_fixed,
	owned_varlena_datum
};

/** Raw storage passed through the private vector-function boundary. */
export struct vector_column_storage
{
	/** Number of populated lanes. */
	std::uint32_t lane_count;
	/** Bytes reserved for each lane in data. */
	std::uint16_t element_width;
	/** Whether fetch_att reads each lane as an inline Datum value. */
	bool stored_by_value;
	/** Ownership policy for lane payloads. */
	vector_storage_kind storage_kind;
	/** Context that owns copied varlena payloads. */
	MemoryContext storage_context;
	/** Aligned fixed-stride lane buffer. */
	Datum* data;
	/** PostgreSQL NULL flag for each possible lane. */
	bool is_null[vector_column_capacity];
	/** Optional batch-level source-row mapping. */
	std::uint64_t* source_row_numbers;

	[[nodiscard]] std::span<bool> nulls() noexcept
	{
		return {is_null, lane_count};
	}

	[[nodiscard]] std::span<const bool> nulls() const noexcept
	{
		return {is_null, lane_count};
	}

	template <class p_value>
	[[nodiscard]] std::span<p_value> values() noexcept
	{
		return {reinterpret_cast<p_value*>(data), lane_count};
	}

	template <class p_value>
	[[nodiscard]] std::span<const p_value> values() const noexcept
	{
		return {reinterpret_cast<const p_value*>(data), lane_count};
	}
};

/** TupleTableSlot extension that owns one vector batch. */
export struct vector_tuple_table_slot_storage
{
	/** PostgreSQL virtual-slot prefix consumed by TTSOpsVirtual. */
	VirtualTupleTableSlot virtual_slot;
	/** Number of populated rows. */
	std::uint32_t row_count;
	/** Rows that remain after vector predicate evaluation. */
	bool row_is_active[vector_column_capacity];
	/** Source row number for each batch row. */
	std::uint64_t source_row_numbers[vector_column_capacity];
};

/*
 * These enum values are the first members of reinterpret-cast structures.
 * Their explicit `int` representation therefore remains part of the ABI.
 */
export enum class vector_operand_storage_kind : int
{
	constant,
	column
};

export struct vector_operand_storage
{
	vector_operand_storage_kind kind;
	Datum datum;
};

/** Serialized storage used by PostgreSQL's int8 aggregate state. */
export struct int128_aggregate_state_storage
{
	bool calculate_sum_of_squares;
	int64 count;
	int128 sum;
	int128 sum_of_squares;
};

export struct int64_aggregate_state_storage
{
	int64 count;
	int64 sum;
};

/* Direct assertions protect the storage boundaries without shadow layouts. */
namespace {

[[nodiscard]] consteval std::size_t
aligned_offset(std::size_t offset, std::size_t alignment) noexcept
{
	return (offset + alignment - 1) / alignment * alignment;
}

static_assert(std::is_standard_layout_v<vector_operand_storage>);
static_assert(offsetof(vector_operand_storage, kind) == 0);
static_assert(
	std::is_same_v<
		std::underlying_type_t<vector_operand_storage_kind>, int>);
static_assert(
	offsetof(vector_operand_storage, datum)
	== aligned_offset(
		sizeof(vector_operand_storage_kind), alignof(Datum)));
static_assert(std::is_standard_layout_v<vector_column_storage>);
static_assert(offsetof(vector_column_storage, lane_count) == 0);
static_assert(std::is_standard_layout_v<vector_tuple_table_slot_storage>);
static_assert(
	offsetof(vector_tuple_table_slot_storage, virtual_slot) == 0);
static_assert(
	offsetof(VirtualTupleTableSlot, base) == 0);
static_assert(std::is_standard_layout_v<int128_aggregate_state_storage>);
static_assert(offsetof(int128_aggregate_state_storage,
	calculate_sum_of_squares) == 0);
static_assert(
	offsetof(int128_aggregate_state_storage, count)
	== aligned_offset(sizeof(bool), alignof(int64)));
static_assert(
	offsetof(int128_aggregate_state_storage, sum)
	== aligned_offset(
		offsetof(int128_aggregate_state_storage, count) + sizeof(int64),
		alignof(int128)));
static_assert(
	offsetof(int128_aggregate_state_storage, sum_of_squares)
	== offsetof(int128_aggregate_state_storage, sum) + sizeof(int128));
static_assert(
	sizeof(int128_aggregate_state_storage)
	== offsetof(int128_aggregate_state_storage, sum_of_squares)
		+ sizeof(int128));
static_assert(std::is_standard_layout_v<int64_aggregate_state_storage>);
static_assert(offsetof(int64_aggregate_state_storage, count) == 0);
static_assert(
	offsetof(int64_aggregate_state_storage, sum) == sizeof(int64));
static_assert(sizeof(int64_aggregate_state_storage) == 2 * sizeof(int64));

}  // namespace

/** Allocate an empty vector column with fixed batch-capacity storage. */
export [[nodiscard]] vector_column_storage*
allocate_vector_column(
	std::int16_t element_width,
	bool stored_by_value,
	std::uint64_t* source_row_numbers,
	vector_storage_kind storage_kind = vector_storage_kind::inline_fixed,
	MemoryContext storage_context = CurrentMemoryContext)
{
	vector_column_storage* column =
		static_cast<vector_column_storage*>(palloc0(sizeof(vector_column_storage)));

	column->lane_count = 0;
	column->data = static_cast<Datum*>(palloc0(
		bioseq::parallelism::saturating_mul(
			static_cast<std::uint64_t>(element_width),
			vector_column_capacity)));
	column->element_width = static_cast<uint16>(element_width);
	column->stored_by_value = stored_by_value;
	column->storage_kind = storage_kind;
	column->storage_context = storage_context;
	column->source_row_numbers = source_row_numbers;

	return column;
}

namespace {

void
note_owned_varlena_source_form(Datum source) noexcept
{
	if (!bioseq::postgres::support::runtime_diagnostics::
			is_bpchar_source_observation_active())
		return;

	const Pointer raw = DatumGetPointer(source);
	using enum bioseq::postgres::support::runtime_diagnostics::
		bpchar_source_form;
	auto form = unknown;
	if (raw != nullptr)
	{
		if (VARATT_IS_EXTERNAL_ONDISK(raw))
			form = external;
		else if (VARATT_IS_COMPRESSED(raw))
			form = compressed;
		else if (VARATT_IS_4B_U(raw))
			form = four_byte_uncompressed;
		else if (VARATT_IS_SHORT(raw))
			form = short_header;
	}
	bioseq::postgres::support::runtime_diagnostics::
		record_bpchar_source_form(
			form,
			bioseq::postgres::support::runtime_diagnostics::
				bpchar_source_site::vector_copy);
}

void
cleanup_vector_slot_contents(
	vector_tuple_table_slot_storage* vector_slot,
	bioseq::postgres::support::runtime_diagnostics::
		vector_payload_cleanup_kind cleanup_kind)
{
	bioseq::postgres::support::runtime_diagnostics::
		record_vector_cleanup(cleanup_kind);
	TupleDesc tuple_desc = vector_slot->virtual_slot.base.tts_tupleDescriptor;
	if (tuple_desc == nullptr)
		return;

	for (int attribute_index = 0; attribute_index < tuple_desc->natts;
		 ++attribute_index)
	{
		vector_column_storage* column =
			reinterpret_cast<vector_column_storage*>(
				DatumGetPointer(
					vector_slot->virtual_slot.base.tts_values[attribute_index]));
		if (column == nullptr)
			continue;

		if (column->storage_kind == vector_storage_kind::owned_varlena_datum)
		{
			for (uint32 row_index = 0; row_index < column->lane_count; ++row_index)
			{
				Datum& datum = column->data[row_index];
				if (!column->is_null[row_index] &&
					DatumGetPointer(datum) != nullptr)
				{
					bioseq::postgres::support::runtime_diagnostics::
						record_vector_payload_cleanup_free(cleanup_kind);
					pfree(DatumGetPointer(datum));
				}
				datum = static_cast<Datum>(0);
			}
		}
		std::fill_n(column->is_null, vector_column_capacity, true);
		column->lane_count = 0;
	}

	std::fill_n(
		vector_slot->row_is_active, vector_column_capacity, true);
	vector_slot->row_count = 0;
}

void
clear_vector_tuple_slot(TupleTableSlot* slot)
{
	cleanup_vector_slot_contents(
		reinterpret_cast<vector_tuple_table_slot_storage*>(slot),
		bioseq::postgres::support::runtime_diagnostics::
			vector_payload_cleanup_kind::slot_clear);
	TTSOpsVirtual.clear(slot);
}

void
release_vector_tuple_slot(TupleTableSlot* slot)
{
	auto* vector_slot = reinterpret_cast<vector_tuple_table_slot_storage*>(slot);
	cleanup_vector_slot_contents(
		vector_slot,
		bioseq::postgres::support::runtime_diagnostics::
			vector_payload_cleanup_kind::slot_release);
	TTSOpsVirtual.clear(slot);

	TupleDesc tuple_desc = slot->tts_tupleDescriptor;
	if (tuple_desc == nullptr) {
		TTSOpsVirtual.release(slot);
		return;
	}
	for (int attribute_index = 0; attribute_index < tuple_desc->natts;
		 ++attribute_index)
	{
		vector_column_storage* column =
			reinterpret_cast<vector_column_storage*>(
				DatumGetPointer(slot->tts_values[attribute_index]));
		if (column == nullptr)
			continue;
		pfree(column->data);
		pfree(column);
		slot->tts_values[attribute_index] = static_cast<Datum>(0);
		slot->tts_isnull[attribute_index] = true;
	}
	TTSOpsVirtual.release(slot);
}

void
materialize_vector_tuple_slot(TupleTableSlot*)
{
	/* Every fixed-width lane is inline and every varlena lane is owned. */
}

void
copy_vector_tuple_slot(TupleTableSlot*, TupleTableSlot*)
{
	ereport(ERROR,
		(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		 errmsg("copying a vector batch slot is not supported")));
}

HeapTuple
copy_vector_heap_tuple(TupleTableSlot*)
{
	ereport(ERROR,
		(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		 errmsg("a vector batch slot cannot be converted to a heap tuple")));
	pg_unreachable();
}

MinimalTuple
copy_vector_minimal_tuple(TupleTableSlot*)
{
	ereport(ERROR,
		(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		 errmsg("a vector batch slot cannot be converted to a minimal tuple")));
	pg_unreachable();
}

[[nodiscard]] const TupleTableSlotOps*
vector_tuple_slot_ops()
{
	static const TupleTableSlotOps operations = [] {
		TupleTableSlotOps result = TTSOpsVirtual;
		result.base_slot_size = sizeof(vector_tuple_table_slot_storage);
		result.clear = clear_vector_tuple_slot;
		result.release = release_vector_tuple_slot;
		result.materialize = materialize_vector_tuple_slot;
		result.copyslot = copy_vector_tuple_slot;
		result.copy_heap_tuple = copy_vector_heap_tuple;
		result.copy_minimal_tuple = copy_vector_minimal_tuple;
		return result;
	}();
	return &operations;
}

}  // namespace

export [[nodiscard]] bool
is_vector_tuple_table_slot(const TupleTableSlot* slot) noexcept
{
	return slot != nullptr && slot->tts_ops == vector_tuple_slot_ops();
}

/** Allocate a vector slot with one owned column per tuple attribute. */
export TupleTableSlot*
create_vector_tuple_table_slot(TupleDesc tuple_desc)
{
	TupleTableSlot* slot =
		MakeTupleTableSlot(CreateTupleDescCopy(tuple_desc),
						   vector_tuple_slot_ops());

	TupleDesc slot_tuple_desc = slot->tts_tupleDescriptor;

	vector_tuple_table_slot_storage* vector_slot =
		reinterpret_cast<vector_tuple_table_slot_storage*>(slot);

	/* No rows are active before the first batch is populated. */
	std::fill_n(
		vector_slot->row_is_active, vector_column_capacity, false);

	for (int attribute_index = 0; attribute_index < slot_tuple_desc->natts; attribute_index++)
	{
		const Oid column_type_oid =
			TupleDescAttr(slot_tuple_desc, attribute_index)->atttypid;

		int16_t declared_type_width = 0;
		bool declared_stored_by_value = false;
		get_typlenbyval(
			column_type_oid,
			&declared_type_width,
			&declared_stored_by_value);

		const std::int16_t element_width = declared_type_width == -1
			? static_cast<std::int16_t>(sizeof(Datum))
			: declared_type_width;
		const vector_storage_kind storage_kind =
			declared_type_width == -1
				? vector_storage_kind::owned_varlena_datum
				: vector_storage_kind::inline_fixed;

		vector_column_storage* column = allocate_vector_column(
			element_width,
			declared_type_width == -1 || declared_stored_by_value,
			vector_slot->source_row_numbers,
			storage_kind,
			slot->tts_mcxt);

		vector_slot->virtual_slot.base.tts_values[attribute_index] =
			PointerGetDatum(column);
		vector_slot->virtual_slot.base.tts_isnull[attribute_index] = false;
	}

	vector_slot->virtual_slot.base.tts_nvalid = tuple_desc->natts;

	return slot;
}


/** Materialize selected attributes from one vector row into a scalar slot. */
export void
extract_tuple_from_vector_slot(
	TupleTableSlot* output_slot,
	vector_tuple_table_slot_storage* vector_slot,
	std::int32_t row_index,
	List* attr_needed_list)
{
	/* Only attributes_needed is materialized below, but ExecStoreVirtualTuple
	 * publishes every descriptor attribute as valid.  Establish canonical
	 * virtual-slot contents first so omitted columns are NULL rather than stale
	 * Datums left behind by an earlier row. */
	bioseq::postgres::support::pg_slot_datums_view{output_slot}.clear_all_to_null();
	output_slot->tts_nvalid = 0;
	foreach_int(attribute_number, attr_needed_list)
	{
		if (!TupleDescAttr(output_slot->tts_tupleDescriptor, attribute_number)->attisdropped)
		{
			vector_column_storage* column =
				reinterpret_cast<vector_column_storage*>(
					vector_slot->virtual_slot.base.tts_values[attribute_number]);
			std::int8_t* raw_column_data =
				reinterpret_cast<std::int8_t*>(column->data)
				+ column->element_width * row_index;
			output_slot->tts_values[attribute_number] = fetch_att(
				raw_column_data,
				column->stored_by_value,
				column->element_width);
			output_slot->tts_isnull[attribute_number] = column->is_null[row_index];
		}
	}

	ExecStoreVirtualTuple(output_slot);
}

/** Store one PostgreSQL datum in a vector column, copying owned varlena bytes. */
export void write_datum_to_vector_column(
	vector_column_storage* column,
	Datum input_datum,
	bool is_null,
	std::int32_t row_index)
{
	if (row_index < 0 ||
		static_cast<uint32>(row_index) >= vector_column_capacity)
		bioseq::postgres::support::invoke_pg_as_cpp(
			[row_index] {
				ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("vector row index %d exceeds the batch capacity",
							row_index)));
			});

	if (column->storage_kind == vector_storage_kind::owned_varlena_datum &&
		static_cast<uint32>(row_index) < column->lane_count &&
		!column->is_null[row_index] &&
		DatumGetPointer(column->data[row_index]) != nullptr)
	{
		bioseq::postgres::support::runtime_diagnostics::
			record_vector_payload_overwrite_free();
		pfree(DatumGetPointer(column->data[row_index]));
		column->data[row_index] = static_cast<Datum>(0);
	}
	column->is_null[row_index] = is_null;
	auto* write_position =
		reinterpret_cast<int8_t*>(column->data) + column->element_width * row_index;

	if (!is_null)
	{
		if (column->storage_kind == vector_storage_kind::owned_varlena_datum)
		{
			note_owned_varlena_source_form(input_datum);
			Pointer original_pointer = DatumGetPointer(input_datum);
			auto* varlena_source =
				bioseq::postgres::support::invoke_pg_as_cpp(
					[input_datum] {
						return PG_DETOAST_DATUM_PACKED(input_datum);
					});
			Size data_length = VARSIZE_ANY(varlena_source);
			void* owned_copy =
				bioseq::postgres::support::invoke_pg_as_cpp(
					[column, data_length] {
						return MemoryContextAlloc(
							column->storage_context,
							data_length);
			});
			std::memcpy(owned_copy, varlena_source, data_length);
			bioseq::postgres::support::runtime_diagnostics::
				record_vector_payload_allocation();
			if (reinterpret_cast<Pointer>(varlena_source) != original_pointer)
			{
				bioseq::postgres::support::runtime_diagnostics::
					record_detoast_temporary_free();
				pfree(varlena_source);
			}
			*reinterpret_cast<Datum*>(write_position) =
				PointerGetDatum(owned_copy);
		}
		else if (column->stored_by_value)
		{
			store_att_byval(write_position, input_datum, column->element_width);
		}
		else
		{
			std::memcpy(write_position, DatumGetPointer(input_datum),
				   column->element_width);
		}
	}
	else
	{
		std::memset(write_position, 0, column->element_width);
	}

	column->lane_count =
		std::max(column->lane_count, static_cast<uint32>(row_index + 1));
}

/** Append one scalar tuple to a vector batch at the requested row index. */
export void write_tuple_to_vector_slot(
	TupleTableSlot* input_slot,
	vector_tuple_table_slot_storage* vector_slot,
	std::int32_t row_index)
{
	TupleDesc tuple_desc = input_slot->tts_tupleDescriptor;

	for (int attribute_index = 0; attribute_index < tuple_desc->natts; attribute_index++)
	{
		vector_column_storage* column =
			reinterpret_cast<vector_column_storage*>(
				vector_slot->virtual_slot.base.tts_values[attribute_index]);
		write_datum_to_vector_column(
			column,
			input_slot->tts_values[attribute_index],
			input_slot->tts_isnull[attribute_index],
			row_index);
	}
}

/** Release owned lane payloads and reset a vector slot for the next batch. */
export void reset_vector_tuple_table_slot(
	vector_tuple_table_slot_storage* vector_slot)
{
	cleanup_vector_slot_contents(
		vector_slot,
		bioseq::postgres::support::runtime_diagnostics::
			vector_payload_cleanup_kind::explicit_cleanup);
}
