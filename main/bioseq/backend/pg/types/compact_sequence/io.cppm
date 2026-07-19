/* Text/binary/typmod I/O and the exhaustive seq self-cast matrix. */
module;

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <variant>

#include "headers/pg_build_config.h"
#include "headers/pg_compat.h"
extern "C" {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wregister"
#include "libpq/pqformat.h"
#pragma GCC diagnostic pop
#include "utils/expandeddatum.h"
}

export module bioseq.types.pg_compact_sequence:io;

import :flat_abi;
import :tree_types;
import :tree_ops;
import :view_session;
import :packed_output;
import bioseq.common.mlogging;
import bioseq.sequences.simd;
import bioseq.storage.coordinates;
import bioseq.storage.lazy;

export [[nodiscard]] inline compact_sequence_stdc_type_params
decode_seq_typmod_checked(int32_t typmod)
{
    if (typmod < 0) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bioseq.seq destination typmod is required")));
    }
    auto params = pgseq_normalize_type_params(
        compact_sequence_stdc_type_params::decode(typmod));
    if (!is_supported_alphabet_code(params.alphabet_code)
        || !pgseq_supported_storage_unit_byte_widths::contains_runtime(
            params.storage_unit_byte_width)) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("invalid bioseq.seq typmod %d", typmod)));
    }
    return params;
}

export [[nodiscard]] inline std::uint8_t parse_alphabet_name_checked(
    char const* alphabet_name,
    char const* type_name)
{
    const auto code =
        pgseq_supported_alphabets::try_index_of_name(alphabet_name);
    if (!code.has_value()) {
        constexpr auto valid =
            pgseq_supported_alphabets::valid_alphabet_names_csv();
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s alphabet \"%s\" is not supported",
                        type_name, alphabet_name),
                 errhint("Valid alphabets: %.*s",
                         static_cast<int>(valid.size()), valid.data())));
    }
    return static_cast<std::uint8_t>(*code);
}

namespace {

[[nodiscard]] inline pgseq_t* allocate_unbound_ascii(
    char const* input,
    std::size_t length,
    bool require_already_canonical)
{
    if (length > std::numeric_limits<pgseq_t::bp_size_t>::max()) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq.seq input length %zu exceeds uint32", length)));
    }
    if (length > std::numeric_limits<std::size_t>::max() - 7u) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq.seq input size overflows size_t")));
    }
    const std::size_t total = pgseq_t::header_bytes()
        + round_up_to_uint64(length);
    if (total > MaxAllocSize) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq.seq input length %zu exceeds MaxAllocSize",
                        length)));
    }
    pgseq_t* result = pg_alloc(total);
    if (result == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_OUT_OF_MEMORY),
                 errmsg("out of memory constructing unbound bioseq.seq")));
    }
    SET_VARSIZE(result, total);
    result->alphabet_code = pgseq_unbound_alphabet_code;
    result->storage_unit_byte_width = pgseq_unbound_width_tag;
    result->padding_letters = 0u;
    result->bp_size = static_cast<pgseq_t::bp_size_t>(length);
    for (std::size_t index = 0; index < length; ++index) {
        const unsigned char raw = static_cast<unsigned char>(input[index]);
        const auto canonical = pgseq_canonical_unbound_symbol(raw);
        if (!canonical.valid
            || (require_already_canonical
                && static_cast<unsigned char>(canonical.value) != raw)) {
            pfree(result);
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid symbol at byte %zu in unbound bioseq.seq",
                            index),
                     errdetail("byte value=%u", static_cast<unsigned>(raw))));
        }
        result->data[index] = static_cast<std::uint8_t>(canonical.value);
    }
    return result;
}

