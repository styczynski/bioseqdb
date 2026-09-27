/**
 * @file ownership.cppm
 * @brief Keep shared piece-tree nodes and sequence storage alive.
 *
 * Each arena owns a group of tree nodes and their literal bytes. References
 * from roots and  other arenas keep that storage alive. A cleanup ledger tracks
 * unfinished results so PostgreSQL errors can release them during context reset.
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


/* Importing files can each emit an inline tree factory Keep failure state
 * in one out-of-line function so the test and factory, see the same armed error. */
/// Return the armed and observed test failure points for this backend thread.
export [[nodiscard, gnu::noinline]] std::array<std::uint8_t, 2u>&
piece_tree_failure_storage_noexcept() noexcept
{
    static thread_local std::array<std::uint8_t, 2u> storage{};
    return storage;
}

/// Reasons cleanup may stop and keep a suspect arena allocated.
export enum class piece_tree_quarantine_reason : std::uint8_t {
    none,
    arena_refcount,
    arena_dependency,
    arena_live_counter,
    cleanup_ledger,
    retired_join_root,
};

/// Cleanup failure counts and details for  this backend thread.
export struct piece_tree_quarantine_snapshot {
    std::uint64_t invariant_failures;
    std::uint64_t quarantined_arenas;
    piece_tree_quarantine_reason first_reason;
    piece_tree_quarantine_reason last_reason;
    char const* first_detail;
    char const* last_detail;
    std::uint64_t last_arena_generation;
};

/* Inline release functions frod different importing files must record
 * cleanup failures in the same place. */
/// Return the shared cleanup failure record for this backend thread.
export [[nodiscard, gnu::noinline]] piece_tree_quarantine_snapshot&
piece_tree_quarantine_storage_noexcept() noexcept
{
    static thread_local piece_tree_quarantine_snapshot storage{};
    return storage;
}

/// Copy this backend thread's cleanup failure counts and recorded causes.
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
    if (arena->refcount == std::numeric_limits<std::uint32_t>::max()) return;
    arena->refcount = std::numeric_limits<std::uint32_t>::max();
    arena->release_next = nullptr;
    if (state.quarantined_arenas != std::numeric_limits<std::uint64_t>::max()) {
        ++state.quarantined_arenas;
    }
}


} // namespace

/**
 * Record a cleanup failure and mark the supplied arenas unsafe to release.
 * Their reference counts are saturated so later cleanup cannot free suspect storage.
 * This path records the problem without raising another error during cleanup.
 */
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
    if (state.invariant_failures != std::numeric_limits<std::uint64_t>::max()) {
        ++state.invariant_failures;
    }
    state.last_reason = reason;
    state.last_detail = detail;
    mark_piece_tree_arena_quarantined_noexcept(state, first_arena);
    mark_piece_tree_arena_quarantined_noexcept(state, second_arena);
}

/** Record  a cleanup failure in this backend's shared state. */
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


} // namespace

/** Places where the SQL self-test can interrupt tree construction with a PostgreSQL error. */
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

/** Select the next test failure point, or use `none` to disable injection. */
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

/** Record which injected failure occurred. Tests also use `none` to clear it. */
inline void observe_piece_tree_failure_noexcept(
    piece_tree_injected_failure_point point) noexcept
{
    piece_tree_failure_storage_noexcept()[1] =
        static_cast<std::uint8_t>(point);
}

/** Return the last recorded test failure point. */
[[nodiscard]] inline piece_tree_injected_failure_point
observed_piece_tree_failure_noexcept() noexcept
{
    return static_cast<piece_tree_injected_failure_point>(
        piece_tree_failure_storage_noexcept()[1]);
}

namespace {


} // namespace

/** Raise the armed test error once when construction reaches this point */
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

/** Return the node height, or zero for an empty edge. */
[[nodiscard]] inline std::uint16_t edge_height(piece_edge edge) noexcept {
  return edge ? edge.node->height : 0u;
}

/** Check that both valid handles identify  the same sequence source. */
[[nodiscard]] inline bool
same_source_handle(pg_sequence_source_handle left,
                   pg_sequence_source_handle right) noexcept {
  return pg_sequence_source_handle_valid(left) &&
         pg_sequence_source_handle_valid(right) &&
         left.ops->identity(left.state) == right.ops->identity(right.state);
}

