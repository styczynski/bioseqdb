/**
 * @file tree_ops.cppm
 * @brief Export piece-tree operations and provide their SQL self-test.
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

export module bioseq.postgres.types.compact_sequence:tree_ops;

import bioseq.codegen.sql.annotation_model;
import :flat_abi;
import :source_abi;
import :tree_types;
import bioseq.sequences.rank_pack;
import bioseq.storage.coordinates;
import bioseq.storage.layout;

export import :tree_ops_ownership;
export import :tree_ops_transforms;
export import :tree_ops_validation;
export import :tree_ops_failure_contracts;


/* SQL self-test for piece-tree construction and cleanup. Expected errors are
 * caught so later checks can run. Tests of corrupt  cleanup state use local
 * records to keep the backend state usable. */
export namespace bioseq::pg_types::sql_routines {

/// @brief Check piece-tree construction, balancing, and cleanup after failures.
/// @return `true` when all checks pass and no arenas or cleanup failures remain.

/**
 * ```sql
 * -- Run this internal diagnostic as the extension owner in a disposable test session.
 * SELECT bioseq_internal.piece_tree_self_test();
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::execute,
    .from = bioseq::sql::public_role,
})]]
bool piece_tree_self_test() {
  bool volatile saturation_caught = false;
  bool volatile budget_caught = false;
  bool volatile generation_caught = false;
  bool volatile million_piece_balanced = false;
  bool volatile deep_release_iterative = false;
  const std::uint64_t arena_baseline = live_piece_tree_arenas;
  const auto quarantine_baseline = read_piece_tree_quarantine_noexcept();
  const bool clean_entry = arena_baseline == 0u
      && quarantine_baseline.invariant_failures == 0u;
  const bool factory_failure_matrix =
      run_piece_tree_factory_failure_matrix();
  const bool tracked_join_failure_matrix =
      run_tracked_join_failure_matrix();
  const bool error_atomic_matrix =
      run_piece_tree_error_atomic_matrix();
  const bool binding_retirement_overflow =
      run_binding_retirement_overflow_regression();
  const bool tree_length_guardrail_matrix =
      run_tree_length_add_guardrail_matrix();
  const bool arena_alignment_guardrail_matrix =
      run_arena_alignment_guardrail_matrix();
  const bool corrupt_tree_length_guardrail_matrix =
      run_corrupt_tree_length_guardrail_matrix();
  const bool trusted_join_corruption_guardrail_matrix =
      run_trusted_join_corruption_guardrail_matrix();
  const bool transformed_join_rotation =
      run_transformed_join_rotation_regression();

  auto *saturation_arena = make_piece_tree_arena();
  saturation_arena->refcount = std::numeric_limits<std::uint32_t>::max();
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
      /* All edges in this test use the same arena. PostgreSQL ERROR can skip
       * their C++ cleanup, leaving extra references behind. No other arena
       * depends on these nodes, so discard those references before freeing it. */
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
      clean_entry && saturation_caught && budget_caught && generation_caught &&
      factory_failure_matrix && tracked_join_failure_matrix &&
      error_atomic_matrix &&
      binding_retirement_overflow &&
      tree_length_guardrail_matrix &&
      arena_alignment_guardrail_matrix &&
      corrupt_tree_length_guardrail_matrix &&
      trusted_join_corruption_guardrail_matrix &&
      transformed_join_rotation &&
      million_piece_balanced && deep_release_iterative &&
      live_piece_tree_arenas == arena_baseline &&
      read_piece_tree_quarantine_noexcept().invariant_failures
          == quarantine_baseline.invariant_failures;
  if (!all_checks_passed) {
    ereport(WARNING,
            (errmsg_internal(
                "bioseq piece-tree self-test flags: clean_entry=%d saturation=%d budget=%d generation=%d factory=%d tracked_join=%d error_atomic=%d binding_retirement=%d length=%d alignment=%d corrupt=%d trusted=%d transformed_join=%d million=%d deep=%d arena_balance=%d",
                clean_entry ? 1 : 0,
                saturation_caught ? 1 : 0,
                budget_caught ? 1 : 0,
                generation_caught ? 1 : 0,
                factory_failure_matrix ? 1 : 0,
                tracked_join_failure_matrix ? 1 : 0,
                error_atomic_matrix ? 1 : 0,
                binding_retirement_overflow ? 1 : 0,
                tree_length_guardrail_matrix ? 1 : 0,
                arena_alignment_guardrail_matrix ? 1 : 0,
                corrupt_tree_length_guardrail_matrix ? 1 : 0,
                trusted_join_corruption_guardrail_matrix ? 1 : 0,
                transformed_join_rotation ? 1 : 0,
                million_piece_balanced ? 1 : 0,
                deep_release_iterative ? 1 : 0,
                live_piece_tree_arenas == arena_baseline ? 1 : 0)));
  }
  return all_checks_passed;
}

}  // namespace bioseq::pg_types::sql_routines
