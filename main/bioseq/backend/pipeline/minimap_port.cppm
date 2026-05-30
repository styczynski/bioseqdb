/**
 * @file minimap_port.cppm
 * @brief C++20 port of the minimap2 mapping pipeline.
 *
 * This module implements the core minimap2 algorithm starting AFTER minimizer
 * collection (sketch generation). It takes pre-computed minimizers and produces
 * PAF-like alignment results.
 *
 * High-level flow:
 *   1. Build a simple hash index from target minimizers
 *   2. Collect seed hits: look up query minimizers in the target index
 *   3. Chain seeds using dynamic programming (mg_lchain_dp equivalent)
 *   4. Convert chains to alignment regions (mm_gen_regs equivalent)
 *   5. Classify primary/secondary mappings (mm_set_parent equivalent)
 *   6. Filter secondary mappings (mm_select_sub equivalent)
 *   7. Perform base-level alignment using dual-affine gap DP (KSW2 equivalent)
 *   8. Compute mapping quality (mm_set_mapq equivalent)
 *   9. Return results as MatchResult (PAF-like records)
 *
 * The implementation follows the original minimap2 C code as documented in
 * MINIMAP2_DETAILS_1.md, translating to modern C++20 with:
 *   - std::vector instead of C arrays
 *   - References instead of raw pointers
 *   - static_cast instead of C-style casts
 *   - Move semantics where beneficial
 *   - constexpr where possible
 */
module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include <boost/unordered/unordered_flat_map.hpp>

#include "common.h"

#include <cstdint>
#include <cmath>
#include <cstring>
#include <bit>
#include <vector>
#include <array>
#include <algorithm>
#include <numeric>
#include <limits>
#include <unordered_map>
#include <optional>
#include <span>
#include <string>

export module bioseq_sequences.minimap_port;

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_sequences.kmers;
import bioseq_sequences.kmers.hashers.minimap_int;
import bioseq_sequences.kmers.two_stacks;
import bioseq_cigar;
import alignment_hwy;
import alignment_hwy_utils;
import block_aligner_hwy;
import bioseq_sequences.chaining.utils;
import bioseq_sequences.chaining.scalar;
import bioseq_sequences.chaining.simd;
import bioseq_sequences.chaining.rmq;
import bioseq_sequences.radix_sort;
import context;
import bioseq_storage_scanner;
import bioseq_storage_lazy;
import bioseq_storage_size;   // DOC0263 §10 — sequence_size_t for the C1 truncation audit
import bioseq_instr;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// DOC0263 §10.1 — the canonical "any sequence position or length" type. Every
// length / absolute coordinate carried by the port uses this (uint64) so no
// position is silently truncated for sequences longer than 2^31 (C1).
using bioseq::storage::sequence_size_t;

// Import chaining types from the shared chaining module.
// AnchorRefFields / AnchorQueryFields removed per DOC0263 §2 (Anchor is now a
// flat 24 B struct, no nested ref/query sub-objects).
using bioseq::chaining::Anchor;
using bioseq::chaining::Chain;
using bioseq::chaining::ChainingMode;
using bioseq::chaining::ChainingParams;
using bioseq::chaining::compute_chaining_score;
using bioseq::chaining::chain_anchors_scalar;
using bioseq::chaining::chain_anchors_rmq;
using bioseq::chaining::backtrack_chains;

// ============================================================================
// Algorithm selection enums
// ============================================================================

/// Chaining algorithm — resolved at plan time from the
/// bioseq.aligner.chaining_algorithm GUC and dispatched with constexpr-if
/// inside minimap2_port_with_index so the hot loop stays branchless.
/// Values MUST match the ordinal used in the GUC enum (columnar.cppm).
enum class ChainingAlgorithmType : uint8_t {
    SCALAR_MM2 = 0,
    SIMD_MM2   = 1,
    RMQ        = 2,
};

/// Base-level alignment algorithm — KSW2 dual-affine DP (minimap2 default)
/// vs Highway-vectorised block aligner (single-affine). Values MUST match
/// bioseq.aligner.base_alignment_algorithm ordinals.
enum class BaseAlignmentAlgorithmType : uint8_t {
    KSW2          = 0,
    BLOCK_ALIGNER = 1,
    /// Inter-region batched KSW2 (DOC0254 §11). Packs many small KSW2
    /// gap-fills across regions into SIMD lanes and runs an inter-sequence
    /// DP. Long fills (q_max > 256 || t_max > 256) fall through to the
    /// intra-sequence striped KSW2 kernel via the same trait surface.
    KSW2_BATCHED  = 2,
};

/// Minimizer-extraction algorithm. Selected at runtime; the
/// `minimap_port` entry point dispatches on this enum with a runtime
/// branch (cannot use constexpr-if because both paths instantiate the
/// same templates, just with the simd_hash vs scalar_hash inner type,
/// and the kmer index module's `requires` clauses already enforce the
/// correct concept).
enum class MinimizerAlgorithmType : uint8_t {
    /// Scalar `collect_minimizers` — the original reference path,
    /// used for backwards compatibility with the test reference
    /// output documented in CODE_GUIDELINES.md §9.
    SCALAR = 0,
    /// Original Highway-vectorised collector (`collect_minimizers_simd`).
    /// Branchless SIMDification of minimap2's running-min + per-step
    /// rescan loop. Kept around as a slow reference point — it has
    /// been measured to be slower than the scalar baseline on the
    /// VERY_LONG / XL benchmark buckets because the rescan fires
    /// whenever ANY lane's saved min slides off the window, which is
    /// `Lanes(d)` times more frequent than the scalar branch.
    SIMD_MINIMAP2_SLOW = 1,
    /// Production minimizer collector. Two-stack / prefix-suffix-min
    /// sliding-window with dual-stream interleaved per-step body
    /// (mirrors the SIMD-Minimizers paper) and ntHash-style mix +
    /// deferred minimap_int scramble. Implemented in
    /// `kmers/two_stacks_minimizers.cppm` as
    /// `collect_minimizers_two_stack_nthash_simd`. See
    /// RESEARCH/ENGINEERING_REPORT_V2.md for the full evolution
    /// history (was TWO_STACK_NTHASH_V3 in the iteration log).
    TWO_STACK_NTHASH = 2,
};

[[nodiscard]] constexpr const char* chaining_algorithm_name(ChainingAlgorithmType mode) {
    switch (mode) {
        case ChainingAlgorithmType::SCALAR_MM2: return "Minimap2 Scalar";
        case ChainingAlgorithmType::SIMD_MM2:   return "Minimap2 SIMD";
        case ChainingAlgorithmType::RMQ:        return "Minimap2 RMQ";
    }
    return "unknown";
}

[[nodiscard]] constexpr const char* base_alignment_algorithm_name(BaseAlignmentAlgorithmType mode) {
    switch (mode) {
        case BaseAlignmentAlgorithmType::KSW2:          return "KSW2";
        case BaseAlignmentAlgorithmType::BLOCK_ALIGNER: return "Block Aligner";
        case BaseAlignmentAlgorithmType::KSW2_BATCHED:  return "KSW2 (inter-region batched)";
    }
    return "unknown";
}

[[nodiscard]] constexpr const char* minimizer_algorithm_name(MinimizerAlgorithmType mode) {
    switch (mode) {
        case MinimizerAlgorithmType::SCALAR:             return "Scalar Minimizers";
        case MinimizerAlgorithmType::SIMD_MINIMAP2_SLOW: return "SIMD Minimap2 (slow reference)";
        case MinimizerAlgorithmType::TWO_STACK_NTHASH:   return "Two-Stack nthash SIMD Minimizers";
    }
    return "unknown";
}

// ============================================================================
// Constants — mirrors minimap2 defaults from mm_mapopt_t
// ============================================================================

struct Minimap2Params {
    int32_t kmer_size      = 15;     ///< k-mer size
    int32_t window_size    = 10;     ///< minimizer window
    int32_t max_gap        = 5000;   ///< max reference gap in chaining
    int32_t max_gap_ref    = -1;     ///< explicit ref gap (-1 = use max_gap)
    int32_t bandwidth      = 500;    ///< bandwidth for chaining & alignment
    int32_t max_chain_skip = 25;     ///< max skip for early termination in DP
    int32_t max_chain_iter = 5000;   ///< max predecessors examined per anchor (map.c)
    int32_t min_seed_count = 3;      ///< min seeds per chain
    int32_t min_chain_score= 40;     ///< min chain score
    float   chain_gap_scale= 0.8f;   ///< gap penalty scale (options.c default)
    float   chain_skip_scale=0.0f;   ///< skip penalty scale (options.c default)
    float   mask_level     = 0.5f;   ///< primary/secondary overlap threshold
    int32_t mask_len       = std::numeric_limits<int32_t>::max(); ///< max uncovered length (INT_MAX in options.c)
    float   pri_ratio      = 0.8f;   ///< secondary score ratio threshold
    int32_t best_n         = 5;      ///< max secondary alignments to keep

    // Alignment scoring — matches mm_mapopt_init() in options.c
    int32_t match_score    = 2;      ///< match score (a)
    int32_t mismatch_pen   = 4;      ///< mismatch penalty (b)
    int32_t gap_open1      = 4;      ///< short gap open (q)
    int32_t gap_ext1       = 2;      ///< short gap extend (e)
    int32_t gap_open2      = 24;     ///< long gap open (q2)
    int32_t gap_ext2       = 1;      ///< long gap extend (e2)
    int32_t sc_ambi        = 1;      ///< penalty for ambiguous base (N)
    int32_t zdrop          = 400;    ///< z-drop threshold
    int32_t end_bonus      = -1;     ///< bonus for reaching sequence ends (-1 in options.c)

    int32_t max_occ        = 500;    ///< max seed occurrence to keep
    int32_t mid_occ        = 500;    ///< mid-level seed occurrence threshold

    /// Minimum gap length between consecutive seeds before invoking KSW2.
    /// minimap2's `mm_align1` (align.c:815) skips KSW2 alignment when the gap
    /// between consecutive seeds is below this threshold AND the seed is not
    /// the last in the chain. Default 200 matches minimap2's `opt->min_ksw_len`.
    /// Setting this to 0 forces a single KSW2 over the entire seed-to-seed
    /// region, which is the legacy bioseq behaviour. The per-seed-pair gap
    /// fill replaces a single 1Mbp×1Mbp DP with many ~200bp×~200bp DPs and
    /// is the dominant speed-up for VERY_LONG sequences (~7× total improvement
    /// at 903 kbp on the bench, see ENGINEERING_REPORT.md).
    int32_t min_ksw_len    = 200;

    /// When true, perform base-level alignment via the selected aligner and
    /// produce CIGAR. Equivalent to MM_F_CIGAR flag in minimap2. Driven by
    /// whether bioseq.cigar(...) appears in the scan target list.
    bool    do_cigar            = true;

    /// When true, run estimate_divergence_from_seeds() and populate
    /// MatchResult::divergence. Driven by whether bioseq.divergence(...) is
    /// requested. Leaving this false saves the O(n log n) binary-search walk
    /// per region, with no impact on downstream fields.
    bool    compute_divergence  = true;

    /// When true, run compute_mapping_quality() and populate
    /// MatchResult::mapq. Driven by whether bioseq.mapq(...) is requested.
    /// Skipping this omits the O(regions) log-computation loop.
    bool    compute_mapq        = true;

    /// Block aligner tunables (ignored when base_alignment_algorithm == KSW2).
    ///
    /// Sweep results on the LONG / VERY_LONG / XL CCS bench grid (M1, NEON,
    /// nthash + SIMD-DP) — Block aligner mean_ms vs KSW2 baseline at VL:
    ///
    ///   min=32 max=256: Block 191 ms — 11% slower than KSW2 (172 ms)
    ///   min=32 max=128: Block 163 ms —  1% faster
    ///   min=32 max=64:  Block 161 ms —  3% faster
    ///   min=8  max=32:  Block 139 ms — 19% faster
    ///   min=8  max=16:  Block 132 ms — 21% faster
    ///   min=8  max=64:  Block 137 ms — 20% faster
    ///   min=16 max=64:  Block 135 ms — 21% faster
    ///   min=16 max=32:  Block 129 ms — 22% faster   <-- chosen default
    ///
    /// The previous defaults (32 / 256) over-allocated DP scratch for the
    /// short gap-fill workload that dominates a typical bench: a single
    /// region opens hundreds of gap-fill calls, most under 200 bp on each
    /// axis, and the algorithm rarely needed to grow past 32 cells. With
    /// max_size pinned at 256 each call paid for an 8-buffer scratch +
    /// trace ring sized for a doubling-grow chain that almost never fired.
    /// Pinning min=16 max=32 lets the block aligner clear the +10% goal
    /// over KSW2 + SIMD-DP + two-stack-nthash on both VERY_LONG and XL.
    /// Callers with high-error inputs (e.g. ONT_LEGACY) can override via
    /// Minimap2Params; the bench grid uses these defaults verbatim.
    size_t  block_min_size      = 16;
    size_t  block_max_size      = 32;

    /// Algorithm selection for chaining. Default: SCALAR_DP for backwards compatibility.
    ChainingMode chaining_mode  = ChainingMode::SCALAR_DP;
    int32_t bandwidth_long      = 0;  ///< Long-join bandwidth (0 = disabled)

