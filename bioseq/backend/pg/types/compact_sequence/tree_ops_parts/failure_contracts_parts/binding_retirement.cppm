/**
 * @file binding_retirement.cppm
 * @brief Binding-retirement overflow cleanup contracts.
 */
module;

#include <algorithm>
#include <array>
#include <bioseq/pg_compat.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <utility>

export module bioseq.postgres.types.compact_sequence:tree_ops_failure_binding_retirement;

import :tree_ops_ownership;
import :tree_ops_transforms;
import :tree_ops_validation;
import :flat_abi;
import :source_abi;
import :tree_types;
import bioseq.sequences.rank_pack;
import bioseq.storage.coordinates;
import bioseq.storage.layout;

import :tree_ops_failure_factory;
import :tree_ops_failure_tracked;

namespace {

/// Deep literal tree and baseline counters for retirement tests.
struct binding_retirement_fixture {
    piece_tree_arena* arena;
    piece_edge root;
    std::uint64_t length;
    std::uint32_t arena_refcount_baseline;
    std::uint64_t live_baseline;
};

/// Build the bounded binding-retirement fixture.
[[nodiscard]] inline binding_retirement_fixture make_binding_retirement_fixture() {
    constexpr std::uint32_t leaf_count = 4'096u;
    constexpr std::uint64_t long_length = 17'000u;
    constexpr std::uint64_t short_length = 3'000u;
    auto* arena = make_piece_tree_arena();
    auto* storage = allocate_piece_literal_storage(*arena, long_length);
    std::fill_n(storage, long_length, std::byte{0x41});
    auto* level = static_cast<piece_edge*>(palloc0(sizeof(piece_edge) * leaf_count));
    const compact_sequence_stdc_type_params unbound_type{
        .alphabet_code = pgseq_unbound_alphabet_code,
        .storage_unit_byte_width = pgseq_unbound_width_tag,
    };
    std::uint64_t total_length = 0u;
    for (std::uint32_t index = 0u; index < leaf_count; ++index) {
        const std::uint64_t length = index % 5u == 0u ? long_length : short_length;
        total_length += length;
        level[index] = make_literal_node(*arena,
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
            level[index / 2u] = make_branch_node(*arena, level[index], level[index + 1u]);
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

/// Exact length, rank, and binding counts observed in one tree.
struct binding_output_audit {
    std::uint64_t length;
    std::uint32_t leaves;
    bool exact;
};

template <class p_instance>
/// Inspect binding output without changing ownership.
[[nodiscard]] inline binding_output_audit audit_binding_output(piece_edge edge,
                                                               std::uint8_t expected_rank) {
    if (!edge)
        return {.exact = true};
    if (edge.transform != bioseq::storage::sequence_transform::identity) {
        return {.exact = false};
    }
    if (edge.node->node_kind == piece_node::kind::branch) {
        const auto left =
            audit_binding_output<p_instance>(edge.node->payload.branch.left, expected_rank);
        const auto right =
            audit_binding_output<p_instance>(edge.node->payload.branch.right, expected_rank);
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
    bool exact = literal.encoding == literal_encoding::packed &&
                 literal.validation == literal_validation::alphabet_validated;
    for (std::uint64_t position = literal.symbol_from; exact && position < literal.symbol_to;
         ++position) {
        exact = packed_literal_rank_at<p_instance>(literal, position) == expected_rank;
    }
    return {
        .length = literal.symbol_to - literal.symbol_from,
        .leaves = 1u,
        .exact = exact,
    };
}

} // namespace

/** Provide the SQL facade with this deterministic piece-tree contract. */
[[nodiscard]] inline bool run_binding_retirement_overflow_regression() {
    binding_retirement_fixture fixture = make_binding_retirement_fixture();
    const compact_sequence_stdc_type_params bound_type{
        .alphabet_code = pgseq_supported_alphabets::index_of<seq::dna4>(),
        .storage_unit_byte_width = 8u,
    };
    std::uint8_t expected_rank = UINT8_MAX;
    const bool rank_found = pgseq_rank_for_symbol(
        bound_type.alphabet_code, static_cast<unsigned char>('A'), expected_rank);

    auto* destination = make_piece_tree_arena();
    piece_edge_cleanup_ledger ledger{};
    tracked_piece_edge tracked =
        bind_literals_copy_error_atomic(*destination, fixture.root, bound_type, ledger);
    transfer_piece_edge_cleanup_root_noexcept(ledger, tracked.cleanup, tracked.edge);
    const bool ledger_empty = piece_edge_cleanup_ledger_empty_noexcept(ledger);
    validate_tree_and_limits(tracked.edge, bound_type, fixture.length, true);
    binding_output_audit output{};
    bioseq::pg_types::seq::dispatch_instance(bound_type.alphabet_code,
                                             bound_type.storage_unit_byte_width,
                                             [&output, tracked, expected_rank]<class p_instance>() {
                                                 output = audit_binding_output<p_instance>(
                                                     tracked.edge, expected_rank);
                                             });
    const std::uint32_t inline_capacity =
        static_cast<std::uint32_t>(destination->retired_join_roots.size());
    const std::uint32_t overflow_roots =
        destination->retired_join_root_count > inline_capacity
            ? destination->retired_join_root_count - inline_capacity
            : 0u;
    const std::uint32_t expected_blocks =
        overflow_roots == 0u
            ? 0u
            : (overflow_roots - 1u) / static_cast<std::uint32_t>(max_retired_join_roots) + 1u;
    const std::uint64_t component_bytes =
        static_cast<std::uint64_t>(destination->node_metadata_bytes) +
        destination->arena_dependency_metadata_bytes +
        destination->source_dependency_metadata_bytes + destination->literal_storage_bytes +
        destination->retired_join_root_metadata_bytes;
    const bool success =
        rank_found && ledger_empty && output.exact && output.length == fixture.length &&
        output.leaves == tracked.edge.node->piece_count && fixture.length == 23'768'000u &&
        destination->retired_join_rotation_count > 64u &&
        destination->retired_join_rotation_count == destination->retired_join_root_count &&
        destination->retired_join_root_overflow_block_count == expected_blocks &&
        destination->retired_join_root_metadata_bytes ==
            static_cast<Size>(expected_blocks) * MAXALIGN(sizeof(retired_join_root_block)) &&
        component_bytes == destination->accounted_bytes &&
        destination->node_metadata_bytes ==
            static_cast<Size>(destination->node_count) * MAXALIGN(sizeof(piece_node)) &&
        destination->arena_dependency_count == 0u && destination->source_dependency_count == 0u;
    release_edge(tracked.edge);
    release_arena(destination);

    auto* observation = static_cast<tracked_join_failure_observation*>(
        palloc0(sizeof(tracked_join_failure_observation)));
    MemoryContext build_context = AllocSetContextCreate(
        CurrentMemoryContext, "bioseq binding retirement failure", ALLOCSET_SMALL_SIZES);
    const MemoryContext saved_context = CurrentMemoryContext;
    bool volatile caught = false;
    bool volatile sqlstate_matched = false;
    observe_piece_tree_failure_noexcept(piece_tree_injected_failure_point::none);
    arm_piece_tree_failure_noexcept(
        piece_tree_injected_failure_point::join_retired_root_overflow_allocation);
    PG_TRY();
    {
        auto* guard = static_cast<tracked_join_failure_guard*>(
            MemoryContextAllocZero(build_context, sizeof(tracked_join_failure_guard)));
        guard->callback.func = tracked_join_failure_reset;
        guard->callback.arg = guard;
        guard->observation = observation;
        guard->armed = true;
        MemoryContextRegisterResetCallback(build_context, std::addressof(guard->callback));
        guard->destination_arena = make_piece_tree_arena();
        (void)bind_literals_copy_error_atomic(
            *guard->destination_arena, fixture.root, bound_type, guard->ledger);
        arm_piece_tree_failure_noexcept(piece_tree_injected_failure_point::none);
        MemoryContextDelete(build_context);
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        arm_piece_tree_failure_noexcept(piece_tree_injected_failure_point::none);
        sqlstate_matched = consume_expected_error(ERRCODE_INTERNAL_ERROR, saved_context);
        MemoryContextDelete(build_context);
        caught = true;
    }
    PG_END_TRY();
    const bool unwind =
        caught && sqlstate_matched &&
        observed_piece_tree_failure_noexcept() ==
            piece_tree_injected_failure_point::join_retired_root_overflow_allocation &&
        observation->callbacks == 1u && observation->armed_callbacks == 1u &&
        observation->tokens_released != 0u && observation->destination_tokens_released == 1u &&
        observation->ledger_drained && fixture.arena->refcount == fixture.arena_refcount_baseline &&
        live_piece_tree_arenas == fixture.live_baseline;
    pfree(observation);
    release_edge(fixture.root);
    release_arena(fixture.arena);
    return success && unwind;
}
