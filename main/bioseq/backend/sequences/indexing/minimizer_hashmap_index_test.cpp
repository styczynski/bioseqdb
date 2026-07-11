// backend/sequences/indexing/minimizer_hashmap_index_test.cpp
//
// DOC0279 §16.1 — MinimizerHashmapFragmentIndex (the storage-free pipeline IndexT)
// + the storage-bound MinimizersPerFragmentIndex<S> builder/assembler.
//
// This file reaches collect_minimizers_view / MinimapSketchData / minimap_int_hasher
// / sketch_collector, so its body lives inside bioseq::simd::HWY_NAMESPACE and
// reaches those symbols UNQUALIFIED — exactly like lazy_minimizer_parity_test.cpp.
// The bioseq::indexing::* types are named fully-qualified.

#include "common_simd.h"
#include "common_tests.h"
#include "common.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.sequences.kmers.mm_sketch_baseline;
import bioseq.storage.scanner;
import bioseq.storage.lazy;
import bioseq.storage.size;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.symbol_counts;
import bioseq.parallelism.memory_admission;
import bioseq.indexing.registry;

#include "index_test_fakes.h"

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {

namespace {

using bioseq::storage::sequence_scanner_test_chunked;
using bioseq::storage::lazy_compact_sequence;
using bioseq::indexing::test::FakeFragmentStorage;

using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;   // 32 letters / uint64 unit
using range_t  = compact_sequence_range<dna4_u64>;
using dna5_u64 = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
using dna5_range_t = compact_sequence_range<dna5_u64>;

static_assert(std::is_standard_layout_v<
    bioseq::indexing::ExactSimdIndexPackedFragment>);
static_assert(std::is_trivially_copyable_v<
    bioseq::indexing::ExactSimdIndexPackedFragment>);
static_assert(std::is_standard_layout_v<
    bioseq::indexing::ExactSimdIndexBridgeRequest>);
static_assert(std::is_trivially_copyable_v<
    bioseq::indexing::ExactSimdIndexBridgeRequest>);
static_assert(std::is_standard_layout_v<
    bioseq::indexing::ExactSimdIndexBridgeResult>);
static_assert(std::is_same_v<
    decltype(bioseq::indexing::collect_exact_simd_index_erased(
        std::declval<const bioseq::indexing::ExactSimdIndexBridgeRequest&>())),
    bioseq::indexing::ExactSimdIndexBridgeResult>);

[[nodiscard]] std::vector<seq::dna4> make_dna4(std::size_t n, std::uint64_t seed = 0x9e3779b97f4a7c15ULL) {
    std::vector<seq::dna4> v(n);
    std::uint64_t s = seed;
    for (std::size_t i = 0; i < n; ++i) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        seq::dna4 c;
        c.assign_rank(static_cast<decltype(c.to_rank())>((s >> 33) & 0x3));
        v[i] = c;
    }
    return v;
}

[[nodiscard]] std::vector<bioseq::indexing::CanonicalMinimizerEvent>
baseline_dna5_events(
        const std::vector<seq::dna5>& data,
        std::size_t window_size,
        std::size_t kmer_size) {
    std::string bases;
    bases.reserve(data.size());
    for (const seq::dna5 symbol : data) bases.push_back(symbol.to_char());
    const auto entries = ::bioseq::mm_sketch_baseline_ns::mm_sketch(
        bases.data(),
        static_cast<int>(bases.size()),
        static_cast<int>(window_size),
        static_cast<int>(kmer_size));
    std::vector<bioseq::indexing::CanonicalMinimizerEvent> events;
    events.reserve(entries.size());
    for (const auto& entry : entries) {
        events.push_back({
            .hash = entry.x >> 8,
            .encoded_position = entry.y & std::uint64_t{0xFFFFFFFFu},
        });
    }
    return events;
}

// (kmer -> sorted encoded positions) view of an index over an explicit kmer set.
[[nodiscard]] std::map<std::uint64_t, std::vector<std::uint64_t>>
lookup_map(const bioseq::indexing::MinimizerHashmapFragmentIndex& idx,
           const std::vector<std::uint64_t>& kmers) {
    std::map<std::uint64_t, std::vector<std::uint64_t>> out;
    for (std::uint64_t kmer : kmers) {
        if (out.count(kmer)) continue;
        auto span = idx.lookup(kmer);
        std::vector<std::uint64_t> v(span.begin(), span.end());
        std::sort(v.begin(), v.end());
        out.emplace(kmer, std::move(v));
    }
    return out;
}

[[nodiscard]] std::map<std::uint64_t, std::vector<std::uint64_t>>
canonical_event_lookup_map(
        std::span<const bioseq::indexing::CanonicalMinimizerEvent> events) {
    std::map<std::uint64_t, std::vector<std::uint64_t>> lookup;
    for (const auto& event : events) {
        lookup[event.hash].push_back(event.encoded_position);
    }
    for (auto& [hash, positions] : lookup) {
        (void)hash;
        std::ranges::sort(positions);
        const auto unique_end = std::ranges::unique(positions).begin();
        positions.erase(unique_end, positions.end());
    }
    return lookup;
}

// Whole-sequence minimizers, globalised the same way the builders do (collect
// over the entire range, take last_pos as the global position).
template <class Range>
[[nodiscard]] std::vector<MinimapSketchData>
whole_sequence_minimizers(Range& range, std::size_t w, std::size_t k) {
    sketch_collector collector;
    collect_minimizers_view<minimap_int_hasher, MinimapSketchData>(
        collector, range, range.begin(), range.end(), w, k);
    return collector.extract_sketches_data();
}

template <class UInt, std::size_t Size>
[[nodiscard]] constexpr UInt test_read_le(
        const std::array<std::byte, Size>& bytes,
        std::size_t offset) noexcept {
    static_assert(std::is_unsigned_v<UInt>);
    UInt value = 0;
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        value |= static_cast<UInt>(std::to_integer<std::uint8_t>(bytes[offset + i]))
            << (i * 8u);
    }
    return value;
}

template <class UInt>
[[nodiscard]] constexpr UInt test_read_le(
        std::span<const std::byte> bytes,
        std::size_t offset) noexcept {
    static_assert(std::is_unsigned_v<UInt>);
    UInt value = 0;
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        value |= static_cast<UInt>(std::to_integer<std::uint8_t>(bytes[offset + i]))
            << (i * 8u);
    }
    return value;
}

template <class UInt, std::size_t Size>
constexpr void test_write_le(
        std::array<std::byte, Size>& bytes,
        std::size_t offset,
        UInt value) noexcept {
    static_assert(std::is_unsigned_v<UInt>);
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        bytes[offset + i] = static_cast<std::byte>(value & UInt{0xff});
        value >>= 8u;
    }
}

template <class UInt>
constexpr void test_write_le(
        std::span<std::byte> bytes,
        std::size_t offset,
        UInt value) noexcept {
    static_assert(std::is_unsigned_v<UInt>);
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        bytes[offset + i] = static_cast<std::byte>(value & UInt{0xff});
        value >>= 8u;
    }
}

template <class T>
void require_wire_error(
        const std::expected<T, bioseq::indexing::MinimizerGenerationWireError>& value,
        bioseq::indexing::MinimizerGenerationWireError expected) {
    REQUIRE_FALSE(value.has_value());
    REQUIRE(value.error() == expected);
}

template <class T>
void require_wire_value(
        const std::expected<T, bioseq::indexing::MinimizerGenerationWireError>& value,
        const T& expected) {
    REQUIRE(value.has_value());
    REQUIRE(*value == expected);
}

[[nodiscard]] constexpr bioseq::indexing::MinimizerGenerationHead
golden_generation_head() noexcept {
    return {
        .generation_id = 0x0102030405060708ULL,
        .source_identity = 0x1112131415161718ULL,
        .compatibility_checksum = 0x2122232425262728ULL,
        .manifest_checksum = 0x3132333435363738ULL,
        .source_content_checksum = 0x4142434445464748ULL,
    };
}

[[nodiscard]] constexpr bioseq::indexing::MinimizerGenerationManifest
golden_generation_manifest() noexcept {
    return {
        .generation_id = 0x0102030405060708ULL,
        .source_identity = 0x1112131415161718ULL,
        .source_content_checksum = 0x2122232425262728ULL,
        .target_length = 20,
        .control_count = 0x3132333435363738ULL,
        .position_count = 0x4142434445464748ULL,
        .payload_bytes = 0x5152535455565758ULL,
        .payload_checksum = 0x6162636465666768ULL,
        .directory_bytes = 0x7172737475767778ULL,
        .directory_checksum = 0x090A0B0C0D0E0F10ULL,
        .position_segment_count = 3,
        .extent_count = 0x11223344U,
        .directory_chunk_count = 0x55667788U,
        .alphabet_code = 0x1122U,
        .storage_unit_byte_width = 0x3344U,
        .kmer_size = 0x5566U,
        .window_size = 0x7788U,
        .gpu_abi_version = 0x99AABBCCU,
        .position_segment_span_bp = 8,
        .segment_geometry_version =
            bioseq::indexing::MinimizerGenerationManifest::uniform_segment_geometry_version,
    };
}

[[nodiscard]] constexpr bioseq::indexing::MinimizerGenerationEncodeOptions
test_generation_options(std::uint64_t target_length) noexcept {
    return {
        .generation_id = 37,
        .source_identity = 0x1122334455667788ULL,
        .source_content_checksum = 0x8877665544332211ULL,
        .target_length = target_length,
        .alphabet_code = 4,
        .storage_unit_byte_width = 8,
        .kmer_size = 15,
        .window_size = 10,
        .minimizer_scheme_id =
            bioseq::indexing::kLegacyMinimapV1SchemeId,
        .minimizer_scheme_version =
            bioseq::indexing::kLegacyMinimapV1SchemeVersion,
    };
}

[[nodiscard]] constexpr bioseq::indexing::ExpectedMinimizerCompatibility
test_expected_compatibility() noexcept {
    return {
        .alphabet_code = 4,
        .storage_unit_byte_width = 8,
        .kmer_size = 15,
        .window_size = 10,
        .gpu_abi_version =
            bioseq::indexing::kMinimizerSortedDenseGpuAbiVersion,
        .minimizer_scheme_id =
            bioseq::indexing::kLegacyMinimapV1SchemeId,
        .minimizer_scheme_version =
            bioseq::indexing::kLegacyMinimapV1SchemeVersion,
    };
}

struct CollectedMinimizerRow {
    bioseq::indexing::IndexChunkAddress address{};
    std::vector<std::byte> bytes;
    std::uint64_t row_checksum = 0;
};

struct CollectingMinimizerRowSink {
    std::vector<CollectedMinimizerRow> rows;
    std::size_t max_callback_bytes = 0;

    [[nodiscard]] std::expected<void, bioseq::indexing::MinimizerGenerationWireError>
    write(const bioseq::indexing::MinimizerGenerationRowView& row) {
        max_callback_bytes = std::max(max_callback_bytes, row.bytes.size());
        rows.push_back(CollectedMinimizerRow{
            .address = row.address,
            .bytes = std::vector<std::byte>(row.bytes.begin(), row.bytes.end()),
            .row_checksum = row.row_checksum,
        });
        return {};
    }
};

struct VectorReplayMinimizerProducer {
    std::vector<bioseq::indexing::CanonicalMinimizerEvent> events;
    std::size_t scan_count = 0;
    bioseq::indexing::MinimizerSchemeIdentity scheme =
        bioseq::indexing::kLegacyMinimapV1SchemeIdentity;

    explicit VectorReplayMinimizerProducer(
            std::span<const std::pair<std::uint64_t, std::uint64_t>> pairs,
            bioseq::indexing::MinimizerSchemeIdentity scheme_identity =
                bioseq::indexing::kLegacyMinimapV1SchemeIdentity)
        : scheme(scheme_identity) {
        events.reserve(pairs.size());
        for (const auto& pair : pairs) {
            events.push_back({pair.first, pair.second});
        }
    }

    [[nodiscard]] bioseq::indexing::MinimizerSchemeIdentity
    minimizer_scheme_identity() const noexcept {
        return scheme;
    }

    [[nodiscard]] std::expected<void, bioseq::indexing::MinimizerGenerationWireError>
    scan_sorted_events(
            std::uint64_t span,
            bioseq::indexing::CanonicalMinimizerEventReceiver receiver) {
        ++scan_count;
        if (span == 0) {
            return std::unexpected(
                bioseq::indexing::MinimizerGenerationWireError::segment_span);
        }
        std::vector<bioseq::indexing::CanonicalMinimizerEvent> ordered = events;
        std::sort(ordered.begin(), ordered.end(),
            [span](const auto& lhs, const auto& rhs) {
                const std::uint64_t lhs_segment =
                    (lhs.encoded_position >> 1u) / span;
                const std::uint64_t rhs_segment =
                    (rhs.encoded_position >> 1u) / span;
                if (lhs_segment != rhs_segment) return lhs_segment < rhs_segment;
                if (lhs.hash != rhs.hash) return lhs.hash < rhs.hash;
                return lhs.encoded_position < rhs.encoded_position;
            });
        for (const auto& event : ordered) {
            if (auto accepted = receiver(event); !accepted) {
                return accepted;
            }
        }
        return {};
    }
};

struct DriftingReplayMinimizerProducer {
    std::size_t scan_count = 0;

    [[nodiscard]] bioseq::indexing::MinimizerSchemeIdentity
    minimizer_scheme_identity() const noexcept {
        return bioseq::indexing::kLegacyMinimapV1SchemeIdentity;
    }

    [[nodiscard]] std::expected<void, bioseq::indexing::MinimizerGenerationWireError>
    scan_sorted_events(
            std::uint64_t span,
            bioseq::indexing::CanonicalMinimizerEventReceiver receiver) {
        ++scan_count;
        if (span == 0) {
            return std::unexpected(
                bioseq::indexing::MinimizerGenerationWireError::segment_span);
        }
        const std::array events{
            bioseq::indexing::CanonicalMinimizerEvent{.hash = 1, .encoded_position = 2},
            bioseq::indexing::CanonicalMinimizerEvent{
                .hash = 2,
                .encoded_position = scan_count == 1 ? 4u : 6u,
            },
        };
        for (const auto& event : events) {
            if (auto accepted = receiver(event); !accepted) {
                return accepted;
            }
        }
        return {};
    }
};

