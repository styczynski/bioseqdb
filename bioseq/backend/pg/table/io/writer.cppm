/**
 * @file
 * @brief Buffered stripe writer for the Bioseq column format.
 *
 * Owns the
 * column_write_state lifecycle: in-memory chunk accumulation, chunk
 * compression, stripe assembly, file-level metadata writes (skip nodes,
 * chunk-group summaries) and the WAL-emitting flush path.
 */
module;
#include <bioseq/pg_compat.h>

#include "safe_lib.h"

export module bioseq.postgres.table.io:writer;

import bioseq.common.logging;
import bioseq.postgres.storage.compression;
import bioseq.postgres.storage.physical;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.configuration.core;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.postgres.types.compact_sequence;
import :buffers;

/** @brief Flush any buffered rows for a column-table writer. */
export void flush_pending_column_writes(column_write_state *state);


/** @brief Allocate buffers for a stripe's configured row and chunk limits. */
static stripe_buffers * allocate_stripe_buffers(uint32_t stripe_max_row_count,
												uint32_t chunk_group_row_limit,
												uint32_t column_count);
/** @brief Allocate skip metadata for a stripe's configured chunk capacity. */
static stripe_skip_list * allocate_stripe_skip_list(uint32_t stripe_max_row_count,
												  uint32_t chunk_group_row_limit,
												  uint32_t column_count);
/** @brief Persist the writer's current stripe and its metadata. */
static void flush_stripe(column_write_state *write_state);
/** @brief Pack Boolean values into a serialized bitmap. */
static StringInfo serialize_boolean_array(bool *boolean_values, uint32_t boolean_count);
/** @brief Serialize one completed chunk group's presence and value streams. */
static void serialize_chunk_data(column_write_state *write_state, uint32_t chunk_index,
							   uint32_t row_count);
/** @brief Update one chunk's extrema from a non-null column value. */
static void update_chunk_min_max(chunk_skip_node *chunk_skip_node,
									  Datum column_value, bool column_type_by_value,
									  int column_type_length, Oid column_collation,
									  FmgrInfo *comparison_function);
/** @brief Make a deep copy of a serialized StringInfo buffer. */
static StringInfo copy_string_info(StringInfo source_buffer);

/*
 * begin_column_write initializes a columnar data load operation and returns a table
 * handle. This handle should be used for adding the row values and finishing the
 * data load operation.
 */
/** @brief Create buffered write state for a column-table relation. */
export column_write_state *
begin_column_write(RelFileLocator relation_file_locator,
				   column_options options,
				   TupleDesc tuple_descriptor)
{
	/* get comparison function pointers for each of the columns */
	uint32_t column_count = tuple_descriptor->natts;
	const Oid sequence_type_oid = bioseq_seq_type_oid_missing_ok();
	FmgrInfo **comparison_functions = bioseq::postgres::support::allocate_zeroed_array<FmgrInfo*>(column_count);
	for (uint32_t column_index = 0; column_index < column_count; column_index++)
	{
		FmgrInfo *comparison_function = NULL;
		FormData_pg_attribute *attribute = TupleDescAttr(tuple_descriptor,
															 column_index);

		if (!attribute->attisdropped
			&& (!OidIsValid(sequence_type_oid)
				|| attribute->atttypid != sequence_type_oid))
		{
			Oid type_oid = attribute->atttypid;

			comparison_function = find_index_support_function(type_oid, BTREE_AM_OID,
													   BTORDER_PROC);
		}

		comparison_functions[column_index] = comparison_function;
	}

	/*
	 * We allocate all stripe specific data in the stripe_write_context, and
	 * reset this memory context once we have flushed the stripe to the file.
	 * This is to avoid memory leaks.
	 */
	MemoryContext stripe_write_context = AllocSetContextCreate(CurrentMemoryContext,
															 "Stripe Write Memory Context",
															 ALLOCSET_DEFAULT_SIZES);

	bool *column_mask = bioseq::postgres::support::allocate_zeroed_array<bool>(column_count);
	memset(column_mask, true, column_count * sizeof(bool));

	chunk_data *pending_chunk = allocate_chunk_data(column_count, column_mask,
												options.chunk_group_row_limit);

	column_write_state *write_state = bioseq::postgres::support::allocate_zeroed_object<column_write_state>();
	write_state->relation_file_locator = relation_file_locator;
	write_state->options = options;
	write_state->tuple_descriptor = CreateTupleDescCopy(tuple_descriptor);
	write_state->comparison_functions = comparison_functions;
	write_state->buffers = NULL;
	write_state->skip_list = NULL;
	write_state->stripe_reservation = NULL;
	write_state->stripe_write_context = stripe_write_context;
	write_state->pending_chunk = pending_chunk;
	write_state->compression_buffer = NULL;
	write_state->tuple_context = AllocSetContextCreate(CurrentMemoryContext,
														"BioseqColumn per tuple context",
														ALLOCSET_DEFAULT_SIZES);

	return write_state;
}

