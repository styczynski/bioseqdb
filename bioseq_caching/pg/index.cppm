/*-------------------------------------------------------------------------
 *
 * Thin primary interface for the bioseq_cache PostgreSQL adapter.
 *
 * Runtime behavior and its typed SQL backend surfaces are owned by the
 * dedicated runtime module. This primary only composes that module with the
 * PostgreSQL extension entry point.
 *
 *-------------------------------------------------------------------------
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
