/**
 * @file execution_receipts_test.cpp
 * @brief Check that each job records the algorithms and CPU or GPU work it used.
 */
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.unit.pipeline.pipe_support;

using namespace bioseq::tests::pipeline::pipe_support;

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
TEST_CASE("selected component validation keeps Vulkan chaining separate from CPU seeding",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::simd;
    const auto make_fixture = [] {
        match_result_collection collection;
        scalar_pipeline_execution_stats scalar{};
        scalar.query_minimizers.record_input(4096u);
        scalar.record_seed_lookup(128u);
        scalar.record_ksw2_call(scalar_ksw2_stage::left, 64u, 64u);
        collection.record_scalar_pipeline_execution_stats(scalar);
        partial_vulkan_execution_stats gpu{};
        gpu.chain_score_dispatches = 1u;
        collection.record_partial_vulkan_execution_stats(gpu);
        return collection;
    };
    const auto accepts = [](const match_result_collection& collection,
                            const chaining_algorithm observed_chaining =
                                chaining_algorithm::vulkan_tiled_dynamic_programming) {
        return collection.is_selected_components_execution_consistent_for_observed(
            minimizer_algorithm::scalar, chaining_algorithm::vulkan_tiled_dynamic_programming,
            base_alignment_algorithm::ksw2,
            bioseq::chaining::simd_chaining_policy::exact_visited, false, true,
            minimizer_algorithm::scalar, observed_chaining,
            base_alignment_algorithm::ksw2);
    };
    auto valid = make_fixture();
    REQUIRE(accepts(valid));
    // The CPU-only check must reject this result and preserve its GPU counters.
    REQUIRE_FALSE(valid.is_selected_scalar_sketch_execution_consistent(false, true));
    REQUIRE(valid.partial_vulkan_stats.chain_score_dispatches == 1u);
    auto missing_gpu = make_fixture();
    missing_gpu.partial_vulkan_stats = {};
    REQUIRE_FALSE(accepts(missing_gpu));
    auto stray_seed = make_fixture();
    stray_seed.partial_vulkan_stats.seed_lookup_dispatches = 1u;
    REQUIRE_FALSE(accepts(stray_seed));
    auto stray_resident = make_fixture();
    stray_resident.partial_vulkan_stats.gpu_index_materializations = 1u;
    REQUIRE_FALSE(accepts(stray_resident));
    auto scalar_fallback = make_fixture();
    scalar_fallback.scalar_pipeline_stats.record_scalar_chain(128u);
    scalar_fallback.scalar_pipeline_stats.record_scalar_chain_stage_completion(128u);
    REQUIRE_FALSE(accepts(scalar_fallback));
    auto missing_alignment = make_fixture();
    missing_alignment.scalar_pipeline_stats.ksw2_left_calls = 0u;
    missing_alignment.scalar_pipeline_stats.ksw2_input_cell_area = 0u;
    REQUIRE_FALSE(accepts(missing_alignment));
    auto mislabeled = make_fixture();
    REQUIRE_FALSE(accepts(mislabeled, chaining_algorithm::range_minimum_query));
}

TEST_CASE("selected component validation preserves GPU seeding and the chosen host suffix",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::simd;
    const auto make_fixture = [](chaining_algorithm chaining) {
        match_result_collection collection;
        scalar_pipeline_execution_stats scalar{};
        if (chaining == chaining_algorithm::scalar_minimap2 ||
            chaining == chaining_algorithm::range_minimum_query) {
            scalar.record_scalar_chain(128u);
            scalar.record_scalar_chain_stage_completion(128u);
        }
        scalar.record_ksw2_call(scalar_ksw2_stage::gap, 64u, 64u);
        collection.record_scalar_pipeline_execution_stats(scalar);
        partial_vulkan_execution_stats gpu{};
        if (chaining == chaining_algorithm::vulkan_tiled_dynamic_programming) {
            gpu.chain_score_dispatches = 1u;
        }
        collection.record_partial_vulkan_execution_stats(gpu);
        return collection;
    };
    const auto make_seed_proof = [] {
        vulkan_seed_frontend_job_execution proof{};
        proof.completion = vulkan_seed_frontend_completion_kind::mps_runtime;
        proof.seed_stats.seed_lookup_dispatches = 1u;
        proof.seed_stats.seed_sorted_dense_lookup_dispatches = 1u;
        proof.seed_stats.seed_anchor_stats_operations = 1u;
        proof.seed_stats.seed_canonical_anchor_count = 128u;
        proof.seed_stats.seed_canonical_anchor_order_checks = 1u;
        proof.seed_stats.seed_canonical_anchor_order_accepts = 1u;
        proof.operation_identity = 41u;
        proof.query_authority_identity = 17u;
        proof.query_ordinal = 0u;
        proof.minimizer_count = 64u;
        proof.target_len = 4096u;
        proof.anchor_count = 128u;
        proof.target_completion =
            vulkan_seed_target_completion_kind::mps_runtime_generated;
        proof.target_operation_identity = 23u;
        return proof;
    };
    const auto accepts = [](const match_result_collection& collection,
                            chaining_algorithm chaining,
                            const vulkan_seed_frontend_job_execution& seed_proof) {
        return collection.is_selected_components_execution_consistent_for_observed(
            minimizer_algorithm::vulkan_minimap_integer, chaining,
            base_alignment_algorithm::ksw2,
            bioseq::chaining::simd_chaining_policy::exact_visited, false, true,
            minimizer_algorithm::vulkan_minimap_integer, chaining,
            base_alignment_algorithm::ksw2, std::addressof(seed_proof));
    };
    for (const auto chaining : {chaining_algorithm::scalar_minimap2,
                                chaining_algorithm::range_minimum_query,
                                chaining_algorithm::vulkan_tiled_dynamic_programming}) {
        auto valid = make_fixture(chaining);
        const auto valid_seed = make_seed_proof();
        REQUIRE(accepts(valid, chaining, valid_seed));
        if (chaining == chaining_algorithm::range_minimum_query) {
            auto missing_rmq_stage_receipt = make_fixture(chaining);
            missing_rmq_stage_receipt.scalar_pipeline_stats.chain_stage_completion_calls = 0u;
            missing_rmq_stage_receipt.scalar_pipeline_stats.chain_stage_completed_anchor_inputs =
                0u;
            REQUIRE_FALSE(accepts(missing_rmq_stage_receipt, chaining, valid_seed));
            auto mismatched_rmq_receipt = make_fixture(chaining);
            mismatched_rmq_receipt.scalar_pipeline_stats.chain_backtrack_calls = 0u;
            REQUIRE_FALSE(accepts(mismatched_rmq_receipt, chaining, valid_seed));
        }
        auto missing_seed = make_fixture(chaining);
        auto missing_seed_proof = make_seed_proof();
        missing_seed_proof.seed_stats.seed_lookup_dispatches = 0u;
        REQUIRE_FALSE(accepts(missing_seed, chaining, missing_seed_proof));
        auto wrong_representation = make_fixture(chaining);
        auto wrong_representation_proof = make_seed_proof();
        wrong_representation_proof.seed_stats.seed_open_addressed_lookup_dispatches = 1u;
        REQUIRE_FALSE(accepts(wrong_representation, chaining, wrong_representation_proof));
        auto scalar_seed = make_fixture(chaining);
        scalar_seed.scalar_pipeline_stats.record_seed_lookup(128u);
        REQUIRE_FALSE(accepts(scalar_seed, chaining, valid_seed));
        auto host_sort_fallback = make_fixture(chaining);
        auto host_sort_proof = make_seed_proof();
        host_sort_proof.seed_stats.seed_host_sort_fallbacks = 1u;
        REQUIRE_FALSE(accepts(host_sort_fallback, chaining, host_sort_proof));
        auto rejected_order = make_fixture(chaining);
        auto rejected_order_proof = make_seed_proof();
        rejected_order_proof.seed_stats.seed_canonical_anchor_order_accepts = 0u;
        REQUIRE_FALSE(accepts(rejected_order, chaining, rejected_order_proof));
        auto target_rebuild = make_fixture(chaining);
        target_rebuild.partial_vulkan_stats.target_index_build_dispatches = 1u;
        REQUIRE_FALSE(accepts(target_rebuild, chaining, valid_seed));
    }
}

