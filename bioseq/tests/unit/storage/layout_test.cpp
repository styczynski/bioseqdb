/**
 * @file
 * @brief Check sequence-size buckets, stored locator bytes, and fragment boundaries.
 */

#include <cstring>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.storage.layout;

namespace {

using bioseq::storage::make_sequence_bucket_bit_thresholds;
using bioseq::storage::sequence_locator;
using bioseq::storage::sequence_size_bit_width;

/// Upper bit-width limit of the first default bucket.
constexpr std::size_t minimum_bits = 8;
/// First bit width placed in the final default bucket.
constexpr std::size_t maximum_bits = 42;

/**
 * @brief Find  the default storage bucket for a sequence length
 * @param[in] size Number of symbols.
 * @return Zero-based bucket index.
 */
[[nodiscard]] std::size_t bucket_of(std::uint64_t size) {
    return bioseq::storage::sequence_bucket_index<std::uint64_t, minimum_bits, maximum_bits>(size);
}

/**
 * @brief Calculate the expected default bucket independently of its threshold table.
 * @param[in] length_bits Width returned by sequence_size_bit_width.
 * @return Expected bucket index.
 */
[[nodiscard]] std::size_t expected_bucket_for_bits(std::size_t length_bits) {
    if (length_bits <= 8u)
        return 0u; // The first bucket covers widths through 8 bits.
    if (length_bits <= 41u)
        return length_bits - 8u;
    return 34u; // The final bucket covers widths from 42 through 64 bits.
}

} // namespace

TEST_CASE("bucket threshold table has 35 entries with the canonical cut-points",
          "[component:bioseq][layer:unit][feature:storage]") {
    constexpr auto thresholds =
        make_sequence_bucket_bit_thresholds<std::uint64_t, minimum_bits, maximum_bits>();
    REQUIRE(thresholds.size() == 35u);
    REQUIRE(thresholds[0] == 8u); // First bucket.
    REQUIRE(thresholds[1] == 9u);
    REQUIRE(thresholds[33] == 41u); // Last bucket covering a single bit width.
    REQUIRE(thresholds[34] == 64u); // Final bucket covers widths 42 through 64.
    // Thresholds must increase strictly.
    for (std::size_t i = 1; i < thresholds.size(); ++i) {
        REQUIRE(thresholds[i] > thresholds[i - 1]);
    }
}

TEST_CASE("sequence_size_bit_width matches ceil-log2 boundary semantics",
          "[component:bioseq][layer:unit][feature:storage]") {
    // Use enough bits for  the last, zero-based position, with a miimum of one bit.
    REQUIRE(sequence_size_bit_width(0u) == 1u);
    REQUIRE(sequence_size_bit_width(1u) == 1u);
    REQUIRE(sequence_size_bit_width(2u) == 1u);
    REQUIRE(sequence_size_bit_width(3u) == 2u);
    REQUIRE(sequence_size_bit_width(256u) == 8u);
    REQUIRE(sequence_size_bit_width(257u) == 9u);
    REQUIRE(sequence_size_bit_width(std::uint64_t{1} << 41) == 41u);
}

TEST_CASE("sequence_bucket_index routes every L in [1, 63] to the expected bucket",
          "[component:bioseq][layer:unit][feature:storage]") {
    // Powers of two exercise each position width from 1 through 63 bits.
    for (std::size_t length_bits = 1; length_bits <= 63u; ++length_bits) {
        const std::uint64_t size = std::uint64_t{1} << length_bits;
        INFO("length_bits=" << length_bits << " size=" << size);
        REQUIRE(sequence_size_bit_width(size) == length_bits);
        REQUIRE(bucket_of(size) == expected_bucket_for_bits(length_bits));
    }
}

TEST_CASE("bucket boundaries 8/9 and 41/42 land in adjacent buckets",
          "[component:bioseq][layer:unit][feature:storage]") {
    // Check the transition out of the first bucket.
    REQUIRE(bucket_of(std::uint64_t{1} << 8) == 0u);
    REQUIRE(bucket_of(std::uint64_t{1} << 9) == 1u);
    // Check the transition into the final bucket.
    REQUIRE(bucket_of(std::uint64_t{1} << 41) == 33u);
    REQUIRE(bucket_of(std::uint64_t{1} << 42) == 34u);
    // The largest representable length belongs to the final bucket.
    REQUIRE(bucket_of(~std::uint64_t{0}) == 34u);
    // Empty and short sequences share the first bucket.
    REQUIRE(bucket_of(0u) == 0u);
    REQUIRE(bucket_of(1u) == 0u);
    REQUIRE(bucket_of(256u) == 0u);
}

