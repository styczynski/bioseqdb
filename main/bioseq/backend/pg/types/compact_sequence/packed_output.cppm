/* Expanded-object ownership and the sole pgseq_t flattening boundary. */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <utility>

#include "headers/pg_compat.h"
#include "fmgr_boundary.h"
extern "C" {
#include "storage/shmem.h"
#include "utils/datum.h"
#include "utils/expandeddatum.h"
}

export module bioseq.types.pg_compact_sequence:packed_output;

import :flat_abi;
import :source_abi;
import :trace;
import :tree_types;
import :tree_ops;
import :view_session;
import bioseq.pg_utils;
import bioseq.sequences.rank_pack;
import bioseq.storage.coordinates;
import bioseq.storage.lazy;

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

export [[nodiscard]] inline Size checked_size_cast(std::uint64_t value)
{
    if (value > std::numeric_limits<Size>::max()) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq size does not fit PostgreSQL Size")));
    }
    return static_cast<Size>(value);
}

export [[nodiscard]] inline Size checked_mul_size(Size left, Size right)
{
    return mul_size(left, right);
}

export [[nodiscard]] inline Size checked_add_size(Size left, Size right)
{
    return add_size(left, right);
}

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

template<class Instance>
[[nodiscard]] inline Size bound_flat_size(std::uint64_t bp_length)
{
    using range = compact_sequence_range<Instance>;
    using traits = typename range::traits;
    if (bp_length > std::numeric_limits<pgseq_t::bp_size_t>::max()) {
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
    const Size total = checked_add_size(
        pgseq_t::header_bytes(), checked_round_up_to_uint64(payload));
    if (total > MaxAllocSize) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("flattened bioseq value exceeds MaxAllocSize")));
    }
    return total;
}

namespace {

[[nodiscard]] inline Size expanded_flat_size(pg_sequence_expanded const& value)
{
    if (!value.alphabet_bound) {
        if (value.bp_length
            > std::numeric_limits<pgseq_t::bp_size_t>::max()) {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("unbound bioseq sequence is too long to flatten")));
        }
        const Size total = checked_add_size(
            pgseq_t::header_bytes(),
            checked_round_up_to_uint64(checked_size_cast(value.bp_length)));
        if (total > MaxAllocSize) {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("flattened unbound bioseq value exceeds MaxAllocSize")));
        }
        return total;
    }
    Size size = 0u;
    pgseq_t::dispatch_instance(
        value.type_params.alphabet_code,
        value.type_params.storage_unit_byte_width,
        [&size, &value]<class Instance>() {
            size = bound_flat_size<Instance>(value.bp_length);
        });
    return size;
}

