/**
 * @file vulkan/abi/sequence_batch.cppm
 * @brief Batch record layouts for packed sequences and their GPU outputs.
 *
 * These structures describe buffer offsets and counts in a fixed field order
 */
module;

#include <cstddef>

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
 * `seq_word_offset` uses 32-bit words. The unit and  alphabet fields describe
 * how to extract symbol ranks from those words.
 */
struct gpu_sequence_view_wire {
    std::uint64_t seq_word_offset;   ///< Offset in the packed buffer, in 32-bit words.
    std::uint64_t byte_count;        ///< Bytes containing sequence data, before padding.
    std::uint64_t padded_byte_count; ///< Byte count rounded up to a multiple of four.
    std::uint64_t unit_count;        ///< Number of packed sequence units.
    std::uint64_t bp_offset;         ///< Starting base-pair offset of this sequence record.
    std::uint64_t bp_count;          ///< Length of this sequence record in base pairs.
    std::uint32_t letters_per_unit;  ///< Number, of symbols stored in each unit.
    std::uint32_t bits_per_char;     ///< Number of bits used for each symbol.
    std::uint32_t unit_bits;         ///< Number of bits in one packed unit.
    std::uint32_t alphabet_size;     ///< Number of symbols in the alhabet.
    std::uint32_t char_mask;         ///< Mask for one  symbol rank, truncated to 32 bits.
    std::uint32_t padding_letters;   ///< Unused symbol slots in the final packed unit.
    std::uint32_t alphabet_kind;     ///< `alphabet_kernel_kind` from the upload view.
    std::uint32_t leading_symbol_offset; ///< First logical slot within the first unit.
    std::uint32_t alphabet_code;     ///< Stable registered alphabet identity.
    std::uint32_t seed_policy_id;    ///< Native seed policy identity.
};

/**
 * @brief Describes a query and the output ranges reserved for its search.
 *
 * Offsets and counts address minimizer, anchor, and chain-range arrays.
 */
