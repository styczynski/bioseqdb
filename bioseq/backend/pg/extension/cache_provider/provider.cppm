/**
 * @file provider.cppm
 * @brief Register Bioseq scans and batch joins with the optional query cache.
 *
 * PostgreSQL rendezvous variables let either extension find the other
 * regardless of load order. Compatibility checks protect the shared C API.
 */
module;

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/errcodes.h"
#include "utils/plancache.h"
}

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <array>

export module bioseq.postgres.extension.cache_provider:provider;

import :plan_recipe;
import bioseq.cache.provider_api;
import bioseq.postgres.support;
import bioseq.postgres.table;

namespace bioseq::postgres::extension::cache_provider {
namespace detail {

inline constexpr std::uint32_t k_postgres_major_version =
    static_cast<std::uint32_t>(PG_VERSION_NUM / 10000);

inline void** g_directory_slot = nullptr;
inline void** g_cache_api_slot = nullptr;
inline const bioseq_cache_api* g_preflight_cache_api = nullptr;
inline bool g_preflight_cache_api_compatible = true;
inline const bioseq_cache_api* g_bound_api = nullptr;
inline std::uint64_t g_bound_api_cookie = 0;
inline constexpr std::size_t k_provider_descriptor_count = 2;
inline constexpr std::size_t k_batch_join_provider_index = 1;
inline std::array<bioseq_cache_provider_registration,
                  k_provider_descriptor_count> g_registrations{{
    bioseq_cache_provider_registration{
        .struct_size = static_cast<std::uint32_t>(
            sizeof(bioseq_cache_provider_registration)),
    },
    bioseq_cache_provider_registration{
        .struct_size = static_cast<std::uint32_t>(
            sizeof(bioseq_cache_provider_registration)),
    },
}};
inline std::array<bool, k_provider_descriptor_count> g_registered{};
inline bool g_directory_published = false;

/** @brief Derive a nonzero id for this backend's provider directory. */
[[nodiscard]] std::uint64_t mix_cookie(std::uint64_t value) noexcept {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    return value == 0 ? 1U : value;
}

/** @return unsupported. This provider does not analyze SQL query intent. */
[[nodiscard]] bioseq_cache_status unsupported_analyze_query(
        const Query*,
        bioseq_cache_query_intent*) {
    return bioseq_cache_status_unsupported;
}

/** @return unsupported. This provider does not describe reusable path coverage. */
[[nodiscard]] bioseq_cache_status unsupported_describe_path(
        PlannerInfo*,
        RelOptInfo*,
        Path*,
        const bioseq_cache_query_intent*,
        bioseq_cache_path_domain*) {
    return bioseq_cache_status_unsupported;
}

/** @return unsupported. This provider does not map reusable source rows. */
[[nodiscard]] bioseq_cache_status unsupported_map_source(
        PlannerInfo*,
        const bioseq_cache_path_domain*,
        const bioseq_cache_reusable_source*,
        bioseq_cache_source_mapping*) {
    return bioseq_cache_status_unsupported;
}

/** @return unsupported. This provider does not build paths from reusable coverage. */
[[nodiscard]] bioseq_cache_status unsupported_decorate_path(
        PlannerInfo*,
        RelOptInfo*,
        Path*,
        const bioseq_cache_path_domain*,
        List*,
        bioseq_cache_decorated_path*) {
    return bioseq_cache_status_unsupported;
}

/** @return unsupported. This provider does not extract row occurrences. */
[[nodiscard]] bioseq_cache_status unsupported_extract_occurrence(
        List*,
        TupleTableSlot*,
        bioseq_cache_occurrence*) {
    return bioseq_cache_status_unsupported;
}

/** @return unsupported. This provider does not fetch source columns after cache lookup. */
[[nodiscard]] bioseq_cache_status unsupported_late_fetch(
        List*,
        EState*,
        const bioseq_cache_occurrence*,
        std::size_t,
        const std::int16_t*,
        std::size_t,
        bioseq_cache_late_fetch_sink*) {
    return bioseq_cache_status_unsupported;
}

/** @return Shared callback table for exact-result caching of Bioseq scans. */
[[nodiscard]] const bioseq_cache_provider& provider_descriptor() noexcept {
    static const bioseq_cache_provider provider{
        .struct_size =
            static_cast<std::uint32_t>(sizeof(bioseq_cache_provider)),
        .abi_revision = bioseq::cache::k_provider_abi_revision,
        .postgres_major = k_postgres_major_version,
        .provider_id = k_bioseq_scan_provider_id,
        .provider_flags =
            bioseq_cache_provider_exact_source_elision_safe,
        .reserved = 0,
        .provider_name = k_bioseq_scan_provider_name,
        .provider_scan_methods = bioseq_customscan_methods(),
        .analyze_query = &unsupported_analyze_query,
        .describe_path = &unsupported_describe_path,
        .append_domain_semantic_digest = &bioseq_append_domain_digest,
        .append_generation_semantic_digest =
            &bioseq_append_generation_digest,
        .append_runtime_generation_semantic_digest =
            &bioseq_append_runtime_generation_digest,
        .map_reusable_source = &unsupported_map_source,
        .decorate_path = &unsupported_decorate_path,
        .build_plan_digest_recipe =
            &bioseq_build_plan_digest_recipe,
        .extract_occurrence = &unsupported_extract_occurrence,
        .late_fetch_batch = &unsupported_late_fetch,
        .explain_domain = &bioseq_explain_exact_domain,
    };
    return provider;
}

/** @return Shared callback table for exact-result caching of Bioseq batch joins. */
[[nodiscard]] const bioseq_cache_provider&
batch_join_provider_descriptor() noexcept {
    static const bioseq_cache_provider provider{
        .struct_size =
            static_cast<std::uint32_t>(sizeof(bioseq_cache_provider)),
        .abi_revision = bioseq::cache::k_provider_abi_revision,
        .postgres_major = k_postgres_major_version,
        .provider_id = k_bioseq_batch_join_provider_id,
        .provider_flags =
            bioseq_cache_provider_exact_source_elision_safe,
        .reserved = 0,
        .provider_name = k_bioseq_batch_join_provider_name,
        .provider_scan_methods = bioseq_batch_join_customscan_methods(),
        .analyze_query = &unsupported_analyze_query,
        .describe_path = &unsupported_describe_path,
        .append_domain_semantic_digest =
            &bioseq_batch_join_append_domain_digest,
        .append_generation_semantic_digest =
            &bioseq_batch_join_append_generation_digest,
        .append_runtime_generation_semantic_digest =
            &bioseq_batch_join_append_runtime_generation_digest,
        .map_reusable_source = &unsupported_map_source,
        .decorate_path = &unsupported_decorate_path,
        .build_plan_digest_recipe =
            &bioseq_batch_join_build_plan_digest_recipe,
        .extract_occurrence = &unsupported_extract_occurrence,
        .late_fetch_batch = &unsupported_late_fetch,
        .explain_domain = &bioseq_batch_join_explain_exact_domain,
    };
    return provider;
}

/** @return Batch-join callbacks for its registration index, scan callbacks otherwise. */
[[nodiscard]] const bioseq_cache_provider& provider_descriptor(
        std::size_t index) noexcept {
    return index == k_batch_join_provider_index
        ? batch_join_provider_descriptor()
        : provider_descriptor();
}

/** @return true when both providers have registered successfully. */
[[nodiscard]] bool all_providers_registered() noexcept {
    for (const bool registered : g_registered) {
        if (!registered) {
            return false;
        }
    }
    return true;
}

/** @brief Clear the local registration flag and token for one provider. */
void reset_registration(std::size_t index) noexcept {
    g_registered[index] = false;
    g_registrations[index] = bioseq_cache_provider_registration{
        .struct_size = static_cast<std::uint32_t>(
            sizeof(bioseq_cache_provider_registration)),
    };
}

/** @brief Check the cache API version, PostgreSQL version and required callbacks.
 * @return true if this backend can use the supplied API.
 */
[[nodiscard]] bool cache_api_valid(
        const bioseq_cache_api* api) noexcept {
    return api != nullptr
        && api->struct_size >= sizeof(bioseq_cache_api)
        && api->abi_revision == bioseq::cache::k_provider_abi_revision
        && api->postgres_major == k_postgres_major_version
        && api->reserved == 0
        && api->instance_cookie != 0
        && api->register_provider != nullptr
        && api->unregister_provider != nullptr
        && api->record_relation_write != nullptr
        && api->planner_bridge != nullptr
        && api->planner_bridge->struct_size
            >= sizeof(bioseq_cache_planner_bridge)
        && api->planner_bridge->abi_revision
            == bioseq::cache::k_provider_abi_revision
        && api->planner_bridge->postgres_major
            == k_postgres_major_version
        && api->planner_bridge->after_provider_rel_paths != nullptr
        && api->planner_bridge->after_planned_statement != nullptr;
}

/** @brief Check that a returned token names this provider and has valid ownership ids. */
[[nodiscard]] bool registration_valid(
        const bioseq_cache_provider_registration& registration,
        std::uint32_t provider_id) noexcept {
    return registration.struct_size
            >= sizeof(bioseq_cache_provider_registration)
        && registration.provider_id == provider_id
        && registration.registry_generation != 0
        && registration.owner_cookie != 0;
}

/** @brief Undo successful registrations, starting with the last provider.
 * Keep a local token if the cache refuses to unregister it.
 */
void rollback_registrations(const bioseq_cache_api* api) {
    for (std::size_t offset = 0;
         offset < k_provider_descriptor_count;
         ++offset) {
        const std::size_t index =
            k_provider_descriptor_count - offset - 1U;
        if (!g_registered[index]) {
            continue;
        }
        const bool removed = bioseq::postgres::support::invoke_pg_as_cpp(
            [api, index] {
                return api->unregister_provider(&g_registrations[index]);
            });
        if (removed) {
            reset_registration(index);
        }
    }
}

/**
 * @brief Register scan and batch-join callbacks with a compatible cache instance.
 * Repeated binding to the same fully registered instance succeeds immediately.
 * A rejected registration or invalid receipt triggers an attempt to undo
 * registrations already accepted. C++ exceptions return error status.
 */
extern "C" bioseq_cache_status bind_cache_api(
        const bioseq_cache_api* api) noexcept {
    if (!cache_api_valid(api)) {
        return bioseq_cache_status_unsupported;
    }
    if (all_providers_registered() && g_bound_api == api
        && g_bound_api_cookie == api->instance_cookie) {
        return bioseq_cache_status_ok;
    }
    try {
        if (g_bound_api_cookie != 0
            && (g_bound_api != api
                || g_bound_api_cookie != api->instance_cookie)) {
            /* A replacement cache library has a new instance id. Discard plans made
             * with the old binding and forget its tokens. Scan hooks stay installed
             * for the backend lifetime, so this does not call the old library's
             * unregister callback. */
            bioseq::postgres::support::invoke_pg_as_cpp(
                [] { ResetPlanCache(); });
            g_bound_api = nullptr;
            g_bound_api_cookie = 0;
            for (std::size_t index = 0;
                 index < k_provider_descriptor_count;
                 ++index) {
                reset_registration(index);
            }
        }

        g_bound_api = api;
        g_bound_api_cookie = api->instance_cookie;
        for (std::size_t index = 0;
             index < k_provider_descriptor_count;
             ++index) {
            bioseq_cache_provider_registration candidate{
                .struct_size = static_cast<std::uint32_t>(
                    sizeof(bioseq_cache_provider_registration)),
            };
            const bioseq_cache_provider& descriptor =
                provider_descriptor(index);
            const bioseq_cache_status status =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [api, &candidate, &descriptor] {
                        return api->register_provider(
                            &descriptor, &candidate);
                    });
            if (status != bioseq_cache_status_ok
                || !registration_valid(
                    candidate, descriptor.provider_id)) {
                rollback_registrations(api);
                if (!all_providers_registered()) {
                    bool any_registered = false;
                    for (const bool registered : g_registered) {
                        any_registered = any_registered || registered;
                    }
                    if (!any_registered) {
                        g_bound_api = nullptr;
                        g_bound_api_cookie = 0;
                    }
                }
                return status == bioseq_cache_status_unsupported
                    ? bioseq_cache_status_unsupported
                    : bioseq_cache_status_error;
            }
            g_registrations[index] = candidate;
            g_registered[index] = true;
        }
        return bioseq_cache_status_ok;
    } catch (...) {
        return bioseq_cache_status_error;
    }
}

inline bioseq_cache_provider_directory g_directory{
    .struct_size =
        static_cast<std::uint32_t>(
            sizeof(bioseq_cache_provider_directory)),
    .abi_revision = bioseq::cache::k_provider_abi_revision,
    .postgres_major = k_postgres_major_version,
    .reserved = 0,
    .instance_cookie = 0,
    .bind_cache_api = &bind_cache_api,
};

} // namespace detail