TEST_CASE("hybrid CPU Vulkan stage proof preserves selected engines and the host tail",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    using seed = minimizer_algorithm_identity;
    using chain = chaining_algorithm_identity;
    using base = base_alignment_algorithm_identity;
    REQUIRE(static_cast<std::uint8_t>(executed_backend::none) == 0u);
    REQUIRE(static_cast<std::uint8_t>(executed_backend::vulkan_gpu) == 3u);
    REQUIRE(static_cast<std::uint8_t>(executed_backend::mixed_invalid) == 4u);
    REQUIRE(static_cast<std::uint8_t>(executed_backend::mixed_cpu) == 5u);
    REQUIRE(static_cast<std::uint8_t>(executed_backend::hybrid_cpu_vulkan) == 6u);
    REQUIRE(executed_backend_name(executed_backend::hybrid_cpu_vulkan) == "hybrid_cpu_vulkan");
    REQUIRE(merge_backend_identity(executed_backend::hybrid_cpu_vulkan,
                                  executed_backend::hybrid_cpu_vulkan) ==
            executed_backend::hybrid_cpu_vulkan);
    // Combining CPU-only and GPU-only jobs must report incompatible backends.
    REQUIRE(merge_backend_identity(executed_backend::scalar_cpu, executed_backend::vulkan_gpu) ==
            executed_backend::mixed_invalid);
    REQUIRE(merge_backend_identity(executed_backend::hybrid_cpu_vulkan,
                                  executed_backend::vulkan_gpu) == executed_backend::mixed_invalid);
    REQUIRE(is_environment_valid_for_backend(make_gpu_environment(),
                                             executed_backend::hybrid_cpu_vulkan));
    auto missing_gpu = make_gpu_environment();
    missing_gpu.is_gpu_environment_available = false;
    REQUIRE_FALSE(is_environment_valid_for_backend(missing_gpu,
                                                   executed_backend::hybrid_cpu_vulkan));

    const auto one = [](execution_engine engine) {
        stage_coverage coverage{};
        coverage.eligible_jobs = 1u;
        coverage.executed_jobs[engine_index(engine)] = 1u;
        return coverage;
    };
    const auto accepts = [](const stage_coverage& coverage, pipeline_stage stage,
                            seed minimizer, chain chaining, base alignment,
                            std::uint64_t maximum = 1u) {
        return is_hybrid_stage_coverage_exclusive(coverage,
            target_index_source::recomputed, stage, minimizer, chaining, alignment, maximum);
    };
    const auto gpu = one(execution_engine::vulkan_gpu);
    const auto scalar = one(execution_engine::scalar_cpu);
    const auto simd = one(execution_engine::simd_cpu);
    REQUIRE(accepts(simd, pipeline_stage::query_sketch, seed::bounded_nthash_simd,
                    chain::vulkan_tiled_dynamic_programming, base::block_aligner));
    REQUIRE_FALSE(accepts(scalar, pipeline_stage::query_sketch, seed::bounded_nthash_simd,
                          chain::vulkan_tiled_dynamic_programming, base::block_aligner));
    REQUIRE(accepts(gpu, pipeline_stage::chain_score, seed::bounded_nthash_simd,
                    chain::vulkan_tiled_dynamic_programming, base::block_aligner));
    REQUIRE(accepts(scalar, pipeline_stage::chain_backtrack, seed::bounded_nthash_simd,
                    chain::vulkan_tiled_dynamic_programming, base::block_aligner));
    REQUIRE_FALSE(accepts(gpu, pipeline_stage::chain_backtrack, seed::bounded_nthash_simd,
                          chain::vulkan_tiled_dynamic_programming, base::block_aligner));
    for (const auto selected_chain : {chain::scalar_minimap2, chain::range_minimum_query,
                                     chain::simd_minimap2, chain::vulkan_tiled_dynamic_programming}) {
        CAPTURE(selected_chain);
        REQUIRE(accepts(gpu, pipeline_stage::seed_lookup, seed::vulkan_minimap_integer,
                        selected_chain, base::ksw2));
        REQUIRE_FALSE(accepts(scalar, pipeline_stage::seed_lookup, seed::vulkan_minimap_integer,
                              selected_chain, base::ksw2));
        REQUIRE(accepts(scalar, pipeline_stage::mapq, seed::vulkan_minimap_integer,
                        selected_chain, base::ksw2));
        REQUIRE_FALSE(accepts(gpu, pipeline_stage::mapq, seed::vulkan_minimap_integer,
                              selected_chain, base::ksw2));
    }
    REQUIRE(accepts(simd, pipeline_stage::gap_extension, seed::vulkan_minimap_integer,
                    chain::simd_minimap2, base::block_aligner));
    REQUIRE_FALSE(accepts(scalar, pipeline_stage::gap_extension, seed::vulkan_minimap_integer,
                          chain::simd_minimap2, base::block_aligner));
    REQUIRE_FALSE(accepts(gpu, pipeline_stage::gap_extension, seed::vulkan_minimap_integer,
                          chain::vulkan_tiled_dynamic_programming, base::vulkan_simple_cigar));
    REQUIRE_FALSE(accepts(simd, pipeline_stage::query_sketch, seed::two_stack_nthash,
                          chain::simd_minimap2, base::block_aligner));
    REQUIRE_FALSE(accepts(gpu, pipeline_stage::query_sketch, seed::none,
                          chain::vulkan_tiled_dynamic_programming, base::ksw2));
    auto fallback = gpu;
    fallback.fallback_jobs = 1u;
    REQUIRE_FALSE(accepts(fallback, pipeline_stage::query_sketch, seed::vulkan_minimap_integer,
                          chain::simd_minimap2, base::block_aligner));
    auto mixed_stage = gpu;
    mixed_stage.executed_jobs[engine_index(execution_engine::scalar_cpu)] = 1u;
    REQUIRE_FALSE(accepts(mixed_stage, pipeline_stage::query_sketch, seed::vulkan_minimap_integer,
                          chain::simd_minimap2, base::block_aligner));
    auto aggregate = gpu;
    aggregate.eligible_jobs = 5u;
    aggregate.executed_jobs[engine_index(execution_engine::vulkan_gpu)] = 5u;
    REQUIRE(accepts(aggregate, pipeline_stage::query_sketch, seed::vulkan_minimap_integer,
                    chain::simd_minimap2, base::block_aligner, 5u));
    REQUIRE_FALSE(accepts(aggregate, pipeline_stage::query_sketch, seed::vulkan_minimap_integer,
                          chain::simd_minimap2, base::block_aligner, 4u));

    pipeline_job_execution_stats zero_query{};
    zero_query.requested_backend = executed_backend::hybrid_cpu_vulkan;
    zero_query.actual_backend = executed_backend::hybrid_cpu_vulkan;
    zero_query.requested_target_index_source = target_index_source::recomputed;
    zero_query.actual_target_index_source = target_index_source::recomputed;
    zero_query.environment = make_gpu_environment();
    zero_query.stages.coverage[stage_index(pipeline_stage::query_sketch)] = gpu;
    REQUIRE_FALSE(is_job_execution_stats_valid(
        zero_query, seed::vulkan_minimap_integer, chain::scalar_minimap2, base::ksw2));
    REQUIRE(is_job_execution_stats_valid(
        zero_query, seed::vulkan_minimap_integer, chain::scalar_minimap2, base::ksw2,
        /*authenticated_query_empty_recomputed_bypass=*/true));
    auto fabricated_target_work = zero_query;
    fabricated_target_work.stages.coverage[
        stage_index(pipeline_stage::target_sketch)] = gpu;
    REQUIRE_FALSE(is_job_execution_stats_valid(
        fabricated_target_work, seed::vulkan_minimap_integer, chain::scalar_minimap2,
        base::ksw2, /*authenticated_query_empty_recomputed_bypass=*/true));
    REQUIRE_FALSE(is_job_execution_stats_valid(
        zero_query, seed::scalar, chain::scalar_minimap2, base::ksw2,
        /*authenticated_query_empty_recomputed_bypass=*/true));
}
#endif

