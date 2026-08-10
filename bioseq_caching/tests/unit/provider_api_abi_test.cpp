#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

import bioseq.cache.provider_api;

using namespace bioseq::cache;

extern "C" const bioseq_cache_provider_directory*
bioseq_cache_test_fake_provider_directory() noexcept;
extern "C" const bioseq_cache_provider*
bioseq_cache_test_fake_provider_descriptor() noexcept;
extern "C" void
bioseq_cache_test_reset_fake_provider() noexcept;
extern "C" std::uint32_t
bioseq_cache_test_fake_provider_callback_mask() noexcept;
extern "C" bool
bioseq_cache_test_fake_provider_observed_api() noexcept;

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
inline constexpr std::uint32_t k_all_provider_callbacks =
    (1U << 11U) - 1U;

struct cache_observations {
    bool provider_descriptor_valid = false;
    bool registration_returned = false;
    bool unregister_token_valid = false;
    bool physical_identity_valid = false;
    bool provider_bridge_called = false;
    bool finalized_bridge_called = false;
    std::array<std::uint8_t, 9> digest_bytes{};
    std::size_t digest_size = 0;
    bool late_value_valid = false;
    bool late_status_valid = false;
    bool explain_text_valid = false;
    bool explain_uint64_valid = false;
    bool explain_bool_valid = false;
};

inline cache_observations g_observations{};

[[nodiscard]] bool provider_descriptor_valid(
        const bioseq_cache_provider* provider) noexcept {
    return provider != nullptr
        && provider->struct_size == sizeof(bioseq_cache_provider)
        && provider->abi_revision == k_provider_abi_revision
        && provider->postgres_major == k_postgres_major
        && provider->provider_id == k_provider_id
        && provider->provider_flags == k_provider_flags
        && provider->reserved == 0
        && provider->provider_name != nullptr
        && std::string_view(provider->provider_name)
            == "separate-tu-fake-provider"
        && provider->provider_scan_methods != nullptr
        && provider->analyze_query != nullptr
        && provider->describe_path != nullptr
        && provider->append_domain_semantic_digest != nullptr
        && provider->append_generation_semantic_digest != nullptr
        && provider->append_runtime_generation_semantic_digest != nullptr
        && provider->map_reusable_source != nullptr
        && provider->decorate_path != nullptr
        && provider->build_plan_digest_recipe != nullptr
        && provider->extract_occurrence != nullptr
        && provider->late_fetch_batch != nullptr
        && provider->explain_domain != nullptr;
}

bioseq_cache_status fake_cache_register_provider(
        const bioseq_cache_provider* provider,
        bioseq_cache_provider_registration* registration) {
    g_observations.provider_descriptor_valid =
        provider_descriptor_valid(provider);
    if (!g_observations.provider_descriptor_valid
        || registration == nullptr
        || registration->struct_size
            != sizeof(bioseq_cache_provider_registration)) {
        return bioseq_cache_status_error;
    }
    *registration = bioseq_cache_provider_registration{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(bioseq_cache_provider_registration)),
        .provider_id = k_provider_id,
        .registry_generation = k_registry_generation,
        .owner_cookie = k_owner_cookie,
    };
    g_observations.registration_returned = true;
    return bioseq_cache_status_ok;
}

bool fake_cache_unregister_provider(
        const bioseq_cache_provider_registration* registration) {
    g_observations.unregister_token_valid =
        registration != nullptr
        && registration->struct_size
            == sizeof(bioseq_cache_provider_registration)
        && registration->provider_id == k_provider_id
        && registration->registry_generation == k_registry_generation
        && registration->owner_cookie == k_owner_cookie;
    return g_observations.unregister_token_valid;
}

void fake_cache_record_relation_write(
        std::uint32_t database_oid,
        std::uint32_t relation_oid,
        const bioseq_cache_physical_identity* identity) {
    bool bytes_valid = identity != nullptr;
    if (identity != nullptr) {
        for (std::size_t index = 0;
             index < sizeof(identity->bytes);
             ++index) {
            bytes_valid = bytes_valid
                && identity->bytes[index]
                    == static_cast<std::uint8_t>(0x40U + index);
        }
    }
    g_observations.physical_identity_valid =
        database_oid == k_database_oid
        && relation_oid == k_relation_oid
        && identity != nullptr
        && identity->struct_size
            == sizeof(bioseq_cache_physical_identity)
        && identity->kind == 2U
        && identity->length == sizeof(identity->bytes)
        && identity->reserved == 0
        && bytes_valid;
}

