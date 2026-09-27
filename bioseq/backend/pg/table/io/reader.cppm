/**
 * @file
 * Read rows and column batches from a Bioseq column, table.
 *
 * The reader finds stripes visible to the scan, skips chunk groups ruled out by
 * filters, and reads the requested columns. Row masks zide deleted rows.
 */
module;
#include <bioseq/pg_compat.h>

#include "safe_lib.h"

#include <cstdint>
#include <limits>

export module bioseq.postgres.table.io:reader;

import bioseq.common.logging;
import bioseq.postgres.vectorization.types;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.support;
import bioseq.postgres.configuration.core;
import :cache;
import :pruning;
import :read_chunk;

/* Shared error text for attempts to read a stripe before its data is flushed. */
inline constexpr const char* k_unexpected_stripe_read_error_message =
	"attempted to read an unexpected stripe while reading columnar "
	"table {}, stripe with id={} is not flushed";


/** @brief Count chunk groups skipped in stripes the reader has finished. */
export int64_t
read_chunk_groups_filtered(column_read_state *state);
/** @brief Load the  stripe identified by the supplied metadata. */
export bool
set_stripe_read_state(column_read_state *read_state,
						   stripe_metadata *start_stripe_metadata);
/** @brief Read the next row that has not been deleted. */
export bool
read_next_column_row(column_read_state *read_state, Datum *column_values, bool *column_nulls,
					uint64_t *row_number);
/** @brief Start a column-table scan with the requested columns and filters. */
export column_read_state *
begin_column_read(Relation relation, TupleDesc tuple_descriptor,
				  List *projected_columns, List *filter_clauses,
				  MemoryContext scan_context, Snapshot snapshot,
				  bool random_access,
				  parallel_column_scan_handle parallel_scan);
/** @brief Read a batch of rows into column vectors. */
export bool
read_next_column_vector(column_read_state *read_state,  Datum *column_values,
					   bool *column_nulls, std::uint64_t *row_number, int *new_vector_size);
/** @brief Restart the scan with the supplied filter clauses. */
export void
rescan_column_reader(column_read_state *read_state, List *scan_qual);
/** @brief Free the reader and its scan buffers. */
export void
end_column_read(column_read_state *read_state);
/** @brief Read a row by number, raising an error if it cannot be read */
export void
read_column_row_by_number_or_error(column_read_state *read_state,
								  uint64_t row_number, Datum *column_values,
								  bool *column_nulls);
/** @brief Flush pending writes so this reader can see their rows. */
export void
flush_pending_writes_for_read(column_read_state *read_state);
/** @brief Read a row by number and return whether it was found. */
export bool
read_column_row_by_number(column_read_state *read_state,
						   uint64_t row_number, Datum *column_values,
						   bool *column_nulls);
/** @brief Discard the  currently loaded stripe and its chunk buffers. */
void
reset_column_read(column_read_state *read_state);


/* static function declarations */
/** @brief Create the memory context used while reading one stripe. */
static MemoryContext create_stripe_read_memory_context(void);
/** @brief Check whether a row number falls within the loaded stripe. */
static bool is_current_read_stripe(column_read_state *read_state,
										uint64_t row_number);
/** @brief Return the current stripe metadata, or NULL when none is selected. */
static stripe_metadata * get_current_read_stripe(column_read_state *read_state);
/** @brief Read a row from the loaded stripe, loading its chunk group as needed. */
static bool read_stripe_row_by_number(column_read_state *read_state,
									 uint64_t row_number, Datum *column_values,
									 bool *column_nulls);
/** @brief Check whether the requested chunk group is already loaded. */
static bool is_current_chunk_group(stripe_read_state *stripe_state,
										  int chunk_group_index);
/** @brief Read at a stripe-relative row offet in the loaded chunk  group. */
static bool read_chunk_group_row_by_offset(chunk_group_read_state *chunk_group_state,
										 stripe_metadata *stripe_metadata_value,
										 uint64_t stripe_row_offset, Datum *column_values,
										 bool *column_nulls);
