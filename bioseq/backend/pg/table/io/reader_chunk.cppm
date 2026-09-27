/**
 * @file
 * @brief Chunk loading, decoding, and row materialization for table reads.
 */
module;
#include <bioseq/pg_compat.h>

#include <cstdint>

export module bioseq.postgres.table.io:read_chunk;

import bioseq.common.logging;
import bioseq.postgres.vectorization.types;
import bioseq.postgres.storage.compression;
import bioseq.postgres.storage.physical;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.support;
import bioseq.postgres.configuration.core;
import :buffers;
import :cache;
import :pruning;

using namespace bioseq::postgres::support;

/** @brief Decode packed presence bits into a Boolean array. */
static void
deserialize_boolean_array(StringInfo bool_array_buffer, bool *bool_array,
					 uint32_t bool_array_length)
{
	uint32_t bool_array_index = 0;

	uint32_t maximum_bool_count = bool_array_buffer->len * 8;
	if (bool_array_length > maximum_bool_count)
	{
		bioseq_log<logging_level::error, logging_target::postgres>("insufficient data for reading boolean array");
	}

	for (bool_array_index = 0; bool_array_index < bool_array_length; bool_array_index++)
	{
		uint32_t byte_index = bool_array_index / 8;
		uint32_t bit_index = bool_array_index % 8;
		uint8_t bitmask = (1 << bit_index);

		uint8_t shifted_bit = (bool_array_buffer->data[byte_index] & bitmask);
		if (shifted_bit == 0)
		{
			bool_array[bool_array_index] = false;
		}
		else
		{
			bool_array[bool_array_index] = true;
		}
	}
}

/** @brief Decode serialized non-null datums into their row positions. */
static void
deserialize_datum_array(StringInfo datum_buffer, bool *present_values, uint32_t datum_count,
					  bool datum_type_by_value, int datum_type_length,
					  char datum_type_align, Datum *datum_array)
{
	uint32_t datum_index = 0;
	uint32_t current_datum_data_offset = 0;

	for (datum_index = 0; datum_index < datum_count; datum_index++)
	{
		if (!present_values[datum_index])
		{
			continue;
		}

		char *current_datum_data_pointer = datum_buffer->data + current_datum_data_offset;

		datum_array[datum_index] = fetch_att(current_datum_data_pointer, datum_type_by_value,
										   datum_type_length);
		current_datum_data_offset = att_addlength_datum(current_datum_data_offset,
													 datum_type_length,
													 datum_array[datum_index]);
		current_datum_data_offset = att_align_nominal(current_datum_data_offset,
												   datum_type_align);

		if (current_datum_data_offset > datum_buffer->len)
		{
			bioseq_log<logging_level::error, logging_target::postgres>("insufficient data left in datum buffer: {}, {}",
								current_datum_data_offset, datum_buffer->len);
		}
	}
}

/** @brief Evaluate a column's immutable default expression. */
static Datum
get_column_default_value(TupleConstr *tuple_constraints, Form_pg_attribute attribute_form)
{
	Node *default_value_node = NULL;
	int def_val_index = 0;

	for (def_val_index = 0; def_val_index < tuple_constraints->num_defval; def_val_index++)
	{
		AttrDefault attr_default = tuple_constraints->defval[def_val_index];
		if (attr_default.adnum == attribute_form->attnum)
		{
			default_value_node = static_cast<Node*>(stringToNode(attr_default.adbin));
			break;
		}
	}

	Assert(default_value_node != NULL);

	/* try reducing the default value node to a const node */
	default_value_node = eval_const_expressions(NULL, default_value_node);
	if (Const *const_node = try_cast_pg_node<Const>(default_value_node))
	{
		return const_node->constvalue;
	}
	else
	{
		const char *column_name = NameStr(attribute_form->attname);
		bioseq_report<logging_level::error, logging_target::postgres>().with_message("unsupported default value for column \"{}\"", column_name)
					   .with_hint("Expression is either mutable or "
							 "does not evaluate to constant value");
	}
}