/**
 * @brief Check the shared lookup slots before installing extension hooks.
 * Find Bioseq's provider slot, inspect any existing cache API and save
 * the results for publication. Slot conflicts raise PostgreSQL errors here
 * while extension initialization can still handle them.
 */
export void bioseq_cache_provider_preflight() {
    void** const directory_slot = find_rendezvous_variable(
        bioseq::cache::k_provider_directory_rendezvous_name);
    if (directory_slot == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("Bioseq cache-provider rendezvous allocation failed")));
    }
    if (*directory_slot != nullptr
        && *directory_slot != static_cast<void*>(&detail::g_directory)) {
        ereport(ERROR,
            (errcode(ERRCODE_DUPLICATE_OBJECT),
             errmsg("Bioseq cache-provider rendezvous is already owned"),
             errdetail("Rendezvous name: %s",
                bioseq::cache::k_provider_directory_rendezvous_name)));
    }
    void** const api_slot = find_rendezvous_variable(
        bioseq::cache::k_cache_api_rendezvous_name);
    if (api_slot == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("Bioseq cache API rendezvous allocation failed")));
    }
    const auto* const cache_api =
        static_cast<const bioseq_cache_api*>(*api_slot);
    const bool cache_api_compatible =
        cache_api == nullptr || detail::cache_api_valid(cache_api);
    if (!cache_api_compatible) {
        ereport(WARNING,
            (errmsg("Bioseq ignored an incompatible cache API rendezvous"),
             errdetail("Rendezvous name: %s",
                bioseq::cache::k_cache_api_rendezvous_name)));
    }
    detail::g_directory_slot = directory_slot;
    detail::g_cache_api_slot = api_slot;
    detail::g_preflight_cache_api = cache_api;
    detail::g_preflight_cache_api_compatible =
        cache_api_compatible;
    if (detail::g_directory.instance_cookie == 0) {
        const auto address = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(&detail::g_directory));
        detail::g_directory.instance_cookie =
            detail::mix_cookie(
                address ^ 0x76325f62696f7365ULL);
    }
}

