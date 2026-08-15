/**
 * @file statistics_receipts_test.cpp
 * @brief Exercises pipeline statistics and receipts.
 */
#include <tests/support/common.hpp>

#include <catch2/matchers/catch_matchers.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

import bioseq.tests.unit.pipeline.pipe_support;

using namespace bioseq::tests::pipeline::pipe_support;

TEST_CASE("canonical pipeline stages have stable ordinals and names",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using bioseq::pipeline_stats::pipeline_stage_name;
    static_assert(static_cast<std::uint8_t>(pipeline_stage::query_sketch) == 0);
    static_assert(static_cast<std::uint8_t>(pipeline_stage::target_index_build) == 2);
    static_assert(static_cast<std::uint8_t>(pipeline_stage::chain_backtrack) == 8);
    static_assert(static_cast<std::uint8_t>(pipeline_stage::target_window_io) == 14);
    static_assert(static_cast<std::uint8_t>(pipeline_stage::right_extension) == 19);
    static_assert(static_cast<std::uint8_t>(pipeline_stage::count) == 20);
    static_assert(bioseq::pipeline_stats::k_pipeline_stage_count == 20);
    static_assert(std::is_trivially_copyable_v<
        bioseq::pipeline_stats::pipeline_execution_stats>);
    static_assert(std::is_standard_layout_v<
        bioseq::pipeline_stats::pipeline_execution_stats>);

    constexpr std::array<std::string_view, 20> expected_names{
        "query_sketch",
        "target_sketch",
        "target_index_build",
        "persisted_index_decode",
        "partition_selection",
        "device_index_upload",
        "seed_lookup",
        "chain_score",
        "chain_backtrack",
        "region_generation",
        "primary_secondary_reduction",
        "divergence_estimation",
        "mapq",
        "target_window_plan",
        "target_window_io",
        "target_window_upload",
        "cigar_job_generation",
        "left_extension",
        "gap_extension",
        "right_extension",
    };
    for (std::size_t i = 0; i < expected_names.size(); ++i) {
        REQUIRE(pipeline_stage_name(static_cast<pipeline_stage>(i)) == expected_names[i]);
    }
}

TEST_CASE("unbound pipeline description owns exact Highway kernels by stage",
          "[component:bioseq][layer:unit][feature:planner][resource:highway]") {
    using bioseq::simd::base_alignment_algorithm;
    using bioseq::simd::chaining_algorithm;
    using bioseq::simd::minimap2_parameters;
    using bioseq::simd::minimizer_algorithm;
    using bioseq::simd::pipe::query_sketch_route;
    using bioseq::simd_dispatch::highway_kernel;
    using bioseq::simd_dispatch::highway_kernel_mask_for;

    minimap2_parameters params{};
    const auto& table =
        ::bioseq::simd_dispatch::highway_test_kernel_table();
    const auto* alphabet_identity = table.is_complete()
        ? table.resolve_alphabet_identity<::seq::dna4>() : nullptr;
    REQUIRE(alphabet_identity != nullptr);
    params.alphabet_code = alphabet_identity->alphabet_code;
    params.minimizer_algorithm =
        minimizer_algorithm::bounded_nthash_simd;
    params.chaining_algorithm = chaining_algorithm::simd_minimap2;
    params.prefilter_enabled = true;
    params.prefilter_backend =
        bioseq::prefilter::prefilter_backend::simd;
    params.base_alignment_algorithm =
        base_alignment_algorithm::ksw2_batched;
    params.do_cigar = true;
    REQUIRE(params.highway_kernels == nullptr);

    const auto description = bioseq::simd::pipe::describe_pipeline_for<
        ::seq::dna4>(
        params, params.alphabet_code,
        query_sketch_route::recompute_capable);
    REQUIRE(bioseq::simd::pipe::query_sketch_stage_owner_count(
                description) == 1u);

    const auto stage_kernels = [&description](const std::string_view name) {
        const auto found = std::ranges::find(
            description.stages, name,
            &bioseq::simd::pipe::stage_description::stage_name);
        REQUIRE(found != description.stages.end());
        REQUIRE(found->algorithm.has_value());
        return found->algorithm->highway_kernels;
    };
    REQUIRE(stage_kernels("QuerySketchStage") == highway_kernel_mask_for(
        highway_kernel::minimizer_bounded_nthash));
    REQUIRE(stage_kernels("chain_stage") == highway_kernel_mask_for(
        highway_kernel::chaining_chain_anchors_simd));
    REQUIRE(stage_kernels("prefilter_stage") == highway_kernel_mask_for(
        highway_kernel::prefilter_sneaky_snake_simd_batch));
    auto alignment_kernels = highway_kernel_mask_for(
        highway_kernel::alignment_ksw2_batched);
    alignment_kernels |= highway_kernel_mask_for(
        highway_kernel::alignment_ksw2);
    REQUIRE(stage_kernels("align_stage") == alignment_kernels);
}

TEST_CASE("all-GPU bounded CIGAR description advertises no Highway kernel",
          "[component:bioseq][layer:unit][feature:planner][resource:highway]") {
    using bioseq::simd::base_alignment_algorithm;
    using bioseq::simd::chaining_algorithm;
    using bioseq::simd::minimap2_parameters;
    using bioseq::simd::minimizer_algorithm;
    using bioseq::simd::pipe::query_sketch_route;

    minimap2_parameters params{};
    const auto& table =
        ::bioseq::simd_dispatch::highway_test_kernel_table();
    const auto* alphabet_identity = table.is_complete()
        ? table.resolve_alphabet_identity<::seq::dna4>() : nullptr;
    REQUIRE(alphabet_identity != nullptr);
    params.alphabet_code = alphabet_identity->alphabet_code;
    params.kmer_size = 15;
    params.window_size = 10;
    params.minimizer_algorithm =
        minimizer_algorithm::vulkan_minimap_integer;
    params.chaining_algorithm = chaining_algorithm::vulkan_tiled_dynamic_programming;
    params.base_alignment_algorithm =
        base_alignment_algorithm::block_aligner;
    params.vulkan_enabled = true;
    params.vulkan_seed_hits_enabled = true;
    params.vulkan_fused_frontend_enabled = true;
    params.vulkan_gpu_index_build_enabled = true;
    params.vulkan_extend_enabled = true;
    params.do_cigar = true;
    params.compute_divergence = false;
    params.compute_mapq = false;
    params.compute_edit_distance = false;
    params.exact_edit_filter_enabled = false;
    params.prefilter_enabled = false;
    params.max_occ = 500;

    REQUIRE(bioseq::simd::pipe::complete_all_gpu_tuple_selected(params));
    REQUIRE(params.highway_kernels == nullptr);

    const auto description = bioseq::simd::pipe::describe_pipeline_for<
        ::seq::dna4>(
        params, params.alphabet_code,
        query_sketch_route::recompute_capable);
    const auto align_stage = std::ranges::find(
        description.stages, "align_stage",
        &bioseq::simd::pipe::stage_description::stage_name);
    REQUIRE(align_stage != description.stages.end());
    REQUIRE(align_stage->algorithm.has_value());
    REQUIRE(align_stage->algorithm->highway_kernels.none());
    REQUIRE(std::ranges::all_of(
        description.stages,
        [](const bioseq::simd::pipe::stage_description& stage) {
            return !stage.algorithm.has_value()
                || stage.algorithm->highway_kernels.none();
        }));
}

TEST_CASE("persisted grouped-SoA publication rejects burned fully-consumed stats",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    using counts_type = persisted_grouped_soa_coordinate_publication_stats;

    constexpr counts_type accepted{
        .expected_jobs = 2,
        .validated_jobs = 2,
        .consumed_jobs = 2,
        .accepted_jobs = 2,
        .burned_jobs = 0,
        .pending_jobs = 0,
        .staged_jobs = 2,
    };
    static_assert(is_persisted_grouped_soa_coordinate_publication_consistent(
        accepted));
    REQUIRE(is_persisted_grouped_soa_coordinate_publication_consistent(accepted));

    /* This fail-open fixture has consume() mark both records
     * consumed even though the second expectation mismatched and burned. */
    constexpr counts_type burned_but_fully_consumed{
        .expected_jobs = 2,
        .validated_jobs = 2,
        .consumed_jobs = 2,
        .accepted_jobs = 1,
        .burned_jobs = 1,
        .pending_jobs = 0,
        .staged_jobs = 1,
    };
    static_assert(!is_persisted_grouped_soa_coordinate_publication_consistent(
        burned_but_fully_consumed));
    REQUIRE_FALSE(is_persisted_grouped_soa_coordinate_publication_consistent(
        burned_but_fully_consumed));

    const auto rejected = [](counts_type counts) {
        REQUIRE_FALSE(
            is_persisted_grouped_soa_coordinate_publication_consistent(counts));
    };
    auto malformed = accepted;
    malformed.expected_jobs = 0;
    rejected(malformed);
    malformed = accepted;
    malformed.validated_jobs = 1;
    rejected(malformed);
    malformed = accepted;
    malformed.consumed_jobs = 1;
    rejected(malformed);
    malformed = accepted;
    malformed.accepted_jobs = 1;
    rejected(malformed);
    malformed = accepted;
    malformed.burned_jobs = 1;
    rejected(malformed);
    malformed = accepted;
    malformed.pending_jobs = 1;
    rejected(malformed);
    malformed = accepted;
    malformed.staged_jobs = 1;
    rejected(malformed);
}

