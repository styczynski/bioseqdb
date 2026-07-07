// main/bioseq/backend/storage/lazy_compact_sequence_test.cpp
//
// DOC0263 §11.2 — unit tests for the no-fragment and fragmented
// specialisations of lazy_compact_sequence, plus the fragment_cache and the
// sequence_scanner concepts. These run in the NO_POSTGRES unit-test executable
// (bioseq_aligner_tests_exec) and link the new standalone storage modules
// directly (DOC0263 §2.2). The minimizer/aligner end-to-end parity is covered
// by the integration tests; here we lock the wrapper machinery itself:
// pointer-sized no-fragment wrapper, fragment-aware iterator, read_cptr
// zero-copy + stitch, the encoded_iterator slot ring + bounded eviction,
// BP iteration bit-identical to the baseline, and to_encoded_into parity.

#include "common_tests.h"
#include "common.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.storage.scanner;
import bioseq.storage.lazy;
import bioseq.common.fragment_cache;
import bioseq.indexing.symbol_counts;   // DOC0279 §12.5 — tally_symbol_counts (replaces count_symbols)

using namespace seq::literals;

namespace {

using bioseq::storage::sequence_scanner_none;
using bioseq::storage::sequence_scanner_test_chunked;
using bioseq::storage::lazy_compact_sequence;
using bioseq::storage::make_lazy_in_memory;
using bioseq::storage::is_valid_sequence_scanner;
using bioseq::storage::is_active_sequence_scanner;

using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;  // 32 letters / uint64 unit
using range_t  = compact_sequence_range<dna4_u64>;

// 2 storage units per fragment => 64 dna4 letters per fragment. Small enough
// that every multi-unit sequence crosses several fragment boundaries.
using chunked2 = sequence_scanner_test_chunked<std::uint64_t, 2>;
using chunked1 = sequence_scanner_test_chunked<std::uint64_t, 1>;  // worst case: 32 letters/frag

// Deterministic pseudo-random dna4 vector (xorshift-ish LCG → ranks 0..3).
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

}  // namespace

TEST_CASE("DOC0263 §11.2 — no-fragment lazy_compact_sequence is pointer-sized and forwards",
          "[lazy-fragmented][unit]") {
    static_assert(sizeof(lazy_compact_sequence<dna4_u64, sequence_scanner_none<std::uint64_t>>)
                  == sizeof(void*),
                  "no-fragment lazy must be pointer-sized");

    auto data = make_dna4(100);
    range_t range(data);
    auto lazy = make_lazy_in_memory(range);

    REQUIRE(lazy.bp_size() == 100u);
    REQUIRE(lazy.size() == range.size());
    REQUIRE(lazy.padding() == range.padding());
    REQUIRE(lazy.number_of_fragments() == 1u);
    // read_cptr returns the same pointer the underlying range would.
    REQUIRE(lazy.read_cptr(lazy.cbegin()) == range.read_cptr(range.cbegin()));
    // bit-cast view is available on the no-fragment side.
    auto view = lazy.template bit_cast_to_storage_unit<std::uint32_t>();
    REQUIRE(view.bp_size() == range.bp_size());
}

TEST_CASE("DOC0263 §11.2 — scanner concept conformance",
          "[lazy-fragmented][unit]") {
    static_assert(is_valid_sequence_scanner<sequence_scanner_none<std::uint64_t>>);
    static_assert(!is_active_sequence_scanner<sequence_scanner_none<std::uint64_t>>);
    static_assert(is_valid_sequence_scanner<chunked2>);
    static_assert(is_active_sequence_scanner<chunked2>);
    static_assert(is_active_sequence_scanner<chunked1>);
    SUCCEED("concepts hold at compile time");
}

TEST_CASE("DOC0263 §11.2 — fragmented iterator boundary + monotonic current_fragment_no",
          "[lazy-fragmented][unit]") {
    auto data = make_dna4(200);
    range_t range(data);
    lazy_compact_sequence<dna4_u64, chunked2> lazy{chunked2::from_range(range)};

    REQUIRE(lazy.bp_size() == 200u);
    const std::uint64_t expect_frags = (range.size() + 1u) / 2u;  // ceil(units / 2)
    REQUIRE(lazy.number_of_fragments() == expect_frags);

    std::uint64_t prev = 0;
    std::uint64_t steps = 0;
    for (auto it = lazy.cbegin(); !(it == lazy.cend()); ++it) {
        REQUIRE(it.current_fragment_no() >= prev);
        prev = it.current_fragment_no();
        ++steps;
    }
    REQUIRE(steps == range.size());  // one step per storage unit
}

