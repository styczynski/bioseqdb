/**
 * @file factory.cppm
 * @brief Test cleanup after failures while constructing piece trees.
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

export module bioseq.postgres.types.compact_sequence:tree_ops_failure_factory;

import :tree_ops_ownership;
import :tree_ops_transforms;
import :tree_ops_validation;
import :flat_abi;
import :source_abi;
import :tree_types;
import bioseq.sequences.rank_pack;
import bioseq.storage.coordinates;
import bioseq.storage.layout;

/** Build a balanced test tree with a positive number of identical literal leaves. */
[[nodiscard]] inline piece_edge build_balanced_literal_probe(piece_tree_arena& arena,
                                                             literal_piece literal,
                                                             std::uint32_t piece_count) {
    if (piece_count == 1u) {
        return make_literal_node(arena, literal);
    }
    const std::uint32_t left_count = piece_count / 2u;
    piece_edge left = build_balanced_literal_probe(arena, literal, left_count);
    piece_edge right = build_balanced_literal_probe(arena, literal, piece_count - left_count);
    return make_branch_node(arena, left, right);
}

/// Cleanup counters captured for one injected factory failure.
struct factory_failure_observation {
    std::uint32_t callback_invocations;
    std::uint32_t armed_callback_invocations;
    std::uint32_t root_tokens_released;
    std::uint32_t arena_tokens_released;
};

/// Reset callback state holding a partially built tree and its arena.
struct factory_failure_pending_root {
    MemoryContextCallback callback;
    piece_tree_arena* destination_arena;
    piece_edge root;
    factory_failure_observation* observation;
    bool armed;
};

