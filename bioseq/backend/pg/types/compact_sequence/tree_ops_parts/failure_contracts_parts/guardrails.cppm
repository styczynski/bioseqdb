/**
 * @file guardrails.cppm
 * @brief Length, arena, and trusted-join corruption guardrails.
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

export module bioseq.postgres.types.compact_sequence:tree_ops_failure_guardrails;

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

/// Return whether a length addition raises the program-limit SQLSTATE.
[[nodiscard]] inline bool tree_length_add_raises_program_limit(std::uint64_t left,
                                                               std::uint64_t right) {
    const MemoryContext saved_context = CurrentMemoryContext;
    bool volatile matched = false;
    PG_TRY();
    {
        (void)checked_tree_length_add(left, right);
    }
    PG_CATCH();
    {
        matched = consume_expected_error(ERRCODE_PROGRAM_LIMIT_EXCEEDED, saved_context);
    }
    PG_END_TRY();
    return matched;
}

} // namespace

/** Provide the SQL facade with this deterministic piece-tree contract. */
[[nodiscard]] inline bool run_tree_length_add_guardrail_matrix() {
    constexpr auto max_logical_length = static_cast<std::uint64_t>(INT64_MAX);
    return checked_tree_length_add(max_logical_length, 0u) == max_logical_length &&
           checked_tree_length_add(max_logical_length - 1u, 1u) == max_logical_length &&
           tree_length_add_raises_program_limit(max_logical_length, 1u) &&
           tree_length_add_raises_program_limit(UINT64_MAX, 0u) &&
           tree_length_add_raises_program_limit(0u, UINT64_MAX);
}

namespace {

/// Return whether arena size alignment rejects the requested bytes.
[[nodiscard]] inline bool arena_size_raises_program_limit(Size requested) {
    const MemoryContext saved_context = CurrentMemoryContext;
    bool volatile matched = false;
    PG_TRY();
    {
        (void)checked_arena_allocation_size(requested);
    }
    PG_CATCH();
    {
        matched = consume_expected_error(ERRCODE_PROGRAM_LIMIT_EXCEEDED, saved_context);
    }
    PG_END_TRY();
    return matched;
}

/// Return whether arena accounting rejects the requested bytes.
[[nodiscard]] inline bool arena_reservation_raises_program_limit(piece_tree_arena& arena,
                                                                 Size requested) {
    const MemoryContext saved_context = CurrentMemoryContext;
    bool volatile matched = false;
    PG_TRY();
    {
        reserve_arena_allocation(arena, requested, arena_allocation_kind::literal_storage);
    }
    PG_CATCH();
    {
        matched = consume_expected_error(ERRCODE_PROGRAM_LIMIT_EXCEEDED, saved_context);
    }
    PG_END_TRY();
    return matched;
}

} // namespace

/** Provide the SQL facade with this deterministic piece-tree contract. */
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
            if (!arena_size_raises_program_limit(requested) ||
                !arena_reservation_raises_program_limit(rejected_budget, requested) ||
                rejected_budget.accounted_bytes != 0u ||
                rejected_budget.literal_storage_bytes != 0u) {
                return false;
            }
        }
    }

    piece_tree_arena exact_budget{};
    reserve_arena_allocation(exact_budget, first_boundary, arena_allocation_kind::literal_storage);
    const bool budget_rejected = arena_reservation_raises_program_limit(exact_budget, 1u);
    return exact_budget.accounted_bytes == first_boundary &&
           exact_budget.literal_storage_bytes == first_boundary && budget_rejected;
}

