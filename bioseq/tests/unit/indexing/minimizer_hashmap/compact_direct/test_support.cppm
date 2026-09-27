/**
 * @file compact_direct/test_support.cppm
 * @brief Builds compact index fixtures and compares their query results.
 */
module;

#include <hwy/highway.h>
#include <sys/mman.h>
#include <catch2/catch_test_macros.hpp>

export module bioseq.tests.unit.indexing.minimizer_hashmap.compact_direct_support;

import std;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.context;
import bioseq.common.seqan_adapter;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.sequences;
import bioseq.chaining.core;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.sequences.kmers.minimap2_sketch_baseline;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.pipeline.minimap_port;
import bioseq.gpu.vulkan.runtime;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.symbol_counts;
import bioseq.parallelism.memory_admission;
import bioseq.indexing.registry;
import bioseq.common.hashing;

import bioseq.tests.support.indexing_storage_fakes;
import bioseq.tests.unit.indexing.minimizer_hashmap_support;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
using namespace minimizer_hashmap_test_support;

export namespace compact_direct_test_support {

/// A hash run with its encoded target positions.
struct fragment_compact_direct_run {
    bioseq::indexing::compact_hash32_sorted_run_header header{};
    std::vector<std::uint64_t> encoded_positions{};
};

/// Hash runs supplied to the compact index builder.
struct fragment_compact_direct_source {
    std::vector<fragment_compact_direct_run> runs{};
};

/// Pass each stored run to the receiver, stopping on its first error.
[[nodiscard]] std::expected<void, bioseq::indexing::direct_inverted_index_error>
scan_fragment_compact_direct_runs(
    void* opaque, const bioseq::indexing::compact_hash32_sorted_run_receiver& receiver) {
    const auto& source = *static_cast<const fragment_compact_direct_source*>(opaque);
    for (const fragment_compact_direct_run& run : source.runs) {
        if (auto begun = receiver.begin_run(run.header); !begun) {
            return std::unexpected(begun.error());
        }
        if (auto consumed =
                receiver.consume_positions(std::span<const std::uint64_t>{run.encoded_positions});
            !consumed) {
            return std::unexpected(consumed.error());
        }
    }
    return {};
}

/// Keep an encoded compact-direct image alive with its validated view.
struct fragment_compact_direct_test_index {
    std::shared_ptr<std::vector<std::byte>> image{};
    bioseq::indexing::compact_hash32_direct_index_view view{};
};

/// Build a compact index from hash-position pairs using the generation identity.
/// Positions are grouped by hash, then sorted by strand and coordinate.
[[nodiscard]] fragment_compact_direct_test_index make_fragment_compact_direct_test_index(
    const bioseq::indexing::minimizer_generation_head& head,
    const bioseq::indexing::minimizer_generation_manifest& manifest,
    std::span<const std::pair<std::uint64_t, std::uint64_t>> pairs,
    std::uint32_t max_occ_cap) {
    using namespace bioseq::indexing;
    const auto profile = derive_grouped_soa_minimizer_profile(manifest);
    REQUIRE(profile.has_value());
    REQUIRE(profile->max_hash <= (std::numeric_limits<std::uint32_t>::max)());
    const std::uint32_t geometry_max_hash = static_cast<std::uint32_t>(profile->max_hash);
    const std::uint8_t hash_domain_bits =
        static_cast<std::uint8_t>(std::bit_width(geometry_max_hash));
    REQUIRE(hash_domain_bits >= 9u);
    REQUIRE(hash_domain_bits <= 32u);
    REQUIRE((hash_domain_bits == 32u
                 ? (std::numeric_limits<std::uint32_t>::max)()
                 : static_cast<std::uint32_t>((std::uint64_t{1u} << hash_domain_bits) - 1u)) ==
            geometry_max_hash);
    std::map<std::uint32_t, std::vector<std::uint64_t>> grouped;
    for (const auto [hash, encoded] : pairs) {
        REQUIRE(hash <= geometry_max_hash);
        grouped[static_cast<std::uint32_t>(hash)].push_back(encoded);
    }
    fragment_compact_direct_source source;
    source.runs.reserve(grouped.size());
    for (auto& [hash, positions] : grouped) {
        std::sort(positions.begin(),
                  positions.end(),
                  [](std::uint64_t left, std::uint64_t right) noexcept {
                      return std::pair{left & 1u, left >> 1u} < std::pair{right & 1u, right >> 1u};
                  });
        const std::uint64_t strand_zero_count = static_cast<std::uint64_t>(std::ranges::count_if(
            positions, [](std::uint64_t encoded) noexcept { return (encoded & 1u) == 0u; }));
        source.runs.push_back(fragment_compact_direct_run{
            .header =
                compact_hash32_sorted_run_header{
                    .hash = hash,
                    .global_count = positions.size(),
                    .target_strand_zero_count = strand_zero_count,
                },
            .encoded_positions = std::move(positions),
        });
    }
    const direct_index_identity identity{
        .generation_id = head.generation_id,
        .source_identity = head.source_identity,
        .storage_contract_checksum = head.storage_contract_checksum,
        .target_length = manifest.target_length,
    };
    const compact_hash32_geometry geometry{
        .hash_domain_bits = hash_domain_bits,
        .max_hash = geometry_max_hash,
    };
    const compact_hash32_build_options options{
        .identity = identity,
        .max_occ_cap = max_occ_cap,
        .geometry = geometry,
    };
    const compact_hash32_sorted_run_source source_ref{
        .context = &source,
        .scan_sorted_runs_fn = &scan_fragment_compact_direct_runs,
    };
    auto built = build_compact_hash32_direct_index(source_ref, options);
    REQUIRE(built.has_value());
    auto image = std::make_shared<std::vector<std::byte>>(std::move(*built));
    const auto validated =
        validate_compact_hash32_direct_index(*image, identity, max_occ_cap, geometry);
    REQUIRE(validated.has_value());
    return fragment_compact_direct_test_index{
        .image = std::move(image),
        .view = *validated,
    };
}

/// Keep the fixture image alive while a reader uses its view.
[[nodiscard]] bioseq::indexing::owned_compact_hash32_direct_index_view
own_fragment_compact_direct_test_index(
    const fragment_compact_direct_test_index& compact,
    std::optional<bioseq::indexing::compact_hash32_direct_index_view> override_view =
        std::nullopt) {
    REQUIRE(compact.image != nullptr);
    const auto view = override_view.value_or(compact.view);
    std::shared_ptr<const std::byte> image_alias{compact.image, compact.image->data()};
    auto owned = bioseq::indexing::owned_compact_hash32_direct_index_view::bind(
        std::move(image_alias), view);
    REQUIRE(owned.has_value());
    return std::move(*owned);
}

/// Collect page-read hints in a fixed buffer for test assertions.
struct selected_page_advice_recorder {
    static constexpr std::size_t capacity = 128u;
    std::array<bioseq::indexing::compact_direct_selected_page_range, capacity> records{};
    std::atomic<std::size_t> record_count{0u};
    std::atomic<bool> overflow{false};
    std::atomic<bool> drop_advice{false};

