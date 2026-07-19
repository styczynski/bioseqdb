/* Datum classification plus the one callback-scoped facade/session adapter
 * used by content operations and the algorithm pipeline. */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>

#include "headers/pg_compat.h"
extern "C" {
#include "utils/expandeddatum.h"
}

export module bioseq.types.pg_compact_sequence:view_session;

import :flat_abi;
import :source_abi;
import :tree_types;
import :tree_ops;
import bioseq.storage.bucket_math;
import bioseq.storage.coordinates;
import bioseq.storage.lazy;

namespace {

[[noreturn]] inline void invalid_flat_sequence(char const* detail)
{
    ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("invalid flat bioseq.seq representation"),
             errdetail("%s", detail)));
    __builtin_unreachable();
}

[[noreturn]] inline void unresolved_locator_error()
{
    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("cannot resolve a bioseq locator outside its owning table scan"),
             errdetail("relation, column, snapshot, and cache ownership are unavailable"),
             errhint("use a Bioseq table scan or materialize the value while that scan is active")));
    __builtin_unreachable();
}

[[nodiscard]] inline std::size_t expected_unbound_size(std::uint64_t length)
{
    if (length > std::numeric_limits<std::size_t>::max() - 7u) {
        invalid_flat_sequence("unbound length overflows size_t");
    }
    return pgseq_t::header_bytes()
        + round_up_to_uint64(static_cast<std::size_t>(length));
}

inline void validate_bound_flat(pgseq_t const& value)
{
    bool valid = false;
    pgseq_t::dispatch_instance(
        static_cast<std::uint8_t>(value.alphabet_code),
        static_cast<std::uint8_t>(value.storage_unit_byte_width),
        [&valid, &value]<class Instance>() {
            using range = compact_sequence_range<Instance>;
            using traits = typename range::traits;
            const std::uint64_t units = value.bp_size == 0u ? 0u
                : (static_cast<std::uint64_t>(value.bp_size)
                   + traits::letters_per_unit - 1u)
                    / traits::letters_per_unit;
            const std::uint64_t padding = units == 0u ? 0u
                : units * traits::letters_per_unit - value.bp_size;
            if (padding != value.padding_letters
                || units > std::numeric_limits<std::size_t>::max()
                    / sizeof(typename traits::storage_unit)) {
                return;
            }
            const std::size_t payload = static_cast<std::size_t>(units)
                * sizeof(typename traits::storage_unit);
            const std::size_t expected = pgseq_t::header_bytes()
                + round_up_to_uint64(payload);
            valid = VARSIZE(&value) == expected;
        });
    if (!valid) {
        invalid_flat_sequence("bound header, padding, or payload size mismatch");
    }
}

} // namespace

export [[nodiscard]] inline pg_sequence_expanded const*
get_pg_sequence_expanded_ro(Datum datum)
{
    if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(datum))) {
        return nullptr;
    }
    ExpandedObjectHeader* header = DatumGetEOHP(datum);
    if (header->eoh_methods != std::addressof(pg_sequence_expanded_methods())) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("invalid expanded representation for bioseq.seq")));
    }
    return reinterpret_cast<pg_sequence_expanded const*>(header);
}

export [[nodiscard]] inline pg_sequence_expanded*
get_pg_sequence_expanded_rw(Datum datum)
{
    if (!VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(datum))) {
        return nullptr;
    }
    return const_cast<pg_sequence_expanded*>(get_pg_sequence_expanded_ro(datum));
}

