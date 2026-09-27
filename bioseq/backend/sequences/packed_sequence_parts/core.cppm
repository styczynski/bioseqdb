/**
 * @file core.cppm
 * @brief Packed alphabet, dispatch, and storage-width contracts.
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
/**
 * @brief Adapts an unpacked random-access alphabet range to packed-range APIs.
 * @tparam p_range_type Borrowed alphabet-symbol container.
 */
template<typename p_range_type>
concept sequence_proper_container =
    sequence_alphabet<std::remove_reference_t<
        std::ranges::range_reference_t<p_range_type>>>
    && std::ranges::sized_range<p_range_type>
    && std::ranges::random_access_range<p_range_type>;

/** @brief Detects a branded `compact_sequence_stdc_instance` type. */
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

/** @brief Requires a valid alphabet and runtime display-name entry. */
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
 * @brief Tests one entry name against every following pack entry.
 * @tparam p_entry_index Index of the selected entry.
 * @tparam p_entry_types Complete alphabet-entry pack.
 * @return `true` when no following name matches the selected entry.
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
 * @brief Tests whether every alphabet-entry display name is unique.
 * @tparam p_entry_types Alphabet-entry pack.
 * @return `true` when all display names are distinct.
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
 * @brief Computes live bytes in the formatted alphabet-name list.
 * @tparam p_entry_types Alphabet-entry pack.
 * @return Character count excluding a null terminator.
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
 * @brief Builds the quoted comma-separated alphabet-name buffer.
 * @tparam p_entry_types Alphabet-entry pack.
 * @return Fixed character array containing every display name.
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

/** @brief Number of live characters in the formatted name buffer. */
template <class... p_entry_types>
inline constexpr std::size_t valid_names_live_length_v =
    valid_names_live_length<p_entry_types...>();

}  // namespace detail_supported_alphabets

/**
 * @brief Closed compile-time set of alphabets supported by one stored type.
 * @tparam p_entries Alphabet and name entries with unique types and display names.
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

    /** @brief Matches one runtime alphabet code during type-pack dispatch. */
    struct code_match_probe {
        alphabet_code_t runtime_code;
        /** @brief Tests whether the current pack cursor equals the runtime code. */
        template <class p_entry_type>
        [[gnu::always_inline]]
        constexpr bool matches(alphabet_code_t cursor) const noexcept {
            return cursor == runtime_code;
        }
        /** @brief Rejects a runtime code beyond the final pack cursor. */
        [[noreturn]] void on_no_match(alphabet_code_t cursor) const {
            ASSERT(false,
                fmt::format("dispatch_alphabet_by_code: code {} > pack max {}",
                            static_cast<std::size_t>(runtime_code),
                            static_cast<std::size_t>(cursor)));
        }
    };

    /** @brief Matches one runtime display name during type-pack dispatch. */
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
        /** @brief Leaves the external match flag unchanged after exhaustion. */
        constexpr void on_no_match(alphabet_code_t /*cursor*/) const noexcept {
            // matched stays false; caller checks the return value.
        }
    };

    /** @brief Extracts a display name for one runtime alphabet code. */
    struct code_to_name_probe {
        alphabet_code_t runtime_code;
        /** @brief Returns the current entry name when its cursor matches. */
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

    /** @brief Extracts a pack code for one runtime display name. */
    struct name_to_code_probe {
        const char* runtime_name;
        /** @brief Returns the current cursor when its entry name matches. */
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
     * @brief Adapts alphabet-entry dispatch to an alphabet-typed handler.
     * @tparam p_handler_type Handler invoked with an alphabet template argument.
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

    /**
     * @brief Returns the compile-time pack index of an alphabet.
     * @tparam p_alphabet Alphabet that must belong to the pack.
     */
    template <sequence_alphabet p_alphabet>
    [[nodiscard]] static constexpr alphabet_code_t index_of() {
        using namespace ::bioseq::common::templating;
        return static_cast<alphabet_code_t>(
            index_of_type_pack_checked<
                p_alphabet, typename p_entries::alphabet_type...>());
    }

    /**
     * @brief Returns the compile-time display name of an alphabet.
     * @tparam p_alphabet Alphabet that must belong to the pack.
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
     * @brief Maps a runtime alphabet code to its display name.
     * @param code Runtime pack index.
     * @return Entry display name, or `"?"` for an invalid code.
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
     * @brief Maps a runtime display name to its alphabet code.
     * @param name Null-terminated display name.
     * @return Pack index, or no value when the name is absent.
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

/**
 * @brief Closed compile-time set of supported packed storage byte widths.
 * @tparam p_byte_widths Unique widths selected from 1, 2, 4, and 8 bytes.
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
    /** @brief Dispatches the first matching compile-time width. */
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
     * @brief Dispatches a runtime width as a compile-time size value.
     * @tparam p_handler_type Handler invoked with the matching byte width.
     * @param byte_width Runtime storage width.
     * @param handler Handler invoked for the selected width.
     */
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

/** @brief Storage-width set containing 1, 2, 4, and 8 byte units. */
using all_storage_unit_byte_widths =
    supported_storage_unit_byte_widths<1u, 2u, 4u, 8u>;

/**
 * @brief Dispatches a runtime width across all supported packed unit sizes.
 * @tparam p_handler_type Handler invoked with the matching byte width.
 * @param byte_width Runtime storage width.
 * @param handler Handler invoked for the selected width.
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
 * @brief Aligns a byte count to the next 64-bit storage boundary.
 * @param bytes Unaligned byte count.
 * @return Aligned byte count.
 */
[[gnu::always_inline]]
inline constexpr size_t round_up_to_uint64(size_t bytes) noexcept {
    constexpr size_t u64 = sizeof(std::uint64_t);
    return ((bytes + u64 - 1) / u64) * u64;
}

};
