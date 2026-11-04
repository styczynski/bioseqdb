/**
 * @file dispatch_test.cpp
 * @brief Check how pipeline parameters select compiled algorithms and record sizes.
 */
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.unit.pipeline.pipe_support;
import bioseq.sequences.kmers.minimizer_physical_registry;
import bioseq.sequences.kmers.highway_minimizer_contract;

using namespace bioseq::tests::pipeline::pipe_support;

namespace bioseq::simd::pipe {

using namespace ::bioseq::tests::pipeline::pipe_support;

/// Reuse the existing probe with hash selection preceding its dependent position bucket.
template <auto HashBits, auto PositionBits, auto Alignment>
using hash_position_probe = dispatch_test_support::lkb_probe_leaf<PositionBits, HashBits, Alignment>;

template <class p_query_minimizers>
concept accepts_owned_prebuilt_query = requires(
    ::bioseq::storage::lazy_sequence_view<::seq::dna4> sequence,
    p_query_minimizers&& query_minimizers,
    const ::bioseq::indexing::minimizer_hashmap_fragment_index& index,
    statement_execution_plan_owner plan) {
    ::bioseq::simd::minimap2_port_with_index<
        ::bioseq::minimizers::minimizer_representation<28u, 32u>, ::seq::dna4>(
        sequence, sequence, std::forward<p_query_minimizers>(query_minimizers),
        index, plan);
};

TEST_CASE("minimizer batch retains its checked host subrange without copying records",
          "[component:bioseq][layer:unit][feature:planner]")
{
    using record = ::bioseq::minimizers::minimizer_representation<28u, 32u>;
    using batch = minimizer_batch_view<record>;
    static_assert(!std::is_constructible_v<batch, std::span<const record>>);
    static_assert(!std::is_constructible_v<batch, const std::vector<record>&>);
    static_assert(accepts_owned_prebuilt_query<batch>);
    static_assert(accepts_owned_prebuilt_query<std::shared_ptr<const std::vector<record>>>);
    static_assert(!accepts_owned_prebuilt_query<const std::vector<record>&>);
    static_assert(!accepts_owned_prebuilt_query<std::span<const record>>);
    auto storage = std::make_shared<const std::vector<record>>(3u);
    const auto* original_data = storage->data();
    const std::weak_ptr<const std::vector<record>> lifetime = storage;
    auto view = batch::from_shared_host(storage, 1u, 2u);
    REQUIRE(view.data() == original_data + 1u);
    REQUIRE(view.size() == 2u);
    const std::array<canonical_seed_job_span, 3u> jobs{{
        {0u, 0u, 0u}, {1u, 0u, 2u}, {2u, 2u, 1u}}};
    for (const auto& job : jobs) {
        const auto job_view = batch::from_shared_host(storage, job.offset, job.count);
        REQUIRE(job_view.data() == original_data + job.offset);
        REQUIRE(job_view.size() == job.count);
    }
    REQUIRE_THROWS_AS(batch::from_shared_host(storage, 4u, 0u), std::invalid_argument);
    REQUIRE_THROWS_AS(batch::from_shared_host(storage, 1u, ~std::size_t{0}),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(batch::from_shared_host({}, 0u, 0u), std::invalid_argument);
    storage.reset();
    REQUIRE_FALSE(lifetime.expired());
    REQUIRE(view.data() == original_data + 1u);
    auto retained = std::move(view);
    REQUIRE(retained.data() == original_data + 1u);
    REQUIRE_FALSE(view.has_host_owner());
    REQUIRE(view.size() == 0u);
    {
        using cep_type = static_aligner_parameters<10u, 15u, 28u>;
        std::vector<alignment_region> observed;
        int on_batch_count = 0;
        exact_filter_probe_sink probe{&observed, &on_batch_count};
        divergence_stage<record> divergence{
            {}, std::move(retained), 100u, 100u, 15, false};
        auto bound = divergence.template attach<cep_type>(probe);
        REQUIRE_FALSE(retained.has_host_owner());
        REQUIRE_FALSE(divergence.query_minimizers_.has_host_owner());
        REQUIRE_FALSE(lifetime.expired());
        REQUIRE(bound.self_.query_minimizers_.data() == original_data + 1u);
        REQUIRE(bound.self_.query_minimizers_.size() == 2u);

        std::vector<alignment_region> regions(1u);
        regions.front().mapq = 42;
        const std::vector<::bioseq::chaining::anchor> anchors;
        bound.on_batch_pair(std::span<alignment_region>{regions},
                            std::span<const ::bioseq::chaining::anchor>{anchors});
        bound.on_completed();
        REQUIRE(on_batch_count == 1);
        REQUIRE(observed.size() == 1u);
        REQUIRE(observed.front().mapq == 42);
        REQUIRE_FALSE(lifetime.expired());
    }
    REQUIRE(lifetime.expired());
}

TEST_CASE("statement plan independently classifies all seed and chain selections",
          "[component:bioseq][layer:unit][feature:planner]")
{
    using seed = ::bioseq::simd::minimizer_algorithm;
    using chain = ::bioseq::simd::chaining_algorithm;
    for (const auto selected_seed : {seed::scalar, seed::simd_minimap2_reference,
                                    seed::two_stack_nthash, seed::bounded_nthash_simd,
                                    seed::vulkan_minimap_integer}) {
        for (const auto selected_chain : {chain::scalar_minimap2, chain::simd_minimap2,
                                         chain::range_minimum_query,
                                         chain::vulkan_tiled_dynamic_programming}) {
            ::bioseq::simd::minimap2_parameters params;
            params.minimizer_algorithm = selected_seed;
            params.chaining_algorithm = selected_chain;
            params.vulkan_seed_hits_enabled = selected_seed == seed::vulkan_minimap_integer;
            bind_fixed_highway(params);
            const auto plan = make_statement_execution_plan(std::move(params));
            const bool gpu_seed = selected_seed == seed::vulkan_minimap_integer;
            const bool gpu_chain = selected_chain == chain::vulkan_tiled_dynamic_programming;
            const auto expected = gpu_seed
                ? (gpu_chain ? seed_chain_route::vulkan_seed_vulkan_chain
                             : seed_chain_route::vulkan_seed_host_chain)
                : (gpu_chain ? seed_chain_route::host_seed_vulkan_chain
                             : seed_chain_route::host_seed_host_chain);
            REQUIRE(plan->route == expected);
            REQUIRE(plan->uses_vulkan_seed() == gpu_seed);
            REQUIRE(plan->frozen_parameters.minimizer_algorithm == selected_seed);
            REQUIRE(plan->frozen_parameters.chaining_algorithm == selected_chain);
            const statement_execution_plan_owner retained = plan;
            REQUIRE(retained.get() == plan.get());
            REQUIRE_FALSE(retained.owner_before(plan));
            REQUIRE_FALSE(plan.owner_before(retained));
        }
    }
    ::bioseq::simd::minimap2_parameters invalid;
    invalid.minimizer_algorithm = static_cast<seed>(255);
    REQUIRE_THROWS_AS(make_statement_execution_plan(invalid), std::invalid_argument);
    invalid.minimizer_algorithm = seed::scalar;
    invalid.chaining_algorithm = static_cast<chain>(255);
    REQUIRE_THROWS_AS(make_statement_execution_plan(invalid), std::invalid_argument);
}

#if defined(BIOSEQ_TEST_INDEXES)
TEST_CASE("statement plan route is identical across scan join union admission stats and pipeline entry",
          "[component:bioseq][layer:unit][feature:planner]")
{
    const auto make_guarded_plan = [](downstream_active_wall_tracker& tracker) {
        minimap2_parameters params;
        params.downstream_active_wall = &tracker;
        bind_fixed_highway(params);
        auto plan = make_statement_execution_plan(std::move(params));
        // Bind the pln explicitly in this test. Automatic binding is enabled for
        // end-to-end benchmarks, while ordinary tests can share one timer across
        // several scan plans.
        REQUIRE(tracker.bind_benchmark_e2e_statement_owner(plan));
        return plan;
    };
    const auto attest = [](downstream_active_wall_tracker& tracker,
                           const statement_execution_plan_owner& plan,
                           const benchmark_e2e_statement_plan_checkpoint checkpoint) {
        REQUIRE(tracker.attest_benchmark_e2e_statement_plan_checkpoint(plan, checkpoint));
    };
    const auto record_existing_downstream_work = [](downstream_active_wall_tracker& tracker) {
        tracker.enter();
        tracker.leave();
    };

    SECTION("column scan requires only the common executed route") {
        downstream_active_wall_tracker tracker;
        const auto plan = make_guarded_plan(tracker);
        REQUIRE(tracker.begin_benchmark_e2e_statement_plan_proof(
            plan, benchmark_e2e_statement_plan_shape::column_scan));
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::scan_dispatch);
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::pipeline);
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::statistics);
        record_existing_downstream_work(tracker);
        REQUIRE(tracker.finalize().available);
    }

    SECTION("batch join attests optional union and overlapping completed retries") {
        downstream_active_wall_tracker tracker;
        const auto plan = make_guarded_plan(tracker);
        REQUIRE(tracker.begin_benchmark_e2e_statement_plan_proof(
            plan, benchmark_e2e_statement_plan_shape::batch_join));
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::join_preparation);
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::query_union);
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::scan_dispatch);
        // Two  batch callbacks can request retries before either resumes. Each
        // callback tracks its own pending retry, and both use the same statement plan.
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::retry_request);
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::retry_request);
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::retry_reentry);
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::retry_reentry);
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::pipeline);
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::statistics);
        record_existing_downstream_work(tracker);
        REQUIRE(tracker.finalize().available);
    }

    SECTION("batch join does not require a query union on routes that skip it") {
        downstream_active_wall_tracker tracker;
        const auto plan = make_guarded_plan(tracker);
        REQUIRE(tracker.begin_benchmark_e2e_statement_plan_proof(
            plan, benchmark_e2e_statement_plan_shape::batch_join));
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::join_preparation);
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::scan_dispatch);
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::pipeline);
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::statistics);
        record_existing_downstream_work(tracker);
        REQUIRE(tracker.finalize().available);
    }

    SECTION("final validation rejects a retry request without re-entry") {
        downstream_active_wall_tracker tracker;
        const auto plan = make_guarded_plan(tracker);
        REQUIRE(tracker.begin_benchmark_e2e_statement_plan_proof(
            plan, benchmark_e2e_statement_plan_shape::column_scan));
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::scan_dispatch);
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::retry_request);
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::pipeline);
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::statistics);
        record_existing_downstream_work(tracker);
        REQUIRE_FALSE(tracker.finalize().available);
    }

    SECTION("final validation rejects missing required handoff") {
        downstream_active_wall_tracker tracker;
        const auto plan = make_guarded_plan(tracker);
        REQUIRE(tracker.begin_benchmark_e2e_statement_plan_proof(
            plan, benchmark_e2e_statement_plan_shape::batch_join));
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::scan_dispatch);
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::pipeline);
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::statistics);
        record_existing_downstream_work(tracker);
        REQUIRE_FALSE(tracker.finalize().available);
    }

    SECTION("final validation observes but does not retain owner lifetime") {
        downstream_active_wall_tracker tracker;
        auto plan = make_guarded_plan(tracker);
        REQUIRE(tracker.begin_benchmark_e2e_statement_plan_proof(
            plan, benchmark_e2e_statement_plan_shape::column_scan));
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::scan_dispatch);
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::pipeline);
        attest(tracker, plan, benchmark_e2e_statement_plan_checkpoint::statistics);
        record_existing_downstream_work(tracker);
        plan.reset();
        REQUIRE_FALSE(tracker.finalize().available);
    }

    SECTION("equal parameters in a different control block are rejected") {
        downstream_active_wall_tracker tracker;
        const auto plan = make_guarded_plan(tracker);
        auto duplicate_parameters = plan->frozen_parameters;
        const auto duplicate =
            make_statement_execution_plan(std::move(duplicate_parameters));
        REQUIRE(duplicate.get() != plan.get());
        REQUIRE_FALSE(tracker.bind_benchmark_e2e_statement_owner(duplicate));
    }

    SECTION("ordinary test plans may share timing without automatic campaign binding") {
        downstream_active_wall_tracker tracker;
        minimap2_parameters params;
        params.downstream_active_wall = &tracker;
        bind_fixed_highway(params);
        const auto first = make_statement_execution_plan(params);
        const auto second = make_statement_execution_plan(std::move(params));
        REQUIRE(first.get() != second.get());
        record_existing_downstream_work(tracker);
        REQUIRE(tracker.finalize().available);
    }
}
#endif

