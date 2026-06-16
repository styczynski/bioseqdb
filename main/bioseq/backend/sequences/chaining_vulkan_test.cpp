// main/bioseq/backend/sequences/chaining_vulkan_test.cpp
//
// DOC0304 §10 — validation tests for the Phase A Vulkan (G3SA tiled-DP)
// chaining backend.
//
//   §10.1 Forced-fallback parity  — VULKAN_TILED_DP with vulkan_enabled=false
//                                    must equal SCALAR_MM2 through the public
//                                    pipeline (the only exact-equality test).
//   §10.2 GPU chaining invariants  — chain_anchors_vulkan output is structurally
//                                    valid (chain count, bounds, monotonicity,
//                                    range boundaries, min-score, span!=15,
//                                    equal-score tie policy).
//   §10.3 End-to-end GPU smoke     — when a usable device exists, the pipeline
//                                    dispatches the kernel and emits matches.
//   §10.4 Large coordinates        — ref_position > 2^32 is not truncated.
//   §10.6 Shader compile/layout    — the checked-in GLSL compiles for every
//                                    pipeline key and the SPIR-V carries the
//                                    direct-anchor ABI (stride 24, offsets
//                                    {0,8,16,20}, bindings 0..3, 32-byte push).
//
// The GPU-only cases skip cleanly when vulkan_chaining_supported() is false;
// the parity / shader-layout cases run on every machine.

