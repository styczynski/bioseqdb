#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

import bioseq.cache.core;

using namespace bioseq::cache;

namespace {

struct TestCounter {
    std::uint64_t value = 0;
    bool fail_next_exchange = false;

    [[nodiscard]] std::uint64_t load() const noexcept {
        return value;
    }

    bool compare_exchange(
        std::uint64_t& expected,
        std::uint64_t desired) noexcept {
        if (fail_next_exchange) {
            fail_next_exchange = false;
            expected = value;
            return false;
        }
        if (value != expected) {
            expected = value;
            return false;
        }
        value = desired;
        return true;
    }

    void subtract(std::uint64_t bytes) noexcept {
        value -= bytes;
    }
};

struct TestBudget {
    std::uint64_t allocatable = 0;
    std::uint64_t total = 0;
    bool pressure = false;

    [[nodiscard]] std::uint64_t allocatable_bytes() const noexcept {
        return allocatable;
    }

    [[nodiscard]] std::uint64_t total_bytes() const noexcept {
        return total;
    }

    [[nodiscard]] bool under_pressure() const noexcept {
        return pressure;
    }
};

BioseqCacheStatusV1 analyze_query(
    const ::Query*,
    BioseqCacheQueryIntentV1*) {
    return BIOSEQ_CACHE_STATUS_OK;
}

BioseqCacheStatusV1 describe_path(
    ::PlannerInfo*,
    ::RelOptInfo*,
    ::Path*,
    const BioseqCacheQueryIntentV1*,
    BioseqCachePathDomainV1*) {
    return BIOSEQ_CACHE_STATUS_OK;
}

BioseqCacheStatusV1 append_domain(
    const BioseqCachePathDomainV1*,
    BioseqCacheDigestSinkV1*) {
    return BIOSEQ_CACHE_STATUS_OK;
}

BioseqCacheStatusV1 append_runtime(
    const ::List*,
    ::EState*,
    BioseqCacheDigestSinkV1*) {
    return BIOSEQ_CACHE_STATUS_OK;
}

BioseqCacheStatusV1 map_source(
    ::PlannerInfo*,
    const BioseqCachePathDomainV1*,
    const BioseqCacheReusableSourceV1*,
    BioseqCacheSourceMappingV1*) {
    return BIOSEQ_CACHE_STATUS_OK;
}

BioseqCacheStatusV1 decorate_path(
    ::PlannerInfo*,
    ::RelOptInfo*,
    ::Path*,
    const BioseqCachePathDomainV1*,
    ::List*,
    BioseqCacheDecoratedPathV1*) {
    return BIOSEQ_CACHE_STATUS_OK;
}

BioseqCacheStatusV1 build_recipe(
    ::PlannerInfo*,
    const ::Plan*,
    BioseqCachePlanDigestRecipeV1*) {
    return BIOSEQ_CACHE_STATUS_OK;
}

BioseqCacheStatusV1 extract_occurrence(
    ::List*,
    ::TupleTableSlot*,
    BioseqCacheOccurrenceV1*) {
    return BIOSEQ_CACHE_STATUS_OK;
}

BioseqCacheStatusV1 late_fetch(
    ::List*,
    ::EState*,
    const BioseqCacheOccurrenceV1*,
    std::size_t,
    const std::int16_t*,
    std::size_t,
    BioseqCacheLateFetchSinkV1*) {
    return BIOSEQ_CACHE_STATUS_OK;
}

BioseqCacheColumnSchemaV1 column(
    std::uint32_t attno,
    std::uint64_t semantic_id = 7) {
    return BioseqCacheColumnSchemaV1{
        .struct_size = sizeof(BioseqCacheColumnSchemaV1),
        .attno = attno,
        .semantic_id_high = 0,
        .semantic_id_low = semantic_id,
        .type_oid = 23,
        .typmod = -1,
        .collation_oid = 0,
        .flags = 0,
    };
}

BioseqCacheColumnMapEntryV1 map_entry(
    std::uint32_t consumer_attno,
    std::uint32_t source_attno,
    std::uint64_t semantic_id = 7) {
    return BioseqCacheColumnMapEntryV1{
        .struct_size = sizeof(BioseqCacheColumnMapEntryV1),
        .consumer_attno = consumer_attno,
        .source_attno = source_attno,
        .flags = 0,
        .semantic_id_high = 0,
        .semantic_id_low = semantic_id,
        .type_oid = 23,
        .typmod = -1,
        .collation_oid = 0,
        .reserved = 0,
    };
}

} // namespace