TEST_CASE("Highway execution stats merges preserve exact target observations",
          "[component:bioseq][layer:unit][feature:pipeline][resource:highway]") {
    constexpr std::int64_t target_a = std::int64_t{1} << 20u;
    constexpr std::int64_t target_b = std::int64_t{1} << 21u;
    constexpr std::int64_t both_targets = target_a | target_b;

    const auto make_plan = [](
            const std::int64_t compiled,
            const std::int64_t supported,
            const std::int64_t planned) {
        return ::bioseq::simd_dispatch::highway_dispatch_plan{
            .compiled_targets = compiled,
            .cpu_supported_targets = supported,
            .runnable_targets = compiled & supported,
            .planned_target = planned,
        };
    };
    const auto plan_a = make_plan(both_targets, both_targets, target_a);
    const auto plan_b = make_plan(both_targets, both_targets, target_b);
    const auto plan_a_only = make_plan(target_a, target_a, target_a);

    const auto make_stats = [](
            const ::bioseq::simd_dispatch::highway_dispatch_plan& plan,
            const std::int64_t executed_target,
            const std::uint64_t target_row) {
        auto job = make_attested_job(
            executed_backend::simd_cpu, target_index_source::recomputed);
        job.logical_target_row = target_row;
        job.query_ordinal = 0;
        job.environment = {};
        job.environment.effective_cpu_workers = 2;
        job.environment.record_highway_target(plan, executed_target);
        pipeline_execution_stats stats{};
        stats.merge_job(job);
        return stats;
    };

    SECTION("the canonical empty aggregate is a two-sided identity") {
        const auto stats = make_stats(plan_a, target_a, 1);
        REQUIRE(pipeline_execution_stats::can_retain_full_attestation(stats));

        pipeline_execution_stats empty_on_left{};
        empty_on_left.merge(stats);
        REQUIRE(empty_on_left.environment.highway_compiled_targets
                == both_targets);
        REQUIRE(empty_on_left.environment.highway_cpu_supported_targets
                == both_targets);
        REQUIRE(empty_on_left.environment.highway_runnable_targets
                == both_targets);
        REQUIRE(empty_on_left.environment.highway_executed_target
                == target_a);
        REQUIRE_FALSE(
            empty_on_left.environment.is_highway_executed_target_mixed);
        REQUIRE_FALSE(empty_on_left.environment.is_mixed_invalid);
        REQUIRE(empty_on_left.attempted_alignment_jobs == 1);
        REQUIRE(empty_on_left.fully_attested_alignment_jobs == 1);
        REQUIRE(pipeline_execution_stats::can_retain_full_attestation(
            empty_on_left));

        auto empty_on_right = stats;
        empty_on_right.merge(pipeline_execution_stats{});
        REQUIRE(empty_on_right.environment.highway_compiled_targets
                == both_targets);
        REQUIRE(empty_on_right.environment.highway_executed_target
                == target_a);
        REQUIRE_FALSE(
            empty_on_right.environment.is_highway_executed_target_mixed);
        REQUIRE_FALSE(empty_on_right.environment.is_mixed_invalid);
        REQUIRE(empty_on_right.attempted_alignment_jobs == 1);
        REQUIRE(empty_on_right.fully_attested_alignment_jobs == 1);
        REQUIRE(pipeline_execution_stats::can_retain_full_attestation(
            empty_on_right));
    }

    SECTION("equal masks and executed targets remain singular") {
        auto merged = make_stats(plan_a, target_a, 2);
        merged.merge(make_stats(plan_a, target_a, 3));
        REQUIRE(merged.environment.highway_compiled_targets == both_targets);
        REQUIRE(merged.environment.highway_cpu_supported_targets
                == both_targets);
        REQUIRE(merged.environment.highway_runnable_targets == both_targets);
        REQUIRE(merged.environment.highway_executed_target == target_a);
        REQUIRE_FALSE(merged.environment.is_highway_executed_target_mixed);
        REQUIRE_FALSE(merged.environment.is_mixed_invalid);
        REQUIRE(merged.attempted_alignment_jobs == 2);
        REQUIRE(merged.fully_attested_alignment_jobs == 2);
        REQUIRE(merged.unattested_alignment_jobs == 0);
        REQUIRE(pipeline_execution_stats::can_retain_full_attestation(merged));
    }

    SECTION("different targets become mixed and mixed is permanent") {
        auto mixed = make_stats(plan_a, target_a, 4);
        mixed.merge(make_stats(plan_b, target_b, 5));
        REQUIRE(mixed.environment.highway_compiled_targets == both_targets);
        REQUIRE(mixed.environment.highway_runnable_targets == both_targets);
        REQUIRE(mixed.environment.highway_executed_target == 0);
        REQUIRE(mixed.environment.is_highway_executed_target_mixed);
        REQUIRE_FALSE(mixed.environment.is_mixed_invalid);
        REQUIRE(mixed.attempted_alignment_jobs == 2);
        REQUIRE(mixed.fully_attested_alignment_jobs == 0);
        REQUIRE(mixed.unattested_alignment_jobs == 2);
        REQUIRE(mixed.first_unattested_target_row == 4);
        REQUIRE(mixed.first_unattested_query_ordinal == 0);
        REQUIRE_FALSE(pipeline_execution_stats::can_retain_full_attestation(
            mixed));

        mixed.merge(make_stats(plan_a, target_a, 6));
        REQUIRE(mixed.environment.highway_executed_target == 0);
        REQUIRE(mixed.environment.is_highway_executed_target_mixed);
        REQUIRE(mixed.environment.is_mixed_invalid);
        REQUIRE(mixed.attempted_alignment_jobs == 3);
        REQUIRE(mixed.fully_attested_alignment_jobs == 0);
        REQUIRE(mixed.unattested_alignment_jobs == 3);
    }

    SECTION("mask drift is an invariant failure") {
        auto drift = make_stats(plan_a, target_a, 7);
        drift.merge(make_stats(plan_a_only, target_a, 8));
        REQUIRE(drift.environment.is_mixed_invalid);
        REQUIRE_FALSE(pipeline_execution_stats::can_retain_full_attestation(
            drift));
        REQUIRE(drift.attempted_alignment_jobs == 2);
        REQUIRE(drift.fully_attested_alignment_jobs == 0);
        REQUIRE(drift.unattested_alignment_jobs == 2);
    }

    SECTION("a malformed recorded target is never attested") {
        const auto malformed = make_stats(plan_a, target_b, 9);
        REQUIRE(malformed.environment.is_mixed_invalid);
        REQUIRE(malformed.environment.highway_executed_target == 0);
        REQUIRE_FALSE(execution_environment_snapshot::has_valid_highway_fields(
            malformed.environment));
        REQUIRE(malformed.attempted_alignment_jobs == 1);
        REQUIRE(malformed.fully_attested_alignment_jobs == 0);
        REQUIRE(malformed.unattested_alignment_jobs == 1);
        REQUIRE_FALSE(pipeline_execution_stats::can_retain_full_attestation(
            malformed));
    }
}

