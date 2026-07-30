extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "utils/guc.h"
}

#include <cstdint>
#include <cstring>

import bioseq.cache.provider_api;

namespace {

inline std::uint64_t g_registration_calls = 0;

BioseqCacheStatusV2 fake_register_provider(
        const BioseqCacheProviderV2* provider,
        BioseqCacheProviderRegistrationV2* registration) {
    ++g_registration_calls;
    if (provider == nullptr || registration == nullptr) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    *registration = BioseqCacheProviderRegistrationV2{
        .struct_size = static_cast<std::uint32_t>(
            sizeof(BioseqCacheProviderRegistrationV2)),
        .provider_id = provider->provider_id,
        .registry_generation = 1,
        .owner_cookie = 1,
    };
    return BIOSEQ_CACHE_STATUS_OK;
}

bool fake_unregister_provider(
        const BioseqCacheProviderRegistrationV2*) {
    return true;
}

void fake_record_relation_write(
        std::uint32_t,
        std::uint32_t,
        const BioseqCachePhysicalIdentityV2*) {}

void fake_after_provider_rel_paths(
        PlannerInfo*,
        RelOptInfo*,
        unsigned int,
        RangeTblEntry*,
        const BioseqCacheProviderV2*) {}

void fake_after_planned_statement(PlannedStmt*) {}

inline constinit BioseqCachePlannerBridgeV2 g_fake_planner_bridge{
    .struct_size = static_cast<std::uint32_t>(
        sizeof(BioseqCachePlannerBridgeV2)),
    .abi_version = bioseq::cache::kProviderAbiVersion,
    .postgres_major =
        static_cast<std::uint32_t>(PG_VERSION_NUM / 10000),
    .after_provider_rel_paths = &fake_after_provider_rel_paths,
    .after_planned_statement = &fake_after_planned_statement,
};

/*
 * This table is deliberately complete and callable but carries the wrong ABI
 * version.  A consumer that validates the V2 header must warn and decline it
 * before requesting a registration token.
 */
inline BioseqCacheApiV2 g_incompatible_cache_api{
    .struct_size =
        static_cast<std::uint32_t>(sizeof(BioseqCacheApiV2)),
    .abi_version = bioseq::cache::kProviderAbiVersion + 1,
    .postgres_major =
        static_cast<std::uint32_t>(PG_VERSION_NUM / 10000),
    .reserved = 0,
    .instance_cookie = 1,
    .register_provider = &fake_register_provider,
    .unregister_provider = &fake_unregister_provider,
    .record_relation_write = &fake_record_relation_write,
    .planner_bridge = &g_fake_planner_bridge,
};

BioseqCacheStatusV2 fake_bind_cache_api(const BioseqCacheApiV2*) {
    return BIOSEQ_CACHE_STATUS_OK;
}

inline BioseqCacheProviderDirectoryV2 g_foreign_provider_directory{
    .struct_size = static_cast<std::uint32_t>(
        sizeof(BioseqCacheProviderDirectoryV2)),
    .abi_version = bioseq::cache::kProviderAbiVersion,
    .postgres_major =
        static_cast<std::uint32_t>(PG_VERSION_NUM / 10000),
    .reserved = 0,
    .instance_cookie = 1,
    .bind_cache_api = &fake_bind_cache_api,
};

[[nodiscard]] const char* startup_mode() {
    return GetConfigOption(
        "bioseq_cache_peer_test.mode",
        true,
        false);
}

void publish(void* value, const char* rendezvous_name) {
    void** const slot = find_rendezvous_variable(rendezvous_name);
    if (slot == nullptr) {
        ereport(ERROR,
            (errmsg("provider-peer test rendezvous allocation failed")));
        return;
    }
    if (*slot != nullptr && *slot != value) {
        ereport(ERROR,
            (errmsg("provider-peer test rendezvous is already owned")));
    }
    *slot = value;
}

}  // namespace

extern "C" {

PG_MODULE_MAGIC;

void _PG_init() {
    const char* const mode = startup_mode();
    if (mode == nullptr) {
        ereport(ERROR,
            (errmsg("bioseq_cache_peer_test.mode is required")));
        return;
    }
    if (std::strcmp(mode, "incompatible_cache_api") == 0) {
        publish(
            static_cast<void*>(&g_incompatible_cache_api),
            bioseq::cache::kCacheApiRendezvousName);
        return;
    }
    if (std::strcmp(mode, "provider_directory_collision") == 0) {
        publish(
            static_cast<void*>(&g_foreign_provider_directory),
            bioseq::cache::kProviderDirectoryRendezvousName);
        return;
    }
    ereport(ERROR,
        (errmsg(
            "unknown bioseq_cache_peer_test.mode: \"%s\"",
            mode)));
}

PGDLLEXPORT Datum
bioseq_cache_peer_test_registration_calls(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(
    bioseq_cache_peer_test_registration_calls);

Datum bioseq_cache_peer_test_registration_calls(PG_FUNCTION_ARGS) {
    PG_RETURN_INT64(
        static_cast<std::int64_t>(g_registration_calls));
}

}  // extern "C"
