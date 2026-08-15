/**
 * @file test_support.cppm
 * @brief Shared fixtures and comparison helpers for Vulkan minimizer tests.
 */
module;

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

export module bioseq.tests.unit.kmers.vulkan_minimizers_support;

export import bioseq.common.logging;
export import bioseq.common.checked_arithmetic;
export import bioseq.common.seqan_adapter;
export import bioseq.sequences;
export import bioseq.sequences.simd;
export import bioseq.sequences.kmers;
export import bioseq.sequences.kmers.hashers;
export import bioseq.sequences.kmers.vulkan_minimizers;
export import bioseq.gpu.vulkan.abi;
export import bioseq.gpu.vulkan.runtime;
export import bioseq.indexing.minimizer_hashmap;
export import bioseq.indexing.gpu_minimizer_index;
export import bioseq.parallelism.memory_admission;
export import bioseq.storage.scanner;
export import bioseq.storage.lazy_sequence;
export import bioseq.storage.size;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();

export namespace bioseq::simd::HWY_NAMESPACE::vulkan_minimizers_test_support {

/** @brief Namespace selected by the active Highway target. */
namespace hn = hwy::HWY_NAMESPACE;

namespace vk = ::bioseq::gpu::vulkan;
namespace idx = ::bioseq::indexing;

/** @brief Canonical position, hash, and strand tuple used by parity checks. */
using minimizer_tuple_type = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;

/** @return `true` when a hardware Vulkan device can execute the tests. */
[[nodiscard]] inline bool vulkan_ready() {
    return vk::hardware_device_available(vk::runtime::instance());
}

/** @return `true` when the active Vulkan runtime uses a software device. */
[[nodiscard]] inline bool software_vulkan_ready() {
  auto &runtime = vk::runtime::instance();
  return runtime.is_usable() && runtime.features().software_device;
}

#ifdef BIOSEQ_TEST_INDEXES
/** @brief Lazy dna4 facade used to exercise backend lifetime ownership. */
using vulkan_lifetime_facade =
    bioseq::storage::lazy_sequence_view<seq::dna4>;

/** @brief Owns packed test data and records lazy-backend access and destruction. */
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

  /** @brief Records destruction through the optional external counter. */
  ~vulkan_lifetime_source_state() {
    if (destructions != nullptr) {
      destructions->fetch_add(1u, std::memory_order_relaxed);
    }
  }
};

/**
 * @brief Reads a logical range from the lifetime fixture into caller storage.
 * @tparam p_is_reversed Selects reverse-complement traversal.
 * @tparam p_write_reversed Selects descending output order.
 * @param opaque Fixture state.
 * @param from First logical symbol.
 * @param count Number of symbols to read.
 * @param scratch Destination bytes.
 * @return Pointer to the populated scratch storage.
 */
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

/**
 * @brief Reports the logical length of a lifetime fixture.
 * @param opaque Fixture state.
 * @return Number of biological symbols.
 */
[[nodiscard]] bioseq::storage::sequence_size_t
vulkan_lifetime_bp_size(const void *opaque) noexcept {
  return static_cast<const vulkan_lifetime_source_state *>(opaque)->ranks.size();
}

/**
 * @brief Borrows a resident packed window when the fixture exposes one.
 * @param opaque Fixture state.
 * @param from First logical symbol.
 * @param to Exclusive logical end.
 * @param requested_width Requested packed word width.
 * @return Resident descriptor, or no value when the request cannot be borrowed.
 */
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

/**
 * @brief Declares that the lifetime fixture cannot be shared across workers.
 * @return Always `false`.
 */
[[nodiscard]] bool vulkan_lifetime_worker_shareable(const void *) noexcept {
  return false;
}

/** @brief Backend callback table for the lifetime fixture. */
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

/**
 * @brief Creates a non-owning lazy facade over a lifetime fixture.
 * @param state Fixture state that outlives the returned facade.
 * @return Lazy sequence view backed by `state`.
 */
