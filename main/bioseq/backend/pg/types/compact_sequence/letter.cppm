/* Fixed-width self-describing bioseq.letter Datum and its SQL semantics. */
module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "headers/pg_compat.h"
extern "C" {
#include "common/hashfn.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wregister"
#include "libpq/pqformat.h"
#pragma GCC diagnostic pop
#include "utils/builtins.h"
}

export module bioseq.types.pg_compact_sequence:letter;

import :flat_abi;
import :io;

export struct pg_letter {
    std::uint8_t alphabet_code;
    std::uint8_t rank;
};

static_assert(sizeof(pg_letter) == 2u);
static_assert(std::is_trivially_copyable_v<pg_letter>);

export [[nodiscard]] inline Datum make_letter_datum(
    std::uint8_t alphabet_code,
    std::uint8_t rank) noexcept
{
    const std::uint16_t bits =
        (std::uint16_t{alphabet_code} << 8u) | std::uint16_t{rank};
    return UInt16GetDatum(bits);
}

export [[nodiscard]] inline pg_letter datum_get_letter(Datum datum) noexcept
{
    const std::uint16_t bits = DatumGetUInt16(datum);
    return {
        .alphabet_code = static_cast<std::uint8_t>(bits >> 8u),
        .rank = static_cast<std::uint8_t>(bits & 0xffu),
    };
}

/*
 * PostgreSQL coerces an unknown SQL literal to the unmodified base type before
 * invoking the same-type typmod cast.  Consequently letter_in sees typmod -1
 * even for `'A'::bioseq.letter('dna4')`.  This token is a parser-only bridge:
 * the typmod cast must consume it, and every ordinary letter operation rejects
 * it.  A physical letter produced by sequence element access never uses this
 * code and remains valid without an expression typmod.
 */
inline constexpr std::uint8_t unresolved_letter_alphabet_code =
    pgseq_unbound_alphabet_code;

[[noreturn]] inline void unresolved_letter_input_error()
{
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("bioseq.letter input requires an alphabet typmod"),
             errhint("cast as bioseq.letter('dna4') or another supported alphabet")));
    __builtin_unreachable();
}

export [[nodiscard]] inline int32_t encode_letter_typmod(
    std::uint8_t alphabet_code)
{
    if (!is_supported_alphabet_code(alphabet_code)) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("invalid bioseq.letter alphabet code %u",
                        static_cast<unsigned>(alphabet_code))));
    }
    return static_cast<int32_t>(alphabet_code) + 1;
}

export [[nodiscard]] inline std::uint8_t decode_letter_typmod_checked(
    int32_t typmod)
{
    if (typmod <= 0
        || typmod > static_cast<int32_t>(
            pgseq_supported_alphabets::alphabet_count)) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("invalid bioseq.letter typmod %d", typmod)));
    }
    return static_cast<std::uint8_t>(typmod - 1);
}

export [[nodiscard]] inline int32_t letter_typmod_from_seq_typmod(
    int32_t seq_typmod)
{
    if (seq_typmod < 0) {
        return -1;
    }
    return encode_letter_typmod(
        decode_seq_typmod_checked(seq_typmod).alphabet_code);
}

export [[nodiscard]] inline Oid bioseq_letter_oid()
{
    const Oid oid = bioseq_letter_type_oid_missing_ok();
    if (!OidIsValid(oid)) {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("bioseq.letter type is not installed")));
    }
    return oid;
}

export [[nodiscard]] inline pg_letter validate_letter_datum(Datum datum)
{
    const pg_letter value = datum_get_letter(datum);
    if (value.alphabet_code == unresolved_letter_alphabet_code) {
        unresolved_letter_input_error();
    }
    char symbol = '\0';
    if (!is_supported_alphabet_code(value.alphabet_code)
        || !pgseq_symbol_for_rank(
            value.alphabet_code, value.rank, symbol)) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("invalid physical bioseq.letter value"),
                 errdetail("alphabet code=%u, rank=%u",
                           static_cast<unsigned>(value.alphabet_code),
                           static_cast<unsigned>(value.rank))));
    }
    return value;
}

