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

export module bioseq.pg.cache_provider:provider;

import :plan_recipe;
import bioseq.cache.provider_api;
import bioseq.pg_utils;
import bioseq.table;

namespace bioseq::pg::cache_provider {
namespace detail {

inline constexpr std::uint32_t kPostgresMajorVersion =
    static_cast<std::uint32_t>(PG_VERSION_NUM / 10000);

inline void** g_directory_slot = nullptr;
inline void** g_cache_api_slot = nullptr;
inline const BioseqCacheApiV2* g_preflight_cache_api = nullptr;
inline const BioseqCacheApiV2* g_bound_api = nullptr;
inline std::uint64_t g_bound_api_cookie = 0;
inline BioseqCacheProviderRegistrationV2 g_registration{
    .struct_size =
        static_cast<std::uint32_t>(
            sizeof(BioseqCacheProviderRegistrationV2)),
};
inline bool g_registered = false;
inline bool g_directory_published = false;

[[nodiscard]] std::uint64_t mix_cookie(std::uint64_t value) noexcept {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    return value == 0 ? 1U : value;
}

[[nodiscard]] BioseqCacheStatusV2 unsupported_analyze_query(
        const Query*,
        BioseqCacheQueryIntentV2*) {
    return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
}

[[nodiscard]] BioseqCacheStatusV2 unsupported_describe_path(
        PlannerInfo*,
        RelOptInfo*,
        Path*,
        const BioseqCacheQueryIntentV2*,
        BioseqCachePathDomainV2*) {
    return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
}

[[nodiscard]] BioseqCacheStatusV2 unsupported_map_source(
        PlannerInfo*,
        const BioseqCachePathDomainV2*,
        const BioseqCacheReusableSourceV2*,
        BioseqCacheSourceMappingV2*) {
    return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
}

[[nodiscard]] BioseqCacheStatusV2 unsupported_decorate_path(
        PlannerInfo*,
        RelOptInfo*,
        Path*,
        const BioseqCachePathDomainV2*,
        List*,
        BioseqCacheDecoratedPathV2*) {
    return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
}

[[nodiscard]] BioseqCacheStatusV2 unsupported_extract_occurrence(
        List*,
        TupleTableSlot*,
        BioseqCacheOccurrenceV2*) {
    return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
}

[[nodiscard]] BioseqCacheStatusV2 unsupported_late_fetch(
        List*,
        EState*,
        const BioseqCacheOccurrenceV2*,
        std::size_t,
        const std::int16_t*,
        std::size_t,
        BioseqCacheLateFetchSinkV2*) {
    return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
}

[[nodiscard]] const BioseqCacheProviderV2& provider_descriptor() noexcept {
    static const BioseqCacheProviderV2 provider{
        .struct_size =
            static_cast<std::uint32_t>(sizeof(BioseqCacheProviderV2)),
        .abi_version = bioseq::cache::kProviderAbiVersion,
        .postgres_major = kPostgresMajorVersion,
        .provider_id = kBioseqScanProviderId,
        .provider_flags =
            BIOSEQ_CACHE_PROVIDER_EXACT_SOURCE_ELISION_SAFE,
        .reserved = 0,
        .provider_name = kBioseqScanProviderName,
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

[[nodiscard]] bool cache_api_valid(
        const BioseqCacheApiV2* api) noexcept {
    return api != nullptr
        && api->struct_size >= sizeof(BioseqCacheApiV2)
        && api->abi_version == bioseq::cache::kProviderAbiVersion
        && api->postgres_major == kPostgresMajorVersion
        && api->reserved == 0
        && api->instance_cookie != 0
        && api->register_provider != nullptr
        && api->unregister_provider != nullptr
        && api->record_relation_write != nullptr
        && api->planner_bridge != nullptr
        && api->planner_bridge->struct_size
            >= sizeof(BioseqCachePlannerBridgeV2)
        && api->planner_bridge->abi_version
            == bioseq::cache::kProviderAbiVersion
        && api->planner_bridge->postgres_major
            == kPostgresMajorVersion
        && api->planner_bridge->after_provider_rel_paths != nullptr;
}

[[nodiscard]] bool registration_valid(
        const BioseqCacheProviderRegistrationV2& registration) noexcept {
    return registration.struct_size
            >= sizeof(BioseqCacheProviderRegistrationV2)
        && registration.provider_id == kBioseqScanProviderId
        && registration.registry_generation != 0
        && registration.owner_cookie != 0;
}

extern "C" BioseqCacheStatusV2 bind_cache_api(
        const BioseqCacheApiV2* api) noexcept {
    if (!cache_api_valid(api)) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    if (g_registered && g_bound_api == api
        && g_bound_api_cookie == api->instance_cookie) {
        return BIOSEQ_CACHE_STATUS_OK;
    }
    try {
        if (g_bound_api_cookie != 0
            && (g_bound_api != api
                || g_bound_api_cookie != api->instance_cookie)) {
            /*
             * A cache DSO replacement retires this process-local logical
             * binding.  Production scan hooks remain installed for backend
             * lifetime, so deliberately do not call the old unregister slot.
             */
            bioseq::pg_utils::invoke_pg_as_cpp(
                [] { ResetPlanCache(); });
            g_bound_api = nullptr;
            g_bound_api_cookie = 0;
            g_registered = false;
            g_registration = BioseqCacheProviderRegistrationV2{
                .struct_size =
                    static_cast<std::uint32_t>(
                        sizeof(BioseqCacheProviderRegistrationV2)),
            };
        }

        BioseqCacheProviderRegistrationV2 candidate{
            .struct_size =
                static_cast<std::uint32_t>(
                    sizeof(BioseqCacheProviderRegistrationV2)),
        };
        const BioseqCacheStatusV2 status =
            bioseq::pg_utils::invoke_pg_as_cpp(
                [api, &candidate] {
                    return api->register_provider(
                        &provider_descriptor(), &candidate);
                });
        if (status != BIOSEQ_CACHE_STATUS_OK
            || !registration_valid(candidate)) {
            return status == BIOSEQ_CACHE_STATUS_UNSUPPORTED
                ? BIOSEQ_CACHE_STATUS_UNSUPPORTED
                : BIOSEQ_CACHE_STATUS_ERROR;
        }
        g_registration = candidate;
        g_bound_api = api;
        g_bound_api_cookie = api->instance_cookie;
        g_registered = true;
        return BIOSEQ_CACHE_STATUS_OK;
    } catch (...) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
}

inline BioseqCacheProviderDirectoryV2 g_directory{
    .struct_size =
        static_cast<std::uint32_t>(
            sizeof(BioseqCacheProviderDirectoryV2)),
    .abi_version = bioseq::cache::kProviderAbiVersion,
    .postgres_major = kPostgresMajorVersion,
    .reserved = 0,
    .instance_cookie = 0,
    .bind_cache_api = &bind_cache_api,
};

} // namespace detail

export void bioseq_cache_provider_preflight() {
    void** const directory_slot = find_rendezvous_variable(
        bioseq::cache::kProviderDirectoryRendezvousName);
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
                bioseq::cache::kProviderDirectoryRendezvousName)));
    }
    void** const api_slot = find_rendezvous_variable(
        bioseq::cache::kCacheApiRendezvousName);
    if (api_slot == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("Bioseq cache API rendezvous allocation failed")));
    }
    const auto* const cache_api =
        static_cast<const BioseqCacheApiV2*>(*api_slot);
    if (cache_api != nullptr && !detail::cache_api_valid(cache_api)) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("Bioseq cache API rendezvous is incompatible"),
             errdetail("Rendezvous name: %s",
                bioseq::cache::kCacheApiRendezvousName)));
    }
    detail::g_directory_slot = directory_slot;
    detail::g_cache_api_slot = api_slot;
    detail::g_preflight_cache_api = cache_api;
    if (detail::g_directory.instance_cookie == 0) {
        const auto address = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(&detail::g_directory));
        detail::g_directory.instance_cookie =
            detail::mix_cookie(
                address ^ 0x76325f62696f7365ULL);
    }
}

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

    if (*detail::g_cache_api_slot == nullptr) return;
    const auto* api =
        static_cast<const BioseqCacheApiV2*>(
            *detail::g_cache_api_slot);
    if (!detail::cache_api_valid(api)) {
        ereport(WARNING,
            (errmsg("Bioseq cache provider ignored an incompatible cache API"),
             errdetail("Rendezvous name: %s",
                bioseq::cache::kCacheApiRendezvousName)));
        return;
    }
    if (detail::g_preflight_cache_api != nullptr
        && api != detail::g_preflight_cache_api) {
        ereport(WARNING,
            (errmsg("Bioseq cache API rendezvous changed during initialization")));
    }
    const BioseqCacheStatusV2 status =
        detail::bind_cache_api(api);
    if (status != BIOSEQ_CACHE_STATUS_OK) {
        ereport(WARNING,
            (errmsg("Bioseq exact cache provider registration failed"),
             errdetail("Provider status: %u",
                static_cast<unsigned>(status))));
    }
}

export bool bioseq_cache_provider_is_bound() noexcept {
    return detail::g_directory_published
        && detail::g_registered
        && detail::g_bound_api != nullptr
        && detail::g_bound_api_cookie != 0;
}

} // namespace bioseq::pg::cache_provider
