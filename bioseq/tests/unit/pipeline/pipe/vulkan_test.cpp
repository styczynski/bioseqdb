/**
 * @file vulkan_test.cpp
 * @brief Exercises Vulkan pipeline routes.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

import std;
import bioseq.tests.unit.pipeline.pipe_support;

using namespace bioseq::tests::pipeline::pipe_support;

namespace bioseq::simd::pipe {

using namespace ::bioseq::tests::pipeline::pipe_support;

TEST_CASE("canonical Vulkan selection separates capability from a complete all-GPU tuple",
          "[component:bioseq][layer:unit][feature:planner]") {
    using ::bioseq::simd::chaining_algorithm;
    using ::bioseq::simd::minimap2_parameters;
    using ::bioseq::simd::minimizer_algorithm;
    using ::bioseq::simd::pipe::complete_all_gpu_tuple_selected;
    using ::bioseq::simd::pipe::vulkan_accelerator_requested;
    using ::bioseq::simd::pipe::vulkan_components_requested;

    minimap2_parameters params{};
    params.vulkan_enabled = true;
    params.vulkan_index_cache_enabled = true;
    REQUIRE_FALSE(vulkan_components_requested(params));
    REQUIRE_FALSE(vulkan_accelerator_requested(params));
    REQUIRE_FALSE(complete_all_gpu_tuple_selected(params));

    params.prefilter_enabled = true;
    params.prefilter_backend = bioseq::prefilter::prefilter_backend::vulkan;
    REQUIRE_FALSE(vulkan_components_requested(params));
    REQUIRE(vulkan_accelerator_requested(params));
    REQUIRE_FALSE(complete_all_gpu_tuple_selected(params));
    params.prefilter_enabled = false;

    params.minimizer_algorithm = minimizer_algorithm::vulkan_minimap_integer;
    REQUIRE(vulkan_components_requested(params));
    REQUIRE_FALSE(complete_all_gpu_tuple_selected(params));

    params.chaining_algorithm = chaining_algorithm::vulkan_tiled_dynamic_programming;
    params.vulkan_seed_hits_enabled = true;
    params.vulkan_fused_frontend_enabled = true;
    params.vulkan_gpu_index_build_enabled = true;
    params.vulkan_extend_enabled = true;
    params.compute_divergence = false;
    params.compute_mapq = false;
    params.max_occ = 500;
    params.kmer_size = 15;
    params.window_size = 10;
    REQUIRE(complete_all_gpu_tuple_selected(params));

    params.vulkan_enabled = false;
    REQUIRE(vulkan_components_requested(params));
    REQUIRE_FALSE(complete_all_gpu_tuple_selected(params));
    params.vulkan_enabled = true;

    params.vulkan_extend_enabled = false;
    params.do_cigar = true;
    REQUIRE_FALSE(complete_all_gpu_tuple_selected(params));
    params.vulkan_extend_enabled = true;
    REQUIRE(complete_all_gpu_tuple_selected(params));

    params.compute_mapq = true;
    // Vulkan materializes its documented constant MAPQ without executing or
    // falling back to the generic CPU/SIMD MAPQ stage.
    REQUIRE(complete_all_gpu_tuple_selected(params));
    params.compute_mapq = false;
    params.base_alignment_algorithm = ::bioseq::simd::base_alignment_algorithm::ksw2;
    REQUIRE_FALSE(complete_all_gpu_tuple_selected(params));
}

TEST_CASE("persisted grouped-SoA GPU preparation admits zero-based supported alphabets",
          "[component:bioseq][layer:unit][feature:planner]") {
    using ::bioseq::simd::base_alignment_algorithm;
    using ::bioseq::simd::chaining_algorithm;
    using ::bioseq::simd::minimap2_parameters;
    using ::bioseq::simd::minimizer_algorithm;
    using ::bioseq::simd::minimizer_sketch_backend;
    using ::bioseq::simd::pipe::is_persisted_grouped_soa_gpu_preparation_tuple_valid;

    constexpr std::size_t supported_alphabet_count = 2u;
    constexpr std::uint8_t dna4_code = 0u;
    constexpr std::uint8_t dna5_code = 1u;

    minimap2_parameters params{};
    params.vulkan_enabled = true;
    params.vulkan_seed_hits_enabled = true;
    params.vulkan_fused_frontend_enabled = true;
    params.vulkan_gpu_index_build_enabled = true;
    params.vulkan_extend_enabled = true;
    params.minimizer_algorithm = minimizer_algorithm::vulkan_minimap_integer;
    params.chaining_algorithm = chaining_algorithm::vulkan_tiled_dynamic_programming;
    params.base_alignment_algorithm = base_alignment_algorithm::block_aligner;
    params.compute_divergence = false;
    params.compute_mapq = false;
    params.max_occ = 500;

    const auto admitted = [supported_alphabet_count,
                           &params](const std::uint8_t alphabet_code,
                                    const std::uint8_t byte_width,
                                    const minimizer_sketch_backend backend =
                                        minimizer_sketch_backend::vulkan_segment) {
        return is_persisted_grouped_soa_gpu_preparation_tuple_valid(
            alphabet_code, supported_alphabet_count, byte_width, backend, params);
    };

    REQUIRE(admitted(dna4_code, 1u));
    REQUIRE(admitted(dna4_code, 2u));
    REQUIRE(admitted(dna5_code, 4u));
    REQUIRE(admitted(dna5_code, 8u));
    REQUIRE_FALSE(is_persisted_grouped_soa_gpu_preparation_tuple_valid(
        dna4_code, 0u, 1u, minimizer_sketch_backend::vulkan_segment, params));
    REQUIRE_FALSE(admitted(static_cast<std::uint8_t>(supported_alphabet_count), 1u));
    REQUIRE_FALSE(admitted((std::numeric_limits<std::uint8_t>::max)(), 1u));
    REQUIRE_FALSE(admitted(dna4_code, 0u));
    REQUIRE_FALSE(admitted(dna4_code, 3u));
    REQUIRE_FALSE(admitted(dna4_code, 16u));
    REQUIRE_FALSE(admitted(dna4_code, 1u, minimizer_sketch_backend::scalar));

    params.vulkan_enabled = false;
    REQUIRE_FALSE(admitted(dna4_code, 1u));
    params.vulkan_enabled = true;

    params.kmer_size = 14;
    REQUIRE_FALSE(admitted(dna4_code, 1u));
    params.kmer_size = 15;
    params.window_size = 9;
    REQUIRE_FALSE(admitted(dna4_code, 1u));
}

TEST_CASE("canonical seed jobs include empty jobs and prohibit cross-job chains",
          "[component:bioseq][layer:unit][feature:chaining]") {
    using anchor_type = ::bioseq::chaining::anchor;
    using ::bioseq::simd::base_alignment_algorithm;
    using ::bioseq::simd::chaining_algorithm;
    using ::bioseq::simd::minimap2_parameters;
    using ::bioseq::simd::minimizer_algorithm;
    using minimizer_type = ::bioseq::simd::minimizer_representation<15u, 15u>;

    context::context seed_storage_context;
    context::installed_context_scope seed_storage_install{seed_storage_context};
    canonical_seed_batch seeds{};
    seeds.anchors = ::bioseq::chaining::make_arena_slab<anchor_type>();
    seeds.anchors.reserve(3u);
    seeds.anchors.push_back(anchor_type{
        .ref_position = 17u,
        .query_position = 29u,
        .is_reverse = 0u,
        .query_span = 15u,
        .flags = 0u,
        .segment_id = 0u,
    });
    seeds.anchors.push_back(anchor_type{
        .ref_position = 17u,
        .query_position = 31u,
        .is_reverse = 0u,
        .query_span = 15u,
        .flags = 0u,
        .segment_id = 0u,
    });
    seeds.anchors.push_back(anchor_type{
        .ref_position = 21u,
        .query_position = 37u,
        .is_reverse = 1u,
        .query_span = 15u,
        .flags = 0u,
        .segment_id = 0u,
    });
    seeds.jobs = {
        {0u, 0u, 0u},
        {1u, 0u, 2u},
        {2u, 2u, 1u},
    };
    constexpr std::array<std::uint32_t, 3u> submitted_ordinals{0u, 1u, 2u};
    REQUIRE(canonical_seed_jobs_match_submission(seeds, submitted_ordinals));

    minimap2_parameters params{};
    bind_fixed_highway(params);
    params.window_size = 10;
    params.kmer_size = 15;
    params.base_alignment_algorithm = base_alignment_algorithm::block_aligner;
    params.chaining_algorithm = chaining_algorithm::scalar_minimap2;
    params.minimizer_algorithm = minimizer_algorithm::scalar;
    params.compute_divergence = false;
    const auto execution_plan = make_statement_execution_plan(std::move(params));

    std::vector<seq::dna4> query_bases(64u);
    std::vector<seq::dna4> target_bases(64u);
    compact_sequence_range<std::vector<seq::dna4>> query_sequence{query_bases};
    compact_sequence_range<std::vector<seq::dna4>> target_sequence{target_bases};
    auto query_owner = bioseq::storage::make_resident_lazy_sequence(query_sequence);
    auto target_owner = bioseq::storage::make_resident_lazy_sequence(target_sequence);
    const auto query_view = bioseq::storage::make_lazy_sequence_view(query_owner);
    const auto target_view = bioseq::storage::make_lazy_sequence_view(target_owner);
    const auto minimizers_owner =
        std::make_shared<const std::vector<minimizer_type>>();

    bioseq::parallelism::memory_admission_controller admission;
    for (std::size_t job_index = 0u; job_index < seeds.jobs.size(); ++job_index) {
        const auto& job = seeds.jobs[job_index];
        CAPTURE(job_index, job.ordinal, job.offset, job.count);
        route_attempt_memory attempt{admission, {}, nullptr};
        context::installed_context_scope attempt_install{attempt.fresh_context};
        match_result_collection output{};
        auto query_minimizers = minimizer_batch_view<minimizer_type>::from_shared_host(
            minimizers_owner, 0u, 0u);

        dispatch_post_seed_job<seq::dna4, minimizer_type>(
            seeds, job_index, job.ordinal, query_view, target_view,
            std::move(query_minimizers), execution_plan, output, attempt);

        REQUIRE(output.scalar_pipeline_stats.chain_stage_completion_calls == 1u);
        REQUIRE(output.scalar_pipeline_stats.chain_stage_completed_anchor_inputs == job.count);
    }
}

TEST_CASE("bounded Vulkan pipe matches complete CPU biological results",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using ::bioseq::simd::base_alignment_algorithm;
    using ::bioseq::simd::chaining_algorithm;
    using ::bioseq::simd::minimap2_parameters;
    using ::bioseq::simd::minimizer_algorithm;
    using ::bioseq::simd::minimizer_sketch_backend;
    using ::bioseq::simd::pipe::exact_simd_range_route;
    using ::bioseq::simd::pipe::minimizer_range_execution_stats;
    using ::bioseq::simd::pipe::minimizer_range_stats_matches_backend;

    auto target = vulkan_pipeline_test_support::make_dna4(4000, 0xC0FFEEu);
    // Query = a forward sub-region of the target (a guaranteed match).
    std::vector<seq::dna4> query(target.begin() + 500, target.begin() + 1300);

    minimap2_parameters cpu;
    cpu.minimizer_algorithm = minimizer_algorithm::two_stack_nthash;
    cpu.chaining_algorithm = chaining_algorithm::simd_minimap2;
    cpu.base_alignment_algorithm = base_alignment_algorithm::block_aligner;
    const auto cpu_res = vulkan_pipeline_test_support::run(query, target, cpu);
    REQUIRE(cpu.do_cigar);
    REQUIRE(cpu_res.is_exact_simd_query_execution_consistent(true));
    REQUIRE(cpu_res.is_exact_simd_target_execution_consistent(true));
    REQUIRE(cpu_res.is_simd_chaining_execution_consistent(cpu.simd_chaining_policy,
                                                          /*require_nonempty_vector_work=*/true));
    REQUIRE(cpu_res.is_block_aligner_execution_consistent(
        /*require_nonempty_simd_work=*/true));
    REQUIRE(cpu_res.scalar_pipeline_execution_stats_available);
    REQUIRE(cpu_res.scalar_pipeline_stats.seed_lookup_calls > 0u);
    REQUIRE(cpu_res.scalar_pipeline_stats.seed_query_minimizers > 0u);
    REQUIRE(cpu_res.partial_vulkan_stats == ::bioseq::simd::partial_vulkan_execution_stats{});
    REQUIRE(cpu_res.is_all_simd_execution_consistent(minimizer_algorithm::two_stack_nthash,
                                                     cpu.simd_chaining_policy,
                                                     /*require_target_sketch=*/true,
                                                     /*require_nonempty_vector_work=*/true));

    minimizer_range_execution_stats exact_range{};
    exact_range.exact_simd = cpu_res.exact_simd_query_execution_stats;
    exact_range.exact_simd_available = true;
    // This parity fixture deliberately uses dna4/u64 (`vulkan_pipeline_test_support::range_t`).
    // The resident vector_stream route is restricted to dna4/u32,
    // so the authentic exact SIMD stats here is the chronological continuation
    // route.  Resident routing has its own u32 test above; relabelling this u64
    // stats as resident would weaken the persisted-scheme boundary.
    REQUIRE(exact_range.exact_simd.resident_vector_stream_kernel_calls == 0u);
    REQUIRE((exact_range.exact_simd.generic_u64_kernel_calls > 0u ||
             exact_range.exact_simd.kmer_15_window_10_byte_8_uint32_hash_kernel_calls > 0u));
    REQUIRE(minimizer_range_stats_matches_backend(minimizer_sketch_backend::exact_simd,
                                                  exact_range,
                                                  /*require_nonempty_vector_work=*/true,
                                                  exact_simd_range_route::require_continuation));
    auto hidden_bounded_payload = exact_range;
    hidden_bounded_payload.bounded_nthash.scheme_id = 1;
    REQUIRE_FALSE(hidden_bounded_payload.bounded_nthash_available);
    REQUIRE_FALSE(
        minimizer_range_stats_matches_backend(minimizer_sketch_backend::exact_simd,
                                              hidden_bounded_payload,
                                              /*require_nonempty_vector_work=*/false,
                                              exact_simd_range_route::require_continuation));
    auto bounded_presence_only = exact_range;
    bounded_presence_only.bounded_nthash_available = true;
    REQUIRE_FALSE(
        minimizer_range_stats_matches_backend(minimizer_sketch_backend::exact_simd,
                                              bounded_presence_only,
                                              /*require_nonempty_vector_work=*/false,
                                              exact_simd_range_route::require_continuation));

    minimap2_parameters gpu = cpu;
    gpu.minimizer_algorithm = minimizer_algorithm::vulkan_minimap_integer;
    gpu.chaining_algorithm = chaining_algorithm::vulkan_tiled_dynamic_programming;
    gpu.vulkan_enabled = true;
    gpu.vulkan_seed_hits_enabled = true;
    if (!::bioseq::chaining::is_vulkan_chaining_supported()) {
        REQUIRE_THROWS_AS(vulkan_pipeline_test_support::run(query, target, gpu),
                          bioseq::gpu::vulkan::vulkan_runtime_error);
        return;
    }
    const auto gpu_res = vulkan_pipeline_test_support::run(query, target, gpu);

    REQUIRE_FALSE(cpu_res.results.empty());
    REQUIRE_FALSE(gpu_res.results.empty());
    REQUIRE(gpu.do_cigar);
    REQUIRE_FALSE(gpu_res.exact_simd_query_execution_stats_available);
    REQUIRE_FALSE(gpu_res.exact_simd_target_execution_stats_available);
    REQUIRE_FALSE(gpu_res.simd_chaining_execution_stats_available);
    REQUIRE_FALSE(gpu_res.scalar_pipeline_execution_stats_available);
    REQUIRE(gpu_res.scalar_pipeline_stats == ::bioseq::simd::scalar_pipeline_execution_stats{});
    REQUIRE(gpu_res.partial_vulkan_stats.is_exact_stage_mask_consistent(
        /*query_sketch=*/true,
        /*target_sketch=*/true,
        /*target_index_build=*/false,
        /*seed_lookup=*/true,
        /*chain_score=*/true));
    REQUIRE(gpu_res.is_block_aligner_execution_consistent(
        /*require_nonempty_simd_work=*/true));
    REQUIRE(vulkan_pipeline_test_support::exact_biological_results_equal(cpu_res, gpu_res));
    REQUIRE(cpu_res.biological_identity() == gpu_res.biological_identity());
}