[[nodiscard]] inline pgseq_t* pack_ascii_for_target(
    char const* ascii,
    std::size_t length,
    compact_sequence_stdc_type_params target)
{
    for (std::size_t index = 0; index < length; ++index) {
        std::uint8_t rank = 0;
        if (!pgseq_rank_for_symbol(
                target.alphabet_code,
                static_cast<unsigned char>(ascii[index]), rank)) {
            const auto alphabet =
                pgseq_supported_alphabets::name_of_alphabet_code(
                    target.alphabet_code);
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid symbol '%c' for bioseq alphabet %.*s",
                            ascii[index], static_cast<int>(alphabet.size()),
                            alphabet.data()),
                     errdetail("symbol offset=%zu", index)));
        }
    }
    if (length > std::numeric_limits<pgseq_t::bp_size_t>::max()) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq sequence is too long to pack")));
    }
    pgseq_t::dispatch_instance(
        target.alphabet_code, target.storage_unit_byte_width,
        [length]<class Instance>() {
            (void) bound_flat_size<Instance>(length);
        });
    pgseq_t* result = nullptr;
    pgseq_supported_alphabets::dispatch_alphabet_by_code(
        target.alphabet_code,
        [&result, target, length, ascii]<class Alphabet>() {
            result = ::bioseq::simd::active_target::
                stdc_convert_from_cstr<pgseq_t, Alphabet>(
                    target.storage_unit_byte_width,
                    static_cast<pgseq_t::bp_size_t>(length),
                    ascii,
                    pg_alloc);
        });
    if (result == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_OUT_OF_MEMORY),
                 errmsg("out of memory packing bioseq sequence")));
    }
    return result;
}

[[nodiscard]] inline pgseq_t* flatten_sequence_datum(
    Datum datum,
    bool& must_free)
{
    if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(datum))) {
        ExpandedObjectHeader* header = DatumGetEOHP(datum);
        const Size size = EOH_get_flat_size(header);
        auto* result = static_cast<pgseq_t*>(palloc0(size));
        EOH_flatten_into(header, result, size);
        must_free = true;
        return result;
    }
    auto* result = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(datum));
    must_free = reinterpret_cast<Pointer>(result) != DatumGetPointer(datum);
    return result;
}

[[nodiscard]] inline constexpr auto pg_recv_byte_reader =
    []<class Alphabet, class Out>(
        StringInfoData& buffer,
        std::uint8_t width) -> Out {
        switch (width) {
            case 1u: return static_cast<Out>(pq_getmsgint(&buffer, 1));
            case 2u: return static_cast<Out>(pq_getmsgint(&buffer, 2));
            case 4u: return static_cast<Out>(pq_getmsgint(&buffer, 4));
            case 8u: return static_cast<Out>(pq_getmsgint64(&buffer));
        }
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                 errmsg("invalid bioseq wire storage width %u", width)));
        __builtin_unreachable();
    };

[[nodiscard]] inline constexpr auto pg_send_byte_writer =
    []<class In>(StringInfoData& buffer,
                 std::uint64_t unit,
                 std::uint8_t width) {
        switch (width) {
            case 1u: pq_sendint8(&buffer, static_cast<std::uint8_t>(unit)); return;
            case 2u: pq_sendint16(&buffer, static_cast<std::uint16_t>(unit)); return;
            case 4u: pq_sendint32(&buffer, static_cast<std::uint32_t>(unit)); return;
            case 8u: pq_sendint64(&buffer, unit); return;
        }
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                 errmsg("invalid bioseq wire storage width %u", width)));
    };

template<class Alphabet>
void view_to_ascii(
    bioseq::storage::basic_lazy_sequence_view<Alphabet> view,
    char* output)
{
    std::array<std::uint8_t, 256> scratch{};
    std::uint64_t position = 0u;
    while (position < view.bp_size()) {
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(scratch.size(), view.bp_size() - position));
        view.template to_encoded_into<false, false>(
            std::span<std::uint8_t>{scratch.data(), count},
            static_cast<std::size_t>(position),
            static_cast<std::size_t>(position + count));
        for (std::size_t index = 0; index < count; ++index) {
            char symbol = '\0';
            if (!pgseq_symbol_for_rank(
                    pgseq_supported_alphabets::template index_of<Alphabet>(),
                    scratch[index], symbol)) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq view emitted an invalid rank")));
            }
            output[position + index] = symbol;
        }
        position += count;
    }
}

} // namespace