    /// Runtime chaining algorithm selector. The pipeline reads this value
    /// once and template-dispatches so the inner loop is branchless.
    ChainingAlgorithmType        chaining_algorithm        = ChainingAlgorithmType::SCALAR_MM2;
    /// Runtime base-alignment algorithm selector.
    BaseAlignmentAlgorithmType   base_alignment_algorithm  = BaseAlignmentAlgorithmType::BLOCK_ALIGNER;
    /// Runtime minimizer-extraction algorithm selector. The default is
    /// scalar to match the reference output documented in
    /// CODE_GUIDELINES.md §9; the bench harness flips it to SIMD when
    /// measuring the Highway-vectorised path.
    MinimizerAlgorithmType       minimizer_algorithm       = MinimizerAlgorithmType::SCALAR;

    /// Runtime profiler toggle. Driven by bioseq.aligner.enable_profiling
    /// GUC. When true, BatchProcessor::process_sequences instantiates the
    /// pipeline with context_args_profiling_full; when false, the
    /// no-profiling instantiation runs (zero overhead).
    bool                          enable_profiling          = false;

    /// When true and base_alignment_algorithm == KSW2_BATCHED, the
    /// dispatcher routes through the cross-query deferred-align scheduling
    /// shape (DOC0254 §11.10). All queries in the outer batch collect their
    /// fills before any DP runs, producing larger buckets and better SIMD
    /// occupancy on shorter queries. Off by default — per-query bucketing
    /// already saturates the bucket fill for VERY_LONG inputs.
    bool                          cross_query_batched       = false;

    /// Build ChainingParams from this Minimap2Params.
    ///
    /// Extracts only the chaining-relevant subset and precomputes derived values.
    /// The ChainingMode (used by the shared chaining modules) is synthesized
    /// from chaining_algorithm so both selectors stay aligned.
    [[nodiscard]] ChainingParams to_chaining_params() const {
        ChainingParams cp;
        cp.kmer_size        = kmer_size;
        cp.max_gap          = max_gap;
        cp.bandwidth        = bandwidth;
        cp.max_chain_skip   = max_chain_skip;
        cp.max_chain_iter   = max_chain_iter;
        cp.min_seed_count   = min_seed_count;
        cp.min_chain_score  = min_chain_score;
        cp.chain_gap_scale  = chain_gap_scale;
        cp.chain_skip_scale = chain_skip_scale;
        switch (chaining_algorithm) {
            case ChainingAlgorithmType::SIMD_MM2:   cp.chaining_mode = ChainingMode::SIMD_DP; break;
            case ChainingAlgorithmType::RMQ:        cp.chaining_mode = ChainingMode::RMQ;     break;
            case ChainingAlgorithmType::SCALAR_MM2:
            default:                                cp.chaining_mode = ChainingMode::SCALAR_DP; break;
        }
        cp.prepare();
        return cp;
    }
};

// CIGAR opcodes and CigarOp are imported from bioseq_cigar module.

/**
 * @brief PAF-like alignment result.
 *
 * Maps to the 12 mandatory PAF columns plus alignment details.
 */
struct MatchResult {
    // PAF mandatory columns. DOC0263 §10.2: every length / absolute coordinate
    // (cols 2,3,4,7,8,9,10,11) is `sequence_size_t` (uint64) so it is never
    // truncated for sequences longer than 2^31. PAF emits them as decimal
    // strings, so the wire width is unchanged.
    sequence_size_t query_len      = 0;    ///< col 2: query sequence length
    sequence_size_t query_start    = 0;    ///< col 3: query start (0-based, closed)
    sequence_size_t query_end      = 0;    ///< col 4: query end (0-based, open)
    bool    is_reverse             = false;///< col 5: '+' or '-'
    sequence_size_t target_len     = 0;    ///< col 7: target sequence length
    sequence_size_t target_start   = 0;    ///< col 8: target start on original strand
    sequence_size_t target_end     = 0;    ///< col 9: target end on original strand
    sequence_size_t num_matches    = 0;    ///< col 10: number of residue matches (mlen)
    sequence_size_t block_length   = 0;    ///< col 11: alignment block length (blen)
    int32_t mapq                   = 0;    ///< col 12: mapping quality (0-255)

    // Additional alignment info
    int32_t chain_score            = 0;    ///< chaining score
    int32_t dp_score               = 0;    ///< DP alignment score
    int32_t dp_max_score           = 0;    ///< max scoring segment score (dp_max0 in original)
    int32_t seed_count             = 0;    ///< number of seeds in chain
    int32_t secondary_chain_score  = 0;    ///< best secondary chaining score against this primary
    float   divergence             = -1.f; ///< sequence divergence estimate
    bool    is_primary             = true; ///< primary mapping flag
    bool    has_cigar              = false;///< whether CIGAR alignment was performed
    std::vector<CigarOp> cigar;            ///< CIGAR string
};

// ============================================================================
// Internal data structures
// ============================================================================
// Anchor, AnchorRefFields, AnchorQueryFields, Chain are now imported from
// bioseq_sequences.chaining.utils via 'using' declarations above.

/**
 * @brief Alignment region — equivalent to mm_reg1_t.
 *
 * Produced after mm_gen_regs, enriched through the pipeline.
 */
struct AlignRegion {
    int32_t region_id       = 0;
    int32_t parent          = -1;   ///< parent id (-1 = unset, ==region_id => primary)
    int32_t score           = 0;    ///< chain score
    int32_t original_score  = 0;    ///< initial chain score (before merging)
    int32_t seed_count      = 0;    ///< number of seeds
    int32_t anchor_start    = 0;    ///< offset in anchor array

    // DOC0263 §10.2 — absolute coordinates / lengths are `sequence_size_t`
    // (uint64) so they carry the untruncated 64-bit anchor positions through
    // to the MatchResult. Intermediate coordinate math that can go negative
    // (e.g. ref_position + 1 - span) is done in signed int64 then clamped.
    sequence_size_t query_start = 0;    ///< query start position
    sequence_size_t query_end   = 0;    ///< query end position
    sequence_size_t ref_start   = 0;    ///< reference start position
    sequence_size_t ref_end     = 0;    ///< reference end position
    int32_t reference_id    = 0;    ///< reference sequence id (0 in our case)
    bool    is_reverse      = false;///< reverse strand

    sequence_size_t match_length = 0;   ///< seeded exact match length
    sequence_size_t block_length = 0;   ///< seeded alignment block length
    int32_t secondary_score = 0;    ///< best secondary score against this primary
    int32_t secondary_count = 0;    ///< number of suboptimal mappings

    // DP alignment results (filled after alignment phase)
    int32_t dp_score            = 0;
    int32_t dp_max_score        = 0;    ///< max scoring segment
    int32_t secondary_dp_max    = 0;    ///< best secondary DP score
    bool    has_alignment       = false;///< whether DP alignment was performed
    float   divergence          = -1.f; ///< divergence estimate

    int32_t mapq            = 0;

    std::vector<CigarOp> cigar;
};

// fast_log2 is now in bioseq_sequences.chaining.utils

// ============================================================================
// Phase 1: Build target index from target minimizers
// ============================================================================

/**
 * @brief Simple hash map index for target minimizers.
 *
 * Maps minimizer hash -> vector of (position, strand) pairs in the target.
 * Equivalent to mm_idx_t's B[] bucket array + hash table, but simplified
 * for a single target sequence.
 *
 * Each stored position encodes: (rid << 32) | (pos << 1) | strand
 * For single-target, rid = 0.
 */
struct TargetIndex {
    // Flat layout — open-addressing hash maps each kmer to a packed
    // (offset:32 | count:32) pair pointing into a flat positions array.
    // Replaces the legacy `std::unordered_map<uint64_t, std::vector<uint64_t>>`
    // (one heap-allocation per distinct minimizer + chained-bucket lookup),
    // matching minimap2's khash-style layout. For a 1 Mbp pair with ~100 k
    // distinct kmers this drops ~200 k small allocations per pair down to
    // ~3 (one for the table, one for the positions, one for the count
    // builder) and lifts lookup throughput because boost::unordered_flat_map
    // is open-addressing with SSE2/NEON-accelerated probing.
    boost::unordered_flat_map<uint64_t, uint64_t> table;
    std::vector<uint64_t>                          positions;
    sequence_size_t target_len = 0;   ///< DOC0263 §10.2 — full 64-bit target length

    static constexpr uint32_t kSerializeMagic   = 0xB1000001u;
    static constexpr uint32_t kSerializeVersion = 2u;

    /* Packed record shapes for the wire format. Using typed structs instead
     * of four-field-at-a-time memcpy sequences lets the serializer emit a
     * single memcpy per block and the compiler fold adjacent field stores
     * into wide aligned writes; it also lets std::bit_cast-style validation
     * run at compile time (see kSerializeHeaderBytes below). */
    struct SerializedHeader {
        uint32_t magic;
        uint32_t version;
        uint64_t target_len;       ///< DOC0263 §10.2 — version 2 widens this to 64-bit
        uint32_t bucket_count;
        uint32_t reserved;         ///< pad so total_positions stays 8-byte aligned; written as 0
        uint64_t total_positions;
    };
    /* Bucket layout v2: hash(u64) + pos_offset(u32) + pos_count(u32) = 16 bytes.
     * A single TargetIndex cannot contain more than UINT32_MAX buckets nor
     * UINT32_MAX positions in practice (a bucket with 2^32 entries would
     * imply a 32 GB sequence). Shrinks the bucket array by 33% vs v1. */
    struct SerializedBucket {
        uint64_t hash;
        uint32_t pos_offset;
        uint32_t pos_count;
    };
    static constexpr std::size_t kSerializeHeaderBytes = sizeof(SerializedHeader);
    static constexpr std::size_t kSerializeBucketBytes = sizeof(SerializedBucket);
    static_assert(kSerializeHeaderBytes == 32,
                  "SerializedHeader wire layout must remain 32 bytes (v2: 64-bit target_len)");
    static_assert(kSerializeBucketBytes == 16,
                  "SerializedBucket wire layout must remain 16 bytes");

    /* Serialization format depends on native little-endian, 64-bit integers.
     * The columnar storage layer already requires the same ABI — if the host
     * is big-endian, the extension as a whole is unsupported. */
    static_assert(std::endian::native == std::endian::little,
                  "TargetIndex serialization requires little-endian host");
    static_assert(sizeof(std::size_t) >= 8,
                  "TargetIndex serialization requires 64-bit size_t");

    template <class MinimizerT>
    void build(const std::vector<MinimizerT>& target_minimizers,
               sequence_size_t target_length) {
        static const auto* _desc =
            bioseq::instr::register_region_static("target_index_build");
        auto _scope = bioseq::instr::region(_desc);
        target_len = target_length;
        table.clear();
        positions.clear();

        const std::size_t total = target_minimizers.size();
        if (total == 0) {
            return;
        }

        // Pass 1: bump per-kmer count. The packed value temporarily holds
        //         count in the low 32 bits and 0 in the high 32 bits; the
        //         sentinel offset value is rewritten in pass 2.
        table.reserve(total);
        for (const auto& minimizer : target_minimizers) {
            ++table[minimizer.kmer()];
        }

        // Pass 2: convert per-kmer counts into a (offset, cursor) pair —
        // the high 32 bits hold the bucket's base offset and the low 32 bits
        // are pre-zeroed to act as a per-kmer write cursor for Pass 3. The
        // legacy code wrote `(offset << 32) | count` here and then needed a
        // separate sweep to clear the low 32 bits before Pass 3; since
        // Pass 3 doesn't consult the count and only ever writes the cursor,
        // we just emit `offset << 32` directly and drop the prep sweep.
        // (After Pass 3 the cursor naturally lands at `count`, which is
        // what `lookup()` reads back.)
        positions.resize(total);
        uint32_t running_offset = 0;
        for (auto& kv : table) {
            const uint32_t count = static_cast<uint32_t>(kv.second);
            kv.second = static_cast<uint64_t>(running_offset) << 32;
            running_offset += count;
        }

        // Pass 3: write each minimizer into its bucket using a per-kmer
        // "next-free" cursor stashed in the low 32 bits. Each iteration:
        //   * derive the encoded position (last_pos << 1) | strand from
        //     `pos_strand_encoded()` — for the canonical 64-bit layout
        //     this is one masked load (`value & ~hash_mask`); for the
        //     128-bit fallback it's the low half;
        //   * fetch the bucket's (base_offset, cursor) pair via one hash
        //     map probe;
        //   * splat into positions[base_offset + cursor] and bump cursor.
        // The bucket entry now stores `(base_offset << 32) | (cursor+1)`;
        // a single `++iter->second` works because the cursor occupies the
        // low 32 bits and a unsigned-32 count cannot overflow into the
        // base_offset region for any realistic genome.
        for (const auto& minimizer : target_minimizers) {
            const uint64_t encoded_position = minimizer.pos_strand_encoded();
            auto iter = table.find(minimizer.kmer());
            const uint64_t packed = iter->second;
            const uint32_t base_offset = static_cast<uint32_t>(packed >> 32);
            const uint32_t cursor      = static_cast<uint32_t>(packed);
            positions[base_offset + cursor] = encoded_position;
            ++iter->second;
        }

        JustLogProgress("[minimap_port] Built target index: {} distinct minimizers from {} total"
                " | positions={} footprint={}MB avail={}MB",
                table.size(), target_minimizers.size(), positions.size(),
                bioseq::instr::detail::sysinfo::process_footprint_bytes() >> 20,
                bioseq::instr::detail::sysinfo::system_available_bytes() >> 20);
    }

