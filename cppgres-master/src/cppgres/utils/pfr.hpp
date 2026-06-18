#pragma once
// Copyright (c) 2016-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_HPP
#define BOOST_PFR_HPP

/// \file boost/pfr.hpp
/// Includes all the Boost.PFR headers

// Copyright (c) 2016-2023 Antony Polukhin
// Copyright (c) 2022 Denis Mikhailov
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_CONFIG_HPP
#define BOOST_PFR_CONFIG_HPP

#if __cplusplus >= 201402L || (defined(_MSC_VER) && defined(_MSVC_LANG) && _MSC_VER > 1900)
#include <type_traits> // to get non standard platform macro definitions (__GLIBCXX__ for example)
#endif

/// \file boost/pfr/config.hpp
/// Contains all the macros that describe Boost.PFR configuration, like BOOST_PFR_ENABLED
///
/// \note This header file doesn't require C++14 Standard and supports all C++ compilers, even pre C++14 compilers (C++11, C++03...).

// Reminder:
//  * MSVC++ 14.2 _MSC_VER == 1927 <- Loophole is known to work (Visual Studio ????)
//  * MSVC++ 14.1 _MSC_VER == 1916 <- Loophole is known to NOT work (Visual Studio 2017)
//  * MSVC++ 14.0 _MSC_VER == 1900 (Visual Studio 2015)
//  * MSVC++ 12.0 _MSC_VER == 1800 (Visual Studio 2013)

#ifdef BOOST_PFR_NOT_SUPPORTED
#   error Please, do not set BOOST_PFR_NOT_SUPPORTED value manually, use '-DBOOST_PFR_ENABLED=0' instead of it
#endif

#if defined(_MSC_VER)
#   if !defined(_MSVC_LANG) || _MSC_VER <= 1900
#       define BOOST_PFR_NOT_SUPPORTED 1
#   endif
#elif __cplusplus < 201402L
#   define BOOST_PFR_NOT_SUPPORTED 1
#endif

#ifndef BOOST_PFR_USE_LOOPHOLE
#   if defined(_MSC_VER)
#       if _MSC_VER >= 1927
#           define BOOST_PFR_USE_LOOPHOLE 1
#       else
#           define BOOST_PFR_USE_LOOPHOLE 0
#       endif
#   elif defined(__clang_major__) && __clang_major__ >= 8
#       define BOOST_PFR_USE_LOOPHOLE 0
#   else
#       define BOOST_PFR_USE_LOOPHOLE 1
#   endif
#endif

#ifndef BOOST_PFR_USE_CPP17
#   ifdef __cpp_structured_bindings
#       define BOOST_PFR_USE_CPP17 1
#   elif defined(_MSVC_LANG)
#       if _MSVC_LANG >= 201703L
#           define BOOST_PFR_USE_CPP17 1
#       else
#           define BOOST_PFR_USE_CPP17 0
#       endif
#   else
#       define BOOST_PFR_USE_CPP17 0
#   endif
#endif

#if (!BOOST_PFR_USE_CPP17 && !BOOST_PFR_USE_LOOPHOLE)
#   if (defined(_MSC_VER) && _MSC_VER < 1916) ///< in Visual Studio 2017 v15.9 PFR library with classic engine normally works
#      define BOOST_PFR_NOT_SUPPORTED 1
#   endif
#endif

#ifndef BOOST_PFR_USE_STD_MAKE_INTEGRAL_SEQUENCE
// Assume that libstdc++ since GCC-7.3 does not have linear instantiation depth in std::make_integral_sequence
#   if defined( __GLIBCXX__) && __GLIBCXX__ >= 20180101
#       define BOOST_PFR_USE_STD_MAKE_INTEGRAL_SEQUENCE 1
#   elif defined(_MSC_VER)
#       define BOOST_PFR_USE_STD_MAKE_INTEGRAL_SEQUENCE 1
//# elif other known working lib
#   else
#       define BOOST_PFR_USE_STD_MAKE_INTEGRAL_SEQUENCE 0
#   endif
#endif

#ifndef BOOST_PFR_HAS_GUARANTEED_COPY_ELISION
#   if  defined(__cpp_guaranteed_copy_elision) && (!defined(_MSC_VER) || _MSC_VER > 1928)
#       define BOOST_PFR_HAS_GUARANTEED_COPY_ELISION 1
#   else
#       define BOOST_PFR_HAS_GUARANTEED_COPY_ELISION 0
#   endif
#endif

#ifndef BOOST_PFR_ENABLE_IMPLICIT_REFLECTION
#   if  defined(__cpp_lib_is_aggregate)
#       define BOOST_PFR_ENABLE_IMPLICIT_REFLECTION 1
#   else
// There is no way to detect potential ability to be reflectable without std::is_aggregare
#       define BOOST_PFR_ENABLE_IMPLICIT_REFLECTION 0
#   endif
#endif

#ifndef BOOST_PFR_CORE_NAME_ENABLED
#   if  (__cplusplus >= 202002L) || (defined(_MSVC_LANG) && (_MSVC_LANG >= 202002L))
#       if (defined(__cpp_nontype_template_args) && __cpp_nontype_template_args >= 201911) \
         || (defined(__clang_major__) && __clang_major__ >= 12)
#           define BOOST_PFR_CORE_NAME_ENABLED 1
#       else
#           define BOOST_PFR_CORE_NAME_ENABLED 0
#       endif
#   else
#       define BOOST_PFR_CORE_NAME_ENABLED 0
#   endif
#endif


#ifndef BOOST_PFR_CORE_NAME_PARSING
#   if defined(_MSC_VER)
#       define BOOST_PFR_CORE_NAME_PARSING (sizeof("auto __cdecl boost::pfr::detail::name_of_field_impl<") - 1, sizeof(">(void) noexcept") - 1, backward("->"))
#   elif defined(__clang__)
#       define BOOST_PFR_CORE_NAME_PARSING (sizeof("auto boost::pfr::detail::name_of_field_impl() [MsvcWorkaround = ") - 1, sizeof("}]") - 1, backward("."))
#   elif defined(__GNUC__)
#       define BOOST_PFR_CORE_NAME_PARSING (sizeof("consteval auto boost::pfr::detail::name_of_field_impl() [with MsvcWorkaround = ") - 1, sizeof(")]") - 1, backward("::"))
#   else
// Default parser for other platforms... Just skip nothing!
#       define BOOST_PFR_CORE_NAME_PARSING (0, 0, "")
#   endif
#endif

#if defined(__has_cpp_attribute)
#   if __has_cpp_attribute(maybe_unused)
#       define BOOST_PFR_MAYBE_UNUSED [[maybe_unused]]
#   endif
#endif

#ifndef BOOST_PFR_MAYBE_UNUSED
#   define BOOST_PFR_MAYBE_UNUSED
#endif

#ifndef BOOST_PFR_ENABLED
#   ifdef BOOST_PFR_NOT_SUPPORTED
#       define BOOST_PFR_ENABLED 0
#   else
#       define BOOST_PFR_ENABLED 1
#   endif
#endif

#undef BOOST_PFR_NOT_SUPPORTED

#endif // BOOST_PFR_CONFIG_HPP
// Copyright (c) 2016-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_CORE_HPP
#define BOOST_PFR_CORE_HPP

// Copyright (c) 2016-2023 Antony Polukhin
// Copyright (c) 2022 Denis Mikhailov
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_DETAIL_CONFIG_HPP
#define BOOST_PFR_DETAIL_CONFIG_HPP


#if !BOOST_PFR_ENABLED

#error Boost.PFR library is not supported in your environment.             \
       Try one of the possible solutions:                                  \
       1. try to take away an '-DBOOST_PFR_ENABLED=0', if it exists        \
       2. enable C++14;                                                    \
       3. enable C++17;                                                    \
       4. update your compiler;                                            \
       or disable this error by '-DBOOST_PFR_ENABLED=1' if you really know what are you doing.

#endif // !BOOST_PFR_ENABLED

#endif // BOOST_PFR_DETAIL_CONFIG_HPP


// Copyright (c) 2016-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_DETAIL_CORE_HPP
#define BOOST_PFR_DETAIL_CORE_HPP


// Each core provides `boost::pfr::detail::tie_as_tuple` and
// `boost::pfr::detail::for_each_field_dispatcher` functions.
//
// The whole PFR library is build on top of those two functions.
#if BOOST_PFR_USE_CPP17
// Copyright (c) 2016-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)


#ifndef BOOST_PFR_DETAIL_CORE17_HPP
#define BOOST_PFR_DETAIL_CORE17_HPP

// Copyright (c) 2016-2023 Antony Polukhin
// Copyright (c) 2023 Denis Mikhailov
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////// THIS HEADER IS AUTO GENERATED BY misc/generate_cpp17.py                                    ////////////////
//////////////// MODIFY AND RUN THE misc/generate_cpp17.py INSTEAD OF DIRECTLY MODIFYING THE GENERATED FILE ////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_PFR_DETAIL_CORE17_GENERATED_HPP
#define BOOST_PFR_DETAIL_CORE17_GENERATED_HPP

#if !BOOST_PFR_USE_CPP17
#   error C++17 is required for this header.
#endif

// Copyright (c) 2016-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_DETAIL_SEQUENCE_TUPLE_HPP
#define BOOST_PFR_DETAIL_SEQUENCE_TUPLE_HPP

// Copyright (c) 2018 Sergei Fedorov
// Copyright (c) 2019-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_DETAIL_MAKE_INTEGER_SEQUENCE_HPP
#define BOOST_PFR_DETAIL_MAKE_INTEGER_SEQUENCE_HPP


#include <type_traits>
#include <utility>
#include <cstddef>

namespace boost { namespace pfr { namespace detail {

#if BOOST_PFR_USE_STD_MAKE_INTEGRAL_SEQUENCE == 0

#ifdef __has_builtin
#   if __has_builtin(__make_integer_seq)
#       define BOOST_PFR_USE_MAKE_INTEGER_SEQ_BUILTIN
#   endif
#endif

#ifdef BOOST_PFR_USE_MAKE_INTEGER_SEQ_BUILTIN

using std::integer_sequence;

// Clang unable to use namespace qualified std::integer_sequence in __make_integer_seq.
template <typename T, T N>
using make_integer_sequence = __make_integer_seq<integer_sequence, T, N>;

#undef BOOST_PFR_USE_MAKE_INTEGER_SEQ_BUILTIN

#else

template <typename T, typename U>
struct join_sequences;

template <typename T, T... A, T... B>
struct join_sequences<std::integer_sequence<T, A...>, std::integer_sequence<T, B...>> {
    using type = std::integer_sequence<T, A..., B...>;
};

template <typename T, T Min, T Max>
struct build_sequence_impl {
    static_assert(Min < Max, "Start of range must be less than its end");
    static constexpr T size = Max - Min;
    using type = typename join_sequences<
            typename build_sequence_impl<T, Min, Min + size / 2>::type,
            typename build_sequence_impl<T, Min + size / 2 + 1, Max>::type
        >::type;
};

template <typename T, T V>
struct build_sequence_impl<T, V, V> {
    using type = std::integer_sequence<T, V>;
};

template <typename T, std::size_t N>
struct make_integer_sequence_impl : build_sequence_impl<T, 0, N - 1> {};

template <typename T>
struct make_integer_sequence_impl<T, 0> {
    using type = std::integer_sequence<T>;
};

template <typename T, T N>
using make_integer_sequence = typename make_integer_sequence_impl<T, N>::type;

#endif // !defined BOOST_PFR_USE_MAKE_INTEGER_SEQ_BUILTIN
#else // BOOST_PFR_USE_STD_MAKE_INTEGRAL_SEQUENCE == 1

template <typename T, T N>
using make_integer_sequence = std::make_integer_sequence<T, N>;

#endif // BOOST_PFR_USE_STD_MAKE_INTEGRAL_SEQUENCE == 1

template <std::size_t N>
using make_index_sequence = make_integer_sequence<std::size_t, N>;

template <typename... T>
using index_sequence_for = make_index_sequence<sizeof...(T)>;

}}} // namespace boost::pfr::detail

#endif


#include <utility>      // metaprogramming stuff
#include <cstddef>      // std::size_t

///////////////////// Tuple that holds its values in the supplied order
namespace boost { namespace pfr { namespace detail { namespace sequence_tuple {

template <std::size_t N, class T>
struct base_from_member {
    T value;
};

template <class I, class ...Tail>
struct tuple_base;



template <std::size_t... I, class ...Tail>
struct tuple_base< std::index_sequence<I...>, Tail... >
    : base_from_member<I , Tail>...
{
    static constexpr std::size_t size_v = sizeof...(I);

    // We do not use `noexcept` in the following functions, because if user forget to put one then clang will issue an error:
    // "error: exception specification of explicitly defaulted default constructor does not match the calculated one".
    constexpr tuple_base() = default;
    constexpr tuple_base(tuple_base&&) = default;
    constexpr tuple_base(const tuple_base&) = default;

    constexpr tuple_base(Tail... v) noexcept
        : base_from_member<I, Tail>{ v }...
    {}
};

template <>
struct tuple_base<std::index_sequence<> > {
    static constexpr std::size_t size_v = 0;
};

template <std::size_t N, class T>
constexpr T& get_impl(base_from_member<N, T>& t) noexcept {
    // NOLINTNEXTLINE(clang-analyzer-core.uninitialized.UndefReturn)
    return t.value;
}

template <std::size_t N, class T>
constexpr const T& get_impl(const base_from_member<N, T>& t) noexcept {
    // NOLINTNEXTLINE(clang-analyzer-core.uninitialized.UndefReturn)
    return t.value;
}

template <std::size_t N, class T>
constexpr volatile T& get_impl(volatile base_from_member<N, T>& t) noexcept {
    // NOLINTNEXTLINE(clang-analyzer-core.uninitialized.UndefReturn)
    return t.value;
}

template <std::size_t N, class T>
constexpr const volatile T& get_impl(const volatile base_from_member<N, T>& t) noexcept {
    // NOLINTNEXTLINE(clang-analyzer-core.uninitialized.UndefReturn)
    return t.value;
}

template <std::size_t N, class T>
constexpr T&& get_impl(base_from_member<N, T>&& t) noexcept {
    // NOLINTNEXTLINE(clang-analyzer-core.uninitialized.UndefReturn)
    return std::forward<T>(t.value);
}


template <class T, std::size_t N>
constexpr T& get_by_type_impl(base_from_member<N, T>& t) noexcept {
    // NOLINTNEXTLINE(clang-analyzer-core.uninitialized.UndefReturn)
    return t.value;
}

template <class T, std::size_t N>
constexpr const T& get_by_type_impl(const base_from_member<N, T>& t) noexcept {
    // NOLINTNEXTLINE(clang-analyzer-core.uninitialized.UndefReturn)
    return t.value;
}

template <class T, std::size_t N>
constexpr volatile T& get_by_type_impl(volatile base_from_member<N, T>& t) noexcept {
    // NOLINTNEXTLINE(clang-analyzer-core.uninitialized.UndefReturn)
    return t.value;
}

template <class T, std::size_t N>
constexpr const volatile T& get_by_type_impl(const volatile base_from_member<N, T>& t) noexcept {
    // NOLINTNEXTLINE(clang-analyzer-core.uninitialized.UndefReturn)
    return t.value;
}

template <class T, std::size_t N>
constexpr T&& get_by_type_impl(base_from_member<N, T>&& t) noexcept {
    // NOLINTNEXTLINE(clang-analyzer-core.uninitialized.UndefReturn)
    return std::forward<T>(t.value);
}

template <class T, std::size_t N>
constexpr const T&& get_by_type_impl(const base_from_member<N, T>&& t) noexcept {
    // NOLINTNEXTLINE(clang-analyzer-core.uninitialized.UndefReturn)
    return std::forward<T>(t.value);
}




template <class ...Values>
struct tuple: tuple_base<
    detail::index_sequence_for<Values...>,
    Values...>
{
    using tuple_base<
        detail::index_sequence_for<Values...>,
        Values...
    >::tuple_base;

    constexpr static std::size_t size() noexcept { return sizeof...(Values); }
    constexpr static bool empty() noexcept { return size() == 0; }
};


template <std::size_t N, class ...T>
constexpr decltype(auto) get(tuple<T...>& t) noexcept {
    static_assert(N < tuple<T...>::size_v, "====================> Boost.PFR: Tuple index out of bounds");
    return sequence_tuple::get_impl<N>(t);
}

template <std::size_t N, class ...T>
constexpr decltype(auto) get(const tuple<T...>& t) noexcept {
    static_assert(N < tuple<T...>::size_v, "====================> Boost.PFR: Tuple index out of bounds");
    return sequence_tuple::get_impl<N>(t);
}

template <std::size_t N, class ...T>
constexpr decltype(auto) get(const volatile tuple<T...>& t) noexcept {
    static_assert(N < tuple<T...>::size_v, "====================> Boost.PFR: Tuple index out of bounds");
    return sequence_tuple::get_impl<N>(t);
}

template <std::size_t N, class ...T>
constexpr decltype(auto) get(volatile tuple<T...>& t) noexcept {
    static_assert(N < tuple<T...>::size_v, "====================> Boost.PFR: Tuple index out of bounds");
    return sequence_tuple::get_impl<N>(t);
}

template <std::size_t N, class ...T>
constexpr decltype(auto) get(tuple<T...>&& t) noexcept {
    static_assert(N < tuple<T...>::size_v, "====================> Boost.PFR: Tuple index out of bounds");
    return sequence_tuple::get_impl<N>(std::move(t));
}

template <std::size_t I, class T>
using tuple_element = std::remove_reference< decltype(
        ::boost::pfr::detail::sequence_tuple::get<I>( std::declval<T>() )
    ) >;

template <class... Args>
constexpr auto make_sequence_tuple(Args... args) noexcept {
    return ::boost::pfr::detail::sequence_tuple::tuple<Args...>{ args... };
}

}}}} // namespace boost::pfr::detail::sequence_tuple

#endif // BOOST_PFR_CORE_HPP
// Copyright (c) 2016-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_DETAIL_SIZE_T_HPP
#define BOOST_PFR_DETAIL_SIZE_T_HPP

namespace boost { namespace pfr { namespace detail {

///////////////////// General utility stuff
template <std::size_t Index>
using size_t_ = std::integral_constant<std::size_t, Index >;

}}} // namespace boost::pfr::detail

#endif // BOOST_PFR_DETAIL_SIZE_T_HPP
#include <type_traits> // for std::conditional_t, std::is_reference

