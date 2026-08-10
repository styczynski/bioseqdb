/*-------------------------------------------------------------------------
 *
 * index.cppm
 *
 * Primary interface unit for the `storage_metadata` module. It is a tiny
 * umbrella that re-exports the partitions making up the module: the
 * metadata core, the columnar/row-mask write-state machinery, the
 * write-state lifecycle dispatchers and the read-state cache.
 *
 * The partitions share one ownership boundary, avoiding dependency cycles
 * between catalog access and transaction-state lifecycle code.
 *
 *-------------------------------------------------------------------------
 */
export module bioseq.postgres.storage.metadata;

export import :types;
export import :options;
export import :stripe;
export import :row_mask;
export import :seq_sub_table;
export import :tuple_ops;
export import :core;
export import :write_state;
export import :write_state_row_mask;
export import :write_state_interface;
export import :read_state_cache;
