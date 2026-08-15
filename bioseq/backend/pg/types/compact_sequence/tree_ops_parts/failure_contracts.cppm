/**
 * @file failure_contracts.cppm
 * @brief Deterministic failure, cleanup, and balancing contracts for piece trees.
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

export module bioseq.postgres.types.compact_sequence:tree_ops_failure_contracts;

import :tree_ops_ownership;
import :tree_ops_transforms;
import :tree_ops_validation;
import :flat_abi;
import :source_abi;
import :tree_types;
import bioseq.sequences.rank_pack;
import bioseq.storage.coordinates;
import bioseq.storage.layout;

/** Provide the SQL facade with this deterministic piece-tree contract. */
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

namespace {

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
    bioseq::storage::sequence_locator const &,
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

  const bool reverse = bioseq::storage::is_reversed(transform);
  const bool complement = bioseq::storage::is_complemented(transform);
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
      CurrentMemoryContext, "bioseq injected piece build",
      ALLOCSET_SMALL_SIZES);
  const MemoryContext saved_context = CurrentMemoryContext;
  bool volatile caught = false;
  observe_piece_tree_failure_noexcept(
      piece_tree_injected_failure_point::none);
  arm_piece_tree_failure_noexcept(failure_point);

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

    arm_piece_tree_failure_noexcept(
        piece_tree_injected_failure_point::none);
    MemoryContextDelete(build_context);
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(saved_context);
    arm_piece_tree_failure_noexcept(
        piece_tree_injected_failure_point::none);
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
      && observed_piece_tree_failure_noexcept() == failure_point
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
                static_cast<unsigned>(
                    observed_piece_tree_failure_noexcept()),
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

} // namespace

/** Provide the SQL facade with this deterministic piece-tree contract. */
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

