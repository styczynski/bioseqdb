/**
 * @file rank_pack_core.cppm
 * @brief Storage-neutral LSB-first rank-packing mechanics.
 *
 * Scalar encoders, SIMD streams, fixed-span writers, and sequence
 * materializers share these transitions so packing order and final padding
 * remain identical across storage policies.
 */
module;

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#if defined(POSTGRES_H) || defined(FMGR_H) || defined(PG_VERSION_NUM)
#error "rank_pack_core must compile without PostgreSQL headers or APIs"
#endif

export module bioseq.sequences.rank_pack_core;

export namespace bioseq::sequences::rank_pack_detail {

/** @brief Unsigned integral counter accepted by the packing primitives. */
template <class p_counter_type>
concept unsigned_rank_counter =
    std::integral<p_counter_type> && std::is_unsigned_v<p_counter_type>;

/** @brief Result of flushing a final partial packed unit. */
struct lsb_finish_result {
    /** @brief Whether the output policy accepted the final unit. */
    bool capacity_ok{true};
    /** @brief Empty logical-symbol slots in the final packed unit. */
    std::uint16_t padding_letters{};
};

/**
 * @brief Append one validated logical rank to an LSB-first packing state.
 * @tparam p_storage_unit Unsigned destination word type.
 * @tparam p_bits_per_character Logical rank width.
 * @tparam p_letters_per_unit Logical ranks stored in one destination word.
 * @tparam p_counter_type Unsigned emitted-symbol counter type.
 * @tparam p_rank_type Integral input-rank type.
 * @tparam p_write_unit_type Callable accepting one completed destination word.
 * @param partial_word In-progress destination word.
 * @param partial_letters Occupied rank slots in `partial_word`.
 * @param emitted_symbols Total accepted ranks, incremented on success.
 * @param rank Already-validated logical rank.
 * @param write_unit Storage-policy callback for completed words.
 * @return `true` when the rank was admitted and any required flush succeeded.
 */
template <class p_storage_unit,
          std::size_t p_bits_per_character,
          std::size_t p_letters_per_unit,
          unsigned_rank_counter p_counter_type,
          class p_rank_type,
          class p_write_unit_type>
requires (std::unsigned_integral<p_storage_unit> && std::integral<p_rank_type>)
[[nodiscard]] inline constexpr bool append_valid_rank_lsb(
        p_storage_unit& partial_word,
        std::size_t& partial_letters,
        p_counter_type& emitted_symbols,
        p_rank_type rank,
        p_write_unit_type&& write_unit) {
    static_assert(p_bits_per_character > 0u);
    static_assert(p_letters_per_unit > 0u);
    static_assert(p_bits_per_character * p_letters_per_unit
                  <= std::numeric_limits<p_storage_unit>::digits);

    if (partial_letters >= p_letters_per_unit) {
        return false;
    }
    const std::size_t shift = p_bits_per_character * partial_letters;
    partial_word |= static_cast<p_storage_unit>(
        static_cast<p_storage_unit>(rank) << shift);
    ++partial_letters;
    ++emitted_symbols;
    if (partial_letters != p_letters_per_unit) {
        return true;
    }
    if (!static_cast<bool>(write_unit(partial_word))) {
        return false;
    }
    partial_word = p_storage_unit{};
    partial_letters = 0u;
    return true;
}

/**
 * @brief Merge prepacked ranks without scalar rank-by-rank decoding.
 * @tparam p_storage_unit Unsigned destination word type.
 * @tparam p_bits_per_character Logical rank width.
 * @tparam p_letters_per_unit Logical ranks stored in one destination word.
 * @tparam p_packed_type Unsigned type holding the incoming packed chunk.
 * @tparam p_counter_type Unsigned emitted-symbol counter type.
 * @tparam p_write_unit_type Callable accepting one completed destination word.
 * @param partial_word In-progress destination word.
 * @param partial_letters Occupied rank slots in `partial_word`.
 * @param emitted_symbols Total merged ranks, incremented on success.
 * @param packed_ranks LSB-first source ranks.
 * @param rank_count Valid ranks contained in `packed_ranks`.
 * @param write_unit Storage-policy callback for completed words.
 * @return `true` when all ranks were merged and every flush succeeded.
 */
template <class p_storage_unit,
          std::size_t p_bits_per_character,
          std::size_t p_letters_per_unit,
          class p_packed_type,
          unsigned_rank_counter p_counter_type,
          class p_write_unit_type>
requires (std::unsigned_integral<p_storage_unit>
          && std::unsigned_integral<p_packed_type>)
[[nodiscard]] inline constexpr bool append_prepacked_ranks_lsb(
        p_storage_unit& partial_word,
        std::size_t& partial_letters,
        p_counter_type& emitted_symbols,
        p_packed_type packed_ranks,
        std::size_t rank_count,
        p_write_unit_type&& write_unit) {
    static_assert(p_bits_per_character > 0u);
    static_assert(p_letters_per_unit > 0u);
    static_assert(p_bits_per_character * p_letters_per_unit
                  <= std::numeric_limits<p_storage_unit>::digits);
    constexpr std::size_t packed_digits =
        std::numeric_limits<p_packed_type>::digits;
    if (rank_count > packed_digits / p_bits_per_character
        || partial_letters >= p_letters_per_unit) {
        return false;
    }

    std::size_t remaining = rank_count;
    while (remaining != 0u) {
        const std::size_t available = p_letters_per_unit - partial_letters;
        const std::size_t take = remaining < available ? remaining : available;
        const std::size_t packed_bit_count = take * p_bits_per_character;
        const p_packed_type low_mask = packed_bit_count == packed_digits
            ? (std::numeric_limits<p_packed_type>::max)()
            : static_cast<p_packed_type>(
                (p_packed_type{1u} << packed_bit_count) - p_packed_type{1u});
        const p_storage_unit low_bits =
            static_cast<p_storage_unit>(packed_ranks & low_mask);
        partial_word |= static_cast<p_storage_unit>(
            low_bits << (p_bits_per_character * partial_letters));
        partial_letters += take;
        emitted_symbols += static_cast<p_counter_type>(take);
        remaining -= take;

        if (packed_bit_count == packed_digits) {
            packed_ranks = p_packed_type{};
        } else {
            packed_ranks = static_cast<p_packed_type>(
                packed_ranks >> packed_bit_count);
        }

        if (partial_letters == p_letters_per_unit) {
            if (!static_cast<bool>(write_unit(partial_word))) {
                return false;
            }
            partial_word = p_storage_unit{};
            partial_letters = 0u;
        }
    }
    return true;
}

/**
 * @brief Flush the final partial word and derive its empty-rank count.
 * @tparam p_storage_unit Unsigned destination word type.
 * @tparam p_bits_per_character Logical rank width.
 * @tparam p_letters_per_unit Logical ranks stored in one destination word.
 * @tparam p_write_unit_type Callable accepting the final destination word.
 * @param partial_word In-progress destination word; cleared after a flush.
 * @param partial_letters Occupied rank slots; cleared after a flush.
 * @param write_unit Storage-policy callback for the final word.
 * @return Capacity status and the number of padding rank slots.
 */
template <class p_storage_unit,
          std::size_t p_bits_per_character,
          std::size_t p_letters_per_unit,
          class p_write_unit_type>
requires std::unsigned_integral<p_storage_unit>
[[nodiscard]] inline constexpr lsb_finish_result finish_lsb(
        p_storage_unit& partial_word,
        std::size_t& partial_letters,
        p_write_unit_type&& write_unit) {
    static_assert(p_bits_per_character > 0u);
    static_assert(p_letters_per_unit > 0u);
    static_assert(p_bits_per_character * p_letters_per_unit
                  <= std::numeric_limits<p_storage_unit>::digits);

    if (partial_letters == 0u) {
        return {};
    }
    if (partial_letters >= p_letters_per_unit) {
        return {.capacity_ok = false, .padding_letters = 0u};
    }
    const auto padding = static_cast<std::uint16_t>(
        p_letters_per_unit - partial_letters);
    if (!static_cast<bool>(write_unit(partial_word))) {
        return {.capacity_ok = false, .padding_letters = padding};
    }
    partial_word = p_storage_unit{};
    partial_letters = 0u;
    return {.capacity_ok = true, .padding_letters = padding};
}

}  // namespace bioseq::sequences::rank_pack_detail
