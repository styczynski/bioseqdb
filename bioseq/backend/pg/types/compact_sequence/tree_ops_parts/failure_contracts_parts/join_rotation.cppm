/**
 * @file join_rotation.cppm
 * @brief Transformed join rotation regression contracts.
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

export module bioseq.postgres.types.compact_sequence:tree_ops_failure_join_rotation;

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

namespace {

/// Single or double rotation exercised by a transformed join.
enum class transformed_join_rotation_shape : std::uint8_t {
    left_single,
    left_double,
    right_single,
    right_double,
};

inline constexpr std::size_t rotation_regression_rank_capacity = 64u;

/// Bounded rank stream emitted by one rotation fixture.
struct rotation_regression_emission {
    std::array<std::uint8_t, rotation_regression_rank_capacity> ranks{};
    std::size_t size{};
    bool valid{};
};

/** Transfer one owning edge token and clear the source variable. */
[[nodiscard]] inline piece_edge take_rotation_edge(piece_edge& edge) noexcept {
    return std::exchange(edge, {});
}

/** Concatenate two bounded self-test emissions without materializing a tree. */
[[nodiscard]] inline rotation_regression_emission
concat_rotation_emissions(rotation_regression_emission first,
                          rotation_regression_emission second) noexcept {
    rotation_regression_emission result{};
    if (!first.valid || !second.valid || first.size > result.ranks.size() ||
        second.size > result.ranks.size() - first.size) {
        return result;
    }
    std::copy_n(first.ranks.begin(), first.size, result.ranks.begin());
    std::copy_n(second.ranks.begin(), second.size, result.ranks.begin() + first.size);
    result.size = first.size + second.size;
    result.valid = true;
    return result;
}

/** Emit a source-only regression tree through its real transform callback. */
[[nodiscard]] inline rotation_regression_emission emit_rotation_regression_edge(piece_edge edge) {
    rotation_regression_emission result{};
    if (!edge) {
        result.valid = true;
        return result;
    }
    if (edge.node->node_kind == piece_node::kind::branch) {
        const logical_children_result children = logical_children(edge);
        return concat_rotation_emissions(emit_rotation_regression_edge(children.first),
                                         emit_rotation_regression_edge(children.second));
    }
    if (edge.node->node_kind != piece_node::kind::source ||
        edge.node->bp_length > result.ranks.size()) {
        return result;
    }

    const source_piece& source = edge.node->payload.source;
    pg_sequence_read_session session{};
    result.size = static_cast<std::size_t>(edge.node->bp_length);
    const auto status =
        source.source.ops->read_ranks(source.source.state,
                                      session,
                                      source.locator,
                                      source.stored_type_params,
                                      edge.transform,
                                      source.from,
                                      std::span<std::uint8_t>{result.ranks.data(), result.size});
    result.valid = status == pg_sequence_source_read_status::ok;
    return result;
}

/** Compare the exact rank streams emitted by two bounded tree views. */
[[nodiscard]] inline bool
same_rotation_emission(rotation_regression_emission const& left,
                       rotation_regression_emission const& right) noexcept {
    return left.valid && right.valid && left.size == right.size &&
           std::equal(left.ranks.begin(), left.ranks.begin() + left.size, right.ranks.begin());
}

