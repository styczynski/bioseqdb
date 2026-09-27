/**
 * @file packing.cppm
 * Convert sequence trees to flat values and manage trees used by SQL sequence edits.
 */
module;

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

#include <bioseq/pg_compat.h>
extern "C" {
#include "storage/shmem.h"
#include "utils/datum.h"
#include "utils/expandeddatum.h"
}

export module bioseq.postgres.types.compact_sequence:packed_output_packing;

import :packed_output_materialization_trace;
import :flat_abi;
import :source_abi;
import :tree_types;
import :tree_ops;
import :view_session;
import bioseq.postgres.support;
import bioseq.sequences.rank_pack;
import bioseq.storage.coordinates;
import bioseq.storage.lazy_sequence;
import bioseq.storage.layout;

/// Compute an unsigned ceiling division and reject a zero divisor.
export [[nodiscard]] inline std::uint64_t checked_ceil_div(
    std::uint64_t numerator,
    std::uint64_t denominator)
{
    if (denominator == 0u) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("zero divisor in bioseq size calculation")));
    }
    return numerator / denominator + (numerator % denominator != 0u);
}

/// Convert a logical byte count to PostgreSQL Size without truncation.
export [[nodiscard]] inline Size checked_size_cast(std::uint64_t value)
{
    if (value > std::numeric_limits<Size>::max()) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq size does not fit PostgreSQL Size")));
    }
    return static_cast<Size>(value);
}

/// Multiply allocation sizes and report overflow as a PostgreSQL error.
export [[nodiscard]] inline Size checked_mul_size(Size left, Size right)
{
    return mul_size(left, right);
}

/// Add allocation sizes and report overflow as a PostgreSQL error.
export [[nodiscard]] inline Size checked_add_size(Size left, Size right)
{
    return add_size(left, right);
}

/* Check the public allocation limit before calling palloc or growing an
 * output buffer. Oversized sequences should report PROGRAM_LIMIT_EXCEEDED.
 * Compare the uint64 payload before narrowing it so smaller Size types cannot
 * truncate an invalid request into an apparently valid allocation. */
/// Check the total output allocation, including caller-supplied overhead.
export [[nodiscard]] inline Size checked_materialization_allocation_size(
    std::uint64_t payload_bytes,
    Size overhead_bytes,
    char const* target)
{
    if (overhead_bytes > MaxAllocSize
        || payload_bytes > static_cast<std::uint64_t>(
            MaxAllocSize - overhead_bytes)) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq %s materialization exceeds MaxAllocSize",
                        target)));
    }
    return overhead_bytes + static_cast<Size>(payload_bytes);
}

/// Round a byte count up to uint64 alignment without overflowing Size.
export [[nodiscard]] inline Size checked_round_up_to_uint64(Size value)
{
    constexpr Size alignment = sizeof(std::uint64_t);
    if (value > std::numeric_limits<Size>::max() - (alignment - 1u)) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq aligned payload size overflow")));
    }
    return round_up_to_uint64(value);
}

/// Compute a flat sequence's header and aligned packed data size, checking limits.
export template<class p_instance>
[[nodiscard]] inline Size bound_flat_size(std::uint64_t bp_length)
{
    using range = compact_sequence_range<p_instance>;
    using traits = typename range::traits;
    if (bp_length > std::numeric_limits<bioseq::pg_types::seq::bp_size_t>::max()) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq sequence is too long to flatten")));
    }
    const auto units = checked_ceil_div(
        bp_length, traits::letters_per_unit);
    if (units > std::numeric_limits<std::uint32_t>::max()) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq packed unit count exceeds uint32")));
    }
    const Size payload = checked_mul_size(
        checked_size_cast(units), sizeof(typename traits::storage_unit));
    const Size total = checked_materialization_allocation_size(
        checked_round_up_to_uint64(payload), bioseq::pg_types::seq::header_bytes(),
        "flat sequence");
    return total;
}

namespace {

[[nodiscard]] inline Size expanded_flat_size(pg_sequence_expanded const& value)
{
    if (!value.alphabet_bound) {
        if (value.bp_length
            > std::numeric_limits<bioseq::pg_types::seq::bp_size_t>::max()) {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("unbound bioseq sequence is too long to flatten")));
        }
        const Size total = checked_materialization_allocation_size(
            checked_round_up_to_uint64(checked_size_cast(value.bp_length)),
            bioseq::pg_types::seq::header_bytes(), "flat unbound sequence");
        return total;
    }
    Size size = 0u;
    bioseq::pg_types::seq::dispatch_instance(
        value.type_params.alphabet_code,
        value.type_params.storage_unit_byte_width,
        [&size, &value]<class p_instance>() {
            size = bound_flat_size<p_instance>(value.bp_length);
        });
    return size;
}