/** Compare the complee locator records byte for byte. */
[[nodiscard]] inline bool
same_locator(bioseq::storage::sequence_locator const &left,
             bioseq::storage::sequence_locator const &right) noexcept {
  return std::memcmp(std::addressof(left), std::addressof(right),
                     sizeof(left)) == 0;
}

/* Count join and cleanup work so tests can check how it grows with tree size.
 * All importing files use the same backend-thread counters. */
/// Counts of join, validation, and cleanup steps used by tree tests.
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

/** Count one join step in test builds, stopping at the counter limit. */
inline void note_piece_tree_join_step_noexcept() noexcept
{
#ifdef BIOSEQ_TEST_INDEXES
    auto& value = piece_tree_join_work_storage_noexcept().join_steps;
    if (value != (std::numeric_limits<std::uint64_t>::max)()) ++value;
#endif
}

/** Count one trusted-join validation step in test builds. */
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

/** Count one retired-root operation in test builds. */
inline void note_piece_tree_retired_root_step_noexcept() noexcept
{
#ifdef BIOSEQ_TEST_INDEXES
    auto& value = piece_tree_join_work_storage_noexcept().retired_root_steps;
    if (value != (std::numeric_limits<std::uint64_t>::max)()) ++value;
#endif
}

/// Create, an empty arena in TopMemoryContext with one owned reference.
export [[nodiscard]] inline piece_tree_arena *make_piece_tree_arena() {
  MemoryContext saved = CurrentMemoryContext;
  piece_tree_arena *volatile arena = nullptr;
  PG_TRY();
  {
    MemoryContextSwitchTo(TopMemoryContext);
    arena = static_cast<piece_tree_arena *>(palloc0(sizeof(piece_tree_arena)));
    MemoryContextSwitchTo(saved);
    /* Shared trees can outlive any one  expanded sequence or tuple context.
     * Keep their storage under TopMemoryContext and free it when the last
     * arena reference is released. */
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
  if (arena->refcount == std::numeric_limits<std::uint32_t>::max()) {
    ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                    errmsg("bioseq piece arena reference count overflow")));
  }
  ++arena->refcount;
}

/**
 * Release an arena reference and free storage when its last reference is gone.
 *
 * Follow arena dependencies with a work list so a long chain does not exhaust
 * the C++ stack. Corrupt reference counts or dependencies stop cleanup and
 * leave the suspect arenas allocated for diagnosis
 */
export inline void release_arena(piece_tree_arena *arena) noexcept {
  if (arena == nullptr) {
    return;
  }
  if (arena->refcount == 0u ||
      arena->refcount == std::numeric_limits<std::uint32_t>::max()) {
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
        if (child->refcount == 0u ||
            child->refcount == std::numeric_limits<std::uint32_t>::max()) {
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
    // Count an arena as freud only when cleanup reaches pfree. An earlier
    // failure  must leave the live count unchanged for that arena.
    --live_piece_tree_arenas;
    pfree(current);
  }
}

/// Return an edge with a new reference that the caller must release.
export [[nodiscard]] inline piece_edge retain_edge(piece_edge edge)
{
    if (edge) {
        retain_arena(edge.arena);
    }
    return edge;
}

/// Release the arena reference owned by this edge.
export inline void release_edge(piece_edge edge) noexcept
{
    if (edge) {
        release_arena(edge.arena);
    }
}

/// Return whether the ledger reports no pending edges and has a consistent count summary.
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

/** Check that a cleanup handle has a slot and a nonzero generation. */
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
    // Leave owned edges in their slots. Mark the ledger unusable so later
    // code  cannot reuse it or report successful cleanup.
    ledger.initialized = false;
    ledger.next_unused = invalid_piece_edge_cleanup_slot;
    ledger.active_head = invalid_piece_edge_cleanup_slot;
    ledger.free_head = invalid_piece_edge_cleanup_slot;
}

/// Prepare a zero-initialized ledger to track unfinished edge references.
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


} // namespace

/** Check that the fixed cleanup ledger has room for the requested extra references. */
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


} // namespace