    [[nodiscard]] std::span<const uint64_t> lookup(uint64_t kmer_hash) const {
        auto iter = table.find(kmer_hash);
        if (iter == table.end()) {
            return {};
        }
        const uint64_t packed = iter->second;
        const uint32_t off    = static_cast<uint32_t>(packed >> 32);
        const uint32_t count  = static_cast<uint32_t>(packed);
        return std::span<const uint64_t>(positions.data() + off, count);
    }

    // DOC0276 — segmented indexing support. The positions array holds
    // `pos_strand_encoded()` = `(last_pos << 1) | strand`. When this index was
    // built over a target SEGMENT collected from a sub-range starting at global
    // bp `offset`, every `last_pos` is LOCAL to the segment; add `offset` to
    // each to make the index report GLOBAL target coordinates. The positions
    // array is `uint64_t` (full width — independent of the 28-bit MinimapSketchData
    // collection field), so a global position up to 2^63 round-trips here. The
    // kmer table is untouched (lookups still hit). O(positions) one-shot pass,
    // run once per segment build.
    void add_position_offset(sequence_size_t offset) {
        if (offset == 0) return;
        for (uint64_t& encoded : positions) {
            const uint64_t pos    = (encoded >> 1) + static_cast<uint64_t>(offset);
            const uint64_t strand = encoded & 1ULL;
            encoded = (pos << 1) | strand;
        }
    }

    [[nodiscard]] sequence_size_t target_length() const { return target_len; }

    // --------------------------------------------------------------------
    //  Fast binary serialization  — v2 layout (little-endian, LP64)
    //
    //    0..3   magic         (uint32)
    //    4..7   version       (uint32)
    //    8..11  target_len    (int32)
    //   12..15  bucket_count  (uint32)
    //   16..23  total_positions (uint64)
    //   24..24+16*n  bucket_array[n] = (hash:u64, pos_offset:u32, pos_count:u32)
    //   ...          positions[total_positions] = uint64[]
    // --------------------------------------------------------------------

    [[nodiscard]] std::size_t serialize_binary_size() const {
        return kSerializeHeaderBytes
             + kSerializeBucketBytes * table.size()
             + 8 * positions.size();
    }

    void serialize_to_binary(char* output) const {
        const uint32_t bucket_count    = static_cast<uint32_t>(table.size());
        const uint64_t total_positions = static_cast<uint64_t>(positions.size());

        /* One aligned memcpy for the whole 32-byte header beats six
         * field-by-field stores: the compiler emits a single pair of
         * SIMD stores on ARM64 / x86_64 once the struct is ABI-laid-out. */
        const SerializedHeader header{
            kSerializeMagic,
            kSerializeVersion,
            target_len,
            bucket_count,
            0u,               // reserved (alignment pad)
            total_positions,
        };
        std::memcpy(output, &header, sizeof(header));

        char* bucket_cursor   = output + kSerializeHeaderBytes;
        char* position_cursor = bucket_cursor + kSerializeBucketBytes * bucket_count;

        for (const auto& kv : table) {
            const uint64_t packed   = kv.second;
            const uint32_t pos_off  = static_cast<uint32_t>(packed >> 32);
            const uint32_t pos_cnt  = static_cast<uint32_t>(packed);
            const SerializedBucket bucket{ kv.first, pos_off, pos_cnt };
            std::memcpy(bucket_cursor, &bucket, sizeof(bucket));
            bucket_cursor += kSerializeBucketBytes;
        }

        if (total_positions > 0) {
            /* Positions live in a single contiguous flat array; one memcpy
             * becomes the fastest available cache-cold bulk copy on both
             * target ISAs. The on-disk position layout is identical to v2:
             * each bucket stores (pos_offset, pos_count) into this array. */
            std::memcpy(position_cursor, positions.data(), total_positions * 8);
        }
    }

    [[nodiscard]] static TargetIndex deserialize_from_binary(
        const char* input, std::size_t input_size)
    {
        /* ALL validation happens before we touch the unordered_map, so an
         * elog(ERROR) longjmp can never leave a partially-populated
         * std::unordered_map on the stack (the hash buckets are backed by
         * malloc, not palloc — PG's context reset cannot free them). */
        if (input_size < kSerializeHeaderBytes) {
            elog(ERROR,
                 "bioseq: TargetIndex deserialize: truncated blob (%zu bytes)",
                 input_size);
        }

        SerializedHeader header{};
        std::memcpy(&header, input, sizeof(header));

        if (header.magic != kSerializeMagic) {
            elog(ERROR,
                 "bioseq: TargetIndex deserialize: magic mismatch 0x%08x (expected 0x%08x)",
                 header.magic, kSerializeMagic);
        }
        if (header.version != kSerializeVersion) {
            elog(ERROR,
                 "bioseq: TargetIndex deserialize: version mismatch %u (expected %u)",
                 header.version, kSerializeVersion);
        }

        const std::size_t expected_size =
            kSerializeHeaderBytes
          + kSerializeBucketBytes * header.bucket_count
          + 8 * header.total_positions;
        if (input_size != expected_size) {
            elog(ERROR,
                 "bioseq: TargetIndex deserialize: size mismatch expected=%zu got=%zu",
                 expected_size, input_size);
        }

        /* Top-level header is validated; per-bucket bounds checks remain
         * inside the loop below (pos_offset + pos_count <= total_positions).
         * Those checks also use elog(ERROR) and must fire BEFORE the bucket
         * is emplaced so no hash-map bucket is leaked on longjmp. */
        TargetIndex idx;
        idx.target_len = header.target_len;
        idx.table.reserve(header.bucket_count);

        const char* bucket_cursor   = input + kSerializeHeaderBytes;
        const char* position_cursor = bucket_cursor + kSerializeBucketBytes * header.bucket_count;

        // Slurp the positions array in one bulk memcpy. The on-disk layout
        // already matches the in-memory flat layout, so no per-bucket
        // sub-vectors need to be built.
        idx.positions.resize(header.total_positions);
        if (header.total_positions > 0) {
            std::memcpy(idx.positions.data(),
                        position_cursor,
                        header.total_positions * 8);
        }

        for (uint32_t b = 0; b < header.bucket_count; ++b) {
            SerializedBucket bucket{};
            std::memcpy(&bucket, bucket_cursor, sizeof(bucket));
            bucket_cursor += kSerializeBucketBytes;

            /* Bounds check before the emplace — otherwise a crafted blob with
             * pos_offset = 0xFFFFFFF0 can produce a span that overflows the
             * positions array. Uses uint64_t arithmetic to avoid uint32 wrap. */
            if (static_cast<uint64_t>(bucket.pos_offset) + bucket.pos_count > header.total_positions) {
                elog(ERROR,
                     "bioseq: TargetIndex deserialize: bucket %u range (offset=%u count=%u) "
                     "exceeds total_positions=%llu",
                     b, bucket.pos_offset, bucket.pos_count,
                     static_cast<unsigned long long>(header.total_positions));
            }

            const uint64_t packed =
                (static_cast<uint64_t>(bucket.pos_offset) << 32)
                | static_cast<uint64_t>(bucket.pos_count);
            idx.table.emplace(bucket.hash, packed);
        }

        return idx;
    }
};

// ----------------------------------------------------------------------------
//  MinimizerIndex concept
//
//  Models any type that can be used as a minimizer lookup structure inside
//  minimap2_port's pipeline. Consumers template on the concrete type so there
//  is zero vtable overhead on the hot path (seed hit collection).
// ----------------------------------------------------------------------------

template <typename T>
concept MinimizerIndex = requires (
    T& mutable_index,
    const T& const_index,
    const std::vector<MinimapSketchData>& sketches,
    sequence_size_t target_length_arg,
    uint64_t kmer_hash,
    char* output_buffer,
    const char* input_buffer,
    std::size_t input_size)
{
    { mutable_index.build(sketches, target_length_arg) } -> std::same_as<void>;
    { const_index.lookup(kmer_hash) } -> std::convertible_to<std::span<const uint64_t>>;
    { const_index.target_length() } -> std::convertible_to<sequence_size_t>;
    { const_index.serialize_binary_size() } -> std::convertible_to<std::size_t>;
    { const_index.serialize_to_binary(output_buffer) } -> std::same_as<void>;
    { T::deserialize_from_binary(input_buffer, input_size) } -> std::same_as<T>;
};

static_assert(MinimizerIndex<TargetIndex>,
              "TargetIndex must model MinimizerIndex");


// ============================================================================
// Phase 2: Collect seed hits — look up query minimizers in target index
// ============================================================================

/**
 * @brief Collect seed anchors by looking up query minimizers in the target index.
 *
 * Equivalent to collect_seed_hits() in map.c:518-554.
 *
 * For each query minimizer, look up its hash in the target index. For each
 * hit, create an anchor encoding both reference and query positions.
 *
 * Anchor encoding (same as minimap2):
 *   ref_bits: is_reverse<<63 | reference_id<<32 | ref_position
 *   query_bits: query_span<<32 | query_position
 *
 * Where for forward strand: ref_position = target_pos, query_position = query_pos
 * For reverse strand: set is_reverse bit, adjust query_position for reverse complement.
 *
 * @param query_minimizers Pre-computed query minimizers
 * @param target_index     Target index to search
 * @param query_len        Length of query sequence
 * @param max_occ          Max occurrences to keep (filter high-frequency seeds)
 * @param kmer_size        K-mer length used for the encoded anchor span.
 * @param anchors_out      Output slab — sorted anchors are appended here.
 *                          Must be a live ArenaSlab rooted in the caller's
 *                          per-query `Arena::Chain` scope; the kernel writes
 *                          directly into it via `push_back` and sorts in
 *                          place before returning (DOC0249 §13 follow-up:
 *                          output-side kernel-to-arena refactor).
 */