namespace {

/// Return whether one corrupt branch length raises data corruption.
[[nodiscard]] inline bool corrupt_tree_length_raises_data_corrupted(bool oversized_left) {
    auto* arena = make_piece_tree_arena();
    auto* storage = allocate_piece_literal_storage(*arena, 1u);
    storage[0] = std::byte{0x41};
    const compact_sequence_stdc_type_params unbound_type{
        .alphabet_code = pgseq_unbound_alphabet_code,
        .storage_unit_byte_width = pgseq_unbound_width_tag,
    };
    const std::uint64_t left_length = oversized_left ? UINT64_MAX : 1u;
    const std::uint64_t right_length = oversized_left ? 1u : UINT64_MAX;
    const piece_edge left =
        make_literal_node(*arena, factory_failure_unbound_literal(arena, storage, 0u, left_length));
    const piece_edge right = make_literal_node(
        *arena, factory_failure_unbound_literal(arena, storage, 0u, right_length));
    auto* branch = allocate_piece_node(*arena);
    branch->node_kind = piece_node::kind::branch;
    branch->bp_length = 0u;
    branch->piece_count = 2u;
    branch->height = 2u;
    ::new (static_cast<void*>(std::addressof(branch->payload.branch))) branch_piece{left, right};
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
        matched = consume_expected_error(ERRCODE_DATA_CORRUPTED, saved_context);
    }
    PG_END_TRY();

    release_edge(left);
    release_edge(right);
    release_arena(arena);
    return matched;
}

} // namespace

/** Provide the SQL facade with this deterministic piece-tree contract. */
[[nodiscard]] inline bool run_corrupt_tree_length_guardrail_matrix() {
    return corrupt_tree_length_raises_data_corrupted(true) &&
           corrupt_tree_length_raises_data_corrupted(false);
}

