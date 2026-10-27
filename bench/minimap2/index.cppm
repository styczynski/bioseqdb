/**
 * @file bench/minimap2/index.cppm
 * @brief Black-box C++26 wrapper around the original minimap2 C pipeline.
 *
 * Exposes conversion and execution entry points that return the same PAF-like
 * result shape as the native pipeline, allowing both implementations to run
 * on identical decoded inputs without adding conversion work to measurements.
 *
 * The wrapper drives the minimap2 C API via its in-memory entry points
 * (mm_idx_str -> mm_map), so there is no file I/O, no argv parsing, no
 * thread pool — the original minimap2 behaviour is preserved single-
 * threaded. Bundled native sources remain isolated behind this adapter's
 * ordinary C API consumption boundary.
 */
module;

#include <hwy/highway.h>

extern "C" {
#include "minimap2/minimap.h"
}

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

export module bioseq.bench.minimap2;

import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.cigar;
import bioseq.pipeline.minimap_port;
import bioseq.instrumentation;
// The explicit import retains the C trampoline definitions in the module graph.
import :instr_shim;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::bench::HWY_NAMESPACE {

using bioseq::simd::match_result;
using bioseq::simd::match_result_collection;
using bioseq::simd::minimap2_parameters;

/**
 * @brief Converts a compact sequence to an ACGT string.
 *
 * The logical-rank iterator excludes padding, and conversion remains caller-owned
 * so measured minimap2 execution contains no compact-sequence decoding.
 *
 * @tparam p_range_type Compact-sequence configuration or backing range.
 * @param[in] sequence Packed input sequence.
 * @return One uppercase character per logical base.
 */
template<supports_compact_sequence_range p_range_type>
[[nodiscard]] std::string compact_sequence_to_acgt(
    const compact_sequence_range<p_range_type>& sequence)
{
    using alphabet_type = typename compact_sequence_range<
        p_range_type>::traits::alphabet_type;
    using rank_type = seq::alphabet_rank_t<alphabet_type>;
    std::string acgt_chars;
    acgt_chars.reserve(sequence.bp_size());
    alphabet_type letter{};
    for (const auto base_pair_rank : sequence.template iterable<logical_rank>()) {
        letter.assign_rank(static_cast<rank_type>(base_pair_rank));
        acgt_chars.push_back(letter.to_char());
    }
    return acgt_chars;
}

namespace detail {

/**
 * @brief Projects shared alignment parameters into minimap2 option structures.
 *
 * Only fields with direct counterparts in the minimap2 C API are copied.
 * The native RMQ flag follows the shared chaining selector; other bioseq-only
 * backend selectors do not alter the reference run.
 *
 * @param[in,out] index_options Native index options.
 * @param[in,out] mapping_options Native mapping options.
 * @param[in] parameters Shared benchmark parameters.
 * @throws std::invalid_argument When native option constraints are violated.
 */
inline void apply_parameters(
    mm_idxopt_t& index_options,
    mm_mapopt_t& mapping_options,
    const minimap2_parameters& parameters)
{
    if (parameters.kmer_size < 1 || parameters.kmer_size > 28
        || parameters.window_size < 1 || parameters.window_size > 255) [[unlikely]] {
        throw std::invalid_argument(
            "tool_minimap2 requires k-mer size in [1, 28] and window "
            "size in [1, 255]");
    }

    index_options.k = static_cast<short>(parameters.kmer_size);
    index_options.w = static_cast<short>(parameters.window_size);

    mapping_options.max_gap = parameters.max_gap;
    if (parameters.max_gap_ref >= 0) {
        mapping_options.max_gap_ref = parameters.max_gap_ref;
    }
    mapping_options.bw = parameters.bandwidth;
    if (parameters.bandwidth_long > 0) {
        mapping_options.bw_long = parameters.bandwidth_long;
        mapping_options.flag &= ~MM_F_NO_LJOIN;
    } else {
        mapping_options.bw_long = mapping_options.bw;
        mapping_options.flag |= MM_F_NO_LJOIN;
    }
    mapping_options.max_chain_skip = parameters.max_chain_skip;
    mapping_options.max_chain_iter = parameters.max_chain_iter;
    mapping_options.min_cnt = parameters.min_seed_count;
    mapping_options.min_chain_score = parameters.min_chain_score;
    mapping_options.min_ksw_len = parameters.min_ksw_len;
    mapping_options.chain_gap_scale = parameters.chain_gap_scale;
    mapping_options.chain_skip_scale = parameters.chain_skip_scale;
    mapping_options.mask_level = parameters.mask_level;
    mapping_options.mask_len = parameters.mask_len;
    mapping_options.pri_ratio = parameters.pri_ratio;
    mapping_options.best_n = parameters.best_n;
    mapping_options.a = parameters.match_score;
    mapping_options.b = parameters.mismatch_pen;
    mapping_options.q = parameters.gap_open1;
    mapping_options.e = parameters.gap_ext1;
    mapping_options.q2 = parameters.gap_open2;
    mapping_options.e2 = parameters.gap_ext2;
    mapping_options.sc_ambi = parameters.sc_ambi;
    // The shared contract exposes forward Z-drop only; inverse Z-drop retains
    // the native minimap2 default established by mm_set_opt.
    mapping_options.zdrop = parameters.zdrop;
    mapping_options.end_bonus = parameters.end_bonus;
    const auto minimum_dp_score =
        static_cast<std::int64_t>(parameters.min_chain_score)
        * static_cast<std::int64_t>(parameters.match_score);
    if (minimum_dp_score < (std::numeric_limits<int>::min)()
        || minimum_dp_score > (std::numeric_limits<int>::max)()) [[unlikely]] {
        throw std::invalid_argument(
            "tool_minimap2 minimum DP score exceeds the native integer domain");
    }
    mapping_options.min_dp_max = static_cast<int>(minimum_dp_score);
    if (parameters.mid_occ > 0) {
        mapping_options.mid_occ = parameters.mid_occ;
    }
    if (parameters.max_occ > 0) {
        mapping_options.max_occ = parameters.max_occ;
    }
    if (parameters.do_cigar || parameters.compute_edit_distance) {
        mapping_options.flag |= MM_F_CIGAR;
    }
    if (parameters.chaining_algorithm
            == bioseq::simd::chaining_algorithm::range_minimum_query) {
        mapping_options.flag |= MM_F_RMQ;
    }
}

/**
 * @brief Copies one native region and its CIGAR into a shared result.
 *
 * Field mapping mirrors region_to_match_result() in minimap_port.cppm so
 * results from both pipelines live in the same struct shape and an
 * evaluator can compare field-by-field.
 *
 * Semantic differences relevant to cross-pipeline comparison:
 *   * chain_score = score0 ?: score — minimap2 overwrites `score` with
 *     the final DP score after merging, keeping the pre-merge value in
 *     `score0`. The bioseq pipeline keeps its chaining DP score under `chain_score`
 *     straight from the chain builder. For chains that never merged,
 *     score0 == 0 and the two agree.
 *   * dp_score / dp_max_score — minimap2 stores these inside
 *     `mm_extra_t`, which is only populated when CIGAR is computed. When
 *     CIGAR is disabled we fall back to the chaining `score` so the
 *     field is defined but not apples-to-apples.
 *   * is_primary — minimap2 encodes primary mappings as `parent == id`
 *     (see hit.c mm_set_parent). The bioseq pipeline sets is_primary in
 *     classify_primary_secondary; both codepaths should agree but if
 *     they diverge silently the evaluator's primary-vs-secondary
 *     classification will too.
 *
 * @param[in] native_region Minimap2 result record.
 * @param[in] query_length Query length in bases.
 * @param[in] target_length Target length in bases.
 * @param[in] parameters Shared result-projection policy.
 * @return Shared PAF-like result record.
 * @throws std::runtime_error When native CIGAR words violate the shared codec.
 */
[[nodiscard]] match_result region_to_match_result(
    const mm_reg1_t& native_region,
    std::int32_t query_length,
    std::int32_t target_length,
    const minimap2_parameters& parameters)
{
    match_result result;
    result.query_len = query_length;
    result.query_start = native_region.qs;
    result.query_end = native_region.qe;
    result.is_reverse = native_region.rev != 0;
    result.target_len = target_length;
    result.target_start = native_region.rs;
    result.target_end = native_region.re;
    result.num_matches = native_region.mlen;
    result.block_length = native_region.blen;
    result.mapq = parameters.compute_mapq
        ? static_cast<std::int32_t>(native_region.mapq)
        : 0;
    result.chain_score = native_region.score0 != 0
        ? native_region.score0
        : native_region.score;
    result.dp_score = native_region.p != nullptr
        ? native_region.p->dp_score
        : native_region.score;
    result.dp_max_score = native_region.p != nullptr
        ? native_region.p->dp_max0
        : native_region.score;
    result.seed_count = native_region.cnt;
    result.secondary_chain_score = native_region.subsc;
    result.divergence = parameters.compute_divergence
        ? native_region.div
        : -1.0f;
    result.is_primary = native_region.parent == native_region.id;
    result.has_cigar = native_region.p != nullptr
        && native_region.p->n_cigar > 0;
    if (parameters.compute_edit_distance && native_region.p != nullptr) {
        const auto native_edit_distance =
            static_cast<std::int64_t>(native_region.blen)
            - static_cast<std::int64_t>(native_region.mlen)
            + static_cast<std::int64_t>(native_region.p->n_ambi);
        result.edit_distance = native_edit_distance > 0
            ? static_cast<decltype(result.edit_distance)>(native_edit_distance)
            : 0;
    }
    if (result.has_cigar) {
        // Borrow native words and translate through the shared checked codec.
        const std::span<const std::uint32_t> packed_cigar{
            native_region.p->cigar, static_cast<std::size_t>(native_region.p->n_cigar)};
        if (!bioseq::decode_native_cigar(packed_cigar, result.cigar)) {
            throw std::runtime_error{"native minimap2 produced an invalid CIGAR"};
        }
    }
    return result;
}

/**
 * @brief Owns every native allocation produced by one minimap2 invocation.
 *
 * Scoping the lifetime inside a struct means any early return from the
 * wrapper still frees the native allocations. Also documents which
 * allocator owns each pointer: region extras and the region array use
 * `free`, the kalloc-arena-backed index uses `mm_idx_destroy`, and the
 * thread buffer uses `mm_tbuf_destroy`.
 */
struct native_minimap2_handles {
    mm_idx_t* index = nullptr;
    mm_tbuf_t* buffer = nullptr;
    mm_reg1_t* regions = nullptr;
    int region_count = 0;

    native_minimap2_handles() = default;
    native_minimap2_handles(const native_minimap2_handles&) = delete;
    native_minimap2_handles& operator=(const native_minimap2_handles&) = delete;

    ~native_minimap2_handles() noexcept {
        if (regions != nullptr) {
            for (int region_index = 0;
                 region_index < region_count;
                 ++region_index) {
                std::free(regions[region_index].p);
            }
        }
        std::free(regions);
        if (buffer != nullptr) {
            mm_tbuf_destroy(buffer);
        }
        if (index != nullptr) {
            mm_idx_destroy(index);
        }
    }
};

}  // namespace detail

/**
 * @brief Runs the native minimap2 C pipeline on pre-decoded sequences.
 *
 * Compact-sequence decoding is intentionally caller-owned so the measured
 * region contains only native index construction, mapping, and result
 * conversion. Both input strings remain alive and NUL-terminated for every
 * native call made by this function.
 *
 * @param[in] query_acgt ASCII query sequence.
 * @param[in] target_acgt ASCII target sequence.
 * @param[in] parameters Algorithm parameters shared with the bioseq pipeline.
 * @return PAF-like results in the shared collection representation.
 * @throws std::length_error When either sequence exceeds minimap2's signed
 * 32-bit native coordinate domain.
 * @throws std::invalid_argument When shared options cannot be represented by
 * the native minimap2 configuration.
 * @throws std::runtime_error When minimap2 returns an inconsistent region array.
 */
[[nodiscard]] inline match_result_collection tool_minimap2(
    const std::string& query_acgt,
    const std::string& target_acgt,
    const minimap2_parameters& parameters = minimap2_parameters{})
{
    constexpr auto maximum_native_length = static_cast<std::size_t>(
        (std::numeric_limits<std::int32_t>::max)());
    if (query_acgt.size() > maximum_native_length
        || target_acgt.size() > maximum_native_length) [[unlikely]] {
        throw std::length_error(
            "tool_minimap2 input exceeds the native signed-32-bit "
            "coordinate domain");
    }

    // Native call-site markers cover minimap2 internals; this enclosing marker
    // also accounts for wrapper-owned handle setup and result conversion.
    static const auto* run_descriptor =
        bioseq::instrumentation::register_static_region("tool_minimap2.run");
    auto run_scope = bioseq::instrumentation::region(run_descriptor);

    const auto target_length = static_cast<std::int32_t>(target_acgt.size());
    const auto query_length = static_cast<std::int32_t>(query_acgt.size());

    if (query_length == 0 || target_length == 0) {
        bioseq_log<logging_level::debug>(
            "[tool_minimap2] Empty input (qlen={}, tlen={}) — no results",
            query_length,
            target_length);
        return match_result_collection{};
    }

    mm_idxopt_t index_options{};
    mm_mapopt_t mapping_options{};
    mm_set_opt(nullptr, &index_options, &mapping_options);
    detail::apply_parameters(index_options, mapping_options, parameters);

    detail::native_minimap2_handles handles;
    const char* target_sequence_address = target_acgt.c_str();
    const char* target_name = "target";
    // Keep index construction distinct from mapping while preserving nested
    // native minimizer-sketch markers.
    static const auto* index_descriptor =
        bioseq::instrumentation::register_static_region("tool_minimap2.target_index_build");
    {
        auto index_scope = bioseq::instrumentation::region(index_descriptor);
        handles.index = mm_idx_str(
            index_options.w,
            index_options.k,
            (index_options.flag & MM_I_HPC) != 0 ? 1 : 0,
            index_options.bucket_bits,
            1,
            &target_sequence_address,
            &target_name);
    }
    if (handles.index == nullptr) {
        bioseq_log<logging_level::debug>("[tool_minimap2] mm_idx_str returned nullptr");
        return match_result_collection{};
    }
    mm_mapopt_update(&mapping_options, handles.index);

    handles.buffer = mm_tbuf_init();
    // mm_map returns a malloc()'d array (`mm_gen_regs` uses `calloc`
    // for the regs block and each `mm_reg1_t::p` is malloc'd inside
    // hit.c); that's why we free each p with std::free and the array
    // itself with std::free below. kalloc-backed buffers inside the
    // pipeline are owned by `handles.buffer` and freed in its dtor.
    handles.regions = mm_map(
        handles.index,
        query_length,
        query_acgt.c_str(),
        &handles.region_count,
        handles.buffer,
        &mapping_options,
        "query");
    if (handles.region_count < 0
        || (handles.region_count > 0 && handles.regions == nullptr)) {
        throw std::runtime_error(
            "native minimap2 returned an invalid region array");
    }

    std::vector<match_result> results;
    results.reserve(static_cast<std::size_t>(handles.region_count));
    for (int region_index = 0;
         region_index < handles.region_count;
         ++region_index) {
        const mm_reg1_t& native_region = handles.regions[region_index];
        results.push_back(detail::region_to_match_result(
            native_region,
            query_length,
            target_length,
            parameters));
    }

    bioseq_log<logging_level::debug>(
        "[tool_minimap2] Produced {} alignments for qlen={} tlen={}",
        results.size(),
        query_length,
        target_length);
    return match_result_collection(std::move(results));
}

}  // namespace bioseq::bench::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}  // export