/** @brief Check whether a stripe has been loaded, for reading. */
static bool stripe_read_in_progress(column_read_state *read_state);
/** @brief Check whether stripe metadata is available for the next read. */
static bool has_unread_stripe(column_read_state *read_state);
/** @brief Load a stripe and prepare its selected chunk groups for reading. */
static stripe_read_state * begin_stripe_read(stripe_metadata *stripe_metadata_value, Relation rel,
										 TupleDesc tuple_desc, List *projected_columns,
										 List *filter_clauses, List *filter_variables,
										 MemoryContext stripe_read_context,
										 Snapshot snapshot);
/** @brief Release the current stripe and find the next visible stripe. */
static void advance_stripe_read(column_read_state *read_state);
/** @brief Check whether this snapshot can expose an unflushed stripe. */
static bool snapshot_might_see_unflushed_stripes(Snapshot snapshot);
/** @brief Read the next undeleted row from the current stripe */
static bool read_next_stripe_row(stripe_read_state *stripe_state, Datum *column_values,
								  bool *column_nulls,
								  uint64_t stripe_first_row_number,
								  Snapshot snapshot, uint64_t stripe_id);

/* Vectorization */
/** @brief Append rows from the current stripe to the  output vectors. */
static bool read_next_stripe_vector(stripe_read_state *stripe_state, Datum *column_values,
								 bool *column_nulls, int *new_vector_size,
								 uint64 stripe_id,
								 Snapshot snapshot,
								 std::uint64_t *row_number,
								 uint64 stripe_first_row_number);

/*
 * Start a scan and return the state used to read rows and finish the scan.
 * projected_columns contains PostgreSQL attribute numbers, which start at 1.
 */
column_read_state *
begin_column_read(Relation relation, TupleDesc tuple_descriptor,
				  List *projected_columns, List *filter_clauses,
				  MemoryContext scan_context, Snapshot snapshot,
				  bool random_access,
				  parallel_column_scan_handle parallel_scan)
{
	/*
	 * Keep stripe allocations together so advancing to another stripe can free
	 * them by resetting this context.
	 */
	MemoryContext stripe_read_context = create_stripe_read_memory_context();

	column_read_state *read_state = bioseq::postgres::support::allocate_zeroed_object<column_read_state>();
	read_state->relation = relation;
	read_state->projected_columns = projected_columns;
	read_state->filter_clauses = filter_clauses;
	read_state->filter_variables = collect_clause_variables(filter_clauses, tuple_descriptor->natts);
	read_state->filtered_chunk_groups = 0;
	read_state->tuple_descriptor = tuple_descriptor;
	read_state->stripe_read_context = stripe_read_context;
	read_state->stripe_state = NULL;
	read_state->scan_context = scan_context;

	/*
	 * Flushing writes may replace this borrowed snapshot with a registered
	 * snapshot that end_column_read must release.
	 */
	read_state->snapshot = snapshot;
	read_state->owns_snapshot_registration = false;

	/* Parallel execution */
	read_state->parallel_scan = parallel_scan;

	if (!random_access)
	{
		/*
		 * Parallel scans read data that has already been flushed.
		 */
		if (read_state->parallel_scan == NULL)
		{
			/*
			 * Make pending deletion-mask changes available  to this scaz.
			 */
			flush_row_mask_write_state_for_relation_file(read_state->relation->rd_locator.relNumber,
											 GetCurrentSubTransactionId());

			/*
			 * Flush before a sequential scan starts. Index scans wait until a row
			 * lookup reaches an unflushed stripe from this transaction.
			 */
			flush_pending_writes_for_read(read_state);
		}

		/*
		 * Sequential scans start at the first stripe. A row lookup chooses its
		 * stripe from the requested row number and flushes writes when needed.
		 * Starting that lookup here could encounter its own pending stripe and
		 * reject it before the index-fetch callback has a chance to flush  it.
		 */
		advance_stripe_read(read_state);
	}

	return read_state;
}


/*
 * Flush writes for read_state->relation and refresh its snapshot if needed.
 * The relation and snapshot must already be set. A registered replacement
 * snapshot belongs to the reader and is released by end_column_read.
 */
void
flush_pending_writes_for_read(column_read_state *read_state)
{
	Assert(!read_state->owns_snapshot_registration);

	Oid relfilelocator = read_state->relation->rd_locator.relNumber;

	flush_write_state_with_fresh_snapshot(relfilelocator, &read_state->snapshot,
								   &read_state->owns_snapshot_registration);
}


