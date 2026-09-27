/**
 * @file test_support.cppm
 * @brief Builds direct-index fixtures and checks their bytes and query results independently.
 */
module;

#include <cstring>
#include <catch2/catch_test_macros.hpp>

export module bioseq.tests.unit.indexing.compact_hash32_direct_index_support;

import std;
import bioseq.indexing.compact_hash32_direct_index;

export namespace bioseq::tests::unit::indexing::compact_hash32_direct_index_test_support {

namespace idx = bioseq::indexing;

/// Builds a test index identity with fixed generation and source IDs.
[[nodiscard]] constexpr idx::direct_index_identity identity(std::uint64_t target_length) noexcept {
    return idx::direct_index_identity{
        .generation_id = 0x1112131415161718ULL,
        .source_identity = 0x2122232425262728ULL,
        .storage_contract_checksum = 0x3132333435363738ULL,
        .target_length = target_length,
    };
}

/// Builds direct-index settings for a target length, fragment size and optional bucket count.
[[nodiscard]] idx::direct_index_build_options options(std::uint64_t target_length,
                                                      std::uint64_t fragment_span = 0u,
                                                      std::uint64_t bucket_override = 0u) noexcept {
    return idx::direct_index_build_options{
        .identity = identity(target_length),
        .fragment_span_bp = fragment_span,
        .bucket_count_override = bucket_override,
    };
}

/// Reads an unsigned little-endian value without using the production decoder.
template <class p_unsigned_integer>
    requires std::is_unsigned_v<p_unsigned_integer>
[[nodiscard]] constexpr p_unsigned_integer reference_read_le(std::span<const std::byte> input,
                                                             std::size_t offset) noexcept {
    p_unsigned_integer value = 0u;
    for (std::size_t byte = 0u; byte < sizeof(p_unsigned_integer); ++byte) {
        value |=
            static_cast<p_unsigned_integer>(std::to_integer<std::uint8_t>(input[offset + byte]))
            << (byte * 8u);
    }
    return value;
}

/// Writes an unsigned little-endian value without using the production encoder.
template <class p_unsigned_integer>
    requires std::is_unsigned_v<p_unsigned_integer>
constexpr void reference_write_le(std::span<std::byte> output,
                                  std::size_t offset,
                                  p_unsigned_integer value) noexcept {
    for (std::size_t byte = 0u; byte < sizeof(p_unsigned_integer); ++byte) {
        output[offset + byte] = static_cast<std::byte>(value & p_unsigned_integer{0xffu});
        value >>= 8u;
    }
}

/// Computes CRC-64/ECMA-182 one bit at a time as a reference for the table-based codec.
[[nodiscard]] constexpr std::uint64_t reference_crc64(std::span<const std::byte> bytes) noexcept {
    constexpr std::uint64_t polynomial = 0x42F0E1EBA9EA3693ULL;
    std::uint64_t checksum = 0u;
    for (const std::byte byte : bytes) {
        checksum ^= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(byte)) << 56u;
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            checksum = (checksum & (std::uint64_t{1u} << 63u)) != 0u ? (checksum << 1u) ^ polynomial
                                                                     : checksum << 1u;
        }
    }
    return checksum;
}

/// Recomputes the direct-index header checksum after a test changes its bytes.
void reference_reseal_header(std::vector<std::byte>& image) {
    std::vector<std::byte> header(image.begin(),
                                  image.begin() + idx::k_direct_inverted_index_header_bytes);
    std::fill_n(header.begin() +
                    static_cast<std::ptrdiff_t>(idx::k_direct_inverted_index_header_crc_offset),
                idx::k_direct_inverted_index_checksum_bytes,
                std::byte{0});
    reference_write_le<std::uint64_t>(
        image, idx::k_direct_inverted_index_header_crc_offset, reference_crc64(header));
}