/* Owned parser-safe literals for concat/array bridges.  The returned Datum is
 * a self-contained flat value and is therefore safe in Const nodes as well as
 * across calls that immediately promote it into a piece tree. */
export [[nodiscard]] inline Datum make_unbound_sequence_from_ascii(
    char const* input,
    std::size_t length,
    bool require_already_canonical = false)
{
    return PointerGetDatum(allocate_unbound_ascii(
        input, length, require_already_canonical));
}

export [[nodiscard]] inline Datum make_bound_sequence_from_ascii(
    char const* input,
    std::size_t length,
    compact_sequence_stdc_type_params target)
{
    return PointerGetDatum(pack_ascii_for_target(input, length, target));
}

export inline Datum seq_typmod_in_impl(FunctionCallInfo fcinfo)
{
    ArrayType* array = PG_GETARG_ARRAYTYPE_P(0);
    if (ARR_NDIM(array) != 1) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bioseq.seq typmod must be one-dimensional")));
    }
    if (ARR_HASNULL(array) && array_contains_nulls(array)) {
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bioseq.seq typmod arguments must not be NULL")));
    }
    Datum* values = nullptr;
    bool* nulls = nullptr;
    int count = 0;
    deconstruct_array(
        array, CSTRINGOID, -2, false, 'c', &values, &nulls, &count);
    if (count != 2) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bioseq.seq takes exactly 2 type modifiers; got %d",
                        count),
                 errhint("Example: bioseq.seq('dna4', 64)")));
    }
    const auto alphabet = parse_alphabet_name_checked(
        DatumGetCString(values[0]), "bioseq.seq");
    const char* width_text = DatumGetCString(values[1]);
    char* end = nullptr;
    errno = 0;
    const long width_bits = std::strtol(width_text, &end, 10);
    if (end == width_text || *end != '\0' || errno != 0
        || (width_bits != 8 && width_bits != 16
            && width_bits != 32 && width_bits != 64)) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bioseq.seq width \"%s\" is not supported", width_text),
                 errhint("Supported widths: 8, 16, 32, 64")));
    }
    const auto params = compact_sequence_stdc_type_params{
        .alphabet_code = alphabet,
        .storage_unit_byte_width = pgseq_normalize_storage_unit_byte_width(
            static_cast<std::uint8_t>(width_bits / 8)),
    };
    PG_RETURN_INT32(params.encode());
}

export inline Datum seq_typmod_out_impl(FunctionCallInfo fcinfo)
{
    const int32_t typmod = PG_GETARG_INT32(0);
    if (typmod < 0) {
        PG_RETURN_CSTRING(pstrdup(""));
    }
    const auto params = decode_seq_typmod_checked(typmod);
    const auto name = pgseq_supported_alphabets::name_of_alphabet_code(
        params.alphabet_code);
    auto* result = static_cast<char*>(palloc(64));
    std::snprintf(result, 64, "('%.*s',%u)",
                  static_cast<int>(name.size()), name.data(),
                  static_cast<unsigned>(params.storage_unit_byte_width) * 8u);
    PG_RETURN_CSTRING(result);
}

export inline Datum seq_in_impl(FunctionCallInfo fcinfo)
{
    const char* input = PG_GETARG_CSTRING(0);
    const int32_t typmod = PG_GETARG_INT32(2);
    const std::size_t length = std::strlen(input);
    if (typmod < 0) {
        PG_RETURN_POINTER(allocate_unbound_ascii(input, length, false));
    }
    const auto target = decode_seq_typmod_checked(typmod);
    if (length > std::numeric_limits<pgseq_t::bp_size_t>::max()) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq.seq input length %zu exceeds uint32", length)));
    }
    pgseq_t::dispatch_instance(
        target.alphabet_code, target.storage_unit_byte_width,
        [length]<class Instance>() {
            (void) bound_flat_size<Instance>(length);
        });
    pgseq_t* result = nullptr;
    pgseq_supported_alphabets::dispatch_alphabet_by_code(
        target.alphabet_code,
        [&result, target, length, input]<class Alphabet>() {
            result = ::bioseq::simd::active_target::
                stdc_convert_from_cstr<pgseq_t, Alphabet>(
                    target.storage_unit_byte_width,
                    static_cast<pgseq_t::bp_size_t>(length),
                    input,
                    pg_alloc);
        });
    if (result == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_OUT_OF_MEMORY),
                 errmsg("out of memory in bioseq.seq input")));
    }
    PG_RETURN_POINTER(result);
}

