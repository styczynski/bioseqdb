/* Persistent immutable piece-tree construction, splitting, joining, binding,
 * and nonrecursive arena release. */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <utility>

#include "headers/pg_compat.h"

export module bioseq.types.pg_compact_sequence:tree_ops;

import bioseq.codegen.sql.annotation_model;
import :flat_abi;
import :source_abi;
import :tree_types;
import bioseq.sequences.rank_pack;
import bioseq.storage.coordinates;

/* Exported inline tree factories may be emitted from any importing TU at
 * optimization level zero.  Keep the fault-injection seam in one out-of-line
 * module-owned object so the self-test and the selected factory body cannot
 * arm and observe different anonymous-namespace copies. */
export [[nodiscard, gnu::noinline]] std::array<std::uint8_t, 2u>&
piece_tree_failure_storage_noexcept() noexcept
{
    static thread_local std::array<std::uint8_t, 2u> storage{};
    return storage;
}

export enum class piece_tree_quarantine_reason : std::uint8_t {
    none,
    arena_refcount,
    arena_dependency,
    arena_live_counter,
    cleanup_ledger,
    retired_join_root,
};

export struct piece_tree_quarantine_snapshot {
    std::uint64_t invariant_failures;
    std::uint64_t quarantined_arenas;
    piece_tree_quarantine_reason first_reason;
    piece_tree_quarantine_reason last_reason;
    char const* first_detail;
    char const* last_detail;
    std::uint64_t last_arena_generation;
};

/* Exported inline release paths can be emitted by an importing translation
 * unit.  Keep their no-throw diagnostic state in one module-owned object. */
export [[nodiscard, gnu::noinline]] piece_tree_quarantine_snapshot&
piece_tree_quarantine_storage_noexcept() noexcept
{
    static thread_local piece_tree_quarantine_snapshot storage{};
    return storage;
}

export [[nodiscard]] inline piece_tree_quarantine_snapshot
read_piece_tree_quarantine_noexcept() noexcept
{
    return piece_tree_quarantine_storage_noexcept();
}

namespace {

std::uint64_t next_piece_tree_generation = 1u;
std::uint64_t live_piece_tree_arenas = 0u;

inline void mark_piece_tree_arena_quarantined_noexcept(
    piece_tree_quarantine_snapshot& state,
    piece_tree_arena* arena) noexcept
{
    if (arena == nullptr) return;
    state.last_arena_generation = arena->generation;
    if (arena->refcount == UINT32_MAX) return;
    arena->refcount = UINT32_MAX;
    arena->release_next = nullptr;
    if (state.quarantined_arenas != UINT64_MAX) {
        ++state.quarantined_arenas;
    }
}

inline void record_piece_tree_quarantine_noexcept(
    piece_tree_quarantine_snapshot& state,
    piece_tree_quarantine_reason reason,
    char const* detail,
    piece_tree_arena* first_arena = nullptr,
    piece_tree_arena* second_arena = nullptr) noexcept
{
    if (state.invariant_failures == 0u) {
        state.first_reason = reason;
        state.first_detail = detail;
    }
    if (state.invariant_failures != UINT64_MAX) {
        ++state.invariant_failures;
    }
    state.last_reason = reason;
    state.last_detail = detail;
    mark_piece_tree_arena_quarantined_noexcept(state, first_arena);
    mark_piece_tree_arena_quarantined_noexcept(state, second_arena);
}

inline void quarantine_piece_tree_invariant_noexcept(
    piece_tree_quarantine_reason reason,
    char const* detail,
    piece_tree_arena* first_arena = nullptr,
    piece_tree_arena* second_arena = nullptr) noexcept
{
    record_piece_tree_quarantine_noexcept(
        piece_tree_quarantine_storage_noexcept(),
        reason,
        detail,
        first_arena,
        second_arena);
}

/* Backend-local acceptance seam.  Production calls leave this disarmed; the
 * internal SQL self-test arms one dependency-registration point at a time so
 * PostgreSQL ERROR can interrupt a factory at the only point where a returned
 * edge has not yet made the partial retain visible to its caller. */
enum class piece_tree_injected_failure_point : std::uint8_t {
    none,
    arena_factory,
    literal_storage_factory,
    literal_leaf_factory,
    branch_factory,
    arena_dependency_registration,
    source_dependency_registration,
    source_leaf_factory,
    literal_binding_factory,
    join_left_operand_retain,
    join_right_operand_retain,
    join_node_allocation,
    join_node_metadata,
    join_factory_transition,
    join_edge_identity_transition,
    join_recursive_intermediate,
    join_retired_root_record,
    join_retired_root_overflow_allocation,
    split_first_leaf_intermediate,
};

inline void arm_piece_tree_failure_noexcept(
    piece_tree_injected_failure_point point) noexcept
{
    piece_tree_failure_storage_noexcept()[0] =
        static_cast<std::uint8_t>(point);
}

[[nodiscard]] inline piece_tree_injected_failure_point
armed_piece_tree_failure_noexcept() noexcept
{
    return static_cast<piece_tree_injected_failure_point>(
        piece_tree_failure_storage_noexcept()[0]);
}

inline void observe_piece_tree_failure_noexcept(
    piece_tree_injected_failure_point point) noexcept
{
    piece_tree_failure_storage_noexcept()[1] =
        static_cast<std::uint8_t>(point);
}

[[nodiscard]] inline piece_tree_injected_failure_point
observed_piece_tree_failure_noexcept() noexcept
{
    return static_cast<piece_tree_injected_failure_point>(
        piece_tree_failure_storage_noexcept()[1]);
}

inline void inject_piece_tree_failure(
    piece_tree_injected_failure_point point)
{
    if (armed_piece_tree_failure_noexcept() != point) {
        return;
    }
    arm_piece_tree_failure_noexcept(
        piece_tree_injected_failure_point::none);
    observe_piece_tree_failure_noexcept(point);
    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("injected bioseq piece-tree factory failure"),
             errdetail("failure_point=%u",
                       static_cast<unsigned>(point))));
}

[[nodiscard]] inline std::uint16_t edge_height(piece_edge edge) noexcept {
  return edge ? edge.node->height : 0u;
}

[[nodiscard]] inline bool
same_source_handle(pg_sequence_source_handle left,
                   pg_sequence_source_handle right) noexcept {
  return pg_sequence_source_handle_valid(left) &&
         pg_sequence_source_handle_valid(right) &&
         left.ops->identity(left.state) == right.ops->identity(right.state);
}

[[nodiscard]] inline bool
same_locator(bioseq::storage::seq_locator const &left,
             bioseq::storage::seq_locator const &right) noexcept {
  return std::memcmp(std::addressof(left), std::addressof(right),
                     sizeof(left)) == 0;
}

} // namespace

/* Deterministic complexity evidence for the production persistent-join path.
 * The storage accessor is out of line so exported inline tree operations from
 * different importers all report into one backend-thread instance. */
export struct piece_tree_join_work_snapshot {
    std::uint64_t join_steps;
    std::uint64_t bounded_validation_steps;
    std::uint64_t cleanup_ledger_steps;
    std::uint64_t retired_root_steps;
};

export [[nodiscard, gnu::noinline]] piece_tree_join_work_snapshot&
piece_tree_join_work_storage_noexcept() noexcept
{
    static thread_local piece_tree_join_work_snapshot storage{};
    return storage;
}

export inline void reset_piece_tree_join_work_noexcept() noexcept
{
    piece_tree_join_work_storage_noexcept() = {};
}

export [[nodiscard]] inline piece_tree_join_work_snapshot
read_piece_tree_join_work_noexcept() noexcept
{
    return piece_tree_join_work_storage_noexcept();
}

namespace {

inline void note_piece_tree_join_step_noexcept() noexcept
{
#ifdef BIOSEQ_TEST_INDEXES
    auto& value = piece_tree_join_work_storage_noexcept().join_steps;
    if (value != (std::numeric_limits<std::uint64_t>::max)()) ++value;
#endif
}

inline void note_piece_tree_bounded_validation_step_noexcept() noexcept
{
#ifdef BIOSEQ_TEST_INDEXES
    auto& value = piece_tree_join_work_storage_noexcept().bounded_validation_steps;
    if (value != (std::numeric_limits<std::uint64_t>::max)()) ++value;
#endif
}

inline void note_piece_tree_cleanup_ledger_step_noexcept() noexcept
{
#ifdef BIOSEQ_TEST_INDEXES
    auto& value = piece_tree_join_work_storage_noexcept().cleanup_ledger_steps;
    if (value != (std::numeric_limits<std::uint64_t>::max)()) ++value;
#endif
}

inline void note_piece_tree_retired_root_step_noexcept() noexcept
{
#ifdef BIOSEQ_TEST_INDEXES
    auto& value = piece_tree_join_work_storage_noexcept().retired_root_steps;
    if (value != (std::numeric_limits<std::uint64_t>::max)()) ++value;
#endif
}

} // namespace

export [[nodiscard]] inline piece_tree_arena *make_piece_tree_arena() {
  MemoryContext saved = CurrentMemoryContext;
  piece_tree_arena *volatile arena = nullptr;
  PG_TRY();
  {
    MemoryContextSwitchTo(TopMemoryContext);
    arena = static_cast<piece_tree_arena *>(palloc0(sizeof(piece_tree_arena)));
    MemoryContextSwitchTo(saved);
    /* Both the arena token and payload are independent of every EOH and
     * per-tuple context.  Cross-EOH sharing is governed solely by the
     * arena refcount; release_arena explicitly deletes this context. */
    arena->payload_context = AllocSetContextCreate(
        TopMemoryContext, "bioseq piece arena", ALLOCSET_SMALL_SIZES);
    arena->refcount = 1u;
    if (next_piece_tree_generation == 0u) {
      ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                      errmsg("bioseq piece arena generation overflow")));
    }
    arena->generation = next_piece_tree_generation++;
    ++live_piece_tree_arenas;
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(saved);
    if (arena != nullptr) {
      if (arena->payload_context != nullptr) {
        MemoryContextDelete(arena->payload_context);
        arena->payload_context = nullptr;
      }
      pfree(const_cast<piece_tree_arena *>(arena));
    }
    PG_RE_THROW();
  }
  PG_END_TRY();
  return const_cast<piece_tree_arena *>(arena);
}

export inline void retain_arena(piece_tree_arena *arena) noexcept {
  if (arena == nullptr) {
    return;
  }
  if (arena->refcount == 0u) {
    ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                    errmsg("cannot retain a released bioseq piece arena")));
  }
  if (arena->refcount == UINT32_MAX) {
    ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                    errmsg("bioseq piece arena reference count overflow")));
  }
  ++arena->refcount;
}

export inline void release_arena(piece_tree_arena *arena) noexcept {
  if (arena == nullptr) {
    return;
  }
  if (arena->refcount == 0u || arena->refcount == UINT32_MAX) {
    quarantine_piece_tree_invariant_noexcept(
        piece_tree_quarantine_reason::arena_refcount,
        "invalid root refcount",
        arena);
    return;
  }
  if (--arena->refcount != 0u) {
    return;
  }
  piece_tree_arena *work = arena;
  while (work != nullptr) {
    piece_tree_arena *current = work;
    work = work->release_next;
    if (!current->dependencies_released) {
      current->dependencies_released = true;
      for (auto *dependency = current->source_dependencies;
           dependency != nullptr; dependency = dependency->next) {
        release_pg_sequence_source(dependency->source);
      }
      for (auto *dependency = current->arena_dependencies;
           dependency != nullptr; dependency = dependency->next) {
        piece_tree_arena *child = dependency->child;
        if (child == nullptr || child->generation >= current->generation) {
          quarantine_piece_tree_invariant_noexcept(
              piece_tree_quarantine_reason::arena_dependency,
              "dependency is null, cyclic, or newer than its parent",
              current,
              child);
          return;
        }
        if (child->refcount == 0u || child->refcount == UINT32_MAX) {
          quarantine_piece_tree_invariant_noexcept(
              piece_tree_quarantine_reason::arena_dependency,
              "dependency refcount is invalid",
              current,
              child);
          return;
        }
        if (--child->refcount == 0u) {
          child->release_next = work;
          work = child;
        }
      }
    }
    if (live_piece_tree_arenas == 0u) {
      quarantine_piece_tree_invariant_noexcept(
          piece_tree_quarantine_reason::arena_live_counter,
          "live-arena counter underflow",
          current);
      return;
    }
    MemoryContextDelete(current->payload_context);
    // Exactly one decrement corresponds to the one arena allocation freed
    // below.  Keeping it adjacent to pfree prevents a partial cleanup path
    // from manufacturing the next release's underflow.
    --live_piece_tree_arenas;
    pfree(current);
  }
}

export [[nodiscard]] inline piece_edge retain_edge(piece_edge edge)
{
    if (edge) {
        retain_arena(edge.arena);
    }
    return edge;
}

export inline void release_edge(piece_edge edge) noexcept
{
    if (edge) {
        release_arena(edge.arena);
    }
}

export [[nodiscard]] inline bool piece_edge_cleanup_ledger_empty_noexcept(
    piece_edge_cleanup_ledger const& ledger) noexcept
{
    note_piece_tree_cleanup_ledger_step_noexcept();
    if ((ledger.token_count == 0u && ledger.initialized
            && ledger.active_head != invalid_piece_edge_cleanup_slot)
        || (ledger.token_count != 0u
            && (!ledger.initialized
                || ledger.active_head == invalid_piece_edge_cleanup_slot))
        || (!ledger.initialized
            && ledger.next_unused == invalid_piece_edge_cleanup_slot)) {
        quarantine_piece_tree_invariant_noexcept(
            piece_tree_quarantine_reason::cleanup_ledger,
            "concat construction active-token summary is corrupt");
        return false;
    }
    return ledger.token_count == 0u;
}

namespace {

[[nodiscard]] inline bool same_owned_edge_token(
    piece_edge left, piece_edge right) noexcept
{
    return left.arena == right.arena && left.node == right.node
        && left.transform == right.transform;
}

[[nodiscard]] inline bool piece_edge_cleanup_handle_valid_noexcept(
    piece_edge_cleanup_handle handle) noexcept
{
    return handle.slot != invalid_piece_edge_cleanup_slot
        && handle.generation != 0u;
}

inline void quarantine_piece_edge_cleanup_ledger_noexcept(
    piece_edge_cleanup_ledger& ledger,
    char const* detail,
    piece_tree_arena* suspect_arena = nullptr) noexcept
{
    quarantine_piece_tree_invariant_noexcept(
        piece_tree_quarantine_reason::cleanup_ledger,
        detail,
        suspect_arena);
    // Preserve every owning token in place, but poison the summary so this
    // ledger can neither be reused nor mistaken for a successful drain.
    ledger.initialized = false;
    ledger.next_unused = invalid_piece_edge_cleanup_slot;
    ledger.active_head = invalid_piece_edge_cleanup_slot;
    ledger.free_head = invalid_piece_edge_cleanup_slot;
}

[[nodiscard]] inline bool initialize_piece_edge_cleanup_ledger_noexcept(
    piece_edge_cleanup_ledger& ledger) noexcept
{
    if (ledger.initialized) return true;
    if (ledger.token_count != 0u || ledger.next_unused != 0u) {
        quarantine_piece_edge_cleanup_ledger_noexcept(
            ledger,
            "concat construction token ledger was not zero-initialized");
        return false;
    }
    ledger.active_head = invalid_piece_edge_cleanup_slot;
    ledger.free_head = invalid_piece_edge_cleanup_slot;
    ledger.initialized = true;
    note_piece_tree_cleanup_ledger_step_noexcept();
    return true;
}

[[nodiscard]] inline bool ensure_piece_edge_cleanup_capacity(
    piece_edge_cleanup_ledger& ledger, std::uint16_t additional)
{
    if (!initialize_piece_edge_cleanup_ledger_noexcept(ledger)) {
        return false;
    }
    note_piece_tree_cleanup_ledger_step_noexcept();
    if (additional > ledger.slots.size()
        || ledger.token_count > ledger.slots.size() - additional) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq concat construction token bound was exceeded")));
    }
    return true;
}

[[nodiscard]] inline piece_edge_cleanup_handle
adopt_piece_edge_cleanup_token_noexcept(
    piece_edge_cleanup_ledger& ledger, piece_edge token) noexcept
{
    if (!token) return {};
    if (!initialize_piece_edge_cleanup_ledger_noexcept(ledger)) {
        mark_piece_tree_arena_quarantined_noexcept(
            piece_tree_quarantine_storage_noexcept(), token.arena);
        return {};
    }
    if (ledger.token_count >= ledger.slots.size()) {
        quarantine_piece_edge_cleanup_ledger_noexcept(
            ledger,
            "concat construction token ledger overflow",
            token.arena);
        return {};
    }
    std::uint16_t index = invalid_piece_edge_cleanup_slot;
    if (ledger.free_head != invalid_piece_edge_cleanup_slot) {
        if (ledger.free_head >= ledger.slots.size()) {
            quarantine_piece_edge_cleanup_ledger_noexcept(
                ledger,
                "concat construction free-token head is corrupt",
                token.arena);
            return {};
        }
        index = ledger.free_head;
        ledger.free_head = ledger.slots[index].next_free;
    } else {
        if (ledger.next_unused >= ledger.slots.size()) {
            quarantine_piece_edge_cleanup_ledger_noexcept(
                ledger,
                "concat construction token slots were exhausted",
                token.arena);
            return {};
        }
        index = ledger.next_unused++;
    }
    auto& slot = ledger.slots[index];
    if (slot.active || slot.generation == UINT32_MAX) {
        quarantine_piece_edge_cleanup_ledger_noexcept(
            ledger,
            "concat construction token slot state is invalid",
            token.arena);
        return {};
    }
    if (ledger.active_head != invalid_piece_edge_cleanup_slot
        && ledger.active_head >= ledger.slots.size()) {
        quarantine_piece_edge_cleanup_ledger_noexcept(
            ledger,
            "concat construction active-token head is corrupt",
            token.arena);
        return {};
    }
    ++slot.generation;
    slot.token = token;
    slot.previous_active = invalid_piece_edge_cleanup_slot;
    slot.next_active = ledger.active_head;
    slot.next_free = invalid_piece_edge_cleanup_slot;
    slot.active = true;
    if (ledger.active_head != invalid_piece_edge_cleanup_slot) {
        ledger.slots[ledger.active_head].previous_active = index;
    }
    ledger.active_head = index;
    ++ledger.token_count;
    note_piece_tree_cleanup_ledger_step_noexcept();
    return {.slot = index, .generation = slot.generation};
}

struct detached_piece_edge {
    piece_edge token;
    bool detached;
};

[[nodiscard]] inline detached_piece_edge
detach_piece_edge_cleanup_token_noexcept(
    piece_edge_cleanup_ledger& ledger,
    piece_edge_cleanup_handle handle,
    piece_edge expected) noexcept
{
    if (!piece_edge_cleanup_handle_valid_noexcept(handle)
        || handle.slot >= ledger.slots.size()) {
        quarantine_piece_edge_cleanup_ledger_noexcept(
            ledger,
            "concat construction token handle is invalid",
            expected.arena);
        return {};
    }
    auto& slot = ledger.slots[handle.slot];
    if (!ledger.initialized || ledger.token_count == 0u || !slot.active
        || slot.generation != handle.generation
        || !same_owned_edge_token(slot.token, expected)) {
        quarantine_piece_edge_cleanup_ledger_noexcept(
            ledger,
            "concat construction token handle is stale or mismatched",
            expected.arena);
        return {};
    }
    const auto valid_link = [&ledger](std::uint16_t index) noexcept {
        return index == invalid_piece_edge_cleanup_slot
            || index < ledger.slots.size();
    };
    if (!valid_link(slot.previous_active) || !valid_link(slot.next_active)) {
        quarantine_piece_edge_cleanup_ledger_noexcept(
            ledger,
            "concat construction active-token links are corrupt",
            expected.arena);
        return {};
    }
    if (slot.previous_active == invalid_piece_edge_cleanup_slot) {
        if (ledger.active_head != handle.slot) {
            quarantine_piece_edge_cleanup_ledger_noexcept(
                ledger,
                "concat construction active-token head is corrupt",
                expected.arena);
            return {};
        }
    } else {
        const auto& previous = ledger.slots[slot.previous_active];
        if (!previous.active || previous.next_active != handle.slot) {
            quarantine_piece_edge_cleanup_ledger_noexcept(
                ledger,
                "concat construction previous-token link is corrupt",
                expected.arena);
            return {};
        }
    }
    if (slot.next_active != invalid_piece_edge_cleanup_slot) {
        const auto& next = ledger.slots[slot.next_active];
        if (!next.active || next.previous_active != handle.slot) {
            quarantine_piece_edge_cleanup_ledger_noexcept(
                ledger,
                "concat construction next-token link is corrupt",
                expected.arena);
            return {};
        }
    }

    if (slot.previous_active == invalid_piece_edge_cleanup_slot) {
        ledger.active_head = slot.next_active;
    } else {
        ledger.slots[slot.previous_active].next_active = slot.next_active;
    }
    if (slot.next_active != invalid_piece_edge_cleanup_slot) {
        ledger.slots[slot.next_active].previous_active =
            slot.previous_active;
    }
    const piece_edge token = slot.token;
    slot.token = {};
    slot.next_active = invalid_piece_edge_cleanup_slot;
    slot.previous_active = invalid_piece_edge_cleanup_slot;
    slot.next_free = ledger.free_head;
    slot.active = false;
    ledger.free_head = handle.slot;
    --ledger.token_count;
    note_piece_tree_cleanup_ledger_step_noexcept();
    return {.token = token, .detached = true};
}

[[nodiscard]] inline tracked_piece_edge retain_tracked_edge(
    piece_edge_cleanup_ledger* ledger, piece_edge edge)
{
    if (ledger == nullptr) return {.edge = retain_edge(edge)};
    if (!edge) return {};
    if (!ensure_piece_edge_cleanup_capacity(*ledger, 1u)) return {};
    const piece_edge owned = retain_edge(edge);
    const auto cleanup =
        adopt_piece_edge_cleanup_token_noexcept(*ledger, owned);
    if (!piece_edge_cleanup_handle_valid_noexcept(cleanup)) return {};
    return {
        .edge = owned,
        .cleanup = cleanup,
    };
}

