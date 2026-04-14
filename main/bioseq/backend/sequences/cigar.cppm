/**
 * @file cigar.cppm
 * @brief Unified CIGAR utilities for all alignment backends.
 *
 * Provides CIGAR opcode constants, operation types, formatting, and
 * conversion utilities shared by KSW2, block aligner, and the minimap2 port.
 */
module;

#include <hwy/highway.h>

#include "common.h"

#include <cstdint>
#include <vector>
#include <array>
#include <string>
#include <algorithm>

export module bioseq_cigar;

import mlogging;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {

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

    explicit BlockCigar(size_t max_len)
        : ops_(max_len + 5, BlockCigarEntry{BlockCigarOp::Sentinel, 0})
        , idx_(1) {}

    void clear() {
        std::fill(ops_.begin(), ops_.end(),
                  BlockCigarEntry{BlockCigarOp::Sentinel, 0});
        idx_ = 1;
    }

    void add(BlockCigarOp op) {
        const size_t add = (op != ops_[idx_ - 1].op) ? 1 : 0;
        idx_ += add;
        ops_[idx_ - 1].op = op;
        ops_[idx_ - 1].len += 1;
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
            result.push_back((static_cast<uint32_t>(entry.len) << 4) | opcode);
        }
        return result;
    }

private:
    std::vector<BlockCigarEntry> ops_;
    size_t idx_ = 1;
};

}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}  // export
