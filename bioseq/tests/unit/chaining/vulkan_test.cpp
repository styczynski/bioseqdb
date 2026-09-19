/**
 * @file
 * @brief Verifies Vulkan chaining, CIGAR, coordinate, and shader-layout contracts.
 */

#include <catch2/catch_test_macros.hpp>
#include <hwy/highway.h>
#include <vulkan/vulkan.h>

import std;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.common.context;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.sequences;
import bioseq.pipeline.minimap_port; // minimap2_parameters / match_result(_collection)
import bioseq.pipeline.pipe;         // pipeline entry point
import bioseq.chaining.core;         // anchor / chain / chaining_parameters
import bioseq.chaining.vulkan;       // chain_anchors_vulkan / is_vulkan_chaining_supported
import bioseq.gpu.vulkan.runtime;    // runtime / scan_spirv_layout / pipeline key
import bioseq.gpu.vulkan.shader_compiler;
import bioseq.storage.lazy_sequence; // make_resident_lazy_sequence
import bioseq.parallelism.memory_admission;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {

namespace {

namespace ch = ::bioseq::chaining;
namespace gv = ::bioseq::gpu::vulkan;

using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using sequence_range_type = compact_sequence_range<dna4_u64>;

/**
 * @brief Binds the generated fixed-width Highway implementation to test parameters.
 * @param[out] params Parameters receiving the target kernels and alphabet identity.
 */
void bind_fixed_highway(bioseq::simd::minimap2_parameters& params) {
    const auto& binding = ::bioseq::simd_dispatch::highway_test_target_binding();
    params.highway_kernels = binding.kernels;
    params.highway_executed_target = binding.target;
    const auto* identity = binding.kernels == nullptr || !binding.kernels->is_complete()
                               ? nullptr
                               : binding.kernels->resolve_alphabet_identity<seq::dna4>();
    if (identity == nullptr ||
        binding.kernels->resolve_alphabet_identity(identity->alphabet_code) != identity) {
        throw std::logic_error{"generated fixed-target test binding lacks DNA4 identity"};
    }
    params.alphabet_code = identity->alphabet_code;
}

/**
 * @brief Generates a deterministic DNA4 sequence for pipeline tests.
 * @param[in] sequence_length Number of bases to generate.
 * @param[in] seed Initial linear-congruential state.
 * @return Generated DNA4 bases.
 */
[[nodiscard]] std::vector<seq::dna4> make_dna4(std::size_t sequence_length, std::uint64_t seed) {
    std::vector<seq::dna4> bases(sequence_length);
    std::uint64_t random_state = seed;
    for (std::size_t index = 0; index < sequence_length; ++index) {
        random_state = random_state * 6364136223846793005ULL + 1442695040888963407ULL;
        seq::dna4 base;
        base.assign_rank(static_cast<decltype(base.to_rank())>((random_state >> 33) & 0x3));
        bases[index] = base;
    }
    return bases;
}

/**
 * @brief Runs the complete pipeline over owned in-memory query and target sequences.
 *
 * Values remain mutable because `compact_sequence_range` binds the underlying
 * containers by non-const reference.
 *
 * @param[in] query_sequence Query bases owned by the invocation.
 * @param[in] target_sequence Target bases owned by the invocation.
 * @param[in] params Minimap pipeline parameters.
 * @return Pipeline matches.
 */
[[nodiscard]] match_result_collection
run_pipeline(std::vector<seq::dna4> query_sequence,
             std::vector<seq::dna4> target_sequence,
             const bioseq::simd::minimap2_parameters& params) {
    sequence_range_type query_range(query_sequence);
    sequence_range_type target_range(target_sequence);
    auto lazy_query = bioseq::storage::make_resident_lazy_sequence(query_range);
    auto lazy_target = bioseq::storage::make_resident_lazy_sequence(target_range);
    const auto query_view = bioseq::storage::make_lazy_sequence_view(lazy_query);
    const auto target_view = bioseq::storage::make_lazy_sequence_view(lazy_target);
    return minimap_port(query_view, target_view, params);
}

/**
 * @brief Builds one chaining anchor with explicit wire-visible fields.
 * @param[in] reference_position Target coordinate.
 * @param[in] query_position Query coordinate.
 * @param[in] is_reverse Strand discriminator.
 * @param[in] query_span Seed span in bases.
 * @return Initialized anchor.
 */
[[nodiscard]] ch::anchor make_anchor(std::uint64_t reference_position,
                                     std::uint64_t query_position,
                                     std::uint32_t is_reverse,
                                     std::uint8_t query_span) {
    ch::anchor anchor{};
    anchor.ref_position = reference_position;
    anchor.query_position = query_position;
    anchor.is_reverse = is_reverse;
    anchor.query_span = query_span;
    anchor.flags = 0;
    anchor.segment_id = 0;
    return anchor;
}

/** @brief Owns chain outputs copied before the local arena context unwinds. */
struct chain_outputs {
    std::vector<ch::chain> chains;
    std::vector<ch::anchor> reordered;
};

/**
 * @brief Runs Vulkan chaining in a fresh arena and copies results to owned vectors.
 * @param[in] anchors Input anchors.
 * @param[in] params Chaining parameters.
 * @return Owned chains and reordered anchors.
 */
[[nodiscard]] chain_outputs run_vulkan_chain(std::span<const ch::anchor> anchors,
                                             const ch::chaining_parameters& params) {
    context::context runtime_context;
    context::installed_context_scope installed_context{runtime_context};
    auto chains_slab = ch::make_arena_slab<ch::chain>();
    auto reordered_slab = ch::make_arena_slab<ch::anchor>();
    // Reserve a guaranteed upper bound before the call, matching the pipeline
    // stage contract. The chaining kernel pushes its own internal
    // arena::chain scope and `backtrack_chains` writes the output via
    // push_back; without a prior reserve those allocations land ABOVE the
    // kernel's scope marker and are freed (0xDD-poisoned) when the kernel
    // returns. #chains and #reordered-anchors are each <= #input anchors.
    const std::size_t upper_bound = anchors.size() + 1;
    chains_slab.reserve(upper_bound);
    reordered_slab.reserve(upper_bound);
    ch::chain_anchors_vulkan(anchors, params, chains_slab, reordered_slab);
    chain_outputs outputs;
    outputs.chains.assign(chains_slab.data(), chains_slab.data() + chains_slab.size());
    outputs.reordered.assign(reordered_slab.data(), reordered_slab.data() + reordered_slab.size());
    return outputs;
}

/** @brief Returns prepared default chaining parameters for focused tests. */
[[nodiscard]] ch::chaining_parameters default_chain_params() {
    ch::chaining_parameters params; // kmer_size=15, max_gap=5000, bandwidth=500, ...
    params.prepare();
    return params;
}

/**
 * @brief Skips a hardware-only chaining case when no trusted device is available.
 * @return `true` when the caller must return from the current test case.
 */
[[nodiscard]] bool skip_untrusted_vulkan_chaining() {
    if (ch::is_vulkan_chaining_supported())
        return false;
    SUCCEED("Vulkan chaining correctness cases require a trusted hardware Vulkan device");
    return true;
}

/**
 * @brief Asserts the structural invariants required of a chain set.
 * @param[in] outputs Owned chain output.
 * @param[in] anchors Original input anchors.
 * @param[in] params Chaining constraints.
 */
void check_chain_invariants(const chain_outputs& outputs,
                            std::span<const ch::anchor> anchors,
                            const ch::chaining_parameters& params) {
    REQUIRE(outputs.chains.size() <= anchors.size());
    for (const auto& chain : outputs.chains) {
        REQUIRE(chain.anchor_count >= params.min_seed_count);
        REQUIRE(chain.score >= params.min_chain_score);
        REQUIRE(chain.anchor_start >= 0);
        const std::size_t start = static_cast<std::size_t>(chain.anchor_start);
        const std::size_t count = static_cast<std::size_t>(chain.anchor_count);
        REQUIRE(start + count <= outputs.reordered.size());

        const std::uint64_t max_dist_x =
            static_cast<std::uint64_t>(std::max(params.max_gap, params.bandwidth));
        for (std::size_t index = start + 1; index < start + count; ++index) {
            const ch::anchor& previous = outputs.reordered[index - 1];
            const ch::anchor& current = outputs.reordered[index];
            // Monotonic in both ref and query position.
            REQUIRE(current.ref_position > previous.ref_position);
            REQUIRE(current.query_position > previous.query_position);
            // No chain crosses a hard range boundary.
            REQUIRE(current.strand_partition_key() == previous.strand_partition_key());
            REQUIRE(current.ref_position - previous.ref_position <= max_dist_x);
        }
    }
}

} // namespace

// ===========================================================================
// Mandatory Vulkan errors
// ===========================================================================

TEST_CASE("vulkan_tiled_dp with vulkan_enabled=false fails",
          "[component:bioseq][layer:unit][feature:chaining]") {
    bioseq::simd::minimap2_parameters vulkan_params;
    vulkan_params.chaining_algorithm =
        bioseq::simd::chaining_algorithm::vulkan_tiled_dynamic_programming;
    vulkan_params.vulkan_enabled = false;
    bind_fixed_highway(vulkan_params);

    SECTION("direct chaining backend rejects disabled Vulkan") {
        ch::chaining_parameters cp;
        cp.vulkan_enabled = false;
        cp.prepare();
        std::vector<ch::anchor> anchors;
        for (std::uint64_t i = 0; i < 8; ++i) {
            anchors.push_back(make_anchor(1000 + i * 12, 500 + i * 12, 0, 15));
        }
        REQUIRE_THROWS_AS(run_vulkan_chain(anchors, cp), gv::vulkan_runtime_error);
    }

    SECTION("public pipeline rejects selected Vulkan backend") {
        auto query_sequence = make_dna4(250, 0x1357);
        auto target_sequence = make_dna4(1000, 0x2468);
        for (std::size_t index = 0; index < query_sequence.size(); ++index) {
            target_sequence[300 + index] = query_sequence[index];
        }
        // The immutable statement plan rejects a disabled selected backend
        // before allocation or entry into the Vulkan runtime.
        REQUIRE_THROWS_AS(run_pipeline(query_sequence, target_sequence, vulkan_params),
                          std::invalid_argument);
    }
}

TEST_CASE("software Vulkan chaining fails loudly",
          "[component:bioseq][layer:unit][feature:chaining]") {
    auto& runtime = gv::runtime::instance();
    INFO("Vulkan runtime error: " << runtime.initialization_error());
    if (!runtime.is_usable() || !runtime.features().software_device) {
        SUCCEED(
            "No software Vulkan device detected; hardware GPU cases cover the selected backend");
        return;
    }

    auto params = default_chain_params();
    std::vector<ch::anchor> anchors;
    for (std::uint64_t i = 0; i < 8; ++i) {
        anchors.push_back(make_anchor(1000 + i * 12, 500 + i * 12, 0, 15));
    }

    REQUIRE_THROWS_AS(run_vulkan_chain(anchors, params), gv::vulkan_runtime_error);
}

// ===========================================================================
// GPU chaining invariants (chain_anchors_vulkan directly)
// ===========================================================================

TEST_CASE("admitted Vulkan chaining requires its hooked ordinary context",
          "[component:bioseq][layer:unit][feature:chaining]") {
    auto params = default_chain_params();
    SECTION("an ordinary context cannot claim attempt admission") {
        bioseq::parallelism::operation_memory_retention retention;
        context::context ordinary;
        context::installed_context_scope installed{ordinary};
        auto chains = ch::make_arena_slab<ch::chain>();
        auto reordered = ch::make_arena_slab<ch::anchor>();
        REQUIRE_FALSE(ordinary.has_backing_allocation_hook());
        REQUIRE_THROWS_AS(
            ch::chain_anchors_vulkan({}, params, chains, reordered, &retention),
            gv::vulkan_runtime_error);
    }
    SECTION("the experimental coordinate tail is not an admitted campaign route") {
        bioseq::parallelism::memory_admission_controller admission;
        bioseq::simd::pipe::route_attempt_memory attempt{admission, {}, nullptr};
        context::installed_context_scope installed{attempt.fresh_context};
        auto chains = ch::make_arena_slab<ch::chain>();
        auto reordered = ch::make_arena_slab<ch::anchor>();
        params.coordinate_only = true;
        REQUIRE(attempt.fresh_context.has_backing_allocation_hook());
        REQUIRE_THROWS_AS(
            ch::chain_anchors_vulkan({}, params, chains, reordered, &attempt.retention),
            gv::vulkan_runtime_error);
    }
}

TEST_CASE("admitted Vulkan chaining keeps only context storage after device completion",
          "[component:bioseq][layer:unit][feature:chaining][resource:gpu]") {
    if (skip_untrusted_vulkan_chaining()) return;
    auto& runtime = gv::runtime::instance();
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const auto baseline = admission.reserved_bytes();
    const auto cache_baseline = runtime.pipeline_cache_metadata_reserved_bytes();
    const auto dispatches_before = runtime.chaining_dispatch_count();
    const auto params = default_chain_params();
    std::array<ch::anchor, 12> anchors{};
    for (std::size_t i = 0u; i < anchors.size(); ++i) {
        anchors[i] = make_anchor(1000u + i * 12u, 500u + i * 12u, 0u, 15u);
    }
    {
        bioseq::simd::pipe::route_attempt_memory attempt{admission, {}, nullptr};
        context::installed_context_scope installed{attempt.fresh_context};
        const auto initial_retained = attempt.retention.retained_bytes();
        {
            auto chain_scope = context::current().push<context::arena::chain>();
            auto chains = ch::make_arena_slab<ch::chain>();
            auto reordered = ch::make_arena_slab<ch::anchor>();
            chains.reserve(anchors.size() + 1u);
            reordered.reserve(anchors.size());
            REQUIRE(ch::chain_anchors_vulkan(
                anchors, params, chains, reordered, &attempt.retention));
            REQUIRE(chains.size() != 0u);
            REQUIRE(reordered.size() != 0u);
            REQUIRE(runtime.chaining_dispatch_count() > dispatches_before);
            REQUIRE(admission.reserved_bytes() == baseline + attempt.retention.retained_bytes() +
                runtime.pipeline_cache_metadata_reserved_bytes() - cache_baseline);
        }
        // Rewinding/destroying slabs never releases a still-retained context block.
        REQUIRE(attempt.retention.retained_bytes() >= initial_retained);
        REQUIRE(admission.reserved_bytes() == baseline + attempt.retention.retained_bytes() +
            runtime.pipeline_cache_metadata_reserved_bytes() - cache_baseline);
    }
    REQUIRE(admission.reserved_bytes() == baseline +
        runtime.pipeline_cache_metadata_reserved_bytes() - cache_baseline);
}

TEST_CASE("admitted Vulkan chaining buffer denies one byte short before materialization",
          "[component:bioseq][layer:unit][feature:chaining][resource:gpu]") {
    if (skip_untrusted_vulkan_chaining()) return;
    auto& runtime = gv::runtime::instance();
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const auto baseline = admission.reserved_bytes();
    const auto previous_cap = admission.index_memory_cap_bytes();
    struct restore_cap {
        bioseq::parallelism::memory_admission_controller& admission;
        std::uint64_t value;
        ~restore_cap() { admission.set_index_memory_cap_bytes(value); }
    } restore{admission, previous_cap};
    constexpr auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    constexpr std::uint64_t logical_bytes = 12u * sizeof(ch::anchor);
    const auto requirement = runtime.buffer_allocation_requirement(logical_bytes, usage);
    REQUIRE(requirement >= logical_bytes);
    const auto memory_plan = ch::make_vulkan_chaining_memory_plan(12u, 1u);
    bioseq::parallelism::memory_permit permit;
    admission.set_index_memory_cap_bytes(baseline + requirement - 1u);
    REQUIRE_THROWS_AS(ch::vulkan_chaining_detail::make_chaining_device_buffer(
        runtime, logical_bytes, usage, requirement, memory_plan, &permit),
        ch::vulkan_chaining_admission_error);
    REQUIRE_FALSE(permit);
    REQUIRE(admission.reserved_bytes() == baseline);

    admission.set_index_memory_cap_bytes(baseline + requirement);
    {
        auto allocation = ch::vulkan_chaining_detail::make_chaining_device_buffer(
            runtime, logical_bytes, usage, requirement, memory_plan, &permit);
        REQUIRE(allocation.allocation_bytes == requirement);
        REQUIRE((permit.bytes() == 0u || permit.bytes() == requirement));
        REQUIRE(admission.reserved_bytes() == baseline + permit.bytes());
        const auto* bytes = static_cast<const std::byte*>(allocation.mapped);
        REQUIRE(std::all_of(bytes, bytes + requirement,
                            [](std::byte value) { return value == std::byte{}; }));
    }
    permit.release();
    REQUIRE(admission.reserved_bytes() == baseline);
}

TEST_CASE("Vulkan chaining output invariants",
          "[component:bioseq][layer:unit][feature:chaining][resource:gpu]") {
    if (skip_untrusted_vulkan_chaining())
        return;
    const auto params = default_chain_params();

    SECTION("forward diagonal, 12 anchors, span==15") {
        std::vector<ch::anchor> anchors;
        for (std::uint64_t i = 0; i < 12; ++i) {
            anchors.push_back(make_anchor(1000 + i * 12, 500 + i * 12, 0, 15));
        }
        const auto out = run_vulkan_chain(anchors, params);
        check_chain_invariants(out, anchors, params);
        REQUIRE_FALSE(out.chains.empty());
        // Every emitted anchor came from the input span.
        for (const auto& emitted : out.reordered) {
            bool found = false;
            for (const auto& src : anchors) {
                if (src.ref_position == emitted.ref_position &&
                    src.query_position == emitted.query_position &&
                    src.is_reverse == emitted.is_reverse) {
                    found = true;
                    break;
                }
            }
            REQUIRE(found);
        }
    }

    SECTION("span != 15 still produces a valid chain") {
        std::vector<ch::anchor> anchors;
        for (std::uint64_t i = 0; i < 10; ++i) {
            anchors.push_back(make_anchor(2000 + i * 11, 1000 + i * 11, 0, 11));
        }
        const auto out = run_vulkan_chain(anchors, params);
        check_chain_invariants(out, anchors, params);
        REQUIRE_FALSE(out.chains.empty());
    }

    SECTION("mixed forward/reverse anchors (forward sort before reverse)") {
        std::vector<ch::anchor> anchors;
        for (std::uint64_t i = 0; i < 8; ++i) {
            anchors.push_back(make_anchor(1000 + i * 12, 400 + i * 12, 0, 15));
        }
        for (std::uint64_t i = 0; i < 8; ++i) {
            anchors.push_back(make_anchor(5000 + i * 12, 800 + i * 12, 1, 15));
        }
        const auto out = run_vulkan_chain(anchors, params);
        check_chain_invariants(out, anchors, params);
        // Reverse and forward anchors never share a chain.
        for (const auto& chain : out.chains) {
            const std::size_t start = static_cast<std::size_t>(chain.anchor_start);
            const std::size_t count = static_cast<std::size_t>(chain.anchor_count);
            const std::uint32_t strand = out.reordered[start].is_reverse;
            for (std::size_t i = start; i < start + count; ++i) {
                REQUIRE(out.reordered[i].is_reverse == strand);
            }
        }
    }

    SECTION("equal-score predecessor candidates still form a chain (>= tie policy)") {
        // Two equally-spaced predecessors yield equal pair scores into the same
        // successor; the `>=` tie policy must still produce a chain.
        std::vector<ch::anchor> anchors;
        for (std::uint64_t i = 0; i < 10; ++i) {
            anchors.push_back(make_anchor(3000 + i * 15, 2000 + i * 15, 0, 15));
        }
        const auto out = run_vulkan_chain(anchors, params);
        check_chain_invariants(out, anchors, params);
        REQUIRE_FALSE(out.chains.empty());
    }

    SECTION("hard range split: a large ref gap splits chains") {
        std::vector<ch::anchor> anchors;
        // First cluster.
        for (std::uint64_t i = 0; i < 6; ++i) {
            anchors.push_back(make_anchor(1000 + i * 12, 500 + i * 12, 0, 15));
        }
        // Second cluster, separated by >> max_gap so it is a distinct hard range.
        const std::uint64_t gap = static_cast<std::uint64_t>(params.max_gap) * 4;
        for (std::uint64_t i = 0; i < 6; ++i) {
            anchors.push_back(make_anchor(1000 + gap + i * 12, 9000 + i * 12, 0, 15));
        }
        const auto out = run_vulkan_chain(anchors, params);
        check_chain_invariants(out, anchors, params); // boundary invariant enforced here
    }
}

// ===========================================================================
// End-to-end GPU smoke (requires a usable device)
// ===========================================================================

TEST_CASE("end-to-end GPU chaining smoke",
          "[component:bioseq][layer:unit][feature:chaining][resource:gpu]") {
    INFO("Vulkan runtime error: " << gv::runtime::instance().initialization_error());
    if (skip_untrusted_vulkan_chaining())
        return;

    bioseq::simd::minimap2_parameters params;
    params.chaining_algorithm = bioseq::simd::chaining_algorithm::vulkan_tiled_dynamic_programming;
    params.vulkan_enabled = true;
    params.minimizer_algorithm = bioseq::simd::minimizer_algorithm::two_stack_nthash;
    bind_fixed_highway(params);

    auto query_sequence = make_dna4(300, 0x6A5A);
    auto target_sequence = make_dna4(1500, 0x9C9C);
    for (std::size_t index = 0; index < query_sequence.size(); ++index) {
        target_sequence[600 + index] = query_sequence[index];
    }

    const std::uint64_t before = gv::runtime::instance().chaining_dispatch_count();
    const auto matches = run_pipeline(query_sequence, target_sequence, params);
    const std::uint64_t after = gv::runtime::instance().chaining_dispatch_count();

    INFO("dispatch_count before=" << before << " after=" << after
                                  << " matches=" << matches.results.size());
    REQUIRE(after > before);                // the GPU kernel actually ran
    REQUIRE_FALSE(matches.results.empty()); // at least one match emitted
    for (const auto& match : matches.results) {
        REQUIRE(match.query_start >= 0);
        REQUIRE(match.query_end <= static_cast<int32_t>(query_sequence.size()));
        REQUIRE(match.target_start >= 0);
        REQUIRE(match.target_end <= static_cast<int32_t>(target_sequence.size()));
    }
}

TEST_CASE("Vulkan simple extension emits CIGAR",
          "[component:bioseq][layer:unit][feature:chaining][resource:gpu]") {
    auto& runtime = gv::runtime::instance();
    INFO("Vulkan runtime error: " << runtime.initialization_error());
    if (skip_untrusted_vulkan_chaining())
        return;

    constexpr std::uint32_t k_length = 100u;
    std::vector<std::uint32_t> query_symbols(k_length);
    std::vector<std::uint32_t> target_symbols(k_length);
    for (std::uint32_t i = 0; i < k_length; ++i) {
        const std::uint32_t symbol = (i * 7u + 3u) & 3u;
        query_symbols[i] = symbol;
        target_symbols[i] = symbol;
    }

    const std::vector<gv::gpu_alignment_cigar_job_wire> jobs{
        gv::gpu_alignment_cigar_job_wire{
            .query_symbol_offset = 0,
            .target_symbol_offset = 0,
            .query_len = k_length,
            .target_len = k_length,
            .query_global_start = 11,
            .target_global_start = 2007,
            .band_width = k_length,
            .flags = 0,
            .cigar_offset = 0,
            .cigar_capacity = 1,
            .traceback_tile_offset = 0,
            .traceback_tile_count = (k_length + 1u) * (k_length + 1u),
        },
    };
    const gv::gpu_alignment_cigar_params params{
        .alphabet_size = 4,
        .max_band_width = k_length,
        .tile_query = 128,
        .tile_target = 128,
        .match_score = 2,
        .mismatch_penalty = 4,
        .gap_open_short = 4,
        .gap_extend_short = 2,
        .gap_open_long = 24,
        .gap_extend_long = 1,
        .z_drop = 400,
        .end_bonus = 0,
        .flags = 0,
        .reserved = 0,
    };

    const std::uint64_t before = runtime.alignment_extension_dispatch_count();
    const gv::alignment_cigar_batch_result result =
        runtime.run_alignment_simple_cigar_roundtrip(query_symbols,
                                                     target_symbols,
                                                     jobs,
                                                     params,
                                                     /*packed_cigar_capacity=*/1u);
    const std::uint64_t after = runtime.alignment_extension_dispatch_count();

    REQUIRE(after > before);
    REQUIRE(result.results.size() == 1);
    REQUIRE(result.packed_cigar.size() == 1);
    const gv::gpu_alignment_cigar_result_wire& r = result.results.front();
    CHECK(r.status == gv::k_gpu_alignment_extension_status_ok);
    CHECK(r.score == static_cast<std::int32_t>(k_length * params.match_score));
    CHECK(r.max_score == r.score);
    CHECK(r.query_start == 11u);
    CHECK(r.query_end == 11u + k_length);
    CHECK(r.target_start == 2007u);
    CHECK(r.target_end == 2007u + k_length);
    CHECK(r.cigar_offset == 0u);
    CHECK(r.cigar_length == 1u);
    CHECK(result.packed_cigar[0] == ((k_length << 4) | 0u));
}

TEST_CASE("Vulkan 20kb exact CIGAR has no legacy span ceiling",
          "[component:bioseq][layer:unit][feature:chaining][resource:gpu]") {
    auto& runtime = gv::runtime::instance();
    INFO("Vulkan runtime error: " << runtime.initialization_error());
    if (skip_untrusted_vulkan_chaining())
        return;

    constexpr std::uint32_t k_length = 20'000u;
    std::vector<std::uint32_t> query_symbols(k_length);
    std::vector<std::uint32_t> target_symbols(k_length);
    for (std::uint32_t i = 0; i < k_length; ++i) {
        const std::uint32_t symbol = (i * 5u + 1u) & 3u;
        query_symbols[i] = symbol;
        target_symbols[i] = symbol;
    }

    const std::vector<gv::gpu_alignment_cigar_job_wire> jobs{
        gv::gpu_alignment_cigar_job_wire{
            .query_symbol_offset = 0,
            .target_symbol_offset = 0,
            .query_len = k_length,
            .target_len = k_length,
            .query_global_start = 17,
            .target_global_start = 3001,
            .band_width = gv::k_gpu_alignment_extension_max_band_width,
            .flags = 0,
            .cigar_offset = 0,
            .cigar_capacity = 1,
            .traceback_tile_offset = 0,
            .traceback_tile_count = 0,
        },
    };
    const gv::gpu_alignment_cigar_params params{
        .alphabet_size = 4,
        .max_band_width = gv::k_gpu_alignment_extension_max_band_width,
        .tile_query = 128,
        .tile_target = 128,
        .match_score = 2,
        .mismatch_penalty = 4,
        .gap_open_short = 4,
        .gap_extend_short = 2,
        .gap_open_long = 24,
        .gap_extend_long = 1,
        .z_drop = 400,
        .end_bonus = 0,
        .flags = 0,
        .reserved = 0,
    };

    const gv::alignment_cigar_batch_result result =
        runtime.run_alignment_simple_cigar_roundtrip(query_symbols,
                                                     target_symbols,
                                                     jobs,
                                                     params,
                                                     /*packed_cigar_capacity=*/1u);

    REQUIRE(result.results.size() == 1);
    REQUIRE(result.packed_cigar.size() == 1);
    const gv::gpu_alignment_cigar_result_wire& r = result.results.front();
    CHECK(r.status == gv::k_gpu_alignment_extension_status_ok);
    CHECK(r.score == static_cast<std::int32_t>(k_length * params.match_score));
    CHECK(r.query_start == 17u);
    CHECK(r.query_end == 17u + k_length);
    CHECK(r.target_start == 3001u);
    CHECK(r.target_end == 3001u + k_length);
    CHECK(r.cigar_length == 1u);
    CHECK(result.packed_cigar[0] == ((k_length << 4) | 0u));
}

TEST_CASE("Vulkan 20kb banded CIGAR traceback remains q1024 representable",
          "[component:bioseq][layer:unit][feature:chaining]") {
    constexpr std::uint64_t words = gv::gpu_alignment_cigar_gridded_traceback_words(
        20'000u,
        20'000u,
        gv::k_gpu_alignment_cigar_traceback_tile_len,
        gv::k_gpu_alignment_cigar_traceback_tile_len,
        gv::k_gpu_alignment_extension_max_band_width);
    STATIC_REQUIRE(words == 2'041'941u);
    STATIC_REQUIRE(words <= (std::numeric_limits<std::uint32_t>::max)());
    STATIC_REQUIRE(words * 1'024u <= (std::numeric_limits<std::uint32_t>::max)());
    STATIC_REQUIRE(words * 2'103u <= (std::numeric_limits<std::uint32_t>::max)());
    STATIC_REQUIRE(words * 2'104u > (std::numeric_limits<std::uint32_t>::max)());
}

#ifdef BIOSEQ_TEST_INDEXES
TEST_CASE("Vulkan packed reverse 20kb mutations preserve compact CIGAR across forced waves",
          "[component:bioseq][layer:unit][feature:chaining][resource:gpu]") {
    auto& runtime = gv::runtime::instance();
    INFO("Vulkan runtime error: " << runtime.initialization_error());
    if (skip_untrusted_vulkan_chaining())
        return;

    constexpr std::uint32_t k_length = 20'000u;
    constexpr std::uint32_t k_job_count = 3u;
    constexpr std::uint32_t k_band_width = 8u;
    constexpr std::uint32_t k_cigar_capacity = 8u;
    constexpr std::uint64_t k_test_wave_bytes = std::uint64_t{32} << 20u;
    std::vector<std::uint32_t> logical_target(k_length);
    std::uint32_t state = 0x13579bdfu;
    for (std::uint32_t index = 0u; index < k_length; ++index) {
        state = state * 1664525u + 1013904223u;
        logical_target[index] = (state >> 30u) & 3u;
    }
    constexpr std::array mutation_positions{1'024u, 2'048u};
    for (const std::uint32_t position : mutation_positions) {
        logical_target[position] = 0u;
        std::fill_n(logical_target.begin() + position + 1u, 256u, 2u);
    }
    std::vector<std::uint32_t> query = logical_target;
    for (const std::uint32_t position : mutation_positions) {
        query[position] = 1u;
    }
    std::vector<std::uint32_t> stored_reverse_target(k_length);
    for (std::uint32_t index = 0u; index < k_length; ++index) {
        stored_reverse_target[index] = 3u - logical_target[k_length - 1u - index];
    }
    const auto pack_dna4 = [](const std::span<const std::uint32_t> ranks) {
        std::vector<std::uint64_t> packed((ranks.size() + 31u) / 32u, 0u);
        for (std::size_t index = 0u; index < ranks.size(); ++index) {
            packed[index / 32u] |= static_cast<std::uint64_t>(ranks[index]) << (2u * (index % 32u));
        }
        return packed;
    };
    const std::vector<std::uint64_t> query_packed = pack_dna4(std::span{query});
    const std::vector<std::uint64_t> target_packed = pack_dna4(std::span{stored_reverse_target});
    const std::uint32_t packed_byte_count =
        static_cast<std::uint32_t>(query_packed.size() * sizeof(std::uint64_t));
    const std::uint64_t traceback_words =
        gv::gpu_alignment_cigar_gridded_traceback_words(k_length, k_length, 1u, 1u, k_band_width);
    REQUIRE(traceback_words == 7'200'004u);
    REQUIRE(traceback_words * sizeof(std::uint32_t) < k_test_wave_bytes);
    REQUIRE(traceback_words * 2u * sizeof(std::uint32_t) > k_test_wave_bytes);

    std::vector<gv::gpu_alignment_cigar_job_wire> jobs;
    jobs.reserve(k_job_count);
    for (std::uint32_t index = 0u; index < k_job_count; ++index) {
        jobs.push_back(gv::gpu_alignment_cigar_job_wire{
            .query_symbol_offset = 0u,
            .target_symbol_offset = 0u,
            .query_len = k_length,
            .target_len = k_length,
            .query_global_start = 100u + index * k_length,
            .target_global_start = 200u + index * k_length,
            .band_width = k_band_width,
            .flags = gv::k_gpu_alignment_cigar_job_flag_packed_inputs |
                     gv::k_gpu_alignment_cigar_job_flag_reverse_target,
            .cigar_offset = index * k_cigar_capacity,
            .cigar_capacity = k_cigar_capacity,
            .traceback_tile_offset = 0u,
            .traceback_tile_count = static_cast<std::uint32_t>(traceback_words),
            .query_packed_byte_count = packed_byte_count,
            .target_packed_byte_count = packed_byte_count,
            .query_leading_symbol_offset = 0u,
            .target_leading_symbol_offset = 0u,
            .query_storage_unit_width = static_cast<std::uint32_t>(sizeof(std::uint64_t)),
            .target_storage_unit_width = static_cast<std::uint32_t>(sizeof(std::uint64_t)),
            .bits_per_symbol = 2u,
            .problem_index = index,
        });
    }
    const gv::gpu_alignment_cigar_params params{
        .alphabet_size = 4u,
        .max_band_width = k_band_width,
        .tile_query = 1u,
        .tile_target = 1u,
        .match_score = 2,
        .mismatch_penalty = 4,
        .gap_open_short = 4,
        .gap_extend_short = 2,
        .gap_open_long = 24,
        .gap_extend_long = 1,
        .z_drop = 400,
        .end_bonus = 0,
        .flags = 0u,
        .reserved = 0u,
        .complement_rank_words = {0x00010203u},
    };

    const std::uint64_t before = runtime.alignment_extension_dispatch_count();
    const gv::alignment_cigar_batch_result result =
        runtime.run_alignment_packed_cigar_roundtrip_for_test(
            std::as_bytes(std::span{query_packed}),
            std::as_bytes(std::span{target_packed}),
            jobs,
            params,
            k_job_count * k_cigar_capacity,
            k_test_wave_bytes);
    const std::uint64_t after = runtime.alignment_extension_dispatch_count();

    REQUIRE(result.committed_submission_count == 4u);
    REQUIRE(after == before + result.committed_submission_count);
    REQUIRE(result.results.size() == k_job_count);
    REQUIRE(result.packed_cigar.size() == k_job_count);
    const std::int32_t expected_score =
        static_cast<std::int32_t>((k_length - mutation_positions.size()) * params.match_score -
                                  mutation_positions.size() * params.mismatch_penalty);
    for (std::uint32_t index = 0u; index < k_job_count; ++index) {
        const auto& alignment = result.results[index];
        CHECK(alignment.status == gv::k_gpu_alignment_extension_status_ok);
        CHECK(alignment.score == expected_score);
        CHECK(alignment.max_score == expected_score);
        CHECK(alignment.cigar_offset == index);
        CHECK(alignment.cigar_length == 1u);
        CHECK(alignment.num_matches == k_length - mutation_positions.size());
        CHECK(alignment.block_length == k_length);
        CHECK(result.packed_cigar[index] == ((k_length << 4u) | 0u));
    }
}
#endif

TEST_CASE("Vulkan oversized exact CIGAR batch uses trace-free GPU screening",
          "[component:bioseq][layer:unit][feature:chaining][resource:gpu]") {
    auto& runtime = gv::runtime::instance();
    INFO("Vulkan runtime error: " << runtime.initialization_error());
    if (skip_untrusted_vulkan_chaining())
        return;

    constexpr std::uint32_t k_length = 20'000u;
    constexpr std::uint32_t k_job_count = 5u;
    std::vector<std::uint32_t> query_symbols(k_length);
    std::vector<std::uint32_t> target_symbols(k_length);
    for (std::uint32_t i = 0u; i < k_length; ++i) {
        const std::uint32_t symbol = (i * 5u + 1u) & 3u;
        query_symbols[i] = symbol;
        target_symbols[i] = symbol;
    }

    std::vector<gv::gpu_alignment_cigar_job_wire> jobs;
    jobs.reserve(k_job_count);
    for (std::uint32_t i = 0u; i < k_job_count; ++i) {
        jobs.push_back(gv::gpu_alignment_cigar_job_wire{
            .query_symbol_offset = 0u,
            .target_symbol_offset = 0u,
            .query_len = k_length,
            .target_len = k_length,
            .query_global_start = 100u + i * k_length,
            .target_global_start = 200u + i * k_length,
            .band_width = gv::k_gpu_alignment_extension_max_band_width,
            .flags = 0u,
            .cigar_offset = i,
            .cigar_capacity = 1u,
            .traceback_tile_offset = 0u,
            .traceback_tile_count = 0u,
        });
    }
    const gv::gpu_alignment_cigar_params params{
        .alphabet_size = 4u,
        .max_band_width = gv::k_gpu_alignment_extension_max_band_width,
        .tile_query = 128u,
        .tile_target = 128u,
        .match_score = 2,
        .mismatch_penalty = 4,
        .gap_open_short = 4,
        .gap_extend_short = 2,
        .gap_open_long = 24,
        .gap_extend_long = 1,
        .z_drop = 400,
        .end_bonus = 0,
        .flags = 0u,
        .reserved = 0u,
    };

    const std::uint64_t before = runtime.alignment_extension_dispatch_count();
    const gv::alignment_cigar_batch_result result = runtime.run_alignment_simple_cigar_roundtrip(
        query_symbols, target_symbols, jobs, params, k_job_count);
    const std::uint64_t after = runtime.alignment_extension_dispatch_count();

    REQUIRE(after == before + 1u);
    REQUIRE(result.committed_submission_count == 1u);
    REQUIRE(result.results.size() == k_job_count);
    REQUIRE(result.packed_cigar.size() == k_job_count);
    for (std::uint32_t i = 0u; i < k_job_count; ++i) {
        const auto& alignment = result.results[i];
        CHECK(alignment.status == gv::k_gpu_alignment_extension_status_ok);
        CHECK(alignment.score == static_cast<std::int32_t>(k_length * params.match_score));
        CHECK(alignment.query_start == 100u + i * k_length);
        CHECK(alignment.query_end == 100u + (i + 1u) * k_length);
        CHECK(alignment.target_start == 200u + i * k_length);
        CHECK(alignment.target_end == 200u + (i + 1u) * k_length);
        CHECK(alignment.cigar_offset == i);
        CHECK(alignment.cigar_length == 1u);
        CHECK(result.packed_cigar[i] == ((k_length << 4) | 0u));
    }
}

TEST_CASE("Vulkan simple CIGAR scores mismatches",
          "[component:bioseq][layer:unit][feature:chaining][resource:gpu]") {
    auto& runtime = gv::runtime::instance();
    INFO("Vulkan runtime error: " << runtime.initialization_error());
    if (skip_untrusted_vulkan_chaining())
        return;

    constexpr std::uint32_t k_length = 16u;
    std::vector<std::uint32_t> query_symbols(k_length);
    std::vector<std::uint32_t> target_symbols(k_length);
    for (std::uint32_t i = 0; i < k_length; ++i) {
        const std::uint32_t symbol = i & 3u;
        query_symbols[i] = symbol;
        target_symbols[i] = symbol;
    }
    target_symbols[3] = (target_symbols[3] + 1u) & 3u;
    target_symbols[9] = (target_symbols[9] + 2u) & 3u;
    target_symbols[14] = (target_symbols[14] + 3u) & 3u;

    const std::vector<gv::gpu_alignment_cigar_job_wire> jobs{
        gv::gpu_alignment_cigar_job_wire{
            .query_symbol_offset = 0,
            .target_symbol_offset = 0,
            .query_len = k_length,
            .target_len = k_length,
            .query_global_start = 101,
            .target_global_start = 909,
            .band_width = k_length,
            .flags = 0,
            .cigar_offset = 0,
            .cigar_capacity = 1,
            .traceback_tile_offset = 0,
            .traceback_tile_count = (k_length + 1u) * (k_length + 1u),
        },
    };
    const gv::gpu_alignment_cigar_params params{
        .alphabet_size = 4,
        .max_band_width = k_length,
        .tile_query = 128,
        .tile_target = 128,
        .match_score = 2,
        .mismatch_penalty = 4,
        .gap_open_short = 4,
        .gap_extend_short = 2,
        .gap_open_long = 24,
        .gap_extend_long = 1,
        .z_drop = 400,
        .end_bonus = 0,
        .flags = 0,
        .reserved = 0,
    };

    const gv::alignment_cigar_batch_result result =
        runtime.run_alignment_simple_cigar_roundtrip(query_symbols,
                                                     target_symbols,
                                                     jobs,
                                                     params,
                                                     /*packed_cigar_capacity=*/1u);

    REQUIRE(result.results.size() == 1);
    REQUIRE(result.packed_cigar.size() == 1);
    const gv::gpu_alignment_cigar_result_wire& r = result.results.front();
    CHECK(r.status == gv::k_gpu_alignment_extension_status_ok);
    CHECK(r.score ==
          static_cast<std::int32_t>((13u * params.match_score) - (3u * params.mismatch_penalty)));
    CHECK(r.max_score == r.score);
    CHECK(r.query_start == 101u);
    CHECK(r.query_end == 101u + k_length);
    CHECK(r.target_start == 909u);
    CHECK(r.target_end == 909u + k_length);
    CHECK(r.cigar_offset == 0u);
    CHECK(r.cigar_length == 1u);
    CHECK(result.packed_cigar[0] == ((k_length << 4) | 0u));
}

TEST_CASE("Vulkan simple CIGAR emits one-base gaps",
          "[component:bioseq][layer:unit][feature:chaining][resource:gpu]") {
    auto& runtime = gv::runtime::instance();
    INFO("Vulkan runtime error: " << runtime.initialization_error());
    if (skip_untrusted_vulkan_chaining())
        return;

    const std::vector<std::uint32_t> query_symbols{
        0,
        1,
        2,
        3,
        0,
        0,
        1,
        3,
        0,
    };
    const std::vector<std::uint32_t> target_symbols{
        0,
        1,
        3,
        0,
        0,
        1,
        2,
        3,
        0,
    };

    const std::vector<gv::gpu_alignment_cigar_job_wire> jobs{
        gv::gpu_alignment_cigar_job_wire{
            .query_symbol_offset = 0,
            .target_symbol_offset = 0,
            .query_len = 5,
            .target_len = 4,
            .query_global_start = 10,
            .target_global_start = 20,
            .band_width = 5,
            .flags = 0,
            .cigar_offset = 0,
            .cigar_capacity = 3,
            .traceback_tile_offset = 0,
            .traceback_tile_count = 30,
        },
        gv::gpu_alignment_cigar_job_wire{
            .query_symbol_offset = 5,
            .target_symbol_offset = 4,
            .query_len = 4,
            .target_len = 5,
            .query_global_start = 30,
            .target_global_start = 40,
            .band_width = 5,
            .flags = 0,
            .cigar_offset = 3,
            .cigar_capacity = 3,
            .traceback_tile_offset = 30,
            .traceback_tile_count = 30,
        },
    };
    const gv::gpu_alignment_cigar_params params{
        .alphabet_size = 4,
        .max_band_width = 5,
        .tile_query = 128,
        .tile_target = 128,
        .match_score = 2,
        .mismatch_penalty = 4,
        .gap_open_short = 4,
        .gap_extend_short = 2,
        .gap_open_long = 24,
        .gap_extend_long = 1,
        .z_drop = 400,
        .end_bonus = 0,
        .flags = 0,
        .reserved = 0,
    };

    const gv::alignment_cigar_batch_result result =
        runtime.run_alignment_simple_cigar_roundtrip(query_symbols,
                                                     target_symbols,
                                                     jobs,
                                                     params,
                                                     /*packed_cigar_capacity=*/6u);

    REQUIRE(result.results.size() == 2);
    REQUIRE(result.packed_cigar.size() == 6);
    const std::int32_t expected_score = static_cast<std::int32_t>(
        (4u * params.match_score) - (params.gap_open_short + params.gap_extend_short));

    CHECK(result.results[0].status == gv::k_gpu_alignment_extension_status_ok);
    CHECK(result.results[0].score == expected_score);
    CHECK(result.results[0].query_start == 10u);
    CHECK(result.results[0].query_end == 15u);
    CHECK(result.results[0].target_start == 20u);
    CHECK(result.results[0].target_end == 24u);
    CHECK(result.results[0].cigar_offset == 0u);
    CHECK(result.results[0].cigar_length == 3u);
    CHECK(result.packed_cigar[0] == ((2u << 4) | 0u));
    CHECK(result.packed_cigar[1] == ((1u << 4) | 1u));
    CHECK(result.packed_cigar[2] == ((2u << 4) | 0u));

    CHECK(result.results[1].status == gv::k_gpu_alignment_extension_status_ok);
    CHECK(result.results[1].score == expected_score);
    CHECK(result.results[1].query_start == 30u);
    CHECK(result.results[1].query_end == 34u);
    CHECK(result.results[1].target_start == 40u);
    CHECK(result.results[1].target_end == 45u);
    CHECK(result.results[1].cigar_offset == 3u);
    CHECK(result.results[1].cigar_length == 3u);
    CHECK(result.packed_cigar[3] == ((2u << 4) | 0u));
    CHECK(result.packed_cigar[4] == ((1u << 4) | 2u));
    CHECK(result.packed_cigar[5] == ((2u << 4) | 0u));
}

TEST_CASE("Vulkan bounded DP CIGAR emits multiple gaps",
          "[component:bioseq][layer:unit][feature:chaining][resource:gpu]") {
    auto& runtime = gv::runtime::instance();
    INFO("Vulkan runtime error: " << runtime.initialization_error());
    if (skip_untrusted_vulkan_chaining())
        return;

    const std::vector<std::uint32_t> query_symbols{0, 1, 2, 3, 0, 1, 2};
    const std::vector<std::uint32_t> target_symbols{0, 1, 3, 0, 2, 1, 2};

    const std::vector<gv::gpu_alignment_cigar_job_wire> jobs{
        gv::gpu_alignment_cigar_job_wire{
            .query_symbol_offset = 0,
            .target_symbol_offset = 0,
            .query_len = 7,
            .target_len = 7,
            .query_global_start = 100,
            .target_global_start = 200,
            .band_width = 7,
            .flags = 0,
            .cigar_offset = 0,
            .cigar_capacity = 5,
            .traceback_tile_offset = 0,
            .traceback_tile_count = 64,
        },
    };
    const gv::gpu_alignment_cigar_params params{
        .alphabet_size = 4,
        .max_band_width = 7,
        .tile_query = 128,
        .tile_target = 128,
        .match_score = 2,
        .mismatch_penalty = 4,
        .gap_open_short = 4,
        .gap_extend_short = 2,
        .gap_open_long = 24,
        .gap_extend_long = 1,
        .z_drop = 400,
        .end_bonus = 0,
        .flags = 0,
        .reserved = 0,
    };

    const gv::alignment_cigar_batch_result result =
        runtime.run_alignment_simple_cigar_roundtrip(query_symbols,
                                                     target_symbols,
                                                     jobs,
                                                     params,
                                                     /*packed_cigar_capacity=*/5u);

    REQUIRE(result.results.size() == 1);
    REQUIRE(result.packed_cigar.size() == 5);
    const gv::gpu_alignment_cigar_result_wire& r = result.results.front();
    CHECK(r.status == gv::k_gpu_alignment_extension_status_ok);
    CHECK(r.score == 0);
    CHECK(r.max_score == r.score);
    CHECK(r.query_start == 100u);
    CHECK(r.query_end == 107u);
    CHECK(r.target_start == 200u);
    CHECK(r.target_end == 207u);
    CHECK(r.cigar_offset == 0u);
    CHECK(r.cigar_length == 5u);
    CHECK(result.packed_cigar[0] == ((2u << 4) | 0u));
    CHECK(result.packed_cigar[1] == ((1u << 4) | 1u));
    CHECK(result.packed_cigar[2] == ((2u << 4) | 0u));
    CHECK(result.packed_cigar[3] == ((1u << 4) | 2u));
    CHECK(result.packed_cigar[4] == ((2u << 4) | 0u));
}

TEST_CASE("Vulkan long greedy CIGAR emits three gaps",
          "[component:bioseq][layer:unit][feature:chaining][resource:gpu]") {
    auto& runtime = gv::runtime::instance();
    INFO("Vulkan runtime error: " << runtime.initialization_error());
    if (skip_untrusted_vulkan_chaining())
        return;

    constexpr std::uint32_t k_target_length = 620u;
    std::vector<std::uint32_t> target_symbols(k_target_length);
    std::uint32_t state = 0x12345678u;
    for (std::uint32_t i = 0; i < k_target_length; ++i) {
        state = state * 1664525u + 1013904223u;
        target_symbols[i] = (state >> 30u) & 3u;
    }
    target_symbols[200] = (target_symbols[208] + 1u) & 3u;

    std::vector<std::uint32_t> query_symbols;
    query_symbols.reserve(k_target_length);
    query_symbols.insert(query_symbols.end(), target_symbols.begin(), target_symbols.begin() + 200);
    query_symbols.insert(
        query_symbols.end(), target_symbols.begin() + 208, target_symbols.begin() + 400);
    const std::uint32_t inserted_symbol = (target_symbols[400] + 1u) & 3u;
    for (std::uint32_t i = 0; i < 16u; ++i) {
        query_symbols.push_back(inserted_symbol);
    }
    query_symbols.insert(
        query_symbols.end(), target_symbols.begin() + 400, target_symbols.begin() + 612);
    REQUIRE(query_symbols.size() == k_target_length);

    const std::vector<gv::gpu_alignment_cigar_job_wire> jobs{
        gv::gpu_alignment_cigar_job_wire{
            .query_symbol_offset = 0,
            .target_symbol_offset = 0,
            .query_len = static_cast<std::uint32_t>(query_symbols.size()),
            .target_len = k_target_length,
            .query_global_start = 500,
            .target_global_start = 900,
            .band_width = gv::k_gpu_alignment_extension_max_band_width,
            .flags = 0,
            .cigar_offset = 0,
            .cigar_capacity = 6,
            .traceback_tile_offset = 0,
            .traceback_tile_count = 0,
        },
    };
    const gv::gpu_alignment_cigar_params params{
        .alphabet_size = 4,
        .max_band_width = gv::k_gpu_alignment_extension_max_band_width,
        .tile_query = 128,
        .tile_target = 128,
        .match_score = 2,
        .mismatch_penalty = 4,
        .gap_open_short = 4,
        .gap_extend_short = 2,
        .gap_open_long = 24,
        .gap_extend_long = 1,
        .z_drop = 400,
        .end_bonus = 0,
        .flags = 0,
        .reserved = 0,
    };

    const gv::alignment_cigar_batch_result result =
        runtime.run_alignment_simple_cigar_roundtrip(query_symbols,
                                                     target_symbols,
                                                     jobs,
                                                     params,
                                                     /*packed_cigar_capacity=*/6u);

    REQUIRE(result.results.size() == 1);
    REQUIRE(result.packed_cigar.size() == 6);
    const gv::gpu_alignment_cigar_result_wire& r = result.results.front();
    CHECK(r.status == gv::k_gpu_alignment_extension_status_ok);
    CHECK(r.score == 1132);
    CHECK(r.query_start == 500u);
    CHECK(r.query_end == 1120u);
    CHECK(r.target_start == 900u);
    CHECK(r.target_end == 1520u);
    CHECK(r.cigar_length == 6u);
    CHECK(result.packed_cigar[0] == ((200u << 4) | 0u));
    CHECK(result.packed_cigar[1] == ((8u << 4) | 2u));
    CHECK(result.packed_cigar[2] == ((192u << 4) | 0u));
    CHECK(result.packed_cigar[3] == ((16u << 4) | 1u));
    CHECK(result.packed_cigar[4] == ((212u << 4) | 0u));
    CHECK(result.packed_cigar[5] == ((8u << 4) | 2u));
}

TEST_CASE("Vulkan gridded CIGAR handles long many-gap traceback",
          "[component:bioseq][layer:unit][feature:chaining][resource:gpu]") {
    auto& runtime = gv::runtime::instance();
    INFO("Vulkan runtime error: " << runtime.initialization_error());
    if (skip_untrusted_vulkan_chaining())
        return;

    constexpr std::uint32_t k_target_length = 900u;
    constexpr std::uint32_t k_deletion_count = 80u;
    constexpr std::uint32_t k_first_deletion = 20u;
    constexpr std::uint32_t k_deletion_spacing = 10u;
    std::vector<std::uint32_t> target_symbols(k_target_length);
    std::uint32_t state = 0x5EED1234u;
    for (std::uint32_t i = 0; i < k_target_length; ++i) {
        state = state * 1664525u + 1013904223u;
        target_symbols[i] = (state >> 30u) & 3u;
    }

    std::vector<std::uint32_t> query_symbols;
    query_symbols.reserve(k_target_length - k_deletion_count);
    std::uint32_t skipped = 0;
    for (std::uint32_t i = 0; i < k_target_length; ++i) {
        const bool skip_base = skipped < k_deletion_count && i >= k_first_deletion &&
                               ((i - k_first_deletion) % k_deletion_spacing) == 0u;
        if (skip_base) {
            ++skipped;
            continue;
        }
        query_symbols.push_back(target_symbols[i]);
    }
    REQUIRE(skipped == k_deletion_count);
    REQUIRE(query_symbols.size() == k_target_length - k_deletion_count);

    const std::uint64_t traceback_words_u64 = gv::gpu_alignment_cigar_gridded_traceback_words(
        static_cast<std::uint32_t>(query_symbols.size()),
        k_target_length,
        gv::k_gpu_alignment_cigar_traceback_tile_len,
        gv::k_gpu_alignment_cigar_traceback_tile_len,
        gv::k_gpu_alignment_extension_max_band_width);
    REQUIRE(traceback_words_u64 <= std::numeric_limits<std::uint32_t>::max());

    const std::vector<gv::gpu_alignment_cigar_job_wire> jobs{
        gv::gpu_alignment_cigar_job_wire{
            .query_symbol_offset = 0,
            .target_symbol_offset = 0,
            .query_len = static_cast<std::uint32_t>(query_symbols.size()),
            .target_len = k_target_length,
            .query_global_start = 700,
            .target_global_start = 1100,
            .band_width = gv::k_gpu_alignment_extension_max_band_width,
            .flags = 0,
            .cigar_offset = 0,
            .cigar_capacity = 256,
            .traceback_tile_offset = 0,
            .traceback_tile_count = 0u,
        },
    };
    const gv::gpu_alignment_cigar_params params{
        .alphabet_size = 4,
        .max_band_width = gv::k_gpu_alignment_extension_max_band_width,
        .tile_query = gv::k_gpu_alignment_cigar_traceback_tile_len,
        .tile_target = gv::k_gpu_alignment_cigar_traceback_tile_len,
        .match_score = 2,
        .mismatch_penalty = 4,
        .gap_open_short = 4,
        .gap_extend_short = 2,
        .gap_open_long = 24,
        .gap_extend_long = 1,
        .z_drop = 400,
        .end_bonus = 0,
        .flags = 0,
        .reserved = 0,
    };

    const std::uint64_t before = runtime.alignment_extension_dispatch_count();
    const gv::alignment_cigar_batch_result result =
        runtime.run_alignment_simple_cigar_roundtrip(query_symbols,
                                                     target_symbols,
                                                     jobs,
                                                     params,
                                                     /*packed_cigar_capacity=*/256u);
    const std::uint64_t after = runtime.alignment_extension_dispatch_count();

    REQUIRE(after == before + 2u);
    REQUIRE(result.committed_submission_count == 2u);
    REQUIRE(result.results.size() == 1);
    const gv::gpu_alignment_cigar_result_wire& r = result.results.front();
    REQUIRE(result.packed_cigar.size() == r.cigar_length);
    CHECK(r.status == gv::k_gpu_alignment_extension_status_ok);
    CHECK(r.score == static_cast<std::int32_t>(
                         query_symbols.size() * params.match_score -
                         k_deletion_count * (params.gap_open_short + params.gap_extend_short)));
    CHECK(r.query_start == 700u);
    CHECK(r.query_end == 700u + query_symbols.size());
    CHECK(r.target_start == 1100u);
    CHECK(r.target_end == 1100u + k_target_length);
    REQUIRE(r.cigar_length > 64u);
    REQUIRE(r.cigar_length <= 256u);

    std::uint32_t consumed_query = 0;
    std::uint32_t consumed_target = 0;
    std::uint32_t deleted_bases = 0;
    std::uint32_t inserted_bases = 0;
    for (std::uint32_t i = 0; i < r.cigar_length; ++i) {
        const std::uint32_t entry = result.packed_cigar[r.cigar_offset + i];
        const std::uint32_t len = entry >> 4u;
        const std::uint32_t op = entry & 0xfu;
        if (op == 0u) {
            consumed_query += len;
            consumed_target += len;
        } else if (op == 1u) {
            consumed_query += len;
            inserted_bases += len;
        } else if (op == 2u) {
            consumed_target += len;
            deleted_bases += len;
        }
    }
    CHECK(consumed_query == query_symbols.size());
    CHECK(consumed_target == k_target_length);
    CHECK(deleted_bases == k_deletion_count);
    CHECK(inserted_bases == 0u);
}

// ===========================================================================
// Large coordinates (> 2^32) are not truncated
// ===========================================================================

TEST_CASE("Vulkan chaining preserves ref_position > 2^32",
          "[component:bioseq][layer:unit][feature:chaining][resource:gpu]") {
    if (skip_untrusted_vulkan_chaining())
        return;
    const auto params = default_chain_params();
    const std::uint64_t base = (std::uint64_t{1} << 32) + 5'000'000'000ULL; // ~9.3 Gbp
    std::vector<ch::anchor> anchors;
    for (std::uint64_t i = 0; i < 12; ++i) {
        anchors.push_back(make_anchor(base + i * 12, 1000 + i * 12, 0, 15));
    }
    const auto out = run_vulkan_chain(anchors, params);
    check_chain_invariants(out, anchors, params);
    REQUIRE_FALSE(out.chains.empty());
    // Every emitted ref_position is still above 2^32 — no narrowing to 32 bits.
    for (const auto& emitted : out.reordered) {
        REQUIRE(emitted.ref_position > (std::uint64_t{1} << 32));
        REQUIRE(emitted.ref_position >= base);
    }
}

// ===========================================================================
// Shader compile and SPIR-V direct-anchor layout
// ===========================================================================

TEST_CASE("chain_tiled_dp shader compiles with the anchor layout contract",
          "[component:bioseq][layer:unit][feature:chaining]") {
    // The push-constant block includes max_chain_skip and is pinned to 36 bytes;
    // the gpu_anchor_wire stride and offsets below are pinned independently.
    static_assert(sizeof(gv::chaining_push_constants) == 36);

    // Fixtures spanning the selector's three branches plus the keys
    // choose_vulkan_chaining_pipeline picks for small/medium/large devices.
    std::vector<gv::chaining_pipeline_key> keys = {
        {384u, 128u},
        {512u, 128u},
        {1024u, 256u},
    };
    auto fixture_key = [](std::uint32_t inv, std::uint32_t shared) {
        gv::vulkan_feature_set f;
        f.max_workgroup_invocations = inv;
        f.max_shared_memory_bytes = shared;
        return ch::choose_vulkan_chaining_pipeline(f);
    };
    keys.push_back(fixture_key(128u, 16u * 1024u));  // -> 384/128
    keys.push_back(fixture_key(512u, 32u * 1024u));  // -> 512/128
    keys.push_back(fixture_key(1024u, 48u * 1024u)); // -> 1024/256

    for (const auto& key : keys) {
        INFO("tile_size=" << key.tile_size << " local_size=" << key.local_size);
        const auto spirv = gv::compile_shader_spirv(gv::shader_compile_request{
            gv::shader_kind::chain_tiled_dp_u64, key.tile_size, key.local_size});
        REQUIRE_FALSE(spirv.empty());

        const auto scan = gv::scan_spirv_layout(spirv);
        // Anchors runtime-array stride is exactly the 24-byte anchor.
        REQUIRE(scan.has_array_stride(24));
        // gpu_anchor_wire member offsets are {ref(0), query(8), is_reverse(16), span(20)}.
        REQUIRE(scan.has_struct_offsets({0u, 8u, 16u, 20u}));
        // All four descriptor bindings are present.
        REQUIRE(scan.has_binding(0));
        REQUIRE(scan.has_binding(1));
        REQUIRE(scan.has_binding(2));
        REQUIRE(scan.has_binding(3));
    }
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
