/**
 * @file dispatch_test.cpp
 * @brief Exercises pipeline compile-time dispatch geometry.
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

template <class p_query_minimizers>
concept accepts_owned_prebuilt_query = requires(
    ::bioseq::storage::lazy_sequence_view<::seq::dna4> sequence,
    p_query_minimizers&& query_minimizers,
    const ::bioseq::indexing::minimizer_hashmap_fragment_index& index,
    statement_execution_plan_owner plan) {
    ::bioseq::simd::minimap2_port_with_index<
        ::bioseq::minimizers::minimizer_representation<28u, 15u>, ::seq::dna4>(
        sequence, sequence, std::forward<p_query_minimizers>(query_minimizers),
        index, plan);
};

TEST_CASE("minimizer batch retains its checked host subrange without copying records",
          "[component:bioseq][layer:unit][feature:planner]")
{
    using record = ::bioseq::minimizers::minimizer_representation<28u, 15u>;
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
        // Automatic binding is E2E-only. Ordinary TEST executors can contain
        // multiple scan-node plans sharing one downstream timing union.
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
        // Distinct native-batch callbacks can overlap. Their pending edges stay
        // callback-local; the statement tracker accepts each exact-owner edge.
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
    // The bucket list is the union of `compute_breakpoints_for<K>()` across
    // every K in `kmer_size_buckets`. The extended-k-mer capability selects
    // {15, 7, 27}, whose union is L = {7, 15, 17, 27, 33, 49, 63}.
    // Baseline profiles select {15}, collapsing the union to K=15's per-K
    // breakpoints. Gate the table so both specialization domains are covered.
    if constexpr (kmer_size_buckets.size() >= 3u) {
        static constexpr std::array<std::pair<std::size_t, std::size_t>, 15u> cases{{
            { 1u,   7u}, { 7u,   7u}, { 8u,  15u}, {15u, 15u}, {16u, 17u},
            {17u,  17u}, {18u,  27u}, {27u,  27u}, {28u, 33u}, {33u, 33u},
            {34u,  49u}, {49u,  49u}, {50u,  63u}, {63u, 63u}, {64u, 63u},
        }};
        for (const auto& [want, expected] : cases) {
            std::size_t captured = 0u;
            dispatch_test_support::value_capture_callable<std::size_t> cb{&captured};
            dispatch_smallest_not_less<std::size_t, std::size_t>(
                detail::seq_len_log2_buckets_seq{}, want, cb);
            REQUIRE(captured == expected);
        }
    } else {
        // Reduced-branching mode (K = {15}). Reuse the K=15 breakpoints:
        // dispatch_smallest_not_less picks the smallest L breakpoint >= want.
        // Verify the contract holds for a few representative `want` values
        // — the exact breakpoint set is materialised by
        // `compute_breakpoints_for<15>()` so we walk the same sequence.
        constexpr auto bp = detail::compute_breakpoints_for<15u>();
        REQUIRE(bp.second > 0u);
        for (std::size_t i = 0u; i < bp.second; ++i) {
            const std::size_t want = bp.first[i];
            std::size_t captured = 0u;
            dispatch_test_support::value_capture_callable<std::size_t> cb{&captured};
            dispatch_smallest_not_less<std::size_t, std::size_t>(
                detail::seq_len_log2_buckets_seq{}, want, cb);
            REQUIRE(captured >= want);
        }
    }
}

TEST_CASE("round_up_seq_len_log2 produces canonical L",
          "[component:bioseq][layer:unit][feature:planner]")
{
    SECTION("Compile-time properties") {
        static_assert(round_up_seq_len_log2(0u,            15u) == 15u);
        static_assert(round_up_seq_len_log2(1u,            15u) == 15u);
        static_assert(round_up_seq_len_log2(std::size_t{1} << 28, 15u) == 28u);
        static_assert(round_up_seq_len_log2((std::size_t{1} << 48) + 1u, 15u) == 49u);
        static_assert(round_up_seq_len_log2(std::size_t{1} << 62, 15u) == 62u);
        SUCCEED();
    }
    SECTION("Clamps below K") {
        REQUIRE(round_up_seq_len_log2(1u, 27u) == 27u);
        REQUIRE(round_up_seq_len_log2(1u, 7u)  == 7u);
    }
    SECTION("Clamps above 63") {
        REQUIRE(round_up_seq_len_log2(~std::size_t{0}, 15u) == 63u);
    }
}

TEST_CASE("kmer_size_default matches buckets[0]",
          "[component:bioseq][layer:unit][feature:planner]")
{
    static_assert(kmer_size_default == 15u);
    static_assert(kmer_size_default == kmer_size_buckets[0]);
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

TEST_CASE("dispatch_chain heterogeneous NTTPs (L, K, BAA)",
          "[component:bioseq][layer:unit][feature:planner]")
{
    using dispatch_test_support::base_alignment_algorithm_type;
    using dispatch_test_support::lkb_probe_traits;

    ::bioseq::simd::minimap2_parameters params;
    // Build two dummy sequences of differing sizes so seq_len_log2_step
    // picks a non-trivial L.
    std::vector<seq::dna4> q_data(1u << 10);
    std::vector<seq::dna4> t_data(1u << 14);
    compact_sequence_range<std::vector<seq::dna4>> q{q_data};
    compact_sequence_range<std::vector<seq::dna4>> t{t_data};

    SECTION("K=15, BAA=KSW2 — picks the L bucket from max_bp") {
        params.kmer_size                = 15;
        params.base_alignment_algorithm = base_alignment_algorithm_type::ksw2;
        std::size_t l_out = 0u, k_out = 0u;
        base_alignment_algorithm_type baa_out = static_cast<base_alignment_algorithm_type>(0xFF);
        const int r = detail::dispatch_chain<
            int,
            lkb_probe_traits::template leaf,
            detail::seq_len_log2_step,
            detail::kmer_size_step,
            detail::baa_step
        >::template run<>(params, q, t, l_out, k_out, baa_out);
        REQUIRE(r == 1);
        REQUIRE(k_out == 15u);
        REQUIRE(l_out == 15u);  // round_up_seq_len_log2(max(1<<10, 1<<14), 15) = 15
        REQUIRE(baa_out == base_alignment_algorithm_type::ksw2);
    }
    // Gated by `kmer_size_buckets.size()`: only run when the extended-k-mer
    // capability compiles the K=7 dispatch branch. Baseline profiles contain
    // only {15u}, so their smallest-ge fallback is K=15.
    if constexpr (kmer_size_buckets.size() >= 3u) {
        SECTION("K=7 — picks the K=7 bucket and L=7 from short sequences") {
            std::vector<seq::dna4> q_small(64);
            std::vector<seq::dna4> t_small(64);
            compact_sequence_range<std::vector<seq::dna4>> q_sm{q_small};
            compact_sequence_range<std::vector<seq::dna4>> t_sm{t_small};
            params.kmer_size                = 7;
            params.base_alignment_algorithm = base_alignment_algorithm_type::block_aligner;
            std::size_t l_out = 0u, k_out = 0u;
            base_alignment_algorithm_type baa_out = static_cast<base_alignment_algorithm_type>(0xFF);
            detail::dispatch_chain<
                int,
                lkb_probe_traits::template leaf,
                detail::seq_len_log2_step,
                detail::kmer_size_step,
                detail::baa_step
            >::template run<>(params, q_sm, t_sm, l_out, k_out, baa_out);
            REQUIRE(k_out == 7u);
            REQUIRE(l_out == 7u);
            REQUIRE(baa_out == base_alignment_algorithm_type::block_aligner);
        }
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
    // Inject an out-of-range MA value to confirm the chain never reads
    // params.minimizer_algorithm — guarded_step short-circuits to
    // ma_step::default_value (SCALAR) when active=false.
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
        // Both branches use the same fully-instantiated pipeline body —
        // we cannot directly observe which Cep was picked without a probe
        // hook inside pipeline_run_impl.  Instead, we exercise the leaf's
        // (W, K) gate behaviour and confirm the call completes without
        // an out-of-bounds access by passing zero-sized sequences and
        // empty minimizers.
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
            ::bioseq::simd::minimizer_representation<15u, 15u>;
        const auto minimizers_owner =
            std::make_shared<const std::vector<minimizer_type>>();
        const auto mins = minimizer_batch_view<minimizer_type>::from_shared_host(
            minimizers_owner, 0u, 0u);
        const auto plan = make_statement_execution_plan(std::move(params));
        ::bioseq::indexing::minimizer_hashmap_fragment_index idx;
        idx.build(*minimizers_owner, t.bp_size());
        // The leaf's W==10 && K==15 branch picks SpecCep.
        auto result = detail::pipeline_run_leaf<
            pipeline_entry_point::with_prebuilt_minimizers,
            seq::dna4,
            minimizer_type, ::bioseq::indexing::minimizer_hashmap_fragment_index,
            15u, 15u,
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
            ::bioseq::simd::minimizer_representation<15u, 15u>;
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
            15u, 15u,
            base_alignment_algorithm_type::block_aligner, chaining_algorithm_type::scalar_minimap2, minimizer_algorithm_type::scalar>::call(
                plan->frozen_parameters, q_view, t_view, mins, idx, plan,
                nullptr, ::bioseq::indexing::gpu_index_cache_identity{}, false);
        REQUIRE(result.size() == 0u);
    }
}

TEST_CASE("non-canonical kmer_size resolves to the right L bucket",
          "[component:bioseq][layer:unit][feature:planner]")
{
    using dispatch_test_support::base_alignment_algorithm_type;
    using dispatch_test_support::lkb_probe_traits;

    ::bioseq::simd::minimap2_parameters params;
    params.base_alignment_algorithm = base_alignment_algorithm_type::block_aligner;

    for (int32_t k : {int32_t{11}, int32_t{21}}) {
        params.kmer_size = k;
        SECTION("max_bp = 1<<10") {
            std::vector<seq::dna4> qd(1u << 10);
            std::vector<seq::dna4> td(1u << 10);
            compact_sequence_range<std::vector<seq::dna4>> q{qd};
            compact_sequence_range<std::vector<seq::dna4>> t{td};
            std::size_t l_out = 0u, k_out = 0u;
            base_alignment_algorithm_type baa_out = static_cast<base_alignment_algorithm_type>(0xFF);
            detail::dispatch_chain<
                int,
                lkb_probe_traits::template leaf,
                detail::seq_len_log2_step,
                detail::kmer_size_step,
                detail::baa_step
            >::template run<>(params, q, t, l_out, k_out, baa_out);
            REQUIRE(k_out == kmer_size_default);
            // round_up_seq_len_log2(1<<10, k) = max(10, k) = k for k in {11, 21}.
            // Then seq_len_log2_step picks the smallest L >= k from the
            // (union over kmer_size_buckets) breakpoint list. With the
            // extended-k-mer capability that list is
            // {7, 15, 17, 27, 33, 49, 63}: k=11 → L=15;  k=21 → L=27.
            // Baseline profiles include only K=15, so the list collapses to
            // K=15's per-K breakpoints
            // {15, 33, 63}: k=11 → L=15; k=21 → L=33.
            if constexpr (kmer_size_buckets.size() >= 3u) {
                if (k == 11) REQUIRE(l_out == 15u);
                if (k == 21) REQUIRE(l_out == 27u);
            } else {
                if (k == 11) REQUIRE(l_out == 15u);
                if (k == 21) REQUIRE(l_out == 33u);
            }
        }
        SECTION("max_bp = 1<<20") {
            std::vector<seq::dna4> qd(1u << 20);
            std::vector<seq::dna4> td(1u << 20);
            compact_sequence_range<std::vector<seq::dna4>> q{qd};
            compact_sequence_range<std::vector<seq::dna4>> t{td};
            std::size_t l_out = 0u, k_out = 0u;
            base_alignment_algorithm_type baa_out = static_cast<base_alignment_algorithm_type>(0xFF);
            detail::dispatch_chain<
                int,
                lkb_probe_traits::template leaf,
                detail::seq_len_log2_step,
                detail::kmer_size_step,
                detail::baa_step
            >::template run<>(params, q, t, l_out, k_out, baa_out);
            REQUIRE(k_out == kmer_size_default);
            // round_up_seq_len_log2(1<<20, k) = 20. The smallest L >= 20
            // in {7, 15, 17, 27, 33, 49, 63} is 27 (default level >= 2);
            // in {15, 33, 63} (level == 1) it is 33.
            if constexpr (kmer_size_buckets.size() >= 3u) {
                REQUIRE(l_out == 27u);
            } else {
                REQUIRE(l_out == 33u);
            }
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
    // By-value (deduced as non-reference type) is rejected.
    static_assert(!dispatch_test_support::chain_accepts_v<base_alignment_algorithm_type, chaining_algorithm_type, minimizer_algorithm_type>);
    // rvalue ref is rejected.
    static_assert(!dispatch_test_support::chain_accepts_v<base_alignment_algorithm_type&&, chaining_algorithm_type&&, minimizer_algorithm_type&&>);
    SUCCEED();
}

TEST_CASE("algorithm-aware k20 layout separates physical K from runtime biological k w",
          "[component:bioseq][layer:unit][feature:planner]") {
    using namespace ::bioseq::minimizers;
    using algorithm = ::bioseq::simd::minimizer_algorithm;
    for (const auto selected : {algorithm::bounded_nthash_simd, algorithm::two_stack_nthash}) {
        // Bounded keeps its u32 hash; two-stack's selected K20 profile uses
        // the existing wider record for query/index hash-mask compatibility.
        const auto expected_physical_k =
            selected == algorithm::bounded_nthash_simd ? 15u : 20u;
        const auto expected_position_log2 =
            selected == algorithm::bounded_nthash_simd ? 28u : 63u;
        const auto layout = select_minimizer_physical_layout<seq::dna4, std::uint32_t>(
            selected, std::uint64_t{1u} << 28u, 20u, 15u);
        REQUIRE(layout.physical_kmer_size == expected_physical_k);
        REQUIRE(layout.record_position_log2 == expected_position_log2);
        REQUIRE(layout.runtime_kmer_size == 20u);
        REQUIRE(layout.runtime_window_size == 15u);
        REQUIRE(layout.effective_algorithm == selected);
        const auto physical_k = visit_minimizer_physical_layout(
            layout, []<class record>() { return record::layout_type::k_physical_kmer_size; });
        REQUIRE(physical_k == expected_physical_k);
        const auto wide = select_minimizer_physical_layout<seq::dna5, std::uint64_t>(
            selected, (std::uint64_t{1u} << 28u) + 1u, 20u, 15u);
        REQUIRE(wide.record_position_log2 == 63u);
        REQUIRE(wide.physical_kmer_size == expected_physical_k);
    }
    const auto reference = select_minimizer_physical_layout<seq::dna4, std::uint64_t>(
        algorithm::simd_minimap2_reference, (std::uint64_t{1u} << 20u) + 1u, 20u, 15u);
    REQUIRE(reference.record_position_log2 == 23u);
    REQUIRE(reference.physical_kmer_size == 20u);
    REQUIRE(reference.hash_width == minimizer_scalar_width::u64);
    auto mismatched = reference;
    mismatched.hash_width = minimizer_scalar_width::u32;
    REQUIRE_THROWS_AS(visit_minimizer_physical_layout(
        mismatched, []<class record>() { return sizeof(record); }), std::invalid_argument);
    const auto vulkan = select_minimizer_physical_layout<seq::dna5, std::uint64_t>(
        algorithm::vulkan_minimap_integer, 4'000'000'000u, 20u, 15u);
    REQUIRE(vulkan.requested_algorithm == algorithm::vulkan_minimap_integer);
    REQUIRE(vulkan.effective_algorithm == algorithm::vulkan_minimap_integer);
    REQUIRE(vulkan.physical_kmer_size == 20u);
    REQUIRE(vulkan.record_position_log2 == 63u);
    REQUIRE_THROWS_AS((select_minimizer_physical_layout<seq::dna4, std::uint32_t>(
        algorithm::simd_minimap2_reference, 1024u, 20u, 15u)), std::invalid_argument);
    const auto dna5 = select_minimizer_physical_layout<seq::dna5, std::uint32_t>(
        algorithm::simd_minimap2_reference, 1024u, 20u, 15u);
    REQUIRE(dna5.requested_algorithm == algorithm::simd_minimap2_reference);
    REQUIRE(dna5.effective_algorithm == algorithm::simd_minimap2_reference);
    REQUIRE(dna5.physical_kmer_size == 20u);
    REQUIRE_THROWS_AS((select_minimizer_physical_layout<seq::dna4, std::uint64_t>(
        algorithm::bounded_nthash_simd, (std::uint64_t{1u} << 63u) + 1u, 20u, 15u)),
        std::length_error);
}

} // namespace bioseq::simd::pipe
