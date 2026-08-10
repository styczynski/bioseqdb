/**
 * @file vulkan_minimizers_test.cpp
 * @brief Vulkan minimizer sketch parity tests.
 *
 * Tests 1-4: dna4 parity for (15,10)/(7,5)/(27,10), dna5 T/N parity (N resets,
 * T hashes as nt4 code 3), generic-alphabet smoke (no throw), and the
 * 16/32/64/128-bit `minimizer_representation` layouts round-tripping raw fields. All GPU
 * comparisons skip cleanly (SUCCEED) when no hardware Vulkan device exists.
 */
#include <hwy/highway.h>
#include <tests/support/common.hpp>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <tuple>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.parallelism.memory_admission;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

namespace vk = ::bioseq::gpu::vulkan;
namespace idx = ::bioseq::indexing;

using minimizer_tuple_t =
    std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;

[[nodiscard]] static bool vulkan_ready() {
    return vk::hardware_device_available(vk::runtime::instance());
}

[[nodiscard]] static bool software_vulkan_ready() {
  auto &runtime = vk::runtime::instance();
  return runtime.is_usable() && runtime.features().software_device;
}

#ifdef BIOSEQ_TEST_INDEXES
using vulkan_lifetime_facade =
    bioseq::storage::lazy_sequence_view<seq::dna4>;

struct vulkan_lifetime_source_state {
  std::vector<std::uint8_t> ranks;
  std::vector<std::uint32_t> packed;
  bool expose_resident = false;
  std::thread::id backend_thread;
  std::atomic<std::uint64_t> *destructions = nullptr;
  mutable std::atomic<std::uint64_t> read_calls{0u};
  mutable std::atomic<std::uint64_t> resident_probes{0u};
  mutable std::atomic<std::uint64_t> resident_borrows{0u};
  mutable std::atomic<bool> wrong_thread{false};

  ~vulkan_lifetime_source_state() {
    if (destructions != nullptr) {
      destructions->fetch_add(1u, std::memory_order_relaxed);
    }
  }
};

template <bool p_is_reversed, bool p_write_reversed>
[[nodiscard]] const std::uint8_t *
vulkan_lifetime_read_at(const void *opaque,
                       bioseq::storage::sequence_size_t from, std::size_t count,
                       std::span<std::uint8_t> scratch) {
  const auto &state = *static_cast<const vulkan_lifetime_source_state *>(opaque);
  if (std::this_thread::get_id() != state.backend_thread) {
    state.wrong_thread.store(true, std::memory_order_relaxed);
  }
  state.read_calls.fetch_add(1u, std::memory_order_relaxed);
  const std::size_t logical_size = state.ranks.size();
  const std::size_t forward_from =
      p_is_reversed ? logical_size - (static_cast<std::size_t>(from) + count)
                 : static_cast<std::size_t>(from);
  constexpr bool descending_write =
      (p_is_reversed && !p_write_reversed) || (!p_is_reversed && p_write_reversed);
  for (std::size_t index = 0u; index < count; ++index) {
    std::uint8_t rank = state.ranks[forward_from + index];
    if constexpr (p_is_reversed) {
      rank = static_cast<std::uint8_t>(3u - rank);
    }
    scratch[descending_write ? count - index - 1u : index] = rank;
  }
  return scratch.data();
}

[[nodiscard]] bioseq::storage::sequence_size_t
vulkan_lifetime_bp_size(const void *opaque) noexcept {
  return static_cast<const vulkan_lifetime_source_state *>(opaque)->ranks.size();
}

[[nodiscard]] vulkan_lifetime_facade::resident_packed_result
vulkan_lifetime_resident_probe(const void *opaque,
                               bioseq::storage::sequence_size_t from,
                               bioseq::storage::sequence_size_t to,
                               std::uint8_t requested_width) noexcept {
  const auto &state =
      *static_cast<const vulkan_lifetime_source_state *>(opaque);
  state.resident_probes.fetch_add(1u, std::memory_order_relaxed);
  if (!state.expose_resident || requested_width != sizeof(std::uint32_t) ||
      from > to || to > state.ranks.size()) {
    return std::nullopt;
  }
  constexpr bioseq::storage::sequence_size_t letters_per_unit = 16u;
  const bioseq::storage::sequence_size_t length = to - from;
  if (length == 0u) {
    state.resident_borrows.fetch_add(1u, std::memory_order_relaxed);
    return vulkan_lifetime_facade::resident_packed_sequence{
        .units = nullptr,
        .unit_count = 0u,
        .logical_length = 0u,
        .storage_unit_width = sizeof(std::uint32_t),
        .bits_per_symbol = 2u,
        .leading_symbol_offset = 0u,
        .trailing_padding = 0u,
        .bit_order = vulkan_lifetime_facade::packed_symbol_order::
            least_significant_symbol_first,
    };
  }
  const bioseq::storage::sequence_size_t first_unit =
      from / letters_per_unit;
  const bioseq::storage::sequence_size_t leading =
      from % letters_per_unit;
  const bioseq::storage::sequence_size_t occupied = leading + length;
  const bioseq::storage::sequence_size_t unit_count =
      occupied / letters_per_unit +
      static_cast<bioseq::storage::sequence_size_t>(
          occupied % letters_per_unit != 0u);
  const bioseq::storage::sequence_size_t trailing =
      unit_count * letters_per_unit - occupied;
  if (first_unit > state.packed.size() ||
      unit_count > state.packed.size() - first_unit) {
    return std::nullopt;
  }
  state.resident_borrows.fetch_add(1u, std::memory_order_relaxed);
  return vulkan_lifetime_facade::resident_packed_sequence{
      .units = state.packed.data() + static_cast<std::size_t>(first_unit),
      .unit_count = static_cast<std::size_t>(unit_count),
      .logical_length = length,
      .storage_unit_width = sizeof(std::uint32_t),
      .bits_per_symbol = 2u,
      .leading_symbol_offset = static_cast<std::uint16_t>(leading),
      .trailing_padding = static_cast<std::uint16_t>(trailing),
      .bit_order = vulkan_lifetime_facade::packed_symbol_order::
          least_significant_symbol_first,
  };
}

[[nodiscard]] bool vulkan_lifetime_worker_shareable(const void *) noexcept {
  return false;
}

inline const vulkan_lifetime_facade::backend_ops vulkan_lifetime_ops{
    .bp_size = &vulkan_lifetime_bp_size,
    .read_at =
        {
            &vulkan_lifetime_read_at<false, false>,
            &vulkan_lifetime_read_at<false, true>,
            &vulkan_lifetime_read_at<true, false>,
            &vulkan_lifetime_read_at<true, true>,
        },
    .try_resident_packed = &vulkan_lifetime_resident_probe,
    .copy_packed_sequence = nullptr,
    .worker_shareable = &vulkan_lifetime_worker_shareable,
};

[[nodiscard]] vulkan_lifetime_facade
vulkan_lifetime_view(const vulkan_lifetime_source_state &state) noexcept {
  return vulkan_lifetime_facade{
      std::addressof(state), std::addressof(vulkan_lifetime_ops)};
}
#endif

/// Sorted (last_pos, kmer, strand) tuples of a minimizer vector.
template <class p_minimizer_type>
[[nodiscard]] static std::vector<minimizer_tuple_t>
triples(const std::vector<p_minimizer_type> &minimizers) {
  std::vector<minimizer_tuple_t> out;
  out.reserve(minimizers.size());
  for (const p_minimizer_type &m : minimizers) {
    out.emplace_back(m.last_pos(), m.kmer(), m.strand());
  }
  std::sort(out.begin(), out.end());
  return out;
}

[[nodiscard]] static std::string tuple_examples(
    const std::vector<minimizer_tuple_t>& values,
    std::size_t limit = 6) {
    std::ostringstream out;
    const std::size_t n = std::min(values.size(), limit);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& [pos, kmer, strand] = values[i];
        if (i != 0) out << "; ";
        out << "(pos=" << pos << ",kmer=" << kmer << ",strand=" << strand << ")";
    }
    if (values.size() > n) out << "; ...";
    return out.str();
}

static void require_gpu_triple_superset_with_bounded_extras(
    std::vector<minimizer_tuple_t> scalar,
    std::vector<minimizer_tuple_t> gpu,
    vk::gpu_sketch_tile_plan tile_plan,
    std::uint32_t k,
    std::uint32_t window,
    std::uint64_t bp_offset = 0) {
    std::sort(scalar.begin(), scalar.end());
    std::sort(gpu.begin(), gpu.end());

    std::vector<minimizer_tuple_t> missing;
    std::vector<minimizer_tuple_t> extras;
    std::size_t scalar_i = 0;
    std::size_t gpu_i = 0;
    while (scalar_i < scalar.size() && gpu_i < gpu.size()) {
        if (scalar[scalar_i] == gpu[gpu_i]) {
            ++scalar_i;
            ++gpu_i;
        } else if (gpu[gpu_i] < scalar[scalar_i]) {
            extras.push_back(gpu[gpu_i]);
            ++gpu_i;
        } else {
            missing.push_back(scalar[scalar_i]);
            ++scalar_i;
        }
    }
    while (scalar_i < scalar.size()) {
        missing.push_back(scalar[scalar_i]);
        ++scalar_i;
    }
    while (gpu_i < gpu.size()) {
        extras.push_back(gpu[gpu_i]);
        ++gpu_i;
    }

    INFO("missing examples: " << tuple_examples(missing));
    REQUIRE(missing.empty());

    const std::uint64_t radius =
        static_cast<std::uint64_t>(k) + static_cast<std::uint64_t>(window) + 1u;
    const std::uint64_t max_extras =
        static_cast<std::uint64_t>(tile_plan.tile_count) * 2u * radius;
    INFO("extra examples: " << tuple_examples(extras));
    INFO("scalar=" << scalar.size() << " gpu=" << gpu.size()
                   << " extras=" << extras.size()
                   << " max_extras=" << max_extras);
    (void)bp_offset;
    REQUIRE(extras.size() <= max_extras);
}

template <class p_minimizer_type>
static void require_gpu_minimizer_superset_with_bounded_tile_extras(
    const std::vector<p_minimizer_type>& scalar,
    const std::vector<p_minimizer_type>& gpu,
    vk::gpu_sketch_tile_plan tile_plan,
    std::uint32_t k,
    std::uint32_t window,
    std::uint64_t bp_offset = 0) {
    require_gpu_triple_superset_with_bounded_extras(
        triples(scalar), triples(gpu), tile_plan, k, window, bp_offset);
}

static void require_gpu_pair_superset_with_bounded_tile_extras(
    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& scalar,
    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& gpu,
    vk::gpu_sketch_tile_plan tile_plan,
    std::uint32_t k,
    std::uint32_t window,
    std::uint64_t bp_offset = 0) {
    auto to_triples = [](const auto& pairs) {
        std::vector<minimizer_tuple_t> out;
        out.reserve(pairs.size());
        for (const auto& [kmer, encoded_pos] : pairs) {
            out.emplace_back(encoded_pos >> 1u, kmer, encoded_pos & 1u);
        }
        return out;
    };
    require_gpu_triple_superset_with_bounded_extras(
        to_triples(scalar), to_triples(gpu), tile_plan, k, window, bp_offset);
}

TEST_CASE("Vulkan owned segment pairs transfer capacity with their permit",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    const std::uint64_t baseline = admission.reserved_bytes();
    struct restore_cap_guard {
        bioseq::parallelism::memory_admission_controller& admission;
        std::uint64_t previous = 0;
        ~restore_cap_guard() { admission.set_index_memory_cap_bytes(previous); }
    } restore{admission, previous_cap};
    admission.set_index_memory_cap_bytes(std::max<std::uint64_t>(
        previous_cap, baseline + (std::uint64_t{1} << 20)));

    constexpr std::size_t count = 32u;
    using pair_type = vk::vulkan_owned_segment_pairs::value_type;
    const std::uint64_t headroom = count * sizeof(pair_type)
        * bioseq::parallelism::k_vector_preallocation_headroom_factor;
    auto permit = admission.try_reserve(headroom);
    REQUIRE(permit.has_value());
    vk::vulkan_owned_segment_pairs source;
    source.permit = std::move(*permit);
    source.pairs.reserve(count);
    const std::uint64_t exact = source.pairs.capacity() * sizeof(pair_type);
    REQUIRE(exact <= source.permit.bytes());
    source.permit.shrink_to(exact);
    source.pairs.emplace_back(7u, 11u);

    vk::vulkan_owned_segment_pairs destination{std::move(source)};
    REQUIRE(source.pairs.capacity() == 0u);
    REQUIRE_FALSE(static_cast<bool>(source.permit));
    REQUIRE((destination.pairs.front() == pair_type{7u, 11u}));
    REQUIRE(destination.permit.bytes()
            == destination.pairs.capacity() * sizeof(pair_type));
    destination.release_capacity_before_permit();
    REQUIRE(admission.reserved_bytes() == baseline);
}

template <class p_minimizer_type, class p_alphabet>
[[nodiscard]] static std::vector<p_minimizer_type> cpu_sketch(
    std::vector<p_alphabet>& data, std::size_t w, std::size_t k) {
    using seq_type = sequence_custom_alphabet<p_alphabet, std::uint64_t>;
    auto seq = compact_sequence_range<seq_type>(data);
    basic_sketch_collector<p_minimizer_type> collector;
    collect_minimizers<minimap_int_hasher, p_minimizer_type, seq_type,
                       decltype(seq.begin()), std::uint64_t>(
        collector, seq, seq.begin(), seq.end(), w, k);
    return std::move(collector).extract_sketches_data();
}

template <class p_minimizer_type, class p_alphabet>
[[nodiscard]] static std::vector<p_minimizer_type> gpu_sketch(
    std::vector<p_alphabet>& data,
    std::size_t w,
    std::size_t k) {
    using seq_type = sequence_custom_alphabet<p_alphabet, std::uint64_t>;
    auto seq = compact_sequence_range<seq_type>(data);
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(seq);
    const auto sequence_view =
        bioseq::storage::make_lazy_sequence_view(sequence_owner);
    auto result =
        vk::collect_minimizers_vulkan_result<p_minimizer_type>(
            sequence_view, w, k);
    if (!result) return {};
    return std::move(result->host_minimizers);
}

static std::vector<seq::dna4> dna4_fixture{
    "ACGTACGTGGCATTACGATCGATCGTAGCTAGCTACGTACGATCGATCGTAGCTAGCATCGATCG"
    "TTACGATCGTAGCTAGCTTACGTACGATCGATCGTAGCTAGCATCGATCGTAGCTAGCTACGTAC"
    "GGCATTACGATCGATCGTAGCTAGCTACGTACGATCGATCGTAGCTAGCATCGATCGTAGCTAGC"_dna4
};
static std::vector<seq::dna5> dna5_with_t{
    "ACGTACGTGGCATTACGATCGATCGTAGCTAGCTACGTACGATCGATCGTAGCTAGCATCGATCG"
    "TTACGATCGTAGCTAGCTTACGTACGATCGATCGTAGCTAGCATCGATCGTAGCTAGCTACGTAC"_dna5
};
static std::vector<seq::dna5> dna5_with_n{
    "ACGTACGTGGCATTACGNNNNNATCGTAGCTAGCTACGTACGATCGATCGTAGCTAGCATCGATC"
    "GTTACGATNNCGTAGCTAGCTTACGTACGATCGATCGTAGCTAGCATCGATCGTAGCTAGCTACG"_dna5
};

TEST_CASE("vulkan minimizer: target-index host scratch ends before exact main admission",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    constexpr std::uint64_t bp_size = 500'000'000u;
    constexpr std::uint64_t packed_sequence_bytes = 125'000'000u;
    const vk::gpu_sketch_memory_plan plan = vk::make_gpu_sketch_memory_plan(
        bp_size,
        packed_sequence_bytes,
        sizeof(vk::vulkan_rank_lut),
        93'308'896u,
        0u,
        vk::gpu_sketch_purpose::target_index_build,
        15u,
        10u,
        32u);
    REQUIRE(plan.is_valid);
    REQUIRE(plan.host_sequence_staging_bytes == 125'000'000u);
    REQUIRE(plan.host_tile_staging_bytes == 46'875'000u);
    REQUIRE(plan.execution_submit_host_transient_bytes == 0u);
    REQUIRE(plan.host_transient_released_before_execution_bytes
            == 171'875'000u);

    const auto fragmented = vk::make_gpu_target_index_host_scratch_plan(
        plan, packed_sequence_bytes);
    REQUIRE(fragmented.is_valid);
    REQUIRE(fragmented.count_submit_bytes == 125'000'000u);
    REQUIRE(fragmented.execution_submit_bytes == 0u);
    REQUIRE(fragmented.released_before_execution_bytes == 171'875'000u);

    const auto contiguous = vk::make_gpu_target_index_host_scratch_plan(
        plan, 0u);
    REQUIRE(contiguous.is_valid);
    REQUIRE(contiguous.count_submit_bytes == 0u);
    REQUIRE(contiguous.execution_submit_bytes == 0u);
    REQUIRE(contiguous.released_before_execution_bytes == 46'875'000u);

    const vk::gpu_sketch_memory_plan small_words =
        vk::make_gpu_sketch_memory_plan(
            64u,
            16u,
            sizeof(vk::vulkan_rank_lut),
            64u,
            512u,
            vk::gpu_sketch_purpose::minimizer_words,
            15u,
            10u,
            32u);
    REQUIRE(small_words.is_valid);
    REQUIRE(small_words.count_submit_host_transient_bytes
            == small_words.execution_submit_host_transient_bytes);
    REQUIRE(small_words.host_transient_released_before_execution_bytes == 0u);
}