/*
 * write_column_row adds a row to the columnar table. If the stripe is not initialized,
 * we create structures to hold stripe data and skip list. Then, we serialize and
 * append data to serialized value buffer for each of the columns and update
 * corresponding skip nodes. Then, whole chunk data is compressed at every
 * rowChunkCount insertion. Then, if row count exceeds stripe_max_row_count, we flush
 * the stripe, and add its metadata to the table footer.
 *
 * Returns the "row number" assigned to written row.
 */
/** @brief Append one row while the writer's stripe context is active. */
static uint64_t
write_column_row_pg_leaf(
	column_write_state *write_state,
	Datum *column_values,
	bool *column_nulls)
{
	uint32_t column_index = 0;
	stripe_buffers *buffers = write_state->buffers;
	stripe_skip_list *skip_list = write_state->skip_list;
	uint32_t column_count = write_state->tuple_descriptor->natts;
	column_options *options = &write_state->options;
	const uint32_t chunk_group_row_limit = options->chunk_group_row_limit;
	chunk_data *pending_chunk = write_state->pending_chunk;
	uint32_t chunk_index;
	uint32_t chunk_row_index;

	if (buffers)
	{
		chunk_index = buffers->row_count / chunk_group_row_limit;
		chunk_row_index = buffers->row_count % chunk_group_row_limit;
		/*
		* For each column, we first need to check to see if the next row will fit
		* inside the chunk buffer.  If it does not fit, then we need to serialize
		* the stripe and make a new stripe for insertion.
		*/
		bool fits = true;

		for (column_index = 0; column_index < column_count; column_index++)
		{

			/* Check for nulls, skip if null. */
			if (column_nulls[column_index])
			{
				continue;
			}

			Form_pg_attribute attribute =
				TupleDescAttr(write_state->tuple_descriptor, column_index);

			int column_type_length = attribute->attlen;
			char column_type_alignment = attribute->attalign;

			uint32_t datum_length = att_addlength_datum(0, column_type_length, column_values[column_index]);
			uint32_t aligned_datum_length = att_align_nominal(datum_length, column_type_alignment);

			/* Check to see if we are within the 1 gigabyte value. */
			if (static_cast<long>(pending_chunk->referenced_value_buffers[column_index]->len) + static_cast<long>(aligned_datum_length) > 1024000000)
			{
				fits = false;
				break;
			}
		}

		if (!fits)
		{
			/* Flush the stripe. */
			flush_pending_column_writes(write_state);

			/* Then set up for new buffers. */
			buffers = NULL;

			pending_chunk->row_count = 0;
		}
	}

	if (buffers == NULL)
	{
		buffers = allocate_stripe_buffers(options->stripe_row_limit,
												 chunk_group_row_limit, column_count);
		skip_list = allocate_stripe_skip_list(options->stripe_row_limit,
												   chunk_group_row_limit, column_count);
		write_state->buffers = buffers;
		write_state->skip_list = skip_list;
		write_state->compression_buffer = makeStringInfo();

		Oid relation_id = RelidByRelfilenumber(write_state->relation_file_locator.spcOid,
											  write_state->relation_file_locator.relNumber);
		Relation relation = bioseq::postgres::support::open_postgres_relation(relation_id, NoLock);
		write_state->stripe_reservation =
			reserve_empty_stripe(relation, column_count, chunk_group_row_limit,
							   options->stripe_row_limit);
		relation_close(relation, NoLock);

		/*
		 * serialized_value_buffer lives in stripe write memory context so it needs to be
		 * initialized when the stripe is created.
		 */
		for (column_index = 0; column_index < column_count; column_index++)
		{
			cached_string_info *value = bioseq::postgres::support::allocate_zeroed_object<cached_string_info>();
			initStringInfo(&value->data);
			value->cached = false;
			pending_chunk->referenced_value_buffers[column_index] = reinterpret_cast<StringInfo>(value);
		}
	}

	chunk_index = buffers->row_count / chunk_group_row_limit;
	chunk_row_index = buffers->row_count % chunk_group_row_limit;

	for (column_index = 0; column_index < column_count; column_index++)
	{
		chunk_skip_node **chunk_skip_nodes = skip_list->chunk_skip_nodes;
		chunk_skip_node *chunk_skip_node =
			&chunk_skip_nodes[column_index][chunk_index];

		if (column_nulls[column_index])
		{
			pending_chunk->present_values[column_index][chunk_row_index] = false;
		}
		else
		{
			FmgrInfo *comparison_function =
				write_state->comparison_functions[column_index];
			Form_pg_attribute attribute =
				TupleDescAttr(write_state->tuple_descriptor, column_index);
			bool column_type_by_value = attribute->attbyval;
			int column_type_length = attribute->attlen;
			Oid column_collation = attribute->attcollation;
			char column_type_alignment = attribute->attalign;

			pending_chunk->present_values[column_index][chunk_row_index] = true;

			uint32_t datum_length = att_addlength_datum(0, column_type_length, column_values[column_index]);
			uint32_t aligned_datum_length = att_align_nominal(datum_length, column_type_alignment);

			if (aligned_datum_length >= (256UL << 20))
			{
				bioseq::postgres::support::raise_sql_error(
					ERRCODE_INTERNAL_ERROR,
					{
						.message = fmt::format(
							"Error with insert on column '{}'. Inserting "
							"{} bytes, exceeding 256MB",
							attribute->attname.data,
							datum_length),
					});
			}

			bioseq::postgres::support::pg_datum::from_raw(column_values[column_index])
				.append_serialized(pending_chunk->referenced_value_buffers[column_index],
								   column_type_by_value, column_type_length,
								   column_type_alignment);

			update_chunk_min_max(chunk_skip_node, column_values[column_index],
									  column_type_by_value, column_type_length,
									  column_collation, comparison_function);
		}

		chunk_skip_node->row_count++;
	}

	skip_list->chunk_count = chunk_index + 1;

	/* last row of the chunk is inserted serialize the chunk */
	if (chunk_row_index == chunk_group_row_limit - 1)
	{
		serialize_chunk_data(write_state, chunk_index, chunk_group_row_limit);
	}

	uint64_t written_row_number = write_state->stripe_reservation->stripe_first_row_number +
							  buffers->row_count;
	buffers->row_count++;
	if (buffers->row_count >= options->stripe_row_limit)
	{
		flush_pending_column_writes(write_state);
	}

	return written_row_number;
}