[[nodiscard]] vulkan_lifetime_facade
vulkan_lifetime_view(const vulkan_lifetime_source_state &state) noexcept {
  return vulkan_lifetime_facade{
      std::addressof(state), std::addressof(vulkan_lifetime_ops)};
}
#endif

/**
 * @brief Converts minimizers to sorted position, hash, and strand tuples.
 * @tparam p_minimizer_type Packed minimizer record.
 * @param minimizers Records to convert.
 * @return Sorted tuples suitable for parity comparison.
 */
template <class p_minimizer_type>
[[nodiscard]] inline std::vector<minimizer_tuple_type>
triples(const std::vector<p_minimizer_type>& minimizers) {
    std::vector<minimizer_tuple_type> out;
    out.reserve(minimizers.size());
    for (const p_minimizer_type& m : minimizers) {
        out.emplace_back(m.last_pos(), m.kmer(), m.strand());
    }
    std::sort(out.begin(), out.end());
    return out;
}

/**
 * @brief Formats a bounded tuple sample for failed parity diagnostics.
 * @param values Sorted tuple values.
 * @param limit Maximum tuples to include.
 * @return Human-readable tuple sample.
 */
[[nodiscard]] inline std::string tuple_examples(const std::vector<minimizer_tuple_type>& values,
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

/**
 * @brief Requires scalar tuples to appear in the GPU result with bounded tile extras.
 * @param scalar Scalar reference tuples.
 * @param gpu GPU tuples.
 * @param tile_plan Dispatch tiling used by the GPU result.
 * @param k K-mer length.
 * @param window Minimizer window length.
 * @param bp_offset Biological offset used by callers.
 */
inline void
require_gpu_triple_superset_with_bounded_extras(std::vector<minimizer_tuple_type> scalar,
                                                std::vector<minimizer_tuple_type> gpu,
                                                vk::gpu_sketch_tile_plan tile_plan,
                                                std::uint32_t k,
                                                std::uint32_t window,
                                                std::uint64_t bp_offset = 0) {
    std::sort(scalar.begin(), scalar.end());
    std::sort(gpu.begin(), gpu.end());

    std::vector<minimizer_tuple_type> missing;
    std::vector<minimizer_tuple_type> extras;
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

/**
 * @brief Applies tuple-superset checks to packed minimizer records.
 * @tparam p_minimizer_type Packed minimizer record.
 * @param scalar Scalar reference records.
 * @param gpu GPU records.
 * @param tile_plan Dispatch tiling used by the GPU result.
 * @param k K-mer length.
 * @param window Minimizer window length.
 * @param bp_offset Biological offset used by callers.
 */
template <class p_minimizer_type>
inline void require_gpu_minimizer_superset_with_bounded_tile_extras(
    const std::vector<p_minimizer_type>& scalar,
    const std::vector<p_minimizer_type>& gpu,
    vk::gpu_sketch_tile_plan tile_plan,
    std::uint32_t k,
    std::uint32_t window,
    std::uint64_t bp_offset = 0) {
    require_gpu_triple_superset_with_bounded_extras(
        triples(scalar), triples(gpu), tile_plan, k, window, bp_offset);
}

/**
 * @brief Applies tuple-superset checks to hash and encoded-position pairs.
 * @param scalar Scalar reference pairs.
 * @param gpu GPU pairs.
 * @param tile_plan Dispatch tiling used by the GPU result.
 * @param k K-mer length.
 * @param window Minimizer window length.
 * @param bp_offset Biological offset used by callers.
 */
inline void require_gpu_pair_superset_with_bounded_tile_extras(
    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& scalar,
    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& gpu,
    vk::gpu_sketch_tile_plan tile_plan,
    std::uint32_t k,
    std::uint32_t window,
    std::uint64_t bp_offset = 0) {
    auto to_triples = [](const auto& pairs) {
        std::vector<minimizer_tuple_type> out;
        out.reserve(pairs.size());
        for (const auto& [kmer, encoded_pos] : pairs) {
            out.emplace_back(encoded_pos >> 1u, kmer, encoded_pos & 1u);
        }
        return out;
    };
    require_gpu_triple_superset_with_bounded_extras(
        to_triples(scalar), to_triples(gpu), tile_plan, k, window, bp_offset);
}

/**
 * @brief Computes the scalar minimizer reference for a fixture sequence.
 * @tparam p_minimizer_type Packed minimizer record.
 * @tparam p_alphabet Sequence alphabet.
 * @param data Fixture symbols.
 * @param w Minimizer window length.
 * @param k K-mer length.
 * @return Scalar minimizer records in source order.
 */
template <class p_minimizer_type, class p_alphabet>
[[nodiscard]] inline std::vector<p_minimizer_type> cpu_sketch(
    std::vector<p_alphabet>& data, std::size_t w, std::size_t k) {
    using seq_type = sequence_custom_alphabet<p_alphabet, std::uint64_t>;
    auto seq = compact_sequence_range<seq_type>(data);
    basic_sketch_collector<p_minimizer_type> collector;
    collect_minimizers<minimap_int_hasher, p_minimizer_type, seq_type,
                       decltype(seq.begin()), std::uint64_t>(
        collector, seq, seq.begin(), seq.end(), w, k);
    return std::move(collector).extract_sketches_data();
}

/**
 * @brief Computes Vulkan minimizers for a resident fixture sequence.
 * @tparam p_minimizer_type Packed minimizer record.
 * @tparam p_alphabet Sequence alphabet.
 * @param data Fixture symbols.
 * @param w Minimizer window length.
 * @param k K-mer length.
 * @return GPU minimizers, or an empty vector when dispatch declines.
 */
template <class p_minimizer_type, class p_alphabet>
[[nodiscard]] inline std::vector<p_minimizer_type> gpu_sketch(
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

/** @brief Deterministic dna4 fixture shared by parity and segmented tests. */
inline std::vector<seq::dna4> dna4_fixture{
    "ACGTACGTGGCATTACGATCGATCGTAGCTAGCTACGTACGATCGATCGTAGCTAGCATCGATCG"
    "TTACGATCGTAGCTAGCTTACGTACGATCGATCGTAGCTAGCATCGATCGTAGCTAGCTACGTAC"
    "GGCATTACGATCGATCGTAGCTAGCTACGTACGATCGATCGTAGCTAGCATCGATCGTAGCTAGC"_dna4
};
/** @brief Deterministic dna5 fixture containing thymine without ambiguity. */
inline std::vector<seq::dna5> dna5_with_t{
    "ACGTACGTGGCATTACGATCGATCGTAGCTAGCTACGTACGATCGATCGTAGCTAGCATCGATCG"
    "TTACGATCGTAGCTAGCTTACGTACGATCGATCGTAGCTAGCATCGATCGTAGCTAGCTACGTAC"_dna5
};
/** @brief Deterministic dna5 fixture containing ambiguity resets. */
inline std::vector<seq::dna5> dna5_with_n{
    "ACGTACGTGGCATTACGNNNNNATCGTAGCTAGCTACGTACGATCGATCGTAGCTAGCATCGATC"
    "GTTACGATNNCGTAGCTAGCTTACGTACGATCGATCGTAGCTAGCATCGATCGTAGCTAGCTACG"_dna5
};

/**
 * @brief Requires device-only batch output to match host materialization exactly.
 * @tparam p_alphabet Sequence alphabet.
 * @param first_data First batch record.
 * @param second_data Second batch record.
 */
template <class p_alphabet>
inline void require_exact_device_only_batch_parity(
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

} // namespace bioseq::simd::HWY_NAMESPACE::vulkan_minimizers_test_support

HWY_AFTER_NAMESPACE();
