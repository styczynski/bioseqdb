/*-------------------------------------------------------------------------
 *
 * storage_metadata.cppm
 *
 * Primary interface unit for the `storage_metadata` module. It is a tiny
 * umbrella that re-exports the partitions making up the module: the
 * metadata core, the columnar/row-mask write-state machinery, the
 * write-state lifecycle dispatchers and the read-state cache.
 *
 * DOC0322 §7.1 — folding the former standalone write-state trio and the
 * read-state cache into partitions of this module dissolves the
 * `extern "C++"` cross-module cycle that previously connected them.
 *
 *-------------------------------------------------------------------------
 */
export module storage_metadata;

export import :types;
export import :options;
export import :stripe;
export import :row_mask;
export import :seq_sub_table;
export import :core;
export import :write_state;
export import :write_state_row_mask;
export import :write_state_interface;
export import :read_state_cache;
