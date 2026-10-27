/**
 * @file content.cppm
 * Compare and hash sequence contents for SQL operators and indexes.
 *
 * Alphabet binding and alphabet code come before  the letters in the ordering.
 * Values with the same alphabet compare by letter rank, regardless of storage
 * width or piece-tree shape. Unbound values compare their text bytes.
 */
module;

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>

#include <libassert/assert.hpp>

#include <bioseq/pg_compat.h>
extern "C" {
#include "utils/expandeddatum.h"
}

export module bioseq.postgres.types.compact_sequence:content;

import bioseq.codegen.sql.annotation_model;
import :flat_abi;
import :source_abi;
import :tree_types;
import :view_session;
import :packed_output;
import bioseq.storage.lazy_sequence;
import bioseq.common.context;
import bioseq.postgres.support.sql_boundary;
import bioseq.sequences.cigar;
import bioseq.sequences.cigar_alphabet;

namespace {

/** Borrow a function's allocator service only when a residual profile tie needs it. */
struct pg_cigar_comparison_scratch_provider {
    bioseq::postgres::support::function_call_context const& call;

    /** Acquire function-lifetime admitted scratch, without retaining input values. */
    [[nodiscard]] std::expected<std::reference_wrapper<context::context>, bioseq::cigar_error>
    acquire() {
        try {
            return std::ref(acquire_pg_sequence_admitted_context(
                call.function_memory_context(), call.function_extra()));
        } catch (const std::bad_alloc&) {
            return std::unexpected(bioseq::cigar_error{
                bioseq::cigar_error_code::allocation_rejected});
        } catch (const std::length_error&) {
            return std::unexpected(bioseq::cigar_error{
                bioseq::cigar_error_code::output_too_large});
        }
    }
};

struct content_operand {
    Datum dispatch;
    bioseq::pg_types::seq* flat;
    pg_sequence_descriptor descriptor;
    bool free_flat;
};

[[nodiscard]] inline content_operand describe_content_operand(Datum input)
{
    if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(input))) {
        return {
            .dispatch = input,
            .flat = nullptr,
            .descriptor = classify_and_describe_sequence(input),
            .free_flat = false,
        };
    }
    auto* flat = reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(input));
    return {
        .dispatch = PointerGetDatum(flat),
        .flat = flat,
        .descriptor = classify_and_describe_sequence(PointerGetDatum(flat)),
        .free_flat = reinterpret_cast<Pointer>(flat)
            != DatumGetPointer(input),
    };
}

inline void release_content_operand(content_operand& operand) noexcept
{
    if (operand.free_flat && operand.flat != nullptr) {
        pfree(operand.flat);
    }
    operand.flat = nullptr;
    operand.free_flat = false;
}

[[noreturn]] inline void content_locator_error()
{
    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("cannot resolve a bioseq locator outside its owning table scan"),
             errdetail("relation, column, snapshot, and cache ownership are unavailable")));
    __builtin_unreachable();
}

inline void read_unbound_chunk(
    content_operand const& operand,
    std::uint64_t from,
    std::span<std::uint8_t> output)
{
    if (operand.flat != nullptr) {
        std::memcpy(
            output.data(), operand.flat->data + from, output.size());
        return;
    }
    auto const* expanded = get_pg_sequence_expanded_ro(operand.dispatch);
    if (!emit_pg_unbound_ascii(
            expanded->root, from, from + output.size(),
            std::span<std::byte>{
                reinterpret_cast<std::byte*>(output.data()),
                output.size()})) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("invalid unbound bioseq piece tree")));
    }
}

[[nodiscard]] inline int compare_unbound(
    content_operand const& left,
    content_operand const& right)
{
    const std::uint64_t common =
        std::min(left.descriptor.bp_length, right.descriptor.bp_length);
    std::array<std::uint8_t, 256> left_chunk{};
    std::array<std::uint8_t, 256> right_chunk{};
    std::uint64_t position = 0u;
    int result = 0;
    while (position < common && result == 0) {
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(left_chunk.size(), common - position));
        read_unbound_chunk(
            left, position,
            std::span<std::uint8_t>{left_chunk.data(), count});
        read_unbound_chunk(
            right, position,
            std::span<std::uint8_t>{right_chunk.data(), count});
        const int bytes = std::memcmp(
            left_chunk.data(), right_chunk.data(), count);
        result = bytes < 0 ? -1 : bytes > 0 ? 1 : 0;
        position += count;
    }
    if (result == 0) {
        result = left.descriptor.bp_length < right.descriptor.bp_length ? -1
            : left.descriptor.bp_length > right.descriptor.bp_length ? 1 : 0;
    }
    return result;
}

