// main/bioseq/backend/sequences/kmers/two_stacks_minimizers_test.cpp
//
// Behaviour-locking tests for `collect_minimizers_two_stack_nthash_simd`.
// Three deliberately-chosen fixtures, each pinning a distinct code path:
//
//   (a) `dual_padded`   — 257 bp dna4 / uint32 storage. seq.padding() = 15
//                          (17 storage units × 16 letters per unit − 257 = 15
//                          trailing pad slots). 257 ≥ min_seq_for_dual
//                          (≈ 2·Lanes(d)·lpu + (k+w−1) = 2·4·16 + 24 = 152
//                          on M1 NEON), so the dual-stream path runs. The
//                          partial-chunk tail crosses the
//                          `main_counter == main_counter_only_left` mask
//                          transition exactly once.
//
//   (b) `single_nopad`  — 128 bp dna4 / uint32. seq.padding() = 0 (8 units
//                          × 16). 128 < min_seq_for_dual (≈ 152) so the
//                          single-stream path runs. Sequence shared with
//                          `MINIMIZER_TEST_DNA4_SEQ` in `kmers_test.cpp`
//                          for cross-coverage.
//
//   (c) `wrap`          — 50 bp dna4 / uint8 storage / k=4. Storage-width
//                          determines hash_bits = storage_bits / 2 = 4 →
//                          pos_mask = 0x0F → wrap_period = pos_mask − w − 1
//                          = 15 − 10 − 1 = 4. Wrap branch fires every 4
//                          steady-state steps, so a 50 bp run exercises it
//                          ~10× — the only practical way to hit the wrap
//                          arithmetic on a hardcoded test (uint32 storage
//                          would need a 65 535-bp sequence to wrap once).
//                          k=4 fits exactly into max_kmer_length = 8/2 = 4
//                          with no clamping.
//
// EXPECTED_PAIRS literals captured from the OLD impl
// (commit 59887b0b — "wip: fixing issue with nthash narrow lanes") via the
// bioseq_log<DBG> dump in NTHASH_NARROW_FIX/BASELINE_HARDCODED_OUTPUT.txt. The
// NEW impl is locked to produce a sorted-unique pair set bit-identical to
// these literals — see main/DOCS/BUGFIX/REPORT_PHASE_1__DOC0221.md for the
// before/after diff and the validation methodology.

#include "common_simd.h"
#include "common_tests.h"
#include "common.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.two_stacks;
import bioseq.sequences.kmers.hashers;
import bioseq.sequences.kmers.mm_sketch_baseline;
import bioseq.storage.lazy;
import bioseq.storage.scanner;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// =============================================================================
// Fixtures
// =============================================================================

// (a) dual-stream + non-zero padding (257 bp = 16 full units + 1 partial unit).
inline std::vector<seq::dna4> TWO_STACK_HARDCODED_DUAL_PADDED_SEQ{
    "GATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTCAC"
    "CGCAGCACGCAGCGCGAGCCCAGCACGACGCAGCACGACGCAGCACGCAGCGCGAGCCCAGCAC"
    "GATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTCAC"
    "AGCGCGAGCCCAGCACGACGCAGCACGACGCAGCACGCAGCGATCGTAGCTAGTCACTGTAGCT"
    "A"_dna4
};

// (b) single-stream + zero padding (128 bp = 8 full units of 16 letters each).
inline std::vector<seq::dna4> TWO_STACK_HARDCODED_SINGLE_NOPAD_SEQ{
    "GATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTCAC"
    "GATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTCAC"_dna4
};

// (c) wrap-period coverage (50 bp dna4 + uint8 + k=4).
inline std::vector<seq::dna4> TWO_STACK_HARDCODED_WRAP_SEQ{
    "GATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGA"_dna4
};

// =============================================================================
// EXPECTED_PAIRS — captured from the OLD impl's `extract_pairs()` (sorted
// unique (last_pos, kmer)) for each fixture.
// =============================================================================

inline constexpr std::array<std::pair<size_t, uint64_t>, 54>
TWO_STACK_HARDCODED_DUAL_PADDED_EXPECTED{{
    {18, 0x3cbd2736ULL},
    {24, 0x2bebb78bULL},
    {25, 0x2f249414ULL},
    {29, 0x17218675ULL},
    {31, 0xcc88f67ULL},
    {35, 0x30aaf70bULL},
    {45, 0x254afdc8ULL},
    {47, 0x2bebb78bULL},
    {48, 0x2f249414ULL},
    {52, 0x17218675ULL},
    {54, 0xcc88f67ULL},
    {58, 0x30aaf70bULL},
    {68, 0x316abf15ULL},
    {76, 0x839bd00ULL},
    {80, 0x2504e63fULL},
    {84, 0x3ab189f4ULL},
    {94, 0x1587e5b6ULL},
    {103, 0x3b958cafULL},
    {105, 0x1783133bULL},
    {107, 0x35770597ULL},
    {109, 0x12ab46e1ULL},
    {118, 0x2504e63fULL},
    {122, 0x3ab189f4ULL},
    {128, 0x179de8a1ULL},
    {132, 0x1a3c42d4ULL},
    {136, 0x34b1b6afULL},
    {146, 0x3cbd2736ULL},
    {152, 0x2bebb78bULL},
    {153, 0x2f249414ULL},
    {157, 0x17218675ULL},
    {159, 0xcc88f67ULL},
    {163, 0x30aaf70bULL},
    {173, 0x254afdc8ULL},
    {175, 0x2bebb78bULL},
    {176, 0x2f249414ULL},
    {180, 0x17218675ULL},
    {182, 0xcc88f67ULL},
    {186, 0x30aaf70bULL},
    {194, 0x2167ee01ULL},
    {197, 0x2239d2eaULL},
    {200, 0x3abd2666ULL},
    {201, 0x2c460f69ULL},
    {204, 0x33972e51ULL},
    {212, 0x1587e5b6ULL},
    {221, 0x3b958cafULL},
    {223, 0x1783133bULL},
    {225, 0x35770597ULL},
    {235, 0x5eca9e8ULL},
    {241, 0x14ee37cULL},
    {242, 0x19295565ULL},
    {249, 0x3e052179ULL},
    {253, 0x254afdc8ULL},
    {255, 0x2bebb78bULL},
    {256, 0xf36ec4bULL},
}};

inline constexpr std::array<std::pair<size_t, uint64_t>, 26>
TWO_STACK_HARDCODED_SINGLE_NOPAD_EXPECTED{{
    {18, 0x3cbd2736ULL},
    {24, 0x2bebb78bULL},
    {25, 0x2f249414ULL},
    {29, 0x17218675ULL},
    {31, 0xcc88f67ULL},
    {35, 0x30aaf70bULL},
    {45, 0x254afdc8ULL},
    {47, 0x2bebb78bULL},
    {48, 0x2f249414ULL},
    {52, 0x17218675ULL},
    {54, 0xcc88f67ULL},
    {58, 0x30aaf70bULL},
    {67, 0x34db7681ULL},
    {70, 0x1a3433baULL},
    {72, 0x1fb17e7aULL},
    {82, 0x3cbd2736ULL},
    {88, 0x2bebb78bULL},
    {89, 0x2f249414ULL},
    {93, 0x17218675ULL},
    {95, 0xcc88f67ULL},
    {99, 0x30aaf70bULL},
    {109, 0x254afdc8ULL},
    {111, 0x2bebb78bULL},
    {112, 0x2f249414ULL},
    {116, 0x17218675ULL},
    {118, 0xcc88f67ULL},
}};

inline constexpr std::array<std::pair<size_t, uint64_t>, 25>
TWO_STACK_HARDCODED_WRAP_EXPECTED{{
    {14, 0xeeULL},
    {17, 0x57ULL},
    {18, 0xeeULL},
    {21, 0x4fULL},
    {21, 0x7bULL},
    {23, 0xeaULL},
    {23, 0xf5ULL},
    {24, 0x3ULL},
    {25, 0x7bULL},
    {35, 0xf5ULL},
    {37, 0x7bULL},
    {37, 0xeeULL},
    {40, 0x57ULL},
    {41, 0x7bULL},
    {41, 0xeeULL},
    {42, 0xf5ULL},
    {43, 0x22ULL},
    {44, 0x7bULL},
    {46, 0xf5ULL},
    {47, 0x22ULL},
    {47, 0x4bULL},
    {48, 0x7bULL},
    {48, 0xeaULL},
    {60, 0x7bULL},
    {64, 0x7bULL},
}};

// =============================================================================
// Helper: instantiate the public dispatch with `minimap_int_hasher` and the
// requested storage width. Returns the sorted unique (last_pos, kmer) set.
// =============================================================================
template <typename Alphabet, typename UnitType>
[[nodiscard]] static std::vector<std::pair<size_t, uint64_t>>
run_two_stack_pairs(std::vector<Alphabet>& data, size_t w, size_t k) {
    using seq_type = sequence_custom_alphabet<Alphabet, UnitType>;
    auto seq = compact_sequence_range<seq_type>(data);
    sketch_collector collector;
    collect_minimizers_two_stack_nthash_simd<minimap_int_hasher,
        /*MinimizerT=*/MinimapSketchData,
        /*R=*/seq_type>(
            collector, seq, seq.begin(), seq.end(), w, k);
    return collector.extract_pairs();
}

