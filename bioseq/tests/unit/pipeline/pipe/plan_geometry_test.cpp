/**
 * @file plan_geometry_test.cpp
 * @brief Exercises pipeline plans and geometry.
 */
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.unit.pipeline.pipe_support;

using namespace bioseq::tests::pipeline::pipe_support;

namespace bioseq::simd::pipe {

using namespace ::bioseq::tests::pipeline::pipe_support;

TEST_CASE("emit_token trait surface", "[component:bioseq][layer:unit][feature:pipeline]") {
    SECTION("EmitTokenTraits") {
        static_assert(!std::is_copy_constructible_v<emit_token<sink_stage>>);
        static_assert(!std::is_move_constructible_v<emit_token<sink_stage>>);
        static_assert(!std::is_default_constructible_v<emit_token<sink_stage>>);
        SUCCEED();
    }
    SECTION("EmittedTokenTraits") {
        static_assert(!std::is_default_constructible_v<emitted_token<sink_stage>>);
        static_assert(!std::is_copy_constructible_v<emitted_token<sink_stage>>);
        SUCCEED();
    }
    SECTION("EmitMethodReturnType") {
        // emit_token::emit_batch returns emitted_token<Sink>.
        // The expression is evaluated only inside `std::declval`, so no
        // emit_token instance is actually constructed (it is non-default-
        // and non-move-constructible).
        static_assert(std::is_same_v<
            decltype(std::declval<emit_token<sink_stage>>().emit_batch(
                std::declval<std::span<alignment_region>>())),
            emitted_token<sink_stage>>);
        SUCCEED();
    }
    SECTION("SinkConceptHolds") {
        static_assert(batch_sink<sink_stage, alignment_region>);
        static_assert(!batch_sink<sink_stage, ::bioseq::chaining::anchor>);
        SUCCEED();
    }
    SECTION("ProbeBatchSinkConceptHolds") {
        static_assert(batch_sink<probe_batch_sink, alignment_region>);
        SUCCEED();
    }
}

TEST_CASE("mapq_stage emits exactly one batch per upstream batch",
          "[component:bioseq][layer:unit][feature:pipeline]")
{
    using cep_type = static_aligner_parameters<10u, 15u, 28u>;
    int on_batch_count = 0;
    int on_completed_count = 0;
    probe_batch_sink probe{&on_batch_count, &on_completed_count};

    ::bioseq::simd::minimap2_parameters params;
    params.compute_mapq = false;
    mapq_stage stage{{}, params, /*enabled=*/false};
    auto bound = stage.template attach<cep_type>(probe);

    std::vector<alignment_region> regions(1);
    bound.on_batch(std::span<alignment_region>{regions.data(), regions.size()});
    REQUIRE(on_batch_count == 1);
    bound.on_completed();
    REQUIRE(on_completed_count == 1);
}

TEST_CASE("mapq_stage::process_pair delegates exactly once",
          "[component:bioseq][layer:unit][feature:pipeline]")
{
    using cep_type = static_aligner_parameters<10u, 15u, 28u>;
    int on_batch_count = 0;
    int on_completed_count = 0;
    probe_batch_sink probe{&on_batch_count, &on_completed_count};

    ::bioseq::simd::minimap2_parameters params;
    mapq_stage stage{{}, params, /*enabled=*/false};
    auto bound = stage.template attach<cep_type>(probe);

    std::vector<alignment_region> regions(2);
    std::vector<::bioseq::chaining::anchor> anchors(2);
    bound.on_batch_pair(
        std::span<alignment_region>{regions.data(), regions.size()},
        std::span<::bioseq::chaining::anchor>{anchors.data(), anchors.size()});
    REQUIRE(on_batch_count == 1);
}

TEST_CASE("exact_edit_filter_stage applies strict bounds and resets classification state",
          "[component:bioseq][layer:unit][feature:alignment]")
{
    using cep_type = static_aligner_parameters<10u, 15u, 28u>;
    ::bioseq::simd::minimap2_parameters params;
    params.compute_edit_distance = true;
    params.exact_edit_filter_enabled = true;
    params.exact_edit_filter_exclusive_upper_bound = 5;

    std::vector<alignment_region> observed;
    int on_batch_count = 0;
    exact_filter_probe_sink probe{&observed, &on_batch_count};
    exact_edit_filter_stage stage{{}, params};
    auto bound = stage.template attach<cep_type>(probe);

    std::vector<alignment_region> regions(4);
    for (std::size_t index = 0; index < regions.size(); ++index) {
        alignment_region& region = regions[index];
        region.has_alignment = true;
        region.anchor_start = static_cast<std::int32_t>(100u + index);
        region.parent = 91;
        region.secondary_score = 92;
        region.secondary_count = 93;
        region.secondary_dp_max = 94;
        region.mapq = 95;
    }
    regions[0].edit_distance = 5;
    regions[1].edit_distance = 4;
    regions[2].edit_distance = 0;
    regions[3].edit_distance = 8;
    std::vector<::bioseq::chaining::anchor> anchors;

    bound.on_batch_pair(
        std::span<alignment_region>{regions},
        std::span<::bioseq::chaining::anchor>{anchors});

    REQUIRE(on_batch_count == 1);
    REQUIRE(observed.size() == 2);
    REQUIRE(observed[0].anchor_start == 101);
    REQUIRE(observed[1].anchor_start == 102);
    for (std::size_t index = 0; index < observed.size(); ++index) {
        CAPTURE(index);
        CHECK(observed[index].region_id == static_cast<std::int32_t>(index));
        CHECK(observed[index].parent == -1);
        CHECK(observed[index].secondary_score == 0);
        CHECK(observed[index].secondary_count == 0);
        CHECK(observed[index].secondary_dp_max == 0);
        CHECK(observed[index].mapq == 0);
    }
}

TEST_CASE("exact_edit_filter_stage rejects nonpositive bounds and missing traceback",
          "[component:bioseq][layer:unit][feature:alignment]")
{
    using cep_type = static_aligner_parameters<10u, 15u, 28u>;
    std::vector<::bioseq::chaining::anchor> anchors;

    SECTION("nonpositive bound rejects every exact alignment") {
        ::bioseq::simd::minimap2_parameters params;
        params.compute_edit_distance = true;
        params.exact_edit_filter_enabled = true;
        params.exact_edit_filter_exclusive_upper_bound = 0;
        std::vector<alignment_region> observed;
        int on_batch_count = 0;
        exact_filter_probe_sink probe{&observed, &on_batch_count};
        exact_edit_filter_stage stage{{}, params};
        auto bound = stage.template attach<cep_type>(probe);
        std::vector<alignment_region> regions(1);
        regions[0].has_alignment = true;
        regions[0].edit_distance = 0;

        bound.on_batch_pair(
            std::span<alignment_region>{regions},
            std::span<::bioseq::chaining::anchor>{anchors});
        REQUIRE(on_batch_count == 1);
        REQUIRE(observed.empty());
    }

    SECTION("missing traceback fails loudly") {
        ::bioseq::simd::minimap2_parameters params;
        params.compute_edit_distance = true;
        params.exact_edit_filter_enabled = true;
        params.exact_edit_filter_exclusive_upper_bound = 1;
        std::vector<alignment_region> observed;
        int on_batch_count = 0;
        exact_filter_probe_sink probe{&observed, &on_batch_count};
        exact_edit_filter_stage stage{{}, params};
        auto bound = stage.template attach<cep_type>(probe);
        std::vector<alignment_region> regions(1);
        regions[0].has_alignment = false;

        REQUIRE_THROWS_WITH(
            bound.on_batch_pair(
                std::span<alignment_region>{regions},
                std::span<::bioseq::chaining::anchor>{anchors}),
            "exact edit filter received a region without alignment traceback");
        REQUIRE(on_batch_count == 0);
    }
}

TEST_CASE("SingleEmissionInvariant — N=8 queries one emission each",
          "[component:bioseq][layer:unit][feature:pipeline]")
{
    using cep_type = static_aligner_parameters<10u, 15u, 28u>;
    int on_batch_count = 0;
    int on_completed_count = 0;
    probe_batch_sink probe{&on_batch_count, &on_completed_count};

    ::bioseq::simd::minimap2_parameters params;
    mapq_stage stage{{}, params, /*enabled=*/false};
    auto bound = stage.template attach<cep_type>(probe);

    constexpr int num_queries = 8;
    std::vector<alignment_region> regions(1);
    for (int qi = 0; qi < num_queries; ++qi) {
        bound.on_batch(std::span<alignment_region>{regions.data(), regions.size()});
    }
    REQUIRE(on_batch_count == num_queries);
    bound.on_completed();
    REQUIRE(on_completed_count == 1);
}

TEST_CASE("KSW2BatchedParity — trait surface matches KSW2",
          "[component:bioseq][layer:unit][feature:alignment]")
{
    using ksw2_kernel_type     = kernel_t<::bioseq::simd::
                                  base_alignment_algorithm::ksw2>;
    using batched_kernel_type  = kernel_t<::bioseq::simd::
                                  base_alignment_algorithm::ksw2_batched>;

    ::bioseq::simd::minimap2_parameters params;
    std::vector<named_property> ksw2_params;
    std::vector<named_property> batched_params;
    ksw2_kernel_type::populate_runtime_params(params, ksw2_params);
    batched_kernel_type::populate_runtime_params(params, batched_params);
    REQUIRE(ksw2_params.size() == batched_params.size());
    for (std::size_t i = 0; i < ksw2_params.size(); ++i) {
        REQUIRE(ksw2_params[i].name == batched_params[i].name);
        REQUIRE(ksw2_params[i].value == batched_params[i].value);
    }
    REQUIRE(std::string{ksw2_kernel_type::algorithm_label()} == "ksw2");
    REQUIRE(std::string{batched_kernel_type::algorithm_label()}
            == "ksw2 (inter-region batched)");
    static_assert(!ksw2_kernel_type::emits_ksw2_batched_execution_stats);
    static_assert(batched_kernel_type::emits_ksw2_batched_execution_stats);
}

TEST_CASE("KSW2 batched stats rejects zero and tampered observations",
          "[component:bioseq][layer:unit][feature:alignment]") {
    using ::bioseq::simd::ksw2_batched_execution_stats;
    using ::bioseq::simd::match_result_collection;

    ksw2_batched_execution_stats empty{};
    REQUIRE_FALSE(empty.is_coherent(false));
    REQUIRE_FALSE(empty.is_coherent(true));

    ksw2_batched_execution_stats valid{};
    valid.record_invocation(1u);
    valid.record_collected_fills(2u);
    valid.record_short_bucket_dispatch(1u);
    valid.record_nonempty_fill(32u, 31u, false);
    valid.record_nonempty_fill(300u, 301u, true);
    REQUIRE(valid.is_coherent(true));

    auto tampered_lane_count = valid;
    ++tampered_lane_count.short_bucket_lanes;
    REQUIRE_FALSE(tampered_lane_count.is_coherent(true));

    auto dropped_fill = valid;
    ++dropped_fill.fills_collected;
    REQUIRE_FALSE(dropped_fill.is_coherent(true));

    auto tampered_area = valid;
    tampered_area.input_cell_area = 0u;
    REQUIRE_FALSE(tampered_area.is_coherent(true));

    auto impossible_dispatch_count = valid;
    impossible_dispatch_count.short_bucket_dispatches =
        impossible_dispatch_count.short_bucket_lanes + 1u;
    REQUIRE_FALSE(impossible_dispatch_count.is_coherent(true));

    auto impossible_cell_area = valid;
    impossible_cell_area.input_cell_area = 1u;
    REQUIRE_FALSE(impossible_cell_area.is_coherent(true));

    auto saturated = valid;
    saturated.saturated = true;
    REQUIRE_FALSE(saturated.is_coherent(true));

    ksw2_batched_execution_stats combined{};
    combined.merge(valid);
    combined.merge(valid);
    REQUIRE(combined.is_coherent(true));
    REQUIRE(combined.alignment_region_invocations == 2u);
    REQUIRE(combined.nonempty_fill_calls == 4u);

    match_result_collection first;
    first.record_ksw2_batched_execution_stats(valid);
    first.record_query_sketch_host_time(7u);
    first.record_seed_lookup_host_time(11u);
    first.record_chain_score_host_time(13u);
    first.record_region_generation_host_time(17u);
    first.record_alignment_result_host_time(19u);
    match_result_collection second;
    second.record_ksw2_batched_execution_stats(valid);
    second.record_query_sketch_host_time(11u);
    second.record_seed_lookup_host_time(19u);
    second.record_chain_score_host_time(23u);
    second.record_region_generation_host_time(29u);
    second.record_alignment_result_host_time(31u);
    first.combine_execution_stats(second);
    REQUIRE(first.is_ksw2_batched_execution_consistent(true));
    REQUIRE(first.ksw2_batched_stats.alignment_region_invocations
            == 2u);
    REQUIRE(first.query_sketch_host_wall_ns == 18u);
    REQUIRE(first.seed_lookup_host_wall_ns == 30u);
    REQUIRE(first.chain_score_host_wall_ns == 36u);
    REQUIRE(first.region_generation_host_wall_ns == 46u);
    REQUIRE(first.alignment_result_host_wall_ns == 50u);
    REQUIRE_FALSE(first.is_host_stage_timing_saturated);
}

TEST_CASE("Host timing region handoff is non-overlapping and mergeable",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using ::bioseq::simd::match_result_collection;

    match_result_collection collection;
    collection.begin_region_host_stage(100u);
    collection.transition_region_to_alignment_result_host_stage(
        130u, /*alignment_enabled=*/true);
    collection.finish_alignment_result_host_stage(180u);

    REQUIRE(collection.region_generation_host_wall_ns == 30u);
    REQUIRE(collection.alignment_result_host_wall_ns == 50u);
    REQUIRE(collection.region_generation_host_wall_ns
            + collection.alignment_result_host_wall_ns == 80u);
    REQUIRE_FALSE(collection.is_region_host_stage_active);
    REQUIRE_FALSE(collection.is_alignment_result_host_stage_active);
    REQUIRE_FALSE(collection.is_host_stage_timing_saturated);

    match_result_collection saturated;
    saturated.record_alignment_result_host_time(
        (std::numeric_limits<std::uint64_t>::max)());
    saturated.record_alignment_result_host_time(1u);
    REQUIRE(saturated.alignment_result_host_wall_ns
            == (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE(saturated.is_host_stage_timing_saturated);
}

TEST_CASE("dispatch_exact_value_or_default routes to the matching template branch",
          "[component:bioseq][layer:unit][feature:planner]")
{
    using dispatch_test_support::base_alignment_algorithm_type;
    using dispatch_test_support::chaining_algorithm_type;
    using dispatch_test_support::minimizer_algorithm_type;

    SECTION("chaining_algorithm — every enum value routes correctly") {
        using registry_type = algorithm_enum_values<chaining_algorithm_type>;
        for (chaining_algorithm_type v : {chaining_algorithm_type::scalar_minimap2, chaining_algorithm_type::simd_minimap2, chaining_algorithm_type::range_minimum_query, chaining_algorithm_type::vulkan_tiled_dynamic_programming}) {
            chaining_algorithm_type captured = static_cast<chaining_algorithm_type>(0xFF);
            dispatch_test_support::value_capture_callable<chaining_algorithm_type> cb{&captured};
            const chaining_algorithm_type result = dispatch_exact_value_or_default<chaining_algorithm_type, chaining_algorithm_type, registry_type::default_value>(
                registry_type::value_list{}, v, cb);
            REQUIRE(captured == v);
            REQUIRE(result == v);
        }
    }
    SECTION("base_alignment_algorithm — every enum value routes correctly") {
        using registry_type = algorithm_enum_values<base_alignment_algorithm_type>;
        for (base_alignment_algorithm_type v : {base_alignment_algorithm_type::ksw2, base_alignment_algorithm_type::block_aligner, base_alignment_algorithm_type::ksw2_batched}) {
            base_alignment_algorithm_type captured = static_cast<base_alignment_algorithm_type>(0xFF);
            dispatch_test_support::value_capture_callable<base_alignment_algorithm_type> cb{&captured};
            const base_alignment_algorithm_type result = dispatch_exact_value_or_default<base_alignment_algorithm_type, base_alignment_algorithm_type, registry_type::default_value>(
                registry_type::value_list{}, v, cb);
            REQUIRE(captured == v);
            REQUIRE(result == v);
        }
    }
    SECTION("minimizer_algorithm — every enum value routes correctly") {
        using registry_type = algorithm_enum_values<minimizer_algorithm_type>;
        for (minimizer_algorithm_type v : {minimizer_algorithm_type::scalar,
                     minimizer_algorithm_type::simd_minimap2_reference,
                     minimizer_algorithm_type::two_stack_nthash,
                     minimizer_algorithm_type::vulkan_minimap_integer,
                     minimizer_algorithm_type::bounded_nthash_simd}) {
            minimizer_algorithm_type captured = static_cast<minimizer_algorithm_type>(0xFF);
            dispatch_test_support::value_capture_callable<minimizer_algorithm_type> cb{&captured};
            const minimizer_algorithm_type result = dispatch_exact_value_or_default<minimizer_algorithm_type, minimizer_algorithm_type, registry_type::default_value>(
                registry_type::value_list{}, v, cb);
            REQUIRE(captured == v);
            REQUIRE(result == v);
        }
    }
}

TEST_CASE("Exact SIMD chaining metadata permits canonical nonzero skip penalty",
          "[component:bioseq][layer:unit][feature:chaining]") {
    using chaining_algorithm_type = ::bioseq::simd::chaining_algorithm;
    using kernel_type = algorithm_traits<chaining_algorithm_type::simd_minimap2>::kernel_type;
    using cep_type = static_aligner_parameters<10, 15, 28>;
    const auto properties = kernel_type::template static_properties<cep_type>();
    const auto property = std::ranges::find_if(
        properties,
        [](const named_property& candidate) {
            return candidate.name == "requires_zero_skip_penalty";
        });
    REQUIRE(property != properties.end());
    REQUIRE(property->value == "false");
}

TEST_CASE("scalar chaining trait keeps an empty negative free of score work",
          "[component:bioseq][layer:unit][feature:chaining]") {
    using ::bioseq::chaining::anchor;
    using ::bioseq::chaining::chain;
    using ::bioseq::chaining::chaining_parameters;
    using ::bioseq::simd::chaining_algorithm;
    using ::bioseq::simd::scalar_pipeline_execution_stats;
    using kernel_type =
        algorithm_traits<chaining_algorithm::scalar_minimap2>::kernel_type;

    context::context ctx;
    context::installed_context_scope installed{ctx};
    auto scope = ctx.push<context::arena::chain>();
    auto chains = ::bioseq::chaining::make_arena_slab<chain>();
    auto reordered = ::bioseq::chaining::make_arena_slab<anchor>();
    scalar_pipeline_execution_stats stats{};
    kernel_type::call(
        std::span<const anchor>{}, chaining_parameters{}, chains, reordered,
        &stats);

    REQUIRE(chains.size() == 0u);
    REQUIRE(reordered.size() == 0u);
    REQUIRE(stats.chain_score_calls == 0u);
    REQUIRE(stats.chain_backtrack_calls == 0u);
    REQUIRE(stats.chain_anchor_inputs == 0u);
    REQUIRE(stats.chain_stage_completion_calls == 0u);
    REQUIRE(stats.chain_stage_completed_anchor_inputs == 0u);
}

TEST_CASE("SIMD chaining policy is exact by default and explicit in stats metadata",
          "[component:bioseq][layer:unit][feature:chaining]") {
    using ::bioseq::chaining::simd_chaining_policy;
    using ::bioseq::simd::chaining_algorithm;
    using ::bioseq::simd::match_result_collection;
    using ::bioseq::simd::minimap2_parameters;
    using ::bioseq::chaining::simd_chaining_execution_stats;
    using kernel_type = algorithm_traits<chaining_algorithm::simd_minimap2>::kernel_type;

    minimap2_parameters params{};
    REQUIRE(params.simd_chaining_policy
        == simd_chaining_policy::exact_visited);
    REQUIRE(params.make_chaining_parameters().simd_policy
        == simd_chaining_policy::exact_visited);

    params.simd_chaining_policy =
        simd_chaining_policy::bounded_batch_approximate;
    const auto bounded_params = params.make_chaining_parameters();
    REQUIRE(bounded_params.simd_policy
        == simd_chaining_policy::bounded_batch_approximate);

    std::vector<named_property> runtime_properties;
    kernel_type::populate_runtime_params(bounded_params, runtime_properties);
    const auto policy_property = std::ranges::find_if(
        runtime_properties,
        [](const named_property& candidate) {
            return candidate.name == "simd_chaining_policy";
        });
    REQUIRE(policy_property != runtime_properties.end());
    REQUIRE(policy_property->value == "bounded_batch_approximate");

    simd_chaining_execution_stats exact{};
    exact.policy.exact_visited_calls = 1u;
    match_result_collection collection;
    collection.record_simd_chaining_execution_stats(exact);
    REQUIRE(collection.is_simd_chaining_execution_consistent(
        simd_chaining_policy::exact_visited,
        /*require_nonempty_vector_work=*/false));
    REQUIRE_FALSE(collection.is_simd_chaining_execution_consistent(
        simd_chaining_policy::bounded_batch_approximate,
        /*require_nonempty_vector_work=*/false));

    simd_chaining_execution_stats bounded{};
    bounded.policy.bounded_batch_approximate_calls = 1u;
    match_result_collection mixed;
    mixed.record_simd_chaining_execution_stats(exact);
    mixed.record_simd_chaining_execution_stats(bounded);
    REQUIRE_FALSE(mixed.is_simd_chaining_execution_consistent(
        simd_chaining_policy::exact_visited,
        /*require_nonempty_vector_work=*/false));
    REQUIRE_FALSE(mixed.is_simd_chaining_execution_consistent(
        simd_chaining_policy::bounded_batch_approximate,
        /*require_nonempty_vector_work=*/false));
}

TEST_CASE("scalar pipeline stats is operation-local and rejects mixed routes",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using ::bioseq::simd::match_result_collection;
    using ::bioseq::simd::partial_vulkan_execution_stats;
    using ::bioseq::simd::scalar_ksw2_stage;
    using ::bioseq::simd::scalar_minimizer_execution_stats;
    using ::bioseq::simd::scalar_pipeline_execution_stats;

    scalar_minimizer_execution_stats query{};
    query.record_input(5'000u);
    scalar_minimizer_execution_stats target{};
    target.record_input(2'000'000u);

    scalar_pipeline_execution_stats recomputed{};
    recomputed.query_minimizers = query;
    recomputed.target_minimizers = target;
    recomputed.record_seed_lookup(400u);
    recomputed.record_scalar_chain(1'200u);
    recomputed.record_scalar_chain_stage_completion(1'200u);
    recomputed.record_ksw2_call(
        scalar_ksw2_stage::left, 100u, 120u);
    recomputed.record_ksw2_call(
        scalar_ksw2_stage::gap, 80u, 90u);

    match_result_collection complete;
    complete.record_scalar_pipeline_execution_stats(recomputed);
    REQUIRE(complete.is_all_scalar_execution_consistent(
        /*require_target_sketch=*/true,
        /*require_alignment=*/true));
    REQUIRE_FALSE(complete.is_all_simd_execution_consistent(
        ::bioseq::simd::minimizer_algorithm::
            two_stack_nthash,
        ::bioseq::chaining::simd_chaining_policy::exact_visited,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));

    scalar_pipeline_execution_stats persisted = recomputed;
    persisted.target_minimizers = {};
    match_result_collection persisted_collection;
    persisted_collection.record_scalar_pipeline_execution_stats(persisted);
    REQUIRE(persisted_collection.is_all_scalar_execution_consistent(
        /*require_target_sketch=*/false,
        /*require_alignment=*/true));
    REQUIRE_FALSE(persisted_collection.is_all_scalar_execution_consistent(
        /*require_target_sketch=*/true,
        /*require_alignment=*/true));

    match_result_collection no_alignment;
    scalar_pipeline_execution_stats coordinate_only = persisted;
    coordinate_only.ksw2_left_calls = 0;
    coordinate_only.ksw2_gap_calls = 0;
    coordinate_only.ksw2_right_calls = 0;
    coordinate_only.ksw2_input_cell_area = 0;
    no_alignment.record_scalar_pipeline_execution_stats(coordinate_only);
    REQUIRE(no_alignment.is_all_scalar_execution_consistent(
        /*require_target_sketch=*/false,
        /*require_alignment=*/false));
    REQUIRE_FALSE(no_alignment.is_all_scalar_execution_consistent(
        /*require_target_sketch=*/false,
        /*require_alignment=*/true));

    /* A real zero-anchor scalar chain is an attested negative operation.  Its
     * successful stage receipt is distinct from score/backtrack work, and it
     * cannot waive a selected KSW2 alignment requirement. */
    scalar_pipeline_execution_stats zero_anchor = recomputed;
    zero_anchor.chain_stage_completion_calls = 0;
    zero_anchor.chain_stage_completed_anchor_inputs = 0;
    zero_anchor.chain_score_calls = 0;
    zero_anchor.chain_backtrack_calls = 0;
    zero_anchor.chain_anchor_inputs = 0;
    zero_anchor.ksw2_left_calls = 0;
    zero_anchor.ksw2_gap_calls = 0;
    zero_anchor.ksw2_right_calls = 0;
    zero_anchor.ksw2_input_cell_area = 0;
    zero_anchor.record_scalar_chain_stage_completion(0u);
    match_result_collection zero_anchor_negative;
    zero_anchor_negative.record_scalar_pipeline_execution_stats(zero_anchor);
    REQUIRE(zero_anchor_negative.is_all_scalar_execution_consistent(
        /*require_target_sketch=*/true,
        /*require_alignment=*/false));
    REQUIRE_FALSE(zero_anchor_negative.is_all_scalar_execution_consistent(
        /*require_target_sketch=*/true,
        /*require_alignment=*/true));

    scalar_pipeline_execution_stats impossible_empty_chain = zero_anchor;
    impossible_empty_chain.chain_score_calls = 1;
    impossible_empty_chain.chain_backtrack_calls = 1;
    match_result_collection impossible_negative;
    impossible_negative.record_scalar_pipeline_execution_stats(
        impossible_empty_chain);
    REQUIRE_FALSE(impossible_negative.is_all_scalar_execution_consistent(
        /*require_target_sketch=*/true,
        /*require_alignment=*/false));

    scalar_pipeline_execution_stats missing_chain_receipt = zero_anchor;
    missing_chain_receipt.chain_stage_completion_calls = 0;
    match_result_collection incomplete_negative;
    incomplete_negative.record_scalar_pipeline_execution_stats(
        missing_chain_receipt);
    REQUIRE_FALSE(incomplete_negative.is_all_scalar_execution_consistent(
        /*require_target_sketch=*/true,
        /*require_alignment=*/false));

    match_result_collection mixed = complete;
    mixed.exact_simd_query_execution_stats_available = true;
    REQUIRE_FALSE(mixed.is_all_scalar_execution_consistent(
        /*require_target_sketch=*/true,
        /*require_alignment=*/true));

    match_result_collection gpu_mixed = complete;
    partial_vulkan_execution_stats gpu{};
    gpu.seed_lookup_dispatches = 1u;
    gpu_mixed.record_partial_vulkan_execution_stats(gpu);
    REQUIRE_FALSE(gpu_mixed.is_all_scalar_execution_consistent(
        /*require_target_sketch=*/true,
        /*require_alignment=*/true));

    match_result_collection saturated = complete;
    saturated.scalar_pipeline_stats.is_saturated = true;
    REQUIRE_FALSE(saturated.is_all_scalar_execution_consistent(
        /*require_target_sketch=*/true,
        /*require_alignment=*/true));
}

TEST_CASE("exact SIMD collection stats merge valid observations and reject tampering",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using ::bioseq::simd::match_result_collection;
    using ::bioseq::simd::two_stack_execution_stats;

    const auto actual_range_stats = [](std::uint64_t bp) {
        two_stack_execution_stats stats{};
        stats.bp_consumed = bp;
        stats.fragment_symbols_staged = bp;
        stats.packed_decode_lanes = bp;
        stats.generic_u64_kernel_calls = 1;
        stats.event_generation_batches = 1;
        stats.stable_compress_batches = 1;
        if (bp != 0) {
            stats.vector_batches = 1;
            stats.vector_lanes_visited = bp;
            stats.generic_u64_candidate_blocks = 1;
            stats.generic_u64_lane_visits = bp;
            stats.consecutive_kmer_batches = 1;
            stats.canonical_scramble_batches = 1;
            stats.window_reduction_batches = 1;
            stats.event_generation_batches = 2;
            stats.stable_compress_batches = 2;
            stats.tie_matrix_event_batches = 1;
            stats.row_transpose_batches = 1;
            stats.row_vector_stores = 1;
            stats.packed_decode_batches = 1;
        }
        return stats;
    };

    match_result_collection first;
    first.record_exact_simd_target_execution_stats(
        actual_range_stats(17));
    match_result_collection second;
    second.record_exact_simd_target_execution_stats(
        actual_range_stats(23));
    first.combine_execution_stats(second);
    REQUIRE(first.exact_simd_target_execution_stats_available);
    REQUIRE(first.exact_simd_target_execution_stats.bp_consumed == 40);
    REQUIRE(first.exact_simd_target_execution_stats.packed_decode_lanes == 40);
    REQUIRE(first.is_exact_simd_target_execution_consistent(
        /*require_nonempty_vector_work=*/true));

    match_result_collection missing;
    REQUIRE_FALSE(missing.is_exact_simd_target_execution_consistent(false));

    match_result_collection manufactured_empty;
    manufactured_empty.record_exact_simd_query_execution_stats(
        two_stack_execution_stats{});
    REQUIRE_FALSE(
        manufactured_empty.is_exact_simd_query_execution_consistent(false));

    match_result_collection tampered;
    auto forbidden = actual_range_stats(8);
    forbidden.scalar_hash_symbols = 1;
    tampered.record_exact_simd_target_execution_stats(forbidden);
    REQUIRE_FALSE(tampered.is_exact_simd_target_execution_consistent(false));

    // An actual zero-bp invocation is valid route observations for ordinary
    // diagnostics, but it cannot satisfy the strict positive-work gate.
    match_result_collection empty_actual;
    empty_actual.record_exact_simd_query_execution_stats(
        actual_range_stats(0));
    REQUIRE(empty_actual.is_exact_simd_query_execution_consistent(false));
    REQUIRE_FALSE(empty_actual.is_exact_simd_query_execution_consistent(true));

    auto mixed_routes = actual_range_stats(11);
    mixed_routes.kmer_15_window_10_byte_8_uint32_hash_kernel_calls = 1;
    mixed_routes.kmer_15_window_10_byte_8_uint32_hash_candidate_blocks =
        mixed_routes.generic_u64_candidate_blocks;
    mixed_routes.kmer_15_window_10_byte_8_uint32_hash_lane_visits =
        mixed_routes.generic_u64_lane_visits;
    mixed_routes.generic_u64_candidate_blocks = 0;
    mixed_routes.generic_u64_lane_visits = 0;
    match_result_collection mixed;
    mixed.record_exact_simd_query_execution_stats(mixed_routes);
    REQUIRE_FALSE(mixed.is_exact_simd_query_execution_consistent(false));

    auto missing_packed22 = actual_range_stats(11);
    missing_packed22.kmer_15_window_10_byte_8_uint32_hash_kernel_calls = 1;
    missing_packed22.kmer_15_window_10_byte_8_uint32_hash_candidate_blocks =
        missing_packed22.generic_u64_candidate_blocks;
    missing_packed22.kmer_15_window_10_byte_8_uint32_hash_lane_visits =
        missing_packed22.generic_u64_lane_visits;
    missing_packed22.generic_u64_kernel_calls = 0;
    missing_packed22.generic_u64_candidate_blocks = 0;
    missing_packed22.generic_u64_lane_visits = 0;
    match_result_collection missing_packed;
    missing_packed.record_exact_simd_query_execution_stats(
        missing_packed22);
    REQUIRE_FALSE(
        missing_packed.is_exact_simd_query_execution_consistent(false));

    auto packed22 = missing_packed22;
    packed22.kmer_15_window_10_byte_8_packed_22_groups =
        packed22.kmer_15_window_10_byte_8_uint32_hash_candidate_blocks;
    packed22.kmer_15_window_10_byte_8_packed_22_lane_visits =
        packed22.kmer_15_window_10_byte_8_uint32_hash_lane_visits;
    match_result_collection packed;
    packed.record_exact_simd_query_execution_stats(packed22);
    REQUIRE(packed.is_exact_simd_query_execution_consistent(false));

    ++packed22.kmer_15_window_10_byte_8_packed_22_lane_visits;
    match_result_collection tampered_packed;
    tampered_packed.record_exact_simd_query_execution_stats(packed22);
    REQUIRE_FALSE(
        tampered_packed.is_exact_simd_query_execution_consistent(false));
}

TEST_CASE("partial Vulkan stats requires the exact positive stage mask",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using ::bioseq::simd::match_result_collection;
    using ::bioseq::simd::partial_vulkan_execution_stats;

    match_result_collection tiled_chain;
    partial_vulkan_execution_stats chain_only{};
    chain_only.chain_score_dispatches = 1;
    tiled_chain.record_partial_vulkan_execution_stats(chain_only);
    REQUIRE(tiled_chain.partial_vulkan_stats
                .is_exact_stage_mask_consistent(
                    false, false, false, false, true));

    match_result_collection ordered_sketch;
    partial_vulkan_execution_stats frontend{};
    frontend.query_sketch_dispatches = 1;
    frontend.target_sketch_dispatches = 1;
    frontend.target_index_build_dispatches = 1;
    frontend.gpu_index_exact_count_dispatches = 1;
    frontend.gpu_index_exact_count_prefix_scan_dispatches = 3;
    frontend.gpu_index_exact_count_control_readbacks = 1;
    frontend.gpu_index_deterministic_tile_emit_dispatches = 1;
    frontend.gpu_index_emit_validation_control_readbacks = 1;
    frontend.gpu_index_counted_minimizer_count = 53;
    frontend.gpu_index_verified_emit_count = 53;
    frontend.gpu_index_count_emit_parity_verified = 1;
    frontend.gpu_index_target_minimizer_capacity_estimate = 100;
    frontend.gpu_index_arena_trimmed_before_permit_shrink = 1;
    frontend.gpu_index_ungated_emit_dispatches = 1;
    frontend.gpu_index_retained_minimizer_count = 53;
    frontend.seed_lookup_dispatches = 1;
    frontend.chain_score_dispatches = 1;
    ordered_sketch.record_partial_vulkan_execution_stats(frontend);
    REQUIRE(ordered_sketch.partial_vulkan_stats
                .is_exact_stage_mask_consistent(true, true, true, true, true));
    REQUIRE_FALSE(ordered_sketch.partial_vulkan_stats
                      .is_exact_stage_mask_consistent(
                          true, true, false, true, true));
    auto tampered_frontend = frontend;
    tampered_frontend.gpu_index_atomic_append_dispatches = 1u;
    REQUIRE_FALSE(tampered_frontend.is_exact_stage_mask_consistent(
        true, true, true, true, true));
    tampered_frontend = frontend;
    tampered_frontend.gpu_index_exact_count_prefix_scan_dispatches = 2u;
    REQUIRE_FALSE(tampered_frontend.is_exact_stage_mask_consistent(
        true, true, true, true, true));

    partial_vulkan_execution_stats source_direct{};
    source_direct.query_sketch_dispatches = 1u;
    source_direct.target_sketch_dispatches = 1u;
    source_direct.seed_lookup_dispatches = 1u;
    source_direct.chain_score_dispatches = 1u;
    source_direct.gpu_index_materializations = 1u;
    source_direct.gpu_index_sorted_dense_materializations = 1u;
    source_direct.gpu_index_retained_minimizer_count = 53u;
    source_direct.gpu_index_distinct_minimizer_count = 17u;
    source_direct.gpu_index_source_direct_operations = 1u;
    source_direct.gpu_index_source_direct_planning_receipts = 1u;
    source_direct.gpu_index_source_direct_terminal_receipts = 1u;
    source_direct.gpu_index_source_direct_terminal_successes = 1u;
    REQUIRE(source_direct.is_exact_source_direct_stage_mask_consistent(
        true, true, true, true));
    REQUIRE_FALSE(source_direct.is_exact_stage_mask_consistent(
        true, true, false, true, true));
    source_direct.gpu_index_gate_enabled = 1u;
    REQUIRE_FALSE(source_direct.is_exact_source_direct_stage_mask_consistent(
        true, true, true, true));

    match_result_collection combined;
    combined.combine_execution_stats(tiled_chain);
    REQUIRE(combined.has_any_algorithm_execution_stats());
}

TEST_CASE("ordered sketch batch target-build stats is not inflated by query count",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using ::bioseq::simd::partial_vulkan_execution_stats;
    using ::bioseq::simd::
        is_ordered_target_build_owner_query;

    partial_vulkan_execution_stats aggregate{};
    for (std::size_t query_index = 0; query_index < 3u; ++query_index) {
        partial_vulkan_execution_stats per_query{};
        per_query.query_sketch_dispatches = 1u;
        per_query.seed_lookup_dispatches = 1u;
        per_query.chain_score_dispatches = 1u;
        if (is_ordered_target_build_owner_query(query_index)) {
            per_query.target_sketch_dispatches = 1u;
            per_query.target_index_build_dispatches = 1u;
            per_query.gpu_index_count_admissions = 1u;
            per_query.gpu_index_main_peak_admissions = 1u;
            per_query.gpu_index_resident_transfers = 1u;
            per_query.gpu_index_adoption_successes = 1u;
            per_query.gpu_index_owned_resident_bytes = 4096u;
            per_query.gpu_index_ungated_emit_dispatches = 1u;
            per_query.gpu_index_exact_count_dispatches = 1u;
            per_query.gpu_index_exact_count_prefix_scan_dispatches = 3u;
            per_query.gpu_index_exact_count_control_readbacks = 1u;
            per_query.gpu_index_deterministic_tile_emit_dispatches = 1u;
            per_query.gpu_index_emit_validation_control_readbacks = 1u;
            per_query.gpu_index_counted_minimizer_count = 53u;
            per_query.gpu_index_verified_emit_count = 53u;
            per_query.gpu_index_count_emit_parity_verified = 1u;
            per_query.gpu_index_target_minimizer_capacity_estimate = 100u;
            per_query.gpu_index_arena_trimmed_before_permit_shrink = 1u;
            per_query.gpu_index_retained_minimizer_count = 53u;
            per_query.gpu_index_distinct_minimizer_count = 17u;
        }
        aggregate.merge(per_query);
    }

    REQUIRE(aggregate.query_sketch_dispatches == 3u);
    REQUIRE(aggregate.seed_lookup_dispatches == 3u);
    REQUIRE(aggregate.chain_score_dispatches == 3u);
    REQUIRE(aggregate.target_sketch_dispatches == 1u);
    REQUIRE(aggregate.target_index_build_dispatches == 1u);
    REQUIRE(aggregate.gpu_index_count_admissions == 1u);
    REQUIRE(aggregate.gpu_index_main_peak_admissions == 1u);
    REQUIRE(aggregate.gpu_index_resident_transfers == 1u);
    REQUIRE(aggregate.gpu_index_adoption_successes == 1u);
    REQUIRE(aggregate.gpu_index_gate_enabled == 0u);
    REQUIRE(aggregate.gpu_index_gate_count_dispatches == 0u);
    REQUIRE(aggregate.gpu_index_gate_word_count == 0u);
    REQUIRE(aggregate.gpu_index_gate_mask == 0u);
    REQUIRE(aggregate.gpu_index_gate_set_bits == 0u);
    REQUIRE(aggregate.gpu_index_gated_emit_dispatches == 0u);
    REQUIRE(aggregate.gpu_index_ungated_emit_dispatches == 1u);
    REQUIRE(aggregate.gpu_index_exact_count_dispatches == 1u);
    REQUIRE(aggregate.gpu_index_exact_count_prefix_scan_dispatches == 3u);
    REQUIRE(aggregate.gpu_index_exact_count_control_readbacks == 1u);
    REQUIRE(aggregate.gpu_index_deterministic_tile_emit_dispatches == 1u);
    REQUIRE(aggregate.gpu_index_atomic_append_dispatches == 0u);
    REQUIRE(aggregate.gpu_index_emit_validation_control_readbacks == 1u);
    REQUIRE(aggregate.gpu_index_counted_minimizer_count == 53u);
    REQUIRE(aggregate.gpu_index_verified_emit_count == 53u);
    REQUIRE(aggregate.gpu_index_count_emit_parity_verified == 1u);
    REQUIRE(aggregate.gpu_index_target_minimizer_capacity_estimate == 100u);
    REQUIRE(aggregate.gpu_index_capacity_estimate_underestimated == 0u);
    REQUIRE(aggregate.gpu_index_arena_trimmed_before_permit_shrink == 1u);
    REQUIRE(aggregate.gpu_index_retained_minimizer_count == 53u);
    REQUIRE(aggregate.gpu_index_distinct_minimizer_count == 17u);
}

TEST_CASE("ordered sketch query gate becomes immutable after publication",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using minimizer_type =
        ::bioseq::simd::minimizer_representation<15u, 15u>;
    using storage_type = ::bioseq::simd::pipe::
        ordered_sketch_query_gate_storage;

    storage_type storage;
    const std::array<minimizer_type, 1> minimizers{
        minimizer_type{17u, 0u, 0u}};
    storage.add(std::span<const minimizer_type>{minimizers});
    const auto identity = storage.canonical_identity();
    const auto gate = storage.vulkan_gate();

    REQUIRE(identity.enabled);
    REQUIRE(identity.word_count
            == bioseq::gpu::vulkan::k_gpu_query_gate_word_count);
    REQUIRE(identity.mask == bioseq::gpu::vulkan::k_gpu_query_gate_mask);
    REQUIRE(gate.identity_valid);
    REQUIRE(gate.words.data() == gate.identity.canonical_words->data());
    REQUIRE(gate.canonical_identity().is_exactly_equal_to(gate.identity));
    REQUIRE_THROWS_AS(
        storage.add(std::span<const minimizer_type>{minimizers}),
        bioseq::gpu::vulkan::vulkan_runtime_error);
}

TEST_CASE("segmented GPU adoption key preserves the exact frozen gate",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using gate_identity_type = bioseq::gpu::vulkan::gpu_index_gate_identity;
    using key_type = bioseq::indexing::gpu_minimizer_index_cache_key;
    using ::bioseq::simd::pipe::
        bind_verified_gpu_segment_gate_identity;
    using ::bioseq::simd::pipe::freeze_gpu_query_gate;

    std::vector<std::uint32_t> mutable_words(
        bioseq::gpu::vulkan::k_gpu_query_gate_word_count, 0u);
    mutable_words.front() = 1u;
    const auto frozen_gate = freeze_gpu_query_gate(
        bioseq::gpu::vulkan::vulkan_sketch_query_gate{
            std::span<const std::uint32_t>{mutable_words},
            bioseq::gpu::vulkan::k_gpu_query_gate_mask});
    const gate_identity_type expected = frozen_gate.canonical_identity();
    REQUIRE(expected.canonical_words);
    REQUIRE(frozen_gate.words.data() == expected.canonical_words->data());
    REQUIRE(frozen_gate.words.data() != mutable_words.data());

    std::size_t segment_bind_calls = 0u;
    key_type key{};
    for (std::size_t segment = 0; segment < 3u; ++segment) {
        const gate_identity_type built = frozen_gate.canonical_identity();
        REQUIRE(built.canonical_words.get()
                == expected.canonical_words.get());
        key = key_type{
            .source_id = 7u,
            .caller_identity = 11u + segment,
            .target_len = 101u,
            .table_size = 13u,
            .position_count = 17u,
        };
        bind_verified_gpu_segment_gate_identity(key, expected, built);
        ++segment_bind_calls;
        REQUIRE(key.gate_enabled);
        REQUIRE(key.gate_word_count == expected.word_count);
        REQUIRE(key.gate_mask == expected.mask);
        REQUIRE(key.gate_digest_lo == expected.digest_lo);
        REQUIRE(key.gate_digest_hi == expected.digest_hi);
        REQUIRE(key.gate_canonical_words.get()
                == expected.canonical_words.get());
    }
    REQUIRE(segment_bind_calls == 3u);

    auto mismatched_build = expected;
    mismatched_build.digest_hi ^= 1u;
    REQUIRE_THROWS_AS(
        bind_verified_gpu_segment_gate_identity(
            key, expected, mismatched_build),
        bioseq::gpu::vulkan::vulkan_runtime_error);
}

TEST_CASE("Vulkan sketch dispatch token requires an operation-completed roundtrip",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using bioseq::gpu::vulkan::vulkan_sketch_dispatch_route;
    using bioseq::gpu::vulkan::vulkan_sketch_dispatch_token;

    REQUIRE_FALSE(vulkan_sketch_dispatch_token{}.is_internally_consistent());

    const auto completed =
        vulkan_sketch_dispatch_token::completed_collect_minimizers_roundtrip();
    REQUIRE(completed.is_internally_consistent());

    auto no_roundtrip = completed;
    no_roundtrip.completed_roundtrips = 0;
    REQUIRE_FALSE(no_roundtrip.is_internally_consistent());

    auto wrong_count = completed;
    wrong_count.completed_roundtrips = 2;
    REQUIRE_FALSE(wrong_count.is_internally_consistent());

    auto wrong_route = completed;
    wrong_route.route = vulkan_sketch_dispatch_route::none;
    REQUIRE_FALSE(wrong_route.is_internally_consistent());
}

TEST_CASE("Vulkan sketch stats rejects an absent or default operation token",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using minimizer_algorithm_type = ::bioseq::simd::minimizer_algorithm;
    using ::bioseq::simd::match_result_collection;
    using stats = ::bioseq::simd::pipe_entry_detail::
        minimizer_sketch_execution_stats;
    using ::bioseq::simd::pipe_entry_detail::
        minimizer_sketch_role;
    using ::bioseq::simd::pipe_entry_detail::
        minimizer_sketch_stats_matches_selected_route;
    using ::bioseq::simd::pipe_entry_detail::
        record_minimizer_sketch_execution_stats;

    const stats absent{};
    REQUIRE(absent.empty());
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::vulkan_minimap_integer, absent));

    stats already_promoted_without_token{};
    already_promoted_without_token.partial_vulkan.query_sketch_dispatches = 1;
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::vulkan_minimap_integer, already_promoted_without_token));

    stats metadata_without_token{};
    metadata_without_token.partial_vulkan.gpu_index_unique_keys = 1;
    REQUIRE_FALSE(metadata_without_token.partial_vulkan.has_any_dispatch());
    REQUIRE_FALSE(metadata_without_token.empty());
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::scalar, metadata_without_token));
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::two_stack_nthash, metadata_without_token));
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::bounded_nthash_simd, metadata_without_token));
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::vulkan_minimap_integer, metadata_without_token));

    match_result_collection destination;
    record_minimizer_sketch_execution_stats(
        destination, absent, minimizer_sketch_role::query);
    record_minimizer_sketch_execution_stats(
        destination, already_promoted_without_token,
        minimizer_sketch_role::query);
    record_minimizer_sketch_execution_stats(
        destination, metadata_without_token, minimizer_sketch_role::query);
    REQUIRE_FALSE(destination.partial_vulkan_stats
                      .has_any_dispatch());
}

