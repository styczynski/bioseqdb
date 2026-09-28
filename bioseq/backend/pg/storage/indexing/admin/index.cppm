/**
 * @file admin/index.cppm
 * @brief Exports the PostgreSQL index administration and test interfaces.
 */
export module bioseq.postgres.storage.indexing.admin;

export import bioseq.postgres.storage.indexing.handles;
export import :generation_api;
export import :grouped_soa;
export import :inspection_storage_support;
export import :rebuild;

static_assert(__cplusplus >= 202302L, "C++23-or-later");