/// Copy an unbound tree interval [from, to) into an exactly sized byte span.
[[nodiscard]] bool emit_unbound_ascii(
    piece_edge edge,
    std::uint64_t from,
    std::uint64_t to,
    std::span<std::byte> output)
{
    if (from > to || output.size() != to - from) {
        return false;
    }
    if (!edge) {
        return from == 0 && to == 0 && output.empty();
    }
    if (to > edge.node->bp_length
        || bioseq::storage::is_complemented(edge.transform)) {
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
            emitted = static_cast<std::size_t>(first_to - first_from);
            if (!emit_unbound_ascii(
                    children.first, first_from, first_to,
                    output.first(emitted))) {
                return false;
            }
        }
        if (to > split) {
            return emit_unbound_ascii(
                children.second,
                from > split ? from - split : 0u,
                to - split,
                output.subspan(emitted));
        }
        return true;
    }
    if (edge.node->node_kind != piece_node::kind::literal) {
        return false;
    }
    const auto& literal = edge.node->payload.literal;
    if (literal.encoding != literal_encoding::unbound_ascii) {
        return false;
    }
    const bool reverse = bioseq::storage::is_reversed(edge.transform);
    for (std::uint64_t index = from; index < to; ++index) {
        const std::uint64_t physical = !reverse
            ? literal.symbol_from + index
            : literal.symbol_to - 1u - index;
        output[static_cast<std::size_t>(index - from)] =
            literal.bytes[physical];
    }
    return true;
}

[[noreturn]] inline void rank_pack_error(
    bioseq::sequences::rank_pack_status status)
{
    using bioseq::sequences::rank_pack_status;
    switch (status) {
        case rank_pack_status::invalid_rank:
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq tree emitted an invalid alphabet rank")));
            __builtin_unreachable();
        case rank_pack_status::capacity_overflow:
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("bioseq packed output capacity overflow")));
            __builtin_unreachable();
        case rank_pack_status::length_mismatch:
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq packed output length mismatch")));
            __builtin_unreachable();
        case rank_pack_status::unit_count_mismatch:
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq packed output unit-count mismatch")));
            __builtin_unreachable();
        case rank_pack_status::ok:
            break;
    }
    __builtin_unreachable();
}

/// Pack the view's letters into the caller's flat sequence buffer.
template<class p_instance, class p_view>
void pack_view_into(bioseq::pg_types::seq& destination, p_view view, std::uint64_t length)
{
    using range = compact_sequence_range<p_instance>;
    using traits = typename range::traits;
    using unit = typename traits::storage_unit;
    const std::uint64_t unit_count = checked_ceil_div(
        length, traits::letters_per_unit);
    auto units = std::span<unit>{
        reinterpret_cast<unit*>(destination.data),
        static_cast<std::size_t>(unit_count)};
    using sink = bioseq::sequences::fixed_rank_pack_unit_sink<unit>;
    using writer_type = bioseq::sequences::checked_rank_pack_sink<
        p_instance,
        bioseq::sequences::invalid_rank_policy::reject,
        bioseq::sequences::pack_capacity_policy::fixed_span,
        sink>;
    writer_type writer{sink{units}};
    bioseq::storage::stream_transformed_ranks<
        bioseq::storage::sequence_transform::identity>(
            view, 0u, length, writer);
    const auto result = writer.finish(length, unit_count);
    if (!result) {
        rank_pack_error(result.status);
    }
    destination.alphabet_code =
        pgseq_supported_alphabets::template index_of<
            typename p_instance::alphabet_type>();
    destination.storage_unit_byte_width = p_instance::byte_width;
    destination.padding_letters = result.padding_letters;
    destination.bp_size = static_cast<bioseq::pg_types::seq::bp_size_t>(length);
}

inline void pg_sequence_expanded_reset(void* argument) noexcept
{
    auto* value = static_cast<pg_sequence_expanded*>(argument);
    if (value == nullptr) {
        return;
    }
    piece_edge root = value->root;
    value->root = {};
    release_edge(root);
    auto* trace = value->trace;
    value->trace = nullptr;
    release_slice_trace_attachment(trace);
}

} // namespace

/// Copy an unbound tree interval as ASCII in its logical letter order.
export [[nodiscard]] inline bool emit_pg_unbound_ascii(
    piece_edge root,
    std::uint64_t from,
    std::uint64_t to,
    std::span<std::byte> output)
{
    return emit_unbound_ascii(root, from, to, output);
}

/// Return the exact aligned flat varlena size required by an expanded value.
export [[nodiscard]] inline Size pg_sequence_get_flat_size(
    ExpandedObjectHeader* header)
{
    auto* value = reinterpret_cast<pg_sequence_expanded*>(header);
    return expanded_flat_size(*value);
}

