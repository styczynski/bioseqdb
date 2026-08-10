#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

import bioseq.cache.provider_api;

using namespace bioseq::cache;

namespace {

inline constexpr std::uint32_t k_postgres_major = 18;
inline constexpr std::uint32_t k_provider_id = 0x13572468U;
inline constexpr std::uint32_t k_provider_flags =
    static_cast<std::uint32_t>(
        bioseq_cache_provider_exact_source_elision_safe)
    | static_cast<std::uint32_t>(
        bioseq_cache_provider_reusable_coverage)
    | static_cast<std::uint32_t>(
        bioseq_cache_provider_complete_write_observation);
inline constexpr std::uint64_t k_directory_cookie =
    0x1020304050607080ULL;
inline constexpr std::uint64_t k_cache_cookie =
    0x8877665544332211ULL;
inline constexpr std::uint64_t k_registry_generation =
    0x1122334455667788ULL;
inline constexpr std::uint64_t k_owner_cookie =
    0x99aabbccddeeff01ULL;
inline constexpr std::uint32_t k_database_oid = 0x01020304U;
inline constexpr std::uint32_t k_relation_oid = 0x05060708U;

inline std::uint8_t g_scan_methods_cookie = 0;
inline std::uint32_t g_provider_callback_mask = 0;
inline bool g_cache_api_observed = false;

inline constexpr bioseq_cache_column_schema k_boundary_column{
    .struct_size =
        static_cast<std::uint32_t>(
            sizeof(bioseq_cache_column_schema)),
    .attno = 3,
    .semantic_id_high = 0x0102030405060708ULL,
    .semantic_id_low = 0x1112131415161718ULL,
    .type_oid = 23,
    .typmod = -1,
    .collation_oid = 100,
    .flags = 0x21U,
};

inline constexpr bioseq_cache_column_schema k_lineage_column{
    .struct_size =
        static_cast<std::uint32_t>(
            sizeof(bioseq_cache_column_schema)),
    .attno = 5,
    .semantic_id_high = 0x2122232425262728ULL,
    .semantic_id_low = 0x3132333435363738ULL,
    .type_oid = 25,
    .typmod = 64,
    .collation_oid = 101,
    .flags = 0x42U,
};

inline constexpr std::array<std::uint32_t, 1>
    k_required_boundary_attnos{3U};
inline constexpr std::array<std::uint32_t, 1>
    k_required_lineage_attnos{5U};

inline constexpr bioseq_cache_column_map_entry k_boundary_map{
    .struct_size =
        static_cast<std::uint32_t>(
            sizeof(bioseq_cache_column_map_entry)),
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

inline constexpr bioseq_cache_column_map_entry k_lineage_map{
    .struct_size =
        static_cast<std::uint32_t>(
            sizeof(bioseq_cache_column_map_entry)),
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

template <class p_value>
[[nodiscard]] p_value* fake_pointer(std::uintptr_t value) noexcept {
    return reinterpret_cast<p_value*>(value);
}

bioseq_cache_status fake_analyze_query(
        const ::Query*,
        bioseq_cache_query_intent* out) {
    g_provider_callback_mask |= 1U << 0U;
    if (out == nullptr) {
        return bioseq_cache_status_error;
    }
    *out = bioseq_cache_query_intent{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(bioseq_cache_query_intent)),
        .provider_id = k_provider_id,
        .flags = 0x71U,
        .reserved = 0,
        .provider_private_nodes =
            fake_pointer<::List>(0x1010U),
    };
    return bioseq_cache_status_ok;
}

bioseq_cache_status fake_describe_path(
        ::PlannerInfo*,
        ::RelOptInfo*,
        ::Path*,
        const bioseq_cache_query_intent*,
        bioseq_cache_path_domain* out) {
    g_provider_callback_mask |= 1U << 1U;
    if (out == nullptr) {
        return bioseq_cache_status_error;
    }
    *out = bioseq_cache_path_domain{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(bioseq_cache_path_domain)),
        .provider_id = k_provider_id,
        .compatibility_family_id = 0x4142434445464748ULL,
        .capability_mask = 0x5152535455565758ULL,
        .coverage_predicate =
            fake_pointer<::Expr>(0x2020U),
        .boundary_columns = &k_boundary_column,
        .boundary_column_count = 1,
        .lineage_columns = &k_lineage_column,
        .lineage_column_count = 1,
        .required_boundary_attnos =
            k_required_boundary_attnos.data(),
        .required_boundary_attno_count =
            k_required_boundary_attnos.size(),
        .required_lineage_attnos =
            k_required_lineage_attnos.data(),
        .required_lineage_attno_count =
            k_required_lineage_attnos.size(),
        .dependency_nodes =
            fake_pointer<::List>(0x3030U),
        .provider_private_nodes =
            fake_pointer<::List>(0x4040U),
        .provider_security_digest_format_revision =
            bioseq_cache_provider_security_digest_format_canonical,
        .provider_security_flags = 0x81U,
        .provider_security_digest = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
            0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
            0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
        },
    };
    return bioseq_cache_status_ok;
}