[[nodiscard]] std::vector<std::byte> collect_encoded_extent(
        std::span<const CollectedMinimizerRow> rows,
        const bioseq::indexing::MinimizerBucketDirEntry& entry,
        bool controls) {
    const std::uint32_t first = controls
        ? entry.control_first_chunk : entry.position_first_chunk;
    const std::uint32_t count = controls
        ? entry.control_chunk_count : entry.position_chunk_count;
    std::vector<std::byte> bytes;
    for (std::uint32_t chunk = first; chunk < first + count; ++chunk) {
        const auto row = std::find_if(
            rows.begin(), rows.end(),
            [entry, chunk](const CollectedMinimizerRow& value) {
                return value.address.position_segment == entry.position_segment
                    && value.address.sub_key == entry.sub_key
                    && value.address.chunk_no == chunk;
            });
        REQUIRE(row != rows.end());
        bytes.insert(bytes.end(), row->bytes.begin(), row->bytes.end());
    }
    REQUIRE(bytes.size() == (controls
        ? entry.control_encoded_bytes : entry.position_encoded_bytes));
    return bytes;
}

void require_encoded_generations_equal_normalized(
        const bioseq::indexing::EncodedMinimizerGeneration& lhs,
        std::span<const CollectedMinimizerRow> lhs_rows,
        const bioseq::indexing::EncodedMinimizerGeneration& rhs,
        std::span<const CollectedMinimizerRow> rhs_rows) {
    auto lhs_manifest = lhs.manifest;
    auto rhs_manifest = rhs.manifest;
    lhs_manifest.generation_id = 1;
    rhs_manifest.generation_id = 1;
    lhs_manifest.payload_checksum = 0;
    rhs_manifest.payload_checksum = 0;
    REQUIRE(lhs_manifest == rhs_manifest);
    REQUIRE(lhs.head.source_identity == rhs.head.source_identity);
    REQUIRE(lhs.head.compatibility_checksum == rhs.head.compatibility_checksum);
    REQUIRE(lhs.head.source_content_checksum == rhs.head.source_content_checksum);
    REQUIRE(std::ranges::equal(
        lhs.directory.encoded_bytes(), rhs.directory.encoded_bytes()));
    REQUIRE(lhs_rows.size() == rhs_rows.size());
    REQUIRE_FALSE(lhs_rows.empty());
    for (std::size_t index = 0; index + 1u < lhs_rows.size(); ++index) {
        REQUIRE(lhs_rows[index].address.position_segment
            == rhs_rows[index].address.position_segment);
        REQUIRE(lhs_rows[index].address.sub_key
            == rhs_rows[index].address.sub_key);
        REQUIRE(lhs_rows[index].address.chunk_no
            == rhs_rows[index].address.chunk_no);
        REQUIRE(lhs_rows[index].bytes == rhs_rows[index].bytes);
    }
}

[[nodiscard]] bioseq::indexing::MinimizerGenerationRowView row_view(
        const CollectedMinimizerRow& row) noexcept {
    return {
        .address = row.address,
        .bytes = row.bytes,
        .row_checksum = row.row_checksum,
    };
}

[[nodiscard]] bioseq::indexing::MinimizerGenerationRowValidator
make_generation_row_validator(
        const bioseq::indexing::EncodedMinimizerGeneration& generation) {
    auto made = bioseq::indexing::MinimizerGenerationRowValidator::create(
        generation);
    REQUIRE(made.has_value());
    return std::move(*made);
}

void require_complete_generation_rows(
        const bioseq::indexing::EncodedMinimizerGeneration& generation,
        std::span<const CollectedMinimizerRow> rows) {
    auto validator = make_generation_row_validator(generation);
    require_wire_error(
        validator.validate_publication_head(
            bioseq::indexing::MinimizerGenerationRowView{
                .address = generation.unpublished_head_row.address,
                .bytes = generation.unpublished_head_row.bytes(),
                .row_checksum = generation.unpublished_head_row.row_checksum,
            }),
        bioseq::indexing::MinimizerGenerationWireError::row_count);
    for (const auto& row : rows) {
        REQUIRE(validator.accept(row_view(row)).has_value());
    }
    REQUIRE(validator.finish_ready().has_value());
    REQUIRE(validator.validate_publication_head(
        bioseq::indexing::MinimizerGenerationRowView{
            .address = generation.unpublished_head_row.address,
            .bytes = generation.unpublished_head_row.bytes(),
            .row_checksum = generation.unpublished_head_row.row_checksum,
        }).has_value());
}

[[nodiscard]] std::vector<bioseq::indexing::CanonicalMinimizerEvent>
decode_generation_events(
        const bioseq::indexing::EncodedMinimizerGeneration& generation,
        std::span<const CollectedMinimizerRow> rows) {
    std::vector<bioseq::indexing::CanonicalMinimizerEvent> events;
    for (const auto& entry : generation.directory.entries()) {
        const auto control_bytes = collect_encoded_extent(rows, entry, true);
        const auto position_bytes = collect_encoded_extent(rows, entry, false);
        for (std::size_t index = 0;
             index < entry.control_element_count;
             ++index) {
            const auto control =
                bioseq::indexing::decode_minimizer_serialized_bucket(
                    std::span<const std::byte>{control_bytes}.subspan(
                        index * 16u, 16u));
            REQUIRE(control.has_value());
            REQUIRE(control->pos_count > 0);
            for (std::uint32_t position = 0;
                 position < control->pos_count;
                 ++position) {
                events.push_back({
                    .hash = control->hash,
                    .encoded_position = test_read_le<std::uint64_t>(
                        position_bytes,
                        (static_cast<std::size_t>(control->pos_offset)
                            + position) * 8u),
                });
            }
        }
    }
    return events;
}

struct RebuiltGenerationMetadata {
    bioseq::indexing::MinimizerGenerationHead head{};
    bioseq::indexing::MinimizerGenerationManifest manifest{};
    std::vector<std::byte> directory_bytes;
};

[[nodiscard]] RebuiltGenerationMetadata rebuild_generation_metadata(
        const bioseq::indexing::EncodedMinimizerGeneration& original,
        std::span<const bioseq::indexing::MinimizerBucketDirEntry> entries) {
    using namespace bioseq::indexing;
    auto directory_bytes = encode_minimizer_bucket_directory(entries);
    REQUIRE(directory_bytes.has_value());
    auto manifest = original.manifest;
    manifest.directory_bytes = directory_bytes->size();
    manifest.directory_checksum = crc64_ecma182(*directory_bytes);
    const auto manifest_bytes = encode_minimizer_generation_manifest(manifest);
    REQUIRE(manifest_bytes.has_value());
    auto head = original.head;
    head.compatibility_checksum =
        minimizer_generation_compatibility_checksum(manifest);
    head.manifest_checksum = crc64_ecma182(
        std::span<const std::byte>{*manifest_bytes});
    return {
        .head = head,
        .manifest = manifest,
        .directory_bytes = std::move(*directory_bytes),
    };
}

template <std::size_t Size>
[[nodiscard]] bioseq::indexing::EncodedMinimizerFixedRow make_test_fixed_row(
        const bioseq::indexing::IndexChunkAddress& address,
        const std::array<std::byte, Size>& bytes) {
    bioseq::indexing::EncodedMinimizerFixedRow row{
        .address = address,
        .byte_count = static_cast<std::uint32_t>(bytes.size()),
    };
    std::copy(bytes.begin(), bytes.end(), row.storage.begin());
    row.row_checksum = bioseq::indexing::crc64_index_chunk_row(
        address.generation_id,
        address.position_segment,
        address.sub_key,
        address.chunk_no,
        row.byte_count,
        row.bytes());
    return row;
}

void refresh_test_generation_fixed_rows(
        bioseq::indexing::EncodedMinimizerGeneration& generation) {
    using namespace bioseq::indexing;
    const auto manifest_bytes =
        encode_minimizer_generation_manifest(generation.manifest);
    REQUIRE(manifest_bytes.has_value());
    generation.head.compatibility_checksum =
        minimizer_generation_compatibility_checksum(generation.manifest);
    generation.head.manifest_checksum = crc64_ecma182(
        std::span<const std::byte>{*manifest_bytes});
    const auto head_bytes = encode_minimizer_generation_head(generation.head);
    REQUIRE(head_bytes.has_value());
    generation.manifest_row = make_test_fixed_row(
        IndexChunkAddress{
            .generation_id = generation.manifest.generation_id,
            .position_segment = -1,
            .sub_key = DIRECTORY_SUB_KEY,
            .chunk_no = 0,
        },
        *manifest_bytes);
    generation.unpublished_head_row = make_test_fixed_row(
        IndexChunkAddress{
            .generation_id = 0,
            .position_segment = -1,
            .sub_key = GENERATION_HEAD_SUB_KEY,
            .chunk_no = 0,
        },
        *head_bytes);
}

}  // namespace

TEST_CASE("ExactSimd indexing bridge is linked, exact, and instrumented",
          "[indexing][minimizer-hashmap][exact-simd][bridge][contract]") {
    auto data = make_dna4(4097, 0xB81D6EULL);
    range_t range(data);
    sketch_collector exact_collector;
#if HWY_TARGET == HWY_SCALAR
    REQUIRE_THROWS_AS(
        (void)bioseq::indexing::collect_exact_simd_index_range_or_throw(
            exact_collector,
            range,
            0,
            static_cast<std::size_t>(range.size()),
            10,
            15,
            0,
            "bridge contract"),
        bioseq::indexing::ExactSimdIndexRuntimeError);
#else
    const auto proof =
        bioseq::indexing::collect_exact_simd_index_range_or_throw(
            exact_collector,
            range,
            0,
            static_cast<std::size_t>(range.size()),
            10,
            15,
            0,
            "bridge contract");
    const auto scalar = whole_sequence_minimizers(range, 10, 15);
    const auto& exact = exact_collector.extract_sketches_data();
    REQUIRE(exact.size() == scalar.size());
    for (std::size_t index = 0; index < exact.size(); ++index) {
        REQUIRE(exact[index].raw_value() == scalar[index].raw_value());
    }
    REQUIRE(proof.vector_batches > 0);
    REQUIRE(proof.packed_decode_batches > 0);
    REQUIRE(proof.packed_decode_lanes == proof.bp_consumed);
    REQUIRE(proof.scalar_backend_branches == 0);
    REQUIRE(proof.scalar_tail_symbols == 0);
    REQUIRE(proof.scalar_hash_symbols == 0);
    REQUIRE(proof.scalar_window_transitions == 0);
    REQUIRE(proof.scalar_event_records == 0);
    REQUIRE(proof.scalar_sort_records == 0);
    REQUIRE(proof.scalar_dedup_records == 0);
#endif
}

TEST_CASE("ExactSimd indexing bridge preserves DNA5 nt4 and N reset proof",
          "[indexing][minimizer-hashmap][exact-simd][bridge][dna5]") {
    constexpr std::size_t w = 10;
    constexpr std::size_t k = 15;
    std::vector<seq::dna5> data(4097);
    constexpr std::array<std::uint8_t, 4> acgt_ranks{0, 1, 2, 4};
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index].assign_rank(acgt_ranks[(index * 7u + index / 11u) % 4u]);
    }
    dna5_range_t no_n_range(data);
    sketch_collector no_n_exact_collector;
#if HWY_TARGET == HWY_SCALAR
    REQUIRE_THROWS_AS(
        (void)bioseq::indexing::collect_exact_simd_index_range_or_throw(
            no_n_exact_collector,
            no_n_range,
            0,
            static_cast<std::size_t>(no_n_range.size()),
            w,
            k,
            0,
            "DNA5 bridge contract"),
        bioseq::indexing::ExactSimdIndexRuntimeError);
#else
    const auto no_n_proof =
        bioseq::indexing::collect_exact_simd_index_range_or_throw(
            no_n_exact_collector,
            no_n_range,
            0,
            static_cast<std::size_t>(no_n_range.size()),
            w,
            k,
            0,
            "DNA5 bridge contract");
    const auto no_n_scalar = whole_sequence_minimizers(no_n_range, w, k);
    const auto& no_n_exact = no_n_exact_collector.extract_sketches_data();
    REQUIRE(no_n_exact.size() == no_n_scalar.size());
    for (std::size_t index = 0; index < no_n_exact.size(); ++index) {
        REQUIRE(no_n_exact[index].raw_value()
            == no_n_scalar[index].raw_value());
    }
    REQUIRE(no_n_proof.ambiguous_symbols == 0);
    REQUIRE(no_n_proof.ambiguous_reset_batches == 0);
    REQUIRE(no_n_proof.vector_batches > 0);
    REQUIRE(no_n_proof.packed_decode_lanes == no_n_proof.bp_consumed);

    constexpr std::array<std::size_t, 5> n_positions{
        97, 1023, 2048, 3071, 4095};
    for (const std::size_t position : n_positions) {
        data[position].assign_rank(3); // SeqAn dna5 N rank
    }
    dna5_range_t with_n_range(data);
    sketch_collector with_n_collector;
    const auto with_n_proof =
        bioseq::indexing::collect_exact_simd_index_range_or_throw(
            with_n_collector,
            with_n_range,
            0,
            static_cast<std::size_t>(with_n_range.size()),
            w,
            k,
            0,
            "DNA5 bridge N-reset contract");
    REQUIRE(with_n_proof.ambiguous_symbols == n_positions.size());
    REQUIRE(with_n_proof.ambiguous_reset_batches > 0);
    REQUIRE(with_n_proof.scalar_backend_branches == 0);
    REQUIRE(with_n_proof.scalar_hash_symbols == 0);
    REQUIRE(with_n_proof.scalar_window_transitions == 0);
    REQUIRE(with_n_proof.scalar_event_records == 0);
    REQUIRE(with_n_proof.scalar_sort_records == 0);
    REQUIRE(with_n_proof.scalar_dedup_records == 0);
    const auto expected_events = baseline_dna5_events(data, w, k);
    const auto& exact_events = with_n_collector.extract_sketches_data();
    REQUIRE(exact_events.size() == expected_events.size());
    for (std::size_t index = 0; index < exact_events.size(); ++index) {
        REQUIRE(exact_events[index].kmer() == expected_events[index].hash);
        REQUIRE(exact_events[index].pos_strand_encoded()
            == expected_events[index].encoded_position);
    }
#endif
}

TEST_CASE("DOC0371 rev3 reserved generation-head sub-key is range-pinned",
          "[indexing][minimizer-hashmap][wire]") {
    STATIC_REQUIRE(bioseq::indexing::GENERATION_HEAD_SUB_KEY
        == (std::numeric_limits<bioseq::indexing::index_sub_key_t>::max)() - 1);
    STATIC_REQUIRE(bioseq::indexing::GENERATION_HEAD_SUB_KEY
        > bioseq::indexing::TEMP_RUN_BASE);
    STATIC_REQUIRE(bioseq::indexing::GENERATION_HEAD_SUB_KEY
        < bioseq::indexing::DIRECTORY_SUB_KEY);
}