namespace boost { namespace pfr { namespace detail {

template <class... Args>
constexpr auto make_tuple_of_references(Args&&... args) noexcept {
  return sequence_tuple::tuple<Args&...>{ args... };
}

template<typename T, typename Arg>
constexpr decltype(auto) add_cv_like(Arg& arg) noexcept {
    if constexpr (std::is_const<T>::value && std::is_volatile<T>::value) {
        return const_cast<const volatile Arg&>(arg);
    }
    else if constexpr (std::is_const<T>::value) {
        return const_cast<const Arg&>(arg);
    }
    else if constexpr (std::is_volatile<T>::value) {
        return const_cast<volatile Arg&>(arg);
    }
    else {
        return const_cast<Arg&>(arg);
    }
}

// https://gcc.gnu.org/bugzilla/show_bug.cgi?id=78939
template<typename T, typename Sb, typename Arg>
constexpr decltype(auto) workaround_cast(Arg& arg) noexcept {
    using output_arg_t = std::conditional_t<!std::is_reference<Sb>(), decltype(detail::add_cv_like<T>(arg)), Sb>;
    return const_cast<output_arg_t>(arg);
}

template <class T>
constexpr auto tie_as_tuple(T& /*val*/, size_t_<0>) noexcept {
  return sequence_tuple::tuple<>{};
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<1>, std::enable_if_t<std::is_class< std::remove_cv_t<T> >::value>* = nullptr) noexcept {
  auto& [a] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.
  return ::boost::pfr::detail::make_tuple_of_references(detail::workaround_cast<T, decltype(a)>(a));
}


template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<1>, std::enable_if_t<!std::is_class< std::remove_cv_t<T> >::value>* = nullptr) noexcept {
  return ::boost::pfr::detail::make_tuple_of_references( val );
}


template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<2>) noexcept {
  auto& [a,b] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.
  return ::boost::pfr::detail::make_tuple_of_references(detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b));
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<3>) noexcept {
  auto& [a,b,c] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<4>) noexcept {
  auto& [a,b,c,d] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<5>) noexcept {
  auto& [a,b,c,d,e] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<6>) noexcept {
  auto& [a,b,c,d,e,f] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<7>) noexcept {
  auto& [a,b,c,d,e,f,g] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<8>) noexcept {
  auto& [a,b,c,d,e,f,g,h] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<9>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<10>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<11>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<12>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<13>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<14>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<15>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<16>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<17>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<18>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<19>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<20>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<21>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<22>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<23>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<24>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<25>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<26>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<27>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<28>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<29>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<30>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<31>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<32>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<33>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<34>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<35>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<36>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<37>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<38>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<39>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<40>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<41>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<42>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<43>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<44>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<45>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<46>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<47>) noexcept {
  auto& [a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<48>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<49>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<50>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<51>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<52>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<53>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<54>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<55>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<56>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<57>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<58>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<59>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<60>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<61>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<62>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<63>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<64>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<65>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<66>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<67>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<68>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<69>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<70>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<71>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<72>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<73>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<74>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<75>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<76>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD,aE
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD),
    detail::workaround_cast<T, decltype(aE)>(aE)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<77>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD,aE,aF
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD),
    detail::workaround_cast<T, decltype(aE)>(aE),detail::workaround_cast<T, decltype(aF)>(aF)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<78>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD,aE,aF,aG
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD),
    detail::workaround_cast<T, decltype(aE)>(aE),detail::workaround_cast<T, decltype(aF)>(aF),detail::workaround_cast<T, decltype(aG)>(aG)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<79>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD,aE,aF,aG,aH
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD),
    detail::workaround_cast<T, decltype(aE)>(aE),detail::workaround_cast<T, decltype(aF)>(aF),detail::workaround_cast<T, decltype(aG)>(aG),
    detail::workaround_cast<T, decltype(aH)>(aH)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<80>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD,aE,aF,aG,aH,aJ
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD),
    detail::workaround_cast<T, decltype(aE)>(aE),detail::workaround_cast<T, decltype(aF)>(aF),detail::workaround_cast<T, decltype(aG)>(aG),
    detail::workaround_cast<T, decltype(aH)>(aH),detail::workaround_cast<T, decltype(aJ)>(aJ)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<81>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD,aE,aF,aG,aH,aJ,aK
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD),
    detail::workaround_cast<T, decltype(aE)>(aE),detail::workaround_cast<T, decltype(aF)>(aF),detail::workaround_cast<T, decltype(aG)>(aG),
    detail::workaround_cast<T, decltype(aH)>(aH),detail::workaround_cast<T, decltype(aJ)>(aJ),detail::workaround_cast<T, decltype(aK)>(aK)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<82>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD,aE,aF,aG,aH,aJ,aK,aL
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD),
    detail::workaround_cast<T, decltype(aE)>(aE),detail::workaround_cast<T, decltype(aF)>(aF),detail::workaround_cast<T, decltype(aG)>(aG),
    detail::workaround_cast<T, decltype(aH)>(aH),detail::workaround_cast<T, decltype(aJ)>(aJ),detail::workaround_cast<T, decltype(aK)>(aK),
    detail::workaround_cast<T, decltype(aL)>(aL)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<83>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD,aE,aF,aG,aH,aJ,aK,aL,aM
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD),
    detail::workaround_cast<T, decltype(aE)>(aE),detail::workaround_cast<T, decltype(aF)>(aF),detail::workaround_cast<T, decltype(aG)>(aG),
    detail::workaround_cast<T, decltype(aH)>(aH),detail::workaround_cast<T, decltype(aJ)>(aJ),detail::workaround_cast<T, decltype(aK)>(aK),
    detail::workaround_cast<T, decltype(aL)>(aL),detail::workaround_cast<T, decltype(aM)>(aM)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<84>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD,aE,aF,aG,aH,aJ,aK,aL,aM,aN
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD),
    detail::workaround_cast<T, decltype(aE)>(aE),detail::workaround_cast<T, decltype(aF)>(aF),detail::workaround_cast<T, decltype(aG)>(aG),
    detail::workaround_cast<T, decltype(aH)>(aH),detail::workaround_cast<T, decltype(aJ)>(aJ),detail::workaround_cast<T, decltype(aK)>(aK),
    detail::workaround_cast<T, decltype(aL)>(aL),detail::workaround_cast<T, decltype(aM)>(aM),detail::workaround_cast<T, decltype(aN)>(aN)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<85>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD,aE,aF,aG,aH,aJ,aK,aL,aM,aN,aP
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD),
    detail::workaround_cast<T, decltype(aE)>(aE),detail::workaround_cast<T, decltype(aF)>(aF),detail::workaround_cast<T, decltype(aG)>(aG),
    detail::workaround_cast<T, decltype(aH)>(aH),detail::workaround_cast<T, decltype(aJ)>(aJ),detail::workaround_cast<T, decltype(aK)>(aK),
    detail::workaround_cast<T, decltype(aL)>(aL),detail::workaround_cast<T, decltype(aM)>(aM),detail::workaround_cast<T, decltype(aN)>(aN),
    detail::workaround_cast<T, decltype(aP)>(aP)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<86>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD,aE,aF,aG,aH,aJ,aK,aL,aM,aN,aP,aQ
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD),
    detail::workaround_cast<T, decltype(aE)>(aE),detail::workaround_cast<T, decltype(aF)>(aF),detail::workaround_cast<T, decltype(aG)>(aG),
    detail::workaround_cast<T, decltype(aH)>(aH),detail::workaround_cast<T, decltype(aJ)>(aJ),detail::workaround_cast<T, decltype(aK)>(aK),
    detail::workaround_cast<T, decltype(aL)>(aL),detail::workaround_cast<T, decltype(aM)>(aM),detail::workaround_cast<T, decltype(aN)>(aN),
    detail::workaround_cast<T, decltype(aP)>(aP),detail::workaround_cast<T, decltype(aQ)>(aQ)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<87>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD,aE,aF,aG,aH,aJ,aK,aL,aM,aN,aP,aQ,aR
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD),
    detail::workaround_cast<T, decltype(aE)>(aE),detail::workaround_cast<T, decltype(aF)>(aF),detail::workaround_cast<T, decltype(aG)>(aG),
    detail::workaround_cast<T, decltype(aH)>(aH),detail::workaround_cast<T, decltype(aJ)>(aJ),detail::workaround_cast<T, decltype(aK)>(aK),
    detail::workaround_cast<T, decltype(aL)>(aL),detail::workaround_cast<T, decltype(aM)>(aM),detail::workaround_cast<T, decltype(aN)>(aN),
    detail::workaround_cast<T, decltype(aP)>(aP),detail::workaround_cast<T, decltype(aQ)>(aQ),detail::workaround_cast<T, decltype(aR)>(aR)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<88>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD,aE,aF,aG,aH,aJ,aK,aL,aM,aN,aP,aQ,aR,aS
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD),
    detail::workaround_cast<T, decltype(aE)>(aE),detail::workaround_cast<T, decltype(aF)>(aF),detail::workaround_cast<T, decltype(aG)>(aG),
    detail::workaround_cast<T, decltype(aH)>(aH),detail::workaround_cast<T, decltype(aJ)>(aJ),detail::workaround_cast<T, decltype(aK)>(aK),
    detail::workaround_cast<T, decltype(aL)>(aL),detail::workaround_cast<T, decltype(aM)>(aM),detail::workaround_cast<T, decltype(aN)>(aN),
    detail::workaround_cast<T, decltype(aP)>(aP),detail::workaround_cast<T, decltype(aQ)>(aQ),detail::workaround_cast<T, decltype(aR)>(aR),
    detail::workaround_cast<T, decltype(aS)>(aS)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<89>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD,aE,aF,aG,aH,aJ,aK,aL,aM,aN,aP,aQ,aR,aS,aU
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD),
    detail::workaround_cast<T, decltype(aE)>(aE),detail::workaround_cast<T, decltype(aF)>(aF),detail::workaround_cast<T, decltype(aG)>(aG),
    detail::workaround_cast<T, decltype(aH)>(aH),detail::workaround_cast<T, decltype(aJ)>(aJ),detail::workaround_cast<T, decltype(aK)>(aK),
    detail::workaround_cast<T, decltype(aL)>(aL),detail::workaround_cast<T, decltype(aM)>(aM),detail::workaround_cast<T, decltype(aN)>(aN),
    detail::workaround_cast<T, decltype(aP)>(aP),detail::workaround_cast<T, decltype(aQ)>(aQ),detail::workaround_cast<T, decltype(aR)>(aR),
    detail::workaround_cast<T, decltype(aS)>(aS),detail::workaround_cast<T, decltype(aU)>(aU)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<90>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD,aE,aF,aG,aH,aJ,aK,aL,aM,aN,aP,aQ,aR,aS,aU,aV
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD),
    detail::workaround_cast<T, decltype(aE)>(aE),detail::workaround_cast<T, decltype(aF)>(aF),detail::workaround_cast<T, decltype(aG)>(aG),
    detail::workaround_cast<T, decltype(aH)>(aH),detail::workaround_cast<T, decltype(aJ)>(aJ),detail::workaround_cast<T, decltype(aK)>(aK),
    detail::workaround_cast<T, decltype(aL)>(aL),detail::workaround_cast<T, decltype(aM)>(aM),detail::workaround_cast<T, decltype(aN)>(aN),
    detail::workaround_cast<T, decltype(aP)>(aP),detail::workaround_cast<T, decltype(aQ)>(aQ),detail::workaround_cast<T, decltype(aR)>(aR),
    detail::workaround_cast<T, decltype(aS)>(aS),detail::workaround_cast<T, decltype(aU)>(aU),detail::workaround_cast<T, decltype(aV)>(aV)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<91>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD,aE,aF,aG,aH,aJ,aK,aL,aM,aN,aP,aQ,aR,aS,aU,aV,aW
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD),
    detail::workaround_cast<T, decltype(aE)>(aE),detail::workaround_cast<T, decltype(aF)>(aF),detail::workaround_cast<T, decltype(aG)>(aG),
    detail::workaround_cast<T, decltype(aH)>(aH),detail::workaround_cast<T, decltype(aJ)>(aJ),detail::workaround_cast<T, decltype(aK)>(aK),
    detail::workaround_cast<T, decltype(aL)>(aL),detail::workaround_cast<T, decltype(aM)>(aM),detail::workaround_cast<T, decltype(aN)>(aN),
    detail::workaround_cast<T, decltype(aP)>(aP),detail::workaround_cast<T, decltype(aQ)>(aQ),detail::workaround_cast<T, decltype(aR)>(aR),
    detail::workaround_cast<T, decltype(aS)>(aS),detail::workaround_cast<T, decltype(aU)>(aU),detail::workaround_cast<T, decltype(aV)>(aV),
    detail::workaround_cast<T, decltype(aW)>(aW)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<92>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD,aE,aF,aG,aH,aJ,aK,aL,aM,aN,aP,aQ,aR,aS,aU,aV,aW,aX
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD),
    detail::workaround_cast<T, decltype(aE)>(aE),detail::workaround_cast<T, decltype(aF)>(aF),detail::workaround_cast<T, decltype(aG)>(aG),
    detail::workaround_cast<T, decltype(aH)>(aH),detail::workaround_cast<T, decltype(aJ)>(aJ),detail::workaround_cast<T, decltype(aK)>(aK),
    detail::workaround_cast<T, decltype(aL)>(aL),detail::workaround_cast<T, decltype(aM)>(aM),detail::workaround_cast<T, decltype(aN)>(aN),
    detail::workaround_cast<T, decltype(aP)>(aP),detail::workaround_cast<T, decltype(aQ)>(aQ),detail::workaround_cast<T, decltype(aR)>(aR),
    detail::workaround_cast<T, decltype(aS)>(aS),detail::workaround_cast<T, decltype(aU)>(aU),detail::workaround_cast<T, decltype(aV)>(aV),
    detail::workaround_cast<T, decltype(aW)>(aW),detail::workaround_cast<T, decltype(aX)>(aX)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<93>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD,aE,aF,aG,aH,aJ,aK,aL,aM,aN,aP,aQ,aR,aS,aU,aV,aW,aX,aY
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD),
    detail::workaround_cast<T, decltype(aE)>(aE),detail::workaround_cast<T, decltype(aF)>(aF),detail::workaround_cast<T, decltype(aG)>(aG),
    detail::workaround_cast<T, decltype(aH)>(aH),detail::workaround_cast<T, decltype(aJ)>(aJ),detail::workaround_cast<T, decltype(aK)>(aK),
    detail::workaround_cast<T, decltype(aL)>(aL),detail::workaround_cast<T, decltype(aM)>(aM),detail::workaround_cast<T, decltype(aN)>(aN),
    detail::workaround_cast<T, decltype(aP)>(aP),detail::workaround_cast<T, decltype(aQ)>(aQ),detail::workaround_cast<T, decltype(aR)>(aR),
    detail::workaround_cast<T, decltype(aS)>(aS),detail::workaround_cast<T, decltype(aU)>(aU),detail::workaround_cast<T, decltype(aV)>(aV),
    detail::workaround_cast<T, decltype(aW)>(aW),detail::workaround_cast<T, decltype(aX)>(aX),detail::workaround_cast<T, decltype(aY)>(aY)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<94>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD,aE,aF,aG,aH,aJ,aK,aL,aM,aN,aP,aQ,aR,aS,aU,aV,aW,aX,aY,aZ
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD),
    detail::workaround_cast<T, decltype(aE)>(aE),detail::workaround_cast<T, decltype(aF)>(aF),detail::workaround_cast<T, decltype(aG)>(aG),
    detail::workaround_cast<T, decltype(aH)>(aH),detail::workaround_cast<T, decltype(aJ)>(aJ),detail::workaround_cast<T, decltype(aK)>(aK),
    detail::workaround_cast<T, decltype(aL)>(aL),detail::workaround_cast<T, decltype(aM)>(aM),detail::workaround_cast<T, decltype(aN)>(aN),
    detail::workaround_cast<T, decltype(aP)>(aP),detail::workaround_cast<T, decltype(aQ)>(aQ),detail::workaround_cast<T, decltype(aR)>(aR),
    detail::workaround_cast<T, decltype(aS)>(aS),detail::workaround_cast<T, decltype(aU)>(aU),detail::workaround_cast<T, decltype(aV)>(aV),
    detail::workaround_cast<T, decltype(aW)>(aW),detail::workaround_cast<T, decltype(aX)>(aX),detail::workaround_cast<T, decltype(aY)>(aY),
    detail::workaround_cast<T, decltype(aZ)>(aZ)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<95>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD,aE,aF,aG,aH,aJ,aK,aL,aM,aN,aP,aQ,aR,aS,aU,aV,aW,aX,aY,aZ,
    ba
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD),
    detail::workaround_cast<T, decltype(aE)>(aE),detail::workaround_cast<T, decltype(aF)>(aF),detail::workaround_cast<T, decltype(aG)>(aG),
    detail::workaround_cast<T, decltype(aH)>(aH),detail::workaround_cast<T, decltype(aJ)>(aJ),detail::workaround_cast<T, decltype(aK)>(aK),
    detail::workaround_cast<T, decltype(aL)>(aL),detail::workaround_cast<T, decltype(aM)>(aM),detail::workaround_cast<T, decltype(aN)>(aN),
    detail::workaround_cast<T, decltype(aP)>(aP),detail::workaround_cast<T, decltype(aQ)>(aQ),detail::workaround_cast<T, decltype(aR)>(aR),
    detail::workaround_cast<T, decltype(aS)>(aS),detail::workaround_cast<T, decltype(aU)>(aU),detail::workaround_cast<T, decltype(aV)>(aV),
    detail::workaround_cast<T, decltype(aW)>(aW),detail::workaround_cast<T, decltype(aX)>(aX),detail::workaround_cast<T, decltype(aY)>(aY),
    detail::workaround_cast<T, decltype(aZ)>(aZ),detail::workaround_cast<T, decltype(ba)>(ba)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<96>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD,aE,aF,aG,aH,aJ,aK,aL,aM,aN,aP,aQ,aR,aS,aU,aV,aW,aX,aY,aZ,
    ba,bb
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD),
    detail::workaround_cast<T, decltype(aE)>(aE),detail::workaround_cast<T, decltype(aF)>(aF),detail::workaround_cast<T, decltype(aG)>(aG),
    detail::workaround_cast<T, decltype(aH)>(aH),detail::workaround_cast<T, decltype(aJ)>(aJ),detail::workaround_cast<T, decltype(aK)>(aK),
    detail::workaround_cast<T, decltype(aL)>(aL),detail::workaround_cast<T, decltype(aM)>(aM),detail::workaround_cast<T, decltype(aN)>(aN),
    detail::workaround_cast<T, decltype(aP)>(aP),detail::workaround_cast<T, decltype(aQ)>(aQ),detail::workaround_cast<T, decltype(aR)>(aR),
    detail::workaround_cast<T, decltype(aS)>(aS),detail::workaround_cast<T, decltype(aU)>(aU),detail::workaround_cast<T, decltype(aV)>(aV),
    detail::workaround_cast<T, decltype(aW)>(aW),detail::workaround_cast<T, decltype(aX)>(aX),detail::workaround_cast<T, decltype(aY)>(aY),
    detail::workaround_cast<T, decltype(aZ)>(aZ),detail::workaround_cast<T, decltype(ba)>(ba),detail::workaround_cast<T, decltype(bb)>(bb)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<97>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD,aE,aF,aG,aH,aJ,aK,aL,aM,aN,aP,aQ,aR,aS,aU,aV,aW,aX,aY,aZ,
    ba,bb,bc
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD),
    detail::workaround_cast<T, decltype(aE)>(aE),detail::workaround_cast<T, decltype(aF)>(aF),detail::workaround_cast<T, decltype(aG)>(aG),
    detail::workaround_cast<T, decltype(aH)>(aH),detail::workaround_cast<T, decltype(aJ)>(aJ),detail::workaround_cast<T, decltype(aK)>(aK),
    detail::workaround_cast<T, decltype(aL)>(aL),detail::workaround_cast<T, decltype(aM)>(aM),detail::workaround_cast<T, decltype(aN)>(aN),
    detail::workaround_cast<T, decltype(aP)>(aP),detail::workaround_cast<T, decltype(aQ)>(aQ),detail::workaround_cast<T, decltype(aR)>(aR),
    detail::workaround_cast<T, decltype(aS)>(aS),detail::workaround_cast<T, decltype(aU)>(aU),detail::workaround_cast<T, decltype(aV)>(aV),
    detail::workaround_cast<T, decltype(aW)>(aW),detail::workaround_cast<T, decltype(aX)>(aX),detail::workaround_cast<T, decltype(aY)>(aY),
    detail::workaround_cast<T, decltype(aZ)>(aZ),detail::workaround_cast<T, decltype(ba)>(ba),detail::workaround_cast<T, decltype(bb)>(bb),
    detail::workaround_cast<T, decltype(bc)>(bc)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<98>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD,aE,aF,aG,aH,aJ,aK,aL,aM,aN,aP,aQ,aR,aS,aU,aV,aW,aX,aY,aZ,
    ba,bb,bc,bd
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD),
    detail::workaround_cast<T, decltype(aE)>(aE),detail::workaround_cast<T, decltype(aF)>(aF),detail::workaround_cast<T, decltype(aG)>(aG),
    detail::workaround_cast<T, decltype(aH)>(aH),detail::workaround_cast<T, decltype(aJ)>(aJ),detail::workaround_cast<T, decltype(aK)>(aK),
    detail::workaround_cast<T, decltype(aL)>(aL),detail::workaround_cast<T, decltype(aM)>(aM),detail::workaround_cast<T, decltype(aN)>(aN),
    detail::workaround_cast<T, decltype(aP)>(aP),detail::workaround_cast<T, decltype(aQ)>(aQ),detail::workaround_cast<T, decltype(aR)>(aR),
    detail::workaround_cast<T, decltype(aS)>(aS),detail::workaround_cast<T, decltype(aU)>(aU),detail::workaround_cast<T, decltype(aV)>(aV),
    detail::workaround_cast<T, decltype(aW)>(aW),detail::workaround_cast<T, decltype(aX)>(aX),detail::workaround_cast<T, decltype(aY)>(aY),
    detail::workaround_cast<T, decltype(aZ)>(aZ),detail::workaround_cast<T, decltype(ba)>(ba),detail::workaround_cast<T, decltype(bb)>(bb),
    detail::workaround_cast<T, decltype(bc)>(bc),detail::workaround_cast<T, decltype(bd)>(bd)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<99>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD,aE,aF,aG,aH,aJ,aK,aL,aM,aN,aP,aQ,aR,aS,aU,aV,aW,aX,aY,aZ,
    ba,bb,bc,bd,be
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD),
    detail::workaround_cast<T, decltype(aE)>(aE),detail::workaround_cast<T, decltype(aF)>(aF),detail::workaround_cast<T, decltype(aG)>(aG),
    detail::workaround_cast<T, decltype(aH)>(aH),detail::workaround_cast<T, decltype(aJ)>(aJ),detail::workaround_cast<T, decltype(aK)>(aK),
    detail::workaround_cast<T, decltype(aL)>(aL),detail::workaround_cast<T, decltype(aM)>(aM),detail::workaround_cast<T, decltype(aN)>(aN),
    detail::workaround_cast<T, decltype(aP)>(aP),detail::workaround_cast<T, decltype(aQ)>(aQ),detail::workaround_cast<T, decltype(aR)>(aR),
    detail::workaround_cast<T, decltype(aS)>(aS),detail::workaround_cast<T, decltype(aU)>(aU),detail::workaround_cast<T, decltype(aV)>(aV),
    detail::workaround_cast<T, decltype(aW)>(aW),detail::workaround_cast<T, decltype(aX)>(aX),detail::workaround_cast<T, decltype(aY)>(aY),
    detail::workaround_cast<T, decltype(aZ)>(aZ),detail::workaround_cast<T, decltype(ba)>(ba),detail::workaround_cast<T, decltype(bb)>(bb),
    detail::workaround_cast<T, decltype(bc)>(bc),detail::workaround_cast<T, decltype(bd)>(bd),detail::workaround_cast<T, decltype(be)>(be)
  );
}

template <class T>
constexpr auto tie_as_tuple(T& val, size_t_<100>) noexcept {
  auto& [
    a,b,c,d,e,f,g,h,j,k,l,m,n,p,q,r,s,t,u,v,w,x,y,z,A,B,C,D,E,F,G,H,J,K,L,M,N,P,Q,R,S,U,V,W,X,Y,Z,
    aa,ab,ac,ad,ae,af,ag,ah,aj,ak,al,am,an,ap,aq,ar,as,at,au,av,aw,ax,ay,az,aA,aB,aC,aD,aE,aF,aG,aH,aJ,aK,aL,aM,aN,aP,aQ,aR,aS,aU,aV,aW,aX,aY,aZ,
    ba,bb,bc,bd,be,bf
  ] = const_cast<std::remove_cv_t<T>&>(val); // ====================> Boost.PFR: User-provided type is not a SimpleAggregate.

  return ::boost::pfr::detail::make_tuple_of_references(
    detail::workaround_cast<T, decltype(a)>(a),detail::workaround_cast<T, decltype(b)>(b),detail::workaround_cast<T, decltype(c)>(c),
    detail::workaround_cast<T, decltype(d)>(d),detail::workaround_cast<T, decltype(e)>(e),detail::workaround_cast<T, decltype(f)>(f),
    detail::workaround_cast<T, decltype(g)>(g),detail::workaround_cast<T, decltype(h)>(h),detail::workaround_cast<T, decltype(j)>(j),
    detail::workaround_cast<T, decltype(k)>(k),detail::workaround_cast<T, decltype(l)>(l),detail::workaround_cast<T, decltype(m)>(m),
    detail::workaround_cast<T, decltype(n)>(n),detail::workaround_cast<T, decltype(p)>(p),detail::workaround_cast<T, decltype(q)>(q),
    detail::workaround_cast<T, decltype(r)>(r),detail::workaround_cast<T, decltype(s)>(s),detail::workaround_cast<T, decltype(t)>(t),
    detail::workaround_cast<T, decltype(u)>(u),detail::workaround_cast<T, decltype(v)>(v),detail::workaround_cast<T, decltype(w)>(w),
    detail::workaround_cast<T, decltype(x)>(x),detail::workaround_cast<T, decltype(y)>(y),detail::workaround_cast<T, decltype(z)>(z),
    detail::workaround_cast<T, decltype(A)>(A),detail::workaround_cast<T, decltype(B)>(B),detail::workaround_cast<T, decltype(C)>(C),
    detail::workaround_cast<T, decltype(D)>(D),detail::workaround_cast<T, decltype(E)>(E),detail::workaround_cast<T, decltype(F)>(F),
    detail::workaround_cast<T, decltype(G)>(G),detail::workaround_cast<T, decltype(H)>(H),detail::workaround_cast<T, decltype(J)>(J),
    detail::workaround_cast<T, decltype(K)>(K),detail::workaround_cast<T, decltype(L)>(L),detail::workaround_cast<T, decltype(M)>(M),
    detail::workaround_cast<T, decltype(N)>(N),detail::workaround_cast<T, decltype(P)>(P),detail::workaround_cast<T, decltype(Q)>(Q),
    detail::workaround_cast<T, decltype(R)>(R),detail::workaround_cast<T, decltype(S)>(S),detail::workaround_cast<T, decltype(U)>(U),
    detail::workaround_cast<T, decltype(V)>(V),detail::workaround_cast<T, decltype(W)>(W),detail::workaround_cast<T, decltype(X)>(X),
    detail::workaround_cast<T, decltype(Y)>(Y),detail::workaround_cast<T, decltype(Z)>(Z),detail::workaround_cast<T, decltype(aa)>(aa),
    detail::workaround_cast<T, decltype(ab)>(ab),detail::workaround_cast<T, decltype(ac)>(ac),detail::workaround_cast<T, decltype(ad)>(ad),
    detail::workaround_cast<T, decltype(ae)>(ae),detail::workaround_cast<T, decltype(af)>(af),detail::workaround_cast<T, decltype(ag)>(ag),
    detail::workaround_cast<T, decltype(ah)>(ah),detail::workaround_cast<T, decltype(aj)>(aj),detail::workaround_cast<T, decltype(ak)>(ak),
    detail::workaround_cast<T, decltype(al)>(al),detail::workaround_cast<T, decltype(am)>(am),detail::workaround_cast<T, decltype(an)>(an),
    detail::workaround_cast<T, decltype(ap)>(ap),detail::workaround_cast<T, decltype(aq)>(aq),detail::workaround_cast<T, decltype(ar)>(ar),
    detail::workaround_cast<T, decltype(as)>(as),detail::workaround_cast<T, decltype(at)>(at),detail::workaround_cast<T, decltype(au)>(au),
    detail::workaround_cast<T, decltype(av)>(av),detail::workaround_cast<T, decltype(aw)>(aw),detail::workaround_cast<T, decltype(ax)>(ax),
    detail::workaround_cast<T, decltype(ay)>(ay),detail::workaround_cast<T, decltype(az)>(az),detail::workaround_cast<T, decltype(aA)>(aA),
    detail::workaround_cast<T, decltype(aB)>(aB),detail::workaround_cast<T, decltype(aC)>(aC),detail::workaround_cast<T, decltype(aD)>(aD),
    detail::workaround_cast<T, decltype(aE)>(aE),detail::workaround_cast<T, decltype(aF)>(aF),detail::workaround_cast<T, decltype(aG)>(aG),
    detail::workaround_cast<T, decltype(aH)>(aH),detail::workaround_cast<T, decltype(aJ)>(aJ),detail::workaround_cast<T, decltype(aK)>(aK),
    detail::workaround_cast<T, decltype(aL)>(aL),detail::workaround_cast<T, decltype(aM)>(aM),detail::workaround_cast<T, decltype(aN)>(aN),
    detail::workaround_cast<T, decltype(aP)>(aP),detail::workaround_cast<T, decltype(aQ)>(aQ),detail::workaround_cast<T, decltype(aR)>(aR),
    detail::workaround_cast<T, decltype(aS)>(aS),detail::workaround_cast<T, decltype(aU)>(aU),detail::workaround_cast<T, decltype(aV)>(aV),
    detail::workaround_cast<T, decltype(aW)>(aW),detail::workaround_cast<T, decltype(aX)>(aX),detail::workaround_cast<T, decltype(aY)>(aY),
    detail::workaround_cast<T, decltype(aZ)>(aZ),detail::workaround_cast<T, decltype(ba)>(ba),detail::workaround_cast<T, decltype(bb)>(bb),
    detail::workaround_cast<T, decltype(bc)>(bc),detail::workaround_cast<T, decltype(bd)>(bd),detail::workaround_cast<T, decltype(be)>(be),
    detail::workaround_cast<T, decltype(bf)>(bf)
  );
}


