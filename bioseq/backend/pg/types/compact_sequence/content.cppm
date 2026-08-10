/* Total logical comparison and hashing for every valid seq state. */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

#include <bioseq/pg_compat.h>
extern "C" {
#include "utils/expandeddatum.h"
}

export module bioseq.postgres.types.compact_sequence:content;

import bioseq.codegen.sql.annotation_model;
import :flat_abi;
import :tree_types;
import :view_session;
import :packed_output;
import bioseq.storage.lazy_sequence;

namespace {

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
    /* hash_logical_ranks begins at offset^seed. */
    return offset ^ state;
}

} // namespace

[[nodiscard]] inline int seq_compare_internal(
    Datum left_input,
    Datum right_input)
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
    Datum right)
{
    return static_cast<std::int32_t>(
        seq_compare_internal(left, right));
}

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
    Datum right)
{
    return seq_compare_internal(left, right) < 0;
}

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
    Datum right)
{
    return seq_compare_internal(left, right) <= 0;
}

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
    return seq_compare_internal(left, right) == 0;
}

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
    return seq_compare_internal(left, right) != 0;
}

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
    Datum right)
{
    return seq_compare_internal(left, right) >= 0;
}

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
    Datum right)
{
    return seq_compare_internal(left, right) > 0;
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
