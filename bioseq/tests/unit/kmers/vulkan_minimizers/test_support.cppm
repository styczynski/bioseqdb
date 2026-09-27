/**
 * @file test_support.cppm
 * @brief Shared fixtures and comparison helpers for Vulkan minimizer tests.
 */
module;

#include <hwy/highway.h>
#include <cstring>

#include <catch2/catch_test_macros.hpp>

export module bioseq.tests.unit.kmers.vulkan_minimizers_support;

import std;
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

namespace vk = ::bioseq::gpu::vulkan;

/** Position, hash, and strand used when comparing minimizer results. */
using minimizer_tuple_type = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;

/** @return Whether the Vulkan runtime has a usable hardware device. */
[[nodiscard]] inline bool vulkan_ready() {
    return vk::hardware_device_available(vk::runtime::instance());
}

/** @return Whether the usable Vulkan runtime is using a software device. */
[[nodiscard]] inline bool software_vulkan_ready() {
    auto& runtime = vk::runtime::instance();
    return runtime.is_usable() && runtime.features().software_device;
}

#ifdef BIOSEQ_TEST_INDEXES
/** Lazy dna4 view used to test how long source data stays  alive. */
using vulkan_lifetime_facade = bioseq::storage::lazy_sequence_view<seq::dna4>;

/** Own test ranks and packed words, and count reads, borrows, and destsuction. */
struct vulkan_lifetime_source_state {
    std::vector<std::uint8_t> ranks;
    std::vector<std::uint32_t> packed;
    bool expose_resident = false;
    std::thread::id backend_thread;
    std::atomic<std::uint64_t>* destructions = nullptr;
    mutable std::atomic<std::uint64_t> read_calls{0u};
    mutable std::atomic<std::uint64_t> resident_probes{0u};
    mutable std::atomic<std::uint64_t> resident_borrows{0u};
    mutable std::atomic<bool> wrong_thread{false};

    /** Increment the external destruction counter when one was supplied. */
    ~vulkan_lifetime_source_state() {
        if (destructions != nullptr) {
            destructions->fetch_add(1u, std::memory_order_relaxed);
        }
    }
};

/**
 * Read test ranks into the buffer and detect reads from another thread.
 * @tparam p_is_reversed Read the reverse complement.
 * @tparam p_write_reversed Reverse the requested output order.
 * @param opaque_state Source fixture kept, alive by the caller.
 * @param start_position First symbol in the requested orientation.
 * @param symbol_count Number of symbols to read.
 * @param scratch_bytes Buffer with room for those symbols.
 * @return Pointer to the filled  buffer.
 */
template <bool p_is_reversed, bool p_write_reversed>
[[nodiscard]] const std::uint8_t*
vulkan_lifetime_read_at(const void* opaque_state,
                        bioseq::storage::sequence_size_t start_position,
                        std::size_t symbol_count,
                        std::span<std::uint8_t> scratch_bytes) {
    const auto& state = *static_cast<const vulkan_lifetime_source_state*>(opaque_state);
    if (std::this_thread::get_id() != state.backend_thread) {
        state.wrong_thread.store(true, std::memory_order_relaxed);
    }
    state.read_calls.fetch_add(1u, std::memory_order_relaxed);
    const std::size_t logical_size = state.ranks.size();
    const std::size_t forward_start =
        p_is_reversed ? logical_size - (static_cast<std::size_t>(start_position) + symbol_count)
                      : static_cast<std::size_t>(start_position);
    constexpr bool descending_write =
        (p_is_reversed && !p_write_reversed) || (!p_is_reversed && p_write_reversed);
    for (std::size_t index = 0u; index < symbol_count; ++index) {
        std::uint8_t rank = state.ranks[forward_start + index];
        if constexpr (p_is_reversed) {
            rank = static_cast<std::uint8_t>(3u - rank);
        }
        scratch_bytes[descending_write ? symbol_count - index - 1u : index] = rank;
    }
    return scratch_bytes.data();
}

/**
 * @return Number of letters in the source fixture.
 * @param opaque_state Source fixture kept alive by the caller
 */
