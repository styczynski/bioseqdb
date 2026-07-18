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
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <ranges>
#include <span>
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
import bioseq.parallelism.memory_admission;
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
            && (proof.direct_event_compress_batches > 0
                || (proof.row_transpose_batches > 0
                    && proof.row_vector_stores > 0)),
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

using xxl_fast_minimizer = MinimizerRepr<33, 15>;
using xxl_fast_sequence =
    sequence_custom_alphabet<seq::dna4, std::uint32_t>;
using xxl_fast_range = compact_sequence_range<xxl_fast_sequence>;

[[nodiscard]] static continuation_result run_xxl_fast_driver(
    std::vector<seq::dna4>& data,
    std::size_t emitted_position_base = 0)
{
    xxl_fast_range source(data);
    sketch_collector_t<xxl_fast_minimizer> collector;
    const auto exact =
        collect_minimizers_two_stack_nthash_exact_range_checked<
            10, 15, xxl_fast_minimizer>(
                collector,
                source,
                0,
                static_cast<std::size_t>(source.size()),
                10,
                15,
                emitted_position_base);
    ASSERT(exact.ok(),
        "valid K15/W10 XXL fast driver must succeed",
        two_stack_status_name(exact.status));
    return {
        .records = exact_records(collector),
        .proof = exact.proof,
    };
}

[[nodiscard]] static continuation_result run_xxl_resident_vector_stream(
    std::vector<seq::dna4>& data)
{
    xxl_fast_range source(data);
    sketch_collector_t<xxl_fast_minimizer> collector;
    const TwoStackExecutionProof proof =
        collect_minimizers_two_stack_nthash_resident_constexpr_or_throw<
            10, 15, xxl_fast_minimizer>(
                collector, source, 10u, 15u);
    return {
        .records = exact_records(collector),
        .proof = proof,
    };
}

[[nodiscard]] static std::vector<exact_minimizer>
run_xxl_legacy_vector_stream_records(std::vector<seq::dna4>& data)
{
    xxl_fast_range source(data);
    sketch_collector_t<xxl_fast_minimizer> collector;
    collect_minimizers_two_stack_nthash_simd_constexpr<
        10, 15, minimap_int_hasher, xxl_fast_minimizer>(
            collector, source, source.begin(), source.end());
    return exact_records(collector);
}

template<class MinimizerT>
struct dropping_resident_collector {
    std::size_t attempted_records = 0;

    void collect(MinimizerT) noexcept { ++attempted_records; }
    [[nodiscard]] std::size_t size() const noexcept { return 0u; }
};

[[nodiscard]] static continuation_result run_xxl_fast_fragments(
    const std::vector<seq::dna4>& data,
    const std::vector<std::pair<std::size_t, std::size_t>>& fragments)
{
    TwoStackContinuationState<10, 15, xxl_fast_range::traits> state;
    sketch_collector_t<xxl_fast_minimizer> collector;
    ASSERT(reset_two_stack_nthash(state, 10, 15, 0)
        == TwoStackContinuationStatus::ok,
        "valid K15/W10 fast fragmented reset must succeed");
    state.kernel_kind = ExactSimdKernelKind::k15_w10_b8_u32_hash;
    state.proof.generic_u64_kernel_calls = 0;
    state.proof.k15_w10_b8_u32_hash_kernel_calls = 1;
    for (const auto& [begin, end] : fragments) {
        ASSERT(begin <= end && end <= data.size(),
            "invalid K15/W10 fast fragment", begin, end, data.size());
        std::vector<seq::dna4> fragment_data(
            data.begin() + static_cast<std::ptrdiff_t>(begin),
            data.begin() + static_cast<std::ptrdiff_t>(end));
        xxl_fast_range fragment(fragment_data);
        const auto status = consume_two_stack_nthash_fragment<
            10, 15, xxl_fast_minimizer>(
                state,
                collector,
                fragment,
                begin,
                begin,
                end);
        ASSERT(status == TwoStackContinuationStatus::ok,
            "valid K15/W10 fast fragment must succeed",
            begin, end, static_cast<std::uint8_t>(status));
    }
    const auto finalized = finalize_two_stack_nthash(state, collector);
    ASSERT(finalized.ok(),
        "valid K15/W10 fast fragmented finalize must succeed",
        static_cast<std::uint8_t>(finalized.status));
    return {
        .records = exact_records(collector),
        .proof = finalized.proof,
    };
}

static void assert_xxl_fast_route(
    const TwoStackExecutionProof& proof,
    const std::string& fixture)
{
    assert_exact_simd_proof(proof, fixture);
    ASSERT(proof.generic_u64_kernel_calls == 0
        && proof.generic_u64_candidate_blocks == 0
        && proof.generic_u64_lane_visits == 0
        && proof.k15_w10_b8_u32_hash_kernel_calls == 1
        && proof.k15_w10_b8_u32_hash_candidate_blocks
            == proof.vector_batches
        && proof.k15_w10_b8_u32_hash_lane_visits
            == proof.vector_lanes_visited
        && proof.k15_w10_b8_packed22_groups
            == proof.k15_w10_b8_u32_hash_candidate_blocks
        && proof.k15_w10_b8_packed22_lane_visits
            == proof.k15_w10_b8_u32_hash_lane_visits,
        fmt::format(
            "{}: proof must attest only the packed22 B8/u32 route",
            fixture));
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
    two_stack_internal::make_lane_index_table<std::uint64_t, 4>()
    == std::array<std::uint64_t, 4>{0, 1, 2, 3});
static_assert(
    two_stack_internal::make_lane_index_table<std::uint32_t, 4>()
    == std::array<std::uint32_t, 4>{0, 1, 2, 3});

