/**
 * @file types.cppm
 * @brief Shared types for columnar metadata, readers and buffered writes.
 *
 * Keep these definitions separate from catalog access so table I/O and
 * transaction callbacks can share them without importing catalog operations.
 */
module;
#include <bioseq/pg_compat.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

export module bioseq.postgres.storage.metadata:types;

import bioseq.postgres.configuration.core;
export import bioseq.postgres.storage.parallel_scan_contract;

// Other modules share these scan and write-state layouts.
// Keep the layout checks below consistent with any field changes.

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
	/* Value range and row count used when filtering this chunk. */
	bool has_min_max;
	Datum minimum_value;
	Datum maximum_value;
	uint64 row_count;

	/* Locate the value and presence-bit streams without reading earlier chunks. */
	uint64 value_stream_offset;
	uint64 value_stream_length;
	uint64 exists_stream_offset;
	uint64 exists_stream_length;

	/* Original value-buffer  size, used for decompression and compression statistics */
	uint64 value_decompressed_length;

	bioseq::compression_algorithm value_compression_algorithm;
	int value_compression_level;
};

/** @brief Chunk metadata used to skip row groups that cannot match a query.
 * Access a column chunk through chunk_skip_nodes[column][chunk].
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

/// @brief String buffer marked when its storage belongs to the backend chunk cache.
export struct cached_string_info
{
	StringInfoData data;
	bool cached;
};

/** @brief Decoded values and presence flags for a chunk group.
 *
 * present_values and values_by_column have matching [column][row] entries.
 * referenced_value_buffers keeps the bytes pointed to by by-reference Datums.
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

	/* Keep bytes referenced by values_by_column alide. */
	StringInfo *referenced_value_buffers;
};

/** @brief Serialized values and presence bits for one column chunk.
 * value_compression_algorithm describes the value buffer. The saved original
 * byte length lets readers allocate  space for decompression.
 */
export struct chunk_buffers
{
	StringInfo presence_buffer;
	StringInfo value_buffer;
	bioseq::compression_algorithm value_compression_algorithm;
	uint64 value_decompressed_length;
};

/** @brief Serialized chunks belonging to one column in a stripe. */
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

/// @brief Whether a stripe is flushed, aborted or still buffered by a writer.
export enum class stripe_write_state
{
	/* Stripe, data has been flushed and can be read under a suitable snapshot. */
	flushed,

	/* The transaction that reserved the stripe aborted. */
	aborted,

	/* The stripe is still buffered by this transaction or another writer. */
	in_progress
};

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

	/* The transaction that inserted the stripe row  aborted. */
	bool aborted;

	/* Identify our own unflushed stripes when checking their visibility. */
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
	bool row_mask_cached; /* True when row_mask is borrowed from the cache. */
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

	/* Needed column numbers, starting at one. */
	List *projected_columns;

	List *filter_clauses;
	List *filter_variables;

	MemoryContext stripe_read_context;
	int64 filtered_chunk_groups;

	/* Owns scan allocations and must remain alive until the scan ends. */
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

	/* Reuse this scratch buffer for compression within a stripe.
	 * Resetting stripe_write_context releases it */
	StringInfo compression_buffer;

	/* Sequence-shard caches for this writer. Keep their allocation in the
	 * parent of stripe_write_context so stripe resets leave them alive.
	 * The registered cleanup hook closes and destroys the caches. void*
	 * avoids a  module dependency on bioseq::storage::seq_caches. */
	void *seq_caches;
};

// Compare shared record sizes and seleted field offsets. For the parallel
// state, check where the trailing snapshot begins because its size varies.
namespace {
/// @brief Reference layout used to check column_read_state across modules.
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

/// @brief Reference layout used to check column_write_state across modules.
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

/// @brief Mirrors the fixed header of shared parallel-scan state.
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