export [[nodiscard]] inline pg_sequence_descriptor
classify_and_describe_sequence(Datum detoasted_or_expanded)
{
    if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(detoasted_or_expanded))) {
        auto const* expanded = get_pg_sequence_expanded_ro(
            detoasted_or_expanded);
        if (expanded->bp_length > static_cast<std::uint64_t>(INT64_MAX)
            || (expanded->alphabet_bound
                && !is_supported_alphabet_code(
                    expanded->type_params.alphabet_code))) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("invalid expanded bioseq.seq metadata")));
        }
        return {
            .kind = VARATT_IS_EXTERNAL_EXPANDED_RW(
                        DatumGetPointer(detoasted_or_expanded))
                ? pg_sequence_datum_kind::expanded_read_write
                : pg_sequence_datum_kind::expanded_read_only,
            .alphabet_code = expanded->type_params.alphabet_code,
            .storage_unit_byte_width =
                expanded->type_params.storage_unit_byte_width,
            .alphabet_bound = expanded->alphabet_bound,
            .bp_length = expanded->bp_length,
        };
    }

    auto const* flat = reinterpret_cast<pgseq_t const*>(
        DatumGetPointer(detoasted_or_expanded));
    if (flat == nullptr || VARSIZE(flat) < pgseq_t::header_bytes()) {
        invalid_flat_sequence("value is shorter than the pgseq header");
    }
    switch (classify_pg_sequence_flat(flat)) {
        case pg_sequence_flat_kind::bound:
            validate_bound_flat(*flat);
            return {
                .kind = pg_sequence_datum_kind::flat_bound,
                .alphabet_code =
                    static_cast<std::uint8_t>(flat->alphabet_code),
                .storage_unit_byte_width = static_cast<std::uint8_t>(
                    flat->storage_unit_byte_width),
                .alphabet_bound = true,
                .bp_length = flat->bp_size,
            };
        case pg_sequence_flat_kind::unbound: {
            if (flat->padding_letters != 0u
                || VARSIZE(flat) != expected_unbound_size(flat->bp_size)) {
                invalid_flat_sequence("unbound tag, padding, or size mismatch");
            }
            const auto bytes = pgseq_unbound_ascii(*flat);
            for (std::byte raw : bytes) {
                const unsigned char value = std::to_integer<unsigned char>(raw);
                const auto canonical = pgseq_canonical_unbound_symbol(value);
                if (!canonical.valid
                    || static_cast<unsigned char>(canonical.value) != value) {
                    invalid_flat_sequence("unbound payload is not canonical ASCII");
                }
            }
            return {
                .kind = pg_sequence_datum_kind::flat_unbound,
                .alphabet_code = pgseq_unbound_alphabet_code,
                .storage_unit_byte_width = pgseq_unbound_width_tag,
                .alphabet_bound = false,
                .bp_length = flat->bp_size,
            };
        }
        case pg_sequence_flat_kind::locator: {
            if (VARSIZE(flat)
                != pgseq_t::header_bytes()
                    + sizeof(bioseq::storage::seq_locator)) {
                invalid_flat_sequence("locator framing size mismatch");
            }
            const auto payload = std::span<std::byte const>{
                reinterpret_cast<std::byte const*>(flat->data),
                sizeof(bioseq::storage::seq_locator)};
            const auto locator =
                bioseq::storage::decode_seq_locator_payload(payload);
            const std::uint8_t width = static_cast<std::uint8_t>(
                flat->storage_unit_byte_width) & pgseq_physical_width_mask;
            if (!locator.has_value() || locator->source_epoch == 0u
                || locator->total_bp_size
                    > static_cast<std::uint64_t>(INT64_MAX)
                || !is_supported_alphabet_code(
                    static_cast<std::uint8_t>(flat->alphabet_code))
                || !pgseq_supported_storage_unit_byte_widths::
                    contains_runtime(width)) {
                invalid_flat_sequence("locator payload/header is outside the live v3 domain");
            }
            return {
                .kind = pg_sequence_datum_kind::locator,
                .alphabet_code =
                    static_cast<std::uint8_t>(flat->alphabet_code),
                .storage_unit_byte_width = width,
                .alphabet_bound = true,
                .bp_length = locator->total_bp_size,
            };
        }
        case pg_sequence_flat_kind::invalid:
            invalid_flat_sequence("unknown alphabet/width state tag");
    }
    __builtin_unreachable();
}

