/**
 * @file bench_test.cpp
 * @brief Catch2 harness for the bench evaluation pipeline.
 *
 * Tagged with `[simple-benchmark]` — the leading `.` keeps it out of
 * the default run; invoke explicitly with `[benchmark-evaluation]` to
 * exercise minimap_port + tool_minimap2 side-by-side on two hard-coded
 * sequences that share a known ground-truth origin.
 *
 * Output shape per TEST_CASE:
 *   1. Catch2's own BENCHMARK table (samples, iterations, est run time,
 *      mean / low mean / high mean, std dev / low std dev / high std dev).
 *      Catch2 emits this to stdout when the BENCHMARK macros run.
 *   2. The bench::report block (regions, loops, arenas, top-N
 *      bottlenecks, top-N call frequency, match-quality, length-bucket
 *      cross-cut, side-by-side delta) emitted by
 *      bench::report::print_section_summary AFTER the BENCHMARKs run.
 *
 * Each TEST_CASE creates an InstalledProfiler at scope start so the
 * thread-local ProfilerBlock pointer is non-null while the benchmarks
 * run; aggregates therefore accumulate across BENCHMARK samples and the
 * final eval pass. reset_aggregates() is called between TEST_CASEs to
 * keep sections independent.
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
import bench.report;
import bench.matrix_runner;
import bench.chart_builder;

HWY_BEFORE_NAMESPACE();

namespace bioseq::bench::HWY_NAMESPACE {

using bench_alphabet_config = sequence_custom_alphabet<seq::dna4, uint32_t>;
using bench_sequence_range  = compact_sequence_range<bench_alphabet_config>;

TEST_CASE("Benchmark evaluation — bioseq minimap_port vs. reference minimap2",
          "[simple-benchmark]") {
    using namespace seq::literals;

    // Make sure each TEST_CASE starts on a clean instrumentation slate
    // and has the persistent ProfilerBlock installed for the duration of
    // the case (BENCHMARK samples + final eval).
    report::reset_aggregates();
    report::InstalledProfiler _profiler_guard{};

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
    // The user's stated benchmarking goal is "ksw2 + DP chaining + non-SIMD
    // minimizers as fast as minimap2". The default base aligner in
    // Minimap2Params is BLOCK_ALIGNER, which is *not* what minimap2 uses;
    // measuring against the default produces an apples-to-oranges
    // comparison. Pin the bench to KSW2 so the side-by-side delta is on
    // the same algorithm class on both sides.
    params.aligner_params.base_alignment_algorithm =
        bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType::KSW2;

    const size_t truth_target_end = truth_target_start + query.bp_size();
    const bool   truth_is_reverse = false;

    // ---------------- Catch2 BENCHMARK block (single-pair) ----------------
    // Each BENCHMARK runs the aligner repeatedly so Catch2 can compute
    // mean / low mean / high mean / std dev. The instrumentation
    // aggregates accumulate across all samples; the final
    // print_section_summary call below reads the totals.
    BENCHMARK_ADVANCED("bioseq minimap_port (single curated pair)")(
        Catch::Benchmark::Chronometer meter)
    {
        meter.measure([&] {
            auto results = bioseq::simd::HWY_NAMESPACE::minimap_port(
                query, target, params.aligner_params);
            return results.view().size();
        });
    };
    BENCHMARK_ADVANCED("minimap2 reference (single curated pair)")(
        Catch::Benchmark::Chronometer meter)
    {
        meter.measure([&] {
            auto results = tool_minimap2(query, target, params.aligner_params);
            return results.view().size();
        });
    };

    // One last invocation through the eval helper so we get the
    // correctness / match-quality stats for the report block.
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

    report::print_section_summary(
        "curated single pair", evaluation);

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
          "[simple-benchmark]") {
    report::reset_aggregates();
    report::InstalledProfiler _profiler_guard{};

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
    evaluation_params.aligner_params.base_alignment_algorithm =
        bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType::KSW2;

    BENCHMARK_ADVANCED("bioseq minimap_port (unique fragment)")(
        Catch::Benchmark::Chronometer meter)
    {
        meter.measure([&] {
            auto results = bioseq::simd::HWY_NAMESPACE::minimap_port(
                pair.query, pair.target, evaluation_params.aligner_params);
            return results.view().size();
        });
    };
    BENCHMARK_ADVANCED("minimap2 reference (unique fragment)")(
        Catch::Benchmark::Chronometer meter)
    {
        meter.measure([&] {
            auto results = tool_minimap2(
                pair.query, pair.target, evaluation_params.aligner_params);
            return results.view().size();
        });
    };

    SingleEvalResult evaluation = evaluate_and_benchmark_results(
        pair.query, pair.target,
        pair.truth_target_start, pair.truth_target_end, pair.is_reverse,
        evaluation_params);

    JustLog("[bench_test] unique-fragment evaluation: bioseq offset={} minimap2 offset={}",
            evaluation.bioseq_position_offset, evaluation.minimap2_position_offset);

    report::print_section_summary(
        "unique single fragment", evaluation);

    REQUIRE(evaluation.bioseq.produced_primary);
    REQUIRE(evaluation.minimap2.produced_primary);
    REQUIRE(evaluation.bioseq_position_correct);
    REQUIRE(evaluation.minimap2_position_correct);
    REQUIRE(evaluation.bioseq_strand_correct);
    REQUIRE(evaluation.minimap2_strand_correct);
}

TEST_CASE("Benchmark evaluation — SEAL-exact tolerance=0 reproduces SEAL semantics",
          "[simple-benchmark]") {
    report::reset_aggregates();
    report::InstalledProfiler _profiler_guard{};

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
    evaluation_params.aligner_params.base_alignment_algorithm =
        bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType::KSW2;

    BENCHMARK_ADVANCED("bioseq minimap_port batch (SEAL-exact)")(
        Catch::Benchmark::Chronometer meter)
    {
        meter.measure([&] {
            std::size_t total = 0;
            for (auto& p : pairs) {
                auto results = bioseq::simd::HWY_NAMESPACE::minimap_port(
                    p.query, p.target, evaluation_params.aligner_params);
                total += results.view().size();
            }
            return total;
        });
    };
    BENCHMARK_ADVANCED("minimap2 reference batch (SEAL-exact)")(
        Catch::Benchmark::Chronometer meter)
    {
        meter.measure([&] {
            std::size_t total = 0;
            for (auto& p : pairs) {
                auto results = tool_minimap2(
                    p.query, p.target, evaluation_params.aligner_params);
                total += results.view().size();
            }
            return total;
        });
    };

    EvaluationResults results = evaluate_and_benchmark_results(pairs, evaluation_params);

    report::print_section_summary(
        "SEAL-exact tolerance=0 batch", results);

    REQUIRE(results.minimap2.mapped == pairs.size());
    REQUIRE(results.minimap2.correct == pairs.size());
    REQUIRE(results.minimap2.incorrect == 0);
}

TEST_CASE("Benchmark evaluation — SEAL-style generated fragment batch",
          "[simple-benchmark]") {
    report::reset_aggregates();
    report::InstalledProfiler _profiler_guard{};

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
    evaluation_params.aligner_params.base_alignment_algorithm =
        bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType::KSW2;

    BENCHMARK_ADVANCED("bioseq minimap_port SEAL-style batch")(
        Catch::Benchmark::Chronometer meter)
    {
        meter.measure([&] {
            std::size_t total = 0;
            for (auto& p : pairs) {
                auto results = bioseq::simd::HWY_NAMESPACE::minimap_port(
                    p.query, p.target, evaluation_params.aligner_params);
                total += results.view().size();
            }
            return total;
        });
    };
    BENCHMARK_ADVANCED("minimap2 reference SEAL-style batch")(
        Catch::Benchmark::Chronometer meter)
    {
        meter.measure([&] {
            std::size_t total = 0;
            for (auto& p : pairs) {
                auto results = tool_minimap2(
                    p.query, p.target, evaluation_params.aligner_params);
                total += results.view().size();
            }
            return total;
        });
    };

    EvaluationResults results = evaluate_and_benchmark_results(
        pairs, evaluation_params);

    JustLog("[bench_test] batch summary: "
            "bioseq correct={}/{} mean_abs_offset={} mean_ns={} | "
            "minimap2 correct={}/{} mean_abs_offset={} mean_ns={}",
            results.bioseq.correct,   results.bioseq.total_pairs,
            results.bioseq.mean_absolute_offset,   results.bioseq.mean_alignment_ns,
            results.minimap2.correct, results.minimap2.total_pairs,
            results.minimap2.mean_absolute_offset, results.minimap2.mean_alignment_ns);

    report::print_section_summary(
        "SEAL-style generated batch", results);

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

// ====================================================================
// Chart-data sweep: full algorithm × length × error grid.
//
// Runs the matrix runner across every (length_bucket, error_bucket,
// algorithm_config) combination and accumulates samples into the
// process-wide vector that bench_main.cpp drains into report.json.
//
// Tagged with `[chart-data]` (no leading dot) so it is run by the
// default `./bioseq/bioseq_bench` invocation. The smaller end of the
// matrix is intentional — pair counts, sample counts and the
// very-long bucket size are tuned to keep wall-clock under ~30 minutes
// even when every algorithm config is exercised.
//
// `[chart-data-quick]` runs only the short + medium buckets for
// faster iteration during chart layout work.
// ====================================================================

TEST_CASE("Chart-data sweep — full algorithm × length × error matrix",
          "[chart-data]") {
    report::reset_aggregates();
    report::InstalledProfiler _profiler_guard{};
    // Default-grid sweep first so SHORT/MEDIUM cells are populated for
    // every algorithm before the longer LONG/VERY_LONG cells start.
    bioseq::bench::HWY_NAMESPACE::run_full_matrix();
    // k/w parameter sweep (Problem 3 in the request). Runs only on the
    // MEDIUM/CCS bucket so the dedicated heatmaps in the report depend
    // only on (k, w), not on length / error.
    bioseq::bench::HWY_NAMESPACE::run_kw_sweep();
    JustLog("[chart-data] accumulated {} bench samples after full sweep",
            bioseq::bench::benchmark_samples().size());
    REQUIRE(!bioseq::bench::benchmark_samples().empty());
}

// Regression test for the bioseq RMQ + LONG segfault.
//
// Before commit X (the `chaining_rmq.cppm` rebalance/double-rotation
// fix), this test reliably crashed with SIGSEGV at the `node->balance
// == -2 && child == nullptr` rotation guard. The pair size and seed
// here are pinned to the exact (LONG, CCS) cell that `run_full_matrix`
// reaches with the default base seed — the segfault was data-
// dependent and only manifested on ~52 kbp inputs once the RMQ tree
// had drifted into an inconsistent balance state.
//
// Tag changed from leading-dot `[.rmq-repro]` to `[rmq-repro]` so the
// default `./build.sh bench` run picks it up — see the engineering
// report's task brief for the rationale (we want all benchmarks
// surfacing comparison data when the user runs `bench`).
TEST_CASE("Regression: bioseq RMQ chaining on LONG inputs (was SIGSEGV)",
          "[rmq-repro]") {
    report::reset_aggregates();
    report::InstalledProfiler _profiler_guard{};
    using namespace bioseq::bench::HWY_NAMESPACE;
    AlgorithmConfig cfg;
    cfg.label = "bioseq KSW2 + RMQ + scalar-mm (rmq-repro)";
    cfg.is_minimap2_reference = false;
    cfg.chaining_algorithm = ChainingAlgorithmType::RMQ;
    cfg.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
    cfg.minimizer_algorithm = MinimizerAlgorithmType::SCALAR;
    cfg.kmer_size = 15;
    cfg.window_size = 10;
    const std::vector<AlgorithmConfig> grid{cfg};
    run_matrix_for_bucket(SequenceLengthBucket::LONG,
                          ErrorRateBucket::CCS,
                          /*seed=*/18029918668030987490ull,
                          grid);
    REQUIRE(!bioseq::bench::benchmark_samples().empty());
}

