/**
 * @file highway_minimizer_sink_test.cpp
 * @brief Check that  minimizer collectors preserve record bytes and reject invalid output buffers.
 */
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.sequences.kmers.highway_minimizer_contract;

namespace {

using bioseq::minimizers::minimap_sketch_data;
using bioseq::minimizers::minimizer_output_layout;
using bioseq::minimizers::minimizer_representation;
using bioseq::minimizers::minimizer_sink;
using bioseq::minimizers::minimizer_sink_batch_view;
using bioseq::minimizers::k_minimizer_output_layout;
using bioseq::minimizers::minimizer_sink_collector;
using bioseq::minimizers::basic_sketch_collector;

enum class batch_view_fault {
    none,
    layout,
    record_size,
    record_alignment,
    data_alignment,
};

template<class p_minimizer_type>
class exact_sink_state {
public:
    static constexpr std::size_t k_batch_capacity =
        minimizer_sink_collector<p_minimizer_type>::k_batch_capacity;
    static constexpr minimizer_output_layout layout =
        k_minimizer_output_layout<p_minimizer_type>;

    static_assert(layout != minimizer_output_layout::unsupported);
    static_assert(std::is_standard_layout_v<p_minimizer_type>);
    static_assert(std::is_trivially_copyable_v<p_minimizer_type>);
    static_assert(std::is_trivially_destructible_v<p_minimizer_type>);

