/*-------------------------------------------------------------------------
 *
 * vectorization/columnar_vector_types.cppm
 *
 * Owns the vectorised data model used by the columnar custom scan and the
 * exported helpers that build/populate it. A VectorTupleTableSlot stores one
 * VectorColumn per attribute, each of which packs up to
 * BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE values for batched evaluation. The structs are
 * ABI-shared (reinterpret_cast across modules), so their field order/types
 * are guarded with file-local reference replicas. The helpers here allocate slots, write incoming
 * tuples into them, extract scalar tuples out of them and reset them between
 * batches.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

export module bioseq.vector.types;

import bioseq.pg_utils.datums;
import bioseq.pg_utils.error_bridge;
import bioseq.pg_utils.runtime_test_counters;
import bioseq.parallelism.memory_admission;

/*
 * Batch width: how many scalar rows a single VectorColumn / VectorTupleTableSlot
 * can hold. (Kept as BIOSEQ_COLUMN_* here; the columnar -> bioseq_column rename is a
 * later step.)
 */
export constexpr uint32_t BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE = 10000;

/*
 * Varlena values need ownership that is independent of the input slot. Keep
 * that distinction explicit instead of inferring it from columnIsVal: a
 * varlena lane contains an owned Datum pointer, while fixed-width values are
 * stored inline (using PostgreSQL's by-value flag when extracted).
 */
export enum class VectorStorageKind : uint8_t
{
	inline_fixed,
	owned_varlena_datum
};

export struct VectorColumn
{
	uint32	dimension;
	uint16	columnTypeLen;
	bool	columnIsVal;
	VectorStorageKind storageKind;
	MemoryContext storageContext;
	Datum	*value;
	bool	isnull[BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE];
	uint64	*rowNumber;

	/* Layout-neutral accessors (methods do not change the layout). */
	[[nodiscard]] std::span<bool> null_mask() noexcept { return {isnull, dimension}; }
	template <class T>
	[[nodiscard]] std::span<T> values() noexcept
	{
		return {reinterpret_cast<T*>(value), dimension};
	}
};

export struct VectorTupleTableSlot
{
	/* TupleTableSlot structure */
	TupleTableSlot tts;
	/* Storage used by inherited virtual-slot callbacks. */
	char* virtualData;
	/* How many tuples does this slot contain */
	uint32 dimension;
	/* Keep array to represent filtered tuples */
	bool keep[BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE];
	/* Row Number */
	uint64 rowNumber[BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE];
};

/*
 * The two enums are plain (untyped) C enums in the original header, i.e. of
 * width `int`. They are the first member of reinterpret_cast'd structs, so the
 * `: int` underlying type MUST be preserved (narrowing to uint8 would change
 * member offsets/size). They stay unscoped so the bare enumerator names the
 * call sites use remain valid. (The columnar -> bioseq_column rename is later.)
 */
export enum VectorArgType : int
{
	VECTOR_FN_ARG_CONSTANT,
	VECTOR_FN_ARG_VAR
};

export enum VectorQualType : int
{
	VECTOR_QUAL_BOOL_EXPR,
	VECTOR_QUAL_EXPR
};

export struct VectorFnArgument
{
	VectorArgType type;
	Datum arg;
};

export struct VectorQual
{
	VectorQualType vectorQualType;
	union
	{
		struct
		{
			FmgrInfo *fmgrInfo;
			FunctionCallInfo fcInfo;
			VectorFnArgument *vectorFnArguments;
		} expr;
		struct
		{
			BoolExprType boolExprType;
			List *vectorQualExprList;
		} boolExpr;
	} u;
};

/*
 * Aggregate transition states (moved from the deleted types/types.h). They are
 * reinterpret_cast from ARR_DATA_PTR / PG_GETARG_POINTER, so layout matters.
 */
export struct Int128AggState
{
	bool		calcSumX2;		/* if true, calculate sumX2 */
	int64		N;				/* count of processed numbers */
	int128		sumX;			/* sum of processed numbers */
	int128		sumX2;			/* sum of squares of processed numbers */
};

export struct Int64AggState
{
	int64		N;				/* count of processed numbers */
	int64		sumX;			/* sum of processed numbers */
};

/*
 * ABI guards: each reinterpret_cast'd struct is compared against a file-local
 * replica of the ORIGINAL C layout (same field order/types), so any accidental
 * layout drift (size or cross-cast offset) is a hard compile error without
 * needing a numeric baseline.
 */