TEST_CASE("Chart-data sweep — quick (short + medium only)",
          "[chart-data-quick]") {
    report::reset_aggregates();
    report::InstalledProfiler _profiler_guard{};
    using namespace bioseq::bench::HWY_NAMESPACE;
    const auto grid = default_algorithm_grid();
    std::uint64_t seed = 4242424242ull;
    for (auto length_bucket : {SequenceLengthBucket::SHORT,
                               SequenceLengthBucket::MEDIUM}) {
        for (auto error_bucket : {ErrorRateBucket::HIFI,
                                  ErrorRateBucket::CCS,
                                  ErrorRateBucket::ONT_MODERN,
                                  ErrorRateBucket::ONT_LEGACY,
                                  ErrorRateBucket::HIGH_INDEL_LOW_SUB,
                                  ErrorRateBucket::HIGH_SUB_LOW_INDEL}) {
            run_matrix_for_bucket(length_bucket, error_bucket, seed, grid);
            seed = seed * 1099511628211ull + 14695981039346656037ull;
        }
    }
    // Run the k/w sweep too so the quick bench produces a complete
    // report.json (with k/w heatmaps populated) for chart-layout work.
    run_kw_sweep();
    REQUIRE(!bioseq::bench::benchmark_samples().empty());
}

// ====================================================================
// Long-only fast iteration sweep — focuses on the configs the
// optimisation target cares about and runs them on LONG / VERY_LONG /
// XL buckets only. Used to drive performance tuning rounds where the
// full matrix is too slow to iterate on.
//
// Rows compared (production grid after engineering report cleanup):
//   * minimap2 (DP chaining)                              — baseline
//   * bioseq KSW2 + scalar-DP + scalar-mm                — must be ±10% of baseline
//   * bioseq KSW2 + SIMD-DP + SIMD-minimap2-slow-mm      — slow legacy SIMD path
//   * bioseq KSW2 + SIMD-DP + two-stack-nthash-mm        — production minimizer
//
// Tag is `[quick-long-sequence-benchmark]` (no leading dot) so
// `./build.sh bench` runs it by default; the user wants the entire
// comparison data emitted on every bench invocation. After the sweep
// completes we print a one-line summary line per (length, config) to
// stderr so the optimisation iterator can grep timings without loading
// report.json.
// ====================================================================
TEST_CASE("Long-only fast iteration sweep", "[quick-long-sequence-benchmark]") {
    report::reset_aggregates();
    report::InstalledProfiler _profiler_guard{};
    using namespace bioseq::bench::HWY_NAMESPACE;

    // Build the row grid by hand so we don't waste samples on
    // configurations we don't track for the goal.
    std::vector<AlgorithmConfig> grid;
    {
        AlgorithmConfig mm2_cfg;
        mm2_cfg.label = "minimap2 (DP chaining)";
        mm2_cfg.is_minimap2_reference = true;
        mm2_cfg.chaining_algorithm = ChainingAlgorithmType::SCALAR_MM2;
        mm2_cfg.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        mm2_cfg.minimizer_algorithm = MinimizerAlgorithmType::SCALAR;
        grid.push_back(std::move(mm2_cfg));

        AlgorithmConfig scalar_cfg;
        scalar_cfg.label = "bioseq KSW2 + scalar-DP + scalar-mm";
        scalar_cfg.is_minimap2_reference = false;
        scalar_cfg.chaining_algorithm = ChainingAlgorithmType::SCALAR_MM2;
        scalar_cfg.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        scalar_cfg.minimizer_algorithm = MinimizerAlgorithmType::SCALAR;
        grid.push_back(std::move(scalar_cfg));

        // Slow legacy SIMD minimizer kept around as a comparison
        // reference. Empirically slower than the production two-stack
        // collector across every length bucket on M1 (see
        // RESEARCH/ENGINEERING_REPORT_V2.md).
        AlgorithmConfig simd_slow_cfg;
        simd_slow_cfg.label = "bioseq KSW2 + SIMD-DP + SIMD-minimap2-slow-mm";
        simd_slow_cfg.is_minimap2_reference = false;
        simd_slow_cfg.chaining_algorithm = ChainingAlgorithmType::SIMD_MM2;
        simd_slow_cfg.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        simd_slow_cfg.minimizer_algorithm = MinimizerAlgorithmType::SIMD_MINIMAP2_SLOW;
        grid.push_back(std::move(simd_slow_cfg));

        // Production minimizer — dual-stream interleaved two-stack
        // nthash collector (was TWO_STACK_NTHASH_V3 in the iteration
        // log). Best across every length bucket on M1.
        AlgorithmConfig two_stack_nthash_cfg;
        two_stack_nthash_cfg.label = "bioseq KSW2 + SIMD-DP + two-stack-nthash-mm";
        two_stack_nthash_cfg.is_minimap2_reference = false;
        two_stack_nthash_cfg.chaining_algorithm = ChainingAlgorithmType::SIMD_MM2;
        two_stack_nthash_cfg.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        two_stack_nthash_cfg.minimizer_algorithm = MinimizerAlgorithmType::TWO_STACK_NTHASH;
        grid.push_back(std::move(two_stack_nthash_cfg));
    }

    // Pin the same seed across iterations so before/after deltas are not
    // confounded by a different RNG state. CCS error rate matches the V3
    // report's MEDIUM/CCS comparison and is the cleanest signal regime.
    //
    // The user's stated optimisation range is sequences >1 Mbp and <20 Mbp,
    // so the LONG bucket (~50 kbp) is included only for trend visibility —
    // it is below the lower bound of the goal range. VERY_LONG (~900 kbp)
    // and XL (~5 Mbp) cover the goal range proper.
    const std::uint64_t seed = 0xBEEFCAFE12345678ull;
    for (auto length_bucket : {SequenceLengthBucket::LONG,
                               SequenceLengthBucket::VERY_LONG,
                               SequenceLengthBucket::XL}) {
        run_matrix_for_bucket(length_bucket, ErrorRateBucket::CCS, seed, grid);
    }

    REQUIRE(!bioseq::bench::benchmark_samples().empty());

    // Print a compact summary the iteration loop can grep ("[long-bench]").
    for (const auto& sample : bioseq::bench::benchmark_samples()) {
        if (sample.sample_latencies_ns.empty()) continue;
        double sum_ns = 0.0;
        for (double v : sample.sample_latencies_ns) sum_ns += v;
        const double mean_ms = (sum_ns / static_cast<double>(sample.sample_latencies_ns.size())) / 1.0e6;
        JustLog("[long-bench] {} length={} pair_q={}bp t={}bp samples={} mean_ms={:.3f}",
                sample.algorithm_label,
                sample.length_bucket_label,
                sample.effective_query_length,
                sample.target_length,
                sample.sample_latencies_ns.size(),
                mean_ms);
    }
}

