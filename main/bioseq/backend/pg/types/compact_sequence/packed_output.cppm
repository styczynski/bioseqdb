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

/* PostgreSQL's palloc accepts Size, but rejects requests above MaxAllocSize
 * with an internal allocation error.  Every concrete sequence conversion
 * must turn that condition into the public PROGRAM_LIMIT_EXCEEDED contract
 * before it asks a varlena, StringInfo, array, or flat pgseq allocator to
 * grow.  Comparing the uint64 payload before narrowing also covers size_t /
 * Size overflow on narrower PostgreSQL builds. */
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

export template<class Instance>
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
    const Size total = checked_materialization_allocation_size(
        checked_round_up_to_uint64(payload), pgseq_t::header_bytes(),
        "flat sequence");
    return total;
}

namespace {

enum class materialization_size_probe : std::uint8_t {
  round_overflow,
  multiply_overflow,
  addition_overflow,
  maxalloc_payload_overflow,
  maxalloc_header_terminator_overflow,
  bound_flat_bp_overflow,
};

[[nodiscard]] bool materialization_size_probe_raises_program_limit(
    materialization_size_probe probe) {
  const MemoryContext saved_context = CurrentMemoryContext;
  bool volatile matched = false;
  PG_TRY();
  {
    switch (probe) {
      case materialization_size_probe::round_overflow:
        (void)checked_round_up_to_uint64(
            (std::numeric_limits<Size>::max)()
            - (sizeof(std::uint64_t) - 2u));
        break;
      case materialization_size_probe::multiply_overflow:
        (void)checked_mul_size((std::numeric_limits<Size>::max)(), 2u);
        break;
      case materialization_size_probe::addition_overflow:
        (void)checked_add_size((std::numeric_limits<Size>::max)(), 1u);
        break;
      case materialization_size_probe::maxalloc_payload_overflow:
        (void)checked_materialization_allocation_size(
            static_cast<std::uint64_t>(MaxAllocSize) + 1u, 0u,
            "self-test payload");
        break;
      case materialization_size_probe::maxalloc_header_terminator_overflow:
        (void)checked_materialization_allocation_size(
            static_cast<std::uint64_t>(MaxAllocSize) - VARHDRSZ,
            static_cast<Size>(VARHDRSZ + 1u), "self-test framed output");
        break;
      case materialization_size_probe::bound_flat_bp_overflow:
        pgseq_t::dispatch_instance(
            pgseq_supported_alphabets::index_of<seq::dna4>(), 8u,
            []<class Instance>() {
              (void)bound_flat_size<Instance>(
                  static_cast<std::uint64_t>(UINT32_MAX) + 1u);
            });
        break;
    }
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(saved_context);
    ErrorData* error = CopyErrorData();
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

[[nodiscard]] bool run_materialization_size_guardrail_matrix() {
  Size zero_flat_size = 0u;
  pgseq_t::dispatch_instance(
      pgseq_supported_alphabets::index_of<seq::dna4>(), 8u,
      [&zero_flat_size]<class Instance>() {
        zero_flat_size = bound_flat_size<Instance>(0u);
      });
  const Size framed_overhead = static_cast<Size>(VARHDRSZ + 1u);
  return checked_size_cast(0u) == 0u
      && checked_mul_size(0u, (std::numeric_limits<Size>::max)()) == 0u
      && checked_add_size(0u, 0u) == 0u
      && checked_round_up_to_uint64(0u) == 0u
      && checked_round_up_to_uint64(sizeof(std::uint64_t))
          == sizeof(std::uint64_t)
      && checked_round_up_to_uint64(1u) == sizeof(std::uint64_t)
      && checked_materialization_allocation_size(
             static_cast<std::uint64_t>(MaxAllocSize), 0u,
             "self-test exact MaxAlloc") == MaxAllocSize
      && checked_materialization_allocation_size(
             static_cast<std::uint64_t>(MaxAllocSize) - framed_overhead,
             framed_overhead, "self-test exact framed MaxAlloc")
          == MaxAllocSize
      && zero_flat_size >= pgseq_t::header_bytes()
      && materialization_size_probe_raises_program_limit(
          materialization_size_probe::round_overflow)
      && materialization_size_probe_raises_program_limit(
          materialization_size_probe::multiply_overflow)
      && materialization_size_probe_raises_program_limit(
          materialization_size_probe::addition_overflow)
      && materialization_size_probe_raises_program_limit(
          materialization_size_probe::maxalloc_payload_overflow)
      && materialization_size_probe_raises_program_limit(
          materialization_size_probe::maxalloc_header_terminator_overflow)
      && materialization_size_probe_raises_program_limit(
          materialization_size_probe::bound_flat_bp_overflow);
}

[[nodiscard]] inline Size expanded_flat_size(pg_sequence_expanded const& value)
{
    if (!value.alphabet_bound) {
        if (value.bp_length
            > std::numeric_limits<pgseq_t::bp_size_t>::max()) {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("unbound bioseq sequence is too long to flatten")));
        }
        const Size total = checked_materialization_allocation_size(
            checked_round_up_to_uint64(checked_size_cast(value.bp_length)),
            pgseq_t::header_bytes(), "flat unbound sequence");
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
    if (from > to || output.size() != to - from) {
        return false;
    }
    if (!edge) {
        return from == 0 && to == 0 && output.empty();
    }
    if (to > edge.node->bp_length
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
    /* Persistent concat validates only its newly allocated AVL path.  A flat
     * payload is already O(total symbols), so retain the independent full-tree
     * audit at this external materialization boundary. */
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
#ifdef BIOSEQ_TEST_INDEXES
        note_slice_payload_allocation_test_event_noexcept(
            materialization_mechanism::packed_full,
            materialization_reason::executor_copy);
#endif
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
#ifdef BIOSEQ_TEST_INDEXES
    note_slice_payload_allocation_test_event_noexcept(
        materialization_mechanism::packed_full,
        materialization_reason::executor_copy);
#endif
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
    piece_edge_cleanup_ledger build_edges;
    piece_edge root;
    piece_edge_cleanup_handle root_cleanup;
    bool armed;
};

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

template<class Build>
[[nodiscard]] inline tracked_piece_edge invoke_piece_root_build(
    Build&& build,
    piece_tree_arena& arena,
    piece_edge_cleanup_ledger& ledger)
{
    /* A root builder must publish every returned owning edge to the reset
     * callback's ledger before it may perform another PostgreSQL operation.
     * Requiring the tracked return type makes that contract structural. */
    tracked_piece_edge built = std::forward<Build>(build)(arena, ledger);
    return built;
}

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
        const tracked_piece_edge built = invoke_piece_root_build(
            std::forward<Build>(build), *guard->destination_arena,
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

/* Error-safe publication for the one operation whose existing inputs are
 * immutable, previously validated roots.  The destination arena contains only
 * the height-guided concat path, so validation remains O(log pieces) while
 * preserving the same publish-after-validation and unwind ownership contract
 * as replace_root_error_safe. */
template<class Build>
inline void replace_root_trusted_join_error_safe(
    pg_sequence_expanded& value,
    compact_sequence_stdc_type_params type,
    std::uint64_t length,
    bool alphabet_bound,
    piece_edge trusted_left,
    std::uint64_t left_length,
    piece_edge trusted_right,
    std::uint64_t right_length,
    Build&& build)
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
            std::forward<Build>(build), *guard->destination_arena,
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
#ifdef BIOSEQ_TEST_INDEXES
    note_slice_payload_allocation_test_event_noexcept(
        materialization_mechanism::backing_copy,
        materialization_reason::executor_copy);
#endif
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
    const bool bind_left = bound && !left.value->alphabet_bound;
    const bool bind_right = bound && !right.alphabet_bound;
    const piece_edge left_root = left.value->root;
    const piece_edge right_root = right.root();
    const std::uint64_t left_length = left.value->bp_length;
    const std::uint64_t right_length = right.bp_length();
    if (bind_left || bind_right) {
        /* Alphabet adoption necessarily visits the unbound literal side and
         * retains the full validator for that type-changing operation. */
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
                /* Existing subtrees remain immutable.  The tracked join
                 * retains both borrowed operands and every local intermediate
                 * before any operation capable of PostgreSQL ERROR. */
                return join_edges_error_atomic(
                    arena, ledger, left_root, right_root);
            });
    }
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
        [&acquired, from, to](
            piece_tree_arena& arena,
            piece_edge_cleanup_ledger& ledger) {
            return slice_tree(
                arena, ledger, acquired.value->root, from, to);
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
        [&acquired, transform](
            piece_tree_arena&,
            piece_edge_cleanup_ledger& ledger) {
            return transform_piece_edge_error_atomic(
                ledger, acquired.value->root, transform);
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

/* Exercise the public SQL concat implementation in two complementary shapes.
 * A 4,096-step append chain proves deterministic per-generation O(log n)
 * construction *and validation* work.  A second public-path construction uses
 * immutable power-of-two roots to reach exactly one million logical pieces in
 * O(log n) calls, then runs the independent full validator once at that
 * explicit probe boundary.  The separate :tree_ops test still supplies one
 * million distinct physical nodes and the hard metadata ceiling.
 *
 * No wall clock participates in the verdict: join, cleanup-ledger,
 * retirement, and bounded-validator step counters plus exact arena accounting
 * make super-logarithmic construction work fail deterministically. */
[[nodiscard]] inline bool run_public_concat_path_copy_probe() {
  constexpr std::uint32_t piece_count = 4'096u;
  constexpr std::uint64_t piece_length = 4'097u;
  constexpr std::uint16_t final_height = 13u;
  constexpr Size node_unit = MAXALIGN(sizeof(piece_node));
  constexpr Size arena_dependency_unit = MAXALIGN(sizeof(arena_dependency));
  constexpr compact_sequence_stdc_type_params type{
      .alphabet_code = pgseq_supported_alphabets::index_of<seq::dna4>(),
      .storage_unit_byte_width = 8u,
  };
  constexpr bioseq::storage::seq_locator locator{
      .version = bioseq::storage::kLocatorVersion,
      .bucket_idx = 0u,
      ._pad_2 = 0u,
      .shard_idx = 0u,
      .num_fragments = 1u,
      ._pad_4 = 0u,
      .total_bp_size = piece_length,
      .first_sub_row_no = 0u,
      .source_epoch = 1u,
  };

  MemoryContext saved = CurrentMemoryContext;
  MemoryContext volatile probe_context = nullptr;
  bool volatile valid = false;
  const std::uint64_t arena_baseline = piece_tree_live_arena_count_noexcept();
  PG_TRY();
  {
    probe_context = AllocSetContextCreate(
        saved, "bioseq public concat path-copy probe", ALLOCSET_SMALL_SIZES);
    MemoryContextSwitchTo(const_cast<MemoryContext>(probe_context));
    const Datum left = make_source_backed_pg_sequence(
        synthetic_lazy_source_handle(), locator, type, 0u);
    const Datum right = make_source_backed_pg_sequence(
        synthetic_lazy_source_handle(), locator, type, 0u);
    auto const *left_value = get_pg_sequence_expanded_ro(left);
    auto const *right_value = get_pg_sequence_expanded_ro(right);
    bool local_valid = left_value != nullptr && right_value != nullptr
        && left_value->root && right_value->root
        && left_value->root.node->piece_count == 1u
        && right_value->root.node->piece_count == 1u
        && left_value->root.node->height == 1u
        && right_value->root.node->height == 1u;
    piece_node const *const left_root = left_value == nullptr
        ? nullptr : left_value->root.node;
    piece_node const *const right_root = right_value == nullptr
        ? nullptr : right_value->root.node;
    std::uint64_t total_new_nodes = 0u;
    std::uint64_t cumulative_node_budget = 0u;
    std::uint32_t maximum_generation_nodes = 0u;
    std::uint16_t power_height = 1u;
    bool saw_rotation_retirement = false;
    pg_sequence_expanded *result_wrapper = nullptr;
    Datum result = left;
    reset_piece_tree_join_work_noexcept();

    for (std::uint32_t current_count = 2u;
         current_count <= piece_count; ++current_count) {
      const auto work_before = read_piece_tree_join_work_noexcept();
      result = concat_pg_sequences(result, right);
      const auto work_after = read_piece_tree_join_work_noexcept();
      auto *current = get_pg_sequence_expanded_rw(result);
      local_valid = local_valid && current != nullptr && current->root
          && current->root.node->piece_count == current_count
          && current->root.node->bp_length
              == static_cast<std::uint64_t>(current_count) * piece_length;
      if (current == nullptr || !current->root) {
        break;
      }
      if (result_wrapper == nullptr) {
        result_wrapper = current;
      } else {
        local_valid = local_valid && current == result_wrapper;
      }

      auto const &arena = *current->root.arena;
      const Size exact_node_bytes =
          static_cast<Size>(arena.node_count) * node_unit;
      const Size exact_dependency_bytes =
          static_cast<Size>(arena.arena_dependency_count)
          * arena_dependency_unit;
      const auto generation_node_budget = static_cast<std::uint32_t>(
          3u * current->root.node->height + 1u);
      saw_rotation_retirement = saw_rotation_retirement
          || arena.retired_join_root_count != 0u;
#ifdef BIOSEQ_TEST_INDEXES
      const bool work_monotonic =
          work_after.join_steps >= work_before.join_steps
          && work_after.bounded_validation_steps
              >= work_before.bounded_validation_steps
          && work_after.cleanup_ledger_steps
              >= work_before.cleanup_ledger_steps
          && work_after.retired_root_steps
              >= work_before.retired_root_steps;
      const std::uint64_t join_steps = work_monotonic
          ? work_after.join_steps - work_before.join_steps : UINT64_MAX;
      const std::uint64_t validation_steps = work_monotonic
          ? work_after.bounded_validation_steps
              - work_before.bounded_validation_steps
          : UINT64_MAX;
      const std::uint64_t ledger_steps = work_monotonic
          ? work_after.cleanup_ledger_steps
              - work_before.cleanup_ledger_steps
          : UINT64_MAX;
      const std::uint64_t retirement_steps = work_monotonic
          ? work_after.retired_root_steps
              - work_before.retired_root_steps
          : UINT64_MAX;
#else
      const bool work_monotonic = true;
      const std::uint64_t join_steps = 1u;
      const std::uint64_t validation_steps = 1u;
      const std::uint64_t ledger_steps = 1u;
      const std::uint64_t retirement_steps = 0u;
#endif
      const std::uint64_t total_construction_steps = join_steps
          + validation_steps + ledger_steps + retirement_steps;
      local_valid = local_valid && arena.node_count != 0u
          && arena.node_metadata_bytes == exact_node_bytes
          && arena.arena_dependency_metadata_bytes
              == exact_dependency_bytes
          && arena.source_dependency_count == 0u
          && arena.source_dependency_metadata_bytes == 0u
          && arena.literal_storage_bytes == 0u
          && arena.accounted_bytes
              == exact_node_bytes + exact_dependency_bytes
          && arena.arena_dependency_count <= arena.node_count * 2u
          && arena.retired_join_root_count
              <= current->root.node->height
          && arena.node_count <= generation_node_budget
          && work_monotonic && join_steps != 0u
          && join_steps <= current->root.node->height + 1u
          && validation_steps != 0u
          && validation_steps
              <= static_cast<std::uint64_t>(arena.node_count) * 8u + 16u
          && ledger_steps != 0u
          && ledger_steps
              <= static_cast<std::uint64_t>(
                     current->root.node->height) * 24u + 24u
          && retirement_steps
              <= static_cast<std::uint64_t>(
                     current->root.node->height) * 2u
          && total_construction_steps
              <= static_cast<std::uint64_t>(
                     current->root.node->height) * 64u + 64u
          && piece_tree_live_arena_count_noexcept()
              <= arena_baseline + 2u + (current_count - 1u);
      total_new_nodes += arena.node_count;
      cumulative_node_budget += generation_node_budget;
      maximum_generation_nodes = std::max(
          maximum_generation_nodes, arena.node_count);

      if ((current_count & (current_count - 1u)) == 0u) {
        ++power_height;
        local_valid = local_valid
            && current->root.node->height == power_height;
      }
    }

    auto *final_value = get_pg_sequence_expanded_rw(result);
    local_valid = local_valid && final_value != nullptr && final_value->root
        && power_height == final_height
        && final_value->root.node->piece_count == piece_count
        && final_value->root.node->height == final_height
        && final_value->root.node->bp_length
            == static_cast<std::uint64_t>(piece_count) * piece_length
        && maximum_generation_nodes
            <= static_cast<std::uint32_t>(3u * final_height + 1u)
        && total_new_nodes <= cumulative_node_budget
        && saw_rotation_retirement
        && total_new_nodes
            < static_cast<std::uint64_t>(piece_count) * piece_count / 8u
        && piece_tree_live_arena_count_noexcept()
            <= arena_baseline + 2u + (piece_count - 1u)
        && left_root != nullptr && left_root->bp_length == piece_length
        && left_root->piece_count == 1u && left_root->height == 1u
        && right_root != nullptr && right_root->bp_length == piece_length
        && right_root->piece_count == 1u && right_root->height == 1u;
    if (final_value != nullptr && final_value->root) {
      validate_tree_and_limits(
          final_value->root, type,
          static_cast<std::uint64_t>(piece_count) * piece_length, true);
    }

    constexpr std::uint32_t million_piece_count = 1'000'000u;
    constexpr std::size_t million_power_count = 20u;
    const auto checked_public_concat = [
        &local_valid, type, piece_length](
            Datum lhs, Datum rhs, std::uint32_t expected_pieces) {
      const auto before = read_piece_tree_join_work_noexcept();
      const Datum joined = concat_pg_sequences(lhs, rhs);
      const auto after = read_piece_tree_join_work_noexcept();
      auto *value = get_pg_sequence_expanded_rw(joined);
      local_valid = local_valid && value != nullptr && value->root
          && value->alphabet_bound && value->type_params == type
          && value->root.node->piece_count == expected_pieces
          && value->root.node->bp_length
              == static_cast<std::uint64_t>(expected_pieces) * piece_length;
      if (value == nullptr || !value->root) return joined;
      auto const &arena = *value->root.arena;
#ifdef BIOSEQ_TEST_INDEXES
      const bool monotonic = after.join_steps >= before.join_steps
          && after.bounded_validation_steps
              >= before.bounded_validation_steps
          && after.cleanup_ledger_steps >= before.cleanup_ledger_steps
          && after.retired_root_steps >= before.retired_root_steps;
      const std::uint64_t join_steps = monotonic
          ? after.join_steps - before.join_steps : UINT64_MAX;
      const std::uint64_t validation_steps = monotonic
          ? after.bounded_validation_steps
              - before.bounded_validation_steps
          : UINT64_MAX;
      const std::uint64_t ledger_steps = monotonic
          ? after.cleanup_ledger_steps - before.cleanup_ledger_steps
          : UINT64_MAX;
      const std::uint64_t retirement_steps = monotonic
          ? after.retired_root_steps - before.retired_root_steps
          : UINT64_MAX;
#else
      const bool monotonic = true;
      const std::uint64_t join_steps = 1u;
      const std::uint64_t validation_steps = 1u;
      const std::uint64_t ledger_steps = 1u;
      const std::uint64_t retirement_steps = 0u;
#endif
      const std::uint64_t total_construction_steps = join_steps
          + validation_steps + ledger_steps + retirement_steps;
      local_valid = local_valid && monotonic && arena.node_count != 0u
          && arena.node_count
              <= static_cast<std::uint32_t>(
                  3u * value->root.node->height + 1u)
          && join_steps != 0u
          && join_steps <= value->root.node->height + 1u
          && validation_steps != 0u
          && validation_steps
              <= static_cast<std::uint64_t>(arena.node_count) * 8u + 16u
          && ledger_steps != 0u
          && ledger_steps
              <= static_cast<std::uint64_t>(
                     value->root.node->height) * 24u + 24u
          && retirement_steps
              <= static_cast<std::uint64_t>(
                     value->root.node->height) * 2u
          && total_construction_steps
              <= static_cast<std::uint64_t>(
                     value->root.node->height) * 64u + 64u;
      return joined;
    };

    /* Power roots are read-only snapshots, so concatenating a root with itself
     * exercises the real EOH copy/publish path without mutating the saved
     * operand.  Binary decomposition then reaches exactly 1,000,000 pieces. */
    std::array<Datum, million_power_count> powers{};
    powers[0] = MakeExpandedObjectReadOnly(right, false, -1);
    for (std::size_t power = 1u; power < powers.size(); ++power) {
      const std::uint32_t expected = std::uint32_t{1u} << power;
      const Datum doubled = checked_public_concat(
          powers[power - 1u], powers[power - 1u], expected);
      auto *doubled_value = get_pg_sequence_expanded_rw(doubled);
      local_valid = local_valid && doubled_value != nullptr
          && doubled_value->root.node->height == power + 1u;
      powers[power] = MakeExpandedObjectReadOnly(doubled, false, -1);
    }
    Datum million{};
    std::uint32_t assembled_pieces = 0u;
    bool assembled = false;
    for (int power = static_cast<int>(million_power_count) - 1;
         power >= 0; --power) {
      const std::uint32_t contribution = std::uint32_t{1u} << power;
      if ((million_piece_count & contribution) == 0u) continue;
      if (!assembled) {
        million = powers[static_cast<std::size_t>(power)];
        assembled_pieces = contribution;
        assembled = true;
        continue;
      }
      assembled_pieces += contribution;
      million = checked_public_concat(
          million, powers[static_cast<std::size_t>(power)],
          assembled_pieces);
    }
    auto const *million_value = get_pg_sequence_expanded_ro(million);
    local_valid = local_valid && assembled
        && assembled_pieces == million_piece_count
        && million_value != nullptr && million_value->root
        && million_value->root.node->piece_count == million_piece_count
        && million_value->root.node->bp_length
            == static_cast<std::uint64_t>(million_piece_count) * piece_length
        && million_value->root.node->height >= 20u
        && million_value->root.node->height <= 21u
        && piece_tree_live_arena_count_noexcept()
            <= arena_baseline + 2u + (piece_count - 1u) + 64u;
    if (million_value != nullptr && million_value->root) {
      validate_tree_and_limits(
          million_value->root, type,
          static_cast<std::uint64_t>(million_piece_count) * piece_length,
          true);
    }

    MemoryContextSwitchTo(saved);
    MemoryContextDelete(const_cast<MemoryContext>(probe_context));
    probe_context = nullptr;
    valid = local_valid
        && piece_tree_live_arena_count_noexcept() == arena_baseline;
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(saved);
    if (probe_context != nullptr) {
      MemoryContextDelete(const_cast<MemoryContext>(probe_context));
      probe_context = nullptr;
    }
    FlushErrorState();
    valid = false;
  }
  PG_END_TRY();
  MemoryContextSwitchTo(saved);
  return valid && piece_tree_live_arena_count_noexcept() == arena_baseline;
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
      .storage_unit_byte_width = 8u,
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

export inline Datum materialization_size_self_test_impl(FunctionCallInfo) {
  PG_RETURN_BOOL(run_materialization_size_guardrail_matrix());
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
  const bool public_concat_path_copy =
      run_public_concat_path_copy_probe();
#ifdef BIOSEQ_TEST_INDEXES
  const bool read_session_parent_reset =
      pg_sequence_read_session_parent_reset_self_test();
#else
  const bool read_session_parent_reset = true;
#endif
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
          [bytes](piece_tree_arena &arena,
                  piece_edge_cleanup_ledger &ledger) {
            auto *storage = allocate_piece_literal_storage(arena, bytes.size());
            std::memcpy(storage, bytes.data(), bytes.size());
            return make_literal_node_error_atomic(
                arena, ledger,
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
    valid = concat_length_guardrail_matrix && public_concat_path_copy
        && read_session_parent_reset
        && reparented && transfer_survived && copy_survived;

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

CXX_PG_FUNCTION_INFO_V1(materialization_size_self_test);
PGDLLEXPORT Datum materialization_size_self_test(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&materialization_size_self_test_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(expanded_ownership_self_test);
PGDLLEXPORT Datum expanded_ownership_self_test(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&expanded_ownership_self_test_impl, fcinfo);
}
} // extern "C"
