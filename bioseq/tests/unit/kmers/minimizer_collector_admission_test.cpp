/**
 * @file minimizer_collector_admission_test.cpp
 * @brief Check  record-type validation and memory limits when minimizer collectors grow.
 */
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.parallelism.memory_admission;
import bioseq.sequences.kmers.highway_minimizer_contract;

namespace {

using bioseq::minimizers::minimap_sketch_data;
using bioseq::minimizers::admitted_sketch_collector;
using bioseq::minimizers::generated_minimizer_physical_record;
using bioseq::minimizers::minimizer_physical_batch_owner;
using bioseq::minimizers::minimizer_physical_batch_view;
using bioseq::minimizers::minimizer_physical_layout_selection;
using bioseq::minimizers::minimizer_representation;

/** Match a minimizer record's layout without being a registered record type. */
struct same_layout_minimizer_impostor {
    using layout_type = minimap_sketch_data::layout_type;
    minimap_sketch_data::storage_type value_ = 0u;
};

template <class p_record>
concept physical_batch_span_source = requires(
    std::span<const p_record> records,
    std::vector<p_record>& owner) {
    { minimizer_physical_batch_view::from_span(records) }
        -> std::same_as<minimizer_physical_batch_view>;
    minimizer_physical_batch_view{records};
    minimizer_physical_batch_view{owner};
};

template <class p_record>
concept physical_batch_owned_host_source = requires(
    const minimizer_physical_layout_selection& layout,
    std::vector<p_record> records,
    bioseq::parallelism::memory_permit& permit) {
    { minimizer_physical_batch_owner::from_owned_host(
          layout, std::move(records), permit) }
        -> std::same_as<minimizer_physical_batch_owner>;
};

template <class p_record>
concept physical_batch_shared_host_recoverable = requires(
    const minimizer_physical_batch_owner& owner) {
    { owner.template shared_host<p_record>() }
        -> std::same_as<std::shared_ptr<const std::vector<p_record>>>;
};

template <class p_record>
[[nodiscard]] consteval bool physical_batch_entry_points_match_registry() {
    constexpr bool registered = generated_minimizer_physical_record<p_record>;
    constexpr bool decoded_view_supported =
        registered &&
        (sizeof(p_record) == 4u || sizeof(p_record) == 8u || sizeof(p_record) == 16u);
    return physical_batch_span_source<p_record> == decoded_view_supported &&
           physical_batch_owned_host_source<p_record> == registered &&
           physical_batch_shared_host_recoverable<p_record> == registered;
}

TEST_CASE("physical batch type authentication follows every generated record coordinate",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    // Record types included in every generated profile.
    STATIC_REQUIRE(generated_minimizer_physical_record<
                   minimizer_representation<15u, 15u>>);
    STATIC_REQUIRE(generated_minimizer_physical_record<
                   minimizer_representation<28u, 15u>>);
    STATIC_REQUIRE(generated_minimizer_physical_record<
                   minimizer_representation<33u, 15u>>);
    STATIC_REQUIRE(generated_minimizer_physical_record<
                   minimizer_representation<63u, 15u>>);
    STATIC_REQUIRE(generated_minimizer_physical_record<
                   minimizer_representation<20u, 20u>>);
    STATIC_REQUIRE(generated_minimizer_physical_record<
                   minimizer_representation<23u, 20u>>);
    STATIC_REQUIRE(generated_minimizer_physical_record<
                   minimizer_representation<63u, 20u>>);
    STATIC_REQUIRE(physical_batch_entry_points_match_registry<
                   minimizer_representation<15u, 15u>>());
    STATIC_REQUIRE(physical_batch_entry_points_match_registry<
                   minimizer_representation<28u, 15u>>());
    STATIC_REQUIRE(physical_batch_entry_points_match_registry<
                   minimizer_representation<33u, 15u>>());
    STATIC_REQUIRE(physical_batch_entry_points_match_registry<
                   minimizer_representation<63u, 15u>>());
    STATIC_REQUIRE(physical_batch_entry_points_match_registry<
                   minimizer_representation<20u, 20u>>());
    STATIC_REQUIRE(physical_batch_entry_points_match_registry<
                   minimizer_representation<23u, 20u>>());
    STATIC_REQUIRE(physical_batch_entry_points_match_registry<
                   minimizer_representation<63u, 20u>>());

    // These extra record types are accepted only when the generated profile
    // includes them.
    STATIC_REQUIRE(physical_batch_entry_points_match_registry<
                   minimizer_representation<7u, 7u>>());
    STATIC_REQUIRE(physical_batch_entry_points_match_registry<
                   minimizer_representation<17u, 7u>>());
    STATIC_REQUIRE(physical_batch_entry_points_match_registry<
                   minimizer_representation<49u, 7u>>());
    STATIC_REQUIRE(physical_batch_entry_points_match_registry<
                   minimizer_representation<63u, 7u>>());
    STATIC_REQUIRE(physical_batch_entry_points_match_registry<
                   minimizer_representation<27u, 27u>>());
    STATIC_REQUIRE(physical_batch_entry_points_match_registry<
                   minimizer_representation<63u, 27u>>());
}

TEST_CASE("same-layout impostors cannot enter or reopen physical record storage",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    STATIC_REQUIRE(std::same_as<same_layout_minimizer_impostor::layout_type,
                                minimap_sketch_data::layout_type>);
    STATIC_REQUIRE(sizeof(same_layout_minimizer_impostor) ==
                   sizeof(minimap_sketch_data));
    STATIC_REQUIRE(alignof(same_layout_minimizer_impostor) ==
                   alignof(minimap_sketch_data));
    STATIC_REQUIRE(std::is_standard_layout_v<same_layout_minimizer_impostor>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<same_layout_minimizer_impostor>);

    STATIC_REQUIRE(generated_minimizer_physical_record<minimap_sketch_data>);
    STATIC_REQUIRE(physical_batch_span_source<minimap_sketch_data>);
    STATIC_REQUIRE(physical_batch_owned_host_source<minimap_sketch_data>);
    STATIC_REQUIRE(physical_batch_shared_host_recoverable<minimap_sketch_data>);

    STATIC_REQUIRE_FALSE(
        generated_minimizer_physical_record<same_layout_minimizer_impostor>);
    STATIC_REQUIRE_FALSE(physical_batch_span_source<same_layout_minimizer_impostor>);
    STATIC_REQUIRE_FALSE(
        physical_batch_owned_host_source<same_layout_minimizer_impostor>);
    STATIC_REQUIRE_FALSE(
        physical_batch_shared_host_recoverable<same_layout_minimizer_impostor>);
}

TEST_CASE("admitted sketch collector reserves every geometric replacement peak",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    using collector_type =
        admitted_sketch_collector<minimap_sketch_data, bioseq::parallelism::memory_permit>;
    constexpr std::uint64_t factor =
        bioseq::parallelism::k_vector_preallocation_headroom_factor;
    constexpr std::uint64_t record_bytes = sizeof(minimap_sketch_data);
    constexpr std::uint64_t first_peak =
        collector_type::k_initial_growth_records * record_bytes * factor;
    constexpr std::uint64_t second_peak =
        collector_type::k_initial_growth_records * record_bytes +
        2u * collector_type::k_initial_growth_records * record_bytes * factor;
    const minimap_sketch_data record{1u, 0u, 0u};

    SECTION("initial growth is denied one byte short") {
        bioseq::parallelism::memory_admission_controller admission;
        admission.set_index_memory_cap_bytes(first_peak - 1u);
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_type collector{std::move(*permit), factor};
            collector.collect(record);
            REQUIRE_FALSE(collector.is_ok());
            REQUIRE(collector.extract_sketches_data().empty());
            REQUIRE(collector.extract_sketches_data().capacity() == 0u);
            REQUIRE(collector.admitted_capacity_bytes() == 0u);
            REQUIRE(admission.reserved_bytes() == 0u);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("initial growth succeeds at its exact peak") {
        bioseq::parallelism::memory_admission_controller admission;
        admission.set_index_memory_cap_bytes(first_peak);
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_type collector{std::move(*permit), factor};
            collector.collect(record);
            REQUIRE(collector.is_ok());
            REQUIRE(collector.extract_sketches_data().size() == 1u);
            REQUIRE(collector.admitted_capacity_bytes()
                == collector.extract_sketches_data().capacity()
                    * record_bytes);
            REQUIRE(admission.reserved_bytes()
                == collector.admitted_capacity_bytes());
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("replacement is denied one byte short without partial output") {
        bioseq::parallelism::memory_admission_controller admission;
        admission.set_index_memory_cap_bytes(second_peak - 1u);
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_type collector{std::move(*permit), factor};
            for (std::size_t index = 0u; index < collector_type::k_initial_growth_records;
                 ++index) {
                collector.collect(record);
            }
            REQUIRE(collector.is_ok());
            collector.collect(record);
            REQUIRE_FALSE(collector.is_ok());
            REQUIRE(collector.extract_sketches_data().size() ==
                    collector_type::k_initial_growth_records);
            REQUIRE(collector.extract_sketches_data().capacity() ==
                    collector_type::k_initial_growth_records);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("replacement succeeds at its exact existing-plus-replacement peak") {
        bioseq::parallelism::memory_admission_controller admission;
        admission.set_index_memory_cap_bytes(second_peak);
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_type collector{std::move(*permit), factor};
            for (std::size_t index = 0u; index <= collector_type::k_initial_growth_records;
                 ++index) {
                collector.collect(record);
            }
            REQUIRE(collector.is_ok());
            REQUIRE(collector.extract_sketches_data().size() ==
                    collector_type::k_initial_growth_records + 1u);
            REQUIRE(collector.admitted_capacity_bytes()
                == collector.extract_sketches_data().capacity()
                    * record_bytes);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }
}

TEST_CASE("admitted Exact SIMD raw append obeys initial and replacement cliffs",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    using collector_type =
        admitted_sketch_collector<minimap_sketch_data, bioseq::parallelism::memory_permit>;
    using storage_type = minimap_sketch_data::storage_type;
    constexpr std::uint64_t factor =
        bioseq::parallelism::k_vector_preallocation_headroom_factor;
    constexpr std::uint64_t record_bytes = sizeof(minimap_sketch_data);
    constexpr std::uint64_t first_peak =
        collector_type::k_initial_growth_records * record_bytes * factor;
    std::array<storage_type, 2u * collector_type::k_initial_growth_records + 1u> raw{};
    for (std::size_t index = 0u; index < raw.size(); ++index) {
        raw[index] = minimap_sketch_data{
            index + 1u, index, index & 1u}.raw_value();
    }

    SECTION("initial allocation is denied without a partial append") {
        bioseq::parallelism::memory_admission_controller admission;
        admission.set_index_memory_cap_bytes(first_peak - 1u);
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_type collector{std::move(*permit), factor};
            REQUIRE_FALSE(collector.append_raw_storage(raw.data(), 1u));
            REQUIRE_FALSE(collector.is_ok());
            REQUIRE(collector.extract_sketches_data().empty());
            REQUIRE(collector.extract_sketches_data().capacity() == 0u);
            REQUIRE(collector.admitted_capacity_bytes() == 0u);
            REQUIRE(admission.reserved_bytes() == 0u);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("initial allocation succeeds at the exact peak") {
        bioseq::parallelism::memory_admission_controller admission;
        admission.set_index_memory_cap_bytes(first_peak);
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_type collector{std::move(*permit), factor};
            REQUIRE(collector.append_raw_storage(raw.data(), 1u));
            REQUIRE(collector.is_ok());
            REQUIRE(collector.extract_sketches_data().size() == 1u);
            REQUIRE(collector.extract_sketches_data().front().raw_value()
                == raw.front());
            REQUIRE(collector.admitted_capacity_bytes()
                == collector.extract_sketches_data().capacity()
                    * record_bytes);
            REQUIRE(admission.reserved_bytes()
                == collector.admitted_capacity_bytes());
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("typed batch append uses the same exact admission peak") {
        bioseq::parallelism::memory_admission_controller admission;
        admission.set_index_memory_cap_bytes(first_peak);
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_type collector{std::move(*permit), factor};
            const std::array<minimap_sketch_data, 1u> records{
                minimap_sketch_data{7u, 3u, 1u}};
            REQUIRE(collector.append_records(records));
            REQUIRE(collector.is_ok());
            REQUIRE(collector.extract_sketches_data().size() == 1u);
            REQUIRE(collector.extract_sketches_data().front().raw_value()
                == records.front().raw_value());
            REQUIRE(collector.admitted_capacity_bytes()
                == collector.extract_sketches_data().capacity()
                    * record_bytes);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    const auto run_replacement = [&raw](const bool exact_peak) {
        bioseq::parallelism::memory_admission_controller admission;
        admission.set_index_memory_cap_bytes(first_peak);
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_type collector{std::move(*permit), factor};
            REQUIRE(collector.append_raw_storage(raw.data(), 1u));
            const std::size_t existing_capacity =
                collector.extract_sketches_data().capacity();
            REQUIRE(existing_capacity <= 2u * collector_type::k_initial_growth_records);
            REQUIRE(collector.append_raw_storage(
                raw.data() + 1u, existing_capacity - 1u));
            REQUIRE(collector.extract_sketches_data().size()
                == existing_capacity);
            const std::uint64_t existing_permit =
                collector.admitted_capacity_bytes();
            const std::uint64_t replacement_headroom =
                2u * existing_capacity * record_bytes * factor;
            admission.set_index_memory_cap_bytes(
                existing_permit + replacement_headroom
                    - (exact_peak ? 0u : 1u));

            const bool appended = collector.append_raw_storage(
                raw.data() + existing_capacity, 1u);
            REQUIRE(appended == exact_peak);
            REQUIRE(collector.is_ok() == exact_peak);
            REQUIRE(collector.extract_sketches_data().size()
                == existing_capacity + (exact_peak ? 1u : 0u));
            if (!exact_peak) {
                REQUIRE(collector.admitted_capacity_bytes() == existing_permit);
                REQUIRE(admission.reserved_bytes() == existing_permit);
            } else {
                REQUIRE(collector.admitted_capacity_bytes()
                    == collector.extract_sketches_data().capacity()
                        * record_bytes);
                REQUIRE(admission.reserved_bytes()
                    == collector.admitted_capacity_bytes());
            }
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    };

    SECTION("replacement is denied one byte short") {
        run_replacement(false);
    }
    SECTION("replacement succeeds at the exact existing-plus-replacement peak") {
        run_replacement(true);
    }
}

TEST_CASE("coarse exact-record batch preserves the sequential geometric admission cliff",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    using collector_type =
        admitted_sketch_collector<minimap_sketch_data, bioseq::parallelism::memory_permit>;
    constexpr std::uint64_t factor =
        bioseq::parallelism::k_vector_preallocation_headroom_factor;
    constexpr std::size_t batch_records = 256u;
    constexpr std::size_t predecessor_records = batch_records / 2u;
    constexpr std::uint64_t record_bytes = sizeof(minimap_sketch_data);
    constexpr std::uint64_t sequential_final_peak =
        predecessor_records * record_bytes
        + batch_records * record_bytes * factor;

    std::array<minimap_sketch_data, batch_records> records;
    for (std::size_t index = 0u; index < records.size(); ++index) {
        records[index] = minimap_sketch_data{
            index + 1u, index, index & 1u};
    }

    const auto run = [&records](const bool exact_peak) {
        bioseq::parallelism::memory_admission_controller admission;
        admission.set_index_memory_cap_bytes(
            sequential_final_peak - (exact_peak ? 0u : 1u));
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_type collector{std::move(*permit), factor};
            const bool appended = collector.append_records(records);
            REQUIRE(appended == exact_peak);
            REQUIRE(collector.is_ok() == exact_peak);
            REQUIRE(collector.extract_sketches_data().size()
                == (exact_peak ? batch_records : 0u));
            if (exact_peak) {
                REQUIRE(collector.extract_sketches_data().capacity()
                    == batch_records);
                REQUIRE(collector.admitted_capacity_bytes()
                    == batch_records * record_bytes);
            } else {
                REQUIRE(collector.extract_sketches_data().capacity() == 0u);
                REQUIRE(collector.admitted_capacity_bytes() == 0u);
            }
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    };

    SECTION("one byte below the sequential final replacement peak rejects") {
        run(false);
    }
    SECTION("the exact sequential final replacement peak succeeds") {
        run(true);
    }
}

TEST_CASE("live admitted collector composes with destination replacement peak",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    using collector_type =
        admitted_sketch_collector<minimap_sketch_data, bioseq::parallelism::memory_permit>;
    constexpr std::uint64_t factor =
        bioseq::parallelism::k_vector_preallocation_headroom_factor;
    constexpr std::uint64_t record_bytes = sizeof(minimap_sketch_data);
    constexpr std::uint64_t collector_initial_peak =
        collector_type::k_initial_growth_records * record_bytes * factor;
    constexpr std::uint64_t initial_destination_headroom =
        record_bytes * factor;
    const minimap_sketch_data collector_record{1u, 0u, 0u};
    const minimap_sketch_data destination_record{2u, 1u, 1u};

    const auto run = [collector_record, destination_record](
        const bool exact_peak) {
        bioseq::parallelism::memory_admission_controller admission;
        admission.set_index_memory_cap_bytes(
            collector_initial_peak + initial_destination_headroom);
        {
            auto collector_owner = admission.try_reserve(0u);
            REQUIRE(collector_owner.has_value());
            collector_type collector{std::move(*collector_owner), factor};
            collector.collect(collector_record);
            REQUIRE(collector.is_ok());
            const std::uint64_t collector_bytes =
                collector.admitted_capacity_bytes();

            auto destination_owner = admission.try_reserve(0u);
            REQUIRE(destination_owner.has_value());
            bioseq::parallelism::memory_permit destination_permit =
                std::move(*destination_owner);
            std::vector<minimap_sketch_data> destination;
            REQUIRE(destination_permit.grow_to(initial_destination_headroom));
            destination.reserve(1u);
            const std::uint64_t initial_destination_bytes =
                destination.capacity() * record_bytes;
            REQUIRE(initial_destination_bytes
                <= initial_destination_headroom);
            destination_permit.shrink_to(initial_destination_bytes);
            destination.resize(destination.capacity(), destination_record);

            const auto& sketches = collector.extract_sketches_data();
            const std::size_t required =
                destination.size() + sketches.size();
            REQUIRE(required > destination.capacity());
            const std::uint64_t replacement_headroom =
                required * record_bytes * factor;
            const std::uint64_t global_peak = collector_bytes
                + initial_destination_bytes + replacement_headroom;
            admission.set_index_memory_cap_bytes(
                exact_peak ? global_peak : global_peak - 1u);

            const std::size_t existing_size = destination.size();
            const std::size_t existing_capacity = destination.capacity();
            const std::uint64_t destination_peak =
                initial_destination_bytes + replacement_headroom;
            const bool admitted = destination_permit.grow_to(destination_peak);
            REQUIRE(admitted == exact_peak);
            if (!admitted) {
                REQUIRE(destination.size() == existing_size);
                REQUIRE(destination.capacity() == existing_capacity);
                REQUIRE(destination_permit.bytes()
                    == initial_destination_bytes);
                REQUIRE(admission.reserved_bytes()
                    == collector_bytes + initial_destination_bytes);
                return;
            }

            destination.reserve(required);
            const std::uint64_t measured_destination_bytes =
                destination.capacity() * record_bytes;
            REQUIRE(measured_destination_bytes <= replacement_headroom);
            destination_permit.shrink_to(measured_destination_bytes);
            destination.insert(
                destination.end(), sketches.begin(), sketches.end());
            REQUIRE(destination.size() == required);
            REQUIRE(admission.reserved_bytes()
                == collector_bytes + measured_destination_bytes);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    };

    SECTION("one byte short denies before destination allocation") {
        run(false);
    }
    SECTION("exact global peak succeeds and reconciles retained capacity") {
        run(true);
    }
}

} // namespace