// Compare a runtime-collected pair vector against a constexpr EXPECTED_PAIRS
// array. Logs each mismatch via bioseq_log<DBG> before asserting, so a regression
// is easy to diagnose from CI logs without re-running locally.
template <size_t N>
static void assert_pairs_match(
    const std::vector<std::pair<size_t, uint64_t>>& got,
    const std::array<std::pair<size_t, uint64_t>, N>& expected,
    const std::string& fixture_name)
{
    if (got.size() != expected.size()) {
        for (const auto& [last_pos, kmer] : got) {
            bioseq_log<DBG>("[{} GOT] last_pos={} kmer=0x{:x}",
                    fixture_name, last_pos, kmer);
        }
    }
    ASSERT(
        got.size() == expected.size(),
        fmt::format(
            "two_stack_nthash hardcoded {} pair count mismatch: got {} expected {}",
            fixture_name, got.size(), expected.size()),
        got.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        const auto& [exp_pos, exp_kmer] = expected[i];
        const auto& [got_pos, got_kmer] = got[i];
        if (got_pos != exp_pos || got_kmer != exp_kmer) {
            bioseq_log<DBG>("[{} MISMATCH at {}] got=({}, 0x{:x}) expected=({}, 0x{:x})",
                    fixture_name, i, got_pos, got_kmer, exp_pos, exp_kmer);
        }
        ASSERT(
            got_pos == exp_pos && got_kmer == exp_kmer,
            fmt::format(
                "two_stack_nthash hardcoded {} pair[{}] mismatch",
                fixture_name, i),
            got_pos, exp_pos, got_kmer, exp_kmer);
    }
}

// =============================================================================
// Tests — Phase-1 step 6. EXPECTED_PAIRS literals locked from the OLD impl.
// Each test additionally asserts determinism (same seq → same pairs across
// two independent invocations).
// =============================================================================

TEST_CASE("two_stack_nthash hardcoded — dual padded — locked",
          "[two_stack][hardcoded][dual_padded]") {
    const auto pairs = run_two_stack_pairs<seq::dna4, uint32_t>(
        TWO_STACK_HARDCODED_DUAL_PADDED_SEQ, 10, 15);
    assert_pairs_match(pairs, TWO_STACK_HARDCODED_DUAL_PADDED_EXPECTED,
        "dual_padded");

    const auto pairs_b = run_two_stack_pairs<seq::dna4, uint32_t>(
        TWO_STACK_HARDCODED_DUAL_PADDED_SEQ, 10, 15);
    ASSERT(pairs == pairs_b,
        "two_stack_nthash must be deterministic on the dual_padded fixture",
        pairs, pairs_b);
}

TEST_CASE("two_stack_nthash hardcoded — single no-pad — locked",
          "[two_stack][hardcoded][single_nopad]") {
    const auto pairs = run_two_stack_pairs<seq::dna4, uint32_t>(
        TWO_STACK_HARDCODED_SINGLE_NOPAD_SEQ, 10, 15);
    assert_pairs_match(pairs, TWO_STACK_HARDCODED_SINGLE_NOPAD_EXPECTED,
        "single_nopad");

    const auto pairs_b = run_two_stack_pairs<seq::dna4, uint32_t>(
        TWO_STACK_HARDCODED_SINGLE_NOPAD_SEQ, 10, 15);
    ASSERT(pairs == pairs_b,
        "two_stack_nthash must be deterministic on the single_nopad fixture",
        pairs, pairs_b);
}

TEST_CASE("two_stack_nthash hardcoded — wrap — locked",
          "[two_stack][hardcoded][wrap]") {
    // dna4 + uint8 + k=4. wrap_period = 4 → ~10 wrap fires over 50 bp.
    const auto pairs = run_two_stack_pairs<seq::dna4, uint8_t>(
        TWO_STACK_HARDCODED_WRAP_SEQ, 10, 4);
    assert_pairs_match(pairs, TWO_STACK_HARDCODED_WRAP_EXPECTED, "wrap");

    const auto pairs_b = run_two_stack_pairs<seq::dna4, uint8_t>(
        TWO_STACK_HARDCODED_WRAP_SEQ, 10, 4);
    ASSERT(pairs == pairs_b,
        "two_stack_nthash must be deterministic on the wrap fixture",
        pairs, pairs_b);
}

// =============================================================================
// Continuation/range prototype parity.  These comparisons deliberately avoid
// `extract_pairs()`: sorting and uniquing would hide fragment-boundary duplicate
// emissions, strand drift, and ordering changes.  The oracle and SIMD result
// are compared as exact ordered (hash, position, strand) records.
// =============================================================================

struct exact_minimizer {
    std::uint64_t hash;
    std::uint64_t position;
    std::uint64_t strand;

    [[nodiscard]] bool operator==(const exact_minimizer&) const = default;
};

// A 64-bit test representation with a deliberately tiny two-bit hash field.
// For K=1 the canonical A/T hash truncates to all ones, exercising the real
// hash-is-sentinel contract without a probabilistic 35-bit preimage search.
struct sentinel_hash_minimizer {
    using storage_t = std::uint64_t;
    static constexpr unsigned pos_shift = 1;
    static constexpr unsigned hash_shift = 29;
    static constexpr storage_t hash_field_mask = 0x3u;
    static constexpr storage_t pos_field_mask = (storage_t{1} << 28) - 1;
    static constexpr storage_t hash_mask = hash_field_mask << hash_shift;

    storage_t value_ = 0;

    constexpr sentinel_hash_minimizer() = default;
    constexpr sentinel_hash_minimizer(
        std::uint64_t hash,
        std::uint64_t position,
        std::uint64_t strand)
        : value_(((hash & hash_field_mask) << hash_shift)
            | ((position & pos_field_mask) << pos_shift)
            | (strand & 1u)) {}

    [[nodiscard]] static constexpr sentinel_hash_minimizer getMax() {
        sentinel_hash_minimizer result;
        result.value_ = hash_mask;
        return result;
    }
    [[nodiscard]] static constexpr sentinel_hash_minimizer from_raw_storage(
        storage_t raw) {
        sentinel_hash_minimizer result;
        result.value_ = raw;
        return result;
    }
    [[nodiscard]] constexpr storage_t raw_value() const { return value_; }
    [[nodiscard]] constexpr std::uint64_t kmer() const {
        return (value_ & hash_mask) >> hash_shift;
    }
    [[nodiscard]] constexpr std::uint64_t last_pos() const {
        return (value_ >> pos_shift) & pos_field_mask;
    }
    [[nodiscard]] constexpr std::uint64_t strand() const {
        return value_ & 1u;
    }
    [[nodiscard]] constexpr bool is_max() const {
        return (value_ & hash_mask) == hash_mask;
    }
    constexpr void set_max() { value_ = hash_mask; }
    [[nodiscard]] constexpr auto operator<=>(
        const sentinel_hash_minimizer& other) const {
        return (value_ & hash_mask) <=> (other.value_ & hash_mask);
    }
    [[nodiscard]] constexpr bool operator==(
        const sentinel_hash_minimizer& other) const {
        return (value_ & hash_mask) == (other.value_ & hash_mask);
    }
    [[nodiscard]] static constexpr bool does_sketch_match(
        const sentinel_hash_minimizer& left,
        const sentinel_hash_minimizer& right) {
        return ((left.value_ ^ right.value_) & hash_mask) == 0
            && left.value_ != right.value_;
    }
};

using continuation_sequence =
    sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using continuation_range = compact_sequence_range<continuation_sequence>;

template<class MinimizerT>
[[nodiscard]] static std::vector<exact_minimizer> exact_records(
    const sketch_collector_t<MinimizerT>& collector,
    std::size_t position_base = 0)
{
    std::vector<exact_minimizer> records;
    const auto& sketches = collector.extract_sketches_data();
    records.reserve(sketches.size());
    for (const auto& sketch : sketches) {
        records.push_back({
            .hash = sketch.kmer(),
            .position = sketch.last_pos() + position_base,
            .strand = sketch.strand(),
        });
    }
    return records;
}

static void assert_exact_records(
    const std::vector<exact_minimizer>& got,
    const std::vector<exact_minimizer>& expected,
    const std::string& fixture)
{
    ASSERT(got.size() == expected.size(),
        fmt::format("{}: exact minimizer multiplicity mismatch: got {} expected {}",
            fixture, got.size(), expected.size()),
        got.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        ASSERT(got[i] == expected[i],
            fmt::format(
                "{}: exact minimizer[{}] mismatch: got (0x{:x},{},{}) expected (0x{:x},{},{})",
                fixture, i,
                got[i].hash, got[i].position, got[i].strand,
                expected[i].hash, expected[i].position, expected[i].strand),
            got[i].hash, expected[i].hash,
            got[i].position, expected[i].position,
            got[i].strand, expected[i].strand);
    }
}

[[nodiscard]] static std::vector<seq::dna4> continuation_fixture(
    std::size_t size)
{
    // Deterministic non-periodic ranks with both forward- and reverse-winning
    // canonical kmers.  No random library/global state enters the test.
    std::vector<seq::dna4> result;
    result.reserve(size);
    std::uint32_t state = 0x9E3779B9u;
    for (std::size_t i = 0; i < size; ++i) {
        state = state * 1664525u + 1013904223u;
        seq::dna4 symbol{};
        symbol.assign_rank(static_cast<std::uint8_t>((state >> 29) & 0x3u));
        result.push_back(symbol);
    }
    return result;
}

