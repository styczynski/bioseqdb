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

bioseq_cache_status fake_register_provider(
        const bioseq_cache_provider* provider,
        bioseq_cache_provider_registration* registration) {
    ++g_registration_calls;
    if (provider == nullptr || registration == nullptr) {
        return bioseq_cache_status_error;
    }
    *registration = bioseq_cache_provider_registration{
        .struct_size = static_cast<std::uint32_t>(
            sizeof(bioseq_cache_provider_registration)),
        .provider_id = provider->provider_id,
        .registry_generation = 1,
        .owner_cookie = 1,
    };
    return bioseq_cache_status_ok;
}

bool fake_unregister_provider(
        const bioseq_cache_provider_registration*) {
    return true;
}

void fake_record_relation_write(
        std::uint32_t,
        std::uint32_t,
        const bioseq_cache_physical_identity*) {}

void fake_after_provider_rel_paths(
        PlannerInfo*,
        RelOptInfo*,
        unsigned int,
        RangeTblEntry*,
        const bioseq_cache_provider*) {}

void fake_after_planned_statement(PlannedStmt*) {}

inline constinit bioseq_cache_planner_bridge g_fake_planner_bridge{
    .struct_size = static_cast<std::uint32_t>(
        sizeof(bioseq_cache_planner_bridge)),
    .abi_revision = bioseq::cache::k_provider_abi_revision,
    .postgres_major =
        static_cast<std::uint32_t>(PG_VERSION_NUM / 10000),
    .after_provider_rel_paths = &fake_after_provider_rel_paths,
    .after_planned_statement = &fake_after_planned_statement,
};

/*
 * Publish working callbacks with a deliberately wrong ABI revision.
 * Bioseq must warn and reject this table before calling register_provider.
 */
inline bioseq_cache_api g_incompatible_cache_api{
    .struct_size =
        static_cast<std::uint32_t>(sizeof(bioseq_cache_api)),
    .abi_revision = bioseq::cache::k_provider_abi_revision + 1,
    .postgres_major =
        static_cast<std::uint32_t>(PG_VERSION_NUM / 10000),
    .reserved = 0,
    .instance_cookie = 1,
    .register_provider = &fake_register_provider,
    .unregister_provider = &fake_unregister_provider,
    .record_relation_write = &fake_record_relation_write,
    .planner_bridge = &g_fake_planner_bridge,
};

bioseq_cache_status fake_bind_cache_api(const bioseq_cache_api*) {
    return bioseq_cache_status_ok;
}

inline bioseq_cache_provider_directory g_foreign_provider_directory{
    .struct_size = static_cast<std::uint32_t>(
        sizeof(bioseq_cache_provider_directory)),
    .abi_revision = bioseq::cache::k_provider_abi_revision,
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

PGDLLEXPORT void _PG_init();

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
            bioseq::cache::k_cache_api_rendezvous_name);
        return;
    }
    if (std::strcmp(mode, "provider_directory_collision") == 0) {
        publish(
            static_cast<void*>(&g_foreign_provider_directory),
            bioseq::cache::k_provider_directory_rendezvous_name);
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