struct gpu_query_record_wire {
    gpu_sequence_view_wire sequence;  ///< Packed query location and encoding.
    std::uint64_t minimizer_offset;   ///< First minimizer record for this query
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
 * The global base-pair offset translates hits within  this slice to positions
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
static_assert(sizeof(gpu_sequence_view_wire) == 88u);
static_assert(offsetof(gpu_sequence_view_wire, alphabet_code) == 80u);

/** Borrowed native packed bytes retained by the caller through synchronous upload. */
struct gpu_packed_sequence_upload {
    gpu_sequence_view_wire sequence{};
    std::span<const std::byte> bytes{};
};
/** Borrows sorted bounded packed windows covering selected regions of one logical sequence. */
struct gpu_native_sequence_upload {
    std::span<const gpu_packed_sequence_upload> windows{};
    std::uint64_t symbol_count{};
};

/** Half-open original sequence coordinates requested by bounded packed uploads. */
struct sequence_upload_interval {
    std::uint64_t begin{};
    std::uint64_t end{};
};
static_assert(sizeof(sequence_upload_interval) == 2u * sizeof(std::uint64_t));

/** Checks packed geometry and byte extent without reading symbol ranks. */
[[nodiscard]] inline bool packed_sequence_upload_geometry_is_valid(const gpu_packed_sequence_upload& upload) noexcept {
    const auto& sequence = upload.sequence;
    // Reject fractional-byte and unsupported widths before any division or rank read.
    if (!std::has_single_bit(sequence.unit_bits) ||
        sequence.unit_bits < std::numeric_limits<unsigned char>::digits ||
        sequence.unit_bits > std::numeric_limits<std::uint64_t>::digits) return false;
    const auto unit_bytes = sequence.unit_bits / std::numeric_limits<unsigned char>::digits;
    if ((unit_bytes != 1u && unit_bytes != 2u && unit_bytes != 4u && unit_bytes != 8u) ||
        sequence.bits_per_char == 0u || sequence.bits_per_char > 8u ||
        sequence.letters_per_unit != sequence.unit_bits / sequence.bits_per_char ||
        sequence.alphabet_size == 0u || sequence.alphabet_size > 256u ||
        sequence.char_mask != (std::uint32_t{1u} << sequence.bits_per_char) - 1u ||
        sequence.alphabet_size > sequence.char_mask + 1u ||
        sequence.leading_symbol_offset >= sequence.letters_per_unit ||
        sequence.padding_letters >= sequence.letters_per_unit ||
        sequence.unit_count > std::numeric_limits<std::uint64_t>::max() / sequence.letters_per_unit ||
        sequence.unit_count > std::numeric_limits<std::uint64_t>::max() / unit_bytes ||
        sequence.byte_count != sequence.unit_count * unit_bytes ||
        sequence.byte_count != upload.bytes.size() ||
        sequence.byte_count > std::numeric_limits<std::uint64_t>::max() - 3u ||
        sequence.padded_byte_count != (sequence.byte_count + 3u) / 4u * 4u) return false;
    const auto slots = sequence.unit_count * sequence.letters_per_unit;
    if (sequence.padding_letters > slots || sequence.leading_symbol_offset > slots - sequence.padding_letters ||
        sequence.bp_count > slots - sequence.padding_letters - sequence.leading_symbol_offset) return false;
    return true;
}

/** Checks packed geometry and every logical rank without expanding input storage. */
[[nodiscard]] inline bool packed_sequence_upload_is_valid(const gpu_packed_sequence_upload& upload) noexcept {
    if (!packed_sequence_upload_geometry_is_valid(upload)) return false;
    const auto& sequence = upload.sequence;
    const auto unit_bytes = sequence.unit_bits / std::numeric_limits<unsigned char>::digits;
    // Every encoding is valid for a full-cardinality alphabet, including DNA4.
    if (sequence.alphabet_size == sequence.char_mask + 1u) return true;
    for (std::uint64_t position = 0u; position < sequence.bp_count; ++position) {
        const auto slot = sequence.leading_symbol_offset + position;
        const auto bit = slot % sequence.letters_per_unit * sequence.bits_per_char;
        const auto byte = slot / sequence.letters_per_unit * unit_bytes + bit / 8u;
        auto rank = std::to_integer<std::uint32_t>(upload.bytes[byte]) >> (bit % 8u);
        if (bit % 8u + sequence.bits_per_char > 8u)
            rank |= std::to_integer<std::uint32_t>(upload.bytes[byte + 1u]) << (8u - bit % 8u);
        if ((rank & sequence.char_mask) >= sequence.alphabet_size) return false;
    }
    return true;
}

/** A packed upload can fail before touching destination storage. */
enum class gpu_packed_sequence_error : std::uint8_t {
    invalid_geometry, invalid_rank, arena_bounds,
};

/** Copies one validated native packed window directly into an aligned mapped arena.
 * The returned descriptor addresses the arena in u32 words; only transport tail
 * padding is zeroed. Source bytes and storage-unit packing are preserved.
 * The explicit validator performs full validation unless the caller has already
 * validated the same immutable upload; such callers supply the geometry check.
 */
template<class Validator>
requires requires(Validator validate, const gpu_packed_sequence_upload& upload) {
    { validate(upload) } noexcept -> std::same_as<bool>;
}
[[nodiscard]] inline std::expected<gpu_sequence_view_wire, gpu_packed_sequence_error>
place_packed_sequence_upload(const gpu_packed_sequence_upload& upload,
                              std::size_t byte_offset,
                              std::span<std::byte> destination,
                              Validator validate) noexcept {
    if (!validate(upload))
        return std::unexpected{gpu_packed_sequence_error::invalid_geometry};
    if (byte_offset % sizeof(std::uint32_t) != 0u || byte_offset > destination.size() ||
        upload.sequence.padded_byte_count > destination.size() - byte_offset)
        return std::unexpected{gpu_packed_sequence_error::arena_bounds};
    if (!upload.bytes.empty())
        std::memcpy(destination.data() + byte_offset, upload.bytes.data(), upload.bytes.size());
    const auto padding_bytes = upload.sequence.padded_byte_count - upload.bytes.size();
    if (padding_bytes != 0u)
        std::memset(destination.data() + byte_offset + upload.bytes.size(), 0,
                    static_cast<std::size_t>(padding_bytes));
    auto descriptor = upload.sequence;
    descriptor.seq_word_offset = byte_offset / sizeof(std::uint32_t);
    return descriptor;
}

static_assert(std::is_standard_layout_v<gpu_query_record_wire>);
static_assert(std::is_standard_layout_v<gpu_target_record_wire>);

} // namespace bioseq::gpu::vulkan