template<class MinimizerT = MinimapSketchData>
[[nodiscard]] static std::vector<exact_minimizer> scalar_exact_oracle(
    std::vector<seq::dna4>& data,
    std::size_t w,
    std::size_t k,
    std::size_t begin_bp = 0,
    std::size_t end_bp = std::numeric_limits<std::size_t>::max(),
    std::size_t emitted_position_base = 0)
{
    constexpr std::size_t lpu = continuation_range::traits::letters_per_unit;
    if (end_bp == std::numeric_limits<std::size_t>::max()) {
        end_bp = data.size();
    }
    ASSERT(begin_bp <= end_bp && end_bp <= data.size(),
        "scalar bounded-range oracle bounds must be ordered and owned",
        begin_bp, end_bp, data.size());
    ASSERT(begin_bp % lpu == 0
        && (end_bp % lpu == 0 || end_bp == data.size()),
        "scalar bounded-range oracle uses aligned bounds or the physical end",
        begin_bp, end_bp, data.size(), lpu);

    // compact_sequence_range_base::iter() captures the current packed unit by
    // dereferencing its input iterator.  That contract is valid for every
    // non-empty oracle span, but an empty span at the physical end has
    // begin == end == cend(), so constructing either BP iterator would
    // dereference cend().  The scalar minimizer result for an empty span is
    // mathematically empty; return it before entering that non-empty iterator
    // API.  This is an oracle guard, not a continuation/backend fallback.
    if (begin_bp == end_bp) return {};

    continuation_range sequence(data);
    auto begin = sequence.begin();
    auto end = sequence.end();
    std::advance(begin, static_cast<std::ptrdiff_t>(begin_bp / lpu));
    if (end_bp != data.size()) {
        end = sequence.begin();
        std::advance(end, static_cast<std::ptrdiff_t>(end_bp / lpu));
    }
    sketch_collector_t<MinimizerT> collector;
    collect_minimizers_view<minimap_int_hasher, MinimizerT>(
        collector, sequence, begin, end, w, k);
    return exact_records(collector, emitted_position_base);
}

struct continuation_result {
    std::vector<exact_minimizer> records;
    TwoStackExecutionProof proof;
};

template<class MinimizerT>
struct rejecting_raw_collector {
    using minimizer_type = MinimizerT;
    using storage_type = typename MinimizerT::storage_t;

    [[nodiscard]] bool append_raw_storage(
        const storage_type*,
        std::size_t count) {
        return count == 0;
    }
};

static void assert_exact_simd_proof(
    const TwoStackExecutionProof& proof,
    const std::string& fixture,
    bool expect_candidate_batches = true)
{
    if (expect_candidate_batches) {
        ASSERT(proof.vector_batches > 0
            && proof.consecutive_kmer_batches > 0
            && proof.canonical_scramble_batches > 0
            && proof.window_reduction_batches > 0
            && proof.event_generation_batches > 0
            && proof.stable_compress_batches > 0
            && proof.packed_decode_batches > 0
            && proof.packed_decode_lanes > 0
            && proof.row_transpose_batches > 0
            && proof.row_vector_stores > 0,
            fmt::format("{}: every chronological SIMD stage must execute",
                fixture));
    }
    ASSERT(proof.bridge_symbols_staged == 0
        && proof.bridge_vector_runs == 0,
        fmt::format("{}: persistent continuation must not replay a bridge",
            fixture));
    ASSERT(proof.scalar_backend_branches == 0
        && proof.scalar_tail_symbols == 0
        && proof.scalar_hash_symbols == 0
        && proof.scalar_window_transitions == 0
        && proof.scalar_event_records == 0
        && proof.scalar_sort_records == 0
        && proof.scalar_dedup_records == 0
        && proof.fail_closed_rejections == 0,
        fmt::format("{}: forbidden scalar/postprocess counters must stay zero",
            fixture));
    ASSERT(proof.scalar_orchestration_steps > 0,
        fmt::format("{}: scalar orchestration must be reported truthfully",
            fixture));
    ASSERT(proof.packed_decode_lanes == proof.bp_consumed
        && proof.fragment_symbols_staged == proof.bp_consumed,
        fmt::format("{}: every consumed base must pass through SIMD packed decode",
            fixture));
    ASSERT(proof.accepted(expect_candidate_batches),
        fmt::format("{}: ExactSimd operation proof must be accepted", fixture));
}

[[nodiscard]] static std::vector<exact_minimizer>
baseline_dna5_ordered_oracle(
    const std::vector<seq::dna5>& data,
    std::size_t w,
    std::size_t k)
{
    std::string bases;
    bases.reserve(data.size());
    for (const seq::dna5 symbol : data) bases.push_back(symbol.to_char());
    const auto entries = ::bioseq::mm_sketch_baseline_ns::mm_sketch(
        bases.data(),
        static_cast<int>(bases.size()),
        static_cast<int>(w),
        static_cast<int>(k));
    std::vector<exact_minimizer> result;
    result.reserve(entries.size());
    for (const auto& entry : entries) {
        result.push_back({
            .hash = entry.x >> 8,
            .position = (entry.y >> 1) & std::uint64_t{0xFFFFFFFFu},
            .strand = entry.y & 1u,
        });
    }
    return result;
}

template<class Sequence>
[[nodiscard]] static continuation_result run_exact_driver(
    Sequence& source,
    std::size_t w,
    std::size_t k)
{
    sketch_collector collector;
    const auto exact =
        collect_minimizers_two_stack_nthash_exact_range_checked<
            0, 0, MinimapSketchData>(
                collector,
                source,
                0,
                static_cast<std::size_t>(source.size()),
                w,
                k,
                0);
    ASSERT(exact.ok(),
        "valid nucleotide ExactSimd driver must succeed",
        two_stack_status_name(exact.status));
    return {
        .records = exact_records(collector),
        .proof = exact.proof,
    };
}

[[nodiscard]] static std::vector<seq::dna5> make_dna5(
    std::string_view bases)
{
    std::vector<seq::dna5> result;
    result.reserve(bases.size());
    for (const char base : bases) {
        seq::dna5 symbol{};
        symbol.assign_char(base);
        result.push_back(symbol);
    }
    return result;
}

[[nodiscard]] static std::vector<seq::dna5> reverse_complement_dna5(
    const std::vector<seq::dna5>& input)
{
    std::vector<seq::dna5> result;
    result.reserve(input.size());
    for (auto iter = input.rbegin(); iter != input.rend(); ++iter) {
        const char complement = [base = iter->to_char()] {
            switch (base) {
                case 'A': return 'T';
                case 'C': return 'G';
                case 'G': return 'C';
                case 'T': return 'A';
                default: return 'N';
            }
        }();
        seq::dna5 symbol{};
        symbol.assign_char(complement);
        result.push_back(symbol);
    }
    return result;
}

template<class StorageUnit>
static void assert_exact_driver_storage_width(
    std::vector<seq::dna4>& data,
    std::size_t lo_unit,
    std::size_t hi_unit,
    const std::string& fixture)
{
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, StorageUnit>;
    using range_type = compact_sequence_range<sequence_type>;
    range_type source(data);
    ASSERT(hi_unit <= static_cast<std::size_t>(source.size()),
        fmt::format("{}: test range must fit packed source", fixture));

    sketch_collector exact_collector;
    const auto exact =
        collect_minimizers_two_stack_nthash_exact_range_checked<
            0, 0, MinimapSketchData>(
                exact_collector,
                source,
                lo_unit,
                hi_unit,
                5,
                7,
                0);
    ASSERT(exact.ok(),
        fmt::format("{}: ExactSimd checked driver must accept storage width",
            fixture),
        two_stack_status_name(exact.status));

    sketch_collector scalar_collector;
    auto begin = source.cbegin();
    begin += static_cast<decltype(source.size())>(lo_unit);
    auto end = source.cbegin();
    end += static_cast<decltype(source.size())>(hi_unit);
    collect_minimizers_view<minimap_int_hasher, MinimapSketchData>(
        scalar_collector, source, begin, end, 5, 7);
    assert_exact_records(
        exact_records(exact_collector),
        exact_records(scalar_collector),
        fixture);
    assert_exact_simd_proof(exact.proof, fixture);
}

TEST_CASE("ExactSimd DNA5 keeps T as nt4 and preserves canonical strand parity",
          "[two_stack][continuation][exact][dna5][nt4][reverse-complement]") {
    // K=15 is the production/report_76 shape and is odd, so a DNA reverse-
    // complement palindrome is impossible. This makes ordered equality an
    // unqualified test of minimap2's ring/tie/N chronology rather than the
    // separately documented two-stack policy of advancing a max slot for an
    // even-K symmetric k-mer that sketch.c `continue`s over.
    constexpr std::size_t w = 10;
    constexpr std::size_t k = 15;
    std::string bases;
    for (std::size_t repeat = 0; repeat < 12; ++repeat) {
        bases += "ACGTTGCAAGTCCGTATGCA";
    }
    auto forward = make_dna5(bases);
    auto reverse = reverse_complement_dna5(forward);
    using dna5_u64 = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    using dna5_range = compact_sequence_range<dna5_u64>;

    dna5_range forward_range(forward);
    const auto forward_exact = run_exact_driver(forward_range, w, k);
    const auto forward_expected = baseline_dna5_ordered_oracle(forward, w, k);
    assert_exact_records(
        forward_exact.records, forward_expected, "dna5 T forward");
    assert_exact_simd_proof(forward_exact.proof, "dna5 T forward");
    ASSERT(forward_exact.proof.ambiguous_symbols == 0
        && forward_exact.proof.ambiguous_reset_batches == 0,
        "dna5 T rank 4 must normalize to nt4 code 3, never the N sentinel",
        forward_exact.proof.ambiguous_symbols,
        forward_exact.proof.ambiguous_reset_batches);

    dna5_range reverse_range(reverse);
    const auto reverse_exact = run_exact_driver(reverse_range, w, k);
    const auto reverse_expected = baseline_dna5_ordered_oracle(reverse, w, k);
    assert_exact_records(
        reverse_exact.records, reverse_expected, "dna5 reverse complement");
    assert_exact_simd_proof(
        reverse_exact.proof, "dna5 reverse complement");
    ASSERT(std::ranges::any_of(
            forward_exact.records,
            [](const exact_minimizer& value) { return value.strand == 0; })
        && std::ranges::any_of(
            forward_exact.records,
            [](const exact_minimizer& value) { return value.strand == 1; }),
        "canonical parity fixture must exercise both strand decisions");
}