/** @brief Publish the provider directory and register with a compatible cache.
 * Report setup and registration failures as PostgreSQL warnings.
 */
export void bioseq_cache_provider_publish_directory() {
    if (detail::g_directory_slot == nullptr
        || detail::g_cache_api_slot == nullptr) {
        ereport(WARNING,
            (errmsg("Bioseq cache provider directory was not preflighted")));
        return;
    }
    if (*detail::g_directory_slot != nullptr
        && *detail::g_directory_slot
            != static_cast<void*>(&detail::g_directory)) {
        ereport(WARNING,
            (errmsg("Bioseq cache-provider rendezvous ownership changed during initialization")));
        return;
    }
    *detail::g_directory_slot =
        static_cast<void*>(&detail::g_directory);
    detail::g_directory_published = true;

    if (*detail::g_cache_api_slot == nullptr) {
        return;
    }
    const auto* api =
        static_cast<const bioseq_cache_api*>(
            *detail::g_cache_api_slot);
    if (api == detail::g_preflight_cache_api
        && !detail::g_preflight_cache_api_compatible) {
        /* Preflight already rejected this cache API and reported a warning.
         * Skip it without reading its fields or requesting a registration token. */
        return;
    }
    if (!detail::cache_api_valid(api)) {
        ereport(WARNING,
            (errmsg("Bioseq cache provider ignored an incompatible cache API"),
             errdetail("Rendezvous name: %s",
                bioseq::cache::k_cache_api_rendezvous_name)));
        return;
    }
    if (detail::g_preflight_cache_api != nullptr
        && api != detail::g_preflight_cache_api) {
        ereport(WARNING,
            (errmsg("Bioseq cache API rendezvous changed during initialization")));
    }
    const bioseq_cache_status status =
        detail::bind_cache_api(api);
    if (status != bioseq_cache_status_ok) {
        ereport(WARNING,
            (errmsg("Bioseq exact cache provider registration failed"),
             errdetail("Provider status: %u",
                static_cast<unsigned>(status))));
    }
}

/** @return True when the directory and both providers are bound to one peer. */
export bool bioseq_cache_provider_is_bound() noexcept {
    return detail::g_directory_published
        && detail::all_providers_registered()
        && detail::g_bound_api != nullptr
        && detail::g_bound_api_cookie != 0;
}

/**
 * @brief Pass the finished Bioseq plan to the cache's planner callback.
 * Call after all Bioseq rewrites, including subplans. Skip notification if
 * either provider is unregistered or the cache instance has changed.
 */
export void bioseq_cache_provider_after_planned_statement(
        PlannedStmt* statement) noexcept {
    const bioseq_cache_api* const api = detail::g_bound_api;
    if (statement == nullptr || !detail::all_providers_registered()
        || !detail::cache_api_valid(api)
        || api->instance_cookie != detail::g_bound_api_cookie) {
        return;
    }
    api->planner_bridge->after_planned_statement(statement);
}

} // namespace bioseq::postgres::extension::cache_provider