TEST_CASE("dispatch_exact_value_or_default falls back to DefaultValue",
          "[component:bioseq][layer:unit][feature:planner]")
{
    using dispatch_test_support::base_alignment_algorithm_type;
    using dispatch_test_support::chaining_algorithm_type;
    using dispatch_test_support::minimizer_algorithm_type;

    SECTION("chaining_algorithm — out-of-range routes to scalar_minimap2") {
        using registry_type = algorithm_enum_values<chaining_algorithm_type>;
        chaining_algorithm_type captured = static_cast<chaining_algorithm_type>(0x00);
        dispatch_test_support::value_capture_callable<chaining_algorithm_type> cb{&captured};
        dispatch_exact_value_or_default<chaining_algorithm_type, chaining_algorithm_type, registry_type::default_value>(
            registry_type::value_list{}, static_cast<chaining_algorithm_type>(0xFF), cb);
        REQUIRE(captured == chaining_algorithm_type::scalar_minimap2);
    }
    SECTION("base_alignment_algorithm — out-of-range routes to block_aligner") {
        using registry_type = algorithm_enum_values<base_alignment_algorithm_type>;
        base_alignment_algorithm_type captured = static_cast<base_alignment_algorithm_type>(0x00);
        dispatch_test_support::value_capture_callable<base_alignment_algorithm_type> cb{&captured};
        dispatch_exact_value_or_default<base_alignment_algorithm_type, base_alignment_algorithm_type, registry_type::default_value>(
            registry_type::value_list{}, static_cast<base_alignment_algorithm_type>(0xFF), cb);
        REQUIRE(captured == base_alignment_algorithm_type::block_aligner);
    }
    SECTION("minimizer_algorithm — out-of-range routes to SCALAR") {
        using registry_type = algorithm_enum_values<minimizer_algorithm_type>;
        minimizer_algorithm_type captured = static_cast<minimizer_algorithm_type>(0x00);
        dispatch_test_support::value_capture_callable<minimizer_algorithm_type> cb{&captured};
        dispatch_exact_value_or_default<minimizer_algorithm_type, minimizer_algorithm_type, registry_type::default_value>(
            registry_type::value_list{}, static_cast<minimizer_algorithm_type>(0xFF), cb);
        REQUIRE(captured == minimizer_algorithm_type::scalar);
    }
}

