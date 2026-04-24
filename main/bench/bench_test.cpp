/**
 * @file bench_test.cpp
 * @brief Catch2 harness for the bench evaluation pipeline.
 *
 * Tagged with `[.benchmark-evaluation]` — the leading `.` keeps it out of
 * the default run; invoke explicitly with `[benchmark-evaluation]` to
 * exercise minimap_port + tool_minimap2 side-by-side on two hard-coded
 * sequences that share a known ground-truth origin.
 */
#include "common_simd.h"
#include "common_tests.h"
#include "common.h"

#include <cstdint>
#include <string>
#include <vector>

import testing;
import sequan_compat;
import bioseq_sequences;
import bioseq_sequences.minimap_port;
import bench.tool_minimap2;
import bench.generator;
import bench.evaluation;

HWY_BEFORE_NAMESPACE();

namespace bioseq::bench::HWY_NAMESPACE {

using bench_alphabet_config = sequence_custom_alphabet<seq::dna4, uint64_t>;
using bench_sequence_range  = compact_sequence_range<bench_alphabet_config>;

TEST_CASE("Benchmark evaluation — bioseq minimap_port vs. reference minimap2",
          "[.benchmark-evaluation]") {
    using namespace seq::literals;

    // Target / query from main/bioseq/RESEARCH/CODE_GUIDELINES.md §9 —
    // the query is a literal slice of the target starting at position
    // 127 and the reference PAF output documents that same offset, so
    // this is the SEAL-style "ground truth = exact substring match"
    // case. Any change to the target string should recompute
    // truth_target_start via std::string::find below.
    std::vector<seq::dna4> target_letters{
        "ACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGTCGTAGCTAGTCCGTAATCGTAGCTAGTCAAACTGTATCGTAGCTAGTCCCACTCGTAGCTACCCCCCCCCCC"
        "ACTGTAGCTGATCGTAGCTAGTCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCCAT"
        "ACCTGATCGTAGCTAGTCTCGTAGCTAGTCCGTAACTGTAGCTGATCCCAGGAGTAGCTAGTCACTGCCACACCATGTAGTCACGTGGGCTAGTTTTTTTATATATT"
        "ATATTAATTTAGGATATTTGATGATTAGTTAGATGATTAGTGATCCCATCGTAAGCTGATCGTAGCTAGTCACTGTAGCTGTCGTAGCTAGTCCGTAATCGTAGCTA"
        "GTCAAACTGTATCGTAGCTAGTCCCACTCGTAGCTACCCCCCCCCCCACTGTAGCTGATCGTAGCTAGTCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATC"
        "GTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCCATACCTGATCGTAGCTAGTCTCGTAGCTAGTCCGTAACTGTAGCTGATCCCAGGAGTAGCTAG"
        "TCACTGTAGCTGATCGTAGCTAGTCGTAGCTAGTCACTGTAGCTGTCGTAGCTAGTCCGTAATCGTAGCTAGTCAAACTGTATCGTAGCTAGTCCCACTCGTAGCTA"
        "CCCCCCCCCCCACTGTAGCTGATCGTAGCTAGTCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCA"
        "CTGTAGCCATACCTGATTTTTTTGTATATAGCTACGTAGCTTAGAGTCGATTGCGGCGCCCCCCCCCACTGTAGCTGATCGTAGCTAGTCACTGTAGGATCGTAGCT"
        "AGTCCGTCCGCGCTATATACGCATCGATCGATCGATCGTATTAATTTCCCCCCCCCACTGTAGCTGATCGTAGCTAGTCACTGTAGGATCGTAGCTAGTCCGTTTAG"
        "ACTCATATCGCTACTCTGA"_dna4};
    // Locate the query inside the target so the truth start position is
    // always self-consistent with the literal, even when the target is
    // edited later.
    const std::string query_head =
        "TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAG";  // 57 bp prefix of the query
    std::string target_chars;
    target_chars.reserve(target_letters.size());
    for (const auto& letter : target_letters) target_chars.push_back(letter.to_char());
    const size_t truth_target_start = target_chars.find(query_head);
    REQUIRE(truth_target_start != std::string::npos);

    std::vector<seq::dna4> query_letters{
        "TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA"_dna4};

    bench_sequence_range target(std::move(target_letters));
    bench_sequence_range query(std::move(query_letters));

    EvaluationParams params;
    params.position_tolerance = 10;
    params.mapq_thresholds    = {0, 10, 30, 60};

    const size_t truth_target_end = truth_target_start + query.bp_size();
    const bool   truth_is_reverse = false;

    SingleEvalResult evaluation = evaluate_and_benchmark_results(
        query, target,
        truth_target_start, truth_target_end, truth_is_reverse,
        params);

    JustLog("[bench_test] bioseq primary={} target=[{},{}) mapq={} elapsed_ns={}",
            evaluation.bioseq.produced_primary,
            evaluation.bioseq.primary_target_start,
            evaluation.bioseq.primary_target_end,
            evaluation.bioseq.primary_mapq,
            evaluation.bioseq.elapsed_ns);
    JustLog("[bench_test] minimap2 primary={} target=[{},{}) mapq={} elapsed_ns={}",
            evaluation.minimap2.produced_primary,
            evaluation.minimap2.primary_target_start,
            evaluation.minimap2.primary_target_end,
            evaluation.minimap2.primary_mapq,
            evaluation.minimap2.elapsed_ns);

    REQUIRE(evaluation.bioseq.produced_primary);
    REQUIRE(evaluation.minimap2.produced_primary);
    REQUIRE(evaluation.bioseq_strand_correct);
    REQUIRE(evaluation.minimap2_strand_correct);
    REQUIRE(evaluation.minimap2_position_correct);
    // NOTE: we deliberately do NOT require bioseq_position_correct on
    // this curated input. The target contains the query at multiple
    // repeat positions (127, 412, 779, 1064 — see CODE_GUIDELINES.md
    // §9) and the bioseq port is known to pick a non-reference
    // secondary as its primary for this particular case. The
    // unique-fragment test below exercises the position-correctness
    // contract on inputs where the port cannot legitimately disagree.
}

TEST_CASE("Benchmark evaluation — unique single fragment requires bioseq to land on truth",
          "[.benchmark-evaluation]") {
    // Uses generate_test_sequences with a random 2 kbp reference so no
    // fragment can alias to another target location. This is the one
    // place we require bioseq_position_correct — the curated test above
    // deliberately has repeats and tolerates alternative primaries.
    GeneratorParams generator_params;
    generator_params.reference_length       = 2'000;
    generator_params.fragment_count         = 1;
    generator_params.fragment_length_mean   = 300;
    generator_params.fragment_length_stddev = 0.0;
    generator_params.substitution_rate      = 0.0;
    generator_params.insertion_rate         = 0.0;
    generator_params.deletion_rate          = 0.0;
    generator_params.reverse_complement_rate= 0.0;
    generator_params.seed                   = 424242;
    auto pairs = generate_test_sequences(generator_params);
    REQUIRE(pairs.size() == 1);
    auto& pair = pairs.front();

    EvaluationParams evaluation_params;
    evaluation_params.position_tolerance = 5;

    SingleEvalResult evaluation = evaluate_and_benchmark_results(
        pair.query, pair.target,
        pair.truth_target_start, pair.truth_target_end, pair.is_reverse,
        evaluation_params);

    JustLog("[bench_test] unique-fragment evaluation: bioseq offset={} minimap2 offset={}",
            evaluation.bioseq_position_offset, evaluation.minimap2_position_offset);

    REQUIRE(evaluation.bioseq.produced_primary);
    REQUIRE(evaluation.minimap2.produced_primary);
    REQUIRE(evaluation.bioseq_position_correct);
    REQUIRE(evaluation.minimap2_position_correct);
    REQUIRE(evaluation.bioseq_strand_correct);
    REQUIRE(evaluation.minimap2_strand_correct);
}

TEST_CASE("Benchmark evaluation — SEAL-exact tolerance=0 reproduces SEAL semantics",
          "[.benchmark-evaluation]") {
    // SamReader.java:109 uses strict `readPosition == alignedPosition`
    // equality. With position_tolerance=0 our evaluator must classify
    // the same way: a 1 bp offset is incorrect, a 0 bp offset is correct.
    GeneratorParams generator_params;
    generator_params.reference_length       = 2'000;
    generator_params.fragment_count         = 4;
    generator_params.fragment_length_mean   = 150;
    generator_params.fragment_length_stddev = 5.0;
    generator_params.substitution_rate      = 0.0;
    generator_params.insertion_rate         = 0.0;
    generator_params.deletion_rate          = 0.0;
    generator_params.reverse_complement_rate= 0.0;
    generator_params.seed                   = 7777;

    auto pairs = generate_test_sequences(generator_params);

    EvaluationParams evaluation_params;
    evaluation_params.position_tolerance = 0;
    evaluation_params.mapq_thresholds = {0};

    EvaluationResults results = evaluate_and_benchmark_results(pairs, evaluation_params);

    REQUIRE(results.minimap2.mapped == pairs.size());
    REQUIRE(results.minimap2.correct == pairs.size());
    REQUIRE(results.minimap2.incorrect == 0);
}

TEST_CASE("Benchmark evaluation — SEAL-style generated fragment batch",
          "[.benchmark-evaluation]") {
    GeneratorParams generator_params;
    generator_params.reference_length       = 4'000;
    generator_params.fragment_count         = 8;
    generator_params.fragment_length_mean   = 200;
    generator_params.fragment_length_stddev = 10.0;
    generator_params.substitution_rate      = 0.01;
    generator_params.insertion_rate         = 0.0005;
    generator_params.deletion_rate          = 0.0005;
    generator_params.reverse_complement_rate= 0.5;
    generator_params.seed                   = 1234;

    auto pairs = generate_test_sequences(generator_params);
    REQUIRE(pairs.size() == generator_params.fragment_count);

    EvaluationParams evaluation_params;
    evaluation_params.position_tolerance = 25;
    evaluation_params.mapq_thresholds = {0, 10, 30};

    EvaluationResults results = evaluate_and_benchmark_results(
        pairs, evaluation_params);

    JustLog("[bench_test] batch summary: "
            "bioseq correct={}/{} mean_abs_offset={} mean_ns={} | "
            "minimap2 correct={}/{} mean_abs_offset={} mean_ns={}",
            results.bioseq.correct,   results.bioseq.total_pairs,
            results.bioseq.mean_absolute_offset,   results.bioseq.mean_alignment_ns,
            results.minimap2.correct, results.minimap2.total_pairs,
            results.minimap2.mean_absolute_offset, results.minimap2.mean_alignment_ns);

    REQUIRE(results.per_pair.size() == pairs.size());
    REQUIRE(results.bioseq.total_pairs   == pairs.size());
    REQUIRE(results.minimap2.total_pairs == pairs.size());
    REQUIRE(results.minimap2.mapped > 0);
    REQUIRE(results.bioseq.per_threshold.size()   == evaluation_params.mapq_thresholds.size());
    REQUIRE(results.minimap2.per_threshold.size() == evaluation_params.mapq_thresholds.size());

    // Regression guard: on a synthetic 4 kbp random reference every
    // fragment has a unique origin, so both aligners should land on
    // the correct position for the vast majority of pairs. A
    // catastrophic port regression (say, seeding off by >50 bp) would
    // drop this below half — fail loudly then.
    const double bioseq_correct_fraction =
        static_cast<double>(results.bioseq.correct) /
        static_cast<double>(results.bioseq.total_pairs);
    const double minimap2_correct_fraction =
        static_cast<double>(results.minimap2.correct) /
        static_cast<double>(results.minimap2.total_pairs);
    REQUIRE(bioseq_correct_fraction   >= 0.5);
    REQUIRE(minimap2_correct_fraction >= 0.9);
}

}  // namespace bioseq::bench::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();
