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

BioseqCacheStatusV2 analyze_query(
    const ::Query*,
    BioseqCacheQueryIntentV2*) {
    return BIOSEQ_CACHE_STATUS_OK;
}

BioseqCacheStatusV2 describe_path(
    ::PlannerInfo*,
    ::RelOptInfo*,
    ::Path*,
    const BioseqCacheQueryIntentV2*,
    BioseqCachePathDomainV2*) {
    return BIOSEQ_CACHE_STATUS_OK;
}

BioseqCacheStatusV2 append_domain(
    const BioseqCachePathDomainV2*,
    BioseqCacheDigestSinkV2*) {
    return BIOSEQ_CACHE_STATUS_OK;
}

BioseqCacheStatusV2 append_runtime(
    const ::List*,
    ::EState*,
    BioseqCacheDigestSinkV2*) {
    return BIOSEQ_CACHE_STATUS_OK;
}

BioseqCacheStatusV2 map_source(
    ::PlannerInfo*,
    const BioseqCachePathDomainV2*,
    const BioseqCacheReusableSourceV2*,
    BioseqCacheSourceMappingV2*) {
    return BIOSEQ_CACHE_STATUS_OK;
}

BioseqCacheStatusV2 decorate_path(
    ::PlannerInfo*,
    ::RelOptInfo*,
    ::Path*,
    const BioseqCachePathDomainV2*,
    ::List*,
    BioseqCacheDecoratedPathV2*) {
    return BIOSEQ_CACHE_STATUS_OK;
}

BioseqCacheStatusV2 build_recipe(
    ::PlannerInfo*,
    const ::Plan*,
    BioseqCachePlanDigestRecipeV2*) {
    return BIOSEQ_CACHE_STATUS_OK;
}

BioseqCacheStatusV2 extract_occurrence(
    ::List*,
    ::TupleTableSlot*,
    BioseqCacheOccurrenceV2*) {
    return BIOSEQ_CACHE_STATUS_OK;
}

BioseqCacheStatusV2 late_fetch(
    ::List*,
    ::EState*,
    const BioseqCacheOccurrenceV2*,
    std::size_t,
    const std::int16_t*,
    std::size_t,
    BioseqCacheLateFetchSinkV2*) {
    return BIOSEQ_CACHE_STATUS_OK;
}

void after_provider_rel_paths(
    ::PlannerInfo*,
    ::RelOptInfo*,
    unsigned int,
    ::RangeTblEntry*,
    const BioseqCacheProviderV2*) {}

void after_planned_statement(::PlannedStmt*) {}

BioseqCacheColumnSchemaV2 column(
    std::uint32_t attno,
    std::uint64_t semantic_id = 7) {
    return BioseqCacheColumnSchemaV2{
        .struct_size = sizeof(BioseqCacheColumnSchemaV2),
        .attno = attno,
        .semantic_id_high = 0,
        .semantic_id_low = semantic_id,
        .type_oid = 23,
        .typmod = -1,
        .collation_oid = 0,
        .flags = 0,
    };
}

