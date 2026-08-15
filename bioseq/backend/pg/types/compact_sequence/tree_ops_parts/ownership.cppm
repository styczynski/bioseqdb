/**
 * @file ownership.cppm
 * @brief Piece-tree arena, dependency, and edge ownership operations.
 */
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

#include <bioseq/pg_compat.h>

export module bioseq.postgres.types.compact_sequence:tree_ops_ownership;

import :flat_abi;
import :source_abi;
import :tree_types;
import bioseq.sequences.rank_pack;
import bioseq.storage.coordinates;
import bioseq.storage.layout;


/* Exported inline tree factories may be emitted from any importing TU at
 * dna4_uint64 profile. Keep the fault-injection seam in one out-of-line
 * module-owned object so the self-test and the selected factory body cannot
 * arm and observe different anonymous-namespace copies. */
/// Return the backend-thread failure-injection state shared by tree factories.
export [[nodiscard, gnu::noinline]] std::array<std::uint8_t, 2u>&
piece_tree_failure_storage_noexcept() noexcept
{
    static thread_local std::array<std::uint8_t, 2u> storage{};
    return storage;
}

/// First invariant category that forced a piece-tree arena quarantine.
export enum class piece_tree_quarantine_reason : std::uint8_t {
    none,
    arena_refcount,
    arena_dependency,
    arena_live_counter,
    cleanup_ledger,
    retired_join_root,
};

/// Backend-local diagnostic state for no-throw piece-tree cleanup failures.
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
/// Return the backend-thread quarantine state shared by tree release paths.
export [[nodiscard, gnu::noinline]] piece_tree_quarantine_snapshot&
piece_tree_quarantine_storage_noexcept() noexcept
{
    static thread_local piece_tree_quarantine_snapshot storage{};
    return storage;
}

/// Read the current backend-thread quarantine counters and first/last causes.
export [[nodiscard]] inline piece_tree_quarantine_snapshot
read_piece_tree_quarantine_noexcept() noexcept
{
    return piece_tree_quarantine_storage_noexcept();
}

namespace {

std::uint64_t next_piece_tree_generation = 1u;
} // namespace

/** Count live arenas across tree-operation partitions. */
std::uint64_t live_piece_tree_arenas = 0u;

namespace {

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

/// Record one invariant failure and quarantine every implicated arena.
} // namespace

/** Shared with another tree-operations partition. */
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

/** Shared with another tree-operations partition. */
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

namespace {

/* Backend-local acceptance seam.  Production calls leave this disarmed; the
 * internal SQL self-test arms one dependency-registration point at a time so
 * PostgreSQL ERROR can interrupt a factory at the only point where a returned
 * edge has not yet made the partial retain visible to its caller. */
} // namespace

/** Identify a state used across tree-operation partitions. */
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

/** Shared with another tree-operations partition. */
inline void arm_piece_tree_failure_noexcept(
    piece_tree_injected_failure_point point) noexcept
{
    piece_tree_failure_storage_noexcept()[0] =
        static_cast<std::uint8_t>(point);
}

namespace {

[[nodiscard]] inline piece_tree_injected_failure_point
armed_piece_tree_failure_noexcept() noexcept
{
    return static_cast<piece_tree_injected_failure_point>(
        piece_tree_failure_storage_noexcept()[0]);
}

} // namespace

/** Shared with another tree-operations partition. */
inline void observe_piece_tree_failure_noexcept(
    piece_tree_injected_failure_point point) noexcept
{
    piece_tree_failure_storage_noexcept()[1] =
        static_cast<std::uint8_t>(point);
}

/** Shared with another tree-operations partition. */
[[nodiscard]] inline piece_tree_injected_failure_point
observed_piece_tree_failure_noexcept() noexcept
{
    return static_cast<piece_tree_injected_failure_point>(
        piece_tree_failure_storage_noexcept()[1]);
}

namespace {

/// Raise the armed deterministic factory error at its matching transition.
} // namespace

/** Shared with another tree-operations partition. */
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

