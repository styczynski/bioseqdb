#include "common_tests.h"

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

import bioseq.sequences.kmers.highway_contract;

namespace {

using bioseq::minimizers::MinimapSketchData;
using bioseq::minimizers::MinimizerOutputLayout;
using bioseq::minimizers::MinimizerRepr;
using bioseq::minimizers::MinimizerSink;
using bioseq::minimizers::MinimizerSinkBatchView;
using bioseq::minimizers::minimizer_output_layout_v;
using bioseq::minimizers::minimizer_sink_collector;
using bioseq::minimizers::sketch_collector_t;

enum class batch_view_fault {
    none,
    layout,
    record_size,
    record_alignment,
    data_alignment,
};

template<class MinimizerT>
class exact_sink_state {
public:
    static constexpr std::size_t batch_capacity =
        minimizer_sink_collector<MinimizerT>::batch_capacity;
    static constexpr MinimizerOutputLayout layout =
        minimizer_output_layout_v<MinimizerT>;

    static_assert(layout != MinimizerOutputLayout::unsupported);
    static_assert(std::is_standard_layout_v<MinimizerT>);
    static_assert(std::is_trivially_copyable_v<MinimizerT>);
    static_assert(std::is_trivially_destructible_v<MinimizerT>);

    [[nodiscard]] MinimizerSink sink(const bool enable_raw_u64 = true) noexcept {
        return {
            .state = this,
            .acquire = &acquire,
            .commit = &commit,
            .append_raw_u64 = enable_raw_u64 ? &append_raw_u64 : nullptr,
        };
    }

    void rethrow_if_failed() const {
        if (exception_ != nullptr) std::rethrow_exception(exception_);
    }

    std::vector<MinimizerT> records{};
    std::size_t acquire_calls = 0u;
    std::size_t commit_calls = 0u;
    std::size_t raw_calls = 0u;
    std::size_t transported_bytes = 0u;
    batch_view_fault fault = batch_view_fault::none;
    bool reject_acquire = false;
    bool throw_on_commit = false;
    bool rejected = false;

private:
    [[nodiscard]] static MinimizerSinkBatchView acquire(
        void* opaque,
        const std::size_t minimum)
    {
        auto& self = *static_cast<exact_sink_state*>(opaque);
        ++self.acquire_calls;
        if (self.reject_acquire || self.pending_
            || minimum > batch_capacity) {
            self.rejected = true;
            return {};
        }
        self.pending_ = true;
        MinimizerSinkBatchView view{
            .data = self.storage_.data(),
            .capacity = batch_capacity,
            .layout = layout,
            .record_size = sizeof(MinimizerT),
            .record_alignment = alignof(MinimizerT),
        };
        switch (self.fault) {
            case batch_view_fault::none:
                break;
            case batch_view_fault::layout:
                view.layout = MinimizerOutputLayout::unsupported;
                break;
            case batch_view_fault::record_size:
                ++view.record_size;
                break;
            case batch_view_fault::record_alignment:
                ++view.record_alignment;
                break;
            case batch_view_fault::data_alignment:
                view.data = self.storage_.data() + 1u;
                break;
        }
        return view;
    }

    static void commit(void* opaque, const std::size_t initialized) noexcept {
        auto& self = *static_cast<exact_sink_state*>(opaque);
        if (!self.pending_ || initialized > batch_capacity) {
            self.rejected = true;
            return;
        }
        auto* const first = std::launder(reinterpret_cast<MinimizerT*>(
            self.storage_.data()));
        try {
            if (self.throw_on_commit) {
                throw std::runtime_error{"injected exact sink failure"};
            }
            self.records.insert(
                self.records.end(), first, first + initialized);
            ++self.commit_calls;
            self.transported_bytes += initialized * sizeof(MinimizerT);
        } catch (...) {
            self.exception_ = std::current_exception();
            self.rejected = true;
        }
        std::destroy_n(first, initialized);
        self.pending_ = false;
    }

