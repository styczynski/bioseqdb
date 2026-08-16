/**
 * @file support.cppm
 * @brief Provides shared fixtures for pipeline tests.
 */
// Tests for the templated pipeline API.
//
// Static-assert-only cases exercise the emit_token/emitted_token trait surface
// for single-emission invariants and sink concepts. The runtime
// cases drive a probe sink through a canonical pipeline to verify that every
// process*<Cep> body emits exactly once.
//
// These tests are compile-only in the static-assert sections (compile fails ⇒
// test fails); the runtime SECTIONs use Catch2 REQUIRE assertions and run as
// part of the bioseq unit-test target.

module;

#include <catch2/matchers/catch_matchers.hpp>

#include <catch2/catch_test_macros.hpp>

export module bioseq.tests.unit.pipeline.pipe_support;

import std;
export import bioseq.common.logging;
export import bioseq.common.checked_arithmetic;
export import bioseq.common.context;
export import bioseq.common.seqan_adapter;
export import bioseq.sequences;
export import bioseq.sequences.kmers;
export import bioseq.sequences.kmers.hashers.minimap_int;
export import bioseq.sequences.kmers.minimap2_sketch_baseline;
export import bioseq.sequences.kmers.vulkan_minimizers;
export import bioseq.sequences.kmers.two_stack_minimizers;
export import bioseq.highway.dispatch_contract;
export import bioseq.highway.test_support;
export import bioseq.pipeline.pipe;
export import bioseq.storage.scanner;
export import bioseq.storage.lazy_sequence;
export import bioseq.chaining.simd;
export import bioseq.chaining.core;
export import bioseq.chaining.vulkan;
export import bioseq.pipeline.minimap_port;
export import bioseq.indexing.minimizer_hashmap;
export import bioseq.indexing.minimizer_index;
export import bioseq.indexing.gpu_minimizer_index;
export import bioseq.parallelism.memory_admission;
export import bioseq.sequences.cigar;
export import bioseq.storage.size;
export import bioseq.alignment;
export import bioseq.alignment.utils;
export import bioseq.gpu.vulkan.runtime;

export namespace bioseq::tests::pipeline::pipe_support {

[[nodiscard]] const ::bioseq::simd_dispatch::highway_kernel_table*
fixed_highway_kernels() {
    return &::bioseq::simd_dispatch::highway_test_kernel_table();
}

[[nodiscard]] ::bioseq::simd_dispatch::highway_dispatch_plan
fixed_highway_dispatch_plan() {
    const auto& binding =
        ::bioseq::simd_dispatch::highway_test_target_binding();
    return {
        .compiled_targets = binding.target,
        .cpu_supported_targets = binding.target,
        .runnable_targets = binding.target,
        .planned_target = binding.target,
    };
}

template <sequence_alphabet p_alphabet = ::seq::dna4>
void bind_fixed_highway(::bioseq::simd::minimap2_parameters& params) {
    const auto& binding =
        ::bioseq::simd_dispatch::highway_test_target_binding();
    const auto* identity = binding.kernels == nullptr
        || !binding.kernels->is_complete()
        ? nullptr
        : binding.kernels->template resolve_alphabet_identity<p_alphabet>();
    if (identity == nullptr
        || binding.kernels->resolve_alphabet_identity(
               identity->alphabet_code) != identity) {
        throw std::logic_error{
            "generated fixed-target test binding lacks alphabet identity"};
    }
    params.highway_kernels = binding.kernels;
    params.highway_executed_target = binding.target;
    params.highway_dispatch = fixed_highway_dispatch_plan();
    params.alphabet_code = identity->alphabet_code;
}

struct raw_sketch_append_fixture {
    using storage_type = std::uint64_t;
    std::uint64_t value = 0;
};
static_assert(std::is_trivially_copyable_v<raw_sketch_append_fixture>);
static_assert(sizeof(raw_sketch_append_fixture)
              == sizeof(raw_sketch_append_fixture::storage_type));

struct alignas(8) source_direct_minimizer_fixture {
    struct layout_type {
        static constexpr std::uint32_t k_position_bits = 33u;
        static constexpr std::uint32_t k_hash_bits = 30u;
    };
    std::uint64_t value = 0u;
};
static_assert(sizeof(source_direct_minimizer_fixture) == 8u);
static_assert(alignof(source_direct_minimizer_fixture) == 8u);

struct alignas(8) incompatible_source_direct_minimizer_fixture {
    struct layout_type {
        static constexpr std::uint32_t k_position_bits = 32u;
        static constexpr std::uint32_t k_hash_bits = 31u;
    };
    std::uint64_t value = 0u;
};
static_assert(sizeof(incompatible_source_direct_minimizer_fixture) == 8u);
static_assert(alignof(incompatible_source_direct_minimizer_fixture) == 8u);

struct budgeted_sketch_lifetime_probe {
    using storage_type = std::uint64_t;
    struct layout_type {
        static constexpr std::uint32_t k_position_bits = 28u;
        static constexpr std::uint32_t k_hash_bits = 35u;
    };
    static constexpr std::uint32_t k_hash_shift = 29u;
    static constexpr std::uint64_t k_position_field_mask = (std::uint64_t{1} << 28u) - 1u;

    bioseq::parallelism::memory_admission_controller* admission = nullptr;
    std::uint64_t expected_reserved = 0;
    bool* destroyed_with_live_permit = nullptr;
    bool active = true;