[[nodiscard]] bool emit_unbound_ascii(
    piece_edge edge,
    std::uint64_t from,
    std::uint64_t to,
    std::span<std::byte> output)
{
    if (!edge || from > to || to > edge.node->bp_length
        || output.size() != to - from
        || bioseq::storage::has_complement(edge.transform)) {
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
    const bool reverse = bioseq::storage::has_reverse(edge.transform);
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

template<class Instance, class View>
void pack_view_into(pgseq_t& destination, View view, std::uint64_t length)
{
    using range = compact_sequence_range<Instance>;
    using traits = typename range::traits;
    using unit = typename traits::storage_unit;
    const std::uint64_t unit_count = checked_ceil_div(
        length, traits::letters_per_unit);
    auto units = std::span<unit>{
        reinterpret_cast<unit*>(destination.data),
        static_cast<std::size_t>(unit_count)};
    using sink = bioseq::sequences::fixed_rank_pack_unit_sink<unit>;
    using writer_type = bioseq::sequences::checked_rank_pack_sink<
        Instance,
        bioseq::sequences::invalid_rank_policy::reject,
        bioseq::sequences::pack_capacity_policy::fixed_span,
        sink>;
    writer_type writer{sink{units}};
    bioseq::storage::stream_transformed_packed<
        bioseq::storage::sequence_transform::identity>(
            view, 0u, length, writer);
    const auto result = writer.finish(length, unit_count);
    if (!result) {
        rank_pack_error(result.status);
    }
    destination.alphabet_code =
        pgseq_supported_alphabets::template index_of<
            typename Instance::alphabet_type>();
    destination.storage_unit_byte_width = Instance::byte_width;
    destination.padding_letters = result.padding_letters;
    destination.bp_size = static_cast<pgseq_t::bp_size_t>(length);
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

export [[nodiscard]] inline bool emit_pg_unbound_ascii(
    piece_edge root,
    std::uint64_t from,
    std::uint64_t to,
    std::span<std::byte> output)
{
    return emit_unbound_ascii(root, from, to, output);
}

export [[nodiscard]] inline Size pg_sequence_get_flat_size(
    ExpandedObjectHeader* header)
{
    auto* value = reinterpret_cast<pg_sequence_expanded*>(header);
    return expanded_flat_size(*value);
}

export inline void pg_sequence_flatten_into(
    ExpandedObjectHeader* header,
    void* result,
    Size allocated_size)
{
    auto& value = *reinterpret_cast<pg_sequence_expanded*>(header);
    const Size expected = expanded_flat_size(value);
    if (allocated_size != expected) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq EOH flatten size changed between callbacks")));
    }
    std::memset(result, 0, allocated_size);
    auto& destination = *static_cast<pgseq_t*>(result);
    SET_VARSIZE(&destination, allocated_size);

    if (!value.alphabet_bound) {
        destination.alphabet_code = pgseq_unbound_alphabet_code;
        destination.storage_unit_byte_width = pgseq_unbound_width_tag;
        destination.padding_letters = 0u;
        destination.bp_size = static_cast<pgseq_t::bp_size_t>(value.bp_length);
        if (!emit_unbound_ascii(
                value.root, 0u, value.bp_length,
                std::span<std::byte>{
                    reinterpret_cast<std::byte*>(destination.data),
                    static_cast<std::size_t>(value.bp_length)})) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("unbound bioseq tree cannot be flattened")));
        }
        const auto observer = make_slice_payload_allocation_observer_noexcept(
            value.trace, value.producer_value_id,
            materialization_mechanism::packed_full,
            materialization_reason::executor_copy);
        (void)bioseq::storage::observe_sequence_payload_allocation_noexcept(
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
            pgseq_t::dispatch_instance(
                value.type_params.alphabet_code,
                value.type_params.storage_unit_byte_width,
                [&destination, &value, local_session, header,
                 descriptor]<class Instance>() {
                    using alphabet = typename Instance::alphabet_type;
                    auto* owner = make_pg_sequence_call_owner<alphabet>(
                        *local_session, EOHPGetRODatum(header), descriptor);
                    pack_view_into<Instance>(
                        destination, owner->view(), value.bp_length);
                });
            finish_pg_sequence_read_session(local_session, true);
        } catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
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
    const auto observer = make_slice_payload_allocation_observer_noexcept(
        value.trace, value.producer_value_id,
        materialization_mechanism::packed_full,
        materialization_reason::executor_copy);
    (void)bioseq::storage::observe_sequence_payload_allocation_noexcept(
        std::addressof(observer),
        bioseq::storage::sequence_payload_allocation_kind::flat_sequence,
        value.bp_length, value.bp_length,
        static_cast<std::uint64_t>(allocated_size),
        static_cast<std::uint64_t>(allocated_size));
}

export [[nodiscard, gnu::used, gnu::noinline]] ExpandedObjectMethods const&
pg_sequence_expanded_methods() noexcept
{
    static const ExpandedObjectMethods methods{
        &pg_sequence_get_flat_size,
        &pg_sequence_flatten_into,
    };
    return methods;
}

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

export [[nodiscard]] inline Datum pg_sequence_rw_datum(
    pg_sequence_expanded* value) noexcept
{
    return EOHPGetRWDatum(&value->hdr);
}

export [[nodiscard]] inline Datum pg_sequence_ro_datum(
    pg_sequence_expanded* value) noexcept
{
    return EOHPGetRODatum(&value->hdr);
}

export [[nodiscard]] inline slice_trace_attachment*
pg_sequence_trace_attachment_for_datum(Datum input)
{
    auto const* expanded = get_pg_sequence_expanded_ro(input);
    return expanded == nullptr ? nullptr : expanded->trace;
}

