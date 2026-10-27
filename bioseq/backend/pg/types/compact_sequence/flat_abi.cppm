/**
 * @file flat_abi.cppm
 * Define the, flat PostgreSQL sequence type, its format tags, and alphavet helpers.
 */
module;

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <limits>
#include <new>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>

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
import bioseq.sequences.cigar;
import bioseq.storage.layout;

/// Rank-packed alphabets enabled for biological algorithms in this build.
export using pgseq_supported_biological_alphabets =
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    supported_alphabets<alphabet_entry<seq::dna4, "dna4">>;
#else
    supported_alphabets<
        alphabet_entry<seq::dna4, "dna4">,
        alphabet_entry<seq::dna5, "dna5">>;
#endif

/// Storage alphabets, derived from the biological registry without duplicating it.
export using pgseq_supported_alphabets = typename decltype(
    []<class... p_entries>(std::tuple<p_entries...>) {
        return std::type_identity<supported_alphabets<
            p_entries..., alphabet_entry<bioseq::cigar_symbol, "cigar">>>{};
    }(typename pgseq_supported_biological_alphabets::alphabet_entries_tuple{}))::type;

/// Physical CIGAR family code; projection spaces do not add alphabet types.
export inline constexpr std::uint8_t pgseq_cigar_alphabet_code =
    pgseq_supported_alphabets::index_of<bioseq::cigar_symbol>();

/** Return whether an alphabet code selects the run-encoded CIGAR family. */
export [[nodiscard]] constexpr bool pgseq_is_cigar_alphabet(
    std::uint8_t alphabet_code) noexcept
{
    return alphabet_code == pgseq_cigar_alphabet_code;
}

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
 *
 * ```sql
 * -- Bind a DNA string to the dna4 alphabet with 64-bit packing, then read its length and one-based slice.
 * SELECT 'ACGT'::bioseq.seq('dna4', 64) AS sequence, bioseq.len('ACGT'::bioseq.seq('dna4', 64)) AS length,
 *   ('ACGT'::bioseq.seq('dna4', 64))[2:3] AS slice;
 * ```
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

static_assert(seq::header_bytes() == 16u);
static_assert(seqan3::alphabet_size<bioseq::cigar_symbol>
              <= std::size_t{1u} << std::numeric_limits<std::uint8_t>::digits,
              "the PostgreSQL letter and lazy-rank facade require byte-sized ranks");
static_assert(pgseq_supported_storage_unit_byte_widths::contains<sizeof(bioseq::cigar_run)>);

}  // namespace bioseq::pg_types

/** Dispatch rank-packed alphabets without instantiating CIGAR numeric kernels. */
export template<class p_handler>
constexpr void pgseq_dispatch_biological_instance(
    std::uint8_t alphabet_code,
    std::uint8_t byte_width,
    p_handler&& handler)
{
    pgseq_supported_biological_alphabets::dispatch_alphabet_by_code(
        alphabet_code,
        [byte_width, &handler]<class p_alphabet>() {
            pgseq_supported_storage_unit_byte_widths::dispatch(
                byte_width,
                [&handler]<std::size_t p_width>() {
                    handler.template operator()<compact_sequence_stdc_instance<
                        bioseq::pg_types::seq, p_alphabet, p_width>>();
                });
        });
}

export namespace bioseq::postgres::support::detail {

/// @brief Pass a sequence pointer through PostgreSQL's Datum value.
template<>
struct scalar_codec<bioseq::pg_types::seq*> final {
    static constexpr const char* key = "bioseq.seq/opaque_pointer";

