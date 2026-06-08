// backend/sequences/indexing/index_chunk_io_test.cpp
//
// DOC0279 §16.1 — the per-fragment chunk byte-plumbing of the CHUNKED_PER_FRAGMENT
// storage concept, exercised on FakeFragmentStorage<1024> (tiny kMaxChunk so a
// modest payload splits across several chunks). Pure storage I/O — no SIMD kernel,
// so no Highway namespace wrapper is needed.

#include "common_simd.h"
#include "common_tests.h"
#include "common.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <utility>
#include <vector>

import testing;
import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_sequences.kmers;
import bioseq_sequences.kmers.hashers.minimap_int;
import bioseq_storage_scanner;
import bioseq_storage_lazy;
import bioseq_storage_size;
import bioseq_indexing.type;
import bioseq_indexing.storage;
import bioseq_indexing.traits;
import bioseq_indexing.minimizer_index;
import bioseq_indexing.minimizer_hashmap;
import bioseq_indexing.minimizer_sequence;
import bioseq_indexing.symbol_counts;
import bioseq_parallelism.memory_admission;
import bioseq_indexing.registry;

#include "index_test_fakes.h"

namespace {

using bioseq::indexing::test::FakeFragmentStorage;
using bioseq::indexing::index_sub_key_t;

constexpr std::size_t kMax = 1024;
using Storage = FakeFragmentStorage<kMax>;

// Deterministic byte fill for chunk (sub_key, global byte offset).
[[nodiscard]] char fill_byte(index_sub_key_t sub_key, std::size_t global_off) {
    return (char)(((std::size_t)sub_key * 1315423911u + global_off * 2654435761u) & 0xFF);
}

}  // namespace

TEST_CASE("DOC0279 §6.3 — resize splits into ceil(bytes/kMaxChunk) chunks and round-trips bytes",
          "[indexing][chunk-io]") {
    Storage storage;
    const index_sub_key_t sub_key = 3;
    const std::size_t total_bytes = kMax * 2 + 200;          // 3 chunks: 1024, 1024, 200
    storage.resize(sub_key, total_bytes);

    // Fill each chunk with a deterministic per-(sub_key, global-offset) pattern.
    std::size_t base = 0;
    storage.write_to_each<char>(sub_key, [&](std::size_t chunk_no, char* buf, std::size_t cap) {
        for (std::size_t i = 0; i < cap; ++i) buf[i] = fill_byte(sub_key, base + i);
        base += cap;
        return chunk_no;   // advance by 1
    });

    REQUIRE(storage.number_of_chunks(sub_key) == 3u);
    REQUIRE(storage.chunk_byte_size(sub_key, 0) == kMax);
    REQUIRE(storage.chunk_byte_size(sub_key, 1) == kMax);
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

TEST_CASE("DOC0279 §6.3 — resize_chunks honours an explicit small plan",
          "[indexing][chunk-io]") {
    Storage storage;
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

TEST_CASE("DOC0279 §6.3 — read_chunks_in_range concatenation equals the per-chunk walk in order",
          "[indexing][chunk-io]") {
    Storage storage;
    // Three sub_keys, each multi-chunk, written ascending.
    for (index_sub_key_t sub_key : {index_sub_key_t{0}, index_sub_key_t{1}, index_sub_key_t{2}}) {
        const std::size_t bytes = kMax + (std::size_t)(sub_key + 1) * 7;   // 2 chunks each
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

TEST_CASE("DOC0279 §6.3 — empty read_chunks_in_range ranges fire no callback",
          "[indexing][chunk-io]") {
    Storage storage;
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

TEST_CASE("DOC0279 §6.3 — write_to_each<int64_t> caps capacity in T-units, not bytes",
          "[indexing][chunk-io]") {
    Storage storage;
    const index_sub_key_t sub_key = 9;
    // kMax (1024) bytes / 8 = 128 int64 units per chunk; total 200 int64 => 2 chunks.
    const std::size_t total_units = 200;
    storage.resize(sub_key, total_units * sizeof(std::int64_t));

    std::size_t base = 0;
    storage.write_to_each<std::int64_t>(sub_key, [&](std::size_t chunk_no, std::int64_t* buf, std::size_t cap_units) {
        // cap_units is the chunk byte size / sizeof(int64), NEVER the byte size. The
        // chunk is committed only AFTER this writer returns, so verify cap_units against
        // the planned split (kMax/8 = 128 units for chunk 0; the remainder for chunk 1)
        // rather than calling chunk_byte_size() mid-write.
        REQUIRE(cap_units == (chunk_no == 0 ? kMax / sizeof(std::int64_t)
                                            : total_units - kMax / sizeof(std::int64_t)));
        for (std::size_t i = 0; i < cap_units; ++i) buf[i] = (std::int64_t)(base + i);
        base += cap_units;
        return chunk_no;
    });

    REQUIRE(storage.number_of_chunks(sub_key) == 2u);
    REQUIRE(storage.chunk_byte_size(sub_key, 0) == kMax);                 // 128 units
    REQUIRE(storage.chunk_byte_size(sub_key, 1) == (total_units * 8 - kMax));

    std::size_t global = 0;
    for (std::size_t chunk_no = 0; chunk_no < 2; ++chunk_no) {
        const std::size_t units = storage.chunk_byte_size(sub_key, chunk_no) / sizeof(std::int64_t);
        const std::int64_t* p = storage.read_chunk<std::int64_t>(sub_key, chunk_no);
        for (std::size_t i = 0; i < units; ++i) REQUIRE(p[i] == (std::int64_t)(global + i));
        global += units;
    }
    REQUIRE(global == total_units);
}
