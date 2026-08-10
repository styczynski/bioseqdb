#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

import bioseq.cache.provider_api;

using namespace bioseq::cache;

namespace {

inline constexpr std::uint32_t kPostgresMajor = 18;
inline constexpr std::uint32_t kProviderId = 0x13572468U;
inline constexpr std::uint32_t kProviderFlags =
    static_cast<std::uint32_t>(
        BIOSEQ_CACHE_PROVIDER_EXACT_SOURCE_ELISION_SAFE)
    | static_cast<std::uint32_t>(
        BIOSEQ_CACHE_PROVIDER_REUSABLE_COVERAGE)
    | static_cast<std::uint32_t>(
        BIOSEQ_CACHE_PROVIDER_COMPLETE_WRITE_OBSERVATION);
inline constexpr std::uint64_t kDirectoryCookie =
    0x1020304050607080ULL;
inline constexpr std::uint64_t kCacheCookie =
    0x8877665544332211ULL;
inline constexpr std::uint64_t kRegistryGeneration =
    0x1122334455667788ULL;
inline constexpr std::uint64_t kOwnerCookie =
    0x99aabbccddeeff01ULL;
inline constexpr std::uint32_t kDatabaseOid = 0x01020304U;
inline constexpr std::uint32_t kRelationOid = 0x05060708U;

inline std::uint8_t g_scan_methods_cookie = 0;
inline std::uint32_t g_provider_callback_mask = 0;
inline bool g_cache_api_observed = false;

inline constexpr BioseqCacheColumnSchemaV2 kBoundaryColumn{
    .struct_size =
        static_cast<std::uint32_t>(
            sizeof(BioseqCacheColumnSchemaV2)),
    .attno = 3,
    .semantic_id_high = 0x0102030405060708ULL,
    .semantic_id_low = 0x1112131415161718ULL,
    .type_oid = 23,
    .typmod = -1,
    .collation_oid = 100,
    .flags = 0x21U,
};

inline constexpr BioseqCacheColumnSchemaV2 kLineageColumn{
    .struct_size =
        static_cast<std::uint32_t>(
            sizeof(BioseqCacheColumnSchemaV2)),
    .attno = 5,
    .semantic_id_high = 0x2122232425262728ULL,
    .semantic_id_low = 0x3132333435363738ULL,
    .type_oid = 25,
    .typmod = 64,
    .collation_oid = 101,
    .flags = 0x42U,
};

inline constexpr std::array<std::uint32_t, 1>
    kRequiredBoundaryAttnos{3U};
inline constexpr std::array<std::uint32_t, 1>
    kRequiredLineageAttnos{5U};

inline constexpr BioseqCacheColumnMapEntryV2 kBoundaryMap{
    .struct_size =
        static_cast<std::uint32_t>(
            sizeof(BioseqCacheColumnMapEntryV2)),
    .consumer_attno = 3,
    .source_attno = 7,
    .flags = 0x51U,
    .semantic_id_high = 0x0102030405060708ULL,
    .semantic_id_low = 0x1112131415161718ULL,
    .type_oid = 23,
    .typmod = -1,
    .collation_oid = 100,
    .reserved = 0,
};

inline constexpr BioseqCacheColumnMapEntryV2 kLineageMap{
    .struct_size =
        static_cast<std::uint32_t>(
            sizeof(BioseqCacheColumnMapEntryV2)),
    .consumer_attno = 5,
    .source_attno = 9,
    .flags = 0x61U,
    .semantic_id_high = 0x2122232425262728ULL,
    .semantic_id_low = 0x3132333435363738ULL,
    .type_oid = 25,
    .typmod = 64,
    .collation_oid = 101,
    .reserved = 0,
};

template <class T>
[[nodiscard]] T* fake_pointer(std::uintptr_t value) noexcept {
    return reinterpret_cast<T*>(value);
}

BioseqCacheStatusV2 fake_analyze_query(
        const ::Query*,
        BioseqCacheQueryIntentV2* out) {
    g_provider_callback_mask |= 1U << 0U;
    if (out == nullptr) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    *out = BioseqCacheQueryIntentV2{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(BioseqCacheQueryIntentV2)),
        .provider_id = kProviderId,
        .flags = 0x71U,
        .reserved = 0,
        .provider_private_nodes =
            fake_pointer<::List>(0x1010U),
    };
    return BIOSEQ_CACHE_STATUS_OK;
}