TEST_CASE("DOC0371 CRC-64/ECMA-182 is exact and incrementally composable",
          "[indexing][minimizer-hashmap][wire][crc]") {
    using bioseq::indexing::Crc64Ecma182;
    using bioseq::indexing::crc64_ecma182;

    STATIC_REQUIRE(crc64_ecma182(std::string_view{}) == 0);
    STATIC_REQUIRE(crc64_ecma182("123456789") == 0x6C40DF5F0B497347ULL);

    Crc64Ecma182 incremental;
    incremental.update("1234");
    incremental.update("56789");
    REQUIRE(incremental.checksum() == 0x6C40DF5F0B497347ULL);

    constexpr std::array<std::byte, 9> bytes{
        std::byte{'1'}, std::byte{'2'}, std::byte{'3'},
        std::byte{'4'}, std::byte{'5'}, std::byte{'6'},
        std::byte{'7'}, std::byte{'8'}, std::byte{'9'},
    };
    STATIC_REQUIRE(crc64_ecma182(std::span<const std::byte>{bytes})
        == 0x6C40DF5F0B497347ULL);
}

TEST_CASE("DOC0371 generation head has a fixed little-endian 64-byte wire image",
          "[indexing][minimizer-hashmap][wire]") {
    using namespace bioseq::indexing;
    constexpr auto b = [](unsigned value) { return static_cast<std::byte>(value); };
    constexpr std::array<std::byte, 64> expected{
        b(0x48), b(0x4f), b(0x49), b(0x42), b(0x03), b(0x00), b(0x40), b(0x00),
        b(0x08), b(0x07), b(0x06), b(0x05), b(0x04), b(0x03), b(0x02), b(0x01),
        b(0x18), b(0x17), b(0x16), b(0x15), b(0x14), b(0x13), b(0x12), b(0x11),
        b(0x28), b(0x27), b(0x26), b(0x25), b(0x24), b(0x23), b(0x22), b(0x21),
        b(0x38), b(0x37), b(0x36), b(0x35), b(0x34), b(0x33), b(0x32), b(0x31),
        b(0x48), b(0x47), b(0x46), b(0x45), b(0x44), b(0x43), b(0x42), b(0x41),
        b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
    };

    constexpr auto head = golden_generation_head();
    constexpr auto encoded = encode_minimizer_generation_head(head);
    STATIC_REQUIRE(encoded.has_value());
    STATIC_REQUIRE(*encoded == expected);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded, 0) == MinimizerGenerationHead::magic);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*encoded, 4) == MinimizerGenerationHead::version);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*encoded, 6) == 64);

    constexpr auto decoded = decode_minimizer_generation_head(
        std::span<const std::byte>{*encoded});
    STATIC_REQUIRE(decoded.has_value());
    STATIC_REQUIRE(*decoded == head);
}

TEST_CASE("DOC0371 generation head rejects every malformed fixed-header class",
          "[indexing][minimizer-hashmap][wire][corruption]") {
    using namespace bioseq::indexing;
    const auto head = golden_generation_head();
    const auto good = *encode_minimizer_generation_head(head);

    require_wire_error(
        decode_minimizer_generation_head(
            std::span<const std::byte>{good.data(), good.size() - 1}),
        MinimizerGenerationWireError::encoded_size);
    std::array<std::byte, 65> oversized{};
    std::copy(good.begin(), good.end(), oversized.begin());
    require_wire_error(decode_minimizer_generation_head(oversized),
                       MinimizerGenerationWireError::encoded_size);

    auto corrupted = good;
    corrupted[0] ^= std::byte{1};
    require_wire_error(decode_minimizer_generation_head(corrupted),
                       MinimizerGenerationWireError::magic);
    corrupted = good;
    test_write_le<std::uint16_t>(corrupted, 4, 2);
    require_wire_error(decode_minimizer_generation_head(corrupted),
                       MinimizerGenerationWireError::version);
    corrupted = good;
    test_write_le<std::uint16_t>(corrupted, 6, 63);
    require_wire_error(decode_minimizer_generation_head(corrupted),
                       MinimizerGenerationWireError::declared_size);

    for (std::size_t offset = 48; offset < 64; ++offset) {
        INFO("reserved byte offset " << offset);
        corrupted = good;
        corrupted[offset] = std::byte{1};
        require_wire_error(decode_minimizer_generation_head(corrupted),
                           MinimizerGenerationWireError::reserved_nonzero);
    }

    corrupted = good;
    test_write_le<std::uint64_t>(corrupted, 8, 0);
    require_wire_error(decode_minimizer_generation_head(corrupted),
                       MinimizerGenerationWireError::generation_id);
    corrupted = good;
    test_write_le<std::uint64_t>(corrupted, 8,
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1u);
    require_wire_error(decode_minimizer_generation_head(corrupted),
                       MinimizerGenerationWireError::generation_id);

    auto invalid = head;
    invalid.generation_id = 0;
    require_wire_error(encode_minimizer_generation_head(invalid),
                       MinimizerGenerationWireError::generation_id);
    invalid.generation_id =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1u;
    require_wire_error(encode_minimizer_generation_head(invalid),
                       MinimizerGenerationWireError::generation_id);

    auto boundary = head;
    boundary.generation_id =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    boundary.source_identity = (std::numeric_limits<std::uint64_t>::max)();
    boundary.compatibility_checksum = (std::numeric_limits<std::uint64_t>::max)();
    boundary.manifest_checksum = (std::numeric_limits<std::uint64_t>::max)();
    boundary.source_content_checksum = (std::numeric_limits<std::uint64_t>::max)();
    const auto boundary_bytes = encode_minimizer_generation_head(boundary);
    REQUIRE(boundary_bytes.has_value());
    require_wire_value(decode_minimizer_generation_head(*boundary_bytes), boundary);
}

TEST_CASE("DOC0371 generation manifest has a fixed little-endian 128-byte wire image",
          "[indexing][minimizer-hashmap][wire]") {
    using namespace bioseq::indexing;
    constexpr auto b = [](unsigned value) { return static_cast<std::byte>(value); };
    constexpr std::array<std::byte, 128> expected{
        b(0x47), b(0x4f), b(0x49), b(0x42), b(0x03), b(0x00), b(0x80), b(0x00),
        b(0x08), b(0x07), b(0x06), b(0x05), b(0x04), b(0x03), b(0x02), b(0x01),
        b(0x18), b(0x17), b(0x16), b(0x15), b(0x14), b(0x13), b(0x12), b(0x11),
        b(0x28), b(0x27), b(0x26), b(0x25), b(0x24), b(0x23), b(0x22), b(0x21),
        b(0x14), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x38), b(0x37), b(0x36), b(0x35), b(0x34), b(0x33), b(0x32), b(0x31),
        b(0x48), b(0x47), b(0x46), b(0x45), b(0x44), b(0x43), b(0x42), b(0x41),
        b(0x58), b(0x57), b(0x56), b(0x55), b(0x54), b(0x53), b(0x52), b(0x51),
        b(0x68), b(0x67), b(0x66), b(0x65), b(0x64), b(0x63), b(0x62), b(0x61),
        b(0x78), b(0x77), b(0x76), b(0x75), b(0x74), b(0x73), b(0x72), b(0x71),
        b(0x10), b(0x0f), b(0x0e), b(0x0d), b(0x0c), b(0x0b), b(0x0a), b(0x09),
        b(0x03), b(0x00), b(0x00), b(0x00), b(0x44), b(0x33), b(0x22), b(0x11),
        b(0x88), b(0x77), b(0x66), b(0x55), b(0x22), b(0x11), b(0x44), b(0x33),
        b(0x66), b(0x55), b(0x88), b(0x77), b(0xcc), b(0xbb), b(0xaa), b(0x99),
        b(0x08), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x01), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
    };

    constexpr auto manifest = golden_generation_manifest();
    constexpr auto encoded = encode_minimizer_generation_manifest(manifest);
    STATIC_REQUIRE(encoded.has_value());
    STATIC_REQUIRE(*encoded == expected);
    STATIC_REQUIRE(minimizer_generation_compatibility_checksum(manifest)
        == 0x7F0EF85B94D48057ULL);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded, 0)
        == MinimizerGenerationManifest::magic);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*encoded, 4)
        == MinimizerGenerationManifest::version);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*encoded, 6) == 128);

    constexpr auto decoded = decode_minimizer_generation_manifest(
        std::span<const std::byte>{*encoded});
    STATIC_REQUIRE(decoded.has_value());
    STATIC_REQUIRE(*decoded == manifest);
}

TEST_CASE("DOC0371 revision-3 manifests distinguish paper ntHash without changing legacy identity",
          "[indexing][minimizer-hashmap][wire][compatibility][scheme]") {
    using namespace bioseq::indexing;
    constexpr MinimizerGenerationEncodeOptions unbound_writer{};
    STATIC_REQUIRE_FALSE(is_supported_minimizer_scheme_identity(
        unbound_writer.minimizer_scheme_id,
        unbound_writer.minimizer_scheme_version));
    constexpr ExpectedMinimizerCompatibility unbound_reader{};
    STATIC_REQUIRE_FALSE(is_supported_minimizer_scheme_identity(
        unbound_reader.minimizer_scheme_id,
        unbound_reader.minimizer_scheme_version));
    STATIC_REQUIRE(minimizer_scheme_identity_for_backend(
        MinimizerSketchBackend::Scalar) == kLegacyMinimapV1SchemeIdentity);
    STATIC_REQUIRE(minimizer_scheme_identity_for_backend(
        MinimizerSketchBackend::ExactSimd) == kLegacyMinimapV1SchemeIdentity);
    STATIC_REQUIRE(minimizer_scheme_identity_for_backend(
        MinimizerSketchBackend::VulkanSegment)
        == kLegacyMinimapV1SchemeIdentity);
    STATIC_REQUIRE(minimizer_scheme_identity_for_backend(
        MinimizerSketchBackend::PaperNtHashSimd)
        == kPaperNtHashU32R7SumV1SchemeIdentity);
    constexpr auto legacy = golden_generation_manifest();
    constexpr auto legacy_bytes = encode_minimizer_generation_manifest(legacy);
    STATIC_REQUIRE(legacy_bytes.has_value());
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*legacy_bytes, 122)
        == kLegacyMinimapV1SchemeId);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*legacy_bytes, 124)
        == kLegacyMinimapV1SchemeVersion);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*legacy_bytes, 126) == 0u);

    constexpr auto paper = [] {
        auto value = golden_generation_manifest();
        value.minimizer_scheme_id = kPaperNtHashU32R7SumV1SchemeId;
        value.minimizer_scheme_version = kPaperNtHashU32R7SumV1SchemeVersion;
        return value;
    }();
    constexpr auto paper_bytes = encode_minimizer_generation_manifest(paper);
    STATIC_REQUIRE(paper_bytes.has_value());
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*paper_bytes, 122)
        == kPaperNtHashU32R7SumV1SchemeId);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*paper_bytes, 124)
        == kPaperNtHashU32R7SumV1SchemeVersion);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*paper_bytes, 126) == 0u);
    STATIC_REQUIRE(*paper_bytes != *legacy_bytes);
    constexpr auto decoded_paper = decode_minimizer_generation_manifest(
        std::span<const std::byte>{*paper_bytes});
    STATIC_REQUIRE(decoded_paper.has_value());
    STATIC_REQUIRE(*decoded_paper == paper);
    STATIC_REQUIRE(minimizer_generation_compatibility_checksum(paper)
        == 0xD8E8287B0F968560ULL);
    STATIC_REQUIRE(minimizer_generation_compatibility_checksum(paper)
        != minimizer_generation_compatibility_checksum(legacy));

    constexpr ExpectedMinimizerCompatibility legacy_reader{
        .alphabet_code = legacy.alphabet_code,
        .storage_unit_byte_width = legacy.storage_unit_byte_width,
        .kmer_size = legacy.kmer_size,
        .window_size = legacy.window_size,
        .gpu_abi_version = legacy.gpu_abi_version,
        .minimizer_scheme_id = kLegacyMinimapV1SchemeId,
        .minimizer_scheme_version = kLegacyMinimapV1SchemeVersion,
    };
    constexpr ExpectedMinimizerCompatibility paper_reader{
        .alphabet_code = paper.alphabet_code,
        .storage_unit_byte_width = paper.storage_unit_byte_width,
        .kmer_size = paper.kmer_size,
        .window_size = paper.window_size,
        .gpu_abi_version = paper.gpu_abi_version,
        .minimizer_scheme_id = kPaperNtHashU32R7SumV1SchemeId,
        .minimizer_scheme_version = kPaperNtHashU32R7SumV1SchemeVersion,
    };
    STATIC_REQUIRE(validate_minimizer_generation_compatibility(
        legacy, legacy_reader).has_value());
    STATIC_REQUIRE(validate_minimizer_generation_compatibility(
        paper, paper_reader).has_value());
    STATIC_REQUIRE_FALSE(validate_minimizer_generation_compatibility(
        legacy, paper_reader).has_value());
    STATIC_REQUIRE(validate_minimizer_generation_compatibility(
        legacy, paper_reader).error()
        == MinimizerGenerationWireError::compatibility_mismatch);
    STATIC_REQUIRE_FALSE(validate_minimizer_generation_compatibility(
        paper, legacy_reader).has_value());
    STATIC_REQUIRE(validate_minimizer_generation_compatibility(
        paper, legacy_reader).error()
        == MinimizerGenerationWireError::compatibility_mismatch);

    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {1, 2}, {2, 4}, {2, 6},
    };
    VectorReplayMinimizerProducer producer{
        minimizers, kPaperNtHashU32R7SumV1SchemeIdentity};
    CollectingMinimizerRowSink sink;
    auto options = test_generation_options(100);
    options.minimizer_scheme_id = kPaperNtHashU32R7SumV1SchemeId;
    options.minimizer_scheme_version = kPaperNtHashU32R7SumV1SchemeVersion;
    const auto generated = encode_minimizer_generation(producer, sink, options);
    REQUIRE(generated.has_value());
    REQUIRE(generated->manifest.minimizer_scheme_id
        == kPaperNtHashU32R7SumV1SchemeId);
    REQUIRE(generated->manifest.minimizer_scheme_version
        == kPaperNtHashU32R7SumV1SchemeVersion);

    VectorReplayMinimizerProducer invalid_producer{
        minimizers, kPaperNtHashU32R7SumV1SchemeIdentity};
    CollectingMinimizerRowSink invalid_sink;
    options.minimizer_scheme_version = kLegacyMinimapV1SchemeVersion;
    require_wire_error(
        encode_minimizer_generation(invalid_producer, invalid_sink, options),
        MinimizerGenerationWireError::compatibility_mismatch);
    REQUIRE(invalid_producer.scan_count == 0);
    REQUIRE(invalid_sink.rows.empty());

    VectorReplayMinimizerProducer mismatched_producer{
        minimizers, kPaperNtHashU32R7SumV1SchemeIdentity};
    CollectingMinimizerRowSink mismatched_sink;
    auto legacy_options = test_generation_options(100);
    require_wire_error(
        encode_minimizer_generation(
            mismatched_producer, mismatched_sink, legacy_options),
        MinimizerGenerationWireError::compatibility_mismatch);
    REQUIRE(mismatched_producer.scan_count == 0);
    REQUIRE(mismatched_sink.rows.empty());
}

