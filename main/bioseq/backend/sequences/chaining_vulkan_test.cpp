// main/bioseq/backend/sequences/chaining_vulkan_test.cpp
//
// DOC0304 §10 — validation tests for the Phase A Vulkan (G3SA tiled-DP)
// chaining backend.
//
//   §10.1 Mandatory Vulkan errors — VULKAN_TILED_DP with vulkan_enabled=false
//                                    must fail instead of running SCALAR_MM2.
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
// §10.1 — Mandatory Vulkan errors
// ===========================================================================

TEST_CASE("DOC0304 §10.1 — VULKAN_TILED_DP with vulkan_enabled=false fails",
          "[g3sa][vulkan][chaining][mandatory]") {
    Minimap2Params vulkan_params;
    vulkan_params.chaining_algorithm = ChainingAlgorithmType::VULKAN_TILED_DP;
    vulkan_params.vulkan_enabled = false;

    SECTION("direct chaining backend rejects disabled Vulkan") {
        ch::ChainingParams cp;
        cp.vulkan_enabled = false;
        cp.prepare();
        std::vector<ch::Anchor> anchors;
        for (std::uint64_t i = 0; i < 8; ++i) {
            anchors.push_back(make_anchor(1000 + i * 12, 500 + i * 12, 0, 15));
        }
        REQUIRE_THROWS_AS(run_vulkan_chain(anchors, cp), gv::VulkanRuntimeError);
    }

    SECTION("public pipeline rejects selected Vulkan backend") {
        auto qd = make_dna4(250, 0x1357);
        auto td = make_dna4(1000, 0x2468);
        for (std::size_t i = 0; i < qd.size(); ++i) td[300 + i] = qd[i];
        REQUIRE_THROWS_AS(run_pipeline(qd, td, vulkan_params), gv::VulkanRuntimeError);
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
    INFO("Vulkan runtime error: " << gv::Runtime::instance().initialization_error());
    REQUIRE(ch::vulkan_chaining_supported());

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

TEST_CASE("DOC0354 Part 2 slice - Vulkan simple extension emits CIGAR",
          "[g3sa][vulkan][gpu][cigar]") {
    auto& runtime = gv::Runtime::instance();
    INFO("Vulkan runtime error: " << runtime.initialization_error());
    REQUIRE(ch::vulkan_chaining_supported());

    constexpr std::uint32_t kLen = 100u;
    std::vector<std::uint32_t> query_symbols(kLen);
    std::vector<std::uint32_t> target_symbols(kLen);
    for (std::uint32_t i = 0; i < kLen; ++i) {
        const std::uint32_t symbol = (i * 7u + 3u) & 3u;
        query_symbols[i] = symbol;
        target_symbols[i] = symbol;
    }

    const std::vector<gv::GpuAlignmentCigarJobWire> jobs{
        gv::GpuAlignmentCigarJobWire{
            .query_symbol_offset = 0,
            .target_symbol_offset = 0,
            .query_len = kLen,
            .target_len = kLen,
            .query_global_start = 11,
            .target_global_start = 2007,
            .band_width = kLen,
            .flags = 0,
            .cigar_offset = 0,
            .cigar_capacity = 1,
            .traceback_tile_offset = 0,
            .traceback_tile_count = (kLen + 1u) * (kLen + 1u),
        },
    };
    const gv::GpuAlignmentCigarParams params{
        .alphabet_size = 4,
        .max_band_width = kLen,
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
    const gv::AlignmentCigarBatchResult result =
        runtime.run_alignment_simple_cigar_roundtrip(
            query_symbols, target_symbols, jobs, params,
            /*packed_cigar_capacity=*/1u);
    const std::uint64_t after = runtime.alignment_extension_dispatch_count();

    REQUIRE(after > before);
    REQUIRE(result.results.size() == 1);
    REQUIRE(result.packed_cigar.size() == 1);
    const gv::GpuAlignmentCigarResultWire& r = result.results.front();
    CHECK(r.status == gv::k_gpu_alignment_extension_status_ok);
    CHECK(r.score == static_cast<std::int32_t>(kLen * params.match_score));
    CHECK(r.max_score == r.score);
    CHECK(r.query_start == 11u);
    CHECK(r.query_end == 11u + kLen);
    CHECK(r.target_start == 2007u);
    CHECK(r.target_end == 2007u + kLen);
    CHECK(r.cigar_offset == 0u);
    CHECK(r.cigar_length == 1u);
    CHECK(result.packed_cigar[0] == ((kLen << 4) | 0u));
}

TEST_CASE("DOC0354 Part 2 slice - Vulkan long exact CIGAR avoids bounded traceback",
          "[g3sa][vulkan][gpu][cigar]") {
    auto& runtime = gv::Runtime::instance();
    INFO("Vulkan runtime error: " << runtime.initialization_error());
    REQUIRE(ch::vulkan_chaining_supported());

    constexpr std::uint32_t kLen = 2048u;
    std::vector<std::uint32_t> query_symbols(kLen);
    std::vector<std::uint32_t> target_symbols(kLen);
    for (std::uint32_t i = 0; i < kLen; ++i) {
        const std::uint32_t symbol = (i * 5u + 1u) & 3u;
        query_symbols[i] = symbol;
        target_symbols[i] = symbol;
    }

    const std::vector<gv::GpuAlignmentCigarJobWire> jobs{
        gv::GpuAlignmentCigarJobWire{
            .query_symbol_offset = 0,
            .target_symbol_offset = 0,
            .query_len = kLen,
            .target_len = kLen,
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
    const gv::GpuAlignmentCigarParams params{
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

    const gv::AlignmentCigarBatchResult result =
        runtime.run_alignment_simple_cigar_roundtrip(
            query_symbols, target_symbols, jobs, params,
            /*packed_cigar_capacity=*/1u);

    REQUIRE(result.results.size() == 1);
    REQUIRE(result.packed_cigar.size() == 1);
    const gv::GpuAlignmentCigarResultWire& r = result.results.front();
    CHECK(r.status == gv::k_gpu_alignment_extension_status_ok);
    CHECK(r.score == static_cast<std::int32_t>(kLen * params.match_score));
    CHECK(r.query_start == 17u);
    CHECK(r.query_end == 17u + kLen);
    CHECK(r.target_start == 3001u);
    CHECK(r.target_end == 3001u + kLen);
    CHECK(r.cigar_length == 1u);
    CHECK(result.packed_cigar[0] == ((kLen << 4) | 0u));
}

TEST_CASE("DOC0354 Part 2 slice - Vulkan simple CIGAR scores mismatches",
          "[g3sa][vulkan][gpu][cigar]") {
    auto& runtime = gv::Runtime::instance();
    INFO("Vulkan runtime error: " << runtime.initialization_error());
    REQUIRE(ch::vulkan_chaining_supported());

    constexpr std::uint32_t kLen = 16u;
    std::vector<std::uint32_t> query_symbols(kLen);
    std::vector<std::uint32_t> target_symbols(kLen);
    for (std::uint32_t i = 0; i < kLen; ++i) {
        const std::uint32_t symbol = i & 3u;
        query_symbols[i] = symbol;
        target_symbols[i] = symbol;
    }
    target_symbols[3] = (target_symbols[3] + 1u) & 3u;
    target_symbols[9] = (target_symbols[9] + 2u) & 3u;
    target_symbols[14] = (target_symbols[14] + 3u) & 3u;

    const std::vector<gv::GpuAlignmentCigarJobWire> jobs{
        gv::GpuAlignmentCigarJobWire{
            .query_symbol_offset = 0,
            .target_symbol_offset = 0,
            .query_len = kLen,
            .target_len = kLen,
            .query_global_start = 101,
            .target_global_start = 909,
            .band_width = kLen,
            .flags = 0,
            .cigar_offset = 0,
            .cigar_capacity = 1,
            .traceback_tile_offset = 0,
            .traceback_tile_count = (kLen + 1u) * (kLen + 1u),
        },
    };
    const gv::GpuAlignmentCigarParams params{
        .alphabet_size = 4,
        .max_band_width = kLen,
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

    const gv::AlignmentCigarBatchResult result =
        runtime.run_alignment_simple_cigar_roundtrip(
            query_symbols, target_symbols, jobs, params,
            /*packed_cigar_capacity=*/1u);

    REQUIRE(result.results.size() == 1);
    REQUIRE(result.packed_cigar.size() == 1);
    const gv::GpuAlignmentCigarResultWire& r = result.results.front();
    CHECK(r.status == gv::k_gpu_alignment_extension_status_ok);
    CHECK(r.score == static_cast<std::int32_t>((13u * params.match_score) -
                                               (3u * params.mismatch_penalty)));
    CHECK(r.max_score == r.score);
    CHECK(r.query_start == 101u);
    CHECK(r.query_end == 101u + kLen);
    CHECK(r.target_start == 909u);
    CHECK(r.target_end == 909u + kLen);
    CHECK(r.cigar_offset == 0u);
    CHECK(r.cigar_length == 1u);
    CHECK(result.packed_cigar[0] == ((kLen << 4) | 0u));
}

TEST_CASE("DOC0354 Part 2 slice - Vulkan simple CIGAR emits one-base gaps",
          "[g3sa][vulkan][gpu][cigar]") {
    auto& runtime = gv::Runtime::instance();
    INFO("Vulkan runtime error: " << runtime.initialization_error());
    REQUIRE(ch::vulkan_chaining_supported());

    const std::vector<std::uint32_t> query_symbols{
        0, 1, 2, 3, 0,
        0, 1, 3, 0,
    };
    const std::vector<std::uint32_t> target_symbols{
        0, 1, 3, 0,
        0, 1, 2, 3, 0,
    };

    const std::vector<gv::GpuAlignmentCigarJobWire> jobs{
        gv::GpuAlignmentCigarJobWire{
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
        gv::GpuAlignmentCigarJobWire{
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
    const gv::GpuAlignmentCigarParams params{
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

    const gv::AlignmentCigarBatchResult result =
        runtime.run_alignment_simple_cigar_roundtrip(
            query_symbols, target_symbols, jobs, params,
            /*packed_cigar_capacity=*/6u);

    REQUIRE(result.results.size() == 2);
    REQUIRE(result.packed_cigar.size() == 6);
    const std::int32_t expected_score = static_cast<std::int32_t>(
        (4u * params.match_score) -
        (params.gap_open_short + params.gap_extend_short));

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

TEST_CASE("DOC0354 Part 2 slice - Vulkan bounded DP CIGAR emits multiple gaps",
          "[g3sa][vulkan][gpu][cigar]") {
    auto& runtime = gv::Runtime::instance();
    INFO("Vulkan runtime error: " << runtime.initialization_error());
    REQUIRE(ch::vulkan_chaining_supported());

    const std::vector<std::uint32_t> query_symbols{0, 1, 2, 3, 0, 1, 2};
    const std::vector<std::uint32_t> target_symbols{0, 1, 3, 0, 2, 1, 2};

    const std::vector<gv::GpuAlignmentCigarJobWire> jobs{
        gv::GpuAlignmentCigarJobWire{
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
    const gv::GpuAlignmentCigarParams params{
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

    const gv::AlignmentCigarBatchResult result =
        runtime.run_alignment_simple_cigar_roundtrip(
            query_symbols, target_symbols, jobs, params,
            /*packed_cigar_capacity=*/5u);

    REQUIRE(result.results.size() == 1);
    REQUIRE(result.packed_cigar.size() == 5);
    const gv::GpuAlignmentCigarResultWire& r = result.results.front();
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

TEST_CASE("DOC0354 Part 2 slice - Vulkan long greedy CIGAR emits three gaps",
          "[g3sa][vulkan][gpu][cigar]") {
    auto& runtime = gv::Runtime::instance();
    INFO("Vulkan runtime error: " << runtime.initialization_error());
    REQUIRE(ch::vulkan_chaining_supported());

    constexpr std::uint32_t kTargetLen = 620u;
    std::vector<std::uint32_t> target_symbols(kTargetLen);
    std::uint32_t state = 0x12345678u;
    for (std::uint32_t i = 0; i < kTargetLen; ++i) {
        state = state * 1664525u + 1013904223u;
        target_symbols[i] = (state >> 30u) & 3u;
    }
    target_symbols[200] = (target_symbols[208] + 1u) & 3u;

    std::vector<std::uint32_t> query_symbols;
    query_symbols.reserve(kTargetLen);
    query_symbols.insert(
        query_symbols.end(), target_symbols.begin(), target_symbols.begin() + 200);
    query_symbols.insert(
        query_symbols.end(), target_symbols.begin() + 208, target_symbols.begin() + 400);
    const std::uint32_t inserted_symbol = (target_symbols[400] + 1u) & 3u;
    for (std::uint32_t i = 0; i < 16u; ++i) {
        query_symbols.push_back(inserted_symbol);
    }
    query_symbols.insert(
        query_symbols.end(), target_symbols.begin() + 400, target_symbols.begin() + 612);
    REQUIRE(query_symbols.size() == kTargetLen);

    const std::vector<gv::GpuAlignmentCigarJobWire> jobs{
        gv::GpuAlignmentCigarJobWire{
            .query_symbol_offset = 0,
            .target_symbol_offset = 0,
            .query_len = static_cast<std::uint32_t>(query_symbols.size()),
            .target_len = kTargetLen,
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
    const gv::GpuAlignmentCigarParams params{
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

    const gv::AlignmentCigarBatchResult result =
        runtime.run_alignment_simple_cigar_roundtrip(
            query_symbols, target_symbols, jobs, params,
            /*packed_cigar_capacity=*/6u);

    REQUIRE(result.results.size() == 1);
    REQUIRE(result.packed_cigar.size() == 6);
    const gv::GpuAlignmentCigarResultWire& r = result.results.front();
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