TEST_CASE("dispatch_smallest_not_less picks the right bucket",
          "[component:bioseq][layer:unit][feature:planner]")
{
    // Exercise every requested position width against the generated union of
    // physical capacities, including the helper's documented final-bucket fallback.
    for (std::size_t requested = 1u; requested <= 64u; ++requested) {
        std::size_t expected = seq_len_log2_buckets[seq_len_log2_buckets_n - 1u];
        for (std::size_t index = 0u; index < seq_len_log2_buckets_n; ++index) {
            if (seq_len_log2_buckets[index] >= requested) {
                expected = seq_len_log2_buckets[index];
                break;
            }
        }
        std::size_t captured = 0u;
        dispatch_test_support::value_capture_callable<std::size_t> callback{&captured};
        dispatch_smallest_not_less<std::size_t, std::size_t>(
            detail::seq_len_log2_buckets_seq{}, requested, callback);
        REQUIRE(captured == expected);
    }
}

TEST_CASE("round_up_seq_len_log2 measures positions independently of k",
          "[component:bioseq][layer:unit][feature:planner]")
{
    static_assert(round_up_seq_len_log2(0u) == 1u);
    static_assert(round_up_seq_len_log2(1u) == 1u);
    static_assert(round_up_seq_len_log2(std::size_t{1} << 28u) == 28u);
    static_assert(round_up_seq_len_log2((std::size_t{1} << 48u) + 1u) == 49u);
    static_assert(round_up_seq_len_log2(std::size_t{1} << 62u) == 62u);
    REQUIRE_THROWS_AS(round_up_seq_len_log2(~std::size_t{0}), std::length_error);
}

