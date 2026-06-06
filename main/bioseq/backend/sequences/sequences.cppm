module;

#include "common.h"

//include "fmt/ranges.h"

#include <array>
#include <cstdint>
#include <deque>
#include <iterator>
#include <list>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <valarray>
#include <vector>
#include <algorithm>
#include <limits>

#include <boost/integer.hpp>
#include <boost/iterator/iterator_adaptor.hpp>


export module bioseq_sequences:sequences;

import mlogging;
import sequan_compat;
import bioseq_common.templating;

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
    ((sizeof(typename A::storage_unit) * 8) % (sizeof(typename B::storage_unit) * 8) == 0 || (sizeof(typename B::storage_unit) * 8) % (sizeof(typename A::storage_unit) * 8) == 0) && \
    A::leftover_bits == 0 && B::leftover_bits == 0
);

template<typename R>
concept sequence_proper_container = \
    sequence_alphabet<std::remove_reference_t<std::ranges::range_reference_t<R>>> && \
    std::ranges::sized_range<R> && \
    std::ranges::random_access_range<R>;

// DOC0258 brand-marker probe used by the new zero-copy
// `compact_sequence_range<S>` specialisation. The actual
// `compact_sequence_stdc_instance` template is defined later in this
// module; the concept itself only inspects a member name so the
// forward use here is well-formed.
template<typename S>
concept is_compact_sequence_stdc_instance =
    requires { S::is_valid_compact_sequence_stdc_instance; }
    && S::is_valid_compact_sequence_stdc_instance == true;

template<typename R>
concept supports_compact_sequence_range = \
    is_sequence_custom_alphabet<R> || \
    sequence_proper_container<R> || \
    sequence_alphabet<R> || \
    is_compact_sequence_stdc_instance<R>;

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
// DOC0258: supported_alphabets<…> pack carrier, dispatch primitives.
//
// The pack carries a compile-time list of alphabets a particular
// compact_sequence_stdc instantiation can hold. The smallest unsigned
// integer that fits the pack's index becomes `alphabet_code_t`.
// =====================================================================

static_assert(__cplusplus >= 202302L,
              "DOC0258 requires C++23-or-later (project default: C++26)");

// DOC0261 — the alphabet_entry brand carries both an alphabet type
// and a CompTimeStr display name pinned at the type level. The pack
// is a single parameter pack of these brands; every name-based
// dispatch site routes through the pack helpers, never through a
// hand-written strcmp ladder.

template <sequence_alphabet A, ::bioseq::common::templating::CompTimeStr Name>
struct alphabet_entry {
    using alphabet_type = A;
    static constexpr auto alphabet_name = Name;
    static constexpr std::size_t name_size = Name.size();
    static constexpr std::string_view alphabet_name_sv = Name.view();
    static constexpr bool is_valid_alphabet_entry = true;
};

template <class T>
concept is_alphabet_entry = requires {
    requires T::is_valid_alphabet_entry == true;
    typename T::alphabet_type;
    requires sequence_alphabet<typename T::alphabet_type>;
};

namespace dispatch_detail {

template <class Lhs, class Rhs>
[[nodiscard]] consteval bool entries_name_distinct() noexcept {
    if constexpr (Lhs::alphabet_name.size() != Rhs::alphabet_name.size()) {
        return true;
    } else {
        return !(Lhs::alphabet_name == Rhs::alphabet_name);
    }
}

template <std::size_t I, class... Entries, std::size_t... Js>
[[nodiscard]] consteval bool ith_name_unique(std::index_sequence<Js...>) noexcept {
    using Tup = std::tuple<Entries...>;
    using Ei = std::tuple_element_t<I, Tup>;
    return ((entries_name_distinct<Ei,
              std::tuple_element_t<I + 1u + Js, Tup>>()) && ...);
}

template <class... Entries>
[[nodiscard]] consteval bool unique_names_in_pack() noexcept {
    constexpr std::size_t N = sizeof...(Entries);
    if constexpr (N <= 1) {
        return true;
    } else {
        return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            return (ith_name_unique<Is, Entries...>(
                std::make_index_sequence<(N > Is + 1u ? N - Is - 1u : 0u)>{})
                && ...);
        }(std::make_index_sequence<N>{});
    }
}

}  // namespace dispatch_detail

namespace detail_supported_alphabets {

template <class... Entries>
consteval std::size_t valid_names_live_length() noexcept {
    constexpr std::size_t entry_count = sizeof...(Entries);
    constexpr std::size_t names_bytes  = (Entries::alphabet_name.size() + ...);
    constexpr std::size_t quote_bytes  = 2u * entry_count;
    constexpr std::size_t separator_bytes =
        (entry_count > 1u ? 2u * (entry_count - 1u) : 0u);
    return names_bytes + quote_bytes + separator_bytes;
}

template <class... Entries>
consteval auto build_valid_names_buffer() noexcept {
    constexpr std::size_t live_size = valid_names_live_length<Entries...>();
    std::array<char, live_size> name_buffer{};
    std::size_t write_offset = 0;
    bool first_entry = true;
    auto append = [&]<class E>() {
        if (!first_entry) {
            name_buffer[write_offset++] = ',';
            name_buffer[write_offset++] = ' ';
        }
        name_buffer[write_offset++] = '\'';
        for (std::size_t name_byte_index = 0;
             name_byte_index < E::alphabet_name.size();
             ++name_byte_index) {
            name_buffer[write_offset++] = E::alphabet_name[name_byte_index];
        }
        name_buffer[write_offset++] = '\'';
        first_entry = false;
    };
    (append.template operator()<Entries>(), ...);
    return name_buffer;
}

template <class... Entries>
inline constexpr auto valid_names_buffer_v = build_valid_names_buffer<Entries...>();

template <class... Entries>
inline constexpr std::size_t valid_names_live_length_v =
    valid_names_live_length<Entries...>();

}  // namespace detail_supported_alphabets

template <is_alphabet_entry... entries_pack>
requires (sizeof...(entries_pack) > 0)
struct supported_alphabets {
    using alphabet_entries_tuple = std::tuple<entries_pack...>;
    using alphabets_tuple        = std::tuple<typename entries_pack::alphabet_type...>;

    static constexpr size_t alphabet_count = sizeof...(entries_pack);

    /// Smallest unsigned integer type that can represent every pack index.
    /// `boost::uint_t<N>::least` requires N >= 1; clamp via std::max so the
    /// single-alphabet case (`ceil_log2(1) == 0`) still picks a uint8_t.
    using alphabet_code_t = typename boost::uint_t<
        (seqan3::detail::ceil_log2(alphabet_count) > 0
            ? seqan3::detail::ceil_log2(alphabet_count) : 1u)
    >::least;

    static constexpr bool is_valid_supported_alphabets = true;

    static_assert(
        ::bioseq::common::templating::unique_types_in_tuple_v<alphabets_tuple>,
        "supported_alphabets: alphabet types must be unique within the pack");
    static_assert(
        dispatch_detail::unique_names_in_pack<entries_pack...>(),
        "supported_alphabets: alphabet display names must be unique within the pack");

    /// Probe types for the runtime-dispatch primitives. Defined as
    /// nested struct templates so their `matches` / `extract` member
    /// templates are well-formed (local-class templates are forbidden
    /// by C++ — DOC0261 implementer note: probes live alongside the
    /// pack, not inside the dispatch function body).
    struct code_match_probe {
        alphabet_code_t rt;
        template <class Entry>
        [[gnu::always_inline]]
        constexpr bool matches(alphabet_code_t cursor) const noexcept {
            return cursor == rt;
        }
        [[noreturn]] void on_no_match(alphabet_code_t cursor) const {
            ASSERT(false,
                fmt::format("dispatch_alphabet_by_code: code {} > pack max {}",
                            static_cast<std::size_t>(rt),
                            static_cast<std::size_t>(cursor)));
        }
    };

