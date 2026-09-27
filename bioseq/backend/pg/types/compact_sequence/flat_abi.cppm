/**
 * @file flat_abi.cppm
 * Define the flat PostgreSQL sequence type, its format tags, and alphabet helpers.
 */
module;

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <string_view>

#include <bioseq/pg_compat.h>

extern "C" {
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "utils/syscache.h"
}

export module bioseq.postgres.types.compact_sequence:flat_abi;

import bioseq.common.seqan_adapter;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.support.sql_boundary;
import bioseq.postgres.types.compact_sequence.dispatch_profile_contract;
import bioseq.sequences;
import bioseq.storage.layout;

/// Alphabets enabled for sequence operations in this build.
export using pgseq_supported_alphabets =
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    supported_alphabets<alphabet_entry<seq::dna4, "dna4">>;
#else
    supported_alphabets<
        alphabet_entry<seq::dna4, "dna4">,
        alphabet_entry<seq::dna5, "dna5">>;
#endif

/// Storage-unit widths, in bytes, enabled in this build.
export using pgseq_supported_storage_unit_byte_widths =
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES)
    supported_storage_unit_byte_widths<8u>;
#else
    all_storage_unit_byte_widths;
#endif

static_assert(pgseq_supported_alphabets::alphabet_count
              == pgseq_dispatch_profile_alphabet_count);
static_assert([] consteval {
    for (unsigned value = 0u; value <= 0xffu; ++value) {
        const auto byte_width = static_cast<std::uint8_t>(value);
        if (pgseq_supported_storage_unit_byte_widths::contains_runtime(byte_width)
            != pgseq_dispatch_profile_contains_storage_unit_byte_width(byte_width))
            return false;
    }
    return true;
}(), "compact-sequence dispatch-profile width contract drifted");

export namespace bioseq::pg_types {

/**
 * @brief PostgreSQL sequence value with packed letters and an alphabet tag.
 * PostgreSQL can compress or move large values into TOAST storage.
 * @ingroup bioseq_sql_public_api
 */
using seq [[bioseq_sql::type({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .internal_length = bioseq::sql::variable_length,
    .alignment = bioseq::sql::type_alignment::double_,
    .storage = bioseq::sql::type_storage::extended,
})]] = compact_sequence_stdc<
    pgseq_supported_alphabets,
    std::uint32_t,
    std::int32_t,
    pgseq_supported_storage_unit_byte_widths>;

}  // namespace bioseq::pg_types

export namespace bioseq::postgres::support::detail {

/// @brief Pass a sequence pointer through PostgreSQL's Datum value.
template<>
struct scalar_codec<bioseq::pg_types::seq*> final {
    static constexpr const char* key = "bioseq.seq/opaque_pointer";

    /// @brief Read the sequence pointer stored in a Datum.
    /// @param[in] value Datum containing a sequence pointer.
    /// @return A borrowed pointer valid while the original sequence is alive.
    [[nodiscard]] static bioseq::pg_types::seq* read(
        Datum value) noexcept {
        return reinterpret_cast<bioseq::pg_types::seq*>(
            DatumGetPointer(value));
    }

    /// @brief Encodes a sequence pointer as a Datum without changing ownership.
    /// @param[in] value Borrowed sequence pointer.
    /// @return Pointer Datum referring to the same value.
    [[nodiscard]] static Datum write(
        bioseq::pg_types::seq* value) noexcept {
        return PointerGetDatum(value);
    }
};

}  // namespace bioseq::postgres::support::detail

/// Alphabet tag for text whose sequence alphabet has not been chosen yet.
export inline constexpr std::uint8_t pgseq_unbound_alphabet_code = 0xffu;
/// Width tag identifying unbound text instead of packed letters.
export inline constexpr std::uint8_t pgseq_unbound_width_tag = 0x40u;
/// Mask for the storage-unit width bits of a bound sequence.
export inline constexpr std::uint8_t pgseq_physical_width_mask = 0x0fu;