[[nodiscard]] bioseq_cache_status append_digest(
        bioseq_cache_digest_sink* sink,
        std::uint8_t marker) {
    if (sink == nullptr
        || sink->struct_size != sizeof(bioseq_cache_digest_sink)
        || sink->append == nullptr) {
        return bioseq_cache_status_error;
    }
    const std::array<std::uint8_t, 3> bytes{
        marker,
        static_cast<std::uint8_t>(marker + 1U),
        static_cast<std::uint8_t>(marker + 2U),
    };
    return sink->append(
        sink->opaque, bytes.data(), bytes.size())
        ? bioseq_cache_status_ok
        : bioseq_cache_status_error;
}

bioseq_cache_status fake_append_domain_semantic_digest(
        const bioseq_cache_path_domain*,
        bioseq_cache_digest_sink* sink) {
    g_provider_callback_mask |= 1U << 2U;
    return append_digest(sink, 0x10U);
}

bioseq_cache_status fake_append_generation_semantic_digest(
        const bioseq_cache_path_domain*,
        bioseq_cache_digest_sink* sink) {
    g_provider_callback_mask |= 1U << 3U;
    return append_digest(sink, 0x20U);
}

bioseq_cache_status fake_append_runtime_generation_semantic_digest(
        const ::List*,
        ::EState*,
        bioseq_cache_digest_sink* sink) {
    g_provider_callback_mask |= 1U << 4U;
    return append_digest(sink, 0x30U);
}

bioseq_cache_status fake_map_reusable_source(
        ::PlannerInfo*,
        const bioseq_cache_path_domain*,
        const bioseq_cache_reusable_source*,
        bioseq_cache_source_mapping* out) {
    g_provider_callback_mask |= 1U << 5U;
    if (out == nullptr) {
        return bioseq_cache_status_error;
    }
    *out = bioseq_cache_source_mapping{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(bioseq_cache_source_mapping)),
        .flags = 0x91U,
        .boundary_map = &k_boundary_map,
        .boundary_map_count = 1,
        .lineage_map = &k_lineage_map,
        .lineage_map_count = 1,
        .provider_private_nodes =
            fake_pointer<::List>(0x5050U),
    };
    return bioseq_cache_status_ok;
}

bioseq_cache_status fake_decorate_path(
        ::PlannerInfo*,
        ::RelOptInfo*,
        ::Path*,
        const bioseq_cache_path_domain*,
        ::List*,
        bioseq_cache_decorated_path* out) {
    g_provider_callback_mask |= 1U << 6U;
    if (out == nullptr) {
        return bioseq_cache_status_error;
    }
    *out = bioseq_cache_decorated_path{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(bioseq_cache_decorated_path)),
        .flags = 0xa1U,
        .path = fake_pointer<::Path>(0x6060U),
        .actual_boundary_columns = &k_boundary_column,
        .actual_boundary_column_count = 1,
        .actual_lineage_columns = &k_lineage_column,
        .actual_lineage_column_count = 1,
        .provider_private_nodes =
            fake_pointer<::List>(0x7070U),
    };
    return bioseq_cache_status_ok;
}