    struct name_match_probe {
        const char* rt;
        bool& matched_ref;
        template <class Entry>
        [[gnu::always_inline]]
        constexpr bool matches(alphabet_code_t /*cursor*/) const noexcept {
            if (Entry::alphabet_name.equals_cstr(rt)) {
                matched_ref = true;
                return true;
            }
            return false;
        }
        constexpr void on_no_match(alphabet_code_t /*cursor*/) const noexcept {
            // matched_ref stays false; caller checks the return value.
        }
    };

    struct code_to_name_probe {
        alphabet_code_t rt;
        template <class Entry>
        [[gnu::always_inline]]
        constexpr std::optional<std::string_view> extract(
            alphabet_code_t cursor) const noexcept
        {
            if (cursor == rt) return Entry::alphabet_name_sv;
            return std::nullopt;
        }
    };

    struct name_to_code_probe {
        const char* rt;
        template <class Entry>
        [[gnu::always_inline]]
        constexpr std::optional<alphabet_code_t> extract(
            alphabet_code_t cursor) const noexcept
        {
            if (Entry::alphabet_name.equals_cstr(rt)) {
                return cursor;
            }
            return std::nullopt;
        }
    };

    /// Stateless adapter forwarding `operator()<Entry>` to
    /// `operator()<typename Entry::alphabet_type>` on the wrapped handler.
    /// No `[&]` capture (the project has been burned by capture-by-reference
    /// nested lambdas not inlining): the adapter holds the handler by
    /// reference as a data member and the templated `operator()` is
    /// `[[gnu::always_inline]]`.
    template <class HandlerFn>
    struct entry_to_alphabet_adapter {
        HandlerFn& handler;

        template <class Entry>
        [[gnu::always_inline]]
        constexpr void operator()() const {
            handler.template operator()<typename Entry::alphabet_type>();
        }
    };

    /// Runtime → compile-time fanout by code (DOC0261 §5.5). Same shape
    /// as the old `dispatch_alphabet`; renamed for symmetry with the new
    /// `_by_name` member below. Handler is invoked with the alphabet
    /// type (NOT the entry) so existing callers that say
    /// `[&]<class A>(){ ... }` keep working unchanged.
    template <class HandlerFn>
    [[gnu::always_inline]]
    static constexpr void dispatch_alphabet_by_code(
        alphabet_code_t code, HandlerFn&& handler)
    {
        namespace tpl = ::bioseq::common::templating;

        entry_to_alphabet_adapter<std::remove_reference_t<HandlerFn>>
            adapter{handler};
        code_match_probe probe{code};
        tpl::dispatch_recursive_pack<
            alphabet_code_t,
            tpl::type_list<entries_pack...>>(adapter, probe);
    }

    /// Runtime → compile-time fanout by name (DOC0261 §5.6). Walks the
    /// pack; on the first entry whose name equals `name` invokes
    /// `handler.template operator()<Entry>()`. Returns true if a handler
    /// was invoked. Handler is invoked with `Entry` (NOT alphabet) so
    /// callers can recover both the type and the static name buffer.
    template <class HandlerFn>
    static constexpr bool dispatch_alphabet_by_name(
        const char* name, HandlerFn&& handler) noexcept
    {
        namespace tpl = ::bioseq::common::templating;

        bool matched = false;
        name_match_probe probe{name, matched};
        tpl::dispatch_recursive_pack<
            alphabet_code_t,
            tpl::type_list<entries_pack...>>(handler, probe);
        return matched;
    }

    /// Compile-time look-up: pack index of a specific alphabet. Asserting
    /// — absence is a build error.
    template <sequence_alphabet A>
    [[nodiscard]] static constexpr alphabet_code_t index_of() {
        using namespace ::bioseq::common::templating;
        return static_cast<alphabet_code_t>(
            index_of_type_pack_checked<A, typename entries_pack::alphabet_type...>());
    }

    /// Compile-time look-up: display name of a specific alphabet, returned
    /// as a `CompTimeStr<N>` value. The `requires` clause short-circuits
    /// if `A` is not in the pack so diagnostics fire at the alias site.
    template <sequence_alphabet A>
    [[nodiscard]] static constexpr auto name_of() {
        constexpr std::size_t idx = static_cast<std::size_t>(index_of<A>());
        return std::tuple_element_t<idx, alphabet_entries_tuple>::alphabet_name;
    }

    /// Member alias template: the `alphabet_entry<...>` brand for a
    /// specific alphabet. Useful so test files can pull the entry off
    /// the pack rather than naming a literal.
    template <sequence_alphabet A>
        requires (::bioseq::common::templating::contains_type_pack<
            A, typename entries_pack::alphabet_type...>())
    using entry_for = std::tuple_element_t<
        ::bioseq::common::templating::index_of_type_pack<
            A, typename entries_pack::alphabet_type...>(),
        alphabet_entries_tuple>;

    /// Runtime: map a code to the corresponding pack entry's display
    /// name. Returns "?" on out-of-band input with a JustLog telemetry
    /// line so silent corruption is observable.
    [[nodiscard]] static std::string_view name_of_alphabet_code(
        alphabet_code_t code) noexcept
    {
        namespace tpl = ::bioseq::common::templating;

        auto opt = tpl::find_first_match_in_pack<
            alphabet_code_t, std::string_view,
            tpl::type_list<entries_pack...>>(code_to_name_probe{code});

        if (!opt.has_value()) [[unlikely]] {
            JustLog("[bioseq] name_of_alphabet_code: out-of-band code={}",
                    static_cast<std::size_t>(code));
            return std::string_view{"?"};
        }
        return *opt;
    }

    /// Runtime: map a cstring to the corresponding pack index. Returns
    /// `std::nullopt` if the name is not in the pack (DOC0261 §5.7).
    [[nodiscard]] static std::optional<alphabet_code_t> try_index_of_name(
        const char* name) noexcept
    {
        namespace tpl = ::bioseq::common::templating;
        return tpl::find_first_match_in_pack<
            alphabet_code_t, alphabet_code_t,
            tpl::type_list<entries_pack...>>(name_to_code_probe{name});
    }

    /// Build-time CSV of single-quoted, comma-space-separated names —
    /// `"'dna4', 'dna5'"`. Used to generate the `errhint` text in
    /// pg_compact_sequence_typmod_in. Byte-identical to the historical
    /// hand-written hint (DOC0261 R6.1).
    [[nodiscard]] static constexpr std::string_view
    valid_alphabet_names_csv() noexcept
    {
        return std::string_view{
            detail_supported_alphabets::valid_names_buffer_v<entries_pack...>.data(),
            detail_supported_alphabets::valid_names_live_length_v<entries_pack...>};
    }
};

template<class T>
concept is_supported_alphabets_v = requires {
    requires T::is_valid_supported_alphabets == true;
    typename T::alphabets_tuple;
    typename T::alphabet_code_t;
    { T::alphabet_count } -> std::convertible_to<size_t>;
};

template<class Pack, class A>
concept is_in_supported_alphabets_v =
    is_supported_alphabets_v<Pack>
    && ::bioseq::common::templating::contains_type_v<
           A, typename Pack::alphabets_tuple>;

/// Pick the matching templated lambda body for a runtime storage_unit_byte_width
/// in {1, 2, 4, 8}. The four cases pin a `size_t` NTTP to the templated
/// lambda; callers recover the storage_unit type via
/// `typename boost::uint_t<BW * 8>::least`.
template<class HandlerFn>
[[gnu::always_inline]]
inline constexpr void dispatch_storage_unit_byte_width(
    std::uint8_t byte_width, HandlerFn&& handler)
{
    switch (byte_width) {
        case 1: handler.template operator()<size_t{1}>(); return;
        case 2: handler.template operator()<size_t{2}>(); return;
        case 4: handler.template operator()<size_t{4}>(); return;
        case 8: handler.template operator()<size_t{8}>(); return;
    }
    ASSERT(false,
           fmt::format("dispatch_storage_unit_byte_width: invalid width {}",
                       static_cast<unsigned>(byte_width)));
}