export struct pending_root_cleanup {
    MemoryContextCallback callback;
    piece_tree_arena* destination_arena;
    piece_edge root;
    bool armed;
};

export inline void pending_root_reset(void* argument) noexcept
{
    auto* guard = static_cast<pending_root_cleanup*>(argument);
    if (guard == nullptr || !guard->armed) {
        return;
    }
    guard->armed = false;
    release_edge(guard->root);
    guard->root = {};
    release_arena(guard->destination_arena);
    guard->destination_arena = nullptr;
}

export template<class Build>
inline void replace_root_error_safe(
    pg_sequence_expanded& value,
    compact_sequence_stdc_type_params type,
    std::uint64_t length,
    bool alphabet_bound,
    Build&& build)
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

        /* Arena storage is independent of both caller and EOH contexts so a
         * second EOH can retain/share it after this EOH is reset. */
        guard->destination_arena = make_piece_tree_arena();
        guard->root = std::forward<Build>(build)(
            *guard->destination_arena);
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

namespace {

[[nodiscard]] inline literal_piece copy_flat_literal(
    piece_tree_arena& arena,
    pgseq_t const& flat,
    pg_sequence_descriptor descriptor,
    slice_trace_attachment* trace,
    std::uint32_t producer_value_id)
{
    const bool unbound = !descriptor.alphabet_bound;
    const std::size_t count = unbound
        ? static_cast<std::size_t>(descriptor.bp_length)
        : static_cast<std::size_t>(VARSIZE(&flat)) - pgseq_t::header_bytes();
    const std::size_t allocation_bytes = std::max<std::size_t>(count, 1u);
    auto* bytes = allocate_piece_literal_storage(
        arena, allocation_bytes);
    if (count != 0u) {
        std::memcpy(bytes, flat.data, count);
    }
    const auto observer = make_slice_payload_allocation_observer_noexcept(
        trace, producer_value_id,
        materialization_mechanism::backing_copy,
        materialization_reason::executor_copy);
    (void)bioseq::storage::observe_sequence_payload_allocation_noexcept(
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
            : flat.get_type_params(),
        .encoding = unbound
            ? literal_encoding::unbound_ascii
            : literal_encoding::packed,
        .validation = unbound
            ? literal_validation::unvalidated
            : literal_validation::alphabet_validated,
    };
}

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
    /* Two different live producer identities cannot be attributed to one
     * result without its planned result id.  Clear the observation metadata;
     * BioseqSliceProject installs the exact result provenance after evaluation. */
    if (result.trace != selected_trace) {
        retain_slice_trace_attachment(selected_trace);
        auto* previous = result.trace;
        result.trace = selected_trace;
        release_slice_trace_attachment(previous);
    }
    result.producer_value_id = selected_producer;
}

} // namespace

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

    auto* flat = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(input));
    const Datum flat_datum = PointerGetDatum(flat);
    const auto descriptor = classify_and_describe_sequence(flat_datum);
    if (descriptor.kind == pg_sequence_datum_kind::locator) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cannot resolve a bioseq locator outside its owning table scan"),
                 errdetail("relation, column, snapshot, and cache ownership are unavailable")));
    }
    const compact_sequence_stdc_type_params type = descriptor.alphabet_bound
        ? flat->get_type_params()
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
        [flat, descriptor, result](piece_tree_arena& arena) {
            return make_literal_node(
                arena, copy_flat_literal(
                    arena, *flat, descriptor,
                    result->trace, result->producer_value_id));
        });
    if (reinterpret_cast<Pointer>(flat) != DatumGetPointer(input)) {
        pfree(flat);
    }
    return {result, false};
}

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

export [[nodiscard]] inline std::uint64_t checked_add_bp_lengths(
    std::uint64_t left,
    std::uint64_t right)
{
    constexpr auto max_logical_length =
        static_cast<std::uint64_t>(INT64_MAX);
    if (left > max_logical_length || right > max_logical_length
        || right > max_logical_length - left) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq concatenation exceeds bigint logical length")));
    }
    return left + right;
}