bioseq_cache_status fake_build_plan_digest_recipe(
        ::PlannerInfo*,
        const ::Plan*,
        bioseq_cache_plan_digest_recipe* out) {
    g_provider_callback_mask |= 1U << 7U;
    if (out == nullptr) {
        return bioseq_cache_status_error;
    }
    *out = bioseq_cache_plan_digest_recipe{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(bioseq_cache_plan_digest_recipe)),
        .flags = 0xb1U,
        .digest_format_revision = 3U,
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
    return bioseq_cache_status_ok;
}

bioseq_cache_status fake_extract_occurrence(
        ::List*,
        ::TupleTableSlot*,
        bioseq_cache_occurrence* out) {
    g_provider_callback_mask |= 1U << 8U;
    if (out == nullptr) {
        return bioseq_cache_status_error;
    }
    *out = bioseq_cache_occurrence{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(bioseq_cache_occurrence)),
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
    return bioseq_cache_status_ok;
}

bioseq_cache_status fake_late_fetch_batch(
        ::List*,
        ::EState*,
        const bioseq_cache_occurrence*,
        std::size_t occurrence_count,
        const std::int16_t*,
        std::size_t attribute_count,
        bioseq_cache_late_fetch_sink* sink) {
    g_provider_callback_mask |= 1U << 9U;
    if (occurrence_count != 1 || attribute_count != 1
        || sink == nullptr
        || sink->struct_size != sizeof(bioseq_cache_late_fetch_sink)
        || sink->emit_value == nullptr
        || sink->emit_reference_status == nullptr) {
        return bioseq_cache_status_error;
    }
    const bioseq_cache_status status = sink->emit_value(
        sink->opaque,
        0,
        0,
        static_cast<std::uintptr_t>(0xabcdefU),
        false);
    sink->emit_reference_status(
        sink->opaque, 0, bioseq_cache_status_ok);
    return status;
}

void fake_explain_domain(
        ::List*,
        bioseq_cache_explain_sink* sink) {
    g_provider_callback_mask |= 1U << 10U;
    if (sink == nullptr
        || sink->struct_size != sizeof(bioseq_cache_explain_sink)
        || sink->property_text == nullptr
        || sink->property_uint64 == nullptr
        || sink->property_bool == nullptr) {
        return;
    }
    sink->property_text(
        sink->opaque, "fake-provider", "current");
    sink->property_uint64(
        sink->opaque, "capability", 0x1234U);
    sink->property_bool(
        sink->opaque, "complete", true);
}

static_assert(std::is_same_v<
    decltype(bioseq_cache_provider::analyze_query),
    decltype(&fake_analyze_query)>);
static_assert(std::is_same_v<
    decltype(bioseq_cache_provider::describe_path),
    decltype(&fake_describe_path)>);
static_assert(std::is_same_v<
    decltype(
        bioseq_cache_provider::append_domain_semantic_digest),
    decltype(&fake_append_domain_semantic_digest)>);
static_assert(std::is_same_v<
    decltype(
        bioseq_cache_provider::append_generation_semantic_digest),
    decltype(&fake_append_generation_semantic_digest)>);
static_assert(std::is_same_v<
    decltype(
        bioseq_cache_provider::
            append_runtime_generation_semantic_digest),
    decltype(
        &fake_append_runtime_generation_semantic_digest)>);
static_assert(std::is_same_v<
    decltype(bioseq_cache_provider::map_reusable_source),
    decltype(&fake_map_reusable_source)>);
static_assert(std::is_same_v<
    decltype(bioseq_cache_provider::decorate_path),
    decltype(&fake_decorate_path)>);
static_assert(std::is_same_v<
    decltype(bioseq_cache_provider::build_plan_digest_recipe),
    decltype(&fake_build_plan_digest_recipe)>);
static_assert(std::is_same_v<
    decltype(bioseq_cache_provider::extract_occurrence),
    decltype(&fake_extract_occurrence)>);
static_assert(std::is_same_v<
    decltype(bioseq_cache_provider::late_fetch_batch),
    decltype(&fake_late_fetch_batch)>);
