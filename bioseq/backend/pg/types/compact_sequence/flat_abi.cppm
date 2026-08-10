/* PostgreSQL-facing flat ABI for bioseq.seq.  This partition is the narrow
 * shared surface consumed by the table/storage modules. */
module;

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <string_view>

#include <bioseq/pg_build_profile.h>
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
import bioseq.sequences;
import bioseq.storage.layout;

export using pgseq_supported_alphabets =
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    supported_alphabets<alphabet_entry<seq::dna4, "dna4">>;
#else
    supported_alphabets<
        alphabet_entry<seq::dna4, "dna4">,
        alphabet_entry<seq::dna5, "dna5">>;
#endif

export using pgseq_supported_storage_unit_byte_widths =
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES)
    supported_storage_unit_byte_widths<8u>;
#else
    all_storage_unit_byte_widths;
#endif

export namespace bioseq::pg_types {

/**
 * @brief Compact, alphabet-aware biological sequence stored as a variable-length,
 * TOASTable PostgreSQL value for indexing, similarity search, and analysis.
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

template<>
struct scalar_codec<bioseq::pg_types::seq*> final {
    static constexpr const char* key = "bioseq.seq/opaque_pointer";

    [[nodiscard]] static bioseq::pg_types::seq* read(
        Datum value) noexcept {
        return reinterpret_cast<bioseq::pg_types::seq*>(
            DatumGetPointer(value));
    }

    [[nodiscard]] static Datum write(
        bioseq::pg_types::seq* value) noexcept {
        return PointerGetDatum(value);
    }
};

}  // namespace bioseq::postgres::support::detail

export inline constexpr std::uint8_t pgseq_unbound_alphabet_code = 0xffu;
export inline constexpr std::uint8_t pgseq_unbound_width_tag = 0x40u;
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

export [[nodiscard]] constexpr bool
is_supported_alphabet_code(std::uint8_t code) noexcept
{
    return code < pgseq_supported_alphabets::alphabet_count;
}

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

export [[nodiscard]] inline std::span<std::byte const>
pgseq_unbound_ascii(bioseq::pg_types::seq const& value) noexcept
{
    return {
        reinterpret_cast<std::byte const*>(value.data),
        static_cast<std::size_t>(value.bp_size),
    };
}

export [[nodiscard]] inline std::span<std::byte>
pgseq_unbound_ascii(bioseq::pg_types::seq& value) noexcept
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
