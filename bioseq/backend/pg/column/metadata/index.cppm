/**
 * @file index.cppm
 * @brief Export columnar metadata operations and transaction state.
 *
 * The module groups catalog access with pending writes and cached readers,
 * allowing its partitions to share types without circular module imports.
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
