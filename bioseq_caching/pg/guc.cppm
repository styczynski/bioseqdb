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
#include "utils/guc.h"
#include "utils/plancache.h"

#include "miscadmin.h"
}

#include <limits>

export module bioseq.cache.pg.guc;

export namespace bioseq::cache::pg::guc {

enum class query_cache_mode : int {
    disabled = 0,
    exact_query = 1,
    reusable_fragments = 2,
};

} // namespace bioseq::cache::pg::guc

namespace bioseq::cache::pg::guc::detail {

using bioseq::cache::pg::guc::query_cache_mode;

bool g_registered = false;
bool g_preload_requested = false;
int g_mode = static_cast<int>(query_cache_mode::disabled);
int g_ttl_ms = 60 * 60 * 1000;

/** Reject non-disabled use when shared memory was not requested at preload. */
[[nodiscard]] bool check_mode(int* new_value, void**, GucSource) noexcept {
    if (*new_value == static_cast<int>(query_cache_mode::disabled)) {
        return true;
    }
    if (process_shared_preload_libraries_in_progress || g_preload_requested) {
        return true;
    }
    GUC_check_errmsg("bioseq_cache shared memory is not registered");
    GUC_check_errhint("Add bioseq_cache to shared_preload_libraries and restart PostgreSQL.");
    return false;
}

/** Invalidate generic prepared plans on every real mode transition. */
void assign_mode(int new_value, void*) noexcept {
    static int previous_value = static_cast<int>(query_cache_mode::disabled);
    const bool changed = previous_value != new_value;
    previous_value = new_value;
    if (changed && IsTransactionState()) {
        ResetPlanCache();
    }
}

} // namespace bioseq::cache::pg::guc::detail

export namespace bioseq::cache::pg::guc {

[[nodiscard]] query_cache_mode mode() noexcept {
    return static_cast<query_cache_mode>(detail::g_mode);
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
        {"disabled", static_cast<int>(query_cache_mode::disabled), false},
        {"exact_query", static_cast<int>(query_cache_mode::exact_query), false},
        {"reusable_fragments", static_cast<int>(query_cache_mode::reusable_fragments), false},
        {nullptr, 0, false},
    };
    DefineCustomEnumVariable("bioseq_cache.query_cache.mode",
                             "Controls exact request locking and reusable query-result fragments.",
                             nullptr,
                             &detail::g_mode,
                             static_cast<int>(query_cache_mode::disabled),
                             mode_options,
                             PGC_USERSET,
                             0,
                             detail::check_mode,
                             detail::assign_mode,
                             nullptr);
    DefineCustomIntVariable("bioseq_cache.query_cache.completed_result_ttl_ms",
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
