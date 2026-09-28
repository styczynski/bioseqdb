/**
 * @file admin/generation_fixture_write.cppm
 * @brief Build stored minimizer tpst data and check publication failures.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/relation.h"
#include "access/xact.h"
#include "commands/tablespace.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "utils/builtins.h"
#include "utils/errcodes.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <libassert/assert.hpp>
export module bioseq.postgres.storage.indexing.admin:generation_fixture_write;

import :generation_api;
import :grouped_soa;
import :hash_major;
import bioseq.common.logging;
import bioseq.postgres.storage.indexing.admin.diagnostic_support;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.support;
import bioseq.postgres.storage.manager; // seq_caches, sequence_locator, is_locator_stub, decode_locator_from_stub
import bioseq.storage.layout;
import bioseq.storage.scanner;                 // sequence access requirements
import bioseq.postgres.storage.scanner;        // with_postgres_lazy_sequence
import bioseq.postgres.types.compact_sequence; // bioseq::pg_types::seq, dispatch_instance
import bioseq.indexing.type;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.postgres.storage.indexing.chunked_storage;
#ifdef BIOSEQ_TEST_INDEXES
import bioseq.postgres.storage.indexing.sorted_runs;
#endif
import bioseq.postgres.table.io;
import bioseq.postgres.storage.indexing.fragment_handle;
import bioseq.postgres.storage.indexing.index_relation_identity;
import bioseq.postgres.column.table_access;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.indexing.admin.ready_minimizer_fixture_kernel;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::storage::indexing {

[[noreturn]] void ready_minimizer_fixture_backend_error_as_cpp(std::string_view name) {
    bioseq::postgres::support::invoke_pg_as_cpp([name] {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("READY minimizer fixture backend is invalid"),
                 errdetail("backend=%.*s", static_cast<int>(name.size()), name.data()),
                 errhint("Use scalar, exact_simd, bounded_nthash_simd, or vulkan_segment.")));
    });
    pg_unreachable();
}

const ready_minimizer_generation_publication&
ready_minimizer_generation_coordinator_fixture_result::sealed_publication() const noexcept {
    DEBUG_ASSERT(publication.has_value(),
                 "READY fixture publication must be sealed before return");
    return *publication;
}

/// Minimizer backend and failure settings selected  for this test build
struct ready_minimizer_fixture_backend_request {
    bioseq::minimizers::minimizer_sketch_backend backend =
        bioseq::minimizers::minimizer_sketch_backend::scalar;
    minimizer_generation_failure_injection failure = minimizer_generation_failure_injection::none;
#ifdef BIOSEQ_TEST_INDEXES
    bioseq::storage::scanner_leaf_failure scanner_failure =
        bioseq::storage::scanner_leaf_failure::none;
    sorted_run_failure_injection sorted_run_failure = sorted_run_failure_injection::none;
    std::uint64_t sorted_run_failure_operation = 1u;
    std::uint64_t sorted_run_merge_reader_budget_bytes = 0u;
    sorted_run_reader_admission_injection sorted_run_reader_admission =
        sorted_run_reader_admission_injection::none;
    std::uint64_t sorted_run_reader_request_ceiling_bytes = 0u;
#endif
};

/// Parse a backend name or a named test-failure scenario.
[[nodiscard]] inline ready_minimizer_fixture_backend_request
parse_ready_minimizer_fixture_backend_request(std::string_view name) {
#ifdef BIOSEQ_TEST_INDEXES
    using namespace std::string_view_literals;
    using scanner_failure = bioseq::storage::scanner_leaf_failure;
    constexpr std::array k_scanner_failures{
        std::pair{"scalar_fail_scanner_interrupt_before"sv, scanner_failure::interrupt_before},
        std::pair{"scalar_fail_scanner_interrupt_after"sv, scanner_failure::interrupt_after},
        std::pair{"scalar_fail_scanner_row_read_before"sv, scanner_failure::row_read_before},
        std::pair{"scalar_fail_scanner_row_read_after"sv, scanner_failure::row_read_after},
        std::pair{"scalar_fail_scanner_palloc_before"sv, scanner_failure::palloc_before},
        std::pair{"scalar_fail_scanner_palloc_after"sv, scanner_failure::palloc_after},
    };
    for (const auto& [backend_name, failure] : k_scanner_failures) {
        if (name == backend_name) {
            return {
                .backend = bioseq::minimizers::minimizer_sketch_backend::scalar,
                .scanner_failure = failure,
            };
        }
    }
    if (name == "scalar_fail_sorted_compact_create_before_nth2") {
        return {
            .backend = bioseq::minimizers::minimizer_sketch_backend::scalar,
            .sorted_run_failure = sorted_run_failure_injection::compact_create_before,
            .sorted_run_failure_operation = 2u,
            // Limit the merge to two readers so the second output is attempted
            // after the first output has closed its input runs.
            .sorted_run_merge_reader_budget_bytes = std::uint64_t{64} << 10,
        };
    }
    if (name == "scalar_test_sorted_reader_backoff") {
        return {
            .backend = bioseq::minimizers::minimizer_sketch_backend::scalar,
            .sorted_run_merge_reader_budget_bytes = std::uint64_t{4} << 20,
            // Make a three-input merge exceed the test budget while two inputs fit.
            // This tests smaller reader buffers and merging in several passes.
            .sorted_run_reader_admission =
                sorted_run_reader_admission_injection::request_ceiling,
            .sorted_run_reader_request_ceiling_bytes = std::uint64_t{160} << 10,
        };
    }
    if (name == "scalar_fail_sorted_reader_minimum") {
        return {
            .backend = bioseq::minimizers::minimizer_sketch_backend::scalar,
            .sorted_run_merge_reader_budget_bytes = std::uint64_t{4} << 20,
            // Make even one minimum-size reader exceed the test reservation limit.
            .sorted_run_reader_admission =
                sorted_run_reader_admission_injection::request_ceiling,
            .sorted_run_reader_request_ceiling_bytes = std::uint64_t{64} << 10,
        };
    }
    if (name == "scalar_fail_sorted_reader_system_pressure") {
        return {
            .backend = bioseq::minimizers::minimizer_sketch_backend::scalar,
            // Refuse, one reservation because of system memory pressure. The  build
            // must stop without retrying with smaller reader buffers.
            .sorted_run_reader_admission =
                sorted_run_reader_admission_injection::system_pressure_once,
        };
    }
    constexpr std::array k_sorted_run_failures{
        std::pair{"scalar_fail_sorted_create_before"sv,
                  sorted_run_failure_injection::create_before},
        std::pair{"scalar_fail_sorted_create_after"sv, sorted_run_failure_injection::create_after},
        std::pair{"scalar_fail_sorted_write_before"sv, sorted_run_failure_injection::write_before},
        std::pair{"scalar_fail_sorted_write_after"sv, sorted_run_failure_injection::write_after},
        std::pair{"scalar_fail_sorted_read_before"sv, sorted_run_failure_injection::read_before},
        std::pair{"scalar_fail_sorted_read_after"sv, sorted_run_failure_injection::read_after},
        std::pair{"scalar_fail_sorted_seek_before"sv, sorted_run_failure_injection::seek_before},
        std::pair{"scalar_fail_sorted_seek_after"sv, sorted_run_failure_injection::seek_after},
        std::pair{"scalar_fail_sorted_size_before"sv, sorted_run_failure_injection::size_before},
        std::pair{"scalar_fail_sorted_size_after"sv, sorted_run_failure_injection::size_after},
        std::pair{"scalar_fail_sorted_close_before"sv, sorted_run_failure_injection::close_before},
        std::pair{"scalar_fail_sorted_close_after"sv, sorted_run_failure_injection::close_after},
        std::pair{"scalar_fail_sorted_interrupt_before"sv,
                  sorted_run_failure_injection::interrupt_before},
        std::pair{"scalar_fail_sorted_interrupt_after"sv,
                  sorted_run_failure_injection::interrupt_after},
    };
    for (const auto& [backend_name, failure] : k_sorted_run_failures) {
        if (name == backend_name) {
            return {
                .backend = bioseq::minimizers::minimizer_sketch_backend::scalar,
                .sorted_run_failure = failure,
            };
        }
    }
    if (name == "scalar_fail_pg_spill") {
        return {
            .backend = bioseq::minimizers::minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::minimizer_sequence_spill,
        };
    }
    if (name == "scalar_fail_pg_merge") {
        return {
            .backend = bioseq::minimizers::minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::minimizer_sequence_merge,
        };
    }
    if (name == "scalar_fail_pg_tts_create") {
        return {
            .backend = bioseq::minimizers::minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::generation_tts_create,
        };
    }
    if (name == "scalar_fail_pg_tuple_form") {
        return {
            .backend = bioseq::minimizers::minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::generation_tuple_form,
        };
    }
    if (name == "scalar_fail_pg_tts_store") {
        return {
            .backend = bioseq::minimizers::minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::generation_tts_store,
        };
    }
    if (name == "scalar_fail_pg_tts_drop") {
        return {
            .backend = bioseq::minimizers::minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::generation_tts_drop,
        };
    }
    if (name == "scalar_fail_pg_multi_insert") {
        return {
            .backend = bioseq::minimizers::minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::generation_multi_insert,
        };
    }
    if (name == "scalar_fail_pg_reread") {
        return {
            .backend = bioseq::minimizers::minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::generation_reread,
        };
    }
    if (name == "scalar_fail_pg_spi_publish") {
        return {
            .backend = bioseq::minimizers::minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::generation_spi_publish,
        };
    }
    if (name == "scalar_fail_pg_writer_close") {
        return {
            .backend = bioseq::minimizers::minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::generation_writer_close,
        };
    }
    if (name == "scalar_fail_pg_storage_close") {
        return {
            .backend = bioseq::minimizers::minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::generation_storage_close,
        };
    }
    if (name == "scalar_fail_pg_post_publish_reread") {
        return {
            .backend = bioseq::minimizers::minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::generation_post_publish_reread,
        };
    }
    if (name == "scalar_fail_pg_reader_close") {
        return {
            .backend = bioseq::minimizers::minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::generation_reader_close,
        };
    }
#endif
    return {
        .backend = ready_minimizer_fixture_backend_from_name(name),
        .failure = minimizer_generation_failure_injection::none,
    };
}

/// Enable the failure injections and memory limits selected for this test.
inline void arm_ready_minimizer_fixture_failures(
    const ready_minimizer_fixture_backend_request& request) noexcept {
    arm_minimizer_generation_failure_injection(request.failure);
#ifdef BIOSEQ_TEST_INDEXES
    bioseq::storage::arm_scanner_pg_leaf_failure(request.scanner_failure);
    arm_sorted_run_pg_leaf_failure_on_nth(request.sorted_run_failure,
                                          request.sorted_run_failure_operation);
    set_sorted_run_merge_reader_budget_for_test(request.sorted_run_merge_reader_budget_bytes);
    set_sorted_run_reader_admission_injection_for_test(
        request.sorted_run_reader_admission,
        request.sorted_run_reader_request_ceiling_bytes);
#endif
}

/// Clear the test failures and restore the normal merge-reader budget.
inline void disarm_ready_minimizer_fixture_failures() noexcept {
    disarm_minimizer_generation_failure_injection();
#ifdef BIOSEQ_TEST_INDEXES
    bioseq::storage::disarm_scanner_pg_leaf_failure();
    disarm_sorted_run_pg_leaf_failure();
#endif
}

/**
 * Test the grouped-SoA build coordinator on a stored sequence using k=15 and
 * w=10. Publish the generation, then reopen its head and manifest to check
 * their identities and counts. A named backend must be available.
 * @param rel_oid Source Bioseq table.
 * @param index_name Registered fragment or sequence minimizer index.
 * @param sequence_rowno Source row number used as the index query key.
 * @param backend_name Backend name or supported test-failure sceario.
 * @return Publication result and independently  read metadata.
 * @note This test path leaves metadata_age_log unchanged.
 */