/// Release an armed factory result during memory-context reset.
inline void factory_failure_pending_root_reset(void* argument) noexcept {
    auto* guard = static_cast<factory_failure_pending_root*>(argument);
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

/// Test sequence data and counters for reads and source references.
struct factory_failure_source_state {
    std::uint32_t retain_calls;
    std::uint32_t release_calls;
    std::uint32_t read_calls;
    std::uint64_t rank_count;
    std::array<std::uint8_t, 64u> ranks;
    bool invalid_read;
};

/// Count one added reference to the test source.
inline void factory_failure_source_retain(void* argument) noexcept {
    ++static_cast<factory_failure_source_state*>(argument)->retain_calls;
}

/// Record one released factory test source.
inline void factory_failure_source_release(void* argument) noexcept {
    ++static_cast<factory_failure_source_state*>(argument)->release_calls;
}

/// Return the stable identity of a factory test source.
[[nodiscard]] inline pg_sequence_source_identity
factory_failure_source_identity(void const* argument) noexcept {
    return {
        .control_address = reinterpret_cast<std::uintptr_t>(argument),
        .generation = 1u,
    };
}

/// Report that workers cannot share the test source.
[[nodiscard]] inline bool factory_failure_source_worker_shareable(void const*) noexcept {
    return false;
}

/// Read test ranks with the requested reversal or complement and record invalid reads.
[[nodiscard]] inline pg_sequence_source_read_status
factory_failure_source_read(void const* argument,
                            pg_sequence_read_session&,
                            bioseq::storage::sequence_locator const&,
                            compact_sequence_stdc_type_params,
                            bioseq::storage::sequence_transform transform,
                            std::uint64_t source_from,
                            std::span<std::uint8_t> output) {
    auto* state = const_cast<factory_failure_source_state*>(
        static_cast<factory_failure_source_state const*>(argument));
    ++state->read_calls;
    if (source_from > state->rank_count || output.size() > state->rank_count - source_from) {
        state->invalid_read = true;
        std::fill(output.begin(), output.end(), 0u);
        return pg_sequence_source_read_status::ok;
    }

    const bool reverse = bioseq::storage::is_reversed(transform);
    const bool complement = bioseq::storage::is_complemented(transform);
    for (std::size_t output_index = 0u; output_index < output.size(); ++output_index) {
        const std::uint64_t physical =
            source_from + (reverse ? output.size() - 1u - output_index : output_index);
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

/// Callbacks for the factory test sequence source.
constexpr pg_sequence_source_backend_ops factory_failure_source_ops{
    .retain = factory_failure_source_retain,
    .release = factory_failure_source_release,
    .identity = factory_failure_source_identity,
    .worker_shareable = factory_failure_source_worker_shareable,
    .read_ranks = factory_failure_source_read,
};

/// Construct one unbound literal descriptor for a failure fixture.
[[nodiscard]] inline literal_piece factory_failure_unbound_literal(piece_tree_arena* storage_arena,
                                                                   std::byte const* bytes,
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

/// Inject a construction failure and check that reset releases every added reference.
[[nodiscard]] inline bool
run_piece_tree_factory_failure_case(piece_tree_injected_failure_point failure_point) {
    const std::uint64_t arena_baseline = live_piece_tree_arenas;
    piece_tree_arena* external_arena = nullptr;
    piece_edge external_root{};
    std::byte* external_storage = nullptr;

    if (failure_point == piece_tree_injected_failure_point::arena_dependency_registration ||
        failure_point == piece_tree_injected_failure_point::literal_binding_factory) {
        external_arena = make_piece_tree_arena();
        if (failure_point == piece_tree_injected_failure_point::literal_binding_factory) {
            external_storage = allocate_piece_literal_storage(*external_arena, 2u);
            external_storage[0] = std::byte{0x41};
            external_storage[1] = std::byte{0x43};
            external_root = make_literal_node(
                *external_arena,
                factory_failure_unbound_literal(external_arena, external_storage, 0u, 2u));
        }
    }

    auto* observation =
        static_cast<factory_failure_observation*>(palloc0(sizeof(factory_failure_observation)));
    auto* source_state =
        static_cast<factory_failure_source_state*>(palloc0(sizeof(factory_failure_source_state)));
    const pg_sequence_source_handle source{
        .state = source_state,
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
    const source_piece source_leaf{
        .source = source,
        .locator = locator,
        .stored_type_params = bound_type,
        .last_padding_letters = 0u,
        .from = 0u,
        .to = 1u,
    };

    MemoryContext build_context = AllocSetContextCreate(
        CurrentMemoryContext, "bioseq injected piece build", ALLOCSET_SMALL_SIZES);
    const MemoryContext saved_context = CurrentMemoryContext;
    bool volatile caught = false;
    observe_piece_tree_failure_noexcept(piece_tree_injected_failure_point::none);
    arm_piece_tree_failure_noexcept(failure_point);

    PG_TRY();
    {
        auto* guard = static_cast<factory_failure_pending_root*>(
            MemoryContextAllocZero(build_context, sizeof(factory_failure_pending_root)));
        guard->callback.func = factory_failure_pending_root_reset;
        guard->callback.arg = guard;
        guard->observation = observation;
        guard->armed = true;
        MemoryContextRegisterResetCallback(build_context, std::addressof(guard->callback));

        guard->destination_arena = make_piece_tree_arena();
        switch (failure_point) {
        case piece_tree_injected_failure_point::arena_factory:
            inject_piece_tree_failure(failure_point);
            break;
        case piece_tree_injected_failure_point::literal_storage_factory: {
            (void)allocate_piece_literal_storage(*guard->destination_arena, 1u);
            inject_piece_tree_failure(failure_point);
            break;
        }
        case piece_tree_injected_failure_point::literal_leaf_factory: {
            auto* storage = allocate_piece_literal_storage(*guard->destination_arena, 1u);
            storage[0] = std::byte{0x41};
            guard->root = make_literal_node(
                *guard->destination_arena,
                factory_failure_unbound_literal(guard->destination_arena, storage, 0u, 1u));
            inject_piece_tree_failure(failure_point);
            break;
        }
        case piece_tree_injected_failure_point::branch_factory: {
            auto* storage = allocate_piece_literal_storage(*guard->destination_arena, 2u);
            storage[0] = std::byte{0x41};
            storage[1] = std::byte{0x43};
            piece_edge left = make_literal_node(
                *guard->destination_arena,
                factory_failure_unbound_literal(guard->destination_arena, storage, 0u, 1u));
            piece_edge right = make_literal_node(
                *guard->destination_arena,
                factory_failure_unbound_literal(guard->destination_arena, storage, 1u, 2u));
            guard->root = make_branch_node(*guard->destination_arena, left, right);
            inject_piece_tree_failure(failure_point);
            break;
        }
        case piece_tree_injected_failure_point::arena_dependency_registration:
            register_arena_dependency(*guard->destination_arena, external_arena);
            /* Exercise dependency registration before raising the test error. The
             * fallback keeps the failure after registration if the helper returns. */
            inject_piece_tree_failure(failure_point);
            break;
        case piece_tree_injected_failure_point::source_dependency_registration:
            register_source_dependency(*guard->destination_arena, source);
            inject_piece_tree_failure(failure_point);
            break;
        case piece_tree_injected_failure_point::source_leaf_factory:
            guard->root = make_source_node(*guard->destination_arena, source_leaf);
            inject_piece_tree_failure(failure_point);
            break;
        case piece_tree_injected_failure_point::literal_binding_factory:
            guard->root = bind_literals_copy(*guard->destination_arena, external_root, bound_type);
            inject_piece_tree_failure(failure_point);
            break;
        case piece_tree_injected_failure_point::none:
            break;
        }

        arm_piece_tree_failure_noexcept(piece_tree_injected_failure_point::none);
        MemoryContextDelete(build_context);
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        arm_piece_tree_failure_noexcept(piece_tree_injected_failure_point::none);
        FlushErrorState();
        MemoryContextDelete(build_context);
        caught = true;
    }
    PG_END_TRY();

    const bool source_case =
        failure_point == piece_tree_injected_failure_point::source_dependency_registration ||
        failure_point == piece_tree_injected_failure_point::source_leaf_factory;
    const bool published_root_case =
        failure_point == piece_tree_injected_failure_point::literal_leaf_factory ||
        failure_point == piece_tree_injected_failure_point::branch_factory ||
        failure_point == piece_tree_injected_failure_point::source_leaf_factory ||
        failure_point == piece_tree_injected_failure_point::literal_binding_factory;
    const bool external_refcount_restored =
        external_arena == nullptr ||
        (failure_point == piece_tree_injected_failure_point::arena_dependency_registration
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
        caught && observed_piece_tree_failure_noexcept() == failure_point &&
        observation->callback_invocations == 1u && observation->armed_callback_invocations == 1u &&
        observation->root_tokens_released == static_cast<std::uint32_t>(published_root_case) &&
        observation->arena_tokens_released == 1u &&
        source_state->retain_calls == static_cast<std::uint32_t>(source_case) &&
        source_state->release_calls == static_cast<std::uint32_t>(source_case) &&
        external_refcount_restored && live_piece_tree_arenas == arena_baseline;
    if (!result) {
        ereport(WARNING,
                (errmsg_internal("bioseq piece-tree factory failure case: point=%u caught=%d "
                                 "observed=%u callbacks=%u armed_callbacks=%u roots=%u "
                                 "expected_roots=%u arenas=%u source_retains=%u expected_source=%u "
                                 "source_releases=%u external_restored=%d live=%llu baseline=%llu",
                                 static_cast<unsigned>(failure_point),
                                 caught ? 1 : 0,
                                 static_cast<unsigned>(observed_piece_tree_failure_noexcept()),
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

/** Check cleanup at every factory and dependency-registration failure point. */
[[nodiscard]] inline bool run_piece_tree_factory_failure_matrix() {
    for (std::uint8_t ordinal =
             static_cast<std::uint8_t>(piece_tree_injected_failure_point::arena_factory);
         ordinal <=
         static_cast<std::uint8_t>(piece_tree_injected_failure_point::literal_binding_factory);
         ++ordinal) {
        if (!run_piece_tree_factory_failure_case(
                static_cast<piece_tree_injected_failure_point>(ordinal))) {
            return false;
        }
    }
    return true;
}
