/**
 * @file tracked_failures.cppm
 * @brief Test cleanup of partial joins, slices, and alphabet binding.
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

export module bioseq.postgres.types.compact_sequence:tree_ops_failure_tracked;

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

/// Check the current PostgreSQL error code, then free and clear the error.
[[nodiscard]] inline bool consume_expected_error(int expected_sqlstate,
                                                 MemoryContext destination_context) {
    MemoryContextSwitchTo(destination_context);
    ErrorData* error = CopyErrorData();
    const bool matched = error != nullptr && error->sqlerrcode == expected_sqlstate;
    FlushErrorState();
    if (error != nullptr) {
        FreeErrorData(error);
    }
    return matched;
}

/// Tree shape used to reach one tracked join failure point.
enum class tracked_join_failure_shape : std::uint8_t {
    branch,
    source_coalesce,
    literal_coalesce,
    rotation,
    transformed_binding,
};

/// Input trees, source, data, and reference counts for one join failure test.
struct tracked_join_failure_fixture {
    piece_tree_arena* arena;
    piece_edge left;
    piece_edge right;
    factory_failure_source_state* source_state;
    compact_sequence_stdc_type_params type;
    std::uint64_t live_baseline;
    std::uint32_t arena_refcount_baseline;
    std::uint32_t source_retain_baseline;
    std::uint32_t source_release_baseline;
};

/// Build input trees that reach the requested join or binding failure path.
[[nodiscard]] inline tracked_join_failure_fixture*
make_tracked_join_failure_fixture(tracked_join_failure_shape shape) {
    auto* fixture =
        static_cast<tracked_join_failure_fixture*>(palloc0(sizeof(tracked_join_failure_fixture)));
    fixture->live_baseline = live_piece_tree_arenas;
    fixture->arena = make_piece_tree_arena();

    if (shape == tracked_join_failure_shape::literal_coalesce ||
        shape == tracked_join_failure_shape::transformed_binding) {
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
        auto* left_storage = allocate_piece_literal_storage(*fixture->arena, 1u);
        auto* right_storage = allocate_piece_literal_storage(*fixture->arena, 1u);
        left_storage[0] = std::byte{0x41};
        right_storage[0] = std::byte{0x43};
        piece_edge left = make_literal_node(
            *fixture->arena, factory_failure_unbound_literal(fixture->arena, left_storage, 0u, 1u));
        piece_edge right = make_literal_node(
            *fixture->arena,
            factory_failure_unbound_literal(fixture->arena, right_storage, 0u, 1u));
        if (shape == tracked_join_failure_shape::transformed_binding) {
            fixture->left = make_branch_node(*fixture->arena, left, right);
            fixture->left.transform = bioseq::storage::sequence_transform::reversed;
        } else {
            fixture->left = left;
            fixture->right = right;
        }
    } else {
        fixture->source_state = static_cast<factory_failure_source_state*>(
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
        const auto make_leaf = [&fixture, source](std::uint64_t from,
                                                  std::uint64_t to,
                                                  bioseq::storage::sequence_locator locator) {
            return make_source_node(*fixture->arena,
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
            const bioseq::storage::sequence_locator locator{
                .fragment_count = 1u,
                .logical_length = 2u,
                .source_epoch = 1u,
            };
            fixture->left = make_leaf(0u, 1u, locator);
            fixture->right = make_leaf(1u, 2u, locator);
        } else if (shape == tracked_join_failure_shape::branch) {
            const bioseq::storage::sequence_locator locator{
                .fragment_count = 1u,
                .logical_length = 1u,
                .source_epoch = 1u,
            };
            fixture->left = make_leaf(0u, 1u, locator);
            fixture->right = make_leaf(0u, 1u, locator);
        } else {
            const bioseq::storage::sequence_locator locator{
                .fragment_count = 1u,
                .logical_length = 1u,
                .source_epoch = 1u,
            };
            std::array<piece_edge, 7u> leaves{};
            for (piece_edge& leaf : leaves) {
                leaf = make_leaf(0u, 1u, locator);
            }
            piece_edge left_pair = make_branch_node(*fixture->arena, leaves[0], leaves[1]);
            piece_edge right_left = make_branch_node(*fixture->arena, leaves[2], leaves[3]);
            piece_edge right_right = make_branch_node(*fixture->arena, leaves[4], leaves[5]);
            piece_edge right_four = make_branch_node(*fixture->arena, right_left, right_right);
            fixture->left = make_branch_node(*fixture->arena, left_pair, right_four);
            fixture->right = leaves[6];
        }
    }

    fixture->arena_refcount_baseline = fixture->arena->refcount;
    if (fixture->source_state != nullptr) {
        fixture->source_retain_baseline = fixture->source_state->retain_calls;
        fixture->source_release_baseline = fixture->source_state->release_calls;
    }
    return fixture;
}

/// Free the test inputs and check that  source references and arena counts balance
[[nodiscard]] inline bool
destroy_tracked_join_failure_fixture(tracked_join_failure_fixture* fixture) {
    auto* source_state = fixture->source_state;
    const std::uint64_t live_baseline = fixture->live_baseline;
    release_edge(fixture->left);
    release_edge(fixture->right);
    release_arena(fixture->arena);
    const bool source_balanced =
        source_state == nullptr || source_state->retain_calls == source_state->release_calls;
    if (source_state != nullptr)
        pfree(source_state);
    pfree(fixture);
    return source_balanced && live_piece_tree_arenas == live_baseline;
}

/// Cleanup counters captured for one tracked join failure.
struct tracked_join_failure_observation {
    std::uint32_t callbacks;
    std::uint32_t armed_callbacks;
    std::uint32_t tokens_released;
    std::uint32_t destination_tokens_released;
    bool ledger_drained;
};

/// Reset callback state holding a join arena and its unfinished edge references.
struct tracked_join_failure_guard {
    MemoryContextCallback callback;
    piece_tree_arena* destination_arena;
    piece_edge_cleanup_ledger ledger;
    tracked_join_failure_observation* observation;
    bool armed;
};

/// Release tracked edges and the destination arena when the test context resets.
inline void tracked_join_failure_reset(void* argument) noexcept {
    auto* guard = static_cast<tracked_join_failure_guard*>(argument);
    if (guard == nullptr || guard->observation == nullptr)
        return;
    ++guard->observation->callbacks;
    if (!guard->armed)
        return;
    guard->armed = false;
    ++guard->observation->armed_callbacks;
    guard->observation->tokens_released += guard->ledger.token_count;
    release_piece_edge_cleanup_ledger_noexcept(guard->ledger);
    guard->observation->ledger_drained = piece_edge_cleanup_ledger_empty_noexcept(guard->ledger);
    if (guard->destination_arena != nullptr) {
        ++guard->observation->destination_tokens_released;
        release_arena(guard->destination_arena);
        guard->destination_arena = nullptr;
    }
}

/// Inject a join or binding failure and check that cleanup restores the input references.
[[nodiscard]] inline bool
run_tracked_join_failure_case(piece_tree_injected_failure_point failure_point,
                              tracked_join_failure_shape shape) {
    auto* fixture = make_tracked_join_failure_fixture(shape);
    const std::uint64_t fixture_live_arenas = live_piece_tree_arenas;
    auto* observation = static_cast<tracked_join_failure_observation*>(
        palloc0(sizeof(tracked_join_failure_observation)));
    MemoryContext build_context = AllocSetContextCreate(
        CurrentMemoryContext, "bioseq tracked join failure", ALLOCSET_SMALL_SIZES);
    const MemoryContext saved_context = CurrentMemoryContext;
    bool volatile caught = false;
    bool volatile sqlstate_matched = false;
    observe_piece_tree_failure_noexcept(piece_tree_injected_failure_point::none);
    arm_piece_tree_failure_noexcept(failure_point);

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
        if (shape == tracked_join_failure_shape::transformed_binding) {
            (void)bind_literals_copy_error_atomic(
                *guard->destination_arena, fixture->left, fixture->type, guard->ledger);
        } else {
            (void)join_edges_error_atomic(
                *guard->destination_arena, guard->ledger, fixture->left, fixture->right);
        }
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

    const bool fixture_restored =
        fixture->arena->refcount == fixture->arena_refcount_baseline &&
        live_piece_tree_arenas == fixture_live_arenas &&
        (fixture->source_state == nullptr ||
         (fixture->source_state->retain_calls >= fixture->source_retain_baseline &&
          fixture->source_state->release_calls >= fixture->source_release_baseline &&
          fixture->source_state->retain_calls - fixture->source_retain_baseline ==
              fixture->source_state->release_calls - fixture->source_release_baseline));
    const bool case_passed =
        caught && sqlstate_matched && observed_piece_tree_failure_noexcept() == failure_point &&
        observation->callbacks == 1u && observation->armed_callbacks == 1u &&
        observation->tokens_released != 0u && observation->destination_tokens_released == 1u &&
        observation->ledger_drained && fixture_restored;
    if (!case_passed) {
        ereport(WARNING,
                (errmsg_internal("bioseq tracked-join failure case: point=%u shape=%u caught=%d "
                                 "sqlstate=%d observed=%u callbacks=%u armed=%u tokens=%u "
                                 "destinations=%u drained=%d restored=%d",
                                 static_cast<unsigned>(failure_point),
                                 static_cast<unsigned>(shape),
                                 caught ? 1 : 0,
                                 sqlstate_matched ? 1 : 0,
                                 static_cast<unsigned>(observed_piece_tree_failure_noexcept()),
                                 observation->callbacks,
                                 observation->armed_callbacks,
                                 observation->tokens_released,
                                 observation->destination_tokens_released,
                                 observation->ledger_drained ? 1 : 0,
                                 fixture_restored ? 1 : 0)));
    }
    pfree(observation);
    const bool fixture_destroyed = destroy_tracked_join_failure_fixture(fixture);
    return case_passed && fixture_destroyed;
}

/** Test cleanup after failures in reference acquisition, allocation, and join cwnstruction. */
[[nodiscard]] inline bool run_tracked_join_failure_matrix() {
    using point = piece_tree_injected_failure_point;
    using shape = tracked_join_failure_shape;
    constexpr std::array cases{
        std::pair{point::join_left_operand_retain, shape::branch},
        std::pair{point::join_right_operand_retain, shape::branch},
        std::pair{point::arena_dependency_registration, shape::branch},
        std::pair{point::source_dependency_registration, shape::source_coalesce},
        std::pair{point::literal_storage_factory, shape::literal_coalesce},
        std::pair{point::join_node_allocation, shape::branch},
        std::pair{point::join_node_allocation, shape::source_coalesce},
        std::pair{point::join_node_allocation, shape::literal_coalesce},
        std::pair{point::join_node_metadata, shape::branch},
        std::pair{point::join_node_metadata, shape::source_coalesce},
        std::pair{point::join_node_metadata, shape::literal_coalesce},
        std::pair{point::join_factory_transition, shape::branch},
        std::pair{point::join_factory_transition, shape::source_coalesce},
        std::pair{point::join_factory_transition, shape::literal_coalesce},
        std::pair{point::join_edge_identity_transition, shape::transformed_binding},
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

namespace {

/// Fail after the first slice leaf is built and check that reset frees the partial result.
[[nodiscard]] inline bool
run_tracked_slice_failure_iteration(piece_edge source_root,
                                    piece_tree_arena& source_arena,
                                    factory_failure_source_state& source_state) {
    const std::uint64_t live_baseline = live_piece_tree_arenas;
    const std::uint32_t arena_refcount_baseline = source_arena.refcount;
    const std::uint32_t source_retain_baseline = source_state.retain_calls;
    const std::uint32_t source_release_baseline = source_state.release_calls;
    auto* observation = static_cast<tracked_join_failure_observation*>(
        palloc0(sizeof(tracked_join_failure_observation)));
    MemoryContext build_context = AllocSetContextCreate(
        CurrentMemoryContext, "bioseq tracked slice failure", ALLOCSET_SMALL_SIZES);
    const MemoryContext saved_context = CurrentMemoryContext;
    bool volatile caught = false;
    bool volatile sqlstate_matched = false;
    observe_piece_tree_failure_noexcept(piece_tree_injected_failure_point::none);
    arm_piece_tree_failure_noexcept(
        piece_tree_injected_failure_point::split_first_leaf_intermediate);

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
        (void)slice_tree(*guard->destination_arena, guard->ledger, source_root, 1u, 3u);
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

    const bool restored = source_arena.refcount == arena_refcount_baseline &&
                          live_piece_tree_arenas == live_baseline &&
                          source_state.retain_calls > source_retain_baseline &&
                          source_state.release_calls > source_release_baseline &&
                          source_state.retain_calls - source_retain_baseline ==
                              source_state.release_calls - source_release_baseline;
    const bool passed = caught && sqlstate_matched &&
                        observed_piece_tree_failure_noexcept() ==
                            piece_tree_injected_failure_point::split_first_leaf_intermediate &&
                        observation->callbacks == 1u && observation->armed_callbacks == 1u &&
                        observation->tokens_released == 1u &&
                        observation->destination_tokens_released == 1u &&
                        observation->ledger_drained && restored;
    pfree(observation);
    return passed;
}

/// Repeat the slice  failure on one source tree to check for accumulated leaks.
[[nodiscard]] inline bool run_tracked_slice_failure_matrix() {
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
    const piece_edge source_root = make_source_node(*source_arena,
                                                    source_piece{
                                                        .source = source,
                                                        .locator =
                                                            {
                                                                .fragment_count = 1u,
                                                                .logical_length = 4u,
                                                                .source_epoch = 1u,
                                                            },
                                                        .stored_type_params = bound_type,
                                                        .last_padding_letters = 0u,
                                                        .from = 0u,
                                                        .to = 4u,
                                                    });
    bool passed = true;
    for (std::uint32_t iteration = 0u; iteration < repetitions && passed; ++iteration) {
        passed = run_tracked_slice_failure_iteration(source_root, *source_arena, source_state);
    }
    release_edge(source_root);
    release_arena(source_arena);
    return passed && source_state.retain_calls == source_state.release_calls &&
           live_piece_tree_arenas == live_baseline;
}

/// Check that a bad later literal releases the already bound part of the tree.
[[nodiscard]] inline bool
run_invalid_later_literal_binding_iteration(piece_edge source_root,
                                            piece_tree_arena& source_arena,
                                            compact_sequence_stdc_type_params bound_type) {
    const std::uint64_t live_baseline = live_piece_tree_arenas;
    const std::uint32_t arena_refcount_baseline = source_arena.refcount;
    auto* observation = static_cast<tracked_join_failure_observation*>(
        palloc0(sizeof(tracked_join_failure_observation)));
    MemoryContext build_context = AllocSetContextCreate(
        CurrentMemoryContext, "bioseq invalid later literal binding", ALLOCSET_SMALL_SIZES);
    const MemoryContext saved_context = CurrentMemoryContext;
    bool volatile caught = false;
    bool volatile sqlstate_matched = false;

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
            *guard->destination_arena, source_root, bound_type, guard->ledger);
        MemoryContextDelete(build_context);
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        sqlstate_matched =
            consume_expected_error(ERRCODE_INVALID_TEXT_REPRESENTATION, saved_context);
        MemoryContextDelete(build_context);
        caught = true;
    }
    PG_END_TRY();

    const bool passed =
        caught && sqlstate_matched && observation->callbacks == 1u &&
        observation->armed_callbacks == 1u && observation->tokens_released == 1u &&
        observation->destination_tokens_released == 1u && observation->ledger_drained &&
        source_arena.refcount == arena_refcount_baseline && live_piece_tree_arenas == live_baseline;
    pfree(observation);
    return passed;
}

/// Repeatedly bind `AC` followed by invalid DNA4 letter `X` and check cleanup.
[[nodiscard]] inline bool run_invalid_later_literal_binding_matrix() {
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
        *source_arena, factory_failure_unbound_literal(source_arena, storage, 0u, 2u));
    piece_edge invalid = make_literal_node(
        *source_arena, factory_failure_unbound_literal(source_arena, storage, 2u, 3u));
    const piece_edge source_root = make_branch_node(*source_arena, valid, invalid);
    bool passed = true;
    for (std::uint32_t iteration = 0u; iteration < repetitions && passed; ++iteration) {
        passed =
            run_invalid_later_literal_binding_iteration(source_root, *source_arena, bound_type);
    }
    release_edge(source_root);
    release_arena(source_arena);
    return passed && live_piece_tree_arenas == live_baseline;
}