TEST_CASE("DOC0371 generation manifest rejects corrupt headers and invalid geometry",
          "[indexing][minimizer-hashmap][wire][corruption]") {
    using namespace bioseq::indexing;
    const auto manifest = golden_generation_manifest();
    const auto good = *encode_minimizer_generation_manifest(manifest);

    require_wire_error(
        decode_minimizer_generation_manifest(
            std::span<const std::byte>{good.data(), good.size() - 1}),
        MinimizerGenerationWireError::encoded_size);
    std::array<std::byte, 129> oversized{};
    std::copy(good.begin(), good.end(), oversized.begin());
    require_wire_error(decode_minimizer_generation_manifest(oversized),
                       MinimizerGenerationWireError::encoded_size);

    auto corrupted = good;
    corrupted[0] ^= std::byte{1};
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       MinimizerGenerationWireError::magic);
    corrupted = good;
    test_write_le<std::uint16_t>(corrupted, 4, 2);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       MinimizerGenerationWireError::version);
    corrupted = good;
    test_write_le<std::uint16_t>(corrupted, 6, 127);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       MinimizerGenerationWireError::declared_size);

    for (std::size_t offset = 126; offset < 128; ++offset) {
        INFO("reserved byte offset " << offset);
        corrupted = good;
        corrupted[offset] = std::byte{1};
        require_wire_error(decode_minimizer_generation_manifest(corrupted),
                           MinimizerGenerationWireError::reserved_nonzero);
    }

    constexpr std::array<std::pair<std::uint16_t, std::uint16_t>, 4>
        unsupported_schemes{{
            {kLegacyMinimapV1SchemeId, kPaperNtHashU32R7SumV1SchemeVersion},
            {kPaperNtHashU32R7SumV1SchemeId, kLegacyMinimapV1SchemeVersion},
            {2u, kPaperNtHashU32R7SumV1SchemeVersion},
            {kPaperNtHashU32R7SumV1SchemeId, 2u},
        }};
    for (const auto [scheme_id, scheme_version] : unsupported_schemes) {
        INFO("unsupported minimizer scheme " << scheme_id
            << "/" << scheme_version);
        auto invalid_scheme = manifest;
        invalid_scheme.minimizer_scheme_id = scheme_id;
        invalid_scheme.minimizer_scheme_version = scheme_version;
        require_wire_error(
            encode_minimizer_generation_manifest(invalid_scheme),
            MinimizerGenerationWireError::compatibility_mismatch);

        corrupted = good;
        test_write_le<std::uint16_t>(corrupted, 122, scheme_id);
        test_write_le<std::uint16_t>(corrupted, 124, scheme_version);
        require_wire_error(
            decode_minimizer_generation_manifest(corrupted),
            MinimizerGenerationWireError::compatibility_mismatch);
    }

    corrupted = good;
    test_write_le<std::uint64_t>(corrupted, 8, 0);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       MinimizerGenerationWireError::generation_id);
    corrupted = good;
    test_write_le<std::uint64_t>(corrupted, 8,
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1u);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       MinimizerGenerationWireError::generation_id);

    corrupted = good;
    test_write_le<std::uint16_t>(corrupted, 120, 2);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       MinimizerGenerationWireError::segment_geometry_version);
    corrupted = good;
    test_write_le<std::uint64_t>(corrupted, 112, 0);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       MinimizerGenerationWireError::segment_span);
    corrupted = good;
    test_write_le<std::uint32_t>(corrupted, 88, 2);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       MinimizerGenerationWireError::segment_count);
    corrupted = good;
    test_write_le<std::uint32_t>(corrupted, 96,
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()) + 1u);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       MinimizerGenerationWireError::directory_chunk_count);

    corrupted = good;
    test_write_le<std::uint64_t>(corrupted, 32,
        static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()) + 1u);
    test_write_le<std::uint64_t>(corrupted, 112, 1);
    test_write_le<std::uint32_t>(corrupted, 88,
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()) + 1u);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       MinimizerGenerationWireError::segment_count);

    auto invalid = manifest;
    invalid.generation_id = 0;
    require_wire_error(encode_minimizer_generation_manifest(invalid),
                       MinimizerGenerationWireError::generation_id);
    invalid = manifest;
    invalid.position_segment_count = 4;
    require_wire_error(encode_minimizer_generation_manifest(invalid),
                       MinimizerGenerationWireError::segment_count);
    invalid = manifest;
    invalid.position_segment_span_bp = 0;
    require_wire_error(encode_minimizer_generation_manifest(invalid),
                       MinimizerGenerationWireError::segment_span);
    invalid = manifest;
    invalid.segment_geometry_version = 2;
    require_wire_error(encode_minimizer_generation_manifest(invalid),
                       MinimizerGenerationWireError::segment_geometry_version);
    invalid = manifest;
    invalid.directory_chunk_count =
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()) + 1u;
    require_wire_error(encode_minimizer_generation_manifest(invalid),
                       MinimizerGenerationWireError::directory_chunk_count);
    invalid = manifest;
    invalid.generation_id =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1u;
    require_wire_error(encode_minimizer_generation_manifest(invalid),
                       MinimizerGenerationWireError::generation_id);

    auto signed_boundary = manifest;
    signed_boundary.generation_id =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    signed_boundary.target_length =
        static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)());
    signed_boundary.position_segment_span_bp = 1;
    signed_boundary.position_segment_count =
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());
    signed_boundary.directory_chunk_count =
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());
    const auto signed_boundary_bytes =
        encode_minimizer_generation_manifest(signed_boundary);
    REQUIRE(signed_boundary_bytes.has_value());
    require_wire_value(
        decode_minimizer_generation_manifest(*signed_boundary_bytes),
        signed_boundary);

    auto unrestricted_words = manifest;
    unrestricted_words.generation_id =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    unrestricted_words.source_identity = (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.source_content_checksum =
        (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.target_length = (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.control_count = (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.position_count = (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.payload_bytes = (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.payload_checksum = (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.directory_bytes = (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.directory_checksum = (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.position_segment_count = 2;
    unrestricted_words.extent_count = (std::numeric_limits<std::uint32_t>::max)();
    unrestricted_words.directory_chunk_count =
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());
    unrestricted_words.position_segment_span_bp = std::uint64_t{1} << 63u;
    const auto unrestricted_bytes =
        encode_minimizer_generation_manifest(unrestricted_words);
    REQUIRE(unrestricted_bytes.has_value());
    require_wire_value(
        decode_minimizer_generation_manifest(*unrestricted_bytes),
        unrestricted_words);

    auto empty = manifest;
    empty.target_length = 0;
    empty.position_segment_count = 0;
    REQUIRE(encode_minimizer_generation_manifest(empty).has_value());
    empty.position_segment_span_bp = 0;
    REQUIRE(encode_minimizer_generation_manifest(empty).has_value());
    empty.position_segment_count = 1;
    require_wire_error(encode_minimizer_generation_manifest(empty),
                       MinimizerGenerationWireError::segment_count);
}

TEST_CASE("DOC0371 standalone manifest codec defers referenced-object consistency",
          "[indexing][minimizer-hashmap][wire]") {
    using namespace bioseq::indexing;

    auto manifest = golden_generation_manifest();
    // These combinations are intentionally nonsensical without the referenced
    // objects. The fixed codec preserves them; generation validation must match
    // byte counts, checksums, extents, compatibility, and head identity against
    // the actual directory and payload before publication or use.
    manifest.payload_bytes = 0;
    manifest.payload_checksum = (std::numeric_limits<std::uint64_t>::max)();
    manifest.directory_bytes = 1;
    manifest.directory_checksum = (std::numeric_limits<std::uint64_t>::max)();
    manifest.extent_count = (std::numeric_limits<std::uint32_t>::max)();
    manifest.directory_chunk_count = 0;

    const auto encoded = encode_minimizer_generation_manifest(manifest);
    REQUIRE(encoded.has_value());
    require_wire_value(decode_minimizer_generation_manifest(*encoded), manifest);
}

TEST_CASE("DOC0371 uniform persisted segment cores and bp ranges are exact at boundaries",
          "[indexing][minimizer-hashmap][wire][geometry]") {
    using namespace bioseq::indexing;
    const auto manifest = golden_generation_manifest();

    require_wire_value(minimizer_position_segment_core(manifest, 0),
                       MinimizerPositionSegmentCore{0, 8});
    require_wire_value(minimizer_position_segment_core(manifest, 1),
                       MinimizerPositionSegmentCore{8, 16});
    require_wire_value(minimizer_position_segment_core(manifest, 2),
                       MinimizerPositionSegmentCore{16, 20});
    require_wire_error(minimizer_position_segment_core(manifest, 3),
                       MinimizerGenerationWireError::segment_ordinal);

    require_wire_value(minimizer_position_segments_for_bp_range(manifest, 0, 0),
                       MinimizerPositionSegmentOrdinalRange{0, 0});
    require_wire_value(minimizer_position_segments_for_bp_range(manifest, 0, 1),
                       MinimizerPositionSegmentOrdinalRange{0, 1});
    require_wire_value(minimizer_position_segments_for_bp_range(manifest, 7, 9),
                       MinimizerPositionSegmentOrdinalRange{0, 2});
    require_wire_value(minimizer_position_segments_for_bp_range(manifest, 16, 20),
                       MinimizerPositionSegmentOrdinalRange{2, 3});
    require_wire_value(minimizer_position_segments_for_bp_range(manifest, 20, 20),
                       MinimizerPositionSegmentOrdinalRange{3, 3});
    require_wire_error(minimizer_position_segments_for_bp_range(manifest, 9, 8),
                       MinimizerGenerationWireError::bp_range);
    require_wire_error(minimizer_position_segments_for_bp_range(manifest, 0, 21),
                       MinimizerGenerationWireError::bp_range);

    auto huge = manifest;
    huge.target_length = (std::numeric_limits<std::uint64_t>::max)();
    huge.position_segment_span_bp = std::uint64_t{1} << 63u;
    huge.position_segment_count = 2;
    const auto last = minimizer_position_segment_core(huge, 1);
    REQUIRE(last.has_value());
    REQUIRE(last->begin_bp == (std::uint64_t{1} << 63u));
    REQUIRE(last->end_bp == (std::numeric_limits<std::uint64_t>::max)());

    auto empty = manifest;
    empty.target_length = 0;
    empty.position_segment_span_bp = 0;
    empty.position_segment_count = 0;
    require_wire_value(minimizer_position_segments_for_bp_range(empty, 0, 0),
                       MinimizerPositionSegmentOrdinalRange{0, 0});
    require_wire_error(minimizer_position_segment_core(empty, 0),
                       MinimizerGenerationWireError::segment_ordinal);

    STATIC_REQUIRE(sql_bigint_can_represent(
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())));
    STATIC_REQUIRE_FALSE(sql_bigint_can_represent(
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1u));
    STATIC_REQUIRE(sql_integer_can_represent(
        static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())));
    STATIC_REQUIRE_FALSE(sql_integer_can_represent(
        static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()) + 1u));
}

TEST_CASE("DOC0371 lookup and persistence minimizer roles are atomically split",
          "[indexing][minimizer-hashmap][concept]") {
    using namespace bioseq::indexing;
    STATIC_REQUIRE(MinimizerLookupIndex<MinimizerHashmapFragmentIndex>);
    STATIC_REQUIRE(PersistableMinimizerIndex<MinimizerHashmapFragmentIndex>);
    STATIC_REQUIRE(MinimizerLookupIndex<PreparedSerializedSegment>);
    STATIC_REQUIRE_FALSE(PersistableMinimizerIndex<PreparedSerializedSegment>);

    constexpr SegmentUnitRange range{1, 9, 32, 256};
    STATIC_REQUIRE(range.cs_u == 1);
    STATIC_REQUIRE(range.ce_u == 9);
    STATIC_REQUIRE(range.core_lo_bp == 32);
    STATIC_REQUIRE(range.core_hi_bp == 256);
}

TEST_CASE("DOC0371 directory entry is an exact 96-byte little-endian ABI",
          "[indexing][minimizer-hashmap][wire][directory]") {
    using namespace bioseq::indexing;
    constexpr MinimizerBucketDirEntry entry{
        .partition_id = 0x11223344U,
        .position_segment = 0x10203040,
        .lo_hash = 0x0102030405060708ULL,
        .hi_hash = 0x1112131415161718ULL,
        .sub_key = 0x2122232425262728LL,
        .control_first_chunk = 0,
        .control_chunk_count = 2,
        .position_first_chunk = 2,
        .position_chunk_count = 3,
        .control_element_count = 768,
        .position_element_count = 2304,
        .control_encoded_bytes = 12288,
        .position_encoded_bytes = 18432,
        .control_checksum = 0x3132333435363738ULL,
        .position_checksum = 0x4142434445464748ULL,
    };
    constexpr auto encoded = encode_minimizer_bucket_dir_entry(entry);
    STATIC_REQUIRE(encoded.has_value());
    STATIC_REQUIRE(encoded->size() == 96);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded, 0) == entry.partition_id);
    STATIC_REQUIRE(std::bit_cast<std::int32_t>(
        test_read_le<std::uint32_t>(*encoded, 4)) == entry.position_segment);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 8) == entry.lo_hash);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 16) == entry.hi_hash);
    STATIC_REQUIRE(std::bit_cast<std::int64_t>(
        test_read_le<std::uint64_t>(*encoded, 24)) == entry.sub_key);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded, 32) == 0);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded, 36) == 2);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded, 40) == 2);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded, 44) == 3);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 48) == 768);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 56) == 2304);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 64) == 12288);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 72) == 18432);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 80)
        == entry.control_checksum);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 88)
        == entry.position_checksum);
    constexpr auto decoded = decode_minimizer_bucket_dir_entry(*encoded);
    STATIC_REQUIRE(decoded.has_value());
    STATIC_REQUIRE(*decoded == entry);

    require_wire_error(
        decode_minimizer_bucket_dir_entry(
            std::span<const std::byte>{encoded->data(), encoded->size() - 1u}),
        MinimizerGenerationWireError::encoded_size);
    auto invalid = entry;
    invalid.position_segment = -1;
    require_wire_error(encode_minimizer_bucket_dir_entry(invalid),
                       MinimizerGenerationWireError::position_segment);
    invalid = entry;
    invalid.lo_hash = invalid.hi_hash + 1u;
    require_wire_error(encode_minimizer_bucket_dir_entry(invalid),
                       MinimizerGenerationWireError::hash_range);
    invalid = entry;
    invalid.sub_key = TEMP_RUN_BASE;
    require_wire_error(encode_minimizer_bucket_dir_entry(invalid),
                       MinimizerGenerationWireError::sub_key);
    invalid = entry;
    invalid.control_encoded_bytes -= 16u;
    require_wire_error(encode_minimizer_bucket_dir_entry(invalid),
                       MinimizerGenerationWireError::byte_count);
    invalid = entry;
    invalid.position_first_chunk = 1;
    require_wire_error(encode_minimizer_bucket_dir_entry(invalid),
                       MinimizerGenerationWireError::chunk_range);
}

