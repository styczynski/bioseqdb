/**
 * @file cigar_alphabet.cppm
 * @brief CIGAR operation alphabet, semantic roles, spaces and shared errors.
 */
export module bioseq.sequences.cigar_alphabet;

import std;
export import bioseq.common.seqan_adapter;
import bioseq.storage.size;

export namespace bioseq {

/** Declared operation subalphabet; count is never a stored space. */
enum class cigar_space : std::uint8_t {
    compatibility, reference, reference_n_regions_removed,
    query, query_before_hard_clipping, query_after_soft_clipping,
    pairwise, pairwise_n_regions_removed, pairwise_dense, count
};

/** Physical sequence codec; fragment envelopes are internal storage values. */
enum class sequence_encoding : std::uint8_t {
    rank_packed = 0, cigar_rle = 1, cigar_rle_fragment = 2
};

/** Biological behavior owned by the alphabet, independently of physical rank. */
enum class cigar_role : std::uint8_t {
    paired_unspecified, paired_equal, paired_different, insertion, deletion,
    reference_skip, soft_clip, hard_clip, padding, opaque
};

/** One additional symbol and its optional external native opcode. */
struct cigar_extension_definition {
    char character;
    cigar_role role{cigar_role::opaque};
    std::optional<std::uint8_t> native_opcode{};
};

inline constexpr std::array cigar_extensions{
    cigar_extension_definition{'B', cigar_role::opaque, std::uint8_t{9}}
};
// Intentional pinned-dependency exception, isolated by the SeqAn adapter:
// cigar::operation aliases this exact type in the vendored revision. Loading
// its public alias fails Clang22/libc++26 module compilation through SeqAn's
// unrelated tuple machinery; exposition_only has no claimed stability here.
using standard_cigar_operation = seqan3::exposition_only::cigar_operation;
inline constexpr std::size_t standard_cigar_size =
    seqan3::alphabet_size<standard_cigar_operation>;
inline constexpr std::size_t cigar_symbol_count =
    standard_cigar_size + cigar_extensions.size();

/** Position alphabet delegates all standard character mappings to SeqAn. */
class cigar_symbol : public seqan3::alphabet_base<cigar_symbol, cigar_symbol_count, char> {
    using base_type = seqan3::alphabet_base<cigar_symbol, cigar_symbol_count, char>;
    friend base_type;

    /** Decode a valid rank through the dependency or extension definition. */
    [[nodiscard]] static constexpr char rank_to_char(rank_type rank) noexcept {
        const auto index = static_cast<std::size_t>(rank);
        if (index < standard_cigar_size) {
            using standard_rank = seqan3::alphabet_rank_t<standard_cigar_operation>;
            return seqan3::to_char(standard_cigar_operation{}.assign_rank(
                static_cast<standard_rank>(rank)));
        }
        return cigar_extensions[index - standard_cigar_size].character;
    }

    /** Total assignment; external input must first pass char_is_valid. */
    [[nodiscard]] static constexpr rank_type char_to_rank(char character) noexcept {
        for (std::size_t index = 0; index < cigar_extensions.size(); ++index)
            if (cigar_extensions[index].character == character)
                return static_cast<rank_type>(standard_cigar_size + index);
        return static_cast<rank_type>(seqan3::to_rank(
            standard_cigar_operation{}.assign_char(character)));
    }
public:
    constexpr cigar_symbol() noexcept = default;