/** @brief Read the selected chunks for one physical column. */
static column_buffers *
load_column_buffers(Relation relation, chunk_skip_node *chunk_skip_nodes,
				  uint32_t chunk_count, uint64_t stripe_offset,
				  Form_pg_attribute attribute_form)
{
	uint32_t chunk_index = 0;
	chunk_buffers **chunks =
		bioseq::postgres::support::allocate_zeroed_array<chunk_buffers*>(chunk_count);

	for (chunk_index = 0; chunk_index < chunk_count; chunk_index++)
	{
		chunks[chunk_index] = bioseq::postgres::support::allocate_zeroed_object<chunk_buffers>();
	}

	/*
	 * We first read the "exists" chunks. We don't read "values" array here,
	 * because "exists" chunks are stored sequentially on disk, and we want to
	 * minimize disk seeks.
	 */
	for (chunk_index = 0; chunk_index < chunk_count; chunk_index++)
	{
		chunk_skip_node *chunk_skip_node = &chunk_skip_nodes[chunk_index];
		uint64_t exists_offset = stripe_offset + chunk_skip_node->exists_stream_offset;
		StringInfo raw_exists_buffer = makeStringInfo();

		enlargeStringInfo(raw_exists_buffer, chunk_skip_node->exists_stream_length);
		raw_exists_buffer->len = chunk_skip_node->exists_stream_length;
		read_column_storage(relation, exists_offset, raw_exists_buffer->data,
							chunk_skip_node->exists_stream_length);

		chunks[chunk_index]->presence_buffer = raw_exists_buffer;
	}

	/* then read "values" chunks, which are also stored sequentially on disk */
	for (chunk_index = 0; chunk_index < chunk_count; chunk_index++)
	{
		chunk_skip_node *chunk_skip_node = &chunk_skip_nodes[chunk_index];
		bioseq::compression_algorithm compression_algorithm = chunk_skip_node->value_compression_algorithm;
		uint64_t value_offset = stripe_offset + chunk_skip_node->value_stream_offset;
		StringInfo raw_value_buffer = makeStringInfo();

		enlargeStringInfo(raw_value_buffer, chunk_skip_node->value_stream_length);
		raw_value_buffer->len = chunk_skip_node->value_stream_length;
		read_column_storage(relation, value_offset, raw_value_buffer->data,
							chunk_skip_node->value_stream_length);

		chunks[chunk_index]->value_buffer = raw_value_buffer;
		chunks[chunk_index]->value_compression_algorithm = compression_algorithm;
		chunks[chunk_index]->value_decompressed_length =
			chunk_skip_node->value_decompressed_length;
	}

	column_buffers *loaded_column_buffers = bioseq::postgres::support::allocate_zeroed_object<column_buffers>();
	loaded_column_buffers->chunks = chunks;

	return loaded_column_buffers;
}

