/**
 * @file minimap_port.cppm
 * @brief C++26 implementation of the minimap2-compatible mapping pipeline.
 *
 * This module implements the core minimap2 algorithm starting AFTER minimizer
 * collection (sketch generation). It takes precomputed minimizers and produces
 * PAF-like alignment results.
 *
 * High-level flow:
 *   1. Build a simple hash index from target minimizers
 *   2. Collect seed hits: look up query minimizers in the target index
 *   3. chain seeds using dynamic programming (mg_lchain_dp equivalent)
 *   4. Convert chains to alignment regions (mm_gen_regs equivalent)
 *   5. Classify primary/secondary mappings (mm_set_parent equivalent)
 *   6. Filter secondary mappings (mm_select_sub equivalent)
 *   7. Perform base-level alignment using dual-affine gap DP (KSW2 equivalent)
 *   8. Compute mapping quality (mm_set_mapq equivalent)
 *   9. Return results as match_result (PAF-like records)
 *
 * The implementation keeps biological coordinates explicit and routes
 * minimizer extraction, seed chaining, prefiltering, and base alignment
 * through the selected scalar, SIMD, or Vulkan backend without changing the
 * minimap2 scoring model.
 */
module;

#include <algorithm>
#include <array>
#include <bit>
#include <boost/unordered/unordered_flat_map.hpp>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

export module bioseq.pipeline.minimap_port;

import bioseq.common.logging;
export import bioseq.highway.dispatch_contract;
export import bioseq.prefilter.execution_contract;
import bioseq.common.seqan_adapter;
import bioseq.common.memory_budget;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.two_stack_minimizers;
import bioseq.sequences.cigar;
import bioseq.alignment;
import bioseq.alignment.utils;
import bioseq.alignment.block_aligner;
import bioseq.chaining.core;
import bioseq.chaining.scalar;
import bioseq.chaining.simd;
import bioseq.chaining.rmq;
import bioseq.common.context;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.instrumentation;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_index;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.parallelism.pipeline_ops;

