/**
 * @file cigar.cppm
 * @brief Unified CIGAR utilities for all alignment backends.
 *
 * Provides CIGAR opcode constants, operation types, formatting, and
 * conversion utilities shared by KSW2, block aligner, and the minimap2 port.
 */
module;

#include <cstdint>
#include <vector>
#include <array>
#include <string>
#include <algorithm>
#include <span>
#include <stdexcept>
#include <cstddef>

export module bioseq.sequences.cigar;

import bioseq.sequences;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;

export namespace bioseq::simd {

/** @brief Generic aligned-pair opcode used by minimap2 and KSW2. */
constexpr std::uint32_t cigar_match_opcode = 0;
/** @brief Query-insertion opcode used by minimap2 and KSW2. */
constexpr std::uint32_t cigar_insertion_opcode = 1;
/** @brief Query-deletion opcode used by minimap2 and KSW2. */
constexpr std::uint32_t cigar_deletion_opcode = 2;
/** @brief Reference-skip opcode used by minimap2 and KSW2. */
constexpr std::uint32_t cigar_reference_skip_opcode = 3;
/** @brief Exact-match opcode used by minimap2 and KSW2. */
constexpr std::uint32_t cigar_equal_opcode = 7;
/** @brief Mismatch opcode used by minimap2 and KSW2. */
constexpr std::uint32_t cigar_mismatch_opcode = 8;

/** @brief Character lookup for the minimap2 opcode order `MIDNSHP=XB`. */
constexpr std::array<char, 10> cigar_opcode_characters = {
    'M', 'I', 'D', 'N', 'S', 'H', 'P', '=', 'X', 'B'
};

/** @brief One run-length encoded CIGAR operation. */
struct cigar_operation {
    /** @brief Number of symbols covered by the operation. */
    std::uint32_t length;
    /** @brief Minimap2-compatible operation code. */
    std::uint32_t opcode;
};

/**
 * @brief Append one operation in KSW2 packed form.
 *
 * Adjacent operations with the same opcode are merged to keep the result in
 * canonical run-length form.
 *
 * @param cigar Destination vector containing `(length << 4) | opcode` words.
 * @param opcode Minimap2-compatible opcode stored in the low four bits.
 * @param length Number of symbols represented by the operation.
 */
inline void push_packed_cigar(
    std::vector<std::uint32_t>& cigar,
    std::uint32_t opcode,
    std::uint32_t length)
{
    if (!cigar.empty() && (cigar.back() & 0xfU) == opcode)
        cigar.back() += length << 4;
    else
        cigar.push_back((length << 4) | opcode);
}

/**
 * @brief Decode and append packed CIGAR words to an operation vector.
 * @param destination Run-length destination. Equal adjacent opcodes merge.
 * @param source KSW2 words encoded as `(length << 4) | opcode`.
 */
inline void append_cigar(
    std::vector<cigar_operation>& destination,
    const std::vector<std::uint32_t>& source)
{
    for (std::uint32_t entry : source) {
        const std::uint32_t opcode = entry & 0xfU;
        const std::uint32_t operation_length = entry >> 4;
        if (!destination.empty() && destination.back().opcode == opcode) {
            destination.back().length += operation_length;
        } else {
            destination.push_back({operation_length, opcode});
        }
    }
}

/**
 * @brief Compute the exact unit-cost distance represented by a packed CIGAR.
 *
 * The distance is evaluated against the already-clipped sequence windows.
 * Generic M operations must be
 * compared against the underlying alphabet ranks because the block aligner
 * deliberately projects exact matches and mismatches to the generic match
 * opcode at its public CIGAR boundary.
 *
 * @tparam p_strand_reversed Whether query ranks are decoded in reverse-complement
 * order.
 * @tparam p_alphabet Logical alphabet shared by both sequence views.
 * @param query_seq Lazy query view decoded only for the selected window.
 * @param target_seq Lazy target view decoded only for the selected window.
 * @param query_start Inclusive query offset.
 * @param query_end Exclusive query offset.
 * @param target_start Inclusive target offset.
 * @param target_end Exclusive target offset.
 * @param cigar Packed CIGAR words covering both selected windows exactly.
 * @return Unit-cost edit distance represented by the CIGAR and sequence ranks.
 * @throws std::invalid_argument If either window is inverted.
 * @throws std::runtime_error If the CIGAR is unsupported or does not cover the
 * selected windows exactly.
 */
template <bool p_strand_reversed,
          sequence_alphabet p_alphabet>
[[nodiscard]] bioseq::storage::sequence_size_t
exact_edit_distance_from_packed_cigar(
    bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
    std::size_t query_start,
    std::size_t query_end,
    std::size_t target_start,
    std::size_t target_end,
    std::span<const std::uint32_t> cigar)
{
    if (query_end < query_start || target_end < target_start) {
        throw std::invalid_argument("exact edit-distance window is inverted");
    }
    const std::size_t query_length = query_end - query_start;
    const std::size_t target_length = target_end - target_start;
    std::vector<std::uint8_t> query_ranks(query_length);
    std::vector<std::uint8_t> target_ranks(target_length);
    query_seq.template copy_ranks<p_strand_reversed, false>(
        std::span<std::uint8_t>(query_ranks), query_start, query_end);
    target_seq.template copy_ranks<false, false>(
        std::span<std::uint8_t>(target_ranks), target_start, target_end);

    std::size_t query_position = 0;
    std::size_t target_position = 0;
    bioseq::storage::sequence_size_t edits = 0;
    for (const std::uint32_t packed : cigar) {
        const std::uint32_t opcode = packed & 0xfU;
        const std::size_t length = static_cast<std::size_t>(packed >> 4);
        const bool consumes_query = opcode == cigar_match_opcode
            || opcode == cigar_equal_opcode || opcode == cigar_mismatch_opcode
            || opcode == cigar_insertion_opcode;
        const bool consumes_target = opcode == cigar_match_opcode
            || opcode == cigar_equal_opcode || opcode == cigar_mismatch_opcode
            || opcode == cigar_deletion_opcode
            || opcode == cigar_reference_skip_opcode;
        if ((consumes_query
                && (query_position > query_length
                    || length > query_length - query_position))
            || (consumes_target
                && (target_position > target_length
                    || length > target_length - target_position))) {
            throw std::runtime_error(
                "alignment CIGAR consumes beyond its exact edit-distance window");
        }

        switch (opcode) {
            case cigar_match_opcode:
                for (std::size_t offset = 0; offset < length; ++offset) {
                    edits += query_ranks[query_position + offset]
                        != target_ranks[target_position + offset];
                }
                break;
            case cigar_equal_opcode:
                break;
            case cigar_mismatch_opcode:
            case cigar_insertion_opcode:
            case cigar_deletion_opcode:
            case cigar_reference_skip_opcode:
                edits += static_cast<bioseq::storage::sequence_size_t>(length);
                break;
            default:
                throw std::runtime_error(
                    "alignment CIGAR contains an unsupported exact edit-distance opcode");
        }
        if (consumes_query) query_position += length;
        if (consumes_target) target_position += length;
    }
    if (query_position != query_length || target_position != target_length) {
        throw std::runtime_error(
            "alignment CIGAR does not cover its exact edit-distance window");
    }
    return edits;
}

/**
 * @brief Append the implicit seed span shared by alignment backends.
 *
 * The common
 * prefix is represented as a match, followed by the query-only insertion or
 * target-only deletion tail. Adjacent equal opcodes retain the canonical
 * run-length merge used by append_cigar().
 *
 * @param cigar Destination operation vector.
 * @param query_length Number of query symbols in the seed span.
 * @param target_length Number of target symbols in the seed span.
 */
inline void append_implicit_seed_cigar(
    std::vector<cigar_operation>& cigar,
    std::size_t query_length,
    std::size_t target_length)
{
    // Appends a nonempty operation while coalescing an equal trailing opcode.
    const auto append_op = [&cigar](std::uint32_t opcode, std::size_t length) {
        if (length == 0) return;
        if (!cigar.empty() && cigar.back().opcode == opcode) {
            cigar.back().length += static_cast<std::uint32_t>(length);
        } else {
            cigar.push_back(cigar_operation{
                .length = static_cast<std::uint32_t>(length),
                .opcode = opcode,
            });
        }
    };

    const std::size_t common_length = std::min(query_length, target_length);
    append_op(cigar_match_opcode, common_length);
    if (query_length > common_length) {
        append_op(cigar_insertion_opcode, query_length - common_length);
    } else if (target_length > common_length) {
        append_op(cigar_deletion_opcode, target_length - common_length);
    }
}

/**
 * @brief Count exact edits in the implicit seed of a single-seed region.
 *
 * Multi-seed regions include the first seed in their first mandatory gap
 * fill. A single-seed region has no such fill, so the seed lies between the
 * independently aligned left and right extensions. Equal minimizer hashes do
 * not establish equal underlying ranks, so exact-distance callers compare the
 * seed span explicitly.
 *
 * @tparam p_strand_reversed Whether query ranks are decoded in reverse-complement
 * order.
 * @tparam p_alphabet Logical alphabet shared by both sequence views.
 * @param query_seq Lazy query view.
 * @param target_seq Lazy target view.
 * @param query_start Inclusive query offset.
 * @param query_end Exclusive query offset.
 * @param target_start Inclusive target offset.
 * @param target_end Exclusive target offset.
 * @return Unit-cost edit distance across the implicit seed span.
 * @throws std::invalid_argument If either window is inverted.
 * @throws std::out_of_range If either window exceeds its sequence.
 */
template <bool p_strand_reversed,
          sequence_alphabet p_alphabet>
[[nodiscard]] bioseq::storage::sequence_size_t
exact_edit_distance_for_implicit_seed(
    bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
    std::size_t query_start,
    std::size_t query_end,
    std::size_t target_start,
    std::size_t target_end)
{
    if (query_end < query_start || target_end < target_start) {
        throw std::invalid_argument(
            "implicit seed edit-distance window is inverted");
    }
    if (query_end > query_seq.bp_size() || target_end > target_seq.bp_size()) {
        throw std::out_of_range(
            "implicit seed edit-distance window exceeds the sequence");
    }

    const std::size_t query_length = query_end - query_start;
    const std::size_t target_length = target_end - target_start;
    const std::size_t common_length = std::min(query_length, target_length);
    std::vector<std::uint8_t> query_ranks(common_length);
    std::vector<std::uint8_t> target_ranks(common_length);
    query_seq.template copy_ranks<p_strand_reversed, false>(
        std::span<std::uint8_t>(query_ranks),
        query_start,
        query_start + common_length);
    target_seq.template copy_ranks<false, false>(
        std::span<std::uint8_t>(target_ranks),
        target_start,
        target_start + common_length);

    bioseq::storage::sequence_size_t edits =
        static_cast<bioseq::storage::sequence_size_t>(
            query_length > target_length
                ? query_length - target_length
                : target_length - query_length);
    for (std::size_t offset = 0; offset < common_length; ++offset) {
        edits += query_ranks[offset] != target_ranks[offset];
    }
    return edits;
}

/**
 * @brief Format operations as a human-readable CIGAR string.
 * @param cigar Run-length operations to render.
 * @return Text such as `58M1I33M`. Unknown opcodes render as `?`.
 */
[[nodiscard]] inline std::string format_cigar(
    const std::vector<cigar_operation>& cigar)
{
    std::string cigar_string;
    cigar_string.reserve(cigar.size() * 5);
    for (const auto& cigar_op : cigar) {
        cigar_string += std::to_string(cigar_op.length);
        if (cigar_op.opcode < cigar_opcode_characters.size()) {
            cigar_string += cigar_opcode_characters[cigar_op.opcode];
        } else {
            cigar_string += '?';
        }
    }
    return cigar_string;
}

/** @brief Traceback operation retained by the block aligner. */
enum class block_cigar_operation : std::uint8_t {
    sentinel = 0,
    match = 1,
    equal = 2,
    mismatch = 3,
    insertion = 4,
    deletion = 5,
};

/** @brief One run in a block-aligner traceback. */
struct block_cigar_entry {
    /** @brief Traceback operation for this run. */
    block_cigar_operation operation = block_cigar_operation::sentinel;
    /** @brief Number of traceback steps in this run. */
    std::size_t length = 0;
};

/**
 * @brief Compact run-length traceback buffer for the block aligner.
 *
 * Operations are appended in traceback order and reversed once traceback
 * completes. Storage grows only with emitted runs, preserving the bounded
 * working set expected by short block alignments.
 */
class block_cigar {
public:
    /** @brief Construct an empty buffer without reserving traceback storage. */
    block_cigar() = default;