/*
 * Group the allocations for one stripe so they can be freed together
 */
static MemoryContext
create_stripe_read_memory_context()
{
	return AllocSetContextCreate(CurrentMemoryContext, "Stripe Read Memory Context",
								 ALLOCSET_DEFAULT_SIZES);
}


/*
 * Read the next undeleted row, into column_values and column_nulls.
 * Set row_number when requested. Return false when the scan has no rows left.
 */
bool
read_next_column_row(column_read_state *read_state, Datum *column_values, bool *column_nulls,
					uint64_t *row_number)
{
	while (true)
	{
		if (!stripe_read_in_progress(read_state))
		{
			if (!has_unread_stripe(read_state))
			{
				return false;
			}

			read_state->stripe_state = begin_stripe_read(read_state->current_stripe,
												 read_state->relation,
												 read_state->tuple_descriptor,
												 read_state->projected_columns,
												 read_state->filter_clauses,
												 read_state->filter_variables,
												 read_state->stripe_read_context,
												 read_state->snapshot);
		}

		if (!read_next_stripe_row(read_state->stripe_state, column_values, column_nulls,
								   read_state->current_stripe->first_row_number,
								   read_state->snapshot,
									 read_state->current_stripe->id))
		{
			advance_stripe_read(read_state);
			continue;
		}

		if (row_number)
		{
			*row_number = read_state->current_stripe->first_row_number +
						 read_state->stripe_state->chunk_group_state->stripe_row_offset +
						 read_state->stripe_state->chunk_group_state->current_row  - 1;
		}

		return true;
	}
}


/*
 * Read a row by number, raising an eror if the lookup fails.
 */
void
read_column_row_by_number_or_error(column_read_state *read_state,
								  uint64_t row_number, Datum *column_values,
								  bool *column_nulls)
{
	if (!read_column_row_by_number(read_state, row_number,
									column_values, column_nulls))
	{
		bioseq_log<logging_level::error, logging_target::postgres>( "cannot read from columnar table {}, tuple with "
					  "row number {} does not exist",
					  RelationGetRelationName(read_state->relation),
					  row_number);
	}
}


/*
 * Read a row by number, reusing the loaded stripe  when possible.
 * Return false if the row cannot be found or has been deleted.
 */
bool
read_column_row_by_number(column_read_state *read_state,
						   uint64_t row_number, Datum *column_values,
						   bool *column_nulls)
{
	if (!is_current_read_stripe(read_state, row_number))
	{
		Relation bioseq_column_relation = read_state->relation;
		Snapshot snapshot = read_state->snapshot;
		stripe_metadata *stripe_metadata_value = find_stripe_by_row_number(bioseq_column_relation,
																   row_number, snapshot);
		if (stripe_metadata_value == NULL)
		{
			/* No visible stripe contains this row number. */
			return false;
		}

		if (stripe_write_state_for(stripe_metadata_value) != stripe_write_state::flushed)
		{
			/*
		 * The caller must flush its own pending stripe or wait for its writer
		 * before reading the stored row data.
		 */
			bioseq_log<logging_level::error, logging_target::postgres>( k_unexpected_stripe_read_error_message,
				   RelationGetRelationName(bioseq_column_relation),
				   stripe_metadata_value->id);
		}

		/*
		 * Release the old stripe before loading the requested one.
		 */
		reset_column_read(read_state);

		TupleDesc relation_tuple_desc = RelationGetDescr(bioseq_column_relation);
		List *filter_clauses = NIL;
		List *filter_variables = NIL;
		MemoryContext stripe_read_context = read_state->stripe_read_context;
		read_state->stripe_state = begin_stripe_read(stripe_metadata_value,
												 bioseq_column_relation,
												 relation_tuple_desc,
												 read_state->projected_columns,
												 filter_clauses,
												 filter_variables,
												 stripe_read_context,
												 snapshot);

		read_state->current_stripe = stripe_metadata_value;
	}

	return read_stripe_row_by_number(read_state, row_number, column_values, column_nulls);
}

