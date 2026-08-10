/**
 * @file
 * @brief Stripe and row traversal for the Bioseq column format.
 *
 * Owns the
 * column_read_state lifecycle: locating stripes via the metadata
 * tables, decompressing chunk groups, applying chunk-group filter
 * pushdowns, and yielding tuples (or vectorised slots) to the
 * executor.
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

/*
 * fmt-style format string used by bioseq_log_pg() when a stripe is unflushed
 * yet the read path tries to consume it. Kept as a `const char*` rather
 * than an inline string so call sites use the same wording.
 */
inline constexpr const char* k_unexpected_stripe_read_error_message =
	"attempted to read an unexpected stripe while reading columnar "
	"table {}, stripe with id={} is not flushed";


export int64_t
read_chunk_groups_filtered(column_read_state *state);
export bool
set_stripe_read_state(column_read_state *read_state,
						   stripe_metadata *start_stripe_metadata);
export bool
read_next_column_row(column_read_state *read_state, Datum *column_values, bool *column_nulls,
					uint64_t *row_number);
export column_read_state *
begin_column_read(Relation relation, TupleDesc tuple_descriptor,
				  List *projected_columns, List *filter_clauses,
				  MemoryContext scan_context, Snapshot snapshot,
				  bool random_access,
				  parallel_column_scan_handle parallel_scan);
export bool
read_next_column_vector(column_read_state *read_state,  Datum *column_values,
					   bool *column_nulls, uint64 *row_number, int *new_vector_size);
export void
rescan_column_reader(column_read_state *read_state, List *scan_qual);
export void
end_column_read(column_read_state *read_state);
export void
read_column_row_by_number_or_error(column_read_state *read_state,
								  uint64_t row_number, Datum *column_values,
								  bool *column_nulls);
export void
flush_pending_writes_for_read(column_read_state *read_state);
export bool
read_column_row_by_number(column_read_state *read_state,
						   uint64_t row_number, Datum *column_values,
						   bool *column_nulls);
void
reset_column_read(column_read_state *read_state);


/* static function declarations */
static MemoryContext create_stripe_read_memory_context(void);
static bool is_current_read_stripe(column_read_state *read_state,
										uint64_t row_number);
static stripe_metadata * get_current_read_stripe(column_read_state *read_state);
static bool read_stripe_row_by_number(column_read_state *read_state,
									 uint64_t row_number, Datum *column_values,
									 bool *column_nulls);
static bool is_current_chunk_group(stripe_read_state *stripe_state,
										  int chunk_group_index);
static bool read_chunk_group_row_by_offset(chunk_group_read_state *chunk_group_state,
										 stripe_metadata *stripe_metadata_value,
										 uint64_t stripe_row_offset, Datum *column_values,
										 bool *column_nulls);
static bool stripe_read_in_progress(column_read_state *read_state);
static bool has_unread_stripe(column_read_state *read_state);
static stripe_read_state * begin_stripe_read(stripe_metadata *stripe_metadata_value, Relation rel,
										 TupleDesc tuple_desc, List *projected_columns,
										 List *filter_clauses, List *filter_variables,
										 MemoryContext stripe_read_context,
										 Snapshot snapshot);
static void advance_stripe_read(column_read_state *read_state);
static bool snapshot_might_see_unflushed_stripes(Snapshot snapshot);
static bool read_next_stripe_row(stripe_read_state *stripe_state, Datum *column_values,
								  bool *column_nulls,
								  uint64_t stripe_first_row_number,
								  Snapshot snapshot, uint64_t stripe_id);

/* Vectorization */
static bool read_next_stripe_vector(stripe_read_state *stripe_state, Datum *column_values,
								 bool *column_nulls, int *new_vector_size,
								 uint64 stripe_id,
								 Snapshot snapshot,
								 uint64 *row_number,
								 uint64 stripe_first_row_number);

/*
 * begin_column_read initializes a columnar read operation. This function returns a
 * read handle that's used during reading rows and finishing the read operation.
 *
 * projected_columns is an integer list of attribute numbers (1-indexed).
 */
