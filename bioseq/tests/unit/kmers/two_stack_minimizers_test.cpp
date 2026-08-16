/**
 * @file two_stack_minimizers_test.cpp
 * @brief Biological and dispatch checks for two-stack minimizer kernels.
 */
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.common.seqan_adapter;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.sequences.kmers.two_stack_minimizers;

namespace {

using bioseq::minimizers::minimap_sketch_data;
using bioseq::minimizers::minimizer_output_layout;
using bioseq::simd::bounded_nthash_execution_stats;
using bioseq::simd::two_stack_continuation_status;
using bioseq::simd::exact_simd_minimizer_representation;
using bioseq::simd::k_bounded_nthash_scheme_id;
using bioseq::simd::two_stack_continuation_status_name;

struct unsupported_exact_representation {};

static_assert(exact_simd_minimizer_representation<minimap_sketch_data>);
static_assert(
    !exact_simd_minimizer_representation<unsupported_exact_representation>);
static_assert(
    two_stack_continuation_status_name(two_stack_continuation_status::unsupported_vector_shape)
    == std::string_view{"unsupported_vector_shape"});

using bioseq::minimizers::minimizer_kernel_algorithm;
using bioseq::minimizers::minimizer_kernel_invocation;
using bioseq::minimizers::minimizer_kernel_status;
using bioseq::minimizers::minimizer_sink;
using bioseq::minimizers::bounded_nthash_status;
using bioseq::minimizers::packed_sequence_kernel_view;
using bioseq::minimizers::two_stack_route;

struct minimizer_record {
    std::uint64_t hash = 0u;
    std::uint64_t last_position = 0u;
    std::uint64_t strand = 0u;

    [[nodiscard]] constexpr auto operator<=>(
        const minimizer_record&) const noexcept = default;
};

struct collecting_minimizer_sink {
    static constexpr std::size_t k_batch_capacity = 256u;

    std::vector<minimap_sketch_data> records;
    alignas(minimap_sketch_data) std::array<
        std::byte,
        k_batch_capacity * sizeof(minimap_sketch_data)> storage;
    bool pending = false;
    std::size_t raw_batch_calls = 0u;
    bool protocol_ok = true;

    [[nodiscard]] static bioseq::minimizers::minimizer_sink_batch_view acquire(
        void* opaque,
        std::size_t minimum)
    {
        auto& self = *static_cast<collecting_minimizer_sink*>(opaque);
        if (self.pending || minimum > k_batch_capacity) {
            self.protocol_ok = false;
            return {};
        }
        self.pending = true;
        return {
            .data = self.storage.data(),
            .capacity = k_batch_capacity,
            .layout = bioseq::minimizers::k_minimizer_output_layout<
                minimap_sketch_data>,
            .record_size = sizeof(minimap_sketch_data),
            .record_alignment = alignof(minimap_sketch_data),
        };
    }

    static void commit(void* opaque, std::size_t initialized) noexcept
    {
        auto& self = *static_cast<collecting_minimizer_sink*>(opaque);
        if (!self.pending || initialized > k_batch_capacity) {
            self.protocol_ok = false;
            return;
        }
        auto* const first = std::launder(
            reinterpret_cast<minimap_sketch_data*>(self.storage.data()));
        try {
            self.records.insert(
                self.records.end(), first, first + initialized);
        } catch (...) {
            self.protocol_ok = false;
        }
        std::destroy_n(first, initialized);
        self.pending = false;
    }

    [[nodiscard]] static bool append_raw_u64(
            void* opaque,
            minimizer_output_layout layout,
            const std::uint64_t* values,
            std::size_t count) noexcept {
        auto& self = *static_cast<collecting_minimizer_sink*>(opaque);
        if (self.pending
            || (count != 0u && values == nullptr)) {
            self.protocol_ok = false;
            return false;
        }
        try {
            if (layout != bioseq::minimizers::k_minimizer_output_layout<
                    minimap_sketch_data>) {
                self.protocol_ok = false;
                return false;
            }
            const auto sketches = std::span<const std::uint64_t>{values, count}
                | std::views::transform([](const std::uint64_t value) {
                    return minimap_sketch_data::from_raw_storage(value);
                });
            self.records.insert(
                self.records.end(), sketches.begin(), sketches.end());
            ++self.raw_batch_calls;
            return true;
        } catch (...) {
            self.protocol_ok = false;
            return false;
        }
    }