    /// @brief Read the sequence pointer stored in a Datum
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
/// Mask for the  storage-unit width bits of a bound sequence.
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
              == std::string_view{"'dna4', 'cigar'"},
#else
              == std::string_view{"'dna4', 'dna5', 'cigar'"},
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
/// @param[in] params Requested alphabet and stoage width.
/// @return The same alphabet with the build's selected width.
export [[nodiscard]] constexpr compact_sequence_stdc_type_params
pgseq_normalize_type_params(compact_sequence_stdc_type_params params) noexcept
{
    if (pgseq_is_cigar_alphabet(params.alphabet_code)) {
        params.encoding = bioseq::sequence_encoding::cigar_rle;
    } else {
        params.storage_unit_byte_width =
            pgseq_normalize_storage_unit_byte_width(
                params.storage_unit_byte_width);
    }
    return params;
}

/// Format identified by a  flat sequence's header tags.
export enum class pg_sequence_flat_kind : std::uint8_t {
    bound,
    unbound,
    locator,
    invalid,
};

/* The constructor is defined in :io, which already imports these type
 * definitions. Declaring it here lets shared helpers call it without making
 * these modules import each other. */
/// @brief Copy ASCII letters into a sequence whose alphabet, is still unset.
/// @param[in] input Letters to validate and copy into PostgreSQL memory.
/// @param[in] length Number of input symbols.
/// @param[in] require_already_canonical Reject input that needs case or spelling normalization.
/// @return A newly allocated unbound sequence
export [[nodiscard]] Datum make_unbound_sequence_from_ascii(
    char const* input,
    std::size_t length,
    bool require_already_canonical = false);

/// @brief Identify a flat sequence's format from its header tags.
/// @param[in] value Flat  sequence pointer, or nullptr. The value must already be detoasted.
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
                && value->encoding == bioseq::sequence_encoding::rank_packed
                && value->space == bioseq::cigar_space::compatibility
            ? pg_sequence_flat_kind::unbound
            : pg_sequence_flat_kind::invalid;
    }
    if (!is_supported_alphabet_code(
            static_cast<std::uint8_t>(value->alphabet_code))
        || !pgseq_supported_storage_unit_byte_widths::contains_runtime(width)) {
        return pg_sequence_flat_kind::invalid;
    }
    if (pgseq_is_cigar_alphabet(value->alphabet_code)) {
        if (value->encoding != bioseq::sequence_encoding::cigar_rle
            || value->storage_unit_byte_width != sizeof(bioseq::cigar_run)
            || value->padding_letters != 0u
            || value->space >= bioseq::cigar_space::count) {
            return pg_sequence_flat_kind::invalid;
        }
    } else if (value->encoding != bioseq::sequence_encoding::rank_packed
               || value->space != bioseq::cigar_space::compatibility) {
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

/// @brief Borrow writable ASCII letters of an unboubd sequence.
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

/* Validate a character before assigning it  to a SeqAn letter. Otherwise
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
/// @return Their agreed spelling, or invalid if none accept it or they  disagree.
export [[nodiscard]] inline pgseq_canonical_symbol
pgseq_canonical_unbound_symbol(unsigned char input) noexcept
{
    pgseq_canonical_symbol result{false, '\0'};
    for (std::uint8_t code = 0;
         code < pgseq_supported_alphabets::alphabet_count;
         ++code) {
        if (pgseq_is_cigar_alphabet(code)) {
            continue;
        }
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
/// @param[in] alphabet_code Alphabet to use
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

/// @brief Convert a valid alphabet rank to its leter.
/// @param[in] alphabet_code Alphabet, to use.
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

/// Allocate zeroed sequence storage, returning nullptr if it  is too large or allocation fails.
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

/** Translate a checked CIGAR failure at the PostgreSQL boundary. */
export [[noreturn]] inline void pg_raise_cigar_error(
    bioseq::cigar_error const& error,
    int invalid_sqlstate = ERRCODE_INVALID_TEXT_REPRESENTATION)
{
    using code = bioseq::cigar_error_code;
    const int sqlstate = error.code == code::allocation_rejected
        ? ERRCODE_OUT_OF_MEMORY
        : error.code == code::length_overflow
            || error.code == code::count_overflow
            || error.code == code::output_too_large
        ? ERRCODE_PROGRAM_LIMIT_EXCEEDED
        : error.code == code::invalid_encoding
        ? (invalid_sqlstate == ERRCODE_INVALID_BINARY_REPRESENTATION
            ? invalid_sqlstate : ERRCODE_DATA_CORRUPTED)
        : invalid_sqlstate;
    const char* reason = "invalid CIGAR value";
    switch (error.code) {
        case code::invalid_text: reason = "expected a positive decimal count followed by an operation"; break;
        case code::zero_count: reason = "zero counts and leading zeros are not allowed"; break;
        case code::invalid_operation: reason = "operation is outside the declared CIGAR alphabet or space"; break;
        case code::count_overflow: reason = "operation count exceeds the supported range"; break;
        case code::length_overflow: reason = "CIGAR length exceeds the supported sequence limit"; break;
        case code::invalid_encoding: reason = "inconsistent or noncanonical CIGAR encoding"; break;
        case code::unsupported_geometry: reason = "opaque operations do not define biological geometry"; break;
        case code::incompatible_alphabet: reason = "sequence alphabets are incompatible"; break;
        case code::input_length_mismatch: reason = "input length does not match the CIGAR"; break;
        case code::unknown_symbol_unavailable: reason = "the destination alphabet has no unknown symbol"; break;
        case code::allocation_rejected: reason = "CIGAR allocation was rejected"; break;
        case code::output_too_large: reason = "CIGAR output exceeds the allocation limit"; break;
    }
    ereport(ERROR,
            (errcode(sqlstate),
             errmsg("invalid bioseq CIGAR value: %s", reason),
             errdetail("input offset=%zu", error.input_offset)));
    pg_unreachable();
}

/** Allocate an exact flat CIGAR payload; the caller must complete its run writer. */
export [[nodiscard]] inline bioseq::pg_types::seq* allocate_pg_cigar_payload(
    std::uint32_t run_count,
    bioseq::cigar_space space = bioseq::cigar_space::compatibility)
{
    const auto payload_bytes = bioseq::measure_cigar_payload(run_count);
    if (!payload_bytes) {
        pg_raise_cigar_error(payload_bytes.error());
    }
    if (*payload_bytes > MaxAllocSize - bioseq::pg_types::seq::header_bytes()) {
        pg_raise_cigar_error({bioseq::cigar_error_code::output_too_large});
    }
    const std::size_t total_bytes =
        bioseq::pg_types::seq::header_bytes() + *payload_bytes;
    auto* result = pg_alloc(total_bytes);
    if (result == nullptr) {
        pg_raise_cigar_error({bioseq::cigar_error_code::allocation_rejected});
    }
    SET_VARSIZE(result, total_bytes);
    result->alphabet_code = pgseq_cigar_alphabet_code;
    result->storage_unit_byte_width = sizeof(bioseq::cigar_run);
    result->padding_letters = 0u;
    result->bp_size = 0u;
    result->encoding = bioseq::sequence_encoding::cigar_rle;
    result->space = space;
    return result;
}

/** Parse decimal-operation text directly into canonical flat RLE storage. */
export [[nodiscard]] inline bioseq::pg_types::seq* make_pg_cigar_from_text(
    std::string_view input,
    bioseq::cigar_space space = bioseq::cigar_space::compatibility,
    int invalid_sqlstate = ERRCODE_INVALID_TEXT_REPRESENTATION,
    std::optional<bioseq::cigar_parse_summary> known_measurement = std::nullopt)
{
    const auto measured = known_measurement.has_value()
        ? std::expected<bioseq::cigar_parse_summary, bioseq::cigar_error>{*known_measurement}
        : bioseq::measure_cigar_text(input, space);
    if (!measured) {
        pg_raise_cigar_error(measured.error(), invalid_sqlstate);
    }
    if (measured->run_count > (std::numeric_limits<std::uint32_t>::max)()) {
        pg_raise_cigar_error({bioseq::cigar_error_code::output_too_large});
    }
    auto* result = allocate_pg_cigar_payload(
        static_cast<std::uint32_t>(measured->run_count), space);
    bioseq::cigar_payload_writer<bioseq::cigar_symbol> writer{
        std::span<std::byte>{reinterpret_cast<std::byte*>(result->data),
            static_cast<std::size_t>(VARSIZE(result))
                - bioseq::pg_types::seq::header_bytes()},
        static_cast<std::uint32_t>(measured->run_count), space};
    const auto parsed = bioseq::parse_cigar_runs(
        input,
        [&writer](bioseq::cigar_run run)
            -> std::expected<void, bioseq::cigar_error> {
            return writer.append(run);
        },
        space);
    const auto finished = writer.finish();
    if (!parsed || !finished || *finished != measured->logical_length) {
        pfree(result);
        pg_raise_cigar_error(!parsed ? parsed.error() : !finished ? finished.error()
            : bioseq::cigar_error{bioseq::cigar_error_code::invalid_encoding},
            invalid_sqlstate);
    }
    result->bp_size = *finished;
    return result;
}

/** Allocate an owning flat CIGAR from checked, canonical run input. */
export [[nodiscard]] inline bioseq::pg_types::seq* make_pg_cigar_from_runs(
    std::span<const bioseq::cigar_run> runs,
    bioseq::cigar_space space = bioseq::cigar_space::compatibility)
{
    if (runs.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        pg_raise_cigar_error({bioseq::cigar_error_code::output_too_large});
    }
    auto* result = allocate_pg_cigar_payload(
        static_cast<std::uint32_t>(runs.size()), space);
    const auto payload_bytes = static_cast<std::size_t>(VARSIZE(result))
        - bioseq::pg_types::seq::header_bytes();
    const auto written = bioseq::write_cigar_payload<bioseq::cigar_symbol>(
        std::span<std::byte>{reinterpret_cast<std::byte*>(result->data), payload_bytes},
        runs, space);
    if (!written) {
        pfree(result);
        pg_raise_cigar_error(written.error());
    }
    result->bp_size = *written;
    return result;
}

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