TEST_CASE("shared admission never exceeds the live 25-percent ceiling") {
    TestCounter counter{};
    TestBudget budget{.allocatable = 1'000, .total = 2'000};
    BasicCacheAdmission admission(counter, budget);

    counter.fail_next_exchange = true;
    auto first = admission.try_reserve(400);
    REQUIRE(first.has_value());
    REQUIRE(admission.reserved_bytes() == 400);
    REQUIRE_FALSE(admission.try_reserve(101).has_value());

    REQUIRE(first->grow_to(450));
    REQUIRE(admission.reserved_bytes() == 450);
    REQUIRE_FALSE(first->grow_to(501));
    first->shrink_to(125);
    REQUIRE(admission.reserved_bytes() == 125);

    auto moved = std::move(*first);
    REQUIRE_FALSE(static_cast<bool>(*first));
    REQUIRE(static_cast<bool>(moved));
    moved.release();
    REQUIRE(admission.reserved_bytes() == 0);
}

TEST_CASE("admission reports pressure, overflow, and file floors") {
    TestCounter counter{};
    TestBudget budget{.allocatable = 100, .total = 0, .pressure = true};
    BasicCacheAdmission admission(counter, budget);
    REQUIRE(admission.try_reserve(1).error() == AdmissionError::SystemPressure);

    budget.pressure = false;
    counter.value = (std::numeric_limits<std::uint64_t>::max)();
    REQUIRE(admission.try_reserve(1).error() == AdmissionError::CounterOverflow);

    constexpr std::uint64_t capacity = 200ULL << 30;
    REQUIRE(FileAdmissionPolicy::ceiling_bytes(capacity) == (20ULL << 30));
    REQUIRE(FileAdmissionPolicy::free_floor_bytes(capacity) == (20ULL << 30));
    REQUIRE(FileAdmissionPolicy::can_reserve(
        0, 1ULL << 30, capacity, 25ULL << 30));
    REQUIRE_FALSE(FileAdmissionPolicy::can_reserve(
        0, 6ULL << 30, capacity, 25ULL << 30));
}

TEST_CASE("provider schema maps require exact SQL and semantic meaning") {
    const std::array consumer_columns{column(1)};
    const std::array source_columns{column(9)};
    const std::array<std::uint32_t, 1> consumer_required{1};
    const std::array mappings{map_entry(1, 9)};

    const ProviderContractView consumer{
        .boundary = {
            .columns = consumer_columns.data(),
            .column_count = consumer_columns.size(),
            .required_attnos = consumer_required.data(),
            .required_attno_count = consumer_required.size(),
        },
        .capability_mask = BIOSEQ_CAP_SCORE | BIOSEQ_CAP_CIGAR,
    };
    const ProviderContractView source{
        .boundary = {
            .columns = source_columns.data(),
            .column_count = source_columns.size(),
        },
        .capability_mask = BIOSEQ_CAP_SCORE | BIOSEQ_CAP_CIGAR
            | BIOSEQ_CAP_MAPQ,
    };
    BioseqCacheSourceMappingV1 mapping{
        .struct_size = sizeof(BioseqCacheSourceMappingV1),
        .flags = 0,
        .boundary_map = mappings.data(),
        .boundary_map_count = mappings.size(),
    };
    REQUIRE(validate_source_map(consumer, source, mapping).has_value());

    auto bad_mapping = mappings;
    bad_mapping[0].collation_oid = 100;
    mapping.boundary_map = bad_mapping.data();
    REQUIRE(
        validate_source_map(consumer, source, mapping).error()
        == SourceMapError::CollationMismatch);

    mapping.boundary_map = mappings.data();
    mapping.flags = 1;
    REQUIRE(
        validate_source_map(consumer, source, mapping).error()
        == SourceMapError::InvalidFlags);

    auto weak_source = source;
    weak_source.capability_mask = BIOSEQ_CAP_SCORE;
    mapping.flags = 0;
    REQUIRE(
        validate_source_map(consumer, weak_source, mapping).error()
        == SourceMapError::CapabilityMissing);
}

TEST_CASE("repeated source attributes are legal only for identical meaning") {
    const std::array consumer_columns{column(1), column(2)};
    const std::array source_columns{column(9)};
    const std::array<std::uint32_t, 2> required{1, 2};
    const std::array mappings{map_entry(1, 9), map_entry(2, 9)};
    const ProviderSchemaView consumer{
        .columns = consumer_columns.data(),
        .column_count = consumer_columns.size(),
        .required_attnos = required.data(),
        .required_attno_count = required.size(),
    };
    const ProviderSchemaView source{
        .columns = source_columns.data(),
        .column_count = source_columns.size(),
    };
    REQUIRE(validate_map_half(
        consumer, source, mappings.data(), mappings.size()).has_value());

    auto incompatible_columns = consumer_columns;
    incompatible_columns[1].semantic_id_low = 8;
    const ProviderSchemaView incompatible{
        .columns = incompatible_columns.data(),
        .column_count = incompatible_columns.size(),
        .required_attnos = required.data(),
        .required_attno_count = required.size(),
    };
    REQUIRE(
        validate_map_half(
            incompatible, source, mappings.data(), mappings.size()).error()
        == SourceMapError::SemanticIdMismatch);
}