TEST_CASE("ExactSimd DNA5 N reset survives packed and scanner chunk boundaries",
          "[two_stack][continuation][exact][dna5][nt4][fragmented]") {
    // Keep production K=15 for the same reason as the preceding test: no
    // symmetric-kmer policy exception is possible, so the ordered baseline
    // comparison below must be exact.
    constexpr std::size_t w = 10;
    constexpr std::size_t k = 15;
    constexpr std::size_t size = 311;
    std::string bases(size, 'A');
    constexpr std::string_view motif = "ACGTTGCAGATCCGTA";
    for (std::size_t index = 0; index < bases.size(); ++index) {
        bases[index] = motif[index % motif.size()];
    }
    // dna5/u64 packs 21 bases per word. A two-word scanner fragment therefore
    // ends at bp 42; exercise N immediately before, on, and after that boundary,
    // plus prefix/tail and interior SIMD-lane positions.
    constexpr std::array<std::size_t, 9> n_positions{
        0, 6, 41, 42, 43, 127, 211, 289, size - 1};
    for (const std::size_t position : n_positions) bases[position] = 'N';
    auto data = make_dna5(bases);
    const auto expected = baseline_dna5_ordered_oracle(data, w, k);

    using dna5_u8 = sequence_custom_alphabet<seq::dna5, std::uint8_t>;
    using dna5_u8_range = compact_sequence_range<dna5_u8>;
    dna5_u8_range resident(data);
    const auto resident_exact = run_exact_driver(resident, w, k);
    assert_exact_records(
        resident_exact.records, expected, "dna5 u8 packed reset");
    assert_exact_simd_proof(
        resident_exact.proof, "dna5 u8 packed reset");
    ASSERT(resident_exact.proof.ambiguous_symbols == n_positions.size()
        && resident_exact.proof.ambiguous_reset_batches > 0,
        "every dna5 N must produce one attested reset",
        resident_exact.proof.ambiguous_symbols,
        n_positions.size(),
        resident_exact.proof.ambiguous_reset_batches);

    using dna5_u64 = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    using dna5_u64_range = compact_sequence_range<dna5_u64>;
    using chunked =
        bioseq::storage::sequence_scanner_test_chunked<std::uint64_t, 2>;
    dna5_u64_range packed(data);
    bioseq::storage::lazy_compact_sequence<dna5_u64, chunked> fragmented{
        chunked::from_range(packed)};
    const auto fragmented_exact = run_exact_driver(fragmented, w, k);
    assert_exact_records(
        fragmented_exact.records, expected, "dna5 scanner boundary reset");
    assert_exact_simd_proof(
        fragmented_exact.proof, "dna5 scanner boundary reset");
    ASSERT(fragmented_exact.records == resident_exact.records,
        "resident and scanner-fragmented DNA5 routes must be byte-identical");
    ASSERT(fragmented_exact.proof.ambiguous_symbols == n_positions.size(),
        "fragment boundaries must not drop or replay an N reset",
        fragmented_exact.proof.ambiguous_symbols,
        n_positions.size());
}

template<
    std::size_t W,
    std::size_t K,
    class MinimizerT = MinimapSketchData>
[[nodiscard]] static continuation_result run_continuation_fragments(
    const std::vector<seq::dna4>& data,
    const std::vector<std::pair<std::size_t, std::size_t>>& fragments,
    std::size_t runtime_w,
    std::size_t runtime_k,
    std::size_t emitted_position_base = 0)
{
    using traits = continuation_range::traits;
    TwoStackContinuationState<W, K, traits> state;
    sketch_collector_t<MinimizerT> collector;
    const auto reset_status = reset_two_stack_nthash(
        state, runtime_w, runtime_k, emitted_position_base);
    ASSERT(reset_status == TwoStackContinuationStatus::ok,
        "valid continuation reset must succeed",
        static_cast<std::uint8_t>(reset_status));

    for (const auto& [begin, end] : fragments) {
        ASSERT(begin <= end && end <= data.size(),
            "invalid continuation test fragment", begin, end, data.size());
        std::vector<seq::dna4> fragment_data(
            data.begin() + static_cast<std::ptrdiff_t>(begin),
            data.begin() + static_cast<std::ptrdiff_t>(end));
        continuation_range fragment(fragment_data);
        const auto consume_status =
            consume_two_stack_nthash_fragment<W, K, MinimizerT>(
            state, collector, fragment,
            /*fragment_begin_bp=*/begin,
            /*consume_begin_bp=*/begin,
            /*consume_end_bp=*/end);
        ASSERT(consume_status == TwoStackContinuationStatus::ok,
            "valid continuation fragment must be accepted",
            begin, end, static_cast<std::uint8_t>(consume_status));
    }

    const auto finalized = finalize_two_stack_nthash(state, collector);
    ASSERT(finalized.ok(),
        "valid continuation finalize must succeed",
        static_cast<std::uint8_t>(finalized.status));
    return {
        .records = exact_records(collector),
        .proof = finalized.proof,
    };
}

TEST_CASE("two_stack continuation — exact runtime and constexpr contiguous parity",
          "[two_stack][continuation][exact][contiguous]") {
    constexpr std::size_t W = 5;
    constexpr std::size_t K = 7;
    auto data = continuation_fixture(197);
    const auto expected = scalar_exact_oracle(data, W, K);
    const std::vector<std::pair<std::size_t, std::size_t>> one_fragment{
        {0, data.size()}
    };

    const auto runtime = run_continuation_fragments<0, 0>(
        data, one_fragment, W, K);
    const auto constexpr_result = run_continuation_fragments<W, K>(
        data, one_fragment, W, K);
    assert_exact_records(runtime.records, expected, "runtime contiguous");
    assert_exact_records(constexpr_result.records, expected,
        "constexpr contiguous");
    ASSERT(runtime.records == constexpr_result.records,
        "runtime and constexpr continuation routes must be byte-identical");
    ASSERT(runtime.proof.bp_consumed == data.size(),
        "runtime continuation proof must count logical input once",
        runtime.proof.bp_consumed, data.size());
    ASSERT(runtime.proof.vector_batches > 0,
        "runtime continuation must execute vector batches",
        runtime.proof.vector_batches);
    ASSERT(runtime.proof.masked_tail_lanes > 0,
        "contiguous fixture must exercise the masked SIMD final lane group",
        runtime.proof.masked_tail_lanes);
    ASSERT(runtime.proof.scalar_backend_branches == 0
        && runtime.proof.scalar_tail_symbols == 0,
        "continuation proof must report no scalar biological work",
        runtime.proof.scalar_backend_branches,
        runtime.proof.scalar_tail_symbols);
    assert_exact_simd_proof(runtime.proof, "runtime contiguous");
    assert_exact_simd_proof(constexpr_result.proof, "constexpr contiguous");
    ASSERT(runtime.proof.minimizers_emitted == runtime.records.size(),
        "proof emission count must equal exact runtime record multiplicity",
        runtime.proof.minimizers_emitted, runtime.records.size());
}

TEST_CASE("two_stack continuation — every early split has exact ownership",
          "[two_stack][continuation][exact][split_sweep]") {
    constexpr std::size_t W = 5;
    constexpr std::size_t K = 7;
    auto data = continuation_fixture(113);
    const auto expected = scalar_exact_oracle(data, W, K);

    for (std::size_t split = 0; split <= K + W; ++split) {
        const std::vector<std::pair<std::size_t, std::size_t>> fragments{
            {0, split},
            {split, data.size()},
        };
        const auto runtime = run_continuation_fragments<0, 0>(
            data, fragments, W, K);
        const auto constexpr_result = run_continuation_fragments<W, K>(
            data, fragments, W, K);
        assert_exact_records(runtime.records, expected,
            fmt::format("runtime split {}", split));
        assert_exact_records(constexpr_result.records, expected,
            fmt::format("constexpr split {}", split));
        ASSERT(runtime.proof.bp_consumed == data.size(),
            "split replay must not double-count bridge symbols",
            split, runtime.proof.bp_consumed, data.size());
        ASSERT(runtime.proof.scalar_backend_branches == 0
            && runtime.proof.scalar_tail_symbols == 0,
            "split replay must not perform scalar biological work",
            split,
            runtime.proof.scalar_backend_branches,
            runtime.proof.scalar_tail_symbols);
        assert_exact_simd_proof(runtime.proof,
            fmt::format("runtime split {}", split));
        assert_exact_simd_proof(constexpr_result.proof,
            fmt::format("constexpr split {}", split));
    }
}