/** @brief Append a row and return its assigned relation row number. */
export uint64_t
write_column_row(
	column_write_state *write_state,
	Datum *column_values,
	bool *column_nulls)
{
	return bioseq::postgres::support::invoke_pg_as_cpp([write_state, column_values, column_nulls] {
		const MemoryContext previous =
			MemoryContextSwitchTo(write_state->stripe_write_context);
		const uint64_t result = write_column_row_pg_leaf(
			write_state, column_values, column_nulls);
		MemoryContextSwitchTo(previous);
		return result;
	});
}


/*
 * end_column_write finishes a columnar data load operation. If we have an unflushed
 * stripe, we flush it.
 */
/** @brief Flush and release a column-table writer. */
export void
end_column_write(column_write_state *write_state)
{
	/*
	 * A checked PG leaf restores the MemoryContext that was current when it
	 * entered.  Never let a caller delete stripe_write_context while executing
	 * inside that context (or one of its children), otherwise the next checked
	 * leaf restores a dangling allocator.  Move to the owning parent first.
	 */
	MemoryContext active = CurrentMemoryContext;
	MemoryContext walker = active;
	while (walker != nullptr && walker != write_state->stripe_write_context)
	{
		walker = MemoryContextGetParent(walker);
	}
	if (walker == write_state->stripe_write_context)
	{
		MemoryContextSwitchTo(
			MemoryContextGetParent(write_state->stripe_write_context));
	}
	flush_pending_column_writes(write_state);

	MemoryContextDelete(write_state->stripe_write_context);
	pfree(write_state->comparison_functions);
	free_chunk_data(write_state->pending_chunk);
	pfree(write_state);
}