namespace {

[[nodiscard]] inline bool concat_length_add_raises_program_limit(
    std::uint64_t left, std::uint64_t right) {
  const MemoryContext saved_context = CurrentMemoryContext;
  bool volatile matched = false;
  PG_TRY();
  {
    (void)checked_add_bp_lengths(left, right);
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(saved_context);
    ErrorData *error = CopyErrorData();
    matched = error != nullptr
        && error->sqlerrcode == ERRCODE_PROGRAM_LIMIT_EXCEEDED;
    FlushErrorState();
    if (error != nullptr) {
      FreeErrorData(error);
    }
  }
  PG_END_TRY();
  return matched;
}

[[nodiscard]] inline bool run_concat_length_guardrail_matrix() {
  constexpr auto max_logical_length =
      static_cast<std::uint64_t>(INT64_MAX);
  return checked_add_bp_lengths(max_logical_length, 0u)
             == max_logical_length
      && checked_add_bp_lengths(max_logical_length - 1u, 1u)
             == max_logical_length
      && concat_length_add_raises_program_limit(max_logical_length, 1u)
      && concat_length_add_raises_program_limit(UINT64_MAX, 0u)
      && concat_length_add_raises_program_limit(0u, UINT64_MAX);
}

} // namespace

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
    replace_root_error_safe(
        *left.value, result_type, result_length, bound,
        [bound, result_type, &left, &right](piece_tree_arena& arena) {
            piece_edge bound_left = bound
                ? bind_literals_copy(arena, left.value->root, result_type)
                : retain_edge(left.value->root);
            piece_edge bound_right = bound
                ? bind_literals_copy(arena, right.root(), result_type)
                : retain_edge(right.root());
            return join_edges(arena, bound_left, bound_right);
        });
    reconcile_concat_trace_provenance_noexcept(
        *left.value, *right.backing);
    return pg_sequence_rw_datum(left.value);
}

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
        [&acquired, from, to](piece_tree_arena& arena) {
            return slice_tree(arena, acquired.value->root, from, to);
        });
    return pg_sequence_rw_datum(acquired.value);
}

export [[nodiscard]] Datum transform_pg_sequence(
    Datum input,
    bioseq::storage::sequence_transform transform)
{
    auto acquired = acquire_writable_pg_sequence(input);
    if (!acquired.value->alphabet_bound
        && bioseq::storage::has_complement(transform)) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("reverse complement requires a bound bioseq alphabet"),
                 errhint("cast the sequence as bioseq.seq('alphabet', width) first")));
    }
    replace_root_error_safe(
        *acquired.value, acquired.value->type_params,
        acquired.value->bp_length, acquired.value->alphabet_bound,
        [&acquired, transform](piece_tree_arena&) {
            piece_edge next = retain_edge(acquired.value->root);
            next.transform = bioseq::storage::compose(
                transform, next.transform);
            return next;
        });
    return pg_sequence_rw_datum(acquired.value);
}

