// bioseq/tests/unit/indexing/index_chunk_io_test.cpp
//
// Per-fragment chunk byte plumbing for the fragment_chunks
// storage concept, exercised on fake_fragment_storage<1024> (a tiny maximum
// chunk size, so a modest payload splits across several chunks). Pure storage
// I/O — no SIMD kernel, so no Highway namespace wrapper is needed.

#include <tests/support/common.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
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

import bioseq.tests.support.indexing_storage_fakes;

namespace {

using bioseq::indexing::test::fake_fragment_storage;
using bioseq::indexing::test::fake_qualified_chunk_read_error;
using bioseq::indexing::index_chunk_address;
using bioseq::indexing::index_chunk_cache_policy;
using bioseq::indexing::index_chunk_read_stats;
using bioseq::indexing::index_sub_key_range;
using bioseq::indexing::index_sub_key_request;
using bioseq::indexing::index_sub_key_t;

constexpr std::size_t k_maximum_chunk_bytes = 1024;
using storage_type = fake_fragment_storage<k_maximum_chunk_bytes>;

/// Independent bit-at-a-time oracle for the normal (non-reflected),
/// zero-initialized/no-final-XOR CRC-64/ECMA-182 convention.
[[nodiscard]] std::uint64_t reference_crc64_ecma182(
        std::span<const std::byte> bytes,
        std::uint64_t initial = 0u) noexcept {
    std::uint64_t checksum = initial;
    for (const std::byte byte : bytes) {
        checksum ^= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(byte)) << 56u;
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            checksum = (checksum & (std::uint64_t{1u} << 63u)) != 0u
                ? (checksum << 1u)
                    ^ bioseq::indexing::k_crc64_ecma182_polynomial
                : checksum << 1u;
        }
    }
    return checksum;
}

/// Independent zero-byte state transition oracle. This deliberately does not
/// use the production GF(2) operators or CRC lookup tables.
[[nodiscard]] std::uint64_t reference_crc64_ecma182_advance_zero_bytes(
        std::uint64_t checksum,
        std::uint64_t zero_byte_count) noexcept {
    while (zero_byte_count != 0u) {
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            checksum = (checksum & (std::uint64_t{1u} << 63u)) != 0u
                ? (checksum << 1u)
                    ^ bioseq::indexing::k_crc64_ecma182_polynomial
                : checksum << 1u;
        }
        --zero_byte_count;
    }
    return checksum;
}

// Deterministic byte fill for chunk (sub_key, global byte offset).
[[nodiscard]] char fill_byte(index_sub_key_t sub_key, std::size_t global_off) {
    return (char)(((std::size_t)sub_key * 1315423911u + global_off * 2654435761u) & 0xFF);
}

void store_text(storage_type& storage, const index_chunk_address& address, std::string_view payload) {
    storage.store_qualified_chunk_for_test(
        address, std::span<const char>{payload.data(), payload.size()});
}

void store_undeclared_text(
        storage_type& storage,
        const index_chunk_address& address,
        std::string_view payload) {
    storage.store_undeclared_qualified_chunk_for_test(
        address, std::span<const char>{payload.data(), payload.size()});
}

[[nodiscard]] index_sub_key_request extent_request(
        std::uint64_t generation_id,
        std::int32_t position_segment,
        index_sub_key_t sub_key,
        std::uint32_t first_chunk_no,
        std::initializer_list<std::string_view> chunks) {
    bioseq::indexing::crc64_ecma182_accumulator checksum;
    std::uint64_t total_bytes = 0;
    for (std::string_view chunk : chunks) {
        checksum.update(chunk);
        total_bytes += chunk.size();
    }
    return index_sub_key_request{
        .generation_id = generation_id,
        .position_segment = position_segment,
        .sub_key = sub_key,
        .first_chunk_no = first_chunk_no,
        .expected_chunk_count = static_cast<std::uint32_t>(chunks.size()),
        .expected_total_bytes = total_bytes,
        .expected_checksum = checksum.checksum(),
    };
}

struct qualified_sink_probe {
    void operator()(const index_chunk_address&, const char*, std::size_t) const noexcept {}
};

static_assert(bioseq::indexing::index_chunk_reader_fn<qualified_sink_probe>);
static_assert(requires (
        storage_type& storage,
        std::span<const index_sub_key_range> ranges,
        std::span<const index_sub_key_request> requests) {
    { storage.read_chunks_in_range(
          ranges, requests, index_chunk_cache_policy::bypass_one_shot,
          qualified_sink_probe{}) } -> std::same_as<index_chunk_read_stats>;
});

}  // namespace

TEST_CASE("payload-row checksum pins LE metadata order and relocation identity",
          "[component:bioseq][layer:unit][feature:io]") {
    constexpr auto b = [](unsigned value) { return static_cast<std::byte>(value); };
    constexpr std::array<std::byte, 31> encoded{
        b(0x08), b(0x07), b(0x06), b(0x05), b(0x04), b(0x03), b(0x02), b(0x01),
        b(0xfe), b(0xff), b(0xff), b(0xff),
        b(0xfd), b(0xff), b(0xff), b(0xff), b(0xff), b(0xff), b(0xff), b(0xff),
        b(0x44), b(0x33), b(0x22), b(0x11),
        b(0x03), b(0x00), b(0x00), b(0x00),
        b(0x61), b(0x62), b(0x63),
    };
    constexpr std::array<std::byte, 3> blob{b(0x61), b(0x62), b(0x63)};
    constexpr std::uint64_t expected =
        bioseq::indexing::crc64_ecma182(std::span<const std::byte>{encoded});
    constexpr std::uint64_t actual = bioseq::indexing::crc64_index_chunk_row(
        0x0102030405060708ULL, -2, -3, 0x11223344U, 3,
        std::span<const std::byte>{blob});
    STATIC_REQUIRE(actual == expected);
    STATIC_REQUIRE(actual == 0x75AC50EBED1FCB63ULL);
    STATIC_REQUIRE(actual != bioseq::indexing::crc64_index_chunk_row(
        0x0102030405060709ULL, -2, -3, 0x11223344U, 3,
        std::span<const std::byte>{blob}));
    STATIC_REQUIRE(actual != bioseq::indexing::crc64_index_chunk_row(
        0x0102030405060708ULL, -1, -3, 0x11223344U, 3,
        std::span<const std::byte>{blob}));
    STATIC_REQUIRE(actual != bioseq::indexing::crc64_index_chunk_row(
        0x0102030405060708ULL, -2, -2, 0x11223344U, 3,
        std::span<const std::byte>{blob}));
    STATIC_REQUIRE(actual != bioseq::indexing::crc64_index_chunk_row(
        0x0102030405060708ULL, -2, -3, 0x11223345U, 3,
        std::span<const std::byte>{blob}));
    STATIC_REQUIRE(actual != bioseq::indexing::crc64_index_chunk_row(
        0x0102030405060708ULL, -2, -3, 0x11223344U, 4,
        std::span<const std::byte>{blob}));
}