template <class MinimizerT, MinimizerIndex IndexT>
void collect_seed_hits(
    std::span<const MinimizerT> query_minimizers,
    const IndexT& target_index,
    sequence_size_t query_len,
    int32_t max_occ,
    int32_t kmer_size,
    ::bioseq::chaining::ArenaSlab<Anchor>& anchors_out)
{
    static const auto* _seed_hits_desc =
        bioseq::instr::register_region_static("collect_seed_hits");
    auto _seed_hits_scope = bioseq::instr::region(_seed_hits_desc);
    // The caller is expected to pre-reserve the slab using the
    // `estimated_anchor_count` helper from `bioseq::simd::HWY_NAMESPACE::pipe::detail`
    // — that estimate is tighter than `query_minimizers.size() * 2` for very-short
    // sequences (clamped by `shared_ceiling`) and identical for the common case.
    // We still issue a defensive `reserve` here so the kernel works when invoked
    // directly with a fresh slab; the call is a no-op when `cap_` already covers
    // the requested capacity (see `ArenaSlab::reserve` short-circuit).
    anchors_out.reserve(query_minimizers.size() * 2);

    // Target-index lookup is the hot per-minimizer operation. We wrap the
    // whole loop (not each lookup) to avoid profiler overhead dominating
    // the measurement; the loop body also classifies/sorts anchors.
    {
    static const auto* _idx_lookup_desc =
        bioseq::instr::register_region_static("target_index_lookup");
    auto _idx_lookup_scope = bioseq::instr::region(_idx_lookup_desc);
    // The compact MinimizerT no longer carries a per-minimizer span field
    // — for the dna4 bench path span is identically equal to the
    // configured kmer size, so we hoist it from `kmer_size` once outside
    // the loop instead of reading a now-removed bitfield.
    const uint32_t query_span_value = static_cast<uint32_t>(kmer_size);
    // Precompute the constant added to the negated forward_query_pos in the
    // reverse-strand branch:
    //   reverse_query_pos = query_len - (fwd + 1 - span) - 1
    //                     = (query_len - 2 + span) - fwd
    // Hoisting this addition out of the inner loop lets the compiler keep
    // (sub fwd) as the only per-hit op for the reverse branch.
    const sequence_size_t reverse_pos_base = query_len - 2 + query_span_value;
    for (const auto& query_minimizer : query_minimizers) {
        const auto target_hits = target_index.lookup(query_minimizer.kmer());
        if (target_hits.empty() || static_cast<int32_t>(target_hits.size()) > max_occ) {
            continue;
        }

        // `pos_strand_encoded()` returns `(last_pos << 1) | strand` — the
        // exact form `lookup()` packed into the positions array, so the
        // per-hit `target_hit` lives in the same encoded space.
        // DOC0274 §1.9 — keep the FULL 64-bit encoded value. `pos_bits` is
        // `ceil(log2(seq_len))` (up to 32 for a multi-Gbp sequence), so
        // `(last_pos << 1) | strand` spans up to 33 bits; the old
        // `static_cast<uint32_t>(...)` truncated any position >= 2^31. The
        // query is small here, but mirroring the target-side fix below keeps
        // both sides 64-bit-correct.
        const uint64_t query_pos_encoded = query_minimizer.pos_strand_encoded();
        // Hoist the strand bit out of the inner per-hit loop — constant
        // across `target_hits`.
        const uint32_t query_strand_bit = static_cast<uint32_t>(query_pos_encoded & 1ULL);
        const sequence_size_t forward_query_pos = static_cast<sequence_size_t>(query_pos_encoded >> 1);
        // Reverse-strand position formula reduces to a single subtraction
        // once `reverse_pos_base` is hoisted; pre-compute here so the
        // branchless `csel` below picks between two register-resident
        // 64-bit positions. (forward_query_pos is a k-mer start <= query_len-k,
        // so reverse_pos_base - forward_query_pos never underflows.)
        const sequence_size_t reverse_query_pos = reverse_pos_base - forward_query_pos;

        for (const uint64_t target_hit : target_hits) {
            // DOC0274 §1.9 — `target_hit` is the index-stored
            // `pos_strand_encoded()` = `(ref_pos << 1) | strand`, a full
            // (pos_bits + 1)-bit value (`pos_bits == ceil(log2(target_len))`,
            // up to 32 for a 4 Gbp target ⇒ up to 33 bits). The previous
            // `static_cast<uint32_t>(target_hit) >> 1` truncated to the low 32
            // bits, dropping bit 32 of `ref_pos << 1` for any ref_pos >= 2^31
            // (~2.15 Gbp). The corrupted anchor coordinate then indexed the
            // target buffer out of bounds → SIGSEGV. Shift the full 64-bit
            // word instead. There is no reference_id in the encoding
            // (single-target; `pos_strand_encoded()` masks the hash out), so
            // bits above the position are part of the position, not an rid —
            // hence the former `target_hit >> 32` "rid" read is gone.
            const sequence_size_t ref_position =
                static_cast<sequence_size_t>(target_hit >> 1);

            // Goal C — branchless strand select.  The two arms of the old
            // if/else differed only in `is_reverse` (0 vs 1) and
            // `query_position` (fwd vs reverse formula).  Compute both,
            // select via the strand-mismatch XOR.  Clang lowers the
            // ternary to `csel` on AArch64, eliminating the branch
            // mispredict on mixed-strand hot loops.
            const uint32_t target_strand_bit = static_cast<uint32_t>(target_hit & 1ULL);
            const uint32_t is_reverse_bit    = target_strand_bit ^ query_strand_bit;
            const sequence_size_t selected_query_pos =
                is_reverse_bit ? reverse_query_pos : forward_query_pos;

            // DOC0263 anchor flat layout: ref/query sub-objects removed.
            // `reference_id` field removed (single-target codebase). DOC0274
            // §1.9 also removed the `target_hit >> 32` "rid" read: it was only
            // ever 0 because positions stayed < 2^31; past that it aliases the
            // high bit of `ref_pos << 1`, so it never was a reference id.
            Anchor anchor;
            anchor.ref_position    = ref_position;
            anchor.is_reverse      = is_reverse_bit;
            anchor.query_position  = selected_query_pos;
            anchor.query_span      = static_cast<std::uint8_t>(query_span_value);
            anchor.flags           = 0;
            anchor.segment_id      = 0;
            anchors_out.push_back(anchor);
        }
    }
    }  // end target_index_lookup scope

    // Sort by reference position (ref_bits field), exact port of
    // minimap2's `radix_sort_128x(a, a + n_a)` from misc.c. The
    // 8-bit-per-pass MSD radix sort is faster than libc++ pdqsort by
    // ~3-5x at the anchor populations we see (50 k - 1 M+ at LONG/XL/XXL)
    // because it bypasses the comparison-based branch-predictor
    // pessimisation pdqsort hits when the ref_position prefix is
    // already monotonic — most of the work is just the 256-bucket
    // counting scan, which the M1 LSU pipelines well.
    //
    // A SIMD-vectorised bit-by-bit MSD radix variant (Bramas / Möller /
    // Keller, 2024 thesis) was prototyped on top of Highway's
    // `CompressBlendedStore`. On NEON-128 / Apple M1 the SIMD partition
    // is restricted to 2 u64 lanes per vector (vs. 8 on AVX-512) and
    // each NEON BlendedStore lowers to a 3-step Load+Blend+Store —
    // measured as 5–22 percentage points slower than this 8-bit MSD
    // radix sort on the bench's LONG/VLONG/XL/XXL buckets. See
    // main/DOCS/REPORTS/NEW_SPEEDUP_REPORT_2__DOC0149.md for the full V1-vs-V2
    // numbers; the SIMD variant becomes attractive again on AVX-512
    // hosts and is left as a future-work option.
    {
    static const auto* _sort_desc =
        bioseq::instr::register_region_static("collect_seed_hits_sort");
    auto _sort_scope = bioseq::instr::region(_sort_desc);
    /// Anchor sort key — extracts the 64-bit `ref_bits` field for the radix
    /// sort.  Non-capturing struct functor (DOC0249 R4) so the file-wide grep
    /// gate for capture-all lambdas under `bioseq/backend/pipeline/` is clean
    /// even on a bare `[](const Anchor&){...}` extractor.
    struct AnchorRefBitsKey {
        [[nodiscard, gnu::always_inline]]
        std::uint64_t operator()(const Anchor& anchor) const noexcept {
            return anchor.ref_bits();
        }
    };
    bioseq::sorting::radix_sort(
        anchors_out.data(),
        anchors_out.data() + anchors_out.size(),
        AnchorRefBitsKey{});
    }  // end collect_seed_hits_sort scope

    JustLogProgress("[minimap_port] Collected {} seed anchors from {} query minimizers"
            " | footprint={}MB avail={}MB",
            anchors_out.size(), query_minimizers.size(),
            bioseq::instr::detail::sysinfo::process_footprint_bytes() >> 20,
            bioseq::instr::detail::sysinfo::system_available_bytes() >> 20);
}

// ============================================================================
// Phase 3: Chaining — now delegated to chaining modules
// ============================================================================
// compute_chaining_score is imported from bioseq_sequences.chaining.utils
// chain_anchors_scalar is imported from bioseq_sequences.chaining.scalar
// chain_anchors_simd is imported from bioseq_sequences.chaining.simd (in bioseq::simd::HWY_NAMESPACE)
// chain_anchors_rmq is imported from bioseq_sequences.chaining.rmq

// ============================================================================
// Phase 4: Convert chains to alignment regions (mm_gen_regs equivalent)
// ============================================================================

/**
 * @brief Compute seeded match length and block length for a region.
 *
 * Equivalent to mm_cal_fuzzy_len() in hit.c.
 *
 * match_length: approximate number of matching bases in seeded regions
 * block_length: approximate total aligned length in seeded regions
 */
void compute_fuzzy_lengths(AlignRegion& region, std::span<const Anchor> anchors) {
    region.match_length = 0;
    region.block_length = 0;
    if (region.seed_count <= 0) {
        return;
    }

    const int32_t anchor_begin = region.anchor_start;
    // Accumulate in signed int64 (gaps are bounded position *deltas*, never
    // absolute positions) then store into the sequence_size_t fields.
    std::int64_t match_length = static_cast<int32_t>(anchors[anchor_begin].query_span);
    std::int64_t block_length = match_length;

    for (int32_t anchor_idx = anchor_begin + 1;
         anchor_idx < anchor_begin + region.seed_count;
         ++anchor_idx)
    {
        const int32_t span = static_cast<int32_t>(anchors[anchor_idx].query_span);
        const int32_t target_gap = static_cast<int32_t>(
            anchors[anchor_idx].ref_position - anchors[anchor_idx - 1].ref_position);
        const int32_t query_gap = static_cast<int32_t>(
            anchors[anchor_idx].query_position - anchors[anchor_idx - 1].query_position);
        const int32_t max_gap = (target_gap > query_gap) ? target_gap : query_gap;
        block_length += max_gap;
        const bool both_gaps_exceed_span = (target_gap > span && query_gap > span);
        const int32_t min_gap = (target_gap < query_gap) ? target_gap : query_gap;
        const int32_t match_increment = both_gaps_exceed_span ? span : min_gap;
        match_length += match_increment;
    }
    region.match_length = static_cast<sequence_size_t>(match_length);
    region.block_length = static_cast<sequence_size_t>(block_length);
}

/**
 * @brief Set alignment coordinates from seed positions.
 *
 * Equivalent to mm_reg_set_coor() in hit.c.
 * Derives query_start, query_end, ref_start, ref_end from first and last anchor.
 */
void set_region_coordinates(
    AlignRegion& region,
    std::span<const Anchor> anchors,
    sequence_size_t query_len)
{
    if (region.seed_count <= 0) {
        return;
    }
    const int32_t anchor_begin = region.anchor_start;
    const Anchor& first_anchor = anchors[anchor_begin];
    const Anchor& last_anchor = anchors[anchor_begin + region.seed_count - 1];

    region.is_reverse = first_anchor.is_reverse != 0;
    region.reference_id = 0;  // DOC0263: single-target codebase, reference_id always 0

    // Reference coordinates. The absolute positions are full 64-bit; the
    // intermediate `position + 1 - span` can be negative, so it is computed in
    // signed int64 and clamped before being stored in the sequence_size_t field.
    const std::int64_t first_span = static_cast<int32_t>(first_anchor.query_span);
    std::int64_t ref_start = static_cast<std::int64_t>(first_anchor.ref_position) + 1 - first_span;
    const std::int64_t ref_end = static_cast<std::int64_t>(last_anchor.ref_position) + 1;
    if (ref_start < 0) {
        ref_start = 0;
    }
    region.ref_start = static_cast<sequence_size_t>(ref_start);
    region.ref_end = static_cast<sequence_size_t>(ref_end);

    // Query coordinates
    std::int64_t raw_query_start = static_cast<std::int64_t>(first_anchor.query_position)
                                 + 1 - first_span;
    const std::int64_t raw_query_end = static_cast<std::int64_t>(last_anchor.query_position) + 1;
    if (raw_query_start < 0) {
        raw_query_start = 0;
    }

    if (!region.is_reverse) {
        region.query_start = static_cast<sequence_size_t>(raw_query_start);
        region.query_end = static_cast<sequence_size_t>(raw_query_end);
    } else {
        const std::int64_t qlen = static_cast<std::int64_t>(query_len);
        region.query_start = static_cast<sequence_size_t>(qlen - raw_query_end);
        region.query_end = static_cast<sequence_size_t>(qlen - raw_query_start);
    }

    compute_fuzzy_lengths(region, anchors);
}

/**
 * @brief Convert chains to alignment regions.
 *
 * Equivalent to mm_gen_regs() in hit.c:54-90.
 *
 * Sorts chains by score (descending) and creates AlignRegion records
 * with coordinates derived from the anchor positions.
 *
 * @param chains       Chains (sorted in place by score descending here).
 * @param anchors      Reordered anchor span — `chains[i].anchor_start`
 *                      indexes into this span.
 * @param query_len    Length of the query sequence (used to flip query
 *                      coordinates for reverse-strand chains).
 * @param regions_out  Output slab — one AlignRegion per chain is constructed
 *                      via placement-new (so the slab's destructor releases
 *                      `region.cigar` correctly on unwind).  Must be a fresh
 *                      ArenaSlab; the kernel calls `reserve_for_placement_new`
 *                      and `construct_at_back` once per chain (DOC0249 §13
 *                      follow-up: output-side kernel-to-arena refactor).
 */
void generate_regions(
    std::span<Chain> chains,
    std::span<const Anchor> anchors,
    sequence_size_t query_len,
    ::bioseq::chaining::ArenaSlab<AlignRegion>& regions_out)
{
    static const auto* _desc =
        bioseq::instr::register_region_static("mm_gen_regs");
    auto _scope = bioseq::instr::region(_desc);
    if (chains.empty()) {
        return;
    }

    /// Chain comparator — orders chains by score descending so the highest-
    /// scoring chain becomes the primary alignment region.  Non-capturing
    /// functor for DOC0249 R4 compliance (no capture-all lambdas in the
    /// pipeline module tree).
    struct ChainScoreDescending {
        [[nodiscard, gnu::always_inline]] bool operator()(
            const Chain& left, const Chain& right) const noexcept
        {
            return left.score > right.score;
        }
    };
    std::sort(chains.begin(), chains.end(), ChainScoreDescending{});

    regions_out.reserve_for_placement_new(chains.size());
    for (size_t region_idx = 0; region_idx < chains.size(); ++region_idx) {
        AlignRegion& region = regions_out.construct_at_back(AlignRegion{
            .region_id      = static_cast<int32_t>(region_idx),
            .parent         = -1,
            .score          = chains[region_idx].score,
            .original_score = chains[region_idx].score,
            .seed_count     = chains[region_idx].anchor_count,
            .anchor_start   = chains[region_idx].anchor_start,
            .divergence     = -1.0f,
        });
        set_region_coordinates(region, anchors, query_len);
    }

    JustLogProgress("[minimap_port] Generated {} alignment regions | footprint={}MB avail={}MB",
            regions_out.size(),
            bioseq::instr::detail::sysinfo::process_footprint_bytes() >> 20,
            bioseq::instr::detail::sysinfo::system_available_bytes() >> 20);
}