TEST_CASE("native hash width is the first generated hash bucket",
          "[component:bioseq][layer:unit][feature:planner]")
{
    static_assert(hash_bits_default == std::numeric_limits<std::uint32_t>::digits);
    static_assert(hash_bits_default == hash_bits_buckets[0]);
    SUCCEED();
}

TEST_CASE("algorithm_enum_values registry round-trips through algorithm_traits",
          "[component:bioseq][layer:unit][feature:planner]")
{
    static_assert(detail::all_traits_specialised_for(
        algorithm_enum_values<
            ::bioseq::simd::chaining_algorithm>::value_list{}));
    static_assert(detail::all_traits_specialised_for(
        algorithm_enum_values<
            ::bioseq::simd::base_alignment_algorithm>::value_list{}));
    static_assert(detail::all_traits_specialised_for(
        algorithm_enum_values<
            ::bioseq::simd::minimizer_algorithm>::value_list{}));
    SUCCEED();
}

TEST_CASE("dispatch_chain pins NTTPs in chain order",
          "[component:bioseq][layer:unit][feature:planner]")
{
    using dispatch_test_support::base_alignment_algorithm_type;
    using dispatch_test_support::chaining_algorithm_type;
    using dispatch_test_support::minimizer_algorithm_type;
    using dispatch_test_support::bca_probe_traits;

    ::bioseq::simd::minimap2_parameters params;

    constexpr std::array<base_alignment_algorithm_type, 3> baas = {
        base_alignment_algorithm_type::ksw2, base_alignment_algorithm_type::block_aligner, base_alignment_algorithm_type::ksw2_batched,
    };
    constexpr std::array<chaining_algorithm_type, 4> cas = {
        chaining_algorithm_type::scalar_minimap2, chaining_algorithm_type::simd_minimap2, chaining_algorithm_type::range_minimum_query, chaining_algorithm_type::vulkan_tiled_dynamic_programming,
    };
    constexpr std::array<minimizer_algorithm_type, 3> mas = {
        minimizer_algorithm_type::scalar, minimizer_algorithm_type::simd_minimap2_reference, minimizer_algorithm_type::two_stack_nthash,
    };

    for (base_alignment_algorithm_type baa_v : baas) for (chaining_algorithm_type ca_v : cas) for (minimizer_algorithm_type ma_v : mas) {
        params.base_alignment_algorithm = baa_v;
        params.chaining_algorithm       = ca_v;
        params.minimizer_algorithm      = ma_v;
        base_alignment_algorithm_type baa_out = static_cast<base_alignment_algorithm_type>(0xFF);
        chaining_algorithm_type  ca_out  = static_cast<chaining_algorithm_type>(0xFF);
        minimizer_algorithm_type  ma_out  = static_cast<minimizer_algorithm_type>(0xFF);
        const int r = detail::dispatch_chain<
            int,
            bca_probe_traits::template leaf,
            detail::baa_step,
            detail::ca_step,
            detail::ma_step
        >::template run<>(params, baa_out, ca_out, ma_out);
        REQUIRE(r == 1);
        REQUIRE(baa_out == baa_v);
        REQUIRE(ca_out  == ca_v);
        REQUIRE(ma_out  == ma_v);
    }
}

