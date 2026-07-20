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
#include "fmgr_boundary.h"

export module bioseq.types.pg_compact_sequence:tree_ops;

import :flat_abi;
import :source_abi;
import :tree_types;
import bioseq.sequences.rank_pack;
import bioseq.storage.coordinates;

namespace {

std::uint64_t next_piece_tree_generation = 1u;
std::uint64_t live_piece_tree_arenas = 0u;

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
};

piece_tree_injected_failure_point armed_piece_tree_failure =
    piece_tree_injected_failure_point::none;
piece_tree_injected_failure_point observed_piece_tree_failure =
    piece_tree_injected_failure_point::none;

inline void inject_piece_tree_failure(
    piece_tree_injected_failure_point point)
{
    if (armed_piece_tree_failure != point) {
        return;
    }
    armed_piece_tree_failure = piece_tree_injected_failure_point::none;
    observed_piece_tree_failure = point;
    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("injected bioseq piece-tree factory failure"),
             errdetail("failure_point=%u",
                       static_cast<unsigned>(point))));
}

constexpr int postgres_panic_level = ERROR + 2;

[[noreturn]] inline void
arena_release_invariant_panic(char const *detail) noexcept {
  ereport(postgres_panic_level,
          (errmsg_internal("bioseq piece arena release invariant failed: %s",
                           detail)));
  pg_unreachable();
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
    arena_release_invariant_panic("invalid root refcount");
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
          arena_release_invariant_panic(
              "dependency is null, cyclic, or newer than its parent");
        }
        if (child->refcount == 0u || child->refcount == UINT32_MAX) {
          arena_release_invariant_panic("dependency refcount is invalid");
        }
        if (--child->refcount == 0u) {
          child->release_next = work;
          work = child;
        }
      }
    }
    MemoryContextDelete(current->payload_context);
    if (live_piece_tree_arenas == 0u) {
      arena_release_invariant_panic("live-arena counter underflow");
    }
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
    return static_cast<piece_node*>(
        MemoryContextAllocZero(destination.payload_context,
                               sizeof(piece_node)));
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
    return static_cast<std::byte*>(
        MemoryContextAllocZero(destination.payload_context, allocation));
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
    release_edge(left);
    release_edge(right);
    return retain_edge(piece_edge{
        .arena = std::addressof(destination),
        .node = node,
        .transform = bioseq::storage::sequence_transform::identity,
    });
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
    return retain_edge(piece_edge{
        .arena = std::addressof(destination),
        .node = node,
        .transform = transform,
    });
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
    pgseq_t::dispatch_instance(
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

/**
 * Rebuild two adjacent subtrees after one height-guided join descent.
 *
 * The caller has already joined the only new sequence boundary.  This helper
 * therefore preserves the remaining internal boundaries with branch nodes and
 * performs the one AVL single or double rotation that may be required.  Both
 * input edges are owning tokens and are consumed on every return path.
 */
[[nodiscard]] inline piece_edge rebalance_joined_edges(
    piece_tree_arena& destination,
    piece_edge left,
    piece_edge right)
{
    const std::uint16_t left_height = edge_height(left);
    const std::uint16_t right_height = edge_height(right);
    if (left_height <= right_height + 1u
        && right_height <= left_height + 1u) {
        return make_branch_node(destination, left, right);
    }

    if (left_height == right_height + 2u) {
        const logical_children_result children = logical_children(left);
        const std::uint16_t first_height = edge_height(children.first);
        const std::uint16_t second_height = edge_height(children.second);
        piece_edge result{};
        if (first_height >= second_height) {
            piece_edge rotated_right = make_branch_node(
                destination, retain_edge(children.second), right);
            result = make_branch_node(
                destination, retain_edge(children.first), rotated_right);
        } else {
            const logical_children_result pivot =
                logical_children(children.second);
            piece_edge rotated_left = make_branch_node(
                destination,
                retain_edge(children.first),
                retain_edge(pivot.first));
            piece_edge rotated_right = make_branch_node(
                destination, retain_edge(pivot.second), right);
            result = make_branch_node(
                destination, rotated_left, rotated_right);
        }
        release_edge(left);
        return result;
    }

    if (right_height == left_height + 2u) {
        const logical_children_result children = logical_children(right);
        const std::uint16_t first_height = edge_height(children.first);
        const std::uint16_t second_height = edge_height(children.second);
        piece_edge result{};
        if (second_height >= first_height) {
            piece_edge rotated_left = make_branch_node(
                destination, left, retain_edge(children.first));
            result = make_branch_node(
                destination, rotated_left, retain_edge(children.second));
        } else {
            const logical_children_result pivot =
                logical_children(children.first);
            piece_edge rotated_left = make_branch_node(
                destination, left, retain_edge(pivot.first));
            piece_edge rotated_right = make_branch_node(
                destination,
                retain_edge(pivot.second),
                retain_edge(children.second));
            result = make_branch_node(
                destination, rotated_left, rotated_right);
        }
        release_edge(right);
        return result;
    }

    release_edge(left);
    release_edge(right);
    ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq join encountered a non-AVL subtree"),
             errdetail("left_height=%u right_height=%u",
                       static_cast<unsigned>(left_height),
                       static_cast<unsigned>(right_height))));
    pg_unreachable();
}

} // namespace

