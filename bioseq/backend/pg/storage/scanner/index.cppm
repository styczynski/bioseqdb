/**
 * @file scanner/index.cppm
 * @brief PostgreSQL sequence readers and the resources they need.
 *
 * Exports fragment readers, lazy sequence bindings and source registration.
 */
export module bioseq.postgres.storage.scanner;

export import bioseq.postgres.storage.scanner.fragment_scanner;
export import bioseq.postgres.storage.scanner.lazy_binding;
export import :source_registry;
export import :source_scanner;

static_assert(__cplusplus >= 202302L, "bioseq requires C++23-or-later");