/** @brief Flush buffered rows from within PostgreSQL's protected boundary. */
static void
flush_pending_column_writes_pg_leaf(column_write_state *write_state)
{
	stripe_buffers *buffers = write_state->buffers;
	if (buffers != NULL)
	{
		flush_stripe(write_state);
		MemoryContextReset(write_state->stripe_write_context);

		/* set stripe data and skip list to NULL so they are recreated next time */
		write_state->buffers = NULL;
		write_state->skip_list = NULL;
	}
}


/** @brief Flush buffered rows while preserving the caller's memory context. */
void
flush_pending_column_writes(column_write_state *write_state)
{
	bioseq::postgres::support::invoke_pg_as_cpp([write_state] {
		const MemoryContext previous =
			MemoryContextSwitchTo(write_state->stripe_write_context);
		flush_pending_column_writes_pg_leaf(write_state);
		MemoryContextSwitchTo(previous);
	});
}


/*
 * get_column_write_per_tuple_context
 *
 * Return per-tuple context for columnar write operation.
 */
/** @brief Return the writer's per-tuple scratch memory context. */
export MemoryContext
get_column_write_per_tuple_context(column_write_state *state)
{
	return state->tuple_context;
}


/*
 * allocate_stripe_buffers allocates an empty stripe_buffers structure with the given
 * column count.
 */
static stripe_buffers *
allocate_stripe_buffers(uint32_t stripe_max_row_count, uint32_t chunk_group_row_limit,
						 uint32_t column_count)
{
	uint32_t column_index = 0;
	uint32_t maximum_chunk_count = (stripe_max_row_count / chunk_group_row_limit) + 1;
	column_buffers **columns = bioseq::postgres::support::allocate_zeroed_array<column_buffers*>(column_count);

	for (column_index = 0; column_index < column_count; column_index++)
	{
		uint32_t chunk_index = 0;
		chunk_buffers **chunks =
			bioseq::postgres::support::allocate_zeroed_array<chunk_buffers*>(maximum_chunk_count);

		for (chunk_index = 0; chunk_index < maximum_chunk_count; chunk_index++)
		{
			chunks[chunk_index] = bioseq::postgres::support::allocate_zeroed_object<chunk_buffers>();
			chunks[chunk_index]->presence_buffer = NULL;
			chunks[chunk_index]->value_buffer = NULL;
			chunks[chunk_index]->value_compression_algorithm = bioseq::compression_algorithm::none;
		}

		columns[column_index] = bioseq::postgres::support::allocate_zeroed_object<column_buffers>();
		columns[column_index]->chunks = chunks;
	}

	stripe_buffers *buffers = bioseq::postgres::support::allocate_zeroed_object<stripe_buffers>();
	buffers->columns = columns;
	buffers->column_count = column_count;
	buffers->row_count = 0;

	return buffers;
}


/*
 * allocate_stripe_skip_list allocates an empty stripe_skip_list structure with
 * the given column count. This structure has enough chunks to hold statistics
 * for stripe_max_row_count rows.
 */
static stripe_skip_list *
allocate_stripe_skip_list(uint32_t stripe_max_row_count, uint32_t chunk_group_row_limit,
						  uint32_t column_count)
{
	uint32_t column_index = 0;
	uint32_t maximum_chunk_count = (stripe_max_row_count / chunk_group_row_limit) + 1;

	chunk_skip_node **chunk_skip_nodes =
		bioseq::postgres::support::allocate_zeroed_array<chunk_skip_node*>(column_count);
	for (column_index = 0; column_index < column_count; column_index++)
	{
		chunk_skip_nodes[column_index] =
			bioseq::postgres::support::allocate_zeroed_array<chunk_skip_node>(maximum_chunk_count);
	}

	stripe_skip_list *skip_list = bioseq::postgres::support::allocate_zeroed_object<stripe_skip_list>();
	skip_list->column_count = column_count;
	skip_list->chunk_count = 0;
	skip_list->chunk_skip_nodes = chunk_skip_nodes;

	return skip_list;
}