void fake_cache_after_provider_rel_paths(
        ::PlannerInfo* root,
        ::RelOptInfo* rel,
        unsigned int rti,
        ::RangeTblEntry* rte,
        const bioseq_cache_provider* provider) {
    g_observations.provider_bridge_called =
        root == nullptr
        && rel == nullptr
        && rti == 7U
        && rte == nullptr
        && provider
            == bioseq_cache_test_fake_provider_descriptor()
        && provider_descriptor_valid(provider);
}

void fake_cache_after_planned_statement(
        ::PlannedStmt* statement) {
    g_observations.finalized_bridge_called =
        statement == nullptr;
}

bool fake_digest_append(
        void* opaque,
        const void* data,
        std::size_t size) noexcept {
    auto* observations =
        static_cast<cache_observations*>(opaque);
    if (observations == nullptr || data == nullptr
        || observations->digest_size + size
            > observations->digest_bytes.size()) {
        return false;
    }
    const auto* bytes =
        static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        observations->digest_bytes[
            observations->digest_size + index] = bytes[index];
    }
    observations->digest_size += size;
    return true;
}

bioseq_cache_status fake_late_emit_value(
        void* opaque,
        std::size_t reference_index,
        std::size_t requested_attribute_index,
        std::uintptr_t datum,
        bool is_null) noexcept {
    auto* observations =
        static_cast<cache_observations*>(opaque);
    if (observations == nullptr) {
        return bioseq_cache_status_error;
    }
    observations->late_value_valid =
        reference_index == 0
        && requested_attribute_index == 0
        && datum == static_cast<std::uintptr_t>(0xabcdefU)
        && !is_null;
    return observations->late_value_valid
        ? bioseq_cache_status_ok
        : bioseq_cache_status_error;
}

void fake_late_emit_reference_status(
        void* opaque,
        std::size_t reference_index,
        bioseq_cache_status status) noexcept {
    auto* observations =
        static_cast<cache_observations*>(opaque);
    if (observations != nullptr) {
        observations->late_status_valid =
            reference_index == 0
            && status == bioseq_cache_status_ok;
    }
}

void fake_explain_property_text(
        void* opaque,
        const char* name,
        const char* value) noexcept {
    auto* observations =
        static_cast<cache_observations*>(opaque);
    if (observations != nullptr) {
        observations->explain_text_valid =
            name != nullptr
            && value != nullptr
            && std::string_view(name) == "fake-provider"
            && std::string_view(value) == "current";
    }
}

void fake_explain_property_uint64(
        void* opaque,
        const char* name,
        std::uint64_t value) noexcept {
    auto* observations =
        static_cast<cache_observations*>(opaque);
    if (observations != nullptr) {
        observations->explain_uint64_valid =
            name != nullptr
            && std::string_view(name) == "capability"
            && value == 0x1234U;
    }
}

void fake_explain_property_bool(
        void* opaque,
        const char* name,
        bool value) noexcept {
    auto* observations =
        static_cast<cache_observations*>(opaque);
    if (observations != nullptr) {
        observations->explain_bool_valid =
            name != nullptr
            && std::string_view(name) == "complete"
            && value;
    }
}

static_assert(std::is_same_v<
    decltype(bioseq_cache_api::register_provider),
    decltype(&fake_cache_register_provider)>);
static_assert(std::is_same_v<
    decltype(bioseq_cache_api::unregister_provider),
    decltype(&fake_cache_unregister_provider)>);
static_assert(std::is_same_v<
    decltype(bioseq_cache_api::record_relation_write),
    decltype(&fake_cache_record_relation_write)>);
static_assert(std::is_same_v<
    decltype(
        bioseq_cache_planner_bridge::after_provider_rel_paths),
    decltype(&fake_cache_after_provider_rel_paths)>);
static_assert(std::is_same_v<
    decltype(
        bioseq_cache_planner_bridge::after_planned_statement),
    decltype(&fake_cache_after_planned_statement)>);