[[nodiscard]] ready_minimizer_generation_coordinator_fixture_result
build_ready_minimizer_generation_coordinator_fixture_impl(Oid rel_oid,
                                                          std::string_view index_name,
                                                          std::uint64_t sequence_rowno,
                                                          std::string_view backend_name) {
    using backend_type = bioseq::minimizers::minimizer_sketch_backend;
    constexpr std::uint16_t k_kmer_size = 15u;
    constexpr std::uint16_t k_window_size = 10u;

    const bool is_bioseq_table =
        OidIsValid(rel_oid) && bioseq::postgres::support::invoke_pg_as_cpp([rel_oid] {
            return relation_oid_uses_column_table_access_method(rel_oid);
        });
    if (!is_bioseq_table) {
        ready_minimizer_error_as_cpp(ERRCODE_WRONG_OBJECT_TYPE,
                                     "READY minimizer fixture requires a USING bioseq table");
    }
    if (!superuser()) {
        ready_minimizer_error_as_cpp(ERRCODE_INSUFFICIENT_PRIVILEGE,
                                     "READY minimizer fixture requires superuser");
    }
    if (index_name != k_minimizers_per_fragment_index_name &&
        index_name != k_minimizers_per_sequence_index_name) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "READY minimizer fixture requires minimizers_per_fragment or minimizers_per_sequence");
    }
    if (!bioseq::indexing::sql_bigint_can_represent(sequence_rowno)) {
        ready_minimizer_error_as_cpp(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
                                     "READY minimizer fixture row number exceeds BIGINT");
    }
    const ready_minimizer_fixture_backend_request backend_request =
        parse_ready_minimizer_fixture_backend_request(backend_name);
    const backend_type backend = backend_request.backend;
    const bioseq::minimizers::minimizer_kernels* const minimizer_kernels =
        bind_ready_minimizer_fixture_kernels(backend,
                                             "READY minimizer coordinator fixture");
    Snapshot active_snapshot = GetActiveSnapshot();
    if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
        ready_minimizer_error_as_cpp(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                                     "READY minimizer fixture requires an active MVCC snapshot");
    }

    const std::int16_t seq_attno = bioseq::postgres::support::invoke_pg_as_cpp(
        [rel_oid] { return single_seq_attno_for_inspection(rel_oid); });
    const relation_identity relation_identity = bioseq::postgres::support::invoke_pg_as_cpp(
        [rel_oid] { return index_relation_identity(rel_oid); });
    if (relation_identity.db_oid != MyDatabaseId || !OidIsValid(relation_identity.tablespace_oid) ||
        !OidIsValid(relation_identity.relfilenode)) {
        ready_minimizer_error_as_cpp(ERRCODE_UNDEFINED_TABLE,
                                     "READY minimizer fixture source relation is unavailable");
    }

    struct fixture_state {
        Snapshot source_snapshot = nullptr;
        Snapshot verification_snapshot = nullptr;
        Relation source_relation = nullptr;
        column_read_state* source_read = nullptr;
        seq_caches* fragment_caches = nullptr;
        MemoryContext work_context = nullptr;
        std::optional<fragment_index_storage> storage;
        std::optional<index_storage_fragment_generation_read_session> verification_session;
        std::optional<ready_minimizer_generation_coordinator_fixture_result> result;
        bool completed = false;
    };
    auto* state = new (std::nothrow) fixture_state{};
    if (state == nullptr) {
        ready_minimizer_error_as_cpp(ERRCODE_OUT_OF_MEMORY,
                                     "READY minimizer fixture could not allocate cleanup state");
    }
    const MemoryContext caller_context = CurrentMemoryContext;

    try {
        state->result.emplace(ready_minimizer_generation_coordinator_fixture_result{
            .selected_backend_name = bioseq::minimizers::minimizer_sketch_backend_name(backend),
            .column_attno = seq_attno,
            .sequence_rowno = sequence_rowno,
        });
        ready_minimizer_generation_coordinator_fixture_result& result = *state->result;
        state->source_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        if (state->source_snapshot == nullptr || state->source_snapshot->regd_count == 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_INTERNAL_ERROR,
                "READY minimizer fixture could not register its source snapshot");
        }
        state->work_context = bioseq::postgres::support::invoke_pg_as_cpp([caller_context] {
            return AllocSetContextCreate(caller_context,
                                         "bioseq READY minimizer coordinator fixture",
                                         ALLOCSET_DEFAULT_SIZES);
        });
        state->source_relation = bioseq::postgres::support::invoke_pg_as_cpp([rel_oid] {
            return bioseq::postgres::support::try_open_postgres_relation(rel_oid, AccessShareLock);
        });
        if (state->source_relation == nullptr ||
            RelationGetRelid(state->source_relation) != rel_oid ||
            state->source_relation->rd_locator.dbOid != relation_identity.db_oid ||
            state->source_relation->rd_locator.spcOid != relation_identity.tablespace_oid ||
            static_cast<Oid>(state->source_relation->rd_locator.relNumber) !=
                relation_identity.relfilenode) {
            ready_minimizer_error_as_cpp(
                ERRCODE_UNDEFINED_TABLE,
                "READY minimizer fixture source relation identity changed");
        }

        TupleDesc source_desc = RelationGetDescr(state->source_relation);
        MemoryContext previous_context = bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { return MemoryContextSwitchTo(state->work_context); });
        Datum* values = bioseq::postgres::support::invoke_pg_as_cpp([source_desc] {
            return static_cast<Datum*>(
                palloc0(sizeof(Datum) * static_cast<std::size_t>(source_desc->natts)));
        });
        bool* nulls = bioseq::postgres::support::invoke_pg_as_cpp([source_desc] {
            return static_cast<bool*>(
                palloc0(sizeof(bool) * static_cast<std::size_t>(source_desc->natts)));
        });
        List* projected_columns = bioseq::postgres::support::invoke_pg_as_cpp(
            [seq_attno] { return list_make1_int(seq_attno); });
        state->source_read =
            bioseq::postgres::support::invoke_pg_as_cpp([state, source_desc, projected_columns] {
                return begin_column_read(state->source_relation,
                                         source_desc,
                                         projected_columns,
                                         NIL,
                                         state->work_context,
                                         state->source_snapshot,
                                         /*randomAccess=*/true,
                                         nullptr);
            });
        const bool row_visible =
            bioseq::postgres::support::invoke_pg_as_cpp([state, sequence_rowno, values, nulls] {
                return read_column_row_by_number(state->source_read, sequence_rowno, values, nulls);
            });
        if (!row_visible || nulls[seq_attno - 1]) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [previous_context] { MemoryContextSwitchTo(previous_context); });
            ready_minimizer_error_as_cpp(ERRCODE_NO_DATA_FOUND,
                                         "READY minimizer fixture source row is not visible",
                                         fmt::format("sequence_rowno={}", sequence_rowno));
        }
        bioseq::pg_types::seq* source_header =
            bioseq::postgres::support::invoke_pg_as_cpp([values, seq_attno] {
                return reinterpret_cast<bioseq::pg_types::seq*>(
                    PG_DETOAST_DATUM(values[seq_attno - 1]));
            });
        if (!is_locator_stub(source_header)) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [previous_context] { MemoryContextSwitchTo(previous_context); });
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "READY minimizer fixture requires a fragmented PostgreSQL-backed source");
        }
        result.locator = decode_locator_from_stub(source_header);
        result.alphabet_code = static_cast<std::uint8_t>(source_header->alphabet_code);
        result.storage_unit_byte_width = logical_storage_unit_byte_width(source_header);
        if (result.locator.payload_version != sequence_locator_payload_version ||
            result.locator.source_epoch == 0 || result.locator.logical_length == 0 ||
            result.locator.fragment_count == 0) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [previous_context] { MemoryContextSwitchTo(previous_context); });
            ready_minimizer_error_as_cpp(ERRCODE_DATA_CORRUPTED,
                                         "READY minimizer fixture locator is invalid");
        }
        const auto algorithm_identity =
            bioseq::indexing::select_minimizer_generation_algorithm_identity_for_backend(
                backend,
                index_name == k_minimizers_per_fragment_index_name
                    ? bioseq::indexing::k_minimizer_fragment_algorithm_position_extent_bp
                    : result.locator.logical_length,
                result.alphabet_code,
                static_cast<std::uint8_t>(result.storage_unit_byte_width * 8u),
                k_kmer_size,
                k_window_size);
        if (!algorithm_identity) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "READY minimizer fixture algorithm identity is not READY");
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { end_column_read(state->source_read); });
        state->source_read = nullptr;
        bioseq::postgres::support::invoke_pg_as_cpp(
            [previous_context] { MemoryContextSwitchTo(previous_context); });
        Relation finished_source = std::exchange(state->source_relation, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_source] { relation_close(finished_source, AccessShareLock); });

        state->fragment_caches = bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { return alloc_seq_caches(state->work_context); });
        state->fragment_caches->side = seq_caches::access_side::read;
        state->fragment_caches->lock_mode = AccessShareLock;
        state->fragment_caches->read_state_mctx = state->work_context;
        state->fragment_caches->main_database_oid = relation_identity.db_oid;
        state->fragment_caches->main_tablespace_oid = relation_identity.tablespace_oid;
        state->fragment_caches->main_relfilenode = relation_identity.relfilenode;

        bioseq::pg_types::seq::dispatch_instance(
            result.alphabet_code,
            result.storage_unit_byte_width,
            [state,
             rel_oid,
             relation_identity,
             seq_attno,
             index_name,
             sequence_rowno,
             backend,
             backend_request,
             minimizer_kernels,
             algorithm_identity = *algorithm_identity,
             &result]<class p_sequence_config>() {
                using source_binding =
                    bioseq::storage::postgres_lazy_sequence_binding<p_sequence_config>;
                const source_binding bound_source{
                    relation_identity.db_oid,
                    relation_identity.tablespace_oid,
                    relation_identity.relfilenode,
                    seq_attno,
                    result.locator,
                    source_binding::expected_last_fragment_padding(result.locator),
                    state->source_snapshot,
                    state->fragment_caches,
                    state->work_context};
                auto opened_storage =
                    bioseq::postgres::support::invoke_pg_as_cpp([state,
                                                                 rel_oid,
                                                                 relation_identity,
                                                                 seq_attno,
                                                                 index_name,
                                                                 sequence_rowno,
                                                                 &result] {
                        return open_fragment_storage(rel_oid,
                                                     relation_identity.db_oid,
                                                     relation_identity.tablespace_oid,
                                                     relation_identity.relfilenode,
                                                     seq_attno,
                                                     index_name,
                                                     result.locator,
                                                     sequence_rowno,
                                                     state->fragment_caches,
                                                     state->work_context);
                    });
                state->storage.emplace(std::move(opened_storage));
                result.table_name = state->storage->this_table_name();
                bioseq::storage::with_postgres_lazy_sequence<p_sequence_config>(
                    bound_source,
                    [state,
                     rel_oid,
                     seq_attno,
                     index_name,
                     sequence_rowno,
                     backend,
                     backend_request,
                     minimizer_kernels,
                     algorithm_identity,
                     &result,
                     bound_source](auto lazy_view) {
                        arm_ready_minimizer_fixture_failures(backend_request);
                        result.publication.emplace(
                            build_and_publish_grouped_soa_minimizer_generation_pg(
                                rel_oid,
                                seq_attno,
                                index_name,
                                sequence_rowno,
                                *state->storage,
                                lazy_view,
                                bound_source,
                                algorithm_identity,
                                backend,
                                minimizer_kernels));
                        disarm_ready_minimizer_fixture_failures();
                    });
            });
        const bool execution_kind_matches =
            result.publication.has_value() &&
            (index_name == k_minimizers_per_fragment_index_name
                 ? std::holds_alternative<
                       bioseq::indexing::minimizer_fragment_generation_execution_receipt>(
                       result.publication->execution())
                 : std::holds_alternative<
                       bioseq::indexing::minimizer_sequence_generation_execution_receipt>(
                       result.publication->execution()));
        if (!result.publication.has_value() || !result.publication->published() ||
            !result.publication->execution_accepted() || !execution_kind_matches ||
            result.publication->selected_backend() != backend) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "READY minimizer fixture publication validation was rejected");
        }

        state->storage->close_checked();
        state->storage.reset();
        bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { close_and_destroy_seq_caches(state->fragment_caches, /*commit=*/false); });
        state->fragment_caches = nullptr;
        bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { UnregisterSnapshot(state->source_snapshot); });
        state->source_snapshot = nullptr;

        // Reopen the published head and manifest with a fresh registered snapshot
        // to check that PostgreSQL can read the newly written metadata.
        bioseq::postgres::support::invoke_pg_as_cpp([] { CommandCounterIncrement(); });
        Snapshot verification_source =
            bioseq::postgres::support::invoke_pg_as_cpp([] { return GetActiveSnapshot(); });
        state->verification_snapshot =
            register_minimizer_reread_snapshot_pg(verification_source);
        const auto expected =
            bioseq::indexing::expected_grouped_soa_generation_identity_for_backend(
                bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local,
                backend,
                *algorithm_identity,
                result.alphabet_code,
                result.storage_unit_byte_width,
                k_kmer_size,
                k_window_size,
                bioseq::indexing::k_grouped_soa_default_max_occurrence_cap);
        if (!expected) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "READY minimizer fixture could not derive its read contract");
        }
        state->verification_session.emplace(
            result.table_name,
            sequence_rowno,
            state->verification_snapshot,
            *expected,
            relation_identity.tablespace_oid,
            minimizer_generation_snapshot_registration::borrow_registered);
        result.head = state->verification_session->read_active_head_once();
        result.manifest =
            state->verification_session->read_manifest_directory_once(result.head).manifest;
        const std::uint64_t manifest_storage_contract =
            bioseq::indexing::compute_minimizer_generation_storage_contract_checksum(
                result.manifest);
        const auto scheme = bioseq::indexing::minimizer_scheme_identity_for_backend(backend);
        const auto producer_backend =
            bioseq::indexing::minimizer_producer_backend_identity_for_backend(backend);
        if (result.head.layout !=
                bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local ||
            result.head.generation_id != result.publication->generation_id() ||
            result.head.source_identity != result.publication->source_identity() ||
            result.head.source_content_checksum != result.publication->source_content_checksum() ||
            result.head.storage_contract_checksum !=
                result.publication->storage_contract_checksum() ||
            result.manifest.layout != result.head.layout ||
            result.manifest.generation_id != result.head.generation_id ||
            result.manifest.source_identity != result.head.source_identity ||
            result.manifest.source_content_checksum != result.head.source_content_checksum ||
            result.manifest.target_length != result.locator.logical_length ||
            result.manifest.control_count != result.publication->control_count() ||
            result.manifest.position_count != result.publication->position_count() ||
            result.manifest.kmer_size != k_kmer_size ||
            result.manifest.window_size != k_window_size ||
            result.manifest.minimizer_scheme_id != scheme.id ||
            result.manifest.minimizer_scheme_revision != scheme.revision ||
            result.manifest.producer_backend_id != producer_backend.id ||
            result.manifest.producer_identity_revision != producer_backend.revision ||
            result.manifest.algorithm_identity != *algorithm_identity ||
            manifest_storage_contract != result.publication->storage_contract_checksum()) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "READY minimizer fixture independently read an inconsistent head or manifest");
        }
        result.head_manifest_verified = true;
        state->verification_session->close_metadata_read();
        state->verification_session.reset();
        bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { UnregisterSnapshot(state->verification_snapshot); });
        state->verification_snapshot = nullptr;

        MemoryContext finished_context = std::exchange(state->work_context, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_context] { MemoryContextDelete(finished_context); });
        state->completed = true;
    } catch (...) {
        disarm_ready_minimizer_fixture_failures();
        MemoryContextSwitchTo(caller_context);
        if (state->verification_session.has_value()) {
            state->verification_session.reset();
        }
        if (state->storage.has_value()) {
            state->storage->abandon_after_pg_error_noexcept();
            state->storage.reset();
        }
        state->fragment_caches = nullptr;
        state->source_read = nullptr;
        state->source_relation = nullptr;
        state->verification_snapshot = nullptr;
        state->source_snapshot = nullptr;
        state->work_context = nullptr;
        delete state;
        state = nullptr;
        throw;
    }
    if (state == nullptr || !state->completed || !state->result.has_value()) {
        ready_minimizer_error_as_cpp(ERRCODE_INTERNAL_ERROR,
                                     "READY minimizer fixture did not complete or report an error");
    }
    ready_minimizer_generation_coordinator_fixture_result result = std::move(*state->result);
    state->result.reset();
    delete state;
    state = nullptr;
    return result;
}