/** Shared with another tree-operations partition. */
[[nodiscard]] inline std::uint16_t edge_height(piece_edge edge) noexcept {
  return edge ? edge.node->height : 0u;
}

/** Shared with another tree-operations partition. */
[[nodiscard]] inline bool
same_source_handle(pg_sequence_source_handle left,
                   pg_sequence_source_handle right) noexcept {
  return pg_sequence_source_handle_valid(left) &&
         pg_sequence_source_handle_valid(right) &&
         left.ops->identity(left.state) == right.ops->identity(right.state);
}

/** Shared with another tree-operations partition. */
[[nodiscard]] inline bool
same_locator(bioseq::storage::sequence_locator const &left,
             bioseq::storage::sequence_locator const &right) noexcept {
  return std::memcmp(std::addressof(left), std::addressof(right),
                     sizeof(left)) == 0;
}

/* Deterministic complexity evidence for the production persistent-join path.
 * The storage accessor is out of line so exported inline tree operations from
 * different importers all report into one backend-thread instance. */
/// Bounded work counters for joining and releasing persistent tree edges.
export struct piece_tree_join_work_snapshot {
    std::uint64_t join_steps;
    std::uint64_t bounded_validation_steps;
    std::uint64_t cleanup_ledger_steps;
    std::uint64_t retired_root_steps;
};

/// Return the backend-thread work counters shared by inline tree operations.
export [[nodiscard, gnu::noinline]] piece_tree_join_work_snapshot&
piece_tree_join_work_storage_noexcept() noexcept
{
    static thread_local piece_tree_join_work_snapshot storage{};
    return storage;
}

/// Reset backend-thread join and cleanup work counters.
export inline void reset_piece_tree_join_work_noexcept() noexcept
{
    piece_tree_join_work_storage_noexcept() = {};
}

/// Read backend-thread join and cleanup work counters.
export [[nodiscard]] inline piece_tree_join_work_snapshot
read_piece_tree_join_work_noexcept() noexcept
{
    return piece_tree_join_work_storage_noexcept();
}

/** Shared with another tree-operations partition. */
inline void note_piece_tree_join_step_noexcept() noexcept
{
#ifdef BIOSEQ_TEST_INDEXES
    auto& value = piece_tree_join_work_storage_noexcept().join_steps;
    if (value != (std::numeric_limits<std::uint64_t>::max)()) ++value;
#endif
}

/** Shared with another tree-operations partition. */
inline void note_piece_tree_bounded_validation_step_noexcept() noexcept
{
#ifdef BIOSEQ_TEST_INDEXES
    auto& value = piece_tree_join_work_storage_noexcept().bounded_validation_steps;
    if (value != (std::numeric_limits<std::uint64_t>::max)()) ++value;
#endif
}

namespace {

inline void note_piece_tree_cleanup_ledger_step_noexcept() noexcept
{
#ifdef BIOSEQ_TEST_INDEXES
    auto& value = piece_tree_join_work_storage_noexcept().cleanup_ledger_steps;
    if (value != (std::numeric_limits<std::uint64_t>::max)()) ++value;
#endif
}

} // namespace

/** Shared with another tree-operations partition. */
inline void note_piece_tree_retired_root_step_noexcept() noexcept
{
#ifdef BIOSEQ_TEST_INDEXES
    auto& value = piece_tree_join_work_storage_noexcept().retired_root_steps;
    if (value != (std::numeric_limits<std::uint64_t>::max)()) ++value;
#endif
}

/// Allocate an empty arena in the current PostgreSQL memory context.
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

/// Add one owning reference to a live arena.
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

/// Release one arena reference and iteratively destroy newly unreachable dependencies.
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

/// Return an owning copy of an edge token.
export [[nodiscard]] inline piece_edge retain_edge(piece_edge edge)
{
    if (edge) {
        retain_arena(edge.arena);
    }
    return edge;
}

/// Release an owning edge token.
export inline void release_edge(piece_edge edge) noexcept
{
    if (edge) {
        release_arena(edge.arena);
    }
}