TEST_CASE("persisted grouped-SoA environment binding maps without inference",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    namespace gv = bioseq::gpu::vulkan;
    using namespace bioseq::pipeline_stats;
    gv::runtime_gpu_execution_environment_snapshot binding{};
    binding.gpu_environment_present = true;
    binding.gpu_device_uuid[0] = 0x61u;
    binding.gpu_driver_identity[0] = 'v';
    binding.gpu_driver_identity[1] = 'k';
    binding.cache_policy = gv::runtime_pipeline_cache_policy::
        immutable_preseed_all_hit;
    binding.seed_sha256[0] = 0xa5u;
    binding.seed_size = 4096u;
    binding.manifest_checksum = 0x12345678u;
    binding.pipeline_cache_all_hit = true;
    REQUIRE(binding.is_internally_consistent());
    static_assert(noexcept(make_persisted_grouped_soa_gpu_environment_snapshot(
        binding, 4u)));

    const auto mapped = make_persisted_grouped_soa_gpu_environment_snapshot(
        binding, 4u);
    REQUIRE(mapped.has_value());
    REQUIRE(mapped->effective_cpu_workers == 4u);
    REQUIRE(mapped->is_gpu_environment_available);
    REQUIRE(mapped->gpu_device_uuid == binding.gpu_device_uuid);
    REQUIRE(mapped->gpu_driver_identity == binding.gpu_driver_identity);
    REQUIRE(mapped->gpu_pipeline_cache_policy
            == pipeline_cache_policy::immutable_preseed_all_hit);
    REQUIRE(mapped->gpu_pipeline_cache_seed_sha256 == binding.seed_sha256);
    REQUIRE(mapped->gpu_pipeline_cache_seed_size == binding.seed_size);
    REQUIRE(mapped->gpu_pipeline_cache_manifest_checksum
            == binding.manifest_checksum);
    REQUIRE(mapped->gpu_pipeline_cache_hit == optional_observation_bool::true_value);
    REQUIRE(is_environment_valid_for_backend(
        *mapped, executed_backend::vulkan_gpu));
    REQUIRE(is_persisted_grouped_soa_gpu_environment_consistent(
        binding, binding));
    auto drifted_binding = binding;
    drifted_binding.gpu_driver_identity[1] = 'x';
    REQUIRE_FALSE(is_persisted_grouped_soa_gpu_environment_consistent(
        drifted_binding, binding));

    REQUIRE_FALSE(make_persisted_grouped_soa_gpu_environment_snapshot(
        binding, 0u).has_value());
    REQUIRE_FALSE(make_persisted_grouped_soa_gpu_environment_snapshot(
        binding,
        static_cast<std::size_t>(k_sql_integer_value_max) + 1u).has_value());
    auto invalid = binding;
    invalid.gpu_device_uuid = {};
    REQUIRE_FALSE(make_persisted_grouped_soa_gpu_environment_snapshot(
        invalid, 4u).has_value());
    invalid = binding;
    invalid.cache_policy = gv::runtime_pipeline_cache_policy::invalid;
    REQUIRE_FALSE(make_persisted_grouped_soa_gpu_environment_snapshot(
        invalid, 4u).has_value());
    invalid = binding;
    invalid.pipeline_cache_all_hit = false;
    REQUIRE_FALSE(make_persisted_grouped_soa_gpu_environment_snapshot(
        invalid, 4u).has_value());
    invalid = binding;
    invalid.seed_size = k_sql_bigint_value_max + 1u;
    REQUIRE_FALSE(make_persisted_grouped_soa_gpu_environment_snapshot(
        invalid, 4u).has_value());

    gv::runtime_gpu_cache_metrics_audit cache_metrics{
        .token_identity = 0x999u,
        .gpu_pipeline_cache_loads = 1u,
        .gpu_pipeline_cache_seed_bytes = binding.seed_size,
        .gpu_pipeline_cache_load_ns = 37u,
        .gpu_pipeline_creates = 11u,
        .gpu_pipeline_creation_ns = 91u,
        .saturated = false,
    };
    static_assert(noexcept(
        make_persisted_grouped_soa_gpu_cache_metrics_delta(cache_metrics)));
    const auto cache_delta =
        make_persisted_grouped_soa_gpu_cache_metrics_delta(cache_metrics);
    REQUIRE(cache_delta.has_value());
    REQUIRE(cache_delta->gpu_pipeline_cache_loads == 1u);
    REQUIRE(cache_delta->gpu_pipeline_cache_seed_bytes == binding.seed_size);
    REQUIRE(cache_delta->gpu_pipeline_cache_load_ns == 37u);
    REQUIRE(cache_delta->gpu_pipeline_creates == 11u);
    REQUIRE(cache_delta->gpu_pipeline_creation_ns == 91u);
    auto cache_metrics_preview = cache_metrics;
    cache_metrics_preview.gpu_pipeline_cache_load_ns = 50u;
    cache_metrics_preview.gpu_pipeline_creates = 15u;
    cache_metrics_preview.gpu_pipeline_creation_ns = 120u;
    REQUIRE(is_persisted_grouped_soa_gpu_cache_metrics_bounded_by(
        cache_metrics, cache_metrics_preview));
    auto out_of_bounds = cache_metrics;
    out_of_bounds.gpu_pipeline_creates = 16u;
    REQUIRE_FALSE(is_persisted_grouped_soa_gpu_cache_metrics_bounded_by(
        out_of_bounds, cache_metrics_preview));
    auto wrong_token = cache_metrics;
    wrong_token.token_identity += 1u;
    REQUIRE_FALSE(is_persisted_grouped_soa_gpu_cache_metrics_bounded_by(
        wrong_token, cache_metrics_preview));
    const gv::runtime_gpu_cache_metrics_audit already_reported{
        .token_identity = 0x1000u,
    };
    REQUIRE(already_reported.is_internally_consistent());
    const auto zero_delta =
        make_persisted_grouped_soa_gpu_cache_metrics_delta(already_reported);
    REQUIRE(zero_delta.has_value());
    REQUIRE(zero_delta->gpu_pipeline_cache_loads == 0u);
    REQUIRE(zero_delta->gpu_pipeline_creates == 0u);
    const gv::runtime_gpu_cache_metrics_audit zero_for_same_operation{
        .token_identity = cache_metrics.token_identity,
    };
    REQUIRE(is_persisted_grouped_soa_gpu_cache_metrics_bounded_by(
        zero_for_same_operation, cache_metrics_preview));
    cache_metrics.saturated = true;
    REQUIRE_FALSE(make_persisted_grouped_soa_gpu_cache_metrics_delta(
        cache_metrics).has_value());
    cache_metrics.saturated = false;
    cache_metrics.token_identity = 0u;
    REQUIRE_FALSE(make_persisted_grouped_soa_gpu_cache_metrics_delta(
        cache_metrics).has_value());
}

TEST_CASE("persisted grouped-SoA prospective publication is exact across waves",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    const auto make_wave = [](const std::size_t jobs,
                              const std::uint64_t first_query) {
        pipeline_execution_stats wave{};
        for (std::size_t i = 0; i < jobs; ++i) {
            auto job = make_attested_job(
                executed_backend::vulkan_gpu,
                target_index_source::persisted);
            job.logical_target_row = 73;
            job.query_ordinal = first_query + i;
            wave.merge_job(job);
        }
        return wave;
    };

    for (const std::size_t jobs : {std::size_t{1}, std::size_t{8}}) {
        CAPTURE(jobs);
        const pipeline_execution_stats before{};
        const auto staged = make_wave(jobs, 100);
        auto prospective = before;
        prospective.merge(staged);
        REQUIRE(is_persisted_grouped_soa_coordinate_prospective_stats_consistent(
            before,
            staged,
            prospective,
            static_cast<std::uint64_t>(jobs)));
    }

    auto staged_with_cache_metrics = make_wave(1, 150);
    staged_with_cache_metrics.operational.gpu_pipeline_cache_loads = 1u;
    staged_with_cache_metrics.operational.gpu_pipeline_cache_seed_bytes = 4096u;
    staged_with_cache_metrics.operational.gpu_pipeline_cache_load_ns = 37u;
    staged_with_cache_metrics.operational.gpu_pipeline_creates = 7u;
    staged_with_cache_metrics.operational.gpu_pipeline_creation_ns = 91u;
    pipeline_execution_stats cache_metrics_prospective{};
    cache_metrics_prospective.merge(staged_with_cache_metrics);
    REQUIRE(is_persisted_grouped_soa_coordinate_prospective_stats_consistent(
        pipeline_execution_stats{},
        staged_with_cache_metrics,
        cache_metrics_prospective,
        1u));

    const auto before = make_wave(1, 200);
    const auto staged = make_wave(1, 201);
    auto prospective = before;
    prospective.merge(staged);
    REQUIRE(is_persisted_grouped_soa_coordinate_prospective_stats_consistent(
        before, staged, prospective, 1));

    auto drifted = staged;
    drifted.environment.effective_cpu_workers += 1;
    auto drifted_prospective = before;
    drifted_prospective.merge(drifted);
    REQUIRE_FALSE(is_persisted_grouped_soa_coordinate_prospective_stats_consistent(
        before, drifted, drifted_prospective, 1));

    auto wrong_generation = staged;
    wrong_generation.persisted_generation_id += 1;
    auto generation_prospective = before;
    generation_prospective.merge(wrong_generation);
    REQUIRE_FALSE(is_persisted_grouped_soa_coordinate_prospective_stats_consistent(
        before, wrong_generation, generation_prospective, 1));

    auto unattested = staged;
    unattested.fully_attested_alignment_jobs = 0;
    unattested.unattested_alignment_jobs = 1;
    REQUIRE_FALSE(is_persisted_grouped_soa_coordinate_prospective_stats_consistent(
        before, unattested, prospective, 1));
    REQUIRE_FALSE(is_persisted_grouped_soa_coordinate_prospective_stats_consistent(
        before, staged, prospective, 0));
}

