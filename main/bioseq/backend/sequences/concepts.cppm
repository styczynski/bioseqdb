module;

#include "common.h"

#include <array>
#include <deque>
#include <list>
#include <ranges>
#include <set>
#include <valarray>
#include <vector>

#include <boost/integer.hpp>

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

template<
    typename R
>
struct physical_alphabet_storage {};

template<
    typename R
>
requires (
    sequence_alphabet<std::remove_reference_t<std::ranges::range_reference_t<R>>> && \
    std::ranges::sized_range<R> && \
    std::ranges::random_access_range<R>
)
struct physical_alphabet_storage<R> {
    using alphabet_type = std::remove_reference_t<std::ranges::range_reference_t<R>>;
    using storage_unit = boost::int_t<seqan3::detail::ceil_log2(seqan3::alphabet_size<alphabet_type>)>::least;
    using iterator_type = decltype(std::declval<R>().begin());
    using const_iterator_type = decltype(std::declval<R>().cbegin());

    static constexpr size_t letters_per_unit = 1;
    static constexpr storage_unit to_storage_unit(alphabet_type const& a) noexcept {
        return static_cast<storage_unit>(seqan3::to_rank(a));
    }
    static constexpr std::string to_string(const_iterator_type&& begin, const_iterator_type&& end) noexcept {
        // return A().assign_rank(s).to_char();
        //return std::string();
        std::string ret((size_t)(end - begin), '*');
        size_t pos = 0;
        for (auto i = begin; i != end; ++i) {
            ret[pos++] = i->to_char();
        }
        return ret;
    }
};



// Packed sequence
template<sequence_alphabet A>
struct packed_sequence {
    using alphabet_type = A;
    using storage_unit = boost::int_t<seqan3::detail::ceil_log2(seqan3::alphabet_size<A>)>::least;
    static constexpr size_t letters_per_unit = (sizeof(storage_unit) * 8) / seqan3::detail::ceil_log2(seqan3::alphabet_size<A>);
    static constexpr storage_unit to_storage_unit(A const& a) noexcept {
        return static_cast<storage_unit>(seqan3::to_rank(a));
    }
    
    std::vector<storage_unit> _data;

    constexpr auto begin() { return _data.begin(); }
    constexpr auto end() { return _data.end(); }
    constexpr auto cbegin() const { return _data.cbegin(); }
    constexpr auto cend() const { return _data.cend(); }


    using iterator_type = decltype(_data.begin());
    using const_iterator_type = decltype(_data.cbegin());
    static constexpr std::string to_string(const_iterator_type&& begin, const_iterator_type&& end) noexcept {
        // return A().assign_rank(s).to_char();
        return std::string("X");
    }

};

template<sequence_alphabet A>
struct physical_alphabet_storage<packed_sequence<A>> : packed_sequence<A> {};

// container
template<
    typename R
>
concept sequence_container = \
    std::ranges::sized_range<R> && \
    std::ranges::random_access_range<R> && \
    sequence_alphabet<typename physical_alphabet_storage<R>::alphabet_type> && \
    requires (R& r) {
        { typename physical_alphabet_storage<R>::storage_unit() };
        { physical_alphabet_storage<R>::letters_per_unit > 0 };
        { physical_alphabet_storage<R>::to_storage_unit(typename physical_alphabet_storage<R>::alphabet_type()) };
        { physical_alphabet_storage<R>::to_string(r.begin(), r.end()) };
        //{ physical_alphabet_storage<R>::from_storage_unit(typename physical_alphabet_storage<R>::storage_unit()) };
    };

static_assert(sequence_alphabet<seq::dna4>);
static_assert(sequence_container<std::vector<seq::dna4>>);
static_assert(sequence_container<packed_sequence<seq::dna4>>);

};
// template<
//     typename R,
//     typename T = std::remove_reference_t<std::ranges::range_reference_t<R>>
// >
// requires (base_sequence_container<R,T>)
// struct sequence_container_trait {
//     using storage_type = boost::int_t<seqan3::detail::ceil_log2()>::least;
// }


 