export [[nodiscard]] bool alphabet_has_complete_complement(
    std::uint8_t alphabet_code)
{
    bool complete = true;
    pgseq_supported_alphabets::dispatch_alphabet_by_code(
        alphabet_code,
        [alphabet_code, &complete]<class Alphabet>() {
            for (std::uint16_t rank = 0u; rank <= UINT8_MAX; ++rank) {
                char original = '\0';
                if (!pgseq_symbol_for_rank(
                        alphabet_code, static_cast<std::uint8_t>(rank),
                        original)) {
                    continue;
                }
                const auto complemented =
                    bioseq::storage::lazy_rank_stream_traits<Alphabet>::
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

/* Table-AM promotion entrypoint.  The caller has already acquired a retained
 * source-domain handle from the verified binding registry. */
export [[nodiscard]] inline Datum make_source_backed_pg_sequence(
    pg_sequence_source_handle source,
    bioseq::storage::seq_locator locator,
    compact_sequence_stdc_type_params stored_type,
    std::uint16_t last_padding_letters,
    slice_trace_attachment* trace = nullptr,
    std::uint32_t producer_value_id = 0u)
{
    if (!pg_sequence_source_handle_valid(source)
        || locator.total_bp_size > static_cast<std::uint64_t>(INT64_MAX)) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("invalid source binding for bioseq locator promotion")));
    }
    auto* result = make_pg_sequence_expanded(stored_type, true);
    result->trace = trace;
    result->producer_value_id = producer_value_id;
    retain_slice_trace_attachment(trace);
    replace_root_error_safe(
        *result, stored_type, locator.total_bp_size, true,
        [source, locator, stored_type,
         last_padding_letters](piece_tree_arena& arena) {
            return make_source_node(
                arena,
                source_piece{
                    .source = source,
                    .locator = locator,
                    .stored_type_params = stored_type,
                    .last_padding_letters = last_padding_letters,
                    .from = 0u,
                    .to = locator.total_bp_size,
                });
        });
    return pg_sequence_ro_datum(result);
}

namespace {

struct synthetic_lazy_source_state {};

inline void retain_synthetic_lazy_source(void*) noexcept {}
inline void release_synthetic_lazy_source(void*) noexcept {}

[[nodiscard]] inline pg_sequence_source_identity
synthetic_lazy_source_identity(void const* state) noexcept
{
    return {
        .control_address = reinterpret_cast<std::uintptr_t>(state),
        .generation = 1u,
    };
}

[[nodiscard]] inline bool
synthetic_lazy_source_worker_shareable(void const *) noexcept {
  return true;
}

[[nodiscard]] inline pg_sequence_source_read_status
read_synthetic_lazy_source(void const *, pg_sequence_read_session &,
                           bioseq::storage::seq_locator const &locator,
                           compact_sequence_stdc_type_params stored_type,
                           bioseq::storage::sequence_transform transform,
                           std::uint64_t from, std::span<std::uint8_t> output) {
  constexpr std::uint8_t dna4_code =
      pgseq_supported_alphabets::index_of<seq::dna4>();
  if (stored_type.alphabet_code != dna4_code || from > locator.total_bp_size ||
      output.size() > locator.total_bp_size - from) {
    return pg_sequence_source_read_status::wrong_thread;
  }
  std::uint8_t rank = 0u;
  if (bioseq::storage::has_complement(transform)) {
    rank =
        bioseq::storage::lazy_rank_stream_traits<seq::dna4>::letter_complement(
            rank);
  }
  std::fill(output.begin(), output.end(), rank);
  return pg_sequence_source_read_status::ok;
}

[[nodiscard]] inline pg_sequence_source_handle
synthetic_lazy_source_handle() noexcept {
  static synthetic_lazy_source_state state{};
  static const pg_sequence_source_backend_ops ops{
      .retain = &retain_synthetic_lazy_source,
      .release = &release_synthetic_lazy_source,
      .identity = &synthetic_lazy_source_identity,
      .worker_shareable = &synthetic_lazy_source_worker_shareable,
      .read_ranks = &read_synthetic_lazy_source,
  };
  return {
      .state = std::addressof(state),
      .ops = std::addressof(ops),
  };
}

} // namespace

/* Internal acceptance seam for logical-length guardrails. Its immutable
 * process-lifetime source synthesizes DNA4 rank zero on demand, so tests can
 * exercise bigint metadata without allocating the logical payload. */
export inline Datum synthetic_lazy_sequence_impl(FunctionCallInfo fcinfo) {
  const std::int64_t requested = PG_GETARG_INT64(0);
  if (requested < 0) {
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("synthetic bioseq length must be nonnegative")));
  }
  constexpr compact_sequence_stdc_type_params type{
      .alphabet_code = pgseq_supported_alphabets::index_of<seq::dna4>(),
      .storage_unit_byte_width = 1u,
  };
  const auto length = static_cast<std::uint64_t>(requested);
  PG_RETURN_DATUM(make_source_backed_pg_sequence(
      synthetic_lazy_source_handle(),
      bioseq::storage::seq_locator{
          .version = bioseq::storage::kLocatorVersion,
          .bucket_idx = 0u,
          ._pad_2 = 0u,
          .shard_idx = 0u,
          .num_fragments = length == 0u ? 0u : 1u,
          ._pad_4 = 0u,
          .total_bp_size = length,
          .first_sub_row_no = 0u,
          .source_epoch = 1u,
      },
      type, 0u));
}