TEST_CASE("two_stack continuation — empty and one-symbol fragments",
          "[two_stack][continuation][exact][tiny_fragments]") {
    constexpr std::size_t W = 5;
    constexpr std::size_t K = 7;
    auto data = continuation_fixture(79);
    const auto expected = scalar_exact_oracle(data, W, K);
    std::vector<std::pair<std::size_t, std::size_t>> fragments;
    fragments.emplace_back(0, 0);
    for (std::size_t i = 0; i < data.size(); ++i) {
        fragments.emplace_back(i, i + 1);
        if ((i % 7) == 0) fragments.emplace_back(i + 1, i + 1);
    }
    const auto result = run_continuation_fragments<W, K>(
        data, fragments, W, K);
    assert_exact_records(result.records, expected,
        "empty/one-symbol fragments");
    ASSERT(result.proof.bp_consumed == data.size(),
        "empty fragments must not change logical bp count",
        result.proof.bp_consumed, data.size());
    ASSERT(result.proof.fragment_symbols_staged == data.size(),
        "one-symbol fragmentation must stage each logical symbol exactly once",
        result.proof.fragment_symbols_staged, data.size());
    assert_exact_simd_proof(result.proof, "empty/one-symbol fragments");
}

TEST_CASE("two_stack continuation — randomized fragmentation exact parity",
          "[two_stack][continuation][exact][random_fragments]") {
    constexpr std::size_t W = 5;
    constexpr std::size_t K = 7;
    auto data = continuation_fixture(263);
    const auto expected = scalar_exact_oracle(data, W, K);

    for (std::uint32_t seed = 1; seed <= 16; ++seed) {
        std::vector<std::pair<std::size_t, std::size_t>> fragments;
        std::size_t begin = 0;
        std::uint32_t rng = seed * 0x45D9F3Bu;
        while (begin < data.size()) {
            rng = rng * 1664525u + 1013904223u;
            const std::size_t length = 1 + ((rng >> 24) % 31u);
            const std::size_t end = std::min(data.size(), begin + length);
            fragments.emplace_back(begin, end);
            if ((rng & 3u) == 0) fragments.emplace_back(end, end);
            begin = end;
        }
        const auto result = run_continuation_fragments<0, 0>(
            data, fragments, W, K);
        assert_exact_records(result.records, expected,
            fmt::format("random fragmentation seed {}", seed));
        ASSERT(result.proof.bp_consumed == data.size(),
            "random fragmentation proof must count each symbol once",
            seed, result.proof.bp_consumed, data.size());
        assert_exact_simd_proof(result.proof,
            fmt::format("random fragmentation seed {}", seed));
    }
}

TEST_CASE("two_stack continuation — bounded range preserves global coordinates",
          "[two_stack][continuation][exact][bounded_range]") {
    constexpr std::size_t W = 5;
    constexpr std::size_t K = 7;
    constexpr std::size_t begin = 64;
    constexpr std::size_t end = 224;
    constexpr std::size_t output_base = 9000;
    auto data = continuation_fixture(288);
    const auto expected = scalar_exact_oracle(
        data, W, K, begin, end, output_base);

    continuation_range source(data);
    using traits = continuation_range::traits;
    TwoStackContinuationState<W, K, traits> bounded_state;
    sketch_collector bounded_collector;
    ASSERT(reset_two_stack_nthash(bounded_state, W, K, output_base)
            == TwoStackContinuationStatus::ok,
        "bounded reset must succeed");
    const auto bounded_consume_status =
        consume_two_stack_nthash_fragment<W, K, MinimapSketchData>(
        bounded_state, bounded_collector, source,
        /*fragment_begin_bp=*/0,
        /*consume_begin_bp=*/begin,
        /*consume_end_bp=*/end);
    ASSERT(bounded_consume_status == TwoStackContinuationStatus::ok,
        "bounded consume must succeed");
    const auto bounded_finalize =
        finalize_two_stack_nthash(bounded_state, bounded_collector);
    ASSERT(bounded_finalize.ok(), "bounded finalize must succeed");
    const auto& bounded_proof = bounded_finalize.proof;
    assert_exact_records(exact_records(bounded_collector), expected,
        "zero-copy bounded source range");
    ASSERT(bounded_proof.bp_consumed == end - begin,
        "bounded proof must count only owned symbols",
        bounded_proof.bp_consumed, end - begin);

    std::vector<seq::dna4> bounded_data(
        data.begin() + static_cast<std::ptrdiff_t>(begin),
        data.begin() + static_cast<std::ptrdiff_t>(end));
    const std::vector<std::pair<std::size_t, std::size_t>> fragments{
        {0, 37}, {37, 91}, {91, bounded_data.size()}
    };
    const auto result = run_continuation_fragments<W, K>(
        bounded_data, fragments, W, K, output_base);
    assert_exact_records(result.records, expected,
        "bounded fragmented range");
}

TEST_CASE("two_stack continuation — tie minimum crossing split is owned once",
          "[two_stack][continuation][exact][ties][ownership]") {
    constexpr std::size_t W = 5;
    constexpr std::size_t K = 7;
    std::vector<seq::dna4> data;
    data.reserve(96);
    for (std::size_t i = 0; i < 96; ++i) {
        seq::dna4 symbol{};
        // Long equal-kmer runs on both sides of the boundary, with a T-rich
        // island that also forces reverse-strand winners.
        const std::uint8_t rank =
            (i >= 38 && i < 57) ? 3u : static_cast<std::uint8_t>((i / 16) & 1u);
        symbol.assign_rank(rank);
        data.push_back(symbol);
    }
    const auto expected = scalar_exact_oracle(data, W, K);
    const std::vector<std::pair<std::size_t, std::size_t>> fragments{
        {0, 48}, {48, data.size()}
    };
    const auto result = run_continuation_fragments<W, K>(
        data, fragments, W, K);
    assert_exact_records(result.records, expected,
        "tie/reverse ownership boundary");
    assert_exact_simd_proof(result.proof,
        "tie/reverse ownership boundary");
    ASSERT(result.proof.first_window_tie_events > 0,
        "tie fixture must cross the exact first-full-window transition",
        result.proof.first_window_tie_events);
}

TEST_CASE("two_stack continuation — every short length and final flush",
          "[two_stack][continuation][exact][short][final_flush]") {
    constexpr std::size_t W = 5;
    constexpr std::size_t K = 7;
    for (std::size_t length = 0; length <= K + W + 8; ++length) {
        auto data = continuation_fixture(length);
        const auto expected = scalar_exact_oracle(data, W, K);
        const std::vector<std::pair<std::size_t, std::size_t>> fragments{
            {0, length}
        };
        const auto runtime = run_continuation_fragments<0, 0>(
            data, fragments, W, K);
        const auto constexpr_result = run_continuation_fragments<W, K>(
            data, fragments, W, K);
        assert_exact_records(runtime.records, expected,
            fmt::format("runtime short length {}", length));
        assert_exact_records(constexpr_result.records, expected,
            fmt::format("constexpr short length {}", length));
        ASSERT(runtime.proof == constexpr_result.proof,
            "runtime and constexpr continuation must preserve identical state "
            "transitions for every short length",
            length);
        assert_exact_simd_proof(runtime.proof,
            fmt::format("runtime short length {}", length),
            /*expect_candidate_batches=*/length >= K);
        ASSERT(runtime.proof.bp_consumed == length,
            "short-length proof must count the logical input exactly",
            length, runtime.proof.bp_consumed);
        ASSERT(runtime.proof.final_flush_records
                == (expected.empty() ? 0u : 1u),
            "short-length final flush must match the scalar pending-current rule",
            length, runtime.proof.final_flush_records, expected.size());
        if (length < K) {
            ASSERT(expected.empty()
                && runtime.records.empty()
                && runtime.proof.minimizers_emitted == 0
                && runtime.proof.final_flush_records == 0,
                "a sub-K input cannot create or flush a minimizer",
                length, runtime.proof.minimizers_emitted,
                runtime.proof.final_flush_records);
            ASSERT(runtime.proof.vector_batches == 0
                && runtime.proof.consecutive_kmer_batches == 0
                && runtime.proof.canonical_scramble_batches == 0
                && runtime.proof.window_reduction_batches == 0
                && runtime.proof.row_transpose_batches == 0
                && runtime.proof.row_vector_stores == 0,
                "a sub-K input must retain only the bounded code tail and never "
                "enter the chronological candidate kernel",
                length, runtime.proof.vector_batches);
        }
        if (length == 0) {
            ASSERT(runtime.proof.bp_consumed == 0
                && runtime.proof.fragment_symbols_staged == 0
                && runtime.proof.packed_decode_batches == 0
                && runtime.proof.packed_decode_lanes == 0
                && runtime.proof.accepted(),
                "empty continuation must finalize as an accepted no-work route "
                "without touching packed storage");
        }
    }
}

TEST_CASE("two_stack continuation — even-K palindromes consume sentinel slots",
          "[two_stack][continuation][exact][palindrome]") {
    constexpr std::size_t W = 3;
    constexpr std::size_t K = 4;
    std::vector<seq::dna4> data;
    data.reserve(96);
    constexpr std::array<std::uint8_t, 4> acgt{0, 1, 2, 3};
    for (std::size_t i = 0; i < 96; ++i) {
        seq::dna4 symbol{};
        symbol.assign_rank(acgt[i % acgt.size()]);
        data.push_back(symbol);
    }
    const auto expected = scalar_exact_oracle(data, W, K);
    const std::vector<std::pair<std::size_t, std::size_t>> fragments{
        {0, 1}, {1, 9}, {9, 10}, {10, 47}, {47, data.size()}
    };
    const auto result = run_continuation_fragments<W, K>(
        data, fragments, W, K);
    assert_exact_records(result.records, expected,
        "even-K palindrome sentinel slots");
    assert_exact_simd_proof(result.proof,
        "even-K palindrome sentinel slots");
    ASSERT(result.proof.palindrome_lanes > 0,
        "palindrome fixture must execute the SIMD palindrome mask",
        result.proof.palindrome_lanes);
}