    /// Record a page range, or mark overflow when the fixed buffer is full.
    static void advise(void* const opaque,
                       const std::uintptr_t page_begin,
                       const std::uintptr_t page_end) noexcept {
        auto& recorder = *static_cast<selected_page_advice_recorder*>(opaque);
        if (recorder.drop_advice.load(std::memory_order_relaxed))
            return;
        const std::size_t ordinal = recorder.record_count.fetch_add(1u, std::memory_order_relaxed);
        if (ordinal >= recorder.records.size()) {
            recorder.overflow.store(true, std::memory_order_relaxed);
            return;
        }
        recorder.records[ordinal] = bioseq::indexing::compact_direct_selected_page_range{
            .begin = page_begin,
            .end = page_end,
        };
    }

    /// Connect this recorder to a reader with the requested page size.
    [[nodiscard]] bioseq::indexing::compact_direct_selected_page_adviser
    token(const std::size_t page_bytes = 1u) noexcept {
        return bioseq::indexing::compact_direct_selected_page_adviser{
            .context = this,
            .page_bytes = page_bytes,
            .advise_fn = &selected_page_advice_recorder::advise,
        };
    }
};
/// Join all control data, followed by all positions, in directory order.
[[nodiscard]] std::shared_ptr<std::vector<std::byte>>
make_grouped_soa_mapped_payload(const bioseq::indexing::encoded_grouped_soa_generation& generation,
                                std::span<const collected_minimizer_row> rows) {
    auto payload = std::make_shared<std::vector<std::byte>>();
    payload->reserve(static_cast<std::size_t>(generation.manifest.payload_bytes));
    for (const auto& entry : generation.directory.entries()) {
        const auto controls = collect_encoded_extent(rows, entry, true);
        payload->insert(payload->end(), controls.begin(), controls.end());
    }
    for (const auto& entry : generation.directory.entries()) {
        const auto positions = collect_encoded_extent(rows, entry, false);
        payload->insert(payload->end(), positions.begin(), positions.end());
    }
    REQUIRE(payload->size() == generation.manifest.payload_bytes);
    return payload;
}

/// A shared memory mapping for tests with large offsets and counts.
struct sparse_mapped_test_payload {
    std::shared_ptr<std::byte> owner{};
    std::size_t size = 0u;