    [[nodiscard]] minimizer_sink sink(const bool enable_raw_u64 = true) noexcept {
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

    std::vector<p_minimizer_type> records{};
    std::size_t acquire_calls = 0u;
    std::size_t commit_calls = 0u;
    std::size_t raw_calls = 0u;
    std::size_t transported_bytes = 0u;
    batch_view_fault fault = batch_view_fault::none;
    bool reject_acquire = false;
    bool throw_on_commit = false;
    bool rejected = false;

private:
    [[nodiscard]] static minimizer_sink_batch_view acquire(
        void* opaque,
        const std::size_t minimum)
    {
        auto& self = *static_cast<exact_sink_state*>(opaque);
        ++self.acquire_calls;
        if (self.reject_acquire || self.pending_
            || minimum > k_batch_capacity) {
            self.rejected = true;
            return {};
        }
        self.pending_ = true;
        minimizer_sink_batch_view view{
            .data = self.storage_.data(),
            .capacity = k_batch_capacity,
            .layout = layout,
            .record_size = sizeof(p_minimizer_type),
            .record_alignment = alignof(p_minimizer_type),
        };
        switch (self.fault) {
            case batch_view_fault::none:
                break;
            case batch_view_fault::layout:
                view.layout = minimizer_output_layout::unsupported;
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
        if (!self.pending_ || initialized > k_batch_capacity) {
            self.rejected = true;
            return;
        }
        auto* const first = std::launder(reinterpret_cast<p_minimizer_type*>(
            self.storage_.data()));
        try {
            if (self.throw_on_commit) {
                throw std::runtime_error{"injected exact sink failure"};
            }
            self.records.insert(
                self.records.end(), first, first + initialized);
            ++self.commit_calls;
            self.transported_bytes += initialized * sizeof(p_minimizer_type);
        } catch (...) {
            self.exception_ = std::current_exception();
            self.rejected = true;
        }
        std::destroy_n(first, initialized);
        self.pending_ = false;
    }

    [[nodiscard]] static bool append_raw_u64(
        void* opaque,
        const minimizer_output_layout supplied_layout,
        const std::uint64_t* raw,
        const std::size_t count) noexcept
    {
        auto& self = *static_cast<exact_sink_state*>(opaque);
        if (self.pending_ || supplied_layout != layout
            || (count != 0u && raw == nullptr)) {
            self.rejected = true;
            return false;
        }
        if constexpr (requires { typename p_minimizer_type::storage_type; }) {
            if constexpr (std::same_as<
                              typename p_minimizer_type::storage_type,
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
                                return p_minimizer_type::from_raw_storage(value);
                            });
                    self.records.insert(
                        self.records.end(), decoded.begin(), decoded.end());
                    ++self.raw_calls;
                    self.transported_bytes += count * sizeof(p_minimizer_type);
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

    alignas(p_minimizer_type) std::array<
        std::byte,
        k_batch_capacity * sizeof(p_minimizer_type)> storage_;
    std::exception_ptr exception_{};
    bool pending_ = false;
};

template<class p_minimizer_type>
[[nodiscard]] std::vector<p_minimizer_type> exact_records(
    const std::size_t count)
{
    std::vector<p_minimizer_type> result;
    result.reserve(count);
    for (std::size_t index = 0u; index < count; ++index) {
        result.emplace_back(
            static_cast<std::uint64_t>(index * 2654435761u + 17u),
            static_cast<std::uint64_t>(index),
            static_cast<std::uint64_t>(index & 1u));
    }
    return result;
}

template<class p_minimizer_type>
[[nodiscard]] constexpr auto object_bytes(
    const p_minimizer_type value) noexcept
{
    return std::bit_cast<std::array<std::byte, sizeof(p_minimizer_type)>>(value);
}

template<class p_minimizer_type>
void require_exact_object_sequence(
    const std::span<const p_minimizer_type> actual,
    const std::span<const p_minimizer_type> expected)
{
    REQUIRE(actual.size() == expected.size());
    for (std::size_t index = 0u; index < expected.size(); ++index) {
        CAPTURE(index);
        REQUIRE(object_bytes(actual[index]) == object_bytes(expected[index]));
    }
}

template<class p_minimizer_type>
void require_native_append_identity() {
    const std::array<p_minimizer_type, 3u> input{
        p_minimizer_type{1u, 2u, 0u},
        p_minimizer_type{3u, 4u, 1u},
        p_minimizer_type{5u, 6u, 0u},
    };
    basic_sketch_collector<p_minimizer_type> collector;
    REQUIRE(collector.append_records(input));
    const auto& actual = collector.extract_sketches_data();
    REQUIRE(actual.size() == input.size());
    for (std::size_t index = 0u; index < input.size(); ++index) {
        REQUIRE(object_bytes(actual[index]) == object_bytes(input[index]));
    }
}

template<class p_minimizer_type>
void require_typed_transport(const std::size_t count) {
    const auto expected = exact_records<p_minimizer_type>(count);
    exact_sink_state<p_minimizer_type> state;
    minimizer_sink_collector<p_minimizer_type> collector{state.sink(false)};
    for (const p_minimizer_type& record : expected) collector.collect(record);
    REQUIRE(collector.flush());
    REQUIRE(collector.is_ok());
    REQUIRE_FALSE(state.rejected);
    require_exact_object_sequence<p_minimizer_type>(state.records, expected);
    REQUIRE(collector.committed_size() == count);
    REQUIRE(state.transported_bytes == count * sizeof(p_minimizer_type));
    REQUIRE(state.acquire_calls
        == (count + exact_sink_state<p_minimizer_type>::k_batch_capacity - 1u)
            / exact_sink_state<p_minimizer_type>::k_batch_capacity);
    REQUIRE(state.commit_calls == state.acquire_calls);
}

TEST_CASE("native minimizer collectors append exact 2/4/8/16-byte objects",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    using repr_u16 = minimizer_representation<2u, 2u>;
    using repr_u32 = minimizer_representation<7u, 7u>;
    using repr_u64 = minimap_sketch_data;
    using repr_u128 = minimizer_representation<63u, 15u>;
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
          "[component:bioseq][layer:unit][feature:minimizer]") {
    for (const std::size_t count :
         std::array<std::size_t, 5u>{0u, 1u, 255u, 256u, 257u}) {
        CAPTURE(count);
        require_typed_transport<minimizer_representation<7u, 7u>>(count);
        require_typed_transport<minimap_sketch_data>(count);
        require_typed_transport<minimizer_representation<63u, 15u>>(count);
    }
}

TEST_CASE("split minimizer transport preserves low/high words and span",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    using repr = minimizer_representation<63u, 15u>;
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
        repr::k_position_field_mask,
        1u,
        0xFFu};
    REQUIRE(maximal.data_low_64()
        == (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE(maximal.data_high_64()
        == (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE(maximal.kmer() == maximal_hash);
    REQUIRE(maximal.last_pos() == repr::k_position_field_mask);
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
          "[component:bioseq][layer:unit][feature:minimizer]") {
    const auto expected = exact_records<minimap_sketch_data>(257u);
    std::vector<std::uint64_t> raw;
    raw.reserve(expected.size());
    for (const minimap_sketch_data& record : expected) {
        raw.push_back(record.raw_value());
    }

    exact_sink_state<minimap_sketch_data> state;
    minimizer_sink_collector<minimap_sketch_data> collector{state.sink(true)};
    REQUIRE(collector.append_raw_storage(raw.data(), raw.size()));
    REQUIRE(collector.flush());
    require_exact_object_sequence<minimap_sketch_data>(state.records, expected);
    REQUIRE(state.acquire_calls == 0u);
    REQUIRE(state.commit_calls == 0u);
    REQUIRE(state.raw_calls == 1u);
    REQUIRE(state.transported_bytes
        == expected.size() * sizeof(minimap_sketch_data));

    exact_sink_state<minimap_sketch_data> typed_fallback;
    minimizer_sink_collector<minimap_sketch_data> fallback_collector{
        typed_fallback.sink(false)};
    REQUIRE(fallback_collector.append_raw_storage(raw.data(), raw.size()));
    REQUIRE(fallback_collector.flush());
    require_exact_object_sequence<minimap_sketch_data>(
        typed_fallback.records, expected);
    REQUIRE(typed_fallback.raw_calls == 0u);
    REQUIRE(typed_fallback.acquire_calls == 2u);
    REQUIRE(typed_fallback.commit_calls == 2u);
    REQUIRE(typed_fallback.transported_bytes
        == expected.size() * sizeof(minimap_sketch_data));
}

TEST_CASE("narrow raw storage never crosses the u64 callback",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    using repr = minimizer_representation<7u, 7u>;
    const auto expected = exact_records<repr>(257u);
    std::vector<repr::storage_type> raw;
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
          "[component:bioseq][layer:unit][feature:minimizer]") {
    for (const batch_view_fault fault : std::array{
             batch_view_fault::layout,
             batch_view_fault::record_size,
             batch_view_fault::record_alignment,
             batch_view_fault::data_alignment}) {
        CAPTURE(fault);
        exact_sink_state<minimap_sketch_data> state;
        state.fault = fault;
        minimizer_sink_collector<minimap_sketch_data> collector{
            state.sink(false)};
        collector.collect(minimap_sketch_data{1u, 2u, 1u});
        REQUIRE_FALSE(collector.is_ok());
        REQUIRE_FALSE(collector.flush());
        REQUIRE(state.records.empty());
        REQUIRE(state.transported_bytes == 0u);
    }

    exact_sink_state<minimap_sketch_data> rejected;
    rejected.reject_acquire = true;
    minimizer_sink_collector<minimap_sketch_data> collector{
        rejected.sink(false)};
    collector.collect(minimap_sketch_data{1u, 2u, 1u});
    REQUIRE_FALSE(collector.is_ok());
    REQUIRE(rejected.records.empty());
}

TEST_CASE("sink commit captures exceptions and exposes count mismatch",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    SECTION("typed commit captures and rethrows outside the noexcept callback") {
        exact_sink_state<minimap_sketch_data> state;
        state.throw_on_commit = true;
        minimizer_sink_collector<minimap_sketch_data> collector{
            state.sink(false)};
        collector.collect(minimap_sketch_data{1u, 2u, 1u});
        REQUIRE(collector.flush());
        REQUIRE(collector.committed_size() == 1u);
        REQUIRE(state.records.empty());
        REQUIRE(state.transported_bytes == 0u);
        REQUIRE(state.rejected);
        REQUIRE_THROWS_AS(state.rethrow_if_failed(), std::runtime_error);
    }

    SECTION("raw commit reports rejection and retains the captured exception") {
        const std::uint64_t raw =
            minimap_sketch_data{1u, 2u, 1u}.raw_value();
        exact_sink_state<minimap_sketch_data> state;
        state.throw_on_commit = true;
        minimizer_sink_collector<minimap_sketch_data> collector{
            state.sink(true)};
        REQUIRE_FALSE(collector.append_raw_storage(&raw, 1u));
        REQUIRE_FALSE(collector.is_ok());
        REQUIRE(collector.committed_size() == 0u);
        REQUIRE(state.records.empty());
        REQUIRE(state.transported_bytes == 0u);
        REQUIRE(state.rejected);
        REQUIRE_THROWS_AS(state.rethrow_if_failed(), std::runtime_error);
    }
}

} // namespace