TEST_CASE("DOC0263 §11.2 — read_cptr zero-copy fast path",
          "[lazy-fragmented][unit]") {
    auto data = make_dna4(200);
    range_t range(data);
    lazy_compact_sequence<dna4_u64, chunked2> lazy{chunked2::from_range(range)};

    std::array<std::uint64_t, 4> scratch{};
    // A request that fits inside one fragment returns a pointer INTO the
    // fragment (never into scratch).
    auto it = lazy.cbegin();  // unit 0, fragment 0
    const std::uint64_t* p = lazy.read_cptr(it, std::span<std::uint64_t>(scratch), 1);
    REQUIRE(p != scratch.data());
    // The pointed-at unit equals the original packed storage unit 0.
    REQUIRE(*p == *range.read_cptr(range.cbegin()));
}

TEST_CASE("DOC0263 §11.2 — read_cptr stitch across a fragment boundary",
          "[lazy-fragmented][unit]") {
    auto data = make_dna4(200);
    range_t range(data);
    const std::uint64_t* base = range.read_cptr(range.cbegin());
    lazy_compact_sequence<dna4_u64, chunked2> lazy{chunked2::from_range(range)};

    // Position at the last unit of fragment 0 (unit index 1), ask for 2 units:
    // straddles into fragment 1 → must stitch into scratch.
    auto it = lazy.cbegin();
    ++it;  // unit 1
    std::array<std::uint64_t, 2> scratch{};
    const std::uint64_t* p = lazy.read_cptr(it, std::span<std::uint64_t>(scratch), 2);
    REQUIRE(p == scratch.data());
    REQUIRE(p[0] == base[1]);
    REQUIRE(p[1] == base[2]);
}

TEST_CASE("DOC0263 §11.2 — fragmented BP iteration is bit-identical to the baseline",
          "[lazy-fragmented][unit]") {
    for (std::size_t n : {1u, 31u, 32u, 33u, 64u, 65u, 200u, 1000u}) {
        auto data = make_dna4(n, 0xabcdef + n);
        range_t range(data);
        lazy_compact_sequence<dna4_u64, chunked2> lazy{chunked2::from_range(range)};

        std::vector<std::uint8_t> base_ranks;
        for (auto r : range.template iterable<BP>()) base_ranks.push_back(static_cast<std::uint8_t>(r));

        std::vector<std::uint8_t> frag_ranks;
        for (auto bit = lazy.bp_begin(); !(bit == lazy.bp_end()); ++bit)
            frag_ranks.push_back(static_cast<std::uint8_t>(*bit));

        REQUIRE(frag_ranks.size() == base_ranks.size());
        REQUIRE(frag_ranks == base_ranks);
    }
}

TEST_CASE("DOC0263 §11.2 — encoded_iterator round-trip matches to_encoded_vector",
          "[lazy-fragmented][unit]") {
    auto data = make_dna4(512);
    range_t range(data);
    lazy_compact_sequence<dna4_u64, chunked2> lazy{chunked2::from_range(range)};

    const auto baseline = range.template to_encoded_vector<false>(0, range.bp_size());
    auto eit = lazy.template encoded_iter<false, false, std::uint8_t, 2>();

    // Whole sequence (stitches across fragments into scratch).
    std::vector<std::uint8_t> scratch(range.bp_size());
    const std::uint8_t* whole = eit.ptr_at(0, range.bp_size(), std::span<std::uint8_t>(scratch));
    for (std::size_t i = 0; i < baseline.size(); ++i) REQUIRE(whole[i] == baseline[i]);

    // Sub-region that fits one fragment (zero-copy: returns a slot pointer).
    std::array<std::uint8_t, 16> small{};
    const std::uint8_t* sub = eit.ptr_at(10, 16, std::span<std::uint8_t>(small));
    for (std::size_t i = 0; i < 16; ++i) REQUIRE(sub[i] == baseline[10 + i]);
}