#include "common_simd.h"
#include "common_tests.h"
#include "common.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.common.context;
import bioseq.sequences;
import bioseq.sequences.minimap_port;   // Minimap2Params / MatchResult(Collection)
import bioseq.sequences.pipe;           // minimap_port entry
import bioseq.sequences.chaining.utils; // Anchor / Chain / ChainingParams
import bioseq.sequences.chaining.vulkan;// chain_anchors_vulkan / vulkan_chaining_supported
import bioseq.gpu.vulkan.runtime;       // Runtime / scan_spirv_layout / pipeline key
import bioseq.gpu.vulkan.shader_compiler;
import bioseq.storage.lazy;             // make_lazy_in_memory
import bioseq.common.testing;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {

namespace {

namespace ch = ::bioseq::chaining;
namespace gv = ::bioseq::gpu::vulkan;

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

/// Reverse complement of a dna4 vector (rank complement is 3 - rank).
[[nodiscard]] std::vector<seq::dna4> revcomp(const std::vector<seq::dna4>& in) {
    std::vector<seq::dna4> out(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        seq::dna4 c;
        c.assign_rank(static_cast<decltype(c.to_rank())>(3 - in[in.size() - 1 - i].to_rank()));
        out[i] = c;
    }
    return out;
}

/// Canonicalise a MatchResultCollection into a comparable, sorted string set
/// (same shape used by lazy_pipeline_parity_test.cpp).
[[nodiscard]] std::vector<std::string> canon(const MatchResultCollection& c) {
    std::vector<std::string> out;
    for (const auto& m : c.results) {
        std::string cg;
        for (const auto& op : m.cigar)
            cg += fmt::format("{}:{},", op.length, static_cast<int>(op.opcode));
        out.push_back(fmt::format(
            "qs={} qe={} rev={} ts={} te={} nm={} bl={} mapq={} cs={} cg={}",
            m.query_start, m.query_end, m.is_reverse, m.target_start, m.target_end,
            m.num_matches, m.block_length, m.mapq, m.chain_score, cg));
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// Run the full pipeline over an in-memory query/target with the given params.
/// Takes the sequences by value so they are non-const lvalues: the
/// `compact_sequence_range` constructor binds a non-const reference to the
/// underlying container (matching lazy_pipeline_parity_test.cpp's usage).
[[nodiscard]] MatchResultCollection run_pipeline(
    std::vector<seq::dna4> qd,
    std::vector<seq::dna4> td,
    const Minimap2Params& params)
{
    range_t q_range(qd);
    range_t t_range(td);
    auto lazy_q = bioseq::storage::make_lazy_in_memory(q_range);
    auto lazy_t = bioseq::storage::make_lazy_in_memory(t_range);
    return minimap_port(lazy_q, lazy_t, params);
}

[[nodiscard]] ch::Anchor make_anchor(
    std::uint64_t ref, std::uint64_t query, std::uint32_t is_reverse, std::uint8_t span)
{
    ch::Anchor a{};
    a.ref_position = ref;
    a.query_position = query;
    a.is_reverse = is_reverse;
    a.query_span = span;
    a.flags = 0;
    a.segment_id = 0;
    return a;
}

/// Copies of the chain outputs, taken before the local context unwinds.
struct ChainOutputs {
    std::vector<ch::Chain> chains;
    std::vector<ch::Anchor> reordered;
};

/// Install a fresh arena context, run chain_anchors_vulkan, and copy the
/// outputs out (the slabs live in the local context's chain arena).
[[nodiscard]] ChainOutputs run_vulkan_chain(
    std::span<const ch::Anchor> anchors, const ch::ChainingParams& params)
{
    context::context ctx;
    context::installed_context_scope install{ctx};
    auto chains_slab = ch::make_arena_slab<ch::Chain>();
    auto reordered_slab = ch::make_arena_slab<ch::Anchor>();
    // Reserve a guaranteed upper bound BEFORE the call, exactly as
    // ChainStage::process does. The chaining kernel pushes its own internal
    // Arena::Chain scope and `backtrack_chains` writes the output via
    // push_back; without a prior reserve those allocations land ABOVE the
    // kernel's scope marker and are freed (0xDD-poisoned) when the kernel
    // returns. #chains and #reordered-anchors are each <= #input anchors.
    const std::size_t upper_bound = anchors.size() + 1;
    chains_slab.reserve(upper_bound);
    reordered_slab.reserve(upper_bound);
    ch::chain_anchors_vulkan(anchors, params, chains_slab, reordered_slab);
    ChainOutputs out;
    out.chains.assign(chains_slab.data(), chains_slab.data() + chains_slab.size());
    out.reordered.assign(reordered_slab.data(), reordered_slab.data() + reordered_slab.size());
    return out;
}

[[nodiscard]] ch::ChainingParams default_chain_params() {
    ch::ChainingParams params;  // kmer_size=15, max_gap=5000, bandwidth=500, ...
    params.prepare();
    return params;
}

/// Assert every structural invariant DOC0304 §10.2 requires of a chain set.
void check_chain_invariants(
    const ChainOutputs& out,
    std::span<const ch::Anchor> anchors,
    const ch::ChainingParams& params)
{
    REQUIRE(out.chains.size() <= anchors.size());
    for (const auto& chain : out.chains) {
        REQUIRE(chain.anchor_count >= params.min_seed_count);
        REQUIRE(chain.score >= params.min_chain_score);
        REQUIRE(chain.anchor_start >= 0);
        const std::size_t start = static_cast<std::size_t>(chain.anchor_start);
        const std::size_t count = static_cast<std::size_t>(chain.anchor_count);
        REQUIRE(start + count <= out.reordered.size());

        const std::uint64_t max_dist_x =
            static_cast<std::uint64_t>(std::max(params.max_gap, params.bandwidth));
        for (std::size_t i = start + 1; i < start + count; ++i) {
            const ch::Anchor& prev = out.reordered[i - 1];
            const ch::Anchor& cur = out.reordered[i];
            // Monotonic in both ref and query position.
            REQUIRE(cur.ref_position > prev.ref_position);
            REQUIRE(cur.query_position > prev.query_position);
            // No chain crosses a hard range boundary.
            REQUIRE(cur.ref_chromosome_key() == prev.ref_chromosome_key());
            REQUIRE(cur.ref_position - prev.ref_position <= max_dist_x);
        }
    }
}

}  // namespace

// ===========================================================================
// §10.1 — Forced-fallback parity (the only exact-equality test)
// ===========================================================================

TEST_CASE("DOC0304 §10.1 — VULKAN_TILED_DP forced fallback == SCALAR_MM2",
          "[g3sa][vulkan][chaining][parity]") {
    Minimap2Params scalar_params;
    scalar_params.chaining_algorithm = ChainingAlgorithmType::SCALAR_MM2;

    Minimap2Params vulkan_params;
    vulkan_params.chaining_algorithm = ChainingAlgorithmType::VULKAN_TILED_DP;
    vulkan_params.vulkan_enabled = false;  // force the CPU fallback path

    SECTION("empty anchor set produces no chains (chaining boundary)") {
        // minimap_port does not accept empty sequences (pre-existing); the
        // spec's "empty query/target" parity scenario is meaningful at the
        // chaining boundary — zero anchors must yield zero chains on the
        // forced-fallback path, exactly as the scalar backend does.
        ch::ChainingParams cp;
        cp.vulkan_enabled = false;  // force the CPU fallback
        cp.prepare();
        const std::vector<ch::Anchor> none;
        const auto out = run_vulkan_chain(none, cp);
        REQUIRE(out.chains.empty());
        REQUIRE(out.reordered.empty());
    }

    SECTION("short forward embedding (few anchors, one diagonal)") {
        auto qd = make_dna4(80, 0xA11CE);
        auto td = make_dna4(400, 0xB0B);
        for (std::size_t i = 0; i < qd.size(); ++i) td[150 + i] = qd[i];
        const auto a = canon(run_pipeline(qd, td, scalar_params));
        const auto b = canon(run_pipeline(qd, td, vulkan_params));
        INFO("scalar matches=" << a.size() << " vulkan-fallback matches=" << b.size());
        REQUIRE(a == b);
        REQUIRE_FALSE(a.empty());
    }

    SECTION("longer forward embedding (many anchors)") {
        auto qd = make_dna4(300, 0xC0FFEE);
        auto td = make_dna4(1200, 0xDEAD);
        for (std::size_t i = 0; i < qd.size(); ++i) td[400 + i] = qd[i];
        const auto a = canon(run_pipeline(qd, td, scalar_params));
        const auto b = canon(run_pipeline(qd, td, vulkan_params));
        REQUIRE(a == b);
        REQUIRE_FALSE(a.empty());
    }

    SECTION("reverse-complement embedding (mixed/reverse anchors)") {
        auto qd = make_dna4(200, 0x5EED);
        auto td = make_dna4(900, 0xF00D);
        const auto rc = revcomp(qd);
        for (std::size_t i = 0; i < rc.size(); ++i) td[350 + i] = rc[i];
        const auto a = canon(run_pipeline(qd, td, scalar_params));
        const auto b = canon(run_pipeline(qd, td, vulkan_params));
        REQUIRE(a == b);
        REQUIRE_FALSE(a.empty());
    }

    SECTION("non-default kmer size (query_span != 15, generic-fallback path)") {
        Minimap2Params s = scalar_params;
        Minimap2Params v = vulkan_params;
        s.kmer_size = 13;
        v.kmer_size = 13;
        auto qd = make_dna4(250, 0x1357);
        auto td = make_dna4(1000, 0x2468);
        for (std::size_t i = 0; i < qd.size(); ++i) td[300 + i] = qd[i];
        const auto a = canon(run_pipeline(qd, td, s));
        const auto b = canon(run_pipeline(qd, td, v));
        REQUIRE(a == b);
        REQUIRE_FALSE(a.empty());
    }
}

// ===========================================================================
// §10.2 — GPU chaining invariants (chain_anchors_vulkan directly)
// ===========================================================================

TEST_CASE("DOC0304 §10.2 — Vulkan chaining output invariants",
          "[g3sa][vulkan][chaining][invariant]") {
    const auto params = default_chain_params();

    SECTION("forward diagonal, 12 anchors, span==15") {
        std::vector<ch::Anchor> anchors;
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
        std::vector<ch::Anchor> anchors;
        for (std::uint64_t i = 0; i < 10; ++i) {
            anchors.push_back(make_anchor(2000 + i * 11, 1000 + i * 11, 0, 11));
        }
        const auto out = run_vulkan_chain(anchors, params);
        check_chain_invariants(out, anchors, params);
        REQUIRE_FALSE(out.chains.empty());
    }

    SECTION("mixed forward/reverse anchors (forward sort before reverse)") {
        std::vector<ch::Anchor> anchors;
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
        // successor; the G3SA-style `>=` replacement must still produce a chain.
        std::vector<ch::Anchor> anchors;
        for (std::uint64_t i = 0; i < 10; ++i) {
            anchors.push_back(make_anchor(3000 + i * 15, 2000 + i * 15, 0, 15));
        }
        const auto out = run_vulkan_chain(anchors, params);
        check_chain_invariants(out, anchors, params);
        REQUIRE_FALSE(out.chains.empty());
    }

    SECTION("hard range split: a large ref gap splits chains") {
        std::vector<ch::Anchor> anchors;
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
        check_chain_invariants(out, anchors, params);  // boundary invariant enforced here
    }
}

// ===========================================================================
// §10.3 — End-to-end GPU smoke (requires a usable device)
// ===========================================================================

TEST_CASE("DOC0304 §10.3 — end-to-end GPU chaining smoke",
          "[g3sa][vulkan][gpu][smoke]") {
    if (!ch::vulkan_chaining_supported()) {
        SUCCEED("No usable Vulkan device; GPU smoke test skipped (CPU fallback path).");
        return;
    }

    Minimap2Params params;
    params.chaining_algorithm = ChainingAlgorithmType::VULKAN_TILED_DP;
    params.vulkan_enabled = true;
    params.minimizer_algorithm = MinimizerAlgorithmType::TWO_STACK_NTHASH;

    auto qd = make_dna4(300, 0x6A5A);
    auto td = make_dna4(1500, 0x9C9C);
    for (std::size_t i = 0; i < qd.size(); ++i) td[600 + i] = qd[i];

    const std::uint64_t before = gv::Runtime::instance().chaining_dispatch_count();
    const auto matches = run_pipeline(qd, td, params);
    const std::uint64_t after = gv::Runtime::instance().chaining_dispatch_count();

    INFO("dispatch_count before=" << before << " after=" << after
                                  << " matches=" << matches.results.size());
    REQUIRE(after > before);                    // the GPU kernel actually ran
    REQUIRE_FALSE(matches.results.empty());     // at least one match emitted
    for (const auto& m : matches.results) {
        REQUIRE(m.query_start >= 0);
        REQUIRE(m.query_end <= static_cast<int32_t>(qd.size()));
        REQUIRE(m.target_start >= 0);
        REQUIRE(m.target_end <= static_cast<int32_t>(td.size()));
    }
}

// ===========================================================================
// §10.4 — Large coordinates (> 2^32) are not truncated
// ===========================================================================

TEST_CASE("DOC0304 §10.4 — Vulkan chaining preserves ref_position > 2^32",
          "[g3sa][vulkan][chaining][coords]") {
    const auto params = default_chain_params();
    const std::uint64_t base = (std::uint64_t{1} << 32) + 5'000'000'000ULL;  // ~9.3 Gbp
    std::vector<ch::Anchor> anchors;
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
// §10.6 — Shader compile + SPIR-V direct-anchor ABI layout
// ===========================================================================

TEST_CASE("DOC0304 §10.6 — chain_tiled_dp shader compiles with the anchor ABI",
          "[g3sa][vulkan][shader][layout]") {
    // The push-constant block layout is pinned in C++ (36 bytes since DOC0311
    // added max_chain_skip; the AnchorWire stride/offsets below are unchanged).
    static_assert(sizeof(gv::ChainingPushConstants) == 36);

    // Fixtures spanning the selector's three branches plus the keys
    // choose_vulkan_chaining_pipeline picks for small/medium/large devices.
    std::vector<gv::ChainingPipelineKey> keys = {
        {384u, 128u},
        {512u, 128u},
        {1024u, 256u},
    };
    auto fixture_key = [](std::uint32_t inv, std::uint32_t shared) {
        gv::VulkanFeatureSet f;
        f.max_workgroup_invocations = inv;
        f.max_shared_memory_bytes = shared;
        return ch::choose_vulkan_chaining_pipeline(f);
    };
    keys.push_back(fixture_key(128u, 16u * 1024u));        // -> 384/128
    keys.push_back(fixture_key(512u, 32u * 1024u));        // -> 512/128
    keys.push_back(fixture_key(1024u, 48u * 1024u));       // -> 1024/256

    for (const auto& key : keys) {
        INFO("tile_size=" << key.tile_size << " local_size=" << key.local_size);
        const auto spirv = gv::compile_shader_spirv(gv::ShaderCompileRequest{
            gv::ShaderKind::chain_tiled_dp_u64, key.tile_size, key.local_size});
        REQUIRE_FALSE(spirv.empty());

        const auto scan = gv::scan_spirv_layout(spirv);
        // Anchors runtime-array stride is exactly the 24-byte Anchor.
        REQUIRE(scan.has_array_stride(24));
        // AnchorWire member offsets are {ref(0), query(8), is_reverse(16), span(20)}.
        REQUIRE(scan.has_struct_offsets({0u, 8u, 16u, 20u}));
        // All four descriptor bindings are present.
        REQUIRE(scan.has_binding(0));
        REQUIRE(scan.has_binding(1));
        REQUIRE(scan.has_binding(2));
        REQUIRE(scan.has_binding(3));
    }
}

}  // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