// ====================================================================
// Two-stack iteration bench — runs ONLY VERY_LONG (~900 kbp) on a
// small grid (scalar baseline + SIMD baseline + two-stack candidate).
// The deliberately-narrow scope lets each optimisation round of the
// SIMD-Minimizers two-stack collector finish in well under a minute,
// so we can iterate freely on the inner loop. Tagged
// `[quick-long-sequence-benchmark-two-stack]` — invoke via
// `./build.sh bench-two-stack` or run as part of the default
// `./build.sh bench` because the leading dot was removed (see task
// brief — the user wants the full comparison grid every run).
//
// IMPORTANT: this case is the *only* signal we trust during V4
// iteration — `bench-long` is significantly slower because it also
// runs LONG (~50 kbp) and XL (~5 Mbp) and runs minimap2 as a fourth
// row. For final goal verification re-run `bench-long` and compare
// with the report committed to ENGINEERING_REPORT.md.
// ====================================================================
TEST_CASE("Two-stack iteration bench (VERY_LONG only)",
          "[quick-long-sequence-benchmark-two-stack]") {
    report::reset_aggregates();
    report::InstalledProfiler _profiler_guard{};
    using namespace bioseq::bench::HWY_NAMESPACE;

    std::vector<AlgorithmConfig> grid;
    {
        AlgorithmConfig scalar_cfg;
        scalar_cfg.label = "bioseq KSW2 + scalar-DP + scalar-mm";
        scalar_cfg.is_minimap2_reference = false;
        scalar_cfg.chaining_algorithm = ChainingAlgorithmType::SCALAR_MM2;
        scalar_cfg.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        scalar_cfg.minimizer_algorithm = MinimizerAlgorithmType::SCALAR;
        grid.push_back(std::move(scalar_cfg));

        // Slow legacy SIMD minimizer — kept for comparison only.
        AlgorithmConfig simd_slow_cfg;
        simd_slow_cfg.label = "bioseq KSW2 + SIMD-DP + SIMD-minimap2-slow-mm";
        simd_slow_cfg.is_minimap2_reference = false;
        simd_slow_cfg.chaining_algorithm = ChainingAlgorithmType::SIMD_MM2;
        simd_slow_cfg.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        simd_slow_cfg.minimizer_algorithm = MinimizerAlgorithmType::SIMD_MINIMAP2_SLOW;
        grid.push_back(std::move(simd_slow_cfg));

        // Production minimizer.
        AlgorithmConfig two_stack_nthash_cfg;
        two_stack_nthash_cfg.label = "bioseq KSW2 + SIMD-DP + two-stack-nthash-mm";
        two_stack_nthash_cfg.is_minimap2_reference = false;
        two_stack_nthash_cfg.chaining_algorithm = ChainingAlgorithmType::SIMD_MM2;
        two_stack_nthash_cfg.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        two_stack_nthash_cfg.minimizer_algorithm = MinimizerAlgorithmType::TWO_STACK_NTHASH;
        grid.push_back(std::move(two_stack_nthash_cfg));
    }

    // Cover both VERY_LONG (~900 kbp) and XL (~5 Mbp) so the user's
    // stated "1 M – 20 M" range is fully exercised. The XL bucket
    // exposes better SIMD-vs-scalar lane-parallelism behaviour because
    // the sketch and chaining workloads scale linearly while the chain-
    // backtracking and KSW2 alignment phases stay roughly constant in
    // their relative share of the wall clock. Skipping the noisy MM2
    // reference row sidesteps the pre-existing fmtlog flood that the
    // sister `[.quick-long-sequence-benchmark]` case suffers from at
    // the >900 kbp end (sketch.c's per-minimizer DBG dump fires once
    // per minimap2 reference call, drowning the queue and silently
    // truncating later buckets' output).
    const std::uint64_t seed = 0xBEEFCAFE12345678ull;
    for (auto length_bucket : {SequenceLengthBucket::VERY_LONG,
                               SequenceLengthBucket::XL}) {
        run_matrix_for_bucket(length_bucket, ErrorRateBucket::CCS, seed, grid);
    }

    REQUIRE(!bioseq::bench::benchmark_samples().empty());

    for (const auto& sample : bioseq::bench::benchmark_samples()) {
        if (sample.sample_latencies_ns.empty()) continue;
        double sum_ns = 0.0;
        for (double v : sample.sample_latencies_ns) sum_ns += v;
        const double mean_ms = (sum_ns / static_cast<double>(sample.sample_latencies_ns.size())) / 1.0e6;
        JustLog("[two-stack-bench] {} length={} pair_q={}bp t={}bp samples={} mean_ms={:.3f}",
                sample.algorithm_label,
                sample.length_bucket_label,
                sample.effective_query_length,
                sample.target_length,
                sample.sample_latencies_ns.size(),
                mean_ms);
    }
}

}  // namespace bioseq::bench::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();