template <class T, std::size_t I>
constexpr void tie_as_tuple(T& /*val*/, size_t_<I>) noexcept {
  static_assert(sizeof(T) && false,
                "====================> Boost.PFR: Too many fields in a structure T. Regenerate include/boost/pfr/detail/core17_generated.hpp file for appropriate count of fields. For example: `python ./misc/generate_cpp17.py 300 > include/boost/pfr/detail/core17_generated.hpp`");
}

}}} // namespace boost::pfr::detail

#endif // BOOST_PFR_DETAIL_CORE17_GENERATED_HPP

// Copyright (c) 2016-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_DETAIL_FIELDS_COUNT_HPP
#define BOOST_PFR_DETAIL_FIELDS_COUNT_HPP

// Copyright (c) 2019-2023 Antony Polukhin.
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_DETAIL_UNSAFE_DECLVAL_HPP
#define BOOST_PFR_DETAIL_UNSAFE_DECLVAL_HPP


#include <type_traits>

namespace boost { namespace pfr { namespace detail {

// This function serves as a link-time assert. If linker requires it, then
// `unsafe_declval()` is used at runtime.
void report_if_you_see_link_error_with_this_function() noexcept;

// For returning non default constructible types. Do NOT use at runtime!
//
// GCCs std::declval may not be used in potentionally evaluated contexts,
// so we reinvent it.
template <class T>
constexpr T unsafe_declval() noexcept {
    report_if_you_see_link_error_with_this_function();

    typename std::remove_reference<T>::type* ptr = nullptr;
    ptr += 42; // suppresses 'null pointer dereference' warnings
    return static_cast<T>(*ptr);
}

}}} // namespace boost::pfr::detail


#endif // BOOST_PFR_DETAIL_UNSAFE_DECLVAL_HPP


#include <climits>      // CHAR_BIT
#include <type_traits>
#include <utility>      // metaprogramming stuff

#ifdef __clang__
#   pragma clang diagnostic push
#   pragma clang diagnostic ignored "-Wmissing-braces"
#   pragma clang diagnostic ignored "-Wundefined-inline"
#   pragma clang diagnostic ignored "-Wundefined-internal"
#   pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace boost { namespace pfr { namespace detail {

///////////////////// Structure that can be converted to reference to anything
struct ubiq_lref_constructor {
    std::size_t ignore;
    template <class Type> constexpr operator Type&() const && noexcept {  // tweak for template_unconstrained.cpp like cases
        return detail::unsafe_declval<Type&>();
    }

    template <class Type> constexpr operator Type&() const & noexcept {  // tweak for optional_chrono.cpp like cases
        return detail::unsafe_declval<Type&>();
    }
};

///////////////////// Structure that can be converted to rvalue reference to anything
struct ubiq_rref_constructor {
    std::size_t ignore;
    template <class Type> /*constexpr*/ operator Type() const && noexcept {  // Allows initialization of rvalue reference fields and move-only types
        return detail::unsafe_declval<Type>();
    }
};


#ifndef __cpp_lib_is_aggregate
///////////////////// Hand-made is_aggregate_initializable_n<T> trait

// Structure that can be converted to reference to anything except reference to T
template <class T, bool IsCopyConstructible>
struct ubiq_constructor_except {
    std::size_t ignore;
    template <class Type> constexpr operator std::enable_if_t<!std::is_same<T, Type>::value, Type&> () const noexcept; // Undefined
};

template <class T>
struct ubiq_constructor_except<T, false> {
    std::size_t ignore;
    template <class Type> constexpr operator std::enable_if_t<!std::is_same<T, Type>::value, Type&&> () const noexcept; // Undefined
};


// `std::is_constructible<T, ubiq_constructor_except<T>>` consumes a lot of time, so we made a separate lazy trait for it.
template <std::size_t N, class T> struct is_single_field_and_aggregate_initializable: std::false_type {};
template <class T> struct is_single_field_and_aggregate_initializable<1, T>: std::integral_constant<
    bool, !std::is_constructible<T, ubiq_constructor_except<T, std::is_copy_constructible<T>::value>>::value
> {};

// Hand-made is_aggregate<T> trait:
// Before C++20 aggregates could be constructed from `decltype(ubiq_?ref_constructor{I})...` but type traits report that
// there's no constructor from `decltype(ubiq_?ref_constructor{I})...`
// Special case for N == 1: `std::is_constructible<T, ubiq_?ref_constructor>` returns true if N == 1 and T is copy/move constructible.
template <class T, std::size_t N>
struct is_aggregate_initializable_n {
    template <std::size_t ...I>
    static constexpr bool is_not_constructible_n(std::index_sequence<I...>) noexcept {
        return (!std::is_constructible<T, decltype(ubiq_lref_constructor{I})...>::value && !std::is_constructible<T, decltype(ubiq_rref_constructor{I})...>::value)
            || is_single_field_and_aggregate_initializable<N, T>::value
        ;
    }

    static constexpr bool value =
           std::is_empty<T>::value
        || std::is_array<T>::value
        || std::is_fundamental<T>::value
        || is_not_constructible_n(detail::make_index_sequence<N>{})
    ;
};

#endif // #ifndef __cpp_lib_is_aggregate

///////////////////// Detect aggregates with inheritance
template <class Derived, class U>
constexpr bool static_assert_non_inherited() noexcept {
    static_assert(
            !std::is_base_of<U, Derived>::value,
            "====================> Boost.PFR: Boost.PFR: Inherited types are not supported."
    );
    return true;
}

template <class Derived>
struct ubiq_lref_base_asserting {
    template <class Type> constexpr operator Type&() const &&  // tweak for template_unconstrained.cpp like cases
        noexcept(detail::static_assert_non_inherited<Derived, Type>())  // force the computation of assert function
    {
        return detail::unsafe_declval<Type&>();
    }

    template <class Type> constexpr operator Type&() const &  // tweak for optional_chrono.cpp like cases
        noexcept(detail::static_assert_non_inherited<Derived, Type>())  // force the computation of assert function
    {
        return detail::unsafe_declval<Type&>();
    }
};

template <class Derived>
struct ubiq_rref_base_asserting {
    template <class Type> /*constexpr*/ operator Type() const &&  // Allows initialization of rvalue reference fields and move-only types
        noexcept(detail::static_assert_non_inherited<Derived, Type>())  // force the computation of assert function
    {
        return detail::unsafe_declval<Type>();
    }
};

template <class T, std::size_t I0, std::size_t... I, class /*Enable*/ = typename std::enable_if<std::is_copy_constructible<T>::value>::type>
constexpr auto assert_first_not_base(std::index_sequence<I0, I...>) noexcept
    -> typename std::add_pointer<decltype(T{ ubiq_lref_base_asserting<T>{}, ubiq_lref_constructor{I}... })>::type
{
    return nullptr;
}

template <class T, std::size_t I0, std::size_t... I, class /*Enable*/ = typename std::enable_if<!std::is_copy_constructible<T>::value>::type>
constexpr auto assert_first_not_base(std::index_sequence<I0, I...>) noexcept
    -> typename std::add_pointer<decltype(T{ ubiq_rref_base_asserting<T>{}, ubiq_rref_constructor{I}... })>::type
{
    return nullptr;
}

template <class T>
constexpr void* assert_first_not_base(std::index_sequence<>) noexcept
{
    return nullptr;
}

///////////////////// Helper for SFINAE on fields count
template <class T, std::size_t... I, class /*Enable*/ = typename std::enable_if<std::is_copy_constructible<T>::value>::type>
constexpr auto enable_if_constructible_helper(std::index_sequence<I...>) noexcept
    -> typename std::add_pointer<decltype(T{ ubiq_lref_constructor{I}... })>::type;

template <class T, std::size_t... I, class /*Enable*/ = typename std::enable_if<!std::is_copy_constructible<T>::value>::type>
constexpr auto enable_if_constructible_helper(std::index_sequence<I...>) noexcept
    -> typename std::add_pointer<decltype(T{ ubiq_rref_constructor{I}... })>::type;

template <class T, std::size_t N, class /*Enable*/ = decltype( enable_if_constructible_helper<T>(detail::make_index_sequence<N>()) ) >
using enable_if_constructible_helper_t = std::size_t;

///////////////////// Helpers for range size detection
template <std::size_t Begin, std::size_t Last>
using is_one_element_range = std::integral_constant<bool, Begin == Last>;

using multi_element_range = std::false_type;
using one_element_range = std::true_type;

///////////////////// Non greedy fields count search. Templates instantiation depth is log(sizeof(T)), templates instantiation count is log(sizeof(T)).
template <class T, std::size_t Begin, std::size_t Middle>
constexpr std::size_t detect_fields_count(detail::one_element_range, long) noexcept {
    static_assert(
        Begin == Middle,
        "====================> Boost.PFR: Internal logic error."
    );
    return Begin;
}

template <class T, std::size_t Begin, std::size_t Middle>
constexpr std::size_t detect_fields_count(detail::multi_element_range, int) noexcept;

template <class T, std::size_t Begin, std::size_t Middle>
constexpr auto detect_fields_count(detail::multi_element_range, long) noexcept
    -> detail::enable_if_constructible_helper_t<T, Middle>
{
    constexpr std::size_t next_v = Middle + (Middle - Begin + 1) / 2;
    return detail::detect_fields_count<T, Middle, next_v>(detail::is_one_element_range<Middle, next_v>{}, 1L);
}

template <class T, std::size_t Begin, std::size_t Middle>
constexpr std::size_t detect_fields_count(detail::multi_element_range, int) noexcept {
    constexpr std::size_t next_v = Begin + (Middle - Begin) / 2;
    return detail::detect_fields_count<T, Begin, next_v>(detail::is_one_element_range<Begin, next_v>{}, 1L);
}

///////////////////// Greedy search. Templates instantiation depth is log(sizeof(T)), templates instantiation count is log(sizeof(T))*T in worst case.
template <class T, std::size_t N>
constexpr auto detect_fields_count_greedy_remember(long) noexcept
    -> detail::enable_if_constructible_helper_t<T, N>
{
    return N;
}

template <class T, std::size_t N>
constexpr std::size_t detect_fields_count_greedy_remember(int) noexcept {
    return 0;
}

template <class T, std::size_t Begin, std::size_t Last>
constexpr std::size_t detect_fields_count_greedy(detail::one_element_range) noexcept {
    static_assert(
        Begin == Last,
        "====================> Boost.PFR: Internal logic error."
    );
    return detail::detect_fields_count_greedy_remember<T, Begin>(1L);
}

template <class T, std::size_t Begin, std::size_t Last>
constexpr std::size_t detect_fields_count_greedy(detail::multi_element_range) noexcept {
    constexpr std::size_t middle = Begin + (Last - Begin) / 2;
    constexpr std::size_t fields_count_big_range = detail::detect_fields_count_greedy<T, middle + 1, Last>(
        detail::is_one_element_range<middle + 1, Last>{}
    );

    constexpr std::size_t small_range_begin = (fields_count_big_range ? 0 : Begin);
    constexpr std::size_t small_range_last = (fields_count_big_range ? 0 : middle);
    constexpr std::size_t fields_count_small_range = detail::detect_fields_count_greedy<T, small_range_begin, small_range_last>(
        detail::is_one_element_range<small_range_begin, small_range_last>{}
    );
    return fields_count_big_range ? fields_count_big_range : fields_count_small_range;
}

///////////////////// Choosing between array size, greedy and non greedy search.
template <class T, std::size_t N>
constexpr auto detect_fields_count_dispatch(size_t_<N>, long, long) noexcept
    -> typename std::enable_if<std::is_array<T>::value, std::size_t>::type
{
    return sizeof(T) / sizeof(typename std::remove_all_extents<T>::type);
}

template <class T, std::size_t N>
constexpr auto detect_fields_count_dispatch(size_t_<N>, long, int) noexcept
    -> decltype(sizeof(T{}))
{
    constexpr std::size_t middle = N / 2 + 1;
    return detail::detect_fields_count<T, 0, middle>(detail::multi_element_range{}, 1L);
}

template <class T, std::size_t N>
constexpr std::size_t detect_fields_count_dispatch(size_t_<N>, int, int) noexcept {
    // T is not default aggregate initialzable. It means that at least one of the members is not default constructible,
    // so we have to check all the aggregate initializations for T up to N parameters and return the bigest succeeded
    // (we can not use binary search for detecting fields count).
    return detail::detect_fields_count_greedy<T, 0, N>(detail::multi_element_range{});
}

///////////////////// Returns fields count
template <class T>
constexpr std::size_t fields_count() noexcept {
    using type = std::remove_cv_t<T>;

    static_assert(
        !std::is_reference<type>::value,
        "====================> Boost.PFR: Attempt to get fields count on a reference. This is not allowed because that could hide an issue and different library users expect different behavior in that case."
    );

#if !BOOST_PFR_HAS_GUARANTEED_COPY_ELISION
    static_assert(
        std::is_copy_constructible<std::remove_all_extents_t<type>>::value || (
            std::is_move_constructible<std::remove_all_extents_t<type>>::value
            && std::is_move_assignable<std::remove_all_extents_t<type>>::value
        ),
        "====================> Boost.PFR: Type and each field in the type must be copy constructible (or move constructible and move assignable)."
    );
#endif  // #if !BOOST_PFR_HAS_GUARANTEED_COPY_ELISION

    static_assert(
        !std::is_polymorphic<type>::value,
        "====================> Boost.PFR: Type must have no virtual function, because otherwise it is not aggregate initializable."
    );

#ifdef __cpp_lib_is_aggregate
    static_assert(
        std::is_aggregate<type>::value             // Does not return `true` for built-in types.
        || std::is_scalar<type>::value,
        "====================> Boost.PFR: Type must be aggregate initializable."
    );
#endif

// Can't use the following. See the non_std_layout.cpp test.
//#if !BOOST_PFR_USE_CPP17
//    static_assert(
//        std::is_standard_layout<type>::value,   // Does not return `true` for structs that have non standard layout members.
//        "Type must be aggregate initializable."
//    );
//#endif

#if defined(_MSC_VER) && (_MSC_VER <= 1920)
    // Workaround for msvc compilers. Versions <= 1920 have a limit of max 1024 elements in template parameter pack
    constexpr std::size_t max_fields_count = (sizeof(type) * CHAR_BIT >= 1024 ? 1024 : sizeof(type) * CHAR_BIT);
#else
    constexpr std::size_t max_fields_count = (sizeof(type) * CHAR_BIT); // We multiply by CHAR_BIT because the type may have bitfields in T
#endif

    constexpr std::size_t result = detail::detect_fields_count_dispatch<type>(size_t_<max_fields_count>{}, 1L, 1L);

    detail::assert_first_not_base<type>(detail::make_index_sequence<result>{});

#ifndef __cpp_lib_is_aggregate
    static_assert(
        is_aggregate_initializable_n<type, result>::value,
        "====================> Boost.PFR: Types with user specified constructors (non-aggregate initializable types) are not supported."
    );
#endif

    static_assert(
        result != 0 || std::is_empty<type>::value || std::is_fundamental<type>::value || std::is_reference<type>::value,
        "====================> Boost.PFR: If there's no other failed static asserts then something went wrong. Please report this issue to the github along with the structure you're reflecting."
    );

    return result;
}

}}} // namespace boost::pfr::detail

#ifdef __clang__
#   pragma clang diagnostic pop
#endif

#endif // BOOST_PFR_DETAIL_FIELDS_COUNT_HPP
// Copyright (c) 2016-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_DETAIL_FOR_EACH_FIELD_IMPL_HPP
#define BOOST_PFR_DETAIL_FOR_EACH_FIELD_IMPL_HPP


#include <utility>      // metaprogramming stuff

// Copyright (c) 2016-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_DETAIL_RVALUE_T_HPP
#define BOOST_PFR_DETAIL_RVALUE_T_HPP

#include <type_traits>
#include <utility>      // std::enable_if_t

// This header provides aliases rvalue_t and lvalue_t.
//
// Usage: template <class T> void foo(rvalue<T> rvalue);
//
// Those are useful for
//  * better type safety - you can validate at compile time that only rvalue reference is passed into the function
//  * documentation and readability - rvalue_t<T> is much better than T&&+SFINAE

namespace boost { namespace pfr { namespace detail {

/// Binds to rvalues only, no copying allowed.
template <class T
#ifdef BOOST_PFR_DETAIL_STRICT_RVALUE_TESTING
    , class = std::enable_if_t<std::is_rvalue_reference<T&&>::value>
#endif
>
using rvalue_t = T&&;

/// Binds to mutable lvalues only

}}} // namespace boost::pfr::detail

#endif // BOOST_PFR_DETAIL_RVALUE_T_HPP

namespace boost { namespace pfr { namespace detail {

template <std::size_t Index>
using size_t_ = std::integral_constant<std::size_t, Index >;

template <class T, class F, class I, class = decltype(std::declval<F>()(std::declval<T>(), I{}))>
constexpr void for_each_field_impl_apply(T&& v, F&& f, I i, long) {
    std::forward<F>(f)(std::forward<T>(v), i);
}

template <class T, class F, class I>
constexpr void for_each_field_impl_apply(T&& v, F&& f, I /*i*/, int) {
    std::forward<F>(f)(std::forward<T>(v));
}

#if !defined(__cpp_fold_expressions) || __cpp_fold_expressions < 201603
template <class T, class F, std::size_t... I>
constexpr void for_each_field_impl(T& t, F&& f, std::index_sequence<I...>, std::false_type /*move_values*/) {
     const int v[] = {0, (
         detail::for_each_field_impl_apply(sequence_tuple::get<I>(t), std::forward<F>(f), size_t_<I>{}, 1L),
         0
     )...};
     (void)v;
}


template <class T, class F, std::size_t... I>
constexpr void for_each_field_impl(T& t, F&& f, std::index_sequence<I...>, std::true_type /*move_values*/) {
     const int v[] = {0, (
         detail::for_each_field_impl_apply(sequence_tuple::get<I>(std::move(t)), std::forward<F>(f), size_t_<I>{}, 1L),
         0
     )...};
     (void)v;
}
#else
template <class T, class F, std::size_t... I>
constexpr void for_each_field_impl(T& t, F&& f, std::index_sequence<I...>, std::false_type /*move_values*/) {
     (detail::for_each_field_impl_apply(sequence_tuple::get<I>(t), std::forward<F>(f), size_t_<I>{}, 1L), ...);
}

template <class T, class F, std::size_t... I>
constexpr void for_each_field_impl(T& t, F&& f, std::index_sequence<I...>, std::true_type /*move_values*/) {
     (detail::for_each_field_impl_apply(sequence_tuple::get<I>(std::move(t)), std::forward<F>(f), size_t_<I>{}, 1L), ...);
}
#endif

}}} // namespace boost::pfr::detail


#endif // BOOST_PFR_DETAIL_FOR_EACH_FIELD_IMPL_HPP

namespace boost { namespace pfr { namespace detail {

#ifndef _MSC_VER // MSVC fails to compile the following code, but compiles the structured bindings in core17_generated.hpp
struct do_not_define_std_tuple_size_for_me {
    bool test1 = true;
};

template <class T>
constexpr bool do_structured_bindings_work() noexcept { // ******************************************* IN CASE OF ERROR READ THE FOLLOWING LINES IN boost/pfr/detail/core17.hpp FILE:
    T val{};
    auto& [a] = val; // ******************************************* IN CASE OF ERROR READ THE FOLLOWING LINES IN boost/pfr/detail/core17.hpp FILE:

    /****************************************************************************
    *
    * It looks like your compiler or Standard Library can not handle C++17
    * structured bindings.
    *
    * Workaround: Define BOOST_PFR_USE_CPP17 to 0
    * It will disable the C++17 features for Boost.PFR library.
    *
    * Sorry for the inconvenience caused.
    *
    ****************************************************************************/

    return a;
}

static_assert(
    do_structured_bindings_work<do_not_define_std_tuple_size_for_me>(),
    "====================> Boost.PFR: Your compiler can not handle C++17 structured bindings. Read the above comments for workarounds."
);
#endif // #ifndef _MSC_VER

template <class T>
constexpr auto tie_as_tuple(T& val) noexcept {
  static_assert(
    !std::is_union<T>::value,
    "====================> Boost.PFR: For safety reasons it is forbidden to reflect unions. See `Reflection of unions` section in the docs for more info."
  );
  typedef size_t_<boost::pfr::detail::fields_count<T>()> fields_count_tag;
  return boost::pfr::detail::tie_as_tuple(val, fields_count_tag{});
}

template <class T, class F, std::size_t... I>
constexpr void for_each_field_dispatcher(T& t, F&& f, std::index_sequence<I...>) {
    static_assert(
        !std::is_union<T>::value,
        "====================> Boost.PFR: For safety reasons it is forbidden to reflect unions. See `Reflection of unions` section in the docs for more info."
    );
    std::forward<F>(f)(
        detail::tie_as_tuple(t)
    );
}

}}} // namespace boost::pfr::detail

#endif // BOOST_PFR_DETAIL_CORE17_HPP
#elif BOOST_PFR_USE_LOOPHOLE
// Copyright (c) 2017-2018 Alexandr Poltavsky, Antony Polukhin.
// Copyright (c) 2019-2023 Antony Polukhin.
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)


// The Great Type Loophole (C++14)
// Initial implementation by Alexandr Poltavsky, http://alexpolt.github.io
//
// Description:
//  The Great Type Loophole is a technique that allows to exchange type information with template
//  instantiations. Basically you can assign and read type information during compile time.
//  Here it is used to detect data members of a data type. I described it for the first time in
//  this blog post http://alexpolt.github.io/type-loophole.html .
//
// This technique exploits the http://www.open-std.org/jtc1/sc22/wg21/docs/cwg_active.html#2118
// CWG 2118. Stateful metaprogramming via friend injection
// Note: CWG agreed that such techniques should be ill-formed, although the mechanism for prohibiting them is as yet undetermined.

#ifndef BOOST_PFR_DETAIL_CORE14_LOOPHOLE_HPP
#define BOOST_PFR_DETAIL_CORE14_LOOPHOLE_HPP


#include <type_traits>
#include <utility>

#include <boost/pfr/detail/cast_to_layout_compatible.hpp> // still needed for enums
// Copyright (c) 2017-2018 Chris Beck
// Copyright (c) 2019-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_DETAIL_OFFSET_BASED_GETTER_HPP
#define BOOST_PFR_DETAIL_OFFSET_BASED_GETTER_HPP


#include <type_traits>
#include <utility>
#include <memory>  // std::addressof


namespace boost { namespace pfr { namespace detail {

// Our own implementation of std::aligned_storage. On godbolt with MSVC, I have compilation errors
// using the standard version, it seems the compiler cannot generate default ctor.

template<std::size_t s, std::size_t a>
struct internal_aligned_storage {
   alignas(a) char storage_[s];
};

// Metafunction that replaces tuple<T1, T2, T3, ...> with
// tuple<std::aligned_storage_t<sizeof(T1), alignof(T1)>, std::aligned_storage<sizeof(T2), alignof(T2)>, ...>
//
// The point is, the new tuple is "layout compatible" in the sense that members have the same offsets,
// but this tuple is constexpr constructible.

template <typename T>
struct tuple_of_aligned_storage;

template <typename... Ts>
struct tuple_of_aligned_storage<sequence_tuple::tuple<Ts...>> {
  using type = sequence_tuple::tuple<internal_aligned_storage<sizeof(Ts),
#if defined(__GNUC__) && __GNUC__ < 8 && !defined(__x86_64__) && !defined(__CYGWIN__)
      // Before GCC-8 the `alignof` was returning the optimal alignment rather than the minimal one.
      // We have to adjust the alignment because otherwise we get the wrong offset.
      (alignof(Ts) > 4 ? 4 : alignof(Ts))
#else
      alignof(Ts)
#endif
  >...>;
};

// Note: If pfr has a typelist also, could also have an overload for that here

template <typename T>
using tuple_of_aligned_storage_t = typename tuple_of_aligned_storage<T>::type;

/***
 * Given a structure type and its sequence of members, we want to build a function
 * object "getter" that implements a version of `std::get` using offset arithmetic
 * and reinterpret_cast.
 *
 * typename U should be a user-defined struct
 * typename S should be a sequence_tuple which is layout compatible with U
 */

template <typename U, typename S>
class offset_based_getter {
  using this_t = offset_based_getter<U, S>;

