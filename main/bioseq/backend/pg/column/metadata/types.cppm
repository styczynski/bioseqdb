/*-------------------------------------------------------------------------
 *
 * types.cppm
 *
 * Shared storage-metadata data model and ABI guards. Table I/O, transaction
 * state, and catalog operations depend on this leaf partition without pulling
 * in catalog implementation details.
 *
 *-------------------------------------------------------------------------
 */
module;
#include <bioseq/pg_compat.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

export module bioseq.postgres.storage.metadata:types;

import bioseq.postgres.configuration.core;

// Field order and types in the scan and write-state records are ABI-shared.
// The layout assertions below must remain synchronized with these definitions.

/// @brief Resolved options for reading or writing a column table.
export struct column_options
{
	uint64 stripe_row_limit;
	uint32 chunk_group_row_limit;
	bioseq::compression_algorithm compression_algorithm;
	int compression_level;
};

/// @brief Statistics and storage locations for one column chunk.
export struct chunk_skip_node
{
	/* statistics about values of a column chunk */
	bool has_min_max;
	Datum minimum_value;
	Datum maximum_value;
	uint64 row_count;

	/*
	 * Offsets and sizes of value and exists streams in the column data.
	 * These enable us to skip reading suppressed row chunks, and start reading
	 * a chunk without reading previous chunks.
	 */
	uint64 value_stream_offset;
	uint64 value_stream_length;
	uint64 exists_stream_offset;
	uint64 exists_stream_length;

	/*
	 * This is used for (1) determining destination size when decompressing,
	 * (2) calculating compression rates when logging stats.
	 */
	uint64 value_decompressed_length;

	bioseq::compression_algorithm value_compression_algorithm;
	int value_compression_level;
};

/** @brief Chunk metadata used to skip irrelevant row groups.
 *
 * Contains a column chunk
 * skip node for each chunk of each column. chunk_skip_nodes[column][chunk]
 * is the entry for the specified column chunk.
 */
export struct stripe_skip_list
{
	chunk_skip_node **chunk_skip_nodes;
	uint32 *chunk_group_row_counts;
	uint32 *chunk_group_row_offsets;
	uint32 *chunk_group_deleted_rows;
	uint32 column_count;
	uint32 chunk_count;
};

/// @brief Reusable PostgreSQL string buffer with an ownership marker.
export struct cached_string_info
{
	StringInfoData data;
	bool cached;
};

/** @brief Decompressed values and presence bits for a row chunk.
 *
 * values_by_column stores
 * the values of data, and present_values stores whether a value is present.
 * value_buffer is used to store (uncompressed) serialized values referenced by
 * Datum's in values_by_column. It is only used for by-reference Datum's. There is a
 * one-to-one correspondence between values_by_column and present_values.
 */
export struct chunk_data
{
	uint32 row_count;
	uint32 column_count;

	/*
	 * Following are indexed by [column][row]. If a column is not projected,
	 * then present_values[column] and values_by_column[column] are null.
	 */
	bool **present_values;
	Datum **values_by_column;

	/* value_buffer keeps actual data for type-by-reference datums from values_by_column. */
	StringInfo *referenced_value_buffers;
};

/** @brief Serialized presence and value streams for a column chunk.
 *
 * value_buffer stores the serialized values of data, and presence_buffer stores
 * serialized value of presence information. value_compression_algorithm contains
 * compression type if value_buffer is compressed. Finally row_count has the
 * number of rows in this chunk.
 */
export struct chunk_buffers
{
	StringInfo presence_buffer;
	StringInfo value_buffer;
	bioseq::compression_algorithm value_compression_algorithm;
	uint64 value_decompressed_length;
};

/** @brief Serialized chunks for one column in a stripe.
 *
 * column is made of multiple column chunks.
 */
export struct column_buffers
{
	chunk_buffers **chunks;
};

/// @brief Serialized column buffers and selected row-group metadata for a stripe.
export struct stripe_buffers
{
	uint32 column_count;
	uint32 chunk_count;
	uint32 row_count;
	column_buffers **columns;

