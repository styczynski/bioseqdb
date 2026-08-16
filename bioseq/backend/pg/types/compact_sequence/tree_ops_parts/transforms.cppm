/**
 * @file transforms.cppm
 * @brief Persistent piece-tree coalescing, joining, splitting, and binding.
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

export module bioseq.postgres.types.compact_sequence:tree_ops_transforms;

import :tree_ops_ownership;
import :flat_abi;
import :source_abi;
import :tree_types;
import bioseq.sequences.rank_pack;
import bioseq.storage.coordinates;
import bioseq.storage.layout;

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

} // namespace

/** Shared with the piece-tree failure-contract partition. */
template<class p_instance>
[[nodiscard]] inline std::uint8_t packed_literal_rank_at(
    literal_piece const& literal,
    std::uint64_t physical_position) noexcept
{
    using traits = typename compact_sequence_range<p_instance>::traits;
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

namespace {

/// Pack two short literal runs into one destination-owned literal leaf.
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
    const bool reversed = bioseq::storage::is_reversed(transform);
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
         &merged, combined_length, transform]<class p_instance>() {
            using traits = typename compact_sequence_range<p_instance>::traits;
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
                p_instance,
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
                    packed_literal_rank_at<p_instance>(
                        physical_first, position));
                if (status != bioseq::sequences::rank_pack_status::ok) {
                    literal_pack_failure(status);
                }
            }
            for (std::uint64_t position = physical_second.symbol_from;
                 position < physical_second.symbol_to; ++position) {
                const auto status = writer.append_rank(
                    packed_literal_rank_at<p_instance>(
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

/// Return a merged leaf only when source/literal ranges are physically contiguous.
[[nodiscard]] inline piece_edge try_coalesce_contiguous(
    piece_tree_arena& destination,
    piece_edge left,
    piece_edge right)
{
    if (!left || !right || left.transform != right.transform
        || left.node->node_kind != right.node->node_kind) {
        return {};
    }
    const bool reversed = bioseq::storage::is_reversed(left.transform);
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

/// Coalesce adjacent compatible leaves without changing their logical rank stream.
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

/** Select retirement accounting across transform partitions. */
enum class retired_join_root_policy : std::uint8_t {
    fixed_trusted_join,
    proportional_binding,
};

namespace {

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
} // namespace

/** Shared with the piece-tree failure-contract partition. */
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

namespace {

/// Join tracked operands by descending only the taller tree spine.
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

/// Join two owning edges into one balanced persistent tree.
export [[nodiscard]] inline piece_edge join_edges(
    piece_tree_arena& destination,
    piece_edge left,
    piece_edge right)
{
    return join_edges_impl(
        destination, {.edge = left}, {.edge = right}, nullptr,
        retired_join_root_policy::fixed_trusted_join).edge;
}

/// Join ledger-tracked operands and atomically transfer ownership to the result.
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

/// Retain and join two edges under cleanup-ledger ownership.
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

/// Retain an edge and adopt the new token into a cleanup ledger.
export [[nodiscard]] inline tracked_piece_edge retain_piece_edge_error_atomic(
    piece_edge_cleanup_ledger& ledger, piece_edge borrowed)
{
    return retain_tracked_edge(&ledger, borrowed);
}

/// Apply a logical transform to a retained edge under cleanup-ledger ownership.
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
    tracked.edge.transform = bioseq::storage::compose_sequence_transforms(
        transform, tracked.edge.transform);
    transition_piece_edge_cleanup_identity_noexcept(
        &ledger, before_transform, tracked);
    return tracked;
}

/// Split one leaf at a logical symbol offset.
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
    const bool reversed = bioseq::storage::is_reversed(edge.transform);
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

/// Split a tree at a logical symbol offset while preserving content order.
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

/// Build a ledger-owned tree view for the half-open logical interval.
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

/// Recursively bind literal ranks while reusing source leaves and bound subtrees.
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
        joined.edge.transform = bioseq::storage::compose_sequence_transforms(
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
         length, ascii, edge, ledger]<class p_instance>() {
            using traits = typename compact_sequence_range<p_instance>::traits;
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
                p_instance,
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

/// Copy and alphabet-bind every unbound literal in a tree.
export [[nodiscard]] inline piece_edge bind_literals_copy(
    piece_tree_arena& destination,
    piece_edge edge,
    compact_sequence_stdc_type_params result_type)
{
    return bind_literals_copy_impl(
        destination, edge, result_type, nullptr).edge;
}

/// Bind literals and adopt the result into the cleanup ledger before publication.
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
