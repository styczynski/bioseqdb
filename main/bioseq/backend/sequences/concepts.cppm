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

#include <boost/integer.hpp>
#include <boost/iterator/iterator_adaptor.hpp>


export module bioseq_sequences:concepts;

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
    
// New impl

template<
    typename R
>
struct compact_sequence_range {};

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

    inline constexpr derived_range_t* super() {
        return static_cast<derived_range_t*>(this);
    };

    // forward bp iterator


    template<typename I, typename F>
    requires (is_valid_derived_iter<I>::value == true)
    constexpr inline void for_each_bp_scalar(I&& begin, I&& end, F&& fn) {
        static_assert(has_valid_padding<traits::letters_per_unit> == true);
        if (traits::letters_per_unit == 1) {
            for (auto it = begin; it != end; ++it) {
                fn(*it);
            }
        } else {
            if (end == super()->end()) {
                if (begin == end) {
                    return;
                }
                // Special case
                const size_t last_size = traits::letters_per_unit - super()->padding();
                auto it = begin;
                for (; it != end - 1; ++it) {
                    typename traits::storage_unit buf = *it;
                    for (auto bit_no = 0; bit_no < traits::letters_per_unit; ++bit_no) {
                        fn(buf & traits::char_mask);
                        buf >>= traits::bits_per_char;
                    }
                }
                typename traits::storage_unit buf = *it;
                for (auto bit_no = 0; bit_no < last_size; ++bit_no) {
                    fn(buf & traits::char_mask);
                    buf >>= traits::bits_per_char;
                }
            } else {
                // Plain iteration
                for (auto it = begin; it != end; ++it) {
                    typename traits::storage_unit buf = *it;
                    for (auto bit_no = 0; bit_no < traits::letters_per_unit; ++bit_no) {
                        fn(buf & traits::char_mask);
                        buf >>= traits::bits_per_char;
                    }
                }
            }
        }
    }

    template<typename I, typename F>
    requires (is_valid_derived_iter<I>::value == true)
    constexpr inline void for_each_bp(I&& begin, I&& end, F&& fn) {
        for_each_bp_scalar(std::move(begin), std::move(end), [&](const auto& rank){
            typename traits::alphabet_type alph;
            fn(alph.assign_rank(rank));
        });
    }

    template<typename I>
    requires (is_valid_derived_iter<I>::value == true)
    constexpr std::string to_string(I begin, I end) {
        std::string result;
        typename traits::alphabet_type alph;
        result.reserve((end - begin) * traits::letters_per_unit);
        for_each_bp(begin, end, [&](const auto& alph){
            result += alph.to_char();
        });
        return result;
    }

    constexpr std::string to_string() {
        return to_string(super()->cbegin(), super()->cend());
    }

    constexpr size_t bp_size() const {
        return super()->size() * traits::letters_per_unit - super()->padding();
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
                if (traits::unit_leftover_bits > 0 || true) {
                    buf = 0;
                }
            }
        }
        if (bit_count != 0) {
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

};

static_assert(std::is_standard_layout_v<compact_sequence_range<seq::dna4>>);

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
      return formatter<std::string>::format(fmt::format("sequence<size={}>({}->b{})[{}]", seq.size(), T::traits::letters_per_unit, T::traits::unit_width, seq.to_string()), ctx);
    }
};


};

static_assert(fmt::has_formatter<compact_sequence_range<std::vector<seq::dna4>>, char>());

using namespace seq::literals;
int test() {
    std::vector<seq::dna4> FIXED_TARGET{
		"\
ACTGTAGCTGATCGTAGCTAGTCACTGTAGCT\
GTCGTAGCTAGTCCGTAATCGTAGCTAGTCAA\
ACTGTATCGTAGCTAGTCCGTAGCTACTGTAG\
CTGATCGTAGCTAGTCACTGTAGGATCGTAGC\
TAGTCCGTACTGTAGCTGATCGTAGCCCTAGT\
CACTGTAGCTGATCGTAGCTAGTCACTGTAGC\
TGATCGTAGCTAGTCTCGTAGCTAGTCCGTAA\
CTGTAGCTGATCGTAGCTAGTCACTGTAGCTG\
ATCGTAGCTAGTC\
"_dna4};

	//JustLog("TESTETST = <{}>", physical_alphabet_storage<decltype(FIXED_TARGET)>::to_string(FIXED_TARGET.begin(), FIXED_TARGET.end()))

	compact_sequence_range<decltype(FIXED_TARGET)> testtest(FIXED_TARGET);
	JustLog("heheszke = {}", testtest);
    
}