inline void release_tracked_edge(
    piece_edge_cleanup_ledger* ledger, tracked_piece_edge edge) noexcept
{
    if (ledger != nullptr && edge) {
        const auto detached = detach_piece_edge_cleanup_token_noexcept(
            *ledger, edge.cleanup, edge.edge);
        if (!detached.detached) return;
    }
    release_edge(edge.edge);
}

[[nodiscard]] inline tracked_piece_edge
transition_consumed_edges_to_result_noexcept(
    piece_edge_cleanup_ledger* ledger,
    tracked_piece_edge first,
    tracked_piece_edge second,
    piece_edge result) noexcept
{
    if (ledger == nullptr) return {.edge = result};
    if (first) {
        const auto detached = detach_piece_edge_cleanup_token_noexcept(
            *ledger, first.cleanup, first.edge);
        if (!detached.detached) return {};
    }
    if (second) {
        const auto detached = detach_piece_edge_cleanup_token_noexcept(
            *ledger, second.cleanup, second.edge);
        if (!detached.detached) return {};
    }
    const auto cleanup =
        adopt_piece_edge_cleanup_token_noexcept(*ledger, result);
    if (!piece_edge_cleanup_handle_valid_noexcept(cleanup)) return {};
    return {
        .edge = result,
        .cleanup = cleanup,
    };
}

inline void transition_piece_edge_cleanup_identity_noexcept(
    piece_edge_cleanup_ledger* ledger,
    tracked_piece_edge before,
    tracked_piece_edge after) noexcept
{
    if (ledger == nullptr) return;
    if (!before || !after || before.edge.arena != after.edge.arena
        || before.edge.node != after.edge.node
        || before.cleanup.slot != after.cleanup.slot
        || before.cleanup.generation != after.cleanup.generation
        || before.cleanup.slot >= ledger->slots.size()) {
        quarantine_piece_edge_cleanup_ledger_noexcept(
            *ledger,
            "concat construction changed an owning edge target",
            before.edge.arena != nullptr
                ? before.edge.arena
                : after.edge.arena);
        return;
    }
    auto& slot = ledger->slots[before.cleanup.slot];
    if (!slot.active || slot.generation != before.cleanup.generation
        || !same_owned_edge_token(slot.token, before.edge)) {
        quarantine_piece_edge_cleanup_ledger_noexcept(
            *ledger,
            "concat construction changed an untracked edge identity",
            before.edge.arena);
        return;
    }
    slot.token = after.edge;
    note_piece_tree_cleanup_ledger_step_noexcept();
}

} // namespace

export inline void release_piece_edge_cleanup_ledger_noexcept(
    piece_edge_cleanup_ledger& ledger) noexcept
{
    if (!ledger.initialized) {
        if (ledger.token_count != 0u
            || ledger.next_unused == invalid_piece_edge_cleanup_slot) {
            quarantine_piece_edge_cleanup_ledger_noexcept(
                ledger,
                "uninitialized concat token ledger is nonempty");
        }
        return;
    }
    while (ledger.token_count != 0u) {
        if (ledger.active_head == invalid_piece_edge_cleanup_slot
            || ledger.active_head >= ledger.slots.size()) {
            quarantine_piece_edge_cleanup_ledger_noexcept(
                ledger,
                "concat construction active-token list ended early");
            return;
        }
        const auto& slot = ledger.slots[ledger.active_head];
        const piece_edge_cleanup_handle handle{
            .slot = ledger.active_head,
            .generation = slot.generation,
        };
        const auto detached = detach_piece_edge_cleanup_token_noexcept(
            ledger, handle, slot.token);
        if (!detached.detached) return;
        release_edge(detached.token);
    }
    if (ledger.active_head != invalid_piece_edge_cleanup_slot) {
        quarantine_piece_edge_cleanup_ledger_noexcept(
            ledger,
            "concat construction active-token list was not drained");
    }
}

export inline void transfer_piece_edge_cleanup_root_noexcept(
    piece_edge_cleanup_ledger& ledger,
    piece_edge_cleanup_handle handle,
    piece_edge root) noexcept
{
    if (!root) {
        if (piece_edge_cleanup_handle_valid_noexcept(handle)) {
            quarantine_piece_edge_cleanup_ledger_noexcept(
                ledger,
                "empty concat root retained a cleanup handle");
        }
        return;
    }
    const auto detached = detach_piece_edge_cleanup_token_noexcept(
        ledger, handle, root);
    (void)detached;
}

export [[nodiscard]] inline std::uint64_t
piece_tree_live_arena_count_noexcept() noexcept
{
    return live_piece_tree_arenas;
}

namespace {

enum class arena_allocation_kind : std::uint8_t {
    node,
    arena_dependency,
    source_dependency,
    literal_storage,
    retired_join_root_metadata,
};

[[nodiscard]] inline Size checked_arena_allocation_size(Size requested)
{
    const Size nonzero = std::max<Size>(requested, 1u);
    if (nonzero > MaxAllocSize) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq piece arena allocation exceeds MaxAllocSize"),
                 errdetail("requested_bytes=%zu limit_bytes=%zu",
                           static_cast<std::size_t>(nonzero),
                           static_cast<std::size_t>(MaxAllocSize))));
    }
    const Size aligned = MAXALIGN(nonzero);
    if (aligned < nonzero || aligned > MaxAllocSize) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq aligned piece arena allocation exceeds MaxAllocSize"),
                 errdetail("requested_bytes=%zu aligned_bytes=%zu limit_bytes=%zu",
                           static_cast<std::size_t>(nonzero),
                           static_cast<std::size_t>(aligned),
                           static_cast<std::size_t>(MaxAllocSize))));
    }
    return aligned;
}

inline void reserve_arena_allocation(
    piece_tree_arena& destination,
    Size requested,
    arena_allocation_kind kind)
{
    const Size aligned = checked_arena_allocation_size(requested);
    if (aligned > MaxAllocSize
        || destination.accounted_bytes > MaxAllocSize
        || destination.accounted_bytes > MaxAllocSize - aligned) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq piece metadata exceeds MaxAllocSize"),
                 errdetail("accounted_bytes=%zu requested_bytes=%zu "
                           "piece_count=%u limit_bytes=%zu",
                           static_cast<std::size_t>(
                               destination.accounted_bytes),
                           static_cast<std::size_t>(aligned),
                           destination.node_count,
                           static_cast<std::size_t>(MaxAllocSize))));
    }
    destination.accounted_bytes += aligned;
    switch (kind) {
        case arena_allocation_kind::node:
            destination.node_metadata_bytes += aligned;
            if (destination.node_count == UINT32_MAX) {
                ereport(ERROR,
                        (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                         errmsg("bioseq piece node count overflow")));
            }
            ++destination.node_count;
            break;
        case arena_allocation_kind::arena_dependency:
            destination.arena_dependency_metadata_bytes += aligned;
            if (destination.arena_dependency_count == UINT32_MAX) {
                ereport(ERROR,
                        (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                         errmsg("bioseq arena dependency count overflow")));
            }
            ++destination.arena_dependency_count;
            break;
        case arena_allocation_kind::source_dependency:
            destination.source_dependency_metadata_bytes += aligned;
            if (destination.source_dependency_count == UINT32_MAX) {
                ereport(ERROR,
                        (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                         errmsg("bioseq source dependency count overflow")));
            }
            ++destination.source_dependency_count;
            break;
        case arena_allocation_kind::literal_storage:
            destination.literal_storage_bytes += aligned;
            break;
        case arena_allocation_kind::retired_join_root_metadata:
            destination.retired_join_root_metadata_bytes += aligned;
            if (destination.retired_join_root_overflow_block_count
                == UINT32_MAX) {
                ereport(ERROR,
                        (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                         errmsg("bioseq retired join-root block count overflow")));
            }
            ++destination.retired_join_root_overflow_block_count;
            break;
    }
}

inline void register_arena_dependency(
    piece_tree_arena& destination,
    piece_tree_arena* child)
{
    if (child == nullptr || child == std::addressof(destination)) {
        return;
    }
    if (child->generation == 0u
        || child->generation >= destination.generation) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq piece arena dependency violates generation order"),
                 errdetail("parent_generation=%llu child_generation=%llu",
                           static_cast<unsigned long long>(
                               destination.generation),
                           static_cast<unsigned long long>(
                               child->generation))));
    }
    if (child->refcount == 0u) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq piece arena dependency is already released")));
    }
    if (child->refcount == UINT32_MAX) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq piece arena dependency reference count overflow")));
    }
    reserve_arena_allocation(
        destination, sizeof(arena_dependency),
        arena_allocation_kind::arena_dependency);
    auto* dependency = static_cast<arena_dependency*>(
        MemoryContextAllocZero(destination.payload_context,
                               sizeof(arena_dependency)));
    retain_arena(child);
    dependency->child = child;
    dependency->next = destination.arena_dependencies;
    destination.arena_dependencies = dependency;
    inject_piece_tree_failure(
        piece_tree_injected_failure_point::arena_dependency_registration);
}

inline void register_source_dependency(
    piece_tree_arena& destination,
    pg_sequence_source_handle source)
{
    if (!pg_sequence_source_handle_valid(source)) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("invalid bioseq source handle in piece tree")));
    }
    reserve_arena_allocation(
        destination, sizeof(source_dependency),
        arena_allocation_kind::source_dependency);
    auto* dependency = static_cast<source_dependency*>(
        MemoryContextAllocZero(destination.payload_context,
                               sizeof(source_dependency)));
    retain_pg_sequence_source(source);
    dependency->source = source;
    dependency->next = destination.source_dependencies;
    destination.source_dependencies = dependency;
    inject_piece_tree_failure(
        piece_tree_injected_failure_point::source_dependency_registration);
}

[[nodiscard]] inline piece_node* allocate_piece_node(
    piece_tree_arena& destination)
{
    reserve_arena_allocation(
        destination, sizeof(piece_node), arena_allocation_kind::node);
    auto* node = static_cast<piece_node*>(
        MemoryContextAllocZero(destination.payload_context,
                               sizeof(piece_node)));
    inject_piece_tree_failure(
        piece_tree_injected_failure_point::join_node_allocation);
    return node;
}

[[nodiscard]] inline std::uint64_t checked_tree_length_add(
    std::uint64_t left,
    std::uint64_t right)
{
    constexpr auto max_logical_length =
        static_cast<std::uint64_t>(INT64_MAX);
    if (left > max_logical_length || right > max_logical_length
        || right > max_logical_length - left) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq logical length exceeds bigint")));
    }
    return left + right;
}

[[nodiscard]] inline std::uint32_t checked_piece_count_add(
    std::uint32_t left,
    std::uint32_t right)
{
    const std::uint64_t total =
        static_cast<std::uint64_t>(left) + right;
    if (total > max_piece_count) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq piece metadata exceeds MaxAllocSize"),
                 errdetail("piece_count=%llu limit=%u",
                           static_cast<unsigned long long>(total),
                           max_piece_count)));
    }
    return static_cast<std::uint32_t>(total);
}

} // namespace

export [[nodiscard]] inline std::byte* allocate_piece_literal_storage(
    piece_tree_arena& destination,
    std::size_t byte_count)
{
    const Size allocation = std::max<Size>(
        static_cast<Size>(byte_count), 1u);
    reserve_arena_allocation(
        destination, allocation, arena_allocation_kind::literal_storage);
    auto* storage = static_cast<std::byte*>(
        MemoryContextAllocZero(destination.payload_context, allocation));
    inject_piece_tree_failure(
        piece_tree_injected_failure_point::literal_storage_factory);
    return storage;
}

export [[nodiscard]] inline piece_edge make_branch_node(
    piece_tree_arena& destination,
    piece_edge left,
    piece_edge right)
{
    if (!left) {
        return right;
    }
    if (!right) {
        return left;
    }
    register_arena_dependency(destination, left.arena);
    register_arena_dependency(destination, right.arena);
    auto* node = allocate_piece_node(destination);
    node->node_kind = piece_node::kind::branch;
    node->bp_length = checked_tree_length_add(
        left.node->bp_length, right.node->bp_length);
    node->piece_count = checked_piece_count_add(
        left.node->piece_count, right.node->piece_count);
    node->height = static_cast<std::uint16_t>(
        1u + std::max(edge_height(left), edge_height(right)));
    ::new (static_cast<void*>(std::addressof(node->payload.branch)))
        branch_piece{left, right};
    inject_piece_tree_failure(
        piece_tree_injected_failure_point::join_node_metadata);
    /* Acquire the returned token before consuming either input.  If retain
     * itself fails, an ERROR cleanup ledger still owns both inputs. */
    const piece_edge result = retain_edge(piece_edge{
        .arena = std::addressof(destination),
        .node = node,
        .transform = bioseq::storage::sequence_transform::identity,
    });
    release_edge(left);
    release_edge(right);
    return result;
}

export [[nodiscard]] inline piece_edge make_source_node(
    piece_tree_arena& destination,
    source_piece piece,
    bioseq::storage::sequence_transform transform =
        bioseq::storage::sequence_transform::identity)
{
    if (piece.from > piece.to
        || piece.to > piece.locator.total_bp_size) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq source piece is outside its locator")));
    }
    register_source_dependency(destination, piece.source);
    auto* node = allocate_piece_node(destination);
    node->node_kind = piece_node::kind::source;
    node->bp_length = piece.to - piece.from;
    node->piece_count = 1u;
    node->height = 1u;
    ::new (static_cast<void*>(std::addressof(node->payload.source)))
        source_piece{piece};
    inject_piece_tree_failure(
        piece_tree_injected_failure_point::join_node_metadata);
    return retain_edge(piece_edge{
        .arena = std::addressof(destination),
        .node = node,
        .transform = transform,
    });
}

export [[nodiscard]] inline piece_edge make_literal_node(
    piece_tree_arena& destination,
    literal_piece piece,
    bioseq::storage::sequence_transform transform =
        bioseq::storage::sequence_transform::identity)
{
    if (piece.symbol_from > piece.symbol_to || piece.bytes == nullptr
        || piece.storage_arena == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("invalid bioseq literal piece")));
    }
    register_arena_dependency(destination, piece.storage_arena);
    auto* node = allocate_piece_node(destination);
    node->node_kind = piece_node::kind::literal;
    node->bp_length = piece.symbol_to - piece.symbol_from;
    node->piece_count = 1u;
    node->height = 1u;
    ::new (static_cast<void*>(std::addressof(node->payload.literal)))
        literal_piece{piece};
    inject_piece_tree_failure(
        piece_tree_injected_failure_point::join_node_metadata);
    return retain_edge(piece_edge{
        .arena = std::addressof(destination),
        .node = node,
        .transform = transform,
    });
}

/* Reserve cleanup capacity before entering a leaf factory.  Once a factory
 * returns its owning arena edge, adopting it into the operation ledger is
 * noexcept, so PostgreSQL ERROR can never observe an unowned returned edge. */
export [[nodiscard]] inline tracked_piece_edge
make_source_node_error_atomic(
    piece_tree_arena& destination,
    piece_edge_cleanup_ledger& ledger,
    source_piece piece,
    bioseq::storage::sequence_transform transform =
        bioseq::storage::sequence_transform::identity)
{
    if (!ensure_piece_edge_cleanup_capacity(ledger, 1u)) return {};
    const piece_edge built = make_source_node(destination, piece, transform);
    const auto cleanup =
        adopt_piece_edge_cleanup_token_noexcept(ledger, built);
    if (!piece_edge_cleanup_handle_valid_noexcept(cleanup)) return {};
    return {
        .edge = built,
        .cleanup = cleanup,
    };
}

export [[nodiscard]] inline tracked_piece_edge
make_literal_node_error_atomic(
    piece_tree_arena& destination,
    piece_edge_cleanup_ledger& ledger,
    literal_piece piece,
    bioseq::storage::sequence_transform transform =
        bioseq::storage::sequence_transform::identity)
{
    if (!ensure_piece_edge_cleanup_capacity(ledger, 1u)) return {};
    const piece_edge built = make_literal_node(destination, piece, transform);
    const auto cleanup =
        adopt_piece_edge_cleanup_token_noexcept(ledger, built);
    if (!piece_edge_cleanup_handle_valid_noexcept(cleanup)) return {};
    return {
        .edge = built,
        .cleanup = cleanup,
    };
}

export struct logical_children_result {
    piece_edge first;
    piece_edge second;
};

export [[nodiscard]] inline logical_children_result logical_children(
    piece_edge edge)
{
    if (!edge || edge.node->node_kind != piece_node::kind::branch) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq logical_children requires a branch")));
    }
    auto apply = [transform = edge.transform](piece_edge child) {
        child.transform = bioseq::storage::compose(transform, child.transform);
        return child;
    };
    if (bioseq::storage::has_reverse(edge.transform)) {
        return {
            apply(edge.node->payload.branch.right),
            apply(edge.node->payload.branch.left),
        };
    }
    return {
        apply(edge.node->payload.branch.left),
        apply(edge.node->payload.branch.right),
    };
}

namespace {

[[noreturn]] inline void literal_pack_failure(
    bioseq::sequences::rank_pack_status status)
{
    ereport(ERROR,
            (errcode(status
                    == bioseq::sequences::rank_pack_status::capacity_overflow
                ? ERRCODE_PROGRAM_LIMIT_EXCEEDED
                : ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq literal rank packing failed"),
             errdetail("rank_pack_status=%u",
                       static_cast<unsigned>(status))));
    pg_unreachable();
}

template<class Instance>
[[nodiscard]] inline std::uint8_t packed_literal_rank_at(
    literal_piece const& literal,
    std::uint64_t physical_position) noexcept
{
    using traits = typename compact_sequence_range<Instance>::traits;
    using unit = typename traits::storage_unit;
    unit packed{};
    std::memcpy(
        std::addressof(packed),
        literal.bytes
            + (physical_position / traits::letters_per_unit) * sizeof(unit),
        sizeof(unit));
    const auto shift = static_cast<std::size_t>(
        physical_position % traits::letters_per_unit)
        * traits::bits_per_char;
    return static_cast<std::uint8_t>(
        (packed >> shift) & traits::char_mask);
}

[[nodiscard]] inline piece_edge copy_small_distinct_literals(
    piece_tree_arena& destination,
    literal_piece const& left,
    literal_piece const& right,
    bioseq::storage::sequence_transform transform)
{
    const std::uint64_t left_length =
        left.symbol_to - left.symbol_from;
    const std::uint64_t right_length =
        right.symbol_to - right.symbol_from;
    const std::uint64_t combined_length = checked_tree_length_add(
        left_length, right_length);
    const bool reversed = bioseq::storage::has_reverse(transform);
    literal_piece const& physical_first = reversed ? right : left;
    literal_piece const& physical_second = reversed ? left : right;

    if (left.encoding == literal_encoding::unbound_ascii) {
        if (combined_length > small_literal_coalesce_bytes) {
            return {};
        }
        auto* storage = allocate_piece_literal_storage(
            destination, static_cast<std::size_t>(combined_length));
        const auto first_length = static_cast<std::size_t>(
            physical_first.symbol_to - physical_first.symbol_from);
        const auto second_length = static_cast<std::size_t>(
            physical_second.symbol_to - physical_second.symbol_from);
        if (first_length != 0u) {
            std::memcpy(
                storage,
                physical_first.bytes + physical_first.symbol_from,
                first_length);
        }
        if (second_length != 0u) {
            std::memcpy(
                storage + first_length,
                physical_second.bytes + physical_second.symbol_from,
                second_length);
        }
        return make_literal_node(
            destination,
            literal_piece{
                .storage_arena = std::addressof(destination),
                .bytes = storage,
                .symbol_from = 0u,
                .symbol_to = combined_length,
                .stored_type_params = left.stored_type_params,
                .encoding = left.encoding,
                .validation = left.validation,
            },
            transform);
    }

    piece_edge merged{};
    bioseq::pg_types::seq::dispatch_instance(
        left.stored_type_params.alphabet_code,
        left.stored_type_params.storage_unit_byte_width,
        [&destination, &physical_first, &physical_second, &left,
         &merged, combined_length, transform]<class Instance>() {
            using traits = typename compact_sequence_range<Instance>::traits;
            using unit = typename traits::storage_unit;
            const std::uint64_t unit_count = combined_length == 0u ? 0u
                : combined_length / traits::letters_per_unit
                    + static_cast<std::uint64_t>(
                        combined_length % traits::letters_per_unit != 0u);
            if (unit_count
                > small_literal_coalesce_bytes / sizeof(unit)) {
                return;
            }
            const auto payload_bytes = static_cast<std::size_t>(
                unit_count * sizeof(unit));
            auto* storage = allocate_piece_literal_storage(
                destination, payload_bytes);
            using sink = bioseq::sequences::fixed_rank_pack_unit_sink<unit>;
            using writer_type = bioseq::sequences::checked_rank_pack_sink<
                Instance,
                bioseq::sequences::invalid_rank_policy::reject,
                bioseq::sequences::pack_capacity_policy::fixed_span,
                sink>;
            auto units = std::span<unit>{
                reinterpret_cast<unit*>(storage),
                static_cast<std::size_t>(unit_count)};
            writer_type writer{sink{units}};
            for (std::uint64_t position = physical_first.symbol_from;
                 position < physical_first.symbol_to; ++position) {
                const auto status = writer.append_rank(
                    packed_literal_rank_at<Instance>(
                        physical_first, position));
                if (status != bioseq::sequences::rank_pack_status::ok) {
                    literal_pack_failure(status);
                }
            }
            for (std::uint64_t position = physical_second.symbol_from;
                 position < physical_second.symbol_to; ++position) {
                const auto status = writer.append_rank(
                    packed_literal_rank_at<Instance>(
                        physical_second, position));
                if (status != bioseq::sequences::rank_pack_status::ok) {
                    literal_pack_failure(status);
                }
            }
            const auto packed = writer.finish(combined_length, unit_count);
            if (!packed) {
                literal_pack_failure(packed.status);
            }
            merged = make_literal_node(
                destination,
                literal_piece{
                    .storage_arena = std::addressof(destination),
                    .bytes = storage,
                    .symbol_from = 0u,
                    .symbol_to = combined_length,
                    .stored_type_params = left.stored_type_params,
                    .encoding = left.encoding,
                    .validation = left.validation,
                },
                transform);
        });
    return merged;
}

[[nodiscard]] inline piece_edge try_coalesce_contiguous(
    piece_tree_arena& destination,
    piece_edge left,
    piece_edge right)
{
    if (!left || !right || left.transform != right.transform
        || left.node->node_kind != right.node->node_kind) {
        return {};
    }
    const bool reversed = bioseq::storage::has_reverse(left.transform);
    if (left.node->node_kind == piece_node::kind::literal) {
        const auto& a = left.node->payload.literal;
        const auto& b = right.node->payload.literal;
        if (a.encoding != b.encoding || a.validation != b.validation
            || a.stored_type_params != b.stored_type_params) {
            return {};
        }
        if (a.storage_arena == b.storage_arena && a.bytes == b.bytes
            && ((!reversed && a.symbol_to == b.symbol_from)
                || (reversed && b.symbol_to == a.symbol_from))) {
            literal_piece merged = a;
            merged.symbol_from = std::min(a.symbol_from, b.symbol_from);
            merged.symbol_to = std::max(a.symbol_to, b.symbol_to);
            return make_literal_node(destination, merged, left.transform);
        }
        return copy_small_distinct_literals(
            destination, a, b, left.transform);
    }
    if (left.node->node_kind == piece_node::kind::source) {
        const auto& a = left.node->payload.source;
        const auto& b = right.node->payload.source;
        if (!same_source_handle(a.source, b.source)
            || !same_locator(a.locator, b.locator)
            || a.stored_type_params != b.stored_type_params
            || a.last_padding_letters != b.last_padding_letters
            || (!reversed && a.to != b.from)
            || (reversed && b.to != a.from)) {
            return {};
        }
        source_piece merged = a;
        merged.from = std::min(a.from, b.from);
        merged.to = std::max(a.to, b.to);
        return make_source_node(destination, merged, left.transform);
    }
    return {};
}

} // namespace

export [[nodiscard]] inline piece_edge coalesce_boundary(
    piece_tree_arena& destination,
    piece_edge left,
    piece_edge right)
{
    if (piece_edge merged = try_coalesce_contiguous(
            destination, left, right); merged) {
        release_edge(left);
        release_edge(right);
        return merged;
    }
    return make_branch_node(destination, left, right);
}

namespace {

enum class retired_join_root_policy : std::uint8_t {
    fixed_trusted_join,
    proportional_binding,
};

[[nodiscard]] inline bool record_retired_join_root(
    piece_tree_arena& destination,
    piece_edge edge,
    retired_join_root_policy policy);

[[nodiscard]] inline tracked_piece_edge make_branch_tracked(
    piece_tree_arena& destination,
    piece_edge_cleanup_ledger* ledger,
    tracked_piece_edge left,
    tracked_piece_edge right)
{
    if (!left) return right;
    if (!right) return left;
    const piece_edge result = make_branch_node(
        destination, left.edge, right.edge);
    tracked_piece_edge tracked_result =
        transition_consumed_edges_to_result_noexcept(
        ledger, left, right, result);
    if (ledger != nullptr) {
        inject_piece_tree_failure(
            piece_tree_injected_failure_point::join_factory_transition);
    }
    return tracked_result;
}

[[nodiscard]] inline tracked_piece_edge coalesce_boundary_tracked(
    piece_tree_arena& destination,
    piece_edge_cleanup_ledger* ledger,
    tracked_piece_edge left,
    tracked_piece_edge right)
{
    if (piece_edge merged = try_coalesce_contiguous(
            destination, left.edge, right.edge); merged) {
        release_edge(left.edge);
        release_edge(right.edge);
        tracked_piece_edge tracked_result =
            transition_consumed_edges_to_result_noexcept(
            ledger, left, right, merged);
        if (ledger != nullptr) {
            inject_piece_tree_failure(
                piece_tree_injected_failure_point::join_factory_transition);
        }
        return tracked_result;
    }
    return make_branch_tracked(destination, ledger, left, right);
}

[[nodiscard]] inline bool record_retired_join_root(
    piece_tree_arena& destination,
    piece_edge edge,
    retired_join_root_policy policy)
{
    if (!edge || edge.arena != std::addressof(destination)
        || edge.node->node_kind != piece_node::kind::branch) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq join attempted to retire an invalid local root")));
    }
    if (destination.retired_join_root_count == UINT32_MAX) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq join retirement count overflow")));
    }
    if (destination.retired_join_rotation_count == UINT32_MAX) {
        quarantine_piece_tree_invariant_noexcept(
            piece_tree_quarantine_reason::retired_join_root,
            "retired join rotation count overflow",
            std::addressof(destination));
        return false;
    }

