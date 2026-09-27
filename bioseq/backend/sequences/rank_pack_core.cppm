/**
 * @file rank_pack_core.cppm
 * @brief Shared packing steps that place the first rank in each word's low bits.
 *
 * Encoders and sequence-copying paths supply their own word writer. These helpers
 * keep rank order and final padding consistent across those writers.
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

/** Packs ranks in sequence order and passes completed words to a caller-supplied writer. */
export namespace bioseq::sequences::rank_pack_detail {

/** @brief Unsigned integral counter accepted by the packing primitives. */
template <class p_counter_type>
concept unsigned_rank_counter =
    std::integral<p_counter_type> && std::is_unsigned_v<p_counter_type>;

/** @brief Result of flushing a final partial packed unit. */
struct lsb_finish_result {
    /** True when no final write was needed or the writer accepted it. */
    bool capacity_ok{true};
    /** @brief Empty logical-symbol slots in the final packed unit. */
    std::uint16_t padding_letters{};
};

/**
 * @brief Adds one valid rank to the partial word and writes the word when full.
 *
 * The caller checks rank validity and counter capacity. A failed write leaves the
 * full word and updated counters in place.
 * @param partial_word Word being assembled.
 * @param partial_letters Used symbol slots in that word.
 * @param emitted_symbols Number of ranks added to the packing state.
 * @param rank Valid alphabet rank.
 * @param write_unit Accepts completed words and reports success.
 * @return False if the state was already full or the writer rejected a word.
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
 * @brief Adds several already-packed ranks to the current output word.
 *
 * Copies groups of low bits and writes each completed word. On writer failure,
 * previous writes and the updated packing state remain. The caller ensures the
 * symbol counter has room.
 * @param partial_word Word being assembled.
 * @param partial_letters Used symbol slots.
 * @param emitted_symbols Number of ranks added to the packing state.
 * @param packed_ranks Source ranks, first symbol in the low bits.
 * @param rank_count Number of source ranks.
 * @param write_unit Accepts completed words.
 * @return False for invalid counts, an already-full state, or a rejected write.
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
 * @brief Writes the final partial word and reports its unused symbol slots.
 *
 * An empty state needs no write. A successful write clears the state, while a
 * rejected write leaves it unchanged.
 * @param partial_word Word being assembled.
 * @param partial_letters Used symbol slots.
 * @param write_unit Accepts the final word.
 * @return Write status and final padding. An already-full state is an error.
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