column_read_state *
begin_column_read(Relation relation, TupleDesc tuple_descriptor,
				  List *projected_columns, List *filter_clauses,
				  MemoryContext scan_context, Snapshot snapshot,
				  bool random_access,
				  parallel_column_scan_handle parallel_scan)
{
	/*
	 * We allocate all stripe specific data in the stripe_read_context, and reset
	 * this memory context before loading a new stripe. This is to avoid memory
	 * leaks.
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
	 * Note that flush_pending_writes_for_read might update those two by
	 * registering a new snapshot.
	 */
	read_state->snapshot = snapshot;
	read_state->owns_snapshot_registration = false;

	/* Parallel execution */
	read_state->parallel_scan = parallel_scan;

	if (!random_access)
	{
		/*
		 * If have parallel columnar scan we don't flush states.
		 */
		if (read_state->parallel_scan == NULL)
		{
			/*
			* Flush any pending in-memory changes to row_mask metadata
			* for next scan.
			*/
			flush_row_mask_write_state_for_relation_file(read_state->relation->rd_locator.relNumber,
											 GetCurrentSubTransactionId());

			/*
			* When doing random access (i.e.: index scan), we don't need to flush
			* pending writes until we need to read them.
			* bioseq_index_fetch_tuple would do so when needed.
			*/
			flush_pending_writes_for_read(read_state);
		}

		/*
		 * advance_stripe_read sets current_stripe for the first stripe
		 * to read if not doing random access. Otherwise, reader (i.e.:
		 * read_column_row_by_number) would already decide the stripe to read
		 * on-the-fly.
		 *
		 * Moreover, Since we don't flush pending writes for random access,
		 * advance_stripe_read might encounter with stripe metadata entries due
		 * to current transaction's pending writes even when using an MVCC
		 * snapshot, but advance_stripe_read would throw an error for that.
		 * Note that this is not the case with for plain table scan methods
		 * (i.e.: SeqScan and BioseqColumn CustomScan).
		 *
		 * For those reasons, we don't call advance_stripe_read if we will do
		 * random access.
		 */
		advance_stripe_read(read_state);
	}

	return read_state;
}


/*
 * flush_pending_writes_for_read flushes pending writes for read operation
 * and sets a new (registered) snapshot if necessary.
 *
 * If it sets a new snapshot, then sets owns_snapshot_registration to true to
 * indicate that caller should unregister the snapshot after finishing read
 * operation.
 *
 * Note that this function assumes that read_state's relation and snapshot
 * fields are already set.
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
 * create_stripe_read_memory_context creates a memory context to be used when
 * reading a stripe.
 */
static MemoryContext
create_stripe_read_memory_context()
{
	return AllocSetContextCreate(CurrentMemoryContext, "Stripe Read Memory Context",
								 ALLOCSET_DEFAULT_SIZES);
}


/*
 * read_next_column_row tries to read a row from the columnar table. On success, it sets
 * column values, column nulls and row_number (if passed to be non-NULL), and returns true.
 * If there are no more rows to read, the function returns false.
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
 * read_column_row_by_number_or_error is a wrapper around
 * read_column_row_by_number that throws an error if tuple
 * with row_number does not exist.
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
 * read_column_row_by_number reads row with row_number from given relation
 * into column_values and column_nulls, and returns true. If no such row
 * exists, then returns false.
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
			/* no such row exists */
			return false;
		}

		if (stripe_write_state_for(stripe_metadata_value) != stripe_write_state::flushed)
		{
			/*
			 * Callers are expected to skip stripes that are not flushed to
			 * disk yet or should wait for the writer xact to commit or abort,
			 * but let's be on the safe side.
			 */
			bioseq_log<logging_level::error, logging_target::postgres>( k_unexpected_stripe_read_error_message,
				   RelationGetRelationName(bioseq_column_relation),
				   stripe_metadata_value->id);
		}

		/* do the cleanup before reading a new stripe */
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
 * set_stripe_read_state
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
			/* no such row exists */
			return false;
		}

		if (stripe_write_state_for(stripe_metadata_value) != stripe_write_state::flushed)
		{
			/*
			 * Callers are expected to skip stripes that are not flushed to
			 * disk yet or should wait for the writer xact to commit or abort,
			 * but let's be on the safe side.
			 */
			bioseq_log<logging_level::error, logging_target::postgres>( k_unexpected_stripe_read_error_message,
				   RelationGetRelationName(bioseq_column_relation),
				   stripe_metadata_value->id);
		}

		/* do the cleanup before reading a new stripe */
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
 * is_current_read_stripe returns true if stripe being read contains
 * row with given row_number.
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
 * get_current_read_stripe returns stripe_metadata for the stripe that is
 * being read.
 */
static stripe_metadata *
get_current_read_stripe(column_read_state *read_state)
{
	return read_state->current_stripe;
}


/*
 * read_stripe_row_by_number reads row with row_number from given
 * stripe_state into column_values and column_nulls.
 * Errors out if no such row exists in the stripe being read.
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
		/* not expected but be on the safe side */
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
 * is_current_chunk_group returns true if chunk group being read is
 * the has given chunk_group_index in its stripe.
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
 * read_chunk_group_row_by_offset reads row with stripe_row_offset from given
 * chunk_group_state into column_values and column_nulls.
 * Errors out if no such row exists in the chunk group being read.
 */
