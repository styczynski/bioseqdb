/*-------------------------------------------------------------------------
 *
 * sequences/rank_pack_core.cppm
 *
 * PostgreSQL-free LSB-first rank packing mechanics shared by the legacy
 * vector encoder, SIMD streaming state, checked fixed-span writer, and
 * admitted materialization adapters.
 *
 *-------------------------------------------------------------------------
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

template <class Counter>
concept unsigned_rank_counter =
    std::integral<Counter> && std::is_unsigned_v<Counter>;

struct lsb_finish_result {
    bool capacity_ok{true};
    std::uint16_t padding_letters{};
};

/// Append one already-validated logical rank. The callback is the only storage
/// policy boundary: vector and fixed-span writers share the exact same shift,
/// full-unit flush, and partial-state transition.
template <class Unit,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit,
          unsigned_rank_counter Counter,
          class Rank,
          class WriteUnit>
requires (std::unsigned_integral<Unit> && std::integral<Rank>)
[[nodiscard]] inline constexpr bool append_valid_rank_lsb(
        Unit& partial_word,
        std::size_t& partial_letters,
        Counter& emitted_symbols,
        Rank rank,
        WriteUnit&& write_unit) {
    static_assert(BitsPerChar > 0u);
    static_assert(LettersPerUnit > 0u);
    static_assert(BitsPerChar * LettersPerUnit
                  <= std::numeric_limits<Unit>::digits);

    if (partial_letters >= LettersPerUnit) {
        return false;
    }
    const std::size_t shift = BitsPerChar * partial_letters;
    partial_word |= static_cast<Unit>(static_cast<Unit>(rank) << shift);
    ++partial_letters;
    ++emitted_symbols;
    if (partial_letters != LettersPerUnit) {
        return true;
    }
    if (!static_cast<bool>(write_unit(partial_word))) {
        return false;
    }
    partial_word = Unit{};
    partial_letters = 0u;
    return true;
}

/// Merge a prepacked LSB-first chunk without decoding it rank-by-rank. SIMD
/// lookup/pack paths use this entry point, keeping their vectorized lookup and
/// bulk packing while sharing the partial-unit and flush rules with scalar and
/// fixed-span encoders.
template <class Unit,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit,
          class Packed,
          unsigned_rank_counter Counter,
          class WriteUnit>
requires (std::unsigned_integral<Unit> && std::unsigned_integral<Packed>)
[[nodiscard]] inline constexpr bool append_prepacked_ranks_lsb(
        Unit& partial_word,
        std::size_t& partial_letters,
        Counter& emitted_symbols,
        Packed packed_ranks,
        std::size_t rank_count,
        WriteUnit&& write_unit) {
    static_assert(BitsPerChar > 0u);
    static_assert(LettersPerUnit > 0u);
    static_assert(BitsPerChar * LettersPerUnit
                  <= std::numeric_limits<Unit>::digits);
    constexpr std::size_t packed_digits = std::numeric_limits<Packed>::digits;
    if (rank_count > packed_digits / BitsPerChar
        || partial_letters >= LettersPerUnit) {
        return false;
    }

    std::size_t remaining = rank_count;
    while (remaining != 0u) {
        const std::size_t available = LettersPerUnit - partial_letters;
        const std::size_t take = remaining < available ? remaining : available;
        const std::size_t packed_bit_count = take * BitsPerChar;
        const Packed low_mask = packed_bit_count == packed_digits
            ? (std::numeric_limits<Packed>::max)()
            : static_cast<Packed>((Packed{1u} << packed_bit_count) - Packed{1u});
        const Unit low_bits = static_cast<Unit>(packed_ranks & low_mask);
        partial_word |= static_cast<Unit>(
            low_bits << (BitsPerChar * partial_letters));
        partial_letters += take;
        emitted_symbols += static_cast<Counter>(take);
        remaining -= take;

        if (packed_bit_count == packed_digits) {
            packed_ranks = Packed{};
        } else {
            packed_ranks = static_cast<Packed>(
                packed_ranks >> packed_bit_count);
        }

        if (partial_letters == LettersPerUnit) {
            if (!static_cast<bool>(write_unit(partial_word))) {
                return false;
            }
            partial_word = Unit{};
            partial_letters = 0u;
        }
    }
    return true;
}

/// Flush the final partial unit and derive its empty-letter count. Exact-unit
/// and empty inputs return zero padding without writing another unit.
template <class Unit,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit,
          class WriteUnit>
requires std::unsigned_integral<Unit>
[[nodiscard]] inline constexpr lsb_finish_result finish_lsb(
        Unit& partial_word,
        std::size_t& partial_letters,
        WriteUnit&& write_unit) {
    static_assert(BitsPerChar > 0u);
    static_assert(LettersPerUnit > 0u);
    static_assert(BitsPerChar * LettersPerUnit
                  <= std::numeric_limits<Unit>::digits);

    if (partial_letters == 0u) {
        return {};
    }
    if (partial_letters >= LettersPerUnit) {
        return {.capacity_ok = false, .padding_letters = 0u};
    }
    const auto padding = static_cast<std::uint16_t>(
        LettersPerUnit - partial_letters);
    if (!static_cast<bool>(write_unit(partial_word))) {
        return {.capacity_ok = false, .padding_letters = padding};
    }
    partial_word = Unit{};
    partial_letters = 0u;
    return {.capacity_ok = true, .padding_letters = padding};
}

}  // namespace bioseq::sequences::rank_pack_detail
