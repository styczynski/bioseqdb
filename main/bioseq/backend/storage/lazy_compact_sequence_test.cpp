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
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.storage.scanner;
import bioseq.storage.lazy;
import bioseq.storage.size;
import bioseq.storage.coordinates;
import bioseq.common.fragment_cache;
import bioseq.parallelism.memory_admission;
import bioseq.sequences.rank_pack;
import bioseq.indexing.symbol_counts;   // DOC0279 §12.5 — tally_symbol_counts (replaces count_symbols)

using namespace seq::literals;

namespace {

using bioseq::storage::sequence_scanner_none;
using bioseq::storage::sequence_scanner_test_chunked;
using bioseq::storage::lazy_compact_sequence;
using bioseq::storage::make_lazy_in_memory;
using bioseq::storage::is_valid_sequence_scanner;
using bioseq::storage::is_active_sequence_scanner;
using bioseq::storage::as_lazy_sequence_view;
using bioseq::storage::basic_lazy_sequence_view;
using bioseq::storage::admitted_packed_region;
using bioseq::storage::sequence_size_t;

struct payload_observer_probe {
    std::array<bioseq::storage::sequence_payload_allocation_event, 8> events{};
    std::size_t count{};
    bool overflow{};
};

void collect_payload_allocation_noexcept(
        void* opaque,
        bioseq::storage::sequence_payload_allocation_event const& event,
        std::uint64_t,
        std::uint64_t) noexcept {
    auto& probe = *static_cast<payload_observer_probe*>(opaque);
    if (probe.count >= probe.events.size()) {
        probe.overflow = true;
        return;
    }
    probe.events[probe.count++] = event;
}

using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;  // 32 letters / uint64 unit
using range_t  = compact_sequence_range<dna4_u64>;

// 2 storage units per fragment => 64 dna4 letters per fragment. Small enough
// that every multi-unit sequence crosses several fragment boundaries.
using chunked2 = sequence_scanner_test_chunked<std::uint64_t, 2>;
using chunked1 = sequence_scanner_test_chunked<std::uint64_t, 1>;  // worst case: 32 letters/frag

using facade_t = basic_lazy_sequence_view<seq::dna4>;
using existing_fragmented_t = lazy_compact_sequence<dna4_u64, chunked2>;
using existing_flat_t = lazy_compact_sequence<
    dna4_u64, sequence_scanner_none<std::uint64_t>>;

template <class Alphabet, class Unit>
consteval bool canonical_adapter_profile_contract() {
    using instance_t = sequence_custom_alphabet<Alphabet, Unit>;
    using range_type = compact_sequence_range<instance_t>;
    using flat_owner = lazy_compact_sequence<
        instance_t, sequence_scanner_none<Unit>>;
    using fragmented_owner = lazy_compact_sequence<
        instance_t, sequence_scanner_test_chunked<Unit, 2u>>;
    using view_type = basic_lazy_sequence_view<Alphabet>;

    return bioseq::storage::supports_lazy_compact_sequence<flat_owner>
        && bioseq::storage::supports_lazy_compact_sequence<fragmented_owner>
        && bioseq::storage::supports_lazy_sequence_view<flat_owner>
        && bioseq::storage::supports_lazy_sequence_view<fragmented_owner>
        && std::is_same_v<
            decltype(as_lazy_sequence_view(
                std::declval<const range_type&>())),
            view_type>
        && std::is_same_v<
            decltype(as_lazy_sequence_view(
                std::declval<const flat_owner&>())),
            view_type>
        && std::is_same_v<
            decltype(as_lazy_sequence_view(
                std::declval<const fragmented_owner&>())),
            view_type>;
}

// DOC0377 genericity gate: every supported alphabet/physical-width profile
// must erase direct-flat, existing flat-lazy, and fragmented-lazy storage to
// exactly one alphabet-specialized facade type.
static_assert(canonical_adapter_profile_contract<seq::dna4, std::uint8_t>());
static_assert(canonical_adapter_profile_contract<seq::dna4, std::uint16_t>());
static_assert(canonical_adapter_profile_contract<seq::dna4, std::uint32_t>());
static_assert(canonical_adapter_profile_contract<seq::dna4, std::uint64_t>());
static_assert(canonical_adapter_profile_contract<seq::dna5, std::uint8_t>());
static_assert(canonical_adapter_profile_contract<seq::dna5, std::uint16_t>());
static_assert(canonical_adapter_profile_contract<seq::dna5, std::uint32_t>());
static_assert(canonical_adapter_profile_contract<seq::dna5, std::uint64_t>());

static_assert(bioseq::storage::supports_lazy_compact_sequence<
              existing_fragmented_t>);
static_assert(bioseq::storage::supports_lazy_sequence_view<
              existing_fragmented_t>);
static_assert(bioseq::storage::supports_lazy_sequence_view<existing_flat_t>);
static_assert(bioseq::storage::supports_lazy_compact_sequence_slice<facade_t>);
static_assert(bioseq::storage::supports_resident_packed_probe<facade_t>);
static_assert(!bioseq::storage::supports_lazy_sequence_view<range_t>);
static_assert(std::is_move_constructible_v<admitted_packed_region<seq::dna4>>);
static_assert(std::is_nothrow_move_constructible_v<
              admitted_packed_region<seq::dna4>>);
static_assert(!std::is_move_assignable_v<admitted_packed_region<seq::dna4>>);
static_assert(!std::is_copy_constructible_v<admitted_packed_region<seq::dna4>>);
static_assert(!std::is_copy_assignable_v<admitted_packed_region<seq::dna4>>);
static_assert(bioseq::storage::lazy_packed_letters_per_unit(1u, 2u)
              == 4u);
static_assert(bioseq::storage::lazy_packed_letters_per_unit(8u, 2u)
              == 32u);
static_assert(bioseq::storage::lazy_packed_letters_per_unit(1u, 3u)
              == 2u);
static_assert(bioseq::storage::lazy_packed_letters_per_unit(8u, 3u)
              == 21u);
static_assert(bioseq::storage::lazy_packed_letters_per_unit(1u, 5u)
              == 1u);
static_assert(bioseq::storage::lazy_packed_letters_per_unit(8u, 5u)
              == 12u);
static_assert(bioseq::storage::lazy_packed_letters_per_unit(4u, 5u)
              == 6u);
static_assert(bioseq::storage::lazy_packed_letters_per_unit(3u, 5u)
              == 0u);

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

template <bool IsReversed, bool WriteReversed, class View>
void require_orientation_matches(const range_t& baseline,
                                 View view,
                                 std::size_t from,
                                 std::size_t to) {
    std::vector<std::uint8_t> expected(to - from);
    std::vector<std::uint8_t> observed(to - from);
    baseline.template to_encoded_into<IsReversed, WriteReversed>(
        expected, from, to);
    view.template to_encoded_into<IsReversed, WriteReversed>(
        observed, from, to);
    REQUIRE(observed == expected);
}

struct mock_lazy_state {
    std::vector<std::uint8_t> ranks;
    std::optional<facade_t::resident_packed_region> resident;
    std::size_t callback_count{};
    std::size_t resident_probe_count{};
    std::size_t bulk_copy_attempt_count{};
    std::size_t bulk_copy_success_count{};
    bool bulk_copy_enabled{};
    bool worker_shareable{};
};

template <bool IsReversed, bool WriteReversed>
[[nodiscard]] const std::uint8_t* mock_ptr_at(
        void const* opaque,
        sequence_size_t from,
        std::size_t length,
        std::span<std::uint8_t> scratch) {
    auto& state = *const_cast<mock_lazy_state*>(
        static_cast<const mock_lazy_state*>(opaque));
    ++state.callback_count;
    if (length == 0u) {
        return scratch.data();
    }
    const std::size_t total = state.ranks.size();
    const std::size_t forward_from = IsReversed
        ? total - (static_cast<std::size_t>(from) + length)
        : static_cast<std::size_t>(from);
    constexpr bool descending_write =
        (IsReversed && !WriteReversed) || (!IsReversed && WriteReversed);
    for (std::size_t index = 0u; index < length; ++index) {
        std::uint8_t rank = state.ranks[forward_from + index];
        if constexpr (IsReversed) {
            rank = facade_t::traits::letter_complement(rank);
        }
        scratch[descending_write ? length - 1u - index : index] = rank;
    }
    return scratch.data();
}

[[nodiscard]] sequence_size_t mock_bp_size(void const* opaque) noexcept {
    return static_cast<const mock_lazy_state*>(opaque)->ranks.size();
}

[[nodiscard]] facade_t::resident_probe_result mock_resident_probe(
        void const* opaque,
        sequence_size_t,
        sequence_size_t,
        std::uint8_t) noexcept {
    auto& state = *const_cast<mock_lazy_state*>(
        static_cast<const mock_lazy_state*>(opaque));
    ++state.resident_probe_count;
    return state.resident;
}

[[nodiscard]] bool mock_worker_shareable(void const* opaque) noexcept {
    return static_cast<const mock_lazy_state*>(opaque)->worker_shareable;
}

[[nodiscard]] bool mock_copy_full_packed(
        void const* opaque,
        void* destination,
        std::size_t unit_count,
        std::uint8_t requested_width) {
    auto& state = *const_cast<mock_lazy_state*>(
        static_cast<const mock_lazy_state*>(opaque));
    ++state.bulk_copy_attempt_count;
    constexpr std::size_t letters_per_unit = 32u;
    const std::size_t expected_units =
        state.ranks.size() / letters_per_unit
        + static_cast<std::size_t>(
            state.ranks.size() % letters_per_unit != 0u);
    if (!state.bulk_copy_enabled || requested_width != sizeof(std::uint64_t)
        || unit_count != expected_units
        || (unit_count != 0u && destination == nullptr)) {
        return false;
    }
    auto units = std::span<std::uint64_t>{
        static_cast<std::uint64_t*>(destination), unit_count};
    std::fill(units.begin(), units.end(), std::uint64_t{});
    for (std::size_t index = 0u; index < state.ranks.size(); ++index) {
        units[index / letters_per_unit] |=
            static_cast<std::uint64_t>(state.ranks[index])
            << (2u * (index % letters_per_unit));
    }
    ++state.bulk_copy_success_count;
    return true;
}

inline const facade_t::backend_ops mock_ops{
    .bp_size = &mock_bp_size,
    .ptr_at = {
        &mock_ptr_at<false, false>,
        &mock_ptr_at<false, true>,
        &mock_ptr_at<true, false>,
        &mock_ptr_at<true, true>,
    },
    .try_resident_packed = &mock_resident_probe,
    .copy_full_packed = &mock_copy_full_packed,
    .worker_shareable = &mock_worker_shareable,
};

[[nodiscard]] facade_t make_mock_view(mock_lazy_state& state) noexcept {
    return facade_t{std::addressof(state), std::addressof(mock_ops)};
}

template <class Alphabet, class Unit>
void require_rank_pack_width() {
    constexpr std::size_t bits =
        seqan3::detail::ceil_log2(seqan3::alphabet_size<Alphabet>);
    using instance_t = sequence_custom_alphabet<Alphabet, Unit, bits>;
    using grow_sink_t = bioseq::sequences::growing_rank_pack_unit_sink<Unit>;
    using fixed_sink_t = bioseq::sequences::fixed_rank_pack_unit_sink<Unit>;
    using grow_packer_t = bioseq::sequences::checked_rank_pack_sink<
        instance_t,
        bioseq::sequences::invalid_rank_policy::reject,
        bioseq::sequences::pack_capacity_policy::grow_vector,
        grow_sink_t>;
    using fixed_packer_t = bioseq::sequences::checked_rank_pack_sink<
        instance_t,
        bioseq::sequences::invalid_rank_policy::reject,
        bioseq::sequences::pack_capacity_policy::fixed_span,
        fixed_sink_t>;
    constexpr std::size_t letters_per_unit =
        std::numeric_limits<Unit>::digits / bits;
    constexpr std::size_t alphabet_size = seqan3::alphabet_size<Alphabet>;
    const std::array<std::size_t, 10> lengths{{
        0u,
        1u,
        letters_per_unit - 1u,
        letters_per_unit,
        letters_per_unit + 1u,
        letters_per_unit * 2u - 1u,
        letters_per_unit * 2u,
        letters_per_unit * 2u + 1u,
        256u,
        257u,
    }};

    for (const std::size_t length : lengths) {
        std::vector<std::uint8_t> ranks(length);
        for (std::size_t index = 0u; index < length; ++index) {
            ranks[index] = static_cast<std::uint8_t>(index % alphabet_size);
        }
        const std::size_t expected_units =
            (length + letters_per_unit - 1u) / letters_per_unit;
        std::vector<Unit> expected(expected_units);
        for (std::size_t index = 0u; index < length; ++index) {
            const auto shift = (index % letters_per_unit) * bits;
            expected[index / letters_per_unit] |= static_cast<Unit>(
                static_cast<Unit>(ranks[index]) << shift);
        }

        std::vector<Unit> grown;
        grow_packer_t growing{grow_sink_t{grown}};
        REQUIRE(growing.append_ranks(ranks)
                == bioseq::sequences::rank_pack_status::ok);
        const auto grow_result = growing.finish(length, expected_units);
        REQUIRE(grow_result.status == bioseq::sequences::rank_pack_status::ok);

        std::vector<Unit> fixed(expected_units);
        fixed_packer_t fixed_writer{
            fixed_sink_t{std::span<Unit>(fixed)}};
        REQUIRE(fixed_writer.append_ranks(ranks)
                == bioseq::sequences::rank_pack_status::ok);
        const auto fixed_result = fixed_writer.finish(length, expected_units);
        REQUIRE(fixed_result.status == bioseq::sequences::rank_pack_status::ok);
        REQUIRE(fixed == grown);
        REQUIRE(fixed == expected);
        REQUIRE(fixed_result.padding_letters
                == (length == 0u || length % letters_per_unit == 0u
                    ? 0u
                    : letters_per_unit - length % letters_per_unit));
    }
}

struct collecting_rank_writer {
    std::vector<std::uint8_t> ranks;