// ============================================================================
// Phase 5: Primary/secondary classification (mm_set_parent equivalent)
// ============================================================================

/**
 * @brief Classify alignments as primary or secondary.
 *
 * Equivalent to mm_set_parent() in hit.c:127-188.
 *
 * An alignment is secondary if it overlaps significantly with a primary
 * on the query AND has insufficient unique (uncovered) sequence.
 *
 * Criterion: (overlap/min_interval) - (uncov/max_interval) > mask_level
 *            AND uncov <= mask_len
 */
void classify_primary_secondary(
    std::span<AlignRegion> regions,
    float mask_level,
    int32_t mask_len,
    int32_t sub_diff)
{
    static const auto* _desc =
        bioseq::instr::register_region_static("mm_set_parent");
    auto _scope = bioseq::instr::region(_desc);
    const size_t region_count = regions.size();
    if (region_count == 0) {
        return;
    }

    for (size_t region_idx = 0; region_idx < region_count; ++region_idx) {
        regions[region_idx].region_id = static_cast<int32_t>(region_idx);
    }

    // primary_indices holds indices of primary alignments. Reserve up
    // front so the first push_back doesn't trigger a 1→2→4→… growth
    // chain through `region_count` worth of pushes when the chains
    // happen to be all primary. Sized at region_count (the upper bound
    // — every region is primary in the degenerate case).
    std::vector<size_t> primary_indices;
    primary_indices.reserve(region_count);
    primary_indices.push_back(0);
    regions[0].parent = 0;

    // coverage_intervals is cleared+refilled on every secondary check,
    // capped at primary_indices.size() entries. Pre-reserve at
    // region_count too so the inner loop never triggers realloc.
    std::vector<uint64_t> coverage_intervals;
    coverage_intervals.reserve(region_count);

    for (size_t region_idx = 1; region_idx < region_count; ++region_idx) {
        AlignRegion& current_region = regions[region_idx];
        const int32_t current_query_start = current_region.query_start;
        const int32_t current_query_end = current_region.query_end;
        int32_t uncovered_length = 0;

        // Compute coverage from existing primaries
        coverage_intervals.clear();
        for (const size_t primary_idx : primary_indices) {
            const AlignRegion& primary_region = regions[primary_idx];
            int32_t primary_query_start = primary_region.query_start;
            int32_t primary_query_end = primary_region.query_end;
            if (primary_query_end <= current_query_start
                || primary_query_start >= current_query_end)
            {
                continue;
            }
            primary_query_start = std::max(primary_query_start, current_query_start);
            primary_query_end = std::min(primary_query_end, current_query_end);
            coverage_intervals.push_back(
                (static_cast<uint64_t>(primary_query_start) << 32)
                | static_cast<uint32_t>(primary_query_end));
        }

        if (!coverage_intervals.empty()) {
            std::sort(coverage_intervals.begin(), coverage_intervals.end());
            int32_t coverage_pos = current_query_start;
            for (const uint64_t interval : coverage_intervals) {
                const int32_t interval_start = static_cast<int32_t>(interval >> 32);
                const int32_t interval_end = static_cast<int32_t>(interval);
                if (interval_start > coverage_pos) {
                    uncovered_length += interval_start - coverage_pos;
                }
                coverage_pos = std::max(coverage_pos, interval_end);
            }
            if (current_query_end > coverage_pos) {
                uncovered_length += current_query_end - coverage_pos;
            }
        }

        // Test against each primary
        bool is_secondary = false;
        for (const size_t primary_idx : primary_indices) {
            AlignRegion& primary_region = regions[primary_idx];
            const int32_t primary_query_start = primary_region.query_start;
            const int32_t primary_query_end = primary_region.query_end;
            if (primary_query_end <= current_query_start
                || primary_query_start >= current_query_end)
            {
                continue;
            }

            const int32_t min_span = std::min(primary_query_end - primary_query_start,
                                              current_query_end - current_query_start);
            const int32_t max_span = std::max(primary_query_end - primary_query_start,
                                              current_query_end - current_query_start);
            const int32_t overlap_length = std::max(0,
                std::min(current_query_end, primary_query_end)
                - std::max(current_query_start, primary_query_start));

            if (min_span > 0 && max_span > 0 &&
                (static_cast<float>(overlap_length) / min_span -
                 static_cast<float>(uncovered_length) / max_span) > mask_level &&
                uncovered_length <= mask_len)
            {
                const int32_t resolved_parent = (primary_region.parent >= 0)
                    ? primary_region.parent : primary_region.region_id;
                current_region.parent = resolved_parent;
                primary_region.secondary_score = std::max(
                    primary_region.secondary_score, current_region.score);
                // Update secondary_dp_max for MAPQ (equivalent to hit.c:176)
                if (current_region.has_alignment && primary_region.has_alignment &&
                    (primary_region.reference_id != current_region.reference_id
                     || primary_region.ref_start != current_region.ref_start
                     || primary_region.ref_end != current_region.ref_end
                     || overlap_length != min_span))
                {
                    primary_region.secondary_dp_max = std::max(
                        primary_region.secondary_dp_max, current_region.dp_max_score);
                    if (primary_region.dp_max_score - current_region.dp_max_score <= sub_diff) {
                        ++primary_region.secondary_count;
                    }
                } else if (current_region.seed_count >= primary_region.seed_count) {
                    ++primary_region.secondary_count;
                }
                is_secondary = true;
                break;
            }
        }

        if (!is_secondary) {
            primary_indices.push_back(region_idx);
            current_region.parent = static_cast<int32_t>(region_idx);
            current_region.secondary_count = 0;
        }
    }

    JustLog("[minimap_port] Classification: {} primary, {} secondary",
            primary_indices.size(), region_count - primary_indices.size());
}

// ============================================================================
// Phase 6: Filter secondary mappings (mm_select_sub equivalent)
// ============================================================================

/**
 * @brief Filter secondary alignments by score ratio.
 *
 * Equivalent to mm_select_sub() in hit.c:257-277.
 *
 * Keep a secondary if:
 *   - score >= parent.score * pri_ratio, OR
 *   - score + min_diff >= parent.score
 *   - up to best_n secondaries total
 */
[[nodiscard]] std::size_t filter_secondary_regions(
    std::span<AlignRegion> regions,
    float pri_ratio,
    int32_t min_diff,
    int32_t best_n)
{
    static const auto* _desc =
        bioseq::instr::register_region_static("mm_select_sub");
    auto _scope = bioseq::instr::region(_desc);
    if (regions.empty() || pri_ratio <= 0.0f) {
        return regions.size();
    }

    // Use index-based filtering to avoid reading from moved-from objects.
    // Mark each region as kept (true) or removed (false), then compact.
    const size_t region_count = regions.size();
    size_t secondary_kept_count = 0;
    std::vector<bool> keep(region_count, false);

    for (size_t region_idx = 0; region_idx < region_count; ++region_idx) {
        const auto& region = regions[region_idx];
        const int32_t parent_idx = region.parent;
        if (parent_idx == region.region_id) {
            // Primary — always keep
            keep[region_idx] = true;
        } else if (parent_idx >= 0 && parent_idx < static_cast<int32_t>(region_count)) {
            const auto& parent_region = regions[parent_idx];
            if ((region.score >= static_cast<int32_t>(parent_region.score * pri_ratio) ||
                 region.score + min_diff >= parent_region.score)
                && static_cast<int32_t>(secondary_kept_count) < best_n)
            {
                // Not identical coordinates
                const bool is_duplicate =
                    region.query_start == parent_region.query_start
                    && region.query_end == parent_region.query_end
                    && region.reference_id == parent_region.reference_id
                    && region.ref_start == parent_region.ref_start
                    && region.ref_end == parent_region.ref_end;
                if (!is_duplicate) {
                    keep[region_idx] = true;
                    ++secondary_kept_count;
                }
            }
        }
    }

    // Compact: move kept regions to front.  Span has no resize(); the caller
    // observes the new effective size via the returned value.  Regions past
    // `write_pos` are moved-from (empty CIGAR vector) — their destructors run
    // when the owning storage (arena slab or vector) unwinds.
    size_t write_pos = 0;
    for (size_t region_idx = 0; region_idx < region_count; ++region_idx) {
        if (keep[region_idx]) {
            if (write_pos < region_idx) {
                regions[write_pos] = std::move(regions[region_idx]);
            }
            ++write_pos;
        }
    }

    // Re-index the surviving prefix.
    for (size_t region_idx = 0; region_idx < write_pos; ++region_idx) {
        regions[region_idx].region_id = static_cast<int32_t>(region_idx);
    }

    JustLog("[minimap_port] After secondary filtering: {} regions remain", write_pos);
    return write_pos;
}

// ============================================================================
// Phase 7: Base-level alignment — dual-affine gap DP (KSW2 equivalent)
// ============================================================================

// generate_score_matrix is now ScoringMatrix<R>::generate in alignment_hwy_utils.

// ============================================================================
// KSW2 alignment wrapper
// ============================================================================

/**
 * @brief Dispatch to KSW2's dual-affine or affine gap alignment.
 *
 * Equivalent to mm_align_pair() in align.c:340-372.
 * If q == q2 && e == e2, uses affine gap (z2); otherwise dual-affine (d2).
 *
 * Accepts compact_sequence_range directly with sub-region indices so that
 * sequence encoding happens only inside the KSW2 core kernel.
 *
 * @tparam is_reversed  True for reverse-complement query strand
 * @tparam QueryRange   compact_sequence_range type for query
 * @tparam TargetRange  compact_sequence_range type for target
 * @tparam AlphabetDim  Scoring matrix dimension (default: alphabet_size + 1)
 */
template<
    bool is_reversed,
    supports_compact_sequence_range QueryRange,
    supports_compact_sequence_range TargetRange,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QueryRange>::traits::storage_unit>>
[[nodiscard]] KswAlignResult ksw2_align_pair(
    const bioseq::storage::lazy_compact_sequence<QueryRange, Scanner>& query_seq,
    const bioseq::storage::lazy_compact_sequence<TargetRange, Scanner>& target_seq,
    size_t query_start,
    size_t query_end,
    size_t target_start,
    size_t target_end,
    const ScoringMatrix<QueryRange>& scoring_matrix,
    const Minimap2Params& params,
    int align_bandwidth, int end_bonus, int zdrop, KswFlags ksw_flags)
{
    if (params.gap_open1 == params.gap_open2 && params.gap_ext1 == params.gap_ext2) {
        const KswZ2Params z2_params{
            .gap_open   = static_cast<int8_t>(params.gap_open1),
            .gap_extend = static_cast<int8_t>(params.gap_ext1),
            .band_width = align_bandwidth,
            .z_drop     = zdrop,
            .end_bonus  = end_bonus,
            .flags      = ksw_flags,
        };
        return HwyKsw2Aligner::z2_align<is_reversed>(
            query_seq, target_seq,
            query_start, query_end, target_start, target_end,
            scoring_matrix, z2_params);
    }
    const KswD2Params d2_params{
        .gap_open_short   = static_cast<int8_t>(params.gap_open1),
        .gap_extend_short = static_cast<int8_t>(params.gap_ext1),
        .gap_open_long    = static_cast<int8_t>(params.gap_open2),
        .gap_extend_long  = static_cast<int8_t>(params.gap_ext2),
        .band_width       = align_bandwidth,
        .z_drop           = zdrop,
        .end_bonus        = end_bonus,
        .flags            = ksw_flags,
    };
    return HwyKsw2Aligner::d2_align<is_reversed>(
        query_seq, target_seq,
        query_start, query_end, target_start, target_end,
        scoring_matrix, d2_params);
}

/**
 * @brief Dispatch to block aligner for single-affine gap alignment.
 *
 * Uses the first gap penalty pair (gap_open1, gap_ext1) only.
 * Block aligner does not support dual-affine gaps.
 */
template<
    bool is_reversed,
    supports_compact_sequence_range QueryRange,
    supports_compact_sequence_range TargetRange,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QueryRange>::traits::storage_unit>>
[[nodiscard]] KswAlignResult block_aligner_align_pair(
    const bioseq::storage::lazy_compact_sequence<QueryRange, Scanner>& query_seq,
    const bioseq::storage::lazy_compact_sequence<TargetRange, Scanner>& target_seq,
    size_t query_start,
    size_t query_end,
    size_t target_start,
    size_t target_end,
    const ScoringMatrix<QueryRange>& scoring_matrix,
    const Minimap2Params& params,
    [[maybe_unused]] int align_bandwidth,
    [[maybe_unused]] int end_bonus,
    int zdrop,
    KswFlags ksw_flags)
{
    BlockAlignerParams ba_params{
        .gap_open   = static_cast<int8_t>(params.gap_open1),
        .gap_extend = static_cast<int8_t>(params.gap_ext1),
        .min_size   = params.block_min_size,
        .max_size   = params.block_max_size,
        .x_drop     = ksw_flags.extz_only ? zdrop : -1,
        .flags      = ksw_flags,
    };
    return HwyBlockAligner::align<is_reversed>(
        query_seq, target_seq,
        query_start, query_end, target_start, target_end,
        scoring_matrix, ba_params);
}

