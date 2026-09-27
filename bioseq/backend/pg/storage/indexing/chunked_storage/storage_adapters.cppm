/**
 * @file storage_adapters.cppm
 * @brief Stable facade for PostgreSQL chunked-index storage adapters.
 *
 * Implementation partitions are target-owned object providers rather than
 * interface dependencies.  Their externally declared definitions therefore
 * remain out-of-line so each required ABI symbol has one emitting unit.
 */
export module bioseq.postgres.storage.indexing.chunked_storage:storage_adapters;

export import :storage_adapter_contract;