    const std::uint32_t index = destination.retired_join_root_count;
    if (index < destination.retired_join_roots.size()) {
        destination.retired_join_roots[index] = edge;
    } else {
        if (policy != retired_join_root_policy::proportional_binding) {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("bioseq trusted join retirement bound was exceeded")));
        }
        const std::uint32_t overflow_index = index
            - static_cast<std::uint32_t>(
                destination.retired_join_roots.size());
        if (overflow_index % max_retired_join_roots == 0u) {
            reserve_arena_allocation(
                destination, sizeof(retired_join_root_block),
                arena_allocation_kind::retired_join_root_metadata);
            auto* block = static_cast<retired_join_root_block*>(
                MemoryContextAllocZero(
                    destination.payload_context,
                    sizeof(retired_join_root_block)));
            if (destination.retired_join_root_overflow_tail == nullptr) {
                if (destination.retired_join_root_overflow != nullptr) {
                    quarantine_piece_tree_invariant_noexcept(
                        piece_tree_quarantine_reason::retired_join_root,
                        "retired join-root overflow head has no tail",
                        std::addressof(destination));
                    return false;
                }
                destination.retired_join_root_overflow = block;
            } else {
                destination.retired_join_root_overflow_tail->next = block;
            }
            destination.retired_join_root_overflow_tail = block;
            inject_piece_tree_failure(
                piece_tree_injected_failure_point::
                    join_retired_root_overflow_allocation);
        }
        auto* block = destination.retired_join_root_overflow_tail;
        if (block == nullptr || block->count >= block->roots.size()) {
            quarantine_piece_tree_invariant_noexcept(
                piece_tree_quarantine_reason::retired_join_root,
                "retired join-root overflow block is unavailable",
                std::addressof(destination));
            return false;
        }
        block->roots[block->count++] = edge;
    }
    ++destination.retired_join_root_count;
    ++destination.retired_join_rotation_count;
    note_piece_tree_retired_root_step_noexcept();
    return true;
}

/**
 * Rebuild two adjacent subtrees after one height-guided join descent.
 *
 * The caller has already joined the only new sequence boundary.  This helper
 * therefore preserves the remaining internal boundaries with branch nodes and
 * performs the one AVL single or double rotation that may be required.  Both
 * input edges are owning tokens and are consumed on every return path.
 */
[[nodiscard]] inline tracked_piece_edge rebalance_joined_edges(
    piece_tree_arena& destination,
    piece_edge_cleanup_ledger* ledger,
    tracked_piece_edge left,
    tracked_piece_edge right,
    retired_join_root_policy retirement_policy)
{
    const std::uint16_t left_height = edge_height(left.edge);
    const std::uint16_t right_height = edge_height(right.edge);
    if (left_height <= right_height + 1u
        && right_height <= left_height + 1u) {
        return make_branch_tracked(destination, ledger, left, right);
    }

    if (left_height == right_height + 2u) {
        const logical_children_result children = logical_children(left.edge);
        const std::uint16_t first_height = edge_height(children.first);
        const std::uint16_t second_height = edge_height(children.second);
        tracked_piece_edge result{};
        if (first_height >= second_height) {
            tracked_piece_edge rotated_right = make_branch_tracked(
                destination, ledger,
                retain_tracked_edge(ledger, children.second), right);
            result = make_branch_tracked(
                destination, ledger,
                retain_tracked_edge(ledger, children.first), rotated_right);
        } else {
            const logical_children_result pivot =
                logical_children(children.second);
            tracked_piece_edge rotated_left = make_branch_tracked(
                destination,
                ledger,
                retain_tracked_edge(ledger, children.first),
                retain_tracked_edge(ledger, pivot.first));
            tracked_piece_edge rotated_right = make_branch_tracked(
                destination, ledger,
                retain_tracked_edge(ledger, pivot.second), right);
            result = make_branch_tracked(
                destination, ledger, rotated_left, rotated_right);
        }
        if (left.edge.arena == std::addressof(destination)) {
            if (!record_retired_join_root(
                    destination, left.edge, retirement_policy)) {
                return {};
            }
            if (ledger != nullptr) {
                inject_piece_tree_failure(
                    piece_tree_injected_failure_point::
                        join_retired_root_record);
            }
        }
        release_tracked_edge(ledger, left);
        return result;
    }

    if (right_height == left_height + 2u) {
        const logical_children_result children = logical_children(right.edge);
        const std::uint16_t first_height = edge_height(children.first);
        const std::uint16_t second_height = edge_height(children.second);
        tracked_piece_edge result{};
        if (second_height >= first_height) {
            tracked_piece_edge rotated_left = make_branch_tracked(
                destination, ledger, left,
                retain_tracked_edge(ledger, children.first));
            result = make_branch_tracked(
                destination, ledger, rotated_left,
                retain_tracked_edge(ledger, children.second));
        } else {
            const logical_children_result pivot =
                logical_children(children.first);
            tracked_piece_edge rotated_left = make_branch_tracked(
                destination, ledger, left,
                retain_tracked_edge(ledger, pivot.first));
            tracked_piece_edge rotated_right = make_branch_tracked(
                destination,
                ledger,
                retain_tracked_edge(ledger, pivot.second),
                retain_tracked_edge(ledger, children.second));
            result = make_branch_tracked(
                destination, ledger, rotated_left, rotated_right);
        }
        if (right.edge.arena == std::addressof(destination)) {
            if (!record_retired_join_root(
                    destination, right.edge, retirement_policy)) {
                return {};
            }
            if (ledger != nullptr) {
                inject_piece_tree_failure(
                    piece_tree_injected_failure_point::
                        join_retired_root_record);
            }
        }
        release_tracked_edge(ledger, right);
        return result;
    }

    release_tracked_edge(ledger, left);
    release_tracked_edge(ledger, right);
    ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq join encountered a non-AVL subtree"),
             errdetail("left_height=%u right_height=%u",
                       static_cast<unsigned>(left_height),
                       static_cast<unsigned>(right_height))));
    pg_unreachable();
}

} // namespace

namespace {

[[nodiscard]] inline tracked_piece_edge join_edges_impl(
    piece_tree_arena& destination,
    tracked_piece_edge left,
    tracked_piece_edge right,
    piece_edge_cleanup_ledger* ledger,
    retired_join_root_policy retirement_policy)
{
    note_piece_tree_join_step_noexcept();
    if (left && left.edge.node->bp_length == 0u) {
        release_tracked_edge(ledger, left);
        return right;
    }
    if (right && right.edge.node->bp_length == 0u) {
        release_tracked_edge(ledger, right);
        return left;
    }
    if (!left) {
        return right;
    }
    if (!right) {
        return left;
    }
    const auto left_height = edge_height(left.edge);
    const auto right_height = edge_height(right.edge);
    if (left_height <= right_height + 1u
        && right_height <= left_height + 1u) {
        return coalesce_boundary_tracked(
            destination, ledger, left, right);
    }
    if (left_height > right_height + 1u) {
        const auto children = logical_children(left.edge);
        tracked_piece_edge joined = join_edges_impl(
            destination,
            retain_tracked_edge(ledger, children.second), right, ledger,
            retirement_policy);
        if (ledger != nullptr) {
            inject_piece_tree_failure(
                piece_tree_injected_failure_point::
                    join_recursive_intermediate);
        }
        tracked_piece_edge result = rebalance_joined_edges(
            destination, ledger,
            retain_tracked_edge(ledger, children.first), joined,
            retirement_policy);
        release_tracked_edge(ledger, left);
        return result;
    }
    const auto children = logical_children(right.edge);
    tracked_piece_edge joined = join_edges_impl(
        destination, left,
        retain_tracked_edge(ledger, children.first), ledger,
        retirement_policy);
    if (ledger != nullptr) {
        inject_piece_tree_failure(
            piece_tree_injected_failure_point::
                join_recursive_intermediate);
    }
    tracked_piece_edge result = rebalance_joined_edges(
        destination, ledger, joined,
        retain_tracked_edge(ledger, children.second), retirement_policy);
    release_tracked_edge(ledger, right);
    return result;
}

} // namespace

export [[nodiscard]] inline piece_edge join_edges(
    piece_tree_arena& destination,
    piece_edge left,
    piece_edge right)
{
    return join_edges_impl(
        destination, {.edge = left}, {.edge = right}, nullptr,
        retired_join_root_policy::fixed_trusted_join).edge;
}

export [[nodiscard]] inline tracked_piece_edge
join_tracked_edges_error_atomic(
    piece_tree_arena& destination,
    piece_edge_cleanup_ledger& ledger,
    tracked_piece_edge left,
    tracked_piece_edge right)
{
    return join_edges_impl(
        destination, left, right, &ledger,
        retired_join_root_policy::proportional_binding);
}

export [[nodiscard]] inline tracked_piece_edge join_edges_error_atomic(
    piece_tree_arena& destination,
    piece_edge_cleanup_ledger& ledger,
    piece_edge borrowed_left,
    piece_edge borrowed_right)
{
    tracked_piece_edge left = retain_tracked_edge(&ledger, borrowed_left);
    inject_piece_tree_failure(
        piece_tree_injected_failure_point::join_left_operand_retain);
    tracked_piece_edge right = retain_tracked_edge(&ledger, borrowed_right);
    inject_piece_tree_failure(
        piece_tree_injected_failure_point::join_right_operand_retain);
    return join_edges_impl(
        destination, left, right, &ledger,
        retired_join_root_policy::fixed_trusted_join);
}

export [[nodiscard]] inline tracked_piece_edge retain_piece_edge_error_atomic(
    piece_edge_cleanup_ledger& ledger, piece_edge borrowed)
{
    return retain_tracked_edge(&ledger, borrowed);
}

export [[nodiscard]] inline tracked_piece_edge
transform_piece_edge_error_atomic(
    piece_edge_cleanup_ledger& ledger,
    piece_edge borrowed,
    bioseq::storage::sequence_transform transform)
{
    tracked_piece_edge tracked = retain_tracked_edge(&ledger, borrowed);
    if (!tracked) {
        return {};
    }
    const tracked_piece_edge before_transform = tracked;
    tracked.edge.transform = bioseq::storage::compose(
        transform, tracked.edge.transform);
    transition_piece_edge_cleanup_identity_noexcept(
        &ledger, before_transform, tracked);
    return tracked;
}

export [[nodiscard]] inline split_result split_leaf(
    piece_tree_arena& destination,
    piece_edge_cleanup_ledger& ledger,
    piece_edge edge,
    std::uint64_t offset)
{
    if (!edge || edge.node->node_kind == piece_node::kind::branch
        || offset == 0u || offset >= edge.node->bp_length) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("invalid bioseq leaf split")));
    }
    const bool reversed = bioseq::storage::has_reverse(edge.transform);
    if (edge.node->node_kind == piece_node::kind::source) {
        const source_piece& original = edge.node->payload.source;
        source_piece prefix = original;
        source_piece suffix = original;
        if (!reversed) {
            prefix.to = original.from + offset;
            suffix.from = prefix.to;
        } else {
            prefix.from = original.to - offset;
            suffix.to = prefix.from;
        }
        tracked_piece_edge left = make_source_node_error_atomic(
            destination, ledger, prefix, edge.transform);
        inject_piece_tree_failure(
            piece_tree_injected_failure_point::split_first_leaf_intermediate);
        tracked_piece_edge right = make_source_node_error_atomic(
            destination, ledger, suffix, edge.transform);
        return {left, right};
    }
    const literal_piece& original = edge.node->payload.literal;
    literal_piece prefix = original;
    literal_piece suffix = original;
    if (!reversed) {
        prefix.symbol_to = original.symbol_from + offset;
        suffix.symbol_from = prefix.symbol_to;
    } else {
        prefix.symbol_from = original.symbol_to - offset;
        suffix.symbol_to = prefix.symbol_from;
    }
    tracked_piece_edge left = make_literal_node_error_atomic(
        destination, ledger, prefix, edge.transform);
    inject_piece_tree_failure(
        piece_tree_injected_failure_point::split_first_leaf_intermediate);
    tracked_piece_edge right = make_literal_node_error_atomic(
        destination, ledger, suffix, edge.transform);
    return {left, right};
}

export [[nodiscard]] inline split_result split_tree(
    piece_tree_arena& destination,
    piece_edge_cleanup_ledger& ledger,
    piece_edge tree,
    std::uint64_t offset)
{
    if (!tree) {
        return {};
    }
    if (offset == 0u) {
        return {{}, retain_piece_edge_error_atomic(ledger, tree)};
    }
    if (offset >= tree.node->bp_length) {
        return {retain_piece_edge_error_atomic(ledger, tree), {}};
    }
    if (tree.node->node_kind != piece_node::kind::branch) {
        return split_leaf(destination, ledger, tree, offset);
    }
    const auto children = logical_children(tree);
    const auto first_length = children.first.node->bp_length;
    if (offset < first_length) {
        split_result parts = split_tree(
            destination, ledger, children.first, offset);
        tracked_piece_edge second = retain_piece_edge_error_atomic(
            ledger, children.second);
        return {
            parts.left,
            join_tracked_edges_error_atomic(
                destination, ledger, parts.right, second),
        };
    }
    split_result parts = split_tree(
        destination, ledger, children.second, offset - first_length);
    tracked_piece_edge first = retain_piece_edge_error_atomic(
        ledger, children.first);
    return {
        join_tracked_edges_error_atomic(
            destination, ledger, first, parts.left),
        parts.right,
    };
}

export [[nodiscard]] inline tracked_piece_edge slice_tree(
    piece_tree_arena& destination,
    piece_edge_cleanup_ledger& ledger,
    piece_edge tree,
    std::uint64_t from,
    std::uint64_t to)
{
    if (!tree || from > to || to > tree.node->bp_length) {
        if (!tree && from == 0u && to == 0u) {
            return {};
        }
        ereport(ERROR,
                (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                 errmsg("bioseq slice is outside its logical sequence")));
    }
    split_result at_end = split_tree(destination, ledger, tree, to);
    release_tracked_edge(&ledger, at_end.right);
    split_result at_start = split_tree(
        destination, ledger, at_end.left.edge, from);
    release_tracked_edge(&ledger, at_end.left);
    release_tracked_edge(&ledger, at_start.left);
    return at_start.right;
}

namespace {

[[nodiscard]] inline tracked_piece_edge bind_literals_copy_impl(
    piece_tree_arena& destination,
    piece_edge edge,
    compact_sequence_stdc_type_params result_type,
    piece_edge_cleanup_ledger* ledger)
{
    if (!edge) {
        return {};
    }
    if (edge.node->node_kind == piece_node::kind::source) {
        if (edge.node->payload.source.stored_type_params.alphabet_code
            != result_type.alphabet_code) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("cannot bind bioseq source across alphabets")));
        }
        return retain_tracked_edge(ledger, edge);
    }
    if (edge.node->node_kind == piece_node::kind::branch) {
        const branch_piece& branch = edge.node->payload.branch;
        tracked_piece_edge left = bind_literals_copy_impl(
            destination, branch.left, result_type, ledger);
        tracked_piece_edge right = bind_literals_copy_impl(
            destination, branch.right, result_type, ledger);
        tracked_piece_edge joined = join_edges_impl(
            destination, left, right, ledger,
            retired_join_root_policy::proportional_binding);
        const tracked_piece_edge before_transform = joined;
        joined.edge.transform = bioseq::storage::compose(
            edge.transform, joined.edge.transform);
        transition_piece_edge_cleanup_identity_noexcept(
            ledger, before_transform, joined);
        if (ledger != nullptr) {
            inject_piece_tree_failure(
                piece_tree_injected_failure_point::
                    join_edge_identity_transition);
        }
        return joined;
    }
    const literal_piece& literal = edge.node->payload.literal;
    if (literal.encoding == literal_encoding::packed) {
        if (literal.stored_type_params.alphabet_code
            != result_type.alphabet_code) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("cannot concatenate bioseq values with different alphabets")));
        }
        return retain_tracked_edge(ledger, edge);
    }

    const auto length = literal.symbol_to - literal.symbol_from;
    const auto* ascii = reinterpret_cast<char const*>(literal.bytes)
        + literal.symbol_from;
    if (length > std::numeric_limits<bioseq::pg_types::seq::bp_size_t>::max()) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq literal is too long to bind")));
    }
    if (ledger != nullptr) {
        if (!ensure_piece_edge_cleanup_capacity(*ledger, 1u)) return {};
    }
    tracked_piece_edge bound_edge{};
    bioseq::pg_types::seq::dispatch_instance(
        result_type.alphabet_code,
        result_type.storage_unit_byte_width,
        [&destination, &bound_edge, result_type,
         length, ascii, edge, ledger]<class Instance>() {
            using traits = typename compact_sequence_range<Instance>::traits;
            using unit = typename traits::storage_unit;
            const std::uint64_t unit_count = length == 0u ? 0u
                : length / traits::letters_per_unit
                    + static_cast<std::uint64_t>(
                        length % traits::letters_per_unit != 0u);
            if (unit_count
                > static_cast<std::uint64_t>(
                    MaxAllocSize / sizeof(unit))) {
                ereport(ERROR,
                        (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                         errmsg("bound bioseq literal exceeds MaxAllocSize")));
            }
            const auto payload_bytes = static_cast<std::size_t>(
                unit_count * sizeof(unit));
            auto* storage = allocate_piece_literal_storage(
                destination, payload_bytes);
            using sink = bioseq::sequences::
                fixed_rank_pack_unit_sink<unit>;
            using writer_type = bioseq::sequences::checked_rank_pack_sink<
                Instance,
                bioseq::sequences::invalid_rank_policy::reject,
                bioseq::sequences::pack_capacity_policy::fixed_span,
                sink>;
            auto units = std::span<unit>{
                reinterpret_cast<unit*>(storage),
                static_cast<std::size_t>(unit_count)};
            writer_type writer{sink{units}};
            for (std::uint64_t index = 0u; index < length; ++index) {
                std::uint8_t rank = 0u;
                if (!pgseq_rank_for_symbol(
                        result_type.alphabet_code,
                        static_cast<unsigned char>(ascii[index]), rank)) {
                    const auto alphabet = pgseq_supported_alphabets::
                        name_of_alphabet_code(result_type.alphabet_code);
                    ereport(ERROR,
                            (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                             errmsg("invalid symbol '%c' for bioseq "
                                    "alphabet %.*s",
                                    ascii[index],
                                    static_cast<int>(alphabet.size()),
                                    alphabet.data()),
                             errdetail("symbol offset=%llu",
                                       static_cast<unsigned long long>(
                                           index))));
                }
                const auto status = writer.append_rank(rank);
                if (status != bioseq::sequences::rank_pack_status::ok) {
                    literal_pack_failure(status);
                }
            }
            const auto packed = writer.finish(length, unit_count);
            if (!packed) {
                literal_pack_failure(packed.status);
            }
            const piece_edge built = make_literal_node(
                destination,
                literal_piece{
                    .storage_arena = std::addressof(destination),
                    .bytes = storage,
                    .symbol_from = 0u,
                    .symbol_to = length,
                    .stored_type_params = result_type,
                    .encoding = literal_encoding::packed,
                    .validation = literal_validation::alphabet_validated,
                },
                edge.transform);
            if (ledger != nullptr) {
                const auto cleanup =
                    adopt_piece_edge_cleanup_token_noexcept(*ledger, built);
                if (!piece_edge_cleanup_handle_valid_noexcept(cleanup)) {
                    return;
                }
                bound_edge = {
                    .edge = built,
                    .cleanup = cleanup,
                };
                inject_piece_tree_failure(
                    piece_tree_injected_failure_point::
                        join_factory_transition);
            } else {
                bound_edge = {.edge = built};
            }
        });
    if (!bound_edge) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq literal alphabet dispatch produced no result")));
    }
    return bound_edge;
}

} // namespace