/** @brief Load projected, predicate-selected buffers for one stripe. */
export stripe_buffers *
load_filtered_stripe_buffers(Relation relation, stripe_metadata *stripe_metadata,
						  TupleDesc tuple_descriptor, List *projected_columns,
						  List *filter_clauses, List *filter_variables,
						  int64 *filtered_chunk_groups, Snapshot snapshot)
{
	uint32_t column_index = 0;
	uint32_t column_count = tuple_descriptor->natts;

	bool *projected_column_mask = build_projected_column_mask(column_count, projected_columns);

	stripe_skip_list *skip_list = read_stripe_skip_list(relation->rd_locator,
														stripe_metadata->id,
														tuple_descriptor,
														stripe_metadata->chunk_count,
														snapshot);
	stripe_skip_list *selected_chunk_skip_list = skip_list;

	if (list_length(filter_variables) > 0)
	{
		bool *selected_chunk_mask = select_chunk_mask(skip_list, filter_clauses,
													filter_variables, filtered_chunk_groups);

		selected_chunk_skip_list = select_chunk_skip_list(skip_list, projected_column_mask,
													 selected_chunk_mask);
	}

	/* load column data for projected columns */
	column_buffers **columns = bioseq::postgres::support::allocate_zeroed_array<column_buffers*>(column_count);

	for (column_index = 0; column_index < stripe_metadata->column_count; column_index++)
	{
		if (projected_column_mask[column_index])
		{
			chunk_skip_node *chunk_skip_node =
				selected_chunk_skip_list->chunk_skip_nodes[column_index];
			Form_pg_attribute attribute_form = bioseq::postgres::support::tuple_descriptor_attribute(tuple_descriptor, column_index);
			uint32_t chunk_count = selected_chunk_skip_list->chunk_count;

			column_buffers *column_buffers = load_column_buffers(relation, chunk_skip_node,
															 chunk_count,
															 stripe_metadata->file_offset,
															 attribute_form);

			columns[column_index] = column_buffers;
		}
	}

	stripe_buffers *buffers = bioseq::postgres::support::allocate_zeroed_object<stripe_buffers>();
	buffers->column_count = column_count;
	buffers->chunk_count = selected_chunk_skip_list->chunk_count;
	buffers->row_count = count_stripe_skip_rows(selected_chunk_skip_list);
	buffers->columns = columns;
	buffers->selected_chunk_group_row_counts =
		selected_chunk_skip_list->chunk_group_row_counts;
	buffers->selected_chunk_group_row_offsets =
		selected_chunk_skip_list->chunk_group_row_offsets;
	buffers->selected_chunk_group_deleted_rows =
		selected_chunk_skip_list->chunk_group_deleted_rows;

	return buffers;
}

/** @brief Decompress and materialize one selected chunk group. */
static chunk_data *
deserialize_chunk_data(stripe_buffers *buffers, uint64_t chunk_index,
					 uint32_t row_count, TupleDesc tuple_descriptor,
					 List *projected_columns, stripe_read_state *state, uint64_t stripe_id)
{
	int column_index = 0;
	bool *column_mask = build_projected_column_mask(tuple_descriptor->natts, projected_columns);

	chunk_data *pending_chunk = allocate_chunk_data(tuple_descriptor->natts, column_mask,
												row_count);

	for (column_index = 0; column_index < buffers->column_count; column_index++)
	{
		Form_pg_attribute attribute_form = bioseq::postgres::support::tuple_descriptor_attribute(tuple_descriptor, column_index);
		column_buffers *column_buffers = buffers->columns[column_index];
		bool column_added = false;

		if (column_buffers == NULL && column_mask[column_index])
		{
			column_added = true;
		}

		if (column_buffers != NULL)
		{
			chunk_buffers *chunk_buffers =
				column_buffers->chunks[chunk_index];
			bool should_cache = bioseq::settings::column_cache::enabled == true &&
				chunk_buffers->value_compression_algorithm != bioseq::compression_algorithm::none;

			if (should_cache)
			{
				mark_chunk_group_in_use(state->relation->rd_id, stripe_id, chunk_index);
			}

			/* decompress and deserialize current chunk's data */
			StringInfo value_buffer = NULL;
			bool value_buffer_owned = true;

			if (should_cache)
			{
				value_buffer = static_cast<StringInfo>(retrieve_column_cache_entry(
					state->relation->rd_id, stripe_id, chunk_index, column_index));
			}

			if (value_buffer == NULL)
			{
				if (chunk_buffers->value_compression_algorithm == bioseq::compression_algorithm::none && !should_cache)
				{
					/* Shared raw stripe buffer: use in place and leave owner unchanged. */
					value_buffer = chunk_buffers->value_buffer;
					value_buffer_owned = false;
				}
				else
				{
					MemoryContext old_memory_context;
					if (should_cache)
					{
						old_memory_context = MemoryContextSwitchTo(get_column_cache_memory_context());
					}

					value_buffer = decompress_column_buffer(chunk_buffers->value_buffer,
									 chunk_buffers->value_compression_algorithm,
									 chunk_buffers->value_decompressed_length);

					value_buffer = static_cast<StringInfo>(repalloc(value_buffer, sizeof(cached_string_info)));
					reinterpret_cast<cached_string_info *>(value_buffer)->cached = false;

					if (should_cache)
					{
						reinterpret_cast<cached_string_info *>(value_buffer)->cached = true;

						add_column_cache_entry(state->relation->rd_id, stripe_id, chunk_index,
												  column_index, value_buffer);
						MemoryContextSwitchTo(old_memory_context);
					}
				}
			}

			deserialize_boolean_array(chunk_buffers->presence_buffer,
								 pending_chunk->present_values[column_index],
								 row_count);
			deserialize_datum_array(value_buffer, pending_chunk->present_values[column_index],
								  row_count, attribute_form->attbyval,
								  attribute_form->attlen, attribute_form->attalign,
								  pending_chunk->values_by_column[column_index]);

			/* Never pass the shared raw stripe buffer to the chunk free path. */
			pending_chunk->referenced_value_buffers[column_index] = value_buffer_owned ? value_buffer : NULL;
		}
		else if (column_added)
		{
			/*
			 * This is a column that was added after creation of this stripe.
			 * So we use either the default value or NULL.
			 */
			if (attribute_form->atthasdef)
			{
				int row_index = 0;

				Datum default_value = get_column_default_value(tuple_descriptor->constr,
														attribute_form);

				for (row_index = 0; row_index < row_count; row_index++)
				{
					pending_chunk->present_values[column_index][row_index] = true;
					pending_chunk->values_by_column[column_index][row_index] = default_value;
				}
			}
			else
			{
				memset(pending_chunk->present_values[column_index], false,
					   row_count * sizeof(bool));
			}
		}
	}

	return pending_chunk;
}