    [[nodiscard]] std::uint64_t raw_value() const noexcept { return 0u; }
    budgeted_sketch_lifetime_probe() = default;
    budgeted_sketch_lifetime_probe(
            bioseq::parallelism::memory_admission_controller* owner,
            std::uint64_t expected,
            bool* observed) noexcept
        : admission(owner),
          expected_reserved(expected),
          destroyed_with_live_permit(observed)
    {}
    budgeted_sketch_lifetime_probe(budgeted_sketch_lifetime_probe&& other) noexcept
        : admission(other.admission),
          expected_reserved(other.expected_reserved),
          destroyed_with_live_permit(other.destroyed_with_live_permit),
          active(std::exchange(other.active, false))
    {}
    budgeted_sketch_lifetime_probe(const budgeted_sketch_lifetime_probe&) = delete;
    budgeted_sketch_lifetime_probe& operator=(const budgeted_sketch_lifetime_probe&) = delete;
    budgeted_sketch_lifetime_probe& operator=(budgeted_sketch_lifetime_probe&&) = delete;
    ~budgeted_sketch_lifetime_probe() {
        if (active) {
            *destroyed_with_live_permit =
                admission->reserved_bytes() == expected_reserved;
        }
    }
};

using bioseq::pipeline_stats::executed_backend;
using bioseq::pipeline_stats::execution_engine;
using bioseq::pipeline_stats::execution_environment_snapshot;
using bioseq::pipeline_stats::optional_observation_bool;
using bioseq::pipeline_stats::pipeline_cache_policy;
using bioseq::pipeline_stats::pipeline_execution_stats;
using bioseq::pipeline_stats::pipeline_job_execution_stats;
using bioseq::pipeline_stats::pipeline_stage;
using bioseq::pipeline_stats::simd_alignment_algorithm_identity;
using bioseq::pipeline_stats::simd_chaining_policy_identity;
using bioseq::pipeline_stats::target_index_source;

[[nodiscard]] execution_environment_snapshot make_cpu_environment(
    std::uint32_t workers = 2) {
    execution_environment_snapshot environment{};
    environment.effective_cpu_workers = workers;
    const auto& binding =
        ::bioseq::simd_dispatch::highway_test_target_binding();
    environment.record_highway_target(
        fixed_highway_dispatch_plan(), binding.target);
    return environment;
}

[[nodiscard]] execution_environment_snapshot make_gpu_environment(
    std::uint32_t workers = 2) {
    execution_environment_snapshot environment{};
    environment.effective_cpu_workers = workers;
    environment.is_gpu_environment_available = true;
    environment.gpu_device_uuid[0] = 0x42;
    environment.gpu_driver_identity[0] = 'v';
    environment.gpu_driver_identity[1] = 'k';
    environment.gpu_pipeline_cache_policy =
        pipeline_cache_policy::immutable_preseed_all_hit;
    environment.gpu_pipeline_cache_seed_sha256[0] = 0x91;
    environment.gpu_pipeline_cache_seed_size = 64;
    environment.gpu_pipeline_cache_manifest_checksum = 0x1234;
    environment.gpu_pipeline_cache_hit = optional_observation_bool::true_value;
    const auto& binding =
        ::bioseq::simd_dispatch::highway_test_target_binding();
    environment.record_highway_target(
        fixed_highway_dispatch_plan(), binding.target);
    return environment;
}

[[nodiscard]] pipeline_job_execution_stats make_attested_job(
    executed_backend backend,
    target_index_source source) {
    pipeline_job_execution_stats job{};
    job.requested_backend = backend;
    job.actual_backend = backend;
    if (backend == executed_backend::simd_cpu) {
        job.requested_simd_chaining_policy =
            simd_chaining_policy_identity::bounded_batch_approximate;
        job.observed_simd_chaining_policy =
            simd_chaining_policy_identity::bounded_batch_approximate;
        job.requested_simd_alignment_algorithm =
            simd_alignment_algorithm_identity::block_aligner;
        job.observed_simd_alignment_algorithm =
            simd_alignment_algorithm_identity::block_aligner;
    }
    job.requested_target_index_source = source;
    job.actual_target_index_source = source;
    job.environment = backend == executed_backend::vulkan_gpu
        ? make_gpu_environment()
        : make_cpu_environment();
    if (source == target_index_source::persisted) {
        job.persisted_generation_id = 17;
        job.persisted_source_identity = 0xabc;
    }
    for (std::size_t i = 0; i < bioseq::pipeline_stats::k_pipeline_stage_count; ++i) {
        const auto stage = static_cast<pipeline_stage>(i);
        const execution_engine engine = bioseq::pipeline_stats::expected_execution_engine(
            backend, source, stage);
        if (engine == execution_engine::count) continue;
        auto& coverage = job.stages.coverage[i];
        coverage.eligible_jobs = 1;
        coverage.executed_jobs[bioseq::pipeline_stats::engine_index(engine)] = 1;
    }
    return job;
}

[[nodiscard]] pipeline_job_execution_stats make_zero_match_job(
    executed_backend backend,
    target_index_source source) {
    auto job = make_attested_job(backend, source);
    for (std::size_t i = 0; i < bioseq::pipeline_stats::k_pipeline_stage_count; ++i) {
        const auto stage = static_cast<pipeline_stage>(i);
        if (!bioseq::pipeline_stats::is_front_end_eligibility_required(source, stage)) {
            job.stages.coverage[i] = {};
        }
    }
    return job;
}

} // namespace bioseq::tests::pipeline::pipe_support

using namespace bioseq::tests::pipeline::pipe_support;