    /** Validate exact character membership, without permissive normalization. */
    [[nodiscard]] static constexpr bool char_is_valid(char character) noexcept {
        if (seqan3::char_is_valid_for<standard_cigar_operation>(character)) return true;
        for (const auto& extension : cigar_extensions)
            if (extension.character == character) return true;
        return false;
    }
};

/** Classify each standard operation once, at the alphabet boundary. */
[[nodiscard]] constexpr cigar_role standard_cigar_role(standard_cigar_operation operation) noexcept {
    switch (seqan3::to_char(operation)) {
        case 'M': return cigar_role::paired_unspecified;
        case '=': return cigar_role::paired_equal;
        case 'X': return cigar_role::paired_different;
        case 'I': return cigar_role::insertion;
        case 'D': return cigar_role::deletion;
        case 'N': return cigar_role::reference_skip;
        case 'S': return cigar_role::soft_clip;
        case 'H': return cigar_role::hard_clip;
        case 'P': return cigar_role::padding;
    }
    return cigar_role::opaque;
}

/** Resolve one concrete alphabet symbol's semantics. */
[[nodiscard]] constexpr cigar_role operation_role(cigar_symbol operation) noexcept {
    const auto rank = static_cast<std::size_t>(seqan3::to_rank(operation));
    if (rank >= standard_cigar_size) return cigar_extensions[rank - standard_cigar_size].role;
    using standard_rank = seqan3::alphabet_rank_t<standard_cigar_operation>;
    return standard_cigar_role(standard_cigar_operation{}.assign_rank(static_cast<standard_rank>(rank)));
}

/** Semantic customization point for other admitted CIGAR alphabets. */
template<seqan3::alphabet Alphabet> struct cigar_alphabet_traits {};
template<> struct cigar_alphabet_traits<cigar_symbol> {
    /** Resolve operation semantics without referring to storage layout. */
    [[nodiscard]] static constexpr cigar_role role(cigar_symbol operation) noexcept {
        return operation_role(operation);
    }
};

/** An alphabet whose entries have explicit CIGAR semantics. */
template<class Alphabet>
concept cigar_operation_alphabet = seqan3::alphabet<Alphabet> && requires(Alphabet symbol) {
    { cigar_alphabet_traits<Alphabet>::role(symbol) } -> std::same_as<cigar_role>;
};

/** Alphabet-sized whitelist; rank capacity is never a fixed operation count. */
template<seqan3::alphabet Alphabet>
using cigar_operation_mask = std::bitset<seqan3::alphabet_size<Alphabet>>;

/** One positive count and operation; codecs never serialize its implicit padding. */
template<cigar_operation_alphabet Alphabet> struct cigar_run_value {
    using alphabet_type = Alphabet;
    using length_type = std::uint32_t;
    length_type length{};
    Alphabet operation{};
    [[nodiscard]] constexpr bool operator==(const cigar_run_value&) const = default;
};
using cigar_run = cigar_run_value<cigar_symbol>;

/** Recoverable input, representation and allocation failures. */
enum class cigar_error_code : std::uint8_t {
    invalid_text, zero_count, invalid_operation, count_overflow,
    length_overflow, invalid_encoding, unsupported_geometry,
    incompatible_alphabet, input_length_mismatch, unknown_symbol_unavailable,
    allocation_rejected, output_too_large
};
struct cigar_error {
    cigar_error_code code;
    std::size_t input_offset{};
    [[nodiscard]] constexpr bool operator==(const cigar_error&) const = default;
};

/** Whether a role advances both alignment axes. */
[[nodiscard]] constexpr bool is_paired(cigar_role role) noexcept {
    return role == cigar_role::paired_unspecified || role == cigar_role::paired_equal ||
           role == cigar_role::paired_different;
}
/** Whether an operation advances both alignment axes. */
template<cigar_operation_alphabet Alphabet>
[[nodiscard]] constexpr bool is_paired(Alphabet operation) noexcept {
    return is_paired(cigar_alphabet_traits<Alphabet>::role(operation));
}
/** Whether a semantic class reports M-like or explicitly equal positions. */
[[nodiscard]] constexpr bool is_reported_match(cigar_role role) noexcept {
    return role == cigar_role::paired_unspecified || role == cigar_role::paired_equal;
}
/** Whether a semantic class explicitly marks different symbols. */
[[nodiscard]] constexpr bool is_explicit_mismatch(cigar_role role) noexcept {
    return role == cigar_role::paired_different;
}
/** Select roles across the actual alphabet with a compile-time predicate. */
template<cigar_operation_alphabet Alphabet, auto Predicate>
    requires std::predicate<decltype(Predicate), cigar_role>
[[nodiscard]] constexpr cigar_operation_mask<Alphabet> cigar_role_mask() {
    cigar_operation_mask<Alphabet> mask;
    using rank_type = seqan3::alphabet_rank_t<Alphabet>;
    for (std::size_t rank = 0; rank < seqan3::alphabet_size<Alphabet>; ++rank) {
        const auto symbol = Alphabet{}.assign_rank(static_cast<rank_type>(rank));
        mask.set(rank, Predicate(cigar_alphabet_traits<Alphabet>::role(symbol)));
    }
    return mask;
}
/** Whether biological geometry is defined for an operation. */
template<cigar_operation_alphabet Alphabet>
[[nodiscard]] constexpr bool geometry_supported(Alphabet operation) noexcept {
    return cigar_alphabet_traits<Alphabet>::role(operation) != cigar_role::opaque;
}
/** Validate a declared space, excluding the cardinality sentinel. */
[[nodiscard]] constexpr bool valid_cigar_space(cigar_space space) noexcept {
    return std::to_underlying(space) < std::to_underlying(cigar_space::count);
}
/** Derive operation visibility from its alphabet-owned biological role. */
template<cigar_operation_alphabet Alphabet>
[[nodiscard]] constexpr bool visible_in_space(Alphabet operation, cigar_space space) noexcept {
    if (!valid_cigar_space(space)) return false;
    if (space == cigar_space::compatibility) return true;
    const auto role = cigar_alphabet_traits<Alphabet>::role(operation);
    if (is_paired(role)) return true;
    switch (space) {
        case cigar_space::reference:
            return role == cigar_role::deletion || role == cigar_role::reference_skip;
        case cigar_space::reference_n_regions_removed:
            return role == cigar_role::deletion;
        case cigar_space::query:
            return role == cigar_role::insertion || role == cigar_role::soft_clip;
        case cigar_space::query_before_hard_clipping:
            return role == cigar_role::insertion || role == cigar_role::soft_clip || role == cigar_role::hard_clip;
        case cigar_space::query_after_soft_clipping:
            return role == cigar_role::insertion;
        case cigar_space::pairwise:
            return role == cigar_role::insertion || role == cigar_role::deletion || role == cigar_role::reference_skip;
        case cigar_space::pairwise_n_regions_removed:
            return role == cigar_role::insertion || role == cigar_role::deletion;
        case cigar_space::pairwise_dense: return false;
        default: return false;
    }
}
/** Build a space's whitelist by visiting every rank of the admitted alphabet. */
template<cigar_operation_alphabet Alphabet>
[[nodiscard]] constexpr cigar_operation_mask<Alphabet> cigar_space_mask(cigar_space space) noexcept {
    cigar_operation_mask<Alphabet> mask;
    using rank_type = seqan3::alphabet_rank_t<Alphabet>;
    for (std::size_t rank = 0; rank < seqan3::alphabet_size<Alphabet>; ++rank)
        mask[rank] = visible_in_space(Alphabet{}.assign_rank(static_cast<rank_type>(rank)), space);
    return mask;
}
/** Whether an operation can be stripped from an unmatched query flank. */
template<cigar_operation_alphabet Alphabet>
[[nodiscard]] constexpr bool is_query_strip_candidate(Alphabet operation) noexcept {
    return geometry_supported(operation) && visible_in_space(operation, cigar_space::query) &&
           !visible_in_space(operation, cigar_space::reference);
}
/** Whether an operation can be stripped from an unmatched reference flank. */
template<cigar_operation_alphabet Alphabet>
[[nodiscard]] constexpr bool is_reference_strip_candidate(Alphabet operation) noexcept {
    return geometry_supported(operation) && visible_in_space(operation, cigar_space::reference) &&
           !visible_in_space(operation, cigar_space::query);
}

inline constexpr std::array<std::string_view, std::to_underlying(cigar_space::count)> cigar_space_names{
    "cigar", "cigar-reference", "cigar-reference-N-regions-removed", "cigar-query",
    "cigar-query-before-hard-clipping", "cigar-query-after-soft-clipping", "cigar-pairwise",
    "cigar-pairwise-N-regions-removed", "cigar-pairwise-dense"
};
/** Return the unique SQL alphabet spelling for a valid space, or an empty name. */
[[nodiscard]] constexpr std::string_view cigar_space_name(cigar_space space) noexcept {
    return valid_cigar_space(space) ? cigar_space_names[std::to_underlying(space)] : std::string_view{};
}
/** Parse the shared SQL/full-space spelling, or its plain projection suffix. */
[[nodiscard]] constexpr std::optional<cigar_space> cigar_space_from_name(std::string_view name) noexcept {
    if (name == "default" || name == "compatibility") return cigar_space::compatibility;
    for (std::size_t index = 0; index < cigar_space_names.size(); ++index) {
        const auto full_name = cigar_space_names[index];
        if (name == full_name || (index != 0 && name == full_name.substr(std::string_view{"cigar-"}.size())))
            return static_cast<cigar_space>(index);
    }
    return std::nullopt;
}

/** Resolve BAM native opcode at the boundary, never from SeqAn rank. */
[[nodiscard]] constexpr std::optional<std::uint8_t> cigar_native_opcode(cigar_symbol operation) noexcept {
    const auto rank = static_cast<std::size_t>(seqan3::to_rank(operation));
    if (rank >= standard_cigar_size) return cigar_extensions[rank - standard_cigar_size].native_opcode;
    switch (operation_role(operation)) {
        case cigar_role::paired_unspecified: return 0;
        case cigar_role::insertion: return 1;
        case cigar_role::deletion: return 2;
        case cigar_role::reference_skip: return 3;
        case cigar_role::soft_clip: return 4;
        case cigar_role::hard_clip: return 5;
        case cigar_role::padding: return 6;
        case cigar_role::paired_equal: return 7;
        case cigar_role::paired_different: return 8;
        default: return std::nullopt;
    }
}
/** Decode a native opcode by visiting the single alphabet-owned mapping. */
[[nodiscard]] constexpr std::optional<cigar_symbol> cigar_symbol_from_native_opcode(std::uint8_t opcode) noexcept {
    using rank_type = seqan3::alphabet_rank_t<cigar_symbol>;
    for (std::size_t rank = 0; rank < seqan3::alphabet_size<cigar_symbol>; ++rank) {
        const auto symbol = cigar_symbol{}.assign_rank(static_cast<rank_type>(rank));
        if (cigar_native_opcode(symbol) == opcode) return symbol;
    }
    return std::nullopt;
}

namespace cigar_alphabet_detail {
/** Fail compilation if extension syntax/native codes or upstream roles drift. */
[[nodiscard]] consteval bool definitions_valid() {
    using standard_rank = seqan3::alphabet_rank_t<standard_cigar_operation>;
    for (std::size_t rank = 0; rank < standard_cigar_size; ++rank)
        if (standard_cigar_role(standard_cigar_operation{}.assign_rank(static_cast<standard_rank>(rank))) == cigar_role::opaque)
            return false;
    for (std::size_t index = 0; index < cigar_extensions.size(); ++index) {
        const auto& extension = cigar_extensions[index];
        const auto character = static_cast<unsigned char>(extension.character);
        if (character < 33 || character > 126 || (character >= '0' && character <= '9') ||
            character == '*' || character == '+' || character == '-' ||
            seqan3::char_is_valid_for<standard_cigar_operation>(extension.character)) return false;
        if (extension.native_opcode && (*extension.native_opcode > 15 || *extension.native_opcode < standard_cigar_size)) return false;
        for (std::size_t earlier = 0; earlier < index; ++earlier)
            if (cigar_extensions[earlier].character == extension.character ||
                (extension.native_opcode && extension.native_opcode == cigar_extensions[earlier].native_opcode)) return false;
    }
    return true;
}
}
static_assert(cigar_alphabet_detail::definitions_valid(), "invalid CIGAR alphabet extension or uncovered standard role");
static_assert(sizeof(cigar_symbol) == sizeof(seqan3::alphabet_rank_t<cigar_symbol>));
static_assert(seqan3::alphabet_size<cigar_symbol> <= std::numeric_limits<std::uint8_t>::max() + std::size_t{1});
static_assert(std::is_trivially_copyable_v<cigar_run>);
}
