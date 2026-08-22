/**
 * @file minimizer_fragment_compact_hash32_adapter_test.cpp
 * @brief Tests compact-hash32 adaptation from persisted minimizer fragments.
 */

#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.indexing.minimizer_fragment_compact_hash32_adapter;
import bioseq.indexing.storage;
import bioseq.indexing.type;

namespace {

namespace idx = bioseq::indexing;

/// Encode one unsigned fixture scalar in little-endian order.
template <class p_unsigned_integer>
requires std::is_unsigned_v<p_unsigned_integer>
void write_le(
        std::span<std::byte> output,
        std::size_t offset,
        p_unsigned_integer value) {
    REQUIRE(offset <= output.size());
    REQUIRE(sizeof(p_unsigned_integer) <= output.size() - offset);
    for (std::size_t byte = 0u; byte < sizeof(p_unsigned_integer); ++byte) {
        output[offset + byte] = static_cast<std::byte>(value & p_unsigned_integer{0xffu});
        value >>= 8u;
    }
}

/// Encode one scalar using the selected grouped-SoA width.
void write_shape_scalar(
        std::span<std::byte> output,
        std::size_t offset,
        idx::minimizer_scalar_width width,
        std::uint64_t value) {
    switch (width) {
    case idx::minimizer_scalar_width::u16:
        REQUIRE(value <= (std::numeric_limits<std::uint16_t>::max)());
        write_le<std::uint16_t>(
            output, offset, static_cast<std::uint16_t>(value));
        return;
    case idx::minimizer_scalar_width::u32:
        REQUIRE(value <= (std::numeric_limits<std::uint32_t>::max)());
        write_le<std::uint32_t>(
            output, offset, static_cast<std::uint32_t>(value));
        return;
    case idx::minimizer_scalar_width::u64:
        write_le<std::uint64_t>(output, offset, value);
        return;
    }
    FAIL("unsupported grouped-SoA scalar width");
}

/// Describe one segment-local position and strand.
struct local_position {
    std::uint64_t bp = 0u;
    bool strand_one = false;
};

/// Describe one hash and its sorted local positions.
struct control_spec {
    std::uint32_t hash = 0u;
    std::vector<local_position> positions{};
};

/// Describe all controls assigned to one position segment.
struct fragment_spec {
    std::uint32_t position_segment = 0u;
    std::vector<control_spec> controls{};
};

/// Own a valid grouped-SoA fixture consumed by the adapter.
struct minimizer_fragment_fixture {
    idx::minimizer_generation_head head{};
    idx::minimizer_generation_manifest manifest{};
    idx::grouped_soa_directory_view directory{};
    std::vector<std::byte> payload{};
};

/// Return the positive chunk count required for one encoded extent.
[[nodiscard]] std::uint32_t chunks_for(std::uint64_t bytes) {
    const std::uint64_t chunks = bytes
            / idx::k_grouped_soa_extent_payload_bytes
        + (bytes % idx::k_grouped_soa_extent_payload_bytes != 0u);
    REQUIRE(chunks > 0u);
    REQUIRE(chunks <= (std::numeric_limits<std::uint32_t>::max)());
    return static_cast<std::uint32_t>(chunks);
}

/// Encode grouped-SoA fragment rows from semantic fixture specifications.
[[nodiscard]] minimizer_fragment_fixture make_minimizer_fragment_fixture(
        std::uint64_t target_length,
        std::uint64_t segment_span,
        std::uint64_t max_occ_cap,
        idx::minimizer_scheme_identity scheme,
        idx::minimizer_producer_identity producer,
        std::uint16_t kmer_size,
        std::span<const fragment_spec> fragments) {
    const auto profile = idx::derive_grouped_soa_minimizer_profile(
        scheme,
        idx::k_minimizer_persisted_dna4_alphabet_code,
        kmer_size,
        segment_span,
        max_occ_cap);
    REQUIRE(profile.has_value());

    minimizer_fragment_fixture fixture;
    fixture.manifest = idx::minimizer_generation_manifest{
        .layout = idx::minimizer_generation_layout::
            grouped_soa_segment_local,
        .generation_id = 0x12345678u,
        .source_identity = 0x23456789u,
        .source_content_checksum = 0x3456789au,
        .target_length = target_length,
        .alphabet_code = idx::k_minimizer_persisted_dna4_alphabet_code,
        .storage_unit_byte_width = 1u,
        .kmer_size = kmer_size,
        .window_size = 2u,
        .gpu_abi_revision = profile->gpu_abi_revision,
        .position_segment_span_bp = segment_span,
        .segment_geometry_revision =
            idx::minimizer_generation_manifest::
                uniform_segment_geometry_revision,
        .minimizer_scheme_id = scheme.id,
        .minimizer_scheme_revision = scheme.revision,
        .producer_backend_id = producer.id,
        .producer_identity_revision = producer.revision,
        .hash_width = profile->hash_width,
        .position_width = profile->position_width,
        .count_width = profile->count_width,
        .offset_policy = profile->offset_policy,
        .control_layout = profile->control_layout,
        .count_semantics = profile->count_semantics,
        .position_semantics = profile->position_semantics,
        .max_hash = profile->max_hash,
        .max_occ_cap = profile->max_occ_cap,
    };
    fixture.manifest.position_segment_count = static_cast<std::uint32_t>(
        target_length / segment_span
        + (target_length % segment_span != 0u));

    std::vector<idx::grouped_soa_bucket_directory_entry> entries;
    std::vector<std::vector<std::byte>> control_images;
    std::vector<std::vector<std::byte>> position_images;
    for (const fragment_spec& fragment : fragments) {
        REQUIRE_FALSE(fragment.controls.empty());
        REQUIRE(fragment.position_segment
            < fixture.manifest.position_segment_count);
        std::uint64_t position_count = 0u;
        std::uint32_t previous_hash = 0u;
        bool have_previous_hash = false;
        for (const control_spec& control : fragment.controls) {
            REQUIRE_FALSE(control.positions.empty());
            REQUIRE_FALSE((have_previous_hash
                && control.hash <= previous_hash));
            previous_hash = control.hash;
            have_previous_hash = true;
            position_count += control.positions.size();
        }
        const idx::minimizer_scalar_width offset_width =
            idx::select_grouped_soa_offset_width(position_count);
        const auto layout = idx::plan_grouped_soa_control_plane(
            fragment.controls.size(),
            profile->hash_width,
            offset_width,
            profile->count_width);
        REQUIRE(layout.has_value());
        std::vector<std::byte> controls(
            static_cast<std::size_t>(layout->encoded_bytes), std::byte{0});
        std::vector<std::byte> positions(
            static_cast<std::size_t>(position_count)
                * static_cast<std::uint8_t>(
                    profile->position_width),
            std::byte{0});
        const std::size_t hash_width = static_cast<std::uint8_t>(
            profile->hash_width);
        const std::size_t prefix_width = static_cast<std::uint8_t>(
            offset_width);
        const std::size_t count_width = static_cast<std::uint8_t>(
            profile->count_width);
        const std::size_t position_width = static_cast<std::uint8_t>(
            profile->position_width);
        std::uint64_t prefix = 0u;
        write_shape_scalar(
            controls,
            static_cast<std::size_t>(layout->prefix_offset),
            offset_width,
            0u);
        for (std::size_t control_ordinal = 0u;
             control_ordinal < fragment.controls.size();
             ++control_ordinal) {
            const control_spec& control =
                fragment.controls[control_ordinal];
            write_shape_scalar(
                controls,
                control_ordinal * hash_width,
                profile->hash_width,
                control.hash);
            std::optional<std::uint64_t> previous_local{};
            for (const local_position& position : control.positions) {
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
                    profile->position_width,
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
                profile->count_width,
                std::min<std::uint64_t>(
                    control.positions.size(), max_occ_cap + 1u));
        }
        REQUIRE(prefix == position_count);
        const std::uint32_t partition =
            static_cast<std::uint32_t>(entries.size());
        entries.push_back(idx::grouped_soa_bucket_directory_entry{
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
    const auto directory = idx::grouped_soa_directory_view::decode(
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
    fixture.head = idx::minimizer_generation_head{
        .layout = fixture.manifest.layout,
        .generation_id = fixture.manifest.generation_id,
        .source_identity = fixture.manifest.source_identity,
        .storage_contract_checksum = 0x456789abu,
        .manifest_checksum = 0x56789abcu,
        .source_content_checksum =
            fixture.manifest.source_content_checksum,
    };
    return fixture;
}

/// Hash-to-global-position map used as the adapter oracle.
using oracle_type = std::map<std::uint32_t, std::vector<std::uint64_t>>;

/// Globalize fixture positions and group them by hash.
[[nodiscard]] oracle_type make_oracle(
        std::uint64_t segment_span,
        std::span<const fragment_spec> fragments) {
    oracle_type oracle;
    for (const fragment_spec& fragment : fragments) {
        const std::uint64_t base =
            static_cast<std::uint64_t>(fragment.position_segment)
            * segment_span;
        for (const control_spec& control : fragment.controls) {
            for (const local_position& position : control.positions) {
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

/// Own postings collected for each adapter selection.
struct collected_postings {
    std::vector<std::vector<std::uint64_t>> by_selection{};
    std::uint64_t callbacks = 0u;
};

/// Append one posting through the erased query callback.
bool collect_posting(
        void* opaque,
        const idx::direct_index_posting& posting) {
    auto& collected = *static_cast<collected_postings*>(opaque);
    if (posting.selection_ordinal >= collected.by_selection.size()) {
        return false;
    }
    collected.by_selection[posting.selection_ordinal].push_back(
        posting.encoded_position);
    ++collected.callbacks;
    return posting.target_strand_one
        == ((posting.encoded_position & 1u) != 0u);
}

/// Own sorted runs captured from the adapter source.
struct captured_runs {
    std::vector<idx::compact_hash32_sorted_run_header> headers{};
    std::vector<std::vector<std::uint64_t>> positions{};
};

/// Begin one captured adapter run.
[[nodiscard]] std::expected<void, idx::direct_inverted_index_error>
capture_begin(
        void* opaque,
        const idx::compact_hash32_sorted_run_header& header) {
    auto& captured = *static_cast<captured_runs*>(opaque);
    captured.headers.push_back(header);
    captured.positions.emplace_back();
    return {};
}

/// Append one positions batch to the active captured run.
[[nodiscard]] std::expected<void, idx::direct_inverted_index_error>
capture_positions(
        void* opaque,
        std::span<const std::uint64_t> positions) {
    auto& captured = *static_cast<captured_runs*>(opaque);
    if (captured.positions.empty()) {
        return std::unexpected(idx::direct_inverted_index_error::source_read);
    }
    captured.positions.back().insert(
        captured.positions.back().end(),
        positions.begin(), positions.end());
    return {};
}

} // namespace

TEST_CASE(
    "Minimizer-fragment compact-hash32 adapter globally merges fragment runs and matches "
    "a map oracle without a target-wide owner",
    "[component:bioseq][layer:unit][feature:index]") {
    constexpr std::uint64_t segment_span = 16u;
    const std::vector<fragment_spec> fragments{
        fragment_spec{
            .position_segment = 0u,
            .controls = {
                control_spec{
                    .hash = 5u,
                    .positions = {{.bp = 0u, .strand_one = false}},
                },
                control_spec{
                    .hash = 10u,
                    // Same bp on both strands is a real duplicate-bp case,
                    // while the two encoded grouped-SoA events remain distinct.
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
        fragment_spec{
            .position_segment = 0u,
            .controls = {
                control_spec{
                    .hash = 20u,
                    .positions = {
                        {.bp = 2u, .strand_one = false},
                        {.bp = 8u, .strand_one = true},
                    },
                },
            },
        },
        fragment_spec{
            .position_segment = 1u,
            .controls = {
                control_spec{
                    .hash = 10u,
                    .positions = {{.bp = 0u, .strand_one = false}},
                },
                control_spec{
                    .hash = 20u,
                    .positions = {
                        {.bp = 0u, .strand_one = false},
                        {.bp = 7u, .strand_one = true},
                    },
                },
            },
        },
        fragment_spec{
            .position_segment = 2u,
            .controls = {
                control_spec{
                    .hash = 20u,
                    .positions = {{.bp = 0u, .strand_one = false}},
                },
                control_spec{
                    .hash = 250u,
                    .positions = {{.bp = 7u, .strand_one = true}},
                },
            },
        },
    };
    const oracle_type oracle = make_oracle(segment_span, fragments);
    auto fixture = make_minimizer_fragment_fixture(
        40u,
        segment_span,
        10u,
        idx::k_minimap_scheme_identity,
        idx::minimizer_producer_identity{
            .id = idx::minimizer_producer_backend::scalar,
            .revision = idx::k_minimizer_producer_identity_revision,
        },
        5u,
        fragments);
    auto adapter = idx::minimizer_fragment_compact_hash32_adapter::open(
        idx::opened_minimizer_fragment_generation_view{
            .head = &fixture.head,
            .manifest = &fixture.manifest,
            .directory = &fixture.directory,
            .mapped_payload = fixture.payload,
        },
        idx::minimizer_fragment_compact_hash32_options{
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
    const auto image = idx::build_compact_hash32_direct_index(
        source, *build_options);
    REQUIRE(image.has_value());
    const auto view = idx::validate_compact_hash32_direct_index(
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
    const auto query = idx::query_compact_hash32_direct_index_unique(
        *view, query_hashes, 3u);
    REQUIRE(query.has_value());
    REQUIRE(query->selections.size() == 3u);
    CHECK(query->stats.rejected_posting_reads == 0u);
    CHECK(query->selections[0].query_ordinal == 0u);
    CHECK(query->selections[1].query_ordinal == 1u);
    CHECK(query->selections[2].query_ordinal == 4u);
    CHECK(query->selections[0].dense_ordinal == 0u);
    CHECK(query->selections[1].dense_ordinal == 1u);
    CHECK(query->selections[2].dense_ordinal == 2u);

    collected_postings collected{
        .by_selection = std::vector<std::vector<std::uint64_t>>(
            query->selections.size()),
    };
    const auto visited = idx::visit_compact_hash32_direct_index_postings(
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
    const auto& stats = adapter->scan_stats();
    CHECK(estimate.compact_prebuild_source_scan_count == 2u);
    CHECK(estimate.nonempty_fragment_count == 3u);
    CHECK(estimate.directory_entry_count == 4u);
    CHECK(estimate.fragment_controls_per_source_scan
        == fixture.manifest.control_count);
    CHECK(estimate.prebuild_fragment_controls_read
        == fixture.manifest.control_count * 2u);
    CHECK(estimate.owned_target_control_or_position_bytes == 0u);
    CHECK(stats.source_scan_attempts == 2u);
    CHECK(stats.source_scans_completed == 2u);
    CHECK(stats.fragment_controls_read
        == estimate.prebuild_fragment_controls_read);
    CHECK(stats.control_scalar_bytes_read
        == estimate.prebuild_control_scalar_bytes_read);
    CHECK(stats.position_scalars_read == 40u);
    CHECK(stats.positions_emitted
        == fixture.manifest.position_count * 2u);
    CHECK(stats.replayed_heavy_runs == 2u);
    CHECK(stats.peak_buffered_run_positions == 3u);
    CHECK(stats.receiver_position_chunks
        > stats.run_headers_emitted);
}

TEST_CASE(
    "Minimizer-fragment compact-hash32 adapter clamps huge scratch requests to a tiny "
    "authenticated generation and keeps issued sources valid across moves",
    "[component:bioseq][layer:unit][feature:index]") {
    const std::vector<fragment_spec> fragments{
        fragment_spec{
            .position_segment = 0u,
            .controls = {
                control_spec{
                    .hash = 7u,
                    .positions = {{.bp = 0u, .strand_one = false}},
                },
            },
        },
    };
    auto fixture = make_minimizer_fragment_fixture(
        1u,
        16u,
        10u,
        idx::k_minimap_scheme_identity,
        idx::minimizer_producer_identity{
            .id = idx::minimizer_producer_backend::scalar,
            .revision = idx::k_minimizer_producer_identity_revision,
        },
        5u,
        fragments);
    auto adapter = idx::minimizer_fragment_compact_hash32_adapter::open(
        idx::opened_minimizer_fragment_generation_view{
            .head = &fixture.head,
            .manifest = &fixture.manifest,
            .directory = &fixture.directory,
            .mapped_payload = fixture.payload,
        },
        idx::minimizer_fragment_compact_hash32_options{
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
    captured_runs captured;
    const idx::compact_hash32_sorted_run_receiver receiver{
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
    CHECK(moved_adapter.scan_stats().source_scans_completed == 2u);
    CHECK(moved_adapter.scan_stats().receiver_position_chunks == 2u);
    CHECK(moved_adapter.scan_stats().peak_buffered_run_positions == 1u);
}

TEST_CASE(
    "Minimizer-fragment compact-hash32 adapter preserves full-u32 hashes and global P64 "
    "positions across a fragment base",
    "[component:bioseq][layer:unit][feature:index]") {
    constexpr std::uint64_t segment_span = std::uint64_t{1u} << 31u;
    constexpr std::uint64_t target_length = segment_span + 100u;
    constexpr std::uint32_t full_hash = 0xf1234567u;
    const std::vector<fragment_spec> fragments{
        fragment_spec{
            .position_segment = 1u,
            .controls = {
                control_spec{
                    .hash = full_hash,
                    .positions = {{.bp = 7u, .strand_one = true}},
                },
            },
        },
    };
    auto fixture = make_minimizer_fragment_fixture(
        target_length,
        segment_span,
        10u,
        idx::k_bounded_nthash_u32_r7_sum_scheme_identity,
        idx::minimizer_producer_identity{
            .id = idx::minimizer_producer_backend::bounded_nthash_simd,
            .revision = idx::k_minimizer_producer_identity_revision,
        },
        15u,
        fragments);
    auto adapter = idx::minimizer_fragment_compact_hash32_adapter::open(
        idx::opened_minimizer_fragment_generation_view{
            .head = &fixture.head,
            .manifest = &fixture.manifest,
            .directory = &fixture.directory,
            .mapped_payload = fixture.payload,
        },
        idx::minimizer_fragment_compact_hash32_options{
            .buffered_run_position_cap = 10u,
            .emission_chunk_positions = 1u,
        });
    REQUIRE(adapter.has_value());
    CHECK(adapter->geometry().hash_domain_bits == 32u);
    CHECK(adapter->geometry().max_hash
        == (std::numeric_limits<std::uint32_t>::max)());

    captured_runs captured;
    const idx::compact_hash32_sorted_run_receiver receiver{
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
    const auto plan = idx::plan_compact_hash32_direct_index(
        source, *build_options);
    REQUIRE(plan.has_value());
    CHECK(plan->posting_width_bytes == 8u);
    CHECK(plan->accepted_hash_count == 1u);
    CHECK(plan->accepted_posting_count == 1u);
    CHECK(adapter->scan_stats().source_scans_completed == 2u);
}
