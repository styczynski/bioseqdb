/**
 * @file cigar.cppm
 * @brief Unified CIGAR utilities for all alignment backends.
 *
 * Provides CIGAR opcode constants, operation types, formatting, and
 * conversion utilities shared by KSW2, block aligner, and the minimap2 port.
 */
module;

#include "common.h"

#include <cstdint>
#include <vector>
#include <array>
#include <string>
#include <algorithm>
#include <span>
#include <stdexcept>

export module bioseq.sequences.cigar;

import bioseq.common.mlogging;
import bioseq.sequences;
import bioseq.storage.lazy;
import bioseq.storage.size;

export namespace bioseq::simd {

// ============================================================================
// CIGAR operation codes — matches minimap2 (minimap.h) and KSW2
// ============================================================================

constexpr uint32_t CIGAR_MATCH  = 0;
constexpr uint32_t CIGAR_INS    = 1;
constexpr uint32_t CIGAR_DEL    = 2;
constexpr uint32_t CIGAR_N_SKIP = 3;
constexpr uint32_t CIGAR_EQ     = 7;
constexpr uint32_t CIGAR_X      = 8;

/// Character representation of CIGAR ops (MM_CIGAR_STR "MIDNSHP=XB").
constexpr std::array<char, 10> CIGAR_OP_CHAR = {
    'M', 'I', 'D', 'N', 'S', 'H', 'P', '=', 'X', 'B'
};

// ============================================================================
// CigarOp — single CIGAR operation (length + opcode)
// ============================================================================

struct CigarOp {
    uint32_t length;
    uint32_t opcode;
};

// ============================================================================
// Packed CIGAR helpers (KSW2 format: length<<4 | opcode)
// ============================================================================

/// Push a CIGAR operation in packed format, merging with the last op if same
/// opcode.
inline void push_packed_cigar(
    std::vector<uint32_t>& cigar, uint32_t op, uint32_t len)
{
    if (!cigar.empty() && (cigar.back() & 0xfU) == op)
        cigar.back() += len << 4;
    else
        cigar.push_back((len << 4) | op);
}

/// Append packed CIGAR ops (length<<4 | opcode) to a CigarOp vector,
/// merging adjacent same-opcode entries.
inline void append_cigar(
    std::vector<CigarOp>& destination, const std::vector<uint32_t>& source)
{
    for (uint32_t entry : source) {
        const uint32_t opcode    = entry & 0xfU;
        const uint32_t op_length = entry >> 4;
        if (!destination.empty() && destination.back().opcode == opcode) {
            destination.back().length += op_length;
        } else {
            destination.push_back({op_length, opcode});
        }
    }
}

/**
 * Compute the exact unit-cost edit distance represented by one packed CIGAR
 * and its already-clipped sequence windows. Generic M operations must be
 * compared against the underlying alphabet ranks because the block aligner
 * deliberately projects both Eq and X to M at its public CIGAR boundary.
 */
template <bool strand_reversed,
          sequence_alphabet Alphabet>
[[nodiscard]] bioseq::storage::sequence_size_t
exact_edit_distance_from_packed_cigar(
    bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> target_seq,
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
    query_seq.template to_encoded_into<strand_reversed, false>(
        std::span<std::uint8_t>(query_ranks), query_start, query_end);
    target_seq.template to_encoded_into<false, false>(
        std::span<std::uint8_t>(target_ranks), target_start, target_end);

    std::size_t query_position = 0;
    std::size_t target_position = 0;
    bioseq::storage::sequence_size_t edits = 0;
    for (const std::uint32_t packed : cigar) {
        const std::uint32_t opcode = packed & 0xfU;
        const std::size_t length = static_cast<std::size_t>(packed >> 4);
        const bool consumes_query = opcode == CIGAR_MATCH
            || opcode == CIGAR_EQ || opcode == CIGAR_X
            || opcode == CIGAR_INS;
        const bool consumes_target = opcode == CIGAR_MATCH
            || opcode == CIGAR_EQ || opcode == CIGAR_X
            || opcode == CIGAR_DEL || opcode == CIGAR_N_SKIP;
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
            case CIGAR_MATCH:
                for (std::size_t offset = 0; offset < length; ++offset) {
                    edits += query_ranks[query_position + offset]
                        != target_ranks[target_position + offset];
                }
                break;
            case CIGAR_EQ:
                break;
            case CIGAR_X:
            case CIGAR_INS:
            case CIGAR_DEL:
            case CIGAR_N_SKIP:
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
 * Append the implicit seed span shared by alignment backends. The common
 * prefix is represented as a match, followed by the query-only insertion or
 * target-only deletion tail. Adjacent equal opcodes retain the canonical
 * run-length merge used by append_cigar().
 */
inline void append_implicit_seed_cigar(
    std::vector<CigarOp>& cigar,
    std::size_t query_length,
    std::size_t target_length)
{
    const auto append_op = [&cigar](std::uint32_t opcode, std::size_t length) {
        if (length == 0) return;
        if (!cigar.empty() && cigar.back().opcode == opcode) {
            cigar.back().length += static_cast<std::uint32_t>(length);
        } else {
            cigar.push_back(CigarOp{
                .length = static_cast<std::uint32_t>(length),
                .opcode = opcode,
            });
        }
    };

    const std::size_t common_length = std::min(query_length, target_length);
    append_op(CIGAR_MATCH, common_length);
    if (query_length > common_length) {
        append_op(CIGAR_INS, query_length - common_length);
    } else if (target_length > common_length) {
        append_op(CIGAR_DEL, target_length - common_length);
    }
}

/**
 * Count the implicit seed alignment of a single-seed region.
 *
 * Multi-seed regions include the first seed in their first mandatory gap
 * fill. A single-seed region has no such fill, so the seed lies between the
 * independently aligned left and right extensions. A minimizer hash is not
 * a proof that the underlying ranks are equal; exact edit-distance callers
 * must compare this span explicitly.
 */
template <bool strand_reversed,
          sequence_alphabet Alphabet>
[[nodiscard]] bioseq::storage::sequence_size_t
exact_edit_distance_for_implicit_seed(
    bioseq::storage::basic_lazy_sequence_view<Alphabet> query_seq,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> target_seq,
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
    query_seq.template to_encoded_into<strand_reversed, false>(
        std::span<std::uint8_t>(query_ranks),
        query_start,
        query_start + common_length);
    target_seq.template to_encoded_into<false, false>(
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

/// Format a CigarOp vector as a human-readable string (e.g. "58M1I33M").
[[nodiscard]] inline std::string format_cigar(const std::vector<CigarOp>& cigar)
{
    std::string cigar_string;
    cigar_string.reserve(cigar.size() * 5);
    for (const auto& cigar_op : cigar) {
        cigar_string += std::to_string(cigar_op.length);
        if (cigar_op.opcode < CIGAR_OP_CHAR.size()) {
            cigar_string += CIGAR_OP_CHAR[cigar_op.opcode];
        } else {
            cigar_string += '?';
        }
    }
    return cigar_string;
}

// ============================================================================
// Block CIGAR — run-length-encoded CIGAR for block aligner traceback
// ============================================================================

enum class BlockCigarOp : uint8_t {
    Sentinel = 0, M = 1, Eq = 2, X = 3, I = 4, D = 5
};

struct BlockCigarEntry {
    BlockCigarOp op = BlockCigarOp::Sentinel;
    size_t len = 0;
};

/// Run-length-encoded CIGAR. Operations are added in reverse during
/// traceback, then reversed at the end.
class BlockCigar {
public:
    BlockCigar() = default;

    explicit BlockCigar(size_t max_len) {
        // Reserve without value-init: the previous implementation
        // value-initialised every entry up to `max_len + 5`, which for a
        // 200x200 alignment is ~16 KB of zero-fill per call. The actual
        // working set is `idx_..len()` which is bounded by the number of
        // backtrack steps the aligner emits — usually <100. resize() is
        // still needed because indexed writes happen inside `add(...)`,
        // but we keep the working set tight by sentinelling only the
        // first slot the algorithm reads on the very first add(...) call.
        ops_.reserve(max_len + 5);
        ops_.resize(1);
        ops_[0] = BlockCigarEntry{BlockCigarOp::Sentinel, 0};
        idx_   = 1;
    }

    void clear() {
        // Reset to a single sentinel; downstream `add(...)` calls
        // resize ops_ implicitly via push-style growth.
        ops_.resize(1);
        ops_[0] = BlockCigarEntry{BlockCigarOp::Sentinel, 0};
        idx_ = 1;
    }

    void add(BlockCigarOp op) {
        // Grow the working set as the backtrack writes new entries; the
        // initial slot is the sentinel set by the constructor / clear()
        // path. Without this, ops_[idx_] is past-end and writing it is UB.
        if (op != ops_[idx_ - 1].op) {
            if (idx_ >= ops_.size()) {
                ops_.resize(idx_ + 1, BlockCigarEntry{BlockCigarOp::Sentinel, 0});
            }
            ++idx_;
            ops_[idx_ - 1].op = op;
            ops_[idx_ - 1].len = 1;
        } else {
            ops_[idx_ - 1].len += 1;
        }
    }

    void reverse() {
        std::reverse(ops_.begin() + 1,
                     ops_.begin() + static_cast<ptrdiff_t>(idx_));
    }

    [[nodiscard]] size_t len() const { return idx_ - 1; }

    /// Convert to packed CIGAR format: vector of (length << 4 | opcode).
    [[nodiscard]] std::vector<uint32_t> to_packed_cigar() const {
        std::vector<uint32_t> result;
        result.reserve(len());
        for (size_t k = 1; k < idx_; ++k) {
            const auto& entry = ops_[k];
            uint32_t opcode = 0;
            switch (entry.op) {
                case BlockCigarOp::M:
                case BlockCigarOp::Eq:
                case BlockCigarOp::X:  opcode = CIGAR_MATCH; break;
                case BlockCigarOp::I:  opcode = CIGAR_INS; break;
                case BlockCigarOp::D:  opcode = CIGAR_DEL; break;
                default: continue;
            }
            // Traceback keeps Eq/X distinct so callers can inspect exact
            // edit semantics, while the pipeline ABI deliberately projects
            // both to minimap/KSW's generic M opcode.  Coalesce after that
            // projection: otherwise an Eq/X boundary becomes adjacent M
            // records (for example 32M1M), unlike the upstream `cigar()`
            // result and unlike canonical packed CIGAR.
            push_packed_cigar(
                result, opcode, static_cast<uint32_t>(entry.len));
        }
        return result;
    }

private:
    std::vector<BlockCigarEntry> ops_;
    size_t idx_ = 1;
};

}  // namespace bioseq::simd