export inline Datum sequence_piece_count_impl(FunctionCallInfo fcinfo) {
  const auto value = acquire_sequence_for_read(PG_GETARG_DATUM(0));
  PG_RETURN_INT64(
      value.root() ? static_cast<std::int64_t>(value.root().node->piece_count)
                   : 0);
}

/* Direct PostgreSQL ownership-protocol seam: RW datumTransfer reparents the
 * EOH, while RO datumCopy produces an independent flat value.  Both must
 * survive reset/deletion of the context that originally owned the EOH. */
export inline Datum expanded_ownership_self_test_impl(FunctionCallInfo) {
  MemoryContext saved = CurrentMemoryContext;
  MemoryContext volatile transfer_input = nullptr;
  MemoryContext volatile transfer_parent = nullptr;
  MemoryContext volatile copy_input = nullptr;
  MemoryContext volatile copy_parent = nullptr;
  bool volatile valid = false;
  const std::uint64_t arena_baseline = piece_tree_live_arena_count_noexcept();
  const bool concat_length_guardrail_matrix =
      run_concat_length_guardrail_matrix();
  PG_TRY();
  {
    transfer_input = AllocSetContextCreate(saved, "bioseq EOH transfer input",
                                           ALLOCSET_SMALL_SIZES);
    transfer_parent = AllocSetContextCreate(saved, "bioseq EOH transfer parent",
                                            ALLOCSET_SMALL_SIZES);
    copy_input = AllocSetContextCreate(saved, "bioseq EOH copy input",
                                       ALLOCSET_SMALL_SIZES);
    copy_parent = AllocSetContextCreate(saved, "bioseq EOH copy parent",
                                        ALLOCSET_SMALL_SIZES);
    constexpr compact_sequence_stdc_type_params unbound_type{
        .alphabet_code = pgseq_unbound_alphabet_code,
        .storage_unit_byte_width = pgseq_unbound_width_tag,
    };
    const auto make_probe = [unbound_type](MemoryContext input,
                                           std::array<std::byte, 3> bytes) {
      MemoryContext previous = MemoryContextSwitchTo(input);
      auto *value = make_pg_sequence_expanded(unbound_type, false);
      replace_root_error_safe(
          *value, unbound_type, bytes.size(), false,
          [bytes](piece_tree_arena &arena) {
            auto *storage = allocate_piece_literal_storage(arena, bytes.size());
            std::memcpy(storage, bytes.data(), bytes.size());
            return make_literal_node(
                arena,
                literal_piece{
                    .storage_arena = std::addressof(arena),
                    .bytes = storage,
                    .symbol_from = 0u,
                    .symbol_to = bytes.size(),
                    .stored_type_params =
                        {
                            .alphabet_code = pgseq_unbound_alphabet_code,
                            .storage_unit_byte_width = pgseq_unbound_width_tag,
                        },
                    .encoding = literal_encoding::unbound_ascii,
                    .validation = literal_validation::unvalidated,
                });
          });
      MemoryContextSwitchTo(previous);
      return value;
    };

    auto *transferred_value =
        make_probe(const_cast<MemoryContext>(transfer_input),
                   {std::byte{0x41}, std::byte{0x43}, std::byte{0x47}});
    MemoryContextSwitchTo(const_cast<MemoryContext>(transfer_parent));
    const Datum transferred =
        datumTransfer(pg_sequence_rw_datum(transferred_value), false, -1);
    MemoryContextSwitchTo(saved);
    const bool reparented =
        get_pg_sequence_expanded_rw(transferred) == transferred_value &&
        MemoryContextGetParent(transferred_value->object_context) ==
            transfer_parent;
    MemoryContextReset(const_cast<MemoryContext>(transfer_input));
    std::array<std::byte, 3> transferred_text{};
    const bool transfer_survived =
        emit_pg_unbound_ascii(transferred_value->root, 0u,
                              transferred_text.size(), transferred_text) &&
        transferred_text == std::array<std::byte, 3>{std::byte{0x41},
                                                     std::byte{0x43},
                                                     std::byte{0x47}};

    auto *copied_value =
        make_probe(const_cast<MemoryContext>(copy_input),
                   {std::byte{0x54}, std::byte{0x47}, std::byte{0x41}});
    const Datum read_only = MakeExpandedObjectReadOnly(
        pg_sequence_rw_datum(copied_value), false, -1);
    MemoryContextSwitchTo(const_cast<MemoryContext>(copy_parent));
    const Datum copied = datumCopy(read_only, false, -1);
    MemoryContextSwitchTo(saved);
    MemoryContextReset(const_cast<MemoryContext>(copy_input));
    auto const *copied_flat =
        reinterpret_cast<pgseq_t const *>(DatumGetPointer(copied));
    const bool copy_survived =
        !VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(copied)) &&
        VARSIZE(copied_flat) >=
            pgseq_t::header_bytes() + transferred_text.size() &&
        copied_flat->alphabet_code == pgseq_unbound_alphabet_code &&
        copied_flat->storage_unit_byte_width == pgseq_unbound_width_tag &&
        copied_flat->bp_size == transferred_text.size() &&
        std::memcmp(copied_flat->data, "TGA", transferred_text.size()) == 0;
    valid = concat_length_guardrail_matrix && reparented
        && transfer_survived && copy_survived;

    MemoryContextDelete(const_cast<MemoryContext>(copy_input));
    copy_input = nullptr;
    MemoryContextDelete(const_cast<MemoryContext>(transfer_input));
    transfer_input = nullptr;
    MemoryContextDelete(const_cast<MemoryContext>(copy_parent));
    copy_parent = nullptr;
    MemoryContextDelete(const_cast<MemoryContext>(transfer_parent));
    transfer_parent = nullptr;
    valid = valid && piece_tree_live_arena_count_noexcept() == arena_baseline;
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(saved);
    if (copy_input != nullptr) {
      MemoryContextDelete(const_cast<MemoryContext>(copy_input));
    }
    if (transfer_input != nullptr) {
      MemoryContextDelete(const_cast<MemoryContext>(transfer_input));
    }
    if (copy_parent != nullptr) {
      MemoryContextDelete(const_cast<MemoryContext>(copy_parent));
    }
    if (transfer_parent != nullptr) {
      MemoryContextDelete(const_cast<MemoryContext>(transfer_parent));
    }
    FlushErrorState();
    valid = false;
  }
  PG_END_TRY();
  MemoryContextSwitchTo(saved);
  PG_RETURN_BOOL(valid);
}