    /**
     * @brief Construct an empty buffer with a traceback capacity hint.
     * @param maximum_length Upper-bound hint for traceback steps.
     */
    explicit block_cigar(std::size_t maximum_length) {
        // Reserve capacity without initializing every potential traceback
        // entry. Only the sentinel is materialized until runs are appended.
        ops_.reserve(maximum_length + 5);
        ops_.resize(1);
        ops_[0] = block_cigar_entry{};
        size_ = 1;
    }

    /** @brief Reset the buffer while retaining allocated capacity. */
    void clear() {
        ops_.resize(1);
        ops_[0] = block_cigar_entry{};
        size_ = 1;
    }

    /**
     * @brief Append one traceback step, coalescing an equal preceding run.
     * @param operation Operation emitted by the current traceback step.
     */
    void add(block_cigar_operation operation) {
        if (operation != ops_[size_ - 1].operation) {
            if (size_ >= ops_.size()) {
                ops_.resize(size_ + 1, block_cigar_entry{});
            }
            ++size_;
            ops_[size_ - 1].operation = operation;
            ops_[size_ - 1].length = 1;
        } else {
            ops_[size_ - 1].length += 1;
        }
    }

    /** @brief Reverse accumulated runs from traceback order to forward order. */
    void reverse() {
        std::reverse(ops_.begin() + 1,
                     ops_.begin() + static_cast<std::ptrdiff_t>(size_));
    }

