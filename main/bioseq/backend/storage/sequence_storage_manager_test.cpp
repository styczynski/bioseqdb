// main/bioseq/backend/storage/sequence_storage_manager_test.cpp
//
// DOC0260 §12.1 — unit-level tests for the PG-free pieces of the storage
// manager: the length-bucket math (every L in [0, 64] lands in the expected
// bucket) and the locator round-trip. The catalog CRUD, shard rollover and
// per-scan cache hit-ratio paths are Postgres-dependent and are exercised by
// the §12.2 integration tests in integration_tests/test_storage_buckets.cpp;
// this TU links the no-Postgres test executable, so it imports only the
// bioseq_bucket_math module (the math + seq_locator POD factored out of
// bioseq_storage:sequence_storage_manager for exactly this reason).

#include "common_tests.h"
#include "common.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

import bioseq.storage.bucket_math;

namespace {

using bioseq::storage::seq_locator;
using bioseq::storage::generate_seq_buckets_len_thresholds;
using bioseq::storage::len_log2_for_size;

// Default alias parameters: sequence_size_t = uint64_t, [8, 42] bit window.
constexpr std::size_t kMin = 8;
constexpr std::size_t kMax = 42;

[[nodiscard]] std::size_t bucket_of(std::uint64_t size) {
    return bioseq::storage::bucket_index_for_size<std::uint64_t, kMin, kMax>(size);
}

// Reference bucketing, independent of the LUT under test: thresholds are
// [8, 9, 10, ..., 41, 64]; a length-log2 L maps to the first threshold >= L.
[[nodiscard]] std::size_t expected_bucket_for_L(std::size_t L) {
    if (L <= 8u)  return 0u;        // bucket 0 covers L in [0, 8]
    if (L <= 41u) return L - 8u;    // one bucket each for L in {9 .. 41}
    return 34u;                     // bucket 34 covers L in [42, 64]
}

}  // namespace

TEST_CASE("DOC0260 §4 — bucket threshold table is 35 entries with the documented cut-points",
          "[storage][buckets][unit]") {
    constexpr auto thresholds =
        generate_seq_buckets_len_thresholds<std::uint64_t, kMin, kMax>();
    REQUIRE(thresholds.size() == 35u);
    REQUIRE(thresholds[0]  == 8u);    // first bucket: everything up to 8 bits
    REQUIRE(thresholds[1]  == 9u);
    REQUIRE(thresholds[33] == 41u);   // last single-bit bucket
    REQUIRE(thresholds[34] == 64u);   // tail bucket: [42, 64] bits
    // Strictly increasing.
    for (std::size_t i = 1; i < thresholds.size(); ++i) {
        REQUIRE(thresholds[i] > thresholds[i - 1]);
    }
}

TEST_CASE("DOC0260 §4 — len_log2_for_size matches ceil-log2 boundary semantics",
          "[storage][buckets][unit]") {
    // need = (size > 1 ? size - 1 : 1); L = bit_width(need).
    REQUIRE(len_log2_for_size(0u)   == 1u);
    REQUIRE(len_log2_for_size(1u)   == 1u);
    REQUIRE(len_log2_for_size(2u)   == 1u);
    REQUIRE(len_log2_for_size(3u)   == 2u);
    REQUIRE(len_log2_for_size(256u) == 8u);
    REQUIRE(len_log2_for_size(257u) == 9u);
    REQUIRE(len_log2_for_size(std::uint64_t{1} << 41) == 41u);
}

TEST_CASE("DOC0260 §4 — bucket_index_for_size routes every L in [1, 63] to the expected bucket",
          "[storage][buckets][unit]") {
    // size = 2^L gives len_log2_for_size(2^L) = bit_width(2^L - 1) = L, so we
    // can sweep every reachable length-log2 by powers of two.
    for (std::size_t L = 1; L <= 63u; ++L) {
        const std::uint64_t size = std::uint64_t{1} << L;
        INFO("L=" << L << " size=" << size);
        REQUIRE(len_log2_for_size(size) == L);
        REQUIRE(bucket_of(size) == expected_bucket_for_L(L));
    }
}

TEST_CASE("DOC0260 §4 — bucket boundaries 8/9 and 41/42 land in adjacent buckets",
          "[storage][buckets][unit]") {
    // L = 8 -> bucket 0, L = 9 -> bucket 1.
    REQUIRE(bucket_of(std::uint64_t{1} << 8) == 0u);
    REQUIRE(bucket_of(std::uint64_t{1} << 9) == 1u);
    // L = 41 -> bucket 33, L = 42 -> bucket 34 (the tail bucket).
    REQUIRE(bucket_of(std::uint64_t{1} << 41) == 33u);
    REQUIRE(bucket_of(std::uint64_t{1} << 42) == 34u);
    // Saturation: the largest representable size stays in the tail bucket.
    REQUIRE(bucket_of(~std::uint64_t{0}) == 34u);
    // Tiny sizes collapse into bucket 0.
    REQUIRE(bucket_of(0u) == 0u);
    REQUIRE(bucket_of(1u) == 0u);
    REQUIRE(bucket_of(256u) == 0u);
}