static_assert(std::is_same_v<
    decltype(bioseq_cache_digest_sink::append),
    decltype(&fake_digest_append)>);
static_assert(std::is_same_v<
    decltype(bioseq_cache_late_fetch_sink::emit_value),
    decltype(&fake_late_emit_value)>);
static_assert(std::is_same_v<
    decltype(
        bioseq_cache_late_fetch_sink::emit_reference_status),
    decltype(&fake_late_emit_reference_status)>);
static_assert(std::is_same_v<
    decltype(bioseq_cache_explain_sink::property_text),
    decltype(&fake_explain_property_text)>);
static_assert(std::is_same_v<
    decltype(bioseq_cache_explain_sink::property_uint64),
    decltype(&fake_explain_property_uint64)>);
static_assert(std::is_same_v<
    decltype(bioseq_cache_explain_sink::property_bool),
    decltype(&fake_explain_property_bool)>);

void require_column(
        const bioseq_cache_column_schema& column,
        std::uint32_t expected_attno,
        std::uint64_t expected_semantic_high,
        std::uint64_t expected_semantic_low,
        std::uint32_t expected_type,
        std::int32_t expected_typmod,
        std::uint32_t expected_collation,
        std::uint32_t expected_flags) {
    REQUIRE(
        column.struct_size
        == sizeof(bioseq_cache_column_schema));
    REQUIRE(column.attno == expected_attno);
    REQUIRE(column.semantic_id_high == expected_semantic_high);
    REQUIRE(column.semantic_id_low == expected_semantic_low);
    REQUIRE(column.type_oid == expected_type);
    REQUIRE(column.typmod == expected_typmod);
    REQUIRE(column.collation_oid == expected_collation);
    REQUIRE(column.flags == expected_flags);
}

void require_map_entry(
        const bioseq_cache_column_map_entry& entry,
        std::uint32_t expected_consumer_attno,
        std::uint32_t expected_source_attno,
        std::uint32_t expected_flags,
        std::uint64_t expected_semantic_high,
        std::uint64_t expected_semantic_low,
        std::uint32_t expected_type,
        std::int32_t expected_typmod,
        std::uint32_t expected_collation) {
    REQUIRE(
        entry.struct_size
        == sizeof(bioseq_cache_column_map_entry));
    REQUIRE(entry.consumer_attno == expected_consumer_attno);
    REQUIRE(entry.source_attno == expected_source_attno);
    REQUIRE(entry.flags == expected_flags);
    REQUIRE(entry.semantic_id_high == expected_semantic_high);
    REQUIRE(entry.semantic_id_low == expected_semantic_low);
    REQUIRE(entry.type_oid == expected_type);
    REQUIRE(entry.typmod == expected_typmod);
    REQUIRE(entry.collation_oid == expected_collation);
    REQUIRE(entry.reserved == 0);
}

}  // namespace