/** Make one non-coalescible three-rank source leaf for a rotation fixture. */
[[nodiscard]] inline piece_edge
make_rotation_regression_leaf(piece_tree_arena& arena,
                              pg_sequence_source_handle source,
                              compact_sequence_stdc_type_params bound_type,
                              bioseq::storage::sequence_locator locator,
                              std::uint64_t leaf_ordinal,
                              bioseq::storage::sequence_transform transform) {
    constexpr std::uint64_t leaf_stride = 4u;
    constexpr std::uint64_t leaf_length = 3u;
    const std::uint64_t from = leaf_ordinal * leaf_stride;
    return make_source_node(arena,
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
 * the rotation. Counter and arena checks prove dependency ownership balances.
 */
[[nodiscard]] inline bool
run_transformed_join_rotation_case(transformed_join_rotation_shape shape) {
    const std::uint64_t arena_baseline = live_piece_tree_arenas;
    factory_failure_source_state source_state{};
    constexpr std::array<std::uint8_t, 12u> source_patterns{
        0u,
        0u,
        1u,
        1u,
        2u,
        3u,
        2u,
        1u,
        0u,
        3u,
        3u,
        2u,
    };
    for (std::size_t leaf = 0u; leaf < 4u; ++leaf) {
        for (std::size_t symbol = 0u; symbol < 3u; ++symbol) {
            source_state.ranks[leaf * 4u + symbol] = source_patterns[leaf * 3u + symbol];
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
    const bioseq::storage::sequence_locator locator{
        .fragment_count = 1u,
        .logical_length = source_state.rank_count,
        .source_epoch = 1u,
    };

    auto* source_arena = make_piece_tree_arena();
    piece_edge first = make_rotation_regression_leaf(*source_arena,
                                                     source,
                                                     bound_type,
                                                     locator,
                                                     0u,
                                                     bioseq::storage::sequence_transform::identity);
    piece_edge second =
        make_rotation_regression_leaf(*source_arena,
                                      source,
                                      bound_type,
                                      locator,
                                      1u,
                                      bioseq::storage::sequence_transform::reversed);
    piece_edge third =
        make_rotation_regression_leaf(*source_arena,
                                      source,
                                      bound_type,
                                      locator,
                                      2u,
                                      bioseq::storage::sequence_transform::complemented);
    piece_edge fourth =
        make_rotation_regression_leaf(*source_arena,
                                      source,
                                      bound_type,
                                      locator,
                                      3u,
                                      bioseq::storage::sequence_transform::reverse_complement);

    piece_edge heavy{};
    piece_edge light{};
    switch (shape) {
    case transformed_join_rotation_shape::left_single: {
        piece_edge inner =
            make_branch_node(*source_arena, take_rotation_edge(second), take_rotation_edge(third));
        heavy =
            make_branch_node(*source_arena, take_rotation_edge(first), take_rotation_edge(inner));
        heavy.transform = bioseq::storage::sequence_transform::reverse_complement;
        light = take_rotation_edge(fourth);
        break;
    }
    case transformed_join_rotation_shape::left_double: {
        piece_edge pivot =
            make_branch_node(*source_arena, take_rotation_edge(second), take_rotation_edge(third));
        pivot.transform = bioseq::storage::sequence_transform::reversed;
        heavy =
            make_branch_node(*source_arena, take_rotation_edge(first), take_rotation_edge(pivot));
        heavy.transform = bioseq::storage::sequence_transform::complemented;
        light = take_rotation_edge(fourth);
        break;
    }
    case transformed_join_rotation_shape::right_single: {
        piece_edge inner =
            make_branch_node(*source_arena, take_rotation_edge(third), take_rotation_edge(fourth));
        inner.transform = bioseq::storage::sequence_transform::complemented;
        heavy =
            make_branch_node(*source_arena, take_rotation_edge(inner), take_rotation_edge(second));
        heavy.transform = bioseq::storage::sequence_transform::reversed;
        light = take_rotation_edge(first);
        break;
    }
    case transformed_join_rotation_shape::right_double: {
        piece_edge pivot =
            make_branch_node(*source_arena, take_rotation_edge(second), take_rotation_edge(third));
        pivot.transform = bioseq::storage::sequence_transform::complemented;
        heavy =
            make_branch_node(*source_arena, take_rotation_edge(fourth), take_rotation_edge(pivot));
        heavy.transform = bioseq::storage::sequence_transform::reverse_complement;
        light = take_rotation_edge(first);
        break;
    }
    }

    const bool left_heavy = shape == transformed_join_rotation_shape::left_single ||
                            shape == transformed_join_rotation_shape::left_double;
    const rotation_regression_emission expected =
        left_heavy ? concat_rotation_emissions(emit_rotation_regression_edge(heavy),
                                               emit_rotation_regression_edge(light))
                   : concat_rotation_emissions(emit_rotation_regression_edge(light),
                                               emit_rotation_regression_edge(heavy));

    auto* destination_arena = make_piece_tree_arena();
    tracked_piece_edge tracked_joined =
        left_heavy ? rebalance_joined_edges(*destination_arena,
                                            nullptr,
                                            {.edge = take_rotation_edge(heavy)},
                                            {.edge = take_rotation_edge(light)},
                                            retired_join_root_policy::fixed_trusted_join)
                   : rebalance_joined_edges(*destination_arena,
                                            nullptr,
                                            {.edge = take_rotation_edge(light)},
                                            {.edge = take_rotation_edge(heavy)},
                                            retired_join_root_policy::fixed_trusted_join);
    piece_edge joined = tracked_joined.edge;
    validate_tree_and_limits(joined, bound_type, expected.size, true);
    const rotation_regression_emission actual = emit_rotation_regression_edge(joined);
    const logical_children_result joined_children = logical_children(joined);
    const bool balanced = joined.node->bp_length == 12u && joined.node->piece_count == 4u &&
                          joined.node->height == 3u && edge_height(joined_children.first) == 2u &&
                          edge_height(joined_children.second) == 2u;
    const bool content_matches = same_rotation_emission(expected, actual);

    release_edge(joined);
    release_arena(destination_arena);
    release_arena(source_arena);
    return balanced && content_matches && !source_state.invalid_read &&
           source_state.retain_calls == 4u && source_state.release_calls == 4u &&
           source_state.read_calls == 8u && live_piece_tree_arenas == arena_baseline;
}

/** Exercise an inward-heavy transformed join without unbounded recursion. */
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
    const bioseq::storage::sequence_locator locator{
        .fragment_count = 1u,
        .logical_length = 1u,
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

    auto* arena = make_piece_tree_arena();
    piece_edge first = make_source_node(*arena, leaf_value);
    piece_edge second = make_source_node(*arena, leaf_value);
    piece_edge third = make_source_node(*arena, leaf_value);
    piece_edge fourth = make_source_node(*arena, leaf_value);
    piece_edge inner =
        make_branch_node(*arena, take_rotation_edge(third), take_rotation_edge(fourth));
    piece_edge asymmetric =
        make_branch_node(*arena, take_rotation_edge(second), take_rotation_edge(inner));
    asymmetric.transform = bioseq::storage::sequence_transform::reversed;

    piece_edge joined =
        join_edges(*arena, take_rotation_edge(first), take_rotation_edge(asymmetric));
    validate_tree_and_limits(joined, bound_type, 4u, true);
    const bool balanced =
        joined.node->bp_length == 4u && joined.node->piece_count == 4u && joined.node->height == 3u;
    release_edge(joined);
    release_arena(arena);
    return balanced && source_state.retain_calls == 4u && source_state.release_calls == 4u &&
           live_piece_tree_arenas == arena_baseline;
}

/** Cover the original join and all four single/double rotation shapes. */
} // namespace

/** Provide the SQL facade with this deterministic piece-tree contract. */
[[nodiscard]] inline bool run_transformed_join_rotation_regression() {
    return run_inward_transformed_join_regression() &&
           run_transformed_join_rotation_case(transformed_join_rotation_shape::left_single) &&
           run_transformed_join_rotation_case(transformed_join_rotation_shape::left_double) &&
           run_transformed_join_rotation_case(transformed_join_rotation_shape::right_single) &&
           run_transformed_join_rotation_case(transformed_join_rotation_shape::right_double);
}
