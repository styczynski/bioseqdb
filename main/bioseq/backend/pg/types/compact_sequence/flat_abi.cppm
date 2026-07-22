/* PostgreSQL-facing flat ABI for bioseq.seq.  This partition is the narrow
 * compatibility surface consumed by the table/storage modules. */
module;

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <string_view>

#include "headers/pg_build_config.h"
#include "headers/pg_compat.h"

extern "C" {
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "utils/syscache.h"
}

export module bioseq.types.pg_compact_sequence:flat_abi;

import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.storage.bucket_math;

export constexpr int32_t PG_SEQ_TYPMOD_UNCONSTRAINED = -1;

export using pgseq_supported_alphabets =
#if BIOSEQ_OPTTIMIZATION_LEVEL == 0
    supported_alphabets<alphabet_entry<seq::dna4, "dna4">>;
#else
    supported_alphabets<
        alphabet_entry<seq::dna4, "dna4">,
        alphabet_entry<seq::dna5, "dna5">>;
#endif

export using pgseq_supported_storage_unit_byte_widths =
#if BIOSEQ_OPTTIMIZATION_LEVEL <= 1
    supported_storage_unit_byte_widths<8u>;
#else
    all_storage_unit_byte_widths;
#endif

/* Kept only for source compatibility with existing importers.  Typmodless
 * SQL input no longer binds to this alphabet; it uses the unbound tag below. */
export using pgseq_unconstrained_input_alphabet =
#if BIOSEQ_OPTTIMIZATION_LEVEL == 0
    seq::dna4;
#else
    seq::dna5;
#endif

export using pgseq_t = compact_sequence_stdc<
    pgseq_supported_alphabets,
    std::uint32_t,
    std::int32_t,
    pgseq_supported_storage_unit_byte_widths>;

export inline constexpr std::uint8_t pgseq_unbound_alphabet_code = 0xffu;
export inline constexpr std::uint8_t pgseq_unbound_width_tag = 0x40u;
export inline constexpr std::uint8_t pgseq_physical_width_mask = 0x0fu;

static_assert((pgseq_unbound_width_tag
               & bioseq::storage::k_seq_locator_stub_marker) == 0u);
static_assert((pgseq_unbound_width_tag & pgseq_physical_width_mask) == 0u);
static_assert((bioseq::storage::k_seq_locator_stub_marker
               & pgseq_physical_width_mask) == 0u);
static_assert(pgseq_unbound_alphabet_code
              >= pgseq_supported_alphabets::alphabet_count);

static_assert(pgseq_supported_alphabets::valid_alphabet_names_csv()
#if BIOSEQ_OPTTIMIZATION_LEVEL == 0
              == std::string_view{"'dna4'"},
#else
              == std::string_view{"'dna4', 'dna5'"},
#endif
              "supported-alphabet display names drifted");
static_assert(
    pgseq_supported_alphabets::entry_for<seq::dna4>::alphabet_name_sv
        == std::string_view{"dna4"});
#if BIOSEQ_OPTTIMIZATION_LEVEL >= 1
static_assert(
    pgseq_supported_alphabets::entry_for<seq::dna5>::alphabet_name_sv
        == std::string_view{"dna5"});
#endif

export [[nodiscard]] constexpr bool
is_supported_alphabet_code(std::uint8_t code) noexcept
{
    return code < pgseq_supported_alphabets::alphabet_count;
}

export [[nodiscard]] constexpr std::uint8_t
pgseq_normalize_storage_unit_byte_width(std::uint8_t requested) noexcept
{
#if BIOSEQ_OPTTIMIZATION_LEVEL <= 1
    (void) requested;
    return 8u;
#else
    return requested;
#endif
}

export [[nodiscard]] constexpr compact_sequence_stdc_type_params
pgseq_normalize_type_params(compact_sequence_stdc_type_params params) noexcept
{
    params.storage_unit_byte_width =
        pgseq_normalize_storage_unit_byte_width(
            params.storage_unit_byte_width);
    return params;
}