TEST_CASE("ExactSimd portable unsigned lane ordinals and variable shifts",
          "[two_stack][continuation][exact][target_shape][lane_indices]") {
    using d_t = hn::ScalableTag<std::uint64_t>;
    constexpr std::size_t capacity = hn::MaxLanes(d_t{});
    const d_t d;
    const std::size_t lanes = hn::Lanes(d);
    const auto indices = two_stack_internal::lane_indices(d);
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

TEST_CASE("ExactSimd u32/u64 mask split and join preserve lane order",
          "[two_stack][continuation][exact][target_shape][mask_join]") {
    const hn::ScalableTag<std::uint32_t> d32;
    const hn::ScalableTag<std::uint64_t> d64;
    if (hn::Lanes(d32) != 4 || hn::Lanes(d64) != 2) return;

    for (unsigned bits = 0; bits < 16; ++bits) {
        const auto original = hn::Dup128MaskFromMaskBits(d32, bits);
        const auto halves = two_stack_internal::split_u32_mask(
            d32, d64, original);
        const auto joined = two_stack_internal::join_u64_masks(
            d32, d64, halves.lower, halves.upper);
        REQUIRE((hn::BitsFromMask(d32, joined) & 0xFu) == bits);
    }
}

TEST_CASE("ExactSimd K15/W10 metadata reduction preserves rightmost minima",
          "[two_stack][continuation][exact][k15_w10_b8][minimum_metadata]") {
    const hn::ScalableTag<std::uint32_t> d32;
    const hn::ScalableTag<std::uint64_t> d64;
    if (hn::Lanes(d32) != 4 || hn::Lanes(d64) != 2) return;

    constexpr std::uint32_t maximum_hash = 0x3fffffffu;
    constexpr std::uint64_t maximum_raw =
        (std::numeric_limits<std::uint64_t>::max)();
    constexpr std::uint64_t ordinal_sentinel =
        (std::numeric_limits<std::uint64_t>::max)();
    constexpr std::size_t combined_size = 13;
    HWY_ALIGN std::array<std::uint32_t, combined_size> hashes{};
    HWY_ALIGN std::array<std::uint64_t, combined_size> raw{};
    HWY_ALIGN std::array<std::uint64_t, combined_size> ordinals{};
    for (std::size_t index = 0; index < combined_size; ++index) {
        raw[index] = 0x8000000000000000ull
            | (static_cast<std::uint64_t>(index) << 34u)
            | static_cast<std::uint64_t>(index);
        ordinals[index] = 0x9000000000000000ull
            + (static_cast<std::uint64_t>(index) << 33u)
            + index;
    }

    SECTION("every chronological slot can win lane zero") {
        for (std::size_t winner_slot = 0; winner_slot < 10;
             ++winner_slot) {
            hashes.fill(maximum_hash);
            hashes[winner_slot] = 1;
            const auto minimum =
                two_stack_internal::reduce_exact_k15_w10_minimum_metadata(
                    d32,
                    d64,
                    hashes.data(),
                    raw.data(),
                    ordinals.data(),
                    maximum_hash,
                    maximum_raw,
                    ordinal_sentinel);
            HWY_ALIGN std::array<std::uint64_t, 4> minimum_raw{};
            HWY_ALIGN std::array<std::uint64_t, 4> minimum_ordinal{};
            hn::StoreU(minimum.raw_lower, d64, minimum_raw.data());
            hn::StoreU(minimum.raw_upper, d64, minimum_raw.data() + 2);
            hn::StoreU(
                minimum.ordinal_lower, d64, minimum_ordinal.data());
            hn::StoreU(
                minimum.ordinal_upper, d64, minimum_ordinal.data() + 2);
            REQUIRE(minimum_raw[0] == raw[winner_slot]);
            REQUIRE(minimum_ordinal[0] == ordinals[winner_slot]);
        }
    }

    SECTION("absolute lane offsets and full-width ordinals are preserved") {
        hashes.fill(maximum_hash);
        hashes[8] = 4;
        hashes[10] = 3;
        hashes[11] = 2;
        hashes[12] = 1;
        const auto minimum =
            two_stack_internal::reduce_exact_k15_w10_minimum_metadata(
                d32,
                d64,
                hashes.data(),
                raw.data(),
                ordinals.data(),
                maximum_hash,
                maximum_raw,
                ordinal_sentinel);
        constexpr std::array<std::size_t, 4> winners{8, 10, 11, 12};
        HWY_ALIGN std::array<std::uint64_t, 4> minimum_raw{};
        HWY_ALIGN std::array<std::uint64_t, 4> minimum_ordinal{};
        hn::StoreU(minimum.raw_lower, d64, minimum_raw.data());
        hn::StoreU(minimum.raw_upper, d64, minimum_raw.data() + 2);
        hn::StoreU(minimum.ordinal_lower, d64, minimum_ordinal.data());
        hn::StoreU(
            minimum.ordinal_upper, d64, minimum_ordinal.data() + 2);
        for (std::size_t lane = 0; lane < winners.size(); ++lane) {
            REQUIRE(minimum_raw[lane] == raw[winners[lane]]);
            REQUIRE(minimum_ordinal[lane] == ordinals[winners[lane]]);
        }
    }

    SECTION("equal minima choose the later chronological occurrence") {
        hashes.fill(maximum_hash);
        hashes[2] = 7;
        hashes[7] = 7;
        const auto minimum =
            two_stack_internal::reduce_exact_k15_w10_minimum_metadata(
                d32,
                d64,
                hashes.data(),
                raw.data(),
                ordinals.data(),
                maximum_hash,
                maximum_raw,
                ordinal_sentinel);
        HWY_ALIGN std::array<std::uint64_t, 2> minimum_raw{};
        HWY_ALIGN std::array<std::uint64_t, 2> minimum_ordinal{};
        hn::StoreU(minimum.raw_lower, d64, minimum_raw.data());
        hn::StoreU(minimum.ordinal_lower, d64, minimum_ordinal.data());
        REQUIRE(minimum_raw[0] == raw[7]);
        REQUIRE(minimum_ordinal[0] == ordinals[7]);
    }

    SECTION("all-sentinel windows choose their bounded right edge") {
        hashes.fill(maximum_hash);
        const auto minimum =
            two_stack_internal::reduce_exact_k15_w10_minimum_metadata(
                d32,
                d64,
                hashes.data(),
                raw.data(),
                ordinals.data(),
                maximum_hash,
                maximum_raw,
                ordinal_sentinel);
        constexpr std::array<std::size_t, 4> winners{9, 10, 11, 12};
        HWY_ALIGN std::array<std::uint64_t, 4> minimum_raw{};
        HWY_ALIGN std::array<std::uint64_t, 4> minimum_ordinal{};
        hn::StoreU(minimum.raw_lower, d64, minimum_raw.data());
        hn::StoreU(minimum.raw_upper, d64, minimum_raw.data() + 2);
        hn::StoreU(minimum.ordinal_lower, d64, minimum_ordinal.data());
        hn::StoreU(
            minimum.ordinal_upper, d64, minimum_ordinal.data() + 2);
        for (std::size_t lane = 0; lane < winners.size(); ++lane) {
            REQUIRE(minimum_raw[lane] == raw[winners[lane]]);
            REQUIRE(minimum_ordinal[lane] == ordinals[winners[lane]]);
        }
    }
}

TEST_CASE("ExactSimd K15/B8 packed22 SWAR and SIMD windows are recurrence exact",
          "[two_stack][continuation][exact][k15_w10_b8][packed22]") {
    for (std::uint32_t packed = 0; packed <= 0xffffu; ++packed) {
        std::array<std::uint8_t, 8> codes{};
        for (std::size_t index = 0; index < codes.size(); ++index) {
            codes[index] = static_cast<std::uint8_t>(
                (packed >> (2u * index)) & 3u);
        }
        REQUIRE(two_stack_internal::pack_eight_dna4_codes_swar(
                    codes.data()) == packed);
    }

    const hn::ScalableTag<std::uint32_t> d32;
    const hn::ScalableTag<std::uint64_t> d64;
    if (hn::Lanes(d32) != 4 || hn::Lanes(d64) != 2) return;
    constexpr std::uint32_t mask = 0x3fffffffu;
    for (std::uint32_t fixture = 0; fixture < 257; ++fixture) {
        std::array<std::uint8_t, 24> codes{};
        std::uint64_t expected_packed = 0;
        for (std::size_t index = 0; index < 22; ++index) {
            codes[index] = static_cast<std::uint8_t>(
                (fixture * 29u + index * 7u + (index >> 1u)) & 3u);
            expected_packed |= static_cast<std::uint64_t>(codes[index])
                << (2u * index);
        }
        const std::uint64_t packed22 =
            two_stack_internal::pack_k15_b8_dna4_codes_swar(codes.data());
        REQUIRE(packed22 == expected_packed);
        const auto natural = two_stack_internal::extract_exact_k15_b8_windows(
            d32, d64, packed22);
        const auto forward_lower = two_stack_internal::reverse_two_bit_groups(
            d32, natural.lower);
        const auto forward_upper = two_stack_internal::reverse_two_bit_groups(
            d32, natural.upper);
        HWY_ALIGN std::array<std::uint32_t, 8> natural_lanes{};
        HWY_ALIGN std::array<std::uint32_t, 8> forward_lanes{};
        hn::StoreU(natural.lower, d32, natural_lanes.data());
        hn::StoreU(natural.upper, d32, natural_lanes.data() + 4);
        hn::StoreU(forward_lower, d32, forward_lanes.data());
        hn::StoreU(forward_upper, d32, forward_lanes.data() + 4);
        for (std::size_t start = 0; start < 8; ++start) {
            std::uint32_t expected_forward = 0;
            std::uint32_t expected_reverse = 0;
            for (std::size_t offset = 0; offset < 15; ++offset) {
                const std::uint32_t code = codes[start + offset];
                expected_forward = ((expected_forward << 2u) | code) & mask;
                expected_reverse |= (code ^ 3u) << (2u * offset);
            }
            REQUIRE(natural_lanes[start]
                == static_cast<std::uint32_t>(
                    (packed22 >> (2u * start)) & mask));
            REQUIRE(forward_lanes[start] == expected_forward);
            REQUIRE((natural_lanes[start] ^ mask) == expected_reverse);
        }
    }
}

TEST_CASE("ExactSimd K15/W10 B8 selector is representation and target exact",
          "[two_stack][continuation][exact][k15_w10_b8][selector]") {
    using fast_traits = xxl_fast_range::traits;
    using u64_sequence =
        sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using u64_traits = compact_sequence_range<u64_sequence>::traits;

    const bool target_supports_fast =
        !two_stack_internal::exact_compiled_target_is_scalar
        && hn::Lanes(hn::ScalableTag<std::uint32_t>{}) == 4
        && hn::Lanes(hn::ScalableTag<std::uint64_t>{}) == 2;
    REQUIRE(two_stack_internal::select_exact_simd_kernel_kind<
            xxl_fast_minimizer, fast_traits>(4096)
        == (target_supports_fast
            ? ExactSimdKernelKind::k15_w10_b8_u32_hash
            : ExactSimdKernelKind::generic_u64));
    REQUIRE(two_stack_internal::select_exact_simd_kernel_kind<
            MinimapSketchData, fast_traits>(4096)
        == (target_supports_fast
            ? ExactSimdKernelKind::k15_w10_b8_u32_hash
            : ExactSimdKernelKind::generic_u64));
    REQUIRE(two_stack_internal::select_exact_simd_kernel_kind<
            xxl_fast_minimizer, u64_traits>(4096)
        == ExactSimdKernelKind::generic_u64);
    REQUIRE(two_stack_internal::select_exact_simd_kernel_kind<
            xxl_fast_minimizer, fast_traits>(
                static_cast<std::size_t>(
                    (std::numeric_limits<std::uint32_t>::max)()) + 1)
        == ExactSimdKernelKind::generic_u64);
}

TEST_CASE("ExactSimd packed DNA4 decode is lane-width exact at word boundaries",
          "[two_stack][continuation][exact][packed_decode][u32_lanes]") {
    using d32_t = hn::ScalableTag<std::uint32_t>;
    const d32_t d32;
    if (hn::Lanes(d32) != 4) return;
    const hn::Rebind<std::uint8_t, d32_t> d8;
    auto data = continuation_fixture(96);

    const auto check_storage = [&]<class StorageUnit>() {
        using sequence_type =
            sequence_custom_alphabet<seq::dna4, StorageUnit>;
        using range_type = compact_sequence_range<sequence_type>;
        range_type source(data);
        const StorageUnit* packed = source.read_cptr(source.cbegin());
        bool saw_crossed_word = false;
        constexpr std::array<std::size_t, 10> starts{
            13, 14, 15, 16, 17, 29, 30, 31, 32, 33};
        for (const std::size_t start : starts) {
            for (std::size_t count = 1; count <= 4; ++count) {
                const auto decoded =
                    two_stack_internal::decode_packed_dna4_rank_vector<
                        d32_t, typename range_type::traits>(
                            d32,
                            std::span<const StorageUnit>{
                                packed,
                                static_cast<std::size_t>(source.size())},
                            start,
                            count);
                HWY_ALIGN std::array<std::uint8_t, 4> ranks{};
                hn::StoreN(decoded.ranks, d8, ranks.data(), count);
                saw_crossed_word = saw_crossed_word || decoded.crossed_word;
                for (std::size_t lane = 0; lane < count; ++lane) {
                    REQUIRE(ranks[lane]
                        == static_cast<std::uint8_t>(
                            data[start + lane].to_rank()));
                }
            }
        }
        REQUIRE(saw_crossed_word);
    };

    check_storage.template operator()<std::uint32_t>();
    check_storage.template operator()<std::uint64_t>();
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

TEST_CASE("ExactSimd K15/W10 B8 matches compact scalar for every partial tail",
          "[two_stack][continuation][exact][k15_w10_b8][partial]") {
    if (two_stack_internal::select_exact_simd_kernel_kind<
            xxl_fast_minimizer, xxl_fast_range::traits>(4096)
            != ExactSimdKernelKind::k15_w10_b8_u32_hash) {
        return;
    }

    constexpr std::size_t initial_codes = 15 - 1;
    constexpr std::size_t full_block = 8;
    for (std::size_t tail = 1; tail < full_block; ++tail) {
        auto data = continuation_fixture(initial_codes + full_block + tail);
        const auto actual = run_xxl_fast_driver(data);
        const auto expected = scalar_exact_oracle<xxl_fast_minimizer>(
            data, 10, 15);
        const std::string fixture = fmt::format("B8 partial tail {}", tail);
        assert_exact_records(actual.records, expected, fixture);
        assert_xxl_fast_route(actual.proof, fixture);
        REQUIRE(actual.proof.vector_lanes_visited == full_block + tail);
        REQUIRE(actual.proof.masked_tail_lanes
            == (4 - tail % 4) % 4);
    }
}

TEST_CASE("ExactSimd K15/W10 packed22 survives adversarial fragment cuts",
          "[two_stack][continuation][exact][k15_w10_b8][packed22][fragmented]") {
    if (two_stack_internal::select_exact_simd_kernel_kind<
            xxl_fast_minimizer, xxl_fast_range::traits>(4096)
            != ExactSimdKernelKind::k15_w10_b8_u32_hash) {
        return;
    }

    auto data = continuation_fixture(257);
    const auto expected = scalar_exact_oracle<xxl_fast_minimizer>(
        data, 10, 15);
    const std::vector<std::vector<std::size_t>> cut_sets{
        {0, 1, 7, 13, 14, 15, 16, 17, 21, 22, 23, 29, 30, 31,
            32, 33, 47, 64, 65, 127, 128, 129, data.size()},
        {0, 3, 11, 18, 19, 20, 26, 27, 34, 35, 41, 42, 49, 50,
            113, 181, data.size()},
    };
    for (const auto& cuts : cut_sets) {
        std::vector<std::pair<std::size_t, std::size_t>> fragments;
        for (std::size_t index = 1; index < cuts.size(); ++index) {
            fragments.emplace_back(cuts[index - 1], cuts[index]);
        }
        const auto actual = run_xxl_fast_fragments(data, fragments);
        assert_exact_records(
            actual.records, expected, "packed22 adversarial fragments");
        assert_xxl_fast_route(
            actual.proof, "packed22 adversarial fragments");
    }

    std::vector<std::pair<std::size_t, std::size_t>> one_base_fragments;
    one_base_fragments.reserve(data.size());
    for (std::size_t bp = 0; bp < data.size(); ++bp) {
        one_base_fragments.emplace_back(bp, bp + 1);
    }
    const auto one_base = run_xxl_fast_fragments(data, one_base_fragments);
    assert_exact_records(
        one_base.records, expected, "packed22 one-base fragments");
    assert_xxl_fast_route(one_base.proof, "packed22 one-base fragments");
}

TEST_CASE("ExactSimd resident K15/W10 restores legacy dual vector_stream and keeps fragmented exactness",
          "[two_stack][resident][vector_stream][dual][proof][fragmented][parity]") {
    if (two_stack_internal::select_exact_simd_kernel_kind<
            xxl_fast_minimizer, xxl_fast_range::traits>(4096)
            != ExactSimdKernelKind::k15_w10_b8_u32_hash) {
        return;
    }

    auto data = continuation_fixture(4096);
    const auto resident = run_xxl_resident_vector_stream(data);
    const auto legacy = run_xxl_legacy_vector_stream_records(data);
    assert_exact_records(
        resident.records, legacy, "resident report-76 vector_stream parity");
    REQUIRE(resident.proof.accepted(
        /*require_nonempty_vector_work=*/true));
    REQUIRE(resident.proof.resident_vector_stream_kernel_calls == 1u);
    REQUIRE(resident.proof.resident_vector_stream_single_calls == 0u);
    REQUIRE(resident.proof.resident_vector_stream_dual_calls == 1u);
    REQUIRE(resident.proof.resident_vector_stream_nonempty_work_calls == 1u);
    REQUIRE(resident.proof.resident_vector_stream_input_bp == data.size());
    REQUIRE(resident.proof.resident_vector_stream_minimizers_emitted
        == resident.records.size());
    REQUIRE(resident.proof.generic_u64_kernel_calls == 0u);
    REQUIRE(resident.proof.k15_w10_b8_u32_hash_kernel_calls == 0u);
    REQUIRE(resident.proof.scalar_backend_branches == 0u);
    REQUIRE(resident.proof.scalar_hash_symbols == 0u);

    const std::vector<std::size_t> cuts{
        0, 1, 7, 14, 15, 16, 23, 24, 31, 127, 151, 152, 153,
        511, 1024, 2047, 2048, 3073, data.size()};
    std::vector<std::pair<std::size_t, std::size_t>> fragments;
    fragments.reserve(cuts.size() - 1u);
    for (std::size_t index = 1; index < cuts.size(); ++index) {
        fragments.emplace_back(cuts[index - 1], cuts[index]);
    }
    const auto fragmented = run_xxl_fast_fragments(data, fragments);
    assert_xxl_fast_route(
        fragmented.proof, "fragment-boundary continuation route");
    const auto scalar = scalar_exact_oracle<xxl_fast_minimizer>(
        data, 10u, 15u);
    assert_exact_records(
        fragmented.records, scalar, "fragment-boundary exact parity");

    auto mixed_kernel_evidence = resident.proof;
    mixed_kernel_evidence.k15_w10_b8_u32_hash_kernel_calls = 1u;
    REQUIRE_FALSE(mixed_kernel_evidence.accepted(false));
    auto missing_dual_evidence = resident.proof;
    missing_dual_evidence.resident_vector_stream_dual_calls = 0u;
    REQUIRE_FALSE(missing_dual_evidence.accepted(false));
    auto fabricated_scalar_evidence = resident.proof;
    fabricated_scalar_evidence.scalar_hash_symbols = 1u;
    REQUIRE_FALSE(fabricated_scalar_evidence.accepted(false));
    auto fabricated_zero_output = resident.proof;
    fabricated_zero_output.minimizers_emitted = 0u;
    fabricated_zero_output.resident_vector_stream_minimizers_emitted = 0u;
    REQUIRE_FALSE(fabricated_zero_output.accepted(false));
}

TEST_CASE("resident vector_stream fails closed before proof on unsupported targets and no-op collectors",
          "[two_stack][resident][vector_stream][fail-closed][proof][no-op]") {
    auto data = continuation_fixture(4096);
    xxl_fast_range source(data);

    SECTION("constexpr runtime dimensions must match their specialization") {
        sketch_collector_t<xxl_fast_minimizer> collector;
        constexpr std::array<std::pair<std::size_t, std::size_t>, 2>
            mismatched_dimensions{{{9u, 15u}, {10u, 14u}}};
        for (const auto& [runtime_w, runtime_k] : mismatched_dimensions) {
            try {
                (void)collect_minimizers_two_stack_nthash_resident_constexpr_or_throw<
                    10, 15, xxl_fast_minimizer>(
                        collector, source, runtime_w, runtime_k);
                FAIL("mismatched runtime dimensions must fail before resident execution");
            } catch (const ExactSimdRuntimeError& error) {
                REQUIRE(error.status()
                    == TwoStackContinuationStatus::invalid_static_configuration);
            }
        }
        REQUIRE(collector.extract_sketches_data().empty());
    }

    dropping_resident_collector<xxl_fast_minimizer> collector;
    if (!two_stack_internal::resident_vector_shape_supported()) {
        try {
            (void)collect_minimizers_two_stack_nthash_resident_or_throw<
                xxl_fast_minimizer>(collector, source, 10u, 15u);
            FAIL("unsupported Highway target must fail before resident execution");
        } catch (const ExactSimdRuntimeError& error) {
            REQUIRE(error.status()
                == TwoStackContinuationStatus::unsupported_vector_shape);
        }
        REQUIRE(collector.attempted_records == 0u);
        return;
    }

    try {
        (void)collect_minimizers_two_stack_nthash_resident_or_throw<
            xxl_fast_minimizer>(collector, source, 10u, 15u);
        FAIL("a collector that drops every emitted minimizer must fail closed");
    } catch (const ExactSimdRuntimeError& error) {
        REQUIRE(error.status() == TwoStackContinuationStatus::invalid_state);
    }
    REQUIRE(collector.attempted_records > 0u);
    REQUIRE(collector.size() == 0u);
}

TEST_CASE("ExactSimd K15/W10 B8 preserves compact tie chronology",
          "[two_stack][continuation][exact][k15_w10_b8][ties]") {
    if (two_stack_internal::select_exact_simd_kernel_kind<
            xxl_fast_minimizer, xxl_fast_range::traits>(4096)
            != ExactSimdKernelKind::k15_w10_b8_u32_hash) {
        return;
    }

    std::vector<seq::dna4> data(160);
    for (auto& symbol : data) symbol.assign_rank(0);
    const auto actual = run_xxl_fast_driver(data);
    const auto expected = scalar_exact_oracle<xxl_fast_minimizer>(
        data, 10, 15);
    assert_exact_records(actual.records, expected, "B8 repeated-A ties");
    assert_xxl_fast_route(actual.proof, "B8 repeated-A ties");
    REQUIRE(actual.proof.tie_matrix_event_batches > 0);
    REQUIRE((actual.proof.first_window_tie_events > 0
        || actual.proof.expiry_tie_events > 0));
    REQUIRE(actual.proof.tie_hash_prefilter_batches > 0);
    REQUIRE(actual.proof.tie_raw_verification_batches > 0);
    REQUIRE(actual.proof.tie_hash_prefilter_batches
        >= actual.proof.tie_raw_verification_batches);
}

TEST_CASE("ExactSimd K15/W10 packed22 preserves the reachable hash sentinel",
          "[two_stack][continuation][exact][k15_w10_b8][packed22][sentinel]") {
    if (two_stack_internal::select_exact_simd_kernel_kind<
            xxl_fast_minimizer, xxl_fast_range::traits>(4096)
            != ExactSimdKernelKind::k15_w10_b8_u32_hash) {
        return;
    }

    constexpr std::string_view sentinel_kmer = "AACATATAAATTCAA";
    std::uint32_t forward = 0;
    std::uint32_t reverse = 0;
    std::vector<seq::dna4> data;
    const std::string bases =
        std::string{"GTCGAGTACCGTACGATCGTACGATCG"}
        + std::string{sentinel_kmer}
        + std::string{"CTAGGCTACGATTCGACGTAGCTAGCA"};
    data.reserve(bases.size());
    for (const char base : bases) {
        seq::dna4 symbol{};
        symbol.assign_char(base);
        data.push_back(symbol);
    }
    for (std::size_t index = 0; index < sentinel_kmer.size(); ++index) {
        seq::dna4 symbol{};
        symbol.assign_char(sentinel_kmer[index]);
        const std::uint32_t code = symbol.to_rank();
        forward = ((forward << 2u) | code) & 0x3fffffffu;
        reverse |= (code ^ 3u) << (2u * index);
    }
    const std::uint32_t canonical = std::min(forward, reverse);
    REQUIRE((minimap_int_hasher<seq::dna4>::template scramble<
        std::uint32_t>(canonical, 0x3fffffffu)) == 0x3fffffffu);

    const auto actual = run_xxl_fast_driver(data);
    const auto expected = scalar_exact_oracle<xxl_fast_minimizer>(
        data, 10, 15);
    assert_exact_records(
        actual.records, expected, "packed22 reachable sentinel hash");
    assert_xxl_fast_route(
        actual.proof, "packed22 reachable sentinel hash");
    REQUIRE(std::ranges::none_of(actual.records,
        [](const exact_minimizer& record) {
            return record.hash == 0x3fffffffull;
        }));
}

TEST_CASE("ExactSimd K15/W10 B8 keeps 33-bit emitted positions",
          "[two_stack][continuation][exact][k15_w10_b8][position]") {
    if (two_stack_internal::select_exact_simd_kernel_kind<
            xxl_fast_minimizer, xxl_fast_range::traits>(4096)
            != ExactSimdKernelKind::k15_w10_b8_u32_hash) {
        return;
    }

    auto data = continuation_fixture(257);
    constexpr std::size_t output_base =
        static_cast<std::size_t>(
            (std::numeric_limits<std::uint32_t>::max)()) + 8192;
    const auto actual = run_xxl_fast_driver(data, output_base);
    const auto expected = scalar_exact_oracle<xxl_fast_minimizer>(
        data, 10, 15, 0, data.size(), output_base);
    assert_exact_records(actual.records, expected, "B8 33-bit positions");
    assert_xxl_fast_route(actual.proof, "B8 33-bit positions");
    REQUIRE(!actual.records.empty());
    REQUIRE(std::ranges::all_of(
        actual.records,
        [](const exact_minimizer& record) {
            return record.position
                > (std::numeric_limits<std::uint32_t>::max)();
        }));
}

TEST_CASE("ExactSimd K15/W10 B8 accepts unaligned u32 packed ownership",
          "[two_stack][continuation][exact][k15_w10_b8][unaligned]") {
    if (two_stack_internal::select_exact_simd_kernel_kind<
            xxl_fast_minimizer, xxl_fast_range::traits>(4096)
            != ExactSimdKernelKind::k15_w10_b8_u32_hash) {
        return;
    }

    constexpr std::size_t begin = 13;
    constexpr std::size_t end = 181;
    auto data = continuation_fixture(224);
    std::vector<seq::dna4> slice(
        data.begin() + static_cast<std::ptrdiff_t>(begin),
        data.begin() + static_cast<std::ptrdiff_t>(end));
    const auto expected = scalar_exact_oracle<xxl_fast_minimizer>(
        slice, 10, 15);

    xxl_fast_range source(data);
    TwoStackContinuationState<10, 15, xxl_fast_range::traits> state;
    sketch_collector_t<xxl_fast_minimizer> collector;
    REQUIRE(reset_two_stack_nthash(state, 10, 15, 0)
        == TwoStackContinuationStatus::ok);
    state.kernel_kind = ExactSimdKernelKind::k15_w10_b8_u32_hash;
    state.proof.generic_u64_kernel_calls = 0;
    state.proof.k15_w10_b8_u32_hash_kernel_calls = 1;
    const std::uint32_t* packed = source.read_cptr(source.cbegin());
    REQUIRE(consume_two_stack_nthash_packed_fragment<
            10, 15, xxl_fast_minimizer>(
                state,
                collector,
                std::span<const std::uint32_t>{
                    packed, static_cast<std::size_t>(source.size())},
                static_cast<std::size_t>(source.bp_size()),
                0,
                begin,
                end)
        == TwoStackContinuationStatus::ok);
    const auto finalized = finalize_two_stack_nthash(state, collector);
    REQUIRE(finalized.ok());
    assert_exact_records(
        exact_records(collector), expected, "B8 unaligned u32 ownership");
    assert_xxl_fast_route(finalized.proof, "B8 unaligned u32 ownership");
    REQUIRE(finalized.proof.packed_word_boundary_merges > 0);
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

// =============================================================================
// Isolated simd-minimizers/seq-hash paper POC tests.
//
// These helpers intentionally do not call any bioseq hasher or window helper.
// They are a small, direct scalar specification of u32 ntHash R=7 and an
// O(W)-per-window left/right minimum, independent from both the paper POC and
// the production ExactSimd oracle above.
// =============================================================================

inline constexpr std::array<std::uint32_t, 4>
PAPER_NTHASH_ORACLE_FORWARD_TABLE{
    0x95c60474u,
    0x62a02b4cu,
    0x82572324u,
    0x4be24456u,
};

/** Rotate one scalar u32 left by `bits` modulo 32. */
[[nodiscard]] static constexpr std::uint32_t paper_oracle_rotate_left(
    std::uint32_t value,
    std::size_t bits) noexcept
{
    return std::rotl(value, static_cast<int>(bits & 31u));
}

/** Compute canonical seq-hash u32 ntHash directly, without rolling state. */
template<std::size_t K>
[[nodiscard]] static std::uint32_t paper_oracle_kmer_hash(
    std::span<const std::uint32_t> ranks,
    std::size_t start)
{
    std::uint32_t forward = 0;
    std::uint32_t reverse = 0;
    for (std::size_t offset = 0; offset < K; ++offset) {
        const std::size_t rank = ranks[start + offset];
        ASSERT(rank < 4,
            "paper ntHash oracle accepts nt4 ranks only", rank);
        forward ^= paper_oracle_rotate_left(
            PAPER_NTHASH_ORACLE_FORWARD_TABLE[rank],
            (K - 1 - offset) * 7);
        reverse ^= paper_oracle_rotate_left(
            PAPER_NTHASH_ORACLE_FORWARD_TABLE[3 - rank],
            offset * 7);
    }
    return forward + reverse;
}

/**
 * Independent scalar minimizer oracle: one endpoint for a unique minimum,
 * both leftmost/rightmost endpoints for a tied minimum.
 */
template<std::size_t K, std::size_t W>
[[nodiscard]] static std::vector<PaperNthashMinimizer>
paper_scalar_minimizer_oracle(std::span<const std::uint32_t> ranks)
{
    const std::size_t kmer_count = ranks.size() - K + 1;
    const std::size_t window_count = kmer_count - W + 1;
    std::vector<std::uint32_t> hashes(kmer_count);
    for (std::size_t position = 0; position < kmer_count; ++position) {
        hashes[position] = paper_oracle_kmer_hash<K>(ranks, position);
    }

    std::vector<PaperNthashMinimizer> result;
    result.reserve(window_count * 2);
    for (std::size_t window_start = 0;
         window_start < window_count;
         ++window_start) {
        std::uint32_t minimum_hash =
            (std::numeric_limits<std::uint32_t>::max)();
        std::size_t left_position = window_start;
        std::size_t right_position = window_start;
        for (std::size_t offset = 0; offset < W; ++offset) {
            const std::size_t position = window_start + offset;
            if (hashes[position] < minimum_hash) {
                minimum_hash = hashes[position];
                left_position = position;
                right_position = position;
            } else if (hashes[position] == minimum_hash) {
                right_position = position;
            }
        }
        result.push_back({
            .hash = minimum_hash,
            .position = static_cast<std::uint32_t>(left_position),
            .window_start = static_cast<std::uint32_t>(window_start),
        });
        if (right_position != left_position) {
            result.push_back({
                .hash = minimum_hash,
                .position = static_cast<std::uint32_t>(right_position),
                .window_start = static_cast<std::uint32_t>(window_start),
            });
        }
    }
    return result;
}

/** Deterministic nt4 rank fixture with no dependency on sequence packing. */
[[nodiscard]] static std::vector<std::uint32_t>
paper_rank_fixture(std::size_t size)
{
    std::vector<std::uint32_t> result(size);
    std::uint32_t state = 0x243f6a88u;
    for (std::uint32_t& rank : result) {
        state = state * 1664525u + 1013904223u;
        rank = (state >> 28) & 3u;
    }
    return result;
}

/** Assert exact ordered equality while retaining useful mismatch context. */
static void assert_paper_records_equal(
    const std::vector<PaperNthashMinimizer>& actual,
    const std::vector<PaperNthashMinimizer>& expected,
    std::string_view fixture)
{
    ASSERT(actual.size() == expected.size(),
        fmt::format("{}: paper record count mismatch", fixture),
        actual.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        ASSERT(actual[index] == expected[index],
            fmt::format("{}: paper record {} mismatch", fixture, index),
            actual[index].hash,
            actual[index].position,
            actual[index].window_start,
            expected[index].hash,
            expected[index].position,
            expected[index].window_start);
    }
}

TEST_CASE("paper ntHash POC — eight streams match independent scalar oracle",
          "[two_stack][paper_nthash][simd][oracle]") {
    constexpr std::size_t K = 15;
    constexpr std::size_t W = 10;
    const auto ranks = paper_rank_fixture(521);
    const auto actual =
        collect_paper_nthash_minimizers_simd<K, W>(ranks);
    const std::size_t lanes = hn::Lanes(hn::ScalableTag<std::uint32_t>{});
    if (lanes != 4 && lanes != 8) {
        ASSERT(actual.status
                == PaperNthashSimdStatus::unsupported_vector_shape,
            "paper POC must reject non-NEON/non-AVX2 shapes without fallback");
        ASSERT(actual.proof.scalar_fallback_calls == 0,
            "unsupported target must not call a scalar backend");
        return;
    }

    ASSERT(actual.ok(), "supported native target must run paper SIMD POC");
    const auto expected = paper_scalar_minimizer_oracle<K, W>(ranks);
    assert_paper_records_equal(actual.minimizers, expected,
        "default K15/W10");
    ASSERT(actual.proof.telemetry_complete(),
        "paper POC diagnostic telemetry must be internally complete");
    ASSERT(!actual.production_acceptance_eligible(),
        "O(N) semantic POC must never be production-acceptance eligible");
    ASSERT(actual.proof.logical_streams == 8
        && actual.proof.vector_registers_per_step * lanes == 8,
        "paper POC must use exactly eight logical chunk streams",
        actual.proof.logical_streams,
        actual.proof.vector_registers_per_step,
        lanes);
    ASSERT(actual.proof.scalar_hash_symbols == 0
        && actual.proof.scalar_window_transitions == 0
        && actual.proof.scalar_tie_decisions == 0
        && actual.proof.scalar_fallback_calls == 0,
        "paper POC must perform no scalar biological fallback");
}

TEST_CASE("paper ntHash POC — canonical hash and endpoint policy are RC symmetric",
          "[two_stack][paper_nthash][simd][reverse_complement]") {
    constexpr std::size_t K = 15;
    constexpr std::size_t W = 10;
    const auto forward_ranks = paper_rank_fixture(377);
    std::vector<std::uint32_t> reverse_ranks;
    reverse_ranks.reserve(forward_ranks.size());
    for (auto iter = forward_ranks.rbegin();
         iter != forward_ranks.rend();
         ++iter) {
        reverse_ranks.push_back(3u - *iter);
    }
    const auto forward =
        collect_paper_nthash_minimizers_simd<K, W>(forward_ranks);
    const auto reverse =
        collect_paper_nthash_minimizers_simd<K, W>(reverse_ranks);
    const std::size_t lanes = hn::Lanes(hn::ScalableTag<std::uint32_t>{});
    if (lanes != 4 && lanes != 8) {
        ASSERT(forward.status
                == PaperNthashSimdStatus::unsupported_vector_shape
            && reverse.status
                == PaperNthashSimdStatus::unsupported_vector_shape,
            "RC test target must reject unsupported shapes consistently");
        return;
    }
    ASSERT(forward.ok() && reverse.ok(),
        "both orientations must run on a supported vector target");

    const std::size_t kmer_count = forward_ranks.size() - K + 1;
    const std::size_t window_count = kmer_count - W + 1;
    std::vector<PaperNthashMinimizer> mapped_reverse;
    mapped_reverse.reserve(reverse.minimizers.size());
    for (const PaperNthashMinimizer record : reverse.minimizers) {
        mapped_reverse.push_back({
            .hash = record.hash,
            .position = static_cast<std::uint32_t>(
                kmer_count - 1 - record.position),
            .window_start = static_cast<std::uint32_t>(
                window_count - 1 - record.window_start),
        });
    }
    const auto record_order = [](
        const PaperNthashMinimizer& lhs,
        const PaperNthashMinimizer& rhs) {
        return std::tie(lhs.window_start, lhs.position, lhs.hash)
            < std::tie(rhs.window_start, rhs.position, rhs.hash);
    };
    auto sorted_forward = forward.minimizers;
    std::ranges::sort(sorted_forward, record_order);
    std::ranges::sort(mapped_reverse, record_order);
    assert_paper_records_equal(
        mapped_reverse, sorted_forward, "reverse-complement mapping");

    for (std::size_t position = 0; position < kmer_count; ++position) {
        const std::size_t reverse_position = kmer_count - 1 - position;
        ASSERT(paper_oracle_kmer_hash<K>(forward_ranks, position)
                == paper_oracle_kmer_hash<K>(
                    reverse_ranks, reverse_position),
            "independent canonical ntHash oracle must itself be RC symmetric",
            position, reverse_position);
    }
}

TEST_CASE("paper ntHash POC — ties emit exactly the two minimum endpoints",
          "[two_stack][paper_nthash][simd][ties]") {
    constexpr std::size_t K = 15;
    constexpr std::size_t W = 10;
    const std::vector<std::uint32_t> ranks(96, 0u);
    const auto actual =
        collect_paper_nthash_minimizers_simd<K, W>(ranks);
    const std::size_t lanes = hn::Lanes(hn::ScalableTag<std::uint32_t>{});
    if (lanes != 4 && lanes != 8) {
        ASSERT(actual.status
                == PaperNthashSimdStatus::unsupported_vector_shape,
            "tie test must reject unsupported vector shapes");
        return;
    }
    ASSERT(actual.ok(), "constant-rank tie fixture must execute");
    const std::size_t window_count = ranks.size() - K - W + 2;
    ASSERT(actual.proof.tied_minimum_windows == window_count
        && actual.proof.unique_minimum_windows == 0
        && actual.proof.endpoints_emitted == 2 * window_count,
        "every constant-sequence window must emit two and only two endpoints",
        actual.proof.tied_minimum_windows,
        actual.proof.unique_minimum_windows,
        actual.proof.endpoints_emitted,
        window_count);
    const auto expected = paper_scalar_minimizer_oracle<K, W>(ranks);
    assert_paper_records_equal(actual.minimizers, expected,
        "constant-rank ties");
    for (std::size_t window = 0; window < window_count; ++window) {
        ASSERT(actual.minimizers[2 * window].position == window
            && actual.minimizers[2 * window + 1].position
                == window + W - 1,
            "tie policy must emit only leftmost and rightmost endpoints",
            window,
            actual.minimizers[2 * window].position,
            actual.minimizers[2 * window + 1].position);
    }
}

TEST_CASE("paper ntHash POC — inactive streams never gather past W1 input",
          "[two_stack][paper_nthash][simd][boundary]") {
    constexpr std::size_t K = 15;
    constexpr std::size_t W = 1;
    const std::size_t lanes = hn::Lanes(hn::ScalableTag<std::uint32_t>{});
    for (std::size_t window_count = 1; window_count < 8; ++window_count) {
        const auto ranks = paper_rank_fixture(K + window_count - 1);
        const auto actual =
            collect_paper_nthash_minimizers_simd<K, W>(ranks);
        if (lanes != 4 && lanes != 8) {
            ASSERT(actual.status
                    == PaperNthashSimdStatus::unsupported_vector_shape,
                "unsupported target must fail closed for W1 boundary");
            continue;
        }
        ASSERT(actual.ok(),
            "W1 boundary fixture must execute without inactive-lane OOB",
            window_count);
        const auto expected = paper_scalar_minimizer_oracle<K, W>(ranks);
        assert_paper_records_equal(
            actual.minimizers, expected, "W1 inactive streams");
        ASSERT(actual.proof.telemetry_complete(),
            "W1 diagnostic telemetry must cover every live window",
            window_count);
    }
}

TEST_CASE("paper ntHash POC — invalid ranks fail before SIMD table lookup",
          "[two_stack][paper_nthash][simd][invalid]") {
    auto ranks = paper_rank_fixture(64);
    ranks[31] = 4u;
    const auto actual = collect_paper_nthash_minimizers_simd<15, 10>(ranks);
    const std::size_t lanes = hn::Lanes(hn::ScalableTag<std::uint32_t>{});
    if (lanes != 4 && lanes != 8) {
        ASSERT(actual.status
                == PaperNthashSimdStatus::unsupported_vector_shape,
            "vector-shape rejection precedes rank validation");
        return;
    }
    ASSERT(actual.status == PaperNthashSimdStatus::invalid_rank,
        "invalid nt4 rank must fail closed before a table permutation");
    ASSERT(actual.minimizers.empty(),
        "invalid rank must not emit semantic-model records");
}

// =============================================================================
// Production-oriented packed paper-ntHash fork tests.
//
// These tests intentionally keep the scalar oracle independent of the packed
// kernel. They compare the set of selected (full-u32 hash, end position,
// strand) records because the eight logical streams append in SIMD stream
// order, whereas the oracle walks windows chronologically.
// =============================================================================

struct packed_paper_record {
    std::uint32_t hash = 0;
    std::uint32_t last_position = 0;
    std::uint32_t strand = 0;

    [[nodiscard]] constexpr auto operator<=> (
        const packed_paper_record&) const noexcept = default;
};

struct packed_paper_hash_and_strand {
    std::uint32_t hash = 0;
    std::uint32_t strand = 0;
};

template<std::size_t K>
[[nodiscard]] static packed_paper_hash_and_strand
packed_paper_oracle_hash_and_strand(
    std::span<const std::uint32_t> ranks,
    std::size_t start)
{
    std::uint32_t forward = 0;
    std::uint32_t reverse = 0;
    for (std::size_t offset = 0; offset < K; ++offset) {
        const std::uint32_t rank = ranks[start + offset];
        ASSERT(rank < 4u,
            "packed paper oracle accepts dna4 ranks only", rank);
        forward ^= paper_oracle_rotate_left(
            PAPER_NTHASH_ORACLE_FORWARD_TABLE[rank],
            (K - 1 - offset) * 7);
        reverse ^= paper_oracle_rotate_left(
            PAPER_NTHASH_ORACLE_FORWARD_TABLE[3u - rank],
            offset * 7);
    }
    return {
        .hash = static_cast<std::uint32_t>(forward + reverse),
        .strand = reverse < forward ? 1u : 0u,
    };
}

template<std::size_t K, std::size_t W>
[[nodiscard]] static std::vector<packed_paper_record>
packed_paper_scalar_oracle(std::span<const std::uint32_t> ranks)
{
    const std::size_t kmer_count = ranks.size() - K + 1;
    const std::size_t window_count = kmer_count - W + 1;
    std::vector<packed_paper_hash_and_strand> kmers(kmer_count);
    for (std::size_t position = 0; position < kmer_count; ++position) {
        kmers[position] =
            packed_paper_oracle_hash_and_strand<K>(ranks, position);
    }

    std::vector<packed_paper_record> records;
    records.reserve(window_count * 2);
    for (std::size_t window = 0; window < window_count; ++window) {
        std::uint32_t minimum_hash =
            (std::numeric_limits<std::uint32_t>::max)();
        std::size_t left_position = window;
        std::size_t right_position = window;
        for (std::size_t offset = 0; offset < W; ++offset) {
            const std::size_t position = window + offset;
            if (kmers[position].hash < minimum_hash) {
                minimum_hash = kmers[position].hash;
                left_position = position;
                right_position = position;
            } else if (kmers[position].hash == minimum_hash) {
                right_position = position;
            }
        }
        const auto append = [&](std::size_t position) {
            const packed_paper_record record{
                .hash = minimum_hash,
                .last_position = static_cast<std::uint32_t>(position + K - 1),
                .strand = kmers[position].strand,
            };
            if (records.empty() || records.back() != record) {
                records.push_back(record);
            }
        };
        append(left_position);
        if (right_position != left_position) append(right_position);
    }
    return records;
}

template<class MinimizerT = MinimapSketchData>
struct packed_paper_run {
    std::vector<packed_paper_record> records{};
    PackedPaperNthashResult result{};
};

template<
    std::size_t K = 15,
    std::size_t W = 10,
    class StorageUnit = std::uint32_t>
[[nodiscard]] static packed_paper_run<>
run_packed_paper_kernel(std::vector<seq::dna4>& data)
{
    using packed_sequence =
        sequence_custom_alphabet<seq::dna4, StorageUnit>;
    compact_sequence_range<packed_sequence> sequence(data);
    sketch_collector collector;
    auto result = collect_minimizers_packed_paper_nthash_simd<
        K, W, MinimapSketchData>(collector, sequence);
    std::vector<packed_paper_record> records;
    records.reserve(collector.extract_sketches_data().size());
    for (const MinimapSketchData record :
         collector.extract_sketches_data()) {
        records.push_back({
            .hash = static_cast<std::uint32_t>(record.kmer()),
            .last_position = static_cast<std::uint32_t>(record.last_pos()),
            .strand = static_cast<std::uint32_t>(record.strand()),
        });
    }
    return {
        .records = std::move(records),
        .result = result,
    };
}

TEST_CASE("admitted sketch collector reserves every geometric replacement peak",
          "[two_stack][collector][admission]") {
    using collector_t = admitted_sketch_collector_t<
        MinimapSketchData, bioseq::parallelism::MemoryPermit>;
    constexpr std::uint64_t factor =
        bioseq::parallelism::kVectorPreallocationHeadroomFactor;
    constexpr std::uint64_t record_bytes = sizeof(MinimapSketchData);
    constexpr std::uint64_t first_peak =
        collector_t::initial_growth_records * record_bytes * factor;
    constexpr std::uint64_t second_peak =
        collector_t::initial_growth_records * record_bytes
        + 2u * collector_t::initial_growth_records * record_bytes * factor;
    const MinimapSketchData record{1u, 0u, 0u};

    {
        bioseq::parallelism::MemoryAdmissionController admission;
        admission.set_index_memory_cap_bytes(first_peak - 1u);
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_t collector{std::move(*permit), factor};
            collector.collect(record);
            REQUIRE_FALSE(collector.ok());
            REQUIRE(collector.extract_sketches_data().empty());
            REQUIRE(collector.extract_sketches_data().capacity() == 0u);
            REQUIRE(collector.admitted_capacity_bytes() == 0u);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }
    {
        bioseq::parallelism::MemoryAdmissionController admission;
        admission.set_index_memory_cap_bytes(first_peak);
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_t collector{std::move(*permit), factor};
            collector.collect(record);
            REQUIRE(collector.ok());
            REQUIRE(collector.extract_sketches_data().size() == 1u);
            REQUIRE(collector.admitted_capacity_bytes()
                == collector.extract_sketches_data().capacity()
                    * record_bytes);
            REQUIRE(admission.reserved_bytes()
                == collector.admitted_capacity_bytes());
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }
    {
        bioseq::parallelism::MemoryAdmissionController admission;
        admission.set_index_memory_cap_bytes(second_peak - 1u);
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_t collector{std::move(*permit), factor};
            for (std::size_t index = 0;
                 index < collector_t::initial_growth_records;
                 ++index) {
                collector.collect(record);
            }
            REQUIRE(collector.ok());
            collector.collect(record);
            REQUIRE_FALSE(collector.ok());
            REQUIRE(collector.extract_sketches_data().size()
                == collector_t::initial_growth_records);
            REQUIRE(collector.extract_sketches_data().capacity()
                == collector_t::initial_growth_records);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }
    {
        bioseq::parallelism::MemoryAdmissionController admission;
        admission.set_index_memory_cap_bytes(second_peak);
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_t collector{std::move(*permit), factor};
            for (std::size_t index = 0;
                 index <= collector_t::initial_growth_records;
                 ++index) {
                collector.collect(record);
            }
            REQUIRE(collector.ok());
            REQUIRE(collector.extract_sketches_data().size()
                == collector_t::initial_growth_records + 1u);
            REQUIRE(collector.admitted_capacity_bytes()
                == collector.extract_sketches_data().capacity()
                    * record_bytes);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }
}

TEST_CASE("admitted Exact SIMD raw append obeys initial and replacement cliffs",
          "[two_stack][exact_simd][collector][admission][raw_append]") {
    using collector_t = admitted_sketch_collector_t<
        MinimapSketchData, bioseq::parallelism::MemoryPermit>;
    using storage_t = MinimapSketchData::storage_t;
    constexpr std::uint64_t factor =
        bioseq::parallelism::kVectorPreallocationHeadroomFactor;
    constexpr std::uint64_t record_bytes = sizeof(MinimapSketchData);
    constexpr std::uint64_t first_peak =
        collector_t::initial_growth_records * record_bytes * factor;
    std::array<
        storage_t,
        2u * collector_t::initial_growth_records + 1u> raw{};
    for (std::size_t index = 0; index < raw.size(); ++index) {
        raw[index] = MinimapSketchData{
            index + 1u, index, index & 1u}.raw_value();
    }

    SECTION("initial allocation is denied one byte short without a partial append") {
        bioseq::parallelism::MemoryAdmissionController admission;
        admission.set_index_memory_cap_bytes(first_peak - 1u);
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_t collector{std::move(*permit), factor};
            REQUIRE_FALSE(collector.append_raw_storage(raw.data(), 1u));
            REQUIRE_FALSE(collector.ok());
            REQUIRE(collector.extract_sketches_data().empty());
            REQUIRE(collector.extract_sketches_data().capacity() == 0u);
            REQUIRE(collector.admitted_capacity_bytes() == 0u);
            REQUIRE(admission.reserved_bytes() == 0u);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("initial allocation succeeds at the exact peak") {
        bioseq::parallelism::MemoryAdmissionController admission;
        admission.set_index_memory_cap_bytes(first_peak);
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_t collector{std::move(*permit), factor};
            REQUIRE(collector.append_raw_storage(raw.data(), 1u));
            REQUIRE(collector.ok());
            REQUIRE(collector.extract_sketches_data().size() == 1u);
            REQUIRE(collector.admitted_capacity_bytes()
                == collector.extract_sketches_data().capacity()
                    * record_bytes);
            REQUIRE(admission.reserved_bytes()
                == collector.admitted_capacity_bytes());
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("replacement is denied one byte short and preserves old ownership") {
        bioseq::parallelism::MemoryAdmissionController admission;
        admission.set_index_memory_cap_bytes(first_peak);
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_t collector{std::move(*permit), factor};
            REQUIRE(collector.append_raw_storage(raw.data(), 1u));
            const auto initial_capacity =
                collector.extract_sketches_data().capacity();
            REQUIRE(initial_capacity
                <= 2u * collector_t::initial_growth_records);
            REQUIRE(collector.append_raw_storage(
                raw.data() + 1u, initial_capacity - 1u));
            const auto old_size = collector.extract_sketches_data().size();
            const auto old_capacity =
                collector.extract_sketches_data().capacity();
            const auto old_permit = collector.admitted_capacity_bytes();
            REQUIRE(old_size == old_capacity);
            const std::uint64_t replacement_headroom =
                2u * old_capacity * record_bytes * factor;
            const std::uint64_t replacement_peak =
                old_permit + replacement_headroom;
            admission.set_index_memory_cap_bytes(replacement_peak - 1u);
            REQUIRE_FALSE(collector.append_raw_storage(
                raw.data() + old_size, 1u));
            REQUIRE_FALSE(collector.ok());
            REQUIRE(collector.extract_sketches_data().size() == old_size);
            REQUIRE(collector.extract_sketches_data().capacity()
                == old_capacity);
            REQUIRE(collector.admitted_capacity_bytes() == old_permit);
            REQUIRE(admission.reserved_bytes() == old_permit);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("replacement succeeds at the exact old-plus-new peak") {
        bioseq::parallelism::MemoryAdmissionController admission;
        admission.set_index_memory_cap_bytes(first_peak);
        {
            auto permit = admission.try_reserve(0u);
            REQUIRE(permit.has_value());
            collector_t collector{std::move(*permit), factor};
            REQUIRE(collector.append_raw_storage(raw.data(), 1u));
            const auto old_capacity =
                collector.extract_sketches_data().capacity();
            REQUIRE(old_capacity
                <= 2u * collector_t::initial_growth_records);
            REQUIRE(collector.append_raw_storage(
                raw.data() + 1u, old_capacity - 1u));
            const std::uint64_t old_permit =
                collector.admitted_capacity_bytes();
            const std::uint64_t replacement_headroom =
                2u * old_capacity * record_bytes * factor;
            const std::uint64_t replacement_peak =
                old_permit + replacement_headroom;
            admission.set_index_memory_cap_bytes(replacement_peak);
            REQUIRE(collector.append_raw_storage(
                raw.data() + old_capacity, 1u));
            REQUIRE(collector.ok());
            REQUIRE(collector.extract_sketches_data().size()
                == old_capacity + 1u);
            REQUIRE(collector.admitted_capacity_bytes()
                == collector.extract_sketches_data().capacity()
                    * record_bytes);
            REQUIRE(admission.reserved_bytes()
                == collector.admitted_capacity_bytes());
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }
}

TEST_CASE("live admitted collector composes with destination replacement peak",
          "[two_stack][collector][admission][destination]") {
    using collector_t = admitted_sketch_collector_t<
        MinimapSketchData, bioseq::parallelism::MemoryPermit>;
    constexpr std::uint64_t factor =
        bioseq::parallelism::kVectorPreallocationHeadroomFactor;
    constexpr std::uint64_t record_bytes = sizeof(MinimapSketchData);
    constexpr std::uint64_t collector_initial_peak =
        collector_t::initial_growth_records * record_bytes * factor;
    constexpr std::uint64_t initial_destination_headroom =
        record_bytes * factor;
    const MinimapSketchData collector_record{1u, 0u, 0u};
    const MinimapSketchData destination_record{2u, 1u, 1u};

    const auto run = [&](const bool exact_peak) {
        bioseq::parallelism::MemoryAdmissionController admission;
        admission.set_index_memory_cap_bytes(
            collector_initial_peak + initial_destination_headroom);
        {
            auto collector_owner = admission.try_reserve(0u);
            REQUIRE(collector_owner.has_value());
            collector_t collector{std::move(*collector_owner), factor};
            collector.collect(collector_record);
            REQUIRE(collector.ok());
            const std::uint64_t collector_bytes =
                collector.admitted_capacity_bytes();

            auto destination_owner = admission.try_reserve(0u);
            REQUIRE(destination_owner.has_value());
            bioseq::parallelism::MemoryPermit destination_permit =
                std::move(*destination_owner);
            std::vector<MinimapSketchData> destination;
            REQUIRE(destination_permit.grow_to(
                initial_destination_headroom));
            destination.reserve(1u);
            const std::uint64_t initial_destination_bytes =
                destination.capacity() * record_bytes;
            REQUIRE(initial_destination_bytes
                <= initial_destination_headroom);
            destination_permit.shrink_to(initial_destination_bytes);
            destination.resize(destination.capacity(), destination_record);

            const auto& sketches = collector.extract_sketches_data();
            const std::size_t required =
                destination.size() + sketches.size();
            REQUIRE(required > destination.capacity());
            const std::uint64_t replacement_headroom =
                required * record_bytes * factor;
            const std::uint64_t global_peak = collector_bytes
                + initial_destination_bytes + replacement_headroom;
            admission.set_index_memory_cap_bytes(
                exact_peak ? global_peak : global_peak - 1u);

            const std::size_t old_size = destination.size();
            const std::size_t old_capacity = destination.capacity();
            const std::uint64_t destination_peak =
                initial_destination_bytes + replacement_headroom;
            const bool admitted = destination_permit.grow_to(
                destination_peak);
            REQUIRE(admitted == exact_peak);
            if (!admitted) {
                REQUIRE(destination.size() == old_size);
                REQUIRE(destination.capacity() == old_capacity);
                REQUIRE(destination_permit.bytes()
                    == initial_destination_bytes);
                REQUIRE(admission.reserved_bytes()
                    == collector_bytes + initial_destination_bytes);
                return;
            }

            destination.reserve(required);
            const std::uint64_t measured_destination_bytes =
                destination.capacity() * record_bytes;
            REQUIRE(measured_destination_bytes <= replacement_headroom);
            destination_permit.shrink_to(measured_destination_bytes);
            destination.insert(
                destination.end(), sketches.begin(), sketches.end());
            REQUIRE(destination.size() == required);
            REQUIRE(admission.reserved_bytes()
                == collector_bytes + measured_destination_bytes);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    };

    SECTION("one byte short denies before destination allocation") {
        run(false);
    }
    SECTION("exact global peak succeeds and reconciles retained capacity") {
        run(true);
    }
}

TEST_CASE("admitted collector owns tie-heavy packed Paper output above input bp",
          "[two_stack][packed_paper_nthash][simd][ties][admission]") {
    using packed_sequence =
        sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    using collector_t = admitted_sketch_collector_t<
        MinimapSketchData, bioseq::parallelism::MemoryPermit>;
    std::vector<seq::dna4> data(96);
    for (seq::dna4& base : data) base.assign_rank(0u);
    compact_sequence_range<packed_sequence> sequence(data);

    bioseq::parallelism::MemoryAdmissionController admission;
    {
        auto permit = admission.try_reserve(0u);
        REQUIRE(permit.has_value());
        collector_t collector{
            std::move(*permit),
            bioseq::parallelism::kVectorPreallocationHeadroomFactor};
        const auto result = collect_minimizers_packed_paper_nthash_simd<
            15, 10, MinimapSketchData>(collector, sequence);
        const std::size_t lanes =
            hn::Lanes(hn::ScalableTag<std::uint32_t>{});
        if (lanes != 4 && lanes != 8) {
            REQUIRE_FALSE(result.ok());
        } else {
            REQUIRE(result.ok());
            REQUIRE(result.proof.accepted(true));
            REQUIRE(collector.ok());
            REQUIRE(collector.extract_sketches_data().size()
                == result.proof.minimizers_emitted);
            REQUIRE(collector.extract_sketches_data().size() > data.size());
            REQUIRE(collector.admitted_capacity_bytes()
                == collector.extract_sketches_data().capacity()
                    * sizeof(MinimapSketchData));
            REQUIRE(admission.reserved_bytes()
                == collector.admitted_capacity_bytes());
        }
    }
    REQUIRE(admission.reserved_bytes() == 0u);
}

template<
    std::size_t K = 15,
    std::size_t W = 10,
    class StorageUnit = std::uint64_t>
[[nodiscard]] static packed_paper_run<>
run_packed_paper_kernel_units(
    std::vector<seq::dna4>& data,
    std::size_t begin_unit,
    std::size_t end_unit)
{
    using packed_sequence =
        sequence_custom_alphabet<seq::dna4, StorageUnit>;
    compact_sequence_range<packed_sequence> sequence(data);
    ASSERT(begin_unit <= end_unit && end_unit <= sequence.size(),
        "packed subrange fixture units must be ordered and owned",
        begin_unit, end_unit, sequence.size());
    auto begin = sequence.begin();
    auto end = sequence.begin();
    std::advance(begin, static_cast<std::ptrdiff_t>(begin_unit));
    std::advance(end, static_cast<std::ptrdiff_t>(end_unit));
    sketch_collector collector;
    auto result = collect_minimizers_packed_paper_nthash_simd_checked<
        K, W, MinimapSketchData>(collector, sequence, begin, end);
    std::vector<packed_paper_record> records;
    records.reserve(collector.extract_sketches_data().size());
    for (const MinimapSketchData record :
         collector.extract_sketches_data()) {
        records.push_back({
            .hash = static_cast<std::uint32_t>(record.kmer()),
            .last_position = static_cast<std::uint32_t>(record.last_pos()),
            .strand = static_cast<std::uint32_t>(record.strand()),
        });
    }
    return {
        .records = std::move(records),
        .result = result,
    };
}

[[nodiscard]] static std::vector<std::uint32_t>
packed_paper_ranks(const std::vector<seq::dna4>& data)
{
    std::vector<std::uint32_t> ranks;
    ranks.reserve(data.size());
    for (const seq::dna4 symbol : data) {
        ranks.push_back(static_cast<std::uint32_t>(symbol.to_rank()));
    }
    return ranks;
}

static void assert_packed_paper_records_equal(
    const std::vector<packed_paper_record>& actual,
    const std::vector<packed_paper_record>& expected,
    std::string_view fixture)
{
    auto sorted_actual = actual;
    auto sorted_expected = expected;
    std::ranges::sort(sorted_actual);
    std::ranges::sort(sorted_expected);
    ASSERT(sorted_actual.size() == sorted_expected.size(),
        fmt::format("{}: packed paper record count mismatch", fixture),
        sorted_actual.size(), sorted_expected.size());
    for (std::size_t index = 0; index < sorted_expected.size(); ++index) {
        ASSERT(sorted_actual[index] == sorted_expected[index],
            fmt::format("{}: packed paper record {} mismatch", fixture, index),
            sorted_actual[index].hash,
            sorted_actual[index].last_position,
            sorted_actual[index].strand,
            sorted_expected[index].hash,
            sorted_expected[index].last_position,
            sorted_expected[index].strand);
    }
}

TEST_CASE("packed paper ntHash — K15/W10 resident dna4 matches scalar oracle",
          "[two_stack][packed_paper_nthash][simd][oracle]") {
    constexpr std::size_t K = 15;
    constexpr std::size_t W = 10;
    auto data = continuation_fixture(521);
    const auto ranks = packed_paper_ranks(data);
    const auto actual = run_packed_paper_kernel<K, W>(data);
    const std::size_t lanes = hn::Lanes(hn::ScalableTag<std::uint32_t>{});
    if (lanes != 4 && lanes != 8) {
        ASSERT(actual.result.status
                == PackedPaperNthashStatus::unsupported_vector_shape,
            "packed paper kernel must fail closed on unsupported targets");
        ASSERT(actual.result.proof.packed_decode_batches == 0
            && actual.result.proof.fallback_calls == 0,
            "shape rejection must precede biological work/fallback");
        return;
    }

    ASSERT(actual.result.kernel_proof_accepted(),
        "packed paper execution proof must be accepted");
    const auto expected = packed_paper_scalar_oracle<K, W>(ranks);
    assert_packed_paper_records_equal(
        actual.records, expected, "K15/W10 random resident dna4");
    ASSERT(actual.result.proof.logical_streams == 8
        && actual.result.proof.vector_registers_per_step * lanes == 8,
        "packed kernel must execute exactly eight logical streams",
        actual.result.proof.logical_streams,
        actual.result.proof.vector_registers_per_step,
        lanes);
    ASSERT(actual.result.proof.scheme_id
            == packed_paper_nthash_scheme_id
        && actual.result.proof.kmer_size == K
        && actual.result.proof.window_size == W
        && actual.result.proof.nthash_rotation == 7
        && actual.result.proof.canonical_sum_batches
            == actual.result.proof.window_update_batches,
        "packed proof must bind immutable paper-u32-r7-sum-v1 semantics");
    ASSERT(actual.result.proof.rolling_hash_batches > 0
        && actual.result.proof.window_update_batches > 0
        && actual.result.proof.emission_decision_batches > 0,
        "packed proof must attest rolling hash, bounded window and emission");
    ASSERT(actual.result.proof.scalar_hash_symbols == 0
        && actual.result.proof.scalar_window_transitions == 0
        && actual.result.proof.scalar_minimum_decisions == 0
        && actual.result.proof.fallback_calls == 0,
        "packed paper kernel must not execute a scalar biological fallback");
    ASSERT(actual.records.size()
            == actual.result.proof.minimizers_emitted,
        "raw collector multiplicity must equal proof emission count",
        actual.records.size(), actual.result.proof.minimizers_emitted);
    ASSERT(std::ranges::any_of(actual.records,
            [](const packed_paper_record& record) {
                return record.hash > 0xFFFFu;
            }),
        "packed collector must preserve more than the paper's selection u16");
    if (lanes == 4) {
        ASSERT(actual.result.proof.masked_tail_lane_visits > 0,
            "padded dna4 tail must attest actually masked lane visits");
    }
}

TEST_CASE("packed paper ntHash — reverse complement preserves hashes and flips strand",
          "[two_stack][packed_paper_nthash][simd][reverse_complement]") {
    constexpr std::size_t K = 15;
    constexpr std::size_t W = 10;
    auto forward_data = continuation_fixture(377);
    std::vector<seq::dna4> reverse_data;
    reverse_data.reserve(forward_data.size());
    for (auto iterator = forward_data.rbegin();
         iterator != forward_data.rend();
         ++iterator) {
        seq::dna4 complemented{};
        complemented.assign_rank(
            static_cast<std::uint8_t>(3u - iterator->to_rank()));
        reverse_data.push_back(complemented);
    }
    const auto forward = run_packed_paper_kernel<K, W>(forward_data);
    const auto reverse = run_packed_paper_kernel<K, W>(reverse_data);
    const std::size_t lanes = hn::Lanes(hn::ScalableTag<std::uint32_t>{});
    if (lanes != 4 && lanes != 8) {
        ASSERT(forward.result.status
                == PackedPaperNthashStatus::unsupported_vector_shape
            && reverse.result.status
                == PackedPaperNthashStatus::unsupported_vector_shape,
            "both RC orientations must reject the same unsupported target");
        return;
    }
    ASSERT(forward.result.kernel_proof_accepted()
        && reverse.result.kernel_proof_accepted(),
        "both RC orientations must carry accepted packed proofs");

    std::vector<packed_paper_record> mapped_reverse;
    mapped_reverse.reserve(reverse.records.size());
    for (const packed_paper_record record : reverse.records) {
        mapped_reverse.push_back({
            .hash = record.hash,
            .last_position = static_cast<std::uint32_t>(
                forward_data.size() + K - 2 - record.last_position),
            .strand = record.strand ^ 1u,
        });
    }
    std::ranges::sort(mapped_reverse);
    assert_packed_paper_records_equal(
        mapped_reverse, forward.records, "packed RC mapping");
}

TEST_CASE("packed paper ntHash — partial packed tails retain earlier streams",
          "[two_stack][packed_paper_nthash][simd][tail][boundary]") {
    constexpr std::array<std::size_t, 13> lengths{
        24, 25, 31, 32, 33, 63, 64, 65, 127, 128, 129, 257, 521};
    const std::size_t lanes = hn::Lanes(hn::ScalableTag<std::uint32_t>{});
    for (const std::size_t length : lengths) {
        auto data = continuation_fixture(length);
        const auto ranks = packed_paper_ranks(data);
        const auto actual = run_packed_paper_kernel<15, 10>(data);
        if (lanes != 4 && lanes != 8) {
            ASSERT(actual.result.status
                    == PackedPaperNthashStatus::unsupported_vector_shape,
                "boundary target must fail closed", length);
            continue;
        }
        ASSERT(actual.result.kernel_proof_accepted(),
            "every supported resident boundary must carry accepted proof",
            length);
        const auto expected = packed_paper_scalar_oracle<15, 10>(ranks);
        assert_packed_paper_records_equal(
            actual.records,
            expected,
            fmt::format("packed boundary length {}", length));
    }
}

TEST_CASE("packed paper ntHash — u64 packed storage downcasts zero-copy to u32",
          "[two_stack][packed_paper_nthash][simd][u64][bitcast]") {
    auto data = continuation_fixture(521);
    const auto ranks = packed_paper_ranks(data);
    const auto actual =
        run_packed_paper_kernel<15, 10, std::uint64_t>(data);
    const auto u32_control =
        run_packed_paper_kernel<15, 10, std::uint32_t>(data);
    const std::size_t lanes = hn::Lanes(hn::ScalableTag<std::uint32_t>{});
    if (lanes != 4 && lanes != 8) return;
    ASSERT(actual.result.kernel_proof_accepted(),
        "u64 resident storage must run through the bounded u32 view");
    const auto expected = packed_paper_scalar_oracle<15, 10>(ranks);
    assert_packed_paper_records_equal(
        actual.records, expected, "u64-to-u32 whole range");
    ASSERT(actual.records == u32_control.records,
        "u64 downcast must preserve exact unsorted collector order/multiplicity",
        actual.records.size(), u32_control.records.size());
    ASSERT(actual.records.size()
            == actual.result.proof.minimizers_emitted,
        "u64 raw multiplicity must exactly match proof");
}

TEST_CASE("packed paper ntHash — persistent minima dedup all stream joins",
          "[two_stack][packed_paper_nthash][simd][boundary_dedup]") {
    constexpr std::size_t length = 521;
    constexpr std::size_t K = 15;
    constexpr std::size_t W = 10;
    std::vector<seq::dna4> data;
    data.reserve(length);
    std::uint32_t state = 1;
    for (std::size_t index = 0; index < length; ++index) {
        state = state * 1664525u + 1013904223u;
        seq::dna4 symbol{};
        symbol.assign_rank(static_cast<std::uint8_t>((state >> 28) & 3u));
        data.push_back(symbol);
    }
    const auto ranks = packed_paper_ranks(data);
    const std::size_t view_units = (length + 15) / 16;
    constexpr std::size_t context_units = (K + W - 1 + 15) / 16;
    const auto [units_per_lane, offsets] =
        calculate_iteration_offsets<8>(view_units, 8, context_units);
    (void)units_per_lane;
    std::vector<packed_paper_hash_and_strand> kmers(
        length - K + 1);
    for (std::size_t position = 0; position < kmers.size(); ++position) {
        kmers[position] =
            packed_paper_oracle_hash_and_strand<K>(ranks, position);
    }
    const auto unique_minimum = [&](std::size_t window) {
        std::uint32_t minimum =
            (std::numeric_limits<std::uint32_t>::max)();
        std::size_t position = 0;
        std::size_t multiplicity = 0;
        for (std::size_t offset = 0; offset < W; ++offset) {
            const std::uint32_t hash = kmers[window + offset].hash;
            if (hash < minimum) {
                minimum = hash;
                position = window + offset;
                multiplicity = 1;
            } else if (hash == minimum) {
                ++multiplicity;
            }
        }
        return std::tuple{minimum, position, multiplicity};
    };
    for (std::size_t lane = 1; lane < 8; ++lane) {
        const std::size_t boundary = offsets[lane] * 16;
        const auto before = unique_minimum(boundary - 1);
        const auto after = unique_minimum(boundary);
        ASSERT(std::get<0>(before) == std::get<0>(after)
            && std::get<1>(before) == std::get<1>(after)
            && std::get<2>(before) == 1
            && std::get<2>(after) == 1,
            "fixture must retain one unique minimizer across every join",
            lane, boundary);
    }

    const auto actual = run_packed_paper_kernel<K, W>(data);
    const std::size_t lanes = hn::Lanes(hn::ScalableTag<std::uint32_t>{});
    if (lanes != 4 && lanes != 8) return;
    const auto expected = packed_paper_scalar_oracle<K, W>(ranks);
    assert_packed_paper_records_equal(
        actual.records, expected, "persistent boundary minima");
    ASSERT(actual.records.size() == expected.size(),
        "boundary test must compare exact multiplicity before normalization");
    ASSERT(actual.result.proof.stream_boundary_records_buffered == 7
        && actual.result.proof.stream_boundary_duplicates_suppressed == 7
        && actual.result.proof.stream_boundary_records_committed == 0,
        "all seven persistent boundary duplicates must be withheld/suppressed",
        actual.result.proof.stream_boundary_records_buffered,
        actual.result.proof.stream_boundary_duplicates_suppressed,
        actual.result.proof.stream_boundary_records_committed);
}

TEST_CASE("packed paper ntHash — ties retain endpoints and adjacent dedup",
          "[two_stack][packed_paper_nthash][simd][ties][dedup]") {
    std::vector<seq::dna4> data(257);
    for (seq::dna4& symbol : data) symbol.assign_rank(0);
    const auto ranks = packed_paper_ranks(data);
    const auto actual = run_packed_paper_kernel<15, 10>(data);
    const std::size_t lanes = hn::Lanes(hn::ScalableTag<std::uint32_t>{});
    if (lanes != 4 && lanes != 8) return;
    ASSERT(actual.result.kernel_proof_accepted(),
        "constant packed fixture must carry accepted proof");
    const auto expected = packed_paper_scalar_oracle<15, 10>(ranks);
    assert_packed_paper_records_equal(
        actual.records, expected, "packed constant ties");
    ASSERT(actual.result.proof.tied_endpoint_records_emitted > 0,
        "constant sequence must attest emitted right-tie endpoints");
    ASSERT(actual.result.proof.stream_boundary_records_buffered
            == actual.result.proof.stream_boundary_records_committed
                + actual.result.proof.stream_boundary_duplicates_suppressed,
        "bounded boundary finalization must account for every withheld record");
}

TEST_CASE("packed paper ntHash — fw equals rc has explicit strand zero",
          "[two_stack][packed_paper_nthash][simd][palindrome][strand]") {
    std::vector<seq::dna4> data;
    data.reserve(257);
    constexpr std::array<std::uint8_t, 4> acgt{0, 1, 2, 3};
    for (std::size_t index = 0; index < 257; ++index) {
        seq::dna4 symbol{};
        symbol.assign_rank(acgt[index & 3u]);
        data.push_back(symbol);
    }
    const auto ranks = packed_paper_ranks(data);
    const auto actual = run_packed_paper_kernel<4, 1>(data);
    const std::size_t lanes = hn::Lanes(hn::ScalableTag<std::uint32_t>{});
    if (lanes != 4 && lanes != 8) return;
    ASSERT(actual.result.kernel_proof_accepted(),
        "K4/W1 palindrome fixture must carry accepted kernel proof");
    const auto expected = packed_paper_scalar_oracle<4, 1>(ranks);
    assert_packed_paper_records_equal(
        actual.records, expected, "packed palindrome strand policy");
    const auto first_hash = packed_paper_oracle_hash_and_strand<4>(ranks, 0);
    ASSERT(first_hash.strand == 0,
        "ACGT has fw == rc and oracle policy must choose strand zero");
    ASSERT(std::ranges::any_of(actual.records,
            [&](const packed_paper_record& record) {
                return record.last_position == 3
                    && record.hash == first_hash.hash
                    && record.strand == 0;
            }),
        "packed kernel must retain the explicit fw==rc strand-zero record");
}

TEST_CASE("packed paper ntHash — aligned u64 subranges emit local positions",
          "[two_stack][packed_paper_nthash][simd][subrange][u64]") {
    constexpr std::size_t begin_unit = 2;
    constexpr std::size_t end_unit = 20;
    constexpr std::size_t letters_per_u64 = 32;
    auto data = continuation_fixture(1021);
    const auto all_ranks = packed_paper_ranks(data);
    const std::size_t begin_bp = begin_unit * letters_per_u64;
    const std::size_t end_bp = end_unit * letters_per_u64;
    const std::span<const std::uint32_t> local_ranks{
        all_ranks.data() + begin_bp, end_bp - begin_bp};
    const auto actual = run_packed_paper_kernel_units<
        15, 10, std::uint64_t>(data, begin_unit, end_unit);
    const auto u32_control = run_packed_paper_kernel_units<
        15, 10, std::uint32_t>(
            data, 2 * begin_unit, 2 * end_unit);
    const std::size_t lanes = hn::Lanes(hn::ScalableTag<std::uint32_t>{});
    if (lanes != 4 && lanes != 8) return;
    ASSERT(actual.result.kernel_proof_accepted(),
        "aligned u64 subrange must carry an accepted local proof");
    const auto expected = packed_paper_scalar_oracle<15, 10>(local_ranks);
    assert_packed_paper_records_equal(
        actual.records, expected, "u64 aligned local subrange");
    ASSERT(actual.records == u32_control.records,
        "equivalent u64/u32 subranges must match exact raw collector output");
    ASSERT(actual.records.size() == expected.size(),
        "subrange comparison must preserve exact multiplicity");
    ASSERT(actual.result.proof.positions_are_segment_local
        && actual.result.proof.input_symbols == end_bp - begin_bp
        && actual.result.proof.input_storage_units
            == end_unit - begin_unit,
        "subrange proof must bind local coordinate and owned-unit geometry");
    ASSERT(std::ranges::all_of(actual.records,
            [&](const packed_paper_record& record) {
                return record.last_position < end_bp - begin_bp;
            }),
        "subrange collector must never leak whole-sequence coordinates");
}

TEST_CASE("packed paper ntHash — reversed aligned range fails before work",
          "[two_stack][packed_paper_nthash][simd][range][fail_closed]") {
    using packed_sequence =
        sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    auto data = continuation_fixture(257);
    compact_sequence_range<packed_sequence> sequence(data);
    auto later = sequence.begin();
    auto earlier = sequence.begin();
    std::advance(later, 4);
    std::advance(earlier, 2);
    sketch_collector collector;
    const auto rejected =
        collect_minimizers_packed_paper_nthash_simd_checked<
            15, 10, MinimapSketchData>(
                collector, sequence, later, earlier);
    ASSERT(rejected.status == PackedPaperNthashStatus::invalid_range,
        "reversed aligned range must fail closed");
    ASSERT(rejected.proof.fail_closed_rejections == 1
        && rejected.proof == PackedPaperNthashExecutionProof{
            .fail_closed_rejections = 1},
        "range rejection must leave only its explicit rejection evidence");
    ASSERT(collector.extract_sketches_data().empty(),
        "rejected range must not emit records");
}

TEST_CASE("packed paper ntHash — narrow storage upcasts fail closed",
          "[two_stack][packed_paper_nthash][simd][storage][fail_closed]") {
    using narrow_sequence =
        sequence_custom_alphabet<seq::dna4, std::uint16_t>;
    auto data = continuation_fixture(257);
    compact_sequence_range<narrow_sequence> sequence(data);
    sketch_collector collector;
    const auto rejected =
        collect_minimizers_packed_paper_nthash_simd_checked<
            15, 10, MinimapSketchData>(
                collector, sequence, sequence.begin(), sequence.end());
    ASSERT(rejected.status
            == PackedPaperNthashStatus::unsupported_storage,
        "u8/u16-to-u32 slices must reject instead of truncating cast ratios");
    ASSERT(rejected.proof == PackedPaperNthashExecutionProof{
            .fail_closed_rejections = 1}
        && collector.extract_sketches_data().empty(),
        "narrow-storage rejection must precede decode/emission work");
}

TEST_CASE("packed paper ntHash — proof aggregation is saturating and typed",
          "[two_stack][packed_paper_nthash][proof][combine]") {
    auto data = continuation_fixture(2048);
    const auto first = run_packed_paper_kernel_units<
        15, 10, std::uint64_t>(data, 0, 16);
    const auto second = run_packed_paper_kernel_units<
        15, 10, std::uint64_t>(data, 16, 32);
    const std::size_t lanes = hn::Lanes(hn::ScalableTag<std::uint32_t>{});
    if (lanes != 4 && lanes != 8) return;
    auto combined = first.result.proof;
    combined.combine(second.result.proof);
    ASSERT(combined.segments_combined == 2
        && combined.input_symbols
            == first.result.proof.input_symbols
                + second.result.proof.input_symbols
        && combined.accepted(),
        "same-scheme local segment proofs must aggregate and remain accepted");

    auto incompatible = second.result.proof;
    ++incompatible.kmer_size;
    auto mismatch = first.result.proof;
    mismatch.combine(incompatible);
    ASSERT(mismatch.aggregation_mismatch && !mismatch.accepted(),
        "scheme/K mismatch must poison aggregate acceptance");

    auto saturated = first.result.proof;
    saturated.input_symbols =
        (std::numeric_limits<std::uint64_t>::max)();
    saturated.combine(second.result.proof);
    ASSERT(saturated.saturated && !saturated.accepted(),
        "proof counter overflow must saturate and reject acceptance");
}

}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();