namespace {

struct pg_sequence_facade_state {
    pg_sequence_read_session* session;
    pg_sequence_descriptor descriptor;
    pgseq_t const* flat;
    piece_edge tree;
};

template<class Alphabet>
[[nodiscard]] inline std::uint8_t complement_rank(std::uint8_t rank)
{
    return static_cast<std::uint8_t>(
        bioseq::storage::lazy_rank_stream_traits<Alphabet>::
            letter_complement(rank));
}

template<class Alphabet>
[[nodiscard]] bool emit_literal_range(
    piece_edge edge,
    std::uint64_t from,
    std::uint64_t to,
    std::span<std::uint8_t> output)
{
    const auto& literal = edge.node->payload.literal;
    if (literal.encoding != literal_encoding::packed) {
        return false;
    }
    bool ok = false;
    pgseq_t::dispatch_instance(
        literal.stored_type_params.alphabet_code,
        literal.stored_type_params.storage_unit_byte_width,
        [&ok, &literal, edge, from, to, output]<class Instance>() {
            if constexpr (std::is_same_v<
                              typename Instance::alphabet_type, Alphabet>) {
                using range = compact_sequence_range<Instance>;
                using traits = typename range::traits;
                using unit = typename traits::storage_unit;
                auto const* units = reinterpret_cast<unit const*>(literal.bytes);
                const bool reverse =
                    bioseq::storage::has_reverse(edge.transform);
                const bool complement =
                    bioseq::storage::has_complement(edge.transform);
                for (std::uint64_t index = from; index < to; ++index) {
                    const std::uint64_t physical = !reverse
                        ? literal.symbol_from + index
                        : literal.symbol_to - 1u - index;
                    unit word = units[physical / traits::letters_per_unit];
                    const std::size_t shift = static_cast<std::size_t>(
                        physical % traits::letters_per_unit)
                        * traits::bits_per_char;
                    std::uint8_t rank = static_cast<std::uint8_t>(
                        (word >> shift) & traits::char_mask);
                    if (complement) {
                        rank = complement_rank<Alphabet>(rank);
                    }
                    output[static_cast<std::size_t>(index - from)] = rank;
                }
                ok = true;
            }
        });
    return ok;
}

template<class Alphabet>
[[nodiscard]] bool emit_tree_range(
    pg_sequence_read_session& session,
    piece_edge edge,
    std::uint64_t from,
    std::uint64_t to,
    std::span<std::uint8_t> output)
{
    if (!edge || from > to || to > edge.node->bp_length
        || output.size() != to - from) {
        return false;
    }
    if (from == to) {
        return true;
    }
    if (edge.node->node_kind == piece_node::kind::branch) {
        const auto children = logical_children(edge);
        const std::uint64_t split = children.first.node->bp_length;
        const std::uint64_t first_from = std::min(from, split);
        const std::uint64_t first_to = std::min(to, split);
        std::size_t emitted = 0u;
        if (first_to > first_from) {
            const std::size_t count = static_cast<std::size_t>(
                first_to - first_from);
            if (!emit_tree_range<Alphabet>(
                    session, children.first, first_from, first_to,
                    output.first(count))) {
                return false;
            }
            emitted = count;
        }
        if (to > split) {
            const std::uint64_t second_from = from > split ? from - split : 0u;
            const std::uint64_t second_to = to - split;
            if (!emit_tree_range<Alphabet>(
                    session, children.second, second_from, second_to,
                    output.subspan(emitted))) {
                return false;
            }
        }
        return true;
    }
    if (edge.node->node_kind == piece_node::kind::literal) {
        return emit_literal_range<Alphabet>(edge, from, to, output);
    }
    const auto& source = edge.node->payload.source;
    const bool reverse = bioseq::storage::has_reverse(edge.transform);
    const std::uint64_t source_from = !reverse
        ? source.from + from
        : source.to - to;
    const auto status = source.source.ops->read_ranks(
        source.source.state, session, source.locator,
        source.stored_type_params, edge.transform, source_from, output);
    return status == pg_sequence_source_read_status::ok;
}

template<class Alphabet>
[[nodiscard]] bool emit_flat_range(
    pgseq_t const& flat,
    std::uint64_t from,
    std::uint64_t to,
    std::span<std::uint8_t> output)
{
    bool ok = false;
    pgseq_t::dispatch_instance(
        static_cast<std::uint8_t>(flat.alphabet_code),
        static_cast<std::uint8_t>(flat.storage_unit_byte_width),
        [&ok, &flat, from, to, output]<class Instance>() {
            if constexpr (std::is_same_v<
                              typename Instance::alphabet_type, Alphabet>) {
                using range = compact_sequence_range<Instance>;
                using traits = typename range::traits;
                using unit = typename traits::storage_unit;
                auto const* units = reinterpret_cast<unit const*>(flat.data);
                for (std::uint64_t position = from; position < to; ++position) {
                    unit word = units[position / traits::letters_per_unit];
                    const std::size_t shift = static_cast<std::size_t>(
                        position % traits::letters_per_unit)
                        * traits::bits_per_char;
                    output[static_cast<std::size_t>(position - from)] =
                        static_cast<std::uint8_t>(
                            (word >> shift) & traits::char_mask);
                }
                ok = true;
            }
        });
    return ok;
}

template<class Alphabet>
[[nodiscard]] std::uint8_t const* facade_ptr_at(
    pg_sequence_facade_state const& state,
    bioseq::storage::sequence_transform transform,
    bioseq::storage::sequence_size_t from,
    std::size_t count,
    std::span<std::uint8_t> output)
{
    if (from > state.descriptor.bp_length
        || count > state.descriptor.bp_length - from) {
        return nullptr;
    }
    const std::uint64_t to = from + count;
    const bool reverse = bioseq::storage::has_reverse(transform);
    const std::uint64_t base_from = !reverse
        ? from : state.descriptor.bp_length - to;
    const std::uint64_t base_to = !reverse
        ? to : state.descriptor.bp_length - from;
    bool ok = state.flat != nullptr
        ? emit_flat_range<Alphabet>(*state.flat, base_from, base_to,
                                   output.first(count))
        : emit_tree_range<Alphabet>(*state.session, state.tree,
                                   base_from, base_to, output.first(count));
    if (!ok) {
        return nullptr;
    }
    if (reverse) {
        std::reverse(output.begin(), output.begin() + count);
    }
    if (bioseq::storage::has_complement(transform)) {
        for (std::size_t index = 0; index < count; ++index) {
            output[index] = complement_rank<Alphabet>(output[index]);
        }
    }
    return output.data();
}

template<class Alphabet, bool IsReversed, bool WriteReversed>
[[nodiscard]] std::uint8_t const* facade_ptr_at_callback(
    void const* opaque,
    bioseq::storage::sequence_size_t from,
    std::size_t count,
    std::span<std::uint8_t> output)
{
    constexpr auto transform = IsReversed
        ? (WriteReversed
            ? bioseq::storage::sequence_transform::complemented
            : bioseq::storage::sequence_transform::reverse_complement)
        : (WriteReversed
            ? bioseq::storage::sequence_transform::reversed
            : bioseq::storage::sequence_transform::identity);
    return facade_ptr_at<Alphabet>(
        *static_cast<pg_sequence_facade_state const*>(opaque),
        transform, from, count, output);
}

[[nodiscard]] inline bioseq::storage::sequence_size_t facade_bp_size(
    void const* opaque) noexcept
{
    return static_cast<pg_sequence_facade_state const*>(opaque)
        ->descriptor.bp_length;
}

[[nodiscard]] inline bool tree_worker_shareable(piece_edge edge) noexcept
{
    if (!edge) {
        return true;
    }
    if (edge.node->node_kind == piece_node::kind::source) {
        auto source = edge.node->payload.source.source;
        return pg_sequence_source_handle_valid(source)
            && source.ops->worker_shareable(source.state);
    }
    if (edge.node->node_kind == piece_node::kind::branch) {
        return tree_worker_shareable(edge.node->payload.branch.left)
            && tree_worker_shareable(edge.node->payload.branch.right);
    }
    return true;
}

[[nodiscard]] inline bool facade_worker_shareable(void const* opaque) noexcept
{
    auto const& state =
        *static_cast<pg_sequence_facade_state const*>(opaque);
    return state.flat != nullptr || tree_worker_shareable(state.tree);
}

template<class Alphabet>
[[nodiscard]] auto facade_resident_probe(
    void const* opaque,
    bioseq::storage::sequence_size_t from,
    bioseq::storage::sequence_size_t to,
    std::uint8_t requested_width) noexcept
    -> typename bioseq::storage::basic_lazy_sequence_view<Alphabet>::
        resident_probe_result
{
    using view = bioseq::storage::basic_lazy_sequence_view<Alphabet>;
    auto const& state =
        *static_cast<pg_sequence_facade_state const*>(opaque);
    if (state.flat == nullptr
        || state.descriptor.storage_unit_byte_width != requested_width) {
        return std::nullopt;
    }
    using descriptor = typename view::resident_packed_region;
    const auto length = to - from;
    if (length == 0u) {
        return descriptor{};
    }
    constexpr std::uint64_t bits =
        bioseq::storage::lazy_rank_stream_traits<Alphabet>::
            logical_bits_per_symbol;
    const std::uint64_t letters_per_unit = requested_width * 8u / bits;
    const std::uint64_t first_unit = from / letters_per_unit;
    const std::uint64_t leading = from % letters_per_unit;
    const std::uint64_t occupied = leading + length;
    const std::uint64_t unit_count = occupied / letters_per_unit
        + (occupied % letters_per_unit != 0u);
    const std::uint64_t trailing =
        unit_count * letters_per_unit - occupied;
    return descriptor{
        .units = state.flat->data + first_unit * requested_width,
        .unit_count = static_cast<std::size_t>(unit_count),
        .bp_length = length,
        .storage_unit_byte_width = requested_width,
        .bits_per_symbol = static_cast<std::uint8_t>(bits),
        .leading_symbol_offset = static_cast<std::uint16_t>(leading),
        .trailing_padding_letters = static_cast<std::uint16_t>(trailing),
        .bit_order = view::packed_bit_order::least_significant_symbol_first,
    };
}

template<class Alphabet>
struct facade_adapter {
    using view = bioseq::storage::basic_lazy_sequence_view<Alphabet>;
    static inline const typename view::backend_ops ops{
        .bp_size = &facade_bp_size,
        .ptr_at = {
            &facade_ptr_at_callback<Alphabet, false, false>,
            &facade_ptr_at_callback<Alphabet, false, true>,
            &facade_ptr_at_callback<Alphabet, true, false>,
            &facade_ptr_at_callback<Alphabet, true, true>,
        },
        .try_resident_packed = &facade_resident_probe<Alphabet>,
        .worker_shareable = &facade_worker_shareable,
    };
};

template<class Alphabet>
inline void release_facade_owner(void* opaque) noexcept
{
    using owner = bioseq::storage::lazy_sequence_call_owner<Alphabet>;
    static_cast<owner*>(opaque)->~owner();
}

inline void release_facade_backing_noop(void*) noexcept {}

} // namespace