TEST_CASE("vulkan_minimap_integer with vulkan_enabled=false fails",
          "[component:bioseq][layer:unit][feature:configuration]") {
    using ::bioseq::simd::chaining_algorithm;
    using ::bioseq::simd::minimap2_parameters;
    using ::bioseq::simd::minimizer_algorithm;

    auto target = vulkan_pipeline_test_support::make_dna4(3000, 0x1234567u);
    std::vector<seq::dna4> query(target.begin() + 200, target.begin() + 900);

    minimap2_parameters disabled;
    disabled.minimizer_algorithm = minimizer_algorithm::vulkan_minimap_integer;
    disabled.chaining_algorithm = chaining_algorithm::vulkan_tiled_dynamic_programming;
    disabled.vulkan_enabled = false;

    // Statement-plan validation rejects this selection before runtime entry.
    REQUIRE_THROWS_AS(vulkan_pipeline_test_support::run(query, target, disabled),
                      std::invalid_argument);
}

TEST_CASE("source-direct operation emits an exact two-receipt transaction",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using resident_pipeline_test_support::operation_state_type;
    using resident_pipeline_test_support::resident_authority_type;

    SECTION("reflected authority") {
        auto state = resident_pipeline_test_support::full_resident_state(
            resident_authority_type::reflected_process_footprint);
        REQUIRE(state.route_name() == "full_sorted_dense_direct_mapped");
        REQUIRE(state.planning_receipts == 1u);
        REQUIRE(state.terminal_success_invariants());
        state.log_terminal("success", "focused_success");
        REQUIRE(state.terminal_receipts == 1u);
        REQUIRE(state.terminal_success);
        state.log_terminal("failure", "must_not_duplicate");
        REQUIRE(state.terminal_receipts == 1u);
        REQUIRE(state.terminal_success);
    }

    SECTION("retained exact permit authority") {
        auto state = resident_pipeline_test_support::full_resident_state(
            resident_authority_type::retained_exact_permit);
        REQUIRE(state.terminal_success_invariants());
    }

    SECTION("zero anchors still complete two mapped-prefix passes") {
        auto state = resident_pipeline_test_support::full_resident_state(
            resident_authority_type::retained_exact_permit);
        state.slab_requested_bytes = 0u;
        state.slab_actual_bytes = 0u;
        state.slab_retained_permit_bytes = 0u;
        state.slab_permit_releases = 0u;
        state.seed_reserved_bytes = 0u;
        state.seed_planned_vulkan_bytes = 0u;
        state.seed_actual_vulkan_bytes = 0u;
        state.zero_anchor_bypasses = 1u;
        state.seed_dispatches = 0u;
        state.seed_fence_completions = 0u;
        REQUIRE(state.mapped_prefix_passes == 2u);
        REQUIRE(state.terminal_success_invariants());
        --state.mapped_prefix_passes;
        REQUIRE_FALSE(state.terminal_success_invariants());
    }

    SECTION("zero query is an explicit post-publication pipeline bypass") {
        auto state = resident_pipeline_test_support::full_resident_state(
            resident_authority_type::reflected_process_footprint);
        state.expected_seed_consumers = 0u;
        state.completed_seed_consumers = 0u;
        state.cache_required_lookups = 0u;
        state.cache_required_hits = 0u;
        state.mapped_prefix_passes = 0u;
        state.mapped_prefix_lookups = 0u;
        state.mapped_prefix_comparisons = 0u;
        state.slab_requested_bytes = 0u;
        state.slab_actual_bytes = 0u;
        state.slab_retained_permit_bytes = 0u;
        state.slab_permit_releases = 0u;
        state.seed_reserved_bytes = 0u;
        state.seed_planned_vulkan_bytes = 0u;
        state.seed_actual_vulkan_bytes = 0u;
        state.seed_roundtrips = 0u;
        state.seed_dispatches = 0u;
        state.seed_fence_completions = 0u;
        state.zero_query_bypasses = 1u;
        state.cache_lookup_started = false;
        state.mapped_prefix_started = false;
        state.slab_admission_started = false;
        state.seed_dispatch_started = false;
        state.pipeline_started = false;
        state.pipeline_completed = false;
        REQUIRE(state.terminal_success_invariants());
    }

    SECTION("canonical empty publishes neither resident nor fake cleanup") {
        auto state = resident_pipeline_test_support::canonical_empty_state();
        REQUIRE(state.route_name() == "empty_no_resident_zero_anchor");
        REQUIRE(state.terminal_success_invariants());
        state.resident_published = true;
        REQUIRE_FALSE(state.terminal_success_invariants());
    }

    SECTION("failure and not-reached unwind remain exactly two receipts") {
        auto invalid = resident_pipeline_test_support::full_resident_state(
            resident_authority_type::reflected_process_footprint);
        --invalid.materialization.fully_touched_bytes;
        REQUIRE_FALSE(invalid.terminal_success_invariants());
        invalid.log_terminal("success", "injected_touch_failure");
        REQUIRE(invalid.terminal_receipts == 1u);
        REQUIRE_FALSE(invalid.terminal_success);

        operation_state_type not_reached{};
        not_reached.operation_identity = 19u;
        {
            ::bioseq::simd::pipe::direct_resident_receipt_guard guard{&not_reached};
        }
        REQUIRE(not_reached.planning_receipts == 1u);
        REQUIRE(not_reached.terminal_receipts == 1u);
        REQUIRE_FALSE(not_reached.terminal_success);
    }
}