// append_cigar is now in bioseq_cigar module.

/**
 * @brief Perform base-level alignment for each region.
 *
 * Equivalent to align_regs() -> mm_align_skeleton() -> mm_align1() in align.c.
 *
 * For each region, follows the minimap2 3-part alignment strategy:
 *   1. Left extension: reversed query/target from first seed to extended boundary
 *   2. Gap filling: align the entire region between first and last seeds
 *   3. Right extension: query/target from last seed to extended boundary
 *
 * Sequences are passed as compact_sequence_range references with sub-region
 * indices — no pre-encoding into uint8_t vectors is needed. The KSW2 core
 * kernel handles encoding of the specific regions it needs.
 *
 * @tparam base_alignment_algorithm  Compile-time selector between KSW2 and
 *                                   block aligner. Templated so the inner
 *                                   gap-fill branch is resolved at compile
 *                                   time and no runtime branch exists in the
 *                                   hot per-region loop.
 */
/**
 * @brief Single-phase aligner — left extension + gap fill + right extension.
 *
 * Materialises the three KSW2/block-aligner calls per region into a struct
 * (DOC0249 §10.1 R4: replace the `[&]<bool>()` capture-all template lambda
 * that previously lived inside `align_regions::for(auto& region : regions)`
 * with a non-capturing functor whose configuration is stored by value).
 * Inlines to the same instructions at -O3.
 *
 * `run<strand_reversed>` is templated on the strand because the alignment
 * kernels are themselves templated on the reverse-complement encoding bit
 * (left-extension still flips the CIGAR independently via `reverse_cigar`).
 */
template <BaseAlignmentAlgorithmType base_alignment_algorithm,
          supports_compact_sequence_range QueryRange,
          supports_compact_sequence_range TargetRange,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QueryRange>::traits::storage_unit>>
struct AlignOnePhase {
    const bioseq::storage::lazy_compact_sequence<QueryRange, Scanner>&  query_seq;
    const bioseq::storage::lazy_compact_sequence<TargetRange, Scanner>& target_seq;
    const ScoringMatrix<QueryRange>&           scoring_matrix;
    const Minimap2Params&                      params;
    int32_t                                    align_bandwidth;

    template <bool strand_reversed>
    [[gnu::always_inline]] void run(
        AlignRegion&            region,
        std::span<const Anchor> anchors,
        int32_t                 anchor_begin,
        std::int64_t            seed_ref_start,
        std::int64_t            seed_query_start,
        std::int64_t            seed_ref_end,
        std::int64_t            seed_query_end,
        std::int64_t            outer_ref_start,
        std::int64_t            outer_query_start,
        std::int64_t            outer_ref_end,
        std::int64_t            outer_query_end,
        std::int64_t            query_length) const
    {
        bool prior_phase_zdropped = false;

        // === 1. Left extension ===
        std::int64_t final_ref_start = seed_ref_start;
        std::int64_t final_query_start = seed_query_start;
        if (seed_query_start > outer_query_start && seed_ref_start > outer_ref_start) {
            const KswAlignResult left_result = ksw2_align_pair<strand_reversed>(
                query_seq, target_seq,
                static_cast<size_t>(outer_query_start),
                static_cast<size_t>(seed_query_start),
                static_cast<size_t>(outer_ref_start),
                static_cast<size_t>(seed_ref_start),
                scoring_matrix, params,
                align_bandwidth, params.end_bonus, params.zdrop,
                KswFlags{.right_align = true, .extz_only = true, .reverse_cigar = true});
            if (!left_result.cigar.empty()) {
                append_cigar(region.cigar, left_result.cigar);
                region.dp_score += left_result.max;
            }
            const int32_t left_ref_offset = left_result.reach_end
                ? left_result.mqe_t + 1 : left_result.max_t + 1;
            final_ref_start = seed_ref_start - left_ref_offset;
            const int32_t left_query_offset = left_result.reach_end
                ? seed_query_start - outer_query_start : left_result.max_q + 1;
            final_query_start = seed_query_start - left_query_offset;
            prior_phase_zdropped = left_result.zdropped;
        }

        // === 2. Gap filling — per-seed-pair walk ===
        std::int64_t final_ref_end = seed_ref_start;
        std::int64_t final_query_end = seed_query_start;
        if (region.seed_count > 1) {
            std::int64_t qs = seed_query_start;
            std::int64_t rs = seed_ref_start;
            bool zdropped_any = false;
            final_ref_end = seed_ref_start;
            final_query_end = seed_query_start;
            for (int32_t i = 1; i < region.seed_count; ++i) {
                const Anchor& cur_anchor = anchors[anchor_begin + i];
                const std::int64_t qe = static_cast<std::int64_t>(cur_anchor.query_position) + 1;
                const std::int64_t re = static_cast<std::int64_t>(cur_anchor.ref_position) + 1;
                const bool is_last = (i == region.seed_count - 1);
                const std::int64_t gap_query = qe - qs;
                const std::int64_t gap_ref   = re - rs;
                if (!is_last
                    && (gap_query < params.min_ksw_len
                        || gap_ref < params.min_ksw_len))
                {
                    continue;
                }
                if (gap_query <= 0 || gap_ref <= 0) {
                    qs = qe;
                    rs = re;
                    continue;
                }
                KswAlignResult fill_result;
                if constexpr (base_alignment_algorithm == BaseAlignmentAlgorithmType::BLOCK_ALIGNER) {
                    fill_result = block_aligner_align_pair<strand_reversed>(
                        query_seq, target_seq,
                        static_cast<size_t>(qs),
                        static_cast<size_t>(qe),
                        static_cast<size_t>(rs),
                        static_cast<size_t>(re),
                        scoring_matrix, params,
                        align_bandwidth, -1, params.zdrop,
                        KswFlags{.approx_max = true});
                } else {
                    fill_result = ksw2_align_pair<strand_reversed>(
                        query_seq, target_seq,
                        static_cast<size_t>(qs),
                        static_cast<size_t>(qe),
                        static_cast<size_t>(rs),
                        static_cast<size_t>(re),
                        scoring_matrix, params,
                        align_bandwidth, -1, params.zdrop,
                        KswFlags{.approx_max = true});
                }
                if (!fill_result.cigar.empty()) {
                    append_cigar(region.cigar, fill_result.cigar);
                }
                if (fill_result.zdropped) {
                    final_ref_end = rs + (fill_result.max_t + 1);
                    final_query_end = qs + (fill_result.max_q + 1);
                    region.dp_score += fill_result.max;
                    zdropped_any = true;
                    break;
                }
                final_ref_end = re;
                final_query_end = qe;
                region.dp_score += fill_result.score;
                qs = qe;
                rs = re;
            }
            prior_phase_zdropped = zdropped_any;
        } else {
            final_ref_end = seed_ref_end;
            final_query_end = seed_query_end;
        }

        // === 3. Right extension ===
        if (!prior_phase_zdropped
            && seed_query_end < outer_query_end
            && seed_ref_end < outer_ref_end)
        {
            const KswAlignResult right_result = ksw2_align_pair<strand_reversed>(
                query_seq, target_seq,
                static_cast<size_t>(seed_query_end),
                static_cast<size_t>(outer_query_end),
                static_cast<size_t>(seed_ref_end),
                static_cast<size_t>(outer_ref_end),
                scoring_matrix, params,
                align_bandwidth, params.end_bonus, params.zdrop,
                KswFlags{.extz_only = true});
            if (!right_result.cigar.empty()) {
                append_cigar(region.cigar, right_result.cigar);
                region.dp_score += right_result.max;
            }
            const int32_t right_ref_offset = right_result.reach_end
                ? right_result.mqe_t + 1 : right_result.max_t + 1;
            final_ref_end = seed_ref_end + right_ref_offset;
            const int32_t right_query_offset = right_result.reach_end
                ? outer_query_end - seed_query_end : right_result.max_q + 1;
            final_query_end = seed_query_end + right_query_offset;
        }

        // Set final coordinates from alignment extent (align.c:897-899).
        // Internal extent math is signed int64; the stored region fields are
        // sequence_size_t (non-negative absolute coordinates).
        region.ref_start = static_cast<sequence_size_t>(final_ref_start);
        region.ref_end = static_cast<sequence_size_t>(final_ref_end);
        if (!region.is_reverse) {
            region.query_start = static_cast<sequence_size_t>(final_query_start);
            region.query_end = static_cast<sequence_size_t>(final_query_end);
        } else {
            region.query_start = static_cast<sequence_size_t>(query_length - final_query_end);
            region.query_end = static_cast<sequence_size_t>(query_length - final_query_start);
        }
    }
};

template<
    BaseAlignmentAlgorithmType base_alignment_algorithm,
    supports_compact_sequence_range QueryRange,
    supports_compact_sequence_range TargetRange,
    class Scanner = bioseq::storage::sequence_scanner_none<
        typename compact_sequence_range<QueryRange>::traits::storage_unit>>
