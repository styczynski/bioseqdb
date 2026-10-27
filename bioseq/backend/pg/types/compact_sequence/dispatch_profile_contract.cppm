/**
 * @file dispatch_profile_contract.cppm
 * Report which alphabets and packing widths this build supports.
 */
export module bioseq.postgres.types.compact_sequence.dispatch_profile_contract;

import std;

/** @brief Number of rank-packed biological alphabets enabled in this build. */
export inline constexpr std::size_t pgseq_dispatch_profile_biological_alphabet_count =
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    1u;
#else
    2u;
#endif

/** @brief Storage alphabets include one counted CIGAR family after the biological entries. */
export inline constexpr std::size_t pgseq_dispatch_profile_alphabet_count =
    pgseq_dispatch_profile_biological_alphabet_count + 1u;

/** @brief Check whether this build supports a storage-unit width in bytes. */
export [[nodiscard]] constexpr bool
pgseq_dispatch_profile_contains_storage_unit_byte_width(
    const std::uint8_t byte_width) noexcept
{
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES)
    return byte_width == 8u;
#else
    return byte_width == 1u || byte_width == 2u
        || byte_width == 4u || byte_width == 8u;
#endif
}
