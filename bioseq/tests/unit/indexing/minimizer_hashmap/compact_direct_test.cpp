/**
 * @file compact_direct_test.cpp
 * @brief Tests compact-direct planning and prepared fragment queries.
 *
 * The tests run inside `bioseq::simd::HWY_NAMESPACE` because they call the
 * selected minimizer collectors directly.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <hwy/highway.h>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <tests/support/common.hpp>
#include <type_traits>
#include <utility>
#include <vector>

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
import bioseq.common.hashing; //  shared SHA-256

import bioseq.tests.support.indexing_storage_fakes;

import bioseq.tests.unit.indexing.minimizer_hashmap_support;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
using namespace minimizer_hashmap_test_support;

namespace {

/// Own one sorted compact-direct run and its encoded positions.
struct fragment_compact_direct_run {
    bioseq::indexing::compact_hash32_sorted_run_header header{};
    std::vector<std::uint64_t> encoded_positions{};
};

/// Own all sorted runs exposed by a compact-direct source callback.
struct fragment_compact_direct_source {
    std::vector<fragment_compact_direct_run> runs{};
};

/// Replay sorted compact-direct runs through the erased source callback.
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

/// Build and validate a compact-direct image for one grouped-SoA identity.
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

/// Bind a compact-direct view to the shared fixture image owner.
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

/// Record selected-page advice without allocating in the callback.
struct selected_page_advice_recorder {
    static constexpr std::size_t capacity = 128u;
    std::array<bioseq::indexing::compact_direct_selected_page_range, capacity> records{};
    std::atomic<std::size_t> record_count{0u};
    std::atomic<bool> overflow{false};
    std::atomic<bool> drop_advice{false};

    /// Append one advised page interval when recording remains enabled.
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

    /// Return the erased adviser token bound to this recorder.
    [[nodiscard]] bioseq::indexing::compact_direct_selected_page_adviser
    token(const std::size_t page_bytes = 1u) noexcept {
        return bioseq::indexing::compact_direct_selected_page_adviser{
            .context = this,
            .page_bytes = page_bytes,
            .advise_fn = &selected_page_advice_recorder::advise,
        };
    }
};
/// Assemble mapped grouped-SoA payload bytes in directory order.
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

/// Own a sparsely committed virtual mapping used by width-boundary tests.
struct sparse_mapped_test_payload {
    std::shared_ptr<std::byte> owner{};
    std::size_t size = 0u;

    /// Return the writable mapped byte range.
    [[nodiscard]] std::span<std::byte> bytes() const noexcept {
        return {owner.get(), size};
    }
};

/// Reserve a zero-filled virtual address range without materializing its
/// untouched pages. O64/C64 boundary fixtures read only the small control
/// prefix and demonstrate that the multi-gigabyte position extent is
/// not visited after max-occ rejection.
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

/// Open a grouped-SoA fragment reader over captured mapped payload rows.
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

/// Produce the ordinary transient-index anchors used as a query oracle.
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

/// Require byte identity and canonical ordering for two anchor ranges.
void require_anchor_bytes_equal(std::span<const bioseq::chaining::anchor> actual,
                                std::span<const bioseq::chaining::anchor> expected) {
    REQUIRE(actual.size() == expected.size());
    REQUIRE(std::ranges::equal(std::as_bytes(actual), std::as_bytes(expected)));
    REQUIRE(std::ranges::is_sorted(actual, bioseq::chaining::canonical_anchor_less{}));
}

/// Require identical hot-loop counters for the direct path and its oracle.
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
/// Own rebuilt head, manifest, and directory metadata.
struct rebuilt_generation_metadata {
    bioseq::indexing::minimizer_generation_head head{};
    bioseq::indexing::minimizer_generation_manifest manifest{};
    std::vector<std::byte> directory_bytes;
};

/// Rebuild checksummed metadata for a replacement directory.
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
} // namespace

TEST_CASE("packed query occurrence layout is boundary exact",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t u32_max = (std::numeric_limits<std::uint32_t>::max)();
    constexpr std::uint64_t two_to_32 = std::uint64_t{1} << 32u;

    STATIC_REQUIRE(sizeof(packed_query_occurrence) == 8u);
    STATIC_REQUIRE(sizeof(wide_query_occurrence) == 12u);

    constexpr auto empty = plan_query_occurrence_packing(0u, 0u, 0u);
    STATIC_REQUIRE(empty.has_value());
    STATIC_REQUIRE(empty->packed());
    STATIC_REQUIRE(empty->query_bits == 0u);
    STATIC_REQUIRE(empty->ordinal_bits == 32u);
    STATIC_REQUIRE(empty->ordinal_capacity == two_to_32);
    STATIC_REQUIRE(empty->ordinal_mask == UINT32_MAX);

    constexpr auto single = plan_query_occurrence_packing(1u, u32_max, u32_max);
    STATIC_REQUIRE(single.has_value());
    STATIC_REQUIRE(single->packed());
    STATIC_REQUIRE(single->ordinal_bits == 32u);
    constexpr auto single_word = encode_packed_query_occurrence(*single, 0u, UINT32_MAX);
    STATIC_REQUIRE(single_word.has_value());
    STATIC_REQUIRE(*single_word == UINT32_MAX);
    STATIC_REQUIRE((decode_packed_query_occurrence_trusted(*single, *single_word) ==
                    query_occurrence_reference{
                        .batch_query_ordinal = 0u,
                        .query_ordinal = UINT32_MAX,
                    }));

    constexpr auto maximum_queries = plan_query_occurrence_packing(u32_max, 1u, 1u);
    STATIC_REQUIRE(maximum_queries.has_value());
    STATIC_REQUIRE(maximum_queries->packed());
    STATIC_REQUIRE(maximum_queries->query_bits == 32u);
    STATIC_REQUIRE(maximum_queries->ordinal_bits == 0u);
    STATIC_REQUIRE(maximum_queries->ordinal_capacity == 1u);
    constexpr auto maximum_query_word =
        encode_packed_query_occurrence(*maximum_queries, UINT32_MAX - 1u, 0u);
    STATIC_REQUIRE(maximum_query_word.has_value());
    STATIC_REQUIRE((decode_packed_query_occurrence_trusted(*maximum_queries, *maximum_query_word) ==
                    query_occurrence_reference{
                        .batch_query_ordinal = UINT32_MAX - 1u,
                        .query_ordinal = 0u,
                    }));

    constexpr auto maximum_occurrences = plan_query_occurrence_packing(1u, u32_max, u32_max);
    STATIC_REQUIRE(maximum_occurrences.has_value());
    STATIC_REQUIRE_FALSE(plan_query_occurrence_packing(1u, two_to_32, u32_max).has_value());
    STATIC_REQUIRE_FALSE(plan_query_occurrence_packing(two_to_32, 0u, 0u).has_value());
    STATIC_REQUIRE_FALSE(plan_query_occurrence_packing(1u, two_to_32, two_to_32).has_value());

    constexpr auto maximum_packed = plan_query_occurrence_packing(65'536u, 65'537u, 65'536u);
    STATIC_REQUIRE(maximum_packed.has_value());
    STATIC_REQUIRE(maximum_packed->packed());
    STATIC_REQUIRE(maximum_packed->ordinal_bits == 16u);
    constexpr auto forced_wide = plan_query_occurrence_packing(65'536u, 65'538u, 65'537u);
    STATIC_REQUIRE(forced_wide.has_value());
    STATIC_REQUIRE(forced_wide->route == query_occurrence_route::wide);

    constexpr auto malformed_shift = [single] {
        auto layout = *single;
        layout.ordinal_bits = 64u;
        return layout;
    }();
    STATIC_REQUIRE_FALSE(valid_query_occurrence_packing_layout(malformed_shift));
    STATIC_REQUIRE_FALSE(encode_packed_query_occurrence(malformed_shift, 0u, 0u).has_value());
    constexpr auto malformed_mask = [single] {
        auto layout = *single;
        layout.ordinal_mask = 0u;
        return layout;
    }();
    STATIC_REQUIRE_FALSE(valid_query_occurrence_packing_layout(malformed_mask));
    STATIC_REQUIRE_FALSE(encode_packed_query_occurrence(malformed_mask, 0u, 0u).has_value());
}

TEST_CASE("canonical anchor sort preserves the 63-bit reference boundary",
          "[component:bioseq][layer:unit][feature:seeding]") {
    constexpr std::uint64_t max_reference = (std::uint64_t{1} << 63u) - 1u;
    constexpr std::uint64_t low_query_position = 7u;
    constexpr std::uint64_t high_query_position = (std::uint64_t{1} << 32u) + low_query_position;
    context::context ctx;
    context::installed_context_scope installed{ctx};
    auto scope = ctx.push<context::arena::chain>();
    auto anchors = bioseq::chaining::make_arena_slab<bioseq::chaining::anchor>();
    anchors.push_back(bioseq::chaining::anchor{
        .ref_position = max_reference,
        .query_position = high_query_position,
        .is_reverse = 0u,
        .query_span = 15u,
        .flags = 0u,
        .segment_id = 0u,
    });
    anchors.push_back(bioseq::chaining::anchor{
        .ref_position = 0u,
        .query_position = 1u,
        .is_reverse = 1u,
        .query_span = 15u,
        .flags = 0u,
        .segment_id = 0u,
    });
    anchors.push_back(bioseq::chaining::anchor{
        .ref_position = max_reference,
        .query_position = low_query_position,
        .is_reverse = 0u,
        .query_span = 15u,
        .flags = 0u,
        .segment_id = 0u,
    });
    anchors.push_back(bioseq::chaining::anchor{
        .ref_position = max_reference - 1u,
        .query_position = 99u,
        .is_reverse = 0u,
        .query_span = 15u,
        .flags = 0u,
        .segment_id = 0u,
    });

    sort_seed_hits(anchors);
    REQUIRE(anchors.size() == 4u);
    REQUIRE(anchors.data()[0].ref_position == max_reference - 1u);
    REQUIRE(anchors.data()[1].ref_position == max_reference);
    REQUIRE(anchors.data()[1].query_position == low_query_position);
    REQUIRE(anchors.data()[2].ref_position == max_reference);
    REQUIRE(anchors.data()[2].query_position == high_query_position);
    REQUIRE(anchors.data()[1].query_sort_key() == anchors.data()[2].query_sort_key());
    REQUIRE(anchors.data()[3].is_reverse == 1u);
    REQUIRE(std::ranges::is_sorted(
        std::span<const bioseq::chaining::anchor>{anchors.data(), anchors.size()},
        bioseq::chaining::canonical_anchor_less{}));
}

TEST_CASE("bounded u32 anchor radix selects the authenticated width exactly",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using bioseq::chaining::anchor;
    const auto make_anchor = [](std::uint64_t ref_position, std::uint32_t is_reverse) noexcept {
        return anchor{
            .ref_position = ref_position,
            .query_position = ref_position ^ 0x55u,
            .is_reverse = is_reverse,
            .query_span = 15u,
            .flags = 0u,
            .segment_id = 0u,
        };
    };
    const auto require_route =
        [](std::span<anchor> anchors, std::uint64_t target_length, bool expected_bounded_u32) {
            const auto evidence =
                bioseq::chaining::sort_canonical_anchor_primary_bounded(anchors, target_length);
            REQUIRE(evidence.bounded_u32_primary_key == expected_bounded_u32);
            REQUIRE(evidence.primary_bytes == anchors.size_bytes());
            REQUIRE_FALSE(evidence.saturated);
            REQUIRE(std::ranges::is_sorted(anchors, bioseq::chaining::canonical_anchor_less{}));
        };

    SECTION("UINT32_MAX target length uses the four-byte key") {
        constexpr std::uint64_t target_length = (std::numeric_limits<std::uint32_t>::max)();
        std::array anchors{
            make_anchor(target_length - 1u, 0u),
            make_anchor(17u, 0u),
            make_anchor(target_length / 2u, 0u),
        };
        require_route(anchors, target_length, true);
    }

    SECTION("UINT32_MAX plus one retains the full-width key") {
        constexpr std::uint64_t target_length =
            static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()) + 1u;
        std::array anchors{
            make_anchor(target_length - 1u, 0u),
            make_anchor(19u, 0u),
            make_anchor(target_length / 2u, 0u),
        };
        require_route(anchors, target_length, false);
    }

    SECTION("a high authenticated reference retains the full-width key") {
        constexpr std::uint64_t target_length = (std::uint64_t{1u} << 40u) + 2u;
        std::array anchors{
            make_anchor(target_length - 1u, 0u),
            make_anchor(23u, 0u),
            make_anchor(std::uint64_t{1u} << 39u, 0u),
        };
        require_route(anchors, target_length, false);
    }

    SECTION("a reverse-only slice preserves canonical reference order") {
        constexpr std::uint64_t target_length = 10'000u;
        std::array anchors{
            make_anchor(9'999u, 1u),
            make_anchor(3u, 1u),
            make_anchor(5'001u, 1u),
        };
        require_route(anchors, target_length, true);
        REQUIRE(std::ranges::all_of(anchors,
                                    [](const anchor& anchor) { return anchor.is_reverse == 1u; }));
    }
}

TEST_CASE("fragment-native anchors are canonical and match ordinary lookup",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t segment_span = k_minimizer_preferred_segment_span_bp;
    // grouped-SoA directory segments use an int32 ordinal, so a mapped reader
    // fixture cannot encode the synthetic 2^63 boundary above. This position
    // is still well beyond 32 bits and leaves a large run of empty segments.
    constexpr std::uint64_t high_reference_position = (std::uint64_t{1} << 40u) + 19u;
    constexpr auto packed = [](std::uint64_t position, std::uint64_t strand) {
        return (position << 1u) | strand;
    };
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, packed(10u, 0u)},
        {7u, packed(10u, 1u)},
        {7u, packed(segment_span + 3u, 0u)},
        {7u, packed(high_reference_position, 1u)},
        {9u, packed(21u, 0u)},
    };
    constexpr std::uint64_t target_length = high_reference_position + 100u;
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink, test_grouped_soa_generation_options(target_length, 10u));
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest.position_segment_count > 2u);

    const std::vector<minimap_sketch_data> query{
        minimap_sketch_data{/*kmer=*/7u, /*pos=*/20u, /*strand=*/0u},
        // The duplicate produces byte-identical equal canonical keys.
        minimap_sketch_data{/*kmer=*/7u, /*pos=*/20u, /*strand=*/0u},
        minimap_sketch_data{/*kmer=*/7u, /*pos=*/33u, /*strand=*/1u},
        minimap_sketch_data{/*kmer=*/9u, /*pos=*/44u, /*strand=*/0u},
    };
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;

    SECTION("accepted cross-segment positions are byte-identical") {
        bioseq::parallelism::memory_admission_controller admission;
        auto reader = open_grouped_soa_fragment_query_reader(admission, *generation, sink.rows, 4u);
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        const std::array queries{
            fragment_seed_query{
                .minimizers = std::span<const minimap_sketch_data>{query},
                .query_length = query_length,
                .kmer_size = kmer_size,
            },
        };
        const auto expected =
            ordinary_seed_anchors(pairs, target_length, query, query_length, 4, kmer_size);
        auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 4, *scratch);
        REQUIRE(batch.query_count() == 1u);
        REQUIRE(batch.admitted_bytes() > 0u);
        REQUIRE(scratch->wave_outstanding());
        const auto actual =
            batch.query_view(0u).ordered_fragment_seed_anchors(query, query_length, 4, kmer_size);
        require_anchor_bytes_equal(actual, expected);
        REQUIRE(std::ranges::any_of(actual,
                                    [](const auto& anchor) { return anchor.is_reverse == 0u; }));
        REQUIRE(std::ranges::any_of(actual,
                                    [](const auto& anchor) { return anchor.is_reverse == 1u; }));
        REQUIRE(std::ranges::any_of(actual, [](const auto& anchor) {
            return anchor.ref_position == high_reference_position;
        }));
        bool observed_equal_key_duplicate = false;
        for (std::size_t ordinal = 1u; ordinal < actual.size(); ++ordinal) {
            if (std::ranges::equal(std::as_bytes(actual.subspan(ordinal - 1u, 1u)),
                                   std::as_bytes(actual.subspan(ordinal, 1u)))) {
                observed_equal_key_duplicate = true;
                break;
            }
        }
        REQUIRE(observed_equal_key_duplicate);
        const prepared_fragment_anchor_batch_stats& evidence = batch.stats();
        REQUIRE(evidence.waves == 1u);
        REQUIRE(evidence.queries == queries.size());
        REQUIRE(evidence.query_occurrences == query.size());
        REQUIRE(evidence.unique_query_plans == 2u);
        REQUIRE(evidence.packed_occurrence_waves == 1u);
        REQUIRE(evidence.wide_occurrence_waves == 0u);
        REQUIRE(evidence.occurrence_fill_ns > 0u);
        REQUIRE(evidence.occurrence_radix_ns > 0u);
        REQUIRE(evidence.control_count_pass_ns > 0u);
        REQUIRE(evidence.orientation_layout_ns > 0u);
        REQUIRE(evidence.direct_emission_ns > 0u);
        REQUIRE(evidence.local_sort_ns > 0u);
        REQUIRE(evidence.tie_normalization_ns > 0u);
        REQUIRE(evidence.control_count_bytes > 0u);
        REQUIRE(evidence.pass2_position_bytes > 0u);
        REQUIRE(evidence.pass2_position_bytes == evidence.pass3_position_bytes);
        REQUIRE(evidence.anchors_constructed == actual.size());
        REQUIRE(evidence.local_sort_bytes > 0u);
        REQUIRE(evidence.tie_normalization_bytes > 0u);
        REQUIRE(evidence.exact_final_bytes == actual.size_bytes());
        REQUIRE(batch.admitted_bytes() ==
                evidence.exact_final_bytes + queries.size() * sizeof(prepared_query_anchors));
        REQUIRE(evidence.peak_exact_final_bytes == evidence.exact_final_bytes);
        REQUIRE(evidence.global_sort_calls_invoked == 0u);
        REQUIRE(evidence.global_sort_calls_skipped == queries.size());
        REQUIRE(evidence.compositional_sortedness_receipts == queries.size());
        REQUIRE(evidence.forbidden_buffers_are_zero());
        REQUIRE(evidence.peak_occurrence_capacity_bytes > 0u);
        REQUIRE(evidence.peak_query_plan_capacity_bytes == 0u);
        REQUIRE(evidence.peak_query_radix_capacity_bytes > 0u);
        REQUIRE(evidence.peak_occurrence_count_capacity_bytes > 0u);
        REQUIRE(evidence.peak_block_layout_capacity_bytes > 0u);
        REQUIRE(evidence.peak_block_stats_capacity_bytes > 0u);
        REQUIRE_FALSE(evidence.saturated);
        const query_occurrence_capacity_snapshot occurrence_snapshot =
            scratch->occurrence_capacity_snapshot();
        REQUIRE(occurrence_snapshot.route == query_occurrence_route::packed);
        REQUIRE(occurrence_snapshot.packed_size == query.size());
        REQUIRE(occurrence_snapshot.wide_size == 0u);
        REQUIRE(occurrence_snapshot.query_plan_size == 0u);
        REQUIRE(occurrence_snapshot.query_plan_capacity == 0u);
        REQUIRE(occurrence_snapshot.query_plan_reserve_request == 0u);
        REQUIRE(occurrence_snapshot.bucket_direct_matcher_active);
        REQUIRE(occurrence_snapshot.query_membership_filter_capacity_bytes == 16u * 1024u * 1024u);
        REQUIRE(occurrence_snapshot.occurrence_radix_invoked);
        REQUIRE(occurrence_snapshot.aggregate_occurrence_capacity_bytes ==
                evidence.peak_occurrence_capacity_bytes);
        scratch->complete_wave_after_join();
        REQUIRE_FALSE(scratch->wave_outstanding());
    }

    SECTION("compact-direct P64 decodes postings once into canonical slices") {
        auto compact = make_fragment_compact_direct_test_index(
            generation->head, generation->manifest, pairs, 10u);
        REQUIRE(compact.view.header.posting_width_bytes == 8u);
        bioseq::parallelism::memory_admission_controller admission;
        selected_page_advice_recorder selected_page_recorder;
        auto reader = prepared_minimizer_fragment_reader::open_compact_direct(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(10u),
            4u,
            own_fragment_compact_direct_test_index(compact),
            selected_page_recorder.token());
        REQUIRE(reader.has_value());
        REQUIRE(selected_page_recorder.record_count.load() == 0u);
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        const std::array queries{
            fragment_seed_query{
                .minimizers = std::span<const minimap_sketch_data>{query},
                .query_length = query_length,
                .kmer_size = kmer_size,
            },
        };
        const auto expected =
            ordinary_seed_anchors(pairs, target_length, query, query_length, 4, kmer_size);
        auto batch = reader->prepare_fragment_anchor_batch(
            admission,
            queries,
            4,
            *scratch,
            /*baseline_query_plan_matcher_for_testing=*/false,
            /*verify_compositional_order_for_testing=*/true,
            minimizer_fragment_lookup_route::direct_compact_hash_lookup);
        const auto actual =
            batch.query_view(0u).ordered_fragment_seed_anchors(query, query_length, 4, kmer_size);
        require_anchor_bytes_equal(actual, expected);
        const auto& evidence = batch.stats();
        REQUIRE(evidence.compact_direct_selected_postings == pairs.size());
        REQUIRE(evidence.compact_direct_selection_shards == 2u);
        REQUIRE(evidence.compact_direct_shard_query_layouts == 2u);
        REQUIRE(evidence.compact_direct_layout_shards_completed == 2u);
        REQUIRE(evidence.compact_direct_emission_shards_completed == 2u);
        REQUIRE(evidence.compact_direct_posting_visit_passes == 1u);
        REQUIRE(evidence.compact_direct_layout_postings_visited == 0u);
        REQUIRE(evidence.compact_direct_emission_postings_visited == pairs.size());
        REQUIRE(evidence.pass2_position_bytes == 0u);
        REQUIRE(evidence.pass3_position_bytes ==
                pairs.size() * compact.view.header.posting_width_bytes);
        REQUIRE(evidence.anchors_constructed == actual.size());
        REQUIRE(evidence.forbidden_buffers_are_zero());
        REQUIRE_FALSE(selected_page_recorder.overflow.load());
        REQUIRE(selected_page_recorder.record_count.load() == 2u);
        const std::uintptr_t postings_begin =
            reinterpret_cast<std::uintptr_t>(compact.view.image.data()) +
            compact.view.header.postings_offset;
        const std::uintptr_t postings_end = postings_begin + compact.view.header.postings_bytes;
        std::uint64_t advised_bytes = 0u;
        for (std::size_t ordinal = 0u; ordinal < 2u; ++ordinal) {
            const auto range = selected_page_recorder.records[ordinal];
            REQUIRE(range.begin >= postings_begin);
            REQUIRE(range.end <= postings_end);
            REQUIRE(range.begin < range.end);
            advised_bytes += range.end - range.begin;
        }
        REQUIRE(advised_bytes == pairs.size() * compact.view.header.posting_width_bytes);
        scratch->complete_wave_after_join();
    }

    SECTION("late cross-segment max-occ rejection drops only the heavy hash") {
        bioseq::parallelism::memory_admission_controller admission;
        auto reader = open_grouped_soa_fragment_query_reader(admission, *generation, sink.rows, 3u);
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        const std::array queries{
            fragment_seed_query{
                .minimizers = std::span<const minimap_sketch_data>{query},
                .query_length = query_length,
                .kmer_size = kmer_size,
            },
        };
        const auto expected =
            ordinary_seed_anchors(pairs, target_length, query, query_length, 3, kmer_size);
        auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 3, *scratch);
        const auto actual =
            batch.query_view(0u).ordered_fragment_seed_anchors(query, query_length, 3, kmer_size);
        require_anchor_bytes_equal(actual, expected);
        REQUIRE(actual.size() == 1u);
        REQUIRE(actual.front().ref_position == 21u);
        const prepared_fragment_anchor_batch_stats& evidence = batch.stats();
        REQUIRE(evidence.pass2_position_bytes > 0u);
        REQUIRE(evidence.pass2_position_bytes == evidence.pass3_position_bytes);
        REQUIRE(evidence.anchors_constructed == actual.size());
        REQUIRE(evidence.global_sort_calls_invoked == 0u);
        REQUIRE(evidence.global_sort_calls_skipped == 1u);
        REQUIRE(evidence.forbidden_buffers_are_zero());
        scratch->complete_wave_after_join();
    }

    SECTION("all-heavy rejection attributes validation without layout work") {
        bioseq::parallelism::memory_admission_controller admission;
        auto reader = open_grouped_soa_fragment_query_reader(admission, *generation, sink.rows, 3u);
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        const auto heavy_query = std::span<const minimap_sketch_data>{query}.first(3u);
        const std::array queries{
            fragment_seed_query{
                .minimizers = heavy_query,
                .query_length = query_length,
                .kmer_size = kmer_size,
            },
        };
        auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 3, *scratch);
        const auto actual = batch.query_view(0u).ordered_fragment_seed_anchors(
            heavy_query, query_length, 3, kmer_size);
        REQUIRE(actual.empty());

        const prepared_fragment_anchor_batch_stats& evidence = batch.stats();
        REQUIRE(evidence.waves == 1u);
        REQUIRE(evidence.queries == queries.size());
        REQUIRE(evidence.control_count_bytes > 0u);
        REQUIRE(evidence.rejected_control_validation_ns > 0u);
        REQUIRE(evidence.rejected_control_validation_bytes > evidence.control_count_bytes);
        REQUIRE(evidence.orientation_layout_ns == 0u);
        REQUIRE(evidence.direct_emission_ns == 0u);
        REQUIRE(evidence.local_sort_ns == 0u);
        REQUIRE(evidence.tie_normalization_ns == 0u);
        REQUIRE(evidence.pass2_position_bytes == 0u);
        REQUIRE(evidence.pass3_position_bytes == 0u);
        REQUIRE(evidence.anchors_constructed == 0u);
        REQUIRE(evidence.exact_final_bytes == 0u);
        REQUIRE(evidence.peak_exact_final_bytes == 0u);
        REQUIRE(batch.admitted_bytes() == queries.size() * sizeof(prepared_query_anchors));
        REQUIRE(evidence.global_sort_calls_invoked == 0u);
        REQUIRE(evidence.global_sort_calls_skipped == queries.size());
        REQUIRE(evidence.forbidden_buffers_are_zero());
        REQUIRE_FALSE(evidence.saturated);
        scratch->complete_wave_after_join();
    }
}