TEST_CASE("every bucket index is addressable and below bucket_count",
          "[component:bioseq][layer:unit][feature:storage]") {
    constexpr std::size_t bucket_count =
        make_sequence_bucket_bit_thresholds<std::uint64_t, minimum_bits, maximum_bits>().size();
    for (std::size_t length_bits = 1; length_bits <= 63u; ++length_bits) {
        REQUIRE(bucket_of(std::uint64_t{1} << length_bits) < bucket_count);
    }
}

TEST_CASE("sequence_locator is a 40-byte standard-layout POD that round-trips through bytes",
          "[component:bioseq][layer:unit][feature:storage]") {
    static_assert(sizeof(sequence_locator) == 40u);
    static_assert(std::is_standard_layout_v<sequence_locator>);

    sequence_locator loc{};
    loc.payload_version = bioseq::storage::sequence_locator_payload_version;
    loc.bucket_index = 17;
    loc.shard_index = 0xDEADBEEFu;
    loc.fragment_count = 0x12345678u;
    loc.logical_length = 0x0123456789ABCDEFull;
    loc.first_fragment_row = 0xFEDCBA9876543210ull;
    loc.source_epoch = 0x1020304050607080ull;

    // Round-trip the locator through the bytes stored in a sequence stub.
    std::array<unsigned char, sizeof(sequence_locator)> bytes{};
    std::memcpy(bytes.data(), &loc, sizeof(loc));

    sequence_locator decoded{};
    std::memcpy(&decoded, bytes.data(), sizeof(decoded));

    REQUIRE(decoded.payload_version == loc.payload_version);
    REQUIRE(decoded.bucket_index == loc.bucket_index);
    REQUIRE(decoded.shard_index == loc.shard_index);
    REQUIRE(decoded.fragment_count == loc.fragment_count);
    REQUIRE(decoded.logical_length == loc.logical_length);
    REQUIRE(decoded.first_fragment_row == loc.first_fragment_row);
    REQUIRE(decoded.source_epoch == loc.source_epoch);

    // The stored locator reader depends on these field offsets.
    REQUIRE(offsetof(sequence_locator, fragment_count) == 8u);
    REQUIRE(offsetof(sequence_locator, logical_length) == 16u);
    REQUIRE(offsetof(sequence_locator, first_fragment_row) == 24u);
    REQUIRE(offsetof(sequence_locator, source_epoch) == 32u);
}

TEST_CASE("canonical locator payload codec validates exact framing",
          "[component:bioseq][layer:unit][feature:storage]") {
    sequence_locator locator{
        .payload_version = bioseq::storage::sequence_locator_payload_version,
        .bucket_index = (std::numeric_limits<std::uint8_t>::max)(),
        .shard_index = (std::numeric_limits<std::uint32_t>::max)(),
        .fragment_count = (std::numeric_limits<std::uint32_t>::max)(),
        .logical_length = (std::numeric_limits<std::uint64_t>::max)(),
        .first_fragment_row = (std::numeric_limits<std::uint64_t>::max)(),
        .source_epoch = 0u,
    };
    std::array<std::byte, sizeof(sequence_locator)> payload{};
    REQUIRE(bioseq::storage::encode_sequence_locator_payload(payload, locator));
    const auto decoded = bioseq::storage::decode_sequence_locator_payload(payload);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->bucket_index == locator.bucket_index);
    REQUIRE(decoded->shard_index == locator.shard_index);
    REQUIRE(decoded->fragment_count == locator.fragment_count);
    REQUIRE(decoded->logical_length == locator.logical_length);
    REQUIRE(decoded->first_fragment_row == locator.first_fragment_row);
    REQUIRE(decoded->source_epoch == 0u);

    REQUIRE_FALSE(bioseq::storage::encode_sequence_locator_payload(
        std::span<std::byte>(payload).first(payload.size() - 1u), locator));
    REQUIRE_FALSE(bioseq::storage::decode_sequence_locator_payload(
                      std::span<const std::byte>(payload).first(payload.size() - 1u))
                      .has_value());

    locator.payload_version = 2u;
    REQUIRE_FALSE(bioseq::storage::encode_sequence_locator_payload(payload, locator));
    payload[0] = std::byte{2u};
    REQUIRE_FALSE(bioseq::storage::decode_sequence_locator_payload(payload).has_value());

    locator.payload_version = bioseq::storage::sequence_locator_payload_version;
    locator.reserved_16 = 1u;
    REQUIRE_FALSE(bioseq::storage::encode_sequence_locator_payload(payload, locator));

    REQUIRE(bioseq::storage::is_sequence_locator_stub_width(
        bioseq::storage::sequence_locator_stub_width_marker));
    REQUIRE_FALSE(bioseq::storage::is_sequence_locator_stub_width(8u));
}