export {

    namespace bioseq::simd {

    // Every length and absolute coordinate uses the canonical storage size so no
    // position is silently truncated for sequences longer than 2^31 (C1).
    using bioseq::storage::sequence_size_t;

    // Import canonical chaining values from their shared owner.
    using bioseq::chaining::anchor;
    using bioseq::chaining::backtrack_chains;
    using bioseq::chaining::chain;
    using bioseq::chaining::chain_anchors_rmq;
    using bioseq::chaining::chain_anchors_scalar;
    using bioseq::chaining::chaining_mode;
    using bioseq::chaining::chaining_parameters;
    using bioseq::chaining::compute_chaining_score;
    using bioseq::chaining::simd_chaining_policy;
    using bioseq::chaining::simd_chaining_policy_name;

    // Pipeline code consumes the canonical minimizer-index concepts directly.
    using bioseq::indexing::lookup_minimizer_spans;
    using bioseq::indexing::minimizer_hashmap_fragment_index;
    using bioseq::indexing::minimizer_lookup_index;
    using bioseq::indexing::minimizer_lookup_spans;
    using bioseq::indexing::minimizer_position_span;
    using bioseq::indexing::minimizer_seed_index;

    } // namespace bioseq::simd

    namespace bioseq::simd {

    /// Return the stable display name of a chaining algorithm.
    [[nodiscard]] constexpr const char* chaining_algorithm_name(chaining_algorithm mode) {
        switch (mode) {
        case chaining_algorithm::scalar_minimap2:
            return "Minimap2 Scalar";
        case chaining_algorithm::simd_minimap2:
            return "Minimap2 SIMD";
        case chaining_algorithm::range_minimum_query:
            return "Minimap2 RMQ";
        case chaining_algorithm::vulkan_tiled_dynamic_programming:
            return "Vulkan tiled DP";
        }
        return "unknown";
    }

    /// Return the stable display name of a base-alignment algorithm.
    [[nodiscard]] constexpr const char*
    base_alignment_algorithm_name(base_alignment_algorithm mode) {
        switch (mode) {
        case base_alignment_algorithm::ksw2:
            return "KSW2";
        case base_alignment_algorithm::block_aligner:
            return "Block Aligner";
        case base_alignment_algorithm::ksw2_batched:
            return "KSW2 (inter-region batched)";
        }
        return "unknown";
    }

    /// Return the stable display name of a minimizer algorithm.
    [[nodiscard]] constexpr const char* minimizer_algorithm_name(minimizer_algorithm mode) {
        switch (mode) {
        case minimizer_algorithm::scalar:
            return "Scalar Minimizers";
        case minimizer_algorithm::simd_minimap2_reference:
            return "SIMD Minimap2 (slow reference)";
        case minimizer_algorithm::two_stack_nthash:
            return "Two-Stack nthash SIMD Minimizers";
        case minimizer_algorithm::vulkan_minimap_integer:
            return "Vulkan minimap-int minimizers";
        case minimizer_algorithm::bounded_nthash_simd:
            return "Bounded u32 ntHash SIMD Minimizers";
        }
        return "unknown";
    }

    // ============================================================================
    // Constants — mirrors minimap2 defaults from mm_mapopt_t
    // ============================================================================

    // CIGAR opcodes and cigar_operation are imported from bioseq_cigar module.

    /**
     * @brief PAF-like alignment result.
     *
     * Maps to the 12 mandatory PAF columns plus alignment details.
     */
    struct match_result {
        // PAF mandatory columns.: every length / absolute coordinate
        // (cols 2,3,4,7,8,9,10,11) is `sequence_size_t` (uint64) so it is never
        // truncated for sequences longer than 2^31. PAF emits them as decimal
        // strings, so the wire width is unchanged.
        sequence_size_t query_len = 0;    ///< col 2: query sequence length
        sequence_size_t query_start = 0;  ///< col 3: query start (0-based, closed)
        sequence_size_t query_end = 0;    ///< col 4: query end (0-based, open)
        bool is_reverse = false;          ///< col 5: '+' or '-'
        sequence_size_t target_len = 0;   ///< col 7: target sequence length
        sequence_size_t target_start = 0; ///< col 8: target start on original strand
        sequence_size_t target_end = 0;   ///< col 9: target end on original strand
        sequence_size_t num_matches = 0;  ///< col 10: number of residue matches (mlen)
        sequence_size_t block_length = 0; ///< col 11: alignment block length (blen)
        int32_t mapq = 0;                 ///< col 12: mapping quality (0-255)

        // Additional alignment info
        int32_t chain_score = 0;            ///< chaining score
        int32_t dp_score = 0;               ///< DP alignment score
        int32_t dp_max_score = 0;           ///< max scoring segment score (dp_max0 in original)
        int32_t seed_count = 0;             ///< number of seeds in chain
        int32_t secondary_chain_score = 0;  ///< best secondary chaining score against this primary
        float divergence = -1.f;            ///< sequence divergence estimate
        bool is_primary = true;             ///< primary mapping flag
        bool has_cigar = false;             ///< whether CIGAR alignment was performed
        std::vector<cigar_operation> cigar; ///< CIGAR string
        sequence_size_t edit_distance = 0;  ///< exact unit-cost edits in the reported alignment
    };

    /// Exact, truncation-resistant fingerprint of one complete match_result stream.
    /// Storage/index checksums deliberately do not feed this value: this is
    /// biological-result identity and is comparable across independently executed
    /// pipeline routes.  The two lanes use independent byte-mixing recurrences and
    /// every field is encoded explicitly, so struct padding and host endianness are
    /// never part of the stats.
    struct match_result_set_identity {
        std::uint64_t collection_count = 0u;
        std::uint64_t match_result_count = 0u;
        std::uint64_t cigar_result_count = 0u;
        std::uint64_t cigar_operation_count = 0u;
        std::uint64_t match_digest_lane0 = 0u;
        std::uint64_t match_digest_lane1 = 0u;
        std::uint64_t cigar_digest_lane0 = 0u;
        std::uint64_t cigar_digest_lane1 = 0u;
        bool is_computed = false;
        bool is_saturated = false;

        /// Merge one complete result-collection identity into this aggregate.
        void merge(const match_result_set_identity& other) noexcept {
            if (!other.is_computed) {
                // A missing collection stats is a permanent aggregate failure.
                // `is_computed == false && is_saturated == false` is reserved for the
                // pristine accumulator so a later valid collection cannot erase
                // the failure recorded for an earlier collection.
                is_computed = false;
                is_saturated = true;
                return;
            }
            if (!is_computed) {
                if (is_saturated)
                    return;
                *this = other;
                return;
            }
            const auto add = [this](std::uint64_t& destination, std::uint64_t increment) noexcept {
                if (increment > (std::numeric_limits<std::uint64_t>::max)() - destination) {
                    destination = (std::numeric_limits<std::uint64_t>::max)();
                    is_saturated = true;
                } else {
                    destination += increment;
                }
            };
            const std::uint64_t other_match_count = other.match_result_count;
            match_digest_lane0 = std::rotl(match_digest_lane0, 19) ^ other.match_digest_lane0 ^
                                 (other_match_count * 0x9e3779b97f4a7c15ull);
            match_digest_lane1 = std::rotl(match_digest_lane1, 37) ^ other.match_digest_lane1 ^
                                 (other_match_count * 0xbf58476d1ce4e5b9ull);
            cigar_digest_lane0 = std::rotl(cigar_digest_lane0, 23) ^ other.cigar_digest_lane0 ^
                                 (other.cigar_operation_count * 0x94d049bb133111ebull);
            cigar_digest_lane1 = std::rotl(cigar_digest_lane1, 41) ^ other.cigar_digest_lane1 ^
                                 (other.cigar_operation_count * 0xd6e8feb86659fd93ull);
            add(collection_count, other.collection_count);
            add(match_result_count, other.match_result_count);
            add(cigar_result_count, other.cigar_result_count);
            add(cigar_operation_count, other.cigar_operation_count);
            is_saturated = is_saturated || other.is_saturated;
        }

        [[nodiscard]] constexpr bool
        operator==(const match_result_set_identity&) const noexcept = default;
    };

    namespace biological_identity_detail {

    /// Two-lane byte digest used for architecture-independent result identity.
    struct biological_digest_state {
        std::uint64_t lane0 = 0xcbf29ce484222325ull;
        std::uint64_t lane1 = 0x6a09e667f3bcc909ull;

        /// Mix one byte into both digest lanes.
        constexpr void add_byte(std::uint8_t byte) noexcept {
            lane0 ^= byte;
            lane0 *= 0x100000001b3ull;

            lane1 ^= static_cast<std::uint64_t>(byte) + 0x9e3779b97f4a7c15ull + (lane1 << 6u) +
                     (lane1 >> 2u);
            lane1 = std::rotl(lane1, 17) * 0xbf58476d1ce4e5b9ull;
        }

        /// Mix an unsigned 64-bit value in little-endian byte order.
        constexpr void add_u64(std::uint64_t value) noexcept {
            for (std::uint32_t shift = 0u; shift != 64u; shift += 8u) {
                add_byte(static_cast<std::uint8_t>(value >> shift));
            }
        }

        /// Mix an unsigned 32-bit value through the canonical integer encoding.
        constexpr void add_u32(std::uint32_t value) noexcept {
            add_u64(value);
        }

        /// Mix a signed 32-bit value by its bit representation.
        constexpr void add_i32(std::int32_t value) noexcept {
            add_u32(std::bit_cast<std::uint32_t>(value));
        }

        /// Mix one canonical boolean byte.
        constexpr void add_bool(bool value) noexcept {
            add_byte(value ? std::uint8_t{1u} : std::uint8_t{0u});
        }

        /// Mix a floating-point value by its bit representation.
        constexpr void add_float(float value) noexcept {
            add_u32(std::bit_cast<std::uint32_t>(value));
        }
    };

    } // namespace biological_identity_detail

    /// Compute a complete identity for one ordered alignment-result collection.
    [[nodiscard]] inline match_result_set_identity
    make_match_result_set_identity(std::span<const match_result> results) noexcept {
        biological_identity_detail::biological_digest_state match_digest{};
        biological_identity_detail::biological_digest_state cigar_digest{};
        match_result_set_identity stats{
            .collection_count = 1u,
            .match_result_count = static_cast<std::uint64_t>(results.size()),
            .is_computed = true,
        };

        match_digest.add_u64(stats.match_result_count);
        cigar_digest.add_u64(stats.match_result_count);
        for (const match_result& result : results) {
            match_digest.add_byte(0xa5u);
            match_digest.add_u64(result.query_len);
            match_digest.add_u64(result.query_start);
            match_digest.add_u64(result.query_end);
            match_digest.add_bool(result.is_reverse);
            match_digest.add_u64(result.target_len);
            match_digest.add_u64(result.target_start);
            match_digest.add_u64(result.target_end);
            match_digest.add_u64(result.num_matches);
            match_digest.add_u64(result.block_length);
            match_digest.add_i32(result.mapq);
            match_digest.add_i32(result.chain_score);
            match_digest.add_i32(result.dp_score);
            match_digest.add_i32(result.dp_max_score);
            match_digest.add_i32(result.seed_count);
            match_digest.add_i32(result.secondary_chain_score);
            match_digest.add_float(result.divergence);
            match_digest.add_bool(result.is_primary);
            match_digest.add_bool(result.has_cigar);
            match_digest.add_u64(result.edit_distance);
            match_digest.add_u64(static_cast<std::uint64_t>(result.cigar.size()));

            cigar_digest.add_byte(0x5au);
            cigar_digest.add_bool(result.has_cigar);
            cigar_digest.add_u64(static_cast<std::uint64_t>(result.cigar.size()));
            if (result.has_cigar)
                ++stats.cigar_result_count;
            const std::uint64_t cigar_size = static_cast<std::uint64_t>(result.cigar.size());
            if (cigar_size >
                (std::numeric_limits<std::uint64_t>::max)() - stats.cigar_operation_count) {
                stats.cigar_operation_count = (std::numeric_limits<std::uint64_t>::max)();
                stats.is_saturated = true;
            } else {
                stats.cigar_operation_count += cigar_size;
            }
            for (const cigar_operation& operation : result.cigar) {
                match_digest.add_u32(operation.length);
                match_digest.add_u32(operation.opcode);
                cigar_digest.add_u32(operation.length);
                cigar_digest.add_u32(operation.opcode);
            }
        }
        cigar_digest.add_u64(stats.cigar_result_count);
        cigar_digest.add_u64(stats.cigar_operation_count);
        stats.match_digest_lane0 = match_digest.lane0;
        stats.match_digest_lane1 = match_digest.lane1;
        stats.cigar_digest_lane0 = cigar_digest.lane0;
        stats.cigar_digest_lane1 = cigar_digest.lane1;
        return stats;
    }

    // ============================================================================
    // Internal data structures
    // ============================================================================
    // Anchor and chain ownership resides in bioseq.chaining.core.

    // Alignment-region and logarithm helpers are owned by their alignment and
    // chaining modules.

    // ============================================================================
    // Stage 1: Build target index from target minimizers
    // ============================================================================

    // Target index types and lookup concepts are owned by the indexing modules.

    // ============================================================================
    // Stage 2: Collect seed hits — look up query minimizers in target index
    // ============================================================================

    /**
     * Append one borrowed packed target-position run for a single query
     * minimizer. Position width is resolved once by the caller, so a hash-major
     * uint32 posting remains packed through the seed loop and is widened only in
     * the register used to construct its anchor.
     */
    template <std::unsigned_integral p_stored_position>
    inline void
    append_seed_hits_from_position_span(std::span<const p_stored_position> target_hits,
                                        std::uint32_t query_strand_bit,
                                        sequence_size_t forward_query_pos,
                                        sequence_size_t reverse_query_pos,
                                        std::uint32_t query_span_value,
                                        ::bioseq::chaining::arena_slab<anchor>& anchors_out) {
        for (const p_stored_position stored_target_hit : target_hits) {
            const std::uint64_t target_hit = static_cast<std::uint64_t>(stored_target_hit);
            const sequence_size_t ref_position = static_cast<sequence_size_t>(target_hit >> 1u);
            const std::uint32_t target_strand_bit = static_cast<std::uint32_t>(target_hit & 1u);
            const std::uint32_t is_reverse_bit = target_strand_bit ^ query_strand_bit;
            const sequence_size_t selected_query_pos =
                is_reverse_bit ? reverse_query_pos : forward_query_pos;

            anchor anchor;
            anchor.ref_position = ref_position;
            anchor.is_reverse = is_reverse_bit;
            anchor.query_position = selected_query_pos;
            anchor.query_span = static_cast<std::uint8_t>(query_span_value);
            anchor.flags = 0;
            anchor.segment_id = 0;
            anchors_out.push_back(anchor);
        }
    }

    /// Sort seed anchors into canonical reference/query order.
    inline void sort_seed_hits(::bioseq::chaining::arena_slab<anchor>& anchors_out) {
        static const auto* _sort_desc =
            bioseq::instrumentation::register_static_region("collect_seed_hits_sort");
        auto _sort_scope = bioseq::instrumentation::region(_sort_desc);
        if (anchors_out.size() < 2u)
            return;
        (void)::bioseq::chaining::sort_canonical_anchors(
            std::span<anchor>{anchors_out.data(), anchors_out.size()});
    }

    /**
     * @brief Collect seed anchors by looking up query minimizers in the target index.
     *
     * Equivalent to collect_seed_hits() in map.c:518-554.
     *
     * For each query minimizer, look up its hash in the target index. For each
     * hit, create an anchor encoding both reference and query positions.
     *
     * anchor encoding (same as minimap2):
     *   ref_bits: is_reverse<<63 | reference_id<<32 | ref_position
     *   query_bits: query_span<<32 | query_position
     *
     * Where for forward strand: ref_position = target_pos, query_position = query_pos
     * For reverse strand: set is_reverse bit, adjust query_position for reverse complement.
     *
     * @param query_minimizers Precomputed query minimizers
     * @param target_index     Target index to search
     * @param query_len        Length of query sequence
     * @param max_occ          Max occurrences to keep (filter high-frequency seeds)
     * @param kmer_size        K-mer length used for the encoded anchor span.
     * @param anchors_out      Output slab — sorted anchors are appended here.
     *                          Must be a live arena_slab rooted in the caller's
     *                          per-query `Arena::chain` scope. The kernel writes
     *                          directly into it via `push_back` and sorts in
     *                          place before returning.
     */
    template <class p_minimizer, minimizer_lookup_index p_index>
    void collect_seed_hits(std::span<const p_minimizer> query_minimizers,
                           const p_index& target_index,
                           sequence_size_t query_len,
                           int32_t max_occ,
                           int32_t kmer_size,
                           ::bioseq::chaining::arena_slab<anchor>& anchors_out) {
        static const auto* _seed_hits_desc =
            bioseq::instrumentation::register_static_region("collect_seed_hits");
        auto _seed_hits_scope = bioseq::instrumentation::region(_seed_hits_desc);
        // The caller is expected to pre-reserve the slab using the
        // `estimated_anchor_count` helper from `bioseq::simd::pipe::detail`
        // — that estimate is tighter than `query_minimizers.size() * 2` for very-short
        // sequences (clamped by `shared_ceiling`) and identical for the common case.
        // We still issue a defensive `reserve` here so the kernel works when invoked
        // directly with a fresh slab; the call is a no-op when `cap_` already covers
        // the requested capacity (see `arena_slab::reserve` short-circuit).
        anchors_out.reserve(query_minimizers.size() * 2);

        // Target-index lookup is the hot per-minimizer operation. We wrap the
        // whole loop (not each lookup) to avoid profiler overhead dominating
        // the measurement; the loop body also classifies/sorts anchors.
        {
            static const auto* _idx_lookup_desc =
                bioseq::instrumentation::register_static_region("target_index_lookup");
            auto _idx_lookup_scope = bioseq::instrumentation::region(_idx_lookup_desc);
            // On the dna4 benchmark path every minimizer span equals the configured
            // kmer size, so hoist that constant outside the lookup loop.
            const uint32_t query_span_value = static_cast<uint32_t>(kmer_size);
            // Precompute the constant added to the negated forward_query_pos in the
            // reverse-strand branch:
            //   reverse_query_pos = query_len - (fwd + 1 - span) - 1
            //                     = (query_len - 2 + span) - fwd
            // Hoisting this addition out of the inner loop lets the compiler keep
            // (sub fwd) as the only per-hit op for the reverse branch.
            const sequence_size_t reverse_pos_base = query_len - 2 + query_span_value;
            if (max_occ < 0) [[unlikely]] {
                throw std::invalid_argument("collect_seed_hits requires a nonnegative max_occ");
            }
            for (const auto& query_minimizer : query_minimizers) {
                const minimizer_lookup_spans target_hits =
                    lookup_minimizer_spans(target_index, query_minimizer.kmer());
                const std::uint64_t global_occurrence_count = target_hits.global_occurrence_count();
                if (global_occurrence_count == 0 ||
                    global_occurrence_count > static_cast<std::uint64_t>(max_occ)) {
                    continue;
                }
                if (!target_hits.positions_complete() || !target_hits.well_formed()) {
                    throw bioseq::indexing::persisted_minimizer_lookup_error(
                        "persisted minimizer positions unavailable or inconsistent "
                        "after global occurrence pruning");
                }

                // `pos_strand_encoded()` returns `(last_pos << 1) | strand` — the
                // exact form `lookup()` packed into the positions array, so the
                // per-hit `target_hit` lives in the same encoded space.
                // — keep the FULL 64-bit encoded value. `pos_bits` is
                // `ceil(log2(seq_len))` (up to 32 for a multi-Gbp sequence), so
                // `(last_pos << 1) | strand` spans up to 33 bits; the old
                // `static_cast<uint32_t>(...)` truncated any position >= 2^31. The
                // query is small here, but mirroring the target-side fix below keeps
                // both sides 64-bit-correct.
                const uint64_t query_pos_encoded = query_minimizer.pos_strand_encoded();
                // Hoist the strand bit out of the inner per-hit loop — constant
                // across `target_hits`.
                const uint32_t query_strand_bit = static_cast<uint32_t>(query_pos_encoded & 1ULL);
                const sequence_size_t forward_query_pos =
                    static_cast<sequence_size_t>(query_pos_encoded >> 1);
                // Reverse-strand position formula reduces to a single subtraction
                // once `reverse_pos_base` is hoisted; pre-compute here so the
                // branchless `csel` below picks between two register-resident
                // 64-bit positions. (forward_query_pos is a k-mer start <= query_len-k,
                // so reverse_pos_base - forward_query_pos never underflows.)
                const sequence_size_t reverse_query_pos = reverse_pos_base - forward_query_pos;

                const auto contiguous32 = target_hits.contiguous_positions32();
                const auto contiguous64 = target_hits.contiguous_positions();
                if (!contiguous32.empty()) {
                    append_seed_hits_from_position_span(contiguous32,
                                                        query_strand_bit,
                                                        forward_query_pos,
                                                        reverse_query_pos,
                                                        query_span_value,
                                                        anchors_out);
                } else if (!contiguous64.empty()) {
                    append_seed_hits_from_position_span(contiguous64,
                                                        query_strand_bit,
                                                        forward_query_pos,
                                                        reverse_query_pos,
                                                        query_span_value,
                                                        anchors_out);
                } else {
                    for (const minimizer_position_span& target_span :
                         target_hits.segmented_positions()) {
                        append_seed_hits_from_position_span(target_span.positions,
                                                            query_strand_bit,
                                                            forward_query_pos,
                                                            reverse_query_pos,
                                                            query_span_value,
                                                            anchors_out);
                    }
                }
            }
        } // end target_index_lookup scope

        // Establish the canonical `(ref_bits, query_bits, full query_position)`
        // order. The primary reference-key pass remains the exact port of
        // minimap2's `radix_sort_128x(a, a + n_a)` from misc.c; equal-reference
        // runs are then comparison-sorted by `sort_seed_hits`. The
        // 8-bit-per-pass MSD radix sort is faster than libc++ pdqsort by
        // ~3-5x at the anchor populations we see (50 k - 1 M+ at LONG/XL/XXL)
        // because it bypasses the comparison-based branch-predictor
        // pessimisation pdqsort hits when the ref_position prefix is
        // already monotonic — most of the work is just the 256-bucket
        // counting scan, which the M1 LSU pipelines well.
        //
        // A SIMD-vectorised bit-by-bit MSD radix variant (Bramas / Möller /
        // Keller, 2024 thesis) uses Highway's `CompressBlendedStore`. On
        // NEON-128 / Apple M1 the SIMD partition
        // is restricted to 2 u64 lanes per vector (vs. 8 on AVX-512) and
        // each NEON BlendedStore lowers to a 3-step Load+Blend+Store —
        // measured as 5–22 percentage points slower than this 8-bit MSD
        // radix sort on the benchmark's LONG/VLONG/XL/XXL buckets. The SIMD
        // variant becomes attractive again on AVX-512 hosts.
        sort_seed_hits(anchors_out);

        bioseq_log_lazy<logging_level::debug>([&anchors_out, &query_minimizers] {
            bioseq_log_progress<logging_level::debug>(
                "[minimap_port] Collected {} seed anchors from {} query minimizers"
                " | footprint={}MB avail={}MB",
                anchors_out.size(),
                query_minimizers.size(),
                bioseq::common::memory::process_footprint_bytes() >> 20,
                bioseq::common::memory::system_available_bytes() >> 20);
        });
    }

    // ============================================================================
    // Stage 3: Chaining through the selected chaining module
    // ============================================================================
    // compute_chaining_score is imported from bioseq.chaining.core
    // chain_anchors_scalar is imported from bioseq.chaining.scalar
    // chain_anchors_simd is reached through the selected fixed-target table.
    // chain_anchors_rmq is imported from bioseq.chaining.rmq

    // ============================================================================
    // Stage 4: Convert chains to alignment regions (mm_gen_regs equivalent)
    // ============================================================================

    /**
     * @brief Compute seeded match length and block length for a region.
     *
     * Equivalent to mm_cal_fuzzy_len() in hit.c.
     *
     * match_length: approximate number of matching bases in seeded regions
     * block_length: approximate total aligned length in seeded regions
     */
    void compute_fuzzy_lengths(alignment_region& region, std::span<const anchor> anchors) {
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

        for (int32_t anchor_idx = anchor_begin + 1; anchor_idx < anchor_begin + region.seed_count;
             ++anchor_idx) {
            const int32_t span = static_cast<int32_t>(anchors[anchor_idx].query_span);
            const int32_t target_gap = static_cast<int32_t>(anchors[anchor_idx].ref_position -
                                                            anchors[anchor_idx - 1].ref_position);
            const int32_t query_gap = static_cast<int32_t>(anchors[anchor_idx].query_position -
                                                           anchors[anchor_idx - 1].query_position);
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
    void set_region_coordinates(alignment_region& region,
                                std::span<const anchor> anchors,
                                sequence_size_t query_len) {
        if (region.seed_count <= 0) {
            return;
        }
        const int32_t anchor_begin = region.anchor_start;
        const anchor& first_anchor = anchors[anchor_begin];
        const anchor& last_anchor = anchors[anchor_begin + region.seed_count - 1];

        region.is_reverse = first_anchor.is_reverse != 0;
        region.reference_id = 0; // : single-target codebase, reference_id always 0

        // Reference coordinates. The absolute positions are full 64-bit; the
        // intermediate `position + 1 - span` can be negative, so it is computed in
        // signed int64 and clamped before being stored in the sequence_size_t field.
        const std::int64_t first_span = static_cast<int32_t>(first_anchor.query_span);
        std::int64_t ref_start =
            static_cast<std::int64_t>(first_anchor.ref_position) + 1 - first_span;
        const std::int64_t ref_end = static_cast<std::int64_t>(last_anchor.ref_position) + 1;
        if (ref_start < 0) {
            ref_start = 0;
        }
        region.ref_start = static_cast<sequence_size_t>(ref_start);
        region.ref_end = static_cast<sequence_size_t>(ref_end);

        // Query coordinates
        std::int64_t raw_query_start =
            static_cast<std::int64_t>(first_anchor.query_position) + 1 - first_span;
        const std::int64_t raw_query_end =
            static_cast<std::int64_t>(last_anchor.query_position) + 1;
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
     * Sorts chains by score (descending) and creates alignment_region records
     * with coordinates derived from the anchor positions.
     *
     * @param chains       Chains (sorted in place by score descending here).
     * @param anchors      Reordered anchor span — `chains[i].anchor_start`
     *                      indexes into this span.
     * @param query_len    Length of the query sequence (used to flip query
     *                      coordinates for reverse-strand chains).
     * @param regions_out  Output slab — one alignment_region per chain is constructed
     *                      via placement-new (so the slab's destructor releases
     *                      `region.cigar` correctly on unwind).  Must be a fresh
     *                      arena_slab. The kernel calls `reserve_for_placement_new`
     *                      and `construct_at_back` once per chain.
     */
    void generate_regions(std::span<chain> chains,
                          std::span<const anchor> anchors,
                          sequence_size_t query_len,
                          ::bioseq::chaining::arena_slab<alignment_region>& regions_out) {
        static const auto* _desc = bioseq::instrumentation::register_static_region("mm_gen_regs");
        auto _scope = bioseq::instrumentation::region(_desc);
        if (chains.empty()) {
            return;
        }

        /// Order chains by descending score without capturing local state.
        struct chain_score_descending {
            [[nodiscard, gnu::always_inline]] inline bool
            operator()(const chain& left, const chain& right) const noexcept {
                return left.score > right.score;
            }
        };
        std::sort(chains.begin(), chains.end(), chain_score_descending{});

        regions_out.reserve_for_placement_new(chains.size());
        for (size_t region_idx = 0; region_idx < chains.size(); ++region_idx) {
            alignment_region& region = regions_out.construct_at_back(alignment_region{
                .region_id = static_cast<int32_t>(region_idx),
                .parent = -1,
                .score = chains[region_idx].score,
                .original_score = chains[region_idx].score,
                .seed_count = chains[region_idx].anchor_count,
                .anchor_start = chains[region_idx].anchor_start,
                .divergence = -1.0f,
            });
            set_region_coordinates(region, anchors, query_len);
        }

        bioseq_log_lazy<logging_level::debug>([&regions_out] {
            bioseq_log_progress<logging_level::debug>(
                "[minimap_port] Generated {} alignment regions | footprint={}MB avail={}MB",
                regions_out.size(),
                bioseq::common::memory::process_footprint_bytes() >> 20,
                bioseq::common::memory::system_available_bytes() >> 20);
        });
    }

    // ============================================================================
    // Stage 5: Primary/secondary classification (mm_set_parent equivalent)
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
    void classify_primary_secondary(std::span<alignment_region> regions,
                                    float mask_level,
                                    int32_t mask_len,
                                    int32_t sub_diff) {
        static const auto* _desc = bioseq::instrumentation::register_static_region("mm_set_parent");
        auto _scope = bioseq::instrumentation::region(_desc);
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
            alignment_region& current_region = regions[region_idx];
            const int32_t current_query_start = current_region.query_start;
            const int32_t current_query_end = current_region.query_end;
            int32_t uncovered_length = 0;

            // Compute coverage from existing primaries
            coverage_intervals.clear();
            for (const size_t primary_idx : primary_indices) {
                const alignment_region& primary_region = regions[primary_idx];
                int32_t primary_query_start = primary_region.query_start;
                int32_t primary_query_end = primary_region.query_end;
                if (primary_query_end <= current_query_start ||
                    primary_query_start >= current_query_end) {
                    continue;
                }
                primary_query_start = std::max(primary_query_start, current_query_start);
                primary_query_end = std::min(primary_query_end, current_query_end);
                coverage_intervals.push_back((static_cast<uint64_t>(primary_query_start) << 32) |
                                             static_cast<uint32_t>(primary_query_end));
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
                alignment_region& primary_region = regions[primary_idx];
                const int32_t primary_query_start = primary_region.query_start;
                const int32_t primary_query_end = primary_region.query_end;
                if (primary_query_end <= current_query_start ||
                    primary_query_start >= current_query_end) {
                    continue;
                }

                const int32_t min_span = std::min(primary_query_end - primary_query_start,
                                                  current_query_end - current_query_start);
                const int32_t max_span = std::max(primary_query_end - primary_query_start,
                                                  current_query_end - current_query_start);
                const int32_t overlap_length =
                    std::max(0,
                             std::min(current_query_end, primary_query_end) -
                                 std::max(current_query_start, primary_query_start));

                if (min_span > 0 && max_span > 0 &&
                    (static_cast<float>(overlap_length) / min_span -
                     static_cast<float>(uncovered_length) / max_span) > mask_level &&
                    uncovered_length <= mask_len) {
                    const int32_t resolved_parent = (primary_region.parent >= 0)
                                                        ? primary_region.parent
                                                        : primary_region.region_id;
                    current_region.parent = resolved_parent;
                    primary_region.secondary_score =
                        std::max(primary_region.secondary_score, current_region.score);
                    // Update secondary_dp_max for MAPQ (equivalent to hit.c:176)
                    if (current_region.has_alignment && primary_region.has_alignment &&
                        (primary_region.reference_id != current_region.reference_id ||
                         primary_region.ref_start != current_region.ref_start ||
                         primary_region.ref_end != current_region.ref_end ||
                         overlap_length != min_span)) {
                        primary_region.secondary_dp_max =
                            std::max(primary_region.secondary_dp_max, current_region.dp_max_score);
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

        bioseq_log<logging_level::debug>("[minimap_port] Classification: {} primary, {} secondary",
                                         primary_indices.size(),
                                         region_count - primary_indices.size());
    }

    // ============================================================================
    // Stage 6: Filter secondary mappings (mm_select_sub equivalent)
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
    [[nodiscard]] std::size_t filter_secondary_regions(std::span<alignment_region> regions,
                                                       float pri_ratio,
                                                       int32_t min_diff,
                                                       int32_t best_n) {
        static const auto* _desc = bioseq::instrumentation::register_static_region("mm_select_sub");
        auto _scope = bioseq::instrumentation::region(_desc);
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
                     region.score + min_diff >= parent_region.score) &&
                    static_cast<int32_t>(secondary_kept_count) < best_n) {
                    // Not identical coordinates
                    const bool is_duplicate = region.query_start == parent_region.query_start &&
                                              region.query_end == parent_region.query_end &&
                                              region.reference_id == parent_region.reference_id &&
                                              region.ref_start == parent_region.ref_start &&
                                              region.ref_end == parent_region.ref_end;
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

        bioseq_log<logging_level::debug>(
            "[minimap_port] After secondary filtering: {} regions remain", write_pos);
        return write_pos;
    }

    // ============================================================================
    // Stage 7: Base-level alignment — dual-affine gap DP (KSW2 equivalent)
    // ============================================================================

    // Score-matrix generation is owned by the alphabet-specialized matrix.

    // ============================================================================
    // KSW2 alignment wrapper
    // ============================================================================

    /// Position of one scalar KSW2 fill within an alignment region.
    enum class scalar_ksw2_stage : std::uint8_t {
        /// Query and target prefix before the first seed.
        left,
        /// Gap between consecutive seeds.
        gap,
        /// Query and target suffix after the final seed.
        right,
    };

    /// Positive, operation-originated stats for scalar CPU dimensions in a route:
    /// scalar minimizers, CPU seed lookup, scalar chaining/backtracking, and KSW2
    /// extension. Hybrid routes may populate only the scalar dimensions they
    /// execute. SIMD/Block/Vulkan stats objects remain separate.
    struct scalar_pipeline_execution_stats {
        scalar_minimizer_execution_stats query_minimizers{};
        scalar_minimizer_execution_stats target_minimizers{};
        std::uint64_t seed_lookup_calls = 0;
        std::uint64_t seed_query_minimizers = 0;
        // A stage-level receipt written only after the selected scalar chaining
        // kernel returns successfully.  It is distinct from score/backtrack work:
        // a valid negative call can complete with zero anchors and execute neither.
        std::uint64_t chain_stage_completion_calls = 0;
        std::uint64_t chain_stage_completed_anchor_inputs = 0;
        std::uint64_t chain_score_calls = 0;
        std::uint64_t chain_backtrack_calls = 0;
        std::uint64_t chain_anchor_inputs = 0;
        std::uint64_t ksw2_left_calls = 0;
        std::uint64_t ksw2_gap_calls = 0;
        std::uint64_t ksw2_right_calls = 0;
        // Rectangle area admitted to real KSW2 calls. This is not a claim that a
        // banded/z-drop kernel visited every cell; it proves positive DP input.
        std::uint64_t ksw2_input_cell_area = 0;
        bool is_saturated = false;

        /// Add a counter while retaining overflow as a sticky flag.
        static constexpr void add(std::uint64_t& destination,
                                  const std::uint64_t increment,
                                  bool& saturated_flag) noexcept {
            scalar_minimizer_execution_stats::add(destination, increment, saturated_flag);
        }

        /// Record one completed scalar seed lookup.
        constexpr void record_seed_lookup(const std::uint64_t query_minimizer_count) noexcept {
            add(seed_lookup_calls, 1u, is_saturated);
            add(seed_query_minimizers, query_minimizer_count, is_saturated);
        }

        /// Record scalar score generation and backtracking for one anchor set.
        constexpr void record_scalar_chain(const std::uint64_t anchor_count) noexcept {
            add(chain_score_calls, 1u, is_saturated);
            add(chain_backtrack_calls, 1u, is_saturated);
            add(chain_anchor_inputs, anchor_count, is_saturated);
        }

        /// Record completion of the scalar chaining stage, including empty input.
        constexpr void
        record_scalar_chain_stage_completion(const std::uint64_t anchor_count) noexcept {
            add(chain_stage_completion_calls, 1u, is_saturated);
            add(chain_stage_completed_anchor_inputs, anchor_count, is_saturated);
        }

        /// Record one real KSW2 fill and its input rectangle.
        constexpr void record_ksw2_call(const scalar_ksw2_stage stage,
                                        const std::uint64_t query_symbols,
                                        const std::uint64_t target_symbols) noexcept {
            switch (stage) {
            case scalar_ksw2_stage::left:
                add(ksw2_left_calls, 1u, is_saturated);
                break;
            case scalar_ksw2_stage::gap:
                add(ksw2_gap_calls, 1u, is_saturated);
                break;
            case scalar_ksw2_stage::right:
                add(ksw2_right_calls, 1u, is_saturated);
                break;
            }
            if (query_symbols != 0 &&
                target_symbols > (std::numeric_limits<std::uint64_t>::max)() / query_symbols) {
                ksw2_input_cell_area = (std::numeric_limits<std::uint64_t>::max)();
                is_saturated = true;
                return;
            }
            add(ksw2_input_cell_area, query_symbols * target_symbols, is_saturated);
        }

        /// Merge independently completed scalar-route statistics.
        constexpr void merge(const scalar_pipeline_execution_stats& other) noexcept {
            query_minimizers.merge(other.query_minimizers);
            target_minimizers.merge(other.target_minimizers);
            bool overflow = is_saturated || other.is_saturated || query_minimizers.saturated ||
                            target_minimizers.saturated;
            add(seed_lookup_calls, other.seed_lookup_calls, overflow);
            add(seed_query_minimizers, other.seed_query_minimizers, overflow);
            add(chain_stage_completion_calls, other.chain_stage_completion_calls, overflow);
            add(chain_stage_completed_anchor_inputs,
                other.chain_stage_completed_anchor_inputs,
                overflow);
            add(chain_score_calls, other.chain_score_calls, overflow);
            add(chain_backtrack_calls, other.chain_backtrack_calls, overflow);
            add(chain_anchor_inputs, other.chain_anchor_inputs, overflow);
            add(ksw2_left_calls, other.ksw2_left_calls, overflow);
            add(ksw2_gap_calls, other.ksw2_gap_calls, overflow);
            add(ksw2_right_calls, other.ksw2_right_calls, overflow);
            add(ksw2_input_cell_area, other.ksw2_input_cell_area, overflow);
            is_saturated = overflow;
        }

        /// Validate the complete all-scalar tuple. Hybrid routes validate only the
        /// scalar dimensions they selected at their route-stats boundary.
        [[nodiscard]] constexpr bool
        is_route_consistent(const bool require_target_sketch,
                            const bool require_alignment) const noexcept {
            const bool is_target_valid =
                require_target_sketch ? target_minimizers.is_coherent()
                                      : target_minimizers == scalar_minimizer_execution_stats{};
            const bool is_alignment_valid =
                !require_alignment ||
                ((ksw2_left_calls != 0 || ksw2_gap_calls != 0 || ksw2_right_calls != 0) &&
                 ksw2_input_cell_area > 0);
            const bool empty_chain_completed = chain_stage_completed_anchor_inputs == 0 &&
                                               chain_anchor_inputs == 0 && chain_score_calls == 0 &&
                                               chain_backtrack_calls == 0;
            const bool nonempty_chain_completed =
                chain_stage_completed_anchor_inputs > 0 &&
                chain_anchor_inputs == chain_stage_completed_anchor_inputs &&
                chain_score_calls > 0 && chain_score_calls == chain_backtrack_calls &&
                chain_score_calls <= chain_stage_completion_calls;
            return !is_saturated && query_minimizers.is_coherent() && is_target_valid &&
                   seed_lookup_calls > 0 && seed_query_minimizers > 0 &&
                   chain_stage_completion_calls > 0 &&
                   (empty_chain_completed || nonempty_chain_completed) && is_alignment_valid;
        }

        /// Compare every scalar execution counter and state flag.
        constexpr bool operator==(const scalar_pipeline_execution_stats&) const noexcept = default;
    };

    /// Return whether Block Aligner can represent the selected affine penalties.
    [[nodiscard]] constexpr bool
    block_aligner_native_penalties_representable(const minimap2_parameters& params) noexcept {
        return params.gap_open1 > 0 && params.gap_ext1 > 0 &&
               params.gap_open1 <= std::numeric_limits<int8_t>::max() &&
               params.gap_ext1 <= std::numeric_limits<int8_t>::max() &&
               params.gap_open1 + params.gap_ext1 <=
                   -static_cast<int>(std::numeric_limits<int8_t>::min());
    }

    /// Project pipeline scoring parameters into one Block Aligner stage.
    [[nodiscard]] inline block_aligner_parameters
    project_block_aligner_params(const minimap2_parameters& params,
                                 block_alignment_stage stage) noexcept {
        const bool is_extension = stage != block_alignment_stage::gap_extension;
        return block_aligner_parameters{
            .gap_open = static_cast<int8_t>(params.gap_open1),
            .gap_extend = static_cast<int8_t>(params.gap_ext1),
            .minimum_block_size = params.block_min_size,
            .maximum_block_size = params.block_max_size,
            .x_drop_threshold = is_extension ? params.zdrop : 0,
            .mode = is_extension ? block_alignment_mode::x_drop : block_alignment_mode::global,
            .input_order = stage == block_alignment_stage::left_extension
                               ? block_input_order::reversed
                               : block_input_order::forward,
            .stage = stage,
            .is_trace_enabled = params.do_cigar || params.compute_edit_distance,
            .should_compute_edit_distance = params.compute_edit_distance,
        };
    }

    /**
     * Run one scalar KSW2 fill over lazy query and target subranges.
     *
     * @tparam p_is_reversed Whether the query is reverse-complement encoded.
     * @tparam p_alphabet Logical alphabet shared by both sequences.
     */
    template <bool p_is_reversed, sequence_alphabet p_alphabet>
    [[nodiscard]] ksw_alignment_result
    ksw2_align_pair(bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                    bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                    size_t query_start,
                    size_t query_end,
                    size_t target_start,
                    size_t target_end,
                    const scoring_matrix<p_alphabet>& scoring_matrix,
                    const alignment_kernels<p_alphabet>& alignment_kernels,
                    const minimap2_parameters& params,
                    int align_bandwidth,
                    int end_bonus,
                    int zdrop,
                    ksw_flags ksw_flags,
                    block_aligner_execution_stats* forbidden_block_stats = nullptr,
                    scalar_pipeline_execution_stats* scalar_execution_stats = nullptr,
                    scalar_ksw2_stage scalar_stage = scalar_ksw2_stage::gap) {
        if (forbidden_block_stats != nullptr) {
            abort_forbidden_ksw2_fallback(*forbidden_block_stats);
        }
        if (scalar_execution_stats != nullptr) {
            scalar_execution_stats->record_ksw2_call(
                scalar_stage,
                static_cast<std::uint64_t>(query_end - query_start),
                static_cast<std::uint64_t>(target_end - target_start));
        }
        if (params.gap_open1 == params.gap_open2 && params.gap_ext1 == params.gap_ext2) {
            const ksw_single_affine_parameters z2_params{
                .gap_open = static_cast<int8_t>(params.gap_open1),
                .gap_extend = static_cast<int8_t>(params.gap_ext1),
                .band_width = align_bandwidth,
                .z_drop = zdrop,
                .end_bonus = end_bonus,
                .flags = ksw_flags,
            };
            return alignment_kernels.align_ksw2_single_affine(p_is_reversed,
                                                              query_seq,
                                                              target_seq,
                                                              query_start,
                                                              query_end,
                                                              target_start,
                                                              target_end,
                                                              scoring_matrix,
                                                              z2_params);
        }
        const ksw_dual_affine_parameters d2_params{
            .gap_open_short = static_cast<int8_t>(params.gap_open1),
            .gap_extend_short = static_cast<int8_t>(params.gap_ext1),
            .gap_open_long = static_cast<int8_t>(params.gap_open2),
            .gap_extend_long = static_cast<int8_t>(params.gap_ext2),
            .band_width = align_bandwidth,
            .z_drop = zdrop,
            .end_bonus = end_bonus,
            .flags = ksw_flags,
        };
        return alignment_kernels.align_ksw2_dual_affine(p_is_reversed,
                                                        query_seq,
                                                        target_seq,
                                                        query_start,
                                                        query_end,
                                                        target_start,
                                                        target_end,
                                                        scoring_matrix,
                                                        d2_params);
    }

    /**
     * @brief Project the native Block Aligner contract for one minimap stage.
     *
     * This is deliberately not a KSW2-compatible adapter. The reference Block
     * Aligner has a single affine recurrence, adaptive block geometry, global
     * mode, and its own two-confirmation X-drop mode. Consequently only the
     * short affine pair and the Block tunables are projected from minimap2_parameters:
     *
     *   - gap fill: reference Global mode, forward input;
     *   - left extension: reference XDrop mode, reversed input;
     *   - right extension: reference XDrop mode, forward input.
     *
     * Dormant KSW-only defaults in the shared parameter object (bandwidth,
     * right-align, end-bonus, the second affine pair, and approximate-max) do not
     * invalidate a native Block run and are not included in its stats identity.
     * They are intentionally absent from this function's call surface, so a
     * caller that claims those semantics gets a compile-time contract error
     * instead of a KSW2 or scalar fallback.
     */
    template <bool p_is_reversed,
              bool p_enable_diagonal_certificate = true,
              sequence_alphabet p_alphabet>
    [[nodiscard]] ksw_alignment_result
    block_aligner_align_pair(bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                             bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                             size_t query_start,
                             size_t query_end,
                             size_t target_start,
                             size_t target_end,
                             const scoring_matrix<p_alphabet>& scoring_matrix,
                             const alignment_kernels<p_alphabet>& alignment_kernels,
                             const minimap2_parameters& params,
                             block_alignment_stage stage,
                             block_aligner_execution_stats* execution_stats) {
        const bool is_extension = stage != block_alignment_stage::gap_extension;
        if (!block_aligner_native_penalties_representable(params) ||
            (is_extension && params.zdrop < 0)) {
            if (execution_stats != nullptr) {
                execution_stats->add(execution_stats->unsupported_contract_rejections);
            }
            bioseq_log<logging_level::error>(
                "Block Aligner native parameter projection rejected: q={}, e={}, "
                "zdrop={}, stage={}",
                params.gap_open1,
                params.gap_ext1,
                params.zdrop,
                static_cast<unsigned>(stage));
            std::abort();
        }
        const block_aligner_parameters ba_params = project_block_aligner_params(params, stage);
        return alignment_kernels.align_block(p_is_reversed,
                                             p_enable_diagonal_certificate,
                                             query_seq,
                                             target_seq,
                                             query_start,
                                             query_end,
                                             target_start,
                                             target_end,
                                             scoring_matrix,
                                             ba_params,
                                             execution_stats,
                                             /*construction_stats=*/nullptr);
    }

    /// Expected counts for the three native Block Aligner fill positions.
    struct block_alignment_eligibility {
        /// Expected left-extension calls.
        std::uint64_t left = 0;
        /// Expected seed-to-seed gap-fill calls.
        std::uint64_t gap = 0;
        /// Expected right-extension calls.
        std::uint64_t right = 0;
    };

    /// Geometry of one mandatory seed-to-seed global fill.  Left/right
    /// extensions are deliberately excluded: they are local/X-drop domains and
    /// therefore cannot support a no-false-reject edit-threshold prefilter.
    struct mandatory_alignment_gap_window {
        /// Inclusive query start.
        sequence_size_t query_start = 0;
        /// Exclusive query end.
        sequence_size_t query_end = 0;
        /// Inclusive target start.
        sequence_size_t target_start = 0;
        /// Exclusive target end.
        sequence_size_t target_end = 0;
    };

    /**
     * Visit every positive seed-to-seed fill required by one alignment region.
     *
     * @tparam p_callback Callable accepting a mandatory gap window.
     */
    template <class p_callback>
    [[gnu::always_inline]] bool for_each_mandatory_alignment_gap(const alignment_region& region,
                                                                 std::span<const anchor> anchors,
                                                                 const minimap2_parameters& params,
                                                                 p_callback&& callback) {
        if (region.seed_count <= 1 || region.anchor_start < 0 ||
            static_cast<std::size_t>(region.anchor_start) >= anchors.size()) {
            return true;
        }
        const std::int32_t anchor_begin = region.anchor_start;
        const anchor& first_anchor = anchors[anchor_begin];
        const std::int64_t first_span = static_cast<std::int64_t>(first_anchor.query_span);
        std::int64_t query_start =
            static_cast<std::int64_t>(first_anchor.query_position) + 1 - first_span;
        std::int64_t target_start =
            static_cast<std::int64_t>(first_anchor.ref_position) + 1 - first_span;
        query_start = std::max<std::int64_t>(0, query_start);
        target_start = std::max<std::int64_t>(0, target_start);

        for (std::int32_t index = 1; index < region.seed_count; ++index) {
            const std::size_t anchor_index = static_cast<std::size_t>(anchor_begin + index);
            if (anchor_index >= anchors.size())
                return false;
            const anchor& current = anchors[anchor_index];
            const std::int64_t query_end = static_cast<std::int64_t>(current.query_position) + 1;
            const std::int64_t target_end = static_cast<std::int64_t>(current.ref_position) + 1;
            const bool is_last = index == region.seed_count - 1;
            const std::int64_t query_length = query_end - query_start;
            const std::int64_t target_length = target_end - target_start;
            if (!is_last &&
                (query_length < params.min_ksw_len || target_length < params.min_ksw_len)) {
                continue;
            }
            if (query_length <= 0 || target_length <= 0) {
                query_start = query_end;
                target_start = target_end;
                continue;
            }
            const mandatory_alignment_gap_window window{
                .query_start = static_cast<sequence_size_t>(query_start),
                .query_end = static_cast<sequence_size_t>(query_end),
                .target_start = static_cast<sequence_size_t>(target_start),
                .target_end = static_cast<sequence_size_t>(target_end),
            };
            if (!callback(window))
                return false;
            query_start = query_end;
            target_start = target_end;
        }
        return true;
    }

    /// Derive the complete native-Block stage contract before dispatch from only
    /// immutable region geometry. These values are deliberately recorded outside
    /// the call branches they attest, so deleting or bypassing a stage cannot
    /// delete both the expectation and execution statistics.
    [[nodiscard]] inline block_alignment_eligibility
    derive_block_stage_eligibility(const alignment_region& region,
                                   std::span<const anchor> anchors,
                                   std::int32_t anchor_begin,
                                   std::int64_t seed_ref_start,
                                   std::int64_t seed_query_start,
                                   std::int64_t seed_ref_end,
                                   std::int64_t seed_query_end,
                                   std::int64_t outer_ref_start,
                                   std::int64_t outer_query_start,
                                   std::int64_t outer_ref_end,
                                   std::int64_t outer_query_end,
                                   const minimap2_parameters& params) noexcept {
        block_alignment_eligibility expected{
            .left =
                seed_query_start > outer_query_start && seed_ref_start > outer_ref_start ? 1u : 0u,
            .gap = 0,
            .right = seed_query_end < outer_query_end && seed_ref_end < outer_ref_end ? 1u : 0u,
        };
        if (region.seed_count <= 1)
            return expected;

        std::int64_t qs = seed_query_start;
        std::int64_t rs = seed_ref_start;
        for (std::int32_t index = 1; index < region.seed_count; ++index) {
            const anchor& current = anchors[anchor_begin + index];
            const std::int64_t qe = static_cast<std::int64_t>(current.query_position) + 1;
            const std::int64_t re = static_cast<std::int64_t>(current.ref_position) + 1;
            const bool is_last = index == region.seed_count - 1;
            const std::int64_t gap_query = qe - qs;
            const std::int64_t gap_ref = re - rs;
            if (!is_last && (gap_query < params.min_ksw_len || gap_ref < params.min_ksw_len)) {
                continue;
            }
            if (gap_query <= 0 || gap_ref <= 0) {
                qs = qe;
                rs = re;
                continue;
            }
            ++expected.gap;
            qs = qe;
            rs = re;
        }
        return expected;
    }

    /**
     * Execute left extension, mandatory gap fills, and right extension for one
     * region using the compile-time selected alignment algorithm.
     *
     * @tparam p_base_alignment_algorithm KSW2 or native Block Aligner route.
     * @tparam p_alphabet Logical alphabet shared by query and target.
     * @tparam p_enable_diagonal_certificate Enable Block Aligner certificates.
     */
    template <base_alignment_algorithm p_base_alignment_algorithm,
              sequence_alphabet p_alphabet,
              bool p_enable_diagonal_certificate = true>
    struct align_one_region {
        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq;
        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq;
        const scoring_matrix<p_alphabet>& scoring_matrix;
        const alignment_kernels<p_alphabet>& alignment_kernels;
        const minimap2_parameters& params;
        int32_t align_bandwidth;

        /**
         * Return exact edit accounting for one completed fill.
         *
         * Native Block traceback counts edits while its already-expanded padded
         * query/target ranks are resident. Other kernels retain the established
         * post-result expansion path and therefore keep identical semantics.
         */
        template <bool p_strand_reversed>
        [[gnu::always_inline]] sequence_size_t
        exact_edit_distance_for_fill(const ksw_alignment_result& fill_result,
                                     std::size_t query_start,
                                     std::size_t query_end,
                                     std::size_t target_start,
                                     std::size_t target_end) const {
            if constexpr (p_base_alignment_algorithm == base_alignment_algorithm::block_aligner) {
                if (!fill_result.is_edit_distance_valid) {
                    bioseq_log<logging_level::error>(
                        "Block Aligner result omitted requested exact edit accounting");
                    std::abort();
                }
                return static_cast<sequence_size_t>(fill_result.edit_distance);
            } else {
                return exact_edit_distance_from_packed_cigar<p_strand_reversed>(
                    query_seq,
                    target_seq,
                    query_start,
                    query_end,
                    target_start,
                    target_end,
                    std::span<const std::uint32_t>(fill_result.cigar));
            }
        }

        template <bool p_strand_reversed>
        [[gnu::always_inline]] bool apply_gap_fill_result(alignment_region& region,
                                                          const ksw_alignment_result& fill_result,
                                                          std::int64_t qe,
                                                          std::int64_t re,
                                                          std::int64_t& qs,
                                                          std::int64_t& rs,
                                                          std::int64_t& final_query_end,
                                                          std::int64_t& final_ref_end) const {
            if (!fill_result.cigar.empty()) {
                append_cigar(region.cigar, fill_result.cigar);
            }
            const std::int64_t fill_ref_end =
                fill_result.is_z_dropped ? rs + (fill_result.max_t + 1) : re;
            const std::int64_t fill_query_end =
                fill_result.is_z_dropped ? qs + (fill_result.max_q + 1) : qe;
            if (params.compute_edit_distance) {
                region.edit_distance += exact_edit_distance_for_fill<p_strand_reversed>(
                    fill_result,
                    static_cast<std::size_t>(qs),
                    static_cast<std::size_t>(fill_query_end),
                    static_cast<std::size_t>(rs),
                    static_cast<std::size_t>(fill_ref_end));
            }
            if (fill_result.is_z_dropped) {
                final_ref_end = fill_ref_end;
                final_query_end = fill_query_end;
                region.dp_score += fill_result.max;
                return true;
            }
            final_ref_end = re;
            final_query_end = qe;
            region.dp_score += fill_result.score;
            qs = qe;
            rs = re;
            return false;
        }

        template <bool p_strand_reversed>
        [[gnu::always_inline]] void
        run(alignment_region& region,
            std::span<const anchor> anchors,
            int32_t anchor_begin,
            std::int64_t seed_ref_start,
            std::int64_t seed_query_start,
            std::int64_t seed_ref_end,
            std::int64_t seed_query_end,
            std::int64_t outer_ref_start,
            std::int64_t outer_query_start,
            std::int64_t outer_ref_end,
            std::int64_t outer_query_end,
            std::int64_t query_length,
            block_aligner_execution_stats* block_execution_stats,
            scalar_pipeline_execution_stats* scalar_execution_stats) const {
            bool prior_stage_zdropped = false;

            // === 1. Left extension ===
            std::int64_t final_ref_start = seed_ref_start;
            std::int64_t final_query_start = seed_query_start;
            if (seed_query_start > outer_query_start && seed_ref_start > outer_ref_start) {
                ksw_alignment_result left_result;
                if constexpr (p_base_alignment_algorithm ==
                              base_alignment_algorithm::block_aligner) {
                    left_result = block_aligner_align_pair<p_strand_reversed>(
                        query_seq,
                        target_seq,
                        static_cast<size_t>(outer_query_start),
                        static_cast<size_t>(seed_query_start),
                        static_cast<size_t>(outer_ref_start),
                        static_cast<size_t>(seed_ref_start),
                        scoring_matrix,
                        alignment_kernels,
                        params,
                        block_alignment_stage::left_extension,
                        block_execution_stats);
                } else {
                    left_result = ksw2_align_pair<p_strand_reversed>(
                        query_seq,
                        target_seq,
                        static_cast<size_t>(outer_query_start),
                        static_cast<size_t>(seed_query_start),
                        static_cast<size_t>(outer_ref_start),
                        static_cast<size_t>(seed_ref_start),
                        scoring_matrix,
                        alignment_kernels,
                        params,
                        align_bandwidth,
                        params.end_bonus,
                        params.zdrop,
                        ksw_flags{.right_align = true, .extz_only = true, .reverse_cigar = true},
                        block_execution_stats,
                        scalar_execution_stats,
                        scalar_ksw2_stage::left);
                }
                if (!left_result.cigar.empty()) {
                    append_cigar(region.cigar, left_result.cigar);
                    region.dp_score += left_result.max;
                }
                const int32_t left_ref_offset =
                    left_result.has_reached_end ? left_result.mqe_t + 1 : left_result.max_t + 1;
                final_ref_start = seed_ref_start - left_ref_offset;
                const int32_t left_query_offset = left_result.has_reached_end
                                                      ? seed_query_start - outer_query_start
                                                      : left_result.max_q + 1;
                final_query_start = seed_query_start - left_query_offset;
                if (params.compute_edit_distance) {
                    region.edit_distance += exact_edit_distance_for_fill<p_strand_reversed>(
                        left_result,
                        static_cast<std::size_t>(final_query_start),
                        static_cast<std::size_t>(seed_query_start),
                        static_cast<std::size_t>(final_ref_start),
                        static_cast<std::size_t>(seed_ref_start));
                }
                prior_stage_zdropped = left_result.is_z_dropped;
            }

            // === 2. Gap filling — per-seed-pair walk ===
            std::int64_t final_ref_end = seed_ref_start;
            std::int64_t final_query_end = seed_query_start;
            if (region.seed_count > 1) {
                std::int64_t qs = seed_query_start;
                std::int64_t rs = seed_ref_start;
                bool zdropped_any = false;
                block_global_gap_certificate_plan packed_gap_plan{};
                bool packed_gap_plan_ready = false;
                final_ref_end = seed_ref_start;
                final_query_end = seed_query_start;
                for (int32_t i = 1; i < region.seed_count; ++i) {
                    const anchor& cur_anchor = anchors[anchor_begin + i];
                    const std::int64_t qe =
                        static_cast<std::int64_t>(cur_anchor.query_position) + 1;
                    const std::int64_t re = static_cast<std::int64_t>(cur_anchor.ref_position) + 1;
                    const bool is_last = (i == region.seed_count - 1);
                    const std::int64_t gap_query = qe - qs;
                    const std::int64_t gap_ref = re - rs;
                    if (!is_last &&
                        (gap_query < params.min_ksw_len || gap_ref < params.min_ksw_len)) {
                        continue;
                    }
                    if (gap_query <= 0 || gap_ref <= 0) {
                        qs = qe;
                        rs = re;
                        continue;
                    }
                    if constexpr (p_base_alignment_algorithm ==
                                  base_alignment_algorithm::block_aligner) {
                        block_packed_certificate_attempt packed_attempt{};
                        if constexpr (p_enable_diagonal_certificate) {
                            // Prepare the immutable matrix/contract state only
                            // after the anchor walk produces its first valid gap.
                            // Invalid minimap penalty projections remain on the
                            // existing adapter path, which records and aborts.
                            if (block_aligner_native_penalties_representable(params)) {
                                if (!packed_gap_plan_ready) {
                                    packed_gap_plan =
                                        alignment_kernels.prepare_block_global_gap_certificate(
                                            scoring_matrix,
                                            project_block_aligner_params(
                                                params, block_alignment_stage::gap_extension));
                                    packed_gap_plan_ready = true;
                                }
                                packed_attempt = alignment_kernels.try_block_certified_gap(
                                    p_strand_reversed,
                                    query_seq,
                                    target_seq,
                                    static_cast<size_t>(qs),
                                    static_cast<size_t>(qe),
                                    static_cast<size_t>(rs),
                                    static_cast<size_t>(re),
                                    packed_gap_plan);
                            }
                        }

                        if (packed_attempt.decision ==
                            block_packed_certificate_decision::certified) {
                            const std::uint32_t match_length =
                                static_cast<std::uint32_t>(packed_attempt.certificate.symbols);
                            if (params.do_cigar) {
                                if (!region.cigar.empty() &&
                                    region.cigar.back().opcode == cigar_match_opcode) {
                                    region.cigar.back().length += match_length;
                                } else {
                                    region.cigar.push_back(
                                        cigar_operation{match_length, cigar_match_opcode});
                                }
                            }
                            region.dp_score += packed_attempt.certificate.score;
                            if (params.compute_edit_distance) {
                                region.edit_distance += static_cast<sequence_size_t>(
                                    packed_attempt.certificate.edit_distance);
                            }
                            final_ref_end = re;
                            final_query_end = qe;
                            qs = qe;
                            rs = re;
                            record_block_certified_global_gap(packed_gap_plan,
                                                              packed_attempt.certificate,
                                                              p_strand_reversed,
                                                              block_execution_stats);
                            continue;
                        }

                        // Construct shared result storage only for a real DP
                        // fallback.  Certified gaps above therefore avoid even
                        // the default std::vector state/destructor.
                        ksw_alignment_result fill_result;
                        if (packed_attempt.decision ==
                            block_packed_certificate_decision::rejected) {
                            // Exact packed bytes have already been scanned.  Run
                            // the unchanged DP path without paying for the byte
                            // certificate a second time.
                            fill_result = block_aligner_align_pair<p_strand_reversed, false>(
                                query_seq,
                                target_seq,
                                static_cast<size_t>(qs),
                                static_cast<size_t>(qe),
                                static_cast<size_t>(rs),
                                static_cast<size_t>(re),
                                scoring_matrix,
                                alignment_kernels,
                                params,
                                block_alignment_stage::gap_extension,
                                block_execution_stats);
                        } else {
                            // Fragmented storage, DNA5, unsupported matrices and
                            // non-candidate geometry retain today's route,
                            // including the existing byte certificate.
                            fill_result = block_aligner_align_pair<p_strand_reversed,
                                                                   p_enable_diagonal_certificate>(
                                query_seq,
                                target_seq,
                                static_cast<size_t>(qs),
                                static_cast<size_t>(qe),
                                static_cast<size_t>(rs),
                                static_cast<size_t>(re),
                                scoring_matrix,
                                alignment_kernels,
                                params,
                                block_alignment_stage::gap_extension,
                                block_execution_stats);
                        }
                        if (apply_gap_fill_result<p_strand_reversed>(region,
                                                                     fill_result,
                                                                     qe,
                                                                     re,
                                                                     qs,
                                                                     rs,
                                                                     final_query_end,
                                                                     final_ref_end)) {
                            zdropped_any = true;
                            break;
                        }
                    } else {
                        ksw_alignment_result fill_result = ksw2_align_pair<p_strand_reversed>(
                            query_seq,
                            target_seq,
                            static_cast<size_t>(qs),
                            static_cast<size_t>(qe),
                            static_cast<size_t>(rs),
                            static_cast<size_t>(re),
                            scoring_matrix,
                            alignment_kernels,
                            params,
                            params.compute_edit_distance ? -1 : align_bandwidth,
                            -1,
                            params.compute_edit_distance ? -1 : params.zdrop,
                            ksw_flags{.approx_max = true},
                            block_execution_stats,
                            scalar_execution_stats,
                            scalar_ksw2_stage::gap);
                        if (apply_gap_fill_result<p_strand_reversed>(region,
                                                                     fill_result,
                                                                     qe,
                                                                     re,
                                                                     qs,
                                                                     rs,
                                                                     final_query_end,
                                                                     final_ref_end)) {
                            zdropped_any = true;
                            break;
                        }
                    }
                }
                prior_stage_zdropped = zdropped_any;
            } else {
                final_ref_end = seed_ref_end;
                final_query_end = seed_query_end;
                if (params.do_cigar || params.compute_edit_distance) {
                    append_implicit_seed_cigar(
                        region.cigar,
                        static_cast<std::size_t>(seed_query_end - seed_query_start),
                        static_cast<std::size_t>(seed_ref_end - seed_ref_start));
                }
                if (params.compute_edit_distance) {
                    region.edit_distance +=
                        exact_edit_distance_for_implicit_seed<p_strand_reversed>(
                            query_seq,
                            target_seq,
                            static_cast<std::size_t>(seed_query_start),
                            static_cast<std::size_t>(seed_query_end),
                            static_cast<std::size_t>(seed_ref_start),
                            static_cast<std::size_t>(seed_ref_end));
                }
            }

            // === 3. Right extension ===
            if ((p_base_alignment_algorithm == base_alignment_algorithm::block_aligner ||
                 !prior_stage_zdropped) &&
                seed_query_end < outer_query_end && seed_ref_end < outer_ref_end) {
                ksw_alignment_result right_result;
                if constexpr (p_base_alignment_algorithm ==
                              base_alignment_algorithm::block_aligner) {
                    right_result = block_aligner_align_pair<p_strand_reversed>(
                        query_seq,
                        target_seq,
                        static_cast<size_t>(seed_query_end),
                        static_cast<size_t>(outer_query_end),
                        static_cast<size_t>(seed_ref_end),
                        static_cast<size_t>(outer_ref_end),
                        scoring_matrix,
                        alignment_kernels,
                        params,
                        block_alignment_stage::right_extension,
                        block_execution_stats);
                } else {
                    right_result =
                        ksw2_align_pair<p_strand_reversed>(query_seq,
                                                           target_seq,
                                                           static_cast<size_t>(seed_query_end),
                                                           static_cast<size_t>(outer_query_end),
                                                           static_cast<size_t>(seed_ref_end),
                                                           static_cast<size_t>(outer_ref_end),
                                                           scoring_matrix,
                                                           alignment_kernels,
                                                           params,
                                                           align_bandwidth,
                                                           params.end_bonus,
                                                           params.zdrop,
                                                           ksw_flags{.extz_only = true},
                                                           block_execution_stats,
                                                           scalar_execution_stats,
                                                           scalar_ksw2_stage::right);
                }
                if (!right_result.cigar.empty()) {
                    append_cigar(region.cigar, right_result.cigar);
                    region.dp_score += right_result.max;
                }
                const int32_t right_ref_offset =
                    right_result.has_reached_end ? right_result.mqe_t + 1 : right_result.max_t + 1;
                final_ref_end = seed_ref_end + right_ref_offset;
                const int32_t right_query_offset = right_result.has_reached_end
                                                       ? outer_query_end - seed_query_end
                                                       : right_result.max_q + 1;
                final_query_end = seed_query_end + right_query_offset;
                if (params.compute_edit_distance) {
                    region.edit_distance += exact_edit_distance_for_fill<p_strand_reversed>(
                        right_result,
                        static_cast<std::size_t>(seed_query_end),
                        static_cast<std::size_t>(final_query_end),
                        static_cast<std::size_t>(seed_ref_end),
                        static_cast<std::size_t>(final_ref_end));
                }
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

    template <base_alignment_algorithm p_base_alignment_algorithm,
              bool p_enable_diagonal_certificate = true,
              sequence_alphabet p_alphabet>
    void align_regions(std::span<alignment_region> regions,
                       std::span<const anchor> anchors,
                       bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                       bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                       const scoring_matrix<p_alphabet>& scoring_matrix,
                       const alignment_kernels<p_alphabet>& alignment_kernels,
                       const minimap2_parameters& params,
                       block_aligner_execution_stats* block_execution_stats = nullptr,
                       scalar_pipeline_execution_stats* scalar_execution_stats = nullptr) {
        static const auto* _desc =
            bioseq::instrumentation::register_static_region("mm_align_skeleton");
        auto _scope = bioseq::instrumentation::region(_desc);
        // — lengths are full 64-bit (no int32 truncation of bp_size).
        // Region coordinate math below is signed int64 so the extension/clamp logic
        // and the reverse-strand mirror keep working for sequences longer than 2^31.
        const std::int64_t query_length = static_cast<std::int64_t>(query_seq.bp_size());
        const std::int64_t target_length = static_cast<std::int64_t>(target_seq.bp_size());
        const int32_t align_bandwidth = static_cast<int32_t>(params.bandwidth * 1.5 + 1.0);

        // Construct the non-capturing `align_one_region` functor once per region
        // batch with kernel-invariant references. The per-region call surface is
        // `aligner.template run<p_is_reversed>(...)`.
        const align_one_region<p_base_alignment_algorithm,
                               p_alphabet,
                               p_enable_diagonal_certificate>
            aligner{
                query_seq, target_seq, scoring_matrix, alignment_kernels, params, align_bandwidth};

        // stats collection is opt-in, so the normal benchmark path allocates no
        // diagnostic storage. When enabled, each worker writes one disjoint slot;
        // the deterministic reduction below avoids atomics in the DP hot path.
        std::vector<block_aligner_execution_stats> per_region_block_stats;
        if constexpr (p_base_alignment_algorithm == base_alignment_algorithm::block_aligner) {
            if (block_execution_stats != nullptr) {
                per_region_block_stats.resize(regions.size());
            }
        }
        std::vector<scalar_pipeline_execution_stats> per_region_scalar_stats;
        if constexpr (p_base_alignment_algorithm == base_alignment_algorithm::ksw2) {
            if (scalar_execution_stats != nullptr) {
                per_region_scalar_stats.resize(regions.size());
            }
        }

        auto align_one_region_by_index = [query_length,
                                          target_length,
                                          &regions,
                                          &anchors,
                                          &params,
                                          &aligner,
                                          &per_region_block_stats,
                                          &per_region_scalar_stats](std::size_t region_index) {
            alignment_region& region = regions[region_index];
            if (region.seed_count <= 0) {
                return;
            }

            const int32_t anchor_begin = region.anchor_start;
            const anchor& first_anchor = anchors[anchor_begin];
            const anchor& last_anchor = anchors[anchor_begin + region.seed_count - 1];

            // Inner seed boundaries (where seeds are)
            const int32_t first_span = static_cast<int32_t>(first_anchor.query_span);
            std::int64_t seed_ref_start =
                static_cast<std::int64_t>(first_anchor.ref_position) + 1 - first_span;
            std::int64_t seed_query_start =
                static_cast<std::int64_t>(first_anchor.query_position) + 1 - first_span;
            const std::int64_t seed_ref_end =
                static_cast<std::int64_t>(last_anchor.ref_position) + 1;
            const std::int64_t seed_query_end =
                static_cast<std::int64_t>(last_anchor.query_position) + 1;
            if (seed_ref_start < 0) {
                seed_ref_start = 0;
            }
            if (seed_query_start < 0) {
                seed_query_start = 0;
            }

            // Outer boundaries: extend beyond seeds (align.c:708-762 simplified).
            // `extension_len` is a bounded gap (<= params.max_gap) is_computed in int64
            // to match the int64 coordinates; the std::min args are matched in type.
            std::int64_t outer_ref_start = seed_ref_start;
            std::int64_t outer_query_start = seed_query_start;
            std::int64_t outer_ref_end = seed_ref_end;
            std::int64_t outer_query_end = seed_query_end;

            if (seed_query_start > 0 && seed_ref_start > 0) {
                std::int64_t extension_len =
                    std::min<std::int64_t>(seed_query_start, params.max_gap);
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
                std::int64_t extension_len =
                    std::min<std::int64_t>(query_length - seed_query_end, params.max_gap);
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

            //: pre-reserve CIGAR storage from outer extents so the
            // three-stage fill collapses the geometric doubling chain to one
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
                const std::int64_t cigar_r_extent = outer_ref_end - outer_ref_start;
                const std::int64_t cigar_q_clamped = cigar_q_extent > 0 ? cigar_q_extent : 0;
                const std::int64_t cigar_r_clamped = cigar_r_extent > 0 ? cigar_r_extent : 0;
                const std::size_t raw_estimate = static_cast<std::size_t>(cigar_q_clamped) +
                                                 static_cast<std::size_t>(cigar_r_clamped) + 16u;
                const std::size_t cigar_reserve_cap =
                    static_cast<std::size_t>(4 * params.max_gap) + 64u;
                region.cigar.reserve(std::min(raw_estimate, cigar_reserve_cap));
            }

            region.cigar.clear();
            region.dp_score = 0;
            region.edit_distance = 0;

            block_aligner_execution_stats* region_block_stats = nullptr;
            if constexpr (p_base_alignment_algorithm == base_alignment_algorithm::block_aligner) {
                if (!per_region_block_stats.empty()) {
                    region_block_stats = &per_region_block_stats[region_index];
                    region_block_stats->add(region_block_stats->routed_region_scopes);
                    const block_alignment_eligibility expected =
                        derive_block_stage_eligibility(region,
                                                       anchors,
                                                       anchor_begin,
                                                       seed_ref_start,
                                                       seed_query_start,
                                                       seed_ref_end,
                                                       seed_query_end,
                                                       outer_ref_start,
                                                       outer_query_start,
                                                       outer_ref_end,
                                                       outer_query_end,
                                                       params);
                    region_block_stats->add(region_block_stats->left_route_candidates,
                                            expected.left);
                    region_block_stats->add(region_block_stats->gap_route_candidates, expected.gap);
                    region_block_stats->add(region_block_stats->right_route_candidates,
                                            expected.right);
                }
            }
            scalar_pipeline_execution_stats* region_scalar_stats = nullptr;
            if constexpr (p_base_alignment_algorithm == base_alignment_algorithm::ksw2) {
                if (!per_region_scalar_stats.empty()) {
                    region_scalar_stats = &per_region_scalar_stats[region_index];
                }
            }

            // Per-region diagnostics are derived from `region.cigar.empty()` and
            // the seed/outer spans during debugger inspection. Avoiding a formatted
            // log at every boundary keeps measurement runs free of per-region
            // logging overhead.

            // Dispatch the three alignment stages through the non-capturing
            // aligner functor selected by strand orientation.
            if (region.is_reverse) {
                aligner.template run<true>(region,
                                           anchors,
                                           anchor_begin,
                                           seed_ref_start,
                                           seed_query_start,
                                           seed_ref_end,
                                           seed_query_end,
                                           outer_ref_start,
                                           outer_query_start,
                                           outer_ref_end,
                                           outer_query_end,
                                           query_length,
                                           region_block_stats,
                                           region_scalar_stats);
            } else {
                aligner.template run<false>(region,
                                            anchors,
                                            anchor_begin,
                                            seed_ref_start,
                                            seed_query_start,
                                            seed_ref_end,
                                            seed_query_end,
                                            outer_ref_start,
                                            outer_query_start,
                                            outer_ref_end,
                                            outer_query_end,
                                            query_length,
                                            region_block_stats,
                                            region_scalar_stats);
            }

            // Compute dp_max_score from CIGAR (simplified mm_update_extra)
            region.dp_max_score = region.dp_score;
            region.has_alignment = true;

            // Compute match_length/block_length from CIGAR
            int32_t cigar_match_count = 0;
            int32_t cigar_block_count = 0;
            for (const auto& cigar_op : region.cigar) {
                const int32_t op_len = static_cast<int32_t>(cigar_op.length);
                if (cigar_op.opcode == cigar_match_opcode ||
                    cigar_op.opcode == cigar_equal_opcode) {
                    cigar_match_count += op_len;
                    cigar_block_count += op_len;
                } else if (cigar_op.opcode == cigar_mismatch_opcode) {
                    cigar_block_count += op_len;
                } else if (cigar_op.opcode == cigar_insertion_opcode ||
                           cigar_op.opcode == cigar_deletion_opcode) {
                    cigar_block_count += op_len;
                }
            }
            if (cigar_match_count > 0) {
                region.match_length = cigar_match_count;
                const int32_t effective_block =
                    (cigar_block_count > 0) ? cigar_block_count : region.block_length;
                region.block_length = effective_block;
            }
        };

        const auto& pool = bioseq::parallelism::global_pool::instance();
        const bool task_parallel_candidate =
            bioseq::parallelism::has_parallel_tasks(pool.get_mode()) &&
            pool.maximum_concurrency() > 1 &&
            p_base_alignment_algorithm != base_alignment_algorithm::ksw2_batched &&
            bioseq::storage::is_worker_shareable(query_seq) &&
            bioseq::storage::is_worker_shareable(target_seq) &&
            regions.size() >= bioseq::parallelism::task_parallel_minimums::k_region_count;
        std::size_t total_seed_count = 0;
        if (task_parallel_candidate) {
            for (const alignment_region& region : regions) {
                if (region.seed_count > 0) {
                    total_seed_count += static_cast<std::size_t>(region.seed_count);
                }
            }
        }

        const bool may_parallel =
            task_parallel_candidate &&
            total_seed_count >= bioseq::parallelism::task_parallel_minimums::k_seed_count;

        if (may_parallel) {
            bioseq::parallelism::parallel_for_pipeline(
                0,
                regions.size(),
                1,
                bioseq::parallelism::task_parallel_minimums::k_region_count,
                bioseq::parallelism::task_parallel_region::align_regions,
                [&align_one_region_by_index](std::size_t lo, std::size_t hi) {
                    for (std::size_t region_index = lo; region_index < hi; ++region_index) {
                        align_one_region_by_index(region_index);
                    }
                });
        } else {
            //  — liveness heartbeat. A single big region (many gap-fills over a
            // multi-Mbp seed span) can dominate wall-clock; the per-stage milestones
            // alone would show a long silence with no sign of progress. Emit (and flush)
            // a heartbeat at most ~once/second so `tail -f` of the log shows which
            // region index is in flight and the live footprint. The logging facade
            // reads PostgreSQL's active flag when installed and the standalone
            // fallback otherwise, preserving this module's domain-neutral closure.
            const bool debug_logging_enabled = is_bioseq_debug_logging_enabled();
            std::int64_t hb_last_ns =
                debug_logging_enabled ? bioseq::instrumentation::clock::now_ns() : 0;
            std::size_t hb_region_idx = 0;
            for (std::size_t region_index = 0; region_index < regions.size(); ++region_index) {
                ++hb_region_idx;
                if (debug_logging_enabled) {
                    const std::int64_t hb_now_ns = bioseq::instrumentation::clock::now_ns();
                    if (hb_now_ns - hb_last_ns > 1'000'000'000LL) {
                        hb_last_ns = hb_now_ns;
                        bioseq_log_lazy<logging_level::debug>(
                            [hb_region_idx, &regions, region_index] {
                                bioseq_log_progress<logging_level::debug>(
                                    "[minimap_port] aligning region {}/{} seed_count={}"
                                    " | footprint={}MB avail={}MB",
                                    hb_region_idx,
                                    regions.size(),
                                    regions[region_index].seed_count,
                                    bioseq::common::memory::process_footprint_bytes() >> 20,
                                    bioseq::common::memory::system_available_bytes() >> 20);
                            });
                    }
                }
                align_one_region_by_index(region_index);
            }
        }

        if constexpr (p_base_alignment_algorithm == base_alignment_algorithm::block_aligner) {
            if (block_execution_stats != nullptr) {
                *block_execution_stats = {};
                for (const block_aligner_execution_stats& region_stats : per_region_block_stats) {
                    block_execution_stats->merge(region_stats);
                }
                block_execution_stats->add(block_execution_stats->align_region_invocations);
                if (may_parallel) {
                    block_execution_stats->add(
                        block_execution_stats->parallel_align_region_dispatches);
                } else {
                    block_execution_stats->add(
                        block_execution_stats->serial_align_region_dispatches);
                }
                if (!block_execution_stats->is_internally_consistent()) {
                    bioseq_log<logging_level::error>(
                        "Block Aligner execution stats rejected: eligible={}, "
                        "executed={}, ksw2={}, scalar_fallbacks={}",
                        block_execution_stats->eligible_calls(),
                        block_execution_stats->executed_calls(),
                        block_execution_stats->forbidden_ksw2_calls,
                        block_execution_stats->forbidden_scalar_biological_fallbacks);
                    std::abort();
                }
            }
        }
        if constexpr (p_base_alignment_algorithm == base_alignment_algorithm::ksw2) {
            if (scalar_execution_stats != nullptr) {
                *scalar_execution_stats = {};
                for (const scalar_pipeline_execution_stats& region_stats :
                     per_region_scalar_stats) {
                    scalar_execution_stats->merge(region_stats);
                }
            }
        }

        bioseq_log_lazy<logging_level::debug>([&regions] {
            bioseq_log_progress<logging_level::debug>(
                "[minimap_port] Aligned {} regions | footprint={}MB avail={}MB",
                regions.size(),
                bioseq::common::memory::process_footprint_bytes() >> 20,
                bioseq::common::memory::system_available_bytes() >> 20);
        });
    }

    // ============================================================================
    // Stage 8: Compute mapping quality (mm_set_mapq equivalent)
    // ============================================================================

    // ============================================================================
    // Stage 7b: Estimate divergence from seeds (mm_est_err equivalent)
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
    template <class p_minimizer>
    void estimate_divergence_from_seeds(std::span<alignment_region> regions,
                                        std::span<const p_minimizer> query_minimizers,
                                        std::span<const anchor> anchors,
                                        sequence_size_t query_len,
                                        int32_t kmer_size,
                                        sequence_size_t target_len) {
        static const auto* _desc = bioseq::instrumentation::register_static_region("mm_est_err");
        auto _scope = bioseq::instrumentation::region(_desc);
        if (query_minimizers.empty()) {
            return;
        }

        // ----------------------------------------------------------------------
        // A sort-free per-position prefix count provides O(1) membership and
        // O(1) range counts:
        //
        //   prefix_count[i] = #(query minimizers with last_pos < i)
        //
        // Then:
        //   * membership at position p:   prefix_count[p+1] > prefix_count[p]
        //   * #mini_pos in [a, b]:         prefix_count[b+1] - prefix_count[a]
        //
        // Build cost is O(N + query_len). The lane-interleaved SIMD collector does
        // not produce sorted positions, so the prefix-count table avoids a sort
        // and answers membership/range queries with two random-access loads.
        // `compute_forward_pos(s)` gives both forward and reverse chains ascending
        // forward positions for s=0..seed_count-1, so one search range is correct
        // for both directions.
        // ----------------------------------------------------------------------
        // For dna4 (the only benchmark alphabet), every minimizer span equals the
        // configured kmer size, so the average span is exactly `kmer_size`.
        const float avg_kmer_span = static_cast<float>(kmer_size);

        // Build the prefix-count array in two linear passes:
        //   1. mini_pos_count[p + 1] += #(minimizers at last_pos == p)
        //   2. cumulative-sum: mini_pos_count[i] += mini_pos_count[i - 1]
        // Final invariant: mini_pos_count[i] = #(minimizers with last_pos < i)
        //
        // Size is (query_len + 2) so `mini_pos_count[query_len + 1]` is a
        // valid index (covers the case where last_pos == query_len, even
        // though collector emission clamps to query_len - 1). Using
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

        // Non-capturing functors store their dependencies by value, keeping the hot
        // path inlineable without capture-all lifetime coupling.
        struct is_minimizer {
            const std::uint32_t* mini_pos_count;
            sequence_size_t query_len;
            [[nodiscard, gnu::always_inline]]
            inline bool operator()(std::int64_t pos) const noexcept {
                if (pos < 0 || static_cast<sequence_size_t>(pos) > query_len)
                    return false;
                const std::size_t p = static_cast<std::size_t>(pos);
                return mini_pos_count[p + 1] > mini_pos_count[p];
            }
        };
        struct range_count {
            const std::uint32_t* mini_pos_count;
            sequence_size_t query_len;
            [[nodiscard, gnu::always_inline]]
            inline int32_t operator()(std::int64_t a, std::int64_t b) const noexcept {
                if (a < 0)
                    a = 0;
                if (static_cast<sequence_size_t>(b) > query_len)
                    b = static_cast<std::int64_t>(query_len);
                if (a > b)
                    return 0;
                return static_cast<int32_t>(mini_pos_count[static_cast<std::size_t>(b) + 1] -
                                            mini_pos_count[static_cast<std::size_t>(a)]);
            }
        };
        const is_minimizer is_minimizer{mini_pos_count.data(), query_len};
        const range_count range_count{mini_pos_count.data(), query_len};

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
            // Store every dependency by value so the hot-loop callable remains
            // non-capturing and directly inlineable.
            struct compute_forward_position {
                const anchor* anchors_base;
                int32_t anchor_begin;
                int32_t seed_count;
                sequence_size_t query_len;
                bool region_is_reverse;

                [[nodiscard, gnu::always_inline]]
                inline std::int64_t operator()(int32_t s) const noexcept {
                    const anchor& chain_anchor =
                        region_is_reverse ? anchors_base[anchor_begin + seed_count - 1 - s]
                                          : anchors_base[anchor_begin + s];
                    std::int64_t forward_pos =
                        static_cast<std::int64_t>(chain_anchor.query_position);
                    if (chain_anchor.is_reverse) {
                        const std::int64_t anchor_span =
                            static_cast<int32_t>(chain_anchor.query_span);
                        forward_pos = static_cast<std::int64_t>(query_len) - 1 -
                                      (forward_pos + 1 - anchor_span);
                    }
                    return forward_pos;
                }
            };
            const compute_forward_position compute_forward_pos{
                anchors.data(), anchor_begin, region.seed_count, query_len, region.is_reverse};

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
            if (query_len - region.query_start > avg_kmer_span &&
                target_len - region.ref_end > avg_kmer_span) {
                ++total_minimizer_span;
            }

            // Divergence formula (esterr.c:64).
            if (seed_match_count >= total_minimizer_span) {
                region.divergence = 0.0f;
            } else {
                const double match_ratio =
                    static_cast<double>(seed_match_count) / total_minimizer_span;
                region.divergence =
                    static_cast<float>(1.0 - std::pow(match_ratio, 1.0 / avg_kmer_span));
            }
        }
    }

    // ============================================================================
    // Stage 8: Compute mapping quality (mm_set_mapq equivalent)

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
    void compute_mapping_quality(std::span<alignment_region> regions,
                                 const minimap2_parameters& params,
                                 int32_t rep_len) {
        static const auto* _desc = bioseq::instrumentation::register_static_region("mm_set_mapq");
        auto _scope = bioseq::instrumentation::region(_desc);
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
        const float uniqueness_ratio =
            static_cast<float>(primary_score_sum) / static_cast<float>(primary_score_sum + rep_len);

        for (auto& region : regions) {
            if (region.parent != region.region_id) {
                region.mapq = 0; // secondaries get 0
                continue;
            }

            // Primary mapping
            const float score_factor = (region.score > 100) ? 1.0f : 0.01f * region.score;
            const float score_penalty = score_factor * uniqueness_ratio;
            const float raw_seed_penalty =
                (region.seed_count > 10) ? 1.0f : 0.1f * region.seed_count;
            const float seed_count_penalty = std::min(score_penalty, raw_seed_penalty);

            const int32_t effective_secondary_score =
                std::max(region.secondary_score, min_chain_sc);

            int32_t quality_value;
            // hit.c:447-467: three branches based on DP availability
            if (region.has_alignment && region.secondary_dp_max > 0 && region.dp_max_score > 0) {
                // Formula A: with DP scores (hit.c:448-457)
                const float identity =
                    (region.block_length > 0)
                        ? static_cast<float>(region.match_length) / region.block_length
                        : 0.f;
                const float score_ratio =
                    static_cast<float>(region.secondary_dp_max) * effective_secondary_score /
                    (static_cast<float>(region.dp_max_score) * region.original_score);
                quality_value = static_cast<int32_t>(
                    identity * seed_count_penalty * quality_coefficient *
                    (1.0f - score_ratio * score_ratio) *
                    std::log(static_cast<float>(region.dp_max_score) / match_sc));

                // BWA-MEM-like alternative (hit.c:455)
                const int32_t quality_alt = static_cast<int32_t>(
                    6.02f * identity * identity * (region.dp_max_score - region.secondary_dp_max) /
                        match_sc +
                    0.499f);
                quality_value = std::min(quality_value, quality_alt);
            } else {
                // No secondary_dp_max available: use chain score formula (hit.c:460-467)
                const float score_ratio =
                    static_cast<float>(effective_secondary_score) / region.original_score;
                if (region.has_alignment) {
                    const float identity =
                        (region.block_length > 0)
                            ? static_cast<float>(region.match_length) / region.block_length
                            : 0.f;
                    quality_value = static_cast<int32_t>(
                        identity * seed_count_penalty * quality_coefficient * (1.0f - score_ratio) *
                        std::log(static_cast<float>(region.dp_max_score) / match_sc));
                } else {
                    // No DP at all — use chain score (hit.c:466)
                    quality_value = static_cast<int32_t>(
                        seed_count_penalty * quality_coefficient * (1.0f - score_ratio) *
                        std::log(static_cast<float>(region.score)));
                }
            }

            // Subtract secondary_count penalty (hit.c:469)
            quality_value -= static_cast<int32_t>(
                4.343f * std::log(static_cast<float>(region.secondary_count + 1)) + 0.499f);
            quality_value = std::max(quality_value, 0);
            region.mapq = std::min(quality_value, 60);

            // Ensure non-zero if clearly best (hit.c:472)
            if (region.has_alignment && region.dp_max_score > region.secondary_dp_max &&
                region.mapq == 0) {
                region.mapq = 1;
            }
        }

        bioseq_log<logging_level::debug>("[minimap_port] Computed MAPQ for {} regions",
                                         regions.size());
    }

    // ============================================================================
    // Stage 9: Convert to match_result (PAF output)
    // ============================================================================

    /**
     * @brief Convert internal alignment_region to PAF-like match_result.
     *
     * Fields map to PAF columns as documented in mm_write_paf4() (format.c:427-460).
     */
    [[nodiscard]] match_result region_to_match_result(alignment_region&& region,
                                                      sequence_size_t query_len,
                                                      sequence_size_t target_len) {
        return match_result{
            .query_len = query_len,
            .query_start = region.query_start,
            .query_end = region.query_end,
            .is_reverse = region.is_reverse,
            .target_len = target_len,
            .target_start = region.ref_start,
            .target_end = region.ref_end,
            .num_matches = region.match_length,
            .block_length = region.block_length,
            .mapq = region.mapq,
            .chain_score = region.score,
            .dp_score = region.dp_score,
            .dp_max_score = region.dp_max_score,
            .seed_count = region.seed_count,
            .secondary_chain_score = region.secondary_score,
            .divergence = region.divergence,
            .is_primary = (region.parent == region.region_id),
            .has_cigar = region.has_alignment,
            .cigar = std::move(region.cigar),
            .edit_distance = region.edit_distance,
        };
    }

    // ============================================================================
    // PAF output — match_result_collection
    // ============================================================================

    // CIGAR opcode formatting is owned by the CIGAR module.

    /// Positive execution statistics for partially accelerated Vulkan routes.
    /// Counters are incremented only after a selected
    /// fail-loud Vulkan operation returns successfully. Selector flags alone never
    /// populate this stats.
    struct partial_vulkan_execution_stats {
        /// Role-neutral token emitted by a minimizer leaf. It must be consumed as
        /// either query_sketch or target_sketch before a match_result_collection is
        /// returned to callers.
        std::uint64_t sketch_dispatches = 0;
        std::uint64_t query_sketch_dispatches = 0;
        std::uint64_t target_sketch_dispatches = 0;
        std::uint64_t target_index_build_dispatches = 0;
        std::uint64_t seed_lookup_dispatches = 0;
        // Operation-originated representation branch from the completed seed
        // shader. Their sum must equal seed_lookup_dispatches for strict rows.
        std::uint64_t seed_open_addressed_lookup_dispatches = 0;
        std::uint64_t seed_sorted_dense_lookup_dispatches = 0;
        std::uint64_t chain_score_dispatches = 0;
        // Operation-local seed-sort statistics. A selected mode flag never writes
        // these counters; they are copied only from a completed runtime result.
        std::uint64_t seed_bitonic_sort_dispatches = 0;
        std::uint64_t seed_radix_sort_operations = 0;
        std::uint64_t seed_radix_anchor_count = 0;
        std::uint64_t seed_radix_passes = 0;
        std::uint64_t seed_radix_histogram_dispatches = 0;
        std::uint64_t seed_radix_scan_dispatches = 0;
        std::uint64_t seed_radix_scatter_dispatches = 0;
        std::uint64_t seed_host_sort_fallbacks = 0;
        // Biological identity statistics emitted by completed GPU seed roundtrips. The
        // count/order/digest stream is independent from resident storage
        // integrity checks and therefore remains comparable between distinct
        // resident/index construction routes.
        std::uint64_t seed_anchor_stats_operations = 0;
        std::uint64_t seed_canonical_anchor_count = 0;
        std::uint64_t seed_canonical_anchor_order_checks = 0;
        std::uint64_t seed_canonical_anchor_order_accepts = 0;
        std::uint64_t seed_canonical_tie_normalization_runs = 0;
        std::uint64_t seed_canonical_tie_normalization_anchors = 0;
        std::uint64_t seed_anchor_digest_lane0 = 0;
        std::uint64_t seed_anchor_digest_lane1 = 0;
        // Operation-originated target-index materialization statistics.
        // Additive fields describe actual upload operations, never cache hits;
        // max fields preserve the worst collision/load observation across them.
        std::uint64_t gpu_index_materializations = 0;
        std::uint64_t gpu_index_open_addressed_materializations = 0;
        std::uint64_t gpu_index_sorted_dense_materializations = 0;
        std::uint64_t gpu_index_unique_keys = 0;
        std::uint64_t gpu_index_slot_capacity = 0;
        std::uint64_t gpu_index_logical_resident_bytes = 0;
        std::uint64_t gpu_index_actual_allocation_bytes = 0;
        std::uint64_t gpu_index_fully_touched_bytes = 0;
        std::uint64_t gpu_index_staging_bytes = 0;
        std::uint64_t gpu_index_permit_releases = 0;
        std::uint64_t gpu_index_process_footprint_reflections = 0;
        std::uint64_t gpu_index_reflected_footprint_bytes = 0;
        std::uint64_t gpu_index_max_load_ppm = 0;
        std::uint64_t gpu_index_max_probe = 0;
        std::uint64_t gpu_index_count_admissions = 0;
        std::uint64_t gpu_index_main_peak_admissions = 0;
        std::uint64_t gpu_index_resident_transfers = 0;
        std::uint64_t gpu_index_adoption_rereserve_attempts = 0;
        std::uint64_t gpu_index_adoption_successes = 0;
        std::uint64_t gpu_index_owned_resident_bytes = 0;
        std::uint64_t gpu_index_gate_enabled = 0;
        std::uint64_t gpu_index_gate_word_count = 0;
        std::uint64_t gpu_index_gate_mask = 0;
        std::uint64_t gpu_index_gate_set_bits = 0;
        std::uint64_t gpu_index_gate_digest_lo = 0;
        std::uint64_t gpu_index_gate_digest_hi = 0;
        // Operation-originated target-index kernel and shape statistics. These
        // fields are attached to the one query that owns the shared target build,
        // never to every query that subsequently consumes the resident index.
        std::uint64_t gpu_index_gate_count_dispatches = 0;
        std::uint64_t gpu_index_gated_emit_dispatches = 0;
        std::uint64_t gpu_index_ungated_emit_dispatches = 0;
        std::uint64_t gpu_index_exact_count_dispatches = 0;
        std::uint64_t gpu_index_exact_count_prefix_scan_dispatches = 0;
        std::uint64_t gpu_index_exact_count_control_readbacks = 0;
        std::uint64_t gpu_index_deterministic_tile_emit_dispatches = 0;
        std::uint64_t gpu_index_atomic_append_dispatches = 0;
        std::uint64_t gpu_index_emit_validation_control_readbacks = 0;
        std::uint64_t gpu_index_counted_minimizer_count = 0;
        std::uint64_t gpu_index_verified_emit_count = 0;
        std::uint64_t gpu_index_count_emit_parity_verified = 0;
        std::uint64_t gpu_index_target_minimizer_capacity_estimate = 0;
        std::uint64_t gpu_index_capacity_estimate_underestimated = 0;
        std::uint64_t gpu_index_arena_trimmed_before_permit_shrink = 0;
        std::uint64_t gpu_index_retained_minimizer_count = 0;
        std::uint64_t gpu_index_distinct_minimizer_count = 0;
        // query-independent source-direct sorted-dense route.
        // These fields are copied from the operation ledger only after its
        // terminal predicate accepts.  They therefore describe completed work,
        // including the canonical-empty bypass which intentionally has no GPU
        // dispatch and is therefore not represented by dispatch counters.
        std::uint64_t gpu_index_source_direct_operations = 0;
        std::uint64_t gpu_index_source_direct_terminal_successes = 0;
        std::uint64_t gpu_index_source_direct_empty_bypasses = 0;
        std::uint64_t gpu_index_source_direct_zero_query_bypasses = 0;
        std::uint64_t gpu_index_source_direct_planning_receipts = 0;
        std::uint64_t gpu_index_source_direct_terminal_receipts = 0;
        std::uint64_t gpu_index_source_direct_materializations = 0;
        std::uint64_t gpu_index_source_direct_resident_publications = 0;
        std::uint64_t gpu_index_source_direct_logical_control_bytes = 0;
        std::uint64_t gpu_index_source_direct_logical_position_bytes = 0;
        std::uint64_t gpu_index_source_direct_control_requirement_bytes = 0;
        std::uint64_t gpu_index_source_direct_position_requirement_bytes = 0;
        std::uint64_t gpu_index_source_direct_allocation_request_bytes = 0;
        std::uint64_t gpu_index_source_direct_actual_control_bytes = 0;
        std::uint64_t gpu_index_source_direct_actual_position_bytes = 0;
        std::uint64_t gpu_index_source_direct_source_releases = 0;
        std::uint64_t gpu_index_source_direct_source_released_bytes = 0;
        std::uint64_t gpu_index_source_direct_checksum_matches = 0;
        std::uint64_t gpu_index_source_direct_source_traversals = 0;
        std::uint64_t gpu_index_source_direct_mapped_traversals = 0;
        std::uint64_t gpu_index_source_direct_reflected_authorities = 0;
        std::uint64_t gpu_index_source_direct_retained_authorities = 0;
        std::uint64_t gpu_index_source_direct_retained_resident_permit_bytes = 0;
        std::uint64_t gpu_index_source_direct_cache_preinsertions = 0;
        std::uint64_t gpu_index_source_direct_expected_seed_consumers = 0;
        std::uint64_t gpu_index_source_direct_completed_seed_consumers = 0;
        std::uint64_t gpu_index_source_direct_required_lookups = 0;
        std::uint64_t gpu_index_source_direct_required_hits = 0;
        std::uint64_t gpu_index_source_direct_required_misses = 0;
        std::uint64_t gpu_index_source_direct_fallback_attempts = 0;
        std::uint64_t gpu_index_source_direct_mapped_prefix_passes = 0;
        std::uint64_t gpu_index_source_direct_mapped_prefix_lookups = 0;
        std::uint64_t gpu_index_source_direct_mapped_prefix_comparisons = 0;
        std::uint64_t gpu_index_source_direct_seed_slab_requested_bytes = 0;
        std::uint64_t gpu_index_source_direct_seed_slab_initial_capacity = 0;
        std::uint64_t gpu_index_source_direct_seed_slab_actual_bytes = 0;
        std::uint64_t gpu_index_source_direct_seed_slab_retained_bytes = 0;
        std::uint64_t gpu_index_source_direct_seed_slab_permit_releases = 0;
        std::uint64_t gpu_index_source_direct_seed_reserved_bytes = 0;
        std::uint64_t gpu_index_source_direct_seed_planned_vulkan_bytes = 0;
        std::uint64_t gpu_index_source_direct_seed_actual_vulkan_bytes = 0;
        std::uint64_t gpu_index_source_direct_seed_roundtrips = 0;
        std::uint64_t gpu_index_source_direct_zero_anchor_bypasses = 0;
        std::uint64_t gpu_index_source_direct_seed_fence_completions = 0;
        std::uint64_t gpu_index_source_direct_cache_cleanup_releases = 0;
        std::uint64_t gpu_index_source_direct_resident_cleanup_bytes = 0;
        std::uint64_t gpu_index_source_direct_pipeline_completions = 0;
        // Owned-only last-seed retirement statistics. These counters are populated
        // only after the completed seed operation has dropped its local resident
        // handle and the operation-owned lifetime coordinator has actually
        // released storage before chaining.
        std::uint64_t post_seed_owned_gpu_cache_evictions = 0;
        std::uint64_t post_seed_owned_gpu_resident_bytes_released = 0;
        std::uint64_t post_seed_owned_cpu_index_releases = 0;
        std::uint64_t post_seed_owned_cpu_position_bytes_released = 0;
        bool is_saturated = false;

        static constexpr void
        add(std::uint64_t& destination, std::uint64_t increment, bool& saturated_flag) noexcept {
            if (increment > (std::numeric_limits<std::uint64_t>::max)() - destination) {
                destination = (std::numeric_limits<std::uint64_t>::max)();
                saturated_flag = true;
                return;
            }
            destination += increment;
        }

        constexpr void merge(const partial_vulkan_execution_stats& other) noexcept {
            bool overflow = is_saturated || other.is_saturated;
            const std::uint64_t anchor_operations_before = seed_anchor_stats_operations;
            add(sketch_dispatches, other.sketch_dispatches, overflow);
            add(query_sketch_dispatches, other.query_sketch_dispatches, overflow);
            add(target_sketch_dispatches, other.target_sketch_dispatches, overflow);
            add(target_index_build_dispatches, other.target_index_build_dispatches, overflow);
            add(seed_lookup_dispatches, other.seed_lookup_dispatches, overflow);
            add(seed_open_addressed_lookup_dispatches,
                other.seed_open_addressed_lookup_dispatches,
                overflow);
            add(seed_sorted_dense_lookup_dispatches,
                other.seed_sorted_dense_lookup_dispatches,
                overflow);
            add(chain_score_dispatches, other.chain_score_dispatches, overflow);
            add(seed_bitonic_sort_dispatches, other.seed_bitonic_sort_dispatches, overflow);
            add(seed_radix_sort_operations, other.seed_radix_sort_operations, overflow);
            add(seed_radix_anchor_count, other.seed_radix_anchor_count, overflow);
            add(seed_radix_passes, other.seed_radix_passes, overflow);
            add(seed_radix_histogram_dispatches, other.seed_radix_histogram_dispatches, overflow);
            add(seed_radix_scan_dispatches, other.seed_radix_scan_dispatches, overflow);
            add(seed_radix_scatter_dispatches, other.seed_radix_scatter_dispatches, overflow);
            add(seed_host_sort_fallbacks, other.seed_host_sort_fallbacks, overflow);
            add(seed_anchor_stats_operations, other.seed_anchor_stats_operations, overflow);
            add(seed_canonical_anchor_count, other.seed_canonical_anchor_count, overflow);
            add(seed_canonical_anchor_order_checks,
                other.seed_canonical_anchor_order_checks,
                overflow);
            add(seed_canonical_anchor_order_accepts,
                other.seed_canonical_anchor_order_accepts,
                overflow);
            add(seed_canonical_tie_normalization_runs,
                other.seed_canonical_tie_normalization_runs,
                overflow);
            add(seed_canonical_tie_normalization_anchors,
                other.seed_canonical_tie_normalization_anchors,
                overflow);
            if (other.seed_anchor_stats_operations != 0u) {
                if (anchor_operations_before == 0u) {
                    seed_anchor_digest_lane0 = other.seed_anchor_digest_lane0;
                    seed_anchor_digest_lane1 = other.seed_anchor_digest_lane1;
                } else {
                    seed_anchor_digest_lane0 =
                        std::rotl(seed_anchor_digest_lane0, 19) ^ other.seed_anchor_digest_lane0 ^
                        (other.seed_canonical_anchor_count * 0x9e3779b97f4a7c15ull);
                    seed_anchor_digest_lane1 =
                        std::rotl(seed_anchor_digest_lane1, 37) ^ other.seed_anchor_digest_lane1 ^
                        (other.seed_canonical_anchor_count * 0xbf58476d1ce4e5b9ull);
                }
            }
            add(gpu_index_materializations, other.gpu_index_materializations, overflow);
            add(gpu_index_open_addressed_materializations,
                other.gpu_index_open_addressed_materializations,
                overflow);
            add(gpu_index_sorted_dense_materializations,
                other.gpu_index_sorted_dense_materializations,
                overflow);
            add(gpu_index_unique_keys, other.gpu_index_unique_keys, overflow);
            add(gpu_index_slot_capacity, other.gpu_index_slot_capacity, overflow);
            add(gpu_index_logical_resident_bytes, other.gpu_index_logical_resident_bytes, overflow);
            add(gpu_index_actual_allocation_bytes,
                other.gpu_index_actual_allocation_bytes,
                overflow);
            add(gpu_index_fully_touched_bytes, other.gpu_index_fully_touched_bytes, overflow);
            add(gpu_index_staging_bytes, other.gpu_index_staging_bytes, overflow);
            add(gpu_index_permit_releases, other.gpu_index_permit_releases, overflow);
            add(gpu_index_process_footprint_reflections,
                other.gpu_index_process_footprint_reflections,
                overflow);
            add(gpu_index_reflected_footprint_bytes,
                other.gpu_index_reflected_footprint_bytes,
                overflow);
            add(gpu_index_count_admissions, other.gpu_index_count_admissions, overflow);
            add(gpu_index_main_peak_admissions, other.gpu_index_main_peak_admissions, overflow);
            add(gpu_index_resident_transfers, other.gpu_index_resident_transfers, overflow);
            add(gpu_index_adoption_rereserve_attempts,
                other.gpu_index_adoption_rereserve_attempts,
                overflow);
            add(gpu_index_adoption_successes, other.gpu_index_adoption_successes, overflow);
            add(gpu_index_owned_resident_bytes, other.gpu_index_owned_resident_bytes, overflow);
            add(gpu_index_gate_enabled, other.gpu_index_gate_enabled, overflow);
            add(gpu_index_gate_word_count, other.gpu_index_gate_word_count, overflow);
            add(gpu_index_gate_mask, other.gpu_index_gate_mask, overflow);
            add(gpu_index_gate_set_bits, other.gpu_index_gate_set_bits, overflow);
            add(gpu_index_gate_digest_lo, other.gpu_index_gate_digest_lo, overflow);
            add(gpu_index_gate_digest_hi, other.gpu_index_gate_digest_hi, overflow);
            add(gpu_index_gate_count_dispatches, other.gpu_index_gate_count_dispatches, overflow);
            add(gpu_index_gated_emit_dispatches, other.gpu_index_gated_emit_dispatches, overflow);
            add(gpu_index_ungated_emit_dispatches,
                other.gpu_index_ungated_emit_dispatches,
                overflow);
            add(gpu_index_exact_count_dispatches, other.gpu_index_exact_count_dispatches, overflow);
            add(gpu_index_exact_count_prefix_scan_dispatches,
                other.gpu_index_exact_count_prefix_scan_dispatches,
                overflow);
            add(gpu_index_exact_count_control_readbacks,
                other.gpu_index_exact_count_control_readbacks,
                overflow);
            add(gpu_index_deterministic_tile_emit_dispatches,
                other.gpu_index_deterministic_tile_emit_dispatches,
                overflow);
            add(gpu_index_atomic_append_dispatches,
                other.gpu_index_atomic_append_dispatches,
                overflow);
            add(gpu_index_emit_validation_control_readbacks,
                other.gpu_index_emit_validation_control_readbacks,
                overflow);
            add(gpu_index_counted_minimizer_count,
                other.gpu_index_counted_minimizer_count,
                overflow);
            add(gpu_index_verified_emit_count, other.gpu_index_verified_emit_count, overflow);
            add(gpu_index_count_emit_parity_verified,
                other.gpu_index_count_emit_parity_verified,
                overflow);
            add(gpu_index_target_minimizer_capacity_estimate,
                other.gpu_index_target_minimizer_capacity_estimate,
                overflow);
            add(gpu_index_capacity_estimate_underestimated,
                other.gpu_index_capacity_estimate_underestimated,
                overflow);
            add(gpu_index_arena_trimmed_before_permit_shrink,
                other.gpu_index_arena_trimmed_before_permit_shrink,
                overflow);
            add(gpu_index_retained_minimizer_count,
                other.gpu_index_retained_minimizer_count,
                overflow);
            add(gpu_index_distinct_minimizer_count,
                other.gpu_index_distinct_minimizer_count,
                overflow);
            add(gpu_index_source_direct_operations,
                other.gpu_index_source_direct_operations,
                overflow);
            add(gpu_index_source_direct_terminal_successes,
                other.gpu_index_source_direct_terminal_successes,
                overflow);
            add(gpu_index_source_direct_empty_bypasses,
                other.gpu_index_source_direct_empty_bypasses,
                overflow);
            add(gpu_index_source_direct_zero_query_bypasses,
                other.gpu_index_source_direct_zero_query_bypasses,
                overflow);
            add(gpu_index_source_direct_planning_receipts,
                other.gpu_index_source_direct_planning_receipts,
                overflow);
            add(gpu_index_source_direct_terminal_receipts,
                other.gpu_index_source_direct_terminal_receipts,
                overflow);
            add(gpu_index_source_direct_materializations,
                other.gpu_index_source_direct_materializations,
                overflow);
            add(gpu_index_source_direct_resident_publications,
                other.gpu_index_source_direct_resident_publications,
                overflow);
            add(gpu_index_source_direct_logical_control_bytes,
                other.gpu_index_source_direct_logical_control_bytes,
                overflow);
            add(gpu_index_source_direct_logical_position_bytes,
                other.gpu_index_source_direct_logical_position_bytes,
                overflow);
            add(gpu_index_source_direct_control_requirement_bytes,
                other.gpu_index_source_direct_control_requirement_bytes,
                overflow);
            add(gpu_index_source_direct_position_requirement_bytes,
                other.gpu_index_source_direct_position_requirement_bytes,
                overflow);
            add(gpu_index_source_direct_allocation_request_bytes,
                other.gpu_index_source_direct_allocation_request_bytes,
                overflow);
            add(gpu_index_source_direct_actual_control_bytes,
                other.gpu_index_source_direct_actual_control_bytes,
                overflow);
            add(gpu_index_source_direct_actual_position_bytes,
                other.gpu_index_source_direct_actual_position_bytes,
                overflow);
            add(gpu_index_source_direct_source_releases,
                other.gpu_index_source_direct_source_releases,
                overflow);
            add(gpu_index_source_direct_source_released_bytes,
                other.gpu_index_source_direct_source_released_bytes,
                overflow);
            add(gpu_index_source_direct_checksum_matches,
                other.gpu_index_source_direct_checksum_matches,
                overflow);
            add(gpu_index_source_direct_source_traversals,
                other.gpu_index_source_direct_source_traversals,
                overflow);
            add(gpu_index_source_direct_mapped_traversals,
                other.gpu_index_source_direct_mapped_traversals,
                overflow);
            add(gpu_index_source_direct_reflected_authorities,
                other.gpu_index_source_direct_reflected_authorities,
                overflow);
            add(gpu_index_source_direct_retained_authorities,
                other.gpu_index_source_direct_retained_authorities,
                overflow);
            add(gpu_index_source_direct_retained_resident_permit_bytes,
                other.gpu_index_source_direct_retained_resident_permit_bytes,
                overflow);
            add(gpu_index_source_direct_cache_preinsertions,
                other.gpu_index_source_direct_cache_preinsertions,
                overflow);
            add(gpu_index_source_direct_expected_seed_consumers,
                other.gpu_index_source_direct_expected_seed_consumers,
                overflow);
            add(gpu_index_source_direct_completed_seed_consumers,
                other.gpu_index_source_direct_completed_seed_consumers,
                overflow);
            add(gpu_index_source_direct_required_lookups,
                other.gpu_index_source_direct_required_lookups,
                overflow);
            add(gpu_index_source_direct_required_hits,
                other.gpu_index_source_direct_required_hits,
                overflow);
            add(gpu_index_source_direct_required_misses,
                other.gpu_index_source_direct_required_misses,
                overflow);
            add(gpu_index_source_direct_fallback_attempts,
                other.gpu_index_source_direct_fallback_attempts,
                overflow);
            add(gpu_index_source_direct_mapped_prefix_passes,
                other.gpu_index_source_direct_mapped_prefix_passes,
                overflow);
            add(gpu_index_source_direct_mapped_prefix_lookups,
                other.gpu_index_source_direct_mapped_prefix_lookups,
                overflow);
            add(gpu_index_source_direct_mapped_prefix_comparisons,
                other.gpu_index_source_direct_mapped_prefix_comparisons,
                overflow);
            add(gpu_index_source_direct_seed_slab_requested_bytes,
                other.gpu_index_source_direct_seed_slab_requested_bytes,
                overflow);
            add(gpu_index_source_direct_seed_slab_initial_capacity,
                other.gpu_index_source_direct_seed_slab_initial_capacity,
                overflow);
            add(gpu_index_source_direct_seed_slab_actual_bytes,
                other.gpu_index_source_direct_seed_slab_actual_bytes,
                overflow);
            add(gpu_index_source_direct_seed_slab_retained_bytes,
                other.gpu_index_source_direct_seed_slab_retained_bytes,
                overflow);
            add(gpu_index_source_direct_seed_slab_permit_releases,
                other.gpu_index_source_direct_seed_slab_permit_releases,
                overflow);
            add(gpu_index_source_direct_seed_reserved_bytes,
                other.gpu_index_source_direct_seed_reserved_bytes,
                overflow);
            add(gpu_index_source_direct_seed_planned_vulkan_bytes,
                other.gpu_index_source_direct_seed_planned_vulkan_bytes,
                overflow);
            add(gpu_index_source_direct_seed_actual_vulkan_bytes,
                other.gpu_index_source_direct_seed_actual_vulkan_bytes,
                overflow);
            add(gpu_index_source_direct_seed_roundtrips,
                other.gpu_index_source_direct_seed_roundtrips,
                overflow);
            add(gpu_index_source_direct_zero_anchor_bypasses,
                other.gpu_index_source_direct_zero_anchor_bypasses,
                overflow);
            add(gpu_index_source_direct_seed_fence_completions,
                other.gpu_index_source_direct_seed_fence_completions,
                overflow);
            add(gpu_index_source_direct_cache_cleanup_releases,
                other.gpu_index_source_direct_cache_cleanup_releases,
                overflow);
            add(gpu_index_source_direct_resident_cleanup_bytes,
                other.gpu_index_source_direct_resident_cleanup_bytes,
                overflow);
            add(gpu_index_source_direct_pipeline_completions,
                other.gpu_index_source_direct_pipeline_completions,
                overflow);
            add(post_seed_owned_gpu_cache_evictions,
                other.post_seed_owned_gpu_cache_evictions,
                overflow);
            add(post_seed_owned_gpu_resident_bytes_released,
                other.post_seed_owned_gpu_resident_bytes_released,
                overflow);
            add(post_seed_owned_cpu_index_releases,
                other.post_seed_owned_cpu_index_releases,
                overflow);
            add(post_seed_owned_cpu_position_bytes_released,
                other.post_seed_owned_cpu_position_bytes_released,
                overflow);
            gpu_index_max_load_ppm = std::max(gpu_index_max_load_ppm, other.gpu_index_max_load_ppm);
            gpu_index_max_probe = std::max(gpu_index_max_probe, other.gpu_index_max_probe);
            is_saturated = overflow;
        }

        [[nodiscard]] constexpr bool has_any_dispatch() const noexcept {
            return is_saturated || sketch_dispatches != 0 || query_sketch_dispatches != 0 ||
                   target_sketch_dispatches != 0 || target_index_build_dispatches != 0 ||
                   seed_lookup_dispatches != 0 || seed_open_addressed_lookup_dispatches != 0 ||
                   seed_sorted_dense_lookup_dispatches != 0 || chain_score_dispatches != 0 ||
                   seed_bitonic_sort_dispatches != 0 || seed_radix_sort_operations != 0 ||
                   seed_radix_anchor_count != 0 || seed_radix_passes != 0 ||
                   seed_radix_histogram_dispatches != 0 || seed_radix_scan_dispatches != 0 ||
                   seed_radix_scatter_dispatches != 0 || seed_host_sort_fallbacks != 0 ||
                   seed_anchor_stats_operations != 0 || seed_canonical_anchor_count != 0 ||
                   seed_canonical_anchor_order_checks != 0 ||
                   seed_canonical_anchor_order_accepts != 0 ||
                   seed_canonical_tie_normalization_runs != 0 ||
                   seed_canonical_tie_normalization_anchors != 0 || seed_anchor_digest_lane0 != 0 ||
                   seed_anchor_digest_lane1 != 0 || gpu_index_gate_count_dispatches != 0 ||
                   gpu_index_gated_emit_dispatches != 0 || gpu_index_ungated_emit_dispatches != 0 ||
                   gpu_index_exact_count_dispatches != 0 ||
                   gpu_index_exact_count_prefix_scan_dispatches != 0 ||
                   gpu_index_exact_count_control_readbacks != 0 ||
                   gpu_index_deterministic_tile_emit_dispatches != 0 ||
                   gpu_index_atomic_append_dispatches != 0 ||
                   gpu_index_emit_validation_control_readbacks != 0 ||
                   gpu_index_counted_minimizer_count != 0 || gpu_index_verified_emit_count != 0 ||
                   gpu_index_count_emit_parity_verified != 0 ||
                   gpu_index_target_minimizer_capacity_estimate != 0 ||
                   gpu_index_capacity_estimate_underestimated != 0 ||
                   gpu_index_arena_trimmed_before_permit_shrink != 0 ||
                   gpu_index_source_direct_operations != 0 ||
                   gpu_index_source_direct_empty_bypasses != 0 ||
                   gpu_index_source_direct_planning_receipts != 0 ||
                   gpu_index_source_direct_terminal_receipts != 0;
        }

        [[nodiscard]] constexpr bool is_exact_target_index_route_consistent() const noexcept {
            const std::uint64_t builds = target_index_build_dispatches;
            if (builds == 0u) {
                return gpu_index_exact_count_dispatches == 0u &&
                       gpu_index_exact_count_prefix_scan_dispatches == 0u &&
                       gpu_index_exact_count_control_readbacks == 0u &&
                       gpu_index_deterministic_tile_emit_dispatches == 0u &&
                       gpu_index_atomic_append_dispatches == 0u &&
                       gpu_index_emit_validation_control_readbacks == 0u &&
                       gpu_index_counted_minimizer_count == 0u &&
                       gpu_index_verified_emit_count == 0u &&
                       gpu_index_count_emit_parity_verified == 0u &&
                       gpu_index_target_minimizer_capacity_estimate == 0u &&
                       gpu_index_capacity_estimate_underestimated == 0u &&
                       gpu_index_arena_trimmed_before_permit_shrink == 0u &&
                       gpu_index_gate_enabled == 0u && gpu_index_gate_count_dispatches == 0u &&
                       gpu_index_gated_emit_dispatches == 0u &&
                       gpu_index_ungated_emit_dispatches == 0u &&
                       gpu_index_retained_minimizer_count == 0u &&
                       gpu_index_distinct_minimizer_count == 0u;
            }
            // A Stage-C batch owns exactly one shared target build and attaches its
            // stats to query zero. More than one build is not an accepted strict row.
            if (builds != 1u) {
                return false;
            }
            const bool empty = gpu_index_retained_minimizer_count == 0u;
            return target_sketch_dispatches == builds &&
                   gpu_index_exact_count_dispatches == builds &&
                   gpu_index_exact_count_prefix_scan_dispatches == builds * 3u &&
                   gpu_index_exact_count_control_readbacks == builds &&
                   gpu_index_deterministic_tile_emit_dispatches == (empty ? 0u : builds) &&
                   gpu_index_atomic_append_dispatches == 0u &&
                   gpu_index_emit_validation_control_readbacks == (empty ? 0u : builds) &&
                   gpu_index_counted_minimizer_count == gpu_index_retained_minimizer_count &&
                   gpu_index_verified_emit_count == gpu_index_retained_minimizer_count &&
                   gpu_index_count_emit_parity_verified == builds &&
                   gpu_index_target_minimizer_capacity_estimate > 0u &&
                   gpu_index_capacity_estimate_underestimated ==
                       (gpu_index_retained_minimizer_count >
                                gpu_index_target_minimizer_capacity_estimate
                            ? 1u
                            : 0u) &&
                   gpu_index_arena_trimmed_before_permit_shrink == builds &&
                   (gpu_index_gate_enabled == 1u
                        ? gpu_index_gate_count_dispatches == 1u &&
                              gpu_index_gated_emit_dispatches ==
                                  gpu_index_deterministic_tile_emit_dispatches &&
                              gpu_index_ungated_emit_dispatches == 0u
                        : gpu_index_gate_enabled == 0u && gpu_index_gate_count_dispatches == 0u &&
                              gpu_index_gated_emit_dispatches == 0u &&
                              gpu_index_ungated_emit_dispatches ==
                                  gpu_index_deterministic_tile_emit_dispatches);
        }

        /// A source-direct sorted-dense operation owns a resident target index but
        /// deliberately emits no Stage-C target-build dispatch. Its N/D fields are
        /// therefore positive statistics, not contradictory no-build residue. Keep
        /// the target-index ownership, gate, and build surface fail-closed while allowing the
        /// direct operation's full helper to validate its resident shape.
        [[nodiscard]] constexpr bool
        is_exact_source_direct_target_index_route_consistent() const noexcept {
            return target_index_build_dispatches == 0u &&
                   gpu_index_source_direct_operations == 1u &&
                   gpu_index_source_direct_planning_receipts == 1u &&
                   gpu_index_source_direct_terminal_receipts == 1u &&
                   gpu_index_source_direct_terminal_successes == 1u &&
                   gpu_index_count_admissions == 0u && gpu_index_main_peak_admissions == 0u &&
                   gpu_index_resident_transfers == 0u &&
                   gpu_index_adoption_rereserve_attempts == 0u &&
                   gpu_index_adoption_successes == 0u && gpu_index_owned_resident_bytes == 0u &&
                   gpu_index_gate_enabled == 0u && gpu_index_gate_word_count == 0u &&
                   gpu_index_gate_mask == 0u && gpu_index_gate_set_bits == 0u &&
                   gpu_index_gate_digest_lo == 0u && gpu_index_gate_digest_hi == 0u &&
                   gpu_index_gate_count_dispatches == 0u && gpu_index_gated_emit_dispatches == 0u &&
                   gpu_index_ungated_emit_dispatches == 0u &&
                   gpu_index_exact_count_dispatches == 0u &&
                   gpu_index_exact_count_prefix_scan_dispatches == 0u &&
                   gpu_index_exact_count_control_readbacks == 0u &&
                   gpu_index_deterministic_tile_emit_dispatches == 0u &&
                   gpu_index_atomic_append_dispatches == 0u &&
                   gpu_index_emit_validation_control_readbacks == 0u &&
                   gpu_index_counted_minimizer_count == 0u && gpu_index_verified_emit_count == 0u &&
                   gpu_index_count_emit_parity_verified == 0u &&
                   gpu_index_target_minimizer_capacity_estimate == 0u &&
                   gpu_index_capacity_estimate_underestimated == 0u &&
                   gpu_index_arena_trimmed_before_permit_shrink == 0u;
        }

        [[nodiscard]] constexpr bool
        is_exact_stage_mask_consistent(bool expect_query_sketch,
                                       bool expect_target_sketch,
                                       bool expect_target_index_build,
                                       bool expect_seed_lookup,
                                       bool expect_chain_score) const noexcept {
            const auto matches = [](std::uint64_t count, bool expected) noexcept {
                return expected ? count > 0 : count == 0;
            };
            return !is_saturated && sketch_dispatches == 0 &&
                   is_exact_target_index_route_consistent() &&
                   matches(query_sketch_dispatches, expect_query_sketch) &&
                   matches(target_sketch_dispatches, expect_target_sketch) &&
                   matches(target_index_build_dispatches, expect_target_index_build) &&
                   matches(seed_lookup_dispatches, expect_seed_lookup) &&
                   matches(chain_score_dispatches, expect_chain_score);
        }

        [[nodiscard]] constexpr bool
        is_exact_source_direct_stage_mask_consistent(bool expect_query_sketch,
                                                     bool expect_target_sketch,
                                                     bool expect_seed_lookup,
                                                     bool expect_chain_score) const noexcept {
            const auto matches = [](std::uint64_t count, bool expected) noexcept {
                return expected ? count > 0 : count == 0;
            };
            return !is_saturated && sketch_dispatches == 0u &&
                   is_exact_source_direct_target_index_route_consistent() &&
                   matches(query_sketch_dispatches, expect_query_sketch) &&
                   matches(target_sketch_dispatches, expect_target_sketch) &&
                   matches(seed_lookup_dispatches, expect_seed_lookup) &&
                   matches(chain_score_dispatches, expect_chain_score);
        }

        [[nodiscard]] constexpr bool is_exact_sketch_token_consistent() const noexcept {
            if (sketch_dispatches != 1) {
                return false;
            }
            partial_vulkan_execution_stats expected{};
            expected.sketch_dispatches = 1;
            return *this == expected;
        }

        constexpr bool operator==(const partial_vulkan_execution_stats&) const noexcept = default;
    };

    /// A batch constructs one Stage-C target index and shares it with every query.
    /// Query zero is the sole owner of the build/ownership stats so additive batch
    /// aggregation cannot turn one target build into N apparent builds.
    [[nodiscard]] constexpr bool
    is_ordered_target_build_owner_query(std::size_t query_index) noexcept {
        return query_index == 0u;
    }

    static_assert([] {
        partial_vulkan_execution_stats stats{};
        stats.chain_score_dispatches = 1;
        return stats.is_exact_stage_mask_consistent(false, false, false, false, true);
    }());
    static_assert([] {
        partial_vulkan_execution_stats stats{};
        stats.query_sketch_dispatches = 1;
        stats.target_sketch_dispatches = 1;
        stats.target_index_build_dispatches = 1;
        stats.gpu_index_exact_count_dispatches = 1;
        stats.gpu_index_exact_count_prefix_scan_dispatches = 3;
        stats.gpu_index_exact_count_control_readbacks = 1;
        stats.gpu_index_deterministic_tile_emit_dispatches = 1;
        stats.gpu_index_emit_validation_control_readbacks = 1;
        stats.gpu_index_counted_minimizer_count = 53;
        stats.gpu_index_verified_emit_count = 53;
        stats.gpu_index_count_emit_parity_verified = 1;
        stats.gpu_index_target_minimizer_capacity_estimate = 100;
        stats.gpu_index_arena_trimmed_before_permit_shrink = 1;
        stats.gpu_index_ungated_emit_dispatches = 1;
        stats.gpu_index_retained_minimizer_count = 53;
        stats.seed_lookup_dispatches = 1;
        stats.chain_score_dispatches = 1;
        return stats.is_exact_stage_mask_consistent(true, true, true, true, true) &&
               !stats.is_exact_stage_mask_consistent(true, true, false, true, true);
    }());
    static_assert([] {
        partial_vulkan_execution_stats stats{};
        stats.query_sketch_dispatches = 1u;
        stats.target_sketch_dispatches = 1u;
        stats.seed_lookup_dispatches = 1u;
        stats.chain_score_dispatches = 1u;
        stats.gpu_index_materializations = 1u;
        stats.gpu_index_sorted_dense_materializations = 1u;
        stats.gpu_index_retained_minimizer_count = 53u;
        stats.gpu_index_distinct_minimizer_count = 17u;
        stats.gpu_index_source_direct_operations = 1u;
        stats.gpu_index_source_direct_planning_receipts = 1u;
        stats.gpu_index_source_direct_terminal_receipts = 1u;
        stats.gpu_index_source_direct_terminal_successes = 1u;
        if (!stats.is_exact_source_direct_stage_mask_consistent(true, true, true, true)) {
            return false;
        }
        if (stats.is_exact_stage_mask_consistent(true, true, false, true, true)) {
            return false;
        }
        stats.gpu_index_gate_enabled = 1u;
        return !stats.is_exact_source_direct_stage_mask_consistent(true, true, true, true);
    }());
    static_assert([] {
        partial_vulkan_execution_stats lhs{};
        lhs.gpu_index_materializations = 1;
        lhs.gpu_index_open_addressed_materializations = 1;
        lhs.gpu_index_unique_keys = 3;
        lhs.gpu_index_max_probe = 7;
        partial_vulkan_execution_stats rhs{};
        rhs.gpu_index_materializations = 1;
        rhs.gpu_index_open_addressed_materializations = 1;
        rhs.gpu_index_unique_keys = 5;
        rhs.gpu_index_max_probe = 4;
        lhs.merge(rhs);
        return lhs.gpu_index_materializations == 2 &&
               lhs.gpu_index_open_addressed_materializations == 2 &&
               lhs.gpu_index_unique_keys == 8 && lhs.gpu_index_max_probe == 7;
    }());

    /**
     * @brief Owning container of alignment results with PAF output formatting.
     *
     * Wraps a std::vector<match_result> and provides a print() method that outputs
     * results in exact minimap2 PAF format, reproducing mm_write_paf4()
     * (format.c:427-460) with auxiliary tags from write_tags() (format.c:399-425).
     */
    struct match_result_collection {
        std::vector<match_result> results;
        two_stack_execution_stats exact_simd_query_execution_stats{};
        two_stack_execution_stats exact_simd_target_execution_stats{};
        bounded_nthash_execution_stats bounded_nthash_query_execution_stats{};
        bounded_nthash_execution_stats bounded_nthash_target_execution_stats{};
        bioseq::chaining::simd_chaining_execution_stats simd_chaining_stats{};
        block_aligner_execution_stats block_aligner_stats{};
        scalar_pipeline_execution_stats scalar_pipeline_stats{};
        ksw2_batched_execution_stats ksw2_batched_stats{};
        ::bioseq::prefilter::prefilter_execution_stats prefilter_stats{};
        partial_vulkan_execution_stats partial_vulkan_stats{};
        bioseq::indexing::prepared_fragment_anchor_batch_stats fragment_native_query_stats{};
        bool exact_simd_query_execution_stats_available = false;
        bool exact_simd_target_execution_stats_available = false;
        bool bounded_nthash_query_execution_stats_available = false;
        bool bounded_nthash_target_execution_stats_available = false;
        bool simd_chaining_execution_stats_available = false;
        bool block_aligner_execution_stats_available = false;
        bool scalar_pipeline_execution_stats_available = false;
        bool ksw2_batched_execution_stats_available = false;
        bool prefilter_execution_stats_available = false;
        bool fragment_native_query_stats_available = false;
        // Operation-local CPU/SIMD wall clocks. These travel with the result so
        // parallel rows never update shared statement statistics. The PostgreSQL
        // dispatch boundary consumes them exactly once into the existing stable
        // pipeline_stage timing slots.
        std::uint64_t query_sketch_host_wall_ns = 0;
        std::uint64_t persisted_index_decode_host_wall_ns = 0;
        std::uint64_t seed_lookup_host_wall_ns = 0;
        std::uint64_t chain_score_host_wall_ns = 0;
        std::uint64_t region_generation_host_wall_ns = 0;
        std::uint64_t alignment_result_host_wall_ns = 0;
        bool is_host_stage_timing_saturated = false;
        // Coarse clocks cross stage objects but never escape a live pipeline.
        // The boundary methods below turn them into completed, mergeable deltas
        // before the collection is returned to its caller.
        std::uint64_t region_host_stage_started_ns = 0;
        std::uint64_t alignment_result_host_stage_started_ns = 0;
        bool is_region_host_stage_active = false;
        bool is_alignment_result_host_stage_active = false;

        /// Create an empty result collection and execution-stat aggregate.
        match_result_collection() = default;

        /// Adopt a completed ordered result vector.
        explicit match_result_collection(std::vector<match_result>&& results_vec)
            : results(std::move(results_vec)) {}

        /// Merge completed exact-SIMD query-sketch statistics.
        void
        record_exact_simd_query_execution_stats(const two_stack_execution_stats& stats) noexcept {
            exact_simd_query_execution_stats.merge(stats);
            exact_simd_query_execution_stats_available = true;
        }

        /// Merge completed exact-SIMD target-sketch statistics.
        void
        record_exact_simd_target_execution_stats(const two_stack_execution_stats& stats) noexcept {
            exact_simd_target_execution_stats.merge(stats);
            exact_simd_target_execution_stats_available = true;
        }

        /// Merge completed bounded-ntHash query-sketch statistics.
        void record_bounded_nthash_query_execution_stats(
            const bounded_nthash_execution_stats& stats) noexcept {
            bounded_nthash_query_execution_stats.merge(stats);
            bounded_nthash_query_execution_stats_available = true;
        }

        /// Merge completed bounded-ntHash target-sketch statistics.
        void record_bounded_nthash_target_execution_stats(
            const bounded_nthash_execution_stats& stats) noexcept {
            bounded_nthash_target_execution_stats.merge(stats);
            bounded_nthash_target_execution_stats_available = true;
        }

        /// Merge completed SIMD chaining statistics.
        void record_simd_chaining_execution_stats(
            const bioseq::chaining::simd_chaining_execution_stats& stats) noexcept {
            simd_chaining_stats.merge(stats);
            simd_chaining_execution_stats_available = true;
        }

        /// Merge completed Block Aligner statistics.
        void
        record_block_aligner_execution_stats(const block_aligner_execution_stats& stats) noexcept {
            block_aligner_stats.merge(stats);
            block_aligner_execution_stats_available = true;
        }

        /// Merge completed scalar pipeline statistics.
        void record_scalar_pipeline_execution_stats(
            const scalar_pipeline_execution_stats& stats) noexcept {
            scalar_pipeline_stats.merge(stats);
            scalar_pipeline_execution_stats_available = true;
        }

        /// Merge completed batched-KSW2 statistics.
        void
        record_ksw2_batched_execution_stats(const ksw2_batched_execution_stats& stats) noexcept {
            ksw2_batched_stats.merge(stats);
            ksw2_batched_execution_stats_available = true;
        }

        /// Merge completed prefilter statistics.
        void record_prefilter_execution_stats(
            const ::bioseq::prefilter::prefilter_execution_stats& stats) noexcept {
            prefilter_stats.merge(stats);
            prefilter_execution_stats_available = true;
        }

        /// Route scalar query-sketch statistics into the scalar aggregate.
        void record_scalar_query_minimizer_execution_stats(
            const scalar_minimizer_execution_stats& stats) noexcept {
            scalar_pipeline_execution_stats routed{};
            routed.query_minimizers = stats;
            record_scalar_pipeline_execution_stats(routed);
        }

        /// Route scalar target-sketch statistics into the scalar aggregate.
        void record_scalar_target_minimizer_execution_stats(
            const scalar_minimizer_execution_stats& stats) noexcept {
            scalar_pipeline_execution_stats routed{};
            routed.target_minimizers = stats;
            record_scalar_pipeline_execution_stats(routed);
        }

        /// Merge completed partial-Vulkan statistics.
        void record_partial_vulkan_execution_stats(
            const partial_vulkan_execution_stats& stats) noexcept {
            partial_vulkan_stats.merge(stats);
        }

        /// Merge completed fragment-native query statistics.
        void record_fragment_native_query_stats(
            const bioseq::indexing::prepared_fragment_anchor_batch_stats& stats) noexcept {
            fragment_native_query_stats.merge(stats);
            fragment_native_query_stats_available = true;
        }

        /// Add one host-stage duration and retain overflow as a sticky flag.
        void record_host_stage_time(std::uint64_t& destination,
                                    const std::uint64_t elapsed_ns) noexcept {
            if (elapsed_ns > (std::numeric_limits<std::uint64_t>::max)() - destination) {
                destination = (std::numeric_limits<std::uint64_t>::max)();
                is_host_stage_timing_saturated = true;
                return;
            }
            destination += elapsed_ns;
        }

        /// Add query-sketch host time.
        void record_query_sketch_host_time(const std::uint64_t elapsed_ns) noexcept {
            record_host_stage_time(query_sketch_host_wall_ns, elapsed_ns);
        }

        /// Add persisted-index decode host time.
        void record_persisted_index_decode_host_time(const std::uint64_t elapsed_ns) noexcept {
            record_host_stage_time(persisted_index_decode_host_wall_ns, elapsed_ns);
        }

        /// Add seed-lookup host time.
        void record_seed_lookup_host_time(const std::uint64_t elapsed_ns) noexcept {
            record_host_stage_time(seed_lookup_host_wall_ns, elapsed_ns);
        }

        /// Add chain-score host time.
        void record_chain_score_host_time(const std::uint64_t elapsed_ns) noexcept {
            record_host_stage_time(chain_score_host_wall_ns, elapsed_ns);
        }

        /// Add region-generation host time.
        void record_region_generation_host_time(const std::uint64_t elapsed_ns) noexcept {
            record_host_stage_time(region_generation_host_wall_ns, elapsed_ns);
        }

        /// Add alignment and result-materialization host time.
        void record_alignment_result_host_time(const std::uint64_t elapsed_ns) noexcept {
            record_host_stage_time(alignment_result_host_wall_ns, elapsed_ns);
        }

        /// Start the region-generation/selection owner after chaining completes.
        void begin_region_host_stage(const std::uint64_t started_ns) noexcept {
            if (is_region_host_stage_active || is_alignment_result_host_stage_active) {
                is_host_stage_timing_saturated = true;
            }
            region_host_stage_started_ns = started_ns;
            is_region_host_stage_active = true;
        }

        /// Close regions and, when alignment is enabled, reuse the exact same
        /// boundary as the start of the inclusive alignment/result owner.
        void
        transition_region_to_alignment_result_host_stage(const std::uint64_t boundary_ns,
                                                         const bool alignment_enabled) noexcept {
            if (!is_region_host_stage_active || boundary_ns < region_host_stage_started_ns ||
                is_alignment_result_host_stage_active) {
                is_host_stage_timing_saturated = true;
                is_region_host_stage_active = false;
                is_alignment_result_host_stage_active = false;
                return;
            }
            record_region_generation_host_time(
                std::max<std::uint64_t>(1u, boundary_ns - region_host_stage_started_ns));
            is_region_host_stage_active = false;
            if (alignment_enabled) {
                alignment_result_host_stage_started_ns = boundary_ns;
                is_alignment_result_host_stage_active = true;
            }
        }

        /// Close the inclusive alignment, post-alignment filters, MAPQ and result
        /// materialisation owner at the terminal sink.
        void finish_alignment_result_host_stage(const std::uint64_t finished_ns) noexcept {
            if (!is_alignment_result_host_stage_active) {
                return;
            }
            if (finished_ns < alignment_result_host_stage_started_ns) {
                is_host_stage_timing_saturated = true;
                is_alignment_result_host_stage_active = false;
                return;
            }
            record_alignment_result_host_time(
                std::max<std::uint64_t>(1u, finished_ns - alignment_result_host_stage_started_ns));
            is_alignment_result_host_stage_active = false;
        }

        /// Merge all completed execution statistics without moving results.
        void combine_execution_stats(const match_result_collection& other) noexcept {
            if (other.exact_simd_query_execution_stats_available) {
                record_exact_simd_query_execution_stats(other.exact_simd_query_execution_stats);
            }
            if (other.exact_simd_target_execution_stats_available) {
                record_exact_simd_target_execution_stats(other.exact_simd_target_execution_stats);
            }
            if (other.bounded_nthash_query_execution_stats_available) {
                record_bounded_nthash_query_execution_stats(
                    other.bounded_nthash_query_execution_stats);
            }
            if (other.bounded_nthash_target_execution_stats_available) {
                record_bounded_nthash_target_execution_stats(
                    other.bounded_nthash_target_execution_stats);
            }
            if (other.simd_chaining_execution_stats_available) {
                record_simd_chaining_execution_stats(other.simd_chaining_stats);
            }
            if (other.block_aligner_execution_stats_available) {
                record_block_aligner_execution_stats(other.block_aligner_stats);
            }
            if (other.scalar_pipeline_execution_stats_available) {
                record_scalar_pipeline_execution_stats(other.scalar_pipeline_stats);
            }
            if (other.ksw2_batched_execution_stats_available) {
                record_ksw2_batched_execution_stats(other.ksw2_batched_stats);
            }
            if (other.prefilter_execution_stats_available) {
                record_prefilter_execution_stats(other.prefilter_stats);
            }
            partial_vulkan_stats.merge(other.partial_vulkan_stats);
            if (other.fragment_native_query_stats_available) {
                record_fragment_native_query_stats(other.fragment_native_query_stats);
            }
            record_query_sketch_host_time(other.query_sketch_host_wall_ns);
            record_persisted_index_decode_host_time(other.persisted_index_decode_host_wall_ns);
            record_seed_lookup_host_time(other.seed_lookup_host_wall_ns);
            record_chain_score_host_time(other.chain_score_host_wall_ns);
            record_region_generation_host_time(other.region_generation_host_wall_ns);
            record_alignment_result_host_time(other.alignment_result_host_wall_ns);
            is_host_stage_timing_saturated =
                is_host_stage_timing_saturated || other.is_host_stage_timing_saturated ||
                is_region_host_stage_active || is_alignment_result_host_stage_active ||
                other.is_region_host_stage_active || other.is_alignment_result_host_stage_active;
        }

        /// Validate exact-SIMD query execution against the selected work policy.
        [[nodiscard]] bool
        is_exact_simd_query_execution_consistent(bool require_nonempty_vector_work) const noexcept {
            return exact_simd_query_execution_stats_available &&
                   exact_simd_query_execution_stats.is_internally_consistent(
                       require_nonempty_vector_work);
        }

        /// Validate exact-SIMD target execution against the selected work policy.
        [[nodiscard]] bool is_exact_simd_target_execution_consistent(
            bool require_nonempty_vector_work) const noexcept {
            return exact_simd_target_execution_stats_available &&
                   exact_simd_target_execution_stats.is_internally_consistent(
                       require_nonempty_vector_work);
        }

        /// Validate bounded-ntHash query execution.
        [[nodiscard]] bool is_bounded_nthash_query_execution_consistent(
            bool require_nonempty_vector_work) const noexcept {
            return bounded_nthash_query_execution_stats_available &&
                   bounded_nthash_query_execution_stats.is_internally_consistent(
                       require_nonempty_vector_work);
        }

        /// Validate bounded-ntHash target execution.
        [[nodiscard]] bool is_bounded_nthash_target_execution_consistent(
            bool require_nonempty_vector_work) const noexcept {
            return bounded_nthash_target_execution_stats_available &&
                   bounded_nthash_target_execution_stats.is_internally_consistent(
                       require_nonempty_vector_work);
        }

        /// Validate SIMD chaining for the selected policy.
        [[nodiscard]] bool
        is_simd_chaining_execution_consistent(simd_chaining_policy selected_policy,
                                              bool require_nonempty_vector_work) const noexcept {
            return simd_chaining_execution_stats_available &&
                   simd_chaining_stats.is_internally_consistent(selected_policy,
                                                                require_nonempty_vector_work);
        }

        /// Validate the batched-KSW2 execution receipt.
        [[nodiscard]] bool
        is_ksw2_batched_execution_consistent(const bool require_nonempty_work) const noexcept {
            return ksw2_batched_execution_stats_available &&
                   ksw2_batched_stats.is_coherent(require_nonempty_work);
        }

        /// Validate the selected SIMD sketch route and forbid mixed backends.
        [[nodiscard]] bool is_selected_simd_sketch_execution_consistent(
            minimizer_algorithm selected,
            bool require_target_sketch,
            bool require_nonempty_vector_work) const noexcept {
            if (partial_vulkan_stats != partial_vulkan_execution_stats{}) {
                return false;
            }

            const bool exact_query_absent =
                !exact_simd_query_execution_stats_available &&
                exact_simd_query_execution_stats == two_stack_execution_stats{};
            const bool exact_target_absent =
                !exact_simd_target_execution_stats_available &&
                exact_simd_target_execution_stats == two_stack_execution_stats{};
            const bool bounded_query_absent =
                !bounded_nthash_query_execution_stats_available &&
                bounded_nthash_query_execution_stats == bounded_nthash_execution_stats{};
            const bool bounded_target_absent =
                !bounded_nthash_target_execution_stats_available &&
                bounded_nthash_target_execution_stats == bounded_nthash_execution_stats{};

            switch (selected) {
            case minimizer_algorithm::two_stack_nthash:
                return bounded_query_absent && bounded_target_absent &&
                       is_exact_simd_query_execution_consistent(require_nonempty_vector_work) &&
                       (require_target_sketch ? is_exact_simd_target_execution_consistent(
                                                    require_nonempty_vector_work)
                                              : exact_target_absent);
            case minimizer_algorithm::bounded_nthash_simd:
                return exact_query_absent && exact_target_absent &&
                       is_bounded_nthash_query_execution_consistent(require_nonempty_vector_work) &&
                       (require_target_sketch ? is_bounded_nthash_target_execution_consistent(
                                                    require_nonempty_vector_work)
                                              : bounded_target_absent);
            default:
                return false;
            }
        }

        /// Validate the SIMD sketch/chaining/Block tuple plus its real CPU seed
        /// lookup. All other scalar biological dimensions remain forbidden.
        [[nodiscard]] bool
        is_all_simd_execution_consistent(minimizer_algorithm selected,
                                         simd_chaining_policy selected_chaining_policy,
                                         bool require_target_sketch,
                                         bool require_nonempty_vector_work) const noexcept {
            const auto& scalar = scalar_pipeline_stats;
            const bool is_scalar_seed_only_consistent =
                scalar_pipeline_execution_stats_available && !scalar.is_saturated &&
                scalar.query_minimizers == scalar_minimizer_execution_stats{} &&
                scalar.target_minimizers == scalar_minimizer_execution_stats{} &&
                scalar.seed_lookup_calls > 0u &&
                (!require_nonempty_vector_work || scalar.seed_query_minimizers > 0u) &&
                scalar.chain_stage_completion_calls == 0u &&
                scalar.chain_stage_completed_anchor_inputs == 0u &&
                scalar.chain_score_calls == 0u && scalar.chain_backtrack_calls == 0u &&
                scalar.chain_anchor_inputs == 0u && scalar.ksw2_left_calls == 0u &&
                scalar.ksw2_gap_calls == 0u && scalar.ksw2_right_calls == 0u &&
                scalar.ksw2_input_cell_area == 0u;
            return is_scalar_seed_only_consistent && !ksw2_batched_execution_stats_available &&
                   ksw2_batched_stats == ksw2_batched_execution_stats{} &&
                   is_selected_simd_sketch_execution_consistent(
                       selected, require_target_sketch, require_nonempty_vector_work) &&
                   is_simd_chaining_execution_consistent(selected_chaining_policy,
                                                         require_nonempty_vector_work) &&
                   is_block_aligner_execution_consistent(require_nonempty_vector_work);
        }

        /// Validate a complete all-scalar pipeline route.
        [[nodiscard]] bool
        is_all_scalar_execution_consistent(const bool require_target_sketch,
                                           const bool require_alignment) const noexcept {
            const bool simd_chain_absent =
                !simd_chaining_execution_stats_available &&
                simd_chaining_stats == bioseq::chaining::simd_chaining_execution_stats{};
            const bool block_absent = !block_aligner_execution_stats_available &&
                                      block_aligner_stats == block_aligner_execution_stats{};
            const bool batched_absent = !ksw2_batched_execution_stats_available &&
                                        ksw2_batched_stats == ksw2_batched_execution_stats{};
            return is_selected_scalar_sketch_execution_consistent(require_target_sketch,
                                                                  /*require_nonempty_work=*/true) &&
                   scalar_pipeline_stats.is_route_consistent(require_target_sketch,
                                                             require_alignment) &&
                   simd_chain_absent && block_absent && batched_absent &&
                   partial_vulkan_stats == partial_vulkan_execution_stats{};
        }

        /// Validate scalar sketches and require all accelerated sketch stats absent.
        [[nodiscard]] bool is_selected_scalar_sketch_execution_consistent(
            const bool require_target_sketch, const bool require_nonempty_work) const noexcept {
            const bool exact_absent =
                !exact_simd_query_execution_stats_available &&
                !exact_simd_target_execution_stats_available &&
                exact_simd_query_execution_stats == two_stack_execution_stats{} &&
                exact_simd_target_execution_stats == two_stack_execution_stats{};
            const bool bounded_absent =
                !bounded_nthash_query_execution_stats_available &&
                !bounded_nthash_target_execution_stats_available &&
                bounded_nthash_query_execution_stats == bounded_nthash_execution_stats{} &&
                bounded_nthash_target_execution_stats == bounded_nthash_execution_stats{};
            const bool is_target_valid =
                require_target_sketch
                    ? scalar_pipeline_stats.target_minimizers.is_coherent(require_nonempty_work)
                    : scalar_pipeline_stats.target_minimizers == scalar_minimizer_execution_stats{};
            return scalar_pipeline_execution_stats_available &&
                   scalar_pipeline_stats.query_minimizers.is_coherent(require_nonempty_work) &&
                   is_target_valid && exact_absent && bounded_absent &&
                   partial_vulkan_stats == partial_vulkan_execution_stats{};
        }

        /// Return whether any algorithm emitted completed execution statistics.
        [[nodiscard]] bool has_any_algorithm_execution_stats() const noexcept {
            return exact_simd_query_execution_stats_available ||
                   exact_simd_target_execution_stats_available ||
                   bounded_nthash_query_execution_stats_available ||
                   bounded_nthash_target_execution_stats_available ||
                   simd_chaining_execution_stats_available ||
                   block_aligner_execution_stats_available ||
                   scalar_pipeline_execution_stats_available ||
                   ksw2_batched_execution_stats_available ||
                   partial_vulkan_stats.has_any_dispatch();
        }

        /// Validate Block Aligner execution and optional nonempty SIMD work.
        [[nodiscard]] bool
        is_block_aligner_execution_consistent(bool require_nonempty_simd_work) const noexcept {
            if (!block_aligner_execution_stats_available ||
                !block_aligner_stats.is_internally_consistent()) {
                return false;
            }
            if (!require_nonempty_simd_work) {
                return true;
            }
            const auto& stats = block_aligner_stats;
            return stats.executed_calls() > 0 && stats.nonempty_calls > 0 &&
                   stats.nonempty_calls_with_simd == stats.nonempty_calls &&
                   ((stats.simd_dp_vector_batches > 0 && stats.simd_dp_cells > 0) ||
                    stats.simd_diagonal_certificate_calls > 0);
        }

        /// Return the number of alignment results.
        [[nodiscard]] size_t size() const {
            return results.size();
        }

        /// Return whether the collection has no alignment results.
        [[nodiscard]] bool is_empty() const {
            return results.empty();
        }

        /// Borrow the ordered alignment results.
        [[nodiscard]] std::span<const match_result> view() const {
            return std::span<const match_result>(results);
        }

        /// Compute the complete biological result stats while the owning result
        /// vector and all CIGAR payloads are still live.
        [[nodiscard]] match_result_set_identity biological_identity() const noexcept {
            return make_match_result_set_identity(view());
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
        void print(const std::string& query_name = "query",
                   const std::string& target_name = "target") const {
            if (results.empty()) {
                bioseq_log<logging_level::debug>("{}\t0\t0\t0\t*\t*\t0\t0\t0\t0\t0\t0", query_name);
                return;
            }

            for (const auto& match : results) {
                const char strand_char = match.is_reverse ? '-' : '+';
                const char type_char = match.is_primary ? 'P' : 'S';

                std::string line = fmt::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
                                               query_name,
                                               match.query_len,
                                               match.query_start,
                                               match.query_end,
                                               strand_char,
                                               target_name,
                                               match.target_len,
                                               match.target_start,
                                               match.target_end,
                                               match.num_matches,
                                               match.block_length,
                                               match.mapq);

                if (match.has_cigar && !match.cigar.empty()) {
                    // NM = blen - mlen; both are sequence_size_t (uint64) and
                    // blen >= mlen, but compute signed + clamp defensively.
                    const std::int64_t edit_distance =
                        std::max<std::int64_t>(0,
                                               static_cast<std::int64_t>(match.block_length) -
                                                   static_cast<std::int64_t>(match.num_matches));
                    line += fmt::format("\tNM:i:{}\tms:i:{}\tAS:i:{}\tnn:i:0",
                                        edit_distance,
                                        match.dp_max_score,
                                        match.dp_score);
                }

                line += fmt::format("\ttp:A:{}", type_char);
                line += fmt::format("\tcm:i:{}", match.seed_count);
                line += fmt::format("\ts1:i:{}", match.chain_score);

                if (match.is_primary) {
                    line += fmt::format("\ts2:i:{}", match.secondary_chain_score);
                }

                if (match.divergence >= 0.0f && match.divergence <= 1.0f) {
                    const char* divergence_tag =
                        (match.has_cigar && !match.cigar.empty()) ? "\tde:f:" : "\tdv:f:";
                    if (match.divergence == 0.0f) {
                        line += divergence_tag;
                        line += '0';
                    } else {
                        line += fmt::format(
                            "{}{:.4f}", divergence_tag, static_cast<double>(match.divergence));
                    }
                }

                line += "\trl:i:0";

                if (match.has_cigar && !match.cigar.empty()) {
                    line += "\tcg:Z:";
                    line += format_cigar(match.cigar);
                }

                bioseq_log<logging_level::debug>("{}", line);
            }
        }
    };

    } // namespace bioseq::simd

} // export