TEST_CASE("selective pass byte receipts are exact for accepted rejected and mixed work",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t control_count = 64u;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
    pairs.reserve(control_count + 1u);
    for (std::uint64_t hash = 1u; hash <= control_count; ++hash) {
        pairs.emplace_back(hash, (hash + 10u) << 1u);
        if (hash == 7u) {
            pairs.emplace_back(hash, (hash + 100u) << 1u);
        }
    }
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    auto options = test_grouped_soa_generation_options(256u, 65'534u);
    options.position_segment_span_bp = std::uint64_t{1} << 15u;
    const auto generation = encode_minimizer_generation(producer, sink, options);
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest.hash_width == minimizer_scalar_width::u32);
    REQUIRE(generation->manifest.count_width == minimizer_scalar_width::u16);
    REQUIRE(generation->manifest.position_width == minimizer_scalar_width::u16);
    REQUIRE(generation->directory.entries().size() == 1u);
    REQUIRE(generation->directory.entries().front().shape_offset_width ==
            minimizer_scalar_width::u16);

    constexpr std::uint64_t hash_scan_bytes = control_count * 4u;
    constexpr std::uint64_t control_scan_bytes = control_count * (4u + 2u);
    constexpr std::uint64_t one_matched_count_bytes = 2u;
    constexpr std::uint64_t one_prefix_pair_bytes = 2u * 2u;
    constexpr std::uint64_t one_position_bytes = 2u;
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;
    const std::array accepted_minimizers{
        minimap_sketch_data{control_count, 20u, 0u},
    };
    const std::array rejected_minimizers{
        minimap_sketch_data{7u, 20u, 0u},
    };
    const std::array mixed_minimizers{
        minimap_sketch_data{7u, 20u, 0u},
        minimap_sketch_data{9u, 21u, 1u},
    };

    const auto run = [&generation, &sink](std::span<const minimap_sketch_data> minimizers) {
        bioseq::parallelism::memory_admission_controller admission;
        auto reader = open_grouped_soa_fragment_query_reader(admission, *generation, sink.rows, 1u);
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        const std::array queries{
            fragment_seed_query{
                .minimizers = minimizers,
                .query_length = query_length,
                .kmer_size = kmer_size,
            },
        };
        auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 1, *scratch);
        const std::size_t anchor_count =
            batch.query_view(0u)
                .ordered_fragment_seed_anchors(minimizers, query_length, 1, kmer_size)
                .size();
        const prepared_fragment_anchor_batch_stats evidence = batch.stats();
        scratch->complete_wave_after_join();
        return std::pair{evidence, anchor_count};
    };

    SECTION("accepted") {
        const auto [evidence, anchor_count] = run(accepted_minimizers);
        REQUIRE(anchor_count == 1u);
        REQUIRE(evidence.control_count_bytes == control_scan_bytes);
        REQUIRE(evidence.rejected_control_validation_bytes == 0u);
        REQUIRE(evidence.pass2_control_bytes == hash_scan_bytes + one_matched_count_bytes);
        REQUIRE(evidence.pass2_accepted_prefix_bytes == one_prefix_pair_bytes);
        REQUIRE(evidence.pass2_position_bytes == one_position_bytes);
        REQUIRE(evidence.pass3_control_bytes == hash_scan_bytes + one_matched_count_bytes);
        REQUIRE(evidence.pass3_accepted_prefix_bytes == one_prefix_pair_bytes);
        REQUIRE(evidence.pass3_position_bytes == one_position_bytes);
    }

    SECTION("rejected") {
        const auto [evidence, anchor_count] = run(rejected_minimizers);
        REQUIRE(anchor_count == 0u);
        REQUIRE(evidence.control_count_bytes == control_scan_bytes);
        REQUIRE(evidence.rejected_control_validation_bytes ==
                hash_scan_bytes + one_matched_count_bytes + one_prefix_pair_bytes);
        REQUIRE(evidence.pass2_control_bytes == 0u);
        REQUIRE(evidence.pass2_accepted_prefix_bytes == 0u);
        REQUIRE(evidence.pass2_position_bytes == 0u);
        REQUIRE(evidence.pass3_control_bytes == 0u);
        REQUIRE(evidence.pass3_accepted_prefix_bytes == 0u);
        REQUIRE(evidence.pass3_position_bytes == 0u);
    }

    SECTION("mixed") {
        const auto [evidence, anchor_count] = run(mixed_minimizers);
        REQUIRE(anchor_count == 1u);
        REQUIRE(evidence.control_count_bytes == control_scan_bytes);
        REQUIRE(evidence.rejected_control_validation_bytes == one_prefix_pair_bytes);
        REQUIRE(evidence.pass2_control_bytes == hash_scan_bytes + 2u * one_matched_count_bytes);
        REQUIRE(evidence.pass2_accepted_prefix_bytes == one_prefix_pair_bytes);
        REQUIRE(evidence.pass2_position_bytes == one_position_bytes);
        REQUIRE(evidence.pass3_control_bytes == hash_scan_bytes + one_matched_count_bytes);
        REQUIRE(evidence.pass3_accepted_prefix_bytes == one_prefix_pair_bytes);
        REQUIRE(evidence.pass3_position_bytes == one_position_bytes);
    }
}

TEST_CASE("checked dynamic reader covers P and C width boundaries",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;
    const std::array query_minimizers{
        minimap_sketch_data{7u, 20u, 0u},
    };
    const std::array queries{
        fragment_seed_query{
            .minimizers = std::span<const minimap_sketch_data>{query_minimizers},
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };

    const auto verify = [&queries,
                         &query_minimizers](std::uint64_t segment_span,
                                            std::uint64_t max_occ_cap,
                                            minimizer_scalar_width expected_position_width,
                                            minimizer_scalar_width expected_count_width) {
        const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
            {7u, 10u << 1u},
        };
        vector_replay_minimizer_producer producer{pairs};
        collecting_minimizer_row_sink sink;
        auto options = test_grouped_soa_generation_options(segment_span, max_occ_cap);
        options.position_segment_span_bp = segment_span;
        const auto generation = encode_minimizer_generation(producer, sink, options);
        REQUIRE(generation.has_value());
        REQUIRE(generation->manifest.hash_width == minimizer_scalar_width::u32);
        REQUIRE(generation->manifest.position_width == expected_position_width);
        REQUIRE(generation->manifest.count_width == expected_count_width);
        REQUIRE(generation->directory.entries().size() == 1u);
        REQUIRE(generation->directory.entries().front().shape_offset_width ==
                minimizer_scalar_width::u16);

        bioseq::parallelism::memory_admission_controller admission;
        auto reader = open_grouped_soa_fragment_query_reader(admission, *generation, sink.rows, 1u);
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 1, *scratch);
        const auto anchors = batch.query_view(0u).ordered_fragment_seed_anchors(
            query_minimizers, query_length, 1, kmer_size);
        REQUIRE(anchors.size() == 1u);
        const auto& evidence = batch.stats();
        const std::uint64_t control_scan_bytes =
            4u + static_cast<std::uint8_t>(expected_count_width);
        REQUIRE(evidence.control_count_bytes == control_scan_bytes);
        REQUIRE(evidence.pass2_control_bytes == control_scan_bytes);
        REQUIRE(evidence.pass3_control_bytes == control_scan_bytes);
        REQUIRE(evidence.pass2_accepted_prefix_bytes == 4u);
        REQUIRE(evidence.pass3_accepted_prefix_bytes == 4u);
        REQUIRE(evidence.pass2_position_bytes ==
                static_cast<std::uint8_t>(expected_position_width));
        REQUIRE(evidence.pass3_position_bytes ==
                static_cast<std::uint8_t>(expected_position_width));
        scratch->complete_wave_after_join();
    };

    SECTION("P32 lower boundary") {
        verify((std::uint64_t{1} << 15u) + 1u,
               65'534u,
               minimizer_scalar_width::u32,
               minimizer_scalar_width::u16);
    }
    SECTION("P64 lower boundary") {
        verify((std::uint64_t{1} << 31u) + 1u,
               65'534u,
               minimizer_scalar_width::u64,
               minimizer_scalar_width::u16);
    }
    SECTION("C32 lower boundary with P16") {
        verify(std::uint64_t{1} << 15u,
               65'535u,
               minimizer_scalar_width::u16,
               minimizer_scalar_width::u32);
    }
}

TEST_CASE("O32 leaf is exact for fixed and dynamic count profiles",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t position_count = 65'536u;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
    pairs.reserve(position_count);
    for (std::uint64_t encoded = 0u; encoded < position_count; ++encoded) {
        pairs.emplace_back(7u, encoded);
    }
    const std::array query_minimizers{
        minimap_sketch_data{7u, 20u, 0u},
    };
    const std::array queries{
        fragment_seed_query{
            .minimizers = std::span<const minimap_sketch_data>{query_minimizers},
            .query_length = 100u,
            .kmer_size = 15,
        },
    };

    const auto verify = [&pairs, &queries, &query_minimizers](std::uint64_t max_occ_cap,
                                                              minimizer_scalar_width count_width) {
        vector_replay_minimizer_producer producer{pairs};
        collecting_minimizer_row_sink sink;
        auto options = test_grouped_soa_generation_options(std::uint64_t{1} << 15u, max_occ_cap);
        options.position_segment_span_bp = std::uint64_t{1} << 15u;
        const auto generation = encode_minimizer_generation(producer, sink, options);
        REQUIRE(generation.has_value());
        REQUIRE(generation->manifest.hash_width == minimizer_scalar_width::u32);
        REQUIRE(generation->manifest.count_width == count_width);
        REQUIRE(generation->manifest.position_width == minimizer_scalar_width::u16);
        REQUIRE(generation->directory.entries().size() == 1u);
        REQUIRE(generation->directory.entries().front().position_element_count == position_count);
        REQUIRE(generation->directory.entries().front().shape_offset_width ==
                minimizer_scalar_width::u32);

        bioseq::parallelism::memory_admission_controller admission;
        auto reader = open_grouped_soa_fragment_query_reader(admission, *generation, sink.rows, 1u);
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 1, *scratch);
        REQUIRE(batch.query_view(0u)
                    .ordered_fragment_seed_anchors(query_minimizers, 100u, 1, 15)
                    .empty());
        const auto& evidence = batch.stats();
        const std::uint64_t control_scan_bytes = 4u + static_cast<std::uint8_t>(count_width);
        REQUIRE(evidence.control_count_bytes == control_scan_bytes);
        REQUIRE(evidence.rejected_control_validation_bytes == control_scan_bytes + 2u * 4u);
        REQUIRE(evidence.pass2_control_bytes == 0u);
        REQUIRE(evidence.pass2_position_bytes == 0u);
        REQUIRE(evidence.pass3_control_bytes == 0u);
        REQUIRE(evidence.pass3_position_bytes == 0u);
        scratch->complete_wave_after_join();
    };

    SECTION("fixed H32 C16 P16 O32 leaf") {
        verify(65'534u, minimizer_scalar_width::u16);
    }
    SECTION("checked dynamic C32 O32 boundary") {
        verify(65'535u, minimizer_scalar_width::u32);
    }
}

TEST_CASE("O64 fast leaf and C64 dynamic rejection never walk positions",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    STATIC_REQUIRE(sizeof(std::size_t) >= sizeof(std::uint64_t));
    constexpr std::uint64_t position_count =
        static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()) + 1u;
    constexpr std::uint64_t segment_span = std::uint64_t{1} << 15u;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> base_pairs{
        {7u, 0u},
    };
    const std::array query_minimizers{
        minimap_sketch_data{7u, 20u, 0u},
    };
    const std::array queries{
        fragment_seed_query{
            .minimizers = std::span<const minimap_sketch_data>{query_minimizers},
            .query_length = 100u,
            .kmer_size = 15,
        },
    };

    const auto verify = [&base_pairs, &queries, &query_minimizers](
                            std::uint64_t max_occ_cap,
                            minimizer_scalar_width expected_count_width,
                            std::uint64_t encoded_local_count) {
        vector_replay_minimizer_producer producer{base_pairs};
        collecting_minimizer_row_sink sink;
        auto options = test_grouped_soa_generation_options(100u, max_occ_cap);
        options.position_segment_span_bp = segment_span;
        const auto encoded = encode_minimizer_generation(producer, sink, options);
        REQUIRE(encoded.has_value());
        REQUIRE(encoded->manifest.hash_width == minimizer_scalar_width::u32);
        REQUIRE(encoded->manifest.count_width == expected_count_width);
        REQUIRE(encoded->manifest.position_width == minimizer_scalar_width::u16);
        REQUIRE(encoded->directory.entries().size() == 1u);

        auto synthetic = *encoded;
        auto entry = synthetic.directory.entries().front();
        entry.position_element_count = position_count;
        entry.shape_offset_width = minimizer_scalar_width::u64;
        const auto layout = plan_grouped_soa_control_plane(entry.control_element_count,
                                                           synthetic.manifest.hash_width,
                                                           entry.shape_offset_width,
                                                           synthetic.manifest.count_width);
        REQUIRE(layout.has_value());
        entry.control_encoded_bytes = layout->encoded_bytes;
        entry.position_encoded_bytes = position_count * 2u;
        const std::uint64_t extent_bytes =
            minimizer_extent_payload_bytes(minimizer_generation_layout::grouped_soa_segment_local);
        entry.control_chunk_count = static_cast<std::uint32_t>(
            entry.control_encoded_bytes / extent_bytes +
            (entry.control_encoded_bytes % extent_bytes != 0u ? 1u : 0u));
        entry.position_chunk_count = static_cast<std::uint32_t>(
            entry.position_encoded_bytes / extent_bytes +
            (entry.position_encoded_bytes % extent_bytes != 0u ? 1u : 0u));
        const std::array entries{entry};
        synthetic.manifest.control_count = 1u;
        synthetic.manifest.position_count = position_count;
        synthetic.manifest.payload_bytes =
            entry.control_encoded_bytes + entry.position_encoded_bytes;
        const auto metadata = rebuild_generation_metadata(synthetic, entries);

        auto payload = make_sparse_mapped_test_payload(metadata.manifest.payload_bytes);
        REQUIRE(payload.has_value());
        auto mutable_payload = payload->bytes();
        test_write_le<std::uint32_t>(mutable_payload, 0u, 7u);
        test_write_le<std::uint64_t>(
            mutable_payload, static_cast<std::size_t>(layout->prefix_offset), 0u);
        test_write_le<std::uint64_t>(
            mutable_payload, static_cast<std::size_t>(layout->prefix_offset + 8u), position_count);
        if (expected_count_width == minimizer_scalar_width::u16) {
            test_write_le<std::uint16_t>(mutable_payload,
                                         static_cast<std::size_t>(layout->count_offset),
                                         static_cast<std::uint16_t>(encoded_local_count));
        } else {
            REQUIRE(expected_count_width == minimizer_scalar_width::u64);
            test_write_le<std::uint64_t>(mutable_payload,
                                         static_cast<std::size_t>(layout->count_offset),
                                         encoded_local_count);
        }

        bioseq::parallelism::memory_admission_controller admission;
        std::shared_ptr<const void> payload_owner = payload->owner;
        auto reader = prepared_minimizer_fragment_reader::open(
            admission,
            metadata.head,
            metadata.manifest,
            test_grouped_soa_expected_storage_contract(max_occ_cap),
            metadata.directory_bytes,
            1u,
            std::move(payload_owner),
            std::span<const std::byte>{mutable_payload.data(), mutable_payload.size()});
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 1, *scratch);
        REQUIRE(batch.query_view(0u)
                    .ordered_fragment_seed_anchors(query_minimizers, 100u, 1, 15)
                    .empty());
        const auto& evidence = batch.stats();
        const std::uint64_t control_scan_bytes =
            4u + static_cast<std::uint8_t>(expected_count_width);
        REQUIRE(evidence.control_count_bytes == control_scan_bytes);
        REQUIRE(evidence.rejected_control_validation_bytes == control_scan_bytes + 2u * 8u);
        REQUIRE(evidence.pass2_position_bytes == 0u);
        REQUIRE(evidence.pass3_position_bytes == 0u);
        scratch->complete_wave_after_join();
    };

    SECTION("fixed H32 C16 P16 O64 leaf") {
        verify(65'534u, minimizer_scalar_width::u16, 65'535u);
    }
    SECTION("checked dynamic C64 count above UINT32 rejects before narrowing") {
        verify((std::numeric_limits<std::uint32_t>::max)(),
               minimizer_scalar_width::u64,
               position_count);
    }
}