namespace {

/// Reject a trusted join whose dependency arena does not match.
[[nodiscard]] inline bool trusted_join_dependency_mismatch_raises_data_corrupted() {
    auto* backing = make_piece_tree_arena();
    auto* wrong_backing = make_piece_tree_arena();
    auto* destination = make_piece_tree_arena();
    auto* storage = allocate_piece_literal_storage(*backing, 1u);
    storage[0] = std::byte{0x41};
    const compact_sequence_stdc_type_params unbound_type{
        .alphabet_code = pgseq_unbound_alphabet_code,
        .storage_unit_byte_width = pgseq_unbound_width_tag,
    };
    const piece_edge trusted =
        make_literal_node(*backing, factory_failure_unbound_literal(backing, storage, 0u, 1u));
    const piece_edge result =
        make_literal_node(*destination, factory_failure_unbound_literal(backing, storage, 0u, 1u));
    (void)validate_trusted_join_and_limits(
        *destination, result, trusted, 1u, {}, 0u, unbound_type, 1u, false);

    auto* dependency = destination->arena_dependencies;
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
            *destination, result, trusted, 1u, {}, 0u, unbound_type, 1u, false);
    }
    PG_CATCH();
    {
        matched = consume_expected_error(ERRCODE_DATA_CORRUPTED, saved_context);
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

/// Reject a trusted join that aliases one node as both operands.
[[nodiscard]] inline bool trusted_join_shared_node_raises_data_corrupted() {
    auto* arena = make_piece_tree_arena();
    auto* storage = allocate_piece_literal_storage(*arena, 1u);
    storage[0] = std::byte{0x43};
    const compact_sequence_stdc_type_params unbound_type{
        .alphabet_code = pgseq_unbound_alphabet_code,
        .storage_unit_byte_width = pgseq_unbound_width_tag,
    };
    const literal_piece literal = factory_failure_unbound_literal(arena, storage, 0u, 1u);
    const piece_edge shared = make_literal_node(*arena, literal);
    const piece_edge unreachable = make_literal_node(*arena, literal);
    const piece_edge result = make_branch_node(*arena, retain_edge(shared), retain_edge(shared));

    const MemoryContext saved_context = CurrentMemoryContext;
    bool volatile matched = false;
    PG_TRY();
    {
        (void)validate_trusted_join_and_limits(
            *arena, result, shared, 1u, shared, 1u, unbound_type, 2u, false);
    }
    PG_CATCH();
    {
        matched = consume_expected_error(ERRCODE_DATA_CORRUPTED, saved_context);
    }
    PG_END_TRY();
    release_edge(result);
    release_edge(shared);
    release_edge(unreachable);
    release_arena(arena);
    return matched;
}

/// Reject a trusted join with forged retirement metadata.
[[nodiscard]] inline bool trusted_join_forged_retirement_raises_data_corrupted() {
    auto* arena = make_piece_tree_arena();
    auto* storage = allocate_piece_literal_storage(*arena, 2u);
    storage[0] = std::byte{0x47};
    storage[1] = std::byte{0x54};
    const compact_sequence_stdc_type_params unbound_type{
        .alphabet_code = pgseq_unbound_alphabet_code,
        .storage_unit_byte_width = pgseq_unbound_width_tag,
    };
    const piece_edge first =
        make_literal_node(*arena, factory_failure_unbound_literal(arena, storage, 0u, 1u));
    const piece_edge second =
        make_literal_node(*arena, factory_failure_unbound_literal(arena, storage, 1u, 2u));
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
            *arena, result, result, 2u, {}, 0u, unbound_type, 2u, false);
    }
    PG_CATCH();
    {
        matched = consume_expected_error(ERRCODE_DATA_CORRUPTED, saved_context);
    }
    PG_END_TRY();
    arena->retired_join_roots[0] = {};
    arena->retired_join_root_count = 0u;
    arena->retired_join_rotation_count = 0u;

    const auto& published = result.node->payload.branch;
    const piece_edge retired =
        make_branch_node(*arena, retain_edge(published.left), retain_edge(published.right));
    arena->retired_join_roots[0] = retired;
    arena->retired_join_root_count = 1u;
    arena->retired_join_rotation_count = 1u;
    release_edge(retired);
    const auto valid_audit = validate_trusted_join_and_limits(
        *arena, result, result, 2u, {}, 0u, unbound_type, 2u, false);
    const bool valid_retirement = valid_audit.new_nodes_visited == arena->node_count;

    arena->retired_join_roots[1] = arena->retired_join_roots[0];
    arena->retired_join_root_count = 2u;
    arena->retired_join_rotation_count = 2u;
    bool volatile duplicate_matched = false;
    PG_TRY();
    {
        (void)validate_trusted_join_and_limits(
            *arena, result, result, 2u, {}, 0u, unbound_type, 2u, false);
    }
    PG_CATCH();
    {
        duplicate_matched = consume_expected_error(ERRCODE_DATA_CORRUPTED, saved_context);
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

/// Reject a trusted join whose source dependency identity changed.
[[nodiscard]] inline bool trusted_join_source_mismatch_raises_data_corrupted() {
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
    const bioseq::storage::sequence_locator locator{
        .fragment_count = 1u,
        .logical_length = 1u,
        .source_epoch = 1u,
    };
    auto* arena = make_piece_tree_arena();
    const piece_edge result = make_source_node(*arena,
                                               source_piece{
                                                   .source = source,
                                                   .locator = locator,
                                                   .stored_type_params = bound_type,
                                                   .last_padding_letters = 0u,
                                                   .from = 0u,
                                                   .to = 1u,
                                               });
    (void)validate_trusted_join_and_limits(
        *arena, result, result, 1u, {}, 0u, bound_type, 1u, true);

    auto* dependency = arena->source_dependencies;
    if (dependency == nullptr || dependency->source.state != source.state ||
        dependency->source.ops != source.ops) {
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
            *arena, result, result, 1u, {}, 0u, bound_type, 1u, true);
    }
    PG_CATCH();
    {
        matched = consume_expected_error(ERRCODE_DATA_CORRUPTED, saved_context);
    }
    PG_END_TRY();
    dependency->source = source;
    release_edge(result);
    release_arena(arena);
    return matched && source_state.retain_calls == 1u && source_state.release_calls == 1u &&
           wrong_source_state.retain_calls == 0u && wrong_source_state.release_calls == 0u;
}

} // namespace

/** Provide the SQL facade with this deterministic piece-tree contract. */
[[nodiscard]] inline bool run_trusted_join_corruption_guardrail_matrix() {
    return trusted_join_dependency_mismatch_raises_data_corrupted() &&
           trusted_join_shared_node_raises_data_corrupted() &&
           trusted_join_forged_retirement_raises_data_corrupted() &&
           trusted_join_source_mismatch_raises_data_corrupted();
}