TEST_CASE("DOC0260 §4 — every bucket index is addressable and < bucket_count",
          "[storage][buckets][unit]") {
    constexpr std::size_t bucket_count =
        generate_seq_buckets_len_thresholds<std::uint64_t, kMin, kMax>().size();
    for (std::size_t L = 1; L <= 63u; ++L) {
        REQUIRE(bucket_of(std::uint64_t{1} << L) < bucket_count);
    }
}

TEST_CASE("DOC0371 — seq_locator v3 is a 40-byte standard-layout POD that round-trips through bytes",
          "[storage][locator][unit]") {
    static_assert(sizeof(seq_locator) == 40u);
    static_assert(std::is_standard_layout_v<seq_locator>);

    seq_locator loc{};
    loc.version          = bioseq::storage::kLocatorVersion;
    loc.bucket_idx       = 17;
    loc.shard_idx        = 0xDEADBEEFu;
    loc.num_fragments    = 0x12345678u;
    loc.total_bp_size    = 0x0123456789ABCDEFull;
    loc.first_sub_row_no = 0xFEDCBA9876543210ull;
    loc.source_epoch     = 0x1020304050607080ull;

    // memcpy out and back — the encode/decode helpers in
    // sequence_storage_manager do exactly this into the pgseq_t stub data[].
    std::array<unsigned char, sizeof(seq_locator)> bytes{};
    std::memcpy(bytes.data(), &loc, sizeof(loc));

    seq_locator decoded{};
    std::memcpy(&decoded, bytes.data(), sizeof(decoded));

    REQUIRE(decoded.version          == loc.version);
    REQUIRE(decoded.bucket_idx       == loc.bucket_idx);
    REQUIRE(decoded.shard_idx        == loc.shard_idx);
    REQUIRE(decoded.num_fragments    == loc.num_fragments);
    REQUIRE(decoded.total_bp_size    == loc.total_bp_size);
    REQUIRE(decoded.first_sub_row_no == loc.first_sub_row_no);
    REQUIRE(decoded.source_epoch     == loc.source_epoch);

    // The fixed offsets the pg_compact_sequence stub reader depends on.
    REQUIRE(offsetof(seq_locator, num_fragments)    == 8u);
    REQUIRE(offsetof(seq_locator, total_bp_size)    == 16u);
    REQUIRE(offsetof(seq_locator, first_sub_row_no) == 24u);
    REQUIRE(offsetof(seq_locator, source_epoch)     == 32u);
}

TEST_CASE("DOC0377 — canonical locator payload codec validates exact frozen framing",
          "[storage][locator][unit]") {
    seq_locator locator{
        .version = bioseq::storage::kLocatorVersion,
        .bucket_idx = (std::numeric_limits<std::uint8_t>::max)(),
        .shard_idx = (std::numeric_limits<std::uint32_t>::max)(),
        .num_fragments = (std::numeric_limits<std::uint32_t>::max)(),
        .total_bp_size = (std::numeric_limits<std::uint64_t>::max)(),
        .first_sub_row_no = (std::numeric_limits<std::uint64_t>::max)(),
        .source_epoch = 0u,
    };
    std::array<std::byte, sizeof(seq_locator)> payload{};
    REQUIRE(bioseq::storage::encode_seq_locator_payload(payload, locator));
    const auto decoded = bioseq::storage::decode_seq_locator_payload(payload);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->bucket_idx == locator.bucket_idx);
    REQUIRE(decoded->shard_idx == locator.shard_idx);
    REQUIRE(decoded->num_fragments == locator.num_fragments);
    REQUIRE(decoded->total_bp_size == locator.total_bp_size);
    REQUIRE(decoded->first_sub_row_no == locator.first_sub_row_no);
    REQUIRE(decoded->source_epoch == 0u);

    REQUIRE_FALSE(bioseq::storage::encode_seq_locator_payload(
        std::span<std::byte>(payload).first(payload.size() - 1u), locator));
    REQUIRE_FALSE(bioseq::storage::decode_seq_locator_payload(
        std::span<const std::byte>(payload).first(payload.size() - 1u))
                      .has_value());

    locator.version = 2u;
    REQUIRE_FALSE(bioseq::storage::encode_seq_locator_payload(payload, locator));
    payload[0] = std::byte{2u};
    REQUIRE_FALSE(bioseq::storage::decode_seq_locator_payload(payload)
                      .has_value());

    locator.version = bioseq::storage::kLocatorVersion;
    locator._pad_2 = 1u;
    REQUIRE_FALSE(bioseq::storage::encode_seq_locator_payload(payload, locator));
    locator._pad_2 = 0u;
    locator._pad_4 = 1u;
    REQUIRE_FALSE(bioseq::storage::encode_seq_locator_payload(payload, locator));

    REQUIRE(bioseq::storage::is_seq_locator_stub_width(
        bioseq::storage::k_seq_locator_stub_marker));
    REQUIRE_FALSE(bioseq::storage::is_seq_locator_stub_width(8u));
}

