/* Total logical comparison and hashing for every valid seq state. */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

#include "headers/pg_compat.h"
#include "fmgr_boundary.h"
extern "C" {
#include "utils/expandeddatum.h"
}

export module bioseq.types.pg_compact_sequence:content;

import :flat_abi;
import :tree_types;
import :view_session;
import :packed_output;
import bioseq.storage.lazy;

namespace {

struct content_operand {
    Datum dispatch;
    pgseq_t* flat;
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
    auto* flat = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(input));
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
    /* sequence_logical_hash_bytes_v1 begins at offset^seed. */
    return offset ^ state;
}

} // namespace

export [[nodiscard]] inline int seq_compare_internal(FunctionCallInfo fcinfo)
{
    content_operand left = describe_content_operand(PG_GETARG_DATUM(0));
    content_operand right = describe_content_operand(PG_GETARG_DATUM(1));
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
    } else {
        result = with_pg_sequence_views<int>(
            fcinfo, 0, 1,
            [](auto left_view, auto right_view) {
                return bioseq::storage::compare_sequence_views(
                    left_view, right_view);
            });
    }
    release_content_operand(left);
    release_content_operand(right);
    return result;
}

export inline Datum seq_cmp_impl(FunctionCallInfo fcinfo)
{
    PG_RETURN_INT32(seq_compare_internal(fcinfo));
}

export inline Datum seq_lt_impl(FunctionCallInfo fcinfo)
{
    PG_RETURN_BOOL(seq_compare_internal(fcinfo) < 0);
}

export inline Datum seq_le_impl(FunctionCallInfo fcinfo)
{
    PG_RETURN_BOOL(seq_compare_internal(fcinfo) <= 0);
}

export inline Datum seq_eq_impl(FunctionCallInfo fcinfo)
{
    PG_RETURN_BOOL(seq_compare_internal(fcinfo) == 0);
}

export inline Datum seq_ne_impl(FunctionCallInfo fcinfo)
{
    PG_RETURN_BOOL(seq_compare_internal(fcinfo) != 0);
}

export inline Datum seq_ge_impl(FunctionCallInfo fcinfo)
{
    PG_RETURN_BOOL(seq_compare_internal(fcinfo) >= 0);
}

export inline Datum seq_gt_impl(FunctionCallInfo fcinfo)
{
    PG_RETURN_BOOL(seq_compare_internal(fcinfo) > 0);
}

export [[nodiscard]] inline std::uint64_t seq_hash_internal(
    FunctionCallInfo fcinfo,
    std::uint64_t seed)
{
    content_operand operand = describe_content_operand(PG_GETARG_DATUM(0));
    if (operand.descriptor.kind == pg_sequence_datum_kind::locator) {
        content_locator_error();
    }
    std::uint64_t result = 0u;
    if (!operand.descriptor.alphabet_bound) {
        result = hash_unbound(
            operand, prefix_hash_seed(seed, 0u, false, 0u));
    } else {
        const std::uint64_t prefixed = prefix_hash_seed(
            seed, 1u, true, operand.descriptor.alphabet_code);
        result = with_pg_sequence_view<std::uint64_t>(
            fcinfo, 0,
            [prefixed](auto view) {
                return bioseq::storage::sequence_logical_hash_v1(
                    view, prefixed);
            });
    }
    release_content_operand(operand);
    return result;
}

export inline Datum seq_hash_impl(FunctionCallInfo fcinfo)
{
    const std::uint64_t hash = seq_hash_internal(fcinfo, 0u);
    PG_RETURN_INT32(static_cast<std::int32_t>(
        static_cast<std::uint32_t>(hash ^ (hash >> 32u))));
}

export inline Datum seq_hash_extended_impl(FunctionCallInfo fcinfo)
{
    const auto seed = static_cast<std::uint64_t>(PG_GETARG_INT64(1));
    PG_RETURN_INT64(static_cast<std::int64_t>(
        seq_hash_internal(fcinfo, seed)));
}

extern "C" {
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_cmp);
PGDLLEXPORT Datum pg_compact_sequence_cmp(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_cmp_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_lt);
PGDLLEXPORT Datum pg_compact_sequence_lt(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_lt_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_le);
PGDLLEXPORT Datum pg_compact_sequence_le(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_le_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_eq);
PGDLLEXPORT Datum pg_compact_sequence_eq(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_eq_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_ne);
PGDLLEXPORT Datum pg_compact_sequence_ne(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_ne_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_ge);
PGDLLEXPORT Datum pg_compact_sequence_ge(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_ge_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_gt);
PGDLLEXPORT Datum pg_compact_sequence_gt(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_gt_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(seq_hash);
PGDLLEXPORT Datum seq_hash(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_hash_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(seq_hash_extended);
PGDLLEXPORT Datum seq_hash_extended(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_hash_extended_impl, fcinfo);
}
} // extern "C"