/*
 * flush_stripe flushes current stripe data into the file. The function first ensures
 * the last data chunk for each column is properly serialized and compressed. Then,
 * the function creates the skip list and footer buffers. Finally, the function
 * flushes the skip list, data, and footer buffers to the file.
 */
static void
flush_stripe(column_write_state *write_state)
{
	uint32_t column_index = 0;
	uint32_t chunk_index = 0;
	stripe_buffers *buffers = write_state->buffers;
	stripe_skip_list *skip_list = write_state->skip_list;
	chunk_skip_node **column_skip_node_array = skip_list->chunk_skip_nodes;
	TupleDesc tuple_descriptor = write_state->tuple_descriptor;
	uint32_t column_count = tuple_descriptor->natts;
	uint32_t chunk_count = skip_list->chunk_count;
	uint32_t chunk_group_row_limit = write_state->options.chunk_group_row_limit;
	uint32_t last_chunk_index = buffers->row_count / chunk_group_row_limit;
	uint32_t last_chunk_row_count = buffers->row_count % chunk_group_row_limit;
	uint64_t stripe_size = 0;
	uint64_t stripe_row_limit = buffers->row_count;

	bioseq_log_pg(DEBUG1, "Flushing Stripe of size {}", buffers->row_count);

	Oid relation_id = RelidByRelfilenumber(write_state->relation_file_locator.spcOid,
										  write_state->relation_file_locator.relNumber);

	Relation relation = bioseq::postgres::support::open_postgres_relation(relation_id, NoLock);

	/*
	 * check if the last chunk needs serialization , the last chunk was not serialized
	 * if it was not full yet, e.g.  (row_count > 0)
	 */
	if (last_chunk_row_count > 0)
	{
		serialize_chunk_data(write_state, last_chunk_index, last_chunk_row_count);
	}

	/* update buffer sizes in stripe skip list */
	for (column_index = 0; column_index < column_count; column_index++)
	{
		chunk_skip_node *chunk_skip_nodes = column_skip_node_array[column_index];
		column_buffers *column_buffers = buffers->columns[column_index];

		for (chunk_index = 0; chunk_index < chunk_count; chunk_index++)
		{
			chunk_buffers *chunk_buffers =
				column_buffers->chunks[chunk_index];
			uint64_t exists_buffer_size = chunk_buffers->presence_buffer->len;
			chunk_skip_node *chunk_skip_node = &chunk_skip_nodes[chunk_index];

			chunk_skip_node->exists_stream_offset = stripe_size;
			chunk_skip_node->exists_stream_length = exists_buffer_size;
			stripe_size += exists_buffer_size;
		}

		for (chunk_index = 0; chunk_index < chunk_count; chunk_index++)
		{
			chunk_buffers *chunk_buffers =
				column_buffers->chunks[chunk_index];
			uint64_t value_buffer_size = chunk_buffers->value_buffer->len;
			bioseq::compression_algorithm value_compression_algorithm = chunk_buffers->value_compression_algorithm;
			chunk_skip_node *chunk_skip_node = &chunk_skip_nodes[chunk_index];

			chunk_skip_node->value_stream_offset = stripe_size;
			chunk_skip_node->value_stream_length = value_buffer_size;
			chunk_skip_node->value_compression_algorithm = value_compression_algorithm;
			chunk_skip_node->value_compression_level = write_state->options.compression_level;
			chunk_skip_node->value_decompressed_length = chunk_buffers->value_decompressed_length;

			stripe_size += value_buffer_size;
		}
	}

	stripe_metadata *stripe_metadata =
		complete_stripe_reservation(relation, write_state->stripe_reservation->stripe_id,
								  stripe_size, stripe_row_limit, chunk_count);

	uint64_t current_file_offset = stripe_metadata->file_offset;

	/*
	 * Each stripe has only one section:
	 * Data section, in which we store data for each column continuously.
	 * We store data for each for each column in chunks. For each chunk, we
	 * store two buffers: "exists" buffer, and "value" buffer. "exists" buffer
	 * tells which values are not NULL. "value" buffer contains values for
	 * present values. For each column, we first store all "exists" buffers,
	 * and then all "value" buffers.
	 */

	/* flush the data buffers */
	for (column_index = 0; column_index < column_count; column_index++)
	{
		column_buffers *column_buffers = buffers->columns[column_index];

		for (chunk_index = 0; chunk_index < skip_list->chunk_count; chunk_index++)
		{
			chunk_buffers *chunk_buffers =
				column_buffers->chunks[chunk_index];
			StringInfo presence_buffer = chunk_buffers->presence_buffer;

			write_column_storage(relation, current_file_offset,
								 presence_buffer->data, presence_buffer->len);
			current_file_offset += presence_buffer->len;
		}

		for (chunk_index = 0; chunk_index < skip_list->chunk_count; chunk_index++)
		{
			chunk_buffers *chunk_buffers =
				column_buffers->chunks[chunk_index];
			StringInfo value_buffer = chunk_buffers->value_buffer;

			write_column_storage(relation, current_file_offset,
								 value_buffer->data, value_buffer->len);
			current_file_offset += value_buffer->len;
		}
	}

	save_chunk_groups(write_state->relation_file_locator,
					stripe_metadata->id,
					write_state->chunk_group_row_counts);
	save_stripe_skip_list(write_state->relation_file_locator,
					   stripe_metadata->id,
					   skip_list, tuple_descriptor);
	save_empty_row_mask(lookup_storage_id(write_state->relation_file_locator),
					 stripe_metadata->id,
					 stripe_metadata->first_row_number,
					 write_state->chunk_group_row_counts);

	write_state->chunk_group_row_counts = NIL;

	relation_close(relation, NoLock);
}


