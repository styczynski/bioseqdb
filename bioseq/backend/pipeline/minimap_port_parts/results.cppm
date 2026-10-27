/**
 * @file pipeline/minimap_port_parts/results.cppm
 * @brief Shared result types and checksums for comparing alignment results.
 */
export module bioseq.pipeline.minimap_port:results;

import std;
export import bioseq.pipeline.algorithm_identity_contract;
import bioseq.highway.dispatch_contract;
import bioseq.chaining.core;
import bioseq.chaining.scalar;
import bioseq.chaining.simd;
import bioseq.chaining.rmq;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_index;
import bioseq.sequences.cigar;
import bioseq.storage.size;
export import bioseq.pipeline.minimap_port.match_result_contract;

export {

    namespace bioseq::simd {

    // Keep sequence lengths and positions in the shared 64-bit storage type.
    using bioseq::storage::sequence_size_t;

    // Make the shared chaining types and functions available to the pipeline
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

    // Use the index lookup interfaces shared with the index builders and readers.
    using bioseq::indexing::lookup_minimizer_spans;
    using bioseq::indexing::minimizer_hashmap_fragment_index;
    using bioseq::indexing::minimizer_lookup_index;
    using bioseq::indexing::minimizer_lookup_spans;
    using bioseq::indexing::minimizer_position_span;
    using bioseq::indexing::minimizer_seed_index;

    } // namespace bioseq::simd

    namespace bioseq::simd {

    // Result checksums.

    // CIGAR operations come from bioseq.sequences.cigar.

    /// Counts and checksums for comparing ordered alignment results across runs.
    /// Every  result field and CIGAR operation is encoded explicitly, so struct
    /// padding and host byte order do not affect the checksums. Storage and index
    /// checksums are excluded so different execution paths can be compared.
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

        /// Combine another result collection with this running comparison record.
        void merge(const match_result_set_identity& other) noexcept {
            if (!other.is_computed) {
                // Remember, the missing collection even if valid collections arrive later.
                // The saturated flag distinguishes this failure from an empty accumulator.
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

    /// Two byte checksums with  fixed encodings for portable result comparisons.
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

        /// Encode a 32-bit unsigned value as a zero-extended 64-bit value.
        constexpr void add_u32(std::uint32_t value) noexcept {
            add_u64(value);
        }

        /// Encode the 32-bit representution of a signed integer.
        constexpr void add_i32(std::int32_t value) noexcept {
            add_u32(std::bit_cast<std::uint32_t>(value));
        }

        /// Encode false as zero and true as one.
        constexpr void add_bool(bool value) noexcept {
            add_byte(value ? std::uint8_t{1u} : std::uint8_t{0u});
        }

        /// Encode a float using its 32-bit representation.
        constexpr void add_float(float value) noexcept {
            add_u32(std::bit_cast<std::uint32_t>(value));
        }
    };

    } // namespace biological_identity_detail

    /// Count and hash every field and CIGAR operation in result order.
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
            for (const bioseq::cigar_run& operation : result.cigar) {
                // Regression fingerprints retain their native-format identity;
                // SeqAn ranks intentionally use a different operation order.
                const auto native_opcode = bioseq::cigar_native_opcode(operation.operation);
                if (!native_opcode) {
                    stats.is_computed = false;
                    stats.is_saturated = true;
                    return stats;
                }
                match_digest.add_u32(operation.length);
                match_digest.add_u32(*native_opcode);
                cigar_digest.add_u32(operation.length);
                cigar_digest.add_u32(*native_opcode);
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