TEST_CASE("bounded real-GPU prepared resident matches independent ordered sketch",
          "[component:bioseq][layer:unit][feature:pipeline][resource:gpu]") {
    namespace vk = bioseq::gpu::vulkan;
    using ::bioseq::simd::base_alignment_algorithm;
    using ::bioseq::simd::chaining_algorithm;
    using ::bioseq::simd::match_result_collection;
    using ::bioseq::simd::minimap2_parameters;
    using ::bioseq::simd::minimizer_algorithm;
    using direct_minimizer_type = ::bioseq::simd::minimizer_representation<33u, 15u>;

    auto& runtime = vk::runtime::instance();
    if (!vk::hardware_device_available(runtime)) {
        SUCCEED("hardware Vulkan unavailable; prepared-resident parity skipped");
        return;
    }

    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    const std::uint64_t baseline = admission.reserved_bytes();
    const std::uint64_t pipeline_cache_metadata_baseline =
        runtime.pipeline_cache_metadata_reserved_bytes();
    struct restore_cap {
        bioseq::parallelism::memory_admission_controller& controller;
        std::uint64_t cap = 0u;
        ~restore_cap() {
            controller.set_index_memory_cap_bytes(cap);
        }
    } restore{admission, previous_cap};
    admission.set_index_memory_cap_bytes(std::max<std::uint64_t>(
        previous_cap, bioseq::common::saturating_add(baseline, std::uint64_t{512} << 20u)));

    match_result_collection direct_results;
    match_result_collection ordered_results;
    match_result_collection empty_minimizer_results;
    {
        auto target_letters = vulkan_pipeline_test_support::make_dna4(20'000u, 0x0384c0ffeeULL);
        std::vector<seq::dna4> query_letters(target_letters.begin() + 5'000,
                                             target_letters.begin() + 6'500);
        vulkan_pipeline_test_support::range_t query_range(query_letters);
        vulkan_pipeline_test_support::range_t target_range(target_letters);
        auto gpu_query_owner = bioseq::storage::make_resident_lazy_sequence(query_range);
        auto gpu_target_owner = bioseq::storage::make_resident_lazy_sequence(target_range);
        const auto gpu_query_view = bioseq::storage::make_lazy_sequence_view(gpu_query_owner);
        const auto gpu_target_view = bioseq::storage::make_lazy_sequence_view(gpu_target_owner);

        auto target_sketch = vk::collect_minimizers_vulkan_result<direct_minimizer_type>(
            gpu_target_view,
            /*window_size=*/10u,
            /*kmer_size=*/15u,
            nullptr,
            vk::vulkan_sketch_host_ownership_route::fixed_zero_copy_adoption);
        REQUIRE(target_sketch.has_value());
        REQUIRE(target_sketch->dispatch_token.is_internally_consistent());
        REQUIRE_FALSE(target_sketch->host_minimizers.empty());
        auto target_owner =
            ::bioseq::simd::pipe_entry_detail::adopt_fixed_vulkan_sketch_host_result(
                std::move(*target_sketch));
        REQUIRE(target_owner.has_valid_ownership_authorization());

        auto query_sketch =
            vk::collect_minimizers_vulkan_result<direct_minimizer_type>(gpu_query_view,
                                                                        /*window_size=*/10u,
                                                                        /*kmer_size=*/15u);
        REQUIRE(query_sketch.has_value());
        REQUIRE(query_sketch->dispatch_token.is_internally_consistent());
        REQUIRE_FALSE(query_sketch->host_minimizers.empty());

        minimap2_parameters direct_params{};
        bind_fixed_highway(direct_params);
        direct_params.chaining_algorithm = chaining_algorithm::vulkan_tiled_dynamic_programming;
        direct_params.base_alignment_algorithm = base_alignment_algorithm::block_aligner;
        direct_params.minimizer_algorithm = minimizer_algorithm::vulkan_minimap_integer;
        direct_params.vulkan_enabled = true;
        direct_params.vulkan_seed_hits_enabled = true;
        direct_params.vulkan_index_cache_enabled = true;
        direct_params.vulkan_fused_frontend_enabled = false;
        direct_params.vulkan_gpu_index_build_enabled = false;
        direct_params.vulkan_extend_enabled = false;
        direct_params.do_cigar = true;

        auto direct_lazy_query = bioseq::storage::make_resident_lazy_sequence(query_range);
        auto direct_lazy_target = bioseq::storage::make_resident_lazy_sequence(target_range);
        const auto direct_query_view = bioseq::storage::make_lazy_sequence_view(direct_lazy_query);
        const auto direct_target_view =
            bioseq::storage::make_lazy_sequence_view(direct_lazy_target);
        const auto direct_query_minimizer_count = query_sketch->host_minimizers.size();
        auto direct_query_minimizers =
            minimizer_batch_view<direct_minimizer_type>::from_shared_host(
                std::make_shared<const std::vector<direct_minimizer_type>>(
                    std::move(query_sketch->host_minimizers)),
                0u, direct_query_minimizer_count);
        direct_results =
            ::bioseq::simd::pipe_entry_detail::minimap2_port_with_fixed_vulkan_target_handoff(
                direct_query_view,
                direct_target_view,
                std::move(direct_query_minimizers),
                std::move(target_owner),
                make_statement_execution_plan(direct_params));
        query_sketch.reset();
        target_sketch.reset();

        minimap2_parameters ordered_params = direct_params;
        ordered_params.vulkan_fused_frontend_enabled = true;
        ordered_params.vulkan_gpu_index_build_enabled = true;
        auto ordered_lazy_query = bioseq::storage::make_resident_lazy_sequence(query_range);
        auto ordered_lazy_target = bioseq::storage::make_resident_lazy_sequence(target_range);
        const auto ordered_query_view =
            bioseq::storage::make_lazy_sequence_view(ordered_lazy_query);
        const auto ordered_target_view =
            bioseq::storage::make_lazy_sequence_view(ordered_lazy_target);
        ordered_results =
            ::bioseq::simd::minimap_port(ordered_query_view, ordered_target_view, ordered_params);

        // A nonempty query shorter than k has no minimizers, but the already
        // materialized source-direct resident still owns the two required
        // mapped-payload authority passes. Rebuild the small bounded resident
        // and exercise the production wrapper so the generic early-empty seed
        // shortcut cannot regress over this case.
        std::vector<seq::dna4> short_query_letters(target_letters.begin(),
                                                   target_letters.begin() + 8);
        vulkan_pipeline_test_support::range_t short_query_range(short_query_letters);
        auto short_query_owner = bioseq::storage::make_resident_lazy_sequence(short_query_range);
        const auto short_query_view = bioseq::storage::make_lazy_sequence_view(short_query_owner);
        auto empty_query_sketch =
            vk::collect_minimizers_vulkan_result<direct_minimizer_type>(short_query_view,
                                                                        /*window_size=*/10u,
                                                                        /*kmer_size=*/15u);
        REQUIRE(empty_query_sketch.has_value());
        REQUIRE(empty_query_sketch->dispatch_token.is_internally_consistent());
        REQUIRE(empty_query_sketch->host_minimizers.empty());
        auto second_target_sketch = vk::collect_minimizers_vulkan_result<direct_minimizer_type>(
            gpu_target_view,
            /*window_size=*/10u,
            /*kmer_size=*/15u,
            nullptr,
            vk::vulkan_sketch_host_ownership_route::fixed_zero_copy_adoption);
        REQUIRE(second_target_sketch.has_value());
        REQUIRE(second_target_sketch->dispatch_token.is_internally_consistent());
        auto second_target_owner =
            ::bioseq::simd::pipe_entry_detail::adopt_fixed_vulkan_sketch_host_result(
                std::move(*second_target_sketch));
        auto empty_lazy_query = bioseq::storage::make_resident_lazy_sequence(short_query_range);
        auto empty_lazy_target = bioseq::storage::make_resident_lazy_sequence(target_range);
        const auto empty_query_view = bioseq::storage::make_lazy_sequence_view(empty_lazy_query);
        const auto empty_target_view = bioseq::storage::make_lazy_sequence_view(empty_lazy_target);
        const auto empty_query_minimizer_count = empty_query_sketch->host_minimizers.size();
        auto empty_query_minimizers =
            minimizer_batch_view<direct_minimizer_type>::from_shared_host(
                std::make_shared<const std::vector<direct_minimizer_type>>(
                    std::move(empty_query_sketch->host_minimizers)),
                0u, empty_query_minimizer_count);
        empty_minimizer_results =
            ::bioseq::simd::pipe_entry_detail::minimap2_port_with_fixed_vulkan_target_handoff(
                empty_query_view,
                empty_target_view,
                std::move(empty_query_minimizers),
                std::move(second_target_owner),
                make_statement_execution_plan(direct_params));
    }

    REQUIRE_FALSE(direct_results.results.empty());
    REQUIRE_FALSE(ordered_results.results.empty());
    const auto& direct = direct_results.partial_vulkan_stats;
    const auto& ordered_sketch = ordered_results.partial_vulkan_stats;
    REQUIRE_FALSE(direct.is_saturated);
    REQUIRE(direct.gpu_index_source_direct_operations == 1u);
    REQUIRE(direct.gpu_index_source_direct_terminal_successes == 1u);
    REQUIRE(direct.gpu_index_source_direct_planning_receipts == 1u);
    REQUIRE(direct.gpu_index_source_direct_terminal_receipts == 1u);
    REQUIRE(direct.gpu_index_source_direct_materializations == 1u);
    REQUIRE(direct.gpu_index_source_direct_resident_publications == 1u);
    REQUIRE(direct.gpu_index_source_direct_source_releases == 1u);
    REQUIRE(direct.gpu_index_source_direct_checksum_matches == 1u);
    REQUIRE(direct.gpu_index_source_direct_source_traversals == 1u);
    REQUIRE(direct.gpu_index_source_direct_mapped_traversals == 1u);
    REQUIRE(direct.gpu_index_source_direct_cache_preinsertions == 1u);
    REQUIRE(direct.gpu_index_source_direct_required_misses == 0u);
    REQUIRE(direct.gpu_index_source_direct_fallback_attempts == 0u);
    REQUIRE(direct.gpu_index_source_direct_expected_seed_consumers > 0u);
    REQUIRE(direct.gpu_index_source_direct_completed_seed_consumers ==
            direct.gpu_index_source_direct_expected_seed_consumers);
    REQUIRE(direct.gpu_index_source_direct_required_lookups ==
            direct.gpu_index_source_direct_completed_seed_consumers);
    REQUIRE(direct.gpu_index_source_direct_required_hits ==
            direct.gpu_index_source_direct_required_lookups);
    REQUIRE(direct.gpu_index_source_direct_mapped_prefix_passes ==
            2u * direct.gpu_index_source_direct_required_lookups);
    REQUIRE(direct.gpu_index_source_direct_seed_slab_initial_capacity == 0u);
    REQUIRE(direct.gpu_index_source_direct_seed_planned_vulkan_bytes ==
            direct.gpu_index_source_direct_seed_actual_vulkan_bytes);
    REQUIRE(direct.gpu_index_source_direct_seed_roundtrips ==
            direct.gpu_index_source_direct_required_lookups);
    REQUIRE(direct.gpu_index_source_direct_seed_fence_completions ==
            direct.gpu_index_source_direct_seed_roundtrips);
    REQUIRE(direct.gpu_index_source_direct_cache_cleanup_releases == 1u);
    REQUIRE(direct.gpu_index_source_direct_resident_cleanup_bytes ==
            direct.gpu_index_actual_allocation_bytes);
    REQUIRE(direct.gpu_index_source_direct_pipeline_completions == 1u);
    REQUIRE(direct.gpu_index_fully_touched_bytes == direct.gpu_index_actual_allocation_bytes);
    REQUIRE(direct.target_index_build_dispatches == 0u);
    REQUIRE(direct.seed_host_sort_fallbacks == 0u);
    REQUIRE(direct.post_seed_owned_cpu_index_releases == 0u);
    REQUIRE(direct.post_seed_owned_cpu_position_bytes_released == 0u);

    REQUIRE(direct.seed_anchor_stats_operations == direct.gpu_index_source_direct_seed_roundtrips);
    REQUIRE(direct.seed_canonical_anchor_order_checks == direct.seed_anchor_stats_operations);
    REQUIRE(direct.seed_canonical_anchor_order_accepts ==
            direct.seed_canonical_anchor_order_checks);
    REQUIRE(direct.seed_canonical_anchor_count == ordered_sketch.seed_canonical_anchor_count);
    REQUIRE(direct.seed_anchor_digest_lane0 == ordered_sketch.seed_anchor_digest_lane0);
    REQUIRE(direct.seed_anchor_digest_lane1 == ordered_sketch.seed_anchor_digest_lane1);
    REQUIRE(vulkan_pipeline_test_support::exact_biological_results_equal(direct_results,
                                                                         ordered_results));
    REQUIRE(direct_results.biological_identity() == ordered_results.biological_identity());

    REQUIRE(empty_minimizer_results.results.empty());
    const auto& empty_minimizer = empty_minimizer_results.partial_vulkan_stats;
    REQUIRE(empty_minimizer.gpu_index_source_direct_operations == 1u);
    REQUIRE(empty_minimizer.gpu_index_source_direct_terminal_successes == 1u);
    REQUIRE(empty_minimizer.gpu_index_source_direct_required_lookups == 1u);
    REQUIRE(empty_minimizer.gpu_index_source_direct_required_hits == 1u);
    REQUIRE(empty_minimizer.gpu_index_source_direct_mapped_prefix_passes == 2u);
    REQUIRE(empty_minimizer.gpu_index_source_direct_mapped_prefix_lookups == 0u);
    REQUIRE(empty_minimizer.gpu_index_source_direct_mapped_prefix_comparisons == 0u);
    REQUIRE(empty_minimizer.gpu_index_source_direct_seed_roundtrips == 1u);
    REQUIRE(empty_minimizer.gpu_index_source_direct_zero_anchor_bypasses == 1u);
    REQUIRE(empty_minimizer.gpu_index_source_direct_seed_fence_completions == 0u);
    REQUIRE(empty_minimizer.seed_lookup_dispatches == 0u);
    REQUIRE(empty_minimizer.seed_anchor_stats_operations == 1u);
    REQUIRE(empty_minimizer.seed_canonical_anchor_count == 0u);
    REQUIRE(empty_minimizer.seed_anchor_digest_lane0 == vk::k_seed_anchor_digest_seed_low);
    REQUIRE(empty_minimizer.seed_anchor_digest_lane1 == vk::k_seed_anchor_digest_seed_high);
    REQUIRE(empty_minimizer.gpu_index_source_direct_pipeline_completions == 1u);

    const std::uint64_t pipeline_cache_metadata_after =
        runtime.pipeline_cache_metadata_reserved_bytes();
    REQUIRE(pipeline_cache_metadata_after >= pipeline_cache_metadata_baseline);
    REQUIRE(admission.reserved_bytes() ==
            bioseq::common::saturating_add(
                baseline, pipeline_cache_metadata_after - pipeline_cache_metadata_baseline));
}

TEST_CASE("post-seed owned lifetime releases only after the final seed user",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using sketch_type = ::bioseq::simd::minimap_sketch_data;
    std::vector<sketch_type> sketches{
        sketch_type{/*kmer*/ 7, /*pos*/ 1, /*strand*/ 0},
        sketch_type{/*kmer*/ 7, /*pos*/ 3, /*strand*/ 1},
        sketch_type{/*kmer*/ 9, /*pos*/ 5, /*strand*/ 0},
    };
    bioseq::indexing::minimizer_hashmap_fragment_index index;
    index.build(sketches, 100);
    const std::uint64_t position_capacity_bytes =
        index.positions.capacity() * sizeof(std::uint64_t);
    REQUIRE(position_capacity_bytes != 0u);

    post_seed_owned_lifetime lifetime{
        /*seed_users=*/2u,
        /*owned_gpu_cache=*/nullptr,
        transient_target_index_release_authorization{.index = &index}};
    const auto first = lifetime.complete_seed_user();
    REQUIRE(first.cpu_index_releases == 0u);
    REQUIRE_FALSE(index.table_empty());

    const auto last = lifetime.complete_seed_user();
    REQUIRE(last.cpu_index_releases == 1u);
    REQUIRE(last.cpu_position_bytes_released == position_capacity_bytes);
    REQUIRE(last.gpu_cache_evictions == 0u);
    REQUIRE(index.table_empty());
    REQUIRE(index.positions.capacity() == 0u);
}

TEST_CASE("seed lookup applies max_occ once across every borrowed position span",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using sketch_type = ::bioseq::simd::minimap_sketch_data;
    using ::bioseq::indexing::minimizer_lookup_spans;
    using ::bioseq::indexing::minimizer_position_span;
    struct segmented_seed_index {
        std::array<std::uint64_t, 2> first{
            (std::uint64_t{100} << 1u),
            (std::uint64_t{200} << 1u) | 1u,
        };
        std::array<std::uint64_t, 1> second{
            (std::uint64_t{300} << 1u),
        };
        std::array<minimizer_position_span, 2> spans{
            minimizer_position_span{first},
            minimizer_position_span{second},
        };
        bool positions_available = true;
        std::uint64_t reported_count = 3;

        [[nodiscard]] minimizer_lookup_spans lookup_spans(std::uint64_t hash) const noexcept {
            if (hash != 7u) {
                return minimizer_lookup_spans::segmented({}, /*global_occurrence_count=*/0);
            }
            if (!positions_available) {
                return minimizer_lookup_spans::positions_unavailable(reported_count);
            }
            return minimizer_lookup_spans::segmented(spans, reported_count);
        }

        [[nodiscard]] bioseq::storage::sequence_size_t target_length() const noexcept {
            return 1'000;
        }
    };
    STATIC_REQUIRE(::bioseq::indexing::minimizer_lookup_index<segmented_seed_index>);

    const std::array<sketch_type, 1> query{
        sketch_type{/*kmer=*/7, /*pos=*/10, /*strand=*/0},
    };

    SECTION("all spans emit when their global count is admitted") {
        context::context ctx;
        context::installed_context_scope installed{ctx};
        auto scope = ctx.push<context::arena::chain>();
        auto anchors = ::bioseq::chaining::make_arena_slab<::bioseq::chaining::anchor>();
        segmented_seed_index index;
        ::bioseq::simd::collect_seed_hits(std::span<const sketch_type>{query},
                                          index,
                                          /*query_length=*/100,
                                          /*max_occ=*/3,
                                          /*kmer_size=*/15,
                                          anchors);
        REQUIRE(anchors.size() == 3);
        REQUIRE(anchors.data()[0].ref_position == 100);
        REQUIRE(anchors.data()[1].ref_position == 300);
        REQUIRE(anchors.data()[2].ref_position == 200);
        REQUIRE(anchors.data()[2].is_reverse == 1u);
    }

    SECTION("global count prunes although every available span is individually admissible") {
        context::context ctx;
        context::installed_context_scope installed{ctx};
        auto scope = ctx.push<context::arena::chain>();
        auto anchors = ::bioseq::chaining::make_arena_slab<::bioseq::chaining::anchor>();
        segmented_seed_index index;
        ::bioseq::simd::collect_seed_hits(std::span<const sketch_type>{query},
                                          index,
                                          /*query_length=*/100,
                                          /*max_occ=*/2,
                                          /*kmer_size=*/15,
                                          anchors);
        REQUIRE(anchors.size() == 0);
    }

    SECTION("global pruning does not require deliberately unloaded positions") {
        context::context ctx;
        context::installed_context_scope installed{ctx};
        auto scope = ctx.push<context::arena::chain>();
        auto anchors = ::bioseq::chaining::make_arena_slab<::bioseq::chaining::anchor>();
        segmented_seed_index index;
        index.positions_available = false;
        ::bioseq::simd::collect_seed_hits(std::span<const sketch_type>{query},
                                          index,
                                          /*query_length=*/100,
                                          /*max_occ=*/2,
                                          /*kmer_size=*/15,
                                          anchors);
        REQUIRE(anchors.size() == 0);
    }

    SECTION("a surviving hash without positions fails closed") {
        context::context ctx;
        context::installed_context_scope installed{ctx};
        auto scope = ctx.push<context::arena::chain>();
        auto anchors = ::bioseq::chaining::make_arena_slab<::bioseq::chaining::anchor>();
        segmented_seed_index index;
        index.positions_available = false;
        REQUIRE_THROWS_AS(::bioseq::simd::collect_seed_hits(std::span<const sketch_type>{query},
                                                            index,
                                                            /*query_length=*/100,
                                                            /*max_occ=*/3,
                                                            /*kmer_size=*/15,
                                                            anchors),
                          ::bioseq::indexing::persisted_minimizer_lookup_error);
    }

    SECTION("an admitted malformed global count fails closed") {
        context::context ctx;
        context::installed_context_scope installed{ctx};
        auto scope = ctx.push<context::arena::chain>();
        auto anchors = ::bioseq::chaining::make_arena_slab<::bioseq::chaining::anchor>();
        segmented_seed_index index;
        index.reported_count = 2;
        REQUIRE_THROWS_AS(::bioseq::simd::collect_seed_hits(std::span<const sketch_type>{query},
                                                            index,
                                                            /*query_length=*/100,
                                                            /*max_occ=*/2,
                                                            /*kmer_size=*/15,
                                                            anchors),
                          ::bioseq::indexing::persisted_minimizer_lookup_error);
        REQUIRE(anchors.size() == 0);
    }
}

TEST_CASE("Vulkan chaining mapped plan reports exact GPU and CPU scratch peaks",
          "[component:bioseq][layer:unit][feature:chaining]") {
    const auto plan = ::bioseq::chaining::make_vulkan_chaining_memory_plan(
        /*anchor_count=*/1'520'944u,
        /*range_count=*/32'035u);
    REQUIRE(plan.device_anchor_bytes == 36'502'656u);
    REQUIRE(plan.device_score_bytes == 6'083'776u);
    REQUIRE(plan.device_predecessor_bytes == 12'167'552u);
    REQUIRE(plan.device_range_bytes == 256'280u);
    REQUIRE(plan.host_range_bytes == 256'280u);
    REQUIRE(plan.compacted_host_anchor_bytes == 0u);
    REQUIRE(plan.host_scored_index_bytes == 24'335'104u);
    REQUIRE(plan.host_visited_bytes == 6'083'776u);
    REQUIRE(plan.host_chain_index_bytes == 12'167'552u);
    REQUIRE(plan.coordinate_device_trace_bytes == 0u);
    REQUIRE(plan.coordinate_host_trace_bytes == 0u);
    REQUIRE(plan.gpu_execution_bytes == 55'266'544u);
    REQUIRE(plan.host_backtracking_bytes == 61'094'040u);
    REQUIRE(plan.total_bytes == 61'094'040u);

    bioseq::parallelism::memory_admission_failure failure{
        .reason = bioseq::parallelism::memory_admission_failure_reason::system_pressure,
        .requested_bytes = plan.total_bytes,
        .reserved_bytes = 17u,
        .budget_ceiling_bytes = 19u,
        .allocatable_after_reservations_bytes = 2u,
        .process_footprint_bytes = 23u,
        .system_available_bytes = 29u,
        .system_total_bytes = 31u,
        .process_soft_cap_bytes = 37u,
    };
    const ::bioseq::chaining::vulkan_chaining_admission_error error{plan, failure};
    REQUIRE(error.plan.total_bytes == plan.total_bytes);
    REQUIRE(error.admission.reason ==
            bioseq::parallelism::memory_admission_failure_reason::system_pressure);
    REQUIRE(std::string_view{error.what()}.find("admission_reason=system_pressure") !=
            std::string_view::npos);
    REQUIRE(std::string_view{error.what()}.find("range_count=32035") != std::string_view::npos);
}

TEST_CASE("Vulkan chaining shape preflight exactly matches in-place range compaction",
          "[component:bioseq][layer:unit][feature:chaining]") {
    constexpr std::size_t anchor_count =
        ::bioseq::chaining::vulkan_chaining_detail::k_gpu_chain_compact_range_threshold + 2u;
    std::vector<::bioseq::chaining::anchor> anchors(anchor_count);
    for (std::size_t i = 0; i < anchors.size(); ++i) {
        anchors[i].ref_position = i;
        anchors[i].query_position = i;
        anchors[i].query_span = 15u;
    }

    const auto shape = ::bioseq::chaining::vulkan_chaining_detail::analyze_gpu_chaining_shape(
        anchors, /*max_ref_dist=*/10u);
    REQUIRE(shape.range_count == 1u);
    REQUIRE(shape.compaction_required);
    REQUIRE(shape.device_anchor_count < anchors.size());

    context::context ctx;
    context::installed_context_scope installed{ctx};
    auto scope = ctx.push<context::arena::chain>();
    auto ranges = ::bioseq::chaining::vulkan_chaining_detail::build_gpu_chain_ranges(
        anchors, /*max_ref_dist=*/10u, shape.range_count);
    auto compacted = ::bioseq::chaining::make_arena_slab<::bioseq::chaining::anchor>();
    REQUIRE(::bioseq::chaining::vulkan_chaining_detail::compact_long_gpu_chain_ranges(
        anchors, ranges, compacted, ranges));
    REQUIRE(compacted.size() == shape.device_anchor_count);
    REQUIRE(ranges.size() == shape.range_count);
    REQUIRE(ranges[0].begin == 0u);
    REQUIRE(ranges[0].length == shape.device_anchor_count);
    REQUIRE(compacted.data()[0].ref_position == anchors.front().ref_position);
    REQUIRE(compacted.data()[compacted.size() - 1u].ref_position == anchors.back().ref_position);
}

TEST_CASE("match_result biological fingerprints cover every result and CIGAR field",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    match_result result{
        .query_len = 101u,
        .query_start = 3u,
        .query_end = 97u,
        .is_reverse = true,
        .target_len = 1001u,
        .target_start = 23u,
        .target_end = 117u,
        .num_matches = 90u,
        .block_length = 94u,
        .mapq = 42,
        .chain_score = 99,
        .dp_score = 88,
        .dp_max_score = 91,
        .seed_count = 7,
        .secondary_chain_score = 11,
        .divergence = 0.125f,
        .is_primary = true,
        .has_cigar = true,
        .cigar =
            {
                cigar_operation{.length = 10u, .opcode = cigar_match_opcode},
                cigar_operation{.length = 2u, .opcode = cigar_insertion_opcode},
                cigar_operation{.length = 82u, .opcode = cigar_match_opcode},
            },
        .edit_distance = 2u,
    };
    match_result_collection lhs{std::vector<match_result>{result}};
    match_result_collection rhs{std::vector<match_result>{result}};

    const auto expected = lhs.biological_identity();
    REQUIRE(expected.is_computed);
    REQUIRE_FALSE(expected.is_saturated);
    REQUIRE(expected.collection_count == 1u);
    REQUIRE(expected.match_result_count == 1u);
    REQUIRE(expected.cigar_result_count == 1u);
    REQUIRE(expected.cigar_operation_count == 3u);
    REQUIRE(expected == rhs.biological_identity());

    enum class result_field : std::uint8_t {
        query_length,
        query_start,
        query_end,
        is_reverse,
        target_length,
        target_start,
        target_end,
        num_matches,
        block_length,
        mapq,
        chain_score,
        dp_score,
        dp_max_score,
        seed_count,
        secondary_chain_score,
        divergence,
        is_primary,
        has_cigar,
        edit_distance,
    };
    struct result_field_mutation {
        result_field field;
        const char* name;
        bool affects_cigar_digest;
    };
    constexpr std::array<result_field_mutation, 19u> result_mutations{{
        {result_field::query_length, "query_length", false},
        {result_field::query_start, "query_start", false},
        {result_field::query_end, "query_end", false},
        {result_field::is_reverse, "is_reverse", false},
        {result_field::target_length, "target_length", false},
        {result_field::target_start, "target_start", false},
        {result_field::target_end, "target_end", false},
        {result_field::num_matches, "num_matches", false},
        {result_field::block_length, "block_length", false},
        {result_field::mapq, "mapq", false},
        {result_field::chain_score, "chain_score", false},
        {result_field::dp_score, "dp_score", false},
        {result_field::dp_max_score, "dp_max_score", false},
        {result_field::seed_count, "seed_count", false},
        {result_field::secondary_chain_score, "secondary_chain_score", false},
        {result_field::divergence, "divergence", false},
        {result_field::is_primary, "is_primary", false},
        {result_field::has_cigar, "has_cigar", true},
        {result_field::edit_distance, "edit_distance", false},
    }};
    for (const auto& mutation : result_mutations) {
        auto mutated = result;
        switch (mutation.field) {
        case result_field::query_length:
            ++mutated.query_len;
            break;
        case result_field::query_start:
            ++mutated.query_start;
            break;
        case result_field::query_end:
            ++mutated.query_end;
            break;
        case result_field::is_reverse:
            mutated.is_reverse = !mutated.is_reverse;
            break;
        case result_field::target_length:
            ++mutated.target_len;
            break;
        case result_field::target_start:
            ++mutated.target_start;
            break;
        case result_field::target_end:
            ++mutated.target_end;
            break;
        case result_field::num_matches:
            ++mutated.num_matches;
            break;
        case result_field::block_length:
            ++mutated.block_length;
            break;
        case result_field::mapq:
            ++mutated.mapq;
            break;
        case result_field::chain_score:
            ++mutated.chain_score;
            break;
        case result_field::dp_score:
            ++mutated.dp_score;
            break;
        case result_field::dp_max_score:
            ++mutated.dp_max_score;
            break;
        case result_field::seed_count:
            ++mutated.seed_count;
            break;
        case result_field::secondary_chain_score:
            ++mutated.secondary_chain_score;
            break;
        case result_field::divergence:
            mutated.divergence = 0.25f;
            break;
        case result_field::is_primary:
            mutated.is_primary = !mutated.is_primary;
            break;
        case result_field::has_cigar:
            mutated.has_cigar = !mutated.has_cigar;
            break;
        case result_field::edit_distance:
            ++mutated.edit_distance;
            break;
        }
        INFO("mutated match_result field: " << mutation.name);
        rhs.results = {std::move(mutated)};
        const auto fingerprint = rhs.biological_identity();
        REQUIRE(fingerprint.match_digest_lane0 != expected.match_digest_lane0);
        REQUIRE(fingerprint.match_digest_lane1 != expected.match_digest_lane1);
        if (mutation.affects_cigar_digest) {
            REQUIRE(fingerprint.cigar_digest_lane0 != expected.cigar_digest_lane0);
            REQUIRE(fingerprint.cigar_digest_lane1 != expected.cigar_digest_lane1);
        } else {
            REQUIRE(fingerprint.cigar_digest_lane0 == expected.cigar_digest_lane0);
            REQUIRE(fingerprint.cigar_digest_lane1 == expected.cigar_digest_lane1);
        }
    }

    enum class cigar_field : std::uint8_t { length, opcode, vector_shape };
    struct cigar_field_mutation {
        cigar_field field;
        const char* name;
    };
    constexpr std::array<cigar_field_mutation, 3u> cigar_mutations{{
        {cigar_field::length, "cigar.length"},
        {cigar_field::opcode, "cigar.opcode"},
        {cigar_field::vector_shape, "cigar vector shape"},
    }};
    for (const auto& mutation : cigar_mutations) {
        auto mutated = result;
        switch (mutation.field) {
        case cigar_field::length:
            ++mutated.cigar[1].length;
            break;
        case cigar_field::opcode:
            mutated.cigar[1].opcode = cigar_deletion_opcode;
            break;
        case cigar_field::vector_shape:
            mutated.cigar.push_back(cigar_operation{.length = 1u, .opcode = cigar_deletion_opcode});
            break;
        }
        INFO("mutated CIGAR field: " << mutation.name);
        rhs.results = {std::move(mutated)};
        const auto fingerprint = rhs.biological_identity();
        REQUIRE(fingerprint.match_digest_lane0 != expected.match_digest_lane0);
        REQUIRE(fingerprint.match_digest_lane1 != expected.match_digest_lane1);
        REQUIRE(fingerprint.cigar_digest_lane0 != expected.cigar_digest_lane0);
        REQUIRE(fingerprint.cigar_digest_lane1 != expected.cigar_digest_lane1);
    }

    match_result_collection empty_a;
    match_result_collection empty_b;
    const auto empty_fingerprint = empty_a.biological_identity();
    REQUIRE(empty_fingerprint.is_computed);
    REQUIRE_FALSE(empty_fingerprint.is_saturated);
    REQUIRE(empty_fingerprint.collection_count == 1u);
    REQUIRE(empty_fingerprint.match_result_count == 0u);
    REQUIRE(empty_fingerprint.cigar_result_count == 0u);
    REQUIRE(empty_fingerprint.cigar_operation_count == 0u);
    REQUIRE(empty_fingerprint == empty_b.biological_identity());

    auto second = result;
    ++second.query_start;
    ++second.cigar.front().length;
    match_result_collection ordered{std::vector<match_result>{result, second}};
    match_result_collection reversed{std::vector<match_result>{second, result}};
    const auto ordered_fingerprint = ordered.biological_identity();
    const auto reversed_fingerprint = reversed.biological_identity();
    REQUIRE(ordered_fingerprint.match_digest_lane0 != reversed_fingerprint.match_digest_lane0);
    REQUIRE(ordered_fingerprint.match_digest_lane1 != reversed_fingerprint.match_digest_lane1);
    REQUIRE(ordered_fingerprint.cigar_digest_lane0 != reversed_fingerprint.cigar_digest_lane0);
    REQUIRE(ordered_fingerprint.cigar_digest_lane1 != reversed_fingerprint.cigar_digest_lane1);

    match_result_set_identity aggregate{};
    aggregate.merge(expected);
    REQUIRE(aggregate == expected);
    aggregate.merge(match_result_set_identity{});
    REQUIRE_FALSE(aggregate.is_computed);
    REQUIRE(aggregate.is_saturated);
    aggregate.merge(expected);
    REQUIRE_FALSE(aggregate.is_computed);
    REQUIRE(aggregate.is_saturated);
}

} // namespace bioseq::simd::pipe
