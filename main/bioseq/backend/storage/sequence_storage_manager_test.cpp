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
#include <cstdint>
#include <cstring>

import testing;
import bioseq_bucket_math;

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

TEST_CASE("DOC0260 §5.3 — seq_locator is a 24-byte standard-layout POD that round-trips through bytes",
          "[storage][locator][unit]") {
    static_assert(sizeof(seq_locator) == 24u);
    static_assert(std::is_standard_layout_v<seq_locator>);

    seq_locator loc{};
    loc.version       = bioseq::storage::kLocatorVersion;
    loc.bucket_idx    = 17;
    loc.shard_idx     = 0xDEADBEEFu;
    loc.total_bp_size = 0x0123456789ABCDEFull;
    loc.sub_row_no    = 0xFEDCBA9876543210ull;

    // memcpy out and back — the encode/decode helpers in
    // sequence_storage_manager do exactly this into the pgseq_t stub data[].
    std::array<unsigned char, sizeof(seq_locator)> bytes{};
    std::memcpy(bytes.data(), &loc, sizeof(loc));

    seq_locator decoded{};
    std::memcpy(&decoded, bytes.data(), sizeof(decoded));

    REQUIRE(decoded.version       == loc.version);
    REQUIRE(decoded.bucket_idx    == loc.bucket_idx);
    REQUIRE(decoded.shard_idx     == loc.shard_idx);
    REQUIRE(decoded.total_bp_size == loc.total_bp_size);
    REQUIRE(decoded.sub_row_no    == loc.sub_row_no);

    // The fixed offsets the pg_compact_sequence stub reader depends on.
    REQUIRE(offsetof(seq_locator, total_bp_size) == 8u);
    REQUIRE(offsetof(seq_locator, sub_row_no)    == 16u);
}