TEST_CASE(
    "separate translation units agree on the complete current provider ABI",
    "[component:cache][layer:unit][feature:provider]") {
    g_observations = {};
    bioseq_cache_test_reset_fake_provider();

    const bioseq_cache_provider_directory* const directory =
        bioseq_cache_test_fake_provider_directory();
    REQUIRE(directory != nullptr);
    REQUIRE(
        directory->struct_size
        == sizeof(bioseq_cache_provider_directory));
    REQUIRE(directory->abi_revision == k_provider_abi_revision);
    REQUIRE(directory->postgres_major == k_postgres_major);
    REQUIRE(directory->reserved == 0);
    REQUIRE(directory->instance_cookie == k_directory_cookie);
    REQUIRE(directory->bind_cache_api != nullptr);

    const bioseq_cache_provider* const provider =
        bioseq_cache_test_fake_provider_descriptor();
    REQUIRE(provider_descriptor_valid(provider));

    const bioseq_cache_planner_bridge planner_bridge{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(bioseq_cache_planner_bridge)),
        .abi_revision = k_provider_abi_revision,
        .postgres_major = k_postgres_major,
        .after_provider_rel_paths =
            &fake_cache_after_provider_rel_paths,
        .after_planned_statement =
            &fake_cache_after_planned_statement,
    };
    const bioseq_cache_api cache_api{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(bioseq_cache_api)),
        .abi_revision = k_provider_abi_revision,
        .postgres_major = k_postgres_major,
        .reserved = 0,
        .instance_cookie = k_cache_cookie,
        .register_provider = &fake_cache_register_provider,
        .unregister_provider =
            &fake_cache_unregister_provider,
        .record_relation_write =
            &fake_cache_record_relation_write,
        .planner_bridge = &planner_bridge,
    };

    REQUIRE(
        directory->bind_cache_api(&cache_api)
        == bioseq_cache_status_ok);
    REQUIRE(g_observations.provider_descriptor_valid);
    REQUIRE(g_observations.registration_returned);
    REQUIRE(g_observations.unregister_token_valid);
    REQUIRE(g_observations.physical_identity_valid);
    REQUIRE(g_observations.provider_bridge_called);
    REQUIRE(g_observations.finalized_bridge_called);
    REQUIRE(
        bioseq_cache_test_fake_provider_observed_api());

    bioseq_cache_query_intent intent{
        .struct_size = 0,
        .provider_id = 0,
        .flags = 0,
        .reserved = 0,
        .provider_private_nodes = nullptr,
    };
    REQUIRE(
        provider->analyze_query(nullptr, &intent)
        == bioseq_cache_status_ok);
    REQUIRE(
        intent.struct_size == sizeof(bioseq_cache_query_intent));
    REQUIRE(intent.provider_id == k_provider_id);
    REQUIRE(intent.flags == 0x71U);
    REQUIRE(intent.reserved == 0);
    REQUIRE(intent.provider_private_nodes != nullptr);

    bioseq_cache_path_domain domain{
        .struct_size = 0,
        .provider_id = 0,
        .compatibility_family_id = 0,
        .capability_mask = 0,
        .coverage_predicate = nullptr,
        .boundary_columns = nullptr,
        .boundary_column_count = 0,
        .lineage_columns = nullptr,
        .lineage_column_count = 0,
        .required_boundary_attnos = nullptr,
        .required_boundary_attno_count = 0,
        .required_lineage_attnos = nullptr,
        .required_lineage_attno_count = 0,
        .dependency_nodes = nullptr,
        .provider_private_nodes = nullptr,
        .provider_security_digest_format_revision = 0,
        .provider_security_flags = 0,
        .provider_security_digest = {},
    };
    REQUIRE(
        provider->describe_path(
            nullptr, nullptr, nullptr, &intent, &domain)
        == bioseq_cache_status_ok);
    REQUIRE(
        domain.struct_size == sizeof(bioseq_cache_path_domain));
    REQUIRE(domain.provider_id == k_provider_id);
    REQUIRE(
        domain.compatibility_family_id
        == 0x4142434445464748ULL);
    REQUIRE(domain.capability_mask == 0x5152535455565758ULL);
    REQUIRE(domain.coverage_predicate != nullptr);
    REQUIRE(domain.boundary_columns != nullptr);
    REQUIRE(domain.boundary_column_count == 1);
    require_column(
        domain.boundary_columns[0],
        3U,
        0x0102030405060708ULL,
        0x1112131415161718ULL,
        23U,
        -1,
        100U,
        0x21U);
    REQUIRE(domain.lineage_columns != nullptr);
    REQUIRE(domain.lineage_column_count == 1);
    require_column(
        domain.lineage_columns[0],
        5U,
        0x2122232425262728ULL,
        0x3132333435363738ULL,
        25U,
        64,
        101U,
        0x42U);
    REQUIRE(domain.required_boundary_attnos != nullptr);
    REQUIRE(domain.required_boundary_attno_count == 1);
    REQUIRE(domain.required_boundary_attnos[0] == 3U);
    REQUIRE(domain.required_lineage_attnos != nullptr);
    REQUIRE(domain.required_lineage_attno_count == 1);
    REQUIRE(domain.required_lineage_attnos[0] == 5U);
    REQUIRE(domain.dependency_nodes != nullptr);
    REQUIRE(domain.provider_private_nodes != nullptr);
    REQUIRE(
        domain.provider_security_digest_format_revision
        == bioseq_cache_provider_security_digest_format_canonical);
    REQUIRE(domain.provider_security_flags == 0x81U);
    for (std::size_t index = 0;
         index < sizeof(domain.provider_security_digest);
         ++index) {
        REQUIRE(
            domain.provider_security_digest[index]
            == static_cast<std::uint8_t>(index));
    }

    bioseq_cache_digest_sink digest_sink{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(bioseq_cache_digest_sink)),
        .opaque = &g_observations,
        .append = &fake_digest_append,
    };
    REQUIRE(
        provider->append_domain_semantic_digest(
            &domain, &digest_sink)
        == bioseq_cache_status_ok);
    REQUIRE(
        provider->append_generation_semantic_digest(
            &domain, &digest_sink)
        == bioseq_cache_status_ok);
    REQUIRE(
        provider->append_runtime_generation_semantic_digest(
            nullptr, nullptr, &digest_sink)
        == bioseq_cache_status_ok);
    REQUIRE(g_observations.digest_size == 9);
    const std::array<std::uint8_t, 9> expected_digest{
        0x10, 0x11, 0x12,
        0x20, 0x21, 0x22,
        0x30, 0x31, 0x32};
    REQUIRE(g_observations.digest_bytes == expected_digest);

    const bioseq_cache_reusable_source source{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(bioseq_cache_reusable_source)),
        .provider_id = k_provider_id,
        .compatibility_family_id =
            domain.compatibility_family_id,
        .capability_mask = domain.capability_mask,
        .boundary_columns = domain.boundary_columns,
        .boundary_column_count = domain.boundary_column_count,
        .lineage_columns = domain.lineage_columns,
        .lineage_column_count = domain.lineage_column_count,
        .provider_private_nodes =
            domain.provider_private_nodes,
    };
    bioseq_cache_source_mapping mapping{
        .struct_size = 0,
        .flags = 0,
        .boundary_map = nullptr,
        .boundary_map_count = 0,
        .lineage_map = nullptr,
        .lineage_map_count = 0,
        .provider_private_nodes = nullptr,
    };
    REQUIRE(
        provider->map_reusable_source(
            nullptr, &domain, &source, &mapping)
        == bioseq_cache_status_ok);
    REQUIRE(
        mapping.struct_size
        == sizeof(bioseq_cache_source_mapping));
    REQUIRE(mapping.flags == 0x91U);
    REQUIRE(mapping.boundary_map != nullptr);
    REQUIRE(mapping.boundary_map_count == 1);
    require_map_entry(
        mapping.boundary_map[0],
        3U,
        7U,
        0x51U,
        0x0102030405060708ULL,
        0x1112131415161718ULL,
        23U,
        -1,
        100U);
    REQUIRE(mapping.lineage_map != nullptr);
    REQUIRE(mapping.lineage_map_count == 1);
    require_map_entry(
        mapping.lineage_map[0],
        5U,
        9U,
        0x61U,
        0x2122232425262728ULL,
        0x3132333435363738ULL,
        25U,
        64,
        101U);
    REQUIRE(mapping.provider_private_nodes != nullptr);

    bioseq_cache_decorated_path decorated{
        .struct_size = 0,
        .flags = 0,
        .path = nullptr,
        .actual_boundary_columns = nullptr,
        .actual_boundary_column_count = 0,
        .actual_lineage_columns = nullptr,
        .actual_lineage_column_count = 0,
        .provider_private_nodes = nullptr,
    };
    REQUIRE(
        provider->decorate_path(
            nullptr,
            nullptr,
            nullptr,
            &domain,
            nullptr,
            &decorated)
        == bioseq_cache_status_ok);
    REQUIRE(
        decorated.struct_size
        == sizeof(bioseq_cache_decorated_path));
    REQUIRE(decorated.flags == 0xa1U);
    REQUIRE(decorated.path != nullptr);
    REQUIRE(decorated.actual_boundary_columns != nullptr);
    REQUIRE(decorated.actual_boundary_column_count == 1);
    REQUIRE(decorated.actual_lineage_columns != nullptr);
    REQUIRE(decorated.actual_lineage_column_count == 1);
    REQUIRE(decorated.provider_private_nodes != nullptr);

    bioseq_cache_plan_digest_recipe recipe{
        .struct_size = 0,
        .flags = 0,
        .digest_format_revision = 0,
        .reserved = 0,
        .plan_domain_nodes = nullptr,
        .expected_generation_digest = {},
    };
    REQUIRE(
        provider->build_plan_digest_recipe(
            nullptr, nullptr, &recipe)
        == bioseq_cache_status_ok);
    REQUIRE(
        recipe.struct_size
        == sizeof(bioseq_cache_plan_digest_recipe));
    REQUIRE(recipe.flags == 0xb1U);
    REQUIRE(recipe.digest_format_revision == 3U);
    REQUIRE(recipe.reserved == 0);
    REQUIRE(recipe.plan_domain_nodes != nullptr);
    for (std::size_t index = 0;
         index < sizeof(recipe.expected_generation_digest);
         ++index) {
        REQUIRE(
            recipe.expected_generation_digest[index]
            == static_cast<std::uint8_t>(0x20U + index));
    }

    bioseq_cache_occurrence occurrence{
        .struct_size = 0,
        .rte_ordinal = 0,
        .logical_relation_oid = 0,
        .leaf_relation_oid = 0,
        .identity_kind = 0,
        .reserved = 0,
        .row_identity = 0,
        .occurrence_high = 0,
        .occurrence_low = 0,
        .capability_mask = 0,
    };
    REQUIRE(
        provider->extract_occurrence(
            nullptr, nullptr, &occurrence)
        == bioseq_cache_status_ok);
    REQUIRE(
        occurrence.struct_size
        == sizeof(bioseq_cache_occurrence));
    REQUIRE(occurrence.rte_ordinal == 7U);
    REQUIRE(occurrence.logical_relation_oid == 1001U);
    REQUIRE(occurrence.leaf_relation_oid == 1002U);
    REQUIRE(occurrence.identity_kind == 2U);
    REQUIRE(occurrence.reserved == 0);
    REQUIRE(
        occurrence.row_identity == 0x6162636465666768ULL);
    REQUIRE(
        occurrence.occurrence_high == 0x7172737475767778ULL);
    REQUIRE(
        occurrence.occurrence_low == 0x8182838485868788ULL);
    REQUIRE(
        occurrence.capability_mask == 0x9192939495969798ULL);

    bioseq_cache_late_fetch_sink late_sink{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(bioseq_cache_late_fetch_sink)),
        .opaque = &g_observations,
        .emit_value = &fake_late_emit_value,
        .emit_reference_status =
            &fake_late_emit_reference_status,
    };
    const std::array<std::int16_t, 1> attributes{3};
    REQUIRE(
        provider->late_fetch_batch(
            nullptr,
            nullptr,
            &occurrence,
            1,
            attributes.data(),
            attributes.size(),
            &late_sink)
        == bioseq_cache_status_ok);
    REQUIRE(g_observations.late_value_valid);
    REQUIRE(g_observations.late_status_valid);

    bioseq_cache_explain_sink explain_sink{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(bioseq_cache_explain_sink)),
        .opaque = &g_observations,
        .property_text = &fake_explain_property_text,
        .property_uint64 = &fake_explain_property_uint64,
        .property_bool = &fake_explain_property_bool,
    };
    provider->explain_domain(nullptr, &explain_sink);
    REQUIRE(g_observations.explain_text_valid);
    REQUIRE(g_observations.explain_uint64_valid);
    REQUIRE(g_observations.explain_bool_valid);

    const bioseq_cache_path_description path_description{
        .struct_size =
            static_cast<std::uint32_t>(
                sizeof(bioseq_cache_path_description)),
        .flags = 0xc1U,
        .capability_mask = 0xa1a2a3a4a5a6a7a8ULL,
        .dependency_nodes = domain.dependency_nodes,
        .provider_private_nodes = domain.provider_private_nodes,
    };
    REQUIRE(
        path_description.struct_size
        == sizeof(bioseq_cache_path_description));
    REQUIRE(path_description.flags == 0xc1U);
    REQUIRE(
        path_description.capability_mask
        == 0xa1a2a3a4a5a6a7a8ULL);
    REQUIRE(path_description.dependency_nodes != nullptr);
    REQUIRE(path_description.provider_private_nodes != nullptr);

    REQUIRE(
        bioseq_cache_test_fake_provider_callback_mask()
        == k_all_provider_callbacks);
}