    /** @return Number of operation runs, excluding the sentinel. */
    [[nodiscard]] std::size_t size() const noexcept { return size_ - 1; }

    /**
     * @brief Convert traceback runs to canonical KSW2 packed CIGAR words.
     * @return Vector of `(length << 4) | opcode` words.
     */
    [[nodiscard]] std::vector<std::uint32_t> to_packed_cigar() const {
        std::vector<std::uint32_t> result;
        result.reserve(size());
        for (std::size_t k = 1; k < size_; ++k) {
            const auto& entry = ops_[k];
            std::uint32_t opcode = 0;
            switch (entry.operation) {
                case block_cigar_operation::match:
                case block_cigar_operation::equal:
                case block_cigar_operation::mismatch:
                    opcode = cigar_match_opcode;
                    break;
                case block_cigar_operation::insertion:
                    opcode = cigar_insertion_opcode;
                    break;
                case block_cigar_operation::deletion:
                    opcode = cigar_deletion_opcode;
                    break;
                default: continue;
            }
            // Traceback keeps equality and mismatch distinct, while the
            // packed boundary projects both to the generic match opcode.
            push_packed_cigar(
                result, opcode, static_cast<std::uint32_t>(entry.length));
        }
        return result;
    }

private:
    std::vector<block_cigar_entry> ops_;
    std::size_t size_ = 1;
};

}  // namespace bioseq::simd
