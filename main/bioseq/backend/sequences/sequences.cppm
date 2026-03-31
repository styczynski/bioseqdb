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
};

template <typename T>
concept is_sequence_custom_alphabet = T::is_valid_sequence_custom_alphabet == true;

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
        static constexpr storage_unit used_storage_mask = (static_cast<storage_unit>(1) << unit_used_bits) - 1;

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

        T const& begin() {
            return _begin;
        }

        T const& end() {
            return _end;
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
    constexpr inline auto iterable() {
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