TEST_CASE("pipeline stats retain policy identity and distinct vector activity",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;

    auto bounded = make_attested_job(
        executed_backend::simd_cpu, target_index_source::recomputed);
    REQUIRE(is_job_execution_stats_valid(bounded));

    auto mismatched = bounded;
    mismatched.observed_simd_chaining_policy =
        simd_chaining_policy_identity::exact_visited;
    REQUIRE_FALSE(is_job_execution_stats_valid(mismatched));

    auto missing_block_identity = bounded;
    missing_block_identity.observed_simd_alignment_algorithm =
        simd_alignment_algorithm_identity::none;
    REQUIRE_FALSE(is_job_execution_stats_valid(missing_block_identity));

    auto scalar_with_leaked_policy = make_attested_job(
        executed_backend::scalar_cpu, target_index_source::recomputed);
    scalar_with_leaked_policy.requested_simd_chaining_policy =
        simd_chaining_policy_identity::bounded_batch_approximate;
    scalar_with_leaked_policy.observed_simd_chaining_policy =
        simd_chaining_policy_identity::bounded_batch_approximate;
    REQUIRE_FALSE(is_job_execution_stats_valid(scalar_with_leaked_policy));

    pipeline_execution_stats aggregate{};
    aggregate.merge_job(bounded);
    REQUIRE(aggregate.fully_attested_alignment_jobs == 1);
    REQUIRE(aggregate.requested_simd_chaining_policy
            == simd_chaining_policy_identity::bounded_batch_approximate);
    REQUIRE(aggregate.observed_simd_chaining_policy
            == simd_chaining_policy_identity::bounded_batch_approximate);
    REQUIRE(aggregate.requested_simd_alignment_algorithm
            == simd_alignment_algorithm_identity::block_aligner);
    REQUIRE(aggregate.observed_simd_alignment_algorithm
            == simd_alignment_algorithm_identity::block_aligner);

    auto exact = bounded;
    exact.logical_target_row = 1;
    exact.requested_simd_chaining_policy =
        simd_chaining_policy_identity::exact_visited;
    exact.observed_simd_chaining_policy =
        simd_chaining_policy_identity::exact_visited;
    REQUIRE(is_job_execution_stats_valid(exact));
    aggregate.merge_job(exact);
    REQUIRE(aggregate.requested_simd_chaining_policy
            == simd_chaining_policy_identity::mixed_invalid);
    REQUIRE(aggregate.observed_simd_chaining_policy
            == simd_chaining_policy_identity::mixed_invalid);
    REQUIRE(aggregate.attempted_alignment_jobs == 2);
    REQUIRE(aggregate.fully_attested_alignment_jobs == 0);
    REQUIRE(aggregate.unattested_alignment_jobs == 2);

    pipeline_execution_stats bounded_group{};
    bounded_group.merge_job(bounded);
    pipeline_execution_stats exact_group{};
    exact_group.merge_job(exact);
    bounded_group.merge(exact_group);
    REQUIRE(bounded_group.requested_simd_chaining_policy
            == simd_chaining_policy_identity::mixed_invalid);
    REQUIRE(bounded_group.observed_simd_chaining_policy
            == simd_chaining_policy_identity::mixed_invalid);
    REQUIRE(bounded_group.fully_attested_alignment_jobs == 0);
    REQUIRE(bounded_group.unattested_alignment_jobs == 2);

    pipeline_operational_stats continuation{};
    continuation.nthash_vector_batches = 7;
    continuation.simd_chaining_exact_policy_calls = 1;
    pipeline_operational_stats resident{};
    resident.nthash_resident_vector_stream_kernel_calls = 2;
    resident.simd_chaining_bounded_policy_calls = 2;
    resident.simd_chaining_bounded_score_batches = 11;
    resident.simd_chaining_bounded_cutoff_stops = 3;
    resident.simd_chaining_bounded_candidates_skipped = 29;
    continuation.merge(resident);

    REQUIRE(continuation.has_nthash_vector_activity());
    REQUIRE(continuation.nthash_vector_batches == 7);
    REQUIRE(continuation.nthash_resident_vector_stream_kernel_calls == 2);
    REQUIRE(continuation.simd_chaining_exact_policy_calls == 1);
    REQUIRE(continuation.simd_chaining_bounded_policy_calls == 2);
    REQUIRE(continuation.simd_chaining_bounded_score_batches == 11);
    REQUIRE(continuation.simd_chaining_bounded_cutoff_stops == 3);
    REQUIRE(continuation.simd_chaining_bounded_candidates_skipped == 29);

    pipeline_operational_stats resident_only{};
    resident_only.nthash_resident_vector_stream_kernel_calls = 1;
    REQUIRE(resident_only.has_nthash_vector_activity());
    REQUIRE(resident_only.nthash_vector_batches == 0);

    block_aligner_stage_stats first_block{};
    first_block.is_available = true;
    first_block.is_accepted = true;
    first_block.executed_calls = 2;
    first_block.nonempty_calls = 2;
    first_block.nonempty_calls_with_simd = 2;
    first_block.simd_dp_vector_batches = 9;
    first_block.simd_dp_cells = 144;
    block_aligner_stage_stats second_block = first_block;
    second_block.executed_calls = 1;
    second_block.nonempty_calls = 1;
    second_block.nonempty_calls_with_simd = 1;
    second_block.simd_dp_vector_batches = 4;
    second_block.simd_dp_cells = 64;
    REQUIRE_FALSE(first_block.merge(second_block));
    REQUIRE(first_block.is_available);
    REQUIRE(first_block.is_accepted);
    REQUIRE(first_block.executed_calls == 3);
    REQUIRE(first_block.nonempty_calls == 3);
    REQUIRE(first_block.nonempty_calls_with_simd == 3);
    REQUIRE(first_block.simd_dp_vector_batches == 13);
    REQUIRE(first_block.simd_dp_cells == 208);
    REQUIRE(first_block.forbidden_ksw2_calls == 0);
    REQUIRE(first_block.forbidden_scalar_biological_fallbacks == 0);
    REQUIRE_FALSE(first_block.is_saturated);

    block_aligner_stage_stats forbidden_block{};
    forbidden_block.is_available = true;
    forbidden_block.is_accepted = false;
    forbidden_block.forbidden_ksw2_calls = 1;
    REQUIRE_FALSE(first_block.merge(forbidden_block));
    REQUIRE_FALSE(first_block.is_accepted);
    REQUIRE(first_block.forbidden_ksw2_calls == 1);

    for (const bool saturated_policy : {false, true}) {
        pipeline_execution_stats rejected{};
        rejected.merge_job(bounded);
        pipeline_operational_stats invalid_policy{};
        invalid_policy.simd_chaining_policy_rejections =
            saturated_policy ? 0 : 1;
        invalid_policy.are_simd_chaining_policy_stats_saturated =
            saturated_policy;
        rejected.merge_operational(invalid_policy);
        REQUIRE(rejected.fully_attested_alignment_jobs == 0);
        REQUIRE(rejected.unattested_alignment_jobs == 1);
        REQUIRE_FALSE(pipeline_execution_stats::can_retain_full_attestation(
            rejected));
    }
}