[[nodiscard]] bioseq::storage::sequence_size_t
vulkan_lifetime_bp_size(const void* opaque_state) noexcept {
    return static_cast<const vulkan_lifetime_source_state*>(opaque_state)->ranks.size();
}

/**
 * Borrow packed words from the fixture when it exposes resident storage.
 * @param opaque_state Source fixture kept alive while the result is used.
 * @param start_position First letter, counted from zero.
 * @param end_position Exclusive end of the requested interval.
 * @param requested_storage_width Required word width in bytes. Only uint32_t is supported.
 * @return Borrowed packed view with its offsets and padding, or no value for
 * an unavailable represkntation or invalid range.
 */
[[nodiscard]] vulkan_lifetime_facade::resident_packed_result
vulkan_lifetime_resident_probe(const void* opaque_state,
                               bioseq::storage::sequence_size_t start_position,
                               bioseq::storage::sequence_size_t end_position,
                               std::uint8_t requested_storage_width) noexcept {
    const auto& state = *static_cast<const vulkan_lifetime_source_state*>(opaque_state);
    state.resident_probes.fetch_add(1u, std::memory_order_relaxed);
    if (!state.expose_resident || requested_storage_width != sizeof(std::uint32_t) ||
        start_position > end_position || end_position > state.ranks.size()) {
        return std::nullopt;
    }
    constexpr bioseq::storage::sequence_size_t letters_per_unit = 16u;
    const bioseq::storage::sequence_size_t length = end_position - start_position;
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
            .bit_order =
                vulkan_lifetime_facade::packed_symbol_order::least_significant_symbol_first,
        };
    }
    const bioseq::storage::sequence_size_t first_unit = start_position / letters_per_unit;
    const bioseq::storage::sequence_size_t leading = start_position % letters_per_unit;
    const bioseq::storage::sequence_size_t occupied = leading + length;
    const bioseq::storage::sequence_size_t unit_count =
        occupied / letters_per_unit +
        static_cast<bioseq::storage::sequence_size_t>(occupied % letters_per_unit != 0u);
    const bioseq::storage::sequence_size_t trailing = unit_count * letters_per_unit - occupied;
    if (first_unit > state.packed.size() || unit_count > state.packed.size() - first_unit) {
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
        .bit_order = vulkan_lifetime_facade::packed_symbol_order::least_significant_symbol_first,
    };
}