TEST_CASE("H64 reader open fails before admission or payload access",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {1u, 10u << 1u},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    auto options = test_grouped_soa_generation_options(100u, 65'534u);
    options.position_segment_span_bp = std::uint64_t{1} << 15u;
    options.kmer_size = 17u;
    const auto generation = encode_minimizer_generation(producer, sink, options);
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest.hash_width == minimizer_scalar_width::u64);
    REQUIRE(generation->manifest.max_hash > (std::numeric_limits<std::uint32_t>::max)());

    auto payload = make_grouped_soa_mapped_payload(*generation, sink.rows);
    auto expected = test_grouped_soa_expected_storage_contract(65'534u);
    expected.kmer_size = 17u;
    bioseq::parallelism::memory_admission_controller admission;
    REQUIRE(admission.reserved_bytes() == 0u);
    std::shared_ptr<const void> payload_owner = payload;
    const auto opened = prepared_minimizer_fragment_reader::open(
        admission,
        generation->head,
        generation->manifest,
        expected,
        generation->directory.encoded_bytes(),
        1u,
        std::move(payload_owner),
        std::span<const std::byte>{payload->data(), payload->size()});
    require_wire_error(opened, minimizer_generation_encoding_error::storage_contract_mismatch);
    REQUIRE(admission.reserved_bytes() == 0u);
}

TEST_CASE("fixed and dynamic probes reject selected payload corruption",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    const std::array query_minimizers{
        minimap_sketch_data{7u, 20u, 0u},
    };
    const std::array queries{
        fragment_seed_query{
            .minimizers = std::span<const minimap_sketch_data>{query_minimizers},
            .query_length = 100u,
            .kmer_size = 15,
        },
    };

    SECTION("fixed O16 prefix escapes the planned position extent") {
        const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
            {7u, 10u << 1u},
            {7u, 20u << 1u},
        };
        vector_replay_minimizer_producer producer{pairs};
        collecting_minimizer_row_sink sink;
        auto options = test_grouped_soa_generation_options(100u, 65'534u);
        options.position_segment_span_bp = std::uint64_t{1} << 15u;
        const auto generation = encode_minimizer_generation(producer, sink, options);
        REQUIRE(generation.has_value());
        const auto& entry = generation->directory.entries().front();
        REQUIRE(entry.shape_offset_width == minimizer_scalar_width::u16);
        auto payload = make_grouped_soa_mapped_payload(*generation, sink.rows);
        const auto layout = plan_grouped_soa_control_plane(entry.control_element_count,
                                                           generation->manifest.hash_width,
                                                           entry.shape_offset_width,
                                                           generation->manifest.count_width);
        REQUIRE(layout.has_value());
        test_write_le<std::uint16_t>(std::span<std::byte>{payload->data(), payload->size()},
                                     static_cast<std::size_t>(layout->prefix_offset + 2u),
                                     3u);

        bioseq::parallelism::memory_admission_controller admission;
        std::shared_ptr<const void> payload_owner = payload;
        auto reader = prepared_minimizer_fragment_reader::open(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(65'534u),
            generation->directory.encoded_bytes(),
            2u,
            std::move(payload_owner),
            std::span<const std::byte>{payload->data(), payload->size()});
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        REQUIRE_THROWS_AS(reader->prepare_fragment_anchor_batch(admission, queries, 2, *scratch),
                          persisted_minimizer_lookup_error);
        REQUIRE_FALSE(scratch->wave_outstanding());
    }

    SECTION("fixed Pass1 authenticates unmatched H32 and C16 controls") {
        const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
            {7u, 10u << 1u},
            {9u, 20u << 1u},
        };
        vector_replay_minimizer_producer producer{pairs};
        collecting_minimizer_row_sink sink;
        auto options = test_grouped_soa_generation_options(100u, 65'534u);
        options.position_segment_span_bp = std::uint64_t{1} << 15u;
        const auto generation = encode_minimizer_generation(producer, sink, options);
        REQUIRE(generation.has_value());
        const auto& entry = generation->directory.entries().front();
        REQUIRE(entry.control_element_count == 2u);
        REQUIRE(entry.shape_offset_width == minimizer_scalar_width::u16);
        const auto layout = plan_grouped_soa_control_plane(entry.control_element_count,
                                                           generation->manifest.hash_width,
                                                           entry.shape_offset_width,
                                                           generation->manifest.count_width);
        REQUIRE(layout.has_value());
        auto payload = make_grouped_soa_mapped_payload(*generation, sink.rows);
        auto mutable_payload = std::span<std::byte>{payload->data(), payload->size()};

        SECTION("unmatched count is zero") {
            test_write_le<std::uint16_t>(mutable_payload,
                                         static_cast<std::size_t>(layout->count_offset) +
                                             sizeof(std::uint16_t),
                                         0u);
        }
        SECTION("hash order is not strict") {
            test_write_le<std::uint32_t>(mutable_payload,
                                         static_cast<std::size_t>(layout->hash_offset) +
                                             sizeof(std::uint32_t),
                                         7u);
        }

        bioseq::parallelism::memory_admission_controller admission;
        std::shared_ptr<const void> payload_owner = payload;
        auto reader = prepared_minimizer_fragment_reader::open(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(65'534u),
            generation->directory.encoded_bytes(),
            2u,
            std::move(payload_owner),
            std::span<const std::byte>{mutable_payload.data(), mutable_payload.size()});
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        REQUIRE_THROWS_AS(reader->prepare_fragment_anchor_batch(admission, queries, 2, *scratch),
                          persisted_minimizer_lookup_error);
        REQUIRE_FALSE(scratch->wave_outstanding());
    }

    SECTION("dynamic P64 position escapes the authenticated segment core") {
        constexpr std::uint64_t segment_span = (std::uint64_t{1} << 31u) + 1u;
        const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
            {7u, 10u << 1u},
        };
        vector_replay_minimizer_producer producer{pairs};
        collecting_minimizer_row_sink sink;
        auto options = test_grouped_soa_generation_options(segment_span, 65'534u);
        options.position_segment_span_bp = segment_span;
        const auto generation = encode_minimizer_generation(producer, sink, options);
        REQUIRE(generation.has_value());
        REQUIRE(generation->manifest.position_width == minimizer_scalar_width::u64);
        const auto& entry = generation->directory.entries().front();
        auto payload = make_grouped_soa_mapped_payload(*generation, sink.rows);
        test_write_le<std::uint64_t>(std::span<std::byte>{payload->data(), payload->size()},
                                     static_cast<std::size_t>(entry.control_encoded_bytes),
                                     segment_span << 1u);

        bioseq::parallelism::memory_admission_controller admission;
        std::shared_ptr<const void> payload_owner = payload;
        auto reader = prepared_minimizer_fragment_reader::open(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(65'534u),
            generation->directory.encoded_bytes(),
            1u,
            std::move(payload_owner),
            std::span<const std::byte>{payload->data(), payload->size()});
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        REQUIRE_THROWS_AS(reader->prepare_fragment_anchor_batch(admission, queries, 1, *scratch),
                          persisted_minimizer_lookup_error);
        REQUIRE_FALSE(scratch->wave_outstanding());
    }
}

TEST_CASE("fragment blocks preserve segment runs beyond ordinal 128",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t segment_span = k_minimizer_preferred_segment_span_bp;
    constexpr auto packed = [](std::uint64_t position, std::uint64_t strand) {
        return (position << 1u) | strand;
    };
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
    pairs.reserve(132u);
    for (std::uint64_t hash = 1u; hash <= 130u; ++hash) {
        pairs.emplace_back(hash, packed(hash + 1u, 0u));
    }
    // Hash 1 straddles the oversized segment-0 block and the block after the
    // empty segment-1 gap. max_occ therefore has to be reduced globally.
    pairs.emplace_back(1u, packed(2u * segment_span + 4u, 0u));
    pairs.emplace_back(500u, packed(2u * segment_span + 5u, 1u));

    // The planner's fixed-width limit shrinks physical segment geometry; it
    // does not split one segment into directory entries. Build the exact valid
    // wire image here so the reader really sees an equal-segment run crossing
    // its 128-entry block target.
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink, test_grouped_soa_generation_options(2u * segment_span + 100u, 10u));
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest.position_segment_span_bp == segment_span);
    const auto profile = generation->directory.profile();
    REQUIRE(profile.hash_width == minimizer_scalar_width::u32);
    REQUIRE(profile.position_width == minimizer_scalar_width::u32);
    REQUIRE(profile.count_width == minimizer_scalar_width::u16);
    constexpr minimizer_scalar_width offset_width = minimizer_scalar_width::u16;
    const auto control_layout =
        plan_grouped_soa_control_plane(1u, profile.hash_width, offset_width, profile.count_width);
    REQUIRE(control_layout.has_value());

    std::vector<grouped_soa_bucket_directory_entry> manual_entries;
    std::vector<std::byte> control_image;
    std::vector<std::byte> position_image;
    std::vector<std::uint64_t> control_row_checksums;
    std::vector<std::uint64_t> position_row_checksums;
    manual_entries.reserve(pairs.size());
    control_image.reserve(pairs.size() * static_cast<std::size_t>(control_layout->encoded_bytes));
    position_image.reserve(pairs.size() * sizeof(std::uint32_t));
    control_row_checksums.reserve(pairs.size());
    position_row_checksums.reserve(pairs.size());

    const auto append_manual_entry = [&generation,
                                      &control_layout,
                                      &manual_entries,
                                      &control_image,
                                      &position_image,
                                      &control_row_checksums,
                                      &position_row_checksums](
                                         std::int32_t position_segment,
                                         std::uint64_t hash,
                                         std::uint64_t global_packed_position) {
        const auto core = derive_minimizer_position_segment_bounds(
            generation->manifest, static_cast<std::uint32_t>(position_segment));
        REQUIRE(core.has_value());
        const std::uint64_t global_position = global_packed_position >> 1u;
        REQUIRE(global_position >= core->begin_bp);
        REQUIRE(global_position < core->end_bp);
        const std::uint64_t local_packed_position =
            ((global_position - core->begin_bp) << 1u) | (global_packed_position & 1u);
        REQUIRE(local_packed_position <= (std::numeric_limits<std::uint32_t>::max)());

        std::vector<std::byte> controls(static_cast<std::size_t>(control_layout->encoded_bytes));
        test_write_le<std::uint32_t>(controls,
                                     static_cast<std::size_t>(control_layout->hash_offset),
                                     static_cast<std::uint32_t>(hash));
        test_write_le<std::uint16_t>(
            controls, static_cast<std::size_t>(control_layout->prefix_offset), 0u);
        test_write_le<std::uint16_t>(controls,
                                     static_cast<std::size_t>(control_layout->prefix_offset) +
                                         sizeof(std::uint16_t),
                                     1u);
        test_write_le<std::uint16_t>(
            controls, static_cast<std::size_t>(control_layout->count_offset), 1u);
        std::vector<std::byte> positions(sizeof(std::uint32_t));
        test_write_le<std::uint32_t>(
            positions, 0u, static_cast<std::uint32_t>(local_packed_position));

        const auto partition = static_cast<std::uint32_t>(manual_entries.size());
        grouped_soa_bucket_directory_entry entry{
            .partition_id = partition,
            .position_segment = position_segment,
            .lo_hash = hash,
            .hi_hash = hash,
            .sub_key = static_cast<index_sub_key_t>(partition),
            .control_first_chunk = 0u,
            .control_chunk_count = 1u,
            .position_first_chunk = 0u,
            .position_chunk_count = 1u,
            .control_element_count = 1u,
            .position_element_count = 1u,
            .control_encoded_bytes = controls.size(),
            .position_encoded_bytes = positions.size(),
            .control_checksum = crc64_ecma182(controls),
            .position_checksum = crc64_ecma182(positions),
            .shape_offset_width = offset_width,
        };
        const auto position_sub_key = derive_minimizer_position_extent_sub_key(entry.sub_key);
        REQUIRE(position_sub_key.has_value());
        control_row_checksums.push_back(
            crc64_index_chunk_row(generation->manifest.generation_id,
                                  position_segment,
                                  entry.sub_key,
                                  0u,
                                  static_cast<std::uint32_t>(controls.size()),
                                  controls));
        position_row_checksums.push_back(
            crc64_index_chunk_row(generation->manifest.generation_id,
                                  position_segment,
                                  *position_sub_key,
                                  0u,
                                  static_cast<std::uint32_t>(positions.size()),
                                  positions));
        control_image.insert(control_image.end(), controls.begin(), controls.end());
        position_image.insert(position_image.end(), positions.begin(), positions.end());
        manual_entries.push_back(entry);
    };
    for (std::uint64_t hash = 1u; hash <= 130u; ++hash) {
        append_manual_entry(0, hash, packed(hash + 1u, 0u));
    }
    append_manual_entry(2, 1u, packed(2u * segment_span + 4u, 0u));
    append_manual_entry(2, 500u, packed(2u * segment_span + 5u, 1u));

    crc64_ecma182_accumulator payload_checksum;
    for (const std::uint64_t checksum : control_row_checksums) {
        payload_checksum.update_little_endian(checksum);
    }
    for (const std::uint64_t checksum : position_row_checksums) {
        payload_checksum.update_little_endian(checksum);
    }
    auto adjusted_generation = *generation;
    adjusted_generation.manifest.control_count = manual_entries.size();
    adjusted_generation.manifest.position_count = manual_entries.size();
    adjusted_generation.manifest.payload_bytes = control_image.size() + position_image.size();
    adjusted_generation.manifest.payload_checksum = payload_checksum.checksum();
    adjusted_generation.manifest.extent_count =
        static_cast<std::uint32_t>(manual_entries.size() * 2u);
    const std::uint64_t directory_bytes =
        manual_entries.size() * minimizer_directory_entry_encoded_bytes(profile);
    adjusted_generation.manifest.directory_chunk_count = static_cast<std::uint32_t>(
        directory_bytes / k_grouped_soa_extent_payload_bytes +
        (directory_bytes % k_grouped_soa_extent_payload_bytes != 0u ? 1u : 0u));
    const auto metadata = rebuild_generation_metadata(adjusted_generation, manual_entries);
    const auto directory = grouped_soa_directory_view::decode(metadata.directory_bytes, profile);
    REQUIRE(directory.has_value());
    REQUIRE(validate_minimizer_generation_metadata(metadata.head, metadata.manifest, *directory)
                .has_value());
    auto mapped_payload = std::make_shared<std::vector<std::byte>>();
    mapped_payload->reserve(control_image.size() + position_image.size());
    mapped_payload->insert(mapped_payload->end(), control_image.begin(), control_image.end());
    mapped_payload->insert(mapped_payload->end(), position_image.begin(), position_image.end());
    REQUIRE(mapped_payload->size() == metadata.manifest.payload_bytes);

    const auto entries = directory->entries();
    REQUIRE(entries.size() == pairs.size());
    REQUIRE(entries.size() > 128u);
    REQUIRE(entries[127].position_segment == 0);
    REQUIRE(entries[128].position_segment == 0);
    REQUIRE(entries[129].position_segment == 0);
    REQUIRE(entries[130].position_segment == 2);
    REQUIRE(std::ranges::count_if(
                entries, [](const auto& entry) { return entry.position_segment == 0; }) == 130);
    REQUIRE(std::ranges::count_if(
                entries, [](const auto& entry) { return entry.position_segment == 1; }) == 0);
    REQUIRE(std::ranges::count_if(
                entries, [](const auto& entry) { return entry.position_segment == 2; }) == 2);

    const std::vector<minimap_sketch_data> query{
        minimap_sketch_data{1u, 14u, 0u},
        minimap_sketch_data{127u, 15u, 0u},
        minimap_sketch_data{128u, 16u, 0u},
        minimap_sketch_data{129u, 17u, 0u},
        minimap_sketch_data{130u, 18u, 0u},
        minimap_sketch_data{500u, 19u, 0u},
    };
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;
    auto compact =
        make_fragment_compact_direct_test_index(metadata.head, metadata.manifest, pairs, 10u);
    bioseq::parallelism::memory_admission_controller admission;
    std::shared_ptr<const void> payload_owner = mapped_payload;
    auto reader = prepared_minimizer_fragment_reader::open(
        admission,
        metadata.head,
        metadata.manifest,
        test_grouped_soa_expected_storage_contract(10u),
        metadata.directory_bytes,
        1u,
        std::move(payload_owner),
        std::span<const std::byte>{mapped_payload->data(), mapped_payload->size()},
        own_fragment_compact_direct_test_index(compact));
    REQUIRE(reader.has_value());
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());
    const std::array queries{
        fragment_seed_query{
            .minimizers = std::span<const minimap_sketch_data>{query},
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };
    const auto expected =
        ordinary_seed_anchors(pairs, 2u * segment_span + 100u, query, query_length, 1, kmer_size);
    auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 1, *scratch);
    const auto actual =
        batch.query_view(0u).ordered_fragment_seed_anchors(query, query_length, 1, kmer_size);
    require_anchor_bytes_equal(actual, expected);
    REQUIRE(actual.size() == query.size() - 1u);
    REQUIRE(std::ranges::none_of(actual, [segment_span](const auto& anchor) {
        return anchor.ref_position == 2u || anchor.ref_position == 2u * segment_span + 4u;
    }));
    REQUIRE(actual.back().ref_position == 2u * segment_span + 5u);
    REQUIRE(actual.back().is_reverse == 1u);
    scratch->complete_wave_after_join();

    auto direct_batch = reader->prepare_fragment_anchor_batch(
        admission,
        queries,
        1,
        *scratch,
        /*baseline_query_plan_matcher_for_testing=*/false,
        /*verify_compositional_order_for_testing=*/true,
        minimizer_fragment_lookup_route::direct_compact_hash_lookup);
    const auto direct = direct_batch.query_view(0u).ordered_fragment_seed_anchors(
        query, query_length, 1, kmer_size);
    require_anchor_bytes_equal(direct, expected);
    REQUIRE(direct_batch.stats().compact_direct_unique_lookups == query.size());
    REQUIRE(direct_batch.stats().compact_direct_selected_hashes == 5u);
    REQUIRE(direct_batch.stats().compact_direct_runtime_rejected_hashes == 1u);
    REQUIRE(direct_batch.stats().compact_direct_selected_postings == 5u);
    REQUIRE(direct_batch.stats().compact_direct_selection_shards == 5u);
    REQUIRE(direct_batch.stats().compact_direct_shard_query_layouts == 5u);
    REQUIRE(direct_batch.stats().compact_direct_layout_shards_completed == 5u);
    REQUIRE(direct_batch.stats().compact_direct_emission_shards_completed == 5u);
    REQUIRE(direct_batch.stats().compact_direct_posting_visit_passes == 1u);
    REQUIRE(direct_batch.stats().compact_direct_layout_postings_visited == 0u);
    REQUIRE(direct_batch.stats().compact_direct_emission_postings_visited == 5u);
    REQUIRE(direct_batch.stats().compact_direct_layout_selection_callbacks == 5u);
    REQUIRE(direct_batch.stats().compact_direct_emission_selection_callbacks == 5u);
    REQUIRE(direct_batch.stats().compact_direct_layout_spans_exposed == 5u);
    REQUIRE(direct_batch.stats().compact_direct_emission_spans_exposed == 5u);
    REQUIRE(direct_batch.stats().compact_direct_layout_descriptors_examined == 5u);
    REQUIRE(direct_batch.stats().compact_direct_emission_descriptors_examined == 5u);
    REQUIRE(direct_batch.stats().compact_direct_traversal_rank_checkpoints_examined == 0u);
    REQUIRE(direct_batch.stats().compact_direct_traversal_membership_words_examined == 0u);
    REQUIRE(direct_batch.stats().pass2_position_bytes == 0u);
    REQUIRE(direct_batch.stats().pass3_position_bytes ==
            5u * compact.view.header.posting_width_bytes);
    REQUIRE(direct_batch.stats().compact_direct_target_control_scan_bytes == 0u);
    REQUIRE(direct_batch.stats().compact_direct_fallbacks == 0u);
    scratch->complete_wave_after_join();
}

TEST_CASE("query-wave scratch recycles without stale anchors",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    constexpr auto packed = [](std::uint64_t position, std::uint64_t strand) {
        return (position << 1u) | strand;
    };
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, packed(10u, 0u)},
        {7u, packed(20u, 1u)},
        {8u, packed(30u, 0u)},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation =
        encode_minimizer_generation(producer, sink, test_grouped_soa_generation_options(100u, 10u));
    REQUIRE(generation.has_value());

    bioseq::parallelism::memory_admission_controller admission;
    auto reader = open_grouped_soa_fragment_query_reader(admission, *generation, sink.rows, 2u);
    REQUIRE(reader.has_value());
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());
    const std::vector<minimap_sketch_data> first_query{
        minimap_sketch_data{7u, 20u, 0u},
        minimap_sketch_data{7u, 25u, 1u},
    };
    const std::vector<minimap_sketch_data> empty_query{};
    const std::vector<minimap_sketch_data> third_query{
        minimap_sketch_data{8u, 30u, 0u},
    };
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;

    {
        const std::array queries{
            fragment_seed_query{
                std::span<const minimap_sketch_data>{first_query}, query_length, kmer_size},
            fragment_seed_query{
                std::span<const minimap_sketch_data>{empty_query}, query_length, kmer_size},
            fragment_seed_query{
                std::span<const minimap_sketch_data>{third_query}, query_length, kmer_size},
        };
        auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 2, *scratch);
        REQUIRE(batch.query_count() == queries.size());
        const auto first_expected =
            ordinary_seed_anchors(pairs, 100u, first_query, query_length, 2, kmer_size);
        const auto third_expected =
            ordinary_seed_anchors(pairs, 100u, third_query, query_length, 2, kmer_size);
        require_anchor_bytes_equal(batch.query_view(0u).ordered_fragment_seed_anchors(
                                       first_query, query_length, 2, kmer_size),
                                   first_expected);
        REQUIRE(batch.query_view(1u)
                    .ordered_fragment_seed_anchors(empty_query, query_length, 2, kmer_size)
                    .empty());
        require_anchor_bytes_equal(batch.query_view(2u).ordered_fragment_seed_anchors(
                                       third_query, query_length, 2, kmer_size),
                                   third_expected);
        scratch->complete_wave_after_join();
    }
    const std::uint64_t retained_after_first_wave = scratch->admitted_bytes();
    REQUIRE(retained_after_first_wave > 0u);

    const std::vector<minimap_sketch_data> last_query{
        minimap_sketch_data{8u, 14u, 0u},
    };
    {
        const std::array queries{
            fragment_seed_query{
                std::span<const minimap_sketch_data>{last_query}, query_length, kmer_size},
        };
        auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 2, *scratch);
        const auto view = batch.query_view(0u);
        const auto actual =
            view.ordered_fragment_seed_anchors(last_query, query_length, 2, kmer_size);
        const auto expected =
            ordinary_seed_anchors(pairs, 100u, last_query, query_length, 2, kmer_size);
        require_anchor_bytes_equal(actual, expected);
        REQUIRE(actual.size() == 1u);

        const std::vector<minimap_sketch_data> copied_query = last_query;
        REQUIRE_THROWS_AS(
            view.ordered_fragment_seed_anchors(copied_query, query_length, 2, kmer_size),
            persisted_minimizer_lookup_error);
        REQUIRE_THROWS_AS(
            view.ordered_fragment_seed_anchors(last_query, query_length + 1u, 2, kmer_size),
            persisted_minimizer_lookup_error);
        REQUIRE_THROWS_AS(
            view.ordered_fragment_seed_anchors(last_query, query_length, 1, kmer_size),
            persisted_minimizer_lookup_error);
        REQUIRE_THROWS_AS(
            view.ordered_fragment_seed_anchors(last_query, query_length, 2, kmer_size + 1),
            persisted_minimizer_lookup_error);
        scratch->complete_wave_after_join();
    }
    REQUIRE(scratch->admitted_bytes() == retained_after_first_wave);

    {
        const std::array<fragment_seed_query, 0u> empty_wave{};
        auto batch = reader->prepare_fragment_anchor_batch(admission, empty_wave, 2, *scratch);
        REQUIRE(batch.query_count() == 0u);
        scratch->complete_wave_after_join();
        REQUIRE_FALSE(scratch->wave_outstanding());
    }

    constexpr bioseq::storage::sequence_size_t packed_query_limit =
        bioseq::storage::sequence_size_t{1} << 28u;
    const std::vector<minimap_sketch_data> boundary_query{
        minimap_sketch_data{8u, static_cast<std::uint32_t>(packed_query_limit - 1u), 0u},
    };
    {
        const std::array queries{
            fragment_seed_query{std::span<const minimap_sketch_data>{boundary_query},
                                packed_query_limit,
                                kmer_size},
        };
        auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 2, *scratch);
        const auto actual = batch.query_view(0u).ordered_fragment_seed_anchors(
            boundary_query, packed_query_limit, 2, kmer_size);
        const auto expected =
            ordinary_seed_anchors(pairs, 100u, boundary_query, packed_query_limit, 2, kmer_size);
        require_anchor_bytes_equal(actual, expected);
        scratch->complete_wave_after_join();
    }
    const std::array oversized_query{
        fragment_seed_query{std::span<const minimap_sketch_data>{boundary_query},
                            packed_query_limit + 1u,
                            kmer_size},
    };
    REQUIRE_THROWS_AS(
        reader->prepare_fragment_anchor_batch(admission, oversized_query, 2, *scratch),
        persisted_minimizer_lookup_error);
    REQUIRE_FALSE(scratch->wave_outstanding());

    const std::array mismatched_kmer_query{
        fragment_seed_query{
            std::span<const minimap_sketch_data>{last_query}, query_length, kmer_size + 1},
    };
    REQUIRE_THROWS_AS(
        reader->prepare_fragment_anchor_batch(admission, mismatched_kmer_query, 2, *scratch),
        persisted_minimizer_lookup_error);
    REQUIRE_FALSE(scratch->wave_outstanding());

    const std::vector<minimap_sketch_data> out_of_profile_hash_query{
        minimap_sketch_data{static_cast<std::uint64_t>(k_minimap_k15_max_hash) + 1u, 14u, 0u},
    };
    const std::array out_of_profile_hash_queries{
        fragment_seed_query{std::span<const minimap_sketch_data>{out_of_profile_hash_query},
                            query_length,
                            kmer_size},
    };
    REQUIRE_THROWS_AS(
        reader->prepare_fragment_anchor_batch(admission, out_of_profile_hash_queries, 2, *scratch),
        persisted_minimizer_lookup_error);
    REQUIRE_FALSE(scratch->wave_outstanding());
}

