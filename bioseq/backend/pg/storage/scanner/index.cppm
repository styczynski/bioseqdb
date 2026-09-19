/**
 * @file scanner/index.cppm
 * @brief Stable facade for PostgreSQL sequence scanner partitions.
 *
 * Re-exports fragmented scanning, lazy binding, source registry and
 * locator-backed source scanning contracts.
 */
export module bioseq.postgres.storage.scanner;

export import bioseq.postgres.storage.scanner.fragment_scanner;
export import bioseq.postgres.storage.scanner.lazy_binding;
export import :source_registry;
export import :source_scanner;

static_assert(__cplusplus >= 202302L, "bioseq requires C++23-or-later");