void align_regions(
    std::span<AlignRegion> regions,
    std::span<const Anchor> anchors,
    const bioseq::storage::lazy_compact_sequence<QueryRange, Scanner>& query_seq,
    const bioseq::storage::lazy_compact_sequence<TargetRange, Scanner>& target_seq,
    const ScoringMatrix<QueryRange>& scoring_matrix,
    const Minimap2Params& params)
{
    static const auto* _desc =
        bioseq::instr::register_region_static("mm_align_skeleton");
    auto _scope = bioseq::instr::region(_desc);
    // DOC0263 §10.2 — lengths are full 64-bit (no int32 truncation of bp_size).
    // Region coordinate math below is signed int64 so the extension/clamp logic
    // and the reverse-strand mirror keep working for sequences longer than 2^31.
    const std::int64_t query_length = static_cast<std::int64_t>(query_seq.bp_size());
    const std::int64_t target_length = static_cast<std::int64_t>(target_seq.bp_size());
    const int32_t align_bandwidth = static_cast<int32_t>(params.bandwidth * 1.5 + 1.0);

    // Per DOC0249 §10.1 R4: the `[&]<bool>()` capture-all template lambda
    // that used to live inside the per-region loop body is replaced by the
    // non-capturing functor `AlignOnePhase`.  Constructed once per region
    // batch with the kernel-invariant references; the per-region call
    // surface is `aligner.template run<is_reversed>(...)`.
    const AlignOnePhase<base_alignment_algorithm, QueryRange, TargetRange, Scanner> aligner{
        query_seq, target_seq, scoring_matrix, params, align_bandwidth};

    // DOC0276 — liveness heartbeat. A single big region (many gap-fills over a
    // multi-Mbp seed span) can dominate wall-clock; the per-phase milestones
    // alone would show a long silence with no sign of progress. Emit (and flush)
    // a heartbeat at most ~once/second so `tail -f` of the log shows which
    // region index is in flight and the live footprint. Gated on enable_logging
    // so production pays only the loop counter.
    std::int64_t hb_last_ns = enable_logging ? bioseq::instr::clock::now_ns() : 0;
    std::size_t  hb_region_idx = 0;
    for (auto& region : regions) {
        ++hb_region_idx;
        if (enable_logging) {
            const std::int64_t hb_now_ns = bioseq::instr::clock::now_ns();
            if (hb_now_ns - hb_last_ns > 1'000'000'000LL) {
                hb_last_ns = hb_now_ns;
                JustLogProgress("[minimap_port] aligning region {}/{} seed_count={}"
                        " | footprint={}MB avail={}MB",
                        hb_region_idx, regions.size(), region.seed_count,
                        bioseq::instr::detail::sysinfo::process_footprint_bytes() >> 20,
                        bioseq::instr::detail::sysinfo::system_available_bytes() >> 20);
            }
        }
        if (region.seed_count <= 0) {
            continue;
        }

        const int32_t anchor_begin = region.anchor_start;
        const Anchor& first_anchor = anchors[anchor_begin];
        const Anchor& last_anchor = anchors[anchor_begin + region.seed_count - 1];

        // Inner seed boundaries (where seeds are)
        const int32_t first_span = static_cast<int32_t>(first_anchor.query_span);
        std::int64_t seed_ref_start = static_cast<std::int64_t>(first_anchor.ref_position)
                               + 1 - first_span;
        std::int64_t seed_query_start = static_cast<std::int64_t>(first_anchor.query_position)
                                 + 1 - first_span;
        const std::int64_t seed_ref_end = static_cast<std::int64_t>(last_anchor.ref_position) + 1;
        const std::int64_t seed_query_end = static_cast<std::int64_t>(last_anchor.query_position) + 1;
        if (seed_ref_start < 0) { seed_ref_start = 0; }
        if (seed_query_start < 0) { seed_query_start = 0; }

        // Outer boundaries: extend beyond seeds (align.c:708-762 simplified).
        // `extension_len` is a bounded gap (<= params.max_gap) computed in int64
        // to match the int64 coordinates; the std::min args are matched in type.
        std::int64_t outer_ref_start = seed_ref_start;
        std::int64_t outer_query_start = seed_query_start;
        std::int64_t outer_ref_end = seed_ref_end;
        std::int64_t outer_query_end = seed_query_end;

        if (seed_query_start > 0 && seed_ref_start > 0) {
            std::int64_t extension_len = std::min<std::int64_t>(seed_query_start, params.max_gap);
            outer_query_start = seed_query_start - extension_len;
            const std::int64_t bonus_extension =
                (extension_len * params.match_score > params.gap_open1)
                    ? (extension_len * params.match_score - params.gap_open1) / params.gap_ext1
                    : 0;
            extension_len += bonus_extension;
            extension_len = std::min<std::int64_t>(extension_len, params.max_gap);
            extension_len = std::min(extension_len, seed_ref_start);
            outer_ref_start = seed_ref_start - extension_len;
        }
        if (seed_query_end < query_length && seed_ref_end < target_length) {
            std::int64_t extension_len = std::min<std::int64_t>(query_length - seed_query_end, params.max_gap);
            outer_query_end = seed_query_end + extension_len;
            const std::int64_t bonus_extension =
                (extension_len * params.match_score > params.gap_open1)
                    ? (extension_len * params.match_score - params.gap_open1) / params.gap_ext1
                    : 0;
            extension_len += bonus_extension;
            extension_len = std::min<std::int64_t>(extension_len, params.max_gap);
            extension_len = std::min(extension_len, target_length - seed_ref_end);
            outer_ref_end = seed_ref_end + extension_len;
        }
        if (outer_query_end > query_length) {
            outer_query_end = query_length;
        }

        // DOC0254 §10.3: pre-reserve CIGAR storage from outer extents so the
        // three-phase fill collapses the geometric doubling chain to one
        // allocation per region.  `outer_query_end - outer_query_start`
        // bounds the column count of the left-extension + gap-fill +
        // right-extension union; +16 absorbs the per-op rounding.  The
        // reserve is clamped to `4 * params.max_gap + 64` ops to prevent
        // pathological over-allocation on multi-Mbp seed spans (XXL_HIFI
        // workloads where the seeded query extent itself is megabases —
        // run-length-compressed CIGARs are typically 10–100× smaller than
        // the column count, so this cap stays a strict upper bound while
        // protecting the per-query allocator from gigabyte spikes).
        {
            const std::int64_t cigar_q_extent = outer_query_end - outer_query_start;
            const std::int64_t cigar_r_extent = outer_ref_end   - outer_ref_start;
            const std::int64_t cigar_q_clamped = cigar_q_extent > 0 ? cigar_q_extent : 0;
            const std::int64_t cigar_r_clamped = cigar_r_extent > 0 ? cigar_r_extent : 0;
            const std::size_t raw_estimate =
                static_cast<std::size_t>(cigar_q_clamped) +
                static_cast<std::size_t>(cigar_r_clamped) + 16u;
            const std::size_t cigar_reserve_cap =
                static_cast<std::size_t>(4 * params.max_gap) + 64u;
            region.cigar.reserve(std::min(raw_estimate, cigar_reserve_cap));
        }

        region.cigar.clear();
        region.dp_score = 0;

        // Per-region diagnostic logs were previously emitted on every region
        // boundary (left-extension skipped, gap-fill skipped, right-extension
        // skipped, "determine actions" entry). Each fmtlog enqueue + format
        // is ~1-5us, and the bench harness installs `enable_logging = true`
        // via `log_configure(true)` so they fire during measurement runs.
        // For very-long pairs with many regions this stacks up to ~ms of
        // overhead per timed sample. Removed — the same information can be
        // derived from `region.cigar.empty()` and the seed/outer span fields
        // by any debugger-driven introspection without polluting the
        // bench wall-clock. See REPORT_POOR_BENCHMARK_PERFORMANCE_AND_LOW_QUALITY_SIMD_MM_V_3.md.

        // Dispatch the three alignment phases based on strand orientation
        // via the non-capturing `aligner` functor (DOC0249 §10.1 R4).
        if (region.is_reverse) {
            aligner.template run<true>(
                region, anchors, anchor_begin,
                seed_ref_start, seed_query_start,
                seed_ref_end, seed_query_end,
                outer_ref_start, outer_query_start,
                outer_ref_end, outer_query_end,
                query_length);
        } else {
            aligner.template run<false>(
                region, anchors, anchor_begin,
                seed_ref_start, seed_query_start,
                seed_ref_end, seed_query_end,
                outer_ref_start, outer_query_start,
                outer_ref_end, outer_query_end,
                query_length);
        }

        // Compute dp_max_score from CIGAR (simplified mm_update_extra)
        region.dp_max_score = region.dp_score;
        region.has_alignment = true;

        // Compute match_length/block_length from CIGAR
        int32_t cigar_match_count = 0;
        int32_t cigar_block_count = 0;
        for (const auto& cigar_op : region.cigar) {
            const int32_t op_len = static_cast<int32_t>(cigar_op.length);
            if (cigar_op.opcode == CIGAR_MATCH || cigar_op.opcode == CIGAR_EQ) {
                cigar_match_count += op_len;
                cigar_block_count += op_len;
            } else if (cigar_op.opcode == CIGAR_X) {
                cigar_block_count += op_len;
            } else if (cigar_op.opcode == CIGAR_INS || cigar_op.opcode == CIGAR_DEL) {
                cigar_block_count += op_len;
            }
        }
        if (cigar_match_count > 0) {
            region.match_length = cigar_match_count;
            const int32_t effective_block = (cigar_block_count > 0)
                ? cigar_block_count : region.block_length;
            region.block_length = effective_block;
        }
    }

    JustLogProgress("[minimap_port] Aligned {} regions | footprint={}MB avail={}MB",
            regions.size(),
            bioseq::instr::detail::sysinfo::process_footprint_bytes() >> 20,
            bioseq::instr::detail::sysinfo::system_available_bytes() >> 20);
}

// ============================================================================
// Phase 8: Compute mapping quality (mm_set_mapq equivalent)
// ============================================================================

// ============================================================================
// Phase 7b: Estimate divergence from seeds (mm_est_err equivalent)
// ============================================================================

/**
 * @brief Estimate per-base divergence from seed positions.
 *
 * Equivalent of mm_est_err() in esterr.c.
 *
 * Algorithm:
 *   1. Build a sorted array of forward-strand query minimizer positions (mini_pos)
 *   2. For each region, binary-search for the first seed in mini_pos
 *   3. Scan forward through mini_pos, counting how many chain seeds match
 *   4. n_tot = span of mini_pos entries + edge corrections
 *   5. div = 1 - (n_match/n_tot)^(1/avg_k)
 *
 * @param regions           Alignment regions to update
 * @param query_minimizers  All query minimizers (used as mini_pos proxy)
 * @param anchors           Reordered anchor array
 * @param query_len         Query sequence length
 * @param kmer_size         k-mer size
 * @param target_len        Target/reference sequence length
 */
template <class MinimizerT>
void estimate_divergence_from_seeds(
    std::span<AlignRegion> regions,
    std::span<const MinimizerT> query_minimizers,
    std::span<const Anchor> anchors,
    sequence_size_t query_len,
    int32_t kmer_size,
    sequence_size_t target_len)
{
    static const auto* _desc =
        bioseq::instr::register_region_static("mm_est_err");
    auto _scope = bioseq::instr::region(_desc);
    if (query_minimizers.empty()) {
        return;
    }

    // ----------------------------------------------------------------------
    // V13: SORT-FREE per-position prefix-count for O(1) membership AND
    // O(1) range count. Replaces the V12 "build sorted mini_pos array +
    // binary-search-per-seed" path:
    //
    //   prefix_count[i] = #(query minimizers with last_pos < i)
    //
    // Then:
    //   * membership at position p:   prefix_count[p+1] > prefix_count[p]
    //   * #mini_pos in [a, b]:         prefix_count[b+1] - prefix_count[a]
    //
    // Build cost is O(N + query_len). For VERY_LONG (903 kbp) this is a
    // single ~1 M-iteration linear pass — vs V12's 5–6 ms `std::sort` on
    // 168 k entries (the SIMD V12 dual-stream collector emits in
    // lane-interleaved order, so the array is NOT sorted on output and
    // std::sort cannot detect already-sorted input). The V12 phase
    // breakdown's `mm_est_err = 7.96 ms` for SIMD-DP forward chains was
    // dominated by this sort, not the per-seed walk we previously thought.
    //
    // Bench (VERY_LONG, post-V13): mm_est_err per-call drops from 7.96 ms
    // to <2 ms (-75%) because the sort is gone and the per-seed walk hits
    // the cumulative array via two random-access loads (~3 ns each).
    //
    // V13 also drops the V10 reverse-chain search-range split: with the
    // direction-aware `compute_forward_pos(s)` lambda BOTH forward and
    // reverse chains have ASCENDING forward_pos for s=0..seed_count-1, so
    // the previous "search [begin, match_start_idx+1)" for reverse chains
    // was structurally wrong (last_seed_pos > first_forward_pos for both
    // directions — V10 fix was a no-op at best, broken-fast-path at worst).
    // ----------------------------------------------------------------------
    // The compact MinimapSketchData no longer carries a per-minimizer
    // span field — for dna4 (the only bench alphabet) every minimizer's
    // span is identically equal to the configured kmer size. The
    // legacy `span_sum / count` average therefore degenerates to the
    // constant `kmer_size`, which we use directly instead of touching
    // a now-removed bitfield.
    const float avg_kmer_span = static_cast<float>(kmer_size);

    // Build the prefix-count array in two linear passes:
    //   1. mini_pos_count[p + 1] += #(minimizers at last_pos == p)
    //   2. cumulative-sum: mini_pos_count[i] += mini_pos_count[i - 1]
    // Final invariant: mini_pos_count[i] = #(minimizers with last_pos < i)
    //
    // Size is (query_len + 2) so `mini_pos_count[query_len + 1]` is a
    // valid index (covers the case where last_pos == query_len, even
    // though the V12 collector's emit clamps to query_len - 1). Using
    // uint32_t supports up to 4 G minimizers — vastly above realistic
    // cell-genome scale.
    const size_t prefix_size = static_cast<size_t>(query_len) + 2;
    std::vector<uint32_t> mini_pos_count(prefix_size, 0);
    for (const auto& query_min : query_minimizers) {
        const int64_t pos = static_cast<int64_t>(query_min.last_pos());
        if (pos >= 0 && pos <= static_cast<int64_t>(query_len)) {
            ++mini_pos_count[static_cast<size_t>(pos) + 1];
        }
    }
    for (size_t i = 1; i < prefix_size; ++i) {
        mini_pos_count[i] += mini_pos_count[i - 1];
    }

    // Per DOC0249 §10.2 R4: the three `[&]` lambdas previously declared
    // here (`is_minimizer`, `range_count`, `compute_forward_pos`) are
    // converted to non-capturing struct functors so the file-wide grep gate
    // for capture-all lambdas under `bioseq/backend/pipeline/**.cppm` runs
    // clean.  Each functor stores its dependencies by value (no `&`); the
    // compiler treats them identically to inline methods at -O3 (M15).
    struct IsMinimizer {
        const std::uint32_t* mini_pos_count;
        sequence_size_t      query_len;
        [[nodiscard, gnu::always_inline]]
        bool operator()(std::int64_t pos) const noexcept {
            if (pos < 0 || static_cast<sequence_size_t>(pos) > query_len) return false;
            const std::size_t p = static_cast<std::size_t>(pos);
            return mini_pos_count[p + 1] > mini_pos_count[p];
        }
    };
    struct RangeCount {
        const std::uint32_t* mini_pos_count;
        sequence_size_t      query_len;
        [[nodiscard, gnu::always_inline]]
        int32_t operator()(std::int64_t a, std::int64_t b) const noexcept {
            if (a < 0) a = 0;
            if (static_cast<sequence_size_t>(b) > query_len) b = static_cast<std::int64_t>(query_len);
            if (a > b) return 0;
            return static_cast<int32_t>(
                mini_pos_count[static_cast<std::size_t>(b) + 1] -
                mini_pos_count[static_cast<std::size_t>(a)]);
        }
    };
    const IsMinimizer is_minimizer{mini_pos_count.data(), query_len};
    const RangeCount  range_count{mini_pos_count.data(), query_len};

    for (auto& region : regions) {
        region.divergence = -1.0f;
        if (region.seed_count <= 0) {
            continue;
        }

        const int32_t anchor_begin = region.anchor_start;

        // compute_forward_pos walks chain anchors in such order that
        // forward_pos values are MONOTONICALLY ASCENDING in `s`. For forward
        // chains: s -> anchor[s] in chain order (ascending query_position
        // == ascending forward_pos). For reverse chains: s -> anchor[cnt-1-s]
        // in REVERSE chain order; coupled with the y-flip on the
        // reverse-encoded query_position this yields ascending forward_pos.
        // (See esterr.c:50,57-59 — minimap2 walks reverse chains in the
        // same reverse order for the same reason.)
        //
        // Non-capturing struct functor per DOC0249 §10.2 R4: dependencies
        // (region orientation, anchor base, seed count, query length) are
        // stored by value.  The compiler inlines `operator()` identically
        // to the original `[&]` lambda at -O3 (M15).
        struct ComputeForwardPos {
            const Anchor* anchors_base;
            int32_t       anchor_begin;
            int32_t       seed_count;
            sequence_size_t query_len;
            bool          region_is_reverse;

            [[nodiscard, gnu::always_inline]]
            std::int64_t operator()(int32_t s) const noexcept {
                const Anchor& chain_anchor = region_is_reverse
                    ? anchors_base[anchor_begin + seed_count - 1 - s]
                    : anchors_base[anchor_begin + s];
                std::int64_t forward_pos = static_cast<std::int64_t>(chain_anchor.query_position);
                if (chain_anchor.is_reverse) {
                    const std::int64_t anchor_span =
                        static_cast<int32_t>(chain_anchor.query_span);
                    forward_pos = static_cast<std::int64_t>(query_len) - 1 - (forward_pos + 1 - anchor_span);
                }
                return forward_pos;
            }
        };
        const ComputeForwardPos compute_forward_pos{
            anchors.data(), anchor_begin, region.seed_count,
            query_len, region.is_reverse};

        // First chain anchor (s=0) — minimum forward_pos. esterr.c:50 uses
        // `r->rev ? last : first` which is the same anchor compute_forward_pos(0)
        // returns.
        const std::int64_t first_forward_pos = compute_forward_pos(0);
        if (!is_minimizer(first_forward_pos)) {
            continue;
        }

        // Walk chain anchors with O(1) prefix-count probe. Mirror minimap2's
        // halt-on-miss semantics (esterr.c:57-62 — once a chain anchor is
        // not found in mini_pos, the C `for (...; ++j)` loop advances j to
        // the end without ever advancing k, so subsequent chain anchors
        // are not counted). For our pipeline every chain anchor is by
        // construction a query minimizer (collect_seed_hits looks each one
        // up in the target index), so the loop normally walks all
        // seed_count entries.
        int32_t seed_match_count = 1;
        std::int64_t last_match_pos = first_forward_pos;
        for (int32_t walk_idx = 1; walk_idx < region.seed_count; ++walk_idx) {
            const std::int64_t fp = compute_forward_pos(walk_idx);
            if (!is_minimizer(fp)) {
                break;
            }
            last_match_pos = fp;
            ++seed_match_count;
        }

        // n_tot = span of mini_pos between first and last match (esterr.c:61),
        // counting duplicates (cumulative-sum array preserves them).
        int32_t total_minimizer_span = range_count(first_forward_pos, last_match_pos);

        // Edge corrections (esterr.c:62-63).
        if (region.query_start > avg_kmer_span && region.ref_start > avg_kmer_span) {
            ++total_minimizer_span;
        }
        if (query_len - region.query_start > avg_kmer_span
            && target_len - region.ref_end > avg_kmer_span)
        {
            ++total_minimizer_span;
        }

        // Divergence formula (esterr.c:64).
        if (seed_match_count >= total_minimizer_span) {
            region.divergence = 0.0f;
        } else {
            const double match_ratio = static_cast<double>(seed_match_count)
                                     / total_minimizer_span;
            region.divergence = static_cast<float>(
                1.0 - std::pow(match_ratio, 1.0 / avg_kmer_span));
        }
    }
}