TEST_CASE("Vulkan sketch stats cannot promote a token across a tampered route",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using minimizer_algorithm_type = ::bioseq::simd::minimizer_algorithm;
    using ::bioseq::simd::match_result_collection;
    using stats = ::bioseq::simd::pipe_entry_detail::
        minimizer_sketch_execution_stats;
    using ::bioseq::simd::pipe_entry_detail::
        minimizer_sketch_role;
    using ::bioseq::simd::pipe_entry_detail::
        minimizer_sketch_stats_matches_selected_route;
    using ::bioseq::simd::pipe_entry_detail::
        record_minimizer_sketch_execution_stats;

    using vulkan_token_type = bioseq::gpu::vulkan::vulkan_sketch_dispatch_token;
    const auto token = vulkan_token_type::completed_collect_minimizers_roundtrip();
    stats vulkan_sketch{};
    vulkan_sketch.partial_vulkan.sketch_dispatches =
        token.completed_roundtrips;
    REQUIRE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::vulkan_minimap_integer, vulkan_sketch));
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::scalar, vulkan_sketch));
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::two_stack_nthash, vulkan_sketch));

    stats mixed_route = vulkan_sketch;
    mixed_route.exact_simd_available = true;
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::vulkan_minimap_integer, mixed_route));

    stats token_with_metadata = vulkan_sketch;
    token_with_metadata.partial_vulkan.gpu_index_unique_keys = 1;
    REQUIRE_FALSE(token_with_metadata.partial_vulkan
                      .is_exact_sketch_token_consistent());
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::vulkan_minimap_integer, token_with_metadata));

    stats repeated_roundtrip = vulkan_sketch;
    repeated_roundtrip.partial_vulkan.sketch_dispatches = 2;
    REQUIRE_FALSE(repeated_roundtrip.partial_vulkan
                      .is_exact_sketch_token_consistent());
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::vulkan_minimap_integer, repeated_roundtrip));

    match_result_collection repeated_roundtrip_destination;
    record_minimizer_sketch_execution_stats(
        repeated_roundtrip_destination, repeated_roundtrip,
        minimizer_sketch_role::query);
    REQUIRE(repeated_roundtrip_destination.partial_vulkan_stats
                == ::bioseq::simd::
                    partial_vulkan_execution_stats{});

    match_result_collection tampered_destination;
    record_minimizer_sketch_execution_stats(
        tampered_destination, token_with_metadata,
        minimizer_sketch_role::query);
    REQUIRE(tampered_destination.partial_vulkan_stats
                == ::bioseq::simd::
                    partial_vulkan_execution_stats{});

    match_result_collection query_destination;
    record_minimizer_sketch_execution_stats(
        query_destination, vulkan_sketch, minimizer_sketch_role::query);
    REQUIRE(query_destination.partial_vulkan_stats
                .is_exact_stage_mask_consistent(
                    true, false, false, false, false));

    match_result_collection target_destination;
    record_minimizer_sketch_execution_stats(
        target_destination, vulkan_sketch, minimizer_sketch_role::target);
    REQUIRE(target_destination.partial_vulkan_stats
                .is_exact_stage_mask_consistent(
                    false, true, false, false, false));
}