/// Generate repeatable sorted minimizer events for storage tests. Label them
/// with the requested backend identity Vulkan-labelled data spans the target
/// to test multiple position segments, with up to 32768 events. Other backends
/// use up to 4096 events.
struct minimizer_generation_fixture_producer {
    std::uint64_t target_length = 0;
    bioseq::minimizers::minimizer_sketch_backend backend =
        bioseq::minimizers::minimizer_sketch_backend::scalar;

    /// Return the minimizer scheme assigned to this test backend.
    [[nodiscard]] bioseq::indexing::minimizer_scheme_identity scheme_identity() const noexcept {
        return bioseq::indexing::minimizer_scheme_identity_for_backend(backend);
    }

    /// Return  the producer identity assigned to this test backend.
    [[nodiscard]] bioseq::indexing::minimizer_producer_identity
    producer_backend_identity() const noexcept {
        return bioseq::indexing::minimizer_producer_backend_identity_for_backend(backend);
    }

    /// Replay the test events in increasing hash order.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    scan_sorted_events(std::uint64_t,
                       bioseq::indexing::canonical_minimizer_event_receiver receiver) {
        const std::uint64_t fixture_event_limit =
            backend == bioseq::minimizers::minimizer_sketch_backend::vulkan_segment ? 32768u
                                                                                    : 4096u;
        const std::uint64_t event_count =
            std::min<std::uint64_t>(target_length, fixture_event_limit);
        const bool spread_across_target =
            event_count > 1 &&
            backend == bioseq::minimizers::minimizer_sketch_backend::vulkan_segment;
        const std::uint64_t position_divisor = spread_across_target ? event_count - 1u : 1u;
        const std::uint64_t position_quotient =
            spread_across_target ? (target_length - 1u) / position_divisor : 0u;
        const std::uint64_t position_remainder =
            spread_across_target ? (target_length - 1u) % position_divisor : 0u;
        for (std::uint64_t event = 0; event < event_count; ++event) {
            // Spread positions through the taret while avoiding overflow from a
            // target_length-sized multiplication. The last event reaches the
            // final base, allowing long test sources to cross segment boundaries.
            const std::uint64_t position =
                spread_across_target
                    ? event * position_quotient + (event * position_remainder) / position_divisor
                    : event;
            auto emitted = receiver(bioseq::indexing::canonical_minimizer_event{
                .hash = event + 1u,
                .encoded_position = position << 1u,
            });
            if (!emitted) {
                return emitted;
            }
        }
        return {};
    }
};