TEST_CASE("DOC0371 DirView owns bytes and rejects unstable directory order",
          "[indexing][minimizer-hashmap][wire][directory][corruption]") {
    using namespace bioseq::indexing;
    const MinimizerBucketDirEntry first{
        .partition_id = 0,
        .position_segment = 0,
        .lo_hash = 10,
        .hi_hash = 20,
        .sub_key = 0,
        .control_first_chunk = 0,
        .control_chunk_count = 1,
        .position_first_chunk = 1,
        .position_chunk_count = 1,
        .control_element_count = 1,
        .position_element_count = 1,
        .control_encoded_bytes = 16,
        .position_encoded_bytes = 8,
        .control_checksum = 1,
        .position_checksum = 2,
    };
    auto second = first;
    second.partition_id = 1;
    second.lo_hash = 21;
    second.hi_hash = 30;
    second.sub_key = 1;
    const std::array entries{first, second};
    auto bytes = encode_minimizer_bucket_directory(entries);
    REQUIRE(bytes.has_value());
    auto view = DirView::decode(*bytes);
    REQUIRE(view.has_value());
    REQUIRE(view->entries().size() == 2);
    REQUIRE(view->find(0, 10)->partition_id == 0);
    REQUIRE(view->find(0, 30)->partition_id == 1);
    REQUIRE(view->find(0, 31) == nullptr);
    REQUIRE(view->find(1, 10) == nullptr);

    // Source mutation cannot affect the owning view or its GPU-upload bytes.
    (*bytes)[8] ^= std::byte{0xff};
    REQUIRE(view->entries()[0].lo_hash == 10);
    REQUIRE(view->encoded_bytes()[8] != (*bytes)[8]);

    auto overlap = second;
    overlap.lo_hash = first.hi_hash;
    const std::array overlapping{first, overlap};
    require_wire_error(encode_minimizer_bucket_directory(overlapping),
                       MinimizerGenerationWireError::directory_order);
    auto wrong_ordinal = second;
    wrong_ordinal.partition_id = 7;
    const std::array wrong_ordinals{first, wrong_ordinal};
    require_wire_error(encode_minimizer_bucket_directory(wrong_ordinals),
                       MinimizerGenerationWireError::partition_id);
    std::array<std::byte, 95> truncated{};
    require_wire_error(DirView::decode(truncated),
                       MinimizerGenerationWireError::directory_size);
}

TEST_CASE("DOC0371 deterministic encoder fixes core ownership and policy bytes",
          "[indexing][minimizer-hashmap][wire][encoder]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t span = kMinimizerPreferredSegmentSpanBp;
    constexpr auto encoded_position = [](std::uint64_t position, std::uint64_t strand) {
        return (position << 1u) | strand;
    };
    std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {100, encoded_position(span, 1)},
        {7, encoded_position(span - 1u, 0)},
        {100, encoded_position(9, 1)},
        {9, encoded_position(span + 3u, 0)},
        {100, encoded_position(3, 0)},
    };
    const auto options = test_generation_options(span + 100u);

    VectorReplayMinimizerProducer direct_producer{minimizers};
    CollectingMinimizerRowSink direct_sink;
    auto direct = encode_minimizer_generation(
        direct_producer, direct_sink, options);
    REQUIRE(direct.has_value());
    REQUIRE(direct_producer.scan_count == 2);

    std::reverse(minimizers.begin(), minimizers.end());
    VectorReplayMinimizerProducer reversed_producer{minimizers};
    CollectingMinimizerRowSink reversed_sink;
    auto reversed_input = encode_minimizer_generation(
        reversed_producer, reversed_sink, options);
    REQUIRE(reversed_input.has_value());
    require_encoded_generations_equal_normalized(
        *direct, direct_sink.rows, *reversed_input, reversed_sink.rows);

    REQUIRE(direct->manifest.position_segment_span_bp == span);
    REQUIRE(direct->manifest.position_segment_count == 2);
    REQUIRE(direct->manifest.segment_geometry_version
        == kMinimizerSegmentGeometryVersion);
    REQUIRE(direct->manifest.gpu_abi_version
        == kMinimizerSortedDenseGpuAbiVersion);
    REQUIRE(direct->head.compatibility_checksum
        == minimizer_generation_compatibility_checksum(direct->manifest));
    REQUIRE(validate_minimizer_generation_metadata(
        direct->head, direct->manifest, direct->directory).has_value());

    bool saw_first_core = false;
    bool saw_second_core = false;
    for (const MinimizerBucketDirEntry& entry : direct->directory.entries()) {
        saw_first_core = saw_first_core || entry.position_segment == 0;
        saw_second_core = saw_second_core || entry.position_segment == 1;
        const auto core = minimizer_position_segment_core(
            direct->manifest, static_cast<std::uint32_t>(entry.position_segment));
        REQUIRE(core.has_value());
        const auto position_bytes = collect_encoded_extent(
            direct_sink.rows, entry, false);
        for (std::size_t offset = 0; offset < position_bytes.size(); offset += 8u) {
            const std::uint64_t value = test_read_le<std::uint64_t>(
                position_bytes, offset);
            REQUIRE((value >> 1u) >= core->begin_bp);
            REQUIRE((value >> 1u) < core->end_bp);
        }
        REQUIRE(entry.control_encoded_bytes % 16u == 0);
        REQUIRE(entry.position_encoded_bytes % 8u == 0);
    }
    REQUIRE(saw_first_core);
    REQUIRE(saw_second_core);
    REQUIRE(direct_sink.max_callback_bytes <= kMinimizerExtentPayloadBytes);
    for (const CollectedMinimizerRow& row : direct_sink.rows) {
        REQUIRE(row.bytes.size() <= kMinimizerExtentPayloadBytes);
        REQUIRE(row.row_checksum == crc64_index_chunk_row(
            row.address.generation_id,
            row.address.position_segment,
            row.address.sub_key,
            row.address.chunk_no,
            static_cast<std::uint32_t>(row.bytes.size()),
            row.bytes));
    }
    REQUIRE(direct_sink.rows.back().address == direct->manifest_row.address);
    REQUIRE(std::ranges::none_of(
        direct_sink.rows,
        [direct](const CollectedMinimizerRow& row) {
            return row.address == direct->unpublished_head_row.address;
        }));
    require_complete_generation_rows(*direct, direct_sink.rows);
}

TEST_CASE("DOC0371 independent MPF halo and MPS merge producers are canonical",
          "[indexing][minimizer-hashmap][wire][encoder][boundary]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 1024;
    constexpr std::size_t fragment_bp = 32768;
    constexpr std::size_t halo = 10u + 15u - 2u;
    using chunked = sequence_scanner_test_chunked<
        std::uint64_t, fragment_units>;
    using Index = MinimizersPerFragmentIndex<FakeFragmentStorage<>>;
    using MpsIndex = MinimizerSequenceMergedIndex<FakeFragmentStorage<>>;
    auto data = make_dna4(70000, 0x371C0DEULL);
    range_t range(data);

    auto oracle_sketches = whole_sequence_minimizers(range, 10, 15);
    std::vector<CanonicalMinimizerEvent> oracle;
    oracle.reserve(oracle_sketches.size());
    for (const MinimapSketchData& minimizer : oracle_sketches) {
        oracle.push_back({
            .hash = minimizer.kmer(),
            .encoded_position = minimizer.pos_strand_encoded(),
        });
    }
    std::sort(oracle.begin(), oracle.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.hash != rhs.hash) return lhs.hash < rhs.hash;
        return lhs.encoded_position < rhs.encoded_position;
    });
    REQUIRE(std::adjacent_find(oracle.begin(), oracle.end()) == oracle.end());
    REQUIRE(std::ranges::any_of(oracle, [](const auto& event) {
        const std::uint64_t position = event.encoded_position >> 1u;
        return position + halo >= fragment_bp
            && position <= fragment_bp + halo;
    }));

    lazy_compact_sequence<dna4_u64, chunked> lazy{
        chunked::from_range(range)};
    auto mpf_producer = Index::make_generation_replay_producer(
        lazy, MinimizerSketchBackend::Scalar);
    CollectingMinimizerRowSink mpf_sink;
    auto mpf_options = test_generation_options(range.bp_size());
    auto mpf = encode_minimizer_generation(
        mpf_producer, mpf_sink, mpf_options);
    REQUIRE(mpf.has_value());
    REQUIRE(mpf_producer.scan_count() == 2);
    REQUIRE(mpf_producer.peak_buffered_core_bp()
        <= kMinimizerPreferredSegmentSpanBp);

    FakeFragmentStorage<> spill_storage;
    const auto spilled = MpsIndex::spill_generation_replay_runs_serial(
        spill_storage,
        lazy,
        MinimizerSketchBackend::Scalar,
        fragment_bp);
    REQUIRE(spilled.has_value());
    REQUIRE(spilled->run_count >= 3);
    auto mps_producer = MpsIndex::make_generation_replay_producer(
        spill_storage, range.bp_size(), *spilled);
    CollectingMinimizerRowSink mps_sink;
    auto mps_options = mpf_options;
    ++mps_options.generation_id;
    auto mps = encode_minimizer_generation(
        mps_producer, mps_sink, mps_options);
    REQUIRE(mps.has_value());
    REQUIRE(mps_producer.scan_count() == 2);
    REQUIRE(mps_producer.merge_pass_count() == 2);
    REQUIRE(spill_storage.segment_merge_calls() == 2);
    REQUIRE(spill_storage.segment_merge_run_visits()
        == 2u * spill_storage.segment_spill_run_count());
    REQUIRE(spill_storage.segment_merge_bytes_visited()
        == 2u * spill_storage.segment_spill_bytes());
    auto mismatched_spill_producer =
        MpsIndex::make_generation_replay_producer(
            spill_storage, range.bp_size(), *spilled);
    CollectingMinimizerRowSink mismatched_spill_sink;
    auto mismatched_spill_options = mps_options;
    mismatched_spill_options.minimizer_scheme_id =
        kPaperNtHashU32R7SumV1SchemeId;
    mismatched_spill_options.minimizer_scheme_version =
        kPaperNtHashU32R7SumV1SchemeVersion;
    const std::uint64_t merge_calls_before_mismatch =
        spill_storage.segment_merge_calls();
    require_wire_error(
        encode_minimizer_generation(
            mismatched_spill_producer,
            mismatched_spill_sink,
            mismatched_spill_options),
        MinimizerGenerationWireError::compatibility_mismatch);
    REQUIRE(mismatched_spill_producer.scan_count() == 0);
    REQUIRE(spill_storage.segment_merge_calls()
        == merge_calls_before_mismatch);
    REQUIRE(mismatched_spill_sink.rows.empty());
    STATIC_REQUIRE(decltype(mps_producer)::owned_event_capacity() == 0);
    require_encoded_generations_equal_normalized(
        *mpf, mpf_sink.rows, *mps, mps_sink.rows);

    const auto mpf_events = decode_generation_events(*mpf, mpf_sink.rows);
    const auto mps_events = decode_generation_events(*mps, mps_sink.rows);
    REQUIRE(mpf_events == oracle);
    REQUIRE(mps_events == oracle);
    const auto oracle_lookup = canonical_event_lookup_map(oracle);
    REQUIRE(canonical_event_lookup_map(mpf_events) == oracle_lookup);
    REQUIRE(canonical_event_lookup_map(mps_events) == oracle_lookup);

    // The internal all-SIMD backend must traverse the same fragmented MPF
    // cores and MPS spill runs without scalar sketch fallback. Revision-3
    // bytes are compared after normalising generation identity.
    lazy_compact_sequence<dna4_u64, chunked> lazy_exact{
        chunked::from_range(range)};
    auto exact_mpf_producer = Index::make_generation_replay_producer(
        lazy_exact, MinimizerSketchBackend::ExactSimd);
    CollectingMinimizerRowSink exact_mpf_sink;
    auto exact_mpf_options = mpf_options;
    exact_mpf_options.generation_id += 2;
    auto exact_mpf = encode_minimizer_generation(
        exact_mpf_producer, exact_mpf_sink, exact_mpf_options);
    REQUIRE(exact_mpf.has_value());

    FakeFragmentStorage<> exact_spill_storage;
    const auto exact_spilled = MpsIndex::spill_generation_replay_runs_serial(
        exact_spill_storage,
        lazy_exact,
        MinimizerSketchBackend::ExactSimd,
        fragment_bp);
    REQUIRE(exact_spilled.has_value());
    REQUIRE(exact_spilled->run_count == spilled->run_count);
    auto exact_mps_producer = MpsIndex::make_generation_replay_producer(
        exact_spill_storage, range.bp_size(), *exact_spilled);
    CollectingMinimizerRowSink exact_mps_sink;
    auto exact_mps_options = mpf_options;
    exact_mps_options.generation_id += 3;
    auto exact_mps = encode_minimizer_generation(
        exact_mps_producer, exact_mps_sink, exact_mps_options);
    REQUIRE(exact_mps.has_value());

    require_encoded_generations_equal_normalized(
        *mpf, mpf_sink.rows, *exact_mpf, exact_mpf_sink.rows);
    require_encoded_generations_equal_normalized(
        *mps, mps_sink.rows, *exact_mps, exact_mps_sink.rows);
    REQUIRE(decode_generation_events(*exact_mpf, exact_mpf_sink.rows)
        == oracle);
    REQUIRE(decode_generation_events(*exact_mps, exact_mps_sink.rows)
        == oracle);
    require_complete_generation_rows(*mpf, mpf_sink.rows);
    require_complete_generation_rows(*mps, mps_sink.rows);
    require_complete_generation_rows(*exact_mpf, exact_mpf_sink.rows);
    require_complete_generation_rows(*exact_mps, exact_mps_sink.rows);
}

