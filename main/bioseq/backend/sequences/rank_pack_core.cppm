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
template <class counter_type>
concept unsigned_rank_counter =
    std::integral<counter_type> && std::is_unsigned_v<counter_type>;

/** @brief Result of flushing a final partial packed unit. */
struct lsb_finish_result {
    /** @brief Whether the output policy accepted the final unit. */
    bool capacity_ok{true};
    /** @brief Empty logical-symbol slots in the final packed unit. */
    std::uint16_t padding_letters{};
};

/**
 * @brief Append one validated logical rank to an LSB-first packing state.
 * @tparam storage_unit Unsigned destination word type.
 * @tparam bits_per_character Logical rank width.
 * @tparam letters_per_unit Logical ranks stored in one destination word.
 * @tparam counter_type Unsigned emitted-symbol counter type.
 * @tparam rank_type Integral input-rank type.
 * @tparam write_unit_type Callable accepting one completed destination word.
 * @param partial_word In-progress destination word.
 * @param partial_letters Occupied rank slots in `partial_word`.
 * @param emitted_symbols Total accepted ranks, incremented on success.
 * @param rank Already-validated logical rank.
 * @param write_unit Storage-policy callback for completed words.
 * @return `true` when the rank was admitted and any required flush succeeded.
 */
template <class storage_unit,
          std::size_t bits_per_character,
          std::size_t letters_per_unit,
          unsigned_rank_counter counter_type,
          class rank_type,
          class write_unit_type>
requires (std::unsigned_integral<storage_unit> && std::integral<rank_type>)
[[nodiscard]] inline constexpr bool append_valid_rank_lsb(
        storage_unit& partial_word,
        std::size_t& partial_letters,
        counter_type& emitted_symbols,
        rank_type rank,
        write_unit_type&& write_unit) {
    static_assert(bits_per_character > 0u);
    static_assert(letters_per_unit > 0u);
    static_assert(bits_per_character * letters_per_unit
                  <= std::numeric_limits<storage_unit>::digits);

    if (partial_letters >= letters_per_unit) {
        return false;
    }
    const std::size_t shift = bits_per_character * partial_letters;
    partial_word |= static_cast<storage_unit>(
        static_cast<storage_unit>(rank) << shift);
    ++partial_letters;
    ++emitted_symbols;
    if (partial_letters != letters_per_unit) {
        return true;
    }
    if (!static_cast<bool>(write_unit(partial_word))) {
        return false;
    }
    partial_word = storage_unit{};
    partial_letters = 0u;
    return true;
}

/**
 * @brief Merge prepacked ranks without scalar rank-by-rank decoding.
 * @tparam storage_unit Unsigned destination word type.
 * @tparam bits_per_character Logical rank width.
 * @tparam letters_per_unit Logical ranks stored in one destination word.
 * @tparam packed_type Unsigned type holding the incoming packed chunk.
 * @tparam counter_type Unsigned emitted-symbol counter type.
 * @tparam write_unit_type Callable accepting one completed destination word.
 * @param partial_word In-progress destination word.
 * @param partial_letters Occupied rank slots in `partial_word`.
 * @param emitted_symbols Total merged ranks, incremented on success.
 * @param packed_ranks LSB-first source ranks.
 * @param rank_count Valid ranks contained in `packed_ranks`.
 * @param write_unit Storage-policy callback for completed words.
 * @return `true` when all ranks were merged and every flush succeeded.
 */
template <class storage_unit,
          std::size_t bits_per_character,
          std::size_t letters_per_unit,
          class packed_type,
          unsigned_rank_counter counter_type,
          class write_unit_type>
requires (std::unsigned_integral<storage_unit>
          && std::unsigned_integral<packed_type>)
[[nodiscard]] inline constexpr bool append_prepacked_ranks_lsb(
        storage_unit& partial_word,
        std::size_t& partial_letters,
        counter_type& emitted_symbols,
        packed_type packed_ranks,
        std::size_t rank_count,
        write_unit_type&& write_unit) {
    static_assert(bits_per_character > 0u);
    static_assert(letters_per_unit > 0u);
    static_assert(bits_per_character * letters_per_unit
                  <= std::numeric_limits<storage_unit>::digits);
    constexpr std::size_t packed_digits =
        std::numeric_limits<packed_type>::digits;
    if (rank_count > packed_digits / bits_per_character
        || partial_letters >= letters_per_unit) {
        return false;
    }

    std::size_t remaining = rank_count;
    while (remaining != 0u) {
        const std::size_t available = letters_per_unit - partial_letters;
        const std::size_t take = remaining < available ? remaining : available;
        const std::size_t packed_bit_count = take * bits_per_character;
        const packed_type low_mask = packed_bit_count == packed_digits
            ? (std::numeric_limits<packed_type>::max)()
            : static_cast<packed_type>(
                (packed_type{1u} << packed_bit_count) - packed_type{1u});
        const storage_unit low_bits =
            static_cast<storage_unit>(packed_ranks & low_mask);
        partial_word |= static_cast<storage_unit>(
            low_bits << (bits_per_character * partial_letters));
        partial_letters += take;
        emitted_symbols += static_cast<counter_type>(take);
        remaining -= take;

        if (packed_bit_count == packed_digits) {
            packed_ranks = packed_type{};
        } else {
            packed_ranks = static_cast<packed_type>(
                packed_ranks >> packed_bit_count);
        }

        if (partial_letters == letters_per_unit) {
            if (!static_cast<bool>(write_unit(partial_word))) {
                return false;
            }
            partial_word = storage_unit{};
            partial_letters = 0u;
        }
    }
    return true;
}

/**
 * @brief Flush the final partial word and derive its empty-rank count.
 * @tparam storage_unit Unsigned destination word type.
 * @tparam bits_per_character Logical rank width.
 * @tparam letters_per_unit Logical ranks stored in one destination word.
 * @tparam write_unit_type Callable accepting the final destination word.
 * @param partial_word In-progress destination word; cleared after a flush.
 * @param partial_letters Occupied rank slots; cleared after a flush.
 * @param write_unit Storage-policy callback for the final word.
 * @return Capacity status and the number of padding rank slots.
 */
template <class storage_unit,
          std::size_t bits_per_character,
          std::size_t letters_per_unit,
          class write_unit_type>
requires std::unsigned_integral<storage_unit>
[[nodiscard]] inline constexpr lsb_finish_result finish_lsb(
        storage_unit& partial_word,
        std::size_t& partial_letters,
        write_unit_type&& write_unit) {
    static_assert(bits_per_character > 0u);
    static_assert(letters_per_unit > 0u);
    static_assert(bits_per_character * letters_per_unit
                  <= std::numeric_limits<storage_unit>::digits);

    if (partial_letters == 0u) {
        return {};
    }
    if (partial_letters >= letters_per_unit) {
        return {.capacity_ok = false, .padding_letters = 0u};
    }
    const auto padding = static_cast<std::uint16_t>(
        letters_per_unit - partial_letters);
    if (!static_cast<bool>(write_unit(partial_word))) {
        return {.capacity_ok = false, .padding_letters = padding};
    }
    partial_word = storage_unit{};
    partial_letters = 0u;
    return {.capacity_ok = true, .padding_letters = padding};
}

}  // namespace bioseq::sequences::rank_pack_detail