  static_assert(sizeof(U) == sizeof(S), "====================> Boost.PFR: Member sequence does not indicate correct size for struct type! Maybe the user-provided type is not a SimpleAggregate?");
  static_assert(alignof(U) == alignof(S), "====================> Boost.PFR: Member sequence does not indicate correct alignment for struct type!");

  static_assert(!std::is_const<U>::value, "====================> Boost.PFR: const should be stripped from user-defined type when using offset_based_getter or overload resolution will be ambiguous later, this indicates an error within pfr");
  static_assert(!std::is_reference<U>::value, "====================> Boost.PFR: reference should be stripped from user-defined type when using offset_based_getter or overload resolution will be ambiguous later, this indicates an error within pfr");
  static_assert(!std::is_volatile<U>::value, "====================> Boost.PFR: volatile should be stripped from user-defined type when using offset_based_getter or overload resolution will be ambiguous later. this indicates an error within pfr");

  // Get type of idx'th member
  template <std::size_t idx>
  using index_t = typename sequence_tuple::tuple_element<idx, S>::type;

  // Get offset of idx'th member
  // Idea: Layout object has the same offsets as instance of S, so if S and U are layout compatible, then these offset
  // calculations are correct.
  template <std::size_t idx>
  static constexpr std::ptrdiff_t offset() noexcept {
    constexpr tuple_of_aligned_storage_t<S> layout{};
    return &sequence_tuple::get<idx>(layout).storage_[0] - &sequence_tuple::get<0>(layout).storage_[0];
  }

  // Encapsulates offset arithmetic and reinterpret_cast
  template <std::size_t idx>
  static index_t<idx> * get_pointer(U * u) noexcept {
    return reinterpret_cast<index_t<idx> *>(reinterpret_cast<char *>(u) + this_t::offset<idx>());
  }

  template <std::size_t idx>
  static const index_t<idx> * get_pointer(const U * u) noexcept {
    return reinterpret_cast<const index_t<idx> *>(reinterpret_cast<const char *>(u) + this_t::offset<idx>());
  }

  template <std::size_t idx>
  static volatile index_t<idx> * get_pointer(volatile U * u) noexcept {
    return reinterpret_cast<volatile index_t<idx> *>(reinterpret_cast<volatile char *>(u) + this_t::offset<idx>());
  }

  template <std::size_t idx>
  static const volatile index_t<idx> * get_pointer(const volatile U * u) noexcept {
    return reinterpret_cast<const volatile index_t<idx> *>(reinterpret_cast<const volatile char *>(u) + this_t::offset<idx>());
  }

public:
  template <std::size_t idx>
  index_t<idx> & get(U & u, size_t_<idx>) const noexcept {
    return *this_t::get_pointer<idx>(std::addressof(u));
  }

  template <std::size_t idx>
  index_t<idx> const & get(U const & u, size_t_<idx>) const noexcept {
    return *this_t::get_pointer<idx>(std::addressof(u));
  }

  template <std::size_t idx>
  index_t<idx> volatile & get(U volatile & u, size_t_<idx>) const noexcept {
    return *this_t::get_pointer<idx>(std::addressof(u));
  }

  template <std::size_t idx>
  index_t<idx> const volatile & get(U const volatile & u, size_t_<idx>) const noexcept {
    return *this_t::get_pointer<idx>(std::addressof(u));
  }

  // rvalues must not be used here, to avoid template instantiation bloats.
  template <std::size_t idx>
  index_t<idx> && get(rvalue_t<U> u, size_t_<idx>) const = delete;
};


}}} // namespace boost::pfr::detail

#endif // BOOST_PFR_DETAIL_OFFSET_LIST_HPP
// Copyright (c) 2016-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_DETAIL_MAKE_FLAT_TUPLE_OF_REFERENCES_HPP
#define BOOST_PFR_DETAIL_MAKE_FLAT_TUPLE_OF_REFERENCES_HPP


#include <utility>      // metaprogramming stuff


namespace boost { namespace pfr { namespace detail {

template <std::size_t Index>
using size_t_ = std::integral_constant<std::size_t, Index >;

// Helper: Make a "getter" object corresponding to built-in tuple::get
// For user-defined structures, the getter should be "offset_based_getter"
struct sequence_tuple_getter {
  template <std::size_t idx, typename TupleOfReferences>
  decltype(auto) get(TupleOfReferences&& t, size_t_<idx>) const noexcept {
    return sequence_tuple::get<idx>(std::forward<TupleOfReferences>(t));
  }
};


template <class TupleOrUserType, class Getter, std::size_t Begin, std::size_t Size>
constexpr auto make_flat_tuple_of_references(TupleOrUserType&, const Getter&, size_t_<Begin>, size_t_<Size>) noexcept;

template <class TupleOrUserType, class Getter, std::size_t Begin>
constexpr sequence_tuple::tuple<> make_flat_tuple_of_references(TupleOrUserType&, const Getter&, size_t_<Begin>, size_t_<0>) noexcept;

template <class TupleOrUserType, class Getter, std::size_t Begin>
constexpr auto make_flat_tuple_of_references(TupleOrUserType&, const Getter&, size_t_<Begin>, size_t_<1>) noexcept;

template <class... T>
constexpr auto tie_as_tuple_with_references(T&... args) noexcept {
    return sequence_tuple::tuple<T&...>{ args... };
}

template <class... T>
constexpr decltype(auto) tie_as_tuple_with_references(detail::sequence_tuple::tuple<T...>& t) noexcept {
    return detail::make_flat_tuple_of_references(t, sequence_tuple_getter{}, size_t_<0>{}, size_t_<sequence_tuple::tuple<T...>::size_v>{});
}

template <class... T>
constexpr decltype(auto) tie_as_tuple_with_references(const detail::sequence_tuple::tuple<T...>& t) noexcept {
    return detail::make_flat_tuple_of_references(t, sequence_tuple_getter{}, size_t_<0>{}, size_t_<sequence_tuple::tuple<T...>::size_v>{});
}

template <class Tuple1, std::size_t... I1, class Tuple2, std::size_t... I2>
constexpr auto my_tuple_cat_impl(const Tuple1& t1, std::index_sequence<I1...>, const Tuple2& t2, std::index_sequence<I2...>) noexcept {
    return detail::tie_as_tuple_with_references(
        sequence_tuple::get<I1>(t1)...,
        sequence_tuple::get<I2>(t2)...
    );
}

template <class Tuple1, class Tuple2>
constexpr auto my_tuple_cat(const Tuple1& t1, const Tuple2& t2) noexcept {
    return detail::my_tuple_cat_impl(
        t1, detail::make_index_sequence< Tuple1::size_v >{},
        t2, detail::make_index_sequence< Tuple2::size_v >{}
    );
}

template <class TupleOrUserType, class Getter, std::size_t Begin, std::size_t Size>
constexpr auto make_flat_tuple_of_references(TupleOrUserType& t, const Getter& g, size_t_<Begin>, size_t_<Size>) noexcept {
    constexpr std::size_t next_size = Size / 2;
    return detail::my_tuple_cat(
        detail::make_flat_tuple_of_references(t, g, size_t_<Begin>{}, size_t_<next_size>{}),
        detail::make_flat_tuple_of_references(t, g, size_t_<Begin + Size / 2>{}, size_t_<Size - next_size>{})
    );
}

template <class TupleOrUserType, class Getter, std::size_t Begin>
constexpr sequence_tuple::tuple<> make_flat_tuple_of_references(TupleOrUserType&, const Getter&, size_t_<Begin>, size_t_<0>) noexcept {
    return {};
}

template <class TupleOrUserType, class Getter, std::size_t Begin>
constexpr auto make_flat_tuple_of_references(TupleOrUserType& t, const Getter& g, size_t_<Begin>, size_t_<1>) noexcept {
    return detail::tie_as_tuple_with_references(
        g.get(t, size_t_<Begin>{})
    );
}

}}} // namespace boost::pfr::detail

#endif // BOOST_PFR_DETAIL_MAKE_FLAT_TUPLE_OF_REFERENCES_HPP


#ifdef __clang__
#   pragma clang diagnostic push
#   pragma clang diagnostic ignored "-Wmissing-braces"
#   pragma clang diagnostic ignored "-Wundefined-inline"
#   pragma clang diagnostic ignored "-Wundefined-internal"
#   pragma clang diagnostic ignored "-Wmissing-field-initializers"
#elif defined(__GNUC__)
#   pragma GCC diagnostic push
#   pragma GCC diagnostic ignored "-Wnon-template-friend"
#endif


namespace boost { namespace pfr { namespace detail {

// tag<T,N> generates friend declarations and helps with overload resolution.
// There are two types: one with the auto return type, which is the way we read types later.
// The second one is used in the detection of instantiations without which we'd get multiple
// definitions.

template <class T, std::size_t N>
struct tag {
    friend auto loophole(tag<T,N>);
};

// The definitions of friend functions.
template <class T, class U, std::size_t N, bool B>
struct fn_def_lref {
    friend auto loophole(tag<T,N>) {
        // Standard Library containers do not SFINAE on invalid copy constructor. Because of that std::vector<std::unique_ptr<int>> reports that it is copyable,
        // which leads to an instantiation error at this place.
        //
        // To workaround the issue, we check that the type U is movable, and move it in that case.
        using no_extents_t = std::remove_all_extents_t<U>;
        return static_cast< std::conditional_t<std::is_move_constructible<no_extents_t>::value, no_extents_t&&, no_extents_t&> >(
            boost::pfr::detail::unsafe_declval<no_extents_t&>()
        );
    }
};
template <class T, class U, std::size_t N, bool B>
struct fn_def_rref {
    friend auto loophole(tag<T,N>) { return std::move(boost::pfr::detail::unsafe_declval< std::remove_all_extents_t<U>& >()); }
};


// Those specializations are to avoid multiple definition errors.
template <class T, class U, std::size_t N>
struct fn_def_lref<T, U, N, true> {};

template <class T, class U, std::size_t N>
struct fn_def_rref<T, U, N, true> {};


// This has a templated conversion operator which in turn triggers instantiations.
// Important point, using sizeof seems to be more reliable. Also default template
// arguments are "cached" (I think). To fix that I provide a U template parameter to
// the ins functions which do the detection using constexpr friend functions and SFINAE.
template <class T, std::size_t N>
struct loophole_ubiq_lref {
    template<class U, std::size_t M> static std::size_t ins(...);
    template<class U, std::size_t M, std::size_t = sizeof(loophole(tag<T,M>{})) > static char ins(int);

    template<class U, std::size_t = sizeof(fn_def_lref<T, U, N, sizeof(ins<U, N>(0)) == sizeof(char)>)>
    constexpr operator U&() const&& noexcept; // `const&&` here helps to avoid ambiguity in loophole instantiations. optional_like test validate that behavior.
};

template <class T, std::size_t N>
struct loophole_ubiq_rref {
    template<class U, std::size_t M> static std::size_t ins(...);
    template<class U, std::size_t M, std::size_t = sizeof(loophole(tag<T,M>{})) > static char ins(int);

    template<class U, std::size_t = sizeof(fn_def_rref<T, U, N, sizeof(ins<U, N>(0)) == sizeof(char)>)>
    constexpr operator U&&() const&& noexcept; // `const&&` here helps to avoid ambiguity in loophole instantiations. optional_like test validate that behavior.
};


// This is a helper to turn a data structure into a tuple.
template <class T, class U>
struct loophole_type_list_lref;

template <typename T, std::size_t... I>
struct loophole_type_list_lref< T, std::index_sequence<I...> >
     // Instantiating loopholes:
    : sequence_tuple::tuple< decltype(T{ loophole_ubiq_lref<T, I>{}... }, 0) >
{
    using type = sequence_tuple::tuple< decltype(loophole(tag<T, I>{}))... >;
};


template <class T, class U>
struct loophole_type_list_rref;

template <typename T, std::size_t... I>
struct loophole_type_list_rref< T, std::index_sequence<I...> >
     // Instantiating loopholes:
    : sequence_tuple::tuple< decltype(T{ loophole_ubiq_rref<T, I>{}... }, 0) >
{
    using type = sequence_tuple::tuple< decltype(loophole(tag<T, I>{}))... >;
};


// Lazily returns loophole_type_list_{lr}ref.
template <bool IsCopyConstructible /*= true*/, class T, class U>
struct loophole_type_list_selector {
    using type = loophole_type_list_lref<T, U>;
};

template <class T, class U>
struct loophole_type_list_selector<false /*IsCopyConstructible*/, T, U> {
    using type = loophole_type_list_rref<T, U>;
};

template <class T>
auto tie_as_tuple_loophole_impl(T& lvalue) noexcept {
    using type = std::remove_cv_t<std::remove_reference_t<T>>;
    using indexes = detail::make_index_sequence<fields_count<type>()>;
    using loophole_type_list = typename detail::loophole_type_list_selector<
        std::is_copy_constructible<std::remove_all_extents_t<type>>::value, type, indexes
    >::type;
    using tuple_type = typename loophole_type_list::type;

    return boost::pfr::detail::make_flat_tuple_of_references(
        lvalue,
        offset_based_getter<type, tuple_type>{},
        size_t_<0>{},
        size_t_<tuple_type::size_v>{}
    );
}

template <class T>
auto tie_as_tuple(T& val) noexcept {
    static_assert(
        !std::is_union<T>::value,
        "====================> Boost.PFR: For safety reasons it is forbidden to reflect unions. See `Reflection of unions` section in the docs for more info."
    );
    return boost::pfr::detail::tie_as_tuple_loophole_impl(
        val
    );
}

template <class T, class F, std::size_t... I>
void for_each_field_dispatcher(T& t, F&& f, std::index_sequence<I...>) {
    static_assert(
        !std::is_union<T>::value,
        "====================> Boost.PFR: For safety reasons it is forbidden to reflect unions. See `Reflection of unions` section in the docs for more info."
    );
    std::forward<F>(f)(
        boost::pfr::detail::tie_as_tuple_loophole_impl(t)
    );
}

}}} // namespace boost::pfr::detail


#ifdef __clang__
#   pragma clang diagnostic pop
#elif defined(__GNUC__)
#   pragma GCC diagnostic pop
#endif


#endif // BOOST_PFR_DETAIL_CORE14_LOOPHOLE_HPP

#else
// Copyright (c) 2016-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_DETAIL_CORE14_CLASSIC_HPP
#define BOOST_PFR_DETAIL_CORE14_CLASSIC_HPP


#include <type_traits>
#include <utility>      // metaprogramming stuff

// Copyright (c) 2016-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_DETAIL_SIZE_ARRAY_HPP
#define BOOST_PFR_DETAIL_SIZE_ARRAY_HPP


#include <cstddef>      // metaprogramming stuff

namespace boost { namespace pfr { namespace detail {

///////////////////// Array that has the constexpr
template <std::size_t N>
struct size_array {                         // libc++ misses constexpr on operator[]
    typedef std::size_t type;
    std::size_t data[N];

    static constexpr std::size_t size() noexcept { return N; }

    constexpr std::size_t count_nonzeros() const noexcept {
        std::size_t count = 0;
        for (std::size_t i = 0; i < size(); ++i) {
            if (data[i]) {
                ++ count;
            }
        }
        return count;
    }

    constexpr std::size_t count_from_opening_till_matching_parenthis_seq(std::size_t from, std::size_t opening_parenthis, std::size_t closing_parenthis) const noexcept {
        if (data[from] != opening_parenthis) {
            return 0;
        }
        std::size_t unclosed_parnthesis = 0;
        std::size_t count = 0;
        for (; ; ++from) {
            if (data[from] == opening_parenthis) {
                ++ unclosed_parnthesis;
            } else if (data[from] == closing_parenthis) {
                -- unclosed_parnthesis;
            }
            ++ count;

            if (unclosed_parnthesis == 0) {
                return count;
            }
        }

        return count;
    }
};

template <>
struct size_array<0> {                         // libc++ misses constexpr on operator[]
    typedef std::size_t type;
    std::size_t data[1];

    static constexpr std::size_t size() noexcept { return 0; }

    constexpr std::size_t count_nonzeros() const noexcept {
        return 0;
    }
};

template <std::size_t I, std::size_t N>
constexpr std::size_t get(const size_array<N>& a) noexcept {
    static_assert(I < N, "====================> Boost.PFR: Array index out of bounds");
    return a.data[I];
}



}}} // namespace boost::pfr::detail

#endif // BOOST_PFR_DETAIL_SIZE_ARRAY_HPP

#ifdef __clang__
#   pragma clang diagnostic push
#   pragma clang diagnostic ignored "-Wmissing-braces"
#   pragma clang diagnostic ignored "-Wundefined-inline"
#   pragma clang diagnostic ignored "-Wundefined-internal"
#   pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace boost { namespace pfr { namespace detail {

///////////////////// General utility stuff

template <class T> struct identity {
    typedef T type;
};

template <class T>
constexpr T construct_helper() noexcept { // adding const here allows to deal with copyable only types
    return {};
}

template <class T> constexpr size_array<sizeof(T) * 3> fields_count_and_type_ids_with_zeros() noexcept;
template <class T> constexpr auto flat_array_of_type_ids() noexcept;

///////////////////// All the stuff for representing Type as integer and converting integer back to type
namespace typeid_conversions {

///////////////////// Helper constants and typedefs

#ifdef _MSC_VER
#   pragma warning( push )
    // '<<': check operator precedence for possible error; use parentheses to clarify precedence
#   pragma warning( disable : 4554 )
#endif

constexpr std::size_t native_types_mask = 31;
constexpr std::size_t bits_per_extension = 3;
constexpr std::size_t extension_mask = (
    static_cast<std::size_t>((1 << bits_per_extension) - 1)
        << static_cast<std::size_t>(sizeof(std::size_t) * 8 - bits_per_extension)
);
constexpr std::size_t native_ptr_type = (
    static_cast<std::size_t>(1)
        << static_cast<std::size_t>(sizeof(std::size_t) * 8 - bits_per_extension)
);
constexpr std::size_t native_const_ptr_type = (
    static_cast<std::size_t>(2)
        << static_cast<std::size_t>(sizeof(std::size_t) * 8 - bits_per_extension)
);

constexpr std::size_t native_const_volatile_ptr_type = (
    static_cast<std::size_t>(3)
        << static_cast<std::size_t>(sizeof(std::size_t) * 8 - bits_per_extension)
);

constexpr std::size_t native_volatile_ptr_type = (
    static_cast<std::size_t>(4)
        << static_cast<std::size_t>(sizeof(std::size_t) * 8 - bits_per_extension)
);

constexpr std::size_t native_ref_type = (
    static_cast<std::size_t>(5)
        << static_cast<std::size_t>(sizeof(std::size_t) * 8 - bits_per_extension)
);

template <std::size_t Index, std::size_t Extension>
using if_extension = std::enable_if_t< (Index & extension_mask) == Extension >*;

///////////////////// Helper functions
template <std::size_t Unptr>
constexpr std::size_t type_to_id_extension_apply(std::size_t ext) noexcept {
    constexpr std::size_t native_id = (Unptr & native_types_mask);
    constexpr std::size_t extensions = (Unptr & ~native_types_mask);
    static_assert(
        !((extensions >> bits_per_extension) & native_types_mask),
        "====================> Boost.PFR: Too many extensions for a single field (something close to `int************************** p;` is in the POD type)."
    );

    return (extensions >> bits_per_extension) | native_id | ext;
}

template <std::size_t Index>
using remove_1_ext = size_t_<
    ((Index & ~native_types_mask) << bits_per_extension) | (Index & native_types_mask)
>;

#ifdef _MSC_VER
#   pragma warning( pop )
#endif

///////////////////// Forward declarations

template <class Type> constexpr std::size_t type_to_id(identity<Type*>) noexcept;
template <class Type> constexpr std::size_t type_to_id(identity<const Type*>) noexcept;
template <class Type> constexpr std::size_t type_to_id(identity<const volatile Type*>) noexcept;
template <class Type> constexpr std::size_t type_to_id(identity<volatile Type*>) noexcept;
template <class Type> constexpr std::size_t type_to_id(identity<Type&>) noexcept;
template <class Type> constexpr std::size_t type_to_id(identity<Type>, std::enable_if_t<std::is_enum<Type>::value>* = nullptr) noexcept;
template <class Type> constexpr std::size_t type_to_id(identity<Type>, std::enable_if_t<std::is_empty<Type>::value>* = nullptr) noexcept;
template <class Type> constexpr std::size_t type_to_id(identity<Type>, std::enable_if_t<std::is_union<Type>::value>* = nullptr) noexcept;
template <class Type> constexpr size_array<sizeof(Type) * 3> type_to_id(identity<Type>, std::enable_if_t<!std::is_enum<Type>::value && !std::is_empty<Type>::value && !std::is_union<Type>::value>* = 0) noexcept;

template <std::size_t Index> constexpr auto id_to_type(size_t_<Index >, if_extension<Index, native_const_ptr_type> = nullptr) noexcept;
template <std::size_t Index> constexpr auto id_to_type(size_t_<Index >, if_extension<Index, native_ptr_type> = nullptr) noexcept;
template <std::size_t Index> constexpr auto id_to_type(size_t_<Index >, if_extension<Index, native_const_volatile_ptr_type> = nullptr) noexcept;
template <std::size_t Index> constexpr auto id_to_type(size_t_<Index >, if_extension<Index, native_volatile_ptr_type> = nullptr) noexcept;
template <std::size_t Index> constexpr auto id_to_type(size_t_<Index >, if_extension<Index, native_ref_type> = nullptr) noexcept;


///////////////////// Definitions of type_to_id and id_to_type for fundamental types
/// @cond
#define BOOST_MAGIC_GET_REGISTER_TYPE(Type, Index)              \
    constexpr std::size_t type_to_id(identity<Type>) noexcept { \
        return Index;                                           \
    }                                                           \
    constexpr Type id_to_type( size_t_<Index > ) noexcept {     \
        return detail::construct_helper<Type>();                \
    }                                                           \
    /**/
/// @endcond


// Register all base types here
BOOST_MAGIC_GET_REGISTER_TYPE(unsigned char         , 1)
BOOST_MAGIC_GET_REGISTER_TYPE(unsigned short        , 2)
BOOST_MAGIC_GET_REGISTER_TYPE(unsigned int          , 3)
BOOST_MAGIC_GET_REGISTER_TYPE(unsigned long         , 4)
BOOST_MAGIC_GET_REGISTER_TYPE(unsigned long long    , 5)
BOOST_MAGIC_GET_REGISTER_TYPE(signed char           , 6)
BOOST_MAGIC_GET_REGISTER_TYPE(short                 , 7)
BOOST_MAGIC_GET_REGISTER_TYPE(int                   , 8)
BOOST_MAGIC_GET_REGISTER_TYPE(long                  , 9)
BOOST_MAGIC_GET_REGISTER_TYPE(long long             , 10)
BOOST_MAGIC_GET_REGISTER_TYPE(char                  , 11)
BOOST_MAGIC_GET_REGISTER_TYPE(wchar_t               , 12)
BOOST_MAGIC_GET_REGISTER_TYPE(char16_t              , 13)
BOOST_MAGIC_GET_REGISTER_TYPE(char32_t              , 14)
BOOST_MAGIC_GET_REGISTER_TYPE(float                 , 15)
BOOST_MAGIC_GET_REGISTER_TYPE(double                , 16)
BOOST_MAGIC_GET_REGISTER_TYPE(long double           , 17)
BOOST_MAGIC_GET_REGISTER_TYPE(bool                  , 18)
BOOST_MAGIC_GET_REGISTER_TYPE(void*                 , 19)
BOOST_MAGIC_GET_REGISTER_TYPE(const void*           , 20)
BOOST_MAGIC_GET_REGISTER_TYPE(volatile void*        , 21)
BOOST_MAGIC_GET_REGISTER_TYPE(const volatile void*  , 22)
BOOST_MAGIC_GET_REGISTER_TYPE(std::nullptr_t        , 23)
constexpr std::size_t tuple_begin_tag               = 24;
constexpr std::size_t tuple_end_tag                 = 25;

#undef BOOST_MAGIC_GET_REGISTER_TYPE

///////////////////// Definitions of type_to_id and id_to_type for types with extensions and nested types
template <class Type>
constexpr std::size_t type_to_id(identity<Type*>) noexcept {
    constexpr auto unptr = typeid_conversions::type_to_id(identity<Type>{});
    static_assert(
        std::is_same<const std::size_t, decltype(unptr)>::value,
        "====================> Boost.PFR: Pointers to user defined types are not supported."
    );
    return typeid_conversions::type_to_id_extension_apply<unptr>(native_ptr_type);
}

template <class Type>
constexpr std::size_t type_to_id(identity<const Type*>) noexcept {
    constexpr auto unptr = typeid_conversions::type_to_id(identity<Type>{});
    static_assert(
        std::is_same<const std::size_t, decltype(unptr)>::value,
        "====================> Boost.PFR: Const pointers to user defined types are not supported."
    );
    return typeid_conversions::type_to_id_extension_apply<unptr>(native_const_ptr_type);
}

template <class Type>
constexpr std::size_t type_to_id(identity<const volatile Type*>) noexcept {
    constexpr auto unptr = typeid_conversions::type_to_id(identity<Type>{});
    static_assert(
        std::is_same<const std::size_t, decltype(unptr)>::value,
        "====================> Boost.PFR: Const volatile pointers to user defined types are not supported."
    );
    return typeid_conversions::type_to_id_extension_apply<unptr>(native_const_volatile_ptr_type);
}

template <class Type>
constexpr std::size_t type_to_id(identity<volatile Type*>) noexcept {
    constexpr auto unptr = typeid_conversions::type_to_id(identity<Type>{});
    static_assert(
        std::is_same<const std::size_t, decltype(unptr)>::value,
        "====================> Boost.PFR: Volatile pointers to user defined types are not supported."
    );
    return typeid_conversions::type_to_id_extension_apply<unptr>(native_volatile_ptr_type);
}

template <class Type>
constexpr std::size_t type_to_id(identity<Type&>) noexcept {
    constexpr auto unptr = typeid_conversions::type_to_id(identity<Type>{});
    static_assert(
        std::is_same<const std::size_t, decltype(unptr)>::value,
        "====================> Boost.PFR: References to user defined types are not supported."
    );
    return typeid_conversions::type_to_id_extension_apply<unptr>(native_ref_type);
}

template <class Type>
constexpr std::size_t type_to_id(identity<Type>, std::enable_if_t<std::is_enum<Type>::value>*) noexcept {
    return typeid_conversions::type_to_id(identity<typename std::underlying_type<Type>::type >{});
}

template <class Type>
constexpr std::size_t type_to_id(identity<Type>, std::enable_if_t<std::is_empty<Type>::value>*) noexcept {
    static_assert(!std::is_empty<Type>::value, "====================> Boost.PFR: Empty classes/structures as members are not supported.");
    return 0;
}

template <class Type>
constexpr std::size_t type_to_id(identity<Type>, std::enable_if_t<std::is_union<Type>::value>*) noexcept {
    static_assert(
        !std::is_union<Type>::value,
        "====================> Boost.PFR: For safety reasons it is forbidden to reflect unions. See `Reflection of unions` section in the docs for more info."
    );
    return 0;
}

template <class Type>
constexpr size_array<sizeof(Type) * 3> type_to_id(identity<Type>, std::enable_if_t<!std::is_enum<Type>::value && !std::is_empty<Type>::value && !std::is_union<Type>::value>*) noexcept {
    constexpr auto t = detail::flat_array_of_type_ids<Type>();
    size_array<sizeof(Type) * 3> result {{tuple_begin_tag}};
    constexpr bool requires_tuplening = (
        (t.count_nonzeros() != 1)  || (t.count_nonzeros() == t.count_from_opening_till_matching_parenthis_seq(0, tuple_begin_tag, tuple_end_tag))
    );

    if (requires_tuplening) {
        for (std::size_t i = 0; i < t.size(); ++i)
            result.data[i + 1] = t.data[i];
        result.data[result.size() - 1] = tuple_end_tag;
    } else {
        for (std::size_t i = 0; i < t.size(); ++i)
            result.data[i] = t.data[i];
    }
    return result;
}



template <std::size_t Index>
constexpr auto id_to_type(size_t_<Index >, if_extension<Index, native_ptr_type>) noexcept {
    typedef decltype( typeid_conversions::id_to_type(remove_1_ext<Index>()) )* res_t;
    return detail::construct_helper<res_t>();
}

template <std::size_t Index>
constexpr auto id_to_type(size_t_<Index >, if_extension<Index, native_const_ptr_type>) noexcept {
    typedef const decltype( typeid_conversions::id_to_type(remove_1_ext<Index>()) )* res_t;
    return detail::construct_helper<res_t>();
}

template <std::size_t Index>
constexpr auto id_to_type(size_t_<Index >, if_extension<Index, native_const_volatile_ptr_type>) noexcept {
    typedef const volatile decltype( typeid_conversions::id_to_type(remove_1_ext<Index>()) )* res_t;
    return detail::construct_helper<res_t>();
}


template <std::size_t Index>
constexpr auto id_to_type(size_t_<Index >, if_extension<Index, native_volatile_ptr_type>) noexcept {
    typedef volatile decltype( typeid_conversions::id_to_type(remove_1_ext<Index>()) )* res_t;
    return detail::construct_helper<res_t>();
}


template <std::size_t Index>
constexpr auto id_to_type(size_t_<Index >, if_extension<Index, native_ref_type>) noexcept {
    static_assert(!Index, "====================> Boost.PFR: References are not supported");
    return nullptr;
}

} // namespace typeid_conversions