TEST_CASE("dispatch_chain selects dependent hash and position dimensions",
          "[component:bioseq][layer:unit][feature:planner]")
{
    using dispatch_test_support::base_alignment_algorithm_type;
    ::bioseq::simd::minimap2_parameters params;
    params.minimizer_algorithm = ::bioseq::simd::minimizer_algorithm::scalar;
    params.window_size = 10;
    params.base_alignment_algorithm = base_alignment_algorithm_type::ksw2;
    std::vector<seq::dna4> query_data(1u << 10u);
    std::vector<seq::dna4> target_data(1u << 14u);
    compact_sequence_range<std::vector<seq::dna4>> query{query_data};
    compact_sequence_range<std::vector<seq::dna4>> target{target_data};
    for (const auto kmer_size : {7, 15}) {
        params.kmer_size = kmer_size;
        std::size_t position_bits = 0u, hash_bits = 0u;
        auto alignment = static_cast<base_alignment_algorithm_type>(0xFF);
        const auto result = detail::dispatch_chain<
            int, hash_position_probe, detail::hash_bits_step,
            detail::seq_len_log2_step_per_hash, detail::baa_step
        >::template run<>(params, query, target, position_bits, hash_bits, alignment);
        REQUIRE(result == 1);
        REQUIRE(hash_bits == (kmer_size == 7 ? 14u : 32u));
        REQUIRE(position_bits == (kmer_size == 7 ? 17u : 15u));
        REQUIRE(alignment == base_alignment_algorithm_type::ksw2);
    }
}

TEST_CASE("dispatch_chain skips inactive steps via guarded_step",
          "[component:bioseq][layer:unit][feature:planner]")
{
    using dispatch_test_support::base_alignment_algorithm_type;
    using dispatch_test_support::chaining_algorithm_type;
    using dispatch_test_support::minimizer_algorithm_type;
    using dispatch_test_support::bca_probe_traits;

    ::bioseq::simd::minimap2_parameters params;
    // An inactive minimizer step must select its scalar default even when
    // the supplied algorithm value is  invalid.
    params.minimizer_algorithm      = static_cast<minimizer_algorithm_type>(0xFF);
    params.base_alignment_algorithm = base_alignment_algorithm_type::block_aligner;
    params.chaining_algorithm       = chaining_algorithm_type::scalar_minimap2;

    base_alignment_algorithm_type baa_out = static_cast<base_alignment_algorithm_type>(0xFF);
    chaining_algorithm_type  ca_out  = static_cast<chaining_algorithm_type>(0xFF);
    minimizer_algorithm_type  ma_out  = static_cast<minimizer_algorithm_type>(0xFF);
    detail::dispatch_chain<
        int,
        bca_probe_traits::template leaf,
        detail::baa_step,
        detail::ca_step,
        detail::guarded_step<detail::ma_step, /*active=*/false>
    >::template run<>(params, baa_out, ca_out, ma_out);
    REQUIRE(baa_out == base_alignment_algorithm_type::block_aligner);
    REQUIRE(ca_out  == chaining_algorithm_type::scalar_minimap2);
    REQUIRE(ma_out  == minimizer_algorithm_type::scalar);
}

TEST_CASE("dispatch_chain handles void return",
          "[component:bioseq][layer:unit][feature:planner]")
{
    using dispatch_test_support::minimizer_algorithm_type;
    using dispatch_test_support::void_probe_traits;

    ::bioseq::simd::minimap2_parameters params;
    for (minimizer_algorithm_type v : {minimizer_algorithm_type::scalar, minimizer_algorithm_type::simd_minimap2_reference, minimizer_algorithm_type::two_stack_nthash}) {
        params.minimizer_algorithm = v;
        minimizer_algorithm_type captured = static_cast<minimizer_algorithm_type>(0xFF);
        detail::dispatch_chain<
            void,
            void_probe_traits::template leaf,
            detail::ma_step
        >::template run<>(params, captured);
        REQUIRE(captured == v);
    }
}