TEST_CASE("prepared CRC identities preserve all u64 bits and publish once",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    constexpr std::array<std::uint64_t, 4> identities{
        0,
        k_sql_bigint_value_max,
        std::uint64_t{1} << 63,
        (std::numeric_limits<std::uint64_t>::max)(),
    };

    for (const std::uint64_t identity : identities) {
        pipeline_operational_stats aggregate{};
        pipeline_operational_stats publication{};
        publication.cpu_prepared_key_set_checksum = identity;
        publication.cpu_prepared_key_set_checksum_available = true;
        publication.cpu_prepared_build_set_checksum = identity;
        publication.cpu_prepared_build_set_checksum_available = true;
        aggregate.merge(publication);

        REQUIRE(aggregate.cpu_prepared_key_set_checksum_available);
        REQUIRE(aggregate.cpu_prepared_key_set_checksum == identity);
        REQUIRE(aggregate.cpu_prepared_build_set_checksum_available);
        REQUIRE(aggregate.cpu_prepared_build_set_checksum == identity);
        REQUIRE_FALSE(aggregate.has_operational_identity_conflict);
        REQUIRE_FALSE(aggregate.are_stats_saturated);
    }

    pipeline_operational_stats duplicate{};
    pipeline_operational_stats first_publication{};
    first_publication.cpu_prepared_key_set_checksum = std::uint64_t{1} << 63;
    first_publication.cpu_prepared_key_set_checksum_available = true;
    duplicate.merge(first_publication);
    duplicate.merge(first_publication);
    REQUIRE(duplicate.has_operational_identity_conflict);
    REQUIRE(duplicate.cpu_prepared_key_set_checksum == (std::uint64_t{1} << 63));
    REQUIRE_FALSE(duplicate.are_stats_saturated);

    pipeline_operational_stats mismatch{};
    pipeline_operational_stats low{};
    low.cpu_prepared_build_set_checksum = 7;
    low.cpu_prepared_build_set_checksum_available = true;
    pipeline_operational_stats high{};
    high.cpu_prepared_build_set_checksum =
        (std::numeric_limits<std::uint64_t>::max)();
    high.cpu_prepared_build_set_checksum_available = true;
    mismatch.merge(low);
    mismatch.merge(high);
    REQUIRE(mismatch.has_operational_identity_conflict);
    REQUIRE(mismatch.cpu_prepared_build_set_checksum == 7);
    REQUIRE_FALSE(mismatch.are_stats_saturated);
}