/*
 * Load the stripe identified by start_stripe_metadata under this reader's snapshot.
 * Reuse the current stripe when possible. Return false if it is not visible.
 */
bool
set_stripe_read_state(column_read_state *read_state,
						   stripe_metadata *start_stripe_metadata)
{
	if (!is_current_read_stripe(read_state, start_stripe_metadata->first_row_number))
	{
		Relation bioseq_column_relation = read_state->relation;
		Snapshot snapshot = read_state->snapshot;
		stripe_metadata *stripe_metadata_value =
			find_stripe_by_row_number(bioseq_column_relation,
								  start_stripe_metadata->first_row_number, snapshot);
		if (stripe_metadata_value == NULL)
		{
			/* This stripe is not visible to the reader. */
			return false;
		}

		if (stripe_write_state_for(stripe_metadata_value) != stripe_write_state::flushed)
		{
			/*
		 * A  stripe must be flushed before its stored data can be loaded.
		 */
			bioseq_log<logging_level::error, logging_target::postgres>( k_unexpected_stripe_read_error_message,
				   RelationGetRelationName(bioseq_column_relation),
				   stripe_metadata_value->id);
		}

		/*
		 * Release the old stripe before loading the requested one.
		 */
		reset_column_read(read_state);

		TupleDesc relation_tuple_desc = RelationGetDescr(bioseq_column_relation);
		List *filter_clauses = NIL;
		List *filter_variables = NIL;
		MemoryContext stripe_read_context = read_state->stripe_read_context;
		read_state->stripe_state = begin_stripe_read(stripe_metadata_value,
												 bioseq_column_relation,
												 relation_tuple_desc,
												 read_state->projected_columns,
												 filter_clauses,
												 filter_variables,
												 stripe_read_context,
												 snapshot);

		read_state->current_stripe = stripe_metadata_value;
	}

	return true;
}

/*
 * Check the row number against the loaded stripe's inclusive row range.
 */
static bool
is_current_read_stripe(column_read_state *read_state, uint64_t row_number)
{
	if (!stripe_read_in_progress(read_state))
	{
		return false;
	}

	stripe_metadata *current_stripe = read_state->current_stripe;
	if (row_number >= current_stripe->first_row_number &&
		row_number <= stripe_highest_row_number(current_stripe))
	{
		return true;
	}

	return false;
}


/*
 * Return the current stripe metadata, including a stripe selected for the next read.
 */
static stripe_metadata *
get_current_read_stripe(column_read_state *read_state)
{
	return read_state->current_stripe;
}


/*
 * Read the requested row from the loaded stripe Load its chunk group and
 * deletion mask when the group changes. Return false if the row is deleted.
 */
static bool
read_stripe_row_by_number(column_read_state *read_state,
						 uint64_t row_number, Datum *column_values,
						 bool *column_nulls)
{
	stripe_metadata *stripe_metadata_value = get_current_read_stripe(read_state);
	stripe_read_state *stripe_state = read_state->stripe_state;

	if (row_number < stripe_metadata_value->first_row_number)
	{
		/* Reject rows before this stripe before computing an  unsined offset. */
		bioseq_log<logging_level::error, logging_target::postgres>( "row offset cannot be negative");
	}

	/* find the exact chunk group to be read */
	uint64_t stripe_row_offset = row_number - stripe_metadata_value->first_row_number;
	int chunk_group_index = stripe_row_offset / stripe_metadata_value->chunk_group_row_count;
	if (!is_current_chunk_group(stripe_state, chunk_group_index))
	{
		if (stripe_state->chunk_group_state)
		{
			end_chunk_group_read(stripe_state->chunk_group_state);
		}

		stripe_state->chunk_group_index = chunk_group_index;
		stripe_state->chunk_group_state = begin_chunk_group_read(
			stripe_state->buffers,
			stripe_state->chunk_group_index,
			stripe_state->tuple_descriptor,
			stripe_state->projected_columns,
			stripe_state->stripe_read_context,
			stripe_state,
			read_state->current_stripe->id
			);

		uint64_t chunk_first_row_number =
			stripe_metadata_value->first_row_number +
			stripe_state->chunk_group_state->stripe_row_offset;

		if (bioseq::settings::storage::dml::enabled)
		{
			row_mask_row *row_mask_entry =
				find_pending_row_mask(stripe_state->relation->rd_locator.relNumber,
									  GetCurrentSubTransactionId(), row_number);
			if (row_mask_entry != NULL)
			{
				Assert(row_mask_entry->mask.has_value());
				stripe_state->chunk_group_state->row_mask =
					*row_mask_entry->mask;
				stripe_state->chunk_group_state->row_mask_cached = true;
			}
			else if (stripe_state->chunk_group_state->chunk_group_deleted_rows > 0)
			{
				stripe_state->chunk_group_state->row_mask =
					read_chunk_row_mask(stripe_state->relation->rd_locator,
										read_state->snapshot,
										stripe_state->stripe_read_context,
										chunk_first_row_number,
										stripe_state->chunk_group_state->row_count);
				stripe_state->chunk_group_state->row_mask_cached = false;
			}
		}
		else
		{
			stripe_state->chunk_group_state->row_mask = NULL;
		}
	}

	return read_chunk_group_row_by_offset(stripe_state->chunk_group_state,
										stripe_metadata_value, stripe_row_offset,
										column_values, column_nulls);
}


