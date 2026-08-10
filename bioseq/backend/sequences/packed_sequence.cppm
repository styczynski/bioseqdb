/**
 * @file packed_sequence.cppm
 * @brief Packed sequence traits, iteration, dispatch, and storage views.
 *
 * The module keeps alphabet identity at the type level while exposing lazy,
 * zero-copy views over LSB-first packed storage. Width recasting preserves the
 * logical alphabet, padding, and sequence length.
 */
module;

#include <libassert/assert.hpp>

#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iterator>
#include <list>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <valarray>
#include <vector>
#include <algorithm>
#include <limits>

#include <boost/integer.hpp>
#include <boost/iterator/iterator_adaptor.hpp>

export module bioseq.sequences:packed_sequence;

import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.common.templating;
import bioseq.sequences.rank_pack_core;

export {

/** @brief Detect whether a type instantiates a selected class template. */
template<
    typename p_candidate_type,
    template<typename... p_template_arguments> typename p_template_type>
inline constexpr bool is_instance_of_v = std::false_type{};

template<
    template<typename... p_template_arguments> typename p_template_type,
    typename... p_arguments>
inline constexpr bool
    is_instance_of_v<p_template_type<p_arguments...>, p_template_type> = true;

/** @brief Alphabet type exposing rank conversion and a static cardinality. */
template<typename p_alphabet_type>
concept sequence_alphabet =
    requires (const p_alphabet_type symbol) {
        { seqan3::to_rank(symbol) };
        { seqan3::alphabet_size<p_alphabet_type> };
    };

/**
 * @brief Compile-time configuration for one packed sequence representation.
 * @tparam p_alphabet Logical sequence alphabet.
 * @tparam p_storage_type Unsigned word used for packed ranks.
 * @tparam p_bits_per_character Rank bits stored for each logical symbol.
 */
template<
    sequence_alphabet p_alphabet,
    typename p_storage_type = typename boost::uint_t<
        seqan3::detail::ceil_log2(seqan3::alphabet_size<p_alphabet>)>::least,
    std::size_t p_bits_per_character =
        seqan3::detail::ceil_log2(seqan3::alphabet_size<p_alphabet>)
>
struct sequence_custom_alphabet {
static_assert(p_bits_per_character <= sizeof(p_storage_type) * 8);
static_assert(p_bits_per_character > 0);
static_assert(sizeof(p_storage_type) > 0);

public:
    using alphabet_type = p_alphabet;
    using storage_unit = p_storage_type;
    static constexpr std::size_t letter_bit_width = p_bits_per_character;
    static constexpr bool is_valid_sequence_custom_alphabet = true;

    static constexpr std::size_t leftover_bits =
        (sizeof(alphabet_type) * 8) % letter_bit_width;
};

/** @brief Detect a `sequence_custom_alphabet` configuration. */
template <typename p_candidate_type>
concept is_sequence_custom_alphabet =
    p_candidate_type::is_valid_sequence_custom_alphabet == true;

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
/** @brief Packed configurations that can share storage through a bit-cast view. */
template <typename p_source_type, typename p_destination_type>
concept is_sequence_custom_alphabet_bit_castable_to = (
    is_sequence_custom_alphabet<p_source_type>
    && is_sequence_custom_alphabet<p_destination_type>
    && std::same_as<typename p_source_type::alphabet_type,
                    typename p_destination_type::alphabet_type>
    && p_source_type::letter_bit_width == p_destination_type::letter_bit_width
    && ((sizeof(typename p_source_type::storage_unit) * 8)
            % (sizeof(typename p_destination_type::storage_unit) * 8) == 0
        || (sizeof(typename p_destination_type::storage_unit) * 8)
            % (sizeof(typename p_source_type::storage_unit) * 8) == 0)
    && p_source_type::leftover_bits == 0
    && p_destination_type::leftover_bits == 0
);

/** @brief Random-access sized range whose elements are alphabet symbols. */
template<typename p_range_type>
concept sequence_proper_container =
    sequence_alphabet<std::remove_reference_t<
        std::ranges::range_reference_t<p_range_type>>>
    && std::ranges::sized_range<p_range_type>
    && std::ranges::random_access_range<p_range_type>;

// Brand-marker probe used by the zero-copy `compact_sequence_range<S>`
// specialization. The actual
// `compact_sequence_stdc_instance` template is defined later in this
// module; the concept itself only inspects a member name so the
// forward use here is well-formed.
template<typename p_candidate_type>
concept is_compact_sequence_stdc_instance =
    requires { p_candidate_type::is_valid_compact_sequence_stdc_instance; }
    && p_candidate_type::is_valid_compact_sequence_stdc_instance == true;

/** @brief Types admitted by `compact_sequence_range` specializations. */
template<typename p_range_type>
concept supports_compact_sequence_range =
    is_sequence_custom_alphabet<p_range_type>
    || sequence_proper_container<p_range_type>
    || sequence_alphabet<p_range_type>
    || is_compact_sequence_stdc_instance<p_range_type>;

/**
 * @brief Decode one LSB-first packed sequence unit to alphabet characters.
 * @tparam p_storage_unit Unsigned packed-word type.
 * @tparam p_alphabet_type Sequence alphabet represented by the packed ranks.
 * @param value Packed source word.
 * @param complement_symbols Complement each decoded symbol when supported.
 * @param reverse_symbols Reverse the decoded character order.
 * @return One character per complete rank slot in the packed word.
 * @throws std::invalid_argument If complementation is requested for an
 * alphabet without a complement operation.
 */
template <
    std::unsigned_integral p_storage_unit,
    sequence_alphabet p_alphabet_type>
[[nodiscard]] std::string decode_packed_sequence_unit(
    p_storage_unit value,
    bool complement_symbols = false,
    bool reverse_symbols = false)
{
    constexpr std::size_t bits_per_character =
        seqan3::detail::ceil_log2(seqan3::alphabet_size<p_alphabet_type>);
    static_assert(bits_per_character > 0);
    constexpr std::size_t storage_bits =
        std::numeric_limits<p_storage_unit>::digits;
    constexpr std::size_t symbol_count = storage_bits / bits_per_character;
    constexpr p_storage_unit rank_mask = bits_per_character == storage_bits
        ? (std::numeric_limits<p_storage_unit>::max)()
        : static_cast<p_storage_unit>(
            (p_storage_unit{1} << bits_per_character) - p_storage_unit{1});

    std::string decoded(symbol_count, '*');
    p_alphabet_type alphabet{};
    for (std::size_t index = 0; index < symbol_count; ++index) {
        const auto rank = static_cast<p_storage_unit>(value & rank_mask);
        auto symbol = alphabet.assign_rank(rank);
        if (complement_symbols) {
            if constexpr (requires { seqan3::complement(symbol); }) {
                symbol = seqan3::complement(symbol);
            } else {
                throw std::invalid_argument(
                    "sequence alphabet does not support complementation");
            }
        }
        decoded[index] = symbol.to_char();
        value = static_cast<p_storage_unit>(value >> bits_per_character);
    }
    if (reverse_symbols) {
        std::reverse(decoded.begin(), decoded.end());
    }
    return decoded;
}

/**
 * @brief Convert alphabet symbols to their character representation.
 * @tparam p_alphabet_type Sequence alphabet type.
 * @param symbols Symbols to convert.
 * @return Contiguous character string in input order.
 */
template<sequence_alphabet p_alphabet_type>
[[nodiscard]] constexpr std::string symbols_vector_to_string(
    const std::vector<p_alphabet_type>& symbols)
{
    std::string result;
    result.reserve(symbols.size());
    for(const auto& letter : symbols) {
        result.push_back(letter.to_char());
    }
    return result;
}




/** @brief Primary template for packed and unpacked sequence range adapters. */
template<typename p_range_type>
struct compact_sequence_range {};

/** @brief Logical view exposed by a packed-sequence iterator. */
enum iterator_mode : std::uint8_t {
    /** Yield integer ranks without constructing alphabet objects. */
    logical_rank,
    /** Yield alphabet values reconstructed from packed ranks. */
    alphabet_symbol,
};

static_assert(__cplusplus >= 202302L,
              "packed sequence dispatch requires C++23 or later");

/**
 * @brief Type-level pairing of a sequence alphabet and its display name.
 * @tparam p_alphabet Sequence alphabet type.
 * @tparam p_name Compile-time display name used by runtime dispatch.
 */
template <sequence_alphabet p_alphabet,
          ::bioseq::common::templating::compile_time_string p_name>
struct alphabet_entry {
    using alphabet_type = p_alphabet;
    static constexpr auto alphabet_name = p_name;
    static constexpr std::size_t name_size = p_name.size();
    static constexpr std::string_view alphabet_name_sv = p_name.view();
    static constexpr bool is_valid_alphabet_entry = true;
};

template <class p_candidate_type>
concept is_alphabet_entry = requires {
    requires p_candidate_type::is_valid_alphabet_entry == true;
    typename p_candidate_type::alphabet_type;
    requires sequence_alphabet<typename p_candidate_type::alphabet_type>;
};

namespace dispatch_detail {

template <class p_left_entry, class p_right_entry>
[[nodiscard]] consteval bool entries_name_distinct() noexcept {
    if constexpr (
        p_left_entry::alphabet_name.size() != p_right_entry::alphabet_name.size()) {
        return true;
    } else {
        return !(p_left_entry::alphabet_name == p_right_entry::alphabet_name);
    }
}

template <std::size_t p_entry_index,
          class... p_entry_types,
          std::size_t... p_following_indices>
[[nodiscard]] consteval bool ith_name_unique(
    std::index_sequence<p_following_indices...>) noexcept
{
    using entries_tuple = std::tuple<p_entry_types...>;
    using selected_entry =
        std::tuple_element_t<p_entry_index, entries_tuple>;
    return ((entries_name_distinct<selected_entry,
              std::tuple_element_t<
                  p_entry_index + 1u + p_following_indices, entries_tuple>>())
            && ...);
}

template <class... p_entry_types>
[[nodiscard]] consteval bool unique_names_in_pack() noexcept {
    constexpr std::size_t entry_count = sizeof...(p_entry_types);
    if constexpr (entry_count <= 1) {
        return true;
    } else {
        return [&]<std::size_t... p_indices>(std::index_sequence<p_indices...>) {
            return (ith_name_unique<p_indices, p_entry_types...>(
                std::make_index_sequence<
                    (entry_count > p_indices + 1u
                        ? entry_count - p_indices - 1u : 0u)>{})
                && ...);
        }(std::make_index_sequence<entry_count>{});
    }
}

}  // namespace dispatch_detail

namespace detail_supported_alphabets {

template <class... p_entry_types>
consteval std::size_t valid_names_live_length() noexcept {
    constexpr std::size_t entry_count = sizeof...(p_entry_types);
    constexpr std::size_t names_bytes =
        (p_entry_types::alphabet_name.size() + ...);
    constexpr std::size_t quote_bytes  = 2u * entry_count;
    constexpr std::size_t separator_bytes =
        (entry_count > 1u ? 2u * (entry_count - 1u) : 0u);
    return names_bytes + quote_bytes + separator_bytes;
}

template <class... p_entry_types>
consteval auto build_valid_names_buffer() noexcept {
    constexpr std::size_t live_size =
        valid_names_live_length<p_entry_types...>();
    std::array<char, live_size> name_buffer{};
    std::size_t write_offset = 0;
    bool first_entry = true;
    auto append = [&]<class p_entry_type>() {
        if (!first_entry) {
            name_buffer[write_offset++] = ',';
            name_buffer[write_offset++] = ' ';
        }
        name_buffer[write_offset++] = '\'';
        for (std::size_t name_byte_index = 0;
             name_byte_index < p_entry_type::alphabet_name.size();
             ++name_byte_index) {
            name_buffer[write_offset++] =
                p_entry_type::alphabet_name[name_byte_index];
        }
        name_buffer[write_offset++] = '\'';
        first_entry = false;
    };
    (append.template operator()<p_entry_types>(), ...);
    return name_buffer;
}

template <class... p_entry_types>
inline constexpr auto valid_names_buffer_v =
    build_valid_names_buffer<p_entry_types...>();

template <class... p_entry_types>
inline constexpr std::size_t valid_names_live_length_v =
    valid_names_live_length<p_entry_types...>();

}  // namespace detail_supported_alphabets

/**
 * @brief Closed compile-time set of alphabets supported by one stored type.
 * @tparam p_entries Alphabet/name entries; types and display names must be unique.
 */
template <is_alphabet_entry... p_entries>
requires (sizeof...(p_entries) > 0)
struct supported_alphabets {
    using alphabet_entries_tuple = std::tuple<p_entries...>;
    using alphabets_tuple = std::tuple<typename p_entries::alphabet_type...>;

    static constexpr std::size_t alphabet_count = sizeof...(p_entries);

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
        dispatch_detail::unique_names_in_pack<p_entries...>(),
        "supported_alphabets: alphabet display names must be unique within the pack");

    /// Probe types for the runtime-dispatch primitives. Defined as
    /// nested struct templates so their `matches` / `extract` member
    /// templates are well-formed (local-class templates are forbidden
    /// by C++, so probes live alongside the pack rather than in a dispatch
    /// function body).
    struct code_match_probe {
        alphabet_code_t runtime_code;
        template <class p_entry_type>
        [[gnu::always_inline]]
        constexpr bool matches(alphabet_code_t cursor) const noexcept {
            return cursor == runtime_code;
        }
        [[noreturn]] void on_no_match(alphabet_code_t cursor) const {
            ASSERT(false,
                fmt::format("dispatch_alphabet_by_code: code {} > pack max {}",
                            static_cast<std::size_t>(runtime_code),
                            static_cast<std::size_t>(cursor)));
        }
    };

    struct name_match_probe {
        const char* runtime_name;
        bool& matched;
        template <class p_entry_type>
        [[gnu::always_inline]]
        constexpr bool matches(alphabet_code_t /*cursor*/) const noexcept {
            if (p_entry_type::alphabet_name.equals_c_string(runtime_name)) {
                matched = true;
                return true;
            }
            return false;
        }
        constexpr void on_no_match(alphabet_code_t /*cursor*/) const noexcept {
            // matched stays false; caller checks the return value.
        }
    };

    struct code_to_name_probe {
        alphabet_code_t runtime_code;
        template <class p_entry_type>
        [[gnu::always_inline]]
        constexpr std::optional<std::string_view> extract(
            alphabet_code_t cursor) const noexcept
        {
            if (cursor == runtime_code) {
                return p_entry_type::alphabet_name_sv;
            }
            return std::nullopt;
        }
    };

    struct name_to_code_probe {
        const char* runtime_name;
        template <class p_entry_type>
        [[gnu::always_inline]]
        constexpr std::optional<alphabet_code_t> extract(
            alphabet_code_t cursor) const noexcept
        {
            if (p_entry_type::alphabet_name.equals_c_string(runtime_name)) {
                return cursor;
            }
            return std::nullopt;
        }
    };

    /// Stateless adapter forwarding `operator()<p_entry_type>` to
    /// `operator()<typename p_entry_type::alphabet_type>` on the wrapped handler.
    /// No `[&]` capture (the project has been burned by capture-by-reference
    /// nested lambdas not inlining): the adapter holds the handler by
    /// reference as a data member and the templated `operator()` is
    /// `[[gnu::always_inline]]`.
    template <class p_handler_type>
    struct entry_to_alphabet_adapter {
        p_handler_type& handler;

        template <class p_entry_type>
        [[gnu::always_inline]]
        constexpr void operator()() const {
            handler.template operator()<typename p_entry_type::alphabet_type>();
        }
    };

    /**
     * @brief Dispatch a runtime alphabet code to a compile-time alphabet.
     * @tparam p_handler_type Callable with `operator()<alphabet>()`.
     * @param code Runtime alphabet code.
     * @param handler Callable invoked for the selected alphabet.
     */
    template <class p_handler_type>
    [[gnu::always_inline]]
    static constexpr void dispatch_alphabet_by_code(
        alphabet_code_t code, p_handler_type&& handler)
    {
        namespace tpl = ::bioseq::common::templating;

        entry_to_alphabet_adapter<std::remove_reference_t<p_handler_type>>
            adapter{handler};
        code_match_probe probe{code};
        tpl::dispatch_first_matching_type<
            alphabet_code_t,
            tpl::type_list<p_entries...>>(adapter, probe);
    }

    /**
     * @brief Dispatch a runtime display name to a compile-time entry.
     * @tparam p_handler_type Callable with `operator()<alphabet_entry>()`.
     * @param name Null-terminated display name.
     * @param handler Callable invoked for the first matching entry.
     * @return `true` when the name belongs to this alphabet set.
     */
    template <class p_handler_type>
    static constexpr bool dispatch_alphabet_by_name(
        const char* name, p_handler_type&& handler) noexcept
    {
        namespace tpl = ::bioseq::common::templating;

        bool matched = false;
        name_match_probe probe{name, matched};
        tpl::dispatch_first_matching_type<
            alphabet_code_t,
            tpl::type_list<p_entries...>>(handler, probe);
        return matched;
    }

    /// Compile-time look-up: pack index of a specific alphabet. Asserting
    /// — absence is a build error.
    template <sequence_alphabet p_alphabet>
    [[nodiscard]] static constexpr alphabet_code_t index_of() {
        using namespace ::bioseq::common::templating;
        return static_cast<alphabet_code_t>(
            index_of_type_pack_checked<
                p_alphabet, typename p_entries::alphabet_type...>());
    }

    /// Compile-time look-up: display name of a specific alphabet, returned
    /// as a `compile_time_string<N>` value. The `requires` clause short-circuits
    /// if the alphabet is not in the pack so diagnostics fire at the alias site.
    template <sequence_alphabet p_alphabet>
    [[nodiscard]] static constexpr auto name_of() {
        constexpr std::size_t idx =
            static_cast<std::size_t>(index_of<p_alphabet>());
        return std::tuple_element_t<idx, alphabet_entries_tuple>::alphabet_name;
    }

    /// Member alias template: the `alphabet_entry<...>` brand for a
    /// specific alphabet. Useful so test files can pull the entry off
    /// the pack rather than naming a literal.
    template <sequence_alphabet p_alphabet>
        requires (::bioseq::common::templating::contains_type_pack<
            p_alphabet, typename p_entries::alphabet_type...>())
    using entry_for = std::tuple_element_t<
        ::bioseq::common::templating::index_of_type_pack<
            p_alphabet, typename p_entries::alphabet_type...>(),
        alphabet_entries_tuple>;

    /// Runtime: map a code to the corresponding pack entry's display
    /// name. Returns "?" on out-of-band input with debug telemetry
    /// line so silent corruption is observable.
    [[nodiscard]] static std::string_view name_of_alphabet_code(
        alphabet_code_t code) noexcept
    {
        namespace tpl = ::bioseq::common::templating;

        auto opt = tpl::find_first_matching_type<
            alphabet_code_t, std::string_view,
            tpl::type_list<p_entries...>>(code_to_name_probe{code});

        if (!opt.has_value()) [[unlikely]] {
            bioseq_log<logging_level::debug>(
                "[bioseq] name_of_alphabet_code: out-of-band code={}",
                    static_cast<std::size_t>(code));
            return std::string_view{"?"};
        }
        return *opt;
    }

    /// Runtime: map a cstring to the corresponding pack index. Returns
    /// `std::nullopt` if the name is not in the pack.
    [[nodiscard]] static std::optional<alphabet_code_t> try_index_of_name(
        const char* name) noexcept
    {
        namespace tpl = ::bioseq::common::templating;
        return tpl::find_first_matching_type<
            alphabet_code_t, alphabet_code_t,
            tpl::type_list<p_entries...>>(name_to_code_probe{name});
    }

    /// Build-time CSV of single-quoted, comma-space-separated names, for
    /// example `"'dna4', 'dna5'"`.
    [[nodiscard]] static constexpr std::string_view
    valid_alphabet_names_csv() noexcept
    {
        return std::string_view{
            detail_supported_alphabets::valid_names_buffer_v<p_entries...>.data(),
            detail_supported_alphabets::valid_names_live_length_v<p_entries...>};
    }
};