    void append_ranks(std::span<const std::uint8_t> chunk) {
        ranks.insert(ranks.end(), chunk.begin(), chunk.end());
    }
};

template <bioseq::storage::sequence_transform Transform>
void require_stream_transform(
        facade_t view,
        std::span<const std::uint8_t> source,
        std::size_t from,
        std::size_t to) {
    collecting_rank_writer writer;
    bioseq::storage::stream_transformed_packed<Transform>(
        view, from, to, writer);

    std::vector<std::uint8_t> expected(to - from);
    for (std::size_t index = 0u; index < expected.size(); ++index) {
        const std::size_t source_index =
            bioseq::storage::has_reverse(Transform)
            ? to - 1u - index
            : from + index;
        std::uint8_t rank = source[source_index];
        if constexpr (bioseq::storage::has_complement(Transform)) {
            rank = facade_t::traits::letter_complement(rank);
        }
        expected[index] = rank;
    }
    REQUIRE(writer.ranks == expected);
}

template <class Unit>
void require_materialized_width(facade_t source,
                                std::span<const std::uint8_t> expected,
                                std::uint8_t width) {
    auto& admission = bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t before = admission.reserved_bytes();
    auto permit = admission.try_reserve(4096u);
    REQUIRE(permit.has_value());
    {
        auto owner = bioseq::storage::materialize_packed_region(
            source, 3u, 3u + expected.size(), width, std::move(*permit));
        REQUIRE(admission.reserved_bytes() == before + 4096u);
        REQUIRE(owner.descriptor().storage_unit_byte_width == sizeof(Unit));
        REQUIRE(owner.descriptor().leading_symbol_offset == 0u);
        REQUIRE(owner.descriptor().bp_length == expected.size());

        auto issued_before_move = owner.view();
        auto moved = std::move(owner);
        std::vector<std::uint8_t> observed(expected.size());
        issued_before_move.template to_encoded_into<false, false>(
            observed, 0u, observed.size());
        REQUIRE(std::equal(observed.begin(), observed.end(), expected.begin()));
        const auto moved_view = moved.view();
        REQUIRE(lazy_is_worker_shareable(moved_view));

        std::vector<seq::dna4> expected_letters(expected.size());
        for (std::size_t index = 0u; index < expected.size(); ++index) {
            expected_letters[index].assign_rank(expected[index]);
        }
        range_t expected_range(expected_letters);
        require_orientation_matches<false, false>(
            expected_range, moved_view, 0u, expected.size());
        require_orientation_matches<false, true>(
            expected_range, moved_view, 0u, expected.size());
        require_orientation_matches<true, false>(
            expected_range, moved_view, 0u, expected.size());
        require_orientation_matches<true, true>(
            expected_range, moved_view, 0u, expected.size());
    }
    REQUIRE(admission.reserved_bytes() == before);
}

void increment_release_count(void* opaque) noexcept {
    ++*static_cast<std::size_t*>(opaque);
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
    const auto baseline_view = as_lazy_sequence_view(baseline_lazy);
    const auto fragmented_view = as_lazy_sequence_view(frag_lazy);

    auto base = bioseq::indexing::tally_symbol_counts(baseline_view);
    auto got  = bioseq::indexing::tally_symbol_counts(fragmented_view);
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

TEST_CASE("DOC0377 — transform algebra is the two-bit Klein group",
          "[lazy-view][coordinates][unit]") {
    using bioseq::storage::compose;
    using bioseq::storage::has_complement;
    using bioseq::storage::has_reverse;
    using bioseq::storage::sequence_transform;
    constexpr std::array transforms{
        sequence_transform::identity,
        sequence_transform::reversed,
        sequence_transform::complemented,
        sequence_transform::reverse_complement,
    };
    for (const sequence_transform left : transforms) {
        REQUIRE(compose(left, sequence_transform::identity) == left);
        REQUIRE(compose(sequence_transform::identity, left) == left);
        REQUIRE(compose(left, left) == sequence_transform::identity);
        for (const sequence_transform right : transforms) {
            REQUIRE(compose(left, right) == compose(right, left));
        }
    }
    REQUIRE_FALSE(has_reverse(sequence_transform::identity));
    REQUIRE(has_reverse(sequence_transform::reversed));
    REQUIRE_FALSE(has_complement(sequence_transform::reversed));
    REQUIRE(has_complement(sequence_transform::complemented));
    REQUIRE(has_reverse(sequence_transform::reverse_complement));
    REQUIRE(has_complement(sequence_transform::reverse_complement));
}

TEST_CASE("DOC0377 — signed element and slice coordinates normalize without overflow",
          "[lazy-view][coordinates][unit]") {
    using bioseq::storage::normalize_element_index;
    using bioseq::storage::normalize_slice_bound;
    constexpr std::uint64_t length = 4u;
    constexpr auto min_i64 = (std::numeric_limits<std::int64_t>::min)();
    constexpr auto max_i64 = (std::numeric_limits<std::int64_t>::max)();

    REQUIRE(normalize_slice_bound(min_i64, length) == 0u);
    REQUIRE(normalize_slice_bound(max_i64, length) == length);
    REQUIRE(normalize_slice_bound(-5, length) == 0u);
    REQUIRE(normalize_slice_bound(-4, length) == 0u);
    REQUIRE(normalize_slice_bound(-1, length) == 3u);
    REQUIRE(normalize_slice_bound(0, length) == 0u);
    REQUIRE(normalize_slice_bound(3, length) == 3u);
    REQUIRE(normalize_slice_bound(4, length) == 4u);
    REQUIRE(normalize_slice_bound(5, length) == 4u);

    REQUIRE_FALSE(normalize_element_index(min_i64, length).has_value());
    REQUIRE_FALSE(normalize_element_index(-5, length).has_value());
    REQUIRE(normalize_element_index(-4, length) == 0u);
    REQUIRE(normalize_element_index(-1, length) == 3u);
    REQUIRE(normalize_element_index(0, length) == 0u);
    REQUIRE(normalize_element_index(3, length) == 3u);
    REQUIRE_FALSE(normalize_element_index(4, length).has_value());
    REQUIRE_FALSE(normalize_element_index(-1, 0u).has_value());

    constexpr std::uint64_t two_to_63 = std::uint64_t{1} << 63u;
    REQUIRE(normalize_slice_bound(min_i64, two_to_63) == 0u);
    REQUIRE(normalize_element_index(min_i64, two_to_63) == 0u);
}

TEST_CASE("DOC0377 — flat and fragmented backends erase to one canonical facade",
          "[lazy-view][unit]") {
    auto data = make_dna4(600u);
    range_t range(data);
    auto flat_owner = make_lazy_in_memory(range);
    existing_fragmented_t fragmented_owner{chunked2::from_range(range)};
    const auto range_view = as_lazy_sequence_view(range);
    const auto flat_view = as_lazy_sequence_view(flat_owner);
    const auto fragmented_view = as_lazy_sequence_view(fragmented_owner);
    static_assert(std::same_as<decltype(range_view), decltype(flat_view)>);
    static_assert(std::same_as<decltype(flat_view), decltype(fragmented_view)>);

    REQUIRE(range_view.bp_size() == 600u);
    REQUIRE(flat_view.bp_size() == 600u);
    REQUIRE(fragmented_view.bp_size() == 600u);
    REQUIRE(lazy_is_worker_shareable(range_view));
    REQUIRE(lazy_is_worker_shareable(flat_view));
    REQUIRE_FALSE(lazy_is_worker_shareable(fragmented_view));

    const std::array<std::pair<std::size_t, std::size_t>, 6> windows{{
        {0u, 0u}, {0u, 1u}, {31u, 33u}, {63u, 129u},
        {255u, 513u}, {0u, 600u},
    }};
    for (const auto [from, to] : windows) {
        require_orientation_matches<false, false>(range, range_view, from, to);
        require_orientation_matches<false, true>(range, range_view, from, to);
        require_orientation_matches<true, false>(range, range_view, from, to);
        require_orientation_matches<true, true>(range, range_view, from, to);
        require_orientation_matches<false, false>(range, flat_view, from, to);
        require_orientation_matches<false, true>(range, flat_view, from, to);
        require_orientation_matches<true, false>(range, flat_view, from, to);
        require_orientation_matches<true, true>(range, flat_view, from, to);
        require_orientation_matches<false, false>(range, fragmented_view, from, to);
        require_orientation_matches<false, true>(range, fragmented_view, from, to);
        require_orientation_matches<true, false>(range, fragmented_view, from, to);
        require_orientation_matches<true, true>(range, fragmented_view, from, to);
    }

    std::array<std::uint8_t, 16> scratch{};
    auto encoded = fragmented_view.template encoded_iter<
        false, false, std::uint8_t, 2>();
    const std::uint8_t* pointer = encoded.ptr_at(10u, scratch.size(), scratch);
    REQUIRE(pointer == scratch.data());

    const auto resident = flat_view.try_resident_packed(3u, 70u, 8u);
    REQUIRE(resident.has_value());
    REQUIRE(resident->storage_unit_byte_width == 8u);
    REQUIRE(resident->bp_length == 67u);
    REQUIRE(resident->leading_symbol_offset == 3u);
    REQUIRE(resident->unit_count == 3u);
    REQUIRE(resident->trailing_padding_letters == 26u);
    REQUIRE_FALSE(flat_view.try_resident_packed(3u, 70u, 4u).has_value());
    REQUIRE_FALSE(fragmented_view.try_resident_packed(3u, 70u, 8u).has_value());

    const auto range_resident = range_view.try_resident_packed(3u, 70u, 8u);
    REQUIRE(range_resident.has_value());
    REQUIRE(range_resident->units == resident->units);
    REQUIRE(range_resident->unit_count == resident->unit_count);

    const auto empty = flat_view.try_resident_packed(600u, 600u, 8u);
    REQUIRE(empty.has_value());
    REQUIRE(empty->units == nullptr);
    REQUIRE(empty->unit_count == 0u);
}

TEST_CASE("DOC0377 — canonical rank iterator batches callbacks and copies reload independently",
          "[lazy-view][unit]") {
    mock_lazy_state state;
    state.ranks.resize(600u);
    for (std::size_t index = 0u; index < state.ranks.size(); ++index) {
        state.ranks[index] = static_cast<std::uint8_t>(index & 3u);
    }
    auto view = make_mock_view(state);
    std::vector<std::uint8_t> observed;
    observed.reserve(state.ranks.size());
    for (auto cursor = view.begin(); cursor != view.end(); ++cursor) {
        observed.push_back(*cursor);
    }
    REQUIRE(observed == state.ranks);
    REQUIRE(state.callback_count == 3u);

    state.callback_count = 0u;
    auto first = view.begin();
    REQUIRE(*first == state.ranks[0]);
    auto copied = first;
    REQUIRE(*copied == state.ranks[0]);
    REQUIRE(state.callback_count == 2u);
}

TEST_CASE("DOC0377 — resident descriptor validation rejects every malformed field",
          "[lazy-view][resident][unit]") {
    alignas(8) std::array<std::byte, 16> storage{};
    mock_lazy_state state;
    state.ranks = {0u, 1u, 2u, 3u};
    facade_t::resident_packed_region valid{
        .units = storage.data(),
        .unit_count = 1u,
        .bp_length = 4u,
        .storage_unit_byte_width = 8u,
        .bits_per_symbol = 2u,
        .leading_symbol_offset = 0u,
        .trailing_padding_letters = 28u,
        .bit_order = facade_t::packed_bit_order::least_significant_symbol_first,
    };
    state.resident = valid;
    auto view = make_mock_view(state);
    REQUIRE(view.try_resident_packed(0u, 4u, 8u).has_value());

    state.resident = valid;
    state.resident->units = nullptr;
    REQUIRE_FALSE(view.try_resident_packed(0u, 4u, 8u).has_value());
    state.resident = valid;
    state.resident->units = storage.data() + 1u;
    REQUIRE_FALSE(view.try_resident_packed(0u, 4u, 8u).has_value());
    state.resident = valid;
    state.resident->storage_unit_byte_width = 4u;
    REQUIRE_FALSE(view.try_resident_packed(0u, 4u, 8u).has_value());
    state.resident = valid;
    state.resident->bits_per_symbol = 3u;
    REQUIRE_FALSE(view.try_resident_packed(0u, 4u, 8u).has_value());
    state.resident = valid;
    state.resident->bit_order = static_cast<facade_t::packed_bit_order>(1u);
    REQUIRE_FALSE(view.try_resident_packed(0u, 4u, 8u).has_value());
    state.resident = valid;
    state.resident->leading_symbol_offset = 32u;
    REQUIRE_FALSE(view.try_resident_packed(0u, 4u, 8u).has_value());
    state.resident = valid;
    state.resident->trailing_padding_letters = 27u;
    REQUIRE_FALSE(view.try_resident_packed(0u, 4u, 8u).has_value());
    state.resident = valid;
    state.resident->unit_count = 2u;
    REQUIRE_FALSE(view.try_resident_packed(0u, 4u, 8u).has_value());
    state.resident = valid;
    state.resident->unit_count = (std::numeric_limits<std::size_t>::max)();
    REQUIRE_FALSE(view.try_resident_packed(0u, 4u, 8u).has_value());

    state.ranks.clear();
    state.resident = facade_t::resident_packed_region{
        .units = nullptr,
        .unit_count = 0u,
        .bp_length = 0u,
        .storage_unit_byte_width = 8u,
        .bits_per_symbol = 2u,
        .leading_symbol_offset = 0u,
        .trailing_padding_letters = 0u,
        .bit_order = facade_t::packed_bit_order::least_significant_symbol_first,
    };
    REQUIRE(view.try_resident_packed(0u, 0u, 8u).has_value());
    state.resident->units = storage.data();
    REQUIRE_FALSE(view.try_resident_packed(0u, 0u, 8u).has_value());
}

TEST_CASE("DOC0377 — exact resident probes support every physical width",
          "[lazy-view][resident][unit]") {
    auto data = make_dna4(73u);
    using dna4_u8 = sequence_custom_alphabet<seq::dna4, std::uint8_t>;
    using dna4_u16 = sequence_custom_alphabet<seq::dna4, std::uint16_t>;
    using dna4_u32 = sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    compact_sequence_range<dna4_u8> range8(data);
    compact_sequence_range<dna4_u16> range16(data);
    compact_sequence_range<dna4_u32> range32(data);
    range_t range64(data);
    auto lazy8 = make_lazy_in_memory(range8);
    auto lazy16 = make_lazy_in_memory(range16);
    auto lazy32 = make_lazy_in_memory(range32);
    auto lazy64 = make_lazy_in_memory(range64);
    const auto view8 = as_lazy_sequence_view(lazy8);
    const auto view16 = as_lazy_sequence_view(lazy16);
    const auto view32 = as_lazy_sequence_view(lazy32);
    const auto view64 = as_lazy_sequence_view(lazy64);
    REQUIRE(view8.try_resident_packed(1u, 72u, 1u));
    REQUIRE(view16.try_resident_packed(1u, 72u, 2u));
    REQUIRE(view32.try_resident_packed(1u, 72u, 4u));
    REQUIRE(view64.try_resident_packed(1u, 72u, 8u));
}

TEST_CASE("DOC0377 — checked rank packing is byte-identical for grow and fixed sinks",
          "[lazy-view][rank-pack][unit]") {
    require_rank_pack_width<seq::dna4, std::uint8_t>();
    require_rank_pack_width<seq::dna4, std::uint16_t>();
    require_rank_pack_width<seq::dna4, std::uint32_t>();
    require_rank_pack_width<seq::dna4, std::uint64_t>();
    require_rank_pack_width<seq::dna5, std::uint8_t>();
    require_rank_pack_width<seq::dna5, std::uint16_t>();
    require_rank_pack_width<seq::dna5, std::uint32_t>();
    require_rank_pack_width<seq::dna5, std::uint64_t>();
}

TEST_CASE("DOC0377 — checked rank packing reports invalid input and capacity mismatches",
          "[lazy-view][rank-pack][unit]") {
    using instance_t = sequence_custom_alphabet<seq::dna4, std::uint8_t, 2>;
    using grow_sink_t =
        bioseq::sequences::growing_rank_pack_unit_sink<std::uint8_t>;
    using fixed_sink_t =
        bioseq::sequences::fixed_rank_pack_unit_sink<std::uint8_t>;
    using reject_grow_t = bioseq::sequences::checked_rank_pack_sink<
        instance_t,
        bioseq::sequences::invalid_rank_policy::reject,
        bioseq::sequences::pack_capacity_policy::grow_vector,
        grow_sink_t>;
    using drop_grow_t = bioseq::sequences::checked_rank_pack_sink<
        instance_t,
        bioseq::sequences::invalid_rank_policy::drop,
        bioseq::sequences::pack_capacity_policy::grow_vector,
        grow_sink_t>;
    using reject_fixed_t = bioseq::sequences::checked_rank_pack_sink<
        instance_t,
        bioseq::sequences::invalid_rank_policy::reject,
        bioseq::sequences::pack_capacity_policy::fixed_span,
        fixed_sink_t>;
    const std::array<std::uint8_t, 3> invalid{{0u, 0xffu, 1u}};

    std::vector<std::uint8_t> rejected_units;
    reject_grow_t rejected{grow_sink_t{rejected_units}};
    REQUIRE(rejected.append_ranks(invalid)
            == bioseq::sequences::rank_pack_status::invalid_rank);
    REQUIRE(rejected.finish().status
            == bioseq::sequences::rank_pack_status::invalid_rank);

    std::vector<std::uint8_t> dropped_units;
    drop_grow_t dropped{grow_sink_t{dropped_units}};
    REQUIRE(dropped.append_ranks(invalid)
            == bioseq::sequences::rank_pack_status::ok);
    const auto dropped_result = dropped.finish(2u, 1u);
    REQUIRE(dropped_result.status == bioseq::sequences::rank_pack_status::ok);
    REQUIRE(dropped_result.emitted_symbols == 2u);

    const std::array<std::uint8_t, 5> five{{0u, 1u, 2u, 3u, 0u}};
    std::array<std::uint8_t, 1> one_unit{};
    reject_fixed_t short_writer{
        fixed_sink_t{std::span<std::uint8_t>(one_unit)}};
    REQUIRE(short_writer.append_ranks(five)
            == bioseq::sequences::rank_pack_status::ok);
    REQUIRE(short_writer.finish(5u, 2u).status
            == bioseq::sequences::rank_pack_status::capacity_overflow);

    std::vector<std::uint8_t> length_units;
    reject_grow_t length_writer{grow_sink_t{length_units}};
    REQUIRE(length_writer.append_ranks(std::span(invalid).first<1>())
            == bioseq::sequences::rank_pack_status::ok);
    REQUIRE(length_writer.finish(2u, 1u).status
            == bioseq::sequences::rank_pack_status::length_mismatch);

    std::vector<std::uint8_t> count_units;
    reject_grow_t count_writer{grow_sink_t{count_units}};
    REQUIRE(count_writer.append_ranks(std::span(invalid).first<1>())
            == bioseq::sequences::rank_pack_status::ok);
    REQUIRE(count_writer.finish(1u, 2u).status
            == bioseq::sequences::rank_pack_status::unit_count_mismatch);
}

TEST_CASE("DOC0377 — admitted packed owners retain permits and stable views across moves",
          "[lazy-view][resident][rank-pack][unit]") {
    auto& admission = bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    struct cap_restore {
        bioseq::parallelism::MemoryAdmissionController* admission;
        std::uint64_t cap;
        ~cap_restore() { admission->set_index_memory_cap_bytes(cap); }
    } restore{std::addressof(admission), previous_cap};
    admission.set_index_memory_cap_bytes(0u);

    auto data = make_dna4(100u);
    range_t range(data);
    auto lazy = make_lazy_in_memory(range);
    auto source = as_lazy_sequence_view(lazy);
    std::vector<std::uint8_t> expected(73u);
    source.template to_encoded_into<false, false>(expected, 3u, 76u);

    require_materialized_width<std::uint8_t>(source, expected, 1u);
    require_materialized_width<std::uint16_t>(source, expected, 2u);
    require_materialized_width<std::uint32_t>(source, expected, 4u);
    require_materialized_width<std::uint64_t>(source, expected, 8u);
}

TEST_CASE("DOC0377 payload observer is bijective across copied views and failed admission",
          "[lazy-view][resident][allocation-observer][unit]") {
    auto data = make_dna4(257u);
    range_t range(data);
    auto lazy = make_lazy_in_memory(range);
    const facade_t source = as_lazy_sequence_view(lazy);
    payload_observer_probe probe{};
    const bioseq::storage::sequence_payload_allocation_observer observer{
        .context = std::addressof(probe),
        .publish = &collect_payload_allocation_noexcept,
        .correlation_a = 17u,
        .correlation_b = 23u,
    };
    const facade_t observed =
        source.with_payload_allocation_observer(std::addressof(observer));
    facade_t copied = observed;
    const facade_t moved{std::move(copied)};
    REQUIRE(moved.payload_allocation_observer() == std::addressof(observer));

    bioseq::parallelism::MemoryAdmissionController admission;
    const std::uint64_t retained_bytes =
        bioseq::storage::packed_region_admission_bytes<seq::dna4>(
            moved.bp_size(), 8u);
    auto permit = admission.try_reserve(retained_bytes);
    REQUIRE(permit.has_value());
    {
        auto owner = bioseq::storage::materialize_packed_region(
            moved, 0u, moved.bp_size(), 8u, std::move(*permit));
        const std::uint64_t payload_bytes =
            static_cast<std::uint64_t>(owner.descriptor().unit_count)
            * owner.descriptor().storage_unit_byte_width;
        REQUIRE(probe.count == 1u);
        REQUIRE_FALSE(probe.overflow);
        REQUIRE(probe.events[0].allocation_id != 0u);
        REQUIRE(probe.events[0].input_symbols == moved.bp_size());
        REQUIRE(probe.events[0].output_symbols == moved.bp_size());
        REQUIRE(probe.events[0].payload_bytes == payload_bytes);
        REQUIRE(probe.events[0].retained_bytes == retained_bytes);
        REQUIRE(probe.events[0].kind
                == bioseq::storage::sequence_payload_allocation_kind::
                    admitted_packed_region);
    }

    const std::uint64_t calls_before_failure = probe.count;
    auto short_permit = admission.try_reserve(retained_bytes - 1u);
    REQUIRE(short_permit.has_value());
    REQUIRE_THROWS_AS(
        bioseq::storage::materialize_packed_region(
            moved, 0u, moved.bp_size(), 8u, std::move(*short_permit)),
        std::length_error);
    REQUIRE(probe.count == calls_before_failure);

    auto second_permit = admission.try_reserve(retained_bytes);
    REQUIRE(second_permit.has_value());
    {
        auto owner = bioseq::storage::materialize_packed_region(
            observed, 0u, observed.bp_size(), 8u,
            std::move(*second_permit));
        REQUIRE(owner.descriptor().bp_length == observed.bp_size());
    }
    REQUIRE(probe.count == 2u);
    REQUIRE(probe.events[1].allocation_id != 0u);
    REQUIRE(probe.events[1].allocation_id != probe.events[0].allocation_id);
    REQUIRE(probe.events[1].payload_bytes == probe.events[0].payload_bytes);
    REQUIRE(probe.events[1].retained_bytes == probe.events[0].retained_bytes);
}

TEST_CASE("DOC0377 — admitted packed move construction holds a saturated budget and backing",
          "[lazy-view][resident][admission][saturation][lifetime][unit]") {
    using owner_type = admitted_packed_region<seq::dna4>;
    static_assert(std::is_nothrow_move_constructible_v<owner_type>);
    static_assert(!std::is_move_assignable_v<owner_type>);
    static_assert(!std::is_copy_constructible_v<owner_type>);
    static_assert(!std::is_copy_assignable_v<owner_type>);

    bioseq::parallelism::MemoryAdmissionController admission;

    auto data = make_dna4(257u);
    range_t range(data);
    auto lazy = make_lazy_in_memory(range);
    const facade_t source = as_lazy_sequence_view(lazy);
    const std::uint64_t admitted_bytes =
        bioseq::storage::packed_region_admission_bytes<seq::dna4>(
            source.bp_size(), 8u);
    REQUIRE(admitted_bytes > 0u);
    REQUIRE(admission.reserved_bytes() == 0u);

    const std::uint64_t saturated_cap = admitted_bytes;
    admission.set_index_memory_cap_bytes(saturated_cap);
    auto permit = admission.try_reserve(admitted_bytes);
    REQUIRE(permit.has_value());
    REQUIRE(admission.reserved_bytes() == saturated_cap);
    REQUIRE(admission.allocatable_after_reservations() == 0u);

    bioseq::parallelism::MemoryAdmissionFailure failure{};
    REQUIRE_FALSE(admission.try_reserve(1u, std::addressof(failure)));
    REQUIRE(failure.reason ==
            bioseq::parallelism::MemoryAdmissionFailureReason::budget_exhausted);
    REQUIRE(failure.reserved_bytes == saturated_cap);

    {
        owner_type initial = bioseq::storage::materialize_packed_region(
            source, 0u, source.bp_size(), 8u, std::move(*permit));
        const auto* const backing = initial.descriptor().units;
        REQUIRE(backing != nullptr);
        REQUIRE(admission.reserved_bytes() == saturated_cap);

        // First move happens before any facade is issued.
        owner_type moved_before_view{std::move(initial)};
        REQUIRE(moved_before_view.descriptor().units == backing);
        const facade_t issued = moved_before_view.view();

        // The second move happens after the facade captured stable_state.
        owner_type moved_after_view{std::move(moved_before_view)};
        REQUIRE(moved_after_view.descriptor().units == backing);
        REQUIRE(admission.reserved_bytes() == saturated_cap);
        REQUIRE_FALSE(admission.try_reserve(1u));

        std::vector<std::uint8_t> observed(data.size());
        issued.template to_encoded_into<false, false>(
            observed, 0u, observed.size());
        for (std::size_t index = 0u; index < data.size(); ++index) {
            REQUIRE(observed[index]
                    == static_cast<std::uint8_t>(data[index].to_rank()));
        }
        REQUIRE(moved_after_view.view().bp_size() == source.bp_size());
        REQUIRE(admission.reserved_bytes() == saturated_cap);
    }

    REQUIRE(admission.reserved_bytes() == 0u);
    auto reused = admission.try_reserve(admitted_bytes);
    REQUIRE(reused.has_value());
    REQUIRE(admission.reserved_bytes() == saturated_cap);
    reused.reset();
    REQUIRE(admission.reserved_bytes() == 0u);
}

TEST_CASE("DOC0377 — call owner releases adopted backing exactly once",
          "[lazy-view][owner][unit]") {
    mock_lazy_state state{
        .ranks = {0u, 1u, 2u, 3u},
        .worker_shareable = true,
    };
    std::size_t releases = 0u;
    {
        bioseq::storage::lazy_sequence_call_owner<seq::dna4> owner{
            make_mock_view(state), std::addressof(releases),
            &increment_release_count};
        auto borrowed = owner.view();
        auto moved = std::move(owner);
        std::array<std::uint8_t, 4> observed{};
        const std::array<std::uint8_t, 4> expected{{0u, 1u, 2u, 3u}};
        borrowed.template to_encoded_into<false, false>(
            observed, 0u, observed.size());
        REQUIRE(observed == expected);
        moved.release_noexcept();
        REQUIRE(releases == 1u);
        moved.release_noexcept();
        REQUIRE(releases == 1u);
    }
    REQUIRE(releases == 1u);
}

TEST_CASE("DOC0377 — logical comparison and hash ignore chunk and storage layout",
          "[lazy-view][content][unit]") {
    auto data = make_dna4(513u);
    using dna4_u8 = sequence_custom_alphabet<seq::dna4, std::uint8_t>;
    compact_sequence_range<dna4_u8> range8(data);
    range_t range64(data);
    auto lazy8 = make_lazy_in_memory(range8);
    auto lazy64 = make_lazy_in_memory(range64);
    auto view8 = as_lazy_sequence_view(lazy8);
    auto view64 = as_lazy_sequence_view(lazy64);
    REQUIRE(bioseq::storage::compare_sequence_views(view8, view64) == 0);
    REQUIRE(bioseq::storage::sequence_logical_hash_v1(view8, 0u)
            == bioseq::storage::sequence_logical_hash_v1(view64, 0u));
    REQUIRE(bioseq::storage::sequence_logical_hash_v1(view8, 17u)
            == bioseq::storage::sequence_logical_hash_v1(view64, 17u));
    REQUIRE(bioseq::storage::sequence_logical_hash_v1(view8, 0u)
            != bioseq::storage::sequence_logical_hash_v1(view8, 17u));

    std::vector<std::vector<std::uint8_t>> left_chunks{
        {0u}, {}, {1u, 2u}, {3u, 0u},
    };
    std::vector<std::vector<std::uint8_t>> right_chunks{
        {0u, 1u, 2u}, {}, {3u}, {0u},
    };
    REQUIRE(bioseq::storage::compare_logical_rank_streams(
                left_chunks, right_chunks) == 0);
    right_chunks.back()[0] = 1u;
    REQUIRE(bioseq::storage::compare_logical_rank_streams(
                left_chunks, right_chunks) < 0);
    right_chunks.back().clear();
    REQUIRE(bioseq::storage::compare_logical_rank_streams(
                left_chunks, right_chunks) > 0);
}

TEST_CASE("DOC0377 — transformed stream preserves subrange coordinates across chunks",
          "[lazy-view][transform][rank-pack][unit]") {
    auto data = make_dna4(777u);
    range_t range(data);
    auto flat_owner = make_lazy_in_memory(range);
    existing_fragmented_t fragmented_owner{chunked2::from_range(range)};
    const auto flat_view = as_lazy_sequence_view(flat_owner);
    const auto fragmented_view = as_lazy_sequence_view(fragmented_owner);

    std::vector<std::uint8_t> ranks(data.size());
    for (std::size_t index = 0u; index < data.size(); ++index) {
        ranks[index] = static_cast<std::uint8_t>(data[index].to_rank());
    }
    constexpr std::size_t from = 37u;
    constexpr std::size_t to = 731u;

    require_stream_transform<
        bioseq::storage::sequence_transform::identity>(
            flat_view, ranks, from, to);
    require_stream_transform<
        bioseq::storage::sequence_transform::reversed>(
            flat_view, ranks, from, to);
    require_stream_transform<
        bioseq::storage::sequence_transform::complemented>(
            flat_view, ranks, from, to);
    require_stream_transform<
        bioseq::storage::sequence_transform::reverse_complement>(
            flat_view, ranks, from, to);
    require_stream_transform<
        bioseq::storage::sequence_transform::identity>(
            fragmented_view, ranks, from, to);
    require_stream_transform<
        bioseq::storage::sequence_transform::reversed>(
            fragmented_view, ranks, from, to);
    require_stream_transform<
        bioseq::storage::sequence_transform::complemented>(
            fragmented_view, ranks, from, to);
    require_stream_transform<
        bioseq::storage::sequence_transform::reverse_complement>(
            fragmented_view, ranks, from, to);
    require_stream_transform<
        bioseq::storage::sequence_transform::reverse_complement>(
            fragmented_view, ranks, 256u, 257u);
    require_stream_transform<
        bioseq::storage::sequence_transform::identity>(
            fragmented_view, ranks, 409u, 409u);

    collecting_rank_writer writer;
    REQUIRE_THROWS_AS(
        bioseq::storage::stream_transformed_packed<
            bioseq::storage::sequence_transform::identity>(
                flat_view, 12u, 778u, writer),
        std::invalid_argument);
}

TEST_CASE("DOC0377 — access policy borrows only aligned exact residents",
          "[lazy-view][access-policy][resident][unit]") {
    auto data = make_dna4(100u);
    range_t range(data);
    auto flat_owner = make_lazy_in_memory(range);
    existing_fragmented_t fragmented_owner{chunked2::from_range(range)};
    const auto flat_view = as_lazy_sequence_view(flat_owner);
    const auto fragmented_view = as_lazy_sequence_view(fragmented_owner);
    using bioseq::storage::sequence_access_requirement;
    using bioseq::storage::sequence_access_route;

    const auto streaming = bioseq::storage::plan_sequence_access(
        fragmented_view,
        sequence_access_requirement::sequential_symbols,
        3u,
        76u);
    REQUIRE(streaming.route == sequence_access_route::stream_symbols);
    REQUIRE(streaming.admission_bytes == 0u);
    REQUIRE_FALSE(streaming.resident.has_value());

    const auto resident = bioseq::storage::plan_sequence_access(
        flat_view,
        sequence_access_requirement::contiguous_resident,
        0u,
        100u,
        8u);
    REQUIRE(resident.route == sequence_access_route::borrow_resident);
    REQUIRE(resident.resident.has_value());
    REQUIRE(resident.resident->leading_symbol_offset == 0u);
    REQUIRE(resident.admission_bytes == 0u);

    const auto unaligned = bioseq::storage::plan_sequence_access(
        flat_view,
        sequence_access_requirement::contiguous_resident,
        3u,
        76u,
        8u);
    REQUIRE(unaligned.route == sequence_access_route::materialize_admitted);
    REQUIRE_FALSE(unaligned.resident.has_value());
    REQUIRE(unaligned.admission_bytes
            == bioseq::storage::packed_region_admission_bytes<seq::dna4>(
                73u, 8u));

    const auto fragmented = bioseq::storage::plan_sequence_access(
        fragmented_view,
        sequence_access_requirement::contiguous_resident,
        0u,
        100u,
        8u);
    REQUIRE(fragmented.route
            == sequence_access_route::materialize_admitted);
    REQUIRE_FALSE(fragmented.resident.has_value());

    const auto bounded = bioseq::storage::plan_sequence_access(
        flat_view,
        sequence_access_requirement::bounded_random_window,
        0u,
        100u,
        8u);
    REQUIRE(bounded.route == sequence_access_route::materialize_admitted);
    REQUIRE_FALSE(bounded.resident.has_value());

    REQUIRE_THROWS_AS(
        bioseq::storage::plan_sequence_access(
            flat_view,
            sequence_access_requirement::contiguous_resident,
            0u,
            100u,
            3u),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        bioseq::storage::plan_sequence_access(
            flat_view,
            sequence_access_requirement::sequential_symbols,
            101u,
            100u),
        std::invalid_argument);
}

TEST_CASE("DOC0377 — streaming consumers never probe poison resident storage",
          "[lazy-view][poison][stream][admission][unit]") {
    mock_lazy_state state;
    state.ranks.resize(777u);
    for (std::size_t index = 0u; index < state.ranks.size(); ++index) {
        state.ranks[index] = static_cast<std::uint8_t>(index & 3u);
    }
    const auto view = make_mock_view(state);

    collecting_rank_writer streamed;
    bioseq::storage::stream_transformed_packed<
        bioseq::storage::sequence_transform::identity>(
            view, 0u, view.bp_size(), streamed);
    REQUIRE(streamed.ranks == state.ranks);
    REQUIRE(state.resident_probe_count == 0u);
    REQUIRE(state.callback_count == 4u);

    const auto hash = bioseq::storage::sequence_logical_hash_v1(view, 19u);
    REQUIRE(hash == bioseq::storage::sequence_logical_hash_bytes_v1(
        state.ranks, state.ranks.size(), 19u));
    REQUIRE(state.resident_probe_count == 0u);
    REQUIRE(bioseq::storage::compare_sequence_views(view, view) == 0);
    REQUIRE(state.resident_probe_count == 0u);

    const auto counts = bioseq::indexing::tally_symbol_counts(view);
    REQUIRE(counts.size() == 4u);
    REQUIRE(counts[0] + counts[1] + counts[2] + counts[3]
            == static_cast<std::int64_t>(state.ranks.size()));
    REQUIRE(state.resident_probe_count == 0u);

    const auto streaming = bioseq::storage::plan_sequence_access(
        view,
        bioseq::storage::sequence_access_requirement::sequential_symbols,
        0u,
        view.bp_size());
    REQUIRE(streaming.route
            == bioseq::storage::sequence_access_route::stream_symbols);
    REQUIRE(state.resident_probe_count == 0u);

    const auto resident = bioseq::storage::plan_sequence_access(
        view,
        bioseq::storage::sequence_access_requirement::contiguous_resident,
        0u,
        view.bp_size(),
        4u);
    REQUIRE(resident.route
            == bioseq::storage::sequence_access_route::materialize_admitted);
    /* Typed CPU planning probes only canonical u32 storage.  It must not ask
     * for an aliasing u64 byte view; admitted fallback does not re-probe. */
    REQUIRE(state.resident_probe_count == 1u);
    auto& admission = bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t reserved_before = admission.reserved_bytes();
    auto permit = admission.try_reserve(resident.admission_bytes);
    REQUIRE(permit.has_value());
    {
        auto owner = bioseq::storage::materialize_packed_region(
            view, 0u, view.bp_size(), 4u, std::move(*permit));
        REQUIRE(admission.reserved_bytes()
                == reserved_before + resident.admission_bytes);
        REQUIRE(owner.descriptor().bp_length == view.bp_size());
        REQUIRE(owner.descriptor().leading_symbol_offset == 0u);
        REQUIRE(state.resident_probe_count == 1u);
    }
    REQUIRE(admission.reserved_bytes() == reserved_before);
}

TEST_CASE("DOC0377 full packed materialization uses one optional bulk copy",
          "[lazy-view][materialize][bulk-copy][parity][unit]") {
    mock_lazy_state state;
    state.ranks.resize(777u);
    for (std::size_t index = 0u; index < state.ranks.size(); ++index) {
        state.ranks[index] = static_cast<std::uint8_t>((index * 3u) & 3u);
    }
    state.bulk_copy_enabled = true;
    const auto view = make_mock_view(state);
    auto& admission = bioseq::parallelism::MemoryAdmissionController::instance();

    const std::uint64_t full_bytes =
        bioseq::storage::packed_region_admission_bytes<seq::dna4>(
            view.bp_size(), 8u);
    auto full_permit = admission.try_reserve(full_bytes);
    REQUIRE(full_permit.has_value());
    {
        auto owner = bioseq::storage::materialize_packed_region(
            view, 0u, view.bp_size(), 8u, std::move(*full_permit));
        REQUIRE(state.bulk_copy_attempt_count == 1u);
        REQUIRE(state.bulk_copy_success_count == 1u);
        REQUIRE(state.callback_count == 0u);
        std::vector<std::uint8_t> observed(state.ranks.size());
        owner.view().template to_encoded_into<false, false>(
            observed, 0u, observed.size());
        REQUIRE(observed == state.ranks);
    }

    const sequence_size_t partial_from = 3u;
    const sequence_size_t partial_to = view.bp_size() - 5u;
    const std::uint64_t partial_bytes =
        bioseq::storage::packed_region_admission_bytes<seq::dna4>(
            partial_to - partial_from, 8u);
    auto partial_permit = admission.try_reserve(partial_bytes);
    REQUIRE(partial_permit.has_value());
    {
        auto owner = bioseq::storage::materialize_packed_region(
            view, partial_from, partial_to, 8u,
            std::move(*partial_permit));
        REQUIRE(state.bulk_copy_attempt_count == 1u);
        REQUIRE(state.bulk_copy_success_count == 1u);
        REQUIRE(state.callback_count > 0u);
        std::vector<std::uint8_t> observed(
            static_cast<std::size_t>(partial_to - partial_from));
        owner.view().template to_encoded_into<false, false>(
            observed, 0u, observed.size());
        REQUIRE(std::equal(
            observed.begin(), observed.end(),
            state.ranks.begin() + static_cast<std::ptrdiff_t>(partial_from)));
    }
}