export template <sequence_alphabet Alphabet>
[[nodiscard]] inline bioseq::storage::lazy_sequence_call_owner<Alphabet>*
make_pg_sequence_call_owner(
    pg_sequence_read_session& session,
    Datum detoasted_or_expanded,
    pg_sequence_descriptor descriptor)
{
    constexpr std::uint8_t expected_alphabet =
        pgseq_supported_alphabets::template index_of<Alphabet>();
    if (!descriptor.alphabet_bound
        || descriptor.alphabet_code != expected_alphabet
        || descriptor.kind == pg_sequence_datum_kind::locator) {
        if (descriptor.kind == pg_sequence_datum_kind::locator) {
            unresolved_locator_error();
        }
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("operation requires a bound bioseq alphabet")));
    }

    auto* state = static_cast<pg_sequence_facade_state*>(
        MemoryContextAllocZero(session.payload_context,
                               sizeof(pg_sequence_facade_state)));
    state->session = std::addressof(session);
    state->descriptor = descriptor;
    if (descriptor.kind == pg_sequence_datum_kind::flat_bound) {
        state->flat = reinterpret_cast<pgseq_t const*>(
            DatumGetPointer(detoasted_or_expanded));
    } else {
        state->tree = get_pg_sequence_expanded_ro(
            detoasted_or_expanded)->root;
    }

    using adapter = facade_adapter<Alphabet>;
    using owner = bioseq::storage::lazy_sequence_call_owner<Alphabet>;
    auto facade = typename adapter::view{state, std::addressof(adapter::ops)};
    void* storage = MemoryContextAllocZero(
        session.payload_context, sizeof(owner));
    auto* result = ::new (storage) owner{
        facade, state, &release_facade_backing_noop};
    register_pg_sequence_call_owner(
        session, result, &release_facade_owner<Alphabet>);
    return result;
}

