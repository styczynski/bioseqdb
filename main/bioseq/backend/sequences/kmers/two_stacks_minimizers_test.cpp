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
// JustLog dump in NTHASH_NARROW_FIX/BASELINE_HARDCODED_OUTPUT.txt. The
// NEW impl is locked to produce a sorted-unique pair set bit-identical to
// these literals — see main/DOCS/BUGFIX/REPORT_PHASE_1__DOC0221.md for the
// before/after diff and the validation methodology.

#include "common_simd.h"
#include "common_tests.h"
#include "common.h"

#include <vector>
#include <utility>
#include <array>

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_sequences.simd;
import bioseq_sequences.kmers;
import bioseq_sequences.kmers.two_stacks;
import bioseq_sequences.kmers.hashers;
import testing;

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
// array. Logs each mismatch via JustLog before asserting, so a regression
// is easy to diagnose from CI logs without re-running locally.
template <size_t N>
static void assert_pairs_match(
    const std::vector<std::pair<size_t, uint64_t>>& got,
    const std::array<std::pair<size_t, uint64_t>, N>& expected,
    const std::string& fixture_name)
{
    if (got.size() != expected.size()) {
        for (const auto& [last_pos, kmer] : got) {
            JustLog("[{} GOT] last_pos={} kmer=0x{:x}",
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
            JustLog("[{} MISMATCH at {}] got=({}, 0x{:x}) expected=({}, 0x{:x})",
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

}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();