    /// Return the writable mapped byte range.
    [[nodiscard]] std::span<std::byte> bytes() const noexcept {
        return {owner.get(), size};
    }
};

/// Reserve zero-filled virtual memory for tests with large offsets and counts.
/// Tests can describe gigabyte position ranges while touching only a small
/// control prefix. An invalid size or failed mapping returns no value.
[[nodiscard]] std::optional<sparse_mapped_test_payload>
make_sparse_mapped_test_payload(std::uint64_t byte_count) {
    if (byte_count == 0u || byte_count > (std::numeric_limits<std::size_t>::max)()) {
        return std::nullopt;
    }
    const std::size_t size = static_cast<std::size_t>(byte_count);
    void* const mapping =
        mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED)
        return std::nullopt;
    return sparse_mapped_test_payload{
        .owner = std::shared_ptr<std::byte>{static_cast<std::byte*>(mapping),
                                            [size](std::byte* address) noexcept {
                                                (void)munmap(address, size);
                                            }},
        .size = size,
    };
}

/// Assemble stored rows and open a reader that keeps their bytes alive.
[[nodiscard]] std::expected<bioseq::indexing::prepared_minimizer_fragment_reader,
                            bioseq::indexing::minimizer_generation_encoding_error>
open_grouped_soa_fragment_query_reader(
    bioseq::parallelism::memory_admission_controller& admission,
    const bioseq::indexing::encoded_grouped_soa_generation& generation,
    std::span<const collected_minimizer_row> rows,
    std::uint64_t max_occ,
    std::optional<bioseq::indexing::owned_compact_hash32_direct_index_view> compact_direct =
        std::nullopt) {
    auto payload = make_grouped_soa_mapped_payload(generation, rows);
    std::shared_ptr<const void> payload_owner = payload;
    return bioseq::indexing::prepared_minimizer_fragment_reader::open(
        admission,
        generation.head,
        generation.manifest,
        test_grouped_soa_expected_storage_contract(generation.manifest.max_occ_cap),
        generation.directory.encoded_bytes(),
        max_occ,
        std::move(payload_owner),
        std::span<const std::byte>{payload->data(), payload->size()},
        std::move(compact_direct));
}

/// Build an in-memory hash index and collect reference anchors for comparison.
[[nodiscard]] std::vector<bioseq::chaining::anchor>
ordinary_seed_anchors(std::span<const std::pair<std::uint64_t, std::uint64_t>> pairs,
                      std::uint64_t target_length,
                      std::span<const minimap_sketch_data> query_minimizers,
                      bioseq::storage::sequence_size_t query_length,
                      std::int32_t max_occ,
                      std::int32_t kmer_size) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> owned_pairs{pairs.begin(), pairs.end()};
    bioseq::indexing::minimizer_hashmap_fragment_index index;
    index.build_from_pairs(owned_pairs, target_length);

    context::context ctx;
    context::installed_context_scope installed{ctx};
    auto scope = ctx.push<context::arena::chain>();
    auto anchors = bioseq::chaining::make_arena_slab<bioseq::chaining::anchor>();
    collect_seed_hits(query_minimizers, index, query_length, max_occ, kmer_size, anchors);
    if (anchors.size() == 0u)
        return {};
    return std::vector<bioseq::chaining::anchor>{anchors.data(), anchors.data() + anchors.size()};
}

