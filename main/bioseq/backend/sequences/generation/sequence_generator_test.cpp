/**
 * @file sequence_generator_test.cpp
 * @brief DOC0286 §13 — the templated generator across {dna4,dna5}×{uint32,uint64}:
 *        sampled ranks stay in [0, alphabet_size), the reverse-complement path is
 *        compiled only for nucleotide alphabets (both dna4 and dna5 are), and a
 *        generated range round-trips through to_string().
 */
#include "common_simd.h"
#include "common_tests.h"
#include "common.h"

#include <set>
#include <string>
#include <vector>

import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.sequences.generation;
import bioseq.common.testing;

namespace {

namespace gen = bioseq::sequences::generation::HWY_NAMESPACE;

template <sequence_alphabet A, typename U>
void check_generator(std::string_view label, std::string_view valid_chars) {
    gen::GeneratorParams gp{};
    gp.reference_length      = 4000;
    gp.fragment_count        = 6;
    gp.fragment_length_mean  = 400;
    gp.substitution_rate     = 0.02;
    gp.insertion_rate        = 0.005;
    gp.deletion_rate         = 0.005;
    gp.reverse_complement_rate = 0.5;
    gp.seed                  = 1234;

    auto pairs = gen::generate_test_sequences<A, U>(gp);
    ASSERT(pairs.size() == gp.fragment_count,
           fmt::format("{}: expected {} pairs, got {}", label,
                       gp.fragment_count, pairs.size()),
           label);

    const std::set<char> allowed(valid_chars.begin(), valid_chars.end());
    bool any_reverse = false;
    for (const auto& pair : pairs) {
        const std::string target = pair.target.to_string();
        const std::string query  = pair.query.to_string();
        // Target is the shared reference of exactly reference_length bases, so
        // its decoded length pins down "ranks in range + correct packing".
        ASSERT(target.size() == gp.reference_length,
               fmt::format("{}: target len {} != {}", label, target.size(),
                           gp.reference_length), label);
        ASSERT(pair.target.bp_size() == gp.reference_length,
               fmt::format("{}: target bp_size mismatch", label), label);
        for (const char c : target) {
            ASSERT(allowed.contains(c),
                   fmt::format("{}: target base '{}' not in alphabet", label, c), label);
        }
        ASSERT(!query.empty(),
               fmt::format("{}: empty query", label), label);
        for (const char c : query) {
            ASSERT(allowed.contains(c),
                   fmt::format("{}: query base '{}' not in alphabet", label, c), label);
        }
        // Round-trip: re-encode the decoded target into a fresh range and decode
        // again — must reproduce the same string (no rank/padding drift).
        std::vector<A> reencoded(target.size());
        for (std::size_t i = 0; i < target.size(); ++i) {
            reencoded[i].assign_char(target[i]);
        }
        const auto roundtrip =
            compact_sequence_range<sequence_custom_alphabet<A, U>>(reencoded);
        ASSERT(roundtrip.to_string() == target,
               fmt::format("{}: to_string round-trip drift", label), label);
        any_reverse = any_reverse || pair.is_reverse;
    }
    // reverse_complement_rate=0.5 over 6 fragments — at least one reverse flip is
    // overwhelmingly likely; both dna4 and dna5 are nucleotide so the guarded
    // reverse-complement path is compiled and exercised.
    ASSERT(any_reverse,
           fmt::format("{}: expected at least one reverse-complement fragment", label),
           label);
}

}  // namespace

TEST_CASE("generator: dna4/uint32 (bench_v10 layout)", "[generator][doc0286]") {
    check_generator<seq::dna4, std::uint32_t>("dna4/u32", "ACGT");
}
TEST_CASE("generator: dna4/uint64 (E2E layout)", "[generator][doc0286]") {
    check_generator<seq::dna4, std::uint64_t>("dna4/u64", "ACGT");
}
TEST_CASE("generator: dna5/uint32", "[generator][doc0286]") {
    check_generator<seq::dna5, std::uint32_t>("dna5/u32", "ACGTN");
}
TEST_CASE("generator: dna5/uint64 (E2E layout)", "[generator][doc0286]") {
    check_generator<seq::dna5, std::uint64_t>("dna5/u64", "ACGTN");
}