export inline Datum seq_out_impl(FunctionCallInfo fcinfo)
{
    Datum input = PG_GETARG_DATUM(0);
    Datum dispatch = input;
    pgseq_t* flat_copy = nullptr;
    if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(input))) {
        flat_copy = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(input));
        dispatch = PointerGetDatum(flat_copy);
    }
    const auto descriptor = classify_and_describe_sequence(dispatch);
    if (descriptor.kind == pg_sequence_datum_kind::locator) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cannot resolve a bioseq locator outside its owning table scan")));
    }
    auto* output = static_cast<char*>(
        palloc(checked_add_size(checked_size_cast(descriptor.bp_length), 1u)));
    if (!descriptor.alphabet_bound) {
        if (flat_copy != nullptr) {
            std::memcpy(output, flat_copy->data,
                        static_cast<std::size_t>(descriptor.bp_length));
        } else if (!emit_pg_unbound_ascii(
                       get_pg_sequence_expanded_ro(input)->root,
                       0u, descriptor.bp_length,
                       std::span<std::byte>{
                           reinterpret_cast<std::byte*>(output),
                           static_cast<std::size_t>(descriptor.bp_length)})) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("invalid unbound bioseq piece tree")));
        }
    } else {
        with_pg_sequence_view<std::monostate>(
            fcinfo, 0,
            [output](auto view) {
                view_to_ascii(view, output);
                return std::monostate{};
            });
    }
    output[descriptor.bp_length] = '\0';
    if (flat_copy != nullptr) {
        PG_FREE_IF_COPY(flat_copy, 0);
    }
    PG_RETURN_CSTRING(output);
}

export inline Datum seq_cast_impl(FunctionCallInfo fcinfo)
{
    Datum input = PG_GETARG_DATUM(0);
    const int32_t target_typmod = PG_GETARG_INT32(1);
    if (target_typmod < 0) {
        PG_RETURN_DATUM(input);
    }
    const auto target = decode_seq_typmod_checked(target_typmod);

    Datum dispatch = input;
    pgseq_t* flat = nullptr;
    if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(input))) {
        flat = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(input));
        dispatch = PointerGetDatum(flat);
    }
    const auto descriptor = classify_and_describe_sequence(dispatch);
    if (descriptor.kind == pg_sequence_datum_kind::locator) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cannot resolve a bioseq locator outside its owning table scan")));
    }

    if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(input))) {
        auto acquired = acquire_writable_pg_sequence(input);
        if (acquired.value->alphabet_bound
            && acquired.value->type_params.alphabet_code
                != target.alphabet_code) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("cannot cast bound bioseq.seq across alphabets")));
        }
        if (acquired.value->alphabet_bound
            && acquired.value->type_params == target) {
            PG_RETURN_DATUM(pg_sequence_rw_datum(acquired.value));
        }
        replace_root_error_safe(
            *acquired.value, target, acquired.value->bp_length, true,
            [&acquired, target](piece_tree_arena& arena) {
                return acquired.value->alphabet_bound
                    ? retain_edge(acquired.value->root)
                    : bind_literals_copy(arena, acquired.value->root, target);
            });
        PG_RETURN_DATUM(pg_sequence_rw_datum(acquired.value));
    }

    if (!descriptor.alphabet_bound) {
        pgseq_t* result = pack_ascii_for_target(
            reinterpret_cast<char const*>(flat->data),
            static_cast<std::size_t>(flat->bp_size), target);
        PG_FREE_IF_COPY(flat, 0);
        PG_RETURN_POINTER(result);
    }
    if (descriptor.alphabet_code != target.alphabet_code) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("cannot cast bound bioseq.seq across alphabets")));
    }
    if (flat->get_type_params() == target) {
        PG_RETURN_POINTER(flat);
    }
    pgseq_t* result = ::bioseq::simd::active_target::
        stdc_recode_to_type_params(*flat, target, pg_alloc);
    if (result == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_OUT_OF_MEMORY),
                 errmsg("out of memory in bioseq.seq cast")));
    }
    PG_FREE_IF_COPY(flat, 0);
    PG_RETURN_POINTER(result);
}