[[nodiscard]] inline std::uint64_t hash_unbound(
    content_operand const& operand,
    std::uint64_t prefixed_seed)
{
    constexpr std::uint64_t offset = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offset ^ prefixed_seed;
    std::array<std::uint8_t, 256> chunk{};
    std::uint64_t position = 0u;
    while (position < operand.descriptor.bp_length) {
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(
                chunk.size(), operand.descriptor.bp_length - position));
        read_unbound_chunk(
            operand, position,
            std::span<std::uint8_t>{chunk.data(), count});
        for (std::size_t index = 0; index < count; ++index) {
            hash ^= chunk[index];
            hash *= prime;
        }
        position += count;
    }
    for (unsigned shift = 0u; shift < 64u; shift += 8u) {
        hash ^= static_cast<std::uint8_t>(
            operand.descriptor.bp_length >> shift);
        hash *= prime;
    }
    hash ^= hash >> 30u;
    hash *= 0xbf58476d1ce4e5b9ull;
    hash ^= hash >> 27u;
    hash *= 0x94d049bb133111ebull;
    hash ^= hash >> 31u;
    return hash;
}

[[nodiscard]] inline std::uint64_t prefix_hash_seed(
    std::uint64_t seed,
    std::uint8_t state_tag,
    bool include_alphabet,
    std::uint8_t alphabet) noexcept
{
    constexpr std::uint64_t offset = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t state = offset ^ seed;
    state ^= state_tag;
    state *= prime;
    if (include_alphabet) {
        state ^= alphabet;
        state *= prime;
    }
    /* Cancel the next hash function's initial XOR so it starts with this prefix. */
    return offset ^ state;
}

} // namespace

template<bool p_match_order>
[[nodiscard]] inline int seq_compare_internal(
    Datum left_input,
    Datum right_input,
    bioseq::postgres::support::function_call_context const* call = nullptr)
{
    content_operand left = describe_content_operand(left_input);
    content_operand right = describe_content_operand(right_input);
    if (left.descriptor.kind == pg_sequence_datum_kind::locator
        || right.descriptor.kind == pg_sequence_datum_kind::locator) {
        content_locator_error();
    }
    const std::uint8_t left_tag = left.descriptor.alphabet_bound ? 1u : 0u;
    const std::uint8_t right_tag = right.descriptor.alphabet_bound ? 1u : 0u;
    int result = 0;
    if (left_tag != right_tag) {
        result = left_tag < right_tag ? -1 : 1;
    } else if (left_tag == 0u) {
        result = compare_unbound(left, right);
    } else if (left.descriptor.alphabet_code
               != right.descriptor.alphabet_code) {
        result = left.descriptor.alphabet_code
                < right.descriptor.alphabet_code ? -1 : 1;
    } else if (pgseq_is_cigar_alphabet(left.descriptor.alphabet_code)) {
        if constexpr (p_match_order) {
            DEBUG_ASSERT(call != nullptr, "CIGAR ordering requires an explicit scratch provider");
            pg_cigar_comparison_scratch_provider scratch{*call};
            const auto comparison = with_pg_cigar_sequences<
                std::expected<std::strong_ordering, bioseq::cigar_error>>(
                left.dispatch, right.dispatch,
                [&scratch](auto& backend, auto left_view, auto right_view) {
                    return bioseq::compare_cigar(backend, left_view, right_view, scratch);
                });
            if (!comparison) pg_raise_cigar_error(comparison.error());
            result = *comparison < 0 ? -1 : *comparison > 0 ? 1 : 0;
        } else {
            const auto equal = with_pg_cigar_sequences<
                std::expected<bool, bioseq::cigar_error>>(
                left.dispatch, right.dispatch,
                [](auto& backend, auto left_view, auto right_view) {
                    return bioseq::equal_cigar(backend, left_view, right_view);
                });
            if (!equal) pg_raise_cigar_error(equal.error());
            result = *equal ? 0 : 1;
        }
    } else {
        result = with_pg_sequence_views<int>(
            left_input, right_input,
            [](auto left_view, auto right_view) {
                return bioseq::storage::compare_lazy_sequences(
                    left_view, right_view);
            });
    }
    release_content_operand(left);
    release_content_operand(right);
    return result;
}