/*
 * Check whether this chunk group is already loaded.
 */
static bool
is_current_chunk_group(stripe_read_state *stripe_state, int chunk_group_index)
{
	if (!stripe_state->chunk_group_state)
	{
		return false;
	}

	return (stripe_state->chunk_group_index == chunk_group_index);
}


/*
 * Position the loaded chunk group at a stripe-relative row offset and read it.
 * Return false if the row was deleted, even if, the reader found a later row.
 */
static bool
read_chunk_group_row_by_offset(chunk_group_read_state *chunk_group_state,
							 stripe_metadata *stripe_metadata_value,
							 uint64_t stripe_row_offset, Datum *column_values,
							 bool *column_nulls)
{
	/* Convert the stripe offset to a row position within this chunk group. */
	chunk_group_state->current_row = stripe_row_offset %
									  stripe_metadata_value->chunk_group_row_count;
	int is_deleted = 0;
	read_next_chunk_group_row(chunk_group_state, column_values, column_nulls,
						  &is_deleted);
	return !is_deleted;
}


/*
 * Check whether a stripe has been loaded.
 */
static bool
stripe_read_in_progress(column_read_state *read_state)
{
	return read_state->stripe_state != NULL;
}


/*
 * Check whether stripe metadata is available for the next  read.
 */
static bool
has_unread_stripe(column_read_state *read_state)
{
	return read_state->current_stripe != NULL;
}


/*
 * Rewind the reader and replace its filter clauses for the next scan.
 */
void
rescan_column_reader(column_read_state *read_state, List *scan_qual)
{
	bioseq::postgres::support::memory_context_scope ctx_guard{read_state->scan_context};

	reset_column_read(read_state);

	/* Start again at the first stripe. */
	advance_stripe_read(read_state);

	read_state->filtered_chunk_groups = 0;

	read_state->filter_clauses = copyObject(scan_qual);
}


/*
 * Free the stripe buffers and reader state, including any snapshot registration
 * acquired while flushing this reader's pending writes.
 */
void
end_column_read(column_read_state *read_state)
{
	if (read_state->owns_snapshot_registration)
	{
		/*
	 * A snapshot registered while flushing writes belongs to this reader.
	 */
		UnregisterSnapshot(read_state->snapshot);
	}

	MemoryContextDelete(read_state->stripe_read_context);
	if (read_state->current_stripe)
	{
		pfree(read_state->current_stripe);
	}

	pfree(read_state);
}


/*
 * Forget the loaded stripe and free its allocations. The reader can then load
 * another stripe using the same memory context.
 */
void
reset_column_read(column_read_state *read_state)
{
	if (stripe_read_in_progress(read_state))
	{
		pfree(read_state->current_stripe);
		read_state->current_stripe = NULL;

		read_state->stripe_state = NULL;
		MemoryContextReset(read_state->stripe_read_context);
	}
}


/*
 * Load  a stripe's requested columns and the chunk groups allowed by its filters.
 * Allocate the resulting state in the supplied strixe context
 */
