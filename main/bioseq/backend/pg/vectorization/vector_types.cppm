/*-------------------------------------------------------------------------
 *
 * vectorization/columnar_vector_types.cppm
 *
 * Owns the vectorised data model used by the columnar custom scan and the
 * exported helpers that build/populate it. A VectorTupleTableSlot stores one
 * VectorColumn per attribute, each of which packs up to
 * BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE values for batched evaluation. The structs are
 * ABI-shared (reinterpret_cast across modules), so their field order/types
 * are preserved bit-for-bit and guarded with file-local reference replicas of
 * the original C layout. The helpers here allocate slots, write incoming
 * tuples into them, extract scalar tuples out of them and reset them between
 * batches.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

export module bioseq.vector.types;

import bioseq.pg_utils;
import bioseq.parallelism.memory_admission;

/*
 * Batch width: how many scalar rows a single VectorColumn / VectorTupleTableSlot
 * can hold. (Kept as BIOSEQ_COLUMN_* here; the columnar -> bioseq_column rename is a
 * later step.)
 */
export constexpr uint32_t BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE = 10000;

/*
 * VectorColumn holds up to BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE values of one attribute.
 * columnIsVal flags whether the column holds pass-by-value datums (inline) or
 * by-reference datums (pointer-to-copy via VARSIZE). The layout is ABI-shared
 * (reinterpret_cast from the executor + from the compare UDFs).
 */
export struct VectorColumn
{
	uint32	dimension;
	uint16	columnTypeLen;
	bool	columnIsVal;
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
	uint32 dimension;
	bool keep[BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE];
	uint64 rowNumber[BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE];
};
static_assert(std::is_standard_layout_v<VectorTupleTableSlot>);
static_assert(sizeof(VectorTupleTableSlot) == sizeof(_vtts_ref));
static_assert(offsetof(VectorTupleTableSlot, dimension) == offsetof(_vtts_ref, dimension));
static_assert(offsetof(VectorTupleTableSlot, keep) == offsetof(_vtts_ref, keep));
static_assert(offsetof(VectorTupleTableSlot, rowNumber) == offsetof(_vtts_ref, rowNumber));

}  // namespace

/*
 * BuildVectorColumn allocates a single VectorColumn with value storage for
 * BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE elements of the given type. columnIsVal
 * flags whether the column holds pass-by-value datums (inline) or
 * by-reference datums (pointer-to-copy via VARSIZE).
 */
export VectorColumn*
BuildVectorColumn(int16 column_dimension, int16 column_type_length,
				  bool column_is_val, uint64* row_number)
{
	VectorColumn* vector_column = make0<VectorColumn>();

	vector_column->dimension = 0;
	vector_column->value = make1<Datum>(bioseq::parallelism::saturating_mul(
		static_cast<std::uint64_t>(column_type_length), BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE));
	vector_column->columnTypeLen = column_type_length;
	vector_column->columnIsVal = column_is_val;
	vector_column->rowNumber = row_number;

	return vector_column;
}

/*
 * CreateVectorTupleTableSlot allocates a TupleTableSlot whose body is a
 * VectorTupleTableSlot. One VectorColumn is created per attribute; columns
 * with typlen == -1 (varlena) are stored as Datum pointers, as configured
 * by the columnIsVal flag.
 */
export TupleTableSlot*
CreateVectorTupleTableSlot(TupleDesc tuple_desc)
{
	static TupleTableSlotOps tts_ops;
	tts_ops = TTSOpsVirtual;
	tts_ops.base_slot_size = sizeof(VectorTupleTableSlot);

	TupleTableSlot* slot = MakeTupleTableSlot(CreateTupleDescCopy(tuple_desc), &tts_ops);

	TupleDesc slot_tuple_desc = slot->tts_tupleDescriptor;

	VectorTupleTableSlot* vector_slot = reinterpret_cast<VectorTupleTableSlot*>(slot);

	/* All tuples should be skipped in initialization */
	memset(vector_slot->keep, false, BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE);

	for (int attribute_index = 0; attribute_index < slot_tuple_desc->natts; attribute_index++)
	{
		Oid column_type_oid = TupleDescAttr(slot_tuple_desc, attribute_index)->atttypid;

		int16_t column_type_length = get_typlen(column_type_oid);

		int16_t vector_column_type_length =
			column_type_length == -1 ? sizeof(Datum) : column_type_length;

		/*
		 * For typlen == -1 we store a pointer to a freshly allocated VARLEN
		 * copy, so treat it as pass-by-value at the VectorColumn layer.
		 */
		bool vector_column_is_val = vector_column_type_length <= sizeof(Datum);

		VectorColumn* vector_column = BuildVectorColumn(BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE,
														vector_column_type_length,
														vector_column_is_val,
														vector_slot->rowNumber);

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
		VectorColumn* column = reinterpret_cast<VectorColumn*>(vector_slot->tts.tts_values[attribute_index]);

		if (!input_slot->tts_isnull[attribute_index])
		{
			column->isnull[column->dimension] = false;

			if (column->columnIsVal)
			{
				int8_t* write_column_row_position = reinterpret_cast<int8_t*>(column->value) + column->columnTypeLen * row_index;

				store_att_byval(write_column_row_position, input_slot->tts_values[attribute_index], column->columnTypeLen);
			}
			else
			{
				Pointer varlena_source = DatumGetPointer(input_slot->tts_values[attribute_index]);

				Size data_length = VARSIZE_ANY(varlena_source);

				Datum* varlena_container = make1<Datum>(data_length);
				memcpy(varlena_container, varlena_source, data_length);

				*reinterpret_cast<Datum*>(reinterpret_cast<int8_t*>(column->value) + column->columnTypeLen * row_index) =
					PointerGetDatum(varlena_container);
			}
		}

		column->dimension++;
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
	TupleDesc tuple_desc = vector_slot->tts.tts_tupleDescriptor;

	for (int attribute_index = 0; attribute_index < tuple_desc->natts; attribute_index++)
	{
		VectorColumn* column = reinterpret_cast<VectorColumn*>(vector_slot->tts.tts_values[attribute_index]);
		memset(column->isnull, true, BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE);
		column->dimension = 0;
	}

	memset(vector_slot->keep, true, BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE);
	vector_slot->dimension = 0;
}