export namespace bioseq::pg_types::sql_routines {

/**
 * @brief Compare two sequences by alphabet binding, alphabet code, and content.
 * Unbound values sort before bound values and compare by text bytes. Bound
 * values sort by alphabet code, then by their alphabet's sequence order.
 * CIGAR prioritizes reported matches and match intervals before rank order.
 * Other alphabets compare ranks, with a shorter equal prefix first. Packing
 * width and piece-tree layout do
 * not affect the result; unresolved table locators are rejected.
 * @param left First sequence to compare.
 * @param right Second sequence to compare.
 * @return -1 when left sorts first, 0 when equal, or 1 when right sorts first.
 *
 * ```sql
 * -- Compare sequences directly: the longer value sorts after its equal prefix.
 * SELECT bioseq.seq_cmp('ACGT'::bioseq.seq('dna4', 64), 'ACG'::bioseq.seq('dna4', 64));
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
std::int32_t seq_cmp(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum left,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum right,
    bioseq::postgres::support::function_call_context call)
{
    return static_cast<std::int32_t>(
        seq_compare_internal<true>(left, right, std::addressof(call)));
}

/**
 * @brief Test whether the left sequence sorts before the right sequence.
 * Use seq_cmp ordering: unbound values first, then alphabet code and content.
 * @param left First sequence to compare.
 * @param right Second sequence to compare.
 * @return True when seq_cmp(left, right) is negative.
 *
 * ```sql
 * -- Compare two DNA sequences with the < operator and its implementing function seq_lt. Both expressions use alphabet and content ordering.
 * SELECT 'ACGT'::bioseq.seq('dna4', 64) OPERATOR(bioseq.<) 'ACGA'::bioseq.seq('dna4', 64) AS operator_result,
 *   bioseq.seq_lt('ACGT'::bioseq.seq('dna4', 64), 'ACGA'::bioseq.seq('dna4', 64)) AS function_result;
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::operator_({
    .symbol = "<",
    .commutator = bioseq::pg_types::sql_routines::seq_gt,
    .negator = bioseq::pg_types::sql_routines::seq_ge,
    .restrict = bioseq::sql::pg_catalog_reference::scalarltsel,
    .join = bioseq::sql::pg_catalog_reference::scalarltjoinsel,
})]]
bool seq_lt(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum left,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum right,
    bioseq::postgres::support::function_call_context call)
{
    return seq_compare_internal<true>(left, right, std::addressof(call)) < 0;
}

/**
 * @brief Test whether the left sequence sorts before or equals the right sequence.
 * Use seq_cmp ordering: unbound values first, then alphabet code and content.
 * @param left First sequence to compare.
 * @param right Second sequence to compare.
 * @return True when seq_cmp(left, right) is negative or zero.
 *
 * ```sql
 * -- Compare two DNA sequences with the <= operator and its implementing function seq_le. Both expressions use alphabet and content ordering.
 * SELECT 'ACGT'::bioseq.seq('dna4', 64) OPERATOR(bioseq.<=) 'ACGA'::bioseq.seq('dna4', 64) AS operator_result,
 *   bioseq.seq_le('ACGT'::bioseq.seq('dna4', 64), 'ACGA'::bioseq.seq('dna4', 64)) AS function_result;
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::operator_({
    .symbol = "<=",
    .commutator = bioseq::pg_types::sql_routines::seq_ge,
    .negator = bioseq::pg_types::sql_routines::seq_gt,
    .restrict = bioseq::sql::pg_catalog_reference::scalarlesel,
    .join = bioseq::sql::pg_catalog_reference::scalarlejoinsel,
})]]
bool seq_le(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum left,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum right,
    bioseq::postgres::support::function_call_context call)
{
    return seq_compare_internal<true>(left, right, std::addressof(call)) <= 0;
}

/**
 * @brief Test equality of sequence alphabet binding and content.
 * Bound values must have matching alphabet codes and letter ranks. Unbound
 * values compare byte-for-byte. Packing width and piece-tree layout are ignored.
 * @param left First sequence to compare.
 * @param right Second sequence to compare.
 * @return True when both values have the same alphabet binding and content.
 *
 * ```sql
 * -- Compare two DNA sequences with the = operator and its implementing function seq_eq. Both expressions use alphabet and content ordering.
 * SELECT 'ACGT'::bioseq.seq('dna4', 64) OPERATOR(bioseq.=) 'ACGA'::bioseq.seq('dna4', 64) AS operator_result,
 *   bioseq.seq_eq('ACGT'::bioseq.seq('dna4', 64), 'ACGA'::bioseq.seq('dna4', 64)) AS function_result;
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::operator_({
    .symbol = "=",
    .commutator = bioseq::pg_types::sql_routines::seq_eq,
    .negator = bioseq::pg_types::sql_routines::seq_ne,
    .restrict = bioseq::sql::pg_catalog_reference::eqsel,
    .join = bioseq::sql::pg_catalog_reference::eqjoinsel,
    .hashes = true,
    .merges = true,
})]]
bool seq_eq(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum left,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum right)
{
    return seq_compare_internal<false>(left, right) == 0;
}

/**
 * @brief Test whether sequence alphabet binding or content differs.
 * Use the same alphabet and content rules as seq_eq.
 * @param left First sequence to compare.
 * @param right Second sequence to compare.
 * @return True when the values differ in binding, alphabet code, or content.
 *
 * ```sql
 * -- Compare two DNA sequences with the <> operator and its implementing function seq_ne. Both expressions use alphabet and content ordering.
 * SELECT 'ACGT'::bioseq.seq('dna4', 64) OPERATOR(bioseq.<>) 'ACGA'::bioseq.seq('dna4', 64) AS operator_result,
 *   bioseq.seq_ne('ACGT'::bioseq.seq('dna4', 64), 'ACGA'::bioseq.seq('dna4', 64)) AS function_result;
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::operator_({
    .symbol = "<>",
    .commutator = bioseq::pg_types::sql_routines::seq_ne,
    .negator = bioseq::pg_types::sql_routines::seq_eq,
    .restrict = bioseq::sql::pg_catalog_reference::neqsel,
    .join = bioseq::sql::pg_catalog_reference::neqjoinsel,
})]]
bool seq_ne(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum left,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum right)
{
    return seq_compare_internal<false>(left, right) != 0;
}

/**
 * @brief Test whether the left sequence sorts after or equals the right sequence.
 * Use seq_cmp ordering: unbound values first, then alphabet code and content.
 * @param left First sequence to compare.
 * @param right Second sequence to compare.
 * @return True when seq_cmp(left, right) is positive or zero.
 *
 * ```sql
 * -- Compare two DNA sequences with the >= operator and its implementing function seq_ge. Both expressions use alphabet and content ordering.
 * SELECT 'ACGT'::bioseq.seq('dna4', 64) OPERATOR(bioseq.>=) 'ACGA'::bioseq.seq('dna4', 64) AS operator_result,
 *   bioseq.seq_ge('ACGT'::bioseq.seq('dna4', 64), 'ACGA'::bioseq.seq('dna4', 64)) AS function_result;
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::operator_({
    .symbol = ">=",
    .commutator = bioseq::pg_types::sql_routines::seq_le,
    .negator = bioseq::pg_types::sql_routines::seq_lt,
    .restrict = bioseq::sql::pg_catalog_reference::scalargesel,
    .join = bioseq::sql::pg_catalog_reference::scalargejoinsel,
})]]
bool seq_ge(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum left,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum right,
    bioseq::postgres::support::function_call_context call)
{
    return seq_compare_internal<true>(left, right, std::addressof(call)) >= 0;
}

/**
 * @brief Test whether the left sequence sorts after the right sequence.
 * Use seq_cmp ordering: unbound values first, then alphabet code and content.
 * @param left First sequence to compare.
 * @param right Second sequence to compare.
 * @return True when seq_cmp(left, right) is positive.
 *
 * ```sql
 * -- Compare two DNA sequences with the > operator and its implementing function seq_gt. Both expressions use alphabet and content ordering.
 * SELECT 'ACGT'::bioseq.seq('dna4', 64) OPERATOR(bioseq.>) 'ACGA'::bioseq.seq('dna4', 64) AS operator_result,
 *   bioseq.seq_gt('ACGT'::bioseq.seq('dna4', 64), 'ACGA'::bioseq.seq('dna4', 64)) AS function_result;
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::operator_({
    .symbol = ">",
    .commutator = bioseq::pg_types::sql_routines::seq_lt,
    .negator = bioseq::pg_types::sql_routines::seq_le,
    .restrict = bioseq::sql::pg_catalog_reference::scalargtsel,
    .join = bioseq::sql::pg_catalog_reference::scalargtjoinsel,
})]]
bool seq_gt(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum left,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum right,
    bioseq::postgres::support::function_call_context call)
{
    return seq_compare_internal<true>(left, right, std::addressof(call)) > 0;
}

[[nodiscard]] inline std::uint64_t seq_hash_internal(
    Datum input,
    std::uint64_t seed)
{
    content_operand operand = describe_content_operand(input);
    if (operand.descriptor.kind == pg_sequence_datum_kind::locator) {
        content_locator_error();
    }
    std::uint64_t result = 0u;
    if (!operand.descriptor.alphabet_bound) {
        result = hash_unbound(
            operand, prefix_hash_seed(seed, 0u, false, 0u));
    } else if (pgseq_is_cigar_alphabet(operand.descriptor.alphabet_code)) {
        const auto hash = with_pg_cigar_sequence<
            std::expected<std::uint64_t, bioseq::cigar_error>>(
            operand.dispatch,
            [seed](auto& backend, auto view) {
                return bioseq::hash_cigar(backend, view, seed);
            });
        if (!hash) pg_raise_cigar_error(hash.error());
        result = *hash;
    } else {
        const std::uint64_t prefixed = prefix_hash_seed(
            seed, 1u, true, operand.descriptor.alphabet_code);
        result = with_pg_sequence_view<std::uint64_t>(
            input,
            [prefixed](auto view) {
                return bioseq::storage::hash_logical_sequence(
                    view, prefixed);
            });
    }
    release_content_operand(operand);
    return result;
}

/**
 * @brief Return a 32-bit hash of sequence alphabet binding and logical content.
 * Hash the binding state, alphabet code when bound, letter ranks or unbound
 * text bytes, and sequence length. Packing width and piece-tree layout are
 * ignored. This uses seed zero and XOR-folds the 64-bit hash into 32 bits.
 * @param input Sequence to hash; unresolved table locators are rejected.
 * @return A signed 32-bit hash consistent with seq_eq.
 *
 * ```sql
 * -- Hash the same sequence twice; equal contents have equal hashes regardless of the expression that constructed them.
 * SELECT bioseq.seq_hash('ACGT'::bioseq.seq('dna4', 64)) =
 *   bioseq.seq_hash(('AC'::bioseq.seq('dna4', 64) OPERATOR(bioseq.||) 'GT'::text));
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
std::int32_t seq_hash(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum input)
{
    const std::uint64_t hash = seq_hash_internal(input, 0u);
    return static_cast<std::int32_t>(
        static_cast<std::uint32_t>(hash ^ (hash >> 32u)));
}

/**
 * @brief Return a seeded 64-bit hash of sequence alphabet binding and logical content.
 * Hash the binding state, alphabet code when bound, letter ranks or unbound
 * text bytes, and sequence length. Packing width and piece-tree layout are ignored.
 * @param input Sequence to hash; unresolved table locators are rejected.
 * @param seed Initial seed, interpreted as a 64-bit bit pattern.
 * @return A signed 64-bit hash; equal sequences have equal hashes for the same seed.
 *
 * ```sql
 * -- Hash a DNA sequence with an explicit seed. Reusing the seed and sequence reproduces the same 64-bit hash.
 * SELECT bioseq.seq_hash_extended('ACGT'::bioseq.seq('dna4', 64), 42::bigint);
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
std::int64_t seq_hash_extended(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum input,
    std::int64_t seed)
{
    return static_cast<std::int64_t>(
        seq_hash_internal(
            input, static_cast<std::uint64_t>(seed)));
}

}  // namespace bioseq::pg_types::sql_routines