export template <typename T>
inline constexpr bool is_borrowed_view_result_v =
    std::ranges::view<std::remove_cvref_t<T>>
    || std::input_or_output_iterator<std::remove_cvref_t<T>>;

export template <typename Result, typename Fn>
Result with_pg_sequence_view(
    FunctionCallInfo fcinfo,
    int argument_index,
    Fn&& view_callback)
{
    static_assert(!std::is_reference_v<Result> && !std::is_pointer_v<Result>);
    static_assert(!is_borrowed_view_result_v<Result>);
    static_assert(std::is_trivially_destructible_v<Result>);

    Datum datum = PG_GETARG_DATUM(argument_index);
    pgseq_t* flat_copy = nullptr;
    Datum dispatch_datum = datum;
    if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(datum))) {
        flat_copy = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(datum));
        dispatch_datum = PointerGetDatum(flat_copy);
    }
    const auto descriptor = classify_and_describe_sequence(dispatch_datum);
    if (descriptor.kind == pg_sequence_datum_kind::locator) {
        unresolved_locator_error();
    }
    if (!descriptor.alphabet_bound) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("operation requires a bound bioseq alphabet")));
    }

    std::optional<Result> result;
    pg_sequence_read_session* volatile session = nullptr;
    PG_TRY();
    {
        auto* local_session = begin_pg_sequence_read_session(
            CurrentMemoryContext);
        session = local_session;
        pgseq_t::dispatch_instance(
            descriptor.alphabet_code,
            descriptor.storage_unit_byte_width,
            [&result, &view_callback, dispatch_datum, descriptor,
             local_session]<class Instance>() {
                using alphabet = typename Instance::alphabet_type;
                auto* owner = make_pg_sequence_call_owner<alphabet>(
                    *local_session, dispatch_datum, descriptor);
                result.emplace(std::forward<Fn>(view_callback)(owner->view()));
            });
        finish_pg_sequence_read_session(local_session, true);
        session = nullptr;
    }
    PG_CATCH();
    {
        abort_pg_sequence_read_session_noexcept(
            const_cast<pg_sequence_read_session*>(session));
        session = nullptr;
        PG_RE_THROW();
    }
    PG_END_TRY();
    if (flat_copy != nullptr) {
        PG_FREE_IF_COPY(flat_copy, argument_index);
    }
    return std::move(*result);
}