BioseqCacheColumnMapEntryV2 map_entry(
    std::uint32_t consumer_attno,
    std::uint32_t source_attno,
    std::uint64_t semantic_id = 7) {
    return BioseqCacheColumnMapEntryV2{
        .struct_size = sizeof(BioseqCacheColumnMapEntryV2),
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

TEST_CASE("shared admission never exceeds the live 25-percent ceiling",
          "[cache][unit][admission-provider]") {
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

    {
        auto retained = admission.try_reserve(64);
        REQUIRE(retained.has_value());
        REQUIRE(retained->disown() == 64);
    }
    REQUIRE(admission.reserved_bytes() == 64);
    counter.subtract(64);
    REQUIRE(admission.reserved_bytes() == 0);
}

TEST_CASE("fixed and variable DSA accounting share one exact ceiling",
          "[cache][unit][admission-provider]") {
    REQUIRE(conservative_dsa_reservation_bytes(1).value() == 8'192);
    REQUIRE(conservative_dsa_reservation_bytes(4'096).value() == 8'192);
    REQUIRE(conservative_dsa_reservation_bytes(4'097).value() == 12'288);
    REQUIRE(
        conservative_dsa_reservation_bytes(8'192, 2).value() == 16'384);
    REQUIRE(
        conservative_dsa_reservation_bytes(0).error()
        == AdmissionError::InvalidReservation);
    REQUIRE(
        conservative_dsa_reservation_bytes(
            (std::numeric_limits<std::uint64_t>::max)())
            .error()
        == AdmissionError::CounterOverflow);

    const std::array<std::uint64_t, 2> fixed_payloads{1, 4'097};
    REQUIRE(
        conservative_fixed_dsa_reservation_bytes(
            65'536, fixed_payloads).value()
        == 65'536);
    REQUIRE(
        conservative_fixed_dsa_reservation_bytes(
            4'096, fixed_payloads).value()
        == 20'480);
    REQUIRE(
        conservative_fixed_dsa_reservation_bytes(
            0, fixed_payloads).error()
        == AdmissionError::InvalidReservation);

    TestCounter counter{.value = 16'384};
    TestBudget budget{.allocatable = 32'768, .total = 131'072};
    BasicCacheAdmission admission(counter, budget);
    const auto variable = conservative_dsa_reservation_bytes(9'000);
    REQUIRE(variable.has_value());
    REQUIRE(*variable == 16'384);
    auto permit = admission.try_reserve(*variable);
    REQUIRE(permit.has_value());
    REQUIRE(admission.reserved_bytes() == 32'768);
    REQUIRE_FALSE(admission.try_reserve(1).has_value());
    permit->release();
    REQUIRE(admission.reserved_bytes() == 16'384);
}

TEST_CASE("admission reports pressure, overflow, and file floors",
          "[cache][unit][admission-provider]") {
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
    /*
     * Below the production free-space floor, ordinary admission rejects even
     * a small request although the request fits both actual availability and
     * the ten-percent capacity ceiling.  Test-only forced-file storage may
     * waive only that floor; these two limits remain its safety contract.
     */
    constexpr std::uint64_t below_floor_available = 15ULL << 30;
    constexpr std::uint64_t small_request = 1ULL << 20;
    REQUIRE_FALSE(FileAdmissionPolicy::can_reserve(
        0, small_request, capacity, below_floor_available));
    REQUIRE(FileAdmissionPolicy::can_reserve_with_free_floor(
        0, small_request, capacity, below_floor_available, 0));
    REQUIRE_FALSE(FileAdmissionPolicy::can_reserve_with_free_floor(
        0, 16ULL << 30, capacity, below_floor_available, 0));
    REQUIRE_FALSE(FileAdmissionPolicy::can_reserve_with_free_floor(
        0, 21ULL << 30, capacity, 100ULL << 30, 0));
}

TEST_CASE("provider schema maps require exact SQL and semantic meaning",
          "[cache][unit][admission-provider]") {
    const std::array consumer_columns{column(1)};
    const std::array source_columns{column(1)};
    const std::array<std::uint32_t, 1> consumer_required{1};
    const std::array mappings{map_entry(1, 1)};

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
    BioseqCacheSourceMappingV2 mapping{
        .struct_size = sizeof(BioseqCacheSourceMappingV2),
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

TEST_CASE("provider schemas reject zero, duplicate, and out-of-range attnos",
          "[cache][unit][admission-provider]") {
    auto valid = column(1);
    ProviderSchemaView schema{.columns = &valid, .column_count = 1};
    REQUIRE(validate_provider_schema(schema).has_value());

    auto invalid = valid;
    invalid.attno = 0;
    schema.columns = &invalid;
    REQUIRE(
        validate_provider_schema(schema).error()
        == SourceMapError::InvalidAttno);
    invalid.attno = 2;
    REQUIRE(
        validate_provider_schema(schema).error()
        == SourceMapError::InvalidAttno);

    const std::array duplicate{column(1), column(1)};
    schema = ProviderSchemaView{
        .columns = duplicate.data(),
        .column_count = duplicate.size(),
    };
    REQUIRE(
        validate_provider_schema(schema).error()
        == SourceMapError::DuplicateAttno);

    const std::array dense{column(1), column(2)};
    auto incompatible_semantics = dense;
    incompatible_semantics[1].semantic_id_low =
        incompatible_semantics[0].semantic_id_low;
    incompatible_semantics[1].type_oid = 25;
    schema = ProviderSchemaView{
        .columns = incompatible_semantics.data(),
        .column_count = incompatible_semantics.size(),
    };
    REQUIRE(
        validate_provider_schema(schema).error()
        == SourceMapError::SemanticIdMismatch);

    const std::array<std::uint32_t, 1> out_of_range{3};
    schema = ProviderSchemaView{
        .columns = dense.data(),
        .column_count = dense.size(),
        .required_attnos = out_of_range.data(),
        .required_attno_count = out_of_range.size(),
    };
    REQUIRE(
        validate_provider_schema(schema).error()
        == SourceMapError::InvalidAttno);
}

TEST_CASE("repeated source attributes are legal only for identical meaning",
          "[cache][unit][admission-provider]") {
    const std::array consumer_columns{column(1), column(2)};
    const std::array source_columns{column(1)};
    const std::array<std::uint32_t, 2> required{1, 2};
    const std::array mappings{map_entry(1, 1), map_entry(2, 1)};
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

TEST_CASE("source-map shape and meaning failures are exhaustive",
          "[cache][unit][admission-provider]") {
    const std::array consumer_columns{column(1, 7), column(2, 8)};
    const std::array source_columns{column(1, 7), column(2, 8)};
    const std::array<std::uint32_t, 2> required{1, 2};
    const ProviderContractView consumer{
        .boundary = {
            .columns = consumer_columns.data(),
            .column_count = consumer_columns.size(),
            .required_attnos = required.data(),
            .required_attno_count = required.size(),
        },
        .capability_mask = BIOSEQ_CAP_SCORE,
    };
    const ProviderContractView source{
        .boundary = {
            .columns = source_columns.data(),
            .column_count = source_columns.size(),
        },
        .capability_mask = BIOSEQ_CAP_SCORE,
    };
    const std::array valid_maps{map_entry(1, 1, 7), map_entry(2, 2, 8)};
    BioseqCacheSourceMappingV2 mapping{
        .struct_size = sizeof(BioseqCacheSourceMappingV2),
        .boundary_map = valid_maps.data(),
        .boundary_map_count = valid_maps.size(),
    };
    REQUIRE(validate_source_map(consumer, source, mapping).has_value());

    SECTION("null pointer and short or long count") {
        mapping.boundary_map = nullptr;
        REQUIRE(
            validate_source_map(consumer, source, mapping).error()
            == SourceMapError::NullPointer);
        mapping.boundary_map = valid_maps.data();
        mapping.boundary_map_count = 1;
        REQUIRE(
            validate_source_map(consumer, source, mapping).error()
            == SourceMapError::MapCountMismatch);
        mapping.boundary_map_count = 3;
        REQUIRE(
            validate_source_map(consumer, source, mapping).error()
            == SourceMapError::MapCountMismatch);
    }

    SECTION("zero and out-of-range mapping attnos") {
        auto maps = valid_maps;
        maps[0].consumer_attno = 0;
        mapping.boundary_map = maps.data();
        REQUIRE(
            validate_source_map(consumer, source, mapping).error()
            == SourceMapError::InvalidAttno);
        maps = valid_maps;
        maps[0].source_attno = 3;
        REQUIRE(
            validate_source_map(consumer, source, mapping).error()
            == SourceMapError::InvalidAttno);
    }

    SECTION("missing and duplicate consumer attnos") {
        auto maps = valid_maps;
        maps[1].consumer_attno = 1;
        maps[1].semantic_id_low = 7;
        mapping.boundary_map = maps.data();
        REQUIRE(
            validate_source_map(consumer, source, mapping).error()
            == SourceMapError::DuplicateAttno);
        maps = valid_maps;
        maps[1].consumer_attno = 3;
        REQUIRE(
            validate_source_map(consumer, source, mapping).error()
            == SourceMapError::RequiredAttnoMissing);
    }

    SECTION("type typmod collation and semantic metadata") {
        auto maps = valid_maps;
        ++maps[0].type_oid;
        mapping.boundary_map = maps.data();
        REQUIRE(
            validate_source_map(consumer, source, mapping).error()
            == SourceMapError::TypeMismatch);
        maps = valid_maps;
        ++maps[0].typmod;
        REQUIRE(
            validate_source_map(consumer, source, mapping).error()
            == SourceMapError::TypmodMismatch);
        maps = valid_maps;
        ++maps[0].collation_oid;
        REQUIRE(
            validate_source_map(consumer, source, mapping).error()
            == SourceMapError::CollationMismatch);
        maps = valid_maps;
        ++maps[0].semantic_id_low;
        REQUIRE(
            validate_source_map(consumer, source, mapping).error()
            == SourceMapError::SemanticIdMismatch);
    }

    SECTION("same-typed semantic permutation") {
        auto maps = valid_maps;
        maps[0].source_attno = 2;
        maps[1].source_attno = 1;
        mapping.boundary_map = maps.data();
        REQUIRE(
            validate_source_map(consumer, source, mapping).error()
            == SourceMapError::SemanticIdMismatch);
    }
}

TEST_CASE("provider registration enforces callbacks implied by flags",
          "[cache][unit][admission-provider]") {
    BioseqCacheProviderV2 provider{
        .struct_size = sizeof(BioseqCacheProviderV2),
        .abi_version = kProviderAbiVersion,
        .postgres_major = 18,
        .provider_id = 9,
        .provider_flags = 0,
        .reserved = 0,
        .provider_name = "unit-provider",
        .provider_scan_methods =
            reinterpret_cast<const ::CustomScanMethods*>(std::uintptr_t{1}),
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

    provider.provider_flags = BIOSEQ_CACHE_PROVIDER_REUSABLE_COVERAGE
        | BIOSEQ_CACHE_PROVIDER_COMPLETE_WRITE_OBSERVATION;
    REQUIRE(provider_registration_valid(&provider, 18));

    provider.provider_flags = BIOSEQ_CACHE_PROVIDER_EXACT_SOURCE_ELISION_SAFE;
    REQUIRE(provider_registration_valid(&provider, 18));

    provider.provider_flags = 1U << 31;
    REQUIRE_FALSE(provider_registration_valid(&provider, 18));

    provider.provider_flags = 0;
    provider.provider_scan_methods = nullptr;
    REQUIRE_FALSE(provider_registration_valid(&provider, 18));
    provider.provider_scan_methods =
        reinterpret_cast<const ::CustomScanMethods*>(std::uintptr_t{1});
    provider.struct_size = sizeof(BioseqCacheProviderV2) - 1;
    REQUIRE_FALSE(provider_registration_valid(&provider, 18));
    provider.struct_size = sizeof(BioseqCacheProviderV2);
    provider.abi_version = kProviderAbiVersion + 1;
    REQUIRE_FALSE(provider_registration_valid(&provider, 18));
}

TEST_CASE("planner bridge preserves its prefix and finalized Plan tail",
          "[cache][unit][admission-provider]") {
    BioseqCachePlannerBridgeV2 bridge{
        .struct_size = static_cast<std::uint32_t>(
            offsetof(
                BioseqCachePlannerBridgeV2,
                after_planned_statement)),
        .abi_version = kProviderAbiVersion,
        .postgres_major = 18,
        .after_provider_rel_paths = after_provider_rel_paths,
        .after_planned_statement = after_planned_statement,
    };
    REQUIRE(provider_abi_field_present(
        &bridge,
        offsetof(
            BioseqCachePlannerBridgeV2,
            after_provider_rel_paths),
        bridge.after_provider_rel_paths));
    REQUIRE_FALSE(provider_abi_field_present(
        &bridge,
        offsetof(
            BioseqCachePlannerBridgeV2,
            after_planned_statement),
        bridge.after_planned_statement));

    bridge.struct_size = sizeof(BioseqCachePlannerBridgeV2);
    REQUIRE(provider_abi_field_present(
        &bridge,
        offsetof(
            BioseqCachePlannerBridgeV2,
            after_planned_statement),
        bridge.after_planned_statement));
}

TEST_CASE("canonical keys, identities, dependencies, and settings are exact",
          "[cache][unit][admission-provider]") {
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
    right = left;
    right.bytes[1] = std::byte{99};
    REQUIRE_FALSE(physical_identity_equal(left, right));

    PhysicalIdentityV1 locator_a{
        .kind = PhysicalIdentityKind::PgRelFileLocator,
        .length = 12,
    };
    for (std::size_t index = 0; index < locator_a.length; ++index) {
        locator_a.bytes[index] = static_cast<std::byte>(index + 1);
    }
    PhysicalIdentityV1 locator_b = locator_a;
    locator_b.bytes[locator_b.length - 1] = std::byte{0xff};
    CacheDigest256 forced_collision_digest{};
    forced_collision_digest.bytes[0] = 0x5a;
    const CanonicalCacheKey locator_key_a{
        .digest = forced_collision_digest,
        .bytes = std::vector<std::byte>(
            locator_a.bytes.begin(),
            locator_a.bytes.begin() + locator_a.length),
    };
    const CanonicalCacheKey locator_key_b{
        .digest = forced_collision_digest,
        .bytes = std::vector<std::byte>(
            locator_b.bytes.begin(),
            locator_b.bytes.begin() + locator_b.length),
    };
    REQUIRE(locator_key_a.digest == locator_key_b.digest);
    REQUIRE_FALSE(physical_identity_equal(locator_a, locator_b));
    REQUIRE_FALSE(collision_checked_key_equal(
        locator_key_a, locator_key_b));

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

    auto unknown_class = settings;
    unknown_class[0].class_mask = 0x10;
    REQUIRE_FALSE(semantic_setting_inventory_complete(
        required_settings, unknown_class));
    const std::array invalid_duplicate{
        settings[0],
        SemanticSettingRecord{.setting_id = 1, .class_mask = 0x10},
        settings[1],
    };
    REQUIRE_FALSE(semantic_setting_inventory_complete(
        required_settings, invalid_duplicate));
}

TEST_CASE("provider digest framing and runtime recipe checks are unambiguous",
          "[cache][unit][admission-provider]") {
    const std::array payload_a{std::byte{2}, std::byte{3}};
    const std::array payload_b{std::byte{3}};
    std::vector<std::byte> framed_a;
    std::vector<std::byte> framed_b;
    append_provider_digest_frame(framed_a, 1, payload_a);
    append_provider_digest_frame(framed_b, 12, payload_b);
    REQUIRE(framed_a != framed_b);

    std::vector<std::byte> preorder_a;
    append_provider_digest_frame(preorder_a, 1, payload_a);
    append_provider_digest_frame(preorder_a, 2, payload_b);
    std::vector<std::byte> preorder_b;
    append_provider_digest_frame(preorder_b, 2, payload_b);
    append_provider_digest_frame(preorder_b, 1, payload_a);
    REQUIRE(preorder_a != preorder_b);

    REQUIRE(provider_recipe_shape_valid(
        kMaxProviderRecipesPerExactPlan,
        kMaxProviderRecipeBytes,
        kMaxProviderBoundaryColumns));
    REQUIRE_FALSE(provider_recipe_shape_valid(
        kMaxProviderRecipesPerExactPlan + 1u, 0, 0));
    REQUIRE_FALSE(provider_recipe_shape_valid(
        0, kMaxProviderRecipeBytes + 1u, 0));
    REQUIRE_FALSE(provider_recipe_shape_valid(
        0, 0, kMaxProviderBoundaryColumns + 1u));

    CacheDigest256 plan_a{};
    CacheDigest256 plan_b{};
    plan_b.bytes[0] = 1;
    REQUIRE(runtime_digest_matches(plan_a, plan_a));
    REQUIRE(runtime_digest_matches(plan_b, plan_b));
    REQUIRE_FALSE(runtime_digest_matches(plan_a, plan_b));
}