TEST_CASE("DOC0371 actual producers cross the persisted 8MiB core edge bounded",
          "[indexing][minimizer-hashmap][wire][encoder][boundary][large]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t span = kMinimizerPreferredSegmentSpanBp;
    constexpr std::size_t fragment_units = 1024;
    constexpr std::uint64_t fragment_bp = fragment_units * 32u;
    using chunked = sequence_scanner_test_chunked<
        std::uint64_t, fragment_units>;
    using Index = MinimizersPerFragmentIndex<FakeFragmentStorage<>>;
    using MpsIndex = MinimizerSequenceMergedIndex<FakeFragmentStorage<>>;

    auto data = make_dna4(static_cast<std::size_t>(span + 8192u + 7u),
                          0x8B1B0A4DULL);
    // A mirror-symmetric boundary fixture with a homopolymer centre exercises
    // palindrome strand ties and rightmost equal-minimum selection on both
    // sides of the immutable persisted-core edge.
    for (std::size_t offset = 0; offset < 192; ++offset) {
        seq::dna4 value{};
        value.assign_rank(static_cast<std::uint8_t>((offset * 3u) & 3u));
        data[static_cast<std::size_t>(span) - 1u - offset] = value;
        data[static_cast<std::size_t>(span) + offset] = value;
    }
    for (std::size_t position = static_cast<std::size_t>(span) - 24u;
         position < static_cast<std::size_t>(span) + 24u;
         ++position) {
        data[position].assign_rank(0);
    }
    range_t range(data);
    lazy_compact_sequence<dna4_u64, chunked> lazy{
        chunked::from_range(range)};

    auto mpf_producer = Index::make_generation_replay_producer(
        lazy, MinimizerSketchBackend::Scalar);
    const auto options = test_generation_options(range.bp_size());
    const auto mpf_plan = plan_minimizer_generation(mpf_producer, options);
    REQUIRE(mpf_plan.has_value());
    REQUIRE(mpf_plan->position_segment_count == 2);
    REQUIRE(mpf_producer.peak_buffered_core_bp() <= span);
    REQUIRE(mpf_producer.scan_count() == 1);

    FakeFragmentStorage<> spill_storage;
    const auto spilled = MpsIndex::spill_generation_replay_runs_serial(
        spill_storage,
        lazy,
        MinimizerSketchBackend::Scalar,
        fragment_bp);
    REQUIRE(spilled.has_value());
    REQUIRE(spilled->run_count > 256);
    REQUIRE(spill_storage.runs_.empty());
    REQUIRE(spill_storage.segment_spill_run_count() == spilled->run_count);
    REQUIRE(spill_storage.segment_runs_.size() == 2);
    auto mps_producer = MpsIndex::make_generation_replay_producer(
        spill_storage, range.bp_size(), *spilled);
    const auto mps_plan = plan_minimizer_generation(mps_producer, options);
    REQUIRE(mps_plan.has_value());
    REQUIRE(mps_producer.scan_count() == 1);
    // Two persisted segments still cause exactly one segment-major replay:
    // each qualified run/byte is visited once, not one full merge per segment.
    REQUIRE(mps_producer.merge_pass_count() == 1);
    REQUIRE(spill_storage.segment_merge_calls() == 1);
    REQUIRE(spill_storage.segment_merge_run_visits()
        == spill_storage.segment_spill_run_count());
    REQUIRE(spill_storage.segment_merge_bytes_visited()
        == spill_storage.segment_spill_bytes());
    STATIC_REQUIRE(decltype(mps_producer)::owned_event_capacity() == 0);

    REQUIRE(mpf_plan->canonical_event_count
        == mps_plan->canonical_event_count);
    REQUIRE(mpf_plan->canonical_event_checksum
        == mps_plan->canonical_event_checksum);
    REQUIRE(mpf_plan->entries == mps_plan->entries);
    REQUIRE(std::ranges::any_of(mpf_plan->entries, [](const auto& entry) {
        return entry.position_segment == 0;
    }));
    REQUIRE(std::ranges::any_of(mpf_plan->entries, [](const auto& entry) {
        return entry.position_segment == 1;
    }));
}

TEST_CASE("DOC0371 actual producers preserve DNA5 N reset and packed tail",
          "[indexing][minimizer-hashmap][wire][encoder][boundary][dna5]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 1024;
    constexpr std::uint64_t fragment_bp = fragment_units * 21u;
    using chunked = sequence_scanner_test_chunked<
        std::uint64_t, fragment_units>;
    using Index = MinimizersPerFragmentIndex<FakeFragmentStorage<>>;
    using MpsIndex = MinimizerSequenceMergedIndex<FakeFragmentStorage<>>;

    constexpr std::size_t target_size = 70013;
    std::vector<seq::dna5> data(target_size);
    std::uint64_t state = 0xD5A5C0DEULL;
    std::vector<std::size_t> n_positions;
    for (std::size_t index = 0; index < data.size(); ++index) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        std::uint8_t rank = static_cast<std::uint8_t>((state >> 33u) % 4u);
        if (index % 211u == 97u) {
            rank = 3u; // dna5 N: rolling hash/window reset sentinel
            n_positions.push_back(index);
        } else if (rank == 3u) {
            rank = 4u; // T is the fifth dna5 rank, but normal nt4 base 3
        }
        data[index].assign_rank(rank);
    }
    for (const std::size_t boundary : {
            static_cast<std::size_t>(fragment_bp - 1u),
            static_cast<std::size_t>(fragment_bp),
            target_size - 80u}) {
        data[boundary].assign_rank(3u);
        n_positions.push_back(boundary);
    }
    dna5_range_t range(data);
    REQUIRE(range.padding() != 0);

    std::vector<CanonicalMinimizerEvent> oracle =
        baseline_dna5_events(data, 10, 15);
    std::ranges::sort(oracle, {}, [](const CanonicalMinimizerEvent& event) {
        return std::pair{event.hash, event.encoded_position};
    });

    lazy_compact_sequence<dna5_u64, chunked> lazy{
        chunked::from_range(range)};
    const auto collection_range = MpsIndex::make_generation_collection_range(
        lazy,
        fragment_bp,
        fragment_bp + 100u);
    REQUIRE(collection_range.has_value());
    constexpr std::uint64_t halo = 10u + 15u - 2u;
    REQUIRE(collection_range->cs_u == (fragment_bp - halo) / 21u);
    REQUIRE(collection_range->ce_u
        == (fragment_bp + 100u + halo) / 21u
            + ((fragment_bp + 100u + halo) % 21u != 0 ? 1u : 0u));
    REQUIRE(collection_range->core_lo_bp == fragment_bp);
    REQUIRE(collection_range->core_hi_bp == fragment_bp + 100u);
    auto options = test_generation_options(range.bp_size());
    options.alphabet_code = 5;
    auto mpf_producer = Index::make_generation_replay_producer(
        lazy, MinimizerSketchBackend::ExactSimd);
    CollectingMinimizerRowSink mpf_sink;
    auto mpf = encode_minimizer_generation(
        mpf_producer, mpf_sink, options);
    REQUIRE(mpf.has_value());

    FakeFragmentStorage<> spill_storage;
    const auto spilled = MpsIndex::spill_generation_replay_runs_serial(
        spill_storage,
        lazy,
        MinimizerSketchBackend::ExactSimd,
        fragment_bp);
    REQUIRE(spilled.has_value());
    auto mps_producer = MpsIndex::make_generation_replay_producer(
        spill_storage, range.bp_size(), *spilled);
    CollectingMinimizerRowSink mps_sink;
    auto mps_options = options;
    ++mps_options.generation_id;
    auto mps = encode_minimizer_generation(
        mps_producer, mps_sink, mps_options);
    REQUIRE(mps.has_value());
    REQUIRE(mpf_producer.exact_simd_proof_present());
    REQUIRE(mpf_producer.exact_simd_proof().accepted(true));
    REQUIRE(mpf_producer.exact_simd_proof().ambiguous_symbols > 0);
    REQUIRE(spilled->exact_simd_proof_present);
    REQUIRE(spilled->exact_simd_proof.accepted(true));
    REQUIRE(spilled->exact_simd_proof.ambiguous_symbols > 0);
    REQUIRE(mps_producer.merge_pass_count() == 2);
    REQUIRE(spill_storage.segment_merge_calls() == 2);
    REQUIRE(spill_storage.segment_merge_run_visits()
        == 2u * spill_storage.segment_spill_run_count());
    REQUIRE(spill_storage.segment_merge_bytes_visited()
        == 2u * spill_storage.segment_spill_bytes());
    require_encoded_generations_equal_normalized(
        *mpf, mpf_sink.rows, *mps, mps_sink.rows);

    const auto mpf_events = decode_generation_events(*mpf, mpf_sink.rows);
    const auto mps_events = decode_generation_events(*mps, mps_sink.rows);
    REQUIRE(mpf_events == oracle);
    REQUIRE(mps_events == oracle);
    REQUIRE(std::ranges::any_of(mpf_events, [](const auto& event) {
        return (event.encoded_position >> 1u) > target_size - 40u;
    }));
    const auto baseline_lookup = canonical_event_lookup_map(oracle);
    REQUIRE(canonical_event_lookup_map(mpf_events) == baseline_lookup);
    REQUIRE(canonical_event_lookup_map(mps_events) == baseline_lookup);
    for (const CanonicalMinimizerEvent& event : mpf_events) {
        const std::uint64_t last = event.encoded_position >> 1u;
        const std::uint64_t first = last + 1u >= 15u
            ? last + 1u - 15u : 0u;
        REQUIRE(std::ranges::none_of(n_positions, [first, last](std::size_t n) {
            return first <= n && n <= last;
        }));
    }
    require_complete_generation_rows(*mpf, mpf_sink.rows);
    require_complete_generation_rows(*mps, mps_sink.rows);
}

TEST_CASE("DOC0371 segment overflow retry halves deterministically",
          "[indexing][minimizer-hashmap][wire][encoder][geometry]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t preferred = kMinimizerPreferredSegmentSpanBp;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> distributed{
        {1, 2u},
        {2, 4u},
        {3, ((preferred / 2u + 1u) << 1u)},
        {4, ((preferred / 2u + 2u) << 1u)},
    };
    const MinimizerGenerationFixedWidthLimits two_per_core{
        .max_segment_control_count = 2,
        .max_segment_position_count = 2,
        .max_hash_position_count = 2,
    };
    VectorReplayMinimizerProducer producer{distributed};
    const auto selected = plan_minimizer_generation(
        producer, test_generation_options(preferred), two_per_core);
    REQUIRE(selected.has_value());
    REQUIRE(selected->position_segment_span_bp == preferred / 2u);
    REQUIRE(producer.scan_count == 2);

    const std::vector<std::pair<std::uint64_t, std::uint64_t>> indivisible{
        {9, 2u}, {9, 3u},
    };
    const MinimizerGenerationFixedWidthLimits one_per_hash{
        .max_segment_control_count = 2,
        .max_segment_position_count = 2,
        .max_hash_position_count = 1,
    };
    VectorReplayMinimizerProducer impossible_producer{indivisible};
    const auto impossible = plan_minimizer_generation(
        impossible_producer,
        test_generation_options(preferred),
        one_per_hash);
    require_wire_error(impossible, MinimizerGenerationWireError::element_count);
}

TEST_CASE("DOC0371 actual MPS producer reroutes once for overflow halving",
          "[indexing][minimizer-hashmap][wire][encoder][geometry][mps]") {
    using namespace bioseq::indexing;
    using MpsIndex = MinimizerSequenceMergedIndex<FakeFragmentStorage<>>;
    constexpr std::uint64_t preferred = kMinimizerPreferredSegmentSpanBp;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> run{
        {1, 2u},
        {2, 4u},
        {3, ((preferred / 2u + 1u) << 1u)},
        {4, ((preferred / 2u + 2u) << 1u)},
    };
    FakeFragmentStorage<> storage;
    storage.spill_sorted_segment_run(
        0,
        std::span<const std::pair<std::uint64_t, std::uint64_t>>{run});
    const std::uint64_t spill_bytes = storage.segment_spill_bytes();
    REQUIRE(spill_bytes == run.size() * sizeof(run.front()));

    const MpsIndex::GenerationSpillResult spill_identity{
        .run_count = 1,
        .minimizer_scheme = kLegacyMinimapV1SchemeIdentity,
        .target_length = preferred,
        .position_segment_span_bp = preferred,
    };
    auto producer = MpsIndex::make_generation_replay_producer(
        storage, preferred, spill_identity, preferred);
    CollectingMinimizerRowSink sink;
    const MinimizerGenerationFixedWidthLimits two_per_core{
        .max_segment_control_count = 2,
        .max_segment_position_count = 2,
        .max_hash_position_count = 1,
    };
    const auto encoded = encode_minimizer_generation(
        producer,
        sink,
        test_generation_options(preferred),
        two_per_core);
    REQUIRE(encoded.has_value());
    REQUIRE(encoded->manifest.position_segment_span_bp == preferred / 2u);
    REQUIRE(encoded->manifest.position_segment_count == 2);
    REQUIRE(producer.scan_count() == 3); // failed 8MiB plan, 4MiB plan, emit
    REQUIRE(producer.merge_pass_count() == 3);
    REQUIRE(producer.reroute_pass_count() == 1);
    REQUIRE(storage.segment_reroute_calls() == 1);
    REQUIRE(storage.segment_reroute_run_visits() == 1);
    REQUIRE(storage.segment_reroute_bytes_visited() == spill_bytes);
    REQUIRE(storage.segment_runs_.size() == 2);
    REQUIRE(storage.segment_spill_run_count() == 2);
    REQUIRE(storage.segment_merge_calls() == 3);
    REQUIRE(storage.segment_merge_run_visits()
        == 1u + 2u * storage.segment_spill_run_count());
    REQUIRE(storage.segment_merge_bytes_visited() == 3u * spill_bytes);
    require_complete_generation_rows(*encoded, sink.rows);
}

TEST_CASE("DOC0371 actual MPS producer accepts empty target without spill work",
          "[indexing][minimizer-hashmap][wire][encoder][geometry][mps]") {
    using namespace bioseq::indexing;
    using MpsIndex = MinimizerSequenceMergedIndex<FakeFragmentStorage<>>;
    FakeFragmentStorage<> storage;
    const MpsIndex::GenerationSpillResult spill_identity{
        .minimizer_scheme = kLegacyMinimapV1SchemeIdentity,
        .target_length = 0,
        .position_segment_span_bp = kMinimizerPreferredSegmentSpanBp,
    };
    auto producer = MpsIndex::make_generation_replay_producer(
        storage, 0, spill_identity, kMinimizerPreferredSegmentSpanBp);
    CollectingMinimizerRowSink sink;
    const auto encoded = encode_minimizer_generation(
        producer, sink, test_generation_options(0));
    REQUIRE(encoded.has_value());
    REQUIRE(encoded->manifest.position_segment_span_bp == 0);
    REQUIRE(encoded->manifest.position_segment_count == 0);
    REQUIRE(encoded->directory.entries().empty());
    REQUIRE(producer.scan_count() == 2);
    REQUIRE(producer.merge_pass_count() == 0);
    REQUIRE(producer.reroute_pass_count() == 0);
    REQUIRE(storage.segment_merge_calls() == 0);
    REQUIRE(storage.segment_reroute_calls() == 0);
    require_complete_generation_rows(*encoded, sink.rows);
}

