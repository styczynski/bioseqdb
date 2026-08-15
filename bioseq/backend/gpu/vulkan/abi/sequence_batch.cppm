/**
 * @file
 * @brief Defines packed-sequence batch records shared with Vulkan shaders.
 */
module;

#include <cstdint>
#include <type_traits>

export module bioseq.gpu.vulkan.abi:sequence_batch;

export namespace bioseq::gpu::vulkan {

/** @brief Tags the role and strand routing of one packed sequence record. */
enum class gpu_sequence_record_flags : std::uint32_t {
    none = 0u,
    reverse_complement = 1u << 0u,
    target_record = 1u << 1u,
    query_record = 1u << 2u,
};

/**
 * @brief Combines packed-sequence routing flags.
 * @param[in] left First routing mask.
 * @param[in] right Second routing mask.
 * @return Bitwise union of both masks.
 */
[[nodiscard]] constexpr gpu_sequence_record_flags operator|(
    gpu_sequence_record_flags left,
    gpu_sequence_record_flags right) noexcept {
    return static_cast<gpu_sequence_record_flags>(
        static_cast<std::uint32_t>(left)
        | static_cast<std::uint32_t>(right));
}

/**
 * @brief Describes one logical record in a shared packed-sequence buffer.
 *
 * `seq_word_offset` is measured in 32-bit words. Unit traits let shaders read
 * ranks without assuming a particular alphabet.
 */
struct gpu_sequence_view_wire {
    std::uint64_t seq_word_offset;   ///< uint32 word offset in the packed buffer.
    std::uint64_t byte_count;        ///< visible bytes before round-up padding.
    std::uint64_t padded_byte_count; ///< byte_count rounded to a 4-byte word.
    std::uint64_t unit_count;        ///< number of compact_sequence_range units.
    std::uint64_t bp_offset;         ///< base-pair offset of the logical record.
    std::uint64_t bp_count;          ///< logical record length in base pairs.
    std::uint32_t letters_per_unit;  ///< traits::letters_per_unit.
    std::uint32_t bits_per_char;     ///< traits::bits_per_char.
    std::uint32_t unit_bits;         ///< traits::unit_width.
    std::uint32_t alphabet_size;     ///< traits::alphabet_size.
    std::uint32_t char_mask;         ///< traits::char_mask truncated to 32 bits.
    std::uint32_t padding_letters;   ///< sequence padding().
    std::uint32_t alphabet_kind;     ///< alphabet_kernel_kind from the upload view.
    std::uint32_t reserved;
};

/** @brief Binds a query sequence to its minimizer, anchor, and chain slices. */
struct gpu_query_record_wire {
    gpu_sequence_view_wire sequence;
    std::uint64_t minimizer_offset;  ///< output minimizer-word offset (in records).
    std::uint64_t minimizer_count;   ///< filled by count submit.
    std::uint64_t anchor_offset;     ///< output anchor offset.
    std::uint64_t anchor_count;      ///< filled by count submit.
    std::uint64_t chain_range_offset;
    std::uint64_t chain_range_count;
    std::uint32_t record_id;
    std::uint32_t flags;
};

/** @brief Binds a target sequence to its global coordinates and minimizer pairs. */
struct gpu_target_record_wire {
    gpu_sequence_view_wire sequence;
    std::uint64_t target_global_bp_offset;
    std::uint64_t target_id;
    std::uint64_t minimizer_pair_offset;
    std::uint64_t minimizer_pair_count;
    std::uint32_t record_id;
    std::uint32_t flags;
};
static_assert(std::is_standard_layout_v<gpu_sequence_view_wire>);
static_assert(std::is_standard_layout_v<gpu_query_record_wire>);
static_assert(std::is_standard_layout_v<gpu_target_record_wire>);

} // namespace bioseq::gpu::vulkan