/// Recomputes the compact-index header checksum after a test changes its bytes.
void reference_reseal_compact_hash32_header(std::vector<std::byte>& image) {
    REQUIRE(image.size() >= idx::k_compact_hash32_direct_index_header_bytes);
    std::array<std::byte, idx::k_compact_hash32_direct_index_header_bytes> header{};
    std::copy_n(image.begin(), header.size(), header.begin());
    std::fill_n(header.begin() +
                    static_cast<std::ptrdiff_t>(idx::k_compact_hash32_header_crc_offset),
                idx::k_direct_inverted_index_checksum_bytes,
                std::byte{0});
    reference_write_le<std::uint64_t>(
        image, idx::k_compact_hash32_header_crc_offset, reference_crc64(header));
}

/// Recomputes the control-section checksum, then the header checksum that contains it.
void reference_reseal_controls(std::vector<std::byte>& image) {
    const std::uint64_t offset = reference_read_le<std::uint64_t>(image, 112u);
    const std::uint64_t bytes = reference_read_le<std::uint64_t>(image, 120u);
    reference_write_le<std::uint64_t>(
        image,
        idx::k_direct_inverted_index_controls_crc_offset,
        reference_crc64(std::span<const std::byte>{image}.subspan(
            static_cast<std::size_t>(offset), static_cast<std::size_t>(bytes))));
    reference_reseal_header(image);
}

/// Reference occurrences grouped by hash for query comparisons.
using oracle_type = std::map<std::uint32_t, std::vector<idx::direct_index_occurrence>>;

/// Groups occurrences by hash, keeping duplicates.
[[nodiscard]] oracle_type make_oracle(std::span<const idx::direct_index_occurrence> input) {
    oracle_type oracle;
    for (const auto& occurrence : input) {
        oracle[occurrence.hash].push_back(occurrence);
    }
    return oracle;
}

/// Counts occurrences on target strand zero.
[[nodiscard]] std::uint64_t
oracle_strand_zero_count(const std::vector<idx::direct_index_occurrence>& occurrences) noexcept {
    return static_cast<std::uint64_t>(
        std::count_if(occurrences.begin(), occurrences.end(), [](const auto& occurrence) {
            return !occurrence.target_strand_one;
        }));
}

/// Stores positions received for each selected target hash and counts callbacks.
struct collected_postings {
    std::vector<std::vector<std::uint64_t>> by_selection{};
    std::uint64_t callback_count = 0u;
};

/// Saves one position and checks that its strand bit matches the decoded strand.
bool collect_posting(void* opaque, const idx::direct_index_posting& posting) {
    auto& collected = *static_cast<collected_postings*>(opaque);
    if (posting.selection_ordinal >= collected.by_selection.size()) {
        return false;
    }
    collected.by_selection[posting.selection_ordinal].push_back(posting.encoded_position);
    ++collected.callback_count;
    return posting.target_strand_one == ((posting.encoded_position & 1u) != 0u);
}

/// Encode expected postings in fragment, strand, and position order.
[[nodiscard]] std::vector<std::uint64_t>
expected_postings(std::vector<idx::direct_index_occurrence> occurrences,
                  std::uint64_t fragment_span) {
    std::sort(occurrences.begin(),
              occurrences.end(),
              [fragment_span](const auto& left, const auto& right) {
                  const std::uint64_t left_fragment =
                      fragment_span == 0u ? 0u : left.target_bp / fragment_span;
                  const std::uint64_t right_fragment =
                      fragment_span == 0u ? 0u : right.target_bp / fragment_span;
                  return std::tuple{left_fragment, left.target_strand_one, left.target_bp} <
                         std::tuple{right_fragment, right.target_strand_one, right.target_bp};
              });
    std::vector<std::uint64_t> result;
    result.reserve(occurrences.size());
    for (const auto& occurrence : occurrences) {
        result.push_back((occurrence.target_bp << 1u) |
                         static_cast<std::uint64_t>(occurrence.target_strand_one));
    }
    return result;
}

/// Builds a direct-index byte image, failing the test if construction fails.
[[nodiscard]] std::vector<std::byte>
build_or_fail(std::span<const idx::direct_index_occurrence> input,
              const idx::direct_index_build_options& build_options) {
    auto image = idx::build_direct_inverted_index(input, build_options);
    REQUIRE(image.has_value());
    return std::move(*image);
}