    [[nodiscard]] minimizer_sink contract() noexcept
    {
        return {
            .state = this,
            .acquire = &collecting_minimizer_sink::acquire,
            .commit = &collecting_minimizer_sink::commit,
            .append_raw_u64 = &collecting_minimizer_sink::append_raw_u64,
        };
    }
};

[[nodiscard]] const ::bioseq::simd_dispatch::highway_kernel_table&
fixed_highway_table()
{
    return ::bioseq::simd_dispatch::highway_test_kernel_table();
}

template<sequence_alphabet p_alphabet>
[[nodiscard]] std::uint8_t fixed_alphabet_code()
{
    const auto& table = fixed_highway_table();
    const auto* identity = table.is_complete()
        ? table.template resolve_alphabet_identity<p_alphabet>() : nullptr;
    if (identity == nullptr
        || table.resolve_alphabet_identity(identity->alphabet_code)
            != identity) {
        throw std::logic_error{
            "generated fixed-target table lacks the requested alphabet"};
    }
    return identity->alphabet_code;
}

template<class p_range>
[[nodiscard]] packed_sequence_kernel_view fixed_packed_view(const p_range& range)
{
    using traits = typename p_range::traits;
    return {
        .storage = reinterpret_cast<const std::byte*>(
            range.read_cptr(range.cbegin())),
        .storage_units = static_cast<std::size_t>(range.size()),
        .begin_symbol = 0u,
        .end_symbol = static_cast<std::size_t>(range.bp_size()),
        .alphabet_code = fixed_alphabet_code<
            typename traits::alphabet_type>(),
        .bits_per_symbol = static_cast<std::uint8_t>(
            traits::bits_per_char),
        .storage_unit_bytes = static_cast<std::uint8_t>(
            sizeof(typename traits::storage_unit)),
        .final_segment = true,
    };
}

template<class p_records>
[[nodiscard]] std::vector<minimizer_record> decoded_records(
    const p_records& records)
{
    std::vector<minimizer_record> decoded;
    decoded.reserve(records.size());
    for (const auto& record : records) {
        decoded.push_back({
            .hash = record.kmer(),
            .last_position = record.last_pos(),
            .strand = record.strand(),
        });
    }
    return decoded;
}

[[nodiscard]] std::vector<seq::dna4> deterministic_dna4(
    std::size_t size)
{
    std::vector<seq::dna4> result(size);
    std::uint32_t state = 0x9E3779B9u;
    for (seq::dna4& symbol : result) {
        state = state * 1664525u + 1013904223u;
        symbol.assign_rank(static_cast<std::uint8_t>((state >> 29) & 3u));
    }
    return result;
}

template<class p_range>
[[nodiscard]] std::vector<minimizer_record> scalar_exact_oracle(
    p_range& range,
    std::size_t window,
    std::size_t kmer)
{
    ::bioseq::simd::sketch_collector collector;
    ::bioseq::simd::collect_minimizers_view<
        ::bioseq::simd::minimap_int_hasher,
        minimap_sketch_data>(
            collector,
            range,
            range.begin(),
            range.end(),
            window,
            kmer);
    return decoded_records(collector.extract_sketches_data());
}

template<class p_storage_unit>
void require_fixed_exact_parity(
    const std::vector<seq::dna4>& source,
    std::size_t begin_bp,
    std::size_t end_bp)
{
    CAPTURE(sizeof(p_storage_unit), begin_bp, end_bp, source.size());
    REQUIRE(begin_bp < end_bp);
    REQUIRE(end_bp <= source.size());

    std::vector<seq::dna4> slice(
        source.begin() + static_cast<std::ptrdiff_t>(begin_bp),
        source.begin() + static_cast<std::ptrdiff_t>(end_bp));
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, p_storage_unit>;
    compact_sequence_range<sequence_type> range(slice);
    const auto expected = scalar_exact_oracle(range, 5u, 7u);

    collecting_minimizer_sink sink;
    minimizer_kernel_invocation invocation{
        .sequence = fixed_packed_view(range),
        .fragments = {},
        .window = 5u,
        .kmer = 7u,
        .emitted_position_base = 0u,
        .output_layout =
            bioseq::minimizers::k_minimizer_output_layout<minimap_sketch_data>,
        .two_stack_route = two_stack_route::chronological_continuation,
        .sink = sink.contract(),
    };
    const auto& binding =
        ::bioseq::simd_dispatch::highway_test_target_binding();
    const auto kernel = fixed_highway_table().minimizers.two_stack_nthash;
    REQUIRE(kernel != nullptr);
    const auto result = kernel(invocation);
    const auto actual = decoded_records(sink.records);

    REQUIRE(result.status == minimizer_kernel_status::ok);
    REQUIRE(result.stats.algorithm
        == minimizer_kernel_algorithm::two_stack_nthash);
    REQUIRE(result.stats.target == binding.target);
    REQUIRE(sink.protocol_ok);
    REQUIRE(result.emitted == actual.size());
    REQUIRE(actual == expected);

    const auto& stats = result.stats.two_stack;
    REQUIRE(stats.is_internally_consistent(true));
    REQUIRE(stats.bp_consumed == slice.size());
    REQUIRE(stats.minimizers_emitted == actual.size());
    REQUIRE(stats.vector_batches > 0u);
    REQUIRE(stats.packed_decode_lanes == stats.bp_consumed);
    REQUIRE(stats.scalar_backend_branches == 0u);
    REQUIRE(stats.scalar_tail_symbols == 0u);
    REQUIRE(stats.scalar_hash_symbols == 0u);
    REQUIRE(stats.scalar_window_transitions == 0u);
    REQUIRE(stats.scalar_event_records == 0u);
    REQUIRE(stats.scalable_storage_fallback_calls == 0u);
    REQUIRE(sink.raw_batch_calls > 0u);

    auto forged_scalable_fallback = stats;
    forged_scalable_fallback.scalable_storage_fallback_calls = 1u;
    REQUIRE_FALSE(forged_scalable_fallback.is_internally_consistent(true));
}

template<class p_storage_unit>
void require_fixed_exact_width(
    const std::vector<seq::dna4>& source,
    std::size_t bounded_end_unit)
{
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, p_storage_unit>;
    using range_type = compact_sequence_range<sequence_type>;
    constexpr std::size_t letters_per_unit =
        range_type::traits::letters_per_unit;

    require_fixed_exact_parity<p_storage_unit>(
        source,
        letters_per_unit,
        std::min(source.size(), bounded_end_unit * letters_per_unit));
    require_fixed_exact_parity<p_storage_unit>(source, 0u, source.size());
}

void require_fixed_resident_vector_stream_parity(
    std::vector<seq::dna4> source)
{
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    compact_sequence_range<sequence_type> range(source);

    collecting_minimizer_sink sink;
    minimizer_kernel_invocation invocation{
        .sequence = fixed_packed_view(range),
        .fragments = {},
        .window = 10u,
        .kmer = 15u,
        .emitted_position_base = 0u,
        .output_layout =
            bioseq::minimizers::k_minimizer_output_layout<minimap_sketch_data>,
        .two_stack_route = two_stack_route::resident_vector_stream,
        .sink = sink.contract(),
    };
    const auto& binding =
        ::bioseq::simd_dispatch::highway_test_target_binding();
    const auto kernel = fixed_highway_table().minimizers.two_stack_nthash;
    REQUIRE(kernel != nullptr);
    const auto result = kernel(invocation);
    auto actual = decoded_records(sink.records);
    const std::size_t raw_actual_size = actual.size();

    REQUIRE(result.status == minimizer_kernel_status::ok);
    REQUIRE(result.stats.algorithm
        == minimizer_kernel_algorithm::two_stack_nthash);
    REQUIRE(result.stats.target == binding.target);
    REQUIRE(sink.protocol_ok);
    REQUIRE(result.emitted == raw_actual_size);
    std::ranges::sort(actual);
    actual.erase(
        std::ranges::unique(actual).begin(), actual.end());

    // Source-derived resident-vector-stream contract from commit
    // 847df443500008b453250ebedfa0d802b5dade88, source blob
    // e9e433d0d58c1e71eef2d550bada0c4bd1b92e56. Raw overlap multiplicity
    // follows target geometry, but this normalized oracle is identical for
    // 2/4/8/16 u32 lanes.
    std::uint64_t normalized_fingerprint = 1469598103934665603ULL;
    for (const minimizer_record& record : actual) {
        const auto raw = minimap_sketch_data{
            record.hash, record.last_position, record.strand}.raw_value();
        normalized_fingerprint =
            (normalized_fingerprint ^ raw) * 1099511628211ULL;
    }
    REQUIRE(actual.size() == 754u);
    REQUIRE(normalized_fingerprint == 0x0f4a56dc592b279dULL);

    const auto& stats = result.stats.two_stack;
    REQUIRE(stats.is_internally_consistent(true));
    REQUIRE(stats.resident_vector_stream_kernel_calls == 1u);
    REQUIRE(stats.resident_vector_stream_single_calls == 0u);
    REQUIRE(stats.resident_vector_stream_dual_calls == 1u);
    REQUIRE(stats.resident_vector_stream_nonempty_work_calls == 1u);
    REQUIRE(stats.resident_vector_stream_input_bp == source.size());
    REQUIRE(stats.resident_vector_stream_minimizers_emitted
        == raw_actual_size);
    REQUIRE(stats.generic_u64_kernel_calls == 0u);
    REQUIRE(stats.kmer_15_window_10_byte_8_uint32_hash_kernel_calls == 0u);
    REQUIRE(stats.scalar_backend_branches == 0u);
    REQUIRE(stats.scalar_hash_symbols == 0u);
    REQUIRE(stats.scalable_storage_fallback_calls == 0u);
}

inline constexpr std::array<std::uint32_t, 4u>
    bounded_nthash_oracle_forward_table{
        0x95c60474u,
        0x62a02b4cu,
        0x82572324u,
        0x4be24456u,
    };

struct bounded_nthash_hash_and_strand {
    std::uint32_t hash = 0u;
    std::uint32_t strand = 0u;
};

template<std::size_t p_kmer_size>
[[nodiscard]] bounded_nthash_hash_and_strand bounded_nthash_oracle_hash(
    std::span<const std::uint32_t> ranks,
    std::size_t start)
{
    std::uint32_t forward = 0u;
    std::uint32_t reverse = 0u;
    for (std::size_t offset = 0u; offset < p_kmer_size; ++offset) {
        const std::uint32_t rank = ranks[start + offset];
        REQUIRE(rank < bounded_nthash_oracle_forward_table.size());
        forward ^= std::rotl(
            bounded_nthash_oracle_forward_table[rank],
            static_cast<int>(((p_kmer_size - 1u - offset) * 7u) & 31u));
        reverse ^= std::rotl(
            bounded_nthash_oracle_forward_table[3u - rank],
            static_cast<int>((offset * 7u) & 31u));
    }
    return {
        .hash = static_cast<std::uint32_t>(forward + reverse),
        .strand = reverse < forward ? 1u : 0u,
    };
}

template<std::size_t p_kmer_size, std::size_t p_window_size>
[[nodiscard]] std::vector<minimizer_record> bounded_nthash_scalar_oracle(
    std::span<const std::uint32_t> ranks)
{
    REQUIRE(ranks.size() >= p_kmer_size + p_window_size - 1u);
    const std::size_t kmer_count = ranks.size() - p_kmer_size + 1u;
    const std::size_t window_count = kmer_count - p_window_size + 1u;
    std::vector<bounded_nthash_hash_and_strand> kmers(kmer_count);
    for (std::size_t position = 0u; position < kmer_count; ++position) {
        kmers[position] = bounded_nthash_oracle_hash<p_kmer_size>(ranks, position);
    }

    std::vector<minimizer_record> records;
    records.reserve(window_count * 2u);
    for (std::size_t window = 0u; window < window_count; ++window) {
        std::uint32_t minimum_hash =
            (std::numeric_limits<std::uint32_t>::max)();
        std::size_t left_position = window;
        std::size_t right_position = window;
        for (std::size_t offset = 0u; offset < p_window_size; ++offset) {
            const std::size_t position = window + offset;
            if (kmers[position].hash < minimum_hash) {
                minimum_hash = kmers[position].hash;
                left_position = position;
                right_position = position;
            } else if (kmers[position].hash == minimum_hash) {
                right_position = position;
            }
        }
        const auto append = [&minimum_hash, &kmers, &records](std::size_t position) {
            const minimizer_record record{
                .hash = minimum_hash,
                .last_position = position + p_kmer_size - 1u,
                .strand = kmers[position].strand,
            };
            if (records.empty() || records.back() != record) {
                records.push_back(record);
            }
        };
        append(left_position);
        if (right_position != left_position) append(right_position);
    }
    return records;
}

template<class p_range>
[[nodiscard]] std::vector<std::uint32_t> compact_ranks(p_range& range)
{
    std::vector<std::uint32_t> ranks;
    ranks.reserve(range.bp_size());
    for (const auto rank : range.template iterable<logical_rank>()) {
        ranks.push_back(static_cast<std::uint32_t>(rank));
    }
    return ranks;
}

struct fixed_bounded_nthash_run {
    std::vector<minimizer_record> records;
    bounded_nthash_execution_stats stats{};
    std::size_t emitted = 0u;
};

template<class p_storage_unit>
[[nodiscard]] fixed_bounded_nthash_run run_fixed_bounded_nthash(
    std::vector<seq::dna4>& data,
    const std::size_t window = 10u,
    const std::size_t kmer = 15u)
{
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, p_storage_unit>;
    compact_sequence_range<sequence_type> range(data);
    collecting_minimizer_sink sink;
    minimizer_kernel_invocation invocation{
        .sequence = fixed_packed_view(range),
        .fragments = {},
        .window = window,
        .kmer = kmer,
        .emitted_position_base = 0u,
        .output_layout =
            bioseq::minimizers::k_minimizer_output_layout<minimap_sketch_data>,
        .two_stack_route = two_stack_route::chronological_continuation,
        .sink = sink.contract(),
    };
    const auto& binding =
        ::bioseq::simd_dispatch::highway_test_target_binding();
    const auto kernel = fixed_highway_table().minimizers.bounded_nthash;
    REQUIRE(kernel != nullptr);
    const auto result = kernel(invocation);
    auto records = decoded_records(sink.records);

    REQUIRE(result.status == minimizer_kernel_status::ok);
    REQUIRE(result.family_status
        == static_cast<std::uint32_t>(bounded_nthash_status::ok));
    REQUIRE(result.stats.algorithm
        == minimizer_kernel_algorithm::bounded_nthash);
    REQUIRE(result.stats.target == binding.target);
    REQUIRE(sink.protocol_ok);
    REQUIRE(result.emitted == records.size());
    REQUIRE(result.stats.bounded_nthash.is_internally_consistent(true));
    REQUIRE(result.stats.bounded_nthash.source_storage_unit_bytes
        == sizeof(p_storage_unit));
    REQUIRE(result.stats.bounded_nthash.input_symbols == data.size());
    return {
        .records = std::move(records),
        .stats = result.stats.bounded_nthash,
        .emitted = result.emitted,
    };
}

TEST_CASE(
    "generated fixed table two_stack_nthash rejects a canonical position overflow",
    "[component:bioseq][layer:unit][feature:minimizer][resource:highway]")
{
    constexpr std::size_t sequence_size = 32u;
    constexpr std::uint64_t position_mask =
        minimap_sketch_data::k_position_field_mask;
    constexpr std::size_t emitted_position_base =
        static_cast<std::size_t>(
            position_mask - (sequence_size - 2u));
    STATIC_REQUIRE(
        emitted_position_base + sequence_size - 1u == position_mask + 1u);

    auto data = deterministic_dna4(sequence_size);
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    compact_sequence_range<sequence_type> range(data);
    collecting_minimizer_sink sink;
    minimizer_kernel_invocation invocation{
        .sequence = fixed_packed_view(range),
        .fragments = {},
        .window = 10u,
        .kmer = 15u,
        .emitted_position_base = emitted_position_base,
        .output_layout =
            bioseq::minimizers::k_minimizer_output_layout<minimap_sketch_data>,
        .output_hash_mask = minimap_sketch_data::k_hash_field_mask,
        .output_position_mask = position_mask,
        .two_stack_route = two_stack_route::chronological_continuation,
        .sink = sink.contract(),
    };

    const auto& binding =
        ::bioseq::simd_dispatch::highway_test_target_binding();
    const auto kernel = fixed_highway_table().minimizers.two_stack_nthash;
    REQUIRE(kernel != nullptr);
    const auto result = kernel(invocation);

    REQUIRE(result.status == minimizer_kernel_status::invalid_configuration);
    REQUIRE(result.family_status == static_cast<std::uint32_t>(
        two_stack_continuation_status::unsupported_representation));
    REQUIRE(result.stats.algorithm
        == minimizer_kernel_algorithm::two_stack_nthash);
    REQUIRE(result.stats.target == binding.target);
    REQUIRE(result.emitted == 0u);
    REQUIRE(sink.records.empty());
    REQUIRE(sink.protocol_ok);
}

TEST_CASE(
    "generated fixed table bounded ntHash rejects a sub-u32 hash representation",
    "[component:bioseq][layer:unit][feature:minimizer][resource:highway]")
{
    using output_type = bioseq::minimizers::minimizer_representation<33u, 15u>;
    STATIC_REQUIRE(output_type::k_hash_field_mask < 0xFFFFFFFFull);

    auto data = deterministic_dna4(32u);
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    compact_sequence_range<sequence_type> range(data);
    collecting_minimizer_sink sink;
    minimizer_kernel_invocation invocation{
        .sequence = fixed_packed_view(range),
        .fragments = {},
        .window = 10u,
        .kmer = 15u,
        .emitted_position_base = 0u,
        .output_layout =
            bioseq::minimizers::k_minimizer_output_layout<output_type>,
        .output_hash_mask = output_type::k_hash_field_mask,
        .output_position_mask = output_type::k_position_field_mask,
        .two_stack_route = two_stack_route::chronological_continuation,
        .sink = sink.contract(),
    };

    const auto& binding =
        ::bioseq::simd_dispatch::highway_test_target_binding();
    const auto kernel = fixed_highway_table().minimizers.bounded_nthash;
    REQUIRE(kernel != nullptr);
    const auto result = kernel(invocation);

    REQUIRE(result.status == minimizer_kernel_status::invalid_configuration);
    REQUIRE(result.family_status == static_cast<std::uint32_t>(
        bounded_nthash_status::unsupported_representation));
    REQUIRE(result.stats.algorithm == minimizer_kernel_algorithm::bounded_nthash);
    REQUIRE(result.stats.target == binding.target);
    REQUIRE(result.emitted == 0u);
    REQUIRE(sink.records.empty());
    REQUIRE(sink.protocol_ok);
}

TEST_CASE(
    "generated fixed table two_stack_nthash preserves DNA4 packed widths and tails",
    "[component:bioseq][layer:unit][feature:minimizer][resource:highway]")
{
    const auto data = deterministic_dna4(391u);
    require_fixed_exact_width<std::uint8_t>(data, 96u);
    require_fixed_exact_width<std::uint16_t>(data, 48u);
    require_fixed_exact_width<std::uint32_t>(data, 24u);
    require_fixed_exact_width<std::uint64_t>(data, 12u);
}

TEST_CASE(
    "generated fixed table two_stack_nthash preserves the resident-vector-stream hot path",
    "[component:bioseq][layer:unit][feature:minimizer][resource:highway]")
{
    require_fixed_resident_vector_stream_parity(deterministic_dna4(4096u));
}

TEST_CASE(
    "generated fixed table bounded ntHash K15 W10 resident u32 matches independent compact oracle",
    "[component:bioseq][layer:unit][feature:minimizer][resource:highway]")
{
    constexpr std::size_t kmer = 15u;
    constexpr std::size_t window = 10u;
    auto data = deterministic_dna4(521u);
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    compact_sequence_range<sequence_type> range(data);
    const auto ranks = compact_ranks(range);
    auto expected = bounded_nthash_scalar_oracle<kmer, window>(ranks);

    auto run = run_fixed_bounded_nthash<std::uint32_t>(data, window, kmer);

    std::ranges::sort(run.records);
    std::ranges::sort(expected);
    REQUIRE(run.records == expected);

    const auto& stats = run.stats;
    REQUIRE(stats.scheme_id == k_bounded_nthash_scheme_id);
    REQUIRE(stats.kmer_size == kmer);
    REQUIRE(stats.window_size == window);
    REQUIRE(stats.nthash_rotation == 7u);
    REQUIRE(stats.source_storage_unit_bytes == sizeof(std::uint32_t));
    REQUIRE(stats.input_symbols == data.size());
    REQUIRE(stats.logical_streams == 8u);
    REQUIRE(stats.minimizers_emitted == run.records.size());
    REQUIRE(stats.scalar_hash_symbols == 0u);
    REQUIRE(stats.scalar_window_transitions == 0u);
    REQUIRE(stats.scalar_minimum_decisions == 0u);
    REQUIRE(stats.fallback_calls == 0u);
}

TEST_CASE(
    "generated fixed table bounded ntHash preserves u64 packed storage through the u32 SIMD view",
    "[component:bioseq][layer:unit][feature:minimizer][resource:highway]")
{
    auto data = deterministic_dna4(521u);
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    compact_sequence_range<sequence_type> range(data);
    const auto ranks = compact_ranks(range);
    auto expected = bounded_nthash_scalar_oracle<15u, 10u>(ranks);

    auto u32 = run_fixed_bounded_nthash<std::uint32_t>(data);
    auto u64 = run_fixed_bounded_nthash<std::uint64_t>(data);
    REQUIRE(u64.records == u32.records);
    std::ranges::sort(u64.records);
    std::ranges::sort(expected);
    REQUIRE(u64.records == expected);
    REQUIRE(u64.stats.minimizers_emitted == u64.emitted);
}

TEST_CASE(
    "generated fixed table bounded ntHash retains tied endpoints and adjacent deduplication",
    "[component:bioseq][layer:unit][feature:minimizer][resource:highway]")
{
    std::vector<seq::dna4> data(257u);
    for (seq::dna4& symbol : data) symbol.assign_rank(0u);
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    compact_sequence_range<sequence_type> range(data);
    const auto ranks = compact_ranks(range);
    auto expected = bounded_nthash_scalar_oracle<15u, 10u>(ranks);
    auto run = run_fixed_bounded_nthash<std::uint32_t>(data);

    std::ranges::sort(run.records);
    std::ranges::sort(expected);
    REQUIRE(run.records == expected);
    REQUIRE(run.stats.tied_endpoint_records_emitted > 0u);
    REQUIRE(run.stats.stream_boundary_records_buffered
        == run.stats.stream_boundary_records_committed
            + run.stats.stream_boundary_duplicates_suppressed);
}

[[nodiscard]] constexpr bounded_nthash_execution_stats
valid_bounded_nthash_stats(const std::uint64_t input_symbols) noexcept {
    bounded_nthash_execution_stats stats{};
    stats.scheme_id = k_bounded_nthash_scheme_id;
    stats.kmer_size = 15u;
    stats.window_size = 10u;
    stats.nthash_rotation = 7u;
    stats.source_storage_unit_bytes = sizeof(std::uint32_t);
    stats.input_symbols = input_symbols;
    stats.input_storage_units = input_symbols / 16u;
    stats.segments_combined = 1u;
    stats.logical_streams = 8u;
    stats.physical_vector_lanes = 4u;
    stats.vector_registers_per_step = 2u;
    stats.packed_decode_batches = 1u;
    stats.packed_decode_lane_visits = 8u;
    stats.hash_initialization_batches = 1u;
    stats.rolling_hash_batches = 1u;
    stats.hash_table_lookup_batches = 1u;
    stats.canonical_sum_batches = 1u;
    stats.window_update_batches = 1u;
    stats.emission_decision_batches = 1u;
    stats.emission_mask_store_batches = 1u;
    stats.minimizers_emitted = 2u;
    stats.scalar_collector_appends = 2u;
    stats.positions_are_segment_local = true;
    return stats;
}

TEST_CASE("packed bounded ntHash stats aggregation is identity checked and saturating",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    const auto first = valid_bounded_nthash_stats(128u);
    const auto second = valid_bounded_nthash_stats(256u);
    REQUIRE(first.is_internally_consistent());
    REQUIRE(second.is_internally_consistent());

    auto combined = first;
    combined.merge(second);
    REQUIRE(combined.segments_combined == 2u);
    REQUIRE(combined.input_symbols
        == first.input_symbols + second.input_symbols);
    REQUIRE(combined.is_internally_consistent());

    auto unchanged = first;
    unchanged.merge(bounded_nthash_execution_stats{});
    REQUIRE(unchanged == first);

    bounded_nthash_execution_stats adopted{};
    adopted.merge(first);
    REQUIRE(adopted == first);

    auto incompatible = second;
    ++incompatible.kmer_size;
    auto mismatch = first;
    mismatch.merge(incompatible);
    REQUIRE(mismatch.aggregation_mismatch);
    REQUIRE_FALSE(mismatch.is_internally_consistent());

    auto saturated = first;
    saturated.input_symbols =
        (std::numeric_limits<std::uint64_t>::max)();
    saturated.merge(second);
    REQUIRE(saturated.input_symbols
        == (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE(saturated.saturated);
    REQUIRE_FALSE(saturated.is_internally_consistent());
}

TEST_CASE("packed bounded ntHash stats rejects mixed source storage identities",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    auto u32 = valid_bounded_nthash_stats(128u);
    auto u64 = valid_bounded_nthash_stats(128u);
    u64.source_storage_unit_bytes = sizeof(std::uint64_t);
    u64.input_storage_units = 4u;
    REQUIRE(u32.is_internally_consistent());
    REQUIRE(u64.is_internally_consistent());

    auto forward = u32;
    forward.merge(u64);
    REQUIRE(forward.aggregation_mismatch);
    REQUIRE(forward.source_storage_unit_bytes == sizeof(std::uint32_t));
    REQUIRE(forward.segments_combined == 2u);
    REQUIRE_FALSE(forward.is_internally_consistent());

    auto reverse = u64;
    reverse.merge(u32);
    REQUIRE(reverse.aggregation_mismatch);
    REQUIRE(reverse.source_storage_unit_bytes == sizeof(std::uint64_t));
    REQUIRE(reverse.segments_combined == 2u);
    REQUIRE_FALSE(reverse.is_internally_consistent());
}

} // namespace