/// Write an expanded sequence into caller-provided flat PostgreSQL storage.
export inline void pg_sequence_flatten_into(
    ExpandedObjectHeader* header,
    void* result,
    Size allocated_size)
{
    auto& value = *reinterpret_cast<pg_sequence_expanded*>(header);
    /* Concatenation checks the new path through the balanced tree. Before
     * writing a flat value, validate the whole tree as well. Reading all its
     * letters already requires work proportional to the sequence length. */
    validate_tree_and_limits(
        value.root, value.type_params, value.bp_length,
        value.alphabet_bound);
    const Size expected = expanded_flat_size(value);
    if (allocated_size != expected) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq EOH flatten size changed between callbacks")));
    }
    std::memset(result, 0, allocated_size);
    auto& destination = *static_cast<bioseq::pg_types::seq*>(result);
    SET_VARSIZE(&destination, allocated_size);

    if (!value.alphabet_bound) {
        destination.alphabet_code = pgseq_unbound_alphabet_code;
        destination.storage_unit_byte_width = pgseq_unbound_width_tag;
        destination.padding_letters = 0u;
        destination.bp_size = static_cast<bioseq::pg_types::seq::bp_size_t>(value.bp_length);
        if (!emit_unbound_ascii(
                value.root, 0u, value.bp_length,
                std::span<std::byte>{
                    reinterpret_cast<std::byte*>(destination.data),
                    static_cast<std::size_t>(value.bp_length)})) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("unbound bioseq tree cannot be flattened")));
        }
#ifdef BIOSEQ_TEST_INDEXES
        note_slice_payload_allocation_test_event_noexcept(
            materialization_mechanism::packed_full,
            materialization_reason::executor_copy);
#endif
        const auto observer = make_slice_payload_allocation_observer_noexcept(
            value.trace, value.producer_value_id,
            materialization_mechanism::packed_full,
            materialization_reason::executor_copy);
        (void)bioseq::storage::publish_sequence_allocation(
            std::addressof(observer),
            bioseq::storage::sequence_payload_allocation_kind::flat_sequence,
            value.bp_length, value.bp_length,
            static_cast<std::uint64_t>(allocated_size),
            static_cast<std::uint64_t>(allocated_size));
        return;
    }

    pg_sequence_descriptor descriptor{
        .kind = pg_sequence_datum_kind::expanded_read_only,
        .alphabet_code = value.type_params.alphabet_code,
        .storage_unit_byte_width = value.type_params.storage_unit_byte_width,
        .alphabet_bound = true,
        .bp_length = value.bp_length,
    };
    ErrorData* callback_exact_error = nullptr;
    bool callback_standard_error = false;
    bool callback_unknown_error = false;
    char callback_detail[512] = "std::exception without detail";
    pg_sequence_read_session* volatile session = nullptr;
    PG_TRY();
    {
        auto* local_session = begin_pg_sequence_read_session(
            CurrentMemoryContext);
        session = local_session;
        try {
            bioseq::pg_types::seq::dispatch_instance(
                value.type_params.alphabet_code,
                value.type_params.storage_unit_byte_width,
                [&destination, &value, local_session, header,
                 descriptor]<class p_instance>() {
                    using alphabet = typename p_instance::alphabet_type;
                    auto* owner = make_pg_sequence_call_owner<alphabet>(
                        *local_session, EOHPGetRODatum(header), descriptor);
                    pack_view_into<p_instance>(
                        destination, owner->view(), value.bp_length);
                });
            finish_pg_sequence_read_session(local_session, true);
        } catch (bioseq::postgres::support::postgres_error& error) {
            callback_exact_error = error.release_error_data();
        } catch (std::exception const& error) {
            strlcpy(callback_detail,
                    error.what() == nullptr
                        ? "std::exception without detail"
                        : error.what(),
                    sizeof(callback_detail));
            callback_standard_error = true;
        } catch (...) {
            callback_unknown_error = true;
        }
        if (callback_exact_error != nullptr || callback_standard_error
            || callback_unknown_error) {
            abort_pg_sequence_read_session_noexcept(local_session);
        }
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
    if (callback_exact_error != nullptr) {
        ReThrowError(callback_exact_error);
    }
    if (callback_standard_error || callback_unknown_error) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq sequence callback raised a C++ exception"),
                 errdetail("%s", callback_unknown_error
                         ? "unknown non-standard C++ exception"
                         : callback_detail)));
    }
#ifdef BIOSEQ_TEST_INDEXES
    note_slice_payload_allocation_test_event_noexcept(
        materialization_mechanism::packed_full,
        materialization_reason::executor_copy);
#endif
    const auto observer = make_slice_payload_allocation_observer_noexcept(
        value.trace, value.producer_value_id,
        materialization_mechanism::packed_full,
        materialization_reason::executor_copy);
    (void)bioseq::storage::publish_sequence_allocation(
        std::addressof(observer),
        bioseq::storage::sequence_payload_allocation_kind::flat_sequence,
        value.bp_length, value.bp_length,
        static_cast<std::uint64_t>(allocated_size),
        static_cast<std::uint64_t>(allocated_size));
}

