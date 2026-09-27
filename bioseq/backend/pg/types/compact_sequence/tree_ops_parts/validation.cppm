/**
 * @file validation.cppm
 * @brief Full and bounded validation for persistent piece trees.
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

export module bioseq.postgres.types.compact_sequence:tree_ops_validation;

import :tree_ops_ownership;
import :flat_abi;
import :source_abi;
import :tree_types;
import bioseq.sequences.rank_pack;
import bioseq.storage.coordinates;
import bioseq.storage.layout;

namespace {

/// Recomputed logical metadata for one validated subtree.
struct validated_tree_fields {
    std::uint64_t length;
    std::uint32_t pieces;
    std::uint16_t height;
};

/// Validate exact byte/category accounting for one arena.
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
        || arena.refcount == 0u ||
        arena.refcount == std::numeric_limits<std::uint32_t>::max()
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

/// Validate inline and overflow storage for retired rotation roots.
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
        if (rotations == std::numeric_limits<std::uint32_t>::max()) {
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

/// Recursively validate cached tree metadata and leaf bounds.
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
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
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
            || source.to > source.locator.logical_length
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

/// Validate full tree metadata, dependencies, alphabet binding, and logical length.
export inline void validate_tree_and_limits(
    piece_edge edge, compact_sequence_stdc_type_params result_type,
    std::uint64_t expected_length, bool alphabet_bound = true) {
  if (expected_length >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
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

/// Work summary for validation limited to a newly allocated join closure.
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

/// Required and matched multiplicity for one arena dependency.
struct bounded_join_arena_multiset_slot {
    piece_tree_arena* key{};
    std::uint32_t required{};
    std::uint32_t matched{};
};

/// Required and matched multiplicity for one source dependency.
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

/// Validate cached metadata for one edge in a bounded join closure.
inline void validate_bounded_edge_summary(
    piece_edge edge, bounded_join_validation_audit& audit,
    bool count_external)
{
    note_piece_tree_bounded_validation_step_noexcept();
    if (!edge || edge.arena == nullptr || edge.node == nullptr
        || edge.arena->generation == 0u || edge.arena->refcount == 0u
        || edge.arena->refcount == std::numeric_limits<std::uint32_t>::max()
        || std::to_underlying(edge.transform)
            > std::to_underlying(
                bioseq::storage::sequence_transform::reverse_complement)
        || edge.node->bp_length >
               static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
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
    if (count_external &&
        audit.external_edges_checked != std::numeric_limits<std::uint32_t>::max()) {
        ++audit.external_edges_checked;
    }
}

/// Validate source or literal metadata for one bounded join leaf.
inline void validate_bounded_join_leaf(
    piece_edge edge, compact_sequence_stdc_type_params result_type,
    bool alphabet_bound)
{
    const piece_node& node = *edge.node;
    if (node.node_kind == piece_node::kind::source) {
        const auto& source = node.payload.source;
        if (!alphabet_bound || !pg_sequence_source_handle_valid(source.source)
            || source.from > source.to
            || source.to > source.locator.logical_length
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
        || literal.storage_arena->refcount == std::numeric_limits<std::uint32_t>::max()
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

/**
 * @brief Validate a new join closure against two trusted immutable roots.
 *
 * Existing subtrees use cached summaries. Every node and dependency allocated
 * by the destination arena is visited exactly once.
 */
export [[nodiscard]] inline bounded_join_validation_audit
validate_trusted_join_and_limits(
    piece_tree_arena const& destination, piece_edge result,
    piece_edge trusted_left, std::uint64_t left_length,
    piece_edge trusted_right, std::uint64_t right_length,
    compact_sequence_stdc_type_params result_type,
    std::uint64_t expected_length, bool alphabet_bound)
{
    bounded_join_validation_audit audit{};
    if (left_length >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
        || right_length >
               static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
        || right_length >
               static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) -
                   left_length
        || left_length + right_length != expected_length
        || expected_length >
               static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
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
                if (slot.required == std::numeric_limits<std::uint32_t>::max()) {
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
                if (slot.required == std::numeric_limits<std::uint32_t>::max()) {
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
            || branch.right.node->bp_length >
                   static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) -
                       branch.left.node->bp_length
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
     * recorded closure; cached height descent proves that the bounded
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
                || branch.right.node->bp_length >
                       static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) -
                           branch.left.node->bp_length
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
            || dependency->child->refcount ==
                   std::numeric_limits<std::uint32_t>::max()) {
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

/// Publish a validated root and its exact type/length metadata into an expanded value.
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