	uint32 *selected_chunk_group_row_counts;
	uint32 *selected_chunk_group_row_offsets;
	uint32 *selected_chunk_group_deleted_rows;
};

/// @brief Transaction visibility state of a stripe catalog row.
export enum class stripe_write_state
{
	/* stripe write is flushed to disk, so it's readable */
	flushed,

	/*
	 * Writer transaction did abort either before inserting into
	 * bioseq.stripe or after.
	 */
	aborted,

	/*
	 * Writer transaction is still in-progress. Note that it is not certain
	 * if it is being written by current backend's current transaction or
	 * another backend.
	 */
	in_progress
};

/// @brief Pointer-free shared state for a parallel column scan.
export struct parallel_column_scan_state
{
	pg_atomic_uint64 next_stripe_id;	/* CAS cursor; claims may skip aborted-id gaps */
	/* The fixed instrumentation header precedes the snapshot flexible array. */
	std::uint32_t version;
	std::uint64_t counters_offset;
	std::uint32_t entry_count;
	std::uint32_t slot_count;
	std::uint64_t slot_stride;
	char snapshot_data[FLEXIBLE_ARRAY_MEMBER];
};
export using parallel_column_scan_handle = parallel_column_scan_state*;

/// @brief Aggregate cache counters reported by column scans.
export struct column_cache_statistics
{
	uint64 hits;
	uint64 misses;
	uint64 evictions;
	uint64 writes;
	uint64 maximum_cache_size;
	uint64 ending_cache_size;
	uint64 entries;
};

/// @brief In-memory representation of one stripe catalog row.
export struct stripe_metadata
{
	uint64 file_offset;
	uint64 data_length;
	uint32 column_count;
	uint32 chunk_count;
	uint32 chunk_group_row_count;
	uint64 row_count;
	uint64 id;
	uint64 first_row_number;

	/* see stripe_write_state_for */
	bool aborted;

	/*
	 * If write operation is in-progress (i.e. stripe_write_state_for returned
	 * stripe_write_state::in_progress), then inserted_by_current_transaction is used to
	 * distinguish whether it's being written by current transaction or not.
	 */
	bool inserted_by_current_transaction;
};

/// @brief Reserved identity and first row number for a pending stripe.
export struct empty_stripe_reservation
{
	uint64 stripe_id;
	uint64 stripe_first_row_number;
};

/// @brief Cursor and decoded values for one row group.
export struct chunk_group_read_state
{
	int64 current_row;
	int64 row_count;
	int column_count;
	List *projected_columns;  /* borrowed reference */
	chunk_data *chunk_group_data;
	bytea *row_mask;
	bool row_mask_cached; /* If row_mask metadata is cached and borrowed */
	uint32 stripe_row_offset;
	uint32 chunk_group_deleted_rows;
};

/// @brief Per-stripe cursor state owned by a column scan.
export struct stripe_read_state
{
	int column_count;
	int64 row_count;
	int64 current_row;
	TupleDesc tuple_descriptor;
	Relation relation;
	int chunk_group_index;
	int64 filtered_chunk_groups;
	MemoryContext stripe_read_context;
	stripe_buffers *buffers;   /* allocated in stripe_read_context */
	List *projected_columns;      /* borrowed reference */
	chunk_group_read_state *chunk_group_state; /* owned */
};

/// @brief Transaction and cursor state for a column scan.
export struct column_read_state
{
	TupleDesc tuple_descriptor;
	Relation relation;

	stripe_metadata *current_stripe;
	stripe_read_state *stripe_state;

	/*
	 * Integer list of attribute numbers (1-indexed) for columns needed by the
	 * query.
	 */
	List *projected_columns;

	List *filter_clauses;
	List *filter_variables;

	MemoryContext stripe_read_context;
	int64 filtered_chunk_groups;

	/*
	 * Memory context guaranteed to be not freed during scan so we can safely
	 * use for any memory allocations regarding column_read_state itself.
	 */
	MemoryContext scan_context;

	Snapshot snapshot;
	bool owns_snapshot_registration;