TEST_CASE("DOC0371 exact duplicate canonical events fail before row publication",
          "[indexing][minimizer-hashmap][wire][encoder][corruption]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> duplicated{
        {7, 20}, {7, 20},
    };
    VectorReplayMinimizerProducer producer{duplicated};
    CollectingMinimizerRowSink sink;
    const auto encoded = encode_minimizer_generation(
        producer, sink, test_generation_options(100));
    require_wire_error(encoded, MinimizerGenerationWireError::duplicate_event);
    REQUIRE(sink.rows.empty());
}

TEST_CASE("DOC0371 encoder rejects a drifting replay before READY publication",
          "[indexing][minimizer-hashmap][wire][encoder][corruption]") {
    using namespace bioseq::indexing;
    DriftingReplayMinimizerProducer producer;
    CollectingMinimizerRowSink sink;
    const auto encoded = encode_minimizer_generation(
        producer, sink, test_generation_options(100));
    require_wire_error(
        encoded, MinimizerGenerationWireError::replay_mismatch);
    REQUIRE(producer.scan_count == 2);
    REQUIRE(sink.rows.empty());
}

TEST_CASE("DOC0371 heavy hash is never split across partitions or page extents",
          "[indexing][minimizer-hashmap][wire][encoder][heavy-hitter]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t target_length = 100000;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers;
    minimizers.reserve(40001);
    for (std::uint64_t index = 0; index < 40000; ++index) {
        minimizers.emplace_back(42, (index << 1u) | (index & 1u));
    }
    minimizers.emplace_back(43, 90000u << 1u);
    VectorReplayMinimizerProducer producer{minimizers};
    CollectingMinimizerRowSink sink;
    auto generation = encode_minimizer_generation(
        producer, sink, test_generation_options(target_length));
    REQUIRE(generation.has_value());
    REQUIRE(generation->directory.entries().size() == 2);
    const auto& heavy = generation->directory.entries()[0];
    REQUIRE(heavy.lo_hash == 42);
    REQUIRE(heavy.hi_hash == 42);
    REQUIRE(heavy.control_element_count == 1);
    REQUIRE(heavy.position_element_count == 40000);
    REQUIRE(heavy.position_encoded_bytes > kMinimizerPartitionTargetBytes);
    REQUIRE(heavy.position_chunk_count > 1);
    const auto heavy_positions = collect_encoded_extent(
        sink.rows, heavy, false);
    REQUIRE(heavy_positions.size() == 40000u * 8u);
    REQUIRE(generation->directory.entries()[1].lo_hash == 43);
    REQUIRE(sink.max_callback_bytes == kMinimizerExtentPayloadBytes);
    require_complete_generation_rows(*generation, sink.rows);
}

TEST_CASE("DOC0371 prepared segment probes header-free controls under one permit",
          "[indexing][minimizer-hashmap][wire][prepared]") {
    using namespace bioseq::indexing;
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<PreparedSerializedSegment>);
    constexpr std::uint64_t span = kMinimizerPreferredSegmentSpanBp;
    constexpr auto encoded_position = [](std::uint64_t position, std::uint64_t strand) {
        return (position << 1u) | strand;
    };
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {100, encoded_position(3, 0)},
        {100, encoded_position(9, 1)},
        {7, encoded_position(span - 1u, 0)},
        {100, encoded_position(span, 1)},
        {9, encoded_position(span + 3u, 0)},
    };
    VectorReplayMinimizerProducer producer{minimizers};
    CollectingMinimizerRowSink sink;
    auto generation = encode_minimizer_generation(
        producer, sink, test_generation_options(span + 100u));
    REQUIRE(generation.has_value());

    bioseq::parallelism::MemoryAdmissionController controller;
    for (std::int32_t segment = 0; segment < 2; ++segment) {
        std::vector<std::uint32_t> ordinals;
        for (const MinimizerBucketDirEntry& entry : generation->directory.entries()) {
            if (entry.position_segment != segment) {
                continue;
            }
            ordinals.push_back(entry.partition_id);
        }
        const auto required = PreparedSerializedSegment::required_permit_bytes(
            generation->directory.encoded_bytes(), segment, ordinals);
        REQUIRE(required.has_value());
        {
            auto prepared = PreparedSerializedSegment::begin_load(
                controller,
                generation->head,
                generation->manifest,
                test_expected_compatibility(),
                generation->directory.encoded_bytes(),
                segment,
                ordinals);
            REQUIRE(prepared.has_value());
            REQUIRE(controller.reserved_bytes() == *required);
            for (const CollectedMinimizerRow& row : sink.rows) {
                const bool selected = std::ranges::any_of(
                    ordinals,
                    [&generation, &row](std::uint32_t ordinal) {
                        const auto* entry =
                            generation->directory.entry_by_ordinal(ordinal);
                        return entry != nullptr
                            && row.address.position_segment
                                == entry->position_segment
                            && row.address.sub_key == entry->sub_key
                            && row.address.chunk_no
                                < entry->control_chunk_count
                                    + entry->position_chunk_count;
                    });
                if (!selected) continue;
                REQUIRE(prepared->accept_chunk(
                    row.address,
                    reinterpret_cast<const char*>(row.bytes.data()),
                    row.bytes.size()).has_value());
            }
            REQUIRE(prepared->finish_load().has_value());
            REQUIRE(prepared->target_length() == span + 100u);
            REQUIRE(prepared->position_segment() == segment);
            if (segment == 0) {
                REQUIRE(std::ranges::equal(
                    prepared->lookup(100),
                    std::array{
                        encoded_position(3, 0), encoded_position(9, 1)}));
                REQUIRE(std::ranges::equal(
                    prepared->lookup(7),
                    std::array{encoded_position(span - 1u, 0)}));
            } else {
                REQUIRE(std::ranges::equal(
                    prepared->lookup(100),
                    std::array{encoded_position(span, 1)}));
                REQUIRE(std::ranges::equal(
                    prepared->lookup(9),
                    std::array{encoded_position(span + 3u, 0)}));
            }
            REQUIRE(prepared->lookup(999).empty());
            PreparedSerializedSegment moved{std::move(*prepared)};
            REQUIRE(moved.target_length() == span + 100u);
            REQUIRE(controller.reserved_bytes() == *required);
        }
        REQUIRE(controller.reserved_bytes() == 0);
    }

    const auto& first = generation->directory.entries().front();
    const std::array ordinal{first.partition_id};
    const auto required = PreparedSerializedSegment::required_permit_bytes(
        generation->directory.encoded_bytes(), 0, ordinal);
    REQUIRE(required.has_value());
    auto wrong_scheme = test_expected_compatibility();
    wrong_scheme.minimizer_scheme_id = kPaperNtHashU32R7SumV1SchemeId;
    wrong_scheme.minimizer_scheme_version =
        kPaperNtHashU32R7SumV1SchemeVersion;
    auto cross_read = PreparedSerializedSegment::begin_load(
        controller,
        generation->head,
        generation->manifest,
        wrong_scheme,
        generation->directory.encoded_bytes(),
        0,
        ordinal);
    require_wire_error(
        cross_read, MinimizerGenerationWireError::compatibility_mismatch);
    REQUIRE(controller.reserved_bytes() == 0);

    controller.set_index_memory_cap_bytes(*required - 1u);
    auto rejected = PreparedSerializedSegment::begin_load(
        controller,
        generation->head,
        generation->manifest,
        test_expected_compatibility(),
        generation->directory.encoded_bytes(),
        0,
        ordinal);
    require_wire_error(rejected, MinimizerGenerationWireError::admission);
    REQUIRE(controller.reserved_bytes() == 0);
    controller.set_index_memory_cap_bytes(0);

    auto wrong_generation = PreparedSerializedSegment::begin_load(
        controller,
        generation->head,
        generation->manifest,
        test_expected_compatibility(),
        generation->directory.encoded_bytes(),
        0,
        ordinal);
    REQUIRE(wrong_generation.has_value());
    const auto first_payload = std::find_if(
        sink.rows.begin(), sink.rows.end(),
        [first](const CollectedMinimizerRow& row) {
            return row.address.position_segment == first.position_segment
                && row.address.sub_key == first.sub_key;
        });
    REQUIRE(first_payload != sink.rows.end());
    auto wrong_address = first_payload->address;
    ++wrong_address.generation_id;
    require_wire_error(
        wrong_generation->accept_chunk(
            wrong_address,
            reinterpret_cast<const char*>(first_payload->bytes.data()),
            first_payload->bytes.size()),
        MinimizerGenerationWireError::chunk_range);
}

TEST_CASE("DOC0371 prepared callbacks reject control and position corruption",
          "[indexing][minimizer-hashmap][wire][prepared][corruption]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {1, 2}, {1, 4}, {2, 6},
    };
    VectorReplayMinimizerProducer producer{minimizers};
    CollectingMinimizerRowSink sink;
    auto generation = encode_minimizer_generation(
        producer, sink, test_generation_options(100));
    REQUIRE(generation.has_value());
    REQUIRE(generation->directory.entries().size() == 1);
    const auto original_entry = generation->directory.entries().front();
    const auto original_controls = collect_encoded_extent(
        sink.rows, original_entry, true);
    const auto original_positions = collect_encoded_extent(
        sink.rows, original_entry, false);

    const auto expect_finish_error = [
            &generation,
            original_entry](
                std::vector<std::byte> controls,
                std::vector<std::byte> positions,
                MinimizerGenerationWireError expected) {
        auto entry = original_entry;
        entry.control_checksum = crc64_ecma182(controls);
        entry.position_checksum = crc64_ecma182(positions);
        const std::array entries{entry};
        const auto metadata = rebuild_generation_metadata(*generation, entries);
        bioseq::parallelism::MemoryAdmissionController controller;
        const std::array ordinal{entry.partition_id};
        auto prepared = PreparedSerializedSegment::begin_load(
            controller,
            metadata.head,
            metadata.manifest,
            test_expected_compatibility(),
            metadata.directory_bytes,
            entry.position_segment,
            ordinal);
        REQUIRE(prepared.has_value());
        for (std::uint32_t chunk = 0; chunk < entry.control_chunk_count; ++chunk) {
            const std::size_t offset =
                static_cast<std::size_t>(chunk) * kMinimizerExtentPayloadBytes;
            const std::size_t count = std::min<std::size_t>(
                kMinimizerExtentPayloadBytes, controls.size() - offset);
            REQUIRE(prepared->accept_chunk(
                IndexChunkAddress{
                    .generation_id = metadata.manifest.generation_id,
                    .position_segment = entry.position_segment,
                    .sub_key = entry.sub_key,
                    .chunk_no = chunk,
                },
                reinterpret_cast<const char*>(controls.data() + offset),
                count).has_value());
        }
        for (std::uint32_t chunk = 0; chunk < entry.position_chunk_count; ++chunk) {
            const std::size_t offset =
                static_cast<std::size_t>(chunk) * kMinimizerExtentPayloadBytes;
            const std::size_t count = std::min<std::size_t>(
                kMinimizerExtentPayloadBytes, positions.size() - offset);
            REQUIRE(prepared->accept_chunk(
                IndexChunkAddress{
                    .generation_id = metadata.manifest.generation_id,
                    .position_segment = entry.position_segment,
                    .sub_key = entry.sub_key,
                    .chunk_no = entry.position_first_chunk + chunk,
                },
                reinterpret_cast<const char*>(positions.data() + offset),
                count).has_value());
        }
        require_wire_error(
            prepared->finish_load(), expected);
    };

    SECTION("zero-position control") {
        auto controls = original_controls;
        test_write_le<std::uint32_t>(controls, 12, 0);
        expect_finish_error(
            std::move(controls), original_positions,
            MinimizerGenerationWireError::control_range);
    }
    SECTION("non-contiguous local offset") {
        auto controls = original_controls;
        test_write_le<std::uint32_t>(controls, 16u + 8u, 1u);
        expect_finish_error(
            std::move(controls), original_positions,
            MinimizerGenerationWireError::control_range);
    }
    SECTION("unsorted positions") {
        auto positions = original_positions;
        std::swap_ranges(
            positions.begin(), positions.begin() + 8u,
            positions.begin() + 8u);
        expect_finish_error(
            original_controls, std::move(positions),
            MinimizerGenerationWireError::position_range);
    }
    SECTION("position outside fixed core") {
        auto positions = original_positions;
        test_write_le<std::uint64_t>(
            positions, 0, generation->manifest.target_length << 1u);
        expect_finish_error(
            original_controls, std::move(positions),
            MinimizerGenerationWireError::position_range);
    }
    SECTION("extent checksum") {
        auto controls = original_controls;
        controls[0] ^= std::byte{1};
        bioseq::parallelism::MemoryAdmissionController controller;
        const std::array ordinal{original_entry.partition_id};
        auto prepared = PreparedSerializedSegment::begin_load(
            controller,
            generation->head,
            generation->manifest,
            test_expected_compatibility(),
            generation->directory.encoded_bytes(),
            original_entry.position_segment,
            ordinal);
        REQUIRE(prepared.has_value());
        REQUIRE(prepared->accept_chunk(
            IndexChunkAddress{
                .generation_id = generation->manifest.generation_id,
                .position_segment = original_entry.position_segment,
                .sub_key = original_entry.sub_key,
                .chunk_no = 0,
            },
            reinterpret_cast<const char*>(controls.data()),
            controls.size()).has_value());
        require_wire_error(
            prepared->accept_chunk(
                IndexChunkAddress{
                    .generation_id = generation->manifest.generation_id,
                    .position_segment = original_entry.position_segment,
                    .sub_key = original_entry.sub_key,
                    .chunk_no = original_entry.position_first_chunk,
                },
                reinterpret_cast<const char*>(original_positions.data()),
                original_positions.size()),
            MinimizerGenerationWireError::checksum);
    }
    SECTION("ordinal order") {
        const std::array duplicated_ordinals{
            original_entry.partition_id, original_entry.partition_id};
        require_wire_error(
            PreparedSerializedSegment::required_permit_bytes(
                generation->directory.encoded_bytes(),
                original_entry.position_segment,
                duplicated_ordinals),
            MinimizerGenerationWireError::directory_order);
    }
}