export template <typename Result, typename Fn>
Result with_pg_sequence_views(
    FunctionCallInfo fcinfo,
    int left_index,
    int right_index,
    Fn&& callback)
{
    static_assert(!std::is_reference_v<Result> && !std::is_pointer_v<Result>);
    static_assert(!is_borrowed_view_result_v<Result>);
    static_assert(std::is_trivially_destructible_v<Result>);

    Datum datums[2]{PG_GETARG_DATUM(left_index), PG_GETARG_DATUM(right_index)};
    pgseq_t* flat[2]{};
    Datum dispatch[2]{datums[0], datums[1]};
    for (int side = 0; side < 2; ++side) {
        if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(datums[side]))) {
            flat[side] = reinterpret_cast<pgseq_t*>(
                PG_DETOAST_DATUM(datums[side]));
            dispatch[side] = PointerGetDatum(flat[side]);
        }
    }
    const auto left = classify_and_describe_sequence(dispatch[0]);
    const auto right = classify_and_describe_sequence(dispatch[1]);
    if (left.kind == pg_sequence_datum_kind::locator
        || right.kind == pg_sequence_datum_kind::locator) {
        unresolved_locator_error();
    }
    if (!left.alphabet_bound || !right.alphabet_bound
        || left.alphabet_code != right.alphabet_code) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("binary bioseq view operation requires one bound alphabet")));
    }

    std::optional<Result> result;
    pg_sequence_read_session* volatile session = nullptr;
    PG_TRY();
    {
        auto* local_session = begin_pg_sequence_read_session(
            CurrentMemoryContext);
        session = local_session;
        pgseq_t::dispatch_instance(
            left.alphabet_code,
            left.storage_unit_byte_width,
            [&result, &callback, local_session,
             left_dispatch = dispatch[0],
             right_dispatch = dispatch[1], left, right]<class Instance>() {
                using alphabet = typename Instance::alphabet_type;
                auto* left_owner = make_pg_sequence_call_owner<alphabet>(
                    *local_session, left_dispatch, left);
                auto* right_owner = make_pg_sequence_call_owner<alphabet>(
                    *local_session, right_dispatch, right);
                result.emplace(std::forward<Fn>(callback)(
                    left_owner->view(), right_owner->view()));
            });
        finish_pg_sequence_read_session(local_session, true);
        session = nullptr;
    }
    PG_CATCH();
    {
        abort_pg_sequence_read_session_noexcept(
            const_cast<pg_sequence_read_session*>(session));
        session = nullptr;
        PG_RE_THROW();
    }
    PG_END_TRY();
    for (int side = 0; side < 2; ++side) {
        if (flat[side] != nullptr) {
            if (reinterpret_cast<Pointer>(flat[side])
                != DatumGetPointer(datums[side])) {
                pfree(flat[side]);
            }
        }
    }
    return std::move(*result);
}