static stripe_read_state *
begin_stripe_read(stripe_metadata *stripe_metadata_value, Relation rel, TupleDesc tuple_desc,
				List *projected_columns, List *filter_clauses, List *filter_variables,
				MemoryContext stripe_read_context, Snapshot snapshot)
{
	bioseq::postgres::support::memory_context_scope ctx_guard{stripe_read_context};

	stripe_read_state *stripe_state = bioseq::postgres::support::allocate_zeroed_object<stripe_read_state>();

	stripe_state->relation = rel;
	stripe_state->tuple_descriptor = tuple_desc;
	stripe_state->column_count = tuple_desc->natts;
	stripe_state->chunk_group_state = NULL;
	stripe_state->projected_columns = projected_columns;
	stripe_state->stripe_read_context = stripe_read_context;

	stripe_state->buffers = load_filtered_stripe_buffers(rel,
														   stripe_metadata_value,
														   tuple_desc,
															   projected_columns,
															   filter_clauses,
															   filter_variables,
															   &stripe_state->
															   filtered_chunk_groups,
															   snapshot);

	stripe_state->row_count = stripe_state->buffers->row_count;

	return stripe_state;
}


/*
 * Finish the current stripe and select the next visible stripe. Parallel
 * readers claim stripes through the shared scan position.
 */
static void
advance_stripe_read(column_read_state *read_state)
{
	bioseq::postgres::support::memory_context_scope ctx_guard{read_state->scan_context};

	if (read_state->parallel_scan == 0)
	{
		/* Begin at the first row when no stripe has been read. */
		uint64_t last_read_row_number = invalid_column_row_number;
		if (stripe_read_in_progress(read_state))
		{
			/* Continue after the loaded stripe's row range. */
			last_read_row_number = stripe_highest_row_number(read_state->current_stripe);

			read_state->filtered_chunk_groups +=
				read_state->stripe_state->filtered_chunk_groups;
		}

		read_state->current_stripe = find_next_stripe_by_row_number(read_state->relation,
																		last_read_row_number,
																		read_state->snapshot);
	}
	else
	{
		if (stripe_read_in_progress(read_state))
		{
			read_state->filtered_chunk_groups +=
				read_state->stripe_state->filtered_chunk_groups;
		}

		auto *parallel_scan = read_state->parallel_scan;
		for (;;)
		{
			/*
			 * Aborted writes can leave gaps in stripe IDs, so find the next stored
			 * stripe in the catalog. Keep that lookup outside the spinlock.
			 * It can be  interrupted, and an error while holding the lock would
			 * leave other workers waiting for a lock that is never released.
			 */
			uint64 claimed_id = pg_atomic_read_u64(&parallel_scan->next_stripe_id);
			constexpr uint64 max_stripe_id = static_cast<uint64>(
				std::numeric_limits<std::int64_t>::max());
			constexpr uint64 eof_sentinel = max_stripe_id + 1u;
			if (claimed_id == eof_sentinel)
			{
				read_state->current_stripe = nullptr;
				break;
			}
			if (claimed_id == 0u || claimed_id > max_stripe_id)
			{
				bioseq_log<logging_level::error, logging_target::postgres>(
					"bioseq parallel scan cursor is outside the stripe-id domain");
			}

			std::uint64_t lookup_found_id = claimed_id;
			stripe_metadata *candidate = find_next_stripe_for_parallel_worker(
				read_state->relation, read_state->snapshot, claimed_id,
				&lookup_found_id);

			if (candidate == nullptr)
			{
				read_state->current_stripe = nullptr;
				break;
			}

			if (lookup_found_id < claimed_id
				|| lookup_found_id > max_stripe_id)
			{
				pfree(candidate);
				bioseq_log<logging_level::error, logging_target::postgres>(
					"bioseq parallel scan returned an invalid stripe id");
			}

			/*
			 * Claim this stripe only if, another worker has not advanced the shared
			 * position. Retry the lookup if that worker reached it first.
			 */
			const uint64 found_id = static_cast<uint64>(lookup_found_id);
			const uint64 successor = found_id + 1;
			uint64 expected = claimed_id;
			if (pg_atomic_compare_exchange_u64(
					&parallel_scan->next_stripe_id, &expected, successor))
			{
				read_state->current_stripe = candidate;
				break;
			}

			if (candidate != nullptr)
			{
				pfree(candidate);
			}
		}
	}

	if (read_state->current_stripe &&
		stripe_write_state_for(read_state->current_stripe) != stripe_write_state::flushed &&
		!snapshot_might_see_unflushed_stripes(read_state->snapshot))
	{
		/*
		 * Normal scan snapshots should only reach flushed stripes. Snapshots
		 * used for visibility checks can also see incomplete writes, which this
		 * scan skips because their row data is not ready.
		 */
		bioseq_log<logging_level::error, logging_target::postgres>( k_unexpected_stripe_read_error_message,
			   RelationGetRelationName(read_state->relation),
			   read_state->current_stripe->id);
	}

	while (read_state->current_stripe &&
		   stripe_write_state_for(read_state->current_stripe) != stripe_write_state::flushed)
	{
		read_state->current_stripe =
			find_next_stripe_by_row_number(read_state->relation,
										  read_state->current_stripe->first_row_number,
										  read_state->snapshot);
	}

	read_state->stripe_state = NULL;
	MemoryContextReset(read_state->stripe_read_context);
}