TEST_CASE("bounded ntHash sketch stats is distinct and cannot be promoted across routes",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using minimizer_algorithm_type = ::bioseq::simd::minimizer_algorithm;
    using backend_type = ::bioseq::simd::minimizer_sketch_backend;
    using ::bioseq::simd::match_result_collection;
    using ::bioseq::simd::minimap2_parameters;
    using bounded_stats_type = ::bioseq::simd::
        bounded_nthash_execution_stats;
    using range_stats_type = ::bioseq::simd::pipe::
        minimizer_range_execution_stats;
    using stats = ::bioseq::simd::pipe_entry_detail::
        minimizer_sketch_execution_stats;
    using ::bioseq::simd::pipe_entry_detail::
        minimizer_sketch_role;
    using ::bioseq::simd::pipe_entry_detail::
        minimizer_sketch_stats_matches_selected_route;
    using ::bioseq::simd::pipe_entry_detail::
        record_minimizer_sketch_execution_stats;
    using ::bioseq::simd::pipe::
        minimizer_range_stats_matches_backend;
    using ::bioseq::simd::pipe::
        target_index_sketch_backend_from_params;

    bounded_stats_type operation{};
    operation.scheme_id = ::bioseq::simd::
        k_bounded_nthash_scheme_id;
    operation.kmer_size = 15;
    operation.window_size = 10;
    operation.nthash_rotation = 7;
    operation.source_storage_unit_bytes = 4;
    operation.input_symbols = 128;
    operation.input_storage_units = 8;
    operation.segments_combined = 1;
    operation.logical_streams = 8;
    operation.physical_vector_lanes = 4;
    operation.vector_registers_per_step = 2;
    operation.packed_decode_batches = 1;
    operation.packed_decode_lane_visits = 8;
    operation.hash_initialization_batches = 1;
    operation.hash_table_lookup_batches = 2;
    operation.canonical_sum_batches = 1;
    operation.window_update_batches = 1;
    operation.emission_decision_batches = 1;
    operation.emission_mask_store_batches = 1;
    operation.minimizers_emitted = 1;
    operation.scalar_collector_appends = 1;
    operation.positions_are_segment_local = true;
    REQUIRE(operation.is_internally_consistent(true));

    stats bounded{};
    bounded.bounded_nthash = operation;
    bounded.bounded_nthash_available = true;
    REQUIRE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::bounded_nthash_simd, bounded));
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::scalar, bounded));
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::two_stack_nthash, bounded));
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::vulkan_minimap_integer, bounded));

    match_result_collection destination;
    record_minimizer_sketch_execution_stats(
        destination, bounded, minimizer_sketch_role::query);
    record_minimizer_sketch_execution_stats(
        destination, bounded, minimizer_sketch_role::target);
    REQUIRE(destination.is_bounded_nthash_query_execution_consistent(true));
    REQUIRE(destination.is_bounded_nthash_target_execution_consistent(true));
    REQUIRE_FALSE(destination.exact_simd_query_execution_stats_available);
    REQUIRE_FALSE(destination.partial_vulkan_stats.has_any_dispatch());

    range_stats_type bounded_range{};
    bounded_range.bounded_nthash = operation;
    bounded_range.bounded_nthash_available = true;
    REQUIRE(minimizer_range_stats_matches_backend(
        backend_type::bounded_nthash_simd,
        bounded_range,
        /*require_nonempty_vector_work=*/true));
    REQUIRE_FALSE(minimizer_range_stats_matches_backend(
        backend_type::exact_simd, bounded_range));
    REQUIRE_FALSE(minimizer_range_stats_matches_backend(
        backend_type::scalar, bounded_range));
    REQUIRE_FALSE(minimizer_range_stats_matches_backend(
        backend_type::vulkan_segment, bounded_range));

    auto hidden_exact_payload = bounded_range;
    hidden_exact_payload.exact_simd.bp_consumed = 1;
    REQUIRE_FALSE(hidden_exact_payload.exact_simd_available);
    REQUIRE_FALSE(minimizer_range_stats_matches_backend(
        backend_type::bounded_nthash_simd, hidden_exact_payload));

    auto cleared_bounded_flag = bounded_range;
    cleared_bounded_flag.bounded_nthash_available = false;
    REQUIRE_FALSE(minimizer_range_stats_matches_backend(
        backend_type::bounded_nthash_simd, cleared_bounded_flag));

    range_stats_type scalar_range{};
    scalar_range.scalar.record_input(/*symbols=*/128u);
    scalar_range.scalar_available = true;
    REQUIRE(minimizer_range_stats_matches_backend(
        backend_type::scalar, scalar_range));
    REQUIRE_FALSE(minimizer_range_stats_matches_backend(
        backend_type::vulkan_segment, scalar_range));
    scalar_range.bounded_nthash.scheme_id = 1;
    REQUIRE_FALSE(scalar_range.bounded_nthash_available);
    REQUIRE_FALSE(minimizer_range_stats_matches_backend(
        backend_type::scalar, scalar_range));

    range_stats_type scalar_presence_only{};
    scalar_presence_only.scalar_available = true;
    REQUIRE_FALSE(minimizer_range_stats_matches_backend(
        backend_type::scalar, scalar_presence_only));

    minimap2_parameters params{};
    params.minimizer_algorithm = minimizer_algorithm_type::bounded_nthash_simd;
    REQUIRE(target_index_sketch_backend_from_params(params)
        == backend_type::bounded_nthash_simd);
    params.minimizer_algorithm = minimizer_algorithm_type::two_stack_nthash;
    params.chaining_algorithm =
        ::bioseq::simd::chaining_algorithm::scalar_minimap2;
    params.base_alignment_algorithm =
        ::bioseq::simd::base_alignment_algorithm::ksw2;
    REQUIRE(target_index_sketch_backend_from_params(params)
        == backend_type::exact_simd);
}

} // namespace bioseq::simd::pipe
