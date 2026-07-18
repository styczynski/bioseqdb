// Reduced real-wire differential tests for the Format-5 compact-H32 adapter.

#include "common_tests.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

import bioseq.indexing.minimizer_format5_compact_h32_adapter;
import bioseq.indexing.storage;
import bioseq.indexing.type;

namespace {

namespace idx = bioseq::indexing;

template <class UInt>
requires std::is_unsigned_v<UInt>
void write_le(
        std::span<std::byte> output,
        std::size_t offset,
        UInt value) {
    REQUIRE(offset <= output.size());
    REQUIRE(sizeof(UInt) <= output.size() - offset);
    for (std::size_t byte = 0u; byte < sizeof(UInt); ++byte) {
        output[offset + byte] = static_cast<std::byte>(value & UInt{0xffu});
        value >>= 8u;
    }
}

void write_shape_scalar(
        std::span<std::byte> output,
        std::size_t offset,
        idx::MinimizerScalarWidth width,
        std::uint64_t value) {
    switch (width) {
    case idx::MinimizerScalarWidth::u16:
        REQUIRE(value <= (std::numeric_limits<std::uint16_t>::max)());
        write_le<std::uint16_t>(
            output, offset, static_cast<std::uint16_t>(value));
        return;
    case idx::MinimizerScalarWidth::u32:
        REQUIRE(value <= (std::numeric_limits<std::uint32_t>::max)());
        write_le<std::uint32_t>(
            output, offset, static_cast<std::uint32_t>(value));
        return;
    case idx::MinimizerScalarWidth::u64:
        write_le<std::uint64_t>(output, offset, value);
        return;
    }
    FAIL("unsupported Format-5 scalar width");
}

struct LocalPosition {
    std::uint64_t bp = 0u;
    bool strand_one = false;
};

struct ControlSpec {
    std::uint32_t hash = 0u;
    std::vector<LocalPosition> positions{};
};

struct FragmentSpec {
    std::uint32_t position_segment = 0u;
    std::vector<ControlSpec> controls{};
};

struct Format5Fixture {
    idx::MinimizerGenerationHead head{};
    idx::MinimizerGenerationManifest manifest{};
    idx::DirView directory{};
    std::vector<std::byte> payload{};
};

[[nodiscard]] std::uint32_t chunks_for(std::uint64_t bytes) {
    const std::uint64_t chunks = bytes
            / idx::kMinimizerFormat5ExtentPayloadBytes
        + (bytes % idx::kMinimizerFormat5ExtentPayloadBytes != 0u);
    REQUIRE(chunks > 0u);
    REQUIRE(chunks <= (std::numeric_limits<std::uint32_t>::max)());
    return static_cast<std::uint32_t>(chunks);
}

[[nodiscard]] Format5Fixture make_format5_fixture(
        std::uint64_t target_length,
        std::uint64_t segment_span,
        std::uint64_t format5_cap,
        idx::MinimizerSchemeIdentity scheme,
        idx::MinimizerProducerBackendIdentity producer,
        std::uint16_t kmer_size,
        std::span<const FragmentSpec> fragments) {
    const auto profile = idx::minimizer_generation_payload_profile(
        idx::MinimizerGenerationWireFormat::
            grouped_shape_soa_segment_local,
        scheme,
        idx::kMinimizerPersistedDna4AlphabetCode,
        kmer_size,
        segment_span,
        format5_cap);
    REQUIRE(profile.has_value());
    REQUIRE(profile->shape_v5.has_value());

    Format5Fixture fixture;
    fixture.manifest = idx::MinimizerGenerationManifest{
        .wire_format = idx::MinimizerGenerationWireFormat::
            grouped_shape_soa_segment_local,
        .generation_id = 0x12345678u,
        .source_identity = 0x23456789u,
        .source_content_checksum = 0x3456789au,
        .target_length = target_length,
        .alphabet_code = idx::kMinimizerPersistedDna4AlphabetCode,
        .storage_unit_byte_width = 1u,
        .kmer_size = kmer_size,
        .window_size = 2u,
        .gpu_abi_version = profile->gpu_abi_version,
        .position_segment_span_bp = segment_span,
        .segment_geometry_version =
            idx::MinimizerGenerationManifest::
                uniform_segment_geometry_version,
        .minimizer_scheme_id = scheme.id,
        .minimizer_scheme_version = scheme.version,
        .producer_backend_id = producer.id,
        .producer_backend_identity_version = producer.version,
        .shape_v5 = profile->shape_v5,
    };
    fixture.manifest.position_segment_count = static_cast<std::uint32_t>(
        target_length / segment_span
        + (target_length % segment_span != 0u));

    std::vector<idx::MinimizerBucketDirEntry> entries;
    std::vector<std::vector<std::byte>> control_images;
    std::vector<std::vector<std::byte>> position_images;
    for (const FragmentSpec& fragment : fragments) {
        REQUIRE_FALSE(fragment.controls.empty());
        REQUIRE(fragment.position_segment
            < fixture.manifest.position_segment_count);
        std::uint64_t position_count = 0u;
        std::uint32_t previous_hash = 0u;
        bool have_previous_hash = false;
        for (const ControlSpec& control : fragment.controls) {
            REQUIRE_FALSE(control.positions.empty());
            REQUIRE_FALSE((have_previous_hash
                && control.hash <= previous_hash));
            previous_hash = control.hash;
            have_previous_hash = true;
            position_count += control.positions.size();
        }
        const idx::MinimizerScalarWidth offset_width =
            idx::minimizer_shape_v5_offset_width(position_count);
        const auto layout = idx::minimizer_shape_v5_control_layout(
            fragment.controls.size(),
            profile->shape_v5->hash_width,
            offset_width,
            profile->shape_v5->count_width);
        REQUIRE(layout.has_value());
        std::vector<std::byte> controls(
            static_cast<std::size_t>(layout->encoded_bytes), std::byte{0});
        std::vector<std::byte> positions(
            static_cast<std::size_t>(position_count)
                * static_cast<std::uint8_t>(
                    profile->shape_v5->position_width),
            std::byte{0});
        const std::size_t hash_width = static_cast<std::uint8_t>(
            profile->shape_v5->hash_width);
        const std::size_t prefix_width = static_cast<std::uint8_t>(
            offset_width);
        const std::size_t count_width = static_cast<std::uint8_t>(
            profile->shape_v5->count_width);
        const std::size_t position_width = static_cast<std::uint8_t>(
            profile->shape_v5->position_width);
        std::uint64_t prefix = 0u;
        write_shape_scalar(
            controls,
            static_cast<std::size_t>(layout->prefix_offset),
            offset_width,
            0u);
        for (std::size_t control_ordinal = 0u;
             control_ordinal < fragment.controls.size();
             ++control_ordinal) {
            const ControlSpec& control =
                fragment.controls[control_ordinal];
            write_shape_scalar(
                controls,
                control_ordinal * hash_width,
                profile->shape_v5->hash_width,
                control.hash);
            std::optional<std::uint64_t> previous_local{};
            for (const LocalPosition& position : control.positions) {
                REQUIRE(position.bp < segment_span);
                const std::uint64_t local =
                    (position.bp << 1u)
                    | static_cast<std::uint64_t>(position.strand_one);
                REQUIRE_FALSE((previous_local
                    && local <= *previous_local));
                previous_local = local;
                write_shape_scalar(
                    positions,
                    static_cast<std::size_t>(prefix) * position_width,
                    profile->shape_v5->position_width,
                    local);
                ++prefix;
            }
            write_shape_scalar(
                controls,
                static_cast<std::size_t>(layout->prefix_offset)
                    + (control_ordinal + 1u) * prefix_width,
                offset_width,
                prefix);
            write_shape_scalar(
                controls,
                static_cast<std::size_t>(layout->count_offset)
                    + control_ordinal * count_width,
                profile->shape_v5->count_width,
                std::min<std::uint64_t>(
                    control.positions.size(), format5_cap + 1u));
        }
        REQUIRE(prefix == position_count);
        const std::uint32_t partition =
            static_cast<std::uint32_t>(entries.size());
        entries.push_back(idx::MinimizerBucketDirEntry{
            .partition_id = partition,
            .position_segment = static_cast<std::int32_t>(
                fragment.position_segment),
            .lo_hash = fragment.controls.front().hash,
            .hi_hash = fragment.controls.back().hash,
            .sub_key = static_cast<idx::index_sub_key_t>(partition),
            .control_first_chunk = 0u,
            .control_chunk_count = chunks_for(controls.size()),
            .position_first_chunk = 0u,
            .position_chunk_count = chunks_for(positions.size()),
            .control_element_count = fragment.controls.size(),
            .position_element_count = position_count,
            .control_encoded_bytes = controls.size(),
            .position_encoded_bytes = positions.size(),
            .control_checksum = idx::crc64_ecma182(controls),
            .position_checksum = idx::crc64_ecma182(positions),
            .shape_offset_width = offset_width,
        });
        control_images.push_back(std::move(controls));
        position_images.push_back(std::move(positions));
    }

    const auto encoded_directory = idx::encode_minimizer_bucket_directory(
        entries, *profile);
    REQUIRE(encoded_directory.has_value());
    const auto directory = idx::DirView::decode(
        *encoded_directory, *profile);
    REQUIRE(directory.has_value());
    fixture.directory = std::move(*directory);
    for (const auto& controls : control_images) {
        fixture.payload.insert(
            fixture.payload.end(), controls.begin(), controls.end());
    }
    for (const auto& positions : position_images) {
        fixture.payload.insert(
            fixture.payload.end(), positions.begin(), positions.end());
    }
    fixture.manifest.control_count = 0u;
    fixture.manifest.position_count = 0u;
    for (const auto& entry : entries) {
        fixture.manifest.control_count += entry.control_element_count;
        fixture.manifest.position_count += entry.position_element_count;
    }
    fixture.manifest.payload_bytes = fixture.payload.size();
    fixture.manifest.payload_checksum = idx::crc64_ecma182(fixture.payload);
    fixture.manifest.directory_bytes = encoded_directory->size();
    fixture.manifest.directory_checksum =
        idx::crc64_ecma182(*encoded_directory);
    fixture.manifest.extent_count =
        static_cast<std::uint32_t>(entries.size() * 2u);
    fixture.manifest.directory_chunk_count = chunks_for(
        encoded_directory->size());
    fixture.head = idx::MinimizerGenerationHead{
        .wire_format = fixture.manifest.wire_format,
        .generation_id = fixture.manifest.generation_id,
        .source_identity = fixture.manifest.source_identity,
        .compatibility_checksum = 0x456789abu,
        .manifest_checksum = 0x56789abcu,
        .source_content_checksum =
            fixture.manifest.source_content_checksum,
    };
    return fixture;
}

using Oracle = std::map<std::uint32_t, std::vector<std::uint64_t>>;

[[nodiscard]] Oracle make_oracle(
        std::uint64_t segment_span,
        std::span<const FragmentSpec> fragments) {
    Oracle oracle;
    for (const FragmentSpec& fragment : fragments) {
        const std::uint64_t base =
            static_cast<std::uint64_t>(fragment.position_segment)
            * segment_span;
        for (const ControlSpec& control : fragment.controls) {
            for (const LocalPosition& position : control.positions) {
                const std::uint64_t global_bp = base + position.bp;
                oracle[control.hash].push_back(
                    (global_bp << 1u)
                    | static_cast<std::uint64_t>(position.strand_one));
            }
        }
    }
    for (auto& [hash, positions] : oracle) {
        static_cast<void>(hash);
        std::sort(positions.begin(), positions.end(),
            [](std::uint64_t left, std::uint64_t right) {
                return std::tuple{left & 1u, left >> 1u}
                    < std::tuple{right & 1u, right >> 1u};
            });
    }
    return oracle;
}

struct CollectedPostings {
    std::vector<std::vector<std::uint64_t>> by_selection{};
    std::uint64_t callbacks = 0u;
};

bool collect_posting(
        void* opaque,
        const idx::DirectIndexPosting& posting) {
    auto& collected = *static_cast<CollectedPostings*>(opaque);
    if (posting.selection_ordinal >= collected.by_selection.size()) {
        return false;
    }
    collected.by_selection[posting.selection_ordinal].push_back(
        posting.encoded_position);
    ++collected.callbacks;
    return posting.target_strand_one
        == ((posting.encoded_position & 1u) != 0u);
}

struct CapturedRuns {
    std::vector<idx::CompactH32SortedRunHeader> headers{};
    std::vector<std::vector<std::uint64_t>> positions{};
};

[[nodiscard]] std::expected<void, idx::DirectInvertedIndexError>
capture_begin(
        void* opaque,
        const idx::CompactH32SortedRunHeader& header) {
    auto& captured = *static_cast<CapturedRuns*>(opaque);
    captured.headers.push_back(header);
    captured.positions.emplace_back();
    return {};
}

[[nodiscard]] std::expected<void, idx::DirectInvertedIndexError>
capture_positions(
        void* opaque,
        std::span<const std::uint64_t> positions) {
    auto& captured = *static_cast<CapturedRuns*>(opaque);
    if (captured.positions.empty()) {
        return std::unexpected(idx::DirectInvertedIndexError::source_read);
    }
    captured.positions.back().insert(
        captured.positions.back().end(),
        positions.begin(), positions.end());
    return {};
}

} // namespace