/// Constexpr round-up of `bytes` to the next multiple of sizeof(uint64_t).
/// Used by the compact_sequence_stdc factories so `data[]` can be safely
/// reinterpreted across {1, 2, 4, 8} byte widths (DOC0258 R6).
[[gnu::always_inline]]
inline constexpr size_t round_up_to_uint64(size_t bytes) noexcept {
    constexpr size_t u64 = sizeof(std::uint64_t);
    return ((bytes + u64 - 1) / u64) * u64;
}

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
 * `bit_casted_view` lets a `compact_sequence_range<base>` be iterated as if its
 * underlying buffer were a contiguous array of a different `storage_unit` —
 * e.g. a `compact_sequence_range<sequence_custom_alphabet<dna4, uint64_t>>`
 * viewed as `uint32_t` units (downcast) or vice-versa (upcast). This is only
 * sound when both source and target alphabets pack their letters with no
 * leftover bits per unit, so the byte stream is identical and only the
 * "how many letters do I unpack per stride" differs.
 *
 * Exposes the minimal sequence-iteration interface the SIMD loop in
 * `iterate_sequence_bp_simd_v2` needs: `bp_size()`, `byte_size()`, `size()`,
 * `padding()`, `read_cptr(it)`, `advance_iter(it, n)`. A plain
 * `compact_sequence_range<R>` exposes the same shape — both satisfy
 * `supports_compact_sequence_bitcast_view` (defined below `compact_sequence_range_base`).
 */
template <
    typename base_t,
    typename output_t
>
struct bit_casted_view {
public:
    using base_traits = base_t::traits;
    using traits = output_t::traits;
    using iterator = typename base_t::iterator;

private:
    const base_t& base;
public:

    bit_casted_view(const base_t& base_container): base(base_container) {}

    // If this is downcast or upcast. Downcast = we re-interpret a wide unit
    // as a narrower unit (e.g. uint64 → uint32); upcast = the reverse. The
    // sizeof comparison here is on the *storage_unit*, not on the wrapping
    // sequence type — the wrapping types are the same size (both wrap a
    // std::vector + size_t) regardless of the storage_unit.
    static constexpr bool is_down_cast =
        sizeof(typename traits::storage_unit) <=
        sizeof(typename base_traits::storage_unit);
    // Ratio between source and target unit widths in bits — always >= 1.
    static constexpr size_t cast_ratio = is_down_cast
        ? (base_traits::unit_width / traits::unit_width)
        : (traits::unit_width / base_traits::unit_width);
    // Log2 of cast_ratio (useful for shifts).
    static constexpr size_t cast_ratio_log = seqan3::detail::ceil_log2(cast_ratio);

    // Verifies that cast_ratio is power of two, otherwise advance_iter's
    // shift-as-divide / shift-as-multiply lowering would be wrong.
    static_assert(1 << cast_ratio_log == cast_ratio);

    [[always_inline]] constexpr size_t bp_size() const {
        // Number of letters stays the same, we only change interpretation not the value itself
        return base.bp_size();
    };

    [[always_inline]] constexpr size_t byte_size() const {
        // We don't change underlying values so it stays the same
        return base.byte_size();
    };

    [[always_inline]] constexpr size_t size() const {
        return view_units_for_letters();
    };

    [[always_inline]] constexpr size_t padding() const {
        // Empty letter slots in the trailing view-unit: units*lpu - bps.
        return view_units_for_letters() * traits::letters_per_unit - base.bp_size();
    };

private:
    /// Ceil-divide bp_size by view's letters_per_unit. Inlined here because
    /// `int_div_ceil` lives in `bioseq_sequences.simd`, which imports this
    /// module — pulling it in would create a cycle. Centralising the
    /// computation also keeps `size()` and `padding()` consistent (both
    /// depend on the same ceil result).
    [[always_inline]] constexpr size_t view_units_for_letters() const {
        constexpr size_t lpu = traits::letters_per_unit;
        const size_t bps = base.bp_size();
        return (bps + lpu - 1) / lpu;
    }
public:

    template<typename I>
    requires(base_t::template is_valid_derived_iter<I>::value)
    [[always_inline]] constexpr const typename traits::storage_unit* read_cptr(const I& it) const {
        return reinterpret_cast<const typename traits::storage_unit*>(base.read_cptr(it));
    };

    /// Forward base's `cbegin()` so callers that consume the
    /// `supports_compact_sequence_bitcast_view` concept can fetch the
    /// initial iterator uniformly across `compact_sequence_range` and
    /// `bit_casted_view`. Returns a base-side iterator that this view's
    /// `read_cptr` accepts (per the `is_valid_derived_iter` constraint).
    [[always_inline]] constexpr iterator cbegin() const {
        return base.cbegin();
    }
    [[always_inline]] constexpr iterator begin() const {
        return base.cbegin();
    }

    template<typename I>
    requires(base_t::template is_valid_derived_iter<I>::value)
    [[always_inline]] constexpr static void advance_iter(I& it, const size_t value) {
        // This is interesting part:
        // - downcast: one real unit covers `cast_ratio` view units, so each
        //   `cast_ratio` view-unit steps maps to 1 real iter increment. We
        //   require `value` divisible by `cast_ratio` so the shift-as-divide
        //   below is exact (otherwise we'd need pointer arithmetic). The
        //   only call site today is the SIMD chunk loop which advances by
        //   `Lanes(d)` — a power-of-two that scales the same way as
        //   `cast_ratio`, so the divisibility holds by construction.
        // - upcast: each view-unit step covers `cast_ratio` real units, so
        //   we advance by `value * cast_ratio`. No divisibility constraint.
        if constexpr (is_down_cast) {
            DEBUG_ASSERT(value % cast_ratio == 0,
                fmt::format("bit_casted_view downcast (ratio={}) requires advance"
                    " value ({}) divisible by cast_ratio for zero-cost shift-as-divide",
                    cast_ratio, value));
            it += (value >> cast_ratio_log);
        } else {
            it += (value << cast_ratio_log);
        }
    };