export [[nodiscard]] inline piece_edge join_edges(
    piece_tree_arena& destination,
    piece_edge left,
    piece_edge right)
{
    if (left && left.node->bp_length == 0u) {
        release_edge(left);
        return right;
    }
    if (right && right.node->bp_length == 0u) {
        release_edge(right);
        return left;
    }
    if (!left) {
        return right;
    }
    if (!right) {
        return left;
    }
    const auto left_height = edge_height(left);
    const auto right_height = edge_height(right);
    if (left_height <= right_height + 1u
        && right_height <= left_height + 1u) {
        return coalesce_boundary(destination, left, right);
    }
    if (left_height > right_height + 1u) {
        const auto children = logical_children(left);
        piece_edge joined = join_edges(
            destination, retain_edge(children.second), right);
        piece_edge result = rebalance_joined_edges(
            destination, retain_edge(children.first), joined);
        release_edge(left);
        return result;
    }
    const auto children = logical_children(right);
    piece_edge joined = join_edges(
        destination, left, retain_edge(children.first));
    piece_edge result = rebalance_joined_edges(
        destination, joined, retain_edge(children.second));
    release_edge(right);
    return result;
}

export [[nodiscard]] inline split_result split_leaf(
    piece_tree_arena& destination,
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
        return {
            make_source_node(destination, prefix, edge.transform),
            make_source_node(destination, suffix, edge.transform),
        };
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
    return {
        make_literal_node(destination, prefix, edge.transform),
        make_literal_node(destination, suffix, edge.transform),
    };
}

export [[nodiscard]] inline split_result split_tree(
    piece_tree_arena& destination,
    piece_edge tree,
    std::uint64_t offset)
{
    if (!tree) {
        return {};
    }
    if (offset == 0u) {
        return {{}, retain_edge(tree)};
    }
    if (offset >= tree.node->bp_length) {
        return {retain_edge(tree), {}};
    }
    if (tree.node->node_kind != piece_node::kind::branch) {
        return split_leaf(destination, tree, offset);
    }
    const auto children = logical_children(tree);
    const auto first_length = children.first.node->bp_length;
    if (offset < first_length) {
        split_result parts = split_tree(destination, children.first, offset);
        return {
            parts.left,
            join_edges(destination, parts.right, retain_edge(children.second)),
        };
    }
    split_result parts = split_tree(
        destination, children.second, offset - first_length);
    return {
        join_edges(destination, retain_edge(children.first), parts.left),
        parts.right,
    };
}

export [[nodiscard]] inline piece_edge slice_tree(
    piece_tree_arena& destination,
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
    split_result at_end = split_tree(destination, tree, to);
    release_edge(at_end.right);
    split_result at_start = split_tree(destination, at_end.left, from);
    release_edge(at_end.left);
    release_edge(at_start.left);
    return at_start.right;
}

