/**
 * @file tool_minimap2.cppm
 * @brief Black-box C++26 wrapper around the original minimap2 C pipeline.
 *
 * Exposes a single entry point `tool_minimap2(query, target, params)` that
 * accepts our compact_sequence_range inputs and returns a
 * MatchResultCollection in the same shape as our native `minimap_port`, so
 * a benchmarker can run both on identical inputs and compare results.
 *
 * The wrapper drives the minimap2 C API via its in-memory entry points
 * (mm_idx_str -> mm_map), so there is no file I/O, no argv parsing, no
 * thread pool — the original minimap2 behaviour is preserved single-
 * threaded. The minimap2 sources themselves are unchanged on purpose:
 * this module sits on top of them like any other consumer.
 */
module;

#include <hwy/highway.h>

#include "common.h"

extern "C" {
#include "minimap2/minimap.h"
}

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

export module bench.tool_minimap2;

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_cigar;
import bioseq_sequences.minimap_port;
import bioseq_instr;
// Force the C-shim trampolines (extern "C" registration / begin / end)
// to be linked into this TU. Without an explicit import the build
// system's module-graph reachability check can drop the shim on hosts
// where the .c TUs are visited before the wrapper, leaving
// bench_instr_region_register undefined at link time.
import bench.tool_minimap2.instr_shim;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::bench::HWY_NAMESPACE {

using bioseq::simd::HWY_NAMESPACE::CigarOp;
using bioseq::simd::HWY_NAMESPACE::MatchResult;
using bioseq::simd::HWY_NAMESPACE::MatchResultCollection;
using bioseq::simd::HWY_NAMESPACE::Minimap2Params;

/**
 * @brief Convert a compact_sequence_range to an ACGT string (no NUL in the
 *        middle, `bp_size()` chars long).
 *
 * Iterates `iterable<BP>()` directly — that iterator respects padding so
 * we never emit junk past the sequence's logical end. We then reuse
 * `seqan3::alphabet::to_char` per rank for the final char mapping. This
 * is a single O(bp_size) pass with no intermediate `std::vector` (unlike
 * the `to_encoded_vector()` + loop pattern).
 *
 * Promoted out of `detail::` so bench harnesses can call it from
 * `on_input` and pass the resulting string into `tool_minimap2`. That
 * keeps the O(bp_size) decompression out of the per-iteration timing
 * window — see STRANGE_TIMING_COMPREHESIVE_REPORT.md §1b for context.
 */
template<supports_compact_sequence_range Range>
[[nodiscard]] std::string compact_sequence_to_acgt(
    const compact_sequence_range<Range>& sequence)
{
    using alphabet_type = typename compact_sequence_range<Range>::traits::alphabet_type;
    using rank_type     = seqan3::alphabet_rank_t<alphabet_type>;
    std::string acgt_chars;
    acgt_chars.reserve(sequence.bp_size());
    alphabet_type letter{};
    for (const auto& base_pair_rank : sequence.template iterable<BP>()) {
        letter.assign_rank(static_cast<rank_type>(base_pair_rank));
        acgt_chars.push_back(letter.to_char());
    }
    return acgt_chars;
}

namespace detail {

/**
 * @brief Populate an mm_mapopt_t from our Minimap2Params.
 *
 * Only the subset of fields that have direct counterparts in the minimap2
 * C API is copied. Chaining-algorithm / base-alignment-algorithm selectors
 * are bioseq-internal and have no effect on the reference minimap2 run.
 */
inline void apply_params(mm_idxopt_t& idx_opt, mm_mapopt_t& map_opt,
                         const Minimap2Params& params)
{
    idx_opt.k = static_cast<short>(params.kmer_size);
    idx_opt.w = static_cast<short>(params.window_size);

    map_opt.max_gap          = params.max_gap;
    if (params.max_gap_ref >= 0) map_opt.max_gap_ref = params.max_gap_ref;
    map_opt.bw               = params.bandwidth;
    if (params.bandwidth_long > 0) map_opt.bw_long = params.bandwidth_long;
    map_opt.max_chain_skip   = params.max_chain_skip;
    map_opt.max_chain_iter   = params.max_chain_iter;
    map_opt.min_cnt          = params.min_seed_count;
    map_opt.min_chain_score  = params.min_chain_score;
    map_opt.chain_gap_scale  = params.chain_gap_scale;
    map_opt.chain_skip_scale = params.chain_skip_scale;
    map_opt.mask_level       = params.mask_level;
    map_opt.mask_len         = params.mask_len;
    map_opt.pri_ratio        = params.pri_ratio;
    map_opt.best_n           = params.best_n;
    map_opt.a                = params.match_score;
    map_opt.b                = params.mismatch_pen;
    map_opt.q                = params.gap_open1;
    map_opt.e                = params.gap_ext1;
    map_opt.q2               = params.gap_open2;
    map_opt.e2               = params.gap_ext2;
    map_opt.sc_ambi          = params.sc_ambi;
    map_opt.zdrop            = params.zdrop;
    map_opt.end_bonus        = params.end_bonus;
    if (params.mid_occ > 0) map_opt.mid_occ = params.mid_occ;
    if (params.max_occ > 0) map_opt.max_occ = params.max_occ;
    if (params.do_cigar) {
        map_opt.flag |= MM_F_CIGAR;
    }
}

/**
 * @brief Copy a single mm_reg1_t (and its CIGAR) into a MatchResult.
 *
 * Field mapping mirrors region_to_match_result() in minimap_port.cppm so
 * results from both pipelines live in the same struct shape and an
 * evaluator can compare field-by-field.
 *
 * Fields with semantic drift between the two sources (documented so a
 * future reader does not assume 1:1 equivalence):
 *   * chain_score = score0 ?: score — minimap2 overwrites `score` with
 *     the final DP score after merging, keeping the pre-merge value in
 *     `score0`. Our port keeps the chaining DP score under `chain_score`
 *     straight from the chain builder. For chains that never merged,
 *     score0 == 0 and the two agree.
 *   * dp_score / dp_max_score — minimap2 stores these inside
 *     `mm_extra_t`, which is only populated when CIGAR is computed. When
 *     CIGAR is disabled we fall back to the chaining `score` so the
 *     field is defined but not apples-to-apples.
 *   * is_primary — minimap2 encodes primary mappings as `parent == id`
 *     (see hit.c mm_set_parent). Our port sets is_primary explicitly in
 *     classify_primary_secondary; both codepaths should agree but if
 *     they diverge silently the evaluator's primary-vs-secondary
 *     classification will too.
 */
[[nodiscard]] MatchResult reg_to_match_result(
    const mm_reg1_t& native_region, int32_t query_len, int32_t target_len)
{
    MatchResult mr;
    mr.query_len              = query_len;
    mr.query_start            = native_region.qs;
    mr.query_end              = native_region.qe;
    mr.is_reverse             = native_region.rev != 0;
    mr.target_len             = target_len;
    mr.target_start           = native_region.rs;
    mr.target_end             = native_region.re;
    mr.num_matches            = native_region.mlen;
    mr.block_length           = native_region.blen;
    mr.mapq                   = static_cast<int32_t>(native_region.mapq);
    mr.chain_score            = native_region.score0 != 0 ? native_region.score0
                                                           : native_region.score;
    mr.dp_score               = native_region.p ? native_region.p->dp_score : native_region.score;
    mr.dp_max_score           = native_region.p ? native_region.p->dp_max   : native_region.score;
    mr.seed_count             = native_region.cnt;
    mr.secondary_chain_score  = native_region.subsc;
    mr.divergence             = native_region.div;
    mr.is_primary             = (native_region.parent == native_region.id);
    mr.has_cigar              = (native_region.p != nullptr && native_region.p->n_cigar > 0);
    if (mr.has_cigar) {
        // minimap2 packs CIGAR as (length << 4) | opcode, same wire
        // format as bioseq_cigar::append_cigar expects, so we reuse the
        // shared helper instead of re-deriving the mask here.
        const std::vector<uint32_t> packed_cigar(
            native_region.p->cigar,
            native_region.p->cigar + native_region.p->n_cigar);
        bioseq::simd::HWY_NAMESPACE::append_cigar(mr.cigar, packed_cigar);
    }
    return mr;
}

/**
 * @brief RAII guard that pairs `mm_idx_t*` / `mm_tbuf_t*` handles with
 *        their correct minimap2 destructors.
 *
 * Scoping the lifetime inside a struct means any early return from the
 * wrapper still frees the native allocations. Also documents which
 * allocator owns the pointer — kalloc-arena-backed mm_idx_t is freed
 * with mm_idx_destroy; the mm_tbuf_t pairs with mm_tbuf_destroy.
 */
struct NativeMinimap2Handles {
    mm_idx_t*  index  = nullptr;
    mm_tbuf_t* buffer = nullptr;

    NativeMinimap2Handles() = default;
    NativeMinimap2Handles(const NativeMinimap2Handles&) = delete;
    NativeMinimap2Handles& operator=(const NativeMinimap2Handles&) = delete;

    ~NativeMinimap2Handles() {
        if (buffer != nullptr) mm_tbuf_destroy(buffer);
        if (index  != nullptr) mm_idx_destroy(index);
    }
};

}  // namespace detail

/**
 * @brief Run the original minimap2 C pipeline and return a MatchResultCollection.
 *
 * Takes pre-decoded ACGT strings (one per sequence) and calls the
 * minimap2 in-memory entry points (`mm_idx_str` -> `mm_map`). Mirrors
 * the shape of `minimap_port` for downstream evaluation, but the
 * compact-to-ACGT conversion is the *caller's* responsibility — see
 * `compact_sequence_to_acgt(...)` above. Hoisting the conversion
 * out of `on_run` keeps the per-iteration timing window honest
 * (issue 1b in STRANGE_TIMING_COMPREHESIVE_REPORT.md). The
 * cross-alphabet sanity check that this function used to perform via
 * `static_assert` is preserved at the conversion site (the templated
 * `compact_sequence_to_acgt` above is the only entry that produces
 * the strings consumed here).
 *
 * @param query_acgt   ASCII query sequence (NUL-terminated; we pass
 *                     `c_str()` straight into `mm_map`).
 * @param target_acgt  ASCII target sequence, same NUL-termination
 *                     contract.
 * @param params       Algorithm parameters (defaults match minimap2)
 * @return             PAF-like results in the shared MatchResultCollection shape
 */
[[nodiscard]] inline MatchResultCollection tool_minimap2(
    const std::string& query_acgt,
    const std::string& target_acgt,
    const Minimap2Params& params = Minimap2Params{})
{
    // Top-level region for the reference-minimap2 wrapper. Inner regions
    // come from BENCH_INSTR_BLOCK call sites inside minimap2/*.c; this
    // outer region lets the report layer attribute the wrapper-side
    // overhead (mm_idx_str index build, mm_tbuf_init, result copy) that
    // minimap2's own markers don't see. The compact-to-ACGT conversion
    // used to live here and dominated the wrapper time on XXL inputs;
    // it's now the caller's responsibility.
    static const auto* _tool_minimap2_run_desc =
        bioseq::instr::register_region_static("tool_minimap2.run");
    auto _tool_minimap2_run_scope = bioseq::instr::region(_tool_minimap2_run_desc);

    const int32_t target_len = static_cast<int32_t>(target_acgt.size());
    const int32_t query_len  = static_cast<int32_t>(query_acgt.size());

    if (query_len == 0 || target_len == 0) {
        JustLog("[tool_minimap2] Empty input (qlen={}, tlen={}) — no results",
                query_len, target_len);
        return MatchResultCollection{};
    }

    mm_idxopt_t idx_opt{};
    mm_mapopt_t map_opt{};
    mm_set_opt(nullptr, &idx_opt, &map_opt);
    detail::apply_params(idx_opt, map_opt, params);

    detail::NativeMinimap2Handles handles;
    const char* target_ptr  = target_acgt.c_str();
    const char* target_name = "target";
    // Wrap mm_idx_str inside its own region so the bench's per-phase
    // accounting can attribute the index-build cost on the minimap2 side
    // — without this, target index construction inside mm_idx_str (which
    // hashes every k-mer of the target and bins them into the minimizer
    // index) was hidden inside `tool_minimap2.run` and showed up as
    // "wrapper overhead" only. Mirrors `target_index_build` in
    // `minimap_port.cppm` so the chart layer can pair them up cell for
    // cell. The `mm_sketch` calls inside `mm_idx_str` register their
    // own region (`tool_minimap2.mm_sketch`) and nest under this one.
    static const auto* _tool_minimap2_idx_desc =
        bioseq::instr::register_region_static("tool_minimap2.target_index_build");
    {
        auto _idx_scope = bioseq::instr::region(_tool_minimap2_idx_desc);
        handles.index = mm_idx_str(
            idx_opt.w, idx_opt.k,
            (idx_opt.flag & MM_I_HPC) ? 1 : 0,
            idx_opt.bucket_bits,
            1,
            &target_ptr,
            &target_name);
    }
    if (handles.index == nullptr) {
        JustLog("[tool_minimap2] mm_idx_str returned nullptr");
        return MatchResultCollection{};
    }
    mm_mapopt_update(&map_opt, handles.index);

    handles.buffer = mm_tbuf_init();
    int native_region_count = 0;
    // mm_map returns a malloc()'d array (`mm_gen_regs` uses `calloc`
    // for the regs block and each `mm_reg1_t::p` is malloc'd inside
    // hit.c); that's why we free each p with std::free and the array
    // itself with std::free below. kalloc-backed buffers inside the
    // pipeline are owned by `handles.buffer` and freed in its dtor.
    mm_reg1_t* native_regions = mm_map(
        handles.index,
        query_len,
        query_acgt.c_str(),
        &native_region_count,
        handles.buffer,
        &map_opt,
        "query");

    std::vector<MatchResult> results;
    results.reserve(static_cast<size_t>(native_region_count));
    for (int region_index = 0; region_index < native_region_count; ++region_index) {
        const mm_reg1_t& native_region = native_regions[region_index];
        results.push_back(detail::reg_to_match_result(native_region, query_len, target_len));
        std::free(native_region.p);
    }
    std::free(native_regions);

    JustLog("[tool_minimap2] Produced {} alignments for qlen={} tlen={}",
            results.size(), query_len, target_len);
    return MatchResultCollection(std::move(results));
}

}  // namespace bioseq::bench::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}  // export
