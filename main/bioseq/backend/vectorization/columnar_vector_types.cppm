/*-------------------------------------------------------------------------
 *
 * vectorization/columnar_vector_types.cppm
 *
 * Builds the vectorised TupleTableSlot variant used by the columnar custom
 * scan. A VectorTupleTableSlot stores one VectorColumn per attribute, each
 * of which packs up to COLUMNAR_VECTOR_COLUMN_SIZE values for batched
 * evaluation. The helpers here allocate slots, write incoming tuples into
 * them, extract scalar tuples out of them and reset them between batches.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include "columnar/vectorization/columnar_vector_types.h"

export module bioseq_vector_types;

import pg_utils;

extern "C++" {

/*
 * BuildVectorColumn allocates a single VectorColumn with value storage for
 * COLUMNAR_VECTOR_COLUMN_SIZE elements of the given type. columnIsVal
 * flags whether the column holds pass-by-value datums (inline) or
 * by-reference datums (pointer-to-copy via VARSIZE).
 */
VectorColumn*
BuildVectorColumn(int16 column_dimension, int16 column_type_length,
				  bool column_is_val, uint64* row_number)
{
	VectorColumn* vector_column = make0<VectorColumn>();

	vector_column->dimension = 0;
	vector_column->value = make1<Datum>(column_type_length * COLUMNAR_VECTOR_COLUMN_SIZE);
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
TupleTableSlot*
CreateVectorTupleTableSlot(TupleDesc tuple_desc)
{
	static TupleTableSlotOps tts_ops;
	tts_ops = TTSOpsVirtual;
	tts_ops.base_slot_size = sizeof(VectorTupleTableSlot);

	TupleTableSlot* slot = MakeTupleTableSlot(CreateTupleDescCopy(tuple_desc), &tts_ops);

	TupleDesc slot_tuple_desc = slot->tts_tupleDescriptor;

	VectorTupleTableSlot* vector_slot = reinterpret_cast<VectorTupleTableSlot*>(slot);

	/* All tuples should be skipped in initialization */
	memset(vector_slot->keep, false, COLUMNAR_VECTOR_COLUMN_SIZE);

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

		VectorColumn* vector_column = BuildVectorColumn(COLUMNAR_VECTOR_COLUMN_SIZE,
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
void
ExtractTupleFromVectorSlot(TupleTableSlot* output_slot, VectorTupleTableSlot* vector_slot,
						   int32_t row_index, List* attr_needed_list)
{
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
void
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
void
CleanupVectorSlot(VectorTupleTableSlot* vector_slot)
{
	TupleDesc tuple_desc = vector_slot->tts.tts_tupleDescriptor;

	for (int attribute_index = 0; attribute_index < tuple_desc->natts; attribute_index++)
	{
		VectorColumn* column = reinterpret_cast<VectorColumn*>(vector_slot->tts.tts_values[attribute_index]);
		memset(column->isnull, true, COLUMNAR_VECTOR_COLUMN_SIZE);
		column->dimension = 0;
	}

	memset(vector_slot->keep, true, COLUMNAR_VECTOR_COLUMN_SIZE);
	vector_slot->dimension = 0;
}

}