TEST_CASE("CRC-64 bulk path matches the bit oracle for every boundary",
          "[component:bioseq][layer:unit][feature:io]") {
    using bioseq::indexing::crc64_ecma182;
    using bioseq::indexing::crc64_ecma182_accumulator;
    using bioseq::indexing::crc64_ecma182_concatenate;

    constexpr std::array<std::size_t, 26> lengths{
        0u, 1u, 2u, 3u, 7u, 8u, 9u, 15u, 16u, 17u, 23u, 24u,
        25u, 31u, 32u, 33u, 63u, 64u, 65u, 255u, 256u, 257u,
        8'063u, 8'064u, 8'065u, 65'535u,
    };
    constexpr std::size_t maximum_offset = 15u;
    std::vector<std::byte> storage(lengths.back() + maximum_offset + 1u);
    std::uint64_t random = 0xD1B54A32D192ED03ULL;
    for (std::byte& byte : storage) {
        random ^= random >> 12u;
        random ^= random << 25u;
        random ^= random >> 27u;
        byte = static_cast<std::byte>(
            (random * 0x2545F4914F6CDD1DULL) & 0xffu);
    }

    for (std::size_t offset = 0u; offset <= maximum_offset; ++offset) {
        for (const std::size_t length : lengths) {
            INFO("alignment offset=" << offset << " length=" << length);
            const auto bytes = std::span<const std::byte>{storage}
                .subspan(offset, length);
            const std::uint64_t expected = reference_crc64_ecma182(bytes);
            REQUIRE(crc64_ecma182(bytes) == expected);

            const std::uint64_t prior = 0xA5A5A5A55A5A5A5AULL
                ^ static_cast<std::uint64_t>(offset * 131u + length);
            crc64_ecma182_accumulator incremental{prior};
            std::size_t consumed = 0u;
            std::size_t chunk_ordinal = 0u;
            while (consumed < bytes.size()) {
                const std::size_t wanted = 1u
                    + ((chunk_ordinal * 17u + offset * 7u + length) % 41u);
                const std::size_t count = (std::min)(
                    wanted, bytes.size() - consumed);
                incremental.update(bytes.subspan(consumed, count));
                consumed += count;
                ++chunk_ordinal;
            }
            incremental.update(bytes.subspan(bytes.size(), 0u));
            REQUIRE(incremental.checksum()
                == reference_crc64_ecma182(bytes, prior));

            const std::size_t split = length / 2u;
            const auto left = bytes.first(split);
            const auto right = bytes.subspan(split);
            REQUIRE(crc64_ecma182_concatenate(
                crc64_ecma182(left), crc64_ecma182(right), right.size())
                == expected);
        }
    }

    for (std::size_t iteration = 0u; iteration < 128u; ++iteration) {
        random ^= random >> 12u;
        random ^= random << 25u;
        random ^= random >> 27u;
        const std::size_t offset = static_cast<std::size_t>(random & 15u);
        random *= 0x2545F4914F6CDD1DULL;
        const std::size_t length = static_cast<std::size_t>(
            random % (storage.size() - offset));
        INFO("random iteration=" << iteration << " offset=" << offset
            << " length=" << length);
        const auto bytes = std::span<const std::byte>{storage}
            .subspan(offset, length);
        const std::uint64_t expected = reference_crc64_ecma182(bytes);
        REQUIRE(crc64_ecma182(bytes) == expected);

        crc64_ecma182_accumulator chunked;
        std::size_t consumed = 0u;
        while (consumed < bytes.size()) {
            random ^= random >> 12u;
            random ^= random << 25u;
            random ^= random >> 27u;
            random *= 0x2545F4914F6CDD1DULL;
            const std::size_t wanted = 1u
                + static_cast<std::size_t>(random % 257u);
            const std::size_t count = (std::min)(
                wanted, bytes.size() - consumed);
            chunked.update(bytes.subspan(consumed, count));
            consumed += count;
        }
        REQUIRE(chunked.checksum() == expected);
    }

    std::string runtime_golden{
        "The quick brown fox jumps over the lazy dog"};
    REQUIRE(crc64_ecma182(std::string_view{runtime_golden})
        == 0x41E05242FFA9883BULL);

    std::vector<std::byte> runtime_blob{
        std::byte{0x61}, std::byte{0x62}, std::byte{0x63}};
    REQUIRE(bioseq::indexing::crc64_index_chunk_row(
        0x0102030405060708ULL, -2, -3, 0x11223344U, 3,
        std::span<const std::byte>{runtime_blob})
        == 0x75AC50EBED1FCB63ULL);
}