TEST_CASE("persisted GPU SQL accumulation is admitted, causal, and immutable across waves",
          "[component:bioseq][layer:unit][feature:sql_api]") {
    using namespace bioseq::pipeline_stats;
    static_assert(!std::is_copy_constructible_v<
        persisted_gpu_operation_accumulator>);
    static_assert(std::is_nothrow_move_constructible_v<
        persisted_gpu_operation_accumulator>);
    static_assert(noexcept(std::declval<
        persisted_gpu_operation_chunk&>().publish_cache_metrics(
            std::declval<const persisted_gpu_cache_metrics&>())));

    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    const std::uint64_t baseline = admission.reserved_bytes();
    struct restore_cap {
        bioseq::parallelism::memory_admission_controller& admission;
        std::uint64_t previous = 0u;
        ~restore_cap() { admission.set_index_memory_cap_bytes(previous); }
    } restore{admission, previous_cap};
    admission.set_index_memory_cap_bytes(std::max<std::uint64_t>(
        previous_cap, baseline + (std::uint64_t{1} << 23u)));
    constexpr std::uint64_t two_job_chunk_retained_bytes =
        sizeof(persisted_gpu_operation_chunk)
        + 2u * sizeof(persisted_gpu_job_stats);

    REQUIRE(persisted_gpu_operation_chunk::prepare(0u) == nullptr);
    REQUIRE(persisted_gpu_operation_chunk::prepare(
        static_cast<std::size_t>(
            k_persisted_gpu_operation_capacity + 1u)) == nullptr);

    const auto make_chunk = [](
            const std::uint64_t shape_operation_identity,
            const std::uint64_t query_operation_identity,
            const std::uint64_t first_logical_query) {
        auto chunk = persisted_gpu_operation_chunk::prepare(2u);
        if (chunk == nullptr) return chunk;
        persisted_gpu_environment_snapshot environment{};
        environment.device_uuid[0] = 0x41u;
        environment.api_version = 0x00402000u;
        environment.driver_version = 0x01020304u;
        environment.vendor_id = 0x106bu;
        environment.device_id = 0x0011u;
        environment.pipeline_cache_uuid[0] = 0x52u;
        environment.seed_sha256[0] = 0x63u;
        environment.seed_size = 4096u;
        environment.manifest_checksum = 0x7777u;
        persisted_gpu_cache_validation_result qualification{};
        qualification.qualification_wire_revision = 2u;
        qualification.identity = 0x7171u;
        qualification.archive_count = 2u;
        qualification.archive_bytes = 4096u;
        qualification.archive_checksum = 0x7272u;
        qualification.receipt_count = 2u;
        qualification.hash_passes = 1u;
        qualification.hashed_bytes = 4096u;
        qualification.private_files_created = 2u;
        qualification.private_files_unlinked = 2u;
        qualification.helper_wire_revision = 2u;
        constexpr std::string_view helper_build =
            "bioseq_gpu_archive_builder";
        std::copy(
            helper_build.begin(), helper_build.end(),
            qualification.helper_build_identity.begin());
        qualification.helper_build_identity_size =
            static_cast<std::uint32_t>(helper_build.size());
        constexpr std::string_view helper_source = "installed-sibling";
        std::copy(
            helper_source.begin(), helper_source.end(),
            qualification.helper_source.begin());
        qualification.helper_source_size =
            static_cast<std::uint32_t>(helper_source.size());
        qualification.helper_executable_size = 8192u;
        qualification.helper_executable_sha256[0] = 0x73u;
        qualification.helper_metal_registry_id = 0x7474u;
        qualification.device_uuid = environment.device_uuid;
        qualification.api_version = environment.api_version;
        qualification.driver_version = environment.driver_version;
        qualification.vendor_id = environment.vendor_id;
        qualification.device_id = environment.device_id;
        qualification.pipeline_cache_uuid =
            environment.pipeline_cache_uuid;
        qualification.seed_receipt_identity = 0x8888u;
        qualification.seed_sha256 = environment.seed_sha256;
        qualification.seed_size = environment.seed_size;
        qualification.manifest_checksum = environment.manifest_checksum;
        REQUIRE(qualification.is_internally_consistent());
        chunk->operation() = persisted_gpu_operation_stats{
            .operation_identity = shape_operation_identity,
            .publication = {
                .expected_jobs = 2u,
                .validated_jobs = 2u,
                .consumed_jobs = 2u,
                .accepted_jobs = 2u,
                .burned_jobs = 0u,
                .pending_jobs = 0u,
                .staged_jobs = 2u,
            },
            .published_jobs = 2u,
            .publication_commits = 1u,
            .query_pipeline_receipt_count = 3u,
            .shape_pipeline_receipt_count = 7u,
            .submission_count = 6u,
            .query_pipeline_operation_identity = query_operation_identity,
            .shape_pipeline_operation_identity = shape_operation_identity,
            .query_submission_ordinals = {1u, 2u},
            .seed_receipt_identity = 0x8888u,
            .generation_id = 77u,
            .source_identity = 0x9999u,
            .cold_load_identity = 0xaaaau,
            .environment = environment,
            .cache_qualification = qualification,
            .cache_metrics = {
                .token_identity = shape_operation_identity,
                .pipeline_cache_loads = 1u,
                .pipeline_cache_seed_bytes = 4096u,
                .pipeline_cache_load_ns = 31u,
                .pipeline_creates = 4u,
                .pipeline_creation_ns = 47u,
            },
        };
        for (std::size_t i = 0u; i < chunk->jobs().size(); ++i) {
            auto& job = chunk->jobs()[i];
            job = persisted_gpu_job_stats{
                .logical_target_row = 9u,
                .logical_query_ordinal = first_logical_query + i,
                .query_index = static_cast<std::uint32_t>(i),
                .query_length = 1000u,
                .target_length = 2000u,
                .query_minimizer_count = 10u,
                .query_tile_count = 1u,
                .query_count_dispatches = 1u,
                .query_scan_dispatches = 3u,
                .query_emit_dispatches = 1u,
                .query_completed_roundtrips = 2u,
                .direct_count_dispatches = 1u,
                .direct_scan_dispatches = 3u,
                .anchor_count = 12u,
                .direct_emit_dispatches = 1u,
                .sort_operations = 1u,
                .range_build_dispatches = 1u,
                .range_count = 2u,
                .chain_dispatches = 1u,
                .chain_backtrack_dispatches = 1u,
                .coordinate_dispatches = 1u,
                .submissions = {{
                    persisted_gpu_submission_stats{
                        shape_operation_identity, 10u, 0x5010u},
                    persisted_gpu_submission_stats{
                        shape_operation_identity, 11u, 0x5011u},
                    persisted_gpu_submission_stats{
                        shape_operation_identity, 12u, 0x5012u},
                    persisted_gpu_submission_stats{
                        shape_operation_identity, 13u, 0x5013u},
                }},
                .count_status_checks = 1u,
                .emit_status_checks = 1u,
                .status_or = 0u,
                .result_readbacks = 1u,
                .coordinate_vote_dispatches = 0u,
                .cpu_or_simd_biological_calls = 0u,
                .recompute_calls = 0u,
                .fallback_calls = 0u,
                .coordinate = {
                    .query_start = 1u,
                    .query_end = 100u,
                    .target_start = 200u + i,
                    .target_end = 299u + i,
                    .match_length = 90u,
                    .block_length = 99u,
                    .chain_score = 42,
                    .seed_count = 10,
                    .is_reverse = false,
                    .is_valid = true,
                },
            };
        }
        return chunk;
    };
    const auto publish_final_cache = [](
            persisted_gpu_operation_chunk& chunk) {
        const persisted_gpu_cache_metrics final_metrics{
            .token_identity = chunk.operation().operation_identity,
        };
        REQUIRE(final_metrics.is_internally_consistent());
        chunk.publish_cache_metrics(final_metrics);
        REQUIRE(chunk.has_published_cache_metrics());
        REQUIRE(chunk.is_sealed());
    };

    {
        auto inconsistent_wait = make_chunk(0x1000u, 0x2000u, 30u);
        REQUIRE(inconsistent_wait != nullptr);
        inconsistent_wait->jobs()[1].submissions[0].submit_wait_identity += 1u;
        REQUIRE_FALSE(inconsistent_wait->seal());

        auto reused_wait = make_chunk(0x1000u, 0x2000u, 30u);
        REQUIRE(reused_wait != nullptr);
        reused_wait->jobs()[1].submissions[0].ordinal += 100u;
        REQUIRE_FALSE(reused_wait->seal());

        auto wrong_shape_domain = make_chunk(0x1000u, 0x2000u, 30u);
        REQUIRE(wrong_shape_domain != nullptr);
        wrong_shape_domain->operation().shape_pipeline_operation_identity =
            0x3000u;
        REQUIRE_FALSE(wrong_shape_domain->seal());
    }
    REQUIRE(admission.reserved_bytes() == baseline);

    const auto require_qualification_rejection = [&make_chunk](
            const auto& mutate) {
        auto chunk = make_chunk(0x1050u, 0x2050u, 35u);
        REQUIRE(chunk != nullptr);
        mutate(chunk->operation().cache_qualification);
        REQUIRE_FALSE(chunk->seal());
    };
    require_qualification_rejection([](auto& qualification) {
        qualification.hash_passes = 2u;
    });
    require_qualification_rejection([](auto& qualification) {
        --qualification.hashed_bytes;
    });
    require_qualification_rejection([](auto& qualification) {
        --qualification.private_files_unlinked;
    });
    require_qualification_rejection([](auto& qualification) {
        qualification.live_private_paths = 1u;
    });
    require_qualification_rejection([](auto& qualification) {
        qualification.pipeline_filesystem_hash_passes = 1u;
    });
    require_qualification_rejection([](auto& qualification) {
        qualification.helper_source[0] = 'x';
    });
    require_qualification_rejection([](auto& qualification) {
        qualification.helper_build_identity[0] = 'x';
    });
    require_qualification_rejection([](auto& qualification) {
        qualification.helper_executable_sha256 = {};
    });
    require_qualification_rejection([](auto& qualification) {
        qualification.device_uuid[0] ^= 1u;
    });
    require_qualification_rejection([](auto& qualification) {
        ++qualification.seed_receipt_identity;
    });
    require_qualification_rejection([](auto& qualification) {
        qualification.is_saturated = true;
    });
    REQUIRE(admission.reserved_bytes() == baseline);

    {
        auto zero_numeric_device_ids = make_chunk(0x1060u, 0x2060u, 36u);
        REQUIRE(zero_numeric_device_ids != nullptr);
        REQUIRE(admission.reserved_bytes()
                == baseline + two_job_chunk_retained_bytes);
        zero_numeric_device_ids->operation().environment.vendor_id = 0u;
        zero_numeric_device_ids->operation().environment.device_id = 0u;
        zero_numeric_device_ids->operation().cache_qualification.vendor_id =
            0u;
        zero_numeric_device_ids->operation().cache_qualification.device_id =
            0u;
        REQUIRE(zero_numeric_device_ids->seal());
    }
    REQUIRE(admission.reserved_bytes() == baseline);

    {
        persisted_gpu_operation_accumulator state{};
        auto first = make_chunk(0x1000u, 0x2000u, 30u);
        REQUIRE(first != nullptr);
        REQUIRE(first->seal());
        auto first_append = state.prepare_append(first);
        REQUIRE(first_append.is_valid());
        publish_final_cache(*first);
        state.commit_append(std::move(first_append), std::move(first));

        /* A fully drained rescan may repeat the logical rows in a new
         * structured operation domain. */
        auto second = make_chunk(0x1001u, 0x2001u, 30u);
        REQUIRE(second != nullptr);
        REQUIRE(second->seal());
        auto second_append = state.prepare_append(second);
        REQUIRE(second_append.is_valid());
        publish_final_cache(*second);
        state.commit_append(std::move(second_append), std::move(second));
        state.freeze();
        REQUIRE(state.is_complete());
        REQUIRE(state.is_internally_consistent());
        REQUIRE(state.operation_count() == 2u);
        REQUIRE(state.job_count() == 4u);

        persisted_gpu_operation_accumulator frozen{std::move(state)};
        REQUIRE(frozen.is_complete());
        std::vector<std::uint64_t> first_read;
        std::vector<std::uint64_t> second_read;
        for (const persisted_gpu_operation_chunk* chunk = frozen.head();
             chunk != nullptr; chunk = chunk->next()) {
            first_read.push_back(chunk->operation().operation_identity);
        }
        for (const persisted_gpu_operation_chunk* chunk = frozen.head();
             chunk != nullptr; chunk = chunk->next()) {
            second_read.push_back(chunk->operation().operation_identity);
        }
        REQUIRE(first_read == std::vector<std::uint64_t>{0x1000u, 0x1001u});
        REQUIRE(second_read == first_read);
    }
    REQUIRE(admission.reserved_bytes() == baseline);

    const auto require_cross_operation_rejection = [&make_chunk,
            &publish_final_cache](const auto& mutate) {
        persisted_gpu_operation_accumulator state{};
        auto first = make_chunk(0x1100u, 0x2100u, 40u);
        REQUIRE(first != nullptr);
        REQUIRE(first->seal());
        auto first_append = state.prepare_append(first);
        REQUIRE(first_append.is_valid());
        publish_final_cache(*first);
        state.commit_append(std::move(first_append), std::move(first));

        auto second = make_chunk(0x1101u, 0x2101u, 41u);
        REQUIRE(second != nullptr);
        mutate(second->operation());
        REQUIRE(second->seal());
        REQUIRE_FALSE(state.prepare_append(second).is_valid());
    };
    require_cross_operation_rejection([](auto& operation) {
        operation.query_pipeline_operation_identity = 0x2100u;
    });
    require_cross_operation_rejection([](auto& operation) {
        operation.query_pipeline_operation_identity = 0x1100u;
    });
    require_cross_operation_rejection([](auto& operation) {
        operation.seed_receipt_identity += 1u;
        operation.cache_qualification.seed_receipt_identity += 1u;
    });
    require_cross_operation_rejection([](auto& operation) {
        operation.cache_qualification.identity += 1u;
    });
    require_cross_operation_rejection([](auto& operation) {
        operation.cold_load_identity += 1u;
    });

    REQUIRE(admission.reserved_bytes() == baseline);
}

TEST_CASE("measured modes match the literal exclusive-engine matrix",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    using execution_engine_type = execution_engine;
    constexpr execution_engine_type forbidden = execution_engine_type::count;
    constexpr std::array<execution_engine_type, 20> simd_recomputed{
        execution_engine_type::simd_cpu, execution_engine_type::simd_cpu, execution_engine_type::scalar_cpu, forbidden, forbidden,
        forbidden, execution_engine_type::scalar_cpu, execution_engine_type::simd_cpu, execution_engine_type::simd_cpu, execution_engine_type::scalar_cpu,
        execution_engine_type::scalar_cpu, execution_engine_type::scalar_cpu, execution_engine_type::scalar_cpu, execution_engine_type::scalar_cpu, execution_engine_type::host_io,
        forbidden, execution_engine_type::scalar_cpu, execution_engine_type::simd_cpu, execution_engine_type::simd_cpu, execution_engine_type::simd_cpu,
    };
    constexpr std::array<execution_engine_type, 20> simd_persisted{
        execution_engine_type::simd_cpu, forbidden, forbidden, execution_engine_type::host_io, execution_engine_type::scalar_cpu,
        forbidden, execution_engine_type::scalar_cpu, execution_engine_type::simd_cpu, execution_engine_type::simd_cpu, execution_engine_type::scalar_cpu,
        execution_engine_type::scalar_cpu, execution_engine_type::scalar_cpu, execution_engine_type::scalar_cpu, execution_engine_type::scalar_cpu, execution_engine_type::host_io,
        forbidden, execution_engine_type::scalar_cpu, execution_engine_type::simd_cpu, execution_engine_type::simd_cpu, execution_engine_type::simd_cpu,
    };
    constexpr std::array<execution_engine_type, 20> gpu_recomputed{
        execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, forbidden, forbidden,
        forbidden, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu,
        execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::host_io,
        execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu,
    };
    constexpr std::array<execution_engine_type, 20> gpu_persisted{
        execution_engine_type::vulkan_gpu, forbidden, forbidden, execution_engine_type::host_io, execution_engine_type::vulkan_gpu,
        execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu,
        execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::host_io,
        execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu,
    };
    const auto require_matrix = [](
        executed_backend backend,
        target_index_source source,
        const std::array<execution_engine_type, 20>& expected) {
        for (std::size_t i = 0; i < expected.size(); ++i) {
            REQUIRE(expected_execution_engine(
                        backend, source, static_cast<pipeline_stage>(i))
                    == expected[i]);
        }
    };
    require_matrix(
        executed_backend::simd_cpu, target_index_source::recomputed, simd_recomputed);
    require_matrix(
        executed_backend::simd_cpu, target_index_source::persisted, simd_persisted);
    require_matrix(
        executed_backend::vulkan_gpu, target_index_source::recomputed, gpu_recomputed);
    require_matrix(
        executed_backend::vulkan_gpu, target_index_source::persisted, gpu_persisted);

    constexpr std::array<bool, 20> recomputed_required{
        true, true, true, false, false, false, false, false, false, false,
        false, false, false, false, false, false, false, false, false, false,
    };
    constexpr std::array<bool, 20> persisted_required{
        true, false, false, true, true, false, false, false, false, false,
        false, false, false, false, false, false, false, false, false, false,
    };
    for (std::size_t i = 0; i < k_pipeline_stage_count; ++i) {
        const auto stage = static_cast<pipeline_stage>(i);
        REQUIRE(is_front_end_eligibility_required(
                    target_index_source::recomputed, stage)
                == recomputed_required[i]);
        REQUIRE(is_front_end_eligibility_required(
                    target_index_source::persisted, stage)
                == persisted_required[i]);
    }
}