/// Check that anchor bytes match and the result is in canonical order.
void require_anchor_bytes_equal(std::span<const bioseq::chaining::anchor> actual,
                                std::span<const bioseq::chaining::anchor> expected) {
    REQUIRE(actual.size() == expected.size());
    REQUIRE(std::ranges::equal(std::as_bytes(actual), std::as_bytes(expected)));
    REQUIRE(std::ranges::is_sorted(actual, bioseq::chaining::canonical_anchor_less{}));
}

/// Compare scan, sorting, and memory counters with the baseline matcher.
void require_hot_loop_differential_evidence_equal(
    const bioseq::indexing::prepared_fragment_anchor_batch_stats& actual,
    const bioseq::indexing::prepared_fragment_anchor_batch_stats& oracle) {
    REQUIRE(actual.waves == oracle.waves);
    REQUIRE(actual.queries == oracle.queries);
    REQUIRE(actual.query_occurrences == oracle.query_occurrences);
    REQUIRE(actual.unique_query_plans == oracle.unique_query_plans);
    REQUIRE(actual.packed_occurrence_waves == oracle.packed_occurrence_waves);
    REQUIRE(actual.wide_occurrence_waves == oracle.wide_occurrence_waves);
    REQUIRE(actual.control_count_bytes == oracle.control_count_bytes);
    REQUIRE(actual.rejected_control_validation_bytes == oracle.rejected_control_validation_bytes);
    REQUIRE(actual.pass2_control_bytes == oracle.pass2_control_bytes);
    REQUIRE(actual.pass2_accepted_prefix_bytes == oracle.pass2_accepted_prefix_bytes);
    REQUIRE(actual.pass2_position_bytes == oracle.pass2_position_bytes);
    REQUIRE(actual.pass3_control_bytes == oracle.pass3_control_bytes);
    REQUIRE(actual.pass3_accepted_prefix_bytes == oracle.pass3_accepted_prefix_bytes);
    REQUIRE(actual.pass3_position_bytes == oracle.pass3_position_bytes);
    REQUIRE(actual.anchors_constructed == oracle.anchors_constructed);
    REQUIRE(actual.local_sort_bytes == oracle.local_sort_bytes);
    REQUIRE(actual.tie_normalization_bytes == oracle.tie_normalization_bytes);
    REQUIRE(actual.exact_final_bytes == oracle.exact_final_bytes);
    REQUIRE(actual.peak_exact_final_bytes == oracle.peak_exact_final_bytes);
    REQUIRE(actual.global_sort_calls_invoked == oracle.global_sort_calls_invoked);
    REQUIRE(actual.global_sort_calls_skipped == oracle.global_sort_calls_skipped);
    REQUIRE(actual.compositional_sortedness_receipts == oracle.compositional_sortedness_receipts);
    REQUIRE(actual.final_sortedness_validation_bytes == oracle.final_sortedness_validation_bytes);
    REQUIRE(actual.forbidden_buffers_are_zero());
    REQUIRE(oracle.forbidden_buffers_are_zero());
    REQUIRE(actual.saturated == oracle.saturated);
}
/// Replacement directory bytes and the head and manifest that describe them.
struct rebuilt_generation_metadata {
    bioseq::indexing::minimizer_generation_head head{};
    bioseq::indexing::minimizer_generation_manifest manifest{};
    std::vector<std::byte> directory_bytes;
};

/// Update directory metadata and checksums after replacing directory entries.
[[nodiscard]] rebuilt_generation_metadata rebuild_generation_metadata(
    const bioseq::indexing::encoded_grouped_soa_generation& original,
    std::span<const bioseq::indexing::grouped_soa_bucket_directory_entry> entries) {
    using namespace bioseq::indexing;
    auto directory_bytes = encode_minimizer_bucket_directory(entries, original.directory.profile());
    REQUIRE(directory_bytes.has_value());
    auto manifest = original.manifest;
    manifest.directory_bytes = directory_bytes->size();
    manifest.directory_checksum = crc64_ecma182(*directory_bytes);
    const auto manifest_bytes = encode_minimizer_generation_manifest_record(manifest);
    REQUIRE(manifest_bytes.has_value());
    auto head = original.head;
    head.storage_contract_checksum =
        compute_minimizer_generation_storage_contract_checksum(manifest);
    head.manifest_checksum = crc64_ecma182(std::span<const std::byte>{*manifest_bytes});
    return {
        .head = head,
        .manifest = manifest,
        .directory_bytes = std::move(*directory_bytes),
    };
}
} // namespace compact_direct_test_support
} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