// ============================================================================
// Phase 8: Compute mapping quality (mm_set_mapq equivalent)

/**
 * @brief Compute mapping quality for all regions.
 *
 * Equivalent to mm_set_mapq2() in hit.c:423-476.
 *
 * MAPQ formula (primary with DP scores):
 *   mapq = identity * pen_cm * 40 * (1 - ratio^2) * ln(dp_max_score / match_sc)
 *   mapq -= 4.343 * ln(secondary_count + 1)
 *   mapq = clamp(mapq, 0, 60)
 *
 * Where ratio = secondary_dp_max * secondary_chain_score / (dp_max_score * original_score)
 */
void compute_mapping_quality(
    std::span<AlignRegion> regions,
    const Minimap2Params& params,
    int32_t rep_len)
{
    static const auto* _desc =
        bioseq::instr::register_region_static("mm_set_mapq");
    auto _scope = bioseq::instr::region(_desc);
    constexpr float quality_coefficient = 40.0f;
    const int32_t match_sc = params.match_score;
    const int32_t min_chain_sc = params.min_chain_score;

    // Compute uniqueness ratio
    int64_t primary_score_sum = 0;
    for (const auto& region : regions) {
        if (region.parent == region.region_id) {
            primary_score_sum += region.score;
        }
    }
    // hit.c:436: uniq_ratio = (float)sum_sc / (sum_sc + rep_len)
    const float uniqueness_ratio = static_cast<float>(primary_score_sum) /
                                   static_cast<float>(primary_score_sum + rep_len);

    for (auto& region : regions) {
        if (region.parent != region.region_id) {
            region.mapq = 0; // secondaries get 0
            continue;
        }

        // Primary mapping
        const float score_factor = (region.score > 100) ? 1.0f : 0.01f * region.score;
        const float score_penalty = score_factor * uniqueness_ratio;
        const float raw_seed_penalty = (region.seed_count > 10)
            ? 1.0f : 0.1f * region.seed_count;
        const float seed_count_penalty = std::min(score_penalty, raw_seed_penalty);

        const int32_t effective_secondary_score = std::max(
            region.secondary_score, min_chain_sc);

        int32_t quality_value;
        // hit.c:447-467: three branches based on DP availability
        if (region.has_alignment && region.secondary_dp_max > 0
            && region.dp_max_score > 0)
        {
            // Formula A: with DP scores (hit.c:448-457)
            const float identity = (region.block_length > 0)
                ? static_cast<float>(region.match_length) / region.block_length
                : 0.f;
            const float score_ratio =
                static_cast<float>(region.secondary_dp_max) * effective_secondary_score
                / (static_cast<float>(region.dp_max_score) * region.original_score);
            quality_value = static_cast<int32_t>(
                identity * seed_count_penalty * quality_coefficient
                * (1.0f - score_ratio * score_ratio)
                * std::log(static_cast<float>(region.dp_max_score) / match_sc));

            // BWA-MEM-like alternative (hit.c:455)
            const int32_t quality_alt = static_cast<int32_t>(
                6.02f * identity * identity
                * (region.dp_max_score - region.secondary_dp_max) / match_sc + 0.499f);
            quality_value = std::min(quality_value, quality_alt);
        } else {
            // No secondary_dp_max available: use chain score formula (hit.c:460-467)
            const float score_ratio = static_cast<float>(effective_secondary_score)
                                    / region.original_score;
            if (region.has_alignment) {
                const float identity = (region.block_length > 0)
                    ? static_cast<float>(region.match_length) / region.block_length
                    : 0.f;
                quality_value = static_cast<int32_t>(
                    identity * seed_count_penalty * quality_coefficient
                    * (1.0f - score_ratio)
                    * std::log(static_cast<float>(region.dp_max_score) / match_sc));
            } else {
                // No DP at all — use chain score (hit.c:466)
                quality_value = static_cast<int32_t>(
                    seed_count_penalty * quality_coefficient
                    * (1.0f - score_ratio)
                    * std::log(static_cast<float>(region.score)));
            }
        }

        // Subtract secondary_count penalty (hit.c:469)
        quality_value -= static_cast<int32_t>(
            4.343f * std::log(static_cast<float>(region.secondary_count + 1)) + 0.499f);
        quality_value = std::max(quality_value, 0);
        region.mapq = std::min(quality_value, 60);

        // Ensure non-zero if clearly best (hit.c:472)
        if (region.has_alignment && region.dp_max_score > region.secondary_dp_max
            && region.mapq == 0)
        {
            region.mapq = 1;
        }
    }

    JustLog("[minimap_port] Computed MAPQ for {} regions", regions.size());
}

// ============================================================================
// Phase 9: Convert to MatchResult (PAF output)
// ============================================================================

/**
 * @brief Convert internal AlignRegion to PAF-like MatchResult.
 *
 * Fields map to PAF columns as documented in mm_write_paf4() (format.c:427-460).
 */
[[nodiscard]] MatchResult region_to_match_result(
    AlignRegion&& region,
    sequence_size_t query_len,
    sequence_size_t target_len)
{
    return MatchResult{
        .query_len             = query_len,
        .query_start           = region.query_start,
        .query_end             = region.query_end,
        .is_reverse            = region.is_reverse,
        .target_len            = target_len,
        .target_start          = region.ref_start,
        .target_end            = region.ref_end,
        .num_matches           = region.match_length,
        .block_length          = region.block_length,
        .mapq                  = region.mapq,
        .chain_score           = region.score,
        .dp_score              = region.dp_score,
        .dp_max_score          = region.dp_max_score,
        .seed_count            = region.seed_count,
        .secondary_chain_score = region.secondary_score,
        .divergence            = region.divergence,
        .is_primary            = (region.parent == region.region_id),
        .has_cigar             = region.has_alignment,
        .cigar                 = std::move(region.cigar),
    };
}

// ============================================================================
// PAF output — MatchResultCollection
// ============================================================================

// CIGAR_OP_CHAR and format_cigar are now in bioseq_cigar module.

/**
 * @brief Owning container of alignment results with PAF output formatting.
 *
 * Wraps a std::vector<MatchResult> and provides a print() method that outputs
 * results in exact minimap2 PAF format, reproducing mm_write_paf4()
 * (format.c:427-460) with auxiliary tags from write_tags() (format.c:399-425).
 */
struct MatchResultCollection {
    std::vector<MatchResult> results;

    MatchResultCollection() = default;
    explicit MatchResultCollection(std::vector<MatchResult>&& results_vec)
        : results(std::move(results_vec)) {}

    [[nodiscard]] size_t size() const { return results.size(); }
    [[nodiscard]] bool empty() const { return results.empty(); }

    [[nodiscard]] std::span<const MatchResult> view() const {
        return std::span<const MatchResult>(results);
    }

    /**
     * @brief Print alignment results in exact minimap2 PAF format.
     *
     * PAF line format (tab-separated):
     *   col 1:  query name             -- user-provided
     *   col 2:  query sequence length
     *   col 3:  query start (0-based, closed)
     *   col 4:  query end   (0-based, open)
     *   col 5:  relative strand '+' or '-'
     *   col 6:  target name            -- user-provided
     *   col 7:  target sequence length
     *   col 8:  target start on original strand (0-based)
     *   col 9:  target end   on original strand (0-based)
     *   col 10: number of residue matches  (match_length)
     *   col 11: alignment block length     (block_length)
     *   col 12: mapping quality (0-255)
     *
     * Auxiliary tags: NM, ms, AS, nn, tp, cm, s1, s2, de/dv, rl, cg
     *
     * @param query_name  Query sequence name (col 1)
     * @param target_name Target sequence name (col 6)
     */
    void print(const std::string& query_name  = "query",
               const std::string& target_name = "target") const
    {
        if (results.empty()) {
            JustLog("{}\t0\t0\t0\t*\t*\t0\t0\t0\t0\t0\t0", query_name);
            return;
        }

        for (const auto& match : results) {
            const char strand_char = match.is_reverse ? '-' : '+';
            const char type_char = match.is_primary ? 'P' : 'S';

            std::string line = fmt::format(
                "{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
                query_name, match.query_len, match.query_start, match.query_end,
                strand_char, target_name, match.target_len, match.target_start,
                match.target_end, match.num_matches, match.block_length, match.mapq);

            if (match.has_cigar && !match.cigar.empty()) {
                // NM = blen - mlen; both are sequence_size_t (uint64) and
                // blen >= mlen, but compute signed + clamp defensively.
                const std::int64_t edit_distance = std::max<std::int64_t>(0,
                    static_cast<std::int64_t>(match.block_length)
                  - static_cast<std::int64_t>(match.num_matches));
                line += fmt::format("\tNM:i:{}\tms:i:{}\tAS:i:{}\tnn:i:0",
                    edit_distance, match.dp_max_score, match.dp_score);
            }

            line += fmt::format("\ttp:A:{}", type_char);
            line += fmt::format("\tcm:i:{}", match.seed_count);
            line += fmt::format("\ts1:i:{}", match.chain_score);

            if (match.is_primary) {
                line += fmt::format("\ts2:i:{}", match.secondary_chain_score);
            }

            if (match.divergence >= 0.0f && match.divergence <= 1.0f) {
                const char* divergence_tag = (match.has_cigar && !match.cigar.empty())
                    ? "\tde:f:" : "\tdv:f:";
                if (match.divergence == 0.0f) {
                    line += divergence_tag;
                    line += '0';
                } else {
                    line += fmt::format("{}{:.4f}", divergence_tag,
                        static_cast<double>(match.divergence));
                }
            }

            line += "\trl:i:0";

            if (match.has_cigar && !match.cigar.empty()) {
                line += "\tcg:Z:";
                line += format_cigar(match.cigar);
            }

            JustLog("{}", line);
        }
    }
};

/**
 * @brief Print alignment results in exact minimap2 PAF format.
 *
 * Free function wrapper for backwards compatibility. Delegates to
 * MatchResultCollection::print().
 */
void print_match_results(
    const MatchResultCollection& collection,
    const std::string& query_name  = "query",
    const std::string& target_name = "target")
{
    collection.print(query_name, target_name);
}


} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

} // export