export struct pg_sequence_rank_value {
    std::uint8_t alphabet_code;
    std::uint8_t rank;
};

/* One-symbol scalar path used by the subscript handler.  It deliberately owns
 * a normal source read session and asks the facade for exactly one rank, so an
 * element lookup never flattens an EOH or reads a whole source fragment. */
export [[nodiscard]] inline pg_sequence_rank_value
read_pg_sequence_rank_at(Datum input, std::uint64_t index)
{
    Datum dispatch = input;
    pgseq_t* flat_copy = nullptr;
    if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(input))) {
        flat_copy = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(input));
        dispatch = PointerGetDatum(flat_copy);
    }
    const auto descriptor = classify_and_describe_sequence(dispatch);
    if (descriptor.kind == pg_sequence_datum_kind::locator) {
        unresolved_locator_error();
    }
    if (index >= descriptor.bp_length) {
        ereport(ERROR,
                (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                 errmsg("bioseq element index is outside its logical sequence")));
    }
    if (!descriptor.alphabet_bound) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("cannot extract bioseq.letter from an unbound sequence"),
                 errhint("cast the sequence as bioseq.seq('alphabet', width) first")));
    }

    pg_sequence_rank_value result{
        .alphabet_code = descriptor.alphabet_code,
        .rank = 0u,
    };
    pg_sequence_read_session* volatile session = nullptr;
    PG_TRY();
    {
        auto* local_session = begin_pg_sequence_read_session(
            CurrentMemoryContext);
        session = local_session;
        pgseq_t::dispatch_instance(
            descriptor.alphabet_code,
            descriptor.storage_unit_byte_width,
            [&result, dispatch, descriptor, index,
             local_session]<class Instance>() {
                using alphabet = typename Instance::alphabet_type;
                auto* owner = make_pg_sequence_call_owner<alphabet>(
                    *local_session, dispatch, descriptor);
                std::array<std::uint8_t, 1> scratch{};
                owner->view().template to_encoded_into<false, false>(
                    std::span<std::uint8_t>{scratch},
                    static_cast<std::size_t>(index),
                    static_cast<std::size_t>(index + 1u));
                result.rank = scratch[0];
            });
        finish_pg_sequence_read_session(local_session, true);
        session = nullptr;
    }
    PG_CATCH();
    {
        abort_pg_sequence_read_session_noexcept(
            const_cast<pg_sequence_read_session*>(session));
        session = nullptr;
        PG_RE_THROW();
    }
    PG_END_TRY();
    if (flat_copy != nullptr
        && reinterpret_cast<Pointer>(flat_copy) != DatumGetPointer(input)) {
        pfree(flat_copy);
    }
    char symbol = '\0';
    if (!pgseq_symbol_for_rank(result.alphabet_code, result.rank, symbol)) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq source emitted an invalid alphabet rank")));
    }
    return result;
}