static_assert(bioseq::indexing::grouped_soa_event_producer<minimizer_generation_fixture_producer>);

/// Publish deterministic grouped-SoA test data through the normal row writer.
/// A later statement can open a new read session to check  the stored result.
[[nodiscard]] minimizer_generation_integration_fixture_result
build_minimizer_generation_integration_fixture_impl(
    Oid rel_oid,
    std::string_view index_name,
    std::uint64_t sequence_rowno,
    std::uint16_t kmer_size,
    std::uint16_t window_size,
    bioseq::indexing::minimizer_generation_layout layout,
    bioseq::minimizers::minimizer_sketch_backend backend,
    std::uint64_t position_segment_span_bp) {
    if (!OidIsValid(rel_oid) || !bioseq::postgres::support::invoke_pg_as_cpp([rel_oid] {
            return relation_oid_uses_column_table_access_method(rel_oid);
        })) {
        ready_minimizer_error_as_cpp(
            ERRCODE_WRONG_OBJECT_TYPE,
            "bioseq minimizer generation fixture requires a USING bioseq table");
    }
    if (!bioseq::postgres::support::invoke_pg_as_cpp([] { return superuser(); })) {
        ready_minimizer_error_as_cpp(ERRCODE_INSUFFICIENT_PRIVILEGE,
                                     "bioseq minimizer generation fixture requires superuser");
    }
    if (index_name != k_minimizers_per_fragment_index_name &&
        index_name != k_minimizers_per_sequence_index_name) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq minimizer generation fixture requires a minimizer index");
    }
    if (kmer_size == 0 || window_size == 0 ||
        !bioseq::indexing::sql_bigint_can_represent(sequence_rowno)) {
        ready_minimizer_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE,
                                     "bioseq minimizer generation fixture parameters are invalid");
    }
    if (layout != bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local) {
        ready_minimizer_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE,
                                     "bioseq generation fixture requires the grouped-SoA layout");
    }
    const auto requested_scheme = bioseq::indexing::minimizer_scheme_identity_for_backend(backend);

    const std::int16_t seq_attno = single_seq_attno_for_inspection(rel_oid);
    const relation_identity rid = index_relation_identity(rel_oid);
    minimizer_generation_administration_identity identity{
        .database_oid = rid.db_oid,
        .tablespace_oid = rid.tablespace_oid,
        .main_relfilenode = rid.relfilenode,
        .user_relid = rel_oid,
        .column_attno = seq_attno,
        .index_name = std::string{index_name},
    };

    struct fixture_state {
        Snapshot registered_snapshot = nullptr;
        std::optional<minimizer_generation_source_authorization> setup_authorization;
        std::optional<index_storage_fragment_generation_row_sink> writer;
        std::optional<bioseq::indexing::encoded_grouped_soa_generation> generation;
        std::optional<ready_minimizer_generation_token> ready_token;
    };
    fixture_state* state = nullptr;
    minimizer_generation_integration_fixture_result result{};
    try {
        state = new fixture_state{};
        Snapshot active_snapshot =
            bioseq::postgres::support::invoke_pg_as_cpp([] { return GetActiveSnapshot(); });
        if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq minimizer generation fixture requires an active MVCC snapshot");
        }
        state->registered_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        state->setup_authorization.emplace(authorize_minimizer_generation_source_pg(
            identity, sequence_rowno, state->registered_snapshot));
        minimizer_generation_source_locator source =
            state->setup_authorization->source_for_diagnostics();
        const std::uint64_t source_content_checksum =
            state->setup_authorization->source_content_checksum_for_diagnostics();
        result.table_name = format_seqidx_name(index_name,
                                               rid.db_oid,
                                               rid.tablespace_oid,
                                               rid.relfilenode,
                                               seq_attno,
                                               source.locator.bucket_index,
                                               source.locator.shard_index);
        identity.table_name = result.table_name;
        result.layout = layout;
        state->setup_authorization.reset();
        bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { UnregisterSnapshot(state->registered_snapshot); });
        state->registered_snapshot = nullptr;

        const char* quoted_table = bioseq::postgres::support::invoke_pg_as_cpp(
            [&result] { return quote_identifier(result.table_name.c_str()); });
        char* raw_tablespace_name = bioseq::postgres::support::invoke_pg_as_cpp(
            [&rid] { return get_tablespace_name(rid.tablespace_oid); });
        if (raw_tablespace_name == nullptr) {
            ready_minimizer_error_as_cpp(
                ERRCODE_UNDEFINED_OBJECT,
                "bioseq minimizer generation fixture source tablespace is missing");
        }
        const std::string tablespace_name{raw_tablespace_name};
        bioseq::postgres::support::invoke_pg_as_cpp(
            [raw_tablespace_name] { pfree(raw_tablespace_name); });
        const char* quoted_tablespace = bioseq::postgres::support::invoke_pg_as_cpp(
            [&tablespace_name] { return quote_identifier(tablespace_name.c_str()); });
        const std::string drop_ddl =
            fmt::format("DROP TABLE IF EXISTS bioseq_internal.{}", quoted_table);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&drop_ddl] { run_internal_ddl(drop_ddl.c_str()); });
        const std::string create_ddl =
            fmt::format("CREATE UNLOGGED TABLE bioseq_internal.{} ("
                        "query_key BIGINT NOT NULL, generation_id BIGINT NOT NULL, "
                        "position_segment INTEGER NOT NULL, sub_key BIGINT NOT NULL, "
                        "chunk_no INTEGER NOT NULL, chunk_bytes INTEGER NOT NULL, "
                        "checksum BIGINT NOT NULL, blob BYTEA NOT NULL, "
                        "PRIMARY KEY (generation_id,query_key,sub_key,position_segment,chunk_no) "
                        "WITH (fillfactor=90) USING INDEX TABLESPACE {}) "
                        "USING heap WITH (fillfactor=100) TABLESPACE {}",
                        quoted_table,
                        quoted_tablespace,
                        quoted_tablespace);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&create_ddl] { run_internal_ddl(create_ddl.c_str()); });
        const std::string alter_ddl = fmt::format(
            "ALTER TABLE bioseq_internal.{} ALTER COLUMN blob SET STORAGE PLAIN", quoted_table);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&alter_ddl] { run_internal_ddl(alter_ddl.c_str()); });
        bioseq::postgres::support::invoke_pg_as_cpp([] { CommandCounterIncrement(); });

        result.generation_id = bioseq::postgres::support::invoke_pg_as_cpp(
            [] { return allocate_minimizer_generation_id_pg(); });
        const std::uint64_t target_length = source.locator.logical_length;
        if (target_length == 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_INVALID_PARAMETER_VALUE,
                "bioseq minimizer generation fixture requires a nonempty target");
        }
        result.source_identity = minimizer_generation_source_identity(rid.relfilenode, source);
        result.source_content_checksum = source_content_checksum;
        result.source_num_fragments = source.locator.fragment_count;
        result.source_total_bp = source.locator.logical_length;
        result.source_padding_letters = source.padding_letters;
        if (result.source_identity == 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation fixture derived a zero source identity");
        }

        const auto algorithm_identity =
            bioseq::indexing::select_minimizer_generation_algorithm_identity_for_backend(
                backend,
                index_name == k_minimizers_per_fragment_index_name
                    ? bioseq::indexing::k_minimizer_fragment_algorithm_position_extent_bp
                    : target_length,
                source.alphabet_code,
                static_cast<std::uint8_t>(source.storage_unit_byte_width * 8u),
                kmer_size,
                window_size);
        if (!algorithm_identity) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq minimizer generation fixture algorithm identity is not READY");
        }

        state->writer.emplace(identity,
                              sequence_rowno,
                              result.generation_id,
                              /*write_batch_row_limit=*/2);
        minimizer_generation_fixture_producer producer{target_length, backend};
        const bioseq::indexing::grouped_soa_generation_options options{
            .layout = layout,
            .generation_id = result.generation_id,
            .source_identity = result.source_identity,
            .source_content_checksum = source_content_checksum,
            .target_length = target_length,
            .position_segment_span_bp = position_segment_span_bp,
            .alphabet_code = source.alphabet_code,
            .storage_unit_byte_width = source.storage_unit_byte_width,
            .kmer_size = kmer_size,
            .window_size = window_size,
            .minimizer_scheme_id = requested_scheme.id,
            .minimizer_scheme_revision = requested_scheme.revision,
            .algorithm_identity = *algorithm_identity,
        };
        std::optional<minimizer_generation_encoding_error> encode_error;
        {
            auto encoded =
                bioseq::indexing::encode_minimizer_generation(producer, *state->writer, options);
            if (encoded) {
                state->generation.emplace(std::move(*encoded));
            } else {
                encode_error = encoded.error();
            }
        }
        if (encode_error) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation fixture encoder failed",
                fmt::format("wire_error={}", static_cast<unsigned>(*encode_error)));
        }
        result.storage_contract_checksum =
            bioseq::indexing::compute_minimizer_generation_storage_contract_checksum(
                state->generation->manifest);
        state->ready_token.emplace(state->writer->validate_ready_generation(*state->generation));
        result.writer_rows = state->writer->row_count();
        result.writer_batches = state->writer->catalog_multi_insert_batches();
        [[maybe_unused]] ready_minimizer_generation_published_head_receipt receipt =
            publish_ready_minimizer_generation_head_pg(std::move(*state->ready_token));
        state->ready_token.reset();
        result.published_head_bytes = minimizer_generation_head::encoded_bytes;

        result.control_count = state->generation->manifest.control_count;
        result.position_count = state->generation->manifest.position_count;
        result.directory_entries = state->generation->directory.entries().size();
        result.position_segment_count = state->generation->manifest.position_segment_count;
        const std::uint64_t expected_event_count = std::min<std::uint64_t>(
            target_length,
            backend == bioseq::minimizers::minimizer_sketch_backend::vulkan_segment ? 32768u
                                                                                    : 4096u);
        if (result.control_count != expected_event_count ||
            result.position_count != expected_event_count) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation fixture event totals disagree");
        }
        for (const grouped_soa_bucket_directory_entry& entry :
             state->generation->directory.entries()) {
            result.control_encoded_bytes = bioseq::common::saturating_add(
                result.control_encoded_bytes, entry.control_encoded_bytes);
            result.position_encoded_bytes = bioseq::common::saturating_add(
                result.position_encoded_bytes, entry.position_encoded_bytes);
            result.control_chunks =
                bioseq::common::saturating_add(result.control_chunks, entry.control_chunk_count);
            result.position_chunks =
                bioseq::common::saturating_add(result.position_chunks, entry.position_chunk_count);
        }
        if (result.control_encoded_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            result.position_encoded_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            result.control_chunks == (std::numeric_limits<std::uint64_t>::max)() ||
            result.position_chunks == (std::numeric_limits<std::uint64_t>::max)()) {
            ready_minimizer_error_as_cpp(
                ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
                "bioseq minimizer generation fixture payload totals overflow");
        }
        state->generation.reset();
        state->writer.reset();
        bioseq::postgres::support::invoke_pg_as_cpp([] { CommandCounterIncrement(); });
        delete state;
        state = nullptr;
    } catch (...) {
        std::exception_ptr error = std::current_exception();
        if (state != nullptr) {
            state->ready_token.reset();
            state->generation.reset();
            state->writer.reset();
            state->setup_authorization.reset();
        }
        if (state != nullptr && state->registered_snapshot != nullptr) {
            Snapshot cleanup = std::exchange(state->registered_snapshot, nullptr);
            try {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [cleanup] { UnregisterSnapshot(cleanup); });
            } catch (...) {
                error = std::current_exception();
            }
        }
        delete state;
        std::rethrow_exception(error);
    }
    return result;
}

} // namespace bioseq::storage::indexing