static_assert(std::is_same_v<
    decltype(bioseq_cache_provider::explain_domain),
    decltype(&fake_explain_domain)>);

[[nodiscard]] const bioseq_cache_provider&
fake_provider_descriptor() noexcept {
    static const bioseq_cache_provider provider{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(bioseq_cache_provider)),
        .abi_revision = k_provider_abi_revision,
        .postgres_major = k_postgres_major,
        .provider_id = k_provider_id,
        .provider_flags = k_provider_flags,
        .reserved = 0,
        .provider_name = "separate-tu-fake-provider",
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

bioseq_cache_status fake_bind_cache_api(
        const bioseq_cache_api* api) {
    if (api == nullptr
        || api->struct_size != sizeof(bioseq_cache_api)
        || api->abi_revision != k_provider_abi_revision
        || api->postgres_major != k_postgres_major
        || api->reserved != 0
        || api->instance_cookie != k_cache_cookie
        || api->register_provider == nullptr
        || api->unregister_provider == nullptr
        || api->record_relation_write == nullptr
        || api->planner_bridge == nullptr) {
        return bioseq_cache_status_error;
    }
    const bioseq_cache_planner_bridge& bridge =
        *api->planner_bridge;
    if (bridge.struct_size != sizeof(bioseq_cache_planner_bridge)
        || bridge.abi_revision != k_provider_abi_revision
        || bridge.postgres_major != k_postgres_major
        || bridge.after_provider_rel_paths == nullptr
        || bridge.after_planned_statement == nullptr) {
        return bioseq_cache_status_error;
    }

    bioseq_cache_provider_registration registration{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(bioseq_cache_provider_registration)),
        .provider_id = 0,
        .registry_generation = 0,
        .owner_cookie = 0,
    };
    if (api->register_provider(
            &fake_provider_descriptor(), &registration)
            != bioseq_cache_status_ok
        || registration.struct_size
            != sizeof(bioseq_cache_provider_registration)
        || registration.provider_id != k_provider_id
        || registration.registry_generation != k_registry_generation
        || registration.owner_cookie != k_owner_cookie) {
        return bioseq_cache_status_error;
    }

    const bioseq_cache_physical_identity identity{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(bioseq_cache_physical_identity)),
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
        k_database_oid, k_relation_oid, &identity);
    bridge.after_provider_rel_paths(
        nullptr,
        nullptr,
        7U,
        nullptr,
        &fake_provider_descriptor());
    bridge.after_planned_statement(nullptr);
    if (!api->unregister_provider(&registration)) {
        return bioseq_cache_status_error;
    }
    g_cache_api_observed = true;
    return bioseq_cache_status_ok;
}

static_assert(std::is_same_v<
    decltype(bioseq_cache_provider_directory::bind_cache_api),
    decltype(&fake_bind_cache_api)>);

[[nodiscard]] const bioseq_cache_provider_directory&
fake_provider_directory() noexcept {
    static const bioseq_cache_provider_directory directory{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(bioseq_cache_provider_directory)),
        .abi_revision = k_provider_abi_revision,
        .postgres_major = k_postgres_major,
        .reserved = 0,
        .instance_cookie = k_directory_cookie,
        .bind_cache_api = &fake_bind_cache_api,
    };
    return directory;
}

}  // namespace

extern "C" const bioseq_cache_provider_directory*
bioseq_cache_test_fake_provider_directory() noexcept {
    return &fake_provider_directory();
}

extern "C" const bioseq_cache_provider*
bioseq_cache_test_fake_provider_descriptor() noexcept {
    return &fake_provider_descriptor();
}

extern "C" void
bioseq_cache_test_reset_fake_provider() noexcept {
    g_provider_callback_mask = 0;
    g_cache_api_observed = false;
}

extern "C" std::uint32_t
bioseq_cache_test_fake_provider_callback_mask() noexcept {
    return g_provider_callback_mask;
}

extern "C" bool
bioseq_cache_test_fake_provider_observed_api() noexcept {
    return g_cache_api_observed;
}
