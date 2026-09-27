/**
 * @file vulkan/abi/sequence_batch.cppm
 * @brief Batch record layouts for packed sequences and their GPU outputs.
 *
 * These structures describe buffer offsets and counts in a fixed field order.
 */
module;

export module bioseq.gpu.vulkan.abi:sequence_batch;

import std;

export namespace bioseq::gpu::vulkan {

/** @brief Sequence role and reverse-complement flags. */
enum class gpu_sequence_record_flags : std::uint32_t {
    none = 0u,                     ///< No routing flags set.
    reverse_complement = 1u << 0u, ///< Request reverse-complement reads of the packed sequence.
    target_record = 1u << 1u,      ///< Record describes a target sequence slice.
    query_record = 1u << 2u,       ///< Record describes a query sequence slice.
};

/**
 * @brief Combines sequence flags.
 */
[[nodiscard]] constexpr gpu_sequence_record_flags
operator|(gpu_sequence_record_flags left, gpu_sequence_record_flags right) noexcept {
    return static_cast<gpu_sequence_record_flags>(static_cast<std::uint32_t>(left) |
                                                  static_cast<std::uint32_t>(right));
}

/**
 * @brief Locates a sequence in a shared packed buffer and describes its encoding.
 *
 * `seq_word_offset` uses 32-bit words. The unit and alphabet fields describe
 * how to extract symbol ranks from those words.
 */
struct gpu_sequence_view_wire {
    std::uint64_t seq_word_offset;   ///< Offset in the packed buffer, in 32-bit words.
    std::uint64_t byte_count;        ///< Bytes containing sequence data, before padding.
    std::uint64_t padded_byte_count; ///< Byte count rounded up to a multiple of four.
    std::uint64_t unit_count;        ///< Number of packed sequence units.
    std::uint64_t bp_offset;         ///< Starting base-pair offset of this sequence record.
    std::uint64_t bp_count;          ///< Length of this sequence record in base pairs.
    std::uint32_t letters_per_unit;  ///< Number of symbols stored in each unit.
    std::uint32_t bits_per_char;     ///< Number of bits used for each symbol.
    std::uint32_t unit_bits;         ///< Number of bits in one packed unit.
    std::uint32_t alphabet_size;     ///< Number of symbols in the alphabet.
    std::uint32_t char_mask;         ///< Mask for one symbol rank, truncated to 32 bits.
    std::uint32_t padding_letters;   ///< Unused symbol slots in the final packed unit.
    std::uint32_t alphabet_kind;     ///< `alphabet_kernel_kind` from the upload view.
    std::uint32_t reserved;          ///< Reserved. Set to zero on upload.
};

/**
 * @brief Describes a query and the output ranges reserved for its search.
 *
 * Offsets and counts address minimizer, anchor, and chain-range arrays.
 */
struct gpu_query_record_wire {
    gpu_sequence_view_wire sequence;  ///< Packed query location and encoding.
    std::uint64_t minimizer_offset;   ///< First minimizer record for this query.
    std::uint64_t minimizer_count;    ///< Number of query minimizers.
    std::uint64_t anchor_offset;      ///< Output anchor array offset in records.
    std::uint64_t anchor_count;       ///< Number of query anchors.
    std::uint64_t chain_range_offset; ///< Output chain-range array offset in records.
    std::uint64_t chain_range_count;  ///< Number of query chain ranges.
    std::uint32_t record_id;          ///< Stable query record identifier for the batch.
    std::uint32_t flags;              ///< `gpu_sequence_record_flags` bit mask.
};

/**
 * @brief Describes a target slice and its minimizer pairs.
 *
 * The global base-pair offset translates hits within this slice to positions
 * in the full target sequence.
 */
struct gpu_target_record_wire {
    gpu_sequence_view_wire sequence;       ///< Packed target location and encoding.
    std::uint64_t target_global_bp_offset; ///< Base-pair origin of this slice in the full target.
    std::uint64_t target_id;               ///< Stable target identifier within the batch.
    std::uint64_t minimizer_pair_offset;   ///< Minimizer-pair array offset in records.
    std::uint64_t minimizer_pair_count;    ///< Number of minimizer pairs for this target slice.
    std::uint32_t record_id;               ///< Stable target record identifier for the batch.
    std::uint32_t flags;                   ///< `gpu_sequence_record_flags` bit mask.
};
static_assert(std::is_standard_layout_v<gpu_sequence_view_wire>);
static_assert(std::is_standard_layout_v<gpu_query_record_wire>);
static_assert(std::is_standard_layout_v<gpu_target_record_wire>);

} // namespace bioseq::gpu::vulkan