BioseqCacheStatusV2 fake_describe_path(
        ::PlannerInfo*,
        ::RelOptInfo*,
        ::Path*,
        const BioseqCacheQueryIntentV2*,
        BioseqCachePathDomainV2* out) {
    g_provider_callback_mask |= 1U << 1U;
    if (out == nullptr) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    *out = BioseqCachePathDomainV2{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(BioseqCachePathDomainV2)),
        .provider_id = kProviderId,
        .compatibility_family_id = 0x4142434445464748ULL,
        .capability_mask = 0x5152535455565758ULL,
        .coverage_predicate =
            fake_pointer<::Expr>(0x2020U),
        .boundary_columns = &kBoundaryColumn,
        .boundary_column_count = 1,
        .lineage_columns = &kLineageColumn,
        .lineage_column_count = 1,
        .required_boundary_attnos =
            kRequiredBoundaryAttnos.data(),
        .required_boundary_attno_count =
            kRequiredBoundaryAttnos.size(),
        .required_lineage_attnos =
            kRequiredLineageAttnos.data(),
        .required_lineage_attno_count =
            kRequiredLineageAttnos.size(),
        .dependency_nodes =
            fake_pointer<::List>(0x3030U),
        .provider_private_nodes =
            fake_pointer<::List>(0x4040U),
        .provider_security_digest_format_version =
            BIOSEQ_CACHE_PROVIDER_SECURITY_DIGEST_FORMAT_V1,
        .provider_security_flags = 0x81U,
        .provider_security_digest = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
            0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
            0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
        },
    };
    return BIOSEQ_CACHE_STATUS_OK;
}

[[nodiscard]] BioseqCacheStatusV2 append_digest(
        BioseqCacheDigestSinkV2* sink,
        std::uint8_t marker) {
    if (sink == nullptr
        || sink->struct_size != sizeof(BioseqCacheDigestSinkV2)
        || sink->append == nullptr) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    const std::array<std::uint8_t, 3> bytes{
        marker,
        static_cast<std::uint8_t>(marker + 1U),
        static_cast<std::uint8_t>(marker + 2U),
    };
    return sink->append(
        sink->opaque, bytes.data(), bytes.size())
        ? BIOSEQ_CACHE_STATUS_OK
        : BIOSEQ_CACHE_STATUS_ERROR;
}

BioseqCacheStatusV2 fake_append_domain_semantic_digest(
        const BioseqCachePathDomainV2*,
        BioseqCacheDigestSinkV2* sink) {
    g_provider_callback_mask |= 1U << 2U;
    return append_digest(sink, 0x10U);
}

BioseqCacheStatusV2 fake_append_generation_semantic_digest(
        const BioseqCachePathDomainV2*,
        BioseqCacheDigestSinkV2* sink) {
    g_provider_callback_mask |= 1U << 3U;
    return append_digest(sink, 0x20U);
}

BioseqCacheStatusV2 fake_append_runtime_generation_semantic_digest(
        const ::List*,
        ::EState*,
        BioseqCacheDigestSinkV2* sink) {
    g_provider_callback_mask |= 1U << 4U;
    return append_digest(sink, 0x30U);
}

BioseqCacheStatusV2 fake_map_reusable_source(
        ::PlannerInfo*,
        const BioseqCachePathDomainV2*,
        const BioseqCacheReusableSourceV2*,
        BioseqCacheSourceMappingV2* out) {
    g_provider_callback_mask |= 1U << 5U;
    if (out == nullptr) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    *out = BioseqCacheSourceMappingV2{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(BioseqCacheSourceMappingV2)),
        .flags = 0x91U,
        .boundary_map = &kBoundaryMap,
        .boundary_map_count = 1,
        .lineage_map = &kLineageMap,
        .lineage_map_count = 1,
        .provider_private_nodes =
            fake_pointer<::List>(0x5050U),
    };
    return BIOSEQ_CACHE_STATUS_OK;
}

BioseqCacheStatusV2 fake_decorate_path(
        ::PlannerInfo*,
        ::RelOptInfo*,
        ::Path*,
        const BioseqCachePathDomainV2*,
        ::List*,
        BioseqCacheDecoratedPathV2* out) {
    g_provider_callback_mask |= 1U << 6U;
    if (out == nullptr) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    *out = BioseqCacheDecoratedPathV2{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(BioseqCacheDecoratedPathV2)),
        .flags = 0xa1U,
        .path = fake_pointer<::Path>(0x6060U),
        .actual_boundary_columns = &kBoundaryColumn,
        .actual_boundary_column_count = 1,
        .actual_lineage_columns = &kLineageColumn,
        .actual_lineage_column_count = 1,
        .provider_private_nodes =
            fake_pointer<::List>(0x7070U),
    };
    return BIOSEQ_CACHE_STATUS_OK;
}