TEST_CASE("job validation enforces the exclusive backend/source matrix",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;

    const auto simd_recomputed = make_attested_job(
        executed_backend::simd_cpu, target_index_source::recomputed);
    const auto gpu_persisted = make_attested_job(
        executed_backend::vulkan_gpu, target_index_source::persisted);
    REQUIRE(is_job_execution_stats_valid(simd_recomputed));
    REQUIRE(is_job_execution_stats_valid(gpu_persisted));

    auto wrong_engine = simd_recomputed;
    auto& query_sketch =
        wrong_engine.stages.coverage[stage_index(pipeline_stage::query_sketch)];
    query_sketch.executed_jobs[engine_index(execution_engine::simd_cpu)] = 0;
    query_sketch.executed_jobs[engine_index(execution_engine::scalar_cpu)] = 1;
    REQUIRE_FALSE(is_job_execution_stats_valid(wrong_engine));

    auto fallback = gpu_persisted;
    fallback.stages.coverage[stage_index(pipeline_stage::chain_score)].fallback_jobs = 1;
    REQUIRE_FALSE(is_job_execution_stats_valid(fallback));

    auto forbidden_stage = make_attested_job(
        executed_backend::simd_cpu, target_index_source::persisted);
    auto& rebuilt_target =
        forbidden_stage.stages.coverage[stage_index(pipeline_stage::target_sketch)];
    rebuilt_target.eligible_jobs = 1;
    rebuilt_target.executed_jobs[engine_index(execution_engine::simd_cpu)] = 1;
    REQUIRE_FALSE(is_job_execution_stats_valid(forbidden_stage));

    auto missing_identity = gpu_persisted;
    missing_identity.persisted_generation_id = 0;
    REQUIRE_FALSE(is_job_execution_stats_valid(missing_identity));

    auto repeated_required_stage = simd_recomputed;
    auto& repeated_query = repeated_required_stage.stages.coverage[
        stage_index(pipeline_stage::query_sketch)];
    repeated_query.eligible_jobs = 2;
    repeated_query.executed_jobs[engine_index(execution_engine::simd_cpu)] = 2;
    REQUIRE_FALSE(is_job_execution_stats_valid(repeated_required_stage));

    auto missing_required_stage = simd_recomputed;
    missing_required_stage.stages.coverage[
        stage_index(pipeline_stage::query_sketch)] = {};
    REQUIRE_FALSE(is_job_execution_stats_valid(missing_required_stage));

    auto entirely_unattributed = simd_recomputed;
    entirely_unattributed.stages = {};
    REQUIRE_FALSE(is_job_execution_stats_valid(entirely_unattributed));

    auto no_extension = simd_recomputed;
    no_extension.stages.coverage[stage_index(pipeline_stage::gap_extension)] = {};
    REQUIRE(is_job_execution_stats_valid(no_extension));
}

TEST_CASE("zero-match jobs retain front-end stats for every measured mode",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    REQUIRE(is_job_execution_stats_valid(make_zero_match_job(
        executed_backend::simd_cpu, target_index_source::recomputed)));
    REQUIRE(is_job_execution_stats_valid(make_zero_match_job(
        executed_backend::simd_cpu, target_index_source::persisted)));
    REQUIRE(is_job_execution_stats_valid(make_zero_match_job(
        executed_backend::vulkan_gpu, target_index_source::recomputed)));
    REQUIRE(is_job_execution_stats_valid(make_zero_match_job(
        executed_backend::vulkan_gpu, target_index_source::persisted)));
}