TEST_CASE("stats merges saturate and identities fail closed",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;

    stage_coverage coverage{};
    coverage.eligible_jobs = k_sql_bigint_value_max;
    coverage.executed_jobs[engine_index(execution_engine::simd_cpu)] =
        k_sql_bigint_value_max;
    stage_coverage increment{};
    increment.eligible_jobs = 1;
    increment.executed_jobs[engine_index(execution_engine::simd_cpu)] = 1;
    REQUIRE(coverage.merge(increment));
    REQUIRE(coverage.eligible_jobs == k_sql_bigint_value_max);
    REQUIRE(coverage.executed_jobs[engine_index(execution_engine::simd_cpu)]
            == k_sql_bigint_value_max);

    std::uint64_t exact_boundary = k_sql_bigint_value_max - 1;
    REQUIRE_FALSE(merge_counter(exact_boundary, 1));
    REQUIRE(exact_boundary == k_sql_bigint_value_max);
    std::uint64_t oversized = (std::numeric_limits<std::uint64_t>::max)();
    REQUIRE(merge_counter(oversized, 0));
    REQUIRE(oversized == k_sql_bigint_value_max);

    pipeline_operational_stats stage_timing_delta{};
    stage_timing_delta.timing[
        stage_index(pipeline_stage::query_sketch)].host_wall_ns = 7u;
    stage_timing_delta.timing[
        stage_index(pipeline_stage::seed_lookup)].host_wall_ns = 11u;
    stage_timing_delta.timing[
        stage_index(pipeline_stage::chain_score)].host_wall_ns = 13u;
    stage_timing_delta.timing[
        stage_index(pipeline_stage::region_generation)].host_wall_ns = 17u;
    stage_timing_delta.timing[
        stage_index(pipeline_stage::left_extension)].host_wall_ns = 19u;
    pipeline_operational_stats stage_timing_aggregate{};
    stage_timing_aggregate.merge(stage_timing_delta);
    REQUIRE(stage_timing_aggregate.timing[
        stage_index(pipeline_stage::query_sketch)].host_wall_ns == 7u);
    REQUIRE(stage_timing_aggregate.timing[
        stage_index(pipeline_stage::seed_lookup)].host_wall_ns == 11u);
    REQUIRE(stage_timing_aggregate.timing[
        stage_index(pipeline_stage::chain_score)].host_wall_ns == 13u);
    REQUIRE(stage_timing_aggregate.timing[
        stage_index(pipeline_stage::region_generation)].host_wall_ns == 17u);
    REQUIRE(stage_timing_aggregate.timing[
        stage_index(pipeline_stage::left_extension)].host_wall_ns == 19u);
    REQUIRE_FALSE(stage_timing_aggregate.are_stats_saturated);

    pipeline_operational_stats operational{};
    operational.target_rows = k_sql_bigint_value_max;
    pipeline_operational_stats operational_increment{};
    operational_increment.target_rows = 1;
    operational.merge(operational_increment);
    REQUIRE(operational.target_rows == k_sql_bigint_value_max);
    REQUIRE(operational.are_stats_saturated);

    REQUIRE(merge_backend_identity(executed_backend::none, executed_backend::simd_cpu)
            == executed_backend::simd_cpu);
    REQUIRE(merge_backend_identity(executed_backend::simd_cpu, executed_backend::simd_cpu)
            == executed_backend::simd_cpu);
    REQUIRE(merge_backend_identity(executed_backend::simd_cpu, executed_backend::vulkan_gpu)
            == executed_backend::mixed_invalid);
    REQUIRE(merge_target_index_identity(target_index_source::recomputed,
                                        target_index_source::persisted)
            == target_index_source::mixed_invalid);
    REQUIRE(merge_simd_chaining_policy_identity(
                simd_chaining_policy_identity::none,
                simd_chaining_policy_identity::bounded_batch_approximate)
            == simd_chaining_policy_identity::bounded_batch_approximate);
    REQUIRE(merge_simd_chaining_policy_identity(
                simd_chaining_policy_identity::exact_visited,
                simd_chaining_policy_identity::bounded_batch_approximate)
            == simd_chaining_policy_identity::mixed_invalid);
    REQUIRE(merge_simd_alignment_algorithm_identity(
                simd_alignment_algorithm_identity::none,
                simd_alignment_algorithm_identity::block_aligner)
            == simd_alignment_algorithm_identity::block_aligner);
    REQUIRE(merge_simd_alignment_algorithm_identity(
                simd_alignment_algorithm_identity::block_aligner,
                simd_alignment_algorithm_identity::mixed_invalid)
            == simd_alignment_algorithm_identity::mixed_invalid);

    execution_environment_snapshot merged{};
    const execution_environment_snapshot cpu = make_cpu_environment(3);
    merged.merge(cpu);
    REQUIRE_FALSE(merged.is_mixed_invalid);
    merged.merge(cpu);
    REQUIRE_FALSE(merged.is_mixed_invalid);
    merged.merge(make_cpu_environment(4));
    REQUIRE(merged.is_mixed_invalid);

    execution_environment_snapshot missing{};
    missing.merge(execution_environment_snapshot{});
    REQUIRE(missing.is_mixed_invalid);
}

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

TEST_CASE("merge_job and aggregate merge have identical drift semantics",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    const auto aggregate_job = [](const pipeline_job_execution_stats& job) {
        pipeline_execution_stats stats{};
        stats.merge_job(job);
        return stats;
    };
    const auto require_same_counts_and_coordinates = [](
            const pipeline_execution_stats& direct,
            const pipeline_execution_stats& grouped) {
        REQUIRE(direct.requested_backend == grouped.requested_backend);
        REQUIRE(direct.actual_backend == grouped.actual_backend);
        REQUIRE(direct.requested_target_index_source
                == grouped.requested_target_index_source);
        REQUIRE(direct.actual_target_index_source
                == grouped.actual_target_index_source);
        REQUIRE(direct.attempted_alignment_jobs
                == grouped.attempted_alignment_jobs);
        REQUIRE(direct.fully_attested_alignment_jobs
                == grouped.fully_attested_alignment_jobs);
        REQUIRE(direct.unattested_alignment_jobs
                == grouped.unattested_alignment_jobs);
        REQUIRE(direct.first_attempted_target_row
                == grouped.first_attempted_target_row);
        REQUIRE(direct.first_attempted_query_ordinal
                == grouped.first_attempted_query_ordinal);
        REQUIRE(direct.first_unattested_target_row
                == grouped.first_unattested_target_row);
        REQUIRE(direct.first_unattested_query_ordinal
                == grouped.first_unattested_query_ordinal);
    };

    SECTION("persisted generation drift") {
        auto first = make_attested_job(
            executed_backend::vulkan_gpu, target_index_source::persisted);
        first.logical_target_row = 41;
        first.query_ordinal = 5;
        auto second = first;
        second.logical_target_row = 42;
        second.query_ordinal = 6;
        second.persisted_generation_id = 18;

        pipeline_execution_stats direct{};
        direct.merge_job(first);
        direct.merge_job(second);
        auto grouped = aggregate_job(first);
        grouped.merge(aggregate_job(second));

        require_same_counts_and_coordinates(direct, grouped);
        REQUIRE(direct.requested_backend == executed_backend::vulkan_gpu);
        REQUIRE(direct.actual_backend == executed_backend::vulkan_gpu);
        REQUIRE(direct.has_mixed_persisted_identity);
        REQUIRE(grouped.has_mixed_persisted_identity);
        REQUIRE(direct.attempted_alignment_jobs == 2);
        REQUIRE(direct.fully_attested_alignment_jobs == 0);
        REQUIRE(direct.unattested_alignment_jobs == 2);
        REQUIRE(direct.first_unattested_target_row == 41);
        REQUIRE(direct.first_unattested_query_ordinal == 5);
    }

    SECTION("environment drift") {
        auto first = make_attested_job(
            executed_backend::simd_cpu, target_index_source::recomputed);
        first.logical_target_row = 51;
        first.query_ordinal = 7;
        auto second = first;
        second.logical_target_row = 52;
        second.query_ordinal = 8;
        second.environment.effective_cpu_workers = 3;

        pipeline_execution_stats direct{};
        direct.merge_job(first);
        direct.merge_job(second);
        auto grouped = aggregate_job(first);
        grouped.merge(aggregate_job(second));

        require_same_counts_and_coordinates(direct, grouped);
        REQUIRE(direct.requested_backend == executed_backend::simd_cpu);
        REQUIRE(direct.actual_backend == executed_backend::simd_cpu);
        REQUIRE(direct.environment.is_mixed_invalid);
        REQUIRE(grouped.environment.is_mixed_invalid);
        REQUIRE(direct.attempted_alignment_jobs == 2);
        REQUIRE(direct.fully_attested_alignment_jobs == 0);
        REQUIRE(direct.unattested_alignment_jobs == 2);
        REQUIRE(direct.first_unattested_target_row == 51);
        REQUIRE(direct.first_unattested_query_ordinal == 7);
    }
}

TEST_CASE("aggregate merge cannot launder absent environments",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    const auto missing_environment_operand = [] {
        pipeline_execution_stats stats{};
        stats.requested_backend = executed_backend::simd_cpu;
        stats.actual_backend = executed_backend::simd_cpu;
        stats.requested_simd_chaining_policy =
            simd_chaining_policy_identity::bounded_batch_approximate;
        stats.observed_simd_chaining_policy =
            simd_chaining_policy_identity::bounded_batch_approximate;
        stats.requested_target_index_source = target_index_source::recomputed;
        stats.actual_target_index_source = target_index_source::recomputed;
        stats.attempted_alignment_jobs = 1;
        stats.fully_attested_alignment_jobs = 1;
        stats.first_attempted_target_row = 91;
        stats.first_attempted_query_ordinal = 4;
        return stats;
    };
    const auto valid_operand = [] {
        pipeline_execution_stats stats{};
        stats.merge_job(make_attested_job(
            executed_backend::simd_cpu, target_index_source::recomputed));
        return stats;
    };

    auto missing_left = missing_environment_operand();
    missing_left.merge(valid_operand());
    REQUIRE(missing_left.environment.is_mixed_invalid);
    REQUIRE(missing_left.attempted_alignment_jobs == 2);
    REQUIRE(missing_left.fully_attested_alignment_jobs == 0);
    REQUIRE(missing_left.unattested_alignment_jobs == 2);
    REQUIRE(missing_left.first_unattested_target_row == 91);
    REQUIRE(missing_left.first_unattested_query_ordinal == 4);

    auto missing_right = valid_operand();
    missing_right.merge(missing_environment_operand());
    REQUIRE(missing_right.environment.is_mixed_invalid);
    REQUIRE(missing_right.attempted_alignment_jobs == 2);
    REQUIRE(missing_right.fully_attested_alignment_jobs == 0);
    REQUIRE(missing_right.unattested_alignment_jobs == 2);
    REQUIRE(missing_right.first_unattested_target_row == 0);
    REQUIRE(missing_right.first_unattested_query_ordinal == 0);

    pipeline_execution_stats zero_job_nonempty{};
    zero_job_nonempty.requested_backend = executed_backend::simd_cpu;
    zero_job_nonempty.actual_backend = executed_backend::simd_cpu;
    zero_job_nonempty.requested_simd_chaining_policy =
        simd_chaining_policy_identity::bounded_batch_approximate;
    zero_job_nonempty.observed_simd_chaining_policy =
        simd_chaining_policy_identity::bounded_batch_approximate;
    zero_job_nonempty.requested_target_index_source = target_index_source::recomputed;
    zero_job_nonempty.actual_target_index_source = target_index_source::recomputed;
    zero_job_nonempty.environment = make_cpu_environment();
    pipeline_execution_stats zero_job_aggregate{};
    zero_job_aggregate.merge(zero_job_nonempty);
    REQUIRE(zero_job_aggregate.environment.is_mixed_invalid);
    REQUIRE(zero_job_aggregate.attempted_alignment_jobs == 0);
    REQUIRE(zero_job_aggregate.fully_attested_alignment_jobs == 0);
    REQUIRE(zero_job_aggregate.unattested_alignment_jobs == 0);
}