/// Return the single ExpandedObjectMethods table for compact sequences.
export [[nodiscard, gnu::used, gnu::noinline]] ExpandedObjectMethods const&
pg_sequence_expanded_methods() noexcept
{
    static const ExpandedObjectMethods methods{
        &pg_sequence_get_flat_size,
        &pg_sequence_flatten_into,
    };
    return methods;
}

/// Create an empty expanded sequence with its own child memory context.
export [[nodiscard]] inline pg_sequence_expanded* make_pg_sequence_expanded(
    compact_sequence_stdc_type_params params,
    bool alphabet_bound)
{
    MemoryContext object_context = AllocSetContextCreate(
        CurrentMemoryContext, "bioseq lazy sequence", ALLOCSET_SMALL_SIZES);
    auto* result = static_cast<pg_sequence_expanded*>(
        MemoryContextAllocZero(object_context, sizeof(pg_sequence_expanded)));
    result->object_context = object_context;
    result->type_params = params;
    result->alphabet_bound = alphabet_bound;
    EOH_init_header(
        &result->hdr, &pg_sequence_expanded_methods(), object_context);
    result->reset_callback.func = pg_sequence_expanded_reset;
    result->reset_callback.arg = result;
    MemoryContextRegisterResetCallback(
        object_context, &result->reset_callback);
    return result;
}

/// Return the writable expanded-object Datum for a compact sequence.
export [[nodiscard]] inline Datum pg_sequence_rw_datum(
    pg_sequence_expanded* value) noexcept
{
    return EOHPGetRWDatum(&value->hdr);
}

/// Return the read-only expanded-object Datum for a compact sequence.
export [[nodiscard]] inline Datum pg_sequence_ro_datum(
    pg_sequence_expanded* value) noexcept
{
    return EOHPGetRODatum(&value->hdr);
}

/// Borrow the trace attachment from an expanded sequence, or return nullptr.
export [[nodiscard]] inline slice_trace_attachment*
pg_sequence_trace_attachment_for_datum(Datum input)
{
    auto const* expanded = get_pg_sequence_expanded_ro(input);
    return expanded == nullptr ? nullptr : expanded->trace;
}

/// Cleanup state for a new root until it is installed in an expanded sequence.
export struct pending_root_cleanup {
    MemoryContextCallback callback;
    piece_tree_arena* destination_arena;
    piece_edge_cleanup_ledger build_edges;
    piece_edge root;
    piece_edge_cleanup_handle root_cleanup;
    bool armed;
};

/// Release the unfinished root and its tracked allocations when its context resets.
export inline void pending_root_reset(void* argument) noexcept
{
    auto* guard = static_cast<pending_root_cleanup*>(argument);
    if (guard == nullptr || !guard->armed) {
        return;
    }
    guard->armed = false;
    release_piece_edge_cleanup_ledger_noexcept(guard->build_edges);
    release_edge(guard->root);
    guard->root = {};
    release_arena(guard->destination_arena);
    guard->destination_arena = nullptr;
}

/// Call a root builder that returns an edge registered for cleanup.
template<class p_build>
[[nodiscard]] inline tracked_piece_edge invoke_piece_root_build(
    p_build&& build,
    piece_tree_arena& arena,
    piece_edge_cleanup_ledger& ledger)
{
    /* Register every new owning edge with the reset callback before another
     * PostgreSQL operation can fail. The builder returns the edge together
     * with its cleanup handle. */
    tracked_piece_edge built = std::forward<p_build>(build)(arena, ledger);
    return built;
}

/// Move the completed root out of its construction cleanup list.
inline void transfer_built_root_from_ledger(
    pending_root_cleanup& guard)
{
    if (guard.root_cleanup.generation != 0u) {
        transfer_piece_edge_cleanup_root_noexcept(
            guard.build_edges, guard.root_cleanup, guard.root);
        guard.root_cleanup = {};
    }
    if (!piece_edge_cleanup_ledger_empty_noexcept(guard.build_edges)) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq concat construction left unowned edge tokens"),
                 errdetail("token_count=%u",
                           static_cast<unsigned>(
                               guard.build_edges.token_count))));
    }
}