export namespace bioseq::simd::pipe {

// ============================================================================
// Probe sink that counts on_batch invocations.
// ============================================================================

struct probe_batch_sink {
    int* on_batch_count;
    int* on_completed_count;
    [[gnu::always_inline]] void on_batch(std::span<alignment_region> /*regions*/) {
        ++*on_batch_count;
    }
    [[gnu::always_inline]] void on_completed() noexcept { ++*on_completed_count; }
};

struct exact_filter_probe_sink {
    std::vector<alignment_region>* observed;
    int* on_batch_count;

    void on_batch_pair(
        std::span<alignment_region> regions,
        std::span<const ::bioseq::chaining::anchor> /*anchors*/) {
        observed->assign(regions.begin(), regions.end());
        ++*on_batch_count;
    }

    void on_completed() noexcept {}
};

struct prefilter_probe_sink {
    std::vector<alignment_region>* observed;
    int* on_batch_count;

    void on_batch_pair(
        std::span<alignment_region> regions,
        std::span<const ::bioseq::chaining::anchor> /*anchors*/) {
        observed->assign(regions.begin(), regions.end());
        ++*on_batch_count;
    }

    void on_completed() noexcept {}
};

// `SingleEmissionInvariant`.
//
// Drives N=8 invocations through the bound mapq_stage with a probe sink
// and asserts the probe records exactly 8 on_batch invocations.  This
// mirrors the spec's intent of "every process*<Cep> emits exactly one
// on_batch* per upstream on_batch*" — the bound stage is the unit of
// stage-shape composition, so verifying the contract on one bound
// stage exercises the contract shared by every stage in the pipeline (every
// `process*<Cep>` body uses the same emit_token/emitted_token machinery).

// `KSW2BatchedParity` checks whether the trait surface for
// `BAA = KSW2` and `BAA = KSW2_BATCHED` agrees on the algorithm-label
// and runtime-parameter row count.  A full end-to-end parity check
// against `minimap2_port_with_index_batch` would require live
// sequences; the trait surface check verifies the structural contract.

// =============================================================================
// Chain primitive, registry, and leaf tests.
// =============================================================================

namespace dispatch_test_support {

using base_alignment_algorithm_type = ::bioseq::simd::base_alignment_algorithm;
using chaining_algorithm_type  = ::bioseq::simd::chaining_algorithm;
using minimizer_algorithm_type  = ::bioseq::simd::minimizer_algorithm;

/// Probe callable for `dispatch_exact_value_or_default` tests. Stateless POD with a
/// templated `operator()` whose body forwards the matched IC<T, V> into a
/// caller-provided reference slot.
template <class p_t>
struct value_capture_callable {
    p_t* sink;
    template <p_t p_v, class... p_args>
    [[gnu::always_inline]] p_t operator()(
        std::integral_constant<p_t, p_v>, p_args&&...) const
    {
        *sink = p_v;
        return p_v;
    }
};

}  // namespace dispatch_test_support

namespace dispatch_test_support {

/// Probe leaf for the BAA × CA × MA chain order test.  Three NTTPs are
/// pinned. The leaf's `call` writes the (BAA, CA, MA) triple into a
/// caller-provided slot reachable through the trailing args.
template <auto p_baa_pin, auto p_ca_pin, auto p_ma_pin>
struct bca_probe_leaf {
    [[gnu::always_inline]] static int call(
        const ::bioseq::simd::minimap2_parameters& /*params*/,
        base_alignment_algorithm_type& baa_out, chaining_algorithm_type& ca_out, minimizer_algorithm_type& ma_out)
    {
        baa_out = p_baa_pin;
        ca_out  = p_ca_pin;
        ma_out  = p_ma_pin;
        return 1;
    }
};

struct bca_probe_traits {
    template <auto p_baa_pin, auto p_ca_pin, auto p_ma_pin>
    using leaf = bca_probe_leaf<p_baa_pin, p_ca_pin, p_ma_pin>;
};

/// Probe leaf for the heterogeneous (size_t L, size_t K, BAA) chain test.
template <auto p_l_pin, auto p_k_pin, auto p_baa_pin>
struct lkb_probe_leaf {
    [[gnu::always_inline]] static int call(
        const ::bioseq::simd::minimap2_parameters& /*params*/,
        const compact_sequence_range<std::vector<seq::dna4>>& /*q*/,
        const compact_sequence_range<std::vector<seq::dna4>>& /*t*/,
        std::size_t& l_out, std::size_t& k_out, base_alignment_algorithm_type& baa_out)
    {
        l_out   = p_l_pin;
        k_out   = p_k_pin;
        baa_out = p_baa_pin;
        return 1;
    }
};

struct lkb_probe_traits {
    template <auto p_l_pin, auto p_k_pin, auto p_baa_pin>
    using leaf = lkb_probe_leaf<p_l_pin, p_k_pin, p_baa_pin>;
};

/// Void-returning probe leaf for the MA-only chain test.
template <auto p_ma_pin>
struct void_probe_leaf {
    [[gnu::always_inline]] static void call(
        const ::bioseq::simd::minimap2_parameters& /*params*/,
        minimizer_algorithm_type& ma_out)
    {
        ma_out = p_ma_pin;
    }
};

struct void_probe_traits {
    template <auto p_ma_pin>
    using leaf = void_probe_leaf<p_ma_pin>;
};

}  // namespace dispatch_test_support

namespace dispatch_test_support {

/// Records which Cep branch was selected inside `pipeline_run_leaf::call`.
inline thread_local int g_last_recorded_w = 0;
inline thread_local int g_last_recorded_k = 0;

}  // namespace dispatch_test_support

namespace dispatch_test_support {

template <class... p_args>
constexpr bool chain_accepts_v = requires {
    ::bioseq::simd::pipe::detail::dispatch_chain<
        int, bca_probe_traits::template leaf,
        ::bioseq::simd::pipe::detail::baa_step,
        ::bioseq::simd::pipe::detail::ca_step,
        ::bioseq::simd::pipe::detail::ma_step
    >::template run<>(
        std::declval<const ::bioseq::simd::minimap2_parameters&>(),
        std::declval<p_args>()...);
};

}  // namespace dispatch_test_support

// ============================================================================
// vulkan_minimap_integer end-to-end biological parity.
// ============================================================================
//
// The full Vulkan front-end (GPU minimizers + GPU seed-hit lookup) must produce
// the same complete ordered match_result/CIGAR stream as the CPU path. When a
// Vulkan algorithm is selected, disabling Vulkan is an error; fallback
// selection must happen before entering the pipe.
namespace vulkan_pipeline_test_support {

using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using range_t  = compact_sequence_range<dna4_u64>;

[[nodiscard]] std::vector<seq::dna4> make_dna4(std::size_t n, std::uint64_t seed) {
    std::vector<seq::dna4> v(n);
    std::uint64_t s = seed;
    for (std::size_t i = 0; i < n; ++i) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        seq::dna4 c;
        c.assign_rank(static_cast<decltype(c.to_rank())>((s >> 33) & 0x3));
        v[i] = c;
    }
    return v;
}

[[nodiscard]] match_result_collection run(
    std::vector<seq::dna4> qd, std::vector<seq::dna4> td,
    const ::bioseq::simd::minimap2_parameters& params)
{
    range_t q_range(qd);
    range_t t_range(td);
    auto lazy_q = bioseq::storage::make_resident_lazy_sequence(q_range);
    auto lazy_t = bioseq::storage::make_resident_lazy_sequence(t_range);
    const auto q_view = bioseq::storage::make_lazy_sequence_view(lazy_q);
    const auto t_view = bioseq::storage::make_lazy_sequence_view(lazy_t);
    auto fixed_params = params;
    bind_fixed_highway(fixed_params);
    return ::bioseq::simd::minimap_port(q_view, t_view, fixed_params);
}

[[nodiscard]] bool exact_biological_results_equal(
    const match_result_collection& left,
    const match_result_collection& right) noexcept
{
    if (left.results.size() != right.results.size()) return false;
    for (std::size_t index = 0u; index < left.results.size(); ++index) {
        const match_result& lhs = left.results[index];
        const match_result& rhs = right.results[index];
        if (lhs.query_len != rhs.query_len
            || lhs.query_start != rhs.query_start
            || lhs.query_end != rhs.query_end
            || lhs.is_reverse != rhs.is_reverse
            || lhs.target_len != rhs.target_len
            || lhs.target_start != rhs.target_start
            || lhs.target_end != rhs.target_end
            || lhs.num_matches != rhs.num_matches
            || lhs.block_length != rhs.block_length
            || lhs.mapq != rhs.mapq
            || lhs.chain_score != rhs.chain_score
            || lhs.dp_score != rhs.dp_score
            || lhs.dp_max_score != rhs.dp_max_score
            || lhs.seed_count != rhs.seed_count
            || lhs.secondary_chain_score != rhs.secondary_chain_score
            || std::bit_cast<std::uint32_t>(lhs.divergence)
                != std::bit_cast<std::uint32_t>(rhs.divergence)
            || lhs.is_primary != rhs.is_primary
            || lhs.has_cigar != rhs.has_cigar
            || lhs.edit_distance != rhs.edit_distance
            || lhs.cigar.size() != rhs.cigar.size()) {
            return false;
        }
        for (std::size_t cigar_index = 0u;
             cigar_index < lhs.cigar.size(); ++cigar_index) {
            if (lhs.cigar[cigar_index].length
                    != rhs.cigar[cigar_index].length
                || lhs.cigar[cigar_index].opcode
                    != rhs.cigar[cigar_index].opcode) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace vulkan_pipeline_test_support

#ifdef BIOSEQ_TEST_INDEXES
template <class p_collector>
[[nodiscard]] std::vector<std::uint64_t>
normalized_raw_minimizers(const p_collector& collector) {
    std::vector<std::uint64_t> records;
    const auto& sketches = collector.extract_sketches_data();
    records.reserve(sketches.size());
    for (const auto& sketch : sketches) {
        records.push_back(static_cast<std::uint64_t>(sketch.raw_value()));
    }
    std::ranges::sort(records);
    const auto unique_end = std::unique(records.begin(), records.end());
    records.resize(static_cast<std::size_t>(unique_end - records.begin()));
    return records;
}

template <sequence_alphabet p_alphabet, class p_storage_unit>
void check_reference_minimizer_topology_case() {
    using namespace ::bioseq::simd;
    using sequence_type = sequence_custom_alphabet<p_alphabet, p_storage_unit>;
    using range_type = compact_sequence_range<sequence_type>;
    using collector_type = basic_sketch_collector<minimap_sketch_data>;
    using pipe::minimizer_kernel_kind;

    std::vector<p_alphabet> data(4099u);
    for (std::size_t index = 0u; index < data.size(); ++index) {
        std::uint8_t rank = static_cast<std::uint8_t>(index & 3u);
        if constexpr (std::same_as<p_alphabet, seq::dna5>) {
            rank = index % 97u == 41u
                ? 3u
                : std::array<std::uint8_t, 4>{0u, 1u, 2u, 4u}[index & 3u];
        }
        data[index].assign_rank(rank);
    }
    range_type resident(data);
    auto lazy = bioseq::storage::make_resident_lazy_sequence(resident);
    const auto view = bioseq::storage::make_lazy_sequence_view(lazy);
    constexpr bool is_dna5 = std::same_as<p_alphabet, seq::dna5>;
    const auto* table = is_dna5 ? nullptr : fixed_highway_kernels();

    collector_type selected_collector;
    const auto runtime_stats = pipe::minimizer_dispatch<
        minimizer_kernel_kind::simd, minimap_sketch_data>(
            selected_collector, view, 10u, 15u, table);
    collector_type specialized_collector;
    const auto constexpr_stats = pipe::minimizer_dispatch_constexpr<
        minimizer_kernel_kind::simd, 10u, 15u, minimap_sketch_data>(
            specialized_collector, view, 10u, 15u, table);
    collector_type scalar_collector;
    (void)pipe::minimizer_dispatch<
        minimizer_kernel_kind::scalar, minimap_sketch_data>(
            scalar_collector, view, 10u, 15u, table);

    const auto selected = normalized_raw_minimizers(selected_collector);
    const auto specialized =
        normalized_raw_minimizers(specialized_collector);
    const auto scalar = normalized_raw_minimizers(scalar_collector);
    CAPTURE(sizeof(p_storage_unit), is_dna5);
    REQUIRE(runtime_stats == two_stack_execution_stats{});
    REQUIRE(constexpr_stats == two_stack_execution_stats{});
    REQUIRE_FALSE(selected.empty());
    REQUIRE(selected == scalar);
    REQUIRE(specialized == scalar);
    if constexpr (is_dna5) {
        const auto& runtime_records =
            selected_collector.extract_sketches_data();
        const auto& constexpr_records =
            specialized_collector.extract_sketches_data();
        const auto& scalar_records =
            scalar_collector.extract_sketches_data();
        REQUIRE(runtime_records.size() == scalar_records.size());
        REQUIRE(constexpr_records.size() == scalar_records.size());
        for (std::size_t index = 0u; index < scalar_records.size(); ++index) {
            CAPTURE(index);
            REQUIRE(runtime_records[index].raw_value()
                == scalar_records[index].raw_value());
            REQUIRE(constexpr_records[index].raw_value()
                == scalar_records[index].raw_value());
        }
    }
}

template <sequence_alphabet p_alphabet, class p_storage_unit>
void check_two_stack_minimizer_topology_case() {
    using namespace ::bioseq::simd;
    using sequence_type = sequence_custom_alphabet<p_alphabet, p_storage_unit>;
    using range_type = compact_sequence_range<sequence_type>;
    using collector_type = basic_sketch_collector<minimap_sketch_data>;
    using pipe::minimizer_kernel_kind;

    std::vector<p_alphabet> data(4099u);
    for (std::size_t index = 0u; index < data.size(); ++index) {
        std::uint8_t rank = static_cast<std::uint8_t>(index & 3u);
        if constexpr (std::same_as<p_alphabet, seq::dna5>) {
            rank = index % 101u == 53u
                ? 3u
                : std::array<std::uint8_t, 4>{0u, 1u, 2u, 4u}[index & 3u];
        }
        data[index].assign_rank(rank);
    }
    range_type resident(data);
    auto lazy = bioseq::storage::make_resident_lazy_sequence(resident);
    const auto view = bioseq::storage::make_lazy_sequence_view(lazy);

    collector_type selected_collector;
    const auto stats = pipe::minimizer_dispatch<
        minimizer_kernel_kind::exact_simd, minimap_sketch_data>(
            selected_collector,
            view,
            11u,
            15u,
            fixed_highway_kernels());
    REQUIRE(stats.is_internally_consistent(false));

    const auto& selected = selected_collector.extract_sketches_data();
    constexpr bool is_dna5 = std::same_as<p_alphabet, seq::dna5>;
    CAPTURE(sizeof(p_storage_unit), is_dna5);
    REQUIRE(stats.minimizers_emitted == selected.size());
    if constexpr (is_dna5) {
        const auto selected_pairs = selected_collector.extract_pairs();
        const auto baseline_pairs =
            minimap2_sketch_baseline_pairs(resident, 11u, 15u);
        REQUIRE_FALSE(selected_pairs.empty());
        REQUIRE(selected_pairs == baseline_pairs);
    } else {
        collector_type scalar_collector;
        (void)pipe::minimizer_dispatch<
            minimizer_kernel_kind::scalar, minimap_sketch_data>(
                scalar_collector,
                view,
                11u,
                15u,
                fixed_highway_kernels());
        const auto& scalar = scalar_collector.extract_sketches_data();
        REQUIRE(selected.size() == scalar.size());
        for (std::size_t index = 0u; index < selected.size(); ++index) {
            CAPTURE(index);
            REQUIRE(selected[index].raw_value() == scalar[index].raw_value());
        }
    }
}

#endif

template <sequence_alphabet p_alphabet, class p_storage_unit>
void check_fragmented_bounded_nthash_range_materialization() {
    using ::bioseq::simd::minimizer_sketch_backend;
    using ::bioseq::simd::pipe::bounded_nthash_runtime_error;
    using ::bioseq::simd::pipe::minimizer_dispatch_range;
    using ::bioseq::simd::pipe::
        minimizer_range_stats_matches_backend;
    using packed_sequence =
        sequence_custom_alphabet<p_alphabet, p_storage_unit>;
    using range_type = compact_sequence_range<packed_sequence>;
    using chunked_scanner = bioseq::storage::chunked_span_sequence_scanner<
        p_storage_unit, 2>;
    using minimizer_type =
        ::bioseq::simd::minimap_sketch_data;
    using collector_type =
        ::bioseq::simd::basic_sketch_collector<minimizer_type>;

    std::vector<p_alphabet> data(4'123u);
    std::uint64_t state = 0xF12A6E57u + sizeof(p_storage_unit);
    for (std::size_t index = 0u; index < data.size(); ++index) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        std::uint8_t rank = static_cast<std::uint8_t>((state >> 33u) & 3u);
        if constexpr (std::same_as<p_alphabet, seq::dna5>) {
            rank = index % 113u == 61u
                ? 3u
                : std::array<std::uint8_t, 4>{0u, 1u, 2u, 4u}[rank];
        }
        data[index].assign_rank(rank);
    }
    range_type range(data);
    auto resident_owner = bioseq::storage::make_resident_lazy_sequence(range);
    const auto resident_view =
        bioseq::storage::make_lazy_sequence_view(resident_owner);
    REQUIRE(range.size() > 12u);
    REQUIRE(range.padding() != 0u);
    using sequence_size_type = bioseq::storage::sequence_size_t;
    const auto check_slice = [&range, &resident_view](sequence_size_type lo_unit,
                                                      sequence_size_type hi_unit) {
        CAPTURE(lo_unit, hi_unit, sizeof(p_storage_unit));
        constexpr sequence_size_type letters_per_unit =
            range_type::traits::letters_per_unit;
        const sequence_size_type lo_bp = lo_unit * letters_per_unit;
        const sequence_size_type hi_bp = std::min(
            hi_unit * letters_per_unit,
            static_cast<sequence_size_type>(range.bp_size()));
        collector_type resident_collector;
        const auto resident_stats = minimizer_dispatch_range<minimizer_type>(
            resident_collector,
            resident_view,
            lo_bp,
            hi_bp,
            10u,
            15u,
            minimizer_sketch_backend::bounded_nthash_simd,
            exact_simd_range_route::require_continuation,
            fixed_highway_kernels());
        REQUIRE(minimizer_range_stats_matches_backend(
            minimizer_sketch_backend::bounded_nthash_simd,
            resident_stats,
            /*require_nonempty_vector_work=*/true));

        auto& admission =
            bioseq::parallelism::memory_admission_controller::instance();
        const std::uint64_t reserved_before = admission.reserved_bytes();
        collector_type fragmented_collector;
        {
            bioseq::storage::lazy_sequence<
                packed_sequence, chunked_scanner> fragmented{
                    chunked_scanner::from_range(range)};
            const auto fragmented_view =
                bioseq::storage::make_lazy_sequence_view(fragmented);
            const auto fragmented_stats =
                minimizer_dispatch_range<minimizer_type>(
                    fragmented_collector,
                    fragmented_view,
                    lo_bp,
                    hi_bp,
                    10u,
                    15u,
                    minimizer_sketch_backend::bounded_nthash_simd,
                    exact_simd_range_route::require_continuation,
                    fixed_highway_kernels());
            REQUIRE(minimizer_range_stats_matches_backend(
                minimizer_sketch_backend::bounded_nthash_simd,
                fragmented_stats,
                /*require_nonempty_vector_work=*/true));
            /* Bounded consumes canonical, genuinely typed u32 storage. A u64
             * source is admission-materialized rather than reinterpreted, so
             * the execution stats records the kernel input width—not the
             * facade's original resident width. */
            REQUIRE(fragmented_stats.bounded_nthash.source_storage_unit_bytes
                    == sizeof(std::uint32_t));
            REQUIRE(fragmented_stats.bounded_nthash
                    == resident_stats.bounded_nthash);

            collector_type rejected_collector;
            REQUIRE_THROWS_AS(
                minimizer_dispatch_range<minimizer_type>(
                    rejected_collector,
                    fragmented_view,
                    lo_bp,
                    hi_bp,
                    9u,
                    15u,
                    minimizer_sketch_backend::bounded_nthash_simd,
                    exact_simd_range_route::require_continuation,
                    fixed_highway_kernels()),
                bounded_nthash_runtime_error);
        }
        REQUIRE(admission.reserved_bytes() == reserved_before);

        const auto& resident_records =
            resident_collector.extract_sketches_data();
        const auto& fragmented_records =
            fragmented_collector.extract_sketches_data();
        REQUIRE_FALSE(resident_records.empty());
        REQUIRE(fragmented_records.size() == resident_records.size());
        for (std::size_t i = 0; i < resident_records.size(); ++i) {
            CAPTURE(i);
            REQUIRE(fragmented_records[i].kmer()
                    == resident_records[i].kmer());
            REQUIRE(fragmented_records[i].last_pos()
                    == resident_records[i].last_pos());
            REQUIRE(fragmented_records[i].strand()
                    == resident_records[i].strand());
            REQUIRE(fragmented_records[i].raw_value()
                    == resident_records[i].raw_value());
        }
    };

    // Exercise both an interior slice (zero local padding) and a tail slice
    // that must inherit the source range's final packed-unit padding.
    check_slice(3u, static_cast<sequence_size_type>(range.size() - 5u));
    check_slice(5u, static_cast<sequence_size_type>(range.size()));
}

template <class p_storage_unit>
void check_bounded_nthash_rejects_dna5_coordinate() {
    using ::bioseq::simd::minimizer_sketch_backend;
    using ::bioseq::simd::pipe::bounded_nthash_runtime_error;
    using ::bioseq::simd::pipe::minimizer_dispatch_range;
    using packed_sequence =
        sequence_custom_alphabet<seq::dna5, p_storage_unit>;
    using range_type = compact_sequence_range<packed_sequence>;
    using chunked_scanner = bioseq::storage::chunked_span_sequence_scanner<
        p_storage_unit, 2>;
    using minimizer_type = ::bioseq::simd::minimap_sketch_data;
    using collector_type =
        ::bioseq::simd::basic_sketch_collector<minimizer_type>;

    std::vector<seq::dna5> data(4'123u);
    for (std::size_t index = 0u; index < data.size(); ++index) {
        data[index].assign_rank(static_cast<std::uint8_t>(index % 5u));
    }
    range_type range(data);
    auto resident_owner = bioseq::storage::make_resident_lazy_sequence(range);
    const auto resident_view =
        bioseq::storage::make_lazy_sequence_view(resident_owner);
    collector_type resident_collector;
    REQUIRE_THROWS_AS(
        minimizer_dispatch_range<minimizer_type>(
            resident_collector,
            resident_view,
            0u,
            resident_view.bp_size(),
            10u,
            15u,
            minimizer_sketch_backend::bounded_nthash_simd,
            exact_simd_range_route::require_continuation,
            fixed_highway_kernels()),
        bounded_nthash_runtime_error);
    REQUIRE(resident_collector.extract_sketches_data().empty());

    auto& admission =
        bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t reserved_before = admission.reserved_bytes();
    collector_type fragmented_collector;
    {
        bioseq::storage::lazy_sequence<
            packed_sequence, chunked_scanner> fragmented{
                chunked_scanner::from_range(range)};
        const auto fragmented_view =
            bioseq::storage::make_lazy_sequence_view(fragmented);
        REQUIRE_THROWS_AS(
            minimizer_dispatch_range<minimizer_type>(
                fragmented_collector,
                fragmented_view,
                0u,
                fragmented_view.bp_size(),
                10u,
                15u,
                minimizer_sketch_backend::bounded_nthash_simd,
                exact_simd_range_route::require_continuation,
                fixed_highway_kernels()),
            bounded_nthash_runtime_error);
    }
    REQUIRE(fragmented_collector.extract_sketches_data().empty());
    REQUIRE(admission.reserved_bytes() == reserved_before);
}

enum class injected_scanner_leaf {
    row_read,
    palloc,
    interrupt,
};

class injected_scanner_leaf_error final : public std::runtime_error {
public:
    explicit injected_scanner_leaf_error(injected_scanner_leaf point)
        : std::runtime_error(point == injected_scanner_leaf::row_read
              ? "injected scanner row-read failure"
              : point == injected_scanner_leaf::palloc
                  ? "injected scanner palloc failure"
                  : "injected scanner interrupt failure")
    {}
};

template <class p_storage_unit>
struct injected_failure_scanner final {
    using storage_unit = p_storage_unit;
    using base_container = std::span<const storage_unit>;
    static constexpr std::size_t fragment_storage_units = 2u;

    base_container backing{};
    bioseq::storage::sequence_size_t logical_bp_size = 0;
    std::size_t padding = 0;
    injected_scanner_leaf point = injected_scanner_leaf::row_read;
    std::size_t* reads = nullptr;

    [[nodiscard]] static constexpr bool is_fragmented() noexcept {
        return true;
    }
    [[nodiscard]] bioseq::storage::sequence_size_t bp_size() const noexcept {
        return logical_bp_size;
    }
    [[nodiscard]] bioseq::storage::sequence_size_t storage_unit_count() const noexcept {
        return static_cast<bioseq::storage::sequence_size_t>(backing.size());
    }
    [[nodiscard]] bioseq::storage::sequence_size_t storage_byte_count() const noexcept {
        return static_cast<bioseq::storage::sequence_size_t>(
            backing.size() * sizeof(storage_unit));
    }
    [[nodiscard]] std::size_t fragment_count() const noexcept {
        return (backing.size() + fragment_storage_units - 1u)
            / fragment_storage_units;
    }
    [[nodiscard]] bioseq::storage::sequence_size_t
    estimated_bp_size() const noexcept {
        return logical_bp_size;
    }
    [[nodiscard]] std::size_t trailing_padding() const noexcept {
        return padding;
    }
    [[nodiscard]] base_container load_fragment(std::size_t) const {
        if (reads != nullptr) {
            ++*reads;
        }
        throw injected_scanner_leaf_error{point};
    }
    void release_fragment(std::size_t) const noexcept {}
};

static_assert(bioseq::storage::fragmented_sequence_scanner<
    injected_failure_scanner<std::uint64_t>>);

namespace resident_pipeline_test_support {

using operation_state_type = ::bioseq::simd::pipe::
    direct_resident_operation_stats;
using source_authority_type = ::bioseq::simd::pipe::
    source_direct_host_authorization;
using resident_authority_type = bioseq::indexing::
    gpu_resident_accounting_authority;

[[nodiscard]] operation_state_type full_resident_state(resident_authority_type authority) {
    operation_state_type state{};
    state.operation_identity = 17u;
    state.minimizer_count = 53u;
    state.distinct_count = 17u;
    state.logical_control_bytes = 272u;
    state.logical_position_bytes = 424u;
    state.logical_resident_bytes = 696u;
    state.control_requirement_bytes = 1'024u;
    state.position_requirement_bytes = 3'072u;
    state.control_allowance_bytes = 1'024u;
    state.position_allowance_bytes = 3'072u;
    state.conservative_allowance_bytes = 4'096u;
    state.conservative_request_bytes = 4'096u;
    state.source_capacity_bytes = 512u;
    state.source_releases = 1u;
    state.source_released_bytes = 512u;
    state.cache_preinsertions = 1u;
    state.cache_required_lookups = 1u;
    state.cache_required_hits = 1u;
    state.mapped_prefix_passes = 2u;
    state.mapped_prefix_lookups = 20u;
    state.mapped_prefix_comparisons = 80u;
    state.expected_seed_consumers = 1u;
    state.completed_seed_consumers = 1u;
    state.slab_requested_bytes = 1'024u;
    state.slab_actual_bytes = 1'024u;
    state.slab_retained_permit_bytes = 1'024u;
    state.slab_permit_releases = 1u;
    state.seed_reserved_bytes = 4'096u;
    state.seed_planned_vulkan_bytes = 2'048u;
    state.seed_actual_vulkan_bytes = 2'048u;
    state.seed_roundtrips = 1u;
    state.seed_dispatches = 1u;
    state.seed_fence_completions = 1u;
    state.cache_cleanup_releases = 1u;
    state.resident_cleanup_bytes = 4'096u;
    state.materialization = {
        .logical_slot_bytes = 272u,
        .logical_position_bytes = 424u,
        .logical_resident_bytes = 696u,
        .control_allocation_requirement_bytes = 1'024u,
        .position_allocation_requirement_bytes = 3'072u,
        .control_allocation_allowance_bytes = 1'024u,
        .position_allocation_allowance_bytes = 3'072u,
        .conservative_allowance_bytes = 4'096u,
        .conservative_request_bytes = 4'096u,
        .actual_slot_allocation_bytes = 1'024u,
        .actual_position_allocation_bytes = 3'072u,
        .actual_allocation_bytes = 4'096u,
        .source_checksum = 0x1234u,
        .mapped_checksum = 0x1234u,
        .fully_touched_bytes = 4'096u,
        .reflected_footprint_bytes =
            authority == resident_authority_type::reflected_process_footprint
                ? 4'096u : 0u,
        .retained_permit_bytes =
            authority == resident_authority_type::retained_exact_permit
                ? 4'096u : 0u,
        .permit_reconciliations = 1u,
        .complete_source_traversals = 1u,
        .complete_mapped_traversals = 1u,
        .authority = authority,
    };
    state.source_authority =
        authority == resident_authority_type::reflected_process_footprint
            ? source_authority_type::reflected_process_footprint
            : source_authority_type::retained_exact_permit;
    state.gpu_target_index_route_checked = true;
    state.gpu_target_index_route_compatible = true;
    state.source_authority_checked = true;
    state.has_valid_source_authorization = true;
    state.runtime_checked = true;
    state.runtime_available = true;
    state.hardware_checked = true;
    state.hardware_available = true;
    state.sorted_shape_checked = true;
    state.is_sorted_shape_accepted = true;
    state.identity_checked = true;
    state.is_identity_accepted = true;
    state.u32_shape_checked = true;
    state.is_u32_shape_accepted = true;
    state.storage_ranges_checked = true;
    state.is_control_range_accepted = true;
    state.is_position_range_accepted = true;
    state.mapping_plan_checked = true;
    state.is_mapping_plan_accepted = true;
    state.admission_snapshot_taken = true;
    state.is_planning_accepted = true;
    state.route_committed = true;
    state.materialization_started = true;
    state.materialization_completed = true;
    state.resident_published = true;
    state.source_release_completed = true;
    state.source_released_after_publication = true;
    state.cache_lookup_started = true;
    state.mapped_prefix_started = true;
    state.slab_admission_started = true;
    state.seed_dispatch_started = true;
    state.pipeline_started = true;
    state.pipeline_completed = true;
    state.log_planning("proceed", "focused_test_fixture");
    return state;
}

[[nodiscard]] operation_state_type canonical_empty_state() {
    operation_state_type state{};
    state.operation_identity = 18u;
    state.source_authority = source_authority_type::canonical_empty;
    state.gpu_target_index_route_checked = true;
    state.gpu_target_index_route_compatible = true;
    state.source_authority_checked = true;
    state.has_valid_source_authorization = true;
    state.runtime_checked = true;
    state.runtime_available = true;
    state.hardware_checked = true;
    state.hardware_available = true;
    state.sorted_shape_checked = true;
    state.is_sorted_shape_accepted = true;
    state.identity_checked = true;
    state.is_identity_accepted = true;
    state.u32_shape_checked = true;
    state.is_u32_shape_accepted = true;
    state.storage_ranges_checked = true;
    state.is_control_range_accepted = true;
    state.is_position_range_accepted = true;
    state.mapping_plan_checked = true;
    state.is_mapping_plan_accepted = true;
    state.admission_snapshot_taken = true;
    state.canonical_empty = true;
    state.is_planning_accepted = true;
    state.route_committed = true;
    state.source_release_completed = true;
    state.empty_bypass = true;
    state.log_planning("proceed", "focused_empty_fixture");
    return state;
}

}  // namespace resident_pipeline_test_support

}  // namespace bioseq::simd::pipe
