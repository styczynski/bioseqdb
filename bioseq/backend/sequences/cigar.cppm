/**
 * @file cigar.cppm
 * @brief CIGAR operations shared by the alignment backends.
 *
 * Build, combine, and format alignment runs. The edit-count helpers use the
 * aligned sequence windows to count mismatches hidden by an M operation.
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

/** @brief M: align a query symbol with a target symbol, including mismatches. */
constexpr std::uint32_t cigar_match_opcode = 0;
/** @brief I: query symbols with no corresponding target symbols. */
constexpr std::uint32_t cigar_insertion_opcode = 1;
/** @brief D: target symbols with no corresponding query symbols. */
constexpr std::uint32_t cigar_deletion_opcode = 2;
/** @brief N: skip symbols in the target sequence. */
constexpr std::uint32_t cigar_reference_skip_opcode = 3;
/** @brief =: aligned symbols declared to be equal. */
constexpr std::uint32_t cigar_equal_opcode = 7;
/** @brief X: aligned symbols declared to differ. */
constexpr std::uint32_t cigar_mismatch_opcode = 8;

/** @brief Character lookup for the minimap2 opcode order `MIDNSHP=XB`. */
constexpr std::array<char, 10> cigar_opcode_characters = {
    'M', 'I', 'D', 'N', 'S', 'H', 'P', '=', 'X', 'B'
};

/** @brief Consecutive CIGAR steps with the same operation code. */
struct cigar_operation {
    /** @brief Number of symbols covered by the operation. */
    std::uint32_t length;
    /** @brief Minimap2-compatible operation code. */
    std::uint32_t opcode;
};

/**
 * @brief Append a packed CIGAR run, merging it with an equal trailing operation.
 * @param cigar Destination words encoded as `(length << 4) | opcode`.
 * @param opcode Operation code stored in the low four bits.
 * @param length Number of steps to append.
 */
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
template <class p_cigar_storage>
inline void push_packed_cigar(
    p_cigar_storage& cigar,
#else
inline void push_packed_cigar(
    std::vector<std::uint32_t>& cigar,
#endif
    std::uint32_t opcode,
    std::uint32_t length)
{
    if (!cigar.empty() && (cigar.back() & 0xfU) == opcode)
        cigar.back() += length << 4;
    else
        cigar.push_back((length << 4) | opcode);
}

/**
 * @brief Decode packed CIGAR runs and append them, merging equal adjacent runs.
 * @param destination Output operations, which may already contain runs.
 * @param source KSW2 words encoded as `(length << 4) | opcode`.
 */
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
template <class p_destination_storage, class p_source_storage>
inline void append_cigar(
    p_destination_storage& destination,
    const p_source_storage& source)
#else
inline void append_cigar(
    std::vector<cigar_operation>& destination,
    const std::vector<std::uint32_t>& source)
#endif
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
 * @brief Count edits in the alignment described by a packed CIGAR.
 *
 * M includes mismatches. Compare those aligned symbols because the block
 * aligner converts both matches and mismatches to M when it exports a CIGAR.
 * Each substitution, insertion, deletion, or skipped target symbol costs one.
 * The = and X operations are trusted as written.
 *
 * @tparam p_strand_reversed Decode the query as a reverse complement.
 * @tparam p_alphabet Alphabet used by both sequences.
 * @param query_seq Query sequence to read from.
 * @param target_seq Target sequence to read from.
 * @param query_start First query position in the selected window.
 * @param query_end Position just past the query window.
 * @param target_start First target position in the selected window.
 * @param target_end Position just past the target window.
 * @param cigar Packed operations covering both windows exactly.
 * @return Number of edits in this alignment.
 * @throws std::invalid_argument If a window ends before it starts.
 * @throws std::runtime_error If an operation is unsupported or the CIGAR
 * consumes a different number of symbols from either selected window.
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
 * @brief Add the seed span that lies between independently aligned extensions.
 *
 * Pair the common length with M operations. Any extra query symbols become an
 * insertion, and any extra target symbols become a deletion. Merge the result
 * with an equal trailing operation already in the output.
 *
 * @param cigar Destination operations.
 * @param query_length Query length of the seed span.
 * @param target_length Target length of the seed span.
 */
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
template <class p_cigar_storage>
inline void append_implicit_seed_cigar(
    p_cigar_storage& cigar,
#else
inline void append_implicit_seed_cigar(
    std::vector<cigar_operation>& cigar,
#endif
    std::size_t query_length,
    std::size_t target_length)
{
    // Skip empty runs and merge an operation with an equal trailing run.
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
 * @brief Count edits in a seed span that was skipped by gap alignment.
 *
 * A single-seed region aligns only its left and right extensions, so the seed
 * needs its own comparison even when the minimizer hashes agree.
 * Hashes can collide. Compare corresponding symbols, then count any remaining
 * length as insertions or deletions.
 *
 * @tparam p_strand_reversed Decode the query as a reverse complement.
 * @tparam p_alphabet Alphabet used by both sequences.
 * @param query_seq Query sequence to read from.
 * @param target_seq Target sequence to read from.
 * @param query_start First query position in the seed.
 * @param query_end Position just past the query seed.
 * @param target_start First target position in the seed.
 * @param target_end Position just past the target seed.
 * @return Edits in the seed alignment produced by append_implicit_seed_cigar().
 * @throws std::invalid_argument If a window ends before it starts.
 * @throws std::out_of_range If a window extends past its sequence.
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
template <class p_cigar_storage>
[[nodiscard]] inline std::string format_cigar(const p_cigar_storage& cigar) {
    const std::span<const cigar_operation> operations{cigar.data(), cigar.size()};
    std::string cigar_string;
    cigar_string.reserve(operations.size() * 5);
    for (const auto& cigar_op : operations) {
        cigar_string += std::to_string(cigar_op.length);
        if (cigar_op.opcode < cigar_opcode_characters.size()) {
            cigar_string += cigar_opcode_characters[cigar_op.opcode];
        } else {
            cigar_string += '?';
        }
    }
    return cigar_string;
}

/** @brief Operation used in the block-aligner traceback. */
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
 * @brief Store block-aligner traceback as runs of equal operations.
 *
 * Add steps while tracing backwards, then call reverse() to put the runs in
 * alignment order. Index zero is a sentinel and is omitted from the output.
 */
class block_cigar {
public:
    /** @brief Leave storage unallocated. Call clear() before adding steps. */
    block_cigar() = default;

    /**
     * @brief Prepare an empty traceback buffer with space for the expected steps.
     * @param maximum_length Capacity hint for the number of traceback steps.
     */
    explicit block_cigar(std::size_t maximum_length) {
        // Allocate space for the expected trace, but construct only its sentinel
        // until operations are added.
        ops_.reserve(maximum_length + 5);
        ops_.resize(1);
        ops_[0] = block_cigar_entry{};
        size_ = 1;
    }

    /** @brief Reset the buffer, keeping its allocated capacity. */
    void clear() {
        ops_.resize(1);
        ops_[0] = block_cigar_entry{};
        size_ = 1;
    }

    /**
     * @brief Add a traceback step, extending the last run if its operation matches.
     * @param operation Operation for this step.
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
     * @brief Convert the runs to packed CIGAR, combining matches and mismatches as M.
     * @return Words encoded as `(length << 4) | opcode`.
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
            // Export matches and mismatches as M. Callers that need an edit count
            // must compare the corresponding sequence symbols.
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