/// Check that cleanup records corruption and keeps the suspect arena out of further release.
[[nodiscard]] inline bool run_piece_tree_quarantine_contract_noexcept() noexcept {
    piece_tree_quarantine_snapshot observed{};
    piece_tree_arena suspect{};
    suspect.refcount = 1u;
    suspect.generation = 77u;
    suspect.release_next = std::addressof(suspect);
    record_piece_tree_quarantine_noexcept(observed,
                                          piece_tree_quarantine_reason::arena_refcount,
                                          "negative arena quarantine",
                                          std::addressof(suspect));
    record_piece_tree_quarantine_noexcept(observed,
                                          piece_tree_quarantine_reason::cleanup_ledger,
                                          "negative ledger quarantine",
                                          std::addressof(suspect));
    return observed.invariant_failures == 2u && observed.quarantined_arenas == 1u &&
           observed.first_reason == piece_tree_quarantine_reason::arena_refcount &&
           observed.last_reason == piece_tree_quarantine_reason::cleanup_ledger &&
           std::strcmp(observed.first_detail, "negative arena quarantine") == 0 &&
           std::strcmp(observed.last_detail, "negative ledger quarantine") == 0 &&
           observed.last_arena_generation == 77u &&
           suspect.refcount == std::numeric_limits<std::uint32_t>::max() &&
           suspect.release_next == nullptr;
}

} // namespace

/** Test corrupt-state reporting and cleanup after slice or alphabet-binding failures. */
[[nodiscard]] inline bool run_piece_tree_error_atomic_matrix() {
    return run_piece_tree_quarantine_contract_noexcept() && run_tracked_slice_failure_matrix() &&
           run_invalid_later_literal_binding_matrix();
}
