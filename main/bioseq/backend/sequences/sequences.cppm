module;

#include "common.h"

//include "fmt/ranges.h"

#include <array>
#include <deque>
#include <iterator>
#include <list>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <valarray>
#include <vector>
#include <limits>

#include <boost/integer.hpp>
#include <boost/iterator/iterator_adaptor.hpp>


export module bioseq_sequences:sequences;

import mlogging;
import sequan_compat;

using namespace seq::literals; // todo: this must be remvoed

export {

template<typename T, template<typename...> typename U>
inline constexpr bool is_instance_of_v = std::false_type{};

template<template<typename...> typename U, typename... Vs>
inline constexpr bool is_instance_of_v<U<Vs...>,U> = std::true_type{};

template<typename A>
concept sequence_alphabet = \
    requires (A const a) {
        { seqan3::to_rank(a) };
        { seqan3::alphabet_size<A> };
    };

template<
    sequence_alphabet alphabet_t,
    typename storage_unit_t = typename boost::uint_t<seqan3::detail::ceil_log2(seqan3::alphabet_size<alphabet_t>)>::least,
    size_t p_letter_bit_width = seqan3::detail::ceil_log2(seqan3::alphabet_size<alphabet_t>)
>
struct sequence_custom_alphabet {
static_assert(p_letter_bit_width <= sizeof(storage_unit_t) * 8);
static_assert(p_letter_bit_width > 0);
static_assert(sizeof(storage_unit_t) > 0);

public:
    using alphabet_type = alphabet_t;
    using storage_unit = storage_unit_t;
    static constexpr size_t letter_bit_width = p_letter_bit_width;
    static constexpr bool is_valid_sequence_custom_alphabet = true;

    static constexpr size_t leftover_bits = (sizeof(alphabet_type) * 8) % letter_bit_width;
};

template <typename T>
concept is_sequence_custom_alphabet = T::is_valid_sequence_custom_alphabet == true;

// This concept checks if we can do clever trick
// Can we interpret sequence over A as sequence over B?
// This requires:
//   - alphabet to be the same
//   - letter width to be the same
//   - storage unit of A needs to be of size B or larger
//   - there cannot be any leftover bits in either A or B
//   - size of A in bits must be divisible by size of B in bits
// Last two conditions means that each storage unit in A can be split into N units in B
// where N is integer, so this means we can interpret sequence A as sequence over B
// (this needs some tweaking as size and padding adjustment, but this concept just check if we can do it)
template <typename A, typename B>
concept is_sequence_custom_alphabet_bit_castable_to = (
    is_sequence_custom_alphabet<A> && \
    is_sequence_custom_alphabet<B> && \
    std::same_as<typename A::alphabet_type, typename B::alphabet_type> && \
    A::letter_bit_width == B::letter_bit_width && \
    sizeof(typename A::storage_unit) >= sizeof(typename B::storage_unit) && \
    A::leftover_bits == 0 && B::leftover_bits == 0
);

template<typename R>
concept sequence_proper_container = \
    sequence_alphabet<std::remove_reference_t<std::ranges::range_reference_t<R>>> && \
    std::ranges::sized_range<R> && \
    std::ranges::random_access_range<R>;

template<typename R>
concept supports_compact_sequence_range = \
    is_sequence_custom_alphabet<R> || \
    sequence_proper_container<R> || \
    sequence_alphabet<R>;

template <typename T, typename A>
std::string decode_compressed_seq_el(const T& val, bool complement = false, bool reverse = false)  {
    // FIXME: This is very wrong
    constexpr size_t bps_count = sizeof(T) * 8 / seqan3::detail::ceil_log2(A::alphabet_size);
    std::string ret(bps_count, '*');
    T buf = val;
    for(auto i = 0; i < bps_count; ++i) {
        // reverse with: bps_count - i - 1
        // FIXME: 3 here cannot be hardcoded
        if (complement) {
            ret[i] = A().assign_rank((3 ^ buf) & 3).to_char();
        } else {
            ret[i] = A().assign_rank(buf & 3).to_char();
        }
        buf >>= 2;
    }
    if (reverse) {
        std::reverse(ret.begin(), ret.end());
    }
    return ret;
}

template<sequence_alphabet alphabet_t>
constexpr std::string symbols_vector_to_string(const std::vector<alphabet_t>& substr) {
    std::string result;
    result.reserve(substr.size());
    for(const auto& letter : substr) {
        result.push_back(letter.to_char());
    }
    return result;
}




// New impl

template<
    typename R
>
struct compact_sequence_range {};

enum IteratorMode {
    BP,
    SYMBOL
};

// =====================================================================
// Bit-packed unit decomposition helpers
//
// For an alphabet whose bits_per_char divides storage_unit cleanly
// (`unit_leftover_bits == 0`), every bit of a storage_unit holds a
// letter. This means we can extract all letters_per_unit symbols out of
// one storage word with constexpr-templated mask + shift, no iteration
// over individual bits, and let the compiler unroll fully.
//
// IMPORTANT — bioseq's encoding (CODE_GUIDELINES.md §10) places the
// FIRST input letter (sequence position 0) at the *low* bits of a fully
// loaded unit. The legacy `bp_scalar_iter::increment` (sequences.cppm)
// walks `unit_val & char_mask` then `unit_val >>= bits_per_char` —
// LSB-first. `_convert` produces this layout: each new input letter is
// shifted in at the top, then the unit is right-shifted for the next
// one, so by the time the unit is fully populated the FIRST letter
// sits at bit 0..bits_per_char-1 (LSB end) and the LAST letter sits at
// the MSB end. Hand-traced for `dna4 / uint64_t`, sequence "ACGT…32":
// A (rank 0) → bits 0-1, C (rank 1) → bits 2-3, …, last letter (the
// 32nd) → bits 62-63.
//
// `extract_letters_from_unit<Value, bits_per_letter>(val)` therefore
// returns a `std::array<Value, letters_per_unit>` where index 0 holds
// the LSB-slot letter (= first input letter when the unit is fully
// loaded) and index `letters_per_unit - 1` holds the MSB-slot letter
// (= last input letter). Walking the array in increasing index order
// reproduces the same letter sequence as the scalar `bp_scalar_iter`.
//
// The intent is to pre-compute these arrays for the unit-aligned bulk
// of a region and then stream the letters into a destination buffer
// (KSW2 state.target_bytes / state.query_reversed) — avoiding the
// per-letter scalar shift loop that the previous `to_encoded_into`
// walked.
// =====================================================================

namespace bit_pack_detail {

template<typename Value, std::size_t... Is>
[[nodiscard]] constexpr Value get_bitmask_seq(std::index_sequence<Is...>) noexcept {
    return ((static_cast<Value>(1) << static_cast<Value>(Is)) | ...);
}

}  // namespace bit_pack_detail

template<typename Value, std::size_t bits_per_letter>
[[nodiscard]] consteval Value get_unit_bitmask() noexcept {
    return bit_pack_detail::get_bitmask_seq<Value>(
        std::make_index_sequence<bits_per_letter>{});
}

/**
 * @brief Extract a single letter from a storage word.
 *
 * `letter_no` indexes from the *low* end of the unit (index 0 = bottom
 * `bits_per_letter` bits). bioseq's encoding (LSB-first; see header
 * comment above the helpers) places the FIRST input letter at slot
 * 0, so `extract_unit_letter<…, 0>(unit)` returns the first letter
 * directly.
 */
template<typename Value, std::size_t bits_per_letter, std::size_t letter_no>
[[always_inline]] [[nodiscard]] constexpr Value extract_unit_letter(const Value& val) noexcept {
    static_assert(letter_no * bits_per_letter < sizeof(Value) * 8,
        "letter_no out of range for the storage_unit width "
        "(would shift past the high bit of Value)");
    constexpr Value letter_bit_shift =
        static_cast<Value>(bits_per_letter * letter_no);
    constexpr Value mask = get_unit_bitmask<Value, bits_per_letter>() << letter_bit_shift;
    return (val & mask) >> letter_bit_shift;
}

namespace bit_pack_detail {

template<typename Value, std::size_t bits_per_letter, std::size_t letter_count, std::size_t... Is>
[[always_inline]] [[nodiscard]] constexpr auto extract_letters_from_unit_impl(
    std::index_sequence<Is...>, const Value& val) noexcept
    -> std::array<Value, sizeof...(Is)>
{
    // Index 0 reads from bit slot 0 (the LSB end). bioseq's encoding
    // puts the FIRST input letter at bit slot 0, the SECOND at slot 1,
    // etc., so walking the returned array in order matches the
    // input-sequence order — same contract as `bp_scalar_iter`.
    return std::array<Value, sizeof...(Is)>{
        extract_unit_letter<Value, bits_per_letter, Is>(val)...
    };
}

}  // namespace bit_pack_detail

/**
 * @brief Decompose one storage_unit into its constituent letters.
 *
 * Returns a fixed-size array of letter values, ordered to match
 * bioseq's canonical encoding: index 0 is the symbol stored at the
 * unit's low bits, which (for a fully populated unit) is the
 * symbol that comes FIRST in the original input sequence.
 *
 * The whole expansion is constexpr — at runtime this compiles down to
 * a small fixed sequence of mask+shift instructions that the compiler
 * can fully unroll.
 *
 * Use this in performance-critical paths where you want to avoid the
 * scalar `for (bit_no = 0..letters_per_unit) { mask; shift }` loop;
 * the compiler can vectorize the fixed-size array fill into SIMD
 * stores when the destination is contiguous. See
 * `to_encoded_into` (the unit-aligned fast path) in this file.
 *
 * Only valid for alphabets with `unit_leftover_bits == 0`. The default
 * `letter_count` template argument matches `letters_per_unit` for the
 * given (Value, bits_per_letter) pair.
 */
template<
    typename Value,
    std::size_t bits_per_letter,
    std::size_t letter_count = (sizeof(Value) * 8) / bits_per_letter
>
[[always_inline]] [[nodiscard]] constexpr auto extract_letters_from_unit(const Value& val) noexcept
    -> std::array<Value, letter_count>
{
    return bit_pack_detail::extract_letters_from_unit_impl<Value, bits_per_letter, letter_count>(
        std::make_index_sequence<letter_count>{}, val);
}

/*
This struct represents different view of the sequence.
*/
template <
    typename base_t,
    typename output_t
>
struct bit_casted_view {
private:
    const base_t& base;
public:

    bit_casted_view(const base_t& base_container): base(base_container) {}

    using base_traits = base_t::traits;
    using traits = output_t::traits;

    // If this is downcast or upcast. Downcast is when we translate for example 32 uint into 8 uint. Upcast viceversa
    static constexpr bool is_down_cast = sizeof(output_t) <= sizeof(base_t);
    // What is the ratio between sizes in bits between input and output type
    static constexpr size_t cast_ratio = is_down_cast ? (base_traits::unit_width / traits::unit_width) : (traits::unit_width / base_traits::unit_width);
    // Log base 2 for cast_ratio (useful for shifts)
    static constexpr size_t cast_ratio_log = seqan3::detail::ceil_log2(cast_ratio);

    // Assert verifies that cast_ratio is power of two, otherwise we cannot do anything
    static_assert(1 << cast_ratio_log == cast_ratio);

    [[always_inline]] constexpr size_t bp_size() const {
        // Number of letters stays the same, we only change interpretation not the value itself
        return base.bp_size();
    };

    [[always_inline]] constexpr size_t size() const {
        // This calculates number of new units of new type from the number of letters
        return int_div_ceil(base.bp_size(), traits::letters_per_unit).first;
    };

    [[always_inline]] constexpr size_t byte_size() const {
        // We don't change underlying values so it stays the same
        return base.byte_size();
    };

    [[always_inline]] constexpr size_t padding() {
        return int_div_ceil(base.bp_size(), traits::letters_per_unit).second;
    };

    template<typename I>
    requires(base_t::template is_valid_derived_iter<I>::value)
    [[always_inline]] constexpr const typename traits::storage_unit* read_cptr(const I& it) {
        return reinterpret_cast<const typename traits::storage_unit*>(base.read_cptr(it));
    };

    template<typename I>
    requires(base_t::template is_valid_derived_iter<I>::value)
    [[always_inline]] constexpr static void advance_iter(I& it, const size_t value) {
        // This is interesting part:
        // - if we downcast then for one real unit we have view 'cast_ratio' units, so each cast_ratio increments of iterator means 1 real increment
        // - if we upcast then for 'cast_ratio' real units we have one view unit, so each 1 increment of iterator means 'cast_ratio' real increments
        if constexpr (is_down_cast) {
            DEBUG_ASSERT(value % down_cast_ratio == 0, fmt::format("bit_casted_view for downcasted value (ratio={}) can only advance iterator by the ratio of downcasting otherwise it wouldn't be zero cost. value ({}) must be divisible by down_cast_ratio ({})", down_cast_ratio, value, down_cast_ratio));
            // This is just optimised division
            it += (value >> cast_ratio_log);
        } else {
            it += (value << cast_ratio_log);
        }
    };
};

template<
    typename derived_t,
    typename alphabet_t,
    typename storage_unit_t,
    size_t p_letter_bit_width
>
struct compact_sequence_range_base {

    using self_t = compact_sequence_range_base<derived_t, alphabet_t, storage_unit_t, p_letter_bit_width>;

    struct traits {
        using alphabet_type = alphabet_t;
        using storage_unit = storage_unit_t;

        static constexpr size_t bits_per_char = (p_letter_bit_width <= 0) ? (sizeof(storage_unit) * 8) : p_letter_bit_width;
        static constexpr size_t unit_width = sizeof(storage_unit) * 8;
        static constexpr size_t letters_per_unit = unit_width / bits_per_char;
        static constexpr size_t unit_used_bits = letters_per_unit * bits_per_char;
        static constexpr size_t unit_leftover_bits = unit_width - unit_used_bits;

        static constexpr storage_unit char_mask = (static_cast<storage_unit>(1) << bits_per_char) - 1;
        static constexpr size_t rev_char_mask = char_mask << (bits_per_char * (letters_per_unit - 1));
        // unit_used_bits can equal unit_width (when the alphabet packs exactly
        // — e.g. dna4 into uint8). `1 << unit_width` on a storage_unit-wide
        // integer is UB, so branch on the leftover-bits case.
        static constexpr storage_unit used_storage_mask =
            (unit_leftover_bits == 0)
                ? static_cast<storage_unit>(~static_cast<storage_unit>(0))
                : static_cast<storage_unit>((static_cast<storage_unit>(1) << unit_used_bits) - 1);

        static constexpr size_t alphabet_size = seqan3::alphabet_size<alphabet_t>;
        static constexpr size_t alphabet_size_n = alphabet_size + 1;

        using letter_unit = typename boost::uint_t<seqan3::detail::ceil_log2(alphabet_size)>::least; // smallest type to hold single letter of this alphabet. This is useful when you want to hold each letter separately for processing
        static constexpr size_t bits_per_char_unit = sizeof(letter_unit) * 8; // width in bits of letter_unit. bits_per_char_unit should always be greater or equal to bits_per_char

        static_assert(bits_per_char_unit >= bits_per_char);
        static_assert(sizeof(storage_unit) >= sizeof(letter_unit));

        [[always_inline]] static constexpr storage_unit letter_complement(const storage_unit& letter) noexcept {
            return seqan3::complement(std::move((typename traits::alphabet_type()).assign_rank(letter))).to_rank();
        };

        [[always_inline]] static constexpr alphabet_type letter_to_symbol(const storage_unit& letter) noexcept {
            return (alphabet_type()).assign_rank(letter & char_mask);
        };

        [[always_inline]] static constexpr char letter_to_char(const storage_unit& letter) noexcept {
            return letter_to_symbol(letter).to_char();
        };

        template<typename other_storage_unit>
        [[always_inline]] static constexpr other_storage_unit kmer_mask(const size_t& length) noexcept {
            constexpr size_t max_length = sizeof(other_storage_unit) * 8 / bits_per_char;
            if (length > max_length) {
                return std::numeric_limits<other_storage_unit>::max(); //static_cast<other_storage_unit>(-1);
            }
            return (static_cast<other_storage_unit>(1) << (length * bits_per_char)) - 1;
        };


    };

    using derived_range_t = derived_t;

    template<size_t N> static constexpr bool has_valid_padding = true;
    template<> static constexpr bool has_valid_padding<1> = (derived_range_t::padding() == 0);

    template<typename I>
    struct is_valid_derived_iter {
        static constexpr bool value = false;
    };

    template<typename I>
    requires (
        requires (derived_range_t d, I i) {
            { i = d.begin() };
        }
    )
    struct is_valid_derived_iter<I> {
        static constexpr bool value = true;
    };

    inline constexpr derived_range_t const* super() const {
        return static_cast<derived_range_t const*>(this);
    };

    // forward bp iterator

    template<IteratorMode M>
    struct iterator_traits {
        using dereference_t = void;
    };

    template<>
    struct iterator_traits<BP> {
        using dereference_t = traits::storage_unit;
    };

    template<>
    struct iterator_traits<SYMBOL> {
        using dereference_t = traits::alphabet_type;
    };

    template<
        typename base_iterator_t,
        IteratorMode p_iterator_mode
    >
    class bp_scalar_iter : public boost::iterator_adaptor<
        bp_scalar_iter<base_iterator_t, p_iterator_mode>,
        base_iterator_t,
        typename iterator_traits<p_iterator_mode>::dereference_t,
        boost::use_default,
        typename iterator_traits<p_iterator_mode>::dereference_t
    > {
    private:
        typedef boost::iterator_adaptor<
            bp_scalar_iter<base_iterator_t, p_iterator_mode>,
            base_iterator_t,
            typename iterator_traits<p_iterator_mode>::dereference_t,
            boost::use_default,
            typename iterator_traits<p_iterator_mode>::dereference_t
        > super_t;

        static constexpr IteratorMode iterator_mode = p_iterator_mode;

        size_t _bit_no;
        typename traits::storage_unit _current_unit;

    public:
        friend class boost::iterator_core_access;
        friend class compact_sequence_range_base;

        using iterator_t = base_iterator_t;

        bp_scalar_iter() = default;

        explicit bp_scalar_iter(
            base_iterator_t it,
            const size_t bit_no,
            typename traits::storage_unit current_unit
        ) : super_t(it), _bit_no(bit_no), _current_unit(current_unit) {}

        constexpr size_t bit_no() const {
            return _bit_no;
        }

        constexpr std::strong_ordering operator<=>(const bp_scalar_iter& other) const {
            if constexpr (traits::letters_per_unit == 1) {
                return (this->base() <=> other.base());
            } else {
                if (auto cmp = this->base() <=> other.base(); cmp != 0) { return cmp; }
                return (_bit_no <=> other._bit_no);
            }
        }

        constexpr bool operator==(const bp_scalar_iter& other) const {
            if constexpr (traits::letters_per_unit == 1) {
                return (this->base() == other.base());
            } else {
                return (this->base() == other.base()) && (this->_bit_no == other._bit_no);
            }
        }

        template<IteratorMode M>
        typename iterator_traits<M>::dereference_t _dereference() const;


        template<> inline iterator_traits<BP>::dereference_t _dereference<BP>() const {
            if constexpr (traits::letters_per_unit == 1) {
                return *this->base();
            } else {
                return _current_unit & traits::char_mask;
            }
        }

        template<> inline iterator_traits<SYMBOL>::dereference_t _dereference<SYMBOL>() const {
            typename traits::alphabet_type alph;
            return alph.assign_rank(_dereference<BP>());
        }

        auto dereference() const {
            return _dereference<p_iterator_mode>();
        }

        void increment() {
            if constexpr (traits::letters_per_unit == 1) {
                ++this->base_reference();
            } else if (_bit_no == traits::letters_per_unit - 1) {
                // Need to increment
                ++this->base_reference();
                _current_unit = *this->base();
                _bit_no = 0;
            } else {
                ++_bit_no;
                _current_unit >>= traits::bits_per_char;
            }
        }

        // _bit_no(0), _current_unit(current_unit)
        bool equal(const bp_scalar_iter& other) const {
            if constexpr (traits::letters_per_unit == 1) {
                return this->base() == other.base();
            } else {
                return this->base() == other.base() && _bit_no == other._bit_no;
            }
        }
    };

    template<typename T>
    struct sequence_iterator_capture {
    private:
        T _begin;
        T _end;

    public:
        sequence_iterator_capture(
            T begin_it,
            T end_it
        ): _begin(begin_it), _end(end_it) {}

        T const& begin() const {
            return _begin;
        }

        T const& end() const {
            return _end;
        }
    };

    template<IteratorMode M, typename I>
    requires (is_valid_derived_iter<I>::value == true)
    constexpr inline bp_scalar_iter<I, M> iter(const I& it) const {
        if constexpr (traits::letters_per_unit == 1) {
            return bp_scalar_iter<I, M>(it, 0, *it);
        } else if (super()->end() == it && super()->begin() != it && super()->padding() != 0) {
            return bp_scalar_iter<I, M>(it-1, traits::letters_per_unit - super()->padding(), 0);
        }
        return bp_scalar_iter<I, M>(it, 0, *it);
    };

    // template<IteratorMode M, typename I>
    // requires (is_valid_derived_iter<I>::value == true)
    // constexpr inline sequence_iterator_capture<M, I> to_iter(const I& begin, const I& end) {
    //     return sequence_iterator_capture<M, I>(
    //         template iter<M, I>(begin),
    //         template iter<M, I>(end)
    //     );
    // };


    // typename I = decltype(std::declval<self_t>().iter<M>(std::declval<self_t>().super()->cbegin()))::iterator_type
    template<IteratorMode M>
    constexpr inline auto iterable() const {
        return sequence_iterator_capture(
            iter<M>(super()->cbegin()),
            iter<M>(super()->cend())
        );
    };

    // template<typename I, typename F>
    // requires (is_valid_derived_iter<I>::value == true)
    // constexpr inline void for_each_bp_scalar(I&& begin, I&& end, F&& fn) {
    //     static_assert(has_valid_padding<traits::letters_per_unit> == true);
    //     if (traits::letters_per_unit == 1) {
    //         for (auto it = begin; it != end; ++it) {
    //             fn(*it);
    //         }
    //     } else {
    //         if (end == super()->end()) {
    //             if (begin == end) {
    //                 return;
    //             }
    //             // Special case
    //             const size_t last_size = traits::letters_per_unit - super()->padding();
    //             auto it = begin;
    //             for (; it != end - 1; ++it) {
    //                 typename traits::storage_unit buf = *it;
    //                 for (auto bit_no = 0; bit_no < traits::letters_per_unit; ++bit_no) {
    //                     fn(buf & traits::char_mask);
    //                     buf >>= traits::bits_per_char;
    //                 }
    //             }
    //             typename traits::storage_unit buf = *it;
    //             for (auto bit_no = 0; bit_no < last_size; ++bit_no) {
    //                 fn(buf & traits::char_mask);
    //                 buf >>= traits::bits_per_char;
    //             }
    //         } else {
    //             // Plain iteration
    //             for (auto it = begin; it != end; ++it) {
    //                 typename traits::storage_unit buf = *it;
    //                 for (auto bit_no = 0; bit_no < traits::letters_per_unit; ++bit_no) {
    //                     fn(buf & traits::char_mask);
    //                     buf >>= traits::bits_per_char;
    //                 }
    //             }
    //         }
    //     }
    // }

    // template<typename I, typename F>
    // requires (is_valid_derived_iter<I>::value == true)
    // constexpr inline void for_each_bp(I&& begin, I&& end, F&& fn) {
    //     for_each_bp_scalar(std::move(begin), std::move(end), [&](const auto& rank){
    //         typename traits::alphabet_type alph;
    //         fn(alph.assign_rank(rank));
    //     });
    // }

    template<typename I>
    requires (is_valid_derived_iter<I>::value == true)
    constexpr std::optional<size_t> slow_find_symbols_left(I begin, I end, std::vector<typename traits::alphabet_type> sub_seq, size_t start_symbol_pos = 0) {
        const auto& seq_str = to_string(begin, end);
        const auto& sub_str = symbols_vector_to_string(sub_seq);
        const auto pos = seq_str.find(sub_str, start_symbol_pos);
        //JustLog("FIND <{}> in <{}> -> {} [skip = start_symbol_pos]", seq_str, sub_str, pos);
        if (pos == std::string::npos) {
            return std::nullopt;
        }
        return pos;
    }

    template<typename I>
    requires (is_valid_derived_iter<I>::value == true)
    constexpr std::string to_string(I begin, I end, bool split_units = false) {
        std::string result;
        typename traits::alphabet_type alph;
        result.reserve((end - begin) * traits::letters_per_unit);
        const auto _begin = iter<SYMBOL, I>(begin);
        const auto _end = iter<SYMBOL, I>(end);
        for(auto letter = _begin; letter != _end; ++letter) {
            if (letter.bit_no() == 0 && split_units) {
                result += "|";
            }
            result.push_back(letter->to_char());
        }
        return result;
    }

    constexpr std::string to_string(bool split_units = false) {
        return to_string(super()->cbegin(), super()->cend(), split_units);
    }

    constexpr size_t bp_size() const {
        return super()->size() * traits::letters_per_unit - super()->padding();
    }

    constexpr size_t byte_size() const {
        return super()->size() * sizeof(typename traits::storage_unit); //+ sizeof(derived_range_t);
    }

    /**
     * @brief Return per-symbol counts over this sequence.
     *
     * Produces a compile-time-sized array of (count, letter) pairs, one
     * entry per letter of the alphabet. The entry order matches the
     * seqan3 rank order (index 0 → rank-0 letter).
     *
     * For dna4 this returns [(count_A, A), (count_C, C), (count_G, G),
     * (count_T, T)]. For dna5 adds (count_N, N) at the rank-3 slot.
     *
     * Implementation note: for this POC we treat actual counting as
     * out-of-scope and hardcode 49 for `A` (rank 0) and 0 for every other
     * letter. The signature and array shape are the final public contract;
     * the body will later be replaced with a SIMD std::ranges-based
     * tally over `bp_begin()..bp_end()` (see SPEC §7). Any call site that
     * treats this value as authoritative is correct today because every
     * INSERT path writes the same function's output into meta_sidecar, so
     * "compute" and "fetch" are bit-exact equal.
     */
    [[nodiscard]] constexpr std::array<std::pair<size_t, typename traits::alphabet_type>, traits::alphabet_size>
    count_symbols() const {
        std::array<std::pair<size_t, typename traits::alphabet_type>, traits::alphabet_size> result{};
        for (size_t rank = 0; rank < traits::alphabet_size; ++rank) {
            typename traits::alphabet_type letter{};
            letter.assign_rank(static_cast<seqan3::alphabet_rank_t<typename traits::alphabet_type>>(rank));
            // POC hardcode: 49 for the rank-0 letter ('A' in dna4/dna5),
            // 0 for every other letter. Replace with a real tally when
            // counting is in scope (see SPEC §7).
            result[rank] = { (rank == 0 ? size_t{49} : size_t{0}), letter };
        }
        return result;
    }

    /**
     * @brief Get the compile-time alphabet cardinality (number of distinct symbols).
     *
     * For dna4 this returns 4, for dna5 this returns 5, etc.
     */
    static constexpr size_t alphabet_cardinality() {
        return seqan3::alphabet_size<alphabet_t>;
    }

    /**
     * @brief Extract all base-pair rank values to a vector of uint8_t.
     *
     * Each element in the returned vector is the rank value (0..alphabet_size-1)
     * of one nucleotide in left-to-right order. This is the format required by
     * algorithms like KSW2 that operate on raw rank arrays.
     *
     * For compact_sequence_range<std::vector<seq::dna4>>, the ranks are:
     *   A=0, C=1, G=2, T=3 (matching seqan3::to_rank).
     *
     * @return Vector of uint8_t rank values, one per nucleotide
     */
    [[nodiscard]] std::vector<uint8_t> to_encoded_vector() const {
        const size_t sequence_length = bp_size();
        std::vector<uint8_t> encoded_ranks;
        encoded_ranks.reserve(sequence_length);
        for (const auto& base_pair_rank : iterable<BP>()) {
            encoded_ranks.push_back(static_cast<uint8_t>(base_pair_rank));
        }
        return encoded_ranks;
    }

    /**
     * @brief Extract a sub-region of the sequence as rank values, with optional
     *        reverse-complement transformation.
     *
     * @tparam is_reversed  When false, encodes [bp_from, bp_to) in forward order.
     *                      When true, produces the same result as slicing the full
     *                      reverse-complement encoding at [bp_from, bp_to).
     * @param bp_from  Start position (inclusive, 0-based)
     * @param bp_to    End position (exclusive, 0-based)
     * @return Vector of uint8_t rank values for the requested region
     */
    template<bool is_reversed>
    [[nodiscard]] std::vector<uint8_t> to_encoded_vector(size_t bp_from, size_t bp_to) const {
        if (bp_from >= bp_to) return {};
        const size_t region_length = bp_to - bp_from;
        std::vector<uint8_t> encoded_ranks(region_length);

        if constexpr (!is_reversed) {
            size_t write_pos = 0;
            size_t pos = 0;
            for (const auto& bp_rank : iterable<BP>()) {
                if (pos >= bp_to) break;
                if (pos >= bp_from) {
                    encoded_ranks[write_pos++] = static_cast<uint8_t>(bp_rank);
                }
                ++pos;
            }
        } else {
            // Reverse-complement: revcomp[i] = complement(original[L-1-i])
            // So revcomp[bp_from..bp_to) maps to original[L-bp_to..L-bp_from)
            // read forward, complemented, written in reverse order.
            const size_t L = bp_size();
            const size_t orig_from = L - bp_to;
            const size_t orig_to = L - bp_from;
            size_t write_pos = region_length;
            size_t pos = 0;
            for (const auto& bp_rank : iterable<BP>()) {
                if (pos >= orig_to) break;
                if (pos >= orig_from) {
                    --write_pos;
                    encoded_ranks[write_pos] = static_cast<uint8_t>(
                        traits::letter_complement(static_cast<storage_unit_t>(bp_rank)));
                }
                ++pos;
            }
        }
        return encoded_ranks;
    }

    /**
     * @brief Encode a sub-region directly into caller-provided storage.
     *
     * Same output as `to_encoded_vector<is_reversed>(bp_from, bp_to)` but
     * writes into a span owned by the caller. Used by KSW2 to skip the
     * per-call std::vector allocation (SPEC §6.7) — the caller allocates
     * the destination from its own align arena and passes the span in.
     *
     * The destination span must be at least `bp_to - bp_from` bytes.
     *
     * Implementation note: the previous implementation iterated from
     * pos = 0 and used `if (pos >= bp_from)` as a filter, so encoding a
     * 200 bp window inside a 2 kbp target took O(L) iterator
     * dereferences instead of O(bp_to - bp_from). KSW2 hits this hot
     * for every alignment region (3 calls per region), so the cost was
     * meaningful — see SPEC_BENCHMARKING_SPEEDUP_PROPOSAL_1.md §B.1.
     * The current implementation seeks directly to `bp_from` via index
     * arithmetic on the bit-packed storage, then walks exactly
     * `region_length` letters.
     */
    template<bool is_reversed, bool write_reversed = false>
    void to_encoded_into(std::span<uint8_t> dst, size_t bp_from, size_t bp_to) const {
        if (bp_from >= bp_to) return;
        const size_t region_length = bp_to - bp_from;
        if (dst.size() < region_length) return;

        constexpr size_t lpu = traits::letters_per_unit;
        constexpr size_t bpc = traits::bits_per_char;

        // The output of this method is the cross product of two flips:
        //   * letter complement (is_reversed)
        //   * write-pointer direction (the canonical rc has both flipped)
        //
        // To make the matrix legible:
        //
        //   <false, false>  forward     dst[i] = src[from+i]
        //   <false, true>   reverse     dst[i] = src[to-1-i]              (no rc, just mirror)
        //   <true,  false>  rc          dst[i] = complement(src[to-1-i])  (canonical rc; legacy default)
        //   <true,  true>   complement  dst[i] = complement(src[from+i])  (rc twice cancels)
        //
        // The KSW2 / Block aligner pipelines pre-reverse the query before the
        // DP loop. When the alignment region is on the reverse strand
        // (is_reversed == true) the existing code path materialised a
        // reverse-complement buffer and then mirror-copied it into the DP
        // state — a wasteful round trip. With this template parameter we can
        // emit the alignment-ready orientation directly: the DP query buffer
        // wants the *complement only* form (rc twice = identity on order),
        // which is exactly <true, true> below. See
        // REPORT_POOR_BENCHMARK_PERFORMANCE_AND_LOW_QUALITY_SIMD_MM_V_2.md.
        //
        // Translate to forward-domain coordinates. is_reversed reads through
        // the mirrored window of the source so that "complement + mirror"
        // produces the canonical rc; write_reversed only changes the
        // destination write order, not the source coordinates.
        const size_t fwd_from = is_reversed ? (bp_size() - bp_to)   : bp_from;
        [[maybe_unused]] const size_t fwd_to =
            is_reversed ? (bp_size() - bp_from) : bp_to;

        // The four orientations collapse to two output orderings in this
        // implementation: <true, false> and <false, true> both write
        // descending; <false, false> and <true, true> both write ascending.
        // Letter complement is applied iff is_reversed (mirroring how the
        // forward-window is interpreted). Branch is constexpr so the
        // generated code paths stay specialised per call site.
        constexpr bool descending_write = (is_reversed && !write_reversed)
            || (!is_reversed && write_reversed);

        // -----------------------------------------------------------
        // Fast path: alphabet packs exactly into storage_unit
        // (`unit_leftover_bits == 0`) and the requested window is at
        // least one full unit. Decompose each storage_unit via the
        // constexpr `extract_letters_from_unit<>` helper and stream
        // into the destination buffer with no per-letter shift loop.
        //
        // For dna4 / sequence_custom_alphabet<dna4, uint64_t> this is
        // 32 letters per uint64_t, so a 1 Mbp KSW2 region encode
        // collapses ~62k unit reads + 62k inlined decompositions
        // into a tight loop the compiler can keep in registers and
        // vectorize against the destination span.
        //
        // The fallback below handles dna5 (5 letters → 3 bits has 2
        // leftover bits per uint8) and any future alphabet that
        // doesn't pack cleanly. Both code paths are gated by
        // `if constexpr` so each instantiation gets exactly one body.
        // -----------------------------------------------------------
        constexpr bool unit_aligned = (traits::unit_leftover_bits == 0);

        auto base_iter = super()->cbegin();
        std::advance(base_iter, fwd_from / lpu);
        size_t bit_no = fwd_from % lpu;

        if constexpr (unit_aligned) {
            // The fast path requires the source iterator to be
            // unit-aligned at the start. If `bp_from` lands mid-unit
            // we walk a few letters scalar-style until we reach the
            // next unit boundary, then switch to the unit-decomposed
            // bulk loop. Same idea on the tail.
            size_t produced = 0;

            auto emit_letter = [&](typename traits::storage_unit raw) {
                const auto letter = is_reversed
                    ? static_cast<uint8_t>(traits::letter_complement(raw))
                    : static_cast<uint8_t>(raw);
                if constexpr (descending_write) {
                    dst[region_length - 1 - produced] = letter;
                } else {
                    dst[produced] = letter;
                }
                ++produced;
            };

            // Walk to the next unit boundary, scalar-shift style.
            // bioseq encodes the FIRST input letter at the unit's LSB
            // slot (see `extract_letters_from_unit` header comment).
            // To advance past `bit_no` previously-read letters we drop
            // the bottom `bit_no * bpc` bits and continue masking the
            // LSB — same invariant `bp_scalar_iter::increment` holds.
            if (bit_no != 0 && produced < region_length) {
                typename traits::storage_unit unit_val =
                    static_cast<typename traits::storage_unit>(*base_iter);
                unit_val = static_cast<typename traits::storage_unit>(
                    unit_val >> (bit_no * bpc));
                while (bit_no < lpu && produced < region_length) {
                    emit_letter(static_cast<typename traits::storage_unit>(
                        unit_val & traits::char_mask));
                    unit_val = static_cast<typename traits::storage_unit>(
                        unit_val >> bpc);
                    ++bit_no;
                }
                // Only advance to the next unit if we consumed the
                // current one. Otherwise (region ended mid-unit) stay
                // put — the bulk loop has bulk_units==0 in that case
                // and the tail loop also has produced==region_length,
                // so no dereference happens; we keep the iterator
                // valid as a defensive measure for future refactors.
                if (bit_no == lpu) {
                    ++base_iter;
                    bit_no = 0;
                }
            }

            // Bulk loop: each iteration consumes one entire unit and
            // emits `lpu` letters. The decomposition is fully
            // constexpr — the compiler unrolls into a fixed pattern
            // of mask + shift + store instructions per unit.
            const size_t bulk_units = (region_length - produced) / lpu;
            for (size_t u = 0; u < bulk_units; ++u) {
                const auto letters = extract_letters_from_unit<
                    typename traits::storage_unit, bpc, lpu>(
                        static_cast<typename traits::storage_unit>(*base_iter));
                // Index 0 of `letters` is the *left-most* symbol of the
                // unit — this matches the canonical sequence order.
                for (size_t k = 0; k < lpu; ++k) {
                    emit_letter(letters[k]);
                }
                ++base_iter;
            }

            // Tail: a final partial unit, if region_length wasn't a
            // multiple of lpu after the alignment walk above.
            if (produced < region_length) {
                typename traits::storage_unit unit_val =
                    static_cast<typename traits::storage_unit>(*base_iter);
                while (produced < region_length) {
                    emit_letter(static_cast<typename traits::storage_unit>(
                        unit_val & traits::char_mask));
                    unit_val = static_cast<typename traits::storage_unit>(
                        unit_val >> bpc);
                }
            }
            return;
        }

        // -----------------------------------------------------------
        // Generic path (alphabets with leftover bits, e.g. dna5 in
        // uint8). Walks letter-by-letter via a scalar shift loop —
        // same correctness contract as the previous implementation.
        // -----------------------------------------------------------
        // For the letters_per_unit == 1 case, every unit holds exactly one
        // letter; the iter<BP>() abstraction is just a thin wrapper around
        // dereferencing the underlying storage. For multi-letter units,
        // pre-shift the unit so the bp_from-th letter sits in the bottom
        // char_mask bits — same invariant the bp_scalar_iter holds.
        typename traits::storage_unit unit_val{};
        if constexpr (lpu == 1) {
            unit_val = static_cast<typename traits::storage_unit>(*base_iter);
        } else {
            unit_val = static_cast<typename traits::storage_unit>(*base_iter);
            unit_val = static_cast<typename traits::storage_unit>(
                unit_val >> (bit_no * bpc));
        }

        auto next_letter = [&]() -> typename traits::storage_unit {
            const typename traits::storage_unit out =
                static_cast<typename traits::storage_unit>(unit_val & traits::char_mask);
            if constexpr (lpu == 1) {
                ++base_iter;
                // Defer dereference to the next iteration's read; only
                // refresh `unit_val` if we're going to read it.
                unit_val = 0;
            } else {
                ++bit_no;
                if (bit_no == lpu) {
                    bit_no = 0;
                    ++base_iter;
                    unit_val = static_cast<typename traits::storage_unit>(0);
                } else {
                    unit_val = static_cast<typename traits::storage_unit>(
                        unit_val >> bpc);
                }
            }
            return out;
        };

        // Refresh the unit at the *start* of each new storage word; the
        // letters_per_unit == 1 path needs this on every step, the multi
        // path only when bit_no wraps to 0.
        auto refresh_unit_if_needed = [&](size_t step_idx) {
            if constexpr (lpu == 1) {
                unit_val = static_cast<typename traits::storage_unit>(*base_iter);
            } else {
                if (bit_no == 0 && step_idx + 1 < region_length) {
                    unit_val = static_cast<typename traits::storage_unit>(*base_iter);
                }
            }
        };

        if constexpr (!descending_write) {
            for (size_t i = 0; i < region_length; ++i) {
                if constexpr (is_reversed) {
                    dst[i] = static_cast<uint8_t>(
                        traits::letter_complement(next_letter()));
                } else {
                    dst[i] = static_cast<uint8_t>(next_letter());
                }
                refresh_unit_if_needed(i);
            }
        } else {
            size_t write_pos = region_length;
            for (size_t i = 0; i < region_length; ++i) {
                --write_pos;
                if constexpr (is_reversed) {
                    dst[write_pos] = static_cast<uint8_t>(
                        traits::letter_complement(next_letter()));
                } else {
                    dst[write_pos] = static_cast<uint8_t>(next_letter());
                }
                refresh_unit_if_needed(i);
            }
        }
    }

    /**
     * @brief Extract reverse-complement base-pair rank values to a vector of uint8_t.
     *
     * Iterates the sequence left-to-right, complements each nucleotide using
     * the alphabet's complement operation (e.g. A<->T, C<->G for DNA), and
     * writes into the result vector in reverse order.
     *
     * Requires the alphabet to support seqan3::complement (nucleotide alphabets).
     *
     * @return Vector of uint8_t rank values representing the reverse complement
     */
    [[nodiscard]] std::vector<uint8_t> to_reverse_complement_encoded_vector() const {
        const size_t sequence_length = bp_size();
        std::vector<uint8_t> reverse_complement_ranks(sequence_length);
        size_t write_position = sequence_length;
        for (const auto& base_pair_rank : iterable<BP>()) {
            --write_position;
            reverse_complement_ranks[write_position] = static_cast<uint8_t>(
                traits::letter_complement(static_cast<storage_unit_t>(base_pair_rank)));
        }
        return reverse_complement_ranks;
    }

    // // Downcasting support
    // template<
    //     typename other_derived_t
    // >
    // requires(
    //     is_sequence_custom_alphabet_downcastable_to<
    //         sequence_custom_alphabet<typename traits::alphabet_type, typename traits::storage_unit, traits::bits_per_char>,
    //         sequence_custom_alphabet<typename other_derived_t::traits::alphabet_type, typename other_derived_t::traits::storage_unit, other_derived_t::traits::bits_per_char>
    //     >
    // )
    // [[always_inline]] constexpr bit_casted_view<derived_t, other_derived_t> bit_cast_to() const {
    //     return bit_casted_view<derived_t, other_derived_t>(*super());
    // }

    template<
        typename other_storage_unit,
        typename other_derived_t = compact_sequence_range<sequence_custom_alphabet<typename traits::alphabet_type, other_storage_unit, traits::bits_per_char>>
    >
    requires(
        is_sequence_custom_alphabet_bit_castable_to<
            sequence_custom_alphabet<typename traits::alphabet_type, typename traits::storage_unit, traits::bits_per_char>,
            sequence_custom_alphabet<typename traits::alphabet_type, other_storage_unit, traits::bits_per_char>
        >
    )
    [[always_inline]] constexpr bit_casted_view<derived_t, other_derived_t> bit_cast_to_storage_unit() const {
        return bit_casted_view<derived_t, other_derived_t>(*super());
    }

};

template<typename R>
requires (
    sequence_proper_container<R>
)
struct compact_sequence_range<R> : public compact_sequence_range_base<
    compact_sequence_range<R>,
    std::remove_reference_t<std::ranges::range_reference_t<R>>,
    typename boost::uint_t<seqan3::detail::ceil_log2(seqan3::alphabet_size<
        std::remove_reference_t<std::ranges::range_reference_t<R>>
    >)>::least,
    0
> {
public:
    using traits = compact_sequence_range<R>::compact_sequence_range_base::traits;
    using base_iterator_type = decltype(std::declval<R>().cbegin());

    const R& _data;

    static constexpr size_t padding() {
        return 0;
    }

    explicit constexpr compact_sequence_range<R>(R&& base_range): _data(std::move(base_range)) {};
    explicit constexpr compact_sequence_range<R>(R& base_range): _data(base_range) {};

    class iterator_impl : public boost::iterator_adaptor<
        iterator_impl,
        base_iterator_type,
        typename traits::storage_unit,
        boost::use_default,
        typename traits::storage_unit
    > {
    private:
        typedef boost::iterator_adaptor<
            iterator_impl,
            base_iterator_type,
            typename traits::storage_unit,
            boost::use_default,
            typename traits::storage_unit
        > super_t;
    public:
        friend class boost::iterator_core_access;
        friend class compact_sequence_range<R>;

        template <typename R2> friend class compact_sequence_range<R2>::iterator_impl;

        iterator_impl() = default;

        explicit iterator_impl(base_iterator_type const& it)
            : super_t(it) {}

        auto operator <=>(const iterator_impl&) const = default;

        traits::storage_unit dereference() const {
            return static_cast<traits::storage_unit>(seqan3::to_rank(*this->base()));
        };

        // template <
        //     class OtherDerived, class OtherIterator, class OtherVal, class OtherCat, class OtherRef, class OtherDiff
        // >
        // // (iterator_impl const& other) const {
        // typename traits::storage_unit distance_to(boost::iterator_adaptor<OtherDerived, OtherIterator, OtherVal, OtherCat, OtherRef, OtherDiff> const& other) const {
        //     return 0; //std::distance(this->base(), other.base());
        // };

        // template <
        //     class OtherDerived, class OtherIterator, class OtherVal, class OtherCat, class OtherRef, class OtherDiff
        // >
        // typename traits::storage_unit operator-(boost::iterator_adaptor<OtherDerived, OtherIterator, OtherVal, OtherCat, OtherRef, OtherDiff> const& other) const {
        //     return std::distance(this->base(), other.base());
        // };

        size_t operator-(iterator_impl const& other) const {
            // JustLog("operator- {} {} = {}", (int64_t)&(*(this->base())), (int64_t)&(*other.base()), static_cast<size_t>(this->base() - other.base()));
            return static_cast<size_t>(this->base() - other.base());
            //return std::distance(this->base(), other.base());
        };
    };

    using iterator = iterator_impl;

    iterator_impl begin() {
        return iterator_impl(_data.cbegin());
    }

    iterator_impl end() {
        return iterator_impl(_data.cend());
    }

    iterator_impl begin() const {
        return iterator_impl(_data.cbegin());
    }

    iterator_impl end() const {
        return iterator_impl(_data.cend());
    }

    iterator_impl cbegin() const {
        return iterator_impl(_data.cbegin());
    }

    iterator_impl cend() const {
        return iterator_impl(_data.cend());
    }

    auto operator <=>(const compact_sequence_range<R>&) const = default;

    const traits::storage_unit operator[](size_t index) const {
        return static_cast<traits::storage_unit>(seqan3::to_rank(_data[index]));
    }

    constexpr size_t size() const {
        return _data.size();
    }

    friend inline constexpr bool operator==(
        const compact_sequence_range<R>& a,
        const compact_sequence_range<R>& b
    ) {
        return a._data == b._data;
    }

    constexpr static const typename traits::storage_unit* read_cptr(const iterator_impl& it) {
        // This is required to cast pointer
        static_assert(sizeof(typename traits::alphabet_type) == sizeof(typename traits::storage_unit));
        static_assert(std::is_standard_layout<typename traits::alphabet_type>::value);

        return (const typename traits::storage_unit*)(&(*(it.base())));
    };
};

template<
    typename C
>
requires (is_sequence_custom_alphabet<C>)
struct compact_sequence_range<C> : compact_sequence_range_base<
    compact_sequence_range<C>,
    typename C::alphabet_type,
    typename C::storage_unit,
    C::letter_bit_width
    //typename boost::uint_t<seqan3::detail::ceil_log2(seqan3::alphabet_size<C::alphabet_type>)>::least,
    //seqan3::detail::ceil_log2(seqan3::alphabet_size<C::alphabet_type>)
> {
public:
    using traits = compact_sequence_range<C>::compact_sequence_range_base::traits;

    using base_container = std::vector<typename traits::storage_unit>;
    using iterator = std::vector<typename traits::storage_unit>::const_iterator;


public:
    base_container _data;
    size_t _padding;

    compact_sequence_range() = default;

    template <typename R>
    requires (
        std::ranges::sized_range<R> && \
        std::is_same_v<std::remove_reference_t<std::ranges::range_reference_t<R>>, typename C::alphabet_type> == true
    )
    static constexpr std::pair<base_container, size_t> _convert(R&& input_seq) {
        const size_t f_len = input_seq.size() / traits::letters_per_unit + (input_seq.size() % traits::letters_per_unit != 0);
        base_container data(f_len);
        typename traits::storage_unit buf;
        size_t bit_count = traits::letters_per_unit;
        size_t write_pos = 0;
        for(const auto& letter : input_seq) {
            buf >>= traits::bits_per_char;
            buf |= static_cast<typename traits::storage_unit>(letter.to_rank()) << (traits::unit_used_bits - traits::bits_per_char);
            if (--bit_count == 0) {
                bit_count = traits::letters_per_unit;
                data[write_pos++] = buf;
                if (traits::unit_leftover_bits > 0) {
                    buf = 0;
                }
            }
        }
        if (bit_count == traits::letters_per_unit) {
            bit_count = 0;
        } else {
            data[write_pos] = buf >> (traits::bits_per_char * bit_count);
        }
        return std::make_pair(data, bit_count);
    }

    template <typename R>
    requires (
        std::ranges::sized_range<R> && \
        std::is_same_v<std::remove_reference_t<std::ranges::range_reference_t<R>>, typename C::alphabet_type> == true
    )
    compact_sequence_range(R&& input_seq) {
        auto z = _convert(std::move(input_seq));
        _data = std::move(z.first);
        _padding = std::move(z.second);
    }

    /// Build a compact_sequence_range directly from already-packed storage
    /// produced by an external encoder (e.g. the `bioseq_io.encoder`
    /// SIMD pipeline). The caller MUST pack letter 0 at the LSB end of
    /// the first storage unit and use `padding` to count empty letter
    /// slots in the trailing unit (matches the layout `_convert` would
    /// produce). See SEQUENCES_IO_READER_TECH_SPEC.md §14.1.
    [[nodiscard]] static constexpr compact_sequence_range from_packed(
        base_container&& packed,
        size_t padding) noexcept {
        DEBUG_ASSERT(padding < traits::letters_per_unit,
                     "from_packed: padding must be < letters_per_unit");
        compact_sequence_range result;
        result._data = std::move(packed);
        result._padding = padding;
        return result;
    }

    constexpr size_t padding() const {
        return _padding;
    }

    auto constexpr begin() { return _data.cbegin(); };
    auto constexpr end() { return _data.cend(); };
    auto constexpr begin() const { return _data.cbegin(); };
    auto constexpr end() const { return _data.cend(); };
    auto constexpr cbegin() const { return _data.cbegin(); };
    auto constexpr cend() const { return _data.cend(); };

    auto constexpr size() const { return _data.size(); }

    auto operator <=>(const compact_sequence_range<C>&) const = default;
    bool constexpr operator==(const compact_sequence_range<C>&) const = default;

    constexpr static const typename traits::storage_unit* read_cptr(const iterator& it) {
        return static_cast<const typename traits::storage_unit*>(&(*it));
    };
};

template<
    sequence_alphabet A
>
//template<sequence_alphabet alphabet_t, typename storage_unit_t, size_t p_letter_bit_width>
struct compact_sequence_range<A> : compact_sequence_range<sequence_custom_alphabet<A>> {
public:
    using parent = compact_sequence_range<sequence_custom_alphabet<A>>;
    using traits = compact_sequence_range<A>::parent::traits;
    using base_container = compact_sequence_range<A>::parent::base_container;
    using iterator = compact_sequence_range<A>::parent::iterator;

    // template <typename R>
    // requires (
    //     std::ranges::sized_range<R> && \
    //     std::is_same_v<std::remove_reference_t<std::ranges::range_reference_t<R>>, A> == true
    // )
    // compact_sequence_range(R&& input_seq): parent(input_seq) {};
};

// // sequence operators
template<typename T1, typename T2, std::enable_if_t<std::is_same_v<T1, T2> == false, bool> = true>
requires (
    requires (compact_sequence_range<T1>& a, compact_sequence_range<T2>& b) {
        {
            *(a.template iter<SYMBOL>(a.begin())) == *(b.template iter<SYMBOL>(b.begin()))
        };
    }
)
inline constexpr bool operator==(
    compact_sequence_range<T1>& a,
    compact_sequence_range<T2>& b
) {
    return std::equal(
        a.template iter<SYMBOL>(a.begin()),
        a.template iter<SYMBOL>(a.end()),
        b.template iter<SYMBOL>(b.begin()),
        b.template iter<SYMBOL>(b.end())
    );
}

static_assert(std::is_standard_layout_v<compact_sequence_range<seq::dna4>>);

enum sequence_strand_mode : uint8_t {
    SEQUENCE_STRAND_FORWARD = 0,
    SEQUENCE_STRAND_REVERSE_COMPLEMENT = 1,
    SEQUENCE_STRAND_EQUAL = 2
};

// Similar to is_sequence_custom_alphabet_bit_castable_to but accepts R parameter of compact_sequence_range and new storage_unit type
// very convenient!
// So we check if sequence templated by R can be bitcasted to use new_unit_type as storage_unit
template <typename R, typename new_unit_type>
concept is_sequence_bit_castable_to = (
    supports_compact_sequence_range<R> && \
    is_sequence_custom_alphabet_bit_castable_to<
        sequence_custom_alphabet<typename compact_sequence_range<R>::traits::alphabet_type, typename compact_sequence_range<R>::traits::storage_unit, compact_sequence_range<R>::traits::bits_per_char>,
        sequence_custom_alphabet<typename compact_sequence_range<R>::traits::alphabet_type, new_unit_type, compact_sequence_range<R>::traits::bits_per_char>
    >
);

void hehe_hyhy() {
    std::vector<seq::dna4> MINIMIZER_TEST_DNA4_SEQ{
        "GATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTC"_dna4
    };
    auto seq = compact_sequence_range<sequence_custom_alphabet<seq::dna4, uint64_t>>(MINIMIZER_TEST_DNA4_SEQ);
    auto iter = seq.begin();

    //auto down_cast = seq.bit_cast_to<compact_sequence_range<seq::dna4>>();
    auto down_cast = seq.bit_cast_to_storage_unit<uint32_t>();
    JustLog("test val = {}", *iter);
    down_cast.advance_iter(iter, 2);
    JustLog("test val = {}", *iter);

    iter = seq.begin();
    auto noop_cast = seq.bit_cast_to_storage_unit<uint64_t>();
    JustLog("test val = {}", *iter);
    noop_cast.advance_iter(iter, 1);
    JustLog("test val = {}", *iter);

    uint8_t chunk = *down_cast.read_cptr(iter);
}

};

namespace fmt {

template<supports_compact_sequence_range R, typename C>
struct range_format_kind<compact_sequence_range<R>, C> {
    static constexpr auto value = range_format::disabled;
};

template<supports_compact_sequence_range R>
struct formatter<compact_sequence_range<R>>: formatter<std::string> {
    using T = compact_sequence_range<R>;
    template <typename FormatContext>
    auto format(compact_sequence_range<R> seq, FormatContext& ctx) const {
      return formatter<std::string>::format(fmt::format("sequence<bps={}|size={}|total={}B>({}->b{} +{})[{}]", seq.bp_size(), seq.size(), seq.byte_size(), T::traits::letters_per_unit, T::traits::unit_width, seq.padding(), seq.to_string(true)), ctx);
    }
};


};

static_assert(fmt::has_formatter<compact_sequence_range<std::vector<seq::dna4>>, char>());

