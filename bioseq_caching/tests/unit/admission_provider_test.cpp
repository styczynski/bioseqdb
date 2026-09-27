#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

import bioseq.cache.core;

using namespace bioseq::cache;

namespace {

struct test_counter {
    std::uint64_t value = 0;
    bool fail_next_exchange = false;

    [[nodiscard]] std::uint64_t load() const noexcept {
        return value;
    }

    bool compare_exchange(std::uint64_t& expected, std::uint64_t desired) noexcept {
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

struct test_budget {
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

bioseq_cache_status analyze_query(const ::Query*, bioseq_cache_query_intent*) {
    return bioseq_cache_status_ok;
}

bioseq_cache_status describe_path(::PlannerInfo*,
                                  ::RelOptInfo*,
                                  ::Path*,
                                  const bioseq_cache_query_intent*,
                                  bioseq_cache_path_domain*) {
    return bioseq_cache_status_ok;
}

bioseq_cache_status append_domain(const bioseq_cache_path_domain*, bioseq_cache_digest_sink*) {
    return bioseq_cache_status_ok;
}

bioseq_cache_status append_runtime(const ::List*, ::EState*, bioseq_cache_digest_sink*) {
    return bioseq_cache_status_ok;
}

bioseq_cache_status map_source(::PlannerInfo*,
                               const bioseq_cache_path_domain*,
                               const bioseq_cache_reusable_source*,
                               bioseq_cache_source_mapping*) {
    return bioseq_cache_status_ok;
}

bioseq_cache_status decorate_path(::PlannerInfo*,
                                  ::RelOptInfo*,
                                  ::Path*,
                                  const bioseq_cache_path_domain*,
                                  ::List*,
                                  bioseq_cache_decorated_path*) {
    return bioseq_cache_status_ok;
}

bioseq_cache_status build_recipe(::PlannerInfo*, const ::Plan*, bioseq_cache_plan_digest_recipe*) {
    return bioseq_cache_status_ok;
}

bioseq_cache_status extract_occurrence(::List*, ::TupleTableSlot*, bioseq_cache_occurrence*) {
    return bioseq_cache_status_ok;
}

bioseq_cache_status late_fetch(::List*,
                               ::EState*,
                               const bioseq_cache_occurrence*,
                               std::size_t,
                               const std::int16_t*,
                               std::size_t,
                               bioseq_cache_late_fetch_sink*) {
    return bioseq_cache_status_ok;
}

void after_provider_rel_paths(
    ::PlannerInfo*, ::RelOptInfo*, unsigned int, ::RangeTblEntry*, const bioseq_cache_provider*) {}

void after_planned_statement(::PlannedStmt*) {}

bioseq_cache_column_schema column(std::uint32_t attno, std::uint64_t semantic_id = 7) {
    return bioseq_cache_column_schema{
        .struct_size = sizeof(bioseq_cache_column_schema),
        .attno = attno,
        .semantic_id_high = 0,
        .semantic_id_low = semantic_id,
        .type_oid = 23,
        .typmod = -1,
        .collation_oid = 0,
        .flags = 0,
    };
}

bioseq_cache_column_map_entry
map_entry(std::uint32_t consumer_attno, std::uint32_t source_attno, std::uint64_t semantic_id = 7) {
    return bioseq_cache_column_map_entry{
        .struct_size = sizeof(bioseq_cache_column_map_entry),
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
          "[component:cache][layer:unit][feature:admission]") {
    test_counter counter{};
    test_budget budget{.allocatable = 1'000, .total = 2'000};
    basic_cache_admission admission(counter, budget);

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
          "[component:cache][layer:unit][feature:admission]") {
    REQUIRE(conservative_dsa_reservation_bytes(1).value() == 8'192);
    REQUIRE(conservative_dsa_reservation_bytes(4'096).value() == 8'192);
    REQUIRE(conservative_dsa_reservation_bytes(4'097).value() == 12'288);
    REQUIRE(conservative_dsa_reservation_bytes(8'192, 2).value() == 16'384);
    REQUIRE(conservative_dsa_reservation_bytes(0).error() == admission_error::invalid_reservation);
    REQUIRE(
        conservative_dsa_reservation_bytes((std::numeric_limits<std::uint64_t>::max)()).error() ==
        admission_error::counter_overflow);

    const std::array<std::uint64_t, 2> fixed_payloads{1, 4'097};
    REQUIRE(conservative_fixed_dsa_reservation_bytes(65'536, fixed_payloads).value() == 65'536);
    REQUIRE(conservative_fixed_dsa_reservation_bytes(4'096, fixed_payloads).value() == 20'480);
    REQUIRE(conservative_fixed_dsa_reservation_bytes(0, fixed_payloads).error() ==
            admission_error::invalid_reservation);

    test_counter counter{.value = 16'384};
    test_budget budget{.allocatable = 32'768, .total = 131'072};
    basic_cache_admission admission(counter, budget);
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
          "[component:cache][layer:unit][feature:admission]") {
    test_counter counter{};
    test_budget budget{.allocatable = 100, .total = 0, .pressure = true};
    basic_cache_admission admission(counter, budget);
    REQUIRE(admission.try_reserve(1).error() == admission_error::system_pressure);

    budget.pressure = false;
    counter.value = (std::numeric_limits<std::uint64_t>::max)();
    REQUIRE(admission.try_reserve(1).error() == admission_error::counter_overflow);

    constexpr std::uint64_t capacity = 200ULL << 30;
    REQUIRE(file_admission_policy::ceiling_bytes(capacity) == (20ULL << 30));
    REQUIRE(file_admission_policy::free_floor_bytes(capacity) == (20ULL << 30));
    REQUIRE(file_admission_policy::can_reserve(0, 1ULL << 30, capacity, 25ULL << 30));
    REQUIRE_FALSE(file_admission_policy::can_reserve(0, 6ULL << 30, capacity, 25ULL << 30));
    /*
     * Below the production free-space floor, ordinary admission rejects even
     * a small request although the request fits both actual availability and
     * the ten-percent capacity ceiling.  Test-only forced-file storage may
     * waive only that floor; these two limits remain its safety contract.
     */
    constexpr std::uint64_t below_floor_available = 15ULL << 30;
    constexpr std::uint64_t small_request = 1ULL << 20;
    REQUIRE_FALSE(
        file_admission_policy::can_reserve(0, small_request, capacity, below_floor_available));
    REQUIRE(file_admission_policy::can_reserve_with_free_floor(
        0, small_request, capacity, below_floor_available, 0));
    REQUIRE_FALSE(file_admission_policy::can_reserve_with_free_floor(
        0, 16ULL << 30, capacity, below_floor_available, 0));
    REQUIRE_FALSE(file_admission_policy::can_reserve_with_free_floor(
        0, 21ULL << 30, capacity, 100ULL << 30, 0));
}

TEST_CASE("provider schema maps require exact SQL and semantic meaning",
          "[component:cache][layer:unit][feature:admission]") {
    const std::array consumer_columns{column(1)};
    const std::array source_columns{column(1)};
    const std::array<std::uint32_t, 1> consumer_required{1};
    const std::array mappings{map_entry(1, 1)};

    const provider_contract_view consumer{
        .boundary =
            {
                .columns = consumer_columns.data(),
                .column_count = consumer_columns.size(),
                .required_attnos = consumer_required.data(),
                .required_attno_count = consumer_required.size(),
            },
        .capability_mask = bioseq_cap_score | bioseq_cap_cigar,
    };
    const provider_contract_view source{
        .boundary =
            {
                .columns = source_columns.data(),
                .column_count = source_columns.size(),
            },
        .capability_mask = bioseq_cap_score | bioseq_cap_cigar | bioseq_cap_mapq,
    };
    bioseq_cache_source_mapping mapping{
        .struct_size = sizeof(bioseq_cache_source_mapping),
        .flags = 0,
        .boundary_map = mappings.data(),
        .boundary_map_count = mappings.size(),
    };
    REQUIRE(validate_source_map(consumer, source, mapping).has_value());

    auto bad_mapping = mappings;
    bad_mapping[0].collation_oid = 100;
    mapping.boundary_map = bad_mapping.data();
    REQUIRE(validate_source_map(consumer, source, mapping).error() ==
            source_map_error::collation_mismatch);

    mapping.boundary_map = mappings.data();
    mapping.flags = 1;
    REQUIRE(validate_source_map(consumer, source, mapping).error() ==
            source_map_error::invalid_flags);

    auto weak_source = source;
    weak_source.capability_mask = bioseq_cap_score;
    mapping.flags = 0;
    REQUIRE(validate_source_map(consumer, weak_source, mapping).error() ==
            source_map_error::capability_missing);
}

TEST_CASE("provider schemas reject zero, duplicate, and out-of-range attnos",
          "[component:cache][layer:unit][feature:admission]") {
    auto valid = column(1);
    provider_schema_view schema{.columns = &valid, .column_count = 1};
    REQUIRE(validate_provider_schema(schema).has_value());

    auto invalid = valid;
    invalid.attno = 0;
    schema.columns = &invalid;
    REQUIRE(validate_provider_schema(schema).error() == source_map_error::invalid_attno);
    invalid.attno = 2;
    REQUIRE(validate_provider_schema(schema).error() == source_map_error::invalid_attno);

    const std::array duplicate{column(1), column(1)};
    schema = provider_schema_view{
        .columns = duplicate.data(),
        .column_count = duplicate.size(),
    };
    REQUIRE(validate_provider_schema(schema).error() == source_map_error::duplicate_attno);

    const std::array dense{column(1), column(2)};
    auto incompatible_semantics = dense;
    incompatible_semantics[1].semantic_id_low = incompatible_semantics[0].semantic_id_low;
    incompatible_semantics[1].type_oid = 25;
    schema = provider_schema_view{
        .columns = incompatible_semantics.data(),
        .column_count = incompatible_semantics.size(),
    };
    REQUIRE(validate_provider_schema(schema).error() == source_map_error::semantic_id_mismatch);

    const std::array<std::uint32_t, 1> out_of_range{3};
    schema = provider_schema_view{
        .columns = dense.data(),
        .column_count = dense.size(),
        .required_attnos = out_of_range.data(),
        .required_attno_count = out_of_range.size(),
    };
    REQUIRE(validate_provider_schema(schema).error() == source_map_error::invalid_attno);
}

TEST_CASE("repeated source attributes are legal only for identical meaning",
          "[component:cache][layer:unit][feature:admission]") {
    const std::array consumer_columns{column(1), column(2)};
    const std::array source_columns{column(1)};
    const std::array<std::uint32_t, 2> required{1, 2};
    const std::array mappings{map_entry(1, 1), map_entry(2, 1)};
    const provider_schema_view consumer{
        .columns = consumer_columns.data(),
        .column_count = consumer_columns.size(),
        .required_attnos = required.data(),
        .required_attno_count = required.size(),
    };
    const provider_schema_view source{
        .columns = source_columns.data(),
        .column_count = source_columns.size(),
    };
    REQUIRE(validate_map_half(consumer, source, mappings.data(), mappings.size()).has_value());

    auto incompatible_columns = consumer_columns;
    incompatible_columns[1].semantic_id_low = 8;
    const provider_schema_view incompatible{
        .columns = incompatible_columns.data(),
        .column_count = incompatible_columns.size(),
        .required_attnos = required.data(),
        .required_attno_count = required.size(),
    };
    REQUIRE(validate_map_half(incompatible, source, mappings.data(), mappings.size()).error() ==
            source_map_error::semantic_id_mismatch);
}

TEST_CASE("source-map shape and meaning failures are exhaustive",
          "[component:cache][layer:unit][feature:admission]") {
    const std::array consumer_columns{column(1, 7), column(2, 8)};
    const std::array source_columns{column(1, 7), column(2, 8)};
    const std::array<std::uint32_t, 2> required{1, 2};
    const provider_contract_view consumer{
        .boundary =
            {
                .columns = consumer_columns.data(),
                .column_count = consumer_columns.size(),
                .required_attnos = required.data(),
                .required_attno_count = required.size(),
            },
        .capability_mask = bioseq_cap_score,
    };
    const provider_contract_view source{
        .boundary =
            {
                .columns = source_columns.data(),
                .column_count = source_columns.size(),
            },
        .capability_mask = bioseq_cap_score,
    };
    const std::array valid_maps{map_entry(1, 1, 7), map_entry(2, 2, 8)};
    bioseq_cache_source_mapping mapping{
        .struct_size = sizeof(bioseq_cache_source_mapping),
        .boundary_map = valid_maps.data(),
        .boundary_map_count = valid_maps.size(),
    };
    REQUIRE(validate_source_map(consumer, source, mapping).has_value());

    SECTION("null pointer and short or long count") {
        mapping.boundary_map = nullptr;
        REQUIRE(validate_source_map(consumer, source, mapping).error() ==
                source_map_error::null_pointer);
        mapping.boundary_map = valid_maps.data();
        mapping.boundary_map_count = 1;
        REQUIRE(validate_source_map(consumer, source, mapping).error() ==
                source_map_error::map_count_mismatch);
        mapping.boundary_map_count = 3;
        REQUIRE(validate_source_map(consumer, source, mapping).error() ==
                source_map_error::map_count_mismatch);
    }

    SECTION("zero and out-of-range mapping attnos") {
        auto maps = valid_maps;
        maps[0].consumer_attno = 0;
        mapping.boundary_map = maps.data();
        REQUIRE(validate_source_map(consumer, source, mapping).error() ==
                source_map_error::invalid_attno);
        maps = valid_maps;
        maps[0].source_attno = 3;
        REQUIRE(validate_source_map(consumer, source, mapping).error() ==
                source_map_error::invalid_attno);
    }

    SECTION("missing and duplicate consumer attnos") {
        auto maps = valid_maps;
        maps[1].consumer_attno = 1;
        maps[1].semantic_id_low = 7;
        mapping.boundary_map = maps.data();
        REQUIRE(validate_source_map(consumer, source, mapping).error() ==
                source_map_error::duplicate_attno);
        maps = valid_maps;
        maps[1].consumer_attno = 3;
        REQUIRE(validate_source_map(consumer, source, mapping).error() ==
                source_map_error::required_attno_missing);
    }

    SECTION("type typmod collation and semantic metadata") {
        auto maps = valid_maps;
        ++maps[0].type_oid;
        mapping.boundary_map = maps.data();
        REQUIRE(validate_source_map(consumer, source, mapping).error() ==
                source_map_error::type_mismatch);
        maps = valid_maps;
        ++maps[0].typmod;
        REQUIRE(validate_source_map(consumer, source, mapping).error() ==
                source_map_error::typmod_mismatch);
        maps = valid_maps;
        ++maps[0].collation_oid;
        REQUIRE(validate_source_map(consumer, source, mapping).error() ==
                source_map_error::collation_mismatch);
        maps = valid_maps;
        ++maps[0].semantic_id_low;
        REQUIRE(validate_source_map(consumer, source, mapping).error() ==
                source_map_error::semantic_id_mismatch);
    }

    SECTION("same-typed semantic permutation") {
        auto maps = valid_maps;
        maps[0].source_attno = 2;
        maps[1].source_attno = 1;
        mapping.boundary_map = maps.data();
        REQUIRE(validate_source_map(consumer, source, mapping).error() ==
                source_map_error::semantic_id_mismatch);
    }
}

TEST_CASE("provider registration enforces callbacks implied by flags",
          "[component:cache][layer:unit][feature:admission]") {
    bioseq_cache_provider provider{
        .struct_size = sizeof(bioseq_cache_provider),
        .abi_revision = k_provider_abi_revision,
        .postgres_major = 18,
        .provider_id = 9,
        .provider_flags = 0,
        .reserved = 0,
        .provider_name = "unit-provider",
        .provider_scan_methods = reinterpret_cast<const ::CustomScanMethods*>(std::uintptr_t{1}),
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

    provider.provider_flags = bioseq_cache_provider_reusable_coverage;
    provider.late_fetch_batch = nullptr;
    REQUIRE_FALSE(provider_registration_valid(&provider, 18));
    provider.late_fetch_batch = late_fetch;
    REQUIRE(provider_registration_valid(&provider, 18));

    provider.provider_flags =
        bioseq_cache_provider_reusable_coverage | bioseq_cache_provider_complete_write_observation;
    REQUIRE(provider_registration_valid(&provider, 18));

    provider.provider_flags = bioseq_cache_provider_exact_source_elision_safe;
    REQUIRE(provider_registration_valid(&provider, 18));

    provider.provider_flags = 1U << 31;
    REQUIRE_FALSE(provider_registration_valid(&provider, 18));

    provider.provider_flags = 0;
    provider.provider_scan_methods = nullptr;
    REQUIRE_FALSE(provider_registration_valid(&provider, 18));
    provider.provider_scan_methods =
        reinterpret_cast<const ::CustomScanMethods*>(std::uintptr_t{1});
    provider.struct_size = sizeof(bioseq_cache_provider) - 1;
    REQUIRE_FALSE(provider_registration_valid(&provider, 18));
    provider.struct_size = sizeof(bioseq_cache_provider);
    provider.abi_revision = k_provider_abi_revision + 1;
    REQUIRE_FALSE(provider_registration_valid(&provider, 18));
}

TEST_CASE("planner bridge preserves its prefix and finalized Plan tail",
          "[component:cache][layer:unit][feature:admission]") {
    bioseq_cache_planner_bridge bridge{
        .struct_size = static_cast<std::uint32_t>(
            offsetof(bioseq_cache_planner_bridge, after_planned_statement)),
        .abi_revision = k_provider_abi_revision,
        .postgres_major = 18,
        .after_provider_rel_paths = after_provider_rel_paths,
        .after_planned_statement = after_planned_statement,
    };
    REQUIRE(
        provider_abi_field_present(&bridge,
                                   offsetof(bioseq_cache_planner_bridge, after_provider_rel_paths),
                                   bridge.after_provider_rel_paths));
    REQUIRE_FALSE(
        provider_abi_field_present(&bridge,
                                   offsetof(bioseq_cache_planner_bridge, after_planned_statement),
                                   bridge.after_planned_statement));

    bridge.struct_size = sizeof(bioseq_cache_planner_bridge);
    REQUIRE(
        provider_abi_field_present(&bridge,
                                   offsetof(bioseq_cache_planner_bridge, after_planned_statement),
                                   bridge.after_planned_statement));
}

TEST_CASE("canonical keys, identities, dependencies, and settings are exact",
          "[component:cache][layer:unit][feature:admission]") {
    canonical_cache_key first{.bytes = {std::byte{1}, std::byte{2}}};
    canonical_cache_key same = first;
    canonical_cache_key collision = first;
    collision.bytes[1] = std::byte{3};
    REQUIRE(collision_checked_key_equal(first, same));
    REQUIRE_FALSE(collision_checked_key_equal(first, collision));

    physical_identity left{
        .kind = physical_identity_kind::provider_opaque,
        .length = 2,
    };
    left.bytes[0] = std::byte{4};
    left.bytes[1] = std::byte{5};
    physical_identity right = left;
    right.bytes[2] = std::byte{99};
    REQUIRE(physical_identity_equal(left, right));
    right = left;
    right.bytes[1] = std::byte{99};
    REQUIRE_FALSE(physical_identity_equal(left, right));

    physical_identity locator_a{
        .kind = physical_identity_kind::pg_rel_file_locator,
        .length = 12,
    };
    for (std::size_t index = 0; index < locator_a.length; ++index) {
        locator_a.bytes[index] = static_cast<std::byte>(index + 1);
    }
    physical_identity locator_b = locator_a;
    locator_b.bytes[locator_b.length - 1] = std::byte{0xff};
    cache_digest_256 forced_collision_digest{};
    forced_collision_digest.bytes[0] = 0x5a;
    const canonical_cache_key locator_key_a{
        .digest = forced_collision_digest,
        .bytes = std::vector<std::byte>(locator_a.bytes.begin(),
                                        locator_a.bytes.begin() + locator_a.length),
    };
    const canonical_cache_key locator_key_b{
        .digest = forced_collision_digest,
        .bytes = std::vector<std::byte>(locator_b.bytes.begin(),
                                        locator_b.bytes.begin() + locator_b.length),
    };
    REQUIRE(locator_key_a.digest == locator_key_b.digest);
    REQUIRE_FALSE(physical_identity_equal(locator_a, locator_b));
    REQUIRE_FALSE(collision_checked_key_equal(locator_key_a, locator_key_b));

    base_row_reference occurrence_a{
        .rte_ordinal = 1,
        .logical_rel_oid = 10,
        .leaf_rel_oid = 11,
        .physical_identity = left,
        .identity_kind = row_identity_kind::provider_opaque,
        .row_number_or_opaque = 55,
        .xmin_identity = 100,
    };
    auto occurrence_b = occurrence_a;
    occurrence_b.rte_ordinal = 2;
    REQUIRE_FALSE(same_base_occurrence(occurrence_a, occurrence_b));
    occurrence_b = occurrence_a;
    occurrence_b.xmin_identity = 101;
    REQUIRE_FALSE(same_base_occurrence(occurrence_a, occurrence_b));

    const std::array<dependency_key, 4> dependencies{{
        {.object_oid = 9},
        {.object_oid = 2},
        {.object_oid = 9},
        {.object_oid = 7},
    }};
    const auto partitions = canonical_dependency_partitions(
        dependencies, 4, [](const dependency_key& key) { return key.object_oid; });
    REQUIRE(partitions == std::vector<std::uint32_t>{1, 2, 3});

    const std::array<std::uint32_t, 2> required_settings{1, 2};
    const std::array settings{
        semantic_setting_record{
            .setting_id = 1,
            .class_mask = static_cast<std::uint8_t>(semantic_setting_class::plan_choice),
        },
        semantic_setting_record{
            .setting_id = 2,
            .class_mask =
                static_cast<std::uint8_t>(semantic_setting_class::captured_result_semantic),
        },
    };
    REQUIRE(semantic_setting_inventory_complete(required_settings, settings));
    const std::array<std::uint32_t, 2> duplicate_required{1, 1};
    REQUIRE_FALSE(semantic_setting_inventory_complete(duplicate_required, settings));

    auto unknown_class = settings;
    unknown_class[0].class_mask = 0x10;
    REQUIRE_FALSE(semantic_setting_inventory_complete(required_settings, unknown_class));
    const std::array invalid_duplicate{
        settings[0],
        semantic_setting_record{.setting_id = 1, .class_mask = 0x10},
        settings[1],
    };
    REQUIRE_FALSE(semantic_setting_inventory_complete(required_settings, invalid_duplicate));
}

TEST_CASE("provider digest framing and runtime recipe checks are unambiguous",
          "[component:cache][layer:unit][feature:admission]") {
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

    REQUIRE(provider_recipe_shape_valid(k_maximum_provider_recipes_per_exact_plan,
                                        k_maximum_provider_recipe_bytes,
                                        k_maximum_provider_boundary_columns));
    REQUIRE_FALSE(
        provider_recipe_shape_valid(k_maximum_provider_recipes_per_exact_plan + 1u, 0, 0));
    REQUIRE_FALSE(provider_recipe_shape_valid(0, k_maximum_provider_recipe_bytes + 1u, 0));
    REQUIRE_FALSE(provider_recipe_shape_valid(0, 0, k_maximum_provider_boundary_columns + 1u));

    cache_digest_256 plan_a{};
    cache_digest_256 plan_b{};
    plan_b.bytes[0] = 1;
    REQUIRE(runtime_digest_matches(plan_a, plan_a));
    REQUIRE(runtime_digest_matches(plan_b, plan_b));
    REQUIRE_FALSE(runtime_digest_matches(plan_a, plan_b));
}