/* Trace metadata is observational and may be installed on an otherwise
 * read-only EOH wrapper without changing its logical sequence value. */
export [[nodiscard]] inline bool
attach_slice_trace_provenance(Datum expanded, slice_trace_attachment *trace,
                              std::uint32_t producer_value_id) noexcept {
  if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(expanded)) ||
      (trace != nullptr &&
       (!slice_trace_attachment_live(trace) || producer_value_id == 0u))) {
    return false;
  }
  ExpandedObjectHeader *header = DatumGetEOHP(expanded);
  if (header == nullptr ||
      header->eoh_methods != std::addressof(pg_sequence_expanded_methods())) {
    return false;
  }
  auto *value = reinterpret_cast<pg_sequence_expanded *>(header);
  if (value->trace != trace) {
    retain_slice_trace_attachment(trace);
    auto *previous = value->trace;
    value->trace = trace;
    release_slice_trace_attachment(previous);
  }
  value->producer_value_id = trace == nullptr ? 0u : producer_value_id;
  return true;
}

extern "C" {
CXX_PG_FUNCTION_INFO_V1(synthetic_lazy_sequence);
PGDLLEXPORT Datum synthetic_lazy_sequence(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&synthetic_lazy_sequence_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(sequence_piece_count);
PGDLLEXPORT Datum sequence_piece_count(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&sequence_piece_count_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(expanded_ownership_self_test);
PGDLLEXPORT Datum expanded_ownership_self_test(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&expanded_ownership_self_test_impl, fcinfo);
}
} // extern "C"