/** @brief Initialize row-reading state for a selected chunk group. */
export chunk_group_read_state *
begin_chunk_group_read(stripe_buffers *buffers, int chunk_index, TupleDesc tuple_desc,
					List *projected_columns, MemoryContext cxt, stripe_read_state *state,
					uint64_t stripe_id)
{
	if (chunk_index < 0 || static_cast<uint32_t>(chunk_index) >= buffers->chunk_count)
	{
		bioseq_log<logging_level::error, logging_target::postgres>("chunk group index {} is outside selected chunk count {}",
			   chunk_index, buffers->chunk_count);
	}

	uint32_t chunk_group_row_count =
		buffers->selected_chunk_group_row_counts[chunk_index];
	uint32_t chunk_group_row_offsets =
		buffers->selected_chunk_group_row_offsets[chunk_index];
	uint32_t chunk_group_deleted_rows =
		buffers->selected_chunk_group_deleted_rows[chunk_index];

	bioseq::postgres::support::memory_context_scope ctx_guard{cxt};
	chunk_group_read_state *chunk_group_state = bioseq::postgres::support::allocate_zeroed_object<chunk_group_read_state>();

	chunk_group_state->current_row = 0;
	chunk_group_state->row_count = chunk_group_row_count;
	chunk_group_state->stripe_row_offset = chunk_group_row_offsets;
	chunk_group_state->chunk_group_deleted_rows = chunk_group_deleted_rows;
	chunk_group_state->column_count = tuple_desc->natts;
	chunk_group_state->projected_columns = projected_columns;

	chunk_group_state->chunk_group_data = deserialize_chunk_data(buffers, chunk_index,
															   chunk_group_row_count,
															   tuple_desc,
															   projected_columns, state, stripe_id);

	return chunk_group_state;
}

/** @brief Release a chunk group's decoded values and read state. */
export void
end_chunk_group_read(chunk_group_read_state *chunk_group_state)
{
	free_chunk_value_buffers(chunk_group_state->chunk_group_data);
	free_chunk_data(chunk_group_state->chunk_group_data);
	if (chunk_group_state->row_mask != NULL && !chunk_group_state->row_mask_cached)
		pfree(chunk_group_state->row_mask);
	chunk_group_state->row_mask = NULL;
	pfree(chunk_group_state);
}