export enum class pg_sequence_flat_kind : std::uint8_t {
    bound,
    unbound,
    locator,
    invalid,
};

export [[nodiscard]] inline pg_sequence_flat_kind
classify_pg_sequence_flat(pgseq_t const* value) noexcept
{
    if (value == nullptr) {
        return pg_sequence_flat_kind::invalid;
    }
    const auto width =
        static_cast<std::uint8_t>(value->storage_unit_byte_width);
    if (bioseq::storage::is_seq_locator_stub_width(width)) {
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

export [[nodiscard]] inline std::span<std::byte const>
pgseq_unbound_ascii(pgseq_t const& value) noexcept
{
    return {
        reinterpret_cast<std::byte const*>(value.data),
        static_cast<std::size_t>(value.bp_size),
    };
}

export [[nodiscard]] inline std::span<std::byte>
pgseq_unbound_ascii(pgseq_t& value) noexcept
{
    return {
        reinterpret_cast<std::byte*>(value.data),
        static_cast<std::size_t>(value.bp_size),
    };
}

export struct pgseq_canonical_symbol {
    bool valid;
    char value;
};

/* Strict lexical helpers shared by unbound input, letter I/O, literal
 * adoption, and array/text bridges.  SeqAn assignment is performed only after
 * char_is_valid_for succeeds, so invalid input is never mapped to rank zero. */
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
        [&result, input]<class Alphabet>() {
            const char symbol = static_cast<char>(input);
            if (!seqan3::char_is_valid_for<Alphabet>(symbol)) {
                return;
            }
            Alphabet value{};
            value.assign_char(symbol);
            result = {true, seqan3::to_char(value)};
        });
    return result;
}

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
        [input, canonical, &rank, &valid]<class Alphabet>() {
            const char symbol = static_cast<char>(input);
            if (!seqan3::char_is_valid_for<Alphabet>(symbol)) {
                return;
            }
            Alphabet value{};
            value.assign_char(symbol);
            rank = static_cast<std::uint8_t>(seqan3::to_rank(value));
            if (canonical != nullptr) {
                *canonical = seqan3::to_char(value);
            }
            valid = true;
        });
    return valid;
}

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
        [rank, &symbol, &valid]<class Alphabet>() {
            if (rank >= seqan3::alphabet_size<Alphabet>) {
                return;
            }
            Alphabet value{};
            seqan3::assign_rank_to(rank, value);
            symbol = seqan3::to_char(value);
            valid = true;
        });
    return valid;
}

export inline constexpr auto pg_alloc =
    [](std::size_t byte_count) -> pgseq_t* {
        if (byte_count > static_cast<std::size_t>(MaxAllocSize)) {
            return nullptr;
        }
        return static_cast<pgseq_t*>(
            palloc_extended(byte_count,
                            MCXT_ALLOC_NO_OOM | MCXT_ALLOC_ZERO));
    };

static_assert(stdc_allocator<decltype(pg_alloc), pgseq_t>);

/* Legacy flat-only extraction remains available to intentionally unaware
 * consumers.  Slice-aware code classifies the original Datum first. */
export template<class S = pgseq_t>
[[nodiscard]] inline S* pg_seq_arg(FunctionCallInfo fcinfo, int argument)
{
    return reinterpret_cast<S*>(
        PG_DETOAST_DATUM(PG_GETARG_DATUM(argument)));
}

/* Missing-ok and deliberately uncached: integration tests DROP/CREATE the
 * extension in one backend and PostgreSQL assigns fresh type OIDs. */
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

export [[nodiscard]] inline Oid bioseq_seq_type_oid_missing_ok() noexcept
{
    return bioseq_type_oid_missing_ok("seq");
}

export [[nodiscard]] inline Oid bioseq_letter_type_oid_missing_ok() noexcept
{
    return bioseq_type_oid_missing_ok("letter");
}