    [[nodiscard]] static bool append_raw_u64(
        void* opaque,
        const MinimizerOutputLayout supplied_layout,
        const std::uint64_t* raw,
        const std::size_t count) noexcept
    {
        auto& self = *static_cast<exact_sink_state*>(opaque);
        if (self.pending_ || supplied_layout != layout
            || (count != 0u && raw == nullptr)) {
            self.rejected = true;
            return false;
        }
        if constexpr (requires { typename MinimizerT::storage_t; }) {
            if constexpr (std::same_as<
                              typename MinimizerT::storage_t,
                              std::uint64_t>) {
                try {
                    if (self.throw_on_commit) {
                        throw std::runtime_error{
                            "injected exact raw sink failure"};
                    }
                    const auto decoded =
                        std::span<const std::uint64_t>{raw, count}
                        | std::views::transform(
                            [](const std::uint64_t value) {
                                return MinimizerT::from_raw_storage(value);
                            });
                    self.records.insert(
                        self.records.end(), decoded.begin(), decoded.end());
                    ++self.raw_calls;
                    self.transported_bytes += count * sizeof(MinimizerT);
                    return true;
                } catch (...) {
                    self.exception_ = std::current_exception();
                    self.rejected = true;
                    return false;
                }
            } else {
                self.rejected = true;
                return false;
            }
        } else {
            self.rejected = true;
            return false;
        }
    }

