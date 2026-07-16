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
#include <iterator>
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
import bioseq.gpu.vulkan.runtime;
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
using dna4_u16 = sequence_custom_alphabet<seq::dna4, std::uint16_t>;
using dna4_u16_range_t = compact_sequence_range<dna4_u16>;
using dna5_u64 = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
using dna5_range_t = compact_sequence_range<dna5_u64>;

inline constexpr auto kTestV3LegacyProfileResult =
    bioseq::indexing::minimizer_generation_payload_profile(
        bioseq::indexing::MinimizerGenerationWireFormat::
            sorted_dense_global64,
        bioseq::indexing::kLegacyMinimapV1SchemeIdentity,
        15u);
static_assert(kTestV3LegacyProfileResult.has_value());
inline constexpr auto kTestV3LegacyProfile =
    *kTestV3LegacyProfileResult;

inline constexpr auto kTestV4LegacyProfileResult =
    bioseq::indexing::minimizer_generation_payload_profile(
        bioseq::indexing::MinimizerGenerationWireFormat::
            grouped_hash32_segment_local32,
        bioseq::indexing::kLegacyMinimapV1SchemeIdentity,
        15u);
static_assert(kTestV4LegacyProfileResult.has_value());
inline constexpr auto kTestV4LegacyProfile =
    *kTestV4LegacyProfileResult;

inline constexpr auto kTestV4PaperProfileResult =
    bioseq::indexing::minimizer_generation_payload_profile(
        bioseq::indexing::MinimizerGenerationWireFormat::
            grouped_hash32_segment_local32,
        bioseq::indexing::kPaperNtHashU32R7SumV1SchemeIdentity,
        15u);
static_assert(kTestV4PaperProfileResult.has_value());
inline constexpr auto kTestV4PaperProfile =
    *kTestV4PaperProfileResult;

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
static_assert(std::is_standard_layout_v<
    bioseq::indexing::PackedPaperNthashIndexBridgeRequest>);
static_assert(std::is_trivially_copyable_v<
    bioseq::indexing::PackedPaperNthashIndexBridgeRequest>);
static_assert(std::is_standard_layout_v<
    bioseq::indexing::PackedPaperNthashIndexBridgeResult>);
static_assert(std::is_same_v<
    decltype(bioseq::indexing::collect_packed_paper_nthash_index_erased(
        std::declval<const
            bioseq::indexing::PackedPaperNthashIndexBridgeRequest&>())),
    bioseq::indexing::PackedPaperNthashIndexBridgeResult>);

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
        .wire_format = bioseq::indexing::MinimizerGenerationWireFormat::
            sorted_dense_global64,
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
        .wire_format = bioseq::indexing::MinimizerGenerationWireFormat::
            sorted_dense_global64,
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
        .producer_backend_id = bioseq::indexing::
            MinimizerProducerBackendId::scalar,
        .producer_backend_identity_version = bioseq::indexing::
            kMinimizerProducerBackendIdentityVersion,
    };
}

[[nodiscard]] constexpr bioseq::indexing::MinimizerGenerationEncodeOptions
test_generation_options(std::uint64_t target_length) noexcept {
    return {
        .wire_format = bioseq::indexing::MinimizerGenerationWireFormat::
            sorted_dense_global64,
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

[[nodiscard]] constexpr bioseq::indexing::MinimizerGenerationEncodeOptions
test_grouped32_generation_options(std::uint64_t target_length) noexcept {
    auto options = test_generation_options(target_length);
    options.wire_format = bioseq::indexing::MinimizerGenerationWireFormat::
        grouped_hash32_segment_local32;
    return options;
}

[[nodiscard]] constexpr bioseq::indexing::MinimizerGenerationEncodeOptions
test_shape_v5_generation_options(
        std::uint64_t target_length,
        std::uint64_t max_occ_cap = 65'534u) noexcept {
    auto options = test_generation_options(target_length);
    options.wire_format = bioseq::indexing::MinimizerGenerationWireFormat::
        grouped_shape_soa_segment_local;
    options.alphabet_code =
        bioseq::indexing::kMinimizerPersistedDna4AlphabetCode;
    options.max_occ_cap = max_occ_cap;
    return options;
}

[[nodiscard]] constexpr bioseq::indexing::ExpectedMinimizerCompatibility
test_expected_compatibility() noexcept {
    return {
        .wire_format = bioseq::indexing::MinimizerGenerationWireFormat::
            sorted_dense_global64,
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
        .producer_backend_id = bioseq::indexing::
            MinimizerProducerBackendId::scalar,
        .producer_backend_identity_version = bioseq::indexing::
            kMinimizerProducerBackendIdentityVersion,
    };
}

[[nodiscard]] constexpr bioseq::indexing::ExpectedMinimizerCompatibility
test_grouped32_expected_compatibility() noexcept {
    return {
        .wire_format = bioseq::indexing::MinimizerGenerationWireFormat::
            grouped_hash32_segment_local32,
        .alphabet_code = 4,
        .storage_unit_byte_width = 8,
        .kmer_size = 15,
        .window_size = 10,
        .gpu_abi_version =
            bioseq::indexing::kMinimizerGrouped32GpuAbiVersion,
        .minimizer_scheme_id =
            bioseq::indexing::kLegacyMinimapV1SchemeId,
        .minimizer_scheme_version =
            bioseq::indexing::kLegacyMinimapV1SchemeVersion,
        .producer_backend_id = bioseq::indexing::
            MinimizerProducerBackendId::exact_simd,
        .producer_backend_identity_version = bioseq::indexing::
            kMinimizerProducerBackendIdentityVersion,
    };
}

[[nodiscard]] constexpr bioseq::indexing::ExpectedMinimizerCompatibility
test_shape_v5_expected_compatibility(
        std::uint64_t max_occ_cap = 65'534u) noexcept {
    auto expected = test_expected_compatibility();
    expected.wire_format = bioseq::indexing::MinimizerGenerationWireFormat::
        grouped_shape_soa_segment_local;
    expected.alphabet_code =
        bioseq::indexing::kMinimizerPersistedDna4AlphabetCode;
    expected.gpu_abi_version =
        bioseq::indexing::kMinimizerShapeV5GpuAbiVersion;
    expected.max_occ_cap = max_occ_cap;
    return expected;
}

struct CollectedMinimizerRow {
    bioseq::indexing::IndexChunkAddress address{};
    std::vector<std::byte> bytes;
    std::uint64_t row_checksum = 0;
};

struct CollectingMinimizerRowSink {
    std::vector<CollectedMinimizerRow> rows;
    std::size_t max_callback_bytes = 0;
    std::uint64_t format5_payload_write_calls = 0;
    std::uint64_t format5_row_crc_bytes = 0;

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

    [[nodiscard]] std::expected<
        bioseq::indexing::MinimizerGenerationRowWriteReceipt,
        bioseq::indexing::MinimizerGenerationWireError>
    write_format5_payload(
            const bioseq::indexing::
                UnchecksummedMinimizerGenerationRowView& row) {
        const std::uint64_t row_checksum =
            bioseq::indexing::crc64_index_chunk_row(
                row.address.generation_id,
                row.address.position_segment,
                row.address.sub_key,
                row.address.chunk_no,
                static_cast<std::uint32_t>(row.bytes.size()),
                row.bytes);
        if (auto written = write(
                bioseq::indexing::MinimizerGenerationRowView{
                    .address = row.address,
                    .bytes = row.bytes,
                    .row_checksum = row_checksum,
                }); !written) {
            return std::unexpected(written.error());
        }
        ++format5_payload_write_calls;
        format5_row_crc_bytes += row.bytes.size();
        return bioseq::indexing::MinimizerGenerationRowWriteReceipt{
            .row_checksum = row_checksum,
        };
    }
};

struct VectorReplayMinimizerProducer {
    std::vector<bioseq::indexing::CanonicalMinimizerEvent> events;
    std::size_t scan_count = 0;
    bioseq::indexing::MinimizerSchemeIdentity scheme =
        bioseq::indexing::kLegacyMinimapV1SchemeIdentity;
    bioseq::indexing::MinimizerProducerBackendIdentity producer_backend =
        bioseq::indexing::minimizer_producer_backend_identity_for_backend(
            bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend::Scalar);

    explicit VectorReplayMinimizerProducer(
            std::span<const std::pair<std::uint64_t, std::uint64_t>> pairs,
            bioseq::indexing::MinimizerSchemeIdentity scheme_identity =
                bioseq::indexing::kLegacyMinimapV1SchemeIdentity,
            bioseq::indexing::MinimizerProducerBackendIdentity
                producer_backend_identity = {})
        : scheme(scheme_identity),
          producer_backend(
              bioseq::indexing::
                  is_supported_minimizer_producer_backend_identity(
                      producer_backend_identity)
                  ? producer_backend_identity
                  : bioseq::indexing::
                      minimizer_producer_backend_identity_for_backend(
                          scheme_identity == bioseq::indexing::
                                  kPaperNtHashU32R7SumV1SchemeIdentity
                              ? bioseq::simd::HWY_NAMESPACE::
                                  MinimizerSketchBackend::PaperNtHashSimd
                              : bioseq::simd::HWY_NAMESPACE::
                                  MinimizerSketchBackend::Scalar)) {
        events.reserve(pairs.size());
        for (const auto& pair : pairs) {
            events.push_back({pair.first, pair.second});
        }
    }

    [[nodiscard]] bioseq::indexing::MinimizerSchemeIdentity
    minimizer_scheme_identity() const noexcept {
        return scheme;
    }

    [[nodiscard]] bioseq::indexing::MinimizerProducerBackendIdentity
    producer_backend_identity() const noexcept {
        return producer_backend;
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

    [[nodiscard]] bioseq::indexing::MinimizerProducerBackendIdentity
    producer_backend_identity() const noexcept {
        return bioseq::indexing::
            minimizer_producer_backend_identity_for_backend(
                bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend::Scalar);
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

enum class IdentityDriftAxis : std::uint8_t {
    scheme,
    backend,
};

/// Changes one qualified identity immediately after a selected completed
/// scan. Format-5 has three distinct scans: Plan=1, Control=2, Position=3.
struct BoundaryIdentityDriftingProducer {
    std::size_t scan_count = 0;
    std::size_t drift_after_scan = 1;
    IdentityDriftAxis axis = IdentityDriftAxis::scheme;

    [[nodiscard]] bioseq::indexing::MinimizerSchemeIdentity
    minimizer_scheme_identity() const noexcept {
        if (axis == IdentityDriftAxis::scheme
            && scan_count >= drift_after_scan) {
            return bioseq::indexing::kPaperNtHashU32R7SumV1SchemeIdentity;
        }
        return bioseq::indexing::kLegacyMinimapV1SchemeIdentity;
    }

    [[nodiscard]] bioseq::indexing::MinimizerProducerBackendIdentity
    producer_backend_identity() const noexcept {
        const auto backend = axis == IdentityDriftAxis::backend
                && scan_count >= drift_after_scan
            ? bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend::ExactSimd
            : bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend::Scalar;
        return bioseq::indexing::
            minimizer_producer_backend_identity_for_backend(backend);
    }

    [[nodiscard]] std::expected<void,
        bioseq::indexing::MinimizerGenerationWireError>
    scan_sorted_events(
            std::uint64_t span,
            bioseq::indexing::CanonicalMinimizerEventReceiver receiver) {
        ++scan_count;
        if (span == 0u) {
            return std::unexpected(
                bioseq::indexing::MinimizerGenerationWireError::segment_span);
        }
        return receiver(bioseq::indexing::CanonicalMinimizerEvent{
            .hash = 1u,
            .encoded_position = 2u,
        });
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
    const auto position_sub_key =
        bioseq::indexing::derive_minimizer_position_extent_sub_key(
            entry.sub_key);
    REQUIRE(position_sub_key.has_value());
    const bioseq::indexing::index_sub_key_t physical_sub_key = controls
        ? entry.sub_key : *position_sub_key;
    std::vector<std::byte> bytes;
    for (std::uint32_t chunk = first; chunk < first + count; ++chunk) {
        const auto row = std::find_if(
            rows.begin(), rows.end(),
            [entry, physical_sub_key, chunk](
                    const CollectedMinimizerRow& value) {
                return value.address.position_segment == entry.position_segment
                    && value.address.sub_key == physical_sub_key
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
    // This helper compares canonical biological payloads across physical
    // producers. Producer identity is independently attested by each original
    // head/manifest and is therefore an intentional difference for Scalar
    // versus Exact SIMD comparisons.
    REQUIRE(lhs.head.compatibility_checksum
        == minimizer_generation_compatibility_checksum(lhs.manifest));
    REQUIRE(rhs.head.compatibility_checksum
        == minimizer_generation_compatibility_checksum(rhs.manifest));
    lhs_manifest.producer_backend_id =
        bioseq::indexing::MinimizerProducerBackendId::scalar;
    rhs_manifest.producer_backend_id =
        bioseq::indexing::MinimizerProducerBackendId::scalar;
    lhs_manifest.producer_backend_identity_version =
        bioseq::indexing::kMinimizerProducerBackendIdentityVersion;
    rhs_manifest.producer_backend_identity_version =
        bioseq::indexing::kMinimizerProducerBackendIdentityVersion;
    REQUIRE(lhs_manifest == rhs_manifest);
    REQUIRE(lhs.head.source_identity == rhs.head.source_identity);
    REQUIRE(minimizer_generation_compatibility_checksum(lhs_manifest)
        == minimizer_generation_compatibility_checksum(rhs_manifest));
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

[[nodiscard]] bioseq::indexing::ValidatedMinimizerGenerationRowReceipt
trusted_row_receipt(const CollectedMinimizerRow& row) {
    auto validated = bioseq::indexing::
        validate_minimizer_generation_row_receipt(row_view(row));
    REQUIRE(validated.has_value());
    return std::move(*validated);
}

void recompute_row_checksum(CollectedMinimizerRow& row) noexcept {
    row.row_checksum = bioseq::indexing::crc64_index_chunk_row(
        row.address.generation_id,
        row.address.position_segment,
        row.address.sub_key,
        row.address.chunk_no,
        static_cast<std::uint32_t>(row.bytes.size()),
        row.bytes);
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
    using namespace bioseq::indexing;
    std::vector<bioseq::indexing::CanonicalMinimizerEvent> events;
    const auto& profile = generation.directory.profile();
    for (const auto& entry : generation.directory.entries()) {
        const auto control_bytes = collect_encoded_extent(rows, entry, true);
        const auto position_bytes = collect_encoded_extent(rows, entry, false);
        const auto core = minimizer_position_segment_core(
            generation.manifest,
            static_cast<std::uint32_t>(entry.position_segment));
        REQUIRE(core.has_value());
        if (profile.wire_format
                == MinimizerGenerationWireFormat::
                    grouped_shape_soa_segment_local) {
            const auto controls = decode_minimizer_shape_v5_controls(
                control_bytes, entry, profile);
            REQUIRE(controls.has_value());
            const auto positions = decode_minimizer_shape_v5_positions(
                position_bytes, *controls, entry, *core, profile);
            REQUIRE(positions.has_value());
            for (const auto& control : *controls) {
                for (std::uint64_t position = control.position_begin;
                     position < control.position_end; ++position) {
                    events.push_back({
                        .hash = control.hash,
                        .encoded_position = (*positions)[
                            static_cast<std::size_t>(position)],
                    });
                }
            }
            continue;
        }
        for (std::size_t index = 0;
             index < entry.control_element_count;
             ++index) {
            std::uint64_t hash = 0;
            std::uint64_t position_offset = 0;
            std::uint64_t position_count = 0;
            if (profile.wire_format
                    == MinimizerGenerationWireFormat::sorted_dense_global64) {
                const auto control = decode_minimizer_serialized_bucket(
                    std::span<const std::byte>{control_bytes}.subspan(
                        index * 16u, 16u));
                REQUIRE(control.has_value());
                hash = control->hash;
                position_offset = control->pos_offset;
                position_count = control->pos_count;
            } else {
                REQUIRE(profile.wire_format
                    == MinimizerGenerationWireFormat::
                        grouped_hash32_segment_local32);
                const auto control = decode_minimizer_grouped32_control(
                    std::span<const std::byte>{control_bytes}.subspan(
                        index * kMinimizerGrouped32ControlEncodedBytes,
                        kMinimizerGrouped32ControlEncodedBytes),
                    entry.position_element_count,
                    profile);
                REQUIRE(control.has_value());
                hash = control->hash;
                position_offset = control->position_offset;
                position_count = control->position_count;
            }
            REQUIRE(position_count > 0);
            for (std::uint64_t position = 0;
                 position < position_count;
                 ++position) {
                std::uint64_t encoded_position = 0;
                if (profile.wire_format
                        == MinimizerGenerationWireFormat::
                            sorted_dense_global64) {
                    encoded_position = test_read_le<std::uint64_t>(
                        position_bytes,
                        static_cast<std::size_t>(
                            (position_offset + position) * 8u));
                } else {
                    const std::size_t byte_offset = static_cast<std::size_t>(
                        (position_offset + position)
                        * kMinimizerGrouped32PositionEncodedBytes);
                    const auto decoded = decode_minimizer_grouped32_position(
                        std::span<const std::byte>{position_bytes}.subspan(
                            byte_offset,
                            kMinimizerGrouped32PositionEncodedBytes),
                        core->begin_bp,
                        core->end_bp,
                        profile);
                    REQUIRE(decoded.has_value());
                    REQUIRE(decoded->bp
                        <= (std::numeric_limits<std::uint64_t>::max)() >> 1u);
                    encoded_position = (decoded->bp << 1u)
                        | static_cast<std::uint64_t>(decoded->reverse_strand);
                }
                events.push_back({
                    .hash = hash,
                    .encoded_position = encoded_position,
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
    auto directory_bytes = encode_minimizer_bucket_directory(
        entries, original.directory.profile());
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

[[nodiscard]] bioseq::indexing::EncodedMinimizerFixedRow make_test_fixed_row(
        const bioseq::indexing::IndexChunkAddress& address,
        std::span<const std::byte> bytes) {
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

template <std::size_t Size>
[[nodiscard]] bioseq::indexing::EncodedMinimizerFixedRow make_test_fixed_row(
        const bioseq::indexing::IndexChunkAddress& address,
        const std::array<std::byte, Size>& bytes) {
    return make_test_fixed_row(
        address, std::span<const std::byte>{bytes});
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
        std::span<const std::byte>{*manifest_bytes});
    generation.unpublished_head_row = make_test_fixed_row(
        IndexChunkAddress{
            .generation_id = 0,
            .position_segment = -1,
            .sub_key = GENERATION_HEAD_SUB_KEY,
            .chunk_no = 0,
        },
        *head_bytes);
}

struct Grouped32ReaderFixture {
    bioseq::indexing::EncodedMinimizerGeneration generation;
    std::vector<CollectedMinimizerRow> rows;
};

[[nodiscard]] Grouped32ReaderFixture make_grouped32_reader_fixture(
        std::span<const std::pair<std::uint64_t, std::uint64_t>> minimizers,
        std::uint64_t target_length,
        bioseq::indexing::MinimizerSchemeIdentity scheme =
            bioseq::indexing::kLegacyMinimapV1SchemeIdentity,
        bioseq::indexing::MinimizerProducerBackendIdentity producer_backend =
            {}) {
    if (!bioseq::indexing::is_supported_minimizer_producer_backend_identity(
            producer_backend)) {
        producer_backend = bioseq::indexing::
            minimizer_producer_backend_identity_for_backend(
                scheme == bioseq::indexing::
                    kPaperNtHashU32R7SumV1SchemeIdentity
                    ? bioseq::simd::HWY_NAMESPACE::
                        MinimizerSketchBackend::PaperNtHashSimd
                    : bioseq::simd::HWY_NAMESPACE::
                        MinimizerSketchBackend::ExactSimd);
    }
    VectorReplayMinimizerProducer producer{
        minimizers, scheme, producer_backend};
    CollectingMinimizerRowSink sink;
    auto options = test_grouped32_generation_options(target_length);
    options.minimizer_scheme_id = scheme.id;
    options.minimizer_scheme_version = scheme.version;
    auto generation = bioseq::indexing::encode_minimizer_generation(
        producer, sink, options);
    REQUIRE(generation.has_value());
    REQUIRE(producer.scan_count == 2u);
    return Grouped32ReaderFixture{
        .generation = std::move(*generation),
        .rows = std::move(sink.rows),
    };
}

struct Grouped32ColdRouteFixture {
    RebuiltGenerationMetadata metadata;
    bioseq::indexing::DirView directory;
};

[[nodiscard]] std::vector<bioseq::indexing::MinimizerBucketDirEntry>
make_grouped32_cold_route_entries(std::size_t count) {
    using namespace bioseq::indexing;
    std::vector<MinimizerBucketDirEntry> entries;
    entries.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint64_t lo_hash = 100u + index * 10u;
        entries.push_back(MinimizerBucketDirEntry{
            .partition_id = static_cast<std::uint32_t>(index),
            .position_segment = 0,
            .lo_hash = lo_hash,
            .hi_hash = lo_hash + 9u,
            .sub_key = static_cast<index_sub_key_t>(index),
            .control_first_chunk = 0,
            .control_chunk_count = 1,
            .position_first_chunk = 0,
            .position_chunk_count = 1,
            .control_element_count = 1,
            .position_element_count = 1,
            .control_encoded_bytes =
                kMinimizerGrouped32ControlEncodedBytes,
            .position_encoded_bytes =
                kMinimizerGrouped32PositionEncodedBytes,
            .control_checksum = 0x1000u + index,
            .position_checksum = 0x2000u + index,
        });
    }
    return entries;
}

[[nodiscard]] Grouped32ColdRouteFixture
make_grouped32_cold_route_fixture(
        std::span<const bioseq::indexing::MinimizerBucketDirEntry> entries) {
    using namespace bioseq::indexing;
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> seed{{
        {1u, 0u},
    }};
    auto base = make_grouped32_reader_fixture(seed, 100u);
    auto adjusted = base.generation;
    adjusted.manifest.control_count = 0;
    adjusted.manifest.position_count = 0;
    adjusted.manifest.payload_bytes = 0;
    std::int32_t maximum_segment = 0;
    for (const MinimizerBucketDirEntry& entry : entries) {
        adjusted.manifest.control_count += entry.control_element_count;
        adjusted.manifest.position_count += entry.position_element_count;
        adjusted.manifest.payload_bytes +=
            entry.control_encoded_bytes + entry.position_encoded_bytes;
        maximum_segment = std::max(maximum_segment, entry.position_segment);
    }
    adjusted.manifest.extent_count =
        static_cast<std::uint32_t>(entries.size() * 2u);
    adjusted.manifest.directory_bytes =
        entries.size() * MinimizerBucketDirEntry::encoded_bytes;
    adjusted.manifest.directory_chunk_count = static_cast<std::uint32_t>(
        adjusted.manifest.directory_bytes / kMinimizerExtentPayloadBytes
        + (adjusted.manifest.directory_bytes
                % kMinimizerExtentPayloadBytes != 0
            ? 1u : 0u));
    adjusted.manifest.position_segment_count =
        entries.empty() ? 0u : static_cast<std::uint32_t>(maximum_segment + 1);
    adjusted.manifest.target_length = entries.empty()
        ? 0u
        : static_cast<std::uint64_t>(maximum_segment)
                * adjusted.manifest.position_segment_span_bp
            + 100u;
    RebuiltGenerationMetadata metadata =
        rebuild_generation_metadata(adjusted, entries);
    auto directory = DirView::decode(
        metadata.directory_bytes, adjusted.directory.profile());
    REQUIRE(directory.has_value());
    REQUIRE(validate_minimizer_generation_metadata(
        metadata.head, metadata.manifest, *directory).has_value());
    return Grouped32ColdRouteFixture{
        .metadata = std::move(metadata),
        .directory = std::move(*directory),
    };
}

[[nodiscard]] const CollectedMinimizerRow& grouped32_extent_row(
        const Grouped32ReaderFixture& fixture,
        std::uint32_t ordinal,
        bool positions,
        std::uint32_t chunk) {
    using namespace bioseq::indexing;
    const MinimizerBucketDirEntry* entry =
        fixture.generation.directory.entry_by_ordinal(ordinal);
    REQUIRE(entry != nullptr);
    const auto position_sub_key = derive_minimizer_position_extent_sub_key(
        entry->sub_key);
    REQUIRE(position_sub_key.has_value());
    const index_sub_key_t sub_key = positions
        ? *position_sub_key : entry->sub_key;
    const std::uint64_t generation_id =
        fixture.generation.manifest.generation_id;
    const auto row = std::ranges::find_if(
        fixture.rows,
        [entry, generation_id, sub_key, chunk](
                const CollectedMinimizerRow& candidate) {
            return candidate.address.generation_id
                    == generation_id
                && candidate.address.position_segment
                    == entry->position_segment
                && candidate.address.sub_key == sub_key
                && candidate.address.chunk_no == chunk;
        });
    REQUIRE(row != fixture.rows.end());
    return *row;
}

[[nodiscard]] CollectedMinimizerRow& grouped32_extent_row(
        Grouped32ReaderFixture& fixture,
        std::uint32_t ordinal,
        bool positions,
        std::uint32_t chunk) {
    return const_cast<CollectedMinimizerRow&>(grouped32_extent_row(
        std::as_const(fixture), ordinal, positions, chunk));
}

void feed_grouped32_extents(
        bioseq::indexing::PreparedGrouped32QueryReader& reader,
        const Grouped32ReaderFixture& fixture,
        std::span<const std::uint32_t> ordinals,
        bool positions,
        bool misalign_payload = false) {
    using namespace bioseq::indexing;
    for (const std::uint32_t ordinal : ordinals) {
        const MinimizerBucketDirEntry* entry =
            fixture.generation.directory.entry_by_ordinal(ordinal);
        REQUIRE(entry != nullptr);
        const std::uint32_t chunk_count = positions
            ? entry->position_chunk_count : entry->control_chunk_count;
        for (std::uint32_t chunk = 0; chunk < chunk_count; ++chunk) {
            const CollectedMinimizerRow& row = grouped32_extent_row(
                fixture, ordinal, positions, chunk);
            std::span<const std::byte> payload{row.bytes};
            std::vector<std::byte> unaligned;
            if (misalign_payload) {
                unaligned.resize(row.bytes.size() + 1u);
                std::copy(row.bytes.begin(), row.bytes.end(),
                    unaligned.begin() + 1);
                payload = std::span<const std::byte>{unaligned}.subspan(1);
            }
            const auto accepted = positions
                ? reader.accept_position_chunk(row.address, payload)
                : reader.accept_control_chunk(row.address, payload);
            REQUIRE(accepted.has_value());
        }
    }
}

void feed_grouped32_full_generation_physical(
		bioseq::indexing::PreparedGrouped32QueryReader& reader,
		const Grouped32ReaderFixture& fixture) {
	using namespace bioseq::indexing;
	const auto entries = fixture.generation.directory.entries();
	std::size_t segment_begin = 0;
	while (segment_begin < entries.size()) {
		std::size_t segment_end = segment_begin + 1u;
		while (segment_end < entries.size()
			&& entries[segment_end].position_segment
				== entries[segment_begin].position_segment) {
			++segment_end;
		}
		for (const bool positions : {false, true}) {
			for (std::size_t extent = segment_begin;
				 extent < segment_end; ++extent) {
				const auto& entry = entries[extent];
				const std::uint32_t first = positions
					? entry.position_first_chunk : entry.control_first_chunk;
				const std::uint32_t count = positions
					? entry.position_chunk_count : entry.control_chunk_count;
				for (std::uint32_t chunk = 0; chunk < count; ++chunk) {
					const auto& row = grouped32_extent_row(
						fixture, entry.partition_id, positions, first + chunk);
					const auto accepted = positions
						? reader.accept_full_generation_position_chunk(
							row.address, row.bytes)
						: reader.accept_full_generation_control_chunk(
							row.address, row.bytes);
					REQUIRE(accepted.has_value());
				}
			}
		}
		segment_begin = segment_end;
	}
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

TEST_CASE("DOC0373 validated row CRC algebra reconstructs raw extent CRC",
          "[indexing][minimizer-hashmap][wire][crc][cold-load][extent-crc]") {
    using namespace bioseq::indexing;

    REQUIRE(crc64_ecma182_advance_zero_bytes(0, 0) == 0);
    REQUIRE(crc64_ecma182_concatenate(
        crc64_ecma182("control"), crc64_ecma182("position"), 8u)
        == crc64_ecma182("controlposition"));

    const IndexChunkAddress signed_address{
        .generation_id = 0xFEDCBA9876543210ULL,
        .position_segment = (std::numeric_limits<std::int32_t>::min)() + 17,
        .sub_key = (std::numeric_limits<index_sub_key_t>::min)() + 29,
        .chunk_no = 0x89ABCDEFu,
    };
    constexpr std::uint32_t signed_length = 0x10203040u;
    Crc64Ecma182 independently_encoded_prefix;
    independently_encoded_prefix.update_little_endian(
        signed_address.generation_id);
    independently_encoded_prefix.update_little_endian(
        std::bit_cast<std::uint32_t>(signed_address.position_segment));
    independently_encoded_prefix.update_little_endian(
        std::bit_cast<std::uint64_t>(signed_address.sub_key));
    independently_encoded_prefix.update_little_endian(signed_address.chunk_no);
    independently_encoded_prefix.update_little_endian(signed_length);
    REQUIRE(crc64_index_chunk_row_prefix(
        signed_address.generation_id,
        signed_address.position_segment,
        signed_address.sub_key,
        signed_address.chunk_no,
        signed_length) == independently_encoded_prefix.checksum());

    std::uint64_t algebraic_extent = 0;
    Crc64Ecma182 direct_extent;
    std::uint64_t random_state = 0xD1B54A32D192ED03ULL;
    const auto next_random = [&random_state]() noexcept {
        random_state ^= random_state >> 12u;
        random_state ^= random_state << 25u;
        random_state ^= random_state >> 27u;
        return random_state * 0x2545F4914F6CDD1DULL;
    };
    const auto append_case = [&](std::size_t length, std::uint32_t ordinal) {
        INFO("payload length=" << length << " ordinal=" << ordinal);
        std::vector<std::byte> payload(length);
        for (std::byte& byte : payload) {
            byte = static_cast<std::byte>(next_random() & 0xffu);
        }
        const IndexChunkAddress address{
            .generation_id = 0x8000000000000000ULL + ordinal,
            .position_segment = ordinal % 2u == 0
                ? (std::numeric_limits<std::int32_t>::min)()
                    + static_cast<std::int32_t>(ordinal)
                : (std::numeric_limits<std::int32_t>::max)()
                    - static_cast<std::int32_t>(ordinal),
            .sub_key = ordinal % 2u == 0
                ? (std::numeric_limits<index_sub_key_t>::min)()
                    + static_cast<index_sub_key_t>(ordinal)
                : (std::numeric_limits<index_sub_key_t>::max)()
                    - static_cast<index_sub_key_t>(ordinal),
            .chunk_no = 0xF0000000u + ordinal,
        };
        const auto bytes = std::span<const std::byte>{payload};
        const auto chunk_bytes = static_cast<std::uint32_t>(bytes.size());
        const std::uint64_t row_checksum = crc64_index_chunk_row(
            address.generation_id,
            address.position_segment,
            address.sub_key,
            address.chunk_no,
            chunk_bytes,
            bytes);
        direct_extent.update(bytes);
        algebraic_extent = crc64_append_validated_index_chunk_payload(
            algebraic_extent, address, chunk_bytes, row_checksum);
        REQUIRE(algebraic_extent == direct_extent.checksum());
    };

    const std::array<std::size_t, 6> boundary_lengths{
        0u, 1u, 17u, 8063u, 8064u, 31u};
    std::uint32_t ordinal = 0;
    for (const std::size_t length : boundary_lengths) {
        append_case(length, ordinal++);
    }
    for (std::size_t iteration = 0; iteration < 128u; ++iteration) {
        append_case(static_cast<std::size_t>(next_random() % 8065u), ordinal++);
    }

    REQUIRE(algebraic_extent == direct_extent.checksum());
}

TEST_CASE("Format-5 cached ownership ledger replaces and moves exactly",
          "[indexing][minimizer-hashmap][wire][v5][reader][admission][ledger]") {
    using bioseq::indexing::ProtocolOwnedBytesLedger;
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<
        ProtocolOwnedBytesLedger>);
    STATIC_REQUIRE(std::is_nothrow_move_assignable_v<
        ProtocolOwnedBytesLedger>);

    ProtocolOwnedBytesLedger ledger;
    const auto replace = [&ledger](std::uint64_t& field,
                                   std::uint64_t value) {
        REQUIRE(ledger.replace_checked(field, value));
        field = value;
        REQUIRE(ledger.field_sum_holds());
    };
    replace(ledger.directory_bytes, 101u);
    replace(ledger.query_hash_bytes, 211u);
    replace(ledger.query_record_bytes, 307u);
    replace(ledger.selected_extent_owner_bytes, 401u);
    replace(ledger.query_record_position_bytes, 503u);
    replace(ledger.control_scratch_bytes, 601u);
    replace(ledger.selected_extent_owner_bytes, 17u);
    REQUIRE(ledger.add_temporary_peak_checked(8192u));

    const std::uint64_t total_before_failure = ledger.total_bytes;
    REQUIRE_FALSE(ledger.replace_checked(total_before_failure + 1u, 0u));
    REQUIRE(ledger.total_bytes == total_before_failure);
    REQUIRE(ledger.field_sum_holds());

    ProtocolOwnedBytesLedger moved{std::move(ledger)};
    REQUIRE(moved.total_bytes == total_before_failure);
    REQUIRE(moved.field_sum_holds());
    REQUIRE(ledger.total_bytes == 0u);
    REQUIRE(ledger.field_sum_holds());
    moved.reset();
    REQUIRE(moved.total_bytes == 0u);
    REQUIRE(moved.field_sum_holds());

    ProtocolOwnedBytesLedger overflow;
    overflow.directory_bytes =
        (std::numeric_limits<std::uint64_t>::max)() - 3u;
    overflow.total_bytes = overflow.directory_bytes;
    REQUIRE_FALSE(overflow.replace_checked(0u, 4u));
    REQUIRE_FALSE(overflow.add_temporary_peak_checked(4u));
    REQUIRE(overflow.field_sum_holds());
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
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*encoded, 4)
        == static_cast<std::uint16_t>(
            MinimizerGenerationWireFormat::sorted_dense_global64));
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
    invalid.wire_format = MinimizerGenerationWireFormat::invalid;
    require_wire_error(encode_minimizer_generation_head(invalid),
                       MinimizerGenerationWireError::version);
    invalid = head;
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
        b(0x02), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x01), b(0x01),
    };

    constexpr auto manifest = golden_generation_manifest();
    constexpr auto encoded = encode_minimizer_generation_manifest(manifest);
    STATIC_REQUIRE(encoded.has_value());
    STATIC_REQUIRE(*encoded == expected);
    STATIC_REQUIRE(minimizer_generation_compatibility_checksum(manifest)
        == 0x2F5B457F5964F3E1ULL);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded, 0)
        == MinimizerGenerationManifest::magic);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*encoded, 4)
        == static_cast<std::uint16_t>(
            MinimizerGenerationWireFormat::sorted_dense_global64));
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*encoded, 6) == 128);

    constexpr auto decoded = decode_minimizer_generation_manifest(
        std::span<const std::byte>{*encoded});
    STATIC_REQUIRE(decoded.has_value());
    STATIC_REQUIRE(*decoded == manifest);
}

TEST_CASE("wire formats derive closed payload profiles and fixed v4 metadata images",
          "[indexing][minimizer-hashmap][wire][profile][v4]") {
    using namespace bioseq::indexing;
    STATIC_REQUIRE(kTestV3LegacyProfile.control_encoded_byte_width == 16u);
    STATIC_REQUIRE(kTestV3LegacyProfile.position_encoded_byte_width == 8u);
    STATIC_REQUIRE(kTestV3LegacyProfile.payload_layout_abi_version == 1u);
    STATIC_REQUIRE(kTestV3LegacyProfile.gpu_abi_version == 1u);
    STATIC_REQUIRE_FALSE(kTestV3LegacyProfile.segment_local_positions);
    STATIC_REQUIRE(kTestV4LegacyProfile.control_encoded_byte_width == 12u);
    STATIC_REQUIRE(kTestV4LegacyProfile.position_encoded_byte_width == 4u);
    STATIC_REQUIRE(kTestV4LegacyProfile.payload_layout_abi_version == 1u);
    STATIC_REQUIRE(kTestV4LegacyProfile.gpu_abi_version == 2u);
    STATIC_REQUIRE(kTestV4LegacyProfile.max_hash
        == kLegacyMinimapK15MaxHash);
    STATIC_REQUIRE(kTestV4LegacyProfile.segment_local_positions);
    STATIC_REQUIRE(kTestV4PaperProfile.max_hash
        == (std::numeric_limits<std::uint32_t>::max)());
    STATIC_REQUIRE_FALSE(minimizer_generation_payload_profile(
        MinimizerGenerationWireFormat::grouped_hash32_segment_local32,
        kLegacyMinimapV1SchemeIdentity,
        14u).has_value());
    STATIC_REQUIRE_FALSE(minimizer_generation_payload_profile(
        MinimizerGenerationWireFormat::invalid,
        kLegacyMinimapV1SchemeIdentity,
        15u).has_value());

    constexpr auto v4_head = [] {
        auto value = golden_generation_head();
        value.wire_format =
            MinimizerGenerationWireFormat::grouped_hash32_segment_local32;
        return value;
    }();
    constexpr auto v4_head_bytes = encode_minimizer_generation_head(v4_head);
    STATIC_REQUIRE(v4_head_bytes.has_value());
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*v4_head_bytes, 4) == 4u);
    constexpr auto decoded_v4_head = decode_minimizer_generation_head(
        std::span<const std::byte>{*v4_head_bytes});
    STATIC_REQUIRE(decoded_v4_head.has_value());
    STATIC_REQUIRE(*decoded_v4_head == v4_head);

    constexpr auto v4_manifest = [] {
        auto value = golden_generation_manifest();
        value.wire_format =
            MinimizerGenerationWireFormat::grouped_hash32_segment_local32;
        value.kmer_size = 15u;
        value.gpu_abi_version = kMinimizerGrouped32GpuAbiVersion;
        return value;
    }();
    constexpr auto v4_manifest_bytes =
        encode_minimizer_generation_manifest(v4_manifest);
    STATIC_REQUIRE(v4_manifest_bytes.has_value());
    STATIC_REQUIRE(v4_manifest_bytes->size() == 128u);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*v4_manifest_bytes, 4) == 4u);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*v4_manifest_bytes, 104) == 15u);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*v4_manifest_bytes, 108)
        == kMinimizerGrouped32GpuAbiVersion);
    constexpr auto decoded_v4_manifest =
        decode_minimizer_generation_manifest(
            std::span<const std::byte>{*v4_manifest_bytes});
    STATIC_REQUIRE(decoded_v4_manifest.has_value());
    STATIC_REQUIRE(*decoded_v4_manifest == v4_manifest);
    STATIC_REQUIRE(minimizer_generation_compatibility_checksum(v4_manifest)
        == 0x6629492DB73FFC74ULL);
    STATIC_REQUIRE(minimizer_generation_compatibility_checksum(v4_manifest)
        != minimizer_generation_compatibility_checksum(
            golden_generation_manifest()));

    constexpr auto v4_paper_manifest = [] {
        auto value = golden_generation_manifest();
        value.wire_format =
            MinimizerGenerationWireFormat::grouped_hash32_segment_local32;
        value.kmer_size = 15u;
        value.gpu_abi_version = kMinimizerGrouped32GpuAbiVersion;
        value.minimizer_scheme_id = kPaperNtHashU32R7SumV1SchemeId;
        value.minimizer_scheme_version =
            kPaperNtHashU32R7SumV1SchemeVersion;
        value.producer_backend_id =
            MinimizerProducerBackendId::paper_nthash_simd;
        return value;
    }();
    STATIC_REQUIRE(minimizer_generation_compatibility_checksum(
        v4_paper_manifest) == 0x7148BB81AA32B925ULL);
    STATIC_REQUIRE(minimizer_generation_compatibility_checksum(
        v4_paper_manifest)
        != minimizer_generation_compatibility_checksum(v4_manifest));
}

TEST_CASE("v4 directory profile is bound and cross-format metadata fails closed",
          "[indexing][minimizer-hashmap][wire][profile][v4][corruption]") {
    using namespace bioseq::indexing;
    const MinimizerBucketDirEntry entry{
        .partition_id = 0,
        .position_segment = 0,
        .lo_hash = 7,
        .hi_hash = 7,
        .sub_key = 0,
        .control_first_chunk = 0,
        .control_chunk_count = 1,
        .position_first_chunk = 0,
        .position_chunk_count = 1,
        .control_element_count = 1,
        .position_element_count = 2,
        .control_encoded_bytes = 12,
        .position_encoded_bytes = 8,
        .control_checksum = 11,
        .position_checksum = 12,
    };
    const std::array entries{entry};
    const auto bytes = encode_minimizer_bucket_directory(
        entries, kTestV4LegacyProfile);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes->size() == MinimizerBucketDirEntry::encoded_bytes);
    const auto directory = DirView::decode(*bytes, kTestV4LegacyProfile);
    REQUIRE(directory.has_value());
    REQUIRE(directory->profile() == kTestV4LegacyProfile);

    require_wire_error(
        DirView::decode(*bytes, kTestV3LegacyProfile),
        MinimizerGenerationWireError::byte_count);
    require_wire_error(
        encode_minimizer_bucket_directory(entries, kTestV3LegacyProfile),
        MinimizerGenerationWireError::byte_count);
    auto legacy_hash_overflow = entry;
    legacy_hash_overflow.lo_hash =
        static_cast<std::uint64_t>(kLegacyMinimapK15MaxHash) + 1u;
    legacy_hash_overflow.hi_hash = legacy_hash_overflow.lo_hash;
    const std::array overflow_entries{legacy_hash_overflow};
    require_wire_error(
        encode_minimizer_bucket_directory(
            overflow_entries, kTestV4LegacyProfile),
        MinimizerGenerationWireError::hash_range);
    REQUIRE(encode_minimizer_bucket_directory(
        overflow_entries, kTestV4PaperProfile).has_value());

    MinimizerGenerationManifest manifest{
        .wire_format =
            MinimizerGenerationWireFormat::grouped_hash32_segment_local32,
        .generation_id = 91,
        .source_identity = 92,
        .source_content_checksum = 93,
        .target_length = 100,
        .control_count = 1,
        .position_count = 2,
        .payload_bytes = 20,
        .payload_checksum = 94,
        .directory_bytes = bytes->size(),
        .directory_checksum = crc64_ecma182(*bytes),
        .position_segment_count = 1,
        .extent_count = 2,
        .directory_chunk_count = 1,
        .alphabet_code = 4,
        .storage_unit_byte_width = 8,
        .kmer_size = 15,
        .window_size = 10,
        .gpu_abi_version = kMinimizerGrouped32GpuAbiVersion,
        .position_segment_span_bp = 100,
        .segment_geometry_version = kMinimizerSegmentGeometryVersion,
        .minimizer_scheme_id = kLegacyMinimapV1SchemeId,
        .minimizer_scheme_version = kLegacyMinimapV1SchemeVersion,
        .producer_backend_id = MinimizerProducerBackendId::scalar,
        .producer_backend_identity_version =
            kMinimizerProducerBackendIdentityVersion,
    };
    const auto manifest_bytes = encode_minimizer_generation_manifest(manifest);
    REQUIRE(manifest_bytes.has_value());
    MinimizerGenerationHead head{
        .wire_format = manifest.wire_format,
        .generation_id = manifest.generation_id,
        .source_identity = manifest.source_identity,
        .compatibility_checksum =
            minimizer_generation_compatibility_checksum(manifest),
        .manifest_checksum = crc64_ecma182(
            std::span<const std::byte>{*manifest_bytes}),
        .source_content_checksum = manifest.source_content_checksum,
    };
    REQUIRE(validate_minimizer_generation_metadata(
        head, manifest, *directory).has_value());

    const auto v4_expected = expected_minimizer_compatibility_for_backend(
        MinimizerGenerationWireFormat::grouped_hash32_segment_local32,
        MinimizerSketchBackend::Scalar,
        4, 8, 15, 10);
    REQUIRE(v4_expected.has_value());
    REQUIRE(validate_minimizer_generation_compatibility(
        manifest, *v4_expected).has_value());
    const auto v3_expected = expected_minimizer_compatibility_for_backend(
        MinimizerGenerationWireFormat::sorted_dense_global64,
        MinimizerSketchBackend::Scalar,
        4, 8, 15, 10);
    REQUIRE(v3_expected.has_value());
    require_wire_error(
        validate_minimizer_generation_compatibility(manifest, *v3_expected),
        MinimizerGenerationWireError::compatibility_mismatch);

    auto cross_format_head = head;
    cross_format_head.wire_format =
        MinimizerGenerationWireFormat::sorted_dense_global64;
    require_wire_error(
        validate_minimizer_generation_metadata(
            cross_format_head, manifest, *directory),
        MinimizerGenerationWireError::compatibility_mismatch);

    const auto wrong_profile_directory =
        DirView::decode(*bytes, kTestV4PaperProfile);
    REQUIRE(wrong_profile_directory.has_value());
    require_wire_error(
        validate_minimizer_generation_metadata(
            head, manifest, *wrong_profile_directory),
        MinimizerGenerationWireError::compatibility_mismatch);
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
    STATIC_REQUIRE_FALSE(is_supported_minimizer_producer_backend_identity(
        MinimizerProducerBackendIdentity{
            .id = unbound_reader.producer_backend_id,
            .version = unbound_reader.producer_backend_identity_version,
        }));
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
    STATIC_REQUIRE(minimizer_producer_backend_identity_for_backend(
        MinimizerSketchBackend::Scalar).id
        == MinimizerProducerBackendId::scalar);
    STATIC_REQUIRE(minimizer_producer_backend_identity_for_backend(
        MinimizerSketchBackend::ExactSimd).id
        == MinimizerProducerBackendId::exact_simd);
    STATIC_REQUIRE(minimizer_producer_backend_identity_for_backend(
        MinimizerSketchBackend::PaperNtHashSimd).id
        == MinimizerProducerBackendId::paper_nthash_simd);
    STATIC_REQUIRE(minimizer_producer_backend_identity_for_backend(
        MinimizerSketchBackend::VulkanSegment).id
        == MinimizerProducerBackendId::vulkan_segment);
    constexpr auto legacy = golden_generation_manifest();
    constexpr auto legacy_bytes = encode_minimizer_generation_manifest(legacy);
    STATIC_REQUIRE(legacy_bytes.has_value());
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*legacy_bytes, 122)
        == kLegacyMinimapV1SchemeId);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*legacy_bytes, 124)
        == kLegacyMinimapV1SchemeVersion);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*legacy_bytes, 126)
        == static_cast<std::uint8_t>(MinimizerProducerBackendId::scalar));
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*legacy_bytes, 127)
        == kMinimizerProducerBackendIdentityVersion);

    constexpr auto paper = [] {
        auto value = golden_generation_manifest();
        value.minimizer_scheme_id = kPaperNtHashU32R7SumV1SchemeId;
        value.minimizer_scheme_version = kPaperNtHashU32R7SumV1SchemeVersion;
        value.producer_backend_id =
            MinimizerProducerBackendId::paper_nthash_simd;
        return value;
    }();
    constexpr auto paper_bytes = encode_minimizer_generation_manifest(paper);
    STATIC_REQUIRE(paper_bytes.has_value());
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*paper_bytes, 122)
        == kPaperNtHashU32R7SumV1SchemeId);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*paper_bytes, 124)
        == kPaperNtHashU32R7SumV1SchemeVersion);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*paper_bytes, 126)
        == static_cast<std::uint8_t>(
            MinimizerProducerBackendId::paper_nthash_simd));
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*paper_bytes, 127)
        == kMinimizerProducerBackendIdentityVersion);
    STATIC_REQUIRE(*paper_bytes != *legacy_bytes);
    constexpr auto decoded_paper = decode_minimizer_generation_manifest(
        std::span<const std::byte>{*paper_bytes});
    STATIC_REQUIRE(decoded_paper.has_value());
    STATIC_REQUIRE(*decoded_paper == paper);
    STATIC_REQUIRE(minimizer_generation_compatibility_checksum(paper)
        == 0x61D8A289AB47E26CULL);
    STATIC_REQUIRE(minimizer_generation_compatibility_checksum(paper)
        != minimizer_generation_compatibility_checksum(legacy));

    constexpr auto canonical_legacy = [] {
        auto value = golden_generation_manifest();
        value.gpu_abi_version = kMinimizerSortedDenseGpuAbiVersion;
        return value;
    }();
    constexpr auto canonical_paper = [] {
        auto value = golden_generation_manifest();
        value.gpu_abi_version = kMinimizerSortedDenseGpuAbiVersion;
        value.minimizer_scheme_id = kPaperNtHashU32R7SumV1SchemeId;
        value.minimizer_scheme_version = kPaperNtHashU32R7SumV1SchemeVersion;
        value.producer_backend_id =
            MinimizerProducerBackendId::paper_nthash_simd;
        return value;
    }();

    constexpr ExpectedMinimizerCompatibility legacy_reader{
        .wire_format = MinimizerGenerationWireFormat::sorted_dense_global64,
        .alphabet_code = canonical_legacy.alphabet_code,
        .storage_unit_byte_width = canonical_legacy.storage_unit_byte_width,
        .kmer_size = canonical_legacy.kmer_size,
        .window_size = canonical_legacy.window_size,
        .gpu_abi_version = canonical_legacy.gpu_abi_version,
        .minimizer_scheme_id = kLegacyMinimapV1SchemeId,
        .minimizer_scheme_version = kLegacyMinimapV1SchemeVersion,
        .producer_backend_id = MinimizerProducerBackendId::scalar,
        .producer_backend_identity_version =
            kMinimizerProducerBackendIdentityVersion,
    };
    constexpr ExpectedMinimizerCompatibility paper_reader{
        .wire_format = MinimizerGenerationWireFormat::sorted_dense_global64,
        .alphabet_code = canonical_paper.alphabet_code,
        .storage_unit_byte_width = canonical_paper.storage_unit_byte_width,
        .kmer_size = canonical_paper.kmer_size,
        .window_size = canonical_paper.window_size,
        .gpu_abi_version = canonical_paper.gpu_abi_version,
        .minimizer_scheme_id = kPaperNtHashU32R7SumV1SchemeId,
        .minimizer_scheme_version = kPaperNtHashU32R7SumV1SchemeVersion,
        .producer_backend_id = MinimizerProducerBackendId::paper_nthash_simd,
        .producer_backend_identity_version =
            kMinimizerProducerBackendIdentityVersion,
    };
    STATIC_REQUIRE(validate_minimizer_generation_compatibility(
        canonical_legacy, legacy_reader).has_value());
    STATIC_REQUIRE(validate_minimizer_generation_compatibility(
        canonical_paper, paper_reader).has_value());
    STATIC_REQUIRE_FALSE(validate_minimizer_generation_compatibility(
        canonical_legacy, paper_reader).has_value());
    STATIC_REQUIRE(validate_minimizer_generation_compatibility(
        canonical_legacy, paper_reader).error()
        == MinimizerGenerationWireError::compatibility_mismatch);
    STATIC_REQUIRE_FALSE(validate_minimizer_generation_compatibility(
        canonical_paper, legacy_reader).has_value());
    STATIC_REQUIRE(validate_minimizer_generation_compatibility(
        canonical_paper, legacy_reader).error()
        == MinimizerGenerationWireError::compatibility_mismatch);

    constexpr auto canonical_exact = [=] {
        auto value = canonical_legacy;
        value.producer_backend_id = MinimizerProducerBackendId::exact_simd;
        return value;
    }();
    constexpr auto canonical_vulkan = [=] {
        auto value = canonical_legacy;
        value.producer_backend_id = MinimizerProducerBackendId::vulkan_segment;
        return value;
    }();
    constexpr auto exact_reader = [=] {
        auto value = legacy_reader;
        value.producer_backend_id = MinimizerProducerBackendId::exact_simd;
        return value;
    }();
    constexpr auto vulkan_reader = [=] {
        auto value = legacy_reader;
        value.producer_backend_id = MinimizerProducerBackendId::vulkan_segment;
        return value;
    }();
    STATIC_REQUIRE(validate_minimizer_generation_compatibility(
        canonical_exact, exact_reader).has_value());
    STATIC_REQUIRE(validate_minimizer_generation_compatibility(
        canonical_vulkan, vulkan_reader).has_value());
    STATIC_REQUIRE_FALSE(validate_minimizer_generation_compatibility(
        canonical_legacy, exact_reader).has_value());
    STATIC_REQUIRE_FALSE(validate_minimizer_generation_compatibility(
        canonical_exact, legacy_reader).has_value());
    STATIC_REQUIRE_FALSE(validate_minimizer_generation_compatibility(
        canonical_legacy, vulkan_reader).has_value());
    STATIC_REQUIRE_FALSE(validate_minimizer_generation_compatibility(
        canonical_vulkan, legacy_reader).has_value());
    STATIC_REQUIRE(minimizer_generation_compatibility_checksum(
        canonical_legacy) != minimizer_generation_compatibility_checksum(
            canonical_exact));
    STATIC_REQUIRE(minimizer_generation_compatibility_checksum(
        canonical_legacy) != minimizer_generation_compatibility_checksum(
            canonical_vulkan));

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
    REQUIRE(generated->manifest.producer_backend_id
        == MinimizerProducerBackendId::paper_nthash_simd);
    REQUIRE(generated->manifest.producer_backend_identity_version
        == kMinimizerProducerBackendIdentityVersion);

    for (const MinimizerSketchBackend backend : {
             MinimizerSketchBackend::ExactSimd,
             MinimizerSketchBackend::VulkanSegment}) {
        const auto backend_identity =
            minimizer_producer_backend_identity_for_backend(backend);
        VectorReplayMinimizerProducer physical_producer{
            minimizers, kLegacyMinimapV1SchemeIdentity, backend_identity};
        CollectingMinimizerRowSink physical_sink;
        const auto physical = encode_minimizer_generation(
            physical_producer, physical_sink, test_generation_options(100));
        REQUIRE(physical.has_value());
        REQUIRE(physical->manifest.producer_backend_id
            == backend_identity.id);
        REQUIRE(physical->manifest.producer_backend_identity_version
            == backend_identity.version);
        const auto matching = expected_minimizer_compatibility_for_backend(
            MinimizerGenerationWireFormat::sorted_dense_global64,
            backend, 4, 8, 15, 10);
        REQUIRE(matching.has_value());
        REQUIRE(validate_minimizer_generation_compatibility(
            physical->manifest, *matching).has_value());
        REQUIRE_FALSE(validate_minimizer_generation_compatibility(
            physical->manifest, test_expected_compatibility()).has_value());
    }

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

    // The former zero-reserved suffix is never reinterpreted as Scalar. Old
    // manifests and malformed/unknown identity namespaces force rebuild.
    corrupted = good;
    corrupted[126] = std::byte{0};
    corrupted[127] = std::byte{0};
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       MinimizerGenerationWireError::compatibility_mismatch);
    for (const std::uint8_t unsupported_backend : {0u, 5u, 255u}) {
        corrupted = good;
        test_write_le<std::uint8_t>(corrupted, 126, unsupported_backend);
        require_wire_error(decode_minimizer_generation_manifest(corrupted),
                           MinimizerGenerationWireError::compatibility_mismatch);
    }
    for (const std::uint8_t unsupported_version : {0u, 2u, 255u}) {
        corrupted = good;
        test_write_le<std::uint8_t>(corrupted, 127, unsupported_version);
        require_wire_error(decode_minimizer_generation_manifest(corrupted),
                           MinimizerGenerationWireError::compatibility_mismatch);
    }
    corrupted = good;
    test_write_le<std::uint8_t>(corrupted, 126,
        static_cast<std::uint8_t>(
            MinimizerProducerBackendId::paper_nthash_simd));
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       MinimizerGenerationWireError::compatibility_mismatch);

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
    test_write_le<std::uint16_t>(corrupted, 120, 1);
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
    invalid.wire_format = MinimizerGenerationWireFormat::invalid;
    require_wire_error(encode_minimizer_generation_manifest(invalid),
                       MinimizerGenerationWireError::compatibility_mismatch);
    invalid = manifest;
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
    invalid.segment_geometry_version = 1;
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

    STATIC_REQUIRE(kMinimizerSegmentGeometryVersion == 2u);
    STATIC_REQUIRE(POSITION_EXTENT_BASE == (index_sub_key_t{1} << 61));
    STATIC_REQUIRE(POSITION_EXTENT_BASE * 2 == TEMP_RUN_BASE);
    constexpr auto first_position_key =
        derive_minimizer_position_extent_sub_key(0);
    constexpr auto last_position_key =
        derive_minimizer_position_extent_sub_key(POSITION_EXTENT_BASE - 1);
    STATIC_REQUIRE(first_position_key.has_value());
    STATIC_REQUIRE(*first_position_key == POSITION_EXTENT_BASE);
    STATIC_REQUIRE(last_position_key.has_value());
    STATIC_REQUIRE(*last_position_key == TEMP_RUN_BASE - 1);
    STATIC_REQUIRE_FALSE(
        derive_minimizer_position_extent_sub_key(-1).has_value());
    STATIC_REQUIRE_FALSE(derive_minimizer_position_extent_sub_key(
        POSITION_EXTENT_BASE).has_value());
    STATIC_REQUIRE(project_minimizer_extent_sub_key(0, false).has_value());
    STATIC_REQUIRE(*project_minimizer_extent_sub_key(0, false) == 0);
    STATIC_REQUIRE(project_minimizer_extent_sub_key(
        POSITION_EXTENT_BASE, true).has_value());
    STATIC_REQUIRE(*project_minimizer_extent_sub_key(
        POSITION_EXTENT_BASE, true) == 0);
    STATIC_REQUIRE(*project_minimizer_extent_sub_key(
        TEMP_RUN_BASE - 1, true) == POSITION_EXTENT_BASE - 1);
    STATIC_REQUIRE_FALSE(project_minimizer_extent_sub_key(
        (std::numeric_limits<index_sub_key_t>::min)(), true).has_value());
    STATIC_REQUIRE_FALSE(project_minimizer_extent_sub_key(
        POSITION_EXTENT_BASE - 1, true).has_value());
    STATIC_REQUIRE_FALSE(project_minimizer_extent_sub_key(
        POSITION_EXTENT_BASE, false).has_value());
    STATIC_REQUIRE_FALSE(project_minimizer_extent_sub_key(
        TEMP_RUN_BASE, true).has_value());
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

TEST_CASE("DOC0371 multi-span lookup carries one global occurrence decision",
          "[indexing][minimizer-hashmap][lookup-spans]") {
    using namespace bioseq::indexing;
    struct SegmentedFixtureIndex {
        MinimizerLookupSpans result{};
        [[nodiscard]] MinimizerLookupSpans lookup_spans(
                std::uint64_t) const noexcept {
            return result;
        }
        [[nodiscard]] bioseq::storage::sequence_size_t target_length()
                const noexcept {
            return 4096;
        }
    };
    struct UnsafeOwningLookupIndex {
        [[nodiscard]] std::vector<std::uint64_t> lookup(
                std::uint64_t) const {
            return {};
        }
        [[nodiscard]] bioseq::storage::sequence_size_t target_length()
                const noexcept {
            return 0;
        }
    };
    STATIC_REQUIRE(MinimizerLookupIndex<SegmentedFixtureIndex>);
    STATIC_REQUIRE(SegmentedMinimizerLookupIndex<SegmentedFixtureIndex>);
    STATIC_REQUIRE_FALSE(ContiguousMinimizerLookupIndex<SegmentedFixtureIndex>);
    STATIC_REQUIRE_FALSE(ContiguousMinimizerLookupIndex<
        UnsafeOwningLookupIndex>);
    STATIC_REQUIRE_FALSE(MinimizerLookupIndex<UnsafeOwningLookupIndex>);

    const std::array<std::uint64_t, 2> first{4, 8};
    const std::array<std::uint64_t, 3> second{12, 16, 20};
    const std::array<MinimizerPositionSpan, 2> spans{
        MinimizerPositionSpan{first},
        MinimizerPositionSpan{second},
    };
    const auto complete = MinimizerLookupSpans::segmented(
        spans, /*global_occurrence_count=*/5);
    REQUIRE(complete.positions_complete());
    REQUIRE(complete.global_occurrence_count() == 5);
    REQUIRE(complete.contiguous_positions().empty());
    REQUIRE(complete.segmented_positions().size() == 2);
    REQUIRE(complete.well_formed());

    const auto malformed_count = MinimizerLookupSpans::segmented(
        spans, /*global_occurrence_count=*/4);
    REQUIRE_FALSE(malformed_count.well_formed());
    const auto unavailable =
        MinimizerLookupSpans::positions_unavailable(1'000);
    REQUIRE_FALSE(unavailable.positions_complete());
    REQUIRE(unavailable.global_occurrence_count() == 1'000);
    REQUIRE(unavailable.well_formed());

    SegmentedFixtureIndex index{complete};
    const auto adapted = lookup_minimizer_spans(index, 7);
    REQUIRE(adapted.well_formed());
    REQUIRE(adapted.global_occurrence_count() == 5);
}

TEST_CASE("inactive grouped32 control codec has an exact 12-byte LE image",
          "[indexing][minimizer-hashmap][wire][grouped32][golden]") {
    using namespace bioseq::indexing;
    constexpr auto legacy = kTestV4LegacyProfile;
    constexpr MinimizerGrouped32Control control{
        .hash = 0x01234567u,
        .position_offset = 0x10203040u,
        .position_count = 0x01020304u,
    };
    constexpr std::array<std::byte, 12> golden{
        std::byte{0x67}, std::byte{0x45}, std::byte{0x23}, std::byte{0x01},
        std::byte{0x40}, std::byte{0x30}, std::byte{0x20}, std::byte{0x10},
        std::byte{0x04}, std::byte{0x03}, std::byte{0x02}, std::byte{0x01},
    };
    constexpr auto encoded =
        encode_minimizer_grouped32_control(control, legacy);
    STATIC_REQUIRE(encoded.has_value());
    STATIC_REQUIRE(encoded->size() == kMinimizerGrouped32ControlEncodedBytes);
    STATIC_REQUIRE(*encoded == golden);
    constexpr auto decoded = decode_minimizer_grouped32_control(
        *encoded, 0x11223344u, legacy);
    STATIC_REQUIRE(decoded.has_value());
    STATIC_REQUIRE(*decoded == control);

    constexpr auto paper = kTestV4PaperProfile;
    constexpr auto full_width = make_minimizer_grouped32_control(
        (std::numeric_limits<std::uint32_t>::max)(), 0, 1, paper);
    STATIC_REQUIRE(full_width.has_value());
    STATIC_REQUIRE(full_width->hash
        == (std::numeric_limits<std::uint32_t>::max)());
    constexpr auto full_width_bytes =
        encode_minimizer_grouped32_control(*full_width, paper);
    STATIC_REQUIRE(full_width_bytes.has_value());
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*full_width_bytes, 0)
        == (std::numeric_limits<std::uint32_t>::max)());
}

TEST_CASE("inactive grouped32 admission rejects unsupported hashes and bounds",
          "[indexing][minimizer-hashmap][wire][grouped32][admission]") {
    using namespace bioseq::indexing;
    constexpr auto legacy = kTestV4LegacyProfile;
    constexpr auto paper = kTestV4PaperProfile;
    STATIC_REQUIRE(admit_minimizer_grouped32_hash(
        kLegacyMinimapK15MaxHash, legacy).has_value());
    require_wire_error(
        admit_minimizer_grouped32_hash(
            static_cast<std::uint64_t>(kLegacyMinimapK15MaxHash) + 1u,
            legacy),
        MinimizerGenerationWireError::hash_range);
    STATIC_REQUIRE(admit_minimizer_grouped32_hash(
        (std::numeric_limits<std::uint32_t>::max)(), paper).has_value());
    require_wire_error(
        admit_minimizer_grouped32_hash(
            static_cast<std::uint64_t>(
                (std::numeric_limits<std::uint32_t>::max)()) + 1u,
            paper),
        MinimizerGenerationWireError::hash_range);

    auto unsupported_k = legacy;
    unsupported_k.kmer_size = 14;
    require_wire_error(
        validate_minimizer_grouped32_compatibility(unsupported_k),
        MinimizerGenerationWireError::compatibility_mismatch);
    auto unsupported_scheme = legacy;
    unsupported_scheme.minimizer_scheme = MinimizerSchemeIdentity{
        .id = 2,
        .version = 1,
    };
    require_wire_error(
        validate_minimizer_grouped32_compatibility(unsupported_scheme),
        MinimizerGenerationWireError::compatibility_mismatch);
    auto unsupported_layout = legacy;
    unsupported_layout.wire_format =
        static_cast<MinimizerGenerationWireFormat>(5u);
    require_wire_error(
        validate_minimizer_grouped32_compatibility(unsupported_layout),
        MinimizerGenerationWireError::compatibility_mismatch);
    auto unsupported_abi = legacy;
    ++unsupported_abi.payload_layout_abi_version;
    require_wire_error(
        validate_minimizer_grouped32_compatibility(unsupported_abi),
        MinimizerGenerationWireError::compatibility_mismatch);

    require_wire_error(
        make_minimizer_grouped32_control(1, 0, 0, legacy),
        MinimizerGenerationWireError::control_range);
    require_wire_error(
        make_minimizer_grouped32_control(
            1,
            (std::numeric_limits<std::uint32_t>::max)(),
            1,
            legacy),
        MinimizerGenerationWireError::control_range);
    const auto maximum_extent = make_minimizer_grouped32_control(
        1, 0, (std::numeric_limits<std::uint32_t>::max)(), legacy);
    REQUIRE(maximum_extent.has_value());

    constexpr auto good = encode_minimizer_grouped32_control(
        MinimizerGrouped32Control{.hash = 1, .position_offset = 2,
                                  .position_count = 3},
        legacy);
    STATIC_REQUIRE(good.has_value());
    require_wire_error(
        decode_minimizer_grouped32_control(
            std::span<const std::byte>{good->data(), good->size() - 1u},
            5,
            legacy),
        MinimizerGenerationWireError::encoded_size);
    require_wire_error(
        decode_minimizer_grouped32_control(
            *good,
            static_cast<std::uint64_t>(
                (std::numeric_limits<std::uint32_t>::max)()) + 1u,
            legacy),
        MinimizerGenerationWireError::control_range);

    auto corrupt_range = *good;
    test_write_le<std::uint32_t>(corrupt_range, 4, 4u);
    test_write_le<std::uint32_t>(corrupt_range, 8, 2u);
    require_wire_error(
        decode_minimizer_grouped32_control(corrupt_range, 5, legacy),
        MinimizerGenerationWireError::control_range);
    auto corrupt_legacy_hash = *good;
    test_write_le<std::uint32_t>(corrupt_legacy_hash, 0, 0xffffffffu);
    require_wire_error(
        decode_minimizer_grouped32_control(
            corrupt_legacy_hash, 5, legacy),
        MinimizerGenerationWireError::hash_range);
}

TEST_CASE("inactive grouped32 local positions reconstruct above 4G",
          "[indexing][minimizer-hashmap][wire][grouped32][position][golden]") {
    using namespace bioseq::indexing;
    constexpr auto paper = kTestV4PaperProfile;
    constexpr std::uint64_t segment_base = 0x0000000100000000ULL;
    constexpr std::uint64_t local_bp = 0x01234567ULL;
    constexpr std::uint64_t segment_end = segment_base + 0x02000000ULL;
    constexpr auto localized = localize_minimizer_grouped32_position(
        segment_base + local_bp,
        true,
        segment_base,
        segment_end,
        paper);
    STATIC_REQUIRE(localized.has_value());
    STATIC_REQUIRE(*localized == 0x02468acfu);
    constexpr auto encoded =
        encode_minimizer_grouped32_position(*localized, paper);
    STATIC_REQUIRE(encoded.has_value());
    constexpr std::array<std::byte, 4> golden{
        std::byte{0xcf}, std::byte{0x8a}, std::byte{0x46}, std::byte{0x02},
    };
    STATIC_REQUIRE(*encoded == golden);
    constexpr auto decoded = decode_minimizer_grouped32_position(
        *encoded, segment_base, segment_end, paper);
    STATIC_REQUIRE(decoded.has_value());
    STATIC_REQUIRE((*decoded == MinimizerGrouped32GlobalPosition{
        .bp = segment_base + local_bp,
        .reverse_strand = true,
    }));
    STATIC_REQUIRE(decoded->bp > (std::numeric_limits<std::uint32_t>::max)());

    constexpr std::uint64_t max_segment_base = 0x0000000200000000ULL;
    constexpr std::uint64_t max_segment_end =
        max_segment_base
        + static_cast<std::uint64_t>(kMinimizerGrouped32MaxLocalBp) + 1u;
    constexpr auto max_local = localize_minimizer_grouped32_position(
        max_segment_end - 1u,
        true,
        max_segment_base,
        max_segment_end,
        paper);
    STATIC_REQUIRE(max_local.has_value());
    STATIC_REQUIRE(*max_local == 0xffffffffu);
    constexpr auto max_local_bytes =
        encode_minimizer_grouped32_position(*max_local, paper);
    STATIC_REQUIRE(max_local_bytes.has_value());
    constexpr auto max_global = decode_minimizer_grouped32_position(
        *max_local_bytes, max_segment_base, max_segment_end, paper);
    STATIC_REQUIRE(max_global.has_value());
    STATIC_REQUIRE(max_global->bp == max_segment_end - 1u);
    STATIC_REQUIRE(max_global->reverse_strand);
}

TEST_CASE("inactive grouped32 local positions reject corruption and overflow",
          "[indexing][minimizer-hashmap][wire][grouped32][position][corruption]") {
    using namespace bioseq::indexing;
    constexpr auto legacy = kTestV4LegacyProfile;
    require_wire_error(
        validate_minimizer_grouped32_segment(10, 10),
        MinimizerGenerationWireError::position_range);
    require_wire_error(
        validate_minimizer_grouped32_segment(
            0,
            static_cast<std::uint64_t>(kMinimizerGrouped32MaxLocalBp) + 2u),
        MinimizerGenerationWireError::position_range);
    require_wire_error(
        localize_minimizer_grouped32_position(9, false, 10, 20, legacy),
        MinimizerGenerationWireError::position_range);
    require_wire_error(
        localize_minimizer_grouped32_position(20, false, 10, 20, legacy),
        MinimizerGenerationWireError::position_range);

    constexpr std::array<std::byte, 4> corrupt_out_of_segment{
        std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
    };
    require_wire_error(
        decode_minimizer_grouped32_position(
            corrupt_out_of_segment, 1'000, 1'100, legacy),
        MinimizerGenerationWireError::position_range);
    require_wire_error(
        decode_minimizer_grouped32_position(
            std::span<const std::byte>{
                corrupt_out_of_segment.data(),
                corrupt_out_of_segment.size() - 1u},
            1'000,
            1'100,
            legacy),
        MinimizerGenerationWireError::encoded_size);

    constexpr std::array<std::byte, 4> local_two{
        std::byte{0x04}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    };
    require_wire_error(
        decode_minimizer_grouped32_position(
            local_two,
            (std::numeric_limits<std::uint64_t>::max)() - 1u,
            (std::numeric_limits<std::uint64_t>::max)(),
            legacy),
        MinimizerGenerationWireError::arithmetic_overflow);

    auto unsupported = legacy;
    unsupported.kmer_size = 16;
    require_wire_error(
        encode_minimizer_grouped32_position(0, unsupported),
        MinimizerGenerationWireError::compatibility_mismatch);
    require_wire_error(
        decode_minimizer_grouped32_position(
            local_two, 0, 10, unsupported),
        MinimizerGenerationWireError::compatibility_mismatch);
}

TEST_CASE("DOC0371 directory entry is an exact 96-byte little-endian ABI",
          "[indexing][minimizer-hashmap][wire][directory]") {
    using namespace bioseq::indexing;
    constexpr MinimizerBucketDirEntry entry{
        .partition_id = 0x11223344U,
        .position_segment = 0x10203040,
        .lo_hash = 0x0102030405060708ULL,
        .hi_hash = 0x1112131415161718ULL,
        .sub_key = 0x1122232425262728LL,
        .control_first_chunk = 0,
        .control_chunk_count = 2,
        .position_first_chunk = 0,
        .position_chunk_count = 3,
        .control_element_count = 768,
        .position_element_count = 2304,
        .control_encoded_bytes = 12288,
        .position_encoded_bytes = 18432,
        .control_checksum = 0x3132333435363738ULL,
        .position_checksum = 0x4142434445464748ULL,
    };
    constexpr auto encoded = encode_minimizer_bucket_dir_entry(
        entry, kTestV3LegacyProfile);
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
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded, 40) == 0);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded, 44) == 3);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 48) == 768);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 56) == 2304);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 64) == 12288);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 72) == 18432);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 80)
        == entry.control_checksum);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 88)
        == entry.position_checksum);
    constexpr auto decoded = decode_minimizer_bucket_dir_entry(
        *encoded, kTestV3LegacyProfile);
    STATIC_REQUIRE(decoded.has_value());
    STATIC_REQUIRE(*decoded == entry);

    require_wire_error(
        decode_minimizer_bucket_dir_entry(
            std::span<const std::byte>{encoded->data(), encoded->size() - 1u},
            kTestV3LegacyProfile),
        MinimizerGenerationWireError::encoded_size);
    auto invalid = entry;
    invalid.position_segment = -1;
    require_wire_error(encode_minimizer_bucket_dir_entry(
                           invalid, kTestV3LegacyProfile),
                       MinimizerGenerationWireError::position_segment);
    invalid = entry;
    invalid.lo_hash = invalid.hi_hash + 1u;
    require_wire_error(encode_minimizer_bucket_dir_entry(
                           invalid, kTestV3LegacyProfile),
                       MinimizerGenerationWireError::hash_range);
    invalid = entry;
    invalid.sub_key = POSITION_EXTENT_BASE;
    require_wire_error(encode_minimizer_bucket_dir_entry(
                           invalid, kTestV3LegacyProfile),
                       MinimizerGenerationWireError::sub_key);
    invalid = entry;
    invalid.control_encoded_bytes -= 16u;
    require_wire_error(encode_minimizer_bucket_dir_entry(
                           invalid, kTestV3LegacyProfile),
                       MinimizerGenerationWireError::byte_count);
    invalid = entry;
    invalid.position_first_chunk = 1;
    require_wire_error(encode_minimizer_bucket_dir_entry(
                           invalid, kTestV3LegacyProfile),
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
        .position_first_chunk = 0,
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
    auto bytes = encode_minimizer_bucket_directory(
        entries, kTestV3LegacyProfile);
    REQUIRE(bytes.has_value());
    auto view = DirView::decode(*bytes, kTestV3LegacyProfile);
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
    require_wire_error(encode_minimizer_bucket_directory(
                           overlapping, kTestV3LegacyProfile),
                       MinimizerGenerationWireError::directory_order);
    auto wrong_ordinal = second;
    wrong_ordinal.partition_id = 7;
    const std::array wrong_ordinals{first, wrong_ordinal};
    require_wire_error(encode_minimizer_bucket_directory(
                           wrong_ordinals, kTestV3LegacyProfile),
                       MinimizerGenerationWireError::partition_id);
    std::array<std::byte, 95> truncated{};
    require_wire_error(DirView::decode(truncated, kTestV3LegacyProfile),
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
        REQUIRE(entry.sub_key >= 0);
        REQUIRE(entry.sub_key < POSITION_EXTENT_BASE);
        REQUIRE(entry.control_first_chunk == 0);
        REQUIRE(entry.position_first_chunk == 0);
        const auto position_sub_key =
            derive_minimizer_position_extent_sub_key(entry.sub_key);
        REQUIRE(position_sub_key.has_value());
        const auto control_first_row = std::ranges::find_if(
            direct_sink.rows,
            [entry](const CollectedMinimizerRow& row) {
                return row.address.position_segment == entry.position_segment
                    && row.address.sub_key == entry.sub_key
                    && row.address.chunk_no == 0;
            });
        const auto position_first_row = std::ranges::find_if(
            direct_sink.rows,
            [entry, position_sub_key](const CollectedMinimizerRow& row) {
                return row.address.position_segment == entry.position_segment
                    && row.address.sub_key == *position_sub_key
                    && row.address.chunk_no == 0;
            });
        REQUIRE(control_first_row != direct_sink.rows.end());
        REQUIRE(position_first_row != direct_sink.rows.end());
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

TEST_CASE("inactive v4 collecting sink preserves v3 semantics above 4G",
          "[indexing][minimizer-hashmap][wire][encoder][v4][parity]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t span = kMinimizerPreferredSegmentSpanBp;
    constexpr std::uint64_t high_position = (std::uint64_t{1} << 32u) + 37u;
    constexpr auto encoded_position = [](
            std::uint64_t position, std::uint64_t strand) {
        return (position << 1u) | strand;
    };
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {0x01020304u, encoded_position(3u, 1u)},
        {0x01020304u, encoded_position(9u, 0u)},
        {7u, encoded_position(span + 5u, 1u)},
        {kLegacyMinimapK15MaxHash,
            encoded_position(high_position, 1u)},
    };
    const std::uint64_t target_length = high_position + 100u;

    VectorReplayMinimizerProducer v3_producer{minimizers};
    CollectingMinimizerRowSink v3_sink;
    const auto v3 = encode_minimizer_generation(
        v3_producer, v3_sink, test_generation_options(target_length));
    REQUIRE(v3.has_value());

    VectorReplayMinimizerProducer v4_producer{minimizers};
    CollectingMinimizerRowSink v4_sink;
    auto v4_options = test_grouped32_generation_options(target_length);
    ++v4_options.generation_id;
    const auto v4 = encode_minimizer_generation(
        v4_producer, v4_sink, v4_options);
    REQUIRE(v4.has_value());
    REQUIRE(v3_producer.scan_count == 2u);
    REQUIRE(v4_producer.scan_count == 2u);
    REQUIRE(v4->directory.profile() == kTestV4LegacyProfile);
    REQUIRE(v4->manifest.gpu_abi_version == kMinimizerGrouped32GpuAbiVersion);
    REQUIRE(v3->manifest.control_count == v4->manifest.control_count);
    REQUIRE(v3->manifest.position_count == v4->manifest.position_count);
    REQUIRE(v3->manifest.payload_bytes
        == 16u * v3->manifest.control_count
            + 8u * v3->manifest.position_count);
    REQUIRE(v4->manifest.payload_bytes
        == kMinimizerGrouped32ControlEncodedBytes
                * v4->manifest.control_count
            + kMinimizerGrouped32PositionEncodedBytes
                * v4->manifest.position_count);
    REQUIRE(v4->manifest.payload_bytes < v3->manifest.payload_bytes);

    const auto v3_events = decode_generation_events(*v3, v3_sink.rows);
    const auto v4_events = decode_generation_events(*v4, v4_sink.rows);
    REQUIRE(v4_events == v3_events);
    REQUIRE(std::ranges::any_of(v4_events, [](const auto& event) {
        return (event.encoded_position >> 1u) > (std::uint64_t{1} << 32u);
    }));

    for (const MinimizerBucketDirEntry& entry : v4->directory.entries()) {
        REQUIRE(entry.control_encoded_bytes
            == entry.control_element_count
                * kMinimizerGrouped32ControlEncodedBytes);
        REQUIRE(entry.position_encoded_bytes
            == entry.position_element_count
                * kMinimizerGrouped32PositionEncodedBytes);
        REQUIRE(entry.control_chunk_count
            == entry.control_encoded_bytes / kMinimizerExtentPayloadBytes
                + (entry.control_encoded_bytes
                    % kMinimizerExtentPayloadBytes != 0 ? 1u : 0u));
        REQUIRE(entry.position_chunk_count
            == entry.position_encoded_bytes / kMinimizerExtentPayloadBytes
                + (entry.position_encoded_bytes
                    % kMinimizerExtentPayloadBytes != 0 ? 1u : 0u));
    }

    const MinimizerBucketDirEntry& first = v4->directory.entries().front();
    const auto first_controls = collect_encoded_extent(
        v4_sink.rows, first, true);
    const auto first_positions = collect_encoded_extent(
        v4_sink.rows, first, false);
    const std::array<std::byte, 12> expected_control{
        std::byte{0x04}, std::byte{0x03}, std::byte{0x02}, std::byte{0x01},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    };
    const std::array<std::byte, 8> expected_positions{
        std::byte{0x07}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x12}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    };
    REQUIRE(std::ranges::equal(first_controls, expected_control));
    REQUIRE(std::ranges::equal(first_positions, expected_positions));
    require_complete_generation_rows(*v3, v3_sink.rows);
    require_complete_generation_rows(*v4, v4_sink.rows);
}

TEST_CASE("inactive v4 planner enforces scheme-qualified hash bounds",
          "[indexing][minimizer-hashmap][wire][encoder][v4][bounds]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> legacy_overflow{
        {static_cast<std::uint64_t>(kLegacyMinimapK15MaxHash) + 1u, 2u},
    };
    VectorReplayMinimizerProducer legacy_producer{legacy_overflow};
    CollectingMinimizerRowSink legacy_sink;
    require_wire_error(
        encode_minimizer_generation(
            legacy_producer,
            legacy_sink,
            test_grouped32_generation_options(100u)),
        MinimizerGenerationWireError::hash_range);
    REQUIRE(legacy_producer.scan_count == 1u);
    REQUIRE(legacy_sink.rows.empty());

    const std::vector<std::pair<std::uint64_t, std::uint64_t>> paper_maximum{
        {(std::numeric_limits<std::uint32_t>::max)(), 2u},
    };
    VectorReplayMinimizerProducer paper_producer{
        paper_maximum, kPaperNtHashU32R7SumV1SchemeIdentity};
    CollectingMinimizerRowSink paper_sink;
    auto paper_options = test_grouped32_generation_options(100u);
    paper_options.minimizer_scheme_id = kPaperNtHashU32R7SumV1SchemeId;
    paper_options.minimizer_scheme_version =
        kPaperNtHashU32R7SumV1SchemeVersion;
    const auto paper = encode_minimizer_generation(
        paper_producer, paper_sink, paper_options);
    REQUIRE(paper.has_value());
    REQUIRE(decode_generation_events(*paper, paper_sink.rows).size() == 1u);
    require_complete_generation_rows(*paper, paper_sink.rows);
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
    const MpfGenerationExecutionEvidence scalar_execution =
        mpf_producer.execution_evidence();
    REQUIRE(scalar_execution.accepted());
    REQUIRE(scalar_execution.selected_backend
        == MinimizerSketchBackend::Scalar);
    REQUIRE(scalar_execution.scalar_core_runs > 0);
    REQUIRE(scalar_execution.exact_simd_core_runs == 0);
    REQUIRE(scalar_execution.paper_nthash_core_runs == 0);
    REQUIRE(scalar_execution.vulkan_core_runs == 0);
    REQUIRE(mpf_producer.peak_buffered_core_bp()
        <= kMinimizerPreferredSegmentSpanBp);

    FakeFragmentStorage<> spill_storage;
    auto spilled = MpsIndex::spill_generation_replay_runs_serial(
        spill_storage,
        lazy,
        MinimizerSketchBackend::Scalar,
        fragment_bp);
    REQUIRE(spilled.has_value());
    REQUIRE(spilled->run_count() >= 3);
    const std::uint64_t scalar_spill_run_count = spilled->run_count();
    auto mps_producer = MpsIndex::make_generation_replay_producer(
        spill_storage, range.bp_size(), std::move(*spilled));
    CollectingMinimizerRowSink mps_sink;
    auto mps_options = mpf_options;
    ++mps_options.generation_id;
    auto mps = encode_minimizer_generation(
        mps_producer, mps_sink, mps_options);
    REQUIRE(mps.has_value());
    REQUIRE(mps_producer.scan_count() == 2);
    REQUIRE(mps_producer.merge_pass_count() == 2);
    REQUIRE(spill_storage.segment_merge_calls() == 3);
    REQUIRE(spill_storage.segment_merge_run_visits()
        == 3u * spill_storage.segment_spill_run_count());
    REQUIRE(spill_storage.segment_merge_bytes_visited()
        == 3u * spill_storage.segment_spill_bytes());
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
    const MpfGenerationExecutionEvidence exact_execution =
        exact_mpf_producer.execution_evidence();
    REQUIRE(exact_execution.accepted());
    REQUIRE(exact_execution.selected_backend
        == MinimizerSketchBackend::ExactSimd);
    REQUIRE(exact_execution.scalar_core_runs == 0);
    REQUIRE(exact_execution.exact_simd_core_runs > 0);
    REQUIRE(exact_execution.paper_nthash_core_runs == 0);
    REQUIRE(exact_execution.vulkan_core_runs == 0);
    REQUIRE(exact_execution.exact_simd_proof_present);
    REQUIRE(exact_execution.exact_simd_proof.accepted(true));

    FakeFragmentStorage<> exact_spill_storage;
    auto exact_spilled = MpsIndex::spill_generation_replay_runs_serial(
        exact_spill_storage,
        lazy_exact,
        MinimizerSketchBackend::ExactSimd,
        fragment_bp);
    REQUIRE(exact_spilled.has_value());
    REQUIRE(exact_spilled->run_count() == scalar_spill_run_count);
    auto exact_mps_producer = MpsIndex::make_generation_replay_producer(
        exact_spill_storage, range.bp_size(), std::move(*exact_spilled));
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

TEST_CASE("inactive format-4 MPS evidence binds Scalar and Exact spill replay",
          "[indexing][minimizer-hashmap][wire][encoder][mps][evidence]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 128;
    constexpr std::uint64_t fragment_bp = fragment_units * 32u;
    using chunked = sequence_scanner_test_chunked<
        std::uint64_t, fragment_units>;
    using MpsIndex = MinimizerSequenceMergedIndex<FakeFragmentStorage<>>;
    auto data = make_dna4(18013, 0x4D50534556494445ULL);
    range_t range(data);

    const auto verify_backend = [&](MinimizerSketchBackend backend) {
        lazy_compact_sequence<dna4_u64, chunked> lazy{
            chunked::from_range(range)};
        FakeFragmentStorage<> storage;
        MpsGenerationExecutionEvidence spill_evidence;
        auto spill = MpsIndex::spill_generation_replay_runs_serial(
            storage,
            lazy,
            backend,
            fragment_bp,
            kMinimizerPreferredSegmentSpanBp,
            &spill_evidence);
        REQUIRE(spill.has_value());
        REQUIRE(spill_evidence.spill_accepted());
        REQUIRE(spill_evidence.spill_operation_epoch != 0);
        REQUIRE(spill->execution_evidence().spill_accepted());
        REQUIRE(spill_evidence.peak_core_span_bp <= fragment_bp);
        REQUIRE(spill_evidence.spilled_event_count > 0);
        REQUIRE(spill_evidence.spill_run_count == spill->run_count());

        CollectingMinimizerRowSink sink;
        MpsGenerationExecutionEvidence execution;
        const auto encoded = MpsIndex::encode_persisted_generation(
            storage,
            sink,
            std::move(*spill),
            test_grouped32_generation_options(range.bp_size()),
            {},
            &execution);
        REQUIRE(encoded.has_value());
        REQUIRE(execution.accepted());
        const MinimizerGenerationPublicationBinding publication_binding =
            minimizer_generation_publication_binding(*encoded);
        REQUIRE(execution.publication_accepted(publication_binding));
        auto spliced_publication_binding = publication_binding;
        ++spliced_publication_binding.generation_id;
        REQUIRE_FALSE(execution.publication_accepted(
            spliced_publication_binding));
        spliced_publication_binding = publication_binding;
        spliced_publication_binding.source_identity ^= 1u;
        REQUIRE_FALSE(execution.publication_accepted(
            spliced_publication_binding));
        REQUIRE(execution.ready_release_binding_accepted(
            storage.segment_spill_storage_binding()));
        REQUIRE(execution.manifest_bound);
        REQUIRE(execution.manifest_position_count
            == execution.spilled_event_count);
        REQUIRE(execution.replay_scan_count == 2);
        REQUIRE(execution.replay_merge_pass_count == 2);
        REQUIRE(execution.replay_reroute_pass_count == 0);
        REQUIRE(execution.replay_completed_scan_count == 2);
        REQUIRE(execution.replay_failed_scan_count == 0);
        REQUIRE(execution.replay_first_completed_event_count
            == execution.spilled_event_count);
        REQUIRE(execution.replay_last_completed_event_count
            == execution.spilled_event_count);
        REQUIRE(execution.replay_visited_event_count
            == 2u * execution.spilled_event_count);
        auto mixed_route = execution;
        mixed_route.paper_nthash_core_runs = 1;
        REQUIRE_FALSE(mixed_route.accepted());
        auto laundered_vulkan = execution;
        laundered_vulkan.selected_backend =
            MinimizerSketchBackend::VulkanSegment;
        laundered_vulkan.scalar_core_runs = 0;
        laundered_vulkan.exact_simd_core_runs = 0;
        laundered_vulkan.exact_simd_proof = {};
        laundered_vulkan.exact_simd_proof_present = false;
        laundered_vulkan.vulkan_core_runs = 1;
        laundered_vulkan.vulkan_proof.completed_roundtrips = 1;
        REQUIRE_FALSE(laundered_vulkan.spill_accepted());
        REQUIRE_FALSE(laundered_vulkan.accepted());
        auto replay_tamper = execution;
        ++replay_tamper.replay_scan_count;
        REQUIRE_FALSE(replay_tamper.accepted());
        auto unbound = execution;
        unbound.manifest_bound = false;
        REQUIRE_FALSE(unbound.accepted());
        auto checksum_tamper = execution;
        checksum_tamper.spill_content_digest[0] ^= std::byte{1};
        REQUIRE_FALSE(checksum_tamper.spill_accepted());
        REQUIRE_FALSE(checksum_tamper.accepted());
        if (backend == MinimizerSketchBackend::Scalar) {
            REQUIRE(execution.scalar_core_runs > 0);
            REQUIRE(execution.exact_simd_core_runs == 0);
            REQUIRE_FALSE(execution.exact_simd_proof_present);
        } else {
            REQUIRE(execution.scalar_core_runs == 0);
            REQUIRE(execution.exact_simd_core_runs > 0);
            REQUIRE(execution.exact_simd_proof_present);
            REQUIRE(execution.exact_simd_proof.accepted(true));
            auto missing_native_proof = execution;
            missing_native_proof.exact_simd_proof_present = false;
            REQUIRE_FALSE(missing_native_proof.accepted());
        }
        REQUIRE(execution.paper_nthash_core_runs == 0);
        REQUIRE(execution.vulkan_core_runs == 0);
        require_complete_generation_rows(*encoded, sink.rows);
    };

    SECTION("Scalar") {
        verify_backend(MinimizerSketchBackend::Scalar);
    }
    SECTION("Exact SIMD") {
        verify_backend(MinimizerSketchBackend::ExactSimd);
    }
}

TEST_CASE("Format-5 MPS exact SIMD binds the canonical shape profile",
          "[indexing][minimizer-hashmap][wire][encoder][mps][v5]"
          "[exact-simd][fail-closed]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 128;
    constexpr std::uint64_t fragment_bp = fragment_units * 32u;
    using chunked = sequence_scanner_test_chunked<
        std::uint64_t, fragment_units>;
    using MpsIndex = MinimizerSequenceMergedIndex<FakeFragmentStorage<>>;
    auto data = make_dna4(18013, 0x4D5053464D543556ULL);
    range_t range(data);

    const auto spill_exact = [&](FakeFragmentStorage<>& storage) {
        lazy_compact_sequence<dna4_u64, chunked> lazy{
            chunked::from_range(range)};
        return MpsIndex::spill_generation_replay_runs_serial(
            storage,
            lazy,
            MinimizerSketchBackend::ExactSimd,
            fragment_bp,
            kMinimizerPreferredSegmentSpanBp);
    };

    SECTION("canonical Format-5 profile is published with native SIMD proof") {
        FakeFragmentStorage<> storage;
        auto spill = spill_exact(storage);
        REQUIRE(spill.has_value());
        REQUIRE(spill->execution_evidence().spill_accepted());

        const auto options =
            test_shape_v5_generation_options(range.bp_size());
        CollectingMinimizerRowSink sink;
        MpsGenerationExecutionEvidence execution;
        const auto encoded = MpsIndex::encode_persisted_generation(
            storage,
            sink,
            std::move(*spill),
            options,
            {},
            &execution);
        REQUIRE(encoded.has_value());
        REQUIRE(encoded->manifest.wire_format
            == MinimizerGenerationWireFormat::
                grouped_shape_soa_segment_local);
        REQUIRE(encoded->manifest.alphabet_code
            == kMinimizerPersistedDna4AlphabetCode);
        REQUIRE(encoded->manifest.gpu_abi_version
            == kMinimizerShapeV5GpuAbiVersion);
        REQUIRE(encoded->manifest.shape_v5.has_value());
        REQUIRE(encoded->manifest.shape_v5->max_occ_cap
            == options.max_occ_cap);

        const auto canonical_profile = minimizer_generation_payload_profile(
            options.wire_format,
            kLegacyMinimapV1SchemeIdentity,
            options.alphabet_code,
            options.kmer_size,
            encoded->manifest.position_segment_span_bp,
            options.max_occ_cap);
        REQUIRE(canonical_profile.has_value());
        REQUIRE(encoded->directory.profile() == *canonical_profile);
        REQUIRE(encoded->manifest.shape_v5
            == canonical_profile->shape_v5);

        REQUIRE(execution.accepted());
        REQUIRE(execution.generation_wire_format
            == options.wire_format);
        REQUIRE(execution.selected_backend
            == MinimizerSketchBackend::ExactSimd);
        REQUIRE(execution.scalar_core_runs == 0u);
        REQUIRE(execution.exact_simd_core_runs > 0u);
        REQUIRE(execution.exact_simd_proof_present);
        REQUIRE(execution.exact_simd_proof.accepted(true));
        REQUIRE(execution.paper_nthash_core_runs == 0u);
        REQUIRE(execution.vulkan_core_runs == 0u);
        REQUIRE(execution.replay_scan_count == 3u);
        REQUIRE(execution.replay_merge_pass_count == 3u);
        REQUIRE(execution.replay_completed_scan_count == 3u);
        REQUIRE(execution.replay_failed_scan_count == 0u);
        REQUIRE(execution.replay_visited_event_count
            == 3u * execution.spilled_event_count);
        REQUIRE(sink.format5_payload_write_calls > 0u);
        REQUIRE(sink.format5_row_crc_bytes
            == encoded->manifest.payload_bytes);
        require_complete_generation_rows(*encoded, sink.rows);
    }

    const auto require_profile_rejection = [
            &](MinimizerGenerationEncodeOptions options) {
        FakeFragmentStorage<> storage;
        auto spill = spill_exact(storage);
        REQUIRE(spill.has_value());
        CollectingMinimizerRowSink sink;
        MpsGenerationExecutionEvidence execution;
        require_wire_error(
            MpsIndex::encode_persisted_generation(
                storage,
                sink,
                std::move(*spill),
                options,
                {},
                &execution),
            MinimizerGenerationWireError::compatibility_mismatch);
        REQUIRE(sink.rows.empty());
        REQUIRE_FALSE(execution.accepted());
    };

    SECTION("noncanonical DNA4 cardinality cannot masquerade as pack code") {
        auto options = test_shape_v5_generation_options(range.bp_size());
        options.alphabet_code = 4u;
        require_profile_rejection(options);
    }

    SECTION("invalid Format-5 count profile has no legacy fallback") {
        auto options = test_shape_v5_generation_options(range.bp_size());
        options.max_occ_cap =
            (std::numeric_limits<std::uint64_t>::max)();
        require_profile_rejection(options);
    }

    SECTION("unapproved MPS wire formats remain rejected") {
        auto options = test_shape_v5_generation_options(range.bp_size());
        options.wire_format =
            MinimizerGenerationWireFormat::sorted_dense_global64;
        require_profile_rejection(options);
    }
}

TEST_CASE("MPS SHA-256 binding matches standard known vectors and split updates",
          "[indexing][minimizer-hashmap][wire][encoder][mps][sha256]") {
    using namespace bioseq::indexing;
    const auto require_hex = [](const MpsContentDigest& digest,
                                std::string_view expected) {
        REQUIRE(expected.size() == digest.size() * 2u);
        const auto nibble = [](char value) -> std::uint8_t {
            if (value >= '0' && value <= '9') {
                return static_cast<std::uint8_t>(value - '0');
            }
            return static_cast<std::uint8_t>(value - 'a' + 10);
        };
        for (std::size_t index = 0; index < digest.size(); ++index) {
            const std::uint8_t value = static_cast<std::uint8_t>(
                (nibble(expected[index * 2u]) << 4u)
                | nibble(expected[index * 2u + 1u]));
            REQUIRE(std::to_integer<std::uint8_t>(digest[index]) == value);
        }
    };

    mps_evidence_detail::Sha256 empty;
    require_hex(
        empty.finalize(),
        "e3b0c44298fc1c149afbf4c8996fb924"
        "27ae41e4649b934ca495991b7852b855");

    const auto bytes = [](std::string_view value) {
        return std::as_bytes(std::span{value.data(), value.size()});
    };
    mps_evidence_detail::Sha256 contiguous;
    contiguous.update(bytes("abc"));
    const MpsContentDigest contiguous_digest = contiguous.finalize();
    require_hex(
        contiguous_digest,
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad");

    mps_evidence_detail::Sha256 split;
    split.update(bytes("a"));
    split.update(bytes("b"));
    split.update(bytes("c"));
    REQUIRE(split.finalize() == contiguous_digest);
}

TEST_CASE("inactive format-4 MPS spill authority is non-forgeable and detects mutation",
          "[indexing][minimizer-hashmap][wire][encoder][mps][authority][fail-closed]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 64;
    constexpr std::uint64_t fragment_bp = fragment_units * 32u;
    using chunked = sequence_scanner_test_chunked<
        std::uint64_t, fragment_units>;
    using MpsIndex = MinimizerSequenceMergedIndex<FakeFragmentStorage<>>;
    using Spill = MpsIndex::GenerationSpillResult;
    STATIC_REQUIRE_FALSE(std::is_aggregate_v<MpsGenerationExecutionEvidence>);
    STATIC_REQUIRE_FALSE(std::is_aggregate_v<Spill>);
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<Spill>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<Spill>);
    STATIC_REQUIRE(std::is_move_constructible_v<Spill>);
    using BoundProducer =
        MpsExternalSpillGenerationReplayProducer<FakeFragmentStorage<>>;
    using RawProducer =
        MpsUnboundExternalSpillReplayProducer<FakeFragmentStorage<>>;
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<BoundProducer>);
    STATIC_REQUIRE_FALSE(std::is_move_constructible_v<BoundProducer>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<RawProducer>);
    STATIC_REQUIRE_FALSE(std::is_move_constructible_v<RawProducer>);

    MpsGenerationExecutionEvidence forged;
    forged.spill_complete = true;
    forged.backend_bound = true;
    forged.manifest_bound = true;
    forged.target_length = 4096;
    forged.window_size = 10;
    forged.kmer_size = 15;
    forged.configured_core_span_bp = fragment_bp;
    forged.selected_backend = MinimizerSketchBackend::Scalar;
    forged.minimizer_scheme = kLegacyMinimapV1SchemeIdentity;
    forged.scalar_core_runs = 1;
    forged.spill_run_count = 1;
    forged.spilled_event_count = 1;
    forged.spill_qualification_scan_count = 1;
    forged.spill_qualification_merge_count = 1;
    forged.replay_scan_count = 2;
    forged.replay_merge_pass_count = 2;
    forged.replay_completed_scan_count = 2;
    forged.replay_first_completed_event_count = 1;
    forged.replay_last_completed_event_count = 1;
    forged.manifest_position_count = 1;
    REQUIRE_FALSE(forged.spill_accepted());
    REQUIRE_FALSE(forged.accepted());

    auto data = make_dna4(4096, 0x4D50534D55544154ULL);
    range_t range(data);
    lazy_compact_sequence<dna4_u64, chunked> lazy{
        chunked::from_range(range)};
    FakeFragmentStorage<> storage;
    MpsGenerationExecutionEvidence spill_evidence;
    auto spill = MpsIndex::spill_generation_replay_runs_serial(
        storage,
        lazy,
        MinimizerSketchBackend::Scalar,
        fragment_bp,
        kMinimizerPreferredSegmentSpanBp,
        &spill_evidence);
    REQUIRE(spill.has_value());
    REQUIRE(spill_evidence.spill_accepted());
    REQUIRE_FALSE(storage.segment_runs_.empty());
    std::pair<std::uint64_t, std::uint64_t>* first = nullptr;
    std::pair<std::uint64_t, std::uint64_t>* second = nullptr;
    for (auto& [segment, runs] : storage.segment_runs_) {
        (void)segment;
        for (auto& run : runs) {
            for (auto& event : run) {
                if (first == nullptr) {
                    first = &event;
                } else if (event.first != first->first) {
                    second = &event;
                    break;
                }
            }
            if (second != nullptr) break;
        }
        if (second != nullptr) break;
    }
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    REQUIRE(first->second != second->second);
    // Preserve event count, hash multiset, position multiset, their sums and
    // their XORs: this defeats the former algebraic fingerprint pattern while
    // changing the canonical (hash,position) stream that SHA-256 binds.
    std::swap(first->second, second->second);
    for (auto& [segment, runs] : storage.segment_runs_) {
        (void)segment;
        for (auto& run : runs) std::ranges::sort(run);
    }

    CollectingMinimizerRowSink sink;
    MpsGenerationExecutionEvidence execution;
    const auto encoded = MpsIndex::encode_persisted_generation(
        storage,
        sink,
        std::move(*spill),
        test_grouped32_generation_options(range.bp_size()),
        {},
        &execution);
    require_wire_error(encoded, MinimizerGenerationWireError::replay_mismatch);
    REQUIRE_FALSE(execution.accepted());
    REQUIRE(execution.replay_scan_count == 1);
    REQUIRE(execution.replay_completed_scan_count == 0);
    REQUIRE(execution.replay_failed_scan_count == 1);
    REQUIRE(sink.rows.empty());
}

TEST_CASE("format-4 MPF execution evidence is sealed to one publication identity",
          "[indexing][minimizer-hashmap][wire][encoder][mpf][publication][fail-closed]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 64;
    using chunked = sequence_scanner_test_chunked<
        std::uint64_t, fragment_units>;
    using Index = MinimizersPerFragmentIndex<FakeFragmentStorage<>>;
    auto data = make_dna4(4096, 0x4D50465055424C49ULL);
    range_t range(data);
    lazy_compact_sequence<dna4_u64, chunked> lazy{
        chunked::from_range(range)};
    CollectingMinimizerRowSink sink;
    MpfGenerationExecutionEvidence execution;
    const auto encoded = Index::encode_persisted_generation(
        lazy,
        MinimizerSketchBackend::Scalar,
        sink,
        test_grouped32_generation_options(range.bp_size()),
        {},
        &execution);
    REQUIRE(encoded.has_value());
    const MinimizerGenerationPublicationBinding binding =
        minimizer_generation_publication_binding(*encoded);
    REQUIRE(execution.publication_accepted(binding));

    auto spliced = binding;
    ++spliced.generation_id;
    REQUIRE_FALSE(execution.publication_accepted(spliced));
    spliced = binding;
    spliced.source_identity ^= 1u;
    REQUIRE_FALSE(execution.publication_accepted(spliced));

    auto mutated = execution;
    mutated.selected_backend = MinimizerSketchBackend::ExactSimd;
    REQUIRE_FALSE(mutated.publication_accepted(binding));

    /* Scalar and Vulkan intentionally share the legacy minimizer scheme, so
     * forge every public route field into a structurally accepted Vulkan
     * claim.  The private operation snapshot must still reject publication. */
    mutated = execution;
    mutated.selected_backend = MinimizerSketchBackend::VulkanSegment;
    mutated.scalar_core_runs = 0u;
    mutated.vulkan_core_runs = 1u;
    mutated.vulkan_proof.completed_roundtrips = 1u;
    REQUIRE(mutated.accepted());
    REQUIRE_FALSE(mutated.publication_accepted(binding));

    mutated = execution;
    ++mutated.scalar_core_runs;
    REQUIRE(mutated.accepted());
    REQUIRE_FALSE(mutated.publication_accepted(binding));

    mutated = execution;
    ++mutated.geometry_retry_count;
    ++mutated.producer_scan_count;
    REQUIRE(mutated.accepted());
    REQUIRE_FALSE(mutated.publication_accepted(binding));

    mutated = execution;
    ++mutated.target_length;
    REQUIRE(mutated.accepted());
    REQUIRE_FALSE(mutated.publication_accepted(binding));

    mutated = execution;
    ++mutated.vulkan_proof.completed_roundtrips;
    REQUIRE_FALSE(mutated.publication_accepted(binding));

    mutated = execution;
    mutated.generation_wire_format =
        MinimizerGenerationWireFormat::sorted_dense_global64;
    REQUIRE_FALSE(mutated.publication_accepted(binding));
    require_complete_generation_rows(*encoded, sink.rows);
}

TEST_CASE("inactive format-4 MPS accepts real empty and shorter-than-window spills",
          "[indexing][minimizer-hashmap][wire][encoder][mps][short]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 64;
    constexpr std::uint64_t fragment_bp = fragment_units * 32u;
    constexpr std::size_t minimum_work_bases = 10u + 15u - 1u;
    using chunked = sequence_scanner_test_chunked<
        std::uint64_t, fragment_units>;
    using MpsIndex = MinimizerSequenceMergedIndex<FakeFragmentStorage<>>;

    for (std::size_t length = 0; length < minimum_work_bases; ++length) {
        INFO("short MPS target length=" << length);
        auto data = make_dna4(length, 0x4D505353484F5254ULL + length);
        range_t range(data);
        lazy_compact_sequence<dna4_u64, chunked> lazy{
            chunked::from_range(range)};
        FakeFragmentStorage<> storage;
        MpsGenerationExecutionEvidence spill_evidence;
        auto spill = MpsIndex::spill_generation_replay_runs_serial(
            storage,
            lazy,
            MinimizerSketchBackend::Scalar,
            fragment_bp,
            kMinimizerPreferredSegmentSpanBp,
            &spill_evidence);
        REQUIRE(spill.has_value());
        REQUIRE(spill_evidence.spill_accepted());
        REQUIRE(spill_evidence.spilled_event_count == 0);
        REQUIRE(spill_evidence.spill_run_count == 0);
        REQUIRE(spill_evidence.spill_qualification_scan_count == 1);
        REQUIRE(spill_evidence.spill_qualification_merge_count
            == (length == 0 ? 0u : 1u));
        REQUIRE(spill_evidence.scalar_core_runs == 0);
        REQUIRE(spill_evidence.exact_simd_core_runs == 0);
        REQUIRE(spill_evidence.paper_nthash_core_runs == 0);
        REQUIRE(spill_evidence.vulkan_core_runs == 0);
        REQUIRE(spill_evidence.short_core_runs
            == (length == 0 ? 0u : 1u));

        CollectingMinimizerRowSink sink;
        MpsGenerationExecutionEvidence execution;
        const auto encoded = MpsIndex::encode_persisted_generation(
            storage,
            sink,
            std::move(*spill),
            test_grouped32_generation_options(length),
            {},
            &execution);
        INFO("MPS encode wire_error="
            << (encoded.has_value()
                ? 0u : static_cast<unsigned>(encoded.error())));
        REQUIRE(encoded.has_value());
        REQUIRE(execution.accepted());
        REQUIRE(execution.replay_scan_count == 2);
        REQUIRE(execution.replay_completed_scan_count == 2);
        REQUIRE(execution.replay_failed_scan_count == 0);
        REQUIRE(execution.replay_reroute_pass_count == 0);
        REQUIRE(execution.replay_merge_pass_count
            == (length == 0 ? 0u : 2u));
        REQUIRE(storage.segment_merge_calls()
            == (length == 0 ? 0u : 3u));
        REQUIRE(encoded->manifest.position_count == 0);
        REQUIRE(encoded->manifest.position_segment_span_bp
            == (length == 0 ? 0u : kMinimizerPreferredSegmentSpanBp));
        require_complete_generation_rows(*encoded, sink.rows);
    }
}

TEST_CASE("inactive MPS storage incarnation rejects duplicate runs ABA and reuse",
          "[indexing][minimizer-hashmap][wire][encoder][mps][incarnation][fail-closed]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 64;
    constexpr std::uint64_t fragment_bp = fragment_units * 32u;
    using chunked = sequence_scanner_test_chunked<
        std::uint64_t, fragment_units>;
    using MpsIndex = MinimizerSequenceMergedIndex<FakeFragmentStorage<>>;
    auto data = make_dna4(4096, 0x494E4341524E4154ULL);
    range_t range(data);

    SECTION("duplicate physical run advances the storage mutation version") {
        lazy_compact_sequence<dna4_u64, chunked> lazy{
            chunked::from_range(range)};
        FakeFragmentStorage<> storage;
        auto spill = MpsIndex::spill_generation_replay_runs_serial(
            storage, lazy, MinimizerSketchBackend::Scalar, fragment_bp);
        REQUIRE(spill.has_value());
        const MpsSegmentSpillStorageBinding before =
            storage.segment_spill_storage_binding();
        REQUIRE_FALSE(storage.segment_runs_.empty());
        const auto segment = storage.segment_runs_.begin()->first;
        REQUIRE_FALSE(storage.segment_runs_.begin()->second.empty());
        const auto duplicate = storage.segment_runs_.begin()->second.front();
        storage.spill_sorted_segment_run(segment, duplicate);
        const MpsSegmentSpillStorageBinding after =
            storage.segment_spill_storage_binding();
        REQUIRE(after.generation_token == before.generation_token);
        REQUIRE(after.mutation_version == before.mutation_version + 1u);
        CollectingMinimizerRowSink sink;
        MpsGenerationExecutionEvidence execution;
        require_wire_error(
            MpsIndex::encode_persisted_generation(
                storage,
                sink,
                std::move(*spill),
                test_grouped32_generation_options(range.bp_size()),
                {},
                &execution),
            MinimizerGenerationWireError::compatibility_mismatch);
        REQUIRE_FALSE(execution.accepted());
        REQUIRE(sink.rows.empty());
    }

    SECTION("wrong storage and moved-from authority cannot be laundered") {
        lazy_compact_sequence<dna4_u64, chunked> lazy{
            chunked::from_range(range)};
        FakeFragmentStorage<> original;
        FakeFragmentStorage<> replacement;
        auto spill = MpsIndex::spill_generation_replay_runs_serial(
            original, lazy, MinimizerSketchBackend::Scalar, fragment_bp);
        REQUIRE(spill.has_value());
        CollectingMinimizerRowSink wrong_sink;
        require_wire_error(
            MpsIndex::encode_persisted_generation(
                replacement,
                wrong_sink,
                std::move(*spill),
                test_grouped32_generation_options(range.bp_size())),
            MinimizerGenerationWireError::compatibility_mismatch);
        REQUIRE(wrong_sink.rows.empty());

        CollectingMinimizerRowSink accepted_sink;
        MpsGenerationExecutionEvidence execution;
        const auto accepted = MpsIndex::encode_persisted_generation(
            original,
            accepted_sink,
            std::move(*spill),
            test_grouped32_generation_options(range.bp_size()),
            {},
            &execution);
        REQUIRE(accepted.has_value());
        REQUIRE(execution.accepted());
        REQUIRE(spill->run_count() == 0);
        REQUIRE(spill->target_length() == 0);
        REQUIRE(spill->position_segment_span_bp() == 0);
        REQUIRE_FALSE(spill->execution_evidence().spill_accepted());

        CollectingMinimizerRowSink reused_sink;
        require_wire_error(
            MpsIndex::encode_persisted_generation(
                original,
                reused_sink,
                std::move(*spill),
                test_grouped32_generation_options(range.bp_size())),
            MinimizerGenerationWireError::compatibility_mismatch);
        REQUIRE(reused_sink.rows.empty());
    }

    SECTION("release and new begin invalidate even an empty authority") {
        std::vector<seq::dna4> empty_data;
        range_t empty_range(empty_data);
        lazy_compact_sequence<dna4_u64, chunked> empty_lazy{
            chunked::from_range(empty_range)};
        FakeFragmentStorage<> storage;
        auto spill = MpsIndex::spill_generation_replay_runs_serial(
            storage,
            empty_lazy,
            MinimizerSketchBackend::Scalar,
            fragment_bp);
        REQUIRE(spill.has_value());
        const MpsSegmentSpillStorageBinding issued =
            storage.segment_spill_storage_binding();
        REQUIRE(issued.valid());
        storage.release_runs_noexcept();
        const MpsSegmentSpillStorageBinding released =
            storage.segment_spill_storage_binding();
        REQUIRE(released.generation_token == issued.generation_token);
        REQUIRE(released.mutation_version == issued.mutation_version + 1u);
        const MpsSegmentSpillStorageBinding replacement =
            storage.begin_segment_spill_generation();
        REQUIRE(replacement.valid());
        REQUIRE(replacement.generation_token != issued.generation_token);
        CollectingMinimizerRowSink sink;
        require_wire_error(
            MpsIndex::encode_persisted_generation(
                storage,
                sink,
                std::move(*spill),
                test_grouped32_generation_options(0)),
            MinimizerGenerationWireError::compatibility_mismatch);
        REQUIRE(sink.rows.empty());
    }

    SECTION("moving storage cannot preserve address-bound authority") {
        lazy_compact_sequence<dna4_u64, chunked> lazy{
            chunked::from_range(range)};
        FakeFragmentStorage<> original;
        auto spill = MpsIndex::spill_generation_replay_runs_serial(
            original, lazy, MinimizerSketchBackend::Scalar, fragment_bp);
        REQUIRE(spill.has_value());
        const MpsSegmentSpillStorageBinding issued =
            original.segment_spill_storage_binding();
        FakeFragmentStorage<> moved{std::move(original)};
        REQUIRE_FALSE(moved.segment_spill_storage_binding().valid());
        CollectingMinimizerRowSink sink;
        require_wire_error(
            MpsIndex::encode_persisted_generation(
                moved,
                sink,
                std::move(*spill),
                test_grouped32_generation_options(range.bp_size())),
            MinimizerGenerationWireError::compatibility_mismatch);
        REQUIRE(sink.rows.empty());
    }

    SECTION("moving an empty spill invalidates the moved-from authority") {
        std::vector<seq::dna4> empty_data;
        range_t empty_range(empty_data);
        lazy_compact_sequence<dna4_u64, chunked> empty_lazy{
            chunked::from_range(empty_range)};
        FakeFragmentStorage<> original;
        auto spill = MpsIndex::spill_generation_replay_runs_serial(
            original,
            empty_lazy,
            MinimizerSketchBackend::Scalar,
            fragment_bp);
        REQUIRE(spill.has_value());
        REQUIRE(original.segment_spill_storage_binding().valid());

        FakeFragmentStorage<> moved{std::move(original)};
        REQUIRE_FALSE(original.segment_spill_storage_binding().valid());
        REQUIRE_FALSE(moved.segment_spill_storage_binding().valid());
        CollectingMinimizerRowSink sink;
        require_wire_error(
            MpsIndex::encode_persisted_generation(
                original,
                sink,
                std::move(*spill),
                test_grouped32_generation_options(0)),
            MinimizerGenerationWireError::compatibility_mismatch);
        REQUIRE(sink.rows.empty());
    }

    SECTION("representation-only run compaction preserves logical incarnation") {
        lazy_compact_sequence<dna4_u64, chunked> lazy{
            chunked::from_range(range)};
        FakeFragmentStorage<> storage;
        auto spill = MpsIndex::spill_generation_replay_runs_serial(
            storage, lazy, MinimizerSketchBackend::Scalar, fragment_bp);
        REQUIRE(spill.has_value());
        const MpsSegmentSpillStorageBinding before =
            storage.segment_spill_storage_binding();
        const std::uint64_t physical_runs_before =
            storage.segment_spill_run_count();
        REQUIRE(physical_runs_before > storage.segment_runs_.size());

        storage.compact_segment_spill_representation_preserving();
        REQUIRE(storage.segment_spill_storage_binding() == before);
        REQUIRE(storage.segment_spill_run_count() < physical_runs_before);

        CollectingMinimizerRowSink sink;
        MpsGenerationExecutionEvidence execution;
        const auto encoded = MpsIndex::encode_persisted_generation(
            storage,
            sink,
            std::move(*spill),
            test_grouped32_generation_options(range.bp_size()),
            {},
            &execution);
        REQUIRE(encoded.has_value());
        REQUIRE(execution.accepted());
        REQUIRE(execution.ready_release_binding_accepted(
            storage.segment_spill_storage_binding()));
        const auto segment = storage.segment_runs_.begin()->first;
        const auto post_encode_splice =
            storage.segment_runs_.begin()->second.front();
        storage.spill_sorted_segment_run(segment, post_encode_splice);
        REQUIRE_FALSE(execution.ready_release_binding_accepted(
            storage.segment_spill_storage_binding()));
        require_complete_generation_rows(*encoded, sink.rows);
    }

    SECTION("bound producer and consumed spill are single-use") {
        lazy_compact_sequence<dna4_u64, chunked> lazy{
            chunked::from_range(range)};
        FakeFragmentStorage<> storage;
        auto spill = MpsIndex::spill_generation_replay_runs_serial(
            storage, lazy, MinimizerSketchBackend::Scalar, fragment_bp);
        REQUIRE(spill.has_value());
        auto producer = MpsIndex::make_generation_replay_producer(
            storage, range.bp_size(), std::move(*spill));
        CollectingMinimizerRowSink first_sink;
        const auto first = encode_minimizer_generation(
            producer,
            first_sink,
            test_generation_options(range.bp_size()));
        REQUIRE(first.has_value());
        CollectingMinimizerRowSink second_sink;
        require_wire_error(
            encode_minimizer_generation(
                producer,
                second_sink,
                test_generation_options(range.bp_size())),
            MinimizerGenerationWireError::segment_span);

        auto moved_from_producer = MpsIndex::make_generation_replay_producer(
            storage, range.bp_size(), std::move(*spill));
        CollectingMinimizerRowSink moved_from_sink;
        require_wire_error(
            encode_minimizer_generation(
                moved_from_producer,
                moved_from_sink,
                test_generation_options(range.bp_size())),
            MinimizerGenerationWireError::compatibility_mismatch);
        REQUIRE(moved_from_sink.rows.empty());
    }
}

TEST_CASE("inactive MPS storage incarnation counters fail closed at wrap",
          "[indexing][minimizer-hashmap][wire][encoder][mps][incarnation][overflow]") {
    using namespace bioseq::indexing;
    // The uncommon maximum chunk parameter gives this test an isolated static
    // token namespace, independent of every ordinary FakeFragmentStorage<>.
    using WrapStorage = FakeFragmentStorage<123457u>;

    WrapStorage::set_next_segment_spill_generation_token_for_test(
        (std::numeric_limits<std::uint64_t>::max)());
    WrapStorage last_valid;
    const MpsSegmentSpillStorageBinding maximum =
        last_valid.begin_segment_spill_generation();
    REQUIRE(maximum.valid());
    REQUIRE(maximum.generation_token
        == (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE(maximum.mutation_version == 0);

    WrapStorage exhausted;
    REQUIRE_FALSE(exhausted.begin_segment_spill_generation().valid());
    REQUIRE_FALSE(exhausted.begin_segment_spill_generation().valid());

    last_valid.set_segment_spill_mutation_version_for_test(
        (std::numeric_limits<std::uint64_t>::max)());
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> run{{
        {1u, 2u},
    }};
    (void)last_valid.spill_sorted_segment_run(0u, run);
    REQUIRE_FALSE(last_valid.segment_spill_storage_binding().valid());
}

TEST_CASE("inactive format-4 MPS reuses checked bounded packed Paper",
          "[indexing][minimizer-hashmap][wire][encoder][mps][paper][evidence]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 64;
    constexpr std::uint64_t fragment_bp = fragment_units * 32u;
    constexpr std::uint64_t halo = 10u + 15u - 2u;
    using chunked = sequence_scanner_test_chunked<
        std::uint64_t, fragment_units>;
    using MpsIndex = MinimizerSequenceMergedIndex<FakeFragmentStorage<>>;
    auto data = make_dna4(12013, 0x4D50535041504552ULL);
    range_t range(data);
    sketch_collector resident_collector;
    const auto resident_result = collect_packed_paper_nthash_index_erased(
        PackedPaperNthashIndexBridgeRequest{
            .sequence_context = &range,
            .collector_context = &resident_collector,
            .alphabet = PackedPaperNthashIndexAlphabet::dna4,
            .storage_unit_bytes = sizeof(std::uint64_t),
            .letters_per_unit = range_t::traits::letters_per_unit,
            .storage_units = range.size(),
            .local_bp_size = range.bp_size(),
            .window_size = 10,
            .kmer_size = 15,
        });
    if (resident_result.status
        == PackedPaperNthashIndexStatus::unsupported_vector_shape) {
        SKIP("the compiled Highway target cannot execute packed Paper ntHash");
    }
    REQUIRE(resident_result.status == PackedPaperNthashIndexStatus::ok);
    REQUIRE(resident_result.proof.accepted(true));
    std::vector<CanonicalMinimizerEvent> oracle;
    oracle.reserve(resident_collector.extract_sketches_data().size());
    for (const MinimapSketchData& minimizer
         : resident_collector.extract_sketches_data()) {
        oracle.push_back(CanonicalMinimizerEvent{
            .hash = minimizer.kmer(),
            .encoded_position = minimizer.pos_strand_encoded(),
        });
    }
    std::ranges::sort(oracle, {}, [](const CanonicalMinimizerEvent& event) {
        return std::pair{event.hash, event.encoded_position};
    });
    oracle.erase(std::ranges::unique(oracle).begin(), oracle.end());
    lazy_compact_sequence<dna4_u64, chunked> lazy{
        chunked::from_range(range)};
    FakeFragmentStorage<> storage;
    MpsGenerationExecutionEvidence spill_evidence;
    auto spill = MpsIndex::spill_generation_replay_runs_serial(
        storage,
        lazy,
        MinimizerSketchBackend::PaperNtHashSimd,
        fragment_bp,
        kMinimizerPreferredSegmentSpanBp,
        &spill_evidence);
    if (!spill && spill_evidence.packed_paper_status
            == PackedPaperNthashIndexStatus::unsupported_vector_shape) {
        SKIP("the compiled Highway target cannot execute packed Paper ntHash");
    }
    REQUIRE(spill.has_value());
    REQUIRE(spill_evidence.spill_accepted());
    REQUIRE(spill_evidence.scalar_core_runs == 0);
    REQUIRE(spill_evidence.exact_simd_core_runs == 0);
    REQUIRE(spill_evidence.paper_nthash_core_runs > 0);
    REQUIRE(spill_evidence.vulkan_core_runs == 0);
    REQUIRE(spill_evidence.packed_paper_proof_present);
    REQUIRE(spill_evidence.packed_paper_status
        == PackedPaperNthashIndexStatus::ok);
    REQUIRE(spill_evidence.packed_paper_proof.accepted(true));
    REQUIRE(spill_evidence.packed_paper_proof.fallback_calls == 0);
    REQUIRE(spill_evidence.peak_paper_materialized_window_bp
        <= fragment_bp + 2u * halo
            + range_t::traits::letters_per_unit - 1u);
    REQUIRE(spill_evidence.peak_paper_materialized_window_bp
        < range.bp_size());

    auto options = test_grouped32_generation_options(range.bp_size());
    options.minimizer_scheme_id = kPaperNtHashU32R7SumV1SchemeId;
    options.minimizer_scheme_version =
        kPaperNtHashU32R7SumV1SchemeVersion;
    CollectingMinimizerRowSink sink;
    MpsGenerationExecutionEvidence execution;
    const auto encoded = MpsIndex::encode_persisted_generation(
        storage, sink, std::move(*spill), options, {}, &execution);
    REQUIRE(encoded.has_value());
    REQUIRE(execution.accepted());
    REQUIRE(execution.selected_backend
        == MinimizerSketchBackend::PaperNtHashSimd);
    REQUIRE(execution.packed_paper_proof_present);
    REQUIRE(execution.packed_paper_proof.accepted(true));
    REQUIRE(execution.packed_paper_proof.fallback_calls == 0);
    REQUIRE(decode_generation_events(*encoded, sink.rows) == oracle);
    require_complete_generation_rows(*encoded, sink.rows);
}

TEST_CASE("inactive format-4 MPS canonicalizes periodic Paper ties within admission",
          "[indexing][minimizer-hashmap][wire][encoder][mps][paper][duplicate][admission]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 1024;
    constexpr std::uint64_t fragment_bp = fragment_units * 32u;
    using chunked = sequence_scanner_test_chunked<
        std::uint64_t, fragment_units>;
    using MpsIndex = MinimizerSequenceMergedIndex<FakeFragmentStorage<>>;

    std::vector<seq::dna4> data(65565);
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index].assign_rank(static_cast<std::uint8_t>(index & 3u));
    }
    range_t range(data);
    sketch_collector resident_collector;
    const auto resident_result = collect_packed_paper_nthash_index_erased(
        PackedPaperNthashIndexBridgeRequest{
            .sequence_context = &range,
            .collector_context = &resident_collector,
            .alphabet = PackedPaperNthashIndexAlphabet::dna4,
            .storage_unit_bytes = sizeof(std::uint64_t),
            .letters_per_unit = range_t::traits::letters_per_unit,
            .storage_units = range.size(),
            .local_bp_size = range.bp_size(),
            .window_size = 10,
            .kmer_size = 15,
        });
    if (resident_result.status
        == PackedPaperNthashIndexStatus::unsupported_vector_shape) {
        SKIP("the compiled Highway target cannot execute packed Paper ntHash");
    }
    REQUIRE(resident_result.status == PackedPaperNthashIndexStatus::ok);
    REQUIRE(resident_result.proof.accepted(true));

    std::vector<CanonicalMinimizerEvent> canonical;
    canonical.reserve(resident_collector.extract_sketches_data().size());
    for (const MinimapSketchData& minimizer
         : resident_collector.extract_sketches_data()) {
        canonical.push_back(CanonicalMinimizerEvent{
            .hash = minimizer.kmer(),
            .encoded_position = minimizer.pos_strand_encoded(),
        });
    }
    const std::size_t raw_event_count = canonical.size();
    std::ranges::sort(canonical, {}, [](const CanonicalMinimizerEvent& event) {
        return std::pair{event.hash, event.encoded_position};
    });
    canonical.erase(
        std::ranges::unique(canonical).begin(), canonical.end());
    REQUIRE(raw_event_count > canonical.size());

    lazy_compact_sequence<dna4_u64, chunked> lazy{
        chunked::from_range(range)};
    FakeFragmentStorage<> storage;
    MpsGenerationExecutionEvidence spill_evidence;
    auto spill = MpsIndex::spill_generation_replay_runs_serial(
        storage,
        lazy,
        MinimizerSketchBackend::PaperNtHashSimd,
        fragment_bp,
        kMinimizerPreferredSegmentSpanBp,
        &spill_evidence);
    REQUIRE(spill.has_value());
    REQUIRE(spill_evidence.spill_accepted());
    REQUIRE(spill_evidence.spilled_event_count == canonical.size());
    REQUIRE(spill_evidence.paper_nthash_core_runs > 0u);
    REQUIRE(spill_evidence.scalar_core_runs == 0u);
    REQUIRE(spill_evidence.exact_simd_core_runs == 0u);
    REQUIRE(spill_evidence.vulkan_core_runs == 0u);
    REQUIRE(spill_evidence.packed_paper_proof.accepted(true));
    REQUIRE(spill_evidence.packed_paper_proof.fallback_calls == 0u);

    auto options = test_grouped32_generation_options(range.bp_size());
    options.minimizer_scheme_id = kPaperNtHashU32R7SumV1SchemeId;
    options.minimizer_scheme_version =
        kPaperNtHashU32R7SumV1SchemeVersion;
    CollectingMinimizerRowSink sink;
    MpsGenerationExecutionEvidence execution;
    const auto encoded = MpsIndex::encode_persisted_generation(
        storage, sink, std::move(*spill), options, {}, &execution);
    REQUIRE(encoded.has_value());
    REQUIRE(execution.accepted());
    REQUIRE(execution.spilled_event_count == canonical.size());
    REQUIRE(decode_generation_events(*encoded, sink.rows) == canonical);
    require_complete_generation_rows(*encoded, sink.rows);
}

TEST_CASE("inactive format-4 MPS Paper rejects DNA5 without fallback",
          "[indexing][minimizer-hashmap][wire][encoder][mps][paper][fail-closed]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 64;
    constexpr std::uint64_t fragment_bp = fragment_units * 21u;
    using chunked = sequence_scanner_test_chunked<
        std::uint64_t, fragment_units>;
    using MpsIndex = MinimizerSequenceMergedIndex<FakeFragmentStorage<>>;
    std::vector<seq::dna5> data(4096);
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index].assign_rank(static_cast<std::uint8_t>(index % 5u));
    }
    dna5_range_t range(data);
    lazy_compact_sequence<dna5_u64, chunked> lazy{
        chunked::from_range(range)};
    FakeFragmentStorage<> storage;
    MpsGenerationExecutionEvidence evidence;
    auto spill = MpsIndex::spill_generation_replay_runs_serial(
        storage,
        lazy,
        MinimizerSketchBackend::PaperNtHashSimd,
        fragment_bp,
        kMinimizerPreferredSegmentSpanBp,
        &evidence);
    require_wire_error(
        spill, MinimizerGenerationWireError::compatibility_mismatch);
    REQUIRE_FALSE(evidence.spill_complete);
    REQUIRE_FALSE(evidence.spill_accepted());
    REQUIRE(evidence.scalar_core_runs == 0);
    REQUIRE(evidence.exact_simd_core_runs == 0);
    REQUIRE(evidence.paper_nthash_core_runs == 1);
    REQUIRE(evidence.vulkan_core_runs == 0);
    REQUIRE(evidence.packed_paper_proof_present);
    REQUIRE(evidence.packed_paper_status
        == PackedPaperNthashIndexStatus::unsupported_alphabet);
    REQUIRE(evidence.packed_paper_proof.fail_closed_rejections == 1);
    REQUIRE(evidence.packed_paper_proof.fallback_calls == 0);
    REQUIRE(storage.segment_spill_run_count() == 0);
}

TEST_CASE("DOC0371 selected Vulkan evidence requires a dispatch at w+k-1",
          "[indexing][minimizer-hashmap][wire][encoder][vulkan][boundary]") {
    using namespace bioseq::indexing;
    MpfGenerationExecutionEvidence evidence;
    evidence.selected_backend = MinimizerSketchBackend::VulkanSegment;
    evidence.minimizer_scheme = kLegacyMinimapV1SchemeIdentity;
    evidence.target_length = 10u + 15u - 1u;
    evidence.window_size = 10u;
    evidence.kmer_size = 15u;
    evidence.producer_scan_count = 2u;
    evidence.vulkan_core_runs = 2u;
    evidence.backend_bound = true;
    REQUIRE_FALSE(evidence.accepted());

    evidence.vulkan_proof.completed_roundtrips = 2u;
    REQUIRE(evidence.accepted());
}

TEST_CASE("inactive format-4 MPS carries operation-originated Vulkan proof",
          "[indexing][minimizer-hashmap][wire][encoder][mps][vulkan][evidence]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 64;
    constexpr std::uint64_t fragment_bp = fragment_units * 32u;
    using chunked = sequence_scanner_test_chunked<
        std::uint64_t, fragment_units>;
    using MpsIndex = MinimizerSequenceMergedIndex<FakeFragmentStorage<>>;
    auto data = make_dna4(4096, 0x4D505356554C4B4EULL);
    range_t range(data);
    if (!::bioseq::gpu::vulkan::hardware_device_available(
            ::bioseq::gpu::vulkan::Runtime::instance())) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    lazy_compact_sequence<dna4_u64, chunked> lazy{
        chunked::from_range(range)};
    FakeFragmentStorage<> storage;
    MpsGenerationExecutionEvidence spill_evidence;
    auto spill = MpsIndex::spill_generation_replay_runs_serial(
        storage,
        lazy,
        MinimizerSketchBackend::VulkanSegment,
        fragment_bp,
        kMinimizerPreferredSegmentSpanBp,
        &spill_evidence);
    REQUIRE(spill.has_value());
    REQUIRE(spill_evidence.spill_accepted());
    REQUIRE(spill_evidence.scalar_core_runs == 0);
    REQUIRE(spill_evidence.exact_simd_core_runs == 0);
    REQUIRE(spill_evidence.paper_nthash_core_runs == 0);
    REQUIRE(spill_evidence.vulkan_core_runs > 0);
    REQUIRE(spill_evidence.vulkan_proof.accepted(true));

    CollectingMinimizerRowSink sink;
    MpsGenerationExecutionEvidence execution;
    const auto encoded = MpsIndex::encode_persisted_generation(
        storage,
        sink,
        std::move(*spill),
        test_grouped32_generation_options(range.bp_size()),
        {},
        &execution);
    REQUIRE(encoded.has_value());
    REQUIRE(execution.accepted());
    REQUIRE(execution.vulkan_proof.accepted(true));
    REQUIRE(execution.scalar_core_runs == 0);
    REQUIRE(execution.exact_simd_core_runs == 0);
    REQUIRE(execution.paper_nthash_core_runs == 0);
    require_complete_generation_rows(*encoded, sink.rows);
}

TEST_CASE("bounded Paper MPF replay matches one checked resident execution",
          "[indexing][minimizer-hashmap][wire][encoder][paper][simd]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 64;
    constexpr std::uint64_t fragment_bp = fragment_units * 32u;
    constexpr std::uint64_t halo = 10u + 15u - 2u;
    using chunked = sequence_scanner_test_chunked<
        std::uint64_t, fragment_units>;
    using Index = MinimizersPerFragmentIndex<FakeFragmentStorage<>>;

    auto data = make_dna4(70013, 0x50415045524E5448ULL);
    range_t range(data);
    sketch_collector resident_collector;
    const auto resident_result = collect_packed_paper_nthash_index_erased(
        PackedPaperNthashIndexBridgeRequest{
            .sequence_context = &range,
            .collector_context = &resident_collector,
            .alphabet = PackedPaperNthashIndexAlphabet::dna4,
            .storage_unit_bytes = sizeof(std::uint64_t),
            .letters_per_unit = range_t::traits::letters_per_unit,
            .storage_units = range.size(),
            .local_bp_size = range.bp_size(),
            .window_size = 10,
            .kmer_size = 15,
        });
    if (resident_result.status
        == PackedPaperNthashIndexStatus::unsupported_vector_shape) {
        SKIP("the compiled Highway target cannot execute packed Paper ntHash");
    }
    REQUIRE(resident_result.status == PackedPaperNthashIndexStatus::ok);
    REQUIRE(resident_result.proof.accepted(true));

    std::vector<CanonicalMinimizerEvent> oracle;
    oracle.reserve(resident_collector.extract_sketches_data().size());
    for (const MinimapSketchData& minimizer :
         resident_collector.extract_sketches_data()) {
        oracle.push_back({
            .hash = minimizer.kmer(),
            .encoded_position = minimizer.pos_strand_encoded(),
        });
    }
    std::ranges::sort(oracle, {}, [](const CanonicalMinimizerEvent& event) {
        return std::pair{event.hash, event.encoded_position};
    });

    lazy_compact_sequence<dna4_u64, chunked> lazy{
        chunked::from_range(range)};
    auto producer = Index::make_generation_replay_producer(
        lazy, MinimizerSketchBackend::PaperNtHashSimd);
    auto options = test_generation_options(range.bp_size());
    options.minimizer_scheme_id = kPaperNtHashU32R7SumV1SchemeId;
    options.minimizer_scheme_version =
        kPaperNtHashU32R7SumV1SchemeVersion;
    CollectingMinimizerRowSink sink;
    const auto encoded = encode_minimizer_generation(
        producer, sink, options);
    REQUIRE(encoded.has_value());
    REQUIRE(producer.scan_count() == 2);
    const MpfGenerationExecutionEvidence execution =
        producer.execution_evidence();
    REQUIRE(execution.accepted());
    REQUIRE(execution.selected_backend
        == MinimizerSketchBackend::PaperNtHashSimd);
    REQUIRE(execution.scalar_core_runs == 0);
    REQUIRE(execution.exact_simd_core_runs == 0);
    REQUIRE(execution.paper_nthash_core_runs > 0);
    REQUIRE(execution.vulkan_core_runs == 0);
    REQUIRE(execution.packed_paper_proof_present);
    REQUIRE(execution.packed_paper_status
        == PackedPaperNthashIndexStatus::ok);
    REQUIRE(execution.packed_paper_proof.accepted(true));
    REQUIRE(execution.packed_paper_proof.fallback_calls == 0);
    REQUIRE(producer.minimizer_scheme_identity()
        == kPaperNtHashU32R7SumV1SchemeIdentity);
    REQUIRE(producer.packed_paper_proof_present());
    REQUIRE(producer.packed_paper_status()
        == PackedPaperNthashIndexStatus::ok);
    REQUIRE(producer.packed_paper_proof().accepted(true));
    REQUIRE(producer.packed_paper_proof().fallback_calls == 0);
    REQUIRE_FALSE(producer.exact_simd_proof_present());
    REQUIRE(producer.peak_paper_materialized_window_bp()
        <= fragment_bp + 2u * halo
            + range_t::traits::letters_per_unit - 1u);
    REQUIRE(producer.peak_paper_materialized_window_bp()
        < range.bp_size());
    REQUIRE(decode_generation_events(*encoded, sink.rows) == oracle);
    require_complete_generation_rows(*encoded, sink.rows);
}

TEST_CASE("Paper MPF canonicalizes tied periodic events identically across replays",
          "[indexing][minimizer-hashmap][wire][encoder][paper][simd][duplicate]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 64;
    using chunked = sequence_scanner_test_chunked<
        std::uint64_t, fragment_units>;
    using Index = MinimizersPerFragmentIndex<FakeFragmentStorage<>>;

    // This is the periodic shape that exposed duplicate_event in the live MPF
    // coordinator. Tied adjacent windows can report the same canonical Paper
    // minimizer more than once, including after intervening raw records.
    std::vector<seq::dna4> data(65565);
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index].assign_rank(static_cast<std::uint8_t>(index & 3u));
    }
    range_t range(data);
    sketch_collector raw_collector;
    const auto raw_result = collect_packed_paper_nthash_index_erased(
        PackedPaperNthashIndexBridgeRequest{
            .sequence_context = &range,
            .collector_context = &raw_collector,
            .alphabet = PackedPaperNthashIndexAlphabet::dna4,
            .storage_unit_bytes = sizeof(std::uint64_t),
            .letters_per_unit = range_t::traits::letters_per_unit,
            .storage_units = range.size(),
            .local_bp_size = range.bp_size(),
            .window_size = 10,
            .kmer_size = 15,
        });
    if (raw_result.status
        == PackedPaperNthashIndexStatus::unsupported_vector_shape) {
        SKIP("the compiled Highway target cannot execute packed Paper ntHash");
    }
    REQUIRE(raw_result.status == PackedPaperNthashIndexStatus::ok);
    REQUIRE(raw_result.proof.accepted(true));

    std::vector<CanonicalMinimizerEvent> raw;
    raw.reserve(raw_collector.extract_sketches_data().size());
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::size_t> first_seen;
    bool saw_nonadjacent_duplicate = false;
    for (const MinimapSketchData& minimizer :
         raw_collector.extract_sketches_data()) {
        const CanonicalMinimizerEvent event{
            .hash = minimizer.kmer(),
            .encoded_position = minimizer.pos_strand_encoded(),
        };
        const auto key = std::pair{event.hash, event.encoded_position};
        if (const auto found = first_seen.find(key);
            found != first_seen.end()) {
            saw_nonadjacent_duplicate |= raw.size() > found->second + 1u;
        } else {
            first_seen.emplace(key, raw.size());
        }
        raw.push_back(event);
    }
    auto canonical = raw;
    std::ranges::sort(canonical, {}, [](const CanonicalMinimizerEvent& event) {
        return std::pair{event.hash, event.encoded_position};
    });
    canonical.erase(
        std::ranges::unique(canonical).begin(), canonical.end());
    REQUIRE(raw.size() > canonical.size());
    REQUIRE(saw_nonadjacent_duplicate);

    lazy_compact_sequence<dna4_u64, chunked> lazy{
        chunked::from_range(range)};
    auto replay = Index::make_generation_replay_producer(
        lazy, MinimizerSketchBackend::PaperNtHashSimd);
    const auto collect_replay = [](void* context,
            const CanonicalMinimizerEvent& event)
            -> std::expected<void, MinimizerGenerationWireError> {
        static_cast<std::vector<CanonicalMinimizerEvent>*>(context)
            ->push_back(event);
        return {};
    };
    std::vector<CanonicalMinimizerEvent> first_replay;
    std::vector<CanonicalMinimizerEvent> second_replay;
    REQUIRE(replay.scan_sorted_events(
        kMinimizerPreferredSegmentSpanBp,
        {.context = &first_replay, .emit_fn = collect_replay}).has_value());
    REQUIRE(replay.scan_sorted_events(
        kMinimizerPreferredSegmentSpanBp,
        {.context = &second_replay, .emit_fn = collect_replay}).has_value());
    REQUIRE(first_replay == canonical);
    REQUIRE(second_replay == canonical);
    REQUIRE(first_replay == second_replay);
    REQUIRE(replay.scan_count() == 2u);
    REQUIRE(replay.execution_evidence().accepted());
    REQUIRE(replay.execution_evidence().selected_backend
        == MinimizerSketchBackend::PaperNtHashSimd);
    REQUIRE(replay.execution_evidence().scalar_core_runs == 0u);
    REQUIRE(replay.execution_evidence().exact_simd_core_runs == 0u);
    REQUIRE(replay.execution_evidence().vulkan_core_runs == 0u);
    REQUIRE(replay.execution_evidence().paper_nthash_core_runs > 0u);

    auto encoder_replay = Index::make_generation_replay_producer(
        lazy, MinimizerSketchBackend::PaperNtHashSimd);
    auto options = test_generation_options(range.bp_size());
    options.minimizer_scheme_id = kPaperNtHashU32R7SumV1SchemeId;
    options.minimizer_scheme_version =
        kPaperNtHashU32R7SumV1SchemeVersion;
    CollectingMinimizerRowSink sink;
    const auto encoded = encode_minimizer_generation(
        encoder_replay, sink, options);
    REQUIRE(encoded.has_value());
    REQUIRE(encoder_replay.scan_count() == 2u);
    REQUIRE(encoder_replay.execution_evidence().accepted());
    REQUIRE(decode_generation_events(*encoded, sink.rows) == canonical);
    require_complete_generation_rows(*encoded, sink.rows);
}

TEST_CASE("Paper MPF replay rejects incompatible biological and storage shapes",
          "[indexing][minimizer-hashmap][wire][encoder][paper][fail-closed]") {
    using namespace bioseq::indexing;
    using Index = MinimizersPerFragmentIndex<FakeFragmentStorage<>>;

    SECTION("DNA5 is not silently normalized to the Paper DNA4 kernel") {
        using chunked = sequence_scanner_test_chunked<std::uint64_t, 64>;
        std::vector<seq::dna5> data(4096);
        for (std::size_t index = 0; index < data.size(); ++index) {
            data[index].assign_rank(static_cast<std::uint8_t>(index % 5u));
        }
        dna5_range_t range(data);
        lazy_compact_sequence<dna5_u64, chunked> lazy{
            chunked::from_range(range)};
        auto producer = Index::make_generation_replay_producer(
            lazy, MinimizerSketchBackend::PaperNtHashSimd);
        auto options = test_generation_options(range.bp_size());
        options.alphabet_code = 5;
        options.minimizer_scheme_id = kPaperNtHashU32R7SumV1SchemeId;
        options.minimizer_scheme_version =
            kPaperNtHashU32R7SumV1SchemeVersion;
        CollectingMinimizerRowSink sink;
        require_wire_error(
            encode_minimizer_generation(producer, sink, options),
            MinimizerGenerationWireError::compatibility_mismatch);
        REQUIRE(sink.rows.empty());
        REQUIRE(producer.packed_paper_proof_present());
        REQUIRE(producer.packed_paper_status()
            == PackedPaperNthashIndexStatus::unsupported_alphabet);
        REQUIRE(producer.packed_paper_proof().fail_closed_rejections == 1);
        REQUIRE_FALSE(producer.exact_simd_proof_present());
    }

    SECTION("u16 packed storage cannot enter the checked u32 Paper view") {
        using chunked = sequence_scanner_test_chunked<std::uint16_t, 64>;
        auto data = make_dna4(4096, 0x16B17u);
        dna4_u16_range_t range(data);
        lazy_compact_sequence<dna4_u16, chunked> lazy{
            chunked::from_range(range)};
        auto producer = Index::make_generation_replay_producer(
            lazy, MinimizerSketchBackend::PaperNtHashSimd);
        auto options = test_generation_options(range.bp_size());
        options.storage_unit_byte_width = sizeof(std::uint16_t);
        options.minimizer_scheme_id = kPaperNtHashU32R7SumV1SchemeId;
        options.minimizer_scheme_version =
            kPaperNtHashU32R7SumV1SchemeVersion;
        CollectingMinimizerRowSink sink;
        require_wire_error(
            encode_minimizer_generation(producer, sink, options),
            MinimizerGenerationWireError::compatibility_mismatch);
        REQUIRE(sink.rows.empty());
        REQUIRE(producer.packed_paper_proof_present());
        REQUIRE(producer.packed_paper_status()
            == PackedPaperNthashIndexStatus::unsupported_storage);
        REQUIRE(producer.packed_paper_proof().fail_closed_rejections == 1);
        REQUIRE_FALSE(producer.exact_simd_proof_present());
    }

    SECTION("a local position wider than MinimapSketchData fails before dereference") {
        std::byte sentinel{};
        sketch_collector collector;
        const std::size_t local_bp_size = static_cast<std::size_t>(
            MinimapSketchData::pos_field_mask) + 2u;
        const std::size_t storage_units =
            (local_bp_size + range_t::traits::letters_per_unit - 1u)
            / range_t::traits::letters_per_unit;
        const auto rejected = collect_packed_paper_nthash_index_erased(
            PackedPaperNthashIndexBridgeRequest{
                .sequence_context = &sentinel,
                .collector_context = &collector,
                .alphabet = PackedPaperNthashIndexAlphabet::dna4,
                .storage_unit_bytes = sizeof(std::uint64_t),
                .letters_per_unit = range_t::traits::letters_per_unit,
                .storage_units = storage_units,
                .local_bp_size = local_bp_size,
                .window_size = 10,
                .kmer_size = 15,
            });
        REQUIRE(rejected.status
            == PackedPaperNthashIndexStatus::unsupported_representation);
        REQUIRE(rejected.proof.fail_closed_rejections == 1);
        REQUIRE(collector.extract_sketches_data().empty());
    }
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
    auto spilled = MpsIndex::spill_generation_replay_runs_serial(
        spill_storage,
        lazy,
        MinimizerSketchBackend::Scalar,
        fragment_bp);
    REQUIRE(spilled.has_value());
    REQUIRE(spilled->run_count() > 256);
    REQUIRE(spill_storage.runs_.empty());
    REQUIRE(spill_storage.segment_spill_run_count() == spilled->run_count());
    REQUIRE(spill_storage.segment_runs_.size() == 2);
    constexpr std::uint64_t half_span = span / 2u;
    std::vector<std::uint64_t> half_counts(
        range.bp_size() / half_span
            + (range.bp_size() % half_span != 0 ? 1u : 0u));
    for (const auto& [segment, runs] : spill_storage.segment_runs_) {
        (void)segment;
        for (const auto& run : runs) {
            for (const auto& event : run) {
                ++half_counts[(event.second >> 1u) / half_span];
            }
        }
    }
    REQUIRE(half_counts.size() == 3);
    REQUIRE(half_counts[0] > 0);
    REQUIRE(half_counts[1] > 0);
    const std::uint64_t half_max =
        *std::ranges::max_element(half_counts);
    REQUIRE(half_counts[0] + half_counts[1] > half_max);
    MinimizerGenerationFixedWidthLimits one_half_per_segment;
    one_half_per_segment.max_segment_position_count = half_max;
    CollectingMinimizerRowSink mps_sink;
    MpsGenerationExecutionEvidence execution;
    const auto encoded = MpsIndex::encode_persisted_generation(
        spill_storage,
        mps_sink,
        std::move(*spilled),
        test_grouped32_generation_options(range.bp_size()),
        one_half_per_segment,
        &execution);
    REQUIRE(encoded.has_value());
    REQUIRE(execution.accepted());
    REQUIRE(execution.replay_scan_count == 3);
    REQUIRE(execution.replay_merge_pass_count == 3);
    REQUIRE(execution.replay_reroute_pass_count == 1);
    REQUIRE(execution.replay_failed_scan_count == 1);
    REQUIRE(execution.replay_binding_checked_failed_scan_count == 1);
    REQUIRE(execution.replay_completed_scan_count == 2);
    REQUIRE(execution.replay_first_completed_content_digest
        == execution.replay_last_completed_content_digest);
    REQUIRE(execution.replay_final_span_bp == half_span);
    REQUIRE(execution.replay_final_storage_binding.generation_token
        == execution.spill_storage_binding.generation_token);
    REQUIRE(execution.replay_final_storage_binding.mutation_version
        == execution.spill_storage_binding.mutation_version + 1u);
    REQUIRE(encoded->manifest.position_segment_span_bp == half_span);
    REQUIRE(encoded->manifest.position_segment_count == 3);
    REQUIRE(encoded->manifest.position_count
        == mpf_plan->canonical_event_count);
    REQUIRE(spill_storage.segment_reroute_calls() == 1);
    REQUIRE(spill_storage.segment_merge_calls() == 4);
    require_complete_generation_rows(*encoded, mps_sink.rows);
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
    auto spilled = MpsIndex::spill_generation_replay_runs_serial(
        spill_storage,
        lazy,
        MinimizerSketchBackend::ExactSimd,
        fragment_bp);
    REQUIRE(spilled.has_value());
    REQUIRE(spilled->exact_simd_proof_present());
    const ExactSimdIndexExecutionProof mps_exact_proof =
        spilled->exact_simd_proof();
    auto mps_producer = MpsIndex::make_generation_replay_producer(
        spill_storage, range.bp_size(), std::move(*spilled));
    CollectingMinimizerRowSink mps_sink;
    auto mps_options = options;
    ++mps_options.generation_id;
    auto mps = encode_minimizer_generation(
        mps_producer, mps_sink, mps_options);
    REQUIRE(mps.has_value());
    REQUIRE(mpf_producer.exact_simd_proof_present());
    REQUIRE(mpf_producer.exact_simd_proof().accepted(true));
    REQUIRE(mpf_producer.exact_simd_proof().ambiguous_symbols > 0);
    REQUIRE(mps_exact_proof.accepted(true));
    REQUIRE(mps_exact_proof.ambiguous_symbols > 0);
    REQUIRE(mps_producer.merge_pass_count() == 2);
    REQUIRE(spill_storage.segment_merge_calls() == 3);
    REQUIRE(spill_storage.segment_merge_run_visits()
        == 3u * spill_storage.segment_spill_run_count());
    REQUIRE(spill_storage.segment_merge_bytes_visited()
        == 3u * spill_storage.segment_spill_bytes());
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

TEST_CASE("DOC0371 unbound MPS replay cannot claim a producer backend",
          "[indexing][minimizer-hashmap][wire][encoder][identity][mps][fail-closed]") {
    using namespace bioseq::indexing;
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

    MpsUnboundExternalSpillReplayProducer<FakeFragmentStorage<>> producer{
        storage,
        preferred,
        preferred,
        kLegacyMinimapV1SchemeIdentity};
    CollectingMinimizerRowSink sink;
    const MinimizerGenerationFixedWidthLimits two_per_core{
        .max_segment_control_count = 2,
        .max_segment_position_count = 2,
        .max_hash_position_count = 1,
    };
    const auto encoded = encode_minimizer_generation(
        producer,
        sink,
        test_grouped32_generation_options(preferred),
        two_per_core);
    require_wire_error(
        encoded, MinimizerGenerationWireError::compatibility_mismatch);
    const MpsGenerationExecutionEvidence execution =
        producer.execution_evidence();
    REQUIRE_FALSE(execution.accepted());
    REQUIRE(execution.replay_scan_count == 0);
    REQUIRE(execution.replay_merge_pass_count == 0);
    REQUIRE(execution.replay_reroute_pass_count == 0);
    REQUIRE(execution.replay_completed_scan_count == 0);
    REQUIRE(execution.replay_failed_scan_count == 0);
    REQUIRE(execution.replay_illegal_span_transition_count == 0);
    REQUIRE(storage.segment_reroute_calls() == 0);
    REQUIRE(storage.segment_merge_calls() == 0);
    REQUIRE(storage.segment_spill_bytes() == spill_bytes);
    REQUIRE(sink.rows.empty());
}

TEST_CASE("raw MPS replay rejects a non-halving span transition",
          "[indexing][minimizer-hashmap][wire][encoder][geometry][mps][fail-closed]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t preferred = kMinimizerPreferredSegmentSpanBp;
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> run{{
        {1u, 2u},
    }};
    FakeFragmentStorage<> storage;
    storage.spill_sorted_segment_run(0, run);
    MpsUnboundExternalSpillReplayProducer<FakeFragmentStorage<>> producer{
        storage,
        preferred,
        preferred,
        kLegacyMinimapV1SchemeIdentity};
    int receiver_context = 0;
    const auto rejected = producer.scan_sorted_events(
        preferred / 4u,
        CanonicalMinimizerEventReceiver{
            .context = &receiver_context,
            .emit_fn = +[](void*, const CanonicalMinimizerEvent&)
                -> std::expected<void, MinimizerGenerationWireError> {
                return {};
            },
        });
    require_wire_error(
        rejected, MinimizerGenerationWireError::segment_span);
    REQUIRE(producer.scan_count() == 1);
    REQUIRE(producer.merge_pass_count() == 0);
    REQUIRE(producer.reroute_pass_count() == 0);
    REQUIRE(producer.failed_scan_count() == 1);
    REQUIRE(producer.illegal_span_transition_count() == 1);
    REQUIRE(storage.segment_reroute_calls() == 0);
    REQUIRE(storage.segment_merge_calls() == 0);
}

TEST_CASE("DOC0371 empty unbound MPS replay also fails producer identity",
          "[indexing][minimizer-hashmap][wire][encoder][identity][mps][fail-closed]") {
    using namespace bioseq::indexing;
    FakeFragmentStorage<> storage;
    MpsUnboundExternalSpillReplayProducer<FakeFragmentStorage<>> producer{
        storage,
        0,
        kMinimizerPreferredSegmentSpanBp,
        kLegacyMinimapV1SchemeIdentity};
    CollectingMinimizerRowSink sink;
    const auto encoded = encode_minimizer_generation(
        producer, sink, test_generation_options(0));
    require_wire_error(
        encoded, MinimizerGenerationWireError::compatibility_mismatch);
    REQUIRE(producer.scan_count() == 0);
    REQUIRE(producer.merge_pass_count() == 0);
    REQUIRE(producer.reroute_pass_count() == 0);
    REQUIRE(storage.segment_merge_calls() == 0);
    REQUIRE(storage.segment_reroute_calls() == 0);
    REQUIRE(sink.rows.empty());
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
    for (const MinimizerGenerationWireFormat format : {
             MinimizerGenerationWireFormat::sorted_dense_global64,
             MinimizerGenerationWireFormat::
                 grouped_hash32_segment_local32}) {
        DriftingReplayMinimizerProducer producer;
        CollectingMinimizerRowSink sink;
        auto options = test_generation_options(100u);
        options.wire_format = format;
        const auto encoded = encode_minimizer_generation(
            producer, sink, options);
        require_wire_error(
            encoded, MinimizerGenerationWireError::replay_mismatch);
        REQUIRE(producer.scan_count == 2u);
        REQUIRE(sink.rows.empty());
    }
}

TEST_CASE("DOC0373 grouped32 cold planner selects empty selective and full exactly",
          "[indexing][minimizer-hashmap][wire][v4][cold-route][planner]") {
    using namespace bioseq::indexing;
    const auto eleven_entries = make_grouped32_cold_route_entries(11u);
    const auto eleven = make_grouped32_cold_route_fixture(eleven_entries);
    const auto plan = [&](std::span<const std::uint64_t> queries) {
        return plan_minimizer_grouped32_cold_route(
            eleven.metadata.head,
            eleven.metadata.manifest,
            eleven.directory,
            queries);
    };

    SECTION("empty union and empty intersection perform no candidate work") {
        const auto empty = plan({});
        REQUIRE(empty.has_value());
        REQUIRE(empty->route == MinimizerGrouped32ColdLoadRoute::empty);
        REQUIRE(empty->candidate_directory_entries == 0u);
        REQUIRE(empty->candidate_rows_upper_bound == 0u);
        REQUIRE(empty->candidate_bytes_upper_bound == 0u);
        REQUIRE(empty->control_groups == 0u);
        REQUIRE(empty->position_groups_upper_bound == 0u);
        REQUIRE(empty->selective_cost == 0u);
        REQUIRE(empty->full_directory_entries == 11u);
        REQUIRE(empty->full_payload_rows == 22u);
        REQUIRE(empty->full_payload_bytes == 11u * 16u);
        REQUIRE(empty->full_cost == 30u);
        REQUIRE(empty->candidate_directory_ordinals.empty());
        REQUIRE(empty->candidate_directory_ordinals.capacity() == 0u);
        REQUIRE(empty->control_requests.empty());
        REQUIRE(empty->control_requests.capacity() == 0u);
        REQUIRE(empty->position_upper_bound_requests.empty());
        REQUIRE(empty->position_upper_bound_requests.capacity() == 0u);

        const std::array<std::uint64_t, 1> absent{9999u};
        const auto no_intersection = plan(absent);
        REQUIRE(no_intersection.has_value());
        REQUIRE(no_intersection->route
            == MinimizerGrouped32ColdLoadRoute::empty);
        REQUIRE(no_intersection->candidate_rows_upper_bound == 0u);
        REQUIRE(no_intersection->candidate_directory_ordinals.capacity()
            == 0u);
        REQUIRE(no_intersection->control_requests.capacity() == 0u);
        REQUIRE(no_intersection->position_upper_bound_requests.capacity()
            == 0u);
    }

    SECTION("two adjacent whole extents are cheaper than one full scan") {
        const std::array<std::uint64_t, 2> adjacent{100u, 110u};
        const auto selective = plan(adjacent);
        REQUIRE(selective.has_value());
        REQUIRE(selective->route
            == MinimizerGrouped32ColdLoadRoute::selective);
        REQUIRE(selective->candidate_directory_entries == 2u);
        REQUIRE(selective->candidate_control_rows == 2u);
        REQUIRE(selective->candidate_position_rows_upper_bound == 2u);
        REQUIRE(selective->candidate_rows_upper_bound == 4u);
        REQUIRE(selective->candidate_control_bytes == 24u);
        REQUIRE(selective->candidate_position_bytes_upper_bound == 8u);
        REQUIRE(selective->candidate_bytes_upper_bound == 32u);
        REQUIRE(selective->control_groups == 1u);
        REQUIRE(selective->position_groups_upper_bound == 2u);
        REQUIRE(selective->selective_cost == 28u);
        REQUIRE(selective->full_cost == 30u);
        const std::array<std::uint32_t, 2> ordinals{0u, 1u};
        std::array<IndexSubKeyRequest, 2> controls{};
        std::array<IndexSubKeyRequest, 2> positions{};
        for (std::size_t index = 0; index < ordinals.size(); ++index) {
            const MinimizerBucketDirEntry& entry = eleven_entries[index];
            controls[index] = IndexSubKeyRequest{
                .generation_id = eleven.metadata.head.generation_id,
                .position_segment = entry.position_segment,
                .sub_key = entry.sub_key,
                .first_chunk_no = entry.control_first_chunk,
                .expected_chunk_count = entry.control_chunk_count,
                .expected_total_bytes = entry.control_encoded_bytes,
                .expected_checksum = entry.control_checksum,
            };
            positions[index] = IndexSubKeyRequest{
                .generation_id = eleven.metadata.head.generation_id,
                .position_segment = entry.position_segment,
                .sub_key = *derive_minimizer_position_extent_sub_key(
                    entry.sub_key),
                .first_chunk_no = entry.position_first_chunk,
                .expected_chunk_count = entry.position_chunk_count,
                .expected_total_bytes = entry.position_encoded_bytes,
                .expected_checksum = entry.position_checksum,
            };
        }
        REQUIRE(selective->candidate_authority_equals(
            ordinals, controls, positions));
        ++controls.front().expected_checksum;
        REQUIRE_FALSE(selective->candidate_authority_equals(
            ordinals, controls, positions));

        bioseq::parallelism::MemoryAdmissionController admission;
        {
            const auto reader = PreparedGrouped32QueryReader::begin(
                admission,
                eleven.metadata.head,
                eleven.metadata.manifest,
                test_grouped32_expected_compatibility(),
                MinimizerSketchBackend::ExactSimd,
                eleven.directory.encoded_bytes(),
                adjacent,
                10u);
            REQUIRE(reader.has_value());
            REQUIRE(std::ranges::equal(
                reader->control_ordinals(),
                selective->candidate_directory_ordinals));
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("nonadjacent candidates pay separate scans and choose full") {
        const std::array<std::uint64_t, 2> nonadjacent{100u, 120u};
        const auto full = plan(nonadjacent);
        REQUIRE(full.has_value());
        REQUIRE(full->route == MinimizerGrouped32ColdLoadRoute::full);
        REQUIRE(full->candidate_rows_upper_bound == 4u);
        REQUIRE(full->control_groups == 2u);
        REQUIRE(full->position_groups_upper_bound == 2u);
        REQUIRE(full->selective_cost == 36u);
        REQUIRE(full->full_cost == 30u);
        REQUIRE(full->candidate_directory_ordinals.capacity() == 0u);
        REQUIRE(full->control_requests.capacity() == 0u);
        REQUIRE(full->position_upper_bound_requests.capacity() == 0u);
    }

    SECTION("cost equality deterministically chooses full") {
        const auto ten_entries = make_grouped32_cold_route_entries(10u);
        const auto ten = make_grouped32_cold_route_fixture(ten_entries);
        const std::array<std::uint64_t, 2> adjacent{100u, 110u};
        const auto equal = plan_minimizer_grouped32_cold_route(
            ten.metadata.head,
            ten.metadata.manifest,
            ten.directory,
            adjacent);
        REQUIRE(equal.has_value());
        REQUIRE(equal->selective_cost == 28u);
        REQUIRE(equal->full_cost == 28u);
        REQUIRE(equal->route == MinimizerGrouped32ColdLoadRoute::full);
        REQUIRE(equal->candidate_directory_ordinals.capacity() == 0u);
        REQUIRE(equal->control_requests.capacity() == 0u);
        REQUIRE(equal->position_upper_bound_requests.capacity() == 0u);
    }

    SECTION("a dense full route retains counters without candidate allocation") {
        std::array<std::uint64_t, 11> dense{};
        for (std::size_t index = 0; index < dense.size(); ++index) {
            dense[index] = 100u + static_cast<std::uint64_t>(index) * 10u;
        }
        const auto full = plan(dense);
        REQUIRE(full.has_value());
        REQUIRE(full->route == MinimizerGrouped32ColdLoadRoute::full);
        REQUIRE(full->candidate_directory_entries == dense.size());
        REQUIRE(full->candidate_rows_upper_bound == 22u);
        REQUIRE(full->candidate_bytes_upper_bound == 11u * 16u);
        REQUIRE(full->candidate_directory_ordinals.capacity() == 0u);
        REQUIRE(full->control_requests.capacity() == 0u);
        REQUIRE(full->position_upper_bound_requests.capacity() == 0u);
    }
}

TEST_CASE("DOC0373 grouped32 cold planner coalesces only exact physical neighbors",
          "[indexing][minimizer-hashmap][wire][v4][cold-route][coalescing]") {
    using namespace bioseq::indexing;
    const std::array<std::uint64_t, 2> selected{100u, 110u};

    SECTION("a physical sub-key gap separates both phase groups") {
        auto entries = make_grouped32_cold_route_entries(11u);
        for (std::size_t index = 1; index < entries.size(); ++index) {
            ++entries[index].sub_key;
        }
        const auto fixture = make_grouped32_cold_route_fixture(entries);
        const auto planned = plan_minimizer_grouped32_cold_route(
            fixture.metadata.head,
            fixture.metadata.manifest,
            fixture.directory,
            selected);
        REQUIRE(planned.has_value());
        REQUIRE(planned->control_groups == 2u);
        REQUIRE(planned->position_groups_upper_bound == 2u);
        REQUIRE(planned->route == MinimizerGrouped32ColdLoadRoute::full);
    }

    SECTION("a position-segment boundary separates otherwise adjacent keys") {
        auto entries = make_grouped32_cold_route_entries(11u);
        for (std::size_t index = 1; index < entries.size(); ++index) {
            entries[index].position_segment = 1;
            entries[index].sub_key = static_cast<index_sub_key_t>(index - 1u);
        }
        const auto fixture = make_grouped32_cold_route_fixture(entries);
        const auto planned = plan_minimizer_grouped32_cold_route(
            fixture.metadata.head,
            fixture.metadata.manifest,
            fixture.directory,
            selected);
        REQUIRE(planned.has_value());
        REQUIRE(planned->control_groups == 2u);
        REQUIRE(planned->position_groups_upper_bound == 2u);
        REQUIRE(planned->route == MinimizerGrouped32ColdLoadRoute::full);
    }

    SECTION("wire-valid sub-key regressions start a new physical group") {
        auto entries = make_grouped32_cold_route_entries(20u);
        entries[0].sub_key = 1;
        entries[1].sub_key = 0;
        const auto fixture = make_grouped32_cold_route_fixture(entries);
        const auto planned = plan_minimizer_grouped32_cold_route(
            fixture.metadata.head,
            fixture.metadata.manifest,
            fixture.directory,
            selected);
        REQUIRE(planned.has_value());
        REQUIRE(planned->control_groups == 2u);
        REQUIRE(planned->position_groups_upper_bound == 2u);
        REQUIRE(planned->route
            == MinimizerGrouped32ColdLoadRoute::selective);
        REQUIRE(planned->control_requests[0].sub_key == 1);
        REQUIRE(planned->control_requests[1].sub_key == 0);
    }

    SECTION("alternating max-occ retention cannot exceed the position bound") {
        const auto entries = make_grouped32_cold_route_entries(20u);
        const auto fixture = make_grouped32_cold_route_fixture(entries);
        const std::array<std::uint64_t, 3> three_candidates{
            100u, 110u, 120u,
        };
        const auto planned = plan_minimizer_grouped32_cold_route(
            fixture.metadata.head,
            fixture.metadata.manifest,
            fixture.directory,
            three_candidates);
        REQUIRE(planned.has_value());
        REQUIRE(planned->control_groups == 1u);
        REQUIRE(planned->position_groups_upper_bound == 3u);
        REQUIRE(planned->route
            == MinimizerGrouped32ColdLoadRoute::selective);
        REQUIRE(planned->position_upper_bound_requests.size() == 3u);
        // If the middle extent is pruned, the two retained requests are no
        // longer adjacent and need two scans. The pre-I/O bound of three is
        // still conservative and can never be overwritten by that outcome.
        REQUIRE(planned->position_upper_bound_requests[0].sub_key + 2
            == planned->position_upper_bound_requests[2].sub_key);
        constexpr std::uint64_t alternating_retained_groups = 2u;
        REQUIRE(alternating_retained_groups
            <= planned->position_groups_upper_bound);
    }
}

TEST_CASE("DOC0373 grouped32 cold planner validates totals input order and overflow",
          "[indexing][minimizer-hashmap][wire][v4][cold-route][fail-closed]") {
    using namespace bioseq::indexing;
    const auto entries = make_grouped32_cold_route_entries(7u);
    const auto fixture = make_grouped32_cold_route_fixture(entries);

    SECTION("manifest directory totals are revalidated by the planner") {
        auto altered_generation = make_grouped32_reader_fixture(
            std::array<std::pair<std::uint64_t, std::uint64_t>, 1>{{
                {1u, 0u},
            }},
            100u).generation;
        altered_generation.manifest = fixture.metadata.manifest;
        ++altered_generation.manifest.payload_bytes;
        const auto rebuilt = rebuild_generation_metadata(
            altered_generation, fixture.directory.entries());
        require_wire_error(
            plan_minimizer_grouped32_cold_route(
                rebuilt.head,
                rebuilt.manifest,
                fixture.directory,
                std::array<std::uint64_t, 1>{100u}),
            MinimizerGenerationWireError::element_count);
    }

    SECTION("the query union must be canonical and Format-4 representable") {
        const std::array<std::uint64_t, 2> descending{110u, 100u};
        require_wire_error(
            plan_minimizer_grouped32_cold_route(
                fixture.metadata.head,
                fixture.metadata.manifest,
                fixture.directory,
                descending),
            MinimizerGenerationWireError::control_order);
        const std::array<std::uint64_t, 2> duplicate{100u, 100u};
        require_wire_error(
            plan_minimizer_grouped32_cold_route(
                fixture.metadata.head,
                fixture.metadata.manifest,
                fixture.directory,
                duplicate),
            MinimizerGenerationWireError::control_order);
        const std::array<std::uint64_t, 1> too_wide{
            std::uint64_t{1} << 32u,
        };
        require_wire_error(
            plan_minimizer_grouped32_cold_route(
                fixture.metadata.head,
                fixture.metadata.manifest,
                fixture.directory,
                too_wide),
            MinimizerGenerationWireError::hash_range);
    }

    SECTION("route arithmetic saturates and still applies the frozen formula") {
        constexpr std::uint64_t maximum =
            (std::numeric_limits<std::uint64_t>::max)();
        MinimizerGrouped32ColdRouteWork empty_full_overflow{
            .query_union_empty = true,
            .full_directory_entries = 1u,
            .full_payload_rows = maximum,
            .full_payload_bytes = 1u,
        };
        const auto saturated_empty_full =
            finalize_minimizer_grouped32_cold_route(empty_full_overflow);
        REQUIRE(saturated_empty_full.has_value());
        REQUIRE(saturated_empty_full->route
            == MinimizerGrouped32ColdLoadRoute::empty);
        REQUIRE(saturated_empty_full->selective_cost == 0u);
        REQUIRE(saturated_empty_full->full_cost == maximum);
        REQUIRE(saturated_empty_full->full_cost_saturated);
        REQUIRE(saturated_empty_full->candidate_directory_ordinals.empty());
        REQUIRE(saturated_empty_full->candidate_directory_ordinals.capacity()
            == 0u);
        REQUIRE(saturated_empty_full->control_requests.empty());
        REQUIRE(saturated_empty_full->control_requests.capacity() == 0u);
        REQUIRE(saturated_empty_full->position_upper_bound_requests.empty());
        REQUIRE(saturated_empty_full->position_upper_bound_requests.capacity()
            == 0u);

        MinimizerGrouped32ColdRouteWork row_overflow{
            .candidate_directory_entries = 1u,
            .candidate_control_rows = maximum,
            .candidate_position_rows_upper_bound = 1u,
            .candidate_control_bytes = 1u,
            .candidate_position_bytes_upper_bound = 1u,
            .control_groups = 1u,
            .position_groups_upper_bound = 1u,
            .full_directory_entries = 1u,
            .full_payload_rows = maximum,
            .full_payload_bytes = 2u,
        };
        const auto saturated_rows =
            finalize_minimizer_grouped32_cold_route(row_overflow);
        REQUIRE(saturated_rows.has_value());
        REQUIRE(saturated_rows->candidate_rows_upper_bound == maximum);
        REQUIRE(saturated_rows->selective_cost == maximum);
        REQUIRE(saturated_rows->full_cost == maximum);
        REQUIRE(saturated_rows->counters_saturated);
        REQUIRE(saturated_rows->selective_cost_saturated);
        REQUIRE(saturated_rows->full_cost_saturated);
        REQUIRE(saturated_rows->route
            == MinimizerGrouped32ColdLoadRoute::full);

        MinimizerGrouped32ColdRouteWork penalty_overflow{
            .candidate_directory_entries = maximum,
            .candidate_control_rows = 1u,
            .candidate_position_rows_upper_bound = 1u,
            .candidate_control_bytes = 1u,
            .candidate_position_bytes_upper_bound = 1u,
            .control_groups = maximum / 8u + 1u,
            .position_groups_upper_bound = 1u,
            .full_directory_entries = maximum,
            .full_payload_rows = 2u,
            .full_payload_bytes = 2u,
        };
        const auto saturated_penalty =
            finalize_minimizer_grouped32_cold_route(penalty_overflow);
        REQUIRE(saturated_penalty.has_value());
        REQUIRE(saturated_penalty->selective_cost == maximum);
        REQUIRE(saturated_penalty->selective_cost_saturated);
        REQUIRE_FALSE(saturated_penalty->full_cost_saturated);
        REQUIRE(saturated_penalty->route
            == MinimizerGrouped32ColdLoadRoute::full);

        MinimizerGrouped32ColdRouteWork full_overflow{
            .candidate_directory_entries = 1u,
            .candidate_control_rows = 1u,
            .candidate_position_rows_upper_bound = 1u,
            .candidate_control_bytes = 1u,
            .candidate_position_bytes_upper_bound = 1u,
            .control_groups = 1u,
            .position_groups_upper_bound = 1u,
            .full_directory_entries = 1u,
            .full_payload_rows = maximum,
            .full_payload_bytes = 2u,
        };
        const auto saturated_full =
            finalize_minimizer_grouped32_cold_route(full_overflow);
        REQUIRE(saturated_full.has_value());
        REQUIRE(saturated_full->selective_cost == 18u);
        REQUIRE(saturated_full->full_cost == maximum);
        REQUIRE_FALSE(saturated_full->selective_cost_saturated);
        REQUIRE(saturated_full->full_cost_saturated);
        REQUIRE(saturated_full->route
            == MinimizerGrouped32ColdLoadRoute::selective);

        MinimizerGrouped32ColdRouteWork byte_only_overflow{
            .candidate_directory_entries = 1u,
            .candidate_control_rows = 1u,
            .candidate_position_rows_upper_bound = 1u,
            .candidate_control_bytes = maximum,
            .candidate_position_bytes_upper_bound = 1u,
            .control_groups = 1u,
            .position_groups_upper_bound = 1u,
            .full_directory_entries = 2u,
            .full_payload_rows = 20u,
            .full_payload_bytes = maximum,
        };
        const auto saturated_bytes =
            finalize_minimizer_grouped32_cold_route(byte_only_overflow);
        REQUIRE(saturated_bytes.has_value());
        REQUIRE(saturated_bytes->candidate_bytes_upper_bound == maximum);
        REQUIRE(saturated_bytes->counters_saturated);
        REQUIRE_FALSE(saturated_bytes->selective_cost_saturated);
        REQUIRE_FALSE(saturated_bytes->full_cost_saturated);
        REQUIRE(saturated_bytes->route
            == MinimizerGrouped32ColdLoadRoute::selective);
    }
}

TEST_CASE("DOC0373 Format-5 rejects scheme and backend drift at every scan boundary",
          "[indexing][minimizer-hashmap][wire][v5][identity][corruption]") {
    using namespace bioseq::indexing;
    for (const IdentityDriftAxis axis : {
             IdentityDriftAxis::scheme,
             IdentityDriftAxis::backend}) {
        for (const std::size_t boundary : {1u, 2u, 3u}) {
            INFO("axis=" << static_cast<unsigned>(axis)
                << " completed scan=" << boundary);
            BoundaryIdentityDriftingProducer producer{
                .drift_after_scan = boundary,
                .axis = axis,
            };
            CollectingMinimizerRowSink sink;
            const auto encoded = encode_minimizer_generation(
                producer,
                sink,
                test_shape_v5_generation_options(100u, 10u));
            require_wire_error(
                encoded, MinimizerGenerationWireError::replay_mismatch);
            REQUIRE(producer.scan_count == boundary);
            REQUIRE(std::ranges::none_of(sink.rows, [](const auto& row) {
                return row.address.sub_key == DIRECTORY_SUB_KEY;
            }));
        }
    }
}

TEST_CASE("DOC0371 heavy hash is never split across partitions or page extents",
          "[indexing][minimizer-hashmap][wire][encoder][heavy-hitter]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t target_length = 100000;
    constexpr std::uint64_t heavy_position_count = 70000;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers;
    minimizers.reserve(heavy_position_count + 1u);
    for (std::uint64_t index = 0; index < heavy_position_count; ++index) {
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
    REQUIRE(heavy.position_element_count == heavy_position_count);
    REQUIRE(heavy.position_encoded_bytes > kMinimizerPartitionTargetBytes);
    REQUIRE(heavy.position_chunk_count > 1);
    const auto heavy_positions = collect_encoded_extent(
        sink.rows, heavy, false);
    REQUIRE(heavy_positions.size() == heavy_position_count * 8u);
    REQUIRE(generation->directory.entries()[1].lo_hash == 43);
    REQUIRE(sink.max_callback_bytes == kMinimizerExtentPayloadBytes);
    require_complete_generation_rows(*generation, sink.rows);

    VectorReplayMinimizerProducer compact_producer{minimizers};
    CollectingMinimizerRowSink compact_sink;
    auto compact_options = test_grouped32_generation_options(target_length);
    ++compact_options.generation_id;
    const auto compact = encode_minimizer_generation(
        compact_producer, compact_sink, compact_options);
    REQUIRE(compact.has_value());
    REQUIRE(compact->directory.entries().size() == 2u);
    const auto& compact_heavy = compact->directory.entries()[0];
    REQUIRE(compact_heavy.lo_hash == 42u);
    REQUIRE(compact_heavy.hi_hash == 42u);
    REQUIRE(compact_heavy.control_element_count == 1u);
    REQUIRE(compact_heavy.position_element_count == heavy_position_count);
    REQUIRE(compact_heavy.position_encoded_bytes
        == heavy_position_count * kMinimizerGrouped32PositionEncodedBytes);
    REQUIRE(compact_heavy.position_encoded_bytes
        > kMinimizerPartitionTargetBytes);
    REQUIRE(compact_heavy.position_chunk_count > 1u);
    REQUIRE(collect_encoded_extent(
        compact_sink.rows, compact_heavy, false).size()
        == heavy_position_count * kMinimizerGrouped32PositionEncodedBytes);
    REQUIRE(compact_sink.max_callback_bytes == kMinimizerExtentPayloadBytes);
    REQUIRE(decode_generation_events(*compact, compact_sink.rows)
        == decode_generation_events(*generation, sink.rows));
    require_complete_generation_rows(*compact, compact_sink.rows);
}

TEST_CASE("grouped32 reader admits the signed target boundary before ownership",
          "[indexing][minimizer-hashmap][wire][v4][reader][boundary][admission]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t signed_max = static_cast<std::uint64_t>(
        (std::numeric_limits<std::int64_t>::max)());
    const std::array minimizers{
        std::pair<std::uint64_t, std::uint64_t>{42u, 0u}};
    const auto query = std::array<std::uint64_t, 1>{42u};
    auto fixture = make_grouped32_reader_fixture(minimizers, 100u);

    auto at_signed_boundary = fixture.generation.manifest;
    at_signed_boundary.target_length = signed_max;
    bioseq::parallelism::MemoryAdmissionController boundary_admission;
    boundary_admission.set_index_memory_cap_bytes(1u);
    require_wire_error(
        PreparedGrouped32QueryReader::begin(
            boundary_admission,
            fixture.generation.head,
            at_signed_boundary,
            test_grouped32_expected_compatibility(),
            MinimizerSketchBackend::ExactSimd,
            fixture.generation.directory.encoded_bytes(),
            query,
            10u),
        MinimizerGenerationWireError::segment_count);
    REQUIRE(boundary_admission.reserved_bytes() == 0u);

    auto above_signed_boundary = fixture.generation.manifest;
    above_signed_boundary.target_length = signed_max + 1u;
    bioseq::parallelism::MemoryAdmissionController rejected_admission;
    rejected_admission.set_index_memory_cap_bytes(1u);
    require_wire_error(
        PreparedGrouped32QueryReader::begin(
            rejected_admission,
            fixture.generation.head,
            above_signed_boundary,
            test_grouped32_expected_compatibility(),
            MinimizerSketchBackend::ExactSimd,
            fixture.generation.directory.encoded_bytes(),
            query,
            10u),
        MinimizerGenerationWireError::bp_range);
    REQUIRE(rejected_admission.reserved_bytes() == 0u);

    // For target_length == INT64_MAX the greatest valid coordinate remains
    // exactly representable in the ordinary minimizer ABI after adding strand.
    constexpr std::uint64_t final_bp = signed_max - 1u;
    constexpr auto global = globalize_minimizer_grouped32_local_position(
        /*encoded local bp=1, reverse=*/3u,
        final_bp - 1u,
        signed_max);
    STATIC_REQUIRE(global.has_value());
    STATIC_REQUIRE(global->bp == final_bp);
    STATIC_REQUIRE(global->reverse_strand);
    constexpr std::uint64_t encoded =
        (global->bp << 1u)
        | static_cast<std::uint64_t>(global->reverse_strand);
    STATIC_REQUIRE(encoded == (final_bp << 1u) + 1u);
}

TEST_CASE("inactive grouped32 reader owns identity and applies max-occ globally",
          "[indexing][minimizer-hashmap][wire][v4][reader][identity][max-occ]") {
    using namespace bioseq::indexing;
    STATIC_REQUIRE(MinimizerLookupIndex<PreparedGrouped32QueryReader>);
    STATIC_REQUIRE(SegmentedMinimizerLookupIndex<
        PreparedGrouped32QueryReader>);
    constexpr std::uint64_t span = kMinimizerPreferredSegmentSpanBp;
    constexpr std::uint64_t high_bp = (std::uint64_t{1} << 32u) + 37u;
    constexpr auto at = [](std::uint64_t bp, bool reverse = false) {
        return (bp << 1u) | static_cast<std::uint64_t>(reverse);
    };
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {100u, at(3u)},
        {100u, at(9u, true)},
        {7u, at(span + 3u, true)},
        {100u, at(span + 5u)},
        {100u, at(span + 6u, true)},
        {42u, at(high_bp, true)},
    };
    auto fixture = make_grouped32_reader_fixture(
        minimizers, high_bp + 100u);
    auto head = fixture.generation.head;
    auto manifest = fixture.generation.manifest;
    std::vector<std::byte> directory_bytes(
        fixture.generation.directory.encoded_bytes().begin(),
        fixture.generation.directory.encoded_bytes().end());
    std::vector<std::uint64_t> queries{100u, 42u, 11u, 7u, 100u};

    bioseq::parallelism::MemoryAdmissionController admission;
    auto made = PreparedGrouped32QueryReader::begin(
        admission,
        head,
        manifest,
        test_grouped32_expected_compatibility(),
        MinimizerSketchBackend::ExactSimd,
        directory_bytes,
        queries,
        3u);
    REQUIRE(made.has_value());
    auto reader = std::move(*made);
    REQUIRE(reader.qualified_sketch_backend()
        == MinimizerSketchBackend::ExactSimd);
    const std::vector<std::uint32_t> control_ordinals{
        reader.control_ordinals().begin(), reader.control_ordinals().end()};
    REQUIRE(control_ordinals.size() == 3u);

    // Every caller-owned identity/input object may disappear or mutate now.
    head.generation_id = 0;
    manifest.generation_id = 0;
    std::ranges::fill(directory_bytes, std::byte{0xff});
    std::ranges::fill(queries, (std::numeric_limits<std::uint64_t>::max)());

    feed_grouped32_extents(
        reader, fixture, control_ordinals, false, true);
    REQUIRE(reader.finish_controls().has_value());
    const std::vector<std::uint32_t> position_ordinals{
        reader.retained_position_ordinals().begin(),
        reader.retained_position_ordinals().end()};
    REQUIRE(position_ordinals.size() == 2u);
    for (const std::uint32_t ordinal : position_ordinals) {
        const auto* entry = fixture.generation.directory.entry_by_ordinal(ordinal);
        REQUIRE(entry != nullptr);
        REQUIRE(entry->position_segment != 0);
    }
    feed_grouped32_extents(
        reader, fixture, position_ordinals, true, true);
    REQUIRE(reader.finish_load().has_value());
    REQUIRE(reader.ready());
    REQUIRE(reader.target_length() == high_bp + 100u);

    const auto pruned = reader.lookup(100u);
    REQUIRE(pruned.has_value());
    REQUIRE(pruned->global_occurrence_count == 4u);
    REQUIRE_FALSE(pruned->positions_complete);
    REQUIRE(pruned->positions.empty());

    const auto retained = reader.lookup(7u);
    REQUIRE(retained.has_value());
    REQUIRE(retained->global_occurrence_count == 1u);
    REQUIRE(retained->positions_complete);
    REQUIRE(retained->positions.size() == 1u);
    REQUIRE(retained->positions.front() == at(span + 3u, true));

    const auto above_4g = reader.lookup(42u);
    REQUIRE(above_4g.has_value());
    REQUIRE(above_4g->global_occurrence_count == 1u);
    REQUIRE(above_4g->positions.size() == 1u);
    REQUIRE(above_4g->positions.front() == at(high_bp, true));

    const auto absent_gap = reader.lookup(11u);
    REQUIRE(absent_gap.has_value());
    REQUIRE(absent_gap->global_occurrence_count == 0u);
    REQUIRE(absent_gap->positions_complete);
    REQUIRE(absent_gap->positions.empty());

    const MinimizerLookupSpans pruned_spans = reader.lookup_spans(100u);
    REQUIRE(pruned_spans.global_occurrence_count() == 4u);
    REQUIRE_FALSE(pruned_spans.positions_complete());
    REQUIRE(pruned_spans.contiguous_positions().empty());
    REQUIRE(pruned_spans.segmented_positions().empty());
    REQUIRE(pruned_spans.well_formed());

    const MinimizerLookupSpans retained_spans = reader.lookup_spans(7u);
    REQUIRE(retained_spans.global_occurrence_count() == 1u);
    REQUIRE(retained_spans.positions_complete());
    REQUIRE(retained_spans.contiguous_positions().size() == 1u);
    REQUIRE(retained_spans.contiguous_positions().front()
        == at(span + 3u, true));
    REQUIRE(retained_spans.well_formed());

    const MinimizerLookupSpans above_4g_spans = reader.lookup_spans(42u);
    REQUIRE(above_4g_spans.contiguous_positions().front()
        == at(high_bp, true));
    REQUIRE(above_4g_spans.well_formed());

    const MinimizerLookupSpans absent_spans = reader.lookup_spans(11u);
    REQUIRE(absent_spans.global_occurrence_count() == 0u);
    REQUIRE(absent_spans.positions_complete());
    REQUIRE(absent_spans.contiguous_positions().empty());
    REQUIRE(absent_spans.well_formed());

    REQUIRE_THROWS_AS(
        reader.lookup_spans(99u), PersistedMinimizerLookupError);
    require_wire_error(
        reader.lookup(99u), MinimizerGenerationWireError::hash_range);

    // A controls-selected partition containing only a globally pruned query is
    // not position-authorized, even though its physical rows exist.
    const auto fresh_queries = std::array<std::uint64_t, 4>{
        100u, 42u, 11u, 7u};
    bioseq::parallelism::MemoryAdmissionController unauthorized_admission;
    auto unauthorized_made = PreparedGrouped32QueryReader::begin(
        unauthorized_admission,
        fixture.generation.head,
        fixture.generation.manifest,
        test_grouped32_expected_compatibility(),
        MinimizerSketchBackend::ExactSimd,
        fixture.generation.directory.encoded_bytes(),
        fresh_queries,
        3u);
    REQUIRE(unauthorized_made.has_value());
    auto unauthorized_reader = std::move(*unauthorized_made);
    const std::vector<std::uint32_t> fresh_controls{
        unauthorized_reader.control_ordinals().begin(),
        unauthorized_reader.control_ordinals().end()};
    feed_grouped32_extents(
        unauthorized_reader, fixture, fresh_controls, false);
    REQUIRE(unauthorized_reader.finish_controls().has_value());
    const auto authorized = unauthorized_reader.retained_position_ordinals();
    const auto unauthorized = std::ranges::find_if(
        fresh_controls,
        [&](std::uint32_t ordinal) {
            return std::ranges::find(authorized, ordinal) == authorized.end();
        });
    REQUIRE(unauthorized != fresh_controls.end());
    const auto& unauthorized_row = grouped32_extent_row(
        fixture, *unauthorized, true, 0u);
    require_wire_error(
        unauthorized_reader.accept_position_chunk(
            unauthorized_row.address, unauthorized_row.bytes),
        MinimizerGenerationWireError::chunk_range);
    REQUIRE(unauthorized_reader.failed());
}

TEST_CASE("grouped32 physical stream prunes max-occ across segments without generation staging",
		  "[indexing][minimizer-hashmap][wire][v4][reader][physical-stream][max-occ]") {
	using namespace bioseq::indexing;
	constexpr std::uint64_t span = kMinimizerPreferredSegmentSpanBp;
	constexpr auto at = [](std::uint64_t bp, bool reverse = false) {
		return (bp << 1u) | static_cast<std::uint64_t>(reverse);
	};
	const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
		{100u, at(3u)},
		{100u, at(9u, true)},
		{7u, at(span + 3u, true)},
		{100u, at(span + 5u)},
		{100u, at(span + 6u, true)},
		{42u, at(span * 2u + 11u)},
	};
	auto fixture = make_grouped32_reader_fixture(
		minimizers, span * 2u + 100u);
	const std::array<std::uint64_t, 4> queries{100u, 42u, 11u, 7u};
	bioseq::parallelism::MemoryAdmissionController admission;
	auto made = PreparedGrouped32QueryReader::begin_full_generation_stream(
		admission,
		fixture.generation.head,
		fixture.generation.manifest,
		test_grouped32_expected_compatibility(),
		MinimizerSketchBackend::ExactSimd,
		fixture.generation.directory.encoded_bytes(),
		queries,
		3u);
	REQUIRE(made.has_value());
	auto reader = std::move(*made);
	feed_grouped32_full_generation_physical(reader, fixture);
	REQUIRE(reader.finish_full_generation_stream().has_value());
	REQUIRE(reader.ready());

	const auto pruned = reader.lookup(100u);
	REQUIRE(pruned.has_value());
	REQUIRE(pruned->global_occurrence_count == 4u);
	REQUIRE_FALSE(pruned->positions_complete);
	REQUIRE(pruned->positions.empty());
	const auto retained = reader.lookup(7u);
	REQUIRE(retained.has_value());
	REQUIRE(retained->positions.size() == 1u);
	REQUIRE(retained->positions.front() == at(span + 3u, true));
	const auto third_segment = reader.lookup(42u);
	REQUIRE(third_segment.has_value());
	REQUIRE(third_segment->positions.size() == 1u);
	REQUIRE(third_segment->positions.front() == at(span * 2u + 11u));
	const auto absent = reader.lookup(11u);
	REQUIRE(absent.has_value());
	REQUIRE(absent->global_occurrence_count == 0u);
	REQUIRE(absent->positions.empty());
	REQUIRE(reader.retained_position_capacity() == 2u);
}

TEST_CASE("DOC0373 grouped32 trusted receipts require authenticated row CRC",
          "[indexing][minimizer-hashmap][wire][v4][reader][trusted-receipt][mode-isolation]") {
    using namespace bioseq::indexing;
    using Receipt = ValidatedMinimizerGenerationRowReceipt;
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<Receipt>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<Receipt>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<Receipt>);
    STATIC_REQUIRE(std::is_move_constructible_v<Receipt>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<Receipt>);
    STATIC_REQUIRE_FALSE(std::is_trivially_copyable_v<Receipt>);
    STATIC_REQUIRE_FALSE((std::is_constructible_v<Receipt,
        IndexChunkAddress, std::span<const std::byte>, std::uint64_t>));

    const std::vector<std::pair<std::uint64_t, std::uint64_t>> events{
        {42u, 3u << 1u},
        {42u, (9u << 1u) | 1u},
        {99u, 12u << 1u},
    };
    const auto fixture = make_grouped32_reader_fixture(events, 100u);
    REQUIRE(fixture.generation.directory.entries().size() == 1u);
    const auto& entry = fixture.generation.directory.entries().front();
    REQUIRE(entry.control_chunk_count == 1u);
    REQUIRE(entry.position_chunk_count == 1u);
    const std::array<std::uint64_t, 1> query{42u};
    const auto begin = [&](auto& admission) {
        return PreparedGrouped32QueryReader::begin_full_generation_stream(
            admission,
            fixture.generation.head,
            fixture.generation.manifest,
            test_grouped32_expected_compatibility(),
            MinimizerSketchBackend::ExactSimd,
            fixture.generation.directory.encoded_bytes(),
            query,
            10u);
    };
    const auto& control = grouped32_extent_row(
        fixture, entry.partition_id, false, entry.control_first_chunk);
    const auto& position = grouped32_extent_row(
        fixture, entry.partition_id, true, entry.position_first_chunk);

    SECTION("validator rejects bad CRC and trusted accept ignores later view changes") {
        bioseq::parallelism::MemoryAdmissionController admission;
        {
            auto made = begin(admission);
            REQUIRE(made.has_value());
            auto reader = std::move(*made);
            auto control_view = row_view(control);
            auto bad_control_view = control_view;
            bad_control_view.row_checksum ^= 1u;
            require_wire_error(
                validate_minimizer_generation_row_receipt(bad_control_view),
                MinimizerGenerationWireError::checksum);
            auto validated_control =
                validate_minimizer_generation_row_receipt(control_view);
            REQUIRE(validated_control.has_value());
            auto control_receipt = std::move(*validated_control);
            REQUIRE(control_receipt.has_valid_authority());
            control_view.row_checksum ^= 1u;
            REQUIRE(reader.accept_full_generation_control_row(
                control_receipt).has_value());
            auto position_receipt = trusted_row_receipt(position);
            REQUIRE(reader.accept_full_generation_position_row(
                position_receipt).has_value());
            REQUIRE(reader.finish_full_generation_stream().has_value());
            REQUIRE(reader.lookup(42u)->positions.size() == 2u);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("wrong trusted address still poisons and releases") {
        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        auto wrong = control;
        ++wrong.address.sub_key;
        recompute_row_checksum(wrong);
        auto receipt = trusted_row_receipt(wrong);
        require_wire_error(
            made->accept_full_generation_control_row(receipt),
            MinimizerGenerationWireError::row_order);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("wrong trusted physical order still poisons and releases") {
        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        auto receipt = trusted_row_receipt(position);
        require_wire_error(
            made->accept_full_generation_position_row(receipt),
            MinimizerGenerationWireError::row_order);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("trusted row cannot bypass extent CRC") {
        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        auto corrupt = control;
        corrupt.bytes.front() ^= std::byte{1};
        recompute_row_checksum(corrupt);
        auto receipt = trusted_row_receipt(corrupt);
        require_wire_error(
            made->accept_full_generation_control_row(receipt),
            MinimizerGenerationWireError::checksum);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("full-stream mode rejects the selective facade without fallback") {
        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        require_wire_error(
            made->accept_control_chunk(control.address, control.bytes),
            MinimizerGenerationWireError::row_order);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("selective mode rejects the full-stream facade without fallback") {
        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = PreparedGrouped32QueryReader::begin(
            admission,
            fixture.generation.head,
            fixture.generation.manifest,
            test_grouped32_expected_compatibility(),
            MinimizerSketchBackend::ExactSimd,
            fixture.generation.directory.encoded_bytes(),
            query,
            10u);
        REQUIRE(made.has_value());
        require_wire_error(
            made->finish_full_generation_stream(),
            MinimizerGenerationWireError::row_order);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }
}

TEST_CASE("admitted grouped32 directory reserves exact allocation headroom before vector allocation",
		  "[indexing][minimizer-hashmap][wire][v4][reader][admission][directory]") {
	using namespace bioseq::indexing;
	const std::vector<std::pair<std::uint64_t, std::uint64_t>> events{
		{42u, 3u << 1u}, {42u, (9u << 1u) | 1u}, {99u, 12u << 1u}};
	auto fixture = make_grouped32_reader_fixture(events, 100u);
	const auto encoded = fixture.generation.directory.encoded_bytes();
	const auto profile = minimizer_generation_payload_profile(
		MinimizerGenerationWireFormat::grouped_hash32_segment_local32,
		kLegacyMinimapV1SchemeIdentity, 15u);
	REQUIRE(profile.has_value());
	const std::uint64_t logical = encoded.size()
		+ fixture.generation.directory.entries().size()
			* sizeof(MinimizerBucketDirEntry);
	const std::uint64_t exact_headroom = logical
		* bioseq::parallelism::kVectorPreallocationHeadroomFactor;

	{
		bioseq::parallelism::MemoryAdmissionController one_short;
		one_short.set_index_memory_cap_bytes(exact_headroom - 1u);
		{
			auto permit = one_short.try_reserve(logical);
			REQUIRE(permit.has_value());
			require_wire_error(
				DirView::decode_admitted(encoded, *profile, *permit),
				MinimizerGenerationWireError::admission);
		}
		REQUIRE(one_short.reserved_bytes() == 0u);
	}
	{
		bioseq::parallelism::MemoryAdmissionController exact;
		exact.set_index_memory_cap_bytes(exact_headroom);
		{
			auto permit = exact.try_reserve(logical);
			REQUIRE(permit.has_value());
			auto decoded = DirView::decode_admitted(encoded, *profile, *permit);
			REQUIRE(decoded.has_value());
			REQUIRE(permit->bytes() == decoded->owned_capacity_bytes());
			REQUIRE(permit->bytes() <= exact_headroom);
		}
		REQUIRE(exact.reserved_bytes() == 0u);
	}
}

TEST_CASE("grouped32 physical stream drops survivor admission immediately on later max-occ invalidation",
		  "[indexing][minimizer-hashmap][wire][v4][reader][physical-stream][max-occ][admission]") {
	using namespace bioseq::indexing;
	constexpr std::uint64_t span = kMinimizerPreferredSegmentSpanBp;
	std::vector<std::pair<std::uint64_t, std::uint64_t>> events;
	for (std::uint64_t bp = 0; bp < 8u; ++bp) {
		events.emplace_back(42u, bp << 1u);
	}
	events.emplace_back(42u, (span + 1u) << 1u);
	auto fixture = make_grouped32_reader_fixture(events, span + 100u);
	const auto entries = fixture.generation.directory.entries();
	REQUIRE(entries.size() == 2u);
	REQUIRE(entries[0].position_segment == 0);
	REQUIRE(entries[1].position_segment == 1);
	const std::array<std::uint64_t, 1> query{42u};
	bioseq::parallelism::MemoryAdmissionController admission;
	auto made = PreparedGrouped32QueryReader::begin_full_generation_stream(
		admission, fixture.generation.head, fixture.generation.manifest,
		test_grouped32_expected_compatibility(),
		MinimizerSketchBackend::ExactSimd,
		fixture.generation.directory.encoded_bytes(), query, 8u);
	REQUIRE(made.has_value());
	auto reader = std::move(*made);
	const auto feed_extent = [&](const MinimizerBucketDirEntry& entry,
			const bool positions) {
		const std::uint32_t first = positions
			? entry.position_first_chunk : entry.control_first_chunk;
		const std::uint32_t count = positions
			? entry.position_chunk_count : entry.control_chunk_count;
		for (std::uint32_t chunk = 0; chunk < count; ++chunk) {
			const auto& row = grouped32_extent_row(
				fixture, entry.partition_id, positions, first + chunk);
			const auto accepted = positions
				? reader.accept_full_generation_position_chunk(
					row.address, row.bytes)
				: reader.accept_full_generation_control_chunk(
					row.address, row.bytes);
			REQUIRE(accepted.has_value());
		}
	};
	feed_extent(entries[0], false);
	feed_extent(entries[0], true);
	const std::uint64_t with_survivors = admission.reserved_bytes();
	feed_extent(entries[1], false);
	REQUIRE(admission.reserved_bytes() < with_survivors);
	feed_extent(entries[1], true);
	REQUIRE(reader.finish_full_generation_stream().has_value());
	const auto found = reader.lookup(42u);
	REQUIRE(found.has_value());
	REQUIRE(found->global_occurrence_count == 9u);
	REQUIRE_FALSE(found->positions_complete);
	REQUIRE(found->positions.empty());
	REQUIRE(reader.retained_position_capacity() == 0u);
}

TEST_CASE("grouped32 physical stream is fail-closed, admitted, movable, and multichunk",
		  "[indexing][minimizer-hashmap][wire][v4][reader][physical-stream][protocol]") {
	using namespace bioseq::indexing;
	const std::vector<std::pair<std::uint64_t, std::uint64_t>> small_events{
		{42u, 3u << 1u}, {42u, (9u << 1u) | 1u}, {99u, 12u << 1u}};
	const auto query = std::array<std::uint64_t, 1>{42u};

	SECTION("position before the monotonic control cursor poisons the reader") {
		auto fixture = make_grouped32_reader_fixture(small_events, 100u);
		bioseq::parallelism::MemoryAdmissionController admission;
		auto made = PreparedGrouped32QueryReader::begin_full_generation_stream(
			admission, fixture.generation.head, fixture.generation.manifest,
			test_grouped32_expected_compatibility(),
			MinimizerSketchBackend::ExactSimd,
			fixture.generation.directory.encoded_bytes(), query, 10u);
		REQUIRE(made.has_value());
		auto reader = std::move(*made);
		const auto& position = grouped32_extent_row(fixture, 0u, true, 0u);
		require_wire_error(
			reader.accept_full_generation_position_chunk(
				position.address, position.bytes),
			MinimizerGenerationWireError::row_order);
		REQUIRE(reader.failed());
		REQUIRE(admission.reserved_bytes() == 0u);
	}

	SECTION("setup denial releases every admitted byte") {
		auto fixture = make_grouped32_reader_fixture(small_events, 100u);
		bioseq::parallelism::MemoryAdmissionController admission;
		admission.set_index_memory_cap_bytes(1u);
		require_wire_error(
			PreparedGrouped32QueryReader::begin_full_generation_stream(
				admission, fixture.generation.head,
				fixture.generation.manifest,
				test_grouped32_expected_compatibility(),
				MinimizerSketchBackend::ExactSimd,
				fixture.generation.directory.encoded_bytes(), query, 10u),
			MinimizerGenerationWireError::admission);
		REQUIRE(admission.reserved_bytes() == 0u);
	}

	SECTION("survivor growth denial poisons and releases immediately") {
		auto fixture = make_grouped32_reader_fixture(small_events, 100u);
		REQUIRE(fixture.generation.directory.entries().size() == 1u);
		const auto& entry = fixture.generation.directory.entries().front();
		const auto feed_controls = [&](auto& reader) {
			for (std::uint32_t chunk = 0;
				 chunk < entry.control_chunk_count; ++chunk) {
				const auto& row = grouped32_extent_row(
					fixture, entry.partition_id, false,
					entry.control_first_chunk + chunk);
				REQUIRE(reader.accept_full_generation_control_chunk(
					row.address, row.bytes).has_value());
			}
		};
		const auto& position = grouped32_extent_row(
			fixture, entry.partition_id, true, entry.position_first_chunk);

		{
			bioseq::parallelism::MemoryAdmissionController denied;
			{
				auto made =
					PreparedGrouped32QueryReader::begin_full_generation_stream(
						denied, fixture.generation.head,
						fixture.generation.manifest,
						test_grouped32_expected_compatibility(),
						MinimizerSketchBackend::ExactSimd,
						fixture.generation.directory.encoded_bytes(), query, 10u);
				REQUIRE(made.has_value());
				auto reader = std::move(*made);
				feed_controls(reader);
				denied.set_index_memory_cap_bytes(denied.reserved_bytes());
				require_wire_error(
					reader.accept_full_generation_position_chunk(
						position.address, position.bytes),
					MinimizerGenerationWireError::admission);
				REQUIRE(reader.failed());
				REQUIRE(denied.reserved_bytes() == 0u);
			}
			REQUIRE(denied.reserved_bytes() == 0u);
		}

		{
			bioseq::parallelism::MemoryAdmissionController admitted;
			{
				auto made =
					PreparedGrouped32QueryReader::begin_full_generation_stream(
						admitted, fixture.generation.head,
						fixture.generation.manifest,
						test_grouped32_expected_compatibility(),
						MinimizerSketchBackend::ExactSimd,
						fixture.generation.directory.encoded_bytes(), query, 10u);
				REQUIRE(made.has_value());
				auto reader = std::move(*made);
				feed_controls(reader);
				REQUIRE(reader.accept_full_generation_position_chunk(
					position.address, position.bytes).has_value());
				REQUIRE(reader.finish_full_generation_stream().has_value());
				REQUIRE(reader.retained_position_capacity() == 2u);
			}
			REQUIRE(admitted.reserved_bytes() == 0u);
		}
	}
	SECTION("selected control allocation denial poisons and releases") {
		constexpr std::uint64_t event_count = 30000u;
		std::vector<std::pair<std::uint64_t, std::uint64_t>> events;
		events.reserve(event_count);
		for (std::uint64_t index = 0; index < event_count; ++index) {
			events.emplace_back(index + 1u, index << 1u);
		}
		auto fixture = make_grouped32_reader_fixture(
			events, event_count + 1u);
		const auto entries = fixture.generation.directory.entries();
		const auto heavy = std::ranges::find_if(
			entries, [](const MinimizerBucketDirEntry& entry) {
				return entry.control_chunk_count > 1u;
			});
		REQUIRE(heavy != entries.end());
		const std::array<std::uint64_t, 1> heavy_query{heavy->lo_hash};
		const auto feed_before_last = [&](auto& reader) {
			for (const MinimizerBucketDirEntry& entry : entries) {
				const std::uint32_t stop =
					entry.partition_id == heavy->partition_id
					? entry.control_chunk_count - 1u
					: entry.control_chunk_count;
				for (std::uint32_t chunk = 0; chunk < stop; ++chunk) {
					const auto& row = grouped32_extent_row(
						fixture, entry.partition_id, false,
						entry.control_first_chunk + chunk);
					REQUIRE(reader.accept_full_generation_control_chunk(
						row.address, row.bytes).has_value());
				}
				if (entry.partition_id == heavy->partition_id) return;
			}
			FAIL("heavy control extent was not reached");
		};
		const auto& last = grouped32_extent_row(
			fixture, heavy->partition_id, false,
			heavy->control_first_chunk + heavy->control_chunk_count - 1u);

		{
			bioseq::parallelism::MemoryAdmissionController denied;
			{
				auto made =
					PreparedGrouped32QueryReader::begin_full_generation_stream(
						denied, fixture.generation.head,
						fixture.generation.manifest,
						test_grouped32_expected_compatibility(),
						MinimizerSketchBackend::ExactSimd,
						fixture.generation.directory.encoded_bytes(),
						heavy_query, 1u);
				REQUIRE(made.has_value());
				auto reader = std::move(*made);
				feed_before_last(reader);
				denied.set_index_memory_cap_bytes(denied.reserved_bytes());
				require_wire_error(
					reader.accept_full_generation_control_chunk(
						last.address, last.bytes),
					MinimizerGenerationWireError::admission);
				REQUIRE(reader.failed());
				REQUIRE(denied.reserved_bytes() == 0u);
			}
			REQUIRE(denied.reserved_bytes() == 0u);
		}

		{
			bioseq::parallelism::MemoryAdmissionController admitted;
			{
				auto made =
					PreparedGrouped32QueryReader::begin_full_generation_stream(
						admitted, fixture.generation.head,
						fixture.generation.manifest,
						test_grouped32_expected_compatibility(),
						MinimizerSketchBackend::ExactSimd,
						fixture.generation.directory.encoded_bytes(),
						heavy_query, 1u);
				REQUIRE(made.has_value());
				auto reader = std::move(*made);
				feed_before_last(reader);
				REQUIRE(reader.accept_full_generation_control_chunk(
					last.address, last.bytes).has_value());
			}
			REQUIRE(admitted.reserved_bytes() == 0u);
		}
	}
	SECTION("move leaves one admitted owner that can finish") {
		auto fixture = make_grouped32_reader_fixture(small_events, 100u);
		bioseq::parallelism::MemoryAdmissionController admission;
		auto made = PreparedGrouped32QueryReader::begin_full_generation_stream(
			admission, fixture.generation.head, fixture.generation.manifest,
			test_grouped32_expected_compatibility(),
			MinimizerSketchBackend::ExactSimd,
			fixture.generation.directory.encoded_bytes(), query, 10u);
		REQUIRE(made.has_value());
		auto source = std::move(*made);
		const std::uint64_t reserved = admission.reserved_bytes();
		PreparedGrouped32QueryReader destination{std::move(source)};
		REQUIRE(source.failed());
		REQUIRE(admission.reserved_bytes() == reserved);
		feed_grouped32_full_generation_physical(destination, fixture);
		REQUIRE(destination.finish_full_generation_stream().has_value());
		REQUIRE(destination.lookup(42u)->positions.size() == 2u);
	}

	SECTION("finish is allocation-free and incomplete finish releases") {
		const auto fixture = make_grouped32_reader_fixture(
			small_events, 100u);

		{
			bioseq::parallelism::MemoryAdmissionController incomplete;
			{
				auto made =
					PreparedGrouped32QueryReader::begin_full_generation_stream(
						incomplete, fixture.generation.head,
						fixture.generation.manifest,
						test_grouped32_expected_compatibility(),
						MinimizerSketchBackend::ExactSimd,
						fixture.generation.directory.encoded_bytes(), query, 10u);
				REQUIRE(made.has_value());
				auto reader = std::move(*made);
				const auto& entry =
					fixture.generation.directory.entries().front();
				for (std::uint32_t chunk = 0;
					 chunk < entry.control_chunk_count; ++chunk) {
					const auto& row = grouped32_extent_row(
						fixture, entry.partition_id, false,
						entry.control_first_chunk + chunk);
					REQUIRE(reader.accept_full_generation_control_chunk(
						row.address, row.bytes).has_value());
				}
				require_wire_error(
					reader.finish_full_generation_stream(),
					MinimizerGenerationWireError::row_count);
				REQUIRE(reader.failed());
				REQUIRE(incomplete.reserved_bytes() == 0u);
			}
			REQUIRE(incomplete.reserved_bytes() == 0u);
		}

		{
			bioseq::parallelism::MemoryAdmissionController exact;
			{
				auto made =
					PreparedGrouped32QueryReader::begin_full_generation_stream(
						exact, fixture.generation.head,
						fixture.generation.manifest,
						test_grouped32_expected_compatibility(),
						MinimizerSketchBackend::ExactSimd,
						fixture.generation.directory.encoded_bytes(), query, 10u);
				REQUIRE(made.has_value());
				auto reader = std::move(*made);
				feed_grouped32_full_generation_physical(reader, fixture);
				const std::uint64_t before_finish = exact.reserved_bytes();
				exact.set_index_memory_cap_bytes(before_finish);
				REQUIRE(reader.finish_full_generation_stream().has_value());
				REQUIRE(reader.ready());
				REQUIRE(reader.retained_position_capacity() == 2u);
				REQUIRE(exact.reserved_bytes() <= before_finish);
			}
			REQUIRE(exact.reserved_bytes() == 0u);
		}
	}
	SECTION("one selected position extent may span multiple physical chunks") {
		constexpr std::uint64_t occurrences = 70000u;
		std::vector<std::pair<std::uint64_t, std::uint64_t>> events;
		events.reserve(occurrences);
		for (std::uint64_t index = 0; index < occurrences; ++index) {
			events.emplace_back(42u, index << 1u);
		}
		auto fixture = make_grouped32_reader_fixture(events, occurrences + 1u);
		REQUIRE(fixture.generation.directory.entries().size() == 1u);
		REQUIRE(fixture.generation.directory.entries().front()
			.position_chunk_count > 1u);
		bioseq::parallelism::MemoryAdmissionController admission;
		auto made = PreparedGrouped32QueryReader::begin_full_generation_stream(
			admission, fixture.generation.head, fixture.generation.manifest,
			test_grouped32_expected_compatibility(),
			MinimizerSketchBackend::ExactSimd,
			fixture.generation.directory.encoded_bytes(), query, occurrences);
		REQUIRE(made.has_value());
		auto reader = std::move(*made);
		const auto& entry = fixture.generation.directory.entries().front();
		for (std::uint32_t chunk = 0;
			 chunk < entry.control_chunk_count; ++chunk) {
			const auto& row = grouped32_extent_row(
				fixture, entry.partition_id, false,
				entry.control_first_chunk + chunk);
			REQUIRE(reader.accept_full_generation_control_chunk(
				row.address, row.bytes).has_value());
		}
		const std::uint64_t before_positions = admission.reserved_bytes();
		const auto& first_position = grouped32_extent_row(
			fixture, entry.partition_id, true,
			entry.position_first_chunk);
		REQUIRE(reader.accept_full_generation_position_chunk(
			first_position.address, first_position.bytes).has_value());
		// The callback row is the only position payload scratch. Any growth is
		// retained global-u64 output for this chunk, under vector headroom; it
		// must not include a second allocation proportional to the full extent.
		const std::uint64_t first_chunk_retained_headroom =
			first_position.bytes.size()
			* (sizeof(std::uint64_t)
				/ kMinimizerGrouped32PositionEncodedBytes)
			* bioseq::parallelism::kVectorPreallocationHeadroomFactor;
		REQUIRE(admission.reserved_bytes()
			<= before_positions + first_chunk_retained_headroom);
		for (std::uint32_t chunk = 1;
			 chunk < entry.position_chunk_count; ++chunk) {
			const auto& row = grouped32_extent_row(
				fixture, entry.partition_id, true,
				entry.position_first_chunk + chunk);
			REQUIRE(reader.accept_full_generation_position_chunk(
				row.address, row.bytes).has_value());
		}
		REQUIRE(reader.finish_full_generation_stream().has_value());
		const auto found = reader.lookup(42u);
		REQUIRE(found.has_value());
		REQUIRE(found->positions.size() == occurrences);
		REQUIRE(reader.retained_position_capacity() == occurrences);
	}
}

TEST_CASE("inactive grouped32 reader poisons every incomplete or wrong-phase protocol",
          "[indexing][minimizer-hashmap][wire][v4][reader][poison]") {
    using namespace bioseq::indexing;
    STATIC_REQUIRE(std::is_move_constructible_v<
        PreparedGrouped32QueryReader>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<
        PreparedGrouped32QueryReader>);
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {42u, 3u << 1u},
        {42u, (9u << 1u) | 1u},
        {99u, 12u << 1u},
    };
    const auto queries = std::array<std::uint64_t, 1>{42u};
    auto fixture = make_grouped32_reader_fixture(minimizers, 100u);
    const auto begin_reader = [&](
            bioseq::parallelism::MemoryAdmissionController& admission) {
        return PreparedGrouped32QueryReader::begin(
            admission,
            fixture.generation.head,
            fixture.generation.manifest,
            test_grouped32_expected_compatibility(),
            MinimizerSketchBackend::ExactSimd,
            fixture.generation.directory.encoded_bytes(),
            queries,
            10u);
    };

    SECTION("premature lookup is terminal and preserves the first error") {
        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = begin_reader(admission);
        REQUIRE(made.has_value());
        auto reader = std::move(*made);
        REQUIRE_THROWS_AS(
            reader.lookup_spans(42u), PersistedMinimizerLookupError);
        REQUIRE_FALSE(reader.failed());
        require_wire_error(
            reader.lookup(42u), MinimizerGenerationWireError::chunk_range);
        REQUIRE(reader.failed());
        require_wire_error(
            reader.lookup((std::numeric_limits<std::uint64_t>::max)()),
            MinimizerGenerationWireError::chunk_range);
        require_wire_error(
            reader.finish_controls(), MinimizerGenerationWireError::chunk_range);
    }

    SECTION("custom move leaves one owner and a permanently failed source") {
        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = begin_reader(admission);
        REQUIRE(made.has_value());
        auto source = std::move(*made);
        REQUIRE(source.target_length() == 100u);
        const std::uint64_t reserved = admission.reserved_bytes();
        REQUIRE(reserved > 0u);
        {
            PreparedGrouped32QueryReader destination{std::move(source)};
            REQUIRE(source.failed());
            REQUIRE(source.target_length() == 0u);
            REQUIRE(destination.target_length() == 100u);
            REQUIRE(source.control_ordinals().empty());
            REQUIRE(source.retained_position_ordinals().empty());
            REQUIRE_THROWS_AS(
                source.lookup_spans(42u), PersistedMinimizerLookupError);
            require_wire_error(
                source.lookup(42u), MinimizerGenerationWireError::chunk_range);
            REQUIRE(admission.reserved_bytes() == reserved);

            const std::vector<std::uint32_t> controls{
                destination.control_ordinals().begin(),
                destination.control_ordinals().end()};
            feed_grouped32_extents(
                destination, fixture, controls, false);
            REQUIRE(destination.finish_controls().has_value());
            const std::vector<std::uint32_t> positions{
                destination.retained_position_ordinals().begin(),
                destination.retained_position_ordinals().end()};
            feed_grouped32_extents(
                destination, fixture, positions, true);
            REQUIRE(destination.finish_load().has_value());
            const auto lookup = destination.lookup(42u);
            REQUIRE(lookup.has_value());
            REQUIRE(lookup->global_occurrence_count == 2u);
            REQUIRE(lookup->positions.size() == 2u);
            const MinimizerLookupSpans direct =
                destination.lookup_spans(42u);
            REQUIRE(direct.positions_complete());
            REQUIRE(direct.contiguous_positions().size() == 2u);
            REQUIRE(direct.contiguous_positions()[0] == (3u << 1u));
            REQUIRE(direct.contiguous_positions()[1]
                == ((9u << 1u) | 1u));
        }
        // The moved-from source remains alive here, proving it retained no
        // second permit when destruction of the destination releases all bytes.
        REQUIRE(source.failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("premature controls finish is terminal") {
        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = begin_reader(admission);
        REQUIRE(made.has_value());
        auto reader = std::move(*made);
        const auto ordinal = reader.control_ordinals().front();
        require_wire_error(
            reader.finish_controls(), MinimizerGenerationWireError::chunk_range);
        REQUIRE(reader.failed());
        const auto& row = grouped32_extent_row(fixture, ordinal, false, 0u);
        require_wire_error(
            reader.accept_control_chunk(row.address, row.bytes),
            MinimizerGenerationWireError::chunk_range);
    }

    SECTION("position rows before controls are terminal") {
        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = begin_reader(admission);
        REQUIRE(made.has_value());
        auto reader = std::move(*made);
        const auto ordinal = reader.control_ordinals().front();
        const auto& position = grouped32_extent_row(
            fixture, ordinal, true, 0u);
        require_wire_error(
            reader.accept_position_chunk(position.address, position.bytes),
            MinimizerGenerationWireError::chunk_range);
        const auto& control = grouped32_extent_row(
            fixture, ordinal, false, 0u);
        require_wire_error(
            reader.accept_control_chunk(control.address, control.bytes),
            MinimizerGenerationWireError::chunk_range);
    }

    SECTION("wrong identity and byte count cannot be retried") {
        bioseq::parallelism::MemoryAdmissionController identity_admission;
        auto identity_made = begin_reader(identity_admission);
        REQUIRE(identity_made.has_value());
        auto identity_reader = std::move(*identity_made);
        const auto ordinal = identity_reader.control_ordinals().front();
        const auto& row = grouped32_extent_row(fixture, ordinal, false, 0u);
        auto wrong = row.address;
        ++wrong.generation_id;
        require_wire_error(
            identity_reader.accept_control_chunk(wrong, row.bytes),
            MinimizerGenerationWireError::chunk_range);
        require_wire_error(
            identity_reader.accept_control_chunk(row.address, row.bytes),
            MinimizerGenerationWireError::chunk_range);

        bioseq::parallelism::MemoryAdmissionController bytes_admission;
        auto bytes_made = begin_reader(bytes_admission);
        REQUIRE(bytes_made.has_value());
        auto bytes_reader = std::move(*bytes_made);
        require_wire_error(
            bytes_reader.accept_control_chunk(
                row.address,
                std::span<const std::byte>{row.bytes}.first(
                    row.bytes.size() - 1u)),
            MinimizerGenerationWireError::byte_count);
        require_wire_error(
            bytes_reader.accept_control_chunk(row.address, row.bytes),
            MinimizerGenerationWireError::byte_count);
    }

    SECTION("extent CRC mismatch is terminal") {
        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = begin_reader(admission);
        REQUIRE(made.has_value());
        auto reader = std::move(*made);
        const auto ordinal = reader.control_ordinals().front();
        const auto& row = grouped32_extent_row(fixture, ordinal, false, 0u);
        REQUIRE(fixture.generation.directory.entry_by_ordinal(ordinal)
            ->control_chunk_count == 1u);
        auto corrupt = row.bytes;
        corrupt.back() ^= std::byte{1};
        require_wire_error(
            reader.accept_control_chunk(row.address, corrupt),
            MinimizerGenerationWireError::checksum);
        require_wire_error(
            reader.accept_control_chunk(row.address, row.bytes),
            MinimizerGenerationWireError::checksum);
    }

    SECTION("extra callbacks poison an already-ready object") {
        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = begin_reader(admission);
        REQUIRE(made.has_value());
        auto reader = std::move(*made);
        const std::vector<std::uint32_t> controls{
            reader.control_ordinals().begin(), reader.control_ordinals().end()};
        feed_grouped32_extents(reader, fixture, controls, false);
        REQUIRE(reader.finish_controls().has_value());
        const std::vector<std::uint32_t> positions{
            reader.retained_position_ordinals().begin(),
            reader.retained_position_ordinals().end()};
        feed_grouped32_extents(reader, fixture, positions, true);
        REQUIRE(reader.finish_load().has_value());
        const auto& row = grouped32_extent_row(
            fixture, controls.front(), false, 0u);
        require_wire_error(
            reader.accept_control_chunk(row.address, row.bytes),
            MinimizerGenerationWireError::chunk_range);
        REQUIRE(reader.failed());
        require_wire_error(
            reader.lookup(42u), MinimizerGenerationWireError::chunk_range);
    }
}

TEST_CASE("inactive grouped32 reader validates every control and local position word",
          "[indexing][minimizer-hashmap][wire][v4][reader][semantic-validation]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t target_length = 100u;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {42u, 3u << 1u},
        {42u, (3u << 1u) | 1u},
        {99u, 4u << 1u},
    };
    const auto queries = std::array<std::uint64_t, 1>{42u};
    auto fixture = make_grouped32_reader_fixture(minimizers, target_length);
    REQUIRE(fixture.generation.directory.entries().size() == 1u);

    const auto begin_reader = [&](
            bioseq::parallelism::MemoryAdmissionController& admission,
            const MinimizerGenerationHead& head,
            const MinimizerGenerationManifest& manifest,
            std::span<const std::byte> directory) {
        return PreparedGrouped32QueryReader::begin(
            admission,
            head,
            manifest,
            test_grouped32_expected_compatibility(),
            MinimizerSketchBackend::ExactSimd,
            directory,
            queries,
            10u);
    };
    const auto rebuilt_after_payload_change = [&](bool positions) {
        std::vector<MinimizerBucketDirEntry> entries(
            fixture.generation.directory.entries().begin(),
            fixture.generation.directory.entries().end());
        const auto bytes = collect_encoded_extent(
            fixture.rows, entries.front(), !positions);
        if (positions) {
            entries.front().position_checksum = crc64_ecma182(bytes);
        } else {
            entries.front().control_checksum = crc64_ecma182(bytes);
        }
        return rebuild_generation_metadata(fixture.generation, entries);
    };

    SECTION("the same local bp on opposite strands is valid") {
        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = begin_reader(
            admission,
            fixture.generation.head,
            fixture.generation.manifest,
            fixture.generation.directory.encoded_bytes());
        REQUIRE(made.has_value());
        auto reader = std::move(*made);
        const std::vector<std::uint32_t> controls{
            reader.control_ordinals().begin(), reader.control_ordinals().end()};
        feed_grouped32_extents(reader, fixture, controls, false);
        REQUIRE(reader.finish_controls().has_value());
        const std::vector<std::uint32_t> positions{
            reader.retained_position_ordinals().begin(),
            reader.retained_position_ordinals().end()};
        feed_grouped32_extents(reader, fixture, positions, true);
        REQUIRE(reader.finish_load().has_value());
        const auto lookup = reader.lookup(42u);
        REQUIRE(lookup.has_value());
        REQUIRE(lookup->positions.size() == 2u);
        REQUIRE(lookup->positions[0] == (3u << 1u));
        REQUIRE(lookup->positions[1] == ((3u << 1u) | 1u));
    }

    SECTION("a duplicate encoded local position is rejected with a valid CRC") {
        auto& row = grouped32_extent_row(fixture, 0u, true, 0u);
        REQUIRE(row.bytes.size() >= 8u);
        const std::uint32_t first = test_read_le<std::uint32_t>(row.bytes, 0u);
        test_write_le<std::uint32_t>(row.bytes, 4u, first);
        const auto metadata = rebuilt_after_payload_change(true);
        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = begin_reader(
            admission, metadata.head, metadata.manifest,
            metadata.directory_bytes);
        REQUIRE(made.has_value());
        auto reader = std::move(*made);
        const std::vector<std::uint32_t> controls{
            reader.control_ordinals().begin(), reader.control_ordinals().end()};
        feed_grouped32_extents(reader, fixture, controls, false);
        REQUIRE(reader.finish_controls().has_value());
        const std::vector<std::uint32_t> positions{
            reader.retained_position_ordinals().begin(),
            reader.retained_position_ordinals().end()};
        feed_grouped32_extents(reader, fixture, positions, true);
        require_wire_error(
            reader.finish_load(), MinimizerGenerationWireError::position_range);
        REQUIRE(reader.failed());
    }

    SECTION("an invalid non-query position in a loaded extent is rejected") {
        auto& row = grouped32_extent_row(fixture, 0u, true, 0u);
        REQUIRE(row.bytes.size() >= 12u);
        test_write_le<std::uint32_t>(
            row.bytes, 8u, static_cast<std::uint32_t>(target_length << 1u));
        const auto metadata = rebuilt_after_payload_change(true);
        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = begin_reader(
            admission, metadata.head, metadata.manifest,
            metadata.directory_bytes);
        REQUIRE(made.has_value());
        auto reader = std::move(*made);
        const std::vector<std::uint32_t> controls{
            reader.control_ordinals().begin(), reader.control_ordinals().end()};
        feed_grouped32_extents(reader, fixture, controls, false);
        REQUIRE(reader.finish_controls().has_value());
        const std::vector<std::uint32_t> positions{
            reader.retained_position_ordinals().begin(),
            reader.retained_position_ordinals().end()};
        feed_grouped32_extents(reader, fixture, positions, true);
        require_wire_error(
            reader.finish_load(), MinimizerGenerationWireError::position_range);
    }

    SECTION("a duplicate control hash is rejected with a valid CRC") {
        auto& row = grouped32_extent_row(fixture, 0u, false, 0u);
        REQUIRE(row.bytes.size()
            >= 2u * kMinimizerGrouped32ControlEncodedBytes);
        test_write_le<std::uint32_t>(
            row.bytes, kMinimizerGrouped32ControlEncodedBytes, 42u);
        const auto metadata = rebuilt_after_payload_change(false);
        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = begin_reader(
            admission, metadata.head, metadata.manifest,
            metadata.directory_bytes);
        REQUIRE(made.has_value());
        auto reader = std::move(*made);
        const std::vector<std::uint32_t> controls{
            reader.control_ordinals().begin(), reader.control_ordinals().end()};
        feed_grouped32_extents(reader, fixture, controls, false);
        require_wire_error(
            reader.finish_controls(), MinimizerGenerationWireError::control_range);
    }
}

TEST_CASE("grouped32 reader W2 validation preserves non-scatter semantics",
          "[indexing][minimizer-hashmap][wire][v4][reader][simd][vector-stream]") {
    using namespace bioseq::indexing;
    using D8 = hwy::HWY_NAMESPACE::ScalableTag<std::uint8_t>;
    using D32 = hwy::HWY_NAMESPACE::Repartition<std::uint32_t, D8>;
    D32 d32;
    const std::size_t lanes = hwy::HWY_NAMESPACE::Lanes(d32);
    constexpr std::size_t stream_width = 2u;
    REQUIRE(lanes > 0u);
#if defined(__aarch64__) || defined(__arm64__)
    REQUIRE(HWY_TARGET == HWY_NEON);
#else
    REQUIRE(HWY_TARGET != HWY_SCALAR);
#endif

    // Hash 42 is queried and therefore takes the existing scalar scatter
    // route. Hash 99 is deliberately not queried: it shares the retained
    // extent and is long enough for two complete W=2 validation iterations
    // plus a scalar tail.
    const std::size_t intra_stream_boundary = 1u + lanes;
    const std::size_t cross_iteration_boundary =
        1u + stream_width * lanes;
    const std::size_t scalar_tail_boundary =
        1u + 2u * stream_width * lanes;
    const std::size_t nonquery_count = scalar_tail_boundary + 2u;
    const std::uint64_t target_length =
        static_cast<std::uint64_t>(nonquery_count) + 64u;
    CAPTURE(lanes, nonquery_count, target_length);

    std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers;
    minimizers.reserve(nonquery_count + 1u);
    minimizers.emplace_back(42u, 0u);
    for (std::size_t index = 0; index < nonquery_count; ++index) {
        minimizers.emplace_back(
            99u, static_cast<std::uint64_t>(4u + index));
    }
    const auto query = std::array<std::uint64_t, 1>{42u};
    auto fixture = make_grouped32_reader_fixture(minimizers, target_length);
    REQUIRE(fixture.generation.directory.entries().size() == 1u);
    REQUIRE(fixture.generation.directory.entries().front()
        .position_chunk_count == 1u);

    const auto read_nonquery = [](
            const Grouped32ReaderFixture& value,
            std::size_t local_index) {
        const auto& row = grouped32_extent_row(value, 0u, true, 0u);
        const auto bytes = std::span<const std::byte>{row.bytes};
        const std::size_t element = 1u + local_index;
        REQUIRE(element * kMinimizerGrouped32PositionEncodedBytes
            <= bytes.size() - kMinimizerGrouped32PositionEncodedBytes);
        return test_read_le<std::uint32_t>(
            bytes, element * kMinimizerGrouped32PositionEncodedBytes);
    };
    const auto write_nonquery = [](
            Grouped32ReaderFixture& value,
            std::size_t local_index,
            std::uint32_t encoded) {
        auto& row = grouped32_extent_row(value, 0u, true, 0u);
        auto bytes = std::span<std::byte>{row.bytes};
        const std::size_t element = 1u + local_index;
        REQUIRE(element * kMinimizerGrouped32PositionEncodedBytes
            <= bytes.size() - kMinimizerGrouped32PositionEncodedBytes);
        test_write_le<std::uint32_t>(
            bytes,
            element * kMinimizerGrouped32PositionEncodedBytes,
            encoded);
    };
    struct FinishObservation {
        std::expected<void, MinimizerGenerationWireError> outcome;
        PreparedGrouped32QueryReaderSimdRouteEvidence evidence;
    };
    const auto finish_reader = [query](Grouped32ReaderFixture& value) {
        std::vector<MinimizerBucketDirEntry> entries(
            value.generation.directory.entries().begin(),
            value.generation.directory.entries().end());
        REQUIRE(entries.size() == 1u);
        entries.front().position_checksum = crc64_ecma182(
            collect_encoded_extent(value.rows, entries.front(), false));
        const auto metadata = rebuild_generation_metadata(
            value.generation, entries);

        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = PreparedGrouped32QueryReader::begin(
            admission,
            metadata.head,
            metadata.manifest,
            test_grouped32_expected_compatibility(),
            MinimizerSketchBackend::ExactSimd,
            metadata.directory_bytes,
            query,
            64u);
        REQUIRE(made.has_value());
        auto reader = std::move(*made);
        const std::vector<std::uint32_t> controls{
            reader.control_ordinals().begin(),
            reader.control_ordinals().end()};
        feed_grouped32_extents(reader, value, controls, false);
        REQUIRE(reader.finish_controls().has_value());
        const std::vector<std::uint32_t> positions{
            reader.retained_position_ordinals().begin(),
            reader.retained_position_ordinals().end()};
        REQUIRE(positions.size() == 1u);
        feed_grouped32_extents(reader, value, positions, true);
        auto outcome = reader.finish_load();
        return FinishObservation{
            .outcome = std::move(outcome),
            .evidence = reader.simd_route_evidence(),
        };
    };
    const auto require_w2_evidence = [lanes](
            const FinishObservation& observed,
            bool accepted) {
        REQUIRE_FALSE(observed.evidence.saturated);
        REQUIRE(observed.evidence.selected_hwy_target
            == static_cast<std::uint64_t>(HWY_TARGET));
        REQUIRE(observed.evidence.selected_vector_stream_width == 2u);
        REQUIRE(observed.evidence.attempted_spans == 1u);
        REQUIRE(observed.evidence.scalar_required_spans == 0u);
        REQUIRE(observed.evidence.vectorized_words
            == 2u * stream_width * lanes);
        REQUIRE(observed.evidence.accepted_spans
            == static_cast<std::uint64_t>(accepted));
        REQUIRE(observed.evidence.rejected_spans
            == static_cast<std::uint64_t>(!accepted));
    };

    SECTION("a long valid non-scatter slice is accepted") {
        const auto observed = finish_reader(fixture);
        REQUIRE(observed.outcome.has_value());
        require_w2_evidence(observed, true);
    }

    SECTION("scalar-to-vector predecessor corruption is rejected") {
        write_nonquery(fixture, 1u, read_nonquery(fixture, 0u));
        const auto observed = finish_reader(fixture);
        require_wire_error(
            observed.outcome,
            MinimizerGenerationWireError::position_range);
        require_w2_evidence(observed, false);
    }

    SECTION("an intra-iteration stream boundary corruption is rejected") {
        write_nonquery(
            fixture,
            intra_stream_boundary,
            read_nonquery(fixture, intra_stream_boundary - 1u));
        const auto observed = finish_reader(fixture);
        require_wire_error(
            observed.outcome,
            MinimizerGenerationWireError::position_range);
        require_w2_evidence(observed, false);
    }

    SECTION("a cross-iteration predecessor corruption is rejected") {
        write_nonquery(
            fixture,
            cross_iteration_boundary,
            read_nonquery(fixture, cross_iteration_boundary - 1u) - 1u);
        const auto observed = finish_reader(fixture);
        require_wire_error(
            observed.outcome,
            MinimizerGenerationWireError::position_range);
        require_w2_evidence(observed, false);
    }

    SECTION("a scalar-tail predecessor corruption is rejected") {
        write_nonquery(
            fixture,
            scalar_tail_boundary,
            read_nonquery(fixture, scalar_tail_boundary - 1u));
        const auto observed = finish_reader(fixture);
        require_wire_error(
            observed.outcome,
            MinimizerGenerationWireError::position_range);
        require_w2_evidence(observed, false);
    }

    SECTION("an ordered vector block outside the segment is rejected") {
        const auto first_out_of_range = static_cast<std::uint32_t>(
            target_length << 1u);
        for (std::size_t local_index = 2u;
             local_index < nonquery_count;
             ++local_index) {
            write_nonquery(
                fixture,
                local_index,
                first_out_of_range
                    + static_cast<std::uint32_t>(local_index - 2u));
        }
        const auto observed = finish_reader(fixture);
        require_wire_error(
            observed.outcome,
            MinimizerGenerationWireError::position_range);
        require_w2_evidence(observed, false);
    }
}

TEST_CASE("inactive grouped32 reader handles empty, zero-cutoff, and exact admission",
          "[indexing][minimizer-hashmap][wire][v4][reader][admission][empty]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {42u, 3u << 1u},
        {42u, (9u << 1u) | 1u},
    };
    auto fixture = make_grouped32_reader_fixture(minimizers, 100u);

    SECTION("an empty exact query union reads no payload") {
        bioseq::parallelism::MemoryAdmissionController admission;
        const std::span<const std::uint64_t> empty_queries;
        auto made = PreparedGrouped32QueryReader::begin(
            admission,
            fixture.generation.head,
            fixture.generation.manifest,
            test_grouped32_expected_compatibility(),
            MinimizerSketchBackend::ExactSimd,
            fixture.generation.directory.encoded_bytes(),
            empty_queries,
            10u);
        REQUIRE(made.has_value());
        auto reader = std::move(*made);
        REQUIRE(reader.control_ordinals().empty());
        REQUIRE(reader.finish_controls().has_value());
        REQUIRE(reader.retained_position_ordinals().empty());
        REQUIRE(reader.finish_load().has_value());
        REQUIRE(reader.ready());
        REQUIRE_THROWS_AS(
            reader.lookup_spans(42u), PersistedMinimizerLookupError);
        require_wire_error(
            reader.lookup(42u), MinimizerGenerationWireError::hash_range);
    }

    SECTION("max-occ zero retains exact counts but authorizes no positions") {
        bioseq::parallelism::MemoryAdmissionController admission;
        const auto query = std::array<std::uint64_t, 1>{42u};
        auto made = PreparedGrouped32QueryReader::begin(
            admission,
            fixture.generation.head,
            fixture.generation.manifest,
            test_grouped32_expected_compatibility(),
            MinimizerSketchBackend::ExactSimd,
            fixture.generation.directory.encoded_bytes(),
            query,
            0u);
        REQUIRE(made.has_value());
        auto reader = std::move(*made);
        const std::vector<std::uint32_t> controls{
            reader.control_ordinals().begin(), reader.control_ordinals().end()};
        feed_grouped32_extents(reader, fixture, controls, false);
        REQUIRE(reader.finish_controls().has_value());
        REQUIRE(reader.retained_position_ordinals().empty());
        REQUIRE(reader.finish_load().has_value());
        const auto lookup = reader.lookup(42u);
        REQUIRE(lookup.has_value());
        REQUIRE(lookup->global_occurrence_count == 2u);
        REQUIRE_FALSE(lookup->positions_complete);
        REQUIRE(lookup->positions.empty());
        const MinimizerLookupSpans direct = reader.lookup_spans(42u);
        REQUIRE(direct.global_occurrence_count() == 2u);
        REQUIRE_FALSE(direct.positions_complete());
        REQUIRE(direct.well_formed());
    }

    SECTION("ready ownership charges exactly one u64 per retained position") {
        const auto query = std::array<std::uint64_t, 1>{42u};
        const auto ready_owned_bytes = [&](std::uint64_t max_occ) {
            bioseq::parallelism::MemoryAdmissionController admission;
            auto made = PreparedGrouped32QueryReader::begin(
                admission,
                fixture.generation.head,
                fixture.generation.manifest,
                test_grouped32_expected_compatibility(),
                MinimizerSketchBackend::ExactSimd,
                fixture.generation.directory.encoded_bytes(),
                query,
                max_occ);
            REQUIRE(made.has_value());
            auto reader = std::move(*made);
            const std::vector<std::uint32_t> controls{
                reader.control_ordinals().begin(),
                reader.control_ordinals().end()};
            feed_grouped32_extents(reader, fixture, controls, false);
            REQUIRE(reader.finish_controls().has_value());
            const std::vector<std::uint32_t> positions{
                reader.retained_position_ordinals().begin(),
                reader.retained_position_ordinals().end()};
            feed_grouped32_extents(reader, fixture, positions, true);
            REQUIRE(reader.finish_load().has_value());
            REQUIRE(reader.ready());
            return admission.reserved_bytes();
        };
        const std::uint64_t pruned_bytes = ready_owned_bytes(0u);
        const std::uint64_t retained_bytes = ready_owned_bytes(2u);
        REQUIRE(retained_bytes == pruned_bytes + 2u * sizeof(std::uint64_t));
    }

    SECTION("one byte below the observed exact begin ownership is rejected") {
        const auto query = std::array<std::uint64_t, 1>{42u};
        std::uint64_t required = 0;
        {
            bioseq::parallelism::MemoryAdmissionController probe;
            auto made = PreparedGrouped32QueryReader::begin(
                probe,
                fixture.generation.head,
                fixture.generation.manifest,
                test_grouped32_expected_compatibility(),
                MinimizerSketchBackend::ExactSimd,
                fixture.generation.directory.encoded_bytes(),
                query,
                10u);
            REQUIRE(made.has_value());
            required = probe.reserved_bytes();
            REQUIRE(required > 0u);
        }
        bioseq::parallelism::MemoryAdmissionController limited;
        limited.set_index_memory_cap_bytes(required - 1u);
        require_wire_error(
            PreparedGrouped32QueryReader::begin(
                limited,
                fixture.generation.head,
                fixture.generation.manifest,
                test_grouped32_expected_compatibility(),
                MinimizerSketchBackend::ExactSimd,
                fixture.generation.directory.encoded_bytes(),
                query,
                10u),
            MinimizerGenerationWireError::admission);
        REQUIRE(limited.reserved_bytes() == 0u);
    }

    SECTION("duplicate-heavy begin canonicalizes to one exact reservation") {
        const auto unique = std::array<std::uint64_t, 1>{42u};
        std::array<std::uint64_t, 32> duplicate_heavy{};
        duplicate_heavy.fill(42u);
        std::uint64_t exact = 0;
        {
            bioseq::parallelism::MemoryAdmissionController admission;
            auto made = PreparedGrouped32QueryReader::begin(
                admission,
                fixture.generation.head,
                fixture.generation.manifest,
                test_grouped32_expected_compatibility(),
                MinimizerSketchBackend::ExactSimd,
                fixture.generation.directory.encoded_bytes(),
                unique,
                10u);
            REQUIRE(made.has_value());
            exact = admission.reserved_bytes();
            REQUIRE(exact > 0u);
        }
        {
            bioseq::parallelism::MemoryAdmissionController admission;
            auto made = PreparedGrouped32QueryReader::begin(
                admission,
                fixture.generation.head,
                fixture.generation.manifest,
                test_grouped32_expected_compatibility(),
                MinimizerSketchBackend::ExactSimd,
                fixture.generation.directory.encoded_bytes(),
                duplicate_heavy,
                10u);
            REQUIRE(made.has_value());
            REQUIRE(admission.reserved_bytes() == exact);
        }
        const auto minimum_peak_cap = [&](
                std::span<const std::uint64_t> hashes) {
            const auto admitted = [&](std::uint64_t cap) {
                bioseq::parallelism::MemoryAdmissionController admission;
                admission.set_index_memory_cap_bytes(cap);
                auto made = PreparedGrouped32QueryReader::begin(
                    admission,
                    fixture.generation.head,
                    fixture.generation.manifest,
                    test_grouped32_expected_compatibility(),
                    MinimizerSketchBackend::ExactSimd,
                    fixture.generation.directory.encoded_bytes(),
                    hashes,
                    10u);
                if (!made) {
                    REQUIRE(made.error()
                        == MinimizerGenerationWireError::admission);
                    REQUIRE(admission.reserved_bytes() == 0u);
                    return false;
                }
                REQUIRE(admission.reserved_bytes() == exact);
                return true;
            };
            std::uint64_t lower = exact;
            std::uint64_t upper = exact;
            while (!admitted(upper)) {
                REQUIRE(upper
                    <= (std::numeric_limits<std::uint64_t>::max)() / 2u);
                upper *= 2u;
            }
            while (lower < upper) {
                const std::uint64_t middle =
                    lower + (upper - lower) / 2u;
                if (admitted(middle)) {
                    upper = middle;
                } else {
                    lower = middle + 1u;
                }
            }
            return lower;
        };
        const std::uint64_t unique_peak = minimum_peak_cap(unique);
        const std::uint64_t duplicate_peak =
            minimum_peak_cap(duplicate_heavy);
        REQUIRE(duplicate_peak == unique_peak);
        REQUIRE(duplicate_peak >= exact);
        {
            bioseq::parallelism::MemoryAdmissionController admission;
            admission.set_index_memory_cap_bytes(duplicate_peak);
            auto made = PreparedGrouped32QueryReader::begin(
                admission,
                fixture.generation.head,
                fixture.generation.manifest,
                test_grouped32_expected_compatibility(),
                MinimizerSketchBackend::ExactSimd,
                fixture.generation.directory.encoded_bytes(),
                duplicate_heavy,
                10u);
            REQUIRE(made.has_value());
            REQUIRE(admission.reserved_bytes() == exact);
        }
        bioseq::parallelism::MemoryAdmissionController one_short;
        one_short.set_index_memory_cap_bytes(duplicate_peak - 1u);
        require_wire_error(
            PreparedGrouped32QueryReader::begin(
                one_short,
                fixture.generation.head,
                fixture.generation.manifest,
                test_grouped32_expected_compatibility(),
                MinimizerSketchBackend::ExactSimd,
                fixture.generation.directory.encoded_bytes(),
                duplicate_heavy,
                10u),
            MinimizerGenerationWireError::admission);
        REQUIRE(one_short.reserved_bytes() == 0u);
    }

    SECTION("controls-to-positions peak is exact and ready ownership shrinks") {
        const auto query = std::array<std::uint64_t, 1>{42u};
        const auto begin_reader = [&fixture, &query](
                bioseq::parallelism::MemoryAdmissionController& admission) {
            return PreparedGrouped32QueryReader::begin(
                admission,
                fixture.generation.head,
                fixture.generation.manifest,
                test_grouped32_expected_compatibility(),
                MinimizerSketchBackend::ExactSimd,
                fixture.generation.directory.encoded_bytes(),
                query,
                10u);
        };
        std::uint64_t positions_peak = 0;
        {
            bioseq::parallelism::MemoryAdmissionController probe;
            auto made = begin_reader(probe);
            REQUIRE(made.has_value());
            auto reader = std::move(*made);
            const std::vector<std::uint32_t> controls{
                reader.control_ordinals().begin(),
                reader.control_ordinals().end()};
            feed_grouped32_extents(reader, fixture, controls, false);
            REQUIRE(reader.finish_controls().has_value());
            positions_peak = probe.reserved_bytes();
            REQUIRE(positions_peak > 0u);
        }
        {
            bioseq::parallelism::MemoryAdmissionController one_short;
            auto made = begin_reader(one_short);
            REQUIRE(made.has_value());
            auto reader = std::move(*made);
            const std::vector<std::uint32_t> controls{
                reader.control_ordinals().begin(),
                reader.control_ordinals().end()};
            feed_grouped32_extents(reader, fixture, controls, false);
            one_short.set_index_memory_cap_bytes(positions_peak - 1u);
            require_wire_error(
                reader.finish_controls(),
                MinimizerGenerationWireError::admission);
            REQUIRE(reader.failed());
        }
        bioseq::parallelism::MemoryAdmissionController exact;
        {
            auto made = begin_reader(exact);
            REQUIRE(made.has_value());
            auto reader = std::move(*made);
            const std::vector<std::uint32_t> controls{
                reader.control_ordinals().begin(),
                reader.control_ordinals().end()};
            feed_grouped32_extents(reader, fixture, controls, false);
            exact.set_index_memory_cap_bytes(positions_peak);
            REQUIRE(reader.finish_controls().has_value());
            REQUIRE(exact.reserved_bytes() == positions_peak);
            const std::vector<std::uint32_t> positions{
                reader.retained_position_ordinals().begin(),
                reader.retained_position_ordinals().end()};
            feed_grouped32_extents(reader, fixture, positions, true);
            REQUIRE(reader.finish_load().has_value());
            REQUIRE(reader.ready());
            REQUIRE(exact.reserved_bytes() < positions_peak);
        }
        REQUIRE(exact.reserved_bytes() == 0u);
    }

    SECTION("backend/scheme and query hash identities fail closed") {
        const auto query = std::array<std::uint64_t, 1>{42u};
        bioseq::parallelism::MemoryAdmissionController backend_admission;
        require_wire_error(
            PreparedGrouped32QueryReader::begin(
                backend_admission,
                fixture.generation.head,
                fixture.generation.manifest,
                test_grouped32_expected_compatibility(),
                MinimizerSketchBackend::PaperNtHashSimd,
                fixture.generation.directory.encoded_bytes(),
                query,
                10u),
            MinimizerGenerationWireError::compatibility_mismatch);

        const auto scalar_fixture = make_grouped32_reader_fixture(
            minimizers,
            100u,
            kLegacyMinimapV1SchemeIdentity,
            minimizer_producer_backend_identity_for_backend(
                MinimizerSketchBackend::Scalar));
        auto scalar_expected = test_grouped32_expected_compatibility();
        scalar_expected.producer_backend_id =
            MinimizerProducerBackendId::scalar;
        bioseq::parallelism::MemoryAdmissionController producer_admission;
        require_wire_error(
            PreparedGrouped32QueryReader::begin_full_generation_stream(
                producer_admission,
                scalar_fixture.generation.head,
                scalar_fixture.generation.manifest,
                scalar_expected,
                MinimizerSketchBackend::ExactSimd,
                scalar_fixture.generation.directory.encoded_bytes(),
                query,
                10u),
            MinimizerGenerationWireError::compatibility_mismatch);
        REQUIRE(producer_admission.reserved_bytes() == 0u);

        const auto out_of_range = std::array<std::uint64_t, 1>{
            static_cast<std::uint64_t>(kLegacyMinimapK15MaxHash) + 1u};
        bioseq::parallelism::MemoryAdmissionController hash_admission;
        require_wire_error(
            PreparedGrouped32QueryReader::begin(
                hash_admission,
                fixture.generation.head,
                fixture.generation.manifest,
                test_grouped32_expected_compatibility(),
                MinimizerSketchBackend::ExactSimd,
                fixture.generation.directory.encoded_bytes(),
                out_of_range,
                10u),
            MinimizerGenerationWireError::hash_range);
    }
}

TEST_CASE("inactive grouped32 reader qualifies full-width Paper metadata",
          "[indexing][minimizer-hashmap][wire][v4][reader][paper][identity]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t max_hash =
        (std::numeric_limits<std::uint32_t>::max)();
    constexpr std::uint64_t reverse_at_17 = (17u << 1u) | 1u;
    const std::array minimizers{
        std::pair<std::uint64_t, std::uint64_t>{max_hash, reverse_at_17}};
    auto fixture = make_grouped32_reader_fixture(
        minimizers, 100u, kPaperNtHashU32R7SumV1SchemeIdentity);
    auto expected = test_grouped32_expected_compatibility();
    expected.minimizer_scheme_id = kPaperNtHashU32R7SumV1SchemeId;
    expected.minimizer_scheme_version =
        kPaperNtHashU32R7SumV1SchemeVersion;
    expected.producer_backend_id =
        MinimizerProducerBackendId::paper_nthash_simd;
    const auto query = std::array<std::uint64_t, 1>{max_hash};

    bioseq::parallelism::MemoryAdmissionController admission;
    auto made = PreparedGrouped32QueryReader::begin(
        admission,
        fixture.generation.head,
        fixture.generation.manifest,
        expected,
        MinimizerSketchBackend::PaperNtHashSimd,
        fixture.generation.directory.encoded_bytes(),
        query,
        10u);
    REQUIRE(made.has_value());
    auto reader = std::move(*made);
    REQUIRE(reader.qualified_sketch_backend()
        == MinimizerSketchBackend::PaperNtHashSimd);
    const std::vector<std::uint32_t> controls{
        reader.control_ordinals().begin(), reader.control_ordinals().end()};
    feed_grouped32_extents(reader, fixture, controls, false, true);
    REQUIRE(reader.finish_controls().has_value());
    const std::vector<std::uint32_t> positions{
        reader.retained_position_ordinals().begin(),
        reader.retained_position_ordinals().end()};
    feed_grouped32_extents(reader, fixture, positions, true, true);
    REQUIRE(reader.finish_load().has_value());
    const auto lookup = reader.lookup(max_hash);
    REQUIRE(lookup.has_value());
    REQUIRE(lookup->global_occurrence_count == 1u);
    REQUIRE(lookup->positions_complete);
    REQUIRE(lookup->positions.size() == 1u);
    REQUIRE(lookup->positions.front() == reverse_at_17);

    bioseq::parallelism::MemoryAdmissionController mismatch_admission;
    require_wire_error(
        PreparedGrouped32QueryReader::begin(
            mismatch_admission,
            fixture.generation.head,
            fixture.generation.manifest,
            expected,
            MinimizerSketchBackend::ExactSimd,
            fixture.generation.directory.encoded_bytes(),
            query,
            10u),
        MinimizerGenerationWireError::compatibility_mismatch);
    REQUIRE(mismatch_admission.reserved_bytes() == 0u);
}

TEST_CASE("inactive grouped32 reader streams a 70k survivor with exact chunk order",
          "[indexing][minimizer-hashmap][wire][v4][reader][heavy][misaligned]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t count = 70000u;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers;
    minimizers.reserve(count);
    for (std::uint64_t index = 0; index < count; ++index) {
        minimizers.emplace_back(
            42u, (index << 1u) | static_cast<std::uint64_t>(index & 1u));
    }
    auto fixture = make_grouped32_reader_fixture(minimizers, 100000u);
    const auto query = std::array<std::uint64_t, 1>{42u};
    const auto begin_reader = [&](
            bioseq::parallelism::MemoryAdmissionController& admission) {
        return PreparedGrouped32QueryReader::begin(
            admission,
            fixture.generation.head,
            fixture.generation.manifest,
            test_grouped32_expected_compatibility(),
            MinimizerSketchBackend::ExactSimd,
            fixture.generation.directory.encoded_bytes(),
            query,
            count);
    };

    {
        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = begin_reader(admission);
        REQUIRE(made.has_value());
        auto reader = std::move(*made);
        const std::vector<std::uint32_t> controls{
            reader.control_ordinals().begin(), reader.control_ordinals().end()};
        feed_grouped32_extents(reader, fixture, controls, false);
        REQUIRE(reader.finish_controls().has_value());
        REQUIRE(reader.retained_position_ordinals().size() == 1u);
        const std::uint32_t ordinal =
            reader.retained_position_ordinals().front();
        const auto* entry = fixture.generation.directory.entry_by_ordinal(ordinal);
        REQUIRE(entry != nullptr);
        REQUIRE(entry->position_chunk_count > 1u);
        const auto& second = grouped32_extent_row(
            fixture, ordinal, true, 1u);
        require_wire_error(
            reader.accept_position_chunk(second.address, second.bytes),
            MinimizerGenerationWireError::chunk_range);
        const auto& first = grouped32_extent_row(
            fixture, ordinal, true, 0u);
        require_wire_error(
            reader.accept_position_chunk(first.address, first.bytes),
            MinimizerGenerationWireError::chunk_range);
        REQUIRE(reader.failed());
    }

    bioseq::parallelism::MemoryAdmissionController admission;
    auto made = begin_reader(admission);
    REQUIRE(made.has_value());
    auto reader = std::move(*made);
    const std::vector<std::uint32_t> controls{
        reader.control_ordinals().begin(), reader.control_ordinals().end()};
    feed_grouped32_extents(reader, fixture, controls, false, true);
    REQUIRE(reader.finish_controls().has_value());
    const std::vector<std::uint32_t> positions{
        reader.retained_position_ordinals().begin(),
        reader.retained_position_ordinals().end()};
    feed_grouped32_extents(reader, fixture, positions, true, true);
    REQUIRE(reader.finish_load().has_value());
    const auto lookup = reader.lookup(42u);
    REQUIRE(lookup.has_value());
    REQUIRE(lookup->global_occurrence_count == count);
    REQUIRE(lookup->positions_complete);
    REQUIRE(lookup->positions.size() == count);
    REQUIRE(lookup->positions.front() == 0u);
    REQUIRE(lookup->positions.back() == (((count - 1u) << 1u) | 1u));
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
            generation->directory.encoded_bytes(), segment, ordinals,
            generation->directory.profile());
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
            for (const bool position_extent : {false, true}) {
                for (const std::uint32_t ordinal : ordinals) {
                    const auto* entry =
                        generation->directory.entry_by_ordinal(ordinal);
                    REQUIRE(entry != nullptr);
                    const auto position_sub_key =
                        derive_minimizer_position_extent_sub_key(
                            entry->sub_key);
                    REQUIRE(position_sub_key.has_value());
                    const index_sub_key_t physical_sub_key = position_extent
                        ? *position_sub_key : entry->sub_key;
                    const std::uint32_t chunk_count = position_extent
                        ? entry->position_chunk_count
                        : entry->control_chunk_count;
                    for (std::uint32_t chunk = 0;
                         chunk < chunk_count; ++chunk) {
                        const auto row = std::ranges::find_if(
                            sink.rows,
                            [entry, physical_sub_key, chunk](
                                    const CollectedMinimizerRow& value) {
                                return value.address.position_segment
                                        == entry->position_segment
                                    && value.address.sub_key
                                        == physical_sub_key
                                    && value.address.chunk_no == chunk;
                            });
                        REQUIRE(row != sink.rows.end());
                        REQUIRE(prepared->accept_chunk(
                            row->address,
                            reinterpret_cast<const char*>(row->bytes.data()),
                            row->bytes.size()).has_value());
                    }
                }
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
        generation->directory.encoded_bytes(), 0, ordinal,
        generation->directory.profile());
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

TEST_CASE("DOC0371 prepared generation exposes complete and pruned hash spans",
          "[indexing][minimizer-hashmap][wire][prepared][segmented]") {
    using namespace bioseq::indexing;
    STATIC_REQUIRE(SegmentedMinimizerLookupIndex<
        PreparedSerializedGeneration>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<
        PreparedSerializedGeneration>);
    constexpr std::uint64_t span = kMinimizerPreferredSegmentSpanBp;
    constexpr auto encoded_position = [](
            std::uint64_t position, std::uint64_t strand) {
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
    auto persisted = encode_minimizer_generation(
        producer, sink, test_generation_options(span + 100u));
    REQUIRE(persisted.has_value());
    REQUIRE(persisted->manifest.position_segment_count == 2);

    bioseq::parallelism::MemoryAdmissionController controller;
    std::vector<PreparedSerializedSegment> segments;
    segments.reserve(persisted->manifest.position_segment_count);
    for (std::uint32_t segment = 0;
         segment < persisted->manifest.position_segment_count;
         ++segment) {
        std::vector<std::uint32_t> ordinals;
        for (const MinimizerBucketDirEntry& entry
                : persisted->directory.entries()) {
            if (entry.position_segment == static_cast<std::int32_t>(segment)) {
                ordinals.push_back(entry.partition_id);
            }
        }
        auto prepared = PreparedSerializedSegment::begin_load(
            controller,
            persisted->head,
            persisted->manifest,
            test_expected_compatibility(),
            persisted->directory.encoded_bytes(),
            static_cast<std::int32_t>(segment),
            ordinals);
        REQUIRE(prepared.has_value());
        for (const bool position_extent : {false, true}) {
            for (const std::uint32_t ordinal : ordinals) {
                const MinimizerBucketDirEntry* entry =
                    persisted->directory.entry_by_ordinal(ordinal);
                REQUIRE(entry != nullptr);
                const auto position_sub_key =
                    derive_minimizer_position_extent_sub_key(entry->sub_key);
                REQUIRE(position_sub_key.has_value());
                const index_sub_key_t physical_sub_key = position_extent
                    ? *position_sub_key : entry->sub_key;
                const std::uint32_t chunk_count = position_extent
                    ? entry->position_chunk_count : entry->control_chunk_count;
                for (std::uint32_t chunk = 0; chunk < chunk_count; ++chunk) {
                    const auto row = std::ranges::find_if(
                        sink.rows,
                        [entry, physical_sub_key, chunk](
                                const CollectedMinimizerRow& value) {
                            return value.address.position_segment
                                    == entry->position_segment
                                && value.address.sub_key == physical_sub_key
                                && value.address.chunk_no == chunk;
                        });
                    REQUIRE(row != sink.rows.end());
                    REQUIRE(prepared->accept_chunk(
                        row->address,
                        reinterpret_cast<const char*>(row->bytes.data()),
                        row->bytes.size()).has_value());
                }
            }
        }
        REQUIRE(prepared->finish_load().has_value());
        segments.push_back(std::move(*prepared));
    }

    std::vector<PreparedSerializedHashRecord> records{
        {
            .hash = 7,
            .global_occurrence_count = 1,
            .positions_complete = true,
        },
        {
            .hash = 9,
            .global_occurrence_count = 1,
            .positions_complete = false,
        },
        {
            .hash = 100,
            .global_occurrence_count = 3,
            .positions_complete = true,
        },
    };
    constexpr std::uint64_t absent_pruned_records = 4096;
    records.reserve(records.size() + absent_pruned_records);
    for (std::uint64_t hash = 1000;
         hash < 1000 + absent_pruned_records;
         ++hash) {
        records.push_back(PreparedSerializedHashRecord{
            .hash = hash,
            .global_occurrence_count = 1,
            .positions_complete = false,
        });
    }
    auto prepared = PreparedSerializedGeneration::assemble(
        controller, std::move(segments), records);
    REQUIRE(prepared.has_value());
    REQUIRE(prepared->target_length() == span + 100u);
    const PreparedSerializedAssemblyProof& proof = prepared->assembly_proof();
    REQUIRE(proof.linear_merge_bound_holds());
    REQUIRE(proof.total_loaded_controls == 4);
    REQUIRE(proof.hash_record_count == records.size());
    REQUIRE(proof.merged_control_visits == 8);
    REQUIRE(proof.hash_record_visits == records.size() * 2u);
    REQUIRE(proof.heap_build_items == 4);
    REQUIRE(proof.heap_reinsertions == 4);
    REQUIRE(proof.emitted_spans == 3);
    REQUIRE(proof.hash_record_count > proof.merged_control_visits * 500u);

    const MinimizerLookupSpans across_segments = prepared->lookup_spans(100);
    REQUIRE(across_segments.well_formed());
    REQUIRE(across_segments.positions_complete());
    REQUIRE(across_segments.global_occurrence_count() == 3);
    REQUIRE(across_segments.segmented_positions().size() == 2);
    REQUIRE(std::ranges::equal(
        across_segments.segmented_positions()[0].positions,
        std::array{
            encoded_position(3, 0), encoded_position(9, 1)}));
    REQUIRE(std::ranges::equal(
        across_segments.segmented_positions()[1].positions,
        std::array{encoded_position(span, 1)}));
    REQUIRE(prepared->lookup_spans(100).segmented_positions().data()
        == across_segments.segmented_positions().data());

    const MinimizerLookupSpans pruned = prepared->lookup_spans(9);
    REQUIRE(pruned.well_formed());
    REQUIRE_FALSE(pruned.positions_complete());
    REQUIRE(pruned.global_occurrence_count() == 1);
    REQUIRE(pruned.segmented_positions().empty());
    const MinimizerLookupSpans absent = prepared->lookup_spans(999);
    REQUIRE(absent.well_formed());
    REQUIRE(absent.positions_complete());
    REQUIRE(absent.global_occurrence_count() == 0);
    REQUIRE(absent.segmented_positions().empty());

    PreparedSerializedGeneration moved{std::move(*prepared)};
    const MinimizerLookupSpans after_move = moved.lookup_spans(100);
    REQUIRE(after_move.well_formed());
    REQUIRE(after_move.global_occurrence_count() == 3);
    REQUIRE(std::ranges::equal(
        after_move.segmented_positions()[1].positions,
        std::array{encoded_position(span, 1)}));
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
        const auto position_sub_key =
            derive_minimizer_position_extent_sub_key(entry.sub_key);
        REQUIRE(position_sub_key.has_value());
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
                    .sub_key = *position_sub_key,
                    .chunk_no = chunk,
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
    SECTION("overlapping local offset") {
        auto controls = original_controls;
        test_write_le<std::uint32_t>(controls, 16u + 8u, 1u);
        expect_finish_error(
            std::move(controls), original_positions,
            MinimizerGenerationWireError::control_range);
    }
    SECTION("gapped local offset") {
        auto controls = original_controls;
        test_write_le<std::uint32_t>(controls, 16u + 8u, 3u);
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
        require_wire_error(
            prepared->accept_chunk(
                IndexChunkAddress{
                    .generation_id = generation->manifest.generation_id,
                    .position_segment = original_entry.position_segment,
                    .sub_key = original_entry.sub_key,
                    .chunk_no = 0,
                },
                reinterpret_cast<const char*>(controls.data()),
                controls.size()),
            MinimizerGenerationWireError::checksum);
    }
    SECTION("ordinal order") {
        const std::array duplicated_ordinals{
            original_entry.partition_id, original_entry.partition_id};
        require_wire_error(
            PreparedSerializedSegment::required_permit_bytes(
                generation->directory.encoded_bytes(),
                original_entry.position_segment,
                duplicated_ordinals,
                generation->directory.profile()),
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

    auto relabelled_manifest = generation->manifest;
    relabelled_manifest.producer_backend_id =
        MinimizerProducerBackendId::exact_simd;
    const auto relabelled_bytes =
        encode_minimizer_generation_manifest(relabelled_manifest);
    REQUIRE(relabelled_bytes.has_value());
    bad_head = generation->head;
    bad_head.manifest_checksum = crc64_ecma182(
        std::span<const std::byte>{*relabelled_bytes});
    // Even after repairing the standalone manifest CRC, metadata remains bound
    // to the physical producer through the independent compatibility checksum.
    require_wire_error(
        validate_minimizer_generation_metadata(
            bad_head, relabelled_manifest, generation->directory),
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

TEST_CASE("DOC0373 Format-5 derives exact H P C O boundaries and SoA offsets",
          "[indexing][minimizer-hashmap][wire][v5][boundary]") {
    using namespace bioseq::indexing;
    constexpr auto shape = [](std::uint16_t k, std::uint64_t span,
                              std::uint64_t cap) {
        return minimizer_shape_profile_v5(
            kLegacyMinimapV1SchemeIdentity,
            kMinimizerPersistedDna4AlphabetCode,
            k,
            span,
            cap);
    };
    STATIC_REQUIRE(shape(8u, 32'768u, 65'534u)->hash_width
        == MinimizerScalarWidth::u16);
    STATIC_REQUIRE(shape(9u, 32'768u, 65'534u)->hash_width
        == MinimizerScalarWidth::u32);
    STATIC_REQUIRE(shape(15u, 32'768u, 65'534u)->hash_width
        == MinimizerScalarWidth::u32);
    STATIC_REQUIRE(shape(16u, 32'768u, 65'534u)->hash_width
        == MinimizerScalarWidth::u32);
    STATIC_REQUIRE(shape(17u, 32'768u, 65'534u)->hash_width
        == MinimizerScalarWidth::u64);
    STATIC_REQUIRE(shape(28u, 32'768u, 65'534u).has_value());
    STATIC_REQUIRE_FALSE(shape(29u, 32'768u, 65'534u).has_value());
    STATIC_REQUIRE_FALSE(minimizer_shape_profile_v5(
        kLegacyMinimapV1SchemeIdentity, 1u, 15u, 32'768u, 65'534u)
        .has_value());
    STATIC_REQUIRE_FALSE(minimizer_shape_profile_v5(
        kLegacyMinimapV1SchemeIdentity, 4u, 15u, 32'768u, 65'534u)
        .has_value());
    STATIC_REQUIRE(minimizer_shape_profile_v5(
        kPaperNtHashU32R7SumV1SchemeIdentity,
        kMinimizerPersistedDna4AlphabetCode,
        15u, 32'768u, 65'534u)->hash_width
        == MinimizerScalarWidth::u32);
    STATIC_REQUIRE_FALSE(minimizer_shape_profile_v5(
        kPaperNtHashU32R7SumV1SchemeIdentity,
        kMinimizerPersistedDna4AlphabetCode,
        14u, 32'768u, 65'534u).has_value());
    STATIC_REQUIRE_FALSE(minimizer_shape_profile_v5(
        kPaperNtHashU32R7SumV1SchemeIdentity,
        kMinimizerPersistedDna4AlphabetCode,
        16u, 32'768u, 65'534u).has_value());

    constexpr auto scalar_expected =
        expected_minimizer_compatibility_for_backend(
            MinimizerGenerationWireFormat::grouped_shape_soa_segment_local,
            MinimizerSketchBackend::Scalar,
            kMinimizerPersistedDna4AlphabetCode,
            8u,
            15u,
            10u,
            65'534u);
    STATIC_REQUIRE(scalar_expected.has_value());
    constexpr auto exact_simd_expected =
        expected_minimizer_compatibility_for_backend(
            MinimizerGenerationWireFormat::grouped_shape_soa_segment_local,
            MinimizerSketchBackend::ExactSimd,
            kMinimizerPersistedDna4AlphabetCode,
            8u,
            15u,
            10u,
            65'534u);
    STATIC_REQUIRE(exact_simd_expected.has_value());
    STATIC_REQUIRE(shape(15u, 32'768u, 65'534u)->position_width
        == MinimizerScalarWidth::u16);
    STATIC_REQUIRE(shape(15u, 32'769u, 65'534u)->position_width
        == MinimizerScalarWidth::u32);
    STATIC_REQUIRE(shape(15u, std::uint64_t{1} << 31u, 65'534u)
        ->position_width == MinimizerScalarWidth::u32);
    STATIC_REQUIRE(shape(15u, (std::uint64_t{1} << 31u) + 1u, 65'534u)
        ->position_width == MinimizerScalarWidth::u64);
    STATIC_REQUIRE(shape(15u, 1u, 65'534u)->count_width
        == MinimizerScalarWidth::u16);
    STATIC_REQUIRE(shape(15u, 1u, 65'535u)->count_width
        == MinimizerScalarWidth::u32);
    STATIC_REQUIRE(shape(
        15u, 1u,
        static_cast<std::uint64_t>(
            (std::numeric_limits<std::uint32_t>::max)()) - 1u)->count_width
        == MinimizerScalarWidth::u32);
    STATIC_REQUIRE(shape(
        15u, 1u,
        (std::numeric_limits<std::uint32_t>::max)())->count_width
        == MinimizerScalarWidth::u64);
    STATIC_REQUIRE_FALSE(shape(
        15u, 1u,
        (std::numeric_limits<std::uint64_t>::max)()).has_value());
    STATIC_REQUIRE(minimizer_shape_v5_offset_width(65'535u)
        == MinimizerScalarWidth::u16);
    STATIC_REQUIRE(minimizer_shape_v5_offset_width(65'536u)
        == MinimizerScalarWidth::u32);
    STATIC_REQUIRE(minimizer_shape_v5_offset_width(
        (std::numeric_limits<std::uint32_t>::max)())
        == MinimizerScalarWidth::u32);
    STATIC_REQUIRE(minimizer_shape_v5_offset_width(
        static_cast<std::uint64_t>(
            (std::numeric_limits<std::uint32_t>::max)()) + 1u)
        == MinimizerScalarWidth::u64);

    constexpr auto layout = minimizer_shape_v5_control_layout(
        2u,
        MinimizerScalarWidth::u32,
        MinimizerScalarWidth::u16,
        MinimizerScalarWidth::u16);
    STATIC_REQUIRE(layout.has_value());
    STATIC_REQUIRE(layout->hash_offset == 0u);
    STATIC_REQUIRE(layout->prefix_offset == 8u);
    STATIC_REQUIRE(layout->count_offset == 16u);
    STATIC_REQUIRE(layout->encoded_bytes == 24u);
    STATIC_REQUIRE_FALSE(minimizer_shape_v5_control_layout(
        (std::numeric_limits<std::uint64_t>::max)(),
        MinimizerScalarWidth::u64,
        MinimizerScalarWidth::u64,
        MinimizerScalarWidth::u64).has_value());

    constexpr auto profile = minimizer_generation_payload_profile(
        MinimizerGenerationWireFormat::grouped_shape_soa_segment_local,
        kLegacyMinimapV1SchemeIdentity,
        kMinimizerPersistedDna4AlphabetCode,
        15u, 8u, 65'534u);
    STATIC_REQUIRE(profile.has_value());
    constexpr auto chunks = [](std::uint64_t bytes) {
        constexpr std::uint32_t extent_payload_bytes =
            minimizer_extent_payload_bytes(
                MinimizerGenerationWireFormat::
                    grouped_shape_soa_segment_local);
        return static_cast<std::uint32_t>(
            bytes / extent_payload_bytes
            + (bytes % extent_payload_bytes != 0u ? 1u : 0u));
    };
    constexpr auto make_wide_entry = [chunks](std::uint64_t positions) {
        const auto control_layout = minimizer_shape_v5_control_layout(
            1u,
            MinimizerScalarWidth::u32,
            minimizer_shape_v5_offset_width(positions),
            MinimizerScalarWidth::u16);
        return MinimizerBucketDirEntry{
            .partition_id = 0u,
            .position_segment = 0,
            .lo_hash = 1u,
            .hi_hash = 1u,
            .sub_key = 0,
            .control_first_chunk = 0u,
            .control_chunk_count = chunks(control_layout->encoded_bytes),
            .position_first_chunk = 0u,
            .position_chunk_count = chunks(positions * 2u),
            .control_element_count = 1u,
            .position_element_count = positions,
            .control_encoded_bytes = control_layout->encoded_bytes,
            .position_encoded_bytes = positions * 2u,
            .control_checksum = 1u,
            .position_checksum = 2u,
            .shape_offset_width = minimizer_shape_v5_offset_width(positions),
        };
    };
    constexpr auto o32_entry = make_wide_entry(65'536u);
    constexpr auto o32_bytes = encode_minimizer_bucket_dir_entry_v5(
        o32_entry, *profile);
    STATIC_REQUIRE(o32_bytes.has_value());
    STATIC_REQUIRE((*o32_bytes)[97] == std::byte{4});
    constexpr auto decoded_o32 = decode_minimizer_bucket_dir_entry(
        *o32_bytes, *profile);
    STATIC_REQUIRE(decoded_o32.has_value());
    STATIC_REQUIRE(*decoded_o32 == o32_entry);
    constexpr std::uint64_t o64_span =
        (std::uint64_t{1} << 31u) + 1u;
    constexpr auto o64_profile = minimizer_generation_payload_profile(
        MinimizerGenerationWireFormat::grouped_shape_soa_segment_local,
        kLegacyMinimapV1SchemeIdentity,
        kMinimizerPersistedDna4AlphabetCode,
        15u, o64_span, 65'534u);
    STATIC_REQUIRE(o64_profile.has_value());
    STATIC_REQUIRE(o64_profile->shape_v5->position_width
        == MinimizerScalarWidth::u64);
    constexpr auto o64_entry = [make_wide_entry, chunks] {
        auto value = make_wide_entry(
            static_cast<std::uint64_t>(
                (std::numeric_limits<std::uint32_t>::max)()) + 1u);
        value.position_encoded_bytes = value.position_element_count * 8u;
        value.position_chunk_count = chunks(value.position_encoded_bytes);
        return value;
    }();
    constexpr auto o64_bytes = encode_minimizer_bucket_dir_entry_v5(
        o64_entry, *o64_profile);
    STATIC_REQUIRE(o64_bytes.has_value());
    STATIC_REQUIRE((*o64_bytes)[97] == std::byte{8});
    constexpr auto decoded_o64 = decode_minimizer_bucket_dir_entry(
        *o64_bytes, *o64_profile);
    STATIC_REQUIRE(decoded_o64.has_value());
    STATIC_REQUIRE(*decoded_o64 == o64_entry);

    minimizer_generation_stream_detail::PlanPass<
        MinimizerGenerationWireFormat::grouped_shape_soa_segment_local>
        o64_plan{
            .span = o64_span,
            .target_length = o64_span,
            .profile = *o64_profile,
        };
    o64_plan.current_segment = 0u;
    o64_plan.current_hash = 1u;
    o64_plan.group_positions = o64_entry.position_element_count;
    REQUIRE(o64_plan.finish().has_value());
    REQUIRE(o64_plan.entries.size() == 1u);
    REQUIRE(o64_plan.entries.front().position_element_count
        == o64_entry.position_element_count);
    REQUIRE(o64_plan.entries.front().shape_offset_width
        == MinimizerScalarWidth::u64);
    auto planned_o64_expected = o64_entry;
    planned_o64_expected.control_checksum = 0u;
    planned_o64_expected.position_checksum = 0u;
    REQUIRE(o64_plan.entries.front() == planned_o64_expected);

    const auto o64_layout = minimizer_shape_v5_control_layout(
        o64_entry.control_element_count,
        o64_profile->shape_v5->hash_width,
        o64_entry.shape_offset_width,
        o64_profile->shape_v5->count_width);
    REQUIRE(o64_layout.has_value());
    std::vector<std::byte> o64_controls(
        static_cast<std::size_t>(o64_layout->encoded_bytes));
    test_write_le<std::uint32_t>(o64_controls, 0u, 1u);
    test_write_le<std::uint64_t>(
        o64_controls,
        static_cast<std::size_t>(o64_layout->prefix_offset),
        0u);
    test_write_le<std::uint64_t>(
        o64_controls,
        static_cast<std::size_t>(o64_layout->prefix_offset + 8u),
        o64_entry.position_element_count);
    test_write_le<std::uint16_t>(
        o64_controls,
        static_cast<std::size_t>(o64_layout->count_offset),
        65'535u);
    const auto decoded_o64_controls = decode_minimizer_shape_v5_controls(
        o64_controls, o64_entry, *o64_profile);
    REQUIRE(decoded_o64_controls.has_value());
    REQUIRE(decoded_o64_controls->size() == 1u);
    REQUIRE(decoded_o64_controls->front().position_begin == 0u);
    REQUIRE(decoded_o64_controls->front().position_end
        == o64_entry.position_element_count);
    REQUIRE(decoded_o64_controls->front().saturated_count == 65'535u);
}

TEST_CASE("DOC0373 Format-5 fixed records use exact 160 and 128 byte ABIs",
          "[indexing][minimizer-hashmap][wire][v5][golden]") {
    using namespace bioseq::indexing;
    constexpr auto b = [](unsigned value) {
        return static_cast<std::byte>(value);
    };
    constexpr auto shape = minimizer_shape_profile_v5(
        kLegacyMinimapV1SchemeIdentity,
        kMinimizerPersistedDna4AlphabetCode,
        15u, 8u, 10u);
    STATIC_REQUIRE(shape.has_value());
    constexpr auto profile = minimizer_generation_payload_profile(
        MinimizerGenerationWireFormat::grouped_shape_soa_segment_local,
        kLegacyMinimapV1SchemeIdentity,
        kMinimizerPersistedDna4AlphabetCode,
        15u, 8u, 10u);
    STATIC_REQUIRE(profile.has_value());
    constexpr auto manifest = [shape] {
        auto value = golden_generation_manifest();
        value.wire_format = MinimizerGenerationWireFormat::
            grouped_shape_soa_segment_local;
        value.alphabet_code = kMinimizerPersistedDna4AlphabetCode;
        value.kmer_size = 15u;
        value.gpu_abi_version = kMinimizerShapeV5GpuAbiVersion;
        value.shape_v5 = *shape;
        return value;
    }();
    constexpr auto encoded_manifest =
        encode_minimizer_generation_manifest_v5(manifest);
    STATIC_REQUIRE(encoded_manifest.has_value());
    STATIC_REQUIRE(encoded_manifest->size() == 160u);
    constexpr std::array<std::byte, 160> expected_manifest{
        b(0x47), b(0x4f), b(0x49), b(0x42), b(0x05), b(0x00), b(0xa0), b(0x00),
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
        b(0x88), b(0x77), b(0x66), b(0x55), b(0x00), b(0x00), b(0x44), b(0x33),
        b(0x0f), b(0x00), b(0x88), b(0x77), b(0x03), b(0x00), b(0x00), b(0x00),
        b(0x08), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x02), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x01), b(0x01),
        b(0x53), b(0x4f), b(0x49), b(0x42), b(0x01), b(0x00), b(0x20), b(0x00),
        b(0x04), b(0x02), b(0x02), b(0x01), b(0x01), b(0x01), b(0x02), b(0x00),
        b(0xff), b(0xff), b(0xff), b(0x3f), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x0a), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
    };
    STATIC_REQUIRE(*encoded_manifest == expected_manifest);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*encoded_manifest, 4) == 5u);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*encoded_manifest, 6) == 160u);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded_manifest, 108) == 3u);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded_manifest, 128)
        == kMinimizerShapeV5ExtensionMagic);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*encoded_manifest, 132) == 1u);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*encoded_manifest, 134) == 32u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_manifest, 136) == 4u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_manifest, 137) == 2u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_manifest, 138) == 2u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_manifest, 139) == 1u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_manifest, 140) == 1u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_manifest, 141) == 1u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_manifest, 142) == 2u);
    STATIC_REQUIRE((*encoded_manifest)[143] == std::byte{0});
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded_manifest, 144)
        == shape->max_hash);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded_manifest, 152) == 10u);
    constexpr auto decoded_manifest = decode_minimizer_generation_manifest(
        std::span<const std::byte>{*encoded_manifest});
    STATIC_REQUIRE(decoded_manifest.has_value());
    STATIC_REQUIRE(*decoded_manifest == manifest);

    constexpr MinimizerBucketDirEntry entry{
        .partition_id = 0,
        .position_segment = 0,
        .lo_hash = 1,
        .hi_hash = 2,
        .sub_key = 0,
        .control_chunk_count = 1,
        .position_chunk_count = 1,
        .control_element_count = 2,
        .position_element_count = 3,
        .control_encoded_bytes = 24,
        .position_encoded_bytes = 6,
        .control_checksum = 0x0102030405060708ULL,
        .position_checksum = 0x1112131415161718ULL,
        .shape_offset_width = MinimizerScalarWidth::u16,
    };
    constexpr auto encoded_entry = encode_minimizer_bucket_dir_entry_v5(
        entry, *profile);
    STATIC_REQUIRE(encoded_entry.has_value());
    STATIC_REQUIRE(encoded_entry->size() == 128u);
    constexpr std::array<std::byte, 128> expected_entry{
        b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x01), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x02), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x00), b(0x00), b(0x00), b(0x00), b(0x01), b(0x00), b(0x00), b(0x00),
        b(0x00), b(0x00), b(0x00), b(0x00), b(0x01), b(0x00), b(0x00), b(0x00),
        b(0x02), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x03), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x18), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x06), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x08), b(0x07), b(0x06), b(0x05), b(0x04), b(0x03), b(0x02), b(0x01),
        b(0x18), b(0x17), b(0x16), b(0x15), b(0x14), b(0x13), b(0x12), b(0x11),
        b(0x04), b(0x02), b(0x02), b(0x02), b(0x01), b(0x01), b(0x02), b(0x01),
        b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
    };
    STATIC_REQUIRE(*encoded_entry == expected_entry);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_entry, 96) == 4u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_entry, 97) == 2u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_entry, 98) == 2u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_entry, 99) == 2u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_entry, 100) == 1u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_entry, 101) == 1u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_entry, 102) == 2u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_entry, 103) == 1u);
    constexpr auto decoded_entry = decode_minimizer_bucket_dir_entry(
        std::span<const std::byte>{*encoded_entry}, *profile);
    STATIC_REQUIRE(decoded_entry.has_value());
    STATIC_REQUIRE(*decoded_entry == entry);
}

TEST_CASE("DOC0373 Format-5 fixed records reject every extension corruption class",
          "[indexing][minimizer-hashmap][wire][v5][corruption]") {
    using namespace bioseq::indexing;
    const auto shape = minimizer_shape_profile_v5(
        kLegacyMinimapV1SchemeIdentity,
        kMinimizerPersistedDna4AlphabetCode,
        15u, 8u, 10u);
    REQUIRE(shape.has_value());
    const auto profile = minimizer_generation_payload_profile(
        MinimizerGenerationWireFormat::grouped_shape_soa_segment_local,
        kLegacyMinimapV1SchemeIdentity,
        kMinimizerPersistedDna4AlphabetCode,
        15u, 8u, 10u);
    REQUIRE(profile.has_value());
    auto manifest = golden_generation_manifest();
    manifest.wire_format = MinimizerGenerationWireFormat::
        grouped_shape_soa_segment_local;
    manifest.alphabet_code = kMinimizerPersistedDna4AlphabetCode;
    manifest.kmer_size = 15u;
    manifest.gpu_abi_version = kMinimizerShapeV5GpuAbiVersion;
    manifest.shape_v5 = *shape;
    const auto encoded_manifest =
        encode_minimizer_generation_manifest_v5(manifest);
    REQUIRE(encoded_manifest.has_value());
    const auto good_manifest = *encoded_manifest;

    for (const std::size_t offset : {128u, 132u, 134u}) {
        INFO("Format-5 extension header offset " << offset);
        auto corrupt = good_manifest;
        corrupt[offset] ^= std::byte{1};
        require_wire_error(
            decode_minimizer_generation_manifest(corrupt),
            MinimizerGenerationWireError::version);
    }
    for (std::size_t offset = 136u; offset <= 142u; ++offset) {
        INFO("Format-5 shape field offset " << offset);
        auto corrupt = good_manifest;
        corrupt[offset] = std::byte{0};
        REQUIRE_FALSE(decode_minimizer_generation_manifest(corrupt));
    }
    {
        auto corrupt = good_manifest;
        corrupt[143] = std::byte{1};
        require_wire_error(
            decode_minimizer_generation_manifest(corrupt),
            MinimizerGenerationWireError::reserved_nonzero);
    }
    {
        INFO("Format-5 derived hash bound offset 144");
        auto corrupt = good_manifest;
        corrupt[144] ^= std::byte{1};
        REQUIRE_FALSE(decode_minimizer_generation_manifest(corrupt));
    }
    {
        /* max_occ_cap is a caller-selected compatibility parameter, not a
         * bound derivable from K/span.  A same-width value remains a valid
         * standalone manifest but must fail the reader's expected identity. */
        const auto expected = expected_minimizer_compatibility_for_backend(
            MinimizerGenerationWireFormat::
                grouped_shape_soa_segment_local,
            MinimizerSketchBackend::Scalar,
            manifest.alphabet_code,
            manifest.storage_unit_byte_width,
            manifest.kmer_size,
            manifest.window_size,
            shape->max_occ_cap);
        REQUIRE(expected.has_value());
        REQUIRE(validate_minimizer_generation_compatibility(
            manifest, *expected).has_value());
        auto changed_cap = good_manifest;
        changed_cap[152] ^= std::byte{1};
        const auto decoded = decode_minimizer_generation_manifest(changed_cap);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->shape_v5.has_value());
        REQUIRE(decoded->shape_v5->max_occ_cap
            == (shape->max_occ_cap ^ 1u));
        require_wire_error(
            validate_minimizer_generation_compatibility(
                *decoded, *expected),
            MinimizerGenerationWireError::compatibility_mismatch);
    }
    {
        /* Crossing the u16/u32 count-width boundary while retaining the
         * encoded u16 shape is intrinsically inconsistent and fails decode. */
        auto incompatible_cap_width = good_manifest;
        test_write_le<std::uint64_t>(
            incompatible_cap_width, 152u, 65'535u);
        require_wire_error(
            decode_minimizer_generation_manifest(incompatible_cap_width),
            MinimizerGenerationWireError::compatibility_mismatch);
    }

    constexpr MinimizerBucketDirEntry entry{
        .partition_id = 0u,
        .position_segment = 0,
        .lo_hash = 1u,
        .hi_hash = 2u,
        .sub_key = 0,
        .control_first_chunk = 0u,
        .control_chunk_count = 1u,
        .position_first_chunk = 0u,
        .position_chunk_count = 1u,
        .control_element_count = 2u,
        .position_element_count = 3u,
        .control_encoded_bytes = 24u,
        .position_encoded_bytes = 6u,
        .control_checksum = 1u,
        .position_checksum = 2u,
        .shape_offset_width = MinimizerScalarWidth::u16,
    };
    const auto encoded_entry = encode_minimizer_bucket_dir_entry_v5(
        entry, *profile);
    REQUIRE(encoded_entry.has_value());
    const auto good_entry = *encoded_entry;
    for (std::size_t offset = 96u; offset <= 103u; ++offset) {
        INFO("Format-5 directory shape offset " << offset);
        auto corrupt = good_entry;
        corrupt[offset] = std::byte{0};
        require_wire_error(
            decode_minimizer_bucket_dir_entry(corrupt, *profile),
            MinimizerGenerationWireError::compatibility_mismatch);
    }
    for (std::size_t offset = 104u; offset < 128u; ++offset) {
        INFO("Format-5 directory reserved offset " << offset);
        auto corrupt = good_entry;
        corrupt[offset] = std::byte{1};
        require_wire_error(
            decode_minimizer_bucket_dir_entry(corrupt, *profile),
            MinimizerGenerationWireError::reserved_nonzero);
    }

    auto relabeled_v5 = good_manifest;
    test_write_le<std::uint16_t>(relabeled_v5, 4u,
        static_cast<std::uint16_t>(
            MinimizerGenerationWireFormat::grouped_hash32_segment_local32));
    test_write_le<std::uint16_t>(relabeled_v5, 6u, 128u);
    require_wire_error(
        decode_minimizer_generation_manifest(relabeled_v5),
        MinimizerGenerationWireError::declared_size);
    const auto legacy_encoded = encode_minimizer_generation_manifest(
        golden_generation_manifest());
    REQUIRE(legacy_encoded.has_value());
    auto relabeled_legacy = *legacy_encoded;
    test_write_le<std::uint16_t>(relabeled_legacy, 4u,
        static_cast<std::uint16_t>(
            MinimizerGenerationWireFormat::grouped_shape_soa_segment_local));
    test_write_le<std::uint16_t>(relabeled_legacy, 6u, 160u);
    require_wire_error(
        decode_minimizer_generation_manifest(relabeled_legacy),
        MinimizerGenerationWireError::declared_size);
}

TEST_CASE("DOC0373 Format-5 emits exact SoA and prepared reader rejects corruption",
          "[indexing][minimizer-hashmap][wire][v5][reader][corruption]") {
    using namespace bioseq::indexing;
    STATIC_REQUIRE(std::is_move_constructible_v<
        PreparedShapeV5QueryReader>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<
        PreparedShapeV5QueryReader>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<
        PreparedShapeV5QueryReader>);
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {1u, (3u << 1u) | 1u},
        {1u, (9u << 1u)},
        // A new hash may restart below the previous hash's last coordinate.
        // This proves the reader's strict-order state resets per control.
        {2u, (1u << 1u) | 1u},
    };
    VectorReplayMinimizerProducer producer{minimizers};
    CollectingMinimizerRowSink sink;
    auto options = test_shape_v5_generation_options(100u, 10u);
    const auto generation = encode_minimizer_generation(
        producer, sink, options);
    REQUIRE(generation.has_value());
    REQUIRE(producer.scan_count == 3u);
    REQUIRE(generation->manifest.shape_v5.has_value());
    REQUIRE(generation->manifest_row.byte_count == 160u);
    REQUIRE(generation->directory.encoded_bytes().size()
        == generation->directory.entries().size() * 128u);
    REQUIRE(generation->directory.entries().size() == 1u);
    const auto& entry = generation->directory.entries().front();
    REQUIRE(entry.shape_offset_width == MinimizerScalarWidth::u16);
    REQUIRE(sink.rows.size()
        >= entry.control_chunk_count + entry.position_chunk_count);
    REQUIRE(sink.rows.front().address.sub_key == entry.sub_key);
    REQUIRE(sink.rows[entry.control_chunk_count].address.sub_key
        == *derive_minimizer_position_extent_sub_key(entry.sub_key));
    const auto control_bytes = collect_encoded_extent(sink.rows, entry, true);
    const auto position_bytes = collect_encoded_extent(sink.rows, entry, false);
    const auto controls = decode_minimizer_shape_v5_controls(
        control_bytes, entry, generation->directory.profile());
    REQUIRE(controls.has_value());
    REQUIRE(controls->size() == 2u);
    REQUIRE(((*controls)[0] == MinimizerShapeV5DecodedControl{
        .hash = 1u,
        .position_begin = 0u,
        .position_end = 2u,
        .saturated_count = 2u,
    }));
    REQUIRE((*controls)[1].saturated_count == 1u);

    bioseq::parallelism::MemoryAdmissionController admission;
    const std::array<std::uint64_t, 3> queries{1u, 2u, 999u};
    auto reader = PreparedShapeV5QueryReader::begin(
        admission,
        generation->head,
        generation->manifest,
        test_shape_v5_expected_compatibility(10u),
        generation->directory.encoded_bytes(),
        queries,
        10u);
    REQUIRE(reader.has_value());
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0) {
            REQUIRE(reader->accept(row_view(row)).has_value());
        }
    }
    REQUIRE(reader->finish().has_value());
    const auto hash1 = reader->lookup(1u);
    REQUIRE(hash1.global_occurrence_count == 2u);
    REQUIRE(hash1.positions_complete);
    REQUIRE((std::vector<std::uint64_t>{
        hash1.positions.begin(), hash1.positions.end()}
        == std::vector<std::uint64_t>{7u, 18u}));
    REQUIRE(reader->lookup(2u).positions.front() == 3u);
    REQUIRE(reader->lookup(999u).positions.empty());

    SECTION("padding byte") {
        auto corrupt = control_bytes;
        // H32*2 ends at 8; prefixes occupy [8,14), so [14,16) is padding.
        corrupt[14] = std::byte{1};
        require_wire_error(
            decode_minimizer_shape_v5_controls(
                corrupt, entry, generation->directory.profile()),
            MinimizerGenerationWireError::reserved_nonzero);
    }
    SECTION("terminal prefix") {
        auto corrupt = control_bytes;
        test_write_le<std::uint16_t>(corrupt, 12u, 2u);
        require_wire_error(
            decode_minimizer_shape_v5_controls(
                corrupt, entry, generation->directory.profile()),
            MinimizerGenerationWireError::control_range);
    }
    SECTION("interior prefix cannot overlap") {
        auto corrupt = control_bytes;
        test_write_le<std::uint16_t>(corrupt, 10u, 0u);
        require_wire_error(
            decode_minimizer_shape_v5_controls(
                corrupt, entry, generation->directory.profile()),
            MinimizerGenerationWireError::control_range);
    }
    SECTION("saturated count") {
        auto corrupt = control_bytes;
        test_write_le<std::uint16_t>(corrupt, 16u, 1u);
        require_wire_error(
            decode_minimizer_shape_v5_controls(
                corrupt, entry, generation->directory.profile()),
            MinimizerGenerationWireError::control_range);
    }
    SECTION("directory hash endpoints are exact") {
        auto corrupt_first = control_bytes;
        test_write_le<std::uint32_t>(corrupt_first, 0u, 0u);
        require_wire_error(
            decode_minimizer_shape_v5_controls(
                corrupt_first, entry, generation->directory.profile()),
            MinimizerGenerationWireError::hash_range);
        auto corrupt_last = control_bytes;
        test_write_le<std::uint32_t>(corrupt_last, 4u, 3u);
        require_wire_error(
            decode_minimizer_shape_v5_controls(
                corrupt_last, entry, generation->directory.profile()),
            MinimizerGenerationWireError::hash_range);
    }
    SECTION("positions are strictly ordered inside one hash") {
        auto corrupt = position_bytes;
        test_write_le<std::uint16_t>(
            corrupt, 2u, test_read_le<std::uint16_t>(corrupt, 0u));
        require_wire_error(
            decode_minimizer_shape_v5_positions(
                corrupt, *controls, entry,
                *minimizer_position_segment_core(
                    generation->manifest, 0u),
                generation->directory.profile()),
            MinimizerGenerationWireError::position_range);
    }
    SECTION("prepared reader retains strict order state") {
        auto corrupt = sink.rows[entry.control_chunk_count];
        test_write_le<std::uint16_t>(
            corrupt.bytes, 2u,
            test_read_le<std::uint16_t>(corrupt.bytes, 0u));
        recompute_row_checksum(corrupt);
        // Hash 1 is deliberately absent from the query union: every control
        // boundary must still enforce strict order without retaining the old
        // 16-byte route-and-boundary record beside it.
        const std::array<std::uint64_t, 1> unqueried_hash{2u};
        auto made = PreparedShapeV5QueryReader::begin(
            admission,
            generation->head,
            generation->manifest,
            test_shape_v5_expected_compatibility(10u),
            generation->directory.encoded_bytes(),
            unqueried_hash,
            10u);
        REQUIRE(made.has_value());
        for (std::size_t index = 0;
             index < entry.control_chunk_count; ++index) {
            REQUIRE(made->accept(row_view(sink.rows[index])).has_value());
        }
        require_wire_error(
            made->accept(row_view(corrupt)),
            MinimizerGenerationWireError::position_range);
    }
    SECTION("positions cannot escape their persisted segment") {
        auto corrupt = position_bytes;
        test_write_le<std::uint16_t>(corrupt, 0u, 200u);
        require_wire_error(
            decode_minimizer_shape_v5_positions(
                corrupt, *controls, entry,
                *minimizer_position_segment_core(
                    generation->manifest, 0u),
                generation->directory.profile()),
            MinimizerGenerationWireError::position_range);
    }
    SECTION("prepared reader checks extent checksum") {
        auto corrupt = sink.rows.front();
        corrupt.bytes.front() ^= std::byte{1};
        auto made = PreparedShapeV5QueryReader::begin(
            admission,
            generation->head,
            generation->manifest,
            test_shape_v5_expected_compatibility(10u),
            generation->directory.encoded_bytes(),
            queries,
            10u);
        REQUIRE(made.has_value());
        require_wire_error(
            made->accept(row_view(corrupt)),
            MinimizerGenerationWireError::checksum);
    }
    SECTION("prepared reader applies max-occ without losing global count") {
        auto made = PreparedShapeV5QueryReader::begin(
            admission,
            generation->head,
            generation->manifest,
            test_shape_v5_expected_compatibility(10u),
            generation->directory.encoded_bytes(),
            queries,
            1u);
        REQUIRE(made.has_value());
        for (const auto& row : sink.rows) {
            if (row.address.position_segment >= 0) {
                REQUIRE(made->accept(row_view(row)).has_value());
            }
        }
        REQUIRE(made->finish().has_value());
        const auto pruned = made->lookup(1u);
        REQUIRE(pruned.global_occurrence_count == 2u);
        REQUIRE_FALSE(pruned.positions_complete);
        REQUIRE(pruned.positions.empty());
    }
    SECTION("cross-format request") {
        auto v4 = make_grouped32_reader_fixture(minimizers, 100u);
        require_wire_error(
            PreparedShapeV5QueryReader::begin(
                admission,
                v4.generation.head,
                v4.generation.manifest,
                test_shape_v5_expected_compatibility(10u),
                v4.generation.directory.encoded_bytes(),
                queries,
                10u),
            MinimizerGenerationWireError::compatibility_mismatch);
    }
    SECTION("query occurrence limit cannot exceed persisted cap") {
        require_wire_error(
            PreparedShapeV5QueryReader::begin(
                admission,
                generation->head,
                generation->manifest,
                test_shape_v5_expected_compatibility(10u),
                generation->directory.encoded_bytes(),
                queries,
                11u),
            MinimizerGenerationWireError::compatibility_mismatch);
    }
    SECTION("query hashes obey the persisted H width") {
        const std::array<std::uint64_t, 1> too_wide{
            generation->directory.profile().max_hash + 1u};
        require_wire_error(
            PreparedShapeV5QueryReader::begin(
                admission,
                generation->head,
                generation->manifest,
                test_shape_v5_expected_compatibility(10u),
                generation->directory.encoded_bytes(),
                too_wide,
                10u),
            MinimizerGenerationWireError::hash_range);
    }
    SECTION("Format-4 prepared reader cannot relabel Format-5 bytes") {
        const std::array<std::uint64_t, 1> one_query{1u};
        require_wire_error(
            PreparedGrouped32QueryReader::begin(
                admission,
                generation->head,
                generation->manifest,
                test_grouped32_expected_compatibility(),
                MinimizerSketchBackend::ExactSimd,
                generation->directory.encoded_bytes(),
                one_query,
                10u),
            MinimizerGenerationWireError::compatibility_mismatch);
    }
}

TEST_CASE("DOC0373 Format-5 compact counts remain exact across u16",
          "[indexing][minimizer-hashmap][wire][v5][reader]"
          "[compact-state][max-occ]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t segment_span = 8u * 1024u * 1024u;
    constexpr std::uint64_t per_segment = 35'000u;
    constexpr std::uint64_t exact_count = 2u * per_segment;
    constexpr std::uint64_t persisted_cap = 100'000u;
    constexpr std::uint64_t query_hash = 0x1555'5555u;

    std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers;
    minimizers.reserve(exact_count);
    for (std::uint64_t index = 0u; index < per_segment; ++index) {
        minimizers.emplace_back(query_hash, (index + 1u) << 1u);
    }
    for (std::uint64_t index = 0u; index < per_segment; ++index) {
        minimizers.emplace_back(
            query_hash, (segment_span + index + 1u) << 1u);
    }

    VectorReplayMinimizerProducer producer{minimizers};
    CollectingMinimizerRowSink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink,
        test_shape_v5_generation_options(
            2u * segment_span, persisted_cap));
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest.position_count == exact_count);
    const std::array<std::uint64_t, 1> queries{query_hash};

    bioseq::parallelism::MemoryAdmissionController retained_admission;
    bioseq::parallelism::MemoryAdmissionController pruned_admission;
    const auto make_reader = [&](
            bioseq::parallelism::MemoryAdmissionController& admission,
            std::uint64_t max_occ) {
        auto reader = PreparedShapeV5QueryReader::begin(
            admission,
            generation->head,
            generation->manifest,
            test_shape_v5_expected_compatibility(persisted_cap),
            generation->directory.encoded_bytes(),
            queries,
            max_occ);
        REQUIRE(reader.has_value());
        for (const auto& row : sink.rows) {
            if (row.address.position_segment >= 0) {
                REQUIRE(reader->accept(row_view(row)).has_value());
            }
        }
        REQUIRE(reader->finish().has_value());
        return reader;
    };

    auto retained_reader = make_reader(retained_admission, persisted_cap);
    const auto retained = retained_reader->lookup(query_hash);
    REQUIRE(retained.global_occurrence_count == exact_count);
    REQUIRE(retained.positions_complete);
    REQUIRE(retained.positions.size() == exact_count);
    REQUIRE(retained.positions.front() == 2u);
    REQUIRE(retained.positions[per_segment]
        == (segment_span + 1u) << 1u);
    REQUIRE(retained.positions.back()
        == (segment_span + per_segment) << 1u);
    const auto retained_evidence = retained_reader->memory_evidence();
    REQUIRE(retained_evidence.query_record_size_bytes
        == sizeof(std::uint32_t));
    REQUIRE(retained_evidence.query_record_capacity_bytes
        == sizeof(std::uint32_t) + sizeof(std::uint64_t));

    auto pruned_reader = make_reader(
        pruned_admission,
        (std::numeric_limits<std::uint16_t>::max)());
    const auto pruned = pruned_reader->lookup(query_hash);
    REQUIRE(pruned.global_occurrence_count == exact_count);
    REQUIRE_FALSE(pruned.positions_complete);
    REQUIRE(pruned.positions.empty());
    REQUIRE(pruned_reader->memory_evidence().retained_position_count == 0u);
}

TEST_CASE("DOC0373 Format-5 cold reader admits retained positions once",
          "[indexing][minimizer-hashmap][wire][v5][reader][admission]"
          "[cold-load][memory-evidence]") {
    using namespace bioseq::indexing;
    // Model the report_83 shape: a wide query union whose mostly-unique hashes
    // each retain one target position.  The former per-QueryRecord vectors
    // performed two complete ownership walks and fresh OS memory probes for
    // each first append, making this shape quadratic in query-union size.
    constexpr std::uint64_t query_count = 2048u;
    constexpr std::uint64_t segment_span = 8u * 1024u * 1024u;
    constexpr std::uint64_t segment_count = 3u;
    constexpr auto retained_bp = [](std::uint64_t index) {
        return (index % segment_count) * segment_span
            + index / segment_count;
    };
    std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers;
    std::vector<std::uint64_t> queries;
    minimizers.reserve(query_count);
    queries.reserve(query_count);
    for (std::uint64_t index = 0; index < query_count; ++index) {
        // Odd multiplication is a permutation modulo 2^30 and spreads this
        // mostly-unique fixture across the same 30-bit domain as production
        // ntHash keys. This exercises actual high-bit radix occupancy rather
        // than putting every synthetic key in bucket zero.
        const std::uint64_t hash =
            ((index + 1u) * 2'654'435'761ULL) & ((1ULL << 30u) - 1u);
        minimizers.emplace_back(hash, retained_bp(index) << 1u);
        queries.push_back(hash);
    }

    VectorReplayMinimizerProducer producer{minimizers};
    CollectingMinimizerRowSink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink,
        test_shape_v5_generation_options(
            segment_count * segment_span, 10u));
    REQUIRE(generation.has_value());
    REQUIRE(generation->directory.entries().size() == segment_count);

    bioseq::parallelism::MemoryAdmissionController admission;
    auto reader = PreparedShapeV5QueryReader::begin(
        admission,
        generation->head,
        generation->manifest,
        test_shape_v5_expected_compatibility(10u),
        generation->directory.encoded_bytes(),
        queries,
        10u);
    REQUIRE(reader.has_value());
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0) {
            REQUIRE(reader->accept(row_view(row)).has_value());
        }
    }
    REQUIRE(reader->finish().has_value());

    const auto evidence = reader->memory_evidence();
    REQUIRE(evidence.global_position_storage_preparations == 1u);
    REQUIRE(evidence.global_position_storage_allocations == 1u);
    REQUIRE(evidence.position_append_growth_reservations == 0u);
    REQUIRE(evidence.retained_position_count == query_count);
    REQUIRE(evidence.retained_position_capacity == query_count);
    REQUIRE(evidence.query_record_size_bytes == sizeof(std::uint16_t));
    REQUIRE(evidence.query_record_count == query_count);
    REQUIRE(evidence.query_record_capacity >= query_count);
    REQUIRE(evidence.query_record_capacity_bytes
        == query_count
            * (sizeof(std::uint16_t) + sizeof(std::uint64_t)));
    REQUIRE(evidence.query_route_radix_bits == 9u);
    REQUIRE(evidence.query_route_radix_bucket_count == 512u);
    REQUIRE(evidence.query_route_radix_nonempty_bucket_count > 0u);
    REQUIRE(evidence.query_route_radix_nonempty_bucket_count
        <= evidence.query_route_radix_bucket_count);
    REQUIRE(evidence.query_route_radix_max_bucket_size <= 8u);
    REQUIRE(evidence.query_hash_capacity_bytes
        == query_count * sizeof(std::uint32_t)
            + (evidence.query_route_radix_bucket_count + 1u)
                * sizeof(std::uint32_t));
    REQUIRE(evidence.released_query_hash_capacity_bytes
        >= query_count * sizeof(std::uint64_t));
    REQUIRE(evidence.decoded_control_records == query_count);
    REQUIRE(evidence.query_route_control_lookups
        == evidence.decoded_control_records);
    REQUIRE(evidence.query_route_matches == query_count);
    const std::uint64_t global_lower_bound_probe_ceiling =
        evidence.query_route_control_lookups
        * (std::bit_width(query_count - 1u) + 1u);
    REQUIRE(evidence.query_route_hash_probes * 2u
        < global_lower_bound_probe_ceiling);
    REQUIRE(evidence.query_route_hash_probes
        <= evidence.query_route_control_lookups * 5u);
    REQUIRE(evidence.retained_selected_control_records == query_count);
    REQUIRE(evidence.selected_control_size_bytes == 8u);
    REQUIRE(evidence.retained_selected_control_capacity_bytes
        == query_count * (sizeof(std::uint64_t) + sizeof(std::uint32_t)));
    REQUIRE(evidence.current_selected_control_capacity_bytes == 0u);
    REQUIRE(evidence.global_position_capacity_bytes
        == evidence.retained_position_capacity * sizeof(std::uint64_t));
    REQUIRE(evidence.control_payload_capacity_bytes == 0u);
    REQUIRE(evidence.retained_layout_equation_holds());
    REQUIRE(evidence.owned_capacity_bytes == reader->admitted_bytes());
    // Full owner walks are now barrier-only: initialized, controls complete,
    // exact position image published, and final ready. Extent count no longer
    // appears in this equation.
    constexpr std::uint64_t expected_barrier_audits = 4u;
    REQUIRE(evidence.capacity_accounting_passes
        == expected_barrier_audits);
    REQUIRE_FALSE(evidence.saturated);
    REQUIRE(reader->lookup(queries.front()).positions.front()
        == retained_bp(0u) << 1u);
    REQUIRE(reader->lookup(queries.back()).positions.front()
        == retained_bp(query_count - 1u) << 1u);

    // Exercise the statement-level aggregate equation with a second reader
    // that selects only the first of the three control extents. Production
    // merges these per-reader counters, so the aggregate must retain the same
    // 7R + E + 4S shape even when readers select different extent counts.
    const std::array<std::uint64_t, 1> sparse_queries{queries.front()};
    const std::uint64_t sparse_selected_extent_count =
        static_cast<std::uint64_t>(std::ranges::count_if(
            generation->directory.entries(),
            [&](const MinimizerBucketDirEntry& entry) {
                return entry.lo_hash <= sparse_queries.front()
                    && sparse_queries.front() <= entry.hi_hash;
            }));
    REQUIRE(sparse_selected_extent_count > 0u);
    auto sparse_reader = PreparedShapeV5QueryReader::begin(
        admission,
        generation->head,
        generation->manifest,
        test_shape_v5_expected_compatibility(10u),
        generation->directory.encoded_bytes(),
        sparse_queries,
        10u);
    REQUIRE(sparse_reader.has_value());
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0) {
            REQUIRE(sparse_reader->accept(row_view(row)).has_value());
        }
    }
    REQUIRE(sparse_reader->finish().has_value());
    const auto sparse_evidence = sparse_reader->memory_evidence();
    REQUIRE(sparse_evidence.global_position_storage_preparations == 1u);
    REQUIRE(sparse_evidence.global_position_storage_allocations == 1u);
    REQUIRE(sparse_evidence.position_append_growth_reservations == 0u);
    REQUIRE(sparse_evidence.retained_position_count == 1u);
    REQUIRE(sparse_evidence.retained_position_capacity == 1u);
    REQUIRE(sparse_evidence.query_record_size_bytes
        == sizeof(std::uint16_t));
    REQUIRE(sparse_evidence.query_record_count == 1u);
    REQUIRE(sparse_evidence.query_route_radix_bits == 0u);
    REQUIRE(sparse_evidence.query_route_radix_bucket_count == 1u);
    REQUIRE(sparse_evidence.query_route_radix_nonempty_bucket_count == 1u);
    REQUIRE(sparse_evidence.query_route_radix_max_bucket_size == 1u);
    REQUIRE(sparse_evidence.query_hash_capacity_bytes
        == sizeof(std::uint32_t) + 2u * sizeof(std::uint32_t));
    REQUIRE(sparse_evidence.released_query_hash_capacity_bytes
        >= sizeof(std::uint64_t));
    REQUIRE(sparse_evidence.decoded_control_records > 1u);
    REQUIRE(sparse_evidence.query_route_control_lookups > 0u);
    REQUIRE(sparse_evidence.query_route_control_lookups
        <= sparse_evidence.decoded_control_records);
    REQUIRE(sparse_evidence.query_route_matches == 1u);
    REQUIRE(sparse_evidence.query_route_hash_probes
        <= sparse_evidence.query_route_control_lookups * 2u);
    REQUIRE(sparse_evidence.retained_selected_control_records
        == sparse_evidence.decoded_control_records);
    REQUIRE(sparse_evidence.retained_selected_control_capacity_bytes
        == sparse_evidence.decoded_control_records * sizeof(std::uint64_t)
            + sizeof(std::uint32_t) * 2u);
    REQUIRE(sparse_evidence.current_selected_control_capacity_bytes == 0u);
    REQUIRE(sparse_evidence.retained_layout_equation_holds());
    REQUIRE(sparse_evidence.owned_capacity_bytes
        == sparse_reader->admitted_bytes());
    REQUIRE(sparse_evidence.capacity_accounting_passes
        == expected_barrier_audits);
    REQUIRE_FALSE(sparse_evidence.saturated);

    // A selected query absent from the target still crosses the same global
    // controls/positions boundary, but exact P=0 must remain allocation-free.
    const std::array<std::uint64_t, 1> absent_queries{
        queries.front() ^ 1u};
    REQUIRE(std::ranges::find(queries, absent_queries.front())
        == queries.end());
    auto empty_reader = PreparedShapeV5QueryReader::begin(
        admission,
        generation->head,
        generation->manifest,
        test_shape_v5_expected_compatibility(10u),
        generation->directory.encoded_bytes(),
        absent_queries,
        10u);
    REQUIRE(empty_reader.has_value());
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0) {
            REQUIRE(empty_reader->accept(row_view(row)).has_value());
        }
    }
    REQUIRE(empty_reader->finish().has_value());
    const auto empty_evidence = empty_reader->memory_evidence();
    REQUIRE(empty_evidence.global_position_storage_preparations == 1u);
    REQUIRE(empty_evidence.global_position_storage_allocations == 0u);
    REQUIRE(empty_evidence.retained_position_count == 0u);
    REQUIRE(empty_evidence.retained_position_capacity == 0u);
    REQUIRE(empty_evidence.global_position_capacity_bytes == 0u);
    REQUIRE(empty_evidence.retained_layout_equation_holds());
    REQUIRE(empty_evidence.owned_capacity_bytes
        == empty_reader->admitted_bytes());
    REQUIRE(empty_reader->lookup(absent_queries.front()).positions.empty());

    constexpr std::uint64_t aggregate_sessions = 2u;
    const std::uint64_t aggregate_passes =
        evidence.capacity_accounting_passes
        + sparse_evidence.capacity_accounting_passes;
    REQUIRE(aggregate_passes
        == aggregate_sessions * expected_barrier_audits);
}

TEST_CASE("DOC0373 Format-5 admission failures name the exact owner substage",
          "[indexing][minimizer-hashmap][wire][v5][reader][admission]"
          "[diagnostics][failpoint]") {
    using namespace bioseq::indexing;
    STATIC_REQUIRE(static_cast<std::uint8_t>(
        MinimizerGenerationWireError::admission) == 28u);
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {1u, 2u},
        {2u, 4u},
    };
    VectorReplayMinimizerProducer producer{minimizers};
    CollectingMinimizerRowSink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink, test_shape_v5_generation_options(100u, 10u));
    REQUIRE(generation.has_value());
    const std::array<std::uint64_t, 2> queries{1u, 2u};
    std::vector<const CollectedMinimizerRow*> payload;
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0) payload.push_back(&row);
    }
    REQUIRE(payload.size() == 2u);

    using Substage = PreparedShapeV5QueryAdmissionSubstage;
    constexpr std::array begin_substages{
        Substage::identity_and_directory_reservation,
        Substage::query_hash_reservation,
        Substage::query_hash_allocation,
        Substage::query_hash_reconcile,
        Substage::query_route_reservation,
        Substage::query_route_allocation,
        Substage::query_route_reconcile,
    };
    constexpr std::array payload_substages{
        Substage::control_payload_reservation,
        Substage::control_payload_allocation,
        Substage::control_payload_reconcile,
        Substage::control_decode_reservation,
        Substage::control_decode_allocation,
        Substage::control_decode_reconcile,
        Substage::position_image_reservation,
        Substage::position_image_allocation,
        Substage::position_image_reconcile,
    };

    for (const Substage wanted : begin_substages) {
        INFO("begin admission substage "
            << prepared_shape_v5_query_admission_substage_name(wanted));
        bioseq::parallelism::MemoryAdmissionController admission;
        PreparedShapeV5QueryAdmissionEvidence evidence;
        const auto made = PreparedShapeV5QueryReader::begin(
            admission,
            generation->head,
            generation->manifest,
            test_shape_v5_expected_compatibility(10u),
            generation->directory.encoded_bytes(),
            queries,
            10u,
            &evidence,
            PreparedShapeV5QueryReaderTestOptions{
                .fail_admission_substage = wanted,
            });
        require_wire_error(
            made, MinimizerGenerationWireError::admission);
        REQUIRE(evidence.substage == wanted);
        REQUIRE(evidence.injected_failure);
        REQUIRE(prepared_shape_v5_query_admission_substage_name(wanted)
            != std::string_view{"unknown"});
        if (wanted != Substage::identity_and_directory_reservation) {
            REQUIRE(evidence.projected_bytes > 0u);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    for (const Substage wanted : payload_substages) {
        INFO("payload admission substage "
            << prepared_shape_v5_query_admission_substage_name(wanted));
        bioseq::parallelism::MemoryAdmissionController admission;
        PreparedShapeV5QueryAdmissionEvidence begin_evidence;
        auto made = PreparedShapeV5QueryReader::begin(
            admission,
            generation->head,
            generation->manifest,
            test_shape_v5_expected_compatibility(10u),
            generation->directory.encoded_bytes(),
            queries,
            10u,
            &begin_evidence,
            PreparedShapeV5QueryReaderTestOptions{
                .fail_admission_substage = wanted,
            });
        REQUIRE(made.has_value());
        REQUIRE(begin_evidence.substage == Substage::none);
        bool rejected = false;
        for (const CollectedMinimizerRow* row : payload) {
            const auto accepted = made->accept(row_view(*row));
            if (!accepted) {
                REQUIRE(accepted.error()
                    == MinimizerGenerationWireError::admission);
                rejected = true;
                break;
            }
        }
        REQUIRE(rejected);
        const auto evidence = made->admission_failure_evidence();
        REQUIRE(evidence.substage == wanted);
        REQUIRE(evidence.injected_failure);
        REQUIRE(evidence.projected_bytes > 0u);
        REQUIRE(evidence.query_record_count == queries.size());
        REQUIRE(evidence.query_record_capacity >= queries.size());
        REQUIRE(evidence.releasable_protocol_query_hash_bytes == 0u);
        if (wanted == Substage::position_image_reservation
            || wanted == Substage::position_image_allocation
            || wanted == Substage::position_image_reconcile) {
            REQUIRE(evidence.retained_position_count == queries.size());
            REQUIRE(evidence.retained_position_bytes
                == queries.size() * sizeof(std::uint64_t));
            REQUIRE(evidence.allocation_headroom_bytes == 0u);
            if (wanted == Substage::position_image_reconcile) {
                REQUIRE(evidence.projected_bytes
                    == evidence.owned_capacity_bytes);
            } else {
                const std::uint64_t compact_final_bytes =
                    queries.size() * sizeof(std::uint64_t)
                    + queries.size() * sizeof(std::uint8_t);
                REQUIRE(evidence.projected_bytes
                    == evidence.owned_capacity_bytes
                        + evidence.retained_position_bytes
                        + compact_final_bytes);
            }
        }
        REQUIRE(made->failed());
        REQUIRE(made->admitted_bytes() == 0u);
        const auto released = made->memory_evidence();
        REQUIRE(released.retained_position_count == 0u);
        REQUIRE(released.retained_position_capacity == 0u);
        REQUIRE(released.global_position_capacity_bytes == 0u);
        REQUIRE(released.retained_layout_equation_holds());
        REQUIRE(released.global_position_storage_allocations
            == (wanted == Substage::position_image_reconcile ? 1u : 0u));
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("real low budget retains the controller predicate snapshot") {
        bioseq::parallelism::MemoryAdmissionController admission;
        admission.set_index_memory_cap_bytes(1u);
        PreparedShapeV5QueryAdmissionEvidence evidence;
        const auto made = PreparedShapeV5QueryReader::begin(
            admission,
            generation->head,
            generation->manifest,
            test_shape_v5_expected_compatibility(10u),
            generation->directory.encoded_bytes(),
            queries,
            10u,
            &evidence);
        require_wire_error(
            made, MinimizerGenerationWireError::admission);
        REQUIRE(evidence.substage
            == Substage::identity_and_directory_reservation);
        REQUIRE_FALSE(evidence.injected_failure);
        REQUIRE(evidence.controller_failure_snapshot_present);
        REQUIRE(evidence.controller_reason
            != bioseq::parallelism::MemoryAdmissionFailureReason::none);
        REQUIRE(evidence.requested_growth_bytes > 1u);
        REQUIRE(evidence.controller_budget_ceiling_bytes <= 1u);
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("arithmetic overflow is not relabelled as admission") {
        const auto ordinary = plan_prepared_shape_v5_position_image(3u, 5u);
        REQUIRE(ordinary.has_value());
        REQUIRE(ordinary->retained_position_bytes == 24u);
        REQUIRE(ordinary->allocation_headroom_bytes == 0u);
        REQUIRE(ordinary->projected_bytes == 29u);
        constexpr std::uint64_t adversarial_count = 100'000'000u;
        const auto adversarial = plan_prepared_shape_v5_position_image(
            adversarial_count, 17u);
        REQUIRE(adversarial.has_value());
        REQUIRE(adversarial->retained_position_bytes == 800'000'000u);
        REQUIRE(adversarial->allocation_headroom_bytes == 0u);
        REQUIRE(adversarial->projected_bytes == 800'000'017u);
        REQUIRE(adversarial->projected_bytes - 17u
            < adversarial->retained_position_bytes
                * bioseq::parallelism::kVectorPreallocationHeadroomFactor);
        constexpr std::uint64_t adversarial_query_count = 100'000'000u;
        constexpr std::uint64_t prior_query_owned_bytes =
            adversarial_query_count * (64u + 2u * sizeof(std::uint64_t));
        constexpr std::uint64_t exact_query_state_and_hash_bytes =
            adversarial_query_count
                * (sizeof(std::uint32_t) + sizeof(std::uint8_t));
        // ceil(Q / 4) needs 25 high radix bits; the remaining five key bits
        // fit exactly in one bucket-implied suffix byte.
        constexpr std::uint64_t exact_query_radix_bytes =
            ((1ULL << 25u) + 1u) * sizeof(std::uint32_t);
        constexpr std::uint64_t exact_query_owned_bytes =
            exact_query_state_and_hash_bytes + exact_query_radix_bytes;
        STATIC_REQUIRE(prior_query_owned_bytes == 8'000'000'000u);
        STATIC_REQUIRE(exact_query_state_and_hash_bytes == 500'000'000u);
        STATIC_REQUIRE(exact_query_radix_bytes == 134'217'732u);
        STATIC_REQUIRE(exact_query_owned_bytes == 634'217'732u);
        STATIC_REQUIRE(prior_query_owned_bytes - exact_query_owned_bytes
            == 7'365'782'268u);
        require_wire_error(
            plan_prepared_shape_v5_position_image(
                (std::numeric_limits<std::uint64_t>::max)(), 0u),
            MinimizerGenerationWireError::arithmetic_overflow);
        require_wire_error(
            plan_prepared_shape_v5_position_image(
                1u,
                (std::numeric_limits<std::uint64_t>::max)() - 1u),
            MinimizerGenerationWireError::arithmetic_overflow);
    }
}

TEST_CASE("DOC0373 wire format selects its exact immutable row quantum",
          "[indexing][minimizer-hashmap][wire][v5][chunk-equation]") {
    using namespace bioseq::indexing;
    STATIC_REQUIRE(minimizer_extent_payload_bytes(
        MinimizerGenerationWireFormat::sorted_dense_global64) == 6144u);
    STATIC_REQUIRE(minimizer_extent_payload_bytes(
        MinimizerGenerationWireFormat::grouped_hash32_segment_local32)
        == 6144u);
    STATIC_REQUIRE(minimizer_extent_payload_bytes(
        MinimizerGenerationWireFormat::grouped_shape_soa_segment_local)
        == 8064u);

    constexpr std::uint64_t position_count = 5000u;
    constexpr std::uint64_t first_control_count = 8064u / 4u;
    constexpr std::uint64_t queried_control_count =
        position_count - first_control_count - 1u;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers;
    minimizers.reserve(position_count);
    for (std::uint64_t position = 0; position < position_count; ++position) {
        const std::uint64_t hash = position < first_control_count ? 6u
            : position < first_control_count + queried_control_count ? 7u
            : 8u;
        minimizers.emplace_back(hash, position << 1u);
    }
    VectorReplayMinimizerProducer producer{minimizers};
    CollectingMinimizerRowSink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink,
        test_shape_v5_generation_options(position_count + 1u, 65'534u));
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest_row.byte_count == 160u);
    REQUIRE(generation->manifest.position_segment_span_bp
        == kMinimizerPreferredSegmentSpanBp);
    REQUIRE(generation->manifest.shape_v5.has_value());
    REQUIRE(generation->manifest.shape_v5->position_width
        == MinimizerScalarWidth::u32);
    REQUIRE(generation->directory.entries().size() == 1u);
    const MinimizerBucketDirEntry& entry =
        generation->directory.entries().front();
    REQUIRE(entry.position_encoded_bytes == position_count * 4u);
    REQUIRE(entry.position_chunk_count == 3u);
    REQUIRE(sink.format5_payload_write_calls
            == static_cast<std::uint64_t>(entry.control_chunk_count)
                + entry.position_chunk_count);
    REQUIRE(sink.format5_row_crc_bytes
            == generation->manifest.payload_bytes);
    const auto position_sub_key =
        derive_minimizer_position_extent_sub_key(entry.sub_key);
    REQUIRE(position_sub_key.has_value());
    const auto first_position = std::ranges::find_if(
        sink.rows, [position_sub_key](const CollectedMinimizerRow& row) {
            return row.address.sub_key == *position_sub_key
                && row.address.chunk_no == 0u;
        });
    REQUIRE(first_position != sink.rows.end());
    REQUIRE(first_position->bytes.size() == 8064u);
    REQUIRE(sink.max_callback_bytes == 8064u);
    require_complete_generation_rows(*generation, sink.rows);

    // The first unqueried control ends exactly at the first position-row
    // boundary. Querying only the middle of three controls forces the sparse
    // route image and exercises its late ordinal on the following row.
    bioseq::parallelism::MemoryAdmissionController admission;
    const std::array<std::uint64_t, 1> query{7u};
    auto reader = PreparedShapeV5QueryReader::begin(
        admission,
        generation->head,
        generation->manifest,
        test_shape_v5_expected_compatibility(65'534u),
        generation->directory.encoded_bytes(),
        query,
        65'534u);
    REQUIRE(reader.has_value());
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0) {
            REQUIRE(reader->accept(row_view(row)).has_value());
        }
    }
    REQUIRE(reader->finish().has_value());
    const auto lookup = reader->lookup(7u);
    REQUIRE(lookup.positions.size() == queried_control_count);
    REQUIRE(lookup.positions.front() == first_control_count << 1u);
    REQUIRE(lookup.positions.back()
        == (first_control_count + queried_control_count - 1u) << 1u);
    const auto evidence = reader->memory_evidence();
    REQUIRE(evidence.decoded_control_records == 3u);
    REQUIRE(evidence.retained_selected_control_capacity_bytes
        == 3u * sizeof(std::uint64_t) + 2u * sizeof(std::uint32_t));
}

TEST_CASE("DOC0373 Format-5 narrows positions only with a narrow immutable span",
          "[indexing][minimizer-hashmap][wire][v5][position-width]"
          "[geometry][overflow-retry]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t narrow_span = std::uint64_t{1} << 15u;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {7u, 0u},
        {8u, narrow_span << 1u},
    };
    VectorReplayMinimizerProducer producer{minimizers};
    CollectingMinimizerRowSink sink;
    MinimizerGenerationFixedWidthLimits one_control_per_segment;
    one_control_per_segment.max_segment_control_count = 1u;

    const auto generation = encode_minimizer_generation(
        producer,
        sink,
        test_shape_v5_generation_options(narrow_span + 1u, 65'534u),
        one_control_per_segment);
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest.position_segment_span_bp == narrow_span);
    REQUIRE(generation->manifest.position_segment_count == 2u);
    REQUIRE(generation->manifest.gpu_abi_version
        == kMinimizerShapeV5GpuAbiVersion);
    REQUIRE(generation->manifest.shape_v5.has_value());
    REQUIRE(generation->manifest.shape_v5->position_width
        == MinimizerScalarWidth::u16);

    const auto canonical_profile = minimizer_generation_payload_profile(
        generation->manifest.wire_format,
        kLegacyMinimapV1SchemeIdentity,
        generation->manifest.alphabet_code,
        generation->manifest.kmer_size,
        generation->manifest.position_segment_span_bp,
        generation->manifest.shape_v5->max_occ_cap);
    REQUIRE(canonical_profile.has_value());
    REQUIRE(generation->directory.profile() == *canonical_profile);
    REQUIRE(generation->manifest.shape_v5
        == canonical_profile->shape_v5);

    REQUIRE(generation->directory.entries().size() == 2u);
    std::uint64_t position_count = 0u;
    std::uint64_t position_bytes = 0u;
    for (const MinimizerBucketDirEntry& entry :
         generation->directory.entries()) {
        REQUIRE(entry.position_element_count == 1u);
        REQUIRE(entry.position_encoded_bytes == 2u);
        REQUIRE(entry.position_chunk_count == 1u);
        position_count += entry.position_element_count;
        position_bytes += entry.position_encoded_bytes;
    }
    REQUIRE(position_count == generation->manifest.position_count);
    REQUIRE(position_bytes == generation->manifest.position_count * 2u);
    REQUIRE(sink.format5_row_crc_bytes
        == generation->manifest.payload_bytes);
    require_complete_generation_rows(*generation, sink.rows);
}

TEST_CASE("DOC0373 Format-5 authenticates global controls then global positions",
          "[indexing][minimizer-hashmap][wire][v5][physical-order][multi-segment]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t span = kMinimizerPreferredSegmentSpanBp;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {42u, 2u},
        {42u, 4u},
        {42u, ((span + 1u) << 1u)},
        {42u, ((span + 2u) << 1u)},
    };
    VectorReplayMinimizerProducer producer{minimizers};
    CollectingMinimizerRowSink sink;
    const auto generation = encode_minimizer_generation(
        producer,
        sink,
        test_shape_v5_generation_options(span + 100u, 10u));
    REQUIRE(generation.has_value());
    REQUIRE(generation->directory.entries().size() == 2u);
    for (std::size_t ordinal = 0;
         ordinal < generation->directory.entries().size(); ++ordinal) {
        const auto& entry = generation->directory.entries()[ordinal];
        REQUIRE(entry.partition_id == ordinal);
        REQUIRE(entry.sub_key == static_cast<index_sub_key_t>(ordinal));
        REQUIRE(entry.control_chunk_count == 1u);
        REQUIRE(entry.position_chunk_count == 1u);
    }
    std::vector<const CollectedMinimizerRow*> payload;
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0) payload.push_back(&row);
    }
    REQUIRE(payload.size() == 4u);
    REQUIRE(payload[0]->address.position_segment == 0);
    REQUIRE(payload[0]->address.sub_key == 0);
    REQUIRE(payload[1]->address.position_segment == 1);
    REQUIRE(payload[1]->address.sub_key == 1);
    REQUIRE(payload[2]->address.position_segment == 0);
    REQUIRE(payload[2]->address.sub_key
        == *derive_minimizer_position_extent_sub_key(0));
    REQUIRE(payload[3]->address.position_segment == 1);
    REQUIRE(payload[3]->address.sub_key
        == *derive_minimizer_position_extent_sub_key(1));
    require_complete_generation_rows(*generation, sink.rows);

    auto interleaved = make_generation_row_validator(*generation);
    REQUIRE(interleaved.accept(row_view(*payload[0])).has_value());
    require_wire_error(
        interleaved.accept(row_view(*payload[2])),
        MinimizerGenerationWireError::row_order);

    const std::array<std::uint64_t, 1> query{42u};
    SECTION("max-occ is target-global across both segments") {
        bioseq::parallelism::MemoryAdmissionController admission;
        auto pruned = PreparedShapeV5QueryReader::begin(
            admission,
            generation->head,
            generation->manifest,
            test_shape_v5_expected_compatibility(10u),
            generation->directory.encoded_bytes(),
            query,
            3u);
        REQUIRE(pruned.has_value());
        for (const CollectedMinimizerRow* row : payload) {
            REQUIRE(pruned->accept(row_view(*row)).has_value());
        }
        REQUIRE(pruned->finish().has_value());
        const auto found = pruned->lookup(42u);
        REQUIRE(found.global_occurrence_count == 4u);
        REQUIRE_FALSE(found.positions_complete);
        REQUIRE(found.positions.empty());
    }
    SECTION("boundary max-occ retains all cross-segment positions") {
        bioseq::parallelism::MemoryAdmissionController admission;
        auto retained = PreparedShapeV5QueryReader::begin(
            admission,
            generation->head,
            generation->manifest,
            test_shape_v5_expected_compatibility(10u),
            generation->directory.encoded_bytes(),
            query,
            4u);
        REQUIRE(retained.has_value());
        for (const CollectedMinimizerRow* row : payload) {
            REQUIRE(retained->accept(row_view(*row)).has_value());
        }
        REQUIRE(retained->finish().has_value());
        const auto found = retained->lookup(42u);
        REQUIRE(found.global_occurrence_count == 4u);
        REQUIRE(found.positions_complete);
        REQUIRE((std::vector<std::uint64_t>{
            found.positions.begin(), found.positions.end()}
            == std::vector<std::uint64_t>{
                2u,
                4u,
                (span + 1u) << 1u,
                (span + 2u) << 1u}));
    }

    auto invalid_entry = generation->directory.entries().front();
    invalid_entry.sub_key = 7;
    require_wire_error(
        encode_minimizer_bucket_dir_entry_v5(
            invalid_entry, generation->directory.profile()),
        MinimizerGenerationWireError::sub_key);
}

TEST_CASE("DOC0373 Format-5 reader releases admission on every terminal path",
          "[indexing][minimizer-hashmap][wire][v5][reader][admission][protocol]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {1u, 2u},
        {1u, 4u},
    };
    VectorReplayMinimizerProducer producer{minimizers};
    CollectingMinimizerRowSink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink, test_shape_v5_generation_options(100u, 10u));
    REQUIRE(generation.has_value());
    std::vector<const CollectedMinimizerRow*> payload;
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0) payload.push_back(&row);
    }
    REQUIRE(payload.size() == 2u);
    const std::array<std::uint64_t, 1> query{1u};
    const auto begin = [&](auto& admission) {
        return PreparedShapeV5QueryReader::begin(
            admission,
            generation->head,
            generation->manifest,
            test_shape_v5_expected_compatibility(10u),
            generation->directory.encoded_bytes(),
            query,
            10u);
    };

    SECTION("trusted receipt is move-only and V5 requires authenticated row CRC") {
        bioseq::parallelism::MemoryAdmissionController admission;
        {
            auto made = begin(admission);
            REQUIRE(made.has_value());
            auto first_view = row_view(*payload.front());
            auto bad_first_view = first_view;
            bad_first_view.row_checksum ^= 1u;
            require_wire_error(
                validate_minimizer_generation_row_receipt(bad_first_view),
                MinimizerGenerationWireError::checksum);
            auto validated_first =
                validate_minimizer_generation_row_receipt(first_view);
            REQUIRE(validated_first.has_value());
            auto first = std::move(*validated_first);
            REQUIRE(first.has_valid_authority());
            first_view.row_checksum ^= 1u;
            auto moved = std::move(first);
            REQUIRE_FALSE(first.has_valid_authority());
            REQUIRE(moved.has_valid_authority());
            REQUIRE(made->accept(moved).has_value());
            auto second = trusted_row_receipt(*payload.back());
            REQUIRE(made->accept(second).has_value());
            REQUIRE(made->finish().has_value());
            REQUIRE(made->lookup(1u).positions.size() == 2u);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("V5 trusted wrong address still poisons and releases") {
        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        auto wrong = *payload.front();
        ++wrong.address.sub_key;
        recompute_row_checksum(wrong);
        auto receipt = trusted_row_receipt(wrong);
        require_wire_error(
            made->accept(receipt), MinimizerGenerationWireError::row_order);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("ordinary V5 row with wrong row CRC remains rejected") {
        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        auto untrusted = row_view(*payload.front());
        untrusted.row_checksum ^= 1u;
        require_wire_error(
            made->accept(untrusted), MinimizerGenerationWireError::checksum);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("moved-from receipt fails runtime authority validation") {
        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        auto source = trusted_row_receipt(*payload.front());
        auto owner = std::move(source);
        REQUIRE(owner.has_valid_authority());
        REQUIRE_FALSE(source.has_valid_authority());
        require_wire_error(
            made->accept(source), MinimizerGenerationWireError::checksum);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("V5 trusted wrong order still poisons and releases") {
        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        auto receipt = trusted_row_receipt(*payload.back());
        require_wire_error(
            made->accept(receipt), MinimizerGenerationWireError::row_order);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("V5 trusted row cannot bypass extent CRC") {
        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        auto corrupt = *payload.front();
        corrupt.bytes.front() ^= std::byte{1};
        recompute_row_checksum(corrupt);
        auto receipt = trusted_row_receipt(corrupt);
        require_wire_error(
            made->accept(receipt), MinimizerGenerationWireError::checksum);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("move transfers the sole permit owner") {
        bioseq::parallelism::MemoryAdmissionController admission;
        {
            auto made = begin(admission);
            REQUIRE(made.has_value());
            auto source = std::move(*made);
            const std::uint64_t reserved = admission.reserved_bytes();
            REQUIRE(reserved > 0u);
            PreparedShapeV5QueryReader destination{std::move(source)};
            REQUIRE(admission.reserved_bytes() == reserved);
            for (const CollectedMinimizerRow* row : payload) {
                REQUIRE(destination.accept(row_view(*row)).has_value());
            }
            REQUIRE(destination.finish().has_value());
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("move after exact position allocation transfers array ownership") {
        bioseq::parallelism::MemoryAdmissionController admission;
        {
            auto made = begin(admission);
            REQUIRE(made.has_value());
            REQUIRE(made->accept(row_view(*payload.front())).has_value());
            const auto before_move = made->memory_evidence();
            REQUIRE(before_move.global_position_storage_preparations == 1u);
            REQUIRE(before_move.global_position_storage_allocations == 1u);
            REQUIRE(before_move.retained_position_count == 2u);
            REQUIRE(before_move.retained_position_capacity == 2u);
            REQUIRE(before_move.global_position_capacity_bytes
                == 2u * sizeof(std::uint64_t));
            REQUIRE(before_move.owned_capacity_bytes
                == made->admitted_bytes());
            const std::uint64_t reserved = admission.reserved_bytes();

            PreparedShapeV5QueryReader destination{std::move(*made)};
            REQUIRE(made->failed());
            REQUIRE(made->admitted_bytes() == 0u);
            REQUIRE(made->memory_evidence().retained_position_count == 0u);
            REQUIRE(made->memory_evidence().retained_position_capacity == 0u);
            REQUIRE(admission.reserved_bytes() == reserved);
            REQUIRE(destination.memory_evidence().retained_position_count
                == 2u);
            REQUIRE(destination.accept(row_view(*payload.back())).has_value());
            REQUIRE(destination.finish().has_value());
            const auto found = destination.lookup(1u);
            REQUIRE(found.positions.size() == 2u);
            REQUIRE(found.positions[0] == 2u);
            REQUIRE(found.positions[1] == 4u);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("incomplete finish poisons and releases immediately") {
        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        require_wire_error(
            made->finish(), MinimizerGenerationWireError::row_count);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("row corruption poisons and releases immediately") {
        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        auto corrupt = *payload.front();
        corrupt.bytes.front() ^= std::byte{1};
        require_wire_error(
            made->accept(row_view(corrupt)),
            MinimizerGenerationWireError::checksum);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("post-begin allocation denial rolls back and releases") {
        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        admission.set_index_memory_cap_bytes(admission.reserved_bytes());
        require_wire_error(
            made->accept(row_view(*payload.front())),
            MinimizerGenerationWireError::admission);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }
}

TEST_CASE("DOC0373 Format-5 real MPF and spilled MPS replays are identical",
          "[indexing][minimizer-hashmap][wire][v5][mpf][mps][parity]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 256u;
    constexpr std::uint64_t fragment_bp = fragment_units * 32u;
    using chunked = sequence_scanner_test_chunked<
        std::uint64_t, fragment_units>;
    using Index = MinimizersPerFragmentIndex<FakeFragmentStorage<>>;
    using MpsIndex = MinimizerSequenceMergedIndex<FakeFragmentStorage<>>;

    const auto verify = [&](std::vector<seq::dna4> data,
                            bool require_offset_widening) {
        range_t range(data);
        lazy_compact_sequence<dna4_u64, chunked> lazy{
            chunked::from_range(range)};
        auto mpf_producer = Index::make_generation_replay_producer(
            lazy, MinimizerSketchBackend::ExactSimd);
        CollectingMinimizerRowSink mpf_sink;
        auto mpf_options = test_shape_v5_generation_options(
            range.bp_size(), 65'534u);
        const auto mpf_generation = encode_minimizer_generation(
            mpf_producer, mpf_sink, mpf_options);
        REQUIRE(mpf_generation.has_value());
        REQUIRE(mpf_producer.scan_count() == 3u);
        const auto mpf_execution = mpf_producer.execution_evidence();
        INFO("Format-5 MPF evidence scans="
            << mpf_execution.producer_scan_count
            << " required_successful="
            << mpf_execution.required_successful_scan_count()
            << " geometry_retries="
            << mpf_execution.geometry_retry_count
            << " saturated=" << mpf_execution.saturated);
        REQUIRE(mpf_execution.generation_wire_format
            == MinimizerGenerationWireFormat::
                grouped_shape_soa_segment_local);
        REQUIRE(mpf_execution.required_successful_scan_count() == 3u);
        REQUIRE(mpf_execution.accepted());
        REQUIRE(mpf_execution.selected_backend
            == MinimizerSketchBackend::ExactSimd);
        REQUIRE(mpf_execution.scalar_core_runs == 0u);
        REQUIRE(mpf_execution.exact_simd_core_runs > 0u);
        REQUIRE(mpf_execution.paper_nthash_core_runs == 0u);
        REQUIRE(mpf_execution.vulkan_core_runs == 0u);
        REQUIRE(mpf_execution.exact_simd_proof_present);
        REQUIRE(mpf_execution.exact_simd_proof.accepted(true));

        FakeFragmentStorage<> spill_storage;
        MpsGenerationExecutionEvidence spill_execution;
        auto spill = MpsIndex::spill_generation_replay_runs_serial(
            spill_storage,
            lazy,
            MinimizerSketchBackend::ExactSimd,
            fragment_bp,
            kMinimizerPreferredSegmentSpanBp,
            &spill_execution);
        REQUIRE(spill.has_value());
        REQUIRE(spill->run_count() >= 2u);
        REQUIRE(spill_execution.spill_accepted());
        REQUIRE(spill_execution.selected_backend
            == MinimizerSketchBackend::ExactSimd);
        REQUIRE(spill_execution.scalar_core_runs == 0u);
        REQUIRE(spill_execution.exact_simd_core_runs > 0u);
        REQUIRE(spill_execution.paper_nthash_core_runs == 0u);
        REQUIRE(spill_execution.vulkan_core_runs == 0u);
        REQUIRE(spill_execution.exact_simd_proof_present);
        REQUIRE(spill_execution.exact_simd_proof.accepted(true));
        auto mps_producer = MpsIndex::make_generation_replay_producer(
            spill_storage, range.bp_size(), std::move(*spill));
        CollectingMinimizerRowSink mps_sink;
        auto mps_options = mpf_options;
        ++mps_options.generation_id;
        const auto mps_generation = encode_minimizer_generation(
            mps_producer, mps_sink, mps_options);
        REQUIRE(mps_generation.has_value());
        REQUIRE(mps_producer.scan_count() == 3u);
        REQUIRE(mps_producer.merge_pass_count() == 3u);
        const auto mps_replay_evidence =
            mps_producer.execution_evidence();
        REQUIRE(mps_replay_evidence.generation_wire_format
            == MinimizerGenerationWireFormat::
                grouped_shape_soa_segment_local);
        REQUIRE(mps_replay_evidence.required_successful_replay_count()
            == 3u);
        REQUIRE(mps_replay_evidence.replay_scan_count == 3u);
        REQUIRE(mps_replay_evidence.replay_completed_scan_count == 3u);
        REQUIRE(mps_replay_evidence.replay_failed_scan_count == 0u);
        REQUIRE(mps_replay_evidence.replay_reroute_pass_count == 0u);
        REQUIRE(mps_replay_evidence.replay_visited_event_count
            == 3u * mps_replay_evidence.spilled_event_count);
        require_encoded_generations_equal_normalized(
            *mpf_generation,
            mpf_sink.rows,
            *mps_generation,
            mps_sink.rows);
        REQUIRE(decode_generation_events(*mpf_generation, mpf_sink.rows)
            == decode_generation_events(*mps_generation, mps_sink.rows));
        require_complete_generation_rows(*mpf_generation, mpf_sink.rows);
        require_complete_generation_rows(*mps_generation, mps_sink.rows);

        if (require_offset_widening) {
            REQUIRE(std::ranges::any_of(
                mpf_generation->directory.entries(), [](const auto& entry) {
                    return entry.shape_offset_width
                        == MinimizerScalarWidth::u32;
                }));
            REQUIRE(std::ranges::any_of(
                mps_generation->directory.entries(), [](const auto& entry) {
                    return entry.shape_offset_width
                        == MinimizerScalarWidth::u32;
                }));
        }

        const auto fourth_replay = mps_producer.scan_sorted_events(
            mps_generation->manifest.position_segment_span_bp,
            CanonicalMinimizerEventReceiver{
                .context = nullptr,
                .emit_fn = [](
                        void*, const CanonicalMinimizerEvent&)
                        -> std::expected<
                            void, MinimizerGenerationWireError> {
                    return {};
                },
            });
        require_wire_error(
            fourth_replay, MinimizerGenerationWireError::segment_span);
        const auto terminal_evidence = mps_producer.execution_evidence();
        REQUIRE(terminal_evidence.replay_scan_count == 4u);
        REQUIRE(terminal_evidence.replay_completed_scan_count == 3u);
        REQUIRE(terminal_evidence.replay_failed_scan_count == 1u);
    };

    SECTION("random fragmented sequence") {
        verify(make_dna4(18'013u, 0x373352414E444F4DULL), false);
    }
    SECTION("long tandem sequence widens offsets") {
        std::vector<seq::dna4> tandem(70'100u);
        for (auto& base : tandem) base.assign_rank(0u);
        verify(std::move(tandem), true);
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

TEST_CASE("transient minimizer index preserves the XXL 30-bit radix-key domain",
          "[indexing][minimizer-hashmap][radix-key-width]") {
    using XxlMinimizer = MinimizerRepr<33u, 15u>;
    static_assert(XxlMinimizer::layout_t::hash_bits == 30u);
    static_assert(sizeof(XxlMinimizer) == sizeof(std::uint64_t));

    constexpr std::uint64_t low_key = 0x0000'0021u;
    constexpr std::uint64_t high_key = (std::uint64_t{1} << 30u) - 2u;
    std::vector<XxlMinimizer> sketches{
        XxlMinimizer{/*kmer*/ high_key, /*pos*/ 499'999'991u, /*strand*/ 1u},
        XxlMinimizer{/*kmer*/ low_key,  /*pos*/ 17u,          /*strand*/ 0u},
        XxlMinimizer{/*kmer*/ high_key, /*pos*/ 123'456'789u, /*strand*/ 0u},
        XxlMinimizer{/*kmer*/ low_key,  /*pos*/ 29u,          /*strand*/ 1u},
    };

    bioseq::indexing::MinimizerHashmapFragmentIndex index;
    index.build(sketches, /*target_length*/ 500'000'000u);

    auto sorted_positions = [&index](std::uint64_t key) {
        const auto hits = index.lookup(key);
        std::vector<std::uint64_t> result{hits.begin(), hits.end()};
        std::ranges::sort(result);
        return result;
    };
    REQUIRE((sorted_positions(low_key)
             == std::vector<std::uint64_t>{17u << 1u, (29u << 1u) | 1u}));
    REQUIRE((sorted_positions(high_key)
             == std::vector<std::uint64_t>{123'456'789u << 1u,
                                           (499'999'991u << 1u) | 1u}));
    REQUIRE(index.lookup((std::uint64_t{1} << 30u) - 3u).empty());
}

TEST_CASE("transient minimizer index preserves the 128-bit representation kmer domain",
          "[indexing][minimizer-hashmap][radix-key-width]") {
    using WideMinimizer = MinimizerRepr<28u, 30u>;
    static_assert(WideMinimizer::layout_t::hash_bits > 64u);
    static_assert(sizeof(WideMinimizer) == 2u * sizeof(std::uint64_t));
    static_assert(std::is_same_v<decltype(std::declval<const WideMinimizer&>().kmer()),
                                 std::uint64_t>);

    // The 128-bit representation's public kmer() word carries 56 significant
    // bits beside its eight-bit span field.  Exercise values above uint32 so
    // this test rejects any accidental narrowing of its radix key.
    constexpr std::uint64_t low_key = 0x0000'0001'2345'6789ULL;
    constexpr std::uint64_t high_key = 0x00FE'DCBA'9876'5432ULL;
    std::vector<WideMinimizer> sketches{
        WideMinimizer{/*kmer*/ high_key, /*pos*/ 200'000'003u,
                      /*strand*/ 1u, /*span*/ 30u},
        WideMinimizer{/*kmer*/ low_key,  /*pos*/ 19u,
                      /*strand*/ 0u, /*span*/ 30u},
        WideMinimizer{/*kmer*/ high_key, /*pos*/ 123'456'789u,
                      /*strand*/ 0u, /*span*/ 30u},
    };

    bioseq::indexing::MinimizerHashmapFragmentIndex index;
    index.build(sketches, /*target_length*/ 250'000'000u);

    REQUIRE(index.lookup(low_key).size() == 1u);
    REQUIRE(index.lookup(low_key).front() == (19u << 1u));
    const auto high_hits = index.lookup(high_key);
    REQUIRE(high_hits.size() == 2u);
    std::vector<std::uint64_t> sorted_high{high_hits.begin(), high_hits.end()};
    std::ranges::sort(sorted_high);
    REQUIRE((sorted_high
             == std::vector<std::uint64_t>{123'456'789u << 1u,
                                           (200'000'003u << 1u) | 1u}));
    REQUIRE(index.lookup(0x0000'0002'2345'6789ULL).empty());
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

struct Format6MemoryImage {
    struct ReadEvent {
        bioseq::indexing::MinimizerFormat6StreamKind kind{};
        std::uint64_t offset = 0;
        std::size_t byte_count = 0;

        [[nodiscard]] bool operator==(const ReadEvent&) const = default;
    };

    std::array<std::vector<std::byte>, 3> streams{};
    std::vector<bioseq::indexing::MinimizerFormat6StreamKind> append_order{};
    std::vector<ReadEvent> reads{};
    std::vector<std::vector<
        bioseq::indexing::MinimizerFormat6AuthorizedControlRange>>
        control_waves{};
    std::vector<std::vector<
        bioseq::indexing::MinimizerFormat6AuthorizedPostingRange>>
        posting_waves{};
    bioseq::parallelism::MemoryAdmissionController* cap_controller = nullptr;
    std::optional<std::uint64_t> cap_after_directory_read{};
    std::optional<std::uint64_t> cap_after_control_wave{};
    std::size_t size_calls = 0u;
    bool use_wave_callbacks = true;

    [[nodiscard]] static std::size_t index(
            bioseq::indexing::MinimizerFormat6StreamKind kind) {
        return static_cast<std::size_t>(kind) - 1u;
    }

    static bool append_callback(
            void* context,
            bioseq::indexing::MinimizerFormat6StreamKind kind,
            std::span<const std::byte> bytes) {
        auto& self = *static_cast<Format6MemoryImage*>(context);
        auto& stream = self.streams[index(kind)];
        stream.insert(stream.end(), bytes.begin(), bytes.end());
        self.append_order.push_back(kind);
        return true;
    }

    static bool read_callback(
            void* context,
            bioseq::indexing::MinimizerFormat6StreamKind kind,
            std::uint64_t offset,
            std::span<std::byte> output) {
        auto& self = *static_cast<Format6MemoryImage*>(context);
        const auto& stream = self.streams[index(kind)];
        if (offset > stream.size() || output.size() > stream.size() - offset) {
            return false;
        }
        std::copy_n(
            stream.begin() + static_cast<std::size_t>(offset),
            output.size(),
            output.begin());
        self.reads.push_back(ReadEvent{
            .kind = kind,
            .offset = offset,
            .byte_count = output.size(),
        });
        if (kind
                == bioseq::indexing::MinimizerFormat6StreamKind::directory
            && self.cap_controller != nullptr
            && self.cap_after_directory_read) {
            self.cap_controller->set_index_memory_cap_bytes(
                *self.cap_after_directory_read);
        }
        return true;
    }

    static std::uint64_t size_callback(
            void* context,
            bioseq::indexing::MinimizerFormat6StreamKind kind) {
        auto& self = *static_cast<Format6MemoryImage*>(context);
        ++self.size_calls;
        return self.streams[index(kind)].size();
    }

    static bool control_wave_callback(
            void* context,
            std::span<const
                bioseq::indexing::MinimizerFormat6AuthorizedControlRange>
                ranges,
            void* consumer_context,
            bioseq::indexing::MinimizerFormat6ConsumeControlBlockFn consume) {
        auto& self = *static_cast<Format6MemoryImage*>(context);
        self.control_waves.emplace_back(ranges.begin(), ranges.end());
        const auto& stream = self.streams[index(
            bioseq::indexing::MinimizerFormat6StreamKind::controls)];
        for (std::size_t ordinal = 0u; ordinal < ranges.size(); ++ordinal) {
            const auto& range = ranges[ordinal];
            if (range.control_byte_offset > stream.size()
                || range.byte_count
                    > stream.size() - range.control_byte_offset) {
                return false;
            }
            const auto bytes = std::span<const std::byte>{stream}.subspan(
                static_cast<std::size_t>(range.control_byte_offset),
                range.byte_count);
            self.reads.push_back(ReadEvent{
                .kind = bioseq::indexing::
                    MinimizerFormat6StreamKind::controls,
                .offset = range.control_byte_offset,
                .byte_count = range.byte_count,
            });
            if (!consume(
                    consumer_context,
                    static_cast<std::uint32_t>(ordinal),
                    bytes)) {
                return false;
            }
        }
        if (self.cap_controller != nullptr
            && self.cap_after_control_wave) {
            self.cap_controller->set_index_memory_cap_bytes(
                *self.cap_after_control_wave);
        }
        return true;
    }

    static bool posting_wave_callback(
            void* context,
            std::span<const
                bioseq::indexing::MinimizerFormat6AuthorizedPostingRange>
                ranges,
            void* consumer_context,
            bioseq::indexing::MinimizerFormat6ConsumePostingBytesFn consume) {
        auto& self = *static_cast<Format6MemoryImage*>(context);
        self.posting_waves.emplace_back(ranges.begin(), ranges.end());
        const auto& stream = self.streams[index(
            bioseq::indexing::MinimizerFormat6StreamKind::postings)];
        for (std::size_t ordinal = 0u; ordinal < ranges.size(); ++ordinal) {
            const auto& range = ranges[ordinal];
            if (range.posting_byte_offset > stream.size()
                || range.byte_count
                    > stream.size() - range.posting_byte_offset) {
                return false;
            }
            std::uint64_t relative = 0u;
            while (relative < range.byte_count) {
                const std::size_t byte_count = static_cast<std::size_t>(
                    std::min<std::uint64_t>(
                        bioseq::indexing::kMinimizerFormat6ReadSlabBytes,
                        range.byte_count - relative));
                const std::uint64_t absolute =
                    range.posting_byte_offset + relative;
                const auto bytes = std::span<const std::byte>{stream}.subspan(
                    static_cast<std::size_t>(absolute), byte_count);
                self.reads.push_back(ReadEvent{
                    .kind = bioseq::indexing::
                        MinimizerFormat6StreamKind::postings,
                    .offset = absolute,
                    .byte_count = byte_count,
                });
                if (!consume(
                        consumer_context,
                        static_cast<std::uint32_t>(ordinal),
                        relative,
                        bytes)) {
                    return false;
                }
                relative += byte_count;
            }
        }
        return true;
    }

    [[nodiscard]] bioseq::indexing::MinimizerFormat6LogicalStreamSinkRef
    sink() {
        return {
            .context = this,
            .append_bytes = &append_callback,
        };
    }

    [[nodiscard]] bioseq::indexing::MinimizerFormat6LogicalStreamSourceRef
    source() {
        return {
            .context = this,
            .read_bytes = &read_callback,
            .logical_size = &size_callback,
            .read_control_wave = use_wave_callbacks
                ? &control_wave_callback : nullptr,
            .read_posting_wave = use_wave_callbacks
                ? &posting_wave_callback : nullptr,
        };
    }

    [[nodiscard]] std::vector<ReadEvent> reads_of(
            bioseq::indexing::MinimizerFormat6StreamKind kind) const {
        std::vector<ReadEvent> selected;
        std::ranges::copy_if(
            reads, std::back_inserter(selected),
            [kind](const ReadEvent& event) { return event.kind == kind; });
        return selected;
    }
};

[[nodiscard]] bioseq::indexing::MinimizerFormat6GenerationOptions
format6_test_options(std::uint64_t target_length) {
    return bioseq::indexing::MinimizerFormat6GenerationOptions{
        .generation_id = 91u,
        .query_key = 17u,
        .source_epoch = 23u,
        .source_content_crc64 = 0x0123456789abcdefULL,
        .target_length = target_length,
        .compatibility = bioseq::indexing::MinimizerFormat6Compatibility{
            .hash_scheme_id = 0u,
            .hash_scheme_version = 0u,
            .alphabet_code = 0u,
            .kmer_size = 15u,
            .window_size = 10u,
            .hash_width_bytes = 4u,
            .stored_posting_width_bytes =
                static_cast<std::uint8_t>(
                    target_length <= (std::uint64_t{1} << 31u) ? 4u : 8u),
            .position_encoding_version = 1u,
            .hash_semantics_version = 1u,
            .count_semantics_version = 1u,
            .producer_backend_id = 1u,
            .producer_backend_version = 1u,
            .controls_per_block =
                bioseq::indexing::kMinimizerFormat6ControlsPerBlock,
            .hash_seed = 0u,
        },
    };
}

[[nodiscard]] bioseq::indexing::MinimizerFormat6ExpectedGeneration
format6_expected(
        const bioseq::indexing::MinimizerFormat6GenerationOptions& options) {
    return {
        .generation_id = options.generation_id,
        .query_key = options.query_key,
        .source_epoch = options.source_epoch,
        .source_content_crc64 = options.source_content_crc64,
        .target_length = options.target_length,
        .compatibility = options.compatibility,
    };
}

[[nodiscard]] bioseq::indexing::CanonicalQueryHashView format6_query_view(
        std::span<const std::uint32_t> hashes,
        const bioseq::indexing::MinimizerFormat6Compatibility& compatibility) {
    return {
        .values32 = hashes,
        .width_bytes = 4u,
        .storage_width_bytes = 4u,
        .compatibility_digest =
            bioseq::indexing::minimizer_format6_compatibility_digest(
                compatibility),
    };
}

[[nodiscard]] std::string format6_hex(std::span<const std::byte> bytes) {
    constexpr std::string_view digits{"0123456789abcdef"};
    std::string output;
    output.resize(bytes.size() * 2u);
    for (std::size_t index = 0u; index < bytes.size(); ++index) {
        const std::uint8_t value = std::to_integer<std::uint8_t>(bytes[index]);
        output[index * 2u] = digits[value >> 4u];
        output[index * 2u + 1u] = digits[value & 0x0fu];
    }
    return output;
}

void recompute_format6_control_crc(std::vector<std::byte>& bytes) {
    using bioseq::indexing::Crc64Ecma182;
    REQUIRE(bytes.size()
        >= bioseq::indexing::kMinimizerFormat6ControlHeaderBytes);
    Crc64Ecma182 crc;
    crc.update(std::span<const std::byte>{bytes}.first(48u));
    crc.update(std::span<const std::byte>{bytes}.subspan(
        bioseq::indexing::kMinimizerFormat6ControlHeaderBytes));
    test_write_le<std::uint64_t>(std::span<std::byte>{bytes}, 48u,
        crc.checksum());
}

struct Format6Fixture {
    Format6MemoryImage image{};
    bioseq::indexing::MinimizerFormat6GenerationOptions options{};
    bioseq::indexing::MinimizerFormat6EncodedMetadata metadata{};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{};
};

[[nodiscard]] Format6Fixture make_format6_small_fixture() {
    using namespace bioseq::indexing;
    Format6Fixture fixture;
    fixture.options = format6_test_options(100u);
    MinimizerFormat6LogicalEncoder encoder{
        fixture.options, fixture.image.sink()};
    const std::array<std::uint64_t, 2> first{2u, 4u};
    const std::array<std::uint64_t, 1> middle{10u};
    const std::array<std::uint64_t, 2> last{20u, 21u};
    REQUIRE(encoder.append_hash(10u, first).has_value());
    REQUIRE(encoder.append_hash(20u, middle).has_value());
    REQUIRE(encoder.append_hash(30u, last).has_value());
    auto metadata = encoder.finish();
    REQUIRE(metadata.has_value());
    fixture.metadata = *metadata;
    fixture.pairs = {
        {10u, 2u}, {10u, 4u},
        {20u, 10u},
        {30u, 20u}, {30u, 21u},
    };
    return fixture;
}

TEST_CASE("Format-6 exact wire codecs preserve Format-5 ABI",
          "[indexing][minimizer-hashmap][wire][v6][selective]") {
    using namespace bioseq::indexing;
    STATIC_REQUIRE(MinimizerGenerationHead::encoded_bytes == 64u);
    STATIC_REQUIRE(MinimizerGenerationManifest::format5_encoded_bytes == 160u);
    STATIC_REQUIRE(kMinimizerFormat6HeadBytes == 224u);
    STATIC_REQUIRE(kMinimizerFormat6ManifestBytes == 336u);
    STATIC_REQUIRE(kMinimizerFormat6DirectoryEntryBytes == 80u);
    STATIC_REQUIRE(sizeof(ExactGlobalCountException) == 16u);
    STATIC_REQUIRE(sizeof(PostingRequest) == 24u);
    STATIC_REQUIRE(sizeof(MinimizerFormat6AuthorizedControlRange) == 24u);
    STATIC_REQUIRE(sizeof(MinimizerFormat6AuthorizedPostingRange) == 48u);

    auto fixture = make_format6_small_fixture();
    REQUIRE(fixture.metadata.encoded_head.size() == 224u);
    REQUIRE(fixture.metadata.encoded_manifest.size() == 336u);
    REQUIRE(fixture.image.append_order
        == std::vector<MinimizerFormat6StreamKind>{
            MinimizerFormat6StreamKind::postings,
            MinimizerFormat6StreamKind::controls,
            MinimizerFormat6StreamKind::directory});
    REQUIRE(std::to_integer<char>(fixture.metadata.encoded_head[0]) == 'B');
    REQUIRE(std::to_integer<char>(fixture.metadata.encoded_head[6]) == '6');
    REQUIRE(std::to_integer<char>(fixture.metadata.encoded_manifest[0]) == 'B');
    REQUIRE(std::to_integer<char>(fixture.metadata.encoded_manifest[6]) == '6');

    const auto decoded_head = decode_minimizer_format6_head(
        fixture.metadata.encoded_head);
    const auto decoded_manifest = decode_minimizer_format6_manifest(
        fixture.metadata.encoded_manifest);
    REQUIRE(decoded_head.has_value());
    REQUIRE(decoded_manifest.has_value());
    REQUIRE(*decoded_head == fixture.metadata.head);
    REQUIRE(*decoded_manifest == fixture.metadata.manifest);
    const std::array<std::byte, 16> expected_source_identity{
        std::byte{0x9e}, std::byte{0xe7}, std::byte{0x39}, std::byte{0xaa},
        std::byte{0x80}, std::byte{0xf7}, std::byte{0x43}, std::byte{0xdc},
        std::byte{0x1f}, std::byte{0xf3}, std::byte{0x35}, std::byte{0x3e},
        std::byte{0xa5}, std::byte{0x04}, std::byte{0xe3}, std::byte{0xaa},
    };
    const std::array<std::byte, 32> expected_compatibility_digest{
        std::byte{0x18}, std::byte{0xdb}, std::byte{0x2e}, std::byte{0x20},
        std::byte{0x33}, std::byte{0x2c}, std::byte{0xeb}, std::byte{0x74},
        std::byte{0xbe}, std::byte{0xb2}, std::byte{0x4d}, std::byte{0xda},
        std::byte{0x8e}, std::byte{0xf6}, std::byte{0xa5}, std::byte{0xd2},
        std::byte{0x82}, std::byte{0x17}, std::byte{0x34}, std::byte{0x18},
        std::byte{0x50}, std::byte{0xbe}, std::byte{0x23}, std::byte{0x94},
        std::byte{0x7f}, std::byte{0xd8}, std::byte{0x46}, std::byte{0x35},
        std::byte{0x26}, std::byte{0x2e}, std::byte{0xa1}, std::byte{0xfb},
    };
    REQUIRE(decoded_head->source_identity == expected_source_identity);
    REQUIRE(decoded_head->compatibility_digest
        == expected_compatibility_digest);
    REQUIRE(decoded_manifest->unique_hash_count == 3u);
    REQUIRE(decoded_manifest->total_posting_count == 5u);
    REQUIRE(decoded_manifest->streams[0].logical_bytes == 80u);
    REQUIRE(decoded_manifest->streams[2].logical_bytes == 20u);
    REQUIRE(decoded_manifest->streams[0].first_row_ordinal == 0u);
    REQUIRE(decoded_manifest->streams[1].first_row_ordinal == 1u);
    REQUIRE(decoded_manifest->streams[2].first_row_ordinal == 2u);
    REQUIRE(decoded_manifest->streams[0].stream_crc64
        == 0xd9cbbef0ecb8f6a4ULL);
    REQUIRE(decoded_manifest->streams[1].stream_crc64
        == 0xbfd39c3c3e488c19ULL);
    REQUIRE(decoded_manifest->streams[2].stream_crc64
        == 0x5cf59f98c6058b27ULL);
    REQUIRE(decoded_manifest->generation_crc64
        == 0xedd9cd57f12d5423ULL);
    REQUIRE(decoded_head->manifest_payload_crc64
        == 0xe8bab5bdeba98b87ULL);
    REQUIRE(decoded_head->manifest_address_bound_row_crc64
        == 0x8fdbf93d04832856ULL);
    REQUIRE(decoded_head->head_crc64 == 0xdc5d0729f8d20fc8ULL);

    // These are independent full-byte wire images, not values round-tripped
    // through the implementation under test. They pin every reserved byte,
    // descriptor ordinal, checksum field, and packed control tail.
    REQUIRE(format6_hex(fixture.metadata.encoded_head)
        == "42534551474836000600e000000000005b000000000000001100000000000000"
           "17000000000000009ee739aa80f743dc1ff3353ea504e3aaefcdab8967452301"
           "18db2e20332ceb74beb24dda8ef6a5d28217341850be23947fd84635262ea1fb"
           "0000000000000000000000000000000000000000000000005001000001000000"
           "562883043df9db8f878ba9ebbdb5bae8c80fd2f829075ddc0000000000000000"
           "0000000000000000000000000000000000000000000000000000000000000000"
           "0000000000000000000000000000000000000000000000000000000000000000");
    REQUIRE(format6_hex(fixture.metadata.encoded_manifest)
        == "425345514e4c36000600c000000000005b000000000000001700000000000000"
           "9ee739aa80f743dc1ff3353ea504e3aaefcdab89674523016400000000000000"
           "0300000000000000050000000000000000000000000000000f000a0004040003"
           "0100010001000100010000000010000018db2e20332ceb74beb24dda8ef6a5d2"
           "8217341850be23947fd84635262ea1fba4f6b8ecf0becbd923542df157cdd9ed"
           "4800000000000000140000000000000001000000000000000000000000000000"
           "0100010050000000010000000000000050000000000000000100000000000000"
           "a4f6b8ecf0becbd9000000000000000002000100000000000300000000000000"
           "48000000000000000100000000000000198c483e3c9cd3bf0100000000000000"
           "0300010004000000050000000000000014000000000000000100000000000000"
           "278b05c6989ff55c0200000000000000");
    REQUIRE(format6_hex(fixture.image.streams[0])
        == "0a000000000000001e0000000000000000000000000000000000000000000000"
           "4800000003000000000000000000000000000000000000001400000000000000"
           "050000000000000099128aad8d85ba2d");
    REQUIRE(format6_hex(fixture.image.streams[1])
        == "0a000000000000000300000005100000050000000000000000000000000000"
           "0002000000060000000000000000000000dc72c00206f5e8fd278b05c6989f"
           "f55c4051020001000200");
    REQUIRE(format6_hex(fixture.image.streams[2])
        == "02000000040000000a0000001400000015000000");

    SECTION("borrowed canonical lower_bound is hard-bounded by its radix") {
        const std::array<std::uint32_t, 5> hashes{
            1u, 2u, 0x0001'0000u, 0x0001'0001u, 0x0002'0000u};
        CanonicalQueryHashView view{
            .values32 = std::span<const std::uint32_t>{hashes},
            .width_bytes = 4u,
            .storage_width_bytes = 4u,
        };
        REQUIRE(view.lower_bound(2u, 4u, 0x0002'0000u) == 4u);
        REQUIRE(view.lower_bound(2u, 4u, 0x0001'0001u) == 3u);
    }

    SECTION("head and manifest corruption fail closed") {
        auto bad_head = fixture.metadata.encoded_head;
        bad_head[208] ^= std::byte{1};
        REQUIRE_FALSE(decode_minimizer_format6_head(bad_head).has_value());

        auto bad_manifest = fixture.metadata.encoded_manifest;
        bad_manifest[198] ^= std::byte{1};
        REQUIRE_FALSE(
            decode_minimizer_format6_manifest(bad_manifest).has_value());
    }
}

TEST_CASE("Format-6 control codec pins zero delta and exact count exceptions",
          "[indexing][minimizer-hashmap][wire][v6][selective]") {
    using namespace bioseq::indexing;
    const std::array<std::uint64_t, 1> hashes{42u};
    const std::array<std::uint64_t, 1> counts{65'535u};
    auto encoded = encode_minimizer_format6_control_block(
        hashes, counts, 0u, 0x9988776655443322ULL, 4u, 4u);
    REQUIRE(encoded.has_value());
    REQUIRE(encoded->size() == 64u + 2u + 12u);
    auto decoded = decode_minimizer_format6_control_block(*encoded, 4u, 4u);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->header.hash_delta_bit_width == 0u);
    REQUIRE(decoded->header.hash_bytes == 0u);
    REQUIRE(decoded->header.count_bytes == 2u);
    REQUIRE(decoded->header.exception_bytes == 12u);
    const std::array<std::byte, 8> expected_control_crc_le{
        std::byte{0x4b}, std::byte{0xac}, std::byte{0x08}, std::byte{0xa1},
        std::byte{0x07}, std::byte{0x6a}, std::byte{0x71}, std::byte{0x29},
    };
    REQUIRE(std::ranges::equal(
        std::span<const std::byte>{*encoded}.subspan(48u, 8u),
        expected_control_crc_le));
    REQUIRE(decoded->hashes == std::vector<std::uint64_t>{42u});
    REQUIRE(decoded->exact_counts == std::vector<std::uint64_t>{65'535u});

    SECTION("payload checksum") {
        auto corrupt = *encoded;
        corrupt[65] ^= std::byte{1};
        REQUIRE_FALSE(
            decode_minimizer_format6_control_block(corrupt, 4u, 4u)
                .has_value());
    }

    SECTION("noncanonical delta width") {
        const std::array<std::uint64_t, 2> two_hashes{42u, 43u};
        const std::array<std::uint64_t, 2> two_counts{1u, 1u};
        auto canonical = encode_minimizer_format6_control_block(
            two_hashes, two_counts, 0u, 0u, 4u, 4u);
        REQUIRE(canonical.has_value());
        auto corrupt = *canonical;
        REQUIRE(std::to_integer<std::uint8_t>(corrupt[12]) == 1u);
        corrupt[12] = std::byte{2};
        corrupt[64] = std::byte{4};
        recompute_format6_control_crc(corrupt);
        const auto rejected = decode_minimizer_format6_control_block(
            corrupt, 4u, 4u);
        REQUIRE_FALSE(rejected.has_value());
        REQUIRE(rejected.error() == MinimizerFormat6Error::control);
    }

    SECTION("reserved flags and packed tail bits") {
        const std::array<std::uint64_t, 2> two_hashes{42u, 43u};
        const std::array<std::uint64_t, 2> two_counts{1u, 1u};
        auto canonical = encode_minimizer_format6_control_block(
            two_hashes, two_counts, 0u, 0u, 4u, 4u);
        REQUIRE(canonical.has_value());

        auto reserved = *canonical;
        reserved[14] = std::byte{1};
        recompute_format6_control_crc(reserved);
        REQUIRE_FALSE(decode_minimizer_format6_control_block(
            reserved, 4u, 4u));

        auto tail = *canonical;
        tail[64] |= std::byte{0x80};
        recompute_format6_control_crc(tail);
        const auto rejected = decode_minimizer_format6_control_block(
            tail, 4u, 4u);
        REQUIRE_FALSE(rejected.has_value());
        REQUIRE(rejected.error()
            == MinimizerFormat6Error::reserved_nonzero);
    }

    SECTION("exception records are exact, ordered, and exhaustive") {
        const std::array<std::uint64_t, 2> two_hashes{42u, 43u};
        const std::array<std::uint64_t, 2> two_counts{65'535u, 65'536u};
        auto canonical = encode_minimizer_format6_control_block(
            two_hashes, two_counts, 0u, 0u, 4u, 4u);
        REQUIRE(canonical.has_value());
        REQUIRE(canonical->size() == 64u + 1u + 4u + 24u);

        auto duplicate = *canonical;
        test_write_le<std::uint16_t>(
            std::span<std::byte>{duplicate}, 81u, 0u);
        recompute_format6_control_crc(duplicate);
        REQUIRE_FALSE(decode_minimizer_format6_control_block(
            duplicate, 4u, 4u));

        auto missing = *canonical;
        missing.resize(missing.size() - 12u);
        test_write_le<std::uint32_t>(
            std::span<std::byte>{missing}, 40u, 12u);
        recompute_format6_control_crc(missing);
        REQUIRE_FALSE(decode_minimizer_format6_control_block(
            missing, 4u, 4u));

        auto trailing = *canonical;
        trailing.push_back(std::byte{0});
        REQUIRE_FALSE(decode_minimizer_format6_control_block(
            trailing, 4u, 4u));
    }
}

TEST_CASE("Format-5 and Format-6 tagged dispatch is exact-length only",
          "[indexing][minimizer-hashmap][wire][v6][selective]") {
    using namespace bioseq::indexing;
    auto fixture = make_format6_small_fixture();
    const auto tagged_v6_head = decode_tagged_minimizer_generation_head(
        fixture.metadata.encoded_head);
    const auto tagged_v6_manifest =
        decode_tagged_minimizer_generation_manifest(
            fixture.metadata.encoded_manifest);
    REQUIRE(tagged_v6_head.has_value());
    REQUIRE(tagged_v6_head->index() == 1u);
    REQUIRE(tagged_v6_manifest.has_value());
    REQUIRE(tagged_v6_manifest->index() == 1u);

    const auto shape = minimizer_shape_profile_v5(
        kLegacyMinimapV1SchemeIdentity,
        kMinimizerPersistedDna4AlphabetCode,
        15u,
        8u,
        10u);
    REQUIRE(shape.has_value());
    auto v5_head = golden_generation_head();
    v5_head.wire_format = MinimizerGenerationWireFormat::
        grouped_shape_soa_segment_local;
    const auto encoded_v5_head = encode_minimizer_generation_head(v5_head);
    REQUIRE(encoded_v5_head.has_value());
    auto v5_manifest = golden_generation_manifest();
    v5_manifest.wire_format = MinimizerGenerationWireFormat::
        grouped_shape_soa_segment_local;
    v5_manifest.alphabet_code = kMinimizerPersistedDna4AlphabetCode;
    v5_manifest.kmer_size = 15u;
    v5_manifest.gpu_abi_version = kMinimizerShapeV5GpuAbiVersion;
    v5_manifest.shape_v5 = *shape;
    const auto encoded_v5_manifest =
        encode_minimizer_generation_manifest_v5(v5_manifest);
    REQUIRE(encoded_v5_manifest.has_value());
    const auto tagged_v5_head = decode_tagged_minimizer_generation_head(
        *encoded_v5_head);
    const auto tagged_v5_manifest =
        decode_tagged_minimizer_generation_manifest(*encoded_v5_manifest);
    REQUIRE(tagged_v5_head.has_value());
    REQUIRE(tagged_v5_head->index() == 0u);
    REQUIRE(tagged_v5_manifest.has_value());
    REQUIRE(tagged_v5_manifest->index() == 0u);

    for (const std::size_t wrong_size :
         {128u, 159u, 161u, 224u, 335u, 337u}) {
        INFO("wrong tagged manifest size " << wrong_size);
        std::vector<std::byte> bytes(wrong_size);
        std::copy_n(
            fixture.metadata.encoded_manifest.begin(),
            std::min(wrong_size, fixture.metadata.encoded_manifest.size()),
            bytes.begin());
        const auto decoded = decode_tagged_minimizer_generation_manifest(
            bytes);
        REQUIRE_FALSE(decoded.has_value());
        REQUIRE(decoded.error()
            == TaggedMinimizerGenerationWireError::encoded_size);
    }
    for (const std::size_t wrong_size : {63u, 65u, 223u, 225u}) {
        INFO("wrong tagged head size " << wrong_size);
        std::vector<std::byte> bytes(wrong_size);
        std::copy_n(
            fixture.metadata.encoded_head.begin(),
            std::min(wrong_size, fixture.metadata.encoded_head.size()),
            bytes.begin());
        const auto decoded = decode_tagged_minimizer_generation_head(bytes);
        REQUIRE_FALSE(decoded.has_value());
        REQUIRE(decoded.error()
            == TaggedMinimizerGenerationWireError::encoded_size);
    }

    std::array<std::byte, kMinimizerFormat6HeadBytes> padded_v5_head{};
    std::copy(encoded_v5_head->begin(), encoded_v5_head->end(),
        padded_v5_head.begin());
    const auto rejected_padded_head =
        decode_tagged_minimizer_generation_head(padded_v5_head);
    REQUIRE_FALSE(rejected_padded_head.has_value());
    REQUIRE(rejected_padded_head.error()
        == TaggedMinimizerGenerationWireError::format6);
    std::array<std::byte, kMinimizerFormat6ManifestBytes>
        padded_v5_manifest{};
    std::copy(encoded_v5_manifest->begin(), encoded_v5_manifest->end(),
        padded_v5_manifest.begin());
    const auto rejected_padded_manifest =
        decode_tagged_minimizer_generation_manifest(padded_v5_manifest);
    REQUIRE_FALSE(rejected_padded_manifest.has_value());
    REQUIRE(rejected_padded_manifest.error()
        == TaggedMinimizerGenerationWireError::format6);

    const auto old_head = encode_minimizer_generation_head(
        golden_generation_head());
    REQUIRE(old_head.has_value());
    const auto rejected_old_head = decode_tagged_minimizer_generation_head(
        *old_head);
    REQUIRE_FALSE(rejected_old_head.has_value());
    REQUIRE(rejected_old_head.error()
        == TaggedMinimizerGenerationWireError::format5);
    const auto old_manifest = encode_minimizer_generation_manifest(
        golden_generation_manifest());
    REQUIRE(old_manifest.has_value());
    const auto rejected_old_manifest =
        decode_tagged_minimizer_generation_manifest(*old_manifest);
    REQUIRE_FALSE(rejected_old_manifest.has_value());
    REQUIRE(rejected_old_manifest.error()
        == TaggedMinimizerGenerationWireError::encoded_size);
}

TEST_CASE("Format-6 selective reader requests only authorized postings and matches baseline",
          "[indexing][minimizer-hashmap][wire][v6][selective]") {
    using namespace bioseq::indexing;
    auto fixture = make_format6_small_fixture();
    const std::array<std::uint32_t, 3> queries{10u, 30u, 999u};
    auto reader = MinimizerFormat6SelectiveReader::open(
        bioseq::parallelism::MemoryAdmissionController::instance(),
        fixture.metadata.encoded_head,
        fixture.metadata.encoded_manifest,
        format6_expected(fixture.options),
        fixture.image.source(),
        format6_query_view(queries, fixture.options.compatibility),
        2u);
    REQUIRE(reader.has_value());
    REQUIRE(reader->target_length() == 100u);
    REQUIRE(fixture.image.posting_waves.size() == 1u);
    REQUIRE(fixture.image.posting_waves[0].size() == 2u);
    REQUIRE(fixture.image.posting_waves[0][0].control_block_ordinal == 0u);
    REQUIRE(fixture.image.posting_waves[0][0].posting_byte_offset == 0u);
    REQUIRE(fixture.image.posting_waves[0][0].byte_count == 8u);
    REQUIRE(fixture.image.posting_waves[0][1].control_block_ordinal == 0u);
    REQUIRE(fixture.image.posting_waves[0][1].posting_byte_offset == 12u);
    REQUIRE(fixture.image.posting_waves[0][1].byte_count == 8u);
    REQUIRE(fixture.image.control_waves.size() == 1u);
    REQUIRE(reader->posting_wave_peak_owner_bytes()
        <= kMinimizerFormat6ReadSlabBytes);
    REQUIRE(reader->control_wave_peak_owner_bytes()
        <= kMinimizerFormat6ControlReadWaveBlocks
            * sizeof(MinimizerFormat6AuthorizedControlRange));
    REQUIRE(reader->transient_authority_retained_bytes() == 0u);
    const auto posting_reads = fixture.image.reads_of(
        MinimizerFormat6StreamKind::postings);
    REQUIRE(posting_reads.size() == 2u);
    REQUIRE(posting_reads[0].offset == 0u);
    REQUIRE(posting_reads[0].byte_count == 8u);
    REQUIRE(posting_reads[1].offset == 12u);
    REQUIRE(posting_reads[1].byte_count == 8u);

    MinimizerHashmapFragmentIndex baseline;
    auto baseline_pairs = fixture.pairs;
    baseline.build_from_pairs(
        std::span<std::pair<std::uint64_t, std::uint64_t>>{baseline_pairs},
        100u);
    for (const std::uint64_t hash : std::array{10u, 30u, 999u}) {
        const auto spans = reader->lookup_spans(hash);
        REQUIRE(spans.positions_complete());
        REQUIRE(spans.well_formed());
        const auto expected = baseline.lookup(hash);
        REQUIRE(std::ranges::equal(
            spans.contiguous_positions(), expected));
    }
    REQUIRE_THROWS_AS(
        reader->lookup_spans(20u), PersistedMinimizerLookupError);

    SECTION("scalar logical-read fallback preserves the same authority") {
        auto scalar_fixture = make_format6_small_fixture();
        scalar_fixture.image.use_wave_callbacks = false;
        const auto scalar_reader = MinimizerFormat6SelectiveReader::open(
            bioseq::parallelism::MemoryAdmissionController::instance(),
            scalar_fixture.metadata.encoded_head,
            scalar_fixture.metadata.encoded_manifest,
            format6_expected(scalar_fixture.options),
            scalar_fixture.image.source(),
            format6_query_view(queries,
                scalar_fixture.options.compatibility),
            2u);
        REQUIRE(scalar_reader.has_value());
        REQUIRE(scalar_fixture.image.control_waves.empty());
        REQUIRE(scalar_fixture.image.posting_waves.empty());
        REQUIRE(
            scalar_reader->memory_evidence().
                generic_source_fallback_calls > 0u);
        REQUIRE(std::ranges::equal(
            scalar_reader->lookup_spans(10u).contiguous_positions(),
            std::array<std::uint64_t, 2>{2u, 4u}));
        REQUIRE(std::ranges::equal(
            scalar_reader->lookup_spans(30u).contiguous_positions(),
            std::array<std::uint64_t, 2>{20u, 21u}));
    }
}

TEST_CASE("Format-6 applies max_occ before posting IO with exact heavy count",
          "[indexing][minimizer-hashmap][wire][v6][selective]") {
    using namespace bioseq::indexing;
    Format6MemoryImage image;
    constexpr std::uint64_t count = 65'535u;
    const auto options = format6_test_options(70'000u);
    MinimizerFormat6LogicalEncoder encoder{
        options, image.sink()};
    REQUIRE(encoder.begin_hash(42u, count).has_value());
    for (std::uint64_t bp = 0u; bp < count; ++bp) {
        const auto appended = encoder.append_posting(bp << 1u);
        if (!appended) {
            FAIL("Format-6 streaming posting append failed at " << bp);
        }
    }
    REQUIRE(encoder.end_hash().has_value());
    auto metadata = encoder.finish();
    REQUIRE(metadata.has_value());
    const std::array<std::uint32_t, 1> query{42u};
    auto reader = MinimizerFormat6SelectiveReader::open(
        bioseq::parallelism::MemoryAdmissionController::instance(),
        metadata->encoded_head,
        metadata->encoded_manifest,
        format6_expected(options),
        image.source(),
        format6_query_view(query, options.compatibility),
        10u);
    REQUIRE(reader.has_value());
    const auto spans = reader->lookup_spans(42u);
    REQUIRE_FALSE(spans.positions_complete());
    REQUIRE(spans.global_occurrence_count() == count);
    REQUIRE(spans.well_formed());
    REQUIRE(reader->exact_count_exceptions().size() == 1u);
    REQUIRE(image.reads_of(MinimizerFormat6StreamKind::postings).empty());
}

TEST_CASE("Format-6 admission is exact at every pre-I/O phase barrier",
          "[indexing][minimizer-hashmap][wire][v6][selective][admission]") {
    using namespace bioseq::indexing;
    const std::array<std::uint32_t, 3> queries{10u, 20u, 30u};
    MinimizerFormat6MemoryEvidence evidence{};
    {
        auto fixture = make_format6_small_fixture();
        bioseq::parallelism::MemoryAdmissionController admission;
        const auto reader = MinimizerFormat6SelectiveReader::open(
            admission,
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            format6_expected(fixture.options),
            fixture.image.source(),
            format6_query_view(queries, fixture.options.compatibility),
            2u);
        REQUIRE(reader.has_value());
        evidence = reader->memory_evidence();
        REQUIRE(evidence.directory_phase_peak_admitted_bytes != 0u);
        REQUIRE(evidence.control_phase_peak_admitted_bytes != 0u);
        REQUIRE(evidence.posting_phase_peak_admitted_bytes != 0u);
        REQUIRE(evidence.peak_admitted_bytes == std::max({
            evidence.directory_phase_peak_admitted_bytes,
            evidence.control_phase_peak_admitted_bytes,
            evidence.posting_phase_peak_admitted_bytes,
        }));
        REQUIRE(evidence.retained_owner_bytes == reader->admitted_bytes());
        REQUIRE(evidence.directory_stream_crc_receipts == 1u);
        REQUIRE(evidence.control_block_crc_receipts
                == evidence.selected_block_count);
        REQUIRE(evidence.control_semantic_receipts
                == evidence.selected_block_count);
        REQUIRE(evidence.posting_ranges_delivered
                == evidence.posting_semantic_receipts);
        REQUIRE(evidence.posting_range_crc_receipts
                <= evidence.posting_ranges_delivered);
        REQUIRE(evidence.generic_source_fallback_calls == 0u);
        REQUIRE(admission.reserved_bytes() == reader->admitted_bytes());
    }

    SECTION("directory peak minus one denies before source size or read") {
        auto fixture = make_format6_small_fixture();
        bioseq::parallelism::MemoryAdmissionController admission;
        admission.set_index_memory_cap_bytes(
            evidence.directory_phase_peak_admitted_bytes - 1u);
        const auto reader = MinimizerFormat6SelectiveReader::open(
            admission,
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            format6_expected(fixture.options),
            fixture.image.source(),
            format6_query_view(queries, fixture.options.compatibility),
            2u);
        REQUIRE_FALSE(reader.has_value());
        REQUIRE(reader.error() == MinimizerFormat6Error::admission);
        REQUIRE(fixture.image.size_calls == 0u);
        REQUIRE(fixture.image.reads.empty());
        REQUIRE(fixture.image.control_waves.empty());
        REQUIRE(fixture.image.posting_waves.empty());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("control peak minus one denies before a control callback") {
        auto fixture = make_format6_small_fixture();
        bioseq::parallelism::MemoryAdmissionController admission;
        admission.set_index_memory_cap_bytes(
            evidence.directory_phase_peak_admitted_bytes);
        fixture.image.cap_controller = &admission;
        fixture.image.cap_after_directory_read =
            evidence.control_phase_peak_admitted_bytes - 1u;
        const auto reader = MinimizerFormat6SelectiveReader::open(
            admission,
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            format6_expected(fixture.options),
            fixture.image.source(),
            format6_query_view(queries, fixture.options.compatibility),
            2u);
        REQUIRE_FALSE(reader.has_value());
        REQUIRE(reader.error() == MinimizerFormat6Error::admission);
        REQUIRE(fixture.image.size_calls == 3u);
        REQUIRE(fixture.image.reads_of(
            MinimizerFormat6StreamKind::directory).size() == 1u);
        REQUIRE(fixture.image.control_waves.empty());
        REQUIRE(fixture.image.posting_waves.empty());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("posting peak minus one denies before a posting callback") {
        auto fixture = make_format6_small_fixture();
        bioseq::parallelism::MemoryAdmissionController admission;
        admission.set_index_memory_cap_bytes(
            evidence.peak_admitted_bytes);
        fixture.image.cap_controller = &admission;
        fixture.image.cap_after_control_wave =
            evidence.posting_phase_peak_admitted_bytes - 1u;
        const auto reader = MinimizerFormat6SelectiveReader::open(
            admission,
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            format6_expected(fixture.options),
            fixture.image.source(),
            format6_query_view(queries, fixture.options.compatibility),
            2u);
        REQUIRE_FALSE(reader.has_value());
        REQUIRE(reader.error() == MinimizerFormat6Error::admission);
        REQUIRE_FALSE(fixture.image.control_waves.empty());
        REQUIRE(fixture.image.reads_of(
            MinimizerFormat6StreamKind::postings).empty());
        REQUIRE(fixture.image.posting_waves.empty());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("move construction transfers the sole retained permit") {
        auto fixture = make_format6_small_fixture();
        bioseq::parallelism::MemoryAdmissionController admission;
        auto made = MinimizerFormat6SelectiveReader::open(
            admission,
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            format6_expected(fixture.options),
            fixture.image.source(),
            format6_query_view(queries, fixture.options.compatibility),
            2u);
        REQUIRE(made.has_value());
        const std::uint64_t retained_bytes = made->admitted_bytes();
        MinimizerFormat6SelectiveReader moved{std::move(*made)};
        REQUIRE(made->admitted_bytes() == 0u);
        REQUIRE(made->memory_evidence().retained_owner_bytes == 0u);
        REQUIRE(moved.admitted_bytes() == retained_bytes);
        REQUIRE(admission.reserved_bytes() == retained_bytes);
        REQUIRE(std::ranges::equal(
            moved.lookup_spans(10u).contiguous_positions(),
            std::array<std::uint64_t, 2>{2u, 4u}));
    }
}

TEST_CASE("Format-6 authenticates every caller expectation before stream IO",
          "[indexing][minimizer-hashmap][wire][v6][selective]") {
    using namespace bioseq::indexing;
    auto fixture = make_format6_small_fixture();
    const std::array<std::uint32_t, 1> query{10u};
    std::vector<MinimizerFormat6ExpectedGeneration> mismatches;

    auto mismatch = format6_expected(fixture.options);
    ++mismatch.generation_id;
    mismatches.push_back(mismatch);
    mismatch = format6_expected(fixture.options);
    ++mismatch.query_key;
    mismatches.push_back(mismatch);
    mismatch = format6_expected(fixture.options);
    ++mismatch.source_epoch;
    mismatches.push_back(mismatch);
    mismatch = format6_expected(fixture.options);
    mismatch.source_content_crc64 ^= 1u;
    mismatches.push_back(mismatch);
    mismatch = format6_expected(fixture.options);
    ++mismatch.target_length;
    mismatches.push_back(mismatch);
    mismatch = format6_expected(fixture.options);
    mismatch.compatibility.producer_backend_id = 2u;
    mismatches.push_back(mismatch);

    for (std::size_t index = 0u; index < mismatches.size(); ++index) {
        INFO("expected-generation mismatch ordinal " << index);
        const auto reader = MinimizerFormat6SelectiveReader::open(
            bioseq::parallelism::MemoryAdmissionController::instance(),
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            mismatches[index],
            fixture.image.source(),
            format6_query_view(query, fixture.options.compatibility),
            2u);
        REQUIRE_FALSE(reader.has_value());
        REQUIRE(reader.error() == MinimizerFormat6Error::expected_generation);
        REQUIRE(fixture.image.reads.empty());
        REQUIRE(fixture.image.control_waves.empty());
        REQUIRE(fixture.image.posting_waves.empty());
    }
}

TEST_CASE("Format-6 authenticates fully selected blocks and whole streams",
          "[indexing][minimizer-hashmap][wire][v6][selective]") {
    using namespace bioseq::indexing;
    const std::array<std::uint32_t, 3> all_queries{10u, 20u, 30u};

    SECTION("full selected posting block CRC") {
        auto fixture = make_format6_small_fixture();
        auto& postings = fixture.image.streams[
            Format6MemoryImage::index(MinimizerFormat6StreamKind::postings)];
        test_write_le<std::uint32_t>(
            std::span<std::byte>{postings}, 4u, 6u);
        const auto reader = MinimizerFormat6SelectiveReader::open(
            bioseq::parallelism::MemoryAdmissionController::instance(),
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            format6_expected(fixture.options),
            fixture.image.source(),
            format6_query_view(all_queries, fixture.options.compatibility),
            2u);
        REQUIRE_FALSE(reader.has_value());
        REQUIRE(reader.error() == MinimizerFormat6Error::checksum);
    }

    for (const std::size_t stream_index : {1u, 2u}) {
        INFO("whole-stream CRC descriptor ordinal " << stream_index);
        auto fixture = make_format6_small_fixture();
        auto manifest = fixture.metadata.manifest;
        manifest.streams[stream_index].stream_crc64 ^= 1u;
        const auto encoded_manifest = encode_minimizer_format6_manifest(
            manifest);
        REQUIRE(encoded_manifest.has_value());
        auto head = fixture.metadata.head;
        head.manifest_payload_crc64 = crc64_ecma182(*encoded_manifest);
        head.manifest_address_bound_row_crc64 = crc64_index_chunk_row(
            head.generation_id,
            0,
            0,
            0u,
            static_cast<std::uint32_t>(encoded_manifest->size()),
            *encoded_manifest);
        const auto encoded_head = encode_minimizer_format6_head(head);
        REQUIRE(encoded_head.has_value());
        const auto reader = MinimizerFormat6SelectiveReader::open(
            bioseq::parallelism::MemoryAdmissionController::instance(),
            *encoded_head,
            *encoded_manifest,
            format6_expected(fixture.options),
            fixture.image.source(),
            format6_query_view(all_queries, fixture.options.compatibility),
            2u);
        REQUIRE_FALSE(reader.has_value());
        REQUIRE(reader.error() == MinimizerFormat6Error::checksum);
    }
}

TEST_CASE("Format-6 selected corruption fails while unselected bytes stay unread",
          "[indexing][minimizer-hashmap][wire][v6][selective]") {
    using namespace bioseq::indexing;
    SECTION("selected duplicate position fails semantic validation") {
        auto fixture = make_format6_small_fixture();
        auto& postings = fixture.image.streams[
            Format6MemoryImage::index(MinimizerFormat6StreamKind::postings)];
        std::copy_n(postings.begin(), 4u, postings.begin() + 4u);
        const std::array<std::uint32_t, 1> query{10u};
        auto reader = MinimizerFormat6SelectiveReader::open(
            bioseq::parallelism::MemoryAdmissionController::instance(),
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            format6_expected(fixture.options),
            fixture.image.source(),
            format6_query_view(query, fixture.options.compatibility),
            2u);
        REQUIRE_FALSE(reader.has_value());
        REQUIRE(reader.error() == MinimizerFormat6Error::order);
    }
    SECTION("corrupt non-query posting is not fetched") {
        auto fixture = make_format6_small_fixture();
        auto& postings = fixture.image.streams[
            Format6MemoryImage::index(MinimizerFormat6StreamKind::postings)];
        postings[8] = std::byte{0xff};
        postings[9] = std::byte{0xff};
        postings[10] = std::byte{0xff};
        postings[11] = std::byte{0xff};
        const std::array<std::uint32_t, 2> query{10u, 30u};
        auto reader = MinimizerFormat6SelectiveReader::open(
            bioseq::parallelism::MemoryAdmissionController::instance(),
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            format6_expected(fixture.options),
            fixture.image.source(),
            format6_query_view(query, fixture.options.compatibility),
            2u);
        REQUIRE(reader.has_value());
        for (const auto& read : fixture.image.reads_of(
                MinimizerFormat6StreamKind::postings)) {
            REQUIRE((read.offset + read.byte_count <= 8u
                || read.offset >= 12u));
        }
    }
    SECTION("empty query reads directory but no controls or postings") {
        auto fixture = make_format6_small_fixture();
        const std::array<std::uint32_t, 0> query{};
        auto reader = MinimizerFormat6SelectiveReader::open(
            bioseq::parallelism::MemoryAdmissionController::instance(),
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            format6_expected(fixture.options),
            fixture.image.source(),
            format6_query_view(query, fixture.options.compatibility),
            2u);
        REQUIRE(reader.has_value());
        REQUIRE(fixture.image.reads_of(
            MinimizerFormat6StreamKind::controls).empty());
        REQUIRE(fixture.image.reads_of(
            MinimizerFormat6StreamKind::postings).empty());
    }
}

TEST_CASE("Format-6 widens authorized u32 and preserves required u64 postings",
          "[indexing][minimizer-hashmap][wire][v6][selective]") {
    using namespace bioseq::indexing;
    const std::uint64_t target_length = (std::uint64_t{1} << 31u) + 10u;
    Format6MemoryImage image;
    const auto options = format6_test_options(target_length);
    MinimizerFormat6LogicalEncoder encoder{
        options, image.sink()};
    const std::array<std::uint64_t, 2> positions{
        ((std::uint64_t{1} << 31u) << 1u),
        (((std::uint64_t{1} << 31u) + 1u) << 1u) | 1u,
    };
    REQUIRE(encoder.append_hash(7u, positions).has_value());
    auto metadata = encoder.finish();
    REQUIRE(metadata.has_value());
    REQUIRE(metadata->manifest.compatibility.stored_posting_width_bytes == 8u);
    const std::array<std::uint32_t, 1> query{7u};
    auto reader = MinimizerFormat6SelectiveReader::open(
        bioseq::parallelism::MemoryAdmissionController::instance(),
        metadata->encoded_head,
        metadata->encoded_manifest,
        format6_expected(options),
        image.source(),
        format6_query_view(query, options.compatibility),
        2u);
    REQUIRE(reader.has_value());
    REQUIRE(std::ranges::equal(
        reader->lookup_spans(7u).contiguous_positions(), positions));
}

TEST_CASE("Format-6 posting-width and target-length boundaries are exact",
          "[indexing][minimizer-hashmap][wire][v6][selective]") {
    using namespace bioseq::indexing;

    SECTION("2^31 target retains the largest legal u32 posting") {
        constexpr std::uint64_t target_length = std::uint64_t{1} << 31u;
        Format6MemoryImage image;
        const auto options = format6_test_options(target_length);
        REQUIRE(options.compatibility.stored_posting_width_bytes == 4u);
        MinimizerFormat6LogicalEncoder encoder{options, image.sink()};
        const std::array<std::uint64_t, 1> positions{
            (std::numeric_limits<std::uint32_t>::max)()};
        REQUIRE(encoder.append_hash(7u, positions).has_value());
        const auto metadata = encoder.finish();
        REQUIRE(metadata.has_value());
        const std::array<std::uint32_t, 1> query{7u};
        const auto reader = MinimizerFormat6SelectiveReader::open(
            bioseq::parallelism::MemoryAdmissionController::instance(),
            metadata->encoded_head,
            metadata->encoded_manifest,
            format6_expected(options),
            image.source(),
            format6_query_view(query, options.compatibility),
            1u);
        REQUIRE(reader.has_value());
        REQUIRE(reader->lookup_spans(7u).contiguous_positions().front()
            == positions.front());
    }

    SECTION("2^31+1 target requires u64") {
        constexpr std::uint64_t target_length =
            (std::uint64_t{1} << 31u) + 1u;
        const auto options = format6_test_options(target_length);
        REQUIRE(options.compatibility.stored_posting_width_bytes == 8u);
        REQUIRE(valid_minimizer_format6_compatibility(
            options.compatibility, target_length));
    }

    SECTION("2^63 target retains UINT64_MAX and rejects the next length") {
        constexpr std::uint64_t target_length = std::uint64_t{1} << 63u;
        Format6MemoryImage image;
        const auto options = format6_test_options(target_length);
        MinimizerFormat6LogicalEncoder encoder{options, image.sink()};
        const std::array<std::uint64_t, 1> positions{
            (std::numeric_limits<std::uint64_t>::max)()};
        REQUIRE(encoder.append_hash(7u, positions).has_value());
        const auto metadata = encoder.finish();
        REQUIRE(metadata.has_value());
        const std::array<std::uint32_t, 1> query{7u};
        const auto reader = MinimizerFormat6SelectiveReader::open(
            bioseq::parallelism::MemoryAdmissionController::instance(),
            metadata->encoded_head,
            metadata->encoded_manifest,
            format6_expected(options),
            image.source(),
            format6_query_view(query, options.compatibility),
            1u);
        REQUIRE(reader.has_value());
        REQUIRE(reader->lookup_spans(7u).contiguous_positions().front()
            == positions.front());

        constexpr std::uint64_t invalid_target = target_length + 1u;
        const auto invalid_options = format6_test_options(invalid_target);
        REQUIRE_FALSE(valid_minimizer_format6_compatibility(
            invalid_options.compatibility, invalid_target));
        Format6MemoryImage rejected_image;
        MinimizerFormat6LogicalEncoder rejected_encoder{
            invalid_options, rejected_image.sink()};
        REQUIRE_FALSE(rejected_encoder.begin_hash(7u, 1u).has_value());

        auto invalid_manifest = metadata->encoded_manifest;
        test_write_le<std::uint64_t>(invalid_manifest, 56u, invalid_target);
        const auto decoded = decode_minimizer_format6_manifest(
            invalid_manifest);
        REQUIRE_FALSE(decoded.has_value());
        REQUIRE(decoded.error() == MinimizerFormat6Error::stream_descriptor);
    }
}

TEST_CASE("Format-6 row ordinals and bounded waves span physical rows exactly",
          "[indexing][minimizer-hashmap][wire][v6][selective]") {
    using namespace bioseq::indexing;
    constexpr std::uint32_t hash_count =
        kMinimizerFormat6ControlsPerBlock + 1u;
    Format6MemoryImage image;
    const auto options = format6_test_options(5'000u);
    MinimizerFormat6LogicalEncoder encoder{options, image.sink()};
    for (std::uint64_t hash = 1u; hash <= hash_count; ++hash) {
        const std::array<std::uint64_t, 1> position{
            (hash % options.target_length) << 1u};
        REQUIRE(encoder.append_hash(hash, position).has_value());
    }
    const auto metadata = encoder.finish();
    REQUIRE(metadata.has_value());
    REQUIRE(metadata->manifest.directory_entry_count == 2u);
    REQUIRE(image.append_order
        == std::vector<MinimizerFormat6StreamKind>{
            MinimizerFormat6StreamKind::postings,
            MinimizerFormat6StreamKind::postings,
            MinimizerFormat6StreamKind::postings,
            MinimizerFormat6StreamKind::controls,
            MinimizerFormat6StreamKind::directory,
            MinimizerFormat6StreamKind::postings,
            MinimizerFormat6StreamKind::controls,
            MinimizerFormat6StreamKind::directory});
    REQUIRE(metadata->manifest.streams[0].row_count == 1u);
    REQUIRE(metadata->manifest.streams[0].first_row_ordinal == 0u);
    REQUIRE(metadata->manifest.streams[1].row_count == 2u);
    REQUIRE(metadata->manifest.streams[1].first_row_ordinal == 1u);
    REQUIRE(metadata->manifest.streams[2].row_count == 3u);
    REQUIRE(metadata->manifest.streams[2].first_row_ordinal == 3u);

    const std::array<std::uint32_t, 3> queries{
        1u, kMinimizerFormat6ControlsPerBlock, hash_count};
    const auto reader = MinimizerFormat6SelectiveReader::open(
        bioseq::parallelism::MemoryAdmissionController::instance(),
        metadata->encoded_head,
        metadata->encoded_manifest,
        format6_expected(options),
        image.source(),
        format6_query_view(queries, options.compatibility),
        1u);
    REQUIRE(reader.has_value());
    REQUIRE(image.control_waves.size() == 1u);
    REQUIRE(image.control_waves.front().size() == 2u);
    REQUIRE(reader->posting_wave_peak_owner_bytes()
        <= kMinimizerFormat6ReadSlabBytes);
    REQUIRE(reader->transient_authority_retained_bytes() == 0u);

    const auto& directory_bytes = image.streams[
        Format6MemoryImage::index(MinimizerFormat6StreamKind::directory)];
    for (const auto& wave : image.posting_waves) {
        for (const auto& range : wave) {
            REQUIRE(range.control_block_ordinal < 2u);
            const auto entry = decode_minimizer_format6_directory_entry(
                std::span<const std::byte>{directory_bytes}.subspan(
                    static_cast<std::size_t>(range.control_block_ordinal)
                        * kMinimizerFormat6DirectoryEntryBytes,
                    kMinimizerFormat6DirectoryEntryBytes));
            REQUIRE(entry.has_value());
            REQUIRE(range.posting_byte_offset >= entry->posting_byte_offset);
            REQUIRE(range.byte_count
                <= entry->posting_byte_offset + entry->posting_byte_length
                    - range.posting_byte_offset);
        }
    }
}

}  // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
