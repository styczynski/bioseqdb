/**
 * @file vulkan/abi/sequence_batch.cppm
 * @brief Defines packed-sequence batch records shared with Vulkan shaders.
 *
 * Wire structs are standard layout so host uploads and shader loads share one
 * field order without reinterpretation.
 */
module;

#include <cstdint>
#include <type_traits>

export module bioseq.gpu.vulkan.abi:sequence_batch;

export namespace bioseq::gpu::vulkan {

/** @brief Tags the role and strand routing of one packed sequence record. */
enum class gpu_sequence_record_flags : std::uint32_t {
    none = 0u,///< No routing flags set.
    reverse_complement = 1u << 0u,///< Packed bytes are reverse-complemented on read.
    target_record = 1u << 1u,///< Record describes a target sequence slice.
    query_record = 1u << 2u,///< Record describes a query sequence slice.
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
    std::uint32_t reserved;///< Reserved for ABI growth, must be zero on upload.
};

/**
 * @brief Binds a query sequence to its minimizer, anchor, and chain slices.
 *
 * Output offsets address device arrays sized by the count submit. Count fields
 * are written by the count pass and consumed by later seed and chain dispatches.
 */
struct gpu_query_record_wire {
    gpu_sequence_view_wire sequence;///< Packed query bytes and alphabet traits.
    std::uint64_t minimizer_offset;  ///< Output minimizer-word offset in records.
    std::uint64_t minimizer_count;   ///< Minimizer count filled by count submit.
    std::uint64_t anchor_offset;     ///< Output anchor array offset in records.
    std::uint64_t anchor_count;      ///< Anchor count filled by count submit.
    std::uint64_t chain_range_offset;///< Output chain-range array offset in records.
    std::uint64_t chain_range_count;///< Chain-range count filled by count submit.
    std::uint32_t record_id;///< Stable query record identifier for the batch.
    std::uint32_t flags;///< `gpu_sequence_record_flags` bit mask.
};

/**
 * @brief Binds a target sequence to its global coordinates and minimizer pairs.
 *
 * Global base-pair offsets let shaders map segment-local hits back to the full
 * target coordinate space.
 */
struct gpu_target_record_wire {
    gpu_sequence_view_wire sequence;///< Packed target bytes and alphabet traits.
    std::uint64_t target_global_bp_offset;///< Base-pair origin of this slice in the full target.
    std::uint64_t target_id;///< Stable target identifier within the batch.
    std::uint64_t minimizer_pair_offset;///< Minimizer-pair array offset in records.
    std::uint64_t minimizer_pair_count;///< Number of minimizer pairs for this target slice.
    std::uint32_t record_id;///< Stable target record identifier for the batch.
    std::uint32_t flags;///< `gpu_sequence_record_flags` bit mask.
};
static_assert(std::is_standard_layout_v<gpu_sequence_view_wire>);
static_assert(std::is_standard_layout_v<gpu_query_record_wire>);
static_assert(std::is_standard_layout_v<gpu_target_record_wire>);

} // namespace bioseq::gpu::vulkan