/*
 * serialize_boolean_array serializes the given boolean array and returns the result
 * as a StringInfo. This function packs every 8 boolean values into one byte.
 */
static StringInfo
serialize_boolean_array(bool *boolean_values, uint32_t boolean_count)
{
	uint32_t boolean_index = 0;
	uint32_t byte_count = ((boolean_count * sizeof(bool)) + (8 - sizeof(bool))) / 8;

	StringInfo boolean_buffer = makeStringInfo();
	enlargeStringInfo(boolean_buffer, byte_count);
	boolean_buffer->len = byte_count;
	memset(boolean_buffer->data, 0, byte_count);

	for (boolean_index = 0; boolean_index < boolean_count; boolean_index++)
	{
		if (boolean_values[boolean_index])
		{
			uint32_t byte_index = boolean_index / 8;
			uint32_t bit_index = boolean_index % 8;
			boolean_buffer->data[byte_index] |= (1 << bit_index);
		}
	}

	return boolean_buffer;
}


/*
 * serialize_chunk_data serializes and compresses chunk data at given chunk index with given
 * compression type for every column.
 */
static void
serialize_chunk_data(column_write_state *write_state, uint32_t chunk_index, uint32_t row_count)
{
	uint32_t column_index = 0;
	stripe_buffers *buffers = write_state->buffers;
	chunk_data *pending_chunk = write_state->pending_chunk;
	bioseq::compression_algorithm requested_compression = write_state->options.compression_algorithm;
	int compression_level = write_state->options.compression_level;
	const uint32_t column_count = buffers->column_count;
	StringInfo compression_buffer = write_state->compression_buffer;

	write_state->chunk_group_row_counts =
		lappend_int(write_state->chunk_group_row_counts, row_count);

	/* serialize exist values, data values are already serialized */
	for (column_index = 0; column_index < column_count; column_index++)
	{
		column_buffers *column_buffers = buffers->columns[column_index];
		chunk_buffers *chunk_buffers = column_buffers->chunks[chunk_index];

		chunk_buffers->presence_buffer =
			serialize_boolean_array(pending_chunk->present_values[column_index], row_count);
	}

	/*
	 * check and compress value buffers, if a value buffer is not compressable
	 * then keep it as uncompressed, store compression information.
	 */
	for (column_index = 0; column_index < column_count; column_index++)
	{
		column_buffers *column_buffers = buffers->columns[column_index];
		chunk_buffers *chunk_buffers = column_buffers->chunks[chunk_index];
		bioseq::compression_algorithm actual_compression = bioseq::compression_algorithm::none;

		StringInfo serialized_value_buffer = pending_chunk->referenced_value_buffers[column_index];

		Assert(static_cast<int>(requested_compression) >= 0 &&
			   static_cast<int>(requested_compression) < static_cast<int>(bioseq::compression_algorithm::count));

		chunk_buffers->value_decompressed_length =
			pending_chunk->referenced_value_buffers[column_index]->len;

		/*
		 * if serialized_value_buffer is be compressed, update serialized_value_buffer
		 * with compressed data and store compression type.
		 */
		bool compressed = compress_column_buffer(serialized_value_buffer, compression_buffer,
										 requested_compression,
										 compression_level);

		if (compressed)
		{
			serialized_value_buffer = compression_buffer;
			actual_compression = requested_compression;
		}

		/* store (compressed) value buffer */
		chunk_buffers->value_compression_algorithm = actual_compression;
		chunk_buffers->value_buffer = copy_string_info(serialized_value_buffer);

		/* value_buffer needs to be reset for next chunk's data */
		resetStringInfo(pending_chunk->referenced_value_buffers[column_index]);
	}
}


