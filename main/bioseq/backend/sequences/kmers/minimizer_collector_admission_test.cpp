#include "common_tests.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

import bioseq.parallelism.memory_admission;
import bioseq.sequences.kmers.highway_contract;

namespace {

using bioseq::minimizers::MinimapSketchData;
using bioseq::minimizers::admitted_sketch_collector_t;

TEST_CASE("admitted sketch collector reserves every geometric replacement peak",
          "[two_stack][collector][admission]") {
    using collector_t = admitted_sketch_collector_t<
        MinimapSketchData, bioseq::parallelism::MemoryPermit>;
    constexpr std::uint64_t factor =
        bioseq::parallelism::kVectorPreallocationHeadroomFactor;
    constexpr std::uint64_t record_bytes = sizeof(MinimapSketchData);
    constexpr std::uint64_t first_peak =
        collector_t::initial_growth_records * record_bytes * factor;
    constexpr std::uint64_t second_peak =
        collector_t::initial_growth_records * record_bytes
        + 2u * collector_t::initial_growth_records * record_bytes * factor;
    const MinimapSketchData record{1u, 0u, 0u};

    SECTION("initial growth is denied one byte short") {
        bioseq::parallelism::MemoryAdmissionController admission;
        admission.set_index_memory_cap_bytes(first_peak - 1u);
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_t collector{std::move(*permit), factor};
            collector.collect(record);
            REQUIRE_FALSE(collector.ok());
            REQUIRE(collector.extract_sketches_data().empty());
            REQUIRE(collector.extract_sketches_data().capacity() == 0u);
            REQUIRE(collector.admitted_capacity_bytes() == 0u);
            REQUIRE(admission.reserved_bytes() == 0u);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("initial growth succeeds at its exact peak") {
        bioseq::parallelism::MemoryAdmissionController admission;
        admission.set_index_memory_cap_bytes(first_peak);
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_t collector{std::move(*permit), factor};
            collector.collect(record);
            REQUIRE(collector.ok());
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
        bioseq::parallelism::MemoryAdmissionController admission;
        admission.set_index_memory_cap_bytes(second_peak - 1u);
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_t collector{std::move(*permit), factor};
            for (std::size_t index = 0u;
                 index < collector_t::initial_growth_records;
                 ++index) {
                collector.collect(record);
            }
            REQUIRE(collector.ok());
            collector.collect(record);
            REQUIRE_FALSE(collector.ok());
            REQUIRE(collector.extract_sketches_data().size()
                == collector_t::initial_growth_records);
            REQUIRE(collector.extract_sketches_data().capacity()
                == collector_t::initial_growth_records);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("replacement succeeds at its exact old-plus-new peak") {
        bioseq::parallelism::MemoryAdmissionController admission;
        admission.set_index_memory_cap_bytes(second_peak);
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_t collector{std::move(*permit), factor};
            for (std::size_t index = 0u;
                 index <= collector_t::initial_growth_records;
                 ++index) {
                collector.collect(record);
            }
            REQUIRE(collector.ok());
            REQUIRE(collector.extract_sketches_data().size()
                == collector_t::initial_growth_records + 1u);
            REQUIRE(collector.admitted_capacity_bytes()
                == collector.extract_sketches_data().capacity()
                    * record_bytes);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }
}

TEST_CASE("admitted Exact SIMD raw append obeys initial and replacement cliffs",
          "[two_stack][exact_simd][collector][admission][raw_append]") {
    using collector_t = admitted_sketch_collector_t<
        MinimapSketchData, bioseq::parallelism::MemoryPermit>;
    using storage_t = MinimapSketchData::storage_t;
    constexpr std::uint64_t factor =
        bioseq::parallelism::kVectorPreallocationHeadroomFactor;
    constexpr std::uint64_t record_bytes = sizeof(MinimapSketchData);
    constexpr std::uint64_t first_peak =
        collector_t::initial_growth_records * record_bytes * factor;
    std::array<storage_t, 2u * collector_t::initial_growth_records + 1u> raw{};
    for (std::size_t index = 0u; index < raw.size(); ++index) {
        raw[index] = MinimapSketchData{
            index + 1u, index, index & 1u}.raw_value();
    }

    SECTION("initial allocation is denied without a partial append") {
        bioseq::parallelism::MemoryAdmissionController admission;
        admission.set_index_memory_cap_bytes(first_peak - 1u);
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_t collector{std::move(*permit), factor};
            REQUIRE_FALSE(collector.append_raw_storage(raw.data(), 1u));
            REQUIRE_FALSE(collector.ok());
            REQUIRE(collector.extract_sketches_data().empty());
            REQUIRE(collector.extract_sketches_data().capacity() == 0u);
            REQUIRE(collector.admitted_capacity_bytes() == 0u);
            REQUIRE(admission.reserved_bytes() == 0u);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("initial allocation succeeds at the exact peak") {
        bioseq::parallelism::MemoryAdmissionController admission;
        admission.set_index_memory_cap_bytes(first_peak);
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_t collector{std::move(*permit), factor};
            REQUIRE(collector.append_raw_storage(raw.data(), 1u));
            REQUIRE(collector.ok());
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
        bioseq::parallelism::MemoryAdmissionController admission;
        admission.set_index_memory_cap_bytes(first_peak);
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_t collector{std::move(*permit), factor};
            const std::array<MinimapSketchData, 1u> records{
                MinimapSketchData{7u, 3u, 1u}};
            REQUIRE(collector.append_records(records));
            REQUIRE(collector.ok());
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
        bioseq::parallelism::MemoryAdmissionController admission;
        admission.set_index_memory_cap_bytes(first_peak);
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_t collector{std::move(*permit), factor};
            REQUIRE(collector.append_raw_storage(raw.data(), 1u));
            const std::size_t old_capacity =
                collector.extract_sketches_data().capacity();
            REQUIRE(old_capacity
                <= 2u * collector_t::initial_growth_records);
            REQUIRE(collector.append_raw_storage(
                raw.data() + 1u, old_capacity - 1u));
            REQUIRE(collector.extract_sketches_data().size()
                == old_capacity);
            const std::uint64_t old_permit =
                collector.admitted_capacity_bytes();
            const std::uint64_t replacement_headroom =
                2u * old_capacity * record_bytes * factor;
            admission.set_index_memory_cap_bytes(
                old_permit + replacement_headroom
                    - (exact_peak ? 0u : 1u));

            const bool appended = collector.append_raw_storage(
                raw.data() + old_capacity, 1u);
            REQUIRE(appended == exact_peak);
            REQUIRE(collector.ok() == exact_peak);
            REQUIRE(collector.extract_sketches_data().size()
                == old_capacity + (exact_peak ? 1u : 0u));
            if (!exact_peak) {
                REQUIRE(collector.admitted_capacity_bytes() == old_permit);
                REQUIRE(admission.reserved_bytes() == old_permit);
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
    SECTION("replacement succeeds at the exact old-plus-new peak") {
        run_replacement(true);
    }
}

TEST_CASE("coarse exact-M batch preserves the sequential geometric admission cliff",
          "[two_stack][collector][admission][batch][parity]") {
    using collector_t = admitted_sketch_collector_t<
        MinimapSketchData, bioseq::parallelism::MemoryPermit>;
    constexpr std::uint64_t factor =
        bioseq::parallelism::kVectorPreallocationHeadroomFactor;
    constexpr std::size_t batch_records = 256u;
    constexpr std::size_t predecessor_records = batch_records / 2u;
    constexpr std::uint64_t record_bytes = sizeof(MinimapSketchData);
    constexpr std::uint64_t sequential_final_peak =
        predecessor_records * record_bytes
        + batch_records * record_bytes * factor;

    std::array<MinimapSketchData, batch_records> records;
    for (std::size_t index = 0u; index < records.size(); ++index) {
        records[index] = MinimapSketchData{
            index + 1u, index, index & 1u};
    }

    const auto run = [&records](const bool exact_peak) {
        bioseq::parallelism::MemoryAdmissionController admission;
        admission.set_index_memory_cap_bytes(
            sequential_final_peak - (exact_peak ? 0u : 1u));
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_t collector{std::move(*permit), factor};
            const bool appended = collector.append_records(records);
            REQUIRE(appended == exact_peak);
            REQUIRE(collector.ok() == exact_peak);
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
          "[two_stack][collector][admission][destination]") {
    using collector_t = admitted_sketch_collector_t<
        MinimapSketchData, bioseq::parallelism::MemoryPermit>;
    constexpr std::uint64_t factor =
        bioseq::parallelism::kVectorPreallocationHeadroomFactor;
    constexpr std::uint64_t record_bytes = sizeof(MinimapSketchData);
    constexpr std::uint64_t collector_initial_peak =
        collector_t::initial_growth_records * record_bytes * factor;
    constexpr std::uint64_t initial_destination_headroom =
        record_bytes * factor;
    const MinimapSketchData collector_record{1u, 0u, 0u};
    const MinimapSketchData destination_record{2u, 1u, 1u};

    const auto run = [collector_record, destination_record](
        const bool exact_peak) {
        bioseq::parallelism::MemoryAdmissionController admission;
        admission.set_index_memory_cap_bytes(
            collector_initial_peak + initial_destination_headroom);
        {
            auto collector_owner = admission.try_reserve(0u);
            REQUIRE(collector_owner.has_value());
            collector_t collector{std::move(*collector_owner), factor};
            collector.collect(collector_record);
            REQUIRE(collector.ok());
            const std::uint64_t collector_bytes =
                collector.admitted_capacity_bytes();

            auto destination_owner = admission.try_reserve(0u);
            REQUIRE(destination_owner.has_value());
            bioseq::parallelism::MemoryPermit destination_permit =
                std::move(*destination_owner);
            std::vector<MinimapSketchData> destination;
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

            const std::size_t old_size = destination.size();
            const std::size_t old_capacity = destination.capacity();
            const std::uint64_t destination_peak =
                initial_destination_bytes + replacement_headroom;
            const bool admitted = destination_permit.grow_to(destination_peak);
            REQUIRE(admitted == exact_peak);
            if (!admitted) {
                REQUIRE(destination.size() == old_size);
                REQUIRE(destination.capacity() == old_capacity);
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