TEST_CASE("CRC-64 full-row zero advance matches an independent state oracle",
          "[component:bioseq][layer:unit][feature:io]") {
    using bioseq::indexing::crc64_ecma182;
    using bioseq::indexing::crc64_ecma182_accumulator;
    using bioseq::indexing::crc64_ecma182_concatenate;
    using bioseq::indexing::crc64_ecma182_advance_zero_bytes;

    constexpr std::uint64_t compile_time_state = 0xA5A5A5A55A5A5A5AULL;
    constexpr std::uint64_t compile_time_one = [] {
        crc64_ecma182_accumulator crc{0xA5A5A5A55A5A5A5AULL};
        crc.update_byte(0u);
        return crc.checksum();
    }();
    constexpr std::uint64_t compile_time_almost_full =
        crc64_ecma182_advance_zero_bytes(compile_time_state, 8'063u);
    STATIC_REQUIRE(crc64_ecma182_advance_zero_bytes(
        compile_time_state, 0u) == compile_time_state);
    STATIC_REQUIRE(crc64_ecma182_advance_zero_bytes(
        compile_time_state, 1u) == compile_time_one);
    STATIC_REQUIRE(crc64_ecma182_advance_zero_bytes(
        compile_time_state, 8'064u)
        == crc64_ecma182_advance_zero_bytes(
            compile_time_almost_full, 1u));

    constexpr std::array<std::uint64_t, 6> edge_states{
        0u,
        1u,
        std::uint64_t{1u} << 63u,
        bioseq::indexing::k_crc64_ecma182_polynomial,
        compile_time_state,
        (std::numeric_limits<std::uint64_t>::max)(),
    };
    constexpr std::array<std::uint64_t, 17> edge_lengths{
        0u, 1u, 2u, 7u, 8u, 15u, 16u, 63u, 64u,
        127u, 255u, 1'023u, 8'063u, 8'064u, 8'065u,
        16'127u, 16'128u,
    };
    for (const std::uint64_t state : edge_states) {
        for (const std::uint64_t length : edge_lengths) {
            INFO("edge state=" << state << " zero bytes=" << length);
            REQUIRE(crc64_ecma182_advance_zero_bytes(state, length)
                == reference_crc64_ecma182_advance_zero_bytes(
                    state, length));
        }
    }

    std::uint64_t random = 0xD1B54A32D192ED03ULL;
    const auto next_random = [&random]() noexcept {
        random ^= random >> 12u;
        random ^= random << 25u;
        random ^= random >> 27u;
        return random *= 0x2545F4914F6CDD1DULL;
    };
    for (std::size_t iteration = 0u; iteration < 64u; ++iteration) {
        const std::uint64_t state = next_random();
        const std::uint64_t sampled_length = next_random() % 16'385u;
        const std::uint64_t length = iteration % 8u == 0u
            ? 8'064u
            : sampled_length;
        INFO("random iteration=" << iteration << " state=" << state
            << " zero bytes=" << length);
        REQUIRE(crc64_ecma182_advance_zero_bytes(state, length)
            == reference_crc64_ecma182_advance_zero_bytes(state, length));
    }

    constexpr std::array<std::uint64_t, 9> tail_lengths{
        0u, 1u, 7u, 8u, 15u, 16u, 63u, 255u, 1'024u,
    };
    for (const std::uint64_t tail_length : tail_lengths) {
        const std::uint64_t state = next_random();
        const std::uint64_t expected =
            reference_crc64_ecma182_advance_zero_bytes(
                state, 8'064u + tail_length);
        const std::uint64_t full_row_state =
            crc64_ecma182_advance_zero_bytes(state, 8'064u);
        INFO("full-row tail bytes=" << tail_length << " state=" << state);
        REQUIRE(crc64_ecma182_advance_zero_bytes(
            full_row_state, tail_length) == expected);
        REQUIRE(crc64_ecma182_advance_zero_bytes(
            state, 8'064u + tail_length) == expected);
    }

    constexpr std::array<std::size_t, 8> suffix_lengths{
        0u, 1u, 15u, 8'063u, 8'064u, 8'065u, 8'127u, 9'088u,
    };
    std::vector<std::byte> suffix(suffix_lengths.back());
    for (std::byte& byte : suffix) {
        byte = static_cast<std::byte>(next_random() & 0xffu);
    }
    for (const std::size_t suffix_length : suffix_lengths) {
        const std::uint64_t left_state = next_random();
        const auto bytes = std::span<const std::byte>{suffix}
            .first(suffix_length);
        const std::uint64_t right_checksum =
            reference_crc64_ecma182(bytes);
        const std::uint64_t expected =
            reference_crc64_ecma182(bytes, left_state);
        INFO("concatenated suffix bytes=" << suffix_length
            << " left state=" << left_state);
        REQUIRE(crc64_ecma182_concatenate(
            left_state, right_checksum, suffix_length) == expected);
    }

    constexpr std::uint64_t maximum_length =
        (std::numeric_limits<std::uint64_t>::max)();
    constexpr std::uint64_t first_length = maximum_length / 2u;
    constexpr std::uint64_t second_length = maximum_length - first_length;
    const std::uint64_t maximum_direct =
        crc64_ecma182_advance_zero_bytes(
            compile_time_state, maximum_length);
    const std::uint64_t maximum_composed =
        crc64_ecma182_advance_zero_bytes(
            crc64_ecma182_advance_zero_bytes(
                compile_time_state, first_length),
            second_length);
    REQUIRE(maximum_direct == maximum_composed);
}

TEST_CASE("admitted owned buffers die before their permit",
          "[component:bioseq][layer:unit][feature:io]") {
    bioseq::parallelism::memory_admission_controller admission;
    REQUIRE(admission.reserved_bytes() == 0u);
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    admission.set_index_memory_cap_bytes(1u << 20u);
    std::uint64_t observed_during_buffer_destruction = 0;

    struct buffer_lifetime_probe {
        bioseq::parallelism::memory_admission_controller* admission = nullptr;
        std::uint64_t* observed = nullptr;
        ~buffer_lifetime_probe() {
            *observed = admission->reserved_bytes();
        }
    };
    struct admitted_owner {
        bioseq::parallelism::memory_permit permit;
        buffer_lifetime_probe buffer;
    };

    {
        auto permit = admission.try_reserve(4096);
        REQUIRE(permit.has_value());
        admitted_owner owner{
            .permit = std::move(*permit),
            .buffer = buffer_lifetime_probe{
                .admission = &admission,
                .observed = &observed_during_buffer_destruction},
        };
        REQUIRE(admission.reserved_bytes() == 4096u);
    }
    REQUIRE(observed_during_buffer_destruction == 4096u);
    REQUIRE(admission.reserved_bytes() == 0u);
    admission.set_index_memory_cap_bytes(previous_cap);
}

TEST_CASE("resize splits into ceil(bytes/p_max_chunk_size) chunks and round-trips bytes",
          "[component:bioseq][layer:unit][feature:io]") {
    storage_type storage;
    const index_sub_key_t sub_key = 3;
    const std::size_t total_bytes = k_maximum_chunk_bytes * 2 + 200;          // 3 chunks: 1024, 1024, 200
    storage.resize(sub_key, total_bytes);

    // Fill each chunk with a deterministic per-(sub_key, global-offset) pattern.
    std::size_t base = 0;
    storage.write_to_each<char>(sub_key, [&](std::size_t chunk_no, char* buf, std::size_t cap) {
        for (std::size_t i = 0; i < cap; ++i) buf[i] = fill_byte(sub_key, base + i);
        base += cap;
        return chunk_no;   // advance by 1
    });

    REQUIRE(storage.number_of_chunks(sub_key) == 3u);
    REQUIRE(storage.chunk_byte_size(sub_key, 0) == k_maximum_chunk_bytes);
    REQUIRE(storage.chunk_byte_size(sub_key, 1) == k_maximum_chunk_bytes);
    REQUIRE(storage.chunk_byte_size(sub_key, 2) == 200u);
    REQUIRE(storage.chunk_byte_size(sub_key, 3) == 0u);      // absent

    // Read-back per chunk reassembles exactly the bytes we wrote.
    std::size_t global = 0;
    for (std::size_t chunk_no = 0; chunk_no < storage.number_of_chunks(sub_key); ++chunk_no) {
        const std::size_t cb = storage.chunk_byte_size(sub_key, chunk_no);
        const char* p = storage.read_chunk<char>(sub_key, chunk_no);
        for (std::size_t i = 0; i < cb; ++i) REQUIRE(p[i] == fill_byte(sub_key, global + i));
        global += cb;
    }
    REQUIRE(global == total_bytes);
}

TEST_CASE("resize_chunks honours an explicit small plan",
          "[component:bioseq][layer:unit][feature:io]") {
    storage_type storage;
    const index_sub_key_t sub_key = 5;
    const std::array<std::size_t, 3> plan{16, 64, 8};
    storage.resize_chunks(sub_key, std::span<const std::size_t>(plan));

    storage.write_to_each<char>(sub_key, [&](std::size_t chunk_no, char* buf, std::size_t cap) {
        for (std::size_t i = 0; i < cap; ++i) buf[i] = (char)(chunk_no * 11 + i);
        return chunk_no;
    });

    REQUIRE(storage.number_of_chunks(sub_key) == 3u);
    REQUIRE(storage.chunk_byte_size(sub_key, 0) == 16u);
    REQUIRE(storage.chunk_byte_size(sub_key, 1) == 64u);
    REQUIRE(storage.chunk_byte_size(sub_key, 2) == 8u);
    for (std::size_t chunk_no = 0; chunk_no < 3; ++chunk_no) {
        const char* p = storage.read_chunk<char>(sub_key, chunk_no);
        for (std::size_t i = 0; i < storage.chunk_byte_size(sub_key, chunk_no); ++i)
            REQUIRE(p[i] == (char)(chunk_no * 11 + i));
    }
}

TEST_CASE("read_chunks_in_range concatenation equals the per-chunk walk in order",
          "[component:bioseq][layer:unit][feature:io]") {
    storage_type storage;
    // Three sub_keys, each multi-chunk, written ascending.
    for (index_sub_key_t sub_key : {index_sub_key_t{0}, index_sub_key_t{1}, index_sub_key_t{2}}) {
        const std::size_t bytes = k_maximum_chunk_bytes + (std::size_t)(sub_key + 1) * 7;   // 2 chunks each
        storage.resize(sub_key, bytes);
        std::size_t base = 0;
        storage.write_to_each<char>(sub_key, [&](std::size_t chunk_no, char* buf, std::size_t cap) {
            for (std::size_t i = 0; i < cap; ++i) buf[i] = fill_byte(sub_key, base + i);
            base += cap;
            return chunk_no;
        });
    }

    // Expected: the per-chunk read_chunk<char> walk in (sub_key, chunk_no) order.
    std::vector<char> expected;
    std::vector<std::pair<index_sub_key_t, std::size_t>> expected_order;
    for (index_sub_key_t sub_key = 0; sub_key < 3; ++sub_key) {
        for (std::size_t chunk_no = 0; chunk_no < storage.number_of_chunks(sub_key); ++chunk_no) {
            const std::size_t cb = storage.chunk_byte_size(sub_key, chunk_no);
            const char* p = storage.read_chunk<char>(sub_key, chunk_no);
            expected.insert(expected.end(), p, p + cb);
            expected_order.emplace_back(sub_key, chunk_no);
        }
    }

    // Actual: read_chunks_in_range over [0, 3).
    std::vector<char> got;
    std::vector<std::pair<index_sub_key_t, std::size_t>> got_order;
    std::map<index_sub_key_t, std::size_t> per_sub_chunk;
    storage.read_chunks_in_range(0, 3, [&](index_sub_key_t f, const char* blob, std::size_t bytes) {
        got.insert(got.end(), blob, blob + bytes);
        const std::size_t cno = per_sub_chunk[f]++;
        got_order.emplace_back(f, cno);
    });

    REQUIRE(got == expected);
    REQUIRE(got_order == expected_order);

    // The emitted (sub_key, chunk_no) order is strictly ascending lexicographically.
    for (std::size_t i = 1; i < got_order.size(); ++i)
        REQUIRE(got_order[i - 1] < got_order[i]);
}

TEST_CASE("empty read_chunks_in_range ranges fire no callback",
          "[component:bioseq][layer:unit][feature:io]") {
    storage_type storage;
    storage.resize(7, 32);
    storage.write_to_each<char>(7, [&](std::size_t c, char* buf, std::size_t cap) {
        for (std::size_t i = 0; i < cap; ++i) buf[i] = (char)i;
        return c;
    });

    std::size_t calls = 0;
    storage.read_chunks_in_range(0, 0, [&](index_sub_key_t, const char*, std::size_t) { ++calls; });
    REQUIRE(calls == 0u);
    storage.read_chunks_in_range(7, 7, [&](index_sub_key_t, const char*, std::size_t) { ++calls; });
    REQUIRE(calls == 0u);
}

TEST_CASE("qualified bulk read is ordered, exact, and fully attested",
          "[component:bioseq][layer:unit][feature:io]") {
    storage_type storage;

    // Insert deliberately out of order. An intervening unrequested subkey
    // exercises merge filtering and the exact skip-budget validation.
    store_text(storage, {2, 0, 0, 1}, "epsilon");
    store_text(storage, {1, 0, 12, 0}, "gamma");
    store_text(storage, {1, 1, 1, 3}, "delta");
    store_text(storage, {1, 0, 10, 5}, "alpha");
    store_text(storage, {1, 0, 11, 0}, "gap");
    store_text(storage, {1, 0, 10, 6}, "beta");

    const std::array ranges{
        index_sub_key_range{1, 0, 10, 13},
        index_sub_key_range{1, 1, 1, 2},
        index_sub_key_range{2, 0, 0, 1},
    };
    const std::array requests{
        extent_request(1, 0, 10, 5, {"alpha", "beta"}),
        extent_request(1, 0, 12, 0, {"gamma"}),
        extent_request(1, 1, 1, 3, {"delta"}),
        extent_request(2, 0, 0, 1, {"epsilon"}),
    };

    std::vector<index_chunk_address> addresses;
    std::vector<std::string> payloads;
    const index_chunk_read_stats stats = storage.read_chunks_in_range(
        ranges, requests, index_chunk_cache_policy::bypass_one_shot,
        [&](const index_chunk_address& address, const char* payload, std::size_t bytes) {
            addresses.push_back(address);
            payloads.emplace_back(payload, bytes);
        });

    const std::vector<index_chunk_address> expected_addresses{
        {1, 0, 10, 5},
        {1, 0, 10, 6},
        {1, 0, 12, 0},
        {1, 1, 1, 3},
        {2, 0, 0, 1},
    };
    const std::vector<std::string> expected_payloads{
        "alpha", "beta", "gamma", "delta", "epsilon"};
    REQUIRE(addresses == expected_addresses);
    REQUIRE(payloads == expected_payloads);
    for (std::size_t i = 1; i < addresses.size(); ++i) {
        const auto previous = std::tuple{
            addresses[i - 1].generation_id, addresses[i - 1].position_segment,
            addresses[i - 1].sub_key, addresses[i - 1].chunk_no};
        const auto current = std::tuple{
            addresses[i].generation_id, addresses[i].position_segment,
            addresses[i].sub_key, addresses[i].chunk_no};
        REQUIRE(previous < current);
    }

    REQUIRE(stats.relation_opens == 1);
    REQUIRE(stats.index_scans == 1);
    REQUIRE(stats.requested_ranges == ranges.size());
    REQUIRE(stats.heap_rows_read == 6);
    REQUIRE(stats.toast_values_read == 0);
    REQUIRE(stats.detoast_bytes == 0);
    REQUIRE(stats.explicit_copy_bytes == 0);
    REQUIRE(stats.wanted_subkeys == 4);
    REQUIRE(stats.read_subkeys == stats.wanted_subkeys);
    REQUIRE(stats.wanted_extents == requests.size());
    REQUIRE(stats.read_extents == stats.wanted_extents);
    REQUIRE(stats.wanted_chunks == 5);
    REQUIRE(stats.delivered_chunks == stats.wanted_chunks);
    REQUIRE(stats.skipped_rows == 1);
    REQUIRE(stats.skip_budget_rows == stats.skipped_rows);
    REQUIRE(stats.repeated_chunk_reads == 0);
    REQUIRE(stats.delivered_bytes == 26);
    REQUIRE(stats.heap_rows_read == stats.delivered_chunks + stats.skipped_rows);
}

TEST_CASE("qualified requests permit disjoint first-chunk extents on one subkey",
          "[component:bioseq][layer:unit][feature:io]") {
    storage_type storage;
    store_text(storage, {7, 2, 5, 10}, "control-a");
    store_text(storage, {7, 2, 5, 11}, "control-b");
    store_text(storage, {7, 2, 5, 20}, "position-a");
    store_text(storage, {7, 2, 5, 21}, "position-b");

    const std::array ranges{index_sub_key_range{7, 2, 5, 6}};
    const std::array requests{
        extent_request(7, 2, 5, 10, {"control-a", "control-b"}),
        extent_request(7, 2, 5, 20, {"position-a", "position-b"}),
    };
    std::vector<std::uint32_t> chunks;
    const auto stats = storage.read_chunks_in_range(
        ranges, requests, index_chunk_cache_policy::bypass_one_shot,
        [&](const index_chunk_address& address, const char*, std::size_t) {
            chunks.push_back(address.chunk_no);
        });

    REQUIRE((chunks == std::vector<std::uint32_t>{10, 11, 20, 21}));
    REQUIRE(stats.wanted_subkeys == 1);
    REQUIRE(stats.read_subkeys == 1);
    REQUIRE(stats.wanted_extents == 2);
    REQUIRE(stats.read_extents == 2);
    REQUIRE(stats.wanted_chunks == 4);
    REQUIRE(stats.delivered_chunks == 4);
    REQUIRE(stats.skipped_rows == 0);
    REQUIRE(stats.skip_budget_rows == 0);
}

TEST_CASE("qualified fake is state-free across cache policies and repeated groups",
          "[component:bioseq][layer:unit][feature:io]") {
    storage_type storage;
    store_text(storage, {9, 0, 3, 2}, "payload");
    const std::array ranges{index_sub_key_range{9, 0, 3, 4}};
    const std::array requests{extent_request(9, 0, 3, 2, {"payload"})};

    std::size_t calls = 0;
    const auto run = [&](index_chunk_cache_policy policy) {
        return storage.read_chunks_in_range(
            ranges, requests, policy,
            [&](const index_chunk_address&, const char*, std::size_t) { ++calls; });
    };
    storage.reset_cache_touch_probe_for_test();
    const auto populate = run(index_chunk_cache_policy::populate_residency);
    const auto populate_touches = storage.cache_touch_probe_for_test();
    storage.reset_cache_touch_probe_for_test();
    const auto bypass = run(index_chunk_cache_policy::bypass_one_shot);
    const auto bypass_touches = storage.cache_touch_probe_for_test();

    REQUIRE(calls == 2);
    REQUIRE(populate.delivered_chunks == 1);
    REQUIRE(bypass.delivered_chunks == 1);
    REQUIRE(populate.repeated_chunk_reads == 0);
    REQUIRE(bypass.repeated_chunk_reads == 0);
    REQUIRE(populate_touches.lookups == 1);
    REQUIRE(populate_touches.populations == 1);
    REQUIRE(bypass_touches.lookups == 0);
    REQUIRE(bypass_touches.populations == 0);

    const std::span<const index_sub_key_range> no_ranges;
    const std::span<const index_sub_key_request> no_requests;
    const auto empty = storage.read_chunks_in_range(
        no_ranges, no_requests, index_chunk_cache_policy::bypass_one_shot,
        qualified_sink_probe{});
    REQUIRE(empty.relation_opens == 0);
    REQUIRE(empty.index_scans == 0);
    REQUIRE(empty.requested_ranges == 0);
    REQUIRE(empty.heap_rows_read == 0);
    REQUIRE(empty.wanted_subkeys == 0);
    REQUIRE(empty.wanted_extents == 0);
    REQUIRE(empty.wanted_chunks == 0);
    REQUIRE(empty.delivered_chunks == 0);
    REQUIRE(empty.delivered_bytes == 0);
}

TEST_CASE("skip budget is predeclared and undeclared wide-gap rows fail",
          "[component:bioseq][layer:unit][feature:io]") {
    storage_type storage;
    store_text(storage, {11, 0, 1, 0}, "left");
    store_text(storage, {11, 0, 2, 0}, "declared-gap");
    for (index_sub_key_t sub_key = 3; sub_key < 20; ++sub_key) {
        store_undeclared_text(storage, {11, 0, sub_key, 0}, "injected-gap");
    }
    store_text(storage, {11, 0, 100, 0}, "right");

    const std::array ranges{index_sub_key_range{11, 0, 1, 101}};
    const std::array requests{
        extent_request(11, 0, 1, 0, {"left"}),
        extent_request(11, 0, 100, 0, {"right"}),
    };
    std::size_t callbacks = 0;
    REQUIRE_THROWS_AS(
        storage.read_chunks_in_range(
            ranges, requests, index_chunk_cache_policy::bypass_one_shot,
            [&](const index_chunk_address&, const char*, std::size_t) { ++callbacks; }),
        fake_qualified_chunk_read_error);
    // The declared subkey-2 row consumes the sole pre-scan budget. The first
    // injected row at subkey 3 is rejected instead of enlarging that budget.
    REQUIRE(callbacks == 1);
}

TEST_CASE("qualified SQL-key maxima remain valid without signed wrap",
          "[component:bioseq][layer:unit][feature:io]") {
    storage_type storage;
    constexpr std::uint64_t generation = static_cast<std::uint64_t>(
        (std::numeric_limits<std::int64_t>::max)());
    constexpr std::int32_t segment = (std::numeric_limits<std::int32_t>::max)();
    constexpr std::uint32_t chunk = static_cast<std::uint32_t>(
        (std::numeric_limits<std::int32_t>::max)());
    store_text(storage, {generation, segment, 42, chunk}, "boundary");

    const std::array ranges{index_sub_key_range{generation, segment, 42, 43}};
    const std::array requests{
        extent_request(generation, segment, 42, chunk, {"boundary"})};
    const auto stats = storage.read_chunks_in_range(
        ranges, requests, index_chunk_cache_policy::bypass_one_shot,
        qualified_sink_probe{});
    REQUIRE(stats.delivered_chunks == 1);
    REQUIRE(stats.delivered_bytes == 8);
}

TEST_CASE("qualified bulk read rejects unordered, overlapping, and unqualified plans",
          "[component:bioseq][layer:unit][feature:io]") {
    storage_type storage;
    store_text(storage, {1, 0, 1, 0}, "a");
    const index_sub_key_request valid_request = extent_request(1, 0, 1, 0, {"a"});
    const auto read = [&](std::span<const index_sub_key_range> ranges,
                          std::span<const index_sub_key_request> requests,
                          index_chunk_cache_policy policy = index_chunk_cache_policy::bypass_one_shot) {
        return storage.read_chunks_in_range(ranges, requests, policy, qualified_sink_probe{});
    };

    SECTION("ranges must be strictly sorted") {
        const std::array ranges{
            index_sub_key_range{2, 0, 0, 1}, index_sub_key_range{1, 0, 0, 2}};
        const std::array requests{valid_request};
        REQUIRE_THROWS_AS(read(ranges, requests), fake_qualified_chunk_read_error);
    }
    SECTION("ranges must be nonoverlapping") {
        const std::array ranges{
            index_sub_key_range{1, 0, 0, 3}, index_sub_key_range{1, 0, 2, 4}};
        const std::array requests{valid_request};
        REQUIRE_THROWS_AS(read(ranges, requests), fake_qualified_chunk_read_error);
    }
    SECTION("ranges must be nonempty") {
        const std::array ranges{index_sub_key_range{1, 0, 1, 1}};
        const std::array requests{valid_request};
        REQUIRE_THROWS_AS(read(ranges, requests), fake_qualified_chunk_read_error);
    }
    SECTION("requests must be strictly sorted") {
        const std::array ranges{index_sub_key_range{1, 0, 0, 3}};
        const std::array requests{
            extent_request(1, 0, 2, 0, {"x"}), valid_request};
        REQUIRE_THROWS_AS(read(ranges, requests), fake_qualified_chunk_read_error);
    }
    SECTION("duplicate requests are rejected") {
        const std::array ranges{index_sub_key_range{1, 0, 0, 2}};
        const std::array requests{valid_request, valid_request};
        REQUIRE_THROWS_AS(read(ranges, requests), fake_qualified_chunk_read_error);
    }
    SECTION("chunk intervals on one qualified subkey must not overlap") {
        const std::array ranges{index_sub_key_range{1, 0, 1, 2}};
        const std::array requests{
            index_sub_key_request{1, 0, 1, 0, 2, 1, 0},
            index_sub_key_request{1, 0, 1, 1, 2, 1, 0},
        };
        REQUIRE_THROWS_AS(read(ranges, requests), fake_qualified_chunk_read_error);
    }
    SECTION("every request must be covered by a qualified range") {
        const std::array ranges{index_sub_key_range{1, 0, 2, 3}};
        const std::array requests{valid_request};
        REQUIRE_THROWS_AS(read(ranges, requests), fake_qualified_chunk_read_error);
    }
    SECTION("ranges and requests must be empty or nonempty together") {
        const std::array ranges{index_sub_key_range{1, 0, 1, 2}};
        const std::span<const index_sub_key_request> no_requests;
        REQUIRE_THROWS_AS(read(ranges, no_requests), fake_qualified_chunk_read_error);
    }
    SECTION("every range must cover an exact request") {
        const std::array ranges{
            index_sub_key_range{1, 0, 1, 2}, index_sub_key_range{1, 0, 3, 4}};
        const std::array requests{valid_request};
        REQUIRE_THROWS_AS(read(ranges, requests), fake_qualified_chunk_read_error);
    }
    SECTION("zero-length extents are rejected") {
        const std::array ranges{index_sub_key_range{1, 0, 1, 2}};
        auto request = valid_request;
        request.expected_chunk_count = 0;
        const std::array requests{request};
        REQUIRE_THROWS_AS(read(ranges, requests), fake_qualified_chunk_read_error);
    }
    SECTION("chunk intervals must fit signed SQL INTEGER") {
        const std::array ranges{index_sub_key_range{1, 0, 1, 2}};
        auto request = valid_request;
        request.first_chunk_no =
            static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());
        request.expected_chunk_count = 2;
        const std::array requests{request};
        REQUIRE_THROWS_AS(read(ranges, requests), fake_qualified_chunk_read_error);
    }
    SECTION("generation and position qualifiers must fit their SQL axes") {
        const std::array bad_generation_ranges{index_sub_key_range{0, 0, 1, 2}};
        const std::array bad_generation_requests{
            index_sub_key_request{0, 0, 1, 0, 1, 1, 0}};
        REQUIRE_THROWS_AS(
            read(bad_generation_ranges, bad_generation_requests),
            fake_qualified_chunk_read_error);

        const std::array bad_segment_ranges{index_sub_key_range{1, -1, 1, 2}};
        const std::array bad_segment_requests{
            index_sub_key_request{1, -1, 1, 0, 1, 1, 0}};
        REQUIRE_THROWS_AS(
            read(bad_segment_ranges, bad_segment_requests),
            fake_qualified_chunk_read_error);

        constexpr std::uint64_t too_large_generation =
            static_cast<std::uint64_t>(
                (std::numeric_limits<std::int64_t>::max)()) + 1u;
        const std::array large_generation_ranges{
            index_sub_key_range{too_large_generation, 0, 1, 2}};
        const std::array large_generation_requests{
            index_sub_key_request{too_large_generation, 0, 1, 0, 1, 1, 0}};
        REQUIRE_THROWS_AS(
            read(large_generation_ranges, large_generation_requests),
            fake_qualified_chunk_read_error);
    }
    SECTION("cache policy must be one of the explicit modes") {
        const std::array ranges{index_sub_key_range{1, 0, 1, 2}};
        const std::array requests{valid_request};
        REQUIRE_THROWS_AS(
            read(ranges, requests, static_cast<index_chunk_cache_policy>(255)),
            fake_qualified_chunk_read_error);
    }
}

TEST_CASE("qualified bulk read rejects missing and corrupt extent data",
          "[component:bioseq][layer:unit][feature:io]") {
    const auto run_with_sink = [](storage_type& storage,
                                  const index_sub_key_request& request,
                                  auto&& sink) {
        const std::array ranges{
            index_sub_key_range{request.generation_id, request.position_segment,
                             request.sub_key, request.sub_key + 1}};
        const std::array requests{request};
        return storage.read_chunks_in_range(
            ranges, requests, index_chunk_cache_policy::bypass_one_shot,
            std::forward<decltype(sink)>(sink));
    };
    const auto run = [&](storage_type& storage, const index_sub_key_request& request) {
        return run_with_sink(storage, request, qualified_sink_probe{});
    };

    SECTION("entire extent missing") {
        storage_type storage;
        const auto request = extent_request(3, 0, 4, 5, {"a"});
        REQUIRE_THROWS_AS(run(storage, request), fake_qualified_chunk_read_error);
    }
    SECTION("interior chunk missing makes the observed extent non-contiguous") {
        storage_type storage;
        store_text(storage, {3, 0, 4, 5}, "a");
        store_text(storage, {3, 0, 4, 7}, "c");
        const auto request = extent_request(3, 0, 4, 5, {"a", "b", "c"});
        REQUIRE_THROWS_AS(run(storage, request), fake_qualified_chunk_read_error);
    }
    SECTION("trailing chunk missing") {
        storage_type storage;
        store_text(storage, {3, 0, 4, 5}, "a");
        const auto request = extent_request(3, 0, 4, 5, {"a", "b"});
        REQUIRE_THROWS_AS(run(storage, request), fake_qualified_chunk_read_error);
    }
    SECTION("chunk number outside every requested interval is corruption") {
        storage_type storage;
        store_text(storage, {3, 0, 4, 4}, "outside");
        store_text(storage, {3, 0, 4, 5}, "a");
        const auto request = extent_request(3, 0, 4, 5, {"a"});
        std::size_t calls = 0;
        REQUIRE_THROWS_AS(
            run_with_sink(storage, request,
                [&](const index_chunk_address&, const char*, std::size_t) { ++calls; }),
            fake_qualified_chunk_read_error);
        REQUIRE(calls == 0);
    }
    SECTION("row byte metadata is checked before callback") {
        storage_type storage;
        const index_chunk_address address{3, 0, 4, 5};
        store_text(storage, address, "a");
        ++storage.qualified_chunk_row_for_test(address).chunk_bytes;
        const auto request = extent_request(3, 0, 4, 5, {"a"});
        std::size_t calls = 0;
        REQUIRE_THROWS_AS(
            run_with_sink(storage, request,
                [&](const index_chunk_address&, const char*, std::size_t) { ++calls; }),
            fake_qualified_chunk_read_error);
        REQUIRE(calls == 0);
    }
    SECTION("row checksum is checked before callback") {
        storage_type storage;
        const index_chunk_address address{3, 0, 4, 5};
        store_text(storage, address, "a");
        storage.qualified_chunk_row_for_test(address).checksum ^= 1;
        const auto request = extent_request(3, 0, 4, 5, {"a"});
        std::size_t calls = 0;
        REQUIRE_THROWS_AS(
            run_with_sink(storage, request,
                [&](const index_chunk_address&, const char*, std::size_t) { ++calls; }),
            fake_qualified_chunk_read_error);
        REQUIRE(calls == 0);
    }
    SECTION("relocating an otherwise valid row without recomputing CRC is rejected") {
        storage_type storage;
        constexpr std::string_view payload = "a";
        const index_chunk_address original{3, 0, 4, 4};
        const index_chunk_address relocated{3, 0, 4, 5};
        store_text(storage, relocated, payload);
        storage.qualified_chunk_row_for_test(relocated).checksum =
            bioseq::indexing::crc64_index_chunk_row(
                original.generation_id, original.position_segment,
                original.sub_key, original.chunk_no,
                static_cast<std::uint32_t>(payload.size()),
                std::as_bytes(std::span<const char>{payload.data(), payload.size()}));
        const auto request = extent_request(3, 0, 4, 5, {payload});
        std::size_t calls = 0;
        REQUIRE_THROWS_AS(
            run_with_sink(storage, request,
                [&](const index_chunk_address&, const char*, std::size_t) { ++calls; }),
            fake_qualified_chunk_read_error);
        REQUIRE(calls == 0);
    }
    SECTION("aggregate byte count must match the directory request") {
        storage_type storage;
        store_text(storage, {3, 0, 4, 5}, "a");
        auto request = extent_request(3, 0, 4, 5, {"a"});
        ++request.expected_total_bytes;
        std::vector<std::string> staged;
        REQUIRE_THROWS_AS(
            run_with_sink(storage, request,
                [&](const index_chunk_address&, const char* payload, std::size_t bytes) {
                    staged.emplace_back(payload, bytes);
                }),
            fake_qualified_chunk_read_error);
        // Streaming is intentionally not transactional: row validation permits
        // the callback, then the late aggregate mismatch rejects the group. The
        // caller must keep this staged object unpublished and discard it.
        REQUIRE((staged == std::vector<std::string>{"a"}));
    }
    SECTION("aggregate CRC must match the ordered extent") {
        storage_type storage;
        store_text(storage, {3, 0, 4, 5}, "a");
        auto request = extent_request(3, 0, 4, 5, {"a"});
        request.expected_checksum ^= 1;
        std::size_t calls = 0;
        REQUIRE_THROWS_AS(
            run_with_sink(storage, request,
                [&](const index_chunk_address&, const char*, std::size_t) { ++calls; }),
            fake_qualified_chunk_read_error);
        REQUIRE(calls == 1);
    }
}

TEST_CASE("write_to_each<int64_t> caps capacity in T-units, not bytes",
          "[component:bioseq][layer:unit][feature:io]") {
    storage_type storage;
    const index_sub_key_t sub_key = 9;
    // k_maximum_chunk_bytes (1024) bytes / 8 = 128 int64 units per chunk; total 200 int64 => 2 chunks.
    const std::size_t total_units = 200;
    storage.resize(sub_key, total_units * sizeof(std::int64_t));

    std::size_t base = 0;
    storage.write_to_each<std::int64_t>(sub_key, [&](std::size_t chunk_no, std::int64_t* buf, std::size_t cap_units) {
        // cap_units is the chunk byte size / sizeof(int64), NEVER the byte size. The
        // chunk is committed only AFTER this writer returns, so verify cap_units against
        // the planned split (k_maximum_chunk_bytes/8 = 128 units for chunk 0; the remainder for chunk 1)
        // rather than calling chunk_byte_size() mid-write.
        REQUIRE(cap_units == (chunk_no == 0 ? k_maximum_chunk_bytes / sizeof(std::int64_t)
                                            : total_units - k_maximum_chunk_bytes / sizeof(std::int64_t)));
        for (std::size_t i = 0; i < cap_units; ++i) buf[i] = (std::int64_t)(base + i);
        base += cap_units;
        return chunk_no;
    });

    REQUIRE(storage.number_of_chunks(sub_key) == 2u);
    REQUIRE(storage.chunk_byte_size(sub_key, 0) == k_maximum_chunk_bytes);                 // 128 units
    REQUIRE(storage.chunk_byte_size(sub_key, 1) == (total_units * 8 - k_maximum_chunk_bytes));

    std::size_t global = 0;
    for (std::size_t chunk_no = 0; chunk_no < 2; ++chunk_no) {
        const std::size_t units = storage.chunk_byte_size(sub_key, chunk_no) / sizeof(std::int64_t);
        const std::int64_t* p = storage.read_chunk<std::int64_t>(sub_key, chunk_no);
        for (std::size_t i = 0; i < units; ++i) REQUIRE(p[i] == (std::int64_t)(global + i));
        global += units;
    }
    REQUIRE(global == total_units);
}