namespace {

struct _vfa_ref { int type; Datum arg; };
static_assert(std::is_standard_layout_v<VectorFnArgument>);
static_assert(sizeof(VectorFnArgument) == sizeof(_vfa_ref));
static_assert(offsetof(VectorFnArgument, arg) == offsetof(_vfa_ref, arg));

struct _vc_ref
{
	uint32	dimension;
	uint16	columnTypeLen;
	bool	columnIsVal;
	VectorStorageKind storageKind;
	MemoryContext storageContext;
	Datum	*value;
	bool	isnull[BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE];
	uint64	*rowNumber;
};
static_assert(std::is_standard_layout_v<VectorColumn>);
static_assert(sizeof(VectorColumn) == sizeof(_vc_ref));
static_assert(offsetof(VectorColumn, value) == offsetof(_vc_ref, value));
static_assert(offsetof(VectorColumn, isnull) == offsetof(_vc_ref, isnull));
static_assert(offsetof(VectorColumn, rowNumber) == offsetof(_vc_ref, rowNumber));

struct _vtts_ref
{
	TupleTableSlot tts;
	char* virtualData;
	uint32 dimension;
	bool keep[BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE];
	uint64 rowNumber[BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE];
};
static_assert(std::is_standard_layout_v<VectorTupleTableSlot>);
static_assert(sizeof(VectorTupleTableSlot) == sizeof(_vtts_ref));
static_assert(
	offsetof(VectorTupleTableSlot, virtualData) ==
	offsetof(VirtualTupleTableSlot, data));
static_assert(offsetof(VectorTupleTableSlot, dimension) == offsetof(_vtts_ref, dimension));
static_assert(offsetof(VectorTupleTableSlot, keep) == offsetof(_vtts_ref, keep));
static_assert(offsetof(VectorTupleTableSlot, rowNumber) == offsetof(_vtts_ref, rowNumber));

}  // namespace

/*
 * BuildVectorColumn allocates a single VectorColumn with value storage for
 * BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE elements of the given type.
 */
export VectorColumn*
BuildVectorColumn(int16 column_dimension, int16 column_type_length,
				  bool column_is_val, uint64* row_number,
				  VectorStorageKind storage_kind = VectorStorageKind::inline_fixed,
				  MemoryContext storage_context = CurrentMemoryContext)
{
	VectorColumn* vector_column =
		static_cast<VectorColumn*>(palloc0(sizeof(VectorColumn)));

	vector_column->dimension = 0;
	vector_column->value = static_cast<Datum*>(palloc0(
		bioseq::parallelism::saturating_mul(
			static_cast<std::uint64_t>(column_type_length),
			BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE)));
	vector_column->columnTypeLen = column_type_length;
	vector_column->columnIsVal = column_is_val;
	vector_column->storageKind = storage_kind;
	vector_column->storageContext = storage_context;
	vector_column->rowNumber = row_number;

	return vector_column;
}

