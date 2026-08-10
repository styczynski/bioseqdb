/*-------------------------------------------------------------------------
 *
 * PostgreSQL GUC ownership for bioseq_cache.
 *
 * This module is the single owner of the user-visible cache mode and TTL
 * variables. Callers consume typed accessors rather than retaining pointers
 * to PostgreSQL's mutable GUC storage.
 *
 *-------------------------------------------------------------------------
 */
module;

extern "C" {
#include "postgres.h"

#include "access/xact.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/plancache.h"
}

#include <limits>

export module bioseq.cache.pg.guc;

export namespace bioseq::cache::pg::guc {

enum class QueryCacheMode : int {
    Disabled = 0,
    Same = 1,
    Reuse = 2,
};

} // namespace bioseq::cache::pg::guc

namespace bioseq::cache::pg::guc::detail {

using bioseq::cache::pg::guc::QueryCacheMode;

bool g_registered = false;
bool g_preload_requested = false;
int g_mode = static_cast<int>(QueryCacheMode::Disabled);
int g_ttl_ms = 60 * 60 * 1000;

/** Reject non-disabled use when shared memory was not requested at preload. */
[[nodiscard]] bool check_mode(
        int* new_value, void**, GucSource) noexcept {
    if (*new_value == static_cast<int>(QueryCacheMode::Disabled)) {
        return true;
    }
    if (process_shared_preload_libraries_in_progress
        || g_preload_requested) {
        return true;
    }
    GUC_check_errmsg("bioseq_cache shared memory is not registered");
    GUC_check_errhint(
        "Add bioseq_cache to shared_preload_libraries and restart PostgreSQL.");
    return false;
}

/** Invalidate generic prepared plans on every real mode transition. */
void assign_mode(int new_value, void*) noexcept {
    static int previous_value = static_cast<int>(QueryCacheMode::Disabled);
    const bool changed = previous_value != new_value;
    previous_value = new_value;
    if (changed && IsTransactionState()) {
        ResetPlanCache();
    }
}

} // namespace detail

export namespace bioseq::cache::pg::guc {

[[nodiscard]] QueryCacheMode mode() noexcept {
    return static_cast<QueryCacheMode>(detail::g_mode);
}

[[nodiscard]] int ttl_ms() noexcept {
    return detail::g_ttl_ms;
}

[[nodiscard]] bool preload_requested() noexcept {
    return detail::g_preload_requested;
}

void mark_preload_requested() noexcept {
    detail::g_preload_requested = true;
}

/** Register the two production GUCs exactly once per loaded DSO. */
void register_variables() {
    if (detail::g_registered) {
        return;
    }
    static const config_enum_entry mode_options[] = {
        {"disabled", static_cast<int>(QueryCacheMode::Disabled), false},
        {"same", static_cast<int>(QueryCacheMode::Same), false},
        {"reuse", static_cast<int>(QueryCacheMode::Reuse), false},
        {nullptr, 0, false},
    };
    DefineCustomEnumVariable(
        "bioseq.query_cache_optimisation_mode",
        "Controls exact request locking and reusable query-result fragments.",
        nullptr,
        &detail::g_mode,
        static_cast<int>(QueryCacheMode::Disabled),
        mode_options,
        PGC_USERSET,
        0,
        detail::check_mode,
        detail::assign_mode,
        nullptr);
    DefineCustomIntVariable(
        "bioseq.query_cache_ttl",
        "Maximum age of a completed bioseq_cache result.",
        nullptr,
        &detail::g_ttl_ms,
        60 * 60 * 1000,
        0,
        (std::numeric_limits<int>::max)(),
        PGC_USERSET,
        GUC_UNIT_MS,
        nullptr,
        nullptr,
        nullptr);
    /*
     * The bioseq prefix is shared with the main extension DSO, which can be
     * loaded after this preloaded cache DSO. Reserving it here would make
     * PostgreSQL reject the main extension's GUC registrations.
     */
    detail::g_registered = true;
}

} // namespace bioseq::cache::pg::guc