///////////////////// Structure that remembers types as integers on a `constexpr operator Type()` call
struct ubiq_val {
    std::size_t* ref_;

    template <class T>
    constexpr void assign(const T& typeids) const noexcept {
        for (std::size_t i = 0; i < T::size(); ++i)
            ref_[i] = typeids.data[i];
    }

    constexpr void assign(std::size_t val) const noexcept {
        ref_[0] = val;
    }

    template <class Type>
    constexpr operator Type() const noexcept {
        constexpr auto typeids = typeid_conversions::type_to_id(identity<Type>{});
        assign(typeids);
        return detail::construct_helper<Type>();
    }
};

///////////////////// Structure that remembers size of the type on a `constexpr operator Type()` call
struct ubiq_sizes {
    std::size_t& ref_;

    template <class Type>
    constexpr operator Type() const noexcept {
        ref_ = sizeof(Type);
        return detail::construct_helper<Type>();
    }
};

///////////////////// Returns array of (offsets without accounting alignments). Required for keeping places for nested type ids
template <class T, std::size_t N, std::size_t... I>
constexpr size_array<N> get_type_offsets() noexcept {
    typedef size_array<N> array_t;
    array_t sizes{};
    T tmp{ ubiq_sizes{sizes.data[I]}... };
    (void)tmp;

    array_t offsets{{0}};
    for (std::size_t i = 1; i < N; ++i)
        offsets.data[i] = offsets.data[i - 1] + sizes.data[i - 1];

    return offsets;
}

///////////////////// Returns array of typeids and zeros if constructor of a type accepts sizeof...(I) parameters
template <class T, std::size_t N, std::size_t... I>
constexpr void* flat_type_to_array_of_type_ids(std::size_t* types, std::index_sequence<I...>) noexcept
{
    static_assert(
        N <= sizeof(T),
        "====================> Boost.PFR: Bit fields are not supported."
    );

    constexpr auto offsets = detail::get_type_offsets<T, N, I...>();
    T tmp{ ubiq_val{types + get<I>(offsets) * 3}... };
    (void)types;
    (void)tmp;
    (void)offsets; // If type is empty offsets are not used
    return nullptr;
}

///////////////////// Returns array of typeids and zeros
template <class T>
constexpr size_array<sizeof(T) * 3> fields_count_and_type_ids_with_zeros() noexcept {
    size_array<sizeof(T) * 3> types{};
    constexpr std::size_t N = detail::fields_count<T>();
    detail::flat_type_to_array_of_type_ids<T, N>(types.data, detail::make_index_sequence<N>());
    return types;
}

///////////////////// Returns array of typeids without zeros
template <class T>
constexpr auto flat_array_of_type_ids() noexcept {
    constexpr auto types = detail::fields_count_and_type_ids_with_zeros<T>();
    constexpr std::size_t count = types.count_nonzeros();
    size_array<count> res{};
    std::size_t j = 0;
    for (std::size_t i = 0; i < decltype(types)::size(); ++i) {
        if (types.data[i]) {
            res.data[j] = types.data[i];
            ++ j;
        }
    }

    return res;
}

///////////////////// Convert array of typeids into sequence_tuple::tuple

template <class T, std::size_t First, std::size_t... I>
constexpr auto as_flat_tuple_impl(std::index_sequence<First, I...>) noexcept;

template <class T>
constexpr sequence_tuple::tuple<> as_flat_tuple_impl(std::index_sequence<>) noexcept {
    return sequence_tuple::tuple<>{};
}

template <std::size_t Increment, std::size_t... I>
constexpr auto increment_index_sequence(std::index_sequence<I...>) noexcept {
    return std::index_sequence<I + Increment...>{};
}

template <class T, std::size_t V, std::size_t I, std::size_t SubtupleLength>
constexpr auto prepare_subtuples(size_t_<V>, size_t_<I>, size_t_<SubtupleLength>) noexcept {
    static_assert(SubtupleLength == 0, "====================> Boost.PFR: Internal error while representing nested field as tuple");
    return typeid_conversions::id_to_type(size_t_<V>{});
}

template <class T, std::size_t I, std::size_t SubtupleLength>
constexpr auto prepare_subtuples(size_t_<typeid_conversions::tuple_end_tag>, size_t_<I>, size_t_<SubtupleLength>) noexcept {
    static_assert(sizeof(T) == 0, "====================> Boost.PFR: Internal error while representing nested field as tuple");
    return int{};
}

template <class T, std::size_t I, std::size_t SubtupleLength>
constexpr auto prepare_subtuples(size_t_<typeid_conversions::tuple_begin_tag>, size_t_<I>, size_t_<SubtupleLength>) noexcept {
    static_assert(SubtupleLength > 2, "====================> Boost.PFR: Internal error while representing nested field as tuple");
    constexpr auto seq = detail::make_index_sequence<SubtupleLength - 2>{};
    return detail::as_flat_tuple_impl<T>( detail::increment_index_sequence<I + 1>(seq) );
}


template <class Array>
constexpr Array remove_subtuples(Array indexes_plus_1, const Array& subtuple_lengths) noexcept {
    for (std::size_t i = 0; i < subtuple_lengths.size(); ++i) {
        if (subtuple_lengths.data[i]) {
            const std::size_t skips_count = subtuple_lengths.data[i];
            for (std::size_t j = i + 1; j < skips_count + i; ++j) {
                indexes_plus_1.data[j] = 0;
            }
            i += skips_count - 1;
        }
    }
    return indexes_plus_1;
}

template <std::size_t N, class Array>
constexpr size_array<N> resize_dropping_zeros_and_decrementing(size_t_<N>, const Array& a) noexcept {
    size_array<N> result{};
    std::size_t result_indx = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a.data[i]) {
            result.data[result_indx] = static_cast<std::size_t>(a.data[i] - 1);
            ++ result_indx;
        }
    }

    return result;
}

template <class T, std::size_t First, std::size_t... I, std::size_t... INew>
constexpr auto as_flat_tuple_impl_drop_helpers(std::index_sequence<First, I...>, std::index_sequence<INew...>) noexcept {
    constexpr auto a = detail::flat_array_of_type_ids<T>();

    constexpr size_array<sizeof...(I) + 1> subtuples_length {{
        a.count_from_opening_till_matching_parenthis_seq(First, typeid_conversions::tuple_begin_tag, typeid_conversions::tuple_end_tag),
        a.count_from_opening_till_matching_parenthis_seq(I, typeid_conversions::tuple_begin_tag, typeid_conversions::tuple_end_tag)...
    }};

    constexpr size_array<sizeof...(I) + 1> type_indexes_with_subtuple_internals {{ 1, 1 + I - First...}};
    constexpr auto type_indexes_plus_1_and_zeros_as_skips = detail::remove_subtuples(type_indexes_with_subtuple_internals, subtuples_length);
    constexpr auto new_size = size_t_<type_indexes_plus_1_and_zeros_as_skips.count_nonzeros()>{};
    constexpr auto type_indexes = detail::resize_dropping_zeros_and_decrementing(new_size, type_indexes_plus_1_and_zeros_as_skips);

    typedef sequence_tuple::tuple<
        decltype(detail::prepare_subtuples<T>(
            size_t_< a.data[ First + type_indexes.data[INew] ]          >{},    // id of type
            size_t_< First + type_indexes.data[INew]                    >{},    // index of current id in `a`
            size_t_< subtuples_length.data[ type_indexes.data[INew] ]   >{}     // if id of type is tuple, then length of that tuple
        ))...
    > subtuples_uncleanuped_t;

    return subtuples_uncleanuped_t{};
}

template <class Array>
constexpr std::size_t count_skips_in_array(std::size_t begin_index, std::size_t end_index, const Array& a) noexcept {
    std::size_t skips = 0;
    for (std::size_t i = begin_index; i < end_index; ++i) {
        if (a.data[i] == typeid_conversions::tuple_begin_tag) {
            const std::size_t this_tuple_size = a.count_from_opening_till_matching_parenthis_seq(i, typeid_conversions::tuple_begin_tag, typeid_conversions::tuple_end_tag) - 1;
            skips += this_tuple_size;
            i += this_tuple_size - 1;
        }
    }

    return skips;
}

template <class T, std::size_t First, std::size_t... I>
constexpr auto as_flat_tuple_impl(std::index_sequence<First, I...>) noexcept {
    constexpr auto a = detail::flat_array_of_type_ids<T>();
    constexpr std::size_t count_of_I = sizeof...(I);

    return detail::as_flat_tuple_impl_drop_helpers<T>(
        std::index_sequence<First, I...>{},
        detail::make_index_sequence< 1 + count_of_I - count_skips_in_array(First, First + count_of_I, a) >{}
    );
}

template <class T>
constexpr auto internal_tuple_with_same_alignment() noexcept {
    typedef typename std::remove_cv<T>::type type;

    static_assert(
        std::is_trivial<type>::value && std::is_standard_layout<type>::value,
        "====================> Boost.PFR: Type can not be reflected without Loophole or C++17, because it's not POD"
    );
    static_assert(!std::is_reference<type>::value, "====================> Boost.PFR: Not applyable");
    constexpr auto res = detail::as_flat_tuple_impl<type>(
        detail::make_index_sequence< decltype(detail::flat_array_of_type_ids<type>())::size() >()
    );

    return res;
}

template <class T>
using internal_tuple_with_same_alignment_t = decltype( detail::internal_tuple_with_same_alignment<T>() );


///////////////////// Flattening
struct ubiq_is_flat_refelectable {
    bool& is_flat_refelectable;

    template <class Type>
    constexpr operator Type() const noexcept {
        is_flat_refelectable = std::is_fundamental<std::remove_pointer_t<Type>>::value;
        return {};
    }
};

template <class T, std::size_t... I>
constexpr bool is_flat_refelectable(std::index_sequence<I...>) noexcept {
    constexpr std::size_t fields = sizeof...(I);
    bool result[fields] = {static_cast<bool>(I)...};
    const T v{ ubiq_is_flat_refelectable{result[I]}... };
    (void)v;

    for (std::size_t i = 0; i < fields; ++i) {
        if (!result[i]) {
            return false;
        }
    }

    return true;
}

template<class T>
constexpr bool is_flat_refelectable(std::index_sequence<>) noexcept {
    return true; ///< all empty structs always flat refelectable
}

template <class T>
auto tie_as_flat_tuple(T& lvalue) noexcept {
    static_assert(
        !std::is_union<T>::value,
        "====================> Boost.PFR: For safety reasons it is forbidden to reflect unions. See `Reflection of unions` section in the docs for more info."
    );
    using type = std::remove_cv_t<T>;
    using tuple_type = internal_tuple_with_same_alignment_t<type>;

    offset_based_getter<type, tuple_type> getter;
    return boost::pfr::detail::make_flat_tuple_of_references(lvalue, getter, size_t_<0>{}, size_t_<tuple_type::size_v>{});
}

template <class T>
auto tie_as_tuple(T& val) noexcept {
    static_assert(
        !std::is_union<T>::value,
        "====================> Boost.PFR: For safety reasons it is forbidden to reflect unions. See `Reflection of unions` section in the docs for more info."
    );
    static_assert(
        boost::pfr::detail::is_flat_refelectable<T>( detail::make_index_sequence<boost::pfr::detail::fields_count<T>()>{} ),
        "====================> Boost.PFR: Not possible in C++14 to represent that type without loosing information. Change type definition or enable C++17"
    );
    return boost::pfr::detail::tie_as_flat_tuple(val);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

///////////////////// Structure that can be converted to copy of anything
struct ubiq_constructor_constexpr_copy {
    std::size_t ignore;

    template <class Type>
    constexpr operator Type() const noexcept {
        static_assert(
            std::is_trivially_destructible<Type>::value,
            "====================> Boost.PFR: One of the fields in the type passed to `for_each_field` has non trivial destructor."
        );
        return {};
    }
};

/////////////////////

template <class T, std::size_t... I>
struct is_constexpr_aggregate_initializable {
    template<class T2, std::size_t... I2>
    static constexpr void* constexpr_aggregate_initializer() noexcept {
        T2 tmp{ ubiq_constructor_constexpr_copy{I2}... };
        (void)tmp;
        return nullptr;
    }

    template <void* = constexpr_aggregate_initializer<T, I...>() >
    static std::true_type test(long) noexcept;

    static std::false_type test(...) noexcept;

    static constexpr bool value = decltype(test(0)){};
};


template <class T, class F, std::size_t I0, std::size_t... I, class... Fields>
void for_each_field_in_depth(T& t, F&& f, std::index_sequence<I0, I...>, identity<Fields>...);

template <class T, class F, class... Fields>
void for_each_field_in_depth(T& t, F&& f, std::index_sequence<>, identity<Fields>...);

template <class T, class F, class IndexSeq, class... Fields>
struct next_step {
    T& t;
    F& f;

    template <class Field>
    operator Field() const {
         boost::pfr::detail::for_each_field_in_depth(
             t,
             std::forward<F>(f),
             IndexSeq{},
             identity<Fields>{}...,
             identity<Field>{}
         );

         return {};
    }
};

template <class T, class F, std::size_t I0, std::size_t... I, class... Fields>
void for_each_field_in_depth(T& t, F&& f, std::index_sequence<I0, I...>, identity<Fields>...) {
    (void)std::add_const_t<std::remove_reference_t<T>>{
        Fields{}...,
        next_step<T, F, std::index_sequence<I...>, Fields...>{t, f},
        ubiq_constructor_constexpr_copy{I}...
    };
}

template <class T, class F, class... Fields>
void for_each_field_in_depth(T& lvalue, F&& f, std::index_sequence<>, identity<Fields>...) {
    using tuple_type = sequence_tuple::tuple<Fields...>;

    offset_based_getter<std::remove_cv_t<std::remove_reference_t<T>>, tuple_type> getter;
    std::forward<F>(f)(
        boost::pfr::detail::make_flat_tuple_of_references(lvalue, getter, size_t_<0>{}, size_t_<sizeof...(Fields)>{})
    );
}

template <class T, class F, std::size_t... I>
void for_each_field_dispatcher_1(T& t, F&& f, std::index_sequence<I...>, std::true_type /*is_flat_refelectable*/) {
    std::forward<F>(f)(
        boost::pfr::detail::tie_as_flat_tuple(t)
    );
}


template <class T, class F, std::size_t... I>
void for_each_field_dispatcher_1(T& t, F&& f, std::index_sequence<I...>, std::false_type /*is_flat_refelectable*/) {
    boost::pfr::detail::for_each_field_in_depth(
        t,
        std::forward<F>(f),
        std::index_sequence<I...>{}
    );
}

template <class T, class F, std::size_t... I>
void for_each_field_dispatcher(T& t, F&& f, std::index_sequence<I...>) {
    static_assert(
        !std::is_union<T>::value,
        "====================> Boost.PFR: For safety reasons it is forbidden to reflect unions. See `Reflection of unions` section in the docs for more info."
    );
    static_assert(is_constexpr_aggregate_initializable<T, I...>::value, "====================> Boost.PFR: T must be a constexpr initializable type");

    constexpr bool is_flat_refelectable_val = detail::is_flat_refelectable<T>( std::index_sequence<I...>{} );
    detail::for_each_field_dispatcher_1(
        t,
        std::forward<F>(f),
        std::index_sequence<I...>{},
        std::integral_constant<bool, is_flat_refelectable_val>{}
    );
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


#ifdef __clang__
#   pragma clang diagnostic pop
#endif

}}} // namespace boost::pfr::detail

#endif // BOOST_PFR_DETAIL_CORE14_CLASSIC_HPP
#endif

#endif // BOOST_PFR_DETAIL_CORE_HPP

// Copyright (c) 2016-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_DETAIL_STDTUPLE_HPP
#define BOOST_PFR_DETAIL_STDTUPLE_HPP


#include <utility>      // metaprogramming stuff
#include <tuple>


namespace boost { namespace pfr { namespace detail {

template <class T, std::size_t... I>
constexpr auto make_stdtuple_from_tietuple(const T& t, std::index_sequence<I...>) noexcept {
    return std::make_tuple(
        boost::pfr::detail::sequence_tuple::get<I>(t)...
    );
}

template <class T, std::size_t... I>
constexpr auto make_stdtiedtuple_from_tietuple(const T& t, std::index_sequence<I...>) noexcept {
    return std::tie(
        boost::pfr::detail::sequence_tuple::get<I>(t)...
    );
}

template <class T, std::size_t... I>
constexpr auto make_conststdtiedtuple_from_tietuple(const T& t, std::index_sequence<I...>) noexcept {
    return std::tuple<
        std::add_lvalue_reference_t<std::add_const_t<
            std::remove_reference_t<decltype(boost::pfr::detail::sequence_tuple::get<I>(t))>
        >>...
    >(
        boost::pfr::detail::sequence_tuple::get<I>(t)...
    );
}

}}} // namespace boost::pfr::detail

#endif // BOOST_PFR_DETAIL_STDTUPLE_HPP
// Copyright (c) 2018 Adam Butcher, Antony Polukhin
// Copyright (c) 2019-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_DETAIL_TIE_FROM_STRUCTURE_TUPLE_HPP
#define BOOST_PFR_DETAIL_TIE_FROM_STRUCTURE_TUPLE_HPP



// Copyright (c) 2016-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)


#ifndef BOOST_PFR_TUPLE_SIZE_HPP
#define BOOST_PFR_TUPLE_SIZE_HPP


#include <type_traits>
#include <utility>      // metaprogramming stuff


/// \file boost/pfr/tuple_size.hpp
/// Contains tuple-like interfaces to get fields count \forcedlink{tuple_size}, \forcedlink{tuple_size_v}.
///
/// \b Synopsis:
namespace boost { namespace pfr {

/// Has a static const member variable `value` that contains fields count in a T.
/// Works for any T that satisfies \aggregate.
///
/// \b Example:
/// \code
///     std::array<int, boost::pfr::tuple_size<my_structure>::value > a;
/// \endcode
template <class T>
using tuple_size = detail::size_t_< boost::pfr::detail::fields_count<T>() >;


/// `tuple_size_v` is a template variable that contains fields count in a T and
/// works for any T that satisfies \aggregate.
///
/// \b Example:
/// \code
///     std::array<int, boost::pfr::tuple_size_v<my_structure> > a;
/// \endcode
template <class T>
constexpr std::size_t tuple_size_v = tuple_size<T>::value;

}} // namespace boost::pfr

#endif // BOOST_PFR_TUPLE_SIZE_HPP

#include <tuple>

namespace boost { namespace pfr { namespace detail {

/// \brief A `std::tuple` capable of de-structuring assignment used to support
/// a tie of multiple lvalue references to fields of an aggregate T.
///
/// \sa boost::pfr::tie_from_structure
template <typename... Elements>
struct tie_from_structure_tuple : std::tuple<Elements&...> {
    using base = std::tuple<Elements&...>;
    using base::base;

    template <typename T>
    constexpr tie_from_structure_tuple& operator= (T const& t) {
        base::operator=(
            detail::make_stdtiedtuple_from_tietuple(
                detail::tie_as_tuple(t),
                detail::make_index_sequence<tuple_size_v<T>>()));
        return *this;
    }
};

}}} // namespace boost::pfr::detail

#endif // BOOST_PFR_DETAIL_TIE_FROM_STRUCTURE_TUPLE_HPP

#include <type_traits>
#include <utility>      // metaprogramming stuff


/// \file boost/pfr/core.hpp
/// Contains all the basic tuple-like interfaces \forcedlink{get}, \forcedlink{tuple_size}, \forcedlink{tuple_element_t}, and others.
///
/// \b Synopsis:

namespace boost { namespace pfr {

/// \brief Returns reference or const reference to a field with index `I` in \aggregate `val`.
/// Overload taking the type `U` returns reference or const reference to a field
/// with provided type `U` in \aggregate `val` if there's only one field of such type in `val`.
///
/// \b Example:
/// \code
///     struct my_struct { int i, short s; };
///     my_struct s {10, 11};
///
///     assert(boost::pfr::get<0>(s) == 10);
///     boost::pfr::get<1>(s) = 0;
///
///     assert(boost::pfr::get<int>(s) == 10);
///     boost::pfr::get<short>(s) = 11;
/// \endcode
template <std::size_t I, class T>
constexpr decltype(auto) get(const T& val) noexcept {
    return detail::sequence_tuple::get<I>( detail::tie_as_tuple(val) );
}

/// \overload get
template <std::size_t I, class T>
constexpr decltype(auto) get(T& val
#if !BOOST_PFR_USE_CPP17
    , std::enable_if_t<std::is_assignable<T, T>::value>* = nullptr
#endif
) noexcept {
    return detail::sequence_tuple::get<I>( detail::tie_as_tuple(val) );
}

#if !BOOST_PFR_USE_CPP17
/// \overload get
template <std::size_t I, class T>
constexpr auto get(T&, std::enable_if_t<!std::is_assignable<T, T>::value>* = nullptr) noexcept {
    static_assert(sizeof(T) && false, "====================> Boost.PFR: Calling boost::pfr::get on non const non assignable type is allowed only in C++17");
    return 0;
}
#endif


/// \overload get
template <std::size_t I, class T>
constexpr auto get(T&& val, std::enable_if_t< std::is_rvalue_reference<T&&>::value>* = nullptr) noexcept {
    return std::move(detail::sequence_tuple::get<I>( detail::tie_as_tuple(val) ));
}


/// \overload get
template <class U, class T>
constexpr const U& get(const T& val) noexcept {
    return detail::sequence_tuple::get_by_type_impl<const U&>( detail::tie_as_tuple(val) );
}


/// \overload get
template <class U, class T>
constexpr U& get(T& val
#if !BOOST_PFR_USE_CPP17
    , std::enable_if_t<std::is_assignable<T, T>::value>* = nullptr
#endif
) noexcept {
    return detail::sequence_tuple::get_by_type_impl<U&>( detail::tie_as_tuple(val) );
}

#if !BOOST_PFR_USE_CPP17
/// \overload get
template <class U, class T>
constexpr U& get(T&, std::enable_if_t<!std::is_assignable<T, T>::value>* = nullptr) noexcept {
    static_assert(sizeof(T) && false, "====================> Boost.PFR: Calling boost::pfr::get on non const non assignable type is allowed only in C++17");
    return 0;
}
#endif


/// \overload get
template <class U, class T>
constexpr U&& get(T&& val, std::enable_if_t< std::is_rvalue_reference<T&&>::value>* = nullptr) noexcept {
    return std::move(detail::sequence_tuple::get_by_type_impl<U&>( detail::tie_as_tuple(val) ));
}


/// \brief `tuple_element` has a member typedef `type` that returns the type of a field with index I in \aggregate T.
///
/// \b Example:
/// \code
///     std::vector< boost::pfr::tuple_element<0, my_structure>::type > v;
/// \endcode
template <std::size_t I, class T>
using tuple_element = detail::sequence_tuple::tuple_element<I, decltype( ::boost::pfr::detail::tie_as_tuple(std::declval<T&>()) ) >;


/// \brief Type of a field with index `I` in \aggregate `T`.
///
/// \b Example:
/// \code
///     std::vector< boost::pfr::tuple_element_t<0, my_structure> > v;
/// \endcode
template <std::size_t I, class T>
using tuple_element_t = typename tuple_element<I, T>::type;


/// \brief Creates a `std::tuple` from fields of an \aggregate `val`.
///
/// \b Example:
/// \code
///     struct my_struct { int i, short s; };
///     my_struct s {10, 11};
///     std::tuple<int, short> t = boost::pfr::structure_to_tuple(s);
///     assert(get<0>(t) == 10);
/// \endcode
template <class T>
constexpr auto structure_to_tuple(const T& val) noexcept {
    return detail::make_stdtuple_from_tietuple(
        detail::tie_as_tuple(val),
        detail::make_index_sequence< tuple_size_v<T> >()
    );
}


/// \brief std::tie` like function that ties fields of a structure.
///
/// \returns a `std::tuple` with lvalue and const lvalue references to fields of an \aggregate `val`.
///
/// \b Example:
/// \code
///     void foo(const int&, const short&);
///     struct my_struct { int i, short s; };
///
///     const my_struct const_s{1, 2};
///     std::apply(foo, boost::pfr::structure_tie(const_s));
///
///     my_struct s;
///     boost::pfr::structure_tie(s) = std::tuple<int, short>{10, 11};
///     assert(s.s == 11);
/// \endcode
template <class T>
constexpr auto structure_tie(const T& val) noexcept {
    return detail::make_conststdtiedtuple_from_tietuple(
        detail::tie_as_tuple(const_cast<T&>(val)),
        detail::make_index_sequence< tuple_size_v<T> >()
    );
}


/// \overload structure_tie
template <class T>
constexpr auto structure_tie(T& val
#if !BOOST_PFR_USE_CPP17
    , std::enable_if_t<std::is_assignable<T, T>::value>* = nullptr
#endif
) noexcept {
    return detail::make_stdtiedtuple_from_tietuple(
        detail::tie_as_tuple(val),
        detail::make_index_sequence< tuple_size_v<T> >()
    );
}

#if !BOOST_PFR_USE_CPP17
/// \overload structure_tie
template <class T>
constexpr auto structure_tie(T&, std::enable_if_t<!std::is_assignable<T, T>::value>* = nullptr) noexcept {
    static_assert(sizeof(T) && false, "====================> Boost.PFR: Calling boost::pfr::structure_tie on non const non assignable type is allowed only in C++17");
    return 0;
}
#endif


/// \overload structure_tie
template <class T>
constexpr auto structure_tie(T&&, std::enable_if_t< std::is_rvalue_reference<T&&>::value>* = nullptr) noexcept {
    static_assert(sizeof(T) && false, "====================> Boost.PFR: Calling boost::pfr::structure_tie on rvalue references is forbidden");
    return 0;
}

/// Calls `func` for each field of a `value`.
///
/// \param func must have one of the following signatures:
///     * any_return_type func(U&& field)                // field of value is perfect forwarded to function
///     * any_return_type func(U&& field, std::size_t i)
///     * any_return_type func(U&& value, I i)           // Here I is an `std::integral_constant<size_t, field_index>`
///
/// \param value To each field of this variable will be the `func` applied.
///
/// \b Example:
/// \code
///     struct my_struct { int i, short s; };
///     int sum = 0;
///     boost::pfr::for_each_field(my_struct{20, 22}, [&sum](const auto& field) { sum += field; });
///     assert(sum == 42);
/// \endcode
template <class T, class F>
constexpr void for_each_field(T&& value, F&& func) {
    constexpr std::size_t fields_count_val = boost::pfr::detail::fields_count<std::remove_reference_t<T>>();

    ::boost::pfr::detail::for_each_field_dispatcher(
        value,
        [f = std::forward<F>(func)](auto&& t) mutable {
            // MSVC related workaround. Its lambdas do not capture constexprs.
            constexpr std::size_t fields_count_val_in_lambda
                = boost::pfr::detail::fields_count<std::remove_reference_t<T>>();

            ::boost::pfr::detail::for_each_field_impl(
                t,
                std::forward<F>(f),
                detail::make_index_sequence<fields_count_val_in_lambda>{},
                std::is_rvalue_reference<T&&>{}
            );
        },
        detail::make_index_sequence<fields_count_val>{}
    );
}

/// \brief std::tie-like function that allows assigning to tied values from aggregates.
///
/// \returns an object with lvalue references to `args...`; on assignment of an \aggregate value to that
/// object each field of an aggregate is assigned to the corresponding `args...` reference.
///
/// \b Example:
/// \code
///     auto f() {
///       struct { struct { int x, y } p; short s; } res { { 4, 5 }, 6 };
///       return res;
///     }
///     auto [p, s] = f();
///     boost::pfr::tie_from_structure(p, s) = f();
/// \endcode
template <typename... Elements>
constexpr detail::tie_from_structure_tuple<Elements...> tie_from_structure(Elements&... args) noexcept {
    return detail::tie_from_structure_tuple<Elements...>(args...);
}

}} // namespace boost::pfr

#endif // BOOST_PFR_CORE_HPP
// Copyright (c) 2023 Bela Schaum, X-Ryl669, Denis Mikhailov.
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)


// Initial implementation by Bela Schaum, https://github.com/schaumb
// The way to make it union and UB free by X-Ryl669, https://github.com/X-Ryl669
//

#ifndef BOOST_PFR_CORE_NAME_HPP
#define BOOST_PFR_CORE_NAME_HPP


// Copyright (c) 2023 Bela Schaum, X-Ryl669, Denis Mikhailov.
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)


// Initial implementation by Bela Schaum, https://github.com/schaumb
// The way to make it union and UB free by X-Ryl669, https://github.com/X-Ryl669
//

#ifndef BOOST_PFR_DETAIL_CORE_NAME_HPP
#define BOOST_PFR_DETAIL_CORE_NAME_HPP


// Each core_name provides `boost::pfr::detail::get_name` and
// `boost::pfr::detail::tie_as_names_tuple` functions.
//
// The whole functional of extracting field's names is build on top of those
// two functions.
#if BOOST_PFR_CORE_NAME_ENABLED
// Copyright (c) 2023 Bela Schaum, X-Ryl669, Denis Mikhailov.
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)


// Initial implementation by Bela Schaum, https://github.com/schaumb
// The way to make it union and UB free by X-Ryl669, https://github.com/X-Ryl669
//

#ifndef BOOST_PFR_DETAIL_CORE_NAME20_STATIC_HPP
#define BOOST_PFR_DETAIL_CORE_NAME20_STATIC_HPP

// Copyright (c) 2023 Denis Mikhailov
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_DETAIL_STDARRAY_HPP
#define BOOST_PFR_DETAIL_STDARRAY_HPP


#include <utility> // metaprogramming stuff
#include <array>
#include <type_traits> // for std::common_type_t
#include <cstddef>


namespace boost { namespace pfr { namespace detail {

template <class... Types>
constexpr auto make_stdarray(const Types&... t) noexcept {
    return std::array<std::common_type_t<Types...>, sizeof...(Types)>{t...};
}

template <class T, std::size_t... I>
constexpr auto make_stdarray_from_tietuple(const T& t, std::index_sequence<I...>, int) noexcept {
    return detail::make_stdarray(
        boost::pfr::detail::sequence_tuple::get<I>(t)...
    );
}

template <class T>
constexpr auto make_stdarray_from_tietuple(const T&, std::index_sequence<>, long) noexcept {
    return std::array<std::nullptr_t, 0>{};
}

}}} // namespace boost::pfr::detail

#endif // BOOST_PFR_DETAIL_STDARRAY_HPP

// Copyright (c) 2023 Bela Schaum, X-Ryl669, Denis Mikhailov.
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)


// Initial implementation by Bela Schaum, https://github.com/schaumb
// The way to make it union and UB free by X-Ryl669, https://github.com/X-Ryl669
//

#ifndef BOOST_PFR_DETAIL_FAKE_OBJECT_HPP
#define BOOST_PFR_DETAIL_FAKE_OBJECT_HPP


namespace boost { namespace pfr { namespace detail {

template <class T>
extern const T fake_object;

}}} // namespace boost::pfr::detail

#endif // BOOST_PFR_DETAIL_FAKE_OBJECT_HPP

#include <type_traits>
#include <string_view>
#include <array>
#include <memory> // for std::addressof

namespace boost { namespace pfr { namespace detail {

struct core_name_skip {
    std::size_t size_at_begin;
    std::size_t size_at_end;
    bool is_backward;
    std::string_view until_runtime;

    consteval std::string_view apply(std::string_view sv) const noexcept {
        // We use std::min here to make the compiler diagnostic shorter and
        // cleaner in case of misconfigured BOOST_PFR_CORE_NAME_PARSING
        sv.remove_prefix((std::min)(size_at_begin, sv.size()));
        sv.remove_suffix((std::min)(size_at_end, sv.size()));
        if (until_runtime.empty()) {
            return sv;
        }

        const auto found = is_backward ? sv.rfind(until_runtime)
                                       : sv.find(until_runtime);

        const auto cut_until = found + until_runtime.size();
        const auto safe_cut_until = (std::min)(cut_until, sv.size());
        return sv.substr(safe_cut_until);
    }
};

struct backward {
    explicit consteval backward(std::string_view value) noexcept
        : value(value)
    {}

    std::string_view value;
};

consteval core_name_skip make_core_name_skip(std::size_t size_at_begin,
                                             std::size_t size_at_end,
                                             std::string_view until_runtime) noexcept
{
    return core_name_skip{size_at_begin, size_at_end, false, until_runtime};
}

consteval core_name_skip make_core_name_skip(std::size_t size_at_begin,
                                             std::size_t size_at_end,
                                             backward until_runtime) noexcept
{
    return core_name_skip{size_at_begin, size_at_end, true, until_runtime.value};
}

// it might be compilation failed without this workaround sometimes
// See https://github.com/llvm/llvm-project/issues/41751 for details
template <class>
consteval std::string_view clang_workaround(std::string_view value) noexcept
{
    return value;
}

template <class MsvcWorkaround, auto ptr>
consteval auto name_of_field_impl() noexcept {
    // Some of the following compiler specific macro may be defined only
    // inside the function body:

#ifndef BOOST_PFR_FUNCTION_SIGNATURE
#   if defined(__FUNCSIG__)
#       define BOOST_PFR_FUNCTION_SIGNATURE __FUNCSIG__
#   elif defined(__PRETTY_FUNCTION__) || defined(__GNUC__) || defined(__clang__)
#       define BOOST_PFR_FUNCTION_SIGNATURE __PRETTY_FUNCTION__
#   else
#       define BOOST_PFR_FUNCTION_SIGNATURE ""
#   endif
#endif

    constexpr std::string_view sv = detail::clang_workaround<MsvcWorkaround>(BOOST_PFR_FUNCTION_SIGNATURE);
    static_assert(!sv.empty(),
        "====================> Boost.PFR: Field reflection parser configured in a wrong way. "
        "Please define the BOOST_PFR_FUNCTION_SIGNATURE to a compiler specific macro, "
        "that outputs the whole function signature including non-type template parameters."  
    );

    constexpr auto skip = detail::make_core_name_skip BOOST_PFR_CORE_NAME_PARSING;
    static_assert(skip.size_at_begin + skip.size_at_end + skip.until_runtime.size() < sv.size(),
        "====================> Boost.PFR: Field reflection parser configured in a wrong way. "
        "It attempts to skip more chars than available. "
        "Please define BOOST_PFR_CORE_NAME_PARSING to correct values. See documentation section "
        "'Limitations and Configuration' for more information."
    );
    constexpr auto fn = skip.apply(sv);
    static_assert(
        !fn.empty(),
        "====================> Boost.PFR: Extraction of field name is misconfigured for your compiler. "
        "It skipped all the input, leaving the field name empty. "
        "Please define BOOST_PFR_CORE_NAME_PARSING to correct values. See documentation section "
        "'Limitations and Configuration' for more information."
    );
    auto res = std::array<char, fn.size()+1>{};

    auto* out = res.data();
    for (auto x: fn) {
        *out = x;
        ++out;
    }

    return res;
}

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wundefined-var-template"

// clang 16 and earlier don't support address of non-static member as template parameter
// but fortunately it's possible to use C++20 non-type template parameters in another way
// even in clang 16 and more older clangs
// all we need is to wrap pointer into 'clang_wrapper_t' and then pass it into template
template <class T>
struct clang_wrapper_t {
    T v;
};
template <class T>
clang_wrapper_t(T) -> clang_wrapper_t<T>;

template <class T>
constexpr auto make_clang_wrapper(const T& arg) noexcept {
    return clang_wrapper_t{arg};
}

#else

template <class T>
constexpr const T& make_clang_wrapper(const T& arg) noexcept {
    // It's everything OK with address of non-static member as template parameter support on this compiler
    // so we don't need a wrapper here, just pass the pointer into template
    return arg;
}

#endif

template <class MsvcWorkaround, auto ptr>
consteval auto name_of_field() noexcept {
    // Sanity check: known field name must match the deduced one
    static_assert(
        sizeof(MsvcWorkaround)  // do not trigger if `name_of_field()` is not used
        && std::string_view{
            detail::name_of_field_impl<
                core_name_skip, detail::make_clang_wrapper(std::addressof(
                    fake_object<core_name_skip>.size_at_begin
                ))
            >().data()
        } == "size_at_begin",
        "====================> Boost.PFR: Extraction of field name is misconfigured for your compiler. "
        "It does not return the proper field name. "
        "Please define BOOST_PFR_CORE_NAME_PARSING to correct values. See documentation section "
        "'Limitations and Configuration' for more information."
    );

    return detail::name_of_field_impl<MsvcWorkaround, ptr>();
}

// Storing part of a string literal into an array minimizes the binary size.
//
// Without passing 'T' into 'name_of_field' different fields from different structures might have the same name!
// See https://developercommunity.visualstudio.com/t/__FUNCSIG__-outputs-wrong-value-with-C/10458554 for details
template <class T, std::size_t I>
inline constexpr auto stored_name_of_field = detail::name_of_field<T,
    detail::make_clang_wrapper(std::addressof(detail::sequence_tuple::get<I>(
        detail::tie_as_tuple(detail::fake_object<T>)
    )))
>();

#ifdef __clang__
#pragma clang diagnostic pop
#endif

template <class T, std::size_t... I>
constexpr auto tie_as_names_tuple_impl(std::index_sequence<I...>) noexcept {
    return detail::sequence_tuple::make_sequence_tuple(std::string_view{stored_name_of_field<T, I>.data()}...);
}

template <class T, std::size_t I>
constexpr std::string_view get_name() noexcept {
    static_assert(
        !std::is_union<T>::value,
        "====================> Boost.PFR: For safety reasons it is forbidden to reflect unions. See `Reflection of unions` section in the docs for more info."
    );
    static_assert(
        !std::is_array<T>::value,
        "====================> Boost.PFR: It is impossible to extract name from old C array since it doesn't have named members"
    );
    static_assert(
        sizeof(T) && BOOST_PFR_USE_CPP17,
        "====================> Boost.PFR: Extraction of field's names is allowed only when the BOOST_PFR_USE_CPP17 macro enabled."
   );

   return stored_name_of_field<T, I>.data();
}

template <class T>
constexpr auto tie_as_names_tuple() noexcept {
    static_assert(
        !std::is_union<T>::value,
        "====================> Boost.PFR: For safety reasons it is forbidden to reflect unions. See `Reflection of unions` section in the docs for more info."
    );
    static_assert(
        !std::is_array<T>::value,
        "====================> Boost.PFR: It is impossible to extract name from old C array since it doesn't have named members"
    );
    static_assert(
        sizeof(T) && BOOST_PFR_USE_CPP17,
        "====================> Boost.PFR: Extraction of field's names is allowed only when the BOOST_PFR_USE_CPP17 macro enabled."
    );

    return detail::tie_as_names_tuple_impl<T>(detail::make_index_sequence<detail::fields_count<T>()>{});
}

}}} // namespace boost::pfr::detail

#endif // BOOST_PFR_DETAIL_CORE_NAME20_STATIC_HPP

#else
// Copyright (c) 2023 Bela Schaum, X-Ryl669, Denis Mikhailov.
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)


// Initial implementation by Bela Schaum, https://github.com/schaumb
// The way to make it union and UB free by X-Ryl669, https://github.com/X-Ryl669
//

#ifndef BOOST_PFR_DETAIL_CORE_NAME14_DISABLED_HPP
#define BOOST_PFR_DETAIL_CORE_NAME14_DISABLED_HPP


namespace boost { namespace pfr { namespace detail {

template <class T, std::size_t I>
constexpr auto get_name() noexcept {
    static_assert(
        sizeof(T) && false,
        "====================> Boost.PFR: Field's names extracting functionality requires C++20."
    );

    return nullptr;
}

template <class T>
constexpr auto tie_as_names_tuple() noexcept {
    static_assert(
        sizeof(T) && false,
        "====================> Boost.PFR: Field's names extracting functionality requires C++20."
    );

    return detail::sequence_tuple::make_sequence_tuple();
}

}}} // namespace boost::pfr::detail

#endif // BOOST_PFR_DETAIL_CORE_NAME14_DISABLED_HPP

#endif

#endif // BOOST_PFR_DETAIL_CORE_NAME_HPP



#include <cstddef> // for std::size_t


/// \file boost/pfr/core_name.hpp
/// Contains functions \forcedlink{get_name} and \forcedlink{names_as_array} to know which names each field of any \aggregate has.
///
/// \fnrefl for details.
///
/// \b Synopsis:

namespace boost { namespace pfr {

/// \brief Returns name of a field with index `I` in \aggregate `T`.
///
/// \b Example:
/// \code
///     struct my_struct { int i, short s; };
///
///     assert(boost::pfr::get_name<0, my_struct>() == "i");
///     assert(boost::pfr::get_name<1, my_struct>() == "s");
/// \endcode
template <std::size_t I, class T>
constexpr
#ifdef BOOST_PFR_DOXYGEN_INVOKED
std::string_view
#else
auto
#endif
get_name() noexcept {
    return detail::get_name<T, I>();
}

// FIXME: implement this
// template<class U, class T>
// constexpr auto get_name() noexcept {
//     return detail::sequence_tuple::get_by_type_impl<U>( detail::tie_as_names_tuple<T>() );
// }

/// \brief Creates a `std::array` from names of fields of an \aggregate `T`.
///
/// \b Example:
/// \code
///     struct my_struct { int i, short s; };
///     std::array<std::string_view, 2> a = boost::pfr::names_as_array<my_struct>();
///     assert(a[0] == "i");
/// \endcode
template <class T>
constexpr
#ifdef BOOST_PFR_DOXYGEN_INVOKED
std::array<std::string_view, boost::pfr::tuple_size_v<T>>
#else
auto
#endif
names_as_array() noexcept {
    return detail::make_stdarray_from_tietuple(
        detail::tie_as_names_tuple<T>(),
        detail::make_index_sequence< tuple_size_v<T> >(),
        1L
    );
}

}} // namespace boost::pfr

#endif // BOOST_PFR_CORE_NAME_HPP

// Copyright (c) 2016-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_FUNCTIONS_FOR_HPP
#define BOOST_PFR_FUNCTIONS_FOR_HPP


// Copyright (c) 2016-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_OPS_FIELDS_HPP
#define BOOST_PFR_OPS_FIELDS_HPP


// Copyright (c) 2016-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_DETAIL_FUNCTIONAL_HPP
#define BOOST_PFR_DETAIL_FUNCTIONAL_HPP


#include <functional>
#include <cstdint>


namespace boost { namespace pfr { namespace detail {
    template <std::size_t I, std::size_t N>
    struct equal_impl {
        template <class T, class U>
        constexpr static bool cmp(const T& v1, const U& v2) noexcept {
            return ::boost::pfr::detail::sequence_tuple::get<I>(v1) == ::boost::pfr::detail::sequence_tuple::get<I>(v2)
                && equal_impl<I + 1, N>::cmp(v1, v2);
        }
    };

    template <std::size_t N>
    struct equal_impl<N, N> {
        template <class T, class U>
        constexpr static bool cmp(const T&, const U&) noexcept {
            return T::size_v == U::size_v;
        }
    };

    template <std::size_t I, std::size_t N>
    struct not_equal_impl {
        template <class T, class U>
        constexpr static bool cmp(const T& v1, const U& v2) noexcept {
            return ::boost::pfr::detail::sequence_tuple::get<I>(v1) != ::boost::pfr::detail::sequence_tuple::get<I>(v2)
                || not_equal_impl<I + 1, N>::cmp(v1, v2);
        }
    };

    template <std::size_t N>
    struct not_equal_impl<N, N> {
        template <class T, class U>
        constexpr static bool cmp(const T&, const U&) noexcept {
            return T::size_v != U::size_v;
        }
    };

    template <std::size_t I, std::size_t N>
    struct less_impl {
        template <class T, class U>
        constexpr static bool cmp(const T& v1, const U& v2) noexcept {
            return sequence_tuple::get<I>(v1) < sequence_tuple::get<I>(v2)
                || (sequence_tuple::get<I>(v1) == sequence_tuple::get<I>(v2) && less_impl<I + 1, N>::cmp(v1, v2));
        }
    };