/*
 * Some  visibility-check snapshots can see stripes whose writes are pending or
 * aborted. Their metadata may be visible before readable row data exists.
 */
static bool
snapshot_might_see_unflushed_stripes(Snapshot snapshot)
{
	if (snapshot == InvalidSnapshot)
	{
		return false;
	}

	switch (snapshot->snapshot_type)
	{
		case SNAPSHOT_ANY:
		case SNAPSHOT_DIRTY:
		case SNAPSHOT_NON_VACUUMABLE:
		{
			return true;
		}

		default:
			return false;
	}
}


/*
 * Read the neft undeleted row in this stripe, moving to another chunk group
 * when needed. Return false after all selected groups have been read
 */
static bool
read_next_stripe_row(stripe_read_state *stripe_state, Datum *column_values,
				  bool *column_nulls,
				  uint64_t stripe_first_row_number,
				  Snapshot snapshot, uint64_t stripe_id)
{
	if (stripe_state->current_row >= stripe_state->row_count)
	{
		Assert(stripe_state->current_row == stripe_state->row_count);
		return false;
	}

	while (true)
	{
		if (stripe_state->chunk_group_state == NULL)
		{
			stripe_state->chunk_group_state = begin_chunk_group_read(
				stripe_state->buffers,
				stripe_state->
				chunk_group_index,
				stripe_state->
				tuple_descriptor,
				stripe_state->
				projected_columns,
				stripe_state->
				stripe_read_context,
				stripe_state,
				stripe_id
				);

			if (bioseq::settings::storage::dml::enabled &&
				stripe_state->chunk_group_state->chunk_group_deleted_rows != 0)
			{
				uint64_t chunk_first_row_number =
					stripe_first_row_number +
					stripe_state->chunk_group_state->stripe_row_offset;
				stripe_state->chunk_group_state->row_mask =
					read_chunk_row_mask(stripe_state->relation->rd_locator,
									 snapshot,
									 stripe_state->stripe_read_context,
									 chunk_first_row_number,
									 stripe_state->chunk_group_state->row_count);
				stripe_state->chunk_group_state->row_mask_cached = false;
			}
			else
			{
				stripe_state->chunk_group_state->row_mask = NULL;
			}
		}

		int32_t deleted_columns_number = 0;
		if (!read_next_chunk_group_row(stripe_state->chunk_group_state, column_values,
								   column_nulls, &deleted_columns_number))
		{
			/* This chunk group is exhausted. Load the next one. */

			stripe_state->current_row += deleted_columns_number;

			bool is_stripe_exhausted = stripe_state->current_row >= stripe_state->row_count;

			end_chunk_group_read(stripe_state->chunk_group_state);
			stripe_state->chunk_group_state = NULL;

			if (is_stripe_exhausted)
				return false;

			stripe_state->chunk_group_index++;

			continue;
		}

		stripe_state->current_row += 1 + deleted_columns_number;
		return true;
	}
}