static_assert((pgseq_unbound_width_tag
               & bioseq::storage::sequence_locator_stub_width_marker) == 0u);
static_assert((pgseq_unbound_width_tag & pgseq_physical_width_mask) == 0u);
static_assert((bioseq::storage::sequence_locator_stub_width_marker
               & pgseq_physical_width_mask) == 0u);
static_assert(pgseq_unbound_alphabet_code
              >= pgseq_supported_alphabets::alphabet_count);

static_assert(pgseq_supported_alphabets::valid_alphabet_names_csv()
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
              == std::string_view{"'dna4'"},
#else
              == std::string_view{"'dna4', 'dna5'"},
#endif
              "supported-alphabet display names drifted");
static_assert(
    pgseq_supported_alphabets::entry_for<seq::dna4>::alphabet_name_sv
        == std::string_view{"dna4"});
#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
static_assert(
    pgseq_supported_alphabets::entry_for<seq::dna5>::alphabet_name_sv
        == std::string_view{"dna5"});
#endif

/// @brief Check whether this build supports an alphabet code.
/// @param[in] code Alphabet code to check.
/// @return true when the code is in the compiled alphabet list.
export [[nodiscard]] constexpr bool
is_supported_alphabet_code(std::uint8_t code) noexcept
{
    return code < pgseq_supported_alphabets::alphabet_count;
}

/// @brief Choose the storage width used by this build.
/// @param[in] requested Storage-unit width in bytes.
/// @return Eight bytes in a build limited to uint64 storage, otherwise requested.
export [[nodiscard]] constexpr std::uint8_t
pgseq_normalize_storage_unit_byte_width(std::uint8_t requested) noexcept
{
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES)
    (void) requested;
    return 8u;
#else
    return requested;
#endif
}

/// @brief Adjust the packing width to the widths enabled in this build.
/// @param[in] params Requested alphabet and storage width.
/// @return The same alphabet with the build's selected width.
export [[nodiscard]] constexpr compact_sequence_stdc_type_params
pgseq_normalize_type_params(compact_sequence_stdc_type_params params) noexcept
{
    params.storage_unit_byte_width =
        pgseq_normalize_storage_unit_byte_width(
            params.storage_unit_byte_width);
    return params;
}

/// Format identified by a flat sequence's header tags.
export enum class pg_sequence_flat_kind : std::uint8_t {
    bound,
    unbound,
    locator,
    invalid,
};

/* The constructor is defined in :io, which already imports these type
 * definitions. Declaring it here lets shared helpers call it without making
 * these modules import each other. */
/// @brief Copy ASCII letters into a sequence whose alphabet is still unset.
/// @param[in] input Letters to validate and copy into PostgreSQL memory.
/// @param[in] length Number of input symbols.
/// @param[in] require_already_canonical Reject input that needs case or spelling normalization.
/// @return A newly allocated unbound sequence.
export [[nodiscard]] Datum make_unbound_sequence_from_ascii(
    char const* input,
    std::size_t length,
    bool require_already_canonical = false);

/// @brief Identify a flat sequence's format from its header tags.
/// @param[in] value Flat sequence pointer, or nullptr. The value must already be detoasted.
/// @return Its format, or invalid for a null pointer or inconsistent tags.
/// This checks tags only. It does not validate the payload or allocate memory.
export [[nodiscard]] inline pg_sequence_flat_kind
classify_pg_sequence_flat(bioseq::pg_types::seq const* value) noexcept
{
    if (value == nullptr) {
        return pg_sequence_flat_kind::invalid;
    }
    const auto width =
        static_cast<std::uint8_t>(value->storage_unit_byte_width);
    if (bioseq::storage::is_sequence_locator_stub_width(width)) {
        return pg_sequence_flat_kind::locator;
    }
    if (value->alphabet_code == pgseq_unbound_alphabet_code
        || width == pgseq_unbound_width_tag) {
        return value->alphabet_code == pgseq_unbound_alphabet_code
                && width == pgseq_unbound_width_tag
            ? pg_sequence_flat_kind::unbound
            : pg_sequence_flat_kind::invalid;
    }
    if (!is_supported_alphabet_code(
            static_cast<std::uint8_t>(value->alphabet_code))
        || !pgseq_supported_storage_unit_byte_widths::contains_runtime(width)) {
        return pg_sequence_flat_kind::invalid;
    }
    return pg_sequence_flat_kind::bound;
}

