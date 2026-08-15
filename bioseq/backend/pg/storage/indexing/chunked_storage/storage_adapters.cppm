/**
 * @file storage_adapters.cppm
 * @brief Stable facade for PostgreSQL chunked-index storage adapters.
 */
export module bioseq.postgres.storage.indexing.chunked_storage:storage_adapters;

export import :storage_adapter_contract;
export import :fragment_storage_lifecycle;
export import :fragment_storage_catalog;
export import :fragment_storage_io;
export import :storage_relation_cleanup;
export import :fragment_storage_spill;
export import :table_storage_iteration;
export import :storage_adapter_conformance;
