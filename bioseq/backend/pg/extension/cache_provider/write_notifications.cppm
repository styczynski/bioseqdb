/**
 * @file write_notifications.cppm
 * @brief Notify the optional query cache of direct PostgreSQL relation writes.
 *
 * Keep the loader independent of provider registration and planner recipes.
 * The provider owns the callback and its checked cache-API binding.
 */
module;

extern "C" {
#include "postgres.h"
#include "storage/relfilelocator.h"
}

export module bioseq.postgres.extension.cache_provider.write_notifications;

export namespace bioseq::postgres::extension::cache_provider {

/** Receive a locked relation identity before direct writes can change it. */
using relation_write_observer = void (*)(Oid, const RelFileLocator&);

} // namespace bioseq::postgres::extension::cache_provider

namespace bioseq::postgres::extension::cache_provider::detail {

/** Backend-local optional extension callback; never stores per-query state. */
inline relation_write_observer relation_write_notification = nullptr;

} // namespace bioseq::postgres::extension::cache_provider::detail

export namespace bioseq::postgres::extension::cache_provider {

/** Replace the provider-owned callback; null clears a retired API binding. */
void set_relation_write_observer(relation_write_observer observer) noexcept {
    detail::relation_write_notification = observer;
}

/**
 * Report a direct write using the relation's identity under its retained lock.
 * With no cache binding this is a no-op. Callback errors propagate before writes.
 */
void notify_relation_write(Oid relation_oid, const RelFileLocator& locator) {
    if (const auto observer = detail::relation_write_notification)
        observer(relation_oid, locator);
}

} // namespace bioseq::postgres::extension::cache_provider