    alignas(MinimizerT) std::array<
        std::byte,
        batch_capacity * sizeof(MinimizerT)> storage_;
    std::exception_ptr exception_{};
    bool pending_ = false;
};

template<class MinimizerT>
[[nodiscard]] std::vector<MinimizerT> exact_records(
    const std::size_t count)
{
    std::vector<MinimizerT> result;
    result.reserve(count);
    for (std::size_t index = 0u; index < count; ++index) {
        result.emplace_back(
            static_cast<std::uint64_t>(index * 2654435761u + 17u),
            static_cast<std::uint64_t>(index),
            static_cast<std::uint64_t>(index & 1u));
    }
    return result;
}

template<class MinimizerT>
[[nodiscard]] constexpr auto object_bytes(
    const MinimizerT value) noexcept
{
    return std::bit_cast<std::array<std::byte, sizeof(MinimizerT)>>(value);
}

template<class MinimizerT>
void require_exact_object_sequence(
    const std::span<const MinimizerT> actual,
    const std::span<const MinimizerT> expected)
{
    REQUIRE(actual.size() == expected.size());
    for (std::size_t index = 0u; index < expected.size(); ++index) {
        CAPTURE(index);
        REQUIRE(object_bytes(actual[index]) == object_bytes(expected[index]));
    }
}

template<class MinimizerT>
void require_native_append_identity() {
    const std::array<MinimizerT, 3u> input{
        MinimizerT{1u, 2u, 0u},
        MinimizerT{3u, 4u, 1u},
        MinimizerT{5u, 6u, 0u},
    };
    sketch_collector_t<MinimizerT> collector;
    REQUIRE(collector.append_records(input));
    const auto& actual = collector.extract_sketches_data();
    REQUIRE(actual.size() == input.size());
    for (std::size_t index = 0u; index < input.size(); ++index) {
        REQUIRE(object_bytes(actual[index]) == object_bytes(input[index]));
    }
}

template<class MinimizerT>
void require_typed_transport(const std::size_t count) {
    const auto expected = exact_records<MinimizerT>(count);
    exact_sink_state<MinimizerT> state;
    minimizer_sink_collector<MinimizerT> collector{state.sink(false)};
    for (const MinimizerT& record : expected) collector.collect(record);
    REQUIRE(collector.flush());
    REQUIRE(collector.ok());
    REQUIRE_FALSE(state.rejected);
    require_exact_object_sequence<MinimizerT>(state.records, expected);
    REQUIRE(collector.committed_size() == count);
    REQUIRE(state.transported_bytes == count * sizeof(MinimizerT));
    REQUIRE(state.acquire_calls
        == (count + exact_sink_state<MinimizerT>::batch_capacity - 1u)
            / exact_sink_state<MinimizerT>::batch_capacity);
    REQUIRE(state.commit_calls == state.acquire_calls);
}

TEST_CASE("native minimizer collectors append exact 2/4/8/16-byte objects",
          "[minimizers][sink][representation][native-width]") {
    using repr_u16 = MinimizerRepr<2u, 2u>;
    using repr_u32 = MinimizerRepr<7u, 7u>;
    using repr_u64 = MinimapSketchData;
    using repr_u128 = MinimizerRepr<63u, 15u>;
    STATIC_REQUIRE(sizeof(repr_u16) == 2u);
    STATIC_REQUIRE(sizeof(repr_u32) == 4u);
    STATIC_REQUIRE(sizeof(repr_u64) == 8u);
    STATIC_REQUIRE(sizeof(repr_u128) == 16u);

    require_native_append_identity<repr_u16>();
    require_native_append_identity<repr_u32>();
    require_native_append_identity<repr_u64>();
    require_native_append_identity<repr_u128>();
}

TEST_CASE("generated sink transport is exact at batch boundaries",
          "[minimizers][sink][representation][batch]") {
    for (const std::size_t count :
         std::array<std::size_t, 5u>{0u, 1u, 255u, 256u, 257u}) {
        CAPTURE(count);
        require_typed_transport<MinimizerRepr<7u, 7u>>(count);
        require_typed_transport<MinimapSketchData>(count);
        require_typed_transport<MinimizerRepr<63u, 15u>>(count);
    }
}

TEST_CASE("split minimizer transport preserves low/high words and span",
          "[minimizers][sink][representation][u128][span]") {
    using repr = MinimizerRepr<63u, 15u>;
    constexpr std::uint64_t hash = 0x00123456789ABCDEull;
    constexpr std::uint64_t position = 0x123456789ABCDEFull;
    constexpr std::uint64_t span = 37u;
    const repr expected{hash, position, 1u, span};
    REQUIRE(expected.data_low_64() == (position << 1u | 1u));
    REQUIRE(expected.data_high_64() == (hash << 8u | span));
    REQUIRE(expected.kmer() == hash);
    REQUIRE(expected.last_pos() == position);
    REQUIRE(expected.strand() == 1u);
    REQUIRE(expected.span() == span);

    constexpr std::uint64_t maximal_hash = 0x00FFFFFFFFFFFFFFull;
    const repr maximal{
        maximal_hash,
        repr::pos_field_mask,
        1u,
        0xFFu};
    REQUIRE(maximal.data_low_64()
        == (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE(maximal.data_high_64()
        == (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE(maximal.kmer() == maximal_hash);
    REQUIRE(maximal.last_pos() == repr::pos_field_mask);
    REQUIRE(maximal.strand() == 1u);
    REQUIRE(maximal.span() == 0xFFu);

    exact_sink_state<repr> state;
    minimizer_sink_collector<repr> collector{state.sink(false)};
    collector.collect(expected);
    collector.collect(maximal);
    REQUIRE(collector.flush());
    const std::array<repr, 2u> expected_records{expected, maximal};
    require_exact_object_sequence<repr>(state.records, expected_records);
    REQUIRE(state.transported_bytes == 2u * sizeof(repr));
}

TEST_CASE("genuine u64 raw append preserves exact object bits without staging",
          "[minimizers][sink][representation][raw-u64]") {
    const auto expected = exact_records<MinimapSketchData>(257u);
    std::vector<std::uint64_t> raw;
    raw.reserve(expected.size());
    for (const MinimapSketchData& record : expected) {
        raw.push_back(record.raw_value());
    }

    exact_sink_state<MinimapSketchData> state;
    minimizer_sink_collector<MinimapSketchData> collector{state.sink(true)};
    REQUIRE(collector.append_raw_storage(raw.data(), raw.size()));
    REQUIRE(collector.flush());
    require_exact_object_sequence<MinimapSketchData>(state.records, expected);
    REQUIRE(state.acquire_calls == 0u);
    REQUIRE(state.commit_calls == 0u);
    REQUIRE(state.raw_calls == 1u);
    REQUIRE(state.transported_bytes
        == expected.size() * sizeof(MinimapSketchData));

    exact_sink_state<MinimapSketchData> typed_fallback;
    minimizer_sink_collector<MinimapSketchData> fallback_collector{
        typed_fallback.sink(false)};
    REQUIRE(fallback_collector.append_raw_storage(raw.data(), raw.size()));
    REQUIRE(fallback_collector.flush());
    require_exact_object_sequence<MinimapSketchData>(
        typed_fallback.records, expected);
    REQUIRE(typed_fallback.raw_calls == 0u);
    REQUIRE(typed_fallback.acquire_calls == 2u);
    REQUIRE(typed_fallback.commit_calls == 2u);
    REQUIRE(typed_fallback.transported_bytes
        == expected.size() * sizeof(MinimapSketchData));
}

TEST_CASE("narrow raw storage never crosses the u64 callback",
          "[minimizers][sink][representation][raw-u32]") {
    using repr = MinimizerRepr<7u, 7u>;
    const auto expected = exact_records<repr>(257u);
    std::vector<repr::storage_t> raw;
    raw.reserve(expected.size());
    for (const repr& record : expected) raw.push_back(record.raw_value());

    exact_sink_state<repr> state;
    minimizer_sink_collector<repr> collector{state.sink(true)};
    REQUIRE(collector.append_raw_storage(raw.data(), raw.size()));
    REQUIRE(collector.flush());
    require_exact_object_sequence<repr>(state.records, expected);
    REQUIRE(state.raw_calls == 0u);
    REQUIRE(state.acquire_calls == 2u);
    REQUIRE(state.commit_calls == 2u);
    REQUIRE(state.transported_bytes == expected.size() * sizeof(repr));
}

TEST_CASE("generated sink rejects layout, size, alignment, and admission faults",
          "[minimizers][sink][representation][rejection]") {
    for (const batch_view_fault fault : std::array{
             batch_view_fault::layout,
             batch_view_fault::record_size,
             batch_view_fault::record_alignment,
             batch_view_fault::data_alignment}) {
        CAPTURE(fault);
        exact_sink_state<MinimapSketchData> state;
        state.fault = fault;
        minimizer_sink_collector<MinimapSketchData> collector{
            state.sink(false)};
        collector.collect(MinimapSketchData{1u, 2u, 1u});
        REQUIRE_FALSE(collector.ok());
        REQUIRE_FALSE(collector.flush());
        REQUIRE(state.records.empty());
        REQUIRE(state.transported_bytes == 0u);
    }

    exact_sink_state<MinimapSketchData> rejected;
    rejected.reject_acquire = true;
    minimizer_sink_collector<MinimapSketchData> collector{
        rejected.sink(false)};
    collector.collect(MinimapSketchData{1u, 2u, 1u});
    REQUIRE_FALSE(collector.ok());
    REQUIRE(rejected.records.empty());
}

TEST_CASE("sink commit captures exceptions and exposes count mismatch",
          "[minimizers][sink][representation][exception]") {
    SECTION("typed commit captures and rethrows outside the noexcept callback") {
        exact_sink_state<MinimapSketchData> state;
        state.throw_on_commit = true;
        minimizer_sink_collector<MinimapSketchData> collector{
            state.sink(false)};
        collector.collect(MinimapSketchData{1u, 2u, 1u});
        REQUIRE(collector.flush());
        REQUIRE(collector.committed_size() == 1u);
        REQUIRE(state.records.empty());
        REQUIRE(state.transported_bytes == 0u);
        REQUIRE(state.rejected);
        REQUIRE_THROWS_AS(state.rethrow_if_failed(), std::runtime_error);
    }

    SECTION("raw commit reports rejection and retains the captured exception") {
        const std::uint64_t raw =
            MinimapSketchData{1u, 2u, 1u}.raw_value();
        exact_sink_state<MinimapSketchData> state;
        state.throw_on_commit = true;
        minimizer_sink_collector<MinimapSketchData> collector{
            state.sink(true)};
        REQUIRE_FALSE(collector.append_raw_storage(&raw, 1u));
        REQUIRE_FALSE(collector.ok());
        REQUIRE(collector.committed_size() == 0u);
        REQUIRE(state.records.empty());
        REQUIRE(state.transported_bytes == 0u);
        REQUIRE(state.rejected);
        REQUIRE_THROWS_AS(state.rethrow_if_failed(), std::runtime_error);
    }
}

} // namespace