/** @brief Materialize the next visible row from a chunk group. */
export bool
read_next_chunk_group_row(chunk_group_read_state *chunk_group_state, Datum *column_values,
					  bool *column_nulls, int32_t *deleted_columns_number)
{
	/*
	 * Initialize to all-NULL. Only non-NULL projected attributes will be set.
	 */
	memset(column_nulls, true, sizeof(bool) * chunk_group_state->column_count);

	while (chunk_group_state->current_row < chunk_group_state->row_count)
	{
		if (chunk_group_state->row_mask != NULL)
		{
			int8_t check_column_mask = 1 << (chunk_group_state->current_row % 8);
			int8_t check_lookup_mask =
				VARDATA(chunk_group_state->row_mask)[chunk_group_state->current_row / 8 ];

			if (check_lookup_mask & check_column_mask)
			{
				chunk_group_state->current_row++;
				*deleted_columns_number += 1;
				continue;
			}
		}

		for (auto attno : bioseq::postgres::support::postgres_integer_list{chunk_group_state->projected_columns})
		{
			const chunk_data *chunk_group_data = chunk_group_state->chunk_group_data;
			const int row_index = chunk_group_state->current_row;

			/* attno is 1-indexed; present_values is 0-indexed */
			const uint32_t column_index = attno - 1;

			if (chunk_group_data->present_values[column_index][row_index])
			{
				column_values[column_index] = chunk_group_data->values_by_column[column_index][row_index];
				column_nulls[column_index] = false;
			}
		}

		chunk_group_state->current_row++;
		return true;
	}

	return false;
}

/** @brief Append visible chunk-group rows to vectorized output columns. */
export bool
read_next_chunk_group_vector(chunk_group_read_state *chunk_group_state, Datum *column_values,
						 bool *column_nulls, TupleDesc tuple_desc,
						 int32_t *column_value_offset, int *chunk_read_rows,
						 std::uint64_t *row_number,
						 uint64 stripe_first_row_number)
{
	(void) tuple_desc;
	(void) column_value_offset;

	if (chunk_group_state->current_row >= chunk_group_state->row_count)
	{
		Assert(chunk_group_state->current_row == chunk_group_state->row_count);
		return false;
	}

	/*
	 * Initialize to all-NULL. Only non-NULL projected attributes will be set.
	 */
	memset(column_nulls, true, sizeof(bool) * chunk_group_state->column_count);

	int i;
	int row_number_index = 0;

	for (i = 0; i < chunk_group_state->row_count; i ++)
	{
		if (chunk_group_state->current_row >= chunk_group_state->row_count)
			return false;

		if (*chunk_read_rows >= vector_column_capacity)
			break;

		if (chunk_group_state->row_mask != NULL)
		{
			int8_t check_column_mask = 1 << (chunk_group_state->current_row % 8);
			int8_t check_lookup_mask =
				VARDATA(chunk_group_state->row_mask)[chunk_group_state->current_row / 8];

			if (check_lookup_mask & check_column_mask)
			{
				chunk_group_state->current_row++;
				continue;
			}
		}

		for (auto attno : bioseq::postgres::support::postgres_integer_list{chunk_group_state->projected_columns})
		{
			const chunk_data *chunk_group_data = chunk_group_state->chunk_group_data;
			const int row_index = chunk_group_state->current_row;

			/* attno is 1-indexed; present_values is 0-indexed */
			const uint32_t column_index = attno - 1;

			auto* vector_column = reinterpret_cast<vector_column_storage*>(
				column_values[column_index]);
			const bool is_null =
				!chunk_group_data->present_values[column_index][row_index];
			write_datum_to_vector_column(
				vector_column,
				is_null
					? static_cast<Datum>(0)
					: chunk_group_data->values_by_column[column_index][row_index],
				is_null,
				vector_column->lane_count);
		}

		(*chunk_read_rows)++;
		chunk_group_state->current_row++;
		row_number[row_number_index++] = stripe_first_row_number + chunk_group_state->current_row - 1;
	}

	return true;
}