/** @brief Type carrying a valid closed set of supported alphabets. */
template<class p_candidate_type>
concept supported_alphabet_set = requires {
    requires p_candidate_type::is_valid_supported_alphabets == true;
    typename p_candidate_type::alphabets_tuple;
    typename p_candidate_type::alphabet_code_t;
    { p_candidate_type::alphabet_count } -> std::convertible_to<std::size_t>;
};

/** @brief Alphabet present in a closed supported-alphabet set. */
template<class p_alphabet_set, class p_alphabet>
concept alphabet_in_supported_set =
    supported_alphabet_set<p_alphabet_set>
    && ::bioseq::common::templating::contains_type_v<
           p_alphabet, typename p_alphabet_set::alphabets_tuple>;

/// Compile-time list of storage-unit byte widths supported by one
/// `compact_sequence_stdc` instantiation. PostgreSQL build profiles use this
/// policy to remove disallowed width specialisations from the type itself,
/// while standalone/default users retain the full {1,2,4,8} list.
template<std::size_t... p_byte_widths>
requires (sizeof...(p_byte_widths) > 0)
struct supported_storage_unit_byte_widths {
    static constexpr bool is_valid_supported_storage_unit_byte_widths = true;
    static constexpr std::size_t width_count = sizeof...(p_byte_widths);

    static_assert(
        ((p_byte_widths == 1u || p_byte_widths == 2u
          || p_byte_widths == 4u || p_byte_widths == 8u) && ...),
        "supported storage-unit byte widths must be drawn from {1,2,4,8}");
    static_assert(
        ::bioseq::common::templating::unique_types_in_tuple_v<
            std::tuple<std::integral_constant<std::size_t, p_byte_widths>...>>,
        "supported storage-unit byte widths must be unique");

    template<std::size_t p_byte_width>
    static constexpr bool contains =
        ((p_byte_width == p_byte_widths) || ...);

    [[nodiscard]] static constexpr bool contains_runtime(
        std::uint8_t byte_width) noexcept
    {
        return ((byte_width == p_byte_widths) || ...);
    }

private:
    template<std::size_t p_first_width, std::size_t... p_remaining_widths,
             class p_handler_type>
    [[gnu::always_inline]]
    static constexpr void dispatch_impl(
        std::uint8_t byte_width, p_handler_type& handler)
    {
        if (byte_width == p_first_width) {
            handler.template operator()<p_first_width>();
            return;
        }
        if constexpr (sizeof...(p_remaining_widths) > 0) {
            dispatch_impl<p_remaining_widths...>(byte_width, handler);
        } else {
            ASSERT(false,
                   fmt::format(
                       "storage-unit byte width {} is not enabled by this "
                       "compact-sequence type",
                       static_cast<unsigned>(byte_width)));
        }
    }

public:
    /// Pin a supported runtime width as a `size_t` NTTP and invoke `handler`.
    /// Only widths in this policy are instantiated.
    template<class p_handler_type>
    [[gnu::always_inline]]
    static constexpr void dispatch(
        std::uint8_t byte_width, p_handler_type&& handler)
    {
        dispatch_impl<p_byte_widths...>(byte_width, handler);
    }
};

/** @brief Type carrying a valid closed set of packed storage widths. */
template<class p_candidate_type>
concept supported_storage_width_set = requires {
    requires p_candidate_type::is_valid_supported_storage_unit_byte_widths
        == true;
    { p_candidate_type::width_count } -> std::convertible_to<std::size_t>;
    { p_candidate_type::contains_runtime(std::uint8_t{}) }
        -> std::same_as<bool>;
};

using all_storage_unit_byte_widths =
    supported_storage_unit_byte_widths<1u, 2u, 4u, 8u>;

/// Default standalone dispatch across all compact-sequence storage widths.
/// PostgreSQL-bound code dispatches through its `compact_sequence_stdc`
/// width policy instead.
template<class p_handler_type>
[[gnu::always_inline]]
inline constexpr void dispatch_storage_unit_byte_width(
    std::uint8_t byte_width, p_handler_type&& handler)
{
    all_storage_unit_byte_widths::dispatch(
        byte_width, std::forward<p_handler_type>(handler));
}