/// @brief Borrow the ASCII letters of an unbound sequence.
/// @param[in] value Unbound flat sequence.
/// @return A read-only span valid while value is alive.
export [[nodiscard]] inline std::span<std::byte const>
pgseq_unbound_ascii(bioseq::pg_types::seq const& value) noexcept
{
    return {
        reinterpret_cast<std::byte const*>(value.data),
        static_cast<std::size_t>(value.bp_size),
    };
}

/// @brief Borrow writable ASCII letters of an unbound sequence.
/// @param[in,out] value Unbound flat sequence.
/// @return A writable span valid while value is alive.
export [[nodiscard]] inline std::span<std::byte>
pgseq_unbound_ascii(bioseq::pg_types::seq& value) noexcept
{
    return {
        reinterpret_cast<std::byte*>(value.data),
        static_cast<std::size_t>(value.bp_size),
    };
}

/// Whether a symbol was accepted and its normalized spelling.
export struct pgseq_canonical_symbol {
    bool valid;
    char value;
};

/* Validate a character before assigning it to a SeqAn letter. Otherwise
 * SeqAn can map invalid input to rank zero, hiding the input error. These
 * checks are shared by sequence text, letter input, and array conversions. */
/// @brief Validate a symbol and return its alphabet's normalized spelling.
/// @param[in] alphabet_code Alphabet to use.
/// @param[in] input ASCII symbol.
/// @return The normalized character and whether it was accepted.
export [[nodiscard]] inline pgseq_canonical_symbol
pgseq_canonical_symbol_for_alphabet(
    std::uint8_t alphabet_code,
    unsigned char input) noexcept
{
    if (!is_supported_alphabet_code(alphabet_code)) {
        return {false, '\0'};
    }
    pgseq_canonical_symbol result{false, '\0'};
    pgseq_supported_alphabets::dispatch_alphabet_by_code(
        alphabet_code,
        [&result, input]<class p_alphabet>() {
            const char symbol = static_cast<char>(input);
            if (!seqan3::char_is_valid_for<p_alphabet>(symbol)) {
                return;
            }
            p_alphabet value{};
            value.assign_char(symbol);
            result = {true, seqan3::to_char(value)};
        });
    return result;
}

/// @brief Normalize a symbol using the alphabets that accept it.
/// @param[in] input ASCII symbol.
/// @return Their agreed spelling, or invalid if none accept it or they disagree.
export [[nodiscard]] inline pgseq_canonical_symbol
pgseq_canonical_unbound_symbol(unsigned char input) noexcept
{
    pgseq_canonical_symbol result{false, '\0'};
    for (std::uint8_t code = 0;
         code < pgseq_supported_alphabets::alphabet_count;
         ++code) {
        const auto candidate =
            pgseq_canonical_symbol_for_alphabet(code, input);
        if (!candidate.valid) {
            continue;
        }
        if (result.valid && result.value != candidate.value) {
            return {false, '\0'};
        }
        result = candidate;
    }
    return result;
}

