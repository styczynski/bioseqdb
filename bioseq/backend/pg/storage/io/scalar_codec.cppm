/** @file Deterministic scalar text at the PostgreSQL / neutral IO boundary. */
module;
#include <bioseq/pg_compat.h>
extern "C" {
#include "catalog/pg_type_d.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
}
export module bioseq.postgres.storage.io.scalar_codec;
import std;
import bioseq.io.simd;
import bioseq.io.sinks;
import bioseq.parallelism.memory_admission;
import bioseq.postgres.support;
import bioseq.postgres.support.sql_boundary;

export namespace bioseq::postgres::storage::io {

/** Require the encoding used by canonical headers, JSON payloads and IO names.
 * PostgreSQL catalog identifiers and typed metadata share the same boundary;
 * partial value-only conversion would still produce inconsistent records.
 */
inline void require_utf8_io_encoding() {
    if (GetDatabaseEncoding() != PG_UTF8)
        bioseq::postgres::support::raise_sql_error(ERRCODE_FEATURE_NOT_SUPPORTED,
            {.message = "Bioseq canonical and SAM/BAM IO requires a UTF8 database encoding"});
}

/** Scope canonical settings around type IO only, restoring even after PG errors.
 * No source query executes inside this scope; GUC_ACTION_SAVE restores the caller
 * without string snapshots and also composes with function-local settings.
 */
template<class Function>
[[nodiscard]] inline auto with_scalar_io_settings(Function&& function) {
    using bioseq::postgres::support::invoke_pg_as_cpp;
    const int level = invoke_pg_as_cpp([] { return NewGUCNestLevel(); });
    try {
        invoke_pg_as_cpp([] {
            constexpr std::array settings{
                std::pair{"DateStyle", "ISO, YMD"}, std::pair{"IntervalStyle", "iso_8601"},
                std::pair{"TimeZone", "UTC"}, std::pair{"extra_float_digits", "3"},
                std::pair{"bytea_output", "hex"}, std::pair{"search_path", "pg_catalog"},
                std::pair{"array_nulls", "on"}, std::pair{"lc_monetary", "C"},
                std::pair{"xmloption", "content"}};
            for (const auto& [name, value] : settings)
                static_cast<void>(set_config_option(name, value, PGC_USERSET, PGC_S_SESSION,
                    GUC_ACTION_SAVE, true, ERROR, false));
        });
        auto result = std::forward<Function>(function)();
        invoke_pg_as_cpp([level] { AtEOXact_GUC(true, level); });
        return result;
    } catch (...) {
        const auto failure = std::current_exception();
        try { invoke_pg_as_cpp([level] { AtEOXact_GUC(false, level); }); } catch (...) {}
        std::rethrow_exception(failure);
    }
}

/** Admit scalar output bytes through the caller's existing reservation. */
template<class Admit>
concept scalar_output_admitter = requires(Admit& admit, std::size_t bytes) {
    { admit(bytes) } -> std::same_as<bool>;
};

/** Format floating values on the stack, then admit their exact retained spelling. */
template<std::floating_point Value, scalar_output_admitter Admit>
[[nodiscard]] inline std::optional<std::string> format_io_float(Value value, Admit&& admit) {
    std::array<char, 128> characters{};
    std::string_view spelling;
    if (std::isnan(value)) spelling = "NaN";
    else if (std::isinf(value)) spelling = std::signbit(value) ? "-Infinity" : "Infinity";
    else {
        const auto result = std::to_chars(characters.data(), characters.data() + characters.size(),
            value, std::chars_format::general, std::numeric_limits<Value>::max_digits10);
        if (result.ec != std::errc{}) bioseq::postgres::support::raise_sql_error(ERRCODE_INTERNAL_ERROR,
            {.message = "cannot format canonical floating-point metadata"});
        spelling = {characters.data(), static_cast<std::size_t>(result.ptr - characters.data())};
    }
    if (!admit(spelling.size())) return std::nullopt;
    return std::string{spelling};
}

/** Encode primitives and stable PG type output, admitting variable C++ backing
 * before copying it. PG type-output functions necessarily allocate their transient
 * CString first; that caller-thread allocation is freed on success, denial or error.
 * Custom type IO must itself round trip under the scoped settings; arbitrary
 * user-defined functions can depend on other ambient state that IO cannot infer.
 * NULL optional means retryable admission pressure, never SQL NULL metadata.
 */
template<scalar_output_admitter Admit>
[[nodiscard]] inline std::optional<std::string> format_io_scalar(
        Datum value, Oid type, Admit&& admit) {
    using bioseq::postgres::support::invoke_pg_as_cpp;
    const auto base = invoke_pg_as_cpp([type] { return getBaseType(type); });
    if (base == BOOLOID) {
        if (!admit(1u)) return std::nullopt;
        return DatumGetBool(value) ? "1" : "0";
    }
    if (base == FLOAT4OID) return format_io_float(DatumGetFloat4(value), admit);
    if (base == FLOAT8OID) return format_io_float(DatumGetFloat8(value), admit);
    if (base == BYTEAOID) {
        const auto* bytes = invoke_pg_as_cpp([value] { return DatumGetByteaPP(value); });
        const bioseq::postgres::support::detail::palloc_owner copy_owner{
            static_cast<const void*>(bytes) != static_cast<const void*>(DatumGetPointer(value))
                ? const_cast<bytea*>(bytes) : nullptr};
        const auto size = static_cast<std::size_t>(VARSIZE_ANY_EXHDR(bytes));
        if (size > std::numeric_limits<std::size_t>::max() / 2u)
            bioseq::postgres::support::raise_sql_error(ERRCODE_PROGRAM_LIMIT_EXCEEDED,
                {.message = "canonical bytea output size overflows address space"});
        if (!admit(size * 2u)) return std::nullopt;
        std::string encoded(size * 2u, '\0');
        if (!bioseq::io::simd::encode_upper_hex_bytes(
                {reinterpret_cast<const std::uint8_t*>(VARDATA_ANY(bytes)), size},
                {reinterpret_cast<std::uint8_t*>(encoded.data()), encoded.size()}))
            bioseq::postgres::support::raise_sql_error(ERRCODE_INTERNAL_ERROR,
                {.message = "cannot encode canonical bytea metadata"});
        return encoded;
    }
    const auto output = [value, type, &admit]() -> std::optional<std::string> {
        char* const encoded = invoke_pg_as_cpp([value, type] {
            Oid function = InvalidOid;
            bool varlena = false;
            getTypeOutputInfo(type, &function, &varlena);
            return OidOutputFunctionCall(function, value);
        });
        const bioseq::postgres::support::detail::palloc_owner output_owner{encoded};
        const auto size = std::char_traits<char>::length(encoded);
        if (!admit(size)) return std::nullopt;
        return std::string{encoded, size};
    };
    switch (base) {
        case TEXTOID: case VARCHAROID: case BPCHAROID: case NAMEOID:
        case INT2OID: case INT4OID: case INT8OID: case OIDOID:
        case NUMERICOID: case JSONOID: case JSONBOID: return output();
        default: return with_scalar_io_settings(output);
    }
}

/** Parse canonical metadata with the same stable settings and bytea convention.
 * Type input requires writable, NUL-terminated storage. Small spellings stay on
 * the stack; larger copies retain an independent admission owner through type IO.
 */
[[nodiscard]] inline Datum parse_io_scalar(std::string_view input, Oid type, Oid input_function,
        Oid io_parameter, std::int32_t typmod) {
    using bioseq::postgres::support::invoke_pg_as_cpp;
    const auto base = invoke_pg_as_cpp([type] { return getBaseType(type); });
    const std::size_t prefix = base == BYTEAOID ? 2u : 0u;
    if (input.size() > std::numeric_limits<std::size_t>::max() - prefix - 1u)
        bioseq::postgres::support::raise_sql_error(ERRCODE_PROGRAM_LIMIT_EXCEEDED,
            {.message = "canonical scalar input size overflows address space"});
    const auto required = input.size() + prefix + 1u;
    std::array<char, 256u> inline_spelling;
    // Destruction frees the writable backing before releasing its full capacity.
    bioseq::parallelism::memory_permit spelling_permit;
    std::vector<char> owned_spelling;
    char* spelling = inline_spelling.data();
    if (required > inline_spelling.size()) {
        if (auto reserved = bioseq::io::reserve_io_vector_capacity(
                owned_spelling, spelling_permit, required); !reserved)
            bioseq::postgres::support::raise_sql_error(ERRCODE_OUT_OF_MEMORY,
                {.message = "cannot admit canonical scalar input", .detail = std::move(reserved.error())});
        owned_spelling.resize(required);
        spelling = owned_spelling.data();
    }
    if (prefix != 0u) { spelling[0] = '\\'; spelling[1] = 'x'; }
    if (!input.empty()) std::copy_n(input.data(), input.size(), spelling + prefix);
    spelling[required - 1u] = '\0';
    const auto convert = [spelling, input_function, io_parameter, typmod] {
        return invoke_pg_as_cpp([spelling, input_function, io_parameter, typmod] {
            return OidInputFunctionCall(input_function, spelling, io_parameter, typmod);
        });
    };
    switch (base) {
        case TEXTOID: case VARCHAROID: case BPCHAROID: case NAMEOID:
        case INT2OID: case INT4OID: case INT8OID: case OIDOID:
        case NUMERICOID: case JSONOID: case JSONBOID: case BOOLOID:
        case BYTEAOID: case FLOAT4OID: case FLOAT8OID: return convert();
        default: return with_scalar_io_settings(convert);
    }
}
} // namespace bioseq::postgres::storage::io