/// Constexpr round-up of `bytes` to the next multiple of sizeof(uint64_t).
/// Used by the compact_sequence_stdc factories so `data[]` can be safely
/// reinterpreted across {1, 2, 4, 8} byte widths.
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
// loaded unit. `packed_sequence_iterator::increment` walks
// walks `unit_val & char_mask` then `unit_val >>= bits_per_char` —
// LSB-first. `pack_symbols` produces this layout: each new input letter is
// shifted in at the top, then the unit is right-shifted for the next
// one, so by the time the unit is fully populated the FIRST letter
// sits at bit 0..bits_per_char-1 (LSB end) and the LAST letter sits at
// the MSB end. Hand-traced for `dna4 / uint64_t`, sequence "ACGT…32":
// A (rank 0) → bits 0-1, C (rank 1) → bits 2-3, …, last letter (the
// 32nd) → bits 62-63.
//
// `extract_ranks_from_unit<p_value_type, p_bits_per_letter>(value)` therefore
// returns a `std::array<p_value_type, letters_per_unit>` where index 0 holds
// the LSB-slot letter (= first input letter when the unit is fully
// loaded) and index `letters_per_unit - 1` holds the MSB-slot letter
// (= last input letter). Walking the array in increasing index order
// reproduces the same symbol sequence as `packed_sequence_iterator`.
//
// The intent is to pre-compute these arrays for the unit-aligned bulk
// of a region and then stream the letters into a destination buffer
// (KSW2 state.target_bytes / state.query_reversed) — avoiding the
// per-letter scalar shift loop in aligned bulk regions.
// =====================================================================

namespace bit_pack_detail {

template<typename p_value_type, std::size_t... p_bit_indices>
[[nodiscard]] constexpr p_value_type make_low_bit_mask(
    std::index_sequence<p_bit_indices...>) noexcept
{
    return ((static_cast<p_value_type>(1)
             << static_cast<p_value_type>(p_bit_indices)) | ...);
}

}  // namespace bit_pack_detail

/**
 * @brief Build a mask covering one packed rank slot.
 * @tparam p_value_type Unsigned packed word type.
 * @tparam p_bits_per_letter Packed rank width.
 * @return Low-bit mask of `p_bits_per_letter` one bits.
 */
template<typename p_value_type, std::size_t p_bits_per_letter>
[[nodiscard]] consteval p_value_type unit_rank_mask() noexcept {
    return bit_pack_detail::make_low_bit_mask<p_value_type>(
        std::make_index_sequence<p_bits_per_letter>{});
}

/**
 * @brief Extract a single letter from a storage word.
 *
 * `letter_no` indexes from the *low* end of the unit (index 0 = bottom
 * `p_bits_per_letter` bits). bioseq's encoding (LSB-first; see header
 * comment above the helpers) places the FIRST input letter at slot
 * 0, so `extract_rank_from_unit<…, 0>(unit)` returns the first rank
 * directly.
 *
 * @tparam p_value_type Unsigned packed word type.
 * @tparam p_bits_per_letter Packed rank width.
 * @tparam p_letter_index Zero-based rank slot from the least-significant end.
 * @param value Packed source word.
 * @return Rank stored in the selected slot.
 */
template<typename p_value_type,
         std::size_t p_bits_per_letter,
         std::size_t p_letter_index>
[[always_inline]] [[nodiscard]] constexpr p_value_type extract_rank_from_unit(
    const p_value_type& value) noexcept
{
    static_assert(p_letter_index * p_bits_per_letter < sizeof(p_value_type) * 8,
        "rank slot exceeds the packed storage width");
    constexpr p_value_type bit_shift =
        static_cast<p_value_type>(p_bits_per_letter * p_letter_index);
    constexpr p_value_type mask =
        unit_rank_mask<p_value_type, p_bits_per_letter>() << bit_shift;
    return (value & mask) >> bit_shift;
}