/// Validates a direct-index image and returns a view borrowing its bytes.
[[nodiscard]] idx::direct_inverted_index_view
validate_or_fail(const std::vector<std::byte>& image,
                 const idx::direct_index_identity& expected_identity) {
    auto view = idx::validate_direct_inverted_index(image, expected_identity);
    REQUIRE(view.has_value());
    return *view;
}

/// Checks planned counts, strand totals and occurrence-limit decisions against the reference map.
void require_matches_oracle(const idx::direct_index_query_plan& plan,
                            std::span<const idx::direct_index_query_occurrence> queries,
                            const oracle_type& oracle,
                            std::uint64_t max_occ) {
    REQUIRE(plan.results.size() == queries.size());
    for (std::size_t ordinal = 0u; ordinal < queries.size(); ++ordinal) {
        const auto found = oracle.find(queries[ordinal].hash);
        const auto& result = plan.results[ordinal];
        if (found == oracle.end()) {
            CHECK_FALSE(result.present);
            CHECK_FALSE(result.positions_selected);
            CHECK(result.global_count == 0u);
            CHECK(result.forward_count == 0u);
            CHECK(result.reverse_count == 0u);
            continue;
        }
        const std::uint64_t total = found->second.size();
        const std::uint64_t strand_zero = oracle_strand_zero_count(found->second);
        CHECK(result.present);
        CHECK(result.global_count == total);
        CHECK(result.forward_count ==
              (queries[ordinal].query_strand_one ? total - strand_zero : strand_zero));
        CHECK(result.reverse_count == total - result.forward_count);
        CHECK(result.positions_selected == (total <= max_occ));
        CHECK((result.positions_selected
                   ? result.selection_ordinal != idx::k_direct_inverted_index_no_selection
                   : result.selection_ordinal == idx::k_direct_inverted_index_no_selection));
    }
}

/// One hash run with its header and encoded target positions.
struct compact_test_run {
    idx::compact_hash32_sorted_run_header header{};
    std::vector<std::uint64_t> encoded_positions{};
};

/// Holds the first scan's runs and optional changed runs for replay tests.
struct compact_test_source {
    std::vector<compact_test_run> runs{};
    std::vector<compact_test_run> replay_runs{};
    bool use_replay_runs_after_first_scan = false;
    std::uint64_t scans = 0u;
};

/// Sends each fixture run to the receiver in two batches, using changed data on replay if requested.
[[nodiscard]] std::expected<void, idx::direct_inverted_index_error>
scan_compact_test_runs(void* opaque, const idx::compact_hash32_sorted_run_receiver& receiver) {
    auto& source = *static_cast<compact_test_source*>(opaque);
    ++source.scans;
    const auto& runs = source.use_replay_runs_after_first_scan && source.scans > 1u
                           ? source.replay_runs
                           : source.runs;
    for (const compact_test_run& run : runs) {
        if (auto begun = receiver.begin_run(run.header); !begun) {
            return std::unexpected(begun.error());
        }
        const std::size_t split = run.encoded_positions.size() / 2u;
        if (auto first = receiver.consume_positions(
                std::span<const std::uint64_t>{run.encoded_positions}.first(split));
            !first) {
            return std::unexpected(first.error());
        }
        if (auto second = receiver.consume_positions(
                std::span<const std::uint64_t>{run.encoded_positions}.subspan(split));
            !second) {
            return std::unexpected(second.error());
        }
    }
    return {};
}

/// Connects a test source object to the compact-index scan callback.
[[nodiscard]] idx::compact_hash32_sorted_run_source
compact_source_ref(compact_test_source& source) noexcept {
    return idx::compact_hash32_sorted_run_source{
        .context = &source,
        .scan_sorted_runs_fn = &scan_compact_test_runs,
    };
}

/// Defines the hash range represented by the requested number of bits.
[[nodiscard]] constexpr idx::compact_hash32_geometry compact_geometry(std::uint8_t bits) noexcept {
    return idx::compact_hash32_geometry{
        .hash_domain_bits = bits,
        .max_hash = bits == 32u ? (std::numeric_limits<std::uint32_t>::max)()
                                : static_cast<std::uint32_t>((std::uint64_t{1u} << bits) - 1u),
    };
}