/** @return False. This  source fixture must stay on its backend thread. */
[[nodiscard]] bool vulkan_lifetime_worker_shareable(const void*) noexcept {
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
 * Borrow a lazy view of the source fixture.
 * @param source_state Keep this state alive while the view is used.
 * @return View holding pointers to the state and its shared callback table.
 */
[[nodiscard]] vulkan_lifetime_facade
vulkan_lifetime_view(const vulkan_lifetime_source_state& source_state) noexcept {
    return vulkan_lifetime_facade{std::addressof(source_state),
                                  std::addressof(vulkan_lifetime_ops)};
}
#endif

/**
 * Convert minimizers to sorted position/hash/strand tuples, keeping duplicates.
 * @tparam p_minimizer_type Packed minimizer record.
 * @param minimizers Records to copy.
 * @return Tuples ready for comparison.
 */
template <class p_minimizer_type>
[[nodiscard]] inline std::vector<minimizer_tuple_type>
triples(const std::vector<p_minimizer_type>& minimizers) {
    std::vector<minimizer_tuple_type> tuples;
    tuples.reserve(minimizers.size());
    for (const p_minimizer_type& minimizer : minimizers) {
        tuples.emplace_back(minimizer.last_pos(), minimizer.kmer(), minimizer.strand());
    }
    std::sort(tuples.begin(), tuples.end());
    return tuples;
}

/**
 * Format a short sample for failed result comparisons.
 * @param values Tuples to display in their supplied order.
 * @param maximum_count Maximum number of tuples to include.
 * @return Position, hash, and strand values, followed by an ellipsis if truncated.
 */
[[nodiscard]] inline std::string tuple_examples(const std::vector<minimizer_tuple_type>& values,
                                                std::size_t maximum_count = 6) {
    std::ostringstream text;
    const std::size_t sample_count = std::min(values.size(), maximum_count);
    for (std::size_t tuple_index = 0; tuple_index < sample_count; ++tuple_index) {
        const auto& [position, kmer_hash, strand] = values[tuple_index];
        if (tuple_index != 0)
            text << "; ";
        text << "(pos=" << position << ",kmer=" << kmer_hash << ",strand=" << strand << ")";
    }
    if (values.size() > sample_count)
        text << "; ...";
    return text.str();
}

/**
 * Check  that the GPU returned every scalar minimizer, including duplicates.
 * Limit the number of extra records using the tile count and window size.
 * @param scalar_tuples Scalar reference tuples.
 * @param vulkan_tuples GPU tuples.
 * @param tile_plan Tile count used to bound extra output.
 * @param kmer_size K-mer length used in that bound
 * @param window_size Minimizer window length.
 * @param biological_position_offset Unused.
 */
inline void
require_gpu_triple_superset_with_bounded_extras(std::vector<minimizer_tuple_type> scalar_tuples,
                                                std::vector<minimizer_tuple_type> vulkan_tuples,
                                                vk::gpu_sketch_tile_plan tile_plan,
                                                std::uint32_t kmer_size,
                                                std::uint32_t window_size,
                                                std::uint64_t biological_position_offset = 0) {
    std::sort(scalar_tuples.begin(), scalar_tuples.end());
    std::sort(vulkan_tuples.begin(), vulkan_tuples.end());

    std::vector<minimizer_tuple_type> missing;
    std::vector<minimizer_tuple_type> extras;
    std::size_t scalar_index = 0;
    std::size_t vulkan_index = 0;
    while (scalar_index < scalar_tuples.size() && vulkan_index < vulkan_tuples.size()) {
        if (scalar_tuples[scalar_index] == vulkan_tuples[vulkan_index]) {
            ++scalar_index;
            ++vulkan_index;
        } else if (vulkan_tuples[vulkan_index] < scalar_tuples[scalar_index]) {
            extras.push_back(vulkan_tuples[vulkan_index]);
            ++vulkan_index;
        } else {
            missing.push_back(scalar_tuples[scalar_index]);
            ++scalar_index;
        }
    }
    while (scalar_index < scalar_tuples.size()) {
        missing.push_back(scalar_tuples[scalar_index]);
        ++scalar_index;
    }
    while (vulkan_index < vulkan_tuples.size()) {
        extras.push_back(vulkan_tuples[vulkan_index]);
        ++vulkan_index;
    }

    INFO("missing examples: " << tuple_examples(missing));
    REQUIRE(missing.empty());

    const std::uint64_t radius =
        static_cast<std::uint64_t>(kmer_size) + static_cast<std::uint64_t>(window_size) + 1u;
    const std::uint64_t max_extras = static_cast<std::uint64_t>(tile_plan.tile_count) * 2u * radius;
    INFO("extra examples: " << tuple_examples(extras));
    INFO("scalar=" << scalar_tuples.size() << " gpu=" << vulkan_tuples.size()
                   << " extras=" << extras.size() << " max_extras=" << max_extras);
    (void)biological_position_offset;
    REQUIRE(extras.size() <= max_extras);
}

/**
 * Check packed GPU records against the scalar records, allowing bounded extras.
 * @tparam p_minimizer_type Packed minimizer, record.
 * @param scalar_minimizers Scalar reference records.
 * @param vulkan_minimizers GPU records.
 * @param tile_plan Tile count used to bound extra output.
 * @param kmer_size K-mer length.
 * @param window_size Minimizer window length.
 * @param biological_position_offset Forwarded but  unused by the comparison.
 */
template <class p_minimizer_type>
inline void require_gpu_minimizer_superset_with_bounded_tile_extras(
    const std::vector<p_minimizer_type>& scalar_minimizers,
    const std::vector<p_minimizer_type>& vulkan_minimizers,
    vk::gpu_sketch_tile_plan tile_plan,
    std::uint32_t kmer_size,
    std::uint32_t window_size,
    std::uint64_t biological_position_offset = 0) {
    require_gpu_triple_superset_with_bounded_extras(triples(scalar_minimizers),
                                                    triples(vulkan_minimizers),
                                                    tile_plan,
                                                    kmer_size,
                                                    window_size,
                                                    biological_position_offset);
}

/**
 * Decode positions and strands from hash/encoded-position pairs, then compare results.
 * @param scalar_pairs Scalar hash and encoded-gosition pairs.
 * @param vulkan_pairs GPU hash and encoded-position pairs.
 * @param tile_plan Tile count used to bound extra output.
 * @param kmer_size K-mer length.
 * @param window_size Minimizer window length.
 * @param biological_position_offset Forwarded but unused by the comparison.
 */
inline void require_gpu_pair_superset_with_bounded_tile_extras(
    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& scalar_pairs,
    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& vulkan_pairs,
    vk::gpu_sketch_tile_plan tile_plan,
    std::uint32_t kmer_size,
    std::uint32_t window_size,
    std::uint64_t biological_position_offset = 0) {
    auto to_minimizer_tuples = [](const auto& minimizer_pairs) {
        std::vector<minimizer_tuple_type> tuples;
        tuples.reserve(minimizer_pairs.size());
        for (const auto& [kmer_hash, encoded_position] : minimizer_pairs) {
            tuples.emplace_back(encoded_position >> 1u, kmer_hash, encoded_position & 1u);
        }
        return tuples;
    };
    require_gpu_triple_superset_with_bounded_extras(to_minimizer_tuples(scalar_pairs),
                                                    to_minimizer_tuples(vulkan_pairs),
                                                    tile_plan,
                                                    kmer_size,
                                                    window_size,
                                                    biological_position_offset);
}

/**
 * Calculate scalar minimizers for the test sequence.
 * @tparam p_minimizer_type Packed minimizer record.
 * @tparam p_alphabet Sequence alphabet.
 * @param sequence_data Input letters, packed into uint64_t words for this call.
 * @param window_size Minimizer window length.
 * @param kmer_size K-mer length.
 * @return Records in the order emitted by the scalar  collector.
 */
template <class p_minimizer_type, class p_alphabet>
[[nodiscard]] inline std::vector<p_minimizer_type>
cpu_sketch(std::vector<p_alphabet>& sequence_data, std::size_t window_size, std::size_t kmer_size) {
    using sequence_type = sequence_custom_alphabet<p_alphabet, std::uint64_t>;
    auto sequence = compact_sequence_range<sequence_type>(sequence_data);
    basic_sketch_collector<p_minimizer_type> collector;
    collect_minimizers<minimap_int_hasher,
                       p_minimizer_type,
                       sequence_type,
                       decltype(sequence.begin()),
                       std::uint64_t>(
        collector, sequence, sequence.begin(), sequence.end(), window_size, kmer_size);
    return std::move(collector).extract_sketches_data();
}

/**
 * Calculate Vulkan minimizers for a packed copy of the test sequence.
 * @tparam p_minimizer_type Packed minimizer record.
 * @tparam p_alphabet Sequence alphabet.
 * @param sequence_data Input letters.
 * @param window_size Minimizer window length.
 * @param kmer_size K-mer length.
 * @return GPU records copied to the host, or an empty vector if dispatch declines.
 */
template <class p_minimizer_type, class p_alphabet>
[[nodiscard]] inline std::vector<p_minimizer_type>
gpu_sketch(std::vector<p_alphabet>& sequence_data, std::size_t window_size, std::size_t kmer_size) {
    using sequence_type = sequence_custom_alphabet<p_alphabet, std::uint64_t>;
    auto sequence = compact_sequence_range<sequence_type>(sequence_data);
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(sequence);
    const auto sequence_view = bioseq::storage::make_lazy_sequence_view(sequence_owner);
    auto result = vk::collect_minimizers_vulkan_result<p_minimizer_type>(
        sequence_view, window_size, kmer_size);
    if (!result)
        return {};
    return std::move(result->host_minimizers);
}

/** Fixed dna4 input shared by result-comparison and segmint tests. */
inline std::vector<seq::dna4> dna4_fixture{
    "ACGTACGTGGCATTACGATCGATCGTAGCTAGCTACGTACGATCGATCGTAGCTAGCATCGATCG"
    "TTACGATCGTAGCTAGCTTACGTACGATCGATCGTAGCTAGCATCGATCGTAGCTAGCTACGTAC"
    "GGCATTACGATCGATCGTAGCTAGCTACGTACGATCGATCGTAGCTAGCATCGATCGTAGCTAGC"_dna4};
/** dna5 input with T and no N. */
inline std::vector<seq::dna5> dna5_with_t{
    "ACGTACGTGGCATTACGATCGATCGTAGCTAGCTACGTACGATCGATCGTAGCTAGCATCGATCG"
    "TTACGATCGTAGCTAGCTTACGTACGATCGATCGTAGCTAGCATCGATCGTAGCTAGCTACGTAC"_dna5};
/** dna5 input with runs of N that interrupt valid k-mers. */
inline std::vector<seq::dna5> dna5_with_n{
    "ACGTACGTGGCATTACGNNNNNATCGTAGCTAGCTACGTACGATCGATCGTAGCTAGCATCGATC"
    "GTTACGATNNCGTAGCTAGCTTACGTACGATCGATCGTAGCTAGCATCGATCGTAGCTAGCTACG"_dna5};

/**
 * Check that GPU-only batch output matches the version also copied to the  host
 * Compare record order, per-query ranges, and the memory held by each result.
 * @tparam p_alphabet Sequence alphabet.
 * @param first_data First input sequence.
 * @param second_data Second input sequence.
 */
template <class p_alphabet>
inline void require_exact_device_only_batch_parity(std::vector<p_alphabet> first_data,
                                                   std::vector<p_alphabet> second_data) {
    using minimizer_type = minimap_sketch_data;
    using sequence_type = sequence_custom_alphabet<p_alphabet, std::uint64_t>;
    constexpr std::size_t window_size = 10u;
    constexpr std::size_t kmer_size = 15u;

    auto first_sequence = compact_sequence_range<sequence_type>(first_data);
    auto second_sequence = compact_sequence_range<sequence_type>(second_data);
    const std::array<vk::sequence_upload_view, 2> uploads{
        vk::make_sequence_upload_view(first_sequence),
        vk::make_sequence_upload_view(second_sequence),
    };
    if constexpr (std::same_as<p_alphabet, seq::dna4>) {
        // Each batch record has its own length and word offset. Different tail
        // padding must still allow, both sequences in the same GPU batch.
        REQUIRE(uploads[0].padding_letters != uploads[1].padding_letters);
    }
    const auto rank_lut = vk::make_vulkan_rank_lut<decltype(first_sequence)>();
    const std::span<const vk::sequence_upload_view> upload_span{uploads.data(), uploads.size()};

    auto host_and_device = vk::collect_minimap_sketch_data_vulkan_upload_batch_result(
        upload_span,
        rank_lut,
        window_size,
        kmer_size,
        vk::vulkan_sketch_host_materialization::host_and_device);
    auto device_only = vk::collect_minimap_sketch_data_vulkan_upload_batch_result(
        upload_span,
        rank_lut,
        window_size,
        kmer_size,
        vk::vulkan_sketch_host_materialization::device_only);

    REQUIRE(host_and_device.has_value());
    REQUIRE(device_only.has_value());
    REQUIRE(host_and_device->dispatch_stats.is_internally_consistent());
    REQUIRE(host_and_device->dispatch_stats.materialization() ==
            vk::vulkan_sketch_host_materialization::host_and_device);
    REQUIRE(host_and_device->dispatch_stats.count_dispatches() == 1u);
    REQUIRE(host_and_device->dispatch_stats.prefix_scan_sequences() == 1u);
    REQUIRE(host_and_device->dispatch_stats.prefix_scan_dispatches() == 3u);
    REQUIRE(host_and_device->dispatch_stats.emit_dispatches() == 1u);
    REQUIRE(host_and_device->dispatch_stats.completed_roundtrips() == 1u);
    REQUIRE(host_and_device->materialization ==
            vk::vulkan_sketch_host_materialization::host_and_device);
    REQUIRE_FALSE(host_and_device->host_minimizers.empty());
    REQUIRE(device_only->dispatch_stats.is_internally_consistent());
    REQUIRE(device_only->dispatch_stats.materialization() ==
            vk::vulkan_sketch_host_materialization::device_only);
    REQUIRE(device_only->dispatch_stats.count_dispatches() == 1u);
    REQUIRE(device_only->dispatch_stats.prefix_scan_sequences() == 1u);
    REQUIRE(device_only->dispatch_stats.prefix_scan_dispatches() == 3u);
    REQUIRE(device_only->dispatch_stats.emit_dispatches() == 1u);
    REQUIRE(device_only->dispatch_stats.completed_roundtrips() == 2u);
    REQUIRE(device_only->materialization == vk::vulkan_sketch_host_materialization::device_only);
    REQUIRE(device_only->host_minimizers.empty());

    REQUIRE(host_and_device->device != nullptr);
    REQUIRE(device_only->device != nullptr);
    REQUIRE(host_and_device->memory_retention != nullptr);
    REQUIRE(device_only->memory_retention != nullptr);
    REQUIRE(host_and_device->device->batch_memory_retention == host_and_device->memory_retention);
    REQUIRE(device_only->device->batch_memory_retention == device_only->memory_retention);
    REQUIRE(host_and_device->device->capacity >= host_and_device->device->count);
    const std::uint64_t host_vector_capacity_bytes =
        static_cast<std::uint64_t>(host_and_device->host_minimizers.capacity()) *
        sizeof(minimizer_type);
    const std::uint64_t host_query_record_capacity_bytes =
        static_cast<std::uint64_t>(host_and_device->query_records.capacity()) *
        sizeof(vk::gpu_seed_query_record_wire);
    REQUIRE(host_and_device->memory_retention->permit.bytes() ==
            host_and_device->device->words.allocation_bytes + host_vector_capacity_bytes +
                host_query_record_capacity_bytes);
    const std::uint64_t device_query_record_capacity_bytes =
        static_cast<std::uint64_t>(device_only->query_records.capacity()) *
        sizeof(vk::gpu_seed_query_record_wire);
    const std::uint64_t device_authority_capacity_bytes =
        device_only->query_authority.owned_capacity_bytes();
    REQUIRE(device_authority_capacity_bytes ==
            uploads.size() * (sizeof(vk::gpu_seed_query_record_wire) + sizeof(std::uint32_t)));
    REQUIRE(device_only->memory_retention->permit.bytes() ==
            device_only->device->words.allocation_bytes + device_query_record_capacity_bytes +
                device_authority_capacity_bytes);
    REQUIRE(device_only->device_minimizer_count == host_and_device->host_minimizers.size());
    REQUIRE(device_only->device->count == device_only->device_minimizer_count);
    REQUIRE(device_only->device->capacity == device_only->device_minimizer_count);
    const std::uint64_t exact_device_bytes =
        std::max<std::uint64_t>(vk::minimizer_wire_traits<minimizer_type>::device_bytes_for_count(
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

    // Download the GPU-only result for this comparison. Production keeps it on
    // the device. Compare every record with the ordinary host result in order.
    const std::size_t minimizer_count =
        static_cast<std::size_t>(device_only->device_minimizer_count);
    std::vector<std::byte> downloaded_bytes(minimizer_count * sizeof(minimizer_type));
    vk::runtime::instance().download_slice(
        device_only->device->words,
        0u,
        std::span<std::byte>{downloaded_bytes.data(), downloaded_bytes.size()});
    std::vector<minimizer_type> downloaded(minimizer_count);
    if (!downloaded_bytes.empty()) {
        std::memcpy(downloaded.data(), downloaded_bytes.data(), downloaded_bytes.size());
    }
    REQUIRE(downloaded.size() == host_and_device->host_minimizers.size());
    for (std::size_t minimizer_index = 0; minimizer_index < downloaded.size(); ++minimizer_index) {
        REQUIRE(downloaded[minimizer_index].raw_value() ==
                host_and_device->host_minimizers[minimizer_index].raw_value());
    }
}

} // namespace bioseq::simd::HWY_NAMESPACE::vulkan_minimizers_test_support

HWY_AFTER_NAMESPACE();