    /// Distance between two iterators in *view* storage_unit steps.
    /// For a downcast (large→small) view, every base-iter step covers
    /// `cast_ratio` view-units, so the raw `b - a` is multiplied by
    /// `cast_ratio`. For an upcast (small→large), each view-unit takes
    /// `cast_ratio` base-iter steps, so the raw `b - a` is divided by
    /// `cast_ratio`. Mirrors the `compact_sequence_range_base::view_distance`
    /// passthrough so v2 can use one expression regardless of the view
    /// flavour.
    template<typename I>
    [[always_inline]] constexpr static size_t view_distance(const I& a, const I& b) {
        if constexpr (is_down_cast) {
            return static_cast<size_t>(b - a) << cast_ratio_log;
        } else {
            return static_cast<size_t>(b - a) >> cast_ratio_log;
        }
    }
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
    constexpr std::optional<size_t> slow_find_symbols_left(I begin, I end, std::vector<typename traits::alphabet_type> sub_seq, size_t start_symbol_pos = 0) const {
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
    constexpr std::string to_string(I begin, I end, bool split_units = false) const {
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

    constexpr std::string to_string(bool split_units = false) const {
        return to_string(super()->cbegin(), super()->cend(), split_units);
    }

    constexpr size_t bp_size() const {
        return super()->size() * traits::letters_per_unit - super()->padding();
    }

    constexpr size_t byte_size() const {
        return super()->size() * sizeof(typename traits::storage_unit); //+ sizeof(derived_range_t);
    }

    /// DOC0286 §10.0b — zero-copy split of the packed range into records no
    /// larger than `record_bp` bases. `record_bp` MUST be a multiple of
    /// `traits::letters_per_unit` (unit-aligned ⇒ each record starts on a
    /// storage-unit boundary, so this is a pure offset computation — no decode,
    /// no copy). Returns (unit_offset, unit_count, padding) triples; the final
    /// record carries the range's trailing padding, the rest carry 0. The
    /// caller (e.g. the E2E bench FASTA splitter) turns each triple into a
    /// `compact_sequence_view`. Returning offsets rather than views keeps the
    /// layering boundary: the view lives in `bioseq_io.public_api`, and
    /// importing io into `bioseq_sequences` would invert the layering.
    [[nodiscard]] std::vector<std::tuple<std::size_t, std::size_t, std::size_t>>
    split_units(std::size_t record_bp) const {
        DEBUG_ASSERT(record_bp % traits::letters_per_unit == 0,
            "split_units: record_bp must be a multiple of letters_per_unit");
        const std::size_t units_per_record = record_bp / traits::letters_per_unit;
        const std::size_t total_units = super()->size();
        std::vector<std::tuple<std::size_t, std::size_t, std::size_t>> out;
        if (total_units == 0) { out.emplace_back(0u, 0u, 0u); return out; }
        for (std::size_t off = 0; off < total_units; off += units_per_record) {
            const std::size_t count   = std::min(units_per_record, total_units - off);
            const bool        is_last = (off + count >= total_units);
            out.emplace_back(off, count, is_last ? super()->padding() : std::size_t{0});
        }
        return out;
    }

    /// Advance an iterator by `value` storage_unit steps. Mirrors
    /// `bit_casted_view::advance_iter` so callers can use the same
    /// "view-aware" iterator-step API regardless of whether they hold a
    /// raw `compact_sequence_range<R>` or a `bit_casted_view<…>`. For a
    /// plain compact range there is no scale factor — one storage_unit
    /// step is one base-iter increment — so this just forwards to
    /// `it += value`.
    template<typename I>
    [[always_inline]] static inline void advance_iter(I& it, const size_t value) {
        it += value;
    }

    /// Distance between two iterators in storage_unit steps, view-aware.
    /// For a plain `compact_sequence_range<R>` the iterator's `operator-`
    /// already yields units — this is just a pass-through. The
    /// `bit_casted_view<…>` overload scales by `cast_ratio` so callers
    /// see the result in *view* units regardless of the underlying base
    /// iterator's native unit.
    template<typename I>
    [[always_inline]] static inline size_t view_distance(const I& a, const I& b) {
        return static_cast<size_t>(b - a);
    }

    // DOC0279 §17/§12.5 — the POC count_symbols() stub (a hardcoded 49 for
    // rank 0) is deleted. Per-symbol counting is now the real tally in
    // bioseq_indexing.symbol_counts: tally_symbol_counts(lazy) returns an
    // int64[2^bits_per_char] histogram, persisted via the SymbolCountsIndex
    // INLINE index and recomputed on demand (no more garbage 49 stub).

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

// =====================================================================
// DOC0258: Postgres-agnostic compact_sequence_stdc + zero-copy
// compact_sequence_range<S> view.
//
// The new struct replaces the duplicated `PgCompactSequenceHdr` layout
// that previously lived in `pg_compact_sequence.cppm`. The sequences
// module owns the layout; the postgres translation unit binds
// postgres-specific allocators / readers / writers via lambdas.
// =====================================================================

namespace compact_sequence_stdc_detail {

template<size_t Deficit>
consteval auto pick_alignment_reserved_type() {
    if constexpr (Deficit == 0) return std::type_identity<void>{};
    else if constexpr (Deficit == 1) return std::type_identity<std::uint8_t>{};
    else if constexpr (Deficit == 2) return std::type_identity<std::uint16_t>{};
    else if constexpr (Deficit == 4) return std::type_identity<std::uint32_t>{};
    else                              return std::type_identity<std::array<std::uint8_t, Deficit>>{};
}

template<size_t varlena_bytes, size_t code_bytes, size_t bp_size_bytes>
consteval size_t reserved_deficit() {
    constexpr size_t fixed_bytes =
        varlena_bytes + code_bytes
        + 1u /* storage_unit_byte_width */
        + 2u /* padding_letters */
        + bp_size_bytes;
    constexpr size_t target = 8u;
    return (target - (fixed_bytes % target)) % target;
}

template<size_t Deficit>
using alignment_reserved_for_t =
    typename decltype(pick_alignment_reserved_type<Deficit>())::type;

template<size_t Deficit>
struct alignment_reserved_holder {
    alignment_reserved_for_t<Deficit> reserved;
};
template<>
struct alignment_reserved_holder<0> { };

}  // namespace compact_sequence_stdc_detail

/// User-facing pair (alphabet_code, storage_unit_byte_width). The encode/
/// decode pair preserves today's wire-format encoding (low byte = log2 of
/// the storage_unit byte width) so the postgres regress suite produces a
/// zero diff (DOC0258 §5.9).
struct compact_sequence_stdc_type_params {
    std::uint8_t alphabet_code;
    std::uint8_t storage_unit_byte_width;

    [[nodiscard]] constexpr std::int32_t encode() const noexcept {
        const std::uint8_t width_code =
              storage_unit_byte_width == 1 ? 0u
            : storage_unit_byte_width == 2 ? 1u
            : storage_unit_byte_width == 4 ? 2u
            : storage_unit_byte_width == 8 ? 3u
            : 0xFFu;
        DEBUG_ASSERT(width_code != 0xFFu,
                     fmt::format("encode: storage_unit_byte_width {} invalid",
                                 static_cast<unsigned>(storage_unit_byte_width)));
        return (static_cast<std::int32_t>(alphabet_code) << 8) | width_code;
    }

    [[nodiscard]] static constexpr compact_sequence_stdc_type_params
    decode(std::int32_t typmod) noexcept {
        const std::uint8_t width_code =
            static_cast<std::uint8_t>(typmod & 0xFF);
        DEBUG_ASSERT(width_code <= 3u,
                     fmt::format("decode: width_code {} > 3",
                                 static_cast<unsigned>(width_code)));
        return {
            .alphabet_code           = static_cast<std::uint8_t>((typmod >> 8) & 0xFF),
            .storage_unit_byte_width = static_cast<std::uint8_t>(1u << width_code),
        };
    }

    [[nodiscard]] constexpr bool operator==(
        const compact_sequence_stdc_type_params&) const = default;
};

/// Allocator concept: a callable that takes a size_t byte count and
/// returns a pointer to zero-initialised storage of `Self*` aligned to
/// alignof(Self). Returns nullptr on failure (allocation contract from
/// DOC0258 §5.2, §13.1.1).
template<class Alloc, class Self>
concept stdc_allocator = requires (Alloc&& a, size_t bytes) {
    { std::forward<Alloc>(a)(bytes) } -> std::convertible_to<Self*>;
};

// Concepts declared BEFORE the instance forward declaration so the
// constraints below can name them. The marker probes (`is_valid_*`)
// are SFINAE-friendly — the concepts can reference types that aren't
// yet defined.
template<class S>
concept is_compact_sequence_stdc =
    requires {
        requires S::is_valid_compact_sequence_stdc == true;
        typename S::supported_alphabets_type;
        typename S::bp_size_t;
        typename S::varlena_header_t;
        typename S::alphabet_code_t;
    } && is_supported_alphabets_v<typename S::supported_alphabets_type>;

// `is_compact_sequence_stdc_instance` is defined earlier in this module
// (near the primary `compact_sequence_range` template) so it can
// participate in `supports_compact_sequence_range`.

// Forward-declared instance marker; defined after compact_sequence_stdc
// so it can reference the enclosing stdc type via S. Constraints repeated
// exactly in the full definition so the redeclaration matches.
template<
    class S,
    sequence_alphabet alphabet_t,
    size_t storage_unit_byte_width
>
requires (
    is_compact_sequence_stdc<S>
    && is_in_supported_alphabets_v<typename S::supported_alphabets_type, alphabet_t>
    && (storage_unit_byte_width == 1 || storage_unit_byte_width == 2
        || storage_unit_byte_width == 4 || storage_unit_byte_width == 8)
)
struct compact_sequence_stdc_instance;

/// The standard-layout, C-compatible compact sequence struct. Generic
/// over the alphabet pack, the bp_size integer width, and the varlena
/// header type. `data[]` is a flexible-array member that holds the
/// packed payload; the `_reserved` field bridges the fixed-size header
/// to a uint64_t-aligned `data[]` boundary via the deficit calculation
/// in `compact_sequence_stdc_detail`.
template<
    class supported_alphabets_t,
    class p_bp_size_t = std::uint32_t,
    class p_varlena_header_t = std::int32_t
>
requires (is_supported_alphabets_v<supported_alphabets_t>)
struct compact_sequence_stdc {
public:
    using supported_alphabets_type = supported_alphabets_t;
    using bp_size_t                = p_bp_size_t;
    using varlena_header_t         = p_varlena_header_t;
    using alphabet_code_t          = typename supported_alphabets_t::alphabet_code_t;

    static constexpr bool is_valid_compact_sequence_stdc = true;

    static constexpr size_t alignment_deficit =
        compact_sequence_stdc_detail::reserved_deficit<
            sizeof(varlena_header_t),
            sizeof(alphabet_code_t),
            sizeof(bp_size_t)
        >();

    varlena_header_t      vl_len_;                  // varlena header
    alphabet_code_t       alphabet_code;
    std::uint8_t          storage_unit_byte_width; // note: this is width of storage units in bytes, so normally it won't be larger than 8 (8 => uint64_t), because no sensible arch supports native 128-width ints
    std::uint16_t         padding_letters;
    bp_size_t             bp_size;
    [[no_unique_address]]
    compact_sequence_stdc_detail::alignment_reserved_holder<alignment_deficit> _reserved;
    std::uint8_t          data[1];

    /// Bytes from the start of the struct to `data[]`. Used by every
    /// factory to size the alloc.
    [[nodiscard]] static constexpr size_t header_bytes() noexcept {
        return offsetof(compact_sequence_stdc, data);
    }

    /// Encode `total_bytes` as a PostgreSQL 4-byte (uncompressed) varlena
    /// header value for `vl_len_`. On little-endian platforms (the project
    /// target, macOS arm64) this is the exact bit pattern PG's
    /// `SET_VARSIZE_4B` writes: `length << 2`, leaving the low two format
    /// bits clear so PG's `VARATT_IS_4B_U` / `VARSIZE` read it as a plain
    /// uncompressed varlena of `total_bytes` bytes.
    ///
    /// We must NOT store the raw byte count here: although the C++ side
    /// never reads `vl_len_` (sizes are derived from `bp_size` /
    /// `unit_count()` / `header_bytes()`), every PostgreSQL varlena
    /// operation that touches the value — `datumCopy`, tuple storage,
    /// `PG_DETOAST_DATUM` with a copy — reads `VARSIZE(vl_len_) =
    /// vl_len_ >> 2`. A raw byte count makes VARSIZE return a quarter of the
    /// real size, truncating the value and corrupting every read that goes
    /// through PG's machinery (DOC0268 Appendix). We replicate the encoding
    /// here rather than call `SET_VARSIZE` because this module is shared with
    /// the NO_POSTGRES standalone build, which has no PostgreSQL headers.
    [[nodiscard]] static constexpr varlena_header_t
    encode_varlena_header(std::size_t total_bytes) noexcept {
        return static_cast<varlena_header_t>(
            static_cast<std::uint32_t>(total_bytes) << 2);
    }

    /// Runtime → compile-time fanout: pin the alphabet from alphabet_code,
    /// pin the storage_unit byte width, invoke `handler.template operator()
    /// <compact_sequence_stdc_instance<this-type, A, BW>>()` exactly once.
    template<class HandlerFn>
    [[gnu::always_inline]]
    static constexpr void dispatch_instance(
        alphabet_code_t alpha_code,
        std::uint8_t byte_width,
        HandlerFn&& handler)
    {
        supported_alphabets_t::dispatch_alphabet_by_code(alpha_code,
            [&]<class A>() {
                dispatch_storage_unit_byte_width(byte_width,
                    [&]<size_t BW>() {
                        handler.template operator()<
                            compact_sequence_stdc_instance<
                                compact_sequence_stdc, A, BW>
                        >();
                    });
            });
    }

    /// Convenience pass-through to the pack's name lookup so callers that
    /// already have a `compact_sequence_stdc*` don't have to spell out
    /// `supported_alphabets_t::name_of_alphabet_code(...)`.
    [[nodiscard]] static std::string_view name_of_alphabet_code(
        alphabet_code_t code) noexcept
    {
        return supported_alphabets_t::name_of_alphabet_code(code);
    }

    /// Streaming recv factory: reads `unit_count` units of `target_storage_byte_width`
    /// bytes each via `recv_fn`, validates against the alphabet's letters_per_unit,
    /// allocates the destination and fills its payload.
    template<
        sequence_alphabet alphabet_t,
        class RecvParam, class RecvFn,
        class Alloc
    >
    requires (
        is_in_supported_alphabets_v<supported_alphabets_t, alphabet_t>
        && stdc_allocator<Alloc, compact_sequence_stdc>
    )
    [[nodiscard]]
    static compact_sequence_stdc* convert_from(
        std::uint8_t target_storage_byte_width,
        std::uint16_t in_padding_letters,
        bp_size_t in_bp_size,
        std::uint32_t in_unit_count,
        RecvParam& recv_param,
        RecvFn&& recv_fn,
        Alloc&& alloc);

    /// Allocate an empty (zero bp_size, zero padding) sequence with the
    /// requested storage capacity. Caller fills `data[]` directly.
    template<
        sequence_alphabet alphabet_t,
        class Alloc
    >
    requires (
        is_in_supported_alphabets_v<supported_alphabets_t, alphabet_t>
        && stdc_allocator<Alloc, compact_sequence_stdc>
    )
    [[nodiscard]]
    static compact_sequence_stdc* reserve(
        std::uint8_t target_storage_byte_width,
        std::uint32_t unit_count_value,
        Alloc&& alloc);

    /// Walk the payload as uint64_t units of the recorded width, calling
    /// `send_fn` once per logical unit. Wraps a `dispatch_instance` so
    /// the unit count and letters_per_unit are constexpr for the loop.
    template<class SendParam, class SendFn>
    void send_to(SendParam& send_param, SendFn&& send_fn) const;

    /// Hand a `compact_sequence_range<S>{*this}` view to a templated
    /// handler. Dispatches once on (alphabet_code, storage_unit_byte_width).
    template<class HandlerFn>
    [[gnu::always_inline]]
    void convert_to(HandlerFn&& handler) const;

    /// Same-alphabet storage-width recast. Returns a freshly-allocated
    /// destination with the payload memcpy'd (no decode/encode).
    template<class Alloc>
    requires (stdc_allocator<Alloc, compact_sequence_stdc>)
    [[nodiscard]]
    compact_sequence_stdc* recast_storage_width(
        std::uint8_t target_byte_width,
        Alloc&& alloc) const;

    /// Logical storage unit count — recovered via dispatch on alphabet/width.
    [[nodiscard]] size_t unit_count() const noexcept;

    /// Logical payload byte count (excludes the up-to-7-byte uint64 round-up).
    [[nodiscard]] size_t payload_bytes() const noexcept {
        return unit_count() * storage_unit_byte_width;
    }

    [[nodiscard]] constexpr compact_sequence_stdc_type_params
    get_type_params() const noexcept {
        return {
            .alphabet_code           = static_cast<std::uint8_t>(alphabet_code),
            .storage_unit_byte_width = storage_unit_byte_width,
        };
    }
};

static_assert(
    std::is_standard_layout_v<
        compact_sequence_stdc<supported_alphabets<alphabet_entry<seq::dna4, "dna4">, alphabet_entry<seq::dna5, "dna5">>>>,
    "compact_sequence_stdc must be standard-layout for C-compatible storage");

namespace compact_sequence_stdc_detail {
template<class P, class BP, class VL>
struct data_offset_check {
    using stdc_t = compact_sequence_stdc<P, BP, VL>;
    static constexpr bool aligned =
        (offsetof(stdc_t, data) % sizeof(std::uint64_t) == 0);
};

static_assert(data_offset_check<supported_alphabets<alphabet_entry<seq::dna4, "dna4">, alphabet_entry<seq::dna5, "dna5">>,
                                std::uint32_t, std::int32_t>::aligned);
static_assert(data_offset_check<supported_alphabets<alphabet_entry<seq::dna4, "dna4">, alphabet_entry<seq::dna5, "dna5">>,
                                std::uint64_t, std::int32_t>::aligned);
static_assert(data_offset_check<supported_alphabets<alphabet_entry<seq::dna4, "dna4">, alphabet_entry<seq::dna5, "dna5">>,
                                std::uint16_t, std::int32_t>::aligned);
static_assert(data_offset_check<supported_alphabets<alphabet_entry<seq::dna4, "dna4">, alphabet_entry<seq::dna5, "dna5">>,
                                std::uint8_t,  std::int32_t>::aligned);
static_assert(data_offset_check<supported_alphabets<alphabet_entry<seq::dna4, "dna4">, alphabet_entry<seq::dna5, "dna5">>,
                                std::uint32_t, std::int64_t>::aligned);
}  // namespace compact_sequence_stdc_detail

/// Compile-time refinement of a `compact_sequence_stdc<…>` runtime type.
/// Pins the alphabet to `alphabet_t` and the storage_unit width to
/// `storage_unit_byte_width`. Carries no runtime state; it is purely a
/// type-level brand that wires (a) the bare struct, (b) the pinned
/// alphabet, and (c) the pinned storage width so the
/// `compact_sequence_range<S>` partial specialisation can dispatch on it.
template<
    class S,
    sequence_alphabet alphabet_t,
    size_t storage_unit_byte_width
>
requires (
    is_compact_sequence_stdc<S>
    && is_in_supported_alphabets_v<typename S::supported_alphabets_type, alphabet_t>
    && (storage_unit_byte_width == 1 || storage_unit_byte_width == 2
        || storage_unit_byte_width == 4 || storage_unit_byte_width == 8)
)
struct compact_sequence_stdc_instance {
    using compact_sequence_stdc_type = S;
    using alphabet_type              = alphabet_t;
    // DOC0268 (this run) — pin storage_unit to the std::uintN_t fixed-width
    // aliases instead of boost::uint_t<N>::least. On macOS arm64 (LP64) the
    // boost variant resolves to `unsigned long` for the 64-bit case, which
    // is a *different* C++ type from `unsigned long long` (which is what
    // std::uint64_t maps to). Highway's `ScalableTag<T>` lane-type traits
    // are specialised only for the fixed-width std::uintN_t types, so the
    // boost-derived `unsigned long` instantiation tripped
    // `IsIntegerLaneType<unsigned long>()` static_assert in
    // `hwy/ops/shared-inl.h` (the long-tail dispatch_buffer.cppm template
    // blocker the previous Phase-A report flagged as out-of-scope). Forcing
    // the alias to the std::uintN_t variant fixes the chain without
    // touching SIMD code.
    using storage_unit =
        std::conditional_t<storage_unit_byte_width == 8, std::uint64_t,
        std::conditional_t<storage_unit_byte_width == 4, std::uint32_t,
        std::conditional_t<storage_unit_byte_width == 2, std::uint16_t,
                                                          std::uint8_t>>>;

    static constexpr size_t byte_width        = storage_unit_byte_width;
    static constexpr size_t letter_bit_width  =
        seqan3::detail::ceil_log2(seqan3::alphabet_size<alphabet_t>);

    // DOC0261 §7 — alphabet name pinned at the type level via the pack
    // lookup. Templated code can emit log lines / error messages that
    // name the alphabet without re-deriving from the runtime byte.
    static constexpr auto alphabet_name =
        S::supported_alphabets_type::template name_of<alphabet_t>();
    static constexpr std::string_view alphabet_name_sv = alphabet_name.view();

    static constexpr bool is_valid_compact_sequence_stdc_instance = true;
};

/// Zero-copy view specialisation of `compact_sequence_range` for the
/// new `compact_sequence_stdc_instance<…>` brand. Holds a const reference
/// to the underlying `compact_sequence_stdc` and computes padding,
/// bp_size, size, and the read pointer directly off the header fields.
/// No `std::vector` payload; constructing the view is a single reference
/// assignment plus three `DEBUG_ASSERT`s.
template<typename S>
requires (is_compact_sequence_stdc_instance<S>)
struct compact_sequence_range<S> : public compact_sequence_range_base<
    compact_sequence_range<S>,
    typename S::alphabet_type,
    typename S::storage_unit,
    S::letter_bit_width
> {
public:
    using traits =
        typename compact_sequence_range<S>::compact_sequence_range_base::traits;
    using base_container =
        const typename S::compact_sequence_stdc_type*;
    // DOC0268 (this run) — keep the iterator's element type in lockstep
    // with `S::storage_unit`. The previous `boost::uint_t<...>::least`
    // resolved to `unsigned long` on macOS-arm64 (LP64), which is a
    // different C++ type from `unsigned long long` (= std::uint64_t)
    // chosen by S::storage_unit. The mismatch left
    // `read_cptr(const iterator&)` returning `const unsigned long *`
    // while the declared return type was `const unsigned long long *`,
    // tripping a hard compile error.
    using iterator =
        const typename S::storage_unit*;

private:
    const typename S::compact_sequence_stdc_type& _stdc;

public:
    explicit constexpr compact_sequence_range(
        const typename S::compact_sequence_stdc_type& stdc) noexcept
        : _stdc(stdc)
    {
        DEBUG_ASSERT(
            stdc.storage_unit_byte_width == S::byte_width,
            fmt::format("compact_sequence_range<S>: stdc width {} != "
                        "instance width {}",
                        static_cast<unsigned>(stdc.storage_unit_byte_width),
                        static_cast<unsigned>(S::byte_width)));
        DEBUG_ASSERT(
            stdc.padding_letters < traits::letters_per_unit
            || (size() == 0 && stdc.padding_letters == 0),
            fmt::format("padding_letters {} >= letters_per_unit {}",
                        static_cast<unsigned>(stdc.padding_letters),
                        traits::letters_per_unit));
        DEBUG_ASSERT(
            static_cast<size_t>(stdc.bp_size) + stdc.padding_letters
            == size() * traits::letters_per_unit
            || (size() == 0 && stdc.bp_size == 0 && stdc.padding_letters == 0),
            fmt::format("bp_size {} + padding {} != size {} * lpu {}",
                        static_cast<size_t>(stdc.bp_size),
                        static_cast<unsigned>(stdc.padding_letters),
                        size(), traits::letters_per_unit));
    }

    [[nodiscard]] constexpr size_t padding() const noexcept {
        return _stdc.padding_letters;
    }

    [[nodiscard]] constexpr size_t size() const noexcept {
        const size_t bp = _stdc.bp_size;
        if (bp == 0) return 0;
        return (bp + traits::letters_per_unit - 1) / traits::letters_per_unit;
    }

    [[nodiscard]] constexpr size_t bp_size() const noexcept {
        return static_cast<size_t>(_stdc.bp_size);
    }

    [[nodiscard]] constexpr iterator cbegin() const noexcept {
        return reinterpret_cast<iterator>(_stdc.data);
    }
    [[nodiscard]] constexpr iterator cend() const noexcept {
        return cbegin() + size();
    }
    [[nodiscard]] constexpr iterator begin() const noexcept { return cbegin(); }
    [[nodiscard]] constexpr iterator end() const noexcept { return cend(); }

    [[nodiscard]] constexpr typename traits::storage_unit operator[](
        size_t unit_index) const noexcept {
        return reinterpret_cast<const typename traits::storage_unit*>(_stdc.data)[unit_index];
    }

    [[nodiscard]] constexpr static const typename traits::storage_unit*
    read_cptr(const iterator& it) noexcept {
        return it;
    }

    [[nodiscard]] friend constexpr bool operator==(
        const compact_sequence_range<S>& a,
        const compact_sequence_range<S>& b)
    {
        if (&a._stdc == &b._stdc) return true;
        if (a._stdc.bp_size != b._stdc.bp_size) return false;
        return std::memcmp(a._stdc.data, b._stdc.data,
            a.size() * sizeof(typename traits::storage_unit)) == 0;
    }
};

// Disambiguation: the instance type must NOT also satisfy the two
// concepts that drive the existing compact_sequence_range partial
// specialisations. A future edit that broke this would manifest as an
// "ambiguous partial specialisation" diagnostic at every use site;
// catch it here with a one-line message instead.
namespace compact_sequence_stdc_detail {
using _check_instance_t = compact_sequence_stdc_instance<
    compact_sequence_stdc<supported_alphabets<alphabet_entry<seq::dna4, "dna4">, alphabet_entry<seq::dna5, "dna5">>>,
    seq::dna4,
    size_t{8}>;
static_assert(!is_sequence_custom_alphabet<_check_instance_t>,
              "compact_sequence_stdc_instance must NOT satisfy "
              "is_sequence_custom_alphabet (partial-spec disambiguation)");
static_assert(!sequence_proper_container<_check_instance_t>,
              "compact_sequence_stdc_instance must NOT satisfy "
              "sequence_proper_container (partial-spec disambiguation)");
static_assert(is_compact_sequence_stdc_instance<_check_instance_t>);
}  // namespace compact_sequence_stdc_detail

};  // close the second `export {` block (opened near line 171) so the
    // out-of-line member function template definitions below are not
    // mistakenly re-exported by the surrounding block. Member
    // definitions inherit their linkage from the class declaration.

// ---------------------------------------------------------------------
// compact_sequence_stdc member function template definitions
// (out-of-line). The SIMD function-template forward declarations live
// at the top of this module fragment; importers reach the actual
// definitions via `import bioseq_sequences.simd;`.
// ---------------------------------------------------------------------

template<class supported_alphabets_t, class p_bp_size_t, class p_varlena_header_t>
requires (is_supported_alphabets_v<supported_alphabets_t>)
template<class HandlerFn>
[[gnu::always_inline]]
inline void compact_sequence_stdc<supported_alphabets_t, p_bp_size_t, p_varlena_header_t>::convert_to(HandlerFn&& handler) const
{
    dispatch_instance(alphabet_code, storage_unit_byte_width,
        [&]<class S>() {
            handler.template operator()<S>(
                compact_sequence_range<S>{*this});
        });
}

template<class supported_alphabets_t, class p_bp_size_t, class p_varlena_header_t>
requires (is_supported_alphabets_v<supported_alphabets_t>)
template<class SendParam, class SendFn>
inline void compact_sequence_stdc<supported_alphabets_t, p_bp_size_t, p_varlena_header_t>::send_to(
    SendParam& send_param, SendFn&& send_fn) const
{
    dispatch_instance(alphabet_code, storage_unit_byte_width,
        [&]<class S>() {
            using storage_unit_t = typename S::storage_unit;
            constexpr std::size_t lpu =
                compact_sequence_range<S>::traits::letters_per_unit;
            const std::size_t units = (bp_size == 0) ? std::size_t{0}
                : (bp_size + lpu - 1) / lpu;
            const auto* reader =
                reinterpret_cast<const storage_unit_t*>(this->data);
            for (std::size_t i = 0; i < units; ++i) {
                send_fn.template operator()<std::uint64_t>(
                    send_param,
                    static_cast<std::uint64_t>(reader[i]),
                    static_cast<std::uint8_t>(S::byte_width));
            }
        });
}

template<class supported_alphabets_t, class p_bp_size_t, class p_varlena_header_t>
requires (is_supported_alphabets_v<supported_alphabets_t>)
inline std::size_t compact_sequence_stdc<supported_alphabets_t, p_bp_size_t, p_varlena_header_t>::unit_count() const noexcept
{
    std::size_t result = 0;
    dispatch_instance(alphabet_code, storage_unit_byte_width,
        [&]<class S>() {
            constexpr std::size_t lpu =
                compact_sequence_range<S>::traits::letters_per_unit;
            result = (bp_size == 0) ? std::size_t{0}
                : (bp_size + lpu - 1) / lpu;
        });
    return result;
}

template<class supported_alphabets_t, class p_bp_size_t, class p_varlena_header_t>
requires (is_supported_alphabets_v<supported_alphabets_t>)
template<sequence_alphabet alphabet_t, class Alloc>
requires (
    is_in_supported_alphabets_v<supported_alphabets_t, alphabet_t>
    && stdc_allocator<Alloc, compact_sequence_stdc<supported_alphabets_t, p_bp_size_t, p_varlena_header_t>>
)
inline compact_sequence_stdc<supported_alphabets_t, p_bp_size_t, p_varlena_header_t>*
compact_sequence_stdc<supported_alphabets_t, p_bp_size_t, p_varlena_header_t>::reserve(
    std::uint8_t target_storage_byte_width,
    std::uint32_t unit_count_value,
    Alloc&& alloc)
{
    DEBUG_ASSERT(
        target_storage_byte_width == 1 || target_storage_byte_width == 2
        || target_storage_byte_width == 4 || target_storage_byte_width == 8,
        fmt::format("reserve: storage_unit_byte_width {} not in {{1,2,4,8}}",
                    static_cast<unsigned>(target_storage_byte_width)));

    const std::size_t payload_bytes_value =
        static_cast<std::size_t>(unit_count_value) * target_storage_byte_width;
    const std::size_t total_bytes =
        header_bytes() + round_up_to_uint64(payload_bytes_value);

    auto* hdr = std::forward<Alloc>(alloc)(total_bytes);
    if (hdr == nullptr) return nullptr;
    hdr->vl_len_                 = encode_varlena_header(total_bytes);
    hdr->alphabet_code           =
        supported_alphabets_t::template index_of<alphabet_t>();
    hdr->storage_unit_byte_width = target_storage_byte_width;
    hdr->padding_letters         = 0;
    hdr->bp_size                 = 0;
    return hdr;
}

template<class supported_alphabets_t, class p_bp_size_t, class p_varlena_header_t>
requires (is_supported_alphabets_v<supported_alphabets_t>)
template<
    sequence_alphabet alphabet_t,
    class RecvParam, class RecvFn,
    class Alloc
>
requires (
    is_in_supported_alphabets_v<supported_alphabets_t, alphabet_t>
    && stdc_allocator<Alloc, compact_sequence_stdc<supported_alphabets_t, p_bp_size_t, p_varlena_header_t>>
)
inline compact_sequence_stdc<supported_alphabets_t, p_bp_size_t, p_varlena_header_t>*
compact_sequence_stdc<supported_alphabets_t, p_bp_size_t, p_varlena_header_t>::convert_from(
    std::uint8_t target_storage_byte_width,
    std::uint16_t in_padding_letters,
    p_bp_size_t in_bp_size,
    std::uint32_t in_unit_count,
    RecvParam& recv_param,
    RecvFn&& recv_fn,
    Alloc&& alloc)
{
    DEBUG_ASSERT(
        target_storage_byte_width == 1 || target_storage_byte_width == 2
        || target_storage_byte_width == 4 || target_storage_byte_width == 8,
        fmt::format("convert_from(recv): storage_unit_byte_width {} not in {{1,2,4,8}}",
                    static_cast<unsigned>(target_storage_byte_width)));

    compact_sequence_stdc* result = nullptr;
    dispatch_storage_unit_byte_width(target_storage_byte_width,
        [&]<std::size_t BW>() {
            using storage_unit_t = typename boost::uint_t<BW * 8>::least;
            using sca_t = sequence_custom_alphabet<
                alphabet_t, storage_unit_t,
                seqan3::detail::ceil_log2(seqan3::alphabet_size<alphabet_t>)>;
            constexpr std::size_t lpu =
                compact_sequence_range<sca_t>::traits::letters_per_unit;

            const std::uint64_t derived_unit_count =
                (static_cast<std::uint64_t>(in_bp_size) + lpu - 1) / lpu;
            const std::uint64_t derived_padding =
                (in_bp_size == 0) ? std::uint64_t{0}
                : (derived_unit_count * lpu) - static_cast<std::uint64_t>(in_bp_size);
            ASSERT(static_cast<std::uint64_t>(in_unit_count) == derived_unit_count,
                   fmt::format("convert_from(recv): unit_count {} != "
                               "derived {} from bp_size {} lpu {}",
                               in_unit_count,
                               static_cast<std::size_t>(derived_unit_count),
                               static_cast<std::size_t>(in_bp_size), lpu));
            ASSERT(static_cast<std::uint64_t>(in_padding_letters) == derived_padding,
                   fmt::format("convert_from(recv): padding {} != derived {} "
                               "from bp_size {} lpu {}",
                               static_cast<unsigned>(in_padding_letters),
                               static_cast<std::size_t>(derived_padding),
                               static_cast<std::size_t>(in_bp_size), lpu));
            ASSERT(in_padding_letters < lpu
                   || (in_unit_count == 0 && in_padding_letters == 0),
                   fmt::format("convert_from(recv): padding {} >= lpu {}",
                               static_cast<unsigned>(in_padding_letters), lpu));

            const std::size_t payload_bytes_value =
                static_cast<std::size_t>(in_unit_count) * BW;
            const std::size_t total_bytes =
                header_bytes() + round_up_to_uint64(payload_bytes_value);

            compact_sequence_stdc* hdr = std::forward<Alloc>(alloc)(total_bytes);
            if (hdr == nullptr) {
                result = nullptr;
                return;
            }
            hdr->vl_len_                 = encode_varlena_header(total_bytes);
            hdr->alphabet_code           =
                supported_alphabets_t::template index_of<alphabet_t>();
            hdr->storage_unit_byte_width = static_cast<std::uint8_t>(BW);
            hdr->padding_letters         = in_padding_letters;
            hdr->bp_size                 = in_bp_size;

            auto* writer = reinterpret_cast<storage_unit_t*>(hdr->data);
            for (std::uint32_t i = 0; i < in_unit_count; ++i) {
                writer[i] = static_cast<storage_unit_t>(
                    recv_fn.template operator()<alphabet_t, std::uint64_t>(
                        recv_param,
                        static_cast<std::uint8_t>(BW)));
            }
            result = hdr;
        });
    return result;
}

template<class supported_alphabets_t, class p_bp_size_t, class p_varlena_header_t>
requires (is_supported_alphabets_v<supported_alphabets_t>)
template<class Alloc>
requires (stdc_allocator<Alloc, compact_sequence_stdc<supported_alphabets_t, p_bp_size_t, p_varlena_header_t>>)
inline compact_sequence_stdc<supported_alphabets_t, p_bp_size_t, p_varlena_header_t>*
compact_sequence_stdc<supported_alphabets_t, p_bp_size_t, p_varlena_header_t>::recast_storage_width(
    std::uint8_t target_byte_width,
    Alloc&& alloc) const
{
    ASSERT(
        target_byte_width == 1 || target_byte_width == 2
        || target_byte_width == 4 || target_byte_width == 8,
        fmt::format("recast_storage_width: target_byte_width {} not in {{1,2,4,8}}",
                    static_cast<unsigned>(target_byte_width)));

    std::uint16_t new_padding = 0;
    std::size_t   payload_bytes_value = 0;
    convert_to([&]<class S>(const compact_sequence_range<S>& /*src*/) {
        dispatch_storage_unit_byte_width(target_byte_width,
            [&]<std::size_t TgtBW>() {
                constexpr std::size_t tgt_lpu =
                    (TgtBW * 8) / S::letter_bit_width;
                const std::uint64_t derived_units = (bp_size == 0) ? std::uint64_t{0}
                    : (static_cast<std::uint64_t>(bp_size) + tgt_lpu - 1) / tgt_lpu;
                new_padding   = static_cast<std::uint16_t>(
                    (bp_size == 0) ? 0u
                    : (derived_units * tgt_lpu - static_cast<std::uint64_t>(bp_size)));
                payload_bytes_value =
                    static_cast<std::size_t>(derived_units) * TgtBW;
            });
    });

    const std::size_t total_bytes =
        header_bytes() + round_up_to_uint64(payload_bytes_value);
    compact_sequence_stdc* dst = std::forward<Alloc>(alloc)(total_bytes);
    if (dst == nullptr) return nullptr;
    dst->vl_len_                 = encode_varlena_header(total_bytes);
    dst->alphabet_code           = alphabet_code;
    dst->storage_unit_byte_width = target_byte_width;
    dst->bp_size                 = bp_size;
    dst->padding_letters         = new_padding;

    if (payload_bytes_value > 0) {
        std::memcpy(dst->data, this->data, payload_bytes_value);
    }
    return dst;
}

export {  // reopen export block to match the close before the
          // out-of-line member definitions block above

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
// Sanity check we should be able to bitcast dna4 sequence to uint64/32/16/8
static_assert(is_sequence_bit_castable_to<seq::dna4, uint64_t>);
static_assert(is_sequence_bit_castable_to<seq::dna4, uint32_t>);
static_assert(is_sequence_bit_castable_to<seq::dna4, uint16_t>);
static_assert(is_sequence_bit_castable_to<seq::dna4, uint8_t>);

// Common minimal interface that both `compact_sequence_range<R>` and
// `bit_casted_view<base, output>` satisfy. Pegs the contract that
// `iterate_sequence_bp_simd_v2` consumes — `bp_size()`, `byte_size()`,
// `size()`, `padding()`, `read_cptr(it)`, `advance_iter(it, n)`,
// `view_distance(a, b)`. Useful when v2 wants to drive iteration over
// either a raw compact range or a view that re-interprets its underlying
// buffer as a different storage_unit width (T4: bit-casted-view substrate).
//
// `read_cptr` is constrained with `convertible_to<…*>` (not `same_as`) to
// admit forward-compatible refinements where the view's pointer might be
// to a const-stricter or qualified-different but compatible type.
template <typename V>
concept supports_compact_sequence_bitcast_view =
    requires (V v, typename V::iterator& it, const typename V::iterator& cit, size_t n) {
        typename V::traits;
        typename V::traits::storage_unit;
        typename V::traits::alphabet_type;
        { v.bp_size()                   } -> std::convertible_to<size_t>;
        { v.byte_size()                 } -> std::convertible_to<size_t>;
        { v.size()                      } -> std::convertible_to<size_t>;
        { v.padding()                   } -> std::convertible_to<size_t>;
        { v.read_cptr(cit)              } -> std::convertible_to<const typename V::traits::storage_unit*>;
        { V::advance_iter(it, n)        };
        { V::view_distance(cit, cit)    } -> std::convertible_to<size_t>;
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