export [[nodiscard]] inline piece_edge bind_literals_copy(
    piece_tree_arena& destination,
    piece_edge edge,
    compact_sequence_stdc_type_params result_type)
{
    return bind_literals_copy_impl(
        destination, edge, result_type, nullptr).edge;
}

export [[nodiscard]] inline tracked_piece_edge
bind_literals_copy_error_atomic(
    piece_tree_arena& destination,
    piece_edge edge,
    compact_sequence_stdc_type_params result_type,
    piece_edge_cleanup_ledger& ledger)
{
    return bind_literals_copy_impl(
        destination, edge, result_type, &ledger);
}

namespace {

struct validated_tree_fields {
    std::uint64_t length;
    std::uint32_t pieces;
    std::uint16_t height;
};

inline void validate_arena_accounting(piece_tree_arena const& arena)
{
    const Size node_unit = MAXALIGN(sizeof(piece_node));
    const Size arena_dependency_unit = MAXALIGN(sizeof(arena_dependency));
    const Size source_dependency_unit = MAXALIGN(sizeof(source_dependency));
    const std::uint64_t expected_nodes =
        static_cast<std::uint64_t>(arena.node_count) * node_unit;
    const std::uint64_t expected_arena_dependencies =
        static_cast<std::uint64_t>(arena.arena_dependency_count)
        * arena_dependency_unit;
    const std::uint64_t expected_source_dependencies =
        static_cast<std::uint64_t>(arena.source_dependency_count)
        * source_dependency_unit;
    const std::uint64_t expected_retirement_metadata =
        static_cast<std::uint64_t>(
            arena.retired_join_root_overflow_block_count)
        * MAXALIGN(sizeof(retired_join_root_block));
    const std::uint64_t component_total =
        static_cast<std::uint64_t>(arena.node_metadata_bytes)
        + arena.arena_dependency_metadata_bytes
        + arena.source_dependency_metadata_bytes
        + arena.literal_storage_bytes
        + arena.retired_join_root_metadata_bytes;
    if (arena.payload_context == nullptr || arena.generation == 0u
        || arena.refcount == 0u || arena.refcount == UINT32_MAX
        || arena.accounted_bytes > MaxAllocSize
        || arena.node_metadata_bytes != expected_nodes
        || arena.arena_dependency_metadata_bytes
            != expected_arena_dependencies
        || arena.source_dependency_metadata_bytes
            != expected_source_dependencies
        || arena.retired_join_root_metadata_bytes
            != expected_retirement_metadata
        || component_total != arena.accounted_bytes) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq piece arena accounting is corrupt"),
                 errdetail("generation=%llu refcount=%u accounted_bytes=%zu "
                           "node_count=%u arena_dependencies=%u "
                           "source_dependencies=%u retired_blocks=%u",
                           static_cast<unsigned long long>(arena.generation),
                           arena.refcount,
                           static_cast<std::size_t>(arena.accounted_bytes),
                           arena.node_count,
                           arena.arena_dependency_count,
                           arena.source_dependency_count,
                           arena.retired_join_root_overflow_block_count)));
    }
}

inline void validate_retired_join_root_storage(
    piece_tree_arena const& arena)
{
    const std::uint32_t inline_capacity = static_cast<std::uint32_t>(
        arena.retired_join_roots.size());
    const std::uint32_t inline_count = std::min(
        arena.retired_join_root_count, inline_capacity);
    std::uint32_t rotations = 0u;
    const auto validate_record = [&arena, &rotations](
            piece_edge record) {
        if (!record || record.arena != std::addressof(arena)
            || record.node->node_kind != piece_node::kind::branch) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq retired join-root record is invalid")));
        }
        if (rotations == UINT32_MAX) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq retired rotation count overflow")));
        }
        ++rotations;
    };
    for (std::uint32_t index = 0u; index < inline_count; ++index) {
        validate_record(arena.retired_join_roots[index]);
    }
    for (std::uint32_t index = inline_count;
         index < inline_capacity; ++index) {
        if (arena.retired_join_roots[index]) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq unused inline retired-root slot is nonempty")));
        }
    }

    const std::uint32_t overflow_count = arena.retired_join_root_count
        - inline_count;
    const std::uint32_t expected_blocks = overflow_count == 0u ? 0u
        : (overflow_count - 1u)
                / static_cast<std::uint32_t>(max_retired_join_roots)
            + 1u;
    if (arena.retired_join_root_overflow_block_count != expected_blocks
        || (expected_blocks == 0u
            && (arena.retired_join_root_overflow != nullptr
                || arena.retired_join_root_overflow_tail != nullptr))
        || (expected_blocks != 0u
            && (arena.retired_join_root_overflow == nullptr
                || arena.retired_join_root_overflow_tail == nullptr))) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq retired join-root overflow summary is corrupt")));
    }

    std::uint32_t remaining = overflow_count;
    std::uint32_t blocks = 0u;
    auto const* block = arena.retired_join_root_overflow;
    while (block != nullptr) {
        if (++blocks > expected_blocks) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq retired join-root overflow list is cyclic")));
        }
        const std::uint16_t expected_count = static_cast<std::uint16_t>(
            std::min<std::uint32_t>(
                remaining,
                static_cast<std::uint32_t>(block->roots.size())));
        if (block->count != expected_count
            || (block == arena.retired_join_root_overflow_tail)
                != (block->next == nullptr)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq retired join-root overflow block is corrupt")));
        }
        for (std::uint16_t index = 0u; index < block->count; ++index) {
            validate_record(block->roots[index]);
        }
        for (std::size_t index = block->count;
             index < block->roots.size(); ++index) {
            if (block->roots[index]) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq unused retired-root overflow slot is nonempty")));
            }
        }
        remaining -= block->count;
        block = block->next;
    }
    if (blocks != expected_blocks || remaining != 0u
        || rotations != arena.retired_join_rotation_count
        || rotations != arena.retired_join_root_count) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq retired join-root accounting is corrupt")));
    }
}

[[nodiscard]] inline validated_tree_fields validate_tree_node(
    piece_edge edge,
    compact_sequence_stdc_type_params result_type,
    bool alphabet_bound)
{
    if (!edge) {
        return {0u, 0u, 0u};
    }
    if (edge.arena == nullptr
        || std::to_underlying(edge.transform)
            > std::to_underlying(
                bioseq::storage::sequence_transform::reverse_complement)) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq piece edge metadata is corrupt")));
    }
    validate_arena_accounting(*edge.arena);
    const piece_node& node = *edge.node;
    if (node.node_kind == piece_node::kind::branch) {
        const auto& branch = node.payload.branch;
        const auto generation_valid = [parent = edge.arena](piece_edge child) {
            return child && child.arena != nullptr
                && (child.arena == parent
                    || child.arena->generation < parent->generation);
        };
        if (!generation_valid(branch.left)
            || !generation_valid(branch.right)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq branch dependency generation is corrupt")));
        }
        const auto left = validate_tree_node(
            branch.left, result_type, alphabet_bound);
        const auto right = validate_tree_node(
            branch.right, result_type, alphabet_bound);
        constexpr auto max_logical_length =
            static_cast<std::uint64_t>(INT64_MAX);
        if (left.length > max_logical_length
            || right.length > max_logical_length
            || right.length > max_logical_length - left.length
            || left.length + right.length != node.bp_length
            || static_cast<std::uint64_t>(left.pieces) + right.pieces
                != node.piece_count
            || 1u + std::max(left.height, right.height) != node.height
            || (left.height > right.height + 1u)
            || (right.height > left.height + 1u)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq piece-tree invariant violation")));
        }
        return {node.bp_length, node.piece_count, node.height};
    }
    if (node.piece_count != 1u || node.height != 1u) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq leaf metadata is corrupt")));
    }
    if (node.node_kind == piece_node::kind::source) {
        const auto& source = node.payload.source;
        if (!pg_sequence_source_handle_valid(source.source)
            || source.from > source.to
            || source.to > source.locator.total_bp_size
            || source.to - source.from != node.bp_length
            || !alphabet_bound
            || node.payload.source.stored_type_params.alphabet_code
                != result_type.alphabet_code) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("bioseq source leaf alphabet mismatch")));
        }
    } else if (node.node_kind == piece_node::kind::literal) {
        const auto& literal = node.payload.literal;
        if (literal.storage_arena == nullptr || literal.bytes == nullptr
            || literal.symbol_from > literal.symbol_to
            || literal.symbol_to - literal.symbol_from != node.bp_length
            || (literal.storage_arena != edge.arena
                && literal.storage_arena->generation
                    >= edge.arena->generation)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq literal leaf metadata is corrupt")));
        }
        if (alphabet_bound
            && literal.encoding == literal_encoding::packed
            && literal.stored_type_params.alphabet_code
                != result_type.alphabet_code) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("bioseq literal leaf alphabet mismatch")));
        }
    } else {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq piece-tree node kind is corrupt")));
    }
    return {node.bp_length, 1u, 1u};
}

} // namespace

export inline void validate_tree_and_limits(
    piece_edge edge, compact_sequence_stdc_type_params result_type,
    std::uint64_t expected_length, bool alphabet_bound = true) {
  if (expected_length > static_cast<std::uint64_t>(INT64_MAX)) {
    ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                    errmsg("bioseq logical length exceeds bigint")));
  }
  const auto fields = validate_tree_node(edge, result_type, alphabet_bound);
  if (edge) {
    validate_retired_join_root_storage(*edge.arena);
  }
  if (fields.length != expected_length || fields.pieces > max_piece_count) {
    ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                    errmsg("bioseq piece-tree cached length/count mismatch")));
  }
}

export struct bounded_join_validation_audit {
    std::uint32_t new_nodes_visited;
    std::uint32_t external_edges_checked;
    std::uint32_t arena_dependencies_checked;
    std::uint32_t source_dependencies_checked;
};

namespace {

inline constexpr std::size_t bounded_join_node_capacity = 193u;
inline constexpr std::size_t bounded_join_arena_dependency_capacity =
    bounded_join_node_capacity * 2u;
inline constexpr std::size_t bounded_join_source_dependency_capacity =
    bounded_join_node_capacity;
inline constexpr std::size_t bounded_join_node_set_capacity = 256u;
inline constexpr std::size_t bounded_join_arena_multiset_capacity = 512u;
inline constexpr std::size_t bounded_join_source_multiset_capacity = 256u;

static_assert((bounded_join_arena_multiset_capacity
               & (bounded_join_arena_multiset_capacity - 1u)) == 0u);
static_assert((bounded_join_node_set_capacity
               & (bounded_join_node_set_capacity - 1u)) == 0u);
static_assert((bounded_join_source_multiset_capacity
               & (bounded_join_source_multiset_capacity - 1u)) == 0u);

[[nodiscard]] inline std::size_t bounded_join_pointer_hash(
    void const* pointer) noexcept
{
    std::uintptr_t value = reinterpret_cast<std::uintptr_t>(pointer);
    value ^= value >> 17u;
    value *= static_cast<std::uintptr_t>(0xed5ad4bbu);
    value ^= value >> 11u;
    value *= static_cast<std::uintptr_t>(0xac4c1b51u);
    value ^= value >> 15u;
    return static_cast<std::size_t>(value);
}

struct bounded_join_arena_multiset_slot {
    piece_tree_arena* key{};
    std::uint32_t required{};
    std::uint32_t matched{};
};

struct bounded_join_source_multiset_slot {
    void* state{};
    pg_sequence_source_backend_ops const* ops{};
    std::uint32_t required{};
    std::uint32_t matched{};
};

[[nodiscard]] inline bool bounded_edge_height_possible(
    std::uint32_t pieces, std::uint16_t height) noexcept
{
    if (pieces == 0u || height == 0u || height > 64u) return false;
    if (height == 1u) return pieces >= 1u;
    std::uint64_t lower = 1u;
    std::uint64_t upper = 2u;
    for (std::uint16_t current = 2u; current < height; ++current) {
        const std::uint64_t next = lower + upper;
        lower = upper;
        upper = next;
    }
    return static_cast<std::uint64_t>(pieces) >= upper;
}

inline void validate_bounded_edge_summary(
    piece_edge edge, bounded_join_validation_audit& audit,
    bool count_external)
{
    note_piece_tree_bounded_validation_step_noexcept();
    if (!edge || edge.arena == nullptr || edge.node == nullptr
        || edge.arena->generation == 0u || edge.arena->refcount == 0u
        || edge.arena->refcount == UINT32_MAX
        || std::to_underlying(edge.transform)
            > std::to_underlying(
                bioseq::storage::sequence_transform::reverse_complement)
        || edge.node->bp_length > static_cast<std::uint64_t>(INT64_MAX)
        || edge.node->piece_count == 0u
        || edge.node->piece_count > max_piece_count
        || !bounded_edge_height_possible(
            edge.node->piece_count, edge.node->height)
        || std::to_underlying(edge.node->node_kind)
            > std::to_underlying(piece_node::kind::literal)) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq trusted join edge summary is corrupt")));
    }
    if ((edge.node->node_kind == piece_node::kind::branch
            && (edge.node->piece_count < 2u || edge.node->height < 2u))
        || (edge.node->node_kind != piece_node::kind::branch
            && (edge.node->piece_count != 1u
                || edge.node->height != 1u))) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq trusted join node shape is corrupt")));
    }
    if (count_external && audit.external_edges_checked != UINT32_MAX) {
        ++audit.external_edges_checked;
    }
}

inline void validate_bounded_join_leaf(
    piece_edge edge, compact_sequence_stdc_type_params result_type,
    bool alphabet_bound)
{
    const piece_node& node = *edge.node;
    if (node.node_kind == piece_node::kind::source) {
        const auto& source = node.payload.source;
        if (!alphabet_bound || !pg_sequence_source_handle_valid(source.source)
            || source.from > source.to
            || source.to > source.locator.total_bp_size
            || source.to - source.from != node.bp_length
            || source.stored_type_params.alphabet_code
                != result_type.alphabet_code) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("bioseq trusted join source leaf is invalid")));
        }
        return;
    }
    if (node.node_kind != piece_node::kind::literal) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq trusted join leaf kind is invalid")));
    }
    const auto& literal = node.payload.literal;
    if (literal.storage_arena == nullptr || literal.bytes == nullptr
        || literal.storage_arena->generation == 0u
        || literal.storage_arena->refcount == 0u
        || literal.storage_arena->refcount == UINT32_MAX
        || literal.symbol_from > literal.symbol_to
        || literal.symbol_to - literal.symbol_from != node.bp_length
        || (literal.storage_arena != edge.arena
            && literal.storage_arena->generation >= edge.arena->generation)
        || (alphabet_bound
            && (literal.encoding != literal_encoding::packed
                || literal.validation
                    != literal_validation::alphabet_validated
                || literal.stored_type_params.alphabet_code
                    != result_type.alphabet_code))
        || (!alphabet_bound
            && literal.encoding != literal_encoding::unbound_ascii)) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("bioseq trusted join literal leaf is invalid")));
    }
}

} // namespace

/* Fast-path validation for concatenating two already-validated immutable
 * roots.  Existing subtrees are checked through their cached summaries; every
 * node and dependency allocated by the new arena is visited exactly once.
 * The work is therefore bounded by the height-guided join path rather than by
 * the total number of logical pieces. */