/**
 * Transfer an owned edge reference into a free cleanup slot.
 * The returned generation distinguishes this use of the slot from earlier uses.
 * Corrupt or full storage is marked unusable without raising a cleanup error.
 */
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
    if (slot.active || slot.generation == std::numeric_limits<std::uint32_t>::max()) {
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

/// Remove the expected edge from the ledger  after checking its, handle and links.
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


} // namespace

/** Add an edge eference and register it for cleanup when a ledger is supplied. */
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

/** Remove a tracked edge from its ledger and release its arena reference */
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


} // namespace

/** Replace consumed input references in the ledger with the factory's result reference. */
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

/** Update a tracked edge's transform while keeping the same node, arena, and cleanup slot. */
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

/// Release all pending edges, stopping if the ledger is corrupt.
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

/// Remove a finished root  from cleanup tracking so its caller can keep the reference.
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

/** Allocation categories used to account for an arena's storage. */
enum class arena_allocation_kind : std::uint8_t {
    node,
    arena_dependency,
    source_dependency,
    literal_storage,
    retired_join_root_metadata,
};

namespace {


} // namespace

/** Round an allocation to PostgreSQL alignment and reject sizes above MaxAllocSize. */
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


} // namespace

/** Charge an aligned allocation to the arena, rejecting totals above MaxAllocSize. */
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
            if (destination.node_count == std::numeric_limits<std::uint32_t>::max()) {
                ereport(ERROR,
                        (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                         errmsg("bioseq piece node count overflow")));
            }
            ++destination.node_count;
            break;
        case arena_allocation_kind::arena_dependency:
            destination.arena_dependency_metadata_bytes += aligned;
            if (destination.arena_dependency_count ==
                std::numeric_limits<std::uint32_t>::max()) {
                ereport(ERROR,
                        (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                         errmsg("bioseq arena dependency count overflow")));
            }
            ++destination.arena_dependency_count;
            break;
        case arena_allocation_kind::source_dependency:
            destination.source_dependency_metadata_bytes += aligned;
            if (destination.source_dependency_count ==
                std::numeric_limits<std::uint32_t>::max()) {
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
            if (destination.retired_join_root_overflow_block_count ==
                std::numeric_limits<std::uint32_t>::max()) {
                ereport(ERROR,
                        (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                         errmsg("bioseq retired join-root block count overflow")));
            }
            ++destination.retired_join_root_overflow_block_count;
            break;
    }
}

namespace {


} // namespace

/**
 * Keep another arena alive while this arena uses its nodes or literal bytes.
 * Dependencies must point to older arenas, which prevents reference cycles.
 * A dependency on the destination itself needs no extra reference.
 */
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
    if (child->refcount == std::numeric_limits<std::uint32_t>::max()) {
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


} // namespace

/** Keep a sequence source alive until the destination arena  is released. */
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


} // namespace

/** Allocate a zeroed tree node and includ it in the arena's memory budget. */
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

/** Add sequence lengths, rejecting any result that cannot fit in PostgreSQL bigint. */
[[nodiscard]] inline std::uint64_t checked_tree_length_add(
    std::uint64_t left,
    std::uint64_t right)
{
    constexpr auto max_logical_length =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
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

/// Allocate zeroed literal storage, and charge its aligned size to the arena.
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

/**
 * Build a branch and consume both input edge references on success.
 * The destination keeps any other arenas needed by the children alive If one
 * input is empty, return the other owned edge without allocating a branch.
 */
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
    /* Add the result's reference before releasing either input. If that
     * raises an  error, the cleanup ledger still owns both input references. */
    const piece_edge result = retain_edge(piece_edge{
        .arena = std::addressof(destination),
        .node = node,
        .transform = bioseq::storage::sequence_transform::identity,
    });
    release_edge(left);
    release_edge(right);
    return result;
}

/// Create an owned source leaf and keep its sequence source alive through the arena.
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

/// Create an owned literal leaf and keep any external storage arena alive.
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
 * Create a source leaf and register its owned edge for error cleanup.
 * Check ledger capacity before construction so recording the result needs no allocation.
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

/// Create a iteral leaf and register its owned edge for error cleanup.
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

/// Borrowed children  in the order seen after applying the parent transform.
export struct logical_children_result {
    piece_edge first;
    piece_edge second;
};

/**
 * Return borrowed branch children in sequence order.
 * Reversal swaps the children. Each child also receives the parent's reversal
 * and complement flags, combined with its own transform.
 */
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