TEST_CASE("provider registration enforces callbacks implied by flags") {
    BioseqCacheProviderV1 provider{
        .struct_size = sizeof(BioseqCacheProviderV1),
        .abi_version = kProviderAbiVersion,
        .postgres_major = 18,
        .provider_id = 9,
        .provider_flags = 0,
        .reserved = 0,
        .provider_name = "unit-provider",
        .provider_scan_methods = nullptr,
        .analyze_query = analyze_query,
        .describe_path = describe_path,
        .append_domain_semantic_digest = append_domain,
        .append_generation_semantic_digest = append_domain,
        .append_runtime_generation_semantic_digest = append_runtime,
        .map_reusable_source = map_source,
        .decorate_path = decorate_path,
        .build_plan_digest_recipe = build_recipe,
        .extract_occurrence = extract_occurrence,
        .late_fetch_batch = late_fetch,
        .explain_domain = nullptr,
    };
    REQUIRE(provider_registration_valid(&provider, 18));
    REQUIRE_FALSE(provider_registration_valid(&provider, 17));

    provider.provider_flags = BIOSEQ_CACHE_PROVIDER_REUSABLE_COVERAGE;
    provider.late_fetch_batch = nullptr;
    REQUIRE_FALSE(provider_registration_valid(&provider, 18));
    provider.late_fetch_batch = late_fetch;
    REQUIRE(provider_registration_valid(&provider, 18));

    provider.provider_flags = BIOSEQ_CACHE_PROVIDER_EXACT_SOURCE_ELISION_SAFE;
    REQUIRE_FALSE(provider_registration_valid(&provider, 18));
    provider.provider_scan_methods =
        reinterpret_cast<const ::CustomScanMethods*>(std::uintptr_t{1});
    REQUIRE(provider_registration_valid(&provider, 18));

    provider.provider_flags = 1U << 31;
    REQUIRE_FALSE(provider_registration_valid(&provider, 18));
}

TEST_CASE("canonical keys, identities, dependencies, and settings are exact") {
    CanonicalCacheKey first{.bytes = {std::byte{1}, std::byte{2}}};
    CanonicalCacheKey same = first;
    CanonicalCacheKey collision = first;
    collision.bytes[1] = std::byte{3};
    REQUIRE(collision_checked_key_equal(first, same));
    REQUIRE_FALSE(collision_checked_key_equal(first, collision));

    PhysicalIdentityV1 left{
        .kind = PhysicalIdentityKind::ProviderOpaque,
        .length = 2,
    };
    left.bytes[0] = std::byte{4};
    left.bytes[1] = std::byte{5};
    PhysicalIdentityV1 right = left;
    right.bytes[2] = std::byte{99};
    REQUIRE(physical_identity_equal(left, right));

    BaseRowReferenceV1 occurrence_a{
        .rte_ordinal = 1,
        .logical_rel_oid = 10,
        .leaf_rel_oid = 11,
        .physical_identity = left,
        .identity_kind = RowIdentityKind::ProviderOpaque,
        .row_number_or_opaque = 55,
        .xmin_identity = 100,
    };
    auto occurrence_b = occurrence_a;
    occurrence_b.rte_ordinal = 2;
    REQUIRE_FALSE(same_base_occurrence(occurrence_a, occurrence_b));
    occurrence_b = occurrence_a;
    occurrence_b.xmin_identity = 101;
    REQUIRE_FALSE(same_base_occurrence(occurrence_a, occurrence_b));

    const std::array<DependencyKey, 4> dependencies{{
        {.object_oid = 9},
        {.object_oid = 2},
        {.object_oid = 9},
        {.object_oid = 7},
    }};
    const auto partitions = canonical_dependency_partitions(
        dependencies,
        4,
        [](const DependencyKey& key) { return key.object_oid; });
    REQUIRE(partitions == std::vector<std::uint32_t>{1, 2, 3});

    const std::array<std::uint32_t, 2> required_settings{1, 2};
    const std::array settings{
        SemanticSettingRecord{
            .setting_id = 1,
            .class_mask = static_cast<std::uint8_t>(
                SemanticSettingClass::PlanChoice),
        },
        SemanticSettingRecord{
            .setting_id = 2,
            .class_mask = static_cast<std::uint8_t>(
                SemanticSettingClass::CapturedResultSemantic),
        },
    };
    REQUIRE(semantic_setting_inventory_complete(required_settings, settings));
    const std::array<std::uint32_t, 2> duplicate_required{1, 1};
    REQUIRE_FALSE(semantic_setting_inventory_complete(
        duplicate_required, settings));
}