/// Builds compact-index settings with a target length, occurrence cap and hash width.
[[nodiscard]] idx::compact_hash32_build_options compact_options(
    std::uint64_t target_length, std::uint32_t max_occ_cap, std::uint8_t hash_bits = 9u) noexcept {
    return idx::compact_hash32_build_options{
        .identity = identity(target_length),
        .max_occ_cap = max_occ_cap,
        .geometry = compact_geometry(hash_bits),
    };
}

/// Copies expected identity, occurrence cap and hash width into the header-open settings.
[[nodiscard]] idx::compact_hash32_bounded_open_options compact_bounded_options(
    const idx::compact_hash32_build_options& build_options,
    std::optional<std::uint64_t> expected_direct_header_crc64 = std::nullopt) noexcept {
    return idx::compact_hash32_bounded_open_options{
        .expected_identity = build_options.identity,
        .expected_max_occ_cap = build_options.max_occ_cap,
        .expected_geometry = build_options.geometry,
        .expected_direct_header_crc64 = expected_direct_header_crc64,
    };
}

/// Packs the target position with the strand in the low bit.
[[nodiscard]] constexpr std::uint64_t encoded_position(std::uint64_t bp, bool strand_one) noexcept {
    return (bp << 1u) | static_cast<std::uint64_t>(strand_one);
}

/// Builds a compact-index byte image, failing the test on an error.
[[nodiscard]] std::vector<std::byte>
build_compact_or_fail(compact_test_source& source,
                      const idx::compact_hash32_build_options& build_options) {
    auto image = idx::build_compact_hash32_direct_index(compact_source_ref(source), build_options);
    REQUIRE(image.has_value());
    return std::move(*image);
}

/// Validates a compact-index image and returns a view borrowing its bytes.
[[nodiscard]] idx::compact_hash32_direct_index_view
validate_compact_or_fail(const std::vector<std::byte>& image,
                         const idx::compact_hash32_build_options& build_options) {
    auto view = idx::validate_compact_hash32_direct_index(
        image, build_options.identity, build_options.max_occ_cap, build_options.geometry);
    REQUIRE(view.has_value());
    return *view;
}

/// Recomputes all compact payload checksums and then the header checksum.
void reference_reseal_compact_hash32_payload_and_header(std::vector<std::byte>& image) {
    const auto reseal_section = [&image](std::size_t crc_offset,
                                         std::size_t section_offset_field,
                                         std::size_t section_bytes_field) {
        const std::uint64_t offset = reference_read_le<std::uint64_t>(image, section_offset_field);
        const std::uint64_t bytes = reference_read_le<std::uint64_t>(image, section_bytes_field);
        REQUIRE(offset <= image.size());
        REQUIRE(bytes <= image.size() - offset);
        reference_write_le<std::uint64_t>(
            image,
            crc_offset,
            reference_crc64(std::span<const std::byte>{image}.subspan(
                static_cast<std::size_t>(offset), static_cast<std::size_t>(bytes))));
    };
    reseal_section(idx::k_compact_hash32_membership_crc_offset, 112u, 120u);
    reseal_section(idx::k_compact_hash32_rank_crc_offset, 128u, 136u);
    reseal_section(idx::k_compact_hash32_descriptors_crc_offset, 144u, 152u);
    reseal_section(idx::k_compact_hash32_postings_crc_offset, 160u, 168u);
    reference_reseal_compact_hash32_header(image);
}

/// Opens the compact header and keeps its backing byte vector alive through shared ownership.
[[nodiscard]] std::expected<idx::compact_hash32_bounded_open_view, idx::direct_inverted_index_error>
open_compact_bounded_owned(std::vector<std::byte> image,
                           const idx::compact_hash32_bounded_open_options& open_options) {
    auto owner = std::make_shared<std::vector<std::byte>>(std::move(image));
    const std::span<const std::byte> direct_section{owner->data(), owner->size()};
    std::shared_ptr<const void> mapped_owner = owner;
    return idx::open_compact_hash32_direct_index_bounded(
        std::move(mapped_owner), direct_section, open_options);
}

} // namespace bioseq::tests::unit::indexing::compact_hash32_direct_index_test_support