TEST_CASE("SQL-bound stats values reject max plus one without clamping",
          "[component:bioseq][layer:unit][feature:sql_api]") {
    using namespace bioseq::pipeline_stats;
    auto boundary = make_attested_job(
        executed_backend::vulkan_gpu, target_index_source::persisted);
    boundary.environment.effective_cpu_workers = k_sql_integer_value_max;
    boundary.environment.gpu_pipeline_cache_seed_size = k_sql_bigint_value_max;
    boundary.persisted_generation_id = k_sql_bigint_value_max;
    boundary.logical_target_row = k_sql_bigint_value_max;
    boundary.query_ordinal = k_sql_bigint_value_max;
    REQUIRE(is_job_execution_stats_valid(boundary));

    auto workers_overflow = boundary;
    workers_overflow.environment.effective_cpu_workers = k_sql_integer_value_max + 1;
    REQUIRE_FALSE(is_job_execution_stats_valid(workers_overflow));

    auto seed_size_overflow = boundary;
    seed_size_overflow.environment.gpu_pipeline_cache_seed_size =
        k_sql_bigint_value_max + 1;
    REQUIRE_FALSE(is_job_execution_stats_valid(seed_size_overflow));

    auto generation_overflow = boundary;
    generation_overflow.persisted_generation_id = k_sql_bigint_value_max + 1;
    REQUIRE_FALSE(is_job_execution_stats_valid(generation_overflow));

    auto target_row_overflow = boundary;
    target_row_overflow.logical_target_row = k_sql_bigint_value_max + 1;
    REQUIRE_FALSE(is_job_execution_stats_valid(target_row_overflow));

    auto query_ordinal_overflow = boundary;
    query_ordinal_overflow.query_ordinal = k_sql_bigint_value_max + 1;
    REQUIRE_FALSE(is_job_execution_stats_valid(query_ordinal_overflow));
}