/*
 * Count chunk groups skipped in stripes the reader has finished.
 */
int64_t
read_chunk_groups_filtered(column_read_state *state)
{
	return state->filtered_chunk_groups;
}

	/* Vectorization */

bool
read_next_column_vector(column_read_state *read_state,  Datum *column_values,
					   bool *column_nulls, std::uint64_t *row_number, int *new_vector_size)
{
	while (true)
	{
		if (!stripe_read_in_progress(read_state))
		{
			if (!has_unread_stripe(read_state))
			{
				return false;
			}

			read_state->stripe_state = begin_stripe_read(read_state->current_stripe,
												 read_state->relation,
												 read_state->tuple_descriptor,
												 read_state->projected_columns,
												 read_state->filter_clauses,
												 read_state->filter_variables,
												 read_state->stripe_read_context,
												 read_state->snapshot);
		}

		if (!read_next_stripe_vector(read_state->stripe_state, column_values, column_nulls,
								  new_vector_size,
								  read_state->current_stripe->id,
								  read_state->snapshot,
								  row_number,
								  read_state->current_stripe->first_row_number))
		{
			advance_stripe_read(read_state);

			// Return rows already collected before loading another stripe.
			if (*new_vector_size)
				return true;

			continue;
		}

		return true;
	}
}


/*
 * Append this stripe's undeleted rows to  the output vectors.
 * Stop when the vectors are full or a completed chunk group has rows to return.
 */
static bool
read_next_stripe_vector(stripe_read_state *stripe_state, Datum *column_values,
					 bool *column_nulls, int *new_vector_size,
					 uint64 stripe_id,
					 Snapshot snapshot,
					 std::uint64_t *row_number,
					 uint64 stripe_first_row_number)
{
	if (stripe_state->current_row >= stripe_state->row_count)
	{
		Assert(stripe_state->current_row == stripe_state->row_count);
		return false;
	}

	int32_t *column_value_offset = bioseq::postgres::support::allocate_zeroed_array<int32_t>(stripe_state->tuple_descriptor->natts);
	memset(column_value_offset, 0, sizeof(int32_t) * stripe_state->tuple_descriptor->natts);

	while (true)
	{
		uint64_t chunk_first_row_number = 0;

		if (stripe_state->chunk_group_state == NULL)
		{
			stripe_state->chunk_group_state = begin_chunk_group_read(
				stripe_state->buffers,
				stripe_state->
				chunk_group_index,
				stripe_state->
				tuple_descriptor,
				stripe_state->
				projected_columns,
				stripe_state->
				stripe_read_context,
				stripe_state,
				stripe_id);

			chunk_first_row_number = stripe_first_row_number +
								  stripe_state->chunk_group_state->stripe_row_offset;

			if (bioseq::settings::storage::dml::enabled &&
				stripe_state->chunk_group_state->chunk_group_deleted_rows != 0)
			{
				stripe_state->chunk_group_state->row_mask =
					read_chunk_row_mask(stripe_state->relation->rd_locator,
									 snapshot,
									 stripe_state->stripe_read_context,
									 chunk_first_row_number,
									 stripe_state->chunk_group_state->row_count);
			}
			else
			{
				stripe_state->chunk_group_state->row_mask = NULL;
			}
		}
		else
		{
			chunk_first_row_number = stripe_first_row_number +
						stripe_state->chunk_group_state->stripe_row_offset;
		}

		if (!read_next_chunk_group_vector(stripe_state->chunk_group_state,
									  column_values, column_nulls,
									  stripe_state->tuple_descriptor,
									  column_value_offset,
									  new_vector_size,
									  row_number,
									  chunk_first_row_number))
		{
			/*
    * Output values may point into this chunk group. Return them before
    * freeing the group or moving to the next one.
    */
			if (*new_vector_size == 0)
			{
				/* No output uses this exhausted group. Move to the next one. */
				end_chunk_group_read(stripe_state->chunk_group_state);
				stripe_state->chunk_group_state = NULL;
				stripe_state->chunk_group_index++;
				continue;
			}
		}
		else
			stripe_state->current_row += stripe_state->chunk_group_state->row_count;

		pfree(column_value_offset);
		return true;
	}
}