TEST_CASE("exact-empty stats checks every stage and operational member",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    const pipeline_execution_stats empty{};
    REQUIRE(pipeline_execution_stats::is_exact_empty_stats_operand(empty));

    SECTION("stage coverage is observations even with no attempted jobs") {
        auto injected = empty;
        injected.stages.coverage[
            stage_index(pipeline_stage::query_sketch)].eligible_jobs = 1;
        REQUIRE_FALSE(pipeline_execution_stats::is_exact_empty_stats_operand(
            injected));
    }

    SECTION("stage timing is operational observations") {
        auto injected = empty;
        injected.operational.timing[
            stage_index(pipeline_stage::query_sketch)].host_wall_ns = 1;
        REQUIRE_FALSE(pipeline_execution_stats::is_exact_empty_stats_operand(
            injected));
    }

    SECTION("an ordinary operational counter is observations") {
        auto injected = empty;
        injected.operational.target_rows = 1;
        REQUIRE_FALSE(pipeline_execution_stats::is_exact_empty_stats_operand(
            injected));
    }

    SECTION("a zero-valued report-once identity still has a presence bit") {
        auto injected = empty;
        injected.operational.cpu_prepared_key_set_checksum_available = true;
        REQUIRE_FALSE(pipeline_execution_stats::is_exact_empty_stats_operand(
            injected));
    }
}

TEST_CASE("zero-job stage observations cannot launder a complete aggregate",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    auto job = make_attested_job(
        executed_backend::simd_cpu, target_index_source::recomputed);
    job.logical_target_row = 71;
    job.query_ordinal = 8;
    pipeline_execution_stats complete{};
    complete.merge_job(job);
    REQUIRE(pipeline_execution_stats::can_retain_full_attestation(complete));

    pipeline_execution_stats injected{};
    auto& query = injected.stages.coverage[
        stage_index(pipeline_stage::query_sketch)];
    query.eligible_jobs = 1;
    query.executed_jobs[engine_index(execution_engine::simd_cpu)] = 1;
    REQUIRE_FALSE(pipeline_execution_stats::is_exact_empty_stats_operand(
        injected));

    const auto require_rejected = [](const pipeline_execution_stats& stats) {
        REQUIRE(stats.requested_backend == executed_backend::simd_cpu);
        REQUIRE(stats.actual_backend == executed_backend::simd_cpu);
        REQUIRE(stats.attempted_alignment_jobs == 1);
        REQUIRE(stats.fully_attested_alignment_jobs == 0);
        REQUIRE(stats.unattested_alignment_jobs == 1);
        REQUIRE(stats.first_unattested_target_row == 71);
        REQUIRE(stats.first_unattested_query_ordinal == 8);
        REQUIRE_FALSE(pipeline_execution_stats::is_aggregate_stage_coverage_valid(
            stats));
        REQUIRE_FALSE(pipeline_execution_stats::can_retain_full_attestation(
            stats));
    };

    auto injected_on_right = complete;
    injected_on_right.merge(injected);
    require_rejected(injected_on_right);

    auto injected_on_left = injected;
    injected_on_left.merge(complete);
    require_rejected(injected_on_left);
}

TEST_CASE("aggregate retention rejects malformed stage matrices",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    auto job = make_attested_job(
        executed_backend::simd_cpu, target_index_source::recomputed);
    job.logical_target_row = 81;
    job.query_ordinal = 6;
    pipeline_execution_stats complete{};
    complete.merge_job(job);
    REQUIRE(pipeline_execution_stats::is_aggregate_stage_coverage_valid(
        complete));
    REQUIRE(pipeline_execution_stats::can_retain_full_attestation(complete));

    const auto require_malformed_stage_rejected = [](
            const pipeline_execution_stats& malformed) {
        REQUIRE_FALSE(pipeline_execution_stats::is_aggregate_stage_coverage_valid(
            malformed));
        REQUIRE_FALSE(pipeline_execution_stats::can_retain_full_attestation(
            malformed));
        pipeline_execution_stats merged{};
        merged.merge(malformed);
        REQUIRE(merged.attempted_alignment_jobs == 1);
        REQUIRE(merged.fully_attested_alignment_jobs == 0);
        REQUIRE(merged.unattested_alignment_jobs == 1);
        REQUIRE(merged.first_unattested_target_row == 81);
        REQUIRE(merged.first_unattested_query_ordinal == 6);
    };

    SECTION("required front-end coverage is missing") {
        auto malformed = complete;
        malformed.stages.coverage[
            stage_index(pipeline_stage::query_sketch)] = {};
        require_malformed_stage_rejected(malformed);
    }

    SECTION("stage total exceeds the attempted-job total") {
        auto malformed = complete;
        auto& query = malformed.stages.coverage[
            stage_index(pipeline_stage::query_sketch)];
        query.eligible_jobs = 2;
        query.executed_jobs[engine_index(execution_engine::simd_cpu)] = 2;
        require_malformed_stage_rejected(malformed);
    }

    SECTION("a forbidden engine lane carries work") {
        auto malformed = complete;
        malformed.stages.coverage[
            stage_index(pipeline_stage::query_sketch)]
            .executed_jobs[engine_index(execution_engine::scalar_cpu)] = 1;
        require_malformed_stage_rejected(malformed);
    }

    SECTION("a fallback lane carries work") {
        auto malformed = complete;
        malformed.stages.coverage[
            stage_index(pipeline_stage::query_sketch)].fallback_jobs = 1;
        require_malformed_stage_rejected(malformed);
    }

    SECTION("a mode-forbidden stage carries work") {
        auto malformed = complete;
        auto& upload = malformed.stages.coverage[
            stage_index(pipeline_stage::device_index_upload)];
        upload.eligible_jobs = 1;
        upload.executed_jobs[engine_index(execution_engine::simd_cpu)] = 1;
        require_malformed_stage_rejected(malformed);
    }

    SECTION("a saturated aggregate cannot retain stats") {
        auto malformed = complete;
        malformed.operational.are_stats_saturated = true;
        REQUIRE(pipeline_execution_stats::is_aggregate_stage_coverage_valid(
            malformed));
        REQUIRE_FALSE(pipeline_execution_stats::can_retain_full_attestation(
            malformed));
        pipeline_execution_stats merged{};
        merged.merge(malformed);
        REQUIRE(merged.fully_attested_alignment_jobs == 0);
        REQUIRE(merged.unattested_alignment_jobs == 1);
        REQUIRE(merged.first_unattested_target_row == 81);
        REQUIRE(merged.first_unattested_query_ordinal == 6);
    }
}

TEST_CASE("backend declines and fallbacks always revoke attestation",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    auto job = make_attested_job(
        executed_backend::simd_cpu, target_index_source::recomputed);
    job.logical_target_row = 91;
    job.query_ordinal = 7;

    const auto require_revoked = [](const pipeline_execution_stats& stats) {
        REQUIRE(stats.attempted_alignment_jobs == 1);
        REQUIRE(stats.fully_attested_alignment_jobs == 0);
        REQUIRE(stats.unattested_alignment_jobs == 1);
        REQUIRE(stats.first_unattested_target_row == 91);
        REQUIRE(stats.first_unattested_query_ordinal == 7);
        REQUIRE_FALSE(pipeline_execution_stats::can_retain_full_attestation(
            stats));
    };

    for (const bool is_fallback : {false, true}) {
        pipeline_operational_stats observations{};
        if (is_fallback) {
            observations.backend_fallbacks = 1;
        } else {
            observations.backend_declines = 1;
        }

        pipeline_execution_stats operational_after_job{};
        operational_after_job.merge_job(job);
        operational_after_job.merge_operational(observations);
        require_revoked(operational_after_job);

        pipeline_execution_stats operational_before_job{};
        operational_before_job.merge_operational(observations);
        operational_before_job.merge_job(job);
        require_revoked(operational_before_job);

        pipeline_execution_stats operational_only{};
        operational_only.merge_operational(observations);
        pipeline_execution_stats grouped_after_job{};
        grouped_after_job.merge_job(job);
        grouped_after_job.merge(operational_only);
        require_revoked(grouped_after_job);

        auto grouped_before_job = operational_only;
        pipeline_execution_stats job_only{};
        job_only.merge_job(job);
        grouped_before_job.merge(job_only);
        require_revoked(grouped_before_job);
    }
}

TEST_CASE("mixed CPU diagnostics retain environment without becoming stats",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    pipeline_job_execution_stats job{};
    job.requested_backend = executed_backend::mixed_invalid;
    job.actual_backend = executed_backend::mixed_invalid;
    job.requested_target_index_source = target_index_source::recomputed;
    job.actual_target_index_source = target_index_source::recomputed;
    job.environment = make_cpu_environment();

    pipeline_execution_stats node{};
    node.merge_job(job);
    REQUIRE(node.attempted_alignment_jobs == 1);
    REQUIRE(node.fully_attested_alignment_jobs == 0);
    REQUIRE(node.unattested_alignment_jobs == 1);
    REQUIRE(pipeline_execution_stats::is_diagnostic_environment_valid(node));

    pipeline_execution_stats statement{};
    statement.merge(node);
    REQUIRE(statement.actual_backend == executed_backend::mixed_invalid);
    REQUIRE(statement.fully_attested_alignment_jobs == 0);
    REQUIRE(statement.unattested_alignment_jobs == 1);
    REQUIRE_FALSE(statement.environment.is_mixed_invalid);
    REQUIRE(pipeline_execution_stats::is_diagnostic_environment_valid(statement));

    pipeline_execution_stats valid_sibling{};
    auto valid_job = make_attested_job(
        executed_backend::simd_cpu, target_index_source::recomputed);
    valid_job.logical_target_row = 12;
    valid_job.query_ordinal = 3;
    valid_sibling.merge_job(valid_job);
    statement.merge(valid_sibling);
    REQUIRE(statement.actual_backend == executed_backend::mixed_invalid);
    REQUIRE(statement.fully_attested_alignment_jobs == 0);
    REQUIRE(statement.unattested_alignment_jobs == 2);
    REQUIRE(pipeline_execution_stats::is_diagnostic_environment_valid(statement));

    auto gpu_work_with_cpu_environment = node;
    gpu_work_with_cpu_environment.operational.gpu_seed_dispatches = 1;
    REQUIRE_FALSE(pipeline_execution_stats::is_diagnostic_environment_valid(
        gpu_work_with_cpu_environment));
    pipeline_execution_stats rejected_statement{};
    rejected_statement.merge(gpu_work_with_cpu_environment);
    REQUIRE(rejected_statement.environment.is_mixed_invalid);
}