TEST_CASE("DOC0377 — flat locator validation rejects every unsafe narrowing and geometry",
          "[storage][locator][unit][corruption][overflow]") {
    using error = bioseq::storage::flat_locator_validation_error;
    const auto validate = [](seq_locator locator) noexcept {
        return bioseq::storage::validate_flat_locator_geometry(locator);
    };
    const seq_locator valid{
        .version = bioseq::storage::kLocatorVersion,
        .bucket_idx = 1u,
        .shard_idx = 2u,
        .num_fragments = 2u,
        .total_bp_size = 32'769u,
        .first_sub_row_no = 17u,
        .source_epoch = 9u,
    };
    REQUIRE(validate(valid) == error::none);

    auto locator = valid;
    locator.version = 2u;
    REQUIRE(validate(locator) == error::invalid_wire_metadata);
    locator = valid;
    locator.source_epoch = 0u;
    REQUIRE(validate(locator) == error::invalid_wire_metadata);
    locator.source_epoch =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())
        + 1u;
    REQUIRE(validate(locator) == error::invalid_wire_metadata);

    locator = valid;
    locator.total_bp_size =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())
        + 1u;
    REQUIRE(validate(locator) == error::logical_length_exceeds_bigint);
    locator.total_bp_size =
        static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())
        + 1u;
    REQUIRE(validate(locator) == error::logical_length_exceeds_flat_field);
    locator.total_bp_size =
        static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)());
    REQUIRE(validate(locator) == error::none);

    locator = valid;
    locator.shard_idx =
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())
        + 1u;
    REQUIRE(validate(locator) == error::shard_exceeds_catalog_domain);
    locator = valid;
    locator.first_sub_row_no =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())
        + 1u;
    REQUIRE(validate(locator) == error::first_row_exceeds_catalog_domain);

    locator = valid;
    locator.num_fragments = 0u;
    REQUIRE(validate(locator) == error::fragment_geometry_inconsistent);
    locator = valid;
    locator.total_bp_size = 0u;
    REQUIRE(validate(locator) == error::fragment_geometry_inconsistent);
    locator = {};
    locator.source_epoch = 1u;
    locator.first_sub_row_no = 1u;
    REQUIRE(validate(locator) == error::fragment_geometry_inconsistent);
    locator = valid;
    locator.first_sub_row_no =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    REQUIRE(validate(locator)
            == error::fragment_row_range_exceeds_catalog_domain);
}

TEST_CASE("DOC0377 — fragment geometry validation is exact at unit and row boundaries",
          "[storage][locator][unit][fragment][corruption]") {
    using frame_error = bioseq::storage::fragment_frame_validation_error;
    using metadata_error =
        bioseq::storage::fragment_metadata_validation_error;

    REQUIRE(bioseq::storage::expected_flat_locator_fragment_count(
                0u, 16u, 1024u) == 0u);
    REQUIRE(bioseq::storage::expected_flat_locator_fragment_count(
                16'384u, 16u, 1024u) == 1u);
    REQUIRE(bioseq::storage::expected_flat_locator_fragment_count(
                16'385u, 16u, 1024u) == 2u);
    REQUIRE(bioseq::storage::expected_flat_locator_fragment_count(
                (std::numeric_limits<std::uint32_t>::max)(), 16u, 1024u)
            == 262'144u);
    REQUIRE(bioseq::storage::expected_flat_locator_fragment_count(
                1u, 0u, 1024u)
            == (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE(bioseq::storage::expected_flat_locator_fragment_count(
                1u, 16u, 0u)
            == (std::numeric_limits<std::uint64_t>::max)());

    REQUIRE(bioseq::storage::validate_fragment_frame_size(
                8'208u, 16u, 8'208u) == frame_error::none);
    REQUIRE(bioseq::storage::validate_fragment_frame_size(
                15u, 16u, 8'208u) == frame_error::shorter_than_header);
    REQUIRE(bioseq::storage::validate_fragment_frame_size(
                8'207u, 16u, 8'208u) == frame_error::size_mismatch);
    REQUIRE(bioseq::storage::validate_fragment_frame_size(
                8'209u, 16u, 8'208u) == frame_error::size_mismatch);

    REQUIRE(bioseq::storage::validate_fragment_metadata(
                0u, 8u, 32'768u, 0u,
                0u, 8u, 32'768u, 0u) == metadata_error::none);
    REQUIRE(bioseq::storage::validate_fragment_metadata(
                1u, 8u, 32'768u, 0u,
                0u, 8u, 32'768u, 0u) == metadata_error::type_mismatch);
    REQUIRE(bioseq::storage::validate_fragment_metadata(
                0u, 4u, 32'768u, 0u,
                0u, 8u, 32'768u, 0u) == metadata_error::type_mismatch);
    REQUIRE(bioseq::storage::validate_fragment_metadata(
                0u, 8u, 32'767u, 0u,
                0u, 8u, 32'768u, 0u)
            == metadata_error::logical_length_mismatch);
    REQUIRE(bioseq::storage::validate_fragment_metadata(
                0u, 8u, 1u, 31u,
                0u, 8u, 1u, 0u) == metadata_error::padding_mismatch);
}
