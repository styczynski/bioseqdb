/**
 * @file dispatch_test.cpp
 * @brief Exercises pipeline compile-time dispatch geometry.
 */
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.unit.pipeline.pipe_support;

using namespace bioseq::tests::pipeline::pipe_support;

namespace bioseq::simd::pipe {

using namespace ::bioseq::tests::pipeline::pipe_support;

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
        const std::vector<minimizer_type> mins{};
        ::bioseq::indexing::minimizer_hashmap_fragment_index idx;
        idx.build(mins, t.bp_size());
        // The leaf's W==10 && K==15 branch picks SpecCep.
        auto result = detail::pipeline_run_leaf<
            pipeline_entry_point::with_prebuilt_minimizers,
            seq::dna4,
            minimizer_type, ::bioseq::indexing::minimizer_hashmap_fragment_index,
            15u, 15u,
            base_alignment_algorithm_type::block_aligner, chaining_algorithm_type::scalar_minimap2, minimizer_algorithm_type::scalar>::call(
                params, q_view, t_view, mins, idx,
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
        const std::vector<minimizer_type> mins{};
        ::bioseq::indexing::minimizer_hashmap_fragment_index idx;
        idx.build(mins, t.bp_size());
        auto result = detail::pipeline_run_leaf<
            pipeline_entry_point::with_prebuilt_minimizers,
            seq::dna4,
            minimizer_type, ::bioseq::indexing::minimizer_hashmap_fragment_index,
            15u, 15u,
            base_alignment_algorithm_type::block_aligner, chaining_algorithm_type::scalar_minimap2, minimizer_algorithm_type::scalar>::call(
                params, q_view, t_view, mins, idx,
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

} // namespace bioseq::simd::pipe