TEST_CASE("GPU environment requires canonical identity and policy-hit pairs",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    constexpr auto constexpr_valid_four_byte = [] {
        std::array<char, 128> value{};
        value[0] = static_cast<char>(0xf0);
        value[1] = static_cast<char>(0x9f);
        value[2] = static_cast<char>(0x98);
        value[3] = static_cast<char>(0x80);
        return value;
    }();
    constexpr auto constexpr_invalid_surrogate = [] {
        std::array<char, 128> value{};
        value[0] = static_cast<char>(0xed);
        value[1] = static_cast<char>(0xa0);
        value[2] = static_cast<char>(0x80);
        return value;
    }();
    static_assert(is_canonical_nonempty_driver_identity(constexpr_valid_four_byte));
    static_assert(!is_canonical_nonempty_driver_identity(constexpr_invalid_surrogate));

    std::array<char, 128> valid_utf8{};
    valid_utf8[0] = static_cast<char>(0xc3);
    valid_utf8[1] = static_cast<char>(0xa9);
    REQUIRE(is_canonical_nonempty_driver_identity(valid_utf8));

    std::array<char, 128> valid_three_byte{};
    valid_three_byte[0] = static_cast<char>(0xe2);
    valid_three_byte[1] = static_cast<char>(0x82);
    valid_three_byte[2] = static_cast<char>(0xac);
    REQUIRE(is_canonical_nonempty_driver_identity(valid_three_byte));

    std::array<char, 128> valid_four_byte{};
    valid_four_byte[0] = static_cast<char>(0xf0);
    valid_four_byte[1] = static_cast<char>(0x9f);
    valid_four_byte[2] = static_cast<char>(0x98);
    valid_four_byte[3] = static_cast<char>(0x80);
    REQUIRE(is_canonical_nonempty_driver_identity(valid_four_byte));

    std::array<char, 128> stray_continuation{};
    stray_continuation[0] = static_cast<char>(0x80);
    REQUIRE_FALSE(is_canonical_nonempty_driver_identity(stray_continuation));

    std::array<char, 128> invalid_continuation{};
    invalid_continuation[0] = static_cast<char>(0xc3);
    invalid_continuation[1] = '(';
    REQUIRE_FALSE(is_canonical_nonempty_driver_identity(invalid_continuation));

    std::array<char, 128> overlong{};
    overlong[0] = static_cast<char>(0xc0);
    overlong[1] = static_cast<char>(0xaf);
    REQUIRE_FALSE(is_canonical_nonempty_driver_identity(overlong));

    std::array<char, 128> c1_overlong{};
    c1_overlong[0] = static_cast<char>(0xc1);
    c1_overlong[1] = static_cast<char>(0xbf);
    REQUIRE_FALSE(is_canonical_nonempty_driver_identity(c1_overlong));

    std::array<char, 128> e0_overlong{};
    e0_overlong[0] = static_cast<char>(0xe0);
    e0_overlong[1] = static_cast<char>(0x9f);
    e0_overlong[2] = static_cast<char>(0xbf);
    REQUIRE_FALSE(is_canonical_nonempty_driver_identity(e0_overlong));

    std::array<char, 128> surrogate{};
    surrogate[0] = static_cast<char>(0xed);
    surrogate[1] = static_cast<char>(0xa0);
    surrogate[2] = static_cast<char>(0x80);
    REQUIRE_FALSE(is_canonical_nonempty_driver_identity(surrogate));

    std::array<char, 128> above_unicode_max{};
    above_unicode_max[0] = static_cast<char>(0xf4);
    above_unicode_max[1] = static_cast<char>(0x90);
    above_unicode_max[2] = static_cast<char>(0x80);
    above_unicode_max[3] = static_cast<char>(0x80);
    REQUIRE_FALSE(is_canonical_nonempty_driver_identity(above_unicode_max));

    std::array<char, 128> f0_overlong{};
    f0_overlong[0] = static_cast<char>(0xf0);
    f0_overlong[1] = static_cast<char>(0x8f);
    f0_overlong[2] = static_cast<char>(0xbf);
    f0_overlong[3] = static_cast<char>(0xbf);
    REQUIRE_FALSE(is_canonical_nonempty_driver_identity(f0_overlong));

    std::array<char, 128> nonzero_padding{};
    nonzero_padding[0] = 'v';
    nonzero_padding[2] = 'x';
    REQUIRE_FALSE(is_canonical_nonempty_driver_identity(nonzero_padding));

    std::array<char, 128> unterminated{};
    unterminated.fill('x');
    REQUIRE_FALSE(is_canonical_nonempty_driver_identity(unterminated));

    auto immutable_without_seed = make_gpu_environment();
    immutable_without_seed.gpu_pipeline_cache_seed_size = 0;

    auto immutable_miss = make_gpu_environment();
    immutable_miss.gpu_pipeline_cache_hit = optional_observation_bool::false_value;

    auto isolated_hit = make_gpu_environment();
    isolated_hit.gpu_pipeline_cache_policy =
        pipeline_cache_policy::isolated_miss_diagnostic;

    auto zero_manifest = make_gpu_environment();
    zero_manifest.gpu_pipeline_cache_manifest_checksum = 0;

    auto zero_uuid = make_gpu_environment();
    zero_uuid.gpu_device_uuid = {};

    auto zero_seed_sha = make_gpu_environment();
    zero_seed_sha.gpu_pipeline_cache_seed_sha256 = {};

    auto not_applicable_policy = make_gpu_environment();
    not_applicable_policy.gpu_pipeline_cache_policy =
        pipeline_cache_policy::not_applicable;

    auto mixed_policy = make_gpu_environment();
    mixed_policy.gpu_pipeline_cache_policy =
        pipeline_cache_policy::mixed_invalid;

    auto immutable_unset_hit = make_gpu_environment();
    immutable_unset_hit.gpu_pipeline_cache_hit =
        optional_observation_bool::not_applicable;

    auto immutable_mixed_hit = make_gpu_environment();
    immutable_mixed_hit.gpu_pipeline_cache_hit =
        optional_observation_bool::mixed_invalid;

    auto isolated_unset_hit = make_gpu_environment();
    isolated_unset_hit.gpu_pipeline_cache_policy =
        pipeline_cache_policy::isolated_miss_diagnostic;
    isolated_unset_hit.gpu_pipeline_cache_hit =
        optional_observation_bool::not_applicable;

    auto isolated_mixed_hit = make_gpu_environment();
    isolated_mixed_hit.gpu_pipeline_cache_policy =
        pipeline_cache_policy::isolated_miss_diagnostic;
    isolated_mixed_hit.gpu_pipeline_cache_hit =
        optional_observation_bool::mixed_invalid;

    struct invalid_environment_case {
        std::string_view name;
        execution_environment_snapshot environment;
    };
    const auto invalid_environments = std::to_array<invalid_environment_case>({
        {"immutable without seed bytes", immutable_without_seed},
        {"immutable cache miss", immutable_miss},
        {"isolated diagnostic cache hit", isolated_hit},
        {"zero manifest checksum", zero_manifest},
        {"zero device UUID", zero_uuid},
        {"zero seed SHA", zero_seed_sha},
        {"not-applicable policy", not_applicable_policy},
        {"mixed policy", mixed_policy},
        {"immutable unset hit", immutable_unset_hit},
        {"immutable mixed hit", immutable_mixed_hit},
        {"isolated unset hit", isolated_unset_hit},
        {"isolated mixed hit", isolated_mixed_hit},
    });
    for (const auto& test : invalid_environments) {
        CAPTURE(test.name);
        REQUIRE_FALSE(is_environment_valid_for_backend(
            test.environment, executed_backend::vulkan_gpu));
    }

    REQUIRE(is_environment_valid_for_backend(
        make_gpu_environment(), executed_backend::vulkan_gpu));

    auto isolated_empty_seed = immutable_without_seed;
    isolated_empty_seed.gpu_pipeline_cache_policy =
        pipeline_cache_policy::isolated_miss_diagnostic;
    isolated_empty_seed.gpu_pipeline_cache_hit = optional_observation_bool::false_value;
    REQUIRE(is_environment_valid_for_backend(
        isolated_empty_seed, executed_backend::vulkan_gpu));
}

TEST_CASE("aggregate merge rejects persisted and environment identity drift",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    pipeline_execution_stats aggregate{};
    auto first = make_attested_job(
        executed_backend::vulkan_gpu, target_index_source::persisted);
    first.logical_target_row = 11;
    first.query_ordinal = 7;
    aggregate.merge_job(first);
    REQUIRE(aggregate.fully_attested_alignment_jobs == 1);
    REQUIRE_FALSE(aggregate.has_mixed_persisted_identity);

    auto changed_generation = first;
    changed_generation.persisted_generation_id = 18;
    changed_generation.logical_target_row = 12;
    aggregate.merge_job(changed_generation);
    REQUIRE(aggregate.has_mixed_persisted_identity);
    REQUIRE(aggregate.attempted_alignment_jobs == 2);
    REQUIRE(aggregate.fully_attested_alignment_jobs == 0);
    REQUIRE(aggregate.unattested_alignment_jobs == 2);
    REQUIRE(aggregate.first_unattested_target_row == 11);
    REQUIRE(aggregate.first_unattested_query_ordinal == 7);

    auto changed_environment = first;
    changed_environment.environment.effective_cpu_workers = 3;
    changed_environment.logical_target_row = 13;
    aggregate.merge_job(changed_environment);
    REQUIRE(aggregate.environment.is_mixed_invalid);
    REQUIRE(aggregate.attempted_alignment_jobs == 3);
    REQUIRE(aggregate.fully_attested_alignment_jobs == 0);
    REQUIRE(aggregate.unattested_alignment_jobs == 3);
    REQUIRE(aggregate.first_unattested_target_row == 11);
    REQUIRE(aggregate.first_unattested_query_ordinal == 7);
}