namespace {

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

enum class tracked_join_failure_shape : std::uint8_t {
  branch,
  source_coalesce,
  literal_coalesce,
  rotation,
  transformed_binding,
};

struct tracked_join_failure_fixture {
  piece_tree_arena *arena;
  piece_edge left;
  piece_edge right;
  factory_failure_source_state *source_state;
  compact_sequence_stdc_type_params type;
  std::uint64_t live_baseline;
  std::uint32_t arena_refcount_baseline;
  std::uint32_t source_retain_baseline;
  std::uint32_t source_release_baseline;
};

[[nodiscard]] inline tracked_join_failure_fixture *
make_tracked_join_failure_fixture(tracked_join_failure_shape shape) {
  auto *fixture = static_cast<tracked_join_failure_fixture *>(
      palloc0(sizeof(tracked_join_failure_fixture)));
  fixture->live_baseline = live_piece_tree_arenas;
  fixture->arena = make_piece_tree_arena();

  if (shape == tracked_join_failure_shape::literal_coalesce
      || shape == tracked_join_failure_shape::transformed_binding) {
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
    auto *left_storage = allocate_piece_literal_storage(*fixture->arena, 1u);
    auto *right_storage = allocate_piece_literal_storage(*fixture->arena, 1u);
    left_storage[0] = std::byte{0x41};
    right_storage[0] = std::byte{0x43};
    piece_edge left = make_literal_node(
        *fixture->arena,
        factory_failure_unbound_literal(
            fixture->arena, left_storage, 0u, 1u));
    piece_edge right = make_literal_node(
        *fixture->arena,
        factory_failure_unbound_literal(
            fixture->arena, right_storage, 0u, 1u));
    if (shape == tracked_join_failure_shape::transformed_binding) {
      fixture->left = make_branch_node(
          *fixture->arena, left, right);
      fixture->left.transform =
          bioseq::storage::sequence_transform::reversed;
    } else {
      fixture->left = left;
      fixture->right = right;
    }
  } else {
    fixture->source_state = static_cast<factory_failure_source_state *>(
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
    const auto make_leaf = [&fixture, source](
        std::uint64_t from, std::uint64_t to,
        bioseq::storage::sequence_locator locator) {
      return make_source_node(
          *fixture->arena,
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
      piece_edge left_pair = make_branch_node(
          *fixture->arena, leaves[0], leaves[1]);
      piece_edge right_left = make_branch_node(
          *fixture->arena, leaves[2], leaves[3]);
      piece_edge right_right = make_branch_node(
          *fixture->arena, leaves[4], leaves[5]);
      piece_edge right_four = make_branch_node(
          *fixture->arena, right_left, right_right);
      fixture->left = make_branch_node(
          *fixture->arena, left_pair, right_four);
      fixture->right = leaves[6];
    }
  }

  fixture->arena_refcount_baseline = fixture->arena->refcount;
  if (fixture->source_state != nullptr) {
    fixture->source_retain_baseline =
        fixture->source_state->retain_calls;
    fixture->source_release_baseline =
        fixture->source_state->release_calls;
  }
  return fixture;
}

[[nodiscard]] inline bool destroy_tracked_join_failure_fixture(
    tracked_join_failure_fixture *fixture) {
  auto *source_state = fixture->source_state;
  const std::uint64_t live_baseline = fixture->live_baseline;
  release_edge(fixture->left);
  release_edge(fixture->right);
  release_arena(fixture->arena);
  const bool source_balanced = source_state == nullptr
      || source_state->retain_calls == source_state->release_calls;
  if (source_state != nullptr) pfree(source_state);
  pfree(fixture);
  return source_balanced && live_piece_tree_arenas == live_baseline;
}

struct tracked_join_failure_observation {
  std::uint32_t callbacks;
  std::uint32_t armed_callbacks;
  std::uint32_t tokens_released;
  std::uint32_t destination_tokens_released;
  bool ledger_drained;
};

struct tracked_join_failure_guard {
  MemoryContextCallback callback;
  piece_tree_arena *destination_arena;
  piece_edge_cleanup_ledger ledger;
  tracked_join_failure_observation *observation;
  bool armed;
};

inline void tracked_join_failure_reset(void *argument) noexcept {
  auto *guard = static_cast<tracked_join_failure_guard *>(argument);
  if (guard == nullptr || guard->observation == nullptr) return;
  ++guard->observation->callbacks;
  if (!guard->armed) return;
  guard->armed = false;
  ++guard->observation->armed_callbacks;
  guard->observation->tokens_released += guard->ledger.token_count;
  release_piece_edge_cleanup_ledger_noexcept(guard->ledger);
  guard->observation->ledger_drained =
      piece_edge_cleanup_ledger_empty_noexcept(guard->ledger);
  if (guard->destination_arena != nullptr) {
    ++guard->observation->destination_tokens_released;
    release_arena(guard->destination_arena);
    guard->destination_arena = nullptr;
  }
}

[[nodiscard]] inline bool run_tracked_join_failure_case(
    piece_tree_injected_failure_point failure_point,
    tracked_join_failure_shape shape) {
  auto *fixture = make_tracked_join_failure_fixture(shape);
  const std::uint64_t fixture_live_arenas = live_piece_tree_arenas;
  auto *observation = static_cast<tracked_join_failure_observation *>(
      palloc0(sizeof(tracked_join_failure_observation)));
  MemoryContext build_context = AllocSetContextCreate(
      CurrentMemoryContext, "bioseq tracked join failure",
      ALLOCSET_SMALL_SIZES);
  const MemoryContext saved_context = CurrentMemoryContext;
  bool volatile caught = false;
  bool volatile sqlstate_matched = false;
  observe_piece_tree_failure_noexcept(
      piece_tree_injected_failure_point::none);
  arm_piece_tree_failure_noexcept(failure_point);

  PG_TRY();
  {
    auto *guard = static_cast<tracked_join_failure_guard *>(
        MemoryContextAllocZero(
            build_context, sizeof(tracked_join_failure_guard)));
    guard->callback.func = tracked_join_failure_reset;
    guard->callback.arg = guard;
    guard->observation = observation;
    guard->armed = true;
    MemoryContextRegisterResetCallback(
        build_context, std::addressof(guard->callback));
    guard->destination_arena = make_piece_tree_arena();
    if (shape == tracked_join_failure_shape::transformed_binding) {
      (void)bind_literals_copy_error_atomic(
          *guard->destination_arena, fixture->left,
          fixture->type, guard->ledger);
    } else {
      (void)join_edges_error_atomic(
          *guard->destination_arena, guard->ledger,
          fixture->left, fixture->right);
    }
    arm_piece_tree_failure_noexcept(
        piece_tree_injected_failure_point::none);
    MemoryContextDelete(build_context);
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(saved_context);
    arm_piece_tree_failure_noexcept(
        piece_tree_injected_failure_point::none);
    sqlstate_matched = consume_expected_error(
        ERRCODE_INTERNAL_ERROR, saved_context);
    MemoryContextDelete(build_context);
    caught = true;
  }
  PG_END_TRY();

  const bool fixture_restored =
      fixture->arena->refcount == fixture->arena_refcount_baseline
      && live_piece_tree_arenas == fixture_live_arenas
      && (fixture->source_state == nullptr
          || (fixture->source_state->retain_calls
                  >= fixture->source_retain_baseline
              && fixture->source_state->release_calls
                  >= fixture->source_release_baseline
              && fixture->source_state->retain_calls
                      - fixture->source_retain_baseline
                  == fixture->source_state->release_calls
                      - fixture->source_release_baseline));
  const bool case_passed = caught && sqlstate_matched
      && observed_piece_tree_failure_noexcept() == failure_point
      && observation->callbacks == 1u
      && observation->armed_callbacks == 1u
      && observation->tokens_released != 0u
      && observation->destination_tokens_released == 1u
      && observation->ledger_drained && fixture_restored;
  if (!case_passed) {
    ereport(WARNING,
            (errmsg_internal(
                "bioseq tracked-join failure case: point=%u shape=%u caught=%d sqlstate=%d observed=%u callbacks=%u armed=%u tokens=%u destinations=%u drained=%d restored=%d",
                static_cast<unsigned>(failure_point),
                static_cast<unsigned>(shape), caught ? 1 : 0,
                sqlstate_matched ? 1 : 0,
                static_cast<unsigned>(
                    observed_piece_tree_failure_noexcept()),
                observation->callbacks,
                observation->armed_callbacks,
                observation->tokens_released,
                observation->destination_tokens_released,
                observation->ledger_drained ? 1 : 0,
                fixture_restored ? 1 : 0)));
  }
  pfree(observation);
  const bool fixture_destroyed =
      destroy_tracked_join_failure_fixture(fixture);
  return case_passed && fixture_destroyed;
}

} // namespace

/** Provide the SQL facade with this deterministic piece-tree contract. */
[[nodiscard]] inline bool run_tracked_join_failure_matrix() {
  using point = piece_tree_injected_failure_point;
  using shape = tracked_join_failure_shape;
  constexpr std::array cases{
      std::pair{point::join_left_operand_retain, shape::branch},
      std::pair{point::join_right_operand_retain, shape::branch},
      std::pair{point::arena_dependency_registration, shape::branch},
      std::pair{point::source_dependency_registration,
                shape::source_coalesce},
      std::pair{point::literal_storage_factory,
                shape::literal_coalesce},
      std::pair{point::join_node_allocation, shape::branch},
      std::pair{point::join_node_allocation, shape::source_coalesce},
      std::pair{point::join_node_allocation, shape::literal_coalesce},
      std::pair{point::join_node_metadata, shape::branch},
      std::pair{point::join_node_metadata, shape::source_coalesce},
      std::pair{point::join_node_metadata, shape::literal_coalesce},
      std::pair{point::join_factory_transition, shape::branch},
      std::pair{point::join_factory_transition, shape::source_coalesce},
      std::pair{point::join_factory_transition, shape::literal_coalesce},
      std::pair{point::join_edge_identity_transition,
                shape::transformed_binding},
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

[[nodiscard]] inline bool run_tracked_slice_failure_iteration(
    piece_edge source_root,
    piece_tree_arena& source_arena,
    factory_failure_source_state& source_state)
{
  const std::uint64_t live_baseline = live_piece_tree_arenas;
  const std::uint32_t arena_refcount_baseline = source_arena.refcount;
  const std::uint32_t source_retain_baseline = source_state.retain_calls;
  const std::uint32_t source_release_baseline = source_state.release_calls;
  auto* observation = static_cast<tracked_join_failure_observation*>(
      palloc0(sizeof(tracked_join_failure_observation)));
  MemoryContext build_context = AllocSetContextCreate(
      CurrentMemoryContext, "bioseq tracked slice failure",
      ALLOCSET_SMALL_SIZES);
  const MemoryContext saved_context = CurrentMemoryContext;
  bool volatile caught = false;
  bool volatile sqlstate_matched = false;
  observe_piece_tree_failure_noexcept(
      piece_tree_injected_failure_point::none);
  arm_piece_tree_failure_noexcept(
      piece_tree_injected_failure_point::split_first_leaf_intermediate);

  PG_TRY();
  {
    auto* guard = static_cast<tracked_join_failure_guard*>(
        MemoryContextAllocZero(
            build_context, sizeof(tracked_join_failure_guard)));
    guard->callback.func = tracked_join_failure_reset;
    guard->callback.arg = guard;
    guard->observation = observation;
    guard->armed = true;
    MemoryContextRegisterResetCallback(
        build_context, std::addressof(guard->callback));
    guard->destination_arena = make_piece_tree_arena();
    (void)slice_tree(
        *guard->destination_arena, guard->ledger,
        source_root, 1u, 3u);
    arm_piece_tree_failure_noexcept(
        piece_tree_injected_failure_point::none);
    MemoryContextDelete(build_context);
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(saved_context);
    arm_piece_tree_failure_noexcept(
        piece_tree_injected_failure_point::none);
    sqlstate_matched = consume_expected_error(
        ERRCODE_INTERNAL_ERROR, saved_context);
    MemoryContextDelete(build_context);
    caught = true;
  }
  PG_END_TRY();

  const bool restored = source_arena.refcount == arena_refcount_baseline
      && live_piece_tree_arenas == live_baseline
      && source_state.retain_calls > source_retain_baseline
      && source_state.release_calls > source_release_baseline
      && source_state.retain_calls - source_retain_baseline
          == source_state.release_calls - source_release_baseline;
  const bool passed = caught && sqlstate_matched
      && observed_piece_tree_failure_noexcept()
          == piece_tree_injected_failure_point::split_first_leaf_intermediate
      && observation->callbacks == 1u
      && observation->armed_callbacks == 1u
      && observation->tokens_released == 1u
      && observation->destination_tokens_released == 1u
      && observation->ledger_drained && restored;
  pfree(observation);
  return passed;
}

[[nodiscard]] inline bool run_tracked_slice_failure_matrix()
{
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
  const piece_edge source_root = make_source_node(
      *source_arena,
      source_piece{
          .source = source,
          .locator = {
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
  for (std::uint32_t iteration = 0u;
       iteration < repetitions && passed; ++iteration) {
    passed = run_tracked_slice_failure_iteration(
        source_root, *source_arena, source_state);
  }
  release_edge(source_root);
  release_arena(source_arena);
  return passed
      && source_state.retain_calls == source_state.release_calls
      && live_piece_tree_arenas == live_baseline;
}

[[nodiscard]] inline bool run_invalid_later_literal_binding_iteration(
    piece_edge source_root,
    piece_tree_arena& source_arena,
    compact_sequence_stdc_type_params bound_type)
{
  const std::uint64_t live_baseline = live_piece_tree_arenas;
  const std::uint32_t arena_refcount_baseline = source_arena.refcount;
  auto* observation = static_cast<tracked_join_failure_observation*>(
      palloc0(sizeof(tracked_join_failure_observation)));
  MemoryContext build_context = AllocSetContextCreate(
      CurrentMemoryContext, "bioseq invalid later literal binding",
      ALLOCSET_SMALL_SIZES);
  const MemoryContext saved_context = CurrentMemoryContext;
  bool volatile caught = false;
  bool volatile sqlstate_matched = false;

  PG_TRY();
  {
    auto* guard = static_cast<tracked_join_failure_guard*>(
        MemoryContextAllocZero(
            build_context, sizeof(tracked_join_failure_guard)));
    guard->callback.func = tracked_join_failure_reset;
    guard->callback.arg = guard;
    guard->observation = observation;
    guard->armed = true;
    MemoryContextRegisterResetCallback(
        build_context, std::addressof(guard->callback));
    guard->destination_arena = make_piece_tree_arena();
    (void)bind_literals_copy_error_atomic(
        *guard->destination_arena, source_root,
        bound_type, guard->ledger);
    MemoryContextDelete(build_context);
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(saved_context);
    sqlstate_matched = consume_expected_error(
        ERRCODE_INVALID_TEXT_REPRESENTATION, saved_context);
    MemoryContextDelete(build_context);
    caught = true;
  }
  PG_END_TRY();

  const bool passed = caught && sqlstate_matched
      && observation->callbacks == 1u
      && observation->armed_callbacks == 1u
      && observation->tokens_released == 1u
      && observation->destination_tokens_released == 1u
      && observation->ledger_drained
      && source_arena.refcount == arena_refcount_baseline
      && live_piece_tree_arenas == live_baseline;
  pfree(observation);
  return passed;
}

[[nodiscard]] inline bool run_invalid_later_literal_binding_matrix()
{
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
      *source_arena,
      factory_failure_unbound_literal(source_arena, storage, 0u, 2u));
  piece_edge invalid = make_literal_node(
      *source_arena,
      factory_failure_unbound_literal(source_arena, storage, 2u, 3u));
  const piece_edge source_root = make_branch_node(
      *source_arena, valid, invalid);
  bool passed = true;
  for (std::uint32_t iteration = 0u;
       iteration < repetitions && passed; ++iteration) {
    passed = run_invalid_later_literal_binding_iteration(
        source_root, *source_arena, bound_type);
  }
  release_edge(source_root);
  release_arena(source_arena);
  return passed && live_piece_tree_arenas == live_baseline;
}

[[nodiscard]] inline bool run_piece_tree_quarantine_contract_noexcept()
    noexcept
{
  piece_tree_quarantine_snapshot observed{};
  piece_tree_arena suspect{};
  suspect.refcount = 1u;
  suspect.generation = 77u;
  suspect.release_next = std::addressof(suspect);
  record_piece_tree_quarantine_noexcept(
      observed,
      piece_tree_quarantine_reason::arena_refcount,
      "negative arena quarantine",
      std::addressof(suspect));
  record_piece_tree_quarantine_noexcept(
      observed,
      piece_tree_quarantine_reason::cleanup_ledger,
      "negative ledger quarantine",
      std::addressof(suspect));
  return observed.invariant_failures == 2u
      && observed.quarantined_arenas == 1u
      && observed.first_reason
          == piece_tree_quarantine_reason::arena_refcount
      && observed.last_reason
          == piece_tree_quarantine_reason::cleanup_ledger
      && std::strcmp(observed.first_detail,
                     "negative arena quarantine") == 0
      && std::strcmp(observed.last_detail,
                     "negative ledger quarantine") == 0
      && observed.last_arena_generation == 77u
      && suspect.refcount == UINT32_MAX
      && suspect.release_next == nullptr;
}

} // namespace

/** Provide the SQL facade with this deterministic piece-tree contract. */
[[nodiscard]] inline bool run_piece_tree_error_atomic_matrix()
{
  return run_piece_tree_quarantine_contract_noexcept()
      && run_tracked_slice_failure_matrix()
      && run_invalid_later_literal_binding_matrix();
}

namespace {

struct binding_retirement_fixture {
  piece_tree_arena *arena;
  piece_edge root;
  std::uint64_t length;
  std::uint32_t arena_refcount_baseline;
  std::uint64_t live_baseline;
};

[[nodiscard]] inline binding_retirement_fixture
make_binding_retirement_fixture() {
  constexpr std::uint32_t leaf_count = 4'096u;
  constexpr std::uint64_t long_length = 17'000u;
  constexpr std::uint64_t short_length = 3'000u;
  auto *arena = make_piece_tree_arena();
  auto *storage = allocate_piece_literal_storage(*arena, long_length);
  std::fill_n(storage, long_length, std::byte{0x41});
  auto *level = static_cast<piece_edge *>(
      palloc0(sizeof(piece_edge) * leaf_count));
  const compact_sequence_stdc_type_params unbound_type{
      .alphabet_code = pgseq_unbound_alphabet_code,
      .storage_unit_byte_width = pgseq_unbound_width_tag,
  };
  std::uint64_t total_length = 0u;
  for (std::uint32_t index = 0u; index < leaf_count; ++index) {
    const std::uint64_t length = index % 5u == 0u
        ? long_length : short_length;
    total_length += length;
    level[index] = make_literal_node(
        *arena,
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
      level[index / 2u] = make_branch_node(
          *arena, level[index], level[index + 1u]);
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

struct binding_output_audit {
  std::uint64_t length;
  std::uint32_t leaves;
  bool exact;
};

template<class p_instance>
[[nodiscard]] inline binding_output_audit
audit_binding_output(piece_edge edge, std::uint8_t expected_rank) {
  if (!edge) return {.exact = true};
  if (edge.transform
      != bioseq::storage::sequence_transform::identity) {
    return {.exact = false};
  }
  if (edge.node->node_kind == piece_node::kind::branch) {
    const auto left = audit_binding_output<p_instance>(
        edge.node->payload.branch.left, expected_rank);
    const auto right = audit_binding_output<p_instance>(
        edge.node->payload.branch.right, expected_rank);
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
  bool exact = literal.encoding == literal_encoding::packed
      && literal.validation == literal_validation::alphabet_validated;
  for (std::uint64_t position = literal.symbol_from;
       exact && position < literal.symbol_to; ++position) {
    exact = packed_literal_rank_at<p_instance>(literal, position)
        == expected_rank;
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
      bound_type.alphabet_code, static_cast<unsigned char>('A'),
      expected_rank);

  auto *destination = make_piece_tree_arena();
  piece_edge_cleanup_ledger ledger{};
  tracked_piece_edge tracked = bind_literals_copy_error_atomic(
      *destination, fixture.root, bound_type, ledger);
  transfer_piece_edge_cleanup_root_noexcept(
      ledger, tracked.cleanup, tracked.edge);
  const bool ledger_empty = piece_edge_cleanup_ledger_empty_noexcept(ledger);
  validate_tree_and_limits(
      tracked.edge, bound_type, fixture.length, true);
  binding_output_audit output{};
  bioseq::pg_types::seq::dispatch_instance(
      bound_type.alphabet_code, bound_type.storage_unit_byte_width,
      [&output, tracked, expected_rank]<class p_instance>() {
        output = audit_binding_output<p_instance>(tracked.edge, expected_rank);
      });
  const std::uint32_t inline_capacity = static_cast<std::uint32_t>(
      destination->retired_join_roots.size());
  const std::uint32_t overflow_roots =
      destination->retired_join_root_count > inline_capacity
      ? destination->retired_join_root_count - inline_capacity : 0u;
  const std::uint32_t expected_blocks = overflow_roots == 0u ? 0u
      : (overflow_roots - 1u)
              / static_cast<std::uint32_t>(max_retired_join_roots)
          + 1u;
  const std::uint64_t component_bytes =
      static_cast<std::uint64_t>(destination->node_metadata_bytes)
      + destination->arena_dependency_metadata_bytes
      + destination->source_dependency_metadata_bytes
      + destination->literal_storage_bytes
      + destination->retired_join_root_metadata_bytes;
  const bool success = rank_found && ledger_empty && output.exact
      && output.length == fixture.length
      && output.leaves == tracked.edge.node->piece_count
      && fixture.length == 23'768'000u
      && destination->retired_join_rotation_count > 64u
      && destination->retired_join_rotation_count
          == destination->retired_join_root_count
      && destination->retired_join_root_overflow_block_count
          == expected_blocks
      && destination->retired_join_root_metadata_bytes
          == static_cast<Size>(expected_blocks)
              * MAXALIGN(sizeof(retired_join_root_block))
      && component_bytes == destination->accounted_bytes
      && destination->node_metadata_bytes
          == static_cast<Size>(destination->node_count)
              * MAXALIGN(sizeof(piece_node))
      && destination->arena_dependency_count == 0u
      && destination->source_dependency_count == 0u;
  release_edge(tracked.edge);
  release_arena(destination);

  auto *observation = static_cast<tracked_join_failure_observation *>(
      palloc0(sizeof(tracked_join_failure_observation)));
  MemoryContext build_context = AllocSetContextCreate(
      CurrentMemoryContext, "bioseq binding retirement failure",
      ALLOCSET_SMALL_SIZES);
  const MemoryContext saved_context = CurrentMemoryContext;
  bool volatile caught = false;
  bool volatile sqlstate_matched = false;
  observe_piece_tree_failure_noexcept(
      piece_tree_injected_failure_point::none);
  arm_piece_tree_failure_noexcept(
      piece_tree_injected_failure_point::
          join_retired_root_overflow_allocation);
  PG_TRY();
  {
    auto *guard = static_cast<tracked_join_failure_guard *>(
        MemoryContextAllocZero(
            build_context, sizeof(tracked_join_failure_guard)));
    guard->callback.func = tracked_join_failure_reset;
    guard->callback.arg = guard;
    guard->observation = observation;
    guard->armed = true;
    MemoryContextRegisterResetCallback(
        build_context, std::addressof(guard->callback));
    guard->destination_arena = make_piece_tree_arena();
    (void)bind_literals_copy_error_atomic(
        *guard->destination_arena, fixture.root, bound_type, guard->ledger);
    arm_piece_tree_failure_noexcept(
        piece_tree_injected_failure_point::none);
    MemoryContextDelete(build_context);
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(saved_context);
    arm_piece_tree_failure_noexcept(
        piece_tree_injected_failure_point::none);
    sqlstate_matched = consume_expected_error(
        ERRCODE_INTERNAL_ERROR, saved_context);
    MemoryContextDelete(build_context);
    caught = true;
  }
  PG_END_TRY();
  const bool unwind = caught && sqlstate_matched
      && observed_piece_tree_failure_noexcept()
          == piece_tree_injected_failure_point::
              join_retired_root_overflow_allocation
      && observation->callbacks == 1u
      && observation->armed_callbacks == 1u
      && observation->tokens_released != 0u
      && observation->destination_tokens_released == 1u
      && observation->ledger_drained
      && fixture.arena->refcount == fixture.arena_refcount_baseline
      && live_piece_tree_arenas == fixture.live_baseline;
  pfree(observation);
  release_edge(fixture.root);
  release_arena(fixture.arena);
  return success && unwind;
}

namespace {

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

} // namespace

/** Provide the SQL facade with this deterministic piece-tree contract. */
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

namespace {

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

namespace {

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

} // namespace

/** Provide the SQL facade with this deterministic piece-tree contract. */
[[nodiscard]] inline bool run_corrupt_tree_length_guardrail_matrix() {
  return corrupt_tree_length_raises_data_corrupted(true)
      && corrupt_tree_length_raises_data_corrupted(false);
}

namespace {

[[nodiscard]] inline bool
trusted_join_dependency_mismatch_raises_data_corrupted() {
  auto *backing = make_piece_tree_arena();
  auto *wrong_backing = make_piece_tree_arena();
  auto *destination = make_piece_tree_arena();
  auto *storage = allocate_piece_literal_storage(*backing, 1u);
  storage[0] = std::byte{0x41};
  const compact_sequence_stdc_type_params unbound_type{
      .alphabet_code = pgseq_unbound_alphabet_code,
      .storage_unit_byte_width = pgseq_unbound_width_tag,
  };
  const piece_edge trusted = make_literal_node(
      *backing, factory_failure_unbound_literal(backing, storage, 0u, 1u));
  const piece_edge result = make_literal_node(
      *destination,
      factory_failure_unbound_literal(backing, storage, 0u, 1u));
  (void)validate_trusted_join_and_limits(
      *destination, result, trusted, 1u, {}, 0u,
      unbound_type, 1u, false);

  auto *dependency = destination->arena_dependencies;
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
        *destination, result, trusted, 1u, {}, 0u,
        unbound_type, 1u, false);
  }
  PG_CATCH();
  {
    matched = consume_expected_error(
        ERRCODE_DATA_CORRUPTED, saved_context);
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

[[nodiscard]] inline bool
trusted_join_shared_node_raises_data_corrupted() {
  auto *arena = make_piece_tree_arena();
  auto *storage = allocate_piece_literal_storage(*arena, 1u);
  storage[0] = std::byte{0x43};
  const compact_sequence_stdc_type_params unbound_type{
      .alphabet_code = pgseq_unbound_alphabet_code,
      .storage_unit_byte_width = pgseq_unbound_width_tag,
  };
  const literal_piece literal = factory_failure_unbound_literal(
      arena, storage, 0u, 1u);
  const piece_edge shared = make_literal_node(*arena, literal);
  const piece_edge unreachable = make_literal_node(*arena, literal);
  const piece_edge result = make_branch_node(
      *arena, retain_edge(shared), retain_edge(shared));

  const MemoryContext saved_context = CurrentMemoryContext;
  bool volatile matched = false;
  PG_TRY();
  {
    (void)validate_trusted_join_and_limits(
        *arena, result, shared, 1u, shared, 1u,
        unbound_type, 2u, false);
  }
  PG_CATCH();
  {
    matched = consume_expected_error(
        ERRCODE_DATA_CORRUPTED, saved_context);
  }
  PG_END_TRY();
  release_edge(result);
  release_edge(shared);
  release_edge(unreachable);
  release_arena(arena);
  return matched;
}

[[nodiscard]] inline bool
trusted_join_forged_retirement_raises_data_corrupted() {
  auto *arena = make_piece_tree_arena();
  auto *storage = allocate_piece_literal_storage(*arena, 2u);
  storage[0] = std::byte{0x47};
  storage[1] = std::byte{0x54};
  const compact_sequence_stdc_type_params unbound_type{
      .alphabet_code = pgseq_unbound_alphabet_code,
      .storage_unit_byte_width = pgseq_unbound_width_tag,
  };
  const piece_edge first = make_literal_node(
      *arena,
      factory_failure_unbound_literal(arena, storage, 0u, 1u));
  const piece_edge second = make_literal_node(
      *arena,
      factory_failure_unbound_literal(arena, storage, 1u, 2u));
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
        *arena, result, result, 2u, {}, 0u,
        unbound_type, 2u, false);
  }
  PG_CATCH();
  {
    matched = consume_expected_error(
        ERRCODE_DATA_CORRUPTED, saved_context);
  }
  PG_END_TRY();
  arena->retired_join_roots[0] = {};
  arena->retired_join_root_count = 0u;
  arena->retired_join_rotation_count = 0u;

  const auto& published = result.node->payload.branch;
  const piece_edge retired = make_branch_node(
      *arena, retain_edge(published.left), retain_edge(published.right));
  arena->retired_join_roots[0] = retired;
  arena->retired_join_root_count = 1u;
  arena->retired_join_rotation_count = 1u;
  release_edge(retired);
  const auto valid_audit = validate_trusted_join_and_limits(
      *arena, result, result, 2u, {}, 0u,
      unbound_type, 2u, false);
  const bool valid_retirement = valid_audit.new_nodes_visited
      == arena->node_count;

  arena->retired_join_roots[1] = arena->retired_join_roots[0];
  arena->retired_join_root_count = 2u;
  arena->retired_join_rotation_count = 2u;
  bool volatile duplicate_matched = false;
  PG_TRY();
  {
    (void)validate_trusted_join_and_limits(
        *arena, result, result, 2u, {}, 0u,
        unbound_type, 2u, false);
  }
  PG_CATCH();
  {
    duplicate_matched = consume_expected_error(
        ERRCODE_DATA_CORRUPTED, saved_context);
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

[[nodiscard]] inline bool
trusted_join_source_mismatch_raises_data_corrupted() {
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
  auto *arena = make_piece_tree_arena();
  const piece_edge result = make_source_node(
      *arena,
      source_piece{
          .source = source,
          .locator = locator,
          .stored_type_params = bound_type,
          .last_padding_letters = 0u,
          .from = 0u,
          .to = 1u,
      });
  (void)validate_trusted_join_and_limits(
      *arena, result, result, 1u, {}, 0u,
      bound_type, 1u, true);

  auto *dependency = arena->source_dependencies;
  if (dependency == nullptr
      || dependency->source.state != source.state
      || dependency->source.ops != source.ops) {
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
        *arena, result, result, 1u, {}, 0u,
        bound_type, 1u, true);
  }
  PG_CATCH();
  {
    matched = consume_expected_error(
        ERRCODE_DATA_CORRUPTED, saved_context);
  }
  PG_END_TRY();
  dependency->source = source;
  release_edge(result);
  release_arena(arena);
  return matched && source_state.retain_calls == 1u
      && source_state.release_calls == 1u
      && wrong_source_state.retain_calls == 0u
      && wrong_source_state.release_calls == 0u;
}

} // namespace

/** Provide the SQL facade with this deterministic piece-tree contract. */
[[nodiscard]] inline bool run_trusted_join_corruption_guardrail_matrix() {
  return trusted_join_dependency_mismatch_raises_data_corrupted()
      && trusted_join_shared_node_raises_data_corrupted()
      && trusted_join_forged_retirement_raises_data_corrupted()
      && trusted_join_source_mismatch_raises_data_corrupted();
}

namespace {

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
    bioseq::storage::sequence_locator locator,
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
 * the rotation. Counter and arena checks prove dependency ownership balances.
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
  const bioseq::storage::sequence_locator locator{
      .fragment_count = 1u,
      .logical_length = source_state.rank_count,
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
  tracked_piece_edge tracked_joined = left_heavy
      ? rebalance_joined_edges(
            *destination_arena, nullptr,
            {.edge = take_rotation_edge(heavy)},
            {.edge = take_rotation_edge(light)},
            retired_join_root_policy::fixed_trusted_join)
      : rebalance_joined_edges(
            *destination_arena, nullptr,
            {.edge = take_rotation_edge(light)},
            {.edge = take_rotation_edge(heavy)},
            retired_join_root_policy::fixed_trusted_join);
  piece_edge joined = tracked_joined.edge;
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
} // namespace

/** Provide the SQL facade with this deterministic piece-tree contract. */
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