/// Build and validate a replacement tree before changing the current root.
/// A PostgreSQL error releases the new tree and leaves the old value intact.
export template<class p_build>
inline void replace_root_error_safe(
    pg_sequence_expanded& value,
    compact_sequence_stdc_type_params type,
    std::uint64_t length,
    bool alphabet_bound,
    p_build&& build)
{
    MemoryContext build_context = AllocSetContextCreate(
        CurrentMemoryContext, "bioseq piece build", ALLOCSET_SMALL_SIZES);
    MemoryContext saved_context = CurrentMemoryContext;
    PG_TRY();
    {
        MemoryContextSwitchTo(build_context);
        auto* guard = static_cast<pending_root_cleanup*>(
            palloc0(sizeof(pending_root_cleanup)));
        guard->callback.func = pending_root_reset;
        guard->callback.arg = guard;
        guard->armed = true;
        MemoryContextRegisterResetCallback(
            build_context, &guard->callback);
        MemoryContextSwitchTo(saved_context);

        /* Tree storage has its own lifetime so another expanded value can keep
         * sharing it after this value's memory context is reset. */
        guard->destination_arena = make_piece_tree_arena();
        const tracked_piece_edge built = invoke_piece_root_build(
            std::forward<p_build>(build), *guard->destination_arena,
            guard->build_edges);
        guard->root = built.edge;
        guard->root_cleanup = built.cleanup;
        transfer_built_root_from_ledger(*guard);
        validate_tree_and_limits(
            guard->root, type, length, alphabet_bound);

        piece_edge old = value.root;
        publish_root_noexcept(
            value, guard->root, type, length, alphabet_bound);
        guard->root = {};
        release_arena(guard->destination_arena);
        guard->destination_arena = nullptr;
        guard->armed = false;
        MemoryContextDelete(build_context);
        release_edge(old);
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        MemoryContextDelete(build_context);
        PG_RE_THROW();
    }
    PG_END_TRY();
}

/* Both input trees are immutable and already validated. A concatenation
 * copies only their joining path, so check those new nodes in O(log pieces).
 * Install the root after validation and keep the same cleanup guarantees as
 * replace_root_error_safe. */
/// Install a joined root after validating the newly created tree path.
template<class p_build>
inline void replace_root_trusted_join_error_safe(
    pg_sequence_expanded& value,
    compact_sequence_stdc_type_params type,
    std::uint64_t length,
    bool alphabet_bound,
    piece_edge trusted_left,
    std::uint64_t left_length,
    piece_edge trusted_right,
    std::uint64_t right_length,
    p_build&& build)
{
    MemoryContext build_context = AllocSetContextCreate(
        CurrentMemoryContext, "bioseq trusted join build", ALLOCSET_SMALL_SIZES);
    MemoryContext saved_context = CurrentMemoryContext;
    PG_TRY();
    {
        MemoryContextSwitchTo(build_context);
        auto* guard = static_cast<pending_root_cleanup*>(
            palloc0(sizeof(pending_root_cleanup)));
        guard->callback.func = pending_root_reset;
        guard->callback.arg = guard;
        guard->armed = true;
        MemoryContextRegisterResetCallback(
            build_context, &guard->callback);
        MemoryContextSwitchTo(saved_context);

        guard->destination_arena = make_piece_tree_arena();
        const tracked_piece_edge built = invoke_piece_root_build(
            std::forward<p_build>(build), *guard->destination_arena,
            guard->build_edges);
        guard->root = built.edge;
        guard->root_cleanup = built.cleanup;
        transfer_built_root_from_ledger(*guard);
        (void)validate_trusted_join_and_limits(
            *guard->destination_arena, guard->root,
            trusted_left, left_length, trusted_right, right_length,
            type, length, alphabet_bound);

        piece_edge old = value.root;
        publish_root_noexcept(
            value, guard->root, type, length, alphabet_bound);
        guard->root = {};
        release_arena(guard->destination_arena);
        guard->destination_arena = nullptr;
        guard->armed = false;
        MemoryContextDelete(build_context);
        release_edge(old);
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        MemoryContextDelete(build_context);
        PG_RE_THROW();
    }
    PG_END_TRY();
}