static bool
read_chunk_group_row_by_offset(chunk_group_read_state *chunk_group_state,
							 stripe_metadata *stripe_metadata_value,
							 uint64_t stripe_row_offset, Datum *column_values,
							 bool *column_nulls)
{
	/* set the exact row number to be read from given chunk roup */
	chunk_group_state->current_row = stripe_row_offset %
									  stripe_metadata_value->chunk_group_row_count;
	int is_deleted = 0;
	read_next_chunk_group_row(chunk_group_state, column_values, column_nulls,
						  &is_deleted);
	return !is_deleted;
}


/*
 * stripe_read_in_progress returns true if we already started reading a stripe.
 */
static bool
stripe_read_in_progress(column_read_state *read_state)
{
	return read_state->stripe_state != NULL;
}


/*
 * has_unread_stripe returns true if we still have stripes to read during current
 * read operation.
 */
static bool
has_unread_stripe(column_read_state *read_state)
{
	return read_state->current_stripe != NULL;
}


/*
 * rescan_column_reader clears the position where we were scanning so that the next read starts at
 * the beginning again
 */
void
rescan_column_reader(column_read_state *read_state, List *scan_qual)
{
	bioseq::postgres::support::memory_context_scope ctx_guard{read_state->scan_context};

	reset_column_read(read_state);

	/* set current_stripe for the first stripe to read */
	advance_stripe_read(read_state);

	read_state->filtered_chunk_groups = 0;

	read_state->filter_clauses = copyObject(scan_qual);
}


/*
 * Finishes a columnar read operation.
 */
void
end_column_read(column_read_state *read_state)
{
	if (read_state->owns_snapshot_registration)
	{
		/*
		 * init_bioseq_read_state created a new snapshot and registered it,
		 * so now forget it.
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
 * reset_column_read resets the stripe and the chunk group that is
 * being read currently (if any).
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
 * begin_stripe_read allocates state for reading a stripe.
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
 * advance_stripe_read updates filtered_chunk_groups and sets
 * current_stripe for next stripe read.
 */
static void
advance_stripe_read(column_read_state *read_state)
{
	bioseq::postgres::support::memory_context_scope ctx_guard{read_state->scan_context};

	if (read_state->parallel_scan == 0)
	{
		/* if not read any stripes yet, start from the first one .. */
		uint64_t last_read_row_number = invalid_column_row_number;
		if (stripe_read_in_progress(read_state))
		{
			/* .. otherwise, continue with the next stripe */
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
			/* Stripe ids are relation-local, and aborted reservations may leave
			 * gaps.  Resolve the next relation-local id without holding a
			 * spinlock: the catalog/index scan is interruptible, and a parallel
			 * worker terminated while holding a spinlock would poison the DSM
			 * lock for every peer. */
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

			/* CAS makes the lookup and gap skip one logical claim.  A peer
			 * that raced on the same candidate retries from the winner's
			 * successor, so no stripe is duplicated. */
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
		 * To be on the safe side, error out if we don't expect to encounter
		 * with an un-flushed stripe. Otherwise, we will skip such stripes
		 * until finding a flushed one.
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
 * snapshot_might_see_unflushed_stripes returns true if given snapshot is
 * expected to see un-flushed stripes either because of other backends'
 * pending writes or aborted transactions.
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
 * read_next_stripe_row: If more rows can be read from the current stripe, fill
 * in non-NULL column_values and return true. Otherwise, return false.
 *
 * On entry, all entries in column_nulls should be true; this function only
 * sets non-NULL entries.
 *
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
			/* if this chunk group is exhausted, fetch the next one and loop */

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
 * read_chunk_groups_filtered
 *
 * Return the number of chunk groups filtered during this read operation.
 */
int64_t
read_chunk_groups_filtered(column_read_state *state)
{
	return state->filtered_chunk_groups;
}

	/* Vectorization */

bool
read_next_column_vector(column_read_state *read_state,  Datum *column_values,
					   bool *column_nulls, uint64 *row_number, int *new_vector_size)
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

			// We have collected rows from last stripe, break loop
			if (*new_vector_size)
				return true;

			continue;
		}

		return true;
	}
}


static bool
read_next_stripe_vector(stripe_read_state *stripe_state, Datum *column_values,
					 bool *column_nulls, int *new_vector_size,
					 uint64 stripe_id,
					 Snapshot snapshot,
					 uint64 *row_number,
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
			 * if *new_vector_size is non-zero, it means there might be some data in
			 * chunk_group_state->chunk_group_data referenced by vector_column.  In
			 * this case, do NOT release chunk_group_read_state.
			 */
			if (*new_vector_size == 0)
			{
				/* if this chunk group is exhausted, fetch the next one and loop */
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