TEST_CASE("sixteen-query compact packed wave preserves exact receipts",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, 20u},
        {7u, 40u},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    auto options = test_grouped_soa_generation_options(100u, 10u);
    options.position_segment_span_bp = std::uint64_t{1u} << 15u;
    const auto generation = encode_minimizer_generation(producer, sink, options);
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest.hash_width == minimizer_scalar_width::u32);
    REQUIRE(generation->manifest.count_width == minimizer_scalar_width::u16);
    REQUIRE(generation->manifest.position_width == minimizer_scalar_width::u16);
    REQUIRE(generation->directory.entries().size() == 1u);
    REQUIRE(generation->directory.entries().front().shape_offset_width ==
            minimizer_scalar_width::u16);

    bioseq::parallelism::memory_admission_controller admission;
    auto reader = open_grouped_soa_fragment_query_reader(admission, *generation, sink.rows, 2u);
    REQUIRE(reader.has_value());
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());

    const std::vector<minimap_sketch_data> minimizers{
        minimap_sketch_data{7u, 14u, 0u},
    };
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;
    constexpr std::size_t query_count = 16u;
    const std::vector<fragment_seed_query> queries(query_count,
                                                   fragment_seed_query{
                                                       .minimizers = minimizers,
                                                       .query_length = query_length,
                                                       .kmer_size = kmer_size,
                                                   });
    const auto expected =
        ordinary_seed_anchors(pairs, 100u, minimizers, query_length, 2, kmer_size);
    REQUIRE(expected.size() == 2u);

    auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 2, *scratch);
    REQUIRE(batch.query_count() == query_count);
    for (std::size_t query_index = 0u; query_index < query_count; ++query_index) {
        require_anchor_bytes_equal(
            batch.query_view(query_index)
                .ordered_fragment_seed_anchors(minimizers, query_length, 2, kmer_size),
            expected);
    }

    constexpr std::uint64_t control_scan_bytes = sizeof(std::uint32_t) + sizeof(std::uint16_t);
    constexpr std::uint64_t accepted_prefix_bytes = 2u * sizeof(std::uint16_t);
    constexpr std::uint64_t position_bytes = 2u * sizeof(std::uint16_t);
    const prepared_fragment_anchor_batch_stats& evidence = batch.stats();
    REQUIRE(evidence.waves == 1u);
    REQUIRE(evidence.queries == query_count);
    REQUIRE(evidence.query_occurrences == query_count);
    REQUIRE(evidence.unique_query_plans == 1u);
    REQUIRE(evidence.packed_occurrence_waves == 1u);
    REQUIRE(evidence.wide_occurrence_waves == 0u);
    REQUIRE(evidence.control_count_bytes == control_scan_bytes);
    REQUIRE(evidence.rejected_control_validation_bytes == 0u);
    REQUIRE(evidence.pass2_control_bytes == control_scan_bytes);
    REQUIRE(evidence.pass2_accepted_prefix_bytes == accepted_prefix_bytes);
    REQUIRE(evidence.pass2_position_bytes == position_bytes);
    REQUIRE(evidence.pass3_control_bytes == control_scan_bytes);
    REQUIRE(evidence.pass3_accepted_prefix_bytes == accepted_prefix_bytes);
    REQUIRE(evidence.pass3_position_bytes == position_bytes);
    REQUIRE(evidence.anchors_constructed == query_count * expected.size());
    REQUIRE(evidence.exact_final_bytes ==
            evidence.anchors_constructed * sizeof(bioseq::chaining::anchor));
    REQUIRE(evidence.peak_exact_final_bytes == evidence.exact_final_bytes);
    REQUIRE(batch.admitted_bytes() ==
            evidence.exact_final_bytes + query_count * sizeof(prepared_query_anchors));
    REQUIRE(evidence.global_sort_calls_invoked == 0u);
    REQUIRE(evidence.global_sort_calls_skipped == query_count);
    REQUIRE(evidence.compositional_sortedness_receipts == query_count);
    REQUIRE(evidence.forbidden_buffers_are_zero());
    REQUIRE_FALSE(evidence.saturated);

    const query_occurrence_capacity_snapshot occurrence_snapshot =
        scratch->occurrence_capacity_snapshot();
    REQUIRE(occurrence_snapshot.route == query_occurrence_route::packed);
    REQUIRE(occurrence_snapshot.packed_size == query_count);
    REQUIRE(occurrence_snapshot.wide_size == 0u);
    REQUIRE(occurrence_snapshot.query_plan_size == 0u);
    REQUIRE(occurrence_snapshot.query_plan_capacity == 0u);
    REQUIRE(occurrence_snapshot.bucket_direct_matcher_active);
    scratch->complete_wave_after_join();
    REQUIRE_FALSE(scratch->wave_outstanding());
}