TEST_CASE("two_stack continuation — equal entering multiplicity is preserved",
          "[two_stack][continuation][exact][ties][multiplicity]") {
    constexpr std::size_t W = 5;
    constexpr std::size_t K = 7;
    std::vector<seq::dna4> data(64);
    for (auto& symbol : data) symbol.assign_rank(0);
    const auto expected = scalar_exact_oracle(data, W, K);
    const std::vector<std::pair<std::size_t, std::size_t>> fragments{
        {0, 11}, {11, 12}, {12, 31}, {31, data.size()}
    };
    const auto result = run_continuation_fragments<W, K>(
        data, fragments, W, K);
    assert_exact_records(result.records, expected,
        "equal entering multiplicity");
    bool has_intentional_duplicate_identity = false;
    for (std::size_t i = 1; i < result.records.size(); ++i) {
        has_intentional_duplicate_identity =
            has_intentional_duplicate_identity
            || result.records[i] == result.records[i - 1];
    }
    ASSERT(has_intentional_duplicate_identity,
        "first-window tie emission followed by entering replacement must retain "
        "the scalar oracle's duplicate identity");
    ASSERT(result.proof.entering_min_transitions > 0,
        "equal candidates must take the <= entering-min transition",
        result.proof.entering_min_transitions);
}

TEST_CASE("two_stack continuation — expiry rescans use rightmost minima",
          "[two_stack][continuation][exact][expiry]") {
    constexpr std::size_t W = 5;
    constexpr std::size_t K = 7;
    auto data = continuation_fixture(1024);
    const auto expected = scalar_exact_oracle(data, W, K);
    const std::vector<std::pair<std::size_t, std::size_t>> fragments{
        {0, 73}, {73, 190}, {190, 511}, {511, data.size()}
    };
    const auto result = run_continuation_fragments<0, 0>(
        data, fragments, W, K);
    assert_exact_records(result.records, expected,
        "expiry rightmost rescan");
    assert_exact_simd_proof(result.proof, "expiry rightmost rescan");
    ASSERT(result.proof.expiry_rescan_transitions > 0,
        "random fixture must exercise the exact expiry/rescan transition",
        result.proof.expiry_rescan_transitions);
}

TEST_CASE("two_stack continuation — W and K supported boundaries",
          "[two_stack][continuation][exact][boundaries]") {
    {
        constexpr std::size_t W = 1;
        constexpr std::size_t K = 1;
        auto data = continuation_fixture(97);
        const auto expected = scalar_exact_oracle(data, W, K);
        const auto result = run_continuation_fragments<0, 0>(
            data, {{0, 13}, {13, 64}, {64, data.size()}}, W, K);
        assert_exact_records(result.records, expected, "W=1 K=1");
        assert_exact_simd_proof(result.proof, "W=1 K=1");
    }
    {
        constexpr std::size_t W = two_stack_max_window;
        constexpr std::size_t K = two_stack_exact_max_kmer;
        auto data = continuation_fixture(K + W + 41);
        const auto expected = scalar_exact_oracle(data, W, K);
        const auto result = run_continuation_fragments<0, 0>(
            data, {{0, 37}, {37, 181}, {181, data.size()}}, W, K);
        assert_exact_records(result.records, expected, "W=256 K=31");
        assert_exact_simd_proof(result.proof, "W=256 K=31");
    }
}

TEST_CASE("two_stack continuation — unaligned packed source range",
          "[two_stack][continuation][exact][unaligned]") {
    constexpr std::size_t W = 5;
    constexpr std::size_t K = 7;
    constexpr std::size_t begin = 5;
    constexpr std::size_t end = 173;
    constexpr std::size_t output_base = 7001;
    auto data = continuation_fixture(211);
    std::vector<seq::dna4> slice(
        data.begin() + static_cast<std::ptrdiff_t>(begin),
        data.begin() + static_cast<std::ptrdiff_t>(end));
    const auto expected = scalar_exact_oracle(
        slice, W, K, 0, slice.size(), output_base);

    continuation_range source(data);
    TwoStackContinuationState<W, K, continuation_range::traits> state;
    sketch_collector collector;
    ASSERT(reset_two_stack_nthash(state, W, K, output_base)
            == TwoStackContinuationStatus::ok,
        "unaligned reset must succeed");
    const auto unaligned_consume_status =
        consume_two_stack_nthash_fragment<W, K, MinimapSketchData>(
            state, collector, source, 0, begin, end);
    ASSERT(unaligned_consume_status == TwoStackContinuationStatus::ok,
        "unaligned packed range must decode without scalar tails");
    const auto finalized = finalize_two_stack_nthash(state, collector);
    ASSERT(finalized.ok(), "unaligned finalize must succeed");
    assert_exact_records(exact_records(collector), expected,
        "unaligned packed source range");
    assert_exact_simd_proof(finalized.proof,
        "unaligned packed source range");
    ASSERT(finalized.proof.packed_word_boundary_merges > 0,
        "unaligned decode must exercise packed-word SIMD boundary merging",
        finalized.proof.packed_word_boundary_merges);
}

TEST_CASE("two_stack continuation — real hash sentinel is never emitted",
          "[two_stack][continuation][exact][sentinel]") {
    constexpr std::size_t W = 1;
    constexpr std::size_t K = 1;
    std::vector<seq::dna4> data(33);
    for (auto& symbol : data) symbol.assign_rank(0);
    const auto expected = scalar_exact_oracle<sentinel_hash_minimizer>(
        data, W, K);
    const auto result =
        run_continuation_fragments<0, 0, sentinel_hash_minimizer>(
            data, {{0, 3}, {3, 17}, {17, data.size()}}, W, K);
    assert_exact_records(result.records, expected, "real sentinel hash");
    ASSERT(result.records.empty(),
        "all-ones truncated real hashes must behave as Max sentinels");
    ASSERT(result.proof.final_flush_records == 0,
        "sentinel current must not escape through final flush",
        result.proof.final_flush_records);
}

TEST_CASE("two_stack continuation — scalar targets reject before proof work",
          "[two_stack][continuation][exact][target_shape]") {
    using traits = continuation_range::traits;
    using state_t = TwoStackContinuationState<5, 7, traits>;

    static_assert(!two_stack_internal::exact_vector_lane_count_supported(
        1, state_t::max_vector_lanes));

#if HWY_TARGET == HWY_SCALAR
    auto data = continuation_fixture(8);
    continuation_range source(data);
    sketch_collector collector;
    state_t state;
    const auto reset_status = reset_two_stack_nthash(state, 5, 7, 0);
    ASSERT(reset_status
            == TwoStackContinuationStatus::unsupported_vector_shape,
        "actual HWY_SCALAR reset must fail with unsupported vector shape");
    ASSERT(state.status
            == TwoStackContinuationStatus::unsupported_vector_shape
        && !state.initialized,
        "actual HWY_SCALAR reset must reject before initialization");
    ASSERT(state.proof == TwoStackExecutionProof{},
        "actual HWY_SCALAR reset must leave every proof counter zero");

    const auto consume_status =
        consume_two_stack_nthash_fragment<5, 7, MinimapSketchData>(
            state, collector, source, 0, 0, data.size());
    ASSERT(consume_status
            == TwoStackContinuationStatus::unsupported_vector_shape,
        "actual HWY_SCALAR consume must preserve unsupported status");
    ASSERT(state.proof == TwoStackExecutionProof{},
        "actual HWY_SCALAR consume must leave every proof counter zero");
#else
    static_assert(two_stack_internal::exact_vector_lane_count_supported(
        2, state_t::max_vector_lanes));
    static_assert(two_stack_internal::exact_vector_lane_count_supported(
        4, 16));
    static_assert(two_stack_internal::exact_vector_lane_count_supported(
        8, 16));
    static_assert(two_stack_internal::exact_vector_lane_count_supported(
        16, 16));

    state_t one_lane_state;
    const auto one_lane_status =
        two_stack_internal::validate_exact_vector_shape(
            one_lane_state,
            /*lanes=*/1,
            state_t::max_vector_lanes,
            /*compiled_target_is_scalar=*/true,
            /*transpose_shape_supported=*/false);
    ASSERT(one_lane_status
            == TwoStackContinuationStatus::unsupported_vector_shape,
        "one-lane target must fail with the exact unsupported status");
    ASSERT(one_lane_state.status
            == TwoStackContinuationStatus::unsupported_vector_shape
        && !one_lane_state.initialized,
        "one-lane target rejection must happen before initialization");
    ASSERT(one_lane_state.proof == TwoStackExecutionProof{},
        "one-lane target rejection must leave every proof counter zero");

    state_t scalar_state;
    const auto scalar_status =
        two_stack_internal::validate_exact_vector_shape(
            scalar_state,
            /*lanes=*/2,
            state_t::max_vector_lanes,
            /*compiled_target_is_scalar=*/true,
            /*transpose_shape_supported=*/true);
    ASSERT(scalar_status
            == TwoStackContinuationStatus::unsupported_vector_shape,
        "HWY_SCALAR must fail even if a synthetic lane count looks wide");
    ASSERT(scalar_state.proof == TwoStackExecutionProof{},
        "HWY_SCALAR rejection must not be counted as SIMD proof work");
#endif
}

