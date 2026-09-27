/**
 * @file
 * @brief Exports the cache runtime and calls its initializer when PostgreSQL loads the extension.
 */
module;

extern "C" {
#include "postgres.h"
#include "fmgr.h"

PG_MODULE_MAGIC;
}

export module bioseq.cache.pg;

export import bioseq.cache.pg.runtime;

extern "C" PGDLLEXPORT void _PG_init() {
    bioseq::cache::pg::detail::initialize_extension();
}