namespace {

[[nodiscard]] inline std::uint16_t letter_key(pg_letter value) noexcept
{
    return static_cast<std::uint16_t>(
        (std::uint16_t{value.alphabet_code} << 8u)
        | std::uint16_t{value.rank});
}

[[nodiscard]] inline int compare_letters(Datum left, Datum right)
{
    const auto left_value = validate_letter_datum(left);
    const auto right_value = validate_letter_datum(right);
    const std::uint16_t left_key = letter_key(left_value);
    const std::uint16_t right_key = letter_key(right_value);
    return left_key < right_key ? -1 : left_key > right_key ? 1 : 0;
}

[[nodiscard]] inline std::array<unsigned char, 2>
letter_hash_bytes(Datum datum)
{
    const auto value = validate_letter_datum(datum);
    return {value.alphabet_code, value.rank};
}

[[nodiscard]] inline std::uint8_t rank_for_letter_symbol_checked(
    std::uint8_t alphabet,
    unsigned char symbol)
{
    std::uint8_t rank = 0u;
    if (!pgseq_rank_for_symbol(alphabet, symbol, rank)) {
        const auto name = pgseq_supported_alphabets::name_of_alphabet_code(
            alphabet);
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("invalid symbol for bioseq.letter('%.*s')",
                        static_cast<int>(name.size()), name.data())));
    }
    return rank;
}

} // namespace

export inline Datum letter_typmod_in_impl(FunctionCallInfo fcinfo)
{
    ArrayType* array = PG_GETARG_ARRAYTYPE_P(0);
    if (ARR_NDIM(array) != 1) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bioseq.letter typmod must be one-dimensional")));
    }
    Datum* values = nullptr;
    bool* nulls = nullptr;
    int count = 0;
    deconstruct_array(
        array, CSTRINGOID, -2, false, 'c', &values, &nulls, &count);
    if (count != 1 || (nulls != nullptr && nulls[0])) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bioseq.letter takes exactly one alphabet modifier"),
                 errhint("Example: bioseq.letter('dna4')")));
    }
    const auto alphabet = parse_alphabet_name_checked(
        DatumGetCString(values[0]), "bioseq.letter");
    PG_RETURN_INT32(encode_letter_typmod(alphabet));
}

export inline Datum letter_typmod_out_impl(FunctionCallInfo fcinfo)
{
    const int32_t typmod = PG_GETARG_INT32(0);
    if (typmod < 0) {
        PG_RETURN_CSTRING(pstrdup(""));
    }
    const auto alphabet = decode_letter_typmod_checked(typmod);
    const auto name = pgseq_supported_alphabets::name_of_alphabet_code(
        alphabet);
    auto* result = static_cast<char*>(palloc(name.size() + 5u));
    std::snprintf(result, name.size() + 5u, "('%.*s')",
                  static_cast<int>(name.size()), name.data());
    PG_RETURN_CSTRING(result);
}

export inline Datum letter_in_impl(FunctionCallInfo fcinfo)
{
    const char* input = PG_GETARG_CSTRING(0);
    const int32_t typmod = PG_GETARG_INT32(2);
    if (std::strlen(input) != 1u) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("bioseq.letter input must contain exactly one symbol")));
    }
    if (typmod < 0) {
        PG_RETURN_DATUM(make_letter_datum(
            unresolved_letter_alphabet_code,
            static_cast<std::uint8_t>(
                static_cast<unsigned char>(input[0]))));
    }
    const auto alphabet = decode_letter_typmod_checked(typmod);
    const auto rank = rank_for_letter_symbol_checked(
        alphabet, static_cast<unsigned char>(input[0]));
    PG_RETURN_DATUM(make_letter_datum(alphabet, rank));
}

export inline Datum letter_out_impl(FunctionCallInfo fcinfo)
{
    const auto value = validate_letter_datum(PG_GETARG_DATUM(0));
    char symbol = '\0';
    (void) pgseq_symbol_for_rank(value.alphabet_code, value.rank, symbol);
    auto* result = static_cast<char*>(palloc(2u));
    result[0] = symbol;
    result[1] = '\0';
    PG_RETURN_CSTRING(result);
}

export inline Datum letter_recv_impl(FunctionCallInfo fcinfo)
{
    auto* buffer = reinterpret_cast<StringInfo>(PG_GETARG_POINTER(0));
    const int32_t typmod = PG_GETARG_INT32(2);
    const auto version = static_cast<std::uint8_t>(pq_getmsgint(buffer, 1));
    const auto alphabet = static_cast<std::uint8_t>(pq_getmsgint(buffer, 1));
    const auto rank = static_cast<std::uint8_t>(pq_getmsgint(buffer, 1));
    pq_getmsgend(buffer);
    if (alphabet == unresolved_letter_alphabet_code) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                 errmsg("unresolved bioseq.letter input is not a wire value")));
    }
    const Datum result = make_letter_datum(alphabet, rank);
    (void) validate_letter_datum(result);
    if (version != 1u) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                 errmsg("unsupported bioseq.letter wire version %u",
                        static_cast<unsigned>(version))));
    }
    if (typmod >= 0
        && decode_letter_typmod_checked(typmod) != alphabet) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("bioseq.letter wire alphabet does not match destination typmod")));
    }
    PG_RETURN_DATUM(result);
}