export [[nodiscard]] inline bounded_join_validation_audit
validate_trusted_join_and_limits(
    piece_tree_arena const& destination, piece_edge result,
    piece_edge trusted_left, std::uint64_t left_length,
    piece_edge trusted_right, std::uint64_t right_length,
    compact_sequence_stdc_type_params result_type,
    std::uint64_t expected_length, bool alphabet_bound)
{
    bounded_join_validation_audit audit{};
    if (left_length > static_cast<std::uint64_t>(INT64_MAX)
        || right_length > static_cast<std::uint64_t>(INT64_MAX)
        || right_length
            > static_cast<std::uint64_t>(INT64_MAX) - left_length
        || left_length + right_length != expected_length
        || expected_length > static_cast<std::uint64_t>(INT64_MAX)
        || destination.dependencies_released
        || destination.release_next != nullptr
        || destination.retired_join_root_count
            > destination.retired_join_roots.size()
        || destination.retired_join_root_overflow != nullptr
        || destination.retired_join_root_overflow_tail != nullptr
        || destination.retired_join_root_overflow_block_count != 0u
        || destination.retired_join_root_metadata_bytes != 0u
        || destination.retired_join_rotation_count
            != destination.retired_join_root_count) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq trusted join length or arena state is invalid")));
    }
    if (alphabet_bound) {
        if (result_type.alphabet_code == pgseq_unbound_alphabet_code
            || result_type.storage_unit_byte_width == pgseq_unbound_width_tag
            || !pgseq_supported_storage_unit_byte_widths::contains_runtime(
                result_type.storage_unit_byte_width)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("bioseq trusted join result type is invalid")));
        }
    } else if (result_type.alphabet_code != pgseq_unbound_alphabet_code
               || result_type.storage_unit_byte_width
                   != pgseq_unbound_width_tag) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("bioseq unbound trusted join acquired a physical type")));
    }

    const auto validate_trusted_root = [&audit](
            piece_edge edge, std::uint64_t expected) {
        if (!edge) {
            if (expected != 0u) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq trusted join root is missing")));
            }
            return;
        }
        validate_bounded_edge_summary(edge, audit, true);
        if (edge.node->bp_length != expected) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq trusted join root length changed")));
        }
    };
    validate_trusted_root(trusted_left, left_length);
    validate_trusted_root(trusted_right, right_length);
    validate_trusted_root(result, expected_length);
    validate_arena_accounting(destination);
    validate_retired_join_root_storage(destination);

    const std::uint16_t result_height = result ? result.node->height : 0u;
    const std::uint32_t node_budget =
        3u * static_cast<std::uint32_t>(result_height) + 1u;
    if (destination.node_count > node_budget
        || node_budget > bounded_join_node_capacity
        || destination.arena_dependency_count
            > bounded_join_arena_dependency_capacity
        || destination.source_dependency_count
            > bounded_join_source_dependency_capacity) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq trusted join exceeded its path allocation bound"),
                 errdetail("new_nodes=%u bound=%u result_height=%u "
                           "arena_dependencies=%u source_dependencies=%u",
                           destination.node_count, node_budget,
                           static_cast<unsigned>(result_height),
                           destination.arena_dependency_count,
                           destination.source_dependency_count)));
    }

    std::array<piece_edge, bounded_join_node_capacity> pending{};
    std::array<piece_node const*, bounded_join_node_set_capacity> visited{};
    std::array<piece_node const*, bounded_join_node_set_capacity>
        retired_visited{};
    std::array<bounded_join_arena_multiset_slot,
               bounded_join_arena_multiset_capacity> required_arenas{};
    std::array<bounded_join_source_multiset_slot,
               bounded_join_source_multiset_capacity> required_sources{};
    std::size_t pending_count = 0u;
    std::size_t visited_count = 0u;
    std::size_t retired_visited_count = 0u;
    std::uint32_t required_arena_count = 0u;
    std::uint32_t required_source_count = 0u;

    const auto require_arena_dependency = [
        &required_arenas, &required_arena_count](piece_tree_arena* child) {
        if (child == nullptr
            || required_arena_count
                >= bounded_join_arena_dependency_capacity) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq trusted join arena dependency bound was exceeded")));
        }
        std::size_t slot_index = bounded_join_pointer_hash(child)
            & (required_arenas.size() - 1u);
        for (std::size_t probe = 0u; probe < required_arenas.size(); ++probe) {
            auto& slot = required_arenas[slot_index];
            if (slot.key == nullptr) slot.key = child;
            if (slot.key == child) {
                if (slot.required == UINT32_MAX) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("bioseq trusted join arena dependency multiplicity overflow")));
                }
                ++slot.required;
                ++required_arena_count;
                note_piece_tree_bounded_validation_step_noexcept();
                return;
            }
            slot_index = (slot_index + 1u) & (required_arenas.size() - 1u);
        }
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq trusted join arena dependency table overflow")));
    };
    const auto require_source_dependency = [
        &required_sources, &required_source_count](
            pg_sequence_source_handle source) {
        if (!pg_sequence_source_handle_valid(source)
            || required_source_count
                >= bounded_join_source_dependency_capacity) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq trusted join source dependency bound was exceeded")));
        }
        std::size_t slot_index = (
            bounded_join_pointer_hash(source.state)
            ^ (bounded_join_pointer_hash(source.ops) << 1u))
            & (required_sources.size() - 1u);
        for (std::size_t probe = 0u; probe < required_sources.size(); ++probe) {
            auto& slot = required_sources[slot_index];
            if (slot.state == nullptr) {
                slot.state = source.state;
                slot.ops = source.ops;
            }
            if (slot.state == source.state && slot.ops == source.ops) {
                if (slot.required == UINT32_MAX) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("bioseq trusted join source dependency multiplicity overflow")));
                }
                ++slot.required;
                ++required_source_count;
                note_piece_tree_bounded_validation_step_noexcept();
                return;
            }
            slot_index = (slot_index + 1u) & (required_sources.size() - 1u);
        }
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq trusted join source dependency table overflow")));
    };
    if (result && result.arena == std::addressof(destination)) {
        pending[pending_count++] = result;
    }
    while (pending_count != 0u) {
        const piece_edge current = pending[--pending_count];
        if (current.arena != std::addressof(destination)) {
            validate_bounded_edge_summary(current, audit, true);
            continue;
        }
        if (visited_count >= bounded_join_node_capacity) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq trusted join contains a cycle or shared new node")));
        }
        std::size_t visited_slot = bounded_join_pointer_hash(current.node)
            & (visited.size() - 1u);
        bool inserted = false;
        for (std::size_t probe = 0u; probe < visited.size(); ++probe) {
            if (visited[visited_slot] == current.node) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq trusted join contains a cycle or shared new node")));
            }
            if (visited[visited_slot] == nullptr) {
                visited[visited_slot] = current.node;
                inserted = true;
                break;
            }
            visited_slot = (visited_slot + 1u) & (visited.size() - 1u);
        }
        if (!inserted) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq trusted join visited-node table overflow")));
        }
        ++visited_count;
        audit.new_nodes_visited = static_cast<std::uint32_t>(visited_count);
        validate_bounded_edge_summary(current, audit, false);
        const piece_node& node = *current.node;
        if (node.node_kind != piece_node::kind::branch) {
            validate_bounded_join_leaf(
                current, result_type, alphabet_bound);
            if (node.node_kind == piece_node::kind::source) {
                require_source_dependency(node.payload.source.source);
            } else if (node.payload.literal.storage_arena
                       != std::addressof(destination)) {
                require_arena_dependency(
                    node.payload.literal.storage_arena);
            }
            continue;
        }
        const auto& branch = node.payload.branch;
        validate_bounded_edge_summary(branch.left, audit,
            branch.left.arena != std::addressof(destination));
        validate_bounded_edge_summary(branch.right, audit,
            branch.right.arena != std::addressof(destination));
        if ((branch.left.arena != std::addressof(destination)
                && branch.left.arena->generation >= destination.generation)
            || (branch.right.arena != std::addressof(destination)
                && branch.right.arena->generation >= destination.generation)
            || branch.right.node->bp_length
                > static_cast<std::uint64_t>(INT64_MAX)
                    - branch.left.node->bp_length
            || branch.left.node->bp_length + branch.right.node->bp_length
                != node.bp_length
            || static_cast<std::uint64_t>(branch.left.node->piece_count)
                    + branch.right.node->piece_count
                != node.piece_count
            || 1u + std::max(branch.left.node->height,
                             branch.right.node->height)
                != node.height
            || branch.left.node->height > branch.right.node->height + 1u
            || branch.right.node->height > branch.left.node->height + 1u) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq trusted join path invariant is invalid")));
        }
        const std::array children{branch.left, branch.right};
        for (piece_edge child : children) {
            if (child.arena != std::addressof(destination)) {
                require_arena_dependency(child.arena);
                continue;
            }
            if (pending_count >= pending.size()) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq trusted join validation stack overflow")));
            }
            pending[pending_count++] = child;
        }
    }

    const auto node_is_in_table = [](auto const& table,
                                     piece_node const* node) noexcept {
        std::size_t slot = bounded_join_pointer_hash(node)
            & (table.size() - 1u);
        for (std::size_t probe = 0u; probe < table.size(); ++probe) {
            if (table[slot] == node) return true;
            if (table[slot] == nullptr) return false;
            slot = (slot + 1u) & (table.size() - 1u);
        }
        return false;
    };
    const auto insert_retired_node = [&retired_visited](
            piece_node const* node) {
        std::size_t slot = bounded_join_pointer_hash(node)
            & (retired_visited.size() - 1u);
        for (std::size_t probe = 0u; probe < retired_visited.size(); ++probe) {
            if (retired_visited[slot] == node) {
                return false;
            }
            if (retired_visited[slot] == nullptr) {
                retired_visited[slot] = node;
                return true;
            }
            slot = (slot + 1u) & (retired_visited.size() - 1u);
        }
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq trusted join retired-node table overflow")));
        pg_unreachable();
    };

    /* A rotation may discard a destination-local branch after reusing its
     * children in the final tree.  Walk those explicitly recorded roots as a
     * second allocation closure.  Final-tree nodes may be shared by that
     * historical closure; cached height descent proves that the bounded
     * immutable allocation graph is acyclic even when old roots overlap. */
    for (std::uint32_t retired_index = 0u;
         retired_index < destination.retired_join_root_count;
         ++retired_index) {
        note_piece_tree_retired_root_step_noexcept();
        const piece_edge retired_root =
            destination.retired_join_roots[retired_index];
        if (!retired_root
            || retired_root.arena != std::addressof(destination)
            || retired_root.node->node_kind != piece_node::kind::branch
            || node_is_in_table(visited, retired_root.node)
            || node_is_in_table(retired_visited, retired_root.node)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq trusted join retired-root record is invalid")));
        }
        if (pending_count >= pending.size()) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq trusted join retired-root stack overflow")));
        }
        pending[pending_count++] = retired_root;
        while (pending_count != 0u) {
            const piece_edge current = pending[--pending_count];
            if (current.arena != std::addressof(destination)) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq trusted join retired graph escaped its arena")));
            }
            if (node_is_in_table(visited, current.node)) continue;
            if (visited_count + retired_visited_count
                >= bounded_join_node_capacity) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq trusted join retired graph exceeded its node bound")));
            }
            if (!insert_retired_node(current.node)) continue;
            ++retired_visited_count;
            audit.new_nodes_visited = static_cast<std::uint32_t>(
                visited_count + retired_visited_count);

            validate_bounded_edge_summary(current, audit, false);
            const piece_node& node = *current.node;
            if (node.node_kind != piece_node::kind::branch) {
                validate_bounded_join_leaf(
                    current, result_type, alphabet_bound);
                if (node.node_kind == piece_node::kind::source) {
                    require_source_dependency(node.payload.source.source);
                } else if (node.payload.literal.storage_arena
                           != std::addressof(destination)) {
                    require_arena_dependency(
                        node.payload.literal.storage_arena);
                }
                continue;
            }

            const auto& branch = node.payload.branch;
            validate_bounded_edge_summary(branch.left, audit,
                branch.left.arena != std::addressof(destination));
            validate_bounded_edge_summary(branch.right, audit,
                branch.right.arena != std::addressof(destination));
            if ((branch.left.arena != std::addressof(destination)
                    && branch.left.arena->generation
                        >= destination.generation)
                || (branch.right.arena != std::addressof(destination)
                    && branch.right.arena->generation
                        >= destination.generation)
                || branch.right.node->bp_length
                    > static_cast<std::uint64_t>(INT64_MAX)
                        - branch.left.node->bp_length
                || branch.left.node->bp_length
                        + branch.right.node->bp_length
                    != node.bp_length
                || static_cast<std::uint64_t>(
                        branch.left.node->piece_count)
                        + branch.right.node->piece_count
                    != node.piece_count
                || 1u + std::max(branch.left.node->height,
                                 branch.right.node->height)
                    != node.height
                || branch.left.node->height
                    > branch.right.node->height + 1u
                || branch.right.node->height
                    > branch.left.node->height + 1u) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq trusted join retired path invariant is invalid")));
            }
            const std::array children{branch.left, branch.right};
            for (piece_edge child : children) {
                if (child.arena != std::addressof(destination)) {
                    require_arena_dependency(child.arena);
                    continue;
                }
                if (pending_count >= pending.size()) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("bioseq trusted join retired validation stack overflow")));
                }
                pending[pending_count++] = child;
            }
        }
    }
    if (audit.new_nodes_visited != destination.node_count) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq trusted join arena contains unreachable nodes"),
                 errdetail("reachable=%u allocated=%u",
                           audit.new_nodes_visited, destination.node_count)));
    }

    if (required_arena_count != destination.arena_dependency_count
        || required_source_count != destination.source_dependency_count) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq trusted join dependency multiplicity changed"),
                 errdetail("required_arena=%u registered_arena=%u "
                           "required_source=%u registered_source=%u",
                           required_arena_count,
                           destination.arena_dependency_count,
                           required_source_count,
                           destination.source_dependency_count)));
    }

    for (auto* dependency = destination.arena_dependencies;
         dependency != nullptr; dependency = dependency->next) {
        note_piece_tree_bounded_validation_step_noexcept();
        if (audit.arena_dependencies_checked
                >= destination.arena_dependency_count
            || dependency->child == nullptr
            || dependency->child->generation == 0u
            || dependency->child->generation >= destination.generation
            || dependency->child->refcount == 0u
            || dependency->child->refcount == UINT32_MAX) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq trusted join arena dependency is invalid")));
        }
        std::size_t slot_index = bounded_join_pointer_hash(dependency->child)
            & (required_arenas.size() - 1u);
        bool matched = false;
        for (std::size_t probe = 0u; probe < required_arenas.size(); ++probe) {
            auto& slot = required_arenas[slot_index];
            if (slot.key == nullptr) break;
            if (slot.key == dependency->child) {
                if (slot.matched >= slot.required) break;
                ++slot.matched;
                matched = true;
                break;
            }
            slot_index = (slot_index + 1u) & (required_arenas.size() - 1u);
        }
        if (!matched) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq trusted join arena dependency does not match a reachable edge")));
        }
        ++audit.arena_dependencies_checked;
    }
    for (auto* dependency = destination.source_dependencies;
         dependency != nullptr; dependency = dependency->next) {
        note_piece_tree_bounded_validation_step_noexcept();
        if (audit.source_dependencies_checked
                >= destination.source_dependency_count
            || !pg_sequence_source_handle_valid(dependency->source)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq trusted join source dependency is invalid")));
        }
        std::size_t slot_index = (
            bounded_join_pointer_hash(dependency->source.state)
            ^ (bounded_join_pointer_hash(dependency->source.ops) << 1u))
            & (required_sources.size() - 1u);
        bool matched = false;
        for (std::size_t probe = 0u; probe < required_sources.size(); ++probe) {
            auto& slot = required_sources[slot_index];
            if (slot.state == nullptr) break;
            if (slot.state == dependency->source.state
                && slot.ops == dependency->source.ops) {
                if (slot.matched >= slot.required) break;
                ++slot.matched;
                matched = true;
                break;
            }
            slot_index = (slot_index + 1u) & (required_sources.size() - 1u);
        }
        if (!matched) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq trusted join source dependency does not match a reachable leaf")));
        }
        ++audit.source_dependencies_checked;
    }
    if (audit.arena_dependencies_checked
            != destination.arena_dependency_count
        || audit.source_dependencies_checked
            != destination.source_dependency_count) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq trusted join dependency accounting changed")));
    }
    return audit;
}

export inline void publish_root_noexcept(pg_sequence_expanded &value,
                                         piece_edge next,
                                         compact_sequence_stdc_type_params type,
                                         std::uint64_t length,
                                         bool alphabet_bound) noexcept {
  value.root = next;
  value.type_params = type;
  value.bp_length = length;
  value.alphabet_bound = alphabet_bound;
}

