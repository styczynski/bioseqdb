/**
 * @file pipeline/minimap_port_parts/results.cppm
 * @brief Mapping result contracts and stable biological identities.
 */
export module bioseq.pipeline.minimap_port:results;

import std;
import bioseq.highway.dispatch_contract;
import bioseq.chaining.core;
import bioseq.chaining.scalar;
import bioseq.chaining.simd;
import bioseq.chaining.rmq;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_index;
import bioseq.sequences.cigar;
import bioseq.storage.size;

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

    } // namespace bioseq::simd

} // export