TEST_CASE("sequence locator validation rejects every unsafe narrowing and geometry",
          "[component:bioseq][layer:unit][feature:storage]") {
    using error = bioseq::storage::sequence_locator_validation_error;
    const auto validate = [](sequence_locator locator) noexcept {
        return bioseq::storage::validate_sequence_locator(locator);
    };
    const sequence_locator valid{
        .payload_version = bioseq::storage::sequence_locator_payload_version,
        .bucket_index = 1u,
        .shard_index = 2u,
        .fragment_count = 2u,
        .logical_length = 32'769u,
        .first_fragment_row = 17u,
        .source_epoch = 9u,
    };
    REQUIRE(validate(valid) == error::none);

    auto locator = valid;
    locator.payload_version = 2u;
    REQUIRE(validate(locator) == error::invalid_payload_metadata);
    locator = valid;
    locator.source_epoch = 0u;
    REQUIRE(validate(locator) == error::invalid_payload_metadata);
    locator.source_epoch =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1u;
    REQUIRE(validate(locator) == error::invalid_payload_metadata);

    locator = valid;
    locator.logical_length =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1u;
    REQUIRE(validate(locator) == error::logical_length_exceeds_sql_bigint);
    locator.logical_length =
        static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()) + 1u;
    REQUIRE(validate(locator) == error::logical_length_exceeds_flat_storage);
    locator.logical_length =
        static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)());
    REQUIRE(validate(locator) == error::none);

    locator = valid;
    locator.shard_index =
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()) + 1u;
    REQUIRE(validate(locator) == error::shard_index_exceeds_catalog);
    locator = valid;
    locator.first_fragment_row =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1u;
    REQUIRE(validate(locator) == error::first_fragment_row_exceeds_catalog);

    locator = valid;
    locator.fragment_count = 0u;
    REQUIRE(validate(locator) == error::inconsistent_fragment_geometry);
    locator = valid;
    locator.logical_length = 0u;
    REQUIRE(validate(locator) == error::inconsistent_fragment_geometry);
    locator = {};
    locator.source_epoch = 1u;
    locator.first_fragment_row = 1u;
    REQUIRE(validate(locator) == error::inconsistent_fragment_geometry);
    locator = valid;
    locator.first_fragment_row =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    REQUIRE(validate(locator) == error::fragment_row_range_exceeds_catalog);
}

TEST_CASE("fragment geometry validation is exact at unit and row boundaries",
          "[component:bioseq][layer:unit][feature:storage]") {
    using frame_error = bioseq::storage::fragment_frame_error;
    using metadata_error = bioseq::storage::fragment_metadata_error;

    REQUIRE(bioseq::storage::expected_sequence_fragment_count(0u, 16u, 1024u) == 0u);
    REQUIRE(bioseq::storage::expected_sequence_fragment_count(16'384u, 16u, 1024u) == 1u);
    REQUIRE(bioseq::storage::expected_sequence_fragment_count(16'385u, 16u, 1024u) == 2u);
    REQUIRE(bioseq::storage::expected_sequence_fragment_count(
                (std::numeric_limits<std::uint32_t>::max)(), 16u, 1024u) == 262'144u);
    REQUIRE(bioseq::storage::expected_sequence_fragment_count(1u, 0u, 1024u) ==
            (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE(bioseq::storage::expected_sequence_fragment_count(1u, 16u, 0u) ==
            (std::numeric_limits<std::uint64_t>::max)());

    REQUIRE(bioseq::storage::validate_fragment_frame(8'208u, 16u, 8'208u) == frame_error::none);
    REQUIRE(bioseq::storage::validate_fragment_frame(15u, 16u, 8'208u) ==
            frame_error::shorter_than_header);
    REQUIRE(bioseq::storage::validate_fragment_frame(8'207u, 16u, 8'208u) ==
            frame_error::size_mismatch);
    REQUIRE(bioseq::storage::validate_fragment_frame(8'209u, 16u, 8'208u) ==
            frame_error::size_mismatch);

    REQUIRE(bioseq::storage::validate_fragment_metadata(0u, 8u, 32'768u, 0u, 0u, 8u, 32'768u, 0u) ==
            metadata_error::none);
    REQUIRE(bioseq::storage::validate_fragment_metadata(1u, 8u, 32'768u, 0u, 0u, 8u, 32'768u, 0u) ==
            metadata_error::type_mismatch);
    REQUIRE(bioseq::storage::validate_fragment_metadata(0u, 4u, 32'768u, 0u, 0u, 8u, 32'768u, 0u) ==
            metadata_error::type_mismatch);
    REQUIRE(bioseq::storage::validate_fragment_metadata(0u, 8u, 32'767u, 0u, 0u, 8u, 32'768u, 0u) ==
            metadata_error::logical_length_mismatch);
    REQUIRE(bioseq::storage::validate_fragment_metadata(0u, 8u, 1u, 31u, 0u, 8u, 1u, 0u) ==
            metadata_error::padding_mismatch);
}