namespace {

[[nodiscard]] inline literal_piece copy_flat_literal(
    piece_tree_arena& arena,
    bioseq::pg_types::seq const& flat,
    pg_sequence_descriptor descriptor,
    slice_trace_attachment* trace,
    std::uint32_t producer_value_id)
{
    const bool unbound = !descriptor.alphabet_bound;
    const std::size_t count = unbound
        ? static_cast<std::size_t>(descriptor.bp_length)
        : static_cast<std::size_t>(bioseq::postgres::support::varlena_4byte_size(&flat)) - bioseq::pg_types::seq::header_bytes();
    const std::size_t allocation_bytes = std::max<std::size_t>(count, 1u);
    auto* bytes = allocate_piece_literal_storage(
        arena, allocation_bytes);
    if (count != 0u) {
        std::memcpy(bytes, flat.data, count);
    }
#ifdef BIOSEQ_TEST_INDEXES
    note_slice_payload_allocation_test_event_noexcept(
        materialization_mechanism::backing_copy,
        materialization_reason::executor_copy);
#endif
    const auto observer = make_slice_payload_allocation_observer_noexcept(
        trace, producer_value_id,
        materialization_mechanism::backing_copy,
        materialization_reason::executor_copy);
    (void)bioseq::storage::publish_sequence_allocation(
        std::addressof(observer),
        bioseq::storage::sequence_payload_allocation_kind::backing_copy,
        descriptor.bp_length, descriptor.bp_length,
        static_cast<std::uint64_t>(allocation_bytes),
        static_cast<std::uint64_t>(allocation_bytes));
    return {
        .storage_arena = std::addressof(arena),
        .bytes = bytes,
        .symbol_from = 0u,
        .symbol_to = descriptor.bp_length,
        .stored_type_params = unbound
            ? compact_sequence_stdc_type_params{
                .alphabet_code = pgseq_unbound_alphabet_code,
                .storage_unit_byte_width = pgseq_unbound_width_tag,
              }
            : flat.type_parameters(),
        .encoding = unbound
            ? literal_encoding::unbound_ascii
            : literal_encoding::packed,
        .validation = unbound
            ? literal_validation::unvalidated
            : literal_validation::alphabet_validated,
    };
}

/// Keep trace attribution when the inputs provide one unambiguous producer.
inline void reconcile_concat_trace_provenance_noexcept(
    pg_sequence_expanded& result,
    pg_sequence_expanded const& right) noexcept
{
    const bool left_live = slice_trace_attachment_live(result.trace)
        && result.producer_value_id != 0u;
    const bool right_live = slice_trace_attachment_live(right.trace)
        && right.producer_value_id != 0u;

    slice_trace_attachment* selected_trace = nullptr;
    std::uint32_t selected_producer = 0u;
    if (left_live && !right_live) {
        selected_trace = result.trace;
        selected_producer = result.producer_value_id;
    } else if (!left_live && right_live) {
        selected_trace = right.trace;
        selected_producer = right.producer_value_id;
    } else if (left_live && right_live
               && result.trace == right.trace
               && result.producer_value_id == right.producer_value_id) {
        selected_trace = result.trace;
        selected_producer = result.producer_value_id;
    }
    /* Two different producers need a new result ID. Clear their attribution
     * here so BioseqSliceProject can attach the planned result after evaluation. */
    if (result.trace != selected_trace) {
        retain_slice_trace_attachment(selected_trace);
        auto* previous = result.trace;
        result.trace = selected_trace;
        release_slice_trace_attachment(previous);
    }
    result.producer_value_id = selected_producer;
}

} // namespace

/// Obtain a writable expanded sequence, reusing a writable input when possible.
/// A read-only tree is shared through a new wrapper. Flat input is copied into
/// a tree-owned literal so the result outlives the input buffer.
export [[nodiscard]] inline acquired_sequence acquire_writable_pg_sequence(
    Datum input,
    slice_trace_attachment const* trace = nullptr,
    std::uint32_t producer_value_id = 0u)
{
    if (auto* rw = get_pg_sequence_expanded_rw(input); rw != nullptr) {
        if (trace != nullptr && rw->trace != trace) {
            auto* replacement = const_cast<slice_trace_attachment*>(trace);
            retain_slice_trace_attachment(replacement);
            auto* previous = rw->trace;
            rw->trace = replacement;
            release_slice_trace_attachment(previous);
        }
        if (producer_value_id != 0u) {
            rw->producer_value_id = producer_value_id;
        }
        return {rw, true};
    }
    if (auto const* ro = get_pg_sequence_expanded_ro(input); ro != nullptr) {
        auto* copy = make_pg_sequence_expanded(
            ro->type_params, ro->alphabet_bound);
        copy->bp_length = ro->bp_length;
        copy->root = retain_edge(ro->root);
        copy->producer_value_id = producer_value_id != 0u
            ? producer_value_id : ro->producer_value_id;
        copy->trace = const_cast<slice_trace_attachment*>(
            trace != nullptr ? trace : ro->trace);
        retain_slice_trace_attachment(copy->trace);
        return {copy, false};
    }

    auto* flat = reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(input));
    const Datum flat_datum = PointerGetDatum(flat);
    const auto descriptor = classify_and_describe_sequence(flat_datum);
    if (descriptor.kind == pg_sequence_datum_kind::locator) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cannot resolve a bioseq locator outside its owning table scan"),
                 errdetail("relation, column, snapshot, and cache ownership are unavailable")));
    }
    const compact_sequence_stdc_type_params type = descriptor.alphabet_bound
        ? flat->type_parameters()
        : compact_sequence_stdc_type_params{
            .alphabet_code = pgseq_unbound_alphabet_code,
            .storage_unit_byte_width = pgseq_unbound_width_tag,
          };
    auto* result = make_pg_sequence_expanded(type, descriptor.alphabet_bound);
    result->trace = const_cast<slice_trace_attachment*>(trace);
    result->producer_value_id = producer_value_id;
    retain_slice_trace_attachment(result->trace);
    replace_root_error_safe(
        *result, type, descriptor.bp_length, descriptor.alphabet_bound,
        [flat, descriptor, result](
            piece_tree_arena& arena,
            piece_edge_cleanup_ledger& ledger) {
            return make_literal_node_error_atomic(
                arena, ledger,
                copy_flat_literal(
                    arena, *flat, descriptor,
                    result->trace, result->producer_value_id));
        });
    if (reinterpret_cast<Pointer>(flat) != DatumGetPointer(input)) {
        pfree(flat);
    }
    return {result, false};
}

