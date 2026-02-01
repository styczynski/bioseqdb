module;

#include "common.h"

//include "fmt/ranges.h"

#include <array>
#include <deque>
#include <iterator>
#include <list>
#include <ranges>
#include <set>
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

export {

template<typename A>
concept sequence_alphabet = \
    requires (A const a) {
        { seqan3::to_rank(a) };
        { seqan3::alphabet_size<A> };
    };

template<typename R>
concept sequence_proper_container = \
    sequence_alphabet<std::remove_reference_t<std::ranges::range_reference_t<R>>> && \
    std::ranges::sized_range<R> && \
    std::ranges::random_access_range<R>;

template<typename R>
concept supports_compact_sequence_range = \
    sequence_proper_container<R> || \
    sequence_alphabet<R>;

template <typename T, typename A>
std::string decode_compressed_seq_el(const T& val)  {
    // FIXME: This is very wrong
    constexpr size_t bps_count = sizeof(T) * 8 / seqan3::detail::ceil_log2(A::alphabet_size);
    std::string ret(bps_count, '*');
    T buf = val;
    for(auto i = 0; i < bps_count; ++i) {
        // reverse with: bps_count - i - 1
        ret[i] = A().assign_rank(buf & 3).to_char();
        buf >>= 2;
    }
    return ret;
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

template<
    typename derived_t,
    typename alphabet_t,
    typename storage_unit_t,
    size_t p_letter_bit_width
>
struct compact_sequence_range_base {

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
        static constexpr storage_unit used_storage_mask = (static_cast<storage_unit>(1) << unit_used_bits) - 1;

        [[always_inline]] static constexpr storage_unit letter_complement(const storage_unit& letter) noexcept {
            return seqan3::complement(std::move((typename traits::alphabet_type()).assign_rank(letter))).to_rank();
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


        template<> iterator_traits<BP>::dereference_t _dereference<BP>() const {
            if constexpr (traits::letters_per_unit == 1) {
                return *this->base();
            } else {
                return _current_unit & traits::char_mask;
            }
        }

        template<> iterator_traits<SYMBOL>::dereference_t _dereference<SYMBOL>() const {
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

    template<IteratorMode M, typename I>
    requires (is_valid_derived_iter<I>::value == true)
    constexpr inline bp_scalar_iter<I, M> iter(const I& it) {
        if constexpr (traits::letters_per_unit == 1) {
            return bp_scalar_iter<I, M>(it, 0, *it);
        } else if (super()->end() == it && super()->begin() != it && super()->padding() != 0) {
            return bp_scalar_iter<I, M>(it-1, traits::letters_per_unit - super()->padding(), 0);
        }
        return bp_scalar_iter<I, M>(it, 0, *it);
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
    constexpr std::string to_string(I begin, I end) {
        std::string result;
        typename traits::alphabet_type alph;
        result.reserve((end - begin) * traits::letters_per_unit);
        const auto _begin = iter<SYMBOL, I>(begin);
        const auto _end = iter<SYMBOL, I>(end);
        for(auto letter = _begin; letter != _end; ++letter) {
            if (letter.bit_no() == 0) {
                result += "|";
            }
            result.push_back(letter->to_char());
        }
        return result;
    }

    constexpr std::string to_string() {
        return to_string(super()->cbegin(), super()->cend());
    }

    constexpr size_t bp_size() const {
        return super()->size() * traits::letters_per_unit - super()->padding();
    }

    constexpr size_t byte_size() const {
        return super()->size() * sizeof(typename traits::storage_unit) + sizeof(derived_range_t);
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

        iterator_impl() = default;

        explicit iterator_impl(base_iterator_type const& it)
            : super_t(it) {}

        auto operator <=>(const iterator_impl&) const = default;

        traits::storage_unit dereference() const {
            return static_cast<traits::storage_unit>(seqan3::to_rank(*this->base()));
        }
    };

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
};

template<
    sequence_alphabet A
>
struct compact_sequence_range<A> : compact_sequence_range_base<
    compact_sequence_range<A>,
    A,
    typename boost::uint_t<seqan3::detail::ceil_log2(seqan3::alphabet_size<A>)>::least,
    seqan3::detail::ceil_log2(seqan3::alphabet_size<A>)
> {
public:
    using traits = compact_sequence_range<A>::compact_sequence_range_base::traits;

    using base_container = std::vector<typename traits::storage_unit>;
    using iterator = std::vector<typename traits::storage_unit>::const_iterator;


public:
    base_container _data;
    size_t _padding;

    compact_sequence_range() = default;

    template <typename R>
    requires (
        std::ranges::sized_range<R> && \
        std::is_same_v<std::remove_reference_t<std::ranges::range_reference_t<R>>, A> == true
    )
    static constexpr std::pair<base_container, size_t> _convert(R&& input_seq) {
        const size_t f_len = input_seq.size() / traits::letters_per_unit + (input_seq.size() % traits::letters_per_unit != 0);
        base_container data(f_len);
        typename traits::storage_unit buf;
        size_t bit_count = traits::letters_per_unit;
        size_t write_pos = 0;
        for(const auto& letter : input_seq) {
            buf >>= traits::bits_per_char;
            buf |= letter.to_rank() << (traits::unit_used_bits - traits::bits_per_char);
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
        std::is_same_v<std::remove_reference_t<std::ranges::range_reference_t<R>>, A> == true
    )
    compact_sequence_range(R&& input_seq) {
        auto z = _convert(std::move(input_seq));
        _data = std::move(z.first);
        _padding = std::move(z.second);
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

    auto operator <=>(const compact_sequence_range<A>&) const = default;
    bool constexpr operator==(const compact_sequence_range<A>&) const = default;
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

// hasher
template<
    typename p_derived_t,
    sequence_alphabet p_alphabet_t,
    typename p_input_t,
    typename p_output_t
>
struct hasher_base {
    struct traits : public compact_sequence_range<p_alphabet_t>::compact_sequence_range_base::traits {
        using output_t = p_output_t;
        using input_t = p_input_t;

        using hasher_return_t = std::pair<output_t, sequence_strand_mode>;

        template <typename output_t_like>
        [[always_inline]] static constexpr hasher_return_t select_min_hash_strand(
            const output_t_like forward, 
            const output_t_like reverse_complement
        ) noexcept {
            if (forward == reverse_complement) [[unlikely]] {
                return std::make_pair(static_cast<output_t>(forward), SEQUENCE_STRAND_EQUAL);
            } else if (forward < reverse_complement) {
                return std::make_pair(static_cast<output_t>(forward), SEQUENCE_STRAND_FORWARD);
            } else {
                return std::make_pair(static_cast<output_t>(reverse_complement), SEQUENCE_STRAND_REVERSE_COMPLEMENT);
            }
        }


    };
};


template<template<typename H_A, typename H_O> typename H, typename A, typename O = uint64_t, typename I = uint64_t, typename hasher_t = H<A, O>::template scalar_hash<I>>
concept supports_scalar_hash = sequence_alphabet<A> && requires (hasher_t& hasher, I&& incoming) {
	{ hasher.next(std::move(incoming), std::move(incoming)) } -> std::same_as<typename hasher_t::traits::hasher_return_t>;
};

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
      return formatter<std::string>::format(fmt::format("sequence<size={}|B{}>({}->b{} +{})[{}]", seq.bp_size(), seq.byte_size(), T::traits::letters_per_unit, T::traits::unit_width, seq.padding(), seq.to_string()), ctx);
    }
};


};

static_assert(fmt::has_formatter<compact_sequence_range<std::vector<seq::dna4>>, char>());