TEST_CASE("bucket-direct Bloom matcher is byte-identical to its test oracle",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, 20u},
        {511u, 40u},
        {512u, 60u},
        // Low-9 mask 63 is covered by the union of query masks 0 and 28.
        // This absent hash therefore exercises Bloom-positive exact fallback.
        {8'192u + 63u, 70u},
        {1'048'575u, 80u},
        {1'048'576u, 100u},
        {1'073'741'822u, 120u},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation =
        encode_minimizer_generation(producer, sink, test_grouped_soa_generation_options(256u, 10u));
    REQUIRE(generation.has_value());

    const std::vector<minimap_sketch_data> minimizers{
        minimap_sketch_data{7u, 20u, 0u},
        minimap_sketch_data{7u, 20u, 0u},
        minimap_sketch_data{511u, 30u, 1u},
        minimap_sketch_data{512u, 40u, 0u},
        minimap_sketch_data{513u, 50u, 1u},
        minimap_sketch_data{8'192u + 0u, 52u, 0u},
        minimap_sketch_data{8'192u + 28u, 54u, 1u},
        minimap_sketch_data{1'048'575u, 60u, 0u},
        minimap_sketch_data{1'048'576u, 70u, 1u},
        minimap_sketch_data{1'073'741'822u, 80u, 0u},
    };
    constexpr bioseq::storage::sequence_size_t query_length = 128u;
    constexpr std::int32_t kmer_size = 15;
    const std::array queries{
        fragment_seed_query{
            .minimizers = minimizers,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };

    bioseq::parallelism::memory_admission_controller admission;
    auto reader = open_grouped_soa_fragment_query_reader(admission, *generation, sink.rows, 10u);
    REQUIRE(reader.has_value());
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());

    std::vector<bioseq::chaining::anchor> bucket_direct_anchors;
    prepared_fragment_anchor_batch_stats bucket_direct_evidence{};
    {
        auto batch =
            reader->prepare_fragment_anchor_batch(admission,
                                                  queries,
                                                  10,
                                                  *scratch,
                                                  /*baseline_query_plan_matcher_for_testing=*/false,
                                                  /*verify_compositional_order_for_testing=*/true);
        const auto anchors = batch.query_view(0u).ordered_fragment_seed_anchors(
            minimizers, query_length, 10, kmer_size);
        bucket_direct_anchors.assign(anchors.begin(), anchors.end());
        bucket_direct_evidence = batch.stats();
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.bucket_direct_matcher_active);
        REQUIRE(snapshot.query_plan_size == 0u);
        REQUIRE(snapshot.query_plan_capacity == 0u);
        REQUIRE(snapshot.query_plan_reserve_request == 0u);
        REQUIRE(snapshot.query_group_offset_size == bucket_direct_evidence.unique_query_plans + 1u);
        REQUIRE(snapshot.query_membership_filter_capacity_bytes == 16u * 1024u * 1024u);
        REQUIRE(bucket_direct_evidence.peak_query_membership_filter_capacity_bytes ==
                snapshot.query_membership_filter_capacity_bytes);
        REQUIRE(bucket_direct_evidence.query_membership_filter_allocations == 1u);
        REQUIRE(bucket_direct_evidence.bucket_direct_matcher_view_receipts == 1u);
        REQUIRE(bucket_direct_evidence.query_plan_allocations == 0u);
        REQUIRE(bucket_direct_evidence.peak_query_plan_capacity_bytes == 0u);
        REQUIRE(bucket_direct_evidence.peak_occurrence_count_capacity_bytes ==
                bucket_direct_evidence.unique_query_plans * sizeof(std::atomic<std::uint32_t>));
        REQUIRE(bucket_direct_evidence.peak_query_group_offset_capacity_bytes >=
                snapshot.query_group_offset_size * sizeof(std::uint32_t));
        REQUIRE(bucket_direct_evidence.compositional_sortedness_receipts == queries.size());
        REQUIRE(bucket_direct_evidence.final_sortedness_validation_bytes ==
                bucket_direct_evidence.exact_final_bytes);
        REQUIRE(batch.admitted_bytes() == bucket_direct_evidence.exact_final_bytes +
                                              queries.size() * sizeof(prepared_query_anchors));
        REQUIRE(admission.reserved_bytes() ==
                reader->admitted_bytes() + scratch->admitted_bytes() + batch.admitted_bytes());
        scratch->complete_wave_after_join();
    }

    {
        auto oracle_batch =
            reader->prepare_fragment_anchor_batch(admission,
                                                  queries,
                                                  10,
                                                  *scratch,
                                                  /*baseline_query_plan_matcher_for_testing=*/true,
                                                  /*verify_compositional_order_for_testing=*/true);
        const auto oracle_anchors = oracle_batch.query_view(0u).ordered_fragment_seed_anchors(
            minimizers, query_length, 10, kmer_size);
        require_anchor_bytes_equal(oracle_anchors, bucket_direct_anchors);
        const auto& oracle_evidence = oracle_batch.stats();
        REQUIRE(oracle_evidence.bucket_direct_matcher_view_receipts == 0u);
        REQUIRE(oracle_evidence.unique_query_plans == bucket_direct_evidence.unique_query_plans);
        REQUIRE(oracle_evidence.control_count_bytes == bucket_direct_evidence.control_count_bytes);
        REQUIRE(oracle_evidence.pass2_control_bytes == bucket_direct_evidence.pass2_control_bytes);
        REQUIRE(oracle_evidence.pass2_accepted_prefix_bytes ==
                bucket_direct_evidence.pass2_accepted_prefix_bytes);
        REQUIRE(oracle_evidence.pass2_position_bytes ==
                bucket_direct_evidence.pass2_position_bytes);
        REQUIRE(oracle_evidence.pass3_control_bytes == bucket_direct_evidence.pass3_control_bytes);
        REQUIRE(oracle_evidence.pass3_accepted_prefix_bytes ==
                bucket_direct_evidence.pass3_accepted_prefix_bytes);
        REQUIRE(oracle_evidence.pass3_position_bytes ==
                bucket_direct_evidence.pass3_position_bytes);
        REQUIRE(oracle_evidence.anchors_constructed == bucket_direct_evidence.anchors_constructed);
        REQUIRE(oracle_evidence.exact_final_bytes == bucket_direct_evidence.exact_final_bytes);
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE_FALSE(snapshot.bucket_direct_matcher_active);
        REQUIRE(snapshot.query_plan_size == bucket_direct_evidence.unique_query_plans);
        REQUIRE(snapshot.query_plan_reserve_request == bucket_direct_evidence.unique_query_plans);
        REQUIRE(oracle_batch.admitted_bytes() ==
                oracle_evidence.exact_final_bytes +
                    queries.size() * sizeof(prepared_query_anchors));
        REQUIRE(admission.reserved_bytes() == reader->admitted_bytes() + scratch->admitted_bytes() +
                                                  oracle_batch.admitted_bytes());
        scratch->complete_wave_after_join();
    }
}

TEST_CASE("scalar matcher differential covers counter mapping and dynamic leaves",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    constexpr auto packed_target = [](std::uint64_t bp, std::uint64_t strand) noexcept {
        return (bp << 1u) | strand;
    };
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, packed_target(10u, 0u)},
        {7u, packed_target(20u, 1u)},
        {7u, packed_target(30u, 0u)},
        {9u, packed_target(40u, 0u)},
        {9u, packed_target(50u, 1u)},
        {11u, packed_target(60u, 1u)},
    };
    const std::array query0{
        minimap_sketch_data{7u, 20u, 0u},
        minimap_sketch_data{9u, 30u, 1u},
        minimap_sketch_data{13u, 40u, 0u},
    };
    const std::array query1{
        minimap_sketch_data{7u, 22u, 1u},
        minimap_sketch_data{9u, 31u, 0u},
        minimap_sketch_data{11u, 42u, 1u},
        minimap_sketch_data{11u, 50u, 0u},
    };
    const std::array heavy_query0{
        minimap_sketch_data{7u, 20u, 0u},
    };
    const std::array heavy_query1{
        minimap_sketch_data{7u, 22u, 1u},
        minimap_sketch_data{7u, 35u, 0u},
    };
    constexpr bioseq::storage::sequence_size_t query_length = 128u;
    constexpr std::int32_t kmer_size = 15;
    constexpr std::int32_t max_occ = 2;
    const std::array mixed_queries{
        fragment_seed_query{
            .minimizers = query0,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
        fragment_seed_query{
            .minimizers = query1,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };
    const std::array all_heavy_queries{
        fragment_seed_query{
            .minimizers = heavy_query0,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
        fragment_seed_query{
            .minimizers = heavy_query1,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };

    struct differential_run {
        std::vector<std::vector<bioseq::chaining::anchor>> anchors{};
        prepared_fragment_anchor_batch_stats evidence{};
        std::uint64_t admitted_bytes = 0u;
    };

    const auto verify_fixture = [&pairs, &mixed_queries, &all_heavy_queries](
                                    std::uint64_t target_length,
                                    std::uint64_t segment_span,
                                    minimizer_scalar_width position_width) {
        vector_replay_minimizer_producer producer{pairs};
        collecting_minimizer_row_sink sink;
        auto options = test_grouped_soa_generation_options(target_length, 10u);
        options.position_segment_span_bp = segment_span;
        const auto generation = encode_minimizer_generation(producer, sink, options);
        REQUIRE(generation.has_value());
        REQUIRE(generation->manifest.hash_width == minimizer_scalar_width::u32);
        REQUIRE(generation->manifest.count_width == minimizer_scalar_width::u16);
        REQUIRE(generation->manifest.position_width == position_width);
        REQUIRE(std::ranges::all_of(generation->directory.entries(),
                                    [](const grouped_soa_bucket_directory_entry& entry) noexcept {
                                        return entry.shape_offset_width ==
                                               minimizer_scalar_width::u16;
                                    }));

        bioseq::parallelism::memory_admission_controller admission;
        auto reader =
            open_grouped_soa_fragment_query_reader(admission, *generation, sink.rows, max_occ);
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());

        const auto run = [&reader, &admission, &scratch](
                             std::span<const fragment_seed_query> queries, bool oracle) {
            differential_run result;
            auto batch = reader->prepare_fragment_anchor_batch(
                admission,
                queries,
                max_occ,
                *scratch,
                /*baseline_query_plan_matcher_for_testing=*/oracle,
                /*verify_compositional_order_for_testing=*/true);
            result.anchors.resize(queries.size());
            for (std::size_t query_ordinal = 0u; query_ordinal < queries.size(); ++query_ordinal) {
                const fragment_seed_query& query = queries[query_ordinal];
                const auto anchors =
                    batch.query_view(query_ordinal)
                        .ordered_fragment_seed_anchors(
                            query.minimizers, query.query_length, max_occ, query.kmer_size);
                result.anchors[query_ordinal].assign(anchors.begin(), anchors.end());
            }
            result.evidence = batch.stats();
            result.admitted_bytes = batch.admitted_bytes();
            REQUIRE(admission.reserved_bytes() ==
                    reader->admitted_bytes() + scratch->admitted_bytes() + batch.admitted_bytes());
            scratch->complete_wave_after_join();
            REQUIRE_FALSE(scratch->wave_outstanding());
            return result;
        };

        const auto compare_runs = [](const differential_run& actual,
                                     const differential_run& oracle) {
            REQUIRE(actual.anchors.size() == oracle.anchors.size());
            for (std::size_t query_ordinal = 0u; query_ordinal < actual.anchors.size();
                 ++query_ordinal) {
                require_anchor_bytes_equal(actual.anchors[query_ordinal],
                                           oracle.anchors[query_ordinal]);
            }
            require_hot_loop_differential_evidence_equal(actual.evidence, oracle.evidence);
            REQUIRE(actual.admitted_bytes == oracle.admitted_bytes);
        };

        const differential_run mixed_actual = run(mixed_queries, false);
        const differential_run mixed_oracle = run(mixed_queries, true);
        compare_runs(mixed_actual, mixed_oracle);
        REQUIRE(mixed_actual.anchors[0u].size() == 2u);
        REQUIRE(mixed_actual.anchors[1u].size() == 4u);
        for (std::size_t query_ordinal = 0u; query_ordinal < mixed_queries.size();
             ++query_ordinal) {
            const fragment_seed_query& query = mixed_queries[query_ordinal];
            const auto ordinary = ordinary_seed_anchors(pairs,
                                                        target_length,
                                                        query.minimizers,
                                                        query.query_length,
                                                        max_occ,
                                                        query.kmer_size);
            require_anchor_bytes_equal(mixed_actual.anchors[query_ordinal], ordinary);
        }
        constexpr std::uint64_t control_count_bytes =
            3u * (sizeof(std::uint32_t) + sizeof(std::uint16_t));
        constexpr std::uint64_t hash_scan_bytes = 3u * sizeof(std::uint32_t);
        constexpr std::uint64_t accepted_prefix_bytes = 2u * 2u * sizeof(std::uint16_t);
        constexpr std::uint64_t rejected_prefix_bytes = 2u * sizeof(std::uint16_t);
        const bool dynamic = position_width != minimizer_scalar_width::u16;
        REQUIRE(mixed_actual.evidence.query_occurrences == 7u);
        REQUIRE(mixed_actual.evidence.unique_query_plans == 4u);
        REQUIRE(mixed_actual.evidence.control_count_bytes == control_count_bytes);
        REQUIRE(mixed_actual.evidence.rejected_control_validation_bytes == rejected_prefix_bytes);
        REQUIRE(mixed_actual.evidence.pass2_control_bytes == control_count_bytes);
        REQUIRE(mixed_actual.evidence.pass2_accepted_prefix_bytes == accepted_prefix_bytes);
        REQUIRE(mixed_actual.evidence.pass2_position_bytes ==
                3u * static_cast<std::uint8_t>(position_width));
        REQUIRE(mixed_actual.evidence.pass3_control_bytes ==
                (dynamic ? control_count_bytes : hash_scan_bytes + 2u * sizeof(std::uint16_t)));
        REQUIRE(mixed_actual.evidence.pass3_accepted_prefix_bytes == accepted_prefix_bytes);
        REQUIRE(mixed_actual.evidence.pass3_position_bytes ==
                mixed_actual.evidence.pass2_position_bytes);
        REQUIRE(mixed_actual.evidence.anchors_constructed == 6u);
        REQUIRE(mixed_actual.evidence.exact_final_bytes == 6u * sizeof(bioseq::chaining::anchor));
        REQUIRE(mixed_actual.admitted_bytes ==
                mixed_actual.evidence.exact_final_bytes +
                    mixed_queries.size() * sizeof(prepared_query_anchors));

        const differential_run rejected_actual = run(all_heavy_queries, false);
        const differential_run rejected_oracle = run(all_heavy_queries, true);
        compare_runs(rejected_actual, rejected_oracle);
        REQUIRE(rejected_actual.anchors[0u].empty());
        REQUIRE(rejected_actual.anchors[1u].empty());
        REQUIRE(rejected_actual.evidence.control_count_bytes == control_count_bytes);
        REQUIRE(rejected_actual.evidence.rejected_control_validation_bytes ==
                (dynamic ? control_count_bytes + rejected_prefix_bytes
                         : hash_scan_bytes + sizeof(std::uint16_t) + rejected_prefix_bytes));
        REQUIRE(rejected_actual.evidence.pass2_control_bytes == 0u);
        REQUIRE(rejected_actual.evidence.pass2_accepted_prefix_bytes == 0u);
        REQUIRE(rejected_actual.evidence.pass2_position_bytes == 0u);
        REQUIRE(rejected_actual.evidence.pass3_control_bytes == 0u);
        REQUIRE(rejected_actual.evidence.pass3_accepted_prefix_bytes == 0u);
        REQUIRE(rejected_actual.evidence.pass3_position_bytes == 0u);
        REQUIRE(rejected_actual.evidence.anchors_constructed == 0u);
        REQUIRE(rejected_actual.evidence.exact_final_bytes == 0u);
        REQUIRE(rejected_actual.admitted_bytes ==
                all_heavy_queries.size() * sizeof(prepared_query_anchors));
    };

    SECTION("fixed H32 C16 P16 leaves") {
        verify_fixture(256u, std::uint64_t{1u} << 15u, minimizer_scalar_width::u16);
    }
    SECTION("checked dynamic H32 C16 P32 leaves") {
        verify_fixture((std::uint64_t{1u} << 15u) + 1u,
                       (std::uint64_t{1u} << 15u) + 1u,
                       minimizer_scalar_width::u32);
    }
}

TEST_CASE("packed and wide query occurrences recycle exact capacities",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, 20u},
        {8u, 41u},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation =
        encode_minimizer_generation(producer, sink, test_grouped_soa_generation_options(100u, 10u));
    REQUIRE(generation.has_value());
    auto compact =
        make_fragment_compact_direct_test_index(generation->head, generation->manifest, pairs, 10u);

    bioseq::parallelism::memory_admission_controller admission;
    auto reader = open_grouped_soa_fragment_query_reader(
        admission, *generation, sink.rows, 2u, own_fragment_compact_direct_test_index(compact));
    REQUIRE(reader.has_value());
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());

    constexpr std::size_t query_count = 65'536u;
    constexpr std::size_t present_query_ordinal = query_count - 1u;
    constexpr std::size_t singleton_query_ordinal = query_count - 2u;
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;
    std::vector<minimap_sketch_data> skewed_query(65'536u, minimap_sketch_data{6u, 14u, 0u});
    skewed_query[0] = minimap_sketch_data{7u, 14u, 0u};
    skewed_query[1] = minimap_sketch_data{7u, 20u, 1u};
    const std::vector<minimap_sketch_data> present_hash{
        minimap_sketch_data{7u, 14u, 0u},
    };
    const std::vector<minimap_sketch_data> singleton_hash{
        minimap_sketch_data{8u, 30u, 1u},
    };
    std::vector<fragment_seed_query> queries(query_count,
                                             fragment_seed_query{
                                                 .minimizers = {},
                                                 .query_length = query_length,
                                                 .kmer_size = kmer_size,
                                             });
    queries[0].minimizers = skewed_query;
    queries[singleton_query_ordinal].minimizers = singleton_hash;
    queries[present_query_ordinal].minimizers = present_hash;

    {
        auto batch = reader->prepare_fragment_anchor_batch(admission, queries, 2, *scratch);
        const auto actual =
            batch.query_view(present_query_ordinal)
                .ordered_fragment_seed_anchors(present_hash, query_length, 2, kmer_size);
        const auto expected =
            ordinary_seed_anchors(pairs, 100u, present_hash, query_length, 2, kmer_size);
        require_anchor_bytes_equal(actual, expected);
        const auto skewed_actual = batch.query_view(0u).ordered_fragment_seed_anchors(
            skewed_query, query_length, 2, kmer_size);
        const auto skewed_expected =
            ordinary_seed_anchors(pairs, 100u, skewed_query, query_length, 2, kmer_size);
        require_anchor_bytes_equal(skewed_actual, skewed_expected);
        REQUIRE(skewed_actual.size() == 2u);
        const auto& evidence = batch.stats();
        REQUIRE(evidence.query_occurrences == 65'538u);
        REQUIRE(evidence.unique_query_plans == 3u);
        REQUIRE(evidence.packed_occurrence_waves == 1u);
        REQUIRE(evidence.wide_occurrence_waves == 0u);
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.route == query_occurrence_route::packed);
        REQUIRE(snapshot.packed_size == 65'538u);
        REQUIRE(snapshot.wide_size == 0u);
        REQUIRE(snapshot.query_plan_size == 0u);
        REQUIRE(snapshot.query_plan_reserve_request == 0u);
        REQUIRE(snapshot.query_plan_capacity == 0u);
        REQUIRE(snapshot.bucket_direct_matcher_active);
        REQUIRE(snapshot.occurrence_radix_invoked);
        REQUIRE_FALSE(snapshot.parallel_occurrence_radix_split_selected);
        REQUIRE(snapshot.aggregate_occurrence_capacity_bytes ==
                evidence.peak_occurrence_capacity_bytes);
        scratch->complete_wave_after_join();
    }
    const auto retained_packed = scratch->occurrence_capacity_snapshot();
    REQUIRE(retained_packed.packed_capacity >= 65'538u);

    STATIC_REQUIRE(k_compact_direct_parallel_packed_occurrence_radix_min_count == 65'536u);
    std::vector<minimap_sketch_data> split_boundary_query = skewed_query;
    split_boundary_query[2u] = minimap_sketch_data{(std::uint32_t{1u} << 24u) - 1u, 14u, 0u};
    split_boundary_query[3u] = minimap_sketch_data{std::uint32_t{1u} << 24u, 14u, 0u};
    split_boundary_query[4u] = minimap_sketch_data{(std::uint32_t{2u} << 24u) - 1u, 14u, 0u};
    split_boundary_query[5u] = minimap_sketch_data{std::uint32_t{2u} << 24u, 14u, 0u};
    split_boundary_query[6u] = minimap_sketch_data{(std::uint32_t{3u} << 24u) - 1u, 14u, 0u};
    split_boundary_query[7u] = minimap_sketch_data{std::uint32_t{3u} << 24u, 14u, 0u};
    const auto run_compact_packed_boundary =
        [&pairs, &admission, &scratch, &generation, reader = &*reader](
            std::span<const minimap_sketch_data> minimizers, bool expected_split_selection) {
            const std::array boundary_queries{
                fragment_seed_query{
                    .minimizers = minimizers,
                    .query_length = query_length,
                    .kmer_size = kmer_size,
                },
            };
            const auto expected =
                ordinary_seed_anchors(pairs, 100u, minimizers, query_length, 2, kmer_size);
            auto batch = reader->prepare_fragment_anchor_batch(
                admission,
                boundary_queries,
                2,
                *scratch,
                /*baseline_query_plan_matcher_for_testing=*/false,
                /*verify_compositional_order_for_testing=*/true,
                minimizer_fragment_lookup_route::direct_compact_hash_lookup);
            const auto actual = batch.query_view(0u).ordered_fragment_seed_anchors(
                minimizers, query_length, 2, kmer_size);
            require_anchor_bytes_equal(actual, expected);

            const std::array<std::uint32_t, 8u> exact_unique_hashes{
                6u,
                7u,
                (std::uint32_t{1u} << 24u) - 1u,
                std::uint32_t{1u} << 24u,
                (std::uint32_t{2u} << 24u) - 1u,
                std::uint32_t{2u} << 24u,
                (std::uint32_t{3u} << 24u) - 1u,
                std::uint32_t{3u} << 24u,
            };
            const compact_direct_query_set_stats expected_query_set_stats{
                .query_count = boundary_queries.size(),
                .query_occurrence_count = minimizers.size(),
                .unique_hash_count = exact_unique_hashes.size(),
                .group_key_set_checksum = compact_direct_query_set_checksum(
                    exact_unique_hashes,
                    static_cast<std::uint32_t>(kmer_size),
                    static_cast<std::uint32_t>(generation->manifest.window_size),
                    2u),
                .present = true,
            };
            REQUIRE(batch.compact_direct_query_stats() == expected_query_set_stats);
            REQUIRE(batch.stats().unique_query_plans == exact_unique_hashes.size());
            REQUIRE(batch.stats().packed_occurrence_waves == 1u);
            REQUIRE(batch.stats().wide_occurrence_waves == 0u);

            const auto snapshot = scratch->occurrence_capacity_snapshot();
            REQUIRE(snapshot.route == query_occurrence_route::packed);
            REQUIRE(snapshot.packed_size == minimizers.size());
            REQUIRE(snapshot.wide_size == 0u);
            REQUIRE(snapshot.query_plan_size == exact_unique_hashes.size());
            REQUIRE(snapshot.occurrence_radix_invoked);
            // This receipt records the exact split-route threshold and output
            // parity; it deliberately makes no claim about observed pool workers.
            REQUIRE(snapshot.parallel_occurrence_radix_split_selected == expected_split_selection);
            REQUIRE_FALSE(snapshot.bucket_direct_matcher_active);
            REQUIRE(snapshot.aggregate_occurrence_capacity_bytes ==
                    batch.stats().peak_occurrence_capacity_bytes);
            scratch->complete_wave_after_join();
        };
    const std::span<const minimap_sketch_data> skewed_span{split_boundary_query};
    run_compact_packed_boundary(
        skewed_span.first(k_compact_direct_parallel_packed_occurrence_radix_min_count - 1u), false);
    run_compact_packed_boundary(
        skewed_span.first(k_compact_direct_parallel_packed_occurrence_radix_min_count), true);

    skewed_query.push_back(minimap_sketch_data{6u, 14u, 0u});
    queries[0].minimizers = skewed_query;
    std::vector<bioseq::chaining::anchor> wide_production_query0;
    std::vector<bioseq::chaining::anchor> wide_production_present_query;
    prepared_fragment_anchor_batch_stats wide_production_evidence{};
    std::uint64_t wide_production_admitted_bytes = 0u;
    {
        auto batch =
            reader->prepare_fragment_anchor_batch(admission,
                                                  queries,
                                                  2,
                                                  *scratch,
                                                  /*baseline_query_plan_matcher_for_testing=*/false,
                                                  /*verify_compositional_order_for_testing=*/true);
        const auto actual =
            batch.query_view(present_query_ordinal)
                .ordered_fragment_seed_anchors(present_hash, query_length, 2, kmer_size);
        const auto expected =
            ordinary_seed_anchors(pairs, 100u, present_hash, query_length, 2, kmer_size);
        require_anchor_bytes_equal(actual, expected);
        const auto skewed_actual = batch.query_view(0u).ordered_fragment_seed_anchors(
            skewed_query, query_length, 2, kmer_size);
        const auto skewed_expected =
            ordinary_seed_anchors(pairs, 100u, skewed_query, query_length, 2, kmer_size);
        require_anchor_bytes_equal(skewed_actual, skewed_expected);
        REQUIRE(skewed_actual.size() == 2u);
        const auto& evidence = batch.stats();
        wide_production_query0.assign(skewed_actual.begin(), skewed_actual.end());
        wide_production_present_query.assign(actual.begin(), actual.end());
        wide_production_evidence = evidence;
        wide_production_admitted_bytes = batch.admitted_bytes();
        REQUIRE(evidence.query_occurrences == 65'539u);
        REQUIRE(evidence.unique_query_plans == 3u);
        REQUIRE(evidence.packed_occurrence_waves == 0u);
        REQUIRE(evidence.wide_occurrence_waves == 1u);
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.route == query_occurrence_route::wide);
        REQUIRE(snapshot.packed_size == 0u);
        REQUIRE(snapshot.wide_size == 65'539u);
        REQUIRE(snapshot.packed_capacity == retained_packed.packed_capacity);
        REQUIRE(snapshot.wide_capacity >= 65'539u);
        REQUIRE(snapshot.query_plan_size == 0u);
        REQUIRE(snapshot.query_plan_reserve_request == 0u);
        REQUIRE(snapshot.query_plan_capacity == 0u);
        REQUIRE(snapshot.bucket_direct_matcher_active);
        REQUIRE(snapshot.occurrence_radix_invoked);
        REQUIRE_FALSE(snapshot.parallel_occurrence_radix_split_selected);
        REQUIRE(snapshot.aggregate_occurrence_capacity_bytes ==
                evidence.peak_occurrence_capacity_bytes);
        REQUIRE(admission.reserved_bytes() ==
                reader->admitted_bytes() + scratch->admitted_bytes() + batch.admitted_bytes());
        scratch->complete_wave_after_join();
    }
    {
        auto oracle_batch =
            reader->prepare_fragment_anchor_batch(admission,
                                                  queries,
                                                  2,
                                                  *scratch,
                                                  /*baseline_query_plan_matcher_for_testing=*/true,
                                                  /*verify_compositional_order_for_testing=*/true);
        const auto oracle_query0 = oracle_batch.query_view(0u).ordered_fragment_seed_anchors(
            skewed_query, query_length, 2, kmer_size);
        const auto oracle_present_query =
            oracle_batch.query_view(present_query_ordinal)
                .ordered_fragment_seed_anchors(present_hash, query_length, 2, kmer_size);
        require_anchor_bytes_equal(oracle_query0, wide_production_query0);
        require_anchor_bytes_equal(oracle_present_query, wide_production_present_query);
        require_hot_loop_differential_evidence_equal(wide_production_evidence,
                                                     oracle_batch.stats());
        REQUIRE(oracle_batch.admitted_bytes() == wide_production_admitted_bytes);
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.route == query_occurrence_route::wide);
        REQUIRE(snapshot.wide_size == 65'539u);
        REQUIRE(snapshot.query_plan_size == 3u);
        REQUIRE(snapshot.query_plan_reserve_request == 3u);
        REQUIRE_FALSE(snapshot.bucket_direct_matcher_active);
        REQUIRE(admission.reserved_bytes() == reader->admitted_bytes() + scratch->admitted_bytes() +
                                                  oracle_batch.admitted_bytes());
        scratch->complete_wave_after_join();
    }
    {
        auto direct_batch = reader->prepare_fragment_anchor_batch(
            admission,
            queries,
            2,
            *scratch,
            /*baseline_query_plan_matcher_for_testing=*/false,
            /*verify_compositional_order_for_testing=*/true,
            minimizer_fragment_lookup_route::direct_compact_hash_lookup);
        const auto direct_query0 = direct_batch.query_view(0u).ordered_fragment_seed_anchors(
            skewed_query, query_length, 2, kmer_size);
        const auto direct_present_query =
            direct_batch.query_view(present_query_ordinal)
                .ordered_fragment_seed_anchors(present_hash, query_length, 2, kmer_size);
        const auto direct_singleton_query =
            direct_batch.query_view(singleton_query_ordinal)
                .ordered_fragment_seed_anchors(singleton_hash, query_length, 2, kmer_size);
        const auto expected_singleton_query =
            ordinary_seed_anchors(pairs, 100u, singleton_hash, query_length, 2, kmer_size);
        require_anchor_bytes_equal(direct_query0, wide_production_query0);
        require_anchor_bytes_equal(direct_present_query, wide_production_present_query);
        require_anchor_bytes_equal(direct_singleton_query, expected_singleton_query);
        REQUIRE(direct_singleton_query.size() == 1u);
        REQUIRE(direct_batch.stats().wide_occurrence_waves == 1u);
        REQUIRE(direct_batch.stats().packed_occurrence_waves == 0u);
        REQUIRE(direct_batch.stats().compact_direct_unique_lookups == 3u);
        REQUIRE(direct_batch.stats().compact_direct_selected_hashes == 2u);
        REQUIRE(direct_batch.stats().compact_direct_selected_postings == 2u);
        REQUIRE(direct_batch.stats().compact_direct_posting_visit_passes == 1u);
        REQUIRE(direct_batch.stats().compact_direct_emission_postings_visited == 2u);
        REQUIRE(direct_batch.stats().compact_direct_rejected_posting_reads == 0u);
        REQUIRE(direct_batch.stats().pass2_position_bytes == 0u);
        REQUIRE(direct_batch.stats().pass3_position_bytes ==
                2u * compact.view.header.posting_width_bytes);
        REQUIRE(direct_batch.stats().anchors_constructed == 4u);
        REQUIRE(direct_batch.stats().forbidden_buffers_are_zero());
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.route == query_occurrence_route::wide);
        REQUIRE(snapshot.wide_size == 65'539u);
        REQUIRE(snapshot.query_plan_size == 3u);
        REQUIRE(snapshot.query_membership_filter_capacity_bytes == 0u);
        REQUIRE(snapshot.query_group_offset_capacity == 0u);
        REQUIRE_FALSE(snapshot.parallel_occurrence_radix_split_selected);
        REQUIRE_FALSE(snapshot.bucket_direct_matcher_active);
        scratch->complete_wave_after_join();
    }
    const auto retained_both = scratch->occurrence_capacity_snapshot();
    REQUIRE(retained_both.packed_capacity > 0u);
    REQUIRE(retained_both.wide_capacity > 0u);

    const std::array smaller_wave{
        fragment_seed_query{
            .minimizers = present_hash,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };
    {
        auto batch = reader->prepare_fragment_anchor_batch(admission, smaller_wave, 2, *scratch);
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.route == query_occurrence_route::packed);
        REQUIRE(snapshot.packed_size == 1u);
        REQUIRE(snapshot.wide_size == 0u);
        REQUIRE(snapshot.packed_capacity == retained_both.packed_capacity);
        REQUIRE(snapshot.wide_capacity == retained_both.wide_capacity);
        REQUIRE(snapshot.query_plan_reserve_request == 0u);
        REQUIRE(snapshot.query_plan_capacity == 0u);
        REQUIRE(snapshot.bucket_direct_matcher_active);
        REQUIRE_FALSE(snapshot.occurrence_radix_invoked);
        REQUIRE(snapshot.aggregate_occurrence_capacity_bytes ==
                8u * snapshot.packed_capacity + 12u * snapshot.wide_capacity);
        REQUIRE(batch.stats().query_occurrences == 1u);
        REQUIRE(batch.stats().unique_query_plans == 1u);
        REQUIRE(batch.stats().packed_occurrence_waves == 1u);
        REQUIRE(batch.stats().wide_occurrence_waves == 0u);
        scratch->complete_wave_after_join();
    }

    const std::array small_compact_minimizers{
        minimap_sketch_data{8u, 30u, 1u},
        minimap_sketch_data{7u, 14u, 0u},
        minimap_sketch_data{7u, 20u, 1u},
    };
    const std::array small_compact_wave{
        fragment_seed_query{
            .minimizers = small_compact_minimizers,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };
    {
        auto batch = reader->prepare_fragment_anchor_batch(
            admission,
            small_compact_wave,
            2,
            *scratch,
            /*baseline_query_plan_matcher_for_testing=*/false,
            /*verify_compositional_order_for_testing=*/true,
            minimizer_fragment_lookup_route::direct_compact_hash_lookup);
        const auto actual = batch.query_view(0u).ordered_fragment_seed_anchors(
            small_compact_minimizers, query_length, 2, kmer_size);
        const auto expected = ordinary_seed_anchors(
            pairs, 100u, small_compact_minimizers, query_length, 2, kmer_size);
        require_anchor_bytes_equal(actual, expected);
        const std::array<std::uint32_t, 2u> exact_unique_hashes{7u, 8u};
        const compact_direct_query_set_stats expected_query_set_stats{
            .query_count = small_compact_wave.size(),
            .query_occurrence_count = small_compact_minimizers.size(),
            .unique_hash_count = exact_unique_hashes.size(),
            .group_key_set_checksum = compact_direct_query_set_checksum(
                exact_unique_hashes,
                static_cast<std::uint32_t>(kmer_size),
                static_cast<std::uint32_t>(generation->manifest.window_size),
                2u),
            .present = true,
        };
        REQUIRE(batch.compact_direct_query_stats() == expected_query_set_stats);
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.route == query_occurrence_route::packed);
        REQUIRE(snapshot.occurrence_radix_invoked);
        REQUIRE_FALSE(snapshot.parallel_occurrence_radix_split_selected);
        scratch->complete_wave_after_join();
    }

    constexpr std::size_t unique_count = 1'024u;
    constexpr bioseq::storage::sequence_size_t unique_query_length = 2'048u;
    std::vector<minimap_sketch_data> all_unique;
    all_unique.reserve(unique_count);
    for (std::size_t ordinal = 0u; ordinal < unique_count; ++ordinal) {
        all_unique.push_back(minimap_sketch_data{
            100u + ordinal,
            static_cast<std::uint32_t>(14u + ordinal),
            static_cast<std::uint32_t>(ordinal & 1u),
        });
    }
    const std::array all_unique_wave{
        fragment_seed_query{
            .minimizers = all_unique,
            .query_length = unique_query_length,
            .kmer_size = kmer_size,
        },
    };
    {
        auto batch = reader->prepare_fragment_anchor_batch(admission, all_unique_wave, 2, *scratch);
        const auto actual = batch.query_view(0u).ordered_fragment_seed_anchors(
            all_unique, unique_query_length, 2, kmer_size);
        REQUIRE(actual.empty());
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.route == query_occurrence_route::packed);
        REQUIRE(snapshot.packed_size == unique_count);
        REQUIRE(snapshot.wide_size == 0u);
        REQUIRE(snapshot.query_plan_size == 0u);
        REQUIRE(snapshot.query_plan_capacity == 0u);
        REQUIRE(snapshot.query_plan_reserve_request == 0u);
        REQUIRE(snapshot.bucket_direct_matcher_active);
        REQUIRE(snapshot.occurrence_radix_invoked);
        REQUIRE(batch.stats().query_occurrences == unique_count);
        REQUIRE(batch.stats().unique_query_plans == unique_count);
        scratch->complete_wave_after_join();
    }

    const std::array<fragment_seed_query, 0u> empty_wave{};
    {
        auto batch = reader->prepare_fragment_anchor_batch(admission, empty_wave, 2, *scratch);
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.route == query_occurrence_route::packed);
        REQUIRE(snapshot.packed_size == 0u);
        REQUIRE(snapshot.wide_size == 0u);
        REQUIRE(snapshot.query_plan_size == 0u);
        REQUIRE(snapshot.query_plan_reserve_request == 0u);
        REQUIRE_FALSE(snapshot.occurrence_radix_invoked);
        REQUIRE_FALSE(snapshot.parallel_occurrence_radix_split_selected);
        REQUIRE(batch.stats().query_occurrences == 0u);
        REQUIRE(batch.stats().unique_query_plans == 0u);
        REQUIRE(batch.stats().packed_occurrence_waves == 1u);
        REQUIRE(batch.stats().wide_occurrence_waves == 0u);
        scratch->complete_wave_after_join();
    }

    scratch->release_retained_capacity();
    REQUIRE(scratch->admitted_bytes() == 0u);
}

TEST_CASE("compact-direct selected-page advice is exact coalesced and fail open",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;

    SECTION("overlap and touch merge while real gaps flush") {
        selected_page_advice_recorder recorder;
        const auto adviser = recorder.token();
        compact_direct_selected_page_advice_accumulator accumulator;
        REQUIRE(accumulator.accumulate(adviser, {.begin = 100u, .end = 200u}));
        REQUIRE(accumulator.accumulate(adviser, {.begin = 150u, .end = 240u}));
        REQUIRE(accumulator.accumulate(adviser, {.begin = 240u, .end = 300u}));
        REQUIRE(recorder.record_count.load() == 0u);

        REQUIRE(accumulator.accumulate(adviser, {.begin = 400u, .end = 450u}));
        REQUIRE(recorder.record_count.load() == 1u);
        REQUIRE(recorder.records[0u] == compact_direct_selected_page_range{
                                            .begin = 100u,
                                            .end = 300u,
                                        });
        accumulator.flush(adviser);
        REQUIRE(recorder.record_count.load() == 2u);
        REQUIRE(recorder.records[1u] == compact_direct_selected_page_range{
                                            .begin = 400u,
                                            .end = 450u,
                                        });
        REQUIRE_FALSE(accumulator.pending);
        REQUIRE_FALSE(accumulator.accumulate(adviser, {.begin = 500u, .end = 500u}));
    }

    SECTION("a callback which drops advice cannot affect control flow") {
        selected_page_advice_recorder recorder;
        recorder.drop_advice.store(true);
        const auto adviser = recorder.token();
        compact_direct_selected_page_advice_accumulator accumulator;
        REQUIRE(accumulator.accumulate(adviser, {.begin = 100u, .end = 200u}));
        accumulator.flush(adviser);
        REQUIRE_FALSE(accumulator.pending);
        REQUIRE(recorder.record_count.load() == 0u);
    }

    SECTION("alignment and arithmetic overflow remain release active") {
        const auto aligned = compact_direct_align_selected_page_range(0x1003u, 2u, 4096u);
        REQUIRE(aligned.has_value());
        REQUIRE(*aligned == compact_direct_selected_page_range{
                                .begin = 0x1000u,
                                .end = 0x2000u,
                            });
        const auto overflow = compact_direct_align_selected_page_range(
            (std::numeric_limits<std::uintptr_t>::max)() - 1u, 2u, 4096u);
        REQUIRE_FALSE(overflow.has_value());
        REQUIRE(overflow.error() == direct_inverted_index_error::arithmetic_overflow);
        const auto empty = compact_direct_align_selected_page_range(0x1000u, 0u, 4096u);
        REQUIRE_FALSE(empty.has_value());
        REQUIRE(empty.error() == direct_inverted_index_error::query_plan);
    }

    SECTION("P32 inline descriptors are excluded and posting bounds reject") {
        fragment_compact_direct_source source{
            .runs =
                {
                    fragment_compact_direct_run{
                        .header =
                            compact_hash32_sorted_run_header{
                                .hash = 7u,
                                .global_count = 2u,
                                .target_strand_zero_count = 1u,
                            },
                        .encoded_positions = {20u, 23u},
                    },
                    fragment_compact_direct_run{
                        .header =
                            compact_hash32_sorted_run_header{
                                .hash = 8u,
                                .global_count = 1u,
                                .target_strand_zero_count = 1u,
                            },
                        .encoded_positions = {40u},
                    },
                },
        };
        const direct_index_identity identity{
            .generation_id = 1u,
            .source_identity = 2u,
            .storage_contract_checksum = 3u,
            .target_length = 400u,
        };
        const compact_hash32_geometry geometry{
            .hash_domain_bits = 9u,
            .max_hash = 511u,
        };
        const compact_hash32_build_options options{
            .identity = identity,
            .max_occ_cap = 4u,
            .geometry = geometry,
        };
        const compact_hash32_sorted_run_source source_ref{
            .context = &source,
            .scan_sorted_runs_fn = &scan_fragment_compact_direct_runs,
        };
        auto image = build_compact_hash32_direct_index(source_ref, options);
        REQUIRE(image.has_value());
        const auto direct =
            validate_compact_hash32_direct_index(*image, identity, options.max_occ_cap, geometry);
        REQUIRE(direct.has_value());
        REQUIRE(direct->header.posting_width_bytes == 4u);
        REQUIRE(direct->header.postings_bytes == 8u);

        selected_page_advice_recorder recorder;
        const auto adviser = recorder.token();
        const auto posting_span =
            direct->image.subspan(static_cast<std::size_t>(direct->header.postings_offset), 8u);
        const auto selected = compact_direct_select_page_range(adviser, *direct, posting_span);
        REQUIRE(selected.has_value());
        REQUIRE(selected->has_value());
        REQUIRE(**selected == compact_direct_selected_page_range{
                                  .begin = reinterpret_cast<std::uintptr_t>(posting_span.data()),
                                  .end = reinterpret_cast<std::uintptr_t>(posting_span.data()) +
                                         posting_span.size(),
                              });

        const auto inline_descriptor =
            direct->image.subspan(static_cast<std::size_t>(direct->header.descriptors_offset +
                                                           direct->header.descriptor_width_bytes),
                                  4u);
        const auto excluded = compact_direct_select_page_range(adviser, *direct, inline_descriptor);
        REQUIRE(excluded.has_value());
        REQUIRE_FALSE(excluded->has_value());

        const auto outside_sections =
            compact_direct_select_page_range(adviser, *direct, direct->image.first(1u));
        REQUIRE_FALSE(outside_sections.has_value());
        REQUIRE(outside_sections.error() == direct_inverted_index_error::query_plan);
        auto malformed = *direct;
        malformed.header.postings_bytes = malformed.image.size();
        const auto rejected_bounds =
            compact_direct_select_page_range(adviser, malformed, posting_span);
        REQUIRE_FALSE(rejected_bounds.has_value());
        REQUIRE(rejected_bounds.error() == direct_inverted_index_error::section_layout);
    }

    SECTION("P64 singleton postings remain selected") {
        fragment_compact_direct_source source{
            .runs =
                {
                    fragment_compact_direct_run{
                        .header =
                            compact_hash32_sorted_run_header{
                                .hash = 7u,
                                .global_count = 1u,
                                .target_strand_zero_count = 1u,
                            },
                        .encoded_positions = {20u},
                    },
                },
        };
        const direct_index_identity identity{
            .generation_id = 4u,
            .source_identity = 5u,
            .storage_contract_checksum = 6u,
            .target_length = (std::uint64_t{1u} << 31u) + 1u,
        };
        const compact_hash32_geometry geometry{
            .hash_domain_bits = 9u,
            .max_hash = 511u,
        };
        const compact_hash32_build_options options{
            .identity = identity,
            .max_occ_cap = 4u,
            .geometry = geometry,
        };
        const compact_hash32_sorted_run_source source_ref{
            .context = &source,
            .scan_sorted_runs_fn = &scan_fragment_compact_direct_runs,
        };
        auto image = build_compact_hash32_direct_index(source_ref, options);
        REQUIRE(image.has_value());
        const auto direct =
            validate_compact_hash32_direct_index(*image, identity, options.max_occ_cap, geometry);
        REQUIRE(direct.has_value());
        REQUIRE(direct->header.posting_width_bytes == 8u);
        REQUIRE(direct->header.postings_bytes == 8u);

        selected_page_advice_recorder recorder;
        const auto adviser = recorder.token();
        const auto posting_span =
            direct->image.subspan(static_cast<std::size_t>(direct->header.postings_offset), 8u);
        const auto selected = compact_direct_select_page_range(adviser, *direct, posting_span);
        REQUIRE(selected.has_value());
        REQUIRE(selected->has_value());
        REQUIRE((**selected).end - (**selected).begin == 8u);
    }
}

TEST_CASE("compact-direct query-set checksum preserves canonical-u32 identity",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    constexpr std::uint32_t kmer_size = 15u;
    constexpr std::uint32_t window_size = 10u;
    constexpr std::uint32_t max_occ = 500u;
    std::vector<std::uint32_t> duplicate_heavy{(std::numeric_limits<std::uint32_t>::max)(),
                                               0u,
                                               1u,
                                               0u,
                                               1u,
                                               1u,
                                               (std::numeric_limits<std::uint32_t>::max)(),
                                               0x7fffffffu,
                                               0x7fffffffu,
                                               0u};
    std::sort(duplicate_heavy.begin(), duplicate_heavy.end());
    duplicate_heavy.erase(std::unique(duplicate_heavy.begin(), duplicate_heavy.end()),
                          duplicate_heavy.end());
    const std::vector<std::uint32_t> expected_unique{
        0u, 1u, 0x7fffffffu, (std::numeric_limits<std::uint32_t>::max)()};
    REQUIRE(duplicate_heavy == expected_unique);

    const auto reference_mix = [](std::uint64_t hash, std::uint64_t value) noexcept {
        for (int byte_index = 0; byte_index < 8; ++byte_index) {
            hash ^= value & UINT64_C(0xff);
            hash *= UINT64_C(1099511628211);
            value >>= 8u;
        }
        return hash;
    };
    std::uint64_t reference_checksum = UINT64_C(1469598103934665603);
    reference_checksum = reference_mix(reference_checksum, kmer_size);
    reference_checksum = reference_mix(reference_checksum, window_size);
    reference_checksum = reference_mix(reference_checksum, max_occ);
    for (const std::uint32_t hash : duplicate_heavy) {
        reference_checksum = reference_mix(reference_checksum, hash);
    }
    REQUIRE(compact_direct_query_set_checksum(duplicate_heavy, kmer_size, window_size, max_occ) ==
            reference_checksum);
    REQUIRE(compact_direct_query_set_checksum(
                std::span<const std::uint32_t>{}, kmer_size, window_size, max_occ) ==
            compact_direct_query_set_checksum_prefix(kmer_size, window_size, max_occ));
}

TEST_CASE("compact direct fragment route is exact and scan free",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    constexpr auto packed = [](std::uint64_t bp, std::uint64_t strand) noexcept {
        return (bp << 1u) | strand;
    };
    constexpr std::uint32_t build_cap = 4u;
    constexpr std::int32_t runtime_max_occ = 2;
    constexpr std::uint64_t target_length = 400u;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, packed(20u, 0u)},
        {7u, packed(150u, 1u)},
        {8u, packed(30u, 0u)},
        {8u, packed(31u, 0u)},
        {8u, packed(32u, 1u)},
        {9u, packed(40u, 0u)},
        {9u, packed(41u, 0u)},
        {9u, packed(42u, 0u)},
        {9u, packed(43u, 1u)},
        {9u, packed(44u, 1u)},
        {10u, packed(250u, 0u)},
        {10u, packed(251u, 1u)},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink, test_grouped_soa_generation_options(target_length, build_cap));
    REQUIRE(generation.has_value());
    auto compact = make_fragment_compact_direct_test_index(
        generation->head, generation->manifest, pairs, build_cap);

    const std::vector<minimap_sketch_data> query0{
        minimap_sketch_data{7u, 14u, 0u},
        minimap_sketch_data{7u, 20u, 1u},
        minimap_sketch_data{8u, 30u, 0u},
        minimap_sketch_data{9u, 40u, 1u},
        minimap_sketch_data{11u, 50u, 0u},
    };
    const std::vector<minimap_sketch_data> query1{
        minimap_sketch_data{10u, 14u, 0u},
        minimap_sketch_data{7u, 25u, 1u},
    };
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;
    const std::array queries{
        fragment_seed_query{
            .minimizers = query0,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
        fragment_seed_query{
            .minimizers = query1,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };

    {
        bioseq::parallelism::memory_admission_controller no_direct_admission;
        auto no_direct_reader = open_grouped_soa_fragment_query_reader(
            no_direct_admission, *generation, sink.rows, runtime_max_occ);
        REQUIRE(no_direct_reader.has_value());
        auto no_direct_scratch = query_wave_scratch::create(no_direct_admission);
        REQUIRE(no_direct_scratch.has_value());
        REQUIRE_THROWS_AS(no_direct_reader->prepare_fragment_anchor_batch(
                              no_direct_admission,
                              queries,
                              runtime_max_occ,
                              *no_direct_scratch,
                              /*baseline_query_plan_matcher_for_testing=*/false,
                              /*verify_compositional_order_for_testing=*/false,
                              minimizer_fragment_lookup_route::direct_compact_hash_lookup),
                          persisted_minimizer_lookup_error);
        REQUIRE_FALSE(no_direct_scratch->wave_outstanding());
    }

    {
        auto subset_view = compact.view;
        REQUIRE(subset_view.header.geometry.hash_domain_bits > 9u);
        --subset_view.header.geometry.hash_domain_bits;
        subset_view.header.geometry.max_hash = static_cast<std::uint32_t>(
            (std::uint64_t{1u} << subset_view.header.geometry.hash_domain_bits) - 1u);
        bioseq::parallelism::memory_admission_controller subset_admission;
        const auto subset_reader = open_grouped_soa_fragment_query_reader(
            subset_admission,
            *generation,
            sink.rows,
            runtime_max_occ,
            own_fragment_compact_direct_test_index(compact, subset_view));
        REQUIRE_FALSE(subset_reader.has_value());
        REQUIRE(subset_reader.error() ==
                minimizer_generation_encoding_error::storage_contract_mismatch);

        auto superset_view = compact.view;
        REQUIRE(superset_view.header.geometry.hash_domain_bits < 32u);
        ++superset_view.header.geometry.hash_domain_bits;
        superset_view.header.geometry.max_hash =
            superset_view.header.geometry.hash_domain_bits == 32u
                ? (std::numeric_limits<std::uint32_t>::max)()
                : static_cast<std::uint32_t>(
                      (std::uint64_t{1u} << superset_view.header.geometry.hash_domain_bits) - 1u);
        bioseq::parallelism::memory_admission_controller superset_admission;
        const auto superset_reader = open_grouped_soa_fragment_query_reader(
            superset_admission,
            *generation,
            sink.rows,
            runtime_max_occ,
            own_fragment_compact_direct_test_index(compact, superset_view));
        REQUIRE_FALSE(superset_reader.has_value());
        REQUIRE(superset_reader.error() ==
                minimizer_generation_encoding_error::storage_contract_mismatch);

        auto unrelated = std::make_shared<std::vector<std::byte>>(1u);
        std::shared_ptr<const std::byte> unrelated_alias{unrelated, unrelated->data()};
        REQUIRE_FALSE(
            owned_compact_hash32_direct_index_view::bind(std::move(unrelated_alias), compact.view));
    }

    {
        auto direct_only_compact = make_fragment_compact_direct_test_index(
            generation->head, generation->manifest, pairs, build_cap);
        selected_page_advice_recorder selected_page_recorder;
        auto wrong_segment_manifest = generation->manifest;
        ++wrong_segment_manifest.position_segment_count;
        bioseq::parallelism::memory_admission_controller wrong_segment_admission;
        const auto wrong_segment_reader = prepared_minimizer_fragment_reader::open_compact_direct(
            wrong_segment_admission,
            generation->head,
            wrong_segment_manifest,
            test_grouped_soa_expected_storage_contract(build_cap),
            runtime_max_occ,
            own_fragment_compact_direct_test_index(direct_only_compact));
        REQUIRE_FALSE(wrong_segment_reader.has_value());
        REQUIRE(wrong_segment_reader.error() == minimizer_generation_encoding_error::segment_count);
        REQUIRE(wrong_segment_admission.reserved_bytes() == 0u);

        auto wrong_identity_view = direct_only_compact.view;
        ++wrong_identity_view.header.identity.generation_id;
        bioseq::parallelism::memory_admission_controller wrong_identity_admission;
        const auto wrong_identity_reader = prepared_minimizer_fragment_reader::open_compact_direct(
            wrong_identity_admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(build_cap),
            runtime_max_occ,
            own_fragment_compact_direct_test_index(direct_only_compact, wrong_identity_view));
        REQUIRE_FALSE(wrong_identity_reader.has_value());
        REQUIRE(wrong_identity_reader.error() ==
                minimizer_generation_encoding_error::storage_contract_mismatch);
        REQUIRE(wrong_identity_admission.reserved_bytes() == 0u);

        // A protected decoy represents the complete grouped-SoA payload that a
        // minimap_manifest reader would own.  It is protected before open and is never
        // passed to the direct-only API, so any accidental payload dependency
        // would fault instead of being hidden by a warm test allocation.
        auto forbidden_grouped_soa_payload =
            make_sparse_mapped_test_payload(generation->manifest.payload_bytes);
        REQUIRE(forbidden_grouped_soa_payload.has_value());
        REQUIRE(mprotect(forbidden_grouped_soa_payload->owner.get(),
                         forbidden_grouped_soa_payload->size,
                         PROT_NONE) == 0);

        const std::uint8_t direct_only_posting_width =
            direct_only_compact.view.header.posting_width_bytes;
        bioseq::parallelism::memory_admission_controller direct_admission;
        // One byte cannot admit even one fragment_block_plan. Direct open must
        // therefore succeed only by retaining no scan-route address plan.
        direct_admission.set_index_memory_cap_bytes(1u);
        auto direct_reader = prepared_minimizer_fragment_reader::open_compact_direct(
            direct_admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(build_cap),
            runtime_max_occ,
            own_fragment_compact_direct_test_index(direct_only_compact),
            selected_page_recorder.token());
        REQUIRE(direct_reader.has_value());
        REQUIRE(selected_page_recorder.record_count.load() == 0u);
        REQUIRE(direct_reader->ready());
        REQUIRE(direct_reader->compact_direct_ready());
        REQUIRE(direct_reader->compact_direct_only());
        REQUIRE(direct_reader->mapped_grouped_soa_payload_bytes() == 0u);
        REQUIRE(direct_reader->grouped_soa_directory_entry_count() == 0u);
        REQUIRE(direct_reader->fragment_block_count() ==
                generation->manifest.position_segment_count);
        REQUIRE(direct_reader->largest_fragment_block_entry_count() == 0u);
        REQUIRE(direct_reader->oversized_segment_block_count() == 0u);
        const auto direct_memory = direct_reader->memory_stats();
        REQUIRE(direct_memory.directory_capacity_bytes == 0u);
        REQUIRE(direct_memory.global_position_capacity_bytes == 0u);
        REQUIRE(direct_memory.control_payload_capacity_bytes == 0u);
        REQUIRE(direct_memory.ordinal_route_capacity_bytes == 0u);
        REQUIRE(direct_memory.owned_capacity_bytes == 0u);
        REQUIRE(direct_memory.retained_layout_equation_holds());
        REQUIRE(direct_reader->admitted_bytes() == 0u);
        REQUIRE(direct_admission.reserved_bytes() == 0u);
        direct_admission.set_index_memory_cap_bytes(0u);

        // Leave the reader as the only lifetime authority for its direct
        // image.  The unrelated PROT_NONE grouped-SoA mapping remains live.
        const std::uintptr_t direct_image_begin =
            reinterpret_cast<std::uintptr_t>(direct_only_compact.view.image.data());
        const std::uint64_t direct_postings_offset =
            direct_only_compact.view.header.postings_offset;
        direct_only_compact.image.reset();
        direct_only_compact.view = {};
        auto direct_scratch = query_wave_scratch::create(direct_admission);
        REQUIRE(direct_scratch.has_value());
        REQUIRE_THROWS_AS(direct_reader->prepare_fragment_anchor_batch(
                              direct_admission,
                              queries,
                              runtime_max_occ,
                              *direct_scratch,
                              /*baseline_query_plan_matcher_for_testing=*/false,
                              /*verify_compositional_order_for_testing=*/false,
                              minimizer_fragment_lookup_route::grouped_soa_control_scans),
                          persisted_minimizer_lookup_error);
        REQUIRE_FALSE(direct_scratch->wave_outstanding());

        std::array<std::vector<bioseq::chaining::anchor>, 2u> direct_expected;
        direct_expected[0u] = ordinary_seed_anchors(
            pairs, target_length, query0, query_length, runtime_max_occ, kmer_size);
        direct_expected[1u] = ordinary_seed_anchors(
            pairs, target_length, query1, query_length, runtime_max_occ, kmer_size);
        auto direct_batch = direct_reader->prepare_fragment_anchor_batch(
            direct_admission,
            queries,
            runtime_max_occ,
            *direct_scratch,
            /*baseline_query_plan_matcher_for_testing=*/false,
            /*verify_compositional_order_for_testing=*/true,
            minimizer_fragment_lookup_route::direct_compact_hash_lookup);
        const std::array<std::uint32_t, 5u> exact_unique_hashes{7u, 8u, 9u, 10u, 11u};
        const compact_direct_query_set_stats expected_query_set_stats{
            .query_count = queries.size(),
            .query_occurrence_count = query0.size() + query1.size(),
            .unique_hash_count = exact_unique_hashes.size(),
            .group_key_set_checksum =
                compact_direct_query_set_checksum(exact_unique_hashes,
                                                  static_cast<std::uint32_t>(kmer_size),
                                                  10u,
                                                  static_cast<std::uint32_t>(runtime_max_occ)),
            .present = true,
        };
        REQUIRE(direct_batch.compact_direct_query_stats() == expected_query_set_stats);
        auto moved_direct_batch = std::move(direct_batch);
        REQUIRE_FALSE(direct_batch.compact_direct_query_stats().present);
        REQUIRE(moved_direct_batch.compact_direct_query_stats() == expected_query_set_stats);
        require_anchor_bytes_equal(moved_direct_batch.query_view(0u).ordered_fragment_seed_anchors(
                                       query0, query_length, runtime_max_occ, kmer_size),
                                   direct_expected[0u]);
        require_anchor_bytes_equal(moved_direct_batch.query_view(1u).ordered_fragment_seed_anchors(
                                       query1, query_length, runtime_max_occ, kmer_size),
                                   direct_expected[1u]);
        const auto& direct_evidence = moved_direct_batch.stats();
        REQUIRE(direct_evidence.compact_direct_waves == 1u);
        REQUIRE(direct_evidence.compact_direct_selection_shards == 2u);
        REQUIRE(direct_evidence.compact_direct_shard_query_layouts == 4u);
        REQUIRE(direct_evidence.compact_direct_layout_shards_completed == 2u);
        REQUIRE(direct_evidence.compact_direct_emission_shards_completed == 2u);
        REQUIRE(direct_evidence.compact_direct_posting_visit_passes == 1u);
        REQUIRE(direct_evidence.compact_direct_layout_selection_callbacks == 2u);
        REQUIRE(direct_evidence.compact_direct_emission_selection_callbacks == 2u);
        REQUIRE(direct_evidence.compact_direct_layout_spans_exposed == 4u);
        REQUIRE(direct_evidence.compact_direct_emission_spans_exposed == 4u);
        REQUIRE(direct_evidence.compact_direct_layout_descriptors_examined == 2u);
        REQUIRE(direct_evidence.compact_direct_emission_descriptors_examined == 2u);
        REQUIRE(direct_evidence.compact_direct_traversal_rank_checkpoints_examined == 0u);
        REQUIRE(direct_evidence.compact_direct_traversal_membership_words_examined == 0u);
        REQUIRE(direct_evidence.compact_direct_rejected_posting_reads == 0u);
        REQUIRE(direct_evidence.compact_direct_target_control_scan_bytes == 0u);
        REQUIRE(direct_evidence.compact_direct_target_control_scan_passes == 0u);
        REQUIRE(direct_evidence.compact_direct_fallbacks == 0u);
        REQUIRE(direct_evidence.control_count_pass_ns == 0u);
        REQUIRE(direct_evidence.control_count_bytes == 0u);
        REQUIRE(direct_evidence.pass2_control_bytes == 0u);
        REQUIRE(direct_evidence.pass3_control_bytes == 0u);
        REQUIRE(direct_evidence.pass2_accepted_prefix_bytes == 0u);
        REQUIRE(direct_evidence.pass3_accepted_prefix_bytes == 0u);
        REQUIRE(direct_evidence.pass2_position_bytes == 0u);
        REQUIRE(direct_evidence.pass3_position_bytes == 4u * direct_only_posting_width);
        REQUIRE(direct_evidence.anchors_constructed == 8u);
        REQUIRE(direct_evidence.forbidden_buffers_are_zero());
        REQUIRE_FALSE(selected_page_recorder.overflow.load());
        REQUIRE(selected_page_recorder.record_count.load() == 2u);
        std::array<compact_direct_selected_page_range, 2u> advised_ranges{
            selected_page_recorder.records[0u],
            selected_page_recorder.records[1u],
        };
        std::ranges::sort(advised_ranges, {}, &compact_direct_selected_page_range::begin);
        REQUIRE(advised_ranges[0u] == compact_direct_selected_page_range{
                                          .begin = direct_image_begin + direct_postings_offset,
                                          .end = direct_image_begin + direct_postings_offset + 8u,
                                      });
        REQUIRE(advised_ranges[1u] ==
                compact_direct_selected_page_range{
                    .begin = direct_image_begin + direct_postings_offset + 20u,
                    .end = direct_image_begin + direct_postings_offset + 28u,
                });
        const auto direct_memory_after_query = direct_reader->memory_stats();
        REQUIRE(direct_memory_after_query.directory_capacity_bytes == 0u);
        REQUIRE(direct_memory_after_query.ordinal_route_capacity_bytes == 0u);
        REQUIRE(direct_memory_after_query.owned_capacity_bytes == 0u);
        REQUIRE(direct_memory_after_query.retained_layout_equation_holds());
        REQUIRE(direct_reader->admitted_bytes() == 0u);
        direct_scratch->complete_wave_after_join();

        const std::array<fragment_seed_query, 0u> empty_queries{};
        auto empty_direct_batch = direct_reader->prepare_fragment_anchor_batch(
            direct_admission,
            empty_queries,
            runtime_max_occ,
            *direct_scratch,
            /*baseline_query_plan_matcher_for_testing=*/false,
            /*verify_compositional_order_for_testing=*/false,
            minimizer_fragment_lookup_route::direct_compact_hash_lookup);
        const compact_direct_query_set_stats expected_empty_stats{
            .query_count = 0u,
            .query_occurrence_count = 0u,
            .unique_hash_count = 0u,
            .group_key_set_checksum = compact_direct_query_set_checksum_prefix(
                static_cast<std::uint32_t>(kmer_size),
                10u,
                static_cast<std::uint32_t>(runtime_max_occ)),
            .present = true,
        };
        REQUIRE(empty_direct_batch.compact_direct_query_stats() == expected_empty_stats);
        REQUIRE(selected_page_recorder.record_count.load() == 2u);
        direct_scratch->complete_wave_after_join();
    }

    {
        bioseq::parallelism::memory_admission_controller zero_cap_admission;
        auto zero_cap_reader =
            open_grouped_soa_fragment_query_reader(zero_cap_admission,
                                                   *generation,
                                                   sink.rows,
                                                   0u,
                                                   own_fragment_compact_direct_test_index(compact));
        REQUIRE(zero_cap_reader.has_value());
        auto zero_cap_scratch = query_wave_scratch::create(zero_cap_admission);
        REQUIRE(zero_cap_scratch.has_value());
        auto zero_cap_batch = zero_cap_reader->prepare_fragment_anchor_batch(
            zero_cap_admission,
            queries,
            0,
            *zero_cap_scratch,
            /*baseline_query_plan_matcher_for_testing=*/false,
            /*verify_compositional_order_for_testing=*/true,
            minimizer_fragment_lookup_route::direct_compact_hash_lookup);
        REQUIRE_FALSE(zero_cap_batch.compact_direct_query_stats().present);
        for (std::size_t query_ordinal = 0u; query_ordinal < queries.size(); ++query_ordinal) {
            REQUIRE(zero_cap_batch.query_view(query_ordinal)
                        .ordered_fragment_seed_anchors(queries[query_ordinal].minimizers,
                                                       queries[query_ordinal].query_length,
                                                       0,
                                                       queries[query_ordinal].kmer_size)
                        .empty());
        }
        const auto& zero_evidence = zero_cap_batch.stats();
        REQUIRE(zero_evidence.compact_direct_waves == 1u);
        REQUIRE(zero_evidence.compact_direct_unique_query_groups == 0u);
        REQUIRE(zero_evidence.compact_direct_unique_lookups == 0u);
        REQUIRE(zero_evidence.compact_direct_selected_hashes == 0u);
        REQUIRE(zero_evidence.compact_direct_selected_postings == 0u);
        REQUIRE(zero_evidence.compact_direct_selection_shards == 0u);
        REQUIRE(zero_evidence.compact_direct_shard_query_layouts == 0u);
        REQUIRE(zero_evidence.compact_direct_layout_shards_completed == 0u);
        REQUIRE(zero_evidence.compact_direct_emission_shards_completed == 0u);
        REQUIRE(zero_evidence.compact_direct_posting_visit_passes == 0u);
        REQUIRE(zero_evidence.compact_direct_plan_bytes == 0u);
        REQUIRE(zero_evidence.query_plan_allocations == 0u);
        REQUIRE(zero_evidence.peak_query_plan_capacity_bytes == 0u);
        REQUIRE(zero_evidence.compact_direct_lookup_ns == 0u);
        REQUIRE(zero_evidence.control_count_pass_ns == 0u);
        REQUIRE(zero_evidence.control_count_bytes == 0u);
        REQUIRE(zero_evidence.pass2_position_bytes == 0u);
        REQUIRE(zero_evidence.pass3_position_bytes == 0u);
        REQUIRE(zero_evidence.anchors_constructed == 0u);
        REQUIRE(zero_evidence.exact_final_bytes == 0u);
        const auto zero_snapshot = zero_cap_scratch->occurrence_capacity_snapshot();
        REQUIRE(zero_snapshot.packed_size == 0u);
        REQUIRE(zero_snapshot.wide_size == 0u);
        REQUIRE(zero_snapshot.query_plan_size == 0u);
        zero_cap_scratch->complete_wave_after_join();
    }

    const std::uint8_t direct_posting_width = compact.view.header.posting_width_bytes;
    auto grouped_soa_payload = make_grouped_soa_mapped_payload(*generation, sink.rows);
    auto protected_payload = make_sparse_mapped_test_payload(grouped_soa_payload->size());
    REQUIRE(protected_payload.has_value());
    std::copy(grouped_soa_payload->begin(),
              grouped_soa_payload->end(),
              protected_payload->bytes().begin());
    const auto protected_payload_bytes = std::span<const std::byte>{protected_payload->bytes()};
    void* const protected_payload_address = protected_payload->owner.get();
    const std::size_t protected_payload_size = protected_payload->size;

    bioseq::parallelism::memory_admission_controller admission;
    std::shared_ptr<const void> protected_payload_owner = protected_payload->owner;
    auto reader = prepared_minimizer_fragment_reader::open(
        admission,
        generation->head,
        generation->manifest,
        test_grouped_soa_expected_storage_contract(build_cap),
        generation->directory.encoded_bytes(),
        runtime_max_occ,
        std::move(protected_payload_owner),
        protected_payload_bytes,
        own_fragment_compact_direct_test_index(compact));
    REQUIRE(reader.has_value());
    REQUIRE(reader->compact_direct_ready());
    // The reader now owns both mappings.  Drop every external lifetime token,
    // then make the grouped-SoA payload inaccessible: the explicit direct route
    // must remain valid and cannot silently scan controls or positions.
    REQUIRE(mprotect(protected_payload_address, protected_payload_size, PROT_NONE) == 0);
    protected_payload->owner.reset();
    grouped_soa_payload.reset();
    compact.image.reset();
    compact.view = {};
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());

    std::array<std::vector<bioseq::chaining::anchor>, 2u> expected;
    expected[0u] = ordinary_seed_anchors(
        pairs, target_length, query0, query_length, runtime_max_occ, kmer_size);
    expected[1u] = ordinary_seed_anchors(
        pairs, target_length, query1, query_length, runtime_max_occ, kmer_size);
    auto batch = reader->prepare_fragment_anchor_batch(
        admission,
        queries,
        runtime_max_occ,
        *scratch,
        /*baseline_query_plan_matcher_for_testing=*/false,
        /*verify_compositional_order_for_testing=*/true,
        minimizer_fragment_lookup_route::direct_compact_hash_lookup);
    require_anchor_bytes_equal(batch.query_view(0u).ordered_fragment_seed_anchors(
                                   query0, query_length, runtime_max_occ, kmer_size),
                               expected[0u]);
    require_anchor_bytes_equal(batch.query_view(1u).ordered_fragment_seed_anchors(
                                   query1, query_length, runtime_max_occ, kmer_size),
                               expected[1u]);
    REQUIRE(expected[0u].size() == 4u);
    REQUIRE(expected[1u].size() == 4u);
    const auto& evidence = batch.stats();
    REQUIRE(evidence.compact_direct_waves == 1u);
    REQUIRE(evidence.compact_direct_unique_query_groups == 5u);
    REQUIRE(evidence.compact_direct_unique_lookups == 5u);
    REQUIRE(evidence.compact_direct_selected_hashes == 2u);
    REQUIRE(evidence.compact_direct_runtime_rejected_hashes == 1u);
    REQUIRE(evidence.compact_direct_runtime_rejected_postings == 3u);
    REQUIRE(evidence.compact_direct_selected_postings == 4u);
    REQUIRE(evidence.compact_direct_selection_shards == 2u);
    REQUIRE(evidence.compact_direct_shard_query_layouts == 4u);
    REQUIRE(evidence.compact_direct_layout_shards_completed == 2u);
    REQUIRE(evidence.compact_direct_emission_shards_completed == 2u);
    REQUIRE(evidence.compact_direct_posting_visit_passes == 1u);
    REQUIRE(evidence.compact_direct_layout_postings_visited == 0u);
    REQUIRE(evidence.compact_direct_emission_postings_visited == 4u);
    REQUIRE(evidence.compact_direct_layout_selection_callbacks == 2u);
    REQUIRE(evidence.compact_direct_emission_selection_callbacks == 2u);
    REQUIRE(evidence.compact_direct_layout_spans_exposed == 4u);
    REQUIRE(evidence.compact_direct_emission_spans_exposed == 4u);
    REQUIRE(evidence.compact_direct_layout_descriptors_examined == 2u);
    REQUIRE(evidence.compact_direct_emission_descriptors_examined == 2u);
    REQUIRE(evidence.compact_direct_traversal_rank_checkpoints_examined == 0u);
    REQUIRE(evidence.compact_direct_traversal_membership_words_examined == 0u);
    REQUIRE(evidence.compact_direct_rejected_posting_reads == 0u);
    REQUIRE(evidence.compact_direct_target_control_scan_bytes == 0u);
    REQUIRE(evidence.compact_direct_target_control_scan_passes == 0u);
    REQUIRE(evidence.compact_direct_fallbacks == 0u);
    REQUIRE(evidence.compact_direct_plan_bytes == 2u * sizeof(compact_hash32_unique_selection));
    REQUIRE(evidence.compact_direct_plan_reserve_calls == 1u);
    REQUIRE(evidence.compact_direct_plan_reserve_requested_elements == 5u);
    REQUIRE(evidence.compact_direct_plan_initial_capacity_elements == 5u);
    REQUIRE(evidence.compact_direct_plan_fill_reallocations == 0u);
    REQUIRE(evidence.compact_direct_plan_compaction_allocations == 1u);
    REQUIRE(evidence.compact_direct_plan_storage_allocations == 2u);
    REQUIRE(evidence.compact_direct_plan_retained_capacity_elements == 2u);
    REQUIRE(evidence.compact_direct_plan_retained_capacity_bytes ==
            evidence.compact_direct_plan_bytes);
    REQUIRE(evidence.peak_compact_direct_plan_capacity_bytes ==
            7u * sizeof(compact_hash32_unique_selection));
    REQUIRE(evidence.compact_direct_lookup_ns > 0u);
    REQUIRE(evidence.control_count_pass_ns == 0u);
    REQUIRE(evidence.control_count_bytes == 0u);
    REQUIRE(evidence.pass2_control_bytes == 0u);
    REQUIRE(evidence.pass3_control_bytes == 0u);
    REQUIRE(evidence.pass2_accepted_prefix_bytes == 0u);
    REQUIRE(evidence.pass3_accepted_prefix_bytes == 0u);
    REQUIRE(evidence.pass2_position_bytes == 0u);
    REQUIRE(evidence.pass3_position_bytes == 4u * direct_posting_width);
    REQUIRE(evidence.anchors_constructed == 8u);
    REQUIRE(evidence.exact_final_bytes == 8u * sizeof(bioseq::chaining::anchor));
    REQUIRE(evidence.forbidden_buffers_are_zero());
    const auto snapshot = scratch->occurrence_capacity_snapshot();
    REQUIRE(snapshot.route == query_occurrence_route::packed);
    REQUIRE(snapshot.query_plan_size == 5u);
    REQUIRE(evidence.peak_query_plan_capacity_bytes ==
            static_cast<std::uint64_t>(snapshot.query_plan_capacity) * 16u);
    REQUIRE(snapshot.query_membership_filter_capacity_bytes == 0u);
    REQUIRE(snapshot.query_group_offset_capacity == 0u);
    REQUIRE_FALSE(snapshot.bucket_direct_matcher_active);
    scratch->complete_wave_after_join();

    const auto retained_before_plan_boundary = admission.reserved_bytes();
    const auto capacities_before_plan_boundary = scratch->occurrence_capacity_snapshot();
    constexpr std::uint64_t admitted_unique_capacity = 4u;
    const std::uint64_t admitted_plan_peak =
        2u * admitted_unique_capacity * sizeof(compact_hash32_unique_selection);
    const std::uint64_t next_plan_metadata = queries.size() * sizeof(prepared_query_anchors);
    admission.set_index_memory_cap_bytes(retained_before_plan_boundary + next_plan_metadata +
                                         admitted_plan_peak);
    // This query has c+1=5 unique hashes. Its exact preallocation peak is
    // rejected before the vector reserve, and all wave owners return reusable.
    REQUIRE_THROWS_AS(reader->prepare_fragment_anchor_batch(
                          admission,
                          queries,
                          runtime_max_occ,
                          *scratch,
                          /*baseline_query_plan_matcher_for_testing=*/false,
                          /*verify_compositional_order_for_testing=*/false,
                          minimizer_fragment_lookup_route::direct_compact_hash_lookup),
                      persisted_minimizer_lookup_error);
    REQUIRE_FALSE(scratch->wave_outstanding());
    REQUIRE(admission.reserved_bytes() == retained_before_plan_boundary);
    const auto capacities_after_plan_boundary = scratch->occurrence_capacity_snapshot();
    REQUIRE(capacities_after_plan_boundary.route == capacities_before_plan_boundary.route);
    REQUIRE(capacities_after_plan_boundary.packed_size ==
            capacities_before_plan_boundary.packed_size);
    REQUIRE(capacities_after_plan_boundary.packed_capacity ==
            capacities_before_plan_boundary.packed_capacity);
    REQUIRE(capacities_after_plan_boundary.wide_size == capacities_before_plan_boundary.wide_size);
    REQUIRE(capacities_after_plan_boundary.wide_capacity ==
            capacities_before_plan_boundary.wide_capacity);
    REQUIRE(capacities_after_plan_boundary.query_plan_size ==
            capacities_before_plan_boundary.query_plan_size);
    REQUIRE(capacities_after_plan_boundary.query_plan_capacity ==
            capacities_before_plan_boundary.query_plan_capacity);
    REQUIRE(capacities_after_plan_boundary.query_plan_reserve_request ==
            capacities_before_plan_boundary.query_plan_reserve_request);
    REQUIRE(capacities_after_plan_boundary.query_group_offset_size ==
            capacities_before_plan_boundary.query_group_offset_size);
    REQUIRE(capacities_after_plan_boundary.query_group_offset_capacity ==
            capacities_before_plan_boundary.query_group_offset_capacity);
    REQUIRE(capacities_after_plan_boundary.aggregate_occurrence_capacity_bytes ==
            capacities_before_plan_boundary.aggregate_occurrence_capacity_bytes);
    REQUIRE(capacities_after_plan_boundary.query_membership_filter_capacity_bytes ==
            capacities_before_plan_boundary.query_membership_filter_capacity_bytes);
    REQUIRE(capacities_after_plan_boundary.occurrence_radix_invoked ==
            capacities_before_plan_boundary.occurrence_radix_invoked);
    REQUIRE(capacities_after_plan_boundary.bucket_direct_matcher_active ==
            capacities_before_plan_boundary.bucket_direct_matcher_active);

    admission.set_index_memory_cap_bytes((std::numeric_limits<std::uint64_t>::max)());
    auto reused_batch = reader->prepare_fragment_anchor_batch(
        admission,
        queries,
        runtime_max_occ,
        *scratch,
        /*baseline_query_plan_matcher_for_testing=*/false,
        /*verify_compositional_order_for_testing=*/false,
        minimizer_fragment_lookup_route::direct_compact_hash_lookup);
    REQUIRE(reused_batch.stats().occurrence_allocations == 0u);
    REQUIRE(reused_batch.stats().query_plan_allocations == 0u);
    REQUIRE(reused_batch.stats().compact_direct_plan_fill_reallocations == 0u);
    REQUIRE(reused_batch.stats().compact_direct_plan_storage_allocations == 2u);
    const auto reused_snapshot = scratch->occurrence_capacity_snapshot();
    REQUIRE(reused_batch.stats().peak_query_plan_capacity_bytes ==
            static_cast<std::uint64_t>(reused_snapshot.query_plan_capacity) * 16u);
    scratch->complete_wave_after_join();

    REQUIRE_THROWS_AS(reader->prepare_fragment_anchor_batch(
                          admission,
                          queries,
                          runtime_max_occ,
                          *scratch,
                          /*baseline_query_plan_matcher_for_testing=*/true,
                          /*verify_compositional_order_for_testing=*/false,
                          minimizer_fragment_lookup_route::direct_compact_hash_lookup),
                      persisted_minimizer_lookup_error);
    REQUIRE_FALSE(scratch->wave_outstanding());
    scratch->release_retained_capacity();
    REQUIRE(scratch->admitted_bytes() == 0u);
}

TEST_CASE("compact-direct single-query occurrence emission covers P32 spans",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    constexpr auto packed = [](std::uint64_t bp, std::uint64_t strand) noexcept {
        return (bp << 1u) | strand;
    };
    constexpr std::uint32_t max_occ = 4u;
    constexpr std::uint64_t target_length = 400u;
    constexpr bioseq::storage::sequence_size_t query_length = 200u;
    constexpr std::int32_t kmer_size = 15;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        // Both P32 inline-singleton strand orientations.
        {7u, packed(10u, 0u)},
        {8u, packed(20u, 1u)},
        // One query occurrence expands an out-of-line two-strand list.
        {9u, packed(30u, 0u)},
        {9u, packed(31u, 0u)},
        {9u, packed(32u, 1u)},
        {9u, packed(33u, 1u)},
        // Duplicate query occurrences retain the generic fallback.
        {10u, packed(40u, 0u)},
        {10u, packed(41u, 1u)},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink, test_grouped_soa_generation_options(target_length, max_occ));
    REQUIRE(generation.has_value());
    auto compact = make_fragment_compact_direct_test_index(
        generation->head, generation->manifest, pairs, max_occ);
    REQUIRE(compact.view.header.posting_width_bytes == 4u);
    REQUIRE(compact.view.header.inline_singleton_count == 2u);
    REQUIRE(compact.view.header.materialized_posting_count == 6u);

    const std::vector<minimap_sketch_data> query{
        minimap_sketch_data{7u, 20u, 0u},
        minimap_sketch_data{8u, 30u, 1u},
        minimap_sketch_data{9u, 40u, 0u},
        minimap_sketch_data{10u, 50u, 0u},
        minimap_sketch_data{10u, 70u, 1u},
    };
    const std::array queries{
        fragment_seed_query{
            .minimizers = query,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };
    const auto expected =
        ordinary_seed_anchors(pairs, target_length, query, query_length, max_occ, kmer_size);

    bioseq::parallelism::memory_admission_controller admission;
    auto reader = prepared_minimizer_fragment_reader::open_compact_direct(
        admission,
        generation->head,
        generation->manifest,
        test_grouped_soa_expected_storage_contract(max_occ),
        max_occ,
        own_fragment_compact_direct_test_index(compact));
    REQUIRE(reader.has_value());
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());
    auto batch = reader->prepare_fragment_anchor_batch(
        admission,
        queries,
        max_occ,
        *scratch,
        /*baseline_query_plan_matcher_for_testing=*/false,
        /*verify_compositional_order_for_testing=*/true,
        minimizer_fragment_lookup_route::direct_compact_hash_lookup);
    const auto actual =
        batch.query_view(0u).ordered_fragment_seed_anchors(query, query_length, max_occ, kmer_size);
    require_anchor_bytes_equal(actual, expected);
    REQUIRE(actual.size() == 10u);

    const auto& evidence = batch.stats();
    REQUIRE(evidence.compact_direct_selected_hashes == 4u);
    REQUIRE(evidence.compact_direct_selected_postings == 8u);
    REQUIRE(evidence.compact_direct_posting_visit_passes == 1u);
    REQUIRE(evidence.compact_direct_layout_postings_visited == 0u);
    REQUIRE(evidence.compact_direct_emission_postings_visited == 8u);
    REQUIRE(evidence.compact_direct_layout_spans_exposed == 6u);
    REQUIRE(evidence.compact_direct_emission_spans_exposed == 6u);
    REQUIRE(evidence.pass2_position_bytes == 0u);
    REQUIRE(evidence.pass3_position_bytes == 8u * 4u);
    REQUIRE(evidence.anchors_constructed == actual.size());
    REQUIRE(evidence.exact_final_bytes == actual.size_bytes());
    REQUIRE(evidence.forbidden_buffers_are_zero());
    scratch->complete_wave_after_join();
}

TEST_CASE("compact-direct single-query occurrence emission covers P64 spans",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    constexpr auto packed = [](std::uint64_t bp, std::uint64_t strand) noexcept {
        return (bp << 1u) | strand;
    };
    constexpr std::uint32_t max_occ = 4u;
    constexpr std::uint64_t high_bp = (std::uint64_t{1u} << 31u) + 10u;
    constexpr std::uint64_t target_length = high_bp + 1'000u;
    constexpr bioseq::storage::sequence_size_t query_length = 200u;
    constexpr std::int32_t kmer_size = 15;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        // P64 never inlines, including a count-one posting list.
        {7u, packed(high_bp, 0u)},
        // One occurrence expands a multi-position, two-strand P64 list.
        {8u, packed(100u, 0u)},
        {8u, packed(101u, 0u)},
        {8u, packed(high_bp + 1u, 1u)},
        {8u, packed(high_bp + 2u, 1u)},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink, test_grouped_soa_generation_options(target_length, max_occ));
    REQUIRE(generation.has_value());
    auto compact = make_fragment_compact_direct_test_index(
        generation->head, generation->manifest, pairs, max_occ);
    REQUIRE(compact.view.header.posting_width_bytes == 8u);
    REQUIRE(compact.view.header.inline_singleton_count == 0u);
    REQUIRE(compact.view.header.materialized_posting_count == pairs.size());

    const std::vector<minimap_sketch_data> query{
        minimap_sketch_data{7u, 20u, 0u},
        minimap_sketch_data{8u, 40u, 1u},
    };
    const std::array queries{
        fragment_seed_query{
            .minimizers = query,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };
    const auto expected =
        ordinary_seed_anchors(pairs, target_length, query, query_length, max_occ, kmer_size);

    bioseq::parallelism::memory_admission_controller admission;
    auto reader = prepared_minimizer_fragment_reader::open_compact_direct(
        admission,
        generation->head,
        generation->manifest,
        test_grouped_soa_expected_storage_contract(max_occ),
        max_occ,
        own_fragment_compact_direct_test_index(compact));
    REQUIRE(reader.has_value());
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());
    auto batch = reader->prepare_fragment_anchor_batch(
        admission,
        queries,
        max_occ,
        *scratch,
        /*baseline_query_plan_matcher_for_testing=*/false,
        /*verify_compositional_order_for_testing=*/true,
        minimizer_fragment_lookup_route::direct_compact_hash_lookup);
    const auto actual =
        batch.query_view(0u).ordered_fragment_seed_anchors(query, query_length, max_occ, kmer_size);
    require_anchor_bytes_equal(actual, expected);
    REQUIRE(actual.size() == pairs.size());

    const auto& evidence = batch.stats();
    REQUIRE(evidence.compact_direct_selected_hashes == 2u);
    REQUIRE(evidence.compact_direct_selected_postings == pairs.size());
    REQUIRE(evidence.compact_direct_posting_visit_passes == 1u);
    REQUIRE(evidence.compact_direct_layout_postings_visited == 0u);
    REQUIRE(evidence.compact_direct_emission_postings_visited == pairs.size());
    REQUIRE(evidence.compact_direct_layout_spans_exposed == 3u);
    REQUIRE(evidence.compact_direct_emission_spans_exposed == 3u);
    REQUIRE(evidence.pass2_position_bytes == 0u);
    REQUIRE(evidence.pass3_position_bytes == pairs.size() * 8u);
    REQUIRE(evidence.anchors_constructed == actual.size());
    REQUIRE(evidence.exact_final_bytes == actual.size_bytes());
    REQUIRE(evidence.forbidden_buffers_are_zero());
    scratch->complete_wave_after_join();
}

TEST_CASE("compact-direct selection shards preserve P32 differential parity",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    constexpr auto packed = [](std::uint64_t bp, std::uint64_t strand) noexcept {
        return (bp << 1u) | strand;
    };
    constexpr std::uint32_t build_cap = 4u;
    constexpr std::int32_t runtime_max_occ = 2;
    constexpr std::uint64_t target_length = 10'000u;
    constexpr std::uint64_t selected_hash_count = 40u;
    constexpr std::uint64_t heavy_hash = 500u;
    constexpr bioseq::storage::sequence_size_t query_length = 300u;
    constexpr std::int32_t kmer_size = 15;

    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
    pairs.reserve(selected_hash_count + 3u);
    for (std::uint64_t hash = 1u; hash <= selected_hash_count; ++hash) {
        pairs.emplace_back(hash, packed(100u + 3u * hash, hash & 1u));
    }
    pairs.emplace_back(heavy_hash, packed(1'000u, 0u));
    pairs.emplace_back(heavy_hash, packed(1'001u, 0u));
    pairs.emplace_back(heavy_hash, packed(1'002u, 1u));

    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink, test_grouped_soa_generation_options(target_length, build_cap));
    REQUIRE(generation.has_value());
    auto compact = make_fragment_compact_direct_test_index(
        generation->head, generation->manifest, pairs, build_cap);
    REQUIRE(compact.view.header.posting_width_bytes == 4u);

    std::vector<minimap_sketch_data> query0;
    std::vector<minimap_sketch_data> query1;
    query0.reserve(selected_hash_count + 2u);
    query1.reserve(selected_hash_count + 1u);
    for (std::uint64_t hash = 1u; hash <= selected_hash_count; ++hash) {
        query0.push_back(minimap_sketch_data{
            hash,
            20u + hash,
            static_cast<std::uint8_t>((hash + 1u) & 1u),
        });
    }
    // Repeat one accepted occurrence byte-for-byte. It must expand through
    // its shard-local cursor without introducing an atomic or shared cursor.
    query0.push_back(query0[6u]);
    query0.push_back(minimap_sketch_data{heavy_hash, 90u, 0u});
    for (std::uint64_t hash = selected_hash_count; hash != 0u; --hash) {
        query1.push_back(minimap_sketch_data{
            hash,
            140u + hash,
            static_cast<std::uint8_t>(hash & 1u),
        });
    }
    query1.push_back(minimap_sketch_data{heavy_hash, 200u, 1u});
    const std::array queries{
        fragment_seed_query{
            .minimizers = query0,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
        fragment_seed_query{
            .minimizers = query1,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };

    bioseq::parallelism::memory_admission_controller admission;
    selected_page_advice_recorder selected_page_recorder;
    auto reader = prepared_minimizer_fragment_reader::open_compact_direct(
        admission,
        generation->head,
        generation->manifest,
        test_grouped_soa_expected_storage_contract(build_cap),
        runtime_max_occ,
        own_fragment_compact_direct_test_index(compact),
        selected_page_recorder.token());
    REQUIRE(reader.has_value());
    REQUIRE(selected_page_recorder.record_count.load() == 0u);
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());

    const auto expected0 = ordinary_seed_anchors(
        pairs, target_length, query0, query_length, runtime_max_occ, kmer_size);
    const auto expected1 = ordinary_seed_anchors(
        pairs, target_length, query1, query_length, runtime_max_occ, kmer_size);
    auto batch = reader->prepare_fragment_anchor_batch(
        admission,
        queries,
        runtime_max_occ,
        *scratch,
        /*baseline_query_plan_matcher_for_testing=*/false,
        /*verify_compositional_order_for_testing=*/true,
        minimizer_fragment_lookup_route::direct_compact_hash_lookup);
    const auto actual0 = batch.query_view(0u).ordered_fragment_seed_anchors(
        query0, query_length, runtime_max_occ, kmer_size);
    const auto actual1 = batch.query_view(1u).ordered_fragment_seed_anchors(
        query1, query_length, runtime_max_occ, kmer_size);
    require_anchor_bytes_equal(actual0, expected0);
    require_anchor_bytes_equal(actual1, expected1);
    REQUIRE(actual0.size() == selected_hash_count + 1u);
    REQUIRE(actual1.size() == selected_hash_count);

    const auto& evidence = batch.stats();
    REQUIRE(evidence.compact_direct_unique_query_groups == selected_hash_count + 1u);
    REQUIRE(evidence.compact_direct_selected_hashes == selected_hash_count);
    REQUIRE(evidence.compact_direct_runtime_rejected_hashes == 1u);
    REQUIRE(evidence.compact_direct_runtime_rejected_postings == 3u);
    REQUIRE(evidence.compact_direct_selected_postings == selected_hash_count);
    REQUIRE(evidence.compact_direct_selection_shards == 32u);
    REQUIRE(evidence.compact_direct_shard_query_layouts == 64u);
    REQUIRE(evidence.compact_direct_layout_shards_completed == 32u);
    REQUIRE(evidence.compact_direct_emission_shards_completed == 32u);
    REQUIRE(evidence.compact_direct_posting_visit_passes == 1u);
    REQUIRE(evidence.compact_direct_layout_postings_visited == 0u);
    REQUIRE(evidence.compact_direct_emission_postings_visited == selected_hash_count);
    REQUIRE(evidence.compact_direct_layout_selection_callbacks == selected_hash_count);
    REQUIRE(evidence.compact_direct_emission_selection_callbacks == selected_hash_count);
    REQUIRE(evidence.compact_direct_layout_descriptors_examined == selected_hash_count);
    REQUIRE(evidence.compact_direct_emission_descriptors_examined == selected_hash_count);
    REQUIRE(evidence.compact_direct_traversal_rank_checkpoints_examined == 0u);
    REQUIRE(evidence.compact_direct_traversal_membership_words_examined == 0u);
    REQUIRE(evidence.pass2_position_bytes == 0u);
    REQUIRE(evidence.pass3_position_bytes ==
            selected_hash_count * compact.view.header.posting_width_bytes);
    REQUIRE(evidence.anchors_constructed == actual0.size() + actual1.size());
    REQUIRE(evidence.forbidden_buffers_are_zero());
    REQUIRE_FALSE(selected_page_recorder.overflow.load());
    REQUIRE(selected_page_recorder.record_count.load() == 0u);
    scratch->complete_wave_after_join();
}

TEST_CASE("fragment anchor admission failure leaves reusable owners exact",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, 20u},
        {7u, 40u},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation =
        encode_minimizer_generation(producer, sink, test_grouped_soa_generation_options(100u, 10u));
    REQUIRE(generation.has_value());
    const std::vector<minimap_sketch_data> query{
        minimap_sketch_data{7u, 20u, 0u},
    };
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;
    const std::array queries{
        fragment_seed_query{std::span<const minimap_sketch_data>{query}, query_length, kmer_size},
    };

    bioseq::parallelism::memory_admission_controller admission;
    {
        auto reader = open_grouped_soa_fragment_query_reader(admission, *generation, sink.rows, 2u);
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        {
            auto warm = reader->prepare_fragment_anchor_batch(admission, queries, 2, *scratch);
            REQUIRE(warm.admitted_bytes() > 0u);
            REQUIRE_FALSE(warm.query_view(0u)
                              .ordered_fragment_seed_anchors(query, query_length, 2, kmer_size)
                              .empty());
            scratch->complete_wave_after_join();
        }
        const std::uint64_t retained_without_batch = admission.reserved_bytes();
        REQUIRE(retained_without_batch == reader->admitted_bytes() + scratch->admitted_bytes());
        REQUIRE(scratch->admitted_bytes() > 0u);
        admission.set_index_memory_cap_bytes(retained_without_batch +
                                             sizeof(prepared_query_anchors));

        REQUIRE_THROWS_AS(reader->prepare_fragment_anchor_batch(admission, queries, 2, *scratch),
                          persisted_minimizer_lookup_error);
        REQUIRE_FALSE(scratch->wave_outstanding());
        REQUIRE(admission.reserved_bytes() == retained_without_batch);
        scratch->release_retained_capacity();
        REQUIRE(scratch->admitted_bytes() == 0u);
        REQUIRE(admission.reserved_bytes() == reader->admitted_bytes());
    }
    REQUIRE(admission.reserved_bytes() == 0u);
}

TEST_CASE("fragment anchor preparation rejects trusted payload corruption",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, 20u},
        {7u, 40u},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation =
        encode_minimizer_generation(producer, sink, test_grouped_soa_generation_options(100u, 10u));
    REQUIRE(generation.has_value());
    REQUIRE(generation->directory.entries().size() == 1u);
    auto payload = make_grouped_soa_mapped_payload(*generation, sink.rows);
    const auto& entry = generation->directory.entries().front();
    const auto layout = plan_grouped_soa_control_plane(entry.control_element_count,
                                                       generation->directory.profile().hash_width,
                                                       entry.shape_offset_width,
                                                       generation->directory.profile().count_width);
    REQUIRE(layout.has_value());
    REQUIRE(generation->directory.profile().count_width == minimizer_scalar_width::u16);
    test_write_le<std::uint16_t>(std::span<std::byte>{payload->data(), payload->size()},
                                 static_cast<std::size_t>(layout->count_offset),
                                 3u);

    bioseq::parallelism::memory_admission_controller admission;
    {
        std::shared_ptr<const void> payload_owner = payload;
        auto reader = prepared_minimizer_fragment_reader::open(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(10u),
            generation->directory.encoded_bytes(),
            1u,
            std::move(payload_owner),
            std::span<const std::byte>{payload->data(), payload->size()});
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        const std::vector<minimap_sketch_data> query{
            minimap_sketch_data{7u, 20u, 0u},
        };
        const std::array queries{
            fragment_seed_query{std::span<const minimap_sketch_data>{query}, 100u, 15},
        };
        REQUIRE_THROWS_AS(reader->prepare_fragment_anchor_batch(admission, queries, 1, *scratch),
                          persisted_minimizer_lookup_error);
        REQUIRE_FALSE(scratch->wave_outstanding());
        scratch->release_retained_capacity();
        REQUIRE(admission.reserved_bytes() == reader->admitted_bytes());
    }
    REQUIRE(admission.reserved_bytes() == 0u);
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