	/* Parallel execution state, or null for a serial scan. */
	parallel_column_scan_handle parallel_scan;
};

/// @brief Buffered state for a column write operation.
export struct column_write_state
{
	TupleDesc tuple_descriptor;
	FmgrInfo **comparison_functions;
	RelFileLocator relation_file_locator;

	MemoryContext stripe_write_context;
	MemoryContext tuple_context;
	stripe_buffers *buffers;
	stripe_skip_list *skip_list;
	empty_stripe_reservation *stripe_reservation;
	column_options options;
	chunk_data *pending_chunk;

	List *chunk_group_row_counts;

	/*
	 * compression_buffer buffer is used as temporary storage during data value
	 * compression operation. It is kept here to minimize memory allocations.
	 * It lives in stripe_write_context and gets deallocated when memory context
	 * is reset.
	 */
	StringInfo compression_buffer;

	/*
	 * Per-write sequence-subtable cache. Opaque void* keeps this ABI record
	 * decoupled
	 * from bioseq::storage; the actual `bioseq::storage::seq_caches*` is
	 * placement-new'd into stripe_write_context's stable owning parent and
	 * destroyed by the registered teardown hook. It must outlive every reset
	 * of the stripe scratch context and retains the per-write cap snapshot.
	 */
	void *seq_caches;
};

// ABI mirrors pin size and representative offsets. The parallel state has a
// flexible array, so its fixed-header offset is checked instead of sizeof.
namespace {
struct column_read_state_layout
{
	TupleDesc tuple_descriptor;
	Relation relation;
	stripe_metadata *current_stripe;
	stripe_read_state *stripe_state;
	List *projected_columns;
	List *filter_clauses;
	List *filter_variables;
	MemoryContext stripe_read_context;
	int64 filtered_chunk_groups;
	MemoryContext scan_context;
	Snapshot snapshot;
	bool owns_snapshot_registration;
	parallel_column_scan_handle parallel_scan;
};
static_assert(std::is_standard_layout_v<column_read_state> &&
			  sizeof(column_read_state) == sizeof(column_read_state_layout) &&
			  offsetof(column_read_state, tuple_descriptor) == offsetof(column_read_state_layout, tuple_descriptor) &&
			  offsetof(column_read_state, parallel_scan) == offsetof(column_read_state_layout, parallel_scan),
			  "column_read_state layout drift (reinterpret_cast'd across modules)");

struct column_write_state_layout
{
	TupleDesc tuple_descriptor;
	FmgrInfo **comparison_functions;
	RelFileLocator relation_file_locator;
	MemoryContext stripe_write_context;
	MemoryContext tuple_context;
	stripe_buffers *buffers;
	stripe_skip_list *skip_list;
	empty_stripe_reservation *stripe_reservation;
	column_options options;
	chunk_data *pending_chunk;
	List *chunk_group_row_counts;
	StringInfo compression_buffer;
	void *seq_caches;
};
static_assert(std::is_standard_layout_v<column_write_state> &&
			  sizeof(column_write_state) == sizeof(column_write_state_layout) &&
			  offsetof(column_write_state, tuple_descriptor) == offsetof(column_write_state_layout, tuple_descriptor) &&
			  offsetof(column_write_state, seq_caches) == offsetof(column_write_state_layout, seq_caches),
			  "column_write_state layout drift (reinterpret_cast'd across modules)");

struct parallel_column_scan_state_layout
{
	pg_atomic_uint64 next_stripe_id;
	std::uint32_t version;
	std::uint64_t counters_offset;
	std::uint32_t entry_count;
	std::uint32_t slot_count;
	std::uint64_t slot_stride;
	char snapshot_data[FLEXIBLE_ARRAY_MEMBER];
};
static_assert(std::is_standard_layout_v<parallel_column_scan_state>,
				  "parallel_column_scan_state must stay standard-layout (FAM struct, reinterpret_cast'd across modules)");
static_assert(offsetof(parallel_column_scan_state, snapshot_data)
				  == offsetof(parallel_column_scan_state_layout, snapshot_data),
				  "parallel_column_scan_state fixed header drift");
}  // namespace