TEST_CASE("pipeline_run_leaf::call routes Cep on (W, K) runtime match",
          "[component:bioseq][layer:unit][feature:planner]")
{
    using dispatch_test_support::base_alignment_algorithm_type;
    using dispatch_test_support::chaining_algorithm_type;
    using dispatch_test_support::minimizer_algorithm_type;

    SECTION("(W=10, K=15) — selects the specialised Cep") {
        // Call the matching parameter branch with empty minimizers and check, for
        // an empty result This test cannot directly observe which compiled
        // parameter type the pipeline selected.
        ::bioseq::simd::minimap2_parameters params;
        bind_fixed_highway(params);
        params.window_size = 10;
        params.kmer_size   = 15;
        params.base_alignment_algorithm = base_alignment_algorithm_type::block_aligner;
        params.chaining_algorithm       = chaining_algorithm_type::scalar_minimap2;
        params.minimizer_algorithm      = minimizer_algorithm_type::scalar;
        std::vector<seq::dna4> q_data(64);
        std::vector<seq::dna4> t_data(64);
        compact_sequence_range<std::vector<seq::dna4>> q{q_data};
        compact_sequence_range<std::vector<seq::dna4>> t{t_data};
        auto q_owner = bioseq::storage::make_resident_lazy_sequence(q);
        auto t_owner = bioseq::storage::make_resident_lazy_sequence(t);
        const auto q_view = bioseq::storage::make_lazy_sequence_view(q_owner);
        const auto t_view = bioseq::storage::make_lazy_sequence_view(t_owner);
        using minimizer_type =
            ::bioseq::simd::minimizer_representation<15u, 32u>;
        const auto minimizers_owner =
            std::make_shared<const std::vector<minimizer_type>>();
        const auto mins = minimizer_batch_view<minimizer_type>::from_shared_host(
            minimizers_owner, 0u, 0u);
        const auto plan = make_statement_execution_plan(std::move(params));
        ::bioseq::indexing::minimizer_hashmap_fragment_index idx;
        idx.build(*minimizers_owner, t.bp_size());
        // W=10 and K=15 match the specialized parameters when that build option is enabled.
        auto result = detail::pipeline_run_leaf<
            pipeline_entry_point::with_prebuilt_minimizers,
            seq::dna4,
            minimizer_type, ::bioseq::indexing::minimizer_hashmap_fragment_index,
            15u, 32u,
            base_alignment_algorithm_type::block_aligner, chaining_algorithm_type::scalar_minimap2, minimizer_algorithm_type::scalar>::call(
                plan->frozen_parameters, q_view, t_view, mins, idx, plan,
                nullptr, ::bioseq::indexing::gpu_index_cache_identity{}, false);
        REQUIRE(result.size() == 0u);
    }
    SECTION("(W=5, K=15) — selects the fallback Cep") {
        ::bioseq::simd::minimap2_parameters params;
        bind_fixed_highway(params);
        params.window_size = 5;
        params.kmer_size   = 15;
        params.base_alignment_algorithm = base_alignment_algorithm_type::block_aligner;
        params.chaining_algorithm       = chaining_algorithm_type::scalar_minimap2;
        params.minimizer_algorithm      = minimizer_algorithm_type::scalar;
        std::vector<seq::dna4> q_data(64);
        std::vector<seq::dna4> t_data(64);
        compact_sequence_range<std::vector<seq::dna4>> q{q_data};
        compact_sequence_range<std::vector<seq::dna4>> t{t_data};
        auto q_owner = bioseq::storage::make_resident_lazy_sequence(q);
        auto t_owner = bioseq::storage::make_resident_lazy_sequence(t);
        const auto q_view = bioseq::storage::make_lazy_sequence_view(q_owner);
        const auto t_view = bioseq::storage::make_lazy_sequence_view(t_owner);
        using minimizer_type =
            ::bioseq::simd::minimizer_representation<15u, 32u>;
        const auto minimizers_owner =
            std::make_shared<const std::vector<minimizer_type>>();
        const auto mins = minimizer_batch_view<minimizer_type>::from_shared_host(
            minimizers_owner, 0u, 0u);
        const auto plan = make_statement_execution_plan(std::move(params));
        ::bioseq::indexing::minimizer_hashmap_fragment_index idx;
        idx.build(*minimizers_owner, t.bp_size());
        auto result = detail::pipeline_run_leaf<
            pipeline_entry_point::with_prebuilt_minimizers,
            seq::dna4,
            minimizer_type, ::bioseq::indexing::minimizer_hashmap_fragment_index,
            15u, 32u,
            base_alignment_algorithm_type::block_aligner, chaining_algorithm_type::scalar_minimap2, minimizer_algorithm_type::scalar>::call(
                plan->frozen_parameters, q_view, t_view, mins, idx, plan,
                nullptr, ::bioseq::indexing::gpu_index_cache_identity{}, false);
        REQUIRE(result.size() == 0u);
    }
}