/// Obtain a read handle without taking ownership of the input.
/// Expanded input is borrowed. Flat input is copied into an expanded value
/// owned by the current memory context.
export [[nodiscard]] inline pg_sequence_read_handle acquire_sequence_for_read(
    Datum input,
    slice_trace_attachment const* trace = nullptr,
    std::uint32_t producer_value_id = 0u)
{
    if (auto const* expanded = get_pg_sequence_expanded_ro(input);
        expanded != nullptr) {
        return {
            .backing = expanded,
            .root_value = expanded->root,
            .type_params = expanded->type_params,
            .logical_length = expanded->bp_length,
            .alphabet_bound = expanded->alphabet_bound,
        };
    }
    acquired_sequence adopted = acquire_writable_pg_sequence(
        input, trace, producer_value_id);
    return {
        .backing = adopted.value,
        .root_value = adopted.value->root,
        .type_params = adopted.value->type_params,
        .logical_length = adopted.value->bp_length,
        .alphabet_bound = adopted.value->alphabet_bound,
    };
}

/// Add sequence lengths, rejecting results beyond PostgreSQL bigint.
export [[nodiscard]] inline std::uint64_t checked_add_bp_lengths(
    std::uint64_t left,
    std::uint64_t right)
{
    constexpr auto max_logical_length =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (left > max_logical_length || right > max_logical_length
        || right > max_logical_length - left) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq concatenation exceeds bigint logical length")));
    }
    return left + right;
}


/// Choose the concatenation alphabet and width. Bound alphabets must agree.
/// Use the left bound type when available, then the right, or leave both unset.
export [[nodiscard]] inline compact_sequence_stdc_type_params
resolve_concat_type(
    pg_sequence_expanded const* left,
    pg_sequence_read_handle const& right)
{
    if (left->alphabet_bound && right.alphabet_bound) {
        if (left->type_params.alphabet_code
            != right.type_params.alphabet_code) {
            const auto left_name =
                pgseq_supported_alphabets::name_of_alphabet_code(
                    left->type_params.alphabet_code);
            const auto right_name =
                pgseq_supported_alphabets::name_of_alphabet_code(
                    right.type_params.alphabet_code);
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("cannot concatenate bioseq alphabets %.*s and %.*s",
                            static_cast<int>(left_name.size()),
                            left_name.data(),
                            static_cast<int>(right_name.size()),
                            right_name.data())));
        }
        return left->type_params;
    }
    if (left->alphabet_bound) {
        return left->type_params;
    }
    if (right.alphabet_bound) {
        return right.type_params;
    }
    return {
        .alphabet_code = pgseq_unbound_alphabet_code,
        .storage_unit_byte_width = pgseq_unbound_width_tag,
    };
}

/// Join the left and right sequences in order, sharing existing tree pieces.
/// If only one alphabet is bound, validate and pack the other side using it.
export [[nodiscard]] Datum concat_pg_sequences(
    Datum left_datum,
    Datum right_datum)
{
    auto left = acquire_writable_pg_sequence(left_datum);
    auto right = acquire_sequence_for_read(right_datum);
    const auto result_type = resolve_concat_type(left.value, right);
    const bool bound = left.value->alphabet_bound || right.alphabet_bound;
    const auto result_length = checked_add_bp_lengths(
        left.value->bp_length, right.bp_length());
    const bool bind_left = bound && !left.value->alphabet_bound;
    const bool bind_right = bound && !right.alphabet_bound;
    const piece_edge left_root = left.value->root;
    const piece_edge right_root = right.root();
    const std::uint64_t left_length = left.value->bp_length;
    const std::uint64_t right_length = right.bp_length();
    if (bind_left || bind_right) {
        /* Binding an alphabet visits and packs the unbound letters. Validate the
         * resulting whole tree before replacing the old root. */
        replace_root_error_safe(
            *left.value, result_type, result_length, bound,
            [bind_left, bind_right, result_type,
             left_root, right_root](
                piece_tree_arena& arena,
                piece_edge_cleanup_ledger& ledger) {
                tracked_piece_edge bound_left = bind_left
                    ? bind_literals_copy_error_atomic(
                        arena, left_root, result_type, ledger)
                    : retain_piece_edge_error_atomic(
                        ledger, left_root);
                tracked_piece_edge bound_right = bind_right
                    ? bind_literals_copy_error_atomic(
                        arena, right_root, result_type, ledger)
                    : retain_piece_edge_error_atomic(
                        ledger, right_root);
                return join_tracked_edges_error_atomic(
                    arena, ledger, bound_left, bound_right);
            });
    } else {
        replace_root_trusted_join_error_safe(
            *left.value, result_type, result_length, bound,
            left_root, left_length, right_root, right_length,
            [left_root, right_root](
                piece_tree_arena& arena,
                piece_edge_cleanup_ledger& ledger) {
                /* Keep existing subtrees unchanged. Register input references and new
                 * intermediate edges for cleanup before any PostgreSQL call can raise
                 * an error. */
                return join_edges_error_atomic(
                    arena, ledger, left_root, right_root);
            });
    }
    reconcile_concat_trace_provenance_noexcept(
        *left.value, *right.backing);
    return pg_sequence_rw_datum(left.value);
}