TEST_CASE("aggregate merge is associative under environment drift",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    const auto aggregate_job = [](const pipeline_job_execution_stats& job) {
        pipeline_execution_stats stats{};
        stats.merge_job(job);
        return stats;
    };

    auto first_job = make_attested_job(
        executed_backend::simd_cpu, target_index_source::recomputed);
    first_job.logical_target_row = 10;
    first_job.query_ordinal = 1;
    auto second_job = first_job;
    second_job.logical_target_row = 20;
    second_job.query_ordinal = 2;
    second_job.environment.effective_cpu_workers = 3;
    auto third_job = second_job;
    third_job.logical_target_row = 30;
    third_job.query_ordinal = 3;

    auto left_grouped = aggregate_job(first_job);
    left_grouped.merge(aggregate_job(second_job));
    left_grouped.merge(aggregate_job(third_job));

    auto right_tail = aggregate_job(second_job);
    right_tail.merge(aggregate_job(third_job));
    auto right_grouped = aggregate_job(first_job);
    right_grouped.merge(right_tail);

    const auto require_same_fail_closed_result = [](
            const pipeline_execution_stats& stats) {
        REQUIRE(stats.requested_backend == executed_backend::simd_cpu);
        REQUIRE(stats.actual_backend == executed_backend::simd_cpu);
        REQUIRE(stats.requested_target_index_source
                == target_index_source::recomputed);
        REQUIRE(stats.actual_target_index_source
                == target_index_source::recomputed);
        REQUIRE(stats.environment.is_mixed_invalid);
        REQUIRE(stats.attempted_alignment_jobs == 3);
        REQUIRE(stats.fully_attested_alignment_jobs == 0);
        REQUIRE(stats.unattested_alignment_jobs == 3);
        REQUIRE(stats.first_attempted_target_row == 10);
        REQUIRE(stats.first_attempted_query_ordinal == 1);
        REQUIRE(stats.first_unattested_target_row == 10);
        REQUIRE(stats.first_unattested_query_ordinal == 1);
    };
    require_same_fail_closed_result(left_grouped);
    require_same_fail_closed_result(right_grouped);
    REQUIRE(left_grouped.attempted_alignment_jobs
            == right_grouped.attempted_alignment_jobs);
    REQUIRE(left_grouped.fully_attested_alignment_jobs
            == right_grouped.fully_attested_alignment_jobs);
    REQUIRE(left_grouped.unattested_alignment_jobs
            == right_grouped.unattested_alignment_jobs);
    REQUIRE(left_grouped.first_unattested_target_row
            == right_grouped.first_unattested_target_row);
    REQUIRE(left_grouped.first_unattested_query_ordinal
            == right_grouped.first_unattested_query_ordinal);
}

TEST_CASE("aggregate merge is associative at SQL bigint saturation",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    const auto aggregate_job = [](std::uint64_t target_row) {
        auto job = make_attested_job(
            executed_backend::simd_cpu, target_index_source::recomputed);
        job.logical_target_row = target_row;
        pipeline_execution_stats stats{};
        stats.merge_job(job);
        return stats;
    };

    auto near_limit = aggregate_job(10);
    near_limit.attempted_alignment_jobs = k_sql_bigint_value_max;
    near_limit.fully_attested_alignment_jobs = k_sql_bigint_value_max;
    auto second = aggregate_job(20);
    auto third = aggregate_job(30);

    auto left_grouped = near_limit;
    left_grouped.merge(second);
    left_grouped.merge(third);

    auto right_tail = second;
    right_tail.merge(third);
    auto right_grouped = near_limit;
    right_grouped.merge(right_tail);

    const auto require_saturated = [](const pipeline_execution_stats& stats) {
        REQUIRE(stats.attempted_alignment_jobs == k_sql_bigint_value_max);
        REQUIRE(stats.fully_attested_alignment_jobs == 0);
        REQUIRE(stats.unattested_alignment_jobs == k_sql_bigint_value_max);
        REQUIRE(stats.first_unattested_target_row == 10);
        REQUIRE(stats.first_unattested_query_ordinal == 0);
        REQUIRE(stats.operational.are_stats_saturated);
    };
    require_saturated(left_grouped);
    require_saturated(right_grouped);
    REQUIRE(left_grouped.requested_backend == right_grouped.requested_backend);
    REQUIRE(left_grouped.actual_backend == right_grouped.actual_backend);
    REQUIRE(left_grouped.fully_attested_alignment_jobs
            == right_grouped.fully_attested_alignment_jobs);
    REQUIRE(left_grouped.unattested_alignment_jobs
            == right_grouped.unattested_alignment_jobs);
}

TEST_CASE("operational saturation is fail closed independent of ordering",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    auto job = make_attested_job(
        executed_backend::simd_cpu, target_index_source::recomputed);
    job.logical_target_row = 61;
    job.query_ordinal = 9;
    const auto saturate_operational = [](pipeline_execution_stats& stats) {
        pipeline_operational_stats at_limit{};
        at_limit.target_rows = k_sql_bigint_value_max;
        pipeline_operational_stats overflow{};
        overflow.target_rows = 1;
        stats.merge_operational(at_limit);
        stats.merge_operational(overflow);
    };

    pipeline_execution_stats operational_after_job{};
    operational_after_job.merge_job(job);
    saturate_operational(operational_after_job);

    pipeline_execution_stats operational_before_job{};
    saturate_operational(operational_before_job);
    operational_before_job.merge_job(job);

    pipeline_execution_stats operational_only{};
    saturate_operational(operational_only);
    pipeline_execution_stats grouped{};
    grouped.merge_job(job);
    grouped.merge(operational_only);

    const auto require_fail_closed = [](const pipeline_execution_stats& stats) {
        REQUIRE(stats.requested_backend == executed_backend::simd_cpu);
        REQUIRE(stats.actual_backend == executed_backend::simd_cpu);
        REQUIRE(stats.attempted_alignment_jobs == 1);
        REQUIRE(stats.fully_attested_alignment_jobs == 0);
        REQUIRE(stats.unattested_alignment_jobs == 1);
        REQUIRE(stats.first_unattested_target_row == 61);
        REQUIRE(stats.first_unattested_query_ordinal == 9);
        REQUIRE(stats.operational.target_rows == k_sql_bigint_value_max);
        REQUIRE(stats.operational.are_stats_saturated);
    };
    require_fail_closed(operational_after_job);
    require_fail_closed(operational_before_job);
    require_fail_closed(grouped);
}

TEST_CASE("cold-index operational telemetry merges and saturates through one authority",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;

    pipeline_operational_stats aggregate{};
    pipeline_operational_stats first{};
    first.cold_index_sidecar_heap_opens = 2;
    first.cold_index_sidecar_index_opens = 2;
    first.cold_index_lock_acquires = 7;
    first.cold_index_lock_releases = 6;
    first.cold_index_payload_bytes_delivered = 4096;
    first.cold_index_payload_bytes_scanned = 6144;
    first.cold_index_payload_rows_scanned = 3;
    first.cold_index_payload_rows_skipped = 1;
    first.cold_index_shared_blks_read = 3;
    first.grouped_soa_reader_capacity_accounting_passes = 5;
    first.grouped_soa_reader_global_position_storage_preparations = 1;
    first.grouped_soa_reader_global_position_storage_allocations = 1;
    first.grouped_soa_reader_retained_position_count = 37;
    first.grouped_soa_reader_retained_position_capacity = 37;
    first.grouped_soa_reader_directory_capacity_bytes = 10;
    first.grouped_soa_reader_query_hash_capacity_bytes = 7;
    first.grouped_soa_reader_query_record_capacity_bytes = 100;
    first.grouped_soa_reader_ordinal_route_capacity_bytes = 20;
    first.grouped_soa_reader_selected_extent_capacity_bytes = 30;
    first.grouped_soa_reader_current_selected_control_capacity_bytes = 11;
    first.grouped_soa_reader_global_position_capacity_bytes = 296;
    first.grouped_soa_reader_control_payload_capacity_bytes = 13;
    first.grouped_soa_reader_owned_capacity_bytes = 487;
    first.grouped_soa_reader_admitted_bytes = 487;
    first.grouped_soa_reader_exact_position_image_checks = 1;
    first.grouped_soa_reader_retained_layout_checks = 1;
    aggregate.merge(first);

    REQUIRE(aggregate.cold_index_sidecar_heap_opens == 2);
    REQUIRE(aggregate.cold_index_sidecar_index_opens == 2);
    REQUIRE(aggregate.cold_index_lock_acquires == 7);
    REQUIRE(aggregate.cold_index_lock_releases == 6);
    REQUIRE(aggregate.cold_index_payload_bytes_delivered == 4096);
    REQUIRE(aggregate.cold_index_payload_bytes_scanned == 6144);
    REQUIRE(aggregate.cold_index_payload_rows_scanned == 3);
    REQUIRE(aggregate.cold_index_payload_rows_skipped == 1);
    REQUIRE(aggregate.cold_index_shared_blks_read == 3);
    REQUIRE(aggregate.grouped_soa_reader_capacity_accounting_passes == 5);
    REQUIRE(aggregate.grouped_soa_reader_global_position_storage_preparations == 1);
    REQUIRE(aggregate.grouped_soa_reader_global_position_storage_allocations == 1);
    REQUIRE(aggregate.grouped_soa_reader_position_append_growth_reservations == 0);
    REQUIRE(aggregate.grouped_soa_reader_retained_position_count == 37);
    REQUIRE(aggregate.grouped_soa_reader_retained_position_capacity == 37);
    REQUIRE(aggregate.grouped_soa_reader_query_hash_capacity_bytes == 7);
    REQUIRE(aggregate.grouped_soa_reader_current_selected_control_capacity_bytes
            == 11);
    REQUIRE(aggregate.grouped_soa_reader_global_position_capacity_bytes == 296);
    REQUIRE(aggregate.grouped_soa_reader_control_payload_capacity_bytes == 13);
    REQUIRE(aggregate.grouped_soa_reader_owned_capacity_bytes == 487);
    REQUIRE(aggregate.grouped_soa_reader_admitted_bytes == 487);
    REQUIRE(aggregate.grouped_soa_reader_exact_position_image_checks == 1);
    REQUIRE(aggregate.grouped_soa_reader_exact_position_image_failures == 0);
    REQUIRE(aggregate.grouped_soa_reader_retained_layout_checks == 1);
    REQUIRE(aggregate.grouped_soa_reader_retained_layout_failures == 0);
    REQUIRE_FALSE(aggregate.are_grouped_soa_reader_memory_stats_saturated);
    REQUIRE_FALSE(aggregate.are_stats_saturated);

    pipeline_operational_stats second{};
    second.grouped_soa_reader_capacity_accounting_passes = 4;
    second.grouped_soa_reader_global_position_storage_preparations = 1;
    second.grouped_soa_reader_global_position_storage_allocations = 1;
    second.grouped_soa_reader_position_append_growth_reservations = 2;
    second.grouped_soa_reader_retained_position_count = 11;
    second.grouped_soa_reader_retained_position_capacity = 11;
    second.grouped_soa_reader_directory_capacity_bytes = 5;
    second.grouped_soa_reader_query_hash_capacity_bytes = 3;
    second.grouped_soa_reader_query_record_capacity_bytes = 40;
    second.grouped_soa_reader_ordinal_route_capacity_bytes = 8;
    second.grouped_soa_reader_selected_extent_capacity_bytes = 12;
    second.grouped_soa_reader_current_selected_control_capacity_bytes = 5;
    second.grouped_soa_reader_global_position_capacity_bytes = 87;
    second.grouped_soa_reader_control_payload_capacity_bytes = 7;
    second.grouped_soa_reader_owned_capacity_bytes = 167;
    second.grouped_soa_reader_admitted_bytes = 168;
    second.grouped_soa_reader_exact_position_image_checks = 1;
    second.grouped_soa_reader_exact_position_image_failures = 1;
    second.grouped_soa_reader_retained_layout_checks = 1;
    second.grouped_soa_reader_retained_layout_failures = 1;
    second.are_grouped_soa_reader_memory_stats_saturated = true;
    aggregate.merge(second);

    REQUIRE(aggregate.grouped_soa_reader_capacity_accounting_passes == 9);
    REQUIRE(aggregate.grouped_soa_reader_global_position_storage_preparations == 2);
    REQUIRE(aggregate.grouped_soa_reader_global_position_storage_allocations == 2);
    REQUIRE(aggregate.grouped_soa_reader_position_append_growth_reservations == 2);
    REQUIRE(aggregate.grouped_soa_reader_retained_position_count == 48);
    REQUIRE(aggregate.grouped_soa_reader_retained_position_capacity == 48);
    REQUIRE(aggregate.grouped_soa_reader_query_hash_capacity_bytes == 10);
    REQUIRE(aggregate.grouped_soa_reader_current_selected_control_capacity_bytes
            == 16);
    REQUIRE(aggregate.grouped_soa_reader_global_position_capacity_bytes == 383);
    REQUIRE(aggregate.grouped_soa_reader_control_payload_capacity_bytes == 20);
    REQUIRE(aggregate.grouped_soa_reader_owned_capacity_bytes == 654);
    REQUIRE(aggregate.grouped_soa_reader_admitted_bytes == 655);
    REQUIRE(aggregate.grouped_soa_reader_exact_position_image_checks == 2);
    REQUIRE(aggregate.grouped_soa_reader_exact_position_image_failures == 1);
    REQUIRE(aggregate.grouped_soa_reader_retained_layout_checks == 2);
    REQUIRE(aggregate.grouped_soa_reader_retained_layout_failures == 1);
    REQUIRE(aggregate.are_grouped_soa_reader_memory_stats_saturated);
    REQUIRE_FALSE(aggregate.are_stats_saturated);

    pipeline_operational_stats at_limit{};
    at_limit.cold_index_payload_bytes_scanned = k_sql_bigint_value_max;
    at_limit.grouped_soa_reader_retained_position_capacity = k_sql_bigint_value_max;
    pipeline_operational_stats overflow{};
    overflow.cold_index_payload_bytes_scanned = 1;
    overflow.grouped_soa_reader_retained_position_capacity = 1;
    aggregate.merge(at_limit);
    aggregate.merge(overflow);

    REQUIRE(aggregate.cold_index_payload_bytes_scanned
            == k_sql_bigint_value_max);
    REQUIRE(aggregate.grouped_soa_reader_retained_position_capacity
            == k_sql_bigint_value_max);
    REQUIRE(aggregate.are_stats_saturated);

    using capacity_member_type =
        std::uint64_t pipeline_operational_stats::*;
    const auto require_independent_saturation = [](capacity_member_type member) {
        pipeline_operational_stats exact_limit{};
        exact_limit.*member = k_sql_bigint_value_max;
        pipeline_operational_stats independently_merged{};
        independently_merged.merge(exact_limit);
        REQUIRE((independently_merged.*member) == k_sql_bigint_value_max);
        REQUIRE_FALSE(independently_merged.are_stats_saturated);

        pipeline_operational_stats plus_one{};
        plus_one.*member = 1;
        independently_merged.merge(plus_one);
        REQUIRE((independently_merged.*member) == k_sql_bigint_value_max);
        REQUIRE(independently_merged.are_stats_saturated);
    };
    require_independent_saturation(
        &pipeline_operational_stats::grouped_soa_reader_query_hash_capacity_bytes);
    require_independent_saturation(
        &pipeline_operational_stats::
            grouped_soa_reader_current_selected_control_capacity_bytes);
    require_independent_saturation(
        &pipeline_operational_stats::grouped_soa_reader_control_payload_capacity_bytes);
}