namespace {

void
note_owned_varlena_source_form(Datum source) noexcept
{
	if (!bioseq::pg_utils::runtime_test_counters::
			bpchar_source_observation_active())
		return;

	const Pointer raw = DatumGetPointer(source);
	using enum bioseq::pg_utils::runtime_test_counters::
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
	bioseq::pg_utils::runtime_test_counters::
		note_bpchar_source_form(
			form,
			bioseq::pg_utils::runtime_test_counters::
				bpchar_source_site::vector_copy);
}

void
cleanup_vector_slot_contents(
	VectorTupleTableSlot* vector_slot,
	bioseq::pg_utils::runtime_test_counters::
		vector_payload_cleanup_kind cleanup_kind)
{
	bioseq::pg_utils::runtime_test_counters::
		note_vector_cleanup_call(cleanup_kind);
	TupleDesc tuple_desc = vector_slot->tts.tts_tupleDescriptor;
	if (tuple_desc == nullptr)
		return;

	for (int attribute_index = 0; attribute_index < tuple_desc->natts;
		 ++attribute_index)
	{
		VectorColumn* column =
			reinterpret_cast<VectorColumn*>(
				DatumGetPointer(vector_slot->tts.tts_values[attribute_index]));
		if (column == nullptr)
			continue;

		if (column->storageKind == VectorStorageKind::owned_varlena_datum)
		{
			for (uint32 row_index = 0; row_index < column->dimension; ++row_index)
			{
				Datum& datum = column->value[row_index];
				if (!column->isnull[row_index] &&
					DatumGetPointer(datum) != nullptr)
				{
					bioseq::pg_utils::runtime_test_counters::
						note_vector_payload_cleanup_free(cleanup_kind);
					pfree(DatumGetPointer(datum));
				}
				datum = static_cast<Datum>(0);
			}
		}
		memset(column->isnull, true, BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE);
		column->dimension = 0;
	}

	memset(vector_slot->keep, true, BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE);
	vector_slot->dimension = 0;
}

void
clear_vector_tuple_slot(TupleTableSlot* slot)
{
	cleanup_vector_slot_contents(
		reinterpret_cast<VectorTupleTableSlot*>(slot),
		bioseq::pg_utils::runtime_test_counters::
			vector_payload_cleanup_kind::slot_clear);
	TTSOpsVirtual.clear(slot);
}

void
release_vector_tuple_slot(TupleTableSlot* slot)
{
	auto* vector_slot = reinterpret_cast<VectorTupleTableSlot*>(slot);
	cleanup_vector_slot_contents(
		vector_slot,
		bioseq::pg_utils::runtime_test_counters::
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
		VectorColumn* column =
			reinterpret_cast<VectorColumn*>(
				DatumGetPointer(slot->tts_values[attribute_index]));
		if (column == nullptr)
			continue;
		pfree(column->value);
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
		result.base_slot_size = sizeof(VectorTupleTableSlot);
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
IsVectorTupleTableSlot(const TupleTableSlot* slot) noexcept
{
	return slot != nullptr && slot->tts_ops == vector_tuple_slot_ops();
}

/*
 * CreateVectorTupleTableSlot allocates a TupleTableSlot whose body is a
 * VectorTupleTableSlot. One VectorColumn is created per attribute; columns
 * with typlen == -1 (varlena) are stored as owned Datum pointers.
 */
export TupleTableSlot*
CreateVectorTupleTableSlot(TupleDesc tuple_desc)
{
	TupleTableSlot* slot =
		MakeTupleTableSlot(CreateTupleDescCopy(tuple_desc),
						   vector_tuple_slot_ops());

	TupleDesc slot_tuple_desc = slot->tts_tupleDescriptor;

	VectorTupleTableSlot* vector_slot = reinterpret_cast<VectorTupleTableSlot*>(slot);

	/* All tuples should be skipped in initialization */
	memset(vector_slot->keep, false, BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE);

	for (int attribute_index = 0; attribute_index < slot_tuple_desc->natts; attribute_index++)
	{
		Oid column_type_oid = TupleDescAttr(slot_tuple_desc, attribute_index)->atttypid;

		int16_t column_type_length = 0;
		bool column_type_by_value = false;
		get_typlenbyval(column_type_oid, &column_type_length, &column_type_by_value);

		int16_t vector_column_type_length =
			column_type_length == -1 ? sizeof(Datum) : column_type_length;
		const VectorStorageKind storage_kind =
			column_type_length == -1
				? VectorStorageKind::owned_varlena_datum
				: VectorStorageKind::inline_fixed;

		VectorColumn* vector_column = BuildVectorColumn(BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE,
														vector_column_type_length,
														column_type_length == -1 ||
															column_type_by_value,
														vector_slot->rowNumber,
														storage_kind,
														slot->tts_mcxt);

		vector_slot->tts.tts_values[attribute_index] = PointerGetDatum(vector_column);
		vector_slot->tts.tts_isnull[attribute_index] = false;
	}

	vector_slot->tts.tts_nvalid = tuple_desc->natts;

	return slot;
}


/*
 * ExtractTupleFromVectorSlot materialises a single scalar tuple (at the
 * given index) from a vector slot into the supplied output slot. Only
 * attributes in attrNeededList are touched.
 */
export void
ExtractTupleFromVectorSlot(TupleTableSlot* output_slot, VectorTupleTableSlot* vector_slot,
						   int32_t row_index, List* attr_needed_list)
{
	/* Only attrNeededList is materialized below, but ExecStoreVirtualTuple
	 * publishes every descriptor attribute as valid.  Establish canonical
	 * virtual-slot contents first so omitted columns are NULL rather than stale
	 * Datums left behind by an earlier row. */
	bioseq::pg_utils::pg_slot_datums_view{output_slot}.clear_all_to_null();
	output_slot->tts_nvalid = 0;
	foreach_int(attribute_number, attr_needed_list)
	{
		if (!TupleDescAttr(output_slot->tts_tupleDescriptor, attribute_number)->attisdropped)
		{
			VectorColumn* column = reinterpret_cast<VectorColumn*>(vector_slot->tts.tts_values[attribute_number]);
			int8_t* raw_column_data = reinterpret_cast<int8_t*>(column->value) + column->columnTypeLen * row_index;
			output_slot->tts_values[attribute_number] = fetch_att(raw_column_data, column->columnIsVal, column->columnTypeLen);
			output_slot->tts_isnull[attribute_number] = column->isnull[row_index];
		}
	}

	ExecStoreVirtualTuple(output_slot);
}

export void
WriteDatumToVectorColumn(VectorColumn* column, Datum input_datum,
						 bool is_null, int32_t row_index)
{
	if (row_index < 0 ||
		static_cast<uint32>(row_index) >= BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE)
		bioseq::pg_utils::invoke_pg_as_cpp(
			[row_index] {
				ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("vector row index %d exceeds the batch capacity",
							row_index)));
			});

	if (column->storageKind == VectorStorageKind::owned_varlena_datum &&
		static_cast<uint32>(row_index) < column->dimension &&
		!column->isnull[row_index] &&
		DatumGetPointer(column->value[row_index]) != nullptr)
	{
		bioseq::pg_utils::runtime_test_counters::
			note_vector_payload_overwrite_free();
		pfree(DatumGetPointer(column->value[row_index]));
		column->value[row_index] = static_cast<Datum>(0);
	}
	column->isnull[row_index] = is_null;
	auto* write_position =
		reinterpret_cast<int8_t*>(column->value) + column->columnTypeLen * row_index;

	if (!is_null)
	{
		if (column->storageKind == VectorStorageKind::owned_varlena_datum)
		{
			note_owned_varlena_source_form(input_datum);
			Pointer original_pointer = DatumGetPointer(input_datum);
			auto* varlena_source =
				bioseq::pg_utils::invoke_pg_as_cpp(
					[input_datum] {
						return PG_DETOAST_DATUM_PACKED(input_datum);
					});
			Size data_length = VARSIZE_ANY(varlena_source);
			void* owned_copy =
				bioseq::pg_utils::invoke_pg_as_cpp(
					[column, data_length] {
						return MemoryContextAlloc(
							column->storageContext,
							data_length);
			});
			memcpy(owned_copy, varlena_source, data_length);
			bioseq::pg_utils::runtime_test_counters::
				note_vector_payload_allocation();
			if (reinterpret_cast<Pointer>(varlena_source) != original_pointer)
			{
				bioseq::pg_utils::runtime_test_counters::
					note_detoast_temporary_free();
				pfree(varlena_source);
			}
			*reinterpret_cast<Datum*>(write_position) =
				PointerGetDatum(owned_copy);
		}
		else if (column->columnIsVal)
		{
			store_att_byval(write_position, input_datum, column->columnTypeLen);
		}
		else
		{
			memcpy(write_position, DatumGetPointer(input_datum),
				   column->columnTypeLen);
		}
	}
	else
	{
		memset(write_position, 0, column->columnTypeLen);
	}

	column->dimension =
		std::max(column->dimension, static_cast<uint32>(row_index + 1));
}

/*
 * WriteTupleToVectorSlot appends a single scalar tuple (from the input
 * slot) into the vector slot at the given index, writing each attribute
 * into its VectorColumn. Varlena attributes are copied into freshly
 * palloc'd buffers so the vector slot owns their storage.
 */
export void
WriteTupleToVectorSlot(TupleTableSlot* input_slot, VectorTupleTableSlot* vector_slot,
					   int32_t row_index)
{
	TupleDesc tuple_desc = input_slot->tts_tupleDescriptor;

	for (int attribute_index = 0; attribute_index < tuple_desc->natts; attribute_index++)
	{
		VectorColumn* column =
			reinterpret_cast<VectorColumn*>(
				vector_slot->tts.tts_values[attribute_index]);
		WriteDatumToVectorColumn(
			column,
			input_slot->tts_values[attribute_index],
			input_slot->tts_isnull[attribute_index],
			row_index);
	}
}

/*
 * CleanupVectorSlot resets a vector slot between batches: every column is
 * marked all-null and dimension is zeroed so the next write sequence
 * starts at index 0. The keep[] mask is initialised to true so downstream
 * qual evaluation starts from a fresh "all rows alive" state.
 */
export void
CleanupVectorSlot(VectorTupleTableSlot* vector_slot)
{
	cleanup_vector_slot_contents(
		vector_slot,
		bioseq::pg_utils::runtime_test_counters::
			vector_payload_cleanup_kind::explicit_cleanup);
}