TEST_CASE("DOC0371 cross-object metadata equations fail closed",
          "[indexing][minimizer-hashmap][wire][corruption]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {1, 2}, {2, 4}, {2, 6},
    };
    VectorReplayMinimizerProducer producer{minimizers};
    CollectingMinimizerRowSink sink;
    auto generation = encode_minimizer_generation(
        producer, sink, test_generation_options(100));
    REQUIRE(generation.has_value());

    auto bad_head = generation->head;
    bad_head.source_identity ^= 1u;
    require_wire_error(
        validate_minimizer_generation_metadata(
            bad_head, generation->manifest, generation->directory),
        MinimizerGenerationWireError::identity_mismatch);

    bad_head = generation->head;
    bad_head.compatibility_checksum ^= 1u;
    require_wire_error(
        validate_minimizer_generation_metadata(
            bad_head, generation->manifest, generation->directory),
        MinimizerGenerationWireError::compatibility_mismatch);

    auto bad_manifest = generation->manifest;
    ++bad_manifest.control_count;
    const auto bad_manifest_bytes = encode_minimizer_generation_manifest(bad_manifest);
    REQUIRE(bad_manifest_bytes.has_value());
    bad_head = generation->head;
    bad_head.manifest_checksum = crc64_ecma182(
        std::span<const std::byte>{*bad_manifest_bytes});
    require_wire_error(
        validate_minimizer_generation_metadata(
            bad_head, bad_manifest, generation->directory),
        MinimizerGenerationWireError::element_count);

    SECTION("validator factory rejects cross-object metadata before rows") {
        auto altered = *generation;
        ++altered.manifest.control_count;
        refresh_test_generation_fixed_rows(altered);
        require_wire_error(
            MinimizerGenerationRowValidator::create(altered),
            MinimizerGenerationWireError::element_count);
    }
    SECTION("validator factory rejects fixed-row CRC before rows") {
        auto altered = *generation;
        altered.manifest_row.row_checksum ^= 1u;
        require_wire_error(
            MinimizerGenerationRowValidator::create(altered),
            MinimizerGenerationWireError::checksum);
    }

    SECTION("qualified row checksum") {
        auto rows = sink.rows;
        rows.front().row_checksum ^= 1u;
        auto validator = make_generation_row_validator(*generation);
        require_wire_error(
            validator.accept(row_view(rows.front())),
            MinimizerGenerationWireError::checksum);
    }
    SECTION("row bytes with self-consistent row CRC still fail extent CRC") {
        auto row = sink.rows.front();
        row.bytes.front() ^= std::byte{1};
        row.row_checksum = crc64_index_chunk_row(
            row.address.generation_id,
            row.address.position_segment,
            row.address.sub_key,
            row.address.chunk_no,
            static_cast<std::uint32_t>(row.bytes.size()),
            row.bytes);
        auto validator = make_generation_row_validator(*generation);
        require_wire_error(
            validator.accept(row_view(row)),
            MinimizerGenerationWireError::checksum);
    }
    SECTION("qualified address ordinal") {
        auto row = sink.rows.front();
        ++row.address.chunk_no;
        auto validator = make_generation_row_validator(*generation);
        require_wire_error(
            validator.accept(row_view(row)),
            MinimizerGenerationWireError::row_order);
    }
    SECTION("qualified row order and ordinal") {
        auto rows = sink.rows;
        REQUIRE(rows.size() > 3);
        std::swap(rows[0], rows[1]);
        auto validator = make_generation_row_validator(*generation);
        require_wire_error(
            validator.accept(row_view(rows.front())),
            MinimizerGenerationWireError::row_order);
    }
    SECTION("missing qualified row") {
        auto rows = sink.rows;
        rows.erase(rows.begin());
        auto validator = make_generation_row_validator(*generation);
        for (const auto& row : rows) {
            const auto accepted = validator.accept(row_view(row));
            if (!accepted) break;
        }
        require_wire_error(
            validator.finish_ready(), MinimizerGenerationWireError::row_count);
    }
    SECTION("payload checksum aggregate") {
        auto altered = *generation;
        altered.manifest.payload_checksum ^= 1u;
        refresh_test_generation_fixed_rows(altered);
        auto rows = sink.rows;
        auto manifest_row = std::ranges::find_if(
            rows,
            [&altered](const CollectedMinimizerRow& row) {
                return row.address == altered.manifest_row.address;
            });
        REQUIRE(manifest_row != rows.end());
        manifest_row->bytes.assign(
            altered.manifest_row.bytes().begin(),
            altered.manifest_row.bytes().end());
        manifest_row->row_checksum = altered.manifest_row.row_checksum;
        auto validator = make_generation_row_validator(altered);
        for (const auto& row : rows) {
            REQUIRE(validator.accept(row_view(row)).has_value());
        }
        require_wire_error(
            validator.finish_ready(), MinimizerGenerationWireError::checksum);
    }
}

TEST_CASE("DOC0279 §9 — MinimizerHashmapFragmentIndex build(vector) + lookup",
          "[indexing][minimizer-hashmap]") {
    // Hand-built sketches; positions kept small so the 28-bit pos field of
    // MinimapSketchData{kmer,pos,strand} is never truncated.
    std::vector<MinimapSketchData> sketches{
        MinimapSketchData{/*kmer*/ 100, /*pos*/ 5,  /*strand*/ 0},
        MinimapSketchData{/*kmer*/ 100, /*pos*/ 9,  /*strand*/ 1},
        MinimapSketchData{/*kmer*/ 7,   /*pos*/ 2,  /*strand*/ 0},
        MinimapSketchData{/*kmer*/ 100, /*pos*/ 1,  /*strand*/ 0},
        MinimapSketchData{/*kmer*/ 42,  /*pos*/ 13, /*strand*/ 1},
    };
    bioseq::indexing::MinimizerHashmapFragmentIndex idx;
    idx.build(sketches, /*target_length*/ 1000);
    REQUIRE(idx.target_length() == 1000);

    auto enc = [](std::uint64_t pos, std::uint64_t strand) { return (pos << 1) | strand; };

    {  // kmer 100 has three positions
        auto span = idx.lookup(100);
        std::vector<std::uint64_t> got(span.begin(), span.end());
        std::sort(got.begin(), got.end());
        std::vector<std::uint64_t> want{enc(1, 0), enc(5, 0), enc(9, 1)};
        std::sort(want.begin(), want.end());
        REQUIRE(got == want);
    }
    {  // kmer 7 single position
        auto span = idx.lookup(7);
        REQUIRE(span.size() == 1u);
        REQUIRE(span[0] == enc(2, 0));
    }
    {  // kmer 42 single position
        auto span = idx.lookup(42);
        REQUIRE(span.size() == 1u);
        REQUIRE(span[0] == enc(13, 1));
    }
    REQUIRE(idx.lookup(/*absent*/ 999).empty());
}

TEST_CASE("transient minimizer index consumes source and releases storage",
          "[indexing][minimizer-hashmap][lifetime]") {
    std::vector<MinimapSketchData> sketches{
        MinimapSketchData{/*kmer*/ 100, /*pos*/ 5,  /*strand*/ 0},
        MinimapSketchData{/*kmer*/ 7,   /*pos*/ 2,  /*strand*/ 0},
        MinimapSketchData{/*kmer*/ 100, /*pos*/ 9,  /*strand*/ 1},
    };
    bioseq::indexing::MinimizerHashmapFragmentIndex idx;
    idx.build_consuming(std::move(sketches), /*target_length*/ 1000);

    REQUIRE(sketches.empty());
    REQUIRE(sketches.capacity() == 0u);
    REQUIRE(idx.lookup(100).size() == 2u);
    REQUIRE(idx.lookup(7).size() == 1u);
    REQUIRE(idx.target_length() == 1000);

    idx.release_storage();
    REQUIRE(idx.table_empty());
    REQUIRE(idx.positions.empty());
    REQUIRE(idx.positions.capacity() == 0u);
    REQUIRE(idx.target_length() == 1000);
}

TEST_CASE("DOC0279 §9 — MinimizerHashmapFragmentIndex serialize/deserialize round-trip",
          "[indexing][minimizer-hashmap]") {
    auto data = make_dna4(2000, 0xC0FFEE);
    range_t range(data);
    auto sketches = whole_sequence_minimizers(range, 10, 15);

    bioseq::indexing::MinimizerHashmapFragmentIndex idx;
    idx.build(sketches, range.bp_size());

    std::vector<char> buf(idx.serialize_binary_size());
    idx.serialize_to_binary(buf.data());
    auto round = bioseq::indexing::MinimizerHashmapFragmentIndex::deserialize_from_binary(
        buf.data(), buf.size());

    REQUIRE(round.target_length() == idx.target_length());
    REQUIRE(round.positions.size() == idx.positions.size());
    REQUIRE(round.table_size() == idx.table_size());

    // Per-kmer position sets must survive the wire round-trip exactly.
    std::vector<std::uint64_t> kmers;
    for (const auto& m : sketches) kmers.push_back(m.kmer());
    REQUIRE(lookup_map(round, kmers) == lookup_map(idx, kmers));
}

TEST_CASE("DOC0279 §9.4 — build_from_pairs equals build(vector) over equivalent pairs",
          "[indexing][minimizer-hashmap]") {
    auto data = make_dna4(3000, 0x5EED);
    range_t range(data);
    auto sketches = whole_sequence_minimizers(range, 10, 15);

    bioseq::indexing::MinimizerHashmapFragmentIndex via_vector;
    via_vector.build(sketches, range.bp_size());

    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
    pairs.reserve(sketches.size());
    for (const auto& m : sketches) pairs.emplace_back(m.kmer(), m.pos_strand_encoded());

    bioseq::indexing::MinimizerHashmapFragmentIndex via_pairs;
    via_pairs.build_from_pairs(std::span<std::pair<std::uint64_t, std::uint64_t>>(pairs), range.bp_size());

    REQUIRE(via_pairs.target_length() == via_vector.target_length());
    REQUIRE(via_pairs.positions.size() == via_vector.positions.size());

    std::vector<std::uint64_t> kmers;
    for (const auto& m : sketches) kmers.push_back(m.kmer());
    REQUIRE(lookup_map(via_pairs, kmers) == lookup_map(via_vector, kmers));
}

TEST_CASE("DOC0279 §9 — assemble_segment over fragmented storage equals whole-sequence build",
          "[indexing][minimizer-hashmap][assemble]") {
    // frag_units = 1024 -> frag_bp = 1024 * 32 = 32768; a ~40000 bp dna4
    // sequence therefore spans 2 fragments (multi-fragment build path).
    constexpr std::size_t FragUnits = 1024;
    constexpr std::size_t frag_bp   = FragUnits * 32;   // 32768
    using chunked = sequence_scanner_test_chunked<std::uint64_t, FragUnits>;

    auto data = make_dna4(40000, 0xABC123);
    range_t range(data);
    const auto total_bp = range.bp_size();
    const std::size_t nfrag = (std::size_t)((total_bp + frag_bp - 1) / frag_bp);
    REQUIRE(nfrag >= 2u);   // multi-fragment

    FakeFragmentStorage<> storage;
    lazy_compact_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
    bioseq::indexing::MinimizersPerFragmentIndex<FakeFragmentStorage<>>::build(storage, lazy);

    bioseq::indexing::MinimizerAssemblyScratch scratch;
    auto assembled = bioseq::indexing::MinimizersPerFragmentIndex<FakeFragmentStorage<>>::assemble_segment(
        storage, /*frag_lo*/ 0, /*frag_hi*/ nfrag, /*frag_bp*/ (bioseq::storage::sequence_size_t)frag_bp,
        /*total_bp*/ total_bp, /*clip_lo*/ 0, /*clip_hi*/ total_bp, scratch);

    // The whole-sequence baseline: a single MinimizerHashmapFragmentIndex over
    // every minimizer collected from the whole range.
    auto whole = whole_sequence_minimizers(range, 10, 15);
    bioseq::indexing::MinimizerHashmapFragmentIndex baseline;
    baseline.build(whole, total_bp);

    REQUIRE(assembled.target_length() == total_bp);

    // dna4 (no N) => EXACT set equality of (kmer -> sorted positions). Build the
    // union of kmers from both sides and compare their lookup maps.
    std::vector<std::uint64_t> kmers;
    for (const auto& m : whole) kmers.push_back(m.kmer());
    REQUIRE(lookup_map(assembled, kmers) == lookup_map(baseline, kmers));
}

TEST_CASE("ExactSimd MPF fragmented build preserves scalar segment lookups",
          "[indexing][minimizer-hashmap][exact-simd][fragmented][parity]") {
    constexpr std::size_t FragUnits = 2;
    constexpr std::size_t frag_bp = FragUnits * 32;
    using chunked = sequence_scanner_test_chunked<std::uint64_t, FragUnits>;
    using Index =
        bioseq::indexing::MinimizersPerFragmentIndex<FakeFragmentStorage<>>;

    auto data = make_dna4(4099, 0xE2AC751DULL);
    range_t range(data);
    const auto total_bp = range.bp_size();
    const std::size_t nfrag = static_cast<std::size_t>(
        (total_bp + frag_bp - 1) / frag_bp);

    FakeFragmentStorage<> scalar_storage;
    FakeFragmentStorage<> exact_storage;
    lazy_compact_sequence<dna4_u64, chunked> scalar_lazy{
        chunked::from_range(range)};
    lazy_compact_sequence<dna4_u64, chunked> exact_lazy{
        chunked::from_range(range)};
    Index::build(
        scalar_storage, scalar_lazy, MinimizerSketchBackend::Scalar);
    Index::build(
        exact_storage, exact_lazy, MinimizerSketchBackend::ExactSimd);

    bioseq::indexing::MinimizerAssemblyScratch scalar_scratch;
    bioseq::indexing::MinimizerAssemblyScratch exact_scratch;
    auto scalar = Index::assemble_segment(
        scalar_storage,
        0,
        nfrag,
        static_cast<bioseq::storage::sequence_size_t>(frag_bp),
        total_bp,
        0,
        total_bp,
        scalar_scratch);
    auto exact = Index::assemble_segment(
        exact_storage,
        0,
        nfrag,
        static_cast<bioseq::storage::sequence_size_t>(frag_bp),
        total_bp,
        0,
        total_bp,
        exact_scratch);

    auto whole = whole_sequence_minimizers(range, 10, 15);
    std::vector<std::uint64_t> kmers;
    kmers.reserve(whole.size());
    for (const auto& minimizer : whole) kmers.push_back(minimizer.kmer());
    REQUIRE(lookup_map(exact, kmers) == lookup_map(scalar, kmers));
}

}  // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
