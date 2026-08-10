/*-------------------------------------------------------------------------
 *
 * Primary interface for bioseq_cache provider discovery and callback
 * adaptation. PostgreSQL-independent provider contracts remain in
 * bioseq.cache.core; this module owns only backend-local registry and bridge
 * behavior.
 *
 *-------------------------------------------------------------------------
 */
export module bioseq.cache.pg.provider;

export import :registry;
export import :reusable_adapter;