TEST_CASE("runtime k selects hash capacity without becoming a template axis",
          "[component:bioseq][layer:unit][feature:planner]")
{
    using dispatch_test_support::base_alignment_algorithm_type;
    ::bioseq::simd::minimap2_parameters params;
    params.minimizer_algorithm = ::bioseq::simd::minimizer_algorithm::scalar;
    params.window_size = 10;
    params.base_alignment_algorithm = base_alignment_algorithm_type::block_aligner;
    for (const auto kmer_size : {11, 21}) {
        params.kmer_size = kmer_size;
        for (const auto sequence_size : {std::size_t{1u} << 10u, std::size_t{1u} << 20u}) {
            std::vector<seq::dna4> query_data(sequence_size);
            std::vector<seq::dna4> target_data(sequence_size);
            compact_sequence_range<std::vector<seq::dna4>> query{query_data};
            compact_sequence_range<std::vector<seq::dna4>> target{target_data};
            std::size_t position_bits = 0u, hash_bits = 0u;
            auto alignment = static_cast<base_alignment_algorithm_type>(0xFF);
            detail::dispatch_chain<
                int, hash_position_probe, detail::hash_bits_step,
                detail::seq_len_log2_step_per_hash, detail::baa_step
            >::template run<>(params, query, target, position_bits, hash_bits, alignment);
            // K21's 42 rank bits fit the registered compact 43-bit hash field.
            REQUIRE(hash_bits == (kmer_size == 11 ? 32u : 43u));
            const auto expected_position_bits = kmer_size == 21 ? 20u
                : sequence_size <= (std::size_t{1u} << 15u) ? 15u : 28u;
            REQUIRE(position_bits == expected_position_bits);
            REQUIRE(alignment == base_alignment_algorithm_type::block_aligner);
        }
    }
}

TEST_CASE("dispatch_chain rejects rvalue args at compile time",
          "[component:bioseq][layer:unit][feature:planner]")
{
    using dispatch_test_support::base_alignment_algorithm_type;
    using dispatch_test_support::chaining_algorithm_type;
    using dispatch_test_support::minimizer_algorithm_type;

    // lvalue references are accepted.
    static_assert(dispatch_test_support::chain_accepts_v<base_alignment_algorithm_type&, chaining_algorithm_type&, minimizer_algorithm_type&>);
    static_assert(dispatch_test_support::chain_accepts_v<const base_alignment_algorithm_type&, const chaining_algorithm_type&, const minimizer_algorithm_type&>);
    // Values passed without references are rejected.
    static_assert(!dispatch_test_support::chain_accepts_v<base_alignment_algorithm_type, chaining_algorithm_type, minimizer_algorithm_type>);
    // Rvalue references are rejected.
    static_assert(!dispatch_test_support::chain_accepts_v<base_alignment_algorithm_type&&, chaining_algorithm_type&&, minimizer_algorithm_type&&>);
    SUCCEED();
}