TEST_CASE("vulkan minimizer: 500-Mbp w10 dispatch excludes pessimistic host result",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    constexpr std::uint64_t bp_size = 500'000'000u;
    constexpr std::uint64_t window_size = 10u;
    constexpr std::uint32_t kmer_size = 15u;
    constexpr std::uint32_t letters_per_unit = 32u;
    constexpr std::uint64_t packed_sequence_bytes = bp_size / 4u;

    const std::uint64_t max_minimizers =
        vk::estimate_vulkan_minimizer_capacity(bp_size, window_size);
    REQUIRE(max_minimizers == 150'000'192u);

    const std::uint64_t pessimistic_initial_host_bytes =
        bioseq::parallelism::saturating_mul(
            bioseq::parallelism::saturating_mul(
                max_minimizers,
                static_cast<std::uint64_t>(sizeof(minimap_sketch_data))),
            bioseq::parallelism::k_vector_preallocation_headroom_factor);
    REQUIRE(pessimistic_initial_host_bytes == 2'400'003'072u);

    const std::uint64_t resident_requested_bytes =
        vk::vulkan_sketch_resident_bytes<minimap_sketch_data>(max_minimizers);
    REQUIRE(resident_requested_bytes == 1'200'001'536u);
    const vk::gpu_sketch_memory_plan plan = vk::make_gpu_sketch_memory_plan(
        bp_size,
        packed_sequence_bytes,
        sizeof(vk::vulkan_rank_lut),
        max_minimizers,
        resident_requested_bytes,
        vk::gpu_sketch_purpose::minimizer_words,
        kmer_size,
        static_cast<std::uint32_t>(window_size),
        letters_per_unit);
    REQUIRE(plan.is_valid);

    const std::uint64_t growable_vector_headroom =
        bioseq::parallelism::saturating_mul(
            plan.count_submit_host_transient_bytes,
            bioseq::parallelism::k_vector_preallocation_headroom_factor);
    const std::uint64_t host_dispatch_headroom =
        vk::vulkan_exact_host_allocation_admission_bytes(
            plan.count_submit_host_transient_bytes);
    REQUIRE(
        host_dispatch_headroom !=
        std::numeric_limits<std::uint64_t>::max());
    REQUIRE(
        growable_vector_headroom !=
        std::numeric_limits<std::uint64_t>::max());
    CHECK(
        host_dispatch_headroom ==
        plan.count_submit_host_transient_bytes +
            vk::k_vulkan_exact_host_allocation_headroom_bytes);
    CHECK(host_dispatch_headroom < growable_vector_headroom);
    CHECK(
        vk::vulkan_exact_host_allocation_admission_bytes(
            std::numeric_limits<std::uint64_t>::max()) ==
        std::numeric_limits<std::uint64_t>::max());
    CHECK(
        vk::vulkan_exact_host_allocation_admission_bytes(
            std::numeric_limits<std::uint64_t>::max() -
            vk::k_vulkan_exact_host_allocation_headroom_bytes + 1u) ==
        std::numeric_limits<std::uint64_t>::max());
    const std::uint64_t split_dispatch_bytes =
        vk::vulkan_sketch_dispatch_admission_bytes(
            plan.arena_scaffold_bytes,
            host_dispatch_headroom,
            0u,
            resident_requested_bytes);
    const std::uint64_t dispatch_without_host_result =
        bioseq::parallelism::saturating_add(
            resident_requested_bytes,
            vk::vulkan_sketch_transient_reserved_bytes(
                bp_size,
                packed_sequence_bytes,
                letters_per_unit,
                window_size,
                kmer_size,
                max_minimizers,
                resident_requested_bytes));
    const std::uint64_t simultaneous_reservation_bytes =
        bioseq::parallelism::saturating_add(
            dispatch_without_host_result,
            pessimistic_initial_host_bytes);

    REQUIRE(split_dispatch_bytes < simultaneous_reservation_bytes);
    REQUIRE(simultaneous_reservation_bytes
                - dispatch_without_host_result
            == 2'400'003'072u);

    // The runtime-derived arena and resident requirements are held constant;
    // the two calculations differ only in the 500-Mbp dna4 staging ownership.
    constexpr std::uint64_t captured_required_bytes =
        1'492'942'336u;
    constexpr std::uint64_t captured_allocatable_bytes =
        1'419'423'232u;
    constexpr std::uint64_t captured_packed_sequence_bytes =
        125'000'000u;
    const std::uint64_t padded_fixed_staging_bytes =
        bioseq::parallelism::saturating_mul(
            captured_packed_sequence_bytes,
            bioseq::parallelism::k_vector_preallocation_headroom_factor);
    const std::uint64_t exact_fixed_staging_bytes =
        vk::vulkan_exact_host_allocation_admission_bytes(
            captured_packed_sequence_bytes);
    REQUIRE(
        captured_required_bytes >=
        padded_fixed_staging_bytes);
    const std::uint64_t captured_corrected_required_bytes =
        captured_required_bytes -
        padded_fixed_staging_bytes +
        exact_fixed_staging_bytes;
    REQUIRE(
        captured_allocatable_bytes >=
        captured_corrected_required_bytes);
    const std::uint64_t captured_corrected_slack_bytes =
        captured_allocatable_bytes -
        captured_corrected_required_bytes;

    CHECK(
        captured_required_bytes >
        captured_allocatable_bytes);
    CHECK(padded_fixed_staging_bytes == 250'000'000u);
    CHECK(exact_fixed_staging_bytes == 125'065'536u);
    CHECK(captured_corrected_required_bytes == 1'368'007'872u);
    CHECK(
        captured_corrected_required_bytes <=
        captured_allocatable_bytes);
    CHECK(captured_corrected_slack_bytes == 51'415'360u);

}

TEST_CASE("vulkan minimizer: zero-copy handoff fits captured memory pressure",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    STATIC_REQUIRE(vk::vulkan_fixed_host_result_admission_bytes(0u) == 0u);
    STATIC_REQUIRE(
        vk::vulkan_fixed_host_result_admission_bytes(1u)
        == 1u + vk::k_vulkan_exact_host_allocation_headroom_bytes);
    constexpr std::uint64_t emitted_minimizers = 93'308'896u;
    constexpr std::uint64_t device_output_bytes = 1'200'001'536u;
    constexpr std::uint64_t persistent_runtime_bytes = 676'076u;
    constexpr std::uint64_t captured_budget_ceiling = 2'601'156'608u;
    constexpr std::uint64_t captured_reserved_before_host =
        device_output_bytes + persistent_runtime_bytes;
    constexpr std::uint64_t captured_allocatable_before_host =
        captured_budget_ceiling - captured_reserved_before_host;

    const std::uint64_t host_payload_bytes =
        emitted_minimizers * sizeof(minimap_sketch_data);
    const std::uint64_t fixed_host_admission =
        vk::vulkan_fixed_host_result_admission_bytes(
            host_payload_bytes);
    const std::uint64_t growable_collector_host_admission =
        bioseq::parallelism::saturating_mul(
            host_payload_bytes,
            bioseq::parallelism::k_vector_preallocation_headroom_factor);
    REQUIRE(host_payload_bytes == 746'471'168u);
    REQUIRE(fixed_host_admission == 746'536'704u);
    REQUIRE(growable_collector_host_admission == 1'492'942'336u);
    REQUIRE(captured_allocatable_before_host == 1'400'478'996u);
    REQUIRE(fixed_host_admission <= captured_allocatable_before_host);
    REQUIRE(growable_collector_host_admission
            > captured_allocatable_before_host);
    REQUIRE(captured_allocatable_before_host - fixed_host_admission
            == 653'942'292u);
    REQUIRE(growable_collector_host_admission
                - captured_allocatable_before_host
            == 92'463'340u);

    // The tighter process ceiling exercises the same route contrast: exact
    // staging fits while the growable-vector 2x reservation cannot be admitted.
    constexpr std::uint64_t captured_tight_budget_ceiling =
        2'479'079'424u;
    constexpr std::uint64_t captured_tight_allocatable_before_host =
        captured_tight_budget_ceiling - captured_reserved_before_host;
    REQUIRE(captured_tight_allocatable_before_host == 1'278'401'812u);
    REQUIRE(captured_tight_allocatable_before_host - fixed_host_admission
            == 531'865'108u);
    REQUIRE(growable_collector_host_admission
                - captured_tight_allocatable_before_host
            == 214'540'524u);

    // The opaque Boost FOA geometry is pinned and measured by the index's
    // allocator. Reserving the full possible-distinct count produces one 2-GiB
    // table allocation; a half-size reserve produces a 1-GiB table
    // and then overlapped it with the 2-GiB growth rehash.
    const auto table_plan =
        idx::minimizer_hashmap_table_allocation_plan(emitted_minimizers);
    REQUIRE(table_plan.valid);
    REQUIRE(table_plan.group_count == 8'388'608u);
    REQUIRE(table_plan.capacity == 125'829'119u);
    REQUIRE(table_plan.allocation_bytes == 2'147'483'648u);
    const auto half_table_plan =
        idx::minimizer_hashmap_table_allocation_plan(
            emitted_minimizers / 2u + 16u);
    REQUIRE(half_table_plan.valid);
    REQUIRE(half_table_plan.allocation_bytes == 1'073'741'824u);

    // After download, serialized materialization reconciles the exact host
    // vector into live RSS and releases its temporary permit. The device owner
    // is then dropped. A fresh permit covers only allocations not yet is_present
    // in that footprint: positions plus the one-shot table.
    const std::uint64_t position_headroom =
        vk::vulkan_exact_host_allocation_admission_bytes(
            emitted_minimizers * sizeof(std::uint64_t));
    const std::uint64_t table_headroom =
        vk::vulkan_exact_host_allocation_admission_bytes(
            table_plan.allocation_bytes);
    const std::uint64_t target_index_build_permit =
        bioseq::parallelism::saturating_add(
            position_headroom,
            table_headroom);
    REQUIRE(target_index_build_permit == 2'894'085'888u);

    // Reconcile the captured pressure sample by subtracting the newly-live host
    // footprint, adding the released device allocation, and adding the arena
    // high-water now physically trimmed before host materialization.
    constexpr std::uint64_t captured_arena_high_water_bytes = 187'533'824u;
    const std::uint64_t reconciled_build_ceiling =
        captured_budget_ceiling
        - host_payload_bytes
        + device_output_bytes
        + captured_arena_high_water_bytes;
    REQUIRE(reconciled_build_ceiling == 3'242'220'800u);
    REQUIRE(target_index_build_permit + persistent_runtime_bytes
            <= reconciled_build_ceiling);
    REQUIRE(reconciled_build_ceiling
                - target_index_build_permit - persistent_runtime_bytes
            == 347'458'836u);

    const std::uint64_t retained_index_bytes =
        host_payload_bytes + table_plan.allocation_bytes;
    REQUIRE(retained_index_bytes == 2'893'954'816u);
    REQUIRE(target_index_build_permit
            < target_index_build_permit
                + half_table_plan.allocation_bytes);
}

TEST_CASE("vulkan minimizer: replacement authority admits dense resident upload",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    // A full CPU-index reservation covers both positions and the FOA table, so
    // the subsequent sorted-dense GPU upload exceeds the remaining budget by
    // 106,379,618 bytes.
    constexpr std::uint64_t retained_position_bytes = 746'471'168u;
    constexpr std::uint64_t retained_table_bytes = 2'147'483'648u;
    constexpr std::uint64_t retained_index_bytes = 2'893'954'816u;
    constexpr std::uint64_t runtime_metadata_bytes = 546u;
    constexpr std::uint64_t budget_ceiling = 4'658'036'736u;
    constexpr std::uint64_t resident_upload_bytes = 1'870'460'992u;
    constexpr std::uint64_t full_index_reserved_bytes = 2'893'955'362u;
    constexpr std::uint64_t full_index_allocatable_bytes = 1'764'081'374u;
    constexpr std::uint64_t full_index_shortfall_bytes = 106'379'618u;

    STATIC_REQUIRE(retained_index_bytes
        == retained_position_bytes + retained_table_bytes);
    STATIC_REQUIRE(full_index_reserved_bytes
        == retained_index_bytes + runtime_metadata_bytes);
    STATIC_REQUIRE(full_index_allocatable_bytes
        == budget_ceiling - full_index_reserved_bytes);
    STATIC_REQUIRE(full_index_shortfall_bytes
        == resident_upload_bytes - full_index_allocatable_bytes);
    STATIC_REQUIRE(resident_upload_bytes > full_index_allocatable_bytes);

    // The dense source host vector was nonempty, exact-size, fully touched and
    // already reflected in process footprint.  Once it is deallocated, the
    // exactly equal, fully initialized positions allocation consumes that
    // one-use authority.  Only the table remains permit-accounted.
    constexpr std::uint64_t transferred_position_footprint_bytes =
        retained_position_bytes;
    constexpr std::uint64_t retained_index_permit_bytes =
        retained_index_bytes - transferred_position_footprint_bytes;
    constexpr std::uint64_t transferred_reserved_bytes =
        retained_index_permit_bytes + runtime_metadata_bytes;
    constexpr std::uint64_t transferred_allocatable_bytes =
        budget_ceiling - transferred_reserved_bytes;
    constexpr std::uint64_t transferred_margin_bytes =
        transferred_allocatable_bytes - resident_upload_bytes;

    STATIC_REQUIRE(retained_index_permit_bytes == 2'147'483'648u);
    STATIC_REQUIRE(transferred_reserved_bytes == 2'147'484'194u);
    STATIC_REQUIRE(transferred_allocatable_bytes == 2'510'552'542u);
    STATIC_REQUIRE(resident_upload_bytes <= transferred_allocatable_bytes);
    STATIC_REQUIRE(transferred_margin_bytes == 640'091'550u);
}

TEST_CASE("vulkan target index: 500-Mbp ungated count replaces C-sized main peak",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    constexpr std::uint64_t bp_size = 500'000'000u;
    constexpr std::uint64_t packed_sequence_bytes = 125'000'000u;
    constexpr std::uint32_t window_size = 10u;
    constexpr std::uint32_t kmer_size = 15u;
    constexpr std::uint32_t letters_per_unit = 32u;
    constexpr std::uint64_t exact_minimizer_count = 93'308'896u;
    constexpr std::uint64_t distinct_minimizer_count = 70'249'364u;
    constexpr std::uint64_t safe_memory_ceiling = 5'102'125'056u;

    const std::uint64_t semantic_hard_bound =
        vk::target_minimizer_count_upper_bound(bp_size, kmer_size);
    REQUIRE(semantic_hard_bound == 499'999'986u);
    REQUIRE(exact_minimizer_count <= semantic_hard_bound);
    REQUIRE(semantic_hard_bound
            <= (std::numeric_limits<std::uint32_t>::max)());
    STATIC_REQUIRE(vk::target_minimizer_count_upper_bound(14u, 15u) == 0u);
    STATIC_REQUIRE(vk::target_minimizer_count_upper_bound(
        (std::numeric_limits<std::uint32_t>::max)(), 1u)
        == (std::numeric_limits<std::uint32_t>::max)());

    const std::uint64_t pessimistic_capacity =
        vk::estimate_vulkan_minimizer_capacity(bp_size, window_size);
    REQUIRE(pessimistic_capacity == 150'000'192u);

    // The capacity-sized route holds three 8-byte streams plus one 16-byte
    // bucket stream, an exact 6,000,007,680-byte request.
    const std::uint64_t capacity_stream_floor =
        bioseq::parallelism::saturating_mul(pessimistic_capacity, 40u);
    const std::uint64_t exact_count_stream_floor =
        bioseq::parallelism::saturating_mul(
            exact_minimizer_count, 40u);
    REQUIRE(capacity_stream_floor == 6'000'007'680u);
    REQUIRE(exact_count_stream_floor == 3'732'355'840u);

    const auto capacity_plan = vk::make_gpu_sketch_memory_plan(
        bp_size,
        packed_sequence_bytes,
        sizeof(vk::vulkan_rank_lut),
        pessimistic_capacity,
        0u,
        vk::gpu_sketch_purpose::target_index_build,
        kmer_size,
        window_size,
        letters_per_unit,
        65'535u);
    const auto exact_plan = vk::make_gpu_sketch_memory_plan(
        bp_size,
        packed_sequence_bytes,
        sizeof(vk::vulkan_rank_lut),
        exact_minimizer_count,
        0u,
        vk::gpu_sketch_purpose::target_index_build,
        kmer_size,
        window_size,
        letters_per_unit,
        65'535u);
    REQUIRE(capacity_plan.is_valid);
    REQUIRE(exact_plan.is_valid);
    REQUIRE(exact_plan.tiles.tile_count == 1'562'500u);
    REQUIRE(exact_plan.tiles.scan_block_count == 6'104u);
    constexpr std::uint32_t max_dispatch_x = 65'535u;
    const std::uint32_t raw_element_groups =
        bioseq::common::ceil_div(exact_minimizer_count, 128u);
    const std::uint32_t raw_scan_block_groups =
        bioseq::common::ceil_div(
            exact_minimizer_count, vk::k_prefix_scan_block_size);
    REQUIRE(raw_element_groups == 728'976u);
    REQUIRE(raw_scan_block_groups == 364'488u);
    REQUIRE(raw_element_groups > max_dispatch_x);
    REQUIRE(raw_scan_block_groups > max_dispatch_x);
    REQUIRE(vk::grid_stride_dispatch_group_count(
                exact_minimizer_count, 128u, max_dispatch_x)
            == max_dispatch_x);
    REQUIRE(vk::grid_stride_dispatch_group_count(
                raw_scan_block_groups, 1u, max_dispatch_x)
            == max_dispatch_x);
    REQUIRE(capacity_plan.admission_bytes > safe_memory_ceiling);
    REQUIRE(exact_plan.admission_bytes < safe_memory_ceiling);
    REQUIRE(exact_plan.standalone_device_bytes
            < capacity_plan.standalone_device_bytes);

    const std::uint64_t adopted_resident_bytes =
        bioseq::parallelism::saturating_add(
            bioseq::parallelism::saturating_mul(
                exact_minimizer_count, 8u),
            bioseq::parallelism::saturating_mul(
                distinct_minimizer_count, 16u));
    REQUIRE(adopted_resident_bytes == 1'870'460'992u);
    REQUIRE(adopted_resident_bytes < exact_count_stream_floor);
}

TEST_CASE("vulkan minimizer: split admission retains exact device and host bytes",
          "[component:bioseq][layer:unit][feature:minimizer][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED(
            "hardware Vulkan unavailable; explicit split-admission hardware "
            "validation not executed");
        return;
    }

    using p_minimizer_type = minimap_sketch_data;
    using seq_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    constexpr std::size_t window_size = 10u;
    constexpr std::size_t kmer_size = 15u;

    auto& runtime = vk::runtime::instance();
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    const std::uint64_t baseline = admission.reserved_bytes();
    const std::uint64_t cache_metadata_before =
        runtime.pipeline_cache_metadata_reserved_bytes();
    struct restore_cap_guard {
        bioseq::parallelism::memory_admission_controller& admission;
        std::uint64_t previous = 0u;
        ~restore_cap_guard() { admission.set_index_memory_cap_bytes(previous); }
    } restore{admission, previous_cap};

    constexpr std::size_t letter_count = 1'000'000u;
    std::vector<seq::dna4> test_data;
    test_data.reserve(letter_count);
    std::uint64_t lcg = 0x243f6a8885a308d3ULL;
    for (std::size_t i = 0; i < letter_count; ++i) {
        lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
        seq::dna4 value{};
        value.assign_rank(static_cast<std::uint8_t>((lcg >> 33u) & 3u));
        test_data.push_back(value);
    }

    auto sequence = compact_sequence_range<seq_type>(test_data);
    const vk::sequence_upload_view upload =
        vk::make_sequence_upload_view(sequence);
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(sequence);
    const auto sequence_view =
        bioseq::storage::make_lazy_sequence_view(sequence_owner);
    const std::uint64_t max_minimizers =
        vk::estimate_vulkan_minimizer_capacity(
            upload.bp_size,
            window_size);

    const auto dispatch_admission_bytes = [
        &runtime,
        &upload,
        max_minimizers,
        window_size,
        kmer_size]() {
        const std::uint64_t resident_requested_bytes =
            vk::vulkan_sketch_resident_bytes<p_minimizer_type>(max_minimizers);
        const vk::gpu_sketch_memory_plan plan =
            vk::make_gpu_sketch_memory_plan(
                upload.bp_size,
                upload.padded_byte_size,
                sizeof(vk::vulkan_rank_lut),
                max_minimizers,
                resident_requested_bytes,
                vk::gpu_sketch_purpose::minimizer_words,
                static_cast<std::uint32_t>(kmer_size),
                static_cast<std::uint32_t>(window_size),
                upload.letters_per_unit,
                runtime.features().max_workgroup_count_x);
        REQUIRE(plan.is_valid);
        const auto allocation_requirements =
            runtime.sketch_batch_allocation_requirements(
                upload.padded_byte_size,
                plan.tiles.tile_count,
                static_cast<std::uint32_t>(
                    vk::minimizer_wire_traits<p_minimizer_type>::k_word_kind),
                vk::minimizer_wire_traits<
                    p_minimizer_type>::k_words32_per_minimizer,
                max_minimizers);
        REQUIRE(allocation_requirements.valid);
        const std::uint64_t physical_arena_bytes =
            runtime.gpu_arena_allocation_requirement(
                plan.arena_scaffold_bytes);
        const std::uint64_t host_transient_headroom_bytes =
            vk::vulkan_exact_host_allocation_admission_bytes(
                plan.count_submit_host_transient_bytes);
        return vk::vulkan_sketch_dispatch_admission_bytes(
            physical_arena_bytes,
            host_transient_headroom_bytes,
            0u,
            allocation_requirements.pessimistic_output_allocation_bytes);
    };

    const std::uint64_t initial_dispatch_peak = dispatch_admission_bytes();
    const std::uint64_t pessimistic_initial_host_bytes =
        bioseq::parallelism::saturating_mul(
            bioseq::parallelism::saturating_mul(
                max_minimizers,
                static_cast<std::uint64_t>(sizeof(p_minimizer_type))),
            bioseq::parallelism::k_vector_preallocation_headroom_factor);
    const std::uint64_t permissive_cap =
        bioseq::parallelism::saturating_add(
            baseline,
            bioseq::parallelism::saturating_add(
                initial_dispatch_peak,
                pessimistic_initial_host_bytes));
    admission.set_index_memory_cap_bytes(permissive_cap);

    auto calibration = vk::collect_minimizers_vulkan_result<p_minimizer_type>(
        sequence_view,
        window_size,
        kmer_size);
    REQUIRE(calibration.has_value());
    REQUIRE(calibration->device != nullptr);
    REQUIRE(calibration->host_permit.has_value());
    REQUIRE_FALSE(calibration->host_minimizers.empty());
    REQUIRE(calibration->arena_trimmed_before_host_materialization);
    REQUIRE_FALSE(runtime.reusable_gpu_arena().device_buffer().is_valid());
    REQUIRE_FALSE(runtime.reusable_gpu_arena().upload_buffer().is_valid());
    REQUIRE_FALSE(runtime.reusable_gpu_arena().download_buffer().is_valid());

    const std::uint64_t actual_device_allocation_bytes =
        calibration->device->words.allocation_bytes;
    const std::uint64_t exact_host_size_bytes =
        static_cast<std::uint64_t>(calibration->host_minimizers.size())
        * sizeof(p_minimizer_type);
    const std::uint64_t exact_host_headroom_bytes =
        bioseq::parallelism::saturating_mul(
            exact_host_size_bytes,
            bioseq::parallelism::k_vector_preallocation_headroom_factor);
    calibration.reset();
    const std::uint64_t cache_metadata_after =
        runtime.pipeline_cache_metadata_reserved_bytes();
    // Pipeline cache entries are retained by the process runtime; query result
    // device/host ownership must still be gone when its result is destroyed.
    REQUIRE(cache_metadata_after > 0u);
    REQUIRE(cache_metadata_after >= cache_metadata_before);
    const std::uint64_t cache_metadata_delta =
        cache_metadata_after - cache_metadata_before;
    REQUIRE(admission.reserved_bytes()
            == bioseq::parallelism::saturating_add(
                baseline, cache_metadata_delta));
    const std::uint64_t operation_baseline = admission.reserved_bytes();

    // Recompute after calibration so pipeline-cache metadata and allocator
    // requirements reflect the live runtime. The request-scoped Vulkan arena
    // itself must already be physically empty.
    const std::uint64_t dispatch_peak = dispatch_admission_bytes();
    const std::uint64_t host_materialization_peak =
        vk::vulkan_sketch_host_materialization_admission_bytes(
            actual_device_allocation_bytes,
            exact_host_headroom_bytes);
    const std::uint64_t split_admission_peak =
        std::max(dispatch_peak, host_materialization_peak);
    // Compare both lifetime models against the same allocator-rounded dispatch
    // peak. The Vulkan arena is persistent and may have an arbitrary physical
    // high-water, so reconstructing a peak from logical bytes would undercount.
    // The simultaneous model keeps the pessimistic host result alive during
    // dispatch; the split model materializes the exact result after dispatch.
    const std::uint64_t simultaneous_reservation_peak =
        bioseq::parallelism::saturating_add(
            dispatch_peak,
            pessimistic_initial_host_bytes);
    REQUIRE(split_admission_peak < simultaneous_reservation_peak);

    admission.set_index_memory_cap_bytes(
        bioseq::parallelism::saturating_add(
            operation_baseline, split_admission_peak));
    bioseq::parallelism::memory_admission_failure simultaneous_failure{};
    auto simultaneous_permit = admission.try_reserve(
        simultaneous_reservation_peak,
        &simultaneous_failure);
    REQUIRE_FALSE(simultaneous_permit.has_value());
    REQUIRE(simultaneous_failure.reason
            == bioseq::parallelism::memory_admission_failure_reason::
                budget_exhausted);

    auto result = vk::collect_minimizers_vulkan_result<p_minimizer_type>(
        sequence_view,
        window_size,
        kmer_size);
    REQUIRE(result.has_value());
    REQUIRE(result->dispatch_token.is_internally_consistent());
    REQUIRE(result->device != nullptr);
    REQUIRE(result->host_permit.has_value());
    REQUIRE(result->arena_trimmed_before_host_materialization);
    REQUIRE_FALSE(runtime.reusable_gpu_arena().device_buffer().is_valid());
    REQUIRE_FALSE(runtime.reusable_gpu_arena().upload_buffer().is_valid());
    REQUIRE_FALSE(runtime.reusable_gpu_arena().download_buffer().is_valid());

    const std::uint64_t exact_host_capacity_bytes =
        static_cast<std::uint64_t>(result->host_minimizers.capacity())
        * sizeof(p_minimizer_type);
    REQUIRE(result->device->permit.bytes()
            == result->device->words.allocation_bytes);
    REQUIRE(result->host_permit->bytes() == exact_host_capacity_bytes);
    REQUIRE(admission.reserved_bytes()
            == operation_baseline + result->device->words.allocation_bytes
                + exact_host_capacity_bytes);

    const auto scalar = cpu_sketch<p_minimizer_type>(
        test_data,
        window_size,
        kmer_size);
    require_gpu_minimizer_superset_with_bounded_tile_extras(
        scalar,
        result->host_minimizers,
        vk::make_sketch_tile_plan(
            test_data.size(),
            static_cast<std::uint32_t>(kmer_size),
            static_cast<std::uint32_t>(window_size),
            upload.letters_per_unit,
            runtime.features().max_workgroup_count_x),
        static_cast<std::uint32_t>(kmer_size),
        static_cast<std::uint32_t>(window_size));

    result.reset();
    REQUIRE(admission.reserved_bytes() == operation_baseline);
}

template <class p_alphabet>
static void require_exact_device_only_batch_parity(
    std::vector<p_alphabet> first_data,
    std::vector<p_alphabet> second_data)
{
    using p_minimizer_type = minimap_sketch_data;
    using seq_type = sequence_custom_alphabet<p_alphabet, std::uint64_t>;
    constexpr std::size_t window_size = 10u;
    constexpr std::size_t kmer_size = 15u;

    auto first_sequence = compact_sequence_range<seq_type>(first_data);
    auto second_sequence = compact_sequence_range<seq_type>(second_data);
    const std::array<vk::sequence_upload_view, 2> uploads{
        vk::make_sequence_upload_view(first_sequence),
        vk::make_sequence_upload_view(second_sequence),
    };
    if constexpr (std::same_as<p_alphabet, seq::dna4>) {
        // Batch tiles carry their own logical bp_count and sequence-word
        // offset.  Tail padding is therefore allowed to differ between
        // records and must not cause the whole GPU batch to decline.
        REQUIRE(uploads[0].padding_letters != uploads[1].padding_letters);
    }
    const auto rank_lut =
        vk::make_vulkan_rank_lut<decltype(first_sequence)>();
    const std::span<const vk::sequence_upload_view> upload_span{
        uploads.data(), uploads.size()};

    auto host_and_device =
        vk::collect_minimap_sketch_data_vulkan_upload_batch_result(
            upload_span,
            rank_lut,
            window_size,
            kmer_size,
            vk::vulkan_sketch_host_materialization::host_and_device);
    auto device_only =
        vk::collect_minimap_sketch_data_vulkan_upload_batch_result(
            upload_span,
            rank_lut,
            window_size,
            kmer_size,
            vk::vulkan_sketch_host_materialization::device_only);

    REQUIRE(host_and_device.has_value());
    REQUIRE(device_only.has_value());
    REQUIRE(host_and_device->dispatch_stats.is_internally_consistent());
    REQUIRE(host_and_device->dispatch_stats.materialization()
            == vk::vulkan_sketch_host_materialization::host_and_device);
    REQUIRE(host_and_device->dispatch_stats.count_dispatches() == 1u);
    REQUIRE(host_and_device->dispatch_stats.prefix_scan_sequences() == 1u);
    REQUIRE(host_and_device->dispatch_stats.prefix_scan_dispatches() == 3u);
    REQUIRE(host_and_device->dispatch_stats.emit_dispatches() == 1u);
    REQUIRE(host_and_device->dispatch_stats.completed_roundtrips() == 1u);
    REQUIRE(host_and_device->materialization
            == vk::vulkan_sketch_host_materialization::host_and_device);
    REQUIRE_FALSE(host_and_device->host_minimizers.empty());
    REQUIRE(device_only->dispatch_stats.is_internally_consistent());
    REQUIRE(device_only->dispatch_stats.materialization()
            == vk::vulkan_sketch_host_materialization::device_only);
    REQUIRE(device_only->dispatch_stats.count_dispatches() == 1u);
    REQUIRE(device_only->dispatch_stats.prefix_scan_sequences() == 1u);
    REQUIRE(device_only->dispatch_stats.prefix_scan_dispatches() == 3u);
    REQUIRE(device_only->dispatch_stats.emit_dispatches() == 1u);
    REQUIRE(device_only->dispatch_stats.completed_roundtrips() == 2u);
    REQUIRE(device_only->materialization
            == vk::vulkan_sketch_host_materialization::device_only);
    REQUIRE(device_only->host_minimizers.empty());

    REQUIRE(host_and_device->device != nullptr);
    REQUIRE(device_only->device != nullptr);
    REQUIRE(host_and_device->memory_retention != nullptr);
    REQUIRE(device_only->memory_retention != nullptr);
    REQUIRE(host_and_device->device->batch_memory_retention
            == host_and_device->memory_retention);
    REQUIRE(device_only->device->batch_memory_retention
            == device_only->memory_retention);
    REQUIRE(host_and_device->device->capacity
            >= host_and_device->device->count);
    const std::uint64_t host_vector_capacity_bytes =
        static_cast<std::uint64_t>(host_and_device->host_minimizers.capacity())
        * sizeof(p_minimizer_type);
    const std::uint64_t host_query_record_capacity_bytes =
        static_cast<std::uint64_t>(host_and_device->query_records.capacity())
        * sizeof(vk::gpu_seed_query_record_wire);
    REQUIRE(host_and_device->memory_retention->permit.bytes()
            == host_and_device->device->words.allocation_bytes
                + host_vector_capacity_bytes
                + host_query_record_capacity_bytes);
    const std::uint64_t device_query_record_capacity_bytes =
        static_cast<std::uint64_t>(device_only->query_records.capacity())
        * sizeof(vk::gpu_seed_query_record_wire);
    const std::uint64_t device_authority_capacity_bytes =
        device_only->query_authority.owned_capacity_bytes();
    REQUIRE(device_authority_capacity_bytes
            == uploads.size()
                * (sizeof(vk::gpu_seed_query_record_wire)
                    + sizeof(std::uint32_t)));
    REQUIRE(device_only->memory_retention->permit.bytes()
            == device_only->device->words.allocation_bytes
                + device_query_record_capacity_bytes
                + device_authority_capacity_bytes);
    REQUIRE(device_only->device_minimizer_count
            == host_and_device->host_minimizers.size());
    REQUIRE(device_only->device->count
            == device_only->device_minimizer_count);
    REQUIRE(device_only->device->capacity
            == device_only->device_minimizer_count);
    const std::uint64_t exact_device_bytes = std::max<std::uint64_t>(
        vk::minimizer_wire_traits<p_minimizer_type>::device_bytes_for_count(
            device_only->device_minimizer_count),
        4u);
    REQUIRE(device_only->device->words.bytes == exact_device_bytes);

    REQUIRE(device_only->query_records.size() == uploads.size());
    REQUIRE(host_and_device->query_records.size() == uploads.size());
    std::uint64_t query_cursor = 0u;
    for (std::size_t query_index = 0; query_index < uploads.size(); ++query_index) {
        const auto& actual = device_only->query_records[query_index];
        const auto& expected = host_and_device->query_records[query_index];
        REQUIRE(actual.minimizer_offset == query_cursor);
        REQUIRE(actual.minimizer_offset == expected.minimizer_offset);
        REQUIRE(actual.minimizer_count == expected.minimizer_count);
        REQUIRE(actual.query_len == expected.query_len);
        REQUIRE(actual.flags == expected.flags);
        query_cursor += actual.minimizer_count;
    }
    REQUIRE(query_cursor == device_only->device_minimizer_count);

    // Test-only readback: production device-only mode never creates a host
    // hash vector, but this focused oracle downloads its exact resident bytes
    // and checks byte/order parity with the ordinary materialized call.
    const std::size_t minimizer_count = static_cast<std::size_t>(
        device_only->device_minimizer_count);
    std::vector<std::byte> downloaded_bytes(
        minimizer_count * sizeof(p_minimizer_type));
    vk::runtime::instance().download_slice(
        device_only->device->words,
        0u,
        std::span<std::byte>{
            downloaded_bytes.data(), downloaded_bytes.size()});
    std::vector<p_minimizer_type> downloaded(minimizer_count);
    if (!downloaded_bytes.empty()) {
        std::memcpy(
            downloaded.data(),
            downloaded_bytes.data(),
            downloaded_bytes.size());
    }
    REQUIRE(downloaded.size() == host_and_device->host_minimizers.size());
    for (std::size_t i = 0; i < downloaded.size(); ++i) {
        REQUIRE(downloaded[i].raw_value()
                == host_and_device->host_minimizers[i].raw_value());
    }
}

TEST_CASE("vulkan minimizer: dna4 batch supports exact device-only materialization",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    std::vector<seq::dna4> second{
        dna4_fixture.begin() + 17,
        dna4_fixture.end() - 11};
    require_exact_device_only_batch_parity(dna4_fixture, std::move(second));
}

TEST_CASE("vulkan minimizer: dna5 batch supports exact device-only materialization",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    require_exact_device_only_batch_parity(dna5_with_t, dna5_with_n);
}

TEST_CASE("vulkan minimizer: device-only zero emission is dispatched and sealed",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    constexpr std::size_t window_size = 10u;
    constexpr std::size_t kmer_size = 15u;

    std::vector<seq::dna5> too_short = "NNNNNNNNNNNNNN"_dna5;
    auto too_short_sequence = compact_sequence_range<seq_type>(too_short);
    const std::array<vk::sequence_upload_view, 1> too_short_uploads{
        vk::make_sequence_upload_view(too_short_sequence),
    };
    const auto rank_lut =
        vk::make_vulkan_rank_lut<decltype(too_short_sequence)>();
    auto nondispatchable =
        vk::collect_minimap_sketch_data_vulkan_upload_batch_result(
            std::span<const vk::sequence_upload_view>{
                too_short_uploads.data(), too_short_uploads.size()},
            rank_lut,
            window_size,
            kmer_size,
            vk::vulkan_sketch_host_materialization::device_only);
    REQUIRE_FALSE(nondispatchable.has_value());

    std::vector<seq::dna5> all_n =
        "NNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNN"
        "NNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNN"_dna5;
    REQUIRE(all_n.size() >= kmer_size);
    auto all_n_sequence = compact_sequence_range<seq_type>(all_n);
    const std::array<vk::sequence_upload_view, 1> all_n_uploads{
        vk::make_sequence_upload_view(all_n_sequence),
    };
    const auto all_n_tile_plan = vk::make_sketch_tile_plan(
        all_n.size(),
        static_cast<std::uint32_t>(kmer_size),
        static_cast<std::uint32_t>(window_size),
        all_n_uploads[0].letters_per_unit,
        vk::runtime::instance().features().max_workgroup_count_x);
    REQUIRE(all_n_tile_plan.is_dispatchable);
    const std::uint64_t pessimistic_count =
        vk::estimate_vulkan_minimizer_capacity(
            all_n.size(),
            window_size);
    const auto allocation_requirements =
        vk::runtime::instance().sketch_batch_allocation_requirements(
            all_n_uploads[0].padded_byte_size,
            all_n_tile_plan.tile_count,
            static_cast<std::uint32_t>(
                vk::minimizer_wire_traits<minimap_sketch_data>::k_word_kind),
            vk::minimizer_wire_traits<
                minimap_sketch_data>::k_words32_per_minimizer,
            pessimistic_count);
    REQUIRE(allocation_requirements.valid);
    REQUIRE(allocation_requirements.arena_requested_bytes > 0u);
    REQUIRE(allocation_requirements.arena_allocation_bytes
            >= allocation_requirements.arena_requested_bytes);
    REQUIRE(allocation_requirements.pessimistic_output_requested_bytes
            == std::max<std::uint64_t>(
                vk::minimizer_wire_traits<
                    minimap_sketch_data>::device_bytes_for_count(
                        pessimistic_count),
                4u));
    REQUIRE(allocation_requirements.pessimistic_output_allocation_bytes
            >= allocation_requirements.pessimistic_output_requested_bytes);
    auto result =
        vk::collect_minimap_sketch_data_vulkan_upload_batch_result(
            std::span<const vk::sequence_upload_view>{
                all_n_uploads.data(), all_n_uploads.size()},
            rank_lut,
            window_size,
            kmer_size,
            vk::vulkan_sketch_host_materialization::device_only);

    REQUIRE(result.has_value());
    REQUIRE_FALSE(vk::is_runtime_metal_archive_configuration_installed());
    REQUIRE(result->query_authority.does_cover_device_output());
    // Structural coverage cannot mint the causal immutable-pipeline stats
    // required by the persisted grouped-SoA consumer.
    REQUIRE_FALSE(result->query_authority.is_internally_consistent());
    REQUIRE(result->dispatch_stats.is_internally_consistent());
    REQUIRE(result->dispatch_stats.materialization()
            == vk::vulkan_sketch_host_materialization::device_only);
    REQUIRE(result->dispatch_stats.count_dispatches() == 1u);
    REQUIRE(result->dispatch_stats.prefix_scan_sequences() == 1u);
    REQUIRE(result->dispatch_stats.prefix_scan_dispatches() == 3u);
    REQUIRE(result->dispatch_stats.emit_dispatches() == 1u);
    REQUIRE(result->dispatch_stats.completed_roundtrips() == 2u);
    REQUIRE(result->query_records.size() == 1u);
    REQUIRE(result->query_records[0].minimizer_offset == 0u);
    REQUIRE(result->query_records[0].minimizer_count == 0u);
    REQUIRE(result->query_records[0].query_len == all_n.size());
    REQUIRE(result->query_records[0].flags == 0u);
    REQUIRE(result->host_minimizers.empty());
    REQUIRE(result->host_minimizers.capacity() == 0u);
    REQUIRE(result->device != nullptr);
    REQUIRE(result->memory_retention != nullptr);
    REQUIRE(result->device->batch_memory_retention
            == result->memory_retention);
    REQUIRE(result->device_minimizer_count == 0u);
    REQUIRE(result->device->count == 0u);
    REQUIRE(result->device->capacity == 0u);
    REQUIRE(result->device->words.bytes == 4u);
    REQUIRE(result->query_authority.owned_capacity_bytes()
            == sizeof(vk::gpu_seed_query_record_wire)
                + sizeof(std::uint32_t));
    REQUIRE(result->memory_retention->permit.bytes()
            == result->device->words.allocation_bytes
                + static_cast<std::uint64_t>(
                    result->query_records.capacity())
                    * sizeof(vk::gpu_seed_query_record_wire)
                + result->query_authority.owned_capacity_bytes());
    REQUIRE(result->memory_retention->permit.bytes()
            >= result->device->words.allocation_bytes);

    std::vector<std::byte> downloaded;
    vk::runtime::instance().download_slice(
        result->device->words,
        0u,
        std::span<std::byte>{downloaded.data(), downloaded.size()});
    REQUIRE(downloaded.empty());

    // A malformed post-submit authority record must fail closed without
    // leaving the shared arena cursor live for the next independent request.
    std::vector<std::byte> packed_sequence(
        static_cast<std::size_t>(all_n_uploads[0].padded_byte_size));
    std::memcpy(
        packed_sequence.data(),
        all_n_uploads[0].bytes,
        static_cast<std::size_t>(all_n_uploads[0].byte_size));
    std::vector<vk::gpu_sketch_tile_wire> tiles;
    tiles.reserve(all_n_tile_plan.tile_count);
    const std::uint32_t overlap = vk::sketch_replay_overlap_bp(
        static_cast<std::uint32_t>(kmer_size),
        static_cast<std::uint32_t>(window_size));
    for (std::uint32_t tile = 0u;
         tile < all_n_tile_plan.tile_count; ++tile) {
        const std::uint32_t lo = tile * all_n_tile_plan.tile_bp;
        const std::uint32_t hi = std::min<std::uint32_t>(
            static_cast<std::uint32_t>(all_n.size()),
            (tile + 1u) * all_n_tile_plan.tile_bp);
        tiles.push_back(vk::gpu_sketch_tile_wire{
            .seq_word_offset = 0u,
            .bp_count = static_cast<std::uint32_t>(all_n.size()),
            .replay_lo = lo > overlap ? lo - overlap : 0u,
            .tile_lo = lo,
            .tile_hi = hi,
            .out_base = 0u,
        });
    }
    const std::array malformed_layouts{
        vk::gpu_sketch_query_layout_wire{
            .begin = 0u,
            .count = static_cast<std::uint32_t>(tiles.size()),
            .query_len = 0u,
        },
    };
    const vk::vulkan_sketch_params malformed_params{
        .bp_size = static_cast<std::uint32_t>(all_n.size()),
        .window_size = static_cast<std::uint32_t>(window_size),
        .kmer_size = static_cast<std::uint32_t>(kmer_size),
        .hash_bits = vk::minimizer_wire_traits<
            minimap_sketch_data>::k_hash_bits,
        .hash_shift = vk::minimizer_wire_traits<
            minimap_sketch_data>::k_hash_shift,
        .pos_bits = vk::minimizer_wire_traits<
            minimap_sketch_data>::k_position_bits,
        .minimizer_word_kind = static_cast<std::uint32_t>(
            vk::minimizer_wire_traits<minimap_sketch_data>::k_word_kind),
        .minimizer_words32 = vk::minimizer_wire_traits<
            minimap_sketch_data>::k_words32_per_minimizer,
        .alphabet_kind = static_cast<std::uint32_t>(
            all_n_uploads[0].alphabet_kind),
        .unit_bits = all_n_uploads[0].unit_bits,
        .bits_per_char = all_n_uploads[0].bits_per_char,
        .letters_per_unit = all_n_uploads[0].letters_per_unit,
        .alphabet_size = all_n_uploads[0].alphabet_size,
        .seq_word_offset = 0u,
        .char_mask = all_n_uploads[0].char_mask,
        .padding_letters = all_n_uploads[0].padding_letters,
    };
    bool malformed_authority_rejected = false;
    try {
        (void)vk::runtime::instance().run_sketch_tile_batch_roundtrip(
            std::span<const std::byte>{
                packed_sequence.data(), packed_sequence.size()},
            rank_lut,
            malformed_params,
            std::span<const vk::gpu_sketch_tile_wire>{
                tiles.data(), tiles.size()},
            std::span<const vk::gpu_sketch_query_layout_wire>{
                malformed_layouts.data(), malformed_layouts.size()},
            pessimistic_count,
            vk::vulkan_sketch_host_materialization::device_only);
    } catch (const vk::vulkan_runtime_error& error) {
        malformed_authority_rejected = std::string_view{error.what()}
            == "gpu sketch query authority does not cover device output";
    }
    REQUIRE(malformed_authority_rejected);
    REQUIRE(vk::runtime::instance().reusable_gpu_arena().release_idle_capacity());
}

TEST_CASE("vulkan minimizer: dna4 parity equals scalar mm-sketch",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) { SUCCEED("no hardware Vulkan device; skipped"); return; }
    for (auto [w, k] : std::vector<std::pair<std::size_t, std::size_t>>{
             {10, 15}, {5, 7}, {10, 27}}) {
        const auto cpu = triples(cpu_sketch<minimap_sketch_data>(dna4_fixture, w, k));
        const auto gpu = triples(gpu_sketch<minimap_sketch_data>(dna4_fixture, w, k));
        ASSERT(!gpu.empty(),
               fmt::format("GPU produced no dna4 minimizers at (w={},k={})", w, k),
               w, k);
        ASSERT(cpu == gpu,
               fmt::format("dna4 GPU sketch != scalar at (w={},k={}): cpu={} gpu={}",
                           w, k, cpu.size(), gpu.size()),
               cpu.size(), gpu.size());
    }
}

TEST_CASE("vulkan minimizer: dna5 with T parity (T is nt4 code 3)",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) { SUCCEED("no hardware Vulkan device; skipped"); return; }
    constexpr std::uint32_t w = 10;
    constexpr std::uint32_t k = 15;
    const auto cpu = cpu_sketch<minimap_sketch_data>(dna5_with_t, w, k);
    const auto gpu = gpu_sketch<minimap_sketch_data>(dna5_with_t, w, k);
    ASSERT(!gpu.empty(), "GPU produced no dna5+T minimizers", gpu.size());
    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    require_gpu_minimizer_superset_with_bounded_tile_extras(
        cpu,
        gpu,
        vk::make_sketch_tile_plan(
            dna5_with_t.size(),
            k,
            w,
            compact_sequence_range<seq_type>::traits::letters_per_unit),
        k,
        w);
}

TEST_CASE("vulkan minimizer: dna5 with N parity (N resets the window)",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) { SUCCEED("no hardware Vulkan device; skipped"); return; }
    constexpr std::uint32_t w = 10;
    constexpr std::uint32_t k = 15;
    const auto cpu_values = cpu_sketch<minimap_sketch_data>(dna5_with_n, w, k);
    const auto gpu_values = gpu_sketch<minimap_sketch_data>(dna5_with_n, w, k);
    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    require_gpu_minimizer_superset_with_bounded_tile_extras(
        cpu_values,
        gpu_values,
        vk::make_sketch_tile_plan(
            dna5_with_n.size(),
            k,
            w,
            compact_sequence_range<seq_type>::traits::letters_per_unit),
        k,
        w);
    const auto gpu = triples(gpu_values);
    // No emitted minimizer's k-mer may span an N (decode the same range).
    const auto decoded = compact_sequence_range<seq_type>(dna5_with_n).to_string();
    std::vector<std::size_t> n_positions;
    for (std::size_t i = 0; i < decoded.size(); ++i)
        if (decoded[i] == 'N') n_positions.push_back(i);
    for (const auto& [last_pos, kmer, strand] : gpu) {
        const std::size_t lo = (last_pos + 1 >= k) ? (last_pos + 1 - k) : 0;
        for (const std::size_t np : n_positions) {
            ASSERT(np < lo || np > last_pos,
                   fmt::format("GPU minimizer at {} spans an N at {}", last_pos, np),
                   last_pos);
        }
    }
}

TEST_CASE("vulkan minimizer: dna5 tile-boundary extras are bounded",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) { SUCCEED("no hardware Vulkan device; skipped"); return; }

    constexpr std::size_t w = 10;
    constexpr std::size_t k = 15;
    constexpr std::size_t letter_count = 21u * 700u;
    std::vector<seq::dna5> data;
    data.reserve(letter_count);
    std::uint64_t lcg = 0x6a09e667f3bcc909ULL;
    for (std::size_t i = 0; i < letter_count; ++i) {
        lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
        seq::dna5 value{};
        value.assign_rank(static_cast<std::uint8_t>((lcg >> 33) % 4u));
        data.push_back(value);
    }
    for (const std::size_t n_pos : std::array<std::size_t, 12>{
             240u, 251u, 252u, 253u, 503u, 504u,
             505u, 755u, 756u, 757u, 1007u, 1008u}) {
        data[n_pos].assign_rank(static_cast<std::uint8_t>(4u));
    }

    const auto cpu = cpu_sketch<minimap_sketch_data>(data, w, k);
    const auto gpu = gpu_sketch<minimap_sketch_data>(data, w, k);
    ASSERT(!gpu.empty(), "fused dna5 sketch produced no minimizers", gpu.size());
    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    require_gpu_minimizer_superset_with_bounded_tile_extras(
        cpu,
        gpu,
        vk::make_sketch_tile_plan(
            data.size(),
            k,
            w,
            compact_sequence_range<seq_type>::traits::letters_per_unit),
        k,
        w);
}

TEST_CASE("vulkan minimizer: generic alphabet functional superset",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) { SUCCEED("no hardware Vulkan device; skipped"); return; }
    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint8_t>;
    constexpr std::uint32_t w = 5;
    constexpr std::uint32_t k = 7;
    std::vector<seq::dna5> data = dna5_with_t;
    auto seq = compact_sequence_range<seq_type>(data);
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(seq);
    const auto sequence_view =
        bioseq::storage::make_lazy_sequence_view(sequence_owner);
    auto result = vk::collect_minimizers_vulkan_result<minimap_sketch_data>(
        sequence_view, w, k);
    REQUIRE(result.has_value());
    auto cpu = cpu_sketch<minimap_sketch_data>(data, w, k);
    require_gpu_minimizer_superset_with_bounded_tile_extras(
        cpu,
        result->host_minimizers,
        vk::make_sketch_tile_plan(
            data.size(),
            k,
            w,
            compact_sequence_range<seq_type>::traits::letters_per_unit),
        k,
        w);
}

TEST_CASE("vulkan minimizer: oversized k declines before dispatch (no throw)",
          "[component:bioseq][layer:unit][feature:seeding]") {
    // The pre-dispatch guards (k > 32, k*step_bits > 64, w > 256) return
    // std::nullopt so selected Vulkan callers can raise a hard decline before
    // dispatch. k=40 trips the k>32
    // guard for dna4 (step_bits=2).
    using seq_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    auto seq = compact_sequence_range<seq_type>(dna4_fixture);
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(seq);
    const auto sequence_view =
        bioseq::storage::make_lazy_sequence_view(sequence_owner);
    vk::vulkan_sketch_decline decline{};
    auto big_k = vk::collect_minimizers_vulkan_result<minimap_sketch_data>(
        sequence_view, 10, 40, &decline);
    ASSERT(!big_k.has_value(), "oversized k must decline (nullopt)", 0);
    REQUIRE(decline.reason == vk::vulkan_sketch_decline_reason::kmer_size_unsupported);
    REQUIRE(decline.kmer_size == 40u);
    auto big_w = vk::collect_minimizers_vulkan_result<minimap_sketch_data>(
        sequence_view, 300, 15, &decline);
    ASSERT(!big_w.has_value(), "oversized window must decline (nullopt)", 0);
    REQUIRE(decline.reason == vk::vulkan_sketch_decline_reason::window_size_unsupported);
    REQUIRE(decline.window_size == 300u);
}

TEST_CASE("vulkan minimizer: typed decline preserves admission snapshot and strict message",
          "[component:bioseq][layer:unit][feature:seeding]") {
    const vk::vulkan_sketch_decline decline{
        .reason = vk::vulkan_sketch_decline_reason::memory_admission_denied,
        .bp_size = 101u,
        .padded_sequence_bytes = 32u,
        .max_minimizers = 17u,
        .query_gate_words = 3u,
        .window_size = 10u,
        .kmer_size = 15u,
        .admission = {
            .reason = bioseq::parallelism::memory_admission_failure_reason::
                budget_exhausted,
            .requested_bytes = 701u,
            .reserved_bytes = 211u,
            .budget_ceiling_bytes = 503u,
            .allocatable_after_reservations_bytes = 292u,
            .process_footprint_bytes = 401u,
            .system_available_bytes = 601u,
            .system_total_bytes = 809u,
            .process_soft_cap_bytes = 307u,
        },
    };
    const std::string formatted = vk::format_vulkan_sketch_decline(decline);
    REQUIRE(formatted.find("reason=memory_admission_denied") != std::string::npos);
    REQUIRE(formatted.find("admission_reason=budget_exhausted") != std::string::npos);
    REQUIRE(formatted.find("required_bytes=701") != std::string::npos);
    REQUIRE(formatted.find("allocatable_after_reservations_bytes=292")
            != std::string::npos);
    REQUIRE(formatted.find("reserved_bytes=211") != std::string::npos);
    REQUIRE(formatted.find("budget_ceiling_bytes=503") != std::string::npos);
    REQUIRE(formatted.find("process_footprint_bytes=401") != std::string::npos);
    REQUIRE(formatted.find("system_available_bytes=601") != std::string::npos);
    REQUIRE(formatted.find("system_total_bytes=809") != std::string::npos);
    REQUIRE(formatted.find("process_soft_cap_bytes=307") != std::string::npos);

    bool caught = false;
    try {
        vk::throw_vulkan_strict_decline("typed target-index fixture", decline);
    } catch (const vk::vulkan_runtime_error& error) {
        caught = true;
        const std::string message = error.what();
        REQUIRE(message.find("typed target-index fixture selected")
                != std::string::npos);
        REQUIRE(message.find("reason=memory_admission_denied")
                != std::string::npos);
        REQUIRE(message.find("admission_reason=budget_exhausted")
                != std::string::npos);
        REQUIRE(message.find("system_total_bytes=809") != std::string::npos);
    }
    REQUIRE(caught);
}

TEST_CASE("vulkan minimizer: only reducible declines permit segment retry",
          "[component:bioseq][layer:unit][feature:seeding]") {
    STATIC_REQUIRE(vk::vulkan_sketch_decline_allows_segment_split(
        vk::vulkan_sketch_decline_reason::memory_admission_denied));
    STATIC_REQUIRE(vk::vulkan_sketch_decline_allows_segment_split(
        vk::vulkan_sketch_decline_reason::output_capacity_overflow));
    STATIC_REQUIRE(vk::vulkan_sketch_decline_allows_segment_split(
        vk::vulkan_sketch_decline_reason::sequence_size_exceeds_u32));
    STATIC_REQUIRE_FALSE(vk::vulkan_sketch_decline_allows_segment_split(
        vk::vulkan_sketch_decline_reason::hardware_device_unavailable));
    STATIC_REQUIRE_FALSE(vk::vulkan_sketch_decline_allows_segment_split(
        vk::vulkan_sketch_decline_reason::kmer_size_unsupported));
    STATIC_REQUIRE_FALSE(vk::vulkan_sketch_decline_allows_segment_split(
        vk::vulkan_sketch_decline_reason::query_gate_exceeds_u32));
}

TEST_CASE("vulkan minimizer: target-index decline identity survives wrappers",
          "[component:bioseq][layer:unit][feature:index]") {
    STATIC_REQUIRE(vk::vulkan_target_index_failure_decline_reason(
        vk::gpu_index_build_failure::output_capacity_overflow)
        == vk::vulkan_sketch_decline_reason::output_capacity_overflow);
    STATIC_REQUIRE(vk::vulkan_target_index_failure_decline_reason(
        vk::gpu_index_build_failure::memory_plan_invalid)
        == vk::vulkan_sketch_decline_reason::memory_plan_invalid);
    STATIC_REQUIRE(vk::vulkan_target_index_failure_decline_reason(
        vk::gpu_index_build_failure::device_buffer_allocation_failed)
        == vk::vulkan_sketch_decline_reason::device_allocation_failed);
    STATIC_REQUIRE(vk::vulkan_target_index_failure_decline_reason(
        vk::gpu_index_build_failure::count_peak_memory_admission_denied)
        == vk::vulkan_sketch_decline_reason::memory_admission_denied);
    STATIC_REQUIRE(vk::vulkan_target_index_failure_decline_reason(
        vk::gpu_index_build_failure::execution_peak_memory_admission_denied)
        == vk::vulkan_sketch_decline_reason::memory_admission_denied);
    STATIC_REQUIRE(vk::vulkan_target_index_failure_decline_reason(
        vk::gpu_index_build_failure::index_invariant_failed)
        == vk::vulkan_sketch_decline_reason::
            target_index_build_invariant_failed);

    using seq_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    auto sequence = compact_sequence_range<seq_type>(dna4_fixture);
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(sequence);
    const auto sequence_view =
        bioseq::storage::make_lazy_sequence_view(sequence_owner);
    vk::vulkan_sketch_decline decline{};
    auto built = vk::build_gpu_index_on_device<minimap_sketch_data>(
        sequence_view,
        10u,
        40u,
        {},
        &decline);
    REQUIRE_FALSE(built.has_value());
    REQUIRE(decline.reason
            == vk::vulkan_sketch_decline_reason::kmer_size_unsupported);
    REQUIRE(decline.bp_size == sequence.bp_size());
    REQUIRE(decline.window_size == 10u);
    REQUIRE(decline.kmer_size == 40u);
}

TEST_CASE("vulkan minimizer: target-index count admission covers persistent arena high-water",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto& runtime = vk::runtime::instance();
    constexpr std::uint64_t high_water_request = std::uint64_t{16} << 20;
    REQUIRE(runtime.reusable_gpu_arena().ensure_capacity(
        high_water_request, 0u, 0u));
    const std::uint64_t high_water =
        runtime.reusable_gpu_arena().device_buffer().allocation_bytes;
    REQUIRE(high_water >= high_water_request);
    REQUIRE(runtime.gpu_arena_allocation_requirement(1024u) >= high_water);

    auto mutable_gate_words = std::make_shared<std::vector<std::uint32_t>>(
        vk::k_gpu_query_gate_word_count,
        (std::numeric_limits<std::uint32_t>::max)());
    const std::shared_ptr<const std::vector<std::uint32_t>> gate_words =
        std::move(mutable_gate_words);
    const auto gate_identity =
        vk::canonical_vulkan_sketch_query_gate_identity(
            std::span<const std::uint32_t>{*gate_words},
            vk::k_gpu_query_gate_mask,
            gate_words);
    const vk::vulkan_sketch_query_gate gate{
        std::span<const std::uint32_t>{*gate_words},
        vk::k_gpu_query_gate_mask,
        gate_identity,
        true};
    using seq_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    auto sequence = compact_sequence_range<seq_type>(dna4_fixture);
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(sequence);
    const auto sequence_view =
        bioseq::storage::make_lazy_sequence_view(sequence_owner);
    vk::vulkan_sketch_decline decline{};
    auto built = vk::build_gpu_index_on_device<minimap_sketch_data>(
        sequence_view, 10u, 15u, gate, &decline);
    INFO(vk::format_vulkan_sketch_decline(decline));
    REQUIRE(built.has_value());
    REQUIRE(built->valid);
    REQUIRE(built->ownership.count_submit_admissions == 1u);
    REQUIRE(built->ownership.execution_peak_admissions == 1u);
    REQUIRE(built->ownership.idle_arena_trim_attempts_before_count == 1u);
    REQUIRE(built->ownership.idle_arena_trim_successes_before_count == 1u);
    REQUIRE(built->ownership.idle_arena_released_bytes_before_count
            >= high_water);
    REQUIRE(built->ownership.pretrim_peak_observed_bytes >= high_water);
    REQUIRE(built->ownership.peak_observed_bytes
            >= built->ownership.pretrim_peak_observed_bytes);
    REQUIRE(built->ownership.direct_sequence_arena_copies == 1u);
    REQUIRE(built->ownership.host_sequence_capacity_releases_before_execution
            == 0u);
    REQUIRE(built->ownership.direct_tile_arena_fills == 1u);
    REQUIRE(built->ownership.count_submit_host_transient_bytes == 0u);
    REQUIRE(built->ownership.execution_submit_host_transient_bytes == 0u);
    REQUIRE(built->ownership.host_transient_released_before_execution_bytes
            == built->ownership.host_tile_staging_elided_bytes);
    REQUIRE(built->ownership.exact_count_dispatches == 1u);
    REQUIRE(built->ownership.exact_count_prefix_scan_dispatches == 3u);
    REQUIRE(built->ownership.exact_count_control_readbacks == 1u);
    REQUIRE(built->ownership.deterministic_tile_emit_dispatches == 1u);
    REQUIRE(built->ownership.atomic_append_dispatches == 0u);
    REQUIRE(built->ownership.emit_validation_control_readbacks == 1u);
    REQUIRE(built->ownership.count_emit_parity_verified == 1u);
    REQUIRE(built->ownership.arena_trimmed_before_permit_shrink == 1u);
    REQUIRE(built->ownership.gate_count_dispatches == 1u);
    REQUIRE(built->ownership.gated_emit_dispatches == 1u);
    REQUIRE(built->ownership.ungated_emit_dispatches == 0u);
    REQUIRE(built->ownership.retained_minimizer_count > 0u);
    REQUIRE(built->ownership.distinct_minimizer_count > 0u);
    REQUIRE(built->ownership.peak_observed_bytes
            <= built->ownership.peak_admitted_bytes);
}

TEST_CASE("vulkan minimizer: exact ungated count and seed arena trim preserve adopted index",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    auto& runtime = vk::runtime::instance();
    auto& arena = runtime.reusable_gpu_arena();

    // A live slice makes trim fail closed.  Once its mark is rewound, trim
    // destroys the backing allocation.  This directly guards the cursor rule
    // used by the request-boundary cleanup below.
    REQUIRE(arena.release_idle_capacity());
    constexpr std::uint64_t arena_probe_bytes = std::uint64_t{1} << 20u;
    REQUIRE(arena.ensure_capacity(arena_probe_bytes, 0u, 0u));
    const auto arena_mark = arena.mark();
    const auto arena_probe = arena.allocate_device(4096u, 16u);
    REQUIRE(arena_probe.is_valid());
    const auto arena_handle = arena.device_buffer().buffer;
    REQUIRE_FALSE(arena.release_idle_capacity());
    REQUIRE(arena.device_buffer().is_valid());
    REQUIRE(arena.device_buffer().buffer == arena_handle);
    arena.rewind(arena_mark);
    REQUIRE(arena.release_idle_capacity());
    REQUIRE_FALSE(arena.device_buffer().is_valid());

    // The same cleanup must run during stack unwinding, before a caller can
    // release or shrink its admission permit.
    bool exception_observed = false;
    try {
        vk::gpu_arena_trim_guard trim_on_exception{arena};
        REQUIRE(arena.ensure_capacity(arena_probe_bytes, 0u, 0u));
        const auto exception_probe = arena.allocate_device(4096u, 16u);
        REQUIRE(exception_probe.is_valid());
        throw std::runtime_error("intentional arena cleanup probe");
    } catch (const std::runtime_error&) {
        exception_observed = true;
    }
    REQUIRE(exception_observed);
    REQUIRE_FALSE(arena.device_buffer().is_valid());
    REQUIRE_FALSE(arena.upload_buffer().is_valid());
    REQUIRE_FALSE(arena.download_buffer().is_valid());

    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t baseline = admission.reserved_bytes();
    const std::uint64_t cache_metadata_before =
        runtime.pipeline_cache_metadata_reserved_bytes();
    using seq_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    auto sequence = compact_sequence_range<seq_type>(dna4_fixture);
    const vk::sequence_upload_view upload =
        vk::make_sequence_upload_view(sequence);
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(sequence);
    const auto sequence_view =
        bioseq::storage::make_lazy_sequence_view(sequence_owner);
    const vk::vulkan_sketch_params target_index_params{
        .bp_size = static_cast<std::uint32_t>(upload.bp_size),
        .window_size = 10u,
        .kmer_size = 15u,
        .hash_bits = vk::minimizer_wire_traits<minimap_sketch_data>::k_hash_bits,
        .hash_shift = vk::minimizer_wire_traits<minimap_sketch_data>::k_hash_shift,
        .pos_bits = vk::minimizer_wire_traits<minimap_sketch_data>::k_position_bits,
        .minimizer_word_kind = static_cast<std::uint32_t>(
            vk::minimizer_wire_traits<minimap_sketch_data>::k_word_kind),
        .minimizer_words32 = vk::minimizer_wire_traits<
            minimap_sketch_data>::k_words32_per_minimizer,
        .alphabet_kind = static_cast<std::uint32_t>(upload.alphabet_kind),
        .unit_bits = upload.unit_bits,
        .bits_per_char = upload.bits_per_char,
        .letters_per_unit = upload.letters_per_unit,
        .alphabet_size = upload.alphabet_size,
        .seq_word_offset = 0u,
        .char_mask = upload.char_mask,
        .padding_letters = upload.padding_letters,
    };
    runtime.prepare_target_index_pipelines(target_index_params);
    const std::uint64_t target_index_pipeline_metadata =
        runtime.pipeline_cache_metadata_reserved_bytes();
    REQUIRE(target_index_pipeline_metadata >= cache_metadata_before);
    runtime.prepare_target_index_pipelines(target_index_params);
    REQUIRE(runtime.pipeline_cache_metadata_reserved_bytes()
            == target_index_pipeline_metadata);
    const std::uint64_t pessimistic_capacity =
        vk::estimate_vulkan_minimizer_capacity(sequence.bp_size(), 10u);
    vk::vulkan_sketch_decline decline{};
    auto built = vk::build_gpu_index_on_device<minimap_sketch_data>(
        sequence_view, 10u, 15u, {}, &decline);
    INFO(vk::format_vulkan_sketch_decline(decline));
    REQUIRE(built.has_value());
    REQUIRE(built->valid);
    // The build's count/radix/mark/emit route must not discover any pipeline
    // metadata after the caller has acquired its transient allocation permit.
    REQUIRE(runtime.pipeline_cache_metadata_reserved_bytes()
            == target_index_pipeline_metadata);
    REQUIRE(built->ownership.count_submit_admissions == 1u);
    REQUIRE(built->ownership.execution_peak_admissions == 1u);
    REQUIRE(built->ownership.exact_count_dispatches == 1u);
    REQUIRE(built->ownership.exact_count_prefix_scan_dispatches == 3u);
    REQUIRE(built->ownership.exact_count_control_readbacks == 1u);
    REQUIRE(built->ownership.deterministic_tile_emit_dispatches == 1u);
    REQUIRE(built->ownership.atomic_append_dispatches == 0u);
    REQUIRE(built->ownership.emit_validation_control_readbacks == 1u);
    REQUIRE(built->ownership.count_emit_parity_verified == 1u);
    REQUIRE(built->ownership.arena_trimmed_before_permit_shrink == 1u);
    REQUIRE_FALSE(arena.device_buffer().is_valid());
    REQUIRE_FALSE(arena.upload_buffer().is_valid());
    REQUIRE_FALSE(arena.download_buffer().is_valid());
    REQUIRE(built->ownership.gate_count_dispatches == 0u);
    REQUIRE(built->ownership.gated_emit_dispatches == 0u);
    REQUIRE(built->ownership.ungated_emit_dispatches == 1u);
    REQUIRE(built->ownership.retained_minimizer_count > 0u);
    REQUIRE(built->ownership.retained_minimizer_count
            < pessimistic_capacity);
    REQUIRE(built->position_count
            == built->ownership.retained_minimizer_count);
    REQUIRE(built->position_count
            == built->ownership.counted_minimizer_count);
    REQUIRE(built->position_count
            == built->ownership.verified_emit_count);
    REQUIRE(built->ownership.peak_observed_bytes
            <= built->ownership.peak_admitted_bytes);

    const auto slots_handle = built->slots.buffer;
    const auto positions_handle = built->positions.buffer;
    const std::uint64_t slots_allocation = built->slots.allocation_bytes;
    const std::uint64_t positions_allocation =
        built->positions.allocation_bytes;
    const std::uint64_t resident_bytes =
        slots_allocation + positions_allocation;
    REQUIRE(built->ownership.resident_bytes == resident_bytes);
    const bool materialization_permit_released =
        built->materialization.is_permit_released;
    if (materialization_permit_released) {
        REQUIRE_FALSE(static_cast<bool>(built->permit));
    } else {
        REQUIRE(built->permit.bytes() == resident_bytes);
    }

    idx::gpu_minimizer_index_cache_key key{
        .source_id = 0u,
        .caller_identity = 0x0371u,
        .target_len = built->target_len,
        .table_size = built->occupied_slots,
        .position_count = built->position_count,
    };
    auto adopted = idx::adopt_gpu_built_index(
        runtime, std::move(*built), key);
    REQUIRE(adopted.has_value());
    auto resident = std::move(adopted).value();
    REQUIRE(resident != nullptr);
    REQUIRE(resident->ownership.resident_transfers == 1u);
    REQUIRE(resident->ownership.adoption_rereserve_attempts == 0u);
    REQUIRE(resident->ownership.adoption_successes == 1u);
    REQUIRE(resident->slots.buffer == slots_handle);
    REQUIRE(resident->positions.buffer == positions_handle);
    REQUIRE(resident->slots.allocation_bytes == slots_allocation);
    REQUIRE(resident->positions.allocation_bytes == positions_allocation);
    const std::uint64_t adopted_reservation = admission.reserved_bytes();
    const std::uint64_t cache_metadata_at_adoption =
        runtime.pipeline_cache_metadata_reserved_bytes();
    REQUIRE(cache_metadata_at_adoption >= cache_metadata_before);

    // Seed twice through the same resident buffers.  Each completed fence copies
    // anchors to owned host storage and then trims only the arena; the adopted
    // standalone slot/position allocations must retain identity and ownership.
    const auto query = cpu_sketch<minimap_sketch_data>(dna4_fixture, 10u, 15u);
    REQUIRE_FALSE(query.empty());
    const vk::gpu_seed_inputs inputs{
        .host_minimizer_bytes = std::as_bytes(
            std::span<const minimap_sketch_data>{query.data(), query.size()}),
        .device_minimizer_words = nullptr,
        .slots = &resident->slots,
        .positions = &resident->positions,
        .target_len = resident->target_len,
        .slot_mask = resident->slot_mask,
        .position_count = resident->position_count,
        .minimizer_count = static_cast<std::uint32_t>(query.size()),
        .minimizer_word_kind = static_cast<std::uint32_t>(
            vk::minimizer_wire_traits<minimap_sketch_data>::k_word_kind),
        .minimizer_words32_per_record =
            vk::minimizer_wire_traits<
                minimap_sketch_data>::k_words32_per_minimizer,
        .hash_shift =
            vk::minimizer_wire_traits<minimap_sketch_data>::k_hash_shift,
        .pos_bits = vk::minimizer_wire_traits<minimap_sketch_data>::k_position_bits,
        .bucket_count = resident->bucket_count,
        .target_position_offset = resident->target_position_offset,
    };
    const auto first = runtime.run_seed_anchor_roundtrip(
        inputs, sequence.bp_size(), 15u, 500);
    REQUIRE(first.count > 0u);
    REQUIRE(first.host_bytes.size() == first.count * 24u);
    REQUIRE_FALSE(arena.device_buffer().is_valid());
    REQUIRE_FALSE(arena.upload_buffer().is_valid());
    REQUIRE_FALSE(arena.download_buffer().is_valid());
    REQUIRE(resident->slots.buffer == slots_handle);
    REQUIRE(resident->positions.buffer == positions_handle);
    const std::uint64_t steady_cache_metadata =
        runtime.pipeline_cache_metadata_reserved_bytes();
    REQUIRE(steady_cache_metadata >= cache_metadata_at_adoption);
    const std::uint64_t steady_adopted_reservation =
        bioseq::parallelism::saturating_add(
            adopted_reservation,
            steady_cache_metadata - cache_metadata_at_adoption);
    REQUIRE(admission.reserved_bytes() == steady_adopted_reservation);

    const auto second = runtime.run_seed_anchor_roundtrip(
        inputs, sequence.bp_size(), 15u, 500);
    REQUIRE(second.count == first.count);
    REQUIRE(second.host_bytes == first.host_bytes);
    REQUIRE_FALSE(arena.device_buffer().is_valid());
    REQUIRE(resident->slots.buffer == slots_handle);
    REQUIRE(resident->positions.buffer == positions_handle);
    REQUIRE(resident->slots.allocation_bytes == slots_allocation);
    REQUIRE(resident->positions.allocation_bytes == positions_allocation);
    REQUIRE(runtime.pipeline_cache_metadata_reserved_bytes()
            == steady_cache_metadata);
    REQUIRE(admission.reserved_bytes() == steady_adopted_reservation);

    resident.reset();
    REQUIRE(admission.reserved_bytes()
            == bioseq::parallelism::saturating_add(
                baseline, steady_cache_metadata - cache_metadata_before));
}

TEST_CASE("vulkan minimizer: ungated all-N target publishes a validated empty index",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    std::vector<seq::dna5> all_n =
        "NNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNN"_dna5;
    auto sequence = compact_sequence_range<seq_type>(all_n);
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(sequence);
    const auto sequence_view =
        bioseq::storage::make_lazy_sequence_view(sequence_owner);
    vk::vulkan_sketch_decline decline{};
    auto built = vk::build_gpu_index_on_device<minimap_sketch_data>(
        sequence_view, 10u, 15u, {}, &decline);
    INFO(vk::format_vulkan_sketch_decline(decline));
    REQUIRE(built.has_value());
    REQUIRE(built->valid);
    REQUIRE(built->position_count == 0u);
    REQUIRE(built->occupied_slots == 0u);
    REQUIRE(built->ownership.exact_count_dispatches == 1u);
    REQUIRE(built->ownership.exact_count_prefix_scan_dispatches == 3u);
    REQUIRE(built->ownership.exact_count_control_readbacks == 1u);
    REQUIRE(built->ownership.deterministic_tile_emit_dispatches == 0u);
    REQUIRE(built->ownership.atomic_append_dispatches == 0u);
    REQUIRE(built->ownership.emit_validation_control_readbacks == 0u);
    REQUIRE(built->ownership.counted_minimizer_count == 0u);
    REQUIRE(built->ownership.verified_emit_count == 0u);
    REQUIRE(built->ownership.count_emit_parity_verified == 1u);
    REQUIRE(built->ownership.gate_count_dispatches == 0u);
    REQUIRE(built->ownership.gated_emit_dispatches == 0u);
    REQUIRE(built->ownership.ungated_emit_dispatches == 0u);
    REQUIRE(built->ownership.arena_trimmed_before_permit_shrink == 1u);

    idx::gpu_minimizer_index_cache_key key{
        .target_len = built->target_len,
        .table_size = built->occupied_slots,
        .position_count = built->position_count,
    };
    auto adopted = idx::adopt_gpu_built_index(
        vk::runtime::instance(), std::move(*built), key);
    REQUIRE(adopted.has_value());
    REQUIRE((*adopted)->position_count == 0u);
    REQUIRE((*adopted)->bucket_count == 0u);
}

TEST_CASE("vulkan minimizer: gated empty target retains count-dominant pretrim peak stats",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    auto& runtime = vk::runtime::instance();
    auto& arena = runtime.reusable_gpu_arena();
    REQUIRE(arena.release_idle_capacity());
    constexpr std::uint64_t high_water_request = std::uint64_t{64} << 20u;
    REQUIRE(arena.ensure_capacity(high_water_request, 0u, 0u));
    const std::uint64_t high_water = arena.device_buffer().allocation_bytes;

    auto mutable_gate_words = std::make_shared<std::vector<std::uint32_t>>(
        vk::k_gpu_query_gate_word_count,
        0u);
    const std::shared_ptr<const std::vector<std::uint32_t>> gate_words =
        std::move(mutable_gate_words);
    const auto gate_identity =
        vk::canonical_vulkan_sketch_query_gate_identity(
            std::span<const std::uint32_t>{*gate_words},
            vk::k_gpu_query_gate_mask,
            gate_words);
    const vk::vulkan_sketch_query_gate gate{
        std::span<const std::uint32_t>{*gate_words},
        vk::k_gpu_query_gate_mask,
        gate_identity,
        true};

    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    std::vector<seq::dna5> all_n(256u);
    for (auto& letter : all_n) letter.assign_rank(4u);
    compact_sequence_range<seq_type> sequence{all_n};
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(sequence);
    const auto sequence_view =
        bioseq::storage::make_lazy_sequence_view(sequence_owner);
    auto built = vk::build_gpu_index_on_device<minimap_sketch_data>(
        sequence_view, 10u, 15u, gate);
    REQUIRE(built.has_value());
    REQUIRE(built->valid);
    REQUIRE(built->position_count == 0u);
    REQUIRE(built->ownership.gate_count_dispatches == 1u);
    REQUIRE(built->ownership.pretrim_peak_observed_bytes >= high_water);
    REQUIRE(built->ownership.peak_observed_bytes
            == built->ownership.pretrim_peak_observed_bytes);
    REQUIRE(built->ownership.peak_observed_bytes
            <= built->ownership.peak_admitted_bytes);
    REQUIRE(built->ownership.execution_submit_host_transient_bytes == 0u);
    REQUIRE(built->ownership.arena_trimmed_before_permit_shrink == 1u);
}

TEST_CASE("vulkan minimizer: exact target count may exceed planning estimate",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    constexpr std::size_t dense_bp = 8'192u;
    constexpr std::size_t window_size = 10u;
    constexpr std::size_t kmer_size = 15u;
    std::vector<seq::dna4> repeated_a(dense_bp);
    for (auto& base : repeated_a) {
        base.assign_rank(0u);
    }
    const std::uint64_t estimate =
        vk::estimate_vulkan_minimizer_capacity(dense_bp, window_size);
    const auto scalar = cpu_sketch<minimap_sketch_data>(
        repeated_a, window_size, kmer_size);
    REQUIRE(scalar.size() > estimate);

    using seq_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    auto sequence = compact_sequence_range<seq_type>(repeated_a);
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(sequence);
    const auto sequence_view =
        bioseq::storage::make_lazy_sequence_view(sequence_owner);
    vk::vulkan_sketch_decline decline{};
    auto built = vk::build_gpu_index_on_device<minimap_sketch_data>(
        sequence_view, window_size, kmer_size, {}, &decline);
    INFO(vk::format_vulkan_sketch_decline(decline));
    REQUIRE(built.has_value());
    REQUIRE(built->valid);
    REQUIRE(built->position_count > estimate);
    REQUIRE(built->position_count == scalar.size());
    REQUIRE(built->ownership.target_minimizer_capacity_estimate == estimate);
    REQUIRE(built->ownership.capacity_estimate_underestimated == 1u);
    REQUIRE(built->ownership.counted_minimizer_count
            == built->position_count);
    REQUIRE(built->ownership.verified_emit_count
            == built->position_count);
    REQUIRE(built->ownership.count_emit_parity_verified == 1u);
    REQUIRE(built->ownership.atomic_append_dispatches == 0u);

    idx::gpu_minimizer_index_cache_key key{
        .target_len = built->target_len,
        .table_size = built->occupied_slots,
        .position_count = built->position_count,
    };
    auto adopted = idx::adopt_gpu_built_index(
        vk::runtime::instance(), std::move(*built), key);
    REQUIRE(adopted.has_value());
    REQUIRE((*adopted)->position_count == scalar.size());
    REQUIRE((*adopted)->ownership.capacity_estimate_underestimated == 1u);
}

TEST_CASE("vulkan minimizer: fragmented facade admission preserves exact index without scanner scratch",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    using config_t = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using range_type = compact_sequence_range<config_t>;
    using storage_unit = typename range_type::traits::storage_unit;
    using scanner_t =
        bioseq::storage::chunked_span_sequence_scanner<
            storage_unit,
            bioseq::storage::default_fragment_storage_units>;
    using lazy_t = bioseq::storage::lazy_sequence<config_t, scanner_t>;

    range_type contiguous{dna4_fixture};
    auto contiguous_owner = bioseq::storage::make_resident_lazy_sequence(contiguous);
    const auto contiguous_view =
        bioseq::storage::make_lazy_sequence_view(contiguous_owner);
    vk::vulkan_sketch_decline contiguous_decline{};
    auto contiguous_built = vk::build_gpu_index_on_device<minimap_sketch_data>(
        contiguous_view, 10u, 15u, {}, &contiguous_decline);
    INFO(vk::format_vulkan_sketch_decline(contiguous_decline));
    REQUIRE(contiguous_built.has_value());
    REQUIRE(contiguous_built->valid);
    const std::size_t slot_bytes =
        static_cast<std::size_t>(contiguous_built->occupied_slots) * 16u;
    const std::size_t position_bytes =
        static_cast<std::size_t>(contiguous_built->position_count) * 8u;
    std::vector<std::byte> expected_slots(slot_bytes);
    std::vector<std::byte> expected_positions(position_bytes);
    std::memcpy(
        expected_slots.data(), contiguous_built->slots.mapped, slot_bytes);
    std::memcpy(
        expected_positions.data(),
        contiguous_built->positions.mapped,
        position_bytes);
    const std::uint32_t expected_slot_count =
        contiguous_built->occupied_slots;
    const std::uint32_t expected_position_count =
        contiguous_built->position_count;
    contiguous_built.reset();

    lazy_t fragmented{scanner_t::from_range(contiguous)};
    const auto fragmented_view =
        bioseq::storage::make_lazy_sequence_view(fragmented);
    vk::vulkan_sketch_decline fragmented_decline{};
    auto fragmented_built = vk::build_gpu_index_on_device<minimap_sketch_data>(
        fragmented_view, 10u, 15u, {}, &fragmented_decline);
    INFO(vk::format_vulkan_sketch_decline(fragmented_decline));
    REQUIRE(fragmented_built.has_value());
    REQUIRE(fragmented_built->valid);
    REQUIRE(fragmented_built->occupied_slots == expected_slot_count);
    REQUIRE(fragmented_built->position_count == expected_position_count);
    REQUIRE(std::memcmp(
        fragmented_built->slots.mapped,
        expected_slots.data(),
        expected_slots.size()) == 0);
    REQUIRE(std::memcmp(
        fragmented_built->positions.mapped,
        expected_positions.data(),
        expected_positions.size()) == 0);

    const auto& stats = fragmented_built->ownership;
    REQUIRE_FALSE(fragmented_view.try_resident_packed(
        0u, fragmented_view.bp_size(), sizeof(std::uint32_t)));
    REQUIRE(stats.direct_sequence_arena_copies == 1u);
    REQUIRE(stats.host_sequence_capacity_releases_before_execution == 0u);
    REQUIRE(stats.host_sequence_capacity_released_bytes == 0u);
    REQUIRE(stats.direct_tile_arena_fills == 1u);
    REQUIRE(stats.count_submit_host_transient_bytes == 0u);
    REQUIRE(stats.execution_submit_host_transient_bytes == 0u);
    REQUIRE(stats.host_transient_released_before_execution_bytes ==
            stats.host_tile_staging_elided_bytes);
    REQUIRE(stats.arena_trimmed_before_permit_shrink == 1u);
}

#ifdef BIOSEQ_TEST_INDEXES
TEST_CASE(
    "Vulkan canonical u32 preparation borrows stable resident storage",
    "[component:bioseq][layer:unit][feature:gpu_runtime]") {
  constexpr std::size_t source_size = 1'000'003u;
  std::vector<seq::dna4> letters(source_size);
  for (std::size_t index = 0u; index < letters.size(); ++index) {
    letters[index].assign_rank(
        static_cast<std::uint8_t>((index * 11u + index / 19u) & 3u));
  }
  using config = sequence_custom_alphabet<seq::dna4, std::uint32_t>;
  compact_sequence_range<config> range{letters};
  auto lazy = bioseq::storage::make_resident_lazy_sequence(range);
  const auto view = bioseq::storage::make_lazy_sequence_view(lazy);
  const auto resident = view.try_resident_packed(
      0u, view.bp_size(), sizeof(std::uint32_t));
  REQUIRE(resident.has_value());
  REQUIRE(resident->leading_symbol_offset == 0u);

  auto &admission = bioseq::parallelism::memory_admission_controller::instance();
  const std::uint64_t baseline = admission.reserved_bytes();
  vk::arm_vulkan_admitted_window_test_seam();
  struct reset_window_seam {
    ~reset_window_seam() { vk::reset_vulkan_admitted_window_test_seam(); }
  } reset;
  vk::vulkan_sketch_decline decline{};
  auto window = vk::admitted_sequence_detail::prepare_canonical_u32_window(
      view, 0u, view.bp_size(), &decline);
  REQUIRE(window.has_value());
  REQUIRE_FALSE(window->owner.has_value());
  REQUIRE(window->borrow_kind
      == bioseq::storage::sequence_resident_borrow_kind::
          native_typed_storage);
  REQUIRE(window->descriptor.units == resident->units);
  REQUIRE(window->descriptor.unit_count == resident->unit_count);
  REQUIRE(window->descriptor.logical_length == view.bp_size());
  const auto snapshot = vk::vulkan_admitted_window_test_snapshot();
  REQUIRE(snapshot.admitted_materializations == 0u);
  REQUIRE(snapshot.live_admitted_windows == 0u);
  REQUIRE(snapshot.live_admitted_permit_bytes == 0u);
  REQUIRE(admission.reserved_bytes() == baseline);
}

TEST_CASE(
    "Vulkan byte upload borrows u64 storage without claiming typed u32",
    "[component:bioseq][layer:unit][feature:gpu_runtime]") {
  constexpr std::size_t source_size = 65'551u;
  std::vector<seq::dna4> letters(source_size);
  for (std::size_t index = 0u; index < letters.size(); ++index) {
    letters[index].assign_rank(
        static_cast<std::uint8_t>((index * 29u + index / 7u) & 3u));
  }

  using source_config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
  compact_sequence_range<source_config> source_range{letters};
  auto lazy = bioseq::storage::make_resident_lazy_sequence(source_range);
  const auto view = bioseq::storage::make_lazy_sequence_view(lazy);
  const auto native = view.try_resident_packed(
      0u, view.bp_size(), sizeof(std::uint64_t));
  REQUIRE(native.has_value());

  const auto typed_plan = bioseq::storage::plan_sequence_access(
      view,
      bioseq::storage::sequence_access_requirement::contiguous_resident,
      0u,
      view.bp_size(),
      sizeof(std::uint32_t));
  REQUIRE(typed_plan.route
      == bioseq::storage::sequence_access_route::materialize_admitted);
  REQUIRE_FALSE(typed_plan.resident.has_value());
  REQUIRE(typed_plan.resident_borrow_kind
      == bioseq::storage::sequence_resident_borrow_kind::none);

  const auto upload_plan = bioseq::storage::plan_sequence_access(
      view,
      bioseq::storage::sequence_access_requirement::contiguous_resident,
      0u,
      view.bp_size(),
      sizeof(std::uint32_t),
      bioseq::storage::sequence_resident_borrow_policy::
          byte_upload_compatible);
  REQUIRE(upload_plan.route
      == bioseq::storage::sequence_access_route::borrow_resident);
  REQUIRE(upload_plan.resident.has_value());
  REQUIRE(upload_plan.resident_borrow_kind
      == bioseq::storage::sequence_resident_borrow_kind::
          reinterpreted_bytes);
  REQUIRE(upload_plan.resident->units == native->units);
  REQUIRE(upload_plan.resident->unit_count
      == (letters.size() + 15u) / 16u);

  using control_config = sequence_custom_alphabet<seq::dna4, std::uint32_t>;
  compact_sequence_range<control_config> control_range{letters};
  auto control_lazy = bioseq::storage::make_resident_lazy_sequence(control_range);
  const auto control_view = bioseq::storage::make_lazy_sequence_view(control_lazy);
  const auto control = control_view.try_resident_packed(
      0u, control_view.bp_size(), sizeof(std::uint32_t));
  REQUIRE(control.has_value());
  REQUIRE(control->unit_count == upload_plan.resident->unit_count);
  REQUIRE(std::memcmp(
      upload_plan.resident->units,
      control->units,
      control->unit_count * sizeof(std::uint32_t)) == 0);

  auto& admission = bioseq::parallelism::memory_admission_controller::instance();
  const std::uint64_t baseline = admission.reserved_bytes();
  vk::arm_vulkan_admitted_window_test_seam();
  struct reset_window_seam {
    ~reset_window_seam() { vk::reset_vulkan_admitted_window_test_seam(); }
  } reset;
  vk::vulkan_sketch_decline decline{};
  auto window = vk::admitted_sequence_detail::prepare_canonical_u32_window(
      view, 0u, view.bp_size(), &decline);
  REQUIRE(window.has_value());
  REQUIRE_FALSE(window->owner.has_value());
  REQUIRE(window->borrow_kind
      == bioseq::storage::sequence_resident_borrow_kind::
          reinterpreted_bytes);
  REQUIRE(window->descriptor.units == native->units);

  bool visited_byte_segment = false;
  vk::admitted_sequence_detail::with_canonical_u32_upload_source<seq::dna4>(
      *window,
      [&](auto& upload_source) {
        using source_type = std::remove_cvref_t<decltype(upload_source)>;
        if constexpr (requires {
                        source_type::is_byte_addressed_upload_only;
                      }) {
          visited_byte_segment = true;
          const vk::segment_upload_descriptor segment =
              vk::compute_segment_descriptor(upload_source, 1u, 5u);
          auto begin = upload_source.cbegin();
          begin += segment.lo_unit;
          const auto* base = upload_source.read_cptr(begin);
          STATIC_REQUIRE(std::same_as<decltype(base), const std::byte*>);
          const vk::sequence_upload_view upload =
              vk::make_upload_view_from_window(
                  upload_source, base, segment);
          const auto* native_bytes =
              static_cast<const std::byte*>(native->units);
          const auto* control_bytes =
              static_cast<const std::byte*>(control->units);
          REQUIRE(upload.bytes == native_bytes + sizeof(std::uint32_t));
          REQUIRE(upload.byte_size == 4u * sizeof(std::uint32_t));
          REQUIRE(upload.unit_bits == 32u);
          REQUIRE(upload.bp_size == 64u);
          REQUIRE(std::memcmp(
              upload.bytes,
              control_bytes + sizeof(std::uint32_t),
              static_cast<std::size_t>(upload.byte_size)) == 0);
        }
      });
  REQUIRE(visited_byte_segment);
  const auto snapshot = vk::vulkan_admitted_window_test_snapshot();
  REQUIRE(snapshot.admitted_materializations == 0u);
  REQUIRE(snapshot.live_admitted_windows == 0u);
  REQUIRE(snapshot.live_admitted_permit_bytes == 0u);
  REQUIRE(admission.reserved_bytes() == baseline);
}

TEST_CASE(
    "Vulkan delayed fence retains admitted or stable borrowed source on faults",
    "[component:bioseq][layer:unit][feature:gpu_runtime][resource:gpu]") {
  if (!vulkan_ready()) {
    SUCCEED("no hardware Vulkan device; delayed-fence fixture skipped");
    return;
  }

  static constexpr std::size_t source_size = 8192u;
  static constexpr bioseq::storage::sequence_size_t selected_from = 64u;
  static constexpr bioseq::storage::sequence_size_t selected_to = 8128u;
  std::vector<seq::dna4> prewarm_letters(source_size);
  for (std::size_t index = 0u; index < prewarm_letters.size(); ++index) {
    prewarm_letters[index].assign_rank(
        static_cast<std::uint8_t>((index * 13u + index / 17u) & 3u));
  }
  using prewarm_config = sequence_custom_alphabet<seq::dna4, std::uint32_t>;
  compact_sequence_range<prewarm_config> prewarm_range{prewarm_letters};
  auto prewarm_owner = bioseq::storage::make_resident_lazy_sequence(prewarm_range);
  basic_sketch_collector<minimap_sketch_data> prewarm_collector;
  vk::collect_minimizers_vulkan_segment_or_throw<minimap_sketch_data>(
      prewarm_collector, bioseq::storage::make_lazy_sequence_view(prewarm_owner),
      selected_from, selected_to, 10u, 15u);

  auto &admission = bioseq::parallelism::memory_admission_controller::instance();
  const std::uint64_t baseline = admission.reserved_bytes();

  const auto run_fault = [&admission, baseline](
                             vk::vulkan_submission_test_fault fault,
                             std::string_view expected_error,
                             bool expose_resident) {
    std::atomic<std::uint64_t> destructions{0u};
    auto source = std::make_shared<vulkan_lifetime_source_state>();
    source->expose_resident = expose_resident;
    source->backend_thread = std::this_thread::get_id();
    source->destructions = std::addressof(destructions);
    source->ranks.resize(source_size);
    source->packed.resize((source_size + 15u) / 16u);
    for (std::size_t index = 0u; index < source->ranks.size(); ++index) {
      source->ranks[index] =
          static_cast<std::uint8_t>((index * 13u + index / 17u) & 3u);
      source->packed[index / 16u] |=
          static_cast<std::uint32_t>(source->ranks[index])
          << (2u * (index % 16u));
    }
    const std::weak_ptr<vulkan_lifetime_source_state> weak_source = source;
    const auto source_view = vulkan_lifetime_view(*source);

    struct observation {
      bool reached_delayed_submit = false;
      bool source_released_before_wait = false;
      bool permit_charged_before_wait = false;
      bool one_admitted_window_live = false;
      bool backend_materialized = false;
      bool reached_post_drain_gate = false;
      bool same_admitted_owner_after_wait = false;
      bool permit_charged_after_wait = false;
      std::uint64_t admitted_permit_bytes = 0u;
      std::uintptr_t admitted_buffer_address = 0u;
      std::uint64_t source_reads = 0u;
      std::uint64_t resident_probes = 0u;
      std::uint64_t resident_borrows = 0u;
      bool wrong_thread = false;
    } observed{};
    std::atomic<bool> call_finished{false};

    vk::arm_vulkan_admitted_window_test_seam();
    vk::arm_vulkan_submission_test_seam(fault);
    struct reset_seams {
      ~reset_seams() {
        vk::release_vulkan_submission_test_fence();
        vk::release_vulkan_submission_test_delivery();
        vk::reset_vulkan_submission_test_seam();
        vk::reset_vulkan_admitted_window_test_seam();
      }
    } reset_seams;

    std::thread observer{
        [&admission, baseline, weak_source, &destructions, &observed,
         expose_resident,
         &call_finished, source = std::move(source)]() mutable {
          const auto submit_deadline =
              std::chrono::steady_clock::now() + std::chrono::seconds{5};
          auto submission = vk::capture_vulkan_submission_test_snapshot();
          while (!submission.waiting_after_submit &&
                 !call_finished.load(std::memory_order_acquire) &&
                 std::chrono::steady_clock::now() < submit_deadline) {
            std::this_thread::yield();
            submission = vk::capture_vulkan_submission_test_snapshot();
          }
          observed.reached_delayed_submit =
              submission.waiting_after_submit && submission.submissions == 1u;
          if (!observed.reached_delayed_submit) {
            vk::release_vulkan_submission_test_fence();
            vk::release_vulkan_submission_test_delivery();
            while (!call_finished.load(std::memory_order_acquire)) {
              std::this_thread::yield();
            }
            return;
          }

          const auto admitted = vk::vulkan_admitted_window_test_snapshot();
          observed.admitted_permit_bytes =
              admitted.live_admitted_permit_bytes;
          observed.admitted_buffer_address =
              admitted.live_admitted_buffer_address;
          const std::uint64_t reserved_before_wait =
              admission.reserved_bytes();
          observed.permit_charged_before_wait = expose_resident
              ? admitted.live_admitted_permit_bytes == 0u &&
                    admitted.live_admitted_buffer_address == 0u
              : admitted.live_admitted_permit_bytes > 0u &&
                    admitted.live_admitted_buffer_address != 0u &&
                    reserved_before_wait >= baseline &&
                    reserved_before_wait - baseline >=
                        admitted.live_admitted_permit_bytes;
          observed.one_admitted_window_live = expose_resident
              ? admitted.admitted_materializations == 0u &&
                    admitted.live_admitted_windows == 0u
              : admitted.admitted_materializations == 1u &&
                    admitted.live_admitted_windows == 1u;
          observed.backend_materialized = expose_resident
              ? admitted.backend_thread_materializations == 0u &&
                    admitted.off_backend_materializations == 0u
              : admitted.backend_thread_materializations == 1u &&
                    admitted.off_backend_materializations == 0u;

          observed.source_reads =
              source->read_calls.load(std::memory_order_relaxed);
          observed.resident_probes =
              source->resident_probes.load(std::memory_order_relaxed);
          observed.resident_borrows =
              source->resident_borrows.load(std::memory_order_relaxed);
          observed.wrong_thread =
              source->wrong_thread.load(std::memory_order_relaxed);
          source.reset();
          observed.source_released_before_wait =
              weak_source.expired() &&
              destructions.load(std::memory_order_relaxed) == 1u;
          vk::release_vulkan_submission_test_fence();

          const auto drain_deadline =
              std::chrono::steady_clock::now() + std::chrono::seconds{5};
          auto drained = vk::capture_vulkan_submission_test_snapshot();
          while (!drained.waiting_after_drain &&
                 !call_finished.load(std::memory_order_acquire) &&
                 std::chrono::steady_clock::now() < drain_deadline) {
            std::this_thread::yield();
            drained = vk::capture_vulkan_submission_test_snapshot();
          }
          observed.reached_post_drain_gate =
              drained.waiting_after_drain && drained.fence_drains == 1u;
          if (observed.reached_post_drain_gate) {
            const auto admitted_after_wait =
                vk::vulkan_admitted_window_test_snapshot();
            observed.same_admitted_owner_after_wait = expose_resident
                ? admitted_after_wait.live_admitted_windows == 0u &&
                    admitted_after_wait.live_admitted_permit_bytes == 0u &&
                    admitted_after_wait.live_admitted_buffer_address == 0u &&
                    weak_source.expired() &&
                    destructions.load(std::memory_order_relaxed) == 1u
                : admitted_after_wait.live_admitted_windows == 1u &&
                    admitted_after_wait.live_admitted_permit_bytes ==
                        observed.admitted_permit_bytes &&
                    admitted_after_wait.live_admitted_buffer_address ==
                        observed.admitted_buffer_address &&
                    weak_source.expired() &&
                    destructions.load(std::memory_order_relaxed) == 1u;
            const std::uint64_t reserved_after_wait =
                admission.reserved_bytes();
            observed.permit_charged_after_wait = expose_resident
                ? admitted_after_wait.live_admitted_permit_bytes == 0u
                : reserved_after_wait >= baseline &&
                    reserved_after_wait - baseline >=
                        observed.admitted_permit_bytes;
          }
          vk::release_vulkan_submission_test_delivery();
        }};

    std::string failure;
    std::exception_ptr unexpected{};
    try {
      basic_sketch_collector<minimap_sketch_data> collector;
      vk::collect_minimizers_vulkan_segment_or_throw<minimap_sketch_data>(
          collector, source_view, selected_from, selected_to, 10u, 15u);
    } catch (const vk::vulkan_runtime_error &error) {
      try {
        failure = error.what();
      } catch (...) {
        unexpected = std::current_exception();
      }
    } catch (...) {
      unexpected = std::current_exception();
    }
    call_finished.store(true, std::memory_order_release);
    vk::release_vulkan_submission_test_fence();
    vk::release_vulkan_submission_test_delivery();
    observer.join();
    if (unexpected) {
      std::rethrow_exception(unexpected);
    }

    const auto submission = vk::capture_vulkan_submission_test_snapshot();
    const auto admitted = vk::vulkan_admitted_window_test_snapshot();

    REQUIRE(observed.reached_delayed_submit);
    REQUIRE(observed.source_released_before_wait);
    REQUIRE(observed.permit_charged_before_wait);
    REQUIRE(observed.one_admitted_window_live);
    REQUIRE(observed.backend_materialized);
    REQUIRE(observed.reached_post_drain_gate);
    REQUIRE(observed.same_admitted_owner_after_wait);
    REQUIRE(observed.permit_charged_after_wait);
    REQUIRE((observed.admitted_permit_bytes > 0u) != expose_resident);
    REQUIRE((observed.admitted_buffer_address != 0u) != expose_resident);
    REQUIRE(submission.upload_latches >= 1u);
    REQUIRE(submission.submissions == 1u);
    REQUIRE(submission.fence_drains == 1u);
    REQUIRE(submission.delivered_faults == 1u);
    REQUIRE(
        submission.cancellation_requested ==
        (fault == vk::vulkan_submission_test_fault::cancellation_after_submit));
    REQUIRE(
        submission.upload_failure_latched ==
        (fault == vk::vulkan_submission_test_fault::upload_failure_after_submit));
    REQUIRE(failure.find(expected_error) != std::string::npos);
    REQUIRE(admitted.admitted_materializations
        == (expose_resident ? 0u : 1u));
    REQUIRE(admitted.live_admitted_windows == 0u);
    REQUIRE(admitted.live_admitted_permit_bytes == 0u);
    REQUIRE(admitted.live_admitted_buffer_address == 0u);
    REQUIRE(admitted.backend_thread_materializations
        == (expose_resident ? 0u : 1u));
    REQUIRE(admitted.off_backend_materializations == 0u);
    REQUIRE(observed.resident_probes == (expose_resident ? 1u : 2u));
    REQUIRE(observed.resident_borrows == (expose_resident ? 1u : 0u));
    REQUIRE((expose_resident
        ? observed.source_reads == 0u
        : observed.source_reads > 0u));
    REQUIRE_FALSE(observed.wrong_thread);
    REQUIRE(weak_source.expired());
    REQUIRE(destructions.load(std::memory_order_relaxed) == 1u);
    REQUIRE(admission.reserved_bytes() == baseline);
  };

  for (const bool expose_resident : std::array<bool, 2>{false, true}) {
    INFO("resident source=" << (expose_resident ? "borrowable" : "absent"));
    run_fault(vk::vulkan_submission_test_fault::cancellation_after_submit,
              "cancellation after fence drain", expose_resident);
    run_fault(vk::vulkan_submission_test_fault::upload_failure_after_submit,
              "upload failure after fence drain", expose_resident);
  }
}
#endif

TEST_CASE(
    "vulkan minimizer: canonical u32 admission removes raw tail bytes with "
    "exact parity",
    "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
  if (!vulkan_ready()) {
    SUCCEED("no hardware Vulkan device; skipped");
    return;
  }

  using packed8_t = sequence_custom_alphabet<seq::dna4, std::uint8_t>;
  using packed64_t = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
  for (const std::size_t letter_count :
       std::array<std::size_t, 3>{65u, 69u, 73u}) {
    std::vector<seq::dna4> letters(letter_count);
    for (std::size_t index = 0u; index < letters.size(); ++index) {
      letters[index].assign_rank(static_cast<std::uint8_t>(index & 3u));
    }
    compact_sequence_range<packed64_t> packed64{letters};
    auto packed64_owner = bioseq::storage::make_resident_lazy_sequence(packed64);
    const auto packed64_view =
        bioseq::storage::make_lazy_sequence_view(packed64_owner);
    auto baseline = vk::build_gpu_index_on_device<minimap_sketch_data>(
        packed64_view, 10u, 15u);
    REQUIRE(baseline.has_value());
    REQUIRE(baseline->valid);
    const std::size_t slot_bytes =
        static_cast<std::size_t>(baseline->occupied_slots) * 16u;
    const std::size_t position_bytes =
        static_cast<std::size_t>(baseline->position_count) * 8u;
    std::vector<std::byte> expected_slots(slot_bytes);
    std::vector<std::byte> expected_positions(position_bytes);
    std::memcpy(expected_slots.data(), baseline->slots.mapped, slot_bytes);
    std::memcpy(expected_positions.data(), baseline->positions.mapped,
                position_bytes);
    const std::uint32_t expected_slot_count = baseline->occupied_slots;
    const std::uint32_t expected_position_count = baseline->position_count;
    baseline.reset();

    compact_sequence_range<packed8_t> packed8{letters};
    const vk::sequence_upload_view upload =
        vk::make_sequence_upload_view(packed8);
    REQUIRE(upload.byte_size % 4u != 0u);
    auto packed8_owner = bioseq::storage::make_resident_lazy_sequence(packed8);
    const auto packed8_view =
        bioseq::storage::make_lazy_sequence_view(packed8_owner);
    REQUIRE_FALSE(packed8_view.try_resident_packed(0u, packed8_view.bp_size(),
                                                   sizeof(std::uint32_t)));
    auto candidate = vk::build_gpu_index_on_device<minimap_sketch_data>(
        packed8_view, 10u, 15u);
    REQUIRE(candidate.has_value());
    REQUIRE(candidate->valid);
    REQUIRE(candidate->ownership.sequence_padding_zeroed_bytes == 0u);
    REQUIRE(candidate->occupied_slots == expected_slot_count);
    REQUIRE(candidate->position_count == expected_position_count);
    REQUIRE(std::memcmp(candidate->slots.mapped, expected_slots.data(),
                        expected_slots.size()) == 0);
    REQUIRE(std::memcmp(candidate->positions.mapped, expected_positions.data(),
                        expected_positions.size()) == 0);
  }
}

TEST_CASE("vulkan minimizer: truncated query gate fails before target-index dispatch",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    using seq_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    auto sequence = compact_sequence_range<seq_type>(dna4_fixture);
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(sequence);
    const auto sequence_view =
        bioseq::storage::make_lazy_sequence_view(sequence_owner);
    const std::array<std::uint32_t, 1> truncated_words{1u};
    const vk::vulkan_sketch_query_gate truncated_gate{
        std::span<const std::uint32_t>{truncated_words}, 31u};
    REQUIRE_THROWS_AS(
        vk::build_gpu_index_on_device<minimap_sketch_data>(
            sequence_view, 10u, 15u, truncated_gate),
        vk::vulkan_runtime_error);
}

TEST_CASE("vulkan minimizer: selected segment preserves typed strict cause",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using config_t = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using range_type = compact_sequence_range<config_t>;
    using storage_unit = typename range_type::traits::storage_unit;
    using scanner_t =
        bioseq::storage::chunked_span_sequence_scanner<
            storage_unit,
            bioseq::storage::default_fragment_storage_units>;
    using lazy_t = bioseq::storage::lazy_sequence<config_t, scanner_t>;

    range_type range(dna4_fixture);
    lazy_t fragmented{scanner_t::from_range(range)};
    const auto fragmented_view =
        bioseq::storage::make_lazy_sequence_view(fragmented);
    basic_sketch_collector<minimap_sketch_data> collector;
    bool caught = false;
    try {
        vk::collect_minimizers_vulkan_segment_or_throw<minimap_sketch_data>(
            collector,
            fragmented_view,
            0u,
            fragmented.bp_size(),
            10u,
            40u);
    } catch (const vk::vulkan_runtime_error& error) {
        caught = true;
        const std::string message = error.what();
        REQUIRE(message.find("Vulkan segment minimizer sketch selected")
                != std::string::npos);
        REQUIRE(message.find("reason=kmer_size_unsupported")
                != std::string::npos);
        REQUIRE(message.find("w=10 k=40") != std::string::npos);
    }
    REQUIRE(caught);
}

TEST_CASE("vulkan minimizer: explicit output overflow has typed identity",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; output-overflow dispatch skipped");
        return;
    }
    using seq_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    auto sequence = compact_sequence_range<seq_type>(dna4_fixture);
    const auto upload = vk::make_sequence_upload_view(sequence);
    const auto rank_lut = vk::make_vulkan_rank_lut<decltype(sequence)>();
    bool capacity_overflowed = false;
    vk::vulkan_sketch_decline decline{};
    auto result = vk::sketch_uploaded_window_result<minimap_sketch_data>(
        upload,
        rank_lut,
        10u,
        15u,
        1u,
        &capacity_overflowed,
        {},
        &decline);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(capacity_overflowed);
    REQUIRE(decline.reason
            == vk::vulkan_sketch_decline_reason::output_capacity_overflow);
    REQUIRE(decline.max_minimizers == 1u);
}

TEST_CASE("vulkan minimizer: software device declines selected paths",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!software_vulkan_ready()) {
        SUCCEED("No software Vulkan device detected; hardware GPU cases cover the selected path");
        return;
    }

    using config_t = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using range_type = compact_sequence_range<config_t>;
    using storage_unit = typename range_type::traits::storage_unit;
    using scanner_t =
        bioseq::storage::chunked_span_sequence_scanner<
            storage_unit,
            bioseq::storage::default_fragment_storage_units>;
    using lazy_t = bioseq::storage::lazy_sequence<config_t, scanner_t>;

    range_type range(dna4_fixture);
    auto range_owner = bioseq::storage::make_resident_lazy_sequence(range);
    const auto range_view =
        bioseq::storage::make_lazy_sequence_view(range_owner);
    auto result = vk::collect_minimizers_vulkan_result<minimap_sketch_data>(
        range_view, 10, 15);
    REQUIRE_FALSE(result.has_value());

    lazy_t fragmented{scanner_t::from_range(range)};
    const auto fragmented_view =
        bioseq::storage::make_lazy_sequence_view(fragmented);
    REQUIRE_THROWS_AS(
        vk::collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
            fragmented_view,
            0,
            fragmented.bp_size(),
            0,
            0,
            static_cast<bioseq::storage::sequence_size_t>(fragmented.bp_size()),
            10,
            15),
        vk::vulkan_runtime_error);
}

TEST_CASE("vulkan minimizer: minimizer_representation 16/32/64/128-bit layouts round-trip",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) { SUCCEED("no hardware Vulkan device; skipped"); return; }
    // 16-bit: k_position_bits=10, hash fits 5 bits -> uint16 storage.
    using representation_16 = minimizer_representation<10u, 2u>;
    using representation_32 = minimizer_representation<10u, 9u>;       // pos 10 + hash 21 -> uint32 storage
    using representation_64 = minimap_sketch_data;            // minimizer_representation<28,15>, 64-bit
    using representation_128 = minimizer_representation<28u, 30u>;     // no room in 64 -> 128-bit
    static_assert(sizeof(representation_16) == 2);
    static_assert(sizeof(representation_32) == 4);
    static_assert(sizeof(representation_64) == 8);
    static_assert(sizeof(representation_128) == 16);

    {
        const auto cpu = triples(cpu_sketch<representation_16>(dna4_fixture, 10, 2));
        const auto gpu = triples(gpu_sketch<representation_16>(dna4_fixture, 10, 2));
        ASSERT(cpu == gpu, fmt::format("u16 repr parity failed (cpu={} gpu={})",
                                       cpu.size(), gpu.size()), cpu.size(), gpu.size());
    }
    {
        const auto cpu = triples(cpu_sketch<representation_32>(dna4_fixture, 10, 9));
        const auto gpu = triples(gpu_sketch<representation_32>(dna4_fixture, 10, 9));
        ASSERT(cpu == gpu, fmt::format("u32 repr parity failed (cpu={} gpu={})",
                                       cpu.size(), gpu.size()), cpu.size(), gpu.size());
    }
    {
        const auto cpu = triples(cpu_sketch<representation_64>(dna4_fixture, 10, 15));
        const auto gpu = triples(gpu_sketch<representation_64>(dna4_fixture, 10, 15));
        ASSERT(cpu == gpu, fmt::format("u64 repr parity failed (cpu={} gpu={})",
                                       cpu.size(), gpu.size()), cpu.size(), gpu.size());
    }
    {
        const auto cpu = triples(cpu_sketch<representation_128>(dna4_fixture, 10, 30));
        const auto gpu = triples(gpu_sketch<representation_128>(dna4_fixture, 10, 30));
        ASSERT(cpu == gpu, fmt::format("u64x2 repr parity failed (cpu={} gpu={})",
                                       cpu.size(), gpu.size()), cpu.size(), gpu.size());
    }
}

// ===========================================================================
// Checked Vulkan segment-path tests.
// ===========================================================================

TEST_CASE("Vulkan segment minimizer sketch matches scalar fragmented windows",
          "[component:bioseq][layer:unit][feature:minimizer][resource:gpu]") {
    if (!vulkan_ready()) { SUCCEED("no hardware Vulkan device; skipped"); return; }

    auto run_case = []<class p_alphabet_type>() {
        using config_t = sequence_custom_alphabet<p_alphabet_type, std::uint64_t>;
        using range_type = compact_sequence_range<config_t>;
        using storage_unit = typename range_type::traits::storage_unit;
        using scanner_t =
            bioseq::storage::chunked_span_sequence_scanner<
                storage_unit,
                bioseq::storage::default_fragment_storage_units>;
        using lazy_t = bioseq::storage::lazy_sequence<config_t, scanner_t>;
        using p_minimizer_type = minimap_sketch_data;

        constexpr std::size_t w = 10;
        constexpr std::size_t k = 15;
        // The segment sketch uses fused tiled Vulkan execution for every supported
        // alphabet. dna4 is still expected to be exact; dna5/generic may contain
        // bounded tile-boundary extras but must cover every scalar minimizer.
        //  (1) draw ranks from the NON-AMBIGUOUS letters only (ACGT, ranks 0..3).
        //      The original periodic `i % alphabet_size` put an N every 5th base
        //      for dna5 (every 15-mer spans an N → vacuous). ACGT here keeps
        //      this segment case focused on window tiling rather than reset
        //      handling. And
        //  (2) make the length a MULTIPLE of letters_per_unit so the trailing
        //      unit has zero padding — otherwise the scalar unit-iterator
        //      sub-range processes the last unit's padding slots (decoded as A)
        //      as spurious end-of-window minimizers that the GPU (which honours
        //      tail_padding) correctly omits. This is a separate scalar/GPU
        //      end-of-sequence discrepancy, independent of the tiled bug.
        constexpr std::uint8_t k_non_ambiguous_rank_count =
            range_type::traits::alphabet_size < 4u
                ? static_cast<std::uint8_t>(range_type::traits::alphabet_size)
                : 4u;
        const std::size_t letter_count = range_type::traits::letters_per_unit * 50000u;
        std::vector<typename config_t::alphabet_type> letters;
        letters.reserve(letter_count);
        std::uint64_t lcg = 0x9e3779b97f4a7c15ULL;
        for (std::size_t i = 0; i < letter_count; ++i) {
            lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
            typename config_t::alphabet_type value{};
            value.assign_rank(static_cast<std::uint8_t>((lcg >> 33) % k_non_ambiguous_rank_count));
            letters.push_back(value);
        }
        range_type range(letters);
        lazy_t fragmented{scanner_t::from_range(range)};
        const auto fragmented_view =
            bioseq::storage::make_lazy_sequence_view(fragmented);

        const std::array<std::pair<std::size_t, std::size_t>, 4> windows{{
            {0, 4096},
            {17, 8192},
            {4096, 16384},
            {range.size() > 20000
                 ? static_cast<std::size_t>(range.size()) - 20000
                 : 0,
             static_cast<std::size_t>(range.size())},
        }};

        for (const auto& window : windows) {
            const std::size_t lo_unit = window.first;
            const std::size_t hi_unit = std::min(
                window.second, static_cast<std::size_t>(range.size()));
            basic_sketch_collector<p_minimizer_type> scalar;
            basic_sketch_collector<p_minimizer_type> gpu;

            auto begin_iter = fragmented.cbegin();
            begin_iter += static_cast<bioseq::storage::sequence_size_t>(lo_unit);
            auto end_iter = fragmented.cbegin();
            end_iter += static_cast<bioseq::storage::sequence_size_t>(hi_unit);
            collect_minimizers_view<minimap_int_hasher, p_minimizer_type>(
                scalar, fragmented, begin_iter, end_iter, w, k);

            vk::collect_minimizers_vulkan_segment_or_throw<p_minimizer_type>(
                gpu,
                fragmented_view,
                static_cast<bioseq::storage::sequence_size_t>(lo_unit)
                    * range_type::traits::letters_per_unit,
                std::min<bioseq::storage::sequence_size_t>(
                    static_cast<bioseq::storage::sequence_size_t>(hi_unit)
                        * range_type::traits::letters_per_unit,
                    fragmented.bp_size()),
                w,
                k);

            auto scalar_values = scalar.extract_sketches_data();
            auto gpu_values = gpu.extract_sketches_data();
            auto normalize = [](auto& values) {
                std::sort(
                    values.begin(),
                    values.end(),
                    [](const p_minimizer_type& lhs, const p_minimizer_type& rhs) {
                        return std::tuple{lhs.kmer(), lhs.last_pos(), lhs.strand()}
                            < std::tuple{rhs.kmer(), rhs.last_pos(), rhs.strand()};
                    });
            };
            normalize(scalar_values);
            normalize(gpu_values);
            if constexpr (std::is_same_v<p_alphabet_type, seq::dna4>) {
                REQUIRE(gpu_values.size() == scalar_values.size());
                for (std::size_t i = 0; i < scalar_values.size(); ++i) {
                    CHECK(gpu_values[i].kmer() == scalar_values[i].kmer());
                    CHECK(gpu_values[i].last_pos() == scalar_values[i].last_pos());
                    CHECK(gpu_values[i].strand() == scalar_values[i].strand());
                }
            } else {
                const std::uint64_t bp_count =
                    static_cast<std::uint64_t>(hi_unit - lo_unit)
                    * static_cast<std::uint64_t>(range_type::traits::letters_per_unit);
                require_gpu_minimizer_superset_with_bounded_tile_extras(
                    scalar_values,
                    gpu_values,
                    vk::make_sketch_tile_plan(
                        bp_count,
                        static_cast<std::uint32_t>(k),
                        static_cast<std::uint32_t>(w),
                        range_type::traits::letters_per_unit),
                    static_cast<std::uint32_t>(k),
                    static_cast<std::uint32_t>(w));
            }
        }
    };

    run_case.template operator()<seq::dna4>();
    run_case.template operator()<seq::dna5>();
}

TEST_CASE("Vulkan segment sorted pairs are ordered by kmer then encoded position",
          "[component:bioseq][layer:unit][feature:minimizer][resource:gpu]") {
    if (!vulkan_ready()) { SUCCEED("no hardware Vulkan device; skipped"); return; }

    {
        using config_t = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
        using range_type = compact_sequence_range<config_t>;
        using storage_unit = typename range_type::traits::storage_unit;
        using scanner_t =
            bioseq::storage::chunked_span_sequence_scanner<
                storage_unit,
                bioseq::storage::default_fragment_storage_units>;
        using lazy_t = bioseq::storage::lazy_sequence<config_t, scanner_t>;

        std::vector<typename config_t::alphabet_type> letters;
        letters.reserve(1u << 20);
        std::uint64_t lcg = 0x9e3779b97f4a7c15ULL;
        for (std::size_t i = 0; i < (1u << 20); ++i) {
            lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
            typename config_t::alphabet_type value{};
            value.assign_rank(static_cast<std::uint8_t>((lcg >> 33) % 4u));
            letters.push_back(value);
        }
        range_type range(letters);
        lazy_t fragmented{scanner_t::from_range(range)};
        const auto fragmented_view =
            bioseq::storage::make_lazy_sequence_view(fragmented);

        basic_sketch_collector<minimap_sketch_data> scalar;
        collect_minimizers_view<minimap_int_hasher, minimap_sketch_data>(
            scalar,
            fragmented,
            fragmented.cbegin(),
            fragmented.cend(),
            10,
            15);
        std::vector<std::pair<std::uint64_t, std::uint64_t>> expected;
        for (const minimap_sketch_data& minimizer : scalar.extract_sketches_data()) {
            expected.emplace_back(
                minimizer.kmer(),
                (static_cast<std::uint64_t>(minimizer.last_pos()) << 1)
                    | static_cast<std::uint64_t>(minimizer.strand()));
        }
        std::sort(expected.begin(), expected.end());

        vk::vulkan_segment_pair_execution_stats execution_stats{};
        const auto pairs =
            vk::collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
                fragmented_view,
                0,
                fragmented.bp_size(),
                0,
                0,
                static_cast<bioseq::storage::sequence_size_t>(fragmented.bp_size()),
                10,
                15,
                &execution_stats);
        REQUIRE(pairs == expected);
        REQUIRE(execution_stats.is_internally_consistent());
        REQUIRE(execution_stats.completed_roundtrips > 0);
        REQUIRE_FALSE(execution_stats.saturated);
    }

    using config_t = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    using range_type = compact_sequence_range<config_t>;
    using storage_unit = typename range_type::traits::storage_unit;
    using scanner_t =
        bioseq::storage::chunked_span_sequence_scanner<
            storage_unit,
            bioseq::storage::default_fragment_storage_units>;
    using lazy_t = bioseq::storage::lazy_sequence<config_t, scanner_t>;

    std::vector<typename config_t::alphabet_type> letters;
    constexpr std::size_t dna5_letter_count = range_type::traits::letters_per_unit * 50000u;
    letters.reserve(dna5_letter_count);
    // Varied LCG ranks ensure that the dna5 sequence has
    // minimizers. `(i*7 + i/31) % 5` is periodic and puts an N (rank 4) every
    // 5th base, so every 15-mer contains an N → zero minimizers → empty pairs.
    std::uint64_t lcg = 0x2545f4914f6cdd1dULL;
    for (std::size_t i = 0; i < dna5_letter_count; ++i) {
        lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
        typename config_t::alphabet_type value{};
        value.assign_rank(static_cast<std::uint8_t>(
            (lcg >> 33) % range_type::traits::alphabet_size));
        letters.push_back(value);
    }
    range_type range(letters);
    lazy_t fragmented{scanner_t::from_range(range)};
    const auto fragmented_view =
        bioseq::storage::make_lazy_sequence_view(fragmented);

    basic_sketch_collector<minimap_sketch_data> scalar;
    collect_minimizers_view<minimap_int_hasher, minimap_sketch_data>(
        scalar,
        fragmented,
        fragmented.cbegin(),
        fragmented.cend(),
        10,
        15);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> expected;
    for (const minimap_sketch_data& minimizer : scalar.extract_sketches_data()) {
        expected.emplace_back(
            minimizer.kmer(),
            (static_cast<std::uint64_t>(minimizer.last_pos()) << 1)
                | static_cast<std::uint64_t>(minimizer.strand()));
    }
    std::sort(expected.begin(), expected.end());

    const auto pairs =
        vk::collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
            fragmented_view,
            0,
            fragmented.bp_size(),
            0,
            0,
            static_cast<bioseq::storage::sequence_size_t>(fragmented.bp_size()),
            10,
            15);
    REQUIRE(!pairs.empty());
    require_gpu_pair_superset_with_bounded_tile_extras(
        expected,
        pairs,
        vk::make_sketch_tile_plan(
            fragmented.bp_size(),
            15,
            10,
            range_type::traits::letters_per_unit),
        15,
        10);
}

TEST_CASE("Vulkan borrowed segment-pair admission covers arena high-water",
          "[component:bioseq][layer:unit][feature:minimizer][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    // Reproduce the ordering that exposed the regression: an earlier Vulkan
    // operation leaves a large reusable arena allocation, then a small
    // persisted-index segment borrows a slice from its outer owner.  Requested
    // bytes for the small segment are much lower than this physical high-water;
    // the borrowed slice must nevertheless cover the allocation that is live.
    auto& runtime = vk::runtime::instance();
    constexpr std::uint64_t high_water_request = std::uint64_t{32} << 20u;
    REQUIRE(runtime.reusable_gpu_arena().ensure_capacity(
        high_water_request,
        0u,
        0u));
    REQUIRE(runtime.reusable_gpu_arena().device_buffer().allocation_bytes
            >= high_water_request);

    using config_t = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using range_type = compact_sequence_range<config_t>;
    using storage_unit = typename range_type::traits::storage_unit;
    using scanner_t =
        bioseq::storage::chunked_span_sequence_scanner<
            storage_unit,
            bioseq::storage::default_fragment_storage_units>;
    using lazy_t = bioseq::storage::lazy_sequence<config_t, scanner_t>;

    range_type range(dna4_fixture);
    lazy_t fragmented{scanner_t::from_range(range)};
    const auto fragmented_view =
        bioseq::storage::make_lazy_sequence_view(fragmented);
    vk::vulkan_segment_pair_execution_stats execution_stats{};
    const auto pairs =
        vk::collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
            fragmented_view,
            0u,
            fragmented.bp_size(),
            0u,
            0u,
            static_cast<bioseq::storage::sequence_size_t>(
                fragmented.bp_size()),
            10u,
            15u,
            &execution_stats);

    REQUIRE_FALSE(pairs.empty());
    REQUIRE(execution_stats.is_internally_consistent());
    REQUIRE(execution_stats.completed_roundtrips > 0u);
    REQUIRE_FALSE(execution_stats.saturated);
}

}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();