BioseqCacheStatusV2 fake_build_plan_digest_recipe(
        ::PlannerInfo*,
        const ::Plan*,
        BioseqCachePlanDigestRecipeV2* out) {
    g_provider_callback_mask |= 1U << 7U;
    if (out == nullptr) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    *out = BioseqCachePlanDigestRecipeV2{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(BioseqCachePlanDigestRecipeV2)),
        .flags = 0xb1U,
        .digest_format_version = 3U,
        .reserved = 0,
        .plan_domain_nodes =
            fake_pointer<::List>(0x8080U),
        .expected_generation_digest = {
            0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
            0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
            0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
            0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
        },
    };
    return BIOSEQ_CACHE_STATUS_OK;
}

BioseqCacheStatusV2 fake_extract_occurrence(
        ::List*,
        ::TupleTableSlot*,
        BioseqCacheOccurrenceV2* out) {
    g_provider_callback_mask |= 1U << 8U;
    if (out == nullptr) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    *out = BioseqCacheOccurrenceV2{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(BioseqCacheOccurrenceV2)),
        .rte_ordinal = 7U,
        .logical_relation_oid = 1001U,
        .leaf_relation_oid = 1002U,
        .identity_kind = 2U,
        .reserved = 0,
        .row_identity = 0x6162636465666768ULL,
        .occurrence_high = 0x7172737475767778ULL,
        .occurrence_low = 0x8182838485868788ULL,
        .capability_mask = 0x9192939495969798ULL,
    };
    return BIOSEQ_CACHE_STATUS_OK;
}

BioseqCacheStatusV2 fake_late_fetch_batch(
        ::List*,
        ::EState*,
        const BioseqCacheOccurrenceV2*,
        std::size_t occurrence_count,
        const std::int16_t*,
        std::size_t attribute_count,
        BioseqCacheLateFetchSinkV2* sink) {
    g_provider_callback_mask |= 1U << 9U;
    if (occurrence_count != 1 || attribute_count != 1
        || sink == nullptr
        || sink->struct_size != sizeof(BioseqCacheLateFetchSinkV2)
        || sink->emit_value == nullptr
        || sink->emit_reference_status == nullptr) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    const BioseqCacheStatusV2 status = sink->emit_value(
        sink->opaque,
        0,
        0,
        static_cast<std::uintptr_t>(0xabcdefU),
        false);
    sink->emit_reference_status(
        sink->opaque, 0, BIOSEQ_CACHE_STATUS_OK);
    return status;
}

void fake_explain_domain(
        ::List*,
        BioseqCacheExplainSinkV2* sink) {
    g_provider_callback_mask |= 1U << 10U;
    if (sink == nullptr
        || sink->struct_size != sizeof(BioseqCacheExplainSinkV2)
        || sink->property_text == nullptr
        || sink->property_uint64 == nullptr
        || sink->property_bool == nullptr) {
        return;
    }
    sink->property_text(
        sink->opaque, "fake-provider", "v2");
    sink->property_uint64(
        sink->opaque, "capability", 0x1234U);
    sink->property_bool(
        sink->opaque, "complete", true);
}

static_assert(std::is_same_v<
    decltype(BioseqCacheProviderV2::analyze_query),
    decltype(&fake_analyze_query)>);
static_assert(std::is_same_v<
    decltype(BioseqCacheProviderV2::describe_path),
    decltype(&fake_describe_path)>);
static_assert(std::is_same_v<
    decltype(
        BioseqCacheProviderV2::append_domain_semantic_digest),
    decltype(&fake_append_domain_semantic_digest)>);
static_assert(std::is_same_v<
    decltype(
        BioseqCacheProviderV2::append_generation_semantic_digest),
    decltype(&fake_append_generation_semantic_digest)>);
static_assert(std::is_same_v<
    decltype(
        BioseqCacheProviderV2::
            append_runtime_generation_semantic_digest),
    decltype(
        &fake_append_runtime_generation_semantic_digest)>);
static_assert(std::is_same_v<
    decltype(BioseqCacheProviderV2::map_reusable_source),
    decltype(&fake_map_reusable_source)>);
static_assert(std::is_same_v<
    decltype(BioseqCacheProviderV2::decorate_path),
    decltype(&fake_decorate_path)>);
static_assert(std::is_same_v<
    decltype(BioseqCacheProviderV2::build_plan_digest_recipe),
    decltype(&fake_build_plan_digest_recipe)>);
static_assert(std::is_same_v<
    decltype(BioseqCacheProviderV2::extract_occurrence),
    decltype(&fake_extract_occurrence)>);
static_assert(std::is_same_v<
    decltype(BioseqCacheProviderV2::late_fetch_batch),
    decltype(&fake_late_fetch_batch)>);
static_assert(std::is_same_v<
    decltype(BioseqCacheProviderV2::explain_domain),
    decltype(&fake_explain_domain)>);