/// Return the zero-based slice [from, to), sharing the pieces it needs.
export [[nodiscard]] Datum slice_pg_sequence(
    Datum input,
    std::uint64_t from,
    std::uint64_t to)
{
    auto acquired = acquire_writable_pg_sequence(input);
    if (from > to || to > acquired.value->bp_length) {
        ereport(ERROR,
                (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                 errmsg("bioseq slice is outside its logical sequence")));
    }
    replace_root_error_safe(
        *acquired.value, acquired.value->type_params, to - from,
        acquired.value->alphabet_bound,
        [&acquired, from, to](
            piece_tree_arena& arena,
            piece_edge_cleanup_ledger& ledger) {
            return slice_tree(
                arena, ledger, acquired.value->root, from, to);
        });
    return pg_sequence_rw_datum(acquired.value);
}

/// Record reversal or complementation on the tree without repacking its letters.
export [[nodiscard]] Datum transform_pg_sequence(
    Datum input,
    bioseq::storage::sequence_transform transform)
{
    auto acquired = acquire_writable_pg_sequence(input);
    if (!acquired.value->alphabet_bound
        && bioseq::storage::is_complemented(transform)) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("reverse complement requires a bound bioseq alphabet"),
                 errhint("cast the sequence as bioseq.seq('alphabet', width) first")));
    }
    replace_root_error_safe(
        *acquired.value, acquired.value->type_params,
        acquired.value->bp_length, acquired.value->alphabet_bound,
        [&acquired, transform](
            piece_tree_arena&,
            piece_edge_cleanup_ledger& ledger) {
            return transform_piece_edge_error_atomic(
                ledger, acquired.value->root, transform);
        });
    return pg_sequence_rw_datum(acquired.value);
}

/// Check that every valid rank in the alphabet has a valid complement.
export [[nodiscard]] bool alphabet_has_complete_complement(
    std::uint8_t alphabet_code)
{
    bool complete = true;
    pgseq_supported_alphabets::dispatch_alphabet_by_code(
        alphabet_code,
        [alphabet_code, &complete]<class p_alphabet>() {
            for (std::uint16_t rank = 0u;
                 rank <= static_cast<int>(std::numeric_limits<std::uint8_t>::max());
                 ++rank) {
                char original = '\0';
                if (!pgseq_symbol_for_rank(
                        alphabet_code, static_cast<std::uint8_t>(rank),
                        original)) {
                    continue;
                }
                const auto complemented =
                    bioseq::storage::lazy_sequence_traits<p_alphabet>::
                        letter_complement(static_cast<std::uint8_t>(rank));
                char symbol = '\0';
                if (!pgseq_symbol_for_rank(
                        alphabet_code, complemented, symbol)) {
                    complete = false;
                    return;
                }
            }
        });
    return complete;
}

/* The table reader has checked that the locator belongs to this source.
 * The new tree holds a source reference so it can read the letters later. */
/// Create a read-only expanded sequence that keeps its table-storage source alive.
export [[nodiscard]] inline Datum make_source_backed_pg_sequence(
    pg_sequence_source_handle source,
    bioseq::storage::sequence_locator locator,
    compact_sequence_stdc_type_params stored_type,
    std::uint16_t last_padding_letters,
    slice_trace_attachment* trace = nullptr,
    std::uint32_t producer_value_id = 0u)
{
    if (!pg_sequence_source_handle_valid(source)
        || locator.logical_length >
               static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("invalid source binding for bioseq locator promotion")));
    }
    auto* result = make_pg_sequence_expanded(stored_type, true);
    result->trace = trace;
    result->producer_value_id = producer_value_id;
    retain_slice_trace_attachment(trace);
    replace_root_error_safe(
        *result, stored_type, locator.logical_length, true,
        [source, locator, stored_type,
         last_padding_letters](
            piece_tree_arena& arena,
            piece_edge_cleanup_ledger& ledger) {
            return make_source_node_error_atomic(
                arena, ledger,
                source_piece{
                    .source = source,
                    .locator = locator,
                    .stored_type_params = stored_type,
                    .last_padding_letters = last_padding_letters,
                    .from = 0u,
                    .to = locator.logical_length,
                });
        });
    return pg_sequence_ro_datum(result);
}