namespace bit_pack_detail {

template<typename p_value_type,
         std::size_t p_bits_per_letter,
         std::size_t p_letter_count,
         std::size_t... p_letter_indices>
[[always_inline]] [[nodiscard]] constexpr auto extract_ranks_from_unit_impl(
    std::index_sequence<p_letter_indices...>,
    const p_value_type& value) noexcept
    -> std::array<p_value_type, sizeof...(p_letter_indices)>
{
    // Index 0 reads from bit slot 0 (the LSB end). bioseq's encoding
    // puts the FIRST input letter at bit slot 0, the SECOND at slot 1,
    // etc., so walking the returned array in order matches the
    // input-sequence order — the packed iterator contract.
    return std::array<p_value_type, sizeof...(p_letter_indices)>{
        extract_rank_from_unit<
            p_value_type, p_bits_per_letter, p_letter_indices>(value)...
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
 * `p_letter_count` template argument matches `letters_per_unit` for the
 * given `(p_value_type, p_bits_per_letter)` pair.
 *
 * @tparam p_value_type Unsigned packed word type.
 * @tparam p_bits_per_letter Packed rank width.
 * @tparam p_letter_count Rank slots extracted from the word.
 * @param value Packed source word.
 * @return Fixed-size array ordered from least- to most-significant rank slot.
 */
template<
    typename p_value_type,
    std::size_t p_bits_per_letter,
    std::size_t p_letter_count = (sizeof(p_value_type) * 8) / p_bits_per_letter
>
[[always_inline]] [[nodiscard]] constexpr auto extract_ranks_from_unit(
    const p_value_type& value) noexcept
    -> std::array<p_value_type, p_letter_count>
{
    return bit_pack_detail::extract_ranks_from_unit_impl<
        p_value_type, p_bits_per_letter, p_letter_count>(
            std::make_index_sequence<p_letter_count>{}, value);
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
 * `iterate_sequence_bp_simd` needs: `bp_size()`, `byte_size()`, `size()`,
 * `padding()`, `read_cptr(it)`, `advance_iter(it, n)`. A plain
 * `compact_sequence_range<R>` exposes the same shape — both satisfy
 * `supports_compact_sequence_bitcast_view` (defined below `compact_sequence_range_base`).
 */
template <typename p_base_range, typename p_output_range>
struct bit_casted_view {
public:
    using base_traits = p_base_range::traits;
    using traits = p_output_range::traits;
    using iterator = typename p_base_range::iterator;

private:
    const p_base_range& base_;
public:

    /** @param base_container Borrowed packed range whose bytes are reinterpreted. */
    explicit bit_casted_view(const p_base_range& base_container)
        : base_(base_container) {}

    // If this is downcast or upcast. Downcast = we re-interpret a wide unit
    // as a narrower unit (e.g. uint64 → uint32); upcast = the reverse. The
    // sizeof comparison here is on the *storage_unit*, not on the wrapping
    // sequence type — the wrapping types are the same size (both wrap a
    // std::vector + size_t) regardless of the storage_unit.
    static constexpr bool is_downcast =
        sizeof(typename traits::storage_unit) <=
        sizeof(typename base_traits::storage_unit);
    // Ratio between source and target unit widths in bits — always >= 1.
    static constexpr std::size_t cast_ratio = is_downcast
        ? (base_traits::unit_width / traits::unit_width)
        : (traits::unit_width / base_traits::unit_width);
    // Log2 of cast_ratio (useful for shifts).
    static constexpr std::size_t cast_ratio_shift =
        seqan3::detail::ceil_log2(cast_ratio);

    // Verifies that cast_ratio is power of two, otherwise advance_iter's
    // shift-as-divide / shift-as-multiply lowering would be wrong.
    static_assert(1u << cast_ratio_shift == cast_ratio);

    /** @return Logical-symbol count, unchanged by storage reinterpretation. */
    [[always_inline]] constexpr std::size_t bp_size() const {
        return base_.bp_size();
    };

    /** @return Borrowed byte count, unchanged by storage reinterpretation. */
    [[always_inline]] constexpr std::size_t byte_size() const {
        return base_.byte_size();
    };

    /** @return Packed units in the reinterpreted view. */
    [[always_inline]] constexpr std::size_t size() const {
        return view_units_for_letters();
    };

    /** @return Empty rank slots in the final reinterpreted unit. */
    [[always_inline]] constexpr std::size_t padding() const {
        // Empty letter slots in the trailing view-unit: units*lpu - bps.
        return view_units_for_letters() * traits::letters_per_unit
            - base_.bp_size();
    };

private:
    /// Ceil-divide the logical-symbol count by the view's symbols per unit.
    /// Keeping the computation local makes size() and padding() derive from
    /// the same exact unit count without another module dependency.
    [[always_inline]] constexpr std::size_t view_units_for_letters() const {
        constexpr std::size_t letters_per_unit = traits::letters_per_unit;
        const std::size_t symbol_count = base_.bp_size();
        return symbol_count / letters_per_unit
            + (symbol_count % letters_per_unit != 0u);
    }
public:

    /**
     * @brief Reinterpret an admitted base iterator as the view storage type.
     * @tparam p_iterator_type Iterator accepted by the base range.
     * @param iterator Base-range iterator.
     * @return Pointer into the same borrowed bytes, with view unit type.
     */
    template<typename p_iterator_type>
    requires(
        p_base_range::template valid_derived_iterator<p_iterator_type>::value)
    [[always_inline]] constexpr const typename traits::storage_unit* read_cptr(
        const p_iterator_type& iterator) const
    {
        return reinterpret_cast<const typename traits::storage_unit*>(
            base_.read_cptr(iterator));
    };

    /// Forward base's `cbegin()` so callers that consume the
    /// `supports_compact_sequence_bitcast_view` concept can fetch the
    /// initial iterator uniformly across `compact_sequence_range` and
    /// `bit_casted_view`. Returns a base-side iterator that this view's
    /// `read_cptr` accepts (per the `valid_derived_iterator` constraint).
    [[always_inline]] constexpr iterator cbegin() const {
        return base_.cbegin();
    }
    [[always_inline]] constexpr iterator begin() const {
        return base_.cbegin();
    }

    template<typename p_iterator_type>
    requires(
        p_base_range::template valid_derived_iterator<p_iterator_type>::value)
    [[always_inline]] constexpr static void advance_iter(
        p_iterator_type& iterator, const std::size_t value) {
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
        if constexpr (is_downcast) {
            DEBUG_ASSERT(value % cast_ratio == 0,
                fmt::format("bit_casted_view downcast (ratio={}) requires advance"
                    " value ({}) divisible by cast_ratio for zero-cost shift-as-divide",
                    cast_ratio, value));
            iterator += (value >> cast_ratio_shift);
        } else {
            iterator += (value << cast_ratio_shift);
        }
    };

    /// Distance between two iterators in *view* storage_unit steps.
    /// For a downcast (large→small) view, every base-iter step covers
    /// `cast_ratio` view-units, so the raw `b - a` is multiplied by
    /// `cast_ratio`. For an upcast (small→large), each view-unit takes
    /// `cast_ratio` base-iter steps, so the raw `b - a` is divided by
    /// `cast_ratio`. Mirrors the `compact_sequence_range_base::view_distance`
    /// passthrough so SIMD iteration can use one expression regardless of view
    /// flavour.
    template<typename p_iterator_type>
    [[always_inline]] constexpr static std::size_t view_distance(
        const p_iterator_type& begin,
        const p_iterator_type& end) {
        if constexpr (is_downcast) {
            return static_cast<std::size_t>(end - begin) << cast_ratio_shift;
        } else {
            return static_cast<std::size_t>(end - begin) >> cast_ratio_shift;
        }
    }
};

template<typename p_derived_range,
         typename p_alphabet,
         typename p_packed_unit,
         std::size_t p_bits_per_character>
struct compact_sequence_range_base {

    using self_type = compact_sequence_range_base<
        p_derived_range, p_alphabet, p_packed_unit, p_bits_per_character>;

    struct traits {
        using alphabet_type = p_alphabet;
        using storage_unit = p_packed_unit;

        static constexpr std::size_t bits_per_char =
            p_bits_per_character == 0
                ? sizeof(storage_unit) * 8
                : p_bits_per_character;
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

        static constexpr size_t alphabet_size =
            seqan3::alphabet_size<p_alphabet>;
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

        template<typename p_other_storage_unit>
        [[always_inline]] static constexpr p_other_storage_unit kmer_mask(const size_t& length) noexcept {
            constexpr size_t max_length = sizeof(p_other_storage_unit) * 8 / bits_per_char;
            if (length > max_length) {
                return std::numeric_limits<p_other_storage_unit>::max(); //static_cast<p_other_storage_unit>(-1);
            }
            return (static_cast<p_other_storage_unit>(1) << (length * bits_per_char)) - 1;
        };


    };

    using derived_range_t = p_derived_range;

    template<typename p_iterator_type>
    struct valid_derived_iterator {
        static constexpr bool value = false;
    };

    template<typename p_iterator_type>
    requires (
        requires (derived_range_t d, p_iterator_type i) {
            { i = d.begin() };
        }
    )
    struct valid_derived_iterator<p_iterator_type> {
        static constexpr bool value = true;
    };

    inline constexpr derived_range_t const* super() const {
        return static_cast<derived_range_t const*>(this);
    };

    // forward bp iterator

    template<iterator_mode p_mode>
    struct packed_iterator_traits {
        using dereference_t = std::conditional_t<
            p_mode == logical_rank,
            typename traits::storage_unit,
            std::conditional_t<
                p_mode == alphabet_symbol,
                typename traits::alphabet_type,
                void
            >
        >;
    };

    template<
        typename p_base_iterator_type,
        iterator_mode p_selected_mode
    >
    class packed_sequence_iterator : public boost::iterator_adaptor<
        packed_sequence_iterator<p_base_iterator_type, p_selected_mode>,
        p_base_iterator_type,
        typename packed_iterator_traits<p_selected_mode>::dereference_t,
        boost::use_default,
        typename packed_iterator_traits<p_selected_mode>::dereference_t
    > {
    private:
        typedef boost::iterator_adaptor<
            packed_sequence_iterator<p_base_iterator_type, p_selected_mode>,
            p_base_iterator_type,
            typename packed_iterator_traits<p_selected_mode>::dereference_t,
            boost::use_default,
            typename packed_iterator_traits<p_selected_mode>::dereference_t
        > super_t;

        static constexpr iterator_mode selected_iterator_mode =
            p_selected_mode;

        std::size_t rank_slot_{};
        p_base_iterator_type end_{};
        typename traits::storage_unit current_unit_{};

    public:
        friend class boost::iterator_core_access;
        friend class compact_sequence_range_base;

        using iterator_t = p_base_iterator_type;

        packed_sequence_iterator() = default;

        explicit packed_sequence_iterator(
            p_base_iterator_type it,
            p_base_iterator_type end,
            const std::size_t rank_slot,
            typename traits::storage_unit current_unit
        ) : super_t(it), rank_slot_(rank_slot), end_(end),
            current_unit_(current_unit) {}

        constexpr std::size_t rank_slot() const noexcept {
            return rank_slot_;
        }

        constexpr std::strong_ordering operator<=> (
            const packed_sequence_iterator& other) const {
            if constexpr (traits::letters_per_unit == 1) {
                return (this->base() <=> other.base());
            } else {
                if (auto cmp = this->base() <=> other.base(); cmp != 0) { return cmp; }
                return (rank_slot_ <=> other.rank_slot_);
            }
        }

        constexpr bool operator==(
            const packed_sequence_iterator& other) const {
            if constexpr (traits::letters_per_unit == 1) {
                return (this->base() == other.base());
            } else {
                return this->base() == other.base()
                    && rank_slot_ == other.rank_slot_;
            }
        }

        template<iterator_mode p_dereference_mode>
        typename packed_iterator_traits<p_dereference_mode>::dereference_t
        _dereference() const {
            if constexpr (p_dereference_mode == logical_rank) {
                if constexpr (traits::letters_per_unit == 1) {
                    return *this->base();
                } else {
                    return current_unit_ & traits::char_mask;
                }
            } else if constexpr (p_dereference_mode == alphabet_symbol) {
                typename traits::alphabet_type alph;
                return alph.assign_rank(_dereference<logical_rank>());
            } else {
                static_assert(
                    p_dereference_mode == logical_rank
                    || p_dereference_mode == alphabet_symbol,
                    "unsupported packed-sequence iterator mode");
            }
        }

        auto dereference() const {
            return _dereference<p_selected_mode>();
        }

        void increment() {
            if constexpr (traits::letters_per_unit == 1) {
                ++this->base_reference();
            } else if (rank_slot_ == traits::letters_per_unit - 1) {
                // Need to increment
                ++this->base_reference();
                if (this->base() != end_) {
                    current_unit_ = *this->base();
                }
                rank_slot_ = 0;
            } else {
                ++rank_slot_;
                current_unit_ >>= traits::bits_per_char;
            }
        }

        bool equal(const packed_sequence_iterator& other) const {
            if constexpr (traits::letters_per_unit == 1) {
                return this->base() == other.base();
            } else {
                return this->base() == other.base()
                    && rank_slot_ == other.rank_slot_;
            }
        }
    };

    /** @brief Borrowed begin/end pair for one packed iterator mode. */
    template<typename p_iterator_type>
    struct packed_iterator_range {
    private:
        p_iterator_type begin_;
        p_iterator_type end_;

    public:
        packed_iterator_range(
            p_iterator_type begin_iterator,
            p_iterator_type end_iterator
        ): begin_(begin_iterator), end_(end_iterator) {}

        p_iterator_type const& begin() const {
            return begin_;
        }

        p_iterator_type const& end() const {
            return end_;
        }
    };

    template<iterator_mode p_mode, typename p_iterator_type>
    requires (valid_derived_iterator<p_iterator_type>::value == true)
    constexpr inline packed_sequence_iterator<p_iterator_type, p_mode> iter(
        const p_iterator_type& iterator) const {
        if (super()->end() == iterator) {
            if constexpr (traits::letters_per_unit != 1) {
                if (super()->begin() != iterator && super()->padding() != 0) {
                    return packed_sequence_iterator<p_iterator_type, p_mode>(
                        iterator - 1,
                        super()->end(),
                        traits::letters_per_unit - super()->padding(),
                        typename traits::storage_unit{});
                }
            }
            return packed_sequence_iterator<p_iterator_type, p_mode>(
                iterator,
                super()->end(),
                0,
                typename traits::storage_unit{});
        }
        return packed_sequence_iterator<p_iterator_type, p_mode>(
            iterator, super()->end(), 0, *iterator);
    };

    template<iterator_mode p_mode>
    constexpr inline auto iterable() const {
        return packed_iterator_range(
            iter<p_mode>(super()->cbegin()),
            iter<p_mode>(super()->cend())
        );
    };

    /**
     * @brief Find an alphabet-symbol subsequence within a packed iterator range.
     * @tparam p_iterator_type Valid iterator type for the derived range.
     * @param begin First packed unit to inspect.
     * @param end One-past-last packed unit.
     * @param subsequence Alphabet symbols to locate.
     * @param start_position First logical-symbol position considered.
     * @return Logical-symbol offset of the first match, or `std::nullopt`.
     */
    template<typename p_iterator_type>
    requires (valid_derived_iterator<p_iterator_type>::value == true)
    constexpr std::optional<std::size_t> find_symbol_subsequence(
        p_iterator_type begin,
        p_iterator_type end,
        std::vector<typename traits::alphabet_type> subsequence,
        std::size_t start_position = 0) const
    {
        const auto sequence_string = to_string(begin, end);
        const auto subsequence_string = symbols_vector_to_string(subsequence);
        const auto position =
            sequence_string.find(subsequence_string, start_position);
        if (position == std::string::npos) {
            return std::nullopt;
        }
        return position;
    }

    template<typename p_iterator_type>
    requires (valid_derived_iterator<p_iterator_type>::value == true)
    constexpr std::string to_string(
        p_iterator_type begin,
        p_iterator_type end,
        bool split_units = false) const {
        std::string result;
        result.reserve((end - begin) * traits::letters_per_unit);
        const auto symbol_begin = iter<alphabet_symbol, p_iterator_type>(begin);
        const auto symbol_end = iter<alphabet_symbol, p_iterator_type>(end);
        for(auto letter = symbol_begin; letter != symbol_end; ++letter) {
            if (letter.rank_slot() == 0 && split_units) {
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

    /**
     * @brief Split packed storage into unit-aligned record descriptors.
     *
     * No ranks are decoded or copied. Only the final descriptor carries the
     * range's trailing padding.
     *
     * @param record_bp Maximum logical symbols per record; must be a multiple
     * of `traits::letters_per_unit`.
     * @return Tuples of unit offset, unit count, and trailing padding.
     * @throws std::invalid_argument If `record_bp` is zero or not unit-aligned.
     */
    [[nodiscard]] std::vector<std::tuple<std::size_t, std::size_t, std::size_t>>
    split_units(std::size_t record_bp) const {
        if (record_bp == 0
            || record_bp % traits::letters_per_unit != 0) {
            throw std::invalid_argument(
                "split_units requires a non-zero unit-aligned record size");
        }
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

    /** @brief Advance a packed iterator by a number of storage units. */
    template<typename p_iterator_type>
    [[always_inline]] static inline void advance_iter(
        p_iterator_type& iterator,
        const size_t distance) {
        iterator += distance;
    }

    /** @brief Return the distance between iterators in packed storage units. */
    template<typename p_iterator_type>
    [[always_inline]] static inline size_t view_distance(
        const p_iterator_type& begin,
        const p_iterator_type& end) {
        return static_cast<size_t>(end - begin);
    }

    /**
     * @brief Get the compile-time alphabet cardinality (number of distinct symbols).
     *
     * For dna4 this returns 4, for dna5 this returns 5, etc.
     */
    static constexpr size_t alphabet_cardinality() {
        return seqan3::alphabet_size<p_alphabet>;
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
        // iterable<logical_rank>() constructs both endpoint iterators through
        // iter(),
        // whose representation dereferences the underlying iterator.
        // Avoid forming those iterators for an empty packed range.
        if (sequence_length == 0u) return {};
        std::vector<uint8_t> encoded_ranks;
        encoded_ranks.reserve(sequence_length);
        for (const auto& base_pair_rank : iterable<logical_rank>()) {
            encoded_ranks.push_back(static_cast<uint8_t>(base_pair_rank));
        }
        return encoded_ranks;
    }

    /**
     * @brief Extract a sub-region of the sequence as rank values, with optional
     *        reverse-complement transformation.
     *
     * @tparam p_is_reversed  When false, encodes [bp_from, bp_to) in forward order.
     *                      When true, produces the same result as slicing the full
     *                      reverse-complement encoding at [bp_from, bp_to).
     * @param bp_from  Start position (inclusive, 0-based)
     * @param bp_to    End position (exclusive, 0-based)
     * @return Vector of uint8_t rank values for the requested region
     */
    template<bool p_is_reversed>
    [[nodiscard]] std::vector<uint8_t> to_encoded_vector(size_t bp_from, size_t bp_to) const {
        if (bp_from >= bp_to) return {};
        const size_t region_length = bp_to - bp_from;
        std::vector<uint8_t> encoded_ranks(region_length);

        if constexpr (!p_is_reversed) {
            size_t write_pos = 0;
            size_t pos = 0;
            for (const auto& bp_rank : iterable<logical_rank>()) {
                if (pos >= bp_to) break;
                if (pos >= bp_from) {
                    encoded_ranks[write_pos++] = static_cast<uint8_t>(bp_rank);
                }
                ++pos;
            }
        } else {
            // Reverse-complement maps each output rank to its mirrored input.
            // read forward, complemented, written in reverse order.
            const size_t sequence_length = bp_size();
            const size_t orig_from = sequence_length - bp_to;
            const size_t orig_to = sequence_length - bp_from;
            size_t write_pos = region_length;
            size_t pos = 0;
            for (const auto& bp_rank : iterable<logical_rank>()) {
                if (pos >= orig_to) break;
                if (pos >= orig_from) {
                    --write_pos;
                    encoded_ranks[write_pos] = static_cast<uint8_t>(
                        traits::letter_complement(
                            static_cast<p_packed_unit>(bp_rank)));
                }
                ++pos;
            }
        }
        return encoded_ranks;
    }

    /**
     * @brief Encode a sub-region directly into caller-provided storage.
     *
     * Same output as `to_encoded_vector<p_is_reversed>(bp_from, bp_to)` but
     * writes into a span owned by the caller. Used by KSW2 to skip the
     * per-call std::vector allocation; the caller allocates
     * the destination from its own align arena and passes the span in.
     *
     * The destination span must be at least `bp_to - bp_from` bytes.
     *
     * Iteration seeks directly to `bp_from`, so runtime and memory traffic are
     * proportional to the selected region rather than the complete sequence.
     * The current implementation seeks directly to `bp_from` via index
     * arithmetic on the bit-packed storage, then walks exactly
     * `region_length` letters.
     */
    template<bool p_is_reversed, bool p_write_reversed = false>
    void to_encoded_into(std::span<uint8_t> dst, size_t bp_from, size_t bp_to) const {
        if (bp_from >= bp_to) return;
        const size_t region_length = bp_to - bp_from;
        if (dst.size() < region_length) return;

        constexpr size_t lpu = traits::letters_per_unit;
        constexpr size_t bpc = traits::bits_per_char;

        // The output of this method is the cross product of two flips:
        //   * letter complement (p_is_reversed)
        //   * write-pointer direction (the canonical rc has both flipped)
        //
        // To make the matrix legible:
        //
        //   <false, false>  forward     dst[i] = src[from+i]
        //   <false, true>   reverse     dst[i] = src[to-1-i]              (no rc, just mirror)
        //   <true,  false>  rc          dst[i] = complement(src[to-1-i])  (canonical rc)
        //   <true,  true>   complement  dst[i] = complement(src[from+i])  (rc twice cancels)
        //
        // The KSW2 / Block aligner pipelines pre-reverse the query before the
        // DP loop. When the alignment region is on the reverse strand
        // (p_is_reversed == true) the existing code path materialised a
        // reverse-complement buffer and then mirror-copied it into the DP
        // state — a wasteful round trip. With this template parameter we can
        // emit the alignment-ready orientation directly: the DP query buffer
        // wants the *complement only* form (rc twice = identity on order),
        // which is exactly <true, true> below.
        //
        // Translate to forward-domain coordinates. p_is_reversed reads through
        // the mirrored window of the source so that "complement + mirror"
        // produces the canonical rc; p_write_reversed only changes the
        // destination write order, not the source coordinates.
        const size_t fwd_from = p_is_reversed ? (bp_size() - bp_to)   : bp_from;
        [[maybe_unused]] const size_t fwd_to =
            p_is_reversed ? (bp_size() - bp_from) : bp_to;

        // The four orientations collapse to two output orderings in this
        // implementation: <true, false> and <false, true> both write
        // descending; <false, false> and <true, true> both write ascending.
        // Letter complement is applied iff p_is_reversed (mirroring how the
        // forward-window is interpreted). Branch is constexpr so the
        // generated code paths stay specialised per call site.
        constexpr bool descending_write = (p_is_reversed && !p_write_reversed)
            || (!p_is_reversed && p_write_reversed);

        // -----------------------------------------------------------
        // Fast path: alphabet packs exactly into storage_unit
        // (`unit_leftover_bits == 0`) and the requested window is at
        // least one full unit. Decompose each storage_unit via the
        // constexpr `extract_ranks_from_unit<>` helper and stream
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
                const auto letter = p_is_reversed
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
            // slot (see `extract_ranks_from_unit` header comment).
            // To advance past `bit_no` earlier letters we drop
            // the bottom `bit_no * bpc` bits and continue masking the
            // LSB — the same invariant held by packed_sequence_iterator.
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
                const auto ranks = extract_ranks_from_unit<
                    typename traits::storage_unit, bpc, lpu>(
                        static_cast<typename traits::storage_unit>(*base_iter));
                // Index 0 of `ranks` is the left-most symbol of the
                // unit — this matches the canonical sequence order.
                for (size_t k = 0; k < lpu; ++k) {
                    emit_letter(ranks[k]);
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
        // same logical-rank and padding contract as the aligned path.
        // -----------------------------------------------------------
        // For the letters_per_unit == 1 case, every unit holds exactly one
        // letter; iter<logical_rank>() is a thin wrapper around
        // dereferencing the underlying storage. For multi-letter units,
        // pre-shift the unit so the bp_from-th letter sits in the bottom
        // char_mask bits — the packed iterator invariant.
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
                if constexpr (p_is_reversed) {
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
                if constexpr (p_is_reversed) {
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
        for (const auto& base_pair_rank : iterable<logical_rank>()) {
            --write_position;
            reverse_complement_ranks[write_position] = static_cast<uint8_t>(
                traits::letter_complement(
                    static_cast<p_packed_unit>(base_pair_rank)));
        }
        return reverse_complement_ranks;
    }

    template<
        typename p_other_storage_unit,
        typename p_other_derived_type = compact_sequence_range<sequence_custom_alphabet<typename traits::alphabet_type, p_other_storage_unit, traits::bits_per_char>>
    >
    requires(
        is_sequence_custom_alphabet_bit_castable_to<
            sequence_custom_alphabet<typename traits::alphabet_type, typename traits::storage_unit, traits::bits_per_char>,
            sequence_custom_alphabet<typename traits::alphabet_type, p_other_storage_unit, traits::bits_per_char>
        >
    )
    [[always_inline]] constexpr bit_casted_view<
        p_derived_range, p_other_derived_type> bit_cast_to_storage_unit() const {
        return bit_casted_view<p_derived_range, p_other_derived_type>(*super());
    }

};

template<typename p_range_type>
requires (
    sequence_proper_container<p_range_type>
)
struct compact_sequence_range<p_range_type> : public compact_sequence_range_base<
    compact_sequence_range<p_range_type>,
    std::remove_reference_t<std::ranges::range_reference_t<p_range_type>>,
    typename boost::uint_t<seqan3::detail::ceil_log2(seqan3::alphabet_size<
        std::remove_reference_t<std::ranges::range_reference_t<p_range_type>>
    >)>::least,
    0
> {
public:
    using traits = typename compact_sequence_range<
        p_range_type>::compact_sequence_range_base::traits;
    using base_iterator_type = decltype(std::declval<p_range_type>().cbegin());

    const p_range_type& data_;

    static constexpr size_t padding() {
        return 0;
    }

    explicit constexpr compact_sequence_range(p_range_type&& base_range)
        : data_(std::move(base_range)) {}
    explicit constexpr compact_sequence_range(p_range_type& base_range)
        : data_(base_range) {}

    /** @brief Rank-projecting iterator over an unpacked alphabet range. */
    class unpacked_rank_iterator : public boost::iterator_adaptor<
        unpacked_rank_iterator,
        base_iterator_type,
        typename traits::storage_unit,
        boost::use_default,
        typename traits::storage_unit
    > {
    private:
        typedef boost::iterator_adaptor<
            unpacked_rank_iterator,
            base_iterator_type,
            typename traits::storage_unit,
            boost::use_default,
            typename traits::storage_unit
        > super_t;
    public:
        friend class boost::iterator_core_access;
        friend class compact_sequence_range<p_range_type>;

        template <typename p_friend_range_type>
        friend struct compact_sequence_range;

        unpacked_rank_iterator() = default;

        explicit unpacked_rank_iterator(base_iterator_type const& it)
            : super_t(it) {}

        auto operator <=>(const unpacked_rank_iterator&) const = default;

        traits::storage_unit dereference() const {
            return static_cast<traits::storage_unit>(seqan3::to_rank(*this->base()));
        };

        size_t operator-(unpacked_rank_iterator const& other) const {
            return static_cast<size_t>(this->base() - other.base());
        };
    };

    using iterator = unpacked_rank_iterator;

    unpacked_rank_iterator begin() {
        return unpacked_rank_iterator(data_.cbegin());
    }

    unpacked_rank_iterator end() {
        return unpacked_rank_iterator(data_.cend());
    }

    unpacked_rank_iterator begin() const {
        return unpacked_rank_iterator(data_.cbegin());
    }

    unpacked_rank_iterator end() const {
        return unpacked_rank_iterator(data_.cend());
    }

    unpacked_rank_iterator cbegin() const {
        return unpacked_rank_iterator(data_.cbegin());
    }

    unpacked_rank_iterator cend() const {
        return unpacked_rank_iterator(data_.cend());
    }

    auto operator <=>(const compact_sequence_range<p_range_type>&) const = default;

    const traits::storage_unit operator[](size_t index) const {
        return static_cast<traits::storage_unit>(seqan3::to_rank(data_[index]));
    }

    constexpr size_t size() const {
        return data_.size();
    }

    friend inline constexpr bool operator==(
        const compact_sequence_range<p_range_type>& a,
        const compact_sequence_range<p_range_type>& b
    ) {
        return a.data_ == b.data_;
    }

    constexpr static const typename traits::storage_unit* read_cptr(
        const unpacked_rank_iterator& it) {
        static_assert(sizeof(typename traits::alphabet_type)
                      == sizeof(typename traits::storage_unit));
        static_assert(
            std::is_standard_layout_v<typename traits::alphabet_type>);

        return reinterpret_cast<const typename traits::storage_unit*>(
            std::addressof(*it.base()));
    };
};

template<typename p_configuration>
requires (is_sequence_custom_alphabet<p_configuration>)
struct compact_sequence_range<p_configuration> : compact_sequence_range_base<
    compact_sequence_range<p_configuration>,
    typename p_configuration::alphabet_type,
    typename p_configuration::storage_unit,
    p_configuration::letter_bit_width
> {
public:
    using traits = typename compact_sequence_range<
        p_configuration>::compact_sequence_range_base::traits;

    using base_container = std::vector<typename traits::storage_unit>;
    using iterator = std::vector<typename traits::storage_unit>::const_iterator;


    /** @brief LSB-first packed alphabet ranks. */
    base_container packed_units;

    /** @brief Unused rank slots in the final packed unit. */
    std::size_t padding_letters{};

    /** @brief Construct an empty packed sequence. */
    compact_sequence_range() = default;

    /**
     * @brief Pack a sized alphabet range into LSB-first storage units.
     * @param input_sequence Symbols to pack in iteration order.
     * @return Packed units and the number of unused final-unit slots.
     */
    template <typename p_input_range>
    requires (
        std::ranges::sized_range<p_input_range>
        && std::is_same_v<std::remove_reference_t<
            std::ranges::range_reference_t<p_input_range>>,
            typename p_configuration::alphabet_type>
    )
    static constexpr std::pair<base_container, std::size_t> pack_symbols(
        p_input_range&& input_sequence)
    {
        const std::size_t required_units =
            input_sequence.size() / traits::letters_per_unit
            + (input_sequence.size() % traits::letters_per_unit != 0);
        base_container units;
        units.reserve(required_units);
        typename traits::storage_unit partial_word{};
        std::size_t partial_letters = 0u;
        std::size_t emitted_symbols = 0u;
        auto write_unit = [&units](typename traits::storage_unit unit) {
            units.push_back(unit);
            return true;
        };
        for(const auto& letter : input_sequence) {
            const bool appended =
                bioseq::sequences::rank_pack_detail::append_valid_rank_lsb<
                    typename traits::storage_unit,
                    traits::bits_per_char,
                    traits::letters_per_unit>(
                        partial_word,
                        partial_letters,
                        emitted_symbols,
                        letter.to_rank(),
                        write_unit);
            DEBUG_ASSERT(appended,
                         "growing rank pack unexpectedly overflowed");
        }
        const auto finished =
            bioseq::sequences::rank_pack_detail::finish_lsb<
                typename traits::storage_unit,
                traits::bits_per_char,
                traits::letters_per_unit>(
                    partial_word, partial_letters, write_unit);
        DEBUG_ASSERT(finished.capacity_ok,
                     "growing rank pack unexpectedly overflowed");
        DEBUG_ASSERT(emitted_symbols == input_sequence.size(),
                     "rank pack emitted-symbol mismatch");
        DEBUG_ASSERT(units.size() == required_units,
                     "rank pack unit-count mismatch");
        return std::make_pair(
            std::move(units),
            static_cast<std::size_t>(finished.padding_letters));
    }

    template <typename p_input_range>
    requires (
        std::ranges::sized_range<p_input_range>
        && std::is_same_v<std::remove_reference_t<
            std::ranges::range_reference_t<p_input_range>>,
            typename p_configuration::alphabet_type>
    )
    /**
     * @brief Construct a packed sequence from a sized alphabet range.
     * @param input_sequence Symbols to pack in iteration order.
     */
    compact_sequence_range(p_input_range&& input_sequence) {
        auto [units, padding] = pack_symbols(
            std::forward<p_input_range>(input_sequence));
        packed_units = std::move(units);
        padding_letters = padding;
    }

    /**
     * @brief Adopt already-packed LSB-first storage without re-encoding.
     * @param units Packed units whose first symbol occupies the low bits.
     * @param padding Unused symbol slots in the final unit.
     * @return Compact sequence owning the supplied storage.
     */
    [[nodiscard]] static constexpr compact_sequence_range from_packed(
        base_container&& units,
        std::size_t padding) noexcept {
        DEBUG_ASSERT(padding < traits::letters_per_unit,
                     "from_packed: padding must be < letters_per_unit");
        compact_sequence_range result;
        result.packed_units = std::move(units);
        result.padding_letters = padding;
        return result;
    }

    constexpr std::size_t padding() const {
        return padding_letters;
    }

    auto constexpr begin() { return packed_units.cbegin(); };
    auto constexpr end() { return packed_units.cend(); };
    auto constexpr begin() const { return packed_units.cbegin(); };
    auto constexpr end() const { return packed_units.cend(); };
    auto constexpr cbegin() const { return packed_units.cbegin(); };
    auto constexpr cend() const { return packed_units.cend(); };

    auto constexpr size() const { return packed_units.size(); }

    auto operator <=> (
        const compact_sequence_range<p_configuration>&) const = default;
    bool constexpr operator==(
        const compact_sequence_range<p_configuration>&) const = default;

    constexpr static const typename traits::storage_unit* read_cptr(
        const iterator& it) {
        return std::to_address(it);
    };
};

template<sequence_alphabet p_alphabet_type>
struct compact_sequence_range<p_alphabet_type>
    : compact_sequence_range<sequence_custom_alphabet<p_alphabet_type>> {
public:
    using parent = compact_sequence_range<
        sequence_custom_alphabet<p_alphabet_type>>;
    using traits = compact_sequence_range<p_alphabet_type>::parent::traits;
    using base_container =
        compact_sequence_range<p_alphabet_type>::parent::base_container;
    using iterator = compact_sequence_range<p_alphabet_type>::parent::iterator;
};

// The sequences module owns the storage-neutral compact header layout;
// PostgreSQL bindings provide allocation and wire callbacks without changing
// the packed representation.

namespace compact_sequence_stdc_detail {

template<std::size_t p_deficit>
consteval auto pick_alignment_reserved_type() {
    if constexpr (p_deficit == 0) return std::type_identity<void>{};
    else if constexpr (p_deficit == 1) return std::type_identity<std::uint8_t>{};
    else if constexpr (p_deficit == 2) return std::type_identity<std::uint16_t>{};
    else if constexpr (p_deficit == 4) return std::type_identity<std::uint32_t>{};
    else return std::type_identity<std::array<std::uint8_t, p_deficit>>{};
}

template<size_t p_varlena_bytes, size_t p_code_bytes, size_t p_bp_size_bytes>
consteval size_t reserved_deficit() {
    constexpr size_t fixed_bytes =
        p_varlena_bytes + p_code_bytes
        + 1u /* storage_unit_byte_width */
        + 2u /* padding_letters */
        + p_bp_size_bytes;
    constexpr size_t target = 8u;
    return (target - (fixed_bytes % target)) % target;
}

template<std::size_t p_deficit>
using alignment_reserved_for_t =
    typename decltype(pick_alignment_reserved_type<p_deficit>())::type;

template<std::size_t p_deficit>
struct alignment_reserved_holder {
    alignment_reserved_for_t<p_deficit> reserved;
};
template<>
struct alignment_reserved_holder<0> { };

}  // namespace compact_sequence_stdc_detail

/**
 * @brief Runtime type parameters for a standard-layout compact sequence.
 *
 * The encoded representation stores the alphabet code in the next byte and
 * the base-two logarithm of the storage-unit byte width in the low byte.
 */
struct compact_sequence_stdc_type_params {
    /** @brief Index of the alphabet in the owning supported-alphabet set. */
    std::uint8_t alphabet_code;

    /** @brief Width of one packed storage unit in bytes. */
    std::uint8_t storage_unit_byte_width;

    /**
     * @brief Encode the parameters as a PostgreSQL type modifier.
     * @return The compact signed 32-bit type modifier.
     */
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

    /**
     * @brief Decode a PostgreSQL type modifier.
     * @param typmod Encoded type modifier produced by encode().
     * @return Decoded alphabet and storage-width parameters.
     */
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

/**
 * @brief Allocator for a standard-layout compact sequence allocation.
 *
 * A conforming allocator accepts a byte count and returns suitably aligned
 * storage convertible to `p_allocated_type*`, or `nullptr` on failure.
 */
template<class p_allocator_type, class p_allocated_type>
concept stdc_allocator = requires (
    p_allocator_type&& allocator, std::size_t bytes) {
    { std::forward<p_allocator_type>(allocator)(bytes) }
        -> std::convertible_to<p_allocated_type*>;
};

/** @brief Identifies a supported standard-layout compact-sequence header. */
template<class p_candidate_type>
concept is_compact_sequence_stdc =
    requires {
        requires p_candidate_type::is_valid_compact_sequence_stdc == true;
        typename p_candidate_type::supported_alphabets_type;
        typename p_candidate_type::supported_storage_unit_byte_widths_type;
        typename p_candidate_type::bp_size_t;
        typename p_candidate_type::varlena_header_t;
        typename p_candidate_type::alphabet_code_t;
    }
    && supported_alphabet_set<
        typename p_candidate_type::supported_alphabets_type>
    && supported_storage_width_set<
        typename p_candidate_type::supported_storage_unit_byte_widths_type>;

/**
 * @brief Compile-time specialization of a runtime compact-sequence header.
 * @tparam p_stdc_type Standard-layout compact-sequence header type.
 * @tparam p_alphabet Pinned alphabet.
 * @tparam p_storage_unit_byte_width Pinned packed-unit width in bytes.
 */
template<
    class p_stdc_type,
    sequence_alphabet p_alphabet,
    size_t p_storage_unit_byte_width
>
requires (
    is_compact_sequence_stdc<p_stdc_type>
    && alphabet_in_supported_set<
        typename p_stdc_type::supported_alphabets_type, p_alphabet>
    && p_stdc_type::supported_storage_unit_byte_widths_type::template contains<
        p_storage_unit_byte_width>
)
struct compact_sequence_stdc_instance;

/// The standard-layout, C-compatible compact sequence struct. Generic
/// over the alphabet pack, the bp_size integer width, and the varlena
/// header type. `data[]` is a flexible-array member that holds the
/// packed payload; the alignment-padding field bridges the fixed-size header
/// to a uint64_t-aligned `data[]` boundary via the deficit calculation
/// in `compact_sequence_stdc_detail`.
template<
    class p_alphabet_set_type,
    class p_base_pair_size_type = std::uint32_t,
    class p_varlena_header_type = std::int32_t,
    class p_storage_width_set_type =
        all_storage_unit_byte_widths
>
requires (
    supported_alphabet_set<p_alphabet_set_type>
    && supported_storage_width_set<
        p_storage_width_set_type>
)
struct compact_sequence_stdc {
public:
    using supported_alphabets_type = p_alphabet_set_type;
    using supported_storage_unit_byte_widths_type =
        p_storage_width_set_type;
    using bp_size_t                = p_base_pair_size_type;
    using varlena_header_t         = p_varlena_header_type;
    using alphabet_code_t          = typename p_alphabet_set_type::alphabet_code_t;

    static constexpr bool is_valid_compact_sequence_stdc = true;

    static constexpr size_t alignment_deficit =
        compact_sequence_stdc_detail::reserved_deficit<
            sizeof(varlena_header_t),
            sizeof(alphabet_code_t),
            sizeof(bp_size_t)
        >();

    /** @brief PostgreSQL four-byte uncompressed varlena header. */
    varlena_header_t      vl_len_;
    /** @brief Alphabet index in supported_alphabets_type. */
    alphabet_code_t       alphabet_code;
    /** @brief Width of one packed storage unit in bytes. */
    std::uint8_t          storage_unit_byte_width;
    /** @brief Unused alphabet slots in the final packed unit. */
    std::uint16_t         padding_letters;
    /** @brief Number of logical alphabet symbols. */
    bp_size_t             bp_size;
    [[no_unique_address]]
    compact_sequence_stdc_detail::alignment_reserved_holder<alignment_deficit>
        alignment_padding;
    /** @brief First byte of the variable-length packed payload. */
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
    /// through PostgreSQL's machinery. We replicate the encoding
    /// here rather than call `SET_VARSIZE` because this module is shared with
    /// the NO_POSTGRES standalone build, which has no PostgreSQL headers.
    [[nodiscard]] static constexpr varlena_header_t
    encode_varlena_header(std::size_t total_bytes) noexcept {
        return static_cast<varlena_header_t>(
            static_cast<std::uint32_t>(total_bytes) << 2);
    }

    /// Runtime-to-compile-time fanout that pins the alphabet and packed-unit
    /// width before invoking the handler exactly once.
    template<class p_handler_type>
    [[gnu::always_inline]]
    static constexpr void dispatch_instance(
        alphabet_code_t alpha_code,
        std::uint8_t byte_width,
        p_handler_type&& handler)
    {
        p_alphabet_set_type::dispatch_alphabet_by_code(alpha_code,
            [&]<class p_dispatched_alphabet>() {
                p_storage_width_set_type::dispatch(byte_width,
                    [&]<size_t p_dispatched_byte_width>() {
                        handler.template operator()<
                            compact_sequence_stdc_instance<
                                compact_sequence_stdc,
                                p_dispatched_alphabet,
                                p_dispatched_byte_width>
                        >();
                    });
            });
    }

    /// Convenience pass-through to the pack's name lookup so callers that
    /// already have a `compact_sequence_stdc*` don't have to spell out
    /// `p_alphabet_set_type::name_of_alphabet_code(...)`.
    [[nodiscard]] static std::string_view name_of_alphabet_code(
        alphabet_code_t code) noexcept
    {
        return p_alphabet_set_type::name_of_alphabet_code(code);
    }

    /// Streaming recv factory: reads `unit_count` units of `target_storage_byte_width`
    /// bytes each via `recv_fn`, validates against the alphabet's letters_per_unit,
    /// allocates the destination and fills its payload.
    template<
        sequence_alphabet p_alphabet_type,
        class p_receive_context_type,
        class p_receiver_type,
        class p_allocator_type
    >
    requires (
        alphabet_in_supported_set<p_alphabet_set_type, p_alphabet_type>
        && stdc_allocator<p_allocator_type, compact_sequence_stdc>
    )
    [[nodiscard]]
    static compact_sequence_stdc* convert_from(
        std::uint8_t target_storage_byte_width,
        std::uint16_t in_padding_letters,
        bp_size_t in_bp_size,
        std::uint32_t in_unit_count,
        p_receive_context_type& receive_context,
        p_receiver_type&& receiver,
        p_allocator_type&& allocator);

    /// Allocate an empty (zero bp_size, zero padding) sequence with the
    /// requested storage capacity. Caller fills `data[]` directly.
    template<
        sequence_alphabet p_alphabet_type,
        class p_allocator_type
    >
    requires (
        alphabet_in_supported_set<p_alphabet_set_type, p_alphabet_type>
        && stdc_allocator<p_allocator_type, compact_sequence_stdc>
    )
    [[nodiscard]]
    static compact_sequence_stdc* reserve(
        std::uint8_t target_storage_byte_width,
        std::uint32_t unit_count_value,
        p_allocator_type&& allocator);

    /// Walk the payload as uint64_t units of the recorded width, calling
    /// `send_fn` once per logical unit. Wraps a `dispatch_instance` so
    /// the unit count and letters_per_unit are constexpr for the loop.
    template<class p_send_context_type, class p_sender_type>
    void send_to(
        p_send_context_type& send_context,
        p_sender_type&& sender) const;

    /// Hand a typed zero-copy range view to a templated
    /// handler. Dispatches once on (alphabet_code, storage_unit_byte_width).
    template<class p_handler_type>
    [[gnu::always_inline]]
    void convert_to(p_handler_type&& handler) const;

    /// Same-alphabet storage-width recast. Returns a freshly-allocated
    /// destination with the payload memcpy'd (no decode/encode).
    template<class p_allocator_type>
    requires (stdc_allocator<p_allocator_type, compact_sequence_stdc>)
    [[nodiscard]]
    compact_sequence_stdc* recast_storage_width(
        std::uint8_t target_byte_width,
        p_allocator_type&& allocator) const;

    /// Logical storage unit count — recovered via dispatch on alphabet/width.
    [[nodiscard]] size_t unit_count() const noexcept;

    /// Logical payload byte count (excludes the up-to-7-byte uint64 round-up).
    [[nodiscard]] size_t payload_bytes() const noexcept {
        return unit_count() * storage_unit_byte_width;
    }

    /** @brief Return the runtime alphabet and storage-width parameters. */
    [[nodiscard]] constexpr compact_sequence_stdc_type_params
    type_parameters() const noexcept {
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
template<class p_alphabet_pack, class p_size_type, class p_header_type>
struct data_offset_check {
    using stdc_t = compact_sequence_stdc<
        p_alphabet_pack, p_size_type, p_header_type>;
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
/// Pins the alphabet and storage-unit width at the type level.
/// Carries no runtime state; it is purely a
/// type-level brand that wires (a) the bare struct, (b) the pinned
/// alphabet, and (c) the pinned storage width so the
/// `compact_sequence_range` partial specialization can dispatch on it.
template<
    class p_stdc_type,
    sequence_alphabet p_alphabet,
    size_t p_storage_unit_byte_width
>
requires (
    is_compact_sequence_stdc<p_stdc_type>
    && alphabet_in_supported_set<
        typename p_stdc_type::supported_alphabets_type, p_alphabet>
    && p_stdc_type::supported_storage_unit_byte_widths_type::template contains<
        p_storage_unit_byte_width>
)
struct compact_sequence_stdc_instance {
    using compact_sequence_stdc_type = p_stdc_type;
    using alphabet_type              = p_alphabet;
    // Fixed-width standard types keep Highway lane selection identical across
    // LP64 platforms, where Boost's least-width type may choose another C++
    // spelling with the same bit width.
    using storage_unit =
        std::conditional_t<p_storage_unit_byte_width == 8, std::uint64_t,
        std::conditional_t<p_storage_unit_byte_width == 4, std::uint32_t,
        std::conditional_t<p_storage_unit_byte_width == 2, std::uint16_t,
                                                          std::uint8_t>>>;

    static constexpr size_t byte_width        = p_storage_unit_byte_width;
    static constexpr size_t letter_bit_width  =
        seqan3::detail::ceil_log2(seqan3::alphabet_size<p_alphabet>);

    // The pack lookup pins the display name at the type level.
    static constexpr auto alphabet_name =
        p_stdc_type::supported_alphabets_type::template name_of<p_alphabet>();
    static constexpr std::string_view alphabet_name_sv = alphabet_name.view();

    static constexpr bool is_valid_compact_sequence_stdc_instance = true;
};

/// Zero-copy view specialization of `compact_sequence_range` for the
/// `compact_sequence_stdc_instance<…>` brand. Holds a const reference
/// to the underlying `compact_sequence_stdc` and computes padding,
/// bp_size, size, and the read pointer directly off the header fields.
/// No `std::vector` payload; constructing the view is a single reference
/// assignment plus three `DEBUG_ASSERT`s.
template<typename p_instance_type>
requires (is_compact_sequence_stdc_instance<p_instance_type>)
struct compact_sequence_range<p_instance_type>
    : public compact_sequence_range_base<
        compact_sequence_range<p_instance_type>,
        typename p_instance_type::alphabet_type,
        typename p_instance_type::storage_unit,
        p_instance_type::letter_bit_width
> {
public:
    using traits =
        typename compact_sequence_range<p_instance_type>::
            compact_sequence_range_base::traits;
    using base_container =
        const typename p_instance_type::compact_sequence_stdc_type*;
    // Iterator element type stays identical to the instance storage type.
    using iterator =
        const typename p_instance_type::storage_unit*;

private:
    const typename p_instance_type::compact_sequence_stdc_type& storage_;

public:
    /**
     * @brief Construct a zero-copy typed view over a compact header.
     * @param storage Header and packed payload whose lifetime outlives the view.
     */
    explicit constexpr compact_sequence_range(
        const typename p_instance_type::compact_sequence_stdc_type& storage)
        noexcept
        : storage_(storage)
    {
        DEBUG_ASSERT(
            storage.storage_unit_byte_width == p_instance_type::byte_width,
            fmt::format("compact_sequence_range: storage width {} != "
                        "instance width {}",
                        static_cast<unsigned>(storage.storage_unit_byte_width),
                        static_cast<unsigned>(p_instance_type::byte_width)));
        DEBUG_ASSERT(
            storage.padding_letters < traits::letters_per_unit
            || (size() == 0 && storage.padding_letters == 0),
            fmt::format("padding_letters {} >= letters_per_unit {}",
                        static_cast<unsigned>(storage.padding_letters),
                        traits::letters_per_unit));
        DEBUG_ASSERT(
            static_cast<size_t>(storage.bp_size) + storage.padding_letters
            == size() * traits::letters_per_unit
            || (size() == 0 && storage.bp_size == 0
                && storage.padding_letters == 0),
            fmt::format("bp_size {} + padding {} != size {} * lpu {}",
                        static_cast<size_t>(storage.bp_size),
                        static_cast<unsigned>(storage.padding_letters),
                        size(), traits::letters_per_unit));
    }

    /** @return Number of unused logical alphabet slots in the final unit. */
    [[nodiscard]] constexpr size_t padding() const noexcept {
        return storage_.padding_letters;
    }

    /** @return Number of packed storage units occupied by the sequence. */
    [[nodiscard]] constexpr size_t size() const noexcept {
        const size_t bp = storage_.bp_size;
        if (bp == 0) return 0;
        return (bp + traits::letters_per_unit - 1) / traits::letters_per_unit;
    }

    /** @return Number of logical alphabet symbols in the sequence. */
    [[nodiscard]] constexpr size_t bp_size() const noexcept {
        return static_cast<size_t>(storage_.bp_size);
    }

    [[nodiscard]] constexpr iterator cbegin() const noexcept {
        return reinterpret_cast<iterator>(storage_.data);
    }
    [[nodiscard]] constexpr iterator cend() const noexcept {
        return cbegin() + size();
    }
    [[nodiscard]] constexpr iterator begin() const noexcept { return cbegin(); }
    [[nodiscard]] constexpr iterator end() const noexcept { return cend(); }

    [[nodiscard]] constexpr typename traits::storage_unit operator[](
        size_t unit_index) const noexcept {
        return reinterpret_cast<const typename traits::storage_unit*>(
            storage_.data)[unit_index];
    }

    [[nodiscard]] constexpr static const typename traits::storage_unit*
    read_cptr(const iterator& it) noexcept {
        return it;
    }

    [[nodiscard]] friend constexpr bool operator==(
        const compact_sequence_range<p_instance_type>& lhs,
        const compact_sequence_range<p_instance_type>& rhs)
    {
        if (&lhs.storage_ == &rhs.storage_) return true;
        if (lhs.storage_.bp_size != rhs.storage_.bp_size) return false;
        return std::memcmp(lhs.storage_.data, rhs.storage_.data,
            lhs.size() * sizeof(typename traits::storage_unit)) == 0;
    }
};

// Disambiguation: the instance type must NOT also satisfy the two
// concepts that drive the existing compact_sequence_range partial
// specialisations. A future edit that broke this would manifest as an
// "ambiguous partial specialisation" diagnostic at every use site;
// catch it here with a one-line message instead.
namespace compact_sequence_stdc_detail {
using checked_instance_type = compact_sequence_stdc_instance<
    compact_sequence_stdc<supported_alphabets<alphabet_entry<seq::dna4, "dna4">, alphabet_entry<seq::dna5, "dna5">>>,
    seq::dna4,
    size_t{8}>;
static_assert(!is_sequence_custom_alphabet<checked_instance_type>,
              "compact_sequence_stdc_instance must NOT satisfy "
              "is_sequence_custom_alphabet (partial-spec disambiguation)");
static_assert(!sequence_proper_container<checked_instance_type>,
              "compact_sequence_stdc_instance must NOT satisfy "
              "sequence_proper_container (partial-spec disambiguation)");
static_assert(is_compact_sequence_stdc_instance<checked_instance_type>);
}  // namespace compact_sequence_stdc_detail

};

// Out-of-line member definitions inherit export status from the class.

template<
    class p_alphabet_set_type,
    class p_base_pair_size_type,
    class p_varlena_header_type,
    class p_storage_width_set_type
>
requires (
    supported_alphabet_set<p_alphabet_set_type>
    && supported_storage_width_set<p_storage_width_set_type>
)
template<class p_handler_type>
[[gnu::always_inline]]
inline void compact_sequence_stdc<
    p_alphabet_set_type,
    p_base_pair_size_type,
    p_varlena_header_type,
    p_storage_width_set_type>::convert_to(
        p_handler_type&& handler) const
{
    dispatch_instance(alphabet_code, storage_unit_byte_width,
        [&]<class p_instance_type>() {
            handler.template operator()<p_instance_type>(
                compact_sequence_range<p_instance_type>{*this});
        });
}

template<
    class p_alphabet_set_type,
    class p_base_pair_size_type,
    class p_varlena_header_type,
    class p_storage_width_set_type
>
requires (
    supported_alphabet_set<p_alphabet_set_type>
    && supported_storage_width_set<p_storage_width_set_type>
)
template<class p_send_context_type, class p_sender_type>
inline void compact_sequence_stdc<
    p_alphabet_set_type,
    p_base_pair_size_type,
    p_varlena_header_type,
    p_storage_width_set_type>::send_to(
        p_send_context_type& send_context, p_sender_type&& sender) const
{
    dispatch_instance(alphabet_code, storage_unit_byte_width,
        [&]<class p_instance_type>() {
            using storage_unit_type = typename p_instance_type::storage_unit;
            constexpr std::size_t letters_per_unit =
                compact_sequence_range<p_instance_type>::traits::letters_per_unit;
            const std::size_t unit_count_value =
                (bp_size == 0) ? std::size_t{0}
                : (bp_size + letters_per_unit - 1) / letters_per_unit;
            const auto* reader =
                reinterpret_cast<const storage_unit_type*>(this->data);
            for (std::size_t unit_index = 0;
                 unit_index < unit_count_value;
                 ++unit_index) {
                sender.template operator()<std::uint64_t>(
                    send_context,
                    static_cast<std::uint64_t>(reader[unit_index]),
                    static_cast<std::uint8_t>(p_instance_type::byte_width));
            }
        });
}

template<
    class p_alphabet_set_type,
    class p_base_pair_size_type,
    class p_varlena_header_type,
    class p_storage_width_set_type
>
requires (
    supported_alphabet_set<p_alphabet_set_type>
    && supported_storage_width_set<p_storage_width_set_type>
)
inline std::size_t compact_sequence_stdc<
    p_alphabet_set_type,
    p_base_pair_size_type,
    p_varlena_header_type,
    p_storage_width_set_type>::unit_count() const noexcept
{
    std::size_t result = 0;
    dispatch_instance(alphabet_code, storage_unit_byte_width,
        [&]<class p_instance_type>() {
            constexpr std::size_t letters_per_unit =
                compact_sequence_range<p_instance_type>::traits::letters_per_unit;
            result = (bp_size == 0) ? std::size_t{0}
                : (bp_size + letters_per_unit - 1) / letters_per_unit;
        });
    return result;
}

template<
    class p_alphabet_set_type,
    class p_base_pair_size_type,
    class p_varlena_header_type,
    class p_storage_width_set_type
>
requires (
    supported_alphabet_set<p_alphabet_set_type>
    && supported_storage_width_set<p_storage_width_set_type>
)
template<sequence_alphabet p_alphabet, class p_allocator_type>
requires (
    alphabet_in_supported_set<p_alphabet_set_type, p_alphabet>
    && stdc_allocator<p_allocator_type, compact_sequence_stdc<
        p_alphabet_set_type,
        p_base_pair_size_type,
        p_varlena_header_type,
        p_storage_width_set_type>>
)
inline compact_sequence_stdc<
    p_alphabet_set_type,
    p_base_pair_size_type,
    p_varlena_header_type,
    p_storage_width_set_type>*
compact_sequence_stdc<
    p_alphabet_set_type,
    p_base_pair_size_type,
    p_varlena_header_type,
    p_storage_width_set_type>::reserve(
    std::uint8_t target_storage_byte_width,
    std::uint32_t unit_count_value,
    p_allocator_type&& allocator)
{
    DEBUG_ASSERT(
        p_storage_width_set_type::contains_runtime(
            target_storage_byte_width),
        fmt::format("reserve: storage_unit_byte_width {} is not enabled",
                    static_cast<unsigned>(target_storage_byte_width)));

    const std::size_t payload_bytes_value =
        static_cast<std::size_t>(unit_count_value) * target_storage_byte_width;
    const std::size_t total_bytes =
        header_bytes() + round_up_to_uint64(payload_bytes_value);

    auto* header = std::forward<p_allocator_type>(allocator)(total_bytes);
    if (header == nullptr) return nullptr;
    header->vl_len_                 = encode_varlena_header(total_bytes);
    header->alphabet_code           =
        p_alphabet_set_type::template index_of<p_alphabet>();
    header->storage_unit_byte_width = target_storage_byte_width;
    header->padding_letters         = 0;
    header->bp_size                 = 0;
    return header;
}

template<
    class p_alphabet_set_type,
    class p_base_pair_size_type,
    class p_varlena_header_type,
    class p_storage_width_set_type
>
requires (
    supported_alphabet_set<p_alphabet_set_type>
    && supported_storage_width_set<p_storage_width_set_type>
)
template<
    sequence_alphabet p_alphabet,
    class p_receive_context_type,
    class p_receiver_type,
    class p_allocator_type
>
requires (
    alphabet_in_supported_set<p_alphabet_set_type, p_alphabet>
    && stdc_allocator<p_allocator_type, compact_sequence_stdc<
        p_alphabet_set_type,
        p_base_pair_size_type,
        p_varlena_header_type,
        p_storage_width_set_type>>
)
inline compact_sequence_stdc<
    p_alphabet_set_type,
    p_base_pair_size_type,
    p_varlena_header_type,
    p_storage_width_set_type>*
compact_sequence_stdc<
    p_alphabet_set_type,
    p_base_pair_size_type,
    p_varlena_header_type,
    p_storage_width_set_type>::convert_from(
    std::uint8_t target_storage_byte_width,
    std::uint16_t in_padding_letters,
    p_base_pair_size_type in_bp_size,
    std::uint32_t in_unit_count,
    p_receive_context_type& receive_context,
    p_receiver_type&& receiver,
    p_allocator_type&& allocator)
{
    DEBUG_ASSERT(
        p_storage_width_set_type::contains_runtime(
            target_storage_byte_width),
        fmt::format(
            "convert_from(recv): storage_unit_byte_width {} is not enabled",
                    static_cast<unsigned>(target_storage_byte_width)));

    compact_sequence_stdc* result = nullptr;
    p_storage_width_set_type::dispatch(
        target_storage_byte_width,
        [&]<std::size_t p_storage_byte_width>() {
            using storage_unit_type =
                typename boost::uint_t<p_storage_byte_width * 8>::least;
            using sequence_configuration = sequence_custom_alphabet<
                p_alphabet,
                storage_unit_type,
                seqan3::detail::ceil_log2(
                    seqan3::alphabet_size<p_alphabet>)>;
            constexpr std::size_t letters_per_unit =
                compact_sequence_range<sequence_configuration>::
                    traits::letters_per_unit;

            const std::uint64_t derived_unit_count =
                (static_cast<std::uint64_t>(in_bp_size)
                    + letters_per_unit - 1)
                / letters_per_unit;
            const std::uint64_t derived_padding =
                (in_bp_size == 0) ? std::uint64_t{0}
                : (derived_unit_count * letters_per_unit)
                    - static_cast<std::uint64_t>(in_bp_size);
            ASSERT(static_cast<std::uint64_t>(in_unit_count) == derived_unit_count,
                   fmt::format("convert_from(recv): unit_count {} != "
                               "derived {} from bp_size {} lpu {}",
                               in_unit_count,
                               static_cast<std::size_t>(derived_unit_count),
                               static_cast<std::size_t>(in_bp_size),
                               letters_per_unit));
            ASSERT(static_cast<std::uint64_t>(in_padding_letters) == derived_padding,
                   fmt::format("convert_from(recv): padding {} != derived {} "
                               "from bp_size {} lpu {}",
                               static_cast<unsigned>(in_padding_letters),
                               static_cast<std::size_t>(derived_padding),
                               static_cast<std::size_t>(in_bp_size),
                               letters_per_unit));
            ASSERT(in_padding_letters < letters_per_unit
                   || (in_unit_count == 0 && in_padding_letters == 0),
                   fmt::format("convert_from(recv): padding {} >= lpu {}",
                               static_cast<unsigned>(in_padding_letters),
                               letters_per_unit));

            const std::size_t payload_bytes_value =
                static_cast<std::size_t>(in_unit_count)
                * p_storage_byte_width;
            const std::size_t total_bytes =
                header_bytes() + round_up_to_uint64(payload_bytes_value);

            compact_sequence_stdc* header =
                std::forward<p_allocator_type>(allocator)(total_bytes);
            if (header == nullptr) {
                result = nullptr;
                return;
            }
            header->vl_len_ = encode_varlena_header(total_bytes);
            header->alphabet_code =
                p_alphabet_set_type::template index_of<p_alphabet>();
            header->storage_unit_byte_width =
                static_cast<std::uint8_t>(p_storage_byte_width);
            header->padding_letters = in_padding_letters;
            header->bp_size = in_bp_size;

            auto* writer =
                reinterpret_cast<storage_unit_type*>(header->data);
            for (std::uint32_t unit_index = 0;
                 unit_index < in_unit_count;
                 ++unit_index) {
                writer[unit_index] = static_cast<storage_unit_type>(
                    receiver.template operator()<p_alphabet, std::uint64_t>(
                        receive_context,
                        static_cast<std::uint8_t>(p_storage_byte_width)));
            }
            result = header;
        });
    return result;
}

template<
    class p_alphabet_set_type,
    class p_base_pair_size_type,
    class p_varlena_header_type,
    class p_storage_width_set_type
>
requires (
    supported_alphabet_set<p_alphabet_set_type>
    && supported_storage_width_set<p_storage_width_set_type>
)
template<class p_allocator_type>
requires (stdc_allocator<p_allocator_type, compact_sequence_stdc<
    p_alphabet_set_type,
    p_base_pair_size_type,
    p_varlena_header_type,
    p_storage_width_set_type>>)
inline compact_sequence_stdc<
    p_alphabet_set_type,
    p_base_pair_size_type,
    p_varlena_header_type,
    p_storage_width_set_type>*
compact_sequence_stdc<
    p_alphabet_set_type,
    p_base_pair_size_type,
    p_varlena_header_type,
    p_storage_width_set_type>::recast_storage_width(
    std::uint8_t target_byte_width,
    p_allocator_type&& allocator) const
{
    ASSERT(
        p_storage_width_set_type::contains_runtime(
            target_byte_width),
        fmt::format(
            "recast_storage_width: target_byte_width {} is not enabled",
                    static_cast<unsigned>(target_byte_width)));

    std::uint16_t new_padding = 0;
    std::size_t   payload_bytes_value = 0;
    convert_to([&]<class p_instance_type>(
                   const compact_sequence_range<p_instance_type>&) {
        p_storage_width_set_type::dispatch(
            target_byte_width,
            [&]<std::size_t p_target_storage_byte_width>() {
                constexpr std::size_t target_letters_per_unit =
                    (p_target_storage_byte_width * 8)
                    / p_instance_type::letter_bit_width;
                const std::uint64_t derived_units = (bp_size == 0) ? std::uint64_t{0}
                    : (static_cast<std::uint64_t>(bp_size)
                        + target_letters_per_unit - 1)
                        / target_letters_per_unit;
                new_padding   = static_cast<std::uint16_t>(
                    (bp_size == 0) ? 0u
                    : (derived_units * target_letters_per_unit
                        - static_cast<std::uint64_t>(bp_size)));
                payload_bytes_value =
                    static_cast<std::size_t>(derived_units)
                    * p_target_storage_byte_width;
            });
    });

    const std::size_t total_bytes =
        header_bytes() + round_up_to_uint64(payload_bytes_value);
    compact_sequence_stdc* destination =
        std::forward<p_allocator_type>(allocator)(total_bytes);
    if (destination == nullptr) return nullptr;
    destination->vl_len_ = encode_varlena_header(total_bytes);
    destination->alphabet_code = alphabet_code;
    destination->storage_unit_byte_width = target_byte_width;
    destination->bp_size = bp_size;
    destination->padding_letters = new_padding;

    if (payload_bytes_value > 0) {
        std::memcpy(destination->data, this->data, payload_bytes_value);
    }
    return destination;
}

export {

/**
 * @brief Compare compact ranges with different backing configurations.
 * @return `true` when both ranges contain the same logical symbols.
 */
template<
    typename p_left_configuration,
    typename p_right_configuration,
    std::enable_if_t<
        !std::is_same_v<p_left_configuration, p_right_configuration>, bool>
        p_configurations_are_distinct = true>
requires (
    requires (
        compact_sequence_range<p_left_configuration>& lhs,
        compact_sequence_range<p_right_configuration>& rhs) {
        {
            *(lhs.template iter<alphabet_symbol>(lhs.begin()))
                == *(rhs.template iter<alphabet_symbol>(rhs.begin()))
        };
    }
)
inline constexpr bool operator==(
    compact_sequence_range<p_left_configuration>& lhs,
    compact_sequence_range<p_right_configuration>& rhs
) {
    return std::equal(
        lhs.template iter<alphabet_symbol>(lhs.begin()),
        lhs.template iter<alphabet_symbol>(lhs.end()),
        rhs.template iter<alphabet_symbol>(rhs.begin()),
        rhs.template iter<alphabet_symbol>(rhs.end())
    );
}

static_assert(std::is_standard_layout_v<compact_sequence_range<seq::dna4>>);

/** @brief Strand relation used by sequence hashing and ordering operations. */
enum class sequence_strand_mode : std::uint8_t {
    /** Traverse symbols in stored order. */
    forward = 0,
    /** Traverse the reverse complement of the stored symbols. */
    reverse_complement = 1,
    /** Compare strand-normalized values for equality. */
    equal = 2,
};

/**
 * @brief Tests whether a compact range can expose a different storage width.
 * @tparam p_range_configuration Compact-range backing configuration.
 * @tparam p_new_storage_unit_type Requested packed storage-unit type.
 */
template <typename p_range_configuration, typename p_new_storage_unit_type>
concept is_sequence_bit_castable_to = (
    supports_compact_sequence_range<p_range_configuration>
    &&
    is_sequence_custom_alphabet_bit_castable_to<
        sequence_custom_alphabet<
            typename compact_sequence_range<p_range_configuration>::
                traits::alphabet_type,
            typename compact_sequence_range<p_range_configuration>::
                traits::storage_unit,
            compact_sequence_range<p_range_configuration>::traits::bits_per_char>,
        sequence_custom_alphabet<
            typename compact_sequence_range<p_range_configuration>::
                traits::alphabet_type,
            p_new_storage_unit_type,
            compact_sequence_range<p_range_configuration>::traits::bits_per_char>
    >
);

static_assert(is_sequence_bit_castable_to<seq::dna4, uint64_t>);
static_assert(is_sequence_bit_castable_to<seq::dna4, uint32_t>);
static_assert(is_sequence_bit_castable_to<seq::dna4, uint16_t>);
static_assert(is_sequence_bit_castable_to<seq::dna4, uint8_t>);

/**
 * @brief Read-only packed-sequence interface accepted by SIMD iteration.
 *
 * The pointer requirement deliberately uses `convertible_to` so a view may
 * expose a more-qualified compatible pointer without materializing storage.
 */
template <typename p_view_type>
concept supports_compact_sequence_bitcast_view =
    requires (
        p_view_type view,
        typename p_view_type::iterator& iterator,
        const typename p_view_type::iterator& const_iterator,
        size_t distance) {
        typename p_view_type::traits;
        typename p_view_type::traits::storage_unit;
        typename p_view_type::traits::alphabet_type;
        { view.bp_size() } -> std::convertible_to<size_t>;
        { view.byte_size() } -> std::convertible_to<size_t>;
        { view.size() } -> std::convertible_to<size_t>;
        { view.padding() } -> std::convertible_to<size_t>;
        { view.read_cptr(const_iterator) }
            -> std::convertible_to<
                const typename p_view_type::traits::storage_unit*>;
        { p_view_type::advance_iter(iterator, distance) };
        { p_view_type::view_distance(const_iterator, const_iterator) }
            -> std::convertible_to<size_t>;
    };

};

namespace fmt {

template<supports_compact_sequence_range p_configuration, typename p_character_type>
struct range_format_kind<
    compact_sequence_range<p_configuration>, p_character_type> {
    static constexpr auto value = range_format::disabled;
};

template<supports_compact_sequence_range p_configuration>
struct formatter<compact_sequence_range<p_configuration>>
    : formatter<std::string> {
    using range_type = compact_sequence_range<p_configuration>;

    template <typename p_format_context_type>
    auto format(
        compact_sequence_range<p_configuration> sequence,
        p_format_context_type& context) const {
        return formatter<std::string>::format(
            fmt::format(
                "sequence<bps={}|size={}|total={}B>({}->b{} +{})[{}]",
                sequence.bp_size(),
                sequence.size(),
                sequence.byte_size(),
                range_type::traits::letters_per_unit,
                range_type::traits::unit_width,
                sequence.padding(),
                sequence.to_string(true)),
            context);
    }
};


};

static_assert(fmt::has_formatter<compact_sequence_range<std::vector<seq::dna4>>, char>());
