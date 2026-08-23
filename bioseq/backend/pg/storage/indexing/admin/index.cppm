/**
 * @file admin/index.cppm
 * @brief Stable facade for PostgreSQL indexing administration partitions.
 *
 * Re-exports policy, publisher, rebuild and integration support contracts.
 */
export module bioseq.postgres.storage.indexing.admin;

export import bioseq.postgres.storage.indexing.handles;
export import :generation_api;
export import :grouped_soa;
export import :inspection_storage_support;
export import :rebuild;

static_assert(__cplusplus >= 202302L, "C++23-or-later");