export inline Datum seq_send_impl(FunctionCallInfo fcinfo)
{
    bool must_free = false;
    pgseq_t* value = flatten_sequence_datum(
        PG_GETARG_DATUM(0), must_free);
    const auto descriptor = classify_and_describe_sequence(
        PointerGetDatum(value));
    if (descriptor.kind == pg_sequence_datum_kind::locator) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cannot send an unresolved bioseq locator")));
    }
    StringInfoData buffer;
    pq_begintypsend(&buffer);
    if (!descriptor.alphabet_bound) {
        pq_sendint8(&buffer, pgseq_unbound_alphabet_code);
        pq_sendint8(&buffer, 1u);
        pq_sendint8(&buffer, pgseq_unbound_width_tag);
        pq_sendint32(&buffer, value->bp_size);
        pq_sendbytes(&buffer, reinterpret_cast<char const*>(value->data),
                     value->bp_size);
    } else {
        const std::uint8_t width = value->storage_unit_byte_width;
        const std::uint8_t width_code = width == 1u ? 0u
            : width == 2u ? 1u : width == 4u ? 2u : 3u;
        pq_sendint8(&buffer, value->alphabet_code);
        pq_sendint8(&buffer, width_code);
        pq_sendint16(&buffer, value->padding_letters);
        pq_sendint32(&buffer, value->bp_size);
        pq_sendint32(&buffer, static_cast<std::uint32_t>(value->unit_count()));
        value->send_to(buffer, pg_send_byte_writer);
    }
    if (must_free) {
        pfree(value);
    }
    PG_RETURN_BYTEA_P(pq_endtypsend(&buffer));
}