[[nodiscard]] const BioseqCacheProviderV2&
fake_provider_descriptor() noexcept {
    static const BioseqCacheProviderV2 provider{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(BioseqCacheProviderV2)),
        .abi_version = kProviderAbiVersion,
        .postgres_major = kPostgresMajor,
        .provider_id = kProviderId,
        .provider_flags = kProviderFlags,
        .reserved = 0,
        .provider_name = "separate-tu-fake-provider-v2",
        .provider_scan_methods =
            reinterpret_cast<const ::CustomScanMethods*>(
                &g_scan_methods_cookie),
        .analyze_query = &fake_analyze_query,
        .describe_path = &fake_describe_path,
        .append_domain_semantic_digest =
            &fake_append_domain_semantic_digest,
        .append_generation_semantic_digest =
            &fake_append_generation_semantic_digest,
        .append_runtime_generation_semantic_digest =
            &fake_append_runtime_generation_semantic_digest,
        .map_reusable_source = &fake_map_reusable_source,
        .decorate_path = &fake_decorate_path,
        .build_plan_digest_recipe =
            &fake_build_plan_digest_recipe,
        .extract_occurrence = &fake_extract_occurrence,
        .late_fetch_batch = &fake_late_fetch_batch,
        .explain_domain = &fake_explain_domain,
    };
    return provider;
}

BioseqCacheStatusV2 fake_bind_cache_api(
        const BioseqCacheApiV2* api) {
    if (api == nullptr
        || api->struct_size != sizeof(BioseqCacheApiV2)
        || api->abi_version != kProviderAbiVersion
        || api->postgres_major != kPostgresMajor
        || api->reserved != 0
        || api->instance_cookie != kCacheCookie
        || api->register_provider == nullptr
        || api->unregister_provider == nullptr
        || api->record_relation_write == nullptr
        || api->planner_bridge == nullptr) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    const BioseqCachePlannerBridgeV2& bridge =
        *api->planner_bridge;
    if (bridge.struct_size != sizeof(BioseqCachePlannerBridgeV2)
        || bridge.abi_version != kProviderAbiVersion
        || bridge.postgres_major != kPostgresMajor
        || bridge.after_provider_rel_paths == nullptr
        || bridge.after_planned_statement == nullptr) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }

    BioseqCacheProviderRegistrationV2 registration{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(BioseqCacheProviderRegistrationV2)),
        .provider_id = 0,
        .registry_generation = 0,
        .owner_cookie = 0,
    };
    if (api->register_provider(
            &fake_provider_descriptor(), &registration)
            != BIOSEQ_CACHE_STATUS_OK
        || registration.struct_size
            != sizeof(BioseqCacheProviderRegistrationV2)
        || registration.provider_id != kProviderId
        || registration.registry_generation != kRegistryGeneration
        || registration.owner_cookie != kOwnerCookie) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }

    const BioseqCachePhysicalIdentityV2 identity{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(BioseqCachePhysicalIdentityV2)),
        .kind = 2U,
        .length = 32U,
        .reserved = 0,
        .bytes = {
            0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
            0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
            0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
            0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f,
        },
    };
    api->record_relation_write(
        kDatabaseOid, kRelationOid, &identity);
    bridge.after_provider_rel_paths(
        nullptr,
        nullptr,
        7U,
        nullptr,
        &fake_provider_descriptor());
    bridge.after_planned_statement(nullptr);
    if (!api->unregister_provider(&registration)) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    g_cache_api_observed = true;
    return BIOSEQ_CACHE_STATUS_OK;
}

static_assert(std::is_same_v<
    decltype(BioseqCacheProviderDirectoryV2::bind_cache_api),
    decltype(&fake_bind_cache_api)>);

[[nodiscard]] const BioseqCacheProviderDirectoryV2&
fake_provider_directory() noexcept {
    static const BioseqCacheProviderDirectoryV2 directory{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(BioseqCacheProviderDirectoryV2)),
        .abi_version = kProviderAbiVersion,
        .postgres_major = kPostgresMajor,
        .reserved = 0,
        .instance_cookie = kDirectoryCookie,
        .bind_cache_api = &fake_bind_cache_api,
    };
    return directory;
}

}  // namespace

extern "C" const BioseqCacheProviderDirectoryV2*
bioseq_cache_test_fake_provider_directory_v2() noexcept {
    return &fake_provider_directory();
}

extern "C" const BioseqCacheProviderV2*
bioseq_cache_test_fake_provider_descriptor_v2() noexcept {
    return &fake_provider_descriptor();
}

extern "C" void
bioseq_cache_test_reset_fake_provider_v2() noexcept {
    g_provider_callback_mask = 0;
    g_cache_api_observed = false;
}

extern "C" std::uint32_t
bioseq_cache_test_fake_provider_callback_mask_v2() noexcept {
    return g_provider_callback_mask;
}

extern "C" bool
bioseq_cache_test_fake_provider_observed_api_v2() noexcept {
    return g_cache_api_observed;
}
