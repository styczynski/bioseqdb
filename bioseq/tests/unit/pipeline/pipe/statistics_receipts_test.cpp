/**
 * @file statistics_receipts_test.cpp
 * @brief Exercises pipeline statistics and receipts.
 */
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/catch_test_macros.hpp>

import std;
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