TEST_CASE("DOC0263 §11.2 — encoded_iterator slot ring is bounded by MaxLoadedSlots",
          "[lazy-fragmented][unit]") {
    auto data = make_dna4(2000);  // ~63 storage units → many fragments at frag=2
    range_t range(data);
    lazy_compact_sequence<dna4_u64, chunked2> lazy{chunked2::from_range(range)};

    constexpr std::size_t kMax = 2;
    auto eit = lazy.template encoded_iter<false, false, std::uint8_t, kMax>();
    std::array<std::uint8_t, 8> scratch{};
    for (std::uint64_t bp = 0; bp + 8 <= range.bp_size(); bp += 13) {
        (void)eit.ptr_at(bp, 8, std::span<std::uint8_t>(scratch));
        REQUIRE(eit._slots.valid_count() <= kMax);
    }
}

TEST_CASE("DOC0263 §11.2 — to_encoded_into parity (forward + reverse-complement)",
          "[lazy-fragmented][unit]") {
    auto data = make_dna4(300);
    range_t range(data);
    lazy_compact_sequence<dna4_u64, chunked2> lazy{chunked2::from_range(range)};

    const std::array<std::pair<std::size_t, std::size_t>, 5> regions{{
        {0, 10}, {30, 90}, {60, 70}, {100, 256}, {0, 300}}};
    for (auto [from, to] : regions) {
        const std::size_t len = to - from;
        // forward, no complement
        std::vector<std::uint8_t> base_fwd(len), got_fwd(len);
        range.template to_encoded_into<false, false>(std::span<std::uint8_t>(base_fwd), from, to);
        lazy.template to_encoded_into<false, false>(std::span<std::uint8_t>(got_fwd), from, to);
        REQUIRE(got_fwd == base_fwd);
        // reverse + complement (the KSW2 reverse-strand path)
        std::vector<std::uint8_t> base_rc(len), got_rc(len);
        range.template to_encoded_into<true, true>(std::span<std::uint8_t>(base_rc), from, to);
        lazy.template to_encoded_into<true, true>(std::span<std::uint8_t>(got_rc), from, to);
        REQUIRE(got_rc == base_rc);
    }
}

TEST_CASE("DOC0279 §12.5 — fragmented tally_symbol_counts matches the in-memory baseline",
          "[lazy-fragmented][unit]") {
    // The POC count_symbols() 49-stub is deleted; the real per-rank tally is
    // tally_symbol_counts() (a flat int64[2^bits_per_char], not (count,letter)
    // pairs). The chunked-lazy tally must equal the scanner_none baseline.
    auto data = make_dna4(128);
    range_t range(data);
    auto baseline_lazy = make_lazy_in_memory(range);                          // scanner_none
    lazy_compact_sequence<dna4_u64, chunked1> frag_lazy{chunked1::from_range(range)};

    auto base = bioseq::indexing::tally_symbol_counts(baseline_lazy);
    auto got  = bioseq::indexing::tally_symbol_counts(frag_lazy);
    REQUIRE(got.size() == base.size());
    std::int64_t total = 0;
    for (std::size_t i = 0; i < base.size(); ++i) {
        REQUIRE(got[i] == base[i]);
        total += got[i];
    }
    REQUIRE(total == 128);   // every base counted exactly once
}

TEST_CASE("DOC0263 §5 — fragment_cache push/find/evict + generation counter",
          "[lazy-fragmented][unit]") {
    bioseq::common::fragment_cache<int, 4> cache;
    auto key_of = [](const int& v) noexcept { return static_cast<std::size_t>(v); };
    auto noevict = [](int&) noexcept {};

    REQUIRE(cache.valid_count() == 0u);
    auto [i0, g0] = cache.push(10, noevict);
    auto [i1, g1] = cache.push(20, noevict);
    REQUIRE(cache.valid_count() == 2u);
    REQUIRE(g1 > g0);  // monotonic generation
    auto f = cache.find(static_cast<std::size_t>(20), key_of);
    REQUIRE(f.has_value());
    REQUIRE(cache.slot_at(f->first) == 20);
    REQUIRE(cache.is_valid_at(i0, g0));

    // Fill to capacity then overflow: eviction frees the oldest, generation grows.
    cache.push(30, noevict);
    cache.push(40, noevict);
    REQUIRE(cache.valid_count() == 4u);
    int evicted = -1;
    auto rec = [&evicted](int& v) noexcept { evicted = v; };
    cache.push(50, rec);  // evicts oldest (10)
    REQUIRE(evicted == 10);
    REQUIRE(cache.valid_count() == 4u);
    REQUIRE_FALSE(cache.find(static_cast<std::size_t>(10), key_of).has_value());
    REQUIRE(cache.find(static_cast<std::size_t>(50), key_of).has_value());
}
