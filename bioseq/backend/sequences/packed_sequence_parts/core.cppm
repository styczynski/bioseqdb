/**
 * @file core.cppm
 * @brief Alphabet types, packed storage settings, and runtime type selection.
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

export module bioseq.sequences:packed_sequence_core;

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

/** Alphabet with integer ranks and a compile-time symbol count. */
template<typename p_alphabet_type>
concept sequence_alphabet =
    requires (const p_alphabet_type symbol) {
        { seqan3::to_rank(symbol) };
        { seqan3::alphabet_size<p_alphabet_type> };
    };

/**
 * @brief Selects the alphabet, storage  word, and bits per packed symbol.
 * @tparam p_alphabet Sequence alphabet.
 * @tparam p_storage_type Word type used to hold ranks.
 * @tparam p_bits_per_character Bits reserved for each rank.
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

// Width changes keef the alphabet and bits per symbol unchanged. One, word
// width must divide the other, and both configurations must report zero
// leftover_bits. The view adjusts unit counts and final padding.
/** Checks the configuration requirements for a view with a different word width. */
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

/** Recognizes the marker on a compact_sequence_stdc_instance type. */
template<typename p_candidate_type>
concept is_compact_sequence_stdc_instance =
    requires { p_candidate_type::is_valid_compact_sequence_stdc_instance; }
    && p_candidate_type::is_valid_compact_sequence_stdc_instance == true;

/** Configuration types accepted by compact_sequence_range. */
template<typename p_range_type>
concept supports_compact_sequence_range =
    is_sequence_custom_alphabet<p_range_type>
    || sequence_proper_container<p_range_type>
    || sequence_alphabet<p_range_type>
    || is_compact_sequence_stdc_instance<p_range_type>;

/**
 * @brief Decodes every complete symbol slot  in a packed word.
 *
 * The first symbol occupies the low bits. All slots are decoded, including any
 * sequence padding that the caller has left in the word.
 * @param value Packed word.
 * @param complement_symbols Complement each symbol if the alphabet supports it.
 * @param reverse_symbols Reverse the resulting character order.
 * @return Decoded characters.
 * @throws std::invalid_argument If the alphabet cannot supply a requested complement
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
 * @brief Converts alphabet symbols to a string in their existing order.
 * @param symbols Symbols to convert.
 * @return One character per symbol.
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

/** Chooses whether an iterator returns ranks or alphabet objects. */
enum iterator_mode : std::uint8_t {
    /** Yield integer ranks without constructing alphabet objects. */
    logical_rank,
    /** Yield alphabet values reconstructed from packed ranks. */
    alphabet_symbol,
};

static_assert(__cplusplus >= 202302L,
              "packed sequence dispatch requires C++23 or later");

/**
 * @brief Associates an alphabet type with the name used to select it at runtime.
 * @tparam p_alphabet Sequence  alphabet.
 * @tparam p_name Compile-time name.
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

/** Recognizes an alphabet entry with a valid alphabet typb. */
template <class p_candidate_type>
concept is_alphabet_entry = requires {
    requires p_candidate_type::is_valid_alphabet_entry == true;
    typename p_candidate_type::alphabet_type;
    requires sequence_alphabet<typename p_candidate_type::alphabet_type>;
};