/*
 * update_chunk_min_max takes the given column value, and checks if this
 * value falls outside the range of minimum/maximum values of the given column
 * chunk skip node. If it does, the function updates the column chunk skip node
 * accordingly.
 */
static void
update_chunk_min_max(chunk_skip_node *chunk_skip_node, Datum column_value,
						  bool column_type_by_value, int column_type_length,
						  Oid column_collation, FmgrInfo *comparison_function)
{
	bool has_min_max = chunk_skip_node->has_min_max;
	Datum previous_minimum = chunk_skip_node->minimum_value;
	Datum previous_maximum = chunk_skip_node->maximum_value;
	Datum current_minimum = 0;
	Datum current_maximum = 0;

	/* if type doesn't have a comparison function, skip min/max values */
	if (comparison_function == NULL)
	{
		return;
	}

	if (!has_min_max)
	{
		current_minimum = bioseq::postgres::support::pg_datum::from_raw(column_value)
			.copy(column_type_by_value, column_type_length).raw();
		current_maximum = bioseq::postgres::support::pg_datum::from_raw(column_value)
			.copy(column_type_by_value, column_type_length).raw();
	}
	else
	{
		Datum minimum_comparison_datum = FunctionCall2Coll(comparison_function,
														 column_collation, column_value,
														 previous_minimum);
		Datum maximum_comparison_datum = FunctionCall2Coll(comparison_function,
														 column_collation, column_value,
														 previous_maximum);
		int minimum_comparison = DatumGetInt32(minimum_comparison_datum);
		int maximum_comparison = DatumGetInt32(maximum_comparison_datum);

		if (minimum_comparison < 0)
		{
			if (!column_type_by_value)
				pfree(DatumGetPointer(previous_minimum));

			current_minimum = bioseq::postgres::support::pg_datum::from_raw(column_value)
				.copy(column_type_by_value, column_type_length).raw();
		}
		else
		{
			current_minimum = previous_minimum;
		}

		if (maximum_comparison > 0)
		{
			if (!column_type_by_value)
				pfree(DatumGetPointer(previous_maximum));

			current_maximum = bioseq::postgres::support::pg_datum::from_raw(column_value)
				.copy(column_type_by_value, column_type_length).raw();
		}
		else
		{
			current_maximum = previous_maximum;
		}
	}

	chunk_skip_node->has_min_max = true;
	chunk_skip_node->minimum_value = current_minimum;
	chunk_skip_node->maximum_value = current_maximum;
}


/*
 * copy_string_info creates a deep copy of given source string allocating only needed
 * amount of memory.
 */
static StringInfo
copy_string_info(StringInfo source_buffer)
{
	StringInfo target_buffer = bioseq::postgres::support::allocate_zeroed_object<StringInfoData>();

	if (source_buffer->len > 0)
	{
		target_buffer->data = bioseq::postgres::support::allocate_zeroed_storage<char>(source_buffer->len);
		target_buffer->len = source_buffer->len;
		target_buffer->maxlen = source_buffer->len;
		memcpy(target_buffer->data, source_buffer->data, source_buffer->len);
	}

	return target_buffer;
}


/** @brief Report whether the writer contains rows not yet persisted. */
export bool
has_pending_column_writes(column_write_state *state)
{
	return state->buffers != NULL && state->buffers->row_count != 0;
}