#if HWY_TARGET != HWY_SCALAR
static_assert(
    two_stack_internal::make_u64_lane_index_table<4>()
    == std::array<std::uint64_t, 4>{0, 1, 2, 3});

TEST_CASE("ExactSimd portable u64 lane ordinals and variable shifts",
          "[two_stack][continuation][exact][target_shape][lane_indices]") {
    using d_t = hn::ScalableTag<std::uint64_t>;
    constexpr std::size_t capacity = hn::MaxLanes(d_t{});
    const d_t d;
    const std::size_t lanes = hn::Lanes(d);
    const auto indices = two_stack_internal::u64_lane_indices(d);
    const auto shifted = hn::Shr(
        hn::Set(d, std::uint64_t{0xFEDCBA9876543210ull}),
        indices);
    HWY_ALIGN std::array<std::uint64_t, capacity> stored_indices{};
    HWY_ALIGN std::array<std::uint64_t, capacity> stored_shifted{};
    hn::StoreU(indices, d, stored_indices.data());
    hn::StoreU(shifted, d, stored_shifted.data());
    for (std::size_t lane = 0; lane < lanes; ++lane) {
        REQUIRE(stored_indices[lane] == lane);
        REQUIRE(stored_shifted[lane]
            == (std::uint64_t{0xFEDCBA9876543210ull} >> lane));
    }
}

TEST_CASE("two_stack continuation — u64 lane transpose is row exact",
          "[two_stack][continuation][exact][target_shape][transpose]") {
    using d_t = hn::ScalableTag<std::uint64_t>;
    using vec_t = hn::Vec<d_t>;
    constexpr std::size_t capacity = hn::MaxLanes(d_t{});
    const d_t d;
    const std::size_t lanes = hn::Lanes(d);
    ASSERT(two_stack_internal::u64_lane_transpose_shape_supported(d),
        "the active non-scalar Highway target must have an exact u64 shape",
        lanes, capacity);

    HWY_ALIGN std::array<vec_t, capacity> columns;
    HWY_ALIGN std::array<std::uint64_t, capacity> column_lanes{};
    for (std::size_t column = 0; column < lanes; ++column) {
        for (std::size_t row = 0; row < lanes; ++row) {
            column_lanes[row] =
                static_cast<std::uint64_t>(1000 * column + row);
        }
        columns[column] = hn::LoadU(d, column_lanes.data());
    }

    ASSERT(two_stack_internal::transpose_u64_lane_matrix(
            d, columns.data()),
        "the pre-approved active target transpose must succeed");
    HWY_ALIGN std::array<std::uint64_t, capacity> row_lanes{};
    for (std::size_t row = 0; row < lanes; ++row) {
        hn::StoreU(columns[row], d, row_lanes.data());
        for (std::size_t column = 0; column < lanes; ++column) {
            ASSERT(row_lanes[column]
                    == static_cast<std::uint64_t>(1000 * column + row),
                "u64 transpose must preserve row-major event order",
                lanes, row, column, row_lanes[column]);
        }
    }
}
#endif

TEST_CASE("two_stack continuation — invalid inputs fail closed in release",
          "[two_stack][continuation][exact][invalid]") {
    using traits = continuation_range::traits;
    auto data = continuation_fixture(32);
    continuation_range source(data);
    sketch_collector collector;

    {
        TwoStackContinuationState<0, 0, traits> state;
        ASSERT(reset_two_stack_nthash(state, 0, 7, 0)
                == TwoStackContinuationStatus::invalid_window_size,
            "W=0 must fail closed");
    }
    {
        TwoStackContinuationState<0, 0, traits> state;
        ASSERT(reset_two_stack_nthash(state, two_stack_max_window + 1, 7, 0)
                == TwoStackContinuationStatus::invalid_window_size,
            "W above the fixed bound must fail closed");
    }
    {
        TwoStackContinuationState<0, 0, traits> state;
        ASSERT(reset_two_stack_nthash(state, 5, 0, 0)
                == TwoStackContinuationStatus::invalid_kmer_size,
            "K=0 must fail closed");
    }
    {
        TwoStackContinuationState<0, 0, traits> state;
        ASSERT(reset_two_stack_nthash(state, 5, 32, 0)
                == TwoStackContinuationStatus::invalid_kmer_size,
            "K=32 must be rejected before any shift-by-64 expression");
    }
    {
        TwoStackContinuationState<5, 7, traits> state;
        ASSERT(reset_two_stack_nthash(state, 6, 7, 0)
                == TwoStackContinuationStatus::invalid_static_configuration,
            "constexpr/runtime W mismatch must fail closed");
    }
    {
        TwoStackContinuationState<5, 7, traits> state;
        const auto consume_status =
            consume_two_stack_nthash_fragment<5, 7, MinimapSketchData>(
                state, collector, source, 0, 0, 1);
        ASSERT(consume_status == TwoStackContinuationStatus::not_initialized,
            "consume before reset must fail closed");
    }
    {
        TwoStackContinuationState<5, 7, traits> state;
        ASSERT(reset_two_stack_nthash(state, 5, 7, 0)
                == TwoStackContinuationStatus::ok,
            "range-check reset must succeed");
        const auto consume_status =
            consume_two_stack_nthash_fragment<5, 7, MinimapSketchData>(
                state, collector, source, 0, 9, 8);
        ASSERT(consume_status == TwoStackContinuationStatus::invalid_range,
            "reversed consume range must fail closed");
    }
    {
        TwoStackContinuationState<5, 7, traits> state;
        ASSERT(reset_two_stack_nthash(state, 5, 7, 0)
                == TwoStackContinuationStatus::ok,
            "bounds-check reset must succeed");
        const auto consume_status =
            consume_two_stack_nthash_fragment<5, 7, MinimapSketchData>(
                state, collector, source, 0, 0, data.size() + 1);
        ASSERT(consume_status == TwoStackContinuationStatus::invalid_range,
            "range beyond the fragment must fail closed");
    }
    {
        TwoStackContinuationState<5, 7, traits> state;
        ASSERT(reset_two_stack_nthash(state, 5, 7, 0)
                == TwoStackContinuationStatus::ok,
            "continuity-check reset must succeed");
        const auto first_consume_status =
            consume_two_stack_nthash_fragment<5, 7, MinimapSketchData>(
                state, collector, source, 0, 0, 3);
        ASSERT(first_consume_status == TwoStackContinuationStatus::ok,
            "first range must succeed");
        const auto second_consume_status =
            consume_two_stack_nthash_fragment<5, 7, MinimapSketchData>(
                state, collector, source, 0, 4, 5);
        ASSERT(second_consume_status
                == TwoStackContinuationStatus::non_contiguous_range,
            "gapped range must fail closed");
    }
    {
        TwoStackContinuationState<5, 7, traits> state;
        ASSERT(reset_two_stack_nthash(state, 5, 7, 0)
                == TwoStackContinuationStatus::ok,
            "state-corruption reset must succeed");
        state.pending_count = state.max_vector_lanes;
        const auto consume_status =
            consume_two_stack_nthash_fragment<5, 7, MinimapSketchData>(
                state, collector, source, 0, 0, 0);
        ASSERT(consume_status == TwoStackContinuationStatus::invalid_state,
            "corrupt pending-lane state must fail closed even for empty input");
    }
    {
        TwoStackContinuationState<1, 2, traits> state;
        ASSERT(reset_two_stack_nthash(
                state, 1, 2, std::numeric_limits<std::size_t>::max())
                == TwoStackContinuationStatus::ok,
            "overflow is checked when a candidate position materializes");
        const auto consume_status =
            consume_two_stack_nthash_fragment<1, 2, MinimapSketchData>(
                state, collector, source, 0, 0, 2);
        ASSERT(consume_status == TwoStackContinuationStatus::ok,
            "overflow fixture consume must stage safely");
        const auto finalized = finalize_two_stack_nthash(state, collector);
        ASSERT(finalized.status
                == TwoStackContinuationStatus::arithmetic_overflow,
            "output-position overflow must fail before vector arithmetic");
    }
}

TEST_CASE("two_stack continuation — lifecycle and collector overflow",
          "[two_stack][continuation][exact][lifecycle][overflow]") {
    using traits = continuation_range::traits;
    auto data = continuation_fixture(1);
    continuation_range source(data);

    {
        TwoStackContinuationState<1, 1, traits> state;
        sketch_collector collector;
        ASSERT(reset_two_stack_nthash(state, 1, 1, 0)
                == TwoStackContinuationStatus::ok,
            "lifecycle reset must succeed");
        const auto consume_status =
            consume_two_stack_nthash_fragment<1, 1, MinimapSketchData>(
                state, collector, source, 0, 0, 1);
        ASSERT(consume_status == TwoStackContinuationStatus::ok,
            "lifecycle consume must succeed");
        ASSERT(finalize_two_stack_nthash(state, collector).ok(),
            "first finalize must succeed");
        ASSERT(finalize_two_stack_nthash(state, collector).status
                == TwoStackContinuationStatus::already_finalized,
            "second finalize must fail closed");
    }
    {
        TwoStackContinuationState<1, 1, traits> state;
        sketch_collector collector;
        ASSERT(reset_two_stack_nthash(state, 1, 1, 0)
                == TwoStackContinuationStatus::ok,
            "post-finalize reset must succeed");
        const auto consume_status =
            consume_two_stack_nthash_fragment<1, 1, MinimapSketchData>(
                state, collector, source, 0, 0, 1);
        ASSERT(consume_status == TwoStackContinuationStatus::ok,
            "post-finalize consume must succeed");
        ASSERT(finalize_two_stack_nthash(state, collector).ok(),
            "post-finalize fixture must finalize");
        const auto post_finalize_status =
            consume_two_stack_nthash_fragment<1, 1, MinimapSketchData>(
                state, collector, source, 0, 1, 1);
        ASSERT(post_finalize_status
                == TwoStackContinuationStatus::already_finalized,
            "consume after finalize must fail closed");
    }
    {
        TwoStackContinuationState<1, 1, traits> state;
        rejecting_raw_collector<MinimapSketchData> collector;
        ASSERT(reset_two_stack_nthash(state, 1, 1, 0)
                == TwoStackContinuationStatus::ok,
            "collector-overflow reset must succeed");
        const auto consume_status =
            consume_two_stack_nthash_fragment<1, 1, MinimapSketchData>(
                state, collector, source, 0, 0, 1);
        ASSERT(consume_status == TwoStackContinuationStatus::ok,
            "collector-overflow consume must stage one candidate");
        const auto finalized = finalize_two_stack_nthash(state, collector);
        ASSERT(finalized.status
                == TwoStackContinuationStatus::collector_overflow,
            "collector capacity rejection must propagate fail closed");
        ASSERT(finalized.proof.scalar_event_records == 0,
            "collector failure must not introduce scalar event fallback",
            finalized.proof.scalar_event_records);
    }
}