export inline Datum seq_recv_impl(FunctionCallInfo fcinfo)
{
    auto* buffer = reinterpret_cast<StringInfo>(PG_GETARG_POINTER(0));
    const int32_t typmod = PG_GETARG_INT32(2);
    const std::uint8_t alphabet =
        static_cast<std::uint8_t>(pq_getmsgint(buffer, 1));
    if (alphabet == pgseq_unbound_alphabet_code) {
        const auto version = static_cast<std::uint8_t>(pq_getmsgint(buffer, 1));
        const auto tag = static_cast<std::uint8_t>(pq_getmsgint(buffer, 1));
        const auto length = static_cast<std::uint32_t>(pq_getmsgint(buffer, 4));
        if (version != 1u || tag != pgseq_unbound_width_tag
            || buffer->len - buffer->cursor != static_cast<int>(length)) {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                     errmsg("invalid unbound bioseq.seq wire framing")));
        }
        const char* ascii = pq_getmsgbytes(buffer, static_cast<int>(length));
        pgseq_t* unbound = allocate_unbound_ascii(
            ascii, length, /* already canonical */ true);
        pq_getmsgend(buffer);
        if (typmod < 0) {
            PG_RETURN_POINTER(unbound);
        }
        const auto target = decode_seq_typmod_checked(typmod);
        pgseq_t* packed = pack_ascii_for_target(
            reinterpret_cast<char const*>(unbound->data), length, target);
        pfree(unbound);
        PG_RETURN_POINTER(packed);
    }

    const auto width_code =
        static_cast<std::uint8_t>(pq_getmsgint(buffer, 1));
    if (!is_supported_alphabet_code(alphabet) || width_code > 3u) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                 errmsg("invalid bound bioseq.seq wire type tags")));
    }
    const std::uint8_t width = static_cast<std::uint8_t>(1u << width_code);
    if (!pgseq_supported_storage_unit_byte_widths::contains_runtime(width)) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                 errmsg("bioseq wire width %u is disabled", width * 8u)));
    }
    const auto padding = static_cast<std::uint16_t>(pq_getmsgint(buffer, 2));
    const auto bp_size = static_cast<std::uint32_t>(pq_getmsgint(buffer, 4));
    const auto unit_count = static_cast<std::uint32_t>(pq_getmsgint(buffer, 4));
    if (typmod >= 0) {
        const auto target = decode_seq_typmod_checked(typmod);
        if (target.alphabet_code != alphabet
            || target.storage_unit_byte_width != width) {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                     errmsg("bioseq wire type does not match destination typmod")));
        }
    }
    const std::uint64_t required =
        static_cast<std::uint64_t>(unit_count) * width;
    if (required > static_cast<std::uint64_t>(buffer->len - buffer->cursor)
        || pgseq_t::header_bytes()
                + round_up_to_uint64(static_cast<std::size_t>(required))
            > MaxAllocSize) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                 errmsg("invalid or oversized bound bioseq wire payload")));
    }
    bool metadata_ok = false;
    pgseq_t::dispatch_instance(
        alphabet, width,
        [&metadata_ok, unit_count, padding, bp_size]<class Instance>() {
        using traits = typename compact_sequence_range<Instance>::traits;
        const std::uint64_t derived = unit_count == 0u ? 0u
            : static_cast<std::uint64_t>(unit_count)
                    * traits::letters_per_unit
                - padding;
        metadata_ok = (unit_count == 0u ? padding == 0u
                                        : padding < traits::letters_per_unit)
            && derived == bp_size;
    });
    if (!metadata_ok) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                 errmsg("inconsistent bound bioseq wire length/padding")));
    }
    pgseq_t* result = nullptr;
    pgseq_supported_alphabets::dispatch_alphabet_by_code(
        alphabet,
        [&result, width, padding, bp_size, unit_count,
         buffer]<class Alphabet>() {
            result = pgseq_t::convert_from<Alphabet>(
                width, padding, bp_size, unit_count,
                *buffer, pg_recv_byte_reader, pg_alloc);
        });
    if (result == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_OUT_OF_MEMORY),
                 errmsg("out of memory receiving bioseq.seq")));
    }
    pq_getmsgend(buffer);
    PG_RETURN_POINTER(result);
}

export inline Datum seq_len_impl(FunctionCallInfo fcinfo)
{
    Datum input = PG_GETARG_DATUM(0);
    Datum dispatch = input;
    pgseq_t* flat = nullptr;
    if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(input))) {
        flat = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(input));
        dispatch = PointerGetDatum(flat);
    }
    const auto descriptor = classify_and_describe_sequence(dispatch);
    if (descriptor.kind == pg_sequence_datum_kind::locator) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cannot resolve a bioseq locator outside its owning table scan")));
    }
    if (flat != nullptr) {
        PG_FREE_IF_COPY(flat, 0);
    }
    PG_RETURN_INT64(static_cast<int64_t>(descriptor.bp_length));
}

export inline Datum seq_unit_len_impl(FunctionCallInfo fcinfo)
{
    Datum input = PG_GETARG_DATUM(0);
    Datum dispatch = input;
    pgseq_t* flat = nullptr;
    if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(input))) {
        flat = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(input));
        dispatch = PointerGetDatum(flat);
    }
    const auto descriptor = classify_and_describe_sequence(dispatch);
    if (descriptor.kind == pg_sequence_datum_kind::locator) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cannot resolve a bioseq locator outside its owning table scan")));
    }
    std::uint64_t units = descriptor.bp_length;
    if (descriptor.alphabet_bound) {
        pgseq_t::dispatch_instance(
            descriptor.alphabet_code,
            descriptor.storage_unit_byte_width,
            [&units, descriptor]<class Instance>() {
                using traits = typename compact_sequence_range<Instance>::traits;
                units = checked_ceil_div(
                    descriptor.bp_length, traits::letters_per_unit);
            });
    }
    if (flat != nullptr) {
        PG_FREE_IF_COPY(flat, 0);
    }
    PG_RETURN_INT64(static_cast<int64_t>(units));
}