TEST_CASE("fragment-native query observations preserves additive and peak merge semantics",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using bioseq::indexing::prepared_fragment_anchor_batch_stats;
    using bioseq::pipeline_stats::pipeline_operational_stats;

    prepared_fragment_anchor_batch_stats first{};
    first.waves = 1u;
    first.queries = 3u;
    first.query_occurrences = 7u;
    first.unique_query_plans = 3u;
    first.packed_occurrence_waves = 1u;
    first.wide_occurrence_waves = 0u;
    first.occurrence_fill_ns = 11u;
    first.control_count_bytes = 101u;
    first.pass2_position_bytes = 40u;
    first.pass3_position_bytes = 40u;
    first.anchors_constructed = 12u;
    first.exact_final_bytes = 288u;
    first.peak_exact_final_bytes = 288u;
    first.global_sort_calls_skipped = 3u;
    first.peak_occurrence_capacity_bytes = 96u;
    first.peak_block_layout_capacity_bytes = 144u;
    first.occurrence_allocations = 1u;

    prepared_fragment_anchor_batch_stats second{};
    second.waves = 2u;
    second.queries = 5u;
    second.query_occurrences = 11u;
    second.unique_query_plans = 4u;
    second.packed_occurrence_waves = 1u;
    second.wide_occurrence_waves = 1u;
    second.occurrence_fill_ns = 17u;
    second.control_count_bytes = 203u;
    second.pass2_position_bytes = 72u;
    second.pass3_position_bytes = 72u;
    second.anchors_constructed = 21u;
    second.exact_final_bytes = 504u;
    second.peak_exact_final_bytes = 360u;
    second.global_sort_calls_skipped = 5u;
    second.peak_occurrence_capacity_bytes = 80u;
    second.peak_block_layout_capacity_bytes = 192u;
    second.occurrence_allocations = 0u;

    first.merge(second);
    REQUIRE(first.waves == 3u);
    REQUIRE(first.queries == 8u);
    REQUIRE(first.query_occurrences == 18u);
    REQUIRE(first.unique_query_plans == 7u);
    REQUIRE(first.query_occurrences >= first.unique_query_plans);
    REQUIRE(first.packed_occurrence_waves == 2u);
    REQUIRE(first.wide_occurrence_waves == 1u);
    REQUIRE(first.packed_occurrence_waves + first.wide_occurrence_waves
        == first.waves);
    REQUIRE(first.occurrence_fill_ns == 28u);
    REQUIRE(first.control_count_bytes == 304u);
    REQUIRE(first.pass2_position_bytes == 112u);
    REQUIRE(first.pass2_position_bytes == first.pass3_position_bytes);
    REQUIRE(first.anchors_constructed == 33u);
    REQUIRE(first.exact_final_bytes == 792u);
    REQUIRE(first.peak_exact_final_bytes == 360u);
    REQUIRE(first.global_sort_calls_invoked == 0u);
    REQUIRE(first.global_sort_calls_skipped == 8u);
    REQUIRE(first.peak_occurrence_capacity_bytes == 96u);
    REQUIRE(first.peak_block_layout_capacity_bytes == 192u);
    REQUIRE(first.occurrence_allocations == 1u);
    REQUIRE(first.forbidden_buffers_are_zero());
    REQUIRE_FALSE(first.saturated);

    pipeline_operational_stats aggregate{};
    pipeline_operational_stats delta{};
    delta.fragment_native_query_stats = first;
    aggregate.merge(delta);
    REQUIRE(aggregate.fragment_native_query_stats == first);
    REQUIRE_FALSE(aggregate.are_stats_saturated);

    pipeline_operational_stats overflow{};
    overflow.fragment_native_query_stats.waves =
        (std::numeric_limits<std::uint64_t>::max)();
    overflow.fragment_native_query_stats.query_occurrences =
        (std::numeric_limits<std::uint64_t>::max)();
    aggregate.merge(overflow);
    REQUIRE(aggregate.fragment_native_query_stats.waves
        == (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE(aggregate.fragment_native_query_stats.query_occurrences
        == (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE(aggregate.fragment_native_query_stats.saturated);
    REQUIRE(aggregate.are_stats_saturated);
}

TEST_CASE("grouped-SoA provider observations preserves stable codes and checked counters",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;

    pipeline_operational_stats first{};
    auto& first_stats = first.grouped_soa_provider;
    first_stats.record_provider_configured(1u);
    first_stats.record_provider_captured(1u);
    first_stats.record_provider_persisted(1u);
    first_stats.record_provider_opened(1u);
    first_stats.record_mapped_sink_preallocation_result(
        grouped_soa_operation_result::succeeded);
    first_stats.record_lazy_state_layout(
        grouped_soa_state_layout::compact_aligned_planes);
    first_stats.record_madvise(
        grouped_soa_mapping_advice::random,
        grouped_soa_operation_result::succeeded);
    first_stats.record_counter(first_stats.provider_head_scans, 1u);
    first_stats.record_counter(first_stats.provider_head_rows, 1u);
    first_stats.record_counter(first_stats.mapped_sink_calls, 1u);
    first_stats.record_counter(first_stats.mapped_sink_bytes, 100u);
    first_stats.record_counter(first_stats.mapping_bytes, 400u);
    first_stats.record_file_action_high_water(2u);
    REQUIRE(first_stats.provider_identity_complete_and_consistent());

    pipeline_operational_stats second{};
    auto& second_stats = second.grouped_soa_provider;
    second_stats.record_provider_configured(1u);
    second_stats.record_provider_captured(1u);
    second_stats.record_provider_persisted(1u);
    second_stats.record_provider_opened(1u);
    second_stats.record_mapped_sink_preallocation_result(
        grouped_soa_operation_result::succeeded);
    second_stats.record_lazy_state_layout(
        grouped_soa_state_layout::compact_aligned_planes);
    second_stats.record_madvise(
        grouped_soa_mapping_advice::random,
        grouped_soa_operation_result::succeeded);
    second_stats.record_counter(second_stats.provider_head_scans, 1u);
    second_stats.record_counter(second_stats.provider_head_rows, 1u);
    second_stats.record_counter(second_stats.mapped_sink_calls, 1u);
    second_stats.record_counter(second_stats.mapped_sink_bytes, 200u);
    second_stats.record_counter(second_stats.mapping_bytes, 800u);
    second_stats.record_file_action_high_water(3u);

    pipeline_operational_stats aggregate{};
    aggregate.merge(first);
    aggregate.merge(second);
    const auto& observations = aggregate.grouped_soa_provider;
    REQUIRE(observations.provider_identity_complete_and_consistent());
    REQUIRE(observations.provider_configured == 1u);
    REQUIRE(observations.provider_captured == 1u);
    REQUIRE(observations.provider_persisted == 1u);
    REQUIRE(observations.provider_opened == 1u);
    REQUIRE(observations.provider_head_scans == 2u);
    REQUIRE(observations.provider_head_rows == 2u);
    REQUIRE(observations.mapped_sink_calls == 2u);
    REQUIRE(observations.mapped_sink_bytes == 300u);
    REQUIRE(observations.mapping_bytes == 1'200u);
    REQUIRE(observations.file_action_high_water == 3u);
    REQUIRE(observations.mapped_sink_preallocation_result
        == static_cast<std::uint64_t>(
            grouped_soa_operation_result::succeeded));
    REQUIRE(observations.lazy_state_layout
        == static_cast<std::uint64_t>(
            grouped_soa_state_layout::compact_aligned_planes));
    REQUIRE(observations.madvise_choice
        == static_cast<std::uint64_t>(grouped_soa_mapping_advice::random));
    REQUIRE(observations.madvise_result
        == static_cast<std::uint64_t>(
            grouped_soa_operation_result::succeeded));
    REQUIRE_FALSE(observations.has_stable_value_conflict);
    REQUIRE_FALSE(observations.is_saturated);
    REQUIRE_FALSE(aggregate.has_operational_identity_conflict);
    REQUIRE_FALSE(aggregate.are_grouped_soa_reader_memory_stats_saturated);
    REQUIRE_FALSE(aggregate.are_stats_saturated);

    pipeline_operational_stats conflicting{};
    conflicting.grouped_soa_provider.record_provider_opened(0u);
    aggregate.merge(conflicting);
    REQUIRE(aggregate.has_operational_identity_conflict);
    REQUIRE(aggregate.grouped_soa_provider.has_stable_value_conflict);
    REQUIRE_FALSE(
        aggregate.grouped_soa_provider.provider_identity_complete_and_consistent());

    pipeline_operational_stats exact_limit{};
    exact_limit.grouped_soa_provider.record_counter(
        exact_limit.grouped_soa_provider.mapping_bytes,
        k_sql_bigint_value_max);
    pipeline_operational_stats saturated_aggregate{};
    saturated_aggregate.merge(exact_limit);
    REQUIRE(saturated_aggregate.grouped_soa_provider.mapping_bytes
        == k_sql_bigint_value_max);
    REQUIRE_FALSE(saturated_aggregate.are_stats_saturated);

    pipeline_operational_stats plus_one{};
    plus_one.grouped_soa_provider.record_counter(
        plus_one.grouped_soa_provider.mapping_bytes, 1u);
    saturated_aggregate.merge(plus_one);
    REQUIRE(saturated_aggregate.grouped_soa_provider.mapping_bytes
        == k_sql_bigint_value_max);
    REQUIRE(saturated_aggregate.grouped_soa_provider.is_saturated);
    REQUIRE(saturated_aggregate.are_grouped_soa_reader_memory_stats_saturated);
    REQUIRE(saturated_aggregate.are_stats_saturated);
}

TEST_CASE("hash-major reader receipts preserve max peaks and owners emit once",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;

    pipeline_operational_stats first{};
    auto& first_hash_major = first.hash_major_index_reader;
    first_hash_major.layout_revision = 6;
    first_hash_major.sessions = 1;
    first_hash_major.identity_available = true;
    first_hash_major.generation_id = 71;
    first_hash_major.source_identity[0] = std::byte{0x11};
    first_hash_major.source_content_crc64 = 91;
    first_hash_major.storage_contract_digest[0] = std::byte{0x22};
    first_hash_major.directory_rows = 2;
    first_hash_major.posting_ranges_delivered = 3;
    first_hash_major.posting_range_crc_receipts = 1;
    first_hash_major.posting_semantic_receipts = 3;
    first_hash_major.directory_peak_bytes = 100;
    first_hash_major.control_peak_bytes = 300;
    first_hash_major.posting_peak_bytes = 200;
    first_hash_major.peak_admitted_bytes = 300;
    first_hash_major.owner_bytes_available = true;
    first_hash_major.retained_owner_bytes = 80;
    first_hash_major.admitted_bytes = 80;

    pipeline_operational_stats second = first;
    auto& second_hash_major = second.hash_major_index_reader;
    second_hash_major.directory_rows = 4;
    second_hash_major.posting_ranges_delivered = 5;
    second_hash_major.posting_range_crc_receipts = 2;
    second_hash_major.posting_semantic_receipts = 5;
    second_hash_major.directory_peak_bytes = 500;
    second_hash_major.control_peak_bytes = 250;
    second_hash_major.posting_peak_bytes = 450;
    second_hash_major.peak_admitted_bytes = 500;

    pipeline_operational_stats aggregate{};
    aggregate.merge(first);
    aggregate.merge(second);
    const auto& merged = aggregate.hash_major_index_reader;
    REQUIRE(merged.sessions == 2);
    REQUIRE(merged.directory_rows == 6);
    REQUIRE(merged.posting_ranges_delivered == 8);
    REQUIRE(merged.posting_range_crc_receipts == 3);
    REQUIRE(merged.posting_semantic_receipts == 8);
    REQUIRE(merged.directory_peak_bytes == 500);
    REQUIRE(merged.control_peak_bytes == 300);
    REQUIRE(merged.posting_peak_bytes == 450);
    REQUIRE(merged.peak_admitted_bytes == 500);
    REQUIRE(merged.retained_owner_bytes == 80);
    REQUIRE(merged.admitted_bytes == 80);
    REQUIRE_FALSE(aggregate.has_operational_identity_conflict);
    REQUIRE_FALSE(aggregate.are_stats_saturated);

    pipeline_operational_stats mismatched_owner = first;
    mismatched_owner.hash_major_index_reader.retained_owner_bytes = 81;
    aggregate.merge(mismatched_owner);
    REQUIRE(aggregate.has_operational_identity_conflict);

    pipeline_operational_stats at_limit{};
    at_limit.hash_major_index_reader.directory_rows =
        k_sql_bigint_value_max;
    pipeline_operational_stats overflow{};
    overflow.hash_major_index_reader.directory_rows = 1;
    at_limit.merge(overflow);
    REQUIRE(
        at_limit.hash_major_index_reader.directory_rows
        == k_sql_bigint_value_max);
    REQUIRE(at_limit.hash_major_index_reader.is_saturated);
    REQUIRE(at_limit.are_stats_saturated);
}

TEST_CASE("aggregate environment drift reclassifies both job sets",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    const auto aggregate_job = [](const pipeline_job_execution_stats& job) {
        pipeline_execution_stats stats{};
        stats.merge_job(job);
        return stats;
    };
    const auto require_reclassified = [](const pipeline_execution_stats& stats) {
        REQUIRE(stats.environment.is_mixed_invalid);
        REQUIRE(stats.attempted_alignment_jobs == 2);
        REQUIRE(stats.fully_attested_alignment_jobs == 0);
        REQUIRE(stats.unattested_alignment_jobs == 2);
    };

    auto worker_left_job = make_attested_job(
        executed_backend::simd_cpu, target_index_source::recomputed);
    auto worker_right_job = worker_left_job;
    worker_right_job.environment.effective_cpu_workers = 3;
    auto worker_left = aggregate_job(worker_left_job);
    worker_left.merge(aggregate_job(worker_right_job));
    require_reclassified(worker_left);

    auto device_left_job = make_attested_job(
        executed_backend::vulkan_gpu, target_index_source::persisted);
    auto device_right_job = device_left_job;
    device_right_job.environment.gpu_device_uuid[1] = 0x7f;
    auto device_left = aggregate_job(device_left_job);
    device_left.merge(aggregate_job(device_right_job));
    require_reclassified(device_left);

    auto cache_right_job = device_left_job;
    cache_right_job.environment.gpu_pipeline_cache_seed_sha256[1] = 0x55;
    auto cache_left = aggregate_job(device_left_job);
    cache_left.merge(aggregate_job(cache_right_job));
    require_reclassified(cache_left);
}

TEST_CASE("per-job coverage does not multiply operational events",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    pipeline_execution_stats aggregate{};
    auto job = make_attested_job(
        executed_backend::vulkan_gpu, target_index_source::persisted);
    for (std::uint64_t i = 0; i < 4096; ++i) {
        job.logical_target_row = i;
        job.query_ordinal = i;
        aggregate.merge_job(job);
    }
    REQUIRE(aggregate.operational.target_rows == 0);

    pipeline_operational_stats once{};
    once.target_rows = 1;
    once.gpu_queue_submits = 1;
    once.gpu_persisted_resident_uploads = 1;
    aggregate.merge_operational(once);

    REQUIRE(aggregate.attempted_alignment_jobs == 4096);
    REQUIRE(aggregate.fully_attested_alignment_jobs == 4096);
    REQUIRE(aggregate.unattested_alignment_jobs == 0);
    REQUIRE(aggregate.first_unattested_target_row == k_unset_job_ordinal);
    REQUIRE(aggregate.stages.coverage[stage_index(pipeline_stage::query_sketch)].eligible_jobs
            == 4096);
    REQUIRE(aggregate.stages.coverage[stage_index(pipeline_stage::query_sketch)]
                .executed_jobs[engine_index(execution_engine::vulkan_gpu)]
            == 4096);
    REQUIRE(aggregate.operational.gpu_queue_submits == 1);
    REQUIRE(aggregate.operational.gpu_persisted_resident_uploads == 1);
    REQUIRE(aggregate.operational.target_rows == 1);
    REQUIRE_FALSE(aggregate.operational.are_stats_saturated);
}

TEST_CASE("dispatch primary and extra-query aggregates merge once",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    const auto incomplete_invocation = [](
            const std::uint64_t query_ordinal,
            const std::uint64_t seed_dispatches,
            const bool primary_invocation) {
        pipeline_execution_stats invocation{};
        pipeline_job_execution_stats job{};
        job.requested_backend = executed_backend::vulkan_gpu;
        job.actual_backend = executed_backend::mixed_invalid;
        job.requested_target_index_source = target_index_source::recomputed;
        job.actual_target_index_source = target_index_source::recomputed;
        job.logical_target_row = 41;
        job.query_ordinal = query_ordinal;
        invocation.merge_job(job);
        if (primary_invocation) {
            invocation.operational.target_rows = 1;
            invocation.operational.recomputed_target_index_rows = 1;
        }
        invocation.operational.gpu_seed_dispatches = seed_dispatches;
        return invocation;
    };

    pipeline_execution_stats completed_buffer{};
    completed_buffer.merge(incomplete_invocation(0, 1, true));
    completed_buffer.merge(incomplete_invocation(1, 2, false));

    REQUIRE(completed_buffer.requested_backend == executed_backend::vulkan_gpu);
    REQUIRE(completed_buffer.actual_backend == executed_backend::mixed_invalid);
    REQUIRE(completed_buffer.requested_target_index_source
            == target_index_source::recomputed);
    REQUIRE(completed_buffer.actual_target_index_source
            == target_index_source::recomputed);
    REQUIRE(completed_buffer.attempted_alignment_jobs == 2);
    REQUIRE(completed_buffer.fully_attested_alignment_jobs == 0);
    REQUIRE(completed_buffer.unattested_alignment_jobs == 2);
    REQUIRE(completed_buffer.first_unattested_target_row == 41);
    REQUIRE(completed_buffer.first_unattested_query_ordinal == 0);
    REQUIRE(completed_buffer.operational.target_rows == 1);
    REQUIRE(completed_buffer.operational.persisted_index_rows == 0);
    REQUIRE(completed_buffer.operational.recomputed_target_index_rows == 1);
    REQUIRE(completed_buffer.operational.gpu_seed_dispatches == 3);
    REQUIRE(completed_buffer.environment.is_mixed_invalid);
    REQUIRE_FALSE(completed_buffer.operational.are_stats_saturated);
}

TEST_CASE("persisted request cannot be laundered by recompute outcome",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    pipeline_execution_stats invocation{};
    pipeline_job_execution_stats job{};
    job.requested_backend = executed_backend::simd_cpu;
    job.actual_backend = executed_backend::mixed_invalid;
    job.requested_simd_chaining_policy =
        simd_chaining_policy_identity::bounded_batch_approximate;
    job.observed_simd_chaining_policy = simd_chaining_policy_identity::none;
    job.requested_target_index_source = target_index_source::persisted;
    job.actual_target_index_source = target_index_source::recomputed;
    job.logical_target_row = 19;
    job.query_ordinal = 3;
    invocation.merge_job(job);

    pipeline_operational_stats operational{};
    operational.target_rows = 1;
    operational.missing_persisted_index_rows = 1;
    operational.recomputed_target_index_rows = 1;
    invocation.merge_operational(operational);

    REQUIRE(invocation.requested_target_index_source
            == target_index_source::persisted);
    REQUIRE(invocation.actual_target_index_source
            == target_index_source::recomputed);
    REQUIRE(invocation.attempted_alignment_jobs == 1);
    REQUIRE(invocation.fully_attested_alignment_jobs == 0);
    REQUIRE(invocation.unattested_alignment_jobs == 1);
    REQUIRE(invocation.first_unattested_target_row == 19);
    REQUIRE(invocation.first_unattested_query_ordinal == 3);
    REQUIRE(invocation.operational.target_rows == 1);
    REQUIRE(invocation.operational.persisted_index_rows == 0);
    REQUIRE(invocation.operational.missing_persisted_index_rows == 1);
    REQUIRE(invocation.operational.recomputed_target_index_rows == 1);
    REQUIRE_FALSE(invocation.has_mixed_persisted_identity);
}

TEST_CASE("logical target identity survives payload coalescing",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using bioseq::pipeline_stats::count_unique_logical_target_rows;

    // The GPU resident-target layer may coalesce identical sequence bytes,
    // but two distinct relation row identities must still count twice.
    std::array<std::uint64_t, 2> identical_payload_distinct_rows{41, 42};
    REQUIRE(count_unique_logical_target_rows(
                std::span<std::uint64_t>{identical_payload_distinct_rows})
            == 2);

    // Re-observing the same logical row is the only case that collapses.
    std::array<std::uint64_t, 2> repeated_logical_row{41, 41};
    REQUIRE(count_unique_logical_target_rows(
                std::span<std::uint64_t>{repeated_logical_row})
            == 1);
}