namespace {

[[nodiscard]] inline piece_edge
build_balanced_literal_probe(piece_tree_arena &arena, literal_piece literal,
                             std::uint32_t piece_count) {
  if (piece_count == 1u) {
    return make_literal_node(arena, literal);
  }
  const std::uint32_t left_count = piece_count / 2u;
  piece_edge left = build_balanced_literal_probe(arena, literal, left_count);
  piece_edge right =
      build_balanced_literal_probe(arena, literal, piece_count - left_count);
  return make_branch_node(arena, left, right);
}

struct factory_failure_observation {
  std::uint32_t callback_invocations;
  std::uint32_t armed_callback_invocations;
  std::uint32_t root_tokens_released;
  std::uint32_t arena_tokens_released;
};

struct factory_failure_pending_root {
  MemoryContextCallback callback;
  piece_tree_arena *destination_arena;
  piece_edge root;
  factory_failure_observation *observation;
  bool armed;
};

inline void factory_failure_pending_root_reset(void *argument) noexcept {
  auto *guard = static_cast<factory_failure_pending_root *>(argument);
  if (guard == nullptr || guard->observation == nullptr) {
    return;
  }
  ++guard->observation->callback_invocations;
  if (!guard->armed) {
    return;
  }
  guard->armed = false;
  ++guard->observation->armed_callback_invocations;
  if (guard->root) {
    ++guard->observation->root_tokens_released;
    release_edge(guard->root);
    guard->root = {};
  }
  if (guard->destination_arena != nullptr) {
    ++guard->observation->arena_tokens_released;
    release_arena(guard->destination_arena);
    guard->destination_arena = nullptr;
  }
}

struct factory_failure_source_state {
  std::uint32_t retain_calls;
  std::uint32_t release_calls;
  std::uint32_t read_calls;
  std::uint64_t rank_count;
  std::array<std::uint8_t, 64u> ranks;
  bool invalid_read;
};

inline void factory_failure_source_retain(void *argument) noexcept {
  ++static_cast<factory_failure_source_state *>(argument)->retain_calls;
}

inline void factory_failure_source_release(void *argument) noexcept {
  ++static_cast<factory_failure_source_state *>(argument)->release_calls;
}

[[nodiscard]] inline pg_sequence_source_identity
factory_failure_source_identity(void const *argument) noexcept {
  return {
      .control_address = reinterpret_cast<std::uintptr_t>(argument),
      .generation = 1u,
  };
}

[[nodiscard]] inline bool
factory_failure_source_worker_shareable(void const *) noexcept {
  return false;
}

[[nodiscard]] inline pg_sequence_source_read_status
factory_failure_source_read(
    void const *argument, pg_sequence_read_session &,
    bioseq::storage::seq_locator const &,
    compact_sequence_stdc_type_params,
    bioseq::storage::sequence_transform transform,
    std::uint64_t source_from, std::span<std::uint8_t> output) {
  auto *state = const_cast<factory_failure_source_state *>(
      static_cast<factory_failure_source_state const *>(argument));
  ++state->read_calls;
  if (source_from > state->rank_count
      || output.size() > state->rank_count - source_from) {
    state->invalid_read = true;
    std::fill(output.begin(), output.end(), 0u);
    return pg_sequence_source_read_status::ok;
  }

  const bool reverse = bioseq::storage::has_reverse(transform);
  const bool complement = bioseq::storage::has_complement(transform);
  for (std::size_t output_index = 0u;
       output_index < output.size(); ++output_index) {
    const std::uint64_t physical = source_from
        + (reverse ? output.size() - 1u - output_index : output_index);
    std::uint8_t rank = state->ranks[static_cast<std::size_t>(physical)];
    if (rank >= 4u) {
      state->invalid_read = true;
      rank = 0u;
    } else if (complement) {
      rank = static_cast<std::uint8_t>(3u - rank);
    }
    output[output_index] = rank;
  }
  return pg_sequence_source_read_status::ok;
}

constexpr pg_sequence_source_backend_ops factory_failure_source_ops{
    .retain = factory_failure_source_retain,
    .release = factory_failure_source_release,
    .identity = factory_failure_source_identity,
    .worker_shareable = factory_failure_source_worker_shareable,
    .read_ranks = factory_failure_source_read,
};

[[nodiscard]] inline literal_piece factory_failure_unbound_literal(
    piece_tree_arena *storage_arena,
    std::byte const *bytes,
    std::uint64_t from,
    std::uint64_t to) noexcept {
  return {
      .storage_arena = storage_arena,
      .bytes = bytes,
      .symbol_from = from,
      .symbol_to = to,
      .stored_type_params =
          {
              .alphabet_code = pgseq_unbound_alphabet_code,
              .storage_unit_byte_width = pgseq_unbound_width_tag,
          },
      .encoding = literal_encoding::unbound_ascii,
      .validation = literal_validation::unvalidated,
  };
}

[[nodiscard]] inline bool run_piece_tree_factory_failure_case(
    piece_tree_injected_failure_point failure_point) {
  const std::uint64_t arena_baseline = live_piece_tree_arenas;
  piece_tree_arena *external_arena = nullptr;
  piece_edge external_root{};
  std::byte *external_storage = nullptr;

  if (failure_point
          == piece_tree_injected_failure_point::
              arena_dependency_registration
      || failure_point
          == piece_tree_injected_failure_point::literal_binding_factory) {
    external_arena = make_piece_tree_arena();
    if (failure_point
        == piece_tree_injected_failure_point::literal_binding_factory) {
      external_storage = allocate_piece_literal_storage(*external_arena, 2u);
      external_storage[0] = std::byte{0x41};
      external_storage[1] = std::byte{0x43};
      external_root = make_literal_node(
          *external_arena,
          factory_failure_unbound_literal(
              external_arena, external_storage, 0u, 2u));
    }
  }

  auto *observation = static_cast<factory_failure_observation *>(
      palloc0(sizeof(factory_failure_observation)));
  auto *source_state = static_cast<factory_failure_source_state *>(
      palloc0(sizeof(factory_failure_source_state)));
  const pg_sequence_source_handle source{
      .state = source_state,
      .ops = std::addressof(factory_failure_source_ops),
  };
  const compact_sequence_stdc_type_params bound_type{
      .alphabet_code = pgseq_supported_alphabets::index_of<seq::dna4>(),
      .storage_unit_byte_width = 8u,
  };
  const bioseq::storage::seq_locator locator{
      .num_fragments = 1u,
      .total_bp_size = 1u,
      .source_epoch = 1u,
  };
  const source_piece source_leaf{
      .source = source,
      .locator = locator,
      .stored_type_params = bound_type,
      .last_padding_letters = 0u,
      .from = 0u,
      .to = 1u,
  };

  MemoryContext build_context = AllocSetContextCreate(
      CurrentMemoryContext, "bioseq injected piece build",
      ALLOCSET_SMALL_SIZES);
  const MemoryContext saved_context = CurrentMemoryContext;
  bool volatile caught = false;
  observe_piece_tree_failure_noexcept(
      piece_tree_injected_failure_point::none);
  arm_piece_tree_failure_noexcept(failure_point);

  PG_TRY();
  {
    auto *guard = static_cast<factory_failure_pending_root *>(
        MemoryContextAllocZero(
            build_context, sizeof(factory_failure_pending_root)));
    guard->callback.func = factory_failure_pending_root_reset;
    guard->callback.arg = guard;
    guard->observation = observation;
    guard->armed = true;
    MemoryContextRegisterResetCallback(
        build_context, std::addressof(guard->callback));

    guard->destination_arena = make_piece_tree_arena();
    switch (failure_point) {
      case piece_tree_injected_failure_point::arena_factory:
        inject_piece_tree_failure(failure_point);
        break;
      case piece_tree_injected_failure_point::literal_storage_factory: {
        (void)allocate_piece_literal_storage(
            *guard->destination_arena, 1u);
        inject_piece_tree_failure(failure_point);
        break;
      }
      case piece_tree_injected_failure_point::literal_leaf_factory: {
        auto *storage = allocate_piece_literal_storage(
            *guard->destination_arena, 1u);
        storage[0] = std::byte{0x41};
        guard->root = make_literal_node(
            *guard->destination_arena,
            factory_failure_unbound_literal(
                guard->destination_arena, storage, 0u, 1u));
        inject_piece_tree_failure(failure_point);
        break;
      }
      case piece_tree_injected_failure_point::branch_factory: {
        auto *storage = allocate_piece_literal_storage(
            *guard->destination_arena, 2u);
        storage[0] = std::byte{0x41};
        storage[1] = std::byte{0x43};
        piece_edge left = make_literal_node(
            *guard->destination_arena,
            factory_failure_unbound_literal(
                guard->destination_arena, storage, 0u, 1u));
        piece_edge right = make_literal_node(
            *guard->destination_arena,
            factory_failure_unbound_literal(
                guard->destination_arena, storage, 1u, 2u));
        guard->root = make_branch_node(
            *guard->destination_arena, left, right);
        inject_piece_tree_failure(failure_point);
        break;
      }
      case piece_tree_injected_failure_point::
          arena_dependency_registration:
        register_arena_dependency(
            *guard->destination_arena, external_arena);
        /* Test the dependency operation directly: an exported inline factory
         * may be instantiated by an importer with a distinct anonymous-
         * namespace acceptance state.  The helper raises first when it sees
         * this TU's arm; the immediate fallback preserves the identical
         * post-registration/pre-publication failure point for any compiler-
         * emitted state copy. */
        inject_piece_tree_failure(failure_point);
        break;
      case piece_tree_injected_failure_point::
          source_dependency_registration:
        register_source_dependency(
            *guard->destination_arena, source);
        inject_piece_tree_failure(failure_point);
        break;
      case piece_tree_injected_failure_point::source_leaf_factory:
        guard->root = make_source_node(
            *guard->destination_arena, source_leaf);
        inject_piece_tree_failure(failure_point);
        break;
      case piece_tree_injected_failure_point::literal_binding_factory:
        guard->root = bind_literals_copy(
            *guard->destination_arena, external_root, bound_type);
        inject_piece_tree_failure(failure_point);
        break;
      case piece_tree_injected_failure_point::none:
        break;
    }

    arm_piece_tree_failure_noexcept(
        piece_tree_injected_failure_point::none);
    MemoryContextDelete(build_context);
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(saved_context);
    arm_piece_tree_failure_noexcept(
        piece_tree_injected_failure_point::none);
    FlushErrorState();
    MemoryContextDelete(build_context);
    caught = true;
  }
  PG_END_TRY();

  const bool source_case =
      failure_point
          == piece_tree_injected_failure_point::
              source_dependency_registration
      || failure_point
          == piece_tree_injected_failure_point::source_leaf_factory;
  const bool published_root_case =
      failure_point
          == piece_tree_injected_failure_point::literal_leaf_factory
      || failure_point
          == piece_tree_injected_failure_point::branch_factory
      || failure_point
          == piece_tree_injected_failure_point::source_leaf_factory
      || failure_point
          == piece_tree_injected_failure_point::literal_binding_factory;
  const bool external_refcount_restored =
      external_arena == nullptr
      || (failure_point
              == piece_tree_injected_failure_point::
                  arena_dependency_registration
              ? external_arena->refcount == 1u
              : external_arena->refcount == 2u);

  if (external_root) {
    release_edge(external_root);
    external_root = {};
  }
  if (external_arena != nullptr) {
    release_arena(external_arena);
    external_arena = nullptr;
  }

  const bool result =
      caught
      && observed_piece_tree_failure_noexcept() == failure_point
      && observation->callback_invocations == 1u
      && observation->armed_callback_invocations == 1u
      && observation->root_tokens_released
          == static_cast<std::uint32_t>(published_root_case)
      && observation->arena_tokens_released == 1u
      && source_state->retain_calls
          == static_cast<std::uint32_t>(source_case)
      && source_state->release_calls
          == static_cast<std::uint32_t>(source_case)
      && external_refcount_restored
      && live_piece_tree_arenas == arena_baseline;
  if (!result) {
    ereport(WARNING,
            (errmsg_internal(
                "bioseq piece-tree factory failure case: point=%u caught=%d observed=%u callbacks=%u armed_callbacks=%u roots=%u expected_roots=%u arenas=%u source_retains=%u expected_source=%u source_releases=%u external_restored=%d live=%llu baseline=%llu",
                static_cast<unsigned>(failure_point), caught ? 1 : 0,
                static_cast<unsigned>(
                    observed_piece_tree_failure_noexcept()),
                observation->callback_invocations,
                observation->armed_callback_invocations,
                observation->root_tokens_released,
                static_cast<unsigned>(published_root_case),
                observation->arena_tokens_released,
                source_state->retain_calls,
                static_cast<unsigned>(source_case),
                source_state->release_calls,
                external_refcount_restored ? 1 : 0,
                static_cast<unsigned long long>(live_piece_tree_arenas),
                static_cast<unsigned long long>(arena_baseline))));
  }
  pfree(source_state);
  pfree(observation);
  return result;
}

[[nodiscard]] inline bool run_piece_tree_factory_failure_matrix() {
  for (std::uint8_t ordinal = static_cast<std::uint8_t>(
           piece_tree_injected_failure_point::arena_factory);
       ordinal <= static_cast<std::uint8_t>(
           piece_tree_injected_failure_point::literal_binding_factory);
       ++ordinal) {
    if (!run_piece_tree_factory_failure_case(
            static_cast<piece_tree_injected_failure_point>(ordinal))) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] inline bool consume_expected_error(
    int expected_sqlstate, MemoryContext destination_context) {
  MemoryContextSwitchTo(destination_context);
  ErrorData *error = CopyErrorData();
  const bool matched =
      error != nullptr && error->sqlerrcode == expected_sqlstate;
  FlushErrorState();
  if (error != nullptr) {
    FreeErrorData(error);
  }
  return matched;
}

enum class tracked_join_failure_shape : std::uint8_t {
  branch,
  source_coalesce,
  literal_coalesce,
  rotation,
  transformed_binding,
};

struct tracked_join_failure_fixture {
  piece_tree_arena *arena;
  piece_edge left;
  piece_edge right;
  factory_failure_source_state *source_state;
  compact_sequence_stdc_type_params type;
  std::uint64_t live_baseline;
  std::uint32_t arena_refcount_baseline;
  std::uint32_t source_retain_baseline;
  std::uint32_t source_release_baseline;
};

[[nodiscard]] inline tracked_join_failure_fixture *
make_tracked_join_failure_fixture(tracked_join_failure_shape shape) {
  auto *fixture = static_cast<tracked_join_failure_fixture *>(
      palloc0(sizeof(tracked_join_failure_fixture)));
  fixture->live_baseline = live_piece_tree_arenas;
  fixture->arena = make_piece_tree_arena();

  if (shape == tracked_join_failure_shape::literal_coalesce
      || shape == tracked_join_failure_shape::transformed_binding) {
    fixture->type = shape == tracked_join_failure_shape::transformed_binding
        ? compact_sequence_stdc_type_params{
              .alphabet_code =
                  pgseq_supported_alphabets::index_of<seq::dna4>(),
              .storage_unit_byte_width = 8u,
          }
        : compact_sequence_stdc_type_params{
              .alphabet_code = pgseq_unbound_alphabet_code,
              .storage_unit_byte_width = pgseq_unbound_width_tag,
          };
    auto *left_storage = allocate_piece_literal_storage(*fixture->arena, 1u);
    auto *right_storage = allocate_piece_literal_storage(*fixture->arena, 1u);
    left_storage[0] = std::byte{0x41};
    right_storage[0] = std::byte{0x43};
    piece_edge left = make_literal_node(
        *fixture->arena,
        factory_failure_unbound_literal(
            fixture->arena, left_storage, 0u, 1u));
    piece_edge right = make_literal_node(
        *fixture->arena,
        factory_failure_unbound_literal(
            fixture->arena, right_storage, 0u, 1u));
    if (shape == tracked_join_failure_shape::transformed_binding) {
      fixture->left = make_branch_node(
          *fixture->arena, left, right);
      fixture->left.transform =
          bioseq::storage::sequence_transform::reversed;
    } else {
      fixture->left = left;
      fixture->right = right;
    }
  } else {
    fixture->source_state = static_cast<factory_failure_source_state *>(
        palloc0(sizeof(factory_failure_source_state)));
    fixture->source_state->rank_count = 64u;
    fixture->type = {
        .alphabet_code = pgseq_supported_alphabets::index_of<seq::dna4>(),
        .storage_unit_byte_width = 8u,
    };
    const pg_sequence_source_handle source{
        .state = fixture->source_state,
        .ops = std::addressof(factory_failure_source_ops),
    };
    const auto make_leaf = [&fixture, source](
        std::uint64_t from, std::uint64_t to,
        bioseq::storage::seq_locator locator) {
      return make_source_node(
          *fixture->arena,
          source_piece{
              .source = source,
              .locator = locator,
              .stored_type_params = fixture->type,
              .last_padding_letters = 0u,
              .from = from,
              .to = to,
          });
    };

    if (shape == tracked_join_failure_shape::source_coalesce) {
      const bioseq::storage::seq_locator locator{
          .num_fragments = 1u,
          .total_bp_size = 2u,
          .source_epoch = 1u,
      };
      fixture->left = make_leaf(0u, 1u, locator);
      fixture->right = make_leaf(1u, 2u, locator);
    } else if (shape == tracked_join_failure_shape::branch) {
      const bioseq::storage::seq_locator locator{
          .num_fragments = 1u,
          .total_bp_size = 1u,
          .source_epoch = 1u,
      };
      fixture->left = make_leaf(0u, 1u, locator);
      fixture->right = make_leaf(0u, 1u, locator);
    } else {
      const bioseq::storage::seq_locator locator{
          .num_fragments = 1u,
          .total_bp_size = 1u,
          .source_epoch = 1u,
      };
      std::array<piece_edge, 7u> leaves{};
      for (piece_edge& leaf : leaves) {
        leaf = make_leaf(0u, 1u, locator);
      }
      piece_edge left_pair = make_branch_node(
          *fixture->arena, leaves[0], leaves[1]);
      piece_edge right_left = make_branch_node(
          *fixture->arena, leaves[2], leaves[3]);
      piece_edge right_right = make_branch_node(
          *fixture->arena, leaves[4], leaves[5]);
      piece_edge right_four = make_branch_node(
          *fixture->arena, right_left, right_right);
      fixture->left = make_branch_node(
          *fixture->arena, left_pair, right_four);
      fixture->right = leaves[6];
    }
  }

  fixture->arena_refcount_baseline = fixture->arena->refcount;
  if (fixture->source_state != nullptr) {
    fixture->source_retain_baseline =
        fixture->source_state->retain_calls;
    fixture->source_release_baseline =
        fixture->source_state->release_calls;
  }
  return fixture;
}

[[nodiscard]] inline bool destroy_tracked_join_failure_fixture(
    tracked_join_failure_fixture *fixture) {
  auto *source_state = fixture->source_state;
  const std::uint64_t live_baseline = fixture->live_baseline;
  release_edge(fixture->left);
  release_edge(fixture->right);
  release_arena(fixture->arena);
  const bool source_balanced = source_state == nullptr
      || source_state->retain_calls == source_state->release_calls;
  if (source_state != nullptr) pfree(source_state);
  pfree(fixture);
  return source_balanced && live_piece_tree_arenas == live_baseline;
}

struct tracked_join_failure_observation {
  std::uint32_t callbacks;
  std::uint32_t armed_callbacks;
  std::uint32_t tokens_released;
  std::uint32_t destination_tokens_released;
  bool ledger_drained;
};

struct tracked_join_failure_guard {
  MemoryContextCallback callback;
  piece_tree_arena *destination_arena;
  piece_edge_cleanup_ledger ledger;
  tracked_join_failure_observation *observation;
  bool armed;
};

inline void tracked_join_failure_reset(void *argument) noexcept {
  auto *guard = static_cast<tracked_join_failure_guard *>(argument);
  if (guard == nullptr || guard->observation == nullptr) return;
  ++guard->observation->callbacks;
  if (!guard->armed) return;
  guard->armed = false;
  ++guard->observation->armed_callbacks;
  guard->observation->tokens_released += guard->ledger.token_count;
  release_piece_edge_cleanup_ledger_noexcept(guard->ledger);
  guard->observation->ledger_drained =
      piece_edge_cleanup_ledger_empty_noexcept(guard->ledger);
  if (guard->destination_arena != nullptr) {
    ++guard->observation->destination_tokens_released;
    release_arena(guard->destination_arena);
    guard->destination_arena = nullptr;
  }
}

[[nodiscard]] inline bool run_tracked_join_failure_case(
    piece_tree_injected_failure_point failure_point,
    tracked_join_failure_shape shape) {
  auto *fixture = make_tracked_join_failure_fixture(shape);
  const std::uint64_t fixture_live_arenas = live_piece_tree_arenas;
  auto *observation = static_cast<tracked_join_failure_observation *>(
      palloc0(sizeof(tracked_join_failure_observation)));
  MemoryContext build_context = AllocSetContextCreate(
      CurrentMemoryContext, "bioseq tracked join failure",
      ALLOCSET_SMALL_SIZES);
  const MemoryContext saved_context = CurrentMemoryContext;
  bool volatile caught = false;
  bool volatile sqlstate_matched = false;
  observe_piece_tree_failure_noexcept(
      piece_tree_injected_failure_point::none);
  arm_piece_tree_failure_noexcept(failure_point);

  PG_TRY();
  {
    auto *guard = static_cast<tracked_join_failure_guard *>(
        MemoryContextAllocZero(
            build_context, sizeof(tracked_join_failure_guard)));
    guard->callback.func = tracked_join_failure_reset;
    guard->callback.arg = guard;
    guard->observation = observation;
    guard->armed = true;
    MemoryContextRegisterResetCallback(
        build_context, std::addressof(guard->callback));
    guard->destination_arena = make_piece_tree_arena();
    if (shape == tracked_join_failure_shape::transformed_binding) {
      (void)bind_literals_copy_error_atomic(
          *guard->destination_arena, fixture->left,
          fixture->type, guard->ledger);
    } else {
      (void)join_edges_error_atomic(
          *guard->destination_arena, guard->ledger,
          fixture->left, fixture->right);
    }
    arm_piece_tree_failure_noexcept(
        piece_tree_injected_failure_point::none);
    MemoryContextDelete(build_context);
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(saved_context);
    arm_piece_tree_failure_noexcept(
        piece_tree_injected_failure_point::none);
    sqlstate_matched = consume_expected_error(
        ERRCODE_INTERNAL_ERROR, saved_context);
    MemoryContextDelete(build_context);
    caught = true;
  }
  PG_END_TRY();

  const bool fixture_restored =
      fixture->arena->refcount == fixture->arena_refcount_baseline
      && live_piece_tree_arenas == fixture_live_arenas
      && (fixture->source_state == nullptr
          || (fixture->source_state->retain_calls
                  >= fixture->source_retain_baseline
              && fixture->source_state->release_calls
                  >= fixture->source_release_baseline
              && fixture->source_state->retain_calls
                      - fixture->source_retain_baseline
                  == fixture->source_state->release_calls
                      - fixture->source_release_baseline));
  const bool case_passed = caught && sqlstate_matched
      && observed_piece_tree_failure_noexcept() == failure_point
      && observation->callbacks == 1u
      && observation->armed_callbacks == 1u
      && observation->tokens_released != 0u
      && observation->destination_tokens_released == 1u
      && observation->ledger_drained && fixture_restored;
  if (!case_passed) {
    ereport(WARNING,
            (errmsg_internal(
                "bioseq tracked-join failure case: point=%u shape=%u caught=%d sqlstate=%d observed=%u callbacks=%u armed=%u tokens=%u destinations=%u drained=%d restored=%d",
                static_cast<unsigned>(failure_point),
                static_cast<unsigned>(shape), caught ? 1 : 0,
                sqlstate_matched ? 1 : 0,
                static_cast<unsigned>(
                    observed_piece_tree_failure_noexcept()),
                observation->callbacks,
                observation->armed_callbacks,
                observation->tokens_released,
                observation->destination_tokens_released,
                observation->ledger_drained ? 1 : 0,
                fixture_restored ? 1 : 0)));
  }
  pfree(observation);
  const bool fixture_destroyed =
      destroy_tracked_join_failure_fixture(fixture);
  return case_passed && fixture_destroyed;
}

[[nodiscard]] inline bool run_tracked_join_failure_matrix() {
  using point = piece_tree_injected_failure_point;
  using shape = tracked_join_failure_shape;
  constexpr std::array cases{
      std::pair{point::join_left_operand_retain, shape::branch},
      std::pair{point::join_right_operand_retain, shape::branch},
      std::pair{point::arena_dependency_registration, shape::branch},
      std::pair{point::source_dependency_registration,
                shape::source_coalesce},
      std::pair{point::literal_storage_factory,
                shape::literal_coalesce},
      std::pair{point::join_node_allocation, shape::branch},
      std::pair{point::join_node_allocation, shape::source_coalesce},
      std::pair{point::join_node_allocation, shape::literal_coalesce},
      std::pair{point::join_node_metadata, shape::branch},
      std::pair{point::join_node_metadata, shape::source_coalesce},
      std::pair{point::join_node_metadata, shape::literal_coalesce},
      std::pair{point::join_factory_transition, shape::branch},
      std::pair{point::join_factory_transition, shape::source_coalesce},
      std::pair{point::join_factory_transition, shape::literal_coalesce},
      std::pair{point::join_edge_identity_transition,
                shape::transformed_binding},
      std::pair{point::join_recursive_intermediate, shape::rotation},
      std::pair{point::join_retired_root_record, shape::rotation},
  };
  static_assert(cases.size() >= 16u);
  for (auto const& [failure_point, fixture_shape] : cases) {
    if (!run_tracked_join_failure_case(failure_point, fixture_shape)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] inline bool run_tracked_slice_failure_iteration(
    piece_edge source_root,
    piece_tree_arena& source_arena,
    factory_failure_source_state& source_state)
{
  const std::uint64_t live_baseline = live_piece_tree_arenas;
  const std::uint32_t arena_refcount_baseline = source_arena.refcount;
  const std::uint32_t source_retain_baseline = source_state.retain_calls;
  const std::uint32_t source_release_baseline = source_state.release_calls;
  auto* observation = static_cast<tracked_join_failure_observation*>(
      palloc0(sizeof(tracked_join_failure_observation)));
  MemoryContext build_context = AllocSetContextCreate(
      CurrentMemoryContext, "bioseq tracked slice failure",
      ALLOCSET_SMALL_SIZES);
  const MemoryContext saved_context = CurrentMemoryContext;
  bool volatile caught = false;
  bool volatile sqlstate_matched = false;
  observe_piece_tree_failure_noexcept(
      piece_tree_injected_failure_point::none);
  arm_piece_tree_failure_noexcept(
      piece_tree_injected_failure_point::split_first_leaf_intermediate);

  PG_TRY();
  {
    auto* guard = static_cast<tracked_join_failure_guard*>(
        MemoryContextAllocZero(
            build_context, sizeof(tracked_join_failure_guard)));
    guard->callback.func = tracked_join_failure_reset;
    guard->callback.arg = guard;
    guard->observation = observation;
    guard->armed = true;
    MemoryContextRegisterResetCallback(
        build_context, std::addressof(guard->callback));
    guard->destination_arena = make_piece_tree_arena();
    (void)slice_tree(
        *guard->destination_arena, guard->ledger,
        source_root, 1u, 3u);
    arm_piece_tree_failure_noexcept(
        piece_tree_injected_failure_point::none);
    MemoryContextDelete(build_context);
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(saved_context);
    arm_piece_tree_failure_noexcept(
        piece_tree_injected_failure_point::none);
    sqlstate_matched = consume_expected_error(
        ERRCODE_INTERNAL_ERROR, saved_context);
    MemoryContextDelete(build_context);
    caught = true;
  }
  PG_END_TRY();

  const bool restored = source_arena.refcount == arena_refcount_baseline
      && live_piece_tree_arenas == live_baseline
      && source_state.retain_calls > source_retain_baseline
      && source_state.release_calls > source_release_baseline
      && source_state.retain_calls - source_retain_baseline
          == source_state.release_calls - source_release_baseline;
  const bool passed = caught && sqlstate_matched
      && observed_piece_tree_failure_noexcept()
          == piece_tree_injected_failure_point::split_first_leaf_intermediate
      && observation->callbacks == 1u
      && observation->armed_callbacks == 1u
      && observation->tokens_released == 1u
      && observation->destination_tokens_released == 1u
      && observation->ledger_drained && restored;
  pfree(observation);
  return passed;
}

[[nodiscard]] inline bool run_tracked_slice_failure_matrix()
{
  constexpr std::uint32_t repetitions = 64u;
  const std::uint64_t live_baseline = live_piece_tree_arenas;
  factory_failure_source_state source_state{};
  const pg_sequence_source_handle source{
      .state = std::addressof(source_state),
      .ops = std::addressof(factory_failure_source_ops),
  };
  const compact_sequence_stdc_type_params bound_type{
      .alphabet_code = pgseq_supported_alphabets::index_of<seq::dna4>(),
      .storage_unit_byte_width = 8u,
  };
  auto* source_arena = make_piece_tree_arena();
  const piece_edge source_root = make_source_node(
      *source_arena,
      source_piece{
          .source = source,
          .locator = {
              .num_fragments = 1u,
              .total_bp_size = 4u,
              .source_epoch = 1u,
          },
          .stored_type_params = bound_type,
          .last_padding_letters = 0u,
          .from = 0u,
          .to = 4u,
      });
  bool passed = true;
  for (std::uint32_t iteration = 0u;
       iteration < repetitions && passed; ++iteration) {
    passed = run_tracked_slice_failure_iteration(
        source_root, *source_arena, source_state);
  }
  release_edge(source_root);
  release_arena(source_arena);
  return passed
      && source_state.retain_calls == source_state.release_calls
      && live_piece_tree_arenas == live_baseline;
}

[[nodiscard]] inline bool run_invalid_later_literal_binding_iteration(
    piece_edge source_root,
    piece_tree_arena& source_arena,
    compact_sequence_stdc_type_params bound_type)
{
  const std::uint64_t live_baseline = live_piece_tree_arenas;
  const std::uint32_t arena_refcount_baseline = source_arena.refcount;
  auto* observation = static_cast<tracked_join_failure_observation*>(
      palloc0(sizeof(tracked_join_failure_observation)));
  MemoryContext build_context = AllocSetContextCreate(
      CurrentMemoryContext, "bioseq invalid later literal binding",
      ALLOCSET_SMALL_SIZES);
  const MemoryContext saved_context = CurrentMemoryContext;
  bool volatile caught = false;
  bool volatile sqlstate_matched = false;

  PG_TRY();
  {
    auto* guard = static_cast<tracked_join_failure_guard*>(
        MemoryContextAllocZero(
            build_context, sizeof(tracked_join_failure_guard)));
    guard->callback.func = tracked_join_failure_reset;
    guard->callback.arg = guard;
    guard->observation = observation;
    guard->armed = true;
    MemoryContextRegisterResetCallback(
        build_context, std::addressof(guard->callback));
    guard->destination_arena = make_piece_tree_arena();
    (void)bind_literals_copy_error_atomic(
        *guard->destination_arena, source_root,
        bound_type, guard->ledger);
    MemoryContextDelete(build_context);
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(saved_context);
    sqlstate_matched = consume_expected_error(
        ERRCODE_INVALID_TEXT_REPRESENTATION, saved_context);
    MemoryContextDelete(build_context);
    caught = true;
  }
  PG_END_TRY();

  const bool passed = caught && sqlstate_matched
      && observation->callbacks == 1u
      && observation->armed_callbacks == 1u
      && observation->tokens_released == 1u
      && observation->destination_tokens_released == 1u
      && observation->ledger_drained
      && source_arena.refcount == arena_refcount_baseline
      && live_piece_tree_arenas == live_baseline;
  pfree(observation);
  return passed;
}

[[nodiscard]] inline bool run_invalid_later_literal_binding_matrix()
{
  constexpr std::uint32_t repetitions = 64u;
  const std::uint64_t live_baseline = live_piece_tree_arenas;
  const compact_sequence_stdc_type_params bound_type{
      .alphabet_code = pgseq_supported_alphabets::index_of<seq::dna4>(),
      .storage_unit_byte_width = 8u,
  };
  auto* source_arena = make_piece_tree_arena();
  auto* storage = allocate_piece_literal_storage(*source_arena, 3u);
  storage[0] = std::byte{0x41};
  storage[1] = std::byte{0x43};
  storage[2] = std::byte{0x58};
  piece_edge valid = make_literal_node(
      *source_arena,
      factory_failure_unbound_literal(source_arena, storage, 0u, 2u));
  piece_edge invalid = make_literal_node(
      *source_arena,
      factory_failure_unbound_literal(source_arena, storage, 2u, 3u));
  const piece_edge source_root = make_branch_node(
      *source_arena, valid, invalid);
  bool passed = true;
  for (std::uint32_t iteration = 0u;
       iteration < repetitions && passed; ++iteration) {
    passed = run_invalid_later_literal_binding_iteration(
        source_root, *source_arena, bound_type);
  }
  release_edge(source_root);
  release_arena(source_arena);
  return passed && live_piece_tree_arenas == live_baseline;
}

[[nodiscard]] inline bool run_piece_tree_quarantine_contract_noexcept()
    noexcept
{
  piece_tree_quarantine_snapshot observed{};
  piece_tree_arena suspect{};
  suspect.refcount = 1u;
  suspect.generation = 77u;
  suspect.release_next = std::addressof(suspect);
  record_piece_tree_quarantine_noexcept(
      observed,
      piece_tree_quarantine_reason::arena_refcount,
      "negative arena quarantine",
      std::addressof(suspect));
  record_piece_tree_quarantine_noexcept(
      observed,
      piece_tree_quarantine_reason::cleanup_ledger,
      "negative ledger quarantine",
      std::addressof(suspect));
  return observed.invariant_failures == 2u
      && observed.quarantined_arenas == 1u
      && observed.first_reason
          == piece_tree_quarantine_reason::arena_refcount
      && observed.last_reason
          == piece_tree_quarantine_reason::cleanup_ledger
      && std::strcmp(observed.first_detail,
                     "negative arena quarantine") == 0
      && std::strcmp(observed.last_detail,
                     "negative ledger quarantine") == 0
      && observed.last_arena_generation == 77u
      && suspect.refcount == UINT32_MAX
      && suspect.release_next == nullptr;
}

[[nodiscard]] inline bool run_piece_tree_error_atomic_matrix()
{
  return run_piece_tree_quarantine_contract_noexcept()
      && run_tracked_slice_failure_matrix()
      && run_invalid_later_literal_binding_matrix();
}

struct binding_retirement_fixture {
  piece_tree_arena *arena;
  piece_edge root;
  std::uint64_t length;
  std::uint32_t arena_refcount_baseline;
  std::uint64_t live_baseline;
};

[[nodiscard]] inline binding_retirement_fixture
make_binding_retirement_fixture() {
  constexpr std::uint32_t leaf_count = 4'096u;
  constexpr std::uint64_t long_length = 17'000u;
  constexpr std::uint64_t short_length = 3'000u;
  auto *arena = make_piece_tree_arena();
  auto *storage = allocate_piece_literal_storage(*arena, long_length);
  std::fill_n(storage, long_length, std::byte{0x41});
  auto *level = static_cast<piece_edge *>(
      palloc0(sizeof(piece_edge) * leaf_count));
  const compact_sequence_stdc_type_params unbound_type{
      .alphabet_code = pgseq_unbound_alphabet_code,
      .storage_unit_byte_width = pgseq_unbound_width_tag,
  };
  std::uint64_t total_length = 0u;
  for (std::uint32_t index = 0u; index < leaf_count; ++index) {
    const std::uint64_t length = index % 5u == 0u
        ? long_length : short_length;
    total_length += length;
    level[index] = make_literal_node(
        *arena,
        literal_piece{
            .storage_arena = arena,
            .bytes = storage,
            .symbol_from = 0u,
            .symbol_to = length,
            .stored_type_params = unbound_type,
            .encoding = literal_encoding::unbound_ascii,
            .validation = literal_validation::unvalidated,
        });
  }
  for (std::uint32_t width = leaf_count; width > 1u; width /= 2u) {
    for (std::uint32_t index = 0u; index < width; index += 2u) {
      level[index / 2u] = make_branch_node(
          *arena, level[index], level[index + 1u]);
    }
  }
  const piece_edge root = level[0];
  pfree(level);
  validate_tree_and_limits(root, unbound_type, total_length, false);
  return {
      .arena = arena,
      .root = root,
      .length = total_length,
      .arena_refcount_baseline = arena->refcount,
      .live_baseline = live_piece_tree_arenas,
  };
}

struct binding_output_audit {
  std::uint64_t length;
  std::uint32_t leaves;
  bool exact;
};

template<class Instance>
[[nodiscard]] inline binding_output_audit
audit_binding_output(piece_edge edge, std::uint8_t expected_rank) {
  if (!edge) return {.exact = true};
  if (edge.transform
      != bioseq::storage::sequence_transform::identity) {
    return {.exact = false};
  }
  if (edge.node->node_kind == piece_node::kind::branch) {
    const auto left = audit_binding_output<Instance>(
        edge.node->payload.branch.left, expected_rank);
    const auto right = audit_binding_output<Instance>(
        edge.node->payload.branch.right, expected_rank);
    return {
        .length = left.length + right.length,
        .leaves = left.leaves + right.leaves,
        .exact = left.exact && right.exact,
    };
  }
  if (edge.node->node_kind != piece_node::kind::literal) {
    return {.exact = false};
  }
  const auto& literal = edge.node->payload.literal;
  bool exact = literal.encoding == literal_encoding::packed
      && literal.validation == literal_validation::alphabet_validated;
  for (std::uint64_t position = literal.symbol_from;
       exact && position < literal.symbol_to; ++position) {
    exact = packed_literal_rank_at<Instance>(literal, position)
        == expected_rank;
  }
  return {
      .length = literal.symbol_to - literal.symbol_from,
      .leaves = 1u,
      .exact = exact,
  };
}

[[nodiscard]] inline bool run_binding_retirement_overflow_regression() {
  binding_retirement_fixture fixture = make_binding_retirement_fixture();
  const compact_sequence_stdc_type_params bound_type{
      .alphabet_code = pgseq_supported_alphabets::index_of<seq::dna4>(),
      .storage_unit_byte_width = 8u,
  };
  std::uint8_t expected_rank = UINT8_MAX;
  const bool rank_found = pgseq_rank_for_symbol(
      bound_type.alphabet_code, static_cast<unsigned char>('A'),
      expected_rank);

  auto *destination = make_piece_tree_arena();
  piece_edge_cleanup_ledger ledger{};
  tracked_piece_edge tracked = bind_literals_copy_error_atomic(
      *destination, fixture.root, bound_type, ledger);
  transfer_piece_edge_cleanup_root_noexcept(
      ledger, tracked.cleanup, tracked.edge);
  const bool ledger_empty = piece_edge_cleanup_ledger_empty_noexcept(ledger);
  validate_tree_and_limits(
      tracked.edge, bound_type, fixture.length, true);
  binding_output_audit output{};
  bioseq::pg_types::seq::dispatch_instance(
      bound_type.alphabet_code, bound_type.storage_unit_byte_width,
      [&output, tracked, expected_rank]<class Instance>() {
        output = audit_binding_output<Instance>(tracked.edge, expected_rank);
      });
  const std::uint32_t inline_capacity = static_cast<std::uint32_t>(
      destination->retired_join_roots.size());
  const std::uint32_t overflow_roots =
      destination->retired_join_root_count > inline_capacity
      ? destination->retired_join_root_count - inline_capacity : 0u;
  const std::uint32_t expected_blocks = overflow_roots == 0u ? 0u
      : (overflow_roots - 1u)
              / static_cast<std::uint32_t>(max_retired_join_roots)
          + 1u;
  const std::uint64_t component_bytes =
      static_cast<std::uint64_t>(destination->node_metadata_bytes)
      + destination->arena_dependency_metadata_bytes
      + destination->source_dependency_metadata_bytes
      + destination->literal_storage_bytes
      + destination->retired_join_root_metadata_bytes;
  const bool success = rank_found && ledger_empty && output.exact
      && output.length == fixture.length
      && output.leaves == tracked.edge.node->piece_count
      && fixture.length == 23'768'000u
      && destination->retired_join_rotation_count > 64u
      && destination->retired_join_rotation_count
          == destination->retired_join_root_count
      && destination->retired_join_root_overflow_block_count
          == expected_blocks
      && destination->retired_join_root_metadata_bytes
          == static_cast<Size>(expected_blocks)
              * MAXALIGN(sizeof(retired_join_root_block))
      && component_bytes == destination->accounted_bytes
      && destination->node_metadata_bytes
          == static_cast<Size>(destination->node_count)
              * MAXALIGN(sizeof(piece_node))
      && destination->arena_dependency_count == 0u
      && destination->source_dependency_count == 0u;
  release_edge(tracked.edge);
  release_arena(destination);

  auto *observation = static_cast<tracked_join_failure_observation *>(
      palloc0(sizeof(tracked_join_failure_observation)));
  MemoryContext build_context = AllocSetContextCreate(
      CurrentMemoryContext, "bioseq binding retirement failure",
      ALLOCSET_SMALL_SIZES);
  const MemoryContext saved_context = CurrentMemoryContext;
  bool volatile caught = false;
  bool volatile sqlstate_matched = false;
  observe_piece_tree_failure_noexcept(
      piece_tree_injected_failure_point::none);
  arm_piece_tree_failure_noexcept(
      piece_tree_injected_failure_point::
          join_retired_root_overflow_allocation);
  PG_TRY();
  {
    auto *guard = static_cast<tracked_join_failure_guard *>(
        MemoryContextAllocZero(
            build_context, sizeof(tracked_join_failure_guard)));
    guard->callback.func = tracked_join_failure_reset;
    guard->callback.arg = guard;
    guard->observation = observation;
    guard->armed = true;
    MemoryContextRegisterResetCallback(
        build_context, std::addressof(guard->callback));
    guard->destination_arena = make_piece_tree_arena();
    (void)bind_literals_copy_error_atomic(
        *guard->destination_arena, fixture.root, bound_type, guard->ledger);
    arm_piece_tree_failure_noexcept(
        piece_tree_injected_failure_point::none);
    MemoryContextDelete(build_context);
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(saved_context);
    arm_piece_tree_failure_noexcept(
        piece_tree_injected_failure_point::none);
    sqlstate_matched = consume_expected_error(
        ERRCODE_INTERNAL_ERROR, saved_context);
    MemoryContextDelete(build_context);
    caught = true;
  }
  PG_END_TRY();
  const bool unwind = caught && sqlstate_matched
      && observed_piece_tree_failure_noexcept()
          == piece_tree_injected_failure_point::
              join_retired_root_overflow_allocation
      && observation->callbacks == 1u
      && observation->armed_callbacks == 1u
      && observation->tokens_released != 0u
      && observation->destination_tokens_released == 1u
      && observation->ledger_drained
      && fixture.arena->refcount == fixture.arena_refcount_baseline
      && live_piece_tree_arenas == fixture.live_baseline;
  pfree(observation);
  release_edge(fixture.root);
  release_arena(fixture.arena);
  return success && unwind;
}

[[nodiscard]] inline bool tree_length_add_raises_program_limit(
    std::uint64_t left, std::uint64_t right) {
  const MemoryContext saved_context = CurrentMemoryContext;
  bool volatile matched = false;
  PG_TRY();
  {
    (void)checked_tree_length_add(left, right);
  }
  PG_CATCH();
  {
    matched = consume_expected_error(
        ERRCODE_PROGRAM_LIMIT_EXCEEDED, saved_context);
  }
  PG_END_TRY();
  return matched;
}

[[nodiscard]] inline bool run_tree_length_add_guardrail_matrix() {
  constexpr auto max_logical_length =
      static_cast<std::uint64_t>(INT64_MAX);
  return checked_tree_length_add(max_logical_length, 0u)
             == max_logical_length
      && checked_tree_length_add(max_logical_length - 1u, 1u)
             == max_logical_length
      && tree_length_add_raises_program_limit(max_logical_length, 1u)
      && tree_length_add_raises_program_limit(UINT64_MAX, 0u)
      && tree_length_add_raises_program_limit(0u, UINT64_MAX);
}

[[nodiscard]] inline bool arena_size_raises_program_limit(Size requested) {
  const MemoryContext saved_context = CurrentMemoryContext;
  bool volatile matched = false;
  PG_TRY();
  {
    (void)checked_arena_allocation_size(requested);
  }
  PG_CATCH();
  {
    matched = consume_expected_error(
        ERRCODE_PROGRAM_LIMIT_EXCEEDED, saved_context);
  }
  PG_END_TRY();
  return matched;
}

[[nodiscard]] inline bool arena_reservation_raises_program_limit(
    piece_tree_arena &arena, Size requested) {
  const MemoryContext saved_context = CurrentMemoryContext;
  bool volatile matched = false;
  PG_TRY();
  {
    reserve_arena_allocation(
        arena, requested, arena_allocation_kind::literal_storage);
  }
  PG_CATCH();
  {
    matched = consume_expected_error(
        ERRCODE_PROGRAM_LIMIT_EXCEEDED, saved_context);
  }
  PG_END_TRY();
  return matched;
}

[[nodiscard]] inline bool run_arena_alignment_guardrail_matrix() {
  const Size alignment = MAXALIGN(1u);
  if (alignment == 0u || alignment - 1u > MaxAllocSize) {
    return false;
  }
  const Size first_boundary = MaxAllocSize - (alignment - 1u);
  for (Size offset = 0u; offset < alignment; ++offset) {
    const Size requested = first_boundary + offset;
    if (offset == 0u) {
      if (checked_arena_allocation_size(requested) != requested) {
        return false;
      }
    } else {
      piece_tree_arena rejected_budget{};
      if (!arena_size_raises_program_limit(requested)
          || !arena_reservation_raises_program_limit(
              rejected_budget, requested)
          || rejected_budget.accounted_bytes != 0u
          || rejected_budget.literal_storage_bytes != 0u) {
        return false;
      }
    }
  }

  piece_tree_arena exact_budget{};
  reserve_arena_allocation(
      exact_budget, first_boundary,
      arena_allocation_kind::literal_storage);
  const bool budget_rejected = arena_reservation_raises_program_limit(
      exact_budget, 1u);
  return exact_budget.accounted_bytes == first_boundary
      && exact_budget.literal_storage_bytes == first_boundary
      && budget_rejected;
}

[[nodiscard]] inline bool corrupt_tree_length_raises_data_corrupted(
    bool oversized_left) {
  auto *arena = make_piece_tree_arena();
  auto *storage = allocate_piece_literal_storage(*arena, 1u);
  storage[0] = std::byte{0x41};
  const compact_sequence_stdc_type_params unbound_type{
      .alphabet_code = pgseq_unbound_alphabet_code,
      .storage_unit_byte_width = pgseq_unbound_width_tag,
  };
  const std::uint64_t left_length = oversized_left ? UINT64_MAX : 1u;
  const std::uint64_t right_length = oversized_left ? 1u : UINT64_MAX;
  const piece_edge left = make_literal_node(
      *arena, factory_failure_unbound_literal(
                  arena, storage, 0u, left_length));
  const piece_edge right = make_literal_node(
      *arena, factory_failure_unbound_literal(
                  arena, storage, 0u, right_length));
  auto *branch = allocate_piece_node(*arena);
  branch->node_kind = piece_node::kind::branch;
  branch->bp_length = 0u;
  branch->piece_count = 2u;
  branch->height = 2u;
  ::new (static_cast<void *>(std::addressof(branch->payload.branch)))
      branch_piece{left, right};
  const piece_edge root{
      .arena = arena,
      .node = branch,
      .transform = bioseq::storage::sequence_transform::identity,
  };

  const MemoryContext saved_context = CurrentMemoryContext;
  bool volatile matched = false;
  PG_TRY();
  {
    validate_tree_and_limits(root, unbound_type, 0u, false);
  }
  PG_CATCH();
  {
    matched = consume_expected_error(
        ERRCODE_DATA_CORRUPTED, saved_context);
  }
  PG_END_TRY();

  release_edge(left);
  release_edge(right);
  release_arena(arena);
  return matched;
}

[[nodiscard]] inline bool run_corrupt_tree_length_guardrail_matrix() {
  return corrupt_tree_length_raises_data_corrupted(true)
      && corrupt_tree_length_raises_data_corrupted(false);
}

[[nodiscard]] inline bool
trusted_join_dependency_mismatch_raises_data_corrupted() {
  auto *backing = make_piece_tree_arena();
  auto *wrong_backing = make_piece_tree_arena();
  auto *destination = make_piece_tree_arena();
  auto *storage = allocate_piece_literal_storage(*backing, 1u);
  storage[0] = std::byte{0x41};
  const compact_sequence_stdc_type_params unbound_type{
      .alphabet_code = pgseq_unbound_alphabet_code,
      .storage_unit_byte_width = pgseq_unbound_width_tag,
  };
  const piece_edge trusted = make_literal_node(
      *backing, factory_failure_unbound_literal(backing, storage, 0u, 1u));
  const piece_edge result = make_literal_node(
      *destination,
      factory_failure_unbound_literal(backing, storage, 0u, 1u));
  (void)validate_trusted_join_and_limits(
      *destination, result, trusted, 1u, {}, 0u,
      unbound_type, 1u, false);

  auto *dependency = destination->arena_dependencies;
  if (dependency == nullptr || dependency->child != backing) {
    release_edge(result);
    release_edge(trusted);
    release_arena(destination);
    release_arena(wrong_backing);
    release_arena(backing);
    return false;
  }
  dependency->child = wrong_backing;
  const MemoryContext saved_context = CurrentMemoryContext;
  bool volatile matched = false;
  PG_TRY();
  {
    (void)validate_trusted_join_and_limits(
        *destination, result, trusted, 1u, {}, 0u,
        unbound_type, 1u, false);
  }
  PG_CATCH();
  {
    matched = consume_expected_error(
        ERRCODE_DATA_CORRUPTED, saved_context);
  }
  PG_END_TRY();
  /* Restore the retained child before arena teardown; the deliberately wrong
   * pointer never owned a reference token. */
  dependency->child = backing;
  release_edge(result);
  release_edge(trusted);
  release_arena(destination);
  release_arena(wrong_backing);
  release_arena(backing);
  return matched;
}

[[nodiscard]] inline bool
trusted_join_shared_node_raises_data_corrupted() {
  auto *arena = make_piece_tree_arena();
  auto *storage = allocate_piece_literal_storage(*arena, 1u);
  storage[0] = std::byte{0x43};
  const compact_sequence_stdc_type_params unbound_type{
      .alphabet_code = pgseq_unbound_alphabet_code,
      .storage_unit_byte_width = pgseq_unbound_width_tag,
  };
  const literal_piece literal = factory_failure_unbound_literal(
      arena, storage, 0u, 1u);
  const piece_edge shared = make_literal_node(*arena, literal);
  const piece_edge unreachable = make_literal_node(*arena, literal);
  const piece_edge result = make_branch_node(
      *arena, retain_edge(shared), retain_edge(shared));

  const MemoryContext saved_context = CurrentMemoryContext;
  bool volatile matched = false;
  PG_TRY();
  {
    (void)validate_trusted_join_and_limits(
        *arena, result, shared, 1u, shared, 1u,
        unbound_type, 2u, false);
  }
  PG_CATCH();
  {
    matched = consume_expected_error(
        ERRCODE_DATA_CORRUPTED, saved_context);
  }
  PG_END_TRY();
  release_edge(result);
  release_edge(shared);
  release_edge(unreachable);
  release_arena(arena);
  return matched;
}

[[nodiscard]] inline bool
trusted_join_forged_retirement_raises_data_corrupted() {
  auto *arena = make_piece_tree_arena();
  auto *storage = allocate_piece_literal_storage(*arena, 2u);
  storage[0] = std::byte{0x47};
  storage[1] = std::byte{0x54};
  const compact_sequence_stdc_type_params unbound_type{
      .alphabet_code = pgseq_unbound_alphabet_code,
      .storage_unit_byte_width = pgseq_unbound_width_tag,
  };
  const piece_edge first = make_literal_node(
      *arena,
      factory_failure_unbound_literal(arena, storage, 0u, 1u));
  const piece_edge second = make_literal_node(
      *arena,
      factory_failure_unbound_literal(arena, storage, 1u, 2u));
  const piece_edge result = make_branch_node(*arena, first, second);

  /* A published node cannot be used as a retirement record to conceal
   * arbitrary allocation accounting. */
  arena->retired_join_roots[0] = result;
  arena->retired_join_root_count = 1u;
  arena->retired_join_rotation_count = 1u;
  const MemoryContext saved_context = CurrentMemoryContext;
  bool volatile matched = false;
  PG_TRY();
  {
    (void)validate_trusted_join_and_limits(
        *arena, result, result, 2u, {}, 0u,
        unbound_type, 2u, false);
  }
  PG_CATCH();
  {
    matched = consume_expected_error(
        ERRCODE_DATA_CORRUPTED, saved_context);
  }
  PG_END_TRY();
  arena->retired_join_roots[0] = {};
  arena->retired_join_root_count = 0u;
  arena->retired_join_rotation_count = 0u;

  const auto& published = result.node->payload.branch;
  const piece_edge retired = make_branch_node(
      *arena, retain_edge(published.left), retain_edge(published.right));
  arena->retired_join_roots[0] = retired;
  arena->retired_join_root_count = 1u;
  arena->retired_join_rotation_count = 1u;
  release_edge(retired);
  const auto valid_audit = validate_trusted_join_and_limits(
      *arena, result, result, 2u, {}, 0u,
      unbound_type, 2u, false);
  const bool valid_retirement = valid_audit.new_nodes_visited
      == arena->node_count;

  arena->retired_join_roots[1] = arena->retired_join_roots[0];
  arena->retired_join_root_count = 2u;
  arena->retired_join_rotation_count = 2u;
  bool volatile duplicate_matched = false;
  PG_TRY();
  {
    (void)validate_trusted_join_and_limits(
        *arena, result, result, 2u, {}, 0u,
        unbound_type, 2u, false);
  }
  PG_CATCH();
  {
    duplicate_matched = consume_expected_error(
        ERRCODE_DATA_CORRUPTED, saved_context);
  }
  PG_END_TRY();
  arena->retired_join_roots[0] = {};
  arena->retired_join_roots[1] = {};
  arena->retired_join_root_count = 0u;
  arena->retired_join_rotation_count = 0u;
  release_edge(result);
  release_arena(arena);
  return matched && valid_retirement && duplicate_matched;
}

[[nodiscard]] inline bool
trusted_join_source_mismatch_raises_data_corrupted() {
  factory_failure_source_state source_state{};
  factory_failure_source_state wrong_source_state{};
  const pg_sequence_source_handle source{
      .state = std::addressof(source_state),
      .ops = std::addressof(factory_failure_source_ops),
  };
  const pg_sequence_source_handle wrong_source{
      .state = std::addressof(wrong_source_state),
      .ops = std::addressof(factory_failure_source_ops),
  };
  const compact_sequence_stdc_type_params bound_type{
      .alphabet_code = pgseq_supported_alphabets::index_of<seq::dna4>(),
      .storage_unit_byte_width = 8u,
  };
  const bioseq::storage::seq_locator locator{
      .num_fragments = 1u,
      .total_bp_size = 1u,
      .source_epoch = 1u,
  };
  auto *arena = make_piece_tree_arena();
  const piece_edge result = make_source_node(
      *arena,
      source_piece{
          .source = source,
          .locator = locator,
          .stored_type_params = bound_type,
          .last_padding_letters = 0u,
          .from = 0u,
          .to = 1u,
      });
  (void)validate_trusted_join_and_limits(
      *arena, result, result, 1u, {}, 0u,
      bound_type, 1u, true);

  auto *dependency = arena->source_dependencies;
  if (dependency == nullptr
      || dependency->source.state != source.state
      || dependency->source.ops != source.ops) {
    release_edge(result);
    release_arena(arena);
    return false;
  }
  dependency->source = wrong_source;
  const MemoryContext saved_context = CurrentMemoryContext;
  bool volatile matched = false;
  PG_TRY();
  {
    (void)validate_trusted_join_and_limits(
        *arena, result, result, 1u, {}, 0u,
        bound_type, 1u, true);
  }
  PG_CATCH();
  {
    matched = consume_expected_error(
        ERRCODE_DATA_CORRUPTED, saved_context);
  }
  PG_END_TRY();
  dependency->source = source;
  release_edge(result);
  release_arena(arena);
  return matched && source_state.retain_calls == 1u
      && source_state.release_calls == 1u
      && wrong_source_state.retain_calls == 0u
      && wrong_source_state.release_calls == 0u;
}

[[nodiscard]] inline bool run_trusted_join_corruption_guardrail_matrix() {
  return trusted_join_dependency_mismatch_raises_data_corrupted()
      && trusted_join_shared_node_raises_data_corrupted()
      && trusted_join_forged_retirement_raises_data_corrupted()
      && trusted_join_source_mismatch_raises_data_corrupted();
}

enum class transformed_join_rotation_shape : std::uint8_t {
  left_single,
  left_double,
  right_single,
  right_double,
};

inline constexpr std::size_t rotation_regression_rank_capacity = 64u;

struct rotation_regression_emission {
  std::array<std::uint8_t, rotation_regression_rank_capacity> ranks{};
  std::size_t size{};
  bool valid{};
};

/** Transfer one owning edge token and clear the source variable. */
[[nodiscard]] inline piece_edge take_rotation_edge(piece_edge &edge) noexcept {
  return std::exchange(edge, {});
}

/** Concatenate two bounded self-test emissions without materializing a tree. */
[[nodiscard]] inline rotation_regression_emission
concat_rotation_emissions(rotation_regression_emission first,
                          rotation_regression_emission second) noexcept {
  rotation_regression_emission result{};
  if (!first.valid || !second.valid
      || first.size > result.ranks.size()
      || second.size > result.ranks.size() - first.size) {
    return result;
  }
  std::copy_n(first.ranks.begin(), first.size, result.ranks.begin());
  std::copy_n(second.ranks.begin(), second.size,
              result.ranks.begin() + first.size);
  result.size = first.size + second.size;
  result.valid = true;
  return result;
}

/** Emit a source-only regression tree through its real transform callback. */
[[nodiscard]] inline rotation_regression_emission
emit_rotation_regression_edge(piece_edge edge) {
  rotation_regression_emission result{};
  if (!edge) {
    result.valid = true;
    return result;
  }
  if (edge.node->node_kind == piece_node::kind::branch) {
    const logical_children_result children = logical_children(edge);
    return concat_rotation_emissions(
        emit_rotation_regression_edge(children.first),
        emit_rotation_regression_edge(children.second));
  }
  if (edge.node->node_kind != piece_node::kind::source
      || edge.node->bp_length > result.ranks.size()) {
    return result;
  }

  const source_piece &source = edge.node->payload.source;
  pg_sequence_read_session session{};
  result.size = static_cast<std::size_t>(edge.node->bp_length);
  const auto status = source.source.ops->read_ranks(
      source.source.state, session, source.locator,
      source.stored_type_params, edge.transform, source.from,
      std::span<std::uint8_t>{result.ranks.data(), result.size});
  result.valid = status == pg_sequence_source_read_status::ok;
  return result;
}

/** Compare the exact rank streams emitted by two bounded tree views. */
[[nodiscard]] inline bool same_rotation_emission(
    rotation_regression_emission const &left,
    rotation_regression_emission const &right) noexcept {
  return left.valid && right.valid && left.size == right.size
      && std::equal(left.ranks.begin(), left.ranks.begin() + left.size,
                    right.ranks.begin());
}

/** Make one non-coalescible three-rank source leaf for a rotation fixture. */
[[nodiscard]] inline piece_edge make_rotation_regression_leaf(
    piece_tree_arena &arena,
    pg_sequence_source_handle source,
    compact_sequence_stdc_type_params bound_type,
    bioseq::storage::seq_locator locator,
    std::uint64_t leaf_ordinal,
    bioseq::storage::sequence_transform transform) {
  constexpr std::uint64_t leaf_stride = 4u;
  constexpr std::uint64_t leaf_length = 3u;
  const std::uint64_t from = leaf_ordinal * leaf_stride;
  return make_source_node(
      arena,
      source_piece{
          .source = source,
          .locator = locator,
          .stored_type_params = bound_type,
          .last_padding_letters = 0u,
          .from = from,
          .to = from + leaf_length,
      },
      transform);
}

/**
 * Exercise one structurally distinct persistent AVL rotation.
 *
 * Inputs live in an older arena and changed spines in a newer arena. Exact
 * emitted ranks prove that logical child order and composed transforms survive
 * the rotation; counter and arena checks prove dependency ownership balances.
 */
[[nodiscard]] inline bool run_transformed_join_rotation_case(
    transformed_join_rotation_shape shape) {
  const std::uint64_t arena_baseline = live_piece_tree_arenas;
  factory_failure_source_state source_state{};
  constexpr std::array<std::uint8_t, 12u> source_patterns{
      0u, 0u, 1u,
      1u, 2u, 3u,
      2u, 1u, 0u,
      3u, 3u, 2u,
  };
  for (std::size_t leaf = 0u; leaf < 4u; ++leaf) {
    for (std::size_t symbol = 0u; symbol < 3u; ++symbol) {
      source_state.ranks[leaf * 4u + symbol] =
          source_patterns[leaf * 3u + symbol];
    }
  }
  source_state.rank_count = source_state.ranks.size();
  const pg_sequence_source_handle source{
      .state = std::addressof(source_state),
      .ops = std::addressof(factory_failure_source_ops),
  };
  const compact_sequence_stdc_type_params bound_type{
      .alphabet_code = pgseq_supported_alphabets::index_of<seq::dna4>(),
      .storage_unit_byte_width = 8u,
  };
  const bioseq::storage::seq_locator locator{
      .num_fragments = 1u,
      .total_bp_size = source_state.rank_count,
      .source_epoch = 1u,
  };

  auto *source_arena = make_piece_tree_arena();
  piece_edge first = make_rotation_regression_leaf(
      *source_arena, source, bound_type, locator, 0u,
      bioseq::storage::sequence_transform::identity);
  piece_edge second = make_rotation_regression_leaf(
      *source_arena, source, bound_type, locator, 1u,
      bioseq::storage::sequence_transform::reversed);
  piece_edge third = make_rotation_regression_leaf(
      *source_arena, source, bound_type, locator, 2u,
      bioseq::storage::sequence_transform::complemented);
  piece_edge fourth = make_rotation_regression_leaf(
      *source_arena, source, bound_type, locator, 3u,
      bioseq::storage::sequence_transform::reverse_complement);

  piece_edge heavy{};
  piece_edge light{};
  switch (shape) {
    case transformed_join_rotation_shape::left_single: {
      piece_edge inner = make_branch_node(
          *source_arena, take_rotation_edge(second),
          take_rotation_edge(third));
      heavy = make_branch_node(
          *source_arena, take_rotation_edge(first),
          take_rotation_edge(inner));
      heavy.transform =
          bioseq::storage::sequence_transform::reverse_complement;
      light = take_rotation_edge(fourth);
      break;
    }
    case transformed_join_rotation_shape::left_double: {
      piece_edge pivot = make_branch_node(
          *source_arena, take_rotation_edge(second),
          take_rotation_edge(third));
      pivot.transform = bioseq::storage::sequence_transform::reversed;
      heavy = make_branch_node(
          *source_arena, take_rotation_edge(first),
          take_rotation_edge(pivot));
      heavy.transform = bioseq::storage::sequence_transform::complemented;
      light = take_rotation_edge(fourth);
      break;
    }
    case transformed_join_rotation_shape::right_single: {
      piece_edge inner = make_branch_node(
          *source_arena, take_rotation_edge(third),
          take_rotation_edge(fourth));
      inner.transform = bioseq::storage::sequence_transform::complemented;
      heavy = make_branch_node(
          *source_arena, take_rotation_edge(inner),
          take_rotation_edge(second));
      heavy.transform = bioseq::storage::sequence_transform::reversed;
      light = take_rotation_edge(first);
      break;
    }
    case transformed_join_rotation_shape::right_double: {
      piece_edge pivot = make_branch_node(
          *source_arena, take_rotation_edge(second),
          take_rotation_edge(third));
      pivot.transform = bioseq::storage::sequence_transform::complemented;
      heavy = make_branch_node(
          *source_arena, take_rotation_edge(fourth),
          take_rotation_edge(pivot));
      heavy.transform =
          bioseq::storage::sequence_transform::reverse_complement;
      light = take_rotation_edge(first);
      break;
    }
  }

  const bool left_heavy =
      shape == transformed_join_rotation_shape::left_single
      || shape == transformed_join_rotation_shape::left_double;
  const rotation_regression_emission expected = left_heavy
      ? concat_rotation_emissions(
            emit_rotation_regression_edge(heavy),
            emit_rotation_regression_edge(light))
      : concat_rotation_emissions(
            emit_rotation_regression_edge(light),
            emit_rotation_regression_edge(heavy));

  auto *destination_arena = make_piece_tree_arena();
  tracked_piece_edge tracked_joined = left_heavy
      ? rebalance_joined_edges(
            *destination_arena, nullptr,
            {.edge = take_rotation_edge(heavy)},
            {.edge = take_rotation_edge(light)},
            retired_join_root_policy::fixed_trusted_join)
      : rebalance_joined_edges(
            *destination_arena, nullptr,
            {.edge = take_rotation_edge(light)},
            {.edge = take_rotation_edge(heavy)},
            retired_join_root_policy::fixed_trusted_join);
  piece_edge joined = tracked_joined.edge;
  validate_tree_and_limits(joined, bound_type, expected.size, true);
  const rotation_regression_emission actual =
      emit_rotation_regression_edge(joined);
  const logical_children_result joined_children = logical_children(joined);
  const bool balanced = joined.node->bp_length == 12u
      && joined.node->piece_count == 4u
      && joined.node->height == 3u
      && edge_height(joined_children.first) == 2u
      && edge_height(joined_children.second) == 2u;
  const bool content_matches = same_rotation_emission(expected, actual);

  release_edge(joined);
  release_arena(destination_arena);
  release_arena(source_arena);
  return balanced && content_matches && !source_state.invalid_read
      && source_state.retain_calls == 4u
      && source_state.release_calls == 4u
      && source_state.read_calls == 8u
      && live_piece_tree_arenas == arena_baseline;
}

/** Reproduce the transformed inward-heavy join that formerly recursed forever. */
[[nodiscard]] inline bool run_inward_transformed_join_regression() {
  const std::uint64_t arena_baseline = live_piece_tree_arenas;
  factory_failure_source_state source_state{};
  const pg_sequence_source_handle source{
      .state = std::addressof(source_state),
      .ops = std::addressof(factory_failure_source_ops),
  };
  const compact_sequence_stdc_type_params bound_type{
      .alphabet_code = pgseq_supported_alphabets::index_of<seq::dna4>(),
      .storage_unit_byte_width = 8u,
  };
  const bioseq::storage::seq_locator locator{
      .num_fragments = 1u,
      .total_bp_size = 1u,
      .source_epoch = 1u,
  };
  const source_piece leaf_value{
      .source = source,
      .locator = locator,
      .stored_type_params = bound_type,
      .last_padding_letters = 0u,
      .from = 0u,
      .to = 1u,
  };

  auto *arena = make_piece_tree_arena();
  piece_edge first = make_source_node(*arena, leaf_value);
  piece_edge second = make_source_node(*arena, leaf_value);
  piece_edge third = make_source_node(*arena, leaf_value);
  piece_edge fourth = make_source_node(*arena, leaf_value);
  piece_edge inner = make_branch_node(
      *arena, take_rotation_edge(third), take_rotation_edge(fourth));
  piece_edge asymmetric = make_branch_node(
      *arena, take_rotation_edge(second), take_rotation_edge(inner));
  asymmetric.transform = bioseq::storage::sequence_transform::reversed;

  piece_edge joined = join_edges(
      *arena, take_rotation_edge(first), take_rotation_edge(asymmetric));
  validate_tree_and_limits(joined, bound_type, 4u, true);
  const bool balanced = joined.node->bp_length == 4u
      && joined.node->piece_count == 4u
      && joined.node->height == 3u;
  release_edge(joined);
  release_arena(arena);
  return balanced
      && source_state.retain_calls == 4u
      && source_state.release_calls == 4u
      && live_piece_tree_arenas == arena_baseline;
}

/** Cover the original join and all four single/double rotation shapes. */
[[nodiscard]] inline bool run_transformed_join_rotation_regression() {
  return run_inward_transformed_join_regression()
      && run_transformed_join_rotation_case(
             transformed_join_rotation_shape::left_single)
      && run_transformed_join_rotation_case(
             transformed_join_rotation_shape::left_double)
      && run_transformed_join_rotation_case(
             transformed_join_rotation_shape::right_single)
      && run_transformed_join_rotation_case(
             transformed_join_rotation_shape::right_double);
}

} // namespace

/* Internal lifetime acceptance seam. It deliberately catches recoverable
 * construction errors and exercises cleanup-quarantine state on local POD
 * fixtures, without poisoning the backend-global production state. */
export namespace bioseq::pg_types::sql_routines {

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::execute,
    .from = bioseq::sql::public_role,
})]]
bool piece_tree_self_test() {
  bool volatile saturation_caught = false;
  bool volatile budget_caught = false;
  bool volatile generation_caught = false;
  bool volatile million_piece_balanced = false;
  bool volatile deep_release_iterative = false;
  const std::uint64_t arena_baseline = live_piece_tree_arenas;
  const auto quarantine_baseline = read_piece_tree_quarantine_noexcept();
  const bool clean_entry = arena_baseline == 0u
      && quarantine_baseline.invariant_failures == 0u;
  const bool factory_failure_matrix =
      run_piece_tree_factory_failure_matrix();
  const bool tracked_join_failure_matrix =
      run_tracked_join_failure_matrix();
  const bool error_atomic_matrix =
      run_piece_tree_error_atomic_matrix();
  const bool binding_retirement_overflow =
      run_binding_retirement_overflow_regression();
  const bool tree_length_guardrail_matrix =
      run_tree_length_add_guardrail_matrix();
  const bool arena_alignment_guardrail_matrix =
      run_arena_alignment_guardrail_matrix();
  const bool corrupt_tree_length_guardrail_matrix =
      run_corrupt_tree_length_guardrail_matrix();
  const bool trusted_join_corruption_guardrail_matrix =
      run_trusted_join_corruption_guardrail_matrix();
  const bool transformed_join_rotation =
      run_transformed_join_rotation_regression();

  auto *saturation_arena = make_piece_tree_arena();
  saturation_arena->refcount = UINT32_MAX;
  PG_TRY();
  {
    retain_arena(saturation_arena);
  }
  PG_CATCH();
  {
    FlushErrorState();
    saturation_caught = true;
  }
  PG_END_TRY();
  saturation_arena->refcount = 1u;
  release_arena(saturation_arena);

  auto *budget_arena = make_piece_tree_arena();
  budget_arena->accounted_bytes = MaxAllocSize;
  PG_TRY();
  {
    (void)allocate_piece_literal_storage(*budget_arena, 1u);
  }
  PG_CATCH();
  {
    FlushErrorState();
    budget_caught = true;
  }
  PG_END_TRY();
  budget_arena->accounted_bytes = 0u;
  release_arena(budget_arena);

  auto *older = make_piece_tree_arena();
  auto *newer = make_piece_tree_arena();
  auto *newer_bytes = allocate_piece_literal_storage(*newer, 1u);
  PG_TRY();
  {
    (void)make_literal_node(
        *older, literal_piece{
                    .storage_arena = newer,
                    .bytes = newer_bytes,
                    .symbol_from = 0u,
                    .symbol_to = 0u,
                    .stored_type_params =
                        {
                            .alphabet_code = pgseq_unbound_alphabet_code,
                            .storage_unit_byte_width = pgseq_unbound_width_tag,
                        },
                    .encoding = literal_encoding::unbound_ascii,
                    .validation = literal_validation::unvalidated,
                });
  }
  PG_CATCH();
  {
    FlushErrorState();
    generation_caught = true;
  }
  PG_END_TRY();
  release_arena(newer);
  release_arena(older);

  piece_tree_arena *volatile million_arena = nullptr;
  piece_node const *volatile million_root_node = nullptr;
  PG_TRY();
  {
    constexpr std::uint32_t piece_count = 1'000'000u;
    million_arena = make_piece_tree_arena();
    auto *storage = allocate_piece_literal_storage(
        *const_cast<piece_tree_arena *>(million_arena), 1u);
    storage[0] = std::byte{0x41};
    const literal_piece literal{
        .storage_arena = const_cast<piece_tree_arena *>(million_arena),
        .bytes = storage,
        .symbol_from = 0u,
        .symbol_to = 1u,
        .stored_type_params =
            {
                .alphabet_code = pgseq_unbound_alphabet_code,
                .storage_unit_byte_width = pgseq_unbound_width_tag,
            },
        .encoding = literal_encoding::unbound_ascii,
        .validation = literal_validation::unvalidated,
    };
    const piece_edge built = build_balanced_literal_probe(
        *const_cast<piece_tree_arena *>(million_arena), literal, piece_count);
    million_root_node = built.node;
    validate_tree_and_limits(built, literal.stored_type_params, piece_count,
                             false);
    million_piece_balanced = built.node->piece_count == piece_count &&
                             built.node->bp_length == piece_count &&
                             built.node->height == 21u;
    release_edge(built);
    million_root_node = nullptr;
    release_arena(const_cast<piece_tree_arena *>(million_arena));
    million_arena = nullptr;
  }
  PG_CATCH();
  {
    if (million_root_node != nullptr) {
      release_edge(piece_edge{
          .arena = const_cast<piece_tree_arena *>(million_arena),
          .node = million_root_node,
          .transform = bioseq::storage::sequence_transform::identity,
      });
      million_root_node = nullptr;
    }
    if (million_arena != nullptr) {
      auto *failed_arena = const_cast<piece_tree_arena *>(million_arena);
      /* Every probe edge is intra-arena.  ERROR may strand temporary
       * edge tokens in skipped C++ frames, but no external dependency;
       * collapse those tokens before deleting the failed arena. */
      failed_arena->refcount = 1u;
      release_arena(failed_arena);
      million_arena = nullptr;
    }
    FlushErrorState();
  }
  PG_END_TRY();

  piece_tree_arena *volatile chain_head = nullptr;
  piece_tree_arena *volatile chain_pending = nullptr;
  PG_TRY();
  {
    constexpr std::uint32_t dependency_depth = 10'000u;
    chain_head = make_piece_tree_arena();
    for (std::uint32_t depth = 1u; depth < dependency_depth; ++depth) {
      auto *previous = const_cast<piece_tree_arena *>(chain_head);
      auto *previous_storage = allocate_piece_literal_storage(*previous, 1u);
      previous_storage[0] = std::byte{0x43};
      auto *next = make_piece_tree_arena();
      chain_pending = next;
      piece_edge dependency = make_literal_node(
          *next, literal_piece{
                     .storage_arena = previous,
                     .bytes = previous_storage,
                     .symbol_from = 0u,
                     .symbol_to = 1u,
                     .stored_type_params =
                         {
                             .alphabet_code = pgseq_unbound_alphabet_code,
                             .storage_unit_byte_width = pgseq_unbound_width_tag,
                         },
                     .encoding = literal_encoding::unbound_ascii,
                     .validation = literal_validation::unvalidated,
                 });
      release_edge(dependency);
      release_arena(previous);
      chain_head = next;
      chain_pending = nullptr;
    }
    release_arena(const_cast<piece_tree_arena *>(chain_head));
    chain_head = nullptr;
    deep_release_iterative = live_piece_tree_arenas == arena_baseline;
  }
  PG_CATCH();
  {
    if (chain_pending != nullptr) {
      release_arena(const_cast<piece_tree_arena *>(chain_pending));
      chain_pending = nullptr;
    }
    if (chain_head != nullptr) {
      release_arena(const_cast<piece_tree_arena *>(chain_head));
      chain_head = nullptr;
    }
    FlushErrorState();
  }
  PG_END_TRY();

  const bool all_checks_passed =
      clean_entry && saturation_caught && budget_caught && generation_caught &&
      factory_failure_matrix && tracked_join_failure_matrix &&
      error_atomic_matrix &&
      binding_retirement_overflow &&
      tree_length_guardrail_matrix &&
      arena_alignment_guardrail_matrix &&
      corrupt_tree_length_guardrail_matrix &&
      trusted_join_corruption_guardrail_matrix &&
      transformed_join_rotation &&
      million_piece_balanced && deep_release_iterative &&
      live_piece_tree_arenas == arena_baseline &&
      read_piece_tree_quarantine_noexcept().invariant_failures
          == quarantine_baseline.invariant_failures;
  if (!all_checks_passed) {
    ereport(WARNING,
            (errmsg_internal(
                "bioseq piece-tree self-test flags: clean_entry=%d saturation=%d budget=%d generation=%d factory=%d tracked_join=%d error_atomic=%d binding_retirement=%d length=%d alignment=%d corrupt=%d trusted=%d transformed_join=%d million=%d deep=%d arena_balance=%d",
                clean_entry ? 1 : 0,
                saturation_caught ? 1 : 0,
                budget_caught ? 1 : 0,
                generation_caught ? 1 : 0,
                factory_failure_matrix ? 1 : 0,
                tracked_join_failure_matrix ? 1 : 0,
                error_atomic_matrix ? 1 : 0,
                binding_retirement_overflow ? 1 : 0,
                tree_length_guardrail_matrix ? 1 : 0,
                arena_alignment_guardrail_matrix ? 1 : 0,
                corrupt_tree_length_guardrail_matrix ? 1 : 0,
                trusted_join_corruption_guardrail_matrix ? 1 : 0,
                transformed_join_rotation ? 1 : 0,
                million_piece_balanced ? 1 : 0,
                deep_release_iterative ? 1 : 0,
                live_piece_tree_arenas == arena_baseline ? 1 : 0)));
  }
  return all_checks_passed;
}

}  // namespace bioseq::pg_types::sql_routines