export [[nodiscard]] inline piece_edge bind_literals_copy(
    piece_tree_arena& destination,
    piece_edge edge,
    compact_sequence_stdc_type_params result_type)
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
        return retain_edge(edge);
    }
    if (edge.node->node_kind == piece_node::kind::branch) {
        const branch_piece& branch = edge.node->payload.branch;
        piece_edge left = bind_literals_copy(
            destination, branch.left, result_type);
        piece_edge right = bind_literals_copy(
            destination, branch.right, result_type);
        piece_edge joined = join_edges(destination, left, right);
        joined.transform = bioseq::storage::compose(
            edge.transform, joined.transform);
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
        return retain_edge(edge);
    }

    const auto length = literal.symbol_to - literal.symbol_from;
    const auto* ascii = reinterpret_cast<char const*>(literal.bytes)
        + literal.symbol_from;
    if (length > std::numeric_limits<pgseq_t::bp_size_t>::max()) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq literal is too long to bind")));
    }
    piece_edge bound_edge{};
    pgseq_t::dispatch_instance(
        result_type.alphabet_code,
        result_type.storage_unit_byte_width,
        [&destination, &bound_edge, result_type,
         length, ascii, edge]<class Instance>() {
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
            bound_edge = make_literal_node(
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
        });
    if (!bound_edge) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq literal alphabet dispatch produced no result")));
    }
    return bound_edge;
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
    const std::uint64_t component_total =
        static_cast<std::uint64_t>(arena.node_metadata_bytes)
        + arena.arena_dependency_metadata_bytes
        + arena.source_dependency_metadata_bytes
        + arena.literal_storage_bytes;
    if (arena.payload_context == nullptr || arena.generation == 0u
        || arena.refcount == 0u || arena.refcount == UINT32_MAX
        || arena.accounted_bytes > MaxAllocSize
        || arena.node_metadata_bytes != expected_nodes
        || arena.arena_dependency_metadata_bytes
            != expected_arena_dependencies
        || arena.source_dependency_metadata_bytes
            != expected_source_dependencies
        || component_total != arena.accounted_bytes) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq piece arena accounting is corrupt"),
                 errdetail("generation=%llu refcount=%u accounted_bytes=%zu "
                           "node_count=%u arena_dependencies=%u "
                           "source_dependencies=%u",
                           static_cast<unsigned long long>(arena.generation),
                           arena.refcount,
                           static_cast<std::size_t>(arena.accounted_bytes),
                           arena.node_count,
                           arena.arena_dependency_count,
                           arena.source_dependency_count)));
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
  if (fields.length != expected_length || fields.pieces > max_piece_count) {
    ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                    errmsg("bioseq piece-tree cached length/count mismatch")));
  }
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
  observed_piece_tree_failure = piece_tree_injected_failure_point::none;
  armed_piece_tree_failure = failure_point;

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

    armed_piece_tree_failure = piece_tree_injected_failure_point::none;
    MemoryContextDelete(build_context);
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(saved_context);
    armed_piece_tree_failure = piece_tree_injected_failure_point::none;
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
      && observed_piece_tree_failure == failure_point
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
                static_cast<unsigned>(observed_piece_tree_failure),
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
  piece_edge joined = left_heavy
      ? rebalance_joined_edges(
            *destination_arena, take_rotation_edge(heavy),
            take_rotation_edge(light))
      : rebalance_joined_edges(
            *destination_arena, take_rotation_edge(light),
            take_rotation_edge(heavy));
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
 * construction errors; impossible release underflow remains a PANIC and is
 * therefore covered by static invariants rather than executed in-process. */
export inline Datum piece_tree_self_test_impl(FunctionCallInfo) {
  bool volatile saturation_caught = false;
  bool volatile budget_caught = false;
  bool volatile generation_caught = false;
  bool volatile million_piece_balanced = false;
  bool volatile deep_release_iterative = false;
  const std::uint64_t arena_baseline = live_piece_tree_arenas;
  const bool factory_failure_matrix =
      run_piece_tree_factory_failure_matrix();
  const bool tree_length_guardrail_matrix =
      run_tree_length_add_guardrail_matrix();
  const bool arena_alignment_guardrail_matrix =
      run_arena_alignment_guardrail_matrix();
  const bool corrupt_tree_length_guardrail_matrix =
      run_corrupt_tree_length_guardrail_matrix();
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
      saturation_caught && budget_caught && generation_caught &&
      factory_failure_matrix && tree_length_guardrail_matrix &&
      arena_alignment_guardrail_matrix &&
      corrupt_tree_length_guardrail_matrix &&
      transformed_join_rotation &&
      million_piece_balanced && deep_release_iterative &&
      live_piece_tree_arenas == arena_baseline;
  if (!all_checks_passed) {
    ereport(WARNING,
            (errmsg_internal(
                "bioseq piece-tree self-test flags: saturation=%d budget=%d generation=%d factory=%d length=%d alignment=%d corrupt=%d transformed_join=%d million=%d deep=%d arena_balance=%d",
                saturation_caught ? 1 : 0,
                budget_caught ? 1 : 0,
                generation_caught ? 1 : 0,
                factory_failure_matrix ? 1 : 0,
                tree_length_guardrail_matrix ? 1 : 0,
                arena_alignment_guardrail_matrix ? 1 : 0,
                corrupt_tree_length_guardrail_matrix ? 1 : 0,
                transformed_join_rotation ? 1 : 0,
                million_piece_balanced ? 1 : 0,
                deep_release_iterative ? 1 : 0,
                live_piece_tree_arenas == arena_baseline ? 1 : 0)));
  }
  PG_RETURN_BOOL(all_checks_passed);
}

extern "C" {
CXX_PG_FUNCTION_INFO_V1(piece_tree_self_test);
PGDLLEXPORT Datum piece_tree_self_test(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&piece_tree_self_test_impl, fcinfo);
}
} // extern "C"