TEST_CASE(
    "Format-5 compact-H32 adapter globally merges fragment runs and matches "
    "a map oracle without a target-wide owner",
    "[doc0383][p5][format5][compact-h32][differential][replay]") {
    constexpr std::uint64_t segment_span = 16u;
    const std::vector<FragmentSpec> fragments{
        FragmentSpec{
            .position_segment = 0u,
            .controls = {
                ControlSpec{
                    .hash = 5u,
                    .positions = {{.bp = 0u, .strand_one = false}},
                },
                ControlSpec{
                    .hash = 10u,
                    // Same bp on both strands is a real duplicate-bp case,
                    // while the two encoded Format-5 events remain distinct.
                    .positions = {
                        {.bp = 15u, .strand_one = false},
                        {.bp = 15u, .strand_one = true},
                    },
                },
            },
        },
        // A second directory entry in the same physical fragment exercises
        // entry-local prefix/position offsets without adding another heap
        // cursor.
        FragmentSpec{
            .position_segment = 0u,
            .controls = {
                ControlSpec{
                    .hash = 20u,
                    .positions = {
                        {.bp = 2u, .strand_one = false},
                        {.bp = 8u, .strand_one = true},
                    },
                },
            },
        },
        FragmentSpec{
            .position_segment = 1u,
            .controls = {
                ControlSpec{
                    .hash = 10u,
                    .positions = {{.bp = 0u, .strand_one = false}},
                },
                ControlSpec{
                    .hash = 20u,
                    .positions = {
                        {.bp = 0u, .strand_one = false},
                        {.bp = 7u, .strand_one = true},
                    },
                },
            },
        },
        FragmentSpec{
            .position_segment = 2u,
            .controls = {
                ControlSpec{
                    .hash = 20u,
                    .positions = {{.bp = 0u, .strand_one = false}},
                },
                ControlSpec{
                    .hash = 250u,
                    .positions = {{.bp = 7u, .strand_one = true}},
                },
            },
        },
    };
    const Oracle oracle = make_oracle(segment_span, fragments);
    auto fixture = make_format5_fixture(
        40u,
        segment_span,
        10u,
        idx::kLegacyMinimapV1SchemeIdentity,
        idx::MinimizerProducerBackendIdentity{
            .id = idx::MinimizerProducerBackendId::scalar,
            .version = idx::kMinimizerProducerBackendIdentityVersion,
        },
        5u,
        fragments);
    auto adapter = idx::Format5CompactH32Adapter::open(
        idx::Format5CompactH32OpenedGenerationView{
            .head = &fixture.head,
            .manifest = &fixture.manifest,
            .directory = &fixture.directory,
            .mapped_payload = fixture.payload,
        },
        idx::Format5CompactH32AdapterOptions{
            .buffered_run_position_cap = 3u,
            .emission_chunk_positions = 2u,
        });
    REQUIRE(adapter.has_value());
    REQUIRE(adapter->ready());
    const auto build_options = adapter->compact_build_options(3u);
    REQUIRE(build_options.has_value());
    CHECK(build_options->geometry.hash_domain_bits == 10u);
    CHECK(build_options->geometry.max_hash == 1'023u);

    auto source = adapter->sorted_run_source();
    const auto image = idx::build_compact_h32_direct_index(
        source, *build_options);
    REQUIRE(image.has_value());
    const auto view = idx::validate_compact_h32_direct_index(
        *image,
        build_options->identity,
        build_options->max_occ_cap,
        build_options->geometry);
    REQUIRE(view.has_value());
    CHECK(view->header.posting_width_bytes == 4u);
    CHECK(view->header.source_hash_count == oracle.size());
    CHECK(view->header.accepted_hash_count == 3u);
    CHECK(view->header.rejected_hash_count == 1u);

    const std::array<std::uint32_t, 5u> query_hashes{
        5u, 10u, 11u, 20u, 250u};
    const auto query = idx::query_compact_h32_direct_index_unique(
        *view, query_hashes, 3u);
    REQUIRE(query.has_value());
    REQUIRE(query->selections.size() == 3u);
    CHECK(query->evidence.rejected_posting_reads == 0u);
    CHECK(query->selections[0].query_ordinal == 0u);
    CHECK(query->selections[1].query_ordinal == 1u);
    CHECK(query->selections[2].query_ordinal == 4u);
    CHECK(query->selections[0].dense_ordinal == 0u);
    CHECK(query->selections[1].dense_ordinal == 1u);
    CHECK(query->selections[2].dense_ordinal == 2u);

    CollectedPostings collected{
        .by_selection = std::vector<std::vector<std::uint64_t>>(
            query->selections.size()),
    };
    const auto visited = idx::visit_compact_h32_direct_index_postings(
        *view, *query, &collected, &collect_posting);
    REQUIRE(visited.has_value());
    for (std::size_t selection = 0u;
         selection < query->selections.size(); ++selection) {
        const std::uint32_t query_ordinal =
            query->selections[selection].query_ordinal;
        REQUIRE(query_ordinal < query_hashes.size());
        const std::uint32_t hash = query_hashes[query_ordinal];
        REQUIRE(oracle.contains(hash));
        CHECK(collected.by_selection[selection] == oracle.at(hash));
    }
    CHECK_FALSE(std::ranges::any_of(
        query->selections,
        [&query_hashes](const auto& selection) {
            return selection.query_ordinal < query_hashes.size()
                && query_hashes[selection.query_ordinal] == 20u;
        }));

    const auto& estimate = adapter->prebuild_estimate();
    const auto& evidence = adapter->scan_evidence();
    CHECK(estimate.compact_prebuild_source_scan_count == 2u);
    CHECK(estimate.nonempty_fragment_count == 3u);
    CHECK(estimate.directory_entry_count == 4u);
    CHECK(estimate.fragment_controls_per_source_scan
        == fixture.manifest.control_count);
    CHECK(estimate.prebuild_fragment_controls_read
        == fixture.manifest.control_count * 2u);
    CHECK(estimate.owned_target_control_or_position_bytes == 0u);
    CHECK(evidence.source_scan_attempts == 2u);
    CHECK(evidence.source_scans_completed == 2u);
    CHECK(evidence.fragment_controls_read
        == estimate.prebuild_fragment_controls_read);
    CHECK(evidence.control_scalar_bytes_read
        == estimate.prebuild_control_scalar_bytes_read);
    CHECK(evidence.position_scalars_read == 40u);
    CHECK(evidence.positions_emitted
        == fixture.manifest.position_count * 2u);
    CHECK(evidence.replayed_heavy_runs == 2u);
    CHECK(evidence.peak_buffered_run_positions == 3u);
    CHECK(evidence.receiver_position_chunks
        > evidence.run_headers_emitted);
}

TEST_CASE(
    "Format-5 compact-H32 adapter clamps huge scratch requests to a tiny "
    "authenticated generation and keeps issued sources valid across moves",
    "[doc0383][p5][format5][compact-h32][accounting][lifetime]") {
    const std::vector<FragmentSpec> fragments{
        FragmentSpec{
            .position_segment = 0u,
            .controls = {
                ControlSpec{
                    .hash = 7u,
                    .positions = {{.bp = 0u, .strand_one = false}},
                },
            },
        },
    };
    auto fixture = make_format5_fixture(
        1u,
        16u,
        10u,
        idx::kLegacyMinimapV1SchemeIdentity,
        idx::MinimizerProducerBackendIdentity{
            .id = idx::MinimizerProducerBackendId::scalar,
            .version = idx::kMinimizerProducerBackendIdentityVersion,
        },
        5u,
        fragments);
    auto adapter = idx::Format5CompactH32Adapter::open(
        idx::Format5CompactH32OpenedGenerationView{
            .head = &fixture.head,
            .manifest = &fixture.manifest,
            .directory = &fixture.directory,
            .mapped_payload = fixture.payload,
        },
        idx::Format5CompactH32AdapterOptions{
            .buffered_run_position_cap =
                (std::numeric_limits<std::uint64_t>::max)(),
            .emission_chunk_positions =
                (std::numeric_limits<std::size_t>::max)(),
        });
    REQUIRE(adapter.has_value());
    CHECK(adapter->prebuild_estimate()
        .effective_buffered_run_position_cap == 1u);
    CHECK(adapter->prebuild_estimate()
        .effective_emission_chunk_position_capacity == 1u);
    CHECK(adapter->prebuild_estimate()
        .maximum_buffered_run_scratch_bytes
            == sizeof(std::uint64_t));
    CHECK(adapter->prebuild_estimate().emission_chunk_scratch_bytes
        == sizeof(std::uint64_t));

    // The source is issued before the adapter move. Its heap-stable token must
    // follow the destination adapter and support both deterministic replays.
    auto source = adapter->sorted_run_source();
    auto moved_adapter = std::move(*adapter);
    CapturedRuns captured;
    const idx::CompactH32SortedRunReceiver receiver{
        .context = &captured,
        .begin_run_fn = &capture_begin,
        .consume_positions_fn = &capture_positions,
    };
    REQUIRE(source.scan_sorted_runs(receiver).has_value());
    REQUIRE(source.scan_sorted_runs(receiver).has_value());
    REQUIRE(captured.headers.size() == 2u);
    REQUIRE(captured.positions.size() == 2u);
    CHECK(captured.positions[0] == std::vector<std::uint64_t>{0u});
    CHECK(captured.positions[1] == std::vector<std::uint64_t>{0u});
    CHECK(moved_adapter.scan_evidence().source_scans_completed == 2u);
    CHECK(moved_adapter.scan_evidence().receiver_position_chunks == 2u);
    CHECK(moved_adapter.scan_evidence().peak_buffered_run_positions == 1u);
}

TEST_CASE(
    "Format-5 compact-H32 adapter preserves full-u32 hashes and global P64 "
    "positions across a fragment base",
    "[doc0383][p5][format5][compact-h32][h32][p64]") {
    constexpr std::uint64_t segment_span = std::uint64_t{1u} << 31u;
    constexpr std::uint64_t target_length = segment_span + 100u;
    constexpr std::uint32_t full_hash = 0xf1234567u;
    const std::vector<FragmentSpec> fragments{
        FragmentSpec{
            .position_segment = 1u,
            .controls = {
                ControlSpec{
                    .hash = full_hash,
                    .positions = {{.bp = 7u, .strand_one = true}},
                },
            },
        },
    };
    auto fixture = make_format5_fixture(
        target_length,
        segment_span,
        10u,
        idx::kPaperNtHashU32R7SumV1SchemeIdentity,
        idx::MinimizerProducerBackendIdentity{
            .id = idx::MinimizerProducerBackendId::paper_nthash_simd,
            .version = idx::kMinimizerProducerBackendIdentityVersion,
        },
        15u,
        fragments);
    auto adapter = idx::Format5CompactH32Adapter::open(
        idx::Format5CompactH32OpenedGenerationView{
            .head = &fixture.head,
            .manifest = &fixture.manifest,
            .directory = &fixture.directory,
            .mapped_payload = fixture.payload,
        },
        idx::Format5CompactH32AdapterOptions{
            .buffered_run_position_cap = 10u,
            .emission_chunk_positions = 1u,
        });
    REQUIRE(adapter.has_value());
    CHECK(adapter->geometry().hash_domain_bits == 32u);
    CHECK(adapter->geometry().max_hash
        == (std::numeric_limits<std::uint32_t>::max)());

    CapturedRuns captured;
    const idx::CompactH32SortedRunReceiver receiver{
        .context = &captured,
        .begin_run_fn = &capture_begin,
        .consume_positions_fn = &capture_positions,
    };
    auto source = adapter->sorted_run_source();
    const auto scanned = source.scan_sorted_runs(receiver);
    REQUIRE(scanned.has_value());
    REQUIRE(captured.headers.size() == 1u);
    REQUIRE(captured.positions.size() == 1u);
    CHECK(captured.headers.front().hash == full_hash);
    CHECK(captured.headers.front().global_count == 1u);
    CHECK(captured.headers.front().target_strand_zero_count == 0u);
    const std::uint64_t expected_position =
        ((segment_span + 7u) << 1u) | 1u;
    CHECK(captured.positions.front()
        == std::vector<std::uint64_t>{expected_position});
    CHECK(expected_position
        > (std::numeric_limits<std::uint32_t>::max)());

    const auto build_options = adapter->compact_build_options(10u);
    REQUIRE(build_options.has_value());
    const auto plan = idx::plan_compact_h32_direct_index(
        source, *build_options);
    REQUIRE(plan.has_value());
    CHECK(plan->posting_width_bytes == 8u);
    CHECK(plan->accepted_hash_count == 1u);
    CHECK(plan->accepted_posting_count == 1u);
    CHECK(adapter->scan_evidence().source_scans_completed == 2u);
}