export inline Datum letter_send_impl(FunctionCallInfo fcinfo)
{
    const auto value = validate_letter_datum(PG_GETARG_DATUM(0));
    StringInfoData buffer;
    pq_begintypsend(&buffer);
    pq_sendint8(&buffer, 1u);
    pq_sendint8(&buffer, value.alphabet_code);
    pq_sendint8(&buffer, value.rank);
    PG_RETURN_BYTEA_P(pq_endtypsend(&buffer));
}

export inline Datum letter_typmod_cast_impl(FunctionCallInfo fcinfo)
{
    const Datum input = PG_GETARG_DATUM(0);
    const int32_t target_typmod = PG_GETARG_INT32(1);
    const auto raw = datum_get_letter(input);
    if (raw.alphabet_code == unresolved_letter_alphabet_code) {
        if (target_typmod < 0) {
            unresolved_letter_input_error();
        }
        const auto target_alphabet =
            decode_letter_typmod_checked(target_typmod);
        const auto rank = rank_for_letter_symbol_checked(
            target_alphabet, raw.rank);
        PG_RETURN_DATUM(make_letter_datum(target_alphabet, rank));
    }
    const auto value = validate_letter_datum(input);
    if (target_typmod >= 0
        && decode_letter_typmod_checked(target_typmod)
            != value.alphabet_code) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("cannot relabel bioseq.letter to a different alphabet")));
    }
    PG_RETURN_DATUM(input);
}

export inline Datum letter_to_text_impl(FunctionCallInfo fcinfo)
{
    const auto value = validate_letter_datum(PG_GETARG_DATUM(0));
    char symbol = '\0';
    (void) pgseq_symbol_for_rank(value.alphabet_code, value.rank, symbol);
    PG_RETURN_TEXT_P(cstring_to_text_with_len(&symbol, 1));
}

export inline Datum letter_cmp_impl(FunctionCallInfo fcinfo)
{
    PG_RETURN_INT32(compare_letters(
        PG_GETARG_DATUM(0), PG_GETARG_DATUM(1)));
}

export inline Datum letter_eq_impl(FunctionCallInfo fcinfo)
{
    PG_RETURN_BOOL(compare_letters(
        PG_GETARG_DATUM(0), PG_GETARG_DATUM(1)) == 0);
}

export inline Datum letter_ne_impl(FunctionCallInfo fcinfo)
{
    PG_RETURN_BOOL(compare_letters(
        PG_GETARG_DATUM(0), PG_GETARG_DATUM(1)) != 0);
}

export inline Datum letter_lt_impl(FunctionCallInfo fcinfo)
{
    PG_RETURN_BOOL(compare_letters(
        PG_GETARG_DATUM(0), PG_GETARG_DATUM(1)) < 0);
}

export inline Datum letter_le_impl(FunctionCallInfo fcinfo)
{
    PG_RETURN_BOOL(compare_letters(
        PG_GETARG_DATUM(0), PG_GETARG_DATUM(1)) <= 0);
}

export inline Datum letter_gt_impl(FunctionCallInfo fcinfo)
{
    PG_RETURN_BOOL(compare_letters(
        PG_GETARG_DATUM(0), PG_GETARG_DATUM(1)) > 0);
}

export inline Datum letter_ge_impl(FunctionCallInfo fcinfo)
{
    PG_RETURN_BOOL(compare_letters(
        PG_GETARG_DATUM(0), PG_GETARG_DATUM(1)) >= 0);
}

export inline Datum letter_hash_impl(FunctionCallInfo fcinfo)
{
    const auto bytes = letter_hash_bytes(PG_GETARG_DATUM(0));
    PG_RETURN_INT32(static_cast<std::int32_t>(hash_bytes(
        bytes.data(), bytes.size())));
}

export inline Datum letter_hash_extended_impl(FunctionCallInfo fcinfo)
{
    const auto bytes = letter_hash_bytes(PG_GETARG_DATUM(0));
    const auto seed = static_cast<std::uint64_t>(PG_GETARG_INT64(1));
    PG_RETURN_INT64(static_cast<std::int64_t>(hash_bytes_extended(
        bytes.data(), bytes.size(), seed)));
}