TEST_CASE("algorithm-aware k20 layout separates hash width from runtime biological k w",
          "[component:bioseq][layer:unit][feature:planner]") {
    using namespace ::bioseq::minimizers;
    using algorithm = ::bioseq::simd::minimizer_algorithm;
    for (const auto selected : {algorithm::bounded_nthash_simd, algorithm::two_stack_nthash}) {
        // Bounded ntHash keeps 32-bit hashes at runtime k=20. Two-stack ntHash
        // uses a wider record so query and index hashes use the same mask.
        const auto expected_hash_bits =
            selected == algorithm::bounded_nthash_simd ? 32u : 40u;
        const auto expected_position_log2 =
            selected == algorithm::bounded_nthash_simd ? 28u : 63u;
        const auto layout = select_minimizer_physical_layout<seq::dna4, std::uint32_t>(
            selected, std::uint64_t{1u} << 28u, 20u, 15u);
        REQUIRE(layout.hash_bits == expected_hash_bits);
        REQUIRE(layout.record_position_log2 == expected_position_log2);
        REQUIRE(layout.runtime_kmer_size == 20u);
        REQUIRE(layout.runtime_window_size == 15u);
        REQUIRE(layout.effective_algorithm == selected);
        const auto hash_bits = visit_minimizer_physical_layout(
            layout, []<class record>() { return record::layout_type::k_hash_bits; });
        REQUIRE(hash_bits == expected_hash_bits);
        const auto wide = select_minimizer_physical_layout<seq::dna5, std::uint64_t>(
            selected, (std::uint64_t{1u} << 28u) + 1u, 20u, 15u);
        REQUIRE(wide.record_position_log2
                == (selected == algorithm::bounded_nthash_simd ? 31u : 63u));
        REQUIRE(wide.hash_bits == expected_hash_bits);
    }
    const auto reference = select_minimizer_physical_layout<seq::dna4, std::uint64_t>(
        algorithm::simd_minimap2_reference, (std::uint64_t{1u} << 20u) + 1u, 20u, 15u);
    REQUIRE(reference.record_position_log2 == 23u);
    REQUIRE(reference.hash_bits == 40u);
    REQUIRE(reference.hash_width == minimizer_scalar_width::u64);
    auto mismatched = reference;
    mismatched.hash_width = minimizer_scalar_width::u32;
    REQUIRE_THROWS_AS(visit_minimizer_physical_layout(
        mismatched, []<class record>() { return sizeof(record); }), std::invalid_argument);
    const auto vulkan = select_minimizer_physical_layout<seq::dna5, std::uint64_t>(
        algorithm::vulkan_minimap_integer, 4'000'000'000u, 20u, 15u);
    REQUIRE(vulkan.requested_algorithm == algorithm::vulkan_minimap_integer);
    REQUIRE(vulkan.effective_algorithm == algorithm::vulkan_minimap_integer);
    REQUIRE(vulkan.hash_bits == 40u);
    REQUIRE(vulkan.record_position_log2 == 63u);
    const auto promoted = select_minimizer_physical_layout<seq::dna4, std::uint32_t>(
        algorithm::simd_minimap2_reference, 1024u, 20u, 15u);
    REQUIRE(promoted.hash_bits == 40u);
    REQUIRE(promoted.hash_width == minimizer_scalar_width::u64);
    REQUIRE(promoted.record_position_log2 == 23u);
    const auto dna5 = select_minimizer_physical_layout<seq::dna5, std::uint32_t>(
        algorithm::simd_minimap2_reference, 1024u, 20u, 15u);
    REQUIRE(dna5.requested_algorithm == algorithm::simd_minimap2_reference);
    REQUIRE(dna5.effective_algorithm == algorithm::simd_minimap2_reference);
    REQUIRE(dna5.hash_bits == 40u);
    // Exact layouts keep the established compact coordinate envelope without
    // widening ordinary k15 hash transport or narrowing explicit bounded32 hashes.
    for (const auto selected : {algorithm::scalar, algorithm::simd_minimap2_reference,
                               algorithm::two_stack_nthash, algorithm::vulkan_minimap_integer}) {
        const auto ordinary = select_minimizer_physical_layout<seq::dna4, std::uint32_t>(
            selected, std::uint64_t{1u} << 28u, 15u, 10u);
        REQUIRE(ordinary.hash_bits == 32u);
        REQUIRE(ordinary.hash_width == minimizer_scalar_width::u32);
        const auto compact = select_minimizer_physical_layout<seq::dna4, std::uint32_t>(
            selected, std::uint64_t{1u} << 33u, 15u, 10u);
        REQUIRE(compact.hash_bits == 30u);
        REQUIRE(compact.record_position_log2 == 33u);
        REQUIRE(compact.hash_width == minimizer_scalar_width::u32);
        REQUIRE(visit_minimizer_physical_layout(compact,
            []<class record>() { return sizeof(record); }) == sizeof(std::uint64_t));
        const auto split = select_minimizer_physical_layout<seq::dna4, std::uint32_t>(
            selected, (std::uint64_t{1u} << 33u) + 1u, 15u, 10u);
        REQUIRE(visit_minimizer_physical_layout(split,
            []<class record>() { return sizeof(record); }) == 2u * sizeof(std::uint64_t));
    }
    for (const auto [kmer_size, position_bits] :
         std::array{std::pair{17u, 28u}, std::pair{21u, 20u}, std::pair{24u, 15u}}) {
        const auto compact = select_minimizer_physical_layout<seq::dna4, std::uint32_t>(
            algorithm::two_stack_nthash, std::uint64_t{1u} << position_bits,
            static_cast<std::uint16_t>(kmer_size), 10u);
        REQUIRE(compact.hash_width == minimizer_scalar_width::u64);
        REQUIRE(visit_minimizer_physical_layout(compact,
            []<class record>() { return sizeof(record); }) == sizeof(std::uint64_t));
    }
    const auto bounded = select_minimizer_physical_layout<seq::dna4, std::uint32_t>(
        algorithm::bounded_nthash_simd, std::uint64_t{1u} << 33u, 15u, 10u);
    REQUIRE(bounded.hash_bits == std::numeric_limits<std::uint32_t>::digits);
    REQUIRE(bounded.record_position_log2 == 63u);
    REQUIRE_THROWS_AS((select_minimizer_physical_layout<seq::dna4, std::uint64_t>(
        algorithm::bounded_nthash_simd, (std::uint64_t{1u} << 63u) + 1u, 20u, 15u)),
        std::length_error);
}

} // namespace bioseq::simd::pipe