/// Report whether a cleanup ledger holds no pending edge token.
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

} // namespace

/** Shared with another tree-operations partition. */
[[nodiscard]] inline bool piece_edge_cleanup_handle_valid_noexcept(
    piece_edge_cleanup_handle handle) noexcept
{
    return handle.slot != invalid_piece_edge_cleanup_slot
        && handle.generation != 0u;
}

namespace {

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

/// Initialize inline cleanup storage for an empty operation ledger.
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

/// Ensure that a cleanup ledger can adopt the requested number of tokens.
} // namespace

/** Shared with another tree-operations partition. */
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

namespace {

/// Adopt one owning edge token into a preallocated cleanup slot.
} // namespace

/** Shared with another tree-operations partition. */
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

namespace {

struct detached_piece_edge {
    piece_edge token;
    bool detached;
};

/// Remove one validated token from a cleanup ledger without releasing it.
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

/// Retain an edge and return its ledger handle as one tracked value.
} // namespace

/** Shared with another tree-operations partition. */
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

/** Shared with another tree-operations partition. */
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

namespace {

/// Replace two consumed ledger tokens with the single factory result token.
} // namespace

/** Shared with another tree-operations partition. */
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

/** Shared with another tree-operations partition. */
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

/// Release every edge token retained by a cleanup ledger.
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

/// Transfer one ledger-owned root into a destination edge.
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

/// Return the backend-thread count of live piece-tree arenas.
export [[nodiscard]] inline std::uint64_t
piece_tree_live_arena_count_noexcept() noexcept
{
    return live_piece_tree_arenas;
}

/** Identify a state used across tree-operation partitions. */
enum class arena_allocation_kind : std::uint8_t {
    node,
    arena_dependency,
    source_dependency,
    literal_storage,
    retired_join_root_metadata,
};

namespace {

/// Validate one arena allocation against PostgreSQL's allocation limit.
} // namespace

/** Shared with another tree-operations partition. */
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

namespace {

/// Charge one aligned allocation to the arena's matching accounting category.
} // namespace

/** Shared with another tree-operations partition. */
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

namespace {

/// Retain and record one older arena required by a newly allocated node.
} // namespace

/** Shared with another tree-operations partition. */
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

namespace {

/// Retain and record one source handle required by a newly allocated leaf.
} // namespace

/** Shared with another tree-operations partition. */
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

namespace {

/// Allocate zeroed node metadata from an arena's payload context.
} // namespace

/** Shared with another tree-operations partition. */
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

/** Shared with another tree-operations partition. */
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

namespace {

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

/// Allocate literal bytes from an arena and charge its exact aligned footprint.
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

/// Create a branch whose arena retains both child edges.
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

/// Create a source leaf and retain its backing-source dependency.
export [[nodiscard]] inline piece_edge make_source_node(
    piece_tree_arena& destination,
    source_piece piece,
    bioseq::storage::sequence_transform transform =
        bioseq::storage::sequence_transform::identity)
{
    if (piece.from > piece.to
        || piece.to > piece.locator.logical_length) {
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

/// Create a literal leaf and retain any external storage arena.
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

/**
 * @brief Create a source leaf whose returned edge is already owned by the cleanup ledger.
 *
 * Cleanup capacity is reserved before entering the leaf factory, so an error
 * cannot expose an owning edge outside the ledger.
 */
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

/// Create a literal leaf whose returned edge is already owned by the cleanup ledger.
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

/// Logical left/right children after applying an edge transform.
export struct logical_children_result {
    piece_edge first;
    piece_edge second;
};

/// Return branch children in logical order with the parent transform composed.
export [[nodiscard]] inline logical_children_result logical_children(
    piece_edge edge)
{
    if (!edge || edge.node->node_kind != piece_node::kind::branch) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq logical_children requires a branch")));
    }
    auto apply = [transform = edge.transform](piece_edge child) {
        child.transform = bioseq::storage::compose_sequence_transforms(
            transform, child.transform);
        return child;
    };
    if (bioseq::storage::is_reversed(edge.transform)) {
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