/// @brief Convert a valid symbol to its alphabet rank.
/// @param[in] alphabet_code Alphabet to use.
/// @param[in] input ASCII symbol.
/// @param[out] rank Rank written on success.
/// @param[out] canonical Optional normalized spelling written on success.
/// @return true when the alphabet accepts the symbol.
export [[nodiscard]] inline bool pgseq_rank_for_symbol(
    std::uint8_t alphabet_code,
    unsigned char input,
    std::uint8_t& rank,
    char* canonical = nullptr) noexcept
{
    if (!is_supported_alphabet_code(alphabet_code)) {
        return false;
    }
    bool valid = false;
    pgseq_supported_alphabets::dispatch_alphabet_by_code(
        alphabet_code,
        [input, canonical, &rank, &valid]<class p_alphabet>() {
            const char symbol = static_cast<char>(input);
            if (!seqan3::char_is_valid_for<p_alphabet>(symbol)) {
                return;
            }
            p_alphabet value{};
            value.assign_char(symbol);
            rank = static_cast<std::uint8_t>(seqan3::to_rank(value));
            if (canonical != nullptr) {
                *canonical = seqan3::to_char(value);
            }
            valid = true;
        });
    return valid;
}

/// @brief Convert a valid alphabet rank to its letter.
/// @param[in] alphabet_code Alphabet to use.
/// @param[in] rank Rank to convert.
/// @param[out] symbol Normalized character written on success.
/// @return true when the alphabet and rank are valid.
export [[nodiscard]] inline bool pgseq_symbol_for_rank(
    std::uint8_t alphabet_code,
    std::uint8_t rank,
    char& symbol) noexcept
{
    if (!is_supported_alphabet_code(alphabet_code)) {
        return false;
    }
    bool valid = false;
    pgseq_supported_alphabets::dispatch_alphabet_by_code(
        alphabet_code,
        [rank, &symbol, &valid]<class p_alphabet>() {
            if (rank >= seqan3::alphabet_size<p_alphabet>) {
                return;
            }
            p_alphabet value{};
            seqan3::assign_rank_to(rank, value);
            symbol = seqan3::to_char(value);
            valid = true;
        });
    return valid;
}

/// Allocate zeroed sequence storage, returning nullptr if it is too large or allocation fails.
export inline constexpr auto pg_alloc =
    [](std::size_t byte_count) -> bioseq::pg_types::seq* {
        if (byte_count > static_cast<std::size_t>(MaxAllocSize)) {
            return nullptr;
        }
        return static_cast<bioseq::pg_types::seq*>(
            palloc_extended(byte_count,
                            MCXT_ALLOC_NO_OOM | MCXT_ALLOC_ZERO));
    };

static_assert(stdc_allocator<decltype(pg_alloc), bioseq::pg_types::seq>);

/* Look up current type OIDs each time. Dropping and recreating the extension
 * in one backend gives its types new OIDs, and a missing type is allowed. */
/// @brief Look up a type in the bioseq schema without caching its OID.
/// @param[in] type_name Null-terminated type name.
/// @return The current OID, or InvalidOid when the schema or type is absent.
export [[nodiscard]] inline Oid bioseq_type_oid_missing_ok(
    char const* type_name) noexcept
{
    const Oid namespace_oid =
        get_namespace_oid("bioseq", /* missing_ok = */ true);
    if (!OidIsValid(namespace_oid)) {
        return InvalidOid;
    }
    return GetSysCacheOid2(
        TYPENAMENSP,
        Anum_pg_type_oid,
        PointerGetDatum(type_name),
        ObjectIdGetDatum(namespace_oid));
}

/// @brief Look up the current bioseq.seq type OID.
/// @return Its OID, or InvalidOid if the type is absent.
export [[nodiscard]] inline Oid bioseq_seq_type_oid_missing_ok() noexcept
{
    return bioseq_type_oid_missing_ok("seq");
}

/// @brief Look up the current bioseq.letter type OID.
/// @return Its OID, or InvalidOid if the type is absent.
export [[nodiscard]] inline Oid bioseq_letter_type_oid_missing_ok() noexcept
{
    return bioseq_type_oid_missing_ok("letter");
}