TEST_CASE("ExactSimd checked driver — DNA4 u8/u16/u32/u64 packed parity",
          "[two_stack][continuation][exact][packed_widths]") {
    auto data = continuation_fixture(391);
    assert_exact_driver_storage_width<std::uint8_t>(
        data, 1, 96, "DNA4/u8 bounded packed range");
    assert_exact_driver_storage_width<std::uint16_t>(
        data, 1, 48, "DNA4/u16 bounded packed range");
    assert_exact_driver_storage_width<std::uint32_t>(
        data, 1, 24, "DNA4/u32 bounded packed range");
    assert_exact_driver_storage_width<std::uint64_t>(
        data, 1, 12, "DNA4/u64 bounded packed range");
}

TEST_CASE("ExactSimd checked driver — fragmented scanner range parity and tails",
          "[two_stack][continuation][exact][fragmented][range]") {
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using range_type = compact_sequence_range<sequence_type>;
    using scanner_type =
        bioseq::storage::sequence_scanner_test_chunked<std::uint64_t, 2>;
    using lazy_type =
        bioseq::storage::lazy_compact_sequence<sequence_type, scanner_type>;

    auto data = continuation_fixture(391);
    range_type resident(data);
    lazy_type fragmented{scanner_type::from_range(resident)};
    constexpr std::size_t runtime_k = 7;
    constexpr std::size_t lo_unit = 1;
    const std::size_t hi_unit = static_cast<std::size_t>(resident.size());
    const std::size_t expected_bp =
        static_cast<std::size_t>(resident.bp_size())
        - lo_unit * range_type::traits::letters_per_unit;
    const std::size_t expected_candidates = expected_bp - (runtime_k - 1);
    const std::size_t lanes = hn::Lanes(hn::ScalableTag<std::uint64_t>{});
    const std::size_t expected_masked_tail =
        (lanes - expected_candidates % lanes) % lanes;
    ASSERT(expected_masked_tail > 0,
        "fragmented tail fixture must not end on a full active SIMD block",
        expected_bp, expected_candidates, lanes);

    sketch_collector exact_collector;
    const auto exact =
        collect_minimizers_two_stack_nthash_exact_range_checked<
            0, 0, MinimapSketchData>(
                exact_collector,
                fragmented,
                lo_unit,
                hi_unit,
                5,
                runtime_k,
                0);
    ASSERT(exact.ok(),
        "fragmented checked driver must accept a bounded multi-fragment range",
        two_stack_status_name(exact.status));

    sketch_collector scalar_collector;
    auto begin = resident.cbegin();
    begin += static_cast<decltype(resident.size())>(lo_unit);
    auto end = resident.cbegin();
    end += static_cast<decltype(resident.size())>(hi_unit);
    collect_minimizers_view<minimap_int_hasher, MinimapSketchData>(
        scalar_collector, resident, begin, end, 5, runtime_k);
    assert_exact_records(
        exact_records(exact_collector),
        exact_records(scalar_collector),
        "fragmented scanner bounded range");
    assert_exact_simd_proof(exact.proof,
        "fragmented scanner bounded range");
    ASSERT(exact.proof.bp_consumed == expected_bp,
        "fragmented range must consume every owned packed symbol exactly once",
        exact.proof.bp_consumed, expected_bp);
    ASSERT(exact.proof.vector_lanes_visited == expected_candidates,
        "fragmented range must visit each chronological kmer candidate once",
        exact.proof.vector_lanes_visited, expected_candidates);
    ASSERT(exact.proof.masked_tail_lanes == expected_masked_tail,
        "bounded fragmented fixture must report the exact inactive tail lanes",
        exact.proof.masked_tail_lanes, expected_masked_tail, lanes);
}

TEST_CASE("ExactSimd checked driver — two-word representation fails before core instantiation",
          "[two_stack][continuation][exact][representation][fail_closed]") {
    using unsupported_repr = MinimizerRepr<63, 15>;
    struct unreachable_collector {};
    static_assert(exact_simd_minimizer_representation<MinimapSketchData>);
    static_assert(MinimizerLayout<63, 15>::storage_bits == 128);
    static_assert(!exact_simd_minimizer_representation<unsupported_repr>);

    auto data = continuation_fixture(96);
    continuation_range source(data);
    unreachable_collector collector;
    const auto rejected =
        collect_minimizers_two_stack_nthash_exact_range_checked<
            0, 0, unsupported_repr>(
                collector,
                source,
                0,
                static_cast<std::size_t>(source.size()),
                10,
                15,
                0);
    ASSERT(rejected.status
            == TwoStackContinuationStatus::unsupported_representation,
        "two-word minimizer representation must fail closed");
    ASSERT(rejected.proof.fail_closed_rejections == 1
        && rejected.proof.vector_batches == 0
        && rejected.proof.scalar_backend_branches == 0
        && rejected.proof.scalar_hash_symbols == 0
        && rejected.proof.scalar_window_transitions == 0,
        "representation rejection must instantiate neither SIMD core nor scalar fallback");

    bool threw = false;
    try {
        (void)collect_minimizers_two_stack_nthash_exact_range_or_throw<
            0, 0, unsupported_repr>(
                collector,
                source,
                0,
                static_cast<std::size_t>(source.size()),
                10,
                15,
                0,
                "unsupported representation test");
    } catch (const ExactSimdRuntimeError& err) {
        threw = err.status()
            == TwoStackContinuationStatus::unsupported_representation;
    }
    ASSERT(threw,
        "throwing boundary must preserve unsupported representation status");
}

TEST_CASE("ExactSimd checked driver — unsupported shapes fail without scalar fallback",
          "[two_stack][continuation][exact][fail_closed]") {
    using amino_sequence =
        sequence_custom_alphabet<seq::aa27, std::uint64_t>;
    using amino_range = compact_sequence_range<amino_sequence>;
    std::vector<seq::aa27> amino_data(96);
    for (std::size_t i = 0; i < amino_data.size(); ++i) {
        amino_data[i].assign_rank(static_cast<std::uint8_t>(i % 27));
    }
    amino_range amino_source(amino_data);
    sketch_collector amino_collector;
    const auto unsupported_alphabet =
        collect_minimizers_two_stack_nthash_exact_range_checked<
            0, 0, MinimapSketchData>(
                amino_collector,
                amino_source,
                0,
                static_cast<std::size_t>(amino_source.size()),
                5,
                7,
                0);
    ASSERT(unsupported_alphabet.status
            == TwoStackContinuationStatus::unsupported_alphabet,
        "non-nucleotide alphabets must fail closed instead of selecting scalar sketching");
    ASSERT(amino_collector.extract_sketches_data().empty()
        && unsupported_alphabet.proof.scalar_backend_branches == 0
        && unsupported_alphabet.proof.scalar_hash_symbols == 0
        && unsupported_alphabet.proof.scalar_window_transitions == 0
        && unsupported_alphabet.proof.fail_closed_rejections == 1,
        "unsupported alphabet rejection must emit nothing and do no scalar work");

    auto dna4_data = continuation_fixture(96);
    continuation_range dna4_source(dna4_data);
    sketch_collector invalid_shape_collector;
    const auto invalid_w =
        collect_minimizers_two_stack_nthash_exact_range_checked<
            0, 0, MinimapSketchData>(
                invalid_shape_collector,
                dna4_source,
                0,
                static_cast<std::size_t>(dna4_source.size()),
                0,
                7,
                0);
    ASSERT(invalid_w.status
            == TwoStackContinuationStatus::invalid_window_size,
        "W=0 must fail closed");

    bool threw = false;
    try {
        (void)collect_minimizers_two_stack_nthash_exact_range_or_throw<
            0, 0, MinimapSketchData>(
                invalid_shape_collector,
                dna4_source,
                0,
                static_cast<std::size_t>(dna4_source.size()),
                5,
                two_stack_exact_max_kmer + 1,
                0,
                "ExactSimd fail-closed test");
    } catch (const ExactSimdRuntimeError& err) {
        threw = err.status()
            == TwoStackContinuationStatus::invalid_kmer_size;
    }
    ASSERT(threw,
        "throwing ExactSimd boundary must surface unsupported K explicitly");
}

}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();