    template <std::size_t N>
    struct less_impl<N, N> {
        template <class T, class U>
        constexpr static bool cmp(const T&, const U&) noexcept {
            return T::size_v < U::size_v;
        }
    };

    template <std::size_t I, std::size_t N>
    struct less_equal_impl {
        template <class T, class U>
        constexpr static bool cmp(const T& v1, const U& v2) noexcept {
            return sequence_tuple::get<I>(v1) < sequence_tuple::get<I>(v2)
                || (sequence_tuple::get<I>(v1) == sequence_tuple::get<I>(v2) && less_equal_impl<I + 1, N>::cmp(v1, v2));
        }
    };

    template <std::size_t N>
    struct less_equal_impl<N, N> {
        template <class T, class U>
        constexpr static bool cmp(const T&, const U&) noexcept {
            return T::size_v <= U::size_v;
        }
    };

    template <std::size_t I, std::size_t N>
    struct greater_impl {
        template <class T, class U>
        constexpr static bool cmp(const T& v1, const U& v2) noexcept {
            return sequence_tuple::get<I>(v1) > sequence_tuple::get<I>(v2)
                || (sequence_tuple::get<I>(v1) == sequence_tuple::get<I>(v2) && greater_impl<I + 1, N>::cmp(v1, v2));
        }
    };

    template <std::size_t N>
    struct greater_impl<N, N> {
        template <class T, class U>
        constexpr static bool cmp(const T&, const U&) noexcept {
            return T::size_v > U::size_v;
        }
    };

    template <std::size_t I, std::size_t N>
    struct greater_equal_impl {
        template <class T, class U>
        constexpr static bool cmp(const T& v1, const U& v2) noexcept {
            return sequence_tuple::get<I>(v1) > sequence_tuple::get<I>(v2)
                || (sequence_tuple::get<I>(v1) == sequence_tuple::get<I>(v2) && greater_equal_impl<I + 1, N>::cmp(v1, v2));
        }
    };

    template <std::size_t N>
    struct greater_equal_impl<N, N> {
        template <class T, class U>
        constexpr static bool cmp(const T&, const U&) noexcept {
            return T::size_v >= U::size_v;
        }
    };

    // Hash combine functions copied from Boost.ContainerHash
    // https://github.com/boostorg/container_hash/blob/171c012d4723c5e93cc7cffe42919afdf8b27dfa/include/boost/container_hash/hash.hpp#L311
    // that is based on Peter Dimov's proposal
    // http://www.open-std.org/JTC1/SC22/WG21/docs/papers/2005/n1756.pdf
    // issue 6.18.
    //
    // This also contains public domain code from MurmurHash. From the
    // MurmurHash header:
    //
    // MurmurHash3 was written by Austin Appleby, and is placed in the public
    // domain. The author hereby disclaims copyright to this source code.
    template <typename SizeT>
    constexpr void hash_combine(SizeT& seed, SizeT value) noexcept {
        seed ^= value + 0x9e3779b9 + (seed<<6) + (seed>>2);
    }

    constexpr auto rotl(std::uint32_t x, std::uint32_t r) noexcept {
        return (x << r) | (x >> (32 - r));
    }

    constexpr void hash_combine(std::uint32_t& h1, std::uint32_t k1) noexcept {
          const std::uint32_t c1 = 0xcc9e2d51;
          const std::uint32_t c2 = 0x1b873593;

          k1 *= c1;
          k1 = detail::rotl(k1,15);
          k1 *= c2;

          h1 ^= k1;
          h1 = detail::rotl(h1,13);
          h1 = h1*5+0xe6546b64;
    }

#if defined(INT64_MIN) && defined(UINT64_MAX)
    constexpr void hash_combine(std::uint64_t& h, std::uint64_t k) noexcept {
        const std::uint64_t m = 0xc6a4a7935bd1e995ULL;
        const int r = 47;

        k *= m;
        k ^= k >> r;
        k *= m;

        h ^= k;
        h *= m;

        // Completely arbitrary number, to prevent 0's
        // from hashing to 0.
        h += 0xe6546b64;
    }
#endif

    template <typename T>
    auto compute_hash(const T& value, long /*priority*/)
        -> decltype(std::hash<T>()(value))
    {
        return std::hash<T>()(value);
    }

    template <typename T>
    std::size_t compute_hash(const T& /*value*/, int /*priority*/) {
        static_assert(sizeof(T) && false, "====================> Boost.PFR: std::hash not specialized for type T");
        return 0;
    }

    template <std::size_t I, std::size_t N>
    struct hash_impl {
        template <class T>
        constexpr static std::size_t compute(const T& val) noexcept {
            std::size_t h = detail::compute_hash( ::boost::pfr::detail::sequence_tuple::get<I>(val), 1L );
            detail::hash_combine(h, hash_impl<I + 1, N>::compute(val) );
            return h;
        }
    };

    template <std::size_t N>
    struct hash_impl<N, N> {
        template <class T>
        constexpr static std::size_t compute(const T&) noexcept {
            return 0;
        }
    };

///////////////////// Define min_element and to avoid inclusion of <algorithm>
    constexpr std::size_t min_size(std::size_t x, std::size_t y) noexcept {
        return x < y ? x : y;
    }

    template <template <std::size_t, std::size_t> class Visitor, class T, class U>
    constexpr bool binary_visit(const T& x, const U& y) {
        constexpr std::size_t fields_count_lhs = detail::fields_count<std::remove_reference_t<T>>();
        constexpr std::size_t fields_count_rhs = detail::fields_count<std::remove_reference_t<U>>();
        constexpr std::size_t fields_count_min = detail::min_size(fields_count_lhs, fields_count_rhs);
        typedef Visitor<0, fields_count_min> visitor_t;

#if BOOST_PFR_USE_CPP17 || BOOST_PFR_USE_LOOPHOLE
        return visitor_t::cmp(detail::tie_as_tuple(x), detail::tie_as_tuple(y));
#else
        bool result = true;
        ::boost::pfr::detail::for_each_field_dispatcher(
            x,
            [&result, &y](const auto& lhs) {
                constexpr std::size_t fields_count_rhs_ = detail::fields_count<std::remove_reference_t<U>>();
                ::boost::pfr::detail::for_each_field_dispatcher(
                    y,
                    [&result, &lhs](const auto& rhs) {
                        result = visitor_t::cmp(lhs, rhs);
                    },
                    detail::make_index_sequence<fields_count_rhs_>{}
                );
            },
            detail::make_index_sequence<fields_count_lhs>{}
        );

        return result;
#endif
    }

}}} // namespace boost::pfr::detail

#endif // BOOST_PFR_DETAIL_FUNCTIONAL_HPP

/// \file boost/pfr/ops_fields.hpp
/// Contains field-by-fields comparison and hash functions.
///
/// \b Example:
/// \code
///     #include <boost/pfr/ops_fields.hpp>
///     struct comparable_struct {      // No operators defined for that structure
///         int i; short s;
///     };
///     // ...
///
///     comparable_struct s1 {0, 1};
///     comparable_struct s2 {0, 2};
///     assert(boost::pfr::lt_fields(s1, s2));
/// \endcode
///
/// \podops for other ways to define operators and more details.
///
/// \b Synopsis:
namespace boost { namespace pfr {

    /// Does a field-by-field equality comparison.
    ///
    /// \returns `L == R && tuple_size_v<T> == tuple_size_v<U>`, where `L` and
    /// `R` are the results of calling `std::tie` on first `N` fields of `lhs` and
    // `rhs` respectively; `N` is `std::min(tuple_size_v<T>, tuple_size_v<U>)`.
    template <class T, class U>
    constexpr bool eq_fields(const T& lhs, const U& rhs) noexcept {
        return detail::binary_visit<detail::equal_impl>(lhs, rhs);
    }


    /// Does a field-by-field inequality comparison.
    ///
    /// \returns `L != R || tuple_size_v<T> != tuple_size_v<U>`, where `L` and
    /// `R` are the results of calling `std::tie` on first `N` fields of `lhs` and
    // `rhs` respectively; `N` is `std::min(tuple_size_v<T>, tuple_size_v<U>)`.
    template <class T, class U>
    constexpr bool ne_fields(const T& lhs, const U& rhs) noexcept {
        return detail::binary_visit<detail::not_equal_impl>(lhs, rhs);
    }

    /// Does a field-by-field greter comparison.
    ///
    /// \returns `L > R || (L == R && tuple_size_v<T> > tuple_size_v<U>)`, where `L` and
    /// `R` are the results of calling `std::tie` on first `N` fields of `lhs` and
    // `rhs` respectively; `N` is `std::min(tuple_size_v<T>, tuple_size_v<U>)`.
    template <class T, class U>
    constexpr bool gt_fields(const T& lhs, const U& rhs) noexcept {
        return detail::binary_visit<detail::greater_impl>(lhs, rhs);
    }


    /// Does a field-by-field less comparison.
    ///
    /// \returns `L < R || (L == R && tuple_size_v<T> < tuple_size_v<U>)`, where `L` and
    /// `R` are the results of calling `std::tie` on first `N` fields of `lhs` and
    // `rhs` respectively; `N` is `std::min(tuple_size_v<T>, tuple_size_v<U>)`.
    template <class T, class U>
    constexpr bool lt_fields(const T& lhs, const U& rhs) noexcept {
        return detail::binary_visit<detail::less_impl>(lhs, rhs);
    }


    /// Does a field-by-field greater equal comparison.
    ///
    /// \returns `L > R || (L == R && tuple_size_v<T> >= tuple_size_v<U>)`, where `L` and
    /// `R` are the results of calling `std::tie` on first `N` fields of `lhs` and
    // `rhs` respectively; `N` is `std::min(tuple_size_v<T>, tuple_size_v<U>)`.
    template <class T, class U>
    constexpr bool ge_fields(const T& lhs, const U& rhs) noexcept {
        return detail::binary_visit<detail::greater_equal_impl>(lhs, rhs);
    }


    /// Does a field-by-field less equal comparison.
    ///
    /// \returns `L < R || (L == R && tuple_size_v<T> <= tuple_size_v<U>)`, where `L` and
    /// `R` are the results of calling `std::tie` on first `N` fields of `lhs` and
    // `rhs` respectively; `N` is `std::min(tuple_size_v<T>, tuple_size_v<U>)`.
    template <class T, class U>
    constexpr bool le_fields(const T& lhs, const U& rhs) noexcept {
        return detail::binary_visit<detail::less_equal_impl>(lhs, rhs);
    }


    /// Does a field-by-field hashing.
    ///
    /// \returns combined hash of all the fields
    template <class T>
    std::size_t hash_fields(const T& x) {
        constexpr std::size_t fields_count_val = boost::pfr::detail::fields_count<std::remove_reference_t<T>>();
#if BOOST_PFR_USE_CPP17 || BOOST_PFR_USE_LOOPHOLE
        return detail::hash_impl<0, fields_count_val>::compute(detail::tie_as_tuple(x));
#else
        std::size_t result = 0;
        ::boost::pfr::detail::for_each_field_dispatcher(
            x,
            [&result](const auto& lhs) {
                // We can not reuse `fields_count_val` in lambda because compilers had issues with
                // passing constexpr variables into lambdas. Computing is again is the most portable solution.
                constexpr std::size_t fields_count_val_lambda = boost::pfr::detail::fields_count<std::remove_reference_t<T>>();
                result = detail::hash_impl<0, fields_count_val_lambda>::compute(lhs);
            },
            detail::make_index_sequence<fields_count_val>{}
        );

        return result;
#endif
    }
}} // namespace boost::pfr

#endif // BOOST_PFR_OPS_HPP
// Copyright (c) 2016-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)


#ifndef BOOST_PFR_IO_FIELDS_HPP
#define BOOST_PFR_IO_FIELDS_HPP



#include <type_traits>
#include <utility>      // metaprogramming stuff

// Copyright (c) 2016-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_DETAIL_IO_HPP
#define BOOST_PFR_DETAIL_IO_HPP


#include <iosfwd>       // stream operators
#include <iomanip>

#if defined(__has_include)
#   if __has_include(<string_view>) && BOOST_PFR_USE_CPP17
#       include <string_view>
#   endif
#endif

namespace boost { namespace pfr { namespace detail {

inline auto quoted_helper(const std::string& s) noexcept {
    return std::quoted(s);
}

#if defined(__has_include)
#   if __has_include(<string_view>) && BOOST_PFR_USE_CPP17
template <class CharT, class Traits>
inline auto quoted_helper(std::basic_string_view<CharT, Traits> s) noexcept {
    return std::quoted(s);
}
#   endif
#endif

inline auto quoted_helper(std::string& s) noexcept {
    return std::quoted(s);
}

template <class T>
inline decltype(auto) quoted_helper(T&& v) noexcept {
    return std::forward<T>(v);
}

template <std::size_t I, std::size_t N>
struct print_impl {
    template <class Stream, class T>
    static void print (Stream& out, const T& value) {
        if (!!I) out << ", ";
        out << detail::quoted_helper(boost::pfr::detail::sequence_tuple::get<I>(value));
        print_impl<I + 1, N>::print(out, value);
    }
};

template <std::size_t I>
struct print_impl<I, I> {
    template <class Stream, class T> static void print (Stream&, const T&) noexcept {}
};


template <std::size_t I, std::size_t N>
struct read_impl {
    template <class Stream, class T>
    static void read (Stream& in, const T& value) {
        char ignore = {};
        if (!!I) {
            in >> ignore;
            if (ignore != ',') in.setstate(Stream::failbit);
            in >> ignore;
            if (ignore != ' ')  in.setstate(Stream::failbit);
        }
        in >> detail::quoted_helper( boost::pfr::detail::sequence_tuple::get<I>(value) );
        read_impl<I + 1, N>::read(in, value);
    }
};

template <std::size_t I>
struct read_impl<I, I> {
    template <class Stream, class T> static void read (Stream&, const T&) {}
};

}}} // namespace boost::pfr::detail

#endif // BOOST_PFR_DETAIL_IO_HPP

/// \file boost/pfr/io_fields.hpp
/// Contains IO manipulator \forcedlink{io_fields} to read/write any \aggregate field-by-field.
///
/// \b Example:
/// \code
///     struct my_struct {
///         int i;
///         short s;
///     };
///
///     std::ostream& operator<<(std::ostream& os, const my_struct& x) {
///         return os << boost::pfr::io_fields(x);  // Equivalent to: os << "{ " << x.i << " ," <<  x.s << " }"
///     }
///
///     std::istream& operator>>(std::istream& is, my_struct& x) {
///         return is >> boost::pfr::io_fields(x);  // Equivalent to: is >> "{ " >> x.i >> " ," >>  x.s >> " }"
///     }
/// \endcode
///
/// \podops for other ways to define operators and more details.
///
/// \b Synopsis:

namespace boost { namespace pfr {

namespace detail {

template <class T>
struct io_fields_impl {
    T value;
};


template <class Char, class Traits, class T>
std::basic_ostream<Char, Traits>& operator<<(std::basic_ostream<Char, Traits>& out, io_fields_impl<const T&>&& x) {
    const T& value = x.value;
    constexpr std::size_t fields_count_val = boost::pfr::detail::fields_count<T>();
    out << '{';
#if BOOST_PFR_USE_CPP17 || BOOST_PFR_USE_LOOPHOLE
    detail::print_impl<0, fields_count_val>::print(out, detail::tie_as_tuple(value));
#else
    ::boost::pfr::detail::for_each_field_dispatcher(
        value,
        [&out](const auto& val) {
            // We can not reuse `fields_count_val` in lambda because compilers had issues with
            // passing constexpr variables into lambdas. Computing is again is the most portable solution.
            constexpr std::size_t fields_count_val_lambda = boost::pfr::detail::fields_count<T>();
            detail::print_impl<0, fields_count_val_lambda>::print(out, val);
        },
        detail::make_index_sequence<fields_count_val>{}
    );
#endif
    return out << '}';
}


template <class Char, class Traits, class T>
std::basic_ostream<Char, Traits>& operator<<(std::basic_ostream<Char, Traits>& out, io_fields_impl<T>&& x) {
    return out << io_fields_impl<const std::remove_reference_t<T>&>{x.value};
}

template <class Char, class Traits, class T>
std::basic_istream<Char, Traits>& operator>>(std::basic_istream<Char, Traits>& in, io_fields_impl<T&>&& x) {
    T& value = x.value;
    constexpr std::size_t fields_count_val = boost::pfr::detail::fields_count<T>();

    const auto prev_exceptions = in.exceptions();
    in.exceptions( typename std::basic_istream<Char, Traits>::iostate(0) );
    const auto prev_flags = in.flags( typename std::basic_istream<Char, Traits>::fmtflags(0) );

    char parenthis = {};
    in >> parenthis;
    if (parenthis != '{') in.setstate(std::basic_istream<Char, Traits>::failbit);

#if BOOST_PFR_USE_CPP17 || BOOST_PFR_USE_LOOPHOLE
    detail::read_impl<0, fields_count_val>::read(in, detail::tie_as_tuple(value));
#else
    ::boost::pfr::detail::for_each_field_dispatcher(
        value,
        [&in](const auto& val) {
            // We can not reuse `fields_count_val` in lambda because compilers had issues with
            // passing constexpr variables into lambdas. Computing is again is the most portable solution.
            constexpr std::size_t fields_count_val_lambda = boost::pfr::detail::fields_count<T>();
            detail::read_impl<0, fields_count_val_lambda>::read(in, val);
        },
        detail::make_index_sequence<fields_count_val>{}
    );
#endif

    in >> parenthis;
    if (parenthis != '}') in.setstate(std::basic_istream<Char, Traits>::failbit);

    in.flags(prev_flags);
    in.exceptions(prev_exceptions);

    return in;
}

template <class Char, class Traits, class T>
std::basic_istream<Char, Traits>& operator>>(std::basic_istream<Char, Traits>& in, io_fields_impl<const T&>&& ) {
    static_assert(sizeof(T) && false, "====================> Boost.PFR: Attempt to use istream operator on a boost::pfr::io_fields wrapped type T with const qualifier.");
    return in;
}

template <class Char, class Traits, class T>
std::basic_istream<Char, Traits>& operator>>(std::basic_istream<Char, Traits>& in, io_fields_impl<T>&& ) {
    static_assert(sizeof(T) && false, "====================> Boost.PFR: Attempt to use istream operator on a boost::pfr::io_fields wrapped temporary of type T.");
    return in;
}

} // namespace detail

/// IO manipulator to read/write \aggregate `value` field-by-field.
///
/// \b Example:
/// \code
///     struct my_struct {
///         int i;
///         short s;
///     };
///
///     std::ostream& operator<<(std::ostream& os, const my_struct& x) {
///         return os << boost::pfr::io_fields(x);  // Equivalent to: os << "{ " << x.i << " ," <<  x.s << " }"
///     }
///
///     std::istream& operator>>(std::istream& is, my_struct& x) {
///         return is >> boost::pfr::io_fields(x);  // Equivalent to: is >> "{ " >> x.i >> " ," >>  x.s >> " }"
///     }
/// \endcode
///
/// Input and output streaming operators for `boost::pfr::io_fields` are symmetric, meaning that you get the original value by streaming it and
/// reading back if each fields streaming operator is symmetric.
///
/// \customio
template <class T>
auto io_fields(T&& value) noexcept {
    return detail::io_fields_impl<T>{std::forward<T>(value)};
}

}} // namespace boost::pfr

#endif // BOOST_PFR_IO_FIELDS_HPP

/// \file boost/pfr/functions_for.hpp
/// Contains BOOST_PFR_FUNCTIONS_FOR macro that defined comparison and stream operators for T along with hash_value function.
/// \b Example:
/// \code
///     #include <boost/pfr/functions_for.hpp>
///
///     namespace my_namespace {
///         struct my_struct {      // No operators defined for that structure
///             int i; short s; char data[7]; bool bl; int a,b,c,d,e,f;
///         };
///         BOOST_PFR_FUNCTIONS_FOR(my_struct)
///     }
/// \endcode
///
/// \podops for other ways to define operators and more details.
///
/// \b Synopsis:

/// \def BOOST_PFR_FUNCTIONS_FOR(T)
/// Defines comparison and stream operators for T along with hash_value function.
///
/// \b Example:
/// \code
///     #include <boost/pfr/functions_for.hpp>
///     struct comparable_struct {      // No operators defined for that structure
///         int i; short s; char data[7]; bool bl; int a,b,c,d,e,f;
///     };
///     BOOST_PFR_FUNCTIONS_FOR(comparable_struct)
///     // ...
///
///     comparable_struct s1 {0, 1, "Hello", false, 6,7,8,9,10,11};
///     comparable_struct s2 {0, 1, "Hello", false, 6,7,8,9,10,11111};
///     assert(s1 < s2);
///     std::cout << s1 << std::endl; // Outputs: {0, 1, H, e, l, l, o, , , 0, 6, 7, 8, 9, 10, 11}
/// \endcode
///
/// \podops for other ways to define operators and more details.
///
/// \b Defines \b following \b for \b T:
/// \code
/// bool operator==(const T& lhs, const T& rhs);
/// bool operator!=(const T& lhs, const T& rhs);
/// bool operator< (const T& lhs, const T& rhs);
/// bool operator> (const T& lhs, const T& rhs);
/// bool operator<=(const T& lhs, const T& rhs);
/// bool operator>=(const T& lhs, const T& rhs);
///
/// template <class Char, class Traits>
/// std::basic_ostream<Char, Traits>& operator<<(std::basic_ostream<Char, Traits>& out, const T& value);
///
/// template <class Char, class Traits>
/// std::basic_istream<Char, Traits>& operator>>(std::basic_istream<Char, Traits>& in, T& value);
///
/// // helper function for Boost unordered containers and boost::hash<>.
/// std::size_t hash_value(const T& value);
/// \endcode

#define BOOST_PFR_FUNCTIONS_FOR(T)                                                                                                          \
    BOOST_PFR_MAYBE_UNUSED inline bool operator==(const T& lhs, const T& rhs) { return ::boost::pfr::eq_fields(lhs, rhs); }                 \
    BOOST_PFR_MAYBE_UNUSED inline bool operator!=(const T& lhs, const T& rhs) { return ::boost::pfr::ne_fields(lhs, rhs); }                 \
    BOOST_PFR_MAYBE_UNUSED inline bool operator< (const T& lhs, const T& rhs) { return ::boost::pfr::lt_fields(lhs, rhs); }                 \
    BOOST_PFR_MAYBE_UNUSED inline bool operator> (const T& lhs, const T& rhs) { return ::boost::pfr::gt_fields(lhs, rhs); }                 \
    BOOST_PFR_MAYBE_UNUSED inline bool operator<=(const T& lhs, const T& rhs) { return ::boost::pfr::le_fields(lhs, rhs); }                 \
    BOOST_PFR_MAYBE_UNUSED inline bool operator>=(const T& lhs, const T& rhs) { return ::boost::pfr::ge_fields(lhs, rhs); }                 \
    template <class Char, class Traits>                                                                                                     \
    BOOST_PFR_MAYBE_UNUSED inline ::std::basic_ostream<Char, Traits>& operator<<(::std::basic_ostream<Char, Traits>& out, const T& value) { \
        return out << ::boost::pfr::io_fields(value);                                                                                       \
    }                                                                                                                                       \
    template <class Char, class Traits>                                                                                                     \
    BOOST_PFR_MAYBE_UNUSED inline ::std::basic_istream<Char, Traits>& operator>>(::std::basic_istream<Char, Traits>& in, T& value) {        \
        return in >> ::boost::pfr::io_fields(value);                                                                                        \
    }                                                                                                                                       \
    BOOST_PFR_MAYBE_UNUSED inline std::size_t hash_value(const T& v) {                                                                      \
        return ::boost::pfr::hash_fields(v);                                                                                                \
    }                                                                                                                                       \
/**/

#endif // BOOST_PFR_FUNCTIONS_FOR_HPP

// Copyright (c) 2016-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_FUNCTORS_HPP
#define BOOST_PFR_FUNCTORS_HPP


// Copyright (c) 2016-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_OPS_HPP
#define BOOST_PFR_OPS_HPP


// Copyright (c) 2016-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_DETAIL_DETECTORS_HPP
#define BOOST_PFR_DETAIL_DETECTORS_HPP


#include <functional>
#include <type_traits>

namespace boost { namespace pfr { namespace detail {
///////////////////// `value` is true if Detector<Tleft, Tright> does not compile (SFINAE)
    struct can_not_apply{};