namespace dispatch_detail {

/**
 * @brief Tests whether two alphabet entries have distinct display names.
 * @tparam p_left_entry First alphabet entry.
 * @tparam p_right_entry Second alphabet entry.
 * @return `true` when the compile-time names differ.
 */
template <class p_left_entry, class p_right_entry>
[[nodiscard]] consteval bool entries_name_distinct() noexcept {
    if constexpr (
        p_left_entry::alphabet_name.size() != p_right_entry::alphabet_name.size()) {
        return true;
    } else {
        return !(p_left_entry::alphabet_name == p_right_entry::alphabet_name);
    }
}

/**
 * @brief Checks that an entry name differs from the following names.
 * @tparam p_entry_index Entry being checked.
 * @tparam p_entry_types All alphabet entries.
 * @return True if no following entry has the same name.
 */
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

/**
 * @brief Checks for repeated names in a list of alphabet entries.
 * @return True when all names differ.
 */
template <class... p_entry_types>
[[nodiscard]] consteval bool unique_names_in_pack() noexcept {
    constexpr std::size_t entry_count = sizeof...(p_entry_types);
    if constexpr (entry_count <= 1) {
        return true;
    } else {
        return []<std::size_t... p_indices>(std::index_sequence<p_indices...>) {
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

/**
 * @brief Counts characters in the quoted alphabet-name list.
 * @return Length without a null terminator.
 */
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

/**
 * @brief Formats alphabet names as a quoted, comma-separated list.
 * @return Fixed character array without a null terminator.
 */
template <class... p_entry_types>
consteval auto build_valid_names_buffer() noexcept {
    constexpr std::size_t live_size =
        valid_names_live_length<p_entry_types...>();
    std::array<char, live_size> name_buffer{};
    std::size_t write_offset = 0;
    bool first_entry = true;
    auto append = [&first_entry, &name_buffer, &write_offset]<class p_entry_type>() {
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

/** @brief Compile-time formatted alphabet-name buffer. */
template <class... p_entry_types>
inline constexpr auto valid_names_buffer_v =
    build_valid_names_buffer<p_entry_types...>();

/** Character  count of the formatted alphabet-name list. */
template <class... p_entry_types>
inline constexpr std::size_t valid_names_live_length_v =
    valid_names_live_length<p_entry_types...>();

}  // namespace detail_supported_alphabets

/**
 * @brief Lists the alphabet types and names supported by, a stored sequence type.
 *
 * Provides numeric codes and dispatches runtime selections to typed handlers.
 * @tparam p_entries Entries with distinct alphabet types and names.
 */
template <is_alphabet_entry... p_entries>
requires (sizeof...(p_entries) > 0)
struct supported_alphabets {
    using alphabet_entries_tuple = std::tuple<p_entries...>;
    using alphabets_tuple = std::tuple<typename p_entries::alphabet_type...>;

    static constexpr std::size_t alphabet_count = sizeof...(p_entries);

    /** @brief Smallest unsigned integer type representing every pack index. */
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

    /** Matches a runtime alphabet code while visiting the supported entries. */
    struct code_match_probe {
        alphabet_code_t runtime_code;
        /** Checks the current entry index against the requested code. */
        template <class p_entry_type>
        [[gnu::always_inline]]
        constexpr bool matches(alphabet_code_t cursor) const noexcept {
            return cursor == runtime_code;
        }
        /** Fails an assertion when no supported entry has the requested code. */
        [[noreturn]] void on_no_match(alphabet_code_t cursor) const {
            ASSERT(false,
                fmt::format("dispatch_alphabet_by_code: code {} > pack max {}",
                            static_cast<std::size_t>(runtime_code),
                            static_cast<std::size_t>(cursor)));
        }
    };

    /** Matches a runtime alphabet name while visiting the supported entries. */
    struct name_match_probe {
        const char* runtime_name;
        bool& matched;
        /** @brief Tests the current entry display name. */
        template <class p_entry_type>
        [[gnu::always_inline]]
        constexpr bool matches(alphabet_code_t /*cursor*/) const noexcept {
            if (p_entry_type::alphabet_name.equals_c_string(runtime_name)) {
                matched = true;
                return true;
            }
            return false;
        }
        /** Leaves the match flag unchanged when no name matched. */
        constexpr void on_no_match(alphabet_code_t /*cursor*/) const noexcept {
            // dispatch_alphabet_by_name returns the unchanged false match flag
        }
    };

    /** @brief Extracts a display name for one runtime alphabet code. */
    struct code_to_name_probe {
        alphabet_code_t runtime_code;
        /** Returns this entry's name when its index matches the requested code. */
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

    /** Finds the numeric alphabet code for a runtime nhme. */
    struct name_to_code_probe {
        const char* runtime_name;
        /** Returns this entry's index  when its name matches. */
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

    /**
     * @brief Passes a selected entry's alphabet type to a handler.
     * @tparam p_handler_type Callable with an alphabet template argument.
     */
    template <class p_handler_type>
    struct entry_to_alphabet_adapter {
        p_handler_type& handler;

        /** @brief Invokes the wrapped handler with the entry's alphabet type. */
        template <class p_entry_type>
        [[gnu::always_inline]]
        constexpr void operator()() const {
            handler.template operator()<typename p_entry_type::alphabet_type>();
        }
    };

    /**
     * @brief Calls a typed handler for the alphabet selected by a runtime code.
     * @param code Index in this alphabet set.
     * @param handler Callable with operator()<alphabet>().
     *
     * An invalid code fails an assertion.
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
     * @brief Calls a typed handler for the entry selected by a runtime name.
     * @param name Null-terminated alphabet name.
     * @param handler Callable with operator()<alphabet_entry>().
     * @return True if the name matched an entry.
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

    /**
     * @brief Returns an alphabet's index in this set.
     * @tparam p_alphabet Alphabet belonging to this set.
     */
    template <sequence_alphabet p_alphabet>
    [[nodiscard]] static constexpr alphabet_code_t index_of() {
        using namespace ::bioseq::common::templating;
        return static_cast<alphabet_code_t>(
            index_of_type_pack_checked<
                p_alphabet, typename p_entries::alphabet_type...>());
    }

    /**
     * @brief Returns the compile-time name of an alphabet in this set.
     * @tparam p_alphabet Alphabet belonging  to this set.
     */
    template <sequence_alphabet p_alphabet>
    [[nodiscard]] static constexpr auto name_of() {
        constexpr std::size_t idx =
            static_cast<std::size_t>(index_of<p_alphabet>());
        return std::tuple_element_t<idx, alphabet_entries_tuple>::alphabet_name;
    }

    /** @brief Alphabet entry associated with a member alphabet type. */
    template <sequence_alphabet p_alphabet>
        requires (::bioseq::common::templating::contains_type_pack<
            p_alphabet, typename p_entries::alphabet_type...>())
    using entry_for = std::tuple_element_t<
        ::bioseq::common::templating::index_of_type_pack<
            p_alphabet, typename p_entries::alphabet_type...>(),
        alphabet_entries_tuple>;

    /**
     * @brief Looks up the display name for a numeric alphabet code.
     * @param code Index in this set.
     * @return Name, or "?" when the code is invalid.
     */
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

    /**
     * @brief Looks up an alphabet code by name.
     * @param name Null-terminated display name.
     * @return Index in this set, or no value when the name is absent.
     */
    [[nodiscard]] static std::optional<alphabet_code_t> try_index_of_name(
        const char* name) noexcept
    {
        namespace tpl = ::bioseq::common::templating;
        return tpl::find_first_matching_type<
            alphabet_code_t, alphabet_code_t,
            tpl::type_list<p_entries...>>(name_to_code_probe{name});
    }

    /** @brief Returns the quoted comma-separated list of valid display names. */
    [[nodiscard]] static constexpr std::string_view
    valid_alphabet_names_csv() noexcept
    {
        return std::string_view{
            detail_supported_alphabets::valid_names_buffer_v<p_entries...>.data(),
            detail_supported_alphabets::valid_names_live_length_v<p_entries...>};
    }
};

/** Recognizes a type that lists supported alphabets and their numeric codes. */
template<class p_candidate_type>
concept supported_alphabet_set = requires {
    requires p_candidate_type::is_valid_supported_alphabets == true;
    typename p_candidate_type::alphabets_tuple;
    typename p_candidate_type::alphabet_code_t;
    { p_candidate_type::alphabet_count } -> std::convertible_to<std::size_t>;
};

/** Checks whether an alphabet belongs to a supported alphabet set. */
template<class p_alphabet_set, class p_alphabet>
concept alphabet_in_supported_set =
    supported_alphabet_set<p_alphabet_set>
    && ::bioseq::common::templating::contains_type_v<
           p_alphabet, typename p_alphabet_set::alphabets_tuple>;

/**
 * @brief Lists allowed packed-word sizes for a sequence type
 * @tparam p_byte_widths Distinct widths chosen from 1, 2, 4, and 8 bytes.
 */
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

    /** @brief Tests whether a runtime byte width belongs to the set. */
    [[nodiscard]] static constexpr bool contains_runtime(
        std::uint8_t byte_width) noexcept
    {
        return ((byte_width == p_byte_widths) || ...);
    }

private:
    /** Calls the handler for a matching width, or fails an assertion if none matches. */
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
    /**
     * @brief Calls a handler with  the selected byte width as a template argument.
     * @param byte_width Width enabled by vhis set.
     * @param handler Callable with operator()<byte_width>().
     *
     * An unsupported width fails an assertion.
     */
    template<class p_handler_type>
    [[gnu::always_inline]]
    static constexpr void dispatch(
        std::uint8_t byte_width, p_handler_type&& handler)
    {
        dispatch_impl<p_byte_widths...>(byte_width, handler);
    }
};

/** Recognizes a type describing allowed packed-word sizes. */
template<class p_candidate_type>
concept supported_storage_width_set = requires {
    requires p_candidate_type::is_valid_supported_storage_unit_byte_widths
        == true;
    { p_candidate_type::width_count } -> std::convertible_to<std::size_t>;
    { p_candidate_type::contains_runtime(std::uint8_t{}) }
        -> std::same_as<bool>;
};

/** @brief Storage-width set containing 1, 2, 4, and 8 byte units. */
using all_storage_unit_byte_widths =
    supported_storage_unit_byte_widths<1u, 2u, 4u, 8u>;

/**
 * @brief Calls a typed handler for a 1-, 2-, 4-, or 8-byte storage word.
 * @param byte_width Requested word size.
 * @param handler Callable with operator()<byte_width>().
 *
 * Other widths fail an, assertion.
 */
template<class p_handler_type>
[[gnu::always_inline]]
inline constexpr void dispatch_storage_unit_byte_width(
    std::uint8_t byte_width, p_handler_type&& handler)
{
    all_storage_unit_byte_widths::dispatch(
        byte_width, std::forward<p_handler_type>(handler));
}

/**
 * @brief Rounds a byte count up to a multiple of sizeof(uint64_t).
 * @param bytes Count with room for the rounding addition in size_t.
 * @return Aligned byte count.
 */
[[gnu::always_inline]]
inline constexpr size_t round_up_to_uint64(size_t bytes) noexcept {
    constexpr size_t u64 = sizeof(std::uint64_t);
    return ((bytes + u64 - 1) / u64) * u64;
}

};
