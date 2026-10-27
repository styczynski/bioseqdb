/**
 * @file packed_output.cppm
 * Export sequence packing, tracing, and ownership helpers with SQL test functions.
 */
module;

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

#include <bioseq/pg_compat.h>
extern "C" {
#include "storage/shmem.h"
#include "utils/datum.h"
#include "utils/expandeddatum.h"
}

export module bioseq.postgres.types.compact_sequence:packed_output;

import bioseq.codegen.sql.annotation_model;
import :flat_abi;
import :source_abi;
import :tree_types;
import :tree_ops;
import :view_session;
import bioseq.postgres.support;
import bioseq.parallelism.pool;
import bioseq.sequences.rank_pack;
import bioseq.storage.coordinates;
import bioseq.storage.lazy_sequence;
import bioseq.storage.layout;

export import :packed_output_materialization_trace;
export import :packed_output_async_ownership;
export import :packed_output_packing;
export import :packed_output_test_contracts;

export namespace bioseq::pg_types::sql_routines {

/// Test sequence-copy tracing  and ownership during asynchronous work.

/**
 * ```sql
 * -- Run this internal diagnostic as the extension owner in a disposable test session.
 * SELECT bioseq_internal.slice_trace_self_test();
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::execute,
    .from = bioseq::sql::public_role,
})]]
bool slice_trace_self_test() noexcept {
  return slice_trace_self_test_noexcept();
}

/// Read or reset test-only payload-allocation counters.

/**
 * ```sql
 * -- Read the packed-full payload allocation counter in a BIOSEQ_TEST_INDEXES build as the extension owner; action 1 leaves counters unchanged.
 * SELECT bioseq_internal.slice_payload_allocation_probe(1);
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .parallel = bioseq::sql::parallel_safety::restricted,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::execute,
    .from = bioseq::sql::public_role,
})]]
std::int64_t slice_payload_allocation_probe(std::int32_t action) {
#ifdef BIOSEQ_TEST_INDEXES
  auto& counters = slice_payload_allocation_test_storage_noexcept();
  switch (action) {
    case 0:
      counters = {};
      return 0;
    case 1:
      return static_cast<std::int64_t>(std::min(
          counters.packed_full,
          static_cast<std::uint64_t>(
              (std::numeric_limits<std::int64_t>::max)())));
    case 2:
      return static_cast<std::int64_t>(std::min(
          counters.executor_copy,
          static_cast<std::uint64_t>(
              (std::numeric_limits<std::int64_t>::max)())));
    case 3:
      return static_cast<std::int64_t>(std::min(
          counters.publication_packed_full,
          static_cast<std::uint64_t>(
              (std::numeric_limits<std::int64_t>::max)())));
    case 4:
      return static_cast<std::int64_t>(std::min(
          counters.publication_executor_copy,
          static_cast<std::uint64_t>(
              (std::numeric_limits<std::int64_t>::max)())));
    default:
      ereport(ERROR,
              (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
               errmsg("invalid slice payload-allocation probe action"),
               errdetail("expected 0 (reset), 1 (packed full), or 2 "
                         "(executor copy), 3 (publication packed full), or 4 "
                         "(publication executor copy); received %d", action)));
  }
#else
  ereport(ERROR,
          (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
           errmsg("slice payload-allocation probe is unavailable"),
           errdetail("The extension was built without BIOSEQ_TEST_INDEXES.")));
#endif
  pg_unreachable();
}

/// Raise a test error in a parallel worker to check that cleanup preserves it.

/**
 * ```sql
 * -- In a BIOSEQ_TEST_INDEXES build, the probe returns true in the backend and raises its injected error if PostgreSQL executes it in a parallel worker.
 * SELECT bioseq_internal.slice_trace_worker_failure_probe(1);
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::execute,
    .from = bioseq::sql::public_role,
})]]
bool slice_trace_worker_failure_probe(std::int32_t action) {
#ifdef BIOSEQ_TEST_INDEXES
  static_cast<void>(action);
  if (IsParallelWorker()) {
    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("injected compact-sequence parallel worker failure"),
             errdetail("the original worker ERROR must survive DSM cleanup")));
  }
  return true;
#else
  static_cast<void>(action);
  ereport(ERROR,
          (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
           errmsg("SliceProject worker failure probe is unavailable"),
           errdetail("The extension was built without BIOSEQ_TEST_INDEXES.")));
#endif
  pg_unreachable();
}

}  // namespace bioseq::pg_types::sql_routines
/* This test source produces DNA4 rank zero on demand. It stays alive for
 * the process, so tests can check very large sequence lengths without
 * allocating all their letters. */
export namespace bioseq::pg_types::sql_routines {

/// Create a test sequence of the requested length whose letters are all DNA4 A.

/**
 * ```sql
 * -- Create a large lazy test sequence and inspect its logical length without displaying all letters. This is an internal diagnostic.
 * SELECT bioseq.len(bioseq_internal.synthetic_lazy_sequence(1000000));
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::restricted,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::execute,
    .from = bioseq::sql::public_role,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
})]]
Datum synthetic_lazy_sequence(std::int64_t requested) {
  if (requested < 0) {
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("synthetic bioseq length must be nonnegative")));
  }
  constexpr compact_sequence_stdc_type_params type{
      .alphabet_code = pgseq_supported_alphabets::index_of<::seq::dna4>(),
      .storage_unit_byte_width = 8u,
  };
  const auto length = static_cast<std::uint64_t>(requested);
  return make_source_backed_pg_sequence(
      synthetic_lazy_source_handle(),
      bioseq::storage::sequence_locator{
          .payload_version = bioseq::storage::sequence_locator_payload_version,
          .bucket_index = 0u,
          .reserved_16 = 0u,
          .shard_index = 0u,
          .fragment_count = length == 0u ? 0u : 1u,
          .encoded_payload_bytes = 0u,
          .logical_length = length,
          .first_fragment_row = 0u,
          .source_epoch = 1u,
      },
      type, 0u);
}

/// Test flat sequence sizes and llocation limits.

/**
 * ```sql
 * -- Run this internal diagnostic as the extension owner in a disposable test session.
 * SELECT bioseq_internal.materialization_size_self_test();
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::execute,
    .from = bioseq::sql::public_role,
})]]
bool materialization_size_self_test() {
  return run_materialization_size_guardrail_matrix();
}

/// Count the leaf pieces in a sequence's tree.

/**
 * ```sql
 * -- Inspect how many pieces a lazily concatenated sequence contains.
 * SELECT bioseq_internal.sequence_piece_count('ACGT'::bioseq.seq('dna4', 64) OPERATOR(bioseq.||) 'TG'::text);
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::restricted,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::execute,
    .from = bioseq::sql::public_role,
})]]
std::int64_t sequence_piece_count(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum input) {
  const auto value = acquire_sequence_for_read(input);
  return value.root()
      ? static_cast<std::int64_t>(value.root().node->piece_count)
      : 0;
}

/* datumTransfer moves a writable  expanded value to the destination memory
 * context. datumCopy copies a read-only value into independent flat storage.
 * Both results must survive a reset of their original memory context. */
/// Test that transferred and copied sequences survive resets of their original contexts.

/**
 * ```sql
 * -- Run this internal diagnostic as the extension owner in a disposable test session.
 * SELECT bioseq_internal.expanded_ownership_self_test();
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::execute,
    .from = bioseq::sql::public_role,
})]]
bool expanded_ownership_self_test() {
  MemoryContext saved = CurrentMemoryContext;
  MemoryContext volatile transfer_input = nullptr;
  MemoryContext volatile transfer_parent = nullptr;
  MemoryContext volatile copy_input = nullptr;
  MemoryContext volatile copy_parent = nullptr;
  bool volatile valid = false;
  const std::uint64_t arena_baseline = piece_tree_live_arena_count_noexcept();
  const bool concat_length_guardrail_matrix =
      run_concat_length_guardrail_matrix();
  const bool public_concat_path_copy =
      run_public_concat_path_copy_probe();
#ifdef BIOSEQ_TEST_INDEXES
  const bool read_session_parent_reset =
      pg_sequence_read_session_parent_reset_self_test();
#else
  const bool read_session_parent_reset = true;
#endif
  PG_TRY();
  {
    transfer_input = AllocSetContextCreate(saved, "bioseq EOH transfer input",
                                           ALLOCSET_SMALL_SIZES);
    transfer_parent = AllocSetContextCreate(saved, "bioseq EOH transfer parent",
                                            ALLOCSET_SMALL_SIZES);
    copy_input = AllocSetContextCreate(saved, "bioseq EOH copy input",
                                       ALLOCSET_SMALL_SIZES);
    copy_parent = AllocSetContextCreate(saved, "bioseq EOH copy parent",
                                        ALLOCSET_SMALL_SIZES);
    constexpr compact_sequence_stdc_type_params unbound_type{
        .alphabet_code = pgseq_unbound_alphabet_code,
        .storage_unit_byte_width = pgseq_unbound_width_tag,
    };
    const auto make_probe = [unbound_type](MemoryContext input,
                                           std::array<std::byte, 3> bytes) {
      MemoryContext previous = MemoryContextSwitchTo(input);
      auto *value = make_pg_sequence_expanded(unbound_type, false);
      replace_root_error_safe(
          *value, unbound_type, bytes.size(), false,
          [bytes](piece_tree_arena &arena,
                  piece_edge_cleanup_ledger &ledger) {
            auto *storage = allocate_piece_literal_storage(arena, bytes.size());
            std::memcpy(storage, bytes.data(), bytes.size());
            return make_literal_node_error_atomic(
                arena, ledger,
                literal_piece{
                    .storage_arena = std::addressof(arena),
                    .bytes = storage,
                    .symbol_from = 0u,
                    .symbol_to = bytes.size(),
                    .stored_type_params =
                        {
                            .alphabet_code = pgseq_unbound_alphabet_code,
                            .storage_unit_byte_width = pgseq_unbound_width_tag,
                        },
                    .encoding = literal_encoding::unbound_ascii,
                    .validation = literal_validation::unvalidated,
                });
          });
      MemoryContextSwitchTo(previous);
      return value;
    };

    auto *transferred_value =
        make_probe(const_cast<MemoryContext>(transfer_input),
                   {std::byte{0x41}, std::byte{0x43}, std::byte{0x47}});
    MemoryContextSwitchTo(const_cast<MemoryContext>(transfer_parent));
    const Datum transferred =
        datumTransfer(pg_sequence_rw_datum(transferred_value), false, -1);
    MemoryContextSwitchTo(saved);
    const bool reparented =
        get_pg_sequence_expanded_rw(transferred) == transferred_value &&
        MemoryContextGetParent(transferred_value->object_context) ==
            transfer_parent;
    MemoryContextReset(const_cast<MemoryContext>(transfer_input));
    std::array<std::byte, 3> transferred_text{};
    const bool transfer_survived =
        emit_pg_unbound_ascii(transferred_value->root, 0u,
                              transferred_text.size(), transferred_text) &&
        transferred_text == std::array<std::byte, 3>{std::byte{0x41},
                                                     std::byte{0x43},
                                                     std::byte{0x47}};

    auto *copied_value =
        make_probe(const_cast<MemoryContext>(copy_input),
                   {std::byte{0x54}, std::byte{0x47}, std::byte{0x41}});
    const Datum read_only = MakeExpandedObjectReadOnly(
        pg_sequence_rw_datum(copied_value), false, -1);
    MemoryContextSwitchTo(const_cast<MemoryContext>(copy_parent));
    const Datum copied = datumCopy(read_only, false, -1);
    MemoryContextSwitchTo(saved);
    MemoryContextReset(const_cast<MemoryContext>(copy_input));
    auto const *copied_flat =
        reinterpret_cast<bioseq::pg_types::seq const *>(DatumGetPointer(copied));
    const bool copy_survived =
        !VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(copied)) &&
        bioseq::postgres::support::varlena_4byte_size(copied_flat) >=
            bioseq::pg_types::seq::header_bytes() + transferred_text.size() &&
        copied_flat->alphabet_code == pgseq_unbound_alphabet_code &&
        copied_flat->storage_unit_byte_width == pgseq_unbound_width_tag &&
        copied_flat->bp_size == transferred_text.size() &&
        std::memcmp(copied_flat->data, "TGA", transferred_text.size()) == 0;
    valid = concat_length_guardrail_matrix && public_concat_path_copy
        && read_session_parent_reset
        && reparented && transfer_survived && copy_survived;

    MemoryContextDelete(const_cast<MemoryContext>(copy_input));
    copy_input = nullptr;
    MemoryContextDelete(const_cast<MemoryContext>(transfer_input));
    transfer_input = nullptr;
    MemoryContextDelete(const_cast<MemoryContext>(copy_parent));
    copy_parent = nullptr;
    MemoryContextDelete(const_cast<MemoryContext>(transfer_parent));
    transfer_parent = nullptr;
    valid = valid && piece_tree_live_arena_count_noexcept() == arena_baseline;
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(saved);
    if (copy_input != nullptr) {
      MemoryContextDelete(const_cast<MemoryContext>(copy_input));
    }
    if (transfer_input != nullptr) {
      MemoryContextDelete(const_cast<MemoryContext>(transfer_input));
    }
    if (copy_parent != nullptr) {
      MemoryContextDelete(const_cast<MemoryContext>(copy_parent));
    }
    if (transfer_parent != nullptr) {
      MemoryContextDelete(const_cast<MemoryContext>(transfer_parent));
    }
    FlushErrorState();
    valid = false;
  }
  PG_END_TRY();
  MemoryContextSwitchTo(saved);
  return valid;
}

}  // namespace bioseq::pg_types::sql_routines