    template <template <class, class> class Detector, class Tleft, class Tright>
    struct not_appliable {
        static constexpr bool value = std::is_same<
            Detector<Tleft, Tright>,
            can_not_apply
        >::value;
    };

///////////////////// Detectors for different operators
    template <class S, class T> auto comp_eq_detector_msvc_helper(long) -> decltype(std::declval<S>() == std::declval<T>());
    template <class S, class T> can_not_apply comp_eq_detector_msvc_helper(int);
    template <class T1, class T2> using comp_eq_detector = decltype(comp_eq_detector_msvc_helper<T1,T2>(1L));

    template <class S, class T> auto comp_ne_detector_msvc_helper(long) -> decltype(std::declval<S>() != std::declval<T>());
    template <class S, class T> can_not_apply comp_ne_detector_msvc_helper(int);
    template <class T1, class T2> using comp_ne_detector = decltype(comp_ne_detector_msvc_helper<T1,T2>(1L));

    template <class S, class T> auto comp_lt_detector_msvc_helper(long) -> decltype(std::declval<S>() < std::declval<T>());
    template <class S, class T> can_not_apply comp_lt_detector_msvc_helper(int);
    template <class T1, class T2> using comp_lt_detector = decltype(comp_lt_detector_msvc_helper<T1,T2>(1L));

    template <class S, class T> auto comp_le_detector_msvc_helper(long) -> decltype(std::declval<S>() <= std::declval<T>());
    template <class S, class T> can_not_apply comp_le_detector_msvc_helper(int);
    template <class T1, class T2> using comp_le_detector = decltype(comp_le_detector_msvc_helper<T1,T2>(1L));

    template <class S, class T> auto comp_gt_detector_msvc_helper(long) -> decltype(std::declval<S>() > std::declval<T>());
    template <class S, class T> can_not_apply comp_gt_detector_msvc_helper(int);
    template <class T1, class T2> using comp_gt_detector = decltype(comp_gt_detector_msvc_helper<T1,T2>(1L));

    template <class S, class T> auto comp_ge_detector_msvc_helper(long) -> decltype(std::declval<S>() >= std::declval<T>());
    template <class S, class T> can_not_apply comp_ge_detector_msvc_helper(int);
    template <class T1, class T2> using comp_ge_detector = decltype(comp_ge_detector_msvc_helper<T1,T2>(1L));


    template <class S> auto hash_detector_msvc_helper(long) -> decltype(std::hash<S>{}(std::declval<S>()));
    template <class S> can_not_apply hash_detector_msvc_helper(int);
    template <class T1, class T2> using hash_detector = decltype(hash_detector_msvc_helper<T1,T2>(1L));


    template <class S, class T> auto ostreamable_detector_msvc_helper(long) -> decltype(std::declval<S>() << std::declval<T>());
    template <class S, class T> can_not_apply ostreamable_detector_msvc_helper(int);
    template <class S, class T> using ostreamable_detector = decltype(ostreamable_detector_msvc_helper<S,T>(1L));

    template <class S, class T> auto istreamable_detector_msvc_helper(long) -> decltype(std::declval<S>() >> std::declval<T>());
    template <class S, class T> can_not_apply istreamable_detector_msvc_helper(int);
    template <class S, class T> using istreamable_detector = decltype(istreamable_detector_msvc_helper<S,T>(1L));

}}} // namespace boost::pfr::detail

#endif // BOOST_PFR_DETAIL_DETECTORS_HPP



/// \file boost/pfr/ops.hpp
/// Contains comparison and hashing functions.
/// If type is comparable using its own operator or its conversion operator, then the types operator is used. Otherwise
/// the operation is done via corresponding function from boost/pfr/ops.hpp header.
///
/// \b Example:
/// \code
///     #include <boost/pfr/ops.hpp>
///     struct comparable_struct {      // No operators defined for that structure
///         int i; short s; char data[7]; bool bl; int a,b,c,d,e,f;
///     };
///     // ...
///
///     comparable_struct s1 {0, 1, "Hello", false, 6,7,8,9,10,11};
///     comparable_struct s2 {0, 1, "Hello", false, 6,7,8,9,10,11111};
///     assert(boost::pfr::lt(s1, s2));
/// \endcode
///
/// \podops for other ways to define operators and more details.
///
/// \b Synopsis:
namespace boost { namespace pfr {

namespace detail {

///////////////////// Helper typedefs that are used by all the ops
    template <template <class, class> class Detector, class T, class U>
    using enable_not_comp_base_t = std::enable_if_t<
        not_appliable<Detector, T const&, U const&>::value,
        bool
    >;

    template <template <class, class> class Detector, class T, class U>
    using enable_comp_base_t = std::enable_if_t<
        !not_appliable<Detector, T const&, U const&>::value,
        bool
    >;
///////////////////// std::enable_if_t like functions that enable only if types do not support operation

    template <class T, class U> using enable_not_eq_comp_t = enable_not_comp_base_t<comp_eq_detector, T, U>;
    template <class T, class U> using enable_not_ne_comp_t = enable_not_comp_base_t<comp_ne_detector, T, U>;
    template <class T, class U> using enable_not_lt_comp_t = enable_not_comp_base_t<comp_lt_detector, T, U>;
    template <class T, class U> using enable_not_le_comp_t = enable_not_comp_base_t<comp_le_detector, T, U>;
    template <class T, class U> using enable_not_gt_comp_t = enable_not_comp_base_t<comp_gt_detector, T, U>;
    template <class T, class U> using enable_not_ge_comp_t = enable_not_comp_base_t<comp_ge_detector, T, U>;

    template <class T> using enable_not_hashable_t = std::enable_if_t<
        not_appliable<hash_detector, const T&, const T&>::value,
        std::size_t
    >;
///////////////////// std::enable_if_t like functions that enable only if types do support operation

    template <class T, class U> using enable_eq_comp_t = enable_comp_base_t<comp_eq_detector, T, U>;
    template <class T, class U> using enable_ne_comp_t = enable_comp_base_t<comp_ne_detector, T, U>;
    template <class T, class U> using enable_lt_comp_t = enable_comp_base_t<comp_lt_detector, T, U>;
    template <class T, class U> using enable_le_comp_t = enable_comp_base_t<comp_le_detector, T, U>;
    template <class T, class U> using enable_gt_comp_t = enable_comp_base_t<comp_gt_detector, T, U>;
    template <class T, class U> using enable_ge_comp_t = enable_comp_base_t<comp_ge_detector, T, U>;

    template <class T> using enable_hashable_t = std::enable_if_t<
        !not_appliable<hash_detector, const T&, const T&>::value,
        std::size_t
    >;
} // namespace detail


/// \brief Compares lhs and rhs for equality using their own comparison and conversion operators; if no operators available returns \forcedlink{eq_fields}(lhs, rhs).
///
/// \returns true if lhs is equal to rhs; false otherwise
template <class T, class U>
constexpr detail::enable_not_eq_comp_t<T, U> eq(const T& lhs, const U& rhs) noexcept {
    return boost::pfr::eq_fields(lhs, rhs);
}

/// \overload eq
template <class T, class U>
constexpr detail::enable_eq_comp_t<T, U> eq(const T& lhs, const U& rhs) {
    return lhs == rhs;
}


/// \brief Compares lhs and rhs for inequality using their own comparison and conversion operators; if no operators available returns \forcedlink{ne_fields}(lhs, rhs).
///
/// \returns true if lhs is not equal to rhs; false otherwise
template <class T, class U>
constexpr detail::enable_not_ne_comp_t<T, U> ne(const T& lhs, const U& rhs) noexcept {
    return boost::pfr::ne_fields(lhs, rhs);
}

/// \overload ne
template <class T, class U>
constexpr detail::enable_ne_comp_t<T, U> ne(const T& lhs, const U& rhs) {
    return lhs != rhs;
}


/// \brief Compares lhs and rhs for less-than using their own comparison and conversion operators; if no operators available returns \forcedlink{lt_fields}(lhs, rhs).
///
/// \returns true if lhs is less than rhs; false otherwise
template <class T, class U>
constexpr detail::enable_not_lt_comp_t<T, U> lt(const T& lhs, const U& rhs) noexcept {
    return boost::pfr::lt_fields(lhs, rhs);
}

/// \overload lt
template <class T, class U>
constexpr detail::enable_lt_comp_t<T, U> lt(const T& lhs, const U& rhs) {
    return lhs < rhs;
}


/// \brief Compares lhs and rhs for greater-than using their own comparison and conversion operators; if no operators available returns \forcedlink{lt_fields}(lhs, rhs).
///
/// \returns true if lhs is greater than rhs; false otherwise
template <class T, class U>
constexpr detail::enable_not_gt_comp_t<T, U> gt(const T& lhs, const U& rhs) noexcept {
    return boost::pfr::gt_fields(lhs, rhs);
}

/// \overload gt
template <class T, class U>
constexpr detail::enable_gt_comp_t<T, U> gt(const T& lhs, const U& rhs) {
    return lhs > rhs;
}


/// \brief Compares lhs and rhs for less-equal using their own comparison and conversion operators; if no operators available returns \forcedlink{le_fields}(lhs, rhs).
///
/// \returns true if lhs is less or equal to rhs; false otherwise
template <class T, class U>
constexpr detail::enable_not_le_comp_t<T, U> le(const T& lhs, const U& rhs) noexcept {
    return boost::pfr::le_fields(lhs, rhs);
}

/// \overload le
template <class T, class U>
constexpr detail::enable_le_comp_t<T, U> le(const T& lhs, const U& rhs) {
    return lhs <= rhs;
}


/// \brief Compares lhs and rhs for greater-equal using their own comparison and conversion operators; if no operators available returns \forcedlink{ge_fields}(lhs, rhs).
///
/// \returns true if lhs is greater or equal to rhs; false otherwise
template <class T, class U>
constexpr detail::enable_not_ge_comp_t<T, U> ge(const T& lhs, const U& rhs) noexcept {
    return boost::pfr::ge_fields(lhs, rhs);
}

/// \overload ge
template <class T, class U>
constexpr detail::enable_ge_comp_t<T, U> ge(const T& lhs, const U& rhs) {
    return lhs >= rhs;
}


/// \brief Hashes value using its own std::hash specialization; if no std::hash specialization available returns \forcedlink{hash_fields}(value).
///
/// \returns std::size_t with hash of the value
template <class T>
constexpr detail::enable_not_hashable_t<T> hash_value(const T& value) noexcept {
    return boost::pfr::hash_fields(value);
}

/// \overload hash_value
template <class T>
constexpr detail::enable_hashable_t<T> hash_value(const T& value) {
    return std::hash<T>{}(value);
}

}} // namespace boost::pfr

#endif // BOOST_PFR_OPS_HPP


/// \file boost/pfr/functors.hpp
/// Contains functors that are close to the Standard Library ones.
/// Each functor calls corresponding Boost.PFR function from boost/pfr/ops.hpp
///
/// \b Example:
/// \code
///     #include <boost/pfr/functors.hpp>
///     struct my_struct {      // No operators defined for that structure
///         int i; short s; char data[7]; bool bl; int a,b,c,d,e,f;
///     };
///     // ...
///
///     std::unordered_set<
///         my_struct,
///         boost::pfr::hash<>,
///         boost::pfr::equal_to<>
///     > my_set;
/// \endcode
///
/// \b Synopsis:
namespace boost { namespace pfr {

///////////////////// Comparisons

/// \brief std::equal_to like comparator that returns \forcedlink{eq}(x, y)
template <class T = void> struct equal_to {
    /// \return \b true if each field of \b x equals the field with same index of \b y.
    bool operator()(const T& x, const T& y) const {
        return boost::pfr::eq(x, y);
    }

#ifdef BOOST_PFR_DOXYGEN_INVOKED
    /// This typedef exists only if T \b is void
    typedef std::true_type is_transparent;

    /// This operator allows comparison of \b x and \b y that have different type.
    /// \pre Exists only if T \b is void.
    template <class V, class U> bool operator()(const V& x, const U& y) const;
#endif
};

/// @cond
template <> struct equal_to<void> {
    template <class T, class U>
    bool operator()(const T& x, const U& y) const {
        return boost::pfr::eq(x, y);
    }

    typedef std::true_type is_transparent;
};
/// @endcond

/// \brief std::not_equal like comparator that returns \forcedlink{ne}(x, y)
template <class T = void> struct not_equal {
    /// \return \b true if at least one field \b x not equals the field with same index of \b y.
    bool operator()(const T& x, const T& y) const {
        return boost::pfr::ne(x, y);
    }

#ifdef BOOST_PFR_DOXYGEN_INVOKED
    /// This typedef exists only if T \b is void
    typedef std::true_type is_transparent;

    /// This operator allows comparison of \b x and \b y that have different type.
    /// \pre Exists only if T \b is void.
    template <class V, class U> bool operator()(const V& x, const U& y) const;
#endif
};

/// @cond
template <> struct not_equal<void> {
    template <class T, class U>
    bool operator()(const T& x, const U& y) const {
        return boost::pfr::ne(x, y);
    }

    typedef std::true_type is_transparent;
};
/// @endcond

/// \brief std::greater like comparator that returns \forcedlink{gt}(x, y)
template <class T = void> struct greater {
    /// \return \b true if field of \b x greater than the field with same index of \b y and all previous fields of \b x equal to the same fields of \b y.
    bool operator()(const T& x, const T& y) const {
        return boost::pfr::gt(x, y);
    }

#ifdef BOOST_PFR_DOXYGEN_INVOKED
    /// This typedef exists only if T \b is void
    typedef std::true_type is_transparent;

    /// This operator allows comparison of \b x and \b y that have different type.
    /// \pre Exists only if T \b is void.
    template <class V, class U> bool operator()(const V& x, const U& y) const;
#endif
};

/// @cond
template <> struct greater<void> {
    template <class T, class U>
    bool operator()(const T& x, const U& y) const {
        return boost::pfr::gt(x, y);
    }

    typedef std::true_type is_transparent;
};
/// @endcond

/// \brief std::less like comparator that returns \forcedlink{lt}(x, y)
template <class T = void> struct less {
    /// \return \b true if field of \b x less than the field with same index of \b y and all previous fields of \b x equal to the same fields of \b y.
    bool operator()(const T& x, const T& y) const {
        return boost::pfr::lt(x, y);
    }

#ifdef BOOST_PFR_DOXYGEN_INVOKED
    /// This typedef exists only if T \b is void
    typedef std::true_type is_transparent;

    /// This operator allows comparison of \b x and \b y that have different type.
    /// \pre Exists only if T \b is void.
    template <class V, class U> bool operator()(const V& x, const U& y) const;
#endif
};

/// @cond
template <> struct less<void> {
    template <class T, class U>
    bool operator()(const T& x, const U& y) const {
        return boost::pfr::lt(x, y);
    }

    typedef std::true_type is_transparent;
};
/// @endcond

/// \brief std::greater_equal like comparator that returns \forcedlink{ge}(x, y)
template <class T = void> struct greater_equal {
    /// \return \b true if field of \b x greater than the field with same index of \b y and all previous fields of \b x equal to the same fields of \b y;
    /// or if each field of \b x equals the field with same index of \b y.
    bool operator()(const T& x, const T& y) const {
        return boost::pfr::ge(x, y);
    }

#ifdef BOOST_PFR_DOXYGEN_INVOKED
    /// This typedef exists only if T \b is void
    typedef std::true_type is_transparent;

    /// This operator allows comparison of \b x and \b y that have different type.
    /// \pre Exists only if T \b is void.
    template <class V, class U> bool operator()(const V& x, const U& y) const;
#endif
};

/// @cond
template <> struct greater_equal<void> {
    template <class T, class U>
    bool operator()(const T& x, const U& y) const {
        return boost::pfr::ge(x, y);
    }

    typedef std::true_type is_transparent;
};
/// @endcond

/// \brief std::less_equal like comparator that returns \forcedlink{le}(x, y)
template <class T = void> struct less_equal {
    /// \return \b true if field of \b x less than the field with same index of \b y and all previous fields of \b x equal to the same fields of \b y;
    /// or if each field of \b x equals the field with same index of \b y.
    bool operator()(const T& x, const T& y) const {
        return boost::pfr::le(x, y);
    }

#ifdef BOOST_PFR_DOXYGEN_INVOKED
    /// This typedef exists only if T \b is void
    typedef std::true_type is_transparent;

    /// This operator allows comparison of \b x and \b y that have different type.
    /// \pre Exists only if T \b is void.
    template <class V, class U> bool operator()(const V& x, const U& y) const;
#endif
};

/// @cond
template <> struct less_equal<void> {
    template <class T, class U>
    bool operator()(const T& x, const U& y) const {
        return boost::pfr::le(x, y);
    }

    typedef std::true_type is_transparent;
};
/// @endcond


/// \brief std::hash like functor that returns \forcedlink{hash_value}(x)
template <class T> struct hash {
    /// \return hash value of \b x.
    std::size_t operator()(const T& x) const {
        return boost::pfr::hash_value(x);
    }
};

}} // namespace boost::pfr

#endif // BOOST_PFR_FUNCTORS_HPP
// Copyright (c) 2016-2023 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_IO_HPP
#define BOOST_PFR_IO_HPP



/// \file boost/pfr/io.hpp
/// Contains IO stream manipulator \forcedlink{io} for types.
/// If type is streamable using its own operator or its conversion operator, then the types operator is used.
///
/// \b Example:
/// \code
///     #include <boost/pfr/io.hpp>
///     struct comparable_struct {      // No operators defined for that structure
///         int i; short s; char data[7]; bool bl; int a,b,c,d,e,f;
///     };
///     // ...
///
///     comparable_struct s1 {0, 1, "Hello", false, 6,7,8,9,10,11};
///     std::cout << boost::pfr::io(s1);  // Outputs: {0, 1, H, e, l, l, o, , , 0, 6, 7, 8, 9, 10, 11}
/// \endcode
///
/// \podops for other ways to define operators and more details.
///
/// \b Synopsis:
namespace boost { namespace pfr {

namespace detail {

///////////////////// Helper typedefs
    template <class Stream, class Type>
    using enable_not_ostreamable_t = std::enable_if_t<
        not_appliable<ostreamable_detector, Stream&, const std::remove_reference_t<Type>&>::value,
        Stream&
    >;

    template <class Stream, class Type>
    using enable_not_istreamable_t = std::enable_if_t<
        not_appliable<istreamable_detector, Stream&, Type&>::value,
        Stream&
    >;

    template <class Stream, class Type>
    using enable_ostreamable_t = std::enable_if_t<
        !not_appliable<ostreamable_detector, Stream&, const std::remove_reference_t<Type>&>::value,
        Stream&
    >;

    template <class Stream, class Type>
    using enable_istreamable_t = std::enable_if_t<
        !not_appliable<istreamable_detector, Stream&, Type&>::value,
        Stream&
    >;

///////////////////// IO impl

template <class T>
struct io_impl {
    T value;
};

template <class Char, class Traits, class T>
enable_not_ostreamable_t<std::basic_ostream<Char, Traits>, T> operator<<(std::basic_ostream<Char, Traits>& out, io_impl<T>&& x) {
    return out << boost::pfr::io_fields(std::forward<T>(x.value));
}

template <class Char, class Traits, class T>
enable_ostreamable_t<std::basic_ostream<Char, Traits>, T> operator<<(std::basic_ostream<Char, Traits>& out, io_impl<T>&& x) {
    return out << x.value;
}

template <class Char, class Traits, class T>
enable_not_istreamable_t<std::basic_istream<Char, Traits>, T> operator>>(std::basic_istream<Char, Traits>& in, io_impl<T>&& x) {
    return in >> boost::pfr::io_fields(std::forward<T>(x.value));
}

template <class Char, class Traits, class T>
enable_istreamable_t<std::basic_istream<Char, Traits>, T> operator>>(std::basic_istream<Char, Traits>& in, io_impl<T>&& x) {
    return in >> x.value;
}

} // namespace detail

/// IO manipulator to read/write \aggregate `value` using its IO stream operators or using \forcedlink{io_fields} if operators are not available.
///
/// \b Example:
/// \code
///     struct my_struct { int i; short s; };
///     my_struct x;
///     std::stringstream ss;
///     ss << "{ 12, 13 }";
///     ss >> boost::pfr::io(x);
///     assert(x.i == 12);
///     assert(x.s == 13);
/// \endcode
///
/// \customio
template <class T>
auto io(T&& value) noexcept {
    return detail::io_impl<T>{std::forward<T>(value)};
}

}} // namespace boost::pfr

#endif // BOOST_PFR_IO_HPP
// Copyright (c) 2022 Denis Mikhailov
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_DETAIL_TRAITS_FWD_HPP
#define BOOST_PFR_DETAIL_TRAITS_FWD_HPP


namespace boost { namespace pfr {

template<class T, class WhatFor>
struct is_reflectable;

}} // namespace boost::pfr

#endif // BOOST_PFR_DETAIL_TRAITS_FWD_HPP


// Copyright (c) 2022 Denis Mikhailov
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_TRAITS_HPP
#define BOOST_PFR_TRAITS_HPP


// Copyright (c) 2022 Denis Mikhailov
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PFR_DETAIL_POSSIBLE_REFLECTABLE_HPP
#define BOOST_PFR_DETAIL_POSSIBLE_REFLECTABLE_HPP


#include <type_traits> // for std::is_aggregate

namespace boost { namespace pfr { namespace detail {

///////////////////// Returns false when the type exactly wasn't be reflectable
template <class T, class WhatFor>
constexpr decltype(is_reflectable<T, WhatFor>::value) possible_reflectable(long) noexcept {
    return is_reflectable<T, WhatFor>::value;
}

#if BOOST_PFR_ENABLE_IMPLICIT_REFLECTION

template <class T, class WhatFor>
constexpr bool possible_reflectable(int) noexcept {
#   if  defined(__cpp_lib_is_aggregate)
    using type = std::remove_cv_t<T>;
    return std::is_aggregate<type>();
#   else
    return true;
#   endif
}

#else

template <class T, class WhatFor>
constexpr bool possible_reflectable(int) noexcept {
    // negative answer here won't change behaviour in PFR-dependent libraries(like Fusion)
    return false;
}

#endif

}}} // namespace boost::pfr::detail

#endif // BOOST_PFR_DETAIL_POSSIBLE_REFLECTABLE_HPP


#include <type_traits>

/// \file boost/pfr/traits.hpp
/// Contains traits \forcedlink{is_reflectable} and \forcedlink{is_implicitly_reflectable} for detecting an ability to reflect type.
///
/// \b Synopsis:

namespace boost { namespace pfr {

/// Has a static const member variable `value` when it is known that type T can or can't be reflected using Boost.PFR; otherwise, there is no member variable.
/// Every user may (and in some difficult cases - should) specialize is_reflectable on his own.
///
/// \b Example:
/// \code
///     namespace boost { namespace pfr {
///         template<class All> struct is_reflectable<A, All> : std::false_type {};       // 'A' won't be interpreted as reflectable everywhere
///         template<> struct is_reflectable<B, boost_fusion_tag> : std::false_type {};   // 'B' won't be interpreted as reflectable in only Boost Fusion
///     }}
/// \endcode
/// \note is_reflectable affects is_implicitly_reflectable, the decision made by is_reflectable is used by is_implicitly_reflectable.
template<class T, class WhatFor>
struct is_reflectable { /*  does not have 'value' because value is unknown */ };

// these specs can't be inherited from 'std::integral_constant< bool, boost::pfr::is_reflectable<T, WhatFor>::value >',
// because it will break the sfinae-friendliness
template<class T, class WhatFor>
struct is_reflectable<const T, WhatFor> : boost::pfr::is_reflectable<T, WhatFor> {};

template<class T, class WhatFor>
struct is_reflectable<volatile T, WhatFor> : boost::pfr::is_reflectable<T, WhatFor> {};

template<class T, class WhatFor>
struct is_reflectable<const volatile T, WhatFor> : boost::pfr::is_reflectable<T, WhatFor> {};

/// Checks the input type for the potential to be reflected.
/// Specialize is_reflectable if you disagree with is_implicitly_reflectable's default decision.
template<class T, class WhatFor>
using is_implicitly_reflectable = std::integral_constant< bool, boost::pfr::detail::possible_reflectable<T, WhatFor>(1L) >;

/// Checks the input type for the potential to be reflected.
/// Specialize is_reflectable if you disagree with is_implicitly_reflectable_v's default decision.
template<class T, class WhatFor>
constexpr bool is_implicitly_reflectable_v = is_implicitly_reflectable<T, WhatFor>::value;

}} // namespace boost::pfr

#endif // BOOST_PFR_TRAITS_HPP


#endif // BOOST_PFR_HPP
